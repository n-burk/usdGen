#ifndef USDGEN_EXPRESSIONS_IR_H
#define USDGEN_EXPRESSIONS_IR_H
#include "usdGen/expressions/context.h"
#include <cstdint>
#include <vector>
namespace usdGen::expr {
enum class IROp : uint8_t { Const, LoadVariable, Add, Sub, Mul, Div, Neg, Compare, Select, Min, Max, Clamp, Abs, Sin, Cos, Pow };
struct IRInstruction { IROp op=IROp::Const; uint16_t dst=0,a=0,b=0,c=0; Variable variable=Variable::Invalid; double immediate=0; char compare=0; uint8_t component=0; };
struct IRProgram { std::vector<IRInstruction> instructions; uint16_t result=0; uint16_t output[4]{}; uint8_t outputCount=0; uint8_t valueComponents=1; uint16_t registerCount=0; bool hasLazyBranches=false; };
}
#endif
