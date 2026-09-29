# Cross-document consistency review — numbers and evidence

Date: 2026-09-05. Status: plan v1 review (draft, pending owner sign-off).

This document is the numbers-and-evidence consistency pass over the fifteen plan files
(`00-…` through `12-…`, `appendix-A-evidence-ledger.md`, `appendix-B-prototype-inventory.md`).
It extracts every timing, count, size, budget, estimate, version and `file:line` citation, flags
each value that differs between documents for the same quantity, checks every MEASURED tag against
`appendix-A-evidence-ledger.md` §2 and — where appendix A and a sibling disagree — against the
report section or the carried prototype output that produced the number, lists every number that
carries no MEASURED / `DERIVED from EV-nnn` / UNMEASURED / ASSUMPTION tag (ADR §9 R42), and lists
the rows appendix A is missing. It is a defect list, not a design change: it never re-decides
anything the ADR settled. Findings are `N-01`…`N-52`; each names the file that must change, the
canonical value, and the fix.

Reads with: `../appendix-A-evidence-ledger.md` (the evidence ledger this pass makes canonical),
`../09-performance-and-benchmarks.md` (the single frame ledger and the single gate registry,
ADR §9 R40–R41), `adr-v1.md` §9 (the rulings every finding is measured against),
`../appendix-B-prototype-inventory.md` (the carried raw outputs used as the tie-break).

---

## 0. Method, authority order, and verdict

### 0.1 Authority order used to arbitrate

1. **Carried prototype output** under `plan/prototypes/` (`results_main.txt`, `noticeCost.txt`, …).
   A number printed by a file in the repo beats a number transcribed into a report.
2. **`research/` report section**, where no carried output exists.
3. **`appendix-A-evidence-ledger.md` §2**, once corrected against (1) and (2). It is the canonical
   handle for every MEASURED number (ADR §9 R1, R43).
4. **`09-performance-and-benchmarks.md`** §0.2 for the frame ledger (ADR §9 R41) and §5 for gate
   ids, tiers, milestones and thresholds (ADR §9 R40).
5. **`02-schema.md`** for property names, types and defaults (ADR §9 R7); **`10-…` §3.5** for env
   vars (R35); **`08-…` §1.4** for the C ABI (R31).
6. **OpenUSD 26.08 at `<openusd-src>`** for every `file:line`, re-verified by reading
   the cited lines during this pass.

### 0.2 Verdict

Six problems are blocking; the rest are ordinary edits.

| # | Blocking problem | Findings |
|---|---|---|
| 1 | **Two incompatible `EV-nnn` schemes.** `09-…` invents a 100-block-per-subsection scheme in which `EV-021` = "AoS pass 0.506 ms"; appendix A and `04-…` use a flat scheme in which `EV-021` = "Storm at 200 k, 12.43/8.17/23.93/23.69 ms". The same handle names two different measurements in two documents. | N-01, N-02 |
| 2 | **Appendix A `EV-010` does not match the carried probe output.** `prototypes/data-plane-benchmark/results_main.txt:95-102` prints `0.0000–0.0003` and `run_full_with_outstanding_ref_ms 3.10`, not `0.00000–0.0001` and `2.32`. The "≈0.35 ms CoW detach" the design leans on is a report transcription, not the carried run (which gives ≈1.2 ms). | N-20, N-21 |
| 3 | **Two frame ledgers.** `01-…` §0.3 publishes a second ledger that differs from `09-…` §0.2 in five of nine rows and declares the wrong viewport. | N-12, N-13 |
| 4 | **Gate id `S-11` names two different gates** (head1M draw record in `09-…`; the tile-count sweep in `12-…` and appendix A), and `SI-10` names two different gates (session arbitration in `12-…`; the `reorder nameChildren` check in appendix A). | N-32, N-33, N-34 |
| 5 | **`esfUsd/stageData.cpp:360` is a comment line.** Verified: the `UsdPrimDefaultPredicate` call is at `:361`; `GetPrimAtPath` at `:351`. Four documents ship `:360`. | N-43 |
| 6 | **`00-…` §2.1's version table contradicts ADR §9 R38/R44** (it puts `UsdGenPtexMap`, `UsdGenInstance` and motion profile P1 in v2; every other document and R38 put them in v1). | N-49 |

Ten of the fifteen documents cite evidence by a handle appendix A does not carry. Only
`04-operators.md` uses appendix A's flat `EV-nnn` scheme correctly today.

---

## 1. Canonical value table

Every row below is the value all fifteen documents must converge on, with the source that makes it
canonical. Where a document currently prints something else, the finding id is named.

