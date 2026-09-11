// usdGen engine — sandboxed SeExpr expression evaluation (T-EXPR-1).
// Contract: plan/07 §9.2:1581-1585 (T0 `testUsdGenExpr` — "every SeExpr
// variable bound and readable", deterministic math only).
//
// SANDBOX — closed function set:
//   * Each call evaluates against a PRIVATE SeExpr2::Context seeded with
//     ONLY the default pure-math built-ins (sin/cos/pow/sqrt/clamp/...).
//     No file/system/network functions, no stdlib (`addStdlib = false`),
//     no rand()/noise()/time — and the global function registry is never
//     consulted (no addFromContext(&getGlobalContext())).
//   * Deterministic: same (expr, vars) => same result, every call.
//   * Variables: every name in `vars` is bound and readable through a
//     private Env; any other name, or any unknown function, is an error.
//
// BUILD NOTE: needs the SeExpr2 expression engine headers
// (<SeExpr2/Expr.h>, <SeExpr2/Context.h>). This tree currently vendors
// only thirdparty/seexpr/SeExpr2/Noise.{h,cpp} (S38; CMakeLists.txt:73
// "M0 = Noise.cpp + headers only"), so the full engine must be added to
// the usdGen_seexpr target before this TU builds. No CMake touched here.
#ifndef USDGEN_EXPR_EVAL_H
#define USDGEN_EXPR_EVAL_H

#include <map>
#include <string>

namespace usdGen {

/// Evaluate `expr` as a scalar SeExpr expression with `vars` bound as
/// read-only double variables.
///
/// Returns true and writes the result to *out (when out != nullptr) on
/// success. ANY parse or eval error — malformed syntax, unknown variable,
/// unknown/unregistered function, non-scalar result — returns false and
/// leaves *out UNTOUCHED.
bool UsdGenEvalExpr(const std::string &expr,
                    const std::map<std::string, double> &vars,
                    double *out);

}  // namespace usdGen

#endif  // USDGEN_EXPR_EVAL_H
