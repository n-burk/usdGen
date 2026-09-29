> Historical engineering note. It records a review or probe, not the current product overview. Start at the [repository README](../../README.md) and [docs index](../README.md).

# Execution report — CODE LANE A (gates + chain test + plugin registration)

Date: 2026-09-05 (usdGen M0, OpenUSD 26.08)
Scope: sol findings S-1, S-2, S-3/X-2, S-4/X-8, X-1. All P0/P1 findings applied.

## Findings

### S-1 (P0) — BINARY BOUNDARY GATES CAN FALSely PASS → **APPLIED**

`cmake/CheckNoStageLink.cmake` (rewritten):
- **Dead ELF-magic branch fixed**: replaced the literal `"\177ELF"` string
  compare (an invalid CMake escape that never matches, so every shared lib
  silently took the static-archive branch) with
  `file(READ ... HEX LIMIT 4)` compared against `7f454c46` (ELF) and
  `213c6172` (`!<ar`, static archive). Any other magic → FATAL_ERROR
  (previously: shared libs were checked with `nm -u`, i.e. the wrong
  tool, and executables/unknown files fell through to "passed").
- **Tool failures are now fatal**: nonzero result from `objdump`, `nm`, or
  `c++filt` → `message(FATAL_ERROR ...)` with the captured stderr. The old
  script ignored `RESULT_VARIABLE` entirely in both branches.
- **Shell pipeline removed**: the static branch previously ran
  `sh -c "nm -u ... | c++filt"` whose exit code was `c++filt`'s (0 even when
  `nm` failed). Now `nm` and `c++filt` are separate `execute_process` calls
  with individual `RESULT_VARIABLE`/`ERROR_VARIABLE` checks; c++filt reads
  the nm output via `INPUT_FILE` (no shell).
- **Forbidden-name extraction**: NEEDED lines are now parsed by stripping
  the leading whitespace+`NEEDED` and testing the library name against the
  (start-anchored) `FORBIDDEN` regex. The old `line MATCHES "NEEDED.*^libusd_..."`
  pattern can never match because CMake's `MATCHES` is unanchored and `^`
  inside the pattern only binds to position 0 — a second latent dead-check
  found during this work.

`cmake/CheckNoThirdPartyExports.cmake` (same class of bug, named in sol S-1):
- Missing/unknown `TARGET_FILE` and missing `nm` are now FATAL (old: the
  whole check was skipped → "passed" on a nonexistent target).
- `sh -c "nm ... | c++filt"` pipeline replaced by two `execute_process`
  calls (`nm` result checked; `c++filt` via `INPUT_FILE`, result checked).

**Proof (negative + positive fixtures, new tests):**
- `tests/fixtures/gateLinkDriver.cmake` runs the gate script and asserts the
  verdict + the *reason* (a real `Gate B-1 VIOLATION`, not a config error).
- `testUsdGenLinkRuleNegativeFixture`: gate on
  `${USD_INSTALL_DIR}/lib/libusd_hd.so` with `FORBIDDEN=^libusd_tf`
  (libusd_hd NEEDEDs libusd_tf) → gate **FATAL**s with
  `Gate B-1 VIOLATION: libusd_hd.so links forbidden library: libusd_tf.so`.
- `testUsdGenLinkRulePositiveFixture`: gate on `build/libusdGen.so` with the
  real B-1 family regex → passes (shared branch actually exercised now).
- Also verified by hand: `usdGenMath.a` passes via the static branch
  (magic `213c6172`, nm+c++filt clean); a missing target file FATALs;
  `libclean.a` (fixture) passes; `libforbidden.a` (fixture referencing
  `pxrInternal_v0_26_8__pxrReserved__::UsdStage::Open`) FATALs with
  `references forbidden pxr symbol`.

### S-2 (P0) — INCLUDE-RULE GATE SCANS ZERO FILES → **APPLIED**

`cmake/CheckNoStageInclude.cmake` (rewritten):
- The single quoted `"dir/*.h;dir/*.cpp"` glob argument (one invalid
  semicolon-joined pattern → zero files scanned → always "passed") is now
  **two separate `file(GLOB_RECURSE)` patterns** per protected dir.
