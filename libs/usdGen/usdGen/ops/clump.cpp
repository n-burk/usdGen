// usdGen — UsdGenClumpOp implementation. 02-schema.md §2.7, 04 §2.8.
//
// Capture resolves the whole result: membership per level, the centre strand
// of every clump, and the pulled CV positions. The pull reads other strands
// (a clump's centre), and a chunk kernel may only read its own chunk (I3), so
// the centre shapes are snapshots of the input; the capture is re-run
// whenever the input's values move (ValidForTopology checks valueVersion,
// exactly like Grow). Evaluate applies usdGen:mask, the one value-class
// control, and writes the clumpId_<level> planes.
//
// Membership, level 0:
//   usdGen:clump:map connected -> strands whose map value is equal (10-bit
//     quantised) form one clump; its centre is the member rooted nearest the
//     group's rest centroid. A ptex region map, a geoSampler voronoi or any
//     expression can drive it.
//   unconnected -> round(density * restArea) centres are picked from the
//     strands in hash order with a minimum spacing (a jittered, even spread)
//     and every strand joins its nearest centre.
// Level L > 0 splits each level L-1 clump the same implicit way, with
// density / sizeReduction^(2L) centres, size * sizeReduction^L reach and
// amount * tightnessReduction^L strength; its centre shapes are the strands
// as level L-1 left them (goal feedback).
// A strand farther than the level's usdGen:clump:size from its centre is not
// clumped at that level.
//
// Pull, per CV:  w = amount * tightness^L * profile(t) * (1 - stray * t^falloff)
//   linearBlend      P = lerp(P, C(t), w)                  tips converge
//   extrudeAndBlend  P = lerp(P, C(t) + (root - rootC), w)  strands align
// then usdGen:preserveLength restores the input segment lengths, root locked.
#include "usdGen/ops/clump.h"

#include "usdGen/opParams.h"
#include "usdGen/ops/opUtil.h"
#include "usdGen/scheduler.h"
#include "usdGenMath/usdGenMath/hash.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "usdGenMath/usdGenMath/ramp.h"

#include "pxr/base/gf/range3d.h"

#include <nanoflann.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

const TfToken sAmount{"clump:amount"}, sMap{"clump:map"}, sDensity{"clump:density"},
    sSize{"clump:size"}, sLevels{"clump:levels"}, sLevel{"clump:level"},
    sSizeReduction{"clump:sizeReduction"}, sTightness{"clump:tightnessReduction"},
    sMethod{"clump:method"}, sProfileKnots{"clump:profile:knots"},
    sProfileInterp{"clump:profile:interpolation"}, sStrayAmount{"clump:stray:amount"},
    sStrayRate{"clump:stray:rate"}, sStrayFalloff{"clump:stray:falloff"},
    sPreserveLength{"preserveLength"}, sExtrude{"extrudeAndBlend"},
    sLinear{"linearBlend"}, sCatmullRom{"catmullRom"};

constexpr uint32_t kSaltClumpStray = kSaltClump + 64u;

struct UsdGenClumpCapture final : public UsdGenCapture
{
    std::vector<float> result;                    // 3 * totalCvs
    std::vector<std::vector<int32_t>> clumpId;    // per level, per strand; -1 = none
    uint64_t upstreamTopologyVersion = 0;
    uint64_t upstreamValueVersion = 0;
    uint32_t upstreamCurves = 0;
    uint32_t upstreamCvs = 0;

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenClumpCapture>(*this);
    }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.valueVersion == upstreamValueVersion &&
               upstream.totalCurves == upstreamCurves &&
               upstream.totalCvs == upstreamCvs;
    }
};

struct Cloud {
    std::vector<float> points;   // xyz
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

VtVec2fArray ReadKnots(UsdGenParamView const *p, TfToken const &name)
{
    if (!p) return {};
    VtValue const v = p->GetVtValue(name, VtValue());
    return v.IsHolding<VtVec2fArray>() ? v.UncheckedGet<VtVec2fArray>() : VtVec2fArray();
}

struct Settings {
    int levels = 1;
    double density = 4.0, size = 1.0, sizeReduction = 0.5, tightness = 0.8;
    double strayAmount = 0.0, strayRate = 0.0, strayFalloff = 1.0;
    bool extrude = false;
};

bool ReadSettings(UsdGenParamView const *p, Settings *s, UsdGenDiagnostics *diag)
{
    auto fail = [&](char const *message) {
        if (diag) diag->Error(std::string("UsdGenClump: ") + message);
        return false;
    };
    if (!p) return true;
    s->levels = p->GetInt(sLevels, 1);
    s->density = p->GetDouble(sDensity, 4.0);
    s->size = p->GetDouble(sSize, 1.0);
    s->sizeReduction = p->GetDouble(sSizeReduction, 0.5);
    s->tightness = p->GetDouble(sTightness, 0.8);
    s->strayAmount = p->GetDouble(sStrayAmount, 0.0);
    s->strayRate = p->GetDouble(sStrayRate, 0.0);
    s->strayFalloff = p->GetDouble(sStrayFalloff, 1.0);
    TfToken const method = p->GetToken(sMethod, sLinear);
    if (method != sLinear && method != sExtrude)
        return fail("usdGen:clump:method must be linearBlend or extrudeAndBlend");
    s->extrude = method == sExtrude;
    if (s->levels < 1 || s->levels > 4) return fail("usdGen:clump:levels must be in [1, 4]");
    if (!std::isfinite(s->density) || s->density < 0.0)
        return fail("usdGen:clump:density must be finite and >= 0");
    if (!std::isfinite(s->size) || s->size <= 0.0)
        return fail("usdGen:clump:size must be finite and > 0");
    if (!(s->sizeReduction > 0.0 && s->sizeReduction <= 1.0))
        return fail("usdGen:clump:sizeReduction must be in (0, 1]");
    if (!(s->tightness >= 0.0 && s->tightness <= 1.0))
        return fail("usdGen:clump:tightnessReduction must be in [0, 1]");
    if (!(s->strayAmount >= 0.0 && s->strayAmount <= 1.0) ||
        !(s->strayRate >= 0.0 && s->strayRate <= 1.0) ||
        !(std::isfinite(s->strayFalloff) && s->strayFalloff >= 0.0))
        return fail("usdGen:clump:stray amount/rate must be in [0, 1] and falloff >= 0");
    return true;
}

/// Picks up to `want` centres among `members` (hash order, minimum spacing)
/// and returns them. `roots` is xyz per strand.
std::vector<uint32_t> PickCentres(std::vector<uint32_t> const &members,
                                  std::vector<float> const &roots,
                                  uint64_t const *ids, int seed, uint32_t salt,
                                  size_t want, double spacing)
{
    std::vector<std::pair<float, uint32_t>> order;
    order.reserve(members.size());
    for (uint32_t c : members)
        order.emplace_back(UsdGenDraw01(seed, ids ? ids[c] : c, salt), c);
    std::sort(order.begin(), order.end());
    std::vector<uint32_t> centres;
    if (want == 0 || order.empty()) return centres;
    // Uniform grid over the accepted centres: a candidate is rejected when an
    // accepted centre lies closer than `spacing`.
    double const cell = spacing > 0.0 ? spacing : 1.0;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;
    auto key = [&](int64_t x, int64_t y, int64_t z) {
        return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791);
    };
    auto cellOf = [&](float v) { return int64_t(std::floor(double(v) / cell)); };
    for (auto const &entry : order) {
        if (centres.size() >= want) break;
        uint32_t const c = entry.second;
        float const *p = &roots[size_t(c) * 3];
        int64_t const cx = cellOf(p[0]), cy = cellOf(p[1]), cz = cellOf(p[2]);
        bool ok = true;
        if (spacing > 0.0) {
            for (int64_t dx = -1; dx <= 1 && ok; ++dx)
                for (int64_t dy = -1; dy <= 1 && ok; ++dy)
                    for (int64_t dz = -1; dz <= 1 && ok; ++dz) {
                        auto found = grid.find(key(cx + dx, cy + dy, cz + dz));
                        if (found == grid.end()) continue;
                        for (uint32_t other : found->second) {
                            float const *q = &roots[size_t(other) * 3];
                            double const ddx = double(p[0]) - q[0], ddy = double(p[1]) - q[1],
                                         ddz = double(p[2]) - q[2];
                            if (ddx * ddx + ddy * ddy + ddz * ddz < spacing * spacing) {
                                ok = false;
                                break;
                            }
                        }
                    }
        }
        if (!ok) continue;
        centres.push_back(c);
        grid[key(cx, cy, cz)].push_back(c);
    }
    return centres;
}

}  // namespace

