// UsdGenDeform on the CPU lane: hair deformed by animated curves through a
// cubic RBF (plan/examples/rbf-deformation.md, examples/rbf-guides-plane.usda).
//
// rbf::CubicField, the math:
//   * the rest pose is the identity field;
//   * the field passes through every sample;
//   * any affine motion of the samples is reproduced everywhere;
//   * coplanar, coincident or too few samples are refused;
//   * SelectSamples drops duplicates and keeps a deterministic, spread subset.
// The example, cooked through the engine (stage builder):
//   * frame 1 (the drivers' rest pose) leaves the styled strands untouched;
//   * frame 20 bends them the way their nearest driver bends, roots locked;
//   * translating every driver translates every CV with lockRoots off;
//   * usdGen:mask 0 is a passthrough; a missing or planar driver set is a
//     diagnostic, not a silent identity.
// And through the groom scene index: moving the stage frame re-cooks the
// published strands. Stepping a frame in usdview's order (stage time, then
// the scene globals' current frame) cooks once and tells Hydra only what
// moved: no universal dirty, the tiles' points and occlusion but not their
// widths or colours, the drivers with the stage's own locators; the scene
// globals' frame alone cooks nothing (the example reads no $frame).
#include "usdGen/compiler.h"
#include "usdGen/graph.h"
#include "usdGen/opRegistry.h"
#include "usdGen/ops/rbfField.h"
#include "usdGen/ops/curveWrap.h"
#include "usdGen/scheduler.h"
#include "usdGenImaging/groomSceneIndexPlugin.h"
#include "usdGenImaging/testHook.h"
#include "usdGenImaging/usdGenGraphDescBuilderStage.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hdsi/sceneGlobalsSceneIndex.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/basisCurves.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace usdGen;

namespace {

int g_failures = 0;
void Check(bool ok, std::string const &what)
{
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what.c_str()); }
    else std::printf("ok: %s\n", what.c_str());
}

constexpr char const *kScene = USDGEN_TEST_SOURCE_DIR "/examples/rbf-guides-plane.usda";
SdfPath const kDescription("/World/Groom/Hair");
SdfPath const kDeform("/World/Groom/Hair/Ops/deform");
SdfPath const kDeformInput("/World/Groom/Hair/Ops/width");   // the operator before deform
SdfPath const kDrivers("/World/Drivers");

double Hash01(uint64_t k)
{
    k += 0x9e3779b97f4a7c15ULL;
    k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ULL;
    k = (k ^ (k >> 27)) * 0x94d049bb133111ebULL;
    return double((k ^ (k >> 31)) >> 11) / double(1ull << 53);
}

std::vector<GfVec3d> Cloud(size_t n, uint64_t seed)
{
    std::vector<GfVec3d> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = GfVec3d(Hash01(seed + i * 3), Hash01(seed + i * 3 + 1),
                         Hash01(seed + i * 3 + 2)) * 2.0 - GfVec3d(1.0);
    return out;
}

// --- the field -----------------------------------------------------------------

void CheckField()
{
    std::vector<GfVec3d> const rest = Cloud(40, 7);
    std::vector<GfVec3d> const queries = Cloud(200, 1001);
    rbf::CubicField field;
    std::string error;
    bool const bound = field.Bind(rest, &error);
    Check(bound, "a 3D sample cloud binds " + error);
    Check(field.Solve(rest, &error), "the rest pose solves");
    double identity = 0.0;
    for (GfVec3d const &q : queries) identity = std::max(identity, field.Displacement(q).GetLength());
    Check(identity < 1e-10, "the rest pose is the identity field (" + std::to_string(identity) + ")");

    // Arbitrary displacements: the field passes through every sample.
    std::vector<GfVec3d> moved(rest.size());
    for (size_t i = 0; i < rest.size(); ++i)
        moved[i] = rest[i] + GfVec3d(0.2 * std::sin(3.0 * rest[i][1]), 0.1 * rest[i][0] * rest[i][2],
                                     -0.15 * std::cos(2.0 * rest[i][0]));
    Check(field.Solve(moved, &error), "an arbitrary pose solves");
    double through = 0.0;
    for (size_t i = 0; i < rest.size(); ++i)
        through = std::max(through, (rest[i] + field.Displacement(rest[i]) - moved[i]).GetLength());
    Check(through < 1e-9, "the field passes through every sample (" + std::to_string(through) + ")");

    // Affine motion: rotation, non-uniform scale and translation.
    GfMatrix4d affine(1.0);
    affine.SetRotate(GfRotation(GfVec3d(0.3, 1.0, -0.2), 35.0));
    GfMatrix4d scale(1.0);
    scale.SetScale(GfVec3d(1.2, 0.9, 1.1));
    affine = scale * affine;
    affine.SetTranslateOnly(GfVec3d(0.4, -0.25, 0.1));
    for (size_t i = 0; i < rest.size(); ++i) moved[i] = affine.Transform(rest[i]);
    Check(field.Solve(moved, &error), "an affine pose solves");
    double reproduced = 0.0;
    for (GfVec3d const &q : queries)
        reproduced = std::max(reproduced, (q + field.Displacement(q) - affine.Transform(q)).GetLength());
    Check(reproduced < 1e-8, "affine motion of the samples is reproduced everywhere (" +
                                 std::to_string(reproduced) + ")");

    // Refusals.
    rbf::CubicField bad;
    std::vector<GfVec3d> planar = rest;
    for (GfVec3d &p : planar) p[1] = 0.3 * p[0] - 0.2 * p[2];   // a tilted plane
    bool refused = !bad.Bind(planar, &error) && error.find("span 3D") != std::string::npos;
    Check(refused, "coplanar samples are refused: " + error);
    refused = !bad.Bind({rest[0], rest[1], rest[2]}, &error) &&
        error.find("four") != std::string::npos;
    Check(refused, "three samples are refused: " + error);
    std::vector<GfVec3d> doubled = rest;
    doubled.push_back(rest[5]);
    refused = !bad.Bind(doubled, &error) && error.find("coincide") != std::string::npos;
    Check(refused, "coincident samples are refused: " + error);
    Check(!bad.Bound() && !bad.Solve(rest, &error), "a refused field does not solve");

    // Sample selection.
    std::vector<GfVec3d> points = Cloud(100, 42);
    points.push_back(points[3]);
    std::vector<size_t> const all = rbf::SelectSamples(points, 1000, 1e-9);
    Check(all.size() == 100 && std::find(all.begin(), all.end(), size_t(100)) == all.end(),
          "an exact duplicate is dropped");
    std::vector<size_t> const some = rbf::SelectSamples(points, 12, 1e-9);
    Check(some.size() == 12 && some == rbf::SelectSamples(points, 12, 1e-9) &&
          std::find(some.begin(), some.end(), size_t(0)) != some.end(),
          "a budget keeps a deterministic subset starting from the first point");
    double spread = 1e30;
    for (size_t a = 0; a < some.size(); ++a)
        for (size_t b = a + 1; b < some.size(); ++b)
            spread = std::min(spread, (points[some[a]] - points[some[b]]).GetLength());
    Check(spread > 0.4, "farthest-point samples are spread out (" + std::to_string(spread) + ")");
}

