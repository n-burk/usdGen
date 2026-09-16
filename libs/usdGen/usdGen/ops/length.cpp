// usdGen — UsdGenLengthOp implementation (M1). 02-schema.md §2.7.1, 04 §2.10.
// Set/scale/cull strand length. The WHOLE type is topology-bumping
// (TopologyEffect() == CurveCount, ADR §9 R14) because cull mode can change
// the curve count; with cullThreshold == 0 (the default) it behaves as a
// value-class re-position.
//
// M1 kernel: a per-curve scale FACTOR captured against the upstream rest
// length at capture time; Evaluate scales the current (deformed) curve about
// its root by that factor. "set" and "scale" both reduce to a factor
// (set: target/restLen, scale: value*lerp(lo,hi,draw) — plan/04 §2.10's
// `length:random` float2, drawn per curve with kSaltLength (plan/04 §0.6)).
// "cutExtend" is the same radial re-position on M1's straight chains; culling
// zero-lengthens a curve (stable ids; a true curve removal is M2's topology
// work, and R14 already routes usdGen:cullThreshold as topology).
#include "usdGen/ops/length.h"


#include "usdGen/opParams.h"
#include "usdGenMath/usdGenMath/hash.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

inline uint64_t doubleAsBits(double d)
{
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

// Reads a `float2` property — GfVec2f or 2-element VtFloatArray; lo/hi swapped
// when hi < lo. Returns false when absent/unparseable. (`length:random`,
// 02 §2.6 float2 — plan/04:1033.)
bool ReadVec2Prop(UsdGenParamView const *p, char const *name, double* lo, double* hi)
{
    if (!p)
        return false;
    VtValue v = p->GetVtValue(TfToken(name), VtValue());
    float x = 0.0f, y = 0.0f;
    if (v.IsHolding<GfVec2f>()) {
        const GfVec2f vec = v.UncheckedGet<GfVec2f>();
        x = vec[0]; y = vec[1];
    } else if (v.IsHolding<VtFloatArray>()) {
        const VtFloatArray& arr = v.UncheckedGet<VtFloatArray>();
        if (arr.size() != 2)
            return false;
        x = arr[0]; y = arr[1];
    } else {
        return false;
    }
    *lo = x;
    *hi = y;
    if (*hi < *lo)
        std::swap(*lo, *hi);
    return true;
}

struct UsdGenLengthCapture final : public UsdGenCapturePayload
{
    // perCurve[c] = lerp(length:random, draw01(seed, curveId, kSaltLength)) —
    // the position-independent part of §2.10's target formula. Rest length is
    // NOT sampled here: capture sees lazily-evaluated upstream buffers whose
    // chunk cvCount is not yet populated, so any restLen/target/restLen ratio
    // must be computed at Evaluate time from the chunk's real input planes.
    TfToken mode = TfToken("scale");
    double value = 1.0;
    double cullThreshold = 0.0;
    double minRemaining = 0.0;
    uint64_t upstreamTopologyVersion = 0;
    uint32_t upstreamCurves = 0;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenLengthCapture>(*this);
    }
    bool OwnsBuffer() const override { return false; }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.totalCurves == upstreamCurves;
    }
};


}  // namespace

static TfTokenVector _topoParams = [] {
    TfTokenVector v = UsdGenBaseTopologyParams();
    v.push_back(TfToken("enabled"));      // §6.2: Length is unconditionally topology
    v.push_back(TfToken("length:mode"));
    v.push_back(TfToken("length:method"));
    v.push_back(TfToken("rebuild"));
    v.push_back(TfToken("length:value"));
    v.push_back(TfToken("length:random"));
    v.push_back(TfToken("minRemainingLength"));
    v.push_back(TfToken("cullThreshold"));
    return v;
}();

static TfTokenVector _valueParams = [] {
    TfTokenVector v = UsdGenBaseValueParams();
    v.push_back(TfToken("mask"));        // operator envelope (02 §2.13)
    return v;
}();

TfSpan<const TfToken> UsdGenLengthOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenLengthOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
std::unique_ptr<UsdGenCapture> UsdGenLengthOp::CreateCapture() const
{
    return std::make_unique<UsdGenLengthCapture>();
}
uint32_t UsdGenLengthOp::PlanesTouched() const
{
    return kPlanePoints;
}

bool UsdGenLengthOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    TfToken mode = params.GetToken(TfToken("length:mode"), TfToken("scale"));
    TfToken method = params.GetToken(TfToken("length:method"), TfToken("scale"));
    if (mode != TfToken("set") && mode != TfToken("scale") && mode != TfToken("cull")) {
        if (diag)
            diag->Error("UsdGenLength: unknown usdGen:length:mode '" + mode.GetString() + "'");
        return false;
    }
    if (method != TfToken("scale") && method != TfToken("cutExtend")) {
        if (diag)
            diag->Error("UsdGenLength: unknown usdGen:length:method '" + method.GetString() + "'");
        return false;
    }
    return true;
}

UsdGenEpoch UsdGenLengthOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    uint64_t h = 1469598103934665603ULL;
    auto feed = [&](std::string const &k, uint64_t v) {
        h ^= v; h *= 0x100000001b3ULL;
        h ^= k.size(); h *= 0x100000001b3ULL;
    };
    if (p) {
        feed("mode", p->GetToken(TfToken("length:mode"), TfToken("scale")).Hash());
        feed("method", p->GetToken(TfToken("length:method"), TfToken("scale")).Hash());
        feed("value", doubleAsBits(p->GetDouble(TfToken("length:value"), 1.0)));
        feed("cull", doubleAsBits(p->GetDouble(TfToken("cullThreshold"), 0.0)));
        feed("minRemaining", doubleAsBits(p->GetDouble(TfToken("minRemainingLength"), 0.0)));
        double lo = 1.0, hi = 1.0;
        ReadVec2Prop(p, "length:random", &lo, &hi);   // M-5: the range is a capture input
        feed("lenRandomLo", doubleAsBits(lo));
        feed("lenRandomHi", doubleAsBits(hi));
        feed("seed", ctx.seed);
    }
    feed("upstream", ctx.upstreamGeneration);
    return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenLengthOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    TF_UNUSED(diag);
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    auto &cap = *static_cast<UsdGenLengthCapture *>(out);
    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamCurves = upstream.totalCurves;
    cap.mode = p ? p->GetToken(TfToken("length:mode"), TfToken("scale")) : TfToken("scale");
    double value = p ? p->GetDouble(TfToken("length:value"), 1.0) : 1.0;
    double lo = 1.0, hi = 1.0;
    ReadVec2Prop(p, "length:random", &lo, &hi);
    double cullThreshold = p ? p->GetDouble(TfToken("cullThreshold"), 0.0) : 0.0;
    double minRemaining = p ? p->GetDouble(TfToken("minRemainingLength"), 0.0) : 0.0;

    cap.value = value;
    cap.cullThreshold = cullThreshold;
    cap.minRemaining = minRemaining;

    // Capture draws only the seed-sensitive multiplier; lengths are a runtime
    // quantity (see UsdGenLengthCapture comment).
    const uint32_t R = upstream.totalCurves;
    cap.perCurve.resize(R);
    auto *mult = cap.perCurve.data();
    auto const *ids = upstream.curveId.empty() ? nullptr : upstream.curveId.data();
    for (uint32_t c = 0; c < R; ++c) {
        // lerp(lo, hi, draw01) — plan/04 §2.10 (`length:random` per-curve
        // multiplier), draw per plan/04 §0.6.
        const float r = UsdGenDraw01(int(ctx.seed), uint64_t(ids ? ids[c] : 0), kSaltLength);
        mult[c] = float(lo + (hi - lo) * double(r));
    }
    return true;
}

void UsdGenLengthOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &captureIn,
    UsdGenChunkView *view) const

{
    UsdGenLengthCapture const &cap =
        static_cast<UsdGenLengthCapture const &>(captureIn);
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    // usdGen:length:value, usdGen:cullThreshold, usdGen:minRemainingLength and
    // usdGen:mask are CONNECTABLE. The first three shape ONE per-curve scale
    // factor, so they are sampled at the curve's root CV; the mask is a true
    // per-CV envelope. Nothing connected leaves these as uniform literals and
    // the arithmetic below bit-identical to the pre-expression kernel.
    UsdGenParamField const valueField =
        p ? p->GetScalarField(sValue, cap.value) : UsdGenParamField{cap.value};
    UsdGenParamField const cullField =
        p ? p->GetScalarField(sCull, cap.cullThreshold) : UsdGenParamField{cap.cullThreshold};
    UsdGenParamField const minField =
        p ? p->GetScalarField(sMinRemaining, cap.minRemaining)
          : UsdGenParamField{cap.minRemaining};
    UsdGenParamField const maskField =
        p ? p->GetScalarField(sMask, 1.0) : UsdGenParamField{1.0};
    float *px = view->px, *py = view->py, *pz = view->pz;
    auto const *inPx = view->inPx, *inPy = view->inPy, *inPz = view->inPz;
    // perCurve is a whole-buffer capture payload (the scheduler pre-offsets
    // only buffer planes) — index it absolutely, grow.cpp pattern.
    auto const *mult = cap.perCurve.empty() ? nullptr : cap.perCurve.cdata();
    const size_t curveBase = view->desc ? view->desc->firstCurve : 0;
    const size_t cvBase = view->desc ? size_t(view->desc->firstCv) : 0;
    // sSet is operator-owned: no per-curve token interning and no lazy
    // static destruction while an asynchronous evaluation is still running.

    for (uint32_t c = 0; c < view->curveCount; ++c) {
        const size_t cv = view->cvCount ? size_t(view->cvCount)
            : (view->cvOffsets ? size_t(view->cvOffsets[c + 1] - view->cvOffsets[c]) : 0);
        const size_t base = view->cvCount ? size_t(c) * view->cvCount
            : (view->cvOffsets ? size_t(view->cvOffsets[c]) -
                (view->desc ? size_t(view->desc->firstCv) : 0) : 0);
        if (!cv) continue;
        const float rx = inPx[base], ry = inPy[base], rz = inPz[base];
        const double m = mult ? double(mult[curveBase + c]) : 1.0;
        // The per-curve controls sample the curve's root CV: one curve gets
        // one length factor, whatever granularity the expression declares.
        const size_t curve = curveBase + c;
        const size_t root = cvBase + base;
        const double value = valueField.Value(curve, root);
        const double cullThreshold = cullField.Value(curve, root);
        const double minRemaining = minField.Value(curve, root);
        float f;
        if (cap.mode == sSet) {
            // target = value * mult, absolute; scale it in over the runtime
            // rest length of THIS (possibly deformed) curve — plan/04 §2.10.
            const float restLen = UsdGenPolylineRestLength(inPx, inPy, inPz, base, cv);
            f = restLen > 1e-9f ? float(value * m / double(restLen)) : 0.0f;
        } else {  // "scale" and "cull": the ratio restLen cancels — plan/04 §2.10.
            f = float(value * m);
        }
        if (cullThreshold > 0.0 || minRemaining > 0.0) {
            const float restLen = UsdGenPolylineRestLength(inPx, inPy, inPz, base, cv);
            const double target = cap.mode == sSet
                                    ? value * m
                                    : double(restLen) * value * m;
            if (target < cullThreshold || target < minRemaining) f = 0.0f;
        }
        for (size_t i = 0; i < cv; ++i) {
            const size_t o = base + i;
            // usdGen:mask is the envelope (§2.13): the scale factor eases
            // toward identity (1.0), so mask == 0 is a bitwise pass-through.
            const float mask = std::clamp(
                static_cast<float>(maskField.Value(curve, cvBase + o)), 0.0f, 1.0f);
            const float fe = 1.0f + (f - 1.0f) * mask;
            if (fe == 1.0f) {
                px[o] = inPx[o]; py[o] = inPy[o]; pz[o] = inPz[o];
                continue;
            }
            px[o] = rx + (inPx[o] - rx) * fe;
            py[o] = ry + (inPy[o] - ry) * fe;
            pz[o] = rz + (inPz[o] - rz) * fe;
        }
    }
}

}  // namespace usdGen
