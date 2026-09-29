> Historical engineering note. It records a review or probe, not the current product overview. Start at the [repository README](../../README.md) and [docs index](../README.md).

> **Review-runner note (qwen, 2026-09-07):** sol (gpt-5.6-sol via codex-sol) was invoked with
> `-s danger-full-access` because the host bwrap netns is broken and the read-only sandbox cannot
> start; sol's read-only discipline came from its prompt, not the sandbox. Runner verification:
> `find` over the worktree confirms **zero repo files modified during the sol run**
> (only `docs/review/` output written; exit code 0, single pass, no retry needed).

# sol M1 engine/imaging/tests validation notes

## Findings

### [P0] M-1: Current M1 sources do not compile and the tested build is stale
- location: libs/usdGen/usdGen/graph.h:29; libs/usdGen/usdGen/graph.cpp:21; libs/usdGen/usdGen/mask.cpp:151; libs/usdGen/usdGen/scheduler.cpp:120
- issue: The uncommitted worktree has multiple compile errors. Existing objects and `libusdGen.so` predate these edits, so CTest does not exercise the reviewed source.
- evidence: `c++ -std=c++17 -fsyntax-only ... graph.cpp` reports `redeclaration of ... descIdx`, invalid `UsdGenGraph::UsdGraph()`, and an undeclared destructor definition. The same command on `mask.cpp` reports `UsdGenMaskSettingsFromParams` redefined at lines 151/188; `scheduler.cpp` reports nonexistent `TfToken::Equals` and further invalid types/helpers. `stat` shows sources modified at 15:50–17:04 while objects and `build/libusdGen.so` are from 14:19.
- fix: Resolve all compile errors, declare/define the graph special members consistently, remove the duplicate mask function, replace invalid scheduler APIs/types, then rebuild and rerun every gate.
- why: M1 cannot be reproduced or committed in its current state; results from the older build cannot validate it.

### [P0] M-2: The execution and commit pipeline remains a scaffold
- location: libs/usdGen/usdGen/session.cpp:54; libs/usdGen/usdGen/scheduler.h:60; libs/usdGen/usdGen/scheduler.cpp:84; libs/usdGen/usdGen/generationStore.cpp:44; libs/usdGen/usdGen/compiler.cpp:402
- issue: `Commit()` does not compile, capture, evaluate, interleave, publish, or diff. `UsdGenScheduler::Run` is declared but has no definition; reference-lane completion is absent; thread calibration returns a hard-coded 8; generation diffing is empty. The four-digest/four-consequence model is not implemented: no graph digest exists, value state is a hash rather than the required monotonic version, and capture epochs are not driven by commits.
- evidence: `rg -n "UsdGenScheduler::Run|Run\\(UsdGenGraph" libs/usdGen` returns only `scheduler.h:64`. `rg -n "graphDigest|valueVersion|captureEpoch" libs/usdGen/usdGen` finds declarations and one reuse assignment, but no graph digest or version update. `session.cpp:71` returns `_store.Get()` without publishing.
- fix: Implement the eight-step commit, reference lane, per-node capture/evaluation caches, supersession, tile interleave, immutable publication, and diff. Implement the four distinct digest/version states and their specified consequences.
- why: Straight hair cannot execute end to end; E-1/E-2/E-6/E-7 and all publication gates are impossible.

### [P0] M-3: Determinism compiler policy does not cover the actual kernels
- location: CMakeLists.txt:93; libs/usdGen/usdGen/op.h:13; libs/usdGen/usdGen/ops/noise.cpp:187; libs/usdGenMath/version.cpp:29
- issue: `-ffp-contract=off` is correctly limited to `usdGenMath`, but the five real Capture/Evaluate kernels live in the `usdGen` target. `usdGenMath` still contains only the M0 placeholder resampler.
- evidence: `rg -n -- '-ffp-contract=off|ops/.*\\.cpp' build/build.ninja` shows `usdGenMath` with `-ffp-contract=off`, while all `usdGen/ops/*.cpp` flags omit it. `version.cpp:29-40` returns an empty resample result.
- fix: Move raw deterministic math kernels into `usdGenMath` and leave stage/parameter binding wrappers in `usdGen`, preserving the “usdGenMath only” flag rule. Add bitwise 1-thread/8-thread coverage.
- why: E-8 is neither guaranteed nor measured.

