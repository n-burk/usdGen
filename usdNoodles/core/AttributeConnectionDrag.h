// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#ifndef NOODLES_CORE_ATTRIBUTE_CONNECTION_DRAG_H
#define NOODLES_CORE_ATTRIBUTE_CONNECTION_DRAG_H

#include "core/NodeData.h"
#include "core/api.h"
#include "undo/Command.h"

#include <functional>
#include <optional>
#include <string>

namespace noodles {

/// A stable graph/UI endpoint. `propertyName` is the exact authored property
/// name (including its namespace); the controller never rewrites it.
struct NOODLES_API AttributeEndpoint {
  std::string nodeId;
  std::string primPath;
  std::string propertyName;
  bool isOutput = false;
  // Host-resolved port center in graph space. Appended so existing aggregate
  // initializers retain their old values and default to the origin.
  Vec2d portPosition{0.0, 0.0};

  bool isValid() const;
  /// USD adapters can consume this opaque canonical property path directly.
  std::string propertyPath() const;
};

struct NOODLES_API AttributeConnectionRequest {
  AttributeEndpoint source;
  AttributeEndpoint target;
  LinkData link;
};

/// Renderer/host-neutral drag state for USD-style attribute connections.
/// The author callback owns the actual model edit and optional undo command;
/// this controller only produces the normalized LinkData preview/intent.
///
/// Host event mapping is deliberately minimal: pointer-down on a port calls
/// Begin, pointer-move supplies the optional hover endpoint to Update,
/// pointer-release calls Drop, and escape/lost-capture calls Cancel.  A host's
/// selected authoring grain (for example a USD evaluation domain) belongs in
/// the Author callback capture, not in this renderer-neutral controller.
class NOODLES_API AttributeConnectionDrag {
 public:
  using Validator = std::function<bool(const AttributeConnectionRequest&)>;
  /// Returns an unapplied undo command. Drop executes it once, then transfers
  /// it to the existing undo stack for redo/undo ownership.
  using Author = std::function<CommandPtr(const AttributeConnectionRequest&)>;

  struct Callbacks {
    Validator validate;
    Author author;
  };

  explicit AttributeConnectionDrag(Callbacks callbacks = {});

  /// Starts from either endpoint side. A later drop normalizes output -> input,
  /// so dragging an operator input back onto an expression output is supported.
  bool Begin(AttributeEndpoint endpoint);
  /// Replaces transient hover only, retaining the current cursor coordinate.
  /// This never edits graph/host data.
  void Update(std::optional<AttributeEndpoint> hover);
  /// Updates the live cursor and optional hover. With no compatible hover,
  /// Preview returns a dangling LinkData that a host can pass to
  /// LinkRenderManager::renderLinks as its temporary drag noodle.
  void Update(Vec2d cursor, std::optional<AttributeEndpoint> hover = std::nullopt);
  /// Returns a renderable data-link intent for a compatible current hover.
  std::optional<LinkData> Preview() const;
  /// Invokes validate once and author once only for a compatible, validated
  /// drop. Invalid/cancelled drops make no authoring call and always end drag.
  bool Drop();
  void Cancel();

  bool active() const { return _source.has_value(); }
  std::optional<AttributeEndpoint> source() const { return _source; }
  std::optional<AttributeEndpoint> hover() const { return _hover; }

 private:
  static std::optional<AttributeConnectionRequest> _Normalize(
      AttributeEndpoint const& first, AttributeEndpoint const& second);

  Callbacks _callbacks;
  std::optional<AttributeEndpoint> _source;
  std::optional<AttributeEndpoint> _hover;
  Vec2d _cursor{0.0, 0.0};
};

} // namespace noodles

#endif // NOODLES_CORE_ATTRIBUTE_CONNECTION_DRAG_H
