# sol M0 core validation notes
## Findings
### [P0] S-1: Binary boundary gates can falsely pass
- location: cmake/CheckNoStageLink.cmake:25, cmake/CheckNoThirdPartyExports.cmake:11
- issue: The ELF-magic comparison is invalid under current CMake policies, so `libusdGen.so` takes the static-archive branch instead of checking `DT_NEEDED`. Tool failures also pass silently; the third-party pipeline reports success when `nm` cannot read its target.
- evidence: `$ cmake -DTARGET_FILE=build/libusdGen.so -DOBJDUMP=/usr/bin/objdump '-DFORBIDDEN=^libusd_tf' -P cmake/CheckNoStageLink.cmake` prints `Invalid escape sequence \1` followed by `B-1 link check passed`, although `objdump -p build/libusdGen.so` reports `NEEDED libusd_tf.so`. `$ cmake -DTARGET_FILE=/does/not/exist/libusdGen.so -DCXXFILT=/usr/bin/c++filt -P cmake/CheckNoThirdPartyExports.cmake` prints `nm: ... No such file` followed by `B-1 no-third-party-exports check passed`.
- fix: Read four bytes as hex (`file(READ ... HEX LIMIT 4)`) and compare with `7f454c46`; fail fatally for every nonzero tool result. Replace shell pipelines with `execute_process(COMMAND nm ... COMMAND c++filt RESULTS_VARIABLE ...)` and require every result to be zero. Add a deliberately forbidden shared-library fixture proving the negative path.

### [P0] S-2: Include-rule gate scans zero files
- location: cmake/CheckNoStageInclude.cmake:16
- issue: The quoted semicolon combines the `.h` and `.cpp` globs into one invalid pattern. Consequently, `_files` is empty for every protected directory and B-1 always passes.
- evidence: `$ cmake --trace-expand ... -P cmake/CheckNoStageInclude.cmake` shows `file(GLOB_RECURSE _files /.../*.h;/.../*.cpp)` immediately followed by `foreach(f IN LISTS _files)` with no loop body, then `B-1 include check passed`.
- fix: Pass two separate arguments: `file(GLOB_RECURSE _files "${dir}/*.h" "${dir}/*.cpp")`. Fail if the complete protected-source set is empty, and add a negative fixture containing a forbidden include.

### [P0] S-3: Chain-order test does not prove the M0 vertical-demo boundary
- location: tests/testUsdGenChainOrder.cpp:106, CMakeLists.txt:261
- issue: The test checks all `UsdImaging`/`UsdSkel` names on only one side, but does not assert every Storm/Hdsi/HdSt node is on the renderer side. Scene-globals and USD-stage checks are conditional, so their absence passes. The second test does not inject the required synthetic `hdPrman:*` ordering entries. Its breadth-first flattening also treats adjacent vector entries as graph edges. Both test environments omit `${USD_INSTALL_DIR}/lib/usd`.
- evidence: Verbose output passes with 26 nodes but contains no `HdsiSceneGlobalsSceneIndex`; it places `UsdGenGroomSceneIndex` at 13 and numerous Hdsi renderer nodes at 1–12. Source lines 134–140 guard scene-globals with `if (... >= 0)`, and lines 142–155 only switch to `JsonMetadataOnly`. CTest reports `PXR_PLUGINPATH_NAME=.../plugin/usd` without `.../lib/usd`.
- fix: Traverse actual input edges and require the usdGen node’s input to be the expected post-UsdImaging/scene-globals terminal; assert every discovered Storm/Hdsi/HdSt node is downstream and every UsdImaging/UsdSkel node upstream. Construct the scene-globals callback path, add synthetic hdPrman ordering coverage, and include both OpenUSD plug-in roots in colon-separated `PXR_PLUGINPATH_NAME`.

