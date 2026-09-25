// usdGen — UsdGenDeform. plan/examples/rbf-deformation.md.
//
// CUDA lane: the persistent RBF library in cudaExecution.cpp deforms the groom
// by samples of the bound surface; this class only supplies metadata there.
//
// CPU lane: drivers are usdGen:guides, or the bound surface when no guides
// are supplied. Surface and groom must share an object-space domain. Capture
// resolves the whole result, because every CV reads the same field:
//
//   samples  up to usdGen:rbfSamples driver CVs, farthest-point sampled at
//            rest (primvars:rest, else the Default-time points), in the
//            description's space;
//   field    rbf::CubicField bound at those rest positions and solved for
//            their current positions: it passes through every sample and
//            reproduces any affine motion of the drivers exactly;
//   strands  every incoming CV x moves to x + D(x), D = F - identity. With
//            usdGen:lockRoots the whole strand is shifted back by D(root), so
//            the root stays where it was and the strand bends above it.
//
// The capture digest carries the driver content, so an animated driver
// re-captures on every frame it moves; the factorization is kept while the
// rest samples do not change. Evaluate applies usdGen:mask.
#include "usdGen/ops/deform.h"

#include "usdGen/opParams.h"
#include "usdGen/ops/opUtil.h"
#include "usdGen/scheduler.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/range3d.h"
#include "pxr/base/trace/trace.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace usdGen {

namespace {

const TfToken sRbfSamples{"rbfSamples"}, sLockRoots{"lockRoots"}, sMask{"mask"},
    sGuides{"guides"};

struct UsdGenDeformCapture final : public UsdGenCapture
{
    std::vector<float> result;                    // 3 * totalCvs
    uint64_t upstreamTopologyVersion = 0;
    uint64_t upstreamValueVersion = 0;
    uint32_t upstreamCurves = 0;
    uint32_t upstreamCvs = 0;
    size_t samples = 0;                           // RBF samples used

    std::unique_ptr<UsdGenCapture> Clone() const override
    {
        return std::make_unique<UsdGenDeformCapture>(*this);
    }
    bool ValidForTopology(UsdGenCurveBuffer const &upstream) const override
    {
        return upstream.topologyVersion == upstreamTopologyVersion &&
               upstream.valueVersion == upstreamValueVersion &&
               upstream.totalCurves == upstreamCurves &&
               upstream.totalCvs == upstreamCvs;
    }
};

template <class F>
void ParallelFor(UsdGenWorkDispatcher *dispatcher, size_t count, F const &body)
{
    if (!dispatcher || count < 64) {
        for (size_t i = 0; i < count; ++i) body(i);
        return;
    }
    struct Payload { F const *body; } payload{&body};
    dispatcher->ParallelFor(count, [](size_t i, void *p) {
        (*static_cast<Payload *>(p)->body)(i);
    }, &payload);
}

void MixArray(opUtil::Digest *d, void const *data, size_t bytes)
{
    auto const *p = static_cast<unsigned char const *>(data);
    for (size_t i = 0; i < bytes; ++i) d->Mix(uint64_t(p[i]));
}

UsdGenCurveSetDesc const *FindCurves(UsdGenGraphDesc const *desc, SdfPath const &path)
{
    if (!desc) return nullptr;
    for (UsdGenCurveSetDesc const &curves : desc->curveSets)
        if (curves.path == path) return &curves;
    return nullptr;
}

/// Where the drivers' points live relative to the description: their prim's
/// world matrix, brought into the description's space. A transform animation
/// moves rest and pose together; animate the points to deform.
GfMatrix4d DriverToGroom(UsdGenGraphDesc const *desc, SdfPath const &path)
{
    UsdGenCurveSetDesc const *curves = FindCurves(desc, path);
    return curves ? curves->worldMatrix * desc->xformMatrix.GetInverse() : GfMatrix4d(1.0);
}

}  // namespace