- **False-pass guards added** (all FATAL):
  - `SOURCE_DIR` / `FORBIDDEN` missing;
  - any of the three protected dirs (`libs/usdGenMath`, `libs/usdGen`,
    `testutils/usdGenTestUtils`) missing;
  - a protected dir containing no `.h/.cpp` files;
  - the total protected-source set empty (the original zero-file vector).
- Status line now reports the file count scanned.

**Proof (negative + positive fixtures, new tests):**
- `tests/fixtures/gateIncludeForbidden/` — mirrors the protected layout,
  `libs/usdGen/bad.cpp` contains `#include "pxr/usd/usd/stage.h"` →
  `testUsdGenIncludeRuleNegativeFixture` asserts the gate FATALs with
  `Gate B-1 include VIOLATION: ...bad.cpp: #include "pxr/usd/usd/stage.h"`.
- `tests/fixtures/gateIncludeClean/` — allowed includes only →
  `testUsdGenIncludeRulePositiveFixture` asserts a pass.
- The real-repo `testUsdGenIncludeRule` now scans **7 files** (previously 0)
  and passes.

### X-1 (P0) — B-1 LABEL COVERAGE → **APPLIED**

- `testUsdGenNoThirdPartyExports` now carries `LABELS "T0;build;gate:B-1"`
  (was `T0;build`).
- `ctest -L '^gate:B-1$' -N` now selects **9 tests**: the five original B-1
  checks (3× link-rule, include-rule, no-third-party-exports) plus the four
  new fixture tests. All five original B-1 tests are covered (superset OK).

### S-3 / X-2 (P0) — CHAIN-ORDER TEST PROVES THE M0 VERTICAL DEMO → **APPLIED**

`tests/testUsdGenChainOrder.cpp` (rewritten). Verified against the real 26.08
chain (dump below) before writing assertions:

1. **PXR_PLUGINPATH_NAME** now includes both OpenUSD roots
   (`${USD_INSTALL_DIR}/plugin/usd` **and** `${USD_INSTALL_DIR}/lib/usd`)
   plus the usdGenSchema + usdGenImaging build-tree resource dirs and the
   new fakeHdPrman fixture dir.
2. **Scene-globals boundary made mandatory**: the test now mirrors the
   usdImagingGL engine structure — the usdImaging terminal is wrapped in
   `HdsiSceneGlobalsSceneIndex::New(...)`
   (`usdImagingGL/engine.cpp:155,1778`) before
   `AppendSceneIndicesForRenderer("GL", ...)` — and asserts
   `HdsiSceneGlobalsSceneIndex` is **present** and `UsdGenGroomSceneIndex`
   is strictly after it (sees `currentFrame` in usdview and usdrecord).
3. **No more "if found" guards**: mandatory presence asserted for
   `UsdGenGroomSceneIndex`, `HdsiSceneGlobalsSceneIndex`,
   `UsdImagingStageSceneIndex`, `HdDependencyForwardingSceneIndex`,
   `HdsiImplicitSurfaceSceneIndex`, `HdsiMaterialBindingResolvingSceneIndex`,
   an `HdSt*/Storm` node (observed display name
   "HdSt: declare Storm dependencies"), and the fake hdPrman node.
   Ordering: every UsdImaging/UsdSkel node strictly upstream of usdGen;
   under the default (Hybrid) policy **every** observed Hdsi*/HdSt*/Storm
   renderer node strictly downstream of usdGen (the two usdImaging-internal
   Hd*/Hdsi* types `HdNoticeBatchingSceneIndex` and
   `HdsiLocatorCachingSceneIndex` are correctly classified usd-side).
   Under JsonMetadataOnly only usdGen's own contract is asserted (hdsi
   nodes may legally reposition via their own JSON metadata — observed).
