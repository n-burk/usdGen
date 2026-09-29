# usdGen Architecture

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document fixes the shape of usdGen: where it runs in the OpenUSD 26.08 / Hydra 2.0 pipeline, what
crosses each boundary, which thread does what, how a commit is triggered, how the code is cut into
libraries, and which alternatives were rejected with what evidence. It settles no operator parameter and
no schema property beyond what a boundary needs. Every load-bearing claim cites an S-number from
`design/brief-v1.md`, an ADR section from `design/adr-v1.md` (its §9 rulings R1-R45 supersede earlier
sections), a research report section, or an OpenUSD `file:line` re-grepped while writing this.

**Citation vocabulary.** A bare `R1`-`R9` is one of `design/brief-v1.md` §1's requirements; an ADR
ruling is always written `ADR §9 R<n>`. Every measured number carries an **`EV-nnn`** handle, and
`EV-nnn` means a row of `appendix-A-evidence-ledger.md` §2 — the only citation handle for a
measurement (ADR §9 R1, R42, R43). Tags are ADR §9 R42's: **MEASURED** (with its `EV-nnn`),
**DERIVED from `EV-nnn`** (scaled or interpolated; never MEASURED), **UNMEASURED** (with its gate),
**ASSUMPTION**.

Reads with: `00-request-and-scope.md` (why), `02-schema.md` (the prim types in §1),
`03-execution-engine.md` (the engine behind §6; §1.4 the tile arithmetic), `06-imaging.md` (the scene
index behind §2-§4), `08-tools.md` (§1.4, the C ABI of contract C4),
`10-build-dependencies-testing.md` (§1.2 the target registry, §1.3 the dependency rule, §1.6 the
target-to-milestone schedule, §3.5 the env-var registry), `11-roadmap.md` (M0-M8, §1's PW-1…PW-6),
`09-performance-and-benchmarks.md` (§0.2, the **only** frame ledger; §5, the normative gate registry),
`appendix-A-evidence-ledger.md` (§2, the `EV-nnn` rows every number here cites),
`12-risks-decisions-open-questions.md` (what is still open).

---

## 0. Thesis, the pipeline, and the frame budget

### 0.1 Ten lines

1. usdGen generates hair **in Hydra**: nothing the evaluator computes is authored into USD (R1, S8).
2. It is **one renderer-level `HdSceneIndexPlugin`**, phase 0 / `InsertionOrderAtEnd`, one binary, under
   Storm, hdPrman and `usdrecord` alike (S1, S2, ADR §5.1).
3. That slot alone is **structurally** downstream of every UsdImaging scene index, so scalps are read
   after usdRig, UsdSkel and flattening (R5, `research/G-chain-order-probe.md` §3).
4. Parameters arrive as Hydra data sources from a **UsdImaging prim adapter** over codeless schemas; no
   `UsdStage` exists downstream of the stage scene index (S8-S10).
5. The evaluator is a **bespoke TBB DAG over planar SoA curve buffers**, 4x faster than a persistent
   `VdfNetwork` at 100 k curves and 9-15x at 1 M (MEASURED, EV-001/EV-006 against EV-011; S21,
   `research/G-data-plane-engine-prototype-benchmark.md` §0, §5).
6. Work is **deferred to a commit**: one cook on one thread, an immutable generation published with
   `std::atomic_store`, dirties from diffing generations (S17-S19, ADR §4.3, §9 R20).
7. The unit of dirtiness is a **chunk** (512 curves); the Hydra prim is a **tile** (`chunksPerTile`
   chunks, 32-256 per description) — different granularities on purpose (ADR §1 on S23/S27, §9 R21).
8. **Storm is the bottleneck, not the evaluator**: a five-operator chain over 100 k x 8 CV costs
   **1.02 ms** in the 8-thread private arena while drawing 200 k curves at refineLevel 2 costs 23.93 ms
   (both MEASURED, EV-008 and EV-021; `research/G-data-plane-engine-prototype-benchmark.md` §3.3;
   `research/G-storm-hair-look-prototype.md` §5). So LOD is decimation, tiles carry `extent`, and an
   unchanged tile is never republished.
9. Freezing is not a mode: a frozen `BasisCurves` re-enters the graph from the scene index as an ordinary
   styler input (R3, R9, S42, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2).
10. Five contracts (C1-C5) freeze in M1-M5; everything else is not installed and may churn until M7
    (ADR §3).

### 0.2 The pipeline

```
  USD stage  (asset layers + session layer)
    |  UsdGenGroom / UsdGenDescription / UsdGenOperator* / UsdGenMap* / UsdGenGuideSet
    |  + the bound Mesh scalps (usdRig-moved, UsdSkel-skinned, or static)
    v
+---------------------------------------------------------------------------------+
| UsdImagingStageSceneIndex + UsdImagingCreateSceneIndices, in source order        |
|   prim adapters: the usdGen adapter publishes the typed `usdGen` container       |
|     (5 plugInfo entries + a UsdGenRestAPI API-schema adapter)                    |
|   -> locator caching -> extent resolving           (sceneIndices.cpp:218, :233)  |
|   -> Pi/Ni prototype propagation; flattening lives INSIDE Ni                     |
|        (sceneIndices.cpp:240, :282; niPrototypePropagatingSceneIndex.cpp:204)    |
|   -> post-instancing HdNoticeBatchingSceneIndex     (sceneIndices.cpp:287)       |
|   -> instance-proxy path translation -> material-bindings resolving (:295, :298) |
|   -> plugin scene indices (_AddPluginSceneIndices, sceneIndices.cpp:302):        |
|        RigExecUsdImagingSceneIndexPlugin ....... usdRig's deformed points        |
|        UsdSkelImagingResolvingSceneIndexPlugin . appends                         |
|          UsdSkelImagingSkeletonResolvingSceneIndex then                          |
|          UsdSkelImagingPointsResolvingSceneIndex (ext-computation points)        |
|        UsdGenMetadataSceneIndexPlugin ......... returns its input; supplies only |
|          InstanceDataSourceNames() + ProxyPathTranslationDataSourceNames()       |
|   -> selection (:305) -> render-settings flattening (:308)                       |
+---------------------------------------------------------------------------------+
    v   HdMergingSceneIndex   (a post-merging HdNoticeBatchingSceneIndex exists only
    v     on the legacy HdRenderIndex::New path, renderIndex.cpp:203; usdview
    v     batches upstream, at sceneIndices.cpp:287)
    v   HdsiSceneGlobalsSceneIndex        (app callback, phase 0 / AtStart)
+---------------------------------------------------------------------------------+
| UsdGenGroomSceneIndex        <-- renderer-level plugin, phase 0, AtEnd (S1)      |
|   private HdSiExtComputationPrimvarPruningSceneIndex over the input (read only)  |
|   UsdGenDirtyRouter -> UsdGenSession (in UsdGenImagingRegistry)                  |
|                        -> UsdGenGraph  (TBB DAG, private task_arena)             |
|   UsdGenGenerationStore: atomic snapshot publish + diff                          |
|   emits PrimsRemoved / PrimsAdded / PrimsDirtied for                             |
|     <Description>/__usdGenRender/{tile_0000.., guides/<set>[/cvs], inst_<op>}    |
+---------------------------------------------------------------------------------+
    v   Storm plugins (implicit surfaces, material bindings, velocity motion,
    v     dependency declaration ph100 / forwarding ph1000)          -> HdStorm
    v   or hdPrman plugins (ext-comp pruning ph0, velocity ph2, motion blur,
    v     pinned-curve expansion ph3, dependency forwarding ph1000)  -> hdPrman
```

Prototype propagation and flattening run **before** `_AddPluginSceneIndices`, so every UsdImaging plugin
— RigExec's, UsdSkel's, usdGen's metadata stub — and everything downstream of the merge already sees
flattened, prototype-propagated prims. That is the whole basis of S4 ("everything usdGen reads is
post-flattening"), and it is a source fact, not an ordering hope: `sceneIndices.cpp:240, :282, :302`,
`niPrototypePropagatingSceneIndex.cpp:204`, plus the same order MEASURED terminal-first in
`research/G-chain-order-probe.md` §2 (plugins at slots 4-8, `UsdImagingMaterialBindingsResolvingSceneIndex`
at slot 9).

The insertion point is a fact of `HdRenderIndex`'s construction: the UsdImaging chain is an *input* to
`HdMergingSceneIndex`, and renderer-level plugins append on top of the merge
(`pxr/imaging/hd/renderIndex.cpp:205-214`, guarded by the non-empty display name at `:208`;
`pxr/usdImaging/usdImagingGL/engine.cpp:1756-1779` for usdview's path). The scene-globals index comes
from `UsdImagingGLEngine`'s own callback at phase 0 / `InsertionOrderAtStart` (`engine.cpp:155`,
registered at `:188-200`), so *any* renderer-level plugin is downstream of it
(`research/G-evaluation-scheduling-and-batching.md` §3).

### 0.3 The frame budget

Target: **100 000 hairs x 8 CV, 1280x720, refineLevel 2, 49 tiles, 60 Hz**. The budget arithmetic uses
**16.6 ms** throughout (the exact 1/60 s is 16.67 ms; nothing here is derived from it), matching
`09-performance-and-benchmarks.md` §2.5's `N_max` solve and §5.3's S-1 threshold. The viewport is
1280x720 **because that is what was measured** (`research/G-storm-hair-look-prototype.md` §5); gate
S-1 also records 1920x1080 as a **separate** baseline that is never compared with the 720p one
(`09-performance-and-benchmarks.md` §5.3, §9).

**`09-performance-and-benchmarks.md` §0.2 is the only frame ledger in the plan** (ADR §9 R41). This
document does not restate it. The five rows §2-§6 reason from, quoted from it verbatim:

| Stage (quoted from `09-…` §0.2) | Cost | Tag |
|---|---:|---|
| usdRig rig evaluation (`ArmShotAnim.usda`, 95 prims) | 1.35 ms | MEASURED, EV-035 |
| **usdGen total, deform frame** | **0.8-1.0 ms** | DERIVED aggregate |
| Storm draw, refineLevel 2, 720p | ~12.2 ms | DERIVED from EV-020/EV-021; UNMEASURED at 100 k, gate **S-1** |
| Storm points upload (9.6 MB, scene-index publish) | ≈ 1.44 ms | DERIVED from EV-022, gate **S-2** |
| **Frame total** | **15.8-16.0 ms** | DERIVED |

Three of §0.2's internal terms are named again below because §4 and §6 turn on them, with §0.2's tags:
dirty routing + commit bookkeeping **0.04 ms** (DERIVED from EV-037, 0.2 µs/entry x ~200; the entry
count is itself UNMEASURED, gate **SI-3**); SoA -> AoS interleave of all dirty tiles **0.25 ms**
(DERIVED **floor** from EV-074 — a strided three-stream gather is not a memcpy, so UNMEASURED, gate
**S-2**); per-tile `extent` fused into that pass **0.05 ms** (**ASSUMPTION**, gate **S-2**).

**60 Hz at 100 k x 8 CV and refineLevel 2 is not established** (ADR §9 R41): 15.8-16.0 ms against a
16.6 ms budget, with the largest term derived rather than measured, and the whole ledger conditional
on gate S-8 selecting `hairTangent` variant A (§4.3, ADR §5.4). The v1 interactive targets are 60 Hz
at <= 40 k curves, >= 30 Hz at 100 k, >= 10 Hz at 200 k (ASSUMPTION until gate S-1);
`usdGen:densityScale` is the lever. Three readings drive the decisions below.

* **usdGen owns about 6 % of the frame** — 0.8-1.0 ms of 15.8-16.0 ms (DERIVED, from the rows above).
  Tripling the engineering on the evaluator buys under 1 ms; halving the drawn curve count buys about
  6 ms of the 12.2 ms draw term (DERIVED from `09-…` §2.5's `draw_ms(N) = 0.379 + 0.1178·(N/1000)`).
  Hence authored `extent` per tile for frustum culling (S29), an LOD ladder that decimates curve
  **count** and not refineLevel (S31; refineLevel 0 is *slower* than 1 at 200 k, 12.43 vs 8.17 ms,
  MEASURED, EV-021), and the rule that a tile whose chunks did not change is never republished.
* **A parameter edit must not cost a full chain run.** MEASURED, default 20-thread arena: last-styler
  edit 0.179-0.411 ms (EV-004), full run 1.72-1.91 ms (EV-001), 1 %-sparse edit 0.035-0.044 ms
  (EV-002) (`research/G-data-plane-engine-prototype-benchmark.md` §4; arena-pinned at 8 threads the
  full run is 1.02 ms, EV-008, §3.3). Per-node output buffers (S24) and per-chunk dirty bytes (S23)
  buy that, at ~6x the curve data in memory (668 MB at 1 M x 8 CV, MEASURED, EV-007, §5).
* **A brush stroke must cost the stroke, not the groom.** 21.3 µs of Python per move (MEASURED,
  EV-061, `research/G-tool-loop-array-transport-and-cv-picking.md` §1.4), ~0.035 ms of engine
  (EV-002), one re-upload.

**The budget this implies.** usdGen's deform-frame cost is capped at **2.0 ms** — gate **S-2**: at most
a 2.0 ms delta over a static frame, publish included. That is the *failure* ceiling; the design share
is the tighter **1.0 ms** of `09-performance-and-benchmarks.md` §0.1 class A, which is also the number
its §2.5 `N_max` solve subtracts. The chain inside that delta is bounded separately
by gate **E-1** at **<= 1.5 ms** for a *full* 5-operator run over 100 k x 8 CV in the 8-thread arena
(MEASURED 1.02 ms, EV-008; ADR §9 R27 tightens the 2.5 ms of `design/proposal-performance.md` §11.2) —
a thing a deform frame never does, because only the deformed tail re-runs (I4). Interactive edit cost
is capped at **0.6 ms** (gate E-3), per-brush-move cost at **1 ms of Python plus engine** (gate T-1);
`09-performance-and-benchmarks.md` §5 is the gate registry (ADR §9 R40). Exceeding those is a design
error, not a tuning problem: Storm has already spent the rest of the frame.

---

## 1. The five nouns, and the Hydra-only render scope

usdGen names the artist's model once and never re-names it downstream (`design/proposal-artist.md` §2,
ADR §2.1-2.2). An a host groomer artist thinks *description -> generator -> modifiers -> guides -> maps -> a look*;
A DCC artist thinks *a chain of SOPs with masks*. Both land on the same five nouns, and every
concrete usdGen prim type in ADR §2.1's hierarchy is an instance of one of them.

| Artist noun | usdGen prim | Hydra consequence |
|---|---|---|
| "my character's hair, all of it" | `UsdGenGroom` (`UsdGeomImageable`) | none: a grouping and defaults scope; carries `uniform int usdGen:schemaVersion = 1`, `uniform string usdGen:sessionId` |
| "the eyebrow description" | `UsdGenDescription` (`UsdGeomBoundable`) | 32-256 `basisCurves` tile prims + a guide prim set + optional instancers (S27, S33) |
| "the clump modifier" | `UsdGenClump`, a `UsdGenOperator` | one node in the TBB DAG (S21) |
| "the density map I painted" | `UsdGenPaintMap`, a `UsdGenMap` | a CPU sample at capture, baked to a per-curve array (S37) |
| "my guides" | `UsdGenGuideSet` + child `BasisCurves` | a `purpose = guide` curves prim, plus a `points` child prim for CV display (S40) |

A sixth thing an artist names — "the frozen groom" — is deliberately **not** a usdGen prim type: it is a
plain `UsdGeomBasisCurves` carrying the S42 primvar contract, re-entering the graph as an ordinary source
node (S42, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2).

The one name the artist never types is the machine's. **Everything the evaluator generates lives under
`<Description>/__usdGenRender`, which exists only in Hydra and is never authored** (ADR §2.2) — usdRig's
`<rig>/__RigExecGenerated` rule (`research/A2-usdrig-imaging.md` §5). Three consequences:

* An artist never sees `tile_0037` in usdview's prim browser, only in the Hydra scene browser; a `.usda`
  diff of a groom shows operators and parameters, never geometry.
* usdGen is the **sole producer** there, so the invalidation rule is the cheap one (bare
  `primvars/points/primvarValue` leaves, S30 / ADR §5.2) and the primvar-descriptor cache stays warm
  (`research/G-storm-throughput-and-prim-granularity.md` §1.9).
* The reserved names are fixed and reproducible across sessions: `tile_0000 … tile_NNNN`,
  `guides/<setName>`, `guides/<setName>/cvs`, `inst_<opName>[/Prototypes/<n>]`, and `material_storm` only
  when `USDGEN_STORM_MATERIAL_OVERRIDE=1` (ADR §2.2, §5.6). Tile count derives from
  `uniform int usdGen:tileTarget = 64` on the Description, clamped 32-256, authored precisely so the prim
  set is identical in every attached index (ADR §2.3, §9 R21); the arithmetic is §4.5.

Authored layout, normative in `02-schema.md`: `<Groom>/<Description>/Ops`, `/Guides`, `/Maps`,
`/Prototypes`, `/Frozen`, plus the Hydra-only `/__usdGenRender`. Chain order is the `rel usdGen:input`
edge set, resolved by Kahn topological sort with namespace order as the only tie-break (S26); the
terminal is `rel usdGen:terminal`, exactly one target (ADR §2.3).

---

## 2. Placement in the Hydra pipeline

### 2.1 S1-S7, restated with the evidence that fixed them

| # | Decision | Evidence re-verified for this document |
|---|---|---|
| S1 | A **renderer-level `HdSceneIndexPlugin`**: `loadWithRenderer: ""`, C++ registration at phase **0** / `InsertionOrderAtEnd`, plus a plugInfo entry with `tags: ["usdGen:groom"]` and `ordering: {after: ["hd:sceneGlobals"], before: ["hdGp:proceduralResolution", "hdPrman:motionBlur"]}` (§2.3). It overrides `HdSceneIndexPlugin::_IsEnabled()` on the kill-switch `USDGEN_ENABLE` (default on): the registry consults it per append and a disabled plugin returns its input unchanged — the supported way out of a chain without editing plugInfo (ADR §5.1). | `InsertionOrder`/`InsertionPhase` at `pxr/imaging/hd/sceneIndexPluginRegistry.h:89-95`; `RegisterSceneIndexForRenderer` at `:141-147`; `allRenderers = ""` at `:27-32`; the `IsEnabled` consult at `sceneIndexPluginRegistry.cpp:1196-1199` (`research/G-hdprman-and-usdrecord-render-time-chain.md` §2.2 point 5). Chain dump MEASURED, `research/G-chain-order-probe.md` §3. |
| S2 | One binary serves Storm (display name `"GL"`), all four RenderMan display names and `usdrecord`. | `pxr/imaging/plugin/hdStorm/plugInfo.json:10`; both `AppendSceneIndicesForRenderer` callers funnel into one registry (`research/G-hdprman-and-usdrecord-render-time-chain.md` §1.2). |
| S3 | Surfaces are read through a **private** `HdSiExtComputationPrimvarPruningSceneIndex`, never spliced into the shared chain. | `pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:35-45`; UsdSkel blocks terminal `primvars:points` and the wrapper restores them headlessly (MEASURED, `research/G-chain-order-probe.md` §4a-4b); Storm ships no such plugin (`research/G-hdprman-and-usdrecord-render-time-chain.md` §5). |
| S4 | Everything read is **post-flattening**: `xform/matrix` is world (or prototype-common) space with `resetXformStack = true`, dirtiness per prim, never hierarchical. Output prims carry the surface's world matrix with `resetXformStack = true` and surface-local points. | S4; `research/A2-usdrig-imaging.md` §5. Consequence: `visibility`, `purpose` and `materialBindings` are authored by hand on synthesized prims — inheritance already happened upstream. |
| S5 | An index that **overrides a prim's `primvars` container** must dirty the bare `primvars` locator too. | MEASURED bug and fix, `research/G-chain-order-probe.md` §4c: UsdSkel's resolved prim refreshes only on `Contains(primvars)` (`usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:1178-1180`), so leaf-only dirties freeze the skin. |
| S6 | hdGp is not the vehicle. | Four disqualifications, evidence in §8.1. |
| S7 | A **metadata-only `UsdImagingSceneIndexPlugin`** (`UsdGenMetadataSceneIndexPlugin`) returns its input unchanged, purely for `InstanceDataSourceNames()` and `ProxyPathTranslationDataSourceNames()` (§2.5). | Both are **non-const** virtuals: `pxr/usdImaging/usdImaging/sceneIndexPlugin.h:81-83`, `:90-92`; `const override` is a compile error (`design/judge-evidence.md` §2.4). |

### 2.2 Why renderer level, and not the two alternatives

**Not a second `UsdImagingSceneIndexPlugin`.** Those append in the order of a `std::set<TfType>` keyed on
the private `_TypeInfo*` heap address (`usdImaging/sceneIndexPlugin.cpp:50-54`); the probe MEASURED that
swapping two `PXR_PLUGINPATH_NAME` entries flips the order (`research/G-chain-order-probe.md` §2). In the
observed default order the plugin also lands **upstream** of `UsdSkelImagingPointsResolvingSceneIndex`
(appended, after `UsdSkelImagingSkeletonResolvingSceneIndex`, by
`UsdSkelImagingResolvingSceneIndexPlugin` at
`pxr/usdImaging/usdSkelImaging/resolvingSceneIndexPlugin.cpp:35, :38`), so it would read the bind pose
instead of the deformed scalp — a silent wrong-geometry failure that violates R5.

**Not a usdRig registry hook.** A hook on `RigExecImagingRegistry` orders usdGen against usdRig only,
says nothing about UsdSkel (the ordering that matters), and makes usdGen unusable without usdRig
(`research/G-chain-order-probe.md` §6).

**Renderer level, phase 0, `InsertionOrderAtEnd`** gives three properties at once: strictly downstream of
the whole UsdImaging chain, strictly downstream of `HdsiSceneGlobalsSceneIndex`, and strictly **upstream**
of every Storm and hdPrman plugin — implicit surfaces, material-bindings resolving, velocity motion,
dependency declaration and forwarding, hdPrman motion blur and pinned-curve expansion (MEASURED chain
dump, `research/G-evaluation-scheduling-and-batching.md` §3). Upstream of velocity motion lets generated
curves join Storm's velocity blur (motion profile P1); upstream of Storm's
`HdsiMaterialBindingResolvingSceneIndex` — appended by `HdSt_MaterialBindingResolvingSceneIndexPlugin`,
phase 0 but `InsertionOrderAtStart`, `pxr/imaging/hdSt/plugInfo.json:44-56` — lets usdGen author tile
material bindings by hand. That is Storm's resolver, a different class from UsdImaging's
`UsdImagingMaterialBindingsResolvingSceneIndex`, which ran far upstream at `sceneIndices.cpp:298`.

### 2.3 What the ordering tag is, and is not

The plugInfo `ordering.after: ["hd:sceneGlobals"]` is **decorative in stock 26.08 and must never be
relied on**. No plugin in the tree declares an `hd:sceneGlobals` tag; the string appears only as an
`after` key on *other* plugins' entries — `third_party/renderman/plugin/hdPrman/plugInfo.json:190,217`
and `pxr/imaging/hdSt/plugInfo.json:118,132,172` — so nothing it could order after ever exists (all five
re-grepped; `design/judge-evidence.md` §0). `HdsiSceneGlobalsSceneIndex` comes from the engine's app
callback (`usdImagingGL/engine.cpp:155`, `:188-200`), not from a tagged plugin; placement after it is
guaranteed by phase 0 / `InsertionOrderAtEnd` against that callback's `InsertionOrderAtStart` (ADR §1 on
S1). Keep the tag as documentation; gate SI-5 asserts the *resolved chain*, not the tag.

Two further registration rules, both measured: a C++ `RegisterSceneIndexForRenderer` call with **no**
matching plugInfo entry is silently dropped under the default `Hybrid` policy, and a plugInfo entry with
no C++ registration is never instantiated — so **both ship**
(`research/G-evaluation-scheduling-and-batching.md` §3 consequence 4; the drop rule itself is
`research/G-hdprman-and-usdrecord-render-time-chain.md` §2.2 point 3, where the composed entry list is
seeded from the JSON entries alone, `pxr/imaging/hd/sceneIndexPluginRegistry.cpp:850-869`). And an app
that news up its own render delegate gets **no** renderer-level plugins, because the display name is
empty and `renderIndex.cpp:208` skips the append; `_SetRendererDisplayName` is private with
`friend class HdRendererPlugin` (`pxr/imaging/hd/renderDelegate.h:584-589`). usdview, `usdrecord` and
hdPrman go through `HdRendererPlugin::CreateDelegate`, so this bites only bespoke hosts, which
`06-imaging.md` states as a host requirement.

### 2.4 The pruning wrapper, and the deformed-surface pickup path

`UsdGenGroomSceneIndex` constructs exactly one `HdSiExtComputationPrimvarPruningSceneIndex` around
`_GetInputSceneIndex()` and keeps it private (S3). It is a pass-through when nothing is computed
(`hdsi/extComputationPrimvarPruningSceneIndex.cpp:703-721`), so it costs nothing on a rig-only stage;
gate SI-9 measures it on a production-density skinned scalp (ADR §9 R40). The pickup path, in order:

1. Prototype propagation, flattening and material-bindings resolving have already run
   (`sceneIndices.cpp:240, :282, :298`), so any UsdImaging plugin sees a mesh whose `xform/matrix` is
   world (or prototype-common) space with `resetXformStack = true`.
2. In the plugin band (`sceneIndices.cpp:302`), whoever deformed the mesh publishes its result — usdRig's
   `RigExecResultsSceneIndex` (`primvars/points` directly), or UsdSkel imaging (a **blocked**
   `primvars:points`, `usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:501-508`, plus
   `extComputationPrimvars:points` whose computation carries a CPU callback,
   `usdSkelImaging/dataSourceResolvedExtComputationPrim.cpp:443-449`;
   `research/G-hdprman-and-usdrecord-render-time-chain.md` §5), or any third modifier.
3. `UsdGenSurfaceReader::Resolve(path)` pulls `primvars/points` through the private wrapper. Still null
   while `extComputationPrimvars/points` exists is a **hard, named diagnostic** (`usdGen: surface <path>
   has computed points that could not be resolved`), never silence (`design/proposal-risk.md` §5.4).
4. Rest points come from the `UsdGenRestAPI` adapter's `usdGen/rest/points` at `UsdTimeCode::Default()`,
   honouring an authored `primvars:rest` (S12). Capture-on-first-cook is rejected: the first cook may be
   at any frame.
5. A `primvars/points/primvarValue` dirty marks only the **deformed-space tail** dirty, for the chunks
   whose roots live on that surface. An `xform/matrix` dirty re-dirties only the tiles' own `xform` (S4).
   A `UniversalSet` dirty or a re-`PrimsAdded` is a resync: re-read topology, recapture.

`GeomSubset` targets restrict scatter to the subset's faces, but `primvars:skinprim` and Ptex face ids
are **always** indices into the parent mesh's faces, and a subset edit is a recapture (ADR §2.3, §9 R15).

### 2.5 Instance-proxy translation and native prototypes (S7, S33-S34)

`InstanceDataSourceNames()` returns `{"usdGen"}` so two native instances with different groom bindings
**aggregate separately** instead of collapsing into one prototype (MEASURED, probe5,
`research/G-stage-free-parameter-and-time-transport.md`). `ProxyPathTranslationDataSourceNames()` returns
`{"usdGen"}` so a `rel usdGen:surface` targeting an instance proxy (`/World/HeadA/Scalp`) translates to
the prototype path. Inside natively instanced scalps, hair is generated **once per propagated prototype
path** (`…/UsdNiPrototype/…`), `instancedBy` is authored by hand from `__usdPrimInfo.isNiPrototype` /
`niPrototypePath`, and `primOrigin` inside a prototype is **relative** (S34). Propagated prototype names
are hashes: discover them, never construct them. The rebase arithmetic mapping `usdGen:surface` onto a
prototype root is an ASSUMPTION from `design/proposal-performance.md` §1 (assumption 4), settled by gate
T-INST-2 in M6.

---

## 3. Stage-free data flow

### 3.1 The four boundaries

```
  USD attribute            Hydra data source          engine value          Hydra prim
  ------------             -----------------          ------------          ----------
  usdGen:clump:amount  ->  usdGen/clump/amount   ->  UsdGenGraphDesc   ->  __usdGenRender/
  rel usdGen:input     ->  usdGen/input               .nodes[i].params      tile_0007
  rel usdGen:surface   ->  usdGen/surface             .edges                 primvars/points
  primvars:rest        ->  usdGen/rest/points         .surfaces              extent, xform
       [adapter]              [scene index]             [engine]              [publisher]
```

**Boundary 1 — adapter.** A codeless prim with no adapter reaches the terminal scene index with
`primType == ""`, no schema attributes, and — decisively — **no notice at all** when a `usdGen:` property
is edited (MEASURED, EV-081 and EV-082, `research/G-stage-free-parameter-and-time-transport.md` §1, §5;
the same failure on a `BasisCurves` in `research/G-evaluation-scheduling-and-batching.md` §4 finding
S5). The fix is a `UsdImagingSceneIndexPrimAdapter`
(`pxr/usdImaging/usdImaging/sceneIndexPrimAdapter.h:27`) publishing a
typed `usdGen` container via `UsdImagingDataSourceMapped`, mappings built generically from
`UsdPrimDefinition::GetPropertyNames()`, invalidation through `UsdImagingDataSourceMapped::Invalidate`
(S10). **One adapter base under five plugInfo entries** — `UsdGenOperator` and `UsdGenMap` with
`includeDerivedPrimTypes: true` (`usdImaging/adapterRegistry.cpp:137-201`), plus `UsdGenGroom`,
`UsdGenDescription`, `UsdGenGuideSet`, which derive from `UsdGeomImageable` rather than the operator base
and would otherwise be invisible (ADR §2.3, mandatory correction from all three judges). The registry
maps one `TfType` per `primTypeName` (`adapterRegistry.cpp:104-131`), so the entries are
`UsdGenPrimAdapterBase` plus five one-line subclasses, with `UsdGenRestAPIAdapter` for the API schema
(ADR §9 R3).

**Boundary 2 — `UsdGenGraphDesc`.** `usdGenImaging` walks the `usdGen` containers and builds a **pure
value description**: node paths, prim types, parameter blobs, `usdGen:input` edges, read phases, surface
and map targets, and the curve sets through which guides, freezes and imports reach the engine (ADR
§4.2.3, §9 R23). The engine consumes only that — which is why it links no `usd`, no `usdImaging` and no
`hd` (§5), and why tier-0 tests need neither a stage nor a scene index.

**Boundary 3 — the generation.** The engine produces `UsdGenCurveBuffer`s in planar SoA. The publisher
interleaves the dirty tiles' per-CV data into `VtVec3fArray` exactly once (I1), computes each tile's
`extent` on the same pass, and assembles an immutable `UsdGenGeneration`.

**Boundary 4 — `GetPrim`.** `UsdGenGroomSceneIndex::GetPrim` does one `std::atomic_load` and wraps the
arrays in retained data sources. It never cooks, never locks, never touches the stage (S19; MEASURED: 8
reader threads x 20 publishes = 16 734 reads, 0 torn reads, EV-039,
`research/G-evaluation-scheduling-and-batching.md` §5).

### 3.2 What crosses, and what deliberately does not

| Crosses the boundary | Never crosses |
|---|---|
| Resolved `SdfAssetPath` (map files) — resolution happens in the adapter, stage-free (`research/G-stage-free-parameter-and-time-transport.md` §2) | A `UsdStage`, a `UsdPrim`, a `UsdAttribute` (S8) |
| `VtArray<SdfPath>` of forwarded relationship targets (`usdImaging/dataSourceRelationship.h:22-59`) | A `.spline` on a **ramp** property — a compile error, since a `.spline` attribute is always deemed time-varying and would dirty every frame (`pxr/usd/usd/stage.cpp:9751-9758`; S11, ADR §9 R11) |
| A whole `TsSpline` as `HdTypedSampledDataSource<TsSpline>`, evaluated at an arbitrary parameter with zero per-frame invalidation (MEASURED, probe6, `research/G-stage-free-parameter-and-time-transport.md` §2.1) | Machine tuning: `USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`, `USDGEN_MEMORY_BUDGET_MB` are env, never authored (ADR §2.3) |
| Rest points at `UsdTimeCode::Default()` via the `UsdGenRestAPI` adapter (S12) | Curve arrays through OpenExec/VDF (S16, S21) |

**Lazy dependency registration is a trap with a rule.** Time-varying and asset-path dependencies register
inside the data source constructor, so a consumer that never pulls a value never gets its dirties
(MEASURED, probe2/probe3, same report). S14's consequence: **`UsdGenOp::Bind()` pulls every mapped
locator of its node, regardless of mode.** A parameter that only matters when `noise:amount > 0` is still
pulled when it is 0, or an edit to it may never invalidate. Gate SI-7 asserts every declared property of
every registered type both appears and dirties (ADR §9 R6).

### 3.3 Which thread does what

| Thread | May do | May not do |
|---|---|---|
| App thread or the notice thread — together **the commit thread** | accumulate dirties; run `Compile`, `Capture`, `Evaluate`; interleave; publish; emit notices | block on I/O without a timeout |
| TBB workers in the session's **private `tbb::task_arena`** | evaluate chunks | allocate into another chunk's buffer, write dirty bytes, emit notices, read another hair chunk (I3) |
| Hydra readers (Storm's parallel Rprim sync) | `std::atomic_load` the generation, wrap it in data sources | evaluate, lock, change time, touch the stage (S19) |

The no-allocation rule binds `Evaluate()` kernels only; `Capture()` may allocate and take the graph mutex
and runs on the commit thread or under `WorkParallelForN`, outside the arena (ADR §9 R22). The arena is
private because Storm syncs Rprims with `WorkWithScopedParallelism` + `WorkParallelForN`
(`pxr/imaging/hd/renderIndex.cpp:1830-1862`): a shared arena would make a commit overlapping a sync fight
for the same workers, and would force a global `PXR_WORK_THREAD_LIMIT` change that slows usdRig down.
Its worker count and the measured knee behind it are I8.

---

## 4. The commit model

### 4.1 Triggers

Cooking is deferred (S17-S18). Notice handlers forward the input notices immediately and unchanged and
accumulate dirty state; nothing **ever** cooks in `GetPrim` (I7). The triggers are ADR §4.3 as amended by
ADR §9 R32:

| Trigger | When | Thread |
|---|---|---|
| (a) `UsdGenImaging_SetTime()` / `UsdGenImaging_Commit()` | the usdview plugin, from `currentFrameChanged` (the **signal's** frame, not the property); an explicit `Commit()` after live-override changes; batch drivers wrapping `usdrecord` | app thread |
| (b) the `/` + `sceneGlobals/currentFrame` dirty | stock hosts with no (a): usdview without the plugin, `usdrecord`, hdPrman | notice thread |
| (c) an operator-parameter / map / surface-topology dirty | **always**, driver or not: commit synchronously at the end of the `_PrimsDirtied` batch that carried it — exactly one cook per edit batch | notice thread |

Because (c) always applies, an application **never** calls `Commit()` for a stage edit (ADR §9 R32): a
tool's release sequence is close the live override without republishing -> author the edit in one
`Sdf.ChangeBlock` -> request a repaint -> `ApplyPendingUpdates` delivers the dirty -> the index cooks and
publishes. No `Usd.Notice` listener ordering rule exists in the design. When (a) is attached, (b) only
sets the dirty flag. All notices are emitted from the commit path, on the thread that committed. Gate
SI-3 counts cooks: 10 interactive events must produce exactly 10 commits.

Trigger (b) is safe because the frame notice is measurably the **last** notice of every un-batched frame
and can never be coalesced with the geometry notices: `HdsiSceneGlobalsSceneIndex` sits downstream of
every notice-batching scene index in either construction path — post-instancing in the engine path
(`sceneIndices.cpp:287`, wrapped per frame at `usdImagingGL/engine.cpp:2368-2371`), post-merging on the
legacy `HdRenderIndex::New` path (`renderIndex.cpp:203`) — and emits its own `_SendPrimsDirtied` for
`currentFrame` (`pxr/imaging/hdsi/sceneGlobalsSceneIndex.cpp:177-193`). Exactly **2 `PrimsDirtied`
calls per frame** regardless of scene size, for N = 1…5 000 dirty prims (MEASURED, EV-037,
`research/G-evaluation-scheduling-and-batching.md` §4, §9). The new time is readable inside the first
notice handler, because `UsdImagingStageSceneIndex::SetTime` assigns it before dirtying
(`usdImaging/stageSceneIndex.cpp:368-370`, the assignment at `:905-916`).

Trigger (c) **replaces** S18's `GetPrim` backstop, withdrawn for two independent reasons (ADR §1 on
S18(c)): a `GetPrim`-driven cook runs on worker threads (MEASURED, EV-038: 4 distinct threads, same
report §5), and it emits zero downstream notices — Storm re-pulls only what was dirtied, so a cook that
announces
nothing is invisible. OpenUSD states the rule twice in hdGp's own source: *"Cooking of procedurals is
driven by notices. Don't cook the procedural in response to scene queries."*
(`hdGp/generativeProceduralResolvingSceneIndex.cpp:63-65`, `:120-122`).

### 4.2 Inside one commit

The order is ADR §9 R22:

```
 1. route the dirties: locator -> node -> chunk -> tile        (UsdGenDirtyRouter; no search)
 2. recompile iff the Merkle structural digest changed, and then only the affected sub-graph
       (~40 us -- ASSUMPTION, design/proposal-performance.md §5.3; gate E-6 measures it:
        <= 0.2 ms to append a node to a 200-node groom, with exactly 1 node rebuilt)
 3. evaluate the reference lane to completion, in dependency order
       (guides, clump centres, card roots -- un-chunked; I3)
 4. re-capture nodes whose capture epoch changed; they read the buffers step 3 just filled
       (parallel over independent nodes; Capture() may allocate and take the graph mutex)
 5. for each hair node in topological order:
        skip if node.dirtyChunks is empty
        arena.execute([&]{ tbb::parallel_for(blocked_range<size_t>(0, nChunks, 1), body); })
        body: skip clean chunks; memcpy input chunk -> node's own buffer; op->Evaluate(...)
    the commit thread then clears the node's dirty bytes (workers never write them: no false sharing)
 6. interleave dirty tiles SoA -> AoS and compute their extents      (parallel over tiles)
 7. build the immutable UsdGenGeneration; UsdGenGenerationStore::Publish (atomic_store)
 8. diff against the previous generation -> UsdGenDirtyReport
 9. emit PrimsRemoved, PrimsAdded, PrimsDirtied, in that order  (usdRig's order, research/A2 §3.4)
```

Steps 5 and 6 are the only regions inside the private arena; step 4 is parallel but outside it. Step 9
runs on the commit thread because observer callbacks are not required to be threadsafe (S19). `Diff` is
cheap by construction: each tile carries a `topologyVersion` and per-array `VtArray` data pointers, so
"did this tile change" is a pointer comparison, and structural changes come from a prim-set signature
(`design/proposal-performance.md` §6.2). If a scrub supersedes an in-flight commit, `Commit` returns the
**previous** generation unchanged **and leaves the session dirty** so the next trigger re-runs; usdGen
never half-publishes (ADR §4.2.4).

**Handoff.** The generation is a `std::shared_ptr<const UsdGenGeneration>` published with
`std::atomic_store` and read with `std::atomic_load` — the idiom the zero-torn-reads probe measured;
`TfRefPtr` has no atomic load (ADR §9 R20). `VtArray` copy-on-write is the default (S24): the publish
costs **0.0000-0.0003 ms**, at or below the probe's timer resolution (MEASURED, EV-010,
`prototypes/data-plane-benchmark/results_main.txt:95-101`), and the next edit pays one detach of the
9.6 MB buffer. **The size of that detach is disputed and is not a measurement**: the report's prose
gives 2.32 vs 1.97 ms while the carried log prints `run_full_with_outstanding_ref_ms 3.10` against
unreferenced runs of 1.72-1.91 ms (`results_main.txt:102`, `:60-66`), and 1.97 ms appears nowhere in
the prototype output — UNMEASURED, gate **E-1** re-measures the detach inside the arena, exactly as
`03-execution-engine.md` §0.3 records. A double-buffered publish ring is permitted
**only** as an M7 optimisation, and only where usdGen can *prove* nothing else references the back buffer
(ADR §1 on S24) — hdPrman retains arrays across a sync.

There is no API to ask a `VtArray` that question: 26.08 exposes **no public uniqueness query**
(`_IsUnique()` is private, `pxr/base/vt/array.h:1023`, behind the `private:` label at `:984`; the only
public identity query is `IsIdentical(VtArray const &)` at `:945`). The proof is structural: every
published array wraps a `UsdGenGenerationBuffer : Vt_ArrayForeignDataSource`
(`pxr/base/vt/array.h:39-51`) constructed with a detached callback, `VtArray` invokes it when no array
shares the buffer any more, and only then does the buffer return to the session pool. A buffer not handed
back is never reused; the fallback is plain CoW. (ADR §9 R20 supersedes the S24 amendment's wording,
which named a `VtArray` uniqueness API that does not exist; `12-risks-decisions-open-questions.md`
carries that as an erratum.)

### 4.3 Invalidation and notice-batching semantics

Per changed tile, where usdGen owns the prim (always, under `__usdGenRender`): the bare leaves
`primvars/points/primvarValue`, `extent/min`, `extent/max`; `xform/matrix` only when the surface's moved.
Where usdGen **overlays** an upstream prim — a frozen `BasisCurves`, a `visibility = false` override on a
C3 source prim, a CV-display child — `HdContainerDataSourceEditor::ComputeDirtyLocators` **and** the bare
`primvars` locator, the measured UsdSkel-freeze rule (S5, ADR §5.2, §9 R29). A primvar that *appears* is
dirtied as `primvars/<name>` once (S30).

Never co-dirty `displayColor` with `points`. Storm's points fastpath is taken only when `DirtyPoints` is
set and `DirtyNormals`, `DirtyWidths`, `DirtyPrimvar` are all clear
(`pxr/imaging/hdSt/basisCurves.cpp:932-935`), and `DirtyPrimvar` is one bit covering **every** primvar
except `points`, `velocities`, `accelerations`, `nonlinearSampleCount`, `normals` and `widths`
(`pxr/imaging/hd/changeTracker.cpp:958-979`). The three names beyond `points`/`normals`/`widths` —
`velocities`, `accelerations`, `nonlinearSampleCount` — are motion-sample plumbing, and the tile
contract blocks all three outside motion profile P1 (ADR §5.3), so on a usdGen tile the exclusion set
is effectively `points`, `normals` and `widths`. Losing the fastpath re-pulls the primvar descriptors
and re-uploads every non-points primvar. That is why `hairTangent` is a measured
fork rather than a mandate (ADR §5.4, gate S-8): the default Storm variant derives the tangent from
`inData.Neye` and publishes **no** `hairTangent` primvar.

`__dependencies` declares **one edge per tile** onto its surface prim — 32-256 edges, never per curve;
`HdDependencyForwardingSceneIndex` fan-out is 0.17 us/prim at N=32 and 1.30 us/prim at N=100 000
(MEASURED, EV-031, `research/G-storm-throughput-and-prim-granularity.md` §1.8, and §8.7 for what that
costs).

`HdNoticeBatchingSceneIndex` **coalesces, it does not deduplicate**
(`research/G-evaluation-scheduling-and-batching.md` §2). With a batch open across a frame — the
post-instancing one the engine opens per frame (`engine.cpp:2368-2371`), or the post-merging one on the
legacy path (`renderIndex.cpp:203`) — the frame notice can arrive *first* and the geometry batch second.
The commit logic must tolerate that inversion and never depend on it. usdGen never wraps a host's frame
block in `MergingSceneIndexNoticeBatchBegin/End` on usdview's behalf; a C++ host we own may.

### 4.4 Initial population

A renderer-level filtering scene index can be constructed over an already-populated input and gets no
`PrimsAdded` replay. Two paths, both required (ADR §4.4):

* **Notice path (usdview, `usdrecord`).** The groom index is constructed *before* the UsdImaging input is
  inserted into the engine's merging index, and `HdMergingSceneIndex::InsertInputScenes` walks the new
  input and calls `_SendPrimsAdded` for it (`pxr/imaging/hd/mergingSceneIndex.cpp:173-244`).
* **Traversal path (a populated input, e.g. the legacy `HdRenderIndex::New` route).** If no `PrimsAdded`
  has been seen (`_populated == false`), the index performs **one bounded traversal** on its first
  `GetChildPrimPaths`/`GetPrim`, scanning only for `UsdGenGroom` roots, budgeted at <= 5 ms on the
  11 005-prim reference stage (ASSUMPTION until SI-6 measures it, ADR §9 R28).

Gate SI-6 asserts both paths populate identically. A stage with no `UsdGenGroom` pays one pointer
comparison per notice and nothing per `GetPrim`; a site that wants usdGen out of the chain sets
`USDGEN_ENABLE=0` and the plugin is never appended (§2.1, S1).

### 4.5 Sessions across renderer switches and multiple chains

`UsdGenImagingRegistry` is a process-global `TfSingleton` keying sessions by (weak stage pointer, groom
root), falling back to `uniform string usdGen:sessionId` only when no stage is reachable, namespaced by
`renderInstanceId` + root-layer identifier so two stages authoring the same id never share a session
(S15, ADR §4.5, §9 R30). The fallback is what makes the tools' C ABI work with no weak stage pointer.

Several `UsdGenGroomSceneIndex` instances **attach to one session**: a renderer switch constructs a new
chain, and a process may run Storm plus an hdPrman preflight at once. The session owns one current frame
— whichever `SetTime`/`currentFrame` arrived last from any attached index; two indices at different times
are unsupported in v1 (ADR §9 R28) — one context and one generation counter; each attached index
republishes the same generation. Consequences that must not be violated:

* **The prim set is a session property and never differs per index.** The tile count is closed in three
  lines, with no free variable left:

  ```
  nChunksMax    = ceil(maxCurves / USDGEN_CHUNK_SIZE)      # maxCurves = the description's maximum
                                                           # reachable curve count, not the current one
  chunksPerTile = max(1, ceil(nChunksMax / usdGen:tileTarget))
  nTiles        = min(nChunksMax, clamp(ceil(nChunksMax / chunksPerTile), 32, 256))
  ```

  The outer `min` stops a groom of fewer than 32 chunks being split into more tiles than it has chunks
  (ADR §9 R21). At 100 000 curves with the default 512-curve chunk and `usdGen:tileTarget = 64`: 196
  chunks, `chunksPerTile = 4`, **49 tiles of 2048 curves**; 64 tiles is the 1 M-curve figure over its
  1 954 chunks (`design/proposal-performance.md` §5.1; the worked table is `03-execution-engine.md`
  §1.4). That the count is fixed for the session's life, taken from the
  maximum reachable density, is an ASSUMPTION (same proposal, §1 assumption 6) required by S27/S28. And
  because `USDGEN_CHUNK_SIZE` is host config, not authored (ADR §2.3), the **session records the chunk
  size that produced its prim set** and every attached index reuses it; a chunk-size change is a session
  restart, never a re-tile of a live session. `usdGen:tileTarget` is not a node-digest term: an authored
  change routes as `UsdGenDirtyTopology` on the description and republishes the prim set (ADR §9 R21).
  Gate SI-6 asserts two indices on one session enumerate an identical prim set.
* **Density context is explicit, never inferred, and the two contexts are exclusive.**
  `float usdGen:densityScale = 1` applies in the interactive context, `float usdGen:renderDensityScale = 1`
  in the `render` context; neither applies in the other, both default to 1 (ADR §9 R13). Context comes
  from `USDGEN_CONTEXT=render` (batch), `UsdGenImaging_SetContext()` (apps), or a usdview toggle. The
  renderer display name is **not** intent, and `sceneGlobals` carries no interactive flag — verified: the
  token list is `primaryCameraPrim, activeRenderPassPrim, activeRenderSettingsPrim, startTimeCode,
  endTimeCode, timeCodesPerSecond, currentFrame, sceneStateId`
  (`pxr/imaging/hd/sceneGlobalsSchema.h:37-47`). Decimation is by stable id — keep iff
  `UsdGenHash32(curveId, kSaltDensity) < keepFraction * 2^32` — so ids, sculpt deltas and clump ids
  survive a density change and every preview set is a subset of a larger scale's. The salt is non-zero,
  so the surviving set is never `{hairId < scale}`; `hairId` is the zero-salt hash of the same 64-bit
  `curveId` (ADR §9 R12-R13).
* A renderer switch **re-attaches** and continues the generation counter; it never rebuilds the groom.
  Sessions tolerate a `-REM /` followed by a full re-add, and caches are never freed inside a notice
  (S15).

---

## 5. Library decomposition and the dependency rule

### 5.1 Targets

Sibling CMake project at `<usdgen-src>`, consuming the unmodified OpenUSD install and
mirroring usdRig's layout and generated-plugInfo pattern verbatim (S44). The normative target
registry, with the full link and install detail, is `10-build-dependencies-testing.md` §1.2 (§1.3 the
dependency rule, §1.6 the target-to-milestone schedule, §3.4 the install tree, §4.4 the plugInfo
entries); the **boundaries** belong here and the table below states them.

| Target | Kind | Installed | Links | Contents |
|---|---|---|---|---|
| `usdGenMath` | STATIC, PIC, hidden, `-ffp-contract=off` | no (headers not installed before M7) | `arch tf gf vt work` · `TBB::tbb` · `nanoflann` · **`usdGen_seexpr`** · optional `rigExec::rigExecMath` | SoA styler kernels, arc-length/resample, RMF frames, the extent kernel body, 257-entry ramp LUTs, Morton codes, kd-tree facade, Poisson relax, barycentric scatter, and the **noise facade over SeExpr's `Noise.h`/`Noise.cpp`** — SeExpr's noise is the single noise implementation for expressions and C++ stylers (S38, `10-build-dependencies-testing.md` §1.2) |
| `usdGen_seexpr` | STATIC, `-fvisibility=hidden`, PIC | **never** | `dl pthread` | vendored wdas/SeExpr `main`@8f8c8f2; **M0 = `Noise.h`/`Noise.cpp` only**, the interpreter sources land at M4 (S38, `10-…` §1.2, §1.6) |
| `usdGen_ptex` | STATIC, `-fvisibility=hidden`, PIC | **never** | `ZLIB::ZLIB Threads::Threads` | vendored Ptex 2.4.3, the ten `src/ptex/*.cpp` sources compiled by usdGen's own CMake (S38) |
| `nanoflann` | INTERFACE | no | — | pinned 1.12.1 header (S38) |
| `usdGen` | SHARED | yes, `lib/` | `usdGenMath usdGen_seexpr usdGen_ptex` · `arch js plug tf gf vt sdf ts ar work trace hio pxOsd` · `TBB::tbb` (`10-…` §1.3's direct set; `hf`, `pegtl`, `python` and `Python3::Python` arrive transitively) | `UsdGenGraph`, `UsdGenOp` + `UsdGenOpRegistry`, `UsdGenCurveBuffer`, chunk/tile geometry, capture caches, digests, scheduler, operators, maps, `UsdGenMotionCache`, `UsdGenStats`. **No `usd`, `usdImaging` or `hd`** |
| `usdGenImaging` | SHARED | yes, `lib/` | `usdGen usdGenSchema` · `hd hdsi hf usd usdGeom usdShade sdr usdImaging sdf tf gf vt trace pxOsd` | `UsdGenImagingRegistry`, `UsdGenSession`, `UsdGenGroomSceneIndex` + plugin, `UsdGenMetadataSceneIndexPlugin`, the private pruning wrapper, `UsdGenGenerationStore`, `UsdGenTileBuilder`, `UsdGenInstancerBuilder`, `UsdGenLiveOverrideStore`, the adapters, **`UsdGenShadersDiscoveryPlugin`** (the `usdGenShaders` domain's `SdrDiscoveryPlugin`, which is why `sdr` and `usdShade` are linked here and forbidden in `usdGen`), the `extern "C"` ABI (`08-tools.md` §1.4) |
| `usdGenSchema` | SHARED (minimal) + plugin resources, `"Type": "library"` | yes, `lib/` and `lib/usd/usdGenSchema/resources` | `tf usd usdGeom` · `usdGenMath` (a static archive, so the linker pulls only the extent object) | codeless `generatedSchema.usda` + generated `plugInfo.json` (`LibraryPath` -> **`libusdGenSchema.so`**); its only code is schema tokens and `UsdGeomRegisterComputeExtentFunction` for `UsdGenDescription` (`implementsComputeExtent: true`), which a resource-only plugin cannot register. Keeps extents working in `usdcat`/`usdrecord` (ADR §9 R19) |
| `usdGenShaders` | plugin resources, `"Type": "library"` -> `usdGenImaging` | yes, `lib/usd/usdGenShaders/resources` | — | `usdGenHairPreview.glslfx`, `usdGenHairPreviewTranslucent.glslfx`, `usdGenHairPreviewPrimvar.glslfx`, `shaderDefs.usda`, `usdGenHair.mtlx` (ADR §9 R4), plus the `"ShaderResources": "shaders"` key. **Not a resource-only plugin:** a shader-def domain must be discoverable by `info:id`, so its `SdrDiscoveryPlugin` subclass `UsdGenShadersDiscoveryPlugin` is compiled into `usdGenImaging` (`10-…` §1.2, §4.4) |
| `_usdGen` | MODULE (pxr_boost.python), **no LTO** | yes, `lib/python/usdgen/` | `usdGenImaging` · `boost tf gf vt` · `Python3::Module` | the array surface (S39) |
| `usdgen` | python package | yes, `lib/python/usdgen/` | — | all lowercase, mirroring `rigexec` (ADR §9 R5): `_usdGen`, `usdGenLib.py` (ctypes), `usdGenUsdview.py` (the `PluginContainer`), the Qt-free model modules and the `*UI.py` panels, brushes and undo (`08-tools.md` §1.2), `builder.py`, `migrate.py`; pins BLAS threads before importing numpy |
| `usdGenUsdview` | plugin resources, `"Type": "python"` | yes, `lib/python/usdgen/plugInfo.json` | — | registers exactly one type, `usdgen.usdGenUsdview.UsdGenUsdviewContainer`, with `"bases": ["pxr.Usdviewq.plugin.PluginContainer"]` (ADR §9 R5; `08-tools.md` §1.1, `10-…` §4.4). It is **not** a second top-level Python package: the modules live in `usdgen` |
| `usdGenTestUtils` | STATIC | no | `usdGen` | `sampler.h`, `counters.h`, synthetic `UsdGenGraphDesc` builders — **T0 only**; links no `usd`/`hd` and is itself covered by gate B-1 |
| `usdGenTestUtilsHd` | STATIC | no | `usdGenImaging usdGenTestUtils` · `hd hdsi usd usdImaging usdImagingGL hdSt hgiGL usdAppUtils` · EGL via `dlopen` | `sceneFixture.h`, `recordingObserver.h`, `imageDiff.h`, `eglctx.h` (the EGL Storm harness), golden-image compare — **T1/T2/T3 only** |

`UsdGenTileBuilder` is `design/proposal-risk.md` §5.2's `UsdGenChunkPrimBuilder`, renamed for the
chunk/tile split (ADR §1, pinned by ADR §9 R3); it builds one tile prim's container —
`basisCurves/topology`, `primvars`, `extent`, `xform`, `displayStyle`, `primOrigin`, `__dependencies`.
`06-imaging.md` uses the same name.

**The test-utility split is load-bearing, not cosmetic** (`10-…` §1.2): one library that linked Hydra
would drag the whole Hydra/GL stack into every tier-0 executable, make this table's "Links: `usdGen`"
false, and fail gate B-1's link rule as `10-…` §1.4 states it. `usdGenTestUtils` is therefore covered
by B-1 alongside `usdGen` and `usdGenMath`; `usdGenTestUtilsHd` is exempt by construction.

Every name in the Links column is a **CMake target**, not a file name. The install exports one
`add_library(<name> SHARED IMPORTED)` per library — `arch js plug tf gf vt ts ar sdf sdr usd usdGeom
usdShade work trace hf hio pxOsd hd hdsi hdSt hgiGL usdImaging usdImagingGL usdAppUtils boost python`
are the ones named above (all verified in
`$USD/cmake/pxrTargets.cmake`) — whose shared objects are `libusd_vt.so` and
friends. There is no `usd_vt` target, and `target_link_libraries(... usd_vt)` does not resolve.

### 5.2 The dependency rule

**`usdGen` links no `usd`, no `usdImaging`, no `hd`.** That is the point of the `UsdGenGraphDesc` boundary
(§3.1) and it makes S8 a **link-time** property: reach for `UsdStage` in the evaluator and the build
breaks. ADR §9 R37 states the rule as: permitted `tf gf vt sdf ar work trace hio pxOsd` plus what `sdf`
pulls transitively; **forbidden** `usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`,
`usdAppUtils`. The exact direct set is `10-build-dependencies-testing.md` §1.3's — `arch js plug tf gf
vt sdf ts ar work trace hio pxOsd` — with `hf`, `pegtl`, `python` and `Python3::Python` acquired
transitively (`hio` brings `hf`; `sdf` brings `ts;pegtl;python;Python3::Python`). The rule is a
**prohibition, not an allow-list**: a transitive dependency of a permitted library is permitted, and
only the forbidden families are enforced. `sdf` is linked for `SdfPath` and `SdfAssetPath` — values, not
stage access; `ts` for the whole-`TsSpline` transport (ADR §9 R11); `hio` for `HioImage` map reading and
EXR paint output; `pxOsd` for curve topology types and `Far::PtexIndices`. Gate **B-1** (T0, M0) reads
`DT_NEEDED` of `libusdGen.so` and fails on any forbidden entry, and runs the same check on `usdGenMath`
and `usdGenTestUtils` plus an `#include` grep (`10-…` §1.4).

Two more rules of the same kind. **Third-party statics are hidden inside `libusdGen.so`, their headers
and `.so`s never installed next to USD's** — otherwise a site's Ptex-enabled `libusd_hdSt.so` picks ours
up through `$ORIGIN` (`research/A8-seexpr-ptex-libs.md` §6.2). And **`-ffp-contract=off` is a
`usdGenMath` property, not a project-wide flag**: usdRig's curvenet failure proves FMA contraction flips
surface-walking branches (S45, `research/B-usdrig-build.md`); gate E-8 asserts bitwise-identical results
at 1 vs 8 threads.

**Target names and directory layout are fixed at M0**, but a target appears when its first consumer
does. The schedule is `10-build-dependencies-testing.md` §1.6's: **M0** = `usdGenMath`,
`usdGen_seexpr` (**noise sources only** — `Noise.h`/`Noise.cpp`), `usdGen`, `usdGenImaging`,
`usdGenSchema`, `usdGenTestUtils`, `usdGenTestUtilsHd`, `nanoflann`; **M1** = `usdGenShaders`; **M4** =
`usdGen_ptex` and the SeExpr **interpreter** sources added to `usdGen_seexpr`; **M5** = `_usdGen`, the
`usdgen` package, `usdGenUsdview`; **M7** = installed headers for `usdGen`/`usdGenImaging`.

`usdGen_seexpr` is an M0 target precisely **because** `usdGenMath` links it: S38 makes SeExpr's
`Noise.h`/`Noise.cpp` the single noise implementation for expressions and C++ stylers, and
`UsdGenNoise` ships in M1 (ADR §7). Only the interpreter half waits for M4, when `UsdGenExprMap` needs
it; `usdGen_ptex` waits for M4 with `UsdGenPtexMap` (ADR §9 R38). `usdGenMath` **links** the static
archive rather than merely including headers because SeExpr's `Noise.cpp` carries explicit template
instantiations and only for `double`, so usdGenMath calls the `double` instantiations and narrows once
at the buffer boundary (`10-…` §1.2).

### 5.3 The contracts, and when each freezes

| # | Contract | Freezes | Why early |
|---|---|---|---|
| C1 | `usdGen:` property and prim type names | M1 | assets reference them; a rename is a migration |
| C2 | the published tile `basisCurves` contract (ADR §5.3) | M1 | Storm, hdPrman and the tools read it, and it is the freeze format |
| C3 | the curve contract for guides, freezes, imports, sim caches (S42 + `primvars:usdGen:role`) | M2 | one contract, four producers; one migration instead of four |
| C4 | the C ABI + pxr_boost array surface (S39), owned by `08-tools.md` §1.4 | M5 | ctypes has no compile-time check; a signature change breaks silently |
| C5 | glslfx parameter names — the shipped `inputs:` block, verbatim (S35) | M1 | they are `UsdShade` inputs on shipped materials |

Everything else — `UsdGenOp`'s v-table, chunk views, graph headers, imaging internals — is **not
installed** and may churn until M7 (ADR §3). `UsdGenOpRegistry` is internal in v1 and v2; a third-party
operator ABI is v3. The split lets M2-M8 rewrite the engine's guts without touching an artist's stage.

---

## 6. Engine invariants I1-I8

Named **I**, not P, because the motion profiles are P0/P1/P2 (ADR §9 R1). Full treatment in
`03-execution-engine.md`.

| # | Invariant | The measured reason |
|---|---|---|
| **I1** | Per-CV data is **planar SoA** (`VtFloatArray px, py, pz`), interleaved to `VtVec3fArray` once per publish; per-*curve* data stays AoS. | 25.3 -> **80.9 GFLOP/s** for the same kernel (MEASURED, EV-014 and EV-016, `research/G-data-plane-engine-prototype-benchmark.md` §6). GCC 13.3 autovectorises to 128-bit NEON unaided: layout is the lever, hand-written SIMD is not. |
| **I2** | **Chunk = 512 curves, curve-aligned**, one dirty byte per chunk per node; uniform CV count is the fast path, ragged (`cvCount == 0` + `cvOffsets`) is supported in v1. | 128-1024 curves/chunk MEASURED best (1.55-1.89 ms), 4096 a 9.40 ms cliff (EV-009). Ragged is v1 because `UsdGenCurveSource` is ragged by nature; gate E-1r requires ragged <= 2x uniform. |
| **I3** | **Hair chunks never read hair chunks.** Guides, clump centres and card roots are un-chunked *reference* buffers evaluated first; consumers store `guideIdx[3]/guideW[3]`, `clumpId[level]` at capture. | Removes cross-chunk fan-in and makes `tbb::parallel_for` over chunks provably race-free — the failure VDF's 500-element grain has (S21, same report §2). Sized for guides at 10 % of hairs (ASSUMPTION, ADR §9 R26, `research/A7-prior-art-grooming.md` §9.3). |
| **I4** | **Capture / evaluate split**, capture cached by a 128-bit epoch digest, every operator classified `restSpace` or `deformedSpace`. | Motion-blur cost becomes `H + k*T`, not `k*(H+T)` (`research/G-motion-blur-sampling-strategy.md` §5); a frame re-runs only the deformed tail. |
| **I5** | **Engine chunk and Hydra prim are different granularities**, related by an integer `chunksPerTile`; 32-256 tiles per description. | 512-curve chunks at 1 M curves are **1 954** chunks (`ceil(1 000 000 / 512)`; the worked table is `03-execution-engine.md` §1.4) and cannot also be <= 256 prims, which S23 and S27 both demand (ADR §1). Chunking the *index build* is where Storm pays: 2.607 -> 0.659 ms at 32 tiles (MEASURED, EV-028, `research/G-storm-throughput-and-prim-granularity.md` §1.7). |
| **I6** | **Publish is a snapshot swap:** `std::atomic_store` of a `std::shared_ptr<const UsdGenGeneration>`; `VtArray` CoW is the default (S24). A ring buffer is allowed only where a `Vt_ArrayForeignDataSource` detached callback has handed the back buffer back — 26.08 has no public `VtArray` uniqueness query (§4.2, ADR §9 R20). | Publish costs **0.0000-0.0003 ms**, at or below the probe's timer resolution (MEASURED, EV-010, `prototypes/data-plane-benchmark/results_main.txt:95-101`); the next edit pays one detach of 9.6 MB whose size is **disputed and UNMEASURED** — gate **E-1** re-measures it in the arena (§4.2, `03-execution-engine.md` §0.3). The ring exists to move that detach off the interactive path, if it is real. |
| **I7** | **Nothing cooks in `GetPrim`; nothing cooks in every `_PrimsDirtied`.** Deferred commit, atomic publish, diffed dirties. | Eager over-cooks 2-2.9x (29 or 20 cooks for 10 events); lazy cooks on 4 worker threads and emits **zero** notices (MEASURED, EV-038, `research/G-evaluation-scheduling-and-batching.md` §5). |
| **I8** | **A private `tbb::task_arena` capped at 8 workers by default** (`USDGEN_THREAD_LIMIT`, one-shot calibration). | 3.90 ms (1 thread) -> **1.02 ms (8)** -> 1.79 ms (20) on this 10x X925 + 10x A725 host (MEASURED, EV-008, `research/G-data-plane-engine-prototype-benchmark.md` §3.3). The default 20-thread arena is 1.75x slower than the knee (DERIVED from EV-008: ratio of 1.79 and 1.02 ms); isolation stops usdGen stealing Storm's sync workers. |

Three digests carry the invalidation model: a **Merkle structural digest** over
`d(n) = H(type, algorithmVersion, mode, readPhase, space, sorted(input paths),
sorted(reference/map/surface targets), d(inputs...))` — over `usdGen:input` **ancestors**, not namespace
children (ADR §4.2.1) — a **capture epoch**, and a per-parameter **value version**. `usdGen:enabled` and
`usdGen:seed` are deliberately not digest terms (ADR §9 R14): `enabled` is a value edit turning a
topology-preserving operator into a memcpy pass-through, while generators, `UsdGenResample` and
`UsdGenLength` route as `UsdGenDirtyTopology` — recapture downstream and one topology publish, still no
recompile; `seed` bumps the capture epoch. Muting an operator is the commonest A/B action an artist
performs and must not cost a 10-150 ms recapture — a range that is an ASSUMPTION, read off
`design/proposal-performance.md` §5.6/§7.4 by `design/judge-evidence.md` §2.3 item 5 and measured
nowhere; gate E-4 measures the kNN component of a capture.

Dirty routing is a four-hop path with no search in it — **locator -> node -> chunk -> tile -> leaf** —
via `UsdGenDirtyRouter` with typed `UsdGenDirtyBits` and a table keyed on the **full** locator path below
`usdGen/` (never its first two elements, ADR §9 R25), generated per property from `02-schema.md`, with
surface-major chunk order so a surface dirty is a contiguous range.

---

## 7. The render-time path

The claim R2 makes — *one plugin binary, render-time curve generation* — is satisfied by the same
registration under every delegate (S2). Detail is in `06-imaging.md`, `07-look-maps-expressions.md` and
`05-static-curves-and-deformation.md`.

**One registry, two callers.** usdview and `usdrecord` build hdPrman's chain through
`usdImagingGL/engine.cpp:1776-1779` and then `HdRenderIndex::NewForBackendEmulation`, whose constructor
skips the legacy `AppendSceneIndicesForRenderer` because a terminal scene index is already set; the
legacy path calls it from `renderIndex.cpp:205-214`. Both funnel into one
`HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer`
(`research/G-hdprman-and-usdrecord-render-time-chain.md` §1.2), so one all-renderers phase-0 registration
covers both. hdPrman's display names are `"RenderMan RIS"`, `"RenderMan XPU"`, `"RenderMan XPU - CPU"`,
`"RenderMan XPU - GPU"`.

**The private pruning wrapper is mandatory under hdPrman.** hdPrman ships its *own* ext-computation
pruning scene index at phase 0, and an all-renderers phase-0 entry precedes a renderer-specific phase-0
entry under the `Hybrid` policy — so usdGen runs **before** hdPrman's pruning and would see blocked
points without its own wrapper (S3, ADR §9 R34, same report §2.2, §5).

**Motion.** Profiles are P0 single (default; Storm never samples), P1 velocities (opt-in; blocks upstream
velocities) and P2 samples (lazy on the first interval pull or an app preflight), selected by
`usdGen:motion:mode = single | velocities | samples` with `usdGen:motion:sampleCount = 3` (2..16) and
`usdGen:motion:forwardSurfaceSamples = false` (ADR §2.3, S32). hdPrman blurs only `points` and instancer
arrays, requires >= 2 samples and a **constant** array size across the shutter, and truncates to 16
samples (same report §3). `UsdGenMotionCache` is keyed by `(graphGen, surfaceGen, absTime)` with
lerp-and-clamp, so I4's `H + k*T` holds. **Motion samples are never baked into a layer** (S42).

**Material terminals.** One `Material` carries three terminals (S36), and every tile binds to it by hand,
post-flattening — nothing is inherited (S4, ADR §9 R34). Storm's delegate advertises the render contexts
`{glslfx, mtlx}` (`pxr/imaging/hdSt/renderDelegate.cpp:695-707`, the accepted range of ADR §9 R43;
`materialBindingPurpose` is `:701` and the `materialRenderContexts` assignment `:702-707`) and
`HdsiMaterialRenderContextFilteringSceneIndex` keeps the **first** context in the priority order it is
constructed with (`pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45`), so
`outputs:glslfx:surface` should win over plain `outputs:surface`. On that reading the per-delegate
binding override is unnecessary: `USDGEN_STORM_MATERIAL_OVERRIDE` ships **default OFF**, and gate **L-1**
(T2, M0 pre-work, binding at M1) decides whether it and `<Description>/__usdGenRender/material_storm` are
needed at all (ADR §1 on S36, §5.6).

**Curve type.** `type = cubic`, `basis = bspline`, `wrap = pinned`; never `centripetalCatmullRom` (hdPrman
rejects it) and never `bezier`. hdPrman expands pinned curves in its own phase-3 plugin, per shutter
sample.

**Batch.** `USDGEN_CONTEXT=render` selects the render context so `usdGen:renderDensityScale` applies
(§4.5). A stock `usdrecord` needs nothing else: trigger (b) fires from `sceneGlobals/currentFrame`, which
hdPrman reads through `HdUtils::GetCurrentFrame`. Gate R-1 checks that `usdrecord` with 3 motion samples
produces correct blur with `k*tail` evaluations, not `k*chain`.

---

## 8. Deliberately rejected alternatives

Listed so nobody re-opens one without new evidence.

**8.1 hdGp (Hydra generative procedurals).** Four independent disqualifications: **off by default**
(`hdGp/sceneIndexPlugin.cpp:25-27`, `HDGP_INCLUDE_DEFAULT_RESOLVER` default `false`, unset by usdview and
`usdrecord`); when on it lands **downstream** of `HdsiVelocityMotionResolvingSceneIndex`, so generated
curves get no Storm velocity blur; `GetChildPrim` is uncached and it is not chainable; and it cooks
synchronously inside `_PrimsDirtied`, MEASURED at 2-2.9x over-cook (EV-038,
`research/G-evaluation-scheduling-and-batching.md` §5, S6). usdGen may later host its own resolver for
third-party procedurals; it will not be built on one.

**8.2 A VDF/OpenExec data plane.** A persistent `VdfNetwork` is **4x slower** at 100 k curves, **9-15x**
at 1 M, and **3-15x on a sparse edit at 100 k and ~250x at 1 M** (MEASURED: §0 gives the 100 k range, §5
gives 72.0 ms VDF vs 0.29 ms TBB on a 0.5 %-sparse edit at 1 M curves; S21's "3-250x" compresses two
workloads into one range and is split here). It also uses roughly double the RSS (1 263 vs 668 MB for
96 MB of curve data) and *regresses with thread count* (5.72 ms at 20 threads vs 3.53 at 1) — all
MEASURED, EV-011 against EV-006 and EV-007,
`research/G-data-plane-engine-prototype-benchmark.md` §0, §3.3, §5. The structural reasons are
worse than the numbers: VDF splits pool nodes into 500-element invocations that ignore curve boundaries,
and its affects-masks are frozen at schedule time. Separately, the ExecUsd/usdExecImaging control plane
is **not plugin-extensible in 26.08** (hard-coded adapter list, S16). An OpenExec backend behind the same
operator interface stays a v3 option; **curve arrays never go through OpenExec**.

**8.3 Cooking in `GetPrim`.** MEASURED (EV-038) to run on 4 distinct worker threads, and it emits zero
downstream notices, so nothing re-pulls (§4.1). Withdrawn, replaced by trigger (c) (ADR §1, §4.3, §9 R32).

**8.4 Implicit chain wiring.** An operator with no authored `usdGen:input` taking its preceding sibling
was proposed for terse hand-written `.usda`. Rejected: S26 settles wiring as explicit `usdGen:input`, and
the fallback needs `reorder nameChildren` to be both visible through the scene index **and**
invalidating — there is no `PrimsChildrenReordered` notice in Hydra and no report measures it. The stack
editor's drag gesture rewrites `usdGen:input`, one attribute edit per moved node; namespace order is only
the Kahn tie-break. (M0 pre-work item **PW-6** closes the question for the record — `11-roadmap.md` §1;
nothing in the design depends on its result.)

**8.5 Per-frame bakes and authored evaluator output.** Nothing the evaluator computes is authored (R1,
S8). Freezes land at `usdGen:frozen:tier = session | sublayer | payload` — **never `.usda`**, and **never
baked motion samples**; a frozen curve set authors `velocities` instead (S42, ADR §2.3). The corollary:
`UsdStage::RemovePrim` is never on an interactive path, because with an OpenExec system attached it
raises a spurious `Tf.ErrorException` (stock 26.08, the invalid-prim predicate at
`pxr/exec/esfUsd/stageData.cpp:361`); undo of a live freeze is `SetActive(false)`, and real removal
happens only when the entry leaves the undo stack (S41).

**8.6 A GPU-first evaluator.** Rejected for v1-v2 on the ledger, not on taste: the CPU chain is 1.02 ms
(arena-pinned, EV-008) to 1.91 ms (default 20-thread arena, EV-001) of a 15.8-16.0 ms frame, and the
interleaved publish plus upload is another ~1.7 ms. Moving the chain to the GPU could save at most
~1.5 ms (DERIVED from EV-001/EV-008 and `09-performance-and-benchmarks.md` §0.2's interleave and upload
rows; no gate covers it — a v3 GPU tail would need one)
while adding a second memory model, a second determinism story (gate E-8 requires bitwise-identical
results) and a hard dependency on Hgi backends this host cannot build (hgiGL only,
`research/ENVIRONMENT.md`). A GPU tail stays on the v3 list.

**8.7 One prim per curve, and one prim per groom.** Both ends are measured out. At 100 000 prims the
scene-index side alone costs ~210 ms per deform frame before Storm does any work (MEASURED, EV-032; the
1.30 µs/prim fan-out term is EV-031); at 1 prim there is no frustum culling, no parallel index build and
a whole-groom re-upload on any edit
(`research/G-storm-throughput-and-prim-granularity.md` §1.8, §1.13, §2). 32-256 tiles is the measured
window.

**8.8 Fixed-capacity point arrays with a shrinking count.** MEASURED (EV-026) to render the whole prim
fallback-red with only a `TF_WARN`, because the vertex-primvar interpolater accepts a longer array only
when the topology has indices (same report §1.4). The publisher **asserts**
`points.size() == Σ curveVertexCounts` (gate SI-1). During a density drag, element counts are held fixed
by other means and the real count change commits on release (S28).

---

## 9. Testing

Five tiers, in the order a change hits them (ADR §9 R2, `design/proposal-risk.md` §9.1). Gate ids, tiers
and milestones come from `09-performance-and-benchmarks.md` §5 (ADR §9 R40).

| Tier | What runs | Cost |
|---|---|---|
| **T0 engine** | `UsdGenGraph` / `UsdGenGraphDesc` over synthetic buffers, no Hydra and no USD; plus the build checks. Links `usdGenTestUtils` only — the T0 helper that itself links nothing but `usdGen` and is covered by gate B-1 | ms, every commit |
| **T1 scene index** | headless tests over the **real** `UsdImagingCreateSceneIndices` chain plus the renderer-plugin append, with a recording observer; assert data-source contents **and emitted dirty locators**. Uses `usdGenTestUtilsHd` (`sceneFixture.h`, `recordingObserver.h`) | < 100 ms each — the primary suite |
| **T2 Storm** | the EGL harness on the GB10, headless (`eglctx.h` and `imageDiff.h` in `usdGenTestUtilsHd`, carried from `prototypes/storm-hair-look/eglctx.h`); golden images plus GPU frame timing | ~1 s each |
| **T3 app** | `testusdview` under the scratchpad Xvfb (`DISPLAY=:77`, llvmpipe), against `usdGenTestUtilsHd`'s fixtures. **llvmpipe frame times are CPU numbers, never quoted as Storm numbers.** | seconds, pre-merge |
| **T4 workstation** | MSAA/OIT, Metal/Vulkan Hgi, non-NVIDIA drivers, a real hdPrman install, 4K interactive. **Release criteria `RC-n` only, never milestone exits** (ADR §7, §9 R39). | manual |

| Claim | Proved by | Tier | Milestone |
|---|---|---|---|
| §0.3 chain cost, sparse edit, last-param edit, thread scaling, determinism, memory | E-1, E-2, E-7, E-8; E-1r, E-3; E-5 | T0 | M1; M2; M3, re-run each milestone |
| §6 kNN capture cost the operator schedule assumes | E-4 (100 k / 1 M rest roots, 8 threads) | T0 | M0 pre-work (PW-1), binding at M3 |
| §0.3 Storm draw at 100 k, replacing the derived number; the 1080p baseline recorded separately | S-1 at 1280x720 **and** 1920x1080; S-9 for the refineLevel switch cost | T2 | S-1 M1; S-9 M0 pre-work (PW-3), binding at M1 |
| §4.2 the disputed CoW detach, re-measured inside the private arena | E-1 (full run with an outstanding reference) | T0 | M1 |
| §2.1-2.3 chain placement (after UsdSkel/RigExec, before Storm's plugins) | SI-5, asserting the resolved chain dump, not the tag | T1 | M0 proof, binding at M1 |
| §2.4 pickup through the private wrapper; the bare-`primvars` rule; wrapper cost | `testUsdGenSkelInterop` under SI-2; SI-9 | T1 | M2 |
| §3.1-3.2 every declared property appears **and** dirties, mode-off parameters included | SI-7 | T1 | M1 |
| §3.1 auto-applied `UsdGenMaskAPI` on a codeless type | SI-8 | T1 | M0 pre-work (PW-5), binding at M1 |
| §4.1 exactly one commit per interactive event batch, on the commit thread | SI-3 | T1 | M1 |
| §4.2 no torn reads under concurrent `GetPrim` | SI-4 (8 readers x 100 publishes) | T1 | M1 |
| §4.3 exact invalidation: a clump edit dirties only `primvars/points/primvarValue` on the dirty tiles | SI-2 | T1 | M1 (**stop condition**: if precise `usdGen:*` invalidation cannot be shown, fall back to `primvars:usdGen:*` and re-plan) |
| §4.3 `hairTangent` strategy and the points fastpath — and with it `09-…` §0.2's precondition, which §0.3 quotes and which assumes variant A | S-8 (A vs B at 100 k deforming, plus `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`) | T2 | M0 pre-work (PW-2), binding at M1 |
| §4.4-4.5 population by both paths; one session across two indices and a renderer switch; the tile arithmetic (`nTiles` from `nChunksMax` and `usdGen:tileTarget`; a live session refuses a chunk-size change) | SI-6 and its extensions + a renderer-switch script | T1/T3 | M1, M7 |
| §5.1 every Links cell names a real CMake imported target; `usdGenTestUtils` and `usdGenTestUtilsHd` are two targets | the M0 configure: a bad target name fails `cmake` before a compile runs | T0 | M0 |
| §5.2 the dependency rule | **B-1**: `DT_NEEDED` of `libusdGen.so`, `usdGenMath` and `usdGenTestUtils` against the forbidden families, plus the `#include` grep (`10-…` §1.4) | T0 | M0 |
| §6 I5 tile granularity and the exact-size assert | SI-1, S-5, S-6; S-3 | T1/T2 | M1; M2 |
| §6 I8 arena knee | E-7 plus the one-shot calibration self-test | T0 | M1 |
| §7 render-time parity | R-1 (`usdrecord`, 3 motion samples); T-INST-1/2 | T4 / T1 | R-1: release criterion `RC-n`, content in M7 — a T4 gate is never a milestone exit (ADR §7, §9 R39); T-INST-1/2: M6 |
| §7 Storm material terminals; whether `USDGEN_STORM_MATERIAL_OVERRIDE` flips from default OFF | L-1 (does Storm resolve `outputs:glslfx:surface` first) | T2 | M0 pre-work (PW-4), binding at M1 |

---

## 10. Out of scope

* Operator parameters, mask arithmetic, ramp semantics, the v1/v2/v3 catalogue — `04-operators.md`.
* Prim type and property tables, `.usda` examples, versioning, freeze/sculpt schema — `02-schema.md`
  (the single normative property registry, ADR §9 R7).
* The `UsdGenOp` interface, chunk views, capture caches, digest formats, eviction, memory budget —
  `03-execution-engine.md`.
* The tile contract's data-source list, generation store, live overrides, instancer synthesis,
  `primOrigin` rules — `06-imaging.md`.
* glslfx and MaterialX names, colour baking, Ptex face mapping, SeExpr variables, map reload —
  `07-look-maps-expressions.md`.
* Panels, brushes, undo, hotkeys, and the C ABI plus pxr_boost signatures of contract C4 —
  `08-tools.md` (§1.4 is the single source of the C ABI, ADR §9 R31).
* The frame ledger and every per-scale cost model — `09-performance-and-benchmarks.md` §0.2, the
  **only** ledger in the plan (ADR §9 R41). The gate registry — `09-…` §5 (ADR §9 R40).
* CMake specifics, vendoring, the target registry and its per-milestone schedule, the env-var registry,
  test runners — `10-build-dependencies-testing.md` (§1.2, §1.6, §3.5; ADR §9 R35).
* Milestone contents, estimates, staffing, the PW-1…PW-6 pre-work definitions — `11-roadmap.md`.
* The `EV-nnn` rows themselves, the probe outputs and the full `file:line` index —
  `appendix-A-evidence-ledger.md` (ADR §9 R43).
* A DCC bridge, a third-party operator ABI, a GPU evaluator, progressive generation, any usdRig source
  change — v3 or out of the plan.

---

## 11. Sources

**Design.** `design/adr-v1.md` §1 (amendments to S1, S11, S18(c), S23/S27, S24, S29, S31, S36), §2.1-2.3,
§3, §4.2-4.5, §5.1-5.6, §6, §7, §8; §9 rulings R1-R7, R11-R16, R19-R23, R25-R32, R34, R35, R37-R43, R45.
`design/brief-v1.md` §1 (R1-R9), §2 (S1-S46), §3 (D1-D8).

**Proposals.** `design/proposal-risk.md` §0, §2, §4.3-4.6, §5.1-5.5, §9.1.
`design/proposal-performance.md` §0.1-0.3, §1, §3.1-3.3, §5.1, §5.3, §5.5-5.6, §6.1-6.3, §11.2.
`design/proposal-artist.md` §0, §2.

**Judges.** `design/judge-evidence.md` §0, §2.1-2.4, §4. `design/judge-delivery.md` §3 weakness 4, §4.1,
§5. `design/judge-artist.md` §3.

**Research.** `research/ENVIRONMENT.md` (host facts; its CORRECTIONS block overrides the text above it);
`research/G-chain-order-probe.md` §2-§6; `research/G-evaluation-scheduling-and-batching.md` §2-§5, §9;
`research/G-data-plane-engine-prototype-benchmark.md` §0, §2, §3.3-3.4, §4-§6;
`research/G-storm-throughput-and-prim-granularity.md` §1.4, §1.7-1.9, §1.13, §2;
`research/G-storm-hair-look-prototype.md` §5; `research/G-stage-free-parameter-and-time-transport.md`
§1-§5 and key facts; `research/G-hdprman-and-usdrecord-render-time-chain.md` §1.2, §2.2, §3, §5;
`research/G-motion-blur-sampling-strategy.md` §5; `research/G-freeze-bake-undo-and-frozen-reentry.md`
§2.2; `research/G-instancing-cards-archives-and-native-instances.md` §2, §6;
`research/G-tool-loop-array-transport-and-cv-picking.md` §1.4; `research/A2-usdrig-imaging.md` §3.4, §5;
`research/A3-usdrig-tools.md` §6 (the conventions this file follows); `research/A7-prior-art-grooming.md`
§9.3; `research/A8-seexpr-ptex-libs.md` §6.2; `research/B-usdrig-build.md`.

**Prototypes.** `prototypes/chain-order/`, `prototypes/evaluation-scheduling/`,
`prototypes/data-plane-benchmark/` (incl. the carried raw log `results_main.txt`),
`prototypes/storm-hair-look/` (incl. `eglctx.h`),
`prototypes/storm-throughput/`, `prototypes/stage-free-transport/`, `prototypes/freeze-bake/`,
`prototypes/instancing/`, `prototypes/tool-loop/`, `prototypes/thirdparty-bench/`,
`prototypes/usdrig-linux-build/`. Carry-in status: `appendix-B-prototype-inventory.md`.

**Siblings this document defers to** (each section re-read while reconciling):
`09-performance-and-benchmarks.md` §0.2 (the only frame ledger), §2.5 (the draw fit and `N_max`), §5
(the gate registry), §9 (the resolution rule); `10-build-dependencies-testing.md` §1.2 (the target
registry), §1.3 (the direct link set), §1.4 (gate B-1), §1.6 (target -> milestone), §3.4 (install tree),
§4.4 (the plugInfo entries); `08-tools.md` §1.1 (the `usdgen` package and the `usdGenUsdview` plugin
type), §1.2, §1.4 (the C ABI); `03-execution-engine.md` §0.3 (the disputed CoW detach), §1.4 (the tile
arithmetic and the 1 954-chunk figure); `11-roadmap.md` §1 (PW-1…PW-6), §2.1 (M0);
`appendix-A-evidence-ledger.md` §2 (every `EV-nnn` cited above), §2.11 (the DERIVED and ASSUMPTION
provenance), §3.6 (the Storm material, fastpath and upload facts).

**OpenUSD 26.08** (`<openusd-src>`, tag v26.08; the install built against is
`$USD`, whose `cmake/pxrTargets.cmake` defines §5.1's imported target names).
Every API name and line range above was re-grepped while writing this. Six the architecture would be
invalid without: `usdImaging/sceneIndices.cpp:240, :282, :287, :295, :298, :302` with
`usdImaging/niPrototypePropagatingSceneIndex.cpp:204` (propagation and flattening precede the plugin
band); `hd/renderIndex.cpp:205-214` (the renderer-level append, display-name guard at `:208`);
`hd/mergingSceneIndex.cpp:173-244` (`InsertInputScenes` replays `PrimsAdded`);
`hdSt/basisCurves.cpp:932-935` (the points fastpath condition) with
`hd/changeTracker.cpp:958-979` (`DirtyPrimvar`'s six-name exclusion set);
`usdImaging/sceneIndexPlugin.h:81-83, :90-92` (the two non-const virtuals); and
`base/vt/array.h:39-51, :945, :984, :1023` (the foreign-data-source hook; no public uniqueness query).
The complete file:line index is `appendix-A-evidence-ledger.md`.
