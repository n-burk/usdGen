// UsdGenWind operator contract and CPU cooks.
//
// Covers the time-dependent force field along the normalized direction:
//   (1) registry HasKernel/Create/TopologyEffect/ReferenceInputs/
//       OutputPrimvars plus the Topology/Value parameter partition;
//   (2) deterministic CPU cooks over small hand-built topologies (fixed
//       seeds): identity at zero effect, the constant-deflection formula,
//       root locking, direction normalization, stiffness resistance and its
//       ramp, gust determinism per time+seed, gust animation across time,
//       time-independence at gustStrength 0, and a connected-expression cook
//       for the per-strand constStrength;
//   (3) every edge fails closed (zero/non-finite direction, non-finite
//       strengths/stiffness/mask inputs, mistyped or non-finite
//       stiffness:knots, unknown stiffness:interpolation);
//   (4) CUDA-backend explicit rejection: Wind is CPU-only (no capability
//       matrix row, like Clump), so ValidateCudaGraph refuses a Wind graph.
#include "usdGen/compiler.h"
#include "usdGen/cudaExecution.h"
#include "usdGen/expressions/context.h"
#include "usdGen/graph.h"
#include "usdGen/opRegistry.h"
#include "usdGen/scheduler.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace usdGen;

static int failures = 0;
static void Check(bool v, char const *s)
{
    if (!v) { ++failures; std::printf("FAIL: %s\n", s); }
    else std::printf("ok: %s\n", s);
}
static expr::ValueShape FloatShape(uint32_t n = 1)
{
    return {expr::ScalarType::Float32, 1, n, 1, 1, false};
}

constexpr float kTol = 1e-4f;
static bool Near(float a, float b, float tol = kTol)
{
    return std::fabs(a - b) <= tol;
}

// Straight strands along +Y from the given roots. Mirrors the CurveSource
// fixture (surface-free, rebind never): valid skin bindings plus identity
// root frames, so rootT=(1,0,0), rootB=(0,1,0), rootN=(0,0,1).
static UsdGenCurveSetDesc StraightStrands(SdfPath const &path,
                                          std::vector<GfVec3f> const &roots,
                                          int cvs, float step)
{
    UsdGenCurveSetDesc set;
    set.path = path;
    set.role = UsdGenRole::Curves;
    set.curveRole = TfToken("hair");
    size_t const n = roots.size();
    set.curveVertexCounts.assign(n, cvs);
    set.points.resize(n * size_t(cvs));
    for (size_t c = 0; c < n; ++c)
        for (int i = 0; i < cvs; ++i)
            set.points[c * size_t(cvs) + size_t(i)] =
                roots[c] + GfVec3f(0.0f, float(i) * step, 0.0f);
    set.rest = set.points;
    set.widths.assign(n * size_t(cvs), 0.1f);
    set.curveId.resize(n);
    for (size_t c = 0; c < n; ++c) set.curveId[c] = uint64_t(c);
    set.skinPrim.resize(n);
    for (size_t c = 0; c < n; ++c) set.skinPrim[c] = int(c);
    set.skinPrimUv.assign(n, GfVec2f(0.2f, 0.3f));
    set.rootFrame.assign(n, GfMatrix4d(1.0));
    return set;
}

static UsdGenNodeDesc SourceNode(SdfPath const &path, SdfPath const &curves)
{
    UsdGenNodeDesc node;
    node.path = path;
    node.type = TfToken("UsdGenCurveSource");
    node.curves = {curves};
    node.params = {{TfToken("rebind"), VtValue(TfToken("never")), false},
                   {TfToken("useRest"), VtValue(true), false}};
    return node;
}

static UsdGenNodeDesc OpNode(char const *path, char const *type,
                             SdfPath const &input, int seed)
{
    UsdGenNodeDesc node;
    node.path = SdfPath(path);
    node.type = TfToken(type);
    node.seed = seed;
    node.inputs = {input};
    return node;
}

static void AddBinding(UsdGenGraphDesc &desc, SdfPath const &nodePath,
                       SdfPath const &exprPath, std::string const &source,
                       TfToken const &nativeType, expr::ValueShape const &shape,
                       TfToken const &destination, expr::Domain domain,
                       VtValue const &literal)
{
    desc.expressions.push_back(
        {exprPath, source, {{TfToken("result"), nativeType, shape}}});
    for (UsdGenNodeDesc &node : desc.nodes) {
        if (node.path != nodePath) continue;
        UsdGenExpressionBinding binding;
        binding.expression = exprPath;
        binding.output = TfToken("result");
        binding.nativeType = nativeType;
        binding.destination = destination;
        binding.destinationShape = shape;
        binding.domain = domain;
        binding.literal = literal;
        node.expressionBindings.push_back(binding);
    }
}

