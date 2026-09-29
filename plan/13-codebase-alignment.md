# usdGen — Codebase alignment (plan v2 overlay)

Date: 2026-09-11. Status: **binding overlay on plan v1**.

This document re-baselines the plan against the built codebase. Plan v1
(`00`–`12`, `design/`, 2026-09-04/05) was written before M0 was built; the
repository has since moved through M0 into late M1 with M2 work started
(git log: `M0: skeleton…`, `M1 wip: Storm stall fix…33/33 ctest`; gate
census `.omp/gate-status.md`, 2026-09-09). Where this document and a v1
document disagree, **this document wins**; the superseded v1 claims are
listed in §6 so the old text stays readable as history. Nothing here changes
a frozen contract: C1/C2/C5 stay frozen exactly as `docs/freezes/C1.md`,
`C2.md`, `C5.md` record them.

It also locks in the three structural guarantees the re-baseline is built
around:

* **§2 — every operator is an OpenUSD prim.** Already true in code; made
  normative here with a per-type kernel-status table and a prim-first rule.
* **§3 — USD relationships form the evaluation graph at Hydra
  runtime/rendertime.** True for graph *timing* (assembly runs on the Hydra
  commit path), **not yet true for graph *sourcing*** (§3.2, debt D1): the
  builder reads a `UsdStage`, not Hydra data sources. §3.3 mandates the
  Hydra-sourced builder and its migration.
* **§7 — all sourcing follows the primAdapter architecture.** Every value
  entering the graph arrives as a Hydra data source published through a
  registered UsdImaging prim/API adapter. This is the general rule of which
  §3's graph-edge migration is one instance.

Reads with: `01-architecture.md` (the pipeline §3 extends),
`02-schema.md` (the prim/relationship registry §2–§3 presuppose),
`06-imaging.md` (the scene-index contract §3 migrates),
`11-roadmap.md` (the milestones §4 re-baselines),
`09-performance-and-benchmarks.md` §5 (the gate registry; statuses quoted
from `.omp/gate-status.md`).

---

## 1. Ground truth (verified 2026-09-11)

| Subsystem | Plan v1 said | Code actually has |
|---|---|---|
| Schema domain | Codeless, full v1–v3 namespace declared at M1 | Done: `libs/usdGenSchema/schema.usda` (966 lines, `skipCodeGeneration`), checked-in `plugin/usdGenSchema/resources/{generatedSchema.usda,plugInfo.json}` (40 types), C1 frozen (`docs/freezes/C1.md`; `testUsdGenContracts` + `tests/checks/c1_registry_check.py` green) |
| Engine core | M1: graph/compiler/scheduler/session/store + 5 ops | Done **plus M2 head-start**: `libs/usdGen/usdGen/{graph,compiler,scheduler,session,generationStore,graphDesc,op}.{h,cpp}`, **7 kernels registered** (`opRegistry.cpp:78-88`: Scatter/Grow/Noise/Length/Width + CurveSource/Deform) under `libs/usdGen/usdGen/ops/`; `usdGenMath` (hash/ramp/kernels, `-ffp-contract=off`) |
| Operator adapters | 1 base + 5 entries + RestAPI adapter | Done: `primAdapter.{h,cpp}` (generic `GetPropertyNames()` mappings, attribute + relationship mappings, single-target `terminal`/`frozen:curves`), `restApiSchemaAdapter.cpp`, plugInfo entries in `plugin/usdGenImaging/resources/plugInfo.json.in` |
| Scene index | Groom SI, dirty router, tile publisher, sessions, triggers (a)/(b)/(c) | Done in structure: `groomSceneIndexPlugin.cpp` (968 lines: adopt/resolve/commit/republish), `usdGenDirtyRouter.{h,cpp}`, `usdGenTilePublisher.{h,cpp}`, `usdGenImagingSession.{h,cpp}`, `usdGenEngineBridge.cpp`, pruning wrapper `_pruned` (`groomSceneIndexPlugin.cpp:114`) |
| Tiles / C2 | 32–256 `basisCurves` tiles, `__usdGenRender` | Done: C2 frozen (`docs/freezes/C2.md`), `testUsdGenTileContract` (SI-1) PASS |
| Shaders / C5 | 3 glslfx + MaterialX, variant-A default | Done: `usdGenShaders/resources/shaders/` (Preview/Translucent/Primvar + `shaderDefs.usda` + `usdGenHair.mtlx`), C5 frozen, S-8 → variant A decided, L-1/L-2 green (latest commit) |
| Gates (M1 exit, 21) | All green at M1 exit | 16 PASS + S-9 decided (no tumble tier) per `.omp/gate-status.md` + S-1 PASS 2026-09-12 (0.82 ms); open: **E-1 FAIL** (1.69–1.94 ms vs ≤ 1.5 ms), **S-5 ruling** (2/2 measured, hair in 1 batch), **S-6/S-12** (bench modes unrun) |
| Vendored third party | SeExpr-noise M0, interpreter + Ptex M4 | Partial: `thirdparty/{nanoflann.hpp,seexpr/SeExpr2,ptex(empty)}`; `exprEval.{h,cpp}` exists but is **untracked work-in-progress** (`git status`: `?? libs/usdGen/usdGen/exprEval.*`); no `benchUsdGenKnn`/`benchUsdGenMaps` yet |
| Tools (C ABI, M5) | 19-entry C ABI + `_usdGen` + `usdgen` package + brushes | **Not started**: `cApi.h` declares all 19 entries, **no `cApi.cpp` implements them**; `python/usdgen/` empty; `tools/fur_gen.py` is an unrelated PointInstancer demo |
| Freezes | C1/C2/C5 at M1, C3 at M2, C4 at M5 | C1/C2/C5 frozen on disk; **no `docs/freezes/C3.md`/`C4.md`** (C3/C4 unfrozen, correctly) |
| Docs/status text | — | **Stale**: root `README.md` says "Status: M0 (skeleton)"; `CMakeLists.txt:465` says tests are "M1-STUB mains" — no `M1-STUB` string remains anywhere under `tests/` or `libs/` |
| Tonic authoring tool (overlay `17-tonic-authoring-tool.md`, plan/17 §10) | Not started | **Implemented**: `17-tonic-authoring-tool.md` phases P0–P6 and the `18-tonic-viewport-tool.md` viewport overlay (V0–V7) are both built and dated 2026-09-19 (see plan/18 §8's Delivered table); `libs/usdGenTonic/`, `plugin/usdGenTonicTools/`, `plugin/usdGenTonic/`, `docs/tonic-tool.md`. Tubes stay authoring-side only, per the overlay's contract; no usdGen kernel reads them |

---

## 2. All operators are OpenUSD prims (binding)

**V2-1. The prim is the operator.** Every grooming operator exists first as
a codeless typed prim under `<Groom>/<Description>/Ops/` deriving (directly
or via `Generator`/`Styler`/`Deformer`) from abstract `UsdGenOperator`
(`libs/usdGenSchema/schema.usda:111-128`). There is no out-of-band operator
registry visible to artists, tools, or Hydra: if it has no prim type, it is
not an operator. `02-schema.md` remains the normative property registry;
C1 stays frozen.

**V2-2. Prim-first, kernel-later.** A new operator lands in this order, and
in this order only:

1. prim type + properties in `schema.usda`, resources regenerated via
   `bin/gen_schema.sh`, C1 checker extended;
2. adapter coverage for free via `includeDerivedPrimTypes` (no adapter edit
   for a new `UsdGenOperator` subtype — S10, already implemented);
3. compiler/dirty-router rows (`02` §6 classification for the new type's
   parameters);
4. engine kernel + `opRegistry` registration.

Between steps 1 and 4 the type is *declared but unimplemented*: it publishes
nothing and raises one `TF_WARN` per prim (`02` §8.2). That is the designed
state, not breakage.

**V2-3. Kernel-status table (normative until a kernel lands).**

| Prim type | Kernel | Notes |
|---|---|---|
| `UsdGenScatter` | **partial** (`ops/scatter.cpp:1`: `mode="random"` only; other modes compile-error, `scatter.cpp:117-118`) | `atGuides` (M3) / `points` (M5) / `uniform` (M8) pending |
| `UsdGenGrow`, `UsdGenNoise`, `UsdGenLength` | **yes** | M1 set, complete |
| `UsdGenWidth` | **yes — root→tip profile guaranteed** | Constant `width`, `width:knots` ramp over hairT, `taper`/`taperStart`, `rootScale`/`tipScale`, set-or-multiply — value-pinned by T0 `testUsdGenWidth` (7 checks). 2026-09-12 fix: the reader used `width:knots:interpolation`, matching no C1 property, so every ramp silently built catmullRom; now reads C1 `width:interpolation` (also added to the op's value params). SeExpr-driven width flows through `UsdGenExprOp` `returnType = width` (M8, per-CV over hairT) / `UsdGenExprMap` (M4), baked to `widths` at capture (S37) — **no C1 change**: the Width op stays the combiner they bake into. Expression determinism incl. a width case is gate L-3 |
| `UsdGenCurveSource`, `UsdGenDeform` | **yes** | M2 head-start; ragged path covered (`tests/testUsdGenCurveRagged.cpp`, `testUsdGenRaggedTopo.cpp`) |
| `UsdGenGuideInterpolate`, `UsdGenClump`, `UsdGenSmooth` (along-curve), `UsdGenResample`, `UsdGenDirection` | **no** | M3 scope (§4) |
| `UsdGenScale`, all `UsdGen*Map` types | **no** | M4 scope; `NoiseMap`'s SeExpr-noise implementation is the single noise impl (S38) |
| `UsdGenSculptLayer`, `UsdGenFreeze` | **no** | M2 remainder (§4); `Freeze` stays a chain cap, never a geometry type (R3/R9) |
| `UsdGenInstance` | **no** | M6 scope |
| `UsdGenCurl/Bend/Straighten/Displace/Wave/Part/ExprOp`, `Scatter.uniform` | **no** | M8 scope |
| `UsdGenCollide/Wind/Braid/SimSource`, `Smooth.neighbours` | **no** | v3, needs unbuilt machinery (`04` §1.4) |

**V2-4. Masks are prims too.** The `UsdGenMaskAPI` block stays auto-applied
to `UsdGenOperator` (SI-8 PASS); mask *sources* (`mask:source`,
`mask:region`) reference `UsdGenMap` prims, never inline arrays.

---

## 3. Relationships form the graph at Hydra runtime (binding)

**V2-5. The graph has exactly one authoring language: USD relationships.**
The edge inventory is closed and frozen by C1:

| Relationship | Role in the graph | Engine field |
|---|---|---|
| `usdGen:input` (ordered, multi-target) | the **only** execution edges; Kahn order + namespace tie-break (S26) | `UsdGenNodeDesc::inputs` |
| `usdGen:terminal` (exactly one target) | graph root; multi-target is a compile error | `UsdGenGraphDesc::terminal` |
| `usdGen:guides` / `usdGen:curves` / `usdGen:frozen:curves` | reference-lane / source curve sets (I3) | `UsdGenNodeDesc::curves` → `curveSets` |
| `usdGen:surface` (+ Description-level inheritance) | surface binding, Mesh or GeomSubset (R15) | `UsdGenNodeDesc::surfaces` |
| `usdGen:clump:centers` | operator-as-guide retarget (reference role) | `references` |
| `usdGen:mask:source`, `usdGen:length:source`, `usdGen:map`, `usdGen:expr:maps`, `usdGen:combine:inputs`, … | map/field wiring (never `usdGen:input`; maps are not operators) | `maps` |
| `usdGen:prototypes`, `usdGen:colliders`, `usdGen:part:curves`, `usdGen:direction:source`, `usdGen:paint:surface`, `usdGen:look:colorMap` | reserved future edges, same transport | per-milestone fields |

Cross-Description edges stay compile errors in v1 (`04` §0.10).

**V2-6. Relationships reach Hydra as data sources, today.** The adapter
already publishes every relationship into the `usdGen` container
(`primAdapter.cpp:196-206`: single-target `terminal`/`frozen:curves` via
`GetPathFromRelationshipDataSourceFactory`, all others via
`GetPathArrayFromRelationshipDataSourceFactory`). The dirty router already
routes Hydra notices to engine dirty bits (`usdGenDirtyRouter.h`). The
commit triggers (a)/(b)/(c) already run at Hydra runtime on the commit
thread (`groomSceneIndexPlugin.cpp`, `usdGenImagingSession.cpp`). So graph
*timing* is at Hydra runtime; only graph *sourcing* is not:

### 3.2 Debt D1 — the builder reads a stage, not Hydra (S8 violation)

`BuildGraphDesc(UsdStageRefPtr, SdfPath)` (`usdGenGraphDescBuilder.{h,cpp}`)
walks `UsdPrimRange`, `UsdAttribute::Get`, and `UsdRelationship::GetTargets`
— i.e. the graph is sourced from the stage, in direct violation of S8
("no `UsdStage` downstream of the stage scene index"). Consequences in-tree:

* `_ResolveStage` is a stub returning null (`groomSceneIndexPlugin.cpp:245-271`);
  staging depends on the `SetStage`/`FindGroomStage` weak-stage side channel
  (`usdGenImagingSession.cpp:262-292`, fed from `primAdapter.cpp:110`);
* `testUsdGenStormSurgery` is PENDING-FIX on this wiring (`.omp/gate-status.md`
  §"Non-registry item");
* any host without a reachable stage (batch render delegates, record
  pipelines) cannot stage a description.

### 3.3 Mandate: Hydra-sourced graph assembly (M2 exit)

**V2-7.** Before M2 exits, `UsdGenGroomSceneIndex::_CommitNow` stages
descriptions from **its input scene index only**:

1. New `BuildGraphDescFromHydra(HdSceneIndexBase&, SdfPath description,
   double time)` beside the stage builder (which stays for T0/offline use
   and tests). Operator discovery follows `usdGen/input` **path-array data
   sources** back from the `usdGen/terminal` target; parameters are read from
   `usdGen/<…>` sampled data sources (S14 pull-all preserved: every mapped
   locator is pulled at least once per topology generation so dependencies
   register); curve sets and surfaces resolve through the existing private
   pruning wrapper (`_pruned`) so deformed points, rest points
   (`usdGen/rest/*`), and GeomSubset indices arrive post-flattening (S3/S4).
2. `_ResolveStage` + the weak-stage registry + `SetStage` are deleted; the
   session key drops its stage member for the Hydra path (sessionId fallback
   already exists, S15).
3. `testUsdGenStormSurgery` is rewritten against the Hydra path and either
   promoted into the gate registry (as an SI-9/SI-10 companion) or deleted —
   it must not linger as an unregistered PENDING-FIX.
4. `usdGen:sessionId` stops being "hardcoded-empty" (`groomSceneIndexPlugin.cpp:244`).

Until then, D1 is contained: the stage path stages the same `UsdGenGraphDesc`
value type the Hydra path will fill, so engine, compiler, router, and
publisher need no changes — the migration is confined to
`usdGenImaging/{usdGenGraphDescBuilder,groomSceneIndexPlugin,primAdapter,
usdGenImagingSession}.*`.

---

## 4. Milestone re-baseline

| Milestone | v1 said | v2 status |
|---|---|---|
| M0 Skeleton | future work (`11` §2.1) | **DONE** (B-1, chain order, tiers, C1 groundwork; commit `831c869`) |
| M1 Straight hair | future work, 5 ops | **NEAR-EXIT**: 16/21 gates PASS (S-1 green 2026-09-12: 0.82 ms), S-9 decided; 7 kernels live (M1 five + M2 head-start); C1/C2/C5 frozen. First generated groom renders in Storm (itemsDrawn=50, all tiles sync refined). Open: **E-1** (perf), **S-5 ruling** (2/2 measured, hair in 1 batch), **S-6/S-12** (run `benchUsdGenStorm` modes), then tag M1 |
| M2 Deform + freeze | future work | **IN PROGRESS**: CurveSource + Deform kernels done; remaining: SculptLayer + Freeze kernels, primAdapter-sourced staging (**§7**, V2-8–V2-11, replaces the stage path), SI-9/SI-10/SI-12/SI-13/E-1r/E-3/B-2, C3 freeze (`docs/freezes/C3.md`) |
| M3 Guides + clumps | future work | **NOT STARTED**: GuideInterpolate/Clump/Smooth-along/Resample/Direction kernels, reference lane, `benchUsdGenKnn` (E-4), E-5 memory work |
| M4 Maps + expressions | future work | **NOT STARTED**: finish `exprEval`, Ptex wiring (`thirdparty/ptex` empty), 7 map prims, Look bake, ReloadMaps, Scale. `thirdparty/seexpr/SeExpr2` present, uninterfaced |
| M5 Tools | future work | **NOT STARTED**: implement `cApi.cpp` (19 entries), `_usdGen` module, `usdgen` package, 10 brushes, C4 freeze |
| M6 Instancing | future work | **NOT STARTED** |
| M7 Render time | future work | **NOT STARTED** (P1/P2, eviction, headers, hdPrman parity) |
| M8 Breadth | continuous | **NOT STARTED** |

Estimates in `11` §4 are untouched (still ASSUMPTION); only the start line
moves: M0 is sunk, M1 is its open gates, M2 is §4's remainder + §7.

---

## 5. Debt register (new items; v1 SC/Q items unchanged)

| # | Debt | Owner doc | Due |
|---|---|---|---|
| D1 | Stage-sourced staging (S8 violation) → primAdapter-sourced staging (§7, V2-8–V2-11) | §3.3, §7, `06-imaging.md` | **M2 exit** |
| D2 | Stale status text (root README "M0", CMakeLists "M1-STUB" comments, `docs/review/*` stub-era notes are history) | this §6 | with this overlay |
| D3 | `cApi.h` header-only; `python/usdgen/` empty; `tools/fur_gen.py` unrelated | `08-tools.md` | M5; until then the header's "single source of truth" note must say *declared, not implemented* |
| D4 | `exprEval.{h,cpp}` untracked WIP (SeExpr2 engine headers unvendored — the TU cannot compile); `thirdparty/ptex` empty | `07-look-maps-expressions.md` | M4; contained since 2026-09-12 by a CMake exclusion guard (`REMOVE_ITEM exprEval.cpp`, revert when wiring M4). Left in-tree (not deleted) for the expression lane; its only consumer is the likewise-unwired `tests/testUsdGenExpr.cpp` |
| D5 | `testUsdGenStormSurgery` unregistered PENDING-FIX | §3.3 | M2 exit (register or delete) |

---

## 6. What this overlay supersedes

* `plan/README.md` §3 ("Status: plan v1 (draft, pending review)") →
  **plan v2 (accepted overlay 13 on v1 history)**; `00`–`12` remain normative
  except the rows below.
* `README.md` "Status: M0 (skeleton) … operator engine … land in M1 and
  later" → §1/§4 of this document (engine + publisher + shaders landed).
* `11-roadmap.md` §2.1–§2.2 as schedule (M0/M1 future work) → §4 of this
  document. Milestone *contents* in `11` §2.3–§2.9 stand.
* `01-architecture.md` §3 "stage-free data flow" as *implemented* → §3.2:
  the boundary design stands, the builder implementation is debt D1 with the
  §7 migration.
* `03-execution-engine.md` / `11` §2.2 "M1 ships five operators" → §1: M1
  ships **seven registered kernels** (CurveSource + Deform early).
* `08-tools.md` §1.4 as *implemented* C ABI → D3: declared, not implemented.
* V2-7 items 1–2 (Hydra builder sketch) → **V2-11** (full primAdapter
  homing table + acceptance). V2-7 items 3–4 stand as restated there.
* `09-performance-and-benchmarks.md` §5 (registry: +B-2, +SI-12, +SI-13;
  SI-7 self-extends per V2-10; M2 exits in §5.5), `09` §8 testing tables
  (`testUsdGenStagingRule`, `testUsdGenHydraParity`),
  `10-build-dependencies-testing.md` §6.5 (three new rows; "fifty" →
  "fifty-three"), `11-roadmap.md` §2.0 (M2 exits + minted-ids paragraph),
  `plan/README.md` §1 ("fifty" → "fifty-three") → **§8 of this document**,
  which maps each cared-about concern to the gates that prove it.
* Registry addition (`plan/README.md` §3): **this document owns
  plan-vs-codebase alignment** (ground-truth table, kernel-status table V2-3,
  milestone re-baseline §4, debt register §5, sourcing mandate §7).

---

## 7. All sourcing follows the primAdapter architecture (binding)

**V2-8. The single sourcing rule.** Every value that enters
`UsdGenGraphDesc` — operator parameters, graph edges, surfaces, curve sets,
maps, look, publication policy — arrives as a Hydra data source published
through a registered `UsdImagingSceneIndexPrimAdapter` /
`UsdImagingAPISchemaAdapter` and read from the groom index's input scene
index (through the private pruning wrapper `_pruned` wherever
ext-computation primvars may exist, S3). The staging path touches no
`UsdStage`, `UsdPrim`, `UsdAttribute`, `UsdRelationship`, `UsdGeom*`, or
`UsdShade*` API. The allowed reader surface is
`HdSceneIndexBase::GetPrim` / `GetChildPrimPaths`, the `Hd*DataSource`
getters, and the `UsdImagingDataSourceMapped` relationship/path factories
the adapters themselves use. B-1 already forbids `usd`/`hd` linkage in the
engine; this rule is its imaging-side twin: **stage APIs stop at the adapter
boundary**.

Why adapters, and only adapters: they are the one place that (a) resolves
asset paths stage-free (S13), (b) registers time-varying/asset dependencies
completely via S14 pull-all (`06` §2.5 — a consumer that never pulls never
gets dirties), (c) emits precise dirty locators (`06` §2.7, the dirty
router's input language), and (d) sees post-flattening, post-skinning values
(S4/S5). Any reader that bypasses them re-pays all four costs and silently
diverges. D1 is the receipt.

**V2-9. Per-source homing table (normative).** "Adapter" names the registered
adapter that publishes the value; "Locator" names what the Hydra builder
reads; "Replaces" names the stage read being deleted (all in
`usdGenGraphDescBuilder.cpp`).

| `GraphDesc` field | Adapter | Hydra locator(s) read | Replaces stage read |
|---|---|---|---|
| node discovery + `inputs` | `UsdGenOperator` (+derived), `UsdGenDescription` | `usdGen/input` path-array data source back from the `usdGen/terminal` target; namespace order from `GetChildPrimPaths` (S26 tie-break) | `_GraphWalker` over `UsdRelationship` (`:185-216, :365-368`) |
| `terminal` | `UsdGenDescription` | `usdGen/terminal` single-path data source, exactly-one-target enforced | `descPrim.GetRelationship("usdGen:terminal")` (`:347-363`) |
| node params, S14 pull-all | per-type `Mappings()` (`primAdapter.cpp:151-217`) | every sampled data source under the `usdGen` container except dedicated fields, pulled at least once per topology generation | `_PullParams` over `GetPropertyNames` + `GetAttribute` (`:153-180`) and the dedicated `_GetTyped` reads (`:380-392`) |
| `references` / `curves` (`guides`, `curves`, `frozen:curves`, `clump:centers`) | `UsdGenOperator` | `usdGen/guides`, `usdGen/curves`, `usdGen/frozen/curves` (single-target), `usdGen/clump/centers` path(-array) data sources | relationship-bucket switch (`:394-422`) |
| `surfaces` (+ Description inheritance, GeomSubset → parent) | `UsdGenOperator`, `UsdGenDescription` | `usdGen/surface` path-array data source | `GetRelationship("usdGen:surface")` (`:432-444, :515-523`) |
| surface geometry (topology, points, uv, velocities, world matrix) | **stock** UsdImaging Mesh/GeomSubset adapters via `_pruned` | `primvars/points`, topology data sources, `primvars:st`, `xform/matrix` (+ `resetXformStack`), subset `indices` (R15: parent-face indices) | `_BuildSurface` `UsdGeom` reads (`:221-261`) |
| rest geometry | `UsdGenRestAPI` | `usdGen/rest/points`, `…/faceVertexCounts`, `…/faceVertexIndices`, `…/st` at `Default` (S12; `usdGenRestApiDataSource.cpp:128-164`) | `_GetPrimvarTyped("rest")` + Default-points fallback (`:246-253`) |
| C3 curve sets (counts, points, widths + `rest`, `skinprim`, `curveId`, `skinprimuv`, `rootFrame`, `role`, `frozenEpoch`, `blend`) | **stock** BasisCurves adapter (+ RestAPI where applied) | the curve prim's `primvars/*` and topology data sources at sample time, rest at `Default` | `_BuildCurveSet` (`:264-320`) |
| maps + resolved assets | `UsdGenMap` (+derived) | `usdGen/source` asset-path data source **as the adapter resolves it** — the resolved path travels, never the raw string (S13) | `SdfAssetPath` attribute read + `GetResolvedPath` in the builder (`:500-508`) |
| Description policy (`densityScale`, `tileTarget`, motion, `curve:basis`, `pickTarget`, `LookAPI` block) | `UsdGenDescription`, `LookAPI` | `usdGen/densityScale`, `usdGen/look/…`, … sampled data sources | `_GetTyped(descPrim.GetAttribute(…))` block (`:537-571`) |
| publication context (`purpose`, `visibility`, material binding, description xform) | upstream indices (binding/flattening already applied, S4) | the input prim's resolved `materialBinding`, `xform`, intent data sources | `ComputeBoundMaterial`, `ComputeLocalToWorldTransform`, token reads (`:573-586`) |

Surfaces and curve sets deliberately name **stock** adapters, not new usdGen
ones: mesh points, BasisCurves topology, and subset indices are not usdGen
concepts, and re-publishing them would fork the invalidation contract every
other consumer relies on. usdGen adapters publish only what usdGen owns: the
`usdGen` container and `usdGen/rest/*`.

**V2-9a. Stage reads must reproduce adapter absence semantics.** Hydra data
sources carry only AUTHORED opinions — an unauthored property is simply
absent — but `UsdAttribute::Get` on the stage returns schema FALLBACKS. Any
stage-side builder read whose fallback differs from the desired absent-value
must gate on `HasAuthoredValueOpinion()`. The known case is `purpose`: the
schema fallback is `"default"`, which is render-tag poison (tiles match no
collection, Storm never syncs them — 2026-09-12: 49 published tiles,
itemsDrawn == 1, zero sync lines — while `visibility`'s `"inherited"`
fallback is valid Hydra and stays). `densityScale`/`tileTarget`/motion/look
fallbacks coincide with their C1 defaults and are harmless. The Hydra-source
builder (V2-11) gets absence semantics for free; the stage builder must
enforce them by hand, per property.

**V2-10. Adapter completeness is a gate, not a hope.** SI-7 already asserts
every `usdGen:*` property of the five registered types *appears and dirties*
(`testUsdGenAdapter`, `testUsdGenRestAdapter`); the standing rule from here
on: **a new prim type or C1 property without adapter coverage fails its T1
test** — extend the adapter tests with the type, and no Hydra-builder support
for it lands without that coverage. No special-case readers: Description
policy, Look, and session hints all travel through adapter-published
locators. A second reader for "just this one attribute" is how D1 happened.

**V2-11. Migration acceptance (supersedes V2-7 items 1–2; restates 3–4).**

1. `BuildGraphDescFromHydra(HdSceneIndexBase&, SdfPath, BuildOptions)` per
   the V2-9 table becomes the single production staging path in `_CommitNow`.
   The stage builder is renamed `BuildGraphDescFromStage`, fenced to
   T0/offline/tests (unit tests, fixture authoring), and no new
   stage-touching TU lands under `usdGenImaging/` after this point.
2. Delete `_ResolveStage`, the weak-stage registry,
   `SetStage`/`FindGroomStage`, and the session key's stage member; wire
   `usdGen:sessionId` through (V2-7 items 2+4, unchanged).
3. `testUsdGenStormSurgery` rewritten against the Hydra path; register or
   delete (D5, unchanged).
4. Parity gate before the stage path is unplugged: both builders produce an
   identical `UsdGenGraphDesc` (node set, edges, structural digests) on the
   G1–G4 fixtures — a new T1 test, added with the Hydra builder. Until it
   passes, D1 stays contained exactly as §3.3 states: both builders fill the
   same value type, so engine, compiler, router, and publisher need no
   changes.

---

## 8. Testing what you care about (binding)

Three concerns, three gate families — each concern below names the exact
registry ids that prove it, so a red gate is a named, owned failure rather
than a vague worry. New ids minted for this overlay are **bold**; everything
else already existed in `09` §5.

### 8.1 Performance — the frame stays inside the ledger

| What is proven | Gates | Status / owner |
|---|---|---|
| Chain throughput ≤ 1.5 ms @8 thr, sparse/terminal edits, recompile, scaling, determinism | E-1 (red — the one M1 blocker), E-2, E-6, E-7, E-8 | `09` §5.1; E-1 FAIL 1.69–1.94 ms (`.omp/gate-status.md`) |
| Ragged path, capture (kNN), 1 M memory | E-1r, E-3 (M2); E-4, E-5 (M3) | `09` §5.1; E-5 at risk by DERIVED accounting |
| Storm draw/upload/cull/batch/VBO at 100 k | S-1 (PASS 0.82 ms, 2026-09-12), S-5 (measured 2/2, hair in 1 batch — criterion ruling pending), S-6, S-12 (pending bench modes); S-2, S-3, S-4 (M2) | `09` §5.3; `benchUsdGenStorm` wired to the G3 fixture with `--refine 2` + perflog, fixtures self-generated by CMake |
| **Adapter-sourced staging costs nothing it cannot afford** | **SI-13** (≤ 0.2 ms on G3, ASSUMPTION, M2) | `09` §5.2; guards S-2's 2.0 ms deform delta against the §7 migration |
| Maps/expressions at scale; brush/pick latency; render-time blur | L-4; T-1, T-2; R-1 | `09` §5.4; T-1/T-2 component-measured |
| Perf never silently regresses | `nightly-perf` + `usdGenBench.json` threshold breaches (`10` §6.2, `09` §8: CI fails on breach, not delta) | every push (T2 correctness), nightly (timing) |

### 8.2 Architecture — the layering is enforced, not documented

| What is proven | Gates | Status / owner |
|---|---|---|
| Engine links no `usd`/`hd`; sources include no forbidden headers | B-1 | `09` §5.1, `10` §1.4 (one regex, two tests) |
| **Staging touches no stage APIs** (the V2-8 fence) | **B-2** (M2) | `09` §5.1; `testUsdGenStagingRule`, B-1 pattern |
| **Both builders agree exactly** before the stage path is unplugged | **SI-12** (M2) | `09` §5.2; `testUsdGenHydraParity` on G1–G4 |
| Contracts cannot drift (C1/C2/C5 name lists as literal test data) | `testUsdGenContracts` (+ C1 checker), `11` §0.2 | frozen `docs/freezes/C{1,2,5}.md` |
| Determinism across thread counts; exactness on every publish; operator value profiles | E-8, L-3; SI-1 (hard assert, release builds too); T0 `testUsdGenWidth` pins the Width root→tip profile (constant, ramp, interpolation token, taper, root/tip scale, set/multiply) | `09` §§5.1–5.2, `10` §5.5 |

### 8.3 Hydra 2.0 — the plugin is a disciplined scene-index citizen

| What is proven | Gates | Status / owner |
|---|---|---|
| Placement: after UsdImaging chain + scene globals, before Storm/hdPrman plugins | SI-5 (chain order) | `09` §5.2; first green in M0 |
| Cook discipline: one commit per event batch, zero cooks in `GetPrim`, correct threads, batching-tolerant | SI-3 (scheduling) | `09` §5.2; thread-id asserts |
| Lock-free reads under Storm's parallel sync | SI-4 (0 torn reads) | `09` §5.2; measured 16 734/0-torn |
| Precise invalidation (bare locators, no co-dirtying, overlay rule) | SI-2 (+ M2 overlaid-prims re-run) | `09` §5.2 |
| Population on both paths; sessions shared across indices | SI-6; SI-10 | `09` §5.2 |
| **Adapter coverage self-extends**: every C1 property of every type appears *and dirties*, present and future types | SI-7 + V2-10 rule | `09` §5.2; the *absence* was the measured failure (EV-081) |
| Pruning-wrapper cost on a production skinned scalp; batching; no VBO relocation | SI-9 (M2); S-5, S-6 | `09` §§5.2–5.3 |
| One binary under Storm, hdPrman, `usdrecord`; correct motion sampling | M7 `usdrecord --renderer GL`, sampled-points contract; R-1 (release) | `11` §2.8, `09` §5.4 |

Negative architecture (what Hydra 2.0 forbids us, and where that is
recorded): no hdGp vehicle (`01` §2.1 S6, `12` X-items), no cooking in
`GetPrim` (SI-3 asserts it; I7), no authored geometry under
`__usdGenRender`, no per-curve Hydra prims (chunk≠tile, `01` §0.1.7).

---

## 9. Harness handoff — resume point (2026-09-11)

The starting line for the next harness session, superseding §4's M2 row for
*position* only (contents and milestones unchanged). Source session
`ses_f7131d82fffeyTKSAhpQX8xIVT` (ultraworker, 567 msgs) stalled at 08:54Z
mid-V2-11: SI-12 had just gone green, the `_CommitNow`→Hydra migration had
just been scoped. Everything below was re-verified by fresh build + ctest
runs on 2026-09-11, not recalled from the stalled session.

### 9.1 Landed and green — do not redo

| Item | Evidence |
|---|---|
| `BuildGraphDescFromHydra` implemented (`usdGenGraphDescBuilder.cpp:1147`) per §7 V2-9/V2-10 | `testUsdGenHydraParity` **Passed** (ctest, 2026-09-11); library builds clean |
| **SI-12 green** — Hydra/stage parity on G1–G4 (node set, edges, digests) | `testUsdGenHydraParity` Passed 0.82 s; all six first-diffs (terminal, node count, surface count, motionMode, visibility, xformMatrix) resolved |
| Stage builder renamed `BuildGraphDescFromStage` (`:860`) — V2-11 item 1's rename half | header `usdGenGraphDescBuilder.h:69`; consumers: `groomSceneIndexPlugin.cpp:807`, `testUsdGenHydraParity.cpp:261` only |
| B-2 gate machinery wired and proven | `cmake/CheckNoStageStaging.cmake` + `testUsdGenStagingRule{,Negative,Positive}Fixture` (`CMakeLists.txt:~415–480`); both fixtures green — the gate bites both ways |

### 9.2 Opened but not closed — the live work

| Item | State |
|---|---|
| **B-2 RED: 56 hits** | `ctest -R testUsdGenStagingRule` fails; all hits are the stage builder *living inside the fenced TUs*: `usdGenGraphDescBuilder.h:16` (`stage.h` include), `usdGenGraphDescBuilder.cpp` (stage/attribute/primRange/relationship/usdGeom/usdShade includes + `:860` implementation), `groomSceneIndexPlugin.cpp` (`_ResolveStage`, stage-key reads). Green requires §9.4 B–D |
| `_CommitNow` still stage-sourced | `groomSceneIndexPlugin.cpp:807` calls `BuildGraphDescFromStage`; this is the exact stall point (V2-11 items 1–2) |
| Stage-side channels still live | `_ResolveStage` (`:246`), weak-stage registry fallback (`:315–318`), `SetStage` stash in `primAdapter.cpp:108–111`, session-store registry (`usdGenImagingSession.cpp:262–292`), session-key stage member (`usdGenImagingSession.h:43`, ADR §4.5 weak-stage primary) |
| `usdGen:sessionId` hardcoded-empty | `groomSceneIndexPlugin.cpp:243` ("M1 stub debt") — blocks the key's sessionId lane (V2-7 item 4; D1 closure needs it as the only key when the stage goes) |
| `build/` unconfigured for ninja | `build.ninja` absent (stale tree); test binaries + CTestTestfile survive so ctest runs. Reconfigure per README Build block before building |
| D5 surgery TU quarantined | `testUsdGenStormSurgery.cpp` at `.omp/quarantine/…broken-20260910`, registration EXISTS-guarded — rewrite against the Hydra path or delete at M2 exit |

### 9.3 Serving-layout facts — settled by dump; do not re-litigate

Discovered while greening SI-12; re-deriving them is wasted time:

1. The adapter overlays the **mapped source flat at the prim root** —
   `usdGen:*` props arrive as `terminal`, `surface`, `motion/mode`,
   `curve/…`; there is **no** intermediate `usdGen` container node.
2. Topology serves at `mesh/topology/…`, not directly under `mesh`.
3. Scalp mesh points serve as primvars (`primvars/points/primvarValue`);
   no top-level `points`/`widths` — the Hydra builder needs primvar
   fallback for both (implemented).
4. The B-2 nm half matches `Usd(Stage|Prim|Attribute|Relationship|Geom|
   Shade)` — **any** symbol containing those substrings trips it, including
   `UsdImagingUsdPrimInfoSchema`-style reads; the fallback path must use
   raw value reads, not schema getters.

### 9.4 Next actions — ordered, each with its acceptance check

Pre: `export PATH=$VENV/bin:$PATH`; reconfigure `build/`
(README Build block). Then:

| # | Action | Accept |
|---|---|---|
| A | Split the stage builder out of the fenced TUs: move the `BuildGraphDescFromStage` declaration + implementation (and every stage-only helper) into a non-fenced TU — recommended home `testutils/usdGenTestUtilsHd/` (the T1/T2 Hydra-permitting test-support lib, `CMakeLists.txt:202–206`; the stage builder's surviving consumers are the parity oracle and authoring only, so V2-11's "no new stage-touching TU under `usdGenImaging/`" stays satisfiable). Fenced `usdGenGraphDescBuilder.{h,cpp}` keeps the Hydra side only, drops `stage.h` from the header | compiles; B-1 gate unaffected (`ctest -L '^gate:B-1$'`) |
| B | Migrate `_CommitNow`: `groomSceneIndexPlugin.cpp:807` calls `BuildGraphDescFromHydra` (commit-window scene index + description path + time) | full `ctest` T1 green including `testUsdGenHydraParity` |
| C | Unplug the stage side channel: delete `_ResolveStage`/`FindGroomStage` fallback and the `key.stage` write from the plugin; delete the `primAdapter.cpp:108–111` `SetStage` stash and the session-store registry; drop the stage member from the session key (`usdGenImagingSession.h:43`) with `sessionId` primary — requires wiring `usdGen:sessionId` (V2-7 item 4, stub at `:243`) | `testUsdGenSessions` + `testUsdGenSnapshotRace` green |
| D | Close the gate | `ctest -R 'testUsdGenStagingRule'` **green** + both fixtures green (`ctest -L '^gate:B-2$'`) |
| E | SI-13: timing section in `testUsdGenHydraParity` (same harness) — `BuildGraphDescFromHydra` on G3 ≤ **0.2 ms** (ASSUMPTION; falsify by measurement) | `09` §5.2 row; M2 exit needs it |
| F | D5: rewrite `testUsdGenStormSurgery` against the Hydra path or delete it (EXISTS-guarded registration drops out either way) | M2 exit, D5 |

### 9.5 Standing constraints

* Run tests via ctest only — T1 tests carry the plugin-path `ENVIRONMENT`;
  a raw binary run loses adapter publication and fakes failures.
* `gate-status.md` is the frozen 2026-09-09 census — no Status-cell edits;
  `09` §5 owns gate status. SI-12's first-green evidence lives here (§9.1);
  the `09` row flips only with the full-suite run at M2 closeout.
* Commits held absent Integrator GO (wave-coordinator ruling stands): HEAD
  `a446e95`, ~1758 insertions uncommitted across 23 modified + 23 untracked.
* Frozen contracts (`docs/freezes/C{1,2,5}.md`) untouched; §7 sourcing
  mandate binding; S-bench gates require the quiescent-desktop protocol
  (`docs/workstation-protocol.md`).
