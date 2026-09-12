// usdGen engine — the compiled graph (03-execution-engine.md §3, §9.1).
//
// A UsdGenGraph is the compiled, topologically-sorted node list with per-node
// buffers, captures and chunk-dirty bytes. The compiler (§3) fills it; the
// scheduler (§5) mutates dirty state and evaluates; the session (§9.1) owns it.
#ifndef USDGEN_GRAPH_H
#define USDGEN_GRAPH_H

#include "usdGen/curveBuffer.h"
#include "usdGen/op.h"
#include "usdGen/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
class UsdGenCudaExecutionPlan;

/// Owning, read-only routing input detached from a compiled graph.  The
/// imaging router may retain this value while the graph is recompiled or
/// destroyed; none of its fields point into UsdGenGraph or an operator.
struct UsdGenGraphRoutingNode
{
    UsdGenNodeId id = kUsdGenInvalidNode;
    SdfPath path;
    TfToken type;
    int algorithmVersion = 0;
    std::vector<TfToken> topologyParameters;
    std::vector<TfToken> valueParameters;
    std::vector<std::pair<TfToken, uint32_t>> paramRouting;
    UsdGenSurfaceId surface = 0;
    bool hasSurface = false;
    std::vector<SdfPath> curveRefs;
    std::vector<SdfPath> mapRefs;
};

struct UsdGenGraphRoutingSnapshot
{
    SdfPath description;
    std::vector<SdfPath> surfacePaths;
    std::vector<UsdGenGraphRoutingNode> nodes;
    UsdGenNodeId terminal = kUsdGenInvalidNode;
};

/// One compiled node. Owns its UsdGenOp, its output buffer, its capture, its
/// chunk dirty bytes and its digests (03 §3.2).
struct UsdGenCompiledNode
{
    UsdGenNodeDesc const *desc = nullptr;         // points into the graph's desc copy
    int                  descIdx = -1;            // index of that desc in graph Desc().nodes

    UsdGenNodeId           id = kUsdGenInvalidNode;
    TfToken                type;
    int                    algorithmVersion = 0;
    std::unique_ptr<UsdGenOp> op;

    UsdGenNodeId           input = kUsdGenInvalidNode;   // primary usdGen:input (dense range test)
    std::vector<UsdGenNodeId> inputs;                   // all usdGen:input targets

    UsdGenSpace            space = UsdGenSpace::Inherit;       // resolved
    UsdGenReadPhase        readPhase = UsdGenReadPhase::Final; // Preceding -> Final (R9)
    UsdGenTopoFx           topoFx = UsdGenTopoFx::None;
    UsdGenRole             role = UsdGenRole::Curves;
    bool                   enabled = true;

    UsdGenEpoch            structuralDigest {};  // Merkle d(n) (03 §3.3)
    UsdGenEpoch            captureEpoch {};      // (03 §3.4)
    uint64_t               valueVersion = 0;     // (03 §3.2)
    uint64_t               topologySeq = 0;       // monotone install counter for generators

    UsdGenCurveBuffer      buffer;               // this node's output (S24)
    std::unique_ptr<UsdGenCapture> capture;       // epoch-keyed payload
    std::vector<UsdGenChunkDesc> chunks;          // this node's chunk partition
    std::vector<uint8_t>      chunkDirty;         // one dirty byte per chunk (UsdGenDirtyBits)
    std::vector<uint8_t>      chunkCaptured;      // capture-validity byte per chunk (reference lane: single flag)

    // --- compile-time routing data (03 §5.1; filled by UsdGenCompiler) ----
    UsdGenParamView        paramView;            // points into the graph's desc copy
    std::vector<std::pair<TfToken, uint32_t>> paramRouting;  // param name -> UsdGenDirtyBits
    std::vector<UsdGenNodeId> descendants;       // strict descendants, topological order
    UsdGenSurfaceId        surface = 0;
    bool                   hasSurface = false;
    std::vector<SdfPath>   curveRefs;            // usdGen:curves/guides/frozen:curves targets
    std::vector<SdfPath>   mapRefs;              // usdGen:mask:source targets
    bool                   captureNeeded = false; // set by the session, cleared by the capture step

    // --- evaluation-skip state (value-class no-op detection; keeps SI-2's
    // "nothing else" notice set exact and the sparse path cheap) ---
    uint64_t paramValueDigest = 0;  // hash of the value-class params + enabled
    uint64_t lastParamDigest  = 0;  // the paramValueDigest last evaluated
};

class UsdGenGraph
{
    friend class UsdGenCompiler;
public:
    using NodeId = UsdGenNodeId;
    static constexpr NodeId InvalidNode = kUsdGenInvalidNode;

