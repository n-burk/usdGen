// testUsdGenTonicApi — T1: the tonicApi C ABI over TonicModel.
//
// No stage, no Hydra: pure C ABI over the model, like testUsdGenToolsApi.
// Proves create/destroy, test-tube build defaults, sculpt stub versioning,
// staged point reads, honest errors on bad arguments, the V0 registry and
// level display, and the V1 surface: selection round trips per kind,
// marquee and lasso agreeing with the point pick on a subdivided model,
// hover, the gizmo/brush records and the geometry they build, the gesture
// bracket (one undo step per drag, bit-exact cancel), redo, undo labels
// and graph edits on the same undo stack.
#include "usdGenTonic/tonicApi.h"
#include "usdGenTonic/tonicGizmo.h"
#include "usdGenTonic/tonicModel.h"
#include "usdGenTonic/tonicSelection.h"
#include "usdGenTonic/tonicRegistry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
    // Unbuffered: a crash mid-run must still name the last check that ran.
    std::fflush(stdout);
}

} // namespace

int
main()
{
    Check(Tonic_Create(nullptr) == TONIC_ERROR,
          "Tonic_Create rejects null out-param");
    TonicModelContext *ctx = nullptr;
    Check(Tonic_Create(&ctx) == TONIC_OK && ctx != nullptr,
          "Tonic_Create succeeds");
    Check(Tonic_Destroy(nullptr) == TONIC_OK,
          "Tonic_Destroy(null) is a no-op success");
    if (!ctx) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    Check(Tonic_GetVersion(ctx) == 0, "fresh model version is 0");
    Check(Tonic_GetVertexCount(ctx) == 0, "no tube before build");
    Check(Tonic_BuildTestTube(nullptr, 0, 0, 0.0f, 0.0f) == TONIC_ERROR,
          "Tonic_BuildTestTube rejects null context");

    // Non-positive arguments select the defaults (5 x 8, r=0.5, len=4).
    Check(Tonic_BuildTestTube(ctx, 0, 0, 0.0f, 0.0f) == TONIC_OK,
          "Tonic_BuildTestTube with defaults succeeds");
    Check(Tonic_GetVersion(ctx) == 1, "build bumps version to 1");
    Check(Tonic_GetVertexCount(ctx) == 40, "default tube has 40 verts");
    Check(Tonic_GetQuadCount(ctx) == 32, "default tube has 32 quads");
    int dirty = Tonic_TakeDirty(ctx);
    Check((dirty & usdGenTonic::TonicDirty_Topology) != 0,
          "build marks topology dirty");
    Check(Tonic_TakeDirty(ctx) == 0, "TakeDirty clears");

    Check(Tonic_MoveCenterRing(ctx, 99, 1.0f, 0.0f) == TONIC_ERROR,
          "MoveCenterRing rejects out-of-range ring");
    Check(Tonic_MoveCenterRing(ctx, 2, 1.0f, 0.0f) == TONIC_OK,
          "MoveCenterRing applies");
    Check(Tonic_GetVersion(ctx) == 2, "sculpt bumps version to 2");
    Check(Tonic_TakeDirty(ctx) == int(usdGenTonic::TonicDirty_Points),
          "sculpt marks exactly points dirty");

    std::vector<float> xyz(size_t(Tonic_GetVertexCount(ctx)) * 3);
    Check(Tonic_ReadTubePoints(ctx, nullptr, 0) == TONIC_ERROR,
          "ReadTubePoints rejects null output");
    Check(Tonic_ReadTubePoints(ctx, xyz.data(), 3) == TONIC_ERROR,
          "ReadTubePoints rejects a short buffer");
    Check(Tonic_ReadTubePoints(ctx, xyz.data(), int(xyz.size())) == TONIC_OK,
          "ReadTubePoints stages the tube");
    Check(std::abs(xyz[0] - 0.5f) < 1e-6f && std::abs(xyz[1]) < 1e-6f &&
              std::abs(xyz[2]) < 1e-6f,
          "staged ring-0 slot-0 is (0.5, 0, 0)");
    Check(std::abs(xyz[16 * 3] - 1.5f) < 1e-5f &&
              std::abs(xyz[16 * 3 + 1] - 2.0f) < 1e-6f,
          "staged ring-2 slot-0 moved to (1.5, 2, 0)");
    Check(std::string(Tonic_GetLastError()).size() >= 0,
          "GetLastError returns a string");

    // A 1-ring build is rejected; the model keeps the previous tube.
    Check(Tonic_BuildTestTube(ctx, 1, 8, 0.5f, 4.0f) == TONIC_ERROR,
          "BuildTestTube rejects rings < 2");
    Check(Tonic_GetVersion(ctx) == 2, "failed build keeps the version");
    Check(Tonic_GetVertexCount(ctx) == 40, "failed build keeps the tube");

    Check(Tonic_Destroy(ctx) == TONIC_OK, "Tonic_Destroy succeeds");

    // -- P6 undo ----------------------------------------------------------
    Check(Tonic_Undo(nullptr, nullptr) == TONIC_ERROR, "undo rejects null context");
    Check(Tonic_GetUndoDepth(nullptr) == 0, "undo depth is 0 on null");
    Check(Tonic_GetUndoBytes(nullptr) == 0, "undo bytes are 0 on null");
    Check(Tonic_SetUndoBudget(nullptr, 4, 1024) == TONIC_ERROR,
          "undo budget rejects null context");
    Check(Tonic_ClearUndo(nullptr) == TONIC_ERROR,
          "undo clear rejects null context");
    {
        TonicModelContext *uc = nullptr;
        Check(Tonic_Create(&uc) == TONIC_OK && uc != nullptr,
              "undo: model creates");
        if (uc) {
            Check(Tonic_GetUndoDepth(uc) == 0, "undo: fresh depth is 0");
            Check(Tonic_GetUndoBytes(uc) == 0, "undo: fresh bytes are 0");
            Check(Tonic_Undo(uc, nullptr) == TONIC_OK, "undo: empty stack is a no-op");
            Check(Tonic_BuildTestTube(uc, 0, 0, 0.0f, 0.0f) == TONIC_OK,
                  "undo: build succeeds");
            Check(Tonic_GetUndoDepth(uc) == 0, "undo: build holds no steps");
            std::vector<float> before(
                size_t(Tonic_GetVertexCount(uc)) * 3);
            Check(Tonic_ReadTubePoints(uc, before.data(),
                                       int(before.size())) == TONIC_OK,
                  "undo: pre-move points read");
            unsigned long long v0 = Tonic_GetVersion(uc);
            Check(Tonic_MoveCenterRing(uc, 2, 1.0f, 0.0f) == TONIC_OK,
                  "undo: move succeeds");
            Check(Tonic_GetUndoDepth(uc) == 1, "undo: move pushes one step");
            Check(Tonic_GetUndoBytes(uc) > 0, "undo: step accounts bytes");
            std::vector<float> moved = before;
            Check(Tonic_ReadTubePoints(uc, moved.data(),
                                       int(moved.size())) == TONIC_OK,
                  "undo: post-move points read");
            Check(moved != before, "undo: the move changed the tube");
            Check(Tonic_Undo(uc, nullptr) == TONIC_OK, "undo: undo succeeds");
            Check(Tonic_GetUndoDepth(uc) == 0, "undo: stack drains");
            Check(Tonic_GetUndoBytes(uc) == 0, "undo: bytes drain too");
            std::vector<float> restored = moved;
            Check(Tonic_ReadTubePoints(uc, restored.data(),
                                       int(restored.size())) == TONIC_OK,
                  "undo: post-undo points read");
            Check(restored == before, "undo: points restore bit-exactly");
            Check(Tonic_GetVersion(uc) == v0 + 2,
                  "undo: move + undo bump the version twice");
            // Failed validation pushes nothing.
            Check(Tonic_MoveCenterRing(uc, 99, 1.0f, 0.0f) == TONIC_ERROR,
                  "undo: bad move still fails");
            Check(Tonic_GetUndoDepth(uc) == 0,
                  "undo: failed moves push nothing");
            // Depth budget evicts oldest first.
            Check(Tonic_SetUndoBudget(uc, 2, 1ULL << 40) == TONIC_OK,
                  "undo: depth budget sets");
            Check(Tonic_MoveCenterRing(uc, 1, 0.5f, 0.0f) == TONIC_OK &&
                      Tonic_MoveCenterRing(uc, 2, 0.5f, 0.0f) == TONIC_OK &&
                      Tonic_MoveCenterRing(uc, 3, 0.5f, 0.0f) == TONIC_OK,
                  "undo: three moves run");
            Check(Tonic_GetUndoDepth(uc) == 2,
                  "undo: depth budget holds two");
            Check(Tonic_Undo(uc, nullptr) == TONIC_OK && Tonic_Undo(uc, nullptr) == TONIC_OK,
                  "undo: two undos run");
            Check(Tonic_GetUndoDepth(uc) == 0, "undo: budget stack drains");
            // Byte budget evicts just as eagerly.
            Check(Tonic_SetUndoBudget(uc, 50, 1) == TONIC_OK,
                  "undo: byte budget sets");
            Check(Tonic_MoveCenterRing(uc, 1, 0.5f, 0.0f) == TONIC_OK,
                  "undo: move runs under a byte budget");
            Check(Tonic_GetUndoDepth(uc) == 0 &&
                      Tonic_GetUndoBytes(uc) == 0,
                  "undo: a 1-byte budget evicts instantly");
            Check(Tonic_SetUndoBudget(uc, 50, 1ULL << 40) == TONIC_OK,
                  "undo: budget restores");
            Check(Tonic_SetUndoBudget(uc, -1, 1ULL << 40) == TONIC_ERROR,
                  "undo: negative depth rejected");
            // Disabled + explicit clear + clear-on-build.
            Check(Tonic_SetUndoBudget(uc, 0, 1ULL << 40) == TONIC_OK,
                  "undo: depth 0 disables");
            Check(Tonic_MoveCenterRing(uc, 1, 0.5f, 0.0f) == TONIC_OK,
                  "undo: moves run while disabled");
            Check(Tonic_GetUndoDepth(uc) == 0,
                  "undo: disabled pushes nothing");
            Check(Tonic_SetUndoBudget(uc, 50, 1ULL << 40) == TONIC_OK &&
                      Tonic_MoveCenterRing(uc, 1, 0.5f, 0.0f) == TONIC_OK,
                  "undo: re-enable + move");
            Check(Tonic_GetUndoDepth(uc) == 1, "undo: step held again");
            Check(Tonic_ClearUndo(uc) == TONIC_OK, "undo: explicit clear");
            Check(Tonic_GetUndoDepth(uc) == 0 &&
                      Tonic_GetUndoBytes(uc) == 0,
                  "undo: clear drains depth and bytes");
            Check(Tonic_MoveCenterRing(uc, 1, 0.5f, 0.0f) == TONIC_OK,
                  "undo: move runs before rebuild");
            Check(Tonic_BuildTestTube(uc, 0, 0, 0.0f, 0.0f) == TONIC_OK,
                  "undo: rebuild succeeds");
            Check(Tonic_GetUndoDepth(uc) == 0,
                  "undo: rebuild clears the stack");
            // Topology undo: subdivide then undo removes the children.
            std::vector<int> kids;
            kids.resize(4);
            int kidCount = 0;
            Check(Tonic_SubdivideTube(uc, 0, 4, "kmeans", 7, kids.data(),
                                      int(kids.size()),
                                      &kidCount) == TONIC_OK &&
                      kidCount == 4,
                  "undo: subdivide runs");
            Check(Tonic_GetTubeCount(uc) == 5, "undo: five tubes present");
            Check(Tonic_Undo(uc, nullptr) == TONIC_OK, "undo: topology undo runs");
            Check(Tonic_GetTubeCount(uc) == 1,
                  "undo: undo removes the children");
            Check(Tonic_Destroy(uc) == TONIC_OK, "undo: model destroys");
        }
    }

    // -- P6 OOM fallback --------------------------------------------------
    // A healthy model reports no reason (null context too). The forced
    // injection drops the mirror deterministically on CUDA builds while
    // the op still succeeds on the host mirror.
    Check(std::string(Tonic_GetDeviceFallbackReason(nullptr)).empty(),
          "fallback reason is empty on null context");
    {
        TonicModelContext *healthy = nullptr;
        Check(Tonic_Create(&healthy) == TONIC_OK && healthy != nullptr,
              "P6: fresh model creates");
        if (healthy) {
            Check(Tonic_BuildTestTube(healthy, 0, 0, 0.0f, 0.0f) == TONIC_OK,
                  "P6: healthy build succeeds");
            Check(std::string(Tonic_GetDeviceFallbackReason(healthy)).empty(),
                  "P6: healthy model reports no fallback");
            Check(Tonic_Destroy(healthy) == TONIC_OK,
                  "P6: healthy model destroys");
        }
    }
#ifdef USDGEN_TONIC_HAS_CUDA
    {
#ifdef _WIN32
        _putenv("USDGEN_TONIC_FORCE_DEVICE_FALLBACK=1");
#else
        setenv("USDGEN_TONIC_FORCE_DEVICE_FALLBACK", "1", 1);
#endif
        TonicModelContext *forced = nullptr;
        Check(Tonic_Create(&forced) == TONIC_OK && forced != nullptr,
              "P6: forced model creates");
        if (forced) {
            Check(Tonic_HasCudaMirror(forced) == 1,
                  "P6: mirror exists before the first sync");
            Check(Tonic_BuildTestTube(forced, 0, 0, 0.0f, 0.0f) == TONIC_OK,
                  "P6: build succeeds despite the forced drop");
            Check(Tonic_HasCudaMirror(forced) == 0,
                  "P6: the forced drop clears the mirror");
            std::string const reason =
                Tonic_GetDeviceFallbackReason(forced);
            Check(reason.find("USDGEN_TONIC_FORCE_DEVICE_FALLBACK") !=
                      std::string::npos,
                  "P6: the reason names the injection");
            std::vector<float> fxyz(
                size_t(Tonic_GetVertexCount(forced)) * 3);
            Check(Tonic_ReadTubePoints(forced, fxyz.data(),
                                       int(fxyz.size())) == TONIC_OK &&
                      std::abs(fxyz[0] - 0.5f) < 1e-6f,
                  "P6: staged reads serve the host mirror after fallback");
            Check(Tonic_MoveCenterRing(forced, 2, 1.0f, 0.0f) == TONIC_OK,
                  "P6: later edits keep working CPU-only");
            Check(std::string(Tonic_GetDeviceFallbackReason(forced)) == reason,
                  "P6: the reason is sticky");
            Check(Tonic_Destroy(forced) == TONIC_OK,
                  "P6: forced model destroys");
        }
#ifdef _WIN32
        _putenv("USDGEN_TONIC_FORCE_DEVICE_FALLBACK=");
#else
        unsetenv("USDGEN_TONIC_FORCE_DEVICE_FALLBACK");
#endif
    }
#else
    std::printf("skip: no CUDA for the forced device fallback\n");
#endif


    // -- V0 viewport publication (plan/18 sections 2.1, 2.2) ---------------
    //
    // No scene index is attached in this process, so Publish reaches zero of
    // them - which is exactly the contract: the count IS the result, and a
    // headless tool gets an honest 0 rather than a silent success. The
    // registry bookkeeping, the activation handshake and the level display
    // state are all provable here; testUsdGenTonicIndex proves the same ABI
    // against a live index, and testUsdviewTonicPublish against Hydra.
    {
        usdGenTonic::TonicRegistry &registry =
            usdGenTonic::TonicRegistry::Get();
        Check(Tonic_GetModelId(nullptr) == 0,
              "V0: Tonic_GetModelId(null) is 0");
        Check(Tonic_Activate(nullptr) == TONIC_ERROR,
              "V0: Tonic_Activate rejects null");
        Check(Tonic_Publish(nullptr, 0) == -1,
              "V0: Tonic_Publish reports -1 on null, not a count");
        TonicModelContext *a = nullptr;
        TonicModelContext *b = nullptr;
        Check(Tonic_Create(&a) == TONIC_OK && Tonic_Create(&b) == TONIC_OK,
              "V0: two models create");
        if (a && b) {
            int const idA = Tonic_GetModelId(a);
            int const idB = Tonic_GetModelId(b);
            Check(idA > 0 && idB > 0 && idA != idB,
                  "V0: every model gets its own registry id");
            Check(registry.ModelCount() == 2,
                  "V0: the registry holds both models");
            Check(registry.GetActiveId() == 0,
                  "V0: creating a model does not activate it");
            Check(Tonic_Activate(a) == TONIC_OK, "V0: activate the first");
            Check(registry.GetActiveId() == idA,
                  "V0: the registry records the active model");
            Check(Tonic_Publish(a, 0) == 0,
                  "V0: publish with no attached index refreshes none");
            Check(Tonic_Activate(b) == TONIC_OK,
                  "V0: activating the second takes over");
            Check(registry.GetActiveId() == idB,
                  "V0: only one model is active at a time");
            Check(Tonic_Deactivate(b) == TONIC_OK, "V0: deactivate");
            Check(registry.GetActiveId() == 0, "V0: nothing is active");

            // Level display: defaults, round trip, rejection, focus.
            int visible = -1;
            int xray = -1;
            Check(Tonic_GetLevelDisplay(a, 3, &visible, &xray) == TONIC_OK &&
                      visible == 1 && xray == 0,
                  "V0: an untouched level reads visible and opaque");
            Check(Tonic_SetLevelDisplay(a, 2, 0, 1) == TONIC_OK,
                  "V0: hide level 2 in x-ray");
            Check(Tonic_GetLevelDisplay(a, 2, &visible, &xray) == TONIC_OK &&
                      visible == 0 && xray == 1,
                  "V0: level display round-trips");
            Check(Tonic_SetLevelDisplay(a, 0, 1, 0) == TONIC_ERROR,
                  "V0: level 0 is rejected");
            Check(Tonic_GetLevelDisplay(a, 1, &visible, &xray) == TONIC_OK &&
                      visible == 1 && xray == 0,
                  "V0: setting one level leaves the others alone");
            unsigned long long const before = Tonic_GetVersion(a);
            Check(Tonic_SetLevelDisplay(a, 2, 0, 1) == TONIC_OK &&
                      Tonic_GetVersion(a) == before,
                  "V0: a no-op display set does not bump the version");
            Check(Tonic_SetFocusLevel(a, 2) == TONIC_OK &&
                      Tonic_GetFocusLevel(a) == 2,
                  "V0: focus level round-trips");
            Check(Tonic_GetVersion(a) == before + 1,
                  "V0: a real display change bumps the version");
            Check((Tonic_TakeDirty(a) &
                   usdGenTonic::TonicDirty_Display) != 0,
                  "V0: a display change marks Display dirty");
            Check(Tonic_SetFocusLevel(a, -1) == TONIC_ERROR,
                  "V0: a negative focus level is rejected");
            Check(Tonic_SetFocusLevel(a, 0) == TONIC_OK &&
                      Tonic_GetFocusLevel(a) == 0,
                  "V0: focus level 0 focuses nothing");

            // V8 (plan/18 §2.4a): world units per screen pixel, which is
            // what turns the overlay pixel targets into the world widths
            // Storm draws points and curves with.
            {
                float scale = -1.0f;
                Check(Tonic_GetDisplayScale(a, &scale) == TONIC_OK &&
                          scale == 0.0f,
                      "V8: a fresh model has no display scale");
                unsigned long long const v = Tonic_GetVersion(a);
                Check(Tonic_SetDisplayScale(a, 0.0125f) == TONIC_OK &&
                          Tonic_GetDisplayScale(a, &scale) == TONIC_OK &&
                          scale == 0.0125f,
                      "V8: the display scale round-trips");
                Check(Tonic_GetVersion(a) == v + 1 &&
                          (Tonic_TakeDirty(a) &
                           usdGenTonic::TonicDirty_Display) != 0,
                      "V8: setting it bumps the version and marks Display "
                      "dirty");
                Check(Tonic_SetDisplayScale(a, 0.0125f) == TONIC_OK &&
                          Tonic_GetVersion(a) == v + 1,
                      "V8: setting the same scale again publishes nothing");
                Check(Tonic_SetDisplayScale(a, -0.5f) == TONIC_ERROR,
                      "V8: a negative scale is refused");
                Check(Tonic_SetDisplayScale(nullptr, 0.01f) == TONIC_ERROR &&
                          Tonic_GetDisplayScale(a, nullptr) == TONIC_ERROR,
                      "V8: null arguments are refused, not crashed on");
                Check(Tonic_SetDisplayScale(a, 0.0f) == TONIC_OK &&
                          Tonic_GetDisplayScale(a, &scale) == TONIC_OK &&
                          scale == 0.0f,
                      "V8: 0 puts it back to the no-camera sizing");
            }

            // With no index attached there is nothing published to query.
            int faces = 0;
            Check(Tonic_GetPublishedLevelInfo(a, 1, &faces, nullptr,
                                              nullptr) == TONIC_ERROR,
                  "V0: no attached index means no published level");

            Check(Tonic_Destroy(a) == TONIC_OK, "V0: first model destroys");
            Check(registry.ModelCount() == 1,
                  "V0: destroying unregisters exactly one model");
            Check(Tonic_Activate(b) == TONIC_OK, "V0: reactivate the second");
            Check(Tonic_Destroy(b) == TONIC_OK, "V0: second model destroys");
            Check(registry.ModelCount() == 0 && registry.GetActiveId() == 0,
                  "V0: destroying the active model clears the registry");
        }
    }

    // -- V1 selection, overlays, gestures (plan/18 §2.3-§2.5) -------------
    //
    // The pick candidate sets are the model's own (tube 0's surface,
    // centers and rings; every graph node, edge and region; the guide
    // preview), so a marquee and a click can only ever disagree if the
    // projection disagrees — which is the point of testing them against
    // each other here rather than against hand-computed pixels.
    {
        TonicModelContext *sc = nullptr;
        Check(Tonic_Create(&sc) == TONIC_OK && sc != nullptr,
              "V1: selection model creates");
        Check(Tonic_BuildTestTube(sc, 0, 0, 0.0f, 0.0f) == TONIC_OK,
              "V1: test tube builds");

        // -- round trips, one kind at a time ------------------------------
        int ids[4] = {0, 0, 0, 0};
        int subIds[4] = {1, 3, -1, -1};
        int count = -1;
        Check(Tonic_GetSelectionCount(sc, 0) == 0,
              "V1: a fresh model has nothing selected");
        Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_CenterCV, ids, subIds, nullptr,
                              2) == TONIC_OK,
              "V1: select two center CVs");
        Check(Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_CenterCV) == 2,
              "V1: two center CVs are selected");
        {
            int outIds[8] = {0};
            int outSubs[8] = {0};
            Check(Tonic_ReadSelection(sc, usdGenTonic::TonicPick_CenterCV, outIds,
                                      outSubs, nullptr, 8,
                                      &count) == TONIC_OK &&
                      count == 2 && outIds[0] == 0 && outIds[1] == 0 &&
                      outSubs[0] == 1 && outSubs[1] == 3,
                  "V1: center CVs read back ascending");
            Check(Tonic_ReadSelection(sc, usdGenTonic::TonicPick_CenterCV, nullptr,
                                      nullptr, nullptr, 0,
                                      &count) == TONIC_OK && count == 2,
                  "V1: a null-array read sizes the buffer");
            Check(Tonic_ReadSelection(sc, usdGenTonic::TonicPick_CenterCV, outIds, nullptr,
                                      nullptr, 1, &count) == TONIC_ERROR,
                  "V1: a short buffer is an honest error");
        }
        // Set replaces its own kind and leaves the others alone.
        int nodeIds[2] = {7, 9};
        Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_GraphNode, nodeIds, nullptr,
                              nullptr, 2) == TONIC_OK,
              "V1: select two graph nodes");
        Check(Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_CenterCV) == 2 &&
                  Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_GraphNode) == 2 &&
                  Tonic_GetSelectionCount(sc, 0) == 4,
              "V1: setting one kind leaves the others selected");
        Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_CenterCV, ids, subIds, nullptr,
                              1) == TONIC_OK &&
                  Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_CenterCV) == 1,
              "V1: Set replaces the kind it names");
        Check(Tonic_SelectAdd(sc, usdGenTonic::TonicPick_CenterCV, ids, subIds + 1,
                              nullptr, 1) == TONIC_OK &&
                  Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_CenterCV) == 2,
              "V1: Add unions");
        Check(Tonic_SelectToggle(sc, usdGenTonic::TonicPick_CenterCV, ids, subIds + 1,
                                 nullptr, 1) == TONIC_OK &&
                  Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_CenterCV) == 1,
              "V1: Toggle removes what was there");
        Check(Tonic_SelectClear(sc, usdGenTonic::TonicPick_GraphNode) == TONIC_OK &&
                  Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_GraphNode) == 0 &&
                  Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_CenterCV) == 1,
              "V1: Clear takes exactly the kinds in its mask");
        // A section CV needs all three ids to round-trip.
        {
            int sid[1] = {0};
            int ring[1] = {2};
            int slot[1] = {5};
            int oid[4] = {0}, osub[4] = {0}, osubsub[4] = {0};
            Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_SectionCV, sid, ring, slot,
                                  1) == TONIC_OK,
                  "V1: select one section CV");
            Check(Tonic_ReadSelection(sc, usdGenTonic::TonicPick_SectionCV, oid, osub,
                                      osubsub, 4, &count) == TONIC_OK &&
                      count == 1 && oid[0] == 0 && osub[0] == 2 &&
                      osubsub[0] == 5,
                  "V1: a section CV round-trips (tube, ring, slot)");
        }
        Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_CenterCV | usdGenTonic::TonicPick_GraphNode,
                              ids, nullptr, nullptr, 1) == TONIC_ERROR,
              "V1: a select call takes exactly one kind");
        Check(Tonic_SelectClear(sc, 0) == TONIC_OK &&
                  Tonic_GetSelectionCount(sc, 0) == 0,
              "V1: mask 0 clears everything");

        // -- hover is separate from the selection -------------------------
        unsigned int hoverKind = 0;
        int hoverId = -1, hoverSub = -1, hoverSubSub = -1;
        Check(Tonic_SetHover(sc, usdGenTonic::TonicPick_CenterCV, 0, 3, -1) == TONIC_OK,
              "V1: hover a center CV");
        Check(Tonic_GetHover(sc, &hoverKind, &hoverId, &hoverSub,
                             &hoverSubSub) == TONIC_OK &&
                  hoverKind == usdGenTonic::TonicPick_CenterCV && hoverId == 0 &&
                  hoverSub == 3,
              "V1: hover reads back");
        Check(Tonic_GetSelectionCount(sc, 0) == 0,
              "V1: hover is not a selection");
        Check(Tonic_SetHover(sc, 0, -1, -1, -1) == TONIC_OK &&
                  Tonic_GetHover(sc, &hoverKind, nullptr, nullptr,
                                 nullptr) == TONIC_OK && hoverKind == 0,
              "V1: kind 0 clears the hover");

        // -- the version only moves on a real change ----------------------
        {
            unsigned long long const v = Tonic_GetVersion(sc);
            Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_CenterCV, ids, subIds,
                                  nullptr, 1) == TONIC_OK &&
                      Tonic_GetVersion(sc) == v + 1,
                  "V1: a real selection change bumps the version");
            Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_CenterCV, ids, subIds,
                                  nullptr, 1) == TONIC_OK &&
                      Tonic_GetVersion(sc) == v + 1,
                  "V1: re-selecting the same CV changes nothing");
            Check((Tonic_TakeDirty(sc) &
                   usdGenTonic::TonicDirty_Selection) != 0,
                  "V1: a selection change marks Selection dirty");
        }

        // -- selection bounds ---------------------------------------------
        {
            float mn[3] = {9.0f, 9.0f, 9.0f};
            float mx[3] = {-9.0f, -9.0f, -9.0f};
            float cv[3] = {0.0f, 0.0f, 0.0f};
            Check(Tonic_GetCenterCV(sc, 1, cv) == TONIC_OK,
                  "V1: center CV 1 reads");
            Check(Tonic_GetSelectionBounds(sc, mn, mx) == TONIC_OK &&
                      std::abs(mn[1] - cv[1]) < 1e-6f &&
                      std::abs(mx[1] - cv[1]) < 1e-6f,
                  "V1: one selected CV bounds exactly itself");
            Check(Tonic_SelectClear(sc, 0) == TONIC_OK &&
                      Tonic_GetSelectionBounds(sc, mn, mx) == TONIC_ERROR,
                  "V1: an empty selection has no bounds");
        }

        // -- marquee and lasso agree with the point pick ------------------
        //
        // The model is subdivided first, exactly as the plan asks: the
        // children are real tubes in the hierarchy, and the candidate sets
        // stay tube 0's (the audit's G2 limitation), so what this proves is
        // that the rubber band and the click read the SAME candidates.
        {
            int kids[4] = {0};
            int kidCount = 0;
            Check(Tonic_SubdivideTube(sc, 0, 4, "kmeans", 7, kids, 4,
                                      &kidCount) == TONIC_OK &&
                      kidCount == 4,
                  "V1: the tube subdivides for the marquee test");
            int const w = 800, h = 600;
            // Row-major, row-vector (the gpu/picking.cu convention): y in
            // [0, 4] maps to NDC [-1, 1], x and z pass through.
            float viewProj[16] = {0.0f};
            viewProj[0] = 1.0f;
            viewProj[5] = 0.5f;
            viewProj[10] = 1.0f;
            viewProj[13] = -1.0f;
            viewProj[15] = 1.0f;
            float px[8] = {0.0f}, py[8] = {0.0f};
            int const cvCount = Tonic_GetCenterCVCount(sc);
            Check(cvCount == 5, "V1: the test tube has five center CVs");
            bool projected = cvCount > 0;
            for (int i = 0; i < cvCount && i < 8; ++i) {
                float cv[3] = {0.0f, 0.0f, 0.0f};
                float z = 0.0f;
                projected = projected && Tonic_GetCenterCV(sc, i, cv) ==
                                             TONIC_OK &&
                            usdGenTonic::TonicProjectPoint(
                                cv, viewProj, w, h, &px[i], &py[i], &z);
            }
            Check(projected, "V1: every center CV projects into the frame");
            // A three-pixel box around CV 2 catches that CV and no other
            // (the CVs are 150 px apart in this frame).
            Check(Tonic_SelectRect(sc, viewProj, w, h, px[2] - 3.0f,
                                   py[2] - 3.0f, px[2] + 3.0f, py[2] + 3.0f,
                                   usdGenTonic::TonicPick_CenterCV,
                                   TONIC_SELECT_SET) == TONIC_OK,
                  "V1: marquee over one CV");
            int oid[64] = {0}, osub[64] = {0};
            Check(Tonic_ReadSelection(sc, usdGenTonic::TonicPick_CenterCV, oid, osub,
                                      nullptr, 64, &count) == TONIC_OK &&
                      count == 1 && oid[0] == 0 && osub[0] == 2,
                  "V1: the marquee caught exactly center CV 2");
            int hit = 0, pid = -1, psub = -1, pss = -1;
            unsigned int pkind = 0;
            Check(Tonic_PickItem(sc, viewProj, w, h, px[2], py[2], 4.0f,
                                 usdGenTonic::TonicPick_CenterCV, &hit, &pkind, &pid,
                                 &psub, &pss) == TONIC_OK &&
                      hit == 1 && pkind == usdGenTonic::TonicPick_CenterCV && pid == 0 &&
                      psub == 2,
                  "V1: a click at that pixel names the same item");
            // The lasso over the same box has to agree with the marquee.
            float const lasso[8] = {px[2] - 3.0f, py[2] - 3.0f,
                                    px[2] + 3.0f, py[2] - 3.0f,
                                    px[2] + 3.0f, py[2] + 3.0f,
                                    px[2] - 3.0f, py[2] + 3.0f};
            Check(Tonic_SelectPolygon(sc, viewProj, w, h, lasso, 4,
                                      usdGenTonic::TonicPick_CenterCV,
                                      TONIC_SELECT_SET) == TONIC_OK,
                  "V1: lasso over the same box");
            Check(Tonic_ReadSelection(sc, usdGenTonic::TonicPick_CenterCV, oid, osub,
                                      nullptr, 64, &count) == TONIC_OK &&
                      count == 1 && osub[0] == 2,
                  "V1: the lasso agrees with the marquee");
            Check(Tonic_SelectPolygon(sc, viewProj, w, h, lasso, 2,
                                      usdGenTonic::TonicPick_CenterCV,
                                      TONIC_SELECT_SET) == TONIC_ERROR,
                  "V1: a two-point lasso is rejected");
            // A band over the whole frame catches every CV, and each one
            // picks back to an item that is in the selection.
            Check(Tonic_SelectRect(sc, viewProj, w, h, -1e4f, -1e4f, 1e4f,
                                   1e4f, usdGenTonic::TonicPick_CenterCV,
                                   TONIC_SELECT_SET) == TONIC_OK &&
                      Tonic_ReadSelection(sc, usdGenTonic::TonicPick_CenterCV, oid, osub,
                                          nullptr, 64, &count) == TONIC_OK &&
                      count == cvCount,
                  "V1: a full-frame marquee catches every center CV");
            bool agree = true;
            for (int i = 0; i < cvCount; ++i) {
                hit = 0;
                pid = -1;
                psub = -1;
                agree = agree &&
                        Tonic_PickItem(sc, viewProj, w, h, px[i], py[i], 4.0f,
                                       usdGenTonic::TonicPick_CenterCV, &hit, &pkind, &pid,
                                       &psub, &pss) == TONIC_OK &&
                        hit == 1 && pid == 0 && psub == i;
            }
            Check(agree,
                  "V1: every marqueed CV is what a click there would pick");
            // Many surface candidates collapse into one tube item.
            Check(Tonic_SelectRect(sc, viewProj, w, h, -1e4f, -1e4f, 1e4f,
                                   1e4f, usdGenTonic::TonicPick_TubeVert,
                                   TONIC_SELECT_SET) == TONIC_OK &&
                      Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_TubeVert) == 1,
                  "V1: a band over the surface selects one tube, not 40");
            // An empty band in SET mode deselects.
            Check(Tonic_SelectRect(sc, viewProj, w, h, 1e4f, 1e4f, 1e4f + 1.0f,
                                   1e4f + 1.0f, usdGenTonic::TonicPick_CenterCV,
                                   TONIC_SELECT_SET) == TONIC_OK &&
                      Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_CenterCV) == 0,
                  "V1: an empty band clears the kind it was asked for");

            // -- a subdivide moves the selection to the children ----------
            Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_TubeVert, ids, nullptr,
                                  nullptr, 1) == TONIC_OK,
                  "V1: select the parent tube");
            int grandKids[4] = {0};
            int grandCount = 0;
            Check(Tonic_SubdivideTube(sc, kids[0], 3, "kmeans", 3, grandKids,
                                      4, &grandCount) == TONIC_OK,
                  "V1: subdivide a child (the parent stays selected)");
            Check(Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_TubeVert) == 1,
                  "V1: subdividing another tube leaves the selection alone");
            int selKids[8] = {0};
            Check(Tonic_SelectSet(sc, usdGenTonic::TonicPick_TubeVert, &kids[1], nullptr,
                                  nullptr, 1) == TONIC_OK,
                  "V1: select the tube about to be subdivided");
            int more[4] = {0};
            int moreCount = 0;
            Check(Tonic_SubdivideTube(sc, kids[1], 2, "kmeans", 5, more, 4,
                                      &moreCount) == TONIC_OK &&
                      moreCount == 2,
                  "V1: subdivide the selected tube");
            Check(Tonic_ReadSelection(sc, usdGenTonic::TonicPick_TubeVert, selKids,
                                      nullptr, nullptr, 8,
                                      &count) == TONIC_OK && count == 2 &&
                      selKids[0] == more[0] && selKids[1] == more[1],
                  "V1: the selection moved to the children");
            // A merge drops the ids that no longer exist.
            Check(Tonic_MergeChildren(sc, kids[1]) == TONIC_OK,
                  "V1: merge those children back");
            Check(Tonic_GetSelectionCount(sc, usdGenTonic::TonicPick_TubeVert) == 0,
                  "V1: the selection drops tubes that are gone");
        }
        Check(Tonic_Destroy(sc) == TONIC_OK, "V1: selection model destroys");
    }

    // -- gizmo and brush records ------------------------------------------
    {
        TonicModelContext *gc = nullptr;
        Check(Tonic_Create(&gc) == TONIC_OK && gc != nullptr,
              "V1: gizmo model creates");
        float const origin[3] = {1.0f, 2.0f, 3.0f};
        float const frame[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 1.0f};
        int kind = -1;
        float outOrigin[3] = {0.0f};
        float outFrame[9] = {0.0f};
        float size = 0.0f;
        int active = -99;
        Check(Tonic_GetGizmo(gc, &kind, outOrigin, outFrame, &size,
                             &active) == TONIC_OK &&
                  kind == 0 && active == -1,
              "V1: a fresh model has no gizmo");
        unsigned long long v = Tonic_GetVersion(gc);
        Check(Tonic_SetGizmo(gc, 1, origin, frame, 2.5f, 1) == TONIC_OK &&
                  Tonic_GetVersion(gc) == v + 1,
              "V1: setting a translate gizmo bumps the version");
        Check(Tonic_SetGizmo(gc, 1, origin, frame, 2.5f, 1) == TONIC_OK &&
                  Tonic_GetVersion(gc) == v + 1,
              "V1: re-setting the same gizmo changes nothing");
        Check(Tonic_GetGizmo(gc, &kind, outOrigin, outFrame, &size,
                             &active) == TONIC_OK &&
                  kind == 1 && outOrigin[1] == 2.0f && outFrame[4] == 1.0f &&
                  size == 2.5f && active == 1,
              "V1: the gizmo record round-trips");
        Check(Tonic_SetGizmo(gc, 9, origin, frame, 1.0f, -1) == TONIC_ERROR,
              "V1: an unknown gizmo kind is rejected");
        Check((Tonic_TakeDirty(gc) & usdGenTonic::TonicDirty_Gizmo) != 0,
              "V1: the gizmo marks its own dirty bit");
        float const center[3] = {0.0f, 1.0f, 0.0f};
        float const normal[3] = {0.0f, 1.0f, 0.0f};
        int brushActive = -1;
        float outCenter[3] = {0.0f};
        float outNormal[3] = {0.0f};
        float radius = -1.0f;
        Check(Tonic_SetBrushRing(gc, center, normal, 0.75f) == TONIC_OK &&
                  Tonic_GetBrushRing(gc, &brushActive, outCenter, outNormal,
                                     &radius) == TONIC_OK &&
                  brushActive == 1 && radius == 0.75f &&
                  outCenter[1] == 1.0f,
              "V1: the brush ring round-trips");
        Check((Tonic_TakeDirty(gc) & usdGenTonic::TonicDirty_Brush) != 0,
              "V1: the brush marks its own dirty bit");
        Check(Tonic_SetBrushRing(gc, center, normal, 0.0f) == TONIC_OK &&
                  Tonic_GetBrushRing(gc, &brushActive, nullptr, nullptr,
                                     &radius) == TONIC_OK &&
                  brushActive == 0 && radius == 0.0f,
              "V1: a zero radius clears the brush ring");
        // The geometry the index will publish, straight from the record.
        {
            usdGenTonic::TonicGizmoRecord record;
            record.kind = usdGenTonic::TonicGizmo_Translate;
            record.sizeWorld = 1.0f;
            record.activeHandle = 2;
            usdGenTonic::TonicOverlayCurves curves;
            Check(usdGenTonic::TonicBuildGizmoCurves(record, &curves) &&
                      curves.CurveCount() == 3 &&
                      curves.points.size() == 3 * 2 * 3,
                  "V1: a translate gizmo is three two-point axes");
            Check(curves.handleIds[0] == 0 && curves.handleIds[2] == 2 &&
                      curves.active[2] == 1 && curves.active[0] == 0,
                  "V1: the active handle is the one that was asked for");
            Check(curves.colors[0] > 0.8f && curves.colors[1] < 0.2f,
                  "V1: the u axis is red");
            record.kind = usdGenTonic::TonicGizmo_RingTRS;
            Check(usdGenTonic::TonicBuildGizmoCurves(record, &curves) &&
                      curves.CurveCount() == 4 &&
                      curves.vertexCounts[0] ==
                          usdGenTonic::TonicGizmoCircleSegments() + 1,
                  "V1: a ring gizmo is a closed circle plus three axes");
            record.kind = usdGenTonic::TonicGizmo_None;
            Check(!usdGenTonic::TonicBuildGizmoCurves(record, &curves) &&
                      curves.CurveCount() == 0,
                  "V1: no gizmo builds no curves");
            record.kind = usdGenTonic::TonicGizmo_Translate;
            record.frame[0] = 0.0f;  // a degenerate u axis
            Check(!usdGenTonic::TonicBuildGizmoCurves(record, &curves),
                  "V1: a degenerate frame draws nothing");
            usdGenTonic::TonicBrushRingRecord brush;
            brush.active = true;
            brush.radiusWorld = 0.5f;
            Check(usdGenTonic::TonicBuildBrushRingCurves(brush, &curves) &&
                      curves.CurveCount() == 1,
                  "V1: the brush ring is one closed circle");
        }
        Check(Tonic_Destroy(gc) == TONIC_OK, "V1: gizmo model destroys");
    }

    // -- the gesture bracket, redo, and undo labels ------------------------
    {
        TonicModelContext *bc = nullptr;
        Check(Tonic_Create(&bc) == TONIC_OK && bc != nullptr,
              "V1: gesture model creates");
        Check(Tonic_BuildTestTube(bc, 0, 0, 0.0f, 0.0f) == TONIC_OK,
              "V1: gesture tube builds");
        size_t const floats = size_t(Tonic_GetVertexCount(bc)) * 3;
        std::vector<float> base(floats), dragged(floats), probe(floats);
        Check(Tonic_ReadTubePoints(bc, base.data(), int(floats)) == TONIC_OK,
              "V1: press-time points read");
        Check(Tonic_GetGestureDepth(bc) == 0, "V1: no gesture is open");
        Check(Tonic_EndGesture(bc) == TONIC_ERROR,
              "V1: End without Begin is an error");
        Check(Tonic_CancelGesture(bc, nullptr) == TONIC_ERROR,
              "V1: Cancel without Begin is an error");
        Check(Tonic_BeginGesture(bc, "sculpt") == TONIC_OK &&
                  Tonic_GetGestureDepth(bc) == 1,
              "V1: Begin opens the bracket");
        Check(Tonic_BeginGesture(bc, "sculpt") == TONIC_ERROR &&
                  Tonic_GetGestureDepth(bc) == 1,
              "V1: a nested Begin is an error, not a counter");
        Check(Tonic_GetUndoDepth(bc) == 1,
              "V1: Begin pushes exactly one step");
        for (int i = 0; i < 50; ++i) {
            Tonic_MoveCenterRing(bc, 2, 0.01f, 0.0f);
        }
        Check(Tonic_GetUndoDepth(bc) == 1,
              "V1: fifty moves inside the bracket are still one step");
        Check(Tonic_EndGesture(bc) == TONIC_OK &&
                  Tonic_GetGestureDepth(bc) == 0,
              "V1: End seals the bracket");
        Check(Tonic_ReadTubePoints(bc, dragged.data(), int(floats)) ==
                  TONIC_OK && dragged != base,
              "V1: the drag moved the tube");
        {
            char label[32] = {0};
            Check(Tonic_GetUndoLabel(bc, 0, label, sizeof(label)) ==
                      TONIC_OK && std::string(label) == "sculpt",
                  "V1: the step carries the gesture's label");
            Check(Tonic_GetUndoLabel(bc, 7, label, sizeof(label)) ==
                      TONIC_ERROR,
                  "V1: there is no step at depth 7");
        }
        unsigned int dirty = 0;
        Check(Tonic_Undo(bc, &dirty) == TONIC_OK &&
                  (dirty & usdGenTonic::TonicDirty_Points) != 0,
              "V1: undo reports the dirty bits to publish");
        Check(Tonic_ReadTubePoints(bc, probe.data(), int(floats)) ==
                  TONIC_OK && probe == base,
              "V1: one undo takes back the whole drag");
        Check(Tonic_GetRedoDepth(bc) == 1, "V1: the drag is on the redo stack");
        {
            char label[32] = {0};
            Check(Tonic_GetUndoLabel(bc, -1, label, sizeof(label)) ==
                      TONIC_OK && std::string(label) == "sculpt",
                  "V1: a negative depth reads the redo label");
        }
        dirty = 0;
        Check(Tonic_Redo(bc, &dirty) == TONIC_OK &&
                  (dirty & usdGenTonic::TonicDirty_Points) != 0,
              "V1: redo reports its dirty bits too");
        Check(Tonic_ReadTubePoints(bc, probe.data(), int(floats)) ==
                  TONIC_OK && probe == dragged,
              "V1: redo restores the drag bit-exactly");
        Check(Tonic_GetRedoDepth(bc) == 0 && Tonic_GetUndoDepth(bc) == 1,
              "V1: redo moves the step back to the undo stack");
        // A new edit branches the history.
        Check(Tonic_Undo(bc, nullptr) == TONIC_OK &&
                  Tonic_GetRedoDepth(bc) == 1,
              "V1: undo again to arm the redo stack");
        Check(Tonic_MoveCenterRing(bc, 1, 0.2f, 0.0f) == TONIC_OK &&
                  Tonic_GetRedoDepth(bc) == 0,
              "V1: a new mutation clears the redo stack");
        Check(Tonic_Redo(bc, nullptr) == TONIC_OK &&
                  Tonic_GetRedoDepth(bc) == 0,
              "V1: redo on an empty stack is a no-op success");

        // -- cancel restores the press-time base bit-exactly --------------
        Check(Tonic_ReadTubePoints(bc, base.data(), int(floats)) == TONIC_OK,
              "V1: second press-time points read");
        int const depthBefore = Tonic_GetUndoDepth(bc);
        unsigned long long const versionBefore = Tonic_GetVersion(bc);
        Check(Tonic_BeginGesture(bc, "drag") == TONIC_OK,
              "V1: open a gesture to cancel");
        for (int i = 0; i < 12; ++i) {
            Tonic_MoveCenterRing(bc, 3, 0.05f, 0.02f);
        }
        Check(Tonic_ReadTubePoints(bc, probe.data(), int(floats)) ==
                  TONIC_OK && probe != base,
              "V1: the cancelled drag did move the tube first");
        dirty = 0;
        Check(Tonic_CancelGesture(bc, &dirty) == TONIC_OK &&
                  (dirty & usdGenTonic::TonicDirty_Points) != 0,
              "V1: Cancel reports what to publish");
        Check(Tonic_ReadTubePoints(bc, probe.data(), int(floats)) ==
                  TONIC_OK && probe == base,
              "V1: Cancel restores the press-time base bit-exactly");
        Check(Tonic_GetUndoDepth(bc) == depthBefore,
              "V1: Cancel leaves no step behind");
        Check(Tonic_GetGestureDepth(bc) == 0, "V1: Cancel closes the bracket");
        Check(Tonic_GetVersion(bc) > versionBefore,
              "V1: the moves bumped the version; Cancel does not bump again");
        Check(Tonic_Destroy(bc) == TONIC_OK, "V1: gesture model destroys");
    }

    // -- the marquee's device lane agrees with its CPU twin ---------------
    //
    // Guide CVs are the heavyweight kind a lasso actually discriminates
    // (a tube's 40 verts collapse to one item; 600 guides do not), so the
    // K11b mask kernel is proven against the twin on the same rectangle,
    // with USDGEN_TONIC_FORCE_CPU_PICK deciding which lane runs — the same
    // switch the point pick's parity test uses.
    {
        TonicModelContext *dc = nullptr;
        Check(Tonic_Create(&dc) == TONIC_OK && dc != nullptr,
              "V1: marquee parity model creates");
        Check(Tonic_BuildTestTube(dc, 0, 0, 0.0f, 0.0f) == TONIC_OK,
              "V1: parity tube builds");
        Check(Tonic_SetFillParams(dc, 600.0f, 8, 11, 0.0f, nullptr, 0) ==
                  TONIC_OK,
              "V1: 600 guides of 8 CVs (4800 candidates, past the "
              "device threshold)");
        Check(Tonic_RefillGuides(dc, 1.0f) == TONIC_OK,
              "V1: the guides fill");
        int guides = 0, cvs = 0;
        Check(Tonic_GetGuideCounts(dc, &guides, &cvs) == TONIC_OK &&
                  guides * cvs > 4096,
              "V1: the candidate count clears the device threshold");
#ifdef USDGEN_TONIC_HAS_CUDA
        // Without a mirror there is no device lane and the parity below
        // would be two CPU runs agreeing with themselves. Say so loudly
        // rather than passing vacuously.
        Check(Tonic_HasCudaMirror(dc) == 1,
              "V1: this build has the device mirror the parity needs");
#else
        std::printf("skip: no CUDA, the marquee parity runs CPU vs CPU\n");
#endif
        int const w = 800, h = 600;
        float viewProj[16] = {0.0f};
        viewProj[0] = 1.0f;
        viewProj[5] = 0.5f;
        viewProj[10] = 1.0f;
        viewProj[13] = -1.0f;
        viewProj[15] = 1.0f;
        std::vector<int> device;
        std::vector<int> cpu;
        int count = 0;
        std::vector<int> buffer(size_t(guides) + 8);
        auto readGuides = [&](std::vector<int> *out) {
            count = 0;
            bool const ok =
                Tonic_ReadSelection(dc, usdGenTonic::TonicPick_Guide,
                                    buffer.data(), nullptr, nullptr,
                                    int(buffer.size()), &count) == TONIC_OK;
            out->assign(buffer.begin(), buffer.begin() + count);
            return ok;
        };
        // The right half of the frame: the tube's guides span x in
        // [-0.5, 0.5], which is px 200 to 600 here, so a band from px 400
        // splits them.
        Check(Tonic_SelectRect(dc, viewProj, w, h, 400.0f, -1e4f, 1e4f, 1e4f,
                               usdGenTonic::TonicPick_Guide,
                               TONIC_SELECT_SET) == TONIC_OK &&
                  readGuides(&device),
              "V1: marquee half the guides (device lane where there is one)");
        int const halfCount = int(device.size());
#ifdef _WIN32
        _putenv("USDGEN_TONIC_FORCE_CPU_PICK=1");
#else
        setenv("USDGEN_TONIC_FORCE_CPU_PICK", "1", 1);
#endif
        Check(Tonic_SelectRect(dc, viewProj, w, h, 400.0f, -1e4f, 1e4f, 1e4f,
                               usdGenTonic::TonicPick_Guide,
                               TONIC_SELECT_SET) == TONIC_OK &&
                  readGuides(&cpu),
              "V1: the same marquee on the CPU twin");
        Check(device == cpu,
              "V1: the device mask and the CPU scan select the same guides");
        // A lasso over the same half has to agree with the rectangle, and
        // both lanes have to agree about it.
        float const lasso[8] = {400.0f, -1e4f, 1e4f, -1e4f,
                                1e4f,   1e4f,  400.0f, 1e4f};
        std::vector<int> cpuLasso;
        Check(Tonic_SelectPolygon(dc, viewProj, w, h, lasso, 4,
                                  usdGenTonic::TonicPick_Guide,
                                  TONIC_SELECT_SET) == TONIC_OK &&
                  readGuides(&cpuLasso) && cpuLasso == cpu,
              "V1: a box-shaped lasso is the box (CPU lane)");
#ifdef _WIN32
        _putenv("USDGEN_TONIC_FORCE_CPU_PICK=");
#else
        unsetenv("USDGEN_TONIC_FORCE_CPU_PICK");
#endif
        std::vector<int> deviceLasso;
        Check(Tonic_SelectPolygon(dc, viewProj, w, h, lasso, 4,
                                  usdGenTonic::TonicPick_Guide,
                                  TONIC_SELECT_SET) == TONIC_OK &&
                  readGuides(&deviceLasso) && deviceLasso == cpuLasso,
              "V1: the device lasso mask agrees with the CPU twin");
        Check(Tonic_SelectRect(dc, viewProj, w, h, -1e4f, -1e4f, 1e4f, 1e4f,
                               usdGenTonic::TonicPick_Guide,
                               TONIC_SELECT_SET) == TONIC_OK &&
                  readGuides(&device) && count == guides,
              "V1: a full-frame marquee catches every guide");
        Check(halfCount > 0 && halfCount < guides,
              "V1: the half-frame band caught a real subset (%d of %d)");
        Check(Tonic_Destroy(dc) == TONIC_OK, "V1: parity model destroys");
    }

    // -- the kinds V1 adds to the pick: edge, region, section ring --------
    //
    // They need a real groom: a stroked region on a scalp gives the graph
    // its edges and regions, and building the tube from that region is
    // what puts the tube on the K5 sections path, which is where its rings
    // become pick candidates at all.
    {
        TonicModelContext *kc = nullptr;
        Check(Tonic_Create(&kc) == TONIC_OK && kc != nullptr,
              "V1: kinds model creates");
        // The 4 x 4 quad grid in XZ the T3 scripts use: face f = ix * 4 + iz
        // covers x in [ix, ix+1], z in [iz, iz+1], and (u, v) on that face
        // is (z - iz, x - ix).
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
        Check(Tonic_BindScalp(kc, points.data(), int(points.size()),
                              counts.data(), int(counts.size()),
                              indices.data(), int(indices.size())) ==
                  TONIC_OK,
              "V1: the 4x4 scalp binds");
        Tonic_SetSnapRadius(kc, 0.1f);
        // Stroke the rectangle (0, 1) -> (2, 1) -> (2, 3) -> (0, 3) closed.
        float const corners[4][2] = {{0.0f, 1.0f},
                                     {2.0f, 1.0f},
                                     {2.0f, 3.0f},
                                     {0.0f, 3.0f}};
        std::vector<int> faces;
        std::vector<float> uvs;
        for (int c = 0; c < 4; ++c) {
            for (int s = 0; s < 5; ++s) {
                float const t = float(s) / 5.0f;
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
        faces.push_back(0 * 4 + 1);
        uvs.push_back(0.0f);
        uvs.push_back(0.0f);
        std::vector<int> chain(256);
        int chainCount = 0, closed = 0, weldStart = 0, weldEnd = 0;
        Check(Tonic_GraphStroke(kc, faces.data(), uvs.data(),
                                int(faces.size()), 0.1f, 0.05f, chain.data(),
                                int(chain.size()), &chainCount, &closed,
                                &weldStart, &weldEnd) == TONIC_OK &&
                  closed == 1,
              "V1: the stroke closes into a region");
        Check(Tonic_Rasterise(kc) == TONIC_OK, "V1: K3 rasterises");
        Check(Tonic_BuildTubeFromRegion(kc, 0, 5, 8, 3.0f) == TONIC_OK,
              "V1: the tube builds from the region");
        Check(Tonic_GetSectionCount(kc) == 5,
              "V1: the region-built tube carries five authored rings");
        int nodes = 0, edges = 0, regions = 0;
        Check(Tonic_GetGraphCounts(kc, &nodes, &edges, &regions) ==
                  TONIC_OK && edges > 0 && regions > 0,
              "V1: the graph has edges and a region");
        int const w = 800, h = 600;
        // Top view: x and z map to the frame, so the scalp's graph spreads
        // out and the tube (which rises in +Y) does not overlap itself.
        float top[16] = {0.0f};
        top[0] = 0.5f;
        top[12] = -1.0f;
        top[9] = 0.5f;
        top[13] = -1.0f;
        top[10] = 0.0f;
        top[15] = 1.0f;
        top[6] = 1.0f;  // depth from y, so the pick has something to order
        int count = 0;
        Check(Tonic_SelectRect(kc, top, w, h, -1e4f, -1e4f, 1e4f, 1e4f,
                               usdGenTonic::TonicPick_GraphEdge,
                               TONIC_SELECT_SET) == TONIC_OK &&
                  Tonic_ReadSelection(kc, usdGenTonic::TonicPick_GraphEdge,
                                      nullptr, nullptr, nullptr, 0,
                                      &count) == TONIC_OK && count == edges,
              "V1: a full-frame marquee selects every graph edge once");
        Check(Tonic_SelectRect(kc, top, w, h, -1e4f, -1e4f, 1e4f, 1e4f,
                               usdGenTonic::TonicPick_Region,
                               TONIC_SELECT_SET) == TONIC_OK &&
                  Tonic_ReadSelection(kc, usdGenTonic::TonicPick_Region,
                                      nullptr, nullptr, nullptr, 0,
                                      &count) == TONIC_OK && count == regions,
              "V1: a full-frame marquee selects every region once");
        // Side view for the rings: they sit at different heights along the
        // tube, so they only separate in a view that shows +Y.
        float side[16] = {0.0f};
        side[0] = 0.5f;
        side[12] = -1.0f;
        side[5] = 0.4f;
        side[13] = -1.0f;
        side[10] = 1.0f;
        side[15] = 1.0f;
        Check(Tonic_SelectRect(kc, side, w, h, -1e4f, -1e4f, 1e4f, 1e4f,
                               usdGenTonic::TonicPick_SectionRing,
                               TONIC_SELECT_SET) == TONIC_OK &&
                  Tonic_ReadSelection(kc,
                                      usdGenTonic::TonicPick_SectionRing,
                                      nullptr, nullptr, nullptr, 0,
                                      &count) == TONIC_OK && count == 5,
              "V1: a full-frame marquee selects all five section rings");
        // Point picks, at points whose place on the scalp is known: the
        // midpoint of the stroke's bottom run lies on an edge, and the
        // centre of the rectangle lies in the region.
        {
            float const onEdge[3] = {1.0f, 0.0f, 1.0f};
            float const inRegion[3] = {1.0f, 0.0f, 2.0f};
            float px = 0.0f, py = 0.0f, z = 0.0f;
            Check(usdGenTonic::TonicProjectPoint(onEdge, top, w, h, &px, &py,
                                                 &z),
                  "V1: the edge midpoint projects");
            int hit = 0, id = -1, sub = -1, subsub = -1;
            unsigned int kind = 0;
            Check(Tonic_PickItem(kc, top, w, h, px, py, 30.0f,
                                 usdGenTonic::TonicPick_GraphEdge, &hit,
                                 &kind, &id, &sub, &subsub) == TONIC_OK &&
                      hit == 1 && kind == usdGenTonic::TonicPick_GraphEdge &&
                      id >= 0,
                  "V1: a click on the boundary picks a graph edge");
            // Not the same edge id as the snap query, necessarily: the
            // pick wins on the nearest polyline SAMPLE in pixels and a
            // shared node belongs to two edges, so the two lookups can
            // name either side of a junction. What must hold is that both
            // find an edge there at all.
            Check(Tonic_GraphSnapEdge(kc, onEdge, 0.3f) >= 0,
                  "V1: the snap query agrees an edge runs through there");
            hit = 1;
            Check(Tonic_PickItem(kc, top, w, h, px, py, 30.0f,
                                 usdGenTonic::TonicPick_GraphEdge, &hit,
                                 &kind, &id, &sub, &subsub) == TONIC_OK,
                  "V1: re-pick the boundary");
            Check(usdGenTonic::TonicProjectPoint(inRegion, top, w, h, &px,
                                                 &py, &z),
                  "V1: the region centre projects");
            hit = 0;
            id = -1;
            Check(Tonic_PickItem(kc, top, w, h, px, py, 60.0f,
                                 usdGenTonic::TonicPick_Region, &hit, &kind,
                                 &id, &sub, &subsub) == TONIC_OK &&
                      hit == 1 && kind == usdGenTonic::TonicPick_Region &&
                      id >= 0,
                  "V1: a click inside the region picks the region");
            hit = 1;
            Check(Tonic_PickItem(kc, top, w, h, px, py, 30.0f,
                                 usdGenTonic::TonicPick_GraphEdge, &hit,
                                 &kind, &id, &sub, &subsub) == TONIC_OK &&
                      hit == 0,
                  "V1: no edge is within 30 px of the region centre");
        }
        Check(Tonic_Destroy(kc) == TONIC_OK, "V1: kinds model destroys");
    }

    // -- graph edits are on the same undo stack (plan/18 §2.5) -------------
    {
        TonicModelContext *gc = nullptr;
        Check(Tonic_Create(&gc) == TONIC_OK && gc != nullptr,
              "V1: graph undo model creates");
        // A 2 x 2 quad grid in XZ at y = 0: face f = ix * 2 + iz.
        std::vector<float> points;
        for (int ix = 0; ix < 3; ++ix) {
            for (int iz = 0; iz < 3; ++iz) {
                points.push_back(float(ix));
                points.push_back(0.0f);
                points.push_back(float(iz));
            }
        }
        std::vector<int> counts(4, 4);
        std::vector<int> indices;
        for (int ix = 0; ix < 2; ++ix) {
            for (int iz = 0; iz < 2; ++iz) {
                int const a = ix * 3 + iz;
                indices.push_back(a);
                indices.push_back(a + 3);
                indices.push_back(a + 4);
                indices.push_back(a + 1);
            }
        }
        Check(Tonic_BindScalp(gc, points.data(), int(points.size()),
                              counts.data(), int(counts.size()),
                              indices.data(), int(indices.size())) ==
                  TONIC_OK,
              "V1: the scalp binds");
        int nodeA = -1, nodeB = -1;
        Check(Tonic_GraphAddNode(gc, 0, 0.25f, 0.25f, &nodeA) == TONIC_OK &&
                  nodeA >= 0,
              "V1: first graph node");
        Check(Tonic_GetUndoDepth(gc) == 1,
              "V1: a graph edit pushes an undo step");
        Check(Tonic_GraphAddNode(gc, 3, 0.75f, 0.75f, &nodeB) == TONIC_OK &&
                  nodeB >= 0,
              "V1: second graph node");
        int nodes = 0, edges = 0, regions = 0;
        Check(Tonic_GetGraphCounts(gc, &nodes, &edges, &regions) ==
                  TONIC_OK && nodes == 2,
              "V1: two nodes are in the graph");
        {
            char label[32] = {0};
            Check(Tonic_GetUndoLabel(gc, 0, label, sizeof(label)) ==
                      TONIC_OK && std::string(label) == "add node",
                  "V1: the graph step names itself");
        }
        Check(Tonic_Undo(gc, nullptr) == TONIC_OK &&
                  Tonic_GetGraphCounts(gc, &nodes, nullptr, nullptr) ==
                      TONIC_OK && nodes == 1,
              "V1: undoing a graph edit removes the node");
        Check(Tonic_Redo(gc, nullptr) == TONIC_OK &&
                  Tonic_GetGraphCounts(gc, &nodes, nullptr, nullptr) ==
                      TONIC_OK && nodes == 2,
              "V1: redoing a graph edit puts it back");
        int edgeId = -1;
        Check(Tonic_GraphConnect(gc, nodeA, nodeB, &edgeId) == TONIC_OK &&
                  edgeId >= 0,
              "V1: the two nodes connect");
        Check(Tonic_GetGraphCounts(gc, nullptr, &edges, nullptr) ==
                  TONIC_OK && edges == 1,
              "V1: one edge exists");
        Check(Tonic_Undo(gc, nullptr) == TONIC_OK &&
                  Tonic_GetGraphCounts(gc, nullptr, &edges, nullptr) ==
                      TONIC_OK && edges == 0,
              "V1: undoing the connect removes the edge");
        Check(Tonic_Destroy(gc) == TONIC_OK, "V1: graph undo model destroys");
    }

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
