# M2 worker packets — Deform and freeze (dispatchable)

Source: `.omp/wave7-plan.md` M2 § + packet sketches, cross-checked against
`plan/11-roadmap.md` §2.3 (M2 scope/exit gates) and `plan/09-performance-and-benchmarks.md`
§5.1–§5.3 (registry — owns every threshold). **Registry wins on conflict.**
Ownership: root CMakeLists.txt = CMakeIntegrator (serialized integrator lane). No packet edits it; new-test packets (P3/P4/P6) send CMakeIntegrator a complete snippet when sources EXIST on disk (add_executable + links + add_test + set_tests_properties tier/labels/timeouts); CMakeIntegrator applies, builds, tests, and reports verdicts back.

## 1. M2 gate table

| Gate | Exact pass criterion (09 §5 quote / 11 §2.3) | Test file | Tier / milestone |
|---|---|---|---|
| E-1r | "the same chain on a ragged buffer (`cvCount == 0` + `cvOffsets`): ≤ 2× the uniform path" (09 §5.1) | `tests/perf/benchUsdGenChain.cpp` `--ragged` | T0 / M2 (UNMEASURED) |
| E-3 | "terminal-parameter edit only: ≤ 0.6 ms" (09 §5.1; MEASURED-pass EV-004 0.179–0.411 ms @20thr, 8thr UNMEASURED) | `tests/perf/benchUsdGenSparse.cpp` `--terminal` | T0 / M2 |
| SI-2 re-run | "exactly `primvars/points/primvarValue` + `extent/*` on the dirty tiles, nothing else" on the **overlaid-prim case** (`ComputeDirtyLocators` **and** the bare `primvars` locator) (09 §5.2; 11 §2.3) | `tests/testUsdGenInvalidation.cpp` (extend) | T1 / M1, re-run M2 (not an exit) |
| SI-9 | "the per-deforming-frame wrapper cost is recorded and stays inside class A's ≤ 1.0 ms usdGen share (§0.1); red ⇒ SC-13" on a production-density skinned scalp (09 §5.2) | NEW `tests/testUsdGenPruningCost.cpp` (gate); `tests/testUsdGenSkelInterop.cpp` companion (resolves `skinnedPoints` with `HD_ENABLE_SCENE_INDEX_EMULATION` off) | T1 / M2 (UNMEASURED) |
| SI-10 | "two scene-index instances attached to one session: identical generation, prim set and frame for one commit" (09 §5.2; RK-09) | NEW `tests/testUsdGenSessions.cpp` over `usdGenImagingSession.{h,cpp}` (exists) | T1 / M2 (UNMEASURED) |
| S-2 | "deform frame: scene-index `points` publish, 49 tiles: ≤ 2.0 ms delta over static" (09 §5.3; ≈1.44 ms DERIVED from EV-022) | `tests/perf/benchUsdGenStorm.cpp` `--deform` | T2 / M2 (UNMEASURED) |
| S-3 | "one-tile edit (1 of 49 dirty): ≤ 1.2× static" (09 §5.3) | `tests/perf/benchUsdGenStorm.cpp` `--onetile` (NEW mode) | T2 / M2 (UNMEASURED) |
| S-4 | "culling: camera frames ¼ of G4: `itemsDrawn` drops ≥ 3× at 32 and 196 tiles, not at all at 1 prim" (09 §5.3; 11 §2.3) | `tests/perf/benchUsdGenStorm.cpp` `--cull` (NEW mode) + G4 fixture (09 §4.4) | T2 / M2 (UNMEASURED) |
| (non-gate freeze) | Frozen output re-enters bit-identical (`testUsdGenFrozenReentry`); `RemovePrim` containment (`testUsdGenStageEdits`); `SubtreeSnapshot` + `SetActive(false)` rule (`test_usdgen_undo.py`, `test_usdgen_freeze_author.py`); freeze ≤ 1 ms + sidecar reopen ≤ 1 ms (EV-045/EV-047) | NEW `libs/usdGen/usdGen/ops/deform.cpp`, `ops/curveSource.cpp`, `ops/freeze.cpp`; `python/usdgen/` scripts (dir currently EMPTY) | T0/T1 / M2, freezes C3 |

## 2. Packets (6)

Shared build/test commands (all packets): `ninja=/home/burkard/.venv/bin/ninja`,
shared build dir `build/` (NOT `build-engine/`/`build-imaging/`/`build-schema/`).
GPU packets (P5) are `RUN_SERIAL` + require quiet GPU; T0/T1 packets MUST NOT
take the GPU lock and run concurrently with other T0/T1 packets. S-2/S-3/S-4
assert wall-clock deltas: run each `--repeats 3`, discard cold repeat, median of medians (09 §4.5).

### P1 — EngineRagged (E-1r)
- **Target files (owned, disjoint):** `libs/usdGen/usdGen/curveBuffer.h` (ragged `cvCount==0`+`cvOffsets` view), NEW `libs/usdGen/usdGen/ops/curveSource.cpp` (+ header), NEW `libs/usdGen/usdGen/ops/deform.cpp` (+ header). Does NOT touch `tests/perf/benchUsdGenChain.cpp` (NoiseE1 active) — the `--ragged` CLI flag is added by **CMakeIntegrator** serially after this packet + NoiseE1 land (send nothing; CMakeIntegrator owns that file).
- **Change steps:** (1) add ragged buffer view to `curveBuffer.h` following the SoA planar layout (I1); (2) implement `UsdGenCurveSource` kernel producing ragged buffers; (3) implement `UsdGenDeform` kernel (deformed-tail evaluate, rest-points at `Default()` + authored `primvars:rest` honoured); (4) expose both via `opRegistry`.
- **Acceptance:** single observable criterion — `benchUsdGenChain --ragged` (CMakeIntegrator-added flag) reports ragged/uniform ratio ≤ 2.0. Commands: `$HOME/.venv/bin/ninja -C build benchUsdGenChain && ./build/benchUsdGenChain --ragged`.
- **Agent:** `task` (large-context engine work) — NOT task_remote/qwen. **Initial-context budget:** 12K.

### P2 — EngineSparseTerminal (E-3)
- **Target files:** `tests/perf/benchUsdGenSparse.cpp` (add `--terminal` mode: dirty terminal node only, 9 runs @8 threads, median). No engine source changes expected; if the budget fails, report numbers only — do not retune the engine in this packet.
- **Change steps:** (1) add `--terminal` argv path beside the existing E-2 1%-sparse path; (2) pin 8 threads (`USDGEN_THREAD_LIMIT=8`), median ≤ 0.6 ms FAIL only when `USDGEN_GATE=1` (benchUsdGenChain.cpp convention).
- **Acceptance:** `./build/benchUsdGenSparse --terminal` prints median ≤ 0.6 ms. Commands: `$HOME/.venv/bin/ninja -C build benchUsdGenSparse && USDGEN_THREAD_LIMIT=8 ./build/benchUsdGenSparse --terminal`.
- **Agent:** `nemotron` (small self-contained). **Budget:** 5K.

### P3 — ImagingPrune (SI-9)
- **Target files:** NEW `libs/usdGenImaging/usdGenImaging/pruningWrapper.{h,cpp}` (private `HdSiExtComputationPrimvarPruningSceneIndex` wrapper), NEW `tests/testUsdGenPruningCost.cpp`, NEW `tests/testUsdGenSkelInterop.cpp`. Must NOT touch `groomSceneIndexPlugin.cpp`, `primAdapter.cpp`, `testHook.*` (imaging lane fence).
- **Change steps:** (1) implement the pruning wrapper per `05-static-curves-and-deformation.md` §4.2; (2) `testUsdGenSkelInterop` first: resolves `skinnedPoints` with `HD_ENABLE_SCENE_INDEX_EMULATION` off on `chain-order/stages/skelAndRig.usda` (verify presence at kickoff — carry-in); (3) `testUsdGenPruningCost`: per-deforming-frame wrapper cost on production-density skinned scalp, recorded vs class-A ≤ 1.0 ms share; red ⇒ report SC-13, do not redesign.
- **Acceptance:** `ctest -R 'testUsdGenPruningCost|testUsdGenSkelInterop'` exit 0 with cost ≤ 1.0 ms recorded. Commands: `$HOME/.venv/bin/ninja -C build testUsdGenPruningCost testUsdGenSkelInterop && ctest --test-dir build -R 'testUsdGenPruningCost|testUsdGenSkelInterop'`.
- **Agent:** `lean-context-task-runner`/`hivemind` (focused ≤80K). **Budget:** 8K.

