// usdGen engine — scheduler implementation (03-execution-engine.md §5.3/§5.4).
//
// Run() executes the engine-side commit steps in topological node order
// (M1's chains are DAGs with a single input per node, so per-node
// capture-then-evaluate is equivalent to the plan's separate capture phase
// and evaluate phase, while keeping every capture's upstream fully final):
//   - reference lane (03 §1.5): path reserved; no M1 operator has the
//     Reference role, and encountering one is a diagnostics error;
//   - re-capture when the capture epoch moved (03 §3.4); generators install
//     their captured buffer and the graph re-partitions on topology change;
//   - per-node chunk-evaluate sweep, tbb::parallel_for inside the private
//     task_arena (I8 — never pxr work::, which would serialise under
//     PXR_WORK_THREAD_LIMIT); chunk skip is governed by the dirty bytes plus
//     the evaluation signature (param value digest + upstream valueVersion);
//   - tile interleave: extents + dirty flags over dirty tiles only.
//
// I8: every parallel region is a plain tbb::parallel_for inside
// _arena.execute, never pxr work::.
#include "usdGen/scheduler.h"

#include "usdGen/graph.h"
#include "usdGen/op.h"
#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/range3f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/vt/array.h"

#include "tbb/parallel_for.h"
#include "tbb/task_arena.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

int ResolveThreadLimit(int requested)
{
    if (requested > 0) return requested;
    const std::string env = TfGetenv("USDGEN_THREAD_LIMIT");  // 26.08: std::string
    if (!env.empty()) {
        const int parsed = std::atoi(env.c_str());
        if (parsed > 0) return parsed;
    }
    // 03 §5.3: the measured 8-thread knee (EV-001/EV-008, this host is
    // heterogeneous) is the documented default until gate E-7 ships the
    // one-shot sweep; `USDGEN_THREAD_LIMIT` above wins and skips it.
    return 8;
}


UsdGenCurveBuffer const &EmptyBuffer()
{
    static UsdGenCurveBuffer const empty;
    return empty;
}

/// Per-curve arrays a styler needs but does not compute (inherited from the
/// upstream buffer; VtArray assignment is a cheap CoW share).
void InheritPerCurve(UsdGenCurveBuffer &dst, UsdGenCurveBuffer const &src)
{
    if (dst.curveId.empty() && !src.curveId.empty()) dst.curveId = src.curveId;
    if (dst.rootPrim.empty() && !src.rootPrim.empty()) dst.rootPrim = src.rootPrim;
    if (dst.rootUV.empty() && !src.rootUV.empty()) dst.rootUV = src.rootUV;
    if (dst.rootT.empty() && !src.rootT.empty()) dst.rootT = src.rootT;
    if (dst.rootN.empty() && !src.rootN.empty()) dst.rootN = src.rootN;
    if (dst.rootB.empty() && !src.rootB.empty()) dst.rootB = src.rootB;
    if (dst.curveMask.empty() && !src.curveMask.empty()) dst.curveMask = src.curveMask;
}

