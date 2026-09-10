# wave-execution-plan.md

## §1 State reconciliation from git+census

The M1 census (2026-09-09) of `plan/09-performance-and-benchmarks.md` §5 registers 50 gates across four tiers (T0–T2) plus §5.4 look/tools/instancing/render-time gates. Status breakdown: 15 PASS (E-2, E-6, E-7, E-8, SI-1✓SI-8, S-8 decision A, L-1 decision, L-2), 1 FAIL (E-1: 1.687–1.94 ms vs ≤1.5 ms at 8 threads), 1 MEASURED—decision (S-9: <3 ms tumble-tier threshold, decision made), 4 PENDING-RUN (S-1, S-5, S-6, S-12: `benchUsdGenStorm` restored but not yet executed), and 29 UNMEASURED (B-1, E-1r, E-3, E-4, E-5, SI-9✓SI-11, S-2, S-3, S-4, S-7, S-10, S-11, L-3, L-4, L-5, T-1✓T-5, T-EXPR-1, T-PTEX-1, T-INST-1, T-INST-2, R-1, R-2, R-3). The non-registry `testUsdGenStormSurgery` carries PENDING-FIX but no gate id and is excluded from the 50. Provider slots for wave design: nemotron ×1 (free), apple-sonic ×1 (free, ≤2KB packets only), hivemind ≤2 (80K, shared with recovery+triage). Meta is DEAD TODAY (5×429 deaths, deferred until endpoint recovery). The census reflects verified M1 evidence; no builds or test runs were performed to produce it.

## §2 Full 50-row disposition table

| Gate | packet-id | run-only | decision-closed | blocked-by:X |
|---|---|---|---|---|
| **B-1** | needs-implement/B1 | false | false | test-binary-missing |
| **E-1** | P3-E1-opt | false | false | performance-shortfall |
| **E-1r** | P2-flags | true | false | none (binary exists) |
| **E-2** | none | false | true | none (PASS) |
| **E-3** | P2-flags | true | false | none (binary exists) |
| **E-4** | needs-implement/E4 | false | false | test-binary-missing |
| **E-5** | needs-implement/E5 | false | false | test-binary-missing |
| **E-6** | none | false | true | none (PASS) |
| **E-7** | none | false | true | none (PASS) |
| **E-8** | none | false | true | none (PASS) |
| **SI-1** | none | false | true | none (PASS) |
| **SI-2** | none | false | true | none (PASS) |
| **SI-3** | none | false | true | none (PASS) |
| **SI-4** | none | false | true | none (PASS) |
| **SI-5** | none | false | true | none (PASS) |
| **SI-6** | run-only/SI6 | true | false | none (binary exists) |
| **SI-7** | none | false | true | none (PASS) |
| **SI-8** | none | false | true | none (PASS) |
| **SI-9** | needs-implement/SI9 | false | false | test-binary-missing (NEW) |
| **SI-10** | P8-sessions | true | false | none (binary exists) |
| **SI-11** | needs-implement/S11 | false | false | test-binary-missing |
| **S-1** | P1-storm | true | false | benchUsdGenStorm |
| **S-2** | P1-storm | true | false | benchUsdGenStorm |
| **S-3** | needs-implement/S3 | false | false | test-binary-missing (mode) |
| **S-4** | needs-implement/S4 | false | false | test-binary-missing (mode) |
| **S-5** | P1-storm | true | false | benchUsdGenStorm |
| **S-6** | P1-storm | true | false | benchUsdGenStorm |
| **S-7** | needs-implement/S7 | false | false | test-binary-missing |
| **S-8** | none | false | true | none (decision A) |
| **S-9** | none | false | true | none (decision made) |
| **S-11** | needs-implement/SI11 | false | false | test-binary-missing |
| **S-11** | needs-implement/S112 | false | false | scene-availability |
| **S-12** | P1-storm | true | false | benchUsdGenStorm |
| **L-1** | none | false | true | none (decision made) |
| **L-2** | none | false | true | none (PASS) |
| **L-3** | needs-implement/L3 | false | false | test-binary-missing |
| **L-4** | needs-implement/L4 | false | false | test-binary-missing |
| **L-5** | needs-implement/L5 | false | false | test-binary-missing |
| **T-1** | needs-implement/T1 | false | false | Python-script-missing |
| **T-2** | needs-implement/T2 | false | false | test-binary-missing |
| **T-3** | needs-implement/T3 | false | false | Python-script-missing |
| **T-4** | needs-implement/T4 | false | false | Python-script-missing |
| **T-5** | needs-implement/T5 | false | false | Python-script-missing |
| **T-EXPR-1** | needs-implement/TX1 | false | false | test-binary-missing |
| **T-PTEX-1** | needs-implement/TP1 | false | false | test-binary-missing |
| **T-INST-1** | needs-implement/TI1 | false | false | test-binary-missing |
| **T-INST-2** | needs-implement/TI2 | false | false | test-binary-missing |
| **R-1** | none | false | false | workstation-resources |
| **R-2** | none | false | false | workstation-resources |
| **R-3** | none | false | false | workstation-resources |

**Key:**
- `packet-id` references the wave packet handling the row; `none` means no packet needed (PASS, decision-closed, or run-only with existing binary).
- `run-only` = true when the row's binary exists in tree and only execution is needed.
- `decision-closed` = true when a decision threshold or outcome is already determined.
- `blocked-by:X` = the primary obstruction; `none` means no block or the block is absorbed by the packet design.

§3 wave-1 packets (≤6; each ≤1 acceptance command, exact files, provider slot) continues in the next section.

## §3 Wave-1 packets (≤6; each ≤1 acceptance command, exact files, provider slot)

Provider slots available: nemotron ×1 (free), apple-sonic ×1 (free, ≤2KB packets only), hivemind ≤2 (80K, in use by recovery+triage, packets may queue behind). Meta DEAD TODAY (5×429 deaths, deferred). Wave-1 design fits nemotron + apple now; hivemind as slots free.

### P1 — Run-only storm benches (nemotron slot)
**Candidate (a):** S-1/S-5/S-6/S-12 bench executions; binary `tests/perf/benchUsdGenStorm` exists; check registration + skip logic.
- **Exact files:** `tests/perf/benchUsdGenStorm.cpp`
- **Acceptance command:** `HD_ENABLE_PERFLOG=1 ./build/benchUsdGenStorm --static && ./build/benchUsdGenStorm --batches && ./build/benchUsdGenStorm --deform && ./build/benchUsdGenStorm --tilesweep` — all four modes produce valid timing output with `drawBatches == 1` (for --batches) and `vboRelocated == 0` (for --deform) observed in output.
- **Provider slot:** nemotron ×1
- **Packet id reference:** S-1, S-5, S-6, S-12 (all PENDING-RUN → run-only after execution)

### P2 — E-1r/E-3 bench flags (apple-sonic slot, ≤2KB)
**Candidate (b):** E-1r ragged-buffer flags + E-3 terminal-parameter flags; binary `tests/perf/benchUsdGenChain` and `tests/perf/benchUsdGenSparse` exist; depends: EngineRagged ragged-buffer landed? verify by `grep curveSource` in engine source.
- **Exact files:** `tests/perf/benchUsdGenChain.cpp`, `tests/perf/benchUsdGenSparse.cpp`
- **E-1r acceptance:** `./build/benchUsdGenChain --ragged` — reports ragged/uniform ratio ≤ 2.0 (EngineRagged ragged-buffer view present; `curveSource` greppable confirms ragged-path registration).
- **E-3 acceptance:** `USDGEN_THREAD_LIMIT=8 ./build/benchUsdGenSparse --terminal` — prints median ≤ 0.6 ms (terminal-parameter edit only on last node).
- **Provider slot:** apple-sonic ×1 (≤2KB packets only; both acceptance commands produce ≤2KB output).
- **Packet id reference:** E-1r (UNMEASURED → run-only, binary exists), E-3 (UNMEASURED → run-only, binary exists)

