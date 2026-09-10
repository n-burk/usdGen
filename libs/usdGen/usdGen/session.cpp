// usdGen engine — session implementation (03 §5.4/§5.6/§6.1/§9.1).
//
// The 8-step commit (03 §5.4), supersession (03 §5.6) and the trigger wiring
// (06 §3.9) are the engine + imaging lanes' M1 work. Threading invariants:
// _commitMutex is non-recursive and held only by Commit; Generation() is a
// lock-free atomic load (I7). The scheduler owns steps 2-5 inside Run();
// the session owns recompile routing (1), generation build + publish (6) and
// the dirty diff (7).
#include "usdGen/session.h"

#include "usdGenMath/usdGenMath/hash.h"

#include "pxr/pxr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

// <description>/__usdGenRender/tile_NNNN — must stay byte-identical to the
// imaging-side UsdGenTilePublisher::TilePath formula (06 §4.2).
SdfPath _TilePath(SdfPath const &description, UsdGenTileId tile)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "tile_%04d", static_cast<int>(tile));
    return description
        .AppendChild(TfToken("__usdGenRender"))
        .AppendChild(TfToken(buf));
}

// Sample a named curve buffer plane into the publication's uniform array.
// "uniform" planes index by curve, "vertex" planes by CV.
void _GatherPlane(const UsdGenPlane &plane, uint32_t curveIdx, uint32_t cvIdx,
                  bool wantUniform, VtFloatArray *outF)
{
    if (plane.type == TfToken("int") || plane.arity == 0) return;
    const size_t idx =
        (wantUniform && plane.interpolation != TfToken("vertex"))
            ? curveIdx : cvIdx;
    if ((idx + 1) * plane.arity > plane.f.size()) return;
    for (uint8_t k = 0; k < plane.arity; ++k)
        outF->push_back(plane.f[idx * plane.arity + k]);
}

// Color planes may be float3 (arity 3) or float (arity 1, grey).
void _GatherColor(const UsdGenPlane &plane, uint32_t curveIdx, uint32_t cvIdx,
                  VtVec3fArray *out)
{
    VtFloatArray tmp;
    _GatherPlane(plane, curveIdx, cvIdx, /*wantUniform=*/true, &tmp);
    if (plane.arity >= 3) {
        for (size_t q = 0; q + 3 <= tmp.size(); q += 3)
            out->push_back(GfVec3f(tmp[q], tmp[q + 1], tmp[q + 2]));
    } else if (plane.arity == 1) {
        for (float v : tmp) out->push_back(GfVec3f(v, v, v));
    }
}

const UsdGenPlane *_FindPlane(const std::vector<UsdGenPlane> &planes,
                              TfToken name)
{
    for (const UsdGenPlane &p : planes)
        if (p.name == name) return &p;
    return nullptr;
}

}  // namespace

UsdGenSession::UsdGenSession(int threadLimit) : _scheduler(threadLimit) {}

UsdGenSession::~UsdGenSession() = default;

void UsdGenSession::SetGraphDesc(UsdGenGraphDesc const &desc)
{
    // A new desc is a structural dirty: the next commit recompiles from
    // scratch (03 §5.4 step 1). The session copies the desc (03 §2.2).
    _desc = desc;
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pending = UsdGenPendingDirty{true, false, {}, {}};
    }
    _dirty = true;
}

void UsdGenSession::SetContext(UsdGenContext context) { _context = context; }

void UsdGenSession::AccumulateDirty(UsdGenPendingDirty &&pending)
{
    if (!pending.Any()) return;
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        if (pending.structural) _pending = UsdGenPendingDirty{true, false, {}, {}};
        else {
            if (pending.surfaceTopology) _pending.surfaceTopology = true;
            for (auto const &kv : pending.nodeBits)
                _pending.nodeBits[kv.first] |= kv.second;
            for (auto const &kv : pending.surfaceBits)
                _pending.surfaceBits[kv.first] |= kv.second;
        }
    }
    _dirty = true;
}

bool UsdGenSession::NeedsCommit() const noexcept { return _dirty.load(); }

