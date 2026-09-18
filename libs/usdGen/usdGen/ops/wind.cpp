// usdGen — UsdGenWindOp implementation. 02-schema.md §2.8, 04 §4.
// Time-dependent force field (see ops/wind.h for the full contract):
// constant deflection plus a per-strand SeExpr FBM gust that drifts with the
// cook time, resisted by a groom-wide stiffness and its hairT ramp, with the
// deflection scaled by hairT so roots never move.
//
// Capture pins the time-independent per-curve rest roots (opUtil::RestPoint:
// the rest plane when complete, else the current positions) and validates
// every literal and connected value fail-closed. Evaluate re-samples the gust
// every frame from the live (time, seed): the readsTime/rebuild split —
// time-dependent state is never pinned, so gusts animate yet stay
// deterministic per time + seed, and no time term enters the capture digest.
// gustStrength 0 skips the FBM entirely (pure constant deflection).
// direction/constStrength/gustStrength sample per curve root (primitive);
// stiffness and its ramp are groom-wide; the mask envelopes per CV.
#include "usdGen/ops/wind.h"

#include "usdGen/opParams.h"
#include "usdGen/ops/opUtil.h"
#include "usdGenMath/usdGenMath/hash.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "usdGenMath/usdGenMath/ramp.h"
#include "SeExpr2/Noise.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

// File-local salt ("Wind"); hash.h owns the shared per-operator table and is
// outside this kernel's write set, so the parent may promote this there.
constexpr uint32_t kSaltWind = 0x57696E64u;

// Fixed gust FBM shape (Wind declares no noise-shaping parameters):
// 3 octaves, lacunarity 2.0, gain 0.5 over rest-root space with a
// seconds-driven drift along x.
constexpr float kGustSpaceFreq = 0.5f;
constexpr float kGustTimeRate = 0.5f;
constexpr int kGustOctaves = 3;
constexpr float kGustLacunarity = 2.0f;
constexpr float kGustGain = 0.5f;

struct UsdGenWindCapture final : public UsdGenCapturePayload
{
    VtVec3fArray rootRest;   // per-curve rest root, pinned at capture
    uint64_t upstreamTopologyVersion = 0;
    uint64_t upstreamValueVersion = 0;
    uint32_t upstreamCurves = 0;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenWindCapture>(*this);
    }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        // Rest roots track the current positions when the rest plane is
        // incomplete, so value changes invalidate the pin (Direction precedent).
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.valueVersion == upstreamValueVersion &&
               upstream.totalCurves == upstreamCurves;
    }
};

bool Finite(double v) { return std::isfinite(v); }

bool FiniteVec3(GfVec3f const &v)
{
    return Finite(v[0]) && Finite(v[1]) && Finite(v[2]);
}

/// Read a float2[] ramp-knots property (02 §2.17 scalar ramp). Missing or
/// differently typed values fall back to the caller's default.
VtVec2fArray ReadRampKnots(UsdGenParamView const *p, TfToken const &name)
{
    VtVec2fArray knots;
    if (p) {
        if (VtValue const v = p->GetVtValue(name, VtValue());
            v.IsHolding<VtVec2fArray>()) {
            knots = v.UncheckedGet<VtVec2fArray>();
        }
    }
    return knots;
}

/// Per-curve hash vector h_c, mirroring UsdGenNoiseOp: three [0,1) draws over
/// the stable id with distinct salts.
void HashVec3(uint32_t seed, uint64_t curveId, float out[3])
{
    out[0] = UsdGenDraw01(int(seed), curveId, kSaltWind + 0u);
    out[1] = UsdGenDraw01(int(seed), curveId, kSaltWind + 1u);
    out[2] = UsdGenDraw01(int(seed), curveId, kSaltWind + 2u);
}

}  // namespace

static TfTokenVector _topoParams = [] {
    return UsdGenBaseTopologyParams();
}();

