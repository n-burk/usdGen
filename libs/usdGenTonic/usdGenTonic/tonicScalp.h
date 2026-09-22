// usdGenTonic — scalp binding + LBVH + raycast (plan/17 §4.1 K1, P2).
//
// K1 owns the growth surface: rest points, face adjacency, an LBVH over faces
// (Morton codes over centroids + a Karras binary radix tree), a raycast used
// by every mouse pick on the scalp, and a closest-point query used by stroke
// projection, snapping and mirror-X.
//
// This TU is USD-free and Qt-free. The CUDA raycast (tonicKernels.cu,
// TonicLaunchRaycastBatch) traverses an uploaded copy of TonicScalpBvh with
// the same traversal order and triangle tests; TonicRaycastCpu is its twin
// (plan/17 TN-6).
//
// Face-local (u, v) convention (stable node handles, round-trippable):
//   * quads: bilinear with p(0,0) = p0, p(1,0) = p1, p(1,1) = p2, p(0,1) = p3
//     — the same convention UsdGenPtexFaceCoordinate uses, so a node (u, v)
//     addresses the same texel the ptex() expression reads;
//   * other n-gons: the face is split into n sub-quads exactly as the Ptex
//     sampler documents (corner k, edge-midpoint k/k+1, centroid, edge-midpoint
//     k-1/k); u = (k + fu) / n carries the winning sub-face k, v = fv.
// TonicFacePosition inverts the encoding exactly.
#ifndef USDGEN_TONIC_SCALP_H
#define USDGEN_TONIC_SCALP_H

#include "usdGenTonic/api.h"

#include <cstddef>
#include <string>
#include <vector>

namespace usdGenTonic {

// Rest-space scalp mesh. Topology is fixed for the life of a binding
// (plan/17 §3.4: a topology change invalidates the binding and freezes
// editing until the artist re-binds); deformation only refits the BVH.
struct USDGENTONIC_API TonicScalpMesh {
    std::vector<float> points;  // 3N, rest space
    std::vector<int> faceVertexCounts;
    std::vector<int> faceVertexIndices;
    std::vector<int> faceOffsets;  // F+1, filled by TonicScalpFinalize

    // Derived per-face data, filled by TonicScalpFinalize:
    std::vector<float> faceCentroids;  // 3F
    std::vector<float> faceNormals;    // 3F, Newell, unit length
    std::vector<float> faceAreas;      // F
    std::vector<float> faceMin;        // 3F, AABBs
    std::vector<float> faceMax;        // 3F
    std::vector<std::vector<int>> faceNeighbours;  // dual graph, via verts
    float meanEdgeLength = 0.0f;
    float medianFaceArea = 0.0f;
    // Immutable rest-space extent, cached at finalization for display-only
    // overlay offsets without rescanning every vertex per graph CV.
    float boundsDiagonal = 0.0f;
    bool finalized = false;
};

// Validates topology, fills faceOffsets and every derived field. Returns
// false with *err set on bad topology (short faces, out-of-range indices).
USDGENTONIC_API bool TonicScalpFinalize(TonicScalpMesh *mesh, std::string *err);

// One LBVH node. Leaves hold a Morton-order run of faces (one face per leaf
// in this implementation); interior nodes hold child indices. The layout is
// plain C so the identical bytes upload to the device for K1.
struct USDGENTONIC_API TonicScalpBvhNode {
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    int left = -1;   // child node index, or -1 for a leaf
    int right = -1;  // child node index, or -1 for a leaf
    int faceBegin = 0;  // leaf run in `order`
    int faceEnd = 0;
};

struct USDGENTONIC_API TonicScalpBvh {
    std::vector<TonicScalpBvhNode> nodes;
    std::vector<int> order;  // face ids in Morton order
    int root = -1;
    bool valid = false;
};

// Builds the LBVH over the mesh faces (Morton sort + radix tree + bottom-up
// AABBs). Empty meshes build an empty-but-valid BVH (root -1).
USDGENTONIC_API bool TonicScalpBvhBuild(TonicScalpMesh const &mesh,
                                       TonicScalpBvh *bvh, std::string *err);
// Recomputes every node AABB from the current rest points after a deform.
// The Morton order and tree shape are kept (refit-only, plan/17 K1).
USDGENTONIC_API bool TonicScalpBvhRefit(TonicScalpMesh const &mesh,
                                       TonicScalpBvh *bvh);

// A located surface point: raycast hits, closest points, graph nodes.
struct USDGENTONIC_API TonicHit {
    bool hit = false;
    int faceId = -1;      // coarse mesh face
    int ptexFaceId = -1;  // Ptex sub-face id (quads: == faceId)
    float t = 0.0f;       // ray distance (raycast) or point distance (closest)
    float u = 0.0f;       // face-local coordinates (§ header convention)
    float v = 0.0f;
    float px = 0.0f, py = 0.0f, pz = 0.0f;  // hit position
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;  // geometric normal
};

// CPU twin of the K1 device raycast: identical traversal order and triangle
// tests (fan triangulation v0/vi/vi+1, Möller–Trumbore, nearest t wins).
// Degenerate input (zero direction) returns a miss, never NaN.
USDGENTONIC_API TonicHit TonicRaycastCpu(TonicScalpMesh const &mesh,
                                        TonicScalpBvh const &bvh,
                                        float const origin[3],
                                        float const dir[3]);

// Brute-force closest point over all faces (stroke projection, snap search,
// mirror-X). Graph-scale call sites project tens of samples; the per-face
// cost is one point-to-fan projection.
USDGENTONIC_API TonicHit TonicClosestPointCpu(TonicScalpMesh const &mesh,
                                             float const p[3]);

// Mirrors p across the x = 0 plane and returns the closest surface point to
// the mirrored position (plan/17 §5.1 mirror-X).
USDGENTONIC_API TonicHit TonicMirrorXCpu(TonicScalpMesh const &mesh,
                                        float const p[3]);

// Face-local coordinate of p on coarse face `face` (the § header encoding).
// Returns false for an out-of-range face or degenerate geometry.
USDGENTONIC_API bool TonicFaceCoordinate(TonicScalpMesh const &mesh, int face,
                                        float px, float py, float pz,
                                        float *u, float *v, int *ptexFaceId);
// Position of face-local (u, v) on coarse face `face` (exact inverse of
// TonicFaceCoordinate up to float rounding). Returns false for bad input.
USDGENTONIC_API bool TonicFacePosition(TonicScalpMesh const &mesh, int face,
                                      float u, float v,
                                      float *px, float *py, float *pz);

}  // namespace usdGenTonic

#endif  // USDGEN_TONIC_SCALP_H