/// Commit-thread preparation of one node's output buffer before its
/// evaluate sweep: topology fields and per-curve arrays are inherited from
/// the upstream node; CV planes the operator does NOT touch are CoW-aliased
/// from upstream (re-aliased every commit, so a stale shared array can
/// never outlive its source); planes it DOES touch must exist at full
/// buffer size (chunk-local pointers are sliced from these).
void PrepareNodeForEval(
    UsdGenCompiledNode &node, UsdGenOp &op,
    UsdGenCurveBuffer const &upBuf, bool hasUp)
{
    UsdGenCurveBuffer &buf = node.buffer;

    // Inherit the topology FIRST: the CV count of the sizing below must be
    // the node's EFFECTIVE curve count, not its pre-inheritance 0 (an
    // un-populated styler buffer would otherwise size every touched plane
    // to zero and hand the kernel null plane pointers).
    if (hasUp && !op.IsGenerator()) {
        if (buf.totalCurves == 0) {
            buf.totalCurves = upBuf.totalCurves;
            buf.totalCvs = upBuf.totalCvs;
            buf.topologyVersion = upBuf.topologyVersion;
        }
        InheritPerCurve(buf, upBuf);
        if (!(node.capture && node.capture->OwnsBuffer())) {
            // Topology metadata refresh/clear on pass-through preps only; owning
            // captures (e.g. Deform) keep their authored cvOffsets.
            buf.cvOffsets = upBuf.cvOffsets;
        } else if (buf.cvOffsets.empty() && !upBuf.cvOffsets.empty() &&
                   buf.totalCurves == upBuf.totalCurves &&
                   buf.totalCvs == upBuf.totalCvs) {
            // FIX (P1, ragged-at-Deform): an owning capture that authored NO
            // topology of its own (Deform — the totals just above are the
            // inherited upstream ones) must not drop a ragged input layout:
            // ragged totals with EMPTY offsets make Repartition derive a
            // uniform cvp (totalCvs/totalCurves == 12/3 == 4) and sweep
            // ragged spans as uniform. Carry the input cvOffsets verbatim,
            // mirroring the pass-through carry; the totals equality pins
            // "no topology authored" so a re-topologised owner keeps its own.
            buf.cvOffsets = upBuf.cvOffsets;
        }
    }

    uint32_t const nCurves = buf.totalCurves;
    uint32_t const cvp = node.chunks.empty() ? 0 : node.chunks[0].cvCount;
    uint32_t const nCvs = (cvp == 0 && !buf.cvOffsets.empty() &&
                           size_t(nCurves) < buf.cvOffsets.size())
        ? uint32_t(buf.cvOffsets[nCurves])   // ragged: total CVs from offsets
        : nCurves * cvp;

    // Plane policy (03 §1.2/§8.5, S24): a plane the OP WRITES must be THIS
    // node's own storage (VtArray CoW sharing would make the kernel's write
    // detach-and-memcpy the whole buffer once per run — the measured cost of
    // every sparse commit); a plane the op passes through is a read-only
    // alias of the upstream plane, refreshed only when the identity differs.
    // Kernels write through the raw pointer SweepChunk slices from
    // cdata()+const_cast — data() is never called on a potentially shared
    // array (see EnsureNoDetaches).
    uint32_t const planes = op.PlanesTouched();
    auto prep = [&](bool write, VtFloatArray &a, VtFloatArray const &up) {
        if (write) {
            if (a.size() != nCvs || (hasUp && a.cdata() == up.cdata()))
                a = VtFloatArray(nCvs, 0.0f);   // fresh private storage
        } else if (hasUp && up.size() == nCvs) {
            if (a.cdata() != up.cdata()) a = up;  // CoW share; read-only
        } else if (a.size() != nCvs) {
            a = VtFloatArray(nCvs, 0.0f);
        }
    };
    prep(planes & UsdGenOp::kPlanePoints, buf.px, upBuf.px);
    prep(planes & UsdGenOp::kPlanePoints, buf.py, upBuf.py);
    prep(planes & UsdGenOp::kPlanePoints, buf.pz, upBuf.pz);
    prep(planes & UsdGenOp::kPlaneWidths, buf.width, upBuf.width);
    prep(planes & UsdGenOp::kPlaneHairT, buf.hairT, upBuf.hairT);
}

/// Kept as the single documented answer to "who may call data()": NOBODY.
/// A shared plane is written only through the const-cast raw pointer; every
/// potential writer already holds PRIVATE storage (prep re-allocates when a
/// to-be-written plane still aliases its input). This function now only
/// guarantees generators' planes exist at buffer size; it must never detach.
void EnsureNoDetaches(UsdGenCompiledNode &node, UsdGenOp &op)
{
    TF_UNUSED(node); TF_UNUSED(op);
}
struct NodeSweepPayload
{
    UsdGenCompiledNode *node;
    UsdGenGraph const *graph;
    UsdGenEvalContext ctx;          // node-local copy (params/seed already set)
    UsdGenOp const *op;
    UsdGenCompiledNode const *up;  // null when the node has no input
    bool evalAll;
    float blend;
    bool blendable;                // styler with an input: capture !OwnsBuffer() && topoFx None (03 §8.5)
    uint32_t planes;
    std::vector<uint8_t> didEval;  // one byte per chunk: a bit-packed
                                   // vector<bool> makes 64 workers share one
                                   // word — cache-line ping-pong (03 §5.4
                                   // post-sweep bookkeeping lives on the
                                   // commit thread; per-chunk flags must not
                                   // false-share).
};

