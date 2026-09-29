// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#include "core/AttributeConnectionDrag.h"

#ifdef NOODLES_PACKAGE_CONSUMER_WITH_USD
#include "usd/ExpressionConnections.h"
#endif

#include <string>

int main() {
  noodles::AttributeEndpoint endpoint{
      "node", "/Description/Expression", "outputs:result", true};
  if (endpoint.propertyPath() != "/Description/Expression.outputs:result") {
    return 1;
  }
  if (std::string(NOODLES_PACKAGE_ASSET_DIR).empty()) {
    return 2;
  }
#ifdef NOODLES_PACKAGE_CONSUMER_WITH_USD
  if (!noodles::usd::GetExpressionConnections(pxr::UsdPrim{}).empty()) {
    return 3;
  }
#endif
  return 0;
}
