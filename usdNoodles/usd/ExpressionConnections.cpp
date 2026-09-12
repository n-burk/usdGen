// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "usd/ExpressionConnections.h"

#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/tf/weakPtr.h"
#include "pxr/base/vt/dictionary.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/usd/sdf/valueTypeName.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usd/editTarget.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace noodles::usd {
namespace {

TfToken const kExpressionType("UsdGenExpression");
TfToken const kOperatorType("UsdGenOperator");
TfToken const kDescriptionType("UsdGenDescription");
TfToken const kExpressionsName("Expressions");
TfToken const kOpsName("Ops");
TfToken const kEvaluationKey("usdGen:evaluation");

UsdStageRefPtr LockStage(const UsdStageWeakPtr& stage) {
  // UsdStageWeakPtr is a TfWeakPtr, not std::weak_ptr.  The protected
  // conversion obtains a strong owner-thread reference without dereferencing
  // an expiring stage.
  return TfCreateRefPtrFromProtectedWeakPtr(stage);
}

bool Fail(std::string* diagnostic, const char* message) {
  if (diagnostic) {
    *diagnostic = message;
  }
  return false;
}

bool IsOutputName(const TfToken& name) {
  std::string const value = name.GetString();
  return value.size() > 8 && value.compare(0, 8, "outputs:") == 0;
}

bool IsEligibleScalarType(const SdfValueTypeName& scalar) {
  // Sdf has no general "numeric" predicate.  Keep this list explicit so
  // strings, tokens, assets, paths and arbitrary plugin value types remain
  // literal rather than becoming accidental expression consumers.
  return scalar == SdfValueTypeNames->Bool ||
      scalar == SdfValueTypeNames->UChar ||
      scalar == SdfValueTypeNames->Int ||
      scalar == SdfValueTypeNames->UInt ||
      scalar == SdfValueTypeNames->Int64 ||
      scalar == SdfValueTypeNames->UInt64 ||
      scalar == SdfValueTypeNames->Half ||
      scalar == SdfValueTypeNames->Float ||
      scalar == SdfValueTypeNames->Double ||
      scalar == SdfValueTypeNames->Int2 ||
      scalar == SdfValueTypeNames->Int3 ||
      scalar == SdfValueTypeNames->Int4 ||
      scalar == SdfValueTypeNames->Half2 ||
      scalar == SdfValueTypeNames->Half3 ||
      scalar == SdfValueTypeNames->Half4 ||
      scalar == SdfValueTypeNames->Float2 ||
      scalar == SdfValueTypeNames->Float3 ||
      scalar == SdfValueTypeNames->Float4 ||
      scalar == SdfValueTypeNames->Double2 ||
      scalar == SdfValueTypeNames->Double3 ||
      scalar == SdfValueTypeNames->Double4 ||
      scalar == SdfValueTypeNames->Point3h ||
      scalar == SdfValueTypeNames->Point3f ||
      scalar == SdfValueTypeNames->Point3d ||
      scalar == SdfValueTypeNames->Vector3h ||
      scalar == SdfValueTypeNames->Vector3f ||
      scalar == SdfValueTypeNames->Vector3d ||
      scalar == SdfValueTypeNames->Normal3h ||
      scalar == SdfValueTypeNames->Normal3f ||
      scalar == SdfValueTypeNames->Normal3d ||
      scalar == SdfValueTypeNames->Color3h ||
      scalar == SdfValueTypeNames->Color3f ||
      scalar == SdfValueTypeNames->Color3d ||
      scalar == SdfValueTypeNames->Color4h ||
      scalar == SdfValueTypeNames->Color4f ||
      scalar == SdfValueTypeNames->Color4d ||
      scalar == SdfValueTypeNames->Quatf ||
      scalar == SdfValueTypeNames->Quatd ||
      scalar == SdfValueTypeNames->Quath ||
      scalar == SdfValueTypeNames->Matrix2d ||
      scalar == SdfValueTypeNames->Matrix3d ||
      scalar == SdfValueTypeNames->Matrix4d ||
      scalar == SdfValueTypeNames->Frame4d ||
      scalar == SdfValueTypeNames->TexCoord2h ||
      scalar == SdfValueTypeNames->TexCoord2f ||
      scalar == SdfValueTypeNames->TexCoord2d ||
      scalar == SdfValueTypeNames->TexCoord3h ||
      scalar == SdfValueTypeNames->TexCoord3f ||
      scalar == SdfValueTypeNames->TexCoord3d;
}

UsdPrim FindDescription(UsdPrim prim) {
  for (; prim; prim = prim.GetParent()) {
    if (prim.IsA(kDescriptionType)) {
      return prim;
    }
  }
  return {};
}

UsdAttribute AttributeForEndpoint(
    const UsdStageWeakPtr& stage,
    const AttributeEndpoint& endpoint) {
  UsdStageRefPtr locked = LockStage(stage);
  if (!locked || !endpoint.isValid()) {
    return {};
  }
  SdfPath const primPath(endpoint.primPath);
  if (!primPath.IsAbsolutePath() || !primPath.IsPrimPath()) {
    return {};
  }
  return locked->GetPrimAtPath(primPath).GetAttribute(TfToken(endpoint.propertyName));
}

bool RequireFixedArrayLength(
    const UsdAttribute& output,
    const UsdAttribute& destination,
    std::string* diagnostic) {
  // The expression output's authored typed default is the sole available
  // authoring-level array-shape declaration.  Do not treat an absent value as
  // scalar, and do not rewrite either literal merely to manufacture a shape.
  VtValue outputValue;
  VtValue destinationValue;
  if (!output.HasAuthoredValue() || !output.Get(&outputValue) ||
      !destination.Get(&destinationValue) || !outputValue.IsArrayValued() ||
      !destinationValue.IsArrayValued()) {
    return Fail(diagnostic, "expression array output needs an authored fixed-length default");
  }
  if (outputValue.GetArraySize() != destinationValue.GetArraySize()) {
    return Fail(diagnostic, "expression array output length does not match destination literal");
  }
  return true;
}

bool ValidateAttributes(
    const UsdAttribute& output,
    const UsdAttribute& destination,
    std::string* diagnostic) {
  if (!output || !destination) {
    return Fail(diagnostic, "expression connection endpoints must be attributes");
  }
  UsdPrim const expression = output.GetPrim();
  UsdPrim const consumer = destination.GetPrim();
  if (!expression.IsA(kExpressionType) || !IsOutputName(output.GetName())) {
    return Fail(diagnostic, "source must be a UsdGenExpression outputs:* attribute");
  }
  if (!consumer.IsA(kOperatorType) || IsOutputName(destination.GetName())) {
    return Fail(diagnostic, "destination must be a UsdGenOperator input attribute");
  }

  UsdPrim const expressionDescription = FindDescription(expression);
  UsdPrim const consumerDescription = FindDescription(consumer);
  if (!expressionDescription || expressionDescription != consumerDescription ||
      expression.GetParent().GetName() != kExpressionsName ||
      expression.GetParent().GetParent() != expressionDescription ||
      !consumer.GetPath().HasPrefix(consumerDescription.GetPath().AppendChild(kOpsName))) {
    return Fail(diagnostic, "expression and operator must be in the same description scope");
  }

  SdfValueTypeName const outputType = output.GetTypeName();
  SdfValueTypeName const destinationType = destination.GetTypeName();
  if (!outputType || !destinationType || outputType != destinationType ||
      !IsEligibleScalarType(outputType.GetScalarType())) {
    return Fail(diagnostic, "expression output type must exactly match an eligible numeric or bool destination");
  }
  if (outputType.IsArray() &&
      !RequireFixedArrayLength(output, destination, diagnostic)) {
    return false;
  }
  return true;
}

bool ValidateEditTarget(const UsdStageWeakPtr& stage,
                        const SdfPath& sourcePath,
                        const SdfPath& destinationPath,
                        std::string* diagnostic) {
  UsdStageRefPtr const locked = LockStage(stage);
  if (!locked) {
    return Fail(diagnostic, "USD stage is no longer available");
  }
  UsdEditTarget const editTarget = locked->GetEditTarget();
  SdfLayerHandle const layer = editTarget.GetLayer();
  if (!layer || !layer->PermissionToEdit()) {
    return Fail(diagnostic, "USD edit target is not writable");
  }
  if (editTarget.MapToSpecPath(sourcePath).IsEmpty() ||
      editTarget.MapToSpecPath(destinationPath).IsEmpty()) {
    return Fail(diagnostic, "USD edit target cannot author this connection");
  }
  return true;
}

const char* DomainName(ExpressionEvaluationDomain domain) {
  switch (domain) {
    case ExpressionEvaluationDomain::Groom:
      return "groom";
    case ExpressionEvaluationDomain::Primitive:
      return "primitive";
    case ExpressionEvaluationDomain::Point:
      return "point";
  }
  return "groom";
}

struct ConnectionListSnapshot {
  bool authored = false;
  SdfPathListOp listOp;
};

ConnectionListSnapshot SnapshotConnectionList(const UsdEditTarget& target,
                                              const SdfPath& path) {
  ConnectionListSnapshot snapshot;
  SdfAttributeSpecHandle const spec = target.GetAttributeSpecForScenePath(path);
  if (!spec || !spec->HasConnectionPaths()) {
    return snapshot;
  }
  VtValue const value = spec->GetInfo(SdfFieldKeys->ConnectionPaths);
  if (value.IsHolding<SdfPathListOp>()) {
    snapshot.authored = true;
    snapshot.listOp = value.UncheckedGet<SdfPathListOp>();
  }
  return snapshot;
}

struct EvaluationSnapshot {
  bool authored = false;
  VtValue value;
};

EvaluationSnapshot SnapshotEvaluation(const UsdEditTarget& target,
                                      const SdfPath& path) {
  EvaluationSnapshot snapshot;
  SdfAttributeSpecHandle const spec = target.GetAttributeSpecForScenePath(path);
  if (!spec) {
    return snapshot;
  }
  VtDictionary const customData = spec->GetCustomData();
  if (VtValue const* value = customData.GetValueAtPath(kEvaluationKey.GetString())) {
    snapshot.authored = true;
    snapshot.value = *value;
  }
  return snapshot;
}

bool RestoreConnectionList(const UsdEditTarget& editTarget,
                           const UsdAttribute& attribute,
                           const ConnectionListSnapshot& snapshot) {
  SdfAttributeSpecHandle spec =
      editTarget.GetAttributeSpecForScenePath(attribute.GetPath());
  if (!spec) {
    // The command may have created no prior edit-target spec.  ClearConnections
    // creates just the target attribute spec; only its connection field is then
    // changed, leaving independently authored fields intact.
    if (!attribute.ClearConnections()) {
      return false;
    }
    spec = editTarget.GetAttributeSpecForScenePath(attribute.GetPath());
    if (!spec) {
      return false;
    }
  }
  SdfConnectionsProxy editor = spec->GetConnectionPathList();
  editor.ClearEdits();
  if (!snapshot.authored) {
    return true;
  }
  if (snapshot.listOp.IsExplicit()) {
    editor.ClearEditsAndMakeExplicit();
    editor.GetExplicitItems() = snapshot.listOp.GetExplicitItems();
    return true;
  }
  editor.GetAddedItems() = snapshot.listOp.GetAddedItems();
  editor.GetPrependedItems() = snapshot.listOp.GetPrependedItems();
  editor.GetAppendedItems() = snapshot.listOp.GetAppendedItems();
  editor.GetDeletedItems() = snapshot.listOp.GetDeletedItems();
  editor.GetOrderedItems() = snapshot.listOp.GetOrderedItems();
  return true;
}

class ExpressionConnectionCommand final : public Command {
 public:
  ExpressionConnectionCommand(
      UsdStageRefPtr stage,
      UsdEditTarget editTarget,
      SdfPath sourcePath,
      SdfPath destinationPath,
      ExpressionEvaluationDomain domain,
      bool authorEvaluation,
      bool targetSpecWasAbsent,
      ConnectionListSnapshot connections,
      EvaluationSnapshot evaluation)
      : _stage(std::move(stage)),
        _editTarget(std::move(editTarget)),
        _sourcePath(std::move(sourcePath)),
        _destinationPath(std::move(destinationPath)),
        _domain(domain),
        _authorEvaluation(authorEvaluation),
        _targetSpecWasAbsent(targetSpecWasAbsent),
        _connections(std::move(connections)),
        _evaluation(std::move(evaluation)) {}