void SweepChunk(size_t index, void *payload)
{
    NodeSweepPayload &pl = *static_cast<NodeSweepPayload *>(payload);
    UsdGenCompiledNode &node = *pl.node;
    UsdGenChunkDesc const &cd = node.chunks[index];
    if (cd.curveCount == 0) return;
    bool const chunkDirty =
        node.chunkDirty.empty()
            ? true   // no dirty bookkeeping for this partition: evaluate (never skip blindly)
            : (node.chunkDirty[index] & UsdGenDirtyParameter) != 0;
    if (!pl.evalAll && !chunkDirty) return;

    UsdGenCurveBuffer &buf = node.buffer;
    UsdGenCurveBuffer const &upBuf = pl.up ? pl.up->buffer : EmptyBuffer();
    UsdGenChunkDesc const *upC = (pl.up && pl.up->chunks.size() == node.chunks.size())
        ? &pl.up->chunks[index] : nullptr;

    uint32_t nCvs = cd.curveCount * cd.cvCount;
    if (cd.cvCount == 0 && !buf.cvOffsets.empty()) {
        // Ragged span [firstCurve, firstCurve+curveCount) from topology offsets.
        size_t const f = cd.firstCurve;
        if (f + cd.curveCount < buf.cvOffsets.size())
            nCvs = uint32_t(buf.cvOffsets[f + cd.curveCount] - buf.cvOffsets[f]);
    }
    size_t const base = cd.firstCv;
    size_t const upBase = upC ? upC->firstCv : 0;

    UsdGenChunkView view{};
    view.desc = &cd;
    view.curveCount = cd.curveCount;
    view.cvCount = cd.cvCount;
    view.inCvCount = upC ? upC->cvCount : 0;
    view.outCount = 0;
    view.inCount = 0;
    view.outF = nullptr;
    view.outI = nullptr;
    view.inF = nullptr;
    view.inI = nullptr;
    // Chunk-local BY BINDING (zero copy): absolute offsets; planes below slice at
    // base=cd.firstCv, so the op-facing plane-relative CV index is
    //   view->cvOffsets[c] - int(desc->firstCv) + i
    // (ops MUST apply the -firstCv subtraction; uniform fast path stays null).
    view.cvOffsets = (cd.cvCount == 0 && !buf.cvOffsets.empty() &&
                      size_t(cd.firstCurve) < buf.cvOffsets.size())
        ? buf.cvOffsets.cdata() + cd.firstCurve : nullptr;

    auto outPlane = [&](VtFloatArray &a) -> float * {
        // cdata(), NOT data(): detach is hoisted to the commit thread
        // (EnsureOwnedPlanes). data() here would CoW-copy the full buffer
        // once per chunk while a pass-through alias holds a reference.
        return a.empty() ? nullptr : const_cast<float *>(a.cdata()) + base;
    };
    auto inPlane = [&](VtFloatArray const &a) -> float const * {
        return a.empty() ? nullptr : a.cdata() + upBase;
    };

    if (pl.planes & UsdGenOp::kPlanePoints) {
        view.px = outPlane(buf.px);
        view.py = outPlane(buf.py);
        view.pz = outPlane(buf.pz);
        view.inPx = inPlane(upBuf.px);
        view.inPy = inPlane(upBuf.py);
        view.inPz = inPlane(upBuf.pz);
    } else {
        view.px = buf.px.empty() ? nullptr : const_cast<float *>(buf.px.cdata()) + base;
        view.py = buf.py.empty() ? nullptr : const_cast<float *>(buf.py.cdata()) + base;
        view.pz = buf.pz.empty() ? nullptr : const_cast<float *>(buf.pz.cdata()) + base;
    }
    view.inWidth = inPlane(upBuf.width);   // upstream-width READ port (03 §1.2)
    if (pl.planes & UsdGenOp::kPlaneWidths) {
        view.width = outPlane(buf.width);
        if (!view.inPx) view.inPx = inPlane(upBuf.px);
    } else {
        view.width = buf.width.empty() ? nullptr
            : const_cast<float *>(buf.width.cdata()) + base;
    }
    if (pl.planes & UsdGenOp::kPlaneHairT) {
        view.hairT = outPlane(buf.hairT);
    } else {
        view.hairT = buf.hairT.empty() ? nullptr
            : const_cast<float *>(buf.hairT.cdata()) + base;
    }

    // Per-curve arrays (read-only views into the node's own buffer).
    size_t const baseCurve = cd.firstCurve;
    view.curveId = buf.curveId.empty() ? nullptr
        : buf.curveId.cdata() + baseCurve;
    view.rootPrim = buf.rootPrim.empty() ? nullptr
        : buf.rootPrim.cdata() + baseCurve;
    view.rootUV = buf.rootUV.empty() ? nullptr
        : buf.rootUV.cdata() + baseCurve;
    view.rootT = buf.rootT.empty() ? nullptr
        : const_cast<GfVec3f *>(buf.rootT.cdata()) + baseCurve;
    view.rootN = buf.rootN.empty() ? nullptr
        : const_cast<GfVec3f *>(buf.rootN.cdata()) + baseCurve;
    view.rootB = buf.rootB.empty() ? nullptr
        : const_cast<GfVec3f *>(buf.rootB.cdata()) + baseCurve;
    view.curveMask = buf.curveMask.empty() ? nullptr
        : const_cast<float *>(buf.curveMask.cdata()) + baseCurve;

    // Capture-payload views: the resolved mask is the node's own; the
    // magnitude LUT comes from the node's capture.
    view.rampLut = nullptr;
    if (node.capture) {
        if (auto p = dynamic_cast<UsdGenCapturePayload const *>(node.capture.get())) {
            view.rampLut = p->rampLut.empty() ? nullptr : p->rampLut.cdata();
            if (!p->curveMask.empty())
                view.curveMask = const_cast<float *>(p->curveMask.cdata()) + baseCurve;
        }
    }

    // ---- envelope (03 §8.5), applied by the framework, not by kernels ----
    // Generators (captures that OwnsBuffer) are exempt — their output is
    // created, not styled, and blend on a TopologyEffect node is ignored
    // with a diagnostic (emitted once per run in Run(), not per chunk).
    // w <= 0 skips Evaluate entirely and aliases the input; 0 < w < 1 lerps
    // every touched plane; w >= 1 runs no blend pass. Untouched planes are
    // already CoW aliases of the input from PrepareNodeForEval.
    bool const blendable = pl.blendable;
    bool const muted = blendable && pl.blend <= 0.0f;
    if (!muted) {
        pl.op->Evaluate(pl.ctx, *node.capture, &view);
    }

    float const *inPx = inPlane(upBuf.px);
    float const *inPy = inPlane(upBuf.py);
    float const *inPz = inPlane(upBuf.pz);
    float const *inW = view.inWidth;
    float const *inT = inPlane(upBuf.hairT);
    if (muted) {
        // w <= 0: out := in on every plane this node would have touched.
        if (pl.planes & UsdGenOp::kPlanePoints) {
            if (view.px && inPx) std::copy_n(inPx, nCvs, view.px);
            if (view.py && inPy) std::copy_n(inPy, nCvs, view.py);
            if (view.pz && inPz) std::copy_n(inPz, nCvs, view.pz);
        }
        if ((pl.planes & UsdGenOp::kPlaneWidths) && view.width && inW)
            std::copy_n(inW, nCvs, view.width);
        if ((pl.planes & UsdGenOp::kPlaneHairT) && view.hairT && inT)
            std::copy_n(inT, nCvs, view.hairT);
        for (uint32_t s = 0; view.outF && s < view.outCount; ++s)
            if (view.outF[s] && view.inF && view.inF[s])
                std::copy_n(view.inF[s], nCvs, view.outF[s]);
    } else if (blendable && pl.blend > 0.0f && pl.blend < 1.0f) {
        if ((pl.planes & UsdGenOp::kPlanePoints) && view.px && inPx && inPy && inPz)
            UsdGenBlendEnvelopeVec3(inPx, inPy, inPz, view.px, view.py, view.pz,
                                    pl.blend, nCvs);
        if ((pl.planes & UsdGenOp::kPlaneWidths) && view.width && inW)
            UsdGenBlendEnvelope(inW, view.width, pl.blend, nCvs);
        if ((pl.planes & UsdGenOp::kPlaneHairT) && view.hairT && inT)
            UsdGenBlendEnvelope(inT, view.hairT, pl.blend, nCvs);
        for (uint32_t s = 0; view.outF && s < view.outCount; ++s)
            if (view.outF[s] && view.inF && view.inF[s])
                UsdGenBlendEnvelope(view.inF[s], view.outF[s], pl.blend, nCvs);
    }

    pl.didEval[index] = 1;
}

