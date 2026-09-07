// usdGenGroom renderer-level scene index plugin.
#include "usdGenImaging/groomSceneIndexPlugin.h"

#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/token.h"

#include "usdGenImaging/usdGenEnable.h"

PXR_NAMESPACE_OPEN_SCOPE

// Kill switch (ADR §5.1): the registry consults _IsEnabled per append, so a
// disabled plugin returns its input unchanged and drops out of the chain
// without editing plugInfo. Shared with the metadata plugin via usdGenEnable.h
// (single TF_DEFINE_ENV_SETTING lives in this translation unit).
TF_DEFINE_ENV_SETTING(
    USDGEN_ENABLE, true,
    "Enable the usdGen scene index plugins (false disables both the groom "
    "resolution plugin and the metadata-only plugin).");

TF_REGISTRY_FUNCTION(TfType) {
    HdSceneIndexPluginRegistry::Define<UsdGenGroomSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers.GetString(),
        TfToken("UsdGenGroomSceneIndexPlugin"),
        /*inputArgs      =*/ nullptr,
        /*insertionPhase =*/ 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

HdSceneIndexBaseRefPtr
UsdGenGroomSceneIndexPlugin::_AppendSceneIndex(
    const std::string &renderInstanceId,
    const HdSceneIndexBaseRefPtr &inputScene,
    const HdContainerDataSourceHandle &inputArgs)
{
    TF_UNUSED(renderInstanceId);
    TF_UNUSED(inputArgs);
    // M0: pass-through node so the chain position is observable. M1 wraps the
    // input in the real UsdGenGroomSceneIndex (evaluator + tile publisher).
    return UsdGenGroomSceneIndex::New(inputScene);
}

bool
UsdGenGroomSceneIndexPlugin::_IsEnabled(
    const HdContainerDataSourceHandle &inputArgs) const
{
    TF_UNUSED(inputArgs);
    return TfGetEnvSetting(USDGEN_ENABLE);
}

PXR_NAMESPACE_CLOSE_SCOPE
