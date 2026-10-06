// usdGen — UsdGenScatterOp implementation (M1, mode="random").
//
// 02-schema.md §2.6, 04-operators.md :620-676. Area-weighted scatter:
//   n_f = floor(expected) + (Hash01(Hash64(seed, f, kSaltScatter),
//                                    kSaltScatter) < frac(expected))
//   expected = density * areaRest(f)                        (plan/04 :634-635)
// Curve ids are UsdGenCurveId(seed, faceIndex, k) (plan/04 :637) so they stay
// stable across density edits (plan/04 :663).
// Each root position is drawn from three curveId-keyed draws (area-weighted
// fan-triangle pick + uniform in-triangle point): the M1 realization of the
// "low-discrepancy sample seeded by curveId" (plan/04 :638) — being purely
// curveId-keyed it is order-invariant across thread counts and chunk splits
// (review M-3 / engine rule E-8). Roots are Morton-sorted by rest position so
// chunk placement is surface-major (plan/04 :641).
#include "usdGen/ops/scatter.h"

#include "usdGen/opParams.h"
#include "usdGen/digest.h"
#include "usdGen/limitSurface.h"
#include "usdGenMath/usdGenMath/hash.h"
#include "usdGenMath/usdGenMath/kernels.h"

#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
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
    v.push_back(TfToken("density"));     // capture (02 §6.4)
    v.push_back(TfToken("flip"));
    v.push_back(TfToken("subdivisionLevel"));
    return v;
}();

// Scatter is a generator: it has no upstream curves to leave untouched, so it
// declares no usdGen:mask.
static TfTokenVector _valueParams = [] {
    return UsdGenBaseValueParams();
}();

TfSpan<const TfToken> UsdGenScatterOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenScatterOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
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
    int const level=params.GetInt(TfToken("subdivisionLevel"),0);
    if(level<0 || level>6) {
        if(diag)diag->Error("UsdGenScatter: subdivisionLevel must be in [0,6]");
        return false;
    }
    double const density = params.GetDouble(TfToken("density"), 100.0);
    if (!std::isfinite(density) || density < 0.0) {
        if (diag) diag->Error("UsdGenScatter: density must be finite and >= 0");
        return false;
    }
    return true;
}

