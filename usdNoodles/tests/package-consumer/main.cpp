// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

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