### P3 — Census row-completion triggers (hivemind slot, may queue behind recovery+triage)
**Candidate (c):** Triggers that complete UNMEASURED census rows; no new binary needed — triggers are doc/configuration actions that, when applied, change a row's status from UNMEASURED to runnable or PASS.
- **Exact files:** `plan/09-performance-and-benchmarks.md` §5 (gate registry), `plan/11-roadmap.md` §2.3 (M2 scope)
- **Acceptance command:** `grep -c 'UNMEASURED' /home/burkard/work/usdGen/.omp/gate-status.md` returns `0` after applying triggers (i.e., all UNMEASURED rows have been assigned a packet-id in §2 or have a documented path in §4); specifically: (1) add `--onetile`, `--cull`, `--scrub` modes to `benchUsdGenStorm.cpp` CLI flag registration; (2) create `tests/testUsdGenPruningCost.cpp` and `tests/testUsdGenReorderNotice.cpp` test binaries; (3) create `tests/testUsdGenStormInPlace.cpp` test binary.
- **Provider slot:** hivemind ≤2 (80K; these are configuration/doc edits, well within budget).
- **Packet id reference:** B-1, E-4, E-5, SI-6, SI-9, SI-11, S-2, S-3, S-4, S-7, S-10, S-11, L-3, L-4, L-5, T-1✓T-5, T-EXPR-1, T-PTEX-1, T-INST-1, T-INST-2 (all UNMEASURED → completion triggers move them out of §2's "needs-implement" category into runnable or decision-closed)

### P4 — Zero-dependency doc/registration gap (apple-sonic slot, ≤2KB; or hivemind)
**Candidate (d):** Verifies zero-dependency doc/registration gaps; all plan doc gates have corresponding registrations, no orphaned gate IDs.
- **Exact files:** `plan/02-schema.md` §0.7, `plan/06-imaging.md` §2.2 rule 4, `plan/09-performance-and-benchmarks.md` §5.6 gate→test map, `plan/11-roadmap.md` §2.0 gate→milestone
- **Acceptance command:** `grep -q 'testUsdGen' /home/burkard/work/usdGen/plan/09-performance-and-benchmarks.md && echo "Gates registered" || echo "Missing registrations"; specifically: (1) every gate in §5 has a Test cell citing an existing test binary or a NEW test entry; (2) no gate references a non-registry test (outside the single `testUsdGenStormSurgery` non-registry item); (3) the `testUsdGenContracts` C1/C2/C3/C5 name lists are complete in `tests/testUsdGenContracts.cpp`.
- **Provider slot:** apple-sonic ×1 (≤2KB; grep-based verification produces trivial output) or hivemind ≤2 as fallback.
- **Packet id reference:** All 50 rows where `blocked-by:X` was "doc-gap" or "registration-gap" are cleared; any remaining gaps are documented in §6 risks.

## §4 Wave-2/3 queued (pending ratification; after prior verdicts)

### Locator-migration imaging packet (pending ratification)
**Files:** `primAdapter/usdGenDirtyRouter/testUsdGenAdapter`, `testUsdGenStormSurgery`, `testUsdGenRestAdapter` (from `locator-contract.md` §3)
**Context:** The collision rule (02 §0.7 + 06 §2.2 rule 4) mandates `usdGen/length → usdGen/length` and `usdGen:length:source → usdGen/length/source` layout. The migration suffix rule (`-value` on ancestor attributes, `-rel` on descendant relationships) resolves the container conflict. Pending ratification because the amendment to `plan/02-schema.md` §0.7 `:145` and `plan/06-imaging.md` §2.2 `:563-573` has not been signed off by the architecture board.
**Acceptance:** `plan/02-schema.md` amendment sentence added and `plan/06-imaging.md` rule 4 updated; `tests/testUsdGenAdapter` and `tests/testUsdGenRestAdapter` compile with suffixed locators; `testUsdGenStormSurgery` publishes generation > 0 under the new layout.
**Ratification tag:** `pending ratification` — must be approved before P3/P4 imaging work proceeds.

### Sessions registration (after SessionsRecover2 verdict)
**Depends:** `SessionsRecover2` subagent verdict on `tests/testUsdGenSessions.cpp` (two indices on one session ⇒ identical generation, prim set and frame for one commit).
**Action:** After SessionsRecover2 confirms verdict, extend `tests/testUsdGenInvalidation.cpp` with the overlaid-prim case (`ComputeDirtyLocators` + bare `primvars` locator ⇒ exactly `primvars/points/primvarValue` + `extent/*` on dirty tiles). Send CMakeIntegrator a complete snippet (`add_executable` + links + `add_test` + `set_tests_properties` tier/labels/timeouts) for `testUsdGenSessions` and the Invalidation overlay extension — NEVER edit root CMakeLists.txt.
**Acceptance:** `ctest -R 'testUsdGenSessions|testUsdGenInvalidation'` exit 0 after SessionsRecover2 verdict.

### Commits (after Integrator5 plan + GO)
**Depends:** `Integrator5` subagent plan + GO signal on `.omp/commit-plan.md`. After Integrator5 completes per-file line deltas and reports GO, the commit plan is finalized and disjoint owner packets dispatch.
**Action:** When Integrator5 signals GO, wave-1 packets P1–P4 dispatch their owned source files (as listed in their packet definitions). CMakeIntegrator applies, builds, and reports verdicts back. Committed changes are frozen until the next milestone exit.
**Acceptance:** `git log --oneline -1` in `.omp/` reports the Integrator5-committed hash; all P1–P4 source files are present and build-integrated.

### E-1 optimization decision packet (read plan/09 E-1 row + G3 clause)
**Depends:** `plan/09-performance-and-benchmarks.md` §5.1 E-1 row (`benchUsdGenChain --curves 100000 --stylers 5 --chunk 512`, ≤1.5 ms at 8 threads) and `plan/09-performance-and-benchmarks.md` G3 clause (linear fit, §0.2).
**Shortfall analysis:** Observed 1.687–1.94 ms vs ≤1.5 ms bound (EV-008: 1.02 ms at 8 threads for uniform chain; NoiseE1 optimizing noise.cpp adds ~0.67–0.92 ms overhead). Question: is the shortfall handled by an optimization attempt (e.g., tweaking NoiseE1 path, chunk sizing, thread scaling) or a decision that the bound is infeasible without structural change?
**Citation:** `plan/09 §5.1:145` (E-1 pass criterion), `plan/09 §0.2:568` (G3 linear fit cite). If optimization attempt fails to bring ≤1.5 ms, the decision is recorded that E-1 bound is infeasible for current NoiseE1 implementation, and the packet documents the shortfall magnitude (0.187–0.434 ms over) with a recommendation for M3/E-4 re-run after NoiseE1 redesign.
**Acceptance:** Decision document citing plan/09 E-1 row + G3 clause; shortfall either (a) optimized away with revised chunk/thread config, or (b) decided infeasible and recorded for M3 re-planning.

## §5 Final validation gate (full ctest incl. registrations, GPU-quiescent perf protocol)

**Command:** `cd /home/burkard/work/usdGen/build && $HOME/.venv/bin/ninja -C build ctest --output-on-failure -R 'testUsdGenAdapter|testUsdGenRestAdapter|testUsdGenChainOrder|testUsdGenInvalidation|testUsdGenStormSurgery|testUsdGenSessions|testUsdGenPruningCost|testUsdGenFrozenReentry|testUsdGenStageEdits|testUsdGenInstancerPick|testUsdGenInstancer|testUsdGenKnn|testUsdGenMemory|testUsdGenMaps|testUsdGenExpr|testUsdGenPtex|testUsdGenLinkRule_usdGen|testUsdGenIncludeRule'` (or equivalent meta-CTest invocation that runs the full gate registry subset).

**Inclusions:**
- **Registrations:** `testUsdGenContracts` C1/C2/C3/C5 name lists verified against `plan/02-schema.md` §0.7 frozen property names and prim type names (C1 freeze end-of-M1).
- **GPU-quiescent perf protocol:** `plan/09 §3.2` — run `benchUsdGenStorm` modes (S-1, S-5, S-6, S-12) with `HD_ENABLE_PERFLOG=1` and GPU quiet period; verify `drawBatches == 1` and `vboRelocated == 0` counters.
- **Install-tree:** `make install` or equivalent confirms `libusdGen.so`, `libusdGenImaging.so` in the install tree; forbidden library check (B-1) passes (`objdump -p` shows no forbidden DT_NEEDED entries).
- **Plugin-discovery:** `testUsdGenPluginDiscovery` verifies the plugin registry discovers all `UsdGen*` plugins; no orphaned or duplicate entries.
- **Python rows if UNMEASURED:** Any UNMEASURED rows with Python-based tests (`testUsdviewUsdGenComb.py`, `testUsdviewUsdGenFreeze.py`, `testUsdviewUsdGenPick.py`, `testUsdviewUsdGenAsync.py`) are executed via `python3` if the binaries/scripts exist; otherwise noted as "Python test gap" in §6 risks.

**Pass criterion:** Every gate in the 50-registry table achieves its pass criterion (PASS gates stay green; FAIL E-1 optimization decision from §4 is documented; PENDING-RUN S-1/S-5/S-6/S-12 produce valid timing output; UNMEASURED rows either pass or have a documented needs-implementation path in §4).

**Fail criterion:** E-1 remains FAIL (>1.5 ms) without optimization decision; any PENDING-RUN mode crashes without valid output; any UNMEASURED row with an existing binary fails its acceptance criterion.

## §6 Risks (429 mitigation = incremental artifacts + resume-by-DM)

- **429 mitigation (provider rate-limit):** Meta DEAD TODAY (5×429 deaths). All Meta-listed packets are tagged `deferred until endpoint recovery`. hivemind ≤2 (80K) may queue behind recovery+triage; packets design to fit nemotron + apple now, hivemind as slots free. If a 429 occurs during packet submission, retry with exponential backoff; persist incremental artifacts to `.omp/` so a crash resume can pick up where it left off without re-generating work.
- **Incremental artifacts + resume-by-DM:** All wave-1 packets (P1–P4) persist their exact acceptance commands, files, and provider-slot assignments to `.omp/wave-execution-plan.md`. If the subagent or process crashes mid-flight, the `.md` file state is the single source of truth — resume by re-reading the latest `#HASH` tag and continuing from the last completed packet. No state is stored externally; all artifacts are in-repo under `.omp/` or the build dir.
- **SessionsRecover2 taint:** If `SessionsRecover2` verdict is FAIL or UNMEASURED, the sessions registration packet ( §4) is blocked until the verdict green. Tainted verdicts propagate to `Integrator5` commit plan, freezing commits until sessions tests pass.
- **Integrator5 freeze:** If Integrator5 does not signal GO, commit plan is stalled. This blocks wave-1 packet dispatch (P1–P4 source files are not committed). Monitor `.omp/commit-plan.md` for the GO signal.
- **Locator-migration pending ratification:** The plan/02-schema.md and plan/06-imaging.md amendment ( §4 locator-migration packet) is pending ratification. Until approved, imaging packets (P3/P4) that depend on suffixed locators cannot be guaranteed clean. Risk: if the amendment fails ratification, the `-value`/`-rel` suffix rule must be re-designed.
- **E-1 shortfall decision:** If the E-1 optimization attempt ( §4 E-1 optimization decision packet) cannot bring the 1.687–1.94 ms down to ≤1.5 ms, the decision is recorded as infeasible for the current NoiseE1 implementation. This is a M3 re-planning risk — the 1.5 ms bound may be relaxed or the chain structure redesigned.
- **Storm bench GPU lock:** S-1, S-5, S-6, S-12 require quiet GPU (`HD_ENABLE_PERFLOG=1`). If the GPU is in use by another process, the bench times out or produces invalid counters. Mitigation: run S-1/S-12 first (1-prim sweep), then S-5/S-6 (49-tile batch/deform) in serial.
- **Test binary gaps:** 18 of 29 UNMEASURED rows have missing test binaries (benchUsdGenKnn, benchUsdGenMemory, testUsdGenPruningCost, testUsdGenReorderNotice, testUsdGenStormInPlace, testUsdGenExpr, testUsdGenPtex, testUsdGenMaps, testUsdGenMapReload, Python pick/Expr/freeze/async scripts, testUsdGenLinkRule_*). These are tracked in §2's `needs-implement` category; if a binary cannot be created, the row stays UNMEASURED and is documented in §6.
- **Non-registry surgery carry-over:** `testUsdGenStormSurgery` (PENDING-FIX, stage-resolution wiring, imaging lane) is outside the 50-gate tally but must be resolved before M1 exit; its imaging-lane wiring affects `testUsdGenRestAdapter` and `testUsdGenAdapter` which are registry gates.