  void execute() override {
    if (!_stage) {
      throw std::runtime_error("USD stage is no longer available");
    }
    try {
      UsdEditContext const context(_stage, _editTarget);
      UsdAttribute const target = _stage->GetAttributeAtPath(_destinationPath);
      if (!target) {
        throw std::runtime_error("expression connection target no longer exists");
      }
      TfErrorMark connectionErrors;
      bool const connected = _authorEvaluation
          ? target.SetConnections(SdfPathVector{_sourcePath})
          : target.RemoveConnection(_sourcePath);
      if (!connected || !connectionErrors.IsClean()) {
        connectionErrors.Clear();
        throw std::runtime_error("USD could not author expression connection");
      }
      _CaptureCreatedSpecBaseline();
      if (_authorEvaluation) {
        TfErrorMark metadataErrors;
        target.SetCustomDataByKey(kEvaluationKey, VtValue(std::string(DomainName(_domain))));
        if (!metadataErrors.IsClean()) {
          metadataErrors.Clear();
          throw std::runtime_error("USD could not author expression evaluation metadata");
        }
      }
    } catch (...) {
      _RestoreOwnedStateNoThrow();
      throw;
    }
  }

  void undo() override {
    if (!_stage) {
      throw std::runtime_error("USD stage is no longer available");
    }
    UsdEditContext const context(_stage, _editTarget);
    UsdAttribute const target = _stage->GetAttributeAtPath(_destinationPath);
    if (!target) {
      throw std::runtime_error("expression connection target no longer exists");
    }
    TfErrorMark errors;
    if (!RestoreConnectionList(_editTarget, target, _connections)) {
      errors.Clear();
      throw std::runtime_error("USD could not restore expression connection list-op");
    }
    if (_authorEvaluation) {
      RestoreEvaluation(target);
    }
    RemoveCreatedOverrideIfSafe();
    if (!errors.IsClean()) {
      errors.Clear();
      throw std::runtime_error("USD could not restore expression connection metadata");
    }
  }

