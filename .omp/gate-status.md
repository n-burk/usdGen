# M1 gate-status census — usdGen

Census of every gate registered in `plan/09-performance-and-benchmarks.md` §5 (the single
gate registry, ADR R40). Status cells are from verified M1 evidence dated 2026-09-09.
Status vocabulary: **PASS** / **FAIL** (value vs bound) / **MEASURED — decision** /
**PENDING-RUN** / **PENDING-FIX** / **UNMEASURED**. Pass-criterion and Test cells are
copied verbatim from §5.1–§5.4; the registry's "Status today" column is replaced by the
census Status below. Read-only census — no builds, no tests were run for this file.

## 5.1 Build and engine gates

| Gate | Tier | Metric | Pass criterion | Test | M | Status (M1, 2026-09-09) |
|---|:--:|---|---|---|:--:|---|
| **B-1** | T0 | `DT_NEEDED` of `libusdGen.so` against `USDGEN_FORBIDDEN_LIB_REGEX`, and the sources against `USDGEN_FORBIDDEN_INC_REGEX` (both CMake cache variables, `10-build-dependencies-testing.md` §1.4) | zero forbidden entries (`usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`, `usdAppUtils`) | `testUsdGenLinkRule_{usdGen,usdGenMath,usdGenTestUtils}` + `testUsdGenIncludeRule` | **M0** | UNMEASURED |
| **E-1** | T0 | 5-op chain, G3, full run | ≤ **1.5 ms** at 8 threads in the private arena; the 20-thread number is quoted in parentheses only (ADR R27) | `benchUsdGenChain --curves 100000 --stylers 5 --chunk 512` | M1 | **FAIL — 1.687–1.94 ms vs ≤ 1.5 ms** at 8 threads (NoiseE1 optimizing noise.cpp now) |
| **E-1r** | T0 | the same chain on a ragged buffer (`cvCount == 0` + `cvOffsets`) | ≤ 2× the uniform path | `benchUsdGenChain --ragged` | M2 | UNMEASURED |
| **E-2** | T0 | 1 % of chunks dirty on all nodes | ≤ 0.10 ms | `benchUsdGenSparse` | M1 | **PASS** |
| **E-3** | T0 | terminal-parameter edit only | ≤ 0.6 ms | `benchUsdGenSparse --terminal` | **M2** | UNMEASURED |
| **E-4** | T0 | nanoflann kNN capture, 100 k / 1 M rest roots, 4 k guides, 8 threads | ≤ 25 ms at 100 k **and linear in roots** (ASSUMPTION). Stop: > 300 ms at 1 M ⇒ SC-6 | `benchUsdGenKnn` | M0 pre-work (**PW-1**), binding **M3** | UNMEASURED |
| **E-5** | T0 | RSS at 1 M × 8 CV, 5 nodes, default caching | ≤ 800 MB | `benchUsdGenMemory` | **M3**, re-run M7 | UNMEASURED |
| **E-6** | T0 | append one node to a 200-node groom | ≤ 0.2 ms **and exactly 1 node rebuilt** | `testUsdGenGraph` | M1 | **PASS** |
| **E-7** | T0 | thread scaling 1 → 8 on E-1 | ≥ 3× | `benchUsdGenChain --threads` | M1 | **PASS** |
| **E-8** | T0 | determinism at 1/2/4/8/20 threads, `-ffp-contract=off` | bitwise identical | `testUsdGenKernelDeterminism` on G1–G4 and `head1M` | **M1**, re-run M4 | **PASS (bitwise)** |

## 5.2 Scene-index gates

