// UsdGenRestAPIAdapter: API schema adapter for UsdGenRestAPI (S12, ADR §5.1 #3).
// M0: registration only; rest-point data sourcing lands in M2 with the
// freeze/sculpt work.
#include "usdGenImaging/restApiSchemaAdapter.h"

#include "pxr/base/tf/type.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    TfType t = TfType::Define<UsdGenRestAPIAdapter,
                              TfType::Bases<UsdImagingAPISchemaAdapter>>();
    t.SetFactory<UsdImagingAPISchemaAdapterFactory<UsdGenRestAPIAdapter>>();
}

PXR_NAMESPACE_CLOSE_SCOPE
