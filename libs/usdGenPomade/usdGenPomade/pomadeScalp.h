// usdGenPomade — scalp binding + LBVH + raycast (plan/17 §4.1 K1, P2).
//
// K1 owns the growth surface: rest points, face adjacency, an LBVH over faces
// (Morton codes over centroids + a Karras binary radix tree), a raycast used
// by every mouse pick on the scalp, and a closest-point query used by stroke
// projection, snapping and mirror-X.
//
// This TU is USD-free and Qt-free. The CUDA raycast (pomadeKernels.cu,
// PomadeLaunchRaycastBatch) traverses an uploaded copy of PomadeScalpBvh with
// the same traversal order and triangle tests; PomadeRaycastCpu is its twin
// (plan/17 TN-6).
//
// Face-local (u, v) convention (stable node handles, round-trippable):
//   * quads: bilinear with p(0,0) = p0, p(1,0) = p1, p(1,1) = p2, p(0,1) = p3
//     — the same convention UsdGenPtexFaceCoordinate uses, so a node (u, v)
//     addresses the same texel the ptex() expression reads;
//   * other n-gons: the face is split into n sub-quads exactly as the Ptex
//     sampler documents (corner k, edge-midpoint k/k+1, centroid, edge-midpoint
//     k-1/k); u = (k + fu) / n carries the winning sub-face k, v = fv.
// PomadeFacePosition inverts the encoding exactly.
//
// Face subsets (plan/02 §2.20): a UsdGeomSubset scalp binds its parent
// Mesh whole and names the growth surface in PomadeScalpMesh::activeFaces.
// Face ids, ptex ids and every per-face array stay parent-mesh indexed (a
// subset never renumbers faces); only the surface queries -- the LBVH and
// so every raycast, closest point, region raster -- skip inactive faces.
// The device edge trace (PomadeLaunchEdgeTrace) is a test-only twin that
// still walks every face.
#ifndef USDGEN_POMADE_SCALP_H
#define USDGEN_POMADE_SCALP_H

#include "usdGenPomade/api.h"

#include <cstddef>
#include <string>
#include <vector>

namespace usdGenPomade {

// Rest-space scalp mesh. Topology is fixed for the life of a binding
// (plan/17 §3.4: a topology change invalidates the binding and freezes
// editing until the artist re-binds); deformation only refits the BVH.
struct USDGENPOMADE_API PomadeScalpMesh {
    std::vector<float> points;  // 3N, rest space
    std::vector<int> faceVertexCounts;
    std::vector<int> faceVertexIndices;
    std::vector<int> faceOffsets;  // F+1, filled by PomadeScalpFinalize
    // The face subset: parent-mesh face ids the growth surface is limited
    // to; empty binds every face. PomadeScalpFinalize sorts and de-dupes it
    // and rejects an out-of-range id.
    std::vector<int> activeFaces;

    // Derived per-face data, filled by PomadeScalpFinalize:
    std::vector<char> faceActive;      // F, 1 = on the growth surface
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
// false with *err set on bad topology (short faces, out-of-range indices)
// or a face subset naming a face the mesh does not have.
USDGENPOMADE_API bool PomadeScalpFinalize(PomadeScalpMesh *mesh, std::string *err);

// Whether coarse face `face` is on the growth surface: in range, and in the
// face subset when one is bound.
inline bool PomadeScalpFaceActive(PomadeScalpMesh const &mesh, int face)
{
    return face >= 0 && size_t(face) < mesh.faceVertexCounts.size() &&
           (mesh.faceActive.empty() || mesh.faceActive[size_t(face)] != 0);
}

// One LBVH node. Leaves hold a Morton-order run of faces (one face per leaf
// in this implementation); interior nodes hold child indices. The layout is
// plain C so the identical bytes upload to the device for K1.
struct USDGENPOMADE_API PomadeScalpBvhNode {
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    int left = -1;   // child node index, or -1 for a leaf
    int right = -1;  // child node index, or -1 for a leaf
    int faceBegin = 0;  // leaf run in `order`
    int faceEnd = 0;
};

struct USDGENPOMADE_API PomadeScalpBvh {
    std::vector<PomadeScalpBvhNode> nodes;
    std::vector<int> order;  // face ids in Morton order
    int root = -1;
    bool valid = false;
};

// Builds the LBVH over the active faces (Morton sort + radix tree +
// bottom-up AABBs), so a raycast never lands outside a face subset. Empty
// meshes build an empty-but-valid BVH (root -1).
USDGENPOMADE_API bool PomadeScalpBvhBuild(PomadeScalpMesh const &mesh,
                                       PomadeScalpBvh *bvh, std::string *err);
// Recomputes every node AABB from the current rest points after a deform.
// The Morton order and tree shape are kept (refit-only, plan/17 K1).
USDGENPOMADE_API bool PomadeScalpBvhRefit(PomadeScalpMesh const &mesh,
                                       PomadeScalpBvh *bvh);

// A located surface point: raycast hits, closest points, graph nodes.
struct USDGENPOMADE_API PomadeHit {
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
USDGENPOMADE_API PomadeHit PomadeRaycastCpu(PomadeScalpMesh const &mesh,
                                        PomadeScalpBvh const &bvh,
                                        float const origin[3],
                                        float const dir[3]);

// Brute-force closest point over the active faces (stroke projection, snap
// search, mirror-X). Graph-scale call sites project tens of samples; the
// per-face cost is one point-to-fan projection. `activeOnly = false` walks
// every parent face, which is how a root sampler tells a candidate that
// lies over an inactive face from one on the growth surface.
USDGENPOMADE_API PomadeHit PomadeClosestPointCpu(PomadeScalpMesh const &mesh,
                                             float const p[3],
                                             bool activeOnly = true);

// Mirrors p across the x = 0 plane and returns the closest surface point to
// the mirrored position (plan/17 §5.1 mirror-X).
USDGENPOMADE_API PomadeHit PomadeMirrorXCpu(PomadeScalpMesh const &mesh,
                                        float const p[3]);

// Face-local coordinate of p on coarse face `face` (the § header encoding).
// Returns false for an out-of-range face or degenerate geometry.
USDGENPOMADE_API bool PomadeFaceCoordinate(PomadeScalpMesh const &mesh, int face,
                                        float px, float py, float pz,
                                        float *u, float *v, int *ptexFaceId);
// Position of face-local (u, v) on coarse face `face` (exact inverse of
// PomadeFaceCoordinate up to float rounding). Returns false for bad input.
USDGENPOMADE_API bool PomadeFacePosition(PomadeScalpMesh const &mesh, int face,
                                      float u, float v,
                                      float *px, float *py, float *pz);

}  // namespace usdGenPomade

#endif  // USDGEN_POMADE_SCALP_H