// ---------------------------------------------------------------------------
// Tile-interleave body (03 §5.4 step 5): per-tile extents + dirty flags.
// Tiles are disjoint memory; the pass parallelises in the private arena.
// ---------------------------------------------------------------------------

struct InterleavePayload
{
    UsdGenGraph *graph;
    UsdGenCompiledNode const *tn;
    UsdGenCurveBuffer const *term;
    std::vector<char> *touched;
    bool widthsFlag;
};

void InterleaveTile(size_t index, void *payload)
{
    InterleavePayload &pl = *static_cast<InterleavePayload *>(payload);
    UsdGenTileView &tv =
        const_cast<UsdGenTileView &>(pl.graph->Tiles()[index]);
    // Tile dirty flags are per-run observations (03 §5.4 step 5), not sticky
    // state: clear first so a run that evaluates no chunk of this tile reports
    // "nothing changed" instead of replaying the previous run's flag.
    tv.pointsDirty = false;
    tv.widthsDirty = false;
    if (!(*pl.touched)[index]) return;

    // Terminal node owns the chunk partition the tiles index into.
    if (pl.tn->chunks.size() < tv.firstChunk + tv.chunkCount) return;

    UsdGenCurveBuffer const &term = *pl.term;
    GfRange3f extent;
    uint64_t liveCurves = 0, liveCvs = 0;
    float const *tpx = term.px.empty() ? nullptr : term.px.cdata();
    float const *tpy = term.py.empty() ? nullptr : term.py.cdata();
    float const *tpz = term.pz.empty() ? nullptr : term.pz.cdata();
    size_t const nPx = term.px.size();
    bool const ragged = !term.cvOffsets.empty();
    for (uint32_t i = 0; i < tv.chunkCount; ++i) {
        UsdGenChunkDesc const &cd = pl.tn->chunks[tv.firstChunk + i];
        // Ragged path (03 §1.3): per-curve CV spans come from cvOffsets, not
        // liveCount*cvCount (cvCount == 0 on the ragged path). g indexes the
        // absolute curve so cvOffsets[g] is valid.
        if (ragged && cd.cvCount == 0) {
            for (uint32_t c = 0; c < cd.liveCount; ++c) {
                size_t const g = size_t(cd.firstCurve) + c;
                if (g + 1 >= term.cvOffsets.size()) break;
                uint32_t const p0 = static_cast<uint32_t>(term.cvOffsets[g]);
                uint32_t const len =
                    static_cast<uint32_t>(term.cvOffsets[g + 1]) - p0;
                ++liveCurves;
                liveCvs += len;
                if (!tpx || !tpy || !tpz) continue;
                for (uint32_t v = 0; v < len; ++v) {
                    size_t const p = size_t(p0) + v;
                    if (p >= nPx) break;
                    extent.ExtendBy(GfVec3f(tpx[p], tpy[p], tpz[p]));
                }
            }
            continue;
        }
        liveCurves += cd.liveCount;
        liveCvs += uint64_t(cd.liveCount) * cd.cvCount;
        if (!tpx || !tpy || !tpz) continue;
        for (uint32_t c = 0; c < cd.liveCount; ++c) {
            size_t const o = size_t(cd.firstCv) + size_t(c) * cd.cvCount;
            for (uint32_t v = 0; v < cd.cvCount; ++v) {
                size_t const p = o + v;
                if (p >= nPx) break;
                extent.ExtendBy(GfVec3f(tpx[p], tpy[p], tpz[p]));
            }
        }
    }
    tv.extent = extent;
    tv.totalLiveCurves = static_cast<uint32_t>(liveCurves);
    tv.totalLiveCvs = static_cast<uint32_t>(liveCvs);
    tv.pointsDirty = true;
    tv.widthsDirty = pl.widthsFlag;
}

}  // namespace