// --- the example, through the engine ---------------------------------------------

struct Cooked {
    bool ok = false;
    std::vector<std::string> errors;
    UsdGenCurveBuffer input, output;   // the deform node's input and output
    uint32_t cvs = 0;
};

Cooked Cook(UsdStageRefPtr const &stage, double time,
            SdfPath const &description = kDescription,
            SdfPath const &deform = kDeform,
            SdfPath const &input = kDeformInput)
{
    Cooked out;
    usdGenImaging::UsdGenGraphDescBuildOptions options;
    options.time = time;
    UsdGenGraphDesc const desc = usdGenImaging::BuildGraphDescFromStage(stage, description, options);
    out.errors = desc.validationErrors;
    UsdGenCompiler compiler;
    UsdGenGraph graph;
    UsdGenCompileResult const compiled = compiler.Compile(desc, &graph);
    if (!compiled.ok) {
        out.errors.insert(out.errors.end(), compiled.errors.begin(), compiled.errors.end());
        return out;
    }
    UsdGenScheduler scheduler(4);
    UsdGenEvalContext context;
    context.time = time;
    UsdGenRunResult const run = scheduler.Run(graph, context, 1);
    out.errors.insert(out.errors.end(), run.diagnostics.errors.begin(), run.diagnostics.errors.end());
    if (run.diagnostics.HasErrors()) return out;
    out.input = graph.Node(graph.NodeIdForPath(input)).buffer;
    out.output = graph.Node(graph.NodeIdForPath(deform)).buffer;
    out.ok = out.output.totalCurves > 0 && out.output.totalCvs == out.input.totalCvs &&
        out.output.totalCvs % out.output.totalCurves == 0;
    out.cvs = out.ok ? out.output.totalCvs / out.output.totalCurves : 0;
    return out;
}

GfVec3f P(UsdGenCurveBuffer const &b, size_t cv) { return GfVec3f(b.px[cv], b.py[cv], b.pz[cv]); }

double MaxShift(Cooked const &c, bool rootsOnly)
{
    double shift = 0.0;
    for (size_t cv = 0; cv < c.output.totalCvs; ++cv) {
        if (rootsOnly && cv % c.cvs != 0) continue;
        shift = std::max(shift, double((P(c.output, cv) - P(c.input, cv)).GetLength()));
    }
    return shift;
}

std::string Errors(Cooked const &c)
{
    std::string text;
    for (auto const &e : c.errors) text += "\n  " + e;
    return text;
}

