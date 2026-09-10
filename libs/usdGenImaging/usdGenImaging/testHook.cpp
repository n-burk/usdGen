// usdGen imaging — StormSurgery lock-free test hook (test-only surface).
//
// UsdGenImagingTestHook exposes the atomic published-tile snapshot for the
// groom roots adopted by live UsdGenGroomSceneIndex instances, so
// testUsdGenStormSurgery can assert the P0 race contract from the render
// thread: every observation corresponds to one complete generation
// snapshot, never a torn map. Both entry points are lock-free on the read
// path (registry mutex only; the per-groom published map is atomic_load'ed,
// exactly like GetPrim).
//
// Lives in its own translation unit (NOT the plugin registration TU) so the
// symbols link into libusdGenImaging for testUsdGenStormSurgery
// (usdGenTestUtilsHd -> usdGenImaging). No new public plugin API.
#include "usdGenImaging/testHook.h"

#include "usdGenImaging/groomSceneIndexPlugin.h"

#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLocator.h"
#include "pxr/usd/sdf/path.h"

#include <mutex>
#include <algorithm>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// Process-wide registry of live groom indices, populated by the index
// ctor/dtor. Guarded by a plain mutex; hook readers copy the weak handles
// out and promote them without holding it.
std::mutex &_HookRegistryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<HdSceneIndexBasePtr> &_HookRegistry()
{
    static std::vector<HdSceneIndexBasePtr> registry;
    return registry;
}

}  // namespace

void
UsdGenImagingTestHook::_RegisterIndex(HdSceneIndexBase *index)
{
    std::lock_guard<std::mutex> lock(_HookRegistryMutex());
    auto &registry = _HookRegistry();
    for (auto const &weak : registry) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(weak);
        if (live && live.operator->() == index) return;
    }
    registry.push_back(TfCreateWeakPtr(index));
}

void
UsdGenImagingTestHook::_UnregisterIndex(HdSceneIndexBase *index)
{
    std::lock_guard<std::mutex> lock(_HookRegistryMutex());
    auto &registry = _HookRegistry();
    registry.erase(
        std::remove_if(registry.begin(), registry.end(),
                       [index](HdSceneIndexBasePtr const &weak) {
                           HdSceneIndexBaseRefPtr live =
                               TfCreateRefPtrFromProtectedWeakPtr(weak);
                           return !live || live.operator->() == index;
                       }),
        registry.end());
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
    std::vector<HdSceneIndexBasePtr> weaks;
    {
        std::lock_guard<std::mutex> lock(_HookRegistryMutex());
        weaks = _HookRegistry();
    }
    int64_t best = -1;
    for (auto const &weak : weaks) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(weak);
        if (!live) continue;
        best = std::max(best, _PublishedGenerationOn(*live, groom));
    }
    return best < 0 ? 0 : best;
}

size_t
UsdGenImagingTestHook::publishedTileCount(SdfPath const &groom)
{
    std::vector<HdSceneIndexBasePtr> weaks;
    {
        std::lock_guard<std::mutex> lock(_HookRegistryMutex());
        weaks = _HookRegistry();
    }
    size_t total = 0;
    for (auto const &weak : weaks) {
        HdSceneIndexBaseRefPtr live =
            TfCreateRefPtrFromProtectedWeakPtr(weak);
        if (!live) continue;
        total += _PublishedTileCountOn(*live, groom);
    }
    return total;
}

PXR_NAMESPACE_CLOSE_SCOPE