### [P1] S-4: Metadata plugin is double-registered and ignores the kill switch
- location: libs/usdGenImaging/usdGenImaging/metadataSceneIndexPlugin.cpp:13, libs/usdGenImaging/usdGenImaging/metadataSceneIndexPlugin.h:27
- issue: The type is defined once explicitly and again by `UsdImagingSceneIndexPlugin::Define<T>()`, which internally calls `TfType::Define`. The metadata names remain active when `USDGEN_ENABLE=0`, so the switch does not disable both scene-index plug-ins.
- evidence: Verbose chain-test output emits `Coding Error ... TfType 'UsdGenMetadataSceneIndexPlugin' already has a defined C++ type; cannot redefine`, yet the executable returns success. `rg USDGEN_ENABLE libs/usdGenImaging` finds uses only in `groomSceneIndexPlugin.cpp`.
- fix: Remove the standalone `TF_REGISTRY_FUNCTION(TfType)` block. Declare the shared `TfEnvSetting<bool>` in a common header and return empty vectors from both metadata-name methods when disabled. Add a separate `USDGEN_ENABLE=0` test and use `TfErrorMark` so coding errors fail tests.

### [P1] S-5: Installed CMake package config is unusable
- location: CMakeLists.txt:311, cmake/usdGenConfig.cmake.in:8
- issue: `configure_package_config_file` omits `PATH_VARS`, so both exported directory variables become empty. The pxr dependency is also resolved before package-relative paths are captured and lacks the plan’s `if(NOT TARGET usd)` double-pxr guard.
- evidence: `$ sed -n '1,80p' build/usdGenConfig.cmake` shows `set_and_check(usdGen_PLUGIN_DIR "")` and `set_and_check(usdGen_LIBRARY_DIR "")`.
- fix: Add `PATH_VARS USDGEN_INSTALL_PLUGINDIR USDGEN_INSTALL_LIBDIR` to `configure_package_config_file`. Resolve `set_and_check` variables before nested dependencies, and guard `find_dependency(pxr CONFIG)` with `if(NOT TARGET usd)`. Add the planned installed-tree consumer test.

### [P1] S-6: Installed include layout does not match exported include roots
- location: CMakeLists.txt:64, CMakeLists.txt:74, CMakeLists.txt:324
- issue: Repeated `install(DIRECTORY ...)` rules append directory basenames, producing nested paths such as `include/usdGen/usdGen/version.h`, while exported targets advertise only `<prefix>/include`. They also install `.cpp` files. SeExpr becomes doubly nested, and nanoflann’s exported include root does not contain `nanoflann.hpp`.
- evidence: `build/cmake_install.cmake` installs source directory `libs/usdGen` to `include` and then `libs/usdGen/usdGen` to `include/usdGen`; both produce the nested `usdGen/usdGen` layout. Exported targets report `INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include"`. SeExpr installs directory `SeExpr2` into destination `include/seexpr/SeExpr2`.
- fix: Use trailing source slashes and `FILES_MATCHING PATTERN "*.h"`: for example, `install(DIRECTORY libs/usdGen/usdGen/ DESTINATION include/usdGen ...)`. Apply the same pattern once to schema and imaging, remove duplicates, align nanoflann’s install interface with its destination, and install `SeExpr2/` contents without adding a second `SeExpr2`.

### [P1] S-7: Compute-extent registration misses the required const-reference form
- location: libs/usdGenSchema/usdGenSchema.cpp:79
- issue: `TfType::FindByName` returns `TfType const&`, but the implementation copies it into `const TfType`. Runtime behavior currently works, but it does not match the required registration form.
- evidence: The installed `tf/type.h` declares `static TfType const& FindByName(...)`; usdGen uses `const TfType type = TfType::FindByName(...)`.
- fix: Change to `const TfType &type = TfType::FindByName("UsdGenDescription");` and guard with `if (!type.IsUnknown())` before registration.