void CheckExample()
{
    UsdStageRefPtr const stage = UsdStage::Open(kScene);
    if (!stage) { Check(false, std::string("cannot open ") + kScene); return; }

    Cooked const rest = Cook(stage, 1.0);
    Check(rest.ok, "the example cooks at frame 1" + Errors(rest));
    if (!rest.ok) return;
    std::printf("  %u strands, %u CVs each\n", rest.output.totalCurves, rest.cvs);
    Check(MaxShift(rest, false) < 1e-5, "frame 1 (the drivers' rest pose) leaves the strands untouched");

    Cooked const posed = Cook(stage, 20.0);
    Check(posed.ok, "the example cooks at frame 20" + Errors(posed));
    if (!posed.ok) return;
    Check(MaxShift(posed, true) < 1e-5, "usdGen:lockRoots keeps every root in place");
    double tips = 0.0;
    for (size_t c = 0; c < posed.output.totalCurves; ++c) {
        size_t const tip = (c + 1) * posed.cvs - 1;
        tips += (P(posed.output, tip) - P(posed.input, tip)).GetLength();
    }
    tips /= double(posed.output.totalCurves);
    std::printf("  mean tip displacement at frame 20: %.3f\n", tips);
    Check(tips > 0.05, "frame 20 bends the strands");

    // Each strand bends the way the driver rooted nearest it bends.
    UsdGeomBasisCurves drivers(stage->GetPrimAtPath(kDrivers));
    VtIntArray counts;
    VtVec3fArray restPoints, posePoints;
    drivers.GetCurveVertexCountsAttr().Get(&counts);
    drivers.GetPointsAttr().Get(&restPoints, UsdTimeCode::Default());
    drivers.GetPointsAttr().Get(&posePoints, UsdTimeCode(20.0));
    std::vector<GfVec3f> roots, tipMotion;
    size_t first = 0;
    for (int n : counts) {
        roots.push_back(restPoints[first]);
        tipMotion.push_back(posePoints[first + n - 1] - restPoints[first + n - 1]);
        first += size_t(n);
    }
    size_t agree = 0, considered = 0;
    for (size_t c = 0; c < posed.output.totalCurves; ++c) {
        GfVec3f const root = P(posed.input, c * posed.cvs);
        size_t nearest = 0;
        for (size_t g = 1; g < roots.size(); ++g)
            if ((roots[g] - root).GetLengthSq() < (roots[nearest] - root).GetLengthSq()) nearest = g;
        if ((roots[nearest] - root).GetLength() > 0.15 || tipMotion[nearest].GetLength() < 0.05) continue;
        size_t const tip = (c + 1) * posed.cvs - 1;
        GfVec3f const motion = P(posed.output, tip) - P(posed.input, tip);
        ++considered;
        if (GfDot(motion, tipMotion[nearest]) > 0.0f) ++agree;
    }
    std::printf("  %zu of %zu strands near a moving driver follow it\n", agree, considered);
    Check(considered > 50 && agree == considered, "strands near a driver bend the way it bends");

    // A rigid translation of every driver moves every CV by it (lockRoots off).
    VtVec3fArray shifted = restPoints;
    GfVec3f const offset(0.1f, 0.05f, -0.03f);
    for (GfVec3f &p : shifted) p += offset;
    drivers.GetPointsAttr().Set(shifted, UsdTimeCode(100.0));
    UsdAttribute lock = stage->GetAttributeAtPath(kDeform.AppendProperty(TfToken("usdGen:lockRoots")));
    lock.Set(false);
    Cooked const moved = Cook(stage, 100.0);
    Check(moved.ok, "the translated pose cooks" + Errors(moved));
    double error = 0.0;
    for (size_t cv = 0; moved.ok && cv < moved.output.totalCvs; ++cv)
        error = std::max(error, double((P(moved.output, cv) - P(moved.input, cv) - offset).GetLength()));
    Check(moved.ok && error < 2e-4, "translating the drivers translates the whole groom (" +
                                        std::to_string(error) + ")");
    lock.Set(true);

    // The envelope.
    UsdAttribute mask = stage->GetAttributeAtPath(kDeform.AppendProperty(TfToken("usdGen:mask")));
    Check(mask && mask.Set(0.0f), "author usdGen:mask = 0");
    Cooked const muted = Cook(stage, 20.0);
    Check(muted.ok && MaxShift(muted, false) < 1e-6, "mask 0 leaves the strands untouched");
    mask.Set(1.0f);

    // Refusals are diagnostics, not a silent identity.
    UsdRelationship guides = stage->GetPrimAtPath(kDeform).GetRelationship(TfToken("usdGen:guides"));
    guides.ClearTargets(true);
    Cooked const unguided = Cook(stage, 20.0);
    bool mentions = false;
    for (auto const &e : unguided.errors)
        mentions |= e.find("span 3D") != std::string::npos || e.find("rest") != std::string::npos;
    Check(!unguided.ok && mentions, "without guides the planar/rest-less surface is refused" + Errors(unguided));

    UsdGeomBasisCurves flat = UsdGeomBasisCurves::Define(stage, SdfPath("/World/FlatDrivers"));
    flat.GetPrim().AddAppliedSchema(TfToken("UsdGenCurveAPI"));
    flat.GetCurveVertexCountsAttr().Set(VtIntArray{2, 2, 2});
    flat.GetPointsAttr().Set(VtVec3fArray{GfVec3f(-1, 0, -1), GfVec3f(1, 0, -1), GfVec3f(-1, 0, 1),
                                          GfVec3f(1, 0, 1), GfVec3f(0, 0, 0), GfVec3f(0.5f, 0, 0.2f)});
    guides.SetTargets({SdfPath("/World/FlatDrivers")});
    Cooked const planar = Cook(stage, 20.0);
    mentions = false;
    for (auto const &e : planar.errors) mentions |= e.find("span 3D") != std::string::npos;
    Check(!planar.ok && mentions, "drivers in one plane are refused" + Errors(planar));
    guides.SetTargets({kDrivers});
}