static bool CookAt(UsdGenGraphDesc const &desc, double time, UsdGenCurveBuffer *out)
{
    UsdGenCompiler compiler;
    UsdGenGraph graph;
    UsdGenCompileResult compiled = compiler.Compile(desc, &graph);
    if (!compiled.ok) {
        for (auto const &e : compiled.errors)
            std::printf("  compile: %s\n", e.c_str());
        return false;
    }
    UsdGenScheduler scheduler(2);
    UsdGenEvalContext ctx;
    ctx.time = time;
    ctx.desc = &graph.Desc();
    UsdGenRunResult run = scheduler.Run(graph, ctx, 1);
    if (run.diagnostics.HasErrors()) {
        for (auto const &e : run.diagnostics.errors)
            std::printf("  run: %s\n", e.c_str());
        return false;
    }
    *out = graph.Output();
    return true;
}

static bool Cook(UsdGenGraphDesc const &desc, UsdGenCurveBuffer *out)
{
    return CookAt(desc, 0.0, out);
}

static bool CookSourceOnly(UsdGenGraphDesc const &desc, SdfPath const &source,
                           UsdGenCurveBuffer *out)
{
    UsdGenGraphDesc plain = desc;
    plain.nodes.pop_back();
    plain.expressions.clear();
    plain.terminal = source;
    return Cook(plain, out);
}

static bool PointsEqual(UsdGenCurveBuffer const &a, UsdGenCurveBuffer const &b)
{
    return a.totalCurves == b.totalCurves && a.totalCvs == b.totalCvs &&
        a.px == b.px && a.py == b.py && a.pz == b.pz;
}

// --- (1) registry contract ---------------------------------------------

static void CheckRegistryContracts()
{
    UsdGenOpRegistry &registry = UsdGenOpRegistry::Get();
    TfToken const type("UsdGenWind");
    Check(registry.HasKernel(type), "HasKernel(UsdGenWind)");
    std::unique_ptr<UsdGenOp> op = registry.Create(type);
    Check(op != nullptr, "Create(UsdGenWind)");
    if (!op) return;
    Check(op->Type() == type, "Wind reports its own type");
    Check(op->TopologyEffect() == UsdGenTopoFx::None,
          "Wind has TopologyEffect None");
    Check(op->ReferenceInputs().empty(),
          "Wind declares no reference inputs");
    Check(op->OutputPrimvars().empty(), "Wind emits no output primvars");
    Check(op->PlanesTouched() == UsdGenOp::kPlanePoints,
          "Wind touches the points plane only");
    std::set<std::string> topo, value;
    for (TfToken const &t : op->TopologyParameters()) topo.insert(t.GetString());
    for (TfToken const &t : op->ValueParameters()) value.insert(t.GetString());
    Check(topo == std::set<std::string>{"input", "surface", "seed"},
          "Wind topology parameters are input/surface/seed");
    Check(value == std::set<std::string>{"enabled", "mask", "direction",
                                         "constStrength", "gustStrength",
                                         "stiffness", "stiffness:knots",
                                         "stiffness:interpolation"},
          "Wind value parameters are enabled/mask/direction/const/gust/stiffness/knots/interp");
}

// --- (2) CPU cooks -----------------------------------------------------
// With direction (0,0,1), gust 0, stiffness 0 and mask 1 the field is
// d = constStrength * hairT along +Z; roots (t = 0) never move.

static UsdGenGraphDesc WindDesc(std::vector<UsdGenParamValue> const &params)
{
    UsdGenGraphDesc desc;
    desc.description = SdfPath("/wind");
    desc.defaultWidth = 0.01f;
    std::vector<GfVec3f> const roots = {GfVec3f(0, 0, 0), GfVec3f(3, 0, 0)};
    desc.curveSets.push_back(
        StraightStrands(SdfPath("/wind/hair"), roots, 6, 0.25f));
    desc.nodes.push_back(
        SourceNode(SdfPath("/wind/source"), SdfPath("/wind/hair")));
    UsdGenNodeDesc wind = OpNode("/wind/op", "UsdGenWind",
                                 SdfPath("/wind/source"), 7);
    wind.params = params;
    desc.nodes.push_back(wind);
    desc.terminal = SdfPath("/wind/op");
    return desc;
}

