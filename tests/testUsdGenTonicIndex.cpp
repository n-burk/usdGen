// testUsdGenTonicIndex — T1: the Tonic scene index publishes the active model.
//
// Plan/18 §2.6: the registry join and the per-level publication are proven
// here, before any Python touches them. No stage, no renderer: the input is
// an empty HdRetainedSceneIndex, so this is T1 with no plugin-path
// environment (the tonic library links directly).
//
// Proven here:
//   * the index attaches to TonicRegistry on construction and detaches on
//     destruction; it owns no model;
//   * with no active model it publishes the static test tube, and the first
//     Tonic_Activate removes it (PrimsRemoved) for good — deactivating does
//     not bring it back;
//   * an activated model publishes /__usdGenTonic/tubes/L1 plus its center
//     curves, CV dots, rings and ring CVs, with uniform primvars sized to
//     the real face/curve count (not the P3 single-element `constant`);
//   * Tonic_SubdivideTube adds the whole L2 family, and the L1 prims are
//     dirtied for topology while L2 arrives as PrimsAdded;
//   * a single-tube move re-tessellates exactly one tube and dirties
//     EXACTLY the point/extent leaves of the level it moved in — no
//     topology, no uniforms, no other level;
//   * an x-ray change dirties exactly the xray primvar of that level's
//     mesh, a visibility change exactly the visibility leaf of every prim
//     in the level, and a focus change exactly the overlay widths;
//   * the clump palette is the same table the scalp tint reads;
//   * input prims pass through GetPrim/GetChildPrimPaths untouched, and
//     input notices forward;
//   * V1: a selection change dirties EXACTLY the `selected` primvar of the
//     level mesh and its curves, and exactly displayColor on the CV dots,
//     re-tessellating nothing; a hover reads 2 where a selection reads 1;
//     a level selection lights every tube at that level and no other; and
//     the gizmo / brushRing prims appear when the model holds a record,
//     dirty only their own leaves when it moves, and are removed when it
//     is cleared;
//   * V8: every scalp face a region claims is painted the clumpColor of
//     the tube rooted in that region, the tint mesh is lifted off the
//     scalp it copies so the two cannot z-fight, and the overlay widths
//     are the section 2.4a pixel targets times the model's display scale
//     (with the pre-V8 radius-relative widths when no camera has set
//     one).

#include "usdGenTonic/imaging/tonicSceneIndex.h"
#include "usdGenTonic/tonicApi.h"
#include "usdGenTonic/tonicGizmo.h"
#include "usdGenTonic/tonicModel.h"
#include "usdGenTonic/tonicPublish.h"
#include "usdGenTonic/tonicSelection.h"
#include "usdGenTonic/tonicTransport.h"
#include "usdGenTonic/tonicRegistry.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/dataSourceTypeDefs.h"
#include "pxr/imaging/hd/materialBindingSchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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
    // Unbuffered: a crash in a later block must not swallow the line that
    // says how far the run got.
    std::fflush(stdout);
}

bool HasChild(HdSceneIndexBase const &index, SdfPath const &parent,
              SdfPath const &child)
{
    for (SdfPath const &candidate : index.GetChildPrimPaths(parent)) {
        if (candidate == child) {
            return true;
        }
    }
    return false;
}

HdSampledDataSourceHandle SampledAt(HdContainerDataSourceHandle const &root,
                                    HdDataSourceLocator const &locator)
{
    return HdSampledDataSource::Cast(
        HdContainerDataSource::Get(root, locator));
}

HdSampledDataSourceHandle Primvar(HdSceneIndexBase const &index,
                                  SdfPath const &path, char const *name)
{
    return SampledAt(index.GetPrim(path).dataSource,
                     HdDataSourceLocator(TfToken("primvars"), TfToken(name),
                                         TfToken("primvarValue")));
}

// The material a prim binds for allPurpose, or an empty path.
SdfPath MaterialBindingOf(HdSceneIndexBase const &index, SdfPath const &path)
{
    HdSampledDataSourceHandle ds = SampledAt(
        index.GetPrim(path).dataSource,
        HdDataSourceLocator(HdMaterialBindingsSchemaTokens->materialBindings,
                            HdMaterialBindingsSchemaTokens->allPurpose,
                            HdMaterialBindingSchemaTokens->path));
    if (!ds) {
        return SdfPath();
    }
    VtValue const v = ds->GetValue(0.0f);
    return v.IsHolding<SdfPath>() ? v.UncheckedGet<SdfPath>() : SdfPath();
}

template <class T>
bool ArraySize(HdSceneIndexBase const &index, SdfPath const &path,
               char const *name, size_t expected, T *out = nullptr)
{
    HdSampledDataSourceHandle ds = Primvar(index, path, name);
    if (!ds) {
        return false;
    }
    VtValue v = ds->GetValue(0.0f);
    if (!v.IsHolding<T>()) {
        return false;
    }
    T const &array = v.UncheckedGet<T>();
    if (out) {
        *out = array;
    }
    return array.size() == expected;
}

class RecordingObserver : public HdSceneIndexObserver {
public:
    void PrimsAdded(HdSceneIndexBase const &,
                    AddedPrimEntries const &entries) override
    {
        added.insert(added.end(), entries.begin(), entries.end());
    }
    void PrimsRemoved(HdSceneIndexBase const &,
                      RemovedPrimEntries const &entries) override
    {
        removed.insert(removed.end(), entries.begin(), entries.end());
    }
    void PrimsDirtied(HdSceneIndexBase const &,
                      DirtiedPrimEntries const &entries) override
    {
        dirtied.insert(dirtied.end(), entries.begin(), entries.end());
    }
    void PrimsRenamed(HdSceneIndexBase const &,
                      RenamedPrimEntries const &entries) override
    {
        renamed.insert(renamed.end(), entries.begin(), entries.end());
    }
    void Clear()
    {
        added.clear();
        removed.clear();
        dirtied.clear();
        renamed.clear();
    }
    bool WasAdded(SdfPath const &path) const
    {
        for (auto const &e : added) {
            if (e.primPath == path) {
                return true;
            }
        }
        return false;
    }
    bool WasRemoved(SdfPath const &path) const
    {
        for (auto const &e : removed) {
            if (e.primPath == path) {
                return true;
            }
        }
        return false;
    }
    // The locator set sent for `path`, or an empty set when it was not
    // dirtied. Used for exact-equality asserts, so a co-dirty fails loudly.
    HdDataSourceLocatorSet DirtiedFor(SdfPath const &path) const
    {
        HdDataSourceLocatorSet set;
        for (auto const &e : dirtied) {
            if (e.primPath == path) {
                set.insert(e.dirtyLocators);
            }
        }
        return set;
    }
    bool WasDirtied(SdfPath const &path) const
    {
        for (auto const &e : dirtied) {
            if (e.primPath == path) {
                return true;
            }
        }
        return false;
    }
    AddedPrimEntries added;
    RemovedPrimEntries removed;
    DirtiedPrimEntries dirtied;
    RenamedPrimEntries renamed;
};

size_t LocatorSetSize(HdDataSourceLocatorSet const &set)
{
    size_t n = 0;
    for (HdDataSourceLocator const &loc : set) {
        (void)loc;
        ++n;
    }
    return n;
}

std::string ToString(HdDataSourceLocatorSet const &set)
{
    std::string out = "{";
    bool first = true;
    for (HdDataSourceLocator const &loc : set) {
        if (!first) {
            out += ", ";
        }
        first = false;
        out += loc.GetString();
    }
    return out + "}";
}

HdDataSourceLocator Pv(char const *name)
{
    return HdDataSourceLocator(TfToken("primvars"), TfToken(name),
                               TfToken("primvarValue"));
}

HdDataSourceLocator const kExtentMin(TfToken("extent"), TfToken("min"));
HdDataSourceLocator const kExtentMax(TfToken("extent"), TfToken("max"));
HdDataSourceLocator const kVisibility(TfToken("visibility"),
                                      TfToken("visibility"));

} // namespace