static TfTokenVector _valueParams = [] {
    TfTokenVector v = UsdGenBaseValueParams();
    v.push_back(TfToken("enabled"));      // toggle: value-class (02 §6.3)
    v.push_back(TfToken("direction"));
    v.push_back(TfToken("constStrength"));
    v.push_back(TfToken("gustStrength"));
    v.push_back(TfToken("stiffness"));
    v.push_back(TfToken("stiffness:knots"));
    v.push_back(TfToken("stiffness:interpolation"));
    v.push_back(TfToken("mask"));         // operator envelope (02 §2.13)
    return v;
}();

TfSpan<const TfToken> UsdGenWindOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenWindOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
std::unique_ptr<UsdGenCapture> UsdGenWindOp::CreateCapture() const
{
    return std::make_unique<UsdGenWindCapture>();
}
uint32_t UsdGenWindOp::PlanesTouched() const
{
    return kPlanePoints;
}

bool UsdGenWindOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    VtValue const direction = params.GetVtValue(sDirection, VtValue());
    if (!direction.IsEmpty() && !direction.IsHolding<GfVec3f>()) {
        if (diag) diag->Error("UsdGenWind: usdGen:direction must be vector3f");
        return false;
    }
    if (direction.IsHolding<GfVec3f>()) {
        GfVec3f const v = direction.UncheckedGet<GfVec3f>();
        if (!FiniteVec3(v)) {
            if (diag) diag->Error("UsdGenWind: usdGen:direction must be finite");
            return false;
        }
        if (v.GetLength() <= 1e-12f) {
            if (diag) diag->Error("UsdGenWind: usdGen:direction must be nonzero");
            return false;
        }
    }
    for (TfToken const *name : {&sConst, &sGust, &sStiffness}) {
        double const v = params.GetDoubleLiteral(*name, 0.0);
        if (!Finite(v)) {
            if (diag) diag->Error("UsdGenWind: usdGen:" + name->GetString() +
                                  " must be finite");
            return false;
        }
    }
    VtValue const knots = params.GetVtValue(sKnots, VtValue());
    if (!knots.IsEmpty() && !knots.IsHolding<VtVec2fArray>()) {
        if (diag) diag->Error("UsdGenWind: usdGen:stiffness:knots must be float2[]");
        return false;
    }
    if (knots.IsHolding<VtVec2fArray>()) {
        for (GfVec2f const &knot : knots.UncheckedGet<VtVec2fArray>()) {
            if (!Finite(knot[0]) || !Finite(knot[1])) {
                if (diag) diag->Error("UsdGenWind: usdGen:stiffness:knots must be finite");
                return false;
            }
        }
    }
    TfToken const interp = params.GetToken(sInterp, sCatmullRom);
    if (interp != sLinear && interp != sCatmullRom && interp != sBspline &&
        interp != sConstant) {
        if (diag)
            diag->Error("UsdGenWind: unknown usdGen:stiffness:interpolation '" +
                        interp.GetString() + "'");
        return false;
    }
    return true;
}

