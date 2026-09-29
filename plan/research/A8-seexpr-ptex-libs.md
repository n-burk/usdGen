# A8 — SeExpr2, Ptex, image IO and third-party dependency strategy for usdGen

Research report for the usdGen (procedural grooming) plan. Everything below is either
cited to a file:line on this machine, cited to a URL, or explicitly marked UNVERIFIED.
Beyond reading source, three things were actually executed on this host (aarch64,
GCC 13.3, CMake 3.28.3) to turn "should work" into "does work":

| Verification | Location | Result |
|---|---|---|
| SeExpr `main` (commit 8f8c8f2, 2026-01-27) built interpreter-only, no Qt/Python/LLVM/SSE4 | `<session-scratch>{seexpr,build-seexpr,install}` | `libSeExpr2.so.2.0` + `share/cmake/seexpr2/seexpr2-config.cmake` produced |
| Ptex `v2.4.3` built static+shared against system zlib | `.../thirdparty/{ptex,build-ptex,install}` | `libPtex.a`, `libPtex.so.2.4`, `lib/cmake/Ptex/ptex-config.cmake` produced |
| SeExpr embedding + micro-benchmark (custom vars, custom `map()` function, 8-thread VarBlock evaluation) | `.../thirdparty/bench/seexpr_bench.cpp` | 13–117 ns/eval single-thread; 50 M evals/s on 8 threads |
| Ptex write/read/filter API test (quad + triangle files, cross-face blend, 8-thread lookups) | `.../thirdparty/bench/ptex_test.cpp` | 23–26 ns/lookup single-thread; 228 M lookups/s on 8 threads |
| Probe linking the installed `hdSt`/`hio` to test Ptex/Hio support of the 26.08 install | `.../scratchpad/ptexprobe/probe.cpp` | `HdStIsSupportedPtexTexture("a.ptx") == 0`; Hio supports png/jpg/bmp/tga/hdr/exr/avif, not tif/tx/ptx |

---

## 0. Host inventory relevant to dependencies

| Item | State | Evidence |
|---|---|---|
| OpenUSD install | 26.08, shared libs, Python 3.12, TBB 2020.3, OpenSubdiv 3.6.1, MaterialX 1.39.5 | `$USD/pxrConfig.cmake` (`PXR_VERSION "2608"`, `find_dependency(OpenSubdiv 3.6.1 CONFIG)`, `find_dependency(MaterialX)`); `$USD/include/tbb/tbb_stddef.h:21-25` (`TBB_VERSION_MAJOR 2020`, `MINOR 3`, `TBB_INTERFACE_VERSION 11103`) |
| Compiler used for the install | GCC 13.3.0 (Ubuntu 24.04) | `strings libusd_tf.so` → `GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0` |
| Ptex in the install | **absent** (no header, no lib, no CMake mention) | `grep -rn PTEX pxrConfig.cmake cmake/` → nothing; `ls include | grep -i ptex` → nothing |
| OpenImageIO / OpenColorIO / OpenVDB plugins | **absent** (all default OFF) | `<openusd-src>/cmake/defaults/Options.cmake:16,20,37`; installed plugins are only `hdStorm, hioAvif, hioOpenEXR, sdrGlslfx, usdShaders` (`ls $USD/plugin/usd`) |
| SeExpr / KSeExpr | absent anywhere on machine | task statement; confirmed by `dpkg -l` grep |
| bison / flex / sed | present: bison 3.8.2, flex 2.6.4 | `which bison flex` |
| Ninja | **absent** (`which ninja` empty; CMake `-G Ninja` fails) — use `Unix Makefiles` or install ninja | configure error seen during verification |
| zlib dev | present (`/usr/include/zlib.h`, zlib1g-dev 1.3) | `dpkg -l` |
| libdeflate | runtime only (`libdeflate.so.0`), **no headers** | `ls /usr/include/libdeflate.h` → missing |
| LLVM | runtime libs only (`libllvm18`, `libllvm20`), no `llvm-config`/dev headers | `dpkg -l`, `llvm-config --version` → empty |
| Python venv | PySide6 6.11.2, PyOpenGL; **no numpy, no pybind11** | `python -m pybind11 --cmakedir` → "No module named pybind11" |
| Arch | aarch64 (NVIDIA GB10) — x86 `-msse4.1` flags must be disabled | `uname`; SeExpr `ENABLE_SSE4` default TRUE adds `-msse4.1` (`seexpr/CMakeLists.txt:98,202`) |

---

## 1. SeExpr

### 1.1 Repository, versions, license, maintenance