UsdGenGenerationConstPtr UsdGenSession::Commit(double frame, UsdGenCommitReason reason)
{
    std::lock_guard<std::mutex> lock(_commitMutex);
    const auto t0 = std::chrono::steady_clock::now();
    TF_UNUSED(reason);

    // Supersession ticket (03 §5.6): any newer Commit/trigger bumps the
    // counter; if it moved while we cooked, we abandon publication.
    const uint64_t myReq =
        _generationRequested.fetch_add(1, std::memory_order_acq_rel) + 1;

    UsdGenPendingDirty pending;
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        pending = std::move(_pending);
        _pending = UsdGenPendingDirty{};
    }

    // -- step 1: recompile on structural dirt (03 §3.5, E-6) --------------
    const bool structural = pending.structural || _graph.NodeCount() == 0;
    if (structural) {
        UsdGenCompileResult cr = _compiler.Compile(_desc, &_graph);
        ++_stats.recompiles;
        if (!cr.ok) {
            // Bad graph: keep the previous generation published, stay dirty.
            _dirty = true;
            return _store.Get();
        }
        _stats.cookedNodes = static_cast<uint64_t>(_graph.NodeCount());
    } else {
        // Route the drained dirties into the graph (03 §5.1): per-node bits
        // through the compiled routing tables, surface bits by range.
        for (auto const &kv : pending.nodeBits)
            if (kv.first < static_cast<UsdGenNodeId>(_graph.NodeCount()))
                _graph.MarkNode(kv.first, kv.second);
        for (auto const &kv : pending.surfaceBits)
            _graph.DirtySurface(kv.first, kv.second);
        if (pending.surfaceTopology)
            for (auto const &s : _desc.surfaces)
                _graph.DirtySurface(s.id, UsdGenDirtySurfaceTopo);
    }

    // -- steps 2-5: reference lane, capture, evaluate, interleave (03 §5.4)
    UsdGenEvalContext evalCtx;
    evalCtx.time = frame;
    evalCtx.desc = &_desc;
    UsdGenRunResult result = _scheduler.Run(_graph, evalCtx, myReq);

    if (result.superseded ||
        _generationRequested.load(std::memory_order_acquire) != myReq) {
        // Newer request: publish nothing, return the PREVIOUS generation,
        // leave _dirty set so the next trigger redoes the work (03 §5.6).
        ++_stats.supersessions;
        _dirty = true;
        return _store.Get();
    }

    // -- step 6: build the immutable generation (03 §6.1) ------------------
    UsdGenGenerationConstPtr prev = _store.Get();
    UsdGenGeneration gen;
    gen.frame = frame;
    gen.tiles.reserve(result.tiles.size());


    for (UsdGenTileView const &tv : result.tiles) {
        // E-4: untouched tiles carry over their publication wholesale (the
        // VtArray copies share buffers, so step 7 sees IsIdentical == true).
        const bool rebuild =
            result.topologyChanged || tv.pointsDirty || tv.widthsDirty;
        const UsdGenTilePublication *carry = nullptr;
        if (!rebuild && prev) {
            auto it = std::lower_bound(
                prev->tiles.begin(), prev->tiles.end(), tv.tile,
                [](UsdGenTilePublication const &p, UsdGenTileId t) {
                    return p.tile < t;
                });
            if (it != prev->tiles.end() && it->tile == tv.tile) carry = &*it;
        }
        if (carry) {
            gen.tiles.push_back(*carry);
            continue;
        }
        gen.tiles.push_back(_BuildTilePublication(tv, result, prev));
    }
    std::sort(gen.tiles.begin(), gen.tiles.end(),
              [](UsdGenTilePublication const &a, UsdGenTilePublication const &b) {
                  return a.tile < b.tile;
              });

    // Signature: the prim-set identity step 7 diffs structurally (03 §6.1).
    gen.signature.tileCount = static_cast<uint32_t>(gen.tiles.size());
    gen.signature.instancerCount = 0;
    gen.signature.primPaths.reserve(gen.tiles.size());
    gen.signature.primTypes.assign(gen.tiles.size(), "basisCurves");
    gen.signature.primvarNames.reserve(gen.tiles.size());
    for (UsdGenTilePublication const &t : gen.tiles) {
        gen.signature.primPaths.push_back(t.primPath.GetString());
        std::vector<std::string> names{"points", "widths", "hairT", "hairId"};
        if (!t.st.empty())            names.emplace_back("st");
        if (!t.displayColor.empty())  names.emplace_back("displayColor");
        if (!t.bakeColor.empty())     names.emplace_back("bakeColor");
        if (!t.velocities.empty())    names.emplace_back("velocities");
        for (UsdGenPlane const &p : t.extraUniform)
            names.push_back(p.name.GetString());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        gen.signature.primvarNames.push_back(std::move(names));
    }

    _store.Publish(std::move(gen));

    // -- step 7: diff vs the previous generation (06 §5.1) -----------------
    UsdGenGenerationConstPtr next = _store.Get();
    _lastReport = _store.Diff(prev ? *prev : UsdGenGeneration{}, *next);

    // -- stats (03 §9.2) ----------------------------------------------------
    ++_stats.commits;
    _stats.publishedTiles = static_cast<uint64_t>(next->tiles.size());
    _lastNodeStats.clear();
    double captureMs = 0.0, evalMs = 0.0;
    for (UsdGenNodeRunStats const &rs : result.nodeStats) {
        _lastNodeStats[rs.id] = rs;
        captureMs += rs.captureMs;
        evalMs += rs.evalMs;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> dt = t1 - t0;
    UsdGenCommitTiming &slot = _stats.ring[_stats.ringHead];
    slot = UsdGenCommitTiming{};
    slot.generation = static_cast<uint64_t>(next->id);
    slot.captureMs = captureMs;
    slot.evaluateMs = evalMs;
    slot.totalMs = dt.count();
    _stats.ringHead = (_stats.ringHead + 1) % _stats.ring.size();

    // Remaining chunk dirt (skipped no-op nodes are clean; anything the
    // scheduler could not run stays dirty; 03 §5.4).
    _dirty = _graph.AnyDirty();
    return next;
}