int
main(int argc, char **argv)
{
    // USDGENTONIC_TEST_TUBE=0 (what record_usd.ps1 sets): the constructor
    // publishes nothing — no tube under the root, GetPrim empty. The ctest
    // entry testUsdGenTonicIndexNoTestTube drives this branch. The tool
    // path still works: create a model, activate it, and the level prims
    // appear.
    if (argc > 1 && std::string(argv[1]) == "--no-test-tube") {
        HdRetainedSceneIndexRefPtr input = HdRetainedSceneIndex::New();
        HdSceneIndexBaseRefPtr tonic = UsdGenTonicSceneIndex::New(input);
        SdfPath const root = UsdGenTonicSceneIndex::RootPath();
        SdfPath const tube = UsdGenTonicSceneIndex::TestTubePath();
        Check(!HasChild(*tonic, root, tube),
              "no-test-tube: testTube not announced under root");
        HdSceneIndexPrim tubePrim = tonic->GetPrim(tube);
        Check(tubePrim.primType.IsEmpty() && !tubePrim.dataSource,
              "no-test-tube: GetPrim(testTube) is empty");
        TonicModelContext *ctx = nullptr;
        Check(Tonic_Create(&ctx) == TONIC_OK && ctx,
              "no-test-tube: model creates");
        Check(Tonic_BuildTestTube(ctx, 0, 0, 0.0f, 0.0f) == TONIC_OK,
              "no-test-tube: tube builds");
        Check(Tonic_Activate(ctx) == TONIC_OK, "no-test-tube: activate");
        Check(HasChild(*tonic, UsdGenTonicSceneIndex::TubesScopePath(),
                       UsdGenTonicSceneIndex::TubesPath(1)),
              "no-test-tube: tubes/L1 announced after activate");
        Check(!HasChild(*tonic, root, tube),
              "no-test-tube: the test tube never appears");
        Tonic_Destroy(ctx);
        std::printf("%d failure(s)\n", g_failures);
        return g_failures ? 1 : 0;
    }

    usdGenTonic::TonicRegistry &registry = usdGenTonic::TonicRegistry::Get();
    SdfPath const root = UsdGenTonicSceneIndex::RootPath();
    SdfPath const testTube = UsdGenTonicSceneIndex::TestTubePath();
    SdfPath const tubesL1 = UsdGenTonicSceneIndex::TubesPath(1);
    SdfPath const tubesL2 = UsdGenTonicSceneIndex::TubesPath(2);
    SdfPath const centersL1 = UsdGenTonicSceneIndex::CentersPath(1);
    SdfPath const centerCVsL1 = UsdGenTonicSceneIndex::CenterCVsPath(1);
    SdfPath const ringsL1 = UsdGenTonicSceneIndex::RingsPath(1);
    SdfPath const ringCVsL1 = UsdGenTonicSceneIndex::RingCVsPath(1);
    SdfPath const centersL2 = UsdGenTonicSceneIndex::CentersPath(2);
    SdfPath const centerCVsL2 = UsdGenTonicSceneIndex::CenterCVsPath(2);
    SdfPath const ringsL2 = UsdGenTonicSceneIndex::RingsPath(2);
    SdfPath const ringCVsL2 = UsdGenTonicSceneIndex::RingCVsPath(2);

    Check(registry.IndexCount() == 0, "registry starts with no index");

    HdRetainedSceneIndexRefPtr input = HdRetainedSceneIndex::New();
    HdSceneIndexBaseRefPtr tonic = UsdGenTonicSceneIndex::New(input);
    UsdGenTonicSceneIndex *tonicRaw =
        dynamic_cast<UsdGenTonicSceneIndex *>(tonic.operator->());
    Check(tonicRaw != nullptr, "tonic index constructs over empty input");
    if (!tonicRaw) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    Check(registry.IndexCount() == 1,
          "the index attaches to the registry on construction");

    // -- the test tube, published while no model is active -----------------
    Check(root == SdfPath("/__usdGenTonic"), "root path is /__usdGenTonic");
    Check(HasChild(*tonic, SdfPath::AbsoluteRootPath(), root),
          "root announced under /");
    Check(HasChild(*tonic, root, testTube), "testTube announced under root");
    Check(tonicRaw->HasTestTube(), "the index reports the test tube");
    {
        HdSceneIndexPrim prim = tonic->GetPrim(testTube);
        Check(prim.primType == TfToken("mesh"), "test tube primType is mesh");
        VtVec3fArray points;
        Check(ArraySize(*tonic, testTube, "points", 40, &points),
              "test tube has 5 x 8 = 40 points");
        if (points.size() == 40) {
            // Ring 0, slot 0: angle 0 -> (0.5, 0, 0). Ring 4, slot 4
            // (index 4*8+4 = 36): angle pi -> (-0.5, 4, 0).
            Check(std::abs(points[0][0] - 0.5f) < 1e-6f &&
                      std::abs(points[0][1]) < 1e-6f &&
                      std::abs(points[0][2]) < 1e-6f,
                  "ring-0 slot-0 point is (0.5, 0, 0)");
            Check(std::abs(points[36][0] + 0.5f) < 1e-5f &&
                      std::abs(points[36][1] - 4.0f) < 1e-6f &&
                      std::abs(points[36][2]) < 1e-5f,
                  "ring-4 slot-4 point is (-0.5, 4, 0)");
        }
        Check(ArraySize<VtVec3fArray>(*tonic, testTube, "normals", 40),
              "test tube has 40 normals");
        // Uniform means per face: 32 entries, not the single-element
        // `constant` the P3 index published.
        Check(ArraySize<VtIntArray>(*tonic, testTube, "tubeId", 32),
              "test tube tubeId is uniform, one per face");
        Check(ArraySize<VtVec3fArray>(*tonic, testTube, "clumpColor", 32),
              "test tube clumpColor is uniform, one per face");
        Check(ArraySize<VtFloatArray>(*tonic, testTube, "selected", 32),
              "test tube selected is uniform, one per face");
        Check(ArraySize<VtIntArray>(*tonic, testTube, "hierarchyLevel", 1),
              "hierarchyLevel is constant");
        HdSampledDataSourceHandle counts =
            SampledAt(prim.dataSource,
                      HdDataSourceLocator(TfToken("mesh"),
                                          TfToken("topology"),
                                          TfToken("faceVertexCounts")));
        Check(counts && counts->GetValue(0.0f)
                            .UncheckedGet<VtIntArray>()
                            .size() == 32,
              "test tube has 32 quad faces");
    }
    Check(HasChild(*tonic, root, UsdGenTonicSceneIndex::TubeMaterialPath()),
          "tube material announced");
    Check(HasChild(*tonic, root, UsdGenTonicSceneIndex::OverlayMaterialPath()),
          "overlay material announced");
    Check(tonic->GetPrim(UsdGenTonicSceneIndex::OverlayMaterialPath())
                  .primType == TfToken("material"),
          "overlay material primType is material");

    // -- activate a model: the test tube goes, the levels arrive -----------
    HdSceneIndexObserverPtr observer(new RecordingObserver());
    tonic->AddObserver(observer);
    RecordingObserver *rec =
        static_cast<RecordingObserver *>(observer.operator->());

    TonicModelContext *ctx = nullptr;
    Check(Tonic_Create(&ctx) == TONIC_OK && ctx != nullptr, "model creates");
    if (!ctx) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    Check(Tonic_GetModelId(ctx) > 0, "the model registers and gets an id");
    Check(registry.ModelCount() == 1, "the registry holds one model");
    Check(Tonic_BuildTestTube(ctx, 0, 0, 0.0f, 0.0f) == TONIC_OK,
          "the model builds its tube");
    Check(Tonic_Activate(ctx) == TONIC_OK, "activate succeeds");
    Check(registry.GetActiveId() == Tonic_GetModelId(ctx),
          "the registry records the active model");
    Check(rec->WasRemoved(testTube),
          "activating removes the test tube (PrimsRemoved)");
    Check(!tonicRaw->HasTestTube(), "the test tube is gone");
    Check(rec->WasAdded(tubesL1), "activating adds tubes/L1");
    Check(rec->WasAdded(centersL1) && rec->WasAdded(centerCVsL1) &&
              rec->WasAdded(ringsL1) && rec->WasAdded(ringCVsL1),
          "activating adds the L1 overlays");

    // -- the L1 payload ----------------------------------------------------
    Check(tonic->GetPrim(tubesL1).primType == TfToken("mesh"),
          "tubes/L1 primType is mesh");
    Check(ArraySize<VtVec3fArray>(*tonic, tubesL1, "points", 40),
          "tubes/L1 carries the tube's 40 points");
    Check(ArraySize<VtIntArray>(*tonic, tubesL1, "tubeId", 32),
          "tubes/L1 tubeId is uniform, one per face");
    Check(ArraySize<VtVec3fArray>(*tonic, tubesL1, "clumpColor", 32),
          "tubes/L1 clumpColor is uniform, one per face");
    Check(ArraySize<VtFloatArray>(*tonic, tubesL1, "selected", 32),
          "tubes/L1 selected is uniform, one per face");
    {
        VtIntArray level;
        Check(ArraySize(*tonic, tubesL1, "hierarchyLevel", 1, &level) &&
                  level[0] == 1,
              "tubes/L1 hierarchyLevel is a constant 1");
        VtFloatArray xray;
        Check(ArraySize(*tonic, tubesL1, "xray", 1, &xray) &&
                  xray[0] == 0.0f,
              "tubes/L1 xray is a constant 0");
    }
    Check(tonic->GetPrim(centersL1).primType == TfToken("basisCurves"),
          "centers/L1 primType is basisCurves");
    Check(tonic->GetPrim(centerCVsL1).primType == TfToken("points"),
          "centerCVs/L1 primType is points");
    Check(ArraySize<VtVec3fArray>(*tonic, centersL1, "points", 5),
          "centers/L1 has the 5 center CVs");
    Check(ArraySize<VtVec3fArray>(*tonic, centersL1, "displayColor", 1),
          "centers/L1 displayColor is uniform, one per curve");
    Check(ArraySize<VtFloatArray>(*tonic, centersL1, "widths", 1),
          "centers/L1 widths is uniform, one per curve");
    Check(ArraySize<VtVec3fArray>(*tonic, centerCVsL1, "displayColor", 5),
          "centerCVs/L1 displayColor is per vertex");
    // -- V6: the ring visibility rule (plan/18 §2.4a) ----------------------
    //
    // Rings and ring CV dots draw in Tube mode's Ring/Section sub-modes and
    // on selected tubes everywhere else, so the default (Rings_Selected,
    // nothing selected) publishes no ring geometry at all.
    Check(Tonic_GetRingDisplay(ctx) == 1,
          "rings default to 'selected tubes only'");
    Check(ArraySize<VtVec3fArray>(*tonic, ringsL1, "points", 0),
          "rings/L1 is empty while nothing is selected");
    Check(ArraySize<VtVec3fArray>(*tonic, ringCVsL1, "points", 0),
          "ringCVs/L1 is empty while nothing is selected");
    {
        int const tubeZero = 0;
        Check(Tonic_SelectSet(ctx, usdGenTonic::TonicPick_TubeVert,
                              &tubeZero, nullptr, nullptr, 1) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "selecting the tube publishes its rings");
        Check(ArraySize<VtVec3fArray>(*tonic, ringsL1, "points", 45),
              "rings/L1 has 5 closed rings of 9 CVs once selected");
        Check(Tonic_SelectClear(ctx, 0) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1 &&
                  ArraySize<VtVec3fArray>(*tonic, ringsL1, "points", 0),
              "deselecting takes them away again");
    }
    // Ring mode shows every tube's rings; the rest of this test reads them,
    // so it stays on from here.
    Check(Tonic_SetRingDisplay(ctx, 2) == TONIC_OK &&
              Tonic_Publish(ctx, 0) == 1,
          "Ring sub-mode turns every tube's rings on");
    // 5 rings of 8 verts: 5 closed curves of 9 CVs, 40 ring CV dots.
    Check(ArraySize<VtVec3fArray>(*tonic, ringsL1, "points", 45),
          "rings/L1 has 5 closed rings of 9 CVs");
    Check(ArraySize<VtVec3fArray>(*tonic, ringCVsL1, "points", 40),
          "ringCVs/L1 has the 40 ring CVs");
    {
        int faces = 0, points = 0, tubes = 0;
        Check(Tonic_GetPublishedLevelInfo(ctx, 1, &faces, &points, &tubes) ==
                      TONIC_OK &&
                  faces == 32 && points == 40 && tubes == 1,
              "Tonic_GetPublishedLevelInfo reports L1 as 32/40/1");
        Check(Tonic_GetPublishedLevelInfo(ctx, 7, &faces, &points, &tubes) ==
                  TONIC_ERROR,
              "Tonic_GetPublishedLevelInfo fails for an unpublished level");
    }

    // -- a single-tube move: leaf-exact, one level, one restage ------------
    rec->Clear();
    Check(Tonic_MoveCenterRing(ctx, 2, 1.0f, 0.0f) == TONIC_OK,
          "MoveCenterRing(2, +1 X) applies");
    Check(Tonic_Publish(ctx, 0) == 1, "publish reaches the one index");
    Check(rec->added.empty() && rec->removed.empty(),
          "a move sends no added/removed");
    {
        HdDataSourceLocatorSet const expected{
            Pv("points"), Pv("normals"), kExtentMin, kExtentMax,
        };
        Check(rec->DirtiedFor(tubesL1) == expected,
              "the move dirties tubes/L1 leaf-exactly: " +
                  ToString(rec->DirtiedFor(tubesL1)));
        HdDataSourceLocatorSet const curveExpected{
            Pv("points"), kExtentMin, kExtentMax,
        };
        Check(rec->DirtiedFor(centersL1) == curveExpected,
              "the move dirties centers/L1 leaf-exactly: " +
                  ToString(rec->DirtiedFor(centersL1)));
        Check(rec->DirtiedFor(ringCVsL1) == curveExpected,
              "the move dirties ringCVs/L1 leaf-exactly");
    }
    Check(tonicRaw->LastRestagedTubeCount() == 1,
          "the move re-tessellated exactly one tube");
    {
        VtVec3fArray points;
        ArraySize(*tonic, tubesL1, "points", 40, &points);
        Check(points.size() == 40 &&
                  std::abs(points[16][0] - 1.5f) < 1e-5f &&
                  std::abs(points[16][1] - 2.0f) < 1e-6f,
              "ring-2 slot-0 point moved to (1.5, 2, 0)");
    }

    // -- subdivide: the whole L2 family arrives ----------------------------
    rec->Clear();
    int kids[4] = {0, 0, 0, 0};
    int kidCount = 0;
    Check(Tonic_SubdivideTube(ctx, 0, 4, "kmeans", 7, kids, 4, &kidCount) ==
                  TONIC_OK &&
              kidCount == 4,
          "Tonic_SubdivideTube splits tube 0 into four children");
    Check(Tonic_Publish(ctx, 0) == 1, "publish after subdivide");
    Check(rec->WasAdded(tubesL2), "subdivide adds tubes/L2");
    Check(rec->WasAdded(centersL2) && rec->WasAdded(centerCVsL2) &&
              rec->WasAdded(ringsL2) && rec->WasAdded(ringCVsL2),
          "subdivide adds the L2 overlays");
    {
        int faces = 0, points = 0, tubes = 0;
        Check(Tonic_GetPublishedLevelInfo(ctx, 2, &faces, &points, &tubes) ==
                      TONIC_OK &&
                  tubes == 4,
              "L2 publishes the four children in one mesh");
        // The staged grid is the model's own census: (sections - 1) *
        // displaySegments + 1 rows of ringVerts, per child.
        int expectFaces = 0;
        int expectPoints = 0;
        int const segments = Tonic_GetDisplaySegments(ctx);
        for (int i = 0; i < kidCount; ++i) {
            int const sections = Tonic_GetTubeSectionCount(ctx, kids[i]);
            float t = 0.0f, scale = 0.0f, twist = 0.0f;
            float uv[128] = {0.0f};
            int cvCount = 0;
            Tonic_GetTubeSection(ctx, kids[i], 0, &t, uv, 128, &cvCount,
                                 &scale, &twist);
            int const rings = (sections - 1) * segments + 1;
            expectFaces += (rings - 1) * cvCount;
            expectPoints += rings * cvCount;
        }
        Check(expectFaces > 0 && faces == expectFaces &&
                  points == expectPoints,
              "L2 stages every child's grid (want " +
                  std::to_string(expectFaces) + "/" +
                  std::to_string(expectPoints) + ", got " +
                  std::to_string(faces) + "/" + std::to_string(points) + ")");
        VtIntArray tubeIds;
        Check(ArraySize(*tonic, tubesL2, "tubeId", size_t(faces), &tubeIds),
              "tubes/L2 tubeId is uniform, one per face");
        bool distinct = false;
        for (size_t i = 1; i < tubeIds.size(); ++i) {
            distinct = distinct || tubeIds[i] != tubeIds[0];
        }
        Check(distinct, "tubes/L2 carries more than one tube id");
        VtIntArray level;
        Check(ArraySize(*tonic, tubesL2, "hierarchyLevel", 1, &level) &&
                  level[0] == 2,
              "tubes/L2 hierarchyLevel is a constant 2");
    }
    Check(ArraySize<VtVec3fArray>(*tonic, centersL2, "displayColor", 4),
          "centers/L2 has one colour per child curve");
    Check(tonicRaw->PublishedLevels() == std::vector<int>({1, 2}),
          "the index reports exactly the two published levels");

    // -- per-tube slice restage: moving tube 0 leaves L2 alone -------------
    rec->Clear();
    Check(Tonic_MoveCenterRing(ctx, 1, 0.25f, 0.0f) == TONIC_OK,
          "a second move applies");
    Check(Tonic_Publish(ctx, 0) == 1, "publish after the second move");
    Check(tonicRaw->LastRestagedTubeCount() == 1,
          "with five tubes staged, one move re-tessellates one tube");
    Check(rec->WasDirtied(tubesL1), "the second move dirties L1");
    Check(!rec->WasDirtied(tubesL2), "the second move leaves L2 clean");

    // -- display: x-ray, visibility and focus are separate leaves ----------
    rec->Clear();
    Check(Tonic_SetLevelDisplay(ctx, 2, 1, 1) == TONIC_OK, "L2 x-ray on");
    Check(Tonic_Publish(ctx, 0) == 1, "publish the x-ray change");
    {
        // V9: turning x-ray on also swaps the level onto the translucent
        // twin of the tube material — that is what stops the ghosted tube
        // writing depth over the curves inside it — so the binding is a
        // second leaf, and the only second leaf.
        HdDataSourceLocatorSet const expected{
            Pv("xray"), HdDataSourceLocator(TfToken("materialBindings"))};
        Check(rec->DirtiedFor(tubesL2) == expected,
              "x-ray dirties exactly the xray primvar and the binding: " +
                  ToString(rec->DirtiedFor(tubesL2)));
        Check(!rec->WasDirtied(centersL2),
              "x-ray does not touch the overlays");
        Check(!rec->WasDirtied(tubesL1), "x-ray does not touch L1");
    }
    {
        VtFloatArray xray;
        Check(ArraySize(*tonic, tubesL2, "xray", 1, &xray) &&
                  std::fabs(xray[0] -
                            usdGenTonic::TonicModel::kDefaultXrayOpacity) <
                      1e-6f,
              "tubes/L2 xray reads the level's alpha, not a 0/1 flag");
        Check(MaterialBindingOf(*tonic, tubesL2) ==
                  UsdGenTonicSceneIndex::TubeXrayMaterialPath(),
              "an x-rayed level binds the translucent tube material");
        Check(MaterialBindingOf(*tonic, tubesL1) ==
                  UsdGenTonicSceneIndex::TubeMaterialPath(),
              "an opaque level keeps the opaque tube material");
    }
    rec->Clear();
    Check(Tonic_SetLevelDisplay(ctx, 2, 0, 1) == TONIC_OK, "L2 hidden");
    Check(Tonic_Publish(ctx, 0) == 1, "publish the visibility change");
    {
        HdDataSourceLocatorSet const expected{kVisibility};
        Check(rec->DirtiedFor(tubesL2) == expected,
              "hiding a level dirties exactly visibility: " +
                  ToString(rec->DirtiedFor(tubesL2)));
        Check(rec->DirtiedFor(centersL2) == expected,
              "the level's curves dirty exactly visibility");
        Check(rec->DirtiedFor(ringCVsL2) == expected,
              "the level's CV dots dirty exactly visibility");
        Check(!rec->WasDirtied(tubesL1), "hiding L2 does not touch L1");
    }
    {
        int visible = 1, xray = 0;
        Check(Tonic_GetLevelDisplay(ctx, 2, &visible, &xray) == TONIC_OK &&
                  visible == 0 && xray == 1,
              "Tonic_GetLevelDisplay reads back hidden + x-ray");
        HdSampledDataSourceHandle vis =
            SampledAt(tonic->GetPrim(tubesL2).dataSource, kVisibility);
        Check(vis && !vis->GetValue(0.0f).UncheckedGet<bool>(),
              "tubes/L2 visibility is false");
    }
    rec->Clear();
    float unfocusedWidth = 0.0f;
    {
        VtFloatArray widths;
        ArraySize(*tonic, centersL1, "widths", 1, &widths);
        unfocusedWidth = widths.empty() ? 0.0f : widths[0];
    }
    Check(unfocusedWidth > 0.0f, "an unfocused center curve has a width");
    Check(Tonic_SetFocusLevel(ctx, 1) == TONIC_OK, "focus L1");
    Check(Tonic_GetFocusLevel(ctx) == 1, "focus level reads back");
    Check(Tonic_Publish(ctx, 0) == 1, "publish the focus change");
    {
        HdDataSourceLocatorSet const expected{Pv("widths")};
        Check(rec->DirtiedFor(centersL1) == expected,
              "focus dirties exactly the center widths: " +
                  ToString(rec->DirtiedFor(centersL1)));
        Check(rec->DirtiedFor(centerCVsL1) == expected,
              "focus dirties exactly the CV widths");
        Check(!rec->WasDirtied(tubesL1),
              "focus does not touch the tube mesh");
    }
    {
        VtFloatArray widths;
        ArraySize(*tonic, centersL1, "widths", 1, &widths);
        Check(widths.size() == 1 && widths[0] > unfocusedWidth,
              "the focused level's center curve is the thicker one");
    }
    Check(tonicRaw->LastRestagedTubeCount() == 0,
          "no display change re-tessellates anything");

    // -- selection drives the `selected` primvars, leaf-exactly ------------
    //
    // A click must not restage a vertex and must not dirty anything but the
    // one leaf that carries it: `selected` on the mesh and the curves,
    // displayColor on the CV dots (a dot shows its state as its colour).
    {
        SdfPath const gizmo = UsdGenTonicSceneIndex::GizmoPath();
        SdfPath const brush = UsdGenTonicSceneIndex::BrushRingPath();
        // L2 was left hidden + x-ray by the display block above; put it
        // back so the selection asserts read a normal level.
        Check(Tonic_SetLevelDisplay(ctx, 2, 1, 0) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "selection: L2 visible again");
        rec->Clear();
        int const tube0 = 0;
        Check(Tonic_SelectSet(ctx, usdGenTonic::TonicPick_TubeVert, &tube0,
                              nullptr, nullptr, 1) == TONIC_OK,
              "selection: select tube 0");
        Check(Tonic_Publish(ctx, 0) == 1, "selection: publish the click");
        Check(tonicRaw->LastRestagedTubeCount() == 0,
              "selection: a click re-tessellates nothing");
        {
            HdDataSourceLocatorSet const expected{Pv("selected")};
            Check(rec->DirtiedFor(tubesL1) == expected,
                  "selection dirties exactly the selected primvar: " +
                      ToString(rec->DirtiedFor(tubesL1)));
            Check(rec->DirtiedFor(centersL1) == expected,
                  "selection dirties exactly `selected` on the centers");
            Check(rec->DirtiedFor(ringsL1) == expected,
                  "selection dirties exactly `selected` on the rings");
        }
        {
            HdDataSourceLocatorSet const expected{Pv("displayColor")};
            Check(rec->DirtiedFor(centerCVsL1) == expected,
                  "selection dirties exactly the CV colours: " +
                      ToString(rec->DirtiedFor(centerCVsL1)));
            Check(rec->DirtiedFor(ringCVsL1) == expected,
                  "selection dirties exactly the ring CV colours");
        }
        Check(!rec->WasDirtied(tubesL2),
              "selecting an L1 tube leaves L2 alone");
        {
            VtFloatArray selected;
            Check(ArraySize(*tonic, tubesL1, "selected", 32, &selected) &&
                      selected[0] == 1.0f && selected[31] == 1.0f,
                  "tubes/L1 `selected` reads 1 on every face of the tube");
            // V9: float, like the mesh's. `selected` is one of the three
            // primvars tonicTube.glslfx declares, so its published type
            // has to be the one the shader reads on every prim that
            // binds the material, curves included.
            VtFloatArray curveSelected;
            Check(ArraySize(*tonic, centersL1, "selected", 1,
                            &curveSelected) &&
                      curveSelected[0] == 1.0f,
                  "centers/L1 `selected` reads 1 as a float");
        }
        // Hover is state 2, and it wins over selected on the same item.
        rec->Clear();
        Check(Tonic_SetHover(ctx, usdGenTonic::TonicPick_TubeVert, 0, -1,
                             -1) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "selection: hover the same tube");
        {
            VtFloatArray selected;
            Check(ArraySize(*tonic, tubesL1, "selected", 32, &selected) &&
                      selected[0] == 2.0f,
                  "a hovered tube reads 2, not 1");
            HdDataSourceLocatorSet const expected{Pv("selected")};
            Check(rec->DirtiedFor(tubesL1) == expected,
                  "hover dirties exactly the selected primvar");
        }
        Check(Tonic_SetHover(ctx, 0, -1, -1, -1) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "selection: clear the hover");
        // A center CV turns white; its neighbours keep the clump colour.
        rec->Clear();
        {
            int const ids[1] = {0};
            int const subs[1] = {2};
            Check(Tonic_SelectSet(ctx, usdGenTonic::TonicPick_CenterCV, ids,
                                  subs, nullptr, 1) == TONIC_OK &&
                      Tonic_Publish(ctx, 0) == 1,
                  "selection: select one center CV");
            VtVec3fArray colors;
            Check(ArraySize(*tonic, centerCVsL1, "displayColor", 5,
                            &colors) &&
                      colors[2] == GfVec3f(1.0f, 1.0f, 1.0f) &&
                      colors[1] != GfVec3f(1.0f, 1.0f, 1.0f),
                  "the selected CV dot is white and its neighbour is not");
        }
        // A level selection is every tube at that level and nothing else.
        rec->Clear();
        {
            Check(Tonic_SelectClear(ctx, 0) == TONIC_OK,
                  "selection: start the level test from nothing selected");
            int const level2[1] = {2};
            Check(Tonic_SelectSet(ctx, usdGenTonic::TonicPick_Level, level2,
                                  nullptr, nullptr, 1) == TONIC_OK &&
                      Tonic_Publish(ctx, 0) == 1,
                  "selection: select hierarchy level 2");
            int faces = 0;
            Check(Tonic_GetPublishedLevelInfo(ctx, 2, &faces, nullptr,
                                              nullptr) == TONIC_OK &&
                      faces > 0,
                  "selection: L2 reports its face count");
            VtFloatArray l2;
            Check(ArraySize(*tonic, tubesL2, "selected", size_t(faces),
                            &l2) && l2[0] == 1.0f &&
                      l2[size_t(faces) - 1] == 1.0f,
                  "every face of every L2 tube reads selected");
            VtFloatArray l1;
            Check(ArraySize(*tonic, tubesL1, "selected", 32, &l1) &&
                      l1[0] == 0.0f,
                  "the L1 tube is not selected by a level-2 selection");
        }
        Check(Tonic_SelectClear(ctx, 0) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "selection: clear everything");
        {
            VtFloatArray selected;
            Check(ArraySize(*tonic, tubesL1, "selected", 32, &selected) &&
                      selected[0] == 0.0f,
                  "clearing the selection puts `selected` back to 0");
        }

        // -- the gizmo and the brush ring appear and disappear -------------
        rec->Clear();
        Check(!HasChild(*tonic, root, gizmo),
              "no gizmo prim while the model holds no gizmo record");
        float const origin[3] = {0.0f, 1.0f, 0.0f};
        float const frame[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 1.0f};
        Check(Tonic_SetGizmo(ctx, 1, origin, frame, 1.0f, 1) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "publish a translate gizmo");
        Check(rec->WasAdded(gizmo), "the gizmo prim arrives");
        Check(tonic->GetPrim(gizmo).primType == TfToken("basisCurves"),
              "the gizmo is basisCurves");
        Check(ArraySize<VtVec3fArray>(*tonic, gizmo, "points", 6),
              "a translate gizmo is three two-point axes");
        {
            VtIntArray handles;
            VtIntArray active;
            Check(ArraySize(*tonic, gizmo, "handleId", 3, &handles) &&
                      handles[0] == 0 && handles[2] == 2,
                  "the gizmo carries one handle id per axis");
            Check(ArraySize(*tonic, gizmo, "active", 3, &active) &&
                      active[1] == 1 && active[0] == 0,
                  "the active handle is flagged");
        }
        // Moving it dirties its own leaves and nothing else.
        rec->Clear();
        float const moved[3] = {0.5f, 1.0f, 0.0f};
        Check(Tonic_SetGizmo(ctx, 1, moved, frame, 1.0f, 1) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "move the gizmo");
        {
            HdDataSourceLocatorSet const expected{
                Pv("points"),  Pv("displayColor"), Pv("widths"),
                Pv("active"),  kExtentMin,         kExtentMax,
            };
            Check(rec->DirtiedFor(gizmo) == expected,
                  "a gizmo move dirties exactly its own leaves: " +
                      ToString(rec->DirtiedFor(gizmo)));
            Check(!rec->WasDirtied(tubesL1),
                  "a gizmo move does not touch the geometry");
        }
        Check(tonicRaw->LastRestagedTubeCount() == 0,
              "a gizmo move re-tessellates nothing");
        // The brush ring is its own prim with its own life.
        rec->Clear();
        float const center[3] = {0.0f, 2.0f, 0.0f};
        float const normal[3] = {0.0f, 1.0f, 0.0f};
        Check(Tonic_SetBrushRing(ctx, center, normal, 0.4f) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "publish a brush ring");
        Check(rec->WasAdded(brush), "the brush ring prim arrives");
        Check(ArraySize<VtVec3fArray>(
                  *tonic, brush, "points",
                  size_t(usdGenTonic::TonicGizmoCircleSegments()) + 1),
              "the brush ring is one closed circle");
        Check(!rec->WasDirtied(gizmo),
              "setting the brush does not dirty the gizmo");
        rec->Clear();
        Check(Tonic_SetBrushRing(ctx, center, normal, 0.0f) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "clear the brush ring");
        Check(rec->WasRemoved(brush), "the brush ring prim goes away");
        Check(!HasChild(*tonic, root, brush),
              "the brush ring is no longer announced");
        rec->Clear();
        Check(Tonic_SetGizmo(ctx, 0, origin, frame, 1.0f, -1) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "clear the gizmo");
        Check(rec->WasRemoved(gizmo), "the gizmo prim goes away");
        Check(!HasChild(*tonic, root, gizmo),
              "the gizmo is no longer announced");
    }

    // -- notice builders are pure functions --------------------------------
    {
        Check(UsdGenTonicSceneIndex::NoticesFor(
                  usdGenTonic::TonicDirty_Clean)
                  .IsEmpty(),
              "NoticesFor(clean) is empty");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::NoticesFor(
                  usdGenTonic::TonicDirty_Points)) == 4,
              "NoticesFor(points) has 4 leaves");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::NoticesFor(
                  usdGenTonic::TonicDirty_Topology)) == 8,
              "NoticesFor(topology) has 8 leaves");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::NoticesFor(
                  UsdGenTonicSceneIndex::Dirty_Xray)) == 1,
              "NoticesFor(xray) has 1 leaf");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::CurveNoticesFor(
                  UsdGenTonicSceneIndex::Dirty_Widths)) == 1,
              "CurveNoticesFor(widths) has 1 leaf");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::PointNoticesFor(
                  usdGenTonic::TonicDirty_Points)) == 3,
              "PointNoticesFor(points) has 3 leaves");
        Check(UsdGenTonicSceneIndex::GuideNoticesFor(
                  usdGenTonic::TonicDirty_Points, false)
                  .IsEmpty(),
              "GuideNoticesFor(points) is empty (no guide co-dirty)");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::GuideNoticesFor(
                  usdGenTonic::TonicDirty_Guides, false)) == 5,
              "GuideNoticesFor(guides) has 5 leaves");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::GuideNoticesFor(
                  usdGenTonic::TonicDirty_Guides, true)) == 6,
              "GuideNoticesFor(guides, countChanged) has 6 leaves");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::NoticesFor(
                  usdGenTonic::TonicDirty_Selection)) == 1,
              "NoticesFor(selection) has 1 leaf");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::CurveNoticesFor(
                  usdGenTonic::TonicDirty_Selection)) == 1,
              "CurveNoticesFor(selection) has 1 leaf");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::PointNoticesFor(
                  usdGenTonic::TonicDirty_Selection)) == 1,
              "PointNoticesFor(selection) has 1 leaf");
        Check(UsdGenTonicSceneIndex::OverlayNoticesFor(
                  usdGenTonic::TonicDirty_Points, false)
                  .IsEmpty(),
              "OverlayNoticesFor(points) is empty (no overlay co-dirty)");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::OverlayNoticesFor(
                  usdGenTonic::TonicDirty_Gizmo, false)) == 6,
              "OverlayNoticesFor(gizmo) has 6 leaves");
        Check(LocatorSetSize(UsdGenTonicSceneIndex::OverlayNoticesFor(
                  usdGenTonic::TonicDirty_Gizmo, true)) == 8,
              "OverlayNoticesFor(gizmo, countChanged) has 8 leaves");
    }

    // -- the guide preview publishes per level -----------------------------
    {
        SdfPath const guides = UsdGenTonicSceneIndex::GuidesPath(1);
        Check(!HasChild(*tonic, UsdGenTonicSceneIndex::GuidesScopePath(),
                        guides),
              "guides/L1 absent before the first refill");
        Check(Tonic_SetFillParams(ctx, 16.0f, 8, 7, 0.0f, nullptr, 0) ==
                  TONIC_OK,
              "fill params set");
        Check(Tonic_RefillGuides(ctx, 1.0f) == TONIC_OK,
              "full-density refill runs");
        rec->Clear();
        Check(Tonic_Publish(ctx, 0) == 1, "publish the refill");
        Check(rec->WasAdded(guides), "the refill adds guides/L1");
        Check(tonic->GetPrim(guides).primType == TfToken("basisCurves"),
              "guides/L1 primType is basisCurves");
        Check(ArraySize<VtVec3fArray>(*tonic, guides, "points", 128),
              "guides/L1 has 16 guides x 8 CVs");
        Check(ArraySize<VtFloatArray>(*tonic, guides, "widths", 128),
              "guides/L1 has 128 widths");
        VtFloatArray hairT;
        Check(ArraySize(*tonic, guides, "hairT", 128, &hairT) &&
                  std::abs(hairT[0]) < 1e-6f &&
                  std::abs(hairT[7] - 1.0f) < 1e-6f,
              "guides/L1 hairT ramps 0 -> 1 per guide");
        Check(ArraySize<VtVec3fArray>(*tonic, guides, "displayColor", 16),
              "guides/L1 displayColor is uniform, one per curve");
        rec->Clear();
        Check(Tonic_RefillGuides(ctx, 1.0f) == TONIC_OK,
              "same-density refill runs");
        Check(Tonic_Publish(ctx, 0) == 1, "publish the same-count refill");
        HdDataSourceLocatorSet const expected{
            Pv("points"), Pv("widths"), Pv("hairT"), kExtentMin, kExtentMax,
        };
        Check(rec->DirtiedFor(guides) == expected,
              "a same-count refill dirties the guide leaves exactly: " +
                  ToString(rec->DirtiedFor(guides)));
    }

    // -- the clump palette is one table ------------------------------------
    {
        Check(usdGenTonic::TonicClumpPaletteSize() == 16,
              "the clump palette holds 16 entries");
        usdGenTonic::TonicRgb const a = usdGenTonic::TonicClumpColor(0, 1, -1);
        usdGenTonic::TonicRgb const b = usdGenTonic::TonicClumpColor(1, 1, -1);
        usdGenTonic::TonicRgb const wrapped =
            usdGenTonic::TonicClumpColor(16, 1, -1);
        Check(a.r != b.r || a.g != b.g || a.b != b.b,
              "neighbouring region ids take different palette slots");
        Check(a.r == wrapped.r && a.g == wrapped.g && a.b == wrapped.b,
              "region ids wrap every 16 slots");
        usdGenTonic::TonicRgb const child0 =
            usdGenTonic::TonicClumpColor(3, 2, 0);
        usdGenTonic::TonicRgb const child1 =
            usdGenTonic::TonicClumpColor(3, 2, 1);
        usdGenTonic::TonicRgb const parent =
            usdGenTonic::TonicClumpColor(3, 1, -1);
        Check(child0.r > parent.r && child1.r < parent.r,
              "children step lighter and darker around the parent hue");
    }

    // -- V9: the clump HUE FAMILY (plan/18 section 2.4a) -------------------
    //
    // "Children of one L1 tube take the parent's hue with +/- lightness
    // steps per childIndex so a lock stays one hue family." Two claims,
    // both checkable: siblings must be visibly different, and they must be
    // the same hue. The step V8 shipped (0.12/(level-1)) satisfied only the
    // first, and only on paper: +/-6 % at L3 is below the spread the
    // shader's own key/fill rig puts across one curved tube.
    {
        // Hue as the standard max/min formula, in [0, 6).
        auto hue = [](usdGenTonic::TonicRgb const &c) {
            float const mx = std::max(c.r, std::max(c.g, c.b));
            float const mn = std::min(c.r, std::min(c.g, c.b));
            float const d = mx - mn;
            if (d <= 0.0f) {
                return 0.0f;
            }
            float h = 0.0f;
            if (mx == c.r) {
                h = (c.g - c.b) / d;
            } else if (mx == c.g) {
                h = 2.0f + (c.b - c.r) / d;
            } else {
                h = 4.0f + (c.r - c.g) / d;
            }
            return h < 0.0f ? h + 6.0f : h;
        };
        auto luma = [](usdGenTonic::TonicRgb const &c) {
            return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        };
        bool sameHue = true;
        bool separated = true;
        bool withinBand = true;
        usdGenTonic::TonicRgb const base =
            usdGenTonic::TonicClumpColor(3, 1, -1);
        float const baseHue = hue(base);
        for (int level = 2; level <= 4; ++level) {
            for (int child = 0; child < 4; ++child) {
                usdGenTonic::TonicRgb const c =
                    usdGenTonic::TonicClumpColor(3, level, child);
                if (std::fabs(hue(c) - baseHue) > 1e-4f) {
                    sameHue = false;
                }
                // Every sibling pair has to separate; the neighbour one
                // index away is the hardest case.
                usdGenTonic::TonicRgb const other =
                    usdGenTonic::TonicClumpColor(3, level, child + 1);
                if (std::fabs(luma(c) - luma(other)) < 0.02f) {
                    separated = false;
                }
                if (std::fabs(luma(c) - luma(base)) > 0.45f) {
                    withinBand = false;
                }
            }
        }
        Check(sameHue, "siblings at every level keep their L1 ancestor's hue");
        Check(separated,
              "neighbouring siblings differ in luma by more than 2 %");
        Check(withinBand,
              "a sibling stays inside its ancestor's lightness band");
        Check(usdGenTonic::TonicClumpColor(3, 2, -1).r == base.r,
              "a tube with no childIndex is the plain palette entry");
    }

    // -- V9: THE display policy (plan/18 section 2.4a) ---------------------
    //
    // The table itself, asserted row by row. It is a pure function, so this
    // is a T0 inside the T1 binary: no model, no index, no publish.
    {
        using usdGenTonic::TonicModel;
        auto policy = [](char const *mode, char const *sub, int level,
                         int focus) {
            int const id = usdGenTonic::TonicDisplayModeFromNames(mode, sub);
            return usdGenTonic::TonicPolicyLevelDisplay(id, level, focus);
        };
        auto rings = [](char const *mode, char const *sub) {
            return usdGenTonic::TonicPolicyRingDisplay(
                usdGenTonic::TonicDisplayModeFromNames(mode, sub));
        };
        Check(usdGenTonic::TonicDisplayModeFromNames("nonsense", "") < 0,
              "an unknown mode is rejected, not guessed");
        Check(usdGenTonic::TonicDisplayModeFromNames("tube", "ring") ==
                      usdGenTonic::TonicDisplayMode_TubeRing &&
                  usdGenTonic::TonicDisplayModeFromNames("tube", "section") ==
                      usdGenTonic::TonicDisplayMode_TubeRing &&
                  usdGenTonic::TonicDisplayModeFromNames("tube", "center") ==
                      usdGenTonic::TonicDisplayMode_TubeCenter,
              "Ring and Section share Tube's ring row, Center does not");

        // Graph and Output: opaque tubes, no centers, no rings.
        for (char const *mode : {"graph", "output"}) {
            TonicModel::LevelDisplay const focused = policy(mode, "", 2, 2);
            TonicModel::LevelDisplay const other = policy(mode, "", 1, 2);
            Check(!focused.xray && !other.xray,
                  std::string(mode) + " keeps every level opaque");
            Check(!focused.centers && !other.centers,
                  std::string(mode) + " draws no centers or CV dots");
            Check(rings(mode, "") == TonicModel::Rings_Off,
                  std::string(mode) + " draws no rings");
        }

        // Tube / Ring: the focused level stays opaque (its rings lie on the
        // surface), everything behind it ghosts.
        {
            TonicModel::LevelDisplay const focused =
                policy("tube", "ring", 2, 2);
            TonicModel::LevelDisplay const other =
                policy("tube", "ring", 1, 2);
            Check(!focused.xray && focused.centers,
                  "Tube/Ring keeps the focused level opaque with centers");
            Check(other.xray &&
                      other.xrayOpacity == TonicModel::kDefaultXrayOpacity,
                  "Tube/Ring x-rays the other levels at 25 %");
            Check(rings("tube", "section") == TonicModel::Rings_All,
                  "Tube/Section shows every ring");
        }

        // Tube / Center, Hierarchy, Sculpt: the ladder of two x-ray
        // strengths, centers everywhere.
        char const *ladder[3][2] = {
            {"tube", "center"}, {"hierarchy", ""}, {"sculpt", ""}};
        for (auto const &pair : ladder) {
            TonicModel::LevelDisplay const focused =
                policy(pair[0], pair[1], 2, 2);
            TonicModel::LevelDisplay const other =
                policy(pair[0], pair[1], 1, 2);
            std::string const what = std::string(pair[0]) + "/" + pair[1];
            Check(focused.xray &&
                      focused.xrayOpacity == TonicModel::kDefaultXrayOpacity,
                  what + " x-rays the focused level at 25 %");
            Check(other.xray &&
                      other.xrayOpacity == TonicModel::kFaintXrayOpacity,
                  what + " x-rays the levels behind it at 10 %");
            Check(focused.centers && other.centers,
                  what + " draws the center curves and CV dots");
            Check(rings(pair[0], pair[1]) == TonicModel::Rings_Selected,
                  what + " rings follow the selection");
        }

        // Fill: every level at the same 25 %, so the guide preview reads
        // from root to tip across the whole groom.
        {
            TonicModel::LevelDisplay const focused = policy("fill", "", 2, 2);
            TonicModel::LevelDisplay const other = policy("fill", "", 1, 2);
            Check(focused.xray && other.xray &&
                      focused.xrayOpacity == other.xrayOpacity &&
                      other.xrayOpacity == TonicModel::kDefaultXrayOpacity,
                  "Fill x-rays every level at the same 25 %");
        }

        // No focus is not "everything is a ghost".
        {
            TonicModel::LevelDisplay const a = policy("sculpt", "", 1, 0);
            TonicModel::LevelDisplay const b = policy("sculpt", "", 3, 0);
            Check(a.xrayOpacity == TonicModel::kDefaultXrayOpacity &&
                      b.xrayOpacity == TonicModel::kDefaultXrayOpacity,
                  "with no focused level every level reads as the focused one");
        }
        Check(policy("graph", "", 1, 1).visible,
              "the policy never hides a level: visibility is the panel's");
    }

    // -- input passthrough --------------------------------------------------
    rec->Clear();
    {
        SdfPath const inputPath("/inputMesh");
        HdRetainedSceneIndex::AddedPrimEntries inputAdded = {
            {inputPath, TfToken("mesh"), HdRetainedContainerDataSource::New()},
        };
        input->AddPrims(inputAdded);
        Check(HasChild(*tonic, SdfPath::AbsoluteRootPath(), inputPath),
              "input prim passes through GetChildPrimPaths");
        Check(tonic->GetPrim(inputPath).primType == TfToken("mesh"),
              "input prim passes through GetPrim");
        Check(HasChild(*tonic, SdfPath::AbsoluteRootPath(), root),
              "tonic root still announced alongside input prims");
        Check(rec->added.size() == 1 && rec->added[0].primPath == inputPath,
              "input PrimsAdded forwards");
    }

    // -- V6 / G7: "show amplified hair" is a real switch -------------------
    //
    // plan/17 §3.2: while the cook's amplified tiles are up, the Tonic guide
    // preview steps aside; while a gesture is live the tiles are hidden
    // (they describe a groom older than the drag) and the preview comes
    // back. Before V6 the flag was written by the panel and read nowhere.
    rec->Clear();
    {
        SdfPath const guidesL1 = UsdGenTonicSceneIndex::GuidesPath(1);
        // A tile exactly where the groom scene index publishes them.
        SdfPath const tile("/Groom/Hair/__usdGenRender/tile0");
        input->AddPrims({{tile, TfToken("basisCurves"),
                          HdRetainedContainerDataSource::New()}});
        Check(tonicRaw->AmplifiedTileCount() == 1,
              "G7: the index remembers the amplified tile");

        auto tileVisible = [&]() {
            HdSampledDataSourceHandle vis =
                SampledAt(tonic->GetPrim(tile).dataSource, kVisibility);
            // No opinion at all means the groom index's own visibility
            // stands, which is "visible".
            return !vis || vis->GetValue(0.0f).UncheckedGet<bool>();
        };
        auto guidesVisible = [&]() {
            HdSampledDataSourceHandle vis =
                SampledAt(tonic->GetPrim(guidesL1).dataSource, kVisibility);
            return !vis || vis->GetValue(0.0f).UncheckedGet<bool>();
        };

        Check(Tonic_GetAmplifiedHair(ctx) == 0,
              "G7: amplified hair is off by default");
        Check(!tonicRaw->AmplifiedTilesVisible() && !tileVisible(),
              "G7: with it off the tiles are hidden");
        Check(guidesVisible(), "G7: and the guide preview draws");

        rec->Clear();
        Check(Tonic_SetAmplifiedHair(ctx, 1) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "G7: turning amplified hair on publishes");
        Check(tonicRaw->AmplifiedTilesVisible() && tileVisible(),
              "G7: the tiles come up");
        Check(!guidesVisible(),
              "G7: and the guide preview steps aside for them");
        Check(rec->WasDirtied(tile),
              "G7: the flip dirties the tile, not the whole scene");

        rec->Clear();
        Check(Tonic_BeginGesture(ctx, "drag") == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1,
              "G7: a gesture starts");
        Check(!tonicRaw->AmplifiedTilesVisible() && !tileVisible(),
              "G7: visibility=false goes over the tiles for the gesture");
        Check(guidesVisible(),
              "G7: and the guide preview is what the artist drags against");
        Check(Tonic_GetAmplifiedHair(ctx) == 1,
              "G7: the artist's own setting is untouched by the gesture");

        rec->Clear();
        Check(Tonic_EndGesture(ctx) == TONIC_OK && Tonic_Publish(ctx, 0) == 1,
              "G7: the gesture ends");
        Check(tonicRaw->AmplifiedTilesVisible() && tileVisible(),
              "G7: release restores the tiles");
        Check(!guidesVisible(), "G7: and hides the preview again");

        Check(Tonic_SetAmplifiedHair(ctx, 0) == TONIC_OK &&
                  Tonic_Publish(ctx, 0) == 1 && !tileVisible() &&
                  guidesVisible(),
              "G7: turning it back off restores the preview");
        input->RemovePrims({{tile}});
        Check(tonicRaw->AmplifiedTileCount() == 0,
              "G7: a removed tile is forgotten");
    }

    // -- V8: the scalp tint and the tube agree, and the overlays are
    // -- sized in pixels (plan/18 section 2.4a) ----------------------------
    //
    // A second model, because this one has a test tube and no scalp: the
    // tint only exists over a bound scalp with a rasterised region, and the
    // point of the check is that the region's patch on the head and the
    // tube rooted in that region come out of the palette at the same slot.
    {
        Check(Tonic_Deactivate(ctx) == TONIC_OK, "V8: park the first model");
        TonicModelContext *sc = nullptr;
        Check(Tonic_Create(&sc) == TONIC_OK && sc != nullptr,
              "V8: the scalp model creates");
        // The 4x4 quad grid in XZ the T3 scripts use: face f = ix * 4 + iz
        // covers x in [ix, ix+1] and z in [iz, iz+1].
        std::vector<float> points;
        for (int ix = 0; ix < 5; ++ix) {
            for (int iz = 0; iz < 5; ++iz) {
                points.push_back(float(ix));
                points.push_back(0.0f);
                points.push_back(float(iz));
            }
        }
        std::vector<int> counts(16, 4);
        std::vector<int> indices;
        for (int ix = 0; ix < 4; ++ix) {
            for (int iz = 0; iz < 4; ++iz) {
                int const a = ix * 5 + iz;
                indices.push_back(a);
                indices.push_back(a + 5);
                indices.push_back(a + 6);
                indices.push_back(a + 1);
            }
        }
        Check(Tonic_BindScalp(sc, points.data(), int(points.size()),
                              counts.data(), int(counts.size()),
                              indices.data(), int(indices.size())) ==
                  TONIC_OK,
              "V8: the 4x4 scalp binds");
        Tonic_SetSnapRadius(sc, 0.1f);
        float const corners[4][2] = {
            {0.0f, 1.0f}, {2.0f, 1.0f}, {2.0f, 3.0f}, {0.0f, 3.0f}};
        std::vector<int> faces;
        std::vector<float> uvs;
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k < 5; ++k) {
                float const t = float(k) / 5.0f;
                float const x = corners[c][0] +
                                (corners[(c + 1) % 4][0] - corners[c][0]) * t;
                float const z = corners[c][1] +
                                (corners[(c + 1) % 4][1] - corners[c][1]) * t;
                int const ix = std::min(std::max(int(std::floor(x)), 0), 3);
                int const iz = std::min(std::max(int(std::floor(z)), 0), 3);
                faces.push_back(ix * 4 + iz);
                uvs.push_back(z - float(iz));
                uvs.push_back(x - float(ix));
            }
        }
        faces.push_back(1);
        uvs.push_back(0.0f);
        uvs.push_back(0.0f);
        std::vector<int> chain(256);
        int chainCount = 0, closed = 0, weldStart = 0, weldEnd = 0;
        Check(Tonic_GraphStroke(sc, faces.data(), uvs.data(),
                                int(faces.size()), 0.1f, 0.05f, chain.data(),
                                int(chain.size()), &chainCount, &closed,
                                &weldStart, &weldEnd) == TONIC_OK &&
                  closed == 1,
              "V8: the stroke closes into a region");
        Check(Tonic_Rasterise(sc) == TONIC_OK, "V8: K3 rasterises it");
        Check(Tonic_BuildTubeFromRegion(sc, 0, 5, 8, 3.0f) == TONIC_OK,
              "V8: an L1 tube builds from region 0");
        Check(Tonic_Activate(sc) == TONIC_OK && Tonic_Publish(sc, ~0u) >= 1,
              "V8: the scalp model activates and publishes");

        SdfPath const regions = UsdGenTonicSceneIndex::GraphRegionsPath();
        SdfPath const nodes = UsdGenTonicSceneIndex::GraphNodesPath();
        VtVec3fArray faceColor;
        VtIntArray faceRegion;
        VtVec3fArray clump;
        Check(ArraySize(*tonic, regions, "displayColor", 16, &faceColor),
              "V8: graph/regions paints one colour per scalp face");
        Check(ArraySize(*tonic, regions, "usdGen:tonicRegion", 16,
                        &faceRegion),
              "V8: and carries the region primvar per face");
        {
            HdSampledDataSourceHandle ds = Primvar(*tonic, tubesL1,
                                                   "clumpColor");
            VtValue v = ds ? ds->GetValue(0.0f) : VtValue();
            Check(v.IsHolding<VtVec3fArray>(),
                  "V8: tubes/L1 carries a clumpColor");
            if (v.IsHolding<VtVec3fArray>()) {
                clump = v.UncheckedGet<VtVec3fArray>();
            }
        }
        // The one assertion this block exists for. Every face the region
        // claims must be painted the colour the tube rooted in that region
        // draws with: one palette, one index, no interp-id detour.
        int covered = 0;
        bool matched = !clump.empty();
        for (size_t f = 0; f < faceRegion.size() && f < faceColor.size();
             ++f) {
            if (faceRegion[f] < 0) {
                continue;
            }
            ++covered;
            if (!clump.empty() &&
                (faceColor[f] - clump[0]).GetLength() > 1e-6f) {
                matched = false;
            }
        }
        Check(covered > 0, "V8: the region claims scalp faces");
        Check(matched,
              "V8: every claimed face is painted the clumpColor of the tube "
              "rooted in its region");
        {
            // Uncovered faces keep their own warning colour, so the match
            // above is a statement about the region and not about the mesh
            // having one colour.
            bool warned = false;
            for (size_t f = 0; f < faceRegion.size() && f < faceColor.size();
                 ++f) {
                if (faceRegion[f] < 0 && !clump.empty() &&
                    (faceColor[f] - clump[0]).GetLength() > 1e-3f) {
                    warned = true;
                }
            }
            Check(warned, "V8: uncovered faces are not painted the clump "
                          "colour");
        }
        {
            // The tint mesh is lifted off the scalp it copies. Coincident
            // opaque meshes resolve by depth-buffer luck, and the luck ran
            // against the tint: the artist saw an untinted head.
            VtVec3fArray tintPoints;
            Check(ArraySize(*tonic, regions, "points", 25, &tintPoints),
                  "V8: graph/regions carries the scalp's 25 points");
            float lift = 0.0f;
            bool everyPointLifted = !tintPoints.empty();
            for (size_t i = 0; i < tintPoints.size(); ++i) {
                GfVec3f const source(points[i * 3 + 0], points[i * 3 + 1],
                                     points[i * 3 + 2]);
                float const d = (tintPoints[i] - source).GetLength();
                lift = std::max(lift, d);
                if (!(d > 0.0f)) {
                    everyPointLifted = false;
                }
            }
            Check(everyPointLifted,
                  "V8: no tint vertex is coincident with the scalp");
            // Small enough to be invisible as parallax (the 4x4 scalp's
            // diagonal is about 5.7 units), large enough to beat depth
            // precision by orders of magnitude.
            Check(lift > 1e-4f && lift < 0.05f,
                  "V8: and the lift stays sub-pixel at any sane framing");
        }

        // -- the display scale sizes the overlays in pixels ---------------
        SdfPath const scCenters = UsdGenTonicSceneIndex::CentersPath(1);
        SdfPath const scCenterCVs = UsdGenTonicSceneIndex::CenterCVsPath(1);
        SdfPath const scRingCVs = UsdGenTonicSceneIndex::RingCVsPath(1);
        auto firstWidth = [&](SdfPath const &path) -> float {
            HdSampledDataSourceHandle ds = Primvar(*tonic, path, "widths");
            if (!ds) {
                return -1.0f;
            }
            VtValue v = ds->GetValue(0.0f);
            if (!v.IsHolding<VtFloatArray>()) {
                return -1.0f;
            }
            VtFloatArray const &a = v.UncheckedGet<VtFloatArray>();
            return a.empty() ? -1.0f : a[0];
        };
        float const fallbackCV = firstWidth(scCenterCVs);
        Check(fallbackCV > 0.0f,
              "V8: with no camera the CV dots keep their radius-relative "
              "width");
        Check(Tonic_SetRingDisplay(sc, 2) == TONIC_OK &&
                  Tonic_Publish(sc, 0) >= 1,
              "V8: rings on, so the ring CV dots publish too");
        float const fallbackRingCV = firstWidth(scRingCVs);
        Check(fallbackRingCV > 0.0f, "V8: and so do their widths");

        float scale = 0.0f;
        Check(Tonic_GetDisplayScale(sc, &scale) == TONIC_OK && scale == 0.0f,
              "V8: a model with no camera reports display scale 0");
        rec->Clear();
        float const perPixel = 0.004f;
        Check(Tonic_SetDisplayScale(sc, perPixel) == TONIC_OK &&
                  Tonic_GetDisplayScale(sc, &scale) == TONIC_OK &&
                  scale == perPixel,
              "V8: Tonic_SetDisplayScale round trips");
        Check(Tonic_Publish(sc, 0) >= 1, "V8: the scale publishes");
        // The focused level is 0 here (nothing focused), so the "else"
        // column of the section 2.4a table is what these must equal.
        using Px = usdGenTonic::TonicOverlayPixels;
        Check(std::fabs(firstWidth(scCenterCVs) -
                        Px::kCenterCVOther * perPixel) < 1e-6f,
              "V8: centerCVs are 5 px wide at the display scale");
        Check(std::fabs(firstWidth(scRingCVs) - Px::kRingCV * perPixel) <
                  1e-6f,
              "V8: ringCVs are 5 px wide at the display scale");
        Check(std::fabs(firstWidth(scCenters) -
                        Px::kCenterCurveOther * perPixel) < 1e-6f,
              "V8: an unfocused level's center curves are 1 px");
        Check(std::fabs(firstWidth(nodes) - Px::kGraphNode * perPixel) <
                  1e-6f,
              "V8: graph node dots are 6 px");
        Check(rec->WasDirtied(scCenterCVs) && rec->WasDirtied(scCenters),
              "V8: a camera move dirties the overlay widths");
        Check(!rec->WasDirtied(tubesL1) ||
                  rec->DirtiedFor(tubesL1).Intersects(
                      HdDataSourceLocatorSet{Pv("points")}) == false,
              "V8: and re-tessellates nothing");

        Check(Tonic_SetFocusLevel(sc, 1) == TONIC_OK &&
                  Tonic_Publish(sc, 0) >= 1,
              "V8: focusing L1 republishes");
        Check(std::fabs(firstWidth(scCenterCVs) -
                        Px::kCenterCVFocused * perPixel) < 1e-6f,
              "V8: the focused level's CV dots step up to 8 px");
        Check(std::fabs(firstWidth(scCenters) -
                        Px::kCenterCurveFocused * perPixel) < 1e-6f,
              "V8: and its control curves to 3 px");

        // Double the scale (the artist dollies out), halve nothing else.
        Check(Tonic_SetDisplayScale(sc, perPixel * 2.0f) == TONIC_OK &&
                  Tonic_Publish(sc, 0) >= 1,
              "V8: doubling the scale republishes");
        Check(std::fabs(firstWidth(scCenterCVs) -
                        Px::kCenterCVFocused * perPixel * 2.0f) < 1e-6f,
              "V8: the world width follows the scale linearly");

        // Back to the unfocused state the fallback widths were read in:
        // the radius-relative column has its own focused/unfocused split,
        // so comparing across a focus change would prove nothing.
        Check(Tonic_SetFocusLevel(sc, 0) == TONIC_OK &&
                  Tonic_SetDisplayScale(sc, 0.0f) == TONIC_OK &&
                  Tonic_Publish(sc, 0) >= 1 &&
                  std::fabs(firstWidth(scRingCVs) - fallbackRingCV) < 1e-6f &&
                  std::fabs(firstWidth(scCenterCVs) - fallbackCV) < 1e-6f,
              "V8: clearing the scale restores the radius-relative widths");
        Check(Tonic_SetDisplayScale(sc, -1.0f) == TONIC_ERROR,
              "V8: a negative scale is refused");

        Check(Tonic_Deactivate(sc) == TONIC_OK && Tonic_Destroy(sc) ==
                  TONIC_OK,
              "V8: the scalp model destroys");
        Check(Tonic_Activate(ctx) == TONIC_OK && Tonic_Publish(ctx, ~0u) >= 1,
              "V8: the first model comes back for the teardown checks");
    }

    // -- V9 T1: the published prims follow the display policy -------------
    //
    // The table is proven pure above; this proves the wiring. One model,
    // two levels, one Tonic_SetDisplayPolicy per mode, and the assertion
    // is on what the INDEX published: the xray alpha, the material the
    // mesh binds, and whether the centers and CV dots are visible.
    {
        TonicModelContext *dc = nullptr;
        Check(Tonic_Create(&dc) == TONIC_OK, "V9: a policy model creates");
        Check(Tonic_BuildTestTube(dc, 0, 0, 0.0f, 0.0f) == TONIC_OK,
              "V9: the policy model builds its root tube");
        int dKids[2] = {0, 0};
        int dKidCount = 0;
        Check(Tonic_SubdivideTube(dc, 0, 2, "kmeans", 7, dKids, 2,
                                  &dKidCount) == TONIC_OK &&
                  dKidCount == 2,
              "V9: it subdivides into two children");
        Check(Tonic_Activate(dc) == TONIC_OK && Tonic_Publish(dc, ~0u) >= 1,
              "V9: the policy model activates and publishes");

        SdfPath const dTubes1 = UsdGenTonicSceneIndex::TubesPath(1);
        SdfPath const dTubes2 = UsdGenTonicSceneIndex::TubesPath(2);
        SdfPath const dCenters2 = UsdGenTonicSceneIndex::CentersPath(2);
        SdfPath const dCVs2 = UsdGenTonicSceneIndex::CenterCVsPath(2);
        auto xrayOf = [&](SdfPath const &path) -> float {
            VtFloatArray a;
            return ArraySize(*tonic, path, "xray", 1, &a) && !a.empty()
                       ? a[0]
                       : -1.0f;
        };
        auto visibleOf = [&](SdfPath const &path) -> bool {
            HdSampledDataSourceHandle ds = SampledAt(
                tonic->GetPrim(path).dataSource, kVisibility);
            if (!ds) {
                return false;
            }
            VtValue const v = ds->GetValue(0.0f);
            return v.IsHolding<bool>() && v.UncheckedGet<bool>();
        };

        // Graph: opaque tubes, no centers, no CV dots.
        Check(Tonic_SetDisplayPolicy(dc, "graph", "", 0) == TONIC_OK &&
                  Tonic_Publish(dc, 0) >= 1,
              "V9: the Graph policy applies and publishes");
        Check(xrayOf(dTubes1) == 0.0f && xrayOf(dTubes2) == 0.0f,
              "V9: Graph publishes xray 0 on every level");
        Check(MaterialBindingOf(*tonic, dTubes1) ==
                  UsdGenTonicSceneIndex::TubeMaterialPath(),
              "V9: and binds the opaque tube material");
        Check(!visibleOf(dCenters2) && !visibleOf(dCVs2),
              "V9: Graph hides the center curves and CV dots");

        // Hierarchy at L2 focus: focused level 25 %, the level behind 10 %,
        // centers back on, and the ghosted levels on the OIT material so
        // they stop writing depth over what is inside them.
        Check(Tonic_SetDisplayPolicy(dc, "hierarchy", "", 2) == TONIC_OK &&
                  Tonic_Publish(dc, 0) >= 1,
              "V9: the Hierarchy policy applies at L2 focus");
        Check(std::fabs(xrayOf(dTubes2) -
                        usdGenTonic::TonicModel::kDefaultXrayOpacity) < 1e-6f,
              "V9: the focused level publishes 25 % x-ray");
        Check(std::fabs(xrayOf(dTubes1) -
                        usdGenTonic::TonicModel::kFaintXrayOpacity) < 1e-6f,
              "V9: the level behind it publishes 10 %");
        Check(MaterialBindingOf(*tonic, dTubes1) ==
                      UsdGenTonicSceneIndex::TubeXrayMaterialPath() &&
                  MaterialBindingOf(*tonic, dTubes2) ==
                      UsdGenTonicSceneIndex::TubeXrayMaterialPath(),
              "V9: both x-rayed levels bind the translucent tube material");
        Check(visibleOf(dCenters2) && visibleOf(dCVs2),
              "V9: and the center curves and CV dots come back");
        {
            int focus = 0;
            focus = Tonic_GetFocusLevel(dc);
            Check(focus == 2, "V9: the policy sets the focus level too");
            Check(Tonic_GetRingDisplay(dc) == 1,
                  "V9: Hierarchy leaves the rings on the selection");
        }
        {
            float opacity = -1.0f;
            int centers = -1;
            Check(Tonic_GetLevelDraw(dc, 1, &opacity, &centers) == TONIC_OK &&
                      std::fabs(opacity -
                                usdGenTonic::TonicModel::kFaintXrayOpacity) <
                          1e-6f &&
                      centers == 1,
                  "V9: Tonic_GetLevelDraw reads the policy back");
        }

        // Tube / Ring: focused opaque with every ring, others ghosted.
        Check(Tonic_SetDisplayPolicy(dc, "tube", "ring", 2) == TONIC_OK &&
                  Tonic_Publish(dc, 0) >= 1,
              "V9: the Tube/Ring policy applies");
        Check(xrayOf(dTubes2) == 0.0f,
              "V9: Tube/Ring keeps the focused level opaque");
        Check(std::fabs(xrayOf(dTubes1) -
                        usdGenTonic::TonicModel::kDefaultXrayOpacity) < 1e-6f,
              "V9: and ghosts the level behind it at 25 %");
        Check(Tonic_GetRingDisplay(dc) == 2,
              "V9: Tube/Ring shows every ring");
        Check(Tonic_SetDisplayPolicy(dc, "nonsense", "", 0) == TONIC_ERROR,
              "V9: an unknown mode is an error and changes nothing");
        Check(xrayOf(dTubes2) == 0.0f,
              "V9: the rejected call left the published state alone");

        // -- the committed guides hide while the model is live -----------
        //
        // <groom>/Guides is last commit's curves, an ordinary BasisCurves
        // with no displayColor, so Storm draws it plain white over the
        // tubes. The index hides it in Hydra only.
        SdfPath const groom("/World/TonicGroom");
        SdfPath const committed = groom.AppendChild(TfToken("Guides"));
        input->AddPrims({{committed, TfToken("basisCurves"),
                          HdRetainedContainerDataSource::New()}});
        Check(UsdGenTonicSceneIndex::CommittedGuidesPath(
                  "/World/TonicGroom") == committed,
              "V9: the hidden path is <groom>/Guides");
        Check(UsdGenTonicSceneIndex::CommittedGuidesPath("").IsEmpty(),
              "V9: no groom path hides nothing");
        // The input publishes no visibility of its own, so "drawing" here
        // means "this index authors no opinion over it".
        auto hiddenOf = [&](SdfPath const &path) {
            HdSampledDataSourceHandle ds = SampledAt(
                tonic->GetPrim(path).dataSource, kVisibility);
            if (!ds) {
                return false;
            }
            VtValue const v = ds->GetValue(0.0f);
            return v.IsHolding<bool>() && !v.UncheckedGet<bool>();
        };
        Check(tonicRaw->HiddenGuidesPath().IsEmpty(),
              "V9: with no groom path set, nothing is hidden");
        Check(!hiddenOf(committed),
              "V9: and the committed guides draw");
        rec->Clear();
        Check(Tonic_SetGroomPath(dc, "/World/TonicGroom") == TONIC_OK &&
                  Tonic_Publish(dc, 0) >= 1,
              "V9: Tonic_SetGroomPath publishes");
        Check(tonicRaw->HiddenGuidesPath() == committed,
              "V9: the index now hides that prim");
        Check(hiddenOf(committed),
              "V9: the committed guides are invisible while the model lives");
        Check(rec->WasDirtied(committed),
              "V9: and their visibility was dirtied, not resynced");
        {
            char buf[64] = {0};
            Check(Tonic_GetGroomPath(dc, buf, int(sizeof(buf))) == TONIC_OK &&
                      std::string(buf) == "/World/TonicGroom",
                  "V9: Tonic_GetGroomPath round trips");
        }
        // Every other input prim is untouched by the override.
        SdfPath const bystander("/World/Scalp");
        input->AddPrims({{bystander, TfToken("mesh"),
                          HdRetainedContainerDataSource::New()}});
        Check(!hiddenOf(bystander) &&
                  tonic->GetPrim(bystander).primType == TfToken("mesh"),
              "V9: an unrelated input prim passes through unchanged");
        rec->Clear();
        Check(Tonic_Deactivate(dc) == TONIC_OK && Tonic_Publish(dc, 0) >= 0,
              "V9: the model deactivates");
        Check(tonicRaw->HiddenGuidesPath().IsEmpty(),
              "V9: deactivating stops hiding the committed guides");
        Check(!hiddenOf(committed),
              "V9: and they draw again");
        Check(Tonic_Destroy(dc) == TONIC_OK, "V9: the policy model destroys");
    }
    Check(Tonic_Activate(ctx) == TONIC_OK && Tonic_Publish(ctx, ~0u) >= 1,
          "V9: the first model comes back for the teardown checks");

    // -- deactivate: the levels go, the test tube does not return ----------
    rec->Clear();
    Check(Tonic_Deactivate(ctx) == TONIC_OK, "deactivate succeeds");
    Check(rec->WasRemoved(tubesL1) && rec->WasRemoved(tubesL2),
          "deactivating removes the level prims");
    Check(!rec->WasAdded(testTube) && !tonicRaw->HasTestTube(),
          "the test tube stays retired after deactivate");
    Check(registry.GetActiveId() == 0, "no model is active");

    tonic->RemoveObserver(observer);
    Check(Tonic_Destroy(ctx) == TONIC_OK, "model destroys");
    Check(registry.ModelCount() == 0, "destroying unregisters the model");

    // -- the index detaches on destruction ---------------------------------
    tonic = nullptr;
    Check(registry.IndexCount() == 0,
          "the index detaches from the registry on destruction");

    // -- V6 / G6: what the un-aliased VtArray fill actually costs ----------
    //
    // plan/17 §4.3 phase A promised VtArrays that ALIAS the pinned block, so
    // a publish would make no second copy. VtArray has no external-buffer
    // form (that was a Hydra 1.x HdVtBufferSource spelling), so a publish
    // memcpys the pinned block into the array Hydra retains. This measures
    // the copy that promise was about, at the plan/17 §7 reference tube
    // vertex count, so the decision to keep it is a number and not a guess.
    {
        size_t const verts = 770000;
        usdGenTonic::TonicPinnedStaging positions, normals;
        float *pinnedP = positions.Ensure(verts * 3);
        float *pinnedN = normals.Ensure(verts * 3);
        Check(pinnedP != nullptr && pinnedN != nullptr,
              "G6: the reference-scale pinned block allocates");
        if (pinnedP && pinnedN) {
            for (size_t i = 0; i < verts * 3; ++i) {
                pinnedP[i] = float(i) * 1e-5f;
                pinnedN[i] = 0.0f;
            }
            VtVec3fArray pts, nrm;
            auto const t0 = std::chrono::steady_clock::now();
            pts.resize(verts);
            nrm.resize(verts);
            std::memcpy(pts.data(), pinnedP, verts * 3 * sizeof(float));
            std::memcpy(nrm.data(), pinnedN, verts * 3 * sizeof(float));
            auto const t1 = std::chrono::steady_clock::now();
            double const ms =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::printf("info: G6 pinned -> VtArray at %zu verts "
                        "(pos + normals, %.1f MB): %.3f ms, pinned=%d\n",
                        verts, double(verts * 6 * sizeof(float)) / 1048576.0,
                        ms, int(positions.IsPinned()));
            Check(pts.size() == verts && nrm.size() == verts,
                  "G6: the staged arrays are the reference size");
            // The §7 per-move budget is 8 ms. The copy is charged against it
            // on every publish, so it has to be a small fraction of one.
            Check(ms < 8.0,
                  "G6: the retained-array copy fits inside one move budget");
        }
    }

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