  std::string description() const override {
    return "Connect SeExpr Attribute";
  }

 private:
  void RestoreEvaluation(const UsdAttribute& target) const {
    if (_evaluation.authored) {
      target.SetCustomDataByKey(kEvaluationKey, _evaluation.value);
    } else {
      target.ClearCustomDataByKey(kEvaluationKey);
    }
  }

  void RemoveCreatedOverrideIfSafe() const {
    if (!_targetSpecWasAbsent) {
      return;
    }
    SdfAttributeSpecHandle const spec =
        _editTarget.GetAttributeSpecForScenePath(_destinationPath);
    if (!spec || !_MatchesCreatedSpecBaseline(spec)) {
      return;
    }
    if (SdfPrimSpecHandle owner = TfDynamic_cast<SdfPrimSpecHandle>(spec->GetOwner())) {
      owner->RemoveProperty(spec);
    }
  }

  void _CaptureCreatedSpecBaseline() {
    if (!_targetSpecWasAbsent || _createdBaselineCaptured) {
      return;
    }
    SdfAttributeSpecHandle const spec =
        _editTarget.GetAttributeSpecForScenePath(_destinationPath);
    if (!spec) {
      throw std::runtime_error("USD did not create an expression target spec");
    }
    _createdSpecBaseline.clear();
    for (TfToken const& field : spec->GetLayer()->ListFields(spec->GetPath())) {
      // USD creates both the authored list-op and its connectionChildren
      // acceleration field.  Both are owned by Set/RemoveConnection and both
      // are cleared before cleanup; neither is part of the inert baseline.
      if (field != SdfFieldKeys->ConnectionPaths &&
          field != SdfChildrenKeys->ConnectionChildren) {
        _createdSpecBaseline.emplace_back(field, spec->GetInfo(field));
      }
    }
    _createdBaselineCaptured = true;
  }

