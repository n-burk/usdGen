// usdGen prim adapters (ADR §5.1 #3).
//
// One adapter base under FIVE plugInfo entries. primTypeName is a single
// string per plugInfo entry, and UsdGenGroom / UsdGenDescription /
// UsdGenGuideSet derive from UsdGeomImageable rather than UsdGenOperator, so
// includeDerivedPrimTypes cannot reach them from an operator base entry.
// Hence five one-line final subclasses of a single implementation.
#ifndef USDGEN_IMAGING_PRIM_ADAPTER_H
#define USDGEN_IMAGING_PRIM_ADAPTER_H

#include "usdGenImaging/api.h"

#include "pxr/usdImaging/usdImaging/sceneIndexPrimAdapter.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

/// \class UsdGenPrimAdapterBase
///
/// Pass-through adapter: usdGen container/operator prims contribute no
/// subprims of their own; geometry is published by the scene index plugin
/// under the description. M0 implements the pure virtuals minimally; M1+
/// extends this to emit usdGen:* data sources.
class UsdGenPrimAdapterBase : public UsdImagingSceneIndexPrimAdapter
{
public:
    TfTokenVector GetImagingSubprims(UsdPrim const &prim) override
    {
        return TfTokenVector();
    }

    TfToken GetImagingSubprimType(
            UsdPrim const &prim, TfToken const &subprim) override
    {
        return TfToken();
    }

    HdContainerDataSourceHandle GetImagingSubprimData(
            UsdPrim const &prim,
            TfToken const &subprim,
            const UsdImagingDataSourceStageGlobals &stageGlobals) override
    {
        return nullptr;
    }

    HdDataSourceLocatorSet InvalidateImagingSubprim(
            UsdPrim const &prim,
            TfToken const &subprim,
            TfTokenVector const &properties,
            UsdImagingPropertyInvalidationType invalidationType) override
    {
        return HdDataSourceLocatorSet();
    }
};

/// Five one-line final subclasses, one per concrete prim type that must be
/// individually claimed by the adapter registry (ADR §2.3).
class UsdGenGroomAdapter final : public UsdGenPrimAdapterBase {};
class UsdGenDescriptionAdapter final : public UsdGenPrimAdapterBase {};
class UsdGenOperatorAdapter final : public UsdGenPrimAdapterBase {};
class UsdGenMapAdapter final : public UsdGenPrimAdapterBase {};
class UsdGenGuideSetAdapter final : public UsdGenPrimAdapterBase {};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // USDGEN_IMAGING_PRIM_ADAPTER_H