static void CheckIdentityCook()
{
    UsdGenGraphDesc desc = WindDesc({{TfToken("constStrength"), VtValue(0.0f), false},
                                     {TfToken("gustStrength"), VtValue(0.0f), false}});
    UsdGenCurveBuffer out, plain;
    Check(Cook(desc, &out), "Wind cooks on the CPU lane");
    Check(CookSourceOnly(desc, SdfPath("/wind/source"), &plain),
          "Wind fixture source cooks on its own");
    if (out.totalCvs == 0 || plain.totalCvs == 0) return;
    Check(out.totalCurves == 2 && out.totalCvs == 12,
          "Wind preserves the strand topology");
    Check(PointsEqual(out, plain),
          "Wind with zero strengths passes points through bit-for-bit");

    // Full stiffness with no knots (the empty-knots identity ramp, exactly 1.0)
    // resists everything.
    UsdGenGraphDesc stiff = WindDesc({{TfToken("constStrength"), VtValue(2.0f), false},
                                      {TfToken("stiffness"), VtValue(1.0f), false}});
    UsdGenCurveBuffer held;
    Check(Cook(stiff, &held), "Wind with full stiffness cooks");
    if (held.totalCvs == 0) return;
    Check(PointsEqual(held, plain),
          "Wind with stiffness 1 holds every CV bit-for-bit");

    UsdGenGraphDesc masked = WindDesc({{TfToken("constStrength"), VtValue(2.0f), false},
                                       {TfToken("mask"), VtValue(0.0f), false}});
    UsdGenCurveBuffer muted;
    Check(Cook(masked, &muted), "Wind with mask 0 cooks");
    if (muted.totalCvs == 0) return;
    Check(PointsEqual(muted, plain),
          "Wind with mask 0 passes points through bit-for-bit");
}

static void CheckConstantCook()
{
    // d = 2 * hairT along (0,0,1); the source strands lie in z = 0.
    UsdGenGraphDesc desc = WindDesc({{TfToken("direction"), VtValue(GfVec3f(0, 0, 1)), false},
                                     {TfToken("constStrength"), VtValue(2.0f), false},
                                     {TfToken("gustStrength"), VtValue(0.0f), false}});
    UsdGenCurveBuffer out, plain;
    Check(Cook(desc, &out), "Wind constant deflection cooks on the CPU lane");
    Check(CookSourceOnly(desc, SdfPath("/wind/source"), &plain),
          "Wind constant-cook source cooks on its own");
    if (out.totalCvs == 0 || plain.totalCvs == 0) return;
    bool hairTLinear = plain.hairT.size() == 12;
    for (size_t o = 0; o < 12 && hairTLinear; ++o)
        hairTLinear = Near(plain.hairT[o], float(o % 6) / 5.0f, 1e-6f);
    Check(hairTLinear, "Wind fixture hairT runs 0..1 linearly per strand");
    bool profile = true, sidesExact = true, rootsExact = true;
    for (size_t c = 0; c < 2; ++c) {
        float const rootX = c == 0 ? 0.0f : 3.0f;
        for (int i = 0; i < 6; ++i) {
            size_t const o = c * 6 + size_t(i);
            float const t = plain.hairT[o];
            profile = profile && Near(out.pz[o], plain.pz[o] + 2.0f * t, 1e-5f);
            sidesExact = sidesExact && out.px[o] == plain.px[o] &&
                out.py[o] == plain.py[o];
        }
        rootsExact = rootsExact && out.px[c * 6] == plain.px[c * 6] &&
            out.py[c * 6] == plain.py[c * 6] && out.pz[c * 6] == plain.pz[c * 6];
    }
    Check(profile, "Wind deflects every CV by constStrength * hairT");
    Check(sidesExact, "Wind moves nothing off the direction axis");
    Check(rootsExact, "Wind locks roots bit-for-bit");

    // The direction normalizes: (0,0,5) cooks exactly like (0,0,1).
    UsdGenGraphDesc scaled = WindDesc({{TfToken("direction"), VtValue(GfVec3f(0, 0, 5)), false},
                                       {TfToken("constStrength"), VtValue(2.0f), false},
                                       {TfToken("gustStrength"), VtValue(0.0f), false}});
    UsdGenCurveBuffer rescaled;
    Check(Cook(scaled, &rescaled), "Wind with a scaled direction cooks");
    if (rescaled.totalCvs == 0) return;
    Check(PointsEqual(out, rescaled),
          "Wind normalizes its direction before deflecting");

    // gustStrength 0 is pure constant deflection: the cook is time-independent.
    UsdGenCurveBuffer late;
    Check(CookAt(desc, 240.0, &late), "Wind constant cook runs at a late time");
    if (late.totalCvs == 0) return;
    Check(PointsEqual(out, late),
          "Wind with gustStrength 0 is identical at every time");
}

