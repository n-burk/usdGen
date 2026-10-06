// usdGen imaging — StormSurgery lock-free test hook (test-only surface).
//
// UsdGenImagingTestHook exposes the atomic published-tile snapshot for the
// groom roots adopted by live UsdGenGroomSceneIndex instances, so
// testUsdGenStormSurgery can assert the P0 race contract from the render
// thread: every observation corresponds to one complete generation
// snapshot, never a torn map. Both entry points perform no owner wait (an
// immutable registry snapshot and the per-groom published map are
// atomic_load'ed, exactly like GetPrim).
//
// Lives in its own translation unit (NOT the plugin registration TU) so the
// symbols link into libusdGenImaging for testUsdGenStormSurgery
// (usdGenTestUtilsHd -> usdGenImaging). No new public plugin API.
#include "usdGenImaging/testHook.h"

#include "usdGenImaging/groomSceneIndexPlugin.h"
#include "usdGen/executionPipeline.h"

#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/types.h"
#include "pxr/base/tf/weakPtr.h"
#include "pxr/usd/sdf/path.h"

#include <algorithm>
#include <memory>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

struct RegistryEntry {
    HdSceneIndexBasePtr weak;
    // TfWeakPtr<void> compares the remnant/control identity, rather than a
    // raw address which may be reused after a scene-index destructor runs.
    TfWeakPtr<void> identity;
};
using RegistrySnapshot = std::shared_ptr<std::vector<RegistryEntry> const>;

// All membership mutation belongs to this serial framework owner. Readers
// load a completed immutable vector; weak promotion then keeps an index alive
// for a probe without an owner command or scene-index mutation.
struct HookRegistry {
    usdGen::UsdGenExecutionRuntime runtime{8};
    RegistrySnapshot published = std::make_shared<std::vector<RegistryEntry>>();
    usdGen::UsdGenExecutionPipeline owner{runtime};
};

HookRegistry &_HookRegistry()
{
    // Leaked on purpose, like every process-lifetime arena owner in this
    // library (see ImagingRuntime in usdGenImagingSession.cpp): a static
    // destructor here runs at DLL_PROCESS_DETACH on Windows, after ExitProcess
    // has killed the TBB workers, so the pipeline shutdown wait and the arena
    // teardown could never complete. External shutdown must quiesce callback
    // producers/readers before returning from main; Drain is the explicit
    // test/shutdown boundary.
    static auto* registry = new HookRegistry;
    return *registry;
}

template <class Mutation>
void _MutateRegistry(Mutation mutation)
{
    HookRegistry &registry = _HookRegistry();
    auto command = [&registry, mutation=std::move(mutation)]() mutable {
        RegistrySnapshot before = std::atomic_load(&registry.published);
        auto next = std::make_shared<std::vector<RegistryEntry>>(
            before ? *before : std::vector<RegistryEntry>{});
        mutation(*next);
        std::atomic_store(&registry.published,
            std::static_pointer_cast<std::vector<RegistryEntry> const>(next));
    };
    // Construction/destruction may happen from a framework callback.  Such a
    // callback must never wait behind its own owner.  External construction
    // retains the old immediate-visibility contract needed by test probes.
    if (usdGen::UsdGenExecutionPipeline::IsExecuting()) {
        registry.owner.PostCommand(std::move(command));
    } else {
        registry.owner.InvokeOwner(std::move(command));
    }
}

}  // namespace

void
UsdGenImagingTestHook::_RegisterIndex(HdSceneIndexBase *index)
{
    HdSceneIndexBasePtr weak = TfCreateWeakPtr(index);
    TfWeakPtr<void> identity(weak);
    _MutateRegistry([weak=std::move(weak), identity=std::move(identity)](
                        std::vector<RegistryEntry> &registry) mutable {
        for (auto const &candidate : registry) {
            if (candidate.identity == identity) return;
        }
        registry.erase(std::remove_if(registry.begin(), registry.end(),
            [](RegistryEntry const &candidate) {
                return candidate.weak.IsExpired();
            }), registry.end());
        registry.push_back({std::move(weak), std::move(identity)});
    });
}

void
UsdGenImagingTestHook::_UnregisterIndex(HdSceneIndexBase *index)
{
    TfWeakPtr<void> identity(TfCreateWeakPtr(index));
    _MutateRegistry([identity=std::move(identity)](
                        std::vector<RegistryEntry> &registry) {
        registry.erase(std::remove_if(registry.begin(), registry.end(),
                       [&identity](RegistryEntry const &candidate) {
                           return candidate.weak.IsExpired() ||
                               candidate.identity == identity;
                       }),
        registry.end());
    });
}

// Snapshot one index's published-tile generation stamps for `groom`:
// max `generation` int across the adopted groom's published map, or -1
// when the groom is unknown / unpublished. Lock-free past the registry
// copy: the map itself is atomic_load'ed.
int64_t
UsdGenImagingTestHook::_PublishedGenerationOn(
    HdSceneIndexBase const &index, SdfPath const &groom)
{
    UsdGenGroomSceneIndex const *groomIndex =
        dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    if (!groomIndex) return -1;
    return groomIndex->_TestPublishedGeneration(groom);
}