| Quantity | Canonical value | Source that makes it canonical | Divergent documents |
|---|---|---|---|
| Rig evaluation, frame-ledger row | **1.35 ms** MEASURED (`EV-035`; the underlying spread is 1.351–1.502) | `09-…` §0.2 (ADR R41) | `01-…` 1.35–1.50 (N-12) |
| Dirty routing + commit bookkeeping | **0.04 ms** DERIVED from `EV-037` (0.2 µs × ~200 entries) | `09-…` §0.2 | `01-…` ~0.05; `03-…` §5.5 ≈0.05; appendix A §2.11 ≈0.05 (N-12, N-15, N-17) |
| Deformed tail, 800 k CVs | **0.4–0.6 ms** DERIVED | `09-…` §0.2 | — |
| SoA→AoS interleave, all tiles at 100 k | **0.25 ms** DERIVED floor, gate **S-2** | `09-…` §0.2, §2.1 (`I = 0.0255 ms/MB`) | `01-…` 0.25–0.5 ms, gate E-1; `03-…` `I ≈ 0.25–0.5 ms` (N-12, N-15) |
| Per-tile `extent`, 100 k | **0.05 ms at 49 tiles, ASSUMPTION** | `09-…` §0.2; appendix A §2.11 | `01-…` DERIVED (N-45); `03-…` §6.3 "64 tiles", UNMEASURED (N-15); `06-…` "64 tiles", DERIVED (N-16) |
| Generation diff + publish | **0.02 ms at 49 tiles** DERIVED | `09-…` §0.2 | `03-…` §5.5 "at 64 prims" (N-15) |
| **usdGen total, deform frame** | **0.8–1.0 ms** DERIVED aggregate | `09-…` §0.2 (ADR R41) | `01-…` 0.8–1.2 (N-12); `03-…` §0.1 0.8–1.2 (N-14) |
| Storm draw at 100 k, refine 2, 720p | **~12.2 ms**, `DERIVED from EV-020/EV-021`, gate **S-1** | `09-…` §0.2 | `01-…` ~12 ms; `06-…` ~12 ms; appendix B ~12 ms (N-12, cosmetic elsewhere) |
| Storm points upload at 100 k | **≈1.44 ms** DERIVED (delta interpolation), gate **S-2** | `09-…` §0.2, §2.1 | `01-…` ~1.4 ms; `11-…` §2.3 **≈1.25 ms** (N-19) |
| **Frame total** | **15.8–16.0 ms** DERIVED | `09-…` §0.2 | `01-…` ~16 ms (N-12); `03-…` "~15 ms frame" (N-14) |
| 60 Hz budget used in arithmetic | **16.6 ms** | `09-…` §2.5's `N_max` solve and §5.3's S-1 threshold both use 16.6 | `01-…` 16.7 ms; `08-…` 16.7 ms (N-18) |
| Viewport for every measured Storm number | **1280×720** | `09-…` §0.2, §9; `research/G-storm-hair-look-prototype.md` §5 | `01-…` §0.3 header "1920x1080-class" (N-13) |
| Tiles at 100 k / chunk 512 / `tileTarget` 64 | **49** (`nTiles = min(nChunks, clamp(ceil(nChunks/chunksPerTile), 32, 256))`) | ADR §9 R21; appendix A §6 K24 | `00-…` §5.2 formula omits `min` (N-22); `03-…` §5.5, §6.3 use 64 tiles; `06-…` uses 64 tiles (N-15, N-16) |
| Chunks at 1 M curves | **1 954** (`ceil(1000000/512)`) | arithmetic; `03-…` §1.4 | `01-…` §5 I5 "1 953" (N-23) |
| Tile-count sweep points | **1 / 32 / 128 / 196 tiles** (196 = `nChunks` at 100 k; 512 is above the 256 clamp *and* above `nChunks`) | `09-…` §5.3 S-12 | `12-…` RK-07, Q-02, §7 "512-tile" (N-24) |
| Per-curve AoS bytes | **60 B/curve** = id 8 (uint64, R12) + rootPrim 4 + rootUV 8 + root frame 36 + mask 4 → **6.0 MB at 100 k** | `09-…` §2.6; ADR §9 R12 | `03-…` §7 uses `(4+4+8+36)` = 52 B → 5.2 MB (N-25) |
| Engine RSS model at 100 k | **77–86 MB** DERIVED (`≈8–9×` raw positions) | `09-…` §2.6 | `03-…` §7 "≈80 MB, ≈8.4×" with capture caches 8.0 MB vs 09's 3.6 MB (N-25) |
| HUD example memory figure | **86 MB** (top of the 77–86 MB model) | `09-…` §6.1 | `06-…` §8 "89 MB" (N-26) |
| E-5 baseline | **668 MB peak RSS with per-node point buffers; 293 MB with one shared buffer** | `EV-007`; `research/G-data-plane-engine-prototype-benchmark.md` §5 | `11-…` §2.4 "293 MB for buffers alone" (N-27) |
| Capture cost per enable/disable gesture | **10–150 ms, ASSUMPTION** | ADR §9 R42; appendix A §2.11 | `02-…` §0.4 "10–300 ms" (N-46); `12-…` X-07 tags it DERIVED (N-47) |
| VDF full run at 100 k (strip, request-all) | **min 3.797 / median 8.321 / max 13.979 ms** — quote as "3.80 min, 8.32 median" and say so | `prototypes/data-plane-benchmark/results_main.txt:18`; appendix B §5 already states it | appendix A `EV-011` prints "3.80–8.32" under a "ranges are min–max" preamble (N-28); `09-…` §1 prints "3.80–10.4" (N-29) |
| `VtArray` CoW publish cost | **0.0000–0.0003 ms** (below the timer's resolution; the max printed sample is 0.0003) | `results_main.txt:95-101` | appendix A `EV-010` "0.00000–0.0001"; `01-…` "0.00005 ms"; `03-…` correctly prints 0.0003 (N-20, N-21) |
| CoW detach on the next full run | **UNMEASURED / disputed**: the carried run is `3.10 ms` against `1.72–1.91 ms`; the report's run is `2.32 ms` against `1.97 ms` | `results_main.txt:102` vs `research/G-data-plane-engine-prototype-benchmark.md` §3.4 | appendix A `EV-010` and `01-…` §4.2 present 2.32/1.97 as the printed source (N-20, N-21) |
| Session-layer freeze author cost | **0.4 ms, no I/O** (MEASURED, freeze report §3 line "session layer, static (author only, no file)") — a third probe, distinct from `EV-041` (C++ 0.25/0.27/0.27) and `EV-042` (Python 0.51/0.52) | `research/G-freeze-bake-undo-and-frozen-reentry.md` §3 | appendix A has no row (N-37); `08-…` §7.3 conflates it as "0.4–0.52 ms" (N-38) |
| Notice cascade per frame | pick one run and say which: report §5 gives 0.005 / 0.007 / 0.061 / 0.21 / 1.13 ms; the carried `noticeCost.txt` gives 0.0030 / 0.0108 / 0.0279 / 0.1977 / 1.2376 ms | `prototypes/evaluation-scheduling/noticeCost.txt` | appendix A `EV-037` uses the report run, appendix B §6 uses the carried run, without saying they differ (N-30) |
| Predicate line in the OpenExec resync defect | **`pxr/exec/esfUsd/stageData.cpp:361`** (`:351` = `GetPrimAtPath`; `:360` is a comment) — verified this pass | OpenUSD 26.08 source | `02-…`, `04-…`, `06-…`, `11-…` ship `:360` (N-43) |
| Storm render-context order citation | **`pxr/imaging/hdSt/renderDelegate.cpp:695-707`** (function `_RenderDelegateInfo()` opens at `:695`; the `materialRenderContexts` assignment is `:702-707`; `:701` is `materialBindingPurpose`) | ADR §9 R43; verified this pass | `00-…`, `01-…`, `11-…` ship `:701-707`; `07-…` ships `:701-710` (N-44, N-45) |
| Version scope of `UsdGenPtexMap` / `UsdGenInstance` / motion P1 | **v1** (`PtexMap` M4, `Instance` M6, P0/P1/P2 M7) | ADR §9 R38, R44; `02-…`, `04-…`, `07-…`, `11-…` all say v1 | `00-…` §2.1 says v2 (N-49) |
| Env-var registry location | **`10-build-dependencies-testing.md` §3.5** | R35; the registry is at 10 §3.5 | `06-…` §8 cites "10-… §7.2" (N-51) |

---

## 2. The evidence-handle split (blocking)

### 2.1 Two `EV-nnn` schemes

`09-…` §1 declares a **100-block-per-appendix-subsection** scheme (`§2.1 → EV-001…`,
`§2.2 → EV-101…`, `§2.3 → EV-201…`, … `§2.12 → EV-951…`) and asserts "appendix A adopts these ids
per R1". Appendix A did not: it uses a **flat** `EV-001…EV-083` numbering, and its §2.0 map is
written against a *third*, retired 09 shorthand (`Q<n>` plus an "A-row" suffix) that 09 no longer
uses. The collisions are real, not cosmetic:

| Handle | Means in appendix A (and `04-…`) | Means in `09-…` |
|---|---|---|
| `EV-021` | Storm at 200 k curves: 12.43 / 8.17 / **23.93** / 23.69 ms | AoS 800 k-CV pass: 0.506 ms = 25.3 GFLOP/s |
| `EV-022` | per-frame deform delta +0.22 / +0.83 / +2.46 ms | SoA streamed pass: 0.545 ms |
| `EV-023` | refineLevel 3 costs nothing over 2 | SoA strip pass: 0.158 ms = 80.9 GFLOP/s |

`09-…` is also internally inconsistent: §1 says §2.2 maps to `EV-101…`, then labels those very rows
`EV-021/022/023` and cites `EV-021` twice in §2.2's prose. Appendix A's flat scheme is canonical
(ADR §9 R1, R43: "Appendix A measurement rows are `EV-001…` and are the only citation handle").
Mapping from 09's ids to appendix A's, for the reconciler:

`EV-021/022/023 → EV-014/015/016` · `EV-101 → EV-014` · `EV-201/202/203/204/205 → EV-019/020/021/022/021`
· `EV-301/303/304/305/307/308/310 → EV-024/026/027/028/030/031/033` · `EV-312 → a new appendix A row`
· `EV-401/402/403/404/405 → EV-035/036/037/038/039` · `EV-501/504/510 → EV-040/044/051`
· `EV-601/603/604/607/610/611 → EV-055/057/058/061/064/065` · `EV-701/703/704 → EV-067/069/070`
· `EV-801…805 → EV-071…075` · `EV-901/904 → EV-076/079` · `EV-951 → EV-081`.

### 2.2 Handle scheme in use, per document

| Document | Handle scheme today | Conformant? |
|---|---|---|
| `00-…` | none (report sections only) | no |
| `01-…` | none (report sections only) | no — and R41 requires it to *cite* 09 §0.2, not restate it |
| `02-…` | retired: "§2.6 row `FZ8`", "§2.8 rows `X1`, `X3`, `X4`" | no |
| `03-…` | none; §12 asserts appendix A has zero `EV-0` rows | no (and the assertion is now false) |
| `04-…` | flat `EV-001`, `EV-014`–`EV-016`, `EV-052`, `EV-067`, `EV-069`, `EV-070`, `EV-081`, `EV-082` | **yes** |
| `05-…` | none; "once appendix A adopts R43's numbering" | no |
| `06-…` | retired: "ledger row `CPU8`", "ledger row `MB5`" | no |
| `07-…` | none; "gain their `EV-nnn` handles when appendix A assigns them" | no |
| `08-…` | literal unfilled `EV-nnn` placeholders (§8.1 ×2, §8.3 ×1) | no |
| `09-…` | parallel 100-block scheme | no |
| `10-…` | retired: `A2.10-B1`, `A2.10-B2`, `A2.10-B5`, `A2.1-E8` | no |
| `11-…` | retired `A2.n-Xn` throughout, incl. the §9 sources row | no |
| `12-…` | retired `A2.n-Xn` throughout | no |
| appendix A | flat `EV-001…EV-083` | **yes** (canonical) |
| appendix B | none (report sections and raw-log paths) | acceptable — it is the prototype inventory |

### 2.3 Appendix A's own legacy map does not resolve

Appendix A §2.0 maps ranges, not rows, so `02-…`'s "row `FZ8`" cannot be resolved mechanically:
`EV-040…EV-054` is **fifteen** rows against `FZ1…FZ13` with "`FZ10` splits into `EV-050` and
`EV-051`" — fourteen names. By content, `02-…`'s `FZ8` (`primvars:rest` +26 bytes) is **`EV-048`**;
`11-…`'s `A2.6-FZ5` (freeze total 0.55/0.59/0.59 ms) is **`EV-045`**; `A2.6-FZ7` (reopen 0.4–0.8 ms)
is **`EV-047`**; `A2.6-FZ10` (usdview author 3.45 ms / `_resetGUI` 35.4 ms) is **`EV-050`/`EV-051`**.
Appendix A must publish a row-for-row map, not a range map.

---

## 3. Frame-ledger discrepancies

`09-…` §0.2 is the only frame ledger (ADR §9 R41). `01-…` §0.3 publishes a second one:

| Row | `09-…` §0.2 (canonical) | `01-…` §0.3 | `03-…` | `05-…` §8 |
|---|---|---|---|---|
| rig | 1.35 ms M | 1.35–1.50 ms M | — | — |
| routing | 0.04 ms D | ~0.05 ms D | ≈0.05 ms D (§5.5) | — |
| deformed tail | 0.4–0.6 ms D | 0.4–0.6 ms D | 0.4–0.6 D | 0.4–0.6 D ✓ |
| interleave | 0.25 ms D floor, gate **S-2** | 0.25–0.5 ms D, gate **E-1** | `I ≈ 0.25–0.5 ms` | ≈0.25 D ✓ |
| per-tile extent | 0.05 ms **ASSUMPTION**, 49 tiles | ~0.05 ms **DERIVED**, 49 tiles | ≈0.05 ms **UNMEASURED**, **64 tiles** | 0.05 ms at **49** tiles ✓ |
| publish + diff | 0.02 ms D, 49 tiles | ~0.02 ms D, 49 tiles | ≈0.02 ms D at **64 prims** | — |
| **usdGen total** | **0.8–1.0 ms** | **0.8–1.2 ms** | **0.8–1.2 ms** (§0.1) | **0.8–1.0 ms** ✓ |
| Storm draw | ~12.2 ms D | ~12 ms D | — | ~12.2 D ✓ |
| upload | ≈1.44 ms D | ~1.4 ms D | — | — |
| **frame total** | **15.8–16.0 ms** | **~16 ms** | "~15 ms frame" (§0.1) | — |
| viewport | **1280×720** | header says **1920×1080-class**, rows say 720p | — | — |

`05-…` is already reconciled; appendix A §2.11's note that "`05-…` §8 still prints 0.8–1.2 ms" is
stale.

---

## 4. Gate-registry discrepancies

`09-…` §5 is the single gate registry (ADR §9 R40). Conflicts:

| Gate id | `09-…` §5 | `12-…` | appendix A §5 | `10-…` §6.5 | ADR §9 R40 | Canonical |
|---|---|---|---|---|---|---|
| **S-11** | head1M static draw record, **M7** | tile-count sweep, M1 | tile-count sweep ("the rename target this ledger proposes"), M1 | absent | not named | **09 wins (R40)**: S-11 = head1M draw record, M7 |
| **S-12** | tile/prim-count sweep 1/32/128/196, **M1** | absent (uses S-11) | absent (uses S-11) | absent | not named | **S-12 = the tile sweep** |
| **SI-9** | *two* metrics: session agreement **and** pruning-wrapper cost, M2 | session arbitration is `SI-10`; SI-9 = pruning cost | pruning-wrapper cost (U19) | pruning-wrapper cost, M2 | pruning-wrapper cost, M2 | **SI-9 = pruning-wrapper cost only**; 09 must split |
| **SI-10** | **absent** | session arbitration, M2 | `reorder nameChildren` notice check (U13), M0 | absent | not named | 09 must register both; the two claims on `SI-10` must be split into two ids |
| **SI-5** | M0 | — | — | M1 ("first green in M0") | **M1** | M1 |
| **S-6** | M2 | — | M1 (§1.6) | M1 | **M1** | M1 |
| **S-4** | M2 | — | — | M2 | M2 | M2 — `06-…` §9's note that "09 §5.3 still says M3" is stale |
| **E-3 / E-8 / T-5** | M2 / M1 / M8 | — | — | M2 / M1 / M8 | M2 / M1 / M8 | agreed — `10-…` §6.5's "rows that differ from 09" list is stale |
| **T-EXPR-1 / T-PTEX-1** | registered, M4 | requested from RK-15/RK-16 | not listed | **absent from the map** | not named | keep in 09; add to `10-…` §6.5 |

Other gate-number defects:

* `11-…` §2.4 states a threshold `09-…` §5 does not carry: "the 5-node chain at 1 M × 8 CV ≤ 10 ms"
  (E-1 at scale). Either register it in 09 §5.1 as an E-1 sub-criterion or drop it (N-35).
* `00-…` §5.4's gate-family list stops at SI-8, S-9, L-1 and has no `B-` family, no `SI-9/SI-10`, no
  `S-10/S-11/S-12`, no `L-2…L-5`, no `T-EXPR-1/T-PTEX-1` — ADR §9 R44 requires them (N-36).
* The **workstation-protocol item numbers** disagree three ways: `09-…` §4.3 numbers five items
  (R-1 = §5, R-2 = §2, R-3 = §3–4); `10-…` §5.2 numbers eight (R-1 = §7, R-2 = §3, R-3 = §5) and
  `10-…` §6.5 uses those; `08-…` §8.3 appends `W1–W4` as "items 6–9" of 09's five-item list (N-39).

---

## 5. Numbers that do not match the evidence

### 5.1 Appendix A rows to correct against the carried prototype output

| Row | Appendix A says | The carried output says | Fix |
|---|---|---|---|
| `EV-010` | publish "below the 0.0001 ms timer resolution (printed `0.00000–0.0001`)"; "next full run with an outstanding reference **2.32 ms** vs a **1.72–1.97 ms** unreferenced run"; "the ~0.35 ms delta is one CoW detach"; "**§4 is the printed source**" | `results_main.txt:95-101` prints `0.0003 / 0.0001 / 0.0000 ×5`; `:102` prints `run_full_with_outstanding_ref_ms 3.10`; `:60-66` print `run_full_ms 1.72–1.91`. `1.97` appears nowhere in the carried output | Publish `0.0000–0.0003 ms` for the publish; record **both** detach runs (report §3.4: 2.32 vs 1.97; carried: 3.10 vs 1.72–1.91) and tag the detach cost **UNMEASURED / disputed** with the gate that settles it, as `03-…` §0.3 and appendix B §5 already do |
| `EV-011` | "VDF strip/request-all **3.80–8.32 ms** at 100 k", under a §2 preamble that says "ranges are min–max" | `results_main.txt:18` prints `min 3.797 med~ 8.321 **max 13.979** (n=7)` | State "min 3.80 / median 8.32 / max 13.98" or change the preamble; appendix B §5 already prints the max |
| `EV-013` | "0.36–0.53 ms at 220 nodes, 2.93–4.39 ms at 2 200, 13.7–23.0 ms at 11 000" | `:27-29` (strip) give 0.388–0.525 / 2.965–4.386 / 13.703–23.042; `:52-54` (cv) give 0.363–0.485 / 2.931–4.017 / 13.681–17.360. The quoted ranges take the min from one probe section and the max from the other | Quote one section, or label the range as "across both probe configurations" |
| `EV-037` | "0.005 / 0.007 / 0.061 / 0.21 / 1.13 ms at 1 / 10 / 100 / 1 000 / 5 000 scalps" (the report §5 run) | `prototypes/evaluation-scheduling/noticeCost.txt` prints 0.0030 / 0.0108 / 0.0279 / 0.1977 / 1.2376 (batch off) and 0.0017 / 0.0096 / 0.0301 / 0.2119 / 1.0777 (batch on) — up to 2.2× apart at 100 scalps | Name which run the row quotes and add the other; the ≈0.2 µs/entry conclusion survives both |
| `EV-026` | "3.46 ms" with `curveIndices` | `prototypes/storm-throughput/PROBE_OUTPUT.txt` probe C gives **3.50 ms** (appendix B §9 already records both) | Record both, name the source |

### 5.2 Rows appendix A is missing (siblings cite numbers with no ledger row)

| Missing quantity | Value | Report / probe | Who cites it |
|---|---|---|---|
| `DirtiedPrimEntry` build with a cached locator set | **0.008 µs/prim** (0.145 µs at N = 100 k; 0.26 µs recomputed) | `research/G-storm-throughput-and-prim-granularity.md` §1.8; appendix B §9 | `01-…` §0.3; `09-…` `EV-307`'s middle value |
| `HdRetainedSceneIndex::DirtyPrims` → 1 observer, notice delivery | **0.02 → 0.53 µs/prim** at N = 32 → 100 k | same §1.8 | `09-…` mints `EV-312` for it and says appendix A "must gain `A2.4-CPU12` before M0 closes" |
| `HdDataSourceLocatorSet::Intersects` | **0.013 µs** | same §1.8 | `03-…` §8.1; `06-…` §6 |
| `GetPrim` through K pass-through filters | **0.17–0.20 µs, flat in K (0…10)** | same §1.8 | the "per-hop cost is below noise" claim in `01-…` and `06-…` |
| Session-layer freeze, author only, no file | **0.4 ms / 0 bytes** | `research/G-freeze-bake-undo-and-frozen-reentry.md` §3 (a third probe, distinct from `EV-041` and `EV-042`) | `05-…` §5.4; `08-…` §7.3; appendix B §7 |
| Scene-index pull of a whole C3 prim | **0.19 / 0.19 / 0.20 ms** at 10 k / 100 k / 1 M; second pull **0.01 ms** | freeze report §4.1 | `05-…` §2.7, §3, §7.2 |
| `probeImagingPipeline` tier-1 harness cost | **36 assertions in 0.07 s** | `research/B-usdrig-build.md` decision 4 | `10-…` §0.4, §5.2 |
| Storm process footprint on the EGL harness | **172 MB** | `research/G-storm-hair-look-prototype.md` §6 | `10-…` §5.4 |
| Full `ComputeDirtyLocators` entry build | **0.90–1.77 µs/prim** (1.77 at N=1 k, 1.16 at 10 k, 0.90 at 100 k; 0.66 µs cached at 100 k) | `research/G-storm-throughput-and-prim-granularity.md` §1.8; appendix B §9 | the S30 "bare leaf locator" argument in `06-…` §5 |

### 5.3 Mis-stated MEASURED values

| Where | Printed | Should be |
|---|---|---|
| `09-…` §1, `EV-011/EV-013` row | "VDF `VdfNetwork` 100 k / 1 M … **3.80–10.4** / 69.9–134.8 ms" | 10.364 is the **cv-layout** median from probe section B, a different configuration; the strip-layout 100 k figures are 3.797 min / 8.321 median / 13.979 max (`results_main.txt:18`, `:39`) |
| `11-…` §2.3 (S-2 anchor) | "9.6 MB at 100 k gives **≈ 1.25 ms**, DERIVED from `A2.3-ST4`" | **≈1.44 ms** — `09-…` §2.1 states explicitly that the ledger uses the *delta* interpolation (1.44), not the per-MB-rate method (which gives 1.25 at 0.13 ms/MB) |
| `11-…` §2.4 | "MEASURED baseline 668 MB with per-node caching (**293 MB for buffers alone**)" | `EV-007`: 293 MB is the **single shared buffer** configuration, not "buffers alone"; `09-…` §2.6 separately notes 668 MB covers the *position plane only* |
| `06-…` §7.3 | "a uniform colour for **3 125 curves** is 37.5 KB … (MEASURED)" | 3 125 curves/tile implies 32 tiles, not the 49-tile default; and 37.5 KB is `3125 × 12 B` — arithmetic, so `DERIVED`, not MEASURED |
| `01-…` §4.2 and §0.3 | "publish is free (**0.00005 ms** MEASURED)"; "**2.32 vs 1.97 ms** for 9.6 MB (MEASURED)" | the carried output prints 0.0000–0.0003 and 3.10 vs 1.72–1.91; both values are report-transcription figures and one of them (1.97) is not reproducible from any carried output |

---

## 6. Number tags (ADR §9 R42)

| Quantity | Canonical tag | Divergences |
|---|---|---|
| per-tile `extent` 0.05 ms | **ASSUMPTION** (min/max fused into the interleave loop) — `09-…` §0.2, appendix A §2.11 | `01-…` §0.3 DERIVED; `06-…` §7.6 DERIVED; `03-…` §6.3 UNMEASURED |
| capture per enable/disable gesture | **ASSUMPTION**, 10–150 ms — ADR §9 R42 | `12-…` X-07 DERIVED; `02-…` §0.4 ASSUMPTION but 10–**300** ms |
| interleave floor | **DERIVED floor**, gate **S-2** | `01-…` §0.3 names gate **E-1** |
| Storm draw at 100 k | **`DERIVED from EV-020/EV-021`**, never MEASURED | uniformly correct; only the value (12 vs 12.2) drifts |
| 37.5 KB uniform colour | **DERIVED** | `06-…` tags MEASURED |
| kNN capture ≈10 ms | **ASSUMPTION** | uniformly correct |
| ~40 µs sub-graph recompile | **ASSUMPTION** | uniformly correct |
| `hairTangent` variant-B republish +1.5–2.5 ms | **DERIVED** | uniformly correct |
| staffing / calendar | **ASSUMPTION** | uniformly correct |

Untagged or under-tagged numbers found (each needs one of the four tags):

* `03-…` §2.1 "3.2 MB at 800 k CVs" for `hairT`, and §2.1 "9.6 MB per node" for `tx/ty/tz` — plain
  arithmetic in prose, no tag.
* `03-…` §8.4 "1 954 `GfRange3f` tests at 1 M curves, ≈ 5 µs DERIVED" — the 5 µs is tagged, the
  1 954 count is not (it is arithmetic and correct).
* `06-…` §7.3 "37.5 KB" — tagged MEASURED, should be DERIVED (above).
* `09-…` §0.3 `head1M` row "~6.5 D" deform-frame total — the scaling factor behind it is not stated.
* `01-…` §0.3 "usdGen owns roughly 7 % of the frame" — tagged DERIVED, but 0.8–1.2 / ~16 is
  5.0–7.5 %; with the canonical 0.8–1.0 / 15.8–16.0 it is 5.0–6.3 %.

---

## 7. `file:line` citations

Every citation below was re-read in `<openusd-src>` during this pass.

### 7.1 Wrong, and load-bearing

| Citation as shipped | Verified truth | Documents to fix |
|---|---|---|
| `pxr/exec/esfUsd/stageData.cpp:360` | `:360` is the comment "// the targeted object exists in the scene."; `if (!UsdPrimDefaultPredicate(resyncedPrim)) {` is at **`:361`**; `GetPrimAtPath` at `:351` | `02-…` (§0.3, §5.4), `04-…` (§0.4, §10), `06-…` (§7), `11-…` (§3.3, §6.4). `00-…`, `01-…`, `05-…`, `08-…`, `12-…` and appendix A already use `:361`. Appendix B flags it correctly in its §12 table |
| `pxr/imaging/hdSt/renderDelegate.cpp:701-707` | `:701` is `info.materialBindingPurpose`; the `materialRenderContexts` assignment is `:702-707`; `_RenderDelegateInfo()` opens at `:695`. ADR §9 R43 fixes the plan's citation at **`:695-707`** | `00-…` (§3.3, §9), `01-…` (§7.2), `11-…` (§1.2, §9) |
| `pxr/imaging/hdSt/renderDelegate.cpp:701-710` and `:709` | same; `:709` is `isPrimvarFilteringNeeded`, `:710` is `shaderSourceTypes` | `07-…` (§4.5, §10) |
| `pxr/imaging/hd/changeTracker.cpp:957-979`, "one bit for every primvar except points/normals/widths" | the function is `:958-979` and the exclusion set is `points, velocities, accelerations, nonlinearSampleCount, normals, widths` — three names are missing | `01-…` (§4.3) |

### 7.2 Approximate but harmless (one-line drift; correct on the next touch)

| Citation | Verified | Where |
|---|---|---|
| `hdSt/basisCurves.cpp:931-935` (points fastpath) | the `if` is `:932-935`; `:930-931` is its comment. Appendix A's `:930-935` and `09-…`'s `:932-935` are both fine | `01-…` |
| `base/ts/types.h:28-37` | `TS_SPLINE_SUPPORTED_VALUE_TYPES` is `:32-36`; appendix A's `:32-37` is right | `02-…` (§2.15, §2.16) |
| `usd/usd/attribute.h:558-563` | `HasSpline` `:552`, `GetSpline` `:563`, `SetSpline` `:568` (appendix A is exact) | `02-…` |
| `hdSt/basisCurvesComputations.h:238-247` for `HdSt_ExpandVarying` | the call is at `:248`; `:236-247` is the varying branch | `07-…` (§3.2, §4.7) |
| `base/vt/array.h:39-54` / `:39-51` / `:39-55` | the `Vt_ArrayForeignDataSource` class is **`:39-55`**; the constructor `:42-46`; `_ArraysDetached()` `:51`. `_IsUnique()` `:1023`, `private:` `:984`, `IsIdentical` `:945` — all verified | `01-…`, `03-…`, `09-…`, appendix A, appendix B (three spellings in use) |
| `hd/sceneIndexAdapterSceneDelegate.cpp:511-522` / `:510-523` | the descriptor-cache-clearing loop is **`:510-522`** (appendix A is exact) | `06-…`, `09-…` |
| `hdSt/vboMemoryManager.cpp:605-620` | the live `#else` branch is `:605-619`, `#endif` at `:620`, dead branch `:596-603`, `#else` `:604`. Appendix A's `:604-618` and 09's `:605-620` both cover it | — |
| `hd/sceneGlobalsSchema.h:37-47` vs `:37-46` | the token macro is `:37-46`; `:47` is blank | — |
| appendix A §3.6 "`_RenderDelegateInfo()` at `:695-712`" | the function body ends at `:714` | appendix A |

### 7.3 Verified correct (spot-checked this pass, no action)

`hd/renderIndex.cpp:205-214` with the display-name guard at `:208` and the append at `:211`;
`hd/mergingSceneIndex.cpp:173-244` with `_SendPrimsAdded` at `:244` and the `_IsObserved()` early
return at `:211-213`; `usdImagingGL/engine.cpp:148-201` (`HdsiSceneGlobalsSceneIndex::New` at `:155`,
phase 0 at `:188`, `RegisterSceneIndexForRenderer` at `:195-201`, `InsertionOrderAtStart` at `:200`);
`hd/sceneIndexPluginRegistry.h:27-31, 91-92, 95, 142, 184`; `sceneIndexPluginRegistry.cpp:1196-1199`
(the `IsEnabled` consult); `hdSt/basisCurves.cpp:292-301` (refineLevel-0 downcast), `:975-990`
(`GetValue(0.0f)`); `hdSt/primUtils.cpp:887-891` (no extent → no culling);
`hdsi/extComputationPrimvarPruningSceneIndex.h:17-45`; `hdSt/basisCurvesComputations.h:202-203`
(shared, not copied) and `:210-234` (indices-or-fallback);
`third_party/renderman/plugin/hdPrman/renderDelegate.cpp:803-810` (`GetMaterialRenderContexts`);
`cmake/defaults/Options.cmake:27, 36`; `pxrConfig.cmake:17`; `usd/usd/usdGenSchema.py:208, 246`.

### 7.4 Citations appendix A itself marks unverifiable

Appendix A §3.11 records four verification limits and the plan honours three of them. The fourth —
"the reports quote `hdSt/renderDelegate.cpp:701-707` … so the plan cites `:695-712`" — is **not**
honoured: four plan documents still ship `:701-707` or `:701-710` (§7.1 above). Every hdPrman
`file:line` in the plan remains source-verified-only and correctly gated on **R-1** (tier 4); the
`hydra_prim_schemas.dox:233-245` claim is correctly recorded as stale everywhere it appears.

---

## 8. Findings

Severity: **blocking** = ships a wrong number or a colliding identifier; **high** = a reader would
draw a different conclusion from two documents; **medium** = a stale or unresolvable citation;
**low** = rounding, one-line drift, or an editorial slip.

| # | File | Sev | Location | Problem | Canonical | Fix |
|---|---|---|---|---|---|---|
| N-01 | `09-performance-and-benchmarks.md` | blocking | §1 preamble and every `EV-` citation | Invents a 100-block `EV-nnn` scheme; `EV-021/022/023` collide with appendix A's Storm rows; internally, §1 declares §2.2 → `EV-101…` then labels those rows `EV-021…023` | appendix A's flat `EV-001…EV-083` (ADR §9 R1, R43) | Renumber with §2.1's map; drop the "appendix A adopts these ids" sentence; keep the `EV-312` request as a row appendix A must add |
| N-02 | `02-schema.md` | high | §2.13, §9 sources | Cites retired ledger ids "§2.6 row `FZ8`", "§2.8 rows `X1`/`X3`/`X4`" | `EV-048`; `EV-067`/`EV-069`/`EV-070` | Replace with `EV-nnn` |
| N-03 | `06-imaging.md` | high | §5.4 (`__dependencies`), §6.4 (motion memory) | Cites retired ids "ledger row `CPU8`", "ledger row `MB5`" | `EV-031`; `EV-075` | Replace with `EV-nnn` |
| N-04 | `10-build-dependencies-testing.md` | high | §0.6, §1.5, §5.5, §11 | Cites `A2.10-B1/B2/B5`, `A2.1-E8`; §11 claims the rename is still owed | `EV-076`, `EV-077`, `EV-080`, `EV-008` | Replace and close the owed-edit row |
| N-05 | `11-roadmap.md` | high | §1 note, §2.1–§2.8, §9 sources | Cites `A2.n-Xn` ids throughout | `EV-nnn` per §2.1's map | Replace; the §9 sources row becomes a list of `EV-` rows |
| N-06 | `12-risks-decisions-open-questions.md` | high | §1 preamble, §1–§4, §6 | Cites `A2.n-Xn` ids and states appendix A "still carries its per-subsection ids" | `EV-nnn`; appendix A carries `EV-001…EV-083` today | Replace and delete the preamble caveat |
| N-07 | `05-static-curves-and-deformation.md` | high | §0.2, §0.4 and every MEASURED row | No `EV-` handles; says appendix A has not adopted R43's numbering | `EV-nnn` | Add the handle to each MEASURED row; delete the caveat |
| N-08 | `07-look-maps-expressions.md` | high | §0.3, §0.4 | Same: "gain their `EV-nnn` handles when appendix A assigns them" | `EV-019`–`EV-023`, `EV-033`, `EV-034`, `EV-067`–`EV-070` | Add handles; delete the caveat |
| N-09 | `08-tools.md` | high | §0.4, §5.2 (×2), §8.3 | Ships the literal placeholder "`appendix-A-evidence-ledger.md` **EV-nnn**" | `EV-002` (sparse edit), `EV-022`/`EV-020`/`EV-021` (upload and draw), `EV-050` (freeze author) | Fill in the row ids |
| N-10 | `01-architecture.md` | high | §0.3, §4.2, §5 | No `EV-` handles anywhere; cites report sections for numbers ADR §9 R41 says it must *cite from* `09-…` §0.2 | `09-…` §0.2 plus `EV-nnn` | Replace the restated ledger with a citation and per-number `EV-` handles |
| N-11 | `03-execution-engine.md` | medium | §12 owed-edit register, row D | Asserts "`grep -c 'EV-0' appendix-A-evidence-ledger.md` = 0" and "09 still cites `Q2`, `Q4`, `Q5`, `Q9`, `Q13`" | appendix A now carries 83 `EV-` rows; 09 no longer uses `Q<n>` | Rewrite row D as "adopt `EV-nnn`", and adopt them in §0.3–§0.4 |
| N-12 | `01-architecture.md` | blocking | §0.3 ledger table | A second frame ledger differing from `09-…` §0.2 in five rows: rig 1.35–1.50, routing ~0.05, interleave 0.25–0.5, total 0.8–1.2, frame ~16 | `09-…` §0.2: 1.35 / 0.04 / 0.25 / 0.8–1.0 / 15.8–16.0 | Replace the table with a pointer to `09-…` §0.2 and keep only the three readings |
| N-13 | `01-architecture.md` | blocking | §0.3 first line | "1920x1080-class viewport" while every row and every underlying measurement is 1280×720; `09-…` §9 forbids quoting at another resolution until S-1 | **1280×720** | Change the target line to 1280×720 and note that 1080p is gate S-1's second baseline |
| N-14 | `03-execution-engine.md` | high | §0.1 item 1 | "0.8–1.2 ms of a ~15 ms frame — roughly 7 %" | 0.8–1.0 ms of a 15.8–16.0 ms frame (`09-…` §0.2) | Restate from 09 §0.2 |
| N-15 | `03-execution-engine.md` | high | §5.5 constants, §6.3 step 2 | `R ≈ 0.05 ms`; `I ≈ 0.25–0.5 ms`; `P ≈ 0.02 ms … at 64 prims`; "≈ 0.05 ms for **64 tiles**" | `R = 0.04 ms`; `I = 0.25 ms` at 100 k (0.0255 ms/MB); publish and extent quoted at **49 tiles** (ADR §9 R21) | Correct the four constants; 64 tiles is the 1 M row |
| N-16 | `06-imaging.md` | high | §7.6 (extent channel) | "0.05 ms for **64 tiles**: DERIVED" | 0.05 ms at **49 tiles**, **ASSUMPTION** (`09-…` §0.2) | Correct count and tag |
| N-17 | `appendix-A-evidence-ledger.md` | medium | §2.11 routing row | "usdGen dirty routing + commit bookkeeping ≈ **0.05 ms**" while its own arithmetic (0.2 µs × ~200) and `09-…` §0.2 give 0.04 ms | **0.04 ms** (`09-…` §0.2 wins by the row's own rule) | Change to 0.04 ms |
| N-18 | `01-architecture.md`, `08-tools.md` | low | `01-…` §0.3; `08-…` §5.2, §5.4 | 60 Hz budget written 16.7 ms; every derived threshold in `09-…` (`N_max`, S-1 ≤ 12.8 ms) uses 16.6 ms | **16.6 ms** | Use 16.6 ms wherever the number feeds arithmetic |
| N-19 | `11-roadmap.md` | high | §2.3, S-2 anchor | "9.6 MB at 100 k gives **≈ 1.25 ms**, DERIVED from `A2.3-ST4`" — the per-MB-rate method the ledger explicitly does not use | **≈1.44 ms** (`09-…` §0.2, §2.1) | Restate as 1.44 ms and cite `09-…` §2.1's note on the two interpolation methods |
| N-20 | `appendix-A-evidence-ledger.md` | blocking | §2.10 `EV-010` | Prints "`extract_vtarray_cow_ms 0.00000–0.0001`" and "outstanding reference **2.32 ms** vs **1.72–1.97 ms**", calling §4 of the report "the printed source" | `prototypes/data-plane-benchmark/results_main.txt:95-101` prints `0.0000–0.0003`; `:102` prints `3.10`; `1.97` is in no carried output | Publish the carried values, record the report's run as a second observation, and tag the CoW-detach delta **UNMEASURED / disputed** |
| N-21 | `01-architecture.md` | high | §0.3 note and §4.2 | "publish is free (**0.00005 ms** MEASURED)" and "**2.32 vs 1.97 ms** for 9.6 MB (MEASURED)" | 0.0000–0.0003 ms MEASURED; the detach delta is disputed (N-20) | Requote from the carried output and drop the MEASURED tag on the detach |
| N-22 | `00-request-and-scope.md` | high | §5.2 "Chunk vs tile" | `nTiles = clamp(ceil(nChunks / chunksPerTile), 32, 256)` — the `min(nChunks, …)` term is missing, the exact defect appendix A §6 K24 records | `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` (ADR §9 R21) | Add the `min` term |
| N-23 | `01-architecture.md` | low | §5, invariant I5 | "512-curve chunks at 1 M curves are **1 953** chunks" | **1 954** = `ceil(1000000/512)` (`03-…` §1.4) | Correct the count |
| N-24 | `12-risks-decisions-open-questions.md` | medium | §3 RK-07, §7 Q-02, §6 gate request | A "1 / 32 / 128 / **512**-tile" sweep at 100 k × 8 CV — impossible: `nTiles` is clamped to 256 and `nChunks` is 196 at 100 k | **1 / 32 / 128 / 196 tiles** (`09-…` §5.3, gate S-12) | Change 512 → 196 in all three places |
| N-25 | `03-execution-engine.md` | high | §7 memory table | Per-curve arrays costed at `(4+4+8+36)` B = 52 B → 5.2 MB, using a 4-byte `curveId` against ADR §9 R12's `uint64`; capture caches 8.0 MB against `09-…` §2.6's 3.6 MB; total ≈80 MB vs 77–86 MB | **60 B/curve → 6.0 MB**; the two accountings must agree or the difference must be stated (`09-…` §2.6) | Widen `curveId` to 8 B and reconcile the capture-cache line with `09-…` §2.6 |
| N-26 | `06-imaging.md` | low | §8 (HUD line) | HUD example ends "gen 1042 **89 MB**" | **86 MB** (`09-…` §6.1's identical example line; the top of §2.6's 77–86 MB model) | Change to 86 MB |
| N-27 | `11-roadmap.md` | medium | §2.4, E-5 exit | "MEASURED baseline 668 MB with per-node caching (**293 MB for buffers alone**)" | `EV-007`: 293 MB is the **one-shared-buffer** configuration | Restate as "293 MB with a single shared buffer" |
| N-28 | `appendix-A-evidence-ledger.md` | medium | §2 preamble and `EV-011` | The preamble says ranges are min–max; `EV-011`'s "3.80–8.32 ms" is min–median (raw max 13.979) | min 3.80 / median 8.32 / max 13.98 (`results_main.txt:18`) | State the statistic, or widen the range |
| N-29 | `09-performance-and-benchmarks.md` | high | §1 evidence table, `EV-011/EV-013` row | "VDF … 100 k … **3.80–10.4** ms" — 10.364 is the *cv-layout* median from a different probe section | 3.80 min / 8.32 median / 13.98 max for the strip layout (`results_main.txt:18`) | Correct the cell and name the layout |
| N-30 | `appendix-A-evidence-ledger.md` | medium | §2.5 `EV-037` | Quotes the report's notice-cascade run (0.005 / 0.007 / 0.061 / 0.21 / 1.13 ms) while `appendix-B` quotes the carried `noticeCost.txt` run (0.0030 / 0.0108 / 0.0279 / 0.1977 / 1.2376), up to 2.2× apart, with neither saying so | Name the run; the ≈0.2 µs/entry conclusion holds for both | Record both runs in the Caveats column |
| N-31 | `appendix-A-evidence-ledger.md` | medium | §2.0 legacy handle map | A range map, not a row map: `EV-040…EV-054` is 15 rows against 14 `FZ` names, so `02-…`'s "row `FZ8`" and `11-…`'s `A2.6-FZ5/FZ7/FZ10` cannot be resolved mechanically; the right-hand column describes a `Q<n>` scheme `09-…` no longer uses | A row-for-row map (`FZ8 → EV-048`, `FZ5 → EV-045`, `FZ7 → EV-047`, `FZ10 → EV-050/EV-051`) | Replace the range map with per-row entries; drop the retired `Q<n>` column |
| N-32 | `09-performance-and-benchmarks.md` | blocking | §5.3 | Mints `S-11` for the `head1M` draw record and `S-12` for the tile sweep, while `12-…` and appendix A both use `S-11` for the tile sweep | `09-…` §5 owns gate ids (ADR §9 R40): **S-11 = head1M draw record (M7); S-12 = tile/prim-count sweep (M1)** | Keep 09's assignment; add a one-line note that `S-11` was previously proposed for the sweep |
| N-33 | `12-risks-decisions-open-questions.md` | blocking | §3 RK-07, §6, §7 Q-02 | Uses `S-11` for the tile-count sweep and `SI-10` for session arbitration | **S-12** for the sweep (`09-…` §5.3); session arbitration needs an id `09-…` registers and appendix A does not already claim | Renumber the sweep to `S-12`; agree one id for session arbitration with `09-…` and appendix A |
| N-34 | `appendix-A-evidence-ledger.md` | high | §5 preamble ("Collision to fix elsewhere") and U1, U13 | Mints `S-11` for the sweep and `SI-10` for the `reorder nameChildren` check, and states `12-…` uses `SI-9`/`S-10` for gates it no longer uses | `S-12` for the sweep; `SI-10` must be one gate, not two | Rewrite the paragraph against `09-…` §5 as it stands and against `12-…`'s current ids |
| N-35 | `09-performance-and-benchmarks.md` | medium | §5.2 `SI-5`, `SI-9`; §5.3 `S-6`; §5.5 | `SI-5` at **M0** and `S-6` at **M2** contradict ADR §9 R40 (both M1) and `10-…` §6.5; `SI-9` merges the session-agreement metric with the pruning-wrapper cost R40 defines it as; `SI-10` (minted by appendix A §5, requested by `12-…`) is unregistered | R40: `SI-5` M1, `S-6` M1, `SI-9` = pruning-wrapper cost only | Move `SI-5` to M1 (note "first green in M0"), `S-6` to M1, split `SI-9`, register `SI-10` |
| N-36 | `00-request-and-scope.md` | high | §5.4 gate-id paragraph | The family list stops at `SI-8`, `S-9`, `L-1`: no `B-`, no `SI-9`/`SI-10`, no `S-10`/`S-11`/`S-12`, no `L-2…L-5`, no `T-EXPR-1`/`T-PTEX-1` | ADR §9 R44 requires the `L-`, `B-`, `SI-6…SI-9` and `S-10` additions; `09-…` §5 is the registry | Regenerate the paragraph from `09-…` §5 |
| N-37 | `appendix-A-evidence-ledger.md` | high | §2.6 | No row for the freeze report §3 measurement "session layer, static (author only, no file) **0.4 ms / 0 bytes**", which `05-…`, `08-…` and appendix B all quote as MEASURED | Add the row; it is a third probe alongside `EV-041` (C++) and `EV-042` (Python) | Add `EV-0nn` with the report §3 citation |
| N-38 | `08-tools.md` | medium | §7.3 | "authoring a freeze into the session layer is **0.4–0.52 ms**" splices the §3 probe (0.4) onto the §1.3 Python probe (0.51/0.52) — the conflation `EV-042` explicitly forbids | Quote one probe per number: 0.4 ms (author only, no file), 0.25–0.27 ms (C++), 0.51–0.52 ms (Python) | Split the range and name each probe |
| N-39 | `08-tools.md`, `09-performance-and-benchmarks.md`, `10-build-dependencies-testing.md` | medium | `08-…` §8.3; `09-…` §4.3, §5.4; `10-…` §5.2, §6.5 | Three numberings of `docs/workstation-protocol.md`: R-1 = §5 (09) vs §7 (10); R-2 = §2 (09) vs §3 (10); R-3 = §3–4 (09) vs §5 (10); `08-…` appends `W1–W4` as "items 6–9" of 09's five-item list | One numbering, owned by `10-…` §5.2 (it defines the file) and cited by 09 §5.4 and 08 §8.3 | Adopt `10-…` §5.2's §§1–8 and re-point `09-…` §5.4 and `08-…` §8.3 |
| N-40 | `10-build-dependencies-testing.md` | medium | §6.5 gate→test map | Omits `S-11`, `S-12`, `SI-10`, `T-EXPR-1`, `T-PTEX-1`; its "rows that differ from 09 §5" list (T-5, T-INST-1, S-4, S-6, E-3, E-8, SI-5) is stale — only `S-6` and `SI-5` still differ | `09-…` §5 as reconciled by R40 | Add the five missing rows; trim the divergence list to `S-6` and `SI-5` |
| N-41 | `06-imaging.md` | low | §9 gate table, S-4 row | States "`09-…` §5.3 and `10-…` §6.5 still say M3 — reconcile to R40"; both now say M2 | S-4 is M2 everywhere | Delete the stale note |
| N-42 | `11-roadmap.md` | medium | §2.4 exit gates | "the 5-node chain at 1 M × 8 CV ≤ 10 ms" is a threshold `09-…` §5.1's E-1 row does not carry (it records 1 M at 8 threads as UNMEASURED) | `09-…` §5 is the single registry (R40) | Register the sub-criterion in `09-…` §5.1 or drop it from `11-…` |
| N-43 | `02-schema.md`, `04-operators.md`, `06-imaging.md`, `11-roadmap.md` | blocking | `02-…` §0.3, §5.4; `04-…` §0.4, §10; `06-…` §7; `11-…` §3.3, §6.4 | Cite `esfUsd/stageData.cpp:360`, a comment line | **`:361`** (`if (!UsdPrimDefaultPredicate(resyncedPrim)) {`), with `:351` for `GetPrimAtPath` — verified this pass and already used by six siblings and appendix A | Change `:360` → `:361` |
| N-44 | `00-request-and-scope.md`, `01-architecture.md`, `11-roadmap.md` | medium | `00-…` §3.3, §9; `01-…` §7.2; `11-…` §1.2, §9 | Cite `hdSt/renderDelegate.cpp:701-707`; `:701` is `materialBindingPurpose`, not the render-context list | **`:695-707`** (ADR §9 R43; the assignment is `:702-707`) | Change the range |
| N-45 | `07-look-maps-expressions.md` | medium | §4.5, §10 | Cites `hdSt/renderDelegate.cpp:701-710` (×2) and `:709` | **`:695-707`**; `shaderSourceTypes = materialRenderContexts` is `:710` | Change the range; cite `:710` for the shader-source-types claim |
| N-46 | `02-schema.md` | high | §0.4 | Capture per enable/disable gesture given as "**10–300 ms**", citing `judge-artist.md` §3 | **10–150 ms, ASSUMPTION** (ADR §9 R42; appendix A §2.11; `01-…`, `04-…`, `06-…` all use it) | Restate as 10–150 ms; keep the §3 quote as the upper-bound rationale if wanted |
| N-47 | `12-risks-decisions-open-questions.md` | low | §2, X-07 | Tags the same 10–150 ms capture cost **DERIVED** | **ASSUMPTION** (no probe exists; ADR §9 R42) | Change the tag |
| N-48 | `01-architecture.md` | medium | §0.3 rows 5 and 6 | Per-tile `extent` tagged **DERIVED** (canonical ASSUMPTION) and the interleave row's gate given as **E-1** (canonical S-2) | `09-…` §0.2 | Correct both |
| N-49 | `00-request-and-scope.md` | high | §2.1 version table | Puts `UsdGenPtexMap`, `UsdGenInstance` (cards/archives/spheres) and motion profile P1 in **v2** | ADR §9 R38: v1 = the M0–M7 deliverable set, including `UsdGenPtexMap` (M4), `UsdGenInstance` (M6) and P0/P1/P2 (M7); R44 requires 00's version table to match. `02-…` §2.10, `04-…` §2.3, `07-…` §6, `11-…` §6.1 all say v1 | Rewrite the v1/v2 rows against R38 |
| N-50 | `00-request-and-scope.md` | medium | §6.1 document map; §5.2 | Calls the engine invariants "**P1–P8**" and cites "(P5)", "(P3, ADR §4.1)", "(P4)", "(S17, P7)" | **I1–I8** (ADR §9 R1); `P0/P1/P2` are motion profiles only | Rename to `I1`–`I8` |
| N-51 | `06-imaging.md` | low | §8 (env table) | "`10-build-dependencies-testing.md` §7.2 owns the single env-var registry" | The registry is `10-…` **§3.5** (ADR §9 R35; `09-…` §6.1 cites §3.5 correctly) | Fix the section number |
| N-52 | `05-static-curves-and-deformation.md` | low | §8 | Cites a sibling document by line number ("`03-…` §6.3's aside (`:1211`)") | Appendix A §0.3: siblings are cited by section, never by line | Cite `03-…` §6.3 only |

---

## 9. Testing

What proves this document's content is a docs-lint job, not a runtime tier. Three checks, all
tier **T0**, all runnable before M0 closes; they belong to gate **B-1**'s job in CI
(`10-build-dependencies-testing.md` §6.2) because they need no USD and no GPU.

| What is proved | Tier | Mechanism |
|---|---|---|
| Every `EV-nnn` cited in any `plan/*.md` exists in `appendix-A-evidence-ledger.md` §2, and no document cites a retired handle (`A2.n-Xn`, `Q<n>`, `FZ*`, `ST*`, `CPU*`, `TL*`, `MB*`, `X<n>`, or the literal string `EV-nnn`) | **T0** | `check_evidence_handles` — grep each handle, resolve against §2, fail on a miss. Closes N-01…N-11 |
| Every number appendix A §2.11 lists appears in siblings only with a `MEASURED` / `DERIVED` / `UNMEASURED` / `ASSUMPTION` token within the same table cell or sentence | **T0** | appendix A §7's existing grep check, extended with the figures this pass added (`0.04 ms`, `1.44 ms`, `49 tiles`, `60 B/curve`, `16.6 ms`, `0.0003 ms`) |
| Every gate id in any `plan/*.md` appears in `09-…` §5 with the same tier and milestone, and no id names two gates | **T0** | `check_gate_registry` — parse `09-…` §5, diff against every other document. Closes N-32…N-36, N-40, N-42 |
| Every `file:line` in `plan/*.md` still resolves to the quoted symbol in `<openusd-src>` and `<usdrig-src>` | **T0** | appendix A §7's `check_citations` script, run over all fifteen documents rather than appendix A alone. Closes N-43…N-45 |
| Every value in §1's canonical table appears identically in each document that quotes that quantity | **T0** | `check_canonical_numbers` — a table-driven grep seeded from §1 above |

---

## 10. Out of scope

This pass does not re-decide anything: not the tile arithmetic (ADR §9 R21), not the gate
thresholds (`09-…` §5), not the version split (R38), not the schema (`02-…`). It does not review
prose, structure, naming or completeness — only numbers, tags, evidence handles and `file:line`
citations. It does not check facts that carry no number (behaviours, contracts, API shapes) except
where a `file:line` backing a number was wrong. It proposes no new measurements: every UNMEASURED
quantity already has a gate, and where one did not (`SI-10`, the CoW-detach delta) the finding says
so rather than minting a gate here. It edits no plan document — every fix is filed against the file
that must change.

---

## 11. Sources

**Plan documents read in full for numbers, tags and citations:** `00-request-and-scope.md`,
`01-architecture.md`, `02-schema.md`, `03-execution-engine.md`, `04-operators.md`,
`05-static-curves-and-deformation.md`, `06-imaging.md`, `07-look-maps-expressions.md`,
`08-tools.md`, `09-performance-and-benchmarks.md`, `10-build-dependencies-testing.md`,
`11-roadmap.md`, `12-risks-decisions-open-questions.md`, `appendix-A-evidence-ledger.md`,
`appendix-B-prototype-inventory.md`.

**Design:** `design/adr-v1.md` §1 (amendments), §7 (milestones and gates), §8 (document map) and
**§9 R1, R2, R12, R13, R21, R26–R28, R35, R37–R45** (the rulings every finding is measured
against); `design/brief-v1.md` §2 (S1–S46).

**Carried prototype output used as the tie-break:**
`prototypes/data-plane-benchmark/results_main.txt:18, 27-29, 39, 52-54, 60-102` and
`results_scale.txt:6-12`; `prototypes/evaluation-scheduling/noticeCost.txt`;
`prototypes/storm-throughput/PROBE_OUTPUT.txt` (via `appendix-B-prototype-inventory.md` §9).

**Reports consulted for the disputed rows:**
`research/G-data-plane-engine-prototype-benchmark.md` §0, §3.3–3.4, §4–§6, Key facts;
`research/G-storm-hair-look-prototype.md` §5 and Key facts;
`research/G-storm-throughput-and-prim-granularity.md` §1.6–§1.8;
`research/G-chain-order-probe.md` §5 and Key facts;
`research/G-evaluation-scheduling-and-batching.md` §5, §9;
`research/G-freeze-bake-undo-and-frozen-reentry.md` §1.3, §3, §4.1, §4.3;
`research/A8-seexpr-ptex-libs.md` §1.6, §2.8; `research/ENVIRONMENT.md` with CORRECTIONS.

**OpenUSD 26.08** at `<openusd-src>` (v26.08), re-read this pass:
`pxr/exec/esfUsd/stageData.cpp:345-365`; `pxr/imaging/hdSt/renderDelegate.cpp:690-715`;
`pxr/base/vt/array.h:36-56, 940-950, 980-990, 1018-1028`;
`pxr/imaging/hdSt/basisCurves.cpp:286-345, 925-940, 970-995`;
`pxr/imaging/hd/changeTracker.cpp:952-982`; `pxr/imaging/hdSt/vboMemoryManager.cpp:590-630`;
`pxr/imaging/hdSt/primUtils.cpp:880-915`; `pxr/imaging/hd/sceneGlobalsSchema.h:34-50`;
`pxr/base/ts/types.h:26-40`; `pxr/usd/usd/attribute.h:548-572`;
`pxr/imaging/hd/renderIndex.cpp:198-218`; `pxr/imaging/hd/mergingSceneIndex.cpp:165-250`;
`pxr/imaging/hd/sceneIndexPluginRegistry.h:24-32, 91-95, 142, 184` and
`sceneIndexPluginRegistry.cpp:1190-1205`; `pxr/usdImaging/usdImagingGL/engine.cpp:145-205, 2310-2355`;
`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:505-525`;
`pxr/imaging/hdSt/basisCurvesComputations.h:186-250`;
`pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:14-48` and `.cpp:690-735`;
`third_party/renderman/plugin/hdPrman/renderDelegate.cpp:800-812`;
`cmake/defaults/Options.cmake:25-40`; `pxr/usd/usd/usdGenSchema.py:205-210, 244-248`;
`$USD/pxrConfig.cmake:15-19`.
