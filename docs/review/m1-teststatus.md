> Historical engineering note. It records a review or probe, not the current product overview. Start at the [repository README](../../README.md) and [docs index](../README.md).

# M1 Test-Status Census

Date: 2026-09-09 · Tree: `usdGen` @ `831c869` (+ untracked in-flight work from sibling lanes)
Census of **all 33 registered ctest entries** (`ctest -N` → 33; table below has 33 rows).

## Commands run (env: `set -a; . bin/_env.sh; set +a`, from `build/`)

| Set | Command | Result |
|---|---|---|
| Build | `cmake --build build` (once) + targeted `--target testUsdGenPluginDiscovery` | 2 failed compile units (see Build findings) |
| T0/T1 | `ctest -L '^T[01]$' -j8 --output-on-failure` | 27 tests: 21 "passed" (incl. 6 self-skips), 3 failed, 6 not-run |
| T2 storm | `ctest -L '^gate:S' -R Storm --output-on-failure` (serial) | 4 tests, all Skipped (M1 stubs) |
| T2 look | `ctest -L '^gate:L-1$' --output-on-failure` (serial) | 1 test, Skipped (M1 stub) |
| T2 look (L-2, outside the commands above) | `./testUsdGenStormLook` direct (PXR_PLUGINPATH_NAME set to build-tree plugin dirs) | exit 77, prints `M1-STUB testUsdGenStormLook` → Skipped |

`USDGEN_GATE=1` was **not** needed: it only forces timing-gate failure in the perf benches
(`benchUsdGenChain`/`benchUsdGenSparse`); all T2 Storm tests are M1 stubs, so no EGL/GPU work ran.

## Summary

- **PASS: 18** (of which 2 are ctest-"Passed" self-reported post-install skips, marked `*`; 1 ran a stale binary, marked `**`)
- **FAIL: 3** — `testUsdGenGraph`, `benchUsdGenChain`, `testUsdGenAutoApply`
- **SKIP: 12** — 6 T0/T1 M1-stub exits-77 / `M1-STUB` echoes, 6 T2 (4 registered stubs + 2 stub binaries)
- Build: `libusdGenImaging.so` **does not compile** (sibling-owned, EngineCore lane in flight) → any target linking it cannot relink; ctest ran pre-existing binaries (vintages noted below).

## Test table

Legend: `*` = ctest reports "Passed" (exit 0) but the test prints its own SKIP reason (post-install gate).
`**` = test ran from a **stale binary** (predates current source; see vintages).
First-error column: `file:line` of the failing check / first compile error.