UsdGenDeformOp::UsdGenDeformOp()
    : topologyParameters_(UsdGenBaseTopologyParams()),
      valueParameters_(UsdGenBaseValueParams())
{
    topologyParameters_.push_back(sRbfSamples);
    valueParameters_.push_back(TfToken("enabled"));
    valueParameters_.push_back(sLockRoots);
    valueParameters_.push_back(sMask);   // operator envelope (02 §2.13)
}

bool UsdGenDeformOp::Bind(UsdGenParamView const& params, UsdGenDiagnostics* diagnostics) {
    auto fail = [&](char const *message) {
        if (diagnostics) diagnostics->Error(message);
        return false;
    };
    if (!params.node || !params.node->mode.IsEmpty())
        return fail("UsdGenDeform has no mode property; it is always RBF");
    bool const cuda = params.desc &&
        params.desc->executionBackend == UsdGenExecutionBackend::Cuda;
    bool const guided = !params.node->curves.empty() || !params.node->references.empty();
    if (cuda && guided)
        return fail("UsdGenDeform: guide-driven RBF (usdGen:guides) runs on the CPU lane; "
                    "the CUDA lane samples the bound surface");
    if (!cuda && !guided && (!params.desc || params.desc->surfaces.empty()))
        return fail("UsdGenDeform: requires usdGen:guides or a bound usdGen:surface");
    return true;
}

TfSpan<const TfToken> UsdGenDeformOp::TopologyParameters() const {
    return TfSpan<const TfToken>(topologyParameters_.data(), topologyParameters_.size());
}
TfSpan<const TfToken> UsdGenDeformOp::ValueParameters() const {
    return TfSpan<const TfToken>(valueParameters_.data(), valueParameters_.size());
}
TfSpan<const TfToken> UsdGenDeformOp::ReferenceInputs() const {
    static TfToken const slots[] = {sGuides};
    return TfSpan<const TfToken>(slots, 1);
}

UsdGenEpoch UsdGenDeformOp::CaptureDigest(UsdGenCaptureContext const& ctx) const {
    // The CUDA rest cache is keyed by its own surface identity; on the CPU
    // lane the capture follows the drivers' content, which moves every frame
    // they are animated.
    opUtil::Digest d;
    if (UsdGenParamView const *p = ctx.params) {
        d.Mix(uint64_t(p->GetInt(sRbfSamples, 100)));
        d.Mix(uint64_t(p->GetBool(sLockRoots, true)));
    }
    d.Mix(ctx.upstreamGeneration);
    if (ctx.referenceCount == 0 && ctx.desc && ctx.surface < ctx.desc->surfaces.size()) {
        auto const &surface = ctx.desc->surfaces[ctx.surface];
        MixArray(&d, surface.restPoints.cdata(), surface.restPoints.size() * sizeof(GfVec3f));
        MixArray(&d, surface.points.cdata(), surface.points.size() * sizeof(GfVec3f));
        d.Mix(uint64_t(surface.restFromCurrentPoints));
        GfMatrix4d const relative = surface.worldMatrix * ctx.desc->xformMatrix.GetInverse();
        MixArray(&d, relative.GetArray(), 16 * sizeof(double));
    }
    for (uint32_t r = 0; r < ctx.referenceCount; ++r) {
        UsdGenResolvedReferenceValue const *value =
            ctx.resolvedReferences ? ctx.resolvedReferences[r] : nullptr;
        if (!value || !value->value) { d.Mix(uint64_t(~0ull)); continue; }
        UsdGenCurveBuffer const &b = value->value->buffer;
        d.Mix(uint64_t(b.totalCurves));
        d.Mix(uint64_t(b.totalCvs));
        MixArray(&d, b.px.cdata(), b.px.size() * sizeof(float));
        MixArray(&d, b.py.cdata(), b.py.size() * sizeof(float));
        MixArray(&d, b.pz.cdata(), b.pz.size() * sizeof(float));
        MixArray(&d, b.rest.cdata(), b.rest.size() * sizeof(GfVec3f));
        GfMatrix4d const m = DriverToGroom(ctx.desc, value->path);
        MixArray(&d, m.GetArray(), 16 * sizeof(double));
    }
    return d.Epoch(0x446566726dull);
}

