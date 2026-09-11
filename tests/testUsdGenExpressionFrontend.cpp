#include "usdGen/expressions/frontend.h"
#include <cstdio>
using namespace usdGen::expr;
int main() {
    int fail=0;
    for (auto const *s : {"4 * 2", "$frame >= 1", "$value * (0.5 + 0.5 * $u)", "$value * $t", "$value * (1 - 0.95 * $t)", "$value * (0.25 + 0.75 * $u)"}) {
        auto r=Frontend::Compile(s, {Domain::Point, ScalarType::Float32, 1});
        if (!r.ok) { ++fail; std::printf("FAIL valid expression %s:\n",s); for (auto const &d:r.diagnostics) std::printf("  %s\n",d.c_str()); }
    }
    if (Frontend::Compile("unknownFile($u)", {Domain::Point,ScalarType::Float32,1}).ok) ++fail;
    if (Frontend::Compile("$pointIndex", {Domain::Groom,ScalarType::Float32,1}).ok) ++fail;
    if (!Frontend::Compile("$frame >= 1", {Domain::Groom,ScalarType::Bool,1}).ok) ++fail;
    if (!Frontend::Compile("4*2", {Domain::Groom,ScalarType::Int32,1}).ok) ++fail;
    if (!Frontend::Compile("$value*0.5", {Domain::Point,ScalarType::Float16,1}).ok) ++fail;
    if (Frontend::Compile("1", {Domain::Groom,ScalarType::Invalid,1}).ok) ++fail;
    std::printf("testUsdGenExpressionFrontend: %s\n", fail ? "FAILED" : "PASS");
    return fail;
}
