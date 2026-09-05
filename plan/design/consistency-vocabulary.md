# Cross-document consistency review — VOCABULARY lens

Date: 2026-09-05. Status: review v1 (findings only; no plan document was edited).

This report is the vocabulary pass over the fifteen usdGen plan documents. It builds the union of
every prim type, property (with type/default), Hydra-only prim name, C++ class/function/struct, CMake
target, environment variable, C ABI entry point, Python module, `TfDebug` code, contract id, gate id
and milestone id used across the plan, and lists every place where two documents name the same thing
differently, use a name no sibling defines, or violate the binding vocabulary of `design/adr-v1.md`
§2, §3, §6, §7 and the §9 addendum (R1–R45).

**Authority order used throughout.** `design/adr-v1.md` is canonical, and its §9 addendum wins over
its own §1–§8. Where the ADR is silent: `02-schema.md` for schema names, types, defaults and allowed
tokens (R7); `03-execution-engine.md` for engine C++ names; `06-imaging.md` for imaging names and the
tile contract C2; `10-build-dependencies-testing.md` for CMake targets, the env-var registry (R35),
the `TfDebug` codes and the CTest name registry; `09-performance-and-benchmarks.md` §5 for gate ids,
tiers and milestones (R40); `11-roadmap.md` for milestone ids and the `SC-n` register;
`08-tools.md` §1.4 for the C ABI (R31); `appendix-A-evidence-ledger.md` for `EV-nnn` measurement
handles (R1, R43).

Findings are `CV-nn`. Severity: **blocking** = a contract (C1–C5) or a binding ruling is violated, or
two documents would generate different code/schema from the same name; **major** = a registry is
internally contradicted and an implementer would have to guess; **minor** = a stale cross-reference,
a cross-reference to the wrong section, or a cosmetic naming drift.

A recurring failure mode deserves naming up front: **many documents carry "owed edit" or "must be
corrected" notes against siblings that have since been corrected.** Those stale notes are findings in
their own right, because a reconciler acting on them would re-break a document that already conforms.
They are marked *(stale claim)*.

---

## 0. Summary of the five systemic problems

| # | Problem | Documents affected |
|---|---|---|
| 1 | **Two incompatible `EV-nnn` numbering schemes.** `appendix-A-evidence-ledger.md` §2 assigns a **flat** sequence `EV-001…EV-083`. `09-performance-and-benchmarks.md` §1 declares a **100-block-per-subsection** scheme and cites `EV-101, EV-201…EV-951`, forty-four ids of which **thirty-one do not exist** in the ledger and two (`EV-021`, `EV-023`) resolve to *different measurements* than 09 intends. Six further documents still cite the **retired** `A2.n-Xn` handles, and four assert the ledger has no `EV-` rows at all. | 09, 10, 11, 12, 02, 03, 05, 07, 08 |
| 2 | **Gate-id collisions.** `09` §5 mints `S-11` = the `head1M` static draw and `S-12` = the prim-count sweep, and mints `T-EXPR-1` / `T-PTEX-1`. `12` and `appendix-A` mint `S-11` = the prim-count sweep and `SI-10` = session arbitration, and `12` §0.4 lists `T-EXPR-*` as a *superseded* family. Three documents therefore use `S-11` for two different gates and one gate (`SI-10`) exists in two documents and not in the registry. | 09, 12, appendix A, 11, 10, 00 |
| 3 | **Two `TfDebug` code sets and two `TfTrace` scope-naming conventions.** `10` §7.2 (the registry) and `03` §9.2 and `09` §6.2 agree on six codes; `06` §9 still lists the eight retired ones. Scope names are plain (`UsdGen::Route`) in 03 and 09 and gate-qualified (`UsdGen::SI-3::Commit`) in 06 and 10. | 06, 10, 03, 09 |
| 4 | **The published emitted-primvar names have two spellings.** The ADR (§2.3, R24), `03` and `06` (contract C2) publish `clumpId_<level>`, `guideIndex`, `guideWeight`. `02` (the property registry), `04` and `05` publish `primvars:usdGen:clumpId_<level>` / `…:guideIndex` / `…:guideWeight`. Contract C2 and contract C1 disagree on a name each freezes at M1. | 02, 04, 05 |
| 5 | **Target-graph divergence between `01` §5.1 and `10` §1.2.** Four rows differ materially (`usdGenMath`'s SeExpr link, the `usdGenTestUtils` split, `usdGenShaders`' plugin type, the `usdGenUsdview` install path), and `02` §7.1's `usdGenSchema` link line contradicts `10`'s. | 01, 02, 10 |

---

## 1. Canonical values (the union, resolved)

### 1.1 Prim types and API schemas (ADR §2.1; `02-schema.md` §1)

Concrete: `UsdGenGroom`, `UsdGenDescription`, `UsdGenGuideSet`, `UsdGenScatter`, `UsdGenGrow`,
`UsdGenGuideInterpolate`, `UsdGenCurveSource`, `UsdGenClump`, `UsdGenNoise`, `UsdGenCurl`,
`UsdGenBend`, `UsdGenDirection`, `UsdGenLength`, `UsdGenWidth`, `UsdGenSmooth`, `UsdGenStraighten`,
`UsdGenDisplace`, `UsdGenWave`, `UsdGenScale`, `UsdGenResample`, `UsdGenSculptLayer`,
`UsdGenExprOp`, `UsdGenDeform`, `UsdGenCollide`, `UsdGenWind`, `UsdGenFreeze`, `UsdGenInstance`,
`UsdGenImageMap`, `UsdGenPtexMap`, `UsdGenExprMap`, `UsdGenPaintMap`, `UsdGenNoiseMap`,
`UsdGenCombineMap`, `UsdGenGuideProximityMap`. Abstract: `UsdGenOperator`, `UsdGenGenerator`,
`UsdGenStyler`, `UsdGenDeformer`, `UsdGenMap`. API: `UsdGenMaskAPI`, `UsdGenLookAPI`,
`UsdGenRestAPI`, `UsdGenCurveAPI`. v2/v3 reserved: `UsdGenPart`, `UsdGenBraid`, `UsdGenSimSource`,
`UsdGenForce`. Rejected: `UsdGenPrimitive`, four `UsdGenScatterX` types.

### 1.2 Hydra-only prim names (ADR §2.2)

`<Description>/__usdGenRender/{tile_0000…tile_NNNN, guides/<setName>, guides/<setName>/cvs,
inst_<opName>, inst_<opName>/Prototypes/<n>, material_storm}`. Consistent in every document.

### 1.3 Engine and imaging C++ names (R3; `03` §1–§9, `06` §1–§3)

`UsdGenGroomSceneIndexPlugin`, `UsdGenGroomSceneIndex`, `UsdGenMetadataSceneIndexPlugin`,
`UsdGenImagingRegistry`, `UsdGenSession`, `UsdGenTileBuilder`, `UsdGenInstancerBuilder`,
`UsdGenPrimAdapterBase` + `UsdGenOperatorAdapter` / `UsdGenMapAdapter` / `UsdGenGroomAdapter` /
`UsdGenDescriptionAdapter` / `UsdGenGuideSetAdapter` / `UsdGenRestAPIAdapter`,
`UsdGenShadersDiscoveryPlugin`, `UsdGenSurfaceResolver`, `UsdGenSurfaceReader`,
`UsdGenLiveOverrideStore`, `UsdGenPublishedIndex`, `UsdGenGenerationStore`, `UsdGenGeneration`,
`UsdGenGenerationBuffer`, `UsdGenDirtyRouter`, `UsdGenPendingDirty`, `UsdGenGraph`,
`UsdGenGraphDesc` (+ `UsdGenNodeDesc`, `UsdGenCurveSetDesc`, `UsdGenSurfaceDesc`,
`UsdGenSurfaceSample`, `UsdGenMapDesc`, `UsdGenParamValue`, `UsdGenRampDesc`), `UsdGenCompiler`,
`UsdGenScheduler`, `UsdGenCurveBuffer`, `UsdGenChunkDesc`, `UsdGenChunkView`, `UsdGenTileView`,
`UsdGenPlane`, `UsdGenCapture`, `UsdGenKdTree`, `UsdGenMotionCache`, `UsdGenOp`,
`UsdGenOpRegistry`, `UsdGenStats`, `UsdGenNodeStats`, `UsdGenCurveLoader`, `UsdGenSourceArrays`.
Enums: `UsdGenSpace {Inherit, Rest, Deformed}`, `UsdGenTopoFx {None, CurveCount, CvCount, Both}`,
`UsdGenRole {Curves, Reference}`, `UsdGenReadPhase {Base, Preceding, Final, Explicit}`, and the
unscoped `UsdGenDirtyBits` (`UsdGenDirtyNone` … `UsdGenDirtyLiveOverride`).

### 1.4 Shader identifiers and files (R4)