  bool _MatchesCreatedSpecBaseline(const SdfAttributeSpecHandle& spec) const {
    if (!_createdBaselineCaptured) {
      return false;
    }
    // The current connection list and evaluation key have already been
    // restored.  An empty customData container can be left by the underlying
    // dictionary erase; remove only that empty command-owned container before
    // comparing the exact creation snapshot.
    if (std::find_if(_createdSpecBaseline.begin(), _createdSpecBaseline.end(),
                     [](auto const& item) { return item.first == SdfFieldKeys->CustomData; })
            == _createdSpecBaseline.end() &&
        spec->GetCustomData().empty()) {
      spec->GetLayer()->EraseField(spec->GetPath(), SdfFieldKeys->CustomData);
    }
    std::vector<TfToken> const fields = spec->GetLayer()->ListFields(spec->GetPath());
    if (fields.size() != _createdSpecBaseline.size()) {
      return false;
    }
    for (auto const& [field, value] : _createdSpecBaseline) {
      if (std::find(fields.begin(), fields.end(), field) == fields.end() ||
          spec->GetInfo(field) != value) {
        return false;
      }
    }
    return true;
  }

  void _RestoreOwnedStateNoThrow() const noexcept {
    try {
      if (!_stage) {
        return;
      }
      UsdEditContext const context(_stage, _editTarget);
      UsdAttribute const target = _stage->GetAttributeAtPath(_destinationPath);
      if (!target) {
        return;
      }
      TfErrorMark errors;
      RestoreConnectionList(_editTarget, target, _connections);
      if (_authorEvaluation) {
        RestoreEvaluation(target);
      }
      RemoveCreatedOverrideIfSafe();
      errors.Clear();
    } catch (...) {
      // The original authoring failure is authoritative.  A best-effort
      // rollback never masks it, and only fields owned by this command move.
    }
  }

