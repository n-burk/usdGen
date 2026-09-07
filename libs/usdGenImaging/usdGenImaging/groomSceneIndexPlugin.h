// Renderer-level scene index plugin for usdGen groom generation.
// Registered at phase 0 / InsertionOrderAtEnd so it lands after the whole
// UsdImaging chain (incl. UsdSkel) and before every Storm/hdPrman plugin
// (ADR S1/S2, §2.1). M0: pass-through filter node so the chain position is
// observable in the vertical demo; M1 replaces it with the real generator
// (tile publisher + dirty router).
#ifndef USDGEN_IMAGING_GROOM_SCENE_INDEX_PLUGIN_H
#define USDGEN_IMAGING_GROOM_SCENE_INDEX_PLUGIN_H

#include "usdGenImaging/api.h"

#include "pxr/imaging/hd/sceneIndexPlugin.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"

#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

/// \class UsdGenGroomSceneIndex
///
/// M0: identity pass-through filter. It exists so the plugin registers a real
/// node in the render index chain (observable in the chain dump) while the
/// M1 evaluator/publisher replaces it. Notice forwarding is preserved so
/// downstream observers keep working when it is in the chain.
class UsdGenGroomSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase
{
public:
    static HdSceneIndexBaseRefPtr New(
        const HdSceneIndexBaseRefPtr &inputScene)
    {
        return TfCreateRefPtr(new UsdGenGroomSceneIndex(inputScene));
    }

protected:
    explicit UsdGenGroomSceneIndex(
        const HdSceneIndexBaseRefPtr &inputScene)
        : HdSingleInputFilteringSceneIndexBase(inputScene)
    {
    }

    HdSceneIndexPrim GetPrim(const SdfPath &primPath) const override
    {
        return _GetInputSceneIndex()->GetPrim(primPath);
    }
    SdfPathVector GetChildPrimPaths(const SdfPath &primPath) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    }

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

/// \class UsdGenGroomSceneIndexPlugin
class UsdGenGroomSceneIndexPlugin final : public HdSceneIndexPlugin
{
public:
    UsdGenGroomSceneIndexPlugin() = default;
    ~UsdGenGroomSceneIndexPlugin() override = default;

protected:
    // The three-arg overload wins when both are overridden
    // (pxr/imaging/hd/sceneIndexPlugin.h).
    HdSceneIndexBaseRefPtr
    _AppendSceneIndex(
        const std::string &renderInstanceId,
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &inputArgs) override;

    bool
    _IsEnabled(
        const HdContainerDataSourceHandle &inputArgs) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // USDGEN_IMAGING_GROOM_SCENE_INDEX_PLUGIN_H
