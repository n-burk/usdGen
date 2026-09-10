// usdGen — UsdGenScatterOp implementation (M1, mode="random").
//
// 02-schema.md §2.6, 04-operators.md :620-676. Area-weighted scatter:
//   n_f = floor(expected) + (Hash01(Hash64(seed, f, kSaltScatter),
//                                    kSaltScatter) < frac(expected))
//   expected = density * areaRest(f) * meanMask(f)          (plan/04 :634-635)
// Curve ids are UsdGenCurveId(seed, faceIndex, k) (plan/04 :637) so they stay
// stable across density edits (plan/04 :663); densityScale is NOT applied
// here — it decimates the captured root set by stable id at publish time
// (KeepCurve), so a density scrub never re-runs Capture (ADR §9 R13).
// Each root position is drawn from three curveId-keyed draws (area-weighted
// fan-triangle pick + uniform in-triangle point): the M1 realization of the
// "low-discrepancy sample seeded by curveId" (plan/04 :638) — being purely
// curveId-keyed it is order-invariant across thread counts and chunk splits
// (review M-3 / engine rule E-8). Roots are Morton-sorted by rest position so
// chunk placement is surface-major (plan/04 :641).
#include "usdGen/ops/scatter.h"

#include "usdGen/mask.h"
#include "usdGen/maskParams.h"
#include "usdGenMath/usdGenMath/hash.h"
#include "usdGenMath/usdGenMath/kernels.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

struct UsdGenScatterCapture final : public UsdGenCapture
{
    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        auto c = std::make_unique<UsdGenScatterCapture>();
        *c = *this;
        return c;
    }
    bool OwnsBuffer() const override { return true; }
};

GfVec3f Normalize3(GfVec3f const &v)
{
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 1e-12f ? v / l : GfVec3f(0.0f, 1.0f, 0.0f);
}

inline uint64_t double_as_bits(double d)
{
    uint64_t u;
    static_assert(sizeof(u) == sizeof(d), "");
    std::memcpy(&u, &d, sizeof(d));
    return u;
}

}  // namespace

static TfTokenVector _topoParams = [] {
    TfTokenVector v = UsdGenBaseTopologyParams();
    v.push_back(TfToken("enabled"));      // generator: topology-class (02 §6.2)
    v.push_back(TfToken("mode"));        // structural (02 §6.1)
    v.push_back(TfToken("density"));     // capture (02 §6.4)
    v.push_back(TfToken("spacingU"));
    v.push_back(TfToken("spacingV"));
    v.push_back(TfToken("jitter"));
    v.push_back(TfToken("rootPrims"));
    v.push_back(TfToken("rootUVs"));
    v.push_back(TfToken("relaxIterations"));
    v.push_back(TfToken("areaCompensation"));
    v.push_back(TfToken("flip"));
    v.push_back(TfToken("perGuide"));
    auto m = UsdGenMaskTopologyParams();
    v.insert(v.end(), m.begin(), m.end());
    return v;
}();

static TfTokenVector _valueParams = [] {
    TfTokenVector v = UsdGenBaseValueParams();
    auto m = UsdGenMaskValueParams();
    v.insert(v.end(), m.begin(), m.end());
    return v;
}();

static TfTokenVector _refInputs = { TfToken("guides") };

TfSpan<const TfToken> UsdGenScatterOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenScatterOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
TfSpan<const TfToken> UsdGenScatterOp::ReferenceInputs() const
{
    return TfSpan<const TfToken>(_refInputs.data(), _refInputs.size());
}
std::unique_ptr<UsdGenCapture> UsdGenScatterOp::CreateCapture() const
{
    return std::make_unique<UsdGenScatterCapture>();
}
uint32_t UsdGenScatterOp::PlanesTouched() const
{
    return kPlanePoints | kPlaneHairT;
}

bool UsdGenScatterOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    TfToken mode = params.GetToken(TfToken("mode"), TfToken("random"));
    if (mode != TfToken("random")) {
        if (diag) {
            diag->Error("UsdGenScatter: M1 implements usdGen:mode=\"random\" only "
                        "(got \"" + mode.GetString() + "\")");
        }
        return false;
    }
    if (params.GetDouble(TfToken("jitter"), 0.0) < 0.0) {
        if (diag) diag->Error("UsdGenScatter: jitter must be >= 0");
        return false;
    }
    return true;
}

