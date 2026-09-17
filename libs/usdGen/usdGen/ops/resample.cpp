// usdGen — UsdGenResampleOp implementation. 02-schema.md §2.7.1, 04 §2.11.
// The only CvCount styler: Capture rebuilds the buffer topology (R curves x
// cvCount CVs, uniform offsets) and resamples the rest and width channels
// with the same weights Evaluate applies to points, so rest stays the
// resampled authored shape while points follow deformed inputs and live mask
// edits. Per-curve planes (curveId, root frames, ...) inherit generically in
// the scheduler, exactly like Grow's output.
#include "usdGen/ops/resample.h"

#include "usdGen/curveBuffer.h"
#include "usdGen/opParams.h"
#include "usdGen/ops/opUtil.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/diagnostic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

struct UsdGenResampleCapture final : public UsdGenCapturePayload
{
    int cvCount = 8;
    bool keepParam = false;
    bool restoreLengths = false;
    uint64_t upstreamTopologyVersion = 0;
    uint64_t upstreamValueVersion = 0;
    uint32_t upstreamCurves = 0;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenResampleCapture>(*this);
    }
    bool OwnsBuffer() const override { return true; }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.valueVersion == upstreamValueVersion &&
               upstream.totalCurves == upstreamCurves;
    }
};

// Nearest input-CV index for output j: parameter-space hold. Shared by the
// mask sampling and the hold blend so both agree on which input CV an
// output CV belongs to.
inline uint32_t HoldIndex(uint32_t j, uint32_t nOut, uint32_t nIn)
{
    if (nOut <= 1 || nIn == 0) return 0;
    uint32_t idx = static_cast<uint32_t>(
        std::llround(double(j) * double(nIn - 1) / double(nOut - 1)));
    return idx >= nIn ? nIn - 1 : idx;
}

}  // namespace

static TfTokenVector _topoParams = [] {
    TfTokenVector v = UsdGenBaseTopologyParams();
    v.push_back(TfToken("cvCount"));               // capture: output topology
    v.push_back(TfToken("distribution"));          // structural: kernel branch
    v.push_back(TfToken("restoreSegmentLengths")); // capture: output values
    return v;
}();

static TfTokenVector _valueParams = [] {
    TfTokenVector v = UsdGenBaseValueParams();
    v.push_back(TfToken("enabled"));
    v.push_back(TfToken("mask"));
    return v;
}();

TfSpan<const TfToken> UsdGenResampleOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenResampleOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
std::unique_ptr<UsdGenCapture> UsdGenResampleOp::CreateCapture() const
{
    return std::make_unique<UsdGenResampleCapture>();
}
uint32_t UsdGenResampleOp::PlanesTouched() const
{
    return kPlanePoints | kPlaneWidths | kPlaneHairT;
}

bool UsdGenResampleOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    int const cvCount = params.GetInt(sCvCount, 8);
    if (cvCount < 1 || cvCount > 64) {
        if (diag) diag->Error("UsdGenResample: usdGen:cvCount must be in [1, 64]");
        return false;
    }
    TfToken const distribution = params.GetToken(sDistribution, sUniform);
    if (distribution != sUniform && distribution != sKeepParam) {
        if (diag) diag->Error("UsdGenResample: unknown usdGen:distribution '" +
                              distribution.GetString() + "'");
        return false;
    }
    return true;
}

UsdGenEpoch UsdGenResampleOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    opUtil::Digest d;
    d.Mix(p ? uint64_t(p->GetInt(sCvCount, 8)) : 8u);
    d.Mix(p ? uint64_t(p->GetToken(sDistribution, sUniform).Hash()) : 0u);
    d.Mix(p && p->GetBool(sRestore, false) ? uint64_t(1) : uint64_t(0));
    d.Mix(uint64_t(ctx.seed));
    d.Mix(ctx.upstreamGeneration);
    return d.Epoch(0x5265736D50ULL);
}

