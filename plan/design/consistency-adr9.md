# Cross-document consistency pass — ADR §9 conformance (R1–R45)

Date: 2026-09-05. Status: review report (plan v1, pre-publication).

This report is the cross-document consistency pass over the fifteen plan documents in
`plan/`, read against the binding addendum `design/adr-v1.md` §9 (rulings **R1–R45**). It
records every sentence, table row, code block or example that contradicts a ruling, quotes the
offending text, names the ruling, gives the canonical value with the source that makes it
canonical, and names the edit. It changes no plan document.

Reads with: `design/adr-v1.md` (§9 is binding over every earlier section), `design/brief-v1.md`
(S1–S46), and the fifteen documents listed in §0.2.

Method: every document was read in full; every ruling R1–R45 was additionally checked by grep
across all fifteen files; every OpenUSD `file:line` cited in a disputed passage was re-grepped in
`/home/burkard/work/OpenUSD`.

---

## 0. Summary

### 0.1 The five systemic defects

| # | Defect | Ruling | Documents affected |
|---|---|---|---|
| D1 | **Two incompatible evidence-ledger numbering schemes.** `appendix-A-evidence-ledger.md` carries a flat `EV-001…EV-083` with a §2.0 map of the retired `A2.n-Xn` ids. `09-performance-and-benchmarks.md` §1 declares a *different* 100-block scheme (§2.1 → `EV-001…`, §2.2 → `EV-101…`, §2.3 → `EV-201…`, …) and cites 33 `EV-nnn` handles that do not exist in appendix A; `EV-001` means different measurements in the two documents. Four further documents still cite the retired `A2.n-Xn` handles, and three cite bare family ids (`X1`, `FZ8`, `CPU7`). | R1, R42, R43 | 02, 06, 07, 09, 10, 11, 12 |
| D2 | **Stale reconciliation notes.** Nine documents carry "still says / must be edited / verified absent" notes against siblings that already conform. Every one of them now asserts a falsehood about a sibling and directs an implementer to make a wrong edit. | R45 | 03, 04, 05, 06, 07, 08, 09, 10, 11, 12, appendix A |
| D3 | **Gate-registry divergence.** `09-…` §5 is the single registry (R40) but itself violates R40 on **SI-5** (M0, must be M1) and **S-6** (M2, must be M1), broadens **SI-9** beyond R40's definition, and mints **S-11 / S-12** with meanings that `12-…` and `appendix-A` contradict (both use **S-11** for the tile sweep; `12-…` and `appendix-A` mint **SI-10** for two different checks). | R40 | 00, 05, 06, 09, 11, 12, appendix A, appendix B |
| D4 | **Frame-ledger restatement.** R41 makes `09-…` §0.2 the only frame ledger and requires siblings to cite it. `01-…` §0.3 restates it with six rows whose values differ; `03-…` §0.2 quotes the superseded `0.8–1.2 ms` aggregate. | R41 | 01, 03 |
| D5 | **v1/v2 boundary not applied in `00-…`.** R38 makes v1 = the M0–M7 deliverable set. `02-…` and `04-…` conform; `00-…` still lists `UsdGenInstance`, `UsdGenPtexMap` and motion profile P1 as **v2** and records `Scatter mode = uniform` as "unscheduled / not agreed". | R38, R44 | 00 |

### 0.2 Findings per document

| Document | Findings | Highest severity |
|---|---:|---|
| `00-request-and-scope.md` | 8 | **high** |
| `01-architecture.md` | 3 | **high** |
| `02-schema.md` | 4 | medium |
| `03-execution-engine.md` | 5 | **high** |
| `04-operators.md` | 3 | medium |
| `05-static-curves-and-deformation.md` | 2 | medium |
| `06-imaging.md` | 5 | **high** |
| `07-look-maps-expressions.md` | 6 | **high** |
| `08-tools.md` | 4 | medium |
| `09-performance-and-benchmarks.md` | 6 | **high** |
| `10-build-dependencies-testing.md` | 4 | medium |
| `11-roadmap.md` | 4 | medium |
| `12-risks-decisions-open-questions.md` | 5 | **high** |
| `appendix-A-evidence-ledger.md` | 5 | medium |
| `appendix-B-prototype-inventory.md` | 2 | low |

---

## 1. Canonical values this report enforces

Where two documents disagree, this is the value that ships. Each row names the ruling or the
registry that makes it canonical.