### P4 — ImagingSessions + SI-2 overlay re-run (SI-10)
- **Target files:** NEW `tests/testUsdGenSessions.cpp`; extend `tests/testUsdGenInvalidation.cpp` with the overlaid-prim case (bare-`primvars` locator). Read-only on `libs/usdGenImaging/usdGenImaging/usdGenImagingSession.{h,cpp}` and `libs/usdGen/session.cpp`/`scheduler.cpp` (engine lane — NO edits).
- **Change steps:** (1) `testUsdGenSessions`: two indices on one session, one commit ⇒ identical generation + prim set + frame; (2) SI-2 overlay case: `ComputeDirtyLocators` + bare `primvars` locator ⇒ exactly `primvars/points/primvarValue` + `extent/*` on dirty tiles; (3) when source files EXIST on disk, send CMakeIntegrator a complete snippet (`add_executable` + links + `add_test` + `set_tests_properties` tier/labels/timeouts) — NEVER edit root CMakeLists.txt; already-registered `testUsdGenInvalidation` extension needs nothing from it.
- **Acceptance:** `ctest -R 'testUsdGenSessions|testUsdGenInvalidation'` exit 0. Commands: `$HOME/.venv/bin/ninja -C build testUsdGenSessions testUsdGenInvalidation && ctest --test-dir build -R 'testUsdGenSessions|testUsdGenInvalidation'`.
- **Agent:** `nemotron` (small self-contained). **Budget:** 6K.

### P5 — StormBenchModes (S-2, S-3, S-4 bench side)
- **Target files:** `tests/perf/benchUsdGenStorm.cpp` ONLY (CLI + modes). NOTE tree finding: `--deform` currently exists but is labeled `gate="S-6"` (vboRelocated counters) — this packet re-labels/extends it to also assert the S-2 ≤ 2.0 ms delta; `--onetile` and `--cull` are NEW modes. Fixture scenes come from P6; serialize on P6 landing (bench asserts need G3/G4 scenes present).
- **Change steps:** (1) `--deform`: static vs deform median delta ≤ 2.0 ms @100k×8CV 720p 49 tiles (+ keep S-6 `vboRelocated` counter read); (2) `--onetile`: 1-of-49 dirty ≤ 1.2× static; (3) `--cull`: camera frames ¼ of G4 ⇒ `itemsDrawn` ≥ 3× drop at 32/196 tiles, flat at 1 prim (`HD_ENABLE_PERFLOG=1` counters, JSON null never 0).
- **Acceptance:** all three modes pass on G3/G4 scenes. Commands (GPU-serial, quiet GPU): `$HOME/.venv/bin/ninja -C build benchUsdGenStorm && HD_ENABLE_PERFLOG=1 ./build/benchUsdGenStorm --scene bench/head100k.usda --deform --res 1280x720 --refine 2 --frames 60 --warmup 10 --repeats 3 --json out/S2.json` (analogous `--onetile`, `--cull`).
- **Agent:** `lean-context-task-runner`/`hivemind` (focused bench driver). **Budget:** 8K.

### P6 — FixturesFreeze (G2/G4 variants + freeze/undo + C3)
- **Target files:** `tests/perf/gen_hair_stages.py` (G4 200k×8 + G3/G4 1-prim/32-tile/196-tile variants per 09 §4.4), `tests/scenes/` additions, NEW `libs/usdGen/usdGen/ops/freeze.cpp` (+ header), NEW `python/usdgen/usdGenUndo.py` (`SubtreeSnapshot`) + freeze-author script, NEW `tests/testUsdGenFrozenReentry.cpp` + `tests/testUsdGenStageEdits.cpp`, `tests/python/test_usdgen_undo.py` + `test_usdgen_freeze_author.py`. `tileTarget` plumbing is DONE (`graphDesc.h:143`, `types.h`, `compiler.cpp`, dirty-router) — no plumbing work, just use `tileTarget=32/256` to emit variants.
- **Change steps:** (1) extend generator for G4 + prim-count variants (identical curve data across variants); (2) `UsdGenFreeze` kernel (`frozen|live`, advisory tier) + `UsdGenCurveAPI` role; (3) freeze/undo scripts + T1 re-entry (bit match) + `RemovePrim` containment tests; (4) C3 name list lands in `testUsdGenContracts` — send the complete snippet to CMakeIntegrator only if targets are missing; NEVER edit root CMakeLists.txt.
- **Acceptance:** G4 + variants exist AND `ctest -R 'testUsdGenFrozenReentry|testUsdGenStageEdits'` exit 0 (bit-identical re-entry). Commands: `python3 tests/perf/gen_hair_stages.py --out bench && $HOME/.venv/bin/ninja -C build testUsdGenFrozenReentry testUsdGenStageEdits && ctest --test-dir build -R 'testUsdGenFrozenReentry|testUsdGenStageEdits'`.
- **Agent:** `task` (large-context: fixtures + kernel + python). **Budget:** 12K.

## 3. M2 prerequisites checklist (who owns what)

