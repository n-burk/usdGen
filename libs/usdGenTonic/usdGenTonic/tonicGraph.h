// usdGenTonic — the scalp graph (plan/17 §4.1 G1, P2).
//
// One shared planar graph on the growth surface: nodes are SHARED between
// neighbouring regions, edges always bound two regions (or one at the graph
// border), and welding keeps adjacent regions sharing their boundary instead
// of merely touching (plan/17 §5.1). Graph scale is hundreds of nodes, so
// this stays CPU, Qt-free and USD-free, and unit-tested at T0.
//
// Node ids are stable and never reused: welding marks the dropped node dead
// and re-links every incident edge to the kept id. Region ids are dense
// 0..R-1 after every extraction; the interpolation id (what the bake writes
// to channel 0 and the live primvar carries) folds linkedRegions through a
// union-find, so linked regions share one id while their tubes stay distinct.
#ifndef USDGEN_TONIC_GRAPH_H
#define USDGEN_TONIC_GRAPH_H

#include "usdGenTonic/api.h"
#include "usdGenTonic/tonicScalp.h"

#include <string>
#include <vector>

namespace usdGenTonic {

// K2: on-surface polyline between two located points (plan/17 §4.1 K2).
//
// Bounded-step discrete geodesic: N = clamp(ceil(dist / h), 2, maxSamples)
// chord samples projected to the surface (closest point), where h is the
// mesh mean edge length. On a planar patch this is the chord; on curved
// scalps it is our documented reconstruction of the unpublished Tonic trace
// (plan/17 Gap G4 owns the same caveat for K9). Returns xyz triples.
USDGENTONIC_API std::vector<float> TonicTraceEdgeCpu(
    TonicScalpMesh const &mesh, float const a[3], float const b[3],
    int maxSamples = 64);

// Deterministic region colour (golden-ratio hue ramp, sRGB-ish). Tubes stay
// distinct under linking: colour keys on the region id, never the interp id.
USDGENTONIC_API void TonicRegionColor(int regionId, float rgb[3]);

struct USDGENTONIC_API TonicGraphNode {
    int id = -1;
    bool alive = true;
    int faceId = -1;  // (faceId, u, v): survives scalp deformation
    float u = 0.0f;
    float v = 0.0f;
    float p[3] = {0.0f, 0.0f, 0.0f};  // rest position (refreshed on deform)
    float n[3] = {0.0f, 1.0f, 0.0f};
};

struct USDGENTONIC_API TonicGraphEdge {
    int id = -1;
    bool alive = true;
    int a = -1;  // node ids
    int b = -1;
    std::vector<float> polyline;  // K2 xyz triples, a -> b, >= 2 samples
};

struct USDGENTONIC_API TonicGraphRegion {
    int id = -1;  // dense 0..R-1 after extraction
    std::vector<int> loop;  // node ids in order (>= 3)
    std::vector<float> boundary;  // loop polyline xyz (edge polylines joined)
    float color[3] = {1.0f, 1.0f, 1.0f};
    int interpId = -1;  // union-find over linkedRegions (bake channel 0)
    int seedFace = -1;  // rasteriser flood seed (nearest face to centroid)
    bool isOutside = false;  // true only for the excluded outer face
};

class USDGENTONIC_API TonicScalpGraph {
public:
    TonicScalpGraph() = default;

    // -- queries ---------------------------------------------------------
    int NodeCount() const;  // alive nodes
    int EdgeCount() const;  // alive edges
    int RegionCount() const { return int(_regions.size()); }
    TonicGraphNode const *FindNode(int id) const;
    TonicGraphEdge const *FindEdge(int id) const;
    std::vector<TonicGraphNode> const &Nodes() const { return _nodes; }
    std::vector<TonicGraphEdge> const &Edges() const { return _edges; }
    std::vector<TonicGraphRegion> const &Regions() const { return _regions; }
    std::vector<std::pair<int, int>> const &LinkedPairs() const
    {
        return _linked;
    }
    int InterpId(int regionId) const;  // union-find representative (min id)
    int Valence(int nodeId) const;
    // Regions incident to a node (after extraction).
    std::vector<int> NodeRegions(int nodeId) const;
    // Regions on either side of an edge (1 at the border, 2 shared).
    std::vector<int> EdgeRegions(int edgeId) const;
    // True when every alive edge bounds >= 1 region and every shared edge is
    // claimed by exactly the regions on both sides (no T-junction gaps).
    bool IsWatertight() const;
    // Coincident alive node sets (unwelded duplicates): groups of ids within
    // `eps` of each other. The HUD draws these as warning rings.
    std::vector<std::vector<int>> CoincidentSets(float eps) const;
    char const *GetDiagnostic() const { return _diagnostic.c_str(); }

