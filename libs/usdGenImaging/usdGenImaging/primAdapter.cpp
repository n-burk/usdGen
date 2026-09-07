// usdGen prim adapters (ADR §5.1 #3).
//
// One adapter base under five plugInfo entries. primTypeName is a single
// string per plugInfo entry, and UsdGenGroom / UsdGenDescription /
// UsdGenGuideSet derive from UsdGeomImageable rather than from
// UsdGenOperator, so includeDerivedPrimTypes on an operator-base entry cannot
// reach them. Hence five one-line final subclasses of a single
// implementation (ADR §2.3, 06-imaging.md §2.1).
//
// M0: all five are pass-through. They contribute no subprims of their own;
// generated tile geometry is published under the description by the scene
// index (M1). This is enough for plugInfo type discovery, adapter lookup,
// and the T1 suite.
#include "usdGenImaging/primAdapter.h"

#include "pxr/base/tf/type.h"

PXR_NAMESPACE_OPEN_SCOPE

#define USDGEN_DEFINE_PRIM_ADAPTER(AdapterType)                              \
    TF_REGISTRY_FUNCTION(TfType) {                                           \
        TfType t = TfType::Define<                                           \
            AdapterType,                                                    \
            TfType::Bases<UsdImagingSceneIndexPrimAdapter>>();               \
        t.SetFactory<UsdImagingPrimAdapterFactory<AdapterType>>();           \
    }

USDGEN_DEFINE_PRIM_ADAPTER(UsdGenGroomAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenDescriptionAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenOperatorAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenMapAdapter)
USDGEN_DEFINE_PRIM_ADAPTER(UsdGenGuideSetAdapter)

#undef USDGEN_DEFINE_PRIM_ADAPTER

PXR_NAMESPACE_CLOSE_SCOPE
