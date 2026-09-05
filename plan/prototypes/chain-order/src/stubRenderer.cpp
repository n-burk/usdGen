//
// A null HdRendererPlugin whose sole purpose is to give the render delegate a
// non-empty display name. hd/renderDelegate.h:584-589 makes
// _SetRendererDisplayName private with `friend class HdRendererPlugin`, and
// hd/rendererPlugin.cpp:76-78 is the only caller, so a delegate constructed
// directly by an application always has an EMPTY display name and
// hd/renderIndex.cpp:208 therefore SKIPS AppendSceneIndicesForRenderer.
//
#include "pxr/pxr.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/type.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"
#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

class UsdGenStubNullRenderDelegate final : public HdRenderDelegate
{
public:
    const TfTokenVector &GetSupportedRprimTypes() const override
    { static TfTokenVector v = { HdPrimTypeTokens->mesh,
                                 HdPrimTypeTokens->basisCurves,
                                 HdPrimTypeTokens->points }; return v; }
    const TfTokenVector &GetSupportedSprimTypes() const override
    { static TfTokenVector v; return v; }
    const TfTokenVector &GetSupportedBprimTypes() const override
    { static TfTokenVector v; return v; }
    HdResourceRegistrySharedPtr GetResourceRegistry() const override
    { static HdResourceRegistrySharedPtr r; return r; }
    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex *, HdRprimCollection const &) override { return nullptr; }
    HdInstancer *CreateInstancer(HdSceneDelegate *, SdfPath const &) override
    { return nullptr; }
    void DestroyInstancer(HdInstancer *) override {}
    HdRprim *CreateRprim(TfToken const &, SdfPath const &) override
    { return nullptr; }
    void DestroyRprim(HdRprim *) override {}
    HdSprim *CreateSprim(TfToken const &, SdfPath const &) override
    { return nullptr; }
    HdSprim *CreateFallbackSprim(TfToken const &) override { return nullptr; }
    void DestroySprim(HdSprim *) override {}
    HdBprim *CreateBprim(TfToken const &, SdfPath const &) override
    { return nullptr; }
    HdBprim *CreateFallbackBprim(TfToken const &) override { return nullptr; }
    void DestroyBprim(HdBprim *) override {}
    void CommitResources(HdChangeTracker *) override {}
};

class UsdGenStubRendererPlugin final : public HdRendererPlugin
{
public:
    bool IsSupported(const HdRendererCreateArgsSchema &,
                     std::string * = nullptr) const override { return true; }
    HdRenderDelegate *CreateRenderDelegate() override
    { return new UsdGenStubNullRenderDelegate(); }
    void DeleteRenderDelegate(HdRenderDelegate *d) override { delete d; }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<UsdGenStubRendererPlugin,
                                     HdRendererPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE
