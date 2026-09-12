// usdGen — UsdGenGrowOp implementation (M1). 02-schema.md §2.6, 04 §2.2.
// Roots -> straight strands: sets the CV count (usdGen:segments), the length
// and the lift off the surface. Capture fixes the topology (cvCount) and the
// per-curve target length (kSaltGrow draw); Evaluate writes the CV
// positions and hairT. Direction: surfaceNormal (root frame N), attribute
// (M1: root frame T), vector (usdGen:directionVector).
#include "usdGen/ops/grow.h"

#include "usdGen/mask.h"
#include "usdGen/maskParams.h"
#include "usdGenMath/usdGenMath/hash.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

// usdGen:lengthRandom is a float2 (lo, hi); the property is stored as a
// GfVec2f or a len-2 VtFloatArray depending on the source (02 §2.6).
inline void ReadVec2Prop(UsdGenParamView const &p, TfToken const &name,
                         double &lo, double &hi)
{
    lo = 1.0; hi = 1.0;
    if (VtValue const v = p.GetVtValue(name, VtValue()); v.IsHolding<VtFloatArray>()) {
        VtFloatArray const a = v.UncheckedGet<VtFloatArray>();
        if (a.size() >= 2) { lo = a[0]; hi = a[1]; }
    } else if (v.IsHolding<GfVec2f>()) {
        GfVec2f const r = v.UncheckedGet<GfVec2f>();
        lo = r[0]; hi = r[1];
    }
}

inline uint64_t doubleAsBits(double d)
{
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

struct UsdGenGrowCapture final : public UsdGenCapturePayload
{
    // perCurve[c] = target length of curve c (kSaltGrow draw applied).
    int cvCount = 0;
    float lift = 0.0f;
    TfToken direction = TfToken("surfaceNormal");
    GfVec3f directionVector{0.0f, 1.0f, 0.0f};
    uint64_t upstreamTopologyVersion = 0;
    uint32_t upstreamCurves = 0;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenGrowCapture>(*this);
    }
    bool OwnsBuffer() const override { return true; }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.totalCurves == upstreamCurves;
    }
};

}  // namespace

static TfTokenVector _topoParams = [] {
    TfTokenVector v = UsdGenBaseTopologyParams();
    v.push_back(TfToken("enabled"));
    v.push_back(TfToken("segments"));
    v.push_back(TfToken("direction"));
    v.push_back(TfToken("length:source"));
    v.push_back(TfToken("length"));
    v.push_back(TfToken("lengthRandom"));
    v.push_back(TfToken("directionPrimvar"));
    auto m = UsdGenMaskTopologyParams();
    v.insert(v.end(), m.begin(), m.end());
    return v;
}();

static TfTokenVector _valueParams = [] {
    TfTokenVector v = UsdGenBaseValueParams();
    v.push_back(TfToken("directionVector"));
    v.push_back(TfToken("lift"));
    v.push_back(TfToken("uvBlend"));
    auto m = UsdGenMaskValueParams();
    v.insert(v.end(), m.begin(), m.end());
    return v;
}();

TfSpan<const TfToken> UsdGenGrowOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenGrowOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
std::unique_ptr<UsdGenCapture> UsdGenGrowOp::CreateCapture() const
{
    return std::make_unique<UsdGenGrowCapture>();
}
uint32_t UsdGenGrowOp::PlanesTouched() const
{
    return kPlanePoints | kPlaneHairT;
}

bool UsdGenGrowOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    int segments = params.GetInt(TfToken("segments"), 8);
    if (segments < 2 || segments > 64) {
        if (diag)
            diag->Error("UsdGenGrow: usdGen:segments must be in [2, 64]");
        return false;
    }
    TfToken dir = params.GetToken(TfToken("direction"), TfToken("surfaceNormal"));
    if (dir != TfToken("surfaceNormal") && dir != TfToken("attribute") &&
        dir != TfToken("vector")) {
        if (diag)
            diag->Error("UsdGenGrow: unknown usdGen:direction token '" +
                        dir.GetString() + "'");
        return false;
    }
    return true;
}

UsdGenEpoch UsdGenGrowOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    // Parameter-class inputs only; the upstream topology validity is checked
    // through ValidForTopology() at commit time (03 §3.4).
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    uint64_t h = 1469598103934665603ULL;
    auto feed = [&](std::string const &k, uint64_t v) {
        h ^= v; h *= 0x100000001b3ULL;
        h ^= k.size(); h *= 0x100000001b3ULL;
    };
    if (p) {
        feed("segments", uint64_t(p->GetInt(TfToken("segments"), 8)));
        feed("length", doubleAsBits(p->GetDouble(TfToken("length"), 1.0)));
        double lo = 1.0, hi = 1.0;
        ReadVec2Prop(*p, TfToken("lengthRandom"), lo, hi);
        feed("lengthRandomLo", doubleAsBits(lo));
        feed("lengthRandomHi", doubleAsBits(hi));
        feed("direction",
             uint64_t(p->GetToken(TfToken("direction"), TfToken("surfaceNormal")).Hash()));
        feed("seed", ctx.seed);
    }
    return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenGrowOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    TF_UNUSED(diag);
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    auto &cap = *static_cast<UsdGenGrowCapture *>(out);

    int segments = p ? p->GetInt(TfToken("segments"), 8) : 8;
    cap.cvCount = segments;
    cap.lift = p ? static_cast<float>(p->GetDouble(TfToken("lift"), 0.0)) : 0.0f;
    cap.direction = p ? p->GetToken(TfToken("direction"), TfToken("surfaceNormal"))
                      : TfToken("surfaceNormal");
    cap.directionVector = GfVec3f(0.0f, 1.0f, 0.0f);
    if (p) {
        if (VtValue const v = p->GetVtValue(TfToken("directionVector"), VtValue());
            v.IsHolding<GfVec3f>()) {
            cap.directionVector = v.UncheckedGet<GfVec3f>();
        }
    }
    double length = p ? p->GetDouble(TfToken("length"), 1.0) : 1.0;
    double lo = 1.0, hi = 1.0;
    if (p) ReadVec2Prop(*p, TfToken("lengthRandom"), lo, hi);
    if (lo > hi) std::swap(lo, hi);

    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamCurves = upstream.totalCurves;

    uint32_t const R = upstream.totalCurves;
    UsdGenCurveBuffer &buf = cap.MutableBuffer();
    buf.totalCurves = R;
    buf.totalCvs = R * uint32_t(cap.cvCount);
    buf.topologyVersion = upstream.topologyVersion + 1;

    // Per-curve target lengths: len = length * (lo + (hi-lo) * Draw01)
    // (plan/04 :700), the kSaltGrow draw over the stable id (04 §0.6),
    // lo/hi from usdGen:lengthRandom (02 §2.6, default (1,1)).
    if (R) {
        cap.perCurve.resize(R);
        auto const *ids = upstream.curveId.empty() ? nullptr : upstream.curveId.data();
        auto *len = cap.perCurve.data();
        for (uint32_t c = 0; c < R; ++c) {
            float const r = UsdGenDraw01(int(ctx.seed), ids ? ids[c] : 0,
                                         kSaltGrow);
            len[c] = static_cast<float>(length * (lo + r * (hi - lo)));
        }
    }

    // Mask block (02 §2.13) — resolved once per capture epoch (I4).
    if (p) {
        UsdGenMaskSettings m = UsdGenMaskSettingsFromParams(*p);
        auto const mask = EvaluateMask(m, upstream.curveId, upstream.px, {}, 0.0);
        cap.curveMask = mask.curveMask;
        cap.maskRampLut = mask.rampLut;
    }
    return true;
}

void UsdGenGrowOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &captureIn,
    UsdGenChunkView *view) const
{
    TF_UNUSED(ctx);
    UsdGenGrowCapture const &cap =
        static_cast<UsdGenGrowCapture const &>(captureIn);

    float const invSpan = view->cvCount > 1 ? 1.0f / float(view->cvCount - 1) : 0.0f;
    float *px = view->px;
    float *py = view->py;
    float *pz = view->pz;
    auto const *inPx = view->inPx;
    auto const *inPy = view->inPy;
    auto const *inPz = view->inPz;

    // Mask envelope (04 §5.2): w(c,i) = curveMask(c) * rampLUT(hairT), the
    // weight every §2 kernel writes. maskRampLut lives in the capture payload
    // (like noise); curveMask reaches the chunk via the scheduler's
    // per-chunk view pointer (scheduler.cpp:259-260).
    auto const *maskLut = cap.maskRampLut.empty() ? nullptr : cap.maskRampLut.data();
    for (uint32_t c = 0; c < view->curveCount; ++c) {
        // Root sits at the upstream (single-CV) position of this curve.
        uint32_t const r = c * view->inCvCount;
        float const rx = inPx[r], ry = inPy[r], rz = inPz[r];

        // Direction compares use operator-owned tokens: constructing a
        // TfToken("literal") per curve re-hashes + re-probes the global
        // token table 100k times per run (measured: most of the 7 ms
        // single-thread grow sweep).
        GfVec3f dir;
        if (cap.direction == sVector) {
            dir = cap.directionVector;
        } else if (cap.direction == sAttr) {
            dir = view->rootT ? view->rootT[c] : GfVec3f(0.0f, 1.0f, 0.0f);
        } else {  // surfaceNormal
            dir = view->rootN ? view->rootN[c] : GfVec3f(0.0f, 1.0f, 0.0f);
        }
        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        GfVec3f d = dl > 1e-12f ? dir / dl : GfVec3f(0.0f, 1.0f, 0.0f);
        const float s0 = cap.lift;
        // perCurve is a whole-buffer capture payload: chunk views pre-offset
        // the plane/per-curve arrays only, so index by absolute curve.
        const float targetLen = cap.perCurve.empty()
            ? 0.0f
            : cap.perCurve[view->desc->firstCurve + c];
        // Per-curve mask weight (04 §5.2); 1.0 when the mask block is inert.
        const float mWeight = view->curveMask ? view->curveMask[c] : 1.0f;

        for (uint32_t i = 0; i < view->cvCount; ++i) {
            const float t = i * invSpan;
            const float w = mWeight * (maskLut ? UsdGenEvalLut257(maskLut, t) : 1.0f);
            const float s = (s0 + targetLen * t) * w;
            uint32_t const o = view->Cv(c, i);
            px[o] = rx + d[0] * s;
            py[o] = ry + d[1] * s;
            pz[o] = rz + d[2] * s;
            if (view->hairT) view->hairT[o] = t;
        }
    }
}

}  // namespace usdGen
