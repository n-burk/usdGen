> Historical engineering note. It records a review or probe, not the current product overview. Start at the [repository README](../../README.md) and [docs index](../README.md).

# sol M0 exit-criteria notes

## Checklist

| item | status (PASS/FAIL/IN-PROGRESS) | evidence | fix needed |
|---|---|---|---|
| 1 | FAIL | All five B-1-related tests are registered and pass. `CMakeLists.txt:218-249` labels the three `testUsdGenLinkRule_*` tests and `testUsdGenIncludeRule` as `T0;build;gate:B-1`, but `testUsdGenNoThirdPartyExports` has only `T0;build`. CTest consequently reports 5 T0 tests but only 4 `gate:B-1` tests. | Add `gate:B-1` to `testUsdGenNoThirdPartyExports`, reconfigure, and confirm the gate label selects five tests. |
| 2 | FAIL | The requested run completed: **7/7 passed** in 0.24 s; T0: 5, T1: 2. Both `testUsdGenPluginDiscovery` and `testUsdGenChainOrder` pass at T1. However, the chain-order test does not cover the complete M0 vertical-demo contract: no mandatory `HdsiSceneGlobalsSceneIndex`/`HdSt_*` assertions and no synthetic `hdPrman:*` ordering case. Its verbose run also emits a Tf coding error. | Strengthen `testUsdGenChainOrder` to assert every required boundary and synthetic hdPrman ordering, and eliminate the coding error. |
| 3 | PASS | The requested `usdcat --flatten` run exits 0 and contains `def UsdGenGroom` and `def UsdGenDescription`. The current positive-control probe exits 0 with both types known/concrete and `ComputeExtent ok=1 size=2`, extent `[(-0.5,-0.5,-0.5)..(0.5,0.5,0.5)]`, `empty=0`. `testUsdGenPluginDiscovery` independently passes the same extent assertions. | None. Retain the evidence document’s clarification that flattening itself does not request or print an extent. |
| 4 | FAIL | T0 and T1 labels exist in `CMakeLists.txt`; T0–T4 definitions in `plan/10-build-dependencies-testing.md:794-863` and `plan/11-roadmap.md:66-72` agree. However, `README.md:46-49,111-113` falsely says two link-rule tests lack T0 labels, and `.github/workflows/usdgen.yml:283-288` repeats that stale claim while lines 315-318 say the opposite. | Remove the obsolete bridge/label statements and describe the current seven-test label selection consistently. |
| 5 | FAIL | `.github/workflows/usdgen.yml` exists and PyYAML parses it successfully. Jobs include `configure-offline`, `build-release`, `t0-t1`, and `install-check`; T0+T1 uses the exact anchored selector with `-j8`; T2/T3/T4 are documented as workstation/manual. The install job nevertheless omits the plan-required installed-tree `testUsdGenInstallTree`, `testUsdGenConsumer`, and `testUsdGenPluginDiscovery`. The generated `build/usdGenConfig.cmake:36-37` contains empty plugin/library directories, so the current layout-only job can pass a broken consumer package. | Add `PATH_VARS USDGEN_INSTALL_PLUGINDIR USDGEN_INSTALL_LIBDIR` to `configure_package_config_file`, register the three install/consumer tests, and run them exclusively against the scratch install. |
| 6 | FAIL | `docs/workstation-protocol.md` has exactly 11 numbered sections with the required topic mapping; every section contains command, environment, and artefact fields, and no `W-1`…`W-4` identifiers occur. Section 5 is incomplete for its Metal/Vulkan purpose: Vulkan has no artefact-producing capture command, while Metal is only prose and supplies neither an exact command nor macOS loader environment. | Add copy-pasteable Vulkan and Metal commands, including output/log paths and the applicable macOS environment such as `DYLD_LIBRARY_PATH`. |
| 7 | FAIL | A clean shell sourcing `bin/_env.sh` gets the USD prefix, plugin path, and USD library path, but `PATH` remains `/usr/bin:/bin` and `usdcat` is `NOT_FOUND`. `USD`, `PY`, and the repository/build-root variables are not exported; `LD_LIBRARY_PATH` omits the usdGen build directory. This differs from `plan/10` §3.6. | Export `USD`, `GEN`, `GENBUILD`, and `PY`; prepend `$GENBUILD` and `$USD/bin` to `PATH`; add `$GENBUILD` to the loader path; use the complete build-tree and stock plugin paths. |
| 8 | PASS | All six required files exist. Each contains a literal `DECISION:` line: PW-1 line 106, PW-2 line 142, PW-3 line 232, PW-4 line 96, PW-5 line 176, and PW-6 line 315. | None. |
| 9 | FAIL | `LICENSE`, `NOTICE`, and `README.md` exist, and the README clearly limits current functionality to M0. `.gitignore` covers the main build tree, T4 output, and known probe executables, but has no `scratch/` rule. Seven generated, unignored probe stages total about 88 MiB, including three 12–43 MiB `.usdc` files, despite the plan’s generated-large-stage policy. | Add an explicit `scratch/` rule and ignore or remove regenerable PW-egl stages before staging the M0 tree. |
| 10 | IN-PROGRESS | `git status --porcelain` reports the entire M0 implementation untracked: `.github/`, `.gitignore`, `CMakeLists.txt`, `LICENSE`, `NOTICE`, `README.md`, `bin/`, `cmake/`, `docs/`, `libs/`, `plugin/`, `tests/`, `testutils/`, and `thirdparty/`. Ignored local artefacts include `build/`, `Testing/`, the PW-egl build tree, and extensionless probe executables. The large PW-egl generated stages are unignored. | Resolve findings first, exclude generated probe stages and stale review reports, then stage the intended M0 source/document set and re-run status. |

