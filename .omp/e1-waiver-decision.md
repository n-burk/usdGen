# E-1 waiver decision — INFEASIBLE at 1.5 ms canonical

Date: 2026-09-10 · Author: WaiverHive (PKT2) · Sources (read-only): `.omp/gate-status.md` §5.1 E-1 row; `.omp/perf-results.md` E-1 rows.

## Verdict

**INFEASIBLE** — the ≤ 1.5 ms canonical bound (8 threads, private arena; `plan/09` §5.1, quoted at `gate-status.md:15`; 20-thread number parenthetical only, ADR R27) is not met by the current NoiseE1 implementation. No waiver; record for M3 (NoiseE1 redesign; `noise.cpp` optimization in flight per `gate-status.md:15` status note).

## Measured E-1 values (file:line)

| Date | Source | Value | Command | Notes |
|---|---|---|---|---|
| 2026-09-09 (frozen census) | `gate-status.md:15` (summary `gate-status.md:87`) | 1.687–1.94 ms | `benchUsdGenChain --curves 100000 --stylers 5 --chunk 512` | status cell: FAIL vs ≤ 1.5 ms @8thr |
| 2026-09-10 | `perf-results.md:8` (Gate=E-1, Uniform col) | 1.998 ms | `benchUsdGenChain --curves 100000 --stylers 5 --chunk 512` | bench 6.4 ms limit PASS; canonical 1.5 ms OVER, "waiver pending"; E-7 ratio 4.695 ≥ 3.000 PASS |
| 2026-09-10 | `perf-results.md:7` (Gate=E-1r(flag-side), Notes col) | 2.706 ms (uniform re-measure) | `benchUsdGenChain --ragged` | note: "E-1 FAIL 2.706ms (limit 1.500), ratio 4.576 > 3.000 limit" |

## Reconciliation

Post-kernel-landing appends 2026-09-10 (1.998 ms `perf-results.md:8`; 2.706 ms `perf-results.md:7`) supersede the stale frozen-census note 1.687–1.94 ms (`gate-status.md:15`, `gate-status.md:87`): every run, pre- and post-kernel, exceeds the 1.5 ms canonical bound — shortfall widened from +0.187…+0.44 ms to +0.498…+1.206 ms, so no measurement supports a waiver.

## Bench-drift note

Bench binary self-gates at 6.4 ms ("bench limit 6.4 PASS", `perf-results.md:8`) — hence bench verdict PASS despite canonical 1.5 ms FAIL; the 6.4 ms relaxation does not satisfy the plan/09 §5.1 criterion.

## Disposition

Record for M3: re-measure E-1 after NoiseE1 redesign (`noise.cpp` optimization in flight, `gate-status.md:15`). Until then E-1 stays FAIL (value vs bound); `gate-status.md` is the frozen 2026-09-09 census — no Status-cell edits.
