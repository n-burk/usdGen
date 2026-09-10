# COORD-AUDIT-1 (2026-09-10 UTC) — lane-state reconciliation

## (A) Per-lane table

| Lane | Verdict | Evidence |
|---|---|---|
| PlanFinalizer (nemotron, running) | ALIVE-PROGRESSING | Transcript shows deep validation loop (binary-existence greps, CMakeLists + tests/ finds, plan/09 reads, primAdapter.cpp:249 review); hit 24-request soft budget,зЋ"wrap up and yield" notice outstanding. Wave file grew 151→155 lines (§3 P1–P4 + §4/§5/§6 present). BUT `.omp/adjudication-1.md` still NOT on disk (read → not found; glob confirms). Locator verdict ping not yet sent. |
| RaggedP1 (nemotron, running) | DEAD-NO-ARTIFACT | Hub roster: active 5m4s ago. Transcript tail = assignment text ONLY, zero assistant actions. `.omp/ragged-impl-report.md` absent (glob). perf-results E-1r row (EngineRagged, 2026-09-10) confirms `ops/curveSource.*`, `ops/deform.*` absent + `--ragged` grep count 0. No delivery; treat as silent-stalled. |
| SessionsRecover3 (nemotron, running) | ALIVE-PROGRESSING (early, no artifact) | Active 25.9s ago; transcript shows 1 action: workspace-root read (135 lines). `.omp/sessions-recover.md` absent (glob). Early recon — not dead, just started. |
| ToolsFlags (Meta, running) | ALIVE-PROGRESSING (just dispatched) | Active 1.5s ago; transcript = assignment only. No perf-results rows yet (file = 1 E-1r row only). Meta endpoint flaky per context — allow retries. |
| SparseTerminal (Meta, running) | ALIVE-PROGRESSING (code done, numbers unlogged) | Transcript: implemented `--terminal` in tests/perf/benchUsdGenSparse.cpp (grep confirms lines 122–126), fixed `]}` syntax error, `ninja -C build benchUsdGenSparse` GREEN, ran baseline + `--terminal` (both ok, outputs truncated). NO rows appended anywhere (perf-results.md has only E-1r row). |
| LocatorFreeze (parked) | DELIVERED-baseline / FROZEN | `.omp/locator-impl-report.md` §3.1 ALREADY-IMPLEMENTED finding present; `.omp/locator-contract.md` FINAL RULE present. Correctly parked awaiting adjudication verdict. NB: its baseline notes `usdGenGraphDescBuilder.cpp:112-114` missing `gf/array.h` include → usdGenImaging RED at time of writing (blocks adapter/surgery builds until RATIFIED-GO fix). |

agent:// reads 404 for all four running lanes (artifact store only lists parked/completed agents) — expected, not a finding; history:// transcripts used instead.

## (B) Wave-plan §3/§4 quotes + invalid-path flags

ROGUE-WRITER CHECK: CLEAN. No `testUsdGenSparseBuffers.cpp`, no `SI-112`, no locator-storm/GPU-bench wave-1. Only hit for `S112` is the known §2 row-42 typo (`needs-implement/S112`, should be `S11`). §3 = P1 (storm run-only, nemotron) / P2 (E-1r+E-3 flags, apple-sonic) / P3 (census triggers, hivemind) / P4 (doc gap, apple-sonic). §4 = locator-migration (pending ratification) / sessions registration (after SessionsRecover2 verdict) / commits (after Integrator5 GO) / E-1 decision packet. §5 command + §6 risks as read (full text in file, 155 lines).