Identifiers `UsdGenHairPreview`, `UsdGenHairPreviewTranslucent`, `UsdGenHairPreviewPrimvar`; files
`usdGenHairPreview.glslfx`, `usdGenHairPreviewTranslucent.glslfx`,
`usdGenHairPreviewPrimvar.glslfx`, plus `shaderDefs.usda` and `usdGenHair.mtlx`.

### 1.5 CMake targets (`10` §1.2, binding)

`usdGenMath`, `usdGen_seexpr`, `usdGen_ptex`, `nanoflann`, `usdGen`, `usdGenImaging`,
`usdGenSchema`, `usdGenShaders`, `_usdGen`, `usdgen`, `usdGenUsdview`, `usdGenTestUtils`,
`usdGenTestUtilsHd`. The Python package is **`usdgen`** (R5); `usdGenPy` is not a name.

### 1.6 Environment variables (`10` §3.5, the single registry, R35)

`USDGEN_ENABLE`, `USDGEN_CONTEXT`, `USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`,
`USDGEN_MEMORY_BUDGET_MB`, `USDGEN_STORM_MATERIAL_OVERRIDE`, `USDGEN_DIAGNOSTICS`,
`USDGEN_IMAGE_CACHE_MB`, `USDGEN_PTEX_CACHE_MB`, `USDGEN_PTEX_MAX_FILES`, `USDGEN_OP_CHECKS`,
`USDGEN_IMAGING_DLL`, `USDGEN_XVFB_ROOT`, `USDGEN_DISPLAY`. (Build-time cache variables
`USDGEN_FP_CONTRACT`, `USDGEN_WITH_RIGEXEC`, `USDGEN_INSTALL_*`, `USDGEN_USE_SYSTEM_*` are CMake
options, not runtime env, and are correctly kept separate.)

### 1.7 `TfDebug` codes (`10` §7.2, canonical)

`USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`, `USDGEN_CAPTURE`,
`USDGEN_MEMORY`. Retired: `USDGEN_GRAPH` (synonym of `USDGEN_COMPILE`), `USDGEN_SESSION`,
`USDGEN_MAPS`, `USDGEN_EXPR`, `USDGEN_TIMING`.

### 1.8 C ABI (contract C4, `08` §1.4)

Nineteen entry points: `Activate`, `Deactivate`, `SetTime`, `Commit`, `SetContext`, `GetGeneration`,
`GetTopologyGeneration`, `BeginLiveOverride`, `SetLiveOverrideIndexed`, `ClearLiveOverride`,
`PickCV`, `Footprint`, `ClosestSurfacePoint`, `BuildMirrorMap`, `SetInteractiveLOD`,
`SetMaskVisualisation`, `ReloadMaps`, `GetStatsJson`, `GetLastError`. `06` §3.8's copy is
byte-identical in the name set — the two are already reconciled.

### 1.9 Contract, gate, milestone, tier and stop-condition ids

Contracts `C1`–`C5`. Milestones `M0`–`M8`. Test tiers `T0`–`T4` (no hyphen). Gate families
`B- E- SI- S- L- T- T-INST- R-` (always hyphenated). Pre-work `PW-1…PW-6`. Third-party patches
`TP-1…TP-8`. Engine invariants `I1–I8`; motion profiles `P0/P1/P2`. Register ids `RK-nn`, `X-nn`,
`Q-nn`, `A-nn`; stop conditions `SC-n`; release criteria `RC-n`. Measurement handles `EV-nnn`.

---

## 2. Findings

