# exec-code-B report — lane B (package config, install layout, public API)

Run: lane B of the M0 exit-criteria fix round. Assigned: S-5/X-4, S-6, S-7, S-8.
No findings were rejected. No P2 items were assigned to this lane (S-9 and X-10
are record-only elsewhere and untouched here).

## S-5 / X-4 — installed CMake package config + install-check tests: APPLIED

Changes:
- `cmake/usdGenConfig.cmake.in` (rewritten):
  - `set_and_check(usdGen_PLUGIN_DIR)` / `set_and_check(usdGen_LIBRARY_DIR)` now
    resolve before any nested dependency find (previously after `find_dependency(pxr)`).
  - `find_dependency(pxr CONFIG)` guarded with `if(NOT TARGET usd)` so a consumer
    that already imported pxr does not re-enter pxrConfig (double-pxr trap, plan §3.2).
  - `find_dependency(TBB)` guarded with `if(NOT TARGET TBB::tbb)` and
    `find_dependency(OpenGL)` with `if(NOT TARGET OpenGL::GL)`.
    Empirically required: with `PXR_FIND_TBB_IN_CONFIG=OFF` (the default, and the
    configuration this OpenUSD install was built with) pxrConfig itself creates
    `TBB::tbb` as an imported target pointing at `${usd}/lib/libtbb.so`, and no
    TBB CMake config package is installed on this system — an unconditional
    `find_dependency(TBB)` fails the consumer configure with
    "Could not find a package configuration file provided by TBB".
  - Emits `usdGen_PLUGINPATHS` (the two installed `lib/usd/*/resources` dirs) so
    a consuming build can compose PXR_PLUGINPATH_NAME without hardcoding.
- `CMakeLists.txt`: `configure_package_config_file(... PATH_VARS USDGEN_INSTALL_PLUGINDIR
  USDGEN_INSTALL_LIBDIR)`. Generated config now contains real paths:
  `set_and_check(usdGen_PLUGIN_DIR "${PACKAGE_PREFIX_DIR}/lib/usd")`,
  `set_and_check(usdGen_LIBRARY_DIR "${PACKAGE_PREFIX_DIR}/lib")` (verified in the
  scratch install).
- New tests, registered in `CMakeLists.txt` with labels `T0;install` (plan §6.2 tier
  table lists both as T0; they also carry `install` for the CI job):
  - `tests/testUsdGenInstallTree.cpp` — layout assertions against the installed tree
    (positive: libs, headers, plugin resources, cmake config; negative: the exact
    S-6 double-nesting paths and `.cpp` files must NOT exist) plus the stock-only
    extent probe from `docs/prework/probes/m0-usdcat-extent/` (codeless types
    known+concrete via `TfType::FindByName` const-ref form, `ComputeExtent` non-empty)
    with PXR_PLUGINPATH_NAME pointed at the installed resources dirs (the test
    re-execs itself once so the plugin path is in the environment before Plug
    discovery).
  - `tests/testUsdGenConsumer.cpp` + `tests/consumer/` — configures, builds, and runs
    a tiny downstream CMake project that does `find_package(pxr REQUIRED CONFIG)`
    then `find_package(usdGen REQUIRED)`, links `usdGen::usdGen`, and calls
    `usdGen::GetVersionString()`.
  - **Both take the install prefix from the `USDGEN_INSTALL_PREFIX` environment
    variable (exact name — coordinate with docs lane / CI). When unset they print a
    note and return 0 (clean skip), so the canonical `ctest -L '^T[01]$'` run stays
    green without an install.** The existing `.github/workflows/usdgen.yml`
    install-check job already sets `USDGEN_INSTALL_PREFIX` and selects these two
    tests via `ctest -R 'testUsdGen(InstallTree|Consumer)$'` — names align.