  UsdStageRefPtr _stage;
  UsdEditTarget _editTarget;
  SdfPath _sourcePath;
  SdfPath _destinationPath;
  ExpressionEvaluationDomain _domain;
  bool _authorEvaluation;
  bool _targetSpecWasAbsent;
  ConnectionListSnapshot _connections;
  EvaluationSnapshot _evaluation;
  bool _createdBaselineCaptured = false;
  std::vector<std::pair<TfToken, VtValue>> _createdSpecBaseline;
};

AttributeEndpoint MakeEndpoint(const UsdAttribute& attribute, bool isOutput) {
  AttributeEndpoint endpoint;
  endpoint.nodeId = attribute.GetPrim().GetPath().GetString();
  endpoint.primPath = endpoint.nodeId;
  endpoint.propertyName = attribute.GetName().GetString();
  endpoint.isOutput = isOutput;
  return endpoint;
}

} // namespace

bool ValidateExpressionConnection(
    const UsdStageWeakPtr& stage,
    const AttributeConnectionRequest& request,
    std::string* diagnostic) {
  if (!request.source.isOutput || request.target.isOutput) {
    return Fail(diagnostic, "expression connection must be normalized from output to input");
  }
  UsdAttribute const output = AttributeForEndpoint(stage, request.source);
  UsdAttribute const destination = AttributeForEndpoint(stage, request.target);
  return ValidateAttributes(output, destination, diagnostic);
}