4. **Synthetic hdPrman ordering case**: new fixture
   `tests/fixtures/fakeHdPrmanPlugin.cpp` +
   `tests/fixtures/fakeHdPrman/resources/plugInfo.json.in` — a minimal
   all-renderers `HdSceneIndexPlugin` tagged `hdPrman:motionBlur`, loaded
   purely through PXR_PLUGINPATH_NAME (new `fakeHdPrmanPlugin` CMake target,
   build-tree plugInfo generated like usdGenImaging's). The test asserts
   usdGen sits strictly **before** it (its plugInfo
   `ordering.before: ["hdPrman:motionBlur"]` honored) under **both** the
   default Hybrid policy and `SetPluginOrderingPolicy(JsonMetadataOnly)`.
5. **Tf coding errors fail the test**: `TfErrorMark` wraps the whole body.
   The test deliberately does **not** link `usdGenImaging` — the plugins
   (usdGen + fixture) are discovered via PXR_PLUGINPATH_NAME and dlopen'd
   during the test, so the mark catches load-time coding errors such as the
   S-4 double-registration ("TfType ... already has a defined C++ type").
   Link set reduced to stock `usdImaging hdsi hd usd tf arch`.

Observed chain (Hybrid, terminal → upstream), 28 nodes:
```
0  HdDependencyForwardingSceneIndex
1  HdsiUnboundMaterialPruningSceneIndex
2  :_SceneIndex [HdSt: declare Storm dependencies]
3  :_RenderPassVisibilitySceneIndex
4  HdsiBackPlateSceneIndex
5  HdsiMaterialPrimvarTransferSceneIndex
6  HdsiRenderPassPruneSceneIndex
7  HdsiVelocityMotionResolvingSceneIndex
8  HdsiTetMeshConversionSceneIndex
9  HdsiNurbsApproximatingSceneIndex
10 HdSiNodeIdentifierResolvingSceneIndex
11 HdsiMaterialBindingResolvingSceneIndex
12 HdsiImplicitSurfaceSceneIndex
13 FakeHdPrmanMotionBlurSceneIndex
14 UsdGenGroomSceneIndex
15 HdsiSceneGlobalsSceneIndex
16 UsdImagingRenderSettingsFlatteningSceneIndex
...
27 UsdImagingStageSceneIndex
```
(JsonMetadataOnly: fake lands at 8, usdGen at 9 — usdGen still strictly
before it and after sceneGlobals at 15.)

### S-4 / X-8 (P1) — METADATA PLUGIN DOUBLE-REGISTERED + KILL SWITCH INCOMPLETE → **APPLIED**

- `libs/usdGenImaging/usdGenImaging/metadataSceneIndexPlugin.cpp`:
  removed the explicit `TF_REGISTRY_FUNCTION(TfType)`
  (`TfType::Define<UsdGenMetadataSceneIndexPlugin, Bases<...>>()`).
  `UsdImagingSceneIndexPlugin::Define<T>()` already performs
  `TfType::Define<T, Bases<UsdImagingSceneIndexPlugin>>().SetFactory` —
  the second Define emitted
  `Coding Error: TfType 'UsdGenMetadataSceneIndexPlugin' already has a
  defined C++ type; cannot redefine` on every load. The
  `TF_REGISTRY_FUNCTION(UsdImagingSceneIndexPlugin)` factory path (the one
  OpenUSD's `GetAllSceneIndexPlugins()` consumes) is kept.
- **Shared kill switch**: new `libs/usdGenImaging/usdGenImaging/usdGenEnable.h`
  declares `extern TfEnvSetting<bool> USDGEN_ENABLE;`; the single
  `TF_DEFINE_ENV_SETTING` now lives in `groomSceneIndexPlugin.cpp`
  (description updated: disables *both* plugins). Both
  `InstanceDataSourceNames()` and `ProxyPathTranslationDataSourceNames()`
  return empty `TfTokenVector`s when `USDGEN_ENABLE` is false.
- Verified: with `USDGEN_ENABLE=0` the chain no longer contains
  `UsdGenGroomSceneIndex` (banner "USDGEN_ENABLE is overridden to
  'false'" printed) while the fixture node remains; the chain test's
  TfErrorMark check reports **0** coding errors in the enabled run.

### P2 items — recorded only (not assigned / out of scope)
- S-9 (EGL `/dev/dri` heuristic): noted; T2 work, not M0-gated.
- No sol finding was rejected as factually wrong; X-1's "five B-1 tests"
  count is met as a superset (9 with the new fixtures).

## Test registration / labels (CMakeLists.txt, lane-A portion only)

- `testUsdGenNoThirdPartyExports` → `LABELS "T0;build;gate:B-1"` (X-1).
- New `fakeHdPrmanPlugin` SHARED target (`tests/fixtures/fakeHdPrmanPlugin.cpp`,
  links `hd hdsi tf arch pxOsd`, default visibility) + generated
  `build/usd/fakeHdPrman/resources/plugInfo.json` (`LibraryPath` via
  `$<TARGET_FILE_NAME:fakeHdPrmanPlugin>`, tag `hdPrman:motionBlur`).
- `testUsdGenChainOrder`: link set changed to `usdImaging hdsi hd usd tf arch`
  (deliberately **no** usdGenImaging — plugins load via PXR_PLUGINPATH_NAME
  so TfErrorMark sees their init); ENVIRONMENT now
  `PXR_PLUGINPATH_NAME=<schema>:<imaging>:<fakeHdPrman>:<plugin/usd>:<lib/usd>;LD_LIBRARY_PATH=...`.
- Four new fixture tests (`T0;build;gate:B-1`):
  `testUsdGenLinkRuleNegativeFixture`, `testUsdGenLinkRulePositiveFixture`,
  `testUsdGenIncludeRuleNegativeFixture`, `testUsdGenIncludeRulePositiveFixture`
  (drivers: `tests/fixtures/gateLinkDriver.cmake`,
  `tests/fixtures/gateIncludeDriver.cmake`).

## ctest summary

Reconfigured + `ninja -C build` clean (only pre-existing TBB deprecation
pragmas). Full run:

```
env LD_LIBRARY_PATH=$USD/lib \
  ctest --test-dir build -L '^T[01]$' --output-on-failure
...
100% tests passed, 0 tests failed out of 11

Label Time Summary:
T0          =   0.18 sec*proc (9 tests)
T1          =   0.10 sec*proc (2 tests)
build       =   0.18 sec*proc (9 tests)
gate:B-1    =   0.18 sec*proc (9 tests)
```

- T0 (9): 3× link-rule, include-rule, no-third-party-exports (all five B-1
  original tests) + 4 fixture tests (2 negative, 2 positive).
- T1 (2): testUsdGenChainOrder (PASS, 0 failures, 0 Tf coding errors,
  full boundary assertions in both policy cases), testUsdGenPluginDiscovery.
- `ctest -L '^gate:B-1$' -N` → 9 tests (superset of the required five).

## Files (git status)

Whole M0 tree is untracked (sol X-9: nothing committed yet) — my files, all
new/modified within untracked top-level entries:

```
?? CMakeLists.txt                  (test registration/labels: B-1 fixtures,
                                     fakeHdPrman target, chain test env/link)
?? cmake/CheckNoStageLink.cmake    (S-1 rewrite)
?? cmake/CheckNoStageInclude.cmake (S-2 rewrite)
?? cmake/CheckNoThirdPartyExports.cmake (S-1 named location: silent-fail fix)
?? tests/testUsdGenChainOrder.cpp  (S-3/X-2 rewrite)
?? tests/fixtures/gateLinkDriver.cmake, gateIncludeDriver.cmake
?? tests/fixtures/gateIncludeForbidden/ (3 files), gateIncludeClean/ (4 files)
?? tests/fixtures/gateLinkForbiddenStatic/libforbidden.a,
   tests/fixtures/gateLinkCleanStatic/libclean.a
?? tests/fixtures/fakeHdPrmanPlugin.cpp,
   tests/fixtures/fakeHdPrman/resources/plugInfo.json.in
?? libs/usdGenImaging/usdGenImaging/usdGenEnable.h (new)
?? libs/usdGenImaging/usdGenImaging/metadataSceneIndexPlugin.{h,cpp} (S-4)
?? libs/usdGenImaging/usdGenImaging/groomSceneIndexPlugin.cpp (shared env setting)
```

No git commits made (per lane rules).
