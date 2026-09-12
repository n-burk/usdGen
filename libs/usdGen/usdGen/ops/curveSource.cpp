#include "usdGen/ops/curveSource.h"

#include "usdGen/opRegistry.h"
#include "usdGen/curveBuffer.h"
#include "usdGenMath/usdGenMath/kernels.h"
#include "usdGen/maskParams.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/getenv.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

namespace {

struct UsdGenCurveSourceCapture final : public UsdGenCapture
{
   std::unique_ptr<UsdGenCapture> Clone() const override
   {
      auto c = std::make_unique<UsdGenCurveSourceCapture>();
      *c = *this;
      return c;
   }
   bool OwnsBuffer() const override { return true; }
};

}  // namespace

UsdGenCurveSourceOp::UsdGenCurveSourceOp()
   : topologyParameters_(UsdGenBaseTopologyParams()),
     valueParameters_(UsdGenBaseValueParams())
{
   topologyParameters_.push_back(TfToken("enabled"));
   topologyParameters_.push_back(TfToken("mode"));
   topologyParameters_.push_back(TfToken("usdGen:useRest"));
   auto topologyMask = UsdGenMaskTopologyParams();
   topologyParameters_.push_back(TfToken("resampleTo"));
   topologyParameters_.insert(topologyParameters_.end(),
                              topologyMask.begin(), topologyMask.end());
   auto valueMask = UsdGenMaskValueParams();
   valueParameters_.insert(valueParameters_.end(), valueMask.begin(), valueMask.end());
}

UsdGenEpoch UsdGenCurveSourceOp::CaptureDigest(UsdGenCaptureContext const &ctx) const
{
   UsdGenParamView const *p = ctx.params ? &*ctx.params : nullptr;
   uint64_t h = 1469598103934665603ULL;
   auto feed = [&](std::string const &k, uint64_t v) {
      h ^= v; h *= 0x100000001b3ULL;
      h ^= k.size(); h *= 0x100000001b3ULL;
   };
   feed("mode", p ? static_cast<uint64_t>(p->GetToken(TfToken("mode"), TfToken("load")).Hash()) : 0);
   feed("seed", ctx.seed);
   return {h, h ^ 0x9E3779B97F4A7C15ull};
}

bool UsdGenCurveSourceOp::Bind(UsdGenParamView const &params, UsdGenDiagnostics *diag)
{
   (void)params; (void)diag;
   return true;
}

std::unique_ptr<UsdGenCapture> UsdGenCurveSourceOp::CreateCapture() const
{
   return std::make_unique<UsdGenCurveSourceCapture>();
}

uint32_t UsdGenCurveSourceOp::PlanesTouched() const
{
   return kPlanePoints | kPlaneWidths | kPlaneHairT;
}

TfSpan<const TfToken> UsdGenCurveSourceOp::TopologyParameters() const
{
   return TfSpan<const TfToken>(topologyParameters_.data(), topologyParameters_.size());
}

TfSpan<const TfToken> UsdGenCurveSourceOp::ValueParameters() const
{
   return TfSpan<const TfToken>(valueParameters_.data(), valueParameters_.size());
}