void CheckSurfaceExample(bool outside = false)
{
    std::string const scene = std::string(USDGEN_TEST_SOURCE_DIR) + "/examples/motion/" +
        (outside ? "felt_sphere_groom_outside_xform.usda" : "felt_sphere_animated.usda");
    auto stage = UsdStage::Open(scene);
    Check(bool(stage), "animated sphere example opens");
    if (!stage) return;
    SdfPath const description(outside ? "/World/Groom/Hair" : "/World/Motion/Groom/Hair");
    SdfPath const deform = description.AppendPath(SdfPath("Ops/surfaceAnimate"));
    SdfPath const input = description.AppendPath(SdfPath("Ops/width"));
    auto cook = [&](double time) { return Cook(stage, time, description, deform, input); };
    Cooked const rest = cook(1);
    Check(rest.ok && (outside || MaxShift(rest, false) < 1e-6), "surface RBF cooks the rest groom" + Errors(rest));
    if (!rest.ok) return;
    for (int frame : {13, 26, 38, 51, 75, 100}) {
        Cooked const pose = cook(frame);
        bool stable = pose.ok && pose.input.totalCvs == rest.input.totalCvs;
        double error = 0;
        double const stretch = std::exp(.48 * std::sin(4 * 3.141592653589793 * (frame-1)/99.0));
        double const wide = 1 / std::sqrt(stretch);
        for (size_t cv = 0; stable && cv < pose.input.totalCvs; ++cv) {
            stable = P(pose.input,cv) == P(rest.input,cv);
            GfVec3f const p = P(rest.input,cv);
            GfVec3f expected(float(p[0]*wide),float(p[1]*wide),float(p[2]*stretch));
            if (outside) expected += GfVec3f(float(.55*std::sin(2*3.141592653589793*(frame-1)/99.0)),0,.5f);
            error = std::max(error,double((P(pose.output,cv)-expected).GetLength()));
        }
        Check(stable, "Scatter/Grow/style remain rest-local at frame " + std::to_string(frame) + Errors(pose));
        Check(pose.ok && error < 2e-6, "surface RBF follows squash/stretch without double parent motion at frame " +
              std::to_string(frame) + " error=" + std::to_string(error));
    }
    // Opening at a posed frame must still use Default-time rest, not first pull.
    auto posedFirst = UsdStage::Open(scene);
    Cooked const first = Cook(posedFirst,38,description,deform,input);
    bool same = first.ok && first.input.totalCvs == rest.input.totalCvs;
    for (size_t cv=0; same && cv<first.input.totalCvs; ++cv)
        same = P(first.input,cv) == P(rest.input,cv);
    Check(same,"opening on frame 38 generates from Default, not the animated input");
    // A time-sampled surface with no default must fail, not bind to first pull.
    auto prim=stage->GetPrimAtPath(SdfPath("/World/Motion/Sphere"));
    prim.GetAttribute(TfToken("points")).ClearAtTime(UsdTimeCode::Default());
    prim.RemoveProperty(TfToken("primvars:rest"));
    Cooked const missing = cook(38);
    Check(!missing.ok,"surface RBF refuses missing Default-time rest data");
}

void CheckCurveWrapField()
{
    curveWrap::Field field;
    std::vector<GfVec3d> rest={{0,0,0},{0,0,.25},{0,0,.5},{0,0,.75},{0,0,1}};
    std::string error;
    Check(field.Bind(rest,rest,&error),"straight centerline binds without a 3D RBF cage");
    auto queries=Cloud(100,456);
    double identity=0;
    for (auto const &q:queries) identity=std::max(identity,(field.Map(q)-q).GetLength());
    Check(identity<1e-12,"curveWrap rest pose is identity, including endpoint offsets");
    auto pose=rest;
    GfRotation rotation(GfVec3d(0,1,0),65);
    GfVec3d shift(.2,.1,-.3);
    for (auto &p:pose) p=rotation.TransformDir(p)+shift;
    Check(field.Bind(rest,pose,&error),"single centerline rotation and translation bind");
    double rigid=0;
    for (auto const &q:queries) rigid=std::max(rigid,(field.Map(q)-rotation.TransformDir(q)-shift).GetLength());
    Check(rigid<1e-10,"curveWrap preserves the full cross-section under rigid bending");
    for (size_t i=0;i<pose.size();++i) {
        double const a=double(i)*.3;
        pose[i]=GfVec3d(1-std::cos(a),0,std::sin(a));
    }
    Check(field.Bind(rest,pose,&error),"a planar curved pose binds");
    double radiusError=0;
    for (size_t i=0;i+1<rest.size();++i) for (int j=0;j<12;++j) {
        double const a=j*2*3.141592653589793/12;
        auto const center=(rest[i]+rest[i+1])*.5;
        auto const q=center+GfVec3d(.05*std::cos(a),.05*std::sin(a),0);
        radiusError=std::max(radiusError,std::abs((field.Map(q)-(pose[i]+pose[i+1])*.5).GetLength()-.05));
    }
    Check(radiusError<1e-10,"curved centerline keeps braid cross-section radius");
    auto bad=rest;bad[1]=bad[0];
    Check(!field.Bind(rest,bad,&error),"collapsed driver segment is diagnosed");
    bad=rest;bad.pop_back();
    Check(!field.Bind(rest,bad,&error),"changing driver topology is diagnosed");
}