CommandPtr MakeExpressionConnectionCommand(
    const UsdStageWeakPtr& stage,
    const AttributeConnectionRequest& request,
    ExpressionEvaluationDomain domain) {
  if (!ValidateExpressionConnection(stage, request)) {
    return {};
  }
  UsdStageRefPtr const locked = LockStage(stage);
  if (!locked) {
    return {};
  }
  UsdAttribute const output = AttributeForEndpoint(stage, request.source);
  UsdAttribute const destination = AttributeForEndpoint(stage, request.target);
  if (!ValidateEditTarget(stage, output.GetPath(), destination.GetPath(), nullptr)) {
    return {};
  }
  UsdEditTarget const editTarget = locked->GetEditTarget();
  return std::make_unique<ExpressionConnectionCommand>(
      locked,
      editTarget,
      output.GetPath(),
      destination.GetPath(),
      domain,
      true,
      !editTarget.GetAttributeSpecForScenePath(destination.GetPath()),
      SnapshotConnectionList(editTarget, destination.GetPath()),
      SnapshotEvaluation(editTarget, destination.GetPath()));
}

CommandPtr MakeExpressionDisconnectionCommand(
    const UsdStageWeakPtr& stage,
    const AttributeConnectionRequest& request) {
  if (!ValidateExpressionConnection(stage, request)) {
    return {};
  }
  UsdStageRefPtr const locked = LockStage(stage);
  if (!locked) {
    return {};
  }
  UsdAttribute const output = AttributeForEndpoint(stage, request.source);
  UsdAttribute const destination = AttributeForEndpoint(stage, request.target);
  if (!ValidateEditTarget(stage, output.GetPath(), destination.GetPath(), nullptr)) {
    return {};
  }
  SdfPathVector connections;
  destination.GetConnections(&connections);
  if (std::find(connections.begin(), connections.end(), output.GetPath()) ==
      connections.end()) {
    return {};
  }
  UsdEditTarget const editTarget = locked->GetEditTarget();
  return std::make_unique<ExpressionConnectionCommand>(
      locked,
      editTarget,
      output.GetPath(),
      destination.GetPath(),
      ExpressionEvaluationDomain::Groom,
      false,
      !editTarget.GetAttributeSpecForScenePath(destination.GetPath()),
      SnapshotConnectionList(editTarget, destination.GetPath()),
      EvaluationSnapshot{});
}

AttributeConnectionDrag::Callbacks MakeExpressionConnectionCallbacks(
    const UsdStageWeakPtr& stage,
    ExpressionEvaluationDomain domain) {
  AttributeConnectionDrag::Callbacks callbacks;
  callbacks.validate = [stage](const AttributeConnectionRequest& request) {
    return ValidateExpressionConnection(stage, request);
  };
  callbacks.author = [stage, domain](const AttributeConnectionRequest& request) {
    return MakeExpressionConnectionCommand(stage, request, domain);
  };
  return callbacks;
}

std::vector<ExpressionConnectionLink> GetExpressionConnections(
    const UsdPrim& operatorPrim) {
  std::vector<ExpressionConnectionLink> result;
  if (!operatorPrim || !operatorPrim.IsA(kOperatorType)) {
    return result;
  }
  UsdStageWeakPtr const stage = operatorPrim.GetStage();
  UsdStageRefPtr const locked = LockStage(stage);
  if (!locked) {
    return result;
  }
  for (UsdAttribute const& destination : operatorPrim.GetAttributes()) {
    SdfPathVector sources;
    destination.GetConnections(&sources);
    for (SdfPath const& sourcePath : sources) {
      UsdAttribute const output = locked->GetAttributeAtPath(sourcePath);
      if (!output) {
        continue;
      }
      ExpressionConnectionLink connection;
      connection.source = MakeEndpoint(output, true);
      connection.target = MakeEndpoint(destination, false);
      AttributeConnectionRequest request;
      request.source = connection.source;
      request.target = connection.target;
      if (!ValidateExpressionConnection(stage, request)) {
        continue;
      }
      connection.link.sourceNodeId = connection.source.nodeId;
      connection.link.sourcePort = connection.source.propertyName;
      connection.link.targetNodeId = connection.target.nodeId;
      connection.link.targetPort = connection.target.propertyName;
      connection.link.targetPropertyName = connection.target.propertyName;
      connection.link.isRelationship = false;
      result.push_back(std::move(connection));
    }
  }
  return result;
}

} // namespace noodles::usd
