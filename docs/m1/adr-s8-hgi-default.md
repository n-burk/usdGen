> Historical engineering note. It records a review or probe, not the current product overview. Start at the [repository README](../../README.md) and [docs index](../README.md).

# ADR S-8: HGI resource generation stays opt-in (B is the default)

Date: 2026-09-09 — Milestone M1 exit gate S-8 (plan/09 §5.3, PW decision).

## Context

Storm can bake per-draw-item GL resources either through the legacy buffer
path (B: `HDST_ENABLE_HGI_RESOURCE_GENERATION=0`, the upstream default) or
through HGI resource generation (A: `=1`). The M1 exit table requires the A/B
switch to produce equivalent looks and for one branch to be chosen as the
shipping default for the usdGen Storm path.

## Evidence

- `ctest -R testUsdGenStormHgiResource` (T2, EGL headless, measurement host,
  GL 4.6.0 NVIDIA 580.173.02): both branches render the L-2 scene at
  complexity 1.2, 256x256; **A/B mean pixel difference = 0.00000** (bit-parity)
  and the B branch is bit-stable across re-renders.
- Earlier in M1 an A-branch crash (SIGSEGV in the HGI bake submit path) was
  observed when HGI=1 was combined with live stage edits and shared-context
  teardown in long-lived processes; the fork-exec child in the gate test no
  longer reproduces it, but the crash history plus zero look difference
  removes any benefit of flipping the switch for M1.

## Decision

Keep the upstream default: usdGen ships with HGI resource generation **off**
(B). The gate test runs A in an isolated fork-exec child so a regression in
the A path can never take down the M1 suite, and reports the A-branch crash
as a documented fallback rather than a suite segfault.

## Consequences

- S-8 is green with B; no env-var change lands in `_env.sh` or the session.
- If M2+ perf work needs A (GPU-side bake for >1M curves), re-open this ADR
  with the teardown-crash repro fixed first; the gate already enforces look
  parity if the flip happens.
