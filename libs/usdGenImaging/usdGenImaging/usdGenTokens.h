// usdGen imaging — shared tokens for the published Hydra payload.
//
// Single definition shared by primAdapter.cpp (schema mappings container)
// and usdGenRestApiDataSource.cpp / the scene index (published data root).
// Plan: plan/06-imaging.md §3.5 (the "usdGen" container token).
//
// Canonical home is the GLOBAL usdGenImaging namespace (where the imaging
// free functions live, e.g. usdGenRestApiDataSource.h). The router and the
// prim adapter sit inside the pxr namespace (TfType/registry visibility), so
// the names are additionally made visible there via using-declarations —
// one function, two spellings, no double-pxr scoping surprises.
#ifndef USDGEN_IMAGING_TOKENS_H
#define USDGEN_IMAGING_TOKENS_H

#include "pxr/base/tf/token.h"
#include "pxr/pxr.h"

namespace usdGenImaging {

/// Container token for all usdGen-published Hydra data ("usdGen").
inline PXR_NS::TfToken const &UsdGenContainerToken()
{
    static TfToken const token("usdGen");
    return token;
}

/// Namespace of published tile/guide prims under a groom description
/// (06-imaging.md §3.4.1; authored prims of the same name are hidden).
inline PXR_NS::TfToken const &UsdGenRenderNamespaceToken()
{
    static TfToken const token("__usdGenRender");
    return token;
}

}  // namespace usdGenImaging

#if PXR_USE_NAMESPACES
namespace PXR_INTERNAL_NS {
namespace usdGenImaging {

using ::usdGenImaging::UsdGenContainerToken;
using ::usdGenImaging::UsdGenRenderNamespaceToken;

}  // namespace usdGenImaging
}  // namespace PXR_INTERNAL_NS
#endif

#endif  // USDGEN_IMAGING_TOKENS_H
