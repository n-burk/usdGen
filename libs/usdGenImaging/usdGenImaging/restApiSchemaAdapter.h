// UsdGenRestAPIAdapter: API schema adapter for UsdGenRestAPI (S12).
// M0: registration only; data sourcing arrives with the RestAPI work in M2.
#ifndef USDGEN_IMAGING_REST_API_SCHEMA_ADAPTER_H
#define USDGEN_IMAGING_REST_API_SCHEMA_ADAPTER_H

#include "usdGenImaging/api.h"

#include "pxr/usdImaging/usdImaging/apiSchemaAdapter.h"
#include "pxr/pxr.h"

PXR_NAMESPACE_OPEN_SCOPE

class UsdGenRestAPIAdapter final
    : public UsdImagingAPISchemaAdapter
{
public:
    UsdGenRestAPIAdapter() = default;
    ~UsdGenRestAPIAdapter() override = default;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // USDGEN_IMAGING_REST_API_SCHEMA_ADAPTER_H