| # | Test | Tier | Gate | Verdict | Evidence (one-liner) | First error (if failing) |
|---|---|---|---|---|---|---|
| 1 | testUsdGenLinkRule_usdGen | T0 | gate:B-1 | PASS | `CheckNoStageLink`: no forbidden `^libusd_(usd\|hd\|hgi\|glf\|garch)` linkage in `build/libusdGen.so` (0.03 s) | — |
| 2 | testUsdGenLinkRule_usdGenMath | T0 | gate:B-1 | PASS | `CheckNoStageLink` on `build/libusdGenMath.a` clean (0.05 s) | — |
| 3 | testUsdGenLinkRule_usdGenTestUtils | T0 | gate:B-1 | PASS | `CheckNoStageLink` on `build/libusdGenTestUtils.a` clean (0.10 s) | — |
| 4 | testUsdGenIncludeRule | T0 | gate:B-1 | PASS | source tree has no forbidden `pxr/(usd\|imaging\|usdImaging)/...` stage includes (0.06 s) | — |
| 5 | testUsdGenNoThirdPartyExports | T0 | gate:B-1 | PASS | no third-party symbols in `libusdGen.so` export table (0.11 s) | — |
| 6 | testUsdGenLinkRuleNegativeFixture | T0 | — | PASS | negative fixture correctly detected as stage-linked (EXPECT=fail) (0.05 s) | — |
| 7 | testUsdGenLinkRulePositiveFixture | T0 | — | PASS | clean fixture passes `CheckNoStageLink` (EXPECT=pass) (0.03 s) | — |
| 8 | testUsdGenIncludeRuleNegativeFixture | T0 | — | PASS | fixture with a forbidden include is flagged (EXPECT=fail) (0.03 s) | — |
| 9 | testUsdGenIncludeRulePositiveFixture | T0 | — | PASS | clean fixture passes include scan (EXPECT=pass) (0.03 s) | — |
| 10 | testUsdGenChainOrder | T1 | gate:SI-5 | PASS | chain-order smoke passes (0.21 s; binary 08:11) | — |
| 11 | testUsdGenPluginDiscovery | T1 | — | PASS `**` | ran stale 08:11 binary (last compilable source) → ok; current TU compiles clean after 1-line fix, relink blocked by `libusdGenImaging` (see findings) | — (build: `tests/testUsdGenPluginDiscovery.cpp:73`, now fixed; link: `libs/usdGenImaging/.../groomSceneIndexPlugin.h:90`) |
| 12 | testUsdGenGraph | T0 | gate:E-6 | **FAIL** | E-6: incremental recompile (200→201 nodes) = **0.903 ms > 0.2 ms limit**; the other three E-6 checks pass (binary built 11:47 by sibling lane) | `tests/testUsdGenGraph.cpp:178` |
| 13 | testUsdGenKernelDeterminism | T0 | gate:E-8 | PASS | 1- vs 8-thread bitwise identity passes (0.29 s; binary built 11:47 by sibling lane) | — |
| 14 | benchUsdGenChain | T0 | perf, gate:E-1, gate:E-7 | **FAIL** | E-1 2.059 ms PASS, E-7 6.51× PASS, but **E-6 section: compile failed** — `no kernel registered for 'UsdGenClump' (prim /chain/n1)` (binary built 11:47 by sibling lane) | report: `tests/perf/benchUsdGenChain.cpp:238` · root: `libs/usdGen/usdGen/compiler.cpp:464` |
| 15 | benchUsdGenSparse | T0 | perf, gate:E-2 | PASS | 1%-chunk sparse gates pass (0.18 s; binary built 11:47 by sibling lane) | — |
| 16 | testUsdGenTileContract | T1 | gate:SI-1 | SKIP | M1 stub: exit 77 = `SKIP_RETURN_CODE` | — |
| 17 | testUsdGenInvalidation | T1 | gate:SI-2 | PASS | invalidation smoke passes (0.03 s; binary 08:11) | — |
| 18 | testUsdGenScheduling | T1 | gate:SI-3 | PASS | scheduling smoke passes (0.03 s; binary 08:11) | — |
| 19 | testUsdGenSnapshotRace | T1 | gate:SI-4 | PASS | snapshot-race smoke passes (0.08 s; binary 08:11) | — |
| 20 | testUsdGenPopulation | T1 | gate:SI-6 | SKIP | M1 stub: exit 77 = `SKIP_RETURN_CODE` | — |
| 21 | testUsdGenAdapter | T1 | gate:SI-7 | SKIP | M1 stub: exit 77 = `SKIP_RETURN_CODE` | — |
| 22 | testUsdGenRestAdapter | T1 | gate:SI-7 | SKIP | M1 stub: exit 77 = `SKIP_RETURN_CODE` | — |
| 23 | testUsdGenAutoApply | T1 | gate:SI-8 | **FAIL** | 10/12 checks ok; **`mask:invert` and `mask:combine` are not M1-neutral** — expected unchanged mask + `invert`/`combine` diagnostic missing (binary 08:11) | `tests/testUsdGenAutoApply.cpp:239` (invert) · `:244` (combine) |
| 24 | testUsdGenContracts | T1 | — | SKIP | M1 stub: exit 77 (3-line stub `tests/testUsdGenContracts.cpp`, untracked predecessor leftover) | — |
| 25 | testUsdGenSchemaUpToDate | T1 | — | SKIP | ctest-level stub: `cmake -E echo "M1-STUB ..."` matches `SKIP_REGULAR_EXPRESSION` | — |
| 26 | testUsdGenStormLook | T2 | gate:L-2 | SKIP | M1-stub binary: exit 77, prints `M1-STUB testUsdGenStormLook` (ran directly; gate L-2 is outside the specified T2 command set) | — |
| 27 | benchUsdGenStorm | T2 | perf, gate:S-1/S-5/S-6/S-12 | SKIP | M1-stub binary: exit 77, prints `M1-STUB benchUsdGenStorm` | — |
| 28 | testUsdGenStormTangent | T2 | gate:S-8 | SKIP | ctest-level stub: `M1-STUB` echo (wave-3 source pending) | — |
| 29 | testUsdGenStormHgiResource | T2 | gate:S-8 | SKIP | ctest-level stub: `M1-STUB` echo (wave-3 source pending) | — |
| 30 | testUsdGenStormRefine | T2 | gate:S-9 | SKIP | ctest-level stub: `M1-STUB` echo (wave-3 source pending) | — |
| 31 | testUsdGenStormMaterial | T2 | gate:L-1 | SKIP | ctest-level stub: `M1-STUB` echo (wave-3 source pending) | — |
| 32 | testUsdGenInstallTree | T0 | — (install) | PASS `*` | exit 0 prints `SKIP (post-install gate) — USDGEN_INSTALL_PREFIX unset and no build-tree package in the current (build) directory` (binary 11:15, predecessor build) | — |
| 33 | testUsdGenConsumer | T0 | — (install) | PASS `*` | exit 0 prints `USDGEN_INSTALL_PREFIX unset — skipping the downstream consumer build` (binary 09-07 20:23, oldest in tree) | — |