bool UsdGenCurveSourceOp::Capture(UsdGenCaptureContext const &ctx,
                                  UsdGenCurveBuffer const &upstream,
                                  UsdGenCapture *out,
                                  UsdGenDiagnostics *diag)
{
   TF_UNUSED(upstream);
   if (!ctx.desc) {
      if (diag) diag->Error("UsdGenCurveSource::Capture: no graph description");
      return false;
   }
   if (ctx.surface >= ctx.desc->surfaces.size()) {
      if (diag) diag->Error("UsdGenCurveSource::Capture: no bound usdGen:surface");
      return false;
   }
   UsdGenSurfaceDesc const &surf = ctx.desc->surfaces[ctx.surface];
   if (surf.faceVertexCounts.empty() || surf.restPoints.empty()) {
      if (diag) diag->Warn(std::string("UsdGenCurveSource::Capture: surface '") +
                           surf.path.GetText() + "' has no topology; 0 roots");
      UsdGenCurveSourceCapture &cap = *static_cast<UsdGenCurveSourceCapture *>(out);
      UsdGenCurveBuffer &buf = cap.MutableBuffer();
      buf.topologyVersion += 1;
      buf.totalCurves = 0;
      buf.totalCvs = 0;
      return true;
   }
   int const resampleTo = ctx.params
       ? std::max(ctx.params->GetInt(TfToken("resampleTo"),
                                    ctx.params->GetInt(TfToken("usdGen:resampleTo"), 0)), 0)
       : 0;
   int totalCurves = static_cast<int>(surf.faceVertexCounts.size());
   int totalCvs = 0;
   if (resampleTo > 0) {
      size_t const uni = size_t(totalCurves) * size_t(resampleTo);
      if (uni > size_t(std::numeric_limits<int>::max())) {
         if (diag) diag->Error("UsdGenCurveSource::Capture: totalCurves * resampleTo exceeds INT_MAX");
         return false;  // reject; *buf left unwritten
      }
      totalCvs = int(uni);  // uniform: resampleTo CVs per curve
   } else {
      for (int c = 0; c < totalCurves; ++c) totalCvs += surf.faceVertexCounts[c];
   }
   UsdGenCurveSourceCapture &cap = *static_cast<UsdGenCurveSourceCapture *>(out);
   UsdGenCurveBuffer &buf = cap.MutableBuffer();
   buf.topologyVersion += 1;
   buf.totalCurves = totalCurves;
   buf.totalCvs = totalCvs;
   // 04 §2.4 `usdGen:resampleTo`: 0 = keep the source CV counts -> ragged
   // (cvOffsets = {0, cumsum(counts)}, size totalCurves+1, [totalCurves] ==
   // totalCvs; chunks stay cvCount == 0). > 0 = uniform resample at capture;
   // cvOffsets stays empty (uniform fast path).
   if (resampleTo == 0) {
      buf.cvOffsets.assign(totalCurves + 1, 0);
      for (int c = 0; c < totalCurves; ++c)
         buf.cvOffsets[c + 1] = buf.cvOffsets[c] + surf.faceVertexCounts[c];
   } else {
      buf.cvOffsets.clear();
   }
   buf.px.resize(totalCvs);
   buf.py.resize(totalCvs);
   buf.pz.resize(totalCvs);
   buf.width.resize(totalCvs);
   buf.hairT.resize(totalCvs);
   size_t srcBase = 0, dst = 0;
   for (int c = 0; c < totalCurves; ++c) {
      int const srcCount = surf.faceVertexCounts[c];
      int const n = resampleTo > 0 ? resampleTo : srcCount;
      for (int i = 0; i < n; ++i) {
         GfVec3f p(0.0f);
         if (srcCount > 0) {
            // ragged: source CV i verbatim (t == i, f == 0); uniform: walk
            // the source polyline at an even index step (linear resample).
            double t = (resampleTo > 0 && srcCount > 1 && n > 1)
                ? double(i) * double(srcCount - 1) / double(n - 1)
                : double(resampleTo == 0 ? i : 0);
            int j = static_cast<int>(t);
            if (j > srcCount - 1) j = srcCount - 1;
            int const j2 = std::min(j + 1, srcCount - 1);
            float const f = float(t - double(j));
            GfVec3f const &a = surf.restPoints[srcBase + size_t(j)];
            GfVec3f const &b = surf.restPoints[srcBase + size_t(j2)];
            p = a + f * (b - a);
         }
         buf.px[dst] = p[0];
         buf.py[dst] = p[1];
         buf.pz[dst] = p[2];
         if (!buf.width.empty()) buf.width[dst] = 1.0f;
         if (!buf.hairT.empty()) buf.hairT[dst] = 0.5f;
         ++dst;
      }
      srcBase += size_t(std::max(srcCount, 0));
   }
   return true;
}

void UsdGenCurveSourceOp::Evaluate(UsdGenEvalContext const &ctx,
                                   UsdGenCapture const &capture,
                                   UsdGenChunkView *view) const
{
   (void)ctx; (void)capture; (void)view;
}

}  // namespace usdGen