static void CheckStiffnessRampCook()
{
    // knots [(0,0),(1,1)] linear with stiffness 1: flex = 1 - t, so
    // d = 2 * t * (1 - t): the tip holds and the middle bends most.
    UsdGenGraphDesc desc = WindDesc({{TfToken("direction"), VtValue(GfVec3f(0, 0, 1)), false},
                                     {TfToken("constStrength"), VtValue(2.0f), false},
                                     {TfToken("gustStrength"), VtValue(0.0f), false},
                                     {TfToken("stiffness"), VtValue(1.0f), false},
                                     {TfToken("stiffness:interpolation"),
                                      VtValue(TfToken("linear")), false}});
    {
        VtVec2fArray knots(2);
        knots[0] = GfVec2f(0.0f, 0.0f);
        knots[1] = GfVec2f(1.0f, 1.0f);
        desc.nodes.back().params.push_back(
            {TfToken("stiffness:knots"), VtValue(knots), false});
    }
    UsdGenCurveBuffer out, plain;
    Check(Cook(desc, &out), "Wind stiffness-ramp cook runs");
    Check(CookSourceOnly(desc, SdfPath("/wind/source"), &plain),
          "Wind ramp-cook source cooks on its own");
    if (out.totalCvs == 0 || plain.totalCvs == 0) return;
    bool profile = true, tipExact = true;
    for (size_t c = 0; c < 2; ++c) {
        for (int i = 0; i < 6; ++i) {
            size_t const o = c * 6 + size_t(i);
            float const t = plain.hairT[o];
            profile = profile && Near(out.pz[o], plain.pz[o] + 2.0f * t * (1.0f - t), 1e-5f);
        }
        size_t const tip = c * 6 + 5;
        tipExact = tipExact && out.px[tip] == plain.px[tip] &&
            out.py[tip] == plain.py[tip] &&
            Near(out.pz[tip], plain.pz[tip], 1e-6f);
    }
    Check(profile, "Wind shapes deflection by 1 - stiffness * ramp(t)");
    Check(tipExact, "Wind holds the tip at full tip stiffness");
}

static void CheckGustDeterminismAndAnimation()
{
    UsdGenGraphDesc desc = WindDesc({{TfToken("direction"), VtValue(GfVec3f(0, 0, 1)), false},
                                     {TfToken("constStrength"), VtValue(0.0f), false},
                                     {TfToken("gustStrength"), VtValue(2.0f), false}});
    UsdGenCurveBuffer early, again, late, plain;
    Check(CookAt(desc, 0.0, &early), "Wind gust cook runs at time 0");
    Check(CookAt(desc, 0.0, &again), "Wind gust cook reruns at time 0");
    Check(CookAt(desc, 240.0, &late), "Wind gust cook runs at a late time");
    Check(CookSourceOnly(desc, SdfPath("/wind/source"), &plain),
          "Wind gust-cook source cooks on its own");
    if (early.totalCvs == 0 || again.totalCvs == 0 || late.totalCvs == 0 ||
        plain.totalCvs == 0) return;
    Check(PointsEqual(early, again),
          "Wind gusts are bit-for-bit deterministic per time+seed");
    bool moved = false;
    for (size_t o = 0; o < 12; ++o)
        moved = moved || early.pz[o] != late.pz[o];
    Check(moved, "Wind gusts animate across time");
    bool rootsExact = true;
    for (size_t c = 0; c < 2; ++c)
        rootsExact = rootsExact && early.pz[c * 6] == plain.pz[c * 6] &&
            late.pz[c * 6] == plain.pz[c * 6];
    Check(rootsExact, "Wind gusts never move roots");
}

