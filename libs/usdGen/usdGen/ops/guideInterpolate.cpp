// usdGen — UsdGenGuideInterpolateOp implementation. 04-operators.md §2.3.
//
// Capture (the whole result, like Clump; the guide set is a reference input
// whose identity already re-runs it):
//   * every guide is resampled to usdGen:cvCount CVs by arc length and kept
//     as root-local offsets (linearBlend) or segment vectors
//     (extrudeAndBlend);
//   * a guide's root normal is its authored usdGen:rootFrame, else the rest
//     normal of the strand rooted nearest it (the surface frame there);
//   * a guide's region is the region value of the strand rooted nearest it,
//     so one connected field (usdGen:region) regions both. A voronoi
//     expression seeded by the guide roots therefore gives every guide its
//     own cell, and a ptex region map gives each guide the region it sits in;
//   * per strand, the nearest guides within usdGen:influenceRadius, of its own
//     region (others scaled by usdGen:regionCrossover), within
//     usdGen:maxGuideAngle, weighted (1 - d/R)^influenceDecay, the heaviest
//     usdGen:maxGuides kept and normalised. A strand with no candidate follows
//     the nearest guide of its region regardless of distance; a region with
//     no guide falls back to the nearest guide overall and is reported.
//   * shapes blend in skin space: each guide's offsets are turned by the
//     rotation taking its root normal onto the strand's (scaled by
//     usdGen:blendInSkinSpace) before weighting.
// Evaluate writes the captured strands, following their roots.
#include "usdGen/ops/guideInterpolate.h"

#include "usdGen/clumpMotion.h"
#include "usdGen/opParams.h"
#include "usdGen/ops/opUtil.h"
#include "usdGen/ops/regionMap.h"
#include "usdGen/scheduler.h"
#include "usdGenMath/usdGenMath/hash.h"

#include <nanoflann.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

const TfToken sCvCount{"cvCount"}, sMaxGuides{"maxGuides"}, sRadius{"influenceRadius"},
    sDecay{"influenceDecay"}, sMaxAngle{"maxGuideAngle"}, sBlendMethod{"blendMethod"},
    sSkinSpace{"blendInSkinSpace"}, sUnique{"useUniqueGuide"}, sRandomize{"randomizeGuide"},
    sCrossover{"regionCrossover"}, sRegion{"region"}, sLinear{"linearBlend"},
    sExtrude{"extrudeAndBlend"}, sGuideIndex{"guideIndex"}, sGuideWeight{"guideWeight"};

constexpr uint32_t kSaltGuide = 0x47554944u;   // "GUID"
constexpr size_t kMaxCandidates = 32;

struct UsdGenGuideInterpolateCapture final : public UsdGenCapture
{
    uint32_t cvCount = 0;
    std::vector<float> result;   // 3 * curves * cvCount, rooted at `roots`
    std::vector<float> roots;    // 3 * curves
    uint64_t upstreamTopologyVersion = 0;
    uint64_t upstreamValueVersion = 0;
    uint32_t upstreamCurves = 0;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenGuideInterpolateCapture>(*this);
    }
    bool OwnsBuffer() const override { return true; }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.valueVersion == upstreamValueVersion &&
               upstream.totalCurves == upstreamCurves;
    }
};

struct Cloud {
    std::vector<float> points;
    size_t kdtree_get_point_count() const { return points.size() / 3; }
    float kdtree_get_pt(size_t i, size_t d) const { return points[i * 3 + d]; }
    template <class B> bool kdtree_get_bbox(B &) const { return false; }
};
using CloudIndex = nanoflann::KDTreeSingleIndexAdaptor<
    nanoflann::L2_Simple_Adaptor<float, Cloud>, Cloud, 3, uint32_t>;

template <class F>
void ParallelFor(UsdGenWorkDispatcher *dispatcher, size_t count, F const &body)
{
    if (!dispatcher || count < 256) {
        for (size_t i = 0; i < count; ++i) body(i);
        return;
    }
    struct Payload { F const *body; } payload{&body};
    dispatcher->ParallelFor(count, [](size_t i, void *p) {
        (*static_cast<Payload *>(p)->body)(i);
    }, &payload);
}

