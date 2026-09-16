// usdGen imaging — private shared decl for the two graph-desc builder TUs.
//
// Split of the S14 dedicated-field set out of usdGenGraphDescBuilder.cpp so
// the stage reference path (usdGenGraphDescBuilderStage.cpp) and the Hydra
// production path (usdGenGraphDescBuilder.cpp) share one definition without
// the B-2-fenced production TU needing any Usd header to reach it.
#ifndef USDGEN_IMAGING_GRAPH_DESC_SHARED_H
#define USDGEN_IMAGING_GRAPH_DESC_SHARED_H

#include "pxr/base/tf/token.h"

#include <string>
#include <unordered_set>

namespace usdGenImaging {

// Attributes that already own a dedicated desc field; everything else
// reaches the engine through params (S14 pull-all).
inline bool
_isDedicated(TfToken const &name)
{
    static std::unordered_set<std::string> const dedicated{
        "usdGen:type", "usdGen:mode",
        "usdGen:enabled", "usdGen:seed",
        "usdGen:references", "usdGen:guides", "usdGen:curves",
        "usdGen:frozen:curves", "usdGen:surface",
        // description-level dedicated fields
        "usdGen:tileTarget", "usdGen:curve:basis",
    };
    // usdGen:look:* lives in UsdGenLookDesc, not params.
    return dedicated.count(name.GetString()) != 0 ||
           name.GetString().rfind("usdGen:look:", 0) == 0;
}

}  // namespace usdGenImaging

#endif  // USDGEN_IMAGING_GRAPH_DESC_SHARED_H