    // -- editing (each re-extracts regions unless noted) ------------------
    // Add a node at a located surface point. Returns the node id, or -1.
    int AddNode(TonicHit const &hit);
    // Move a node to a located point (drag). Edge polylines re-trace.
    bool MoveNode(TonicScalpMesh const &mesh, int nodeId, TonicHit const &hit);
    // Connect two nodes with a K2 edge. Connecting across a region splits it.
    // Returns the edge id, or -1 (already connected, dead nodes, K2 failure).
    int Connect(TonicScalpMesh const &mesh, int a, int b);
    // Split an edge at a located point: one edge becomes two. Returns the new
    // node id, or -1.
    int SplitEdge(TonicScalpMesh const &mesh, int edgeId, TonicHit const &hit);
    // Weld: merge `drop` into `keep` (keeps `keep`'s position), re-link every
    // incident edge, remove degenerate and duplicate edges. Undoable at the
    // model layer (P6). Returns false for dead/same ids.
    bool Weld(TonicScalpMesh const &mesh, int keep, int drop);
    // Weld every pair within `radius` (rest units). Returns the weld count.
    int WeldAllWithinRadius(TonicScalpMesh const &mesh, float radius);
    // Unweld: a node shared by n regions becomes n coincident nodes, one per
    // region, so a boundary can slide independently. Returns the new node ids
    // (empty when the node bounds < 2 regions).
    std::vector<int> Unweld(TonicScalpMesh const &mesh, int nodeId);
    // Deleting an edge merges its two regions.
    bool DeleteEdge(TonicScalpMesh const &mesh, int edgeId);
    // Deleting a node removes its edges; a valence-2 node re-links its two
    // neighbours with a fresh K2 edge.
    bool DeleteNode(TonicScalpMesh const &mesh, int nodeId);
    // Snap search: nearest alive node within `radius` (rest units) of p, or
    // -1. Graph scale is hundreds of nodes: brute force, no index.
    int SnapNode(float const p[3], float radius) const;
    // Nearest alive edge within `radius` of p (distance to its polyline), or
    // -1. Used by stroke-end welding ("ends on an edge: split and weld").
    int SnapEdge(float const p[3], float radius) const;
    // Link two regions for interpolation (symmetric pair list; the bake
    // writes one id for the whole link set). Unlink removes one pair.
    bool LinkRegions(int r0, int r1);
    bool UnlinkRegions(int r0, int r1);
    void ClearLinks();
    // Mirror-X: append mirrored nodes + edges across x = 0 (plan/17 §5.1).
    // Returns the (oldId -> newId) node mapping. Off by default (model flag).
    std::vector<std::pair<int, int>> MirrorX(TonicScalpMesh const &mesh);
    // Re-run the extraction after a batch of direct edits (tests, hydrate).
    // Returns false with a diagnostic when the mesh is unusable.
    bool ExtractRegions(TonicScalpMesh const &mesh);
    void Clear();

    // Direct authoring for hydrate / the committer round-trip: nodes and
    // edges are taken verbatim (ids preserved), then regions extract.
    bool Restore(TonicScalpMesh const &mesh,
                 std::vector<TonicGraphNode> const &nodes,
                 std::vector<std::pair<int, int>> const &edges,
                 std::vector<std::pair<int, int>> const &linked,
                 std::string *err);

private:
    int _NextNodeId() { return _nextNodeId++; }
    int _NextEdgeId() { return _nextEdgeId++; }
    TonicGraphNode *_MutableNode(int id);
    TonicGraphEdge *_MutableEdge(int id);
    void _DropDegenerateEdges();
    void _RetraceIncident(TonicScalpMesh const &mesh, int nodeId);

    std::vector<TonicGraphNode> _nodes;
    std::vector<TonicGraphEdge> _edges;
    std::vector<TonicGraphRegion> _regions;
    std::vector<std::pair<int, int>> _linked;
    int _nextNodeId = 0;
    int _nextEdgeId = 0;
    std::string _diagnostic;
};

// Graph-mode stroke (plan/17 §5.1 Draw): surface samples (one K1 hit per
// mouse sample) become a node chain. Douglas–Peucker simplification runs in
// the surface chart (3D positions, eps = simplifyEps); a stroke point within
// `snapRadius` of a node welds to it, within range of an edge splits that
// edge and welds to the new node; a stroke closing on itself becomes a
// region immediately.
struct USDGENTONIC_API TonicStrokeResult {
    std::vector<int> nodeIds;  // the chain, in stroke order
    std::vector<int> edgeIds;
    bool closedLoop = false;  // the stroke became a region boundary
    bool weldedStart = false;
    bool weldedEnd = false;
};
USDGENTONIC_API TonicStrokeResult TonicStrokeToChain(
    TonicScalpMesh const &mesh, TonicScalpGraph *graph,
    std::vector<TonicHit> const &samples, float snapRadius, float simplifyEps);

}  // namespace usdGenTonic

#endif  // USDGEN_TONIC_GRAPH_H