| Gate | Tier | Metric | Pass criterion | Test | M | Status (M1, 2026-09-09) |
|---|:--:|---|---|---|:--:|---|
| **SI-1** | T1 | array exactness on every published tile | `points.size() == Σ curveVertexCounts`, hard assert, release builds too | `testUsdGenTileContract` | M1 | **PASS** (TileContract 57 checks ok incl extent + value-identical) |
| **SI-2** | T1 | invalidation set for a clump-amount edit | exactly `primvars/points/primvarValue` + `extent/*` on the dirty tiles, nothing else | `testUsdGenInvalidation` | M1, re-run M2 for overlaid prims | **PASS** |
| **SI-3** | T1 | cook count over 10 interactive events | exactly 10 commits; every commit and notice on the app or notice thread (thread-id assert); **zero cooks inside `GetPrim`**; the same script under `MergingSceneIndexNoticeBatchBegin/End` publishes identical points with at most one extra commit per batched frame (`06-imaging.md` §3.9) | `testUsdGenScheduling` | M1 | **PASS** |
| **SI-4** | T1 | 8 readers × 100 publishes | 0 torn reads | `testUsdGenSnapshotRace` | M1 | **PASS** |
| **SI-5** | T1 | chain order | usdGen strictly after UsdSkel/RigExec/flattening, before every Storm/hdPrman plugin | `testUsdGenChainOrder` | **M1** | **PASS** (ChainOrder) |
| **SI-6** | T1 | initial population, both paths | identical prim set from `PrimsAdded` replay and from the bounded first-`GetChildPrimPaths` traversal; ≤ 5 ms on the 11 005-prim stage (ASSUMPTION, ADR R28) | `testUsdGenPopulation` | M1 | **PASS** (Population) |
| **SI-7** | T1 | adapter coverage | every `usdGen:*` property of all five registered types **and** `usdGen/rest/points` from the `UsdGenRestAPI` adapter appears **and dirties** | `testUsdGenAdapter`, `testUsdGenRestAdapter` | M1 | **PASS** (Adapter + RestAdapter) |
| **SI-8** | T1 | auto-applied `UsdGenMaskAPI` on a codeless type | the API's properties appear on every `UsdGenOperator` subtype | `testUsdGenAutoApply` | M0 pre-work (**PW-5**), binding M1 | **PASS** |
| **SI-9** | T1 | cost of the private `HdSiExtComputationPrimvarPruningSceneIndex` wrapper on a **production-density skinned scalp** (the probe mesh had 10 points) | the per-deforming-frame wrapper cost is recorded and stays inside class A's ≤ 1.0 ms usdGen share (§0.1; ASSUMPTION until the gate runs); red ⇒ stop condition SC-13 (`05-static-curves-and-deformation.md` §4.2) | `testUsdGenPruningCost` | **M2** | UNMEASURED |
| **SI-10** | T1 | two scene-index instances attached to one session | identical generation, prim set and frame for one commit | `testUsdGenSessions` | **M2** | UNMEASURED |
| **SI-11** | T1 | does `reorder nameChildren` reach the scene index as an invalidation? | recorded; **nothing in the design depends on the answer** (ADR §2.1) | `testUsdGenReorderNotice` | M0 pre-work (**PW-6**, `11-…` §1.1), record only | UNMEASURED |

## 5.3 Storm gates (EGL harness)