static TfTokenVector _topoParams = [] {
    TfTokenVector v = UsdGenBaseTopologyParams();
    // Clump resolves its whole result at capture (see the file comment), so
    // every control except the envelope is capture-class.
    for (char const *name : {"clump:amount", "clump:map", "clump:density", "clump:size",
                             "clump:levels", "clump:level", "clump:sizeReduction",
                             "clump:tightnessReduction", "clump:method",
                             "clump:profile:knots", "clump:profile:interpolation",
                             "clump:stray:amount", "clump:stray:rate",
                             "clump:stray:falloff", "preserveLength"})
        v.push_back(TfToken(name));
    return v;
}();

static TfTokenVector _valueParams = [] {
    TfTokenVector v = UsdGenBaseValueParams();
    v.push_back(TfToken("enabled"));
    v.push_back(TfToken("mask"));
    return v;
}();

TfSpan<const TfToken> UsdGenClumpOp::TopologyParameters() const
{
    return TfSpan<const TfToken>(_topoParams.data(), _topoParams.size());
}
TfSpan<const TfToken> UsdGenClumpOp::ValueParameters() const
{
    return TfSpan<const TfToken>(_valueParams.data(), _valueParams.size());
}
TfSpan<const TfToken> UsdGenClumpOp::OutputPrimvars() const
{
    return TfSpan<const TfToken>(_outputs.data(), _outputs.size());
}
std::unique_ptr<UsdGenCapture> UsdGenClumpOp::CreateCapture() const
{
    return std::make_unique<UsdGenClumpCapture>();
}
uint32_t UsdGenClumpOp::PlanesTouched() const
{
    return kPlanePoints;
}

void UsdGenClumpOp::Configure(UsdGenParamView const &params)
{
    // usdGen:clump:level = -1 numbers this node by its ordinal among every
    // UsdGenClump of the description, disabled ones included, so toggling a
    // node never renumbers another's published planes (04 §2.8).
    int base = params.GetInt(sLevel, -1);
    if (base < 0) {
        base = 0;
        if (params.desc && params.node) {
            for (UsdGenNodeDesc const &node : params.desc->nodes) {
                if (&node == params.node || node.path == params.node->path) break;
                if (node.type == TfToken("UsdGenClump")) ++base;
            }
        }
    }
    int const levels = std::clamp(params.GetInt(sLevels, 1), 1, 4);
    _outputs.clear();
    for (int l = 0; l < levels; ++l)
        _outputs.push_back(TfToken("clumpId_" + std::to_string(base + l)));
}

bool UsdGenClumpOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
    Settings settings;
    return ReadSettings(&params, &settings, diag);
}

UsdGenEpoch UsdGenClumpOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
    opUtil::Digest d;
    if (UsdGenParamView const *p = ctx.params) {
        for (TfToken const &name : _topoParams) {
            d.Mix(name);
            d.Mix(p->GetVtValue(name, VtValue()));
        }
    }
    d.Mix(uint64_t(ctx.seed));
    d.Mix(ctx.upstreamGeneration);
    if (ctx.desc && ctx.surface < ctx.desc->surfaces.size())
        d.Mix(ctx.desc->surfaces[ctx.surface].surfaceGeneration);
    return d.Epoch(0x436C756D70ull);
}