### [P0] M-4: Dirty routing and dirty-class propagation are nonfunctional
- location: libs/usdGenImaging/usdGenImaging/usdGenDirtyRouter.cpp:14; libs/usdGen/usdGen/compiler.cpp:104; libs/usdGen/usdGen/compiler.cpp:364
- issue: `UsdGenDirtyRouter` clears or ignores every notice. The compiler maps every `TopologyParameters()` entry to capture dirtiness, including structural properties such as Scatter `mode`, Grow `segments`, Length modes, and Width `replace`. Direct consumers are also omitted from `descendants` because the initial consumer is never inserted into `seen`.
- evidence: `nl -ba .../usdGenDirtyRouter.cpp` shows TODO-only `Rebuild`, `Route`, `RouteAdded`, and `RouteRemoved`. `compiler.cpp:127-129` returns `UsdGenDirtyCapture` for every topology parameter; `compiler.cpp:367-377` only inserts consumers reached after the first hop.
- fix: Generate a complete property-to-dirty-class table from schema/C1, distinguish structural/topology/capture/value/publication classes, implement longest-prefix routing and unknown-property handling, and include direct plus transitive descendants.
- why: SI-2 and SI-7 cannot produce exact affected nodes, tiles, or locator sets.

### [P0] M-5: The five-operator chain does not implement straight-hair semantics
- location: libs/usdGen/usdGen/ops/scatter.cpp:168; libs/usdGen/usdGen/ops/grow.cpp:124; libs/usdGen/usdGen/ops/length.cpp:187; libs/usdGen/usdGen/ops/noise.cpp:290; libs/usdGen/usdGen/ops/width.cpp:204
- issue: Random Scatter ignores density, emits exactly one root per face, initializes whole-mesh face indices to zero, and reads the first three topology indices for every face. Grow reads nonexistent `lengthRandomLo`/`lengthRandomHi` instead of the frozen `float2 lengthRandom`. Length parses its random range but uses raw `r`, and cull collapses curves instead of removing/repartitioning them. Grow/Length/Noise capture arrays are indexed chunk-locally without a global curve/CV offset. Noise and Width also hard-code linear ramp evaluation despite the normative interpolation contract.
- evidence: `scatter.cpp:171-200` performs `faces.resize(...)`, uses `R = faces.size()`, and repeatedly accesses `fvi[0..2]`; density appears only in the digest. `grow.cpp:127-128,159-160` requests split property names. `length.cpp:193-196` computes `restLen * value * r`, ignoring `lo`/`hi`.
- fix: Implement area/density-driven deterministic barycentric Scatter with correct face offsets; read frozen `float2` properties; apply `lo + r*(hi-lo)`; perform real cull topology changes; offset capture arrays by chunk descriptors; implement the frozen ramp interpolation modes.
- why: The E-1 five-op workload would not represent the specified groom even if the scheduler ran.

### [P1] M-6: Mask evaluation exceeds the M1 subset
- location: libs/usdGen/usdGen/mask.cpp:102
- issue: M1 is restricted to a constant term plus `usdGen:mask:random`, but the evaluator already applies range remapping, inversion, amount, combine mode, locked curves, and a ramp.
- evidence: `nl -ba .../mask.cpp | sed -n '102,175p'` shows range/invert processing at lines 121-140 and ramp construction at 145-146; the settings loader reads all those properties.
- fix: Keep post-M1 terms declared but neutral in M1. Evaluate only the constant and salted random term, with unsupported authored terms safely ignored.
- why: Premature semantics make M1 output diverge from the frozen milestone and risk accidentally freezing M4 behavior.

### [P0] M-7: C1/C2/interface freezes are absent and the schema is incomplete
- location: plan/11-roadmap.md:247; libs/usdGenSchema/schema.usda:35; libs/usdGenSchema/schema.usda:152
- issue: `docs/m1/interfaces.md`, C1, and C2 do not exist. The schema also diverges from the normative plan: Groom has a non-uniform `schemaVersion`, token/default `"default"` session ID, and lacks density fields/label; Description lacks guides, density, curve, width, motion, pick, and label properties; Noise/Width knots lack their interpolation properties.
- evidence: `for f in docs/m1/interfaces.md docs/freezes/C1.md docs/freezes/C2.md docs/freezes/C5.md ...` prints `MISSING` for the first three and `PRESENT` only for C5. `schema.usda:43-45` and `:58-72` show the incomplete definitions. `testUsdGenContracts` is a one-line exit-77 stub.
- fix: Materialize C1 and C2 from the normative plan, restore `docs/m1/interfaces.md`, reconcile every name/type/default/variability entry, regenerate schema artifacts, and make contract tests exhaustively compare them.
- why: M1 explicitly exits by freezing C1, C2, and C5; two freezes and the pinned interface are unavailable.