UsdGenEpoch UsdGenWindOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    // The pinned rest roots are a function of the upstream topology/values
    // only. Every Wind parameter is value-class and the gust time is a
    // per-frame Evaluate input, so neither enters here: value edits and time
    // steps sweep without recapturing (upstream value motion re-captures
    // through ValidForTopology instead).
    uint64_t h = 1469598103934665603ULL;
    h ^= ctx.upstreamGeneration; h *= 0x100000001b3ULL;
    h ^= uint64_t(ctx.seed); h *= 0x100000001b3ULL;
    return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenWindOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    auto &cap = *static_cast<UsdGenWindCapture *>(out);
    auto fail = [&](std::string const &message) {
        if (diag) diag->Error("UsdGenWind: " + message);
        return false;
    };
    // Literals are validated here (the CPU lane never calls Bind) and every
    // connected value is scanned, so a bad expression fails closed at capture
    // instead of writing NaN points per frame.
    if (p && !Bind(*p, diag)) return false;
    std::vector<uint32_t> spans;
    std::string spansError;
    if (!opUtil::CurveSpans(upstream, &spans, &spansError))
        return fail("upstream CV topology " + spansError);
    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamValueVersion = upstream.valueVersion;
    cap.upstreamCurves = upstream.totalCurves;
    cap.rootRest.clear();
    if (!p) {
        cap.rootRest.resize(upstream.totalCurves);
        for (uint32_t c = 0; c != upstream.totalCurves; ++c)
            cap.rootRest[c] = opUtil::RestPoint(upstream, spans[c]);
        return true;
    }
    // direction/constStrength/gustStrength sample per curve root (primitive);
    // stiffness is groom-wide; the mask envelopes per CV.
    UsdGenParamField const directionField = p->GetScalarField(sDirection, 0.0);
    UsdGenParamField const constField = p->GetScalarField(sConst, 0.0);
    UsdGenParamField const gustField = p->GetScalarField(sGust, 0.0);
    UsdGenParamField const maskField = p->GetScalarField(sMask, 1.0);
    if (directionField.connected && directionField.components != 3)
        return fail("connected direction must be vector3f");
    if (!Finite(p->GetDouble(sStiffness, 0.0)))
        return fail("groom usdGen:stiffness must be finite");
    uint32_t const R = upstream.totalCurves;
    for (uint32_t c = 0; c != R; ++c) {
        uint32_t const root = spans[c];
        if (directionField.connected) {
            double const dx = directionField.Value(c, root, 0);
            double const dy = directionField.Value(c, root, 1);
            double const dz = directionField.Value(c, root, 2);
            if (!Finite(dx) || !Finite(dy) || !Finite(dz))
                return fail("per-strand direction must be finite");
            GfVec3f const v{float(dx), float(dy), float(dz)};
            if (v.GetLength() <= 1e-12f)
                return fail("per-strand direction must be nonzero");
        }
        if (!Finite(constField.Value(c, root)) || !Finite(gustField.Value(c, root)))
            return fail("per-strand constStrength/gustStrength must be finite");
        for (uint32_t cv = spans[c]; cv != spans[c + 1]; ++cv) {
            if (!Finite(maskField.Value(c, cv)))
                return fail("per-CV mask must be finite");
        }
    }
    cap.rootRest.resize(R);
    for (uint32_t c = 0; c != R; ++c)
        cap.rootRest[c] = opUtil::RestPoint(upstream, spans[c]);
    return true;
}

void UsdGenWindOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &captureIn,
    UsdGenChunkView *view) const
{
    auto const &cap = static_cast<UsdGenWindCapture const &>(captureIn);
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    UsdGenParamField const directionField =
        p ? p->GetScalarField(sDirection, 0.0) : UsdGenParamField{0.0};
    UsdGenParamField const constField =
        p ? p->GetScalarField(sConst, 0.0) : UsdGenParamField{0.0};
    UsdGenParamField const gustField =
        p ? p->GetScalarField(sGust, 0.0) : UsdGenParamField{0.0};
    UsdGenParamField const maskField =
        p ? p->GetScalarField(sMask, 1.0) : UsdGenParamField{1.0};
    GfVec3f litDirection(0.0f, 0.0f, 1.0f);
    double stiffness = 0.0;
    if (p) {
        VtValue const v = p->GetVtValue(sDirection, VtValue());
        if (v.IsHolding<GfVec3f>()) litDirection = v.UncheckedGet<GfVec3f>();
        stiffness = p->GetDouble(sStiffness, 0.0);
    }
    // The cook time in seconds, with the scheduler's own conversion guard
    // (scheduler.cpp): timecodes divided by a finite positive rate, else raw.
    // Capture validated the parameters; a non-finite clock can only come from
    // the caller, and clamps to frame 0 rather than poisoning the field.
    double const rate = ctx.desc ? ctx.desc->timeCodesPerSecond : 0.0;
    double tSec = std::isfinite(rate) && rate > 0.0 ? ctx.time / rate : ctx.time;
    if (!std::isfinite(tSec)) tSec = 0.0;
    float const drift = float(tSec) * kGustTimeRate;
    float const *lutPtr = nullptr;
    VtVec2fArray const knots = ReadRampKnots(p, sKnots);
    bool const stiffFlat = knots.empty();
    thread_local std::vector<float> tLut;
    if (!stiffFlat) {
        if (tLut.size() != kUsdGenRampLutSize) tLut.assign(kUsdGenRampLutSize, 1.0f);
        UsdGenBuildRampLut(knots,
            p ? p->GetToken(sInterp, sCatmullRom) : sCatmullRom,
            tLut.data(), kUsdGenRampLutSize);
        lutPtr = tLut.data();
    }
    float *px = view->px, *py = view->py, *pz = view->pz;
    auto const *inPx = view->inPx, *inPy = view->inPy, *inPz = view->inPz;
    auto const *hairT = view->hairT;
    auto const *ids = view->curveId;
    size_t const curveBase = view->desc ? size_t(view->desc->firstCurve) : 0;
    size_t const cvBase = view->desc ? size_t(view->desc->firstCv) : 0;
    auto const *cvOff = view->cvOffsets;
    bool const ragged = cvOff != nullptr;

    for (uint32_t c = 0; c < view->curveCount; ++c) {
        size_t const n = ragged ? size_t(cvOff[c + 1] - cvOff[c]) : size_t(view->cvCount);
        size_t const g = ragged ? size_t(cvOff[c] - cvOff[0]) : view->Cv(c, 0);
        if (!n) continue;
        size_t const curve = curveBase + c;
        GfVec3f dir = litDirection;
        if (directionField.connected) {
            dir = GfVec3f(float(directionField.Value(curve, cvBase + g, 0)),
                          float(directionField.Value(curve, cvBase + g, 1)),
                          float(directionField.Value(curve, cvBase + g, 2)));
        }
        // Capture rejects zero directions fail-closed; the guard below only
        // keeps a direct Evaluate call NaN-free (zero displacement).
        float const len = dir.GetLength();
        float const inv = len > 1e-12f ? 1.0f / len : 0.0f;
        dir *= inv;
        float const c0 = float(constField.Value(curve, cvBase + g));
        float const g0 = float(gustField.Value(curve, cvBase + g));
        float gust = 0.0f;
        if (g0 != 0.0f) {
            GfVec3f const root = curve < cap.rootRest.size()
                ? cap.rootRest[curve] : GfVec3f(0.0f, 0.0f, 0.0f);
            float hvec[3];
            HashVec3(ctx.seed, ids ? ids[c] : uint64_t(curve), hvec);
            float const in3[3] = {root[0] * kGustSpaceFreq + hvec[0] + drift,
                                  root[1] * kGustSpaceFreq + hvec[1],
                                  root[2] * kGustSpaceFreq + hvec[2]};
            float out1 = 0.0f;
            SeExpr2::FBM<3, 1, false, float>(in3, &out1,
                                             kGustOctaves, kGustLacunarity, kGustGain);
            gust = g0 * (2.0f * out1 - 1.0f);
        }
        float const strength = c0 + gust;
        for (size_t i = 0; i < n; ++i) {
            size_t const o = g + i;
            size_t const cv = cvBase + o;
            float const t = hairT ? hairT[o]
                                  : (n > 1 ? float(i) / float(n - 1) : 0.0f);
            float const stiffScale = stiffFlat ? 1.0f : UsdGenEvalLut257(lutPtr, t);
            float const flex =
                std::clamp(1.0f - float(stiffness) * stiffScale, 0.0f, 1.0f);
            float const mask = std::clamp(float(maskField.Value(curve, cv)), 0.0f, 1.0f);
            float const d = strength * flex * mask * t;
            if (d == 0.0f) {
                px[o] = inPx[o]; py[o] = inPy[o]; pz[o] = inPz[o];
                continue;
            }
            px[o] = inPx[o] + dir[0] * d;
            py[o] = inPy[o] + dir[1] * d;
            pz[o] = inPz[o] + dir[2] * d;
        }
    }
}

}  // namespace usdGen
