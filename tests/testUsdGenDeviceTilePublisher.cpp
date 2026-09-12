#include "usdGenImaging/deviceTilePublisher.h"
#include "pxr/imaging/hd/basisCurvesSchema.h"
#include "pxr/imaging/hd/basisCurvesTopologySchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/dependencySchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

static bool CheckMaterialVisibilityMatrix(
    usdGenImaging::UsdGenDeviceTileMetadata const& base,
    HdDataSourceBaseHandle const& provider) {
    if (base.materialPath.IsEmpty()) return false;

    auto checkBinding = [&](HdMaterialBindingSchema const& binding,
                            bool expectPath) -> bool {
        auto path = binding.GetPath();
        return expectPath ? path && path->GetTypedValue(0) == base.materialPath
                          : !path;
    };
    auto checkVisibility = [](HdContainerDataSourceHandle const& prim,
                              bool expected) -> bool {
        auto visibility = HdVisibilitySchema::GetFromParent(prim).GetVisibility();
        return visibility && visibility->GetTypedValue(0) == expected;
    };

    std::vector<TfToken> const purposes{
        TfToken(), TfToken("allPurpose"), TfToken("preview"), TfToken("full"),
        TfToken("custom")};
    for (TfToken const& purpose : purposes) {
        auto metadata = base;
        metadata.materialPurpose = purpose;
        auto prim = usdGenImaging::BuildDeviceTileDataSource(metadata, provider);
        auto bindings = HdMaterialBindingsSchema::GetFromParent(prim);
        bool const defaultPurpose = purpose.IsEmpty() || purpose == TfToken("allPurpose");
        if (!prim || !bindings.IsDefined() ||
            !checkBinding(bindings.GetMaterialBinding(), defaultPurpose) ||
            !checkBinding(bindings.GetMaterialBinding(purpose), true) ||
            !checkBinding(bindings.GetMaterialBinding(TfToken("unmatched")), defaultPurpose))
            return false;
    }

    auto inherited = base;
    inherited.visibility = TfToken("inherited");
    auto inheritedPrim = usdGenImaging::BuildDeviceTileDataSource(inherited, provider);
    auto invisible = base;
    invisible.visibility = TfToken("invisible");
    auto invisiblePrim = usdGenImaging::BuildDeviceTileDataSource(invisible, provider);
    return inheritedPrim && invisiblePrim &&
        checkVisibility(inheritedPrim, true) && checkVisibility(invisiblePrim, false);
}

