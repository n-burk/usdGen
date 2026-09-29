# usdGen — Roadmap

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document turns the architecture into a schedule: nine milestones **M0–M8** (ADR §7), the pre-work
**PW-1…PW-13** (§1 owns that numbering), the gate subset each milestone exits on, the dependency graph
and the execution model across three engineers, the estimates with their contingency, the stop
conditions (§5 owns that register) and the definition of done. It owns *when* and *in what order*, and
no design decision of its own; every
operator name, property name, prim type, gate id and milestone id is the one fixed in ADR §2, §7 and
§9, and the sibling that specifies a thing is named beside it.

Reads with: `01-architecture.md`, `02-schema.md` (the normative property registry C1 freezes,
ADR §9 R7), `03-execution-engine.md`, `04-operators.md`, `05-static-curves-and-deformation.md`,
`06-imaging.md` (tile contract C2, invalidation), `07-look-maps-expressions.md` (gates L-2…L-5),
`08-tools.md` §1.4 (the C ABI, C4), `09-performance-and-benchmarks.md` §5 (**the gate registry**),
`10-build-dependencies-testing.md` (targets, tiers T0–T4, test binaries),
`12-risks-decisions-open-questions.md` (risks, Q-01…Q-16), `appendix-A-evidence-ledger.md`,
`appendix-B-prototype-inventory.md`.

---

## 0. Principles

### 0.1 Vertical slices, never horizontal layers

A milestone is done when an artist can open usdview on a stage, see the feature working, and a CI job
proves it headlessly (`design/proposal-risk.md` §1). M1 ships five operators, a graph, an adapter, a
publisher and a shader together because usdGen's structural risks live in the seams between them, and a
seam is only proved by traffic crossing it. The failure this avoids: building the evaluator against
synthetic buffers and learning at integration that `usdGen:*` edits produce no Hydra notice at all
(S10; MEASURED, `research/G-stage-free-parameter-and-time-transport.md` §1).

### 0.2 Contracts frozen early (C1–C5)

| # | Contract | Frozen at | Specified in |
|---|---|---|---|
| C1 | `usdGen:` property names and prim type names | end of M1 | `02-schema.md` |
| C2 | the published tile `basisCurves` contract | end of M1 | `06-imaging.md` |
| C3 | the curve contract for guides, freezes, imports, sim caches (S42 + `primvars:usdGen:role`) | end of M2 | `05-static-curves-and-deformation.md` |
| C4 | the `extern "C"` control ABI + pxr_boost array surface | end of M5 | `08-tools.md` §1.4 |
| C5 | glslfx parameter names (the shipped `inputs:` block, verbatim) | end of M1 | `07-look-maps-expressions.md` |

Everything else — `UsdGenOp`, chunk views, graph headers, imaging internals — is **not installed** and
may churn until M7 (ADR §3), which is what lets M3–M6 rewrite the engine's guts without touching an
artist's stage. A freeze is enforced by a test: `testUsdGenContracts` (T1) holds the C1, C2, C3 and C5
name lists as literal data; **`testUsdGenAbi`** (T1) holds C4 — `nm -D --defined-only
libusdGenImaging.so` filtered to `UsdGenImaging_*`, compared against the nineteen entry points of
`08-tools.md` §1.4. Both fail on any rename (§7). Test binary names are
`10-build-dependencies-testing.md` §5.6's, which is the CTest name registry and carries both:
`testUsdGenAbi` is **T1, not T0**, because it loads `libusdGenImaging.so` and therefore needs the
plugin registry (ibid.).

### 0.3 The gate registry is `09-…` §5; this document cites it

**`09-performance-and-benchmarks.md` §5 is the single gate registry** (id → metric, pass criterion,
tier, milestone, status); `00`, `10`, `11`, `12` and appendix A cite it and must agree (ADR §9 R40).
Where 09 and R40 disagree, R40 wins; 09 was re-checked against R40 on 2026-09-05 and agrees on every
row. §2.0 transcribes the list. Each milestone exits on a named subset of that registry plus its own vertical demo;
a red exit gate blocks the milestone. **No T4 gate may be a milestone exit** — R-1, R-2, R-3 and the
end-to-end fps number are release criteria (§6.4; ADR §9 R39; `design/judge-delivery.md` §7 clause (a)).
The one place `design/proposal-risk.md` §9.2 broke that rule, an "≥ 30 fps comb stroke on the
workstation" used as a tools-slice exit, is corrected in M5: the automatable half stays a gate, the fps
number becomes RC-4.

Tiers are `10-build-dependencies-testing.md` §5.1 (ADR §9 R2): **T0** engine (no Hydra, no USD) and
build checks, **T1** headless scene index over the real `UsdImagingCreateSceneIndices` chain, **T2**
Storm on the EGL harness, **T3** `testusdview` and `usdrecord --renderer GL` under Xvfb (CPU numbers
only), **T4** workstation. The E-* gates, which `design/proposal-performance.md` §11.2 labels "tier 1",
are **T0** here: each is an engine call over a pure-value `UsdGenGraphDesc`, and the engine links no
`usd`, no `usdImaging`, no `hd` (ADR §4.2.3, §9 R37). **T-2 stays T1** (ADR §9 R40; `09-…` §5.4;
`10-…` §6.5).

### 0.4 Stop conditions are written before the work

Each milestone with an unmeasured dependency carries a named stop condition: failing gate, fallback,
schedule consequence (§5). The hard one is M1's — if precise `usdGen:*` invalidation cannot be
demonstrated, fall back to `primvars:usdGen:*` and **re-plan**, because every later milestone
multiplies imprecise dirtying (ADR §7; `design/proposal-risk.md` §9.2).

### 0.5 Number and citation discipline

Every number is tagged MEASURED (with its `appendix-A-evidence-ledger.md` row id and report section),
**DERIVED from** a ledger row (scaled or interpolated — never MEASURED), UNMEASURED (with the gate that
will measure it), or ASSUMPTION (ADR §9 R42). Ledger rows are cited as **`EV-nnn`**, the only citation
handle for a measured number (ADR §9 R1); `appendix-A-evidence-ledger.md` §2.0 maps the retired
`A2.n-Xn` spellings onto them one-for-one. All calendar and effort numbers in §4 are ASSUMPTION (Q-16).
`R1`–`R9` unqualified always mean `design/brief-v1.md` §1 requirements; ADR §9 rulings are always
written `ADR §9 Rnn`; a proposal's own risk-register item is bare (`R9`) and a render gate is
hyphenated (`R-1`), per `10-build-dependencies-testing.md` §8.4.

---

## 1. Pre-work — PW-1…PW-13

**This section owns the `PW-n` numbering** (ADR §9 R1 names them `PW-n`, never `P-n`;
`10-build-dependencies-testing.md` §6.5 and `appendix-B-prototype-inventory.md` §13 cite it). Thirteen
items in two kinds. **PW-1…PW-6 are the six M0 decision checks** (§1.1): each an afternoon on this
host, each feeding a decision otherwise made blind, all six inside M0's two weeks, with results and
decision lines in `appendix-A-evidence-ledger.md` before M1 is planned in detail. E-4, S-8, S-9, L-1
and SI-8 are M0 pre-work whose gates become *binding* later (ADR §9 R40). **PW-7…PW-13 are harness
pre-work** (§1.2): the harness may be written in M0, but the gate it feeds binds at a later milestone,
so only PW-1…PW-6 are part of M0's exit (§2.1).

### 1.1 PW-1…PW-6 — the M0 decision checks