struct Settings {
    int cvCount = 8, maxGuides = 3;
    double radius = 4.0, decay = 2.0, maxAngle = 90.0, skin = 1.0, randomize = 0.0,
           crossover = 0.0;
    bool unique = false, extrude = false;
};

bool ReadSettings(UsdGenParamView const *p, Settings *s, UsdGenDiagnostics *diag)
{
    auto fail = [&](char const *message) {
        if (diag) diag->Error(std::string("UsdGenGuideInterpolate: ") + message);
        return false;
    };
    if (!p) return true;
    s->cvCount = p->GetInt(sCvCount, 8);
    s->maxGuides = p->GetInt(sMaxGuides, 3);
    s->radius = p->GetDouble(sRadius, 4.0);
    s->decay = p->GetDouble(sDecay, 2.0);
    s->maxAngle = p->GetDouble(sMaxAngle, 90.0);
    s->skin = p->GetDouble(sSkinSpace, 1.0);
    s->randomize = p->GetDouble(sRandomize, 0.0);
    s->crossover = p->GetDouble(sCrossover, 0.0);
    s->unique = p->GetBool(sUnique, false);
    TfToken const method = p->GetToken(sBlendMethod, sLinear);
    if (method != sLinear && method != sExtrude)
        return fail("usdGen:blendMethod must be linearBlend or extrudeAndBlend");
    s->extrude = method == sExtrude;
    if (s->cvCount < 2 || s->cvCount > 64) return fail("usdGen:cvCount must be in [2, 64]");
    if (s->maxGuides < 1 || s->maxGuides > 8) return fail("usdGen:maxGuides must be in [1, 8]");
    if (!std::isfinite(s->radius) || s->radius <= 0.0)
        return fail("usdGen:influenceRadius must be finite and > 0");
    if (!std::isfinite(s->decay) || s->decay < 0.0)
        return fail("usdGen:influenceDecay must be finite and >= 0");
    if (!(s->maxAngle >= 0.0 && s->maxAngle <= 180.0))
        return fail("usdGen:maxGuideAngle must be in [0, 180] degrees");
    if (!(s->skin >= 0.0 && s->skin <= 1.0) || !(s->randomize >= 0.0 && s->randomize <= 1.0) ||
        !(s->crossover >= 0.0 && s->crossover <= 1.0))
        return fail("usdGen:blendInSkinSpace, randomizeGuide and regionCrossover must be in [0, 1]");
    return true;
}

/// A guide resampled to `n` CVs by arc length, into `out` (xyz).
void Resample(float const *points, size_t count, size_t n, float *out)
{
    std::vector<double> arc(count, 0.0);
    for (size_t i = 1; i < count; ++i) {
        double const dx = double(points[i * 3]) - points[(i - 1) * 3];
        double const dy = double(points[i * 3 + 1]) - points[(i - 1) * 3 + 1];
        double const dz = double(points[i * 3 + 2]) - points[(i - 1) * 3 + 2];
        arc[i] = arc[i - 1] + std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    double const total = count ? arc.back() : 0.0;
    size_t segment = 0;
    for (size_t k = 0; k < n; ++k) {
        if (count < 2 || total <= 0.0) {
            for (int d = 0; d < 3; ++d) out[k * 3 + d] = count ? points[d] : 0.0f;
            continue;
        }
        double const target = total * double(k) / double(n - 1);
        while (segment + 2 < count && arc[segment + 1] < target) ++segment;
        double const span = arc[segment + 1] - arc[segment];
        double const f = span > 0.0 ? std::clamp((target - arc[segment]) / span, 0.0, 1.0) : 0.0;
        for (int d = 0; d < 3; ++d)
            out[k * 3 + d] = float(points[segment * 3 + d] +
                (double(points[(segment + 1) * 3 + d]) - points[segment * 3 + d]) * f);
    }
}

}  // namespace

static TfTokenVector _topoParams = [] {
    TfTokenVector v = UsdGenBaseTopologyParams();
    v.push_back(TfToken("enabled"));
    v.push_back(TfToken("guides"));
    v.push_back(TfToken("regionMap"));
    for (char const *name : {"cvCount", "maxGuides", "influenceRadius", "influenceDecay",
                             "maxGuideAngle", "blendMethod", "blendInSkinSpace",
                             "useUniqueGuide", "randomizeGuide", "regionCrossover", "region"})
        v.push_back(TfToken(name));
    return v;
}();

