# SeExpr frontend feasibility and CUDA lowering boundary

## Finding

The checkout under `thirdparty/seexpr/SeExpr2` is not a SeExpr parser
dependency. It contains `Noise.*`, `ExprType.h`, `ExprEnv.h`, and function
declaration headers, but no `Expression.h`, lexer/parser, AST implementation,
interpreter, or build definition. The existing `exprEval.cpp` references the
missing frontend and therefore cannot be made buildable by adding an adapter.
No toy parser is introduced.

The complete upstream core is now vendored under
`thirdparty/seexprFrontend/SeExpr2` from tag `v3.0.1`, commit
`6b0fc581bd2284aa5593eb19762795a6847049a7`. A clean temporary checkout built
`libSeExpr2.so` with UI, LLVM, Python, docs, demos, tests, and SSE4 disabled;
the exact commands and preserved license are recorded in that directory's
README. The existing reduced `thirdparty/seexpr/SeExpr2/Noise.*` remains
untouched: upstream `Noise.cpp` differs and has not been substituted.

The official SeExpr2 API documents `SeExpr2::Expression` as the parser/type
checking entry point, with `isValid()`, `parseError()`, `prep()`, and
`setDesiredReturnType()`. Its `ExprType` has only `tFP`, `tSTRING`, and
`tNONE`, with a floating-point dimension and lifetime (`constant`, `uniform`,
`varying`). It does not model USD bool/int/uint, matrices, or arrays. See the
[Expression API](https://wdas.github.io/SeExpr/doxygen/classSeExpr2_1_1Expression.html)
and [ExprType API](https://wdas.github.io/SeExpr/doxygen/classSeExpr2_1_1ExprType.html).

The official porting notes require linking the complete `SeExpr2` library and
describe its `ExprVarRef`/`ExprFuncSimple` extension points; headers alone are
insufficient. See the [SeExpr2 porting notes](https://wdas.github.io/SeExpr/doxygen/html/SeExpr2_api_porting.html).

## Required implementation

1. Vendor or package one complete, pinned upstream SeExpr2 source release,
   including parser, AST, interpreter, and build metadata. Record the exact
   upstream commit/release and license before changing the dependency.
2. Add a thin `usdGen` frontend wrapper around `Expression`, `ExprType`, and
   the documented variable/function extension points. The wrapper must expose
   parse/type-check diagnostics and never evaluate or author USD values.
3. Bind variables through the stage-free expression registry. Reject unknown
   or unavailable domain variables during preparation; never bind them to zero.
4. Lower only an explicitly whitelisted AST/function set to CUDA IR/source.
   Reject every unsupported node or builtin, including string operations,
   file/map I/O without a device implementation, and host-only functions.
5. Keep type checks strict at the USD boundary. Native float/vector outputs may
   map to SeExpr floating dimensions; USD bool/int/uint/matrix/array outputs
   need explicit frontend/backend support and must not be silently coerced.

## Explicit gaps

Current implementation uses the full private namespaced parser and a scalar-SSA
CUDA interpreter, with up to four output components. Vector variables use
interleaved device fields, scalar operands broadcast, and expression values
execute at the caller's current frame/time. Supported lowering includes numeric
constants/variables, arithmetic, comparisons, unary operations, conditional
selection, vector construction/constant indexing, and
`abs/sin/cos/pow/min/max/clamp`. Unsupported syntax/functions fail closed.
This is an explicitly incomplete language implementation, not a replacement
definition of the required SeExpr function set.

The GPU boundary validates bool as 0/1 and integer conversion as finite, integral
and representable (64-bit conversion is restricted to double's exact-integer
range). Only scalar diagnostics are read back by production evaluator methods;
test oracles explicitly download result fields. Integration status and exact
test checkpoints are recorded in [overlay 14](../14-hierarchy-cuda-implementation.md).

SeExpr2's documented type system cannot by itself represent exact 64-bit
integer arithmetic, typed booleans, matrices, or shaped arrays. Those require a
typed lowering layer extending the parsed AST contract (or a newer frontend
with those types), plus CUDA implementations and shape proofs. Until then,
expression bindings remain fail-closed in `UsdGenCompiler`; there is no claim
of a complete SeExpr or CUDA evaluator.

## Qwen review evidence

`<local-model>` reviewed the vendored-subset situation and recommended
requiring the complete upstream frontend, then applying a strict host-side
AST/function whitelist before CUDA lowering. It specifically warned against a
toy parser and silent host/CUDA type-semantic divergence.