UsdGenScheduler::UsdGenScheduler(int threadLimit)
    : _arena(ResolveThreadLimit(threadLimit)),
      _threadLimit(ResolveThreadLimit(threadLimit))
{
}

UsdGenScheduler::~UsdGenScheduler() = default;

int UsdGenScheduler::ThreadLimit() const noexcept { return _threadLimit; }

void UsdGenWorkDispatcher::ParallelFor(
    size_t count, void (*body)(size_t, void *), void *payload)
{
    // 03 §5.3: every parallel region is a plain tbb::parallel_for run inside
    // the private arena (never pxr work::, which honours the process-global
    // PXR_WORK_THREAD_LIMIT and would serialise under PXR_WORK_THREAD_LIMIT=1).
    // (E-7 pass 3: an explicit ceil(chunks/workers) grainsize REGRESSED E-1
    // 35.5 vs 28.5 — reverted to the default auto-partitioner.)
    _arena->execute([&]() {
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, count),
            [&](tbb::blocked_range<size_t> const &range) {
                for (size_t i = range.begin(); i < range.end(); ++i) {
                    body(i, payload);
                }
            });
    });
}

UsdGenWorkDispatcher UsdGenScheduler::MakeWorkDispatcher() const
{
    return UsdGenWorkDispatcher(const_cast<tbb::task_arena *>(&_arena));
}