bool UsdGenResampleOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    UsdGenParamView const *p = ctx.params;
    auto &cap = *static_cast<UsdGenResampleCapture *>(out);
    auto fail = [&](std::string const &message) {
        if (diag) diag->Error("UsdGenResample: " + message);
        return false;
    };
    int cvCount = 8;
    bool keepParam = false, restore = false;
    if (p) {
        cvCount = p->GetInt(sCvCount, 8);
        if (cvCount < 1 || cvCount > 64)
            return fail("usdGen:cvCount must be in [1, 64]");
        TfToken const distribution = p->GetToken(sDistribution, sUniform);
        if (distribution != sUniform && distribution != sKeepParam)
            return fail("unknown usdGen:distribution '" + distribution.GetString() + "'");
        keepParam = distribution == sKeepParam;
        restore = p->GetBool(sRestore, false);
    }
    cap.cvCount = cvCount;
    cap.keepParam = keepParam;
    cap.restoreLengths = restore;

    uint32_t const R = upstream.totalCurves;
    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamValueVersion = upstream.valueVersion;
    cap.upstreamCurves = upstream.totalCurves;

    std::vector<uint32_t> spans;
    std::string error;
    if (!opUtil::CurveSpans(upstream, &spans, &error))
        return fail("strand topology: " + error);
    for (uint32_t c = 0; c != R; ++c) {
        if (spans[c + 1] == spans[c])
            return fail("cannot resample an empty upstream curve");
    }

    UsdGenCurveBuffer &buf = cap.MutableBuffer();
    if (R > std::numeric_limits<uint32_t>::max() / uint32_t(cvCount))
        return fail("CV topology exceeds uint32 cardinality");
    buf.totalCurves = R;
    buf.totalCvs = R * uint32_t(cvCount);
    buf.topologyVersion = upstream.topologyVersion + 1;
    buf.cvOffsets.clear();  // fixed cvCount: the output is always uniform.

    // Named planes follow the new cardinality (the scheduler keeps these
    // private owners instead of re-aliasing upstream descriptors).
    std::string planeError;
    if (!UsdGenResampleExtraPlanes(upstream, &buf, &planeError))
        return fail("extra planes: " + planeError);

    // Widths resample with the same weights as points; without an upstream
    // plane the description fallback fills the new span (Grow's rule).
    float fallbackWidth = 0.01f;
    if (ctx.desc) {
        if (!std::isfinite(ctx.desc->defaultWidth) || ctx.desc->defaultWidth < 0.0f)
            return fail("description defaultWidth must be finite and >= 0");
        fallbackWidth = ctx.desc->defaultWidth;
    }
    bool const hasWidths = !upstream.width.empty();
    if (hasWidths && upstream.width.size() != upstream.totalCvs)
        return fail("upstream width plane cardinality does not match its CV topology");
    VtFloatArray resampledWidths(buf.totalCvs, fallbackWidth);
    VtVec3fArray resampledRest(buf.totalCvs);
    bool const hasRest = upstream.rest.size() == upstream.totalCvs;
    if (upstream.px.size() != upstream.totalCvs ||
        upstream.py.size() != upstream.totalCvs ||
        upstream.pz.size() != upstream.totalCvs)
        return fail("upstream point plane cardinality does not match its CV topology");

    std::vector<float> cumlen;
    for (uint32_t c = 0; c != R; ++c) {
        uint32_t const first = spans[c], nIn = spans[c + 1] - first;
        // Arc table over the REST shape: rest resamples by arc length so the
        // rest channel stays the authored shape under deformation.
        cumlen.assign(nIn, 0.0f);
        auto restAt = [&](uint32_t i) -> GfVec3f {
            return hasRest ? upstream.rest[first + i]
                : GfVec3f(upstream.px[first + i], upstream.py[first + i],
                          upstream.pz[first + i]);
        };
        for (uint32_t i = 1; i < nIn; ++i)
            cumlen[i] = cumlen[i - 1] + (restAt(i) - restAt(i - 1)).GetLength();
        float const total = nIn ? cumlen[nIn - 1] : 0.0f;
        for (int j = 0; j < cvCount; ++j) {
            float const u = cvCount > 1 ? float(j) / float(cvCount - 1) : 0.0f;
            uint32_t lo = 0, hi = 0;
            float alpha = 0.0f;
            if (keepParam || total <= 0.0f || nIn < 2) {
                float const f = u * float(nIn - 1);
                lo = uint32_t(f);
                if (lo >= nIn - 1) { lo = nIn - 1; hi = nIn - 1; alpha = 0.0f; }
                else { hi = lo + 1; alpha = f - float(lo); }
            } else {
                float const s = u * total;
                lo = 0;
                while (lo + 1 < nIn && cumlen[lo + 1] < s) ++lo;
                hi = lo + 1 < nIn ? lo + 1 : lo;
                float const segLen = hi > lo ? cumlen[hi] - cumlen[lo] : 0.0f;
                alpha = segLen > 0.0f ? (s - cumlen[lo]) / segLen : 0.0f;
            }
            GfVec3f const a = restAt(lo), b = restAt(hi);
            resampledRest[size_t(c) * size_t(cvCount) + size_t(j)] =
                a + (b - a) * alpha;
            if (hasWidths) {
                float const wa = upstream.width[first + lo];
                float const wb = upstream.width[first + hi];
                resampledWidths[size_t(c) * size_t(cvCount) + size_t(j)] =
                    wa + (wb - wa) * alpha;
            }
        }
    }
    buf.width = std::move(resampledWidths);
    buf.rest = std::move(resampledRest);
    return true;
}

void UsdGenResampleOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &captureIn,
    UsdGenChunkView *view) const
{
    auto const &cap = static_cast<UsdGenResampleCapture const &>(captureIn);
    UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
    UsdGenParamField const maskField =
        p ? p->GetScalarField(sMask, 1.0) : UsdGenParamField{1.0};
    int const N = cap.cvCount;
    float *px = view->px, *py = view->py, *pz = view->pz;
    auto const *inPx = view->inPx, *inPy = view->inPy, *inPz = view->inPz;
    float *hairT = view->hairT;
    size_t const curveBase = view->desc ? size_t(view->desc->firstCurve) : 0;
    thread_local std::vector<float> tCum;
    thread_local std::vector<float> tScratch;

    for (uint32_t c = 0; c < view->curveCount; ++c) {
        uint32_t const nIn = view->inCvOffsets
            ? uint32_t(view->inCvOffsets[c + 1] - view->inCvOffsets[c])
            : view->inCvCount;
        uint32_t const gIn = view->inCvOffsets
            ? uint32_t(view->inCvOffsets[c] - int(view->inFirstCv))
            : c * view->inCvCount;
        size_t const gOut = view->Cv(c, 0);
        if (nIn == 0) continue;  // cannot happen: Capture rejects empty curves.
        size_t const curve = curveBase + c;
        // Absolute input CV base for mask sampling: inCvOffsets are absolute
        // in the upstream array; the uniform path derives it from the curve.
        size_t const absInBase = view->inCvOffsets
            ? size_t(view->inCvOffsets[c])
            : curve * size_t(view->inCvCount);
        tCum.assign(nIn, 0.0f);
        for (uint32_t i = 1; i < nIn; ++i) {
            float const dx = inPx[gIn + i] - inPx[gIn + i - 1];
            float const dy = inPy[gIn + i] - inPy[gIn + i - 1];
            float const dz = inPz[gIn + i] - inPz[gIn + i - 1];
            tCum[i] = tCum[i - 1] + std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        float const inLen = tCum[nIn - 1];
        // Pass one: resampled positions into scratch (restore needs the
        // output length before placing anything).
        if (tScratch.size() < size_t(N) * 3) tScratch.resize(size_t(N) * 3);
        for (int j = 0; j < N; ++j) {
            float const u = N > 1 ? float(j) / float(N - 1) : 0.0f;
            uint32_t lo = 0, hi = 0;
            float alpha = 0.0f;
            if (cap.keepParam || inLen <= 0.0f || nIn < 2) {
                float const f = u * float(nIn - 1);
                lo = uint32_t(f);
                if (lo >= nIn - 1) { lo = nIn - 1; hi = nIn - 1; alpha = 0.0f; }
                else { hi = lo + 1; alpha = f - float(lo); }
            } else {
                float const s = u * inLen;
                lo = 0;
                while (lo + 1 < nIn && tCum[lo + 1] < s) ++lo;
                hi = lo + 1 < nIn ? lo + 1 : lo;
                float const segLen = hi > lo ? tCum[hi] - tCum[lo] : 0.0f;
                alpha = segLen > 0.0f ? (s - tCum[lo]) / segLen : 0.0f;
            }
            uint32_t const a = gIn + lo, b = gIn + hi;
            tScratch[size_t(j) * 3] = inPx[a] + (inPx[b] - inPx[a]) * alpha;
            tScratch[size_t(j) * 3 + 1] = inPy[a] + (inPy[b] - inPy[a]) * alpha;
            tScratch[size_t(j) * 3 + 2] = inPz[a] + (inPz[b] - inPz[a]) * alpha;
        }
        float rescale = 1.0f;
        if (cap.restoreLengths && inLen > 0.0f && N > 1) {
            float outLen = 0.0f;
            for (int j = 1; j < N; ++j) {
                float const dx = tScratch[size_t(j) * 3] - tScratch[size_t(j - 1) * 3];
                float const dy = tScratch[size_t(j) * 3 + 1] - tScratch[size_t(j - 1) * 3 + 1];
                float const dz = tScratch[size_t(j) * 3 + 2] - tScratch[size_t(j - 1) * 3 + 2];
                outLen += std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            if (outLen > 0.0f) rescale = inLen / outLen;
        }
        float const rx = tScratch[0], ry = tScratch[1], rz = tScratch[2];
        for (int j = 0; j < N; ++j) {
            float const u = N > 1 ? float(j) / float(N - 1) : 0.0f;
            uint32_t const hold = HoldIndex(uint32_t(j), uint32_t(N), nIn);
            float const mask = std::clamp(
                static_cast<float>(maskField.Value(curve, absInBase + hold)),
                0.0f, 1.0f);
            float const sx = rx + (tScratch[size_t(j) * 3] - rx) * rescale;
            float const sy = ry + (tScratch[size_t(j) * 3 + 1] - ry) * rescale;
            float const sz = rz + (tScratch[size_t(j) * 3 + 2] - rz) * rescale;
            uint32_t const h = gIn + hold;
            size_t const o = gOut + size_t(j);
            px[o] = inPx[h] + (sx - inPx[h]) * mask;
            py[o] = inPy[h] + (sy - inPy[h]) * mask;
            pz[o] = inPz[h] + (sz - inPz[h]) * mask;
            if (hairT) hairT[o] = u;
        }
    }
}

}  // namespace usdGen