## Build findings (not ctest entries)

1. **`libs/usdGenImaging/` — does not compile (sibling-owned, EngineCore lane in flight — left untouched, reported only).**
   - First error: `libs/usdGenImaging/usdGenImaging/groomSceneIndexPlugin.h:90:10` — `UsdGenGroomSceneIndex::_SystemMessage(int)` marked `override`, but does not override
   - `groomSceneIndexPlugin.h:162:13` — `UsdGenGroomSceneIndexPlugin::_GetRendererName() const` marked `override`, but does not override
   - `groomSceneIndexPlugin.cpp:36:1` — no declaration matches `_AppendSceneIndex(const std::string&, std::vector<HdgPrim const*>&)`; header `:158` declares the 2-arg overload
   - **Impact:** `libusdGenImaging.so` (`CMakeLists.txt:157` GLOB) fails → `testUsdGenPluginDiscovery` (`CMakeLists.txt:382` links `usdGenImaging`) cannot relink; ctest ran its stale 08:11 binary (verdict `**` above).
2. **`tests/testUsdGenPluginDiscovery.cpp:73` — compile error, FIXED this census** (per Main's ownership exception):
   `invalid conversion from 'TfWeakPtr<PlugPlugin>' to 'const PlugPlugin*'`. One-line fix:
   `const PlugPlugin *plugin = get_pointer(registry.GetPluginWithName(pluginName));`
   (public free-fn overload, `pxr/base/tf/weakPtrFacade.h:294`; checks liveness). TU now compiles clean; relink still blocked by finding 1.
3. **`tests/testUsdGenInstallTree.cpp:77` — deprecation warning only** (`UsdSchemaRegistry::IsConcrete`; pxr suggests `TfGetSchemaRegistry`). Compiles and passes; **not a compile error, left untouched** (no scope expansion).

## Caveats

- **Binary vintages** (shared build dir, sibling lanes build concurrently):
  - 09-07 20:23 — `Consumer`
  - 09-09 08:11–08:12 — imaging batch: `SnapshotRace`, `AutoApply`, `PluginDiscovery`, `Scheduling`, `Invalidation`, `Adapter`, `TileContract`, `StormLook`, `Contracts`, `RestAdapter`, `Population`, `benchStorm`
  - 09-09 11:15 — `InstallTree` (predecessor build)
  - 09-09 11:47 — `KernelDeterminism`, `benchSparse`, `benchChain`, `Graph` (sibling-lane build)
  Verdicts reflect these binaries; any test linking the failing `libusdGenImaging` cannot re-run against current source until finding 1 lands.
- Predecessor leftovers present in tree (untracked): `docs/freezes/`, `docs/m1/`, `tests/testUsdGenContracts.cpp` (3-line stub, exit 77) — not modified by this census.
