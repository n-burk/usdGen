// Copyright (c) 2026 Nick Burkard
// SPDX-License-Identifier: MIT

#include "core/AttributeConnectionDrag.h"

#include "undo/NoodlesUndoManager.h"

#include <utility>

namespace noodles {

bool AttributeEndpoint::isValid() const {
  return !nodeId.empty() && !primPath.empty() && !propertyName.empty();
}

std::string AttributeEndpoint::propertyPath() const {
  return isValid() ? primPath + "." + propertyName : std::string{};
}

AttributeConnectionDrag::AttributeConnectionDrag(Callbacks callbacks)
    : _callbacks(std::move(callbacks)) {}

bool AttributeConnectionDrag::Begin(AttributeEndpoint endpoint) {
  Cancel();
  if (!endpoint.isValid()) {
    return false;
  }
  _cursor = endpoint.portPosition;
  _source = std::move(endpoint);
  return true;
}

void AttributeConnectionDrag::Update(std::optional<AttributeEndpoint> hover) {
  if (!_source) {
    return;
  }
  _hover = std::move(hover);
}

void AttributeConnectionDrag::Update(
    Vec2d cursor, std::optional<AttributeEndpoint> hover) {
  if (!_source) {
    return;
  }
  _cursor = cursor;
  _hover = std::move(hover);
}

std::optional<AttributeConnectionRequest> AttributeConnectionDrag::_Normalize(
    AttributeEndpoint const& first, AttributeEndpoint const& second) {
  if (!first.isValid() || !second.isValid() || first.isOutput == second.isOutput) {
    return std::nullopt;
  }
  AttributeEndpoint const& output = first.isOutput ? first : second;
  AttributeEndpoint const& input = first.isOutput ? second : first;
  AttributeConnectionRequest request;
  request.source = output;
  request.target = input;
  request.link.sourceNodeId = output.nodeId;
  request.link.sourcePort = output.propertyName;
  request.link.targetNodeId = input.nodeId;
  request.link.targetPort = input.propertyName;
  request.link.targetPropertyName = input.propertyName;
  request.link.isRelationship = false;
  request.link.start = output.portPosition;
  request.link.end = input.portPosition;
  return request;
}

std::optional<LinkData> AttributeConnectionDrag::Preview() const {
  if (!_source) {
    return std::nullopt;
  }
  if (_hover) {
    auto request = _Normalize(*_source, *_hover);
    if (request) {
      return request->link;
    }
  }

  // Keep normal LinkData orientation even when the user started at an input:
  // unknown output is the cursor/start and the known input is the fixed end.
  LinkData link;
  link.isDangling = true;
  link.isRelationship = false;
  if (_source->isOutput) {
    link.sourceNodeId = _source->nodeId;
    link.sourcePort = _source->propertyName;
    link.start = _source->portPosition;
    link.end = _cursor;
    link.danglingDirection = "output";
  } else {
    link.targetNodeId = _source->nodeId;
    link.targetPort = _source->propertyName;
    link.targetPropertyName = _source->propertyName;
    link.start = _cursor;
    link.end = _source->portPosition;
    link.danglingDirection = "input";
  }
  return link;
}

bool AttributeConnectionDrag::Drop() {
  std::optional<AttributeConnectionRequest> request =
      _source && _hover ? _Normalize(*_source, *_hover) : std::nullopt;
  // Clear before callbacks: authoring may synchronously start another drag.
  Cancel();
  if (!request) {
    return false;
  }
  if (_callbacks.validate && !_callbacks.validate(*request)) {
    return false;
  }
  if (!_callbacks.author) {
    return false;
  }
  CommandPtr command = _callbacks.author(*request);
  if (!command) {
    return false;
  }
  command->execute();
  NoodlesUndoManager::instance().pushCommand(std::move(command));
  return true;
}

void AttributeConnectionDrag::Cancel() {
  _source.reset();
  _hover.reset();
}

} // namespace noodles
