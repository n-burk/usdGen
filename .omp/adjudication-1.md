# Adjudication — Locator Collision Migration (singleton packet) — FINAL (2026-09-10)

Supersedes the 2026-09-10 earlier verdict, which claimed the D2 include was applied; re-verification found it still absent from the include group (file snapshot #46E7, includes at L12-26). Fixed now.

## §0 Verdict
RATIFY — the §3.1 recommended migration (ranking A: ancestor-attribute `-value` suffix) is the correct and minimal plan amendment. No rel-key churn; zero gate-text amendments; only 7 attribute leaves acquire `-value` suffixes; rel rows in §6.1 unchanged.

## §1 D1 — §3.1 pseudocode
RATIFY: the final rule (ancestor strict → `-value` on attribute leaf / `-rel` on relationship leaf) is already implemented in `primAdapter.cpp:249-283` (baseline). No delta to the function itself.

## §2 D2 — Builder-file fix (applied)
SINGLE HUNK: `libs/usdGenImaging/usdGenImaging/usdGenGraphDescBuilder.cpp` — inserted `#include "pxr/base/gf/array.h"` into the pxr include group, immediately before `#include "pxr/base/tf/diagnostic.h"` (was absent; blocks ninja build). Verified absent pre-fix; applied in this run.

## §3 GO / NO-GO
GO — RATIFIED; D1 no-op in baseline; D2 one-line include applied. Build prerequisite met; no other code edits pending.

## Adjudication ping
RATIFY • D1: §3.1 rule already in baseline (primAdapter.cpp:249-283) • D2: `#include "pxr/base/gf/array.h"` in usdGenGraphDescBuilder.cpp, applied this run • GO
