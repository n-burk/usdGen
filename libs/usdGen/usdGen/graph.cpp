// usdGen engine — graph implementation (03-execution-engine.md §3, §5.2).
//
// The graph is the compiled, topologically sorted node list. It owns the
// per-node dirty bytes and the tile partition. All mutation happens on the
// commit thread; readers only ever observe the published generation
// (I7), never this structure.
#include "usdGen/graph.h"

#include "usdGen/types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

UsdGenCompiledNode &UsdGenGraph::Node(UsdGenNodeId id)
{
    if (id >= _nodes.size() || !_nodes[id]) {
        throw std::out_of_range("UsdGenGraph::Node: invalid node id");
    }
    return *_nodes[id];
}

UsdGenCompiledNode const &UsdGenGraph::Node(UsdGenNodeId id) const
{
    if (id >= _nodes.size() || !_nodes[id]) {
        throw std::out_of_range("UsdGenGraph::Node: invalid node id");
    }
    return *_nodes[id];
}

UsdGenNodeDesc const &UsdGenGraph::NodeDesc(UsdGenNodeId id) const
{
    if (id >= _nodes.size() || !_nodes[id] || !_nodes[id]->desc) {
        throw std::out_of_range("UsdGenGraph::NodeDesc: invalid node id");
    }
    return *_nodes[id]->desc;
}

UsdGenNodeId UsdGenGraph::NodeIdForPath(SdfPath const &path) const
{
    auto it = _nodeByPath.find(path);
    return it == _nodeByPath.end() ? InvalidNode : it->second;
}

UsdGenCurveBuffer const &UsdGenGraph::Output() const
{
    static UsdGenCurveBuffer const empty;
    if (_terminal == InvalidNode || _nodes.empty() || !_nodes[_terminal]) {
        return empty;
    }
    return _nodes[_terminal]->buffer;
}

TfSpan<const UsdGenChunkDesc> UsdGenGraph::Chunks(UsdGenNodeId id) const
{
    UsdGenCompiledNode const &n = Node(id);
    return TfSpan<const UsdGenChunkDesc>(n.chunks.data(), n.chunks.size());
}

TfSpan<const UsdGenTileView> UsdGenGraph::Tiles() const
{
    return TfSpan<const UsdGenTileView>(_tiles.data(), _tiles.size());
}

