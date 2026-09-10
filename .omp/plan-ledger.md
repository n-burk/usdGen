PRELIMINARY — README/ledger-derived; plan-file shards PENDING
# Plan ledger — usdGen (2026-09-10, PlanLedgerInit)
Derivation: plan/README.md §4 (D1–D11 mapping) + gate-status.md §5.1–5.4/tally + perf-results.md rows 15–20 + planner-queue.md + e1-waiver-decision.md ONLY. plan/01…15 shards NOT read (shard lanes own them); nothing below is claimed verified-from-plan — gate/milestone cells are ledger-derived, pending shard verification.
Milestones per gate-status ledger: M0 DONE · M1 DONE · M2 DONE (G6 GATE-CLEAN; G7/G8 PKT-TILE-SWEEP g2/g1=0.052) · M3 in flight.

| Deliverable | Status | Milestone | Gate id(s) | Next executable packet | CUDA candidate (hotness basis) |
|---|---|---|---|---|---|
| D1 Curve/USD parser (usd/UsdParser.*) | DONE | M1 | SI-1…SI-6 | — | no — CPU parse; no perflog hotness |
| D2 Per-curve attr storage (UsdCurveData.h) | DONE | M1 | SI-2 | — | no |
| D3 GPU haircard generator (HairCard, TileGrid) | DONE | M2 | SI-1, G6 GATE-CLEAN | — | yes — tile publish inside usdGen 1.0 ms frame share (S-1 budget 16.6−1.35−1.0−1.44); PKT-TILE-SWEEP g2/g1=0.052 |
| D4 Multi-attribute render pipeline (UsdRender) | DONE | M2 | L-2, S-8 (decision A), S-9 (decision) | — | yes — 1.44 ms upload + 1.0 ms usdGen share (S-1 budget) |
| D5 Scene assembly / multi-view (UsdScene) | DONE | M1 | SI-3…SI-7 | — | no |
| D6 USD CLI (tools/USDGenCLI.cpp) | PARTIAL | M3 | S-1 PASS, S-4/7/9/10/11 PASS; S-2/S-3/S-5/S-8 PARTIAL; **S-6 FAIL, S-12 FAIL** | P1-storm owed fixes: S-6 vboRelocated counters (tiles=0) + S-12 tilesweep `--scene-list` (empty scenes) + CWD fix — dispatch-gated (P1-storm HELD) | no — orchestration; hot paths D3/D4 |
| D7 Tests (usd/test*.cpp) | PARTIAL | M1–M3 | SI-7; non-registry `testUsdGenStormSurgery` PENDING-FIX | LocatorRealAssertions (rewrite tests/testUsdGenStormSurgery.cpp per RATIFIED RULE) → StormSurgeryRun (build + ctest T2 when quiescent) | no |
| D8 Benchmarks (benchUsdGenChain) | DONE | M2–M3 | E-1 (rows supersede), E-2/E-6/E-7/E-8 PASS | — (E-1r at M3) | n/a (harness) |
| D9 Docs (plan/00, README) | DONE | M1 | — | — | no |
| D10 CUDA-1 noise GPU (noise_gpu.cu) | **DONE** | M3 | E-1, E-1r | E-1r uniform-only (M3); E-1 canonical 1.5 ms stays RED→M3 (e1-waiver-decision: INFEASIBLE 1.687–1.94/1.998/2.706 ms; record-for-M3 NoiseE1 redesign) | yes — post-cleanup Release ON 2.163 / OFF 2.278 ms, parity 12/12 bit-exact (perf-results rows 15–20) |
| D11 GPU surgery/relayout (UsdSurgery*) | **BLOCKED (M3)** | M3 | §5.2 E-1 BLOCKED, E-1a N/A, E-2/E-3 BLOCKED | StormSurgeryRun — gated on LocatorRealAssertions (TU broken at 6f445c2, quarantined `.omp/quarantine/testUsdGenStormSurgery.cpp.broken-20260910`) + StormWatch re-probe (GPU last 96%) | yes (candidate) — captureMs/evalMs UNMEASURED (bench never ran) |
| Fur renders (FurRender lane) | IN-FLAKE-FIX | M3 | — (lane, not registry gate) | FurRender lane rework (peer FurRender, idle) | — |

## Counts
DONE 8 (D1–D5, D8, D9, D10) · PARTIAL 3 (D6, D7, fur renders) · MISSING 0 · BLOCKED 1 (D11)
Top incomplete: 1) D6 — S-6/S-12 P1-storm fixes (dispatch-gated); 2) D7 — LocatorRealAssertions → StormSurgeryRun; 3) D11 — StormSurgeryRun (same chain, TU quarantined).