static void CheckConstExpressionCook()
{
    // Per-strand constStrength 1 and 2 with no gust: strand tips rest at
    // inPz + 1 and inPz + 2 along the direction.
    UsdGenGraphDesc desc = WindDesc({{TfToken("direction"), VtValue(GfVec3f(0, 0, 1)), false},
                                     {TfToken("gustStrength"), VtValue(0.0f), false}});
    AddBinding(desc, SdfPath("/wind/op"),
               SdfPath("/wind/Expressions/const"), "$primIndex + 1.0",
               TfToken("float"), FloatShape(), TfToken("constStrength"),
               expr::Domain::Primitive, VtValue(0.0f));
    UsdGenCurveBuffer out, plain;
    Check(Cook(desc, &out), "Wind with a constStrength expression cooks on the CPU");
    Check(CookSourceOnly(desc, SdfPath("/wind/source"), &plain),
          "Wind expression-cook source cooks on its own");
    if (out.totalCvs == 0 || plain.totalCvs == 0) return;
    bool profile = true;
    for (size_t c = 0; c < 2; ++c) {
        float const expected = float(c) + 1.0f;
        for (int i = 0; i < 6; ++i) {
            size_t const o = c * 6 + size_t(i);
            profile = profile &&
                Near(out.pz[o], plain.pz[o] + expected * plain.hairT[o], 1e-5f);
        }
    }
    Check(profile, "Wind samples constStrength per strand root");
}

// --- (3) fail-closed edges ------------------------------------------------

static void CheckEdgesFailClosed()
{
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    UsdGenCurveBuffer out;
    Check(!Cook(WindDesc({{TfToken("direction"), VtValue(GfVec3f(0, 0, 0)), false}}), &out),
          "Wind with a zero direction fails closed");
    Check(!Cook(WindDesc({{TfToken("direction"), VtValue(GfVec3f(nan, 0, 1)), false}}), &out),
          "Wind with a non-finite direction fails closed");
    Check(!Cook(WindDesc({{TfToken("constStrength"), VtValue(inf), false}}), &out),
          "Wind with a non-finite constStrength fails closed");
    Check(!Cook(WindDesc({{TfToken("gustStrength"), VtValue(nan), false}}), &out),
          "Wind with a non-finite gustStrength fails closed");
    Check(!Cook(WindDesc({{TfToken("stiffness"), VtValue(nan), false}}), &out),
          "Wind with a non-finite stiffness fails closed");
    Check(!Cook(WindDesc({{TfToken("stiffness:knots"),
                           VtValue(VtFloatArray{0.0f, 1.0f}), false}}), &out),
          "Wind with mistyped stiffness:knots fails closed");
    {
        VtVec2fArray knots(2);
        knots[0] = GfVec2f(0.0f, 1.0f);
        knots[1] = GfVec2f(1.0f, nan);
        Check(!Cook(WindDesc({{TfToken("stiffness:knots"), VtValue(knots), false}}), &out),
              "Wind with non-finite stiffness:knots fails closed");
    }
    Check(!Cook(WindDesc({{TfToken("stiffness:interpolation"),
                           VtValue(TfToken("bogus")), false}}), &out),
          "Wind with an unknown stiffness:interpolation fails closed");
}

// --- (4) CUDA-backend explicit rejection ----------------------------------

static void CheckCudaRejectsWind()
{
    UsdGenGraphDesc desc = WindDesc({{TfToken("constStrength"), VtValue(2.0f), false}});
    desc.executionBackend = UsdGenExecutionBackend::Cuda;
    Check(GetCudaExecutionCapabilityMatrix().Find(TfToken("UsdGenWind")) == nullptr,
          "Wind has no CUDA capability row (CPU-only, like Clump)");
    UsdGenDiagnostics cudaDiag;
    bool const admitted = ValidateCudaGraph(desc, &cudaDiag);
    bool namesWind = false, backendUnbuilt = false;
    for (auto const &e : cudaDiag.errors) {
        if (e.find("UsdGenWind") != std::string::npos) namesWind = true;
        if (e.find("not built") != std::string::npos) backendUnbuilt = true;
    }
    Check(!admitted && cudaDiag.HasErrors(),
          "CUDA planner rejects a Wind graph");
    // CUDA builds name the missing operator; CPU-only builds report the
    // backend itself as unbuilt. Either way the graph never runs on CUDA.
    Check(namesWind || backendUnbuilt,
          "CUDA rejection names UsdGenWind (or the unbuilt backend)");
}

int main()
{
    usdGenRegisterM1Operators();
    CheckRegistryContracts();
    CheckIdentityCook();
    CheckConstantCook();
    CheckStiffnessRampCook();
    CheckGustDeterminismAndAnimation();
    CheckConstExpressionCook();
    CheckEdgesFailClosed();
    CheckCudaRejectsWind();
    std::printf("testUsdGenWind: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