bool UsdGenGraph::AnyDirty() const noexcept
{
    for (auto const &n : _nodes) {
        if (!n) continue;
        if (n->captureNeeded) return true;
        for (uint8_t b : n->chunkDirty) {
            if (b != UsdGenDirtyNone) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Dirty routing (03 §5.2 hops 2-4)
// ---------------------------------------------------------------------------

void UsdGenGraph::MarkNode(UsdGenNodeId id, uint32_t bits)
{
    if (id >= _nodes.size() || !_nodes[id]) return;
    UsdGenCompiledNode &n = *_nodes[id];
    if (bits == UsdGenDirtyNone) return;

    if (bits & UsdGenDirtyCapture) n.captureNeeded = true;

    // Hop 3 (03 §5.2): every routed class bit lands in this node's chunk
    // bytes AND in every strict descendant's chunk bytes. Capture and
    // Structural are no exception: a descendant must keep its own dirty
    // state honest for AnyDirty()/recompile even though the Run drain
    // consumes capture at the owner and the session drains structural.
    // (Surface-major chunk order keeps a surface dirty a contiguous
    // range — see DirtySurface.)
    uint8_t const byte = static_cast<uint8_t>(bits);
    for (uint8_t &b : n.chunkDirty) b |= byte;
    for (UsdGenNodeId d : n.descendants) {
        if (d >= _nodes.size() || !_nodes[d]) continue;
        for (uint8_t &b : _nodes[d]->chunkDirty) b |= byte;
    }
}

void UsdGenGraph::DirtyParameter(UsdGenNodeId id, TfToken const &param)
{
    if (id >= _nodes.size() || !_nodes[id]) return;
    UsdGenCompiledNode &n = *_nodes[id];
    uint32_t bits = 0;
    for (auto const &entry : n.paramRouting) {
        if (entry.first == param) { bits = entry.second; break; }
    }
    if (bits == 0) {
        // Unknown usdGen:* leaf on this prim: one-shot value-dirty is the
        // safe superset (02 §6: unknown properties are logged by the router,
        // not silently dropped).
        bits = UsdGenDirtyParameter;
    }
    MarkNode(id, bits);
}

void UsdGenGraph::DirtyCapture(UsdGenNodeId id)
{
    MarkNode(id, UsdGenDirtyCapture);
}

void UsdGenGraph::DirtyTopology(UsdGenNodeId id)
{
    MarkNode(id, UsdGenDirtyTopology | UsdGenDirtyCapture);
}

void UsdGenGraph::DirtySurface(UsdGenSurfaceId surface, uint32_t bits)
{
    // Hop 4: surface-major chunk order -> the chunks binding `surface` are a
    // contiguous range on every node; OR `bits` into them and into the
    // owning nodes + strict descendants.
    for (auto &nPtr : _nodes) {
        if (!nPtr) continue;
        UsdGenCompiledNode &n = *nPtr;
        bool any = false;
        for (size_t c = 0; c < n.chunks.size(); ++c) {
            if (n.chunks[c].surface != surface) continue;
            any = true;
            n.chunkDirty[c] |= static_cast<uint8_t>(bits);
        }
        if (any) {
            uint32_t valueBits = bits & ~(UsdGenDirtyCapture | UsdGenDirtyStructural);
            for (UsdGenNodeId d : n.descendants) {
                if (d >= _nodes.size() || !_nodes[d]) continue;
                UsdGenCompiledNode &dn = *_nodes[d];
                for (size_t c = 0; c < dn.chunks.size(); ++c) {
                    if (dn.chunks[c].surface != surface) continue;
                    if (bits & UsdGenDirtyCapture) dn.captureNeeded = true;
                    dn.chunkDirty[c] |= static_cast<uint8_t>(
                        valueBits ? valueBits : UsdGenDirtyParameter);
                }
            }
        }
    }
}

void UsdGenGraph::DirtyChunks(UsdGenNodeId id, TfSpan<const UsdGenChunkId> chunks)
{
    if (id >= _nodes.size() || !_nodes[id]) return;
    UsdGenCompiledNode &n = *_nodes[id];
    for (UsdGenChunkId c : chunks) {
        if (c < n.chunkDirty.size()) n.chunkDirty[c] |= UsdGenDirtyParameter;
    }
    for (UsdGenNodeId d : n.descendants) {
        if (d >= _nodes.size() || !_nodes[d]) continue;
        for (UsdGenChunkId c : chunks) {
            if (c < _nodes[d]->chunkDirty.size())
                _nodes[d]->chunkDirty[c] |= UsdGenDirtyParameter;
        }
    }
}

void UsdGenGraph::DirtyMap(SdfPath const &mapPrim)
{
    for (auto &nPtr : _nodes) {
        if (!nPtr) continue;
        auto &n = *nPtr;
        for (SdfPath const &m : n.mapRefs) {
            if (m == mapPrim) { n.captureNeeded = true; break; }
        }
    }
}

void UsdGenGraph::DirtyCurves(SdfPath const &curvePrim)
{
    for (auto &nPtr : _nodes) {
        if (!nPtr) continue;
        auto &n = *nPtr;
        for (SdfPath const &c : n.curveRefs) {
            if (c == curvePrim) { n.captureNeeded = true; break; }
        }
    }
}

bool UsdGenGraph::Repartition(int totalCurves, int cvCount)
{
    int const nChunks = ComputeNumChunks(totalCurves, _chunkSize);
    // 03 §1.4: tileTarget is clamped to [32, 256] for the partition.
    int const tileTarget =
        std::clamp(_tileTarget, kUsdGenTileMin, kUsdGenTileMax);
    int const cpt = ComputeChunksPerTile(nChunks, tileTarget);
    int const nTiles = ComputeNumTiles(nChunks, tileTarget);
    bool const tileLayoutChanged = (_chunksPerTile != cpt ||
                                    int(_tiles.size()) != nTiles);
    bool const changed = (_partitionCurves != totalCurves || tileLayoutChanged);
    _partitionCurves = totalCurves;

    if (tileLayoutChanged) {
        _chunksPerTile = cpt;
        _tiles.assign(nTiles, UsdGenTileView());
        for (int t = 0; t < nTiles; ++t) {
            _tiles[t].tile = static_cast<UsdGenTileId>(t);
            _tiles[t].firstChunk = static_cast<uint32_t>(t * cpt);
            // M-10: each tile takes AT MOST chunksPerTile chunks — the last
            // tile holds the remainder, never the whole tail.
            _tiles[t].chunkCount =
                static_cast<uint32_t>(std::min(cpt, nChunks - t * cpt));
        }
    }

    for (auto &nPtr : _nodes) {
        if (!nPtr) continue;
        UsdGenCompiledNode &n = *nPtr;
        uint32_t cvp = static_cast<uint32_t>(std::max(0, cvCount));
        if (n.buffer.totalCurves > 0 &&
            n.buffer.totalCvs % n.buffer.totalCurves == 0) {
            cvp = n.buffer.totalCvs / n.buffer.totalCurves;
        }
        // Layout unchanged for this node: keep chunks AND its dirty bytes
        // (a re-partition must not force a full re-eval of nodes whose
        // curve ranges and cv/curve are untouched — gate E-2 relies on this).
        if (!n.chunks.empty() && int(n.chunks.size()) == nChunks &&
            (n.chunks[0].cvCount == cvp || (n.chunks[0].cvCount == 0 && cvp == 0))) {
            if (tileLayoutChanged) {
                // Tile ids moved under a kept chunk set: refresh them.
                for (auto &cd : n.chunks)
                    cd.tile = static_cast<UsdGenTileId>(
                        cd.firstCurve / std::max<size_t>(1, _chunkSize) /
                        std::max(1, _chunksPerTile));
            }
            continue;
        }
        n.chunks.assign(nChunks, UsdGenChunkDesc());
        for (int c = 0; c < nChunks; ++c) {
            UsdGenChunkDesc &cd = n.chunks[c];
            cd.firstCurve = static_cast<uint32_t>(c * _chunkSize);
            cd.curveCount = static_cast<uint32_t>(
                std::min(_chunkSize, totalCurves - c * _chunkSize));
            cd.firstCv = cd.firstCurve * cvp;
            cd.cvCount = cvp;
            cd.liveCount = cd.curveCount;
            cd.tile = static_cast<UsdGenTileId>(c / std::max(1, _chunksPerTile));
        }
        n.chunkDirty.assign(nChunks, UsdGenDirtyParameter);
        n.chunkCaptured.assign(nChunks, 0);
    }
    return changed;
}

void UsdGenGraph::_PropagateDescendantBits(uint32_t bits,
                                           std::vector<bool> const &)
{
    // Kept for interface symmetry with the pre-M1 scaffold; MarkNode() does
    // the actual descendant propagation.
    TF_UNUSED(bits);
}

}  // namespace usdGen
