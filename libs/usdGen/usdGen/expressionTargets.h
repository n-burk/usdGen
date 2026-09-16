// usdGen engine — which operator parameters may be driven by an expression.
//
// ONE table for both lanes. The compiler calls this for every backend, and the
// CUDA admission functions in cudaExecution.cpp call it instead of keeping
// their own copy, so "what can I connect" cannot mean two different things
// depending on where the groom happens to cook.
//
// The rules are the shape/type/domain admission only. Whether the *value* is
// in range is a runtime question each kernel answers for itself.
#ifndef USDGEN_EXPRESSION_TARGETS_H
#define USDGEN_EXPRESSION_TARGETS_H

#include "usdGen/graphDesc.h"

#include "pxr/base/tf/token.h"

#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGen {

/// The property name with the `usdGen:` prefix stripped. Hydra transports
/// native property names; direct descriptor clients may use operator-local
/// ones, and both must compare equal.
TfToken UsdGenCanonicalParamName(TfToken const &name);

/// Validates every binding on `node` against its operator's allowlist.
/// Appends one message per rejection to `errors` (never null-checked away)
/// and returns false if anything was rejected.
bool UsdGenValidateExpressionTargets(UsdGenNodeDesc const &node,
                                     std::vector<std::string> *errors);

} // namespace usdGen

#endif
