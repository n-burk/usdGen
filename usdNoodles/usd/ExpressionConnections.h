// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef NOODLES_USD_EXPRESSION_CONNECTIONS_H
#define NOODLES_USD_EXPRESSION_CONNECTIONS_H

#include "core/AttributeConnectionDrag.h"
#include "usd/api.h"

#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"

#include <string>
#include <vector>

namespace noodles::usd {

/// Spatial rate authored on the consuming operator attribute.  This is an
/// authoring choice, not a claim that every current usdGen executor supports
/// every eligible type/rate combination.
enum class ExpressionEvaluationDomain {
  Groom,
  Primitive,
  Point,
};

/// A data link reconstructed from an existing USD attribute connection.
/// `source` is always an `outputs:*` attribute on UsdGenExpression and
/// `target` is an attribute on a UsdGenOperator in the same description.
struct NOODLES_USD_API ExpressionConnectionLink {
  AttributeEndpoint source;
  AttributeEndpoint target;
  LinkData link;
};

/// Validates a normalized host-neutral drag request against the USD authoring
/// contract.  On failure, `diagnostic` receives a user-facing reason when it
/// is non-null.  Compatibility is exact SdfValueTypeName equality: no value
/// coercion or shape guessing occurs here.
NOODLES_USD_API bool ValidateExpressionConnection(
    const pxr::UsdStageWeakPtr& stage,
    const AttributeConnectionRequest& request,
    std::string* diagnostic = nullptr);

/// Returns an unapplied command that replaces the target's connection list
/// with the requested expression output and authors usdGen:evaluation.  The
/// command retains its stage and captures the current edit target's connection
/// list-op and that one nested customData value.  Undo/redo therefore use the
/// original target even if a host switches targets later, never restore an
/// entire layer, and never overwrite unrelated metadata edits.
NOODLES_USD_API CommandPtr MakeExpressionConnectionCommand(
    const pxr::UsdStageWeakPtr& stage,
    const AttributeConnectionRequest& request,
    ExpressionEvaluationDomain domain);

/// Returns an unapplied command that removes only `request.source` from the
/// target's authored connection edits.  The literal remains untouched; undo
/// restores the exact edit-target list-op captured before removal.
NOODLES_USD_API CommandPtr MakeExpressionDisconnectionCommand(
    const pxr::UsdStageWeakPtr& stage,
    const AttributeConnectionRequest& request);

/// Convenience callbacks for AttributeConnectionDrag.  The returned author
/// creates an unapplied command; AttributeConnectionDrag executes it once and
/// transfers it to NoodlesUndoManager.
NOODLES_USD_API AttributeConnectionDrag::Callbacks MakeExpressionConnectionCallbacks(
    const pxr::UsdStageWeakPtr& stage,
    ExpressionEvaluationDomain domain);

/// Returns valid existing expression-output connections on `operatorPrim` as
/// ordinary Noodles data links.  Invalid or foreign USD connections remain
/// authored in USD but are deliberately not represented as editable SeExpr
/// links by this adapter.
NOODLES_USD_API std::vector<ExpressionConnectionLink> GetExpressionConnections(
    const pxr::UsdPrim& operatorPrim);

} // namespace noodles::usd

#endif // NOODLES_USD_EXPRESSION_CONNECTIONS_H