void CheckBraidExamples()
{
    auto guide = UsdStage::Open(USDGEN_TEST_SOURCE_DIR "/examples/motion/braid_animated_guides.usda");
    auto both = UsdStage::Open(USDGEN_TEST_SOURCE_DIR "/examples/motion/braid_animated_guides_and_surface.usda");
    Check(bool(guide) && bool(both), "both guide-grown braid examples open");
    if (!guide || !both) return;
    SdfPath const description("/World/Motion/Groom/Hair");
    auto path = [&](char const *name) { return description.AppendPath(SdfPath(std::string("Ops/")+name)); };
    auto const rest = Cook(guide,1,description,path("guideAnimate"),path("width"));
    Check(rest.ok && rest.output.totalCurves == 381 && rest.cvs == 64,
          "fixed guide roots grow 381 strands, 64 CVs each" + Errors(rest));
    if (!rest.ok) return;
    Check(MaxShift(rest,false)<1e-6,"guide rest pose preserves the grown braid");
    for (int frame : {13,26,38,51,75,100}) {
        auto const a = Cook(guide,frame,description,path("guideAnimate"),path("width"));
        auto const b = Cook(both,frame,description,path("surfaceAnimate"),path("width"));
        bool stable = a.ok && b.ok && a.input.totalCvs==rest.input.totalCvs &&
            b.output.totalCvs==a.output.totalCvs;
        double error=0;
        double const stretch=std::exp(.48*std::sin(4*3.141592653589793*(frame-1)/99.0));
        double const wide=1/std::sqrt(stretch);
        for (size_t cv=0; stable && cv<a.input.totalCvs; ++cv) {
            stable = P(a.input,cv)==P(rest.input,cv) && P(b.input,cv)==P(rest.input,cv);
            auto const p=P(a.output,cv);
            GfVec3f const expected(float(p[0]*wide),float(p[1]*wide),float(p[2]*stretch));
            error=std::max(error,double((P(b.output,cv)-expected).GetLength()));
        }
        Check(stable,"braid growth stays at Default at frame "+std::to_string(frame)+Errors(a)+Errors(b));
        Check(a.ok && MaxShift(a,true)<1e-6,"guide bend locks braid roots");
        Check(stable && error<3e-6,"guide and surface deformation compose once ("+std::to_string(error)+")");
        if (frame==26) Check(MaxShift(a,false)>.15,"animated guides visibly bend the braid");
    }
}

void CheckSingleCenterExample()
{
    auto stage=UsdStage::Open(USDGEN_TEST_SOURCE_DIR "/examples/motion/braid_simulated_center_curve.usda");
    Check(bool(stage),"single simulated center example opens");
    if (!stage) return;
    SdfPath const desc("/World/Motion/Groom/Hair");
    auto const deform=desc.AppendPath(SdfPath("Ops/guideAnimate"));
    auto const input=desc.AppendPath(SdfPath("Ops/width"));
    auto rest=Cook(stage,1,desc,deform,input);
    Check(rest.ok && rest.output.totalCurves==381 && MaxShift(rest,false)<1e-6,
          "one straight center drives 381 rest-grown braid strands"+Errors(rest));
    if (!rest.ok) return;
    for (int frame:{18,37,65,100,1}) {
        auto pose=Cook(stage,frame,desc,deform,input);
        bool stable=pose.ok && pose.input.totalCvs==rest.input.totalCvs;
        for (size_t cv=0;stable && cv<pose.input.totalCvs;++cv)
            stable=P(pose.input,cv)==P(rest.input,cv);
        Check(stable,"simulated-center playback keeps rest growth fixed at "+std::to_string(frame)+Errors(pose));
        Check(pose.ok && MaxShift(pose,true)<1e-6,"pinned center preserves attached braid roots");
        if (frame==37) Check(MaxShift(pose,false)>.02,"simulation visibly moves the braid");
    }
    auto center=stage->GetPrimAtPath(SdfPath("/World/Motion/SimulatedCenter"));
    VtIntArray counts;
    center.GetAttribute(TfToken("curveVertexCounts")).Get(&counts);
    Check(counts==VtIntArray{31},"exactly one 31-CV simulation driver is authored");
    center.GetAttribute(TfToken("curveVertexCounts")).Set(VtIntArray{15,16});
    auto invalid=Cook(stage,37,desc,deform,input);
    Check(!invalid.ok,"curveWrap refuses multiple driver curves");
}

