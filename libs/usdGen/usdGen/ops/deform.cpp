#include "usdGen/ops/deform.h"

#include "usdGen/opRegistry.h"
#include "usdGen/curveBuffer.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "usdGen/maskParams.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/getenv.h"

#include <algorithm>
#include <cstring>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

struct UsdGenDeformCapture final : public UsdGenCapture
{
   std::unique_ptr<UsdGenCapture> Clone() const override
   {
      auto c = std::make_unique<UsdGenDeformCapture>();
      *c = *this;
      return c;
   }
   bool OwnsBuffer() const override { return true; }
};

}  // namespace

UsdGenEpoch UsdGenDeformOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
   UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
   uint64_t h = 1469598103934665603ULL;
   auto feed = [&](std::string const &k, uint64_t v) {
      h ^= v; h *= 0x100000001b3ULL;
      h ^= k.size(); h *= 0x100000001b3ULL;
   };
   feed("seed", ctx.seed);
   if (p) {
      feed("blend", p->GetDouble(TfToken("blend"), 1.0));
      feed("usdGen:useRest", p->GetBool(TfToken("usdGen:useRest"), true) ? 1u : 0u);
   }
   return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenDeformOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
   (void)params; (void)diag;
   return true;
}

std::unique_ptr<UsdGenCapture> UsdGenDeformOp::CreateCapture() const
{
   return std::make_unique<UsdGenDeformCapture>();
}

uint32_t UsdGenDeformOp::PlanesTouched() const
{
   return kPlanePoints;
}

TfSpan<const TfToken> UsdGenDeformOp::TopologyParameters() const
{
   static TfTokenVector params = [] {
      TfTokenVector v = UsdGenBaseTopologyParams();
      v.push_back(TfToken("enabled"));
      v.push_back(TfToken("blend"));
      v.push_back(TfToken("usdGen:useRest"));
      return v;
   }();
   return TfSpan<const TfToken>(params.data(), params.size());
}

TfSpan<const TfToken> UsdGenDeformOp::ValueParameters() const
{
   static TfTokenVector params = [] {
      TfTokenVector v = UsdGenBaseValueParams();
      return v;
   }();
   return TfSpan<const TfToken>(params.data(), params.size());
}

bool UsdGenDeformOp::Capture(UsdGenCaptureContext const &ctx,
                              UsdGenCurveBuffer const &upstream,
                              UsdGenCapture *out,
                              UsdGenDiagnostics *diag)
{
   TF_UNUSED(upstream);
      UsdGenParamView const *p = ctx.params ? ctx.params : nullptr;
   if (!ctx.desc) {
      if (diag) diag->Error("UsdGenDeform::Capture: no graph description");
      return false;
   }
   if (ctx.surface >= ctx.desc->surfaces.size()) {
      if (diag) diag->Error("UsdGenDeform::Capture: no bound usdGen:surface");
      return false;
   }
   UsdGenSurfaceDesc const &surf = ctx.desc->surfaces[ctx.surface];
   if (surf.faceVertexCounts.empty() || surf.restPoints.empty()) {
      if (diag) diag->Warn(std::string("UsdGenDeform::Capture: surface '") +
                           surf.path.GetText() + "' has no topology; 0 roots");
      UsdGenDeformCapture &cap = *static_cast<UsdGenDeformCapture *>(out);
      UsdGenCurveBuffer &buf = cap.MutableBuffer();
      buf.topologyVersion += 1;
      buf.totalCurves = 0;
      buf.totalCvs = 0;
      return true;
   }
   int totalCurves = static_cast<int>(surf.faceVertexCounts.size());
   int totalCvs = 0;
   for (int c = 0; c < totalCurves; ++c) totalCvs += surf.faceVertexCounts[c];
   UsdGenDeformCapture &cap = *static_cast<UsdGenDeformCapture *>(out);
   UsdGenCurveBuffer &buf = cap.MutableBuffer();
   buf.topologyVersion += 1;
   buf.totalCurves = totalCurves;
   buf.totalCvs = totalCvs;
   buf.px.resize(totalCvs);
   buf.py.resize(totalCvs);
   buf.pz.resize(totalCvs);
   buf.width.resize(totalCvs);
   buf.hairT.resize(totalCvs);
   for (int c = 0; c < totalCurves; ++c) {
      int cvc = surf.faceVertexCounts[c];
      size_t base = 0;
      for (int k = 0; k < c; ++k) base += surf.faceVertexCounts[k];
      for (int i = 0; i < cvc; ++i) {
         GfVec3f const &restP = surf.restPoints[base + i];
         float blend = 1.0f;
         if (p) {
            blend = static_cast<float>(p->GetDouble(TfToken("blend"), 1.0));
         }
         buf.px[base + i] = restP[0];
         buf.py[base + i] = restP[1] + blend * 0.1f;
         buf.pz[base + i] = restP[2];
         if (!buf.width.empty()) buf.width[base + i] = 1.0f;
         if (!buf.hairT.empty()) buf.hairT[base + i] = 0.5f;
      }
   }
   return true;
}

void UsdGenDeformOp::Evaluate(UsdGenEvalContext const &ctx,
                              UsdGenCapture const &capture,
                              UsdGenChunkView *view) const
{
   (void)ctx; (void)capture; (void)view;
}

}  // namespace usdGen
