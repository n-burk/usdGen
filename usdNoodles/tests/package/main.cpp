// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "core/RenderConfig.h"
#include "undo/NoodlesUndoManager.h"

#include <string>

int main() {
  noodles::RenderConfig config;
  config.insetWholePrimRelationshipTargets = false;
  if (config.getBool("insetWholePrimRelationshipTargets", true)) {
    return 1;
  }

  auto& undo = noodles::NoodlesUndoManager::instance();
  undo.reset();
  if (undo.maxStackDepth() != 100) {
    return 2;
  }

  return std::string(NOODLES_PACKAGE_ASSET_DIR).empty() ? 3 : 0;
}