UsdGenTilePublication UsdGenSession::_BuildTilePublication(
    UsdGenTileView const &tv, UsdGenRunResult const &result,
    UsdGenGenerationConstPtr const &prev)
{
    TF_UNUSED(prev);
    UsdGenTilePublication pub;
    pub.tile = tv.tile;
    pub.primPath = _TilePath(_desc.description, tv.tile);
    if (!_desc.curveBasis.IsEmpty())
        pub.basis = _desc.curveBasis.GetString();
    pub.refineLevel = 2;

    UsdGenCurveBuffer const &term = *result.terminalOutput;
    UsdGenCompiledNode const &tn = _graph.Node(_graph.TerminalNodeId());

    pub.curveVertexCounts.reserve(tv.totalLiveCurves);
    pub.points.reserve(tv.totalLiveCvs);
    pub.widths.reserve(tv.totalLiveCvs);
    pub.hairT.reserve(tv.totalLiveCvs);
    pub.hairId.reserve(tv.totalLiveCurves);

    UsdGenPlane const *displayColor = _FindPlane(term.extraCurve, TfToken("displayColor"));
    std::vector<UsdGenPlane const *> uniformPlanes;
    for (UsdGenPlane const &p : term.extraCurve) {
        if (p.name == TfToken("displayColor")) continue;
        if (p.interpolation == TfToken("vertex")) continue;   // M1: uniform lanes
        uniformPlanes.push_back(&p);
    }
    std::vector<VtFloatArray> extra(uniformPlanes.size());

    GfRange3f bounds;
    uint32_t depSurface = 0;
    bool hasDep = false;
    bool firstChunk = true;

    for (uint32_t i = 0; i < tv.chunkCount; ++i) {
        UsdGenChunkDesc const &cd = tn.chunks[tv.firstChunk + i];
        // id 0 is a legitimate surface (compiler.cpp assigns dense indices
        // from 0): the first chunk of the tile always establishes the
        // dependency, never a `!= 0` sentinel.
        if (firstChunk) { depSurface = cd.surface; hasDep = true; firstChunk = false; }
        for (uint32_t c = 0; c < cd.liveCount; ++c) {
            const uint32_t g = cd.firstCurve + c;
            uint32_t p0 = 0, len = 0;
            if (cd.cvCount != 0) {
                len = cd.cvCount;
                p0 = cd.firstCv + c * cd.cvCount;
            } else if (g + 1 < term.cvOffsets.size()) {   // ragged chunk
                p0 = static_cast<uint32_t>(term.cvOffsets[g]);
                len = static_cast<uint32_t>(term.cvOffsets[g + 1]) - p0;
            }
            pub.curveVertexCounts.push_back(static_cast<int>(len));
            for (uint32_t v = 0; v < len; ++v) {
                const uint32_t p = p0 + v;
                if (p >= term.px.size()) break;
                pub.points.emplace_back(term.px[p], term.py[p], term.pz[p]);
                // C2 (06 §4.1): widths/hairT are vertex channels on every
                // tile. A chain whose terminal never wrote them (grow-only:
                // kPlanePoints|kPlaneHairT) still publishes FULL-SIZE planes
                // from desc defaults (02 §2.6/§2.14: width 0.01, look bake),
                // never a wrong-size array — SI-1 sizes every non-empty
                // vertex plane against points.
                float w = 0.01f;
                if (p < term.width.size()) w = term.width[p];
                else if (!term.width.empty()) w = term.width.back();
                pub.widths.push_back(w);
                pub.hairT.push_back(p < term.hairT.size() ? term.hairT[p] : 0.0f);
            }
            // hairId: UsdGenHash32(curveId, 0) / 2^32 in [0,1) (06, S29).
            pub.hairId.push_back(g < term.curveId.size()
                ? UsdGenHairId(term.curveId[g]) : 0.0f);
            if (!term.rootUV.empty() && g < term.rootUV.size())
                pub.st.push_back(term.rootUV[g]);
            if (displayColor)
                _GatherColor(*displayColor, g, p0, &pub.displayColor);
            else if (_desc.look.bakeTarget != TfToken("none"))
                pub.displayColor.push_back(_desc.look.rootColor);
            for (size_t k = 0; k < uniformPlanes.size(); ++k)
                _GatherPlane(*uniformPlanes[k], g, p0, /*wantUniform=*/true,
                             &extra[k]);
        }
    }
    for (size_t k = 0; k < uniformPlanes.size(); ++k) {
        UsdGenPlane out;
        out.name = uniformPlanes[k]->name;
        out.interpolation = TfToken("uniform");
        out.type = TfToken("float");
        out.arity = uniformPlanes[k]->arity;
        out.f = std::move(extra[k]);
        pub.extraUniform.push_back(std::move(out));
    }

    // Extent is a pure function of the published points (03 §6.3: min/max
    // fused into the interleave loop — but InterleaveTile skips untouched
    // tiles, so tv.extent is empty on any tile this commit did not evaluate
    // while its points are real). Reduce over pub.points in memory: same
    // (g,p0,len) ragged walk already emitted them above, one cheap pass.
    GfRange3f e;
    for (GfVec3f const &pt : pub.points) e.ExtendBy(pt);
    if (e.IsEmpty()) e = GfRange3f(GfVec3f(0.f), GfVec3f(0.f));
    pub.extentMin = GfVec3d(e.GetMin()[0], e.GetMin()[1], e.GetMin()[2]);
    pub.extentMax = GfVec3d(e.GetMax()[0], e.GetMax()[1], e.GetMax()[2]);

    pub.xformMatrix = _desc.xformMatrix;
    pub.purpose = _desc.purpose;
    pub.visibility = _desc.visibility;
    pub.materialPath = _desc.materialPath;
    pub.materialPurpose = TfToken("allPurpose");
    pub.primOrigin = _desc.pickTarget == TfToken("description")
        ? _desc.description : SdfPath();
    if (hasDep) {
        for (UsdGenSurfaceDesc const &s : _desc.surfaces)
            if (s.id == depSurface) { pub.dependencySurface = s.path; break; }
    }
    return pub;
}