bool UsdGenClumpOp::Capture(
    UsdGenCaptureContext const &ctx,
    UsdGenCurveBuffer const &upstream,
    UsdGenCapture *out,
    UsdGenDiagnostics *diag)
{
    UsdGenParamView const *p = ctx.params;
    auto &cap = *static_cast<UsdGenClumpCapture *>(out);
    Settings s;
    if (!ReadSettings(p, &s, diag)) return false;

    size_t const R = upstream.totalCurves;
    size_t const totalCvs = upstream.totalCvs;
    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamValueVersion = upstream.valueVersion;
    cap.upstreamCurves = upstream.totalCurves;
    cap.upstreamCvs = upstream.totalCvs;
    cap.clumpId.assign(size_t(s.levels), std::vector<int32_t>(R, -1));
    cap.result.clear();
    if (R == 0) return true;

    std::vector<uint32_t> spans;
    std::string error;
    if (!opUtil::CurveSpans(upstream, &spans, &error)) {
        if (diag) diag->Error("UsdGenClump: " + error);
        return false;
    }
    if (upstream.px.size() != totalCvs || upstream.py.size() != totalCvs ||
        upstream.pz.size() != totalCvs) {
        if (diag) diag->Error("UsdGenClump: input point planes do not match the CV count");
        return false;
    }

    // Input shapes, strand roots and parameters.
    std::vector<float> shape(totalCvs * 3);
    for (size_t i = 0; i < totalCvs; ++i) {
        shape[i * 3] = upstream.px[i];
        shape[i * 3 + 1] = upstream.py[i];
        shape[i * 3 + 2] = upstream.pz[i];
    }
    std::vector<float> const input = shape;
    std::vector<float> roots(R * 3);
    for (size_t c = 0; c < R; ++c) {
        GfVec3f const r = opUtil::RestPoint(upstream, spans[c]);
        roots[c * 3] = r[0]; roots[c * 3 + 1] = r[1]; roots[c * 3 + 2] = r[2];
    }
    uint64_t const *ids = upstream.curveId.size() == R ? upstream.curveId.cdata() : nullptr;
    auto hairT = [&](size_t cv, size_t i, size_t n) {
        return upstream.hairT.size() == totalCvs
            ? upstream.hairT[cv]
            : (n > 1 ? float(i) / float(n - 1) : 0.0f);
    };
    UsdGenParamField const amountField = p ? p->GetScalarField(sAmount, 0.5) : UsdGenParamField{0.5};
    UsdGenParamField const mapField = p ? p->GetScalarField(sMap, 0.0) : UsdGenParamField{};
    UsdGenParamField const preserveField =
        p ? p->GetScalarField(sPreserveLength, 1.0) : UsdGenParamField{1.0};
    bool const mapMode = mapField.connected;

    std::vector<float> profile;
    VtVec2fArray const knots = ReadKnots(p, sProfileKnots);
    if (!knots.empty()) {
        profile.assign(kUsdGenRampLutSize, 1.0f);
        UsdGenBuildRampLut(knots, p ? p->GetToken(sProfileInterp, sCatmullRom) : sCatmullRom,
                           profile.data(), kUsdGenRampLutSize);
    }

    std::vector<float> stray(R, 0.0f);
    if (s.strayRate > 0.0 && s.strayAmount > 0.0)
        for (size_t c = 0; c < R; ++c)
            if (UsdGenDraw01(int(ctx.seed), ids ? ids[c] : c, kSaltClumpStray) < s.strayRate)
                stray[c] = float(s.strayAmount);

    double area = opUtil::RestSurfaceArea(ctx.desc, ctx.surface, ctx.desc &&
        ctx.surface < ctx.desc->surfaces.size());
    if (!(area > 0.0)) {
        // No bound surface: the roots' bounding box stands in for the area.
        GfRange3d box;
        for (size_t c = 0; c < R; ++c)
            box.UnionWith(GfVec3d(roots[c * 3], roots[c * 3 + 1], roots[c * 3 + 2]));
        GfVec3d const e = box.GetSize();
        double const a = e[0] * e[1], b = e[1] * e[2], d = e[0] * e[2];
        area = std::max({a, b, d, 1e-12});
    }

    std::vector<int32_t> parent(R, 0);
    size_t parentCount = 1;
    for (int level = 0; level < s.levels; ++level) {
        double const reach = s.size * std::pow(s.sizeReduction, level);
        double const density = s.density / std::pow(s.sizeReduction, 2.0 * level);
        float const strength = float(std::pow(s.tightness, level));
        uint32_t const salt = kSaltClumpLevel(level);

        // --- membership ----------------------------------------------------
        std::vector<std::vector<uint32_t>> groups(parentCount);
        for (size_t c = 0; c < R; ++c)
            if (parent[c] >= 0) groups[size_t(parent[c])].push_back(uint32_t(c));
        std::vector<uint32_t> centreOf;          // clump -> centre strand
        std::vector<int32_t> &member = cap.clumpId[size_t(level)];

        if (level == 0 && mapMode) {
            std::map<std::pair<int32_t, int64_t>, std::vector<uint32_t>> regions;
            for (size_t c = 0; c < R; ++c) {
                if (parent[c] < 0) continue;
                double const value = mapField.Value(c, spans[c]);
                regions[{parent[c], opUtil::RegionKey(value)}].push_back(uint32_t(c));
            }
            for (auto const &region : regions) {
                GfVec3d centroid(0.0);
                for (uint32_t c : region.second)
                    centroid += GfVec3d(roots[c * 3], roots[c * 3 + 1], roots[c * 3 + 2]);
                centroid /= double(region.second.size());
                uint32_t best = region.second.front();
                double bestDistance = std::numeric_limits<double>::infinity();
                for (uint32_t c : region.second) {
                    GfVec3d const d = GfVec3d(roots[c * 3], roots[c * 3 + 1], roots[c * 3 + 2]) - centroid;
                    double const distance = d.GetLengthSq();
                    if (distance < bestDistance) { bestDistance = distance; best = c; }
                }
                int32_t const clump = int32_t(centreOf.size());
                centreOf.push_back(best);
                for (uint32_t c : region.second) member[c] = clump;
            }
        } else {
            std::vector<uint32_t> centreParent;
            for (size_t g = 0; g < groups.size(); ++g) {
                auto const &members = groups[g];
                if (members.empty()) continue;
                double const share = area * double(members.size()) / double(R);
                size_t const want = std::max<size_t>(1, size_t(std::llround(density * share)));
                double const spacing = 0.75 * std::sqrt(share / (3.14159265358979 * double(want)));
                for (uint32_t c : PickCentres(members, roots, ids, int(ctx.seed), salt, want, spacing)) {
                    centreOf.push_back(c);
                    centreParent.push_back(uint32_t(g));
                }
            }
            // Nearest centre of the same parent.
            Cloud cloud;
            cloud.points.reserve(centreOf.size() * 3);
            for (uint32_t c : centreOf)
                for (int d = 0; d < 3; ++d) cloud.points.push_back(roots[c * 3 + d]);
            if (!centreOf.empty()) {
                CloudIndex index(3, cloud, nanoflann::KDTreeSingleIndexAdaptorParams(10));
                size_t const k = std::min<size_t>(centreOf.size(), 16);
                std::vector<std::vector<uint32_t>> byParent(groups.size());
                for (size_t i = 0; i < centreOf.size(); ++i) byParent[centreParent[i]].push_back(uint32_t(i));
                ParallelFor(ctx.dispatcher, R, [&](size_t c) {
                    if (parent[c] < 0) return;
                    uint32_t found[16];
                    float distances[16];
                    size_t const n = index.knnSearch(&roots[c * 3], k, found, distances);
                    for (size_t j = 0; j < n; ++j)
                        if (centreParent[found[j]] == uint32_t(parent[c])) {
                            member[c] = int32_t(found[j]);
                            return;
                        }
                    // Every nearby centre belongs to another parent: scan this one's.
                    double best = std::numeric_limits<double>::infinity();
                    for (uint32_t i : byParent[size_t(parent[c])]) {
                        float const *q = &cloud.points[size_t(i) * 3];
                        double const dx = double(roots[c * 3]) - q[0], dy = double(roots[c * 3 + 1]) - q[1],
                                     dz = double(roots[c * 3 + 2]) - q[2];
                        double const distance = dx * dx + dy * dy + dz * dz;
                        if (distance < best) { best = distance; member[c] = int32_t(i); }
                    }
                });
            }
        }

        // --- reach -----------------------------------------------------------
        for (size_t c = 0; c < R; ++c) {
            if (member[c] < 0) continue;
            uint32_t const centre = centreOf[size_t(member[c])];
            double const dx = double(roots[c * 3]) - roots[centre * 3];
            double const dy = double(roots[c * 3 + 1]) - roots[centre * 3 + 1];
            double const dz = double(roots[c * 3 + 2]) - roots[centre * 3 + 2];
            if (dx * dx + dy * dy + dz * dz > reach * reach) member[c] = -1;
        }

        // --- pull (centre shapes are this level's input: goal feedback) -----
        std::vector<float> const before = shape;
        ParallelFor(ctx.dispatcher, R, [&](size_t c) {
            if (member[c] < 0) return;
            uint32_t const centre = centreOf[size_t(member[c])];
            if (centre == c) return;
            size_t const first = spans[c], n = spans[c + 1] - spans[c];
            size_t const cFirst = spans[centre], cN = spans[centre + 1] - spans[centre];
            if (n == 0 || cN == 0) return;
            float const offset[3] = {
                s.extrude ? roots[c * 3] - roots[centre * 3] : 0.0f,
                s.extrude ? roots[c * 3 + 1] - roots[centre * 3 + 1] : 0.0f,
                s.extrude ? roots[c * 3 + 2] - roots[centre * 3 + 2] : 0.0f};
            for (size_t i = 0; i < n; ++i) {
                size_t const cv = first + i;
                float const t = hairT(cv, i, n);
                // The centre sampled at the same parameter, for a centre with
                // a different CV count.
                float const at = cN > 1 ? t * float(cN - 1) : 0.0f;
                size_t const lo = std::min(size_t(at), cN - 1);
                size_t const hi = std::min(lo + 1, cN - 1);
                float const f = at - float(lo);
                float w = std::clamp(float(amountField.Value(c, cv)), 0.0f, 1.0f) * strength;
                if (!profile.empty()) w *= UsdGenEvalLut257(profile.data(), t);
                if (stray[c] > 0.0f)
                    w *= 1.0f - stray[c] * float(std::pow(double(t), s.strayFalloff));
                w = std::clamp(w, 0.0f, 1.0f);
                if (w == 0.0f) continue;
                for (int d = 0; d < 3; ++d) {
                    float const target = before[(cFirst + lo) * 3 + d] +
                        (before[(cFirst + hi) * 3 + d] - before[(cFirst + lo) * 3 + d]) * f +
                        offset[d];
                    float &v = shape[cv * 3 + d];
                    v += (target - v) * w;
                }
            }
        });

        // --- next level's regions -------------------------------------------
        parent = member;
        parentCount = centreOf.size();
    }

    // Restore the input segment lengths, root locked.
    ParallelFor(ctx.dispatcher, R, [&](size_t c) {
        size_t const first = spans[c], n = spans[c + 1] - spans[c];
        for (size_t i = 1; i < n; ++i) {
            size_t const cv = first + i;
            float const keep = std::clamp(float(preserveField.Value(c, cv)), 0.0f, 1.0f);
            if (keep == 0.0f) continue;
            GfVec3f const prev(shape[(cv - 1) * 3], shape[(cv - 1) * 3 + 1], shape[(cv - 1) * 3 + 2]);
            GfVec3f seg(shape[cv * 3] - prev[0], shape[cv * 3 + 1] - prev[1], shape[cv * 3 + 2] - prev[2]);
            GfVec3f const was(input[cv * 3] - input[(cv - 1) * 3],
                              input[cv * 3 + 1] - input[(cv - 1) * 3 + 1],
                              input[cv * 3 + 2] - input[(cv - 1) * 3 + 2]);
            float const now = seg.GetLength();
            if (now < 1e-9f) continue;
            float const length = now + (was.GetLength() - now) * keep;
            seg *= length / now;
            for (int d = 0; d < 3; ++d) shape[cv * 3 + d] = prev[d] + seg[d];
        }
    });
    cap.result = std::move(shape);
    return true;
}