| Subject | Canonical value | Made canonical by |
|---|---|---|
| Engine principles | **I1–I8** ("invariants"); `P0/P1/P2` are motion profiles only; M0 pre-work items are `PW-1…PW-n` | ADR §9.1 R1 |
| Evidence-ledger handles | `appendix-A-evidence-ledger.md`'s flat **`EV-001…EV-083`**; §2.0 maps the retired `A2.n-Xn` ids. No `A2.n-Xn`, no bare `X1`/`FZ8`/`CPU7`, no `Q<n>`, no `EV-1nn…EV-9nn` | ADR §9.1 R1, §9.5 R42, R43; appendix A §2.0, §5 (K25) |
| Python package | **`usdgen`** (all lowercase), containing `_usdGen`, `usdGenLib.py`, `usdGenUsdview.py` and the UI modules, installed at `lib/python/usdgen/`. `usdGenUsdview` is a **CMake target**, not a package directory. `usdGenPy` is not a name | ADR §9.1 R5; `08-tools.md` §1.1; `10-…` §1.1, §1.5 |
| Tile arithmetic | `chunksPerTile = max(1, ceil(nChunks / tileTarget))`; **`nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))`**. 100 k curves / chunk 512 / `tileTarget` 64 → 196 chunks, 4 per tile, **49 tiles**; **64 tiles is the 1 M-curve figure** | ADR §9.3 R21 |
| Commit trigger (c) | **Always applies**, with or without an app driver; commits synchronously at the end of the `_PrimsDirtied` batch. The app never calls `Commit()` for a stage edit | ADR §9.4 R32 |
| Frame ledger | **`09-…` §0.2 only**: rig 1.35 ms (MEASURED); routing 0.04 ms; deformed tail 0.4–0.6 ms; interleave 0.25 ms; extent 0.05 ms; publish 0.02 ms; **usdGen total 0.8–1.0 ms**; Storm draw ~12.2 ms; upload ≈1.44 ms; **frame total 15.8–16.0 ms**. 60 Hz at 100 k × 8 CV is **not established** | ADR §9.5 R41; `09-…` §0.2; appendix A §2.11 |
| Gate tiers/milestones | `09-…` §5 as reconciled by R40, including **SI-5 T1 M1** and **S-6 T2 M1** | ADR §9.5 R40 |
| Gate id for the tile-count sweep | one id, registered in `09-…` §5. `09-…` currently uses **S-12**; `12-…` and appendix A use **S-11**. `S-11` is also 09's `head1M` draw record. **One of the two must move** | ADR §9.5 R40 |
| Gate id for two-index session arbitration | one id registered in `09-…` §5, **distinct from SI-9** (R40 binds SI-9 to the pruning-wrapper cost) and distinct from appendix A's `SI-10` (`reorder nameChildren`) | ADR §9.5 R40, R28 |
| Ramp interpolation tokens | `linear \| catmullRom \| bspline \| constant`, default **`catmullRom`** | ADR §9.2 R11; `02-schema.md` §2.17, §2.14 |
| `curveId` | `uint64[] primvars:usdGen:curveId`; every C++ parameter that takes a curve id is `uint64_t` | ADR §9.2 R12; `02-schema.md` §5 |
| Density-scale dirty class | **value** — the scales never re-scatter and never re-chunk; only `UsdGenChunkDesc::liveCount` moves | ADR §9.2 R13 (over `02-schema.md` §6.4's current *capture*) |
| Mask block | `02-schema.md` §2.13's names, tokens **and arithmetic**; `04-…` §5.2 must be term-for-term identical to it | ADR §9.2 R16 |
| Schema plugin | `usdGenSchema` plugInfo is a **`"Type": "library"`** plugin whose `LibraryPath` is **`libusdGenSchema.so`** — never a resource-only plugin | ADR §9.2 R19 |
| C ABI | **`08-tools.md` §1.4 only**; `06-…` §3.8 repeats it verbatim (it already does) | ADR §9.4 R31 |
| `TfDebug` codes | exactly six: `USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`, `USDGEN_CAPTURE`, `USDGEN_MEMORY` | ADR §9.4 R35; `10-…` §7.2 |
| Env-var registry | `10-…` §3.5 only; every variable any document names must appear there under one spelling | ADR §9.4 R35 |
| v1 / v2 | **v1 = M0–M7**, incl. `UsdGenPtexMap` (M4), `UsdGenInstance` (M6), motion profiles P0/P1/P2 (M7). **v2 = M8**, incl. `Scatter` mode `uniform`, progressive generation (T-5), in-place overlay (S-10) | ADR §9.5 R38 |
| Storm render-context source line | `pxr/imaging/hdSt/renderDelegate.cpp:695-707` | ADR §9.5 R43 |
| Scene-globals callback source line | `pxr/usdImaging/usdImagingGL/engine.cpp:148-201` | ADR §9.5 R43 |
| `esfUsd` resync predicate | `pxr/exec/esfUsd/stageData.cpp:361` (re-verified: `if (!UsdPrimDefaultPredicate(resyncedPrim))`; `GetPrimAtPath` at `:351`) | `12-…` §0.2 S41, `08-…` §9.4, `10-…` §4.6 — the plan's own verification |
| `VtArray` uniqueness | `VtArray::IsUnique()` **does not exist**; the private `_IsUnique()` is at `pxr/base/vt/array.h:1023`, behind the class-scope `private:` at **`:984`** (re-verified) | ADR §9.3 R20 |

---

## 2. `00-request-and-scope.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 00-1 | §4.2 table, `:315` | "the **`usdGenPy`** facade package (ADR §6)" | **R5** | The package is `usdgen` (lowercase); the `rigexec.Builder` facade is `usdgen/builder.py` (`10-…` §1.5) | Replace with "the `usdgen` package's `builder.py` facade (ADR §9.1 R5)". This is the last live `usdGenPy` in the plan |
| 00-2 | §5.2 "Chunk vs tile", `:384` | "`nTiles = clamp(ceil(nChunks / chunksPerTile), 32, 256)`" | **R21** | `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` | Add the `min(nChunks, …)` term; every other document already carries it |
| 00-3 | §6.1 doc map, `:456` | "`01-architecture.md` \| Three-part shape, frame ledger, **invariants P1–P8**, contracts…" | **R1** | Engine principles are **I1–I8**; `01-…` §6 already uses I1–I8 | Rewrite as "invariants I1–I8" |
| 00-4 | §5.2 glossary, `:379`, `:388`, `:392`, `:404` | "(S25, **P4**)", "ADR §1 (**P5**)", "(**P3**, ADR §4.1)", "Nothing cooks in `GetPrim` (S17, **P7**)" | **R1** | `I4`, `I5`, `I3`, `I7` | Renumber all four to the `I` family |
| 00-5 | §5.2 "Commit trigger", `:400-404` | "(c) an operator-parameter, map or surface-topology dirty committed synchronously inside `_PrimsDirtied` **when no driver is attached**. With (a) attached, (b) **and (c)** only set the dirty flag." | **R32** | Trigger (c) **always** applies; only (b) degrades to setting the dirty flag when (a) is attached (`01-…` §4.1, `06-…` §3.9) | Delete the "when no driver is attached" clause and remove (c) from the "only set the dirty flag" sentence |
| 00-6 | §5.4 gate-family list, `:435-444` | "**SI-** … SI-8 auto-applied `UsdGenMaskAPI`…", "**S-** Storm: S-1..S-7 …, S-8 …, S-9 …", "**L-** look: L-1 render-context resolution" — no `B-` family at all | **R44**, R2, R40 | The families are `E-`, `SI-` (through **SI-9**), `S-` (through **S-12**), `L-` (**L-1…L-5**), `T-` (incl. `T-EXPR-1`, `T-PTEX-1`), `T-INST-`, `R-`, and **`B-`** (`B-1`, the link-rule check) | Add `B-1`, `SI-9`, `S-10`, `S-11`, `S-12`, `L-2`–`L-5`, `T-EXPR-1`, `T-PTEX-1`, taking tiers and milestones from `09-…` §5 |
| 00-7 | §0.2 R4b `:91`; §2.1 v1/v2 rows `:148-150`; §2.2 `:158-161`; §2.3 `:172-179` | "ADR §6's **v2** `UsdGenInstance`"; the v2 row lists `UsdGenInstance`, `UsdGenPtexMap`, "motion profile P1 (velocities)"; "`mode = uniform` is **unscheduled**"; "the schedule of `mode = uniform` is **not agreed across the plan** — `04-operators.md` §1.2 dates all four modes M1" | **R38** | v1 = M0–M7: `UsdGenInstance` (M6), `UsdGenPtexMap` (M4), P0/P1/P2 (M7). v2 = M8: `Scatter` mode `uniform`, Curl/Bend/Straighten/Displace/Wave/Part/ExprOp, TsSpline ramps in the UI, sculpt rebase, progressive generation (T-5), in-place overlay (S-10) | Rewrite §2.1's two rows and §2.2's bullets to R38; delete the §2.3 "unresolved" paragraph. `04-…` §1.2 dates `random` M1, `atGuides` M3, `points` M5, `uniform` v2/M8, so the quoted claim about 04 is also false |
| 00-8 | §3.3 `:232`; §9 sources `:533` | "`pxr/imaging/hdSt/renderDelegate.cpp:701-707`" | **R43** | `pxr/imaging/hdSt/renderDelegate.cpp:695-707` | Re-point both citations |
| 00-9 | §4.2 table, `:311` | "the `usdGenSchema` **resource** plugin, generated by `usdGenSchema.py` in codeless mode" | **R19** | The schema classes stay codeless, but the *plugin* is a `"Type": "library"` plugin whose `LibraryPath` is `libusdGenSchema.so` (`02-…` §7.1, `10-…` §4.3, `01-…` §5.1) | Say "the `usdGenSchema` **library** plugin holding a codeless `generatedSchema.usda`" |
| 00-10 | §1.2 parity table, `:121` | "`UsdGenGuideInterpolate` emits `guideIndex[3]`/`guideWeight[3]`" | **R24** | `guideIndex` uniform `int[]` and `guideWeight` uniform `float[]`, both `elementSize = 3`; `int[3]`/`float[3]` are not USD type names | Restate with `elementSize = 3`; keep Unreal's `int32[3]`/`float[3]` only as the *Unreal* arity |

---

## 3. `01-architecture.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 01-1 | §0.3, the ten-row ledger table `:113-127` | A full restatement of the frame ledger whose values differ from `09-…` §0.2: "usdRig rig evaluation (`ArmShotAnim.usda`, **48 frames**) \| **1.35-1.50 ms**"; an extra row "Full terminal traversal, 95 prims \| +0.24 ms"; "SoA -> AoS interleave … **0.25-0.5 ms**"; "Per-tile `extent` … ~0.05 ms \| **DERIVED**"; "**usdGen total, deform frame \| 0.8-1.2 ms**"; "Storm draw … **~12 ms**"; "points upload … **~1.4 ms**"; "**Frame total \| ~16 ms**" | **R41** | `09-…` §0.2 is the only ledger: 1.35 ms (95 prims); routing 0.04; tail 0.4–0.6; interleave **0.25** (DERIVED floor); extent **0.05 (ASSUMPTION)**; publish 0.02; **usdGen total 0.8–1.0 ms**; draw **~12.2**; upload **≈1.44**; **total 15.8–16.0 ms**. Appendix A §2.11 records that 01 "still prints 0.8–1.2 ms and owes a correction" | Replace the table with a two-line pointer to `09-…` §0.2 plus the three readings §0.3 draws from it; keep no numbers of its own |
| 01-2 | §7 "Material terminals", `:706` | "`pxr/imaging/hdSt/renderDelegate.cpp:701-707`" | **R43** | `:695-707` | Re-point |
| 01-3 | §5.1 target table | "`usdGenUsdview` \| **python plugin dir** \| `lib/python/usdGenUsdview/`" | **R5** | Every module lives in the package `usdgen` at `lib/python/usdgen/`; `usdGenUsdview` is a CMake target that installs `"Type": "python"` plugin resources into `lib/python/usdgen/plugInfo.json`, registering `usdgen.usdGenUsdview.UsdGenUsdviewContainer` (`08-…` §1.1, `10-…` §1.5, `:181`) | Change the Installed cell to `lib/python/usdgen/` and the Kind cell to "plugin resources, `\"Type\": \"python\"`" |

---

## 4. `02-schema.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 02-1 | §0 preamble `:36`; §2.13 `:830`; §5 rule 5 `:1636`; §11 sources `:2173` | "the row ids used here are the ledger's current family ids, e.g. §2.8 `X1`"; "(`appendix-A-evidence-ledger.md` §2.8 rows **X1, X3, X4**…)"; "§2.6 row **FZ8**"; "§2.6 row FZ8 …, §2.8 rows X1/X3/X4" | **R1**, R43 | Appendix A rows are `EV-nnn` and are the only citation handle. `A2.8-X1/X3/X4` are `EV-067`/`EV-069`/`EV-070` (appendix A §2.0; `04-…` §5.2 already cites them that way); the `primvars:rest` +26 B row is its `EV-0nn` equivalent in appendix A §2.6 | Replace all four with `EV-nnn` handles and delete the "current family ids" sentence |
| 02-2 | §2.2 `:373`, §2.3 `:387-388`, §6.4 `:1726` | `usdGen:densityScale` / `usdGen:renderDensityScale` carry dirty class **capture**, and §6.4 places them in "Capture rows — bump the capture epoch, re-`Capture()` this node and downstream" | **R13** | The scales **never re-scatter and never re-chunk**: the chunk partition is computed once per capture over the full id set and only `UsdGenChunkDesc::liveCount` moves. The class is **value**. `03-…` §12.1 row A raises the same defect; `03-…` §1.2 implements it as value | Reclass both rows to **value**, move the §6.4 row into §6.5, and state that the id space and the chunk partition are untouched. `03-…` generates its router from this column (R25), so the shipped router is wrong until it changes |
| 02-3 | §2.13 mask block and its pseudocode `:820-828` | The block omits `lockedCurveSuppression(c)` and the `usdGen:blend` factor, and `remap` is written `(s - x) / (y - x)` with no epsilon; `04-…` §5.2 writes `clamp((s - x) / max(y - x, 1e-6), 0, 1)`, folds `lockedCurveSuppression(c)` inside the clamp and multiplies `w(c,i)` by `usdGen:blend` | **R16** | `02-…` §2.13 is canonical for names, tokens *and arithmetic*, and `04-…` §5.2 must be identical to it. Since 04's three terms are load-bearing (R7: 02 gains what a sibling needs), 02 must gain them | Fold `lockedCurveSuppression(c)` into 02's clamp, add the `max(y - x, 1e-6)` guard, and add `usdGen:blend` to `w(h,t)`; then 04 §5.2 is literally 02's block |
| 02-4 | §0.2 `:66`; §2.9 `:701` | "`esfUsd/stageData.cpp:360`" | consistency (the plan's own verification) | `pxr/exec/esfUsd/stageData.cpp:361` — `12-…` §0.2 S41, `08-…` §9.4 and `10-…` §4.6 all record `:360` as the brief's line and `:361` as the predicate call in this checkout | Re-point both. `04-…` `:91`/`:1615`, `11-…` `:264`/`:640` and appendix B `:378`/`:941`/`:1133` carry the same `:360` |
| 02-5 | §2.9 `frozen:tier` row | "S42 labels them **T1/T2/T3**; those labels are **not** the test tiers of §9" | **R1** (low) | Landing tiers are the tokens `session \| sublayer \| payload` only; S42's T1/T2/T3 labels are retired from prose | Drop the T1/T2/T3 sentence; the tokens are self-explanatory |

---

## 5. `03-execution-engine.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 03-1 | §6.3 step 2, `:1294` | "accumulate `extent/min\|max` in the same pass — ≈ 0.05 ms for **64 tiles**" (the surrounding workload is 100 k × 8 CV) | **R21** | **49 tiles** at 100 k curves / chunk 512 / `tileTarget` 64; 64 tiles is the 1 M-curve figure. `01-…` §0.3, `09-…` §0.2, `06-…` §6, appendix A §2.11 and `05-…` §5 all say 49 | Change to "49 tiles"; the claim two lines later that "`01-architecture.md` §0.3 carries the same number" is then true (01 says 49) |
| 03-2 | §0.2 goal 1, `:43` | "The expected cost is **0.8–1.2 ms** of a ~15 ms frame" | **R41** | `09-…` §0.2's aggregate is **0.8–1.0 ms**; the frame total is 15.8–16.0 ms | Quote 0.8–1.0 ms and cite `09-…` §0.2 |
| 03-3 | §0.5, `:150-154`; §12.1 row D | "that ledger carries **no `EV-` rows yet** (verified 2026-09-05), so every MEASURED cell below cites its report section and raw log directly"; "Verified: `grep -c 'EV-0' appendix-A-evidence-ledger.md` = 0, and 09 still cites `Q2`, `Q4`, `Q5`, `Q9`, `Q13`" | **R1**, R42, R45 | Appendix A carries **`EV-001`…`EV-083`**; `09-…` cites no `Q<n>` handles. `03-…`'s thirteen §0.3 rows, eight §0.4 rows and §5.5 constants therefore have their handles today | Delete the claim, add the `EV-nnn` handle to every MEASURED cell (e.g. §0.3's 1.02 ms is `EV-001`/`EV-008`, the chunk sweep is `EV-009`, the 668 MB is `EV-007`), and withdraw §12.1 row D |
| 03-4 | §12.1 "Required corrections", rows B, C, E, G, H, I | "`10-…` … must gain **`USDGEN_OP_CHECKS`** … Verified absent"; "`02-…` §2.5 `preceding` is documented as 'before usdGen's own overlays'"; "`02-…` §2.17 lists `constant \| linear \| smooth \| monotoneCubic`"; "`06-…` §3.9 (`:1017`) Trigger (c) still reads '**with no app driver attached**'"; "**E-1**'s pass criterion is `≤ 2.5 ms` at `:531`; **E-3** is M1 at `:534`; **E-8** is M0 at `:539`"; "`UsdGen::Interleave` (`:644`) must gain gate **E-1**" | **R45** | All six are already fixed in the siblings: `10-…` §3.5 lists `USDGEN_OP_CHECKS` (default 0); `02-…` §2.5 says "`preceding` is accepted in v1 as an **alias for `final`** (ADR §9 R9)"; `02-…` §2.17 lists `linear \| catmullRom \| bspline \| constant`; `06-…` §3.9 `:1110` says "Trigger (c) is unconditional (ADR §9 R32)"; `09-…` §5.1 says E-1 ≤ **1.5 ms**, E-3 **M2**, E-8 **M1**; `09-…` §6.2's `UsdGen::Interleave` row already reads "E-1, S-2" | Delete rows B(first half), C, E, G, H, I. Only two claims survive: `09-…` §6.2's `UsdGen::Compile` still omits "graph digest", and §12.1 row F (`05-…` §2.2's `std::string role`) is unverified here |
| 03-5 | §12 gate paragraph, `:1882` | "**S-1…S-10** and L-1 are the Storm gates in `09-performance-and-benchmarks.md`" | **R40** | `09-…` §5.3 registers **S-1…S-12**; L-1…L-5 are its §5.4 look gates | Update the range and the L family |
| 03-6 | §1.2 code comment, `:201-202` | "ADR §2.3 mandates integer emitted primvars (`clumpId_<level>`, **`guideIndex[3]`**)" | **R24** (low) | `guideIndex` is uniform `int[]` with `elementSize = 3`; §6.3 step 5 already says so | Rewrite the comment to match §6.3 step 5 |

---

## 6. `04-operators.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 04-1 | §5.2 pseudocode block | `curveMask[c] = clamp(combine(...) * nse(c) * region(c) * lockedCurveSuppression(c), 0, 1)`; `w(c, i) = usdGen:blend * curveMask[c] * rampLUT[...]`; `remap(s,x,y) = clamp((s - x) / max(y - x, 1e-6), 0, 1)  # 02 §2.13's exact form` | **R16** | `02-…` §2.13 is canonical for the block's arithmetic. Its current form has no `lockedCurveSuppression`, no `usdGen:blend` factor and no epsilon guard, so the comment "02 §2.13's exact form" is false today | Either land the three terms in `02-…` §2.13 (preferred — 04 §0.13 row 3 already asks for it) or drop them here. The two blocks must be term-for-term identical before C1 |
| 04-2 | §0.13 rows 4, 7, 8 | "`02-schema.md` §2.13's mask pseudocode … calls `UsdGenHash01(mask:randomSeed, curveId(h), kSaltMaskRandom)` — three arguments — while its own §2.19.1 declares `UsdGenHash01(uint64_t key, uint32_t salt)`"; "`10-…` env-var registry \| add `USDGEN_OP_CHECKS`"; "`08-tools.md` §1.4 \| carry `int UsdGenImaging_SetMaskVisualisation(const char *opPath)`" | **R45** | `02-…` §2.19.1 explicitly declares the multi-key fold-left forms and says "`UsdGenHash32 / UsdGenHash01` take the same argument lists"; `10-…` §3.5 lists `USDGEN_OP_CHECKS`; `08-…` §1.4 lists `UsdGenImaging_SetMaskVisualisation(const char *opPath)` | Delete rows 4, 7 and 8 |
| 04-3 | §0.4 `:91`; §10 sources `:1615` | "`esfUsd/stageData.cpp:360`" | consistency | `pxr/exec/esfUsd/stageData.cpp:361` (§1 canonical table) | Re-point |

---

## 7. `05-static-curves-and-deformation.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 05-1 | §11 Sources, "Defects this document records against siblings" `:1432-1437`; §2.4 `:324-327`; §2.1 `:205`; §4.2 `:550-552`; §5.3 `:846-848`; §8 `:1313` | "02 §2.6 and §2.10 (**missing R8 fold-ins**), 02 §2.9 (\"**falls back to `live`**\", contra R18), 02 §5 (**`int[]` `curveId`**, contra R12), 02 §7.3 (**`UsdGenImaging*` adapter names**, contra R3), 03 §2.2 and §3.1 (**`curves` / `UsdGenCurveDesc`**, contra R23), 04 §0.6 (the **murmur3 hash and `kDensitySalt`**, contra R12/R13), 09 §5.1 (**E-1 at 2.5 ms**, contra R27) and 09 §5.2 (**SI-9 absent**)"; "`04-operators.md` §0.6 still writes the older murmur3 form `UsdGenHash32(curveId ^ kDensitySalt)` with `kDensitySalt = 0xDEC1A7E5u`"; "`02-schema.md` §5 (line ~1392) still carries `int[]`"; "`09-…` §5.2 stops at SI-8 and does not list SI-9, and `12-…` (RK-09, `:198`) has minted SI-9 for a different check"; "`02-schema.md` §2.9's freeze table (`:565`) still reads \"the evaluator falls back to `live`\""; "09 §5.1 still carries the older 2.5 ms figure" | **R45** | Every one is false today. `02-…` §2.6 carries all five `UsdGenCurveSource` fold-ins and §2.10 both sculpt fold-ins; `02-…` §2.9's epoch row says the evaluator "**keeps rendering the frozen data**"; `02-…` §5 and §2.16 declare `uint64[] primvars:usdGen:curveId`; `02-…` §7.3 uses `UsdGenOperatorAdapter`/`UsdGenMapAdapter`/`UsdGenGroomAdapter`/`UsdGenDescriptionAdapter`/`UsdGenGuideSetAdapter`/`UsdGenRestAPIAdapter`; `03-…` §2.2 declares `std::vector<UsdGenCurveSetDesc> curveSets`; `04-…` §0.6 is the pinned SplitMix64 with `kSaltDensity`; `09-…` §5.1 says **≤ 1.5 ms** and §5.2 has an **SI-9** row; `12-…` now uses **SI-10**, not SI-9. `02-…` §2.9 is at `:687`, not `:565` | Delete the defect list and the five inline "still …" paragraphs, or re-verify each against the current sibling text. Only 05's own flag of `03-…` §6.3's "64 tiles" (finding 03-1) survives |
| 05-2 | §4.2 `:547-552` | The SI-9 collision paragraph names `12-…` RK-09's SI-9 | **R40** | R40 binds **SI-9** to the pruning-wrapper cost (T1, M2), which is what 05 uses it for — correctly. The live collision is now between `09-…` (which folded the two-index check into SI-9), `12-…` (SI-10) and appendix A (SI-10 for a *third* check) | Rewrite the paragraph to point at §1's canonical table: SI-9 = pruning-wrapper cost only; the session-arbitration check needs a new id registered in `09-…` §5 |

---

## 8. `06-imaging.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 06-1 | §9 Diagnostics, the `TF_DEBUG` row | "`USDGEN_GRAPH`, `USDGEN_CAPTURE`, `USDGEN_DIRTY`, `USDGEN_PUBLISH`, `USDGEN_SESSION`, `USDGEN_MAPS`, `USDGEN_EXPR`, `USDGEN_TIMING` — **the registry list** (`10-build-dependencies-testing.md` §7.2, R35)" | **R35** | `10-…` §7.2 declares **exactly six**: `USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`, `USDGEN_CAPTURE`, `USDGEN_MEMORY`, and explicitly retires `USDGEN_GRAPH`/`SESSION`/`MAPS`/`EXPR`/`TIMING`. `03-…` §9.2 and `09-…` §6.2 carry the same six | Replace the row with the six codes; the §3.7 line "logs one `USDGEN_SESSION` debug line" must move to `USDGEN_PUBLISH` or `USDGEN_COMMIT` |
| 06-2 | §8 "Reconciliation note", `:1717-1721` | "`02-schema.md` §7.2 and `10-build-dependencies-testing.md` §1.2 and §4.3 **still carry the pre-R19 wording** (the extent function in `usdGenImaging`, `LibraryPath` → `usdGenImaging`, `usdGenSchema` as resources only)" | **R45** | `02-…` §7.2 opens "**`libusdGenSchema.so`** — not `usdGenImaging` — carries one `TF_REGISTRY_FUNCTION(UsdGeomBoundable)`"; `10-…` §4.3 says "`ADR §9.2 R19` rules that the library is **`libusdGenSchema.so`** … and **not** `libusdGenImaging.so`" | Delete the note |
| 06-3 | §6 `:1299`; §5.1 `:1416`; §7 `:1553` | "row **CPU8**", "row **CPU7**", "row **MB5**" | **R1**, R43 | Appendix A rows are `EV-nnn`; `A2.4-CPU7/CPU8` and `A2.9-MB5` map to appendix A §2.0's `EV-0nn` ids | Replace with the `EV-nnn` handles |
| 06-4 | §10.1 gate table, S-4 row `:1788`; SI-9 note `:1799` | "M2 (R40; `09-…` §5.3 and `10-…` §6.5 **still say M3** — reconcile to R40)"; "SI-9 is an addition R40 makes and **is not yet a row** in `09-performance-and-benchmarks.md` §5.2" | **R45** | `09-…` §5.3 places S-4 at **M2**; `10-…` §6.5 places S-4 at **M2**; `09-…` §5.2 has an **SI-9** row | Delete both parentheticals |
| 06-5 | §9 Diagnostics, trace-scope row | "`UsdGen::SI-3::Commit`, `UsdGen::SI-1::Publish`, `UsdGen::S-2::Interleave`" | exact names (R40's vocabulary) | `03-…` §9.2 and `09-…` §6.2 are one table in two documents and use `UsdGen::Route`, `UsdGen::Compile`, `UsdGen::Capture`, `UsdGen::Evaluate<opType>`, `UsdGen::Interleave`, `UsdGen::Publish`, `UsdGen::Diff`, `UsdGen::Motion`. `10-…` §7.2 uses a third form, `UsdGen::E-1::Evaluate` / `UsdGen::SI-3::Commit` | Pick one spelling and use it in 03, 06, 09 and 10. `03-…` §9.2 is the declaration site and should win |
| 06-6 | §9 usdview HUD row | "`usdGen 0.9 ms  49/49 tiles  196/196 chunks  gen 1042  **89 MB**`" | low | `09-…` §6.1 prints the identical HUD line with **86 MB** | Make the two example lines identical |

---

## 9. `07-look-maps-expressions.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 07-1 | §1.1 `UsdGenLookAPI` table, `:117` | "`usdGen:look:colorRamp:interpolation` \| `uniform token` \| **`\"linear\"`** \| **`constant \| linear \| smooth \| monotoneCubic`** (`02-schema.md` §2.14)" | **R11**, R7 | `linear \| catmullRom \| bspline \| constant`, default **`catmullRom`**. `02-…` §2.14 and §2.17 already carry exactly that; the citation is to text that does not exist | Replace the default and the token set; this is the last `smooth \| monotoneCubic` in the plan |
| 07-2 | §8.4 (reload), `:1432` | "(ADR §4.3 trigger (c) **when no app driver is attached**, …)" | **R32** | Trigger (c) always applies | Delete the clause |
| 07-3 | §1.4 `:166-168`; §7 `:984` | "`inline float UsdGenHash01(int seed, **uint32_t curveId**, uint32_t salt)`" and "widening 04's `uint32_t curveId` parameter to `uint64_t`"; in the SeExpr `VarBlock` struct, "`int       curveId;                   // stable id`" | **R12** | `curveId` is 64-bit everywhere: `uint64_t` in C++, `uint64[]` in USD. `04-…` §0.6 already declares `UsdGenHash01(uint64_t key, uint32_t salt)` and `UsdGenDraw01(int seed, uint64_t curveId, uint32_t salt)` | Widen both to `uint64_t` (SeExpr's `$id` variable is a `double`, so document the lossy narrowing explicitly at the SeExpr boundary rather than storing an `int`) |
| 07-4 | §2.1 `:276`; §4.2 `:690`; §11 `:1634` | "`pxr/imaging/hdSt/renderDelegate.cpp:709`", "`:701-710`", "`:701-710`" | **R43** | `pxr/imaging/hdSt/renderDelegate.cpp:695-707` | Re-point all three |
| 07-5 | §2.4, `:513` | "rows **S35**" (a ledger citation) | **R1** | `EV-nnn` handles only | Replace |
| 07-6 | §9.2 `:1515`; §3.1 `:1084` | "That section carries L-1 today (§5.4) and **must gain the four new rows**"; "`02-schema.md` §2.12's tier column **still reads `v2`** for [`UsdGenPtexMap`]" | **R45** | `09-…` §5.4 already registers **L-1, L-2, L-3, L-4, L-5** (plus `T-EXPR-1`, `T-PTEX-1`); `02-…` §2.12 `:762` and §1 `:322` both read "**v1 (M4)**" | Delete both claims |
| 07-7 | §9.2 test names | `testUsdGenStormLook`, `testUsdGenMaterialBinding` | exact names | `09-…` §5.4 names L-1's test `testUsdGenStormMaterial` and L-2's `testUsdGenStormGolden`; `10-…` §6.5 names L-2's `testUsdGenStormLook`. Three documents, three sets | Agree one CTest name per gate; `10-…` §6.5 owns the CTest registry and is the natural canonical |

---

## 10. `08-tools.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 08-1 | §1.4, `:210-224` | "`06-imaging.md` §3.8 prints an **earlier draft** of the same ABI; where the two differ this one governs, and §3.8 **must be edited to match** before the C4 freeze review", followed by an eight-row table of "differences" (`SetInteractiveLOD` supersedes `SetLodDensity`, `BuildMirrorMap` addition, `GetTopologyGeneration` addition, `SetLiveOverrideIndexed` addition, `SetMaskVisualisation` addition, `Footprint` renamed from `FootprintCV`) | **R31**, R45 | `06-…` §3.8 `:1045-1085` is already byte-identical to `08-…` §1.4 except for two cross-reference section numbers inside comments. It carries `GetTopologyGeneration`, `SetLiveOverrideIndexed`, `BuildMirrorMap`, `SetInteractiveLOD`, `SetMaskVisualisation` and `Footprint`; it never mentions `SetLodDensity` or `FootprintCV` | Replace the paragraph with "…`06-imaging.md` §3.8 repeats this listing verbatim (R31)" and delete the difference table |
| 08-2 | §0 preamble, `:62-64` | "**Appendix A does not yet carry `EV-` rows**; adding the rows this document cites is an owed edit (§11)" | **R42**, R45 | Appendix A carries `EV-001…EV-083`. `08-…` cites **zero** `EV` handles today, so every MEASURED number in it lacks the handle R42 requires | Delete the claim and add the handle to each measured number (21.3 µs = `EV-061`, 166 µs / 1.66 ms = `EV-060`-family, 0.13–0.18 µs = `EV-055`-family, 3.45 ms author / 35.4 ms `_resetGUI` = the `A2.6-FZ10` split rows) |
| 08-3 | §5.6 `:1096` | "RigExec costs ~1.35 ms/frame on `ArmShotAnim`, leaving **~15 ms/frame for usdGen at 60 Hz** (S45)" — untagged | **R41**, R42 | Appendix A §2.11 tags this ASSUMPTION (arithmetic): `16.6 − EV-035 (1.35) − EV-036 delta (0.24)`, "per rig class; never universal, no gate", and R41 records that 60 Hz at 100 k is not established | Tag it ASSUMPTION and cite `09-…` §0.2 |
| 08-4 | §11 owed edits, `:1303`, `:1313` | "`02-schema.md` must (a) **delete `usdGen:interactive:maxCurves` at §2.3**"; "`01-architecture.md` §6 **must rename the engine principles P1–P8 to I1–I8**" | **R45** | `02-…` has no `usdGen:interactive:maxCurves` anywhere; §2.3.1 already routes the ceiling through `UsdGenImaging_SetInteractiveLOD`. `01-…` §6 is headed "Engine invariants I1-I8" | Delete both items |

---

## 11. `09-performance-and-benchmarks.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 09-1 | §1 "Measured evidence", `:112-167`, and every `EV-` citation in §0.2–§5 | "The numbering is a **100-block per appendix subsection** — §2.1 → `EV-001…`, §2.2 → `EV-101…`, §2.3 → `EV-201…`, §2.4 → `EV-301…`, §2.5 → `EV-401…`, §2.6 → `EV-501…`, §2.7 → `EV-601…`, §2.8 → `EV-701…`, §2.9 → `EV-801…`, §2.10 → `EV-901…`, §2.12 → `EV-951…` — and appendix A adopts these ids per R1"; and rows such as "**EV-001** \| `A2.1-E1` \| 5 stylers, 100 k × 8 CV, **20 threads**, full run \| 1.72–1.91 ms" | **R1**, R42, R43 | Appendix A's flat **`EV-001…EV-083`** is the ledger and the only citation handle. In appendix A, **`EV-001` is "1.02 ms full chain at 8 threads"**, `EV-014`–`EV-016` are the SoA/AoS rows, `EV-019`–`EV-023` the Storm draw rows, `EV-035`–`EV-039` the RigExec rows, `EV-067`–`EV-070` the SeExpr/Ptex rows, `EV-081`–`EV-083` the adapter rows. `04-…` already cites appendix A's scheme correctly | Renumber every `EV-` citation in 09 onto appendix A's ids and delete the 100-block paragraph. 33 handles are dangling today: `EV-101, EV-201…EV-204, EV-301…EV-312, EV-401…EV-405, EV-501…EV-510, EV-601…EV-611, EV-701, EV-801…EV-805, EV-901…EV-951` |
| 09-2 | §5.2 SI-5 row | "\| **SI-5** \| T1 \| chain order \| … \| **M0** \| **MEASURED-pass** \|" | **R40** | R40 fixes **SI-1…SI-5 T1 M1**. `10-…` §6.5 and `11-…` §2.0 already say M1 | Change to M1 (keep "first green in M0" as a note) |
| 09-3 | §5.3 S-6 row | "\| **S-6** \| T2 \| no relocation across deform frames \| `vboRelocated == 0` \| … \| **M2** \|" and §5.5's M2 list | **R40** | R40 fixes **S-6 T2 M1**. `10-…` §6.5 and `11-…` §2.0 already say M1 | Change to M1 and move it out of §5.5's M2 row |
| 09-4 | §5.2 SI-9 row | "\| **SI-9** \| T1 \| **two scene-index instances attached to one session** \| identical generation, prim set and frame for one commit; **and** the pruning-wrapper cost on a production-density skinned scalp \|" | **R40** | R40 defines SI-9 as **"pruning-wrapper cost on a production-density skinned scalp" only**. `05-…` §4.2, `06-…` §10.1 and appendix B PW-13 all describe SI-9 that way | Narrow SI-9 to R40's metric and register the session-arbitration check under a fresh, unused `SI-` id (it is `12-…`'s `SI-10` request, which collides with appendix A's `SI-10`) |
| 09-5 | §5.3 S-11 and S-12 rows | "\| **S-11** \| … G3's `head1M` variant, static draw \| … \| M7 \|"; "\| **S-12** \| … prim-count sweep at 100 k × 8 CV: 1 / 32 / 128 / 196 tiles \| … \| **M1** \| … Re-mint of `12-…`'s `S-10`" | **R40** | The registry must carry one id per check and `00`, `10`, `11`, `12` and appendix A must agree. `12-…` §3/§7 and appendix A §5 both call the **tile sweep `S-11`**; appendix A additionally mints **`SI-10`** for the `reorder nameChildren` notice check, while `12-…` mints `SI-10` for session arbitration | Fix one assignment in 09 and propagate: either rename 09's `head1M` record and give the sweep `S-11`, or make `12-…` and appendix A adopt `S-12`. Same for the two `SI-10`s |
| 09-6 | §5.1 B-1 row | "`DT_NEEDED` of `libusdGen.so` vs **`USDGEN_FORBIDDEN_LIBS`**" | **R35**, R37 | `10-…` §1.4 defines `USDGEN_FORBIDDEN_LIB_REGEX` and `USDGEN_FORBIDDEN_INC_REGEX`; neither `USDGEN_FORBIDDEN_LIBS` nor the two regex variables appear in `10-…` §3.5's registry | Use `USDGEN_FORBIDDEN_LIB_REGEX` and add both CMake variables to `10-…` §3.5 (or state there that CMake cache variables are listed in §1.4) |
| 09-7 | §5.1–§5.4 status cells | "`10-…` §6.5 and `11-…` §2.0 say M1 and **are filed**" (E-3); "`10-…` §6.5 says M0 and is filed" (E-8); "`10-…` §6.5 and `11-…` §2.0 say M3 and are filed" (S-4); "`10-…` §6.5 says M5 and is filed" (T-5) | **R45** | `10-…` §6.5 places E-3 **M2**, E-8 **M1**, S-4 **M2**, T-5 **M8**; `11-…` §2.0 places E-3 M2 and S-4 M2. All four "filed" claims are false | Delete the four parentheticals |
| 09-8 | §6.2 `UsdGen::Compile` row | "Merkle digest, Kahn sort, sub-graph rebuild" | consistency | `03-…` §9.2, which §6.2 says it is identical to, reads "Merkle **+ graph** digests, Kahn sort, sub-graph rebuild" | Add "+ graph" |
| 09-9 | §5 Test column | `testUsdGenStormGolden` (L-2), `testUsdGenExpr`/`testUsdGenMaps` (L-3), `testUsdGenInvalidation --reload` (L-5), `testUsdviewUsdGenBrush` (T-1), `testUsdviewUsdGenPickAccuracy` (T-3), `benchUsdGenStorm --inplace` (S-10), `testUsdviewUsdGenInstancerPick` (T-INST-1), `testUsdGenLinkRule` (B-1) | exact names | `10-…` §6.5 names the same gates `testUsdGenStormLook`, `testUsdGenExpr --threads`/`testUsdGenLookBake`, `testUsdGenMapReload`, `testUsdviewUsdGenComb.py`, `testUsdviewUsdGenPick.py`, `testUsdGenStormInPlace`, `testUsdGenInstancerPick`, `testUsdGenLinkRule_*` + `testUsdGenIncludeRule` | One CTest name per gate; `10-…` §6.5 owns the harness and should be the canonical column |

---

## 12. `10-build-dependencies-testing.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 10-1 | §11 open items 1, 2, 5, 6, 7 | "`02-schema.md` §7.2–§7.3 and `06-imaging.md` §7 **still place** `UsdGeomRegisterComputeExtentFunction` … in `usdGenImaging`"; "09 §5 must adopt R40's assignments for E-3, E-8, SI-5, S-4, S-6, T-5 and T-INST-1 and **gain rows for SI-9, S-10, L-2…L-5 and B-1**"; "`00-…` §4.2 and **`02-schema.md` §8.6 still say `usdGenPy`**"; "Appendix A's measurement rows are `A2.n-Xn`; `ADR §9.1 R1` requires `EV-001…`"; "`06-imaging.md` §9's `TF_DEBUG` row still lists the retired codes" | **R45** | `02-…` §7.2 and `06-…` §8 both route the extent function to `libusdGenSchema.so`; `09-…` §5 has rows for SI-9, S-10, L-2…L-5 and B-1 and has adopted R40 for E-3, E-8, S-4, T-5 and T-INST-1 (SI-5 and S-6 remain, finding 09-2/09-3); `02-…` contains no `usdGenPy` (only `00-…` §4.2 does); appendix A is renumbered to `EV-001…EV-083`. Only item 7 (06's `TF_DEBUG` row) is still true | Delete items 1, 5, 6; narrow item 2 to SI-5 and S-6; keep items 3, 4 and 7 |
| 10-2 | §1 `:32-33`, §2.2 `:105-113`, §6.4 `:882`, §6.5 `:948-949`, §7.3 `:1134`, `:1149`, §8 `:1257`, §11 `:1260` (14 occurrences) | "`A2.10-B1`", "`A2.1-E8`", "`A2.5-RX1`", "`A2.5-RX2`", "`A2.10-B2…B5`", "`A2.10-B3`", "`A2.10-B4`" | **R1**, R43 | Appendix A rows are `EV-nnn`; `A2.10-B1…B5` are `EV-076`…`EV-080`, `A2.1-E8` is `EV-008`, `A2.5-RX1/RX2` are `EV-035`/`EV-036` | Renumber; delete `:33`'s "renumber them `EV-001…`, and every citation here moves with that rename" |
| 10-3 | §7.2 | "`TRACE_SCOPE(\"UsdGen::E-1::Evaluate\")` … `UsdGen::E-1::Evaluate`, `UsdGen::SI-3::Commit`" | exact names | `03-…` §9.2 (the declaration site) and `09-…` §6.2 use `UsdGen::Evaluate<opType>` and there is no `UsdGen::Commit` scope at all | Adopt `03-…` §9.2's list verbatim, or have 03 and 09 adopt the gate-prefixed form; one spelling in four documents |
| 10-4 | §3.5 registry table | Missing `USDGEN_FORBIDDEN_LIB_REGEX` and `USDGEN_FORBIDDEN_INC_REGEX` (defined in §1.4) and `USDGEN_MATH_SOURCES`, `USDGEN_FP_CONTRACT`, `USDGEN_USE_SYSTEM_*`, `USDGEN_INSTALL_*`, `USDGEN_WITH_RIGEXEC`, `USDGEN_USD_SITE_PACKAGES` (CMake cache variables used elsewhere in 10, in `00-…` §4.3, `02-…` §7.1 and `11-…` §1) | **R35** | "`10-build-dependencies-testing.md` owns the **single env-var registry**… Any other document introducing an env var must add it there" | Either extend §3.5 to a two-part registry (runtime env vars + CMake cache variables) or state explicitly in §3.5 that CMake options live in §5 and name them there. `09-…`'s `USDGEN_FORBIDDEN_LIBS` must be reconciled with §1.4's spelling |

---

## 13. `11-roadmap.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 11-1 | §2.0 closing paragraph, `:206-213` | "Rows of `09-…` §5 that R40 corrects, to be filed against it: SI-5 M0 → **M1**; E-3 M1 → **M2**; E-8 M0 → **M1**; S-4 M3 → **M2**; S-6 M2 → **M1**; T-INST-1 tier T3 → **T1**; **E-1 threshold 2.5 → 1.5 ms**. **Five binding ids are absent from 09 §5 and must be added: B-1, SI-9, L-2, L-3/L-4/L-5, S-10.** `12-…` §3 **defines S-10 and SI-9 differently**" | **R45** | `09-…` §5 now says E-3 M2, E-8 M1, S-4 M2, T-INST-1 T1, E-1 ≤ 1.5 ms, and registers B-1, SI-9, L-2, L-3, L-4, L-5 and S-10. `12-…` no longer uses S-10 or SI-9 — it uses S-11 and SI-10 | Reduce the paragraph to the two rows still open (SI-5 M0 → M1, S-6 M2 → M1) and re-point the `12-…` sentence at the S-11/SI-10 collision |
| 11-2 | §2.1–§7 (20 occurrences), e.g. `:212`, `:249`, `:253-254`, `:260-262`, `:287`, `:290`, `:297`, `:82` | "`A2.1-E1`", "`A2.1-E2`", "`A2.1-E8`", "`A2.1-E4`", "`A2.3-ST4`", "`A2.6-FZ5`", "`A2.6-FZ7`", "`A2.1-E7`", "`A2.1-E6`", "`A2.1-E9`", plus "ADR §9 R1 renames them to `EV-001…` and these citations follow that rename one-for-one" | **R1**, R43 | Appendix A's `EV-nnn` ids exist today | Renumber and delete the "will follow" sentence |
| 11-3 | §1 PW-4 `:100`; §8 sources `:714` | "`pxr/imaging/hdSt/renderDelegate.cpp:701-707`" | **R43** | `:695-707` | Re-point |
| 11-4 | §2.2 `:264`; §6.4 RC-10 `:640` | "`esfUsd/stageData.cpp:360`" | consistency | `:361` | Re-point |
| 11-5 | §2.0 M7 exit row | M7's exit gates omit **S-11** | **R40** | `09-…` §5.5 places S-11 at M7 | Add it, or move it once 09-5 is resolved |

---

## 14. `12-risks-decisions-open-questions.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| 12-1 | §0.2 preamble `:58-61` and 22 Evidence cells (`:74`, `:81`, `:83`, `:85-87`, `:95`, `:101`, `:103-105`, `:109`, `:194`, `:201`, `:209`, `:237`, `:243`, `:249`, `:274`, `:316`, `:347`) | "**Ledger handles are in transition:** ADR §9 R1/R43 make `EV-001…` the only citation handle …, but `appendix-A-evidence-ledger.md` **still carries its per-subsection ids** (`A2.1-E8`, `A2.3-ST3`, …). Cells below cite the current id; the renumbering … lands **by the end of M0**" | **R1**, R43, R45 | Appendix A carries `EV-001…EV-083` today, with §2.0 mapping the retired ids and §5 K25 recording the retirement | Delete the paragraph and renumber all 22 cells |
| 12-2 | §3 preamble, `:288-290` | "Role names … are staffing roles, not gate or measurement ids (`E-1` is a gate, **`E1` an appendix-A measurement handle**, `09-performance-and-benchmarks.md` §1)" | **R1** | The only appendix-A measurement handle is `EV-nnn`; `E1` is a retired `A2.1-` family id | Rewrite as "`EV-008` an appendix-A measurement handle" |
| 12-3 | §3 preamble `:294-299`; RK-07 `:237`; RK-09 `:239`; Q-02 `:306`; §7 `:394`, `:408`, `:419` | "**S-11** — T2, a 1 / 32 / 128 / 512-tile EGL sweep …"; "**SI-10** — T1, two scene index instances attached to one session … SI-9 is already taken" | **R40** | `09-…` §5 is the single registry. It currently registers **S-11 = the `head1M` draw record (M7)** and **S-12 = the tile sweep (M1)**, and has no `SI-10`. Appendix A §5 independently mints **`SI-10` for the `reorder nameChildren` notice check** and **`S-11` for the tile sweep** | One id per check, registered in `09-…` §5 first; then 12 and appendix A cite it. Do not ship three meanings of `S-11` and two of `SI-10` |
| 12-4 | §8 sources, `:500` | "`pxr/base/vt/array.h:39-55, 53, 269, 945, **992**, 1009, 1023`" | citation accuracy | Re-verified in `/home/burkard/work/OpenUSD`: the class-scope `private:` label that hides `_IsUnique()` is at **`:984`**, `_IsUnique()` at `:1023`, `IsIdentical` at `:945`. `01-…` §4.2 correctly writes `:984` | Change `992` to `984` |
| 12-5 | RK-29 `:259`; A-19 `:354` | "**`02-schema.md` §7.3 and `10-build-dependencies-testing.md` §4.3 still route the `LibraryPath` at `usdGenImaging`** and must follow R19"; "**`11-roadmap.md` §5.2 SC-4 still carries the superseded unsalted formula** and must follow R13" | **R45** | `02-…` §7.1–§7.3 and `10-…` §4.3 both route it at `libusdGenSchema.so`; `11-…` §5.2 SC-4 carries the salted `UsdGenHash32(curveId, kSaltDensity)` predicate with the R12/R13 citation | Delete both sentences |

---

## 15. `appendix-A-evidence-ledger.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| A-1 | §6 "How to build and run the probes", `:536` | "…which owes a correction: its §0 and its **P-11** row still say `mbbench.cpp` was not carried in, and `prototypes/motion-blur/mbbench.cpp` exists" | **R1**, R45 | M0 pre-work items are **`PW-1…PW-n`**, never `P-n`; appendix B's row is **PW-11**. And appendix B §0.1 row 12 already lists `motion-blur` as carried in, §0.2 records the rename `mbbench.cpp → motion-blur/mbbench.cpp`, and §12 documents the directory. The file exists (verified) | Change `P-11` to `PW-11` and delete the "owes a correction" claim |
| A-2 | §5 "Collision to fix elsewhere" `:452-454`; §5 U13 `:470`; §6 `:534` | "Two ids are minted here …: **`SI-10`** (U13), for ADR §7's M0 `reorder nameChildren` notice check …; and **`S-11`**, the rename target this ledger proposes for the tile-count sweep"; "`12-…` uses `SI-9` for session arbitration and `S-10` for the tile-count sweep … 12 must rename its two gates — session arbitration to a free `SI-` id, the sweep to **S-11**"; "`09-…` §5 **has no build-gate table today**; **B-1** (U20) needs one" | **R40**, R45 | `09-…` §5.1 already has a build-gate table with **B-1**; `09-…` §5.3 uses **S-11** for the `head1M` draw record and **S-12** for the sweep; `12-…` now uses `S-11` and `SI-10`, not `S-10`/`SI-9` | Re-point every id at `09-…` §5 once finding 09-5 is resolved; delete the B-1 claim |
| A-3 | §3 "Approximate sub-range", `:421` | "the plan cites **`:695-712`** (ADR §9 R43 accepts `:695-707`)" — and §2.7 `:361` cites `imaging/hdSt/renderDelegate.cpp:695-712` | **R43** | R43 fixes `hdSt/renderDelegate.cpp:695-707` as the accepted range | Cite `:695-707` and keep `:695-712` only as the containing function's span |
| A-4 | §2.11, `:272` | "Published by `01-architecture.md` §0.3 and **`05-static-curves-and-deformation.md` §8**; **both** still print 0.8–1.2 ms" | **R45** | `05-…` §8 `:1289` prints "≈ **0.8–1.0 ms** … DERIVED aggregate (`09-…` §0.2, the only frame ledger, ADR §9 R41)". Only `01-…` §0.3 and `03-…` §0.2 still print 0.8–1.2 | Drop 05 from the sentence and add `03-…` §0.2 |
| A-5 | §1, `:83` | "…so every `S-` gate and `L-1` become milestone exits …: S-8, S-9 and L-1 as M0 pre-work…; **S-10 at M8**" — the list stops at S-10 and names only `L-1` | **R40** | `09-…` §5.3 registers **S-1…S-12** and §5.4 registers **L-1…L-5** | Extend both families |

---

## 16. `appendix-B-prototype-inventory.md`

| # | Location | Offending text | Ruling | Canonical | Fix |
|---|---|---|---|---|---|
| B-1 | §5 `:378`, §11 `:941`, §14 `:1133` | "`pxr/exec/esfUsd/stageData.cpp:360`" (twice as the defect line; once with the note "comment; the `UsdPrimDefaultPredicate` call is at `:361`") | consistency | `:361` is the predicate call and is what `12-…`, `08-…`, `10-…`, `00-…`, `01-…` and `05-…` cite | Re-point `:378` and `:941`; `:1133`'s parenthetical is already correct |
| B-2 | §0.1 row 4 "Carried into" cell | "`usdGenUsdview/usdGenUndo.py`" | **R5** | The module is `usdgen/usdGenUndo.py` (`08-…` §1.2, `10-…` §1.5 `:149`) | Re-path |
| B-3 | §13 tier table `:1073-1074` | "SI-1…**SI-9**", "**S-1…S-10**, L-1" | **R40** (low) | `09-…` §5 runs to SI-9 (correct) and **S-12**, **L-5** | Extend the S- and L- ranges |

---

## 17. Files to change, in dependency order

Change the two registries first; every other edit cites them.

| Order | File | Why it is first | Findings |
|---|---|---|---|
| 1 | `appendix-A-evidence-ledger.md` | It owns the `EV-nnn` rows every other document must cite (R1, R43). Fix `P-11` → `PW-11`, the `:695-712` range and the S-11/SI-10 minting so 09 can be renumbered against a stable ledger | A-1…A-5 |
| 2 | `09-performance-and-benchmarks.md` | The single gate registry (R40) and the single frame ledger (R41). Renumber all 33 dangling `EV-` handles onto appendix A; fix SI-5, S-6, SI-9 and the S-11/S-12 minting; then every "gates" section in 00, 10, 11, 12 and both appendices can be checked against it | 09-1…09-9 |
| 3 | `02-schema.md` | The normative property registry (R7). Reclass the density scales to **value**, land the three mask-block terms, replace the ledger family ids | 02-1…02-5 |
| 4 | `00-request-and-scope.md` | Owns the glossary and gate-family list every reader starts from (R44) | 00-1…00-10 |
| 5 | `01-architecture.md` | Delete the duplicate frame ledger; fix the `usdgen` package row and the `renderDelegate.cpp` range | 01-1…01-3 |
| 6 | `03-execution-engine.md` | "64 tiles at 100 k", the 0.8–1.2 ms aggregate, the "no `EV-` rows" claim, and six stale §12.1 corrections | 03-1…03-6 |
| 7 | `06-imaging.md` | The retired `TF_DEBUG` codes (R35), the stale R19 note, the ledger family ids | 06-1…06-6 |
| 8 | `07-look-maps-expressions.md` | The last `smooth \| monotoneCubic` ramp set (R11), the 32-bit `curveId`, the "no app driver" clause, three `renderDelegate.cpp` ranges | 07-1…07-7 |
| 9 | `04-operators.md` | Make §5.2 identical to 02 §2.13; delete three stale §0.13 rows | 04-1…04-3 |
| 10 | `05-static-curves-and-deformation.md` | Delete the sibling defect list (eight false claims) and the five inline "still …" paragraphs | 05-1, 05-2 |
| 11 | `08-tools.md` | Delete the false "06 §3.8 is an earlier draft" table (R31) and the two stale §11 items; add the `EV-nnn` handles | 08-1…08-4 |
| 12 | `10-build-dependencies-testing.md` | Renumber the ledger handles, prune §11's stale items, settle the env-var/CMake-variable split and the trace-scope spelling | 10-1…10-4 |
| 13 | `11-roadmap.md` | Renumber the ledger handles; reduce §2.0's correction list to the two rows still open | 11-1…11-5 |
| 14 | `12-risks-decisions-open-questions.md` | Renumber 22 Evidence cells; fix the `E1` handle, the `vt/array.h:992` line, the S-11/SI-10 requests and two stale sibling claims | 12-1…12-5 |
| 15 | `appendix-B-prototype-inventory.md` | Two line references and one module path | B-1…B-3 |

### 17.1 Mechanical checks to add to `testUsdGenDocs`

`00-…` §7 already specifies a docs lint. Four checks would have caught every finding in this report:

1. **Ledger-handle resolution.** Every `EV-\d{3}` in any document resolves to a row id in
   `appendix-A-evidence-ledger.md`; no document contains `A2\.\d+-[A-Z]` or a bare family id
   (`X1`, `FZ8`, `CPU7`, `ST3`, `RX1`, `TL7`, `MB5`, `E9`, `V1`, `B4`, `AD1`).
2. **Gate-id resolution.** Every `\b(E|SI|S|L|T|T-INST|R|B)-\d+[a-z]?\b` resolves to a row of
   `09-…` §5, and its tier and milestone match wherever the id is repeated.
3. **Cross-reference truth.** Every sentence containing "still says", "still carries", "still
   lists", "must be edited", "must gain" or "verified absent" names a sibling section; the check
   greps that section for the quoted string and fails if it is absent (R45). Every claim of this
   shape in the current plan except one (`06-…` §9's `TF_DEBUG` row) fails it.
4. **Banned strings.** `usdGenPy`, `VtArray::IsUnique`, `usdGen:active`, `usdGen:output`,
   `usdGen:cacheOutput`, `usdGen:interactive:maxCurves`, `usdGen:HairPreview` outside a
   "display name" sentence, `UsdGenImagingSession`, `ChunkPrimBuilder`, `UsdGenDirtyBits::`,
   `monotoneCubic`, `renderDelegate.cpp:70\d`, `stageData.cpp:360`, and `P1–P8` / `P[1-8]` used
   as an engine-principle name.

---

## 18. What this pass did **not** find

Recorded so a later reader does not re-check them. Every one was greped across all fifteen files
and is clean:

* `usdGen:active`, `usdGen:output`, `usdGen:cacheOutput`, `usdGen:cache*`, `usdGen:diagnostics`,
  `usdGen:curve:cvCount`, `usdGen:mask:map` — none appears as a live property (R8, R16). The only
  hits are the documents' own "dropped everywhere" notes.
* `usdGen:primitive` survives **only** on `UsdGenInstance`, which R8 permits; `UsdGenDescription`
  carries no primitive-type token (`02-…` §2.3 states the rule explicitly).
* `VtArray::IsUnique()` appears only inside "does not exist" statements (R20). All eight
  documents that mention it carry the `Vt_ArrayForeignDataSource` replacement.
* `UsdGenImagingSession`, `ChunkPrimBuilder`, `UsdGenDirtyBits::Value` — absent (R3). `01-…` §5.1
  records the `UsdGenChunkPrimBuilder` → `UsdGenTileBuilder` rename as a rename, which R3 allows.
* Shader identifiers are `UsdGenHairPreview` / `UsdGenHairPreviewTranslucent` /
  `UsdGenHairPreviewPrimvar` everywhere, and `usdGenShaders` ships three glslfx files (R4).
  `usdGen:HairPreview` appears only as a display name in `07-…` §2.4.
* `usdGen:space` tokens are `auto | rest | deformed` with no `world`; `usdGen:readPhase` defaults to
  `final` with `preceding` as a v1 alias (R9).
* `UsdGenClump` carries no `usdGen:mode`; `usdGen:clump:method` and `uniform int usdGen:clump:level`
  are consistent across 02, 04 and 11 (R10).
* `curveId` is `uint64[]` in 02, 03, 04, 05, 06 and 08; only `07-…` narrows it (finding 07-3) (R12).
* The density predicate `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32` with
  `kSaltDensity ≠ 0` is identical in 01, 02, 03, 04, 05, 06, 08, 09, 11 and 12 (R13).
* `usdGen:enabled` is never a digest term, and the topology-preserving / generator+`Resample`+`Length`
  split is consistent in 02 §0.4, §2.5, §6.2–§6.3, 03 §5.1 and 04 §0.4 (R14).
* `familyName` is documented as never reaching Hydra in 02 §2.20 and 12 (R15).
* `usdGen:algorithmVersion` fallback is `0 = "latest kernel"` in 02 and 04 (R17).
* A stale `UsdGenFreeze` epoch warns and keeps rendering in 02 §2.9, 05 §5.3 and 12 (R18).
* Commit order (R22), `UsdGenGraphDesc::curveSets` / `UsdGenSurfaceDesc::samples` (R23), the
  full-locator router key (R25), the 1–10 % guide sizing (R26), the 8-thread baseline and
  E-1 ≤ 1.5 ms (R27), session keys namespaced by `renderInstanceId` + root-layer identifier (R30),
  `hairTangentWorld` rejected (R33), material bindings on every tile and the all-renderers phase-0
  ordering under hdPrman (R34), Shift+F / Alt+F (R36), and the `usdGen` link rule with gate B-1
  (R37) — all consistent across every document that states them.
* Milestones are M0–M8 with no "S" or "P" phases anywhere; the 34-week calendar and "T4 gates are
  release criteria `RC-n`, never milestone exits" are consistent in 00, 01, 09, 10, 11 and 12 (R39).

---

## 19. Sources

| Source | Sections used |
|---|---|
| `design/adr-v1.md` | **§9 (R1–R45)** as the binding lens; §1 (amendments), §2.1–2.3, §3, §4.1–4.5, §5.1–5.6, §6, §7, §8 for the values §9 amends |
| `design/brief-v1.md` | §2 S1–S46, for the constraints the rulings amend (S11, S18(c), S23/S24/S27, S29, S31, S32, S36, S39, S42, S45) |
| `plan/00-…` – `plan/12-…`, `appendix-A-…`, `appendix-B-…` | read in full; every line reference in this report was taken from the file as of 2026-09-05 |
| `design/judge-evidence.md`, `judge-artist.md`, `judge-delivery.md` | the rejected-idea lists checked in §18 |
| OpenUSD 26.08, `/home/burkard/work/OpenUSD` | re-grepped for this report: `pxr/base/vt/array.h:39-55` (`Vt_ArrayForeignDataSource`), `:945` (`IsIdentical`), `:984` (the class-scope `private:`), `:1023` (`_IsUnique()`); `pxr/imaging/hdSt/renderDelegate.cpp:695-712` (`_RenderDelegateInfo`, `materialRenderContexts` at `:702-707`) |
| `plan/prototypes/` | directory listing, to confirm `prototypes/motion-blur/mbbench.cpp` exists (finding A-1) |