static TfTokenVector _valueParams = [] {
    return UsdGenBaseValueParams();
}();

static TfTokenVector const _referenceInputs{TfToken("guides")};

TfSpan<const TfToken> UsdGenGuideInterpolateOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenGuideInterpolateOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
TfSpan<const TfToken> UsdGenGuideInterpolateOp::ReferenceInputs() const
{
    return TfSpan<const TfToken>(_referenceInputs.data(), _referenceInputs.size());
}
std::unique_ptr<UsdGenCapture> UsdGenGuideInterpolateOp::CreateCapture() const
{
    return std::make_unique<UsdGenGuideInterpolateCapture>();
}
uint32_t UsdGenGuideInterpolateOp::PlanesTouched() const
{
    return kPlanePoints | kPlaneWidths | kPlaneHairT;
}

bool UsdGenGuideInterpolateOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    Settings settings;
    return ReadSettings(&params, &settings, diag);
}

UsdGenEpoch UsdGenGuideInterpolateOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    opUtil::Digest d;
    if (UsdGenParamView const *p = ctx.params) {
        for (TfToken const &name : _topoParams) {
            d.Mix(name);
            d.Mix(p->GetVtValue(name, VtValue()));
        }
    }
    d.Mix(uint64_t(ctx.seed));
    d.Mix(double(ctx.desc ? ctx.desc->defaultWidth : 0.01f));
    return d.Epoch(0x4775696465ull);
}

bool UsdGenGuideInterpolateOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    UsdGenParamView const *p = ctx.params;
    auto &cap = *static_cast<UsdGenGuideInterpolateCapture *>(out);
    Settings s;
    if (!ReadSettings(p, &s, diag)) return false;
    auto fail = [&](std::string const &message) {
        if (diag) diag->Error("UsdGenGuideInterpolate: " + message);
        return false;
    };
    if (ctx.referenceCount != 1 || !ctx.resolvedReferences || !ctx.resolvedReferences[0] ||
        !ctx.resolvedReferences[0]->value)
        return fail("usdGen:guides must target one guide curve set");
    UsdGenCurveBuffer const &guides = ctx.resolvedReferences[0]->value->buffer;
    UsdGenClumpMotion guideClumps;
    std::string clumpError;
    if (!UsdGenBuildClumpMotion(guides, &guideClumps, &clumpError))
        return fail("guide clump motion metadata " + clumpError);
    size_t const G = guides.totalCurves;
    if (G == 0) return fail("the guide curve set has no curves");

    std::vector<uint32_t> guideSpans, rootSpans;
    std::string error;
    if (!opUtil::CurveSpans(guides, &guideSpans, &error))
        return fail("guide topology: " + error);
    if (!opUtil::CurveSpans(upstream, &rootSpans, &error))
        return fail("root topology: " + error);
    if (guides.px.size() != guides.totalCvs || upstream.px.size() != upstream.totalCvs)
        return fail("point planes do not match their CV counts");

    size_t const R = upstream.totalCurves;
    size_t const n = size_t(s.cvCount);
    if (R > std::numeric_limits<uint32_t>::max() / n)
        return fail("CV topology exceeds uint32 cardinality");
    cap.cvCount = uint32_t(n);
    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamValueVersion = upstream.valueVersion;
    cap.upstreamCurves = upstream.totalCurves;

    // --- strand roots, frames and regions ------------------------------------
    std::vector<float> roots(R * 3), rest(R * 3);
    for (size_t c = 0; c < R; ++c) {
        GfVec3f const cur = opUtil::Point(upstream, rootSpans[c]);
        GfVec3f const r = opUtil::RestPoint(upstream, rootSpans[c]);
        for (int d = 0; d < 3; ++d) { roots[c * 3 + d] = cur[d]; rest[c * 3 + d] = r[d]; }
    }
    auto hairNormal = [&](size_t c) {
        return upstream.rootN.size() == R ? opUtil::Normalized(upstream.rootN[c])
                                          : GfVec3f(0, 1, 0);
    };
    UsdGenParamField const regionField = p ? p->GetScalarField(sRegion, 0.0) : UsdGenParamField{};
    std::vector<int64_t> hairRegion(R, 0);
    if (regionField.connected)
        for (size_t c = 0; c < R; ++c)
            hairRegion[c] = opUtil::RegionKey(regionField.Value(c, rootSpans[c]));
    bool mapped=false;std::vector<int> mapRegions;
    if (!UsdGenReadRootRegions(ctx,upstream,&mapRegions,&mapped,&error)) return fail(error);
    if (mapped) {
        if (s.crossover!=0) return fail("regionMap requires regionCrossover=0 for hard partitions");
        for (size_t c=0;c<R;++c) hairRegion[c]=opUtil::RegionKey(double(mapRegions[c]));
    }
    uint64_t const *ids = upstream.curveId.size() == R ? upstream.curveId.cdata() : nullptr;

    Cloud hairCloud;
    hairCloud.points = rest;
    std::unique_ptr<CloudIndex> hairIndex;
    if (R) hairIndex = std::make_unique<CloudIndex>(3, hairCloud, nanoflann::KDTreeSingleIndexAdaptorParams(10));
    auto nearestHair = [&](float const *q) -> int64_t {
        if (!hairIndex) return -1;
        uint32_t found = 0;
        float distance = 0.0f;
        return hairIndex->knnSearch(q, 1, &found, &distance) ? int64_t(found) : -1;
    };

    // --- guides ----------------------------------------------------------------
    std::vector<float> guideRoot(G * 3), guideShape(G * n * 3), guideLength(G * n, 0.0f);
    std::vector<GfVec3f> guideNormal(G, GfVec3f(0, 1, 0));
    std::vector<int64_t> guideRegion(G, 0);
    std::vector<float> guideWidth;
    bool const guidesHaveWidth = guides.width.size() == guides.totalCvs;
    if (guidesHaveWidth) guideWidth.assign(G * n, 0.0f);
    std::vector<float> points;
    for (size_t g = 0; g < G; ++g) {
        size_t const first = guideSpans[g], count = guideSpans[g + 1] - first;
        points.resize(count * 3);
        for (size_t i = 0; i < count; ++i) {
            points[i * 3] = guides.px[first + i];
            points[i * 3 + 1] = guides.py[first + i];
            points[i * 3 + 2] = guides.pz[first + i];
        }
        float *shape = &guideShape[g * n * 3];
        Resample(points.data(), count, n, shape);
        for (int d = 0; d < 3; ++d) guideRoot[g * 3 + d] = shape[d];
        for (size_t k = n; k-- > 0;)
            for (int d = 0; d < 3; ++d) shape[k * 3 + d] -= shape[d];   // root-local
        for (size_t k = 1; k < n; ++k) {
            GfVec3f const seg(shape[k * 3] - shape[(k - 1) * 3],
                              shape[k * 3 + 1] - shape[(k - 1) * 3 + 1],
                              shape[k * 3 + 2] - shape[(k - 1) * 3 + 2]);
            guideLength[g * n + k] = seg.GetLength();
        }
        if (guidesHaveWidth) {
            for (size_t k = 0; k < n; ++k) {
                float const at = n > 1 ? float(k) / float(n - 1) * float(count - 1) : 0.0f;
                size_t const lo = std::min(size_t(at), count - 1), hi = std::min(lo + 1, count - 1);
                guideWidth[g * n + k] = guides.width[first + lo] +
                    (guides.width[first + hi] - guides.width[first + lo]) * (at - float(lo));
            }
        }
        int64_t const near = nearestHair(&guideRoot[g * 3]);
        if (guides.rootN.size() == G) guideNormal[g] = opUtil::Normalized(guides.rootN[g]);
        else if (near >= 0) guideNormal[g] = hairNormal(size_t(near));
        if (near >= 0) guideRegion[g] = hairRegion[size_t(near)];
    }
    Cloud guideCloud;
    guideCloud.points = guideRoot;
    CloudIndex guideIndex(3, guideCloud, nanoflann::KDTreeSingleIndexAdaptorParams(10));

    // --- per strand ---------------------------------------------------------------
    float const fallbackWidth = ctx.desc ? ctx.desc->defaultWidth : 0.01f;
    cap.roots = roots;
    cap.result.assign(R * n * 3, 0.0f);
    std::vector<float> restResult(R * n * 3, 0.0f);
    std::vector<float> widths(R * n, fallbackWidth);
    std::vector<int> planeIndex(R * 3, -1);
    std::vector<float> planeWeight(R * 3, 0.0f);
    // Keep the full geometry blend for native clump inheritance. The
    // published guideIndex/guideWeight primvars intentionally expose only
    // their historical top-three subset, while maxGuides can be eight.
    std::vector<int> selectedGuide(R * 8, -1);
    std::vector<float> selectedWeight(R * 8, 0.0f);
    std::atomic<size_t> orphans{0};
    double const cosMax = std::cos(s.maxAngle * 3.14159265358979323846 / 180.0);
    size_t const maxKeep = s.unique ? 1 : size_t(s.maxGuides);
    bool const rootWidth = upstream.width.size() == upstream.totalCvs;

    ParallelFor(ctx.dispatcher, R, [&](size_t c) {
        GfVec3f const normal = hairNormal(c);
        float const *q = &rest[c * 3];
        struct Candidate { uint32_t guide; double weight; };
        Candidate keep[kMaxCandidates];
        size_t kept = 0;
        uint32_t found[kMaxCandidates];
        float distances[kMaxCandidates];
        size_t const k = std::min(G, kMaxCandidates);
        size_t const hits = guideIndex.knnSearch(q, k, found, distances);
        auto angleOk = [&](uint32_t g) {
            return s.maxAngle >= 180.0 || double(GfDot(normal, guideNormal[g])) >= cosMax - 1e-9;
        };
        for (size_t j = 0; j < hits; ++j) {
            uint32_t const g = found[j];
            double const d = std::sqrt(double(distances[j]));
            if (d >= s.radius || !angleOk(g)) continue;
            double w = std::pow(1.0 - d / s.radius, s.decay);
            if (guideRegion[g] != hairRegion[c]) w *= s.crossover;
            if (s.randomize > 0.0)
                w *= 1.0 - s.randomize *
                    double(UsdGenHash01(UsdGenHash64(ids ? ids[c] : c, uint64_t(g), kSaltGuide),
                                        kSaltGuide + uint32_t(ctx.seed)));
            if (w <= 0.0) continue;
            keep[kept++] = {g, w};
        }
        if (kept == 0) {
            // Nearest guide of the strand's own region, however far.
            auto nearestIn = [&](bool sameRegion, bool checkAngle) -> int64_t {
                for (size_t j = 0; j < hits; ++j)
                    if ((!sameRegion || guideRegion[found[j]] == hairRegion[c]) &&
                        (!checkAngle || angleOk(found[j])))
                        return found[j];
                int64_t best = -1;
                double bestDistance = std::numeric_limits<double>::infinity();
                for (uint32_t g = 0; g < G; ++g) {
                    if ((sameRegion && guideRegion[g] != hairRegion[c]) ||
                        (checkAngle && !angleOk(g))) continue;
                    double const dx = double(q[0]) - guideRoot[g * 3];
                    double const dy = double(q[1]) - guideRoot[g * 3 + 1];
                    double const dz = double(q[2]) - guideRoot[g * 3 + 2];
                    double const distance = dx * dx + dy * dy + dz * dz;
                    if (distance < bestDistance) { bestDistance = distance; best = g; }
                }
                return best;
            };
            int64_t g = nearestIn(true, true);
            if (g < 0) g = nearestIn(true, false);
            if (g < 0) { g = nearestIn(false, false); ++orphans; }
            keep[kept++] = {uint32_t(g), 1.0};
        }
        std::sort(keep, keep + kept, [](Candidate const &a, Candidate const &b) {
            return a.weight != b.weight ? a.weight > b.weight : a.guide < b.guide;
        });
        kept = std::min(kept, maxKeep);
        double total = 0.0;
        for (size_t j = 0; j < kept; ++j) total += keep[j].weight;
        for (size_t j = 0; j < kept; ++j) keep[j].weight = total > 0.0 ? keep[j].weight / total : 1.0 / double(kept);
        for (size_t j = 0; j < 3; ++j) {
            planeIndex[c * 3 + j] = j < kept ? int(keep[j].guide) : -1;
            planeWeight[c * 3 + j] = j < kept ? float(keep[j].weight) : 0.0f;
        }
        for (size_t j = 0; j < kept; ++j) {
            selectedGuide[c * 8 + j] = int(keep[j].guide);
            selectedWeight[c * 8 + j] = float(keep[j].weight);
        }

        // Compose, at the rest root.
        float *outRest = &restResult[c * n * 3];
        GfVec3f previous(q[0], q[1], q[2]);
        for (size_t i = 0; i < n; ++i) {
            GfVec3f value(0.0f);
            float length = 0.0f;
            for (size_t j = 0; j < kept; ++j) {
                uint32_t const g = keep[j].guide;
                float const w = float(keep[j].weight);
                float const *shape = &guideShape[g * n * 3];
                GfVec3f local = s.extrude && i > 0
                    ? GfVec3f(shape[i * 3] - shape[(i - 1) * 3],
                              shape[i * 3 + 1] - shape[(i - 1) * 3 + 1],
                              shape[i * 3 + 2] - shape[(i - 1) * 3 + 2])
                    : GfVec3f(shape[i * 3], shape[i * 3 + 1], shape[i * 3 + 2]);
                local = opUtil::RotateOnto(local, guideNormal[g], normal, float(s.skin));
                value += local * w;
                length += guideLength[g * n + i] * w;
            }
            GfVec3f point;
            if (s.extrude && i > 0) {
                float const l = value.GetLength();
                point = previous + (l > 1e-12f ? value * (length / l) : GfVec3f(0.0f));
            } else {
                point = GfVec3f(q[0], q[1], q[2]) + value;
            }
            previous = point;
            for (int d = 0; d < 3; ++d) outRest[i * 3 + d] = point[d];
            if (guidesHaveWidth) {
                float w = 0.0f;
                for (size_t j = 0; j < kept; ++j)
                    w += guideWidth[keep[j].guide * n + i] * float(keep[j].weight);
                widths[c * n + i] = w;
            } else if (rootWidth) {
                widths[c * n + i] = upstream.width[rootSpans[c]];
            }
        }
        // The current strand: the rest strand moved with its root.
        float const delta[3] = {roots[c * 3] - q[0], roots[c * 3 + 1] - q[1], roots[c * 3 + 2] - q[2]};
        for (size_t i = 0; i < n; ++i)
            for (int d = 0; d < 3; ++d)
                cap.result[(c * n + i) * 3 + d] = outRest[i * 3 + d] + delta[d];
    });
    if (mapped && orphans.load()) return fail("regionMap contains a region with no growth guide");
    if (orphans.load() && diag)
        diag->Warn("UsdGenGuideInterpolate: " + std::to_string(orphans.load()) +
                   " strands lie in regions without a guide and follow the nearest guide of"
                   " another region");

    // --- the output buffer ----------------------------------------------------------
    UsdGenCurveBuffer &buf = cap.MutableBuffer();
    buf.totalCurves = uint32_t(R);
    buf.totalCvs = uint32_t(R * n);
    buf.topologyVersion = upstream.topologyVersion + 1;
    buf.cvOffsets.clear();
    buf.px = VtFloatArray(R * n);
    buf.py = VtFloatArray(R * n);
    buf.pz = VtFloatArray(R * n);
    buf.hairT = VtFloatArray(R * n);
    buf.width = VtFloatArray(widths.begin(), widths.end());
    buf.rest = VtVec3fArray(R * n);
    for (size_t o = 0; o < R * n; ++o) {
        buf.px[o] = cap.result[o * 3];
        buf.py[o] = cap.result[o * 3 + 1];
        buf.pz[o] = cap.result[o * 3 + 2];
        buf.hairT[o] = float(o % n) / float(n - 1);
        buf.rest[o] = GfVec3f(restResult[o * 3], restResult[o * 3 + 1], restResult[o * 3 + 2]);
    }
    std::string planeError;
    if (!UsdGenResampleExtraPlanes(upstream, &buf, &planeError))
        return fail("cannot carry named planes across the CV topology change: " + planeError);
    // A complete native guide quartet follows the chosen guide group. A
    // mixed guide blend retains only the selected group's share of cohesion;
    // the other guides continue shaping geometry without claiming that this
    // strand moves rigidly with a group it did not fully follow. Numeric
    // levels not present on guides retain the root-source planes above.
    auto findGuideIds = [&guides](int level) -> UsdGenPlane const * {
        TfToken const name("clumpId_" + std::to_string(level));
        auto const it = std::lower_bound(guides.extraCurve.begin(), guides.extraCurve.end(), name,
            [](UsdGenPlane const &plane, TfToken const &needle) { return plane.name < needle; });
        return it != guides.extraCurve.end() && it->name == name ? &*it : nullptr;
    };
    auto erasePlane = [&buf](TfToken const &name) {
        auto eraseNamed = [&name](std::vector<UsdGenPlane> *planes) {
            planes->erase(std::remove_if(planes->begin(), planes->end(),
                [&name](UsdGenPlane const &plane) { return plane.name == name; }), planes->end());
        };
        eraseNamed(&buf.extraCurve);
        eraseNamed(&buf.extraCv);
    };
    auto insertNamed = [&buf](UsdGenPlane plane, bool vertex) {
        auto &planes = vertex ? buf.extraCv : buf.extraCurve;
        auto it = std::lower_bound(planes.begin(), planes.end(), plane.name,
            [](UsdGenPlane const &a, TfToken const &b) { return a.name < b; });
        if (it != planes.end() && it->name == plane.name) *it = std::move(plane);
        else planes.insert(it, std::move(plane));
    };
    for (UsdGenClumpMotionLevel const &level : guideClumps.levels) {
        if (level.weight.empty()) continue; // legacy ID only
        UsdGenPlane const *guideIds = findGuideIds(level.level);
        if (!guideIds || guideIds->i.size() != G) continue;
        std::string const suffix = std::to_string(level.level);
        UsdGenPlane outId, outCenter, outCenterId, outWeight;
        outId.name = TfToken("clumpId_" + suffix);
        outId.interpolation = TfToken("uniform");
        outId.type = TfToken("int");
        outId.i = VtIntArray(R, -1);
        outCenter.name = TfToken("clumpCenter_" + suffix);
        outCenter.interpolation = TfToken("uniform");
        outCenter.type = TfToken("float");
        outCenter.arity = 3;
        outCenter.f = VtFloatArray(R * 3, 0.0f);
        outCenterId.name = TfToken("clumpCenterId_" + suffix);
        outCenterId.interpolation = TfToken("uniform");
        outCenterId.type = TfToken("int");
        outCenterId.arity = 2;
        outCenterId.i = VtIntArray(R * 2, 0);
        outWeight.name = TfToken("clumpWeight_" + suffix);
        outWeight.interpolation = TfToken("vertex");
        outWeight.type = TfToken("float");
        outWeight.f = VtFloatArray(R * n, 0.0f);
        for (size_t c = 0; c < R; ++c) {
            uint32_t selected = UINT32_MAX;
            float selectedShare = 0.0f;
            for (size_t j = 0; j < 8; ++j) {
                int const guide = selectedGuide[c * 8 + j];
                if (guide < 0 || size_t(guide) >= G) continue;
                uint32_t const group = level.groupForCurve[size_t(guide)];
                if (group >= level.groups.size()) continue;
                float share = 0.0f;
                for (size_t k = 0; k < 8; ++k) {
                    int const other = selectedGuide[c * 8 + k];
                    if (other >= 0 && size_t(other) < G &&
                        level.groupForCurve[size_t(other)] == group)
                        share += selectedWeight[c * 8 + k];
                }
                if (share > selectedShare || (share == selectedShare &&
                    selected < level.groups.size() &&
                    level.groups[group].centerId < level.groups[selected].centerId)) {
                    selected = group;
                    selectedShare = share;
                }
            }
            if (selected >= level.groups.size() || selectedShare <= 0.0f) continue;
            UsdGenClumpMotionGroup const &group = level.groups[selected];
            for (size_t j = 0; j < 8; ++j) {
                int const guide = selectedGuide[c * 8 + j];
                if (guide < 0 || size_t(guide) >= G ||
                    level.groupForCurve[size_t(guide)] != selected) continue;
                outId.i[c] = guideIds->i[size_t(guide)];
                break;
            }
            for (int d = 0; d < 3; ++d) outCenter.f[c * 3 + size_t(d)] = group.restAnchor[d];
            uint32_t const words[2] = {uint32_t(group.centerId),
                                       uint32_t(group.centerId >> 32)};
            std::memcpy(&outCenterId.i[c * 2], words, sizeof(words));
            for (size_t i = 0; i < n; ++i) {
                float const t = n > 1 ? float(i) / float(n - 1) : 0.0f;
                float blended = 0.0f;
                for (size_t j = 0; j < 8; ++j) {
                    int const guide = selectedGuide[c * 8 + j];
                    if (guide < 0 || size_t(guide) >= G ||
                        level.groupForCurve[size_t(guide)] != selected) continue;
                    size_t const first = guideSpans[size_t(guide)];
                    size_t const count = guideSpans[size_t(guide) + 1] - first;
                    if (count == 0) continue;
                    float const at = count > 1 ? t * float(count - 1) : 0.0f;
                    size_t const lo = std::min(size_t(at), count - 1);
                    size_t const hi = std::min(lo + 1, count - 1);
                    float const f = at - float(lo);
                    float const a = level.weight[first + lo];
                    float const b = level.weight[first + hi];
                    blended += selectedWeight[c * 8 + j] * (a + (b - a) * f);
                }
                outWeight.f[c * n + i] = std::clamp(blended, 0.0f, 1.0f);
            }
        }
        for (TfToken const &name : {outId.name, outCenter.name,
                                    outCenterId.name, outWeight.name}) erasePlane(name);
        insertNamed(std::move(outId), false);
        insertNamed(std::move(outCenter), false);
        insertNamed(std::move(outCenterId), false);
        insertNamed(std::move(outWeight), true);
    }
    auto insertPlane = [&buf](UsdGenPlane plane) {
        auto &planes = buf.extraCurve;
        auto it = std::lower_bound(planes.begin(), planes.end(), plane.name,
            [](UsdGenPlane const &a, TfToken const &b) { return a.name < b; });
        if (it != planes.end() && it->name == plane.name) *it = std::move(plane);
        else planes.insert(it, std::move(plane));
    };
    UsdGenPlane index;
    index.name = sGuideIndex;
    index.interpolation = TfToken("uniform");
    index.type = TfToken("int");
    index.arity = 3;
    index.i = VtIntArray(planeIndex.begin(), planeIndex.end());
    insertPlane(std::move(index));
    UsdGenPlane weight;
    weight.name = sGuideWeight;
    weight.interpolation = TfToken("uniform");
    weight.type = TfToken("float");
    weight.arity = 3;
    weight.f = VtFloatArray(planeWeight.begin(), planeWeight.end());
    insertPlane(std::move(weight));
    return true;
}

void UsdGenGuideInterpolateOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &captureIn,
    UsdGenChunkView *view) const
{
    TF_UNUSED(ctx);
    auto const &cap = static_cast<UsdGenGuideInterpolateCapture const &>(captureIn);
    size_t const n = cap.cvCount;
    size_t const curveBase = view->desc ? size_t(view->desc->firstCurve) : 0;
    for (uint32_t c = 0; c < view->curveCount; ++c) {
        size_t const curve = curveBase + c;
        if ((curve + 1) * n * 3 > cap.result.size()) continue;
        // Input chunks may be ragged roots; rebase like Grow does.
        uint32_t const r = view->inCvOffsets
            ? static_cast<uint32_t>(view->inCvOffsets[c] - int(view->inFirstCv))
            : c * view->inCvCount;
        float const delta[3] = {
            view->inPx ? view->inPx[r] - cap.roots[curve * 3] : 0.0f,
            view->inPy ? view->inPy[r] - cap.roots[curve * 3 + 1] : 0.0f,
            view->inPz ? view->inPz[r] - cap.roots[curve * 3 + 2] : 0.0f};
        for (uint32_t i = 0; i < view->cvCount && i < n; ++i) {
            size_t const o = view->Cv(c, i);
            float const *p = &cap.result[(curve * n + i) * 3];
            view->px[o] = p[0] + delta[0];
            view->py[o] = p[1] + delta[1];
            view->pz[o] = p[2] + delta[2];
            if (view->hairT) view->hairT[o] = n > 1 ? float(i) / float(n - 1) : 0.0f;
        }
    }
}

}  // namespace usdGen