int main() {
    usdGenImaging::UsdGenDeviceTileMetadata m;
    m.primPath = SdfPath("/g/__usdGenRender/tile_0000");
    m.curveType = TfToken("cubic");
    m.curveBasis = TfToken("bezier");
    m.curveWrap = TfToken("pinned");
    m.generation = 7;
    m.extentMin = GfVec3d(-1.0);
    m.extentMax = GfVec3d(1.0);
    HdDataSourceBaseHandle provider(
        HdRetainedTypedSampledDataSource<int>::New(1));
    auto prim = usdGenImaging::BuildDeviceTileDataSource(m, provider);
    std::weak_ptr<HdDataSourceBase> providerWeak = provider;
    auto curves = HdBasisCurvesSchema::GetFromParent(prim);
    auto topology = curves.GetTopology();
    if (!prim || !curves.IsDefined() || !topology.IsDefined() ||
        !topology.GetType() ||
        topology.GetType()->GetTypedValue(0) != m.curveType ||
        !topology.GetBasis() || topology.GetBasis()->GetTypedValue(0) != m.curveBasis ||
        !topology.GetWrap() || topology.GetWrap()->GetTypedValue(0) != m.curveWrap ||
        topology.GetCurveVertexCounts() || topology.GetCurveIndices() ||
        prim->Get(TfToken("primvars")) ||
        !prim->Get(TfToken("hdStBasisCurvesGpu"))) {
        std::fprintf(stderr, "device tile publication failed\n");
        return 1;
    }
    HdDataSourceBaseHandle retainedProvider = provider;
    provider.reset();
    if (!prim->Get(TfToken("hdStBasisCurvesGpu")) || !retainedProvider ||
        providerWeak.expired()) return 1;
    retainedProvider.reset();
    if (providerWeak.expired()) return 1;
    retainedProvider = providerWeak.lock();
    if (!retainedProvider) return 1;
    m.materialPath = SdfPath("/Looks/Hair");
    m.primOrigin = SdfPath("/source");
    m.dependencySurface = SdfPath("/surface");
    m.purpose = TfToken("render");
    m.visibility = TfToken("inherited");
    prim = usdGenImaging::BuildDeviceTileDataSource(m, retainedProvider);
    auto bindings = HdMaterialBindingsSchema::GetFromParent(prim);
    auto visibility = HdVisibilitySchema::GetFromParent(prim);
    auto xform = HdXformSchema::GetFromParent(prim);
    // usdGen's application-facing "allPurpose" spelling maps to Hydra's
    // empty-token default binding child.
    auto materialPath = bindings.GetMaterialBinding().GetPath();
    if (!prim || !bindings.IsDefined() || !materialPath ||
        materialPath->GetTypedValue(0) != m.materialPath ||
        !visibility.IsDefined() || !visibility.GetVisibility() ||
        !xform.IsDefined() || !xform.GetMatrix() || !xform.GetResetXformStack()) return 1;
    if (xform.GetResetXformStack()->GetTypedValue(0) != true ||
        xform.GetMatrix()->GetTypedValue(0) != m.xform ||
        !visibility.GetVisibility()->GetTypedValue(0)) return 1;
    if (!CheckMaterialVisibilityMatrix(m, retainedProvider)) return 1;
    auto origin = prim->Get(TfToken("primOrigin"));
    auto deps = prim->Get(TfToken("__dependencies"));
    if (!origin || !deps) return 1;
    auto originContainer = HdContainerDataSource::Cast(origin);
    auto depsContainer = HdContainerDataSource::Cast(deps);
    if (!originContainer || !depsContainer) return 1;
    auto originScene = originContainer->Get(TfToken("scenePath"));
    auto depEntry = depsContainer->Get(
        TfToken("usdGenSurface"));
    auto depContainer = HdContainerDataSource::Cast(depEntry);
    if (!depContainer) return 1;
    HdDependencySchema dep(depContainer);
    if (!originScene || !HdPathDataSource::Cast(originScene) ||
        HdPathDataSource::Cast(originScene)->GetTypedValue(0) != m.primOrigin ||
        !depEntry || !dep.GetDependedOnPrimPath() ||
        dep.GetDependedOnPrimPath()->GetTypedValue(0) != m.dependencySurface ||
        !dep.GetDependedOnDataSourceLocator() ||
        dep.GetDependedOnDataSourceLocator()->GetTypedValue(0) !=
            HdDataSourceLocator(TfToken("primvars"), TfToken("points")) ||
        !dep.GetAffectedDataSourceLocator() ||
        dep.GetAffectedDataSourceLocator()->GetTypedValue(0) !=
            HdDataSourceLocator(TfToken("hdStBasisCurvesGpu"))) return 1;
    if (usdGenImaging::BuildDeviceTileDataSource(m, {})) return 1;
    auto invalid = m;
    invalid.primPath = SdfPath("relative");
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    invalid = m;
    invalid.xform[0][0] = std::numeric_limits<double>::quiet_NaN();
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    invalid = m;
    invalid.curveType = TfToken("not-a-curve-type");
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    invalid = m;
    invalid.curveBasis = TfToken("not-a-basis");
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    invalid = m;
    invalid.curveWrap = TfToken("not-a-wrap");
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    invalid = m;
    invalid.materialPath = SdfPath("/");
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    auto emptyPurpose = m;
    emptyPurpose.materialPurpose = TfToken();
    auto emptyPurposePrim = usdGenImaging::BuildDeviceTileDataSource(
        emptyPurpose, retainedProvider);
    auto emptyPurposePath = HdMaterialBindingsSchema::GetFromParent(
        emptyPurposePrim).GetMaterialBinding().GetPath();
    if (!emptyPurposePrim || !emptyPurposePath ||
        emptyPurposePath->GetTypedValue(0) != m.materialPath) return 1;
    auto previewPurpose = m;
    previewPurpose.materialPurpose = TfToken("preview");
    auto previewPurposePrim = usdGenImaging::BuildDeviceTileDataSource(
        previewPurpose, retainedProvider);
    auto previewPurposePath = HdMaterialBindingsSchema::GetFromParent(
        previewPurposePrim).GetMaterialBinding(previewPurpose.materialPurpose).GetPath();
    if (!previewPurposePrim || !previewPurposePath ||
        previewPurposePath->GetTypedValue(0) != m.materialPath) return 1;
    invalid = m;
    invalid.refineLevel = -1;
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    invalid = m;
    invalid.extentMin[0] = std::numeric_limits<double>::quiet_NaN();
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    invalid = m;
    invalid.extentMax[1] = -2;
    if (usdGenImaging::BuildDeviceTileDataSource(invalid, retainedProvider)) return 1;
    auto accepted = m;
    accepted.curveType = TfToken("linear");
    accepted.curveBasis = TfToken("linear");
    if (!usdGenImaging::BuildDeviceTileDataSource(accepted, retainedProvider)) return 1;
    accepted.curveBasis = TfToken("bezier");
    if (!usdGenImaging::BuildDeviceTileDataSource(accepted, retainedProvider)) return 1;
    accepted.curveType = TfToken("cubic");
    accepted.curveBasis = TfToken("centripetalCatmullRom");
    if (!usdGenImaging::BuildDeviceTileDataSource(accepted, retainedProvider)) return 1;
    emptyPurposePrim.reset();
    previewPurposePrim.reset();
    retainedProvider.reset();
    prim = {};
    if (!providerWeak.expired()) return 1;
    std::puts("Device tile publisher: PASS");
    return 0;
}