void UsdGenClumpOp::Evaluate(
    UsdGenEvalContext const &ctx,
    UsdGenCapture const &captureIn,
    UsdGenChunkView *view) const
{
    auto const &cap = static_cast<UsdGenClumpCapture const &>(captureIn);
    UsdGenParamView const *p = ctx.params;
    UsdGenParamField const maskField = p ? p->GetScalarField(sMask, 1.0) : UsdGenParamField{1.0};
    size_t const curveBase = view->desc ? size_t(view->desc->firstCurve) : 0;
    size_t const firstCv = view->desc ? size_t(view->desc->firstCv) : 0;
    auto const *cvOff = view->cvOffsets;
    bool const ragged = cvOff != nullptr;
    bool const ready = cap.result.size() == size_t(cap.upstreamCvs) * 3;
    for (uint32_t c = 0; c < view->curveCount; ++c) {
        size_t const n = ragged ? size_t(cvOff[c + 1] - cvOff[c]) : size_t(view->cvCount);
        size_t const g = ragged ? size_t(cvOff[c] - cvOff[0]) : view->Cv(c, 0);
        size_t const curve = curveBase + c;
        for (size_t i = 0; i < n; ++i) {
            size_t const o = g + i;
            size_t const cv = firstCv + o;
            float const in[3] = {view->inPx[o], view->inPy[o], view->inPz[o]};
            float const m = ready
                ? std::clamp(float(maskField.Value(curve, cv)), 0.0f, 1.0f) : 0.0f;
            if (m == 0.0f || cv * 3 + 2 >= cap.result.size()) {
                view->px[o] = in[0]; view->py[o] = in[1]; view->pz[o] = in[2];
                continue;
            }
            float const *r = &cap.result[cv * 3];
            view->px[o] = m == 1.0f ? r[0] : in[0] + (r[0] - in[0]) * m;
            view->py[o] = m == 1.0f ? r[1] : in[1] + (r[1] - in[1]) * m;
            view->pz[o] = m == 1.0f ? r[2] : in[2] + (r[2] - in[2]) * m;
        }
        for (uint32_t slot = 0; slot < view->outCount && slot < cap.clumpId.size(); ++slot) {
            if (!view->outI || !view->outI[slot]) continue;
            auto const &ids = cap.clumpId[slot];
            view->outI[slot][c] = curve < ids.size() ? ids[curve] : -1;
        }
    }
}

}  // namespace usdGen
