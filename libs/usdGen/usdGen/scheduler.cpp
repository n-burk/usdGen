// usdGen engine — scheduler implementation (03-execution-engine.md §5.3/§5.4).
//
// Run() executes engine-side commit steps in deterministic dependency-ready
// frontiers. Each frontier captures and repartitions serially, prepares all
// COW payloads serially, then sweeps its flattened node/chunk work in parallel:
//   - reference lane (03 §1.5): reference-role nodes run first, with
//     immutable resolved CurveSet/map values supplied to their capture;
//     this CPU slice does not implement chaining from a reference node's
//     produced geometry into another reference node;
//   - re-capture when the capture epoch moved (03 §3.4); generators install
//     their captured buffer and the graph re-partitions on topology change;
//   - per-frontier chunk-evaluate sweep, tbb::parallel_for inside the private
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
#include <array>
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

/// A non-owning styler's per-curve channels are immutable aliases of its
/// current input. Refresh them on every preparation: emptiness is not an
/// ownership bit, and a reused node may otherwise retain aliases from an old
/// upstream topology/stable-ID generation.
void AliasPerCurve(UsdGenCurveBuffer &dst, UsdGenCurveBuffer const &src)
{
    dst.curveId = src.curveId;
    dst.rootPrim = src.rootPrim;
    dst.rootUV = src.rootUV;
    dst.rootT = src.rootT;
    dst.rootN = src.rootN;
    dst.rootB = src.rootB;
    dst.curveMask = src.curveMask;
}

void AliasExtraPlanes(UsdGenCurveBuffer &dst, UsdGenCurveBuffer const &src)
{
    // Copying the vectors copies only plane descriptors and VtArray handles.
    // The arrays themselves remain immutable aliases until an output slot is
    // explicitly materialized below.
    dst.extraCv = src.extraCv;
    dst.extraCurve = src.extraCurve;
}

UsdGenPlane const *FindPlane(std::vector<UsdGenPlane> const &planes,
                             TfToken const &name)
{
    auto const it = std::lower_bound(planes.begin(), planes.end(), name,
        [](UsdGenPlane const &plane, TfToken const &needle) {
            return plane.name < needle;
        });
    return it != planes.end() && it->name == name ? &*it : nullptr;
}

struct PlaneLookup
{
    UsdGenPlane const *plane = nullptr;
    bool cv = false;
};

PlaneLookup FindExtraPlane(UsdGenCurveBuffer const &buffer, TfToken const &name)
{
    if (UsdGenPlane const *plane = FindPlane(buffer.extraCv, name))
        return {plane, true};
    if (UsdGenPlane const *plane = FindPlane(buffer.extraCurve, name))
        return {plane, false};
    return {};
}

bool SamePlaneLayout(UsdGenPlane const &a, UsdGenPlane const &b)
{
    return a.name == b.name && a.interpolation == b.interpolation &&
        a.type == b.type && a.arity == b.arity;
}

UsdGenPlane DefaultOutputPlane(TfToken const &name)
{
    // R24's fixed declarations cover first producers.  A later writer takes
    // its layout from the upstream plane instead, which also supports vertex
    // planes created by a generator/capture without teaching the scheduler a
    // second metadata vocabulary.
    UsdGenPlane plane;
    plane.name = name;
    plane.interpolation = TfToken("uniform");
    plane.type = TfToken("float");
    if (name == TfToken("guideIndex") || name.GetString().rfind("clumpId_", 0) == 0)
        plane.type = TfToken("int");
    if (name == TfToken("guideIndex") || name == TfToken("guideWeight"))
        plane.arity = 3;
    return plane;
}

void InsertOrAssignPlane(std::vector<UsdGenPlane> *planes, UsdGenPlane plane)
{
    auto it = std::lower_bound(planes->begin(), planes->end(), plane.name,
        [](UsdGenPlane const &candidate, TfToken const &needle) {
            return candidate.name < needle;
        });
    if (it != planes->end() && it->name == plane.name)
        *it = std::move(plane);
    else
        planes->insert(it, std::move(plane));
}

bool PlaneAliases(UsdGenPlane const &a, UsdGenPlane const *b)
{
    if (!b) return false;
    return a.type == TfToken("int") ? a.i.cdata() == b->i.cdata()
                                    : a.f.cdata() == b->f.cdata();
}

void PrepareExtraPlanes(UsdGenCompiledNode &node,
                        UsdGenCurveBuffer const &upstream, bool hasUpstream)
{
    if (!hasUpstream || node.op->IsGenerator()) return;

    UsdGenCurveBuffer &buffer = node.buffer;
    // Preserve prior private output arrays for sparse sweeps, but overwrite
    // every pass-through descriptor with the CURRENT source first.  That is
    // what prevents a retained node from keeping VtArray aliases from an old
    // topology generation.
    std::vector<UsdGenPlane> const oldCv = buffer.extraCv;
    std::vector<UsdGenPlane> const oldCurve = buffer.extraCurve;
    AliasExtraPlanes(buffer, upstream);

    for (TfToken const &name : node.outputPrimvars) {
        PlaneLookup const source = FindExtraPlane(upstream, name);
        PlaneLookup old;
        if (UsdGenPlane const *plane = FindPlane(oldCv, name)) old = {plane, true};
        else if (UsdGenPlane const *plane = FindPlane(oldCurve, name)) old = {plane, false};

        PlaneLookup const layout = source.plane ? source : old;
        bool const cv = layout.plane ? layout.cv : false;
        UsdGenPlane plane = layout.plane ? *layout.plane : DefaultOutputPlane(name);
        size_t const values = size_t(cv ? buffer.totalCvs : buffer.totalCurves) *
            std::max<uint8_t>(plane.arity, 1);

        // Retain only a same-layout private old output.  If it aliases the
        // upstream payload (or its topology-sized storage no longer fits), a
        // fresh VtArray is required before the chunk kernel receives a raw
        // writable pointer.
        bool const keepOld = old.plane && old.cv == cv &&
            SamePlaneLayout(*old.plane, plane) && !PlaneAliases(*old.plane, source.plane) &&
            (plane.type == TfToken("int") ? old.plane->i.size() == values
                                          : old.plane->f.size() == values);
        if (keepOld) plane = *old.plane;
        else if (plane.type == TfToken("int")) {
            plane.i = VtIntArray(values, 0);
            plane.f.clear();
        } else {
            plane.f = VtFloatArray(values, 0.0f);
            plane.i.clear();
        }
        InsertOrAssignPlane(cv ? &buffer.extraCv : &buffer.extraCurve,
                            std::move(plane));
    }
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
    bool ownsBuffer = false;

    // Inherit the topology FIRST: the CV count of the sizing below must be
    // the node's EFFECTIVE curve count, not its pre-inheritance 0 (an
    // un-populated styler buffer would otherwise size every touched plane
    // to zero and hand the kernel null plane pointers).
    if (hasUp && !op.IsGenerator()) {
        ownsBuffer = node.capture && node.capture->OwnsBuffer();
        if (!ownsBuffer) {
            buf.totalCurves = upBuf.totalCurves;
            buf.totalCvs = upBuf.totalCvs;
            buf.topologyVersion = upBuf.topologyVersion;
            AliasPerCurve(buf, upBuf);
        } else {
            if (buf.totalCurves == 0) {
                buf.totalCurves = upBuf.totalCurves;
                buf.totalCvs = upBuf.totalCvs;
                buf.topologyVersion = upBuf.topologyVersion;
            }
            InheritPerCurve(buf, upBuf);
        }
        if (!ownsBuffer) {
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
        // Rest is an immutable C3 transport channel, separate from current
        // points.  Every topology-preserving operator aliases it on each
        // preparation so a reused node cannot retain a prior generation's
        // owner.  Grow is the one current topology producer that explicitly
        // materializes a new rest layout during Capture.
        if (!(ownsBuffer && op.Type() == TfToken("UsdGenGrow")))
            buf.rest = upBuf.rest;
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
        } else if (!hasUp && op.IsGenerator()) {
            // Source generators own the presence semantics of optional
            // planes.  In particular Scatter has no input width plane;
            // materializing a zero array here would make downstream Grow
            // resample an invented authored width instead of its description
            // fallback.  Capture-created Source planes remain untouched.
        } else if (a.size() != nCvs) {
            a = VtFloatArray(nCvs, 0.0f);
        }
    };
    prep(planes & UsdGenOp::kPlanePoints, buf.px, upBuf.px);
    prep(planes & UsdGenOp::kPlanePoints, buf.py, upBuf.py);
    prep(planes & UsdGenOp::kPlanePoints, buf.pz, upBuf.pz);
    prep(planes & UsdGenOp::kPlaneWidths, buf.width, upBuf.width);
    prep(planes & UsdGenOp::kPlaneHairT, buf.hairT, upBuf.hairT);
    // Grow's capture has already transformed every inherited named plane for
    // its new CV cardinality. Do not replace those private owners with the
    // old upstream descriptors during generic pass-through preparation.
    bool const growOwnsTransformedPlanes = hasUp && ownsBuffer &&
        op.Type() == TfToken("UsdGenGrow");
    if (!growOwnsTransformedPlanes)
        PrepareExtraPlanes(node, upBuf, hasUp);
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

UsdGenEpoch WithExternalValueIdentity(UsdGenEpoch digest,
                                      UsdGenCompiledNode const &node,
                                      UsdGenGraph const &graph)
{
    auto mix = [&digest](uint64_t value) {
        digest[0] ^= value;
        digest[0] *= 0x100000001b3ULL;
        digest[1] ^= ~value;
        digest[1] *= 0x100000001b3ULL;
    };
    TfSpan<const UsdGenResolvedReferenceValue> const references = graph.ReferenceValues();
    for (uint32_t index : node.referenceValues) {
        if (index >= references.size()) { mix(UINT64_MAX); continue; }
        mix(references[index].identity);
        mix(references[index].curveGeneration);
    }
    TfSpan<const UsdGenResolvedMapValue> const maps = graph.MapValues();
    for (uint32_t index : node.mapValues) {
        if (index >= maps.size()) { mix(UINT64_MAX - 1); continue; }
        mix(maps[index].identity);
        mix(maps[index].textureGeneration);
    }
    return digest;
}
struct NodeSweepPayload
{
    UsdGenCompiledNode *node;
    UsdGenGraph const *graph;
    UsdGenEvalContext ctx;          // node-local copy (params/seed already set)
    UsdGenOp const *op;
    UsdGenCompiledNode const *up;  // null when the node has no input
    UsdGenCompiledNode const *up2; // ordered second input for binary kernels
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

// A prepared node job owns the small vectors whose addresses are published in
// its EvalContext.  Preparation happens on the commit thread; arena workers
// only consume the immutable context and write this node's disjoint chunks.
struct NodeExecution
{
    std::vector<UsdGenCurveBuffer const *> upstreamInputs;
    std::vector<UsdGenReferenceSet const *> referenceSets;
    std::vector<UsdGenResolvedReferenceValue const *> resolvedReferences;
    std::vector<UsdGenResolvedMapValue const *> resolvedMaps;
    NodeSweepPayload sweep{};
    bool reCaptured = false;
    bool shouldSweep = false;
    bool evalAll = false;
    bool anyChunkDirty = false;
    std::chrono::steady_clock::time_point captureStart;
    std::chrono::steady_clock::time_point captureEnd;
    std::chrono::steady_clock::time_point evaluationStart;
};

struct ChunkExecution
{
    NodeSweepPayload *payload = nullptr;
    size_t chunk = 0;
};

void SweepChunk(size_t index, void *payload);

void SweepPreparedChunk(size_t index, void *payload)
{
    auto const &work = static_cast<ChunkExecution *>(payload)[index];
    SweepChunk(work.chunk, work.payload);
}

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
    UsdGenCurveBuffer const &upBuf2 = pl.up2 ? pl.up2->buffer : EmptyBuffer();
    UsdGenChunkDesc const *upC2 = (pl.up2 && pl.up2->chunks.size() == node.chunks.size())
        ? &pl.up2->chunks[index] : nullptr;

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
    view.inFirstCv = static_cast<uint32_t>(upBase);
    view.inCvOffsets = (upC && upC->cvCount == 0 && !upBuf.cvOffsets.empty() &&
                        size_t(upC->firstCurve) < upBuf.cvOffsets.size())
        ? upBuf.cvOffsets.cdata() + upC->firstCurve : nullptr;
    view.outCount = static_cast<uint32_t>(node.outputPrimvars.size());
    view.inCount = static_cast<uint32_t>(node.inputPrimvars.size());
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
    view.inWidth2 = upC2 && !upBuf2.width.empty()
        ? upBuf2.width.cdata() + upC2->firstCv : nullptr;
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

    // Extra planes use fixed stack tables: Evaluate() remains allocation-free
    // even when an operator exposes several typed slots.  Slots are resolved
    // from the freshly prepared buffer, rather than cached pointers, so a
    // retained node cannot write a VtArray from a stale topology revision.
    std::array<float *, kUsdGenMaxExtraPlaneSlots> outF{};
    std::array<int *, kUsdGenMaxExtraPlaneSlots> outI{};
    std::array<float const *, kUsdGenMaxExtraPlaneSlots> inF{};
    std::array<int const *, kUsdGenMaxExtraPlaneSlots> inI{};
    std::array<float const *, kUsdGenMaxExtraPlaneSlots> outputInF{};
    std::array<int const *, kUsdGenMaxExtraPlaneSlots> outputInI{};
    std::array<size_t, kUsdGenMaxExtraPlaneSlots> outputValues{};
    auto offsetFor = [&](bool cv, bool upstreamPlane, uint8_t arity) {
        size_t const element = cv
            ? (upstreamPlane ? upBase : base)
            : (upstreamPlane && upC ? upC->firstCurve : cd.firstCurve);
        return element * std::max<uint8_t>(arity, 1);
    };
    for (uint32_t slot = 0; slot < view.outCount; ++slot) {
        PlaneLookup const output = FindExtraPlane(buf, node.outputPrimvars[slot]);
        if (!output.plane) continue;  // compiler/preparation is fail-closed to a null port
        size_t const offset = offsetFor(output.cv, false, output.plane->arity);
        outputValues[slot] = size_t(output.cv ? nCvs : cd.curveCount) *
            std::max<uint8_t>(output.plane->arity, 1);
        if (output.plane->type == TfToken("int")) {
            outI[slot] = output.plane->i.empty() ? nullptr
                : const_cast<int *>(output.plane->i.cdata()) + offset;
        } else {
            outF[slot] = output.plane->f.empty() ? nullptr
                : const_cast<float *>(output.plane->f.cdata()) + offset;
        }
        // Envelope endpoints operate on the logical predecessor of the same
        // output name, not on InputPrimvars slot position (the two lists may
        // legitimately differ).
        PlaneLookup const predecessor = FindExtraPlane(upBuf, node.outputPrimvars[slot]);
        if (!predecessor.plane || predecessor.cv != output.cv ||
            !SamePlaneLayout(*predecessor.plane, *output.plane)) continue;
        size_t const inOffset = offsetFor(predecessor.cv, true, predecessor.plane->arity);
        if (predecessor.plane->type == TfToken("int"))
            outputInI[slot] = predecessor.plane->i.empty() ? nullptr
                : predecessor.plane->i.cdata() + inOffset;
        else
            outputInF[slot] = predecessor.plane->f.empty() ? nullptr
                : predecessor.plane->f.cdata() + inOffset;
    }
    for (uint32_t slot = 0; slot < view.inCount; ++slot) {
        PlaneLookup const input = FindExtraPlane(upBuf, node.inputPrimvars[slot]);
        if (!input.plane) continue;
        size_t const offset = offsetFor(input.cv, true, input.plane->arity);
        if (input.plane->type == TfToken("int"))
            inI[slot] = input.plane->i.empty() ? nullptr : input.plane->i.cdata() + offset;
        else
            inF[slot] = input.plane->f.empty() ? nullptr : input.plane->f.cdata() + offset;
    }
    view.outF = view.outCount ? outF.data() : nullptr;
    view.outI = view.outCount ? outI.data() : nullptr;
    view.inF = view.inCount ? inF.data() : nullptr;
    view.inI = view.inCount ? inI.data() : nullptr;

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
        for (uint32_t s = 0; s < view.outCount; ++s) {
            if (outF[s] && outputInF[s])
                std::copy_n(outputInF[s], outputValues[s], outF[s]);
            if (outI[s] && outputInI[s])
                std::copy_n(outputInI[s], outputValues[s], outI[s]);
        }
    } else if (blendable && pl.blend > 0.0f && pl.blend < 1.0f) {
        if ((pl.planes & UsdGenOp::kPlanePoints) && view.px && inPx && inPy && inPz)
            UsdGenBlendEnvelopeVec3(inPx, inPy, inPz, view.px, view.py, view.pz,
                                    pl.blend, nCvs);
        if ((pl.planes & UsdGenOp::kPlaneWidths) && view.width && inW)
            UsdGenBlendEnvelope(inW, view.width, pl.blend, nCvs);
        if ((pl.planes & UsdGenOp::kPlaneHairT) && view.hairT && inT)
            UsdGenBlendEnvelope(inT, view.hairT, pl.blend, nCvs);
        for (uint32_t s = 0; s < view.outCount; ++s)
            if (outF[s] && outputInF[s])
                UsdGenBlendEnvelope(outputInF[s], outF[s], pl.blend,
                                    outputValues[s]);
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

    // Each frontier is dependency-ready and therefore may share one flattened
    // chunk sweep. Capture, COW preparation, repartition and bookkeeping stay
    // on this commit thread; arena workers only execute SweepChunk.
    std::vector<char> completed(static_cast<size_t>(graph.NodeCount()), 0);
    auto prepareNode = [&](int pos, NodeExecution *job) -> bool {
        UsdGenCompiledNode &node = graph.Node(pos);
        UsdGenOp &op = *node.op;
        UsdGenDiagnostics nodeDiag;
        bool const hasUp = !node.inputs.empty();
        UsdGenCurveBuffer const &upBuf = hasUp
            ? graph.Node(node.inputs.front()).buffer : emptyBuf;
        job->upstreamInputs.reserve(node.inputs.size());
        for (UsdGenNodeId input : node.inputs) {
            if (input >= static_cast<UsdGenNodeId>(graph.NodeCount())) {
                nodeDiag.Error("UsdGen: invalid compiled geometry input on '" +
                               node.desc->path.GetString() + "'");
                break;
            }
            job->upstreamInputs.push_back(&graph.Node(input).buffer);
        }
        job->captureStart = std::chrono::steady_clock::now();

        TfSpan<const UsdGenResolvedReferenceValue> const graphReferences =
            graph.ReferenceValues();
        TfSpan<const UsdGenResolvedMapValue> const graphMaps = graph.MapValues();
        job->referenceSets.reserve(node.referenceValues.size());
        job->resolvedReferences.reserve(node.referenceValues.size());
        for (uint32_t value : node.referenceValues) {
            if (value >= graphReferences.size() || !graphReferences[value].value) {
                nodeDiag.Error("UsdGen: missing compiled reference value on '" +
                               node.desc->path.GetString() + "'");
                break;
            }
            job->resolvedReferences.push_back(&graphReferences[value]);
            job->referenceSets.push_back(graphReferences[value].value.get());
        }
        job->resolvedMaps.reserve(node.mapValues.size());
        for (uint32_t value : node.mapValues) {
            if (value >= graphMaps.size()) {
                nodeDiag.Error("UsdGen: missing compiled map value on '" +
                               node.desc->path.GetString() + "'");
                break;
            }
            job->resolvedMaps.push_back(&graphMaps[value]);
        }

        UsdGenCaptureContext cctx;
        cctx.desc = &graph.Desc();
        cctx.params = &node.paramView;
        cctx.references = job->referenceSets.empty() ? nullptr
            : job->referenceSets.data();
        cctx.resolvedReferences = job->resolvedReferences.empty() ? nullptr
            : job->resolvedReferences.data();
        cctx.referenceCount = static_cast<uint32_t>(job->resolvedReferences.size());
        cctx.maps = job->resolvedMaps.empty() ? nullptr : job->resolvedMaps.data();
        cctx.mapCount = static_cast<uint32_t>(job->resolvedMaps.size());
        cctx.mapBindings = node.mapBindingRefs.empty() ? nullptr
            : node.mapBindingRefs.data();
        cctx.mapBindingCount = static_cast<uint32_t>(node.mapBindingRefs.size());
        cctx.surface = node.hasSurface ? node.surface : 0;
        cctx.readPhase = node.readPhase;
        cctx.seed = node.desc ? static_cast<uint32_t>(node.desc->seed) : 0;
        cctx.upstreamGeneration = hasUp ? upBuf.topologyVersion : 0;
        cctx.upstreams = job->upstreamInputs.empty() ? nullptr
            : job->upstreamInputs.data();
        cctx.upstreamCount = static_cast<uint32_t>(job->upstreamInputs.size());
        cctx.dispatcher = &dispatcher;
        cctx.diag = &nodeDiag;

        UsdGenEpoch const captureIdentity =
            WithExternalValueIdentity(op.CaptureDigest(cctx), node, graph);
        job->reCaptured = node.captureNeeded || !node.capture ||
            captureIdentity != node.captureEpoch ||
            !node.capture->ValidForTopology(upBuf);
        if (job->reCaptured) {
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
            node.captureEpoch = captureIdentity;
            node.captureNeeded = false;

            if (node.op->IsGenerator() && node.buffer.totalCurves > 0) {
                int const total = static_cast<int>(node.buffer.totalCurves);
                int const cvp = int(node.buffer.totalCvs /
                                    std::max<uint32_t>(1, node.buffer.totalCurves));
                int const nChunks = ComputeNumChunks(total, graph.ChunkSize());
                bool const layoutMoved =
                    int(node.chunks.size()) != nChunks ||
                    (node.chunks.empty() ? false :
                     int(node.chunks[0].cvCount) != cvp) ||
                    !node.buffer.cvOffsets.empty();
                if (layoutMoved && graph.Repartition(total, cvp))
                    result.topologyChanged = true;
            } else if (job->reCaptured && node.chunks.empty() && hasUp) {
                UsdGenCompiledNode const &upN = graph.Node(node.input);
                if (!upN.chunks.empty() && upN.buffer.totalCurves > 0 &&
                    graph.Repartition(static_cast<int>(upN.buffer.totalCurves),
                                      static_cast<int>(upN.chunks[0].cvCount)))
                    result.topologyChanged = true;
            }
        }
        for (auto const &e : nodeDiag.errors) aggregated.Error(e);
        for (auto const &w : nodeDiag.warnings) aggregated.Warn(w);
        if (aggregated.HasErrors()) return false;

        if (job->reCaptured && node.capture) {
            std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                      static_cast<uint8_t>(UsdGenDirtyParameter));
            for (UsdGenNodeId d : node.descendants) {
                if (d >= static_cast<UsdGenNodeId>(graph.NodeCount())) continue;
                std::fill(graph.Node(d).chunkDirty.begin(),
                          graph.Node(d).chunkDirty.end(),
                          static_cast<uint8_t>(UsdGenDirtyParameter));
            }
        }
        job->captureEnd = std::chrono::steady_clock::now();
        job->evalAll = node.paramValueDigest != node.lastParamDigest;
        node.lastParamDigest = node.paramValueDigest;
        for (uint8_t b : node.chunkDirty)
            job->anyChunkDirty |= (b & UsdGenDirtyParameter) != 0;

        return true;
    };

    // This is deliberately separate from capture.  Capture may call
    // graph.Repartition(), which updates every node's chunks; no node may
    // publish a prepared chunk payload until all captures in this frontier
    // have completed.
    auto prepareEvaluation = [&](int pos, NodeExecution *job) -> bool {
        UsdGenCompiledNode &node = graph.Node(pos);
        UsdGenOp &op = *node.op;
        bool const hasUp = !node.inputs.empty();
        UsdGenCurveBuffer const &upBuf = hasUp
            ? graph.Node(node.inputs.front()).buffer : emptyBuf;

        if (!node.enabled && node.topoFx == UsdGenTopoFx::None && hasUp) {
            UsdGenCurveBuffer &buf = node.buffer;
            buf.px = upBuf.px; buf.py = upBuf.py; buf.pz = upBuf.pz;
            buf.rest = upBuf.rest;
            buf.width = upBuf.width; buf.hairT = upBuf.hairT;
            AliasPerCurve(buf, upBuf);
            AliasExtraPlanes(buf, upBuf);
            buf.totalCurves = upBuf.totalCurves;
            buf.totalCvs = upBuf.totalCvs;
            buf.topologyVersion = upBuf.topologyVersion;
            std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                      UsdGenDirtyNone);
            return true;
        }
        if ((!job->anyChunkDirty && !job->evalAll) || !node.capture) {
            std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                      UsdGenDirtyNone);
            return true;
        }

        PrepareNodeForEval(node, op, upBuf, hasUp);
        EnsureNoDetaches(node, op);
        NodeSweepPayload &pl = job->sweep;
        pl.node = &node;
        pl.graph = &graph;
        pl.ctx = evalCtx;
        pl.ctx.params = &node.paramView;
        pl.ctx.desc = &graph.Desc();
        pl.ctx.references = job->referenceSets.empty() ? nullptr
            : job->referenceSets.data();
        pl.ctx.resolvedReferences = job->resolvedReferences.empty() ? nullptr
            : job->resolvedReferences.data();
        pl.ctx.referenceCount = static_cast<uint32_t>(job->resolvedReferences.size());
        pl.ctx.maps = job->resolvedMaps.empty() ? nullptr : job->resolvedMaps.data();
        pl.ctx.mapCount = static_cast<uint32_t>(job->resolvedMaps.size());
        pl.ctx.mapBindings = node.mapBindingRefs.empty() ? nullptr
            : node.mapBindingRefs.data();
        pl.ctx.mapBindingCount = static_cast<uint32_t>(node.mapBindingRefs.size());
        pl.ctx.seed = node.desc ? static_cast<uint32_t>(node.desc->seed) : 0;
        pl.op = &op;
        pl.up = hasUp ? &graph.Node(node.input) : nullptr;
        pl.up2 = node.inputs.size() > 1 ? &graph.Node(node.inputs[1]) : nullptr;
        pl.evalAll = job->evalAll;
        pl.blend = node.desc ? static_cast<float>(node.desc->blend) : 1.0f;
        pl.blendable = hasUp && op.UsesFrameworkBlendEnvelope() &&
                       !(node.capture && node.capture->OwnsBuffer()) &&
                       node.topoFx == UsdGenTopoFx::None;
        if (node.topoFx != UsdGenTopoFx::None && pl.blend != 1.0f && hasUp)
            aggregated.Warn("usdGen:blend is ignored on the topology-changing operator '" +
                            node.desc->path.GetString() + "' (03 §8.5)");
        pl.planes = op.PlanesTouched();
        pl.didEval.assign(node.chunks.size(), 0);
        job->shouldSweep = true;
        return true;
    };

    for (int lane = 0; lane < 2; ++lane) {
        bool const referenceLane = lane == 0;
        int remaining = 0;
        for (int pos = 0; pos < graph.NodeCount(); ++pos)
            remaining += ((graph.Node(pos).role == UsdGenRole::Reference) ==
                          referenceLane);
        while (remaining > 0) {
            std::vector<int> frontier;
            for (int pos = 0; pos < graph.NodeCount(); ++pos) {
                UsdGenCompiledNode const &node = graph.Node(pos);
                if (completed[size_t(pos)] ||
                    ((node.role == UsdGenRole::Reference) != referenceLane))
                    continue;
                bool ready = true;
                for (UsdGenNodeId input : node.inputs) {
                    if (input >= static_cast<UsdGenNodeId>(graph.NodeCount()) ||
                        !completed[size_t(input)]) {
                        ready = false;
                        break;
                    }
                }
                if (ready) frontier.push_back(pos);
            }
            if (frontier.empty()) {
                aggregated.Error(referenceLane
                    ? "UsdGen: reference dependency frontier stalled"
                    : "UsdGen: geometry dependency frontier stalled");
                result.diagnostics = std::move(aggregated);
                return result;
            }

            std::vector<std::unique_ptr<NodeExecution>> jobs;
            jobs.reserve(frontier.size());
            for (int pos : frontier) {
                jobs.push_back(std::make_unique<NodeExecution>());
                if (!prepareNode(pos, jobs.back().get())) {
                    result.diagnostics = std::move(aggregated);
                    return result;
                }
            }

            // Prepare every node only after every capture/repartition above
            // has settled the graph-wide chunk layout.
            for (size_t j = 0; j < frontier.size(); ++j) {
                if (!prepareEvaluation(frontier[j], jobs[j].get())) {
                    result.diagnostics = std::move(aggregated);
                    return result;
                }
            }

            std::vector<ChunkExecution> work;
            size_t sweepJobs = 0;
            for (auto const &job : jobs) {
                if (!job->shouldSweep) continue;
                ++sweepJobs;
                for (size_t c = 0; c < job->sweep.node->chunks.size(); ++c) {
                    if (job->evalAll ||
                        (job->sweep.node->chunkDirty[c] & UsdGenDirtyParameter))
                        work.push_back({&job->sweep, c});
                }
            }
            if (!work.empty()) {
                for (auto const &job : jobs)
                    if (job->shouldSweep)
                        job->evaluationStart = std::chrono::steady_clock::now();
                // Preserve the small sparse-node fast path. A frontier with
                // multiple ready nodes intentionally goes through one
                // flattened dispatch, even when each node has only a handful
                // of dirty chunks, so independent branches can overlap.
                if (sweepJobs == 1 && work.size() <= 4) {
                    for (ChunkExecution const &item : work)
                        SweepChunk(item.chunk, item.payload);
                } else {
                    dispatcher.ParallelFor(work.size(), SweepPreparedChunk,
                                           work.data());
                }
            }

            for (size_t j = 0; j < jobs.size(); ++j) {
                NodeExecution &job = *jobs[j];
                UsdGenCompiledNode &node = graph.Node(frontier[j]);
                if (job.shouldSweep) {
                    bool wrote = job.reCaptured;
                    for (uint8_t b : job.sweep.didEval)
                        if (b) { wrote = true; break; }
                    if (wrote) {
                        node.buffer.valueVersion += 1;
                        for (size_t c = 0; c < node.chunks.size(); ++c)
                            if (job.sweep.didEval[c] &&
                                node.chunks[c].tile < tileTouched.size())
                                tileTouched[node.chunks[c].tile] = 1;
                    }
                    std::fill(node.chunkDirty.begin(), node.chunkDirty.end(),
                              UsdGenDirtyNone);
                    auto const end = std::chrono::steady_clock::now();
                    UsdGenNodeRunStats st;
                    st.id = static_cast<UsdGenNodeId>(frontier[j]);
                    st.captureMs = std::chrono::duration<double, std::milli>(
                        job.captureEnd - job.captureStart).count();
                    st.evalMs = std::chrono::duration<double, std::milli>(
                        end - job.evaluationStart).count();
                    for (uint8_t b : job.sweep.didEval) if (b) ++st.chunksEvaluated;
                    result.nodeStats.push_back(st);
                }
                completed[size_t(frontier[j])] = 1;
                --remaining;
            }
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