UsdGenEpoch UsdGenScatterOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    // Scatter consumes only rest geometry. The general surfaceGeneration
    // includes posed points, shutter samples and world transforms, so it
    // would regenerate rest roots on every animation frame.
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    uint64_t h = 1469598103934665603ULL;
    auto feed = [&](std::string const &k, uint64_t v) {
        h ^= v; h *= 0x100000001b3ULL;
        h ^= k.size(); h *= 0x100000001b3ULL;
    };
    feed("density", p ? double_as_bits(p->GetDouble(TfToken("density"), 100.0)) : 0);
    feed("flip", p && p->GetBool(TfToken("flip"), false) ? 1u : 0u);
    feed("seed", ctx.seed);
    feed("subdivisionLevel", p ? p->GetInt(TfToken("subdivisionLevel"),0) : 0);
    UsdGenGraphDesc const *desc = ctx.desc;
    if (desc && ctx.surface < desc->surfaces.size()) {
        auto const &surface = desc->surfaces[ctx.surface];
        auto array = [&](char const *name, auto const &values) {
            uint64_t const hash = UsdGenDigestBytes(
                values.cdata(), values.size() * sizeof(*values.cdata()),
                1469598103934665603ULL);
            feed(name, hash); feed(name, uint64_t(values.size()));
        };
        array("restPoints", surface.restPoints);
        array("faceCounts", surface.faceVertexCounts);
        array("faceIndices", surface.faceVertexIndices);
        array("subset", surface.subsetFaces);
        feed("isSubset", surface.isSubset ? 1u : 0u);
        array("uv", surface.uv);
        feed("subdivision", UsdGenSubdivisionDigest(surface));
        // The paint primvar edits no generation, so the multiplier content
        // itself joins the digest: without this a paint stroke would read
        // back the cached pre-stroke roots.
        auto const &mult = desc->surfaces[ctx.surface].densityMultiplier;
        feed("densityMultSize", uint64_t(mult.size()));
        static_assert(sizeof(float) == 4, "float is 32 bits");
        uint64_t const mh = UsdGenDigestBytes(
            mult.cdata(), mult.size() * sizeof(float),
            1469598103934665603ULL);
        feed("densityMult", mh);
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
    int const level=p ? p->GetInt(TfToken("subdivisionLevel"),0) : 0;
    UsdGenLimitSurface limit;
    if(level) {
        std::string error;
        if(!limit.Build(surf,level,&error)) {
            if(diag)diag->Error("UsdGenScatter: "+error);
            return false;
        }
    }
    if (surf.faceVertexCounts.empty() || surf.restPoints.empty()) {
        if (diag) diag->Warn(std::string("UsdGenScatter::Capture: surface '") +
                             surf.path.GetText() + "' has no topology; 0 roots");
    }

    // Face list: whole mesh or the GeomSubset restriction (R15). An empty
    // subset selects no face. Unrestricted capture walks faces 0..F-1
    // directly (f == fi) instead of materializing the identity table.
    bool const restricted = UsdGenSurfaceRestricted(surf);
    std::vector<int> faces;
    if (restricted) faces.assign(surf.subsetFaces.cbegin(), surf.subsetFaces.cend());
    size_t const faceCount =
        restricted ? faces.size() : surf.faceVertexCounts.size();

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
    // fvi[cornerOff[f] .. cornerOff[f] + fvc[f]). Unrestricted faces run in
    // order, so the loop below accumulates the running base instead of the
    // table; the table is built only for subset (out-of-order) capture.
    std::vector<size_t> cornerOff;
    if (restricted) {
        cornerOff.assign(surf.faceVertexCounts.size() + 1, 0);
        for (size_t f = 0; f < surf.faceVertexCounts.size(); ++f)
            cornerOff[f + 1] = cornerOff[f] + size_t(fvc[f]);
    }

    const double density = p ? p->GetDouble(TfToken("density"), 100.0) : 100.0;
    if (!std::isfinite(density) || density < 0.0) {
        if (diag) diag->Error("UsdGenScatter::Capture: density must be finite and >= 0");
        return false;
    }
    // Per-face density scale from usdGen:paint:density (the brush); empty
    // == all 1.0. Sized once here; the loop below only range-checks values.
    bool const hasMult = !surf.densityMultiplier.empty();
    if (hasMult &&
        surf.densityMultiplier.size() != surf.faceVertexCounts.size()) {
        if (diag) diag->Error("UsdGenScatter::Capture: density multiplier "
                              "has the wrong face count");
        return false;
    }
    const bool flip = p ? p->GetBool(TfToken("flip"), false) : false;

    // Hoisted SplitMix seed hashes (bit-identical CSE): every per-face and
    // per-root hash below folds ctx.seed through UsdGenHash64(seed32, salt)
    // first, and ctx.seed is loop-invariant, so the seed folding is computed
    // once per Capture. Integer-only: no FP association changes anywhere.
    // The uint32_t(int(ctx.seed)) round-trip reproduces ctx.seed exactly, so
    // hSeedScatter matches both the faceKey spelling (which folds
    // uint32_t(ctx.seed)) and the CurveId/Draw01 spellings (which fold
    // uint32_t of the int seed).
    uint64_t const seed32 = uint64_t(uint32_t(int(ctx.seed)));
    uint64_t const hSeedScatter = UsdGenHash64(seed32, kSaltScatter);
    uint64_t const hSeedBary0 = UsdGenHash64(seed32, kSaltScatterBary);
    uint64_t const hSeedBary1 = UsdGenHash64(seed32, kSaltScatterBary + 1u);
    uint64_t const hSeedBary2 = UsdGenHash64(seed32, kSaltScatterBary + 2u);

    // Per-face area-weighted emission (plan/04 :634-635).
    std::vector<float> ax, ay, az;
    std::vector<uint64_t> aids;
    std::vector<int> aPrim;
    std::vector<GfVec2f> aUv;
    std::vector<GfVec3f> aT, aN, aB;
    ax.reserve(faceCount * 4); ay.reserve(faceCount * 4); az.reserve(faceCount * 4);
    aids.reserve(faceCount * 4); aPrim.reserve(faceCount * 4);
    aUv.reserve(faceCount * 4); aT.reserve(faceCount * 4);
    aN.reserve(faceCount * 4); aB.reserve(faceCount * 4);
    // Per-face triangle weights: stack-backed for the common small fan
    // (quads need 2), spilling to a reused vector past 64 triangles or on
    // the subdivision path. Same values, same order, no per-face vector
    // traffic on the hot path.
    float triStack[64];
    std::vector<float> triSpill;
    triSpill.reserve(16);
    size_t runBase = 0;
    for (size_t fi = 0; fi < faceCount; ++fi) {
        int const f = restricted ? faces[fi] : int(fi);
        if (f < 0 || size_t(f) >= surf.faceVertexCounts.size()) continue;  // bad subset
        // Running corner base (unrestricted): fvc[f] is valid here (the
        // range check passed), and this advance runs for every face —
        // ahead of the skips below — so runBase == cornerOff[fi] exactly.
        size_t cbase;
        if (restricted) cbase = cornerOff[f];
        else {
            cbase = runBase;
            runBase += size_t(fvc[f]);
        }
        if(level && limit.IsHole(f)) continue;
        int const nc = fvc[f];
        if (nc < 3) continue;
        bool bad = false;
        for (int i = 0; i < nc; ++i)
            if (fvi[cbase + size_t(i)] < 0 ||
                size_t(fvi[cbase + size_t(i)]) >= surf.restPoints.size()) { bad = true; break; }
        if (bad) continue;  // malformed face; skip rather than read out of bounds
        // Fan triangulation of face f (the same decomposition
        // UsdGenPolygonRestArea uses): triW[t] is the area of the triangle
        // rest[c0], rest[c(t+1)], rest[c(t+2)], nAcc accumulates the face
        // normal. areaRest(f) = UsdGenPolygonRestArea (plan/04 :634); the
        // float sum below equals it up to fp-associativity and is used for the
        // sampling weights so the cumulative weights sum exactly to the
        // denominator.
        GfVec3f const p0 = rest[fvi[cbase]];
        size_t const nFan = size_t(nc) - 2;
        float *triW = triStack;
        size_t ntri = nFan;
        if (nFan > 64) {
            triSpill.assign(nFan, 0.0f);
            triW = triSpill.data();
        }
        double areaRest = 0.0;
        GfVec3f nAcc(0.0f, 0.0f, 0.0f);
        for (int t = 1; t + 1 < nc; ++t) {
            GfVec3f const pb = rest[fvi[cbase + size_t(t)]];
            GfVec3f const pc = rest[fvi[cbase + size_t(t) + 1]];
            float const w = UsdGenTriangleArea(
                p0[0], p0[1], p0[2], pb[0], pb[1], pb[2], pc[0], pc[1], pc[2]);
            triW[size_t(t) - 1] = w;
            areaRest += double(w);
            nAcc += GfCross(pb - p0, pc - p0);
        }
        GfVec3f const Nrest = Normalize3(nAcc);
        // Area quadrature uses a 2^level grid on each coarse patch. Actual
        // roots and frames are evaluated on the bicubic/Gregory limit patch,
        // never on these quadrature triangles. Keep coarse face IDs for Ptex.
        int const grid=level ? (1<<level) : 0;
        if(level) {
            triSpill.clear();areaRest=0;
            std::vector<GfVec3f> samples((grid+1)*(grid+1));
            GfVec3f du,dv;
            for(int y=0;y<=grid;++y) for(int x=0;x<=grid;++x)
                if(!limit.Evaluate(f,float(x)/grid,float(y)/grid,&samples[y*(grid+1)+x],&du,&dv)) {
                    if(diag)diag->Error("UsdGenScatter: limit patch evaluation failed");
                    return false;
                }
            for(int y=0;y<grid;++y) for(int x=0;x<grid;++x) {
                auto const& a=samples[y*(grid+1)+x];auto const& b=samples[y*(grid+1)+x+1];
                auto const& c=samples[(y+1)*(grid+1)+x+1];auto const& d=samples[(y+1)*(grid+1)+x];
                float a0=.5f*GfCross(b-a,c-a).GetLength(),a1=.5f*GfCross(c-a,d-a).GetLength();
                triSpill.push_back(a0);triSpill.push_back(a1);areaRest+=double(a0)+double(a1);
            }
            triW = triSpill.data(); ntri = triSpill.size();
        }

        double mult = 1.0;
        if (hasMult) {
            mult = double(surf.densityMultiplier[size_t(f)]);
            if (!std::isfinite(mult) || mult < 0.0) {
                if (diag) diag->Error("UsdGenScatter::Capture: density "
                                      "multiplier must be finite and >= 0");
                return false;
            }
        }
        double const expected = density * areaRest * mult;
        if (!std::isfinite(areaRest) || !std::isfinite(expected)) {
            if (diag) diag->Error("UsdGenScatter::Capture: non-finite root count on face " +
                                  std::to_string(f));
            return false;
        }
        if (!(expected > 0.0)) continue;
        double const whole = std::floor(expected);
        double const frac = expected - whole;
        if (whole > double(std::numeric_limits<uint32_t>::max())) {
            if (diag) diag->Error("UsdGenScatter::Capture: root count exceeds uint32 "
                                  "cardinality on face " + std::to_string(f));
            return false;
        }
        // UsdGenHash64(seed32, f32, kSaltScatter) with the hoisted seed
        // fold: Hash64(Hash64(seed32, salt) ^ f32, salt), verbatim.
        uint64_t const faceKey = UsdGenHash64(
            hSeedScatter ^ uint64_t(uint32_t(f)), kSaltScatter);
        uint64_t const nf = uint64_t(whole)
                          + (UsdGenHash01(faceKey, kSaltScatter) < float(frac) ? 1u : 0u);
        size_t const maxCurves = std::numeric_limits<uint32_t>::max();
        if (aids.size() > maxCurves || nf > maxCurves - aids.size()) {
            if (diag) diag->Error("UsdGenScatter::Capture: total root count exceeds "
                                  "uint32 cardinality at face " + std::to_string(f));
            return false;
        }

        for (uint64_t k = 0; k < nf; ++k) {
            // UsdGenCurveId(seed, f, k) is Hash64(faceKey ^ k, salt): faceKey
            // above is exactly the id's documented inner fold, so one
            // finalizer replaces three. The draws likewise reuse the hoisted
            // seed folds via the Hash01 spelling Draw01 expands to.
            uint64_t const curveId = UsdGenHash64(
                faceKey ^ uint64_t(uint32_t(k)), kSaltScatter);
            // "low-discrepancy sample seeded by curveId" (plan/04 :638),
            // realized in M1 as: area-weighted fan-triangle pick + uniform
            // in-triangle point, three draws keyed only by curveId.
            float const u0 = UsdGenHash01(hSeedBary0 ^ curveId, kSaltScatterBary);
            float const u1 = UsdGenHash01(hSeedBary1 ^ curveId, kSaltScatterBary + 1u);
            float const u2 = UsdGenHash01(hSeedBary2 ^ curveId, kSaltScatterBary + 2u);
            size_t ti = ntri - 1;
            {
                double cum = 0.0;
                double const target = double(u0) * areaRest;
                for (size_t t = 0; t < ntri; ++t) {
                    cum += double(triW[t]);
                    if (target < cum) { ti = t; break; }
                }
            }
            GfVec3f pos,T,N=Nrest;
            GfVec2f puv;
            if(level) {
                int const cell=int(ti/2),x=cell%grid,y=cell/grid;
                GfVec2f a(float(x)/grid,float(y)/grid);
                GfVec2f b(float(x+1)/grid,float(y+(ti%2 ? 1:0))/grid);
                GfVec2f c(float(x+(ti%2 ? 0:1))/grid,float(y+1)/grid);
                float const r=std::sqrt(u1);
                puv=a*(1-r)+b*(u2*r)+c*((1-u2)*r);
                GfVec3f du,dv;
                if(!limit.Evaluate(f,puv[0],puv[1],&pos,&du,&dv)) return false;
                N=Normalize3(GfCross(du,dv));
                if(surf.orientation==TfToken("leftHanded")) N=-N;
                T=Normalize3(du-N*GfDot(du,N));
            } else {
            size_t const ib = cbase + ti + 1;
            size_t const ic = ib + 1;
            GfVec3f const pb = rest[fvi[ib]];
            GfVec3f const pc = rest[fvi[ic]];
            float const r1 = std::sqrt(u1);
            pos = p0 * (1.0f - r1) + pb * (u2 * r1) + pc * (r1 * (1.0f - u2));
            puv=GfVec2f(1.0f / 3.0f, 1.0f / 3.0f);
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
            T = e0 - Nrest * GfDot(e0, Nrest);
            // Fuse the degenerate-length check with the normalize: T's
            // GetLengthSq is (T*T) and GetLength is its sqrt, so one
            // sqrt feeds both the 1e-9f check and the division. The hot
            // path (len >= 1e-9f) takes Normalize3's division branch
            // (len is far above its 1e-12f floor) with the identical
            // divisor; the cold path runs the original spelling verbatim.
            float tLen = GfSqrt(T * T);
            if (tLen < 1e-9f) {
                T = std::abs(Nrest[0]) > 0.9f ? GfVec3f(0.0f, 1.0f, 0.0f)
                                             : GfVec3f(1.0f, 0.0f, 0.0f);
                T = T - Nrest * GfDot(T, Nrest);
                T = Normalize3(T);
            } else {
                T = T / tLen;
            }
            }
            GfVec3f B = Normalize3(GfCross(N, T));
            if (flip) { T = -T; B = -B; }  // rest frame handedness (02 §2.6)
            ax.push_back(pos[0]); ay.push_back(pos[1]); az.push_back(pos[2]);
            aids.push_back(curveId);
            aPrim.push_back(f);
            aUv.push_back(puv);
            aT.push_back(T); aN.push_back(N); aB.push_back(B);
        }
    }

    // Morton-sort the roots by rest position: surface-major, locality-
    // preserving chunk placement (plan/04 :641, ADR §4.1). cellScale 64
    // cells/unit is an M1 choice — the plan does not pin a cell size; ties
    // keep the deterministic face-major emission order (stable passes), so
    // the result is bit-stable across thread counts and chunk splits (E-8).
    // LSD radix over 16-bit digits: each pass is stable and the digits run
    // least- to most-significant, so the final order is exactly the
    // stable_sort order (primary morton key, ties in emission order) at
    // linear cost. The even pass count lands the result back in the
    // morton/order pair.
    const size_t N = aids.size();
    std::vector<uint64_t> morton(N);
    std::vector<size_t> order(N);
    for (size_t i = 0; i < N; ++i) {
        morton[i] = UsdGenMortonKey3(ax[i], ay[i], az[i], 64.0f);
        order[i] = i;
    }
    if (N > 1) {
        std::vector<uint64_t> tmpKeys(N);
        std::vector<size_t> tmpIdx(N);
        std::vector<size_t> counts(65536);
        uint64_t *keys = morton.data();
        size_t *idx = order.data();
        uint64_t *keysOut = tmpKeys.data();
        size_t *idxOut = tmpIdx.data();
        for (int pass = 0; pass < 4; ++pass) {
            std::fill(counts.begin(), counts.end(), size_t(0));
            int const shift = pass * 16;
            for (size_t i = 0; i < N; ++i)
                ++counts[(keys[i] >> shift) & 0xffffu];
            size_t sum = 0;
            for (size_t c = 0; c < 65536; ++c) {
                size_t const t = counts[c];
                counts[c] = sum;
                sum += t;
            }
            for (size_t i = 0; i < N; ++i) {
                size_t const d = (keys[i] >> shift) & 0xffffu;
                size_t const p = counts[d]++;
                keysOut[p] = keys[i];
                idxOut[p] = idx[i];
            }
            std::swap(keys, keysOut);
            std::swap(idx, idxOut);
        }
    }

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