UsdGenGenerationConstPtr UsdGenSession::Generation() const noexcept
{
    return _store.Get();  // std::atomic_load inside the store; never locks.
}

void UsdGenSession::InvalidateAllValues()
{
    // Bench hook (E-1/E-2): break every node's value-equality signature so
    // the evaluation-skip gate (paramValueDigest != lastParamDigest) forces
    // a full re-evaluate on the next commit. Capture state is untouched.
    for (UsdGenNodeId id = 0; id < static_cast<UsdGenNodeId>(_graph.NodeCount()); ++id) {
        UsdGenCompiledNode &n = _graph.Node(id);
        n.paramValueDigest = ~uint64_t(0);
        n.lastParamDigest = 0;
    }
    _dirty = true;
}

void UsdGenSession::BeginDensityDrag()
{
    // S28 parked publication mode is deferred (M4, 02 §2.3.1): M1 keeps
    // committed-mode publication through a drag; the flag only marks intent.
    _densityDrag = true;
}

void UsdGenSession::EndDensityDrag()
{
    // M1: no parked-mode backlog exists, so a plain dirty recommit republishes
    // the real counts.
    _densityDrag = false;
    _dirty = true;
}

std::optional<UsdGenNodeStats> UsdGenSession::NodeStats(UsdGenNodeId id) const
{
    // 03 §9.2 counters for the last published commit.
    if (id >= static_cast<UsdGenNodeId>(_graph.NodeCount())) return std::nullopt;
    auto it = _lastNodeStats.find(id);
    if (it == _lastNodeStats.end()) return std::nullopt;
    UsdGenCompiledNode const &n = _graph.Node(id);
    UsdGenNodeStats s;
    s.type = n.type;
    s.path = n.desc ? n.desc->path : SdfPath();
    s.captureMs = it->second.captureMs;
    s.lastEvalMs = it->second.evalMs;
    s.meanEvalMs = it->second.evalMs;   // single-sample M1 cache
    s.curvesOut = n.buffer.totalCurves;
    uint64_t dirtyChunks = 0;
    for (uint8_t byte : n.chunkDirty)
        if (byte != 0) ++dirtyChunks;
    s.chunksDirty = dirtyChunks;
    s.chunksTotal = static_cast<uint64_t>(n.chunks.size());
    return s;
}

}  // namespace usdGen