## Findings

### [P0] X-1: B-1 label coverage is incomplete

- location: `CMakeLists.txt:243-249`
- issue: `testUsdGenNoThirdPartyExports` lacks the required `gate:B-1` label.
- evidence: The canonical run reports T0 = 5 tests but `gate:B-1` = 4 tests.
- fix: Set `LABELS "T0;build;gate:B-1"` on the test, reconfigure, and verify `ctest -L '^gate:B-1$'` selects all five checks.

### [P0] X-2: The green chain-order test does not prove the M0 vertical demo

- location: `tests/testUsdGenChainOrder.cpp:106-155`
- issue: `HdsiSceneGlobalsSceneIndex` is checked only if found; no `HdSt_*` boundary is asserted; the second case merely repeats renderer `GL` under `JsonMetadataOnly` and never installs or checks synthetic `hdPrman:*` tags.
- evidence: The verbose chain contains no named `HdsiSceneGlobalsSceneIndex` or `HdSt_*` node, and the source contains no `hdPrman` reference, yet the test passes.
- fix: Require the relevant boundary nodes, assert usdGen’s position relative to every renderer-side node, and add explicit synthetic hdPrman phase/order registrations under `SetPluginOrderingPolicy`.

### [P0] X-3: Tier documentation contradicts the implemented labels

- location: `README.md:46-49,111-113`; `.github/workflows/usdgen.yml:283-288`
- issue: These passages say the Math/TestUtils link tests lack T0 labels and require a by-name bridge, although both are currently labelled and the bridge is gone.
- evidence: `CMakeLists.txt:230-233` labels all three foreach-generated link tests; workflow lines 315-318 directly contradict lines 283-288.
- fix: Delete the obsolete passages and state that the anchored T0/T1 run covers all seven M0 tests.

### [P0] X-4: Install CI can pass a broken installed package

- location: `.github/workflows/usdgen.yml:323-460`; `CMakeLists.txt:311-314`; `cmake/usdGenConfig.cmake.in:12-13`
- issue: `install-check` validates files and JSON only; it does not perform the installed-tree tests required by plan §6.2.
- evidence: `testUsdGenInstallTree` and `testUsdGenConsumer` are not registered. The generated config contains `set_and_check(usdGen_PLUGIN_DIR "")` and `set_and_check(usdGen_LIBRARY_DIR "")`.
- fix: Configure the package with both install variables in `PATH_VARS`, implement/register the missing tests, and run install-tree discovery and a downstream `find_package(usdGen)` consumer from the scratch prefix.