std::unique_ptr<UsdGenCapture> UsdGenDeformOp::CreateCapture() const {
    return std::make_unique<UsdGenDeformCapture>();
}
uint32_t UsdGenDeformOp::PlanesTouched() const { return kPlanePoints; }

bool UsdGenDeformOp::Capture(UsdGenCaptureContext const& ctx, UsdGenCurveBuffer const& upstream,
                             UsdGenCapture* out, UsdGenDiagnostics* diagnostics) {
    auto fail = [&](std::string const &message) {
        if (diagnostics) diagnostics->Error("UsdGenDeform: " + message);
        return false;
    };
    UsdGenParamView const *p = ctx.params;
    if (ctx.desc && ctx.desc->executionBackend == UsdGenExecutionBackend::Cuda)
        return fail("the CUDA RBF executor owns this node; it cannot run through the host scheduler");
    bool const surfaceDriven = ctx.referenceCount == 0;
    if (!surfaceDriven && (ctx.referenceCount != 1 || !ctx.resolvedReferences ||
        !ctx.resolvedReferences[0] || !ctx.resolvedReferences[0]->value))
        return fail("usdGen:guides must resolve to one curve input");
    auto &cap = *static_cast<UsdGenDeformCapture *>(out);
    cap.upstreamTopologyVersion = upstream.topologyVersion;
    cap.upstreamValueVersion = upstream.valueVersion;
    cap.upstreamCurves = upstream.totalCurves;
    cap.upstreamCvs = upstream.totalCvs;
    cap.result.clear();
    cap.samples = 0;

    int const budget = p ? p->GetInt(sRbfSamples, 100) : 100;
    if (budget < 4) return fail("usdGen:rbfSamples must be at least 4");

    // --- driver samples, in the description's space --------------------------
    auto const solveField = [&]() -> bool {
        TRACE_SCOPE("usdGen deform: bind and solve the field");
        std::vector<GfVec3d> driverRest, driverNow;
        std::string driverLabel;
        if (surfaceDriven) {
            if (!ctx.desc || ctx.surface >= ctx.desc->surfaces.size())
                return fail("surface-driven RBF requires one bound usdGen:surface");
            auto const &surface = ctx.desc->surfaces[ctx.surface];
            driverLabel = "usdGen:surface " + surface.path.GetString();
            if (surface.restFromCurrentPoints || surface.restPoints.empty())
                return fail(driverLabel + " requires Default-time rest points and UsdGenRestAPI");
            if (surface.points.size() != surface.restPoints.size())
                return fail(driverLabel + " animated/rest vertex counts differ");
            // Scatter/Grow produce rest-local points. A shared animated parent
            // belongs to publication, not the RBF field (avoid double motion).
            GfMatrix4d const relative = surface.worldMatrix * ctx.desc->xformMatrix.GetInverse();
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
                if (std::abs(relative[r][c] - (r == c ? 1.0 : 0.0)) > 1e-9)
                    return fail(driverLabel + " must share the groom's object space; "
                                "put the surface and groom under the same animated Xform");
            driverRest.assign(surface.restPoints.begin(), surface.restPoints.end());
            driverNow.assign(surface.points.begin(), surface.points.end());
        } else {
            UsdGenResolvedReferenceValue const &reference = *ctx.resolvedReferences[0];
            driverLabel = "usdGen:guides " + reference.path.GetString();
            UsdGenCurveBuffer const &drivers = reference.value->buffer;
            if (UsdGenCurveSetDesc const *curves = FindCurves(ctx.desc, reference.path);
                curves && curves->restFromCurrentPoints)
                return fail("usdGen:guides " + reference.path.GetString() +
                            " has no rest pose to bind; apply UsdGenCurveAPI to it (its rest is "
                            "primvars:rest, else the Default-time points)");
            if (drivers.px.size() != drivers.totalCvs || drivers.py.size() != drivers.totalCvs ||
                drivers.pz.size() != drivers.totalCvs)
                return fail("the driver curves' point planes do not match their CV count");
            GfMatrix4d const toGroom = DriverToGroom(ctx.desc, reference.path);
            driverRest.resize(drivers.totalCvs); driverNow.resize(drivers.totalCvs);
            for (size_t i = 0; i < drivers.totalCvs; ++i) {
                driverRest[i] = toGroom.Transform(GfVec3d(opUtil::RestPoint(drivers, i)));
                driverNow[i] = toGroom.Transform(GfVec3d(opUtil::Point(drivers, i)));
            }
        }
        GfRange3d extent;
        for (GfVec3d const &point : driverRest) extent.UnionWith(point);
        double const size = extent.IsEmpty() ? 0.0 : extent.GetSize().GetLength();
        std::vector<size_t> const chosen =
            rbf::SelectSamples(driverRest, size_t(budget), std::max(1e-12, size * 1e-7));
        if (chosen.size() < 4)
            return fail(driverLabel + " has " +
                        std::to_string(chosen.size()) +
                        " distinct samples; the RBF needs at least four that span 3D");
        std::vector<GfVec3d> rest(chosen.size()), now(chosen.size());
        for (size_t k = 0; k < chosen.size(); ++k) {
            rest[k] = driverRest[chosen[k]];
            now[k] = driverNow[chosen[k]];
        }
        std::string error;
        if (!field_.Bound() || rest != boundRest_) {
            boundRest_.clear();
            if (!field_.Bind(rest, &error))
                return fail(driverLabel + ": " + error);
            boundRest_ = rest;
        }
        if (!field_.Solve(now, &error)) return fail(error);
        cap.samples = rest.size();
        return true;
    };
    if (!solveField()) return false;

    // --- strands ---------------------------------------------------------------
    TRACE_SCOPE("usdGen deform: displace strands");
    std::string error;
    size_t const R = upstream.totalCurves;
    size_t const totalCvs = upstream.totalCvs;
    if (R == 0) return true;
    std::vector<uint32_t> spans;
    if (!opUtil::CurveSpans(upstream, &spans, &error)) return fail(error);
    if (upstream.px.size() != totalCvs || upstream.py.size() != totalCvs ||
        upstream.pz.size() != totalCvs)
        return fail("input point planes do not match the CV count");
    UsdGenParamField const lock = p
        ? p->GetScalarField(sLockRoots, p->GetBool(sLockRoots, true) ? 1.0 : 0.0)
        : UsdGenParamField{1.0};

    cap.result.resize(totalCvs * 3);
    rbf::CubicField const &field = field_;
    std::vector<float> &result = cap.result;
    ParallelFor(ctx.dispatcher, R, [&](size_t c) {
        size_t const first = spans[c], last = spans[c + 1];
        if (first >= last) return;
        // The root's displacement serves as both the root CV's own and,
        // with usdGen:lockRoots, the shift of the whole strand.
        GfVec3d const rootDisplacement = field.Displacement(GfVec3d(opUtil::Point(upstream, first)));
        GfVec3d const shift = lock.Value(c, first) != 0.0 ? rootDisplacement : GfVec3d(0.0);
        for (size_t cv = first; cv < last; ++cv) {
            GfVec3d const x(opUtil::Point(upstream, cv));
            GfVec3d const moved =
                x + (cv == first ? rootDisplacement : field.Displacement(x)) - shift;
            result[cv * 3] = float(moved[0]);
            result[cv * 3 + 1] = float(moved[1]);
            result[cv * 3 + 2] = float(moved[2]);
        }
    });
    for (float v : result)
        if (!std::isfinite(v)) return fail("the deformation produced a non-finite point");
    return true;
}

void UsdGenDeformOp::Evaluate(UsdGenEvalContext const& ctx, UsdGenCapture const& captureIn,
                              UsdGenChunkView* view) const {
    auto const &cap = static_cast<UsdGenDeformCapture const &>(captureIn);
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
    }
}

} // namespace usdGen
