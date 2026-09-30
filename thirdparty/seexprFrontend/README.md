# SeExpr2 frontend vendoring

This directory vendors the complete upstream SeExpr2 core from the immutable
`v3.0.1` tag at commit `6b0fc581bd2284aa5593eb19762795a6847049a7` in
`https://github.com/wdas/SeExpr`.

The source includes the parser (`ExprParser.y`, `ExprParserLex.l`), AST,
interpreter, expression API, and builtin functions. It is intentionally kept
separate from `thirdparty/seexpr`, whose reduced contents are used by the
existing noise implementation. The upstream `Noise.cpp`/`Noise.h` were not
replaced; they differ and require a separate compatibility decision.

The upstream license is preserved in `LICENSE` (Apache 2.0 with a modified
trademarks section).
Core-only validation performed from a clean temporary checkout:

```sh
cmake -S . -B build -DENABLE_LLVM_BACKEND=OFF -DENABLE_QT5=OFF \
  -DUSE_PYTHON=OFF -DBUILD_UTILS=OFF -DBUILD_DEMOS=OFF \
  -DBUILD_DOC=OFF -DBUILD_TESTS=OFF -DENABLE_SSE4=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

This builds `libSeExpr2.so` without UI, LLVM, docs, demos, or tests. Integrate
the core through a thin wrapper and strict usdGen domain/type whitelist before
attempting CUDA lowering; do not claim USD bool/int/matrix/array support until
an explicit typed lowering layer exists.

Local integration patches: LLVM-disabled `ExprConfig.h`; generated parser
sources retained for offline builds without flex/bison; plugin auto-loading
from `SE_EXPR_PLUGINS` disabled when `USDGEN_SEEXPR_SANDBOX` is defined.
CMake compiles this private static dependency with `SeExpr2` and
`SeExprInternal2` renamed to usdGen-private namespaces, avoiding ODR conflicts
with the existing noise subset. No third-party headers or shared library are
installed. The frontend still needs a strict function/AST whitelist before
type preparation; disabling plugin loading alone is not a sandbox.