void CheckRegionExamples()
{
    for (bool dense:{false,true}) {
        std::string const scene=std::string(USDGEN_TEST_SOURCE_DIR)+"/examples/motion/"+
            (dense?"two_braids_ptex_regions.usda":"two_curves_ptex_regions.usda");
        auto stage=UsdStage::Open(scene);
        Check(bool(stage),"Ptex multi-center example opens");if (!stage) continue;
        SdfPath const desc("/World/Motion/Groom/Hair");
        auto const deform=desc.AppendPath(SdfPath("Ops/regionAnimate"));
        auto const input=desc.AppendPath(SdfPath("Ops/width"));
        auto cook=[&](int frame) {return Cook(stage,frame,desc,deform,input);};
        auto rest=cook(1);
        Check(rest.ok && rest.output.totalCurves==(dense?762u:2u),
              "one description grows the expected number of region strands"+Errors(rest));
        if (!rest.ok) continue;
        auto driver=UsdGeomBasisCurves(stage->GetPrimAtPath(SdfPath("/World/Motion/Drivers")));
        VtVec3fArray driverRest;VtIntArray counts;
        driver.GetPointsAttr().Get(&driverRest,UsdTimeCode::Default());
        driver.GetCurveVertexCountsAttr().Get(&counts);
        Check(counts==VtIntArray({31,31}),"two center curves drive a single description");
        auto verify=[&](int frame,bool swapped) {
            auto pose=cook(frame);
            VtVec3fArray driverNow;driver.GetPointsAttr().Get(&driverNow,UsdTimeCode(frame));
            curveWrap::Field fields[2];std::string error;
            for (int g=0;g<2;++g) {
                std::vector<GfVec3d> r,n;
                for (int i=g*31;i<(g+1)*31;++i) {r.emplace_back(driverRest[i]);n.emplace_back(driverNow[i]);}
                Check(fields[g].Bind(r,n,&error),"region driver field binds");
            }
            bool stable=pose.ok && pose.input.totalCvs==rest.input.totalCvs;double maxError=0;
            for (size_t c=0;stable && c<pose.input.totalCurves;++c) {
                int g=P(rest.input,c*rest.cvs)[0]<0 ? 0:1;if (swapped) g=1-g;
                for (size_t cv=c*rest.cvs;stable && cv<(c+1)*rest.cvs;++cv) {
                    stable=P(pose.input,cv)==P(rest.input,cv);
                    maxError=std::max(maxError,(GfVec3d(P(pose.output,cv))-fields[g].Map(GfVec3d(P(rest.input,cv)))).GetLength());
                }
            }
            Check(stable && maxError<2e-6,(swapped?"swapping Ptex texels rebinds across space":"Ptex regions follow only their assigned center")+
                  std::string(" error=")+std::to_string(maxError)+Errors(pose));
        };
        verify(37,false);verify(100,false);
        // Moving just driver 1 cannot affect region 0, even though they share a
        // description, operator, surface and one two-curve driver prim.
        VtVec3fArray changed=driverRest;
        for (size_t i=31;i<changed.size();++i) changed[i]+=GfVec3f(.1f,.07f,-.02f);
        driver.GetPointsAttr().Set(changed,UsdTimeCode(55));verify(55,false);
        auto map=stage->GetPrimAtPath(desc.AppendPath(SdfPath("Maps/Regions")));
        map.GetAttribute(TfToken("usdGen:map:file")).Set(SdfAssetPath(
            USDGEN_TEST_SOURCE_DIR "/examples/motion/maps/two_regions_swapped.ptx"));
        verify(37,true);
        // Reject missing driver IDs instead of silently choosing a nearby driver.
        auto regionIds=stage->GetPrimAtPath(deform).GetAttribute(TfToken("usdGen:guideRegions"));
        regionIds.Set(VtIntArray{7,8});
        auto missing=cook(37);
        Check(!missing.ok && Errors(missing).find("has no center curve")!=std::string::npos,
              "unmapped Ptex IDs are diagnosed"+Errors(missing));
        regionIds.Set(VtIntArray{0,1});
        map.GetAttribute(TfToken("usdGen:map:filter")).Set(TfToken("bilinear"));
        auto filtered=cook(37);
        Check(!filtered.ok,"regionMap refuses filtered/blended category IDs");
    }
}

// --- the example, through the groom scene index -----------------------------------

std::vector<GfVec3f> PublishedPoints(HdSceneIndexBaseRefPtr const &groom,
                                  SdfPath const &description = kDescription)
{
    std::vector<GfVec3f> points;
    SdfPath const render = description.AppendChild(TfToken("__usdGenRender"));
    SdfPathVector tiles = groom->GetChildPrimPaths(render);
    std::sort(tiles.begin(), tiles.end());
    for (SdfPath const &tile : tiles) {
        // Tiles only. The render scope also carries the scalp-shadow cap,
        // whose points are a tessellation of the emitting surface and whose
        // count changes with the hair over it.
        if (tile.GetName().rfind("tile_", 0) != 0) continue;
        auto sampled = HdSampledDataSource::Cast(HdContainerDataSource::Get(
            groom->GetPrim(tile).dataSource,
            HdDataSourceLocator(TfToken("primvars"), TfToken("points"), TfToken("primvarValue"))));
        if (!sampled) continue;
        VtValue const value = sampled->GetValue(0.0f);
        if (!value.IsHolding<VtVec3fArray>()) continue;
        for (GfVec3f const &p : value.UncheckedGet<VtVec3fArray>()) points.push_back(p);
    }
    return points;
}

