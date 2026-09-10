// testUsdGenAutoApply — gate SI-8 engine half (mask block semantics).
//
// The schema-side half of SI-8 (auto-apply of UsdGenMaskAPI on codeless
// types) is imaging work; this T1 body locks the ENGINE contract the mask
// block ultimately drives (02 §2.13, R16), engine-only (gate B-1):
//   1. UsdGenMaskSettingsFromParams maps the mask: params the desc builder
//      puts on a node into UsdGenMaskSettings.
//   2. EvaluateMask M1 form: random == 0 -> mask == 1 everywhere; the random
//      term is r = lerp(1, Draw01(seed, curveId), random) — deterministic
//      (recompute == recompute), seed-sensitive (seed 7 != seed 8), and at
//      random == 1 splits the population ~50/50 at mask >= 0.5.
//   3. M1 neutrality: invert / combine are neutral (mask unchanged, a
//      diagnostic names each).
//   4. mask:amount == 0 on the scatter -> zero live curves end-to-end
//      (plan/04 §5.2 "mask:amount == 0 ⇒ zero roots").
//   5. mask:random == 1 on grow with different seeds -> different terminal
//      payloads; same seed -> bit-identical (selection is deterministic).

#include "usdGen/compiler.h"
#include "usdGen/graph.h"
#include "usdGen/graphDesc.h"
#include "usdGen/mask.h"
#include "usdGen/op.h"
#include "usdGen/opRegistry.h"
#include "usdGen/scheduler.h"
#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using namespace usdGen;

