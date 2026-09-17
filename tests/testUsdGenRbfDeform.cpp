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

Cooked Cook(UsdStageRefPtr const &stage, double time)
{
    Cooked out;
    usdGenImaging::UsdGenGraphDescBuildOptions options;
    options.time = time;
    UsdGenGraphDesc const desc = usdGenImaging::BuildGraphDescFromStage(stage, kDescription, options);
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
    out.input = graph.Node(graph.NodeIdForPath(kDeformInput)).buffer;
    out.output = graph.Node(graph.NodeIdForPath(kDeform)).buffer;
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
    for (auto const &e : unguided.errors) mentions |= e.find("usdGen:guides") != std::string::npos;
    Check(!unguided.ok && mentions, "without usdGen:guides the CPU lane reports why" + Errors(unguided));

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

// --- the example, through the groom scene index -----------------------------------

std::vector<GfVec3f> PublishedPoints(HdSceneIndexBaseRefPtr const &groom)
{
    std::vector<GfVec3f> points;
    SdfPath const render = kDescription.AppendChild(TfToken("__usdGenRender"));
    SdfPathVector tiles = groom->GetChildPrimPaths(render);
    std::sort(tiles.begin(), tiles.end());
    for (SdfPath const &tile : tiles) {
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
    std::vector<TfToken> const moving = {TfToken("points"), TfToken("furTauP"), TfToken("furTauN")};
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
        if (!tile) {
            if (entry.primPath.HasPrefix(render))
                unexpected += " " + entry.primPath.GetString() + ":" + Describe(locators);
            continue;
        }
        if (locators.Intersects(points)) ++tilesWithPoints;
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
    CheckExample();
    CheckScene();
    CheckPlaybackNotices();
    UsdGenGroomSceneIndex::DrainRetired();
    std::printf("testUsdGenRbfDeform: %s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? 1 : 0;
}