int CalibrateThreads()
{
    // M1: the one-shot sweep of 03 §5.3 is deferred; the 8-thread knee
    // (EV-001/EV-008) is the documented default.
    return 8;
}

UsdGenRunResult UsdGenScheduler::Run(
    UsdGenGraph &graph,
    UsdGenEvalContext const &evalCtx,
    uint64_t generationRequested)
{
    TF_UNUSED(generationRequested);  // supersession is checked by the session

    UsdGenRunResult result;
    if (graph.NodeCount() == 0) return result;

    UsdGenDiagnostics aggregated;
    auto dispatcher = MakeWorkDispatcher();
    UsdGenCurveBuffer const emptyBuf;

    int const nTiles = graph.NumTiles();
    std::vector<char> tileTouched(nTiles, 0);

    for (int pos = 0; pos < graph.NodeCount(); ++pos) {
        UsdGenCompiledNode &node = graph.Node(pos);
        UsdGenOp &op = *node.op;
        UsdGenDiagnostics nodeDiag;

        if (node.role == UsdGenRole::Reference) {
            nodeDiag.Error(std::string("M1: reference-lane operators are not supported yet ") +
                           "(node '" + node.desc->path.GetText() + "')");
        }

        bool const hasUp = node.input != UsdGenGraph::InvalidNode;
        UsdGenCurveBuffer const &upBuf = hasUp ? graph.Node(node.input).buffer : emptyBuf;
        auto const _nt0 = std::chrono::steady_clock::now();

        // ---- capture (03 §5.4 step 3, per node in topo order) ----
        UsdGenCaptureContext cctx;
        cctx.desc = &graph.Desc();
        cctx.params = &node.paramView;
        cctx.references = nullptr;
        cctx.surface = node.hasSurface ? node.surface : 0;
        cctx.readPhase = node.readPhase;
        cctx.seed = node.desc ? static_cast<uint32_t>(node.desc->seed) : 0;
        cctx.upstreamGeneration = hasUp ? upBuf.topologyVersion : 0;
        cctx.dispatcher = &dispatcher;
        cctx.diag = &nodeDiag;

        bool const reCaptured =
            node.captureNeeded ||
            !node.capture ||
            op.CaptureDigest(cctx) != node.captureEpoch ||
            !node.capture->ValidForTopology(upBuf);
        if (reCaptured) {
            auto cap = op.CreateCapture();
            if (cap && op.Capture(cctx, upBuf, cap.get(), &nodeDiag)) {
                if (cap->OwnsBuffer()) {
                    node.buffer = cap->Buffer();
                    node.buffer.topologyVersion = ++node.topologySeq;
                    if (hasUp) InheritPerCurve(node.buffer, upBuf);
                }
                node.capture = std::move(cap);
            } else if (cap) {
                nodeDiag.Error(std::string("UsdGen: capture failed on '") +
                               node.desc->path.GetText() + "'");
            }
            node.captureEpoch = op.CaptureDigest(cctx);
            node.captureNeeded = false;

            // Generators establish/refresh the topology: keep the chunk
            // partition and the tile set in lockstep with it.
            if (node.op->IsGenerator() && node.buffer.totalCurves > 0) {
                int const total = static_cast<int>(node.buffer.totalCurves);
                int const cvp = int(node.buffer.totalCvs /
                                    std::max<uint32_t>(1, node.buffer.totalCurves));
                int const nChunks = ComputeNumChunks(total, graph.ChunkSize());
    bool layoutMoved =
        int(node.chunks.size()) != nChunks ||
        (node.chunks.empty() ? false : int(node.chunks[0].cvCount) != cvp) ||
        !node.buffer.cvOffsets.empty();   // ragged recapture: Repartition itself
                                          // compares per-chunk desired layout
                if (layoutMoved) {
                    if (graph.Repartition(total, cvp))
                        result.topologyChanged = true;
                }
            } else if (reCaptured && node.chunks.empty() && hasUp) {
                // A node carried in by the E-6 recompile path (or lazily
                // captured for the first time) can have NO partition yet
                // while its input does: adopt the input's layout. Repartition
                // keeps every node whose curve/CV layout is unchanged
                // (dirty bytes included), so this is a no-op for the chain.
                UsdGenCompiledNode const &upN = graph.Node(node.input);
                if (!upN.chunks.empty() && upN.buffer.totalCurves > 0 &&
                    graph.Repartition(static_cast<int>(upN.buffer.totalCurves),
                                      static_cast<int>(upN.chunks[0].cvCount)))
                    result.topologyChanged = true;
            }
        }

        for (auto const &e : nodeDiag.errors) aggregated.Error(e);
        for (auto const &w : nodeDiag.warnings) aggregated.Warn(w);
        nodeDiag = UsdGenDiagnostics();

        // Re-capture dirties every chunk of this node AND the same chunks of
        // every descendant (03 §5.2 hop 3). Descendants whose capture epoch
        // moved re-Capture in their own topo turn; nothing else may bypass
        // the per-chunk dirty bytes — an upstream valueVersion bump must
        // not force a full downstream sweep (gate E-2 relies on this).
        if (reCaptured && node.capture) {
            std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                      static_cast<uint8_t>(UsdGenDirtyParameter));
            for (UsdGenNodeId d : node.descendants) {
                if (d >= static_cast<UsdGenNodeId>(graph.NodeCount())) continue;
                UsdGenCompiledNode &dn = graph.Node(d);
                std::fill(dn.chunkDirty.begin(), dn.chunkDirty.end(),
                          static_cast<uint8_t>(UsdGenDirtyParameter));
            }
        }
        auto const _nt1 = std::chrono::steady_clock::now();  // capture done
        // ---- evaluate (03 §5.4 step 4) ----
        // evalAll only for THIS node's own value-class parameter change
        // (paramValueDigest moved); upstream changes arrive as chunk bytes.
        bool const evalAll = node.paramValueDigest != node.lastParamDigest;
        node.lastParamDigest = node.paramValueDigest;
        bool anyChunkDirty = false;
        for (uint8_t b : node.chunkDirty) {
            if (b & UsdGenDirtyParameter) { anyChunkDirty = true; break; }
        }

        bool const mutedPassThrough =
            !node.enabled && node.topoFx == UsdGenTopoFx::None && hasUp;
        if (mutedPassThrough) {
            // Plan §5.4: alias the input buffer, clear dirty, continue.
            UsdGenCurveBuffer &buf = node.buffer;
            buf.px = upBuf.px; buf.py = upBuf.py; buf.pz = upBuf.pz;
            buf.width = upBuf.width; buf.hairT = upBuf.hairT;
            InheritPerCurve(buf, upBuf);
            buf.totalCurves = upBuf.totalCurves;
            buf.totalCvs = upBuf.totalCvs;
            buf.topologyVersion = upBuf.topologyVersion;
            node.lastParamDigest = node.paramValueDigest;
            std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                      UsdGenDirtyNone);
            continue;
        }

        if (!anyChunkDirty && !evalAll) {
            std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                      UsdGenDirtyNone);
            continue;
        }
        if (!node.capture) {
            std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                      UsdGenDirtyNone);
            continue;  // capture failed; downstream stays clean of garbage
        }

        PrepareNodeForEval(node, op, upBuf, hasUp);
        EnsureNoDetaches(node, op);

        NodeSweepPayload pl;
        pl.node = &node;
        pl.graph = &graph;
        pl.ctx = evalCtx;
        pl.ctx.params = &node.paramView;
        pl.ctx.desc = &graph.Desc();
        pl.ctx.seed = node.desc ? static_cast<uint32_t>(node.desc->seed) : 0;
        pl.op = &op;
        pl.up = hasUp ? &graph.Node(node.input) : nullptr;
        pl.evalAll = evalAll;
        pl.blend = node.desc ? static_cast<float>(node.desc->blend) : 1.0f;
        // §8.5: blend is applied by the framework to topology-preserving
        // stylers only — generators (capture OwnsBuffer, never the type
        // string) and TopologyEffect != None nodes ignore it, with one
        // diagnostic per run, not per chunk (M1 compile-notes defect 3).
        pl.blendable = hasUp && !(node.capture && node.capture->OwnsBuffer()) &&
                       node.topoFx == UsdGenTopoFx::None;
        if (node.topoFx != UsdGenTopoFx::None && pl.blend != 1.0f && hasUp)
            aggregated.Warn("usdGen:blend is ignored on the topology-changing operator '" +
                            node.desc->path.GetString() + "' (03 §8.5)");
        pl.planes = op.PlanesTouched();
        pl.didEval.assign(node.chunks.size(), 0);
        if (!node.chunks.empty()) {
            // Sparse fast path (E-2): a handful of dirty chunks must not pay
            // a full arena entry + parallel_for dispatch per node. At or
            // below 4 dirty chunks the commit thread runs the same
            // per-chunk body inline, in index order — bitwise identical
            // (E-8: chunks are independent; order never affected values).
            size_t dirtyCount = 0;
            if (!evalAll) {
                for (uint8_t b : node.chunkDirty)
                    if (b & UsdGenDirtyParameter) ++dirtyCount;
            }
            if (!evalAll && dirtyCount <= 4) {
                for (size_t i = 0; i < node.chunks.size(); ++i) {
                    if (node.chunkDirty[i] & UsdGenDirtyParameter)
                        SweepChunk(i, &pl);
                }
            } else {
                dispatcher.ParallelFor(node.chunks.size(), SweepChunk, &pl);
            }
        }

        // Post-sweep bookkeeping (commit thread; workers never touch dirty
        // bytes — no false sharing, 03 §5.4).
        bool wrote = reCaptured;
        for (uint8_t b : pl.didEval) if (b) { wrote = true; break; }
        if (wrote) {
            node.buffer.valueVersion += 1;
            for (size_t c = 0; c < node.chunks.size(); ++c) {
                if (pl.didEval[c] && node.chunks[c].tile < tileTouched.size())
                    tileTouched[node.chunks[c].tile] = 1;
            }
        }
        // Consumed bytes clear on the commit thread (03 §5.4 step 4: "the
        // commit thread then memsets node.dirty"). Without this, one dirty
        // commit pins every later commit full-dirty and no sparse commit can
        // ever be sparse (gate E-2). Workers never touch dirty bytes.
        std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                  UsdGenDirtyNone);
        {   // per-node timing (03 §9.2; feeds session NodeStats)
            auto const _nt2 = std::chrono::steady_clock::now();
            UsdGenNodeRunStats st;
            st.id = static_cast<UsdGenNodeId>(pos);
            st.captureMs = std::chrono::duration<double, std::milli>(_nt1 - _nt0).count();
            st.evalMs = std::chrono::duration<double, std::milli>(_nt2 - _nt1).count();
            st.chunksEvaluated = 0;
            for (uint8_t b : pl.didEval) if (b) ++st.chunksEvaluated;
            result.nodeStats.push_back(st);
        }
    }
    UsdGenCurveBuffer const &term = graph.Output();
    UsdGenCompiledNode const &tn = graph.Node(graph.TerminalNodeId());
    UsdGenOp const *termOp = tn.op.get();

    if (result.topologyChanged)
        std::fill(tileTouched.begin(), tileTouched.end(), 1);
    if (nTiles > 0) {
        InterleavePayload ip;
        ip.graph = &graph;
        ip.tn = &tn;
        ip.term = &term;
        ip.touched = &tileTouched;
        ip.widthsFlag = termOp &&
            (termOp->PlanesTouched() & UsdGenOp::kPlaneWidths) != 0;
        dispatcher.ParallelFor(size_t(nTiles), InterleaveTile, &ip);
    }

    result.terminalOutput = &graph.Output();
    result.tiles = graph.Tiles();
    result.diagnostics = std::move(aggregated);
    return result;
}

}  // namespace usdGen
