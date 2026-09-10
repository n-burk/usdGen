# Planner queue state (checkpoint from Planner2, 2026-09-10)

## ORDER
1. BenchMicro (LIVE — owns build/ until settle): single absolute-ninja bench build FIRST, -I shadow forensics via ninja -t commands, NO bench-:287 edit unless feeding header lacks tiles, smoke E-8 + --ragged, row-3 pipe-fix + measured row.
2. RaggedRatio-resume: E-8 binary + /tmp 5-run medians + ratio ≤2× + finalize ragged-impl-report.md (WRITE-new / existing-TAG rules).
3. LocatorMigrate builds: usdGenImaging + 3 tests, serialized, owned window.
4. LocatorRealAssertions: test-source owner owns tests/testUsdGenStormSurgery.cpp; asserts per RATIFIED RULE.
5. StormWatch: read-only nvidia-smi quiescence check.
6. StormSurgeryRun: GPU-runner — build surgery binary + ctest T2 when quiescent.

## RULINGS
- Build windows: ownership-until-settle; pgrep guard is courtesy-inside-window only; release only at settle.
- Locator gate: UNVERIFIED (census-addendum: SI-7 Adapter/RestAdapter M1-STUB passes verify nothing; StormSurgery never ran; build-green only). gate-status.md is the frozen 2026-09-09 census — NO Status-cell edits.
- Struct truth: SINGLE `usdGen::UsdGenRunResult` WITH tiles (scheduler.h:37-44); bench :275-287 correct → compiler error = stale-header / -I shadow hunt; no bench-usage edit.
- D2 amend-pending: gf/array.h nonexistent in OpenUSD 26.08; vt/array.h:14 correct on disk; adjudication-1.md amend = clerical, later.
- Artifacts landed: adjudication-1.md (RATIFIED+GO), sessions-recover.md (BLOCKED → 3 edits landed, compile-unproven), ragged-impl-report.md (GREEN, E-8/medians ratio pending), perf-results.md 3 rows (row-3 malformed w/ stale numbers).
- Meta endpoint SUSPENDED; P1-storm HELD (GPU + S11/tilesweep/CWD fixes owed at dispatch); commits HELD (no commit-plan); E-1 waiver / R-rows / needs-implement backlog DEFERRED per wave-plan §2.

## TRIGGERS
- Settle → release next named owner in ORDER.
- BenchMicro settle → RaggedP1 ratio window.
- SI-10 compile-green → CMakeIntegrator registration batch (snippet at repo root cmake_sessions_registration_snippet.txt).

## TAXONOMY
- New file → write tool. Existing file → fresh read for [#TAG] immediately before each edit, original line numbers, + -prefixed rows only.

## ADDENDUM-restored 2026-09-10
GPU last 96% (StormWatch must re-probe). P1-storm dispatch fixes: S112→S11 row42 (keep SI-11/SI11 distinct row31); tilesweep --scene-list required; 1 cmd per mode; fix §5 CWD. Commits: D1/D2 ruled in adjudication-1.md RATIFIED — commit-plan still absent, hold until Integrator GO. Meta dead lanes: ToolsFlags/OrchPlanner/VerifyAudit1/E3LogAppend. Deferred: E-1 waiver (1.687-1.94 vs 1.5), R-1..R-3 workstation, needs-implement B-1/E-4/E-5/SI-9/SI-11/S-2/S-3/S-4/S-7/S-10/S-11/L-3/L-4/L-5/T-rows per wave-plan §2.
Surgery TU deferred->M3: P0 race + locator asserts owed; quarantined .omp/quarantine/testUsdGenStormSurgery.cpp.broken-20260910; tree has NO surgery TU by design (EXISTS-guard) — S-11-adjacent T2 unregistered, PENDING-FIX per gate-status.