### [P1] S-8: Public M0 API declarations are inconsistent or undefined
- location: libs/usdGen/usdGen/usdGen.h:16, libs/usdGen/usdGen/version.h:13, libs/usdGenImaging/usdGenImaging/api.h:14, libs/usdGenSchema/usdGenSchema/usdGenSchema.h:24
- issue: The core headers declare the same function with incompatible `const char*` and `std::string` return types; core and math also define the same `usdGen::GetVersionString` name. The shared core hides its implementation, while imaging’s version function and schema’s provider setter/getter have no definitions.
- evidence: `readelf -Ws build/libusdGen.so | c++filt` reports `usdGen::GetVersionString[abi:cxx11]()` as `LOCAL`; `nm -D --defined-only build/libusdGen.so` exposes no usdGen API. `rg GetImagingVersionString` and `rg UsdGenSchema_SetExtentProvider` find declarations only.
- fix: Establish one canonical core header and return type, rename the math version function, add project-specific export macros, and either implement the advertised imaging/schema functions now or remove them from the installed M0 surface.

### [P2] S-9: EGL availability helper is Linux-path dependent
- location: testutils/usdGenTestUtilsHd/testUtilsHd.cpp:7
- issue: It treats successful `fopen("/dev/dri")` as proof that EGL works. This is Linux-specific and does not validate a device node, permissions, EGL initialization, or a usable pbuffer configuration.
- evidence: Source lines 9–12 contain the sole test: `FILE *f = fopen("/dev/dri", "r");`.
- fix: Replace the heuristic with the planned headless EGL initialization probe before enabling T2 tests. The current T0/T1 tests otherwise have no TZ, locale, architecture, or thread-count dependence.

## Verified OK
- Actual B-1 artifacts are clean despite the broken automated gates: `objdump -p build/libusdGen.so | rg NEEDED` reports only `libusd_tf.so`, `libusd_python.so`, Python/runtime libraries, and no usd/hd/hgi/glf/garch-family library. `nm -u build/libusdGenMath.a | c++filt | rg '__pxrReserved__::(Usd|Hd|Hgi|Glf|Garch)'` produces no matches.
- Manual source scan of `libs/usdGen` and `libs/usdGenMath` finds no `pxr/usd`, `pxr/imaging`, `pxr/usdImaging`, or `pxr/gl` include; allowed Tf/Gf/Vt headers are present.
- `nm -D --defined-only build/libusdGen.so | c++filt | rg 'SeExpr2::|Ptex::'` produces no matches.
- `UsdGenGroomSceneIndex` matches OpenUSD 26.08: one-argument base constructor, correct `GetPrim`, `GetChildPrimPaths`, `_PrimsAdded`, `_PrimsRemoved`, and `_PrimsDirtied` overrides, with `_SendPrims*` forwarding.
- Groom registration correctly uses `allRenderers`, phase 0, `InsertionOrderAtEnd`, and `_IsEnabled` reads `USDGEN_ENABLE`.
- `schema.usda` declares all 12 requested codeless types with the correct concrete/abstract/API shapes; `UsdGenDescription` derives from `Boundable`.
- The configured schema manifest is `Type: library`, contains `implementsComputeExtent: true` only on `UsdGenDescription`, and places `AutoApplyAPISchemas` inside `Info`. Its three-hop path resolves to `build/libusdGenSchema.so`. The checked-in resource manifest is intentionally rewritten at generation time as specified by the plan.
- The configured imaging manifest is valid JSON. With `Root: ".."`, its two-hop `LibraryPath` resolves to `build/libusdGenImaging.so`.
- Export membership includes `usdGenMath`, `usdGen_seexpr`, and `nanoflann`; `USDGEN_WITH_RIGEXEC:BOOL=OFF`.
- `build/build.ninja` applies `-ffp-contract=off` only to `usdGenMath`; other target compile rules omit it.
- Schema and imaging `file(GENERATE)` calls occur before test declarations, so their build-tree resources exist before test execution.
- `usdGenConfig.cmake.in` does call `find_dependency` for pxr, TBB, and OpenGL; its path expansion defect is recorded above.
- Requested suite result: `ctest --test-dir <usdgen-src>/build -L '^T[01]$' --output-on-failure` passed 7/7 tests in 0.24 seconds. The hidden coding error found under verbose execution is recorded above.

## Summary
P0: 3, P1: 5, P2: 1; verdict: FAIL