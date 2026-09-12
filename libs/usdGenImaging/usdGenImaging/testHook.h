// usdGen imaging — StormSurgery lock-free test hook (test-only surface).
//
// Exposes the atomic published-tile snapshot for adopted groom roots so
// testUsdGenStormSurgery can assert the P0 race contract from the render
// thread: every observation corresponds to one complete generation
// snapshot, never a torn map. Both entry points perform no owner wait: the
// owner publishes an immutable registry snapshot and each groom map is
// atomic_load'ed, exactly like GetPrim. No new public plugin API.
#ifndef USDGEN_IMAGING_TEST_HOOK_H
#define USDGEN_IMAGING_TEST_HOOK_H

#include "pxr/pxr.h"
#include "pxr/base/tf/declarePtrs.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/usd/sdf/path.h"

#include <cstddef>
#include <cstdint>

PXR_NAMESPACE_OPEN_SCOPE

class HdSceneIndexBase;

/// Test-only probe over the live UsdGenGroomSceneIndex registry.
/// `groom` is the adopted UsdGenGroom root path (e.g. `/groomA`).
class UsdGenImagingTestHook
{
public:
    /// Max `generation` stamp across the groom's published tiles (0 when
    /// unknown / unpublished). Atomic snapshot read; safe at any moment,
    /// concurrently with _Republish.
    static int64_t publishedGeneration(SdfPath const &groom);

    /// Published tile count for the groom across live indices.
    static size_t publishedTileCount(SdfPath const &groom);

    /// Number of registry entries in the published immutable snapshot. This
    /// does not promote weak handles; it is test-only observability.
    static size_t registeredIndexCount();

    /// External test/shutdown boundary only. Never invoke from graph work or
    /// a callback; waits until earlier hook-owner mutations have completed.
    static void Drain();

    // Registry wiring, called by the groom index ctor/dtor (NOT test API).
    static void _RegisterIndex(HdSceneIndexBase *index);
    static void _UnregisterIndex(HdSceneIndexBase *index);

private:
    static int64_t _PublishedGenerationOn(
        HdSceneIndexBase const &index, SdfPath const &groom);
    static size_t _PublishedTileCountOn(
        HdSceneIndexBase const &index, SdfPath const &groom);
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // USDGEN_IMAGING_TEST_HOOK_H