### [P0] X-5: Workstation protocol §5 is not executable for all required backends

- location: `docs/workstation-protocol.md:267-319`
- issue: The Vulkan leg launches an interactive viewport without producing its specified log/image, and the Metal leg is only a prose instruction.
- evidence: No literal Metal command appears; no Vulkan output command writes `compile_vulkan.log` or a Vulkan screenshot; only Linux `LD_LIBRARY_PATH` is documented.
- fix: Provide exact Vulkan and Metal invocations, backend selection/build prerequisites, platform-appropriate loader paths, and commands that create every named artefact.

### [P0] X-6: `bin/_env.sh` does not establish the required usable environment

- location: `bin/_env.sh:12-28`
- issue: The script does not add OpenUSD tools to `PATH`, does not add the usdGen build directory to `LD_LIBRARY_PATH`, and does not export the principal path variables.
- evidence: In a clean shell after sourcing, `PATH=/usr/bin:/bin`, `command -v usdcat` returns nothing, and only `PXR_PLUGINPATH_NAME` and `LD_LIBRARY_PATH` appear in `export -p`.
- fix: Implement the environment contract from `plan/10` §3.6, including exported `USD`, `GEN`, `GENBUILD`, `PY`, OpenUSD/build binaries on `PATH`, and both USD/build libraries on the loader path.

### [P0] X-7: Scratch and generated large-stage hygiene is incomplete

- location: `.gitignore:1-47`; `docs/prework/probes/PW-egl/`
- issue: There is no explicit scratch-directory rule, and regenerable large benchmark stages remain unignored.
- evidence: `git check-ignore scratch` finds no rule. Unignored files include `scene_100k_A.usdc` (12 MiB), `scene_100k_B.usdc` (21 MiB), `scene_200k_B.usdc` (43 MiB), and four approximately 3.3 MiB `pw4_*.usda` stages.
- fix: Add `scratch/` and explicit PW-egl generated-stage patterns, then remove those files from the intended commit set.

### [P1] X-8: Chain-order execution emits an ignored Tf coding error

- location: `libs/usdGenImaging/usdGenImaging/metadataSceneIndexPlugin.cpp:13-22`
- issue: `UsdGenMetadataSceneIndexPlugin` is defined twice: directly through `TfType::Define` and again through `UsdImagingSceneIndexPlugin::Define`.
- evidence: Verbose CTest prints `TfType 'UsdGenMetadataSceneIndexPlugin' already has a defined C++ type; cannot redefine`, but the executable still returns success.
- fix: Remove the redundant direct `TfType::Define` registration and retain the documented `UsdImagingSceneIndexPlugin::Define<T>()` factory registration; make unexpected Tf coding errors fail the test.

### [P1] X-9: The M0 implementation is entirely untracked

- location: repository worktree
- issue: None of the M0 source, tests, CI, documentation, licence, or vendored dependency files is currently tracked.
- evidence: `git status --porcelain` reports 14 untracked top-level entries covering the complete implementation.
- fix: After resolving the gate failures and excluding generated/stale files, stage the intended M0 tree and inspect `git diff --cached --stat` plus `git status --porcelain`.

### [P2] X-10: Ignored local build products remain in the workspace

- location: `build/`, `Testing/`, `docs/prework/probes/PW-egl/build/`, and extensionless binaries under `docs/prework/probes/`
- issue: Numerous compiled objects and executables remain locally, although the current ignore rules prevent them from entering status.
- evidence: File inspection finds aarch64 ELF executables and object files in those paths; `git status --ignored` marks them `!!`.
- fix: No commit fix is required; optionally clean these local products after preserving any needed raw measurements.

## Summary

P0: 7, P1: 2, P2: 1; M0 exit: NOT READY (missing: items 1, 2, 4, 5, 6, 7, and 9; item 10 remains uncommitted)