### [P0] M-8: Prim and RestAPI adapters publish and invalidate nothing
- location: libs/usdGenImaging/usdGenImaging/primAdapter.h:27; libs/usdGenImaging/usdGenImaging/usdGenRestApiDataSource.cpp:14; libs/usdGenImaging/usdGenImaging/restApiSchemaAdapter.h:13
- issue: The common prim adapter returns no subprims, type, data source, or invalidation locators. The Rest factory returns null, its invalidation map is empty, and its API adapter overrides no behavior.
- evidence: `nl -ba .../primAdapter.h` shows empty/null returns at lines 27-52; `usdGenRestApiDataSource.cpp:14-35` is entirely TODO/null. The generated plugInfo does contain the five prim-type entries and RestAPI entry, but their implementations are inert.
- fix: Implement one shared adapter body behind the five registered wrappers, surface every declared property without prefix filtering, map every property to dirtiness, and implement Default-time RestAPI topology/points/UV data sourcing.
- why: Both SI-7 tests are unmet.

### [P0] M-9: Imaging sessions, commits, population, surface capture, and stats are unwired
- location: libs/usdGenImaging/usdGenImaging/groomSceneIndexPlugin.h:44; libs/usdGenImaging/usdGenImaging/usdGenImagingSession.cpp:35; libs/usdGenImaging/usdGenImaging/usdGenGraphDescBuilder.cpp:21; libs/usdGenImaging/usdGenImaging/registry.cpp:10
- issue: The groom scene index is pass-through and never attaches a session. Notices are forwarded without routing or end-of-batch commit; `SetTime` only assigns a number; explicit `Commit` is empty; first `GetPrim` performs no population. Graph building returns an empty description, so Mesh/GeomSubset parent-face capture is absent. Session keys compare `sessionId` even with a reachable stage and omit required fallback namespace fields. Stats are declared but never incremented or published.
- evidence: `groomSceneIndexPlugin.h:44-70` delegates every method directly to its input. `usdGenImagingSession.cpp:43-52` is TODO-only. `BuildGraphDesc` returns at line 46 without reading the stage. `rg -n "publishedTiles|noticeEntries|supersessions" --glob '!plan/**' --glob '!docs/**'` finds declarations only.
- fix: Attach/reuse sessions from the scene index; implement notice-driven and first-GetPrim population; perform triggers (a), (b), and unconditional notice-thread trigger (c); keep worker `GetPrim` to atomic snapshot loads; implement Mesh/GeomSubset parent indexing, membership recapture, stable fallback keys, and stats publication.
- why: SI-3, SI-4, SI-6, R32, R15, and the required stats block are not delivered.

### [P0] M-10: Tile publication is empty and tile ranges overlap
- location: libs/usdGenImaging/usdGenImaging/usdGenTilePublisher.cpp:18; libs/usdGen/usdGen/graph.cpp:213
- issue: All tile topology/primvar data sources and notice generation return null/empty. Additionally, each tile’s `chunkCount` is set to every remaining chunk rather than at most `chunksPerTile`, producing overlapping tile views.
- evidence: `usdGenTilePublisher.cpp:18-52` returns null/empty. For 196 chunks and four chunks per tile, `graph.cpp:223-224` assigns counts `196, 192, 188, ...` instead of `4, 4, 4, ...`.
- fix: Set `chunkCount = min(chunksPerTile, nChunks-firstChunk)`, implement exact-size interleaving and every C2 data source/locator, and hard-assert `points.size() == sum(curveVertexCounts)` per tile.
- why: SI-1/SI-2 fail, and the nominal 49-tile result would not describe a valid partition.

### [P0] M-11: Every M1 T0/T1 gate test and benchmark is an exit-77 stub
- location: CMakeLists.txt:385; tests/testUsdGenGraph.cpp:1; tests/testUsdGenTileContract.cpp:1; tests/perf/benchUsdGenChain.cpp:1
- issue: No SI or E gate is actually exercised. CMake incorrectly claims exit 77 will be recorded as skipped, but no `SKIP_RETURN_CODE` is configured; CTest records failures. A skip would still be P0 under the M1 criteria.
- evidence: The prescribed `ctest -L '^T[01]$' --output-on-failure` reports `M1-STUB` for all 13 M1 tests and `50% tests passed, 13 tests failed out of 26`. E-1/E-2/E-6/E-7/E-8 and SI-1/SI-2/SI-3/SI-4/SI-6/SI-7/SI-8 all record `0.00 sec`.
- fix: Replace every stub with the specified hard assertions and performance protocols, ensuring failures or missing measurements remain nonzero. Record the actual E-1/E-2/E-6/E-7 results.
- why: All M1 engine and SI exit gates are unmeasured and unmet.