Ordering nuance (documented in `tests/consumer/CMakeLists.txt`): pxrConfig is
single-pass — a second `find_package(pxr CONFIG)` in the same CMake process
re-runs it and fails at `add_library(TBB::tbb ...)` ("target already exists").
The consumer therefore finds pxr FIRST and usdGen second; usdGenConfig's
`if(NOT TARGET usd)` guard then skips its nested pxr find. This is the defence
the plan's exit-criterion "a consumer may call find_package(pxr) and
find_package(usdGen) together" requires.

Evidence (scratch prefix `/tmp/usdgen-install-scratch`):
- `USDGEN_INSTALL_PREFIX=/tmp/usdgen-install-scratch ctest -R
  'testUsdGen(InstallTree|Consumer)$'` → **2/2 Passed**.
- InstallTree: all 18 layout checks `ok`, no negative check tripped, then
  `type-check UsdGenGroom known=1 IsConcrete=1`, `type-check
  UsdGenDescription known=1 IsConcrete=1`, `ComputeExtent ok=1 size=2`,
  `extent = [(-0.5 -0.5 -0.5) .. (0.5 0.5 0.5)] empty=0`.
- Consumer: real `cmake -S tests/consumer` configure + build against
  CMAKE_PREFIX_PATH=`<scratch>;<usd install>` (pxr found from the OpenUSD
  install, usdGen from the scratch prefix), `consumer` binary runs and prints
  `usdGen 0.1.0 (M0)`; grep for "usdGen" in output passes.

## S-6 — installed include layout: APPLIED

Rewrote the install rules in `CMakeLists.txt` so the tree matches what the
exported targets' `$<INSTALL_INTERFACE>` directories advertise:

| target | build include root | header spelling | installed path |
|---|---|---|---|
| usdGen | `libs/usdGen` | `usdGen/usdGen.h` | `include/usdGen/usdGen.h`, `include/usdGen/export.h` |
| usdGenMath | `libs` | `usdGenMath/usdGenMath.h` | `include/usdGenMath/usdGenMath.h` |
| usdGenSchema | `libs/usdGenSchema` | `usdGenSchema/usdGenSchema.h` | `include/usdGenSchema/usdGenSchema.h` |
| usdGenImaging | `libs/usdGenImaging` | `usdGenImaging/api.h` | `include/usdGenImaging/*.h` |
| nanoflann | `thirdparty/nanoflann` | `nanoflann.hpp` | `include/nanoflann.hpp` (was `include/nanoflann/nanoflann.hpp` — root had no `nanoflann.hpp`) |
| usdGen_seexpr | `thirdparty/seexpr` | `SeExpr2/Noise.h` | `include/seexpr/SeExpr2/*.h` (was double-nested `include/seexpr/SeExpr2/SeExpr2/`) |

Mechanics: trailing-slash `install(DIRECTORY .../ DESTINATION include/<api>
FILES_MATCHING PATTERN "*.h")` per header dir; `install(FILES
thirdparty/nanoflann/nanoflann.hpp DESTINATION include)`; `install(DIRECTORY
thirdparty/seexpr/SeExpr2 DESTINATION include/seexpr FILES_MATCHING "*.h")`
(no trailing slash so the `SeExpr2` level survives). The eight old, duplicated,
`.cpp`-shipping `install(DIRECTORY ...)` rules were removed. Verified:
`cmake --install` → 40 files, `find /tmp/usdgen-install-scratch -name '*.cpp'`
is empty, `version.h` (now deleted, see S-8) is gone, and
`testUsdGenInstallTree`'s negative checks (double-nesting + `.cpp` paths) pass.

## S-7 — const-reference FindByName in extent registration: APPLIED

`libs/usdGenSchema/usdGenSchema.cpp` now reads:

```cpp
const TfType &type = TfType::FindByName("UsdGenDescription");
if (!type.IsUnknown()) {
    UsdGeomRegisterComputeExtentFunction(type, &usdGen::_ComputeDescriptionExtent);
}
```

(the previous `if (type && !type.IsUnknown())` redundant check was dropped as
well). This matches the 26.08 `pxr/base/tf/type.h` signature
`static TfType const& FindByName(...)`.