void CheckOutsidePublication()
{
    auto stage=UsdStage::Open(USDGEN_TEST_SOURCE_DIR "/examples/motion/felt_sphere_groom_outside_xform.usda");
    if (!stage) { Check(false,"outside groom stage opens for Hydra"); return; }
    SdfPath const description("/World/Groom/Hair");
    // An independently transformed groom must cancel its own world transform
    // during deformation, then receive it once from Hydra publication.
    auto container=stage->GetPrimAtPath(SdfPath("/World/Groom"));
    container.SetTypeName(TfToken("Xform"));
    GfMatrix4d transform(1.0);
    transform.SetRotate(GfRotation(GfVec3d(0,1,0),23));
    transform.SetTranslateOnly(GfVec3d(.12,.05,-.1));
    container.CreateAttribute(TfToken("xformOp:transform"),SdfValueTypeNames->Matrix4d).Set(transform);
    container.CreateAttribute(TfToken("xformOpOrder"),SdfValueTypeNames->TokenArray).Set(VtTokenArray{TfToken("xformOp:transform")});
    UsdImagingCreateSceneIndicesInfo info; info.stage=stage;
    auto indices=UsdImagingCreateSceneIndices(info);
    auto groom=UsdGenGroomSceneIndex::New(indices.finalSceneIndex);
    auto *owner=dynamic_cast<UsdGenGroomSceneIndex *>(groom.operator->());
    for (int frame : {38,1,26,100}) {
        indices.stageSceneIndex->SetTime(UsdTimeCode(frame));
        indices.stageSceneIndex->ApplyPendingUpdates(); owner->Synchronize();
        auto actual=PublishedPoints(groom,description);
        auto rest=Cook(stage,frame,description,description.AppendPath(SdfPath("Ops/surfaceAnimate")),
                       description.AppendPath(SdfPath("Ops/width")));
        double error=0;
        double const stretch=std::exp(.48*std::sin(4*3.141592653589793*(frame-1)/99.0));
        double const wide=1/std::sqrt(stretch);
        GfVec3d offset(.55*std::sin(2*3.141592653589793*(frame-1)/99.0),0,.5);
        bool valid=rest.ok && actual.size()==rest.input.totalCvs;
        for (size_t cv=0; valid && cv<actual.size(); ++cv) {
            auto p=P(rest.input,cv);
            GfVec3d expected(p[0]*wide,p[1]*wide,p[2]*stretch);
            error=std::max(error,(transform.Transform(GfVec3d(actual[cv]))-expected-offset).GetLength());
        }
        Check(valid && error<3e-6,"separately transformed groom publishes correct world points at frame "+
              std::to_string(frame)+" error="+std::to_string(error));
    }
}

void CheckScene()
{
    UsdStageRefPtr const stage = UsdStage::Open(kScene, UsdStage::LoadAll);
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    UsdImagingSceneIndices indices = UsdImagingCreateSceneIndices(info);
    indices.stageSceneIndex->SetTime(UsdTimeCode(1.0));
    HdSceneIndexBaseRefPtr groom = UsdGenGroomSceneIndex::New(indices.finalSceneIndex);
    auto *owner = dynamic_cast<UsdGenGroomSceneIndex *>(groom.operator->());
    Check(owner != nullptr, "the example opens in a groom scene index");
    if (!owner) return;
    owner->Synchronize();
    std::vector<GfVec3f> const atRest = PublishedPoints(groom);
    Check(!atRest.empty(), "frame 1 publishes strands through Hydra");

    indices.stageSceneIndex->SetTime(UsdTimeCode(20.0));
    indices.stageSceneIndex->ApplyPendingUpdates();
    owner->Synchronize();
    std::vector<GfVec3f> const posed = PublishedPoints(groom);
    double moved = 0.0;
    for (size_t i = 0; i < posed.size() && i < atRest.size(); ++i)
        moved = std::max(moved, double((posed[i] - atRest[i]).GetLength()));
    Check(posed.size() == atRest.size() && moved > 0.05,
          "changing the frame re-cooks the strands with the drivers (" + std::to_string(moved) + ")");

    indices.stageSceneIndex->SetTime(UsdTimeCode(1.0));
    indices.stageSceneIndex->ApplyPendingUpdates();
    owner->Synchronize();
    std::vector<GfVec3f> const back = PublishedPoints(groom);
    double drift = 0.0;
    for (size_t i = 0; i < back.size() && i < atRest.size(); ++i)
        drift = std::max(drift, double((back[i] - atRest[i]).GetLength()));
    Check(back.size() == atRest.size() && drift < 1e-6, "scrubbing back to frame 1 restores the rest groom");
}

// --- playback notices ----------------------------------------------------------

class DirtyLog : public HdSceneIndexObserver
{
public:
    void PrimsAdded(HdSceneIndexBase const &, AddedPrimEntries const &entries) override
    {
        added += entries.size();
    }
    void PrimsRemoved(HdSceneIndexBase const &, RemovedPrimEntries const &entries) override
    {
        removed += entries.size();
    }
    void PrimsDirtied(HdSceneIndexBase const &, DirtiedPrimEntries const &entries) override
    {
        dirtied.insert(dirtied.end(), entries.begin(), entries.end());
    }
    void PrimsRenamed(HdSceneIndexBase const &, RenamedPrimEntries const &) override {}
    void Clear()
    {
        added = removed = 0;
        dirtied.clear();
    }

    size_t added = 0, removed = 0;
    DirtiedPrimEntries dirtied;
};

bool Universal(HdDataSourceLocatorSet const &locators)
{
    for (HdDataSourceLocator const &locator : locators)
        if (locator.IsEmpty()) return true;
    return false;
}

std::string Describe(HdDataSourceLocatorSet const &locators)
{
    std::string text;
    for (HdDataSourceLocator const &locator : locators)
        text += " " + (locator.IsEmpty() ? std::string("<universal>") : locator.GetString());
    return text;
}