UsdGenEpoch UsdGenScatterOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    // Capture-class inputs: mode/density/spacing/jitter/flip/perGuide + the
    // mask capture block + seed, pinned to the surface's topology generation
    // (03 §3.4). The surfaceGeneration term is bumped by the imaging layer on
    // any rest/topology change, so no point-level re-hashing is needed here.
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    uint64_t h = 1469598103934665603ULL;
    auto feed = [&](std::string const &k, uint64_t v) {
        h ^= v; h *= 0x100000001b3ULL;
        h ^= k.size(); h *= 0x100000001b3ULL;
    };
    feed("mode", p ? static_cast<uint64_t>(p->GetToken(TfToken("mode"), TfToken("random")).Hash()) : 0);
    feed("density", p ? double_as_bits(p->GetDouble(TfToken("density"), 100.0)) : 0);
    feed("seed", ctx.seed);
    UsdGenGraphDesc const *desc = ctx.desc;
    if (desc && ctx.surface < desc->surfaces.size()) {
        feed("surfaceGen", desc->surfaces[ctx.surface].surfaceGeneration);
        feed("subset", uint64_t(desc->surfaces[ctx.surface].subsetFaces.size()));
    }

    // The mask block changes Capture OUTPUT through meanMask in the expected
    // root count (plan/04 :634, :676), so its inputs belong in the capture
    // digest (review M-5). Same key spellings as the capture block below.
    if (p) {
        feed("mask:amount", double_as_bits(p->GetDouble(TfToken("mask:amount"), 1.0)));
        feed("mask:invert", p->GetBool(TfToken("mask:invert"), false) ? 1u : 0u);
        feed("mask:combine",
             static_cast<uint64_t>(p->GetToken(TfToken("mask:combine"), TfToken("multiply")).Hash()));
        feed("mask:random", double_as_bits(p->GetDouble(TfToken("mask:random"), 0.0)));
        feed("mask:randomSeed", static_cast<uint64_t>(p->GetInt(TfToken("mask:randomSeed"), 0)));
        feed("mask:rangeMin", double_as_bits(p->GetDouble(TfToken("mask:rangeMin"), 0.0)));
        feed("mask:rangeMax", double_as_bits(p->GetDouble(TfToken("mask:rangeMax"), 1.0)));
        feed("mask:ramp:interp",
             static_cast<uint64_t>(p->GetToken(TfToken("mask:ramp:interpolation"),
                                               TfToken("catmullRom")).Hash()));
        auto kn = p->GetVtValue(TfToken("mask:ramp:knots"),
                                VtValue(VtVec2fArray{GfVec2f(0, 1), GfVec2f(1, 1)}));
        if (kn.IsHolding<VtVec2fArray>()) {
            auto const &knots = kn.UncheckedGet<VtVec2fArray>();
            feed("mask:ramp:n", knots.size());
            for (auto const &g : knots) {
                feed("mask:ramp.x", double_as_bits(double(g[0])));
                feed("mask:ramp.y", double_as_bits(double(g[1])));
            }
        }
    }
    return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenScatterOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    TF_UNUSED(upstream);
    UsdGenGraphDesc const *desc = ctx.desc;
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    if (!desc) {
        if (diag) diag->Error("UsdGenScatter::Capture: no graph description");
        return false;
    }
    if (ctx.surface >= desc->surfaces.size()) {
        if (diag) diag->Error("UsdGenScatter::Capture: no bound usdGen:surface");
        return false;
    }
    UsdGenSurfaceDesc const &surf = desc->surfaces[ctx.surface];
    if (surf.faceVertexCounts.empty() || surf.restPoints.empty()) {
        if (diag) diag->Warn(std::string("UsdGenScatter::Capture: surface '") +
                             surf.path.GetText() + "' has no topology; 0 roots");
    }

    // Face list: whole mesh or the GeomSubset restriction (R15).
    std::vector<int> faces;
    if (!surf.subsetFaces.empty()) faces.assign(surf.subsetFaces.cbegin(), surf.subsetFaces.cend());
    else {
        faces.resize(surf.faceVertexCounts.size());
        std::iota(faces.begin(), faces.end(), 0);
    }

    UsdGenScatterCapture &cap = *static_cast<UsdGenScatterCapture *>(out);
    UsdGenCurveBuffer &buf = cap.MutableBuffer();
    buf.topologyVersion += 1;

    auto const *rest = surf.restPoints.empty() ? nullptr : surf.restPoints.data();
    auto const *fvc = surf.faceVertexCounts.empty() ? nullptr : surf.faceVertexCounts.data();
    auto const *fvi = surf.faceVertexIndices.empty() ? nullptr : surf.faceVertexIndices.data();
    auto const *uv = surf.uv.empty() ? nullptr : surf.uv.data();
    if (!rest || !fvc || !fvi) {
        buf.totalCurves = 0;
        buf.totalCvs = 0;
        return true;  // the empty-topology warning above already fired
    }

    // faceVertexIndices are FACE-RELATIVE (02 §2.2): face f's corners are
    // fvi[cornerOff[f] .. cornerOff[f] + fvc[f]).
    std::vector<size_t> cornerOff(surf.faceVertexCounts.size() + 1, 0);
    for (size_t f = 0; f < surf.faceVertexCounts.size(); ++f)
        cornerOff[f + 1] = cornerOff[f] + size_t(fvc[f]);

    const double density = p ? p->GetDouble(TfToken("density"), 100.0) : 100.0;
    const bool flip = p ? p->GetBool(TfToken("flip"), false) : false;

    // Mask settings (02 §2.13; same key spellings as ops/mask.cpp) and the
    // per-face meanMask of plan/04 :634. meanMask is undefined in the plan
    // beyond its name (review M-5): the M1 realization is the mask evaluated
    // at the face-level probe id curveId(seed, f, 0). amount / invert /
    // random still scale it, so mask:amount == 0 ⇒ zero roots holds
    // (plan/04 :676) via the expected <= 0 skip below.
    UsdGenMaskSettings m;
    if (p) {
        m.random = static_cast<float>(p->GetDouble(TfToken("mask:random"), 0.0));
        m.randomSeed = p->GetInt(TfToken("mask:randomSeed"), 0);
        m.combine = p->GetToken(TfToken("mask:combine"), TfToken("multiply"));
        m.amount = static_cast<float>(p->GetDouble(TfToken("mask:amount"), 1.0));
        m.invert = p->GetBool(TfToken("mask:invert"), false);
        m.rangeX = static_cast<float>(p->GetDouble(TfToken("mask:rangeMin"), 0.0));
        m.rangeY = static_cast<float>(p->GetDouble(TfToken("mask:rangeMax"), 1.0));
        m.effectPosition = static_cast<float>(p->GetDouble(TfToken("mask:effectPosition"), 0.5));
        m.falloff = static_cast<float>(p->GetDouble(TfToken("mask:falloff"), 0.5));
        m.influenceWidth = static_cast<float>(p->GetDouble(TfToken("mask:influenceWidth"), 0.5));
        auto knots = p->GetVtValue(TfToken("mask:ramp:knots"),
                                   VtValue(VtVec2fArray{GfVec2f(0, 1), GfVec2f(1, 1)}));
        if (knots.IsHolding<VtVec2fArray>()) m.rampKnots = knots.UncheckedGet<VtVec2fArray>();
        m.rampInterpolation = p->GetToken(TfToken("mask:ramp:interpolation"),
                                          TfToken("catmullRom"));
    }
    VtFloatArray faceMask;
    if (p) {
        VtArray<uint64_t> probeIds(faces.size());
        for (size_t i = 0; i < faces.size(); ++i)
            probeIds[i] = UsdGenCurveId(ctx.seed, uint32_t(faces[i]), 0u);
        faceMask = EvaluateMask(m, probeIds, {}, {}, 0.0).curveMask;
    }

    // Per-face area-weighted emission (plan/04 :634-635).
    std::vector<float> ax, ay, az;
    std::vector<uint64_t> aids;
    std::vector<int> aPrim;
    std::vector<GfVec2f> aUv;
    std::vector<GfVec3f> aT, aN, aB;
    ax.reserve(faces.size() * 4); ay.reserve(faces.size() * 4); az.reserve(faces.size() * 4);
    aids.reserve(faces.size() * 4); aPrim.reserve(faces.size() * 4);
    aUv.reserve(faces.size() * 4); aT.reserve(faces.size() * 4);
    aN.reserve(faces.size() * 4); aB.reserve(faces.size() * 4);
    std::vector<float> triArea;
    triArea.reserve(16);
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        int const f = faces[fi];
        if (f < 0 || size_t(f) >= surf.faceVertexCounts.size()) continue;  // bad subset
        int const nc = fvc[f];
        if (nc < 3) continue;
        size_t const cbase = cornerOff[f];
        bool bad = false;
        for (int i = 0; i < nc; ++i)
            if (fvi[cbase + size_t(i)] < 0 ||
                size_t(fvi[cbase + size_t(i)]) >= surf.restPoints.size()) { bad = true; break; }
        if (bad) continue;  // malformed face; skip rather than read out of bounds
        // Fan triangulation of face f (the same decomposition
        // UsdGenPolygonRestArea uses): triArea[t] is the area of the triangle
        // rest[c0], rest[c(t+1)], rest[c(t+2)], nAcc accumulates the face
        // normal. areaRest(f) = UsdGenPolygonRestArea (plan/04 :634); the
        // float sum below equals it up to fp-associativity and is used for the
        // sampling weights so the cumulative weights sum exactly to the
        // denominator.
        GfVec3f const p0 = rest[fvi[cbase]];
        triArea.clear();
        triArea.reserve(size_t(nc) - 2);
        double areaRest = 0.0;
        GfVec3f nAcc(0.0f, 0.0f, 0.0f);
        for (int t = 1; t + 1 < nc; ++t) {
            GfVec3f const pb = rest[fvi[cbase + size_t(t)]];
            GfVec3f const pc = rest[fvi[cbase + size_t(t) + 1]];
            triArea.push_back(UsdGenTriangleArea(
                p0[0], p0[1], p0[2], pb[0], pb[1], pb[2], pc[0], pc[1], pc[2]));
            areaRest += double(triArea.back());
            nAcc += GfCross(pb - p0, pc - p0);
        }
        GfVec3f const Nrest = Normalize3(nAcc);

        double const maskVal = fi < faceMask.size() ? double(faceMask[fi]) : 1.0;
        double const expected = density * areaRest * maskVal;
        if (!(expected > 0.0)) continue;  // includes mask:amount == 0 (:676)
        double const whole = std::floor(expected);
        double const frac = expected - whole;
        uint64_t const faceKey = UsdGenHash64(uint64_t(uint32_t(ctx.seed)),
                                              uint64_t(uint32_t(f)), kSaltScatter);
        uint64_t const nf = uint64_t(whole)
                          + (UsdGenHash01(faceKey, kSaltScatter) < float(frac) ? 1u : 0u);

        for (uint64_t k = 0; k < nf; ++k) {
            uint64_t const curveId = UsdGenCurveId(ctx.seed, uint32_t(f), uint32_t(k));
            // "low-discrepancy sample seeded by curveId" (plan/04 :638),
            // realized in M1 as: area-weighted fan-triangle pick + uniform
            // in-triangle point, three draws keyed only by curveId.
            float const u0 = UsdGenDraw01(int(ctx.seed), curveId, kSaltScatterBary);
            float const u1 = UsdGenDraw01(int(ctx.seed), curveId, kSaltScatterBary + 1u);
            float const u2 = UsdGenDraw01(int(ctx.seed), curveId, kSaltScatterBary + 2u);
            size_t ti = triArea.size() - 1;
            {
                double cum = 0.0;
                double const target = double(u0) * areaRest;
                for (size_t t = 0; t < triArea.size(); ++t) {
                    cum += double(triArea[t]);
                    if (target < cum) { ti = t; break; }
                }
            }
            size_t const ib = cbase + ti + 1;
            size_t const ic = ib + 1;
            GfVec3f const pb = rest[fvi[ib]];
            GfVec3f const pc = rest[fvi[ic]];
            float const r1 = std::sqrt(u1);
            GfVec3f const pos = p0 * (1.0f - r1) + pb * (u2 * r1) + pc * (r1 * (1.0f - u2));
            GfVec2f puv(1.0f / 3.0f, 1.0f / 3.0f);
            if (uv)
                puv = uv[fvi[cbase]] * (1.0f - r1)
                    + uv[fvi[ib]] * (u2 * r1)
                    + uv[fvi[ic]] * (r1 * (1.0f - u2));
            // orthonormal frame: N_rest from the fan, T from a triangle
            // edge Gram-Schmidt'd against N (plan/04 :639). Replaces the M0
            // |dot(e0,N)| > 0.5 heuristic, which produced non-orthogonal
            // frames for near-axis-aligned faces.
            GfVec3f e0 = pb - p0;
            if (std::abs(float(GfDot(e0, Nrest))) > 0.9f * float(e0.GetLength()))
                e0 = pc - p0;  // e0 too normal-parallel; try the other edge
            GfVec3f T = e0 - Nrest * GfDot(e0, Nrest);
            if (T.GetLength() < 1e-9f) {
                T = std::abs(Nrest[0]) > 0.9f ? GfVec3f(0.0f, 1.0f, 0.0f)
                                             : GfVec3f(1.0f, 0.0f, 0.0f);
                T = T - Nrest * GfDot(T, Nrest);
            }
            T = Normalize3(T);
            GfVec3f B = Normalize3(GfCross(Nrest, T));
            if (flip) { T = -T; B = -B; }  // rest frame handedness (02 §2.6)
            ax.push_back(pos[0]); ay.push_back(pos[1]); az.push_back(pos[2]);
            aids.push_back(curveId);
            aPrim.push_back(f);
            aUv.push_back(puv);
            aT.push_back(T); aN.push_back(Nrest); aB.push_back(B);
        }
    }

    // Morton-sort the roots by rest position: surface-major, locality-
    // preserving chunk placement (plan/04 :641, ADR §4.1). cellScale 64
    // cells/unit is an M1 choice — the plan does not pin a cell size; ties
    // keep the deterministic face-major emission order (stable_sort), so the
    // result is bit-stable across thread counts and chunk splits (E-8).
    const size_t N = aids.size();
    std::vector<uint64_t> morton(N);
    std::vector<size_t> order(N);
    for (size_t i = 0; i < N; ++i) {
        morton[i] = UsdGenMortonKey3(ax[i], ay[i], az[i], 64.0f);
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return morton[a] < morton[b]; });

    buf.totalCurves = uint32_t(N);
    buf.totalCvs = uint32_t(N);
    buf.px = VtFloatArray(N); buf.py = VtFloatArray(N); buf.pz = VtFloatArray(N);
    buf.curveId = VtArray<uint64_t>(N);
    buf.rootPrim = VtIntArray(N);
    buf.rootUV = VtVec2fArray(N);
    buf.rootT = VtVec3fArray(N); buf.rootN = VtVec3fArray(N); buf.rootB = VtVec3fArray(N);
    buf.hairT = VtFloatArray(N, 0.0f);
    for (size_t i = 0; i < N; ++i) {
        size_t const s = order[i];
        buf.px[i] = ax[s]; buf.py[i] = ay[s]; buf.pz[i] = az[s];
        buf.curveId[i] = aids[s];
        buf.rootPrim[i] = aPrim[s];
        buf.rootUV[i] = aUv[s];
        buf.rootT[i] = aT[s]; buf.rootN[i] = aN[s]; buf.rootB[i] = aB[s];
    }

    // Per-root mask resolved at capture (I4: never per frame; 02 §2.13) —
    // downstream ops read it via the curveMask plane.
    if (p) {
        buf.curveMask = EvaluateMask(m, buf.curveId, buf.px, {}, 0.0).curveMask;
    }
    else buf.curveMask = VtFloatArray();
    return true;
}

void UsdGenScatterOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &capture,
    UsdGenChunkView *view) const
{
    // A generator's Evaluate is identity: Capture filled the node buffer
    // (framework pre-copied the planes from the capture buffer).
    TF_UNUSED(ctx); TF_UNUSED(capture); TF_UNUSED(view);
}

}  // namespace usdGen