| Prerequisite | Owner | State |
|---|---|---|
| G2/G4 fixture variants (1-prim / 32 / 196 tiles, identical curve data) | P6 | TODO (generator exists, variants new) |
| `tileTarget` plumbing (`tileTarget=64` default, clamp 32–256) | — | DONE in tree (`graphDesc.h`, `types.h`, `compiler.cpp`, dirty-router) |
| `bench --deform` mode (S-2 delta; currently S-6-labeled) | P5 | EXTEND |
| `bench --onetile` mode (S-3) | P5 | TODO (new) |
| `bench --cull` mode (S-4) | P5 (bench) + P6 (G4 scenes) | TODO (new) |
| `--ragged` flag on `benchUsdGenChain` | CMakeIntegrator (serial after P1 + NoiseE1) | TODO — P1 must NOT touch the file |
| `--terminal` flag on `benchUsdGenSparse` | P2 | TODO |
| `skelAndRig.usda` interop fixture presence | P3 verifies at kickoff | VERIFY |
| C3 freeze (`testUsdGenContracts` gains C3 names) | P6 drafts snippet for CMakeIntegrator if targets missing | TODO |

## 4. M3–M8 previews (unchanged from wave7-plan)

- **M3 — Guides and clumps (5 wk / 13 eng-wk):** E-4 (kNN ≤ 25 ms @100k + linear, else SC-6), E-5 (RSS ≤ 800 MB @1M, at-risk DERIVED 950–1050 MB ⇒ SC-7a/7b), E-1 re-runs (7-node + head1M, record-only at 1M). Kernels: `GuideInterpolate` + multi-level `Clump`.
- **M4 — Maps and expressions (4 wk / 11 eng-wk):** L-3 (bitwise @1 vs 8 threads), L-4 (capture ≤ 200 ms + zero Ptex reopens), L-5 (exact `ReloadMaps` dirty set), T-EXPR-1 (sandbox fallback), T-PTEX-1 (ids match `Far::PtexIndices`). Kernel: `Scale`.
- **M5 — Tools (5 wk / 13 eng-wk, freezes C4):** T-1 (≤ 1 ms/brush move), T-2 (PickCV ≤ 0.25/2.5 ms), T-3 (pick occlusion-only disagreement), T-4 (one freeze, one `_resetGUI`), S-7 (`--scrub` ≤ 1.5×), C4 (19-entry ABI).
- **M6 — Instancing (3 wk / 7 eng-wk, droppable):** T-INST-1 (pick resolves through `primOrigin`), T-INST-2 (surface rebase, single `instancedBy` target).
- **M7 — Render time + hardening (5 wk / 11 eng-wk):** S-11 (`head1M` record-only), P2 1+3 evals assertion, `usdrecord` GL, hdPrman contract; full E-*/SI-*/S-* re-run on G1–G4; R-1 built.
- **M8 — Breadth (continuous):** S-10 (in-place overlay), T-5 (async enablement), per-operator T0+T1 per landing.

## 5. Contradictions found (registry wins)

- **C1 — STALE, tree already fixed.** wave7-plan §C1 claims `tests/testUsdGenStormSurgery.cpp:1` claims "gate S-11" and root CMakeLists bakes `gate:S-11`. Live tree: label is `"T2"` with NO gate tag (`CMakeLists.txt:512`), and 10 §5.6 omits the test — the mislabel is already removed. No action; M7 S-11 still needs its `--scene bench/head1M.usda --static` driver.
- **C2 — OPEN (unchanged).** ABI test name: 11 §0.2 names `testUsdGenAbi` (T1); 10 §5.6 registers `testUsdGenAbiSymbols` (T0, nm-based). Reconcile before M5. (No `Abi` target exists in tree today — confirmed by grep.)
- **C5 — CONFIRMED (unchanged).** SI-9 = `testUsdGenPruningCost` gate + `testUsdGenSkelInterop` companion per 09 §5.2; 11 §2.3 leads with the two named tests and never names the cost driver. M2 kickoff treats the cost driver as the gate (this packet set does).
- **NEW W1 — wave7-plan "DOES NOT EXIST" claims are wrong.** `tests/testUsdGenStormTangent.cpp` (5.0 KB) and `tests/testUsdGenStormMaterial.cpp` (12.8 KB) both EXIST in tree; wave7-plan M1-leftovers table says neither exists. M1 S-8-tangent/L-1 state is "exists, unmeasured", not "missing".
- **NEW W2 — bench CLI drift.** wave7-plan sketches assume `--deform` = S-2, `--onetile`/`--cull` = small additions. Tree: `--deform` is labeled `gate="S-6"` (counters path), and `--onetile`/`--cull` do not exist; likewise `--ragged` (benchUsdGenChain) and `--terminal` (benchUsdGenSparse) do not exist yet. P5/P1/P2 above account for this.
- C3 (E-1 2.8–2.9 ms observed vs 1.5 ms registry), C4 (09 §5.5 vs 11 §2.0 phrasing), C6 (L-4 ASSUMPTION vs flat) carried over unchanged from wave7-plan.