### 2.1 Evidence-ledger citation handles (`EV-nnn`)

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-01 | `09-performance-and-benchmarks.md` | §1 preamble and §0.2–§7 throughout | Declares a **100-block-per-subsection** `EV-nnn` scheme and cites `EV-101, EV-201/202/203/204, EV-301/303/304/305/307/308/310/312, EV-401/403/404/405, EV-501/504/510, EV-601/603/604/607/610/611, EV-701, EV-801/804/805, EV-901/904, EV-951`. `appendix-A-evidence-ledger.md` §2 uses a **flat** sequence and stops at `EV-083`. Thirty-one of 09's ids do not exist; `EV-021`/`EV-023` exist but denote the 200 k Storm draw and the refineLevel-3 row, not the SoA/AoS kernel rows 09 cites them for. 09 also asks appendix A to "gain `A2.4-CPU12`", a retired id | `appendix-A-evidence-ledger.md` §2 and §2.0 (flat `EV-001…EV-083`, ADR §9 R1/R43) | Re-map every `EV-` citation in 09 onto appendix A's flat ids (§2.0's legacy map gives the correspondence); delete §1's 100-block paragraph and the `A2.4-CPU12` request | blocking |
| CV-02 | `03-execution-engine.md` | §0.5; §12.1 correction D | States appendix A "carries **no `EV-` rows yet** (verified 2026-09-05)" and files correction D demanding they be added. Appendix A §2.1–§2.12 carries `EV-001…EV-083` today *(stale claim)* | `appendix-A-evidence-ledger.md` §2 | Delete correction D; replace §0.3/§0.4's report-section citations with the ledger's `EV-` ids | major |
| CV-03 | `08-tools.md` | §0.3; §2.2; §8.1; §8.3 W2; §11 item 6 | Says "Appendix A does not yet carry `EV-` rows" and prints literal `EV-nnn` placeholders in four measured cells *(stale claim)* | `appendix-A-evidence-ledger.md` §2 (`EV-002`, `EV-050`, `EV-061`, `EV-064`, `EV-065`, `EV-020`/`EV-021`) | Substitute the real row ids; delete §11 item 6 | major |
| CV-04 | `05-static-curves-and-deformation.md` | §0.2, §0.3 | Says each number "carries an `EV-nnn` handle **once** `appendix-A-evidence-ledger.md` adopts ADR §9 R43's numbering", then cites report sections only *(stale claim)* | `appendix-A-evidence-ledger.md` §2 | Cite `EV-` ids directly | minor |
| CV-05 | `07-look-maps-expressions.md` | §0.3 closing paragraph | "The MEASURED rows above gain their `EV-nnn` ledger handles when `appendix-A-evidence-ledger.md` assigns them (R43)" *(stale claim)*; the whole §0.3 table cites report sections | `appendix-A-evidence-ledger.md` §2 (`EV-019`–`EV-023`, `EV-033`, `EV-034`, `EV-067`–`EV-070`) | Cite `EV-` ids | minor |
| CV-06 | `02-schema.md` | §0 preamble; §2.13; §5 rule 5 | Cites the **retired** per-subsection ids (`§2.8 rows X1, X3, X4`; `§2.6 row FZ8`) and says "the row ids used here are the ledger's current family ids" | `appendix-A-evidence-ledger.md` §2.0 map: `X1→EV-067`, `X3→EV-069`, `X4→EV-070`, `FZ8→EV-048` | Rewrite to `EV-` ids | major |
| CV-07 | `10-build-dependencies-testing.md` | §0 preamble, §0.6, §5.5, §6.3, §8.1, §8.2, §11 conflict 6 | Cites retired `A2.10-B1/B2/B3/B4/B5`, `A2.1-E8`, `A2.5-RX1/RX2` and states appendix A "still" uses them | `appendix-A-evidence-ledger.md` §2.0: `A2.10-B1…B5 → EV-076…EV-080`, `A2.1-E8 → EV-008`, `A2.5-RX1/RX2 → EV-035/EV-036` | Rewrite; delete conflict 6 | major |
| CV-08 | `11-roadmap.md` | §0.5, §2.2–§2.8, §9 sources table | Cites retired `A2.1-E1/E2/E4/E6/E7/E8/E9`, `A2.3-ST3/ST4`, `A2.6-FZ5/FZ7/FZ10`, `A2.7-TL7/TL10/TL11`, `A2.8-X1/X4`, `A2.9-MB1–MB5`, and says the rename "lands one-for-one" later | `appendix-A-evidence-ledger.md` §2.0 map | Rewrite to `EV-` ids | major |
| CV-09 | `12-risks-decisions-open-questions.md` | §0.2 preamble; §1; §2 RK-*; §4 A-12 | Cites retired `A2.1-E7/E9`, `A2.3-ST2/ST3`, `A2.5-RX4/RX5`, `A2.6-FZ*`, `A2.7-TL*`, `A2.8-X*`, `A2.12-AD*` and says "the renumbering … lands **by the end of M0**" *(stale claim)* | `appendix-A-evidence-ledger.md` §2.0 map | Rewrite to `EV-` ids; delete the transition note | major |

### 2.2 Gate ids, tiers and milestones

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-10 | `12-risks-decisions-open-questions.md` | §2 preamble, §2 RK-07, §3 Q-02, §5 | Requests **`S-11`** for the 1/32/128/512-tile sweep and **`SI-10`** for session arbitration. `09` §5.3 already assigns `S-11` to the `head1M` static draw and `S-12` to the tile sweep, and `09` §5.2 folds session arbitration into `SI-9`. Two gates therefore share the id `S-11` across the plan | `09-performance-and-benchmarks.md` §5 (R40): `S-11` = `head1M` draw, `S-12` = prim-count sweep; `SI-9` covers both the pruning cost and two-index agreement | Re-mint 12's tile sweep as **`S-12`**; drop `SI-10` in favour of `SI-9` (or raise a registry request under a free id) | blocking |
| CV-11 | `appendix-A-evidence-ledger.md` | §2.11 "Multi-prim Storm cost" row; §5 coverage table | Uses `S-11` for the tile-count sweep and lists `SI-10` as a gate with no baseline — 12's ids, not the registry's | `09-…` §5.3/§5.2 | Re-point to `S-12`; drop `SI-10` | major |
| CV-12 | `12-risks-decisions-open-questions.md` | §0.4 row R2 | Lists "`T-EXPR-*`" among the **ad-hoc gate families R2 supersedes**, while `09` §5.4 mints `T-EXPR-1` and `T-PTEX-1` as registered gates. The plan simultaneously bans and registers the family | `09-…` §5.4 (registry, R40) | Either register `T-EXPR-1`/`T-PTEX-1` in 12's vocabulary or move both assertions into `L-3`/`L-4` as 12 §2 already proposes; one or the other, not both | major |
| CV-13 | `00-request-and-scope.md` | §5.4 | The gate-family list omits **`B-`** entirely and omits `SI-9`, `S-3…S-6`, `S-10`, `S-11`, `S-12`, `L-2…L-5`, `T-EXPR-1`, `T-PTEX-1`. ADR §9 **R44** requires 00's gate-family list and glossary to carry "the `L-`, `B-`, `SI-6…SI-9`, `S-10` additions" | ADR §9 R44; `09-…` §5 | Add the `B-` family and the missing ids | blocking |
| CV-14 | `10-build-dependencies-testing.md` | §6.5 gate→test map | Omits `S-11`, `S-12`, `T-EXPR-1`, `T-PTEX-1`, all four registered in `09` §5.3–§5.4 | `09-…` §5 | Add four rows with their CTest names | major |
| CV-15 | `11-roadmap.md` | §2.0; §6.4 RC-1 | The milestone-exit table omits `SI-6`, `SI-7`, `S-11`, `S-12`, `T-EXPR-1`, `T-PTEX-1`, though §2.2's prose does exit on SI-6/SI-7. RC-1 lists "S-1 … S-9" and so omits `S-11`/`S-12` | `09-…` §5.5 | Add the missing ids to §2.0 and RC-1 | major |
| CV-16 | `06-imaging.md` | §10.1 SI-9 row; §10.2 `testUsdGenPruningCost` | Defines **`SI-9` as the pruning-wrapper cost only**, driven by `testUsdGenPruningCost`. `09` §5.2 defines SI-9 as *two scene-index instances agreeing on generation, prim set and frame* **plus** the pruning cost, driven by `testUsdGenSessions` | `09-…` §5.2 | Adopt the two-part metric and name both test binaries | major |
| CV-17 | `10-build-dependencies-testing.md` | §6.5 `SI-9` row | Same single-part definition and `testUsdGenPruningCost` as the only driver | `09-…` §5.2 | As CV-16 | major |
| CV-18 | `10-build-dependencies-testing.md` | §6.5 `S-6`, `SI-5` rows | `S-6` exits **M1** and `SI-5` exits **M1**; `09` §5.3/§5.2/§5.5 place `S-6` at **M2** and `SI-5` at **M0**. 10 claims both are "the R40 reconciliation", which R40's text does not say | `09-…` §5 (R40) | Reconcile with 09 or file the amendment against 09 explicitly | major |
| CV-19 | `11-roadmap.md` | §2.0 M1/M2 rows | Puts `S-6` in M1's exit set; `09` §5.5 puts it in M2's | `09-…` §5.5 | Move `S-6` to the M2 row | major |
| CV-20 | `09-performance-and-benchmarks.md` | §5.4 T-INST-1 row | Test binary is `testUsdviewUsdGenInstancerPick`; `10` §5.6/§6.5 registers `testUsdGenInstancerPick` (T1) with `testUsdviewUsdGenCards.py` as the T3 app-level script | `10-…` §5.6 (the CTest registry) | Use `testUsdGenInstancerPick` for the T1 gate | minor |
| CV-21 | `06-imaging.md` | §10.1 notes under S-4 and T-INST-1 | Says `09-…` §5.3/§5.4 and `10-…` §6.5 "still say M3" for S-4 and "record T3" for T-INST-1. Both now say M2 and T1 *(stale claim)* | `09-…` §5.3/§5.4 | Delete the two reconciliation notes | minor |
| CV-22 | `05-static-curves-and-deformation.md` | §4.2 case 2; §11 sources | Says "`09-…` §5.2 stops at SI-8 and does not list SI-9" and "09 §5.1 (E-1 at 2.5 ms)". 09 §5.2 carries SI-9 and §5.1 carries ≤ 1.5 ms *(stale claim)* | `09-…` §5.1, §5.2 | Delete both claims | minor |
| CV-23 | `03-execution-engine.md` | §12.1 correction I | Says `09` §5 reads "E-1 ≤ 2.5 ms at `:531`", "E-3 M1 at `:534`", "E-8 M0 at `:539`". 09 §5.1 now reads ≤ 1.5 ms, E-3 M2, E-8 M1 *(stale claim)* | `09-…` §5.1 | Delete correction I | minor |
| CV-24 | `08-tools.md` | §11 item 3 | Says "`09-…` §5.4 also still shows **T-INST-1 at tier T3**". 09 §5.4 shows T1 *(stale claim)* | `09-…` §5.4 | Delete the second half of item 3 | minor |
| CV-25 | `09-performance-and-benchmarks.md` | §8 closing paragraph | Says `benchUsdGenMaps` and `testUsdGenMaps` "are new here and must be added" to `10` §5.6, and that `testUsdGenSessions`' SI-9 role must be added. `10` §5.6 already registers all three *(stale claim)* | `10-…` §5.6 | Delete the sentence | minor |
| CV-26 | `11-roadmap.md` | §5.2 SC-7b | Cites "`design/proposal-performance.md` §13 **R-9**". `R-n` with a hyphen is the render-gate family (`R-1…R-3`); a proposal risk id must be bare (`R9`), as `10` §8.4 requires | `10-…` §8.4 collision table | Write `R9` | minor |

### 2.3 Stop conditions

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-27 | `05-static-curves-and-deformation.md` | §4.2 case 2; §9 T1 table | Mints **`SC-11`** = "SI-9 red ⇒ execute the UsdSkel aggregator ourselves", and says `11-roadmap.md` §5.2 "today stops at SC-10". `12` §2.1 independently mints **`SC-11`** = the M1 engine floor and **`SC-12`** = M0 chain order. Two documents assign different meanings to `SC-11` | `11-roadmap.md` §5 is the SC register (`12` §2.1 says so explicitly) | 11 §5.2 must adopt one numbering; 05 and 12 renumber to whatever 11 assigns | blocking |
| CV-28 | `12-risks-decisions-open-questions.md` | §2.1 items 2–3 | Same collision from the other side: requests `SC-11`/`SC-12` without knowing 05 has taken `SC-11` | `11-roadmap.md` §5 | As CV-27 | blocking |
| CV-29 | `11-roadmap.md` | §5.2 | The register stops at `SC-10`, so both requested extensions are unregistered and the reference-integrity test `12` §5 defines ("every `SC-<n>` exists in `11-roadmap.md` §5") fails by construction | `11-roadmap.md` §5 | Add the agreed SC-11/SC-12 (and SC-13 if three are needed) | major |

### 2.4 `TfDebug` codes, `TfTrace` scopes, env vars

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-30 | `06-imaging.md` | §9 diagnostics table, `TF_DEBUG` row | Lists `USDGEN_GRAPH, USDGEN_CAPTURE, USDGEN_DIRTY, USDGEN_PUBLISH, USDGEN_SESSION, USDGEN_MAPS, USDGEN_EXPR, USDGEN_TIMING` and calls it "the registry list". `10` §7.2 retires five of those and declares six codes; `03` §9.2 and `09` §6.2 print the same six | `10-…` §7.2 (R35): `USDGEN_COMPILE, USDGEN_DIRTY, USDGEN_COMMIT, USDGEN_PUBLISH, USDGEN_CAPTURE, USDGEN_MEMORY` | Replace the row; §3.7's "`USDGEN_SESSION` debug line" becomes `USDGEN_COMPILE` or `USDGEN_COMMIT` | blocking |
| CV-31 | `06-imaging.md` | §9 env row | Cites "`10-build-dependencies-testing.md` **§7.2** owns the single env-var registry". The registry is `10` §3.5; §7.2 is the diagnostics section. R45 requires cross-references to resolve | `10-…` §3.5 | Re-point to §3.5 | minor |
| CV-32 | `10-build-dependencies-testing.md` | §7.2 trace-scope paragraph | Names `TfTrace`/`TRACE_SCOPE` scopes **after gates** (`UsdGen::E-1::Evaluate`, `UsdGen::SI-3::Commit`). `03` §9.2 and `09` §6.2 declare a seven-row table of **plain** scope names (`UsdGen::Route`, `UsdGen::Compile`, `UsdGen::Capture`, `UsdGen::Evaluate<opType>`, `UsdGen::Interleave`, `UsdGen::Publish`/`Diff`, `UsdGen::Motion`) and 09 states they are "one table, two documents" | `03-…` §9.2 = `09-…` §6.2 (two documents already agree; 03 owns engine names) | Adopt the plain names, with the gate carried in the table's Gate column as 03/09 do | major |
| CV-33 | `06-imaging.md` | §9 trace-scope row | Uses the gate-qualified form `UsdGen::SI-3::Commit`, `UsdGen::SI-1::Publish`, `UsdGen::S-2::Interleave` | `03-…` §9.2 / `09-…` §6.2 | As CV-32 | major |
| CV-34 | `03-execution-engine.md` | §12.1 correction C | Demands `10` add `USDGEN_OP_CHECKS` to the env registry and states "Verified absent: `grep -n USDGEN_OP_CHECKS 10-…` returns nothing". `10` §3.5 carries it today *(stale claim)* | `10-…` §3.5 | Delete correction C | minor |
| CV-35 | `04-operators.md` | §0.3 rule 1; §0.13 row 7 | Cites the `USDGEN_OP_CHECKS` registry row as "`10-build-dependencies-testing.md` §1" (it is §3.5) and files an owed edit to add a variable that is already there *(stale claim + wrong section, R45)* | `10-…` §3.5 | Re-point; delete row 7 | minor |
| CV-36 | `09-performance-and-benchmarks.md` | §6.2 `UsdGen::Compile` row | Covers "Merkle digest, Kahn sort, sub-graph rebuild"; `03` §3.3 adds a fourth **graph digest** and §12.1 correction B asks 09 to say "Merkle **+ graph** digests" | `03-…` §3.2–§3.3 (engine names) | Amend the row | minor |

### 2.5 C++ class, function and struct names

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-37 | `06-imaging.md` | §3.11 class declaration | Declares `class UsdGenLiveOverride`, while its own §3.1 member is `UsdGenLiveOverrideStore _liveOverrides` and `01` §5.1's target table lists `UsdGenLiveOverrideStore` | `01-…` §5.1 and `06-…` §3.1 (`UsdGenLiveOverrideStore`) | Rename the class declaration | major |
| CV-38 | `06-imaging.md` | §8 extent hook and registry function | Names the setter `UsdGenSchema_SetExtentDelegate(UsdGenExtentDelegate)` and the extent function `::_ComputeUsdGenDescriptionExtent`. `10` §4.3 names `void UsdGenSchema_SetExtentProvider(UsdGenSchemaExtentFn)` and `02` §7.2 / `10` §1.2 name `UsdGenSchema_ComputeDescriptionExtent` | `10-…` §1.2, §4.3 (build names) and `02-…` §7.2 (schema names) | Adopt `UsdGenSchema_SetExtentProvider` / `UsdGenSchemaExtentFn` / `UsdGenSchema_ComputeDescriptionExtent` | major |
| CV-39 | `03-execution-engine.md` | §3.1 step 6 | Says the compiler binds targets "to `UsdGenGraphDesc::curves` indices"; the field declared in its own §2.2 is `curveSets` (`std::vector<UsdGenCurveSetDesc>`), per R23. Internal contradiction in the document that owns the name | `03-…` §2.2 (`curveSets`) | Rename in step 6; also fix `UsdGenNodeDesc::curves`'s doc comment which points at "`UsdGenGraphDesc::curves`" | major |
| CV-40 | `05-static-curves-and-deformation.md` | §2.2 `UsdGenSourceArrays` | Declares `std::string frozenEpoch, role;`. `03` §2.2's `UsdGenCurveSetDesc` uses `TfToken curveRole` for the same C3 marker, and `03` §12.1 correction F asks 05 to rename. The C3 role token has two C++ types across the two documents | `03-…` §2.2 (`TfToken curveRole`) | Rename to `TfToken curveRole` | major |
| CV-41 | `05-static-curves-and-deformation.md` | §2.2 `UsdGenSourceArrays` | Declares `VtArray<uint64_t> curveId`; `03` §2.2 declares `VtUInt64Array curveId` for the same array | `03-…` §2.2 (`VtUInt64Array`) | Use `VtUInt64Array` | minor |
| CV-42 | `05-static-curves-and-deformation.md` | §4.7.1; §11 sources | Says "`03-execution-engine.md` §2.2 and §3.1 step 6 still write `curves` / **`UsdGenCurveDesc`**". 03 uses `UsdGenCurveSetDesc` throughout; only the `curveSets`/`curves` half is still true *(half-stale claim)* | `03-…` §2.2 | Narrow the claim to the `curveSets` rename (CV-39) | minor |
| CV-43 | `07-look-maps-expressions.md` | §7.4 | Writes `UsdGenMath::Noise3`, `Fbm3`, `CellNoise3`, `Voronoi3` — a C++ namespace or class `UsdGenMath` that no sibling declares. `10` §7.1 fixes the file-scope namespace as `namespace usdGen { … }`, and `usdGenMath` is a CMake target name; `03` §8.3 declares kernels as free functions `UsdGenXxxKernel(...)` | `10-…` §7.1 (namespace) and `03-…` §8.3 (free-function kernels) | Spell them `usdGen::UsdGenNoise3` (or plain free functions) and stop using the target name as a scope | major |
| CV-44 | `07-look-maps-expressions.md` | §1.2 | States `UsdGenHash01` is declared `inline float UsdGenHash01(int seed, uint32_t curveId, uint32_t salt)` "(`04-operators.md` §0.6)" and introduces the salt `kSaltLookJitter`. `04` §0.6 declares `UsdGenHash01(uint64_t key, uint32_t salt)` and a three-argument `UsdGenDraw01(int seed, uint64_t curveId, uint32_t salt)`; its salt table has no `kSaltLookJitter` | `04-…` §0.6 (the salt table and hash signatures) and `02-…` §2.19.1 | Use `UsdGenDraw01`; add `kSaltLookJitter` to 04 §0.6's table | major |
| CV-45 | `02-schema.md` | §2.13 mask pseudocode | Calls `UsdGenHash01(mask:randomSeed, curveId(h), kSaltMaskRandom)` — three arguments — while its own §2.19.1 declares the two-argument form. `04` §0.13 row 4 raises the same defect | `04-…` §0.6 `UsdGenDraw01(seed, curveId, salt)` | Use `UsdGenDraw01` in the pseudocode | major |
| CV-46 | `04-operators.md` | §0.2; §2.2 | Proposes the rename `usdGen:direction` → **`usdGen:growDirection`** on `UsdGenGrow`, "due before C1". No sibling declares `usdGen:growDirection`; `02` §2.6 declares `usdGen:direction` | `02-…` §2.6 (`usdGen:direction`, `uniform token`, `"surfaceNormal"`) | Either fold the rename into 02 or drop the proposal; C1 freezes at M1 | major |
| CV-47 | `04-operators.md` | §1.2 `UsdGenInstance` row; §2.8 Interactions | Emits **`protoIndices`** and uses **`protoSelect = "byClumpId"`**. `06` §4.3 (contract C2 for instancers) and `02` §2.11 name `instancerTopology/instanceIndices` and declare no `protoSelect` property | `06-…` §4.3 and `02-…` §2.11 | Replace `protoIndices` with `instancerTopology/instanceIndices`; drop or register `protoSelect` | major |
| CV-48 | `08-tools.md` | §1.5 code fence header | Names the pxr_boost source `python/module_usdGen.cpp`; `10` §1.1 and §3.4 name `python/_usdGen.cpp` | `10-…` §1.1 | Rename to `python/_usdGen.cpp` | minor |

### 2.6 Schema property names, types, defaults and tokens

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-49 | `07-look-maps-expressions.md` | §1.1 `usdGen:look:colorRamp:interpolation` | Default `"linear"`, allowed `constant \| linear \| smooth \| monotoneCubic`, attributed to "`02-schema.md` §2.14". This is the **pre-R11 token set**. `02` §2.14 declares default `"catmullRom"` with `linear \| catmullRom \| bspline \| constant` | ADR §9 **R11**; `02-…` §2.14, §2.17 | Adopt `"catmullRom"` and R11's four tokens | blocking |
| CV-50 | `07-look-maps-expressions.md` | §1.1 rows for `bakeMode`, `colorMapMode` | Types them plain `token`; `02` §2.14 declares both `uniform token`. `07` §1.6's `.usda` example authors `token usdGen:look:bakeMode` and `token usdGen:look:colorMapMode` | `02-…` §2.14 (R7) | Use `uniform token` in the table and the example | major |
| CV-51 | `02-schema.md` | §2.12 `UsdGenMap` block and subtype rows | Lacks four properties `07` §5.1 declares and uses in its error policy: `float usdGen:map:default = 0.0`, `token usdGen:noise:space = "rest"` (`rest \| deformed`), `int usdGen:paint:resolution = 256`, `token usdGen:paint:storage = "primvar"` (`primvar \| file`); and declares no per-declaring-type `allowedTokens` for `usdGen:map:filter`, which `UsdGenImageMap` and `UsdGenPtexMap` give different token sets | ADR §9 **R7/R8** (02 gains what a sibling needs) | Fold the four properties and the two token lists into §2.12 before C1 | blocking |
| CV-52 | `02-schema.md` | §2.6–§2.8, §2.13 | Lacks eighteen operator parameters `04-operators.md` marks ᴾ and specifies kernels for: `usdGen:flip`, `usdGen:perGuide` (Scatter); `usdGen:directionVector`, `usdGen:directionPrimvar`, `usdGen:uvBlend` (Grow); `usdGen:useUniqueGuide` (GuideInterpolate); `usdGen:lockRoots` (Deform); `usdGen:searchRadius`, `usdGen:numNeighbors` (Smooth); `usdGen:direction:source`, `usdGen:direction:knots` (Direction); `usdGen:scaleRandom`, `usdGen:widthToo` (Scale); `usdGen:restoreSegmentLengths` (Resample); `usdGen:mask:influenceWidth`; `usdGen:part:curves/radius/strength`; `usdGen:mode` on `UsdGenDisplace` and on `UsdGenExprOp`; `usdGen:expr:returnType` on `UsdGenExprOp` | ADR §9 R7/R8; `04-…` §0.13 rows 2, 3, 6 | Fold in or delete from 04; C1 freezes at M1 | blocking |
| CV-53 | `02-schema.md` | §2.7.1 note under `UsdGenLength` | "`04-operators.md` §2.10 currently spells `usdGen:length:mode` with a six-token `set\|add\|subtract\|multiply\|cutAbsolute\|cutRelative` set and default `"multiply"`". 04 §2.10 now carries 02's three-token `set \| scale \| cull` set with default `"scale"` *(stale claim)* | `02-…` §2.7.1 | Delete the note (04 §0.13 row 5 asks for exactly this) | minor |
| CV-54 | `02-schema.md` | §6.4 `usdGen/densityScale`, `usdGen/renderDensityScale` rows | Classed **capture** ("re-run the stable-id decimation predicate"). ADR §9 **R13** says the scales never re-scatter and never re-chunk — only `UsdGenChunkDesc::liveCount` moves — which is the **value** class. `03` §3.6 and §12.1 correction A both flag it, and R25 generates the shipped router from this table | ADR §9 **R13**; `03-…` §1.2, §3.6 | Reclassify both rows **value**; keep `usdGen/density` capture | blocking |
| CV-55 | `05-static-curves-and-deformation.md` | §3.1 opening line | "`UsdGenRestAPI` … Applied to the bound surface (a `Mesh`, **or a `GeomSubset` of one**)". ADR §9 **R15** and `02` §2.15 and `06` §2.6 forbid it: `UsdGenRestAPI` may be applied **only to the parent `Mesh`**, and `06` §10.2's `testUsdGenRestAdapter` asserts the refusal | ADR §9 **R15**; `02-…` §2.15 | Delete "or a `GeomSubset` of one" | blocking |
| CV-56 | `05-static-curves-and-deformation.md` | §3.1 `usdGen:rest:file` row | Default printed as "—"; `02` §2.15 declares the default `@@` (an empty asset path) | `02-…` §2.15 | Print `@@` | minor |
| CV-57 | `04-operators.md` | §0.7 third bullet | States "the C++ type-level fallback is `UsdGenReadPhase::**Preceding**` (`03-execution-engine.md` §8.1)". `03` §8.1 declares `virtual UsdGenReadPhase ReadPhase() const { return UsdGenReadPhase::Final; }` and §1.6 says the type fallback and the authored default can never disagree | `03-…` §8.1 (`Final`); ADR §9 **R9** | Change to `Final` | blocking |
| CV-58 | `04-operators.md` | §0.7 third bullet | States `preceding` "resolve[s] to `base` for a rest-class node and to `final` for a deformed-class node (`03-execution-engine.md` §1.6)". `03` §1.6 and ADR §9 **R9** make `preceding` an unconditional **alias for `final`** in v1, rewritten at compile with one `TF_WARN` | ADR §9 **R9**; `03-…` §1.6 | Restate as the v1 alias | blocking |
| CV-59 | `03-execution-engine.md` | §12.1 correction E | Asks `02` §2.5 to mark `preceding` a v1 alias; `02` §2.5 already reads "`preceding` is accepted in v1 as an **alias for `final`** (ADR §9 R9) and is not a distinct phase" *(stale claim)* | `02-…` §2.5 | Delete correction E | minor |
| CV-60 | `03-execution-engine.md` | §12.1 correction G | Says "`02-schema.md` §2.17's scalar-ramp row lists `constant \| linear \| smooth \| monotoneCubic`". §2.17 now lists R11's `linear \| catmullRom \| bspline \| constant`, default `catmullRom` *(stale claim)* — the defect has migrated to `07` §1.1 (CV-49) | `02-…` §2.17 | Re-target correction G at `07` §1.1 or delete it | minor |
| CV-61 | `05-static-curves-and-deformation.md` | §1.6 | "`02-schema.md` §5 (line ~1392) still carries `int[]` and must be corrected before C3 freezes". `02` §5 declares `uint64[] primvars:usdGen:curveId` *(stale claim)* | `02-…` §5 | Delete | minor |
| CV-62 | `05-static-curves-and-deformation.md` | §2.4 last bullet | "`04-operators.md` §0.6 still writes the older murmur3 form `UsdGenHash32(curveId ^ kDensitySalt)` with `kDensitySalt = 0xDEC1A7E5u`". 04 §0.6 carries R12's pinned SplitMix64 and `kSaltDensity` *(stale claim)* | `04-…` §0.6 | Delete | minor |
| CV-63 | `05-static-curves-and-deformation.md` | §3.1 | "`02-schema.md` §7.3's plugInfo block still spells all six `UsdGenImaging*` and must be corrected". §7.3 spells R3's `UsdGenOperatorAdapter` … `UsdGenRestAPIAdapter` *(stale claim)* | `02-…` §7.3 | Delete | minor |
| CV-64 | `05-static-curves-and-deformation.md` | §5.3 | "`02-schema.md` §2.9's freeze table (`:565`) still reads 'the evaluator falls back to `live`'". §2.9 reads "the evaluator **keeps rendering the frozen data**" *(stale claim)* | `02-…` §2.9; ADR §9 R18 | Delete | minor |
| CV-65 | `05-static-curves-and-deformation.md` | §6.1 | "02 §2.10 and `04-operators.md` §2.7 still carry only the other seven rows" for `usdGen:sculpt:rootPrims` / `rootUVs`. Both carry all nine today *(stale claim)* | `02-…` §2.10; `04-…` §2.7 | Delete | minor |
| CV-66 | `05-static-curves-and-deformation.md` | §6.4 | Writes `curveMask[c] = clamp(base(c) * rnd(c) * nse(c), 0, 1) * lockedCurveSuppression(c)` — suppression **outside** the clamp. `04` §5.2, which ADR §9 **R16** makes the executable form and requires to be arithmetically identical to `02` §2.13, folds it **inside** | `04-…` §5.2 (R16) | Move inside the clamp | major |
| CV-67 | `03-execution-engine.md` | §8.3 rule 7 | Evaluates the ramp with a **lerp** between `rampLut[j]` and `rampLut[j+1]` and states that "reading `rampLut[round(t)]` would throw that away". `02` §2.13 and `04` §5.2 both write `rampLUT[round(t * 256)]` — a nearest read. R16 makes 02 canonical for the mask arithmetic | `02-…` §2.13 and `04-…` §5.2 | Reconcile: either 02/04 adopt the lerp (preferable, and 03 gives the reason) or 03 adopts the round; the three must be identical | blocking |
| CV-68 | `08-tools.md` | §11 item 2 | Asks `02` to delete `usdGen:interactive:maxCurves`, widen `curveId`/`sculpt:curveIds`/`lockedCurves` to `uint64[]`, and add `sculpt:rootPrims`/`rootUVs`. `02` carries none of the first, all of the second and both of the third *(stale claim)* | `02-…` §2.3, §2.10, §2.16, §5 | Delete item 2 | minor |
| CV-69 | `02-schema.md` | §2.9 `usdGen:frozen:tier` doc | Still names S42's "T1/T2/T3" landing-tier labels in prose. ADR §9 **R1** retires them ("Freeze landing tiers are the tokens `session\|sublayer\|payload` only; S42's 'T1/T2/T3 landing tiers' are retired from prose") | ADR §9 R1 | Keep the disambiguation sentence but drop the retired labels, as `04` §2.6 and `08` §4.3 do | minor |

### 2.7 Emitted-primvar naming (contract C1 vs contract C2)

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-70 | `02-schema.md` | §2.6 (`UsdGenGuideInterpolate`), §2.7.1 (`UsdGenClump`) | Declares the emitted primvars as `int[] primvars:usdGen:guideIndex`, `float[] primvars:usdGen:guideWeight` and `int[] primvars:usdGen:clumpId_<level>`. The ADR (§2.3, §9 **R24**), `03` §1.2/§6.3 and `06` §4.1 (**contract C2**) publish them bare — `primvars/clumpId_<level>`, `primvars/guideIndex`, `primvars/guideWeight`. C1 and C2 therefore freeze two different names for one primvar at the end of M1 | ADR §2.3 / §9 **R24** and `06-…` §4.1 (C2) | Drop the `usdGen:` prefix on the three emitted primvars, or amend C2; one spelling only | blocking |
| CV-71 | `04-operators.md` | §2.3 Emitted primvars | Same prefixed spelling `primvars:usdGen:guideIndex` / `guideWeight`, while §1.2, §0.5 and §2.8 of the same document write the bare `clumpId_<n>`, `guideIndex`, `guideWeight`. Internally inconsistent | ADR §9 R24 / `06-…` §4.1 | Use one spelling throughout | major |
| CV-72 | `05-static-curves-and-deformation.md` | §7.2 second bullet | Same prefixed spelling `int[] primvars:usdGen:guideIndex` / `float[] primvars:usdGen:guideWeight` | ADR §9 R24 / `06-…` §4.1 | As CV-70 | major |

### 2.8 CMake targets, plugin registration, install layout

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-73 | `01-architecture.md` | §5.1 `usdGenMath` row | States `usdGenMath` has "self-contained **noise kernels**" and "Links **no SeExpr**". `10` §1.2 and §1.6 link `usdGen_seexpr` **into** `usdGenMath` (SeExpr's `Noise.cpp` is the single noise implementation, S38) and make the M0/M4 target schedule depend on it | `10-…` §1.2, §1.6 (build names, R35-adjacent) | Correct the Links and Contents cells; the M0 scheduling sentence in §5.2 ("`usdGenMath` does not link SeExpr") also falls | blocking |
| CV-74 | `01-architecture.md` | §5.1 `usdGenTestUtils` row | One target linking `hdSt hgiGL usdImagingGL` and holding `eglctx.h`. `10` §1.2 splits it into `usdGenTestUtils` (T0, links `usdGen` only, itself covered by gate **B-1**) and `usdGenTestUtilsHd` (T1/T2/T3, links Hydra/GL) and states that a single target "would drag the whole Hydra/GL stack into every tier-0 executable" | `10-…` §1.2 | Adopt the two-target split; add `usdGenTestUtilsHd` to §5.1 and to §9's tier table | major |
| CV-75 | `01-architecture.md` | §5.1 `usdGenShaders` row | Kind is "resource plugin". `10` §1.2 and §4.4 make it a `"Type": "library"` plugin compiling `UsdGenShadersDiscoveryPlugin` (an `SdrDiscoveryPlugin`) into `usdGenImaging`, because shader defs must be discoverable by `info:id` | `10-…` §4.4 | Correct the Kind cell and name the discovery plugin | major |
| CV-76 | `01-architecture.md` | §5.1 `usdGenUsdview` row | Installs to `lib/python/**usdGenUsdview**/`. `08` §1.1 and `10` §1.2/§3.4/§4.4 install the module *inside* the `usdgen` package at `lib/python/usdgen/`, with the plugInfo at `lib/python/usdgen/plugInfo.json` | `10-…` §1.2, §3.4; `08-…` §1.1 (R5) | Correct the install path | major |
| CV-77 | `01-architecture.md` | §5.1 `usdGen` row | Links cell reads `ar sdf vt gf tf work trace hio pxOsd`; `10` §1.3 lists `arch js plug tf gf vt sdf ts ar work trace hio pxOsd` as the **direct** set (with `hf`, `pegtl`, `python` transitive) | `10-…` §1.3 | Align the list | minor |
| CV-78 | `02-schema.md` | §7.1 CMake fence | `target_link_libraries(usdGenSchema PRIVATE usd usdGeom tf gf vt) # and nothing else (gate B-1)`. `10` §1.2 additionally links `usdGenMath` (for the extent kernel body) and declares `tf usd usdGeom` as the pxr set. "and nothing else" is false under 10 | `10-…` §1.2 (build names) | Add `usdGenMath` and drop the "nothing else" absolute, or move the extent kernel out of `usdGenMath` | major |
| CV-79 | `06-imaging.md` | §1.4 usdview plugInfo JSON | Registers `"usdGenUsdview.UsdGenUsdviewContainer"` under `"Name": "usdGenUsdview"`, `"Root": "."`, `"ResourcePath": "."`. `10` §4.4 registers `"Name": "usdgen.usdGenUsdview"` from `lib/python/usdgen/plugInfo.json`, and `08` §1.1 gives the type as `usdgen.usdGenUsdview.UsdGenUsdviewContainer`. Plug loads a python plugin with `import <Name>`, so the two spellings load different modules | `10-…` §4.4 and `08-…` §1.1 (R5) | Rewrite the JSON to the `usdgen.` package-qualified form | blocking |
| CV-80 | `06-imaging.md` | §3.4 generation handoff | Uses `std::atomic<std::shared_ptr<const UsdGenGeneration>>::load/store` (a **C++20** facility) and cites "`10-build-dependencies-testing.md` §3.6 fixes the language standard". `10` §3.1 sets `CMAKE_CXX_STANDARD 17`, and §3.6 is `bin/_env.sh`, not a standard statement. `03` §6.1 uses the free `std::atomic_load`/`atomic_store` (C++17) and records the C++20 caveat | `10-…` §3.1 (C++17) and `03-…` §6.1 | Either use the free functions or raise the standard in `10` §3.1; fix the §3.6 cross-reference (R45) | blocking |
| CV-81 | `06-imaging.md` | §8 "Reconciliation note" | "`02-schema.md` §7.2 and `10-build-dependencies-testing.md` §1.2 and §4.3 still carry the pre-R19 wording (the extent function in `usdGenImaging`, `LibraryPath` → `usdGenImaging`, `usdGenSchema` as resources only)". All three now conform to R19 *(stale claim)* | `02-…` §7.1–§7.2; `10-…` §1.2, §4.3 | Delete the note | minor |
| CV-82 | `10-build-dependencies-testing.md` | §11 conflict 1 | "`02-schema.md` §7.2–§7.3 and `06-imaging.md` §7 still place `UsdGeomRegisterComputeExtentFunction` and the schema `LibraryPath` in `usdGenImaging`". 02 §7.1–§7.2 place both in `libusdGenSchema.so`, and 06's extent section is **§8**, not §7, and also places them there *(stale claim + wrong section)* | `02-…` §7.1–§7.2; `06-…` §8 | Delete conflict 1 | minor |
| CV-83 | `10-build-dependencies-testing.md` | §11 conflict 5 | "`00-request-and-scope.md` §4.2 **and `02-schema.md` §8.6** still say `usdGenPy`". 02 §8.6 says `usdgen.migrate(stage, toVersion)` in the `usdgen` package; only 00 still says `usdGenPy` *(half-stale)* | `02-…` §8.6 | Narrow to 00 §4.2 (see CV-84) | minor |
| CV-84 | `00-request-and-scope.md` | §4.2 "Python facade shape" row | Names "the **`usdGenPy`** facade package (ADR §6)". ADR §9 **R5** retires the name: the package is `usdgen`, all lowercase, and "`usdGenPy` is not a name" | ADR §9 **R5**; `10-…` §1.1 | Rename to `usdgen` (and to `usdgen.builder`, the `rigexec.Builder` facade of `10` §1.2) | blocking |
| CV-85 | `10-build-dependencies-testing.md` | §5.6 T1 row | Registers `testUsdgenArrays` and `testUsdgenAbi` with a lowercase `g`, unlike every other `testUsdGen*` name in the same table | `10-…` §5.6 own convention | Rename `testUsdGenArrays`, `testUsdGenAbi` | minor |

### 2.9 Test-binary names

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-86 | `08-tools.md` | §9.3 preamble and table | Claims "The script names below are `10-build-dependencies-testing.md` §5.6's registry **verbatim**", then names `testUsdviewUsdGen{Panels,Brush,Freeze,Undo,PickAccuracy,InstancerPick,Async,Look}.py`. `10` §5.6 registers `testUsdviewUsdGen{Activate,Stack,Comb,Symmetry,Paint,Freeze,Pick,Cards,Async,Look}.py`. Only `Freeze`, `Async` and `Look` match | `10-…` §5.6 (the CTest registry) | Reconcile the ten T3 script names in one place | blocking |
| CV-87 | `10-build-dependencies-testing.md` | §6.5 T-1, T-3 rows | Drives T-1 with `testUsdviewUsdGenComb.py` and T-3 with `testUsdviewUsdGenPick.py`; `09` §5.4 and `08` §8.2 drive them with `testUsdviewUsdGenBrush` and `testUsdviewUsdGenPickAccuracy` | one of the two; `10` §5.6 is the CTest registry and `09` §5 the gate registry — they must agree | Pick one pair and use it in 08, 09 and 10 | major |
| CV-88 | `11-roadmap.md` | §0.2, §2.3, §7 | Names six test binaries that appear in no CTest registry: `testUsdGenContracts`, `testUsdGenAbiSymbols`, `testUsdGenFreezeUndo`, `testUsdGenKernels`, `benchChain`, `benchKnn` (the last two are `benchUsdGenChain` / `benchUsdGenKnn` in `10` §5.6) | `10-…` §5.6 | Register the first three; rename the last three | major |
| CV-89 | `10-build-dependencies-testing.md` | §5.6 T1 row | Omits `testUsdGenInstanceKeys`, `testUsdGenSurfaceResolver` and `testUsdGenMotionSamples`, which `06` §10.2 defines and flags as owed additions | `06-…` §10.2 (imaging names) | Add the three | minor |
| CV-90 | `10-build-dependencies-testing.md` | §11 conflict 4 | Records that `07` §9.2 names the T2 look tests `testUsdGenStormLook` and `testUsdGenMaterialBinding` while the registry keeps `testUsdGenStormMaterial` — the conflict is real and unresolved; `testUsdGenMaterialBinding` is defined nowhere else | `10-…` §5.6 (`testUsdGenStormMaterial`) | 07 §9.2 adopts `testUsdGenStormMaterial` | minor |
| CV-91 | `appendix-B-prototype-inventory.md` | §0.1 index, "Carried into" column | Carries prototypes into `usdGenUsdview/usdGenUndo.py` (the module lives at `usdgen/usdGenUndo.py`), `usdGenMath/tests/benchChain.cpp` (registry name `benchUsdGenChain`) and `tests/perf/benchMotion.cpp` (`11` §2.8 says `tests/perf/benchMotionSamples.cpp`) | `10-…` §1.1, §5.6 | Align the three names | minor |

### 2.10 Workstation-protocol numbering

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-92 | `08-tools.md` | §8.3, §9.5 | Numbers the workstation protocols `W1`–`W4`. `09` §4.3 numbers five items `(1)`–`(5)`; `10` §5.2 numbers `docs/workstation-protocol.md` §§1–8 and maps `R-1 → §7`, `R-2 → §3`, `R-3 → §5`. Three schemes for one document | `10-…` §5.2 (which declares its own numbering and asks the other two to re-point) | Re-point `W1`–`W4` onto `docs/workstation-protocol.md` §§ | major |
| CV-93 | `09-performance-and-benchmarks.md` | §4.3; §5.4 R-1/R-2/R-3 "Test" cells | Cites "workstation protocol §5", "§2", "§3–4" against `10` §5.2's `R-1 → §7`, `R-2 → §3`, `R-3 → §5` | `10-…` §5.2 | Re-point | major |

### 2.11 Vocabulary rulings R1, R21, R38, R44

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-94 | `00-request-and-scope.md` | §5.2 (four uses), §6.1 | Uses the retired engine-principle names: "(P3, ADR §4.1)", "(P4)", "(P5)", "(S17, P7)" and "invariants **P1–P8**". ADR §9 **R1** renames them `I1–I8` and reserves `P0/P1/P2` for motion profiles only | ADR §9 **R1**; `01-…` §6 | Replace with `I1`–`I8` | blocking |
| CV-95 | `00-request-and-scope.md` | §5.2 "Chunk vs tile" | `nTiles = clamp(ceil(nChunks / chunksPerTile), 32, 256)` — missing ADR §9 **R21**'s outer `min(nChunks, …)` term, which every other document carries | ADR §9 **R21** | Add the `min(nChunks, …)` | blocking |
| CV-96 | `00-request-and-scope.md` | §2.1 version table | Places `UsdGenInstance`, `UsdGenPtexMap` and motion profile **P1** in **v2**, and calls `UsdGenScatter mode = uniform` "unscheduled". ADR §9 **R38** puts `UsdGenPtexMap` (M4), `UsdGenInstance` (M6) and P0/P1/P2 (M7) in **v1**, and `uniform` in **v2/M8**. **R44** explicitly requires 00's version table to match R38 | ADR §9 **R38**, **R44**; `02-…` §2.6, §2.11, §2.12; `04-…` §1.2; `11-…` §6.1 | Rewrite the table to R38 | blocking |
| CV-97 | `07-look-maps-expressions.md` | §5.4, `UsdGenMapEvalContext` paragraph | "one per worker in the private `tbb::task_arena` (ADR §4.1, **P8**)". R1 renames the invariant `I8` | ADR §9 **R1** | Write `I8` | minor |
| CV-98 | `07-look-maps-expressions.md` | §5.4 `UsdGenMapSample` | Declares `int curveId;`. ADR §9 **R12** makes the stable id `uint64`, and `03` §1.2 uses `VtUInt64Array curveId` | ADR §9 **R12** | `uint64_t curveId;` | major |
| CV-99 | `07-look-maps-expressions.md` | §11 Prototypes list | Names `usdGenHairPreview_translucent.glslfx`; §2.1, `10` §1.2/§3.4 and `11` §2.2 name `usdGenHairPreviewTranslucent.glslfx` (R4) | ADR §9 **R4**; `10-…` §3.4 | Rename | minor |
| CV-100 | `11-roadmap.md` | §2.6 scope; §7 bullet 1 | "the **eighteen** entry points of `08-tools.md` §1.4" and "C4's **eighteen** `UsdGenImaging_*` entry points". `08` §1.4 lists **nineteen** | `08-…` §1.4 (R31) | Write nineteen | minor |
| CV-101 | `11-roadmap.md` | §2.1 scope | "All **eleven** ADR §6 target names". `10` §1.2 lists **thirteen** targets and `01` §5.1 twelve; §2.1's own M0 list also omits `usdGenTestUtilsHd`, which `10` §1.6 adds at M0 | `10-…` §1.2, §1.6 | Correct the count and the M0 list | minor |
| CV-102 | `04-operators.md` | §1.2 footnote ² | "`UsdGenScale`'s milestone is an **ASSUMPTION**: neither ADR §7 nor `11-roadmap.md` §6.1 schedules it". `11` §2.5 scopes `UsdGenScale` into M4 and §6.1 lists "`UsdGenScale` (M4)" *(stale claim)* | `11-…` §2.5, §6.1 | Delete the ASSUMPTION note | minor |
| CV-103 | `08-tools.md` | §11 items 1 and 5 | Item 1 lists five differences between `06` §3.8 and `08` §1.4's C ABI; the two listings are **name-for-name identical** today. Item 5 asks `01` §6 to rename `P1–P8` to `I1–I8`; `01` §6 is headed "Engine invariants I1-I8" *(both stale claims)* | `06-…` §3.8; `01-…` §6 | Delete items 1 and 5 | minor |
| CV-104 | `06-imaging.md` | §3.8 preamble | "Where the two ever differ, §1.4 governs and this section is edited to match **before the C4 freeze review**" — the two do not differ; the note reads as an outstanding action | `08-…` §1.4 | Restate as "reproduced verbatim; verified identical" | minor |
| CV-105 | `12-risks-decisions-open-questions.md` | §2 RK-29 | "`02-schema.md` §7.3 and `10-build-dependencies-testing.md` §4.3 still route the `LibraryPath` at `usdGenImaging` and must follow R19". Both route it at `usdGenSchema` *(stale claim)* | `02-…` §7.1; `10-…` §4.3 | Delete the sentence | minor |
| CV-106 | `12-risks-decisions-open-questions.md` | §4 A-19 | "`11-roadmap.md` §5.2 SC-4 still carries the superseded unsalted formula and must follow R13". SC-4 carries `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32` *(stale claim)* | `11-…` §5.2 | Delete | minor |
| CV-107 | `11-roadmap.md` | §2.8 | "Appendix A §2.9 and `appendix-B-prototype-inventory.md` still record the file as **not carried in**; both are corrected". Appendix A §2.9 says "carried in" with a build line and appendix B §0.1 lists `motion-blur` as directory 12 *(stale claim)* | `appendix-A-…` §2.9; `appendix-B-…` §0.1 | Delete | minor |

### 2.12 Python error-containment API

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-108 | `08-tools.md` | §7.4 item 2 | "Containment: `try/except Tf.ErrorException` plus a post-condition assert in Python (**`Tf.ErrorMark` is not bound in Python**)". `12` RK-08 shows the class **is** bound as `Tf.Error.Mark` (`pxr/base/tf/wrapError.cpp:200, 212-221`), that `Tf.ErrorMark` is the name that does not exist, and prescribes `m = Tf.Error.Mark(); m.SetMark(); …; m.GetErrors(); m.Clear()` | `12-…` §2 RK-08 (which corrects S41/S46 and the freeze report by name) | Adopt `Tf.Error.Mark` | major |
| CV-109 | `05-static-curves-and-deformation.md` | §5.6 landmine 2 | Same claim: "`Tf.ErrorMark` is not bound in Python", with `try/except Tf.ErrorException` as the containment | `12-…` §2 RK-08 | Adopt `Tf.Error.Mark` | major |

### 2.13 Numbers whose *labels* are vocabulary (frame ledger, tile count, RSS)

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-110 | `01-architecture.md` | §0.3 frame-ledger table | Prints "Dirty routing … ~0.05 ms", "SoA → AoS interleave 0.25–0.5 ms" and "usdGen total, deform frame **0.8–1.2 ms**" while declaring 09 §0.2 normative. `09` §0.2 (the **only** ledger, R41) prints 0.04 ms, 0.25 ms and **0.8–1.0 ms**; `appendix-A` §2.11 names 01 and 05 as owing the correction | `09-…` §0.2 (R41) | Restate the three rows from 09 | major |
| CV-111 | `05-static-curves-and-deformation.md` | §8.1 "usdGen total on a deform frame" | Prints ≈ 0.8–1.0 ms (correct) but §8.2 then reads it against "a ~12.2 ms Storm draw" and computes "roughly 6 %", while `01` §0.3 computes "roughly 7 %" from 0.8–1.2 ms. Also flagged by appendix A §2.11 | `09-…` §0.2 | Derive the share from 09's rows only | minor |
| CV-112 | `03-execution-engine.md` | §6.3 step 2 | "≈ 0.05 ms for **64 tiles**" at the 100 k × 8 CV fixture. ADR §9 **R21**, `09` §0.2, `05` §4.6 and `02` §3.2 all give **49 tiles** at 100 k; 64 is the 1 M figure. `05` §4.6 names this exact line as the last stale figure | ADR §9 **R21** | Change to 49 tiles | major |
| CV-113 | `06-imaging.md` | §8 channel 1 | "0.05 ms for **64 tiles**" at the same fixture, and §9's HUD line correctly prints `49/49 tiles` two pages later | ADR §9 **R21** | Change to 49 tiles | major |
| CV-114 | `11-roadmap.md` | §2.4 M3 exit gates | Reads the memory measurement as "**668 MB with per-node caching** (293 MB for buffers alone)". `appendix-A` `EV-007`, `03` §0.3, `09` §2.6 and `12` RK-13 all read 668 MB = **per-node buffers** and 293 MB = **one shared buffer**; the parenthesis inverts the meaning | `appendix-A-…` `EV-007` | Correct the parenthesis | major |

### 2.14 Space/tail partition wording

| # | File | §/loc | Problem | Canonical | Fix | Sev |
|---|---|---|---|---|---|---|
| CV-115 | `03-execution-engine.md` | §1.6 opening sentence | "`Space()` partitions the … node list into a **rest head** `H` and a **deformed tail** `T` **at the last `UsdGenSpace::Rest` node**". `04` §0.7 and `05` §4.4 define the tail as "the **first** node whose resolved space is `deformed`, and every node downstream of it" (the downstream-closure form). The two definitions differ whenever spaces are non-monotone, which §1.6's own `UsdGenNoise`-after-`UsdGenDeform` example makes possible | `04-…` §0.7 / `05-…` §4.4 (downstream closure), which 03 §1.6's own later text implies | State the downstream-closure form once, in 03, and have 04/05 cite it | major |

---

## 3. Files to change, by document

| Document | Findings |
|---|---|
| `00-request-and-scope.md` | CV-13, CV-84, CV-94, CV-95, CV-96 |
| `01-architecture.md` | CV-73, CV-74, CV-75, CV-76, CV-77, CV-110 |
| `02-schema.md` | CV-06, CV-45, CV-51, CV-52, CV-53, CV-54, CV-69, CV-70, CV-78 |
| `03-execution-engine.md` | CV-02, CV-23, CV-34, CV-39, CV-59, CV-60, CV-67, CV-112, CV-115 |
| `04-operators.md` | CV-35, CV-46, CV-47, CV-57, CV-58, CV-71, CV-102 |
| `05-static-curves-and-deformation.md` | CV-04, CV-22, CV-27, CV-40, CV-41, CV-42, CV-55, CV-56, CV-61, CV-62, CV-63, CV-64, CV-65, CV-66, CV-72, CV-109, CV-111 |
| `06-imaging.md` | CV-16, CV-21, CV-30, CV-31, CV-33, CV-37, CV-38, CV-79, CV-80, CV-81, CV-104, CV-113 |
| `07-look-maps-expressions.md` | CV-05, CV-43, CV-44, CV-49, CV-50, CV-90, CV-97, CV-98, CV-99 |
| `08-tools.md` | CV-03, CV-24, CV-48, CV-86, CV-92, CV-103, CV-108 |
| `09-performance-and-benchmarks.md` | CV-01, CV-20, CV-25, CV-36, CV-93 |
| `10-build-dependencies-testing.md` | CV-07, CV-14, CV-17, CV-18, CV-32, CV-82, CV-83, CV-85, CV-87, CV-89, CV-90 |
| `11-roadmap.md` | CV-08, CV-15, CV-19, CV-26, CV-29, CV-88, CV-100, CV-101, CV-107, CV-114 |
| `12-risks-decisions-open-questions.md` | CV-09, CV-10, CV-12, CV-28, CV-105, CV-106 |
| `appendix-A-evidence-ledger.md` | CV-11 |
| `appendix-B-prototype-inventory.md` | CV-91 |

---

## 4. What is already consistent (checked, no finding)

Recorded so a reconciler does not re-open them.

* **Prim types and API schemas.** Every `UsdGen*` prim type name in every document matches ADR §2.1
  and `02` §1. No document invents a type; `UsdGenPrimitive` and `UsdGenScatterX` appear only as
  recorded rejections.
* **The Hydra-only prim set** (`tile_NNNN`, `guides/<setName>`, `guides/<setName>/cvs`,
  `inst_<opName>`, `inst_<opName>/Prototypes/<n>`, `material_storm`) is identical in 01, 02, 05, 06
  and 11.
* **The C ABI (contract C4).** `06` §3.8 and `08` §1.4 declare exactly the same nineteen
  `UsdGenImaging_*` entry points with the same signatures.
* **The dirty-bit enum.** `UsdGenDirtyNone/Parameter/Capture/Topology/SurfacePoints/SurfaceXform/
  SurfaceTopo/Map/Structural/LiveOverride` is unscoped everywhere; no document writes
  `UsdGenDirtyBits::Value` (R3).
* **The env-var registry.** Every `USDGEN_*` runtime variable used in any document is in `10` §3.5,
  including `USDGEN_OP_CHECKS`, `USDGEN_IMAGING_DLL`, `USDGEN_XVFB_ROOT` and `USDGEN_DISPLAY`.
* **The five adapter registrations plus `UsdGenRestAPIAdapter`** and the four scene-index
  registrations are spelled identically in 02 §7.3, 06 §1.1/§2.1 and 10 §4.4 (R3).
* **The hash family.** `UsdGenHash64` / `UsdGenHash32` / `UsdGenHash01`, the SplitMix64 constants,
  `hairId = UsdGenHash32(curveId, 0) / 2^32` and the `kSalt*` naming are identical in 02 §2.19.1,
  03 §1.7, 04 §0.6, 05 §2.4, 06 §6.4 and 09 §3.1 (R12/R13) — the only defects are the two signature
  slips of CV-44 and CV-45.
* **`usdGen:` property namespacing (R6).** No document declares a schema property outside the
  `usdGen:` namespace, and `primvars:usdGen:*` appears only in the two sanctioned places (contract
  C3 and the paint-brush surface primvars).
* **Rejected ideas.** No document restates implicit sibling wiring, `usdGen:active`, structural
  `enabled` on topology-preserving operators, a `sceneGlobals` interactive flag, widened chunks,
  array padding, `RemovePrim` in an interactive path, `.usda` bakes, baked motion samples or the
  `GetPrim` cook backstop except as recorded rejections (12 §1 X-01…X-28).
* **Milestone ids** are `M0`–`M8` everywhere; no document writes an "S" or "P" phase.
* **Contract ids** `C1`–`C5` and their freeze milestones (M1, M1, M2, M5, M1) are identical in 00
  §5.3, 01 §5.3, 02 §8.1, 11 §0.2 and 12.
* **Tier vocabulary.** `T0`–`T4` unhyphenated versus `T-1`…`T-5` hyphenated is respected in every
  document, and each of 00, 05, 08 and 10 states the disambiguation explicitly.

---

## 5. Method

Every document was read end to end. The union of names was then rebuilt mechanically over
`plan/*.md` for: `UsdGen[A-Z]\w*` (types, classes, tests, targets), `usdGen:[a-zA-Z:]*` (properties),
`USDGEN_[A-Z_]*` (env vars and debug codes), `UsdGenImaging_\w*` (the C ABI), `EV-\d{3}` and
`A2\.\d+-\w+` (ledger handles), and `\b(B|E|SI|S|L|T|R)-\d+r?\b` plus `T-INST-\d`, `T-EXPR-1`,
`T-PTEX-1` (gate ids). Each divergence was then read back in context in both documents before it was
written down. No plan document was modified.