### [P0] M-12: Storm/look gates and the vertical demo are absent
- location: tests/testUsdGenStormLook.cpp:1; tests/perf/benchUsdGenStorm.cpp:1; libs/usdGenImaging/usdGenImaging/usdGenShadersDiscoveryPlugin.cpp:85
- issue: The EGL binaries are stubs; no static/deform/tilesweep run, batching/VBO counters, HGI-resource compilation, 720p/1080p measurements, bake parity, golden, or canonical example exists. Registered Sdr nodes also lose the glslfx `primvars` metadata even though direct asset parsing finds it.
- evidence: Prescribed `ctest -L T2 --output-on-failure` reports `0% tests passed, 2 tests failed` with `M1-STUB`. `find tests -maxdepth 3 -type f` finds only `scene_empty_groom.usda` and no golden directory/example. `rg "drawBatches|vboRelocated|tilesweep|HDST_ENABLE_HGI_RESOURCE_GENERATION" --glob '!plan/**' --glob '!docs/**'` returns nothing. Manual registered-node lookup reports 20 inputs but `primvars=[]`; direct `GetShaderNodeFromAsset` reports `hairId|hairT|st`, and additionally `hairTangent` for Variant B.
- fix: Carry the EGL harness into real T2 tests; implement all modes and counters, both resolutions, HGI generation checks for both material tags, CPU/GPU parity, approved goldens, and example (a). Make registered shader discovery preserve glslfx primvar metadata.
- why: S-1/S-5/S-6/S-8/S-9/S-12, L-1/L-2, and the vertical demo are unmeasured and unmet.

### [P1] M-13: M1 plugin search path is malformed
- location: CMakeLists.txt:394
- issue: `_usdgen_m1_pluginpath` is a CMake semicolon list embedded in the `ENVIRONMENT` property, so each resource path becomes a separate environment entry instead of one search path.
- evidence: `ctest --test-dir build -N -V` prints `PXR_PLUGINPATH_NAME=.../usdGenSchema/resources`, followed by standalone lines for Imaging, Shaders, and OpenUSD plugin directories.
- fix: Construct the plugin path with `string(JOIN ":" ...)` and assign that single colon-delimited value to `PXR_PLUGINPATH_NAME`.
- why: Real adapter, shader, and population tests may fail discovery or accidentally pass using only the first resource directory.

## Verified OK

- The older build’s M0/build surface remains green: link/include rules, third-party export checks, chain order, plugin discovery, install-tree, and consumer tests all passed.
- Static layout declarations use 512-curve default chunks and planar per-CV `px/py/pz/width/hairT` with per-curve arrays in `curveBuffer.h:54-82`.
- The standalone arithmetic helpers implement the frozen formula and yield 100,000 curves → 196 chunks → four chunks per tile → 49 tiles (`types.h:89-135`); M-10 covers the faulty partition use.
- A private `tbb::task_arena` and arena-contained `parallel_for` wrapper exist (`scheduler.cpp:51-81`), although the scheduler run itself is absent.
- `UsdGenGenerationStore::Publish/Get` use atomic shared-pointer store/load (`generationStore.cpp:18-30`).
- PlugInfo has the required five prim-type registrations and one `UsdGenRestAPI` registration.
- `python3 usdGenShaders/test/checkC5.py` passes: all three files have the identical frozen 20-input block and match `docs/freezes/C5.md`.
- Direct Sdr asset parsing succeeds for all three glslfx files with 20 inputs and the expected variant-specific primvars.
- A manual shipped-schema probe reports `HasAPI("UsdGenMaskAPI") == True`, 22 mask properties, and successful explicit `ApplyAPI`; the formal SI-8 test remains a stub.
- GPU utilization was 0% (`measurement host, 0, 0`). No valid GPU milliseconds were produced, so no contention-qualified timing is reportable.

## Summary

P0: 11, P1: 2, P2: 0; M1 engine/imaging/tests: FAIL