| Fact | Value | Evidence |
|---|---|---|
| Repo | https://github.com/wdas/SeExpr (default branch `main`) | WebFetch of repo page |
| Description | "SeExpr is an embeddable, arithmetic expression language that enables flexible artistic control and customization in creating computer graphics images." | repo README |
| Tags (newest first) | `v3.0.1` (2019-11-19), `v1.58.2` (2019-09-09), `v3.0.0` (2019-07-24), `v1.58.1`, `v1.57.1`, `v1.55.2`, `v1.58.0`, `v1.57.0`, `v1.56.0`, `v1.55.1` (2019) | https://github.com/wdas/SeExpr/tags |
| GitHub "Releases" page | lists older milestone names: `v2.0-beta.1` "LLVM evaluation, variable typing and variable blocks", `v2-1.11.4` "First non-beta release", `v1-2.9` (Apache license adoption + UI library), `v1-2.11`, `rel-1.0.1` | https://github.com/wdas/seexpr/releases |
| Main-branch project version string | `project(seexpr2)`, `set(${PROJECT_NAME}_VERSION "2.0")` — i.e. the CMake *package* is versioned 2.0 even though git tags reach v3.0.1 | `.../thirdparty/seexpr/CMakeLists.txt:22-24` |
| Naming | "v3" is the *generation* (typed vars, LLVM, VarBlocks); the C++ namespace and library are **`SeExpr2`** so v1 and v2/v3 can coexist. "In order to allow both older and newer versions of SeExpr to coexist, all classes were renamed and an 'SeExpr2' namespace was added." | https://wdas.github.io/SeExpr/doxygen/html/SeExpr2_api_porting.html |
| Recent activity | 2026-01-27 "maint: add tags.yaml for TWDC"; 2026-01-08 "build: allow disabling LLVM using make ENABLE_LLVM_BACKEND=OFF"; 2026-01-07 "cmake: set the C++ standard to c++17"; before that Sept 2023 | https://github.com/wdas/SeExpr/commits/main |
| License | **Apache 2.0 with Section 6 (Trademarks) replaced** (Disney's "modified Apache" — same form as OpenUSD's TOST and OpenSubdiv's license) | `.../thirdparty/seexpr/LICENSE:1-11`: "Licensed under the Apache License, Version 2.0 ... and the following modification to it: Section 6 Trademarks. deleted and replaced with: 6. Trademarks. This License does not grant permission to use the trade names, trademarks..." |

### 1.2 Build system, dependencies and options (verified locally)

Top-level options (`.../thirdparty/seexpr/CMakeLists.txt:96-104`):

```cmake
option(ENABLE_LLVM_BACKEND "Whether to build with LLVM backend" TRUE)
option(ENABLE_QT5 "Whether to use Qt5" TRUE)
option(ENABLE_SSE4 "Whether to use SSE4" TRUE)
option(USE_PYTHON "Whether to compile python libraries" TRUE)
option(BUILD_UTILS "Whether to build the utilities" TRUE)
option(BUILD_DEMOS "Whether to build the demos" TRUE)
option(BUILD_DOC "Whether to build the documentation" TRUE)
option(BUILD_TESTS "Whether to build the tests" TRUE)
option(ENABLE_SLOW_TESTS "Whether to enable slow tests" FALSE)
```

Other build facts:

| Fact | Evidence |
|---|---|
| C++17 (`set(CMAKE_CXX_STANDARD 17)`) | `CMakeLists.txt:197` |
| `-msse4.1` appended when `ENABLE_SSE4` (x86 only — **must be OFF on aarch64**) | `CMakeLists.txt:201-202` |
| LLVM: `find_package(LLVM CONFIG ...)`; refuses `LLVM_VERSION VERSION_LESS 3.8.0`; sets `SEEXPR_ENABLE_LLVM_BACKEND` which becomes `#define SEEXPR_ENABLE_LLVM` in generated `ExprConfig.h` | `CMakeLists.txt:110-164`; `src/SeExpr2/ExprConfig.h.in` (`#if @SEEXPR_ENABLE_LLVM_BACKEND@ / #define SEEXPR_ENABLE_LLVM`) |
| Qt only for `src/SeExpr2/UI` (editor widgets) — core lib has no Qt dependency | `CMakeLists.txt:221-232, 279-281` |
| Python: uses deprecated `FindPythonInterp/PythonLibs` + Boost.Python; entirely optional (`USE_PYTHON=OFF`) | `CMakeLists.txt:77-89` |
| Parser: `find_program(BISON_EXE bison)`, `FLEX_EXE`, `SED_EXE`; with all three present it regenerates `ExprParser.cpp`/`ExprParserLex.cpp` via custom commands that run `flex`, `bison --defines --verbose --fixed-output-files -p SeExpr2`, then `sed 's/yy/SeExpr2/g; s/YY/SeExprYY/g'`. Without them it looks for `${PREGENERATED_ROOT}/SeExpr/generated/ExprParser.cpp` — but **`src/SeExpr2/generated/` in the repo contains only a `.gitignore`**, so bison/flex/sed are effectively required (Windows: they ship pre-generated files elsewhere — UNVERIFIED) | `src/SeExpr2/CMakeLists.txt:31-74`; `ls src/SeExpr2/generated` |
| Library target: `add_library(SeExpr2 SHARED ...)` on non-Windows, STATIC on Windows; links `dl pthread`; `SOVERSION 2` | `src/SeExpr2/CMakeLists.txt:83-90` |
| Exported package: `share/cmake/seexpr2/seexpr2-config.cmake` + `seexpr2-exports.cmake` defining imported target **`seexpr2::SeExpr2`** with `INTERFACE_INCLUDE_DIRECTORIES ${prefix}/include`, `INTERFACE_LINK_LIBRARIES "dl;pthread"`; headers under `include/SeExpr2/*.h` (consumers `#include <SeExpr2/Expression.h>`) | `CMakeLists.txt:70-71, 311-326`; verified install `.../thirdparty/install/share/cmake/seexpr2/seexpr2-exports.cmake:59-64` |
| Source size (interpreter-only build): ~30 .cpp/.h files; `Noise.cpp` 247 lines + `NoiseTables.h` 2124 lines; `ExprBuiltins.cpp` 1851; `Interpreter.cpp` 1058; `ExprLLVMCodeGeneration.cpp` 1173 (only compiled with LLVM) | `wc -l` on the clone |

Verified configure/build command that works on this host (took ~1 min):

```sh
cmake -S seexpr -B build-seexpr -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LLVM_BACKEND=OFF -DENABLE_QT5=OFF -DENABLE_SSE4=OFF -DUSE_PYTHON=OFF \
  -DBUILD_UTILS=OFF -DBUILD_DEMOS=OFF -DBUILD_DOC=OFF -DBUILD_TESTS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=<prefix>
```

Configure noise to expect: CMP0148 warning (FindPythonInterp), "Found unsuitable Qt version" (harmless with `ENABLE_QT5=OFF`), one `-Wmaybe-uninitialized`-class warning in `ExprNode.cpp:265`. No errors.

### 1.3 Embedding API (verbatim from the clone)

**Variables** — subclass `ExprVarRef` (`src/SeExpr2/Expression.h:45-70`):

```cpp
class ExprVarRef {
  public:
    ExprVarRef(const ExprType& type) : _type(type) {};
    virtual void setType(const ExprType& type) { _type = type; };
    virtual ExprType type() const { return _type; };
    //! returns this variable's value by setting result
    virtual void eval(double* result) = 0;          // Expression.h:64
    virtual void eval(const char** resultStr) = 0;  // Expression.h:65
};
```

**Pitfall verified the hard way:** `eval(double*)` must write exactly `type().dim()` doubles. Writing 3 doubles for an `FP(1)` variable silently corrupts the interpreter's adjacent constant slots (`$u*2` evaluated to 0) and later crashed with `free(): invalid pointer`. The interpreter stores all doubles in one flat `std::vector<double> d` (`Interpreter.h:43`).

**Expression** (`src/SeExpr2/Expression.h`):

| Member | Line | Note |
|---|---|---|
| `enum EvaluationStrategy { UseInterpreter, UseLLVM }` | 79 | |
| `Expression(const std::string& e, const ExprType& type = ExprType().FP(3), EvaluationStrategy be = defaultEvaluationStrategy, const Context& context = Context::global())` | 106 | desired return type is FP(3) by default — pass `ExprType().FP(1)` for scalars |
| `void setDesiredReturnType(const ExprType&)` | 115 | |
| `bool isValid() const` | 133 | parses+preps lazily; "Variables and functions will also be bound." |
| `const std::string& parseError() const` | 140 | |
| `bool isThreadSafe() const { return _threadUnsafeFunctionCalls.size() == 0; }` | 162 | only reports whether any *called function* declared itself unsafe |
| `bool isVec() const` / `const ExprType& returnType() const` | 177 / 182 | |
| `const double* evalFP(VarBlock* varBlock = nullptr) const` | 189 | returns pointer into interpreter (or VarBlock) storage |
| `const char* evalStr(VarBlock* = nullptr) const` | 193 | |
| `void evalMultiple(VarBlock*, int outputVarBlockOffset, size_t rangeStart, size_t rangeEnd) const` | 185 | batch loop over `indirectIndex` |
| `virtual ExprVarRef* resolveVar(const std::string& name) const { return 0; }` | 199 | override to bind `$u`, `$P`, ... |
| `virtual ExprFunc* resolveFunc(const std::string& name) const { return 0; }` | 202 | override to add `map()`, etc. without touching the global table |
| `void setVarBlockCreator(const VarBlockCreator*)` | 229 | "lifetime of expression must be <= block" |
| `mutable Interpreter* _interpreter; mutable LLVMEvaluator* _llvmEvaluator;` | 306/310 | prep state is `mutable` and created on first `isValid()`/`evalFP()` |

**Custom functions** — two routes:

1. *Plain C function pointers* wrapped in `ExprFunc` (thread-safe assumed). Typedefs (`src/SeExpr2/ExprFuncStandard.h:50-63`):
   ```cpp
   typedef double Func0(); ... typedef double Func6(double x6);
   typedef double Func1v(const Vec3d&);  typedef double Func2v(const Vec3d&, const Vec3d&);
   typedef Vec3d  Func1vv(const Vec3d&); typedef Vec3d  Func2vv(const Vec3d&, const Vec3d&);
   typedef double Funcn(int n, double* params);
   typedef double Funcnv(int n, const Vec3d* params);
   typedef Vec3d  Funcnvv(int n, const Vec3d* params);
   ```
   with constructors `ExprFunc(ExprFuncStandard::Funcnv* f, int minArgs, int maxArgs)` etc. (`ExprFunc.h:87-118`) and global registration `static void ExprFunc::define(const char* name, ExprFunc f, const char* docString)` (`ExprFunc.h:62`), lookup `ExprFunc::lookup(name)` (`:68`), `ExprFunc::init()` also loads plugins from a colon-delimited `SE_EXPR_PLUGINS` env var (`ExprFunc.h:49-50`, `ExprFunc.cpp:158-159`). The global table is guarded by a mutex (`ExprFunc.cpp:110-134`).

2. *`ExprFuncSimple`* for functions that need typed args, strings or per-call-site constant data (`src/SeExpr2/ExprFuncX.h:72-113`):
   ```cpp
   class ExprFuncSimple : public ExprFuncX {
     public:
       ExprFuncSimple(const bool threadSafe) : ExprFuncX(threadSafe) {}
       class ArgHandle {   // ExprFuncX.h:76
           template <int d> Vec<double, d, true> inFp(int i);   // :85
           char* inStr(int i);                                   // :88
           int nargs() const;
           template <int d> Vec<double, d, true> outFpHandle();
           double& outFp; char*& outStr; ExprFuncNode::Data* data;   // :97-99
       };
       virtual ExprType prep(ExprFuncNode* node, bool scalarWanted, ExprVarEnvBuilder& envBuilder) const = 0; // :111
       virtual ExprFuncNode::Data* evalConstant(const ExprFuncNode* node, ArgHandle args) const = 0;         // :112
       virtual void eval(ArgHandle args) = 0;                                                                // :113
   };
   ```
   `prep` validates args with `bool ExprFuncNode::checkArg(int argIndex, ExprType type, ExprVarEnvBuilder&)` (`ExprNode.h:534`), `isStrArg(n)`/`getStrArg(n)` (`ExprNode.h:554-557`), and `node->addError(...)`; `evalConstant` runs once at prep with the constant arguments and returns a `ExprFuncNode::Data` (base struct at `ExprNode.h:78`) that `eval` receives via `args.data` — this is exactly where a `map("path")` implementation opens/looks up its texture once and where a `curve(...)` bakes its ramp (see `CurveFuncX`, `ExprBuiltins.cpp:1295-1340`). Wrap with `ExprFunc(ExprFuncX& f, int min = 1, int max = 1)` (`ExprFunc.h:87`); the `ExprFunc` does **not** own the `ExprFuncX` (it stores `&f`), so keep the functor object alive (static or owned by the plugin).

   Verified working example (from `.../bench/seexpr_bench.cpp`): a `MapFuncX : ExprFuncSimple` whose `prep` does `node->checkArg(0, ExprType().String().Constant(), env)` and returns `ExprType().FP(1).Varying()`, returned from an overridden `resolveFunc("map")` — no global registration needed.

**Types** (`src/SeExpr2/ExprType.h`): `ExprType().FP(d)` for d in 1..16, `.String()`, lifetimes `.Constant()/.Uniform()/.Varying()`, queries `isFP(d)`, `dim()`, `isString()`; helper `TypeVec(n) == ExprType().FP(n).Varying()`. Vec type: `template <class T, int d, bool ref = false> class Vec` with `Vec3d`, `Vec3dRef`, `Vec3dConstRef` aliases (`Vec.h`).

**VarBlock — the fast/parallel path** (`src/SeExpr2/VarBlock.h`):

```cpp
class VarBlock {                     // :33  "A thread local evaluation context. Just allocate and fill in with data."
    double*& Pointer(uint32_t variableOffset);   // :57  pointer to the variable's data
    int indirectIndex;               // :62  "_dataPtrs[someAttributeOffset][indirectIndex]"
    bool threadSafe;                 // :65  "if true, interpreter's data will be copied to this instance before evaluation."
};
class VarBlockCreator {              // :84
    int registerVariable(const std::string& name, const ExprType type);  // :101
    VarBlock create(bool makeThreadSafe = false);                        // :121 "one needed per thread"
    ExprVarRef* resolveVar(const std::string& name) const;               // :126 call from your resolveVar
};
```

With `indirectIndex`, a VarBlock pointer can address SoA arrays (`ptr[indirectIndex]`), which is how `evalMultiple` iterates `rangeStart..rangeEnd` (`Expression.cpp` evalMultiple: sets `varBlock->indirectIndex = i` then `evalFP`). This maps directly onto "evaluate one expression over N hairs/points" without per-hair `ExprVarRef` virtual calls.

### 1.4 Evaluation modes

`Expression.cpp:44-67`: `chooseDefaultEvaluationStrategy()` returns `UseLLVM` when compiled with `SEEXPR_ENABLE_LLVM` unless env `SE_EXPR_EVAL` is set to something other than `"LLVM"`; without LLVM it is always `UseInterpreter`. `evalFP` (`Expression.cpp:304-309`) dispatches: interpreter → `_interpreter->eval(varBlock)` and returns `&(varBlock->d[_returnSlot])` when `varBlock->threadSafe`, else `&_interpreter->d[_returnSlot]`.

The interpreter is a fixed-slot register machine: `Interpreter::eval(VarBlock* block, bool debug)` (`Interpreter.cpp:31-`) executes `ops[pc].first(opData, fp, str, callStack)` in a loop; when `block->threadSafe == true` it first `memcpy`s the whole double/string arrays into the block (`Interpreter.cpp:39-43`). LLVM mode requires LLVM ≥ 3.8 dev headers at build time (not present here) and — per the FAQ — has higher `prep()` cost but faster steady-state evaluation ("running a very simple example expression only once will actually be slower using LLVM", https://wdas.github.io/SeExpr/doxygen/html/SeExpr_FAQ.html). Recommendation: interpreter-only; revisit LLVM only if profiling shows expression evaluation dominating (measurements below suggest it will not).

### 1.5 Thread safety / reentrancy model (verified)

- One `Expression` object evaluated concurrently **without** a thread-safe VarBlock is a data race: `evalFP()` writes into the shared `_interpreter->d` (`Expression.cpp:309`).
- Correct pattern (verified in `seexpr_bench.cpp`, 8 threads × 1 M evals, deterministic result): one `VarBlockCreator`, `registerVariable` for each `$var`, one `Expression` with `setVarBlockCreator`, then **per thread** `VarBlock block = creator.create(true)` and `block.Pointer(off) = &myThreadLocalStorage`, `expr.evalFP(&block)`.
- Cost of `threadSafe`: the memcpy of the interpreter arrays per eval (`Interpreter.cpp:39-46`) — measured 128 ns vs 117 ns for the same noise expression without a VarBlock, i.e. ~10 % overhead, and the copy scales with expression size.
- Alternative pattern: one `Expression` per thread (each with its own `ExprVarRef`s). Prep is cheap (7–53 µs measured), so cloning per worker is fine.
- `ExprFuncX(bool threadSafe)` lets a custom function declare itself unsafe; `Expression::isThreadSafe()` (`Expression.h:162`) then reports false. Builtins with hidden state (`voronoi` family uses a `VoronoiPointData`) are handled internally via per-call `Data`.
- Global function registration (`ExprFunc::define`) is mutex-guarded but must happen before parallel prep (`ExprFunc.cpp:139-147` "NOT THREAD SAFE, it assumes you have a mutex from callee").

### 1.6 Performance (measured on this host, interpreter, `-O2`)

From `.../thirdparty/bench/seexpr_bench.cpp` output:

| Expression | prep | ns / eval (1 thread) |
|---|---|---|
| `$u*$v+1` | 53 µs (first-ever includes builtin init) | **13** |
| `fit(noise($P*4)+0.5*fbm($P*2,4),0,1.5,0.2,1.0)` | 13 µs | **117** |
| `map("length")*(0.5+hash($id))` (custom `map` ExprFuncSimple) | 7 µs | **34** |
| `$c = cellnoise($P*5); voronoi($P*3,1,0.5)*$c + smoothstep($u,0.2,0.8)` | 18 µs | **106** |
| VarBlock(threadSafe) noise+fbm expression, 1 thread | – | 128 |
| Same, 8 threads (one VarBlock each) | – | 159 ns/eval/thread → **50 M evals/s aggregate** |

Ballpark: a typical host-groomer per-hair attribute expression (one noise + fbm + remap) costs ~0.1 µs; 1 M hairs × 10 attributes ≈ 1 s single-threaded, ≈ 0.15 s on 8 threads. That is interactive for regenerate-on-edit if evaluation is only redone for dirty attributes.

### 1.7 Builtins present in SeExpr2 (verified registration)

Registered in `defineBuiltins()` (`ExprBuiltins.cpp:1719-1849`; `FUNCNDOC(func,min,max)` macro at `:1756`): math (`abs acos asin atan atan2 ceil cos cosh exp floor fmod log log10 pow sin sinh sqrt tan tanh cbrt asinh acosh atanh trunc`), degree variants (`deg rad cosd sind tand acosd asind atand atan2d`), `clamp round max min invert compress expand fit gamma bias contrast boxstep linearstep smoothstep gaussstep mix hsi(4..5) midhsi(5..7) hsltorgb rgbtohsl hash(1..n)`, noise: `noise(1..4) snoise vnoise cnoise snoise4 vnoise4 cnoise4 pnoise turbulence vturbulence cturbulence fbm vfbm cfbm fbm4 vfbm4 cfbm4 cellnoise ccellnoise voronoi(1..7) cvoronoi pvoronoi`, vector: `dist length norm dot cross angle ortho rotate up`, selection: `cycle pick choose wchoose`, curves: `spline(5..n) curve ccurve`, `printf sprintf`. Full user documentation with signatures: https://wdas.github.io/SeExpr/doxygen/userdoc.html.

**Not present:** `rand()` — verified at runtime: `Function rand has no definition` (`.../bench/se_min.cpp` run). The userdoc still documents `rand([min,max],[seed])`, but the current `ExprBuiltins.cpp` registers only `hash`. A host groomer exposes `rand` (below), so usdGen must add it (`hash($id, $seed)`-style, or a `Funcn` wrapper).

Noise implementation to reuse from C++ stylers without the expression layer (`src/SeExpr2/Noise.h:23-36`):

```cpp
template <int d_in, int d_out, class T> void Noise(const T* in, T* out);                    // one octave Perlin (quintic interpolant, Noise.cpp:45)
template <int d_in, int d_out, class T> void PNoise(const T* in, const int* period, T* out);
template <int d_in, int d_out, bool turbulence, class T> void FBM(const T* in, T* out, int octaves, T lacunarity, T gain);
template <int d_in, int d_out, class T> void CellNoise(const T* in, T* out);
```

and the higher-level `SeExpr2::noise/snoise/vnoise/cnoise/fbm/vfbm/turbulence/cellnoise/pnoise/hash` in `ExprBuiltins.h`. SeExpr `noise()` returns **0..1** (0.5 at lattice points — verified: `noise($P*4)` at integer P printed 0.5), `snoise` is −1..1.

### 1.8 The a host groomer dialect that users expect

A host groomer expressions are SeExpr with a host groomer-provided globals and a few extra functions. From Autodesk's reference pages (2014 reference: https://download.autodesk.com/global/docs/maya2014/en_us/files/GUID-AFB8F7F3-DCCC-414A-9EC3-83B97FCC8C30.htm ; 2018 globals: https://help.autodesk.com/cloudhelp/2018/ENU/Maya-CharEffEnvBuild/files/GUID-A502A05B-0AF2-4C66-8F64-0D5363BF0A38.htm):

Global variables (floats): `$u`, `$v` ("The u/v parameter of the underlying surface"), `$id` ("The current primitive's ID"), `$frame`, `$cLength`/`$cWidth`/`$cDepth` ("final, computed length/width/depth"), `$descId`, `$faceid`/`$faceId`, `$patchId` (`$objectId` alias), `$aCount`. Vectors: `$P $Pg $Pref $Prefg` (+ world variants `$Pw $Pgw $Prefw $Prefgw`), `$dPdu $dPdug $dPduref $dPdurefg`, `$dPdv ...`, normals `$N $Ng $Nref $Nrefg`; image `$Cs`, `$As`. The 2018 page also documents `$cid` (description id), `$ptexId`, `$fitR` (search snippet — treat exact semantics as UNVERIFIED). Primitive-space variables used in guide/modifier expressions: `$t` (0 root → 1 tip), `$Prefg`, per https://jesusfc.net/blog/xgen-expressions-for-vfx.

A host groomer-specific functions: `map("mapname" [, s, t] [, channel])` — "Evaluates map at current or provided coordinates"; `rand([min, max] [, seed])`; `noise([x][,y][,z])` documented by a host groomer as "Perlin noise function...between -1 and 1" (**different range from SeExpr's 0..1 `noise`** — usdGen must decide which convention to expose; recommend keeping SeExpr semantics and providing `snoise`, documenting the difference); `alignU/alignV/alignN([X])`, `shadow(x)`, `component(x,y,z)`, `dist(x1,y1,z1,x2,y2,z2)` (a host groomer's 6-float form), `remap`, `fit`, `cycle`, `pick`, `curve/ccurve`... (the 2014 page does not list `vmap()`/`cvar()` — UNVERIFIED whether they exist). Painted-map syntax (Autodesk "Basic expression examples", https://help.autodesk.com/cloudhelp/2016/ENU/Maya/files/GUID-72119439-1CDA-4094-BB94-03BECF23DF3A.htm):

```
$a =map('${DESC}/paintmaps/length/'); #3dpaint, 200
$a
```

where `${DESC}` is the description directory macro, the path names a *directory of per-patch Ptex files* ("a DCC creates sub-folders inside the Description for these Ptex files", https://help.autodesk.com/cloudhelp/2017/ENU/Maya/files/GUID-56075F6B-D1C9-4056-BDBC-9766C44AB88C.htm), and the trailing `#3dpaint, 200` comment carries the paint-tool texel resolution; `#0.10,1.00` comments carry slider ranges (`$min= 0.4000; #0.10,1.00`). `${PAL,name}` references palette expressions (`map( "baseCoat_${PAL,myPick}" )`). SeExpr preserves comments (`Expression::_comments`, `Expression.h:291`) so a UI can read these annotations.

Implication for usdGen: the "variables" are all things a groom evaluator already has per hair root (surface uv, face id, P/N/dPdu/dPdv in rest and deformed space, primitive id, frame); map lookup is a custom function bound to the plugin's texture prims; `rand` must be added; `${DESC}`-style macros become a pre-substitution over the expression string (asset paths resolved through Ar).

### 1.9 KSeExpr (Krita fork) — evaluated, not recommended

| Fact | Evidence |
|---|---|
| Repo https://invent.kde.org/graphics/kseexpr ; "The embeddable expression engine fork for Krita"; created 2020 (GSoC) | WebFetch |
| Tags: `v6.0.0.0` (2025-01-09, "Qt6 release"), `v4.0.4.0` (2021-12-14), `v4.0.0.0` (2020-11-12), `v3.4.4.0` (2020-09-28), plus upstream `v3.0.1`, `v1.58.2` | https://invent.kde.org/graphics/kseexpr/-/tags |
| License: **GPL-3.0-or-later** for the fork's changes, layered on Disney's modified Apache 2.0; headers carry both SPDX tags | `src/KSeExpr/Expression.h` header: `SPDX-License-Identifier: LicenseRef-Apache-2.0` and `SPDX-License-Identifier: GPL-3.0-or-later` |
| Namespace renamed to `KSeExpr`; "not ABI-compatible with projects using upstream SeExpr"; Qt ≥ 5.9 is a **core** requirement; C++14; LLVM optional; adds `USE_PREGENERATED_FILES` | README (WebFetch) |

GPL-3 is incompatible with shipping a permissively licensed USD plugin, and the hard Qt dependency is undesirable in a Hydra scene-index library. Use upstream `wdas/SeExpr` `main`. (Krita's fork does show the upstream code base is portable and that pre-generated parser files are a solved problem if Windows builds ever matter.)

### 1.10 Recommendation (SeExpr)

Vendor upstream `wdas/SeExpr` at commit `8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc` (2026-01-27; there is no tag after v3.0.1 and the C++17 fix is only on main) via `FetchContent` with `ENABLE_LLVM_BACKEND=OFF ENABLE_QT5=OFF ENABLE_SSE4=OFF USE_PYTHON=OFF BUILD_*=OFF`. Build it as a **static** library with PIC and hide it inside `usdGen`'s shared library (SeExpr's CMake only offers SHARED on non-Windows — either patch via `add_library` override, or set `BUILD_SHARED_LIBS`-equivalent by editing one line in a vendored copy; simplest: vendor the ~30 core files under `thirdparty/SeExpr2/` with a 20-line CMakeLists that runs bison/flex — the parser regeneration is 3 commands, `src/SeExpr2/CMakeLists.txt:42-74`). Static linking avoids a second `libSeExpr2.so.2` colliding with a host application's copy of SeExpr (symbol clashes with host plugins are UNVERIFIED for 2026 versions). Expose: `$u $v $id $faceId $P $N $dPdu $dPdv $Pref $Nref $t $frame $cLength...` via a `VarBlockCreator`; custom `map()`, `rand()`, `ptex()`; evaluate with one thread-safe VarBlock per TBB worker.

---

## 2. Ptex

### 2.1 Repository, versions, license

| Fact | Evidence |
|---|---|
| Repo https://github.com/wdas/ptex; "Ptex is a texture mapping system developed by Walt Disney Animation Studios for production-quality rendering"; layout `src/ptex` (library), `src/utils` (`ptxinfo`), `src/tests`, `src/doc`, `src/build` | WebFetch |
| Tags: **v2.5.2** (2026-04-11), v2.5.1 (2025-12-12), v2.5.0 (2025-12-09), **v2.4.3** (2024-06-11), v2.4.2 (2022-08-05), v2.4.1, v2.4.0 (2021-05-19), v2.3.2 (2019) | https://github.com/wdas/ptex/tags (GitHub "Releases" page is empty) |
| **v2.5.0 replaced zlib with libdeflate** (`find_package(libdeflate REQUIRED)` on `main`; Fedora heads-up "ptex 2.5.0 soversion change", commit `99d7a3b3`) — v2.4.3 uses `find_package(ZLIB REQUIRED)` | `main` CMakeLists (WebFetch) vs `.../thirdparty/ptex/CMakeLists.txt:36` (`find_package(ZLIB REQUIRED)`); http://www.mail-archive.com/devel@lists.fedoraproject.org/msg210559.html |
| C++ standard: v2.4.3 defaults to **C++98** unless `CMAKE_CXX_STANDARD`/`CXXFLAGS_STD` is set (`CMakeLists.txt:11-17`); `main` defaults to C++17 | local clone; WebFetch of main |
| Options: `PTEX_BUILD_STATIC_LIBS ON`, `PTEX_BUILD_SHARED_LIBS ON`, `PTEX_BUILD_DOCS ON` (needs Doxygen), `PRMAN_15_COMPATIBLE_PTEX OFF` | `.../thirdparty/ptex/CMakeLists.txt:4-7` |
| License: **BSD-3-Clause, Disney variant** ("PTEX SOFTWARE Copyright 2014 Disney Enterprises, Inc." … third clause: the names Disney/Walt Disney Pictures/WDAS "may NOT be used to endorse or promote products derived from this software without specific prior written permission") | `.../thirdparty/ptex/LICENSE:1-8`; https://raw.githubusercontent.com/wdas/ptex/main/LICENSE |

### 2.2 Build and CMake package (verified locally, v2.4.3)

```sh
cmake -S ptex -B build-ptex -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DPTEX_BUILD_DOCS=OFF -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=<prefix>
```

Produces (`src/ptex/CMakeLists.txt:17-46`): `Ptex_static` (OUTPUT_NAME `Ptex`, PUBLIC define `PTEX_STATIC`) and `Ptex_dynamic` (`libPtex.so.2.4`, `SOVERSION major.minor`, PRIVATE `PTEX_EXPORTS`); headers `PtexExports.h PtexHalf.h PtexInt.h PtexPlatform.h Ptexture.h PtexUtils.h PtexVersion.h`; exported package **`lib/cmake/Ptex/ptex-config.cmake`** with imported targets **`Ptex::Ptex_static`** (`INTERFACE_COMPILE_DEFINITIONS "PTEX_STATIC"`, `INTERFACE_LINK_LIBRARIES "Threads::Threads;ZLIB::ZLIB"`) and **`Ptex::Ptex_dynamic`** (`.../install/lib/cmake/Ptex/ptex-exports.cmake:59-72`); the config calls `find_dependency(Threads)` and `find_package(ZLIB REQUIRED)` (`.../install/lib/cmake/Ptex/ptex-config.cmake`). Also `share/pkgconfig/ptex.pc`. Build time on this host: under a minute. The utilities/tests (`ptxinfo`, `wtest`, `rtest`, `ftest`, `halftest`) build too.

These are the same target names OpenUSD expects: `pxr/imaging/hdSt/CMakeLists.txt:35-38` appends `Ptex::Ptex_dynamic` and the `ptexMipmapTextureLoader` class when `PXR_ENABLE_PTEX_SUPPORT` is on.

Choice: **pin v2.4.3** (zlib, available on every platform including this host) unless libdeflate is added as a second FetchContent (MIT, exports `libdeflate::libdeflate_static`, https://github.com/ebiggers/libdeflate). Ptex file format is unchanged between the two, so files interoperate.

### 2.3 C++ API (verbatim, `.../thirdparty/ptex/src/ptex/Ptexture.h`)

Enums (`:66-99`):

```cpp
enum MeshType { mt_triangle, mt_quad };
enum DataType { dt_uint8, dt_uint16, dt_half, dt_float };
enum EdgeFilterMode { efm_none, efm_tanvec };          // tanvec: rotate tangent-space vectors across edges
enum BorderMode { m_clamp, m_black, m_periodic };
enum EdgeId { e_bottom /*(0,0)->(1,0)*/, e_right /*(1,0)->(1,1)*/, e_top /*(1,1)->(0,1)*/, e_left /*(0,1)->(0,0)*/ };
```

`struct Res { int8_t ulog2, vlog2; int u() const {return 1<<ulog2;} int v() const; int size() const; ... }` (`:159`) — per-face resolution is a power of two in each direction. `struct FaceInfo { Res res; uint8_t adjedges; uint8_t flags; int32_t adjfaces[4]; ... EdgeId adjedge(int eid) const; int adjface(int eid) const; bool isSubface() const; void setadjfaces(f0,f1,f2,f3); void setadjedges(e0,e1,e2,e3); enum { flag_constant=1, flag_obsolete=2, flag_nbconstant=4, flag_subface=8 }; }` (`:229`).

Reading:

```cpp
PTEXAPI static PtexTexture* PtexTexture::open(const char* path, Ptex::String& error, bool premultiply=0);   // :474
virtual void getData(int faceid, void* buffer, int stride) = 0;                       // :551 full-res face, v-major, channels interleaved
virtual void getData(int faceid, void* buffer, int stride, Ptex::Res res) = 0;        // mip-reduced
virtual PtexFaceData* getData(int faceid) = 0;                                        // :567 tiled/low-level accessor
virtual void getPixel(int faceid, int u, int v, float* result, int firstchan, int nchannels) = 0;               // :592 (integer texel coords)
virtual void getPixel(int faceid, int u, int v, float* result, int firstchan, int nchannels, Ptex::Res res) = 0; // :606
virtual const Ptex::FaceInfo& getFaceInfo(int faceid) = 0;  virtual int numFaces() = 0;  virtual MeshType meshType() = 0;
virtual DataType dataType() = 0;  virtual int numChannels() = 0;  virtual int alphaChannel() = 0;  virtual bool hasMipMaps() = 0;
```

Cache (`:668-790`):

```cpp
PTEXAPI static PtexCache* PtexCache::create(int maxFiles, size_t maxMem, bool premultiply=false,
                                            PtexInputHandler* inputHandler=0, PtexErrorHandler* errorHandler=0);  // :711
virtual void setSearchPath(const char* path) = 0;      // :725  colon-separated dirs for relative paths
virtual PtexTexture* get(const char* path, Ptex::String& error) = 0;   // :757  (ref-counted; release() returns it to LRU)
virtual void purge(PtexTexture*) / purge(const char* path) / purgeAll();
struct Stats { size_t memUsed, peakMemUsed, filesOpen, peakFilesOpen, filesAccessed, fileReopens, blockReads; }; virtual void getStats(Stats&) = 0; // :779-790
```

Filtering (`:926-1000`):

```cpp
enum FilterType { f_point, f_bilinear, f_box, f_gaussian, f_bicubic /*sharpness*/, f_bspline, f_catmullrom, f_mitchell };
struct Options { int __structSize; FilterType filter; bool lerp /*between mip levels*/; float sharpness; bool noedgeblend /*"Disable cross-face filtering"*/;
                 Options(FilterType filter_=f_box, bool lerp_=0, float sharpness_=0, bool noedgeblend_=0); };
PTEXAPI static PtexFilter* PtexFilter::getFilter(PtexTexture* tx, const Options& opts);   // :971
virtual void eval(float* result, int firstchan, int nchannels, int faceid, float u, float v,
                  float uw1, float vw1, float uw2, float vw2, float width=1, float blur=0) = 0;  // :997
```

"The filter region is a parallelogram centered at the given (u,v) coordinate with sides defined by two vectors [uw1, vw1] and [uw2, vw2]. For an axis-aligned rectangle, the vectors are [uw, 0] and [0, vw]" (`:977-981`). For a hair root sampled at surface (u,v) with no screen-space derivatives, use `uw1 = 1/res.u(), vw2 = 1/res.v()` (one texel) or an artist "blur" parameter; `main` also adds a *static* `PtexFilter::eval(tx, opts, ...)` convenience (WebFetch of `main` Ptexture.h; not in v2.4.3).

Writing (`:801-919`):

```cpp
static PtexWriter* PtexWriter::open(const char* path, Ptex::MeshType mt, Ptex::DataType dt, int nchannels, int alphachan, int nfaces,
                                    Ptex::String& error, bool genmipmaps=true);          // :827
static PtexWriter* PtexWriter::edit(const char* path, bool incremental, MeshType, DataType, int nchannels, int alphachan, int nfaces, Ptex::String& error, bool genmipmaps=true); // :850
virtual bool writeFace(int faceid, const Ptex::FaceInfo& info, const void* data, int stride=0) = 0;   // :907
virtual bool writeConstantFace(int faceid, const Ptex::FaceInfo& info, const void* data) = 0;        // :914
virtual void writeMeta(const char* key, const char* string) = 0;   // + int8/16/32, float, double array overloads
virtual bool close(Ptex::String& error) = 0;                       // :919
```

Class doc (`:801-806`): "textures being written to the file are expected to have unmultiplied-alpha data. Generated mipmaps will be premultiplied by the Ptex library. On read, PtexTexture will (if requested) premultiply all textures by alpha when getData is called; by default only reductions are premultiplied."

`template <class T> class PtexPtr` (`:1032`) — RAII wrapper calling `release()`; Storm uses its own `_ReleaseUniquePtr` for the same purpose (`<openusd-src>/pxr/imaging/hdSt/ptexTextureObject.cpp:31-42`).

### 2.4 Thread safety (documented + verified)

- `PtexCache`: "The cache is fully multi-threaded. Cached data will be shared among all threads that have access to the cache, and the data are protected with internal locks." (`Ptexture.h:678-681`). Implementation notes: "The cache is fully thread-safe and completely lock/wait/atomic-free for accessing data that is present in the cache"; `get` = atomic refcount increment, release = atomic decrement "and potentially a brief spinlock" (`.../thirdparty/ptex/src/ptex/PtexCache.cpp:61-66`). `purge` while in use by another thread keeps that reference valid (`Ptexture.h:759-762`).
- `PtexFilter`: the header says nothing about threads, but `PtexSeparableFilter` carries per-eval scratch state — `float* _result; // temp result` and `float _weight; // accumulated weight` (`PtexSeparableFilter.h:80-81`) — so a filter instance is **not** reentrant: **one `PtexFilter` per thread (or per task)**, obtained from the shared texture via `getFilter`. Verified in `ptex_test.cpp`: 8 threads each doing `cache->get()` + `getFilter()` on the same file ran without error and with correct sums.
- `PtexTexture` obtained from the cache can be shared read-only across threads (reads go through the lock-free cache).

### 2.5 Mesh and face-id conventions (file format)

From the Ptex file-format spec (https://ptex.us/PtexFile.html) and adjacency page (https://ptex.us/adjdata.html):

- "The polymesh has a base type of either quad or triangle. For quad meshes, ptex stores rectangular textures. For triangle meshes, ptex stores triangular textures."
- "Every face (or subface) has a unique index, or 'faceid', which is implied by the ordering in the 'Face Info' block. The faceids are numbered starting from zero."
- "Non-quads embedded in a quad mesh and non-tris embedded in a tri-mesh are subdivided once to generate quad/tri subfaces and textures are stored per subface." → an n-gon (n≠4) in a quad-mesh file occupies **n consecutive faceids**, flagged `flag_subface`. "For a quad mesh with non-quad faces, the Ptex file contains one texture per subface... the first subface encountered in a counter-clockwise (i.e. edgeid order) traversal of the face shall be referenced as the adjacent face" (for a quad bordering an n-gon).
- Quad (u,v): "Face data is stored in v-major order with the first sample corresponding to u=v=0. This is considered the bottom-left corner with u pointing right and v pointing up"; vertex order `<0,0>,<1,0>,<1,1>,<0,1>`; edges 0..3 follow that order (`EdgeId` enum above).
- Adjacency: `adjfaces[4]` (−1 = border) and `adjedges` 2 bits each; "Triangle faces use only 3 entries, but 4 are allocated" / "the adjacent face and edge values for the fourth edge of each face are ignored" (https://ptex.us/tritex.html).
- Triangles: "The resolutions must be symmetric powers of two. I.e. the images must be square"; texels "are divided into even and odd texels and the odd texels are unfolded to form a rectangular image" (tritex page). The filter computes `w = 1 - u - v` and clamps u,v to [0,1] (`PtexTriangleFilter.cpp` `buildKernel`) — i.e. (u,v,w) are barycentric with vertex order implied; the exact vertex↔(u,v) assignment is UNVERIFIED beyond that.
- Per-face resolution is independent per face (`Res` ulog2/vlog2), and mip "levels" are power-of-two reductions across all faces.
- Data types uint8/uint16/half/float; optional alpha channel (unmultiplied at level 0).

### 2.6 Mapping USD mesh faces to Ptex face ids

Two independent sources agree on the rule "quads map 1:1; an n-gon expands to n ptex sub-faces in order":

1. OpenSubdiv (installed, header available): `Far::PtexIndices` (`$USD/include/opensubdiv/far/ptexIndices.h:46-88`):
   ```cpp
   class PtexIndices {
       PtexIndices(TopologyRefiner const &refiner);
       int GetNumFaces() const;                 // "Returns the number of ptex faces in the mesh"
       int GetFaceId(Index f) const;            // "Returns the ptex face index given a coarse face 'f' or -1"
       void GetAdjacency(TopologyRefiner const &refiner, int face, int quadrant, int adjFaces[4], int adjEdges[4]) const;
       // quadrant: "quadrant index if 'face' is not a quad (the local ptex sub-face index). Must be less than the number of face vertices."
   };
   ```
   A `TopologyRefiner` for a Hydra/USD mesh comes from `PxOsdRefinerFactory::Create(PxOsdMeshTopology const&, TfToken name)` (`<openusd-src>/pxr/imaging/pxOsd/refinerFactory.h:34-41`). This gives usdGen both the face-id map **and the `adjfaces/adjedges` needed to author `.ptx` files from a paint tool** (the `GetAdjacency` output is exactly `FaceInfo::setadjfaces/setadjedges` input).
2. Storm's shader convention: for coarse (unrefined) meshes "ptexId matches the primitiveID for quadrangulated or triangulated meshes" (`<openusd-src>/pxr/imaging/hdSt/codeGen.cpp:5620-5621`), and Hd quadrangulation "Produces a mesh where each non-quad face in the base mesh topology is quadrangulated such that the resulting mesh consists entirely of quads" (`<openusd-src>/pxr/imaging/hd/meshUtil.h:132-137`); for refined patches the ptex index rides in `Far::PatchParam` field0 ("faceId | 28 | the faceId of the patch (Storm uses ptexIndex)", `codeGen.cpp:5524`) and `HdSt_Subdivision` uses `patchTable->GetNumPtexFaces()` (`<openusd-src>/pxr/imaging/hdSt/subdivision.cpp:1602`).

Caveat for the tri case: Storm's triangulation is *fan* triangulation of the base face (a different sub-face count than Ptex's "subdivide once" rule for tri meshes); usdGen should treat `mt_quad` files with OSD's `PtexIndices` as the canonical mapping and only support `mt_triangle` files for all-triangle meshes (1:1).

### 2.7 Writing .ptx from a paint tool (verified pattern)

`.../thirdparty/bench/ptex_test.cpp` wrote a 2-face quad file (64×64 and 16×32, shared edge with `adjfaces`/`adjedges` set), a `dt_uint8` `mt_triangle` file (`Res(3,3)`), read them back through `PtexCache`, and confirmed cross-face blending (`eval` at `u=0.999` with a wide footprint returned a value between face 0 and face 1). `ptxinfo -m quad.ptx` reports `meshType: quad, dataType: float32, numChannels: 3, hasMipMaps: yes, numMetaKeys: 1`. Ptex's own reference for adjacency authoring is `src/tests/wtest.cpp` (3×3 grid of faces with explicit `adjfaces`/`adjedges` tables). A paint tool therefore needs: face count + per-face `Res` (from a `#3dpaint, N` style resolution parameter; a host groomer uses this exact annotation), adjacency from `Far::PtexIndices::GetAdjacency`, and channel data; `PtexWriter::edit(path, incremental=true, ...)` supports appending edits without rewriting (`Ptexture.h:850`).

### 2.8 Ptex performance (measured, v2.4.3, `-O2`, this host)

| Operation | ns |
|---|---|
| `f_bilinear` eval, 1-texel footprint, 1 thread | **23** |
| `f_box` eval, 4-texel footprint | **26** |
| bilinear, 8 threads, per-thread filter, shared cache | 35 ns/lookup/thread → **228 M lookups/s** |

So per-root Ptex sampling is cheaper than the SeExpr expression that consumes it; a 1 M-root density/length lookup is ~25 ms single-threaded.

### 2.9 Ptex inside OpenUSD 26.08 / Storm — and what this install actually has

| Fact | Evidence |
|---|---|
| Build flag `option(PXR_ENABLE_PTEX_SUPPORT "Enable Ptex support" OFF)`; when on: `find_package(PTex REQUIRED)` and global `add_definitions(-DPXR_PTEX_SUPPORT_ENABLED)` | `<openusd-src>/cmake/defaults/Options.cmake:36`; `cmake/defaults/Packages.cmake:266-269` |
| `hdSt` links `Ptex::Ptex_dynamic` and adds `ptexMipmapTextureLoader` only under the flag; `ptexTextureObject` and `shaders/ptexTexture.glslfx` are always compiled/installed | `<openusd-src>/pxr/imaging/hdSt/CMakeLists.txt:35-38,110,214` |
| No `FindPTex.cmake` module exists in `cmake/modules` — `find_package(PTex)` relies on Ptex's own `ptex-config.cmake` (verified target names match) | `ls <openusd-src>/cmake/modules | grep -i ptex` → empty |
| **This install was built with the flag OFF.** Probe linking the installed `libusd_hdSt.so`: `HdStIsSupportedPtexTexture("a.ptx") = 0`; `PXR_PTEX_SUPPORT_ENABLED` is not exported to consumers; `include/pxr/imaging/hdSt/ptexTextureObject.h` is installed but `ptexMipmapTextureLoader.h` is not | `.../scratchpad/ptexprobe/probe.cpp` output; `ls include/pxr/imaging/hdSt | grep -i ptex` → `ptexTextureObject.h` only |
| Behaviour of a `.ptx` in a Storm material on this install: `HdStPtexTextureObject::_Load()` body is entirely `#ifdef PXR_PTEX_SUPPORT_ENABLED` (`ptexTextureObject.cpp:111-231`), so `_format` stays `HgiFormatInvalid`, `IsValid()` is false (`:322`) and `_Commit()` uploads a 1×1 black "PtexTextureFallback" texel array plus a 1×1 layout texture (`:233-268`) — i.e. silently black, no error | source |
| How Storm would bind Ptex if enabled: material node with sdr metadata `isPtex` → `texParam.textureType = HdStTextureType::Ptex` (`<openusd-src>/pxr/imaging/hdSt/materialNetwork.cpp:684-686`); `HdStPtexSubtextureIdentifier(premultiplyAlpha)` (`:582-583`); registry creates `HdStPtexTextureObject` (`textureObjectRegistry.cpp:63-64`); loader packs all faces + mips + gutter into a `2DArray` texel texture (`maxNumPages=2048`, `PtexCache::create(1, 128 MiB, premultiplyAlpha)`, `cache->get(filename, err)`, `HdStPtexMipmapTextureLoader loader(reader, maxNumPages, maxLevels=-1, GetTargetMemory())`, layout 3 `UInt16Vec2` texels per face in a `1DArray` up to 16384 wide — `ptexTextureObject.cpp:129-170`); two samplers (`HdStPtexSamplerObject::GetTexelsSampler/GetLayoutSampler`, `samplerObject.h:119-139`); binder emits `<name>` and `<name>_layout` bindings (`textureBinder.cpp:69-80,203-235`); codegen includes `ptexTexture.glslfx`'s `PtexTextureSampler` when a `TEXTURE_PTEX_TEXEL` binding exists (`codeGen.cpp:2191-2193`) and generates `HdGet_<name>() { return GlopPtexTextureLookup(<name>_Data, <name>_Packing, GetPatchCoord()) }` (`codeGen.cpp:6361-6379`); `GetPatchCoord()` = interpolated patch coord with `.w = ptexFaceIndex + HdGet_ptexFaceOffset()` (`shaders/mesh.glslfx:1445-1456`), `ptexFaceOffset` defaulting to 0 (`materialNetworkShader.cpp:316,341`) | source |
| The only shipped sdr node with `isPtex` is the deprecated `HwPtexTexture_1` (`sdrMetadata { token role = "texture"; token isPtex = "1" }`, inputs `faceIndexPrimvar = "ptexFaceIndex"`, `faceOffsetPrimvar = "ptexFaceOffset"`, `asset inputs:file`), installed at `lib/usd/usdHydra/resources/shaders/shaderDefs.usda` | `<openusd-src>/pxr/usd/usdHydra/shaders/shaderDefs.usda:3-29`; `ls $USD/lib/usd/usdHydra/resources/shaders` |
| Storm tests for ptex exist but are compiled only under the flag (`testHdStPtex`, `pxr/imaging/hdSt/CMakeLists.txt:3017-3198`; `usdImagingGL` tests gated on `PTEX_FOUND AND PXR_ENABLE_PTEX_SUPPORT`, `pxr/usdImaging/usdImagingGL/CMakeLists.txt:5486`) | source |

**Design consequence:** on an unmodified 26.08 install (the plan's hard constraint), Storm cannot sample `.ptx`. usdGen must (a) sample Ptex **on the CPU** with the vendored library when evaluating groom attributes (density, length, clump, color at roots), and (b) for *viewport color*, bake results to per-curve / per-vertex `displayColor`-style primvars (or, for surface-space maps, to an ordinary UV image consumed by a `UsdUVTexture`) — never hand a `.ptx` path to a Storm material. If a site builds OpenUSD with `PXR_ENABLE_PTEX_SUPPORT=ON`, usdGen's Ptex copy must then be ABI-compatible with the one Storm loads: build usdGen's Ptex **static with hidden visibility** (`Ptex::Ptex_static`, `-fvisibility=hidden`) so two Ptex versions never clash in one process.

---

## 3. Image IO for painted maps (Hio)

### 3.1 API (`<openusd-src>/pxr/imaging/hio/image.h`)

```cpp
class HioImage {
  enum ImageOriginLocation { OriginUpperLeft, OriginLowerLeft };
  enum SourceColorSpace { Raw, SRGB, Auto };                                   // :53
  class StorageSpec { int width, height, depth; HioFormat format; bool flipped; void* data; };   // :64-74
  static bool IsSupportedImageFile(std::string const& filename);              // :91  (extension-driven)
  static HioImageSharedPtr OpenForReading(std::string const& filename, int subimage = 0, int mip = 0,
                                          SourceColorSpace = Auto, bool suppressErrors = false);   // :100
  virtual bool Read(StorageSpec const& storage) = 0;                          // :108  (converts to storage.format)
  virtual bool ReadCropped(int cropTop, int cropBottom, int cropLeft, int cropRight, StorageSpec const&) = 0; // :111
  static HioImageSharedPtr OpenForWriting(std::string const& filename);      // :124
  virtual bool Write(StorageSpec const& storage, VtDictionary const& metadata = VtDictionary()) = 0;  // :127
  virtual int GetWidth/GetHeight/GetBytesPerPixel; virtual HioFormat GetFormat(); virtual int GetNumMipLevels(); // :148
  virtual bool IsColorSpaceSRGB() const = 0;                                  // :151
  virtual bool GetMetadata(TfToken const&, VtValue*) const = 0;  virtual bool GetSamplerMetadata(HioAddressDimension, HioAddressMode*) const = 0;
};
class HioImageFactory<T> : HioImageFactoryBase { HioImageSharedPtr New() const; };  // :187-198  plugin factory
```

"Texture paths are UTF-8 strings, resolvable by AR. Texture system dispatch is driven by extension" (`image.h:36-37`). Dispatch: `HioImageRegistry::_ConstructImage` lowercases `ArGetResolver().GetExtension(filename)` and looks it up in a `HioRankedTypeMap` built from plugInfo metadata key `imageTypes` with optional `precedence` (default 1; higher wins) (`<openusd-src>/pxr/imaging/hio/imageRegistry.cpp:43-60`; `rankedTypeMap.h:121-137`). Env setting `HIO_IMAGE_PLUGIN_RESTRICTION` "Restricts HioImage plugin loading to the specified plugin" (`imageRegistry.cpp:31`). Formats/helpers: `HioFormat` enum (`hio/types.h:34-113`, e.g. `HioFormatUNorm8Vec4`, `HioFormatFloat16Vec4`, `HioFormatFloat32Vec3`, `HioFormatUNorm8Vec4srgb`), `HioGetFormat(nchannels, HioType, isSRGB)` (`types.h:162`), `HioGetHioType`, `HioGetComponentCount`, `HioGetDataSizeOfFormat` (`:168-184`).

### 3.2 Format plugins present in the install (verified by probe)

| Extension | Plugin | Read | Write | Evidence |
|---|---|---|---|---|
| `bmp jpg jpeg png tga hdr` | `Hio_StbImage` in `libusd_hio.so` (`precedence 30`) | yes (8-bit; `hdr` float) | yes | `$USD/lib/usd/hio/resources/plugInfo.json`; probe: png/jpg/hdr/tga/bmp = 1 |
| `exr` | `hioOpenEXR` plugin (`Hio_OpenEXRImage`, precedence 30) — built-in "nanoexr" reader/writer, no external OpenEXR library | yes (half/float, mips via subimages) | yes | `$USD/plugin/usd/hioOpenEXR/resources/plugInfo.json`; probe exr = 1 |
| `avif` | `hioAvif` plugin | yes | UNVERIFIED (not needed) | `.../plugin/usd/hioAvif/resources/plugInfo.json`; probe avif = 1 |
| `tif tiff tx ptx` | **none** (`PXR_BUILD_OPENIMAGEIO_PLUGIN OFF`; no `hioOiio`) | no | no | probe: tif/tiff/tx/ptx = 0; `Options.cmake:16` |

Reading details: stb path supports only `subimage == 0 && mip == 0` (`stbImage.cpp:387`), `GetNumMipLevels()` returns 1 (`:352`); Storm itself ignores `GetNumMipLevels` and probes successive `mip` indices until dimensions stop shrinking (`<openusd-src>/pxr/imaging/hdSt/textureUtils.cpp:429-441`). stb sources vendored in OpenUSD are `stb_image v2.29`, `stb_image_write v1.16`, `stb_image_resize2 v2.07` (patched) and are **private headers — not installed** (`hio/CMakeLists.txt` lists them under `PRIVATE_HEADERS`; `ls include/pxr/imaging/hio` has no `stb/`). If usdGen needs raw stb (e.g. resize), vendor its own copy (MIT/public-domain dual license, `stb_image.h:7962-7981`).

### 3.3 Writing images (for a paint/bake tool)

- `Hio_StbImage::Write` (`stbImage.cpp:615-700`): "Valid file types are jpg, png, bmp, tga, and hdr" (`:608`). Float/half input is **quantized to 8-bit** for non-hdr targets (`_Quantize<float>`, `:626`; sRGB-aware), `hdr` requires float; `png` written with `stbi_write_png(..., stride = width*bpp)` (`:675`); `flipped` honoured via `stbi_flip_vertically_on_write`; metadata dictionary is ignored (commented out).
- `Hio_OpenEXRImage::Write` (`OpenEXRImage.cpp:831-938`): float → `EXR_PIXEL_FLOAT`, half → `EXR_PIXEL_HALF`, 8-bit is promoted to half; up to 4 channels (R,G,B,A) via `nanoexr_write_exr`; metadata is written through `_AttributeWriteCallback`.
- Therefore for painted **scalar/vector maps** the right on-disk format from Hio is **EXR (float/half)** — lossless, float, mip-capable, readable by Storm; PNG only for 8-bit color. For per-face (Ptex) maps use `PtexWriter` directly (§2.7).
- A custom `HioImage` subclass can be registered by the plugin (`HioImageFactory<T>` + plugInfo `"Types": { "UsdGen_PtexImage": { "bases": ["HioImage"], "imageTypes": ["ptx","ptex"], "precedence": 30 } }`) — this would make `HioImage::OpenForReading("x.ptx")` succeed, but Storm's *UV* texture path would then treat it as a 2-D image, which is meaningless for Ptex; only useful for thumbnails/tools. Not recommended for rendering.

---

## 4. Noise libraries — options and recommendation

| Option | What it gives | License | Cost | Evidence |
|---|---|---|---|---|
| **SeExpr's own** (`Noise.h` templates + `ExprBuiltins.h` `noise/snoise/vnoise/fbm/vfbm/turbulence/cellnoise/pnoise/voronoi/hash`) | identical results in expressions **and** in C++ stylers (frizz/noise modifiers), 1-4D in, 1-3D out, periodic, fbm/turbulence, cellular, voronoi (7-arg, with fbm distortion) | Apache-2.0 (modified) | already linked; `Noise.cpp` 247 lines + 2124-line tables; scalar (no SIMD) | `.../thirdparty/seexpr/src/SeExpr2/Noise.h:23-36`, `ExprBuiltins.cpp:461-727, 840-908` |
| MaterialX stdlib noise (installed) | `mx_noise2d/3d`, `mx_cellnoise`, `mx_worleynoise`, fractal3d GLSL/OSL sources | Apache-2.0 | GPU-side only (shader code); useful if a viewport hair shader wants procedural color | `$USD/libraries/stdlib/genglsl/mx_noise3d_float.glsl` etc. |
| OpenUSD itself | **none** — no CPU noise in pxr (grep for perlin/simplex/cellnoise/voronoi finds only unrelated hits) | – | – | `grep -rli` over `<openusd-src>/pxr` |
| FastNoiseLite (`Cpp/FastNoiseLite.h`) | OpenSimplex2/2S, Cellular, Perlin, Value/ValueCubic, FBm/Ridged/PingPong fractals, domain warp; single header; SIMD-friendly | MIT | fast, but results differ from SeExpr's builtins | https://github.com/Auburn/FastNoiseLite (latest tag v1.1.1) |
| `stb_perlin.h` v0.5 | `stb_perlin_noise3(_seed)`, `_fbm_noise3`, `_turbulence_noise3`, `_ridge_noise3`, wrap/period | public domain / MIT | tiny | https://raw.githubusercontent.com/nothings/stb/master/stb_perlin.h |
| OSL noise | full production set (gabor, simplex, usimplex, cell, hash) | BSD-3 | requires OSL + OIIO + LLVM — far too heavy | UNVERIFIED sizes; not installed |

Recommendation: use **SeExpr2's noise as the single source of truth** (so `noise($P)` in an expression and the C++ "Noise" styler agree bit-for-bit), exposing thin wrappers in `usdGenMath`. Add FastNoiseLite only if a styler needs a *different* look (OpenSimplex, domain warp) and vectorised throughput; keep both behind a `usdGen::Noise` façade so the expression `map`/`noise` variants can be mirrored.

---

## 5. RBF / kd-tree / spatial queries

Needs in the plan: nearest guides per root (guide interpolation), clump-center assignment, neighbour queries for stylers (attraction/repulsion), RBF weights between rest positions.

| Option | Facts | Evidence |
|---|---|---|
| **nanoflann** | "A C++11 header-only library for building KD-Trees of datasets with different topologies: R², R³, SO(2) and SO(3)"; `KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<T, Cloud, 3>, Cloud, 3>`; `knnSearch`, `radiusSearch`, `radiusSearchCustomCallback`, `findWithinBox` (≥1.8), `KDTreeSingleIndexDynamicAdaptor`; adaptor interface avoids copying the point set; **BSD-2-Clause** ("Copyright 2008-2009 Marius Muja... David G. Lowe... 2011-2026 Jose L. Blanco"); newest tags 1.12.1 (2026-08-08), 1.12.0, 1.11.0, 1.10.1 | https://github.com/jlblancoc/nanoflann README; https://raw.githubusercontent.com/jlblancoc/nanoflann/master/COPYING; tags page |
| OpenUSD | no kd-tree / spatial index (`grep -rli "nanoflann\|kdtree\|KdTree"` over pxr → nothing) | grep |
| Own implementation | a 3-D static kd-tree or uniform grid over roots is ~200 lines; grid hashing is often faster for uniformly dense hair roots | UNVERIFIED perf claim |

Recommendation: vendor `nanoflann.hpp` (single header, BSD-2, pin 1.12.1) for kNN/radius queries over guide roots and clump centres; implement the RBF solve on top of it with a small dense solver (usdRig already has `libs/rigExecMath/sparseSolve.cpp`, `<usdrig-src>/CMakeLists.txt` target `rigExecMath`) — usdGen can link `rigExec::rigExecMath` if it consumes the rigExec package, or copy the needed kernels to stay decoupled.

---

## 6. Dependency strategy for a CMake plugin against an unmodified OpenUSD

### 6.1 What usdRig does (to mirror)

| Mechanism | usdRig evidence |
|---|---|
| `set(USD_INSTALL_DIR ".../usd-install" CACHE PATH ...)` then `find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)`; `CMAKE_CXX_STANDARD 17` | `<usdrig-src>/CMakeLists.txt:6-11` |
| Build script passes `-DCMAKE_PREFIX_PATH="$USD"` because `pxrConfig.cmake`'s `find_dependency(OpenSubdiv 3.6.1 CONFIG)` / `find_dependency(MaterialX)` do not inherit `PATHS` | `<usdrig-src>/bin/build_rigexec.sh` (comment + cmake line); `$USD/pxrConfig.cmake` |
| pxr targets are plain names (`usd`, `sdf`, `hd`, `hdSt`, `exec`, `vdf` …), with TBB provided as `TBB::tbb` imported from the USD prefix when `PXR_FIND_TBB_IN_CONFIG` is OFF (this install) | `pxrConfig.cmake` (`add_library(TBB::tbb SHARED IMPORTED)` block); `$USD/cmake/pxrTargets.cmake:585-590` (`hdSt` links `TBB::tbb;OpenSubdiv::osdCPU;OpenSubdiv::osdGPU;MaterialX*`) |
| Install layout mirrors USD: libs in `lib/`, plugins in `lib/usd/<name>/resources/plugInfo.json`, Python in `lib/python/`, CMake package in `lib/cmake/rigExec` | `CMakeLists.txt:31-39` |
| rpath `$ORIGIN` (+ `@loader_path` on macOS) and `CMAKE_INSTALL_RPATH_USE_LINK_PATH ON` so the Plug-loaded imaging library finds siblings and the USD libs | `CMakeLists.txt:48-54` |
| Plugin `plugInfo.json` for a *library* plugin is generated from a `.in` with `$<TARGET_FILE_NAME:rigExecImaging>` (`"LibraryPath": "../../@RIGEXEC_IMAGING_LIBRARY_FILENAME@"`, `"Type": "library"`, base `UsdImagingSceneIndexPlugin`), written to `build/usd/rigExecImaging/resources/` so the same relative hop works in build and install trees | `CMakeLists.txt:506-517`; `<usdrig-src>/plugin/rigExecImaging/resources/plugInfo.json.in` |
| Codeless schema plugInfo is rewritten at configure time from `"Type": "resource"` to `"Type": "library"` with a `LibraryPath` so Plug can dlopen it (needed for `implementsComputeExtent`) | `CMakeLists.txt:410-460` |
| usdview python plugin: `"Type": "python"`, `bases: ["pxr.Usdviewq.plugin.PluginContainer"]` | `<usdrig-src>/plugin/rigExecUsdview/plugInfo.json` |
| Runtime env: `PXR_PLUGINPATH_NAME=<build>/usd/rigExecSchema/resources:<build>/usd/rigExecImaging/resources:<src>/plugin/rigExecUsdview`, `PYTHONPATH` includes plugin dirs and USD site-packages | `<usdrig-src>/bin/_env.sh` |
| Exported package: `install(EXPORT rigExecTargets NAMESPACE rigExec::)`, `configure_package_config_file` with `PATH_VARS`, `write_basic_package_version_file(... SameMinorVersion)`; the config re-finds pxr inside a `block()` that appends the USD prefix to `CMAKE_PREFIX_PATH`, and publishes `rigExec_PLUGINPATHS` | `CMakeLists.txt:522-534`; `<usdrig-src>/cmake/rigExecConfig.cmake.in:8-51` |
| Python bindings are pybind11 (optional, auto-detected via `python -m pybind11 --cmakedir`) coexisting with USD's Boost.Python modules | `CMakeLists.txt:272-305`; `<usdrig-src>/python/CMakeLists.txt` |

### 6.2 Recommended shape for usdGen

```cmake
cmake_minimum_required(VERSION 3.26)
project(usdGen VERSION 0.1.0 LANGUAGES C CXX)          # C needed for zlib/libdeflate/stb if vendored
set(USD_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../OpenUSD_26_08" CACHE PATH "")
list(APPEND CMAKE_PREFIX_PATH "${USD_INSTALL_DIR}")    # for pxrConfig's find_dependency()
find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)
find_package(rigExec CONFIG QUIET)                     # optional: rigExec::rigExec, rigExec::rigExecMath, rigExec_PLUGINPATHS
set(CMAKE_CXX_STANDARD 17)  # must match the install (GCC 13, C++17, libstdc++ ABI)

include(FetchContent)
FetchContent_Declare(ptex GIT_REPOSITORY https://github.com/wdas/ptex.git GIT_TAG v2.4.3)   # zlib-based
set(PTEX_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE) set(PTEX_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ptex)            # target Ptex_static (defines PTEX_STATIC), needs ZLIB::ZLIB
FetchContent_Declare(seexpr GIT_REPOSITORY https://github.com/wdas/SeExpr.git GIT_TAG 8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc)
set(ENABLE_LLVM_BACKEND OFF ...) set(ENABLE_QT5 OFF ...) set(ENABLE_SSE4 OFF ...) set(USE_PYTHON OFF ...) set(BUILD_UTILS/DEMOS/DOC/TESTS OFF ...)
FetchContent_MakeAvailable(seexpr)          # target SeExpr2 (SHARED on Linux — see note)
```

Notes and rules:

1. **Vendored source vs FetchContent vs find_package.** `find_package(Ptex)`/`find_package(seexpr2)` only work if a site already installs them (neither is on this machine); `FetchContent` needs network at configure time; vendoring copies (~35 files for SeExpr core, ~25 for Ptex) is offline-safe and lets the build force `STATIC` + `-fvisibility=hidden`. Recommend: **FetchContent with an offline fallback `FETCHCONTENT_SOURCE_DIR_<NAME>` pointing at `thirdparty/`** (CMake ≥ 3.24 also has `FETCHCONTENT_TRY_FIND_PACKAGE_MODE`), and a top-level option `USDGEN_USE_SYSTEM_SEEXPR/PTEX` for packagers.
2. **Static + hidden** for both third-party libs inside `libusdGen.so`; do not install their headers or `.so`s into `lib/` next to USD (a `libPtex.so` in `lib/` would be picked by `$ORIGIN` rpath of a site's Ptex-enabled `libusd_hdSt.so` — ABI hazard).
3. SeExpr's `src/SeExpr2/CMakeLists.txt:86` hardcodes `SHARED` on non-Windows; either vendor with a modified one-liner, or after `FetchContent_MakeAvailable` build a second `STATIC` target from `${seexpr_SOURCE_DIR}/src/SeExpr2/*.cpp` minus `ExprLLVMCodeGeneration.cpp` plus the bison/flex outputs. bison/flex are hard requirements on Linux/macOS (§1.2); document `apt install bison flex`.
4. **ABI/TBB**: everything must compile with the same `-std=c++17`, same libstdc++ ABI, same TBB headers (`include/tbb` from the USD prefix, TBB 2020.3, interface 11103) — do not let FetchContent pull oneTBB 2021 (SeExpr and Ptex do not use TBB, so this is only a concern for usdGen's own code; link `TBB::tbb` from pxrConfig). SeExpr uses `pthread`/`dl`; Ptex uses `Threads::Threads`.
5. **Windows**: SeExpr builds STATIC there; Ptex fine; bison/flex must come from `winflexbison` or ship pre-generated parser files (KSeExpr has `USE_PREGENERATED_FILES` as precedent). Mark as later work.
6. **Python bindings**: `pip install pybind11 numpy` is required in `$VENV` before enabling `USDGEN_BUILD_PYTHON` (neither is installed today).
7. Ninja is not installed; scripts should default to `Unix Makefiles` or check `ninja`.

---

## 7. Licensing summary

| Component | License | Copyleft? | Notes |
|---|---|---|---|
| OpenUSD 26.08 | Tomorrow Open Source Technology License 1.0 (Apache-2.0 with modified §6 Trademarks) | no | `$USD/LICENSE.txt:1-9` |
| OpenSubdiv 3.6.1 | Apache-2.0 with modification (§6) | no | `include/opensubdiv/version.h` header |
| MaterialX 1.39.5 | Apache-2.0 | no | UNVERIFIED locally (well known) |
| oneTBB 2020.3 | Apache-2.0 | no | UNVERIFIED locally |
| **SeExpr (wdas, main)** | Apache-2.0 with §6 (Trademarks) replaced — no Disney names for endorsement | no | `.../seexpr/LICENSE:1-11` |
| KSeExpr | GPL-3.0-or-later (+ upstream Apache) | **yes** | avoid |
| **Ptex v2.4.3 / v2.5.x** | BSD-3-Clause (Disney variant, no-endorsement clause names Disney) | no | `.../ptex/LICENSE:1-8` |
| zlib (system 1.3) | zlib license | no | UNVERIFIED text; standard |
| libdeflate (if Ptex ≥ 2.5) | MIT | no | https://github.com/ebiggers/libdeflate |
| stb (`stb_image*`, `stb_perlin`) | MIT **or** public domain (dual) | no | `<openusd-src>/pxr/imaging/hio/stb/stb_image.h:7962-7981` |
| nanoflann 1.12.1 | BSD-2-Clause | no | `COPYING` (Muja/Lowe/Blanco) |
| FastNoiseLite v1.1.1 | MIT | no | repo page |
| LLVM (if SeExpr JIT ever enabled) | Apache-2.0 with LLVM exceptions | no | UNVERIFIED; not installed |
| OpenImageIO (not used) | Apache-2.0 | no | not installed |

All recommended components are permissive and compatible with shipping usdGen under a TOST/Apache-style license; only the two Disney "no endorsement / trademark" clauses need a NOTICE entry.

---

## Key facts

- The OpenUSD 26.08 install has **no Ptex, no OpenImageIO**: `grep -rn PTEX $USD/pxrConfig.cmake cmake/` empty; `ls include | grep -i ptex` empty; options default OFF at `<openusd-src>/cmake/defaults/Options.cmake:16,36`.
- A probe linked against the installed `libusd_hdSt.so` returns `HdStIsSupportedPtexTexture("a.ptx") = 0` and `HioImage::IsSupportedImageFile` = 1 for png/jpg/bmp/tga/hdr/exr/avif, 0 for tif/tiff/tx/ptx (`<session-scratch>`).
- With Ptex disabled, a `.ptx` in a Storm material becomes a 1×1 black fallback silently: `_Load` body is `#ifdef PXR_PTEX_SUPPORT_ENABLED` (`<openusd-src>/pxr/imaging/hdSt/ptexTextureObject.cpp:121-231`), fallback at `:233-268`.
- Storm's Ptex path (when enabled) keys off sdr metadata `isPtex` (`<openusd-src>/pxr/imaging/hdSt/materialNetwork.cpp:684-686`), uses `patchCoord.w + ptexFaceOffset` as face id (`shaders/mesh.glslfx:1445-1456`), and the only shipped node is deprecated `HwPtexTexture_1` (`<openusd-src>/pxr/usd/usdHydra/shaders/shaderDefs.usda:3-29`).
- SeExpr upstream: namespace/library `SeExpr2`, package version "2.0", last tag v3.0.1 (2019-11-19), main commit 8f8c8f2 (2026-01-27) sets C++17 (`.../thirdparty/seexpr/CMakeLists.txt:22-24,197`; https://github.com/wdas/SeExpr/tags; commits page).
- SeExpr license is Apache-2.0 with §6 Trademarks replaced (`.../thirdparty/seexpr/LICENSE:1-11`); KSeExpr is GPL-3.0-or-later and namespace `KSeExpr` (its `src/KSeExpr/Expression.h` SPDX header) — not usable.
- SeExpr builds interpreter-only on this aarch64 host with `ENABLE_LLVM_BACKEND=OFF ENABLE_QT5=OFF ENABLE_SSE4=OFF USE_PYTHON=OFF`; `ENABLE_SSE4` (default TRUE) adds `-msse4.1` (`CMakeLists.txt:98,202`); bison/flex/sed are required because `src/SeExpr2/generated/` is empty in git (`src/SeExpr2/CMakeLists.txt:31-40`).
- SeExpr CMake package: `share/cmake/seexpr2/seexpr2-config.cmake`, imported target `seexpr2::SeExpr2` linking `dl;pthread` (`.../thirdparty/install/share/cmake/seexpr2/seexpr2-exports.cmake:59-64`); headers `include/SeExpr2/*.h`; library is SHARED-only on non-Windows (`src/SeExpr2/CMakeLists.txt:86`).
- Embedding API: subclass `Expression`, override `resolveVar` (`Expression.h:199`) returning `ExprVarRef` whose `eval(double*)` must write exactly `type().dim()` doubles (`Expression.h:64`; corruption verified otherwise), `resolveFunc` (`:202`) returning `ExprFunc` wrapping an `ExprFuncSimple` with `prep/evalConstant/eval` (`ExprFuncX.h:111-113`); `evalFP(VarBlock*)` (`:189`), `isValid/parseError` (`:133/140`), `isVec` (`:177`), `isThreadSafe` (`:162`).
- Thread model: `evalFP` on one `Expression` writes shared `_interpreter->d` unless a `VarBlock` with `threadSafe=true` is passed, which memcpy's the interpreter state per eval (`Expression.cpp:304-309`, `Interpreter.cpp:31-43`, `VarBlock.h:65,121`). Verified 8-thread evaluation gives 50 M evals/s aggregate.
- Measured interpreter cost: 13 ns (`$u*$v+1`), 34 ns (custom `map()` + `hash`), 106–117 ns (noise+fbm / cellnoise+voronoi) per eval; prep 7–53 µs (`.../thirdparty/bench/seexpr_bench.cpp`).
- `rand()` is **not** a SeExpr2 builtin ("Function rand has no definition" at runtime; `ExprBuiltins.cpp:1719-1849` registers `hash` but no `rand`) although a host groomer and the SeExpr userdoc list it — usdGen must add `rand`.
- a host groomer globals/functions to emulate: `$u $v $id $faceId $patchId $frame $cLength $cWidth $cDepth $P/$Pg/$Pref/$Prefg(+w) $dPdu* $dPdv* $N/$Ng/$Nref/$Nrefg $Cs $As`, `map("name"[,s,t][,channel])`, `rand([min,max][,seed])`, a host groomer `noise` is −1..1 while SeExpr `noise` is 0..1 (Autodesk 2014/2018 reference pages; `noise($P*4)` at integer P printed 0.5 locally). Painted maps are `map('${DESC}/paintmaps/length/'); #3dpaint, 200` (directory of per-patch Ptex files).
- Ptex: tags v2.5.2 (2026-04-11) … v2.4.3 (2024-06-11); v2.5.0 switched zlib→libdeflate; v2.4.3 uses `find_package(ZLIB REQUIRED)` and defaults to C++98 unless `CMAKE_CXX_STANDARD` is set (`.../thirdparty/ptex/CMakeLists.txt:11-17,36`); libdeflate headers are absent on this host.
- Ptex v2.4.3 built here; exports `Ptex::Ptex_static` (defines `PTEX_STATIC`, links `Threads::Threads;ZLIB::ZLIB`) and `Ptex::Ptex_dynamic` in `lib/cmake/Ptex/ptex-config.cmake` (`.../thirdparty/install/lib/cmake/Ptex/ptex-exports.cmake:59-72`) — the same names `hdSt` expects (`<openusd-src>/pxr/imaging/hdSt/CMakeLists.txt:35-38`).
- Ptex API signatures: `PtexCache::create(int maxFiles, size_t maxMem, bool premultiply=false, ...)` (`Ptexture.h:711`), `cache->get(path, err)` (`:757`), `PtexTexture::getData(faceid, buffer, stride)` (`:551`), `getPixel(faceid,u,v,result,firstchan,nchannels)` (`:592`), `PtexFilter::getFilter(tx, Options{f_point|f_bilinear|f_box|f_gaussian|f_bicubic|f_bspline|f_catmullrom|f_mitchell, lerp, sharpness, noedgeblend})` (`:971`), `eval(result, firstchan, nchannels, faceid, u, v, uw1, vw1, uw2, vw2, width=1, blur=0)` (`:997`), `PtexWriter::open(path, MeshType, DataType, nchannels, alphachan, nfaces, err, genmipmaps=true)` (`:827`), `writeFace(faceid, FaceInfo, data, stride=0)` (`:907`), `close(err)` (`:919`).
- Ptex threading: cache "fully multi-threaded ... protected with internal locks" (`Ptexture.h:678-681`; lock-free reads per `PtexCache.cpp:61-66`); `PtexSeparableFilter` has `float* _result; float _weight; // temp result` scratch state (`PtexSeparableFilter.h:80-81`) → one filter per thread. Verified 8-thread lookups: 228 M/s aggregate; single-thread bilinear 23 ns.
- Ptex face ids: sequential from 0 in Face Info order; non-quads in a quad file are "subdivided once" into n subfaces (`flag_subface`); quad (u,v) origin bottom-left, v-major storage; edges `e_bottom,e_right,e_top,e_left` (`Ptexture.h:94-99`; https://ptex.us/PtexFile.html). USD→Ptex mapping and adjacency come from installed `Far::PtexIndices::GetFaceId/GetAdjacency` (`$USD/include/opensubdiv/far/ptexIndices.h:63-88`) via `PxOsdRefinerFactory::Create` (`<openusd-src>/pxr/imaging/pxOsd/refinerFactory.h:34-41`); Storm's coarse-quad convention "ptexId matches the primitiveID" (`<openusd-src>/pxr/imaging/hdSt/codeGen.cpp:5620-5621`).
- Hio: dispatch by lower-cased extension through plugInfo `imageTypes`/`precedence` (`<openusd-src>/pxr/imaging/hio/imageRegistry.cpp:43-60`, `rankedTypeMap.h:131-137`); stb plugin (`bmp jpg jpeg png tga hdr`, mip 0 only, `stbImage.cpp:387`), EXR via built-in nanoexr (read+write half/float, `OpenEXRImage.cpp:831-938`), AVIF read; stb writes quantize float to 8-bit except `.hdr` (`stbImage.cpp:615-700`); stb headers are private, not installed.
- usdRig's consumer pattern to mirror: `find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)` + `-DCMAKE_PREFIX_PATH=$USD` (`<usdrig-src>/CMakeLists.txt:6-11`, `bin/build_rigexec.sh`), install layout `lib/`, `lib/usd/<name>/resources`, `lib/python`, `lib/cmake/rigExec` (`CMakeLists.txt:31-39`), `$ORIGIN` rpath (`:48-54`), generated library-plugin `plugInfo.json` with `$<TARGET_FILE_NAME:>` (`:506-517`), exported `rigExec::` targets + `rigExec_PLUGINPATHS` (`cmake/rigExecConfig.cmake.in:8-51`).
- Host toolchain: GCC 13.3 (same as the USD build), CMake 3.28.3, bison 3.8.2, flex 2.6.4, zlib dev present; **no ninja, no LLVM dev, no libdeflate dev, no pybind11/numpy in the venv**.
- No CPU noise or kd-tree exists in OpenUSD; SeExpr's `Noise.h` templates (`Noise/PNoise/FBM/CellNoise`, `.../seexpr/src/SeExpr2/Noise.h:23-36`) can serve both expressions and C++ stylers; nanoflann 1.12.1 (BSD-2, header-only) is the spatial-query pick.

## Open questions

- Exact a host groomer 2024+ function list beyond the 2014 reference (whether `vmap()`, `cvar()`, `ptex()` exist and their signatures) — the 2023/2026 Autodesk reference pages returned HTTP 503/partial content; treat as UNVERIFIED.
- Ptex triangle-face (u,v)↔vertex convention and the odd/even texel packing — only "w = 1 - u - v" and "square resolution" were verifiable; needed before supporting `mt_triangle` files for painted maps on triangle meshes.
- Whether SeExpr's LLVM backend (LLVM ≥ 3.8; not installed) is worth it for usdGen — no measurement possible here; interpreter numbers (0.1 µs/eval) suggest not.
- SeExpr on Windows/macOS: pre-generated parser sources and STATIC-only build on Windows (`src/SeExpr2/CMakeLists.txt:89`) — untested; KSeExpr's `USE_PREGENERATED_FILES` is a template.
- Symbol-clash behaviour when a host application that ships its own SeExpr/Ptex loads usdGen — mitigated by static+hidden linking but not tested.
- Ptex ≥ 2.5 (libdeflate) vs 2.4.3 (zlib): whether any site-installed, Ptex-enabled OpenUSD build would expect a particular Ptex SOVERSION (2.4 vs 2.5) if usdGen ever exposed Ptex dynamically — irrelevant with static linking.
- `hioAvif` write support and `Hio_OpenEXRImage` mip/subimage semantics for writing multi-level maps were not exercised.
- Precision/semantics of a host groomer `$ptexId`, `$fitR`, `$cid` (search snippets only).
- Whether `HdStPtexMipmapTextureLoader`-style packing (2DArray + layout texture) is worth re-implementing in usdGen's own Storm shader for surface-space *color* maps if a site enables Ptex in OpenUSD — deferred until a Ptex-enabled build is available.