Wave-1 verbatim (abridged, acceptance commands exact):
- P1 exact files `tests/perf/benchUsdGenStorm.cpp`; accept `HD_ENABLE_PERFLOG=1 ./build/benchUsdGenStorm --static && ... --batches && ... --deform && ... --tilesweep`. Source grep VERIFIES all four flags exist (:437–440). Binary `build/benchUsdGenStorm` EXISTS. ⚠ acceptance chains four modes in ONE command (violates ≤1 acceptance-command guideline) + `--tilesweep` REQUIRES `--scene-list` (source :497–500) or exits 2 — P1 packet as written will fail tilesweep without scene list.
- P2 exact files `tests/perf/benchUsdGenChain.cpp`, `tests/perf/benchUsdGenSparse.cpp`; `--ragged` NOT in chain source (grep: no hit — ToolsFlags owns); `--terminal` PRESENT in sparse source (:122–126, SparseTerminal landed, binary `build/benchUsdGenSparse` EXISTS). `build/benchUsdGenChain` EXISTS.
- P3 acceptance creates `tests/testUsdGenPruningCost.cpp`, `tests/testUsdGenReorderNotice.cpp`, `tests/testUsdGenStormInPlace.cpp` — NONE exist on disk (tests/*.cpp glob: 24 files, none of these; also absent: benchUsdGenKnn, benchUsdGenMemory, testUsdGenExpr/LookBake/Maps/MapReload/Ptex). To-be-created → OK as packet targets, flagged NOT-YET-EXISTING.
- P4 cites `plan/02-schema.md §0.7`, `plan/06-imaging.md §2.2 rule 4`, `tests/testUsdGenContracts.cpp` — existence of plan cites unverified (out of cheap scope); contracts test source EXISTS.

§4 binary list flags: `testUsdGenFrozenReentry`, `testUsdGenStageEdits`, `testUsdGenInstancerPick`, `testUsdGenInstancer`, `testUsdGenKnn`, `testUsdGenMemory`, `testUsdGenMaps`, `testUsdGenExpr`, `testUsdGenPtex`, `testUsdGenLinkRule_*`, `testUsdGenIncludeRule` (§5 command) — NONE have sources in tests/ (glob). §5 command has CWD BUG: `cd .../build && ninja -C build` (double build dir). `testUsdGenPopulation.cpp` source NOT in tests/ tree though `build/testUsdGenPopulation` binary exists [INFERENCE: deleted or relocated in dirty tree — verify]. §2 row SI-6 `needs-implement/test-binary-missing` contradicts gate-status PASS (Population) — PlanFinalizer's assigned fix, still unfixed in file. R-1..R-3 `workstation-resources`: gate-status.md §5.4 lists them as `workstation protocol §3/§5/§7 (release)` — classification CORRECT, keep.

## (C) NEXT DISPATCH LIST (ordered, gated)

1. AWAIT PlanFinalizer yield (budget-capped, imminent) — do not pre-empt; adjudication-1.md locator RATIFY/AMEND verdict unblocks LocatorMigrate. (nemotron, running)
2. On RATIFIED verdict → dispatch LocatorMigrate (idle, wake via hub): `gf/array.h` include fix + build usdGenImaging/testUsdGenAdapter/testUsdGenStormSurgery/testUsdGenRestAdapter + ctest -R. (prefer nemotron/lean-runner; correctness work)
3. NUDGE RaggedP1 via hub `send` check-in (silent 5m+); no answer → re-dispatch P1 ragged engine (nemotron, free endpoint ONLY). Gate: none. (E-1r blocked until this lands.)
4. AWAIT SessionsRecover3 `.omp/sessions-recover.md` (started, progressing); then sessions registration packet (needs SI-10 verdict green). (nemotron)
5. AWAIT ToolsFlags chain flags + perf rows (Meta flaky — tolerate 429 retries, incremental partials).
6. SparseTerminal follow-up (same lane, ≤5 lines): append E-3 `--terminal` median + command to `.omp/perf-results.md`. CPU bench — may run despite GPU load.
7. HOLD P1-storm benches (S-1/S-5/S-6/S-12): GPU at 96% (nvidia-smi) — NOT quiescent. Dispatch only after util <10%.
8. HOLD CMakeIntegrator registration batch until source-green (usdGenImaging red per locator baseline; re-verify post step 2).
9. HOLD all locator-shape edits until adjudication verdict (freeze stands). HOLD commits until Integrator5 GO (no commit-plan.md on disk).
10. Post-PlanFinalizer cleanup packet (apple-sonic ≤2KB or nemotron): fix S112→S11 typo, SI-6 row, §5 CWD bug + stale binary list, P1 tilesweep --scene-list.

## (D) ESCALATIONS TO USER (decision packets)

- E-1 FAIL (1.687–1.94ms vs ≤1.5ms @8T): tuning-vs-waiver decision packet (§4) needs user stance — authorize optimization attempt on NoiseE1 path (chunk/thread tuning) or waive/relax bound to M3 with recorded shortfall (0.187–0.434ms over)? Cannot proceed without direction.
- R-1..R-3 (usdrecord/hdPrman blur, MSAA/A2C-vs-OIT visual sign-off, AMD/Intel glslfx): workstation hardware + human visual judgment — workstation-vs-headless: schedule workstation session or defer to release (M7+)? Gate-status marks release-built M7; confirm deferral.
- GPU at 96%: storm-bench wave (M1-exit-critical S-1/S-5/S-6/S-12) cannot run green until GPU frees — user may know the competing process.
- Git tree: 48 dirty lines (28 modified + 20 untracked), ZERO new commits (HEAD still 1daff8e M1-wip). Dangling-commit D1/D2 ruling still pending from PlanFinalizer — commit discipline holds until Integrator5 GO.

## Task 2–5 evidence summary

- adjudication-1.md / ragged-impl-report.md / sessions-recover.md / commit-plan.md: ALL ABSENT (glob path-not-found). perf-results.md: 1 row (E-1r UNMEASURED, EngineRagged rationale). locator-impl-report.md: ALREADY-IMPLEMENTED baseline + gf/array.h red note.
- Locator asserts: `tests/testUsdGenPrimAdapter.cpp` DOES NOT EXIST (correct file is `tests/testUsdGenAdapter.cpp`) — which CONTAINS explicit `-value` pins (:206–267: length-value, width-value, magnitude-value, clump sibling-unmoved, dirty-exactness) + `errorMark.IsClean()` (:346). Recently added per locator lane. Storm-surgery name-identity asserts: still absent per locator-impl-report (file untouched from HEAD) — outstanding.
- git porcelain: 48 lines (28 M + 20 ??); `git log --oneline -3`: 1daff8e (HEAD, M1 wip) / 831c869 (M0) / 678165b (plan v1) — no new commits.
- GPU: `utilization.gpu 96%` — NOT QUIESCENT; GPU benches gated.
- NOTE: this agent has no write/edit tool in its toolset — `.omp/coord-audit-1.md` could not be written directly; Main MUST persist this `report` field verbatim to that path.

