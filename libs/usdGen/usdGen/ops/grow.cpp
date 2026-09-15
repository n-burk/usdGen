// usdGen — UsdGenGrowOp implementation (M1). 02-schema.md §2.6, 04 §2.2.
// Roots -> straight strands: sets the CV count (usdGen:segments), the length
// and the root-frame-B angular lift followed by normalized U-tangent blending.
// Capture fixes the topology (cvCount) and the
// per-curve target length (kSaltGrow draw); Evaluate writes the CV
// positions and hairT. Direction: surfaceNormal (root frame N), attribute
// (M1: root frame T), vector (usdGen:directionVector).
#include "usdGen/ops/grow.h"

#include "usdGen/growLengthMap.h"
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
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

inline uint32_t floatAsBits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

inline GfVec3f NormalizeGrowDirection(GfVec3f v)
{
    float const l2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    float const length = std::sqrt(l2);
    return length > 1.0e-12f ? v / length : GfVec3f(0.0f, 1.0f, 0.0f);
}

// plan/04 §2.2: lift is an angle in degrees about root-frame B.  Rodrigues'
// form keeps the CPU and CUDA lowerings on the same float arithmetic path.
inline GfVec3f RotateGrowDirection(GfVec3f direction, GfVec3f axis,
                                   float degrees)
{
    if (degrees == 0.0f) return direction;
    axis = NormalizeGrowDirection(axis);
    constexpr float pi = 3.14159265358979323846f;
    float const radians = degrees * (pi / 180.0f);
    float const c = std::cos(radians);
    float const s = std::sin(radians);
    float const dot = axis[0] * direction[0] + axis[1] * direction[1] +
        axis[2] * direction[2];
    GfVec3f const cross(
        axis[1] * direction[2] - axis[2] * direction[1],
        axis[2] * direction[0] - axis[0] * direction[2],
        axis[0] * direction[1] - axis[1] * direction[0]);
    float const oneMinusC = 1.0f - c;
    return GfVec3f(
        direction[0] * c + cross[0] * s + axis[0] * dot * oneMinusC,
        direction[1] * c + cross[1] * s + axis[1] * dot * oneMinusC,
        direction[2] * c + cross[2] * s + axis[2] * dot * oneMinusC);
}

// Blend after lift toward the retained U tangent. Preserve length and keep
// the lifted direction when no usable local tangent/blend direction exists.
inline GfVec3f BlendGrowDirection(GfVec3f direction, GfVec3f tangent, float u)
{
    if (u == 0.0f) return direction;
    float const tangentLength = std::sqrt(tangent[0] * tangent[0] +
        tangent[1] * tangent[1] + tangent[2] * tangent[2]);
    if (!(tangentLength > 1.0e-12f) || !std::isfinite(tangentLength))
        return direction;
    tangent /= tangentLength;
    if (u == 1.0f) return tangent;
    GfVec3f const mixed = direction * (1.0f - u) + tangent * u;
    float const length = std::sqrt(mixed[0] * mixed[0] +
        mixed[1] * mixed[1] + mixed[2] * mixed[2]);
    return length > 1.0e-12f && std::isfinite(length) ? mixed / length : direction;
}

struct UsdGenGrowCapture final : public UsdGenCapturePayload
{
    // perCurve[c] = target length of curve c (kSaltGrow draw applied).
    int cvCount = 0;
    float lift = 0.0f;
    float uvBlend = 0.0f;
    TfToken direction = TfToken("surfaceNormal");
    GfVec3f directionVector{0.0f, 1.0f, 0.0f};
    uint64_t upstreamTopologyVersion = 0;
    uint64_t upstreamValueVersion = 0;
    uint32_t upstreamCurves = 0;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenGrowCapture>(*this);
    }
    bool OwnsBuffer() const override { return true; }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.valueVersion == upstreamValueVersion &&
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
    // Capture materializes a fresh width owner because Grow changes the CV
    // layout.  Mark it written so generic preparation does not replace that
    // transformed owner with an incompatible upstream alias.
    return kPlanePoints | kPlaneWidths | kPlaneHairT;
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
    double const lift = params.GetDouble(TfToken("lift"), 0.0);
    if (!std::isfinite(lift) || lift < -90.0 || lift > 90.0) {
        if (diag)
            diag->Error("UsdGenGrow: usdGen:lift must be finite and in [-90, 90] degrees");
        return false;
    }
    VtValue const uvValue = params.GetVtValue(TfToken("uvBlend"), VtValue());
    if (!uvValue.IsEmpty() && !uvValue.IsHolding<float>() && !uvValue.IsHolding<double>()) {
        if (diag) diag->Error("UsdGenGrow: usdGen:uvBlend must be a floating-point scalar");
        return false;
    }
    double const uvBlend = params.GetDouble(TfToken("uvBlend"), 0.0);
    if (!std::isfinite(uvBlend) || uvBlend < 0.0 || uvBlend > 1.0) {
        if (diag) diag->Error("UsdGenGrow: usdGen:uvBlend must be finite and in [0, 1]");
        return false;
    }
    VtValue const directionValue = params.GetVtValue(
        TfToken("directionVector"), VtValue());
    if (!directionValue.IsEmpty() && !directionValue.IsHolding<GfVec3f>()) {
        if (diag)
            diag->Error("UsdGenGrow: usdGen:directionVector must be vector3f");
        return false;
    }
    if (directionValue.IsHolding<GfVec3f>()) {
        GfVec3f const v = directionValue.UncheckedGet<GfVec3f>();
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) ||
            !std::isfinite(v[2])) {
            if (diag)
                diag->Error("UsdGenGrow: usdGen:directionVector must be finite");
            return false;
        }
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
        feed("lift", doubleAsBits(p->GetDouble(TfToken("lift"), 0.0)));
        feed("uvBlend", doubleAsBits(p->GetDouble(TfToken("uvBlend"), 0.0)));
        // ParamView substitutes defaults for malformed authored values.
        // Retain the native type in the key so a malformed edit cannot reuse
        // a valid zero-blend capture and bypass Capture's type validation.
        feed("uvBlendType", TfToken(p->GetVtValue(TfToken("uvBlend"),
            VtValue()).GetTypeName()).Hash());
        GfVec3f directionVector(0.0f, 1.0f, 0.0f);
        if (VtValue const v = p->GetVtValue(TfToken("directionVector"), VtValue());
            v.IsHolding<GfVec3f>())
            directionVector = v.UncheckedGet<GfVec3f>();
        feed("directionVector.x", floatAsBits(directionVector[0]));
        feed("directionVector.y", floatAsBits(directionVector[1]));
        feed("directionVector.z", floatAsBits(directionVector[2]));
        feed("seed", ctx.seed);
    }
    // Grow captures the fallback only when its input has no width plane, but
    // including it unconditionally is cheap and prevents a stale capture if
    // a description edit adds/removes the inherited plane at the same time.
    feed("defaultWidth", doubleAsBits(ctx.desc ? ctx.desc->defaultWidth : 0.01));
    return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenGrowOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    auto &cap = *static_cast<UsdGenGrowCapture *>(out);

    int segments = p ? p->GetInt(TfToken("segments"), 8) : 8;
    if (segments < 2 || segments > 64) {
        if (diag) diag->Error("UsdGenGrow: usdGen:segments must be in [2, 64]");
        return false;
    }
    double const lift = p ? p->GetDouble(TfToken("lift"), 0.0) : 0.0;
    if (!std::isfinite(lift) || lift < -90.0 || lift > 90.0) {
        if (diag) diag->Error("UsdGenGrow: usdGen:lift must be finite and in [-90, 90] degrees");
        return false;
    }
    VtValue const uvValue = p ? p->GetVtValue(TfToken("uvBlend"), VtValue()) : VtValue();
    if (!uvValue.IsEmpty() && !uvValue.IsHolding<float>() && !uvValue.IsHolding<double>()) {
        if (diag) diag->Error("UsdGenGrow: usdGen:uvBlend must be a floating-point scalar");
        return false;
    }
    double const uvBlend = p ? p->GetDouble(TfToken("uvBlend"), 0.0) : 0.0;
    if (!std::isfinite(uvBlend) || uvBlend < 0.0 || uvBlend > 1.0) {
        if (diag) diag->Error("UsdGenGrow: usdGen:uvBlend must be finite and in [0, 1]");
        return false;
    }
    GfVec3f directionVector(0.0f, 1.0f, 0.0f);
    if (p) {
        VtValue const v = p->GetVtValue(TfToken("directionVector"), VtValue());
        if (!v.IsEmpty() && !v.IsHolding<GfVec3f>()) {
            if (diag) diag->Error("UsdGenGrow: usdGen:directionVector must be vector3f");
            return false;
        }
        if (v.IsHolding<GfVec3f>()) directionVector = v.UncheckedGet<GfVec3f>();
    }
    if (!std::isfinite(directionVector[0]) || !std::isfinite(directionVector[1]) ||
        !std::isfinite(directionVector[2])) {
        if (diag) diag->Error("UsdGenGrow: usdGen:directionVector must be finite");
        return false;
    }
    // A description-supplied fallback is authored data, not a best-effort
    // hint.  Reject invalid values before changing this capture's buffer so
    // the scheduler can retain the last-good generation.
    float const fallbackWidth = ctx.desc ? ctx.desc->defaultWidth : 0.01f;
    if (!std::isfinite(fallbackWidth) || fallbackWidth < 0.0f) {
        if (diag) diag->Error("UsdGenGrow: description defaultWidth must be "
                              "finite and >= 0");
        return false;
    }
    cap.cvCount = segments;
    cap.lift = static_cast<float>(lift);
    cap.uvBlend = static_cast<float>(uvBlend);
    cap.direction = p ? p->GetToken(TfToken("direction"), TfToken("surfaceNormal"))
                      : TfToken("surfaceNormal");
    cap.directionVector = directionVector;
    double length = p ? p->GetDouble(TfToken("length"), 1.0) : 1.0;
    double lo = 1.0, hi = 1.0;
    if (p) ReadVec2Prop(*p, TfToken("lengthRandom"), lo, hi);
    if (lo > hi) std::swap(lo, hi);

    if (!std::isfinite(length) || !std::isfinite(lo) || !std::isfinite(hi)) {
        if (diag) diag->Error("UsdGenGrow: usdGen:length and lengthRandom must be finite");
        return false;
    }

    uint32_t const R = upstream.totalCurves;
    std::shared_ptr<const UsdGenImagePayload> lengthImage;
    UsdGenImageSampleOptions lengthImageOptions;
    if (!ctx.desc || !p || !p->node) {
        if (ctx.mapBindingCount || ctx.mapCount) {
            if (diag) diag->Error("UsdGenGrow: length map binding has no graph descriptor");
            return false;
        }
    } else {
        std::string mapReason;
        if (!ResolveUsdGenGrowLengthImageMap(*ctx.desc, *p->node, &lengthImage,
                                             &lengthImageOptions, &mapReason)) {
            if (diag) diag->Error(mapReason);
            return false;
        }
    }
    if (lengthImage && upstream.rootUV.size() != R) {
        if (diag) diag->Error("UsdGenGrow: LengthSource ImageMap requires one root st UV per curve (got " +
                              std::to_string(upstream.rootUV.size()) + " for " +
                              std::to_string(R) + " curves)");
        return false;
    }

    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamValueVersion = upstream.valueVersion;
    cap.upstreamCurves = upstream.totalCurves;

    UsdGenCurveBuffer &buf = cap.MutableBuffer();
    if (R > std::numeric_limits<uint32_t>::max() /
                static_cast<uint32_t>(cap.cvCount)) {
        if (diag) diag->Error("UsdGenGrow: CV topology exceeds uint32 cardinality");
        return false;
    }
    buf.totalCurves = R;
    buf.totalCvs = R * uint32_t(cap.cvCount);
    buf.topologyVersion = upstream.topologyVersion + 1;
    buf.cvOffsets.clear();  // fixed cvCount: Grow's output is uniform.

    // A CV-topology producer cannot retain the old width owner: it either
    // resamples a valid vertex-width plane to the new span or creates the
    // description fallback.  This happens during capture, before workers
    // receive raw output pointers, and is deliberately private storage.
    VtFloatArray grownWidths(buf.totalCvs, fallbackWidth);
    if (!upstream.width.empty()) {
        if (upstream.width.size() != upstream.totalCvs) {
            if (diag) diag->Error("UsdGenGrow: upstream width plane cardinality "
                                  "does not match its CV topology");
            return false;
        }
        std::vector<uint32_t> sourceSpans;
        if (upstream.cvOffsets.empty()) {
            if (R && upstream.totalCvs % R != 0) {
                if (diag) diag->Error("UsdGenGrow: upstream width plane has "
                                      "non-uniform CV topology without offsets");
                return false;
            }
            sourceSpans.resize(size_t(R) + 1);
            uint32_t const perCurve = R ? upstream.totalCvs / R : 0;
            for (uint32_t c = 0; c != R; ++c)
                sourceSpans[c + 1] = sourceSpans[c] + perCurve;
        } else {
            if (upstream.cvOffsets.size() != size_t(R) + 1 ||
                upstream.cvOffsets.front() != 0 ||
                upstream.cvOffsets.back() != static_cast<int>(upstream.totalCvs)) {
                if (diag) diag->Error("UsdGenGrow: upstream width plane has "
                                      "invalid ragged CV offsets");
                return false;
            }
            sourceSpans.resize(size_t(R) + 1);
            for (uint32_t c = 0; c != R; ++c) {
                int const first = upstream.cvOffsets[c];
                int const last = upstream.cvOffsets[c + 1];
                if (first < 0 || last < first) {
                    if (diag) diag->Error("UsdGenGrow: upstream width plane has "
                                          "decreasing ragged CV offsets");
                    return false;
                }
                sourceSpans[c] = static_cast<uint32_t>(first);
                sourceSpans[c + 1] = static_cast<uint32_t>(last);
            }
        }
        for (uint32_t c = 0; c != R; ++c) {
            uint32_t const sourceFirst = sourceSpans[c];
            uint32_t const sourceCount = sourceSpans[c + 1] - sourceFirst;
            if (sourceCount == 0) {
                if (diag) diag->Error("UsdGenGrow: cannot resample a zero-CV "
                                      "upstream width curve");
                return false;
            }
            for (uint32_t i = 0; i != static_cast<uint32_t>(cap.cvCount); ++i) {
                float const u = cap.cvCount > 1
                    ? float(i) / float(cap.cvCount - 1) : 0.0f;
                float const sourcePosition = u * float(sourceCount - 1);
                // Large spans may round the float endpoint above the last
                // valid index, including to 2^32. Clamp before converting.
                uint32_t const loIndex = sourcePosition >= float(sourceCount - 1)
                    ? sourceCount - 1 : static_cast<uint32_t>(sourcePosition);
                uint32_t const hiIndex = std::min(loIndex + 1, sourceCount - 1);
                float const alpha = sourcePosition - float(loIndex);
                float const loValue = upstream.width[sourceFirst + loIndex];
                float const hiValue = upstream.width[sourceFirst + hiIndex];
                grownWidths[size_t(c) * cap.cvCount + i] =
                    loValue + (hiValue - loValue) * alpha;
            }
        }
    }
    buf.width = std::move(grownWidths);

    // Grow changes every curve's CV span. Named vertex planes therefore need
    // a fresh resample and named uniform/constant planes need fresh copies;
    // retaining the old VtArray handles would pair the new topology with the
    // prior cardinality and let later writes detach the wrong owner.
    std::string planeError;
    if (!UsdGenResampleExtraPlanes(upstream, &buf, &planeError)) {
        if (diag) diag->Error("UsdGenGrow: cannot preserve named planes across "
                              "CV topology change: " + planeError);
        return false;
    }

    // Per-curve target lengths: len = length * (lo + (hi-lo) * Draw01) *
    // LengthSource(root-st).  The image multiplier is intentionally not
    // clamped: authored map options are applied by the sampler, then Grow
    // rejects a negative/non-finite multiplier or final target.
    cap.perCurve.clear();
    if (R) {
        cap.perCurve.resize(R);
        auto const *ids = upstream.curveId.empty() ? nullptr : upstream.curveId.data();
        auto *len = cap.perCurve.data();
        for (uint32_t c = 0; c < R; ++c) {
            float const r = UsdGenDraw01(int(ctx.seed), ids ? ids[c] : 0,
                                         kSaltGrow);
            float const sampled = lengthImage
                ? UsdGenImageSampler::Sample(*lengthImage, upstream.rootUV[c][0],
                                              upstream.rootUV[c][1], lengthImageOptions)
                : 1.0f;
            if (!std::isfinite(sampled) || sampled < 0.0f) {
                if (diag) diag->Error("UsdGenGrow: LengthSource ImageMap produced a negative or non-finite multiplier");
                return false;
            }
            double const random = lo + static_cast<double>(r) * (hi - lo);
            double const target = length * random * static_cast<double>(sampled);
            float const targetFloat = static_cast<float>(target);
            if (!std::isfinite(target) || target < 0.0 ||
                !std::isfinite(targetFloat) || targetFloat < 0.0f) {
                if (diag) diag->Error("UsdGenGrow: length target must be finite and non-negative");
                return false;
            }
            len[c] = targetFloat;
        }
    }

    // Mask block (02 §2.13) — resolved once per capture epoch (I4).
    cap.curveMask.clear();
    cap.maskRampLut.clear();
    if (p) {
        UsdGenMaskSettings m = UsdGenMaskSettingsFromParams(*p);
        auto const mask = EvaluateMask(m, upstream.curveId, upstream.px, {}, 0.0);
        cap.curveMask = mask.curveMask;
        cap.maskRampLut = mask.rampLut;
    }
    // Grow owns a new CV topology, so it also owns a new rest channel.  Use
    // the same direction/length/mask formula as Evaluate, rooted at the C3
    // rest position when present (or at current roots for Scatter).  No
    // worker writes rest and no old-cardinality VtArray can leak through.
    if (upstream.rest.size() && upstream.rest.size() != upstream.totalCvs) {
        if (diag) diag->Error("UsdGenGrow: upstream rest plane cardinality does not match its CV topology");
        return false;
    }
    for (GfVec3f const &point : upstream.rest) {
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])) {
            if (diag) diag->Error("UsdGenGrow: upstream rest plane contains non-finite values");
            return false;
        }
    }
    if ((!upstream.rootT.empty() && upstream.rootT.size() != R) ||
        (!upstream.rootN.empty() && upstream.rootN.size() != R) ||
        (!upstream.rootB.empty() && upstream.rootB.size() != R)) {
        if (diag) diag->Error("UsdGenGrow: upstream root-frame plane cardinality does not match curve topology");
        return false;
    }
    auto finiteFrame = [](VtVec3fArray const &values) {
        for (GfVec3f const &v : values)
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
        return true;
    };
    if (!finiteFrame(upstream.rootT) || !finiteFrame(upstream.rootN) || !finiteFrame(upstream.rootB)) {
        if (diag) diag->Error("UsdGenGrow: upstream root-frame plane contains non-finite values");
        return false;
    }
    if (upstream.px.size() != upstream.totalCvs || upstream.py.size() != upstream.totalCvs ||
        upstream.pz.size() != upstream.totalCvs) {
        if (diag) diag->Error("UsdGenGrow: upstream point plane cardinality does not match its CV topology");
        return false;
    }
    std::vector<uint32_t> rootSpans(size_t(R) + 1, 0);
    if (upstream.cvOffsets.empty()) {
        if (R && upstream.totalCvs % R != 0) {
            if (diag) diag->Error("UsdGenGrow: upstream CV topology is non-uniform without offsets");
            return false;
        }
        uint32_t const perCurve = R ? upstream.totalCvs / R : 0;
        for (uint32_t c = 0; c != R; ++c) rootSpans[c + 1] = rootSpans[c] + perCurve;
    } else {
        if (upstream.cvOffsets.size() != size_t(R) + 1 || upstream.cvOffsets.front() != 0 ||
            upstream.cvOffsets.back() != static_cast<int>(upstream.totalCvs)) {
            if (diag) diag->Error("UsdGenGrow: upstream CV topology has invalid ragged offsets");
            return false;
        }
        for (uint32_t c = 0; c != R; ++c) {
            int const first = upstream.cvOffsets[c], last = upstream.cvOffsets[c + 1];
            if (first < 0 || last <= first) {
                if (diag) diag->Error("UsdGenGrow: upstream CV topology has an empty/decreasing curve");
                return false;
            }
            rootSpans[c] = static_cast<uint32_t>(first);
            rootSpans[c + 1] = static_cast<uint32_t>(last);
        }
    }
    VtVec3fArray grownRest(buf.totalCvs);
    float const *maskLut = cap.maskRampLut.empty() ? nullptr : cap.maskRampLut.cdata();
    for (uint32_t c = 0; c != R; ++c) {
        uint32_t const rootIndex = rootSpans[c];
        GfVec3f root = upstream.rest.empty() ? GfVec3f(upstream.px[rootIndex], upstream.py[rootIndex], upstream.pz[rootIndex])
                                              : upstream.rest[rootIndex];
        GfVec3f dir = cap.direction == sVector ? cap.directionVector
            : cap.direction == sAttr ? (upstream.rootT.empty() ? GfVec3f(0.0f, 1.0f, 0.0f) : upstream.rootT[c])
            : (upstream.rootN.empty() ? GfVec3f(0.0f, 1.0f, 0.0f) : upstream.rootN[c]);
        dir = NormalizeGrowDirection(dir);
        GfVec3f const axis = upstream.rootB.empty()
            ? GfVec3f(0.0f, 1.0f, 0.0f) : upstream.rootB[c];
        dir = RotateGrowDirection(dir, axis, cap.lift);
        dir = BlendGrowDirection(dir, upstream.rootT.empty() ? GfVec3f(0.0f)
            : upstream.rootT[c], cap.uvBlend);
        float const mask = cap.curveMask.empty() ? 1.0f : cap.curveMask[c];
        for (uint32_t i = 0; i != static_cast<uint32_t>(cap.cvCount); ++i) {
            float const t = cap.cvCount > 1 ? float(i) / float(cap.cvCount - 1) : 0.0f;
            float const weight = mask * (maskLut ? UsdGenEvalLut257(maskLut, t) : 1.0f);
            float const distance = cap.perCurve[c] * t * weight;
            grownRest[size_t(c) * cap.cvCount + i] = root + dir * distance;
        }
    }
    buf.rest = std::move(grownRest);
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
        // Input chunks may be ragged C3 curves.  Their plane ports are
        // already sliced at inFirstCv, so rebase the absolute upstream
        // offsets instead of treating inCvCount==0 as every root at CV 0.
        uint32_t const r = view->inCvOffsets
            ? static_cast<uint32_t>(view->inCvOffsets[c] - int(view->inFirstCv))
            : c * view->inCvCount;
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
        GfVec3f d = NormalizeGrowDirection(dir);
        GfVec3f const axis = view->rootB ? view->rootB[c]
            : GfVec3f(0.0f, 1.0f, 0.0f);
        d = RotateGrowDirection(d, axis, cap.lift);
        d = BlendGrowDirection(d, view->rootT ? view->rootT[c] : GfVec3f(0.0f),
                               cap.uvBlend);
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
            const float s = targetLen * t * w;
            uint32_t const o = static_cast<uint32_t>(view->Cv(c, i));
            px[o] = rx + d[0] * s;
            py[o] = ry + d[1] * s;
            pz[o] = rz + d[2] * s;
            if (view->hairT) view->hairT[o] = t;
        }
    }
}

}  // namespace usdGen
