// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

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