    UsdGenGraph() = default;
    UsdGenGraph(UsdGenGraph &&) = default;
    UsdGenGraph &operator=(UsdGenGraph &&) = default;
    UsdGenGraph(const UsdGenGraph &) = delete;
    UsdGenGraph &operator=(const UsdGenGraph &) = delete;

    int NodeCount() const noexcept { return int(_nodes.size()); }
    UsdGenCompiledNode &Node(NodeId id);
    UsdGenCompiledNode const &Node(NodeId id) const;


    // --- dirty routing targets (03 §5.2 hops 2-3; the imaging router feeds these) ---
    void DirtyParameter(NodeId id, TfToken const &param);
    void DirtyCapture(NodeId id);
    void DirtyTopology(NodeId id);
    void DirtySurface(UsdGenSurfaceId surface, uint32_t bits);
    void DirtyChunks(NodeId id, TfSpan<const UsdGenChunkId> chunks);
    /// Bumps the capture epoch of every node whose UsdGenNodeDesc::maps names this prim
    /// (asset path edit, ReloadMaps(), textureGeneration bump). §3.6, UsdGenDirtyMap.
    void DirtyMap(SdfPath const &mapPrim);
    /// Bumps the capture epoch of every node whose UsdGenNodeDesc::curves names this prim
    /// (a C3 BasisCurves changed: guide edit, re-freeze, re-import). §3.6, §4.5.
    void DirtyCurves(SdfPath const &curvePrim);

    /// The terminal node's OUTPUT buffer and its tile partition (03 §6.3).
    UsdGenCurveBuffer const &Output() const;
    TfSpan<const UsdGenChunkDesc> Chunks(NodeId id) const;
    TfSpan<const UsdGenTileView>  Tiles() const;

    /// True when any node carries a dirty byte the next commit must consume.
    bool AnyDirty() const noexcept;

    UsdGenGraphDesc const &Desc() const noexcept { return *_desc; }
    std::shared_ptr<const UsdGenCudaExecutionPlan> const& CudaPlan() const noexcept { return _cudaPlan; }
    UsdGenNodeDesc const &NodeDesc(NodeId id) const;

    /// Make an owning routing snapshot.  This is intentionally a fresh copy:
    /// callers can hand the result to an asynchronous imaging owner without
    /// retaining this mutable graph or any operator/node pointers.
    /// Construct it on the graph's execution owner, never concurrently with
    /// graph mutation. Only the resulting snapshot is safe for readers.
    std::shared_ptr<const UsdGenGraphRoutingSnapshot> RoutingSnapshot() const;

    // Chunk/tile partition state (R21; terminal-node topology in M1).
    int ChunkSize() const noexcept { return _chunkSize; }
    int TileTarget() const noexcept { return _tileTarget; }
    int ChunksPerTile() const noexcept { return _chunksPerTile; }
    int NumTiles() const noexcept { return int(_tiles.size()); }
    NodeId TerminalNodeId() const noexcept { return _terminal; }
    UsdGenNodeId NodeIdForPath(SdfPath const &path) const;
    /// OR bits into node id's chunks (all of them) and propagate the value
    /// bits to every strict descendant (03 §5.2 hop 3).
    void MarkNode(NodeId id, uint32_t bits);
    /// Re-partition every node's chunks + the tile set after a topology
    /// change. Per-node CV count is taken from the node's own buffer when
    /// known (a generator upstream of a CV-changing node keeps its own
    /// cv/curve); `cvCount` is the fallback for nodes whose buffers are not
    /// populated yet. Returns true when the layout (curve count, chunk
    /// count or any node's cv/curve) changed; unchanged nodes keep their
    /// chunk and dirty bytes.
    bool Repartition(int totalCurves, int cvCount);
private:
    void _PropagateDescendantBits(uint32_t bits, std::vector<bool> const &descendants);

    std::unique_ptr<UsdGenGraphDesc> _desc;  // owned copy taken at Compile
    std::shared_ptr<const UsdGenCudaExecutionPlan> _cudaPlan;
    std::vector<std::unique_ptr<UsdGenCompiledNode>> _nodes; // indexed by id
    std::vector<UsdGenTileView> _tiles;          // terminal tile partition
    UsdGenNodeId      _terminal = InvalidNode;
    std::unordered_map<SdfPath, UsdGenNodeId, SdfPath::Hash> _nodeByPath;
    int _chunkSize = kUsdGenDefaultChunkSize;
    int _tileTarget = kUsdGenTileTargetDefault;
    int _chunksPerTile = 1;
    int _partitionCurves = 0;   // totalCurves of the last partition (0 == unpartitioned)
};

}  // namespace usdGen

#endif  // USDGEN_GRAPH_H
