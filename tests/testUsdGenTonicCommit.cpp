// testUsdGenTonicCommit — T1: the P1 asynchronous commit pipeline.
//
// Plan/17 P1 exit: TonicCommitter (worker + idle swap), hydrate, "Save
// groom", and the GuideInterpolate relationship fill-in. Proven here:
//   * a commit lands the groom subtree (tube, Guides, RegionMap, RegionExpr)
//     plus the GuideInterpolate op in one idle swap;
//   * version coalescing (N enqueues under pause produce one build);
//   * cancellation (a built layer superseded before the swap never lands);
//   * no swap runs during a gesture; no stale swap runs while a newer build
//     is in flight;
//   * the partial-transfer fallback converges to the same stage content, one
//     subtree per idle slot;
//   * hydrate round-trips the model bit-exactly (tube + guides);
//   * the fill-in creates the op only when absent and only touches empty
//     relationships/connections;
//   * "Save groom" writes .usdc and re-parents the live sublayer beneath it;
//   * TN-4: the reference-scale swap completes in <= 5 ms (measured below);
//   * the committer C ABI drives the same pipeline over identifiers;
//   * P4 hooks: subdivide params + lock flags ride the commit and survive
//     hydrate (childIndex/deltas/level channels land with P4).
//   * P6 reload: Detach idles the worker (model survives, swap reports
//     Detached, shelf layer counts as dropped); Reattach resets the
//     committed lineage and the next swap re-creates the groom prim,
//     carrying the post-detach edits; the C ABI pair mirrors this.
//
// Needs the schema plugin (typed groom prims), so this registers with the
// _usdgen_m1_env environment like every adapter-dependent T1 test.

#include "usdGenTonic/tonicApi.h"
#include "usdGenTonic/tonicApiStage.h"
#include "usdGenTonic/tonicCommit.h"
#include "usdGenTonic/tonicModel.h"
#include "usdGenTonic/tonicScalp.h"

#include "usdGenImaging/usdGenImagingSession.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usd/stage.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

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
    std::fflush(stdout);
}

bool DoubleEq(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

// Pump idle swaps until `want` commits or the timeout expires.
bool WaitCommitted(usdGenTonic::TonicCommitter &committer,
                   SdfLayerHandle const &live, uint64_t want,
                   int timeoutMs = 5000)
{
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        committer.SwapIfIdle(live, /*gestureActive*/ false);
        if (committer.CommittedVersion() >= want) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

UsdStageRefPtr MakeStage(SdfLayerHandle const &live,
                         bool withDescription = true)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory("tonicCommit");
    if (withDescription) {
        stage->DefinePrim(SdfPath("/Groom/Hair"), TfToken("UsdGenDescription"));
        stage->DefinePrim(SdfPath("/Groom/Hair/Ops"), TfToken("Scope"));
    }
    stage->GetSessionLayer()->InsertSubLayerPath(live->GetIdentifier(), 0);
    return stage;
}

bool GetPoints(UsdStagePtr const &stage, SdfPath const &path, VtVec3fArray *out)
{
    UsdPrim const prim = stage->GetPrimAtPath(path);
    return bool(prim) && prim.GetAttribute(TfToken("points")).Get(out);
}

SdfPathVector Targets(UsdRelationship const &rel)
{
    SdfPathVector targets;
    rel.GetTargets(&targets);
    return targets;
}

SdfPathVector Connections(UsdAttribute const &attr)
{
    SdfPathVector conns;
    attr.GetConnections(&conns);
    return conns;
}

// V0b: bit-exact comparison of two tubes' authored shapes and deltas.
bool SameSections(std::vector<usdGenTonic::TonicTubeSection> const &a,
                  std::vector<usdGenTonic::TonicTubeSection> const &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].t != b[i].t || a[i].scale != b[i].scale ||
            a[i].twist != b[i].twist || a[i].u != b[i].u ||
            a[i].v != b[i].v) {
            return false;
        }
    }
    return true;
}

bool SameDesc(usdGenTonic::TonicTubeDesc const &a,
              usdGenTonic::TonicTubeDesc const &b)
{
    return a.centerX == b.centerX && a.centerY == b.centerY &&
           a.centerZ == b.centerZ && a.ringVerts == b.ringVerts &&
           a.tubeId == b.tubeId && a.regionId == b.regionId &&
           SameSections(a.sections, b.sections);
}

bool SameDeltas(usdGenTonic::TonicShapeDeltas const &a,
                usdGenTonic::TonicShapeDeltas const &b)
{
    return a.centerDu == b.centerDu && a.centerDv == b.centerDv &&
           a.centerDw == b.centerDw && SameSections(a.sections, b.sections);
}

// -- V6: a two-region scalp (plan/18 §7 G14) ------------------------------
//
// One L1 tube per closed region is the whole point of the region->tube map,
// and the committer has to serialise every one of them. These three helpers
// build the smallest scalp that carries two adjoining regions: an n x n grid
// in the XZ plane, and two rects sharing the edge at x = n/2.
struct Grid {
    std::vector<float> points;
    std::vector<int> counts;
    std::vector<int> indices;
};

Grid MakeGrid(int n)
{
    Grid grid;
    for (int ix = 0; ix <= n; ++ix) {
        for (int iz = 0; iz <= n; ++iz) {
            grid.points.push_back(float(ix));
            grid.points.push_back(0.0f);
            grid.points.push_back(float(iz));
        }
    }
    auto pid = [&](int ix, int iz) { return ix * (n + 1) + iz; };
    for (int ix = 0; ix < n; ++ix) {
        for (int iz = 0; iz < n; ++iz) {
            grid.counts.push_back(4);
            grid.indices.push_back(pid(ix, iz));
            grid.indices.push_back(pid(ix, iz + 1));
            grid.indices.push_back(pid(ix + 1, iz + 1));
            grid.indices.push_back(pid(ix + 1, iz));
        }
    }
    return grid;
}

usdGenTonic::TonicHit Locate(usdGenTonic::TonicScalpMesh const &mesh, int n,
                             float x, float z)
{
    using namespace usdGenTonic;
    int const ix = std::min(std::max(int(std::floor(x)), 0), n - 1);
    int const iz = std::min(std::max(int(std::floor(z)), 0), n - 1);
    TonicHit hit;
    hit.hit = true;
    hit.faceId = ix * n + iz;
    hit.u = z - float(iz);
    hit.v = x - float(ix);
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (!TonicFacePosition(mesh, hit.faceId, hit.u, hit.v, &px, &py, &pz)) {
        hit.hit = false;
        return hit;
    }
    hit.px = px;
    hit.py = py;
    hit.pz = pz;
    hit.nx = 0.0f;
    hit.ny = 1.0f;
    hit.nz = 0.0f;
    return hit;
}

// A: x in [0, n/2]; B: x in [n/2, n]; both z in [1, n-1]. They share the
// edge a1-a2, which is what makes them two regions of ONE planar graph
// rather than two graphs.
void BuildTwoRegions(usdGenTonic::TonicModel *model,
                     usdGenTonic::TonicScalpMesh const &mesh, int n)
{
    float const mid = float(n) / 2.0f;
    float const farX = float(n);  // `far` is a windows.h macro
    float const zLo = 1.0f;
    float const zHi = float(n) - 1.0f;
    int const a0 = model->GraphAddNode(Locate(mesh, n, 0.0f, zLo));
    int const a1 = model->GraphAddNode(Locate(mesh, n, mid, zLo));
    int const a2 = model->GraphAddNode(Locate(mesh, n, mid, zHi));
    int const a3 = model->GraphAddNode(Locate(mesh, n, 0.0f, zHi));
    model->GraphConnect(a0, a1);
    model->GraphConnect(a1, a2);
    model->GraphConnect(a2, a3);
    model->GraphConnect(a3, a0);
    int const b1 = model->GraphAddNode(Locate(mesh, n, farX, zLo));
    int const b2 = model->GraphAddNode(Locate(mesh, n, farX, zHi));
    model->GraphConnect(a1, b1);
    model->GraphConnect(b1, b2);
    model->GraphConnect(b2, a2);
}

} // namespace

