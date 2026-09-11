// usdGen engine — sandboxed SeExpr evaluator (T-EXPR-1). See exprEval.h for
// the sandbox contract: private Context (NEVER the global registry), closed
// pure-math function set, deterministic, variables via a private Env.
//
// Build note (exprEval.h): SeExpr2's Expr/Context headers are not vendored
// yet (thirdparty/seexpr is Noise-only, S38). This TU compiles once the
// engine is wired into usdGen_seexpr.
#include "usdGen/exprEval.h"

#include "SeExpr2/Context.h"
#include "SeExpr2/Expr.h"
#include "SeExpr2/ExprEnv.h"

#include <string>
#include <map>

namespace usdGen {

namespace {

/// Read-only Env binding the caller's name -> value map. Unknown names
/// return false, which SeExpr surfaces as an eval error (sandbox: nothing
/// else can leak in — the Context has no other variables).
class UsdGenVarEnv final : public SeExpr2::Env
{
public:
    explicit UsdGenVarEnv(const std::map<std::string, double> &vars)
        : m_vars(vars) {}

    bool getVar(const char *name, SeExpr2::Local &v) const override
    {
        auto it = m_vars.find(name);
        if (it == m_vars.end()) return false;
        v = SeExpr2::Local(it->second);
        return true;
    }

    // setVar stays inherited (returns false): variables are read-only.

private:
    const std::map<std::string, double> &m_vars;
};

}  // namespace

bool UsdGenEvalExpr(const std::string &expr,
                    const std::map<std::string, double> &vars,
                    double *out)
{
    // addDefaultFunctions = false, then seed ONLY the pure-math built-ins
    // (addStdlib = false keeps rand()/noise()/file functions out of the
    // sandbox). The global context is never merged in.
    SeExpr2::Context ctx(/*addDefaultFunctions = */ false);
    SeExpr2::Expr::addDefaultFuncs(&ctx, /*addStdlib = */ false);

    SeExpr2::Expr e(&ctx, expr);
    UsdGenVarEnv env(vars);
    e.setEnv(&env);

    if (!e.parse()) return false;   // syntax error; *out untouched
    if (e.evalError()) return false; // prep error; *out untouched

    const SeExpr2::Value v = e.eval();
    if (e.evalError()) return false; // unknown name / function at eval time
    if (!v.isNumber()) return false; // vector/string result is not a scalar

    if (out) *out = v.toResolvedDouble();
    return true;
}

}  // namespace usdGen