| # | Gate | Decides | Method and threshold |
|---|---|---|---|
| **PW-1** | E-4 | the M3 capture schedule for `UsdGenGuideInterpolate` and `UsdGenClump` | vendored nanoflann 1.12.1 (S38), `KDTreeSingleIndexAdaptor<L2_Simple_Adaptor<float, Cloud>, Cloud, 3>` — exact template arity **UNVERIFIED**: nanoflann is not on this host and `research/A8-seexpr-ptex-libs.md` §5 quotes the upstream README, so PW-1's first step is to vendor the header and confirm the instantiation compiles. k = 3 over 4 000 guide roots for 100 k and 1 M rest roots at 1/4/8/20 threads: ≤ 25 ms at 100 k on 8 threads and linear in roots; > 300 ms at 1 M triggers SC-6. Today's only figure, `design/proposal-performance.md` §5.6's "~10 ms", is an ASSUMPTION (`design/judge-delivery.md` §5 item 6) |
| **PW-2** | S-8 | the default glslfx variant (ADR §5.4), hence whether `hairTangent` is in C2 — settled before M1 freezes C2 | EGL harness (`prototypes/storm-hair-look/{eglctx.h,bench_hair.cpp}`), 100 k × 8 CV deforming, 60 frames: variant A (`inData.Neye`) vs B (`UsdGenHairPreviewPrimvar`, ADR §9 R4); A must also compile under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`. The mechanism is not in doubt: the points fastpath needs `DirtyPoints` set and `DirtyNormals\|DirtyWidths\|DirtyPrimvar` clear (`pxr/imaging/hdSt/basisCurves.cpp:930-935`, verified) |
| **PW-3** | S-9 | whether a refineLevel-1 tumble tier ships as an M5 LOD option (ADR §5.5, S31) | same harness, 200 k × 8 CV, `displayStyle/refineLevel` 2 → 1 → 2 every ten frames: ships under 3 ms of switch cost, else LOD is decimation only. Steady state MEASURED 8.17 vs 23.93 ms (row `EV-021`; `research/G-storm-hair-look-prototype.md` §5) |
| **PW-4** | L-1 | the `USDGEN_STORM_MATERIAL_OVERRIDE` default (ADR §5.6, OFF) and whether `material_storm` is ever synthesized | one `Material` carrying both `outputs:surface` and `outputs:glslfx:surface` through the harness, diffed against two single-terminal renders. Source says glslfx wins: `materialRenderContexts = {glslfx, mtlx}` (`pxr/imaging/hdSt/renderDelegate.cpp:695-707`, `_RenderDelegateInfo()`; verified, ADR §9 R43) |
| **PW-5** | SI-8 | whether the tool must keep applying `UsdGenMaskAPI` explicitly (ADR §2.1 requires it until SI-8 passes) | throwaway codeless schema with `UsdGenMaskAPI` under `AutoApplyAPISchemas`, read from plain Python through `FindConcretePrimDefinition("UsdGenNoise").GetPropertyNames()`. Derived-type propagation is implemented (`pxr/usd/usd/schemaRegistry.cpp:926-940`, the "Collect all the types to apply the API schema to" comment through the `GetAllDerivedTypes` call at `:937`, verified); the codeless case is not |
| **PW-6** | SI-11 (record only) | nothing in the design — it closes Q-06 | author `reorder nameChildren` on `<Description>/Ops` with the adapter attached and record any notice at a terminal observer; justifies in writing the rule that no evaluated result depends on namespace order (S26) |

The pre-work re-measures nothing the prototypes settled: the chain slot (S1), the engine numbers
(S21–S24), the Storm curve contract and throughput (S27–S31), the glslfx look (S35), the instancer
contract (S33), transport and picking (S39–S40), freeze costs (S41–S42), the build story (S44–S45).

### 1.2 PW-7…PW-13 — harness pre-work

Seven gates have no harness in any carried-in prototype (`appendix-B-prototype-inventory.md` §13,
§14). Writing the harness is M0-shaped work; the *gate* binds where `09-performance-and-benchmarks.md`
§5 puts it, so none of these is an M0 exit criterion.

| # | Gate | Harness to write | Binds at |
|---|---|---|---|
| **PW-7** | SI-7 | prim-adapter coverage: every declared property of all five registered types appears **and dirties**. `prototypes/stage-free-transport/probe.cpp` measured the *absence* (`EV-081`); no prototype registers a real `UsdImagingSceneIndexPrimAdapter`, so M0 writes one | M1 |
| **PW-8** | SI-6 | initial population both ways — notice-driven (`HdMergingSceneIndex::InsertInputScenes` replays `PrimsAdded`) and the bounded first-`GetChildPrimPaths` traversal. `prototypes/chain-order/src/probeChainOrder.cpp --renderindex` plus a legacy `HdRenderIndex::New` path | M1 |
| **PW-9** | E-1r | the ragged chunk path (`cvCount == 0` + `cvOffsets`) against the uniform path, in `prototypes/data-plane-benchmark/tbbBench.cpp` → `benchUsdGenChain --ragged`. Measurable only once `UsdGenCurveSource` exists | M2 |
| **PW-10** | S-1 | S-1 re-measured **at 100 k × 8 CV**, refineLevel 2, 1280×720 and 1920×1080: `bench_hair.cpp` plus a `make_scene_100000.py`, and rewrites of `make_scene_40000.py` / `make_scene_200000.py` (one parameter off `make_scene.py`; neither was copied). Replaces the interpolated ~12.2 ms of `09-…` §0.2 with a measurement | M1 |
| **PW-11** | R-1 | motion profiles P1/P2: extend `prototypes/motion-blur/mbbench.cpp` (memory floors only) with the retained per-offset cache and the `k·tail` re-evaluation, then run it against hdPrman. No RenderMan on this host, so it is a T4 workstation item | release (built M7) |
| **PW-12** | S-4, S-5, S-6 | fold `eglctx.h` into `prototypes/storm-throughput/hairbench.cpp` so the Hydra GPU counters (`drawBatches`, `drawCalls`, `itemsDrawn`, `vboRelocated`) run headless here rather than needing a display | M1 (S-5, S-6), M2 (S-4) |
| **PW-13** | SI-9 | the pruning-wrapper cost on a production-density UsdSkel-skinned scalp. `probeChainOrder.cpp` proves the wrapper works headlessly but never times it | M2 |

**Numbering note.** This section owns the numbering (`10-…` §6.5);
`appendix-B-prototype-inventory.md` §13 publishes the same thirteen items with the same ids.

---

## 2. Milestones M0–M8

Milestone ids are **M0–M8** (ADR §7); "S" numbers always mean brief §2 decisions, never phases.
Estimates are calendar weeks with three engineers plus a part-time TD, and eng-weeks of effort; both
are ASSUMPTION, derived in §4. Each milestone gives goal, scope (named against the sibling that
specifies it in full), exit gates, freezes, risks and the prototypes carried in
(`appendix-B-prototype-inventory.md` holds the complete mapping).

### 2.0 Gate → milestone

Transcribed from ADR §9 R40, then completed from `09-performance-and-benchmarks.md` §5 for the nine
ids R40 does not name (SI-10, SI-11, S-11, S-12, T-EXPR-1, T-PTEX-1, B-2, SI-12, SI-13 — "where 09 already places a gate
not named here, 09 stands"). Every gate id in the registry appears below exactly once as an exit,
except the five pre-work gates (E-4, S-8, S-9, L-1, SI-8), which appear twice — as an M0 recording
requirement and at the milestone where they become binding — and SI-11, a record-only M0 item.
Re-runs are regression checks, not exits.

| Milestone | Exit gates (all green to tag) | Re-runs, not exits |
|---|---|---|
| M0 | B-1; PW-1…PW-6 results and decision lines recorded (E-4, S-8, S-9, L-1, SI-8 pre-work; SI-11 record only) | — |
| M1 | E-1, E-2, E-6, E-7, E-8; SI-1, SI-2, SI-3, SI-4, SI-5, SI-6, SI-7, SI-8; S-1, S-5, S-6, S-8, S-9, **S-12**; L-1, L-2 | — |
| M2 | E-1r, E-3; B-2; SI-9, SI-10, SI-12, SI-13; S-2, S-3, S-4 | SI-2 on the overlaid-prim case |
| M3 | E-4 on the shipped capture, E-5 | E-1 with 7 nodes and on `head1M` |
| M4 | L-3, L-4, L-5; **T-EXPR-1**, **T-PTEX-1**; the map/expression goldens of §2.5 | E-1, E-5, E-8 with the map and expression nodes in the chain |
| M5 | S-7; T-1, T-2, T-3, T-4 | — |
| M6 | T-INST-1, T-INST-2 | — |
| M7 | **S-11**; the P2 sample-count assertion, `usdrecord --renderer GL`, the T1 hdPrman sampled-points contract | E-5 at 1 M; full re-run of every E-*, SI-*, S-* on G1–G4; R-1 is *built* here, exits at release |
| M8 | S-10, T-5; per-landing T0 value and T1 invalidation tests | E-1 as the catalogue grows |
| release | R-1, R-2, R-3 (all T4) | — |

`09-…` §5 prints every R40 assignment as R40 states it (re-checked 2026-09-05): **SI-5** at M1 (M0 is
where it first goes green, as a vertical demo; §2.1), **S-6** at M1, E-3 M2, E-8 M1, S-4 M2, T-INST-1
tier T1, E-1 ≤ 1.5 ms; and every binding id is registered there, including B-1, SI-9, L-2,
L-3/L-4/L-5, S-10, S-11 and S-12.

Where R40 is silent, 09 stands: **S-12** (T2, prim-count sweep at 100 k × 8 CV, M1 *before C2
freezes*) and **S-11** (T2, `head1M` static draw, record only, M7) are 09 §5.3 gates and appear above
as exits of M1 and M7; **SI-10** (T1, two indices attached to one session agree on generation, prim
set and frame — the check `12-risks-decisions-open-questions.md` RK-09 asked for) exits M2 beside
**SI-9**, the pruning-wrapper cost; **SI-11** (the `reorder nameChildren` record, PW-6) is recorded at
M0; **T-EXPR-1** and **T-PTEX-1** (both T0, M4) are 09 §5.4 gates and exit M4.

### 2.1 M0 — Skeleton, chain order, test tiers, pre-work · 2 wk / 4 eng-wk

**Goal.** Retire chain placement, the one risk that would invalidate the design, and stand up the
machinery every later milestone is measured by.

**Scope** (`10-build-dependencies-testing.md` §1.2, §1.6). All **thirteen** target names of `10-…`
§1.2 fixed, with the install layout; the eight that build now are `usdGenMath`, `usdGen_seexpr`
(**noise only** — `Noise.h`/`Noise.cpp`, S38), `usdGen`, `usdGenImaging`, `usdGenSchema`,
`usdGenTestUtils`, `usdGenTestUtilsHd` and `nanoflann` (`10-…` §1.6). The rest land with their first
consumer: `usdGenShaders` at M1; `usdGen_ptex` and the SeExpr **interpreter** sources at M4; `_usdGen`,
the `usdgen` package and `usdGenUsdview` at M5 (ADR §9 R5) — so M0 builds no usdview shell; the plugin
container and its `UsdGenToolState` arrive with the tools milestone. The codeless schema (S9) with the
container types, the
abstract bases and three operator stubs — plus `libusdGenSchema.so`, the minimal schema library ADR §9
R19 requires: schema tokens and `UsdGeomRegisterComputeExtentFunction` for `UsdGenDescription`,
`implementsComputeExtent: true` on the type, `LibraryPath` in the plugInfo, linking `usd`/`usdGeom`
only (the schema classes stay codeless; `usdGen` core stays free of `usd`). The four registrations of
ADR §5.1; `bin/_env.sh` (S46); the T0–T4 tier definitions and CI, including the T4 workstation
protocol *written down at M0* so no release criterion is invented later under pressure —
`docs/workstation-protocol.md` §§1–11 (`10-…` §5.2), whose runner is built at M7 (§2.8). PW-1…PW-6
(§1.1); the PW-7…PW-13 harnesses (§1.2) may start here but bind later.

**Exit gates.** **B-1** (T0): `objdump -p libusdGen.so` shows no
`libusd_{usd,usdImaging,hd,hdSt,hdx,glf,garch,hgi*,usdAppUtils}.so` in `DT_NEEDED` (`10-…` §1.4,
`testUsdGenLinkRule_*`; ADR §9 R37). All six PW-1…PW-6 results recorded with raw output and a decision
line (the PW-7…PW-13 harnesses of §1.2 are not M0 exit criteria).
`Usd.SchemaRegistry().IsConcrete("UsdGenScatter")` True from plain Python; a clean `cmake --build` on
Linux/aarch64 with `-ffp-contract=off` on `usdGenMath`; `usdcat --flatten` on the M0 fixture reports a
non-empty `extent` on the `UsdGenDescription`. **Vertical demo (not a gate id):** the chain dump shows
the plugin strictly after `HdsiSceneGlobalsSceneIndex` and the whole UsdImaging chain and before every
`HdSt_*` plugin, with a second assertion using `SetPluginOrderingPolicy` and synthetic `hdPrman:*` tags
so RenderMan's phase numbers are covered without RenderMan installed — SI-5 formalises this assertion
and is registered as an M1 exit (ADR §9 R40).

**Freezes.** None, but target names, install layout and tier definitions are fixed here. **Risks.** Low:
the plugInfo `ordering` tag is decorative in stock 26.08 and placement is guaranteed by phase 0 /
`InsertionOrderAtEnd` (ADR §1, S1). **Carried in.** `chain-order/`, `evaluation-scheduling/chainOrder.cpp`,
`storm-hair-look/eglctx.h`, `usdrig-linux-build/rigexec_env.sh`.

### 2.2 M1 — Straight hair end to end · 5 wk / 14 eng-wk · freezes C1, C2, C5

**Goal.** Prove every structural claim on the smallest feature — adapter invalidation, exact-size
arrays, the commit model, the tile contract, the glslfx binding: a mesh plus a `UsdGenGroom` renders
hair in usdview under the plugin's own shader, and dragging `usdGen:noise:magnitude` on the
`UsdGenNoise` prim (`02-schema.md` §2.7; ADR §9 R7) updates it in one cook.


**Scope.** Operators `UsdGenScatter` (`mode = random`), `UsdGenGrow`, `UsdGenNoise`, `UsdGenLength`,
`UsdGenWidth`, each carrying `UsdGenMaskAPI` (auto-applied, with PW-5's explicit fallback) — the full
mask property list is frozen into C1 here even though only the constant and `mask:random` terms evaluate
before M4. The engine of `03-execution-engine.md` in full: chunk arena, capture/evaluate, the three
digests, `UsdGenDirtyRouter`, the private `tbb::task_arena`, per-node caches. Imaging
(`06-imaging.md`): one adapter class under five plugInfo entries plus the `UsdGenRestAPI` adapter; the
`UsdGenImagingRegistry` session keyed by (weak stage | `uniform string usdGen:sessionId`, groom root),
several indices attaching to **one** session (ADR §4.5); the tile publisher with
`chunksPerTile = max(1, ceil(nChunks / tileTarget))` and
`nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` (ADR §9 R21) over
`uniform int usdGen:tileTarget = 64` — 100 k curves ⇒ 196 chunks, 4 chunks per tile, 49 tiles; commit
triggers (a)/(b)/(c) as amended by ADR §9 R32; initial population on both paths; `usdGen:surface` over
**`Mesh` and `GeomSubset` targets** (ADR §9 R15), where `primvars:skinprim` and Ptex face ids are always
indices into the *parent mesh* and a membership edit is a recapture. Look
(`07-look-maps-expressions.md` §2.1): `usdGenShaders` with `UsdGenHairPreview`,
`UsdGenHairPreviewTranslucent` (ADR §5.4 variant A) and `UsdGenHairPreviewPrimvar` (variant B, which
S-8 may promote; ADR §9 R4) and the three-terminal `Material`; the `inputs:` block shared by all three
is what C5 freezes. Plus canonical example (a) and the `UsdGenStats` block.

**Exit gates.**

| Gate | Tier | Pass criterion |
|---|:--:|---|
| SI-1 / SI-2 | T1 | `points.size() == Σ curveVertexCounts` on every tile, hard assert; a `usdGen:noise:magnitude` edit dirties exactly `primvars/points/primvarValue` + `extent/*` on the affected tiles and nothing else |
| SI-3 / SI-4 | T1 | 10 interactive parameter edits ⇒ exactly 10 commits. Every operator/map/surface-topology dirty commits synchronously at the end of the `_PrimsDirtied` batch that carried it, on the **notice** thread, whether or not an app driver is attached (trigger c, ADR §9 R32); trigger (a) is exercised separately for `SetTime` and for an explicit `Commit()` after a live-override change. **Zero commits on any Hydra worker thread** — `GetPrim` only `atomic_load`s. 8 readers × 100 publishes ⇒ 0 torn reads |
| SI-5…SI-8 | T1 | the M0 chain-order assertion over the shipped plugin; notice-driven and first-`GetPrim` population give identical prim sets; every declared property of every registered type appears and dirties (no prefix filter, ADR §9 R6); auto-applied `UsdGenMaskAPI` confirmed on shipped code |
| E-1 / E-2 | T0 | 5-op chain, G3 (100 k × 8 CV), full run in the private 8-thread arena ≤ **1.5 ms** (MEASURED 1.02 ms at 8 threads, row `EV-008`; 1.72–1.91 ms at 20 threads, row `EV-001`; `research/G-data-plane-engine-prototype-benchmark.md` §3.3, §4; ADR §9 R27); 1 % of chunks dirty ≤ 0.10 ms (MEASURED 0.035–0.044 ms at 20 threads, row `EV-002`; the 8-thread figure is UNMEASURED and E-2 produces it) |
| E-6 / E-7 / E-8 | T0 | append a node to a synthetic 200-node groom ≤ 0.2 ms, exactly one node rebuilt; 1 → 8 threads ≥ 3× on E-1 (MEASURED 3.90 → 1.02 ms = 3.8×, row `EV-008`); 1 vs 8 threads bitwise identical with `-ffp-contract=off` |
| S-1 / S-5 | T2 | 100 k × 8, refineLevel 2, **at 1280×720 and 1920×1080**; 720p ≤ **12.8 ms** (the ledger budget, `09-…` §0.2), 14 ms retained only as a regression ceiling; 1080p recorded as its own baseline, never compared with 720p. **Re-measured here; the ~12 ms figure is INTERPOLATED, UNMEASURED** (ADR §9 R41, Q-12). 49 tiles, one material, one refineLevel ⇒ `drawBatches == 1`, `drawCalls == 1` |
| S-6 / S-8 / S-9 / L-1 / L-2 | T2 | `vboRelocated == 0` across deform frames; the PW-2/3/4 decisions confirmed on shipped code; the chosen glslfx variant compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` with zero warnings under both `defaultMaterialTag` and `translucent`; the `USDGEN_STORM_MATERIAL_OVERRIDE` default set and asserted; L-2's Sdr parse of all three glslfx files, golden image and CPU/GPU bake-parity clause (`07-…` §9.1) |
| S-12 | T2 | prim-count sweep at 100 k × 8 CV over the G3 variants of `09-…` §4.4 — 1 / 32 / 128 / 196 tiles — at the default `uniform int usdGen:tileTarget = 64`: **≤ 1.15× the one-prim frame time and `drawBatches == 1`** (`09-…` §5.3). It runs **before C2 freezes**, because it is the measurement that justifies the tile default; the CPU half is already MEASURED (index build 2.607 ms at 1 prim vs 0.572 ms at 128, row `EV-028`; points resolve 0.224 → 0.053 ms, row `EV-029`) and the GPU half is what this gate produces |

Two further T1 assertions: a `GeomSubset` `indices` edit bumps the capture epoch and re-dirties only
the tiles whose roots left or entered the subset; a second index attaching to a live session
republishes the same generation and prim set. **Vertical demo (not a gate id):** an EGL golden image at
refineLevel 2, ≤ 0.5 % changed pixels, beside example (a).

**Freezes.** C1, C2, C5. **Risks.** Adapter invalidation precision is the hard stop of §5.1. Second is
the tile arithmetic: 512-curve chunks at 1 M curves cannot also be ≤ 256 prims (ADR §1, S23/S27); a
wrong mapping shows up as SI-2 dirtying whole descriptions. M1 is the densest milestone and carries the
stretch the delivery judge applied (`design/judge-delivery.md` §4 item 5, §7). **Carried in.**
`data-plane-benchmark/`, `stage-free-transport/`, `evaluation-scheduling/`, `storm-hair-look/`,
`storm-throughput/` → `tests/perf/benchUsdGenChain.cpp`, `tests/testUsdGenAdapter.cpp`,
`tests/testUsdGenScheduling.cpp`, `usdGenShaders/`, `tests/perf/benchUsdGenStorm.cpp` (the CTest
names are `10-…` §5.6's).

### 2.3 M2 — Deform and freeze · 4 wk / 11 eng-wk · freezes C3

**Goal.** Read a deforming surface from whoever produced it (brief R5) and make a frozen curve and a
generated curve the same kind of styler input (brief R3): hair tracks a usdRig-deformed and a
UsdSkel-skinned scalp, an operator's output freezes to a `BasisCurves` and re-enters through
`UsdGenCurveSource`, and undo restores the pre-freeze stage.

**Scope** (`05-static-curves-and-deformation.md`). `UsdGenDeform`, `UsdGenCurveSource`, `UsdGenFreeze`
(`uniform token usdGen:frozen:mode = "frozen"`, values `frozen | live`;
`uniform token usdGen:frozen:tier = "session"`, values `session | sublayer | payload`, advisory — the
evaluator never reads it, ADR §9 R6, R18), `UsdGenSculptLayer`; the private
`HdSiExtComputationPrimvarPruningSceneIndex` wrapper (S3); the first consumer of the M1 `UsdGenRestAPI`
adapter (`usdGen/rest/points` at `UsdTimeCode::Default()`, an authored `primvars:rest` honoured when
present, S12); `UsdGenCurveAPI` with `primvars:usdGen:role = hair | guide`; the bare-`primvars` dirty
rule for overlays (S5; ADR §9 R29); `SubtreeSnapshot` in `usdGenUndo.py` and the `SetActive(false)`
freeze-undo rule (S41); canonical example (b).

**Exit gates.** **E-1r** (T0, the ragged path — `cvCount == 0` + `cvOffsets` — within 2× the uniform
path; `UsdGenCurveSource` is what first produces ragged buffers). **E-3** (T0, terminal-parameter edit
≤ 0.6 ms; MEASURED 0.179–0.411 ms at 20 threads, row `EV-004`; the 8-thread figure is UNMEASURED and
E-3 produces it, ADR §9 R27). **SI-9** (T1, pruning-wrapper cost on a production-density skinned scalp;
red is stop condition SC-13). **SI-10** (T1, two attached indices report the same generation, prim set
and frame for one commit, `testUsdGenSessions`). **S-2** (T2, deform frame ≤ 2.0 ms
delta over static at 100 k × 8 CV, 1280×720, 49 tiles. Anchor: the measured deltas are +0.83 ms at 40 k
and +2.46 ms at 200 k, row `EV-022`; interpolated **linearly in curve count** they give ≈ **1.44 ms**
at 100 k over 9.6 MB, **DERIVED from `EV-022`** — ≈ 1.4× headroom. `09-…` §0.2 and §2.1 fix that as
the method: interpolating the per-MB *rates* instead (0.13 ms/MB at 200 k → ≈ 1.25 ms) is explicitly
not what the ledger uses. UNMEASURED at 100 k; S-2 produces the number). **S-3** (T2, dirtying 1
of 49 tiles ≤ 1.2× static). **S-4** (T2, camera framing ¼ of the 200 k-curve, 80 k-face body fixture G4
drops `itemsDrawn` ≥ 3× at 32 and 196 tiles, not at all at one prim; `09-…` §4.4). SI-2 re-run on the
overlay case (`ComputeDirtyLocators` **and** the bare `primvars` locator). Two named T1 tests:
`testUsdGenSkelInterop` and `testUsdGenFrozenReentry` (bit-for-bit match, re-entry, a stale epoch warns
once and still renders); the freeze/undo rule itself — `SubtreeSnapshot` plus `SetActive(false)` — is
covered by the T0 Python scripts `test_usdgen_undo.py` and `test_usdgen_freeze_author.py` and by the
T1 `testUsdGenStageEdits` (`RemovePrim` containment), all three registered in `10-…` §5.6; no separate
`testUsdGenFreezeUndo` binary exists or is needed. Freeze cost ≤ 1 ms authored plus apply at 1 M curves
(MEASURED ceiling 0.59 ms, flat in curve count, row `EV-045`;
`research/G-freeze-bake-undo-and-frozen-reentry.md` §4.1); the sidecar `.usdc` reopens in ≤ 1 ms
(MEASURED 0.4–0.8 ms, row `EV-047`).

**Freezes.** C3. **Risks.** The `RemovePrim` bug (`pxr/exec/esfUsd/stageData.cpp:361` — the
`UsdPrimDefaultPredicate` call on a possibly-invalid prim; `:360` is its comment line. S41,
`appendix-A-evidence-ledger.md` §3.10) is certain, contained
and never relied on. Ragged CV counts arrive here; the ragged path is v1 (ADR §4.2.2) and E-1r measures
its penalty before M3 builds `UsdGenResample` on it. **Carried in.** `freeze-bake/` →
`testUsdGenFrozenReentry`, `testUsdGenStageEdits` and `test_usdgen_freeze_author.py` (`10-…` §5.6);
`chain-order/stages/skelAndRig.usda` as the interop fixture.

### 2.4 M3 — Guides and clumps · 5 wk / 13 eng-wk

**Goal.** Cross-curve information without cross-chunk reads: a guide set drives interpolated hair,
multi-level clumping is deterministic across thread counts, and example (a) renders in full.

**Scope** (`04-operators.md`). `UsdGenGuideSet` with per-guide `float[] usdGen:blend`; `UsdGenScatter`
gains `mode = atGuides`; `UsdGenGuideInterpolate` (`regionMap`, `clumpCrossover`, emitting
`uniform int[] guideIndex` and `uniform float[] guideWeight`, both `elementSize = 3`, ADR §9 R24);
multi-level `UsdGenClump` (emitting `clumpId_<level>`, uniform int, numbered by
`uniform int usdGen:clump:level`, ADR §9 R10) with `rel usdGen:clump:centers` as an explicit input;
`UsdGenSmooth` (along-curve only), `UsdGenResample`, `UsdGenDirection`; `rel usdGen:mask:region`.
Engine: invariant **I3**, the reference lane (ADR §4.1, §9 R1) — hair chunks never read hair chunks;
guides, clump centres and card roots are un-chunked reference buffers evaluated to completion first,
consumers storing resolved indices and weights at capture; nanoflann kd-trees, Morton-sorted roots,
surface-major chunk order.

**Exit gates.** **E-4** (T0, on the shipped capture, threshold from PW-1). **E-5** (T0, RSS ≤ 800 MB at
1 M × 8 CV with 5 nodes — MEASURED baseline **668 MB with per-node buffers** (**293 MB with a single
shared buffer**), row `EV-007`; `research/G-data-plane-engine-prototype-benchmark.md` §5, §8. One
caveat travels with that number: the probe stored only `VtVec3fArray` per node — the position plane
alone — so `09-…` §2.6 runs two accountings that add `hairT`, `widths`, the per-curve arrays, the
capture caches and the publish buffer, giving **770–860 MB** on the per-element model and
**950–1050 MB** measured-anchor-plus-model, both DERIVED. That is why the gate is *tight*, why 09
records E-5 as "UNMEASURED for the shipped chain and at risk", and why eviction is load-bearing).
Re-runs: E-1 with a 7-node chain, and **E-1 at scale** on
the `head1M` fixture E-5 uses. `09-…` §5.1 sets **no 1 M threshold** for E-1 and records 1 M at 8
threads as UNMEASURED; the MEASURED anchor is 7.59–7.67 ms at 20 threads (row `EV-006`, ADR §9 R27).
Registering a 1 M sub-criterion — a number and its thread count — in 09 §5.1's E-1 row is an edit filed
against 09, which owns every threshold (ADR §9 R40); until it is registered the plan's 1 M claim is
ungated until R-1 at release. Plus a T1 assertion that a guide edit dirties only the tiles referencing
that guide, and a correctness test for a clump whose centre lives in another chunk.

**Freezes.** None — additive (ADR §3). **Risks.** Capture cost at 1 M roots is the last unmeasured number
in the engine; PW-1 is the early warning. Widening chunks is not a fallback — 4 096-curve chunks are a
measured 5× cliff (row `EV-009`; ADR §4.2.5) — so the fallback is progressive capture plus the
interactive LOD ceiling (SC-6). If S-4 at M2 shows tile culling rejecting nothing, the chunk partition
changes from Morton-over-roots to a UV-island partition *before* M3 ships
(`design/proposal-performance.md` §13 R1 — a register item, written bare so it cannot be read as the
render gate `R-1`; `10-…` §8.4). **Carried in.** `data-plane-benchmark/sampler.h`; the
vendored nanoflann header; PW-1's kNN benchmark → `tests/perf/benchUsdGenKnn.cpp`.

### 2.5 M4 — Maps and expressions · 4 wk / 11 eng-wk

**Goal.** Everything a groom reads from a surface, evaluated on the CPU at capture and baked to primvars
(S37): a density map, a length expression and a Ptex mask each change the groom, colour comes off an
image or Ptex map, and "Reload maps" picks up an overwritten file while nothing else does.

**Scope** (`07-look-maps-expressions.md`). The `usdGen_seexpr` and `usdGen_ptex` targets (static,
hidden, never installed next to USD); `UsdGenImageMap`, `UsdGenPtexMap`, `UsdGenExprMap`,
`UsdGenPaintMap`, `UsdGenNoiseMap`, `UsdGenCombineMap`, `UsdGenGuideProximityMap`; the a host groomer variable set
with `map()`/`ptex()`/`rand()`, one thread-safe `VarBlock` and one `PtexFilter` per worker (S38);
`UsdGenLookAPI` with its explicit bake order; `UsdGenImaging_ReloadMaps`; the mask block resolved once
per capture into one `VtFloatArray` plus a 257-entry LUT. Plus `UsdGenScale`, the last v1 styler moved
in by ADR §6 and scheduled here because its mask input is the map block this milestone delivers.

**Exit gates.** **L-3** (T0, bitwise-identical baked primvars at 1 and 8 threads,
`Expression::isThreadSafe()` true for every shipped function, `rand()` reproducible). **L-4** (T0/T1,
map capture cost at 1 M roots on 8 threads inside its threshold — proposed ≤ 200 ms for one image map,
one Ptex map and one expression, ASSUMPTION — and `PtexCache::Stats.fileReopens == 0` at the default
cache size, which also settles `USDGEN_PTEX_MAX_FILES` and the two cache-size defaults). **L-5** (T1,
`ReloadMaps` re-captures exactly the nodes whose maps changed and dirties exactly the baked primvar's
`primvarValue` on exactly the affected tiles; `displayColor` never co-dirtied with `points`).
**T-EXPR-1** (T0, the SeExpr sandbox: `SE_EXPR_PLUGINS` cleared before `ExprFunc::init()`, a closed
function set, a parse error yielding a fallback value and never an exception into Hydra). **T-PTEX-1**
(T0, Ptex face-id agreement: ids match `Far::PtexIndices` on quads, n-gons and all-triangle meshes, and
a `GeomSubset` does not shift them). Both are registered gates of `09-…` §5.4 at T0/M4, not assertions
folded into L-3/L-4. Their drivers are `testUsdGenExpr --sandbox` and `testUsdGenPtex` (`10-…` §5.6).
Golden per-curve values for a density map, a length expression and a Ptex mask; a T0 value and a T1
invalidation test for `UsdGenScale`; a paint round-trip primvar → EXR → `UsdGenImageMap` and primvar →
`.ptx` → `UsdGenPtexMap` at ≤ 1e-4 error; nothing changes without `ReloadMaps` (S13); the offline build
gate — SeExpr and Ptex from `thirdparty/` via `FETCHCONTENT_SOURCE_DIR_*` with no network. Re-runs, not
exits: E-1, E-5 and E-8 with the map and expression nodes in the chain.

**Freezes.** None — the mask property names were fixed at M1; this milestone only makes the map-fed
terms evaluate (ADR §3). **Risks.** Third-party vendoring: bison/flex, and symbol clash with a DCC's own
SeExpr copy; static plus `-fvisibility=hidden` is the mitigation, KSeExpr's `USE_PREGENERATED_FILES` the
fallback. Ptex face ids are always indices into the **parent mesh's** faces, including under a
`GeomSubset` target (ADR §9 R15) — the fixture must cover a subset. **Carried in.** `thirdparty-bench/`
→ `tests/testUsdGenExpr.cpp`, `tests/perf/`; the MEASURED 13–117 ns/eval (row `EV-067`) and
23 ns/lookup (row `EV-070`) become the T0 regression floor.

### 2.6 M5 — Tools · 5 wk / 13 eng-wk · freezes C4

**Goal.** The app → data → Hydra interaction loop (brief R9), one stage write per gesture: an artist
combs, cuts, paints density, places guides, freezes an operator and undoes all of it at interactive
rates, with no stage write during any drag.

**Scope** (`08-tools.md`). The `extern "C"` control ABI — the **nineteen** entry points of
`08-tools.md` §1.4 (ADR §9 R31's eighteen minimum entry points plus `UsdGenImaging_GetLastError`),
including `GetTopologyGeneration`, `SetContext`, `PickCV`, `ClosestSurfacePoint`,
`Footprint`, `SetInteractiveLOD`, `BuildMirrorMap`, `SetMaskVisualisation`, `GetStatsJson` — and the
`_usdGen` pxr_boost array module (C4); the `usdgen` Python package (`lib/python/usdgen/`) carrying
`_usdGen`, `usdGenLib.py` and the authoring facade that mirrors `rigexec.Builder` (ADR §9 R5); the
`usdGenUsdview` `PluginContainer` with `UsdGenToolState` built in `__init__` and lazy Qt imports; the
ten-brush shelf on the four-phase press/move/release/escape contract, with live overrides during the
drag and CV display as a synthesized `points` child prim; undo via `SubtreeSnapshot` plus
`SetActive(false)` for freezes, every structural edit in one `Sdf.ChangeBlock` with `Define` outside it;
parameter panels generated from `UsdPrimDefinition::GetPropertyNames()`; the stack editor whose drag
rewrites `usdGen:input` (never `reorder nameChildren`); the panels of `08-tools.md` §5 — stack profiler
column, freeze bar, status line, mask visualisation, "show driving guides"; **symmetry (mirror-X)**, an
M5 requirement and not a late add (ADR §6); the interactive LOD control; `UsdGenScatter` `mode = points`.

**Exit gates.** **T-1** (T3, ≤ 1 ms Python plus engine per brush move at 100 k; the Python half MEASURED
at 21.3 µs, row `EV-061`, `research/G-tool-loop-array-transport-and-cv-picking.md` §1.4; end to end
UNMEASURED). **T-2** (**T1**, `UsdGenImaging_PickCV` ≤ 0.25 ms at 100 k CVs and ≤ 2.5 ms at 1 M;
MEASURED 166 µs / 1.66 ms, row `EV-064`, ibid. §2.4). **T-3** (T3, CPU pick vs `view.pick()` over 100
pixels disagrees only on occlusion). **T-4** (T3, one freeze on a 2 205-prim stage: ≤ 5 ms of authoring
— MEASURED 3.45 ms, row `EV-050` — and **exactly one** `_resetGUI`, achieved by batching every freeze
into one `Sdf.ChangeBlock`; usdview's own `_resetGUI` costs 35.4 ms at this prim count, MEASURED, row
`EV-051`, and is not part of the budget). **S-7** (T2, parked-CV density scrub ≤ 1.5× a points-only
frame — a brush behaviour, so it exits with the brushes). Plus `layerChangeCount == 0` between press
and release, `UsdGenImaging_GetGeneration()` advancing exactly once per commit, and Escape leaving no spec. The
end-to-end fps number is **not** a gate here — it is RC-4 (§6.4), per `design/judge-delivery.md` §4
item 4.

**Freezes.** C4. **Risks.** numpy's multi-threaded BLAS destroys brush latency on a loaded host: a
`(100k,4)@(4,4)` matmul measured 22 ms at load 50 versus 0.56 ms pinned (MEASURED, row `EV-065`,
ibid. §1.5), so `usdGenLib.py` pins `OPENBLAS_NUM_THREADS`/`OMP_NUM_THREADS` to 1 before importing
numpy. The pxr_boost module is pinned to the USD build, so control stays on the ctypes surface and its
absence degrades tools, never correctness. **Carried in.** `tool-loop/` → the C ABI in
`usdGenImaging/registry.cpp`, `python/_usdGen.cpp`, `tests/perf/`;
`freeze-bake/uv/testUsdviewFreezeCost.py` → the T3 freeze suite.

### 2.7 M6 — Instancing · 3 wk / 7 eng-wk

**Goal.** Cards, archives and spheres as real instancers, and grooms on natively instanced scalps:
example (c) renders, a click on a card selects the description, and an `instanceable` scalp referenced
eight times produces one prototype's worth of curves and eight instances.

**Scope** (`06-imaging.md`). `UsdGenInstance` with `primitive = cards | archives | spheres`; the
instancer builder with hand-authored `instancedBy` (exactly one path) on prototypes re-rooted under
`<Description>/__usdGenRender/inst_<opName>/Prototypes/<n>`, absolute `primOrigin` outside prototypes
and relative inside; per-instance variation through `instance`-interpolated primvars, material variety
through multiple prototypes; native-instance grooms discovered from `__usdPrimInfo.isNiPrototype` /
`niPrototypePath` (propagated prototype names are hashes — discover, never construct, S34);
`<Description>/Prototypes` pruned from the render; the S7 metadata plugin wired for real.

**Exit gates.** **T-INST-1** (T1, pick round-trip: `ComputeInstancerContext` non-empty — it is empty
without `primOrigin`); **T-INST-2** (T1, prototype rebasing inside propagated native prototypes). Plus a
valid instancer through Hydra-1.0 emulation; a card-transform edit dirties
`primvars/hydra:instanceTranslations` and **not** `instancerTopology`; two scalps with different groom
bindings aggregate separately. **Vertical demo (not a gate id):** example (c) through the T2 EGL harness
with `drawBatches`/`drawCalls` at 10³ and 10⁵ instances and the translations-vs-topology dirty ratio
measured (`research/G-instancing-cards-archives-and-native-instances.md` §9 items 3 and 5), ≤ 0.5 %
changed pixels against the golden.

**Freezes.** None (ADR §3). **Risks.** Low on the scene-index side: the instancer contract, `instancedBy`,
`primOrigin` and the pick round-trip are MEASURED headlessly (ibid. §2, §6). Storm's *rendering* of
instanced curves is UNMEASURED — that report's §9 is a workstation protocol and states plainly that
nothing in it was rendered — which is why M6 carries a T2 demo and not only T1 gates. **Carried in.**
`instancing/` → `tests/testUsdGenInstancer.cpp` and its fixtures.

### 2.8 M7 — Render time and hardening · 5 wk / 11 eng-wk

**Goal.** One binary renders the same groom in Storm and in an hdPrman-class delegate with no usdview
and no C API involved (S8), with correct motion blur.

**Scope.** Motion profiles P1 (`velocities`, opt-in) and P2 (`samples`, lazy on first interval pull or
app preflight, 2–16 samples, constant CV count across the shutter) — the S32 names ADR §9 R1 preserves
— with `UsdGenMotionCache` keyed by `(graphGen, surfaceGen, absTime)` and lerp+clamp for non-retained
times; `EvaluateSample` restricted to the deformed-space tail; the app preflight entry point; the
explicit render context (`USDGEN_CONTEXT=render`, `UsdGenImaging_SetContext()`, the usdview toggle) and
`usdGen:renderDensityScale`; the memory budget and the `recomputeCostMs/bytes` eviction score;
**installed public headers**; the tier-T4 runner over `docs/workstation-protocol.md` §§1–11, the
protocol M0 wrote down (`10-…` §5.2). That runner is not the B1–B12 set: B1–B12 is the tier-T2 Storm
benchmark protocol of `research/G-storm-throughput-and-prim-granularity.md` §3.5, which ships as the
`benchUsdGenStorm` sweep of `09-…` §4.1 and §4.4 and is recorded at release under RC-3 (§6.4).

**Exit gates.** **S-11** (T2, `09-…` §5.3: the `head1M` G3 variant, static draw, refineLevel 2, 720p —
**record only, no threshold**; it establishes whether `09-…` §2.5's linear draw fit holds past 200 k,
and the ~118 ms figure that motivates it is DERIVED, extrapolated 5× from rows `EV-019`–`EV-021`). A T1
assertion that P2 at k = 3 costs one head plus three tail evaluations, read from
`UsdGenStats`. `usdrecord --renderer GL` (T3, Xvfb) renders the canonical stages with no usdview and no
C API involved. The sampled-points contract for an hdPrman-class delegate asserted headlessly in T1
against `research/G-hdprman-and-usdrecord-render-time-chain.md` §3 — 2–16 stable times from
`GetContributingSampleTimesForInterval`, constant CV count across the shutter, zero extra chain
evaluations. Re-runs, not exits: E-5 at 1 M plus the motion cache, and a full re-run of every E-*, SI-*
and S-* gate on G1–G4. **The real `usdrecord -r "RenderMan RIS"` run is release criterion RC-7, not an
M7 exit** (§6.4); R-1 is likewise a T4 release criterion built here (ADR §9 R39).

**Freezes.** None new, but the *not-installed* boundary closes: the `usdGen` and `usdGenImaging` public
headers are installed and stop churning (ADR §3). **Risks.** No RenderMan on the dev host: plugin
ordering is unit-testable with synthetic tags and the sampled-points contract is implemented to the
letter of the report above. If the per-sample pinned expansion cost measured by R-1 exceeds the render
budget, the fallback is SC-8. The motion-blur prototype is `prototypes/motion-blur/mbbench.cpp`, the
sampling-floor bench of `research/G-motion-blur-sampling-strategy.md` §5, §7 (rows `EV-071`–`EV-075`);
it is carried in — the directory exists, `appendix-A-evidence-ledger.md` §2.9 records the build line
and `appendix-B-prototype-inventory.md` §0.1 and §12 give it a carry-into target. It measures
sample-time plumbing, not the retained cache, so `UsdGenMotionCache` itself is written from the
report's contract, and PW-11 (§1.2) extends the bench with the retained per-offset cache and the
`k·tail` re-evaluation — which is why M7 keeps its stretch. **Carried in.** `storm-throughput/` → the
`benchUsdGenStorm` counter sweep (`09-…` §4.1); `motion-blur/mbbench.cpp` →
`tests/perf/benchMotionSamples.cpp` (the target `appendix-B-prototype-inventory.md` §0.1 names);
`usdrig-linux-build/consumer/` as the out-of-tree linkage check for installed headers.

### 2.9 M8 — Breadth · continuous

**Goal.** The v2 operator vocabulary and the comfort features, on a cadence rather than a deadline.

**Scope.** `UsdGenCurl`, `UsdGenBend`, `UsdGenStraighten`, `UsdGenDisplace`, `UsdGenWave`, `UsdGenPart`
and its brush, `UsdGenExprOp` (capture-time only), `UsdGenScatter` `mode = uniform`, TsSpline ramp
transport on `float <p>:spline`, sculpt rebase by nearest root UV, progressive generation
(`asyncAllow`/`asyncPoll`, `_allowAsync` set during `registerPlugins`), the LOD tiers, the documentation
set, and the **in-place overlay optimisation for frozen curves** (ADR §9 R29): the description keeps
publishing tiles in v1, and M8 measures whether overlaying points on the source prim beats
PrimsRemoved/Added.

**Exit gates.** Per-operator T0 value tests and T1 invalidation tests; **T-5** (T3, the plugin enables
async without `--allow-async`); **S-10** (T2, PrimsRemoved/Added versus an in-place points dirty on a
frozen prim). E-1 stays green as the catalogue grows. No milestone-level exit — M8 is measured per
landing.

**Freezes.** None (ADR §3). **Risks.** Scope creep into simulation; the answer is the v3 line: usdGen
consumes a sim cache through `UsdGenCurveSource`, it does not produce one. **Carried in.**
`evaluation-scheduling/asyncProbe.cpp` → `testUsdviewUsdGenAsync.py` (T-5, `10-…` §5.6);
`thirdparty-bench/se_min.cpp`
→ the `UsdGenExprOp` parser fixture.

---

## 3. Dependency graph and execution model

### 3.1 The edges

Ten edges; nothing else in §2 constrains order.

| Edge | Why |
|---|---|
| M0 → M1 | placement, schema plumbing and the T0–T3 harness must exist before anything is measured |
| M1 → M2 | the tile publisher and adapter are what a frozen curve re-enters through |
| M2 → M3 | guides are C3 curves, so C3 must be frozen before a guide set is authored |
| M1 → M4 | maps are capture-time inputs and capture exists from M1 |
| M3 → M4 (soft) | `UsdGenGuideProximityMap` and `Clump`'s masks want the reference lane; M4 runs without it at reduced scope |
| M2, M3 → M5 | the brushes need freeze/undo and guides to comb |
| M4 → M5 (soft) | the Map-paint brush and the density-map fixtures ship with M4; the other nine brushes do not depend on it |
| M1 → M6 | the instancer is a second publisher over the same generation store |
| M2 → M7 | the deformed-space tail is what motion profile P2 re-evaluates per sample |
| M6 → M7 | per-instance transforms are the second motion-blur consumer: `instanceTranslations/Rotations/Scales/Transforms` blur under hdPrman alongside `points` (`research/G-hdprman-and-usdrecord-render-time-chain.md` §3.2) |

### 3.2 Execution model

Milestones are executed **serially** (ADR §9 R39): all three lanes work inside the current milestone
(§3.3), so the calendar total is the sum of the milestone durations, 33 weeks (§4.1). The dependency
graph is a *re-ordering* tool, not a compression tool. **M6 is the only milestone the graph permits to
be dropped outright**; M4 can be resequenced after M5 only at the cost of deferring the Map-paint brush
and the density-map fixtures. The subsequence that cannot be resequenced is
M0 → M1 → M2 → M3 → M4 → M5 → M7 (2 + 5 + 4 + 5 + 4 + 5 + 5 = 30 of the 33 weeks); it is not a shorter
delivery date. It is dominated by M1 and M3, the two milestones where a measured unknown —
invalidation precision, capture cost — can force a re-plan.

### 3.3 Three lanes

| Lane | Eng | M0 | M1 | M2 | M3 | M4 | M5 | M6 | M7 |
|---|---|---|---|---|---|---|---|---|---|
| Engine | E1 | kernels, PW-1 | chunk arena, digests, dirty router, 5 ops | `Deform`, rest/ragged buffers | reference lane, kNN capture, `Clump` | SeExpr/Ptex, masks, `Scale` | node stats, live-override apply | instance transforms | motion cache, eviction, budget |
| Imaging | E2 | registrations, chain order, PW-2/3/4 | adapter ×5 + RestAPI, session, publisher, triggers, `GeomSubset` | pruning wrapper, freeze re-entry, bare-`primvars` | extents, `__dependencies`, guides | `LookAPI` bake, reload | live overrides, CV `points` prim | instancer builder, native prototypes | render context, hdPrman parity, headers |
| Tools/QA | E3 | T0–T3 harness, EGL, CI, T4 protocol, PW-5/PW-6, the PW-7…PW-13 harnesses, B-1 | golden infra, `testUsdGenContracts`, example (a) | `SubtreeSnapshot`, freeze tests, example (b) | determinism suite, example (a) full | map goldens, paint round-trip | C ABI, `_usdGen`, `usdgen`, brushes, undo | example (c), pick round-trip, demo | T4 protocol runner, docs |

E3 is deliberately loaded with harness and content work through M1–M4 so that M5 is five weeks and not
eight. The lanes converge at three sync points — C1/C2/C5 at M1, C3 at M2, C4 at M5 — each a review,
not a merge window. M1 parallelises worst: adapter, publisher and engine all touch the same contracts,
so expect E1 and E2 to pair for its first two weeks on the `UsdGenGraphDesc` boundary; M3's reference
lane is single-owner work (E1), which is where M4 is pulled forward if M3 slips.

---

## 4. Estimates and contingency

### 4.1 The re-baseline

| M | Name | Calendar wk | Eng-wk | Cumulative wk |
|---|---|---:|---:|---:|
| M0 | Skeleton + pre-work | 2 | 4 | 2 |
| M1 | Straight hair end to end | 5 | 14 | 7 |
| M2 | Deform + freeze | 4 | 11 | 11 |
| M3 | Guides + clumps | 5 | 13 | 16 |
| M4 | Maps + expressions | 4 | 11 | 20 |
| M5 | Tools | 5 | 13 | 25 |
| M6 | Instancing | 3 | 7 | 28 |
| M7 | Render time + hardening | 5 | 11 | 33 |
| M8 | Breadth | continuous | — | — |

Calendar is `design/proposal-risk.md` §9.2 rounded up by one week on every milestone except **M0 and
M6** — M6 needs no stretch because its contract is the most measured in the plan, M0 is fixed-scope:
+25 % on M1/M3/M5/M7 and +33 % on M2/M4. A coarser instrument than ADR §7's "30 % contingency on
M1/M3/M7", with the same intent and the same totals. Effort is the same figures rounded to whole
eng-weeks (+22 % to +38 % where a stretch applies, +0 % on M0 and M6; 67 → 84 eng-weeks). Every number
here is ASSUMPTION.

**Group totals**: **M0–M2 ≈ 11–12 weeks** (a deforming, freezable groom); **M0–M5 ≈ 25 weeks** (an
artist can groom); **M0–M7 ≈ 34 weeks** (renderable on a show) — ADR §9 R39; `design/judge-delivery.md`
§7. The column sums to 33; the quoted 34 carries one week of integration float at the C4 freeze. Do not
quote a tighter number: `design/proposal-risk.md` §9.3's uncontingent 27 weeks is this plan with the
buffer removed. 84 eng-weeks against 99 of capacity (3 × 33): the difference is review, integration,
fixture and content time that no line item names, plus the uncounted part-time TD.

### 4.2 Staffing assumptions

Three engineers with the split of §3.3 — numerics and TBB, Hydra 2.0 scene indices, Python/Qt and test
infrastructure — plus a part-time TD for fixtures and look sign-off. All ASSUMPTION (Q-16). Two
engineers does not scale the calendar by 1.5: it serialises M1's three concurrent workstreams and adds
the whole tools lane to the tail, so M0–M5 lands nearer 34 weeks than 25.

### 4.3 What shrinks and what grows the plan

Shrinks: carrying the prototypes in rather than rewriting them (every "carried in" list is load-bearing
for its estimate; `appendix-B-prototype-inventory.md` is the mapping); dropping M6 (self-contained and
off the non-resequenceable subsequence — saves 3 weeks, costs cards, archives and spheres); six brushes
instead of ten (a week of M5); a green S-8 for variant A (one fewer mandatory C2 primvar, no per-frame
republish path).

Grows: any C1–C5 change after its freeze — a migration across the schema, the publisher, the shader and
every authored asset, which is why the freezes exist; M1's stop condition firing (§5.1), ≈ 2 weeks to
re-plan plus a permanent tax on every later invalidation test; an OpenUSD version bump, where every
version-sensitive assumption lives in exactly one named T1 test, so the damage is visible in a day but
the fix is not free; Windows or macOS, out of scope for v1 (§8).

---

## 5. Stop conditions and fallbacks

### 5.1 SC-1 — M1 invalidation (the one hard stop)

**Signal.** Gate SI-2 red: a `usdGen:noise:magnitude` edit does not dirty exactly
`primvars/points/primvarValue` + `extent/*` on the affected tiles. **Fallback.** Move operator
parameters to `primvars:usdGen:*`, S10's stated prototyping fallback, which does produce notices without
an adapter. **Consequence.** C1 changes shape; part of the M1 adapter work is written off; **stop and
re-plan** — do not enter M2 (ADR §7; `design/proposal-risk.md` §9.2).

### 5.2 The rest of the register

| # | Signal | Fallback | Schedule effect |
|---|---|---|---|
| SC-2 `hairTangent` | S-8: variant A fails frame time or the `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` check | variant B becomes default and `hairTangent` (vertex vec3, object space) is republished every deforming frame at ≈ +1.5–2.5 ms at 100 k × 8 CV (**DERIVED from `EV-022`**, ADR §9 R42; S-8 produces the number); one extra mandatory C2 primvar and the loss of Storm's points fastpath | none — the decision lands before C2 freezes |
| SC-3 Storm material | L-1: `outputs:glslfx:surface` does not resolve first | flip `USDGEN_STORM_MATERIAL_OVERRIDE` ON, synthesize `material_storm` with `primOrigin`, bind tiles to it; the prim is added once when a session attaches, never during interaction, so S27's fixed prim set holds (the gap `design/judge-delivery.md` §4 item 3 flagged) | none |
| SC-4 refineLevel tier | S-9: a 2 → 1 switch costs ≥ 3 ms | no tumble tier; LOD is decimation by stable id — keep iff `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32`, `keepFraction = clamp(scale_groom · scale_description, 0, 1)` (ADR §9 R13; the salt is what keeps the surviving set uncorrelated with `hairId = UsdGenHash32(curveId, 0)/2^32`, R12) — the primary lever either way | none — M5 loses one control |
| SC-5 ragged buffers | E-1r > 2× uniform (M2) | the import path resamples to a uniform CV count by default; `UsdGenResample` is already v1 and the tool shows the measured penalty | none |
| SC-6 capture cost | E-4 > 300 ms at 1 M roots (M0/M3) | progressive capture plus the interactive LOD ceiling pulled from M8 into M3 | +1 wk on M3 |
| SC-7a memory, gate red | E-5 in 800 MB – 1.5 GB at 1 M with the default chain (M3/M7) | the eviction policy takes over: `USDGEN_MEMORY_BUDGET_MB` as a hard ceiling, lowest `recomputeCostMs/bytes` first, never the tail base or a live-override target (ADR §4.1). `09-…` §2.6's 770–860 MB projection lands in this band, so this is the expected path, not the exception | none |
| SC-7b memory, hard | E-5 > 1.5 GB at 1 M with the default chain | per-node caching becomes opt-*in* above 500 k curves (`design/proposal-performance.md` §13 **R9** — a register item, bare, never the render gate `R-3`'s family; `10-…` §8.4, where the hyphenated family is `R-1`…`R-3`); no new authored property — C1 is frozen and machine tuning is env/config (ADR §2.3, §9 R35) | +0.5 wk on M3 |
| SC-8 pinned expansion | per-sample pinned expansion under hdPrman — one extra copy of every vertex primvar per shutter sample (`research/G-hdprman-and-usdrecord-render-time-chain.md` §4) — costs more than emitting `nonperiodic` ourselves, measured by R-1 at M7/release | emit `nonperiodic` with duplicated end CVs in the render context behind a per-description flag, at the cost of the Storm pinned index path | +0.5 wk on M7 |
| SC-9 Metal/Vulkan | the `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` proxy fails and no Mac or Vulkan host exists by M5 | declare macOS out of scope for v1 **in writing** | none |
| SC-10 simulation creep | any request for collisions, wind or self-collision on a deadline | v3 (§6.3); usdGen consumes a sim cache through `UsdGenCurveSource` | none |
| **SC-11** engine floor (M1) | E-1 cannot reach **≤ 1.5 ms** for the 5-operator, 100 k × 8 CV chain in the private 8-thread arena, or E-7 falls below **3×** from 1 → 8 threads | stop and re-examine chunking **before any operator is added** — every later milestone assumes that floor. MEASURED baseline 1.02 ms at 8 threads and 3.90 → 1.02 ms = 3.8× (row `EV-008`; ADR §9 R27). `design/proposal-performance.md` §11.2's 2.5 ms is superseded by R27 and survives, if at all, as a regression ceiling | re-plan the engine chapter; M1 slips by the re-examination |
| **SC-12** chain order (M0, and every upgrade) | SI-5 fails — usdGen does not sort strictly after UsdSkel/RigExec/flattening and before every Storm/hdPrman plugin | nothing ships until it passes: the whole design assumes usdGen runs after every deformer (S1; ADR §1). The check is cheap and re-runs on every OpenUSD bump | blocking; no fallback exists |
| **SC-13** UsdSkel pickup (M2) | SI-9 red — the private `HdSiExtComputationPrimvarPruningSceneIndex` wrapper does not resolve `skinnedPoints` with `HD_ENABLE_SCENE_INDEX_EMULATION` off, or its per-frame cost on a production-density skinned scalp is unaffordable | execute the aggregator computation ourselves from `extComputation/inputValues` through the public `UsdSkelSkinPoints` kernel — `research/A2-usdrig-imaging.md` §8 option (a), spelled out in `05-static-curves-and-deformation.md` §4.2. Costs one CPU skinning pass per deforming frame | M2 re-plans around it |

SC-11 and SC-12 are the two additions `12-risks-decisions-open-questions.md` §2.1 requested, with the
meanings it gives them. SC-13 is the addition `05-static-curves-and-deformation.md` §4.2 and §9
requested; this register owns the ids and both documents cite them. `12-…` §5's reference-integrity
test requires every `SC-<n>` cited anywhere in `plan/` to exist here; with these three rows it does.

hdPrman does **not** reject `wrap = pinned`: `HdPrman_PinnedCurveExpandingSceneIndexPlugin` expands the
curves at phase 3, after motion blur (ibid. §4), so SC-8's trigger is the cost of that expansion, not a
rejection. What hdPrman does reject is `centripetalCatmullRom`, which S29 and ADR §5.3 already forbid.

---

## 6. Definition of done

### 6.1 v1 — the M0–M7 deliverable set (ADR §9 R38)

The v1 operator catalogue (`04-operators.md`): `UsdGenScatter` (`random` at M1, `atGuides` at M3,
`points` at M5), `UsdGenGrow`, `UsdGenGuideInterpolate`, `UsdGenCurveSource`, `UsdGenClump`,
`UsdGenNoise`, `UsdGenLength`, `UsdGenWidth`, `UsdGenSmooth`, `UsdGenResample`, `UsdGenDirection`,
`UsdGenScale` (M4), `UsdGenSculptLayer`, `UsdGenDeform`, `UsdGenFreeze`, `UsdGenInstance` (M6), the map
set (`UsdGenImageMap`, `UsdGenPtexMap` (M4), `UsdGenExprMap`, `UsdGenPaintMap`, `UsdGenNoiseMap`,
`UsdGenCombineMap`, `UsdGenGuideProximityMap`), `UsdGenMaskAPI`, `UsdGenLookAPI`, `UsdGenRestAPI`,
`UsdGenCurveAPI`; the ten brushes; motion profiles P0/P1/P2 (M7); Storm and hdPrman. ADR §6's v2 label
on `UsdGenInstance`, `UsdGenPtexMap` and motion profile P1 is superseded by ADR §9 R38; they are v1,
delivered in M6, M4 and M7.

### 6.2 v2 — M8

Done when every operator in §2.9's scope has a T0 value test and a T1 invalidation test, progressive
generation is enabled from the plugin or documented as requiring `--allow-async` (T-5), the in-place
overlay path is measured against the tile path (S-10), sculpt rebase is one undoable action, and every
M0–M7 gate is still green on the same commit. T-5 and S-10 are v2 criteria, never v1 release gates
(ADR §9 R38, R39). `UsdGenOpRegistry` stays internal; the non-C++ extension tier is `UsdGenExprOp`
(ADR §3).

### 6.3 v3 — after the first show

`UsdGenCollide`/Shrinkwrap, `UsdGenWind`/Force, `Smooth(neighbours)` as a two-pass gather/scatter node
on a per-frame grid, Braid, SimSource, an OpenExec backend behind the same operator interface (S16), a
third-party operator ABI, a GPU tail. Each needs a mechanism v1 does not have, so none may block a
release.

### 6.4 Release checklist

| # | Criterion |
|---|---|
| RC-1 | Every T0–T3 gate green on the release commit: B-1; E-1, E-1r, E-2 … E-8; SI-1 … SI-11; **S-1 … S-9, S-11, S-12**; L-1 … L-5; **T-EXPR-1, T-PTEX-1**; T-1, T-2, T-3, T-4; T-INST-1/2 — the whole of `09-…` §5.1–§5.4 except the T4 family and the two v2 ids. **T-5 and S-10 are v2 (M8) criteria (§6.2)**; S-11 is a record-only gate (no threshold) and is green when its number is recorded |
| RC-2 | T4 gates run on a workstation and signed off: R-1 (hdPrman parity — three motion samples produce correct blur with `k·tail`, not `k·chain`, evaluations), R-2 (MSAA / OIT on 1-px strands at 1080p and 4K), R-3 (`HDST_ENABLE_HGI_RESOURCE_GENERATION=1` compiles with zero errors, identical goldens) |
| RC-3 | The B1–B12 Storm benchmark set — the tier-T2 protocol of `research/G-storm-throughput-and-prim-granularity.md` §3.5, shipped as the `benchUsdGenStorm` sweep of `09-…` §4.1 and §4.4 — run and recorded in `appendix-A-evidence-ledger.md`, each number tagged with its host. It is a T2 measurement set, not the T4 workstation protocol of `10-…` §5.2 |
| RC-4 | A comb stroke on a 100 k-CV groom holds ≥ 30 fps end to end at 1080p on the workstation (moved here from M5 because it is a T4 measurement) |
| RC-5 | C1, C2, C3 and C5 recorded with their frozen values and guarded by `testUsdGenContracts`; C4 guarded by `testUsdGenAbi` against the nineteen entry points of `08-tools.md` §1.4 (§0.2) |
| RC-6 | `USDGEN_ENABLE=0` returns the input scene index unchanged; `USDGEN_WITH_RIGEXEC=OFF` builds and passes every non-rig test |
| RC-7 | `usdrecord --renderer GL` **and a real `usdrecord -r "RenderMan RIS"`** render all three canonical examples with no usdview and no C API involved (the S8 property). The RenderMan half needs an install and is therefore T4; the GL half is the M7 exit (§2.8) |
| RC-8 | `uniform int usdGen:schemaVersion = 1` refuse-and-warn verified against a stage authored with version 2; `usdGen:algorithmVersion` present on every operator type |
| RC-9 | No third-party `.so` and no third-party header installed beside USD; the offline build from `thirdparty/` reproduced on a clean machine |
| RC-10 | The four S46 bugs filed upstream with reproducers (`pxr/exec/esfUsd/stageData.cpp:361`; usdRig's bare-`primvars` dirty; `testUsdviewRigExec.py` fixture drift; `_env.sh` Linux gaps), plus the `geomSubset` adapter locator asymmetry (`12-…` §0.2 S46, extended) |
| RC-11 | Documentation published: the three canonical `.usda` workflows — (a) generate-and-style, (b) frozen-and-deform, (c) cards — the operator reference, the env-var list **as published in `10-build-dependencies-testing.md` §3.5**, which must carry every name of ADR §9 R35 (`USDGEN_ENABLE`, `USDGEN_CONTEXT`, `USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`, `USDGEN_MEMORY_BUDGET_MB`, `USDGEN_STORM_MATERIAL_OVERRIDE`, `USDGEN_DIAGNOSTICS`, `USDGEN_IMAGE_CACHE_MB`, `USDGEN_PTEX_CACHE_MB`, `USDGEN_PTEX_MAX_FILES`) plus `USDGEN_IMAGING_DLL`, `USDGEN_XVFB_ROOT`, `USDGEN_DISPLAY`; and the workstation protocol |

---

## 7. Testing

This document is a schedule, so what proves it is coverage: every milestone has an exit gate set (§2.0),
and every gate has a tier, threshold and milestone owned by `09-performance-and-benchmarks.md` §5 and a
test binary owned by `10-build-dependencies-testing.md`.

| Tier | Suite | Gates it carries | When |
|---|---|---|---|
| T0 engine and build | `testUsdGenMath`, `testUsdGenKernelDeterminism`, `testUsdGenGraph`, `testUsdGenOps`, `benchUsdGenChain`, `benchUsdGenSparse`, `benchUsdGenKnn`, `benchUsdGenMemory`, `testUsdGenExpr`, `testUsdGenPtex`, `testUsdGenLinkRule_*` — the spellings of `10-…` §5.6 | B-1; E-1, E-1r, E-2 … E-8; L-3, L-4 (T0 half); T-EXPR-1, T-PTEX-1 | every commit |
| T1 scene index | headless over the real `UsdImagingCreateSceneIndices` chain with a recording observer | SI-1 … SI-11; T-2; T-INST-1/2; L-4 (T1 half), L-5; `testUsdGenContracts`, `testUsdGenAbi`, `testUsdGenSkelInterop`, `testUsdGenFrozenReentry`, `testUsdGenStageEdits` | every commit — the primary regression suite |
| T2 Storm | the EGL device-platform harness on the GB10, driver `benchUsdGenStorm` (`09-…` §4.1) | S-1 … S-12 (S-10 at M8, S-11 record-only at M7, S-12 at M1); L-1, L-2; golden-image demos (not gate ids) | every commit (correctness), nightly (timing) |
| T3 app | `testusdview` and `usdrecord --renderer GL` under Xvfb `:77`, llvmpipe — **CPU numbers, never quoted as Storm numbers** | T-1, T-3, T-4, T-5 | pre-merge |
| T4 workstation | `docs/workstation-protocol.md` §§1–11, written at M0, run by the M7 T4-protocol runner (`10-…` §5.2) | R-1, R-2, R-3, RC-4 | release only |

* **`testUsdGenContracts` (T1)** holds the C1 property-name list (every `UsdGenMaskAPI` property
  included), the C2 primvar list with interpolations, the C3 primvar list and the C5 glslfx `inputs:`
  names as literal data, and fails on any rename after the relevant freeze. **`testUsdGenAbi` (T1,
  `10-…` §5.6)** does the same for C4's **nineteen** `UsdGenImaging_*` entry points — ADR §9 R31's
  eighteen minimum plus `UsdGenImaging_GetLastError`, the list `08-tools.md` §1.4 owns. It is T1
  because it loads `libusdGenImaging.so`. Together they make §0.2 real; `10-…` §5.6 registers both.
* **The milestone gate manifest (CI)**: each milestone tag carries the manifest of its exit gates from
  §2.0, and the release job refuses to tag if a listed gate has no green run on that commit. It keys on
  gate ids, which is why a vertical demo is tracked by milestone tag and never given one — the failure
  `design/judge-delivery.md` §4 item 4 named, a T4 gate quietly waived under pressure.
* **The pre-work ledger**: each of PW-1…PW-6 (§1.1) has a recorded result and a decision line in
  `appendix-A-evidence-ledger.md`; a check whose decision line is missing is a red build, not a TODO.
  PW-7…PW-13 (§1.2) are harness work and carry no decision line — each is done when the gate it feeds
  has a driver that runs.

Every operator ships with both a T0 value test and a T1 test asserting the *notice*, because a
pull-based test structurally cannot catch a missing dirty.

---

## 8. Out of scope

This document does not specify the maths of any operator (`04-operators.md`), the schema text
(`02-schema.md`), the engine internals (`03-execution-engine.md`), the CMake
(`10-build-dependencies-testing.md`), the gate thresholds and their derivation
(`09-performance-and-benchmarks.md` §5, the registry this document cites) or the full risk register
(`12-risks-decisions-open-questions.md`) — it references them.

Out of scope for the plan itself: cost, hiring and vendor selection; Windows and macOS builds beyond
keeping the CMake honest and the SC-9 stop condition; hair simulation and self-collision; XPD/Alembic
import (converted `BasisCurves` enter through `UsdGenCurveSource`); Ptex sampling inside Storm; a
node-graph authoring UI; hdGp hosting of third-party procedurals; GPU evaluation.

---

## 9. Sources

| Source | Sections used |
|---|---|
| `design/adr-v1.md` | §1 (S1/S11/S18(c)/S23/S24/S29/S31/S36); §2.1–§2.3; §3; §4.1–§4.5; §5.1–§5.6; §6; §7; §8; **§9 R1–R45**, in particular R1, R4–R7, R10, R12–R13, R15, R19, R21, R24, R27, R29, R31–R32, R35, R37–R43 |
| `design/brief-v1.md` | §1 R1–R9; §2 S1–S46, cited inline |
| `design/proposal-risk.md` | §1, §2, §3.11, §6.1, §9.1–§9.3, §10–§11 |
| `design/proposal-performance.md` | §5.6, §11.1–§11.2, §12, §13, appendix |
| `design/proposal-artist.md` | §8.2, §8.3 (brushes, four-phase loop) |
| `design/judge-delivery.md` | §4 items 3–5, §4.1, §5 item 6, §7 (re-baseline, clause (a) T4 rule) |
| `design/judge-evidence.md`, `design/judge-artist.md` | §0 (S1 ordering tag, S36 render context); the interpolated-S-1 finding |
| `research/G-chain-order-probe.md` | §2–§3, §4b–4c, §5 |
| `research/G-data-plane-engine-prototype-benchmark.md` | §3.3, §4, §5, §8 |
| `research/G-evaluation-scheduling-and-batching.md` | §5–§7, §9 |
| `research/G-storm-hair-look-prototype.md` | §5, §6 |
| `research/G-storm-throughput-and-prim-granularity.md` | §1.4, §1.8, §2, §3.5 |
| `research/G-freeze-bake-undo-and-frozen-reentry.md` | §1.1, §3, §4.1, §4.3 |
| `research/G-tool-loop-array-transport-and-cv-picking.md` | §1.4, §1.5, §2.4, §5 |
| `research/G-instancing-cards-archives-and-native-instances.md` | §2, §6, §9 |
| `research/G-motion-blur-sampling-strategy.md`, `research/G-hdprman-and-usdrecord-render-time-chain.md` | §5, §7; §3, §4 |
| `research/G-stage-free-parameter-and-time-transport.md` | §1–§4 |
| `research/A3` §6, `research/A7` §9, `research/A8` §5–§6, `research/ENVIRONMENT.md` | conventions; operator catalogue; nanoflann (README-sourced, UNVERIFIED); host facts and the CORRECTIONS block |
| `appendix-A-evidence-ledger.md` | §2.0 (the retired-handle map); §2.1 (`EV-001`, `EV-002`, `EV-004`, `EV-006`, `EV-007`, `EV-008`, `EV-009`), §2.3 (`EV-019`–`EV-022`), §2.4 (`EV-028`, `EV-029`), §2.6 (`EV-045`, `EV-047`, `EV-050`, `EV-051`), §2.7 (`EV-061`, `EV-064`, `EV-065`), §2.8 (`EV-067`, `EV-070`), §2.9 (`EV-071`–`EV-075`), §2.12 (`EV-081`), §3.10 (the `esfUsd` resync defect) |
| `prototypes/` | `chain-order`, `data-plane-benchmark`, `evaluation-scheduling`, `freeze-bake`, `instancing`, `motion-blur`, `stage-free-transport`, `storm-hair-look`, `storm-throughput`, `thirdparty-bench`, `tool-loop`, `usdrig-linux-build` |
| OpenUSD 26.08 | `pxr/imaging/hdSt/renderDelegate.cpp:695-707`; `pxr/exec/esfUsd/stageData.cpp:361`; `pxr/imaging/hdSt/basisCurves.cpp:930-935`; `pxr/usd/usd/schemaRegistry.cpp:926-940`; `pxr/imaging/hd/sceneGlobalsSchema.h:37-47`; `pxr/usdImaging/usdImagingGL/engine.cpp:148-201` |