size_t
UsdGenImagingTestHook::_PublishedTileCountOn(
    HdSceneIndexBase const &index, SdfPath const &groom)
{
    UsdGenGroomSceneIndex const *groomIndex =
        dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    if (!groomIndex) return 0;
    return groomIndex->_TestPublishedTileCount(groom);
}

int64_t
UsdGenImagingTestHook::publishedGeneration(SdfPath const &groom)
{
    RegistrySnapshot weaks = std::atomic_load(&_HookRegistry().published);
    int64_t best = -1;
    if (!weaks) return 0;
    for (auto const &entry : *weaks) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(entry.weak);
        if (!live) continue;
        best = std::max(best, _PublishedGenerationOn(*live, groom));
    }
    return best < 0 ? 0 : best;
}

uint64_t
UsdGenImagingTestHook::issuedIngresses()
{
    RegistrySnapshot snapshot = std::atomic_load(&_HookRegistry().published);
    uint64_t greatest = 0;
    if (!snapshot) return greatest;
    for (auto const &entry : *snapshot) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(entry.weak);
        auto const *groom = live ?
            dynamic_cast<UsdGenGroomSceneIndex const *>(live.operator->()) : nullptr;
        if (groom) greatest = std::max(greatest, groom->_TestIssuedIngress());
    }
    return greatest;
}

size_t
UsdGenImagingTestHook::publishedTileCount(SdfPath const &groom)
{
    RegistrySnapshot weaks = std::atomic_load(&_HookRegistry().published);
    size_t total = 0;
    if (!weaks) return 0;
    for (auto const &entry : *weaks) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(entry.weak);
        if (!live) continue;
        total += _PublishedTileCountOn(*live, groom);
    }
    return total;
}

bool
UsdGenImagingTestHook::publishedCurveStats(SdfPath const &groom,
                                           uint64_t *curves,
                                           double *totalLength)
{
    uint64_t count = 0;
    double length = 0.0;
    bool found = false;
    RegistrySnapshot weaks = std::atomic_load(&_HookRegistry().published);
    if (weaks && groom.IsAbsolutePath() && groom.IsPrimPath()) {
        SdfPath const render = groom.AppendChild(TfToken("__usdGenRender"));
        static HdDataSourceLocator const countsLoc(
            TfToken("basisCurves"), TfToken("topology"),
            TfToken("curveVertexCounts"));
        static HdDataSourceLocator const pointsLoc(
            TfToken("primvars"), TfToken("points"), TfToken("primvarValue"));
        for (auto const &entry : *weaks) {
            HdSceneIndexBaseRefPtr live =
                TfCreateRefPtrFromProtectedWeakPtr(entry.weak);
            if (!live) continue;
            for (SdfPath const &tile : live->GetChildPrimPaths(render)) {
                HdSceneIndexPrim const prim = live->GetPrim(tile);
                HdSampledDataSourceHandle const countsDs =
                    HdSampledDataSource::Cast(
                        HdContainerDataSource::Get(prim.dataSource, countsLoc));
                HdSampledDataSourceHandle const pointsDs =
                    HdSampledDataSource::Cast(
                        HdContainerDataSource::Get(prim.dataSource, pointsLoc));
                if (!countsDs || !pointsDs) continue;
                VtValue const counts = countsDs->GetValue(0.0f);
                VtValue const points = pointsDs->GetValue(0.0f);
                if (!counts.IsHolding<VtIntArray>() ||
                    !points.IsHolding<VtVec3fArray>()) continue;
                found = true;
                VtVec3fArray const &p = points.UncheckedGet<VtVec3fArray>();
                size_t base = 0;
                for (int n : counts.UncheckedGet<VtIntArray>()) {
                    if (n < 0 || base + size_t(n) > p.size()) break;
                    for (int k = 1; k < n; ++k)
                        length += double((p[base + k] - p[base + k - 1]).GetLength());
                    base += size_t(n);
                    ++count;
                }
            }
        }
    }
    if (curves) *curves = count;
    if (totalLength) *totalLength = length;
    return found;
}

size_t
UsdGenImagingTestHook::registeredIndexCount()
{
    RegistrySnapshot snapshot = std::atomic_load(&_HookRegistry().published);
    return snapshot ? snapshot->size() : 0;
}

bool
UsdGenImagingTestHook::holdOneGroomOwnerCredit()
{
    RegistrySnapshot snapshot = std::atomic_load(&_HookRegistry().published);
    if (!snapshot) return false;
    for (auto const &entry : *snapshot) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(entry.weak);
        auto *groom = live ? dynamic_cast<UsdGenGroomSceneIndex *>(live.operator->()) : nullptr;
        if (groom) return groom->_TestHoldOwnerCredit();
    }
    return false;
}

void
UsdGenImagingTestHook::releaseGroomOwnerCredits()
{
    RegistrySnapshot snapshot = std::atomic_load(&_HookRegistry().published);
    if (!snapshot) return;
    for (auto const &entry : *snapshot) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(entry.weak);
        auto *groom = live ? dynamic_cast<UsdGenGroomSceneIndex *>(live.operator->()) : nullptr;
        if (groom) groom->_TestReleaseOwnerCredit();
    }
}