| Gate | Tier | Metric | Pass criterion | Test | M | Status (M1, 2026-09-09) |
|---|:--:|---|---|---|:--:|---|
| **S-1** | T2 | G3 static draw, refineLevel 2, **720p and 1080p** | ≤ **12.8 ms** at 720p (= 16.6 − 1.35 rig − 1.0 usdGen − 1.44 upload, §0.2). `design/proposal-performance.md` §11.2's 14 ms is kept only as a regression ceiling; 1080p is a separate baseline | `benchUsdGenStorm --static` | M1 | **PENDING-RUN** (`benchUsdGenStorm` restored, not yet executed) |
| **S-2** | T2 | deform frame: scene-index `points` publish, 49 tiles | ≤ 2.0 ms delta over static | `benchUsdGenStorm --deform` | M2 | UNMEASURED |
| **S-3** | T2 | one-tile edit (1 of 49 dirty) | ≤ 1.2× static | `benchUsdGenStorm --onetile` | M2 | UNMEASURED |
| **S-4** | T2 | culling: camera frames ¼ of G4 | `itemsDrawn` drops ≥ 3× at 32 and 196 tiles, not at all at 1 prim | `benchUsdGenStorm --cull` | **M2** | UNMEASURED |
| **S-5** | T2 | batching: G3's 49 tiles one material; G2's two materials as the negative case | `drawBatches == 1, drawCalls == 1` on G3; `drawBatches == 2` on G2 | `benchUsdGenStorm --batches` | M1 | **PENDING-RUN** (`benchUsdGenStorm` restored, not yet executed) |
| **S-6** | T2 | no relocation across deform frames | `vboRelocated == 0` | `benchUsdGenStorm --deform` (counters) | **M1** | **PENDING-RUN** (`benchUsdGenStorm` restored, not yet executed) |
| **S-7** | T2 | density scrub: parked-CV drag vs a real count change, with and without the per-CV skip (§3.2) | parked frame ≤ 1.5× a points-only frame | `benchUsdGenStorm --scrub` | M5 | UNMEASURED |
| **S-8** | T2 | `hairTangent` variant A (`inData.Neye`) vs B (vertex primvar) on a deforming G3 | A wins on frame time **and** compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, else B becomes default | `testUsdGenStormTangent` + `testUsdGenStormHgiResource` | M0 pre-work (**PW-2**), **decides M1** | **PASS — decision variant A** (Tangent A/B 1.073, pixels bit-identical; HgiResource green) |
| **S-9** | T2 | refineLevel 1 vs 2 **and the switch cost** | switch < 3 ms ⇒ the tumble tier ships as an option | `testUsdGenStormRefine` on G4 | M0 pre-work (**PW-3**), **decides M1** | **MEASURED — decision**: switch medians 16.6/16.3 ms ≥ 3 ms ⇒ NO tumble tier (SC-4 `plan/11-roadmap.md`:659; fallback `08-tools.md`:502-506 decimation-only). `testUsdGenStormRefine` Passed with decision line |
| **S-10** | T2 | in-place overlay on a frozen prim (v2, ADR R29) | `PrimsRemoved`/`PrimsAdded` versus an in-place `points` dirty | `testUsdGenStormInPlace` | **M8** | UNMEASURED |
| **S-11** | T2 | G3's `head1M` variant, static draw, refineLevel 2, 720p | record only, no threshold — establishes whether §2.5's linear fit holds past 200 k | `benchUsdGenStorm --scene bench/head1M.usda --static` | M7 | UNMEASURED |
| **S-12** | T2 | prim-count sweep at 100 k × 8 CV: 1 / 32 / 128 / 196 tiles | ≤ 1.15× the one-prim frame time at `tileTarget = 64`, `drawBatches == 1` | `benchUsdGenStorm --tilesweep` | **M1**, before C2 freezes | **PENDING-RUN** (`benchUsdGenStorm` restored, not yet executed) |

## 5.4 Look, tools, instancing, render time