void CheckPlaybackNotices()
{
    UsdStageRefPtr const stage = UsdStage::Open(kScene, UsdStage::LoadAll);
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    UsdImagingSceneIndices indices = UsdImagingCreateSceneIndices(info);
    HdsiSceneGlobalsSceneIndexRefPtr const globals =
        HdsiSceneGlobalsSceneIndex::New(indices.finalSceneIndex);
    indices.stageSceneIndex->SetTime(UsdTimeCode(10.0));
    globals->SetCurrentFrame(10.0);
    HdSceneIndexBaseRefPtr groom = UsdGenGroomSceneIndex::New(globals);
    auto *owner = dynamic_cast<UsdGenGroomSceneIndex *>(groom.operator->());
    if (!owner) { Check(false, "the example opens under the scene globals"); return; }
    owner->Synchronize();

    SdfPath const render = kDescription.AppendChild(TfToken("__usdGenRender"));
    std::vector<SdfPath> tiles;
    for (SdfPath const &child : groom->GetChildPrimPaths(render))
        if (groom->GetPrim(child).primType == TfToken("basisCurves")) tiles.push_back(child);
    Check(!tiles.empty(), "frame 10 publishes tiles");

    DirtyLog log;
    groom->AddObserver(HdSceneIndexObserverPtr(&log));

    // One frame step, the way usdview takes it.
    uint64_t const cooks = UsdGenImagingTestHook::groomCookCount(*groom);
    indices.stageSceneIndex->SetTime(UsdTimeCode(11.0));
    globals->SetCurrentFrame(11.0);
    indices.stageSceneIndex->ApplyPendingUpdates();
    owner->Synchronize();
    uint64_t const stepCooks = UsdGenImagingTestHook::groomCookCount(*groom) - cooks;
    Check(stepCooks == 1, "a frame step cooks once (" + std::to_string(stepCooks) + ")");
    Check(log.added == 0 && log.removed == 0, "a frame step adds and removes nothing");

    TfToken const primvars("primvars");
    HdDataSourceLocator const points(primvars, TfToken("points"));
    // `normals` is the scalp cap's: it follows the emitting surface, which the
    // RBF drivers deform, so it moves whenever the cap's points do.
    std::vector<TfToken> const moving = {TfToken("points"), TfToken("furTauP"),
                                         TfToken("furTauN"), TfToken("normals")};
    std::string universal, unexpected;
    size_t tilesWithPoints = 0;
    bool driverPoints = false, driverUniversal = false;
    for (HdSceneIndexObserver::DirtiedPrimEntry const &entry : log.dirtied) {
        HdDataSourceLocatorSet const &locators = entry.dirtyLocators;
        if (Universal(locators)) universal += " " + entry.primPath.GetString();
        if (entry.primPath == kDrivers) {
            driverPoints |= locators.Intersects(points);
            driverUniversal |= Universal(locators);
            continue;
        }
        bool const tile = std::find(tiles.begin(), tiles.end(), entry.primPath) != tiles.end();
        // The scalp-shadow cap is rebaked with the groom, so it moves every
        // frame too. It is held to the same discipline as a tile: precise
        // locators over the primvars that actually change, never universal.
        bool const cap = entry.primPath.GetName() == "scalpShadow";
        if (!tile && !cap) {
            if (entry.primPath.HasPrefix(render))
                unexpected += " " + entry.primPath.GetString() + ":" + Describe(locators);
            continue;
        }
        if (tile && locators.Intersects(points)) ++tilesWithPoints;
        for (HdDataSourceLocator const &locator : locators) {
            if (!locator.HasPrefix(HdDataSourceLocator(primvars))) continue;
            bool const allowed = locator.GetElementCount() >= 2 &&
                std::find(moving.begin(), moving.end(), locator.GetElement(1)) != moving.end();
            if (!allowed) unexpected += " " + entry.primPath.GetName() + ":" + locator.GetString();
        }
    }
    Check(universal.empty(), "a frame step sends no universal dirty (" + universal + " )");
    Check(tilesWithPoints == tiles.size(), "every tile dirties its points (" +
                                               std::to_string(tilesWithPoints) + " of " +
                                               std::to_string(tiles.size()) + ")");
    Check(unexpected.empty(),
          "tiles dirty only points and occlusion; the scope and material stay clean (" +
              unexpected + " )");
    Check(driverPoints && !driverUniversal,
          "the drivers' dirty reaches Hydra with the stage's locators");

    // The scene globals' frame alone.
    log.Clear();
    uint64_t const before = UsdGenImagingTestHook::groomCookCount(*groom);
    globals->SetCurrentFrame(12.0);
    owner->Synchronize();
    uint64_t const globalsCooks = UsdGenImagingTestHook::groomCookCount(*groom) - before;
    bool tileDirtied = false, rootOnlyGlobals = true;
    for (HdSceneIndexObserver::DirtiedPrimEntry const &entry : log.dirtied) {
        tileDirtied |= std::find(tiles.begin(), tiles.end(), entry.primPath) != tiles.end();
        if (entry.primPath.IsAbsoluteRootPath())
            for (HdDataSourceLocator const &locator : entry.dirtyLocators)
                rootOnlyGlobals &= locator.HasPrefix(HdDataSourceLocator(TfToken("sceneGlobals")));
    }
    Check(globalsCooks == 0 && !tileDirtied,
          "the scene globals' frame alone cooks nothing (" + std::to_string(globalsCooks) + ")");
    Check(rootOnlyGlobals, "the root dirty keeps the scene globals' locator");

    groom->RemoveObserver(HdSceneIndexObserverPtr(&log));
}

} // namespace

int main()
{
    usdGenRegisterM1Operators();
    CheckField();
    CheckCurveWrapField();
    CheckExample();
    CheckSurfaceExample();
    CheckSurfaceExample(true);
    CheckBraidExamples();
    CheckSingleCenterExample();
    CheckRegionExamples();
    CheckOutsidePublication();
    CheckScene();
    CheckPlaybackNotices();
    UsdGenGroomSceneIndex::DrainRetired();
    std::printf("testUsdGenRbfDeform: %s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? 1 : 0;
}