void
UsdGenImagingTestHook::setGroomCommandCapacityForTesting(uint64_t capacity)
{
    UsdGenGroomSceneIndex::_TestSetCommandCapacity(capacity);
}

void
UsdGenImagingTestHook::drainGroomOwnersWithoutFrontend(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    if (groom) groom->_TestDrainOwnerWithoutFrontend();
}

size_t
UsdGenImagingTestHook::pendingGroomPublicationCount(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestPendingPublicationCount() : 0;
}

std::weak_ptr<void const>
UsdGenImagingTestHook::pendingGroomPublicationSnapshotWeak(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestPendingSnapshotWeak() : std::weak_ptr<void const>();
}

std::weak_ptr<void const>
UsdGenImagingTestHook::pendingGroomPublicationTileMapWeak(
    HdSceneIndexBase const &index, SdfPath const &groomPath)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestPendingTileMapWeak(groomPath) : std::weak_ptr<void const>();
}

int64_t
UsdGenImagingTestHook::pendingGroomPublishedGeneration(
    HdSceneIndexBase const &index, SdfPath const &groomPath)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestPendingPublishedGeneration(groomPath) : -1;
}

uint64_t
UsdGenImagingTestHook::groomCaptureCount(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestCaptureCount() : 0;
}

uint64_t
UsdGenImagingTestHook::groomCookCount(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestCookCount() : 0;
}

bool
UsdGenImagingTestHook::staleGroomProgressRejected(
    HdSceneIndexBase const &index, SdfPath const &groomPath)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom && groom->_TestStaleProgressRejected(groomPath);
}

void
UsdGenImagingTestHook::groomOwnerCommandBarrier(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    if (groom) groom->_TestOwnerCommandBarrier();
}

uint64_t
UsdGenImagingTestHook::groomSequenceLastIssued(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestSequenceLastIssued() : 0;
}

uint64_t
UsdGenImagingTestHook::groomSequenceCompletedThrough(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestSequenceCompletedThrough() : 0;
}

uint64_t
UsdGenImagingTestHook::groomSequenceCapacity(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestSequenceCapacity() : 0;
}

size_t
UsdGenImagingTestHook::groomEventHistoryCount(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestEventHistoryCount() : 0;
}

size_t
UsdGenImagingTestHook::groomTombstoneHistoryCount(HdSceneIndexBase const &index)
{
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestTombstoneHistoryCount() : 0;
}

void
UsdGenImagingTestHook::setGroomSequenceCapacityForTesting(uint64_t capacity)
{
    UsdGenGroomSceneIndex::_TestSetSequenceCapacity(capacity);
}

size_t UsdGenImagingTestHook::retainedGroomSceneStateCount() {
    return UsdGenGroomSceneIndex::_TestRetainedSceneStateCount();
}
size_t UsdGenImagingTestHook::liveGroomSceneStateCount() {
    return UsdGenGroomSceneIndex::_TestLiveSceneStateCount();
}
uint64_t UsdGenImagingTestHook::groomRetirementRecordCount() {
    return UsdGenGroomSceneIndex::_TestRetirementRecordCount();
}
void UsdGenImagingTestHook::groomSceneServiceCommandBarrier() {
    UsdGenGroomSceneIndex::_TestSceneServiceBarrier();
}
size_t UsdGenImagingTestHook::groomUsedSessionWeakCount(HdSceneIndexBase const &index) {
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestUsedSessionWeakCount() : 0;
}
size_t UsdGenImagingTestHook::groomUsedSessionLiveUniqueCount(HdSceneIndexBase const &index) {
    auto const *groom = dynamic_cast<UsdGenGroomSceneIndex const *>(&index);
    return groom ? groom->_TestUsedSessionLiveUniqueCount() : 0;
}
void UsdGenImagingTestHook::armGroomFinalDeleterPauseForTesting() {
    UsdGenGroomSceneIndex::_TestArmFinalDeleterPause();
}
void UsdGenImagingTestHook::waitGroomFinalDeleterPauseForTesting() {
    UsdGenGroomSceneIndex::_TestWaitFinalDeleterPause();
}
void UsdGenImagingTestHook::releaseGroomFinalDeleterPauseForTesting() {
    UsdGenGroomSceneIndex::_TestReleaseFinalDeleterPause();
}
void UsdGenImagingTestHook::armGroomDrainWaitForTesting() {
    UsdGenGroomSceneIndex::_TestArmDrainWait();
}
void UsdGenImagingTestHook::waitGroomDrainWaitForTesting() {
    UsdGenGroomSceneIndex::_TestWaitDrainWait();
}
void UsdGenImagingTestHook::releaseGroomDrainWaitForTesting() {
    UsdGenGroomSceneIndex::_TestReleaseDrainWait();
}

void
UsdGenImagingTestHook::Drain()
{
    _HookRegistry().owner.Drain();
}

PXR_NAMESPACE_CLOSE_SCOPE
