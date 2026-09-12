// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "usd/ExpressionConnections.h"

#include "undo/NoodlesUndoManager.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/editTarget.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/variantSets.h"

#include <gtest/gtest.h>

#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace noodles::usd {
namespace {

constexpr char kDescriptionPath[] = "/Description";
constexpr char kExpressionPath[] = "/Description/Expressions/value";
constexpr char kOperatorPath[] = "/Description/Ops/width";

AttributeEndpoint Endpoint(const UsdAttribute& attribute, bool output) {
  AttributeEndpoint endpoint;
  endpoint.nodeId = attribute.GetPrim().GetPath().GetString();
  endpoint.primPath = endpoint.nodeId;
  endpoint.propertyName = attribute.GetName().GetString();
  endpoint.isOutput = output;
  endpoint.portPosition = output ? Vec2d(12.0, 18.0) : Vec2d(70.0, 42.0);
  return endpoint;
}

AttributeConnectionRequest Request(const UsdAttribute& source,
                                   const UsdAttribute& destination) {
  AttributeConnectionRequest request;
  request.source = Endpoint(source, true);
  request.target = Endpoint(destination, false);
  return request;
}

SdfPathVector Connections(const UsdAttribute& attribute) {
  SdfPathVector result;
  attribute.GetConnections(&result);
  return result;
}

struct ExpressionScene {
  UsdStageRefPtr stage = UsdStage::CreateInMemory("expression-connections");
  UsdPrim description;
  UsdPrim expression;
  UsdPrim operatorPrim;
  UsdAttribute vectorOutput;
  UsdAttribute previousVectorOutput;
  UsdAttribute matrixOutput;
  UsdAttribute arrayOutput;
  UsdAttribute boolOutput;
  UsdAttribute stringOutput;
  UsdAttribute vectorInput;
  UsdAttribute matrixInput;
  UsdAttribute arrayInput;
  UsdAttribute boolInput;
  UsdAttribute stringInput;
};

ExpressionScene MakeScene() {
  ExpressionScene scene;
  scene.description = scene.stage->DefinePrim(
      SdfPath(kDescriptionPath), TfToken("UsdGenDescription"));
  scene.stage->DefinePrim(SdfPath("/Description/Expressions"), TfToken("Scope"));
  scene.stage->DefinePrim(SdfPath("/Description/Ops"), TfToken("Scope"));
  scene.expression = scene.stage->DefinePrim(
      SdfPath(kExpressionPath), TfToken("UsdGenExpression"));
  scene.operatorPrim = scene.stage->DefinePrim(
      SdfPath(kOperatorPath), TfToken("UsdGenWidth"));

  scene.vectorOutput = scene.expression.CreateAttribute(
      TfToken("outputs:vector"), SdfValueTypeNames->Vector3f, true);
  scene.vectorOutput.Set(GfVec3f(3.0f, 4.0f, 5.0f));
  scene.previousVectorOutput = scene.expression.CreateAttribute(
      TfToken("outputs:previousVector"), SdfValueTypeNames->Vector3f, true);
  scene.previousVectorOutput.Set(GfVec3f(1.0f, 2.0f, 3.0f));
  scene.matrixOutput = scene.expression.CreateAttribute(
      TfToken("outputs:matrix"), SdfValueTypeNames->Matrix4d, true);
  scene.matrixOutput.Set(GfMatrix4d(2.0));
  scene.arrayOutput = scene.expression.CreateAttribute(
      TfToken("outputs:weights"), SdfValueTypeNames->FloatArray, true);
  scene.arrayOutput.Set(VtFloatArray{1.0f, 2.0f, 3.0f});
  scene.boolOutput = scene.expression.CreateAttribute(
      TfToken("outputs:enabled"), SdfValueTypeNames->Bool, true);
  scene.boolOutput.Set(true);
  scene.stringOutput = scene.expression.CreateAttribute(
      TfToken("outputs:label"), SdfValueTypeNames->String, true);
  scene.stringOutput.Set(std::string("literal"));

  scene.vectorInput = scene.operatorPrim.CreateAttribute(
      TfToken("usdGen:style:vector"), SdfValueTypeNames->Vector3f, true);
  scene.vectorInput.Set(GfVec3f(8.0f, 9.0f, 10.0f));
  scene.matrixInput = scene.operatorPrim.CreateAttribute(
      TfToken("usdGen:style:matrix"), SdfValueTypeNames->Matrix4d, true);
  scene.matrixInput.Set(GfMatrix4d(3.0));
  scene.arrayInput = scene.operatorPrim.CreateAttribute(
      TfToken("usdGen:style:weights"), SdfValueTypeNames->FloatArray, true);
  scene.arrayInput.Set(VtFloatArray{9.0f, 8.0f, 7.0f});
  scene.boolInput = scene.operatorPrim.CreateAttribute(
      TfToken("usdGen:style:enabled"), SdfValueTypeNames->Bool, true);
  scene.boolInput.Set(false);
  scene.stringInput = scene.operatorPrim.CreateAttribute(
      TfToken("usdGen:style:label"), SdfValueTypeNames->String, true);
  scene.stringInput.Set(std::string("keep literal"));
  return scene;
}

void ExpectOneConnection(const UsdAttribute& destination, const SdfPath& source) {
  SdfPathVector const expected{source};
  EXPECT_EQ(expected, Connections(destination));
}

TEST(ExpressionConnectionsTest, DragAuthorsAndRestoresExactTargetOpinion) {
  NoodlesUndoManager::instance().reset();
  ExpressionScene scene = MakeScene();
  ASSERT_TRUE(scene.description.IsA(TfToken("UsdGenDescription")));
  ASSERT_TRUE(scene.operatorPrim.IsA(TfToken("UsdGenOperator")));

  // Preserve a non-explicit list-op and nested metadata that predate the
  // drag.  The undo path must not flatten that list-op or replace unrelated
  // custom data authored after the command ran.
  ASSERT_TRUE(scene.vectorInput.AddConnection(scene.previousVectorOutput.GetPath()));
  scene.vectorInput.SetCustomDataByKey(
      TfToken("usdGen:evaluation"), VtValue(std::string("primitive")));
  scene.vectorInput.SetCustomDataByKey(
      TfToken("user:preserved"), VtValue(std::string("before")));
  SdfAttributeSpecHandle const originalSpec = scene.stage->GetEditTarget()
      .GetAttributeSpecForScenePath(scene.vectorInput.GetPath());
  ASSERT_TRUE(originalSpec);
  VtValue const originalConnectionList = originalSpec->GetInfo(
      SdfFieldKeys->ConnectionPaths);

  UsdStageWeakPtr weak = scene.stage;
  AttributeConnectionDrag drag(MakeExpressionConnectionCallbacks(
      weak, ExpressionEvaluationDomain::Point));
  ASSERT_TRUE(drag.Begin(Endpoint(scene.vectorInput, false)));
  drag.Update(Endpoint(scene.vectorOutput, true));
  auto preview = drag.Preview();
  ASSERT_TRUE(preview);
  EXPECT_EQ("outputs:vector", preview->sourcePort);
  EXPECT_EQ("usdGen:style:vector", preview->targetPropertyName);
  EXPECT_TRUE(drag.Drop());
  ExpectOneConnection(scene.vectorInput, scene.vectorOutput.GetPath());
  EXPECT_EQ("point", scene.vectorInput.GetCustomDataByKey(
      TfToken("usdGen:evaluation")).UncheckedGet<std::string>());
  GfVec3f literal;
  ASSERT_TRUE(scene.vectorInput.Get(&literal));
  EXPECT_EQ(GfVec3f(8.0f, 9.0f, 10.0f), literal);

  auto links = GetExpressionConnections(scene.operatorPrim);
  ASSERT_EQ(1u, links.size());
  EXPECT_EQ("outputs:vector", links[0].source.propertyName);
  EXPECT_EQ("usdGen:style:vector", links[0].target.propertyName);
  EXPECT_FALSE(links[0].link.isRelationship);

  // This is deliberately later than execute: undo owns only the nested
  // evaluation key and connection list-op, never all customData.
  scene.vectorInput.SetCustomDataByKey(
      TfToken("user:later"), VtValue(std::string("keep")));
  NoodlesUndoManager::instance().undo();
  ExpectOneConnection(scene.vectorInput, scene.previousVectorOutput.GetPath());
  SdfAttributeSpecHandle spec = scene.stage->GetEditTarget()
      .GetAttributeSpecForScenePath(scene.vectorInput.GetPath());
  ASSERT_TRUE(spec);
  EXPECT_EQ(originalConnectionList, spec->GetInfo(SdfFieldKeys->ConnectionPaths));
  EXPECT_EQ("primitive", scene.vectorInput.GetCustomDataByKey(
      TfToken("usdGen:evaluation")).UncheckedGet<std::string>());
  EXPECT_EQ("before", scene.vectorInput.GetCustomDataByKey(
      TfToken("user:preserved")).UncheckedGet<std::string>());
  EXPECT_EQ("keep", scene.vectorInput.GetCustomDataByKey(
      TfToken("user:later")).UncheckedGet<std::string>());

  NoodlesUndoManager::instance().redo();
  ExpectOneConnection(scene.vectorInput, scene.vectorOutput.GetPath());
  EXPECT_EQ("point", scene.vectorInput.GetCustomDataByKey(
      TfToken("usdGen:evaluation")).UncheckedGet<std::string>());

  AttributeConnectionRequest request = Request(scene.vectorOutput, scene.vectorInput);
  CommandPtr disconnect = MakeExpressionDisconnectionCommand(weak, request);
  ASSERT_TRUE(disconnect);
  disconnect->execute();
  EXPECT_TRUE(Connections(scene.vectorInput).empty());
  disconnect->undo();
  ExpectOneConnection(scene.vectorInput, scene.vectorOutput.GetPath());

  std::string exported;
  ASSERT_TRUE(scene.stage->GetRootLayer()->ExportToString(&exported));
  SdfLayerRefPtr reloadedLayer = SdfLayer::CreateAnonymous("expression-reload");
  ASSERT_TRUE(reloadedLayer->ImportFromString(exported));
  UsdStageRefPtr reloaded = UsdStage::Open(reloadedLayer);
  ASSERT_TRUE(reloaded);
  auto reloadedLinks = GetExpressionConnections(
      reloaded->GetPrimAtPath(SdfPath(kOperatorPath)));
  ASSERT_EQ(1u, reloadedLinks.size());
  EXPECT_EQ("outputs:vector", reloadedLinks[0].source.propertyName);
  EXPECT_EQ("usdGen:style:vector", reloadedLinks[0].target.propertyName);
  NoodlesUndoManager::instance().reset();
}

TEST(ExpressionConnectionsTest, StrictTypedConnectionsAndLiteralFallback) {
  ExpressionScene scene = MakeScene();
  ASSERT_TRUE(scene.operatorPrim.IsA(TfToken("UsdGenOperator")));
  UsdStageWeakPtr weak = scene.stage;

  for (auto const& pair : std::vector<std::pair<UsdAttribute, UsdAttribute>>{
           {scene.vectorOutput, scene.vectorInput},
           {scene.matrixOutput, scene.matrixInput},
           {scene.arrayOutput, scene.arrayInput},
           {scene.boolOutput, scene.boolInput}}) {
    AttributeConnectionRequest request = Request(pair.first, pair.second);
    std::string diagnostic;
    EXPECT_TRUE(ValidateExpressionConnection(weak, request, &diagnostic)) << diagnostic;
    CommandPtr command = MakeExpressionConnectionCommand(
        weak, request, ExpressionEvaluationDomain::Groom);
    ASSERT_TRUE(command);
    command->execute();
    ExpectOneConnection(pair.second, pair.first.GetPath());
  }

  AttributeConnectionRequest stringRequest = Request(scene.stringOutput, scene.stringInput);
  std::string diagnostic;
  EXPECT_FALSE(ValidateExpressionConnection(weak, stringRequest, &diagnostic));
  CommandPtr stringCommand = MakeExpressionConnectionCommand(
      weak, stringRequest, ExpressionEvaluationDomain::Groom);
  EXPECT_FALSE(static_cast<bool>(stringCommand));
  EXPECT_TRUE(Connections(scene.stringInput).empty());
  std::string literal;
  ASSERT_TRUE(scene.stringInput.Get(&literal));
  EXPECT_EQ("keep literal", literal);

  // Array shape is taken from authored source/destination defaults.  Missing
  // source shape and mismatched destination shape must fail closed.
  UsdAttribute unknownArray = scene.expression.CreateAttribute(
      TfToken("outputs:unknownArray"), SdfValueTypeNames->FloatArray, true);
  UsdAttribute wrongArray = scene.operatorPrim.CreateAttribute(
      TfToken("usdGen:style:wrongArray"), SdfValueTypeNames->FloatArray, true);
  wrongArray.Set(VtFloatArray{1.0f, 2.0f});
  EXPECT_FALSE(ValidateExpressionConnection(weak, Request(unknownArray, scene.arrayInput)));
  EXPECT_FALSE(ValidateExpressionConnection(weak, Request(scene.arrayOutput, wrongArray)));

  // Endpoint text is host supplied.  Invalid or non-attribute paths must
  // reject with a diagnostic before USD authoring/path mapping is attempted.
  AttributeConnectionRequest malformed = Request(scene.vectorOutput, scene.vectorInput);
  malformed.target.primPath = "relative/not/a/prim";
  std::string malformedDiagnostic;
  EXPECT_FALSE(ValidateExpressionConnection(weak, malformed, &malformedDiagnostic));
  EXPECT_FALSE(malformedDiagnostic.empty());

  // Empty is an explicit, known fixed shape rather than an absent shape.
  UsdAttribute emptyOutput = scene.expression.CreateAttribute(
      TfToken("outputs:emptyWeights"), SdfValueTypeNames->FloatArray, true);
  UsdAttribute emptyInput = scene.operatorPrim.CreateAttribute(
      TfToken("usdGen:style:emptyWeights"), SdfValueTypeNames->FloatArray, true);
  emptyOutput.Set(VtFloatArray{});
  emptyInput.Set(VtFloatArray{});
  EXPECT_TRUE(ValidateExpressionConnection(weak, Request(emptyOutput, emptyInput)));

  // Matching types are insufficient across descriptions: the adapter keeps
  // dependencies inside one description's Expressions -> Ops transport scope.
  UsdPrim otherDescription = scene.stage->DefinePrim(
      SdfPath("/OtherDescription"), TfToken("UsdGenDescription"));
  scene.stage->DefinePrim(SdfPath("/OtherDescription/Expressions"), TfToken("Scope"));
  UsdPrim otherExpression = scene.stage->DefinePrim(
      SdfPath("/OtherDescription/Expressions/value"), TfToken("UsdGenExpression"));
  UsdAttribute foreignVector = otherExpression.CreateAttribute(
      TfToken("outputs:vector"), SdfValueTypeNames->Vector3f, true);
  foreignVector.Set(GfVec3f(1.0f));
  ASSERT_TRUE(otherDescription.IsA(TfToken("UsdGenDescription")));
  EXPECT_FALSE(ValidateExpressionConnection(weak, Request(foreignVector, scene.vectorInput)));
}

TEST(ExpressionConnectionsTest, UndoRemovesOnlyCommandCreatedAttributeOverride) {
  NoodlesUndoManager::instance().reset();
  ExpressionScene scene = MakeScene();
  UsdStageWeakPtr weak = scene.stage;

  // Both inputs are schema-defined and inherited by UsdGenWidth, hence they
  // have no local attribute specs until a connection command authors one.
  UsdAttribute builtin = scene.operatorPrim.GetAttribute(TfToken("usdGen:width"));
  UsdAttribute taper = scene.operatorPrim.GetAttribute(TfToken("usdGen:taper"));
  ASSERT_TRUE(builtin);
  ASSERT_TRUE(taper);
  EXPECT_FALSE(scene.stage->GetEditTarget().GetAttributeSpecForScenePath(
      builtin.GetPath()));
  EXPECT_FALSE(scene.stage->GetEditTarget().GetAttributeSpecForScenePath(
      taper.GetPath()));
  UsdAttribute output = scene.expression.CreateAttribute(
      TfToken("outputs:width"), SdfValueTypeNames->Float, true);
  output.Set(0.5f);
  AttributeConnectionRequest request = Request(output, taper);
  ASSERT_TRUE(ValidateExpressionConnection(weak, request));
  CommandPtr command = MakeExpressionConnectionCommand(
      weak, request, ExpressionEvaluationDomain::Point);
  ASSERT_TRUE(command);
  command->execute();
  ASSERT_TRUE(scene.stage->GetEditTarget().GetAttributeSpecForScenePath(
      taper.GetPath()));
  command->undo();
  EXPECT_FALSE(scene.stage->GetEditTarget().GetAttributeSpecForScenePath(
      taper.GetPath()));

  // A later unrelated field makes the command-created override non-inert;
  // undo must retain it rather than deleting someone else's opinion.
  CommandPtr preserveCommand = MakeExpressionConnectionCommand(
      weak, Request(output, builtin), ExpressionEvaluationDomain::Point);
  ASSERT_TRUE(preserveCommand);
  preserveCommand->execute();
  builtin.SetCustomDataByKey(TfToken("user:later"), VtValue(std::string("keep")));
  preserveCommand->undo();
  SdfAttributeSpecHandle preservedSpec = scene.stage->GetEditTarget()
      .GetAttributeSpecForScenePath(builtin.GetPath());
  ASSERT_TRUE(preservedSpec);
  EXPECT_EQ("keep", builtin.GetCustomDataByKey(
      TfToken("user:later")).UncheckedGet<std::string>());
  NoodlesUndoManager::instance().reset();
}

TEST(ExpressionConnectionsTest, CapturedVariantEditTargetRestoresMappedListOp) {
  ExpressionScene scene = MakeScene();
  UsdStageWeakPtr weak = scene.stage;
  SdfAttributeSpecHandle const rootSpec = scene.stage->GetRootLayer()
      ->GetAttributeAtPath(scene.vectorInput.GetPath());
  ASSERT_TRUE(rootSpec);
  VtDictionary const rootCustomData = rootSpec->GetCustomData();
  GfVec3f rootLiteral;
  ASSERT_TRUE(scene.vectorInput.Get(&rootLiteral));
  EXPECT_TRUE(Connections(scene.vectorInput).empty());
  UsdVariantSet variant = scene.description.GetVariantSets().AddVariantSet("authoring");
  ASSERT_TRUE(variant.AddVariant("alternate"));
  ASSERT_TRUE(variant.SetVariantSelection("alternate"));
  UsdEditTarget const variantTarget = variant.GetVariantEditTarget();
  ASSERT_TRUE(variantTarget.IsValid());
  SdfPath const mappedDestination = variantTarget.MapToSpecPath(scene.vectorInput.GetPath());
  ASSERT_FALSE(mappedDestination.IsEmpty());
  EXPECT_NE(scene.vectorInput.GetPath(), mappedDestination);
  scene.stage->SetEditTarget(variantTarget);

  CommandPtr command = MakeExpressionConnectionCommand(
      weak, Request(scene.vectorOutput, scene.vectorInput),
      ExpressionEvaluationDomain::Primitive);
  ASSERT_TRUE(command);
  command->execute();
  ASSERT_TRUE(scene.stage->GetRootLayer()->GetAttributeAtPath(mappedDestination));
  ExpectOneConnection(scene.vectorInput, scene.vectorOutput.GetPath());

  // Switching the stage's current target after execute must not redirect undo
  // into the root namespace: the command retains the variant target captured
  // during admission.
  scene.stage->SetEditTarget(UsdEditTarget(scene.stage->GetRootLayer()));
  command->undo();
  EXPECT_FALSE(scene.stage->GetRootLayer()->GetAttributeAtPath(mappedDestination));
  EXPECT_TRUE(Connections(scene.vectorInput).empty());
  SdfAttributeSpecHandle const restoredRootSpec = scene.stage->GetRootLayer()
      ->GetAttributeAtPath(scene.vectorInput.GetPath());
  ASSERT_TRUE(restoredRootSpec);
  EXPECT_FALSE(restoredRootSpec->HasConnectionPaths());
  EXPECT_EQ(rootCustomData, restoredRootSpec->GetCustomData());
  GfVec3f restoredLiteral;
  ASSERT_TRUE(scene.vectorInput.Get(&restoredLiteral));
  EXPECT_EQ(rootLiteral, restoredLiteral);
}

TEST(ExpressionConnectionsTest, StaleTargetThrowsAndIsNotPushedToUndo) {
  NoodlesUndoManager::instance().reset();
  ExpressionScene scene = MakeScene();
  UsdStageWeakPtr weak = scene.stage;
  AttributeConnectionDrag drag({
      [weak](const AttributeConnectionRequest& request) {
        return ValidateExpressionConnection(weak, request);
      },
      [&scene, weak](const AttributeConnectionRequest& request) {
        CommandPtr command = MakeExpressionConnectionCommand(
            weak, request, ExpressionEvaluationDomain::Point);
        // Simulate a synchronous host edit after admission but before Drop's
        // execute.  The command must throw rather than report success/push.
        scene.stage->RemovePrim(scene.operatorPrim.GetPath());
        return command;
      }});
  ASSERT_TRUE(drag.Begin(Endpoint(scene.vectorInput, false)));
  drag.Update(Endpoint(scene.vectorOutput, true));
  EXPECT_THROW(drag.Drop(), std::runtime_error);
  EXPECT_FALSE(NoodlesUndoManager::instance().canUndo());
  NoodlesUndoManager::instance().reset();
}

} // namespace
} // namespace noodles::usd