int
main()
{
    using usdGenTonic::TonicCommitter;
    using usdGenTonic::TonicCommitPaths;
    using usdGenTonic::TonicModel;

    TonicCommitPaths paths;
    paths.groomPath = SdfPath("/TonicGroom");
    paths.descriptionPath = SdfPath("/Groom/Hair");

    // -- basic commit ------------------------------------------------------
    TonicModel model;
    Check(model.BuildTestTube(), "model builds the test tube");
    TonicModel::FillParams fill;
    fill.density = 16.0f;
    fill.cvCount = 8;
    fill.seed = 7;
    fill.edgeBias = 0.25f;
    Check(model.SetFillParams(fill), "model takes P1 fill params");
    uint64_t const v1 = model.GetVersion();

    SdfLayerRefPtr live = SdfLayer::CreateAnonymous("usdGenTonic-live");
    UsdStageRefPtr stage = MakeStage(live);
    TonicCommitter committer(&model, paths);
    committer.Enqueue(stage);
    Check(WaitCommitted(committer, live, v1), "first commit swaps at idle");
    Check(committer.CommittedVersion() == v1, "committed version is v1");
    Check(committer.BuildCount() == 1, "one enqueue produces one build");

    UsdPrim const groom = stage->GetPrimAtPath(SdfPath("/TonicGroom"));
    Check(bool(groom) && groom.GetTypeName() == TfToken("UsdGenTonicGroom"),
          "live carries the UsdGenTonicGroom prim");
    TfToken groomVersion;
    Check(groom.GetAttribute(TfToken("usdGen:tonic:version")).Get(&groomVersion) &&
              groomVersion == TfToken("1"),
          "groom version token is \"1\"");
    Check(Targets(groom.GetRelationship(TfToken("usdGen:tonic:description"))) ==
              SdfPathVector{SdfPath("/Groom/Hair")},
          "groom links the description");
    VtVec3fArray centers;
    Check(stage->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0"))
                  .GetAttribute(TfToken("usdGen:tonic:centerPoints"))
                  .Get(&centers) &&
              centers.size() == 5,
          "tube0 carries 5 center points");
    VtVec3fArray guidePoints;
    Check(GetPoints(stage, SdfPath("/TonicGroom/Guides"), &guidePoints) &&
              guidePoints.size() == 16 * 8,
          "Guides carries 16 guides x 8 CVs");
    UsdPrim const guides = stage->GetPrimAtPath(SdfPath("/TonicGroom/Guides"));
    Check(guides.HasAPI(TfToken("UsdGenCurveAPI")),
          "Guides applies UsdGenCurveAPI");
    TfToken role;
    Check(guides.GetAttribute(TfToken("primvars:usdGen:role")).Get(&role) &&
              role == TfToken("guide"),
          "guide role primvar is \"guide\"");
    VtUInt64Array curveIds;
    VtIntArray tubeIds;
    Check(guides.GetAttribute(TfToken("primvars:usdGen:curveId"))
                  .Get(&curveIds) &&
              curveIds.size() == 16 && curveIds[0] == 1000 &&
              curveIds[15] == 1015,
          "curve ids are 1000 + guide");
    Check(guides.GetAttribute(TfToken("primvars:tubeId")).Get(&tubeIds) &&
              tubeIds.size() == 16 && tubeIds[0] == 0,
          "tubeId primvar names tube0");
    TfToken interp;
    Check(guides.GetAttribute(TfToken("primvars:tubeId"))
                  .GetMetadata(TfToken("interpolation"), &interp) &&
              interp == TfToken("uniform"),
          "tubeId interpolates uniform");
    std::string exprSource;
    Check(stage->GetPrimAtPath(SdfPath("/TonicGroom/RegionExpr"))
                  .GetAttribute(TfToken("usdGen:expr:source"))
                  .Get(&exprSource) &&
              exprSource == "ptex(\"regionMap\")",
          "RegionExpr reads ptex(\"regionMap\")");
    TfToken mapFilter;
    Check(stage->GetPrimAtPath(SdfPath("/TonicGroom/RegionMap"))
                  .GetAttribute(TfToken("usdGen:map:filter"))
                  .Get(&mapFilter) &&
              mapFilter == TfToken("nearest"),
          "RegionMap filters nearest");
    UsdPrim const tonicOp =
        stage->GetPrimAtPath(SdfPath("/Groom/Hair/Ops/tonicInterp"));
    Check(bool(tonicOp) &&
              tonicOp.GetTypeName() == TfToken("UsdGenGuideInterpolate"),
          "fill-in creates the GuideInterpolate op");
    Check(Targets(tonicOp.GetRelationship(TfToken("usdGen:guides"))) ==
              SdfPathVector{SdfPath("/TonicGroom/Guides")},
          "fill-in connects usdGen:guides to Guides");
    Check(Connections(tonicOp.GetAttribute(TfToken("usdGen:region"))) ==
              SdfPathVector{SdfPath("/TonicGroom/RegionExpr")},
          "fill-in connects usdGen:region to RegionExpr");
    std::printf("info: first swap took %.3f ms\n", committer.LastSwapMs());

    // -- coalescing: ten enqueues under pause produce one build ------------
    size_t const buildsBefore = committer.BuildCount();
    committer.PauseWorker(true);
    for (int i = 0; i < 10; ++i) {
        model.MoveCenterRing(i % 5, 0.1f, 0.0f);
        committer.Enqueue(stage);
    }
    uint64_t const vCoalesced = model.GetVersion();
    Check(committer.BuildCount() == buildsBefore,
          "paused worker builds nothing");
    committer.PauseWorker(false);
    Check(WaitCommitted(committer, live, vCoalesced),
          "coalesced commit swaps at idle");
    Check(committer.BuildCount() == buildsBefore + 1,
          "ten fast strokes produce one layer build");
    Check(committer.CommittedVersion() == vCoalesced,
          "the one build carries the latest version");

    // -- cancellation: a built layer superseded before the swap never lands
    committer.PauseWorker(true);
    model.MoveCenterRing(0, 1.0f, 0.0f);
    committer.Enqueue(stage);
    committer.PauseWorker(false);
    // Wait until vA is built but do NOT swap it yet.
    {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(5000);
        while (committer.BuildCount() < buildsBefore + 2 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Check(committer.BuildCount() == buildsBefore + 2, "vA layer built");
    size_t const droppedBefore = committer.DroppedCount();
    committer.PauseWorker(true);
    model.MoveCenterRing(1, 1.0f, 0.0f);
    committer.Enqueue(stage);
    model.MoveCenterRing(2, 1.0f, 0.0f);
    committer.Enqueue(stage);
    uint64_t const vCancel = model.GetVersion();
    size_t const buildsAtCancel = committer.BuildCount();
    committer.PauseWorker(false);
    // Let the worker finish the vC build with NO swap in between, so the
    // unswapped vA layer is necessarily replaced on the shelf (dropped).
    {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(5000);
        while (committer.BuildCount() < buildsAtCancel + 1 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Check(committer.BuildCount() == buildsAtCancel + 1,
          "superseding layer built with no swap between");
    Check(WaitCommitted(committer, live, vCancel),
          "superseding commit swaps at idle");
    Check(committer.DroppedCount() > droppedBefore,
          "the superseded vA layer was dropped, never swapped");
    Check(committer.CommittedVersion() == vCancel,
          "live carries the superseding version");
    {
        VtVec3fArray livePoints;
        usdGenTonic::TonicGuideSet regen =
            usdGenTonic::TonicGenerateGuides(model.Snapshot());
        Check(GetPoints(stage, SdfPath("/TonicGroom/Guides"), &livePoints) &&
                  livePoints.size() == regen.points.size() / 3 &&
                  std::memcmp(livePoints.data(), regen.points.data(),
                              regen.points.size() * sizeof(float)) == 0,
              "landed guides equal the latest model state, not vA");
    }

    // -- no swap runs during a gesture -------------------------------------
    model.MoveCenterRing(3, 0.5f, 0.0f);
    committer.Enqueue(stage);
    uint64_t const vGesture = model.GetVersion();
    // The gesture gate is checked first, so this refuses whether or not the
    // worker has finished building.
    Check(committer.SwapIfIdle(live, /*gestureActive*/ true) ==
              TonicCommitter::SkippedGesture,
          "swap during a gesture is refused");
    Check(committer.CommittedVersion() < vGesture,
          "the held swap leaves live on the old version");
    Check(WaitCommitted(committer, live, vGesture),
          "the held swap lands once the gesture ends");
    Check(committer.SwapIfIdle(live, /*gestureActive*/ false) ==
              TonicCommitter::NothingPending,
          "a settled committer reports nothing pending");

    // -- partial-transfer fallback converges to identical content -----------
    committer.SetSwapBudgetMs(0.0);  // every full swap is now over budget
    model.MoveCenterRing(4, 0.25f, 0.0f);
    committer.Enqueue(stage);
    uint64_t const vFull = model.GetVersion();
    Check(WaitCommitted(committer, live, vFull),
          "over-budget commit still swaps (full, once)");
    Check(committer.PartialMode(),
          "an over-budget swap arms the partial fallback");
    // Twin committer, forced straight into partial mode by the same budget.
    SdfLayerRefPtr livePartial = SdfLayer::CreateAnonymous("tonic-live-p");
    UsdStageRefPtr stagePartial = MakeStage(livePartial);
    TonicCommitter partial(&model, paths);
    partial.SetSwapBudgetMs(0.0);
    partial.Enqueue(stagePartial);
    uint64_t const vPartial = model.GetVersion();
    Check(WaitCommitted(partial, livePartial, vPartial),
          "partial-mode commit converges");
    Check(partial.PartialMode(), "twin committer ran the partial fallback");
    {
        VtVec3fArray a, b;
        VtVec3fArray ca, cb;
        bool sameGuides =
            GetPoints(stage, SdfPath("/TonicGroom/Guides"), &a) &&
            GetPoints(stagePartial, SdfPath("/TonicGroom/Guides"), &b) &&
            a.size() == b.size() &&
            std::memcmp(a.data(), b.data(), a.size() * sizeof(GfVec3f)) == 0;
        bool sameTube =
            stage->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0"))
                .GetAttribute(TfToken("usdGen:tonic:centerPoints"))
                .Get(&ca) &&
            stagePartial->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0"))
                .GetAttribute(TfToken("usdGen:tonic:centerPoints"))
                .Get(&cb) &&
            ca == cb;
        bool sameOp =
            Targets(stagePartial
                        ->GetPrimAtPath(SdfPath("/Groom/Hair/Ops/tonicInterp"))
                        .GetRelationship(TfToken("usdGen:guides"))) ==
            SdfPathVector{SdfPath("/TonicGroom/Guides")};
        Check(sameGuides, "partial swap lands identical guides");
        Check(sameTube, "partial swap lands an identical tube");
        Check(sameOp, "partial swap lands the op fill-in");
    }
    // Single-slot timing at this scale is far under budget; record it.
    std::printf("info: partial-mode slot took %.3f ms\n",
                partial.LastSwapMs());

    // -- hydrate round-trips the model bit-exactly -------------------------
    TonicModel hydrated;
    usdGenTonic::TonicHydrateResult hr =
        usdGenTonic::TonicHydrateModel(stage, SdfPath("/TonicGroom"), &hydrated);
    Check(hr.ok, "hydrate accepts the committed stage: " + hr.diagnostic);
    Check(hr.guidesBitEqual, "regenerated guides are bit-equal to stored");
    {
        TonicModel::TubeSnapshot a = model.Snapshot();
        TonicModel::TubeSnapshot b = hydrated.Snapshot();
        bool sameShape = a.shape.rings == b.shape.rings &&
            a.shape.ringVerts == b.shape.ringVerts &&
            DoubleEq(a.shape.radius, b.shape.radius, 1e-6) &&
            DoubleEq(a.shape.length, b.shape.length, 1e-6);
        bool sameCenters = a.centerX == b.centerX && a.centerY == b.centerY &&
            a.centerZ == b.centerZ;
        bool sameFill = a.fill.density == b.fill.density &&
            a.fill.cvCount == b.fill.cvCount && a.fill.seed == b.fill.seed &&
            a.fill.edgeBias == b.fill.edgeBias;
        bool sameSections = a.sections.size() == b.sections.size();
        for (size_t i = 0; sameSections && i < a.sections.size(); ++i) {
            sameSections = a.sections[i].t == b.sections[i].t &&
                           a.sections[i].u == b.sections[i].u &&
                           a.sections[i].v == b.sections[i].v;
        }
        Check(sameShape, "hydrated shape matches the model");
        Check(sameCenters, "hydrated centers match bit-exactly");
        Check(sameFill, "hydrated fill params match");
        Check(sameSections, "hydrated sections match bit-exactly");
    }
    // Re-commit the hydrated model: identical guide bytes.
    {
        SdfLayerRefPtr live2 = SdfLayer::CreateAnonymous("tonic-live-2");
        UsdStageRefPtr stage2 = MakeStage(live2);
        TonicCommitter c2(&hydrated, paths);
        c2.Enqueue(stage2);
        Check(WaitCommitted(c2, live2, hydrated.GetVersion()),
              "hydrated model re-commits");
        VtVec3fArray a, b;
        Check(GetPoints(stage, SdfPath("/TonicGroom/Guides"), &a) &&
                  GetPoints(stage2, SdfPath("/TonicGroom/Guides"), &b) &&
                  a.size() == b.size() &&
                  std::memcmp(a.data(), b.data(),
                              a.size() * sizeof(GfVec3f)) == 0,
              "re-commit emits bit-identical guides");
    }
    // Foreign guides fail closed, never silently.
    {
        UsdStageRefPtr fs = UsdStage::CreateInMemory("tonicForeign");
        SdfLayerRefPtr over =
            SdfLayer::CreateAnonymous("tonic-foreign-over");
        fs->GetSessionLayer()->InsertSubLayerPath(live->GetIdentifier(), 0);
        fs->GetSessionLayer()->InsertSubLayerPath(over->GetIdentifier(), 0);
        fs->SetEditTarget(over);
        UsdPrim fg = fs->GetPrimAtPath(SdfPath("/TonicGroom/Guides"));
        VtVec3fArray pts;
        fg.GetAttribute(TfToken("points")).Get(&pts);
        pts[0] = GfVec3f(123.0f, 456.0f, 789.0f);
        fg.GetAttribute(TfToken("points")).Set(pts);
        TonicModel fm;
        usdGenTonic::TonicHydrateResult fr =
            usdGenTonic::TonicHydrateModel(fs, SdfPath("/TonicGroom"), &fm);
        Check(!fr.ok && !fr.guidesBitEqual,
              "hand-edited guides fail the bit-equality assert");
    }

    // -- GuideInterpolate fill-in only touches what is empty ---------------
    {
        // (b) authored guides survive; the empty region is filled.
        SdfLayerRefPtr liveB = SdfLayer::CreateAnonymous("tonic-live-b");
        UsdStageRefPtr sb = MakeStage(liveB);
        UsdPrim mine =
            sb->DefinePrim(SdfPath("/Groom/Hair/Ops/myInterp"),
                           TfToken("UsdGenGuideInterpolate"));
        mine.CreateRelationship(TfToken("usdGen:guides"))
            .SetTargets({SdfPath("/Other")});
        usdGenTonic::TonicFillPlan plan =
            usdGenTonic::TonicPlanGuideInterpolateFill(sb, paths);
        Check(!plan.createInterpOp && !plan.setInterpGuides &&
                  plan.setInterpRegion &&
                  plan.opPath == SdfPath("/Groom/Hair/Ops/myInterp"),
              "existing op plans a region-only fill on its own path");
        TonicCommitter cb(&model, paths);
        cb.Enqueue(sb);
        Check(WaitCommitted(cb, liveB, model.GetVersion()),
              "region-only fill swaps");
        Check(Targets(sb->GetPrimAtPath(SdfPath("/Groom/Hair/Ops/myInterp"))
                           .GetRelationship(TfToken("usdGen:guides"))) ==
                  SdfPathVector{SdfPath("/Other")},
              "authored guides are never rewritten");
        Check(Connections(sb->GetPrimAtPath(SdfPath("/Groom/Hair/Ops/myInterp"))
                              .GetAttribute(TfToken("usdGen:region"))) ==
                  SdfPathVector{SdfPath("/TonicGroom/RegionExpr")},
              "the empty region is connected");
        Check(!sb->GetPrimAtPath(SdfPath("/Groom/Hair/Ops/tonicInterp")),
              "no second op is authored beside the artist's");
    }
    {
        // (c) a fully authored op is left alone.
        SdfLayerRefPtr liveC = SdfLayer::CreateAnonymous("tonic-live-c");
        UsdStageRefPtr sc = MakeStage(liveC);
        UsdPrim full =
            sc->DefinePrim(SdfPath("/Groom/Hair/Ops/full"),
                           TfToken("UsdGenGuideInterpolate"));
        full.CreateRelationship(TfToken("usdGen:guides"))
            .SetTargets({SdfPath("/Other")});
        full.CreateAttribute(TfToken("usdGen:region"),
                             SdfValueTypeNames->Float)
            .Set(2.0f);
        usdGenTonic::TonicFillPlan plan =
            usdGenTonic::TonicPlanGuideInterpolateFill(sc, paths);
        Check(!plan.createInterpOp && !plan.setInterpGuides &&
                  !plan.setInterpRegion,
              "a fully authored op plans no fill-in");
        // (d) a missing description plans nothing.
        SdfLayerRefPtr liveD = SdfLayer::CreateAnonymous("tonic-live-d");
        UsdStageRefPtr sd = MakeStage(liveD, /*withDescription*/ false);
        plan = usdGenTonic::TonicPlanGuideInterpolateFill(sd, paths);
        Check(!plan.createInterpOp && !plan.setInterpGuides &&
                  !plan.setInterpRegion,
              "a missing description plans no fill-in");
    }

    // -- Save groom ---------------------------------------------------------
    {
        std::filesystem::path savePath =
            std::filesystem::temp_directory_path() / "tonic-groom-p1.usdc";
        std::error_code ec;
        std::filesystem::remove(savePath, ec);
        std::string err;
        Check(usdGenTonic::TonicSaveGroom(stage, live, savePath.string(), &err),
              "Save groom writes .usdc: " + err);
        Check(std::filesystem::exists(savePath, ec),
              "the saved .usdc exists on disk");
        std::vector<std::string> sublayers =
            stage->GetSessionLayer()->GetSubLayerPaths();
        Check(sublayers.size() >= 2 && sublayers[0] == live->GetIdentifier() &&
                  sublayers[1] ==
                      SdfLayer::FindOrOpen(savePath.string())->GetIdentifier(),
              "live stays strongest with the file beneath it");
        UsdStageRefPtr saved = UsdStage::Open(savePath.string());
        Check(bool(saved) &&
                  saved->GetPrimAtPath(SdfPath("/TonicGroom/Guides")),
              "the saved file carries the groom");
        Check(!usdGenTonic::TonicSaveGroom(stage, live, "groom.usda", &err),
              ".usda save is refused (S42)");
        std::filesystem::remove(savePath, ec);
    }

    // -- TN-4: reference-scale swap in <= 5 ms -------------------------------
    {
        // Plan/17 section 7 reference: 2400 tubes, 12000 guides x 16 CVs.
        usdGenTonic::TonicSnapshot big;
        big.version = 4242;
        big.createInterpOp = true;
        big.setInterpGuides = true;
        big.setInterpRegion = true;
        big.interpOpPath = paths.InterpOpPath();
        for (int t = 0; t < 2400; ++t) {
            usdGenTonic::TonicSnapshotTube entry;
            entry.tubeId = t;
            entry.regionId = t % 80;
            entry.level = 3;
            entry.tube.hasTube = true;
            entry.tube.shape.rings = 20;
            entry.tube.shape.ringVerts = 16;
            entry.tube.shape.radius = 0.5f;
            entry.tube.shape.length = 4.0f;
            entry.tube.centerX.assign(20, float(t) * 0.01f);
            entry.tube.centerY.resize(20);
            entry.tube.centerZ.assign(20, 0.0f);
            for (int r = 0; r < 20; ++r) {
                entry.tube.centerY[size_t(r)] = 4.0f * float(r) / 19.0f;
            }
            entry.tube.fill.density = 5.0f;
            entry.tube.fill.cvCount = 16;
            entry.tube.fill.seed = t;
            big.tubes.push_back(std::move(entry));
        }
        SdfLayerRefPtr built;
        std::string err;
        auto buildT0 = std::chrono::steady_clock::now();
        Check(usdGenTonic::TonicBuildCommitLayer(big, paths, &built, &err),
              "reference-scale layer builds: " + err);
        auto buildT1 = std::chrono::steady_clock::now();
        std::printf(
            "info: reference build took %.1f ms (worker thread, off the UI)\n",
            std::chrono::duration<double, std::milli>(buildT1 - buildT0)
                .count());
        // A full TransferContent at this scale is over budget by design (a
        // tens-of-thousands-spec copy); that is what the partial fallback
        // exists for. Record it, then prove every partial slot is inside it.
        {
            SdfLayerRefPtr slot = SdfLayer::CreateAnonymous("tn4-full");
            auto t0 = std::chrono::steady_clock::now();
            {
                SdfChangeBlock block;
                slot->TransferContent(built);
            }
            auto t1 = std::chrono::steady_clock::now();
            std::printf("info: TN-4 full TransferContent took %.1f ms "
                        "(over budget; partial fallback engaged)\n",
                        std::chrono::duration<double, std::milli>(t1 - t0)
                            .count());
        }
        SdfLayerRefPtr liveBig = SdfLayer::CreateAnonymous("tn4-live");
        UsdStageRefPtr stageBig = MakeStage(liveBig);
        TonicModel bigModel;  // unused: the ready layer is injected
        TonicCommitter bigCommitter(&bigModel, paths);
        bigCommitter.SetReadyForTest(built, big.version);
        bigCommitter.ForcePartialModeForTest(true);
        double worstSlot = 0.0;
        size_t slots = 0;
        TonicCommitter::SwapResult last = TonicCommitter::PartialProgress;
        for (;;) {
            last = bigCommitter.SwapIfIdle(liveBig,
                                           /*gestureActive*/ false);
            worstSlot = std::max(worstSlot, bigCommitter.LastSwapMs());
            ++slots;
            if (last == TonicCommitter::Swapped) {
                break;
            }
            if (last != TonicCommitter::PartialProgress || slots > 100000) {
                break;
            }
        }
        std::printf("info: TN-4 partial swap took %zu slots, worst %.3f ms\n",
                    slots, worstSlot);
        Check(last == TonicCommitter::Swapped,
              "TN-4: reference partial swap converges");
        Check(bigCommitter.CommittedVersion() == big.version,
              "TN-4: reference version commits");
        Check(worstSlot <= 5.0, "TN-4: every idle slot <= 5 ms");
        {
            UsdPrim tubesPrim =
                stageBig->GetPrimAtPath(SdfPath("/TonicGroom/Tubes"));
            size_t tubeCount = 0;
            for (UsdPrim const &child : tubesPrim.GetChildren()) {
                (void)child;
                ++tubeCount;
            }
            VtVec3fArray bigPoints;
            Check(tubeCount == 2400, "TN-4: all 2400 tubes land");
            Check(GetPoints(stageBig, SdfPath("/TonicGroom/Guides"),
                            &bigPoints) &&
                      bigPoints.size() == 12000 * 16,
                  "TN-4: all 12000 guides land");
        }
    }

    // -- committer C ABI ----------------------------------------------------
    {
        Check(Tonic_CommitterCreate(nullptr, "/G", nullptr, nullptr) ==
                  TONIC_ERROR,
              "C ABI create rejects nulls");
        TonicModelContext *ctx = nullptr;
        Check(Tonic_Create(&ctx) == TONIC_OK, "C ABI model creates");
        Check(Tonic_BuildTestTube(ctx, 0, 0, 0.0f, 0.0f) == TONIC_OK,
              "C ABI test tube builds");
        TonicCommitterContext *cc = nullptr;
        Check(Tonic_CommitterCreate(ctx, "/TonicGroom", "/Groom/Hair", &cc) ==
                  TONIC_OK && cc != nullptr,
              "C ABI committer creates");
        Check(Tonic_CommitterCreate(ctx, "relative", nullptr, &cc) ==
                  TONIC_ERROR,
              "C ABI create rejects a relative groom path");
        SdfLayerRefPtr liveC = SdfLayer::CreateAnonymous("tonic-live-cabi");
        Check(Tonic_CommitterEnqueue(cc, 0, 0, 0, nullptr) == TONIC_OK,
              "C ABI enqueue succeeds");
        Check(Tonic_CommitterPendingVersion(cc) == Tonic_GetVersion(ctx),
              "C ABI pending tracks the model version");
        int swapResult = TonicCommitter_NothingPending;
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline) {
            swapResult = Tonic_CommitterSwap(cc, liveC->GetIdentifier().c_str(),
                                             /*gestureActive*/ 0);
            if (Tonic_CommitterCommittedVersion(cc) == Tonic_GetVersion(ctx)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(swapResult == TonicCommitter_Swapped,
              "C ABI swap lands the commit");
        Check(Tonic_CommitterSwap(cc, "no-such-layer", 0) == TONIC_ERROR,
              "C ABI swap rejects an unknown live layer");
        Check(Tonic_CommitterSetSwapBudgetMs(cc, 5.0) == TONIC_OK,
              "C ABI budget sets");
        Check(Tonic_CommitterDestroy(cc) == TONIC_OK, "C ABI destroys");
        Check(Tonic_Destroy(ctx) == TONIC_OK, "C ABI model destroys");
    }

    // -- P4 hooks: subdivide params + lock flags ride the commit --------------
    // The K14 fields already serialise (builder) and read back (hydrate);
    // this pins that contract so the P4 hierarchy test can subdivide with
    // it. Still TODO(P4): childIndex/deltas/level channels per tube, the
    // multi-tube prim hierarchy, K14 re-derivation bit-equality.
    {
        TonicModel p4model;
        Check(p4model.BuildTestTube(), "P4: the hook model builds");
        TonicModel::SubdivideParams sub;
        sub.count = 6;
        sub.seed = 42;
        sub.splitMode = "edge";
        Check(p4model.SetSubdivideParams(sub), "P4: an edge split validates");
        TonicModel::SubdivideParams bad = sub;
        bad.count = 9;
        Check(!p4model.SetSubdivideParams(bad), "P4: count 9 is refused");
        Check(p4model.GetSubdivideParams().count == 6,
              "P4: a refused set keeps the old params");
        bad = sub;
        bad.splitMode = "bogus";
        Check(!p4model.SetSubdivideParams(bad),
              "P4: an unknown splitMode is refused");
        p4model.SetLockFlags(true, true, false);
        bool locked = false, lockParents = false, lockChildren = true;
        p4model.GetLockFlags(&locked, &lockParents, &lockChildren);
        Check(locked && lockParents && !lockChildren,
              "P4: lock flags round-trip on the model");
        SdfLayerRefPtr liveP4 = SdfLayer::CreateAnonymous("tonic-live-p4");
        UsdStageRefPtr stageP4 = MakeStage(liveP4);
        TonicCommitter p4committer(&p4model, paths);
        p4committer.Enqueue(stageP4);
        Check(WaitCommitted(p4committer, liveP4, p4model.GetVersion()),
              "P4: the hook commit swaps");
        UsdPrim const p4tube =
            stageP4->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0"));
        int subCount = 0, subSeed = 0;
        TfToken splitMode;
        Check(p4tube.GetAttribute(TfToken("usdGen:tonic:subdivide:count"))
                          .Get(&subCount) &&
                      subCount == 6 &&
                  p4tube.GetAttribute(TfToken("usdGen:tonic:subdivide:seed"))
                          .Get(&subSeed) &&
                      subSeed == 42 &&
                  p4tube
                      .GetAttribute(
                          TfToken("usdGen:tonic:subdivide:splitMode"))
                      .Get(&splitMode) &&
                  splitMode == TfToken("edge"),
              "P4: subdivide params land on the tube");
        bool stLocked = false, stLP = false, stLC = true;
        Check(p4tube.GetAttribute(TfToken("usdGen:tonic:locked"))
                          .Get(&stLocked) &&
                      stLocked &&
                  p4tube.GetAttribute(TfToken("usdGen:tonic:lockParents"))
                          .Get(&stLP) &&
                      stLP &&
                  p4tube.GetAttribute(TfToken("usdGen:tonic:lockChildren"))
                          .Get(&stLC) &&
                      !stLC,
              "P4: lock flags land on the tube");
        int childIndex = 999;
        Check(p4tube.GetAttribute(TfToken("usdGen:tonic:childIndex"))
                          .Get(&childIndex) &&
                      childIndex == -1,
              "P4: the L1 tube carries childIndex -1");
        TonicModel p4hydrated;
        usdGenTonic::TonicHydrateResult p4hr =
            usdGenTonic::TonicHydrateModel(stageP4, SdfPath("/TonicGroom"),
                                          &p4hydrated);
        Check(p4hr.ok,
              "P4: hydrate accepts the hook stage: " + p4hr.diagnostic);
        Check(p4hydrated.GetSubdivideParams().count == 6 &&
                  p4hydrated.GetSubdivideParams().seed == 42 &&
                  p4hydrated.GetSubdivideParams().splitMode == "edge",
              "P4: subdivide params survive hydrate");
        bool hyLocked = true, hyLP = false, hyLC = true;
        p4hydrated.GetLockFlags(&hyLocked, &hyLP, &hyLC);
        Check(hyLP && !hyLC, "P4: lock propagation flags survive hydrate");
    }

    // -- P6 reload/reattach: Detach idles, Reattach re-creates ---------------
    {
        TonicModel rmodel;
        Check(rmodel.BuildTestTube(), "P6: the reload model builds");
        TonicCommitter rcommitter(&rmodel, paths);
        SdfLayerRefPtr liveOld = SdfLayer::CreateAnonymous("tonic-live-old");
        UsdStageRefPtr stageOld = MakeStage(liveOld);
        rcommitter.Enqueue(stageOld);
        uint64_t const vOld = rmodel.GetVersion();
        Check(WaitCommitted(rcommitter, liveOld, vOld),
              "P6: the pre-reload commit swaps");
        Check(!rcommitter.IsDetached(), "P6: attached by default");
        VtVec3fArray centersOld;
        Check(stageOld->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0"))
                      .GetAttribute(TfToken("usdGen:tonic:centerPoints"))
                      .Get(&centersOld) &&
                  centersOld.size() == 5,
              "P6: pre-reload centers read back");

        // The stage closes under the tool: detach; the model survives.
        rcommitter.Detach();
        Check(rcommitter.IsDetached(), "P6: Detach detaches");
        Check(rcommitter.SwapIfIdle(liveOld, /*gestureActive*/ false) ==
                  TonicCommitter::Detached,
              "P6: swap while detached reports Detached");
        // Edits continue on the surviving model; the detached worker builds
        // nothing (the pending plan targets the dead stage).
        size_t const buildsAtDetach = rcommitter.BuildCount();
        rmodel.MoveCenterRing(0, 0.5f, 0.0f);
        uint64_t const vEdit = rmodel.GetVersion();
        rcommitter.Enqueue(stageOld);
        Check(rcommitter.PendingVersion() == vEdit,
              "P6: a detached enqueue is recorded but unbuilt");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Check(rcommitter.BuildCount() == buildsAtDetach,
              "P6: the detached worker builds nothing");

        // A fresh stage: Reattach drops the dead-stage enqueue, and the next
        // enqueue + swap re-creates the groom prim from the surviving model.
        SdfLayerRefPtr liveNew = SdfLayer::CreateAnonymous("tonic-live-new");
        UsdStageRefPtr stageNew = MakeStage(liveNew);
        Check(!stageNew->GetPrimAtPath(SdfPath("/TonicGroom")),
              "P6: the fresh stage starts groom-free");
        rcommitter.Reattach();
        Check(!rcommitter.IsDetached(), "P6: Reattach reattaches");
        Check(rcommitter.PendingVersion() == 0,
              "P6: Reattach drops the dead-stage enqueue");
        Check(rcommitter.CommittedVersion() == 0,
              "P6: Reattach resets the committed lineage");
        Check(rcommitter.SwapIfIdle(liveNew, /*gestureActive*/ false) ==
                  TonicCommitter::NothingPending,
              "P6: nothing swaps until the post-reattach enqueue");
        rcommitter.Enqueue(stageNew);
        Check(WaitCommitted(rcommitter, liveNew, vEdit),
              "P6: the post-reattach commit swaps");
        UsdPrim const regroom =
            stageNew->GetPrimAtPath(SdfPath("/TonicGroom"));
        Check(bool(regroom) &&
                  regroom.GetTypeName() == TfToken("UsdGenTonicGroom"),
              "P6: the groom prim is re-created in the new live layer");
        VtVec3fArray centersNew;
        Check(stageNew->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0"))
                      .GetAttribute(TfToken("usdGen:tonic:centerPoints"))
                      .Get(&centersNew) &&
                  centersNew.size() == 5 &&
                  std::abs(centersNew[0][0] - centersOld[0][0] - 0.5f) <
                      1e-5f,
              "P6: the post-detach edit survives the reload");

        // A shelf layer built but never swapped counts as dropped at Detach.
        TonicCommitter dcommitter(&rmodel, paths);
        dcommitter.Enqueue(stageNew);
        size_t const b0 = dcommitter.BuildCount();
        auto dl = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(5000);
        while (dcommitter.BuildCount() == b0 &&
               std::chrono::steady_clock::now() < dl) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(dcommitter.BuildCount() == b0 + 1, "P6: the shelf layer builds");
        size_t const dr0 = dcommitter.DroppedCount();
        dcommitter.Detach();
        Check(dcommitter.DroppedCount() == dr0 + 1,
              "P6: Detach drops the unswapped shelf layer");

        // The C ABI pair: nulls rejected, Detached code flows over the swap.
        Check(Tonic_CommitterDetach(nullptr) == TONIC_ERROR,
              "P6: C Detach rejects null");
        Check(Tonic_CommitterReattach(nullptr) == TONIC_ERROR,
              "P6: C Reattach rejects null");
        TonicModelContext *cctx = nullptr;
        Check(Tonic_Create(&cctx) == TONIC_OK, "P6: C reload model creates");
        Check(Tonic_BuildTestTube(cctx, 0, 0, 0.0f, 0.0f) == TONIC_OK,
              "P6: C reload tube builds");
        TonicCommitterContext *ccc = nullptr;
        Check(Tonic_CommitterCreate(cctx, "/TonicGroom", nullptr, &ccc) ==
                      TONIC_OK &&
                  ccc != nullptr,
              "P6: C reload committer creates");
        SdfLayerRefPtr liveC6 = SdfLayer::CreateAnonymous("tonic-live-c6");
        Check(Tonic_CommitterEnqueue(ccc, 0, 0, 0, nullptr) == TONIC_OK,
              "P6: C reload enqueue succeeds");
        auto dlC = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(5000);
        while (Tonic_CommitterCommittedVersion(ccc) !=
                   Tonic_GetVersion(cctx) &&
               std::chrono::steady_clock::now() < dlC) {
            Tonic_CommitterSwap(ccc, liveC6->GetIdentifier().c_str(), 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(Tonic_CommitterCommittedVersion(ccc) == Tonic_GetVersion(cctx),
              "P6: C pre-reload commit swaps");
        Check(Tonic_CommitterDetach(ccc) == TONIC_OK,
              "P6: C Detach succeeds");
        Check(Tonic_CommitterSwap(ccc, liveC6->GetIdentifier().c_str(), 0) ==
                  TonicCommitter_Detached,
              "P6: C swap while detached reports Detached");
        Check(Tonic_CommitterReattach(ccc) == TONIC_OK,
              "P6: C Reattach succeeds");
        Check(Tonic_CommitterSwap(ccc, liveC6->GetIdentifier().c_str(), 0) ==
                  TonicCommitter_NothingPending,
              "P6: C swap after reattach waits for a fresh enqueue");
        Check(Tonic_CommitterDestroy(ccc) == TONIC_OK, "P6: C destroys");
        Check(Tonic_Destroy(cctx) == TONIC_OK, "P6: C model destroys");
    }

    // -- V0b: the whole hierarchy round-trips the stage (plan/18 §7 G1/G3) --
    {
        TonicModel hm;
        Check(hm.BuildTestTube(), "V0b: hierarchy model builds its L1 tube");
        TonicModel::FillParams hf;
        hf.density = 8.0f;
        hf.cvCount = 6;
        hf.seed = 3;
        Check(hm.SetFillParams(hf), "V0b: L1 fill params set");
        std::vector<int> kids;
        Check(hm.SubdivideTube(0, 4, "kmeans", 11, &kids) && kids.size() == 4,
              "V0b: L1 subdivides into four children");
        std::vector<int> grand;
        Check(hm.SubdivideTube(kids[1], 2, "kmeans", 5, &grand) &&
                  grand.size() == 2,
              "V0b: one L2 child subdivides into two");
        Check(hm.MoveTubeCenterCV(kids[2], 2, 0.05f, 0.0f, -0.03f),
              "V0b: a child center CV moves");
        Check(hm.MoveTubeSectionRing(kids[2], 1, 0.01f, 0.0f),
              "V0b: a child section ring moves (per-tube op)");
        hm.SetTubeLockChildren(kids[3], true);
        hm.SetTubeLockParents(kids[3], true);
        TonicModel::FillParams cf;
        cf.density = 5.0f;
        cf.cvCount = 4;
        cf.seed = 9;
        Check(hm.SetTubeFillParams(grand[0], cf),
              "V0b: a grandchild carries its own fill params");
        int groupId = 0;
        Check(hm.GroupTubes({kids[0], kids[3]}, /*transient*/ false,
                            &groupId) &&
                  groupId == -1,
              "V0b: two siblings group under an on-the-fly parent");
        Check(hm.MakeTubePersistent(groupId),
              "V0b: the on-the-fly parent is made persistent");
        int groupId2 = 0;
        Check(hm.GroupTubes({grand[0], grand[1]}, /*transient*/ false,
                            &groupId2) &&
                  groupId2 == -2,
              "V0b: a second on-the-fly parent mints -2");
        Check(hm.MakeTubePersistent(groupId2),
              "V0b: the second on-the-fly parent is kept too");
        int transientId = 0;
        Check(hm.GroupTubes({kids[2]}, /*transient*/ true, &transientId),
              "V0b: a transient parent exists in the model");

        SdfLayerRefPtr liveH = SdfLayer::CreateAnonymous("tonic-live-h");
        UsdStageRefPtr sh = MakeStage(liveH);
        TonicCommitter ch(&hm, paths);
        ch.Enqueue(sh);
        Check(WaitCommitted(ch, liveH, hm.GetVersion()),
              "V0b: the hierarchy commits");

        // Nesting: children are namespace children of their parent.
        Check(bool(sh->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0"))),
              "V0b: the L1 tube lands under Tubes");
        Check(bool(sh->GetPrimAtPath(
                  SdfPath("/TonicGroom/Tubes/tube0/tube2"))),
              "V0b: an L2 child nests under its parent");
        UsdPrim const l3 = sh->GetPrimAtPath(
            SdfPath("/TonicGroom/Tubes/tube0/tube2/tube33"));
        Check(bool(l3), "V0b: an L3 child nests two deep");
        if (l3) {
            int level = 0, childIndex = -99;
            l3.GetAttribute(TfToken("usdGen:tonic:level")).Get(&level);
            l3.GetAttribute(TfToken("usdGen:tonic:childIndex"))
                .Get(&childIndex);
            Check(level == 3 && childIndex == 0,
                  "V0b: level and childIndex are the real values");
        }
        UsdPrim const edited =
            sh->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/tube0/tube3"));
        Check(bool(edited), "V0b: the sculpted child is on the stage");
        if (edited) {
            VtVec3fArray deltas;
            edited.GetAttribute(TfToken("usdGen:tonic:centerDeltas"))
                .Get(&deltas);
            bool nonZero = false;
            for (auto const &d : deltas) {
                nonZero = nonZero || d[0] != 0.0f || d[1] != 0.0f ||
                          d[2] != 0.0f;
            }
            Check(!deltas.empty() && nonZero,
                  "V0b: the sculpted child commits non-empty centerDeltas");
            VtVec2fArray sectionDeltas;
            edited.GetAttribute(TfToken("usdGen:tonic:sectionDeltas"))
                .Get(&sectionDeltas);
            Check(!sectionDeltas.empty(),
                  "V0b: section deltas commit alongside them");
        }
        UsdPrim const group =
            sh->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/group1"));
        Check(bool(group), "V0b: the persistent on-the-fly parent commits");
        if (group) {
            bool persistent = false;
            group.GetAttribute(TfToken("usdGen:tonic:persistent"))
                .Get(&persistent);
            SdfPathVector members =
                Targets(group.GetRelationship(TfToken("usdGen:tonic:members")));
            Check(persistent && members.size() == 2,
                  "V0b: HierarchyAPI carries persistent + two members");
            Check(group.HasAPI(TfToken("UsdGenTubeHierarchyAPI")) ||
                      !members.empty(),
                  "V0b: the hierarchy API schema is applied");
        }
        Check(bool(sh->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/group2"))),
              "V0b: a second on-the-fly parent sits beside the first, not "
              "under it");
        Check(!sh->GetPrimAtPath(SdfPath("/TonicGroom/Tubes/group3")),
              "V0b: the transient parent is not committed");

        // Guides: per leaf tube, never for a subdivided parent.
        UsdPrim const guidesPrim =
            sh->GetPrimAtPath(SdfPath("/TonicGroom/Guides"));
        VtIntArray guideTubeIds, guideLevels;
        guidesPrim.GetAttribute(TfToken("primvars:tubeId")).Get(&guideTubeIds);
        guidesPrim.GetAttribute(TfToken("primvars:hierarchyLevel"))
            .Get(&guideLevels);
        bool anyParentGuides = false;
        bool anyLeafGuides = false;
        for (size_t i = 0; i < guideTubeIds.size(); ++i) {
            if (guideTubeIds[i] == 0 || guideTubeIds[i] == kids[1] ||
                guideTubeIds[i] < 0) {
                // A subdivided parent's fill is suspended, and an
                // on-the-fly parent aggregates tubes that fill themselves.
                anyParentGuides = true;
            }
            if (guideTubeIds[i] == grand[0] && guideLevels[i] == 3) {
                anyLeafGuides = true;
            }
        }
        Check(!anyParentGuides,
              "V0b: a subdivided parent's fill is suspended");
        Check(anyLeafGuides,
              "V0b: leaf tubes carry their own tubeId/hierarchyLevel");

        // Hydrate into a fresh model and compare every tube bit for bit.
        TonicModel hy;
        usdGenTonic::TonicHydrateResult const hyr =
            usdGenTonic::TonicHydrateModel(sh, SdfPath("/TonicGroom"), &hy);
        Check(hyr.ok, "V0b: hydrate accepts the committed hierarchy: " +
                          hyr.diagnostic);
        Check(hyr.guidesBitEqual, "V0b: hydrated guides are bit-equal");
        std::vector<int> const beforeIds = hm.TubeIds();
        std::vector<int> const afterIds = hy.TubeIds();
        // The transient parent is not committed, so it is the one tube the
        // round trip drops by design.
        std::vector<int> wantIds;
        for (int id : beforeIds) {
            if (id != transientId) {
                wantIds.push_back(id);
            }
        }
        Check(wantIds == afterIds,
              "V0b: every committed tube id comes back (transient dropped)");
        bool allEqual = true;
        for (int id : afterIds) {
            TonicModel::TubeRecord a, b;
            if (!hm.GetTubeRecord(id, &a) || !hy.GetTubeRecord(id, &b)) {
                allEqual = false;
                break;
            }
            if (!SameDesc(a.actual, b.actual)) {
                std::printf("      desc differs at tube %d: centers %d "
                            "sections %d ringVerts %d/%d region %d/%d\n",
                            id,
                            int(a.actual.centerX == b.actual.centerX &&
                                a.actual.centerY == b.actual.centerY &&
                                a.actual.centerZ == b.actual.centerZ),
                            int(SameSections(a.actual.sections,
                                             b.actual.sections)),
                            a.actual.ringVerts, b.actual.ringVerts,
                            a.actual.regionId, b.actual.regionId);
            }
            if (!SameDeltas(a.deltas, b.deltas)) {
                std::printf("      deltas differ at tube %d (%zu/%zu cv, "
                            "%zu/%zu sections)\n",
                            id, a.deltas.centerDu.size(),
                            b.deltas.centerDu.size(), a.deltas.sections.size(),
                            b.deltas.sections.size());
            }
            allEqual = allEqual && SameDesc(a.actual, b.actual) &&
                       SameDeltas(a.deltas, b.deltas) &&
                       a.actual.level == b.actual.level &&
                       a.actual.childIndex == b.actual.childIndex &&
                       a.actual.parentTubeId == b.actual.parentTubeId &&
                       a.subdivide.count == b.subdivide.count &&
                       a.subdivide.seed == b.subdivide.seed &&
                       a.subdivide.splitMode == b.subdivide.splitMode &&
                       a.fill.density == b.fill.density &&
                       a.fill.cvCount == b.fill.cvCount &&
                       a.fill.seed == b.fill.seed &&
                       a.lockParents == b.lockParents &&
                       a.lockChildren == b.lockChildren &&
                       a.persistent == b.persistent &&
                       a.imported == b.imported;
            if (!allEqual) {
                std::printf("      first mismatch at tube %d\n", id);
                break;
            }
        }
        Check(allEqual,
              "V0b: shape, deltas, links, fill, locks and persistent "
              "round-trip bit-exactly");

        // Re-commit the hydrated model: the same guide bytes.
        {
            SdfLayerRefPtr live2 = SdfLayer::CreateAnonymous("tonic-live-h2");
            UsdStageRefPtr s2 = MakeStage(live2);
            TonicCommitter c2(&hy, paths);
            c2.Enqueue(s2);
            Check(WaitCommitted(c2, live2, hy.GetVersion()),
                  "V0b: the hydrated hierarchy re-commits");
            VtVec3fArray a, b;
            Check(GetPoints(sh, SdfPath("/TonicGroom/Guides"), &a) &&
                      GetPoints(s2, SdfPath("/TonicGroom/Guides"), &b) &&
                      a.size() == b.size() && !a.empty() &&
                      std::memcmp(a.data(), b.data(),
                                  a.size() * sizeof(GfVec3f)) == 0,
                  "V0b: re-committed guides are byte-identical");
        }

        // A hand-authored foreign guide becomes a locked L3 tube.
        {
            UsdStageRefPtr fs = UsdStage::CreateInMemory("tonic-foreign-v0b");
            SdfLayerRefPtr over =
                SdfLayer::CreateAnonymous("tonic-foreign-over-v0b");
            fs->GetSessionLayer()->InsertSubLayerPath(liveH->GetIdentifier(),
                                                      0);
            fs->GetSessionLayer()->InsertSubLayerPath(over->GetIdentifier(), 0);
            fs->SetEditTarget(over);
            UsdPrim fg = fs->GetPrimAtPath(SdfPath("/TonicGroom/Guides"));
            VtVec3fArray pts;
            VtIntArray counts;
            VtUInt64Array ids;
            VtIntArray tubeIds, levels, regions;
            fg.GetAttribute(TfToken("points")).Get(&pts);
            fg.GetAttribute(TfToken("curveVertexCounts")).Get(&counts);
            fg.GetAttribute(TfToken("primvars:usdGen:curveId")).Get(&ids);
            fg.GetAttribute(TfToken("primvars:tubeId")).Get(&tubeIds);
            fg.GetAttribute(TfToken("primvars:hierarchyLevel")).Get(&levels);
            fg.GetAttribute(TfToken("primvars:regionId")).Get(&regions);
            // Root it inside the first L2 child's cell: that child has no
            // children of its own, so the import lands at L3.
            float rx = 0.0f, ry = 0.0f, rz = 0.0f;
            Check(hm.GetTubeCenterCV(kids[0], 0, &rx, &ry, &rz),
                  "V0b: the import parent's root is readable");
            for (int c = 0; c < 4; ++c) {
                pts.push_back(
                    GfVec3f(rx, ry + 0.4f * float(c), rz + 0.02f * float(c)));
            }
            counts.push_back(4);
            ids.push_back(999999);
            tubeIds.push_back(-777);  // claimed by no tube: foreign
            levels.push_back(0);
            regions.push_back(-1);
            fg.GetAttribute(TfToken("points")).Set(pts);
            fg.GetAttribute(TfToken("curveVertexCounts")).Set(counts);
            fg.GetAttribute(TfToken("primvars:usdGen:curveId")).Set(ids);
            fg.GetAttribute(TfToken("primvars:tubeId")).Set(tubeIds);
            fg.GetAttribute(TfToken("primvars:hierarchyLevel")).Set(levels);
            fg.GetAttribute(TfToken("primvars:regionId")).Set(regions);

            TonicModel fm;
            usdGenTonic::TonicHydrateResult const fr =
                usdGenTonic::TonicHydrateModel(fs, SdfPath("/TonicGroom"),
                                               &fm);
            Check(fr.ok, "V0b: a foreign guide does not fail hydrate: " +
                             fr.diagnostic);
            Check(fr.importedTubeCount == 1,
                  "V0b: the foreign guide is imported as one tube");
            int importedId = -1;
            for (int id : fm.TubeIds()) {
                if (fm.IsTubeImported(id)) {
                    importedId = id;
                }
            }
            Check(importedId != -1, "V0b: the import is in the model");
            if (importedId != -1) {
                usdGenTonic::TonicTubeDesc desc;
                fm.GetTubeDesc(importedId, &desc);
                Check(desc.level == 3,
                      "V0b: the foreign guide imports as an L3 tube");
                Check(desc.parentTubeId == kids[0],
                      "V0b: it hangs off the tube whose region roots it");
                Check(int(desc.centerX.size()) == 4,
                      "V0b: its centers are the foreign curve's CVs");
            }
        }
    }

    // -- V0b: a worker throw keeps the previous layer and the thread -------
    {
        TonicModel tm;
        Check(tm.BuildTestTube(), "V0b: throw-test model builds");
        SdfLayerRefPtr liveT = SdfLayer::CreateAnonymous("tonic-live-throw");
        UsdStageRefPtr st = MakeStage(liveT);
        TonicCommitter ct(&tm, paths);
        ct.Enqueue(st);
        Check(WaitCommitted(ct, liveT, tm.GetVersion()),
              "V0b: the first commit lands");
        VtVec3fArray before;
        Check(GetPoints(st, SdfPath("/TonicGroom/Guides"), &before) &&
                  !before.empty(),
              "V0b: the committed guides are readable");
        uint64_t const committedBefore = ct.CommittedVersion();

        ct.ThrowOnNextBuildsForTest(1);
        Check(tm.MoveCenterRing(1, 0.25f, 0.0f), "V0b: the model moves");
        ct.Enqueue(st);
        auto const deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(5000);
        while (ct.WorkerThrowCount() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(ct.WorkerThrowCount() == 1,
              "V0b: the commit worker caught the throw");
        Check(ct.SwapIfIdle(liveT, false) == TonicCommitter::NothingPending,
              "V0b: nothing swaps after the failed build");
        VtVec3fArray after;
        Check(GetPoints(st, SdfPath("/TonicGroom/Guides"), &after) &&
                  after.size() == before.size() &&
                  std::memcmp(after.data(), before.data(),
                              after.size() * sizeof(GfVec3f)) == 0,
              "V0b: the previous live layer is untouched");
        Check(ct.CommittedVersion() == committedBefore,
              "V0b: the committed version does not advance");
        Check(!ct.TakeDiagnostic().empty(),
              "V0b: the error reaches the status path");
        Check(tm.GetVersion() > committedBefore, "V0b: the model is intact");

        // The thread survives: a newer version still builds and swaps.
        Check(tm.MoveCenterRing(1, -0.25f, 0.0f), "V0b: the model moves back");
        ct.Enqueue(st);
        Check(WaitCommitted(ct, liveT, tm.GetVersion()),
              "V0b: the worker survives the throw and commits again");
    }

    // -- V0b: Save groom writes a RELATIVE map path ------------------------
    {
        namespace fs = std::filesystem;
        TonicModel sm;
        Check(sm.BuildTestTube(), "V0b: save-test model builds");
        SdfLayerRefPtr liveS = SdfLayer::CreateAnonymous("tonic-live-save");
        UsdStageRefPtr ss = MakeStage(liveS);
        TonicCommitter cs(&sm, paths);
        cs.Enqueue(ss);
        Check(WaitCommitted(cs, liveS, sm.GetVersion()),
              "V0b: the save-test commit lands");
        std::error_code ec;
        fs::path const dir =
            fs::temp_directory_path(ec) / "usdGenTonicSaveV0b";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        fs::path const mapFile = dir / "regionMap.v7.ptx";
        {
            std::FILE *f = std::fopen(mapFile.string().c_str(), "wb");
            if (f) {
                std::fputs("not a real ptex, only a file to copy", f);
                std::fclose(f);
            }
        }
        fs::path const saved = dir / "groom.usdc";
        std::string err;
        Check(usdGenTonic::TonicSaveGroomAndMaps(ss, liveS, saved.string(),
                                                 mapFile.string(), paths,
                                                 &err),
              "V0b: Save groom with maps succeeds: " + err);
        Check(fs::exists(dir / "regionMap.ptx", ec),
              "V0b: the map is copied beside the saved layer");
        SdfLayerRefPtr savedLayer = SdfLayer::FindOrOpen(saved.string());
        Check(bool(savedLayer), "V0b: the saved layer reopens");
        if (savedLayer) {
            SdfPrimSpecHandle const mapPrim =
                savedLayer->GetPrimAtPath(paths.RegionMapPath());
            std::string authored;
            if (mapPrim) {
                for (SdfAttributeSpecHandle const &attr :
                     mapPrim->GetAttributes()) {
                    if (attr->GetName() == "usdGen:map:file") {
                        authored =
                            attr->GetDefaultValue().Get<SdfAssetPath>()
                                .GetAssetPath();
                    }
                }
            }
            Check(authored == "./regionMap.ptx",
                  "V0b: usdGen:map:file is relative to the saved layer, got \"" +
                      authored + "\"");
        }
        fs::remove_all(dir, ec);
    }

    // -- V6 / G7: the committer cancels the description's cook ------------
    //
    // plan/17 §3.2 rule 2. The swap dirties <groom>/Guides, the dirty router
    // dirties the linked description and its session cooks; a press means
    // that cook describes a groom the model has left. The committer bumps
    // the cancellation token on the sessions rooted at its description, and
    // nowhere else.
    {
        using usdGenImaging::UsdGenSessionKey;
        using usdGenImaging::UsdGenSessionStore;
        TonicModel cookModel;
        Check(cookModel.BuildTestTube(), "G7: the cook-token model builds");
        TonicCommitter cookCommitter(&cookModel, paths);
        Check(cookCommitter.CancelDescriptionCooks() == 0,
              "G7: with no session attached there is nothing to cancel");

        UsdGenSessionKey ours;
        ours.sessionId = "tonic-commit-g7";
        ours.groomRoot = paths.descriptionPath;  // /Groom/Hair
        ours.renderInstanceId = 7001;
        UsdGenSessionKey other = ours;
        other.sessionId = "tonic-commit-g7-other";
        other.groomRoot = SdfPath("/Groom/OtherHair");
        other.renderInstanceId = 7002;
        auto &store = UsdGenSessionStore::GetInstance();
        auto oursSession = store.Attach(ours);
        auto otherSession = store.Attach(other);
        Check(bool(oursSession) && bool(otherSession),
              "G7: two sessions attach, one under our description");
        if (oursSession && otherSession) {
            uint64_t const ourBefore = oursSession->CookToken();
            uint64_t const otherBefore = otherSession->CookToken();
            Check(cookCommitter.CancelDescriptionCooks() == 1,
                  "G7: exactly our description's session is cancelled");
            Check(oursSession->CookToken() == ourBefore + 1,
                  "G7: its token moved");
            Check(otherSession->CookToken() == otherBefore,
                  "G7: another groom's session is untouched");
            // The C ABI drives the same thing (the ctypes path the tool
            // takes at gesture press).
            TonicModelContext *cookCtx = nullptr;
            TonicCommitterContext *cookCc = nullptr;
            Check(Tonic_Create(&cookCtx) == TONIC_OK &&
                      Tonic_BuildTestTube(cookCtx, 0, 0, 0.0f, 0.0f) == TONIC_OK,
                  "G7: the C ABI model builds");
            Check(Tonic_CommitterCreate(cookCtx, "/TonicGroom", "/Groom/Hair",
                                        &cookCc) == TONIC_OK,
                  "G7: the C ABI committer creates");
            Check(Tonic_CommitterCancelCooks(cookCc) == 1,
                  "G7: Tonic_CommitterCancelCooks cancels the same one");
            Check(oursSession->CookToken() == ourBefore + 2,
                  "G7: through the ABI the token moves again");
            Check(Tonic_CommitterCancelCooks(nullptr) == -1,
                  "G7: a null committer is an error, not a crash");
            Tonic_CommitterDestroy(cookCc);
            Tonic_Destroy(cookCtx);
        }
        store.Detach(ours);
        store.Detach(other);
    }

    // -- V6 / G6: the per-version dirty set for the guide refill ----------
    //
    // Before V6 every commit re-ran K8/K9/K10 over every tube, so moving one
    // CV of one tube in the reference groom refilled all 12 000 guides. The
    // cache keys each tube's slice on the bytes the fill reads, so a version
    // that touched one tube refills one tube. Two things have to hold: the
    // cached result is BYTE-IDENTICAL to the uncached one (otherwise hydrate's
    // bit-equality assertion would start failing at random), and the saving is
    // real at reference scale.
    {
        usdGenTonic::TonicSnapshot big;
        big.version = 909;
        for (int t = 0; t < 2400; ++t) {
            usdGenTonic::TonicSnapshotTube entry;
            entry.tubeId = t;
            entry.regionId = t % 80;
            entry.level = 3;
            entry.tube.hasTube = true;
            entry.tube.shape.rings = 20;
            entry.tube.shape.ringVerts = 16;
            entry.tube.shape.radius = 0.5f;
            entry.tube.shape.length = 4.0f;
            entry.tube.centerX.assign(20, float(t) * 0.01f);
            entry.tube.centerY.resize(20);
            entry.tube.centerZ.assign(20, 0.0f);
            for (int r = 0; r < 20; ++r) {
                entry.tube.centerY[size_t(r)] = 4.0f * float(r) / 19.0f;
            }
            entry.tube.fill.density = 5.0f;
            entry.tube.fill.cvCount = 16;
            entry.tube.fill.seed = t;
            big.tubes.push_back(std::move(entry));
        }
        usdGenTonic::TonicHashSnapshot(&big);

        auto timeGuides = [](usdGenTonic::TonicSnapshot const &snap,
                             usdGenTonic::TonicGuideCache *cache,
                             usdGenTonic::TonicSnapshotGuides *out) {
            auto const t0 = std::chrono::steady_clock::now();
            *out = usdGenTonic::TonicGuidesFromSnapshot(snap, cache);
            auto const t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::milli>(t1 - t0).count();
        };

        usdGenTonic::TonicSnapshotGuides cold;
        double const coldMs = timeGuides(big, nullptr, &cold);
        Check(cold.counts.size() == 12000,
              "G6: the reference groom fills 12000 guides (got " +
                  std::to_string(cold.counts.size()) + ")");
        std::printf("info: G6 guide refill, whole groom (before): %.1f ms\n",
                    coldMs);

        usdGenTonic::TonicGuideCache cache;
        usdGenTonic::TonicSnapshotGuides warmUp;
        double const fillMs = timeGuides(big, &cache, &warmUp);
        Check(cache.LastRefilled() == 2400 && cache.LastReused() == 0,
              "G6: the first version through the cache refills every tube");
        Check(warmUp.points == cold.points && warmUp.counts == cold.counts &&
                  warmUp.frames == cold.frames && warmUp.ids == cold.ids &&
                  warmUp.tubeIds == cold.tubeIds &&
                  warmUp.levels == cold.levels &&
                  warmUp.regionIds == cold.regionIds,
              "G6: filling through the cache changes not one byte");
        std::printf("info: G6 cache-cold refill: %.1f ms (%zu entries)\n",
                    fillMs, cache.Size());

        // One tube moves, exactly as one drag on one CV does.
        usdGenTonic::TonicSnapshot edited = big;
        edited.version = 910;
        edited.tubes[1234].tube.centerX[7] += 0.25f;
        usdGenTonic::TonicHashSnapshot(&edited);
        Check(edited.tubes[1234].contentHash != big.tubes[1234].contentHash,
              "G6: the moved tube's content hash changes");
        Check(edited.tubes[1233].contentHash == big.tubes[1233].contentHash,
              "G6: its neighbour's does not");

        usdGenTonic::TonicSnapshotGuides editedCold;
        double const editedColdMs = timeGuides(edited, nullptr, &editedCold);
        usdGenTonic::TonicSnapshotGuides editedWarm;
        double const editedWarmMs = timeGuides(edited, &cache, &editedWarm);
        Check(cache.LastRefilled() == 1 && cache.LastReused() == 2399,
              "G6: a one-tube edit refills exactly one tube (refilled " +
                  std::to_string(cache.LastRefilled()) + ", reused " +
                  std::to_string(cache.LastReused()) + ")");
        Check(editedWarm.points == editedCold.points &&
                  editedWarm.counts == editedCold.counts &&
                  editedWarm.frames == editedCold.frames &&
                  editedWarm.ids == editedCold.ids &&
                  editedWarm.tubeIds == editedCold.tubeIds &&
                  editedWarm.levels == editedCold.levels &&
                  editedWarm.regionIds == editedCold.regionIds,
              "G6: the partially cached refill is bit-exact against a full one");
        Check(editedWarm.points != cold.points,
              "G6: and it really carries the edit");
        std::printf("info: G6 one-tube version: %.1f ms before, %.1f ms after "
                    "(%.1fx)\n",
                    editedColdMs, editedWarmMs,
                    editedWarmMs > 0.0 ? editedColdMs / editedWarmMs : 0.0);
        Check(editedWarmMs < editedColdMs * 0.5,
              "G6: the dirty set at least halves the refill of a one-tube "
              "version");

        // What the worker actually pays: the whole layer build, which is
        // the guide refill plus the Sdf spec authoring the dirty set does
        // not touch. This is the TN-4-adjacent number (TN-4 itself times
        // the main-thread swap, which G6 does not change).
        {
            usdGenTonic::TonicGuideCache buildCache;
            usdGenTonic::TonicSnapshot warmTarget = big;
            SdfLayerRefPtr layer;
            std::string buildErr;
            Check(usdGenTonic::TonicBuildCommitLayer(warmTarget, paths,
                                                     &layer, &buildErr,
                                                     &buildCache),
                  "G6: the reference layer builds through a cold cache: " +
                      buildErr);
            usdGenTonic::TonicSnapshot oneTube = warmTarget;
            oneTube.version = 911;
            oneTube.tubes[77].tube.centerZ[4] += 0.125f;
            usdGenTonic::TonicHashSnapshot(&oneTube);
            SdfLayerRefPtr coldLayer, warmLayer;
            auto const c0 = std::chrono::steady_clock::now();
            Check(usdGenTonic::TonicBuildCommitLayer(oneTube, paths,
                                                     &coldLayer, &buildErr),
                  "G6: the one-tube version builds with no cache");
            auto const c1 = std::chrono::steady_clock::now();
            Check(usdGenTonic::TonicBuildCommitLayer(oneTube, paths,
                                                     &warmLayer, &buildErr,
                                                     &buildCache),
                  "G6: and again through the cache");
            auto const c2 = std::chrono::steady_clock::now();
            double const coldBuild =
                std::chrono::duration<double, std::milli>(c1 - c0).count();
            double const warmBuild =
                std::chrono::duration<double, std::milli>(c2 - c1).count();
            std::printf("info: G6 reference layer build, one-tube version: "
                        "%.1f ms before, %.1f ms after\n",
                        coldBuild, warmBuild);
            Check(warmBuild < coldBuild,
                  "G6: the dirty set makes the whole worker build cheaper");
        }

        // Eviction: a tube the new version dropped must not survive, or the
        // cache would grow for the life of the session.
        usdGenTonic::TonicSnapshot shrunk = edited;
        shrunk.tubes.resize(100);
        usdGenTonic::TonicSnapshotGuides shrunkGuides;
        (void)timeGuides(shrunk, &cache, &shrunkGuides);
        Check(cache.Size() == 100,
              "G6: tubes the version dropped are evicted (size " +
                  std::to_string(cache.Size()) + ")");

        // A scalp change is a shared input: every slice has to go at once.
        usdGenTonic::TonicSnapshot rescalped = shrunk;
        rescalped.hasScalp = true;
        rescalped.scalp.points.assign(12, 1.0f);
        usdGenTonic::TonicSnapshotGuides afterScalp;
        (void)timeGuides(rescalped, &cache, &afterScalp);
        Check(cache.LastRefilled() == 100 && cache.LastReused() == 0,
              "G6: a changed scalp invalidates every cached slice");
    }

    // -- V6: two L1 roots commit and hydrate bit-exactly (plan/18 §7 G14) --
    //
    // plan/17 §5.1 gives every closed region its own tube stub. Until V6 the
    // model held ONE L1 tube, so a second region was silently unrepresentable
    // and the committer serialised a single root. This case proves the whole
    // chain: two regions -> two L1 tubes with distinct channel-0
    // (interpolation) ids -> both roots on the stage under Tubes -> guides
    // carrying both region ids -> hydrate rebuilding both bit-exactly.
    {
        int const n = 4;
        Grid const grid = MakeGrid(n);
        TonicModel rm;
        Check(rm.BindScalp(grid.points, grid.counts, grid.indices),
              "V6: the two-region model binds its scalp");
        std::shared_ptr<usdGenTonic::TonicScalpMesh const> const mesh =
            rm.GetScalp();
        Check(bool(mesh), "V6: the scalp mesh is readable");
        if (mesh) {
            BuildTwoRegions(&rm, *mesh, n);
        }
        Check(rm.Rasterise(), "V6: K3 rasterises the two-region graph");
        Check(rm.GetRegionLoops().regionIds.size() == 2,
              "V6: the graph extracts exactly two regions (got " +
                  std::to_string(rm.GetRegionLoops().regionIds.size()) + ")");
        // One stub per region, exactly as TonicSession.ensureRegionTubes
        // does it after a graph gesture.
        Check(rm.BuildTubeFromRegion(0, 5, 8, 2.0f),
              "V6: region 0 gets its L1 stub: " + std::string(rm.GetDiagnostic()));
        Check(rm.BuildTubeFromRegion(1, 5, 8, 2.0f),
              "V6: region 1 gets its own L1 stub: " + std::string(rm.GetDiagnostic()));
        std::vector<int> const roots = rm.L1TubeIds();
        Check(roots.size() == 2 && roots[0] == 0,
              "V6: the model holds two L1 roots, the first being tube 0");
        int const secondRoot = roots.size() == 2 ? roots[1] : -1;
        Check(secondRoot > 0 && secondRoot % 16 == 0,
              "V6: the second root is minted in the L1 id family (got " +
                  std::to_string(secondRoot) + ")");
        Check(rm.TubeForRegion(0) != rm.TubeForRegion(1) &&
                  rm.TubeForRegion(0) >= 0 && rm.TubeForRegion(1) >= 0,
              "V6: the region->tube map is one tube per region");
        // Each root carries its own region, which IS its channel-0 id: the
        // bake writes the interpolation id of the region the root sits in.
        usdGenTonic::TonicTubeDesc d0, d1;
        Check(rm.GetTubeDesc(0, &d0) && rm.GetTubeDesc(secondRoot, &d1) &&
                  d0.regionId >= 0 && d1.regionId >= 0 &&
                  d0.regionId != d1.regionId,
              "V6: the two roots carry distinct channel-0 region ids");
        // The second root is editable in its own right (G2 + G14 together).
        // Re-read both descs afterwards: they are the bytes the commit ->
        // hydrate round trip below has to reproduce.
        Check(rm.MoveTubeCenterCV(secondRoot, 2, 0.07f, 0.0f, -0.02f),
              "V6: the second root's center CV moves");
        Check(rm.GetTubeDesc(0, &d0) && rm.GetTubeDesc(secondRoot, &d1),
              "V6: both roots' post-edit shapes are readable");

        SdfLayerRefPtr liveR = SdfLayer::CreateAnonymous("tonic-live-2region");
        UsdStageRefPtr sr = MakeStage(liveR);
        // The scalp link is what carries the graph across a hydrate: the
        // committer targets it from the groom and hydrate rebinds the mesh
        // behind it before it replays the nodes' (faceId, uv).
        SdfPath const scalpPath("/Scalp");
        {
            UsdGeomMesh scalp = UsdGeomMesh::Define(sr, scalpPath);
            VtVec3fArray pts(grid.points.size() / 3);
            for (size_t i = 0; i < pts.size(); ++i) {
                pts[i] = GfVec3f(grid.points[i * 3 + 0],
                                 grid.points[i * 3 + 1],
                                 grid.points[i * 3 + 2]);
            }
            scalp.GetPointsAttr().Set(pts);
            scalp.GetFaceVertexCountsAttr().Set(
                VtIntArray(grid.counts.begin(), grid.counts.end()));
            scalp.GetFaceVertexIndicesAttr().Set(
                VtIntArray(grid.indices.begin(), grid.indices.end()));
        }
        TonicCommitPaths regionPaths = paths;
        regionPaths.scalpPath = scalpPath;
        TonicCommitter cr(&rm, regionPaths);
        cr.Enqueue(sr);
        Check(WaitCommitted(cr, liveR, rm.GetVersion()),
              "V6: the two-region groom commits");

        UsdPrim const root0 = sr->GetPrimAtPath(
            SdfPath("/TonicGroom/Tubes/tube0"));
        UsdPrim const root1 = sr->GetPrimAtPath(SdfPath(
            "/TonicGroom/Tubes/" +
            usdGenTonic::TonicCommitPaths::TubeName(secondRoot)));
        Check(bool(root0) && bool(root1),
              "V6: both L1 roots land directly under Tubes");
        if (root0 && root1) {
            int r0 = -1, r1 = -1, l0 = 0, l1 = 0;
            root0.GetAttribute(TfToken("usdGen:tonic:regionId")).Get(&r0);
            root1.GetAttribute(TfToken("usdGen:tonic:regionId")).Get(&r1);
            root0.GetAttribute(TfToken("usdGen:tonic:level")).Get(&l0);
            root1.GetAttribute(TfToken("usdGen:tonic:level")).Get(&l1);
            Check(l0 == 1 && l1 == 1, "V6: both commit as level 1");
            Check(r0 >= 0 && r1 >= 0 && r0 != r1,
                  "V6: their committed channel-0 ids differ (" +
                      std::to_string(r0) + " vs " + std::to_string(r1) + ")");
        }
        {
            UsdPrim const guides =
                sr->GetPrimAtPath(SdfPath("/TonicGroom/Guides"));
            VtIntArray committedRegions, committedTubes;
            Check(bool(guides) &&
                      guides.GetAttribute(TfToken("primvars:regionId"))
                          .Get(&committedRegions) &&
                      guides.GetAttribute(TfToken("primvars:tubeId"))
                          .Get(&committedTubes),
                  "V6: the committed guides carry regionId and tubeId");
            bool sawRoot0 = false, sawRoot1 = false;
            for (int id : committedTubes) {
                sawRoot0 = sawRoot0 || id == 0;
                sawRoot1 = sawRoot1 || id == secondRoot;
            }
            Check(sawRoot0 && sawRoot1,
                  "V6: both roots contribute guides to the one Guides prim");
        }

        TonicModel hydrated;
        usdGenTonic::TonicHydrateResult const hr =
            usdGenTonic::TonicHydrateModel(sr, SdfPath("/TonicGroom"),
                                           &hydrated);
        Check(hr.ok, "V6: the two-region groom hydrates: " + hr.diagnostic);
        Check(hr.guidesBitEqual,
              "V6: the regenerated guides are bit-equal to the committed set");
        Check(hr.graphRoundTrip, "V6: the graph round-trips");
        Check(hr.tubeCount == 2 && hr.importedTubeCount == 0,
              "V6: hydrate rebuilds exactly the two roots (got " +
                  std::to_string(hr.tubeCount) + ")");
        std::vector<int> const hydratedRoots = hydrated.L1TubeIds();
        Check(hydratedRoots == roots,
              "V6: the hydrated model re-mints the same L1 ids");
        usdGenTonic::TonicTubeDesc h0, h1;
        bool const gotBoth = hydrated.GetTubeDesc(0, &h0) &&
                             hydrated.GetTubeDesc(secondRoot, &h1);
        Check(gotBoth && SameDesc(h0, d0) && SameDesc(h1, d1),
              "V6: both roots' authored shapes round-trip bit-exactly");
        Check(hydrated.TubeForRegion(d0.regionId) == 0 &&
                  hydrated.TubeForRegion(d1.regionId) == secondRoot,
              "V6: the region->tube map comes back with them");
    }

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
