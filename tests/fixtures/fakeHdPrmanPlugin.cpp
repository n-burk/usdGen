// Fake hdPrman motion-blur fixture plugin (sol S-3/X-2).
//
// A minimal renderer-level HdSceneIndexPlugin whose plugInfo carries the tag
// "hdPrman:motionBlur". It exists so testUsdGenChainOrder can prove that the
// usdGen plugInfo ordering contract ("ordering.before: [hdPrman:motionBlur]")
// places UsdGenGroomSceneIndex strictly upstream of an hdPrman-flavoured
// renderer node — without RenderMan being installed.
//
// The appended node is a pass-through filter with a fixed display name so the
// chain walk can identify it.
#include "pxr/pxr.h"
#include "pxr/base/tf/declarePtrs.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DECLARE_REF_PTRS(FakeHdPrmanMotionBlurSceneIndex);

class FakeHdPrmanMotionBlurSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase
{
public:
    static HdSceneIndexBaseRefPtr
    New(const HdSceneIndexBaseRefPtr &inputScene)
    {
        auto si = TfCreateRefPtr(new FakeHdPrmanMotionBlurSceneIndex(inputScene));
        si->SetDisplayName("FakeHdPrmanMotionBlurSceneIndex");
        return si;
    }

    HdSceneIndexPrim
    GetPrim(const SdfPath &primPath) const override
    {
        return _GetInputSceneIndex()->GetPrim(primPath);
    }
    SdfPathVector
    GetChildPrimPaths(const SdfPath &primPath) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    }

protected:
    explicit FakeHdPrmanMotionBlurSceneIndex(
        const HdSceneIndexBaseRefPtr &inputScene)
        : HdSingleInputFilteringSceneIndexBase(inputScene)
    {}

    void _PrimsAdded(
            const HdSceneIndexBase &sender,
            const HdSceneIndexObserver::AddedPrimEntries &entries) override
    {
        _SendPrimsAdded(entries);
    }
    void _PrimsRemoved(
            const HdSceneIndexBase &sender,
            const HdSceneIndexObserver::RemovedPrimEntries &entries) override
    {
        _SendPrimsRemoved(entries);
    }
    void _PrimsDirtied(
            const HdSceneIndexBase &sender,
            const HdSceneIndexObserver::DirtiedPrimEntries &entries) override
    {
        _SendPrimsDirtied(entries);
    }
};

class FakeHdPrmanMotionBlurSceneIndexPlugin final
    : public HdSceneIndexPlugin
{
protected:
    HdSceneIndexBaseRefPtr
    _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &inputArgs) override
    {
        TF_UNUSED(inputArgs);
        return FakeHdPrmanMotionBlurSceneIndex::New(inputScene);
    }

    bool
    _IsEnabled(const HdContainerDataSourceHandle &inputArgs) const override
    {
        TF_UNUSED(inputArgs);
        return true;
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<FakeHdPrmanMotionBlurSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
        TfToken("FakeHdPrmanMotionBlurSceneIndexPlugin"),
        /*inputArgs      =*/ nullptr,
        /*insertionPhase =*/ 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

PXR_NAMESPACE_CLOSE_SCOPE
