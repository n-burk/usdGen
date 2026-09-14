// Strict, backend-neutral ImageMap resolution for UsdGenGrow length:source.
#ifndef USDGEN_GROW_LENGTH_MAP_H
#define USDGEN_GROW_LENGTH_MAP_H

#include "usdGen/export.h"
#include "usdGen/graphDesc.h"
#include "usdGen/imagePayload.h"

#include <memory>
#include <string>

namespace usdGen {

// Resolves the one supported Grow length map contract: exactly one typed
// usdGen:length:source binding to a valid root/st/raw UsdGenImageMap.  A Grow
// with no map binding succeeds with a null image.  This function deliberately
// does not sample: callers own their per-root UV cardinality checks and retain
// the immutable payload for their full capture/dispatch lifetime.
USDGEN_CORE_API bool ResolveUsdGenGrowLengthImageMap(
    UsdGenGraphDesc const& desc,
    UsdGenNodeDesc const& node,
    std::shared_ptr<const UsdGenImagePayload>* image,
    UsdGenImageSampleOptions* options,
    std::string* reason = nullptr);

} // namespace usdGen

#endif // USDGEN_GROW_LENGTH_MAP_H