namespace {

int g_failures = 0;

void Check(bool ok, std::string const &what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

bool HasDiag(UsdGenMaskResult const &r, std::string const &needle)
{
    for (std::string const &d : r.diagnostics)
        if (d.find(needle) != std::string::npos) return true;
    return false;
}

bool SameBytes(void const *a, void const *b, size_t n)
{
    return n == 0 || std::memcmp(a, b, n) == 0;
}

bool SamePlane(VtFloatArray const &a, VtFloatArray const &b)
{
    return a.size() == b.size() &&
           SameBytes(a.empty() ? nullptr : a.data(),
                     b.empty() ? nullptr : b.data(), a.size() * sizeof(float));
}

bool SameCurveIds(VtArray<uint64_t> const &a, VtArray<uint64_t> const &b)
{
    return a.size() == b.size() &&
           SameBytes(a.empty() ? nullptr : a.data(),
                     b.empty() ? nullptr : b.data(), a.size() * sizeof(uint64_t));
}

bool TerminalIdentical(UsdGenGraph const &a, UsdGenGraph &b)
{
    UsdGenCurveBuffer const &ba = a.Output();
    UsdGenCurveBuffer const &bb = b.Output();
    return ba.totalCurves == bb.totalCurves && ba.totalCvs == bb.totalCvs &&
           SamePlane(ba.px, bb.px) && SamePlane(ba.py, bb.py) &&
           SamePlane(ba.pz, bb.pz) && SamePlane(ba.width, bb.width) &&
           SamePlane(ba.hairT, bb.hairT) && SameCurveIds(ba.curveId, bb.curveId);
}

bool TerminalDiffers(UsdGenGraph const &a, UsdGenGraph &b)
{
    UsdGenCurveBuffer const &ba = a.Output();
    UsdGenCurveBuffer const &bb = b.Output();
    if (ba.totalCurves != bb.totalCurves || ba.totalCvs != bb.totalCvs) return true;
    return !(SamePlane(ba.px, bb.px) && SamePlane(ba.py, bb.py) &&
             SamePlane(ba.pz, bb.pz) && SamePlane(ba.width, bb.width) &&
             SamePlane(ba.hairT, bb.hairT));
}

// Small G3; maskParams carry extra params on the chosen node.
UsdGenGraphDesc MakeG3(int NX = 250, int NY = 10)
{
    UsdGenGraphDesc d;
    d.description = SdfPath("/groom");
    d.terminal = SdfPath("/groom/width");
    d.time = 0.0;

    UsdGenSurfaceDesc s;
    s.path = SdfPath("/groom/surface");
    s.id = 0;
    const int np = (NX + 1) * (NY + 1);
    s.restPoints = VtVec3fArray(np);
    s.uv = VtVec2fArray(np);
    for (int j = 0; j <= NY; ++j)
        for (int i = 0; i <= NX; ++i) {
            const int k = j * (NX + 1) + i;
            s.restPoints[k] = GfVec3f(i * 0.1f, j * 0.1f, 0.0f);
            s.uv[k] = GfVec2f(float(i) / NX, float(j) / NY);
        }
    s.faceVertexCounts = VtIntArray(NX * NY, 4);
    s.faceVertexIndices = VtIntArray(NX * NY * 4);
    for (int j = 0; j < NY; ++j)
        for (int i = 0; i < NX; ++i) {
            const int a = j * (NX + 1) + i;
            const int o = (j * NX + i) * 4;
            s.faceVertexIndices[o + 0] = a;
            s.faceVertexIndices[o + 1] = a + 1;
            s.faceVertexIndices[o + 2] = a + NX + 2;
            s.faceVertexIndices[o + 3] = a + NX + 1;
        }
    d.surfaces.push_back(std::move(s));

    auto addNode = [&](std::string const &name, TfToken type,
                       std::string const &input, int seed) {
        UsdGenNodeDesc n;
        n.path = SdfPath("/groom/" + name);
        n.type = type;
        n.enabled = true;
        n.blend = 1.0f;
        n.seed = seed;
        if (!input.empty()) n.inputs.push_back(SdfPath("/groom/" + input));
        if (type == TfToken("UsdGenScatter"))
            n.surfaces.push_back(SdfPath("/groom/surface"));
        d.nodes.push_back(std::move(n));
    };
    addNode("scatter", TfToken("UsdGenScatter"), "", 42);
    addNode("grow", TfToken("UsdGenGrow"), "scatter", 43);
    addNode("noise", TfToken("UsdGenNoise"), "grow", 44);
    addNode("length", TfToken("UsdGenLength"), "noise", 45);
    addNode("width", TfToken("UsdGenWidth"), "length", 46);

    auto setp = [&](std::string const &name, TfToken p, VtValue v) {
        for (auto &n : d.nodes)
            if (n.path == SdfPath("/groom/" + name))
                n.params.push_back(UsdGenParamValue{p, v, false});
    };
    setp("grow", TfToken("segments"), VtValue(8));
    setp("grow", TfToken("length"), VtValue(1.0));
    setp("noise", TfToken("noise:magnitude"), VtValue(0.05));
    setp("noise", TfToken("noise:frequency"), VtValue(3.0));
    setp("noise", TfToken("noise:octaves"), VtValue(2));
    setp("noise", TfToken("noise:correlation"), VtValue(0.5));
    setp("length", TfToken("length:mode"), VtValue(TfToken("scale")));
    setp("length", TfToken("length:value"), VtValue(1.2));
    setp("width", TfToken("width"), VtValue(0.02));
    return d;
}

void AddParam(UsdGenGraphDesc &d, std::string const &node, TfToken p, VtValue v)
{
    for (auto &n : d.nodes)
        if (n.path == SdfPath("/groom/" + node))
            n.params.push_back(UsdGenParamValue{p, v, false});
}

}  // namespace

int main()
{
    usdGenRegisterM1Operators();
    UsdGenScheduler scheduler(8);

    // ---- 1: settings mapping from node params ---------------------------------
    {
        UsdGenGraphDesc d = MakeG3();
        AddParam(d, "scatter", TfToken("mask:amount"), VtValue(0.25));
        AddParam(d, "scatter", TfToken("mask:invert"), VtValue(true));
        AddParam(d, "scatter", TfToken("mask:random"), VtValue(0.7));
        // Engine maps mask:randomSeed via params.GetInt -> author an int.
        AddParam(d, "scatter", TfToken("mask:randomSeed"), VtValue(1234));
        UsdGenCompiler c;
        UsdGenGraph g;
        UsdGenCompileResult const r = c.Compile(d, &g);
        if (!r.ok) { std::printf("FAIL: compile with mask params\n"); return 1; }
        UsdGenMaskSettings const st = UsdGenMaskSettingsFromParams(
            g.Node(g.NodeIdForPath(SdfPath("/groom/scatter"))).paramView);
        Check(st.amount == 0.25f, "mask:amount 0.25 mapped");
        Check(st.invert == true, "mask:invert true mapped");
        Check(st.random == 0.7f, "mask:random 0.7 mapped");
        Check(st.randomSeed == 1234, "mask:randomSeed 1234 mapped");
    }

    // ---- 2: EvaluateMask M1 form (02 §2.13) ------------------------------------
    {
        VtArray<uint64_t> ids(20000);
        for (size_t i = 0; i < ids.size(); ++i) ids[i] = uint64_t(i);

        UsdGenMaskSettings st;
        st.random = 0.0f;
        UsdGenMaskResult const off = EvaluateMask(st, ids, {}, {}, 0.0);
        bool allOne = true;
        for (float m : off.curveMask) allOne = allOne && m == 1.0f;
        Check(allOne, "random == 0 -> mask == 1 everywhere");

        st.random = 1.0f;
        st.randomSeed = 7;
        UsdGenMaskResult const a = EvaluateMask(st, ids, {}, {}, 0.0);
        UsdGenMaskResult const a2 = EvaluateMask(st, ids, {}, {}, 0.0);
        bool inRange = a.curveMask.size() == ids.size();
        size_t half = 0;
        for (float m : a.curveMask) {
            inRange = inRange && m >= 0.0f && m < 1.0f;
            if (m >= 0.5f) ++half;
        }
        Check(inRange, "random == 1 -> mask in [0,1)");
        Check(a.curveMask == a2.curveMask, "draw term deterministic on recompute");
        Check(half > ids.size() / 5 && half < (3 * ids.size()) / 5,
              "random == 1 splits ~50/50 at mask >= 0.5");
        st.randomSeed = 8;
        UsdGenMaskResult const b = EvaluateMask(st, ids, {}, {}, 0.0);
        Check(!(a.curveMask == b.curveMask), "mask:randomSeed changes the selection");

        UsdGenMaskSettings inv = st;
        inv.invert = true;
        UsdGenMaskResult const ri = EvaluateMask(inv, ids, {}, {}, 0.0);
        // b (seed 8) is the current-seed baseline; a was computed at seed 7.
        Check(b.curveMask == ri.curveMask && HasDiag(ri, "invert"),
              "mask:invert is M1-neutral (unchanged mask + diagnostic)");
        UsdGenMaskSettings add = st;
        add.combine = TfToken("add");
        UsdGenMaskResult const ra = EvaluateMask(add, ids, {}, {}, 0.0);
        Check(b.curveMask == ra.curveMask && HasDiag(ra, "combine"),
              "mask:combine is M1-neutral (unchanged mask + diagnostic)");
    }

    // ---- 3: mask:random == 1 on grow: seed-sensitive terminal ------------------
    {
        auto buildRun = [&](uint32_t seed, UsdGenGraph &g) {
            UsdGenGraphDesc d = MakeG3();
            AddParam(d, "grow", TfToken("mask:random"), VtValue(1.0));
            AddParam(d, "grow", TfToken("mask:randomSeed"), VtValue(seed));
            UsdGenCompiler c;
            if (!c.Compile(d, &g).ok) return false;
            UsdGenEvalContext ctx;
            ctx.desc = &g.Desc();
            scheduler.Run(g, ctx, seed);
            return true;
        };
        UsdGenGraph g7, g8, g7b;
        Check(buildRun(7, g7) && buildRun(8, g8), "grow-mask graphs compile+run");
        if (!g_failures) {
            Check(TerminalDiffers(g7, g8), "mask:random selects different curves per seed");
            Check(buildRun(7, g7b) && TerminalIdentical(g7, g7b),
                  "mask:random selection deterministic for a fixed seed (bit-identical)");
        }
    }

    // ---- 4: mask:amount == 0 on scatter -> zero live roots ---------------------
    {
        UsdGenGraphDesc d = MakeG3();
        AddParam(d, "scatter", TfToken("mask:amount"), VtValue(0.0));
        UsdGenCompiler c;
        UsdGenGraph g;
        bool ran = false;
        size_t live = 0;
        if (c.Compile(d, &g).ok) {
            UsdGenEvalContext ctx;
            ctx.desc = &g.Desc();
            UsdGenRunResult res = scheduler.Run(g, ctx, 1);
            for (UsdGenTileView const &tv : res.tiles) live += tv.totalLiveCurves;
            ran = true;
        }
        Check(ran && live == 0,
              "mask:amount == 0 -> zero live curves end-to-end (plan/04 §5.2)");
    }

    std::printf(g_failures ? "testUsdGenAutoApply: FAILED (%d)\n"
                           : "testUsdGenAutoApply: PASS (SI-8 engine half)\n",
                g_failures);
    return g_failures ? 1 : 0;
}