// testUsdGenExpr — T0 (plan/07 §9.2:1581-1585): sandboxed SeExpr evaluator
// (usdGen/exprEval.h, T-EXPR-1). Arithmetic, variable binding, parse-error
// and unknown-function rejection with *out left untouched, and determinism.
//
// Tier T0: engine core only, no stage, no Hydra (gate B-1). Standalone
// main(): PASS/FAIL + exit code.

#include "usdGen/exprEval.h"

#include <cstdio>
#include <map>
#include <string>

using usdGen::UsdGenEvalExpr;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    } else {
        std::printf("ok:   %s\n", what.c_str());
    }
}

constexpr double kSentinel = 1234.5;

}  // namespace

int main()
{
    const std::map<std::string, double> noVars;
    const std::map<std::string, double> widthVar{{"width", 2.5}};

    // ---- plain arithmetic --------------------------------------------------
    {
        double out = kSentinel;
        bool ok = UsdGenEvalExpr("32+4*2", noVars, &out);
        Check(ok && out == 40.0, "\"32+4*2\" == 40");
    }

    // ---- variable binding: every var in the map readable -------------------
    {
        double out = kSentinel;
        bool ok = UsdGenEvalExpr("width*2", widthVar, &out);
        Check(ok && out == 5.0, "vars{width=2.5}: \"width*2\" == 5");
    }

    // ---- parse error: false, *out untouched ---------------------------------
    {
        double out = kSentinel;
        bool ok = UsdGenEvalExpr("1+", noVars, &out);
        Check(!ok, "\"1+\" -> false");
        Check(out == kSentinel, "\"1+\" leaves *out unchanged");
    }

    // ---- math built-in: pow(2,3) == 8 IF registered -------------------------
    {
        double out = kSentinel;
        bool ok = UsdGenEvalExpr("pow(2,3)", noVars, &out);
        if (!ok) {
            std::printf("note: pow() NOT registered in the sandbox "
                        "(Expr::addDefaultFuncs without stdlib did not "
                        "provide it) — documented, not a failure\n");
        } else {
            Check(out == 8.0, "\"pow(2,3)\" == 8");
        }
    }

    // ---- unknown function: false, *out untouched ----------------------------
    {
        double out = kSentinel;
        bool ok = UsdGenEvalExpr("nosuchfn(1)", noVars, &out);
        Check(!ok, "\"nosuchfn(1)\" -> false (closed function set)");
        Check(out == kSentinel, "\"nosuchfn(1)\" leaves *out unchanged");
    }

    // ---- determinism: same expr twice, equal results ------------------------
    {
        double a = kSentinel, b = kSentinel;
        bool okA = UsdGenEvalExpr("width*2 + 1", widthVar, &a);
        bool okB = UsdGenEvalExpr("width*2 + 1", widthVar, &b);
        Check(okA && okB && a == b && a == 6.0, "determinism: same expr twice equal (6)");
    }

    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