## S-8 — public M0 API consistency: APPLIED

- **Canonical core declaration**: `std::string usdGen::GetVersionString()`,
  declared in exactly one header, `libs/usdGen/usdGen/usdGen.h`.
  `libs/usdGen/usdGen/version.h` (the conflicting `const char*` declaration)
  was deleted; `libs/usdGen/version.cpp` now includes `usdGen/usdGen.h`.
- **Export macro**: new `libs/usdGen/usdGen/export.h` with `USDGEN_CORE_API`
  (`__attribute__((visibility("default")))` / Win `dllexport|dllimport`).
  This was required, not optional: the core is a SHARED lib built with
  `CXX_VISIBILITY_PRESET hidden` + `LINKER:--exclude-libs,ALL` (gate B-1), so
  before this its only public symbol was LOCAL —
  `nm -D --defined-only build/libusdGen.so | c++filt` showed no usdGen API at
  all. After: `0000000000001360 T usdGen::GetVersionString[abi:cxx11]()` is
  dynamically exported, and the consumer test links and calls it against the
  installed lib.
- **Math rename**: `usdGenMath::GetVersionString()` (was the colliding
  `usdGen::GetVersionString` in `libs/usdGenMath/usdGenMath.h` /
  `libs/usdGenMath/version.cpp`). No other call sites existed (grep-verified);
  the math lib is static, so no export macro is needed.
- **Imaging dangling declaration deleted**: `usdGen::GetImagingVersionString()`
  had no definition anywhere and no call sites; removed from
  `libs/usdGenImaging/usdGenImaging/api.h` with a comment pointing at the
  canonical M0 version API and the M1 registry as the future home of imaging
  identity.
- **Schema provider setter/getter implemented**: `UsdGenSchema_SetExtentProvider`
  / `UsdGenSchema_GetExtentProvider` (declared `TF_API` in
  `usdGenSchema/usdGenSchema.h`) now have definitions in
  `libs/usdGenSchema/usdGenSchema.cpp` — a process-static provider slot with
  set/get accessors, as the header's contract states ("usdGenImaging installs a
  provider when it loads"; M1 wiring). Both symbols are present in the
  installed `libusdGenSchema.so` dynamic table:
  `pxrInternal_v0_26_8__pxrReserved__::UsdGenSchema_SetExtentProvider(bool (*)(SdfPath const&, UsdTimeCode const&, GfVec3f*))`
  and `...::UsdGenSchema_GetExtentProvider()`.

Result: the installed M0 surface has one canonical version API, no
incompatible duplicates, no dangling declarations, and all declared symbols
defined.

## P2 items

None assigned to lane B. (S-9 EGL heuristic and X-10 workspace products remain
record-only; not touched.)

## Rejected findings

None. All four assigned sol findings were factually correct; evidence above.

## Test results

- Canonical run (no install prefix): `env
  LD_LIBRARY_PATH=/home/burkard/work/OpenUSD_26_08/lib ctest --test-dir
  /home/burkard/work/usdGen/build -L '^T[01]$'` → **13/13 passed**
  (T0 = 11, T1 = 2; the two new install tests skip cleanly with a printed note
  when `USDGEN_INSTALL_PREFIX` is unset).
- Scratch install: `cmake --install build --prefix /tmp/usdgen-install-scratch`
  → 40 files, layout as advertised by the exported targets (top level:
  `include/{nanoflann.hpp,seexpr,usdGen,usdGenImaging,usdGenMath,usdGenSchema}`,
  `lib/{libusdGen.so,libusdGenImaging.so,libusdGenSchema.so,libusdGenMath.a,
  libusdGen_seexpr.a,usd,cmake}`, `share/usdGen`).
- `USDGEN_INSTALL_PREFIX=/tmp/usdgen-install-scratch ctest -R
  'testUsdGen(InstallTree|Consumer)$'` → **2/2 passed** (install tree: layout +
  extent probe green; consumer: configure+build+run green).