| Gate | Tier | Metric | Pass criterion | Test | M | Status (M1, 2026-09-09) |
|---|:--:|---|---|---|:--:|---|
| **L-1** | T2 | Storm render-context resolution | `outputs:glslfx:surface` resolves before plain `outputs:surface`; if not, flip `USDGEN_STORM_MATERIAL_OVERRIDE` to default ON | `testUsdGenStormMaterial` | M0 pre-work (**PW-4**), decides M1 | **PASS — decision** (mtlx+glslfx bit-identical vs glslfx-only) |
| **L-2** | T2 | all three shipped `.glslfx` files: Sdr parse, compile, golden image, refineLevel precedence | exactly the 20 `inputs:` of `07-…` §2.2 and each variant's primvars metadata; 0 warnings, 0 GL errors; golden match against `prototypes/storm-hair-look/hair_pv_tangent.png`; the per-prim `displayStyle/refineLevel` wins over app complexity. **Guards C5** | `testUsdGenStormLook` | M1 | **PASS** (StormLook) |
| **L-3** | T0 | map and expression determinism | bitwise-identical baked primvars at 1 and 8 threads; `Expression::isThreadSafe()` true for every shipped function; `rand()` reproducible | `testUsdGenExpr --threads`, `testUsdGenLookBake` | M4 | UNMEASURED |
| **L-4** | T0/T1 | map capture cost at 1 M roots, 8 threads | ≤ **200 ms** for one image map + one Ptex map + one expression (ASSUMPTION), and `PtexCache::Stats.fileReopens == 0` at the default cache size | `benchUsdGenMaps` | M4 | UNMEASURED |
| **L-5** | T1 | `UsdGenImaging_ReloadMaps()` invalidation | re-captures exactly the nodes whose maps changed and dirties exactly the baked primvar's `primvarValue` on exactly the affected tiles; `displayColor` never co-dirtied with `points` | `testUsdGenMapReload` | M4 | UNMEASURED |
| **T-1** | T3 | brush move at 100 k, press/move/release | ≤ 1 ms Python + engine per move | `testUsdviewUsdGenComb.py` | M5 | UNMEASURED |
| **T-2** | T1 | `UsdGenImaging_PickCV` at 100 k / 1 M CVs | ≤ 0.25 / 2.5 ms | `testUsdGenPick` | M5 | UNMEASURED |
| **T-3** | T3 | CPU pick vs `view.pick()` at 100 pixels | disagreements only where a CV is occluded | `testUsdviewUsdGenPick.py` | M5 | UNMEASURED |
| **T-4** | T3 | one freeze on a 2 205-prim stage | ≤ 5 ms **author** (`_resetGUI` excluded, ADR R40) **and exactly one** `_resetGUI` | `testUsdviewUsdGenFreeze.py` | M5 | UNMEASURED |
| **T-5** | T3 | plugin sets `_allowAsync` without the CLI flag | 1 `asyncAllow`, ~10 `asyncPoll`/s | `testUsdviewUsdGenAsync.py` | **M8** | UNMEASURED |
| **T-EXPR-1** | T0 | SeExpr sandbox | `SE_EXPR_PLUGINS` cleared before `ExprFunc::init()`; closed function set; a parse error yields a fallback value, never an exception into Hydra | `testUsdGenExpr --sandbox` | M4 | UNMEASURED |
| **T-PTEX-1** | T0 | Ptex face-id agreement | ids match `Far::PtexIndices` on quads, n-gons and all-triangle meshes; a `GeomSubset` does not shift them | `testUsdGenPtex` | M4 | UNMEASURED |
| **T-INST-1** | **T1** | instancer pick round-trip | a click on an instance resolves to the `UsdGenDescription` through `primOrigin` | `testUsdGenInstancerPick` | M6 | UNMEASURED |
| **T-INST-2** | T1 | prototype rebasing | `usdGen:surface` inside a propagated native prototype rebases correctly; `instancedBy` has exactly one target | `testUsdGenInstancer` | M6 | UNMEASURED |
| **R-1** | T4 | `usdrecord`/hdPrman, 3 motion samples on G3 | correct blur **and** `k·tail` evaluations, not `k·(head+tail)` | workstation protocol **§7** (§4.3) | release (built M7) | UNMEASURED |
| **R-2** | T4 | MSAA/A2C vs OIT on 1-px strands at 1080p and 4K | visual sign-off, recorded which reads better | workstation protocol **§3** (§4.3) | release | UNMEASURED |
| **R-3** | T4 | `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` and AMD/Intel glslfx compile | 0 errors | workstation protocol **§5**, with §4 for the AMD/Intel half (§4.3) | release | UNMEASURED |

## Tally

50 gates in total: 10 (§5.1) + 11 (§5.2) + 12 (§5.3) + 17 (§5.4).

| Status | Count | Gates |
|---|---:|---|
| PASS | 15 | E-2, E-6, E-7, E-8, SI-1…SI-8, S-8 (decision A), L-1 (decision), L-2 |
| FAIL (value vs bound) | 1 | E-1 (1.687–1.94 ms vs ≤ 1.5 ms) |
| MEASURED — decision | 1 | S-9 (no tumble tier) |
| PENDING-RUN | 4 | S-1, S-5, S-6, S-12 (`benchUsdGenStorm` restored, not yet executed) |
| PENDING-FIX | 0 | — (the surgery test below is not a registry gate) |
| UNMEASURED | 29 | B-1, E-1r, E-3, E-4, E-5, SI-9…SI-11, S-2, S-3, S-4, S-7, S-10, S-11, L-3, L-4, L-5, T-1…T-5, T-EXPR-1, T-PTEX-1, T-INST-1, T-INST-2, R-1, R-2, R-3 |

### M1 exit view (§5.5: E-1, E-2, E-6, E-7, E-8; SI-1…SI-8; S-1, S-5, S-6, S-12; L-2; S-8/S-9/L-1 decisions)

21 gates: 15 PASS, 1 MEASURED — decision (S-9), 1 FAIL (E-1), 4 PENDING-RUN (S-1, S-5, S-6, S-12).
Open for M1 exit: **E-1** (FAIL) and the four `benchUsdGenStorm` runs.

## Non-registry item

`testUsdGenStormSurgery` — **PENDING-FIX** (stage-resolution wiring, imaging lane). Matches no registry Test cell (see contradictions below), so it carries no gate id and is excluded from the 50-gate tally.

## Plan contradictions

1. `testUsdGenStormSurgery` matches no registry Test cell — its CMake label is plain T2 now.
2. S-9's < 3 ms is a decision threshold, not a pass bound (09:592 wording: "switch < 3 ms ⇒ the tumble tier ships as an option").
