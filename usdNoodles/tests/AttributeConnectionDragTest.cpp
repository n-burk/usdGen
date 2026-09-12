// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "core/AttributeConnectionDrag.h"
#include "undo/NoodlesUndoManager.h"

#include <gtest/gtest.h>

#include <memory>

namespace noodles {
namespace {

AttributeEndpoint Output() {
  AttributeEndpoint endpoint{
      "expressionNode", "/Description/Expressions/width", "outputs:result", true};
  endpoint.portPosition = {10.0, 20.0};
  return endpoint;
}

AttributeEndpoint Input() {
  AttributeEndpoint endpoint{
      "widthNode", "/Description/Operators/width", "inputs:width", false};
  endpoint.portPosition = {80.0, 35.0};
  return endpoint;
}

struct Counts {
  int executes = 0;
  int undoes = 0;
};

class RecordingCommand final : public Command {
 public:
  explicit RecordingCommand(std::shared_ptr<Counts> counts) : _counts(std::move(counts)) {}
  void execute() override { ++_counts->executes; }
  void undo() override { ++_counts->undoes; }
  std::string description() const override { return "Connect attributes"; }

 private:
  std::shared_ptr<Counts> _counts;
};

} // namespace

TEST(AttributeConnectionDragTest, PreviewNormalizesExpressionOutputToOperatorInput) {
  AttributeConnectionDrag drag;
  EXPECT_TRUE(drag.Begin(Output()));
  drag.Update(Input());

  auto preview = drag.Preview();
  ASSERT_TRUE(preview);
  EXPECT_EQ("expressionNode", preview->sourceNodeId);
  EXPECT_EQ("outputs:result", preview->sourcePort);
  EXPECT_EQ("widthNode", preview->targetNodeId);
  EXPECT_EQ("inputs:width", preview->targetPort);
  EXPECT_EQ("inputs:width", preview->targetPropertyName);
  EXPECT_FALSE(preview->isRelationship);
  EXPECT_FALSE(preview->isDangling);
  EXPECT_EQ(Vec2d(10.0, 20.0), preview->start);
  EXPECT_EQ(Vec2d(80.0, 35.0), preview->end);
  EXPECT_TRUE(drag.active());
}

TEST(AttributeConnectionDragTest, ReverseDragNormalizesInputToExpressionOutput) {
  AttributeConnectionDrag drag;
  EXPECT_TRUE(drag.Begin(Input()));
  drag.Update(Output());

  auto preview = drag.Preview();
  ASSERT_TRUE(preview);
  EXPECT_EQ("expressionNode", preview->sourceNodeId);
  EXPECT_EQ("outputs:result", preview->sourcePort);
  EXPECT_EQ("widthNode", preview->targetNodeId);
  EXPECT_EQ("inputs:width", preview->targetPort);
  EXPECT_EQ(Vec2d(10.0, 20.0), preview->start);
  EXPECT_EQ(Vec2d(80.0, 35.0), preview->end);
}

TEST(AttributeConnectionDragTest, CursorPreviewIsDanglingAndMovesForBothDirections) {
  AttributeConnectionDrag forward;
  EXPECT_TRUE(forward.Begin(Output()));
  forward.Update(Vec2d(45.0, 60.0));
  auto forwardPreview = forward.Preview();
  ASSERT_TRUE(forwardPreview);
  EXPECT_TRUE(forwardPreview->isDangling);
  EXPECT_EQ("output", forwardPreview->danglingDirection);
  EXPECT_EQ(Vec2d(10.0, 20.0), forwardPreview->start);
  EXPECT_EQ(Vec2d(45.0, 60.0), forwardPreview->end);

  forward.Update(Vec2d(70.0, 90.0));
  forwardPreview = forward.Preview();
  ASSERT_TRUE(forwardPreview);
  EXPECT_EQ(Vec2d(70.0, 90.0), forwardPreview->end);

  AttributeConnectionDrag reverse;
  EXPECT_TRUE(reverse.Begin(Input()));
  reverse.Update(Vec2d(-10.0, 5.0));
  auto reversePreview = reverse.Preview();
  ASSERT_TRUE(reversePreview);
  EXPECT_TRUE(reversePreview->isDangling);
  EXPECT_EQ("input", reversePreview->danglingDirection);
  EXPECT_TRUE(reversePreview->sourceNodeId.empty());
  EXPECT_EQ("widthNode", reversePreview->targetNodeId);
  EXPECT_EQ(Vec2d(-10.0, 5.0), reversePreview->start);
  EXPECT_EQ(Vec2d(80.0, 35.0), reversePreview->end);
  reverse.Cancel();
  EXPECT_FALSE(reverse.Preview());
}

TEST(AttributeConnectionDragTest, DropValidatesAndAuthorsExactlyOnceThroughUndo) {
  NoodlesUndoManager::instance().reset();
  int validations = 0;
  int authors = 0;
  auto counts = std::make_shared<Counts>();
  AttributeConnectionDrag drag({
      [&validations](const AttributeConnectionRequest& request) {
        ++validations;
        return request.source.propertyPath() ==
                   "/Description/Expressions/width.outputs:result" &&
            request.target.propertyPath() == "/Description/Operators/width.inputs:width" &&
            !request.link.isRelationship;
      },
      [&authors, counts](const AttributeConnectionRequest&) {
        ++authors;
        return std::make_unique<RecordingCommand>(counts);
      }});

  EXPECT_TRUE(drag.Begin(Output()));
  drag.Update(Input());
  EXPECT_TRUE(drag.Drop());
  EXPECT_EQ(1, validations);
  EXPECT_EQ(1, authors);
  EXPECT_EQ(1, counts->executes);
  EXPECT_TRUE(NoodlesUndoManager::instance().canUndo());
  EXPECT_FALSE(drag.active());
  EXPECT_FALSE(drag.hover());

  NoodlesUndoManager::instance().undo();
  EXPECT_EQ(1, counts->undoes);
  NoodlesUndoManager::instance().reset();
}

TEST(AttributeConnectionDragTest, CancelAndInvalidDropsNeverAuthor) {
  int validations = 0;
  int authors = 0;
  AttributeConnectionDrag drag({
      [&validations](const AttributeConnectionRequest&) { ++validations; return true; },
      [&authors](const AttributeConnectionRequest&) {
        ++authors;
        return CommandPtr{};
      }});

  EXPECT_TRUE(drag.Begin(Output()));
  drag.Update(Output());
  ASSERT_TRUE(drag.Preview());
  EXPECT_TRUE(drag.Preview()->isDangling);
  EXPECT_FALSE(drag.Drop());
  EXPECT_EQ(0, validations);
  EXPECT_EQ(0, authors);

  EXPECT_TRUE(drag.Begin(Output()));
  drag.Update(Input());
  drag.Cancel();
  EXPECT_FALSE(drag.active());
  EXPECT_FALSE(drag.Drop());
  EXPECT_EQ(0, validations);
  EXPECT_EQ(0, authors);
}

TEST(AttributeConnectionDragTest, RejectedValidationAndInvalidBeginDoNotWrite) {
  int validations = 0;
  int authors = 0;
  AttributeConnectionDrag drag({
      [&validations](const AttributeConnectionRequest&) { ++validations; return false; },
      [&authors](const AttributeConnectionRequest&) {
        ++authors;
        return CommandPtr{};
      }});

  EXPECT_FALSE(drag.Begin({"node", "/Prim", "", true}));
  EXPECT_FALSE(drag.active());
  EXPECT_TRUE(drag.Begin(Output()));
  drag.Update(Input());
  EXPECT_FALSE(drag.Drop());
  EXPECT_EQ(1, validations);
  EXPECT_EQ(0, authors);
}

} // namespace noodles
