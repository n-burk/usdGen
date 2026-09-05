//
// G-chain-order stub plugin.
//
// Built TWICE from the same TU, selected by -DSTUB_TAG_ID=<letter>:
//   * a UsdImagingSceneIndexPlugin  (usdImaging-chain level)  -> UsdGenStubUi<TAG>
//   * an HdSceneIndexPlugin         (renderer level)          -> UsdGenStubHd<TAG>
// Both append a recording/tagging HdSingleInputFilteringSceneIndexBase whose
// display name is the class name, so a chain walk can see where it landed.
//
#include "pxr/pxr.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/refPtr.h"
#include "pxr/base/tf/declarePtrs.h"
#include "pxr/base/tf/type.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/usdImaging/usdImaging/sceneIndexPlugin.h"

#include <mutex>
#include <string>
#include <vector>

#ifndef STUB_TAG_ID
#define STUB_TAG_ID X
#endif

#define _CAT2(a, b) a##b
#define _CAT(a, b) _CAT2(a, b)
#define _STR2(a) #a
#define _STR(a) _STR2(a)

#define STUB_UI_CLASS _CAT(UsdGenStubUi, STUB_TAG_ID)
#define STUB_HD_CLASS _CAT(UsdGenStubHd, STUB_TAG_ID)
#define STUB_UI_NAME  _STR(STUB_UI_CLASS)
#define STUB_HD_NAME  _STR(STUB_HD_CLASS)

PXR_NAMESPACE_OPEN_SCOPE

std::mutex &UsdGenStub_LogMutex() { static std::mutex m; return m; }
std::vector<std::string> &UsdGenStub_LogVec()
{ static std::vector<std::string> v; return v; }

static void _AppendLog(const std::string &s)
{
    std::lock_guard<std::mutex> lock(UsdGenStub_LogMutex());
    UsdGenStub_LogVec().push_back(s);
}

class UsdGenStub_RecordingSceneIndex;
TF_DECLARE_REF_PTRS(UsdGenStub_RecordingSceneIndex);

class UsdGenStub_RecordingSceneIndex
    : public HdSingleInputFilteringSceneIndexBase
{
public:
    static UsdGenStub_RecordingSceneIndexRefPtr New(
        const HdSceneIndexBaseRefPtr &input, const std::string &name)
    {
        return TfCreateRefPtr(
            new UsdGenStub_RecordingSceneIndex(input, name));
    }

    HdSceneIndexPrim GetPrim(const SdfPath &p) const override
    { return _GetInputSceneIndex()->GetPrim(p); }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override
    { return _GetInputSceneIndex()->GetChildPrimPaths(p); }

protected:
    UsdGenStub_RecordingSceneIndex(const HdSceneIndexBaseRefPtr &input,
                                   const std::string &name)
        : HdSingleInputFilteringSceneIndexBase(input)
    { SetDisplayName(name); }

    void _PrimsAdded(const HdSceneIndexBase &,
        const HdSceneIndexObserver::AddedPrimEntries &e) override
    { _SendPrimsAdded(e); }
    void _PrimsRemoved(const HdSceneIndexBase &,
        const HdSceneIndexObserver::RemovedPrimEntries &e) override
    { _SendPrimsRemoved(e); }
    void _PrimsDirtied(const HdSceneIndexBase &,
        const HdSceneIndexObserver::DirtiedPrimEntries &e) override
    {
        // With USDGEN_STUB_PROMOTE_PRIMVARS_DIRTY=1, promote any dirty that
        // touches a primvar sub-locator to the BARE "primvars" locator, which
        // is what usdSkelImaging's
        // UsdSkelImagingDataSourceResolvedPointsBasedPrim::_ProcessDirtyLocators
        // (dataSourceResolvedPointsBasedPrim.cpp:1178-1180) tests with
        // Contains() before asking the points-resolving scene index to rebuild
        // the resolved prim (pointsResolvingSceneIndex.cpp:180-181).
        static const bool promote =
            TfGetenvBool("USDGEN_STUB_PROMOTE_PRIMVARS_DIRTY", false);
        if (!promote) { _SendPrimsDirtied(e); return; }
        HdSceneIndexObserver::DirtiedPrimEntries out;
        out.reserve(e.size());
        for (const HdSceneIndexObserver::DirtiedPrimEntry &d : e) {
            HdDataSourceLocatorSet s = d.dirtyLocators;
            if (s.Intersects(HdPrimvarsSchema::GetDefaultLocator())) {
                s.insert(HdPrimvarsSchema::GetDefaultLocator());
            }
            out.push_back({ d.primPath, s });
        }
        _SendPrimsDirtied(out);
    }
};

// -------------------------- UsdImaging level -------------------------- //

class STUB_UI_CLASS final : public UsdImagingSceneIndexPlugin
{
public:
    HdSceneIndexBaseRefPtr AppendSceneIndex(
        HdSceneIndexBaseRefPtr const &inputScene) override
    {
        _AppendLog(std::string("UI-APPEND ") + STUB_UI_NAME +
                   " onTopOf=" + inputScene->GetDisplayName());
        return UsdGenStub_RecordingSceneIndex::New(inputScene, STUB_UI_NAME);
    }
};

TF_REGISTRY_FUNCTION(UsdImagingSceneIndexPlugin)
{
    UsdImagingSceneIndexPlugin::Define<STUB_UI_CLASS>();
}

// -------------------------- renderer level ---------------------------- //

class STUB_HD_CLASS final : public HdSceneIndexPlugin
{
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &) override
    {
        _AppendLog(std::string("HD-APPEND ") + STUB_HD_NAME +
                   " onTopOf=" + inputScene->GetDisplayName());
        return UsdGenStub_RecordingSceneIndex::New(inputScene, STUB_HD_NAME);
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<STUB_HD_CLASS>();
}

PXR_NAMESPACE_CLOSE_SCOPE

extern "C" int _CAT(UsdGenStub_LogSize_, STUB_TAG_ID)()
{
    std::lock_guard<std::mutex> lock(PXR_NS::UsdGenStub_LogMutex());
    return static_cast<int>(PXR_NS::UsdGenStub_LogVec().size());
}
extern "C" const char *_CAT(UsdGenStub_LogEntry_, STUB_TAG_ID)(int i)
{
    std::lock_guard<std::mutex> lock(PXR_NS::UsdGenStub_LogMutex());
    if (i < 0 || i >= (int)PXR_NS::UsdGenStub_LogVec().size()) return "";
    return PXR_NS::UsdGenStub_LogVec()[i].c_str();
}
