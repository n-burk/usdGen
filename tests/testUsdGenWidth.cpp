// testUsdGenWidth — T0 value test for the UsdGenWidthOp root->tip profile
// (plan 04 §2.11, schema `UsdGenWidth`):
//
//   constant width; root->tip ramp over hairT (`width:knots`); the
//   `width:interpolation` token is honoured (linear vs default catmullRom);
//   taper fall-off; root/tip linear scale; replace (set) vs multiply.
//
// Tier T0: op kernel directly (no scheduler, no Hydra, no stage — gate B-1).
// SeExpr-driven width is NOT covered here: expressions evaluate at capture
// through UsdGenExprOp (`returnType = width`, M8) / UsdGenExprMap (M4) and
// are owned by gate L-3 (`13-codebase-alignment.md` §2); this test pins the
// combiner they bake into.

#include "usdGen/op.h"
#include "usdGen/opRegistry.h"
#include "usdGen/curveBuffer.h"
#include "usdGen/graphDesc.h"

#include "pxr/pxr.h"
#include "pxr/base/vt/array.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {
namespace {

bool g_allOk = true;
void Check(bool ok, std::string const &what)
{
    if (!ok) {
        g_allOk = false;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

constexpr float kEps = 1e-5f;
bool Near(float a, float b) { return std::fabs(a - b) <= kEps; }

/// Drive one Width evaluate over 2 curves x 4 CVs with hairT = i/3.
/// `params` are (prefix-stripped) UsdGenParamValues; `inWidth` null means
/// no upstream widths (set path). Returns the 8 output widths.
std::vector<float> RunWidth(std::vector<UsdGenParamValue> const &params,
                            float const *inWidth)
{
    std::unique_ptr<UsdGenOp> op = CreateWidthOp();

    UsdGenNodeDesc node;
    node.path = SdfPath("/groom/width");
    node.type = TfToken("UsdGenWidth");
    node.params = params;
    UsdGenParamView view;
    view.node = &node;

    UsdGenDiagnostics diag;
    if (!op->Bind(view, &diag)) {
        std::printf("FAIL: Bind rejected params\n");
        g_allOk = false;
        return {};
    }

    UsdGenCaptureContext cctx;
    cctx.params = &view;
    UsdGenCurveBuffer upstream;  // empty: default (constant) mask
    std::unique_ptr<UsdGenCapture> cap = op->CreateCapture();
    if (!op->Capture(cctx, upstream, cap.get(), &diag)) {
        std::printf("FAIL: Capture failed\n");
        g_allOk = false;
        return {};
    }

    constexpr uint32_t kCurves = 2, kCv = 4;
    std::vector<float> hairT(kCurves * kCv), out(kCurves * kCv, -1.0f);
    for (uint32_t c = 0; c < kCurves; ++c)
        for (uint32_t i = 0; i < kCv; ++i)
            hairT[c * kCv + i] = float(i) / float(kCv - 1);

    UsdGenChunkView chunk{};
    chunk.curveCount = kCurves;
    chunk.cvCount = kCv;
    chunk.hairT = hairT.data();
    chunk.inWidth = inWidth;
    chunk.width = out.data();
    chunk.curveMask = nullptr;  // == 1.0

    UsdGenEvalContext ectx;
    ectx.params = &view;
    op->Evaluate(ectx, *cap, &chunk);
    return out;
}

UsdGenParamValue P(char const *name, VtValue v)
{
    return UsdGenParamValue{TfToken(name), std::move(v), false};
}

bool AllNear(std::vector<float> const &got, float expected,
             std::string const &what)
{
    bool ok = got.size() == 8;
    if (ok)
        for (float w : got)
            if (!Near(w, expected)) { ok = false; break; }
    Check(ok, what);
    return ok;
}

}  // namespace
}  // namespace usdGen

using namespace usdGen;

int main()
{
    // 1. Constant width: no knots -> flat 1.0 LUT, every CV == width.
    {
        auto out = RunWidth({P("width", VtValue(0.02))}, nullptr);
        AllNear(out, 0.02f, "constant width=0.02 on all 8 CVs");
    }

    // 2. Root->tip ramp: knots (0,0)->(1,1), linear -> width == 0.02 * t.
    {
        VtVec2fArray knots(2);
        knots[0] = GfVec2f(0.0f, 0.0f);
        knots[1] = GfVec2f(1.0f, 1.0f);
        auto out = RunWidth({P("width", VtValue(0.02)),
                             P("width:knots", VtValue(knots)),
                             P("width:interpolation",
                               VtValue(TfToken("linear")))}, nullptr);
        bool ok = out.size() == 8;
        if (ok)
            for (size_t k = 0; k < 8; ++k) {
                float const t = float(k % 4) / 3.0f;
                if (!Near(out[k], 0.02f * t)) { ok = false; break; }
            }
        Check(ok, "linear ramp (0,0)->(1,1): widths == 0.02*t (root->tip)");
    }

    // 3. The interpolation token is honoured: a V profile is exactly 1/3 at
    // t=1/3 (CV 1 of 4) under linear, and differs under (default)
    // catmullRom. Guards the 2026-09-12 fix (reader used
    // "width:knots:interpolation", matching no C1 property, so every ramp
    // silently built catmullRom).
    {
        VtVec2fArray knots(3);
        knots[0] = GfVec2f(0.0f, 1.0f);
        knots[1] = GfVec2f(0.5f, 0.0f);
        knots[2] = GfVec2f(1.0f, 1.0f);
        auto lin = RunWidth({P("width", VtValue(0.02)),
                             P("width:knots", VtValue(knots)),
                             P("width:interpolation",
                               VtValue(TfToken("linear")))}, nullptr);
        auto cr = RunWidth({P("width", VtValue(0.02)),
                            P("width:knots", VtValue(knots))}, nullptr);
        bool okLin = lin.size() == 8 && Near(lin[1], 0.02f / 3.0f);
        Check(okLin, "linear V ramp: t=1/3 gives exactly one-third width");
        bool okCr = cr.size() == 8 && std::fabs(cr[1] - 0.02f / 3.0f) > 1e-3f;
        Check(okCr, "default (catmullRom) V ramp differs from linear at t=1/3");
    }

    // 4. Taper: taper=1 from taperStart=0 -> w *= (1-t); tip pinches to 0.
    {
        auto out = RunWidth({P("width", VtValue(0.02)),
                             P("taper", VtValue(1.0)),
                             P("taperStart", VtValue(0.0))}, nullptr);
        bool ok = out.size() == 8;
        if (ok)
            for (size_t k = 0; k < 8; ++k) {
                float const t = float(k % 4) / 3.0f;
                if (!Near(out[k], 0.02f * (1.0f - t))) { ok = false; break; }
            }
        Check(ok, "taper=1 from root: widths == 0.02*(1-t), tip == 0");
    }

    // 5. Root/tip scale: 0 at root, 2x at tip, linear in hairT.
    {
        auto out = RunWidth({P("width", VtValue(0.02)),
                             P("rootScale", VtValue(0.0)),
                             P("tipScale", VtValue(2.0))}, nullptr);
        bool ok = out.size() == 8;
        if (ok)
            for (size_t k = 0; k < 8; ++k) {
                float const t = float(k % 4) / 3.0f;
                if (!Near(out[k], 0.04f * t)) { ok = false; break; }
            }
        Check(ok, "rootScale=0 tipScale=2: widths == 0.04*t");
    }

    // 6. replace=false multiplies upstream widths instead of setting.
    {
        float const in[8] = {3, 3, 3, 3, 3, 3, 3, 3};
        auto out = RunWidth({P("width", VtValue(2.0)),
                             P("replace", VtValue(false))}, in);
        AllNear(out, 6.0f, "replace=false: widths == 2.0 * inWidth");
    }

    std::printf(g_allOk ? "testUsdGenWidth: PASS\n" : "testUsdGenWidth: FAILED\n");
    return g_allOk ? 0 : 1;
}
