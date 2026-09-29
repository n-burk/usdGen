# usdGen Schema

Date: 2026-09-04. Status: plan v1 (draft, pending review).

> **Superseded in part, 2026-09-15.** `libs/usdGenSchema/schema.usda` is the
> source of truth for the property registry, and `docs/freezes/C1.md` is its
> frozen mirror; where this document and the schema disagree, the schema wins.
> At the user's direction the registry was cut back to the properties the
> engine actually reads. The following were removed on 2026-09-15 together
> with the code behind them, and the sections below that still describe them
> are stale: on `UsdGenGroom` `usdGen:surface`, `densityScale`,
> `renderDensityScale`, `schemaVersion`, `label`; on `UsdGenDescription`
> `terminal`, `densityScale`, `renderDensityScale`, `motion:mode`,
> `motion:sampleCount`, `motion:forwardSurfaceSamples`, `pickTarget`, `label`;
> on `UsdGenGuideSet` `surface`, `blend`, `label`; on `UsdGenOperator`
> `blend`, `space`, `readPhase`, `surface`, `algorithmVersion`, `label`; on
> `UsdGenMap` `label`; on `UsdGenScatter` `mode`, `spacingU`, `spacingV`,
> `jitter`, `rootPrims`, `rootUVs`, `relaxIterations`, `areaCompensation`,
> `guides`, `perGuide`; on `UsdGenGrow` `directionPrimvar`, `uvBlend`,
> `length:source` (and the `attribute` direction token); on
> `UsdGenGuideInterpolate` and `UsdGenLength` `length:source`; on
> `UsdGenCurveSource` `lane`; on `UsdGenWidth` `taper`, `taperStart`,
> `rootScale`, `tipScale`; on `UsdGenDeform` `mode`, `twistAware`,
> `preserveShape`, `preserveShape:iterations`. See `docs/freezes/C1.md` §9.


This document specifies the USD object model of usdGen: every prim type, every property with its
type and default, the reserved namespace layout, the curve contract shared by guides/freezes/
imports/sim caches, the dirty class of every property, and how the codeless schema is generated,
registered and evolved. It is **the single normative property registry** for the whole plan — names,
types, defaults, allowed tokens and dirty class (ADR §9 R7); every sibling document conforms to it,
and where a sibling needs a property this document lacks, this document gains it. It is the
normative source for contract **C1** (`usdGen:` property names and prim type names, frozen at the
end of milestone **M1**, ADR §3) and for contract **C3** (the curve contract, frozen at the end of
**M2**). Everything an artist reads, diffs, hand-edits or version-controls is defined here; nothing
in this document is authored by the evaluator (brief requirement R1 — the `R`-numbers of
`design/brief-v1.md` §1, which are a different namespace from ADR §9's rulings `R1–R45`; this
document writes the latter as "ADR §9 R<n>" always).

Reads with: `01-architecture.md` (the five nouns and the contract table), `03-execution-engine.md`
(how the dirty classes of §6 become `UsdGenDirtyBits` and how the three digests are computed),
`04-operators.md` (the kernels and the capture/evaluate split behind §2.6–§2.8's rows),
`05-static-curves-and-deformation.md` (freeze/sculpt behaviour), `06-imaging.md` (the adapters and
the published-prim contract C2), `07-look-maps-expressions.md` (`UsdGenLookAPI` bake order and the
map evaluators), `08-tools.md` (what the tool authors), `10-build-dependencies-testing.md`
(the `usdGenSchema` library target), `09-performance-and-benchmarks.md` §5 (the gate registry),
`11-roadmap.md` (milestone exits), `12-risks-decisions-open-questions.md` (the deviations §2.14 and
§2.20 file), `appendix-A-evidence-ledger.md`.

---

## 0. Design rules

These rules explain every shape decision in §1–§8. They are binding; a proposal that
contradicts one of them was rejected by the panel and must not be reintroduced.

Every number below carries one tag — MEASURED, `DERIVED from <row>`, UNMEASURED (with its gate) or
ASSUMPTION (ADR §9 R42). A MEASURED number cites its `appendix-A-evidence-ledger.md` row by that
row's `EV-nnn` handle, which is the only citation handle for a measurement (ADR §9 R1, R43). Gate ids are
`09-performance-and-benchmarks.md` §5's, which is the single gate registry (ADR §9 R40).

### 0.1 Codeless schemas, no generated C++

Every `UsdGen*` prim type and API schema is a **codeless** schema (`skipCodeGeneration = true`,
S9). The domain ships as two data files — `generatedSchema.usda` and `plugInfo.json` — and no
schema C++ class exists anywhere in the project. This is proven, not assumed: a codeless plugin
declaring `UsdGenProbeOperator`/`UsdGenProbeClumpStyler`/`UsdGenProbeImageableOp` gave working
`GetPrimTypeInfo().GetSchemaTypeName()`, `IsA()`, fallbacks and derived-type queries with zero C++
(MEASURED, `research/G-stage-free-parameter-and-time-transport.md` §1;
`prototypes/stage-free-transport/plugin/usdGenProbeSchema/resources/`).

The consequence that pays for it: `UsdPrimDefinition::GetPropertyNames()`
(`pxr/usd/usd/primDefinition.h:38`), `GetSpecType()` (`:290`) and
`GetAttributeDefinition(name).GetTypeName()` (`:277`) are enough to build the imaging adapter's
property mappings **generically** at startup. Use `GetAttributeDefinition`, never the older
`GetSchemaAttributeSpec()`: 26.08 marks that spelling `\deprecated`
(`pxr/usd/usd/primDefinition.h:301-307`). The probe measured the deprecated spelling; the
non-deprecated one returns the same type name. Adding an operator type is therefore a schema entry
plus a kernel — zero imaging code, zero UI code (S10; ADR §2.3; `research/A4-openusd-hdgp-adapters.md`
§2.1).

### 0.2 One prim type per operator *concept*, plus `usdGen:mode`

`UsdGenScatter` carries `uniform token usdGen:mode = "random"` with allowed tokens
`random | uniform | points | atGuides` instead of four sibling types (ADR §2.1). Switching a
scatter from Poisson to at-guides is then one **attribute edit** (structural, but on the same prim),
not a prim delete plus create — which
would be a resync, a rewire of `usdGen:input`, a lost mask block and a walk into the
`RemovePrim`-under-OpenExec defect (`pxr/exec/esfUsd/stageData.cpp:361`, S41, MEASURED
`research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4). a host groomer ships **one** Generator whose
`Generate Primitives` attribute selects "Randomly across the surface" / "In uniform rows and
columns" / "At specified locations" / "At guide locations", for the same reason
(`research/A7-prior-art-grooming.md` §1.1). A7 §9.1's four separate `Scatter*` operators are the
pre-ADR shape; ADR §2.1 collapsed them into `UsdGenScatter` + `usdGen:mode`.

The rule generalises: a `usdGen:mode` token is preferred over sibling types whenever the variants
share their whole property set and differ only in the kernel branch. It is *not* used where the
property sets genuinely differ (a `UsdGenClump` is not a `UsdGenNoise` with a token).

### 0.3 Chain order is the composed hierarchy

**Corrected 2026-09-15.** Chain order is the composed hierarchy under the Description's `Ops`
scope, walked in reverse sibling post-order (bottom sibling first), exactly as
`plan/14-hierarchy-cuda-implementation.md` §1 and `plan/15-resource-aware-execution.md` describe
and as `schema.usda` states. There is no `rel usdGen:input` in the schema and there never was one
in the implementation: both graph-desc builders derive every edge from composed sibling order. The
Kahn topological sort with namespace-order tie-break (S26) still runs, but over that derived linear
chain. `usdGen:terminal` is likewise gone: the terminal is the last node of the derived order.

`HdSceneIndexObserver` has no reorder notice at all — its four virtuals
are `PrimsAdded`, `PrimsRemoved`, `PrimsDirtied`, `PrimsRenamed`
(`pxr/imaging/hd/sceneIndexObserver.h:125-153`, verified) — and whether a `reorder nameChildren`
edit produces *any* Hydra invalidation is **UNMEASURED**; the M0 pre-work notice check closes the
question (ADR §7). Nothing here depends on the answer, because the adapter maps *properties*, not
sibling order. The stack editor's drag gesture therefore has to
`reorder nameChildren` (`08-tools.md`); the pre-2026-09-15 text here described a
`usdGen:input` rewrite that no code ever performed.

### 0.4 `usdGen:enabled`, and why it is never a digest term

The enable flag is `bool usdGen:enabled = true`. It is never `usdGen:active`: USD prim `active` is
metadata that S41 makes load-bearing (`undo of a live freeze = SetActive(false)`), and two flags
named "active" on one prim, one of which removes the prim from composition, is a bug generator
(ADR §2.1).

For **topology-preserving** operators (every `UsdGenStyler` except `UsdGenResample` and
`UsdGenLength`, every `UsdGenDeformer`, `UsdGenSculptLayer`) `usdGen:enabled = false` is
**non-structural**: the node stays in the graph and becomes a memcpy pass-through — no recompile,
no re-capture, only the tail re-runs (ADR §9 R14). Muting is the most frequent A/B gesture an
artist makes, and putting it in the structural digest would throw away that node's kd-trees, clump
ids and map samples — ASSUMPTION, **10–150 ms** of capture per gesture (`design/judge-artist.md`
§2.1, reading `design/proposal-performance.md` §5.6/§7.4; `appendix-A-evidence-ledger.md` §2.11
row "Capture cost per enable/disable gesture"; ADR §9 R42). This is the plan's figure and every
sibling uses it — `04-operators.md` §0.4, `06-imaging.md` §5, `12-risks-decisions-open-questions.md`
X-07. `design/judge-artist.md` §3 Performance #2 quotes a separately cited worst case of ~300 ms at
1 M curves; that is a worst case, not the plan's figure. Gate **E-4** (M0 pre-work, ADR §7) measures
the nanoflann kNN capture that dominates it.

For **generators**, for `UsdGenResample` and for `UsdGenLength` — whose **static**
`TopologyEffect()` is `MayChangeCurveCount` because cull mode exists, so the classification does
not depend on the current `usdGen:length:mode` (ADR §9 R14) — muting may change the curve or CV
count: the node's topology contribution vanishes and every downstream buffer changes size.

Even there the flag is deliberately **not** a term of the Merkle structural digest — ADR §4.2.1:
"`enabled` and `seed` are **not** in the structural digest". A mute must not change a node's
identity, and a muted generator **keeps its own capture**, so ids survive mute/unmute (ADR §9 R14).
The router instead raises `UsdGenDirtyTopology` (unscoped, ADR §9 R3; `03-execution-engine.md`
§5.1) on that node: the topology generation bumps, downstream captures and buffer allocations
re-run, and neither the digest nor the compiled graph shape changes. §6 calls that class
**structural (topology)** to keep the two apart.

### 0.5 Reserved scopes

A `UsdGenDescription` has five reserved namespace children, each a `Scope`: `Ops`, `Guides`,
`Maps`, `Prototypes`, `Frozen` (ADR §2.2). Nothing forces an artist to use them for correctness —
wiring is by relationship — but the tool always authors into them, the panels group by them, and a
`.usda` diff reads as a groom because of them. §3 is normative.

### 0.6 `__usdGenRender` is Hydra-only

`<Description>/__usdGenRender/…` is the reserved path prefix for prims usdGen **synthesizes into
the scene index**. It is never authored on a stage, never appears in a `.usda` file, and any
authored prim under that prefix is ignored with one `TF_WARN`. This mirrors usdRig's
`__RigExecGenerated` (`research/A2-usdrig-imaging.md` §5) and keeps the prim browser showing only
what the artist made. The full synthesized set is §3.2.

### 0.7 Naming conventions

| Rule | Value |
|---|---|
| Prim type prefix | `UsdGen…`; the schema `libraryPrefix` is `UsdGen` |
| Property namespace | `usdGen:` on every schema-declared attribute and relationship |
| Grouped parameters | second and third namespace levels: `usdGen:clump:size`, `usdGen:mask:ramp:knots`. `UsdGenPrimAdapterBase` (ADR §9 R3) splits each schema property name on `:` and hands `UsdImagingDataSourceMapped` an `HdDataSourceLocator` of that many elements — the locator is supplied by the client, the class does not split names (`pxr/usdImaging/usdImaging/dataSourceMapped.h:51-63`: "Has to be non-empty. If length is greater than one, nested container data sources will be created."). The class then builds the nested containers and stores the absolute locator, so invalidation is 1:1: `usdGen:mask:ramp:knots` → `usdGen/mask/ramp/knots`. MEASURED for the two-level case (`research/G-stage-free-parameter-and-time-transport.md` §2: containers `clumpRadius inputs map ramp surface`, nested `ramp/{knots,values}`, and `Invalidate({usdGen:ramp:knots, usdGen:surface})` → `usdGen/ramp/knots`, `usdGen/surface`); three-level nesting is the same code path and is asserted by gate SI-7. This is exactly the granularity the dirty router keys on (§6) |
| Relationships (the complete set §2 declares; gate SI-7 checks it) | `usdGen:input`, `usdGen:surface`, `usdGen:terminal`, `usdGen:guides`, `usdGen:curves`, `usdGen:reference`, `usdGen:frozen:curves`, `usdGen:prototypes`, `usdGen:colliders`, `usdGen:mask:source`, `usdGen:mask:region`, `usdGen:clump:centers`, `usdGen:length:source`, `usdGen:displace:map`, `usdGen:look:colorMap`, `usdGen:expr:maps`, `usdGen:combine:inputs`, `usdGen:paint:surface` |
| Primvars usdGen owns | `primvars:usdGen:<name>` is used in exactly two places: the C3 curve contract (§5) and the paint-brush surface primvars a `UsdGenPaintMap` reads (§2.12). Operator *parameters* are never `primvars:` — that form is the M1 stop-condition fallback only (ADR §7) |
| Booleans | positive sense, `enabled` not `disabled`; default `true` where the absence of an opinion means "on" |
| Tokens | lowerCamelCase values, always with `allowedTokens` authored so the tool can build a combo box from `UsdPrimDefinition::GetAttributeDefinition(name)` (`pxr/usd/usd/primDefinition.h:277`; the older `GetSchemaAttributeSpec()` is deprecated, `:301-307`) |
| Ramps | `<p>:knots` + `<p>:interpolation` (scalar) or `<p>:positions` + `<p>:colors` + `<p>:interpolation` (colour); the interpolation token set is `linear \| catmullRom \| bspline \| constant`, default `"catmullRom"` (ADR §9 R11). A `.spline` on a ramp's knots, positions or colours is a compile error; the dedicated `float <p>:spline` of §2.17 row 3 is the one legal spline inside a ramp |
| Uniformity | `uniform` for anything that must not animate (mode tokens, seeds, versions, counts); plain for anything an artist may key |

### 0.8 Units

`usdGen:density` is **hairs per square stage unit measured on the rest surface**. The doc string of
the property says exactly that (ADR §2.3). Two consequences the implementation must honour:

* Face area comes from `usdGen/rest/points` (§2.15), never from the deformed points, so a character
  that stretches during a shot does not grow hair.
* A non-uniform object scale does not change the count: usdGen reads post-flattening world matrices
  (S4) but computes rest area in the surface's own local space. A show that wants counts to follow
  scale scales `usdGen:density`.

Lengths (`usdGen:length`, `usdGen:clump:size`, widths, noise `magnitude`) are in **stage units**;
the `.usda` files in §4 declare `metersPerUnit` so the numbers are readable. Angles are in
**degrees**. Ramp parameter axes are normalised root→tip `t ∈ [0,1]` unless a `rangeMode` token
says `absoluteLength`.

### 0.9 What deliberately is not in the schema

`chunkSize`, `threadLimit` and `memoryBudgetMB` are **environment/config**
(`USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`, `USDGEN_MEMORY_BUDGET_MB`), never authored into an
asset (ADR §2.3). `10-build-dependencies-testing.md` §3.5 owns the **single env-var registry** (ADR
§9 R35); this document names an env var only where a schema decision turns on it and introduces
none of its own. A workstation's thread count committed into a character asset is a bug that
outlives the workstation. The one machine-shaped number that *is* authored is
`uniform int usdGen:tileTarget` (§2.3), because the published Hydra prim set must be reproducible
across sessions and hosts.

There is also no interactivity flag on the stage and none is read from Hydra:
`HdSceneGlobalsSchema` in this install carries exactly `sceneGlobals, primaryCameraPrim,
activeRenderPassPrim, activeRenderSettingsPrim, startTimeCode, endTimeCode, timeCodesPerSecond,
currentFrame, sceneStateId` (`pxr/imaging/hd/sceneGlobalsSchema.h:37-47`, verified). Render intent
is explicit (§2.3.2).

---

## 1. Type hierarchy

Exactly as ADR §2.1. Abstract types are marked *(abstract)*; everything else is concrete. Every
type is codeless.

```
UsdGeomImageable
 ├─ UsdGenGroom              session root; artist-facing globals only
 ├─ UsdGenDescription        : UsdGeomBoundable
 └─ UsdGenGuideSet           a named guide set; children are BasisCurves under contract C3
UsdTyped
 ├─ UsdGenOperator (abstract)
 │   ├─ UsdGenGenerator (abstract)
 │   │   ├─ UsdGenScatter          mode = random | uniform | points | atGuides
 │   │   ├─ UsdGenGrow
 │   │   ├─ UsdGenGuideInterpolate
 │   │   └─ UsdGenCurveSource
 │   ├─ UsdGenStyler (abstract)
 │   │   └─ UsdGenClump · UsdGenNoise · UsdGenCurl · UsdGenBend · UsdGenDirection ·
 │   │      UsdGenLength · UsdGenWidth · UsdGenSmooth · UsdGenStraighten ·
 │   │      UsdGenDisplace · UsdGenWave · UsdGenScale · UsdGenResample ·
 │   │      UsdGenSculptLayer · UsdGenPart · UsdGenExprOp
 │   ├─ UsdGenDeformer (abstract)
 │   │   └─ UsdGenDeform · UsdGenCollide · UsdGenWind
 │   ├─ UsdGenFreeze
 │   └─ UsdGenInstance
 └─ UsdGenMap (abstract)
     └─ UsdGenImageMap · UsdGenPtexMap · UsdGenExprMap · UsdGenPaintMap ·
        UsdGenNoiseMap · UsdGenCombineMap · UsdGenGuideProximityMap
API schemas (single-apply):
    UsdGenMaskAPI   auto-applied to UsdGenOperator
    UsdGenLookAPI   applied to UsdGenDescription
    UsdGenRestAPI   applied to a bound surface Mesh
    UsdGenCurveAPI  applied to C3 curves (guides, freezes, imports, sim caches)
```

Release tiers are ADR §6 and are shown per type in §2.6–§2.12. **Every** type above, v1 through v3,
is declared in `schema.usda` from M1 so C1 freezes the whole name space once; a declared but
unimplemented type publishes nothing and raises one `TF_WARN` per prim (§8.2).

**`UsdGenGroom`.** One per character or asset, and the session root: the registry keys a session by
(stage or `usdGen:sessionId`, groom root) (S15, ADR §4.5). It carries only artist-facing globals —
default surface, the two density scales, schema version, session id — and no machine tuning (§0.9).
It is `UsdGeomImageable`, but because usdGen runs post-flattening (S4) and authors its tiles by
hand, `visibility` does **not** descend on its own: `usdGenImaging` reads the Description's
flattened `visibility` — which composition has already resolved against this Groom's — and writes it
onto every published tile and guide prim (`06-imaging.md` §4.1, "inherited by hand from the
description"). Hiding a groom therefore hides its hair, but only through that explicit rule.

**`UsdGenDescription`.** The artist's "object" — one operator chain, one bound surface set, one
material, one published Hydra prim set. It derives from `UsdGeomBoundable`, not merely
`UsdGeomImageable`, so a `UsdGeomComputeExtentFunction` can be registered for it and "select the
description, press F" frames the hair: `UsdGeomBBoxCache` ignores Hydra entirely
(`research/A2-usdrig-imaging.md` §6), so a Hydra `extent` on the tiles is not enough. The
registration is `UsdGeomRegisterComputeExtentFunction(TfType::FindByName("UsdGenDescription"), fn)`
called from **`libusdGenSchema.so`** — a minimal library linking only `usd`/`usdGeom` (ADR §9 R19) —
plus `"implementsComputeExtent": true` in the *generated* schema plugInfo
(`pxr/usd/usdGeom/boundableComputeExtent.cpp:155-173, 271-289`; §7.1–§7.2).

**`UsdGenGuideSet`.** A named set of guides; its **namespace children** are `UsdGeomBasisCurves`
prims under contract C3 (§5). It is `UsdGeomImageable`, so `purpose = "guide"` hides guides from a
render while keeping them in the viewport. It is not an operator — no `usdGen:input`, no place in a
chain; operators reach it through `rel usdGen:guides`.

**`UsdGenOperator` (abstract).** Everything with `usdGen:input`. It owns the common vocabulary
(§2.5) and, through auto-apply, the mask block (§2.13); a concrete type adds only its own
parameters. The imaging adapter is registered on this abstract type with
`includeDerivedPrimTypes: true`, so a new derived type needs no imaging entry
(`pxr/usdImaging/usdImaging/adapterRegistry.cpp:130-150, 196-210`).

**`UsdGenGenerator` (abstract).** Operators that may change the curve count or the CV count.
Their `usdGen:enabled` is structural (topology) — it re-allocates without touching the Merkle
digest (§0.4, §6.2). `UsdGenScatter` emits roots on the rest surface with
stable ids; `UsdGenGrow` turns roots into straight strands and sets the CV count; `UsdGenGuideInterpolate`
builds strands by blending guides and stores `guideIndex[3]`/`guideWeight[3]` at capture;
`UsdGenCurveSource` reads a stage `BasisCurves` (a freeze, an import, an Alembic-converted sim
cache) straight out of the scene index into the graph — this is the v1 entry point for brief requirements R3 and R4's
"rigged or simulated curves".

**`UsdGenStyler` (abstract).** Topology-preserving operators, with `UsdGenResample` (CV count) and
`UsdGenLength`'s cull mode as the declared exceptions. `usdGen:enabled` is non-structural for the
rest (§0.4).

**`UsdGenDeformer` (abstract).** Deformed-space operators, re-run once per motion sample when the
motion profile is `samples` (S25, S32). `UsdGenDeform` transports rest curves through the animated
surface frame; `UsdGenCollide` and `UsdGenWind` are v3 and need deformed-space spatial structures.

**`UsdGenFreeze`.** Caps the chain at a snapshot: `usdGen:frozen:mode = "frozen"` reads the frozen
`BasisCurves` and does not evaluate upstream; `"live"` passes the input through and keeps the
snapshot as a stale artefact. Upstream operators stay authored and stay in the stack, greyed —
a host groomer's "Groom Bake deactivates all modifiers below it", reversibly, by one token
(`research/A7-prior-art-grooming.md` §9.3).

**`UsdGenSculptLayer`.** Per-CV deltas in the root frame keyed by stable `curveId`, at a layer
weight, so hand-comb work survives an upstream tweak that keeps ids. It derives from `UsdGenStyler`
because it is topology-preserving and must mute like one.

**`UsdGenInstance`.** The only operator that does not emit curves: it emits a Hydra `instancer`
whose prototypes are the `Prototypes` scope's prims, re-rooted as namespace children of the
instancer (S33). `uniform token usdGen:primitive = "cards"` selects `cards | archives | spheres`.
**v1, milestone M6** (ADR §9 R38).

**`UsdGenMap` (abstract).** A scalar or colour field evaluated on the **CPU at capture time** and
baked to per-curve or per-CV arrays (S37). Maps are not operators: they have no `usdGen:input`, no
`readPhase` and no `space`. They are referenced by relationship from operator parameters.

Purpose of each concrete operator and map type, one line each; parameters are §2.6–§2.12 and
kernels are `04-operators.md`.

| Type | Tier | Purpose |
|---|---|---|
| `UsdGenScatter` | v1 | Emit roots on the rest surface with stable ids, by density, by parametric grid, from an authored point list, or one per guide root. |
| `UsdGenGrow` | v1 | Turn roots into initial straight strands: sets the CV count, the length and the lift off the surface. |
| `UsdGenGuideInterpolate` | v1 | Build each strand by blending its nearest guides in skin space; stores the guide indices and weights at capture. |
| `UsdGenCurveSource` | v1 | Read an existing `BasisCurves` (freeze, import, sim cache) out of the scene index into the graph. |
| `UsdGenClump` | v1 | Pull strands toward shared clump centres, at one or more levels, with stray, volumize, noise and profile controls. |
| `UsdGenNoise` | v1 | Frizz: add correlated fBm displacement in the root frame so it follows the deforming surface. |
| `UsdGenLength` | v1 | Set, scale, cut or cull strand length, with a map and per-curve randomness. |
| `UsdGenWidth` | v1 | Author `widths` from a base width, a root→tip ramp and a taper. |
| `UsdGenDirection` | v1 | a host groomer Tilt/Around-N: rotate strands toward a direction and lift them off the surface. |
| `UsdGenSmooth` | v1 | Laplacian smoothing along each curve with the root locked. |
| `UsdGenResample` | v1 | Change the CV count, uniformly or by arc length; the one styler that is topology-bumping. |
| `UsdGenScale` | v1 | Global length multiplier with an optional ramp (a host groomer IGS). |
| `UsdGenSculptLayer` | v1 | Apply hand-authored per-CV deltas in the root frame, keyed by `curveId`, at a layer weight. |
| `UsdGenFreeze` | v1 | Cap the chain at an authored snapshot; one token switches between frozen and live. |
| `UsdGenDeform` | v1 | Transport rest curves through the animated surface frame (rigid frame, RBF or point deform). |
| `UsdGenCurl` | v2 | Coil each strand around its own tangent frame at a radius and frequency. |
| `UsdGenBend` | v2 | Cumulative per-segment rotation about an axis, ramped along the curve. |
| `UsdGenStraighten` | v2 | Blend each strand toward the straight root→tip line, per plane. |
| `UsdGenDisplace` | v2 | Push CVs along the surface normal by a height or vector map. |
| `UsdGenWave` | v2 | Sinusoidal displacement in the tangent and normal directions. |
| `UsdGenPart` | v2 | Parting lines from a curve set: emit `partId` and suppress guide/clump weights across a parting curve (ADR §2.3). Its name and properties are declared from M1 so C1 covers them (§2.7.2). |
| `UsdGenExprOp` | v2 | A styler whose per-CV kernel is a SeExpr expression, evaluated at **capture time only** (S37). |
| `UsdGenInstance` | v1 (M6) | Emit an instancer of cards, archives or spheres instead of curves. |
| `UsdGenCollide` | v3 | Push or rotate strands out of colliders and the skin. |
| `UsdGenWind` | v3 | Time-dependent force field with gust and shear terms. |
| `UsdGenImageMap` | v1 | Sample a UV-mapped image through the surface's `st`. |
| `UsdGenExprMap` | v1 | Evaluate a SeExpr expression over the a host groomer variable set. |
| `UsdGenPaintMap` | v1 | Read a primvar the paint brush writes on the surface during the session. |
| `UsdGenNoiseMap` | v1 | Procedural fBm/noise field, using SeExpr's noise as the single implementation (S38). |
| `UsdGenCombineMap` | v1 | Combine several maps (multiply, add, max, …) — usdRig's weight-object shape. |
| `UsdGenGuideProximityMap` | v1 | Distance-to-nearest-guide falloff, for masks that follow the guide layout. |
| `UsdGenPtexMap` | v1 (M4) | Sample a `.ptx` file by the ptex face id derived from the parent-mesh face id and the sub-face quadrant, plus the per-face UV (§2.20 rule 2). |

---

## 2. Property reference

### 2.1 Dirty classes

Every row in every table below carries a **dirty class**. The engine's dirty router is compiled
from exactly this column (§6; `03-execution-engine.md`).

| Class | Meaning | Engine consequence |
|---|---|---|
| **structural** | changes the graph's shape or a node's topology contribution | bumps the Merkle structural digest of that node and of everything downstream of it through `usdGen:input`; recompiles that sub-graph; drops its capture caches |
| **capture** | changes what `Capture()` reads | bumps the node's 128-bit capture epoch; re-runs `Capture()` on that node and downstream; does **not** recompile |
| **value** | changes only a per-frame parameter | bumps one parameter's value version; re-runs `Evaluate()` on the node's chunks and on the tail |
| **toggle** | `usdGen:enabled` on a topology-preserving operator | node becomes a memcpy pass-through; no recompile, no re-capture; the tail re-runs |
| **publication** | changes only what the imaging layer writes onto already-published prims | `usdGenImaging` re-emits the affected Hydra locator on every tile of the description; no capture, no evaluate, no `UsdGenDirtyBits` and no engine work at all |
| *cosmetic* | not routed at all | `usdGen:label` and documentation; excluded from all three digests; costs nothing |

The four classes ADR §2.3 names — structural, capture, value, non-structural toggle — are the
**engine-visible** set, and each maps to exactly one unscoped bit of `03-execution-engine.md`
§5.1's `UsdGenDirtyBits` (never written `UsdGenDirtyBits::Value`, ADR §9 R3):

| Class | Bit |
|---|---|
| structural | `UsdGenDirtyStructural` |
| structural (topology) | `UsdGenDirtyTopology` |
| capture | `UsdGenDirtyCapture` |
| value | `UsdGenDirtyParameter` |
| toggle | `UsdGenDirtyParameter` (the node is aliased to its input; no recapture) |

**publication** and *cosmetic* are the two extra classes this document adds. They carry **no**
`UsdGenDirtyBits` and never reach `UsdGenDirtyRouter` at all: `usdGen:pickTarget` needs one because
`primOrigin` is written by the imaging layer, and `usdGen:label` needs one because it must cost
nothing.

**structural** splits in two in §6's action column. A *graph* structural edit changes the compiled
shape and bumps the Merkle digest `d(n) = H(type, algorithmVersion, mode, readPhase, space,
sorted(input paths), sorted(reference/map/surface relationship targets), d(inputs...))` (ADR §4.2.1).
A **structural (topology)** edit — `usdGen:enabled` on a generator, `usdGen:length:mode`,
`usdGen:tileTarget` — changes the curve, CV or published-prim count without changing that formula's
terms: it bumps the
node's topology generation, re-runs `Capture()` and re-allocates downstream buffers, and leaves the
digest alone (§0.4).

### 2.2 `UsdGenGroom` (: `UsdGeomImageable`)

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:surface` | `rel` | — | Default bound surface(s) for every description under this groom. Targets may be `Mesh` prims or `GeomSubset`s (§2.20). An empty relationship on a `UsdGenDescription` inherits this. | structural |
| `usdGen:densityScale` | `float` | `1.0` | Density fraction in the **interactive** context; **ignored in the render context** (ADR §9 R13). Multiplies the description's own; the product is clamped to `[0,1]`. Never re-scatters and never re-chunks: the id space and the chunk partition are untouched and only `UsdGenChunkDesc::liveCount` moves (ADR §9 R13; `03-execution-engine.md` §1.2). | value |
| `usdGen:renderDensityScale` | `float` | `1.0` | Density fraction in the **render** context (§2.3.2); **ignored in the interactive context** (ADR §9 R13). Multiplies the description's own; the product is clamped to `[0,1]`. Same class and same reason as `usdGen:densityScale`. | value |
| `usdGen:schemaVersion` | `uniform int` | `1` | Major schema version this groom was authored against. A binary that knows a lower version refuses the groom (§8.4). | structural |
| `usdGen:sessionId` | `uniform string` | `""` | Fallback registry key, used **only when no `UsdStage` is reachable** from the scene index. The weak stage pointer is always the primary key (S15, ADR §4.5, ADR §9 R30); when it is unavailable the session is keyed by `(renderInstanceId, root-layer identifier, usdGen:sessionId, groom root)`, so two stages authoring the same id never share a session. Empty is legal and means "stage key only". | structural |
| `usdGen:label` | `string` | `""` | Free text shown in the groom panel. | *cosmetic* |

### 2.3 `UsdGenDescription` (: `UsdGeomBoundable`)

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:surface` | `rel` | — (inherits the groom's) | Bound `Mesh` and/or `GeomSubset` targets. Instance-proxy targets are translated to the prototype by S7's `ProxyPathTranslationDataSourceNames()`. | structural |
| `usdGen:terminal` | `rel` | — | **Exactly one** operator prim whose output is published. More than one target is a compile error naming both paths. | structural |
| `usdGen:guides` | `rel` | — | `UsdGenGuideSet` prims visible to this description's operators. An operator's own `usdGen:guides` overrides it. | structural |
| `usdGen:tileTarget` | `uniform int` | `64` | Target number of published `basisCurves` prims; clamped to `[32,256]`. Authored, not env, because the prim set must be reproducible across sessions and hosts (ADR §2.3). Engine chunking is separate and is env (`USDGEN_CHUNK_SIZE`). **Not** a node-digest term: an edit re-allocates the published prim set without recompiling (ADR §9 R21, §3.2, §6.2). | structural (topology) |
| `usdGen:densityScale` | `float` | `1.0` | Multiplies the groom's, **interactive context only** (§2.3.1). Decimation only — no re-scatter, no re-chunk (ADR §9 R13). | value |
| `usdGen:renderDensityScale` | `float` | `1.0` | Multiplies the groom's, **render context only** (§2.3.1). Decimation only — no re-scatter, no re-chunk (ADR §9 R13). | value |
| `usdGen:curve:basis` | `uniform token` | `"bspline"` | `bspline \| catmullRom`. `centripetalCatmullRom` and `bezier` are rejected at compile with a named diagnostic (S29). `type = "cubic"` and `wrap = "pinned"` are contract constants of C2 and are **not** authorable. | structural |
| `usdGen:width:default` | `float` | `0.01` | Strand width in stage units used when no `UsdGenWidth` operator is in the chain. | value |
| `usdGen:motion:mode` | `uniform token` | `"single"` | `single \| velocities \| samples` = profiles P0/P1/P2 (S32). §2.18. | structural |
| `usdGen:motion:sampleCount` | `uniform int` | `3` | Samples across the shutter for `samples`; clamped to `[2,16]`. | structural |
| `usdGen:motion:forwardSurfaceSamples` | `uniform bool` | `false` | `true` forwards the surface's own contributing sample times instead of clamping to the shutter (`research/G-motion-blur-sampling-strategy.md` §4.2). | structural |
| `usdGen:pickTarget` | `uniform token` | `"description"` | `description \| terminal`; selects the `primOrigin/scenePath` written onto the published tiles, i.e. what a viewport click selects (S29). `primOrigin` is part of the tile contract C2 and is written by the imaging layer, never by a kernel (ADR §5.3), so this edit republishes and evaluates nothing. | **publication** |
| `usdGen:label` | `string` | `""` | | *cosmetic* |

`material:binding` (from `UsdShadeMaterialBindingAPI`) on the description is authored by hand onto
every published tile, because usdGen runs after flattening (S4, ADR §5.3). The description does
**not** carry a "primitive type" token: whether a description draws splines or an instancer is
decided by the type of its `usdGen:terminal` operator (`UsdGenInstance` ⇒ instancer, anything else
⇒ tiles of `basisCurves`). This removes the "config prim vs chain node" ambiguity the panel flagged
(`design/judge-artist.md` §2.2).

#### 2.3.1 The density pair

The generator's `Capture()` emits the **full** root set at `usdGen:density` (times the density map
and mask). That set defines the id space and never changes when a scale changes. The two scales are
**exclusive by context** — neither applies in the other's context (ADR §9 R13) — and the surviving
set is chosen by a salted hash of the stable id:

```
scale(g)     = (context == render) ? g.renderDensityScale : g.densityScale
keepFraction = clamp( scale(groom) * scale(description), 0.0, 1.0 )
keep(root r) iff UsdGenHash32( curveId(r), kSaltDensity ) < keepFraction * 2^32
```

`UsdGenHash32` and the per-use salt constants are §2.19.1; `kSaltDensity ≠ 0` is what stops the
surviving set from being `{hairId < keepFraction}`, which would bias the shader's per-curve hue and
value jitter to the low end of its range in the viewport and not at render (ADR §9 R12).

Because the predicate is a pure function of the stable id, ids, sculpt deltas, clump ids and
guide weights survive every scale change, and the preview set is automatically a subset of any
larger scale's set (ADR §9 R13). That is why both scales are **value**, not capture (§6.5): the
chunk partition is computed once per capture over the **full** id set and a scale change only moves
`UsdGenChunkDesc::liveCount` inside the existing chunks — it never re-scatters and never re-chunks
(`03-execution-engine.md` §1.2, §3.6). A `usdGen:density` edit on a *generator* is a different
knob and stays capture (§6.4). A product greater than 1 is clamped with one `TF_WARN` per
description: raising density above the authored `usdGen:density` would move the id space and
silently invalidate every sculpt layer. A groom that wants a quarter-density viewport and a
full-density render authors `usdGen:density` at the **full render count**, `densityScale = 0.25`
and `renderDensityScale = 1` — a host groomer's Render Density Multiplier expressed so that ids hold
(`research/A7-prior-art-grooming.md` §1.5).

The interactive ceiling is a **session** value, not a schema property:
`UsdGenImaging_SetInteractiveLOD(descPath, maxCurves)` (`08-tools.md` §1.4, ADR §9 R8, R31, R36)
applies the same predicate with `keepFraction = min(keepFraction, maxCurves / nCurves)` for the
duration of a drag. During a density scrub the element counts are held at the maximum-density
topology and culled strands collapse to degenerate CVs with zero width; the real count change
commits on release (S28 — padding is broken in 26.08 and renders fallback red, so exact-size arrays
are mandatory).

#### 2.3.2 The explicit context mechanism

There is no interactive flag on `sceneGlobals` (`pxr/imaging/hd/sceneGlobalsSchema.h:37-47`,
verified) and the renderer display name is not intent — `usdrecord --renderer GL` is Storm and *is*
a render; an hdPrman viewport is not Storm and *is* interactive. The session context is therefore
**explicit** and has exactly three sources, in priority order (ADR §2.3):

1. `UsdGenImaging_SetContext("render" | "interactive")` — the C ABI, used by applications and by
   batch drivers that wrap `usdrecord`.
2. `USDGEN_CONTEXT=render` in the environment — the batch default.
3. A usdview toggle in the groom panel, which calls (1).

Absent all three the context is `interactive`. The context is a property of the **session**, not of
a scene index: several indices attached to one session share one context, one current frame and one
generation (ADR §4.5), so a renderer switch never produces two different curve counts for one
groom.

### 2.4 `UsdGenGuideSet` (: `UsdGeomImageable`)

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:surface` | `rel` | — (inherits the description's) | The mesh the guide roots bind to. Used to recompute `skinprim`/`skinprimuv` when guides are replanted. | structural |
| `usdGen:blend` | `float[]` | `[]` | Per-guide range of influence in `[0,1]`, a host groomer's guide "Blend". Indexed by the child curves' `primvars:usdGen:curveId` in ascending order. Empty means all `1.0`. | capture |
| `usdGen:label` | `string` | `""` | | *cosmetic* |

The guide curves themselves are the namespace children of the set: `UsdGeomBasisCurves` prims with
`UsdGenCurveAPI` applied and `primvars:usdGen:role = "guide"` (§5). A set may hold one curves prim
holding all guides, or one prim per guide; the tool authors one prim per set and treats
`curveVertexCounts` as the guide list, because a resync per comb stroke is unacceptable
(`research/G-freeze-bake-undo-and-frozen-reentry.md` §4.2).

### 2.5 `UsdGenOperator` (abstract, : `UsdTyped`)

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:input` | `rel` (ordered) | — | Upstream operator prim(s). Zero targets = a source node (`UsdGenScatter`, `UsdGenCurveSource`). Kahn topological order, namespace order as tie-break; a cycle is a compile error naming the offending path pair (S26). | structural |
| `usdGen:enabled` | `bool` | `true` | `false` = pass through the input unchanged. Non-structural for topology-preserving types; structural (topology) for generators, `UsdGenResample`, `UsdGenLength` (unconditionally — its static `TopologyEffect()` is `MayChangeCurveCount`), `UsdGenFreeze` and `UsdGenInstance` (§0.4, §6.2, ADR §9 R14). A muted generator keeps its own capture. Never a term of the Merkle digest (ADR §4.2.1). | toggle / structural (topology) |
| `usdGen:blend` | `float` | `1.0` | Envelope: final lerp between the input and this operator's result, with **exact endpoints** at 0 and 1 (the `RigExecBlendEnvelope` contract, `research/A1-usdrig-graph.md` §5). | value |
| `usdGen:seed` | `uniform int` | `0` | Every hash inside the operator is `UsdGenDraw01(seed, curveId, kSalt<Use>)` (§2.19.1). The per-use salt decorrelates *different* uses (a clump draw from a noise draw); two prims of the **same** type with the same seed over the same id set draw identical numbers by construction, so the tool authors a distinct seed on every operator prim it creates and the artist varies it (`04-operators.md` §0.6). | capture |
| `usdGen:space` | `uniform token` | `"auto"` | `auto \| rest \| deformed` — three tokens, no more (`design/proposal-performance.md` §4.2). `auto` = the operator type's declared class, which S25 makes a two-way classification (`restSpace` / `deformedSpace`). Overriding to `deformed` moves the node into the motion tail and multiplies its cost by the sample count. A world-space class, if a v3 `UsdGenWind` needs one, is a §8.2 token addition whose fallback is `auto`. | structural |
| `usdGen:readPhase` | `uniform token` | `"final"` | Which generation of the *surface* this operator reads: `base` (the rest surface at `Default()`, S12) · `final` (default; after every modifier, including usdGen's own overlays) · `@<absolute prim path>` (a named prim, e.g. a low-res proxy scalp). `preceding` is accepted in v1 as an **alias for `final`** (ADR §9 R9) and is not a distinct phase; the tool never authors it. Retained from usdRig (S26). | structural |
| `usdGen:surface` | `rel` | — (inherits the description's) | Per-operator surface override, e.g. clumping against a different mesh. | structural |
| `usdGen:algorithmVersion` | `uniform int` | `0` | The kernel revision this operator's look was authored against. `0` (the fallback) means **"track the newest kernel"** (ADR §9 R17). The tool, every freeze and every bake author the explicit current integer, so look preservation is guaranteed for every tool-authored asset; a hand-written asset that omits it follows the newest kernel and says so in the panel (§8.3). One fallback on the base — never re-declared per concrete class. | structural |
| `usdGen:label` | `string` | `""` | Free text shown in the stack editor row. | *cosmetic* |

`UsdGenMaskAPI` (§2.13) is auto-applied to this type and therefore present on every derived type.

### 2.6 Generators

Common to `UsdGenGenerator`: nothing beyond the base. Per-type parameters are summarised here and
specified in full, with their kernels and their capture/evaluate split, in `04-operators.md` §2.

**`UsdGenScatter`** (v1) — emits roots on the rest surface with stable ids
`UsdGenHash64(seed, faceIndex, k, kSaltScatter)` (§2.19.1, ADR §9 R12).

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:mode` | `uniform token` | `"random"` | `random \| uniform \| points \| atGuides`. Tiers (ADR §9 R38): `random` v1/M1, `atGuides` v1/M3, `points` v1/M5, `uniform` **v2 (M8)**. | structural |
| `usdGen:density` | `float` | `100.0` | Hairs per square stage unit on the rest surface (§0.8). `random` mode only. | capture |
| `usdGen:spacingU`, `usdGen:spacingV` | `float` | `0.02` | `uniform` mode: parametric row/column spacing. | capture |
| `usdGen:jitter` | `float` | `0.0` | `uniform` mode. | capture |
| `usdGen:rootPrims` | `int[]` | `[]` | `points` mode: per-root face index (authored by the Place brush). | capture |
| `usdGen:rootUVs` | `texCoord2f[]` | `[]` | `points` mode: per-root barycentric/UV inside that face. | capture |
| `usdGen:relaxIterations` | `int` | `0` | Poisson-disk relaxation passes, `[0,50]`. | capture |
| `usdGen:areaCompensation` | `bool` | `true` | Weight per-face counts by rest area. | capture |
| `usdGen:guides` | `rel` | — | `atGuides` mode: one root per guide root. | structural |
| `usdGen:flip` | `bool` | `false` | Emit on the back side of the surface — the root normal is negated before the root frame is built (`research/A7-prior-art-grooming.md` §9.1 G1). | capture |
| `usdGen:perGuide` | `int` | `1` | `atGuides` mode: roots emitted per guide root, `≥ 1`. | capture |

**`UsdGenGrow`** (v1) — roots → straight strands.

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:segments` | `int` | `8` | `[2,64]`. Sets the CV count, so the edit also re-allocates downstream (§6.1's row says so). | structural |
| `usdGen:length` | `float` | `1.0` | Stage units; may be map-driven. | capture |
| `usdGen:lengthRandom` | `float2` | `(1,1)` | Per-curve multiplier drawn uniformly in `[x,y]`. | capture |
| `usdGen:direction` | `uniform token` | `"surfaceNormal"` | `surfaceNormal \| attribute \| vector`. The flat name is shared with `UsdGenDirection`'s `vector3f usdGen:direction` (§2.7.1); the §6 **On type** column keeps the two apart. | structural |
| `usdGen:directionVector` | `vector3f` | `(0,1,0)` | Used when `direction = "vector"`. | value |
| `usdGen:directionPrimvar` | `token` | `""` | Surface primvar name sampled per root when `direction = "attribute"`. | capture |
| `usdGen:lift` | `float` | `0.0` | Degrees, `[-90,90]`, rotated away from the surface tangent plane. | value |
| `usdGen:uvBlend` | `float` | `0.0` | `[0,1]`. Blend the growth direction toward `dPdu`/`dPdv` (tangential to skin). | value |
| `usdGen:length:source` | `rel` | — | A `UsdGenMap` multiplying `length`. Every relationship retarget is a graph edge (§6.1). | structural |

**`UsdGenGuideInterpolate`** (v1) — the curve generator when guides exist. `usdGen:guides` (`rel`,
structural), `usdGen:maxGuides` (`int`, `3`, capture), `usdGen:influenceRadius` (`float`, `4.0`,
capture), `usdGen:influenceDecay` (`float`, `2.0`, capture), `usdGen:maxGuideAngle` (`float`, `90`
degrees, capture), `usdGen:blendMethod` (`uniform token`, `"linearBlend"`, structural — allowed
`linearBlend | extrudeAndBlend`), `usdGen:blendInSkinSpace` (`float`, `1.0`, value),
`usdGen:useUniqueGuide` (`bool`, `false`, capture — one guide per hair, a host renderer's mode),
`usdGen:randomizeGuide` (`float`, `0.0`, capture), `usdGen:cvCount` (`int`, `8`, structural),
`usdGen:clumpCrossover` (`float`, `0.0`, capture), `usdGen:length:source` (`rel` → a `UsdGenMap`
whose value multiplies the interpolated strand length, structural), `usdGen:mask:region` (inherited
from the mask block, capture). It **emits** `int[] primvars:guideIndex` and
`float[] primvars:guideWeight`, both `uniform` with `elementSize = 3`
(`pxr/usd/usdGeom/primvar.h:325-331`), matching a host renderer's `groom_closest_guides` /
`groom_guide_weights` arity so a bake round-trips. The names carry **no `usdGen:` prefix**: they are
published under contract C2 as `primvars/guideIndex` and `primvars/guideWeight` (ADR §2.3 and §9
R24; `06-imaging.md` §4.1; `03-execution-engine.md` §1.2), and C1 and C2 must freeze one spelling.

**`UsdGenReferenceSource`** (v1) — pulls one already-baked in-stage `BasisCurves`
prim into the stack verbatim, as an alternative to Scatter/Instance. The set enters through the
reference lane (ADR §4.1 I3); ids come from `primvars:usdGen:curveId` when authored and are
synthesized from the source ordering otherwise. One row only.

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:reference` | `rel` | — | Exactly one `BasisCurves` target under contract C3 (§5). | structural |

**`UsdGenCurveSource`** (v1) — the v1 entry point for frozen, imported and simulated curves. All
eight rows are declared here; `04-operators.md` §2.4 and `05-static-curves-and-deformation.md` §2.1
restate them and may not disagree (ADR §9 R7, R8).

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:curves` | `rel` | — | Exactly one `BasisCurves` target under contract C3 (§5). | structural |
| `usdGen:useRest` | `bool` | `true` | `true` = the loaded `points` are a rest pose and `primvars:rest` (or `points` at `Default()`) is the rest buffer; `false` = the points are already deformed. | structural |
| `usdGen:idSource` | `uniform token` | `"primvar"` | `primvar \| index`. `index` fabricates `curveId = i` for curves that carry no `primvars:usdGen:curveId`. | structural |
| `usdGen:lane` | `uniform token` | `"hair"` | `hair \| reference`. `reference` puts the set in the un-chunked reference lane (ADR §4.1 I3). | structural |
| `usdGen:expectEpoch` | `uniform string` | `""` | Empty = never stale; otherwise must equal the target's `primvars:usdGen:frozenEpoch`, prefix `usdgen1:sha1:` (§8.5). | capture |
| `usdGen:staleAction` | `uniform token` | `"warn"` | `warn \| ignore \| block`. **This is the only stale-policy knob in the schema; `UsdGenFreeze` has none** (ADR §9 R18). | structural |
| `usdGen:resampleTo` | `uniform int` | `0` | `0` = keep the source CV counts (ragged); `> 0` = resample every curve to that count at capture — the import tool's "resample to N", with the measured E-1r penalty shown. | structural (topology) |
| `usdGen:rebind` | `uniform token` | `"onError"` | `never \| onError \| always` — when to recompute `skinprim`/`skinprimuv` from the rest surface. | capture |

There is no `usdGen:mode` on `UsdGenCurveSource`: provenance is documentation, and the behavioural
forks are `useRest`, `idSource` and `lane`. Ragged inputs (mixed CV counts) are supported in v1 and
are the reason the ragged buffer path exists (ADR §4.2.2, gate E-1r). A source whose
`primvars:usdGen:curveId` is a 32-bit `int[]` — the shape a foreign exporter writes — is **widened
to `uint64` on import** by `UsdGenCurveSource` (§5).

### 2.7 Stylers

Common to `UsdGenStyler`: nothing beyond the base. The summary table names the parameter set per
type; §2.7.1 (v1) and §2.7.2 (the v2 names reserved at M1) are **normative** for every one of their
names, types, defaults, allowed tokens, uniformity and dirty class (ADR §9 R7). `04-operators.md`
§2.8–§2.15 gives the v1 kernels and §3 the v2 ones, with the capture/evaluate split and the parity
notes; they restate these rows without disagreeing, and a disagreement is a build error caught by
§9 test 3.

| Type | Tier | Key parameters (all `usdGen:`-prefixed) | Topology |
|---|---|---|---|
| `UsdGenClump` | v1 | `clump:amount` `clump:size` `clump:density` `clump:centers`(rel) `clump:seed` `clump:level` `clump:levels` `clump:sizeReduction` `clump:tightnessReduction` `clump:goalFeedback` `clump:method` `clump:crossover` `clump:profile:knots` `clump:stray:{amount,rate,falloff}` `clump:volumize` `clump:copy` `clump:copyVariance` `clump:cut` `clump:flatness` `clump:offset` `clump:curl:{amplitude,frequency}` `clump:noise:{amount,frequency,correlation}` `preserveLength` | no |
| `UsdGenNoise` | v1 | `noise:magnitude` `noise:frequency` `noise:correlation` `noise:octaves` `noise:lacunarity` `noise:gain` `noise:magnitude:knots` `preserveLength` `cumulative` | no |
| `UsdGenLength` | v1 | `length:mode` `length:value` `length:random` `length:method` `length:source`(rel) `minRemainingLength` `cullThreshold` `rebuild` | **always** (static `TopologyEffect()`) |
| `UsdGenWidth` | v1 | `width` `width:knots` `taper` `taperStart` `rootScale` `tipScale` `replace` | no |
| `UsdGenDirection` | v1 | `direction` `amount` `lift` `tiltU` `tiltV` `tiltN` `aroundN` `mode` `followSkinContour` `direction:source`(rel) `direction:knots` | no |
| `UsdGenSmooth` | v1 | `strength` `iterations` `mode`(=`alongCurve`; `neighbours` is v3) `lockRoot` `searchRadius` `numNeighbors` | no |
| `UsdGenResample` | v1 | `cvCount` `distribution` `restoreSegmentLengths` | **CV count** |
| `UsdGenScale` | v1 | `scale` `scale:knots` `scaleRandom` `widthToo` | no |
| `UsdGenSculptLayer` | v1 | §2.10 | no |
| `UsdGenCurl` | v2 | `radius` `radius:knots` `frequency` `phase` `phaseRandom` `taper` `axisMode` `clockwise` | no |
| `UsdGenBend` | v2 | `angle` `angleRandom` `axisMode` `axis` `angle:knots` | no |
| `UsdGenStraighten` | v2 | `tangentStraightness` `normalStraightness` | no |
| `UsdGenDisplace` | v2 | `displace:amount` `displace:map`(rel) `displace:base` `displace:scale` `displace:offset` `mode`(=`height \| vector`) | no |
| `UsdGenWave` | v2 | `amplitudeU` `amplitudeN` `frequencyU` `frequencyN` | no |
| `UsdGenPart` | v2 | `part:curves`(rel) `part:radius` `part:strength` — the parting-line operator; it emits `partId` and multiplies the guide/clump weights the consumers already store (`04-operators.md` §3) | no |
| `UsdGenExprOp` | v2 | `expr:source` `expr:maps`(rel) `expr:returnType` `mode`(=`cv \| curve`) — **capture-time only** (S37; performance ASSUMPTION 8) | no |

Every `…:knots` parameter above is accompanied by a `uniform token …:interpolation` whose token set
is `linear | catmullRom | bspline | constant` and whose default is `"catmullRom"` (§2.17, ADR §9
R11); the summary table names only the knots array to stay readable.

#### 2.7.1 The v1 styler parameters (normative)

One shared, **flat** `float usdGen:preserveLength = 1.0` (`[0,1]`, value) is declared on every
styler that moves CVs — `UsdGenClump`, `UsdGenNoise`, `UsdGenCurl`, `UsdGenBend`, `UsdGenWave`,
`UsdGenDisplace` — and is never namespaced per operator (`04-operators.md` §2.8, §2.9 agree).

`UsdGenClump`:

| Property | Type | Default | Allowed / range | Dirty class |
|---|---|---|---|---|
| `usdGen:clump:amount` | `float` | `0.5` | `[0,1]` | value |
| `usdGen:clump:profile:knots` | `float2[]` | `[(0,0),(1,1)]` | §2.17 scalar ramp | value |
| `usdGen:clump:centers` | `rel` | — | a curve set, a nested `UsdGenScatter`, or a `UsdGenMap` | structural |
| `usdGen:clump:density` | `float` | `4.0` | > 0, clumps per square rest stage unit; used when `centers` is empty | capture |
| `usdGen:clump:size` | `float` | `1.0` | > 0, clump radius in rest stage units | capture |
| `usdGen:clump:seed` | `uniform int` | `0` | | capture |
| `usdGen:clump:level` | `uniform int` | `-1` | `-1` = auto: this node's ordinal among **all** `UsdGenClump` nodes of the compiled chain, disabled ones included, so a mute never renumbers a published `clumpId_<n>` (ADR §9 R10). `≥ 0` pins the index | structural |
| `usdGen:clump:levels` | `int` | `1` | `[1,4]` fractal levels | capture |
| `usdGen:clump:sizeReduction` | `float` | `0.5` | `[0,1]` | capture |
| `usdGen:clump:tightnessReduction` | `float` | `0.8` | `[0,1]` | value |
| `usdGen:clump:goalFeedback` | `float` | `1.0` | `[0,1]` | capture |
| `usdGen:clump:method` | `uniform token` | `"linearBlend"` | `linearBlend \| extrudeAndBlend` (`research/A7-prior-art-grooming.md` §9.2 S1). `UsdGenClump` carries **no** `usdGen:mode` (ADR §9 R10) | structural |
| `usdGen:clump:crossover` | `float` | `0.0` | `[0,1]` | capture |
| `usdGen:clump:volumize` | `float` | `0.0` | `[0,1]` | value |
| `usdGen:clump:stray:amount` / `:rate` / `:falloff` | `float` | `0.0` / `0.0` / `1.0` | `[0,1]`, `[0,1]`, ≥ 0 | value / capture / value |
| `usdGen:clump:copy` / `:copyVariance` | `float` | `0.0` / `0.0` | `[0,1]` | value / capture |
| `usdGen:clump:cut` | `float` | `0.0` | `[0,1]` | capture |
| `usdGen:clump:flatness` | `float` | `0.0` | `[0,1]` | value |
| `usdGen:clump:offset` | `float` | `0.0` | stage units along the root normal | value |
| `usdGen:clump:curl:amplitude` / `:frequency` | `float` | `0.0` / `1.0` | ≥ 0 | value |
| `usdGen:clump:noise:amount` / `:frequency` / `:correlation` | `float` | `0.0` / `1.0` / `0.0` | ≥ 0, > 0, `[0,1]` | value |

`UsdGenNoise`:

| Property | Type | Default | Allowed / range | Dirty class |
|---|---|---|---|---|
| `usdGen:noise:magnitude` | `float` | `0.05` | ≥ 0, stage units | value |
| `usdGen:noise:magnitude:knots` | `float2[]` | `[(0,0),(1,1)]` | §2.17 scalar ramp | value |
| `usdGen:noise:frequency` | `float` | `3.0` | > 0, cycles per stage unit | value |
| `usdGen:noise:correlation` | `float` | `0.5` | `[0,1]` | value |
| `usdGen:noise:octaves` | `int` | `1` | `[1,6]` | value |
| `usdGen:noise:lacunarity` | `float` | `2.0` | > 1 | value |
| `usdGen:noise:gain` | `float` | `0.5` | `[0,1]` | value |
| `usdGen:cumulative` | `bool` | `false` | flat, not `noise:cumulative` | value |

`UsdGenLength` (the whole type is topology-bumping, §0.4):

| Property | Type | Default | Allowed / range | Dirty class |
|---|---|---|---|---|
| `usdGen:length:mode` | `uniform token` | `"scale"` | `set \| scale \| cull` — `cull` removes curves | structural (topology) |
| `usdGen:length:value` | `float` | `1.0` | stage units for `set`, a fraction for `scale` | capture |
| `usdGen:length:random` | `float2` | `(1,1)` | per-curve multiplier drawn in `[x,y]` | capture |
| `usdGen:length:method` | `uniform token` | `"scale"` | `scale \| cutExtend` | structural |
| `usdGen:length:source` | `rel` | — | a `UsdGenMap` multiplying `length:value` | structural |
| `usdGen:rebuild` | `uniform token` | `"keepParam"` | `keepParam \| reparam` | structural |
| `usdGen:minRemainingLength` | `float` | `0.0` | ≥ 0, stage units | capture |
| `usdGen:cullThreshold` | `float` | `0.0` | ≥ 0; curves shorter than this are removed at capture | structural (topology) |

`UsdGenWidth`, `UsdGenDirection`, `UsdGenSmooth`, `UsdGenResample`, `UsdGenScale`:

| Property | Type | Default | Allowed / range | Dirty class |
|---|---|---|---|---|
| `usdGen:width` | `float` | `0.01` | ≥ 0, stage units, `UsdGenWidth` | value |
| `usdGen:width:knots` | `float2[]` | `[(0,1),(1,1)]` | §2.17 scalar ramp | value |
| `usdGen:taper` / `usdGen:taperStart` | `float` | `0.0` / `0.5` | `[0,1]` | value |
| `usdGen:rootScale` / `usdGen:tipScale` | `float` | `1.0` / `1.0` | ≥ 0 | value |
| `usdGen:replace` | `bool` | `true` | `true` = set widths, `false` = multiply upstream widths | structural |
| `usdGen:direction` | `vector3f` | `(0,1,0)` | `UsdGenDirection` target direction | value |
| `usdGen:amount` | `float` | `0.0` | `[0,1]`, `UsdGenDirection` | value |
| `usdGen:lift` | `float` | `0.0` | degrees, `UsdGenGrow` and `UsdGenDirection` | value |
| `usdGen:tiltU` / `:tiltV` / `:tiltN` / `:aroundN` | `float` | `0.0` | degrees, the four a host groomer Tilt axes | value |
| `usdGen:followSkinContour` | `float` | `0.0` | `[0,1]` | value |
| `usdGen:direction:source` | `rel` | — | a `UsdGenMap` supplying a per-root direction (`returnType = "color"` read as a vector), `UsdGenDirection` | structural |
| `usdGen:direction:knots` | `float2[]` | `[(0,1),(1,1)]` | §2.17 scalar ramp on `amount`, meaningful only in `perSegment` mode, `UsdGenDirection` | value |
| `usdGen:mode` | `uniform token` | `"rigid"` | `rigid \| perSegment`, `UsdGenDirection` | structural |
| `usdGen:mode` | `uniform token` | `"alongCurve"` | `alongCurve \| neighbours` (`neighbours` is v3), `UsdGenSmooth` | structural |
| `usdGen:strength` | `float` | `0.5` | `[-1,1]`; negative sharpens, `UsdGenSmooth` | value |
| `usdGen:iterations` | `int` | `1` | `[1,16]`, `UsdGenSmooth` | value |
| `usdGen:lockRoot` | `bool` | `true` | `UsdGenSmooth` | value |
| `usdGen:searchRadius` | `float` | `0.0` | ≥ 0, `UsdGenSmooth`; read only by the v3 `neighbours` mode. Declared from M1 so the v3 upgrade is not a schema change | capture |
| `usdGen:numNeighbors` | `int` | `4` | `[1,32]`, `UsdGenSmooth`; same v3 note | capture |
| `usdGen:cvCount` | `int` | `8` | `[2,64]`, `UsdGenResample` | structural (topology) |
| `usdGen:distribution` | `uniform token` | `"uniform"` | `uniform \| keepParam`, `UsdGenResample` | structural |
| `usdGen:restoreSegmentLengths` | `bool` | `false` | `UsdGenResample`: re-impose the source segment lengths after resampling; the source lengths are stored at capture | capture |
| `usdGen:scale` | `float` | `1.0` | ≥ 0, `UsdGenScale` | value |
| `usdGen:scale:knots` | `float2[]` | `[(0,1),(1,1)]` | §2.17 scalar ramp, `UsdGenScale` | value |
| `usdGen:scaleRandom` | `float2` | `(1,1)` | ≥ 0, `UsdGenScale`: per-curve multiplier drawn in `[x,y]` | capture |
| `usdGen:widthToo` | `bool` | `false` | `UsdGenScale`: also scale `widths` by the same factor, which changes the published primvar set | structural |

`usdGen:mode` appears twice above with different token sets, and §2.6, §2.7.2 and §2.8 declare four
more (`UsdGenScatter`, `UsdGenDeform`, `UsdGenDisplace`, `UsdGenExprOp`). That is legal because the
**On type** column of §6 is load-bearing: the same locator reaches several kernels and several
`allowedTokens` lists, which a codeless schema authors per declaring class. `usdGen:map:filter`
(§2.12) and `usdGen:expr:returnType` (§2.12, §2.7.2) work the same way. The
v2 `UsdGenDisplace` map relationship is `rel usdGen:displace:map`, not `usdGen:map` — the
`usdGen:<op>:<param>` grouping every other operator uses — so its locator `usdGen/displace/map`
cannot collide as a prefix with the `usdGen:map:*` attribute block every `UsdGenMap` prim carries
(§2.12), and a future `usdGen:map:*` addition cannot silently shadow it.

`UsdGenClump` **emits** `int[] primvars:clumpId_<usdGen:clump:level>` through
`primvars:clumpId_<level + usdGen:clump:levels - 1>` (`uniform`, one primvar per fractal level) so a
bake round-trips and a shader can drive variation from clump id (ADR §2.3, §9 R10). Like
`guideIndex`/`guideWeight` the name carries **no `usdGen:` prefix** — contract C2 publishes it as
`primvars/clumpId_<level>` (ADR §9 R24; `06-imaging.md` §4.1).
`rel usdGen:clump:centers` is an explicit artist-visible input; when it is empty,
`float usdGen:clump:density` generates the centres internally (ADR §2.3).

#### 2.7.2 The v2 styler names reserved at M1 (normative)

Declared in `schema.usda` from M1 so C1 freezes the whole vocabulary once (§1, §8.1); the kernels
ship in M8 (ADR §9 R38). `04-operators.md` §3 gives their algorithms and may not disagree with these
rows (ADR §9 R7).

| Property | Type | Default | Allowed / range | On type | Dirty class |
|---|---|---|---|---|---|
| `usdGen:mode` | `uniform token` | `"height"` | `height \| vector` | `UsdGenDisplace` | structural |
| `usdGen:mode` | `uniform token` | `"cv"` | `cv \| curve` — the granularity the expression is captured at | `UsdGenExprOp` | structural |
| `usdGen:expr:returnType` | `uniform token` | `"displacement"` | `displacement \| width \| color` — a **different** token set from `UsdGenExprMap`'s `float \| color` (§2.12); the §6 **On type** column keeps them apart | `UsdGenExprOp` | structural |
| `usdGen:part:curves` | `rel` | — | the parting curve set (contract C3 curves, or an operator in the reference lane) | `UsdGenPart` | structural |
| `usdGen:part:radius` | `float` | `0.05` | > 0, rest stage units — the kd-tree query radius around a parting curve | `UsdGenPart` | capture |
| `usdGen:part:strength` | `float` | `1.0` | `[0,1]` — how hard a crossing weight is suppressed | `UsdGenPart` | value |
| `usdGen:axisMode` | `uniform token` | `"curveTangent"` | `curveTangent \| guide` | `UsdGenCurl` | structural |
| `usdGen:axisMode` | `uniform token` | `"rootDirection"` | `rootDirection \| uniform \| attribute` | `UsdGenBend` | structural |

`UsdGenCurl` and `UsdGenBend` name their axis switch `uniform token usdGen:axisMode`, **not**
`usdGen:mode` (`04-operators.md` §1.3): a `usdGen:mode` is a term of the structural digest `d(n)`
(ADR §4.2.1) and neither branch changes what those two capture. `usdGen:axisMode` is `structural`
all the same, because it selects a kernel branch — a third `allowedTokens` pair keyed by the §6
**On type** column.

`UsdGenPart` **emits** `int[] primvars:partId` (`uniform`, unprefixed like every other emitted
primvar, ADR §9 R24) and multiplies the guide and clump weights the consumers already store at
capture. v1 ships parting as a region map on the mask block (`rel usdGen:mask:region`, §2.13)
consumed by `UsdGenGuideInterpolate` and `UsdGenClump`; `UsdGenPart` is the v2 *producer* of that
same region signal (ADR §2.3).

### 2.8 Deformers

| Type | Tier | Properties | Dirty class |
|---|---|---|---|
| `UsdGenDeform` | v1 | `uniform token usdGen:mode = "rigidFrame"` (`rigidFrame \| rbf \| pointDeform`) · `bool usdGen:twistAware = true` · `bool usdGen:lockRoots = true` (pin CV 0 exactly to the deformed root position, `research/A7-prior-art-grooming.md` §9.1 G7) · `float usdGen:preserveShape = 0.0` · `int usdGen:preserveShape:iterations = 0` · `int usdGen:rbfSamples = 100` | `mode`/`rbfSamples` structural, the rest value |
| `UsdGenCollide` | v3 | `rel usdGen:colliders` · `float usdGen:offset` · `float usdGen:pushAmount` · `int usdGen:iterations` · `uniform token usdGen:resolveType` | structural / value |
| `UsdGenWind` | v3 | `vector3f usdGen:direction` · `float usdGen:constStrength` · `float usdGen:gustStrength` · `float usdGen:stiffness` · `float2[] usdGen:stiffness:knots` | value |

All three declare `deformedSpace`, so their `usdGen:space = "auto"` resolves to `deformed`, which
puts them in the motion tail: with `usdGen:motion:mode = "samples"` they re-run once per shutter sample and nothing
upstream does (S25, S32).

### 2.9 `UsdGenFreeze`

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:frozen:curves` | `rel` | — | The `UsdGeomBasisCurves` prim holding the snapshot; contract C3 (§5). Exactly one target. | structural |
| `usdGen:frozen:mode` | `uniform token` | `"frozen"` | `frozen` = read the snapshot and do not evaluate upstream. `live` = pass the input through; the snapshot is retained but stale. **This one token is the entire unfreeze gesture.** | structural |
| `usdGen:frozen:epoch` | `uniform string` | `""` | Must equal the target's `primvars:usdGen:frozenEpoch`. A mismatch marks the freeze **stale**: one `TF_WARN` names both epochs, the stack editor badges it, and the evaluator **keeps rendering the frozen data** — a freeze is never silently re-cooked (ADR §9 R18). The artist chooses: re-freeze, unfreeze (`usdGen:frozen:mode = "live"`), or rebase the sculpt. Prefix `usdgen1:sha1:` (§8.5). | capture |
| `usdGen:frozen:tier` | `uniform token` | `"session"` | `session \| sublayer \| payload` = S42's three landing places (session layer / sidecar `.usdc` sublayer / `.usdc` payload). These three **tokens** are the only names for a landing place (ADR §9 R1); they are unrelated to the test tiers T0–T4 of §9, which are a different vocabulary. Advisory: it records where the tool put the data so the tool can move it, and the evaluator never reads it (ADR §9 R18). | *cosmetic* |

There is **no `usdGen:staleAction` on `UsdGenFreeze`**: the per-source stale-policy knob exists
only on `UsdGenCurveSource` (§2.6, ADR §9 R18).

The freeze **caps** the chain; upstream operator prims stay authored and are drawn greyed. A
`UsdGenFreeze` may only cap a `restSpace` node: a freeze whose `usdGen:input` resolves to a node in
the deformed tail is a compile error naming the freeze and the first `deformedSpace` node above it
(`05-static-curves-and-deformation.md` §5.5 Rule 0 — re-entering a deformed snapshot above a
`UsdGenDeform` would deform it twice; a frozen-deformed cache is an export that re-enters through
`UsdGenCurveSource` with `usdGen:useRest = false`). Undo of
a live freeze is `SetActive(false)` on the freeze prim, never `RemovePrim` (S41; `RemovePrim` under
an attached OpenExec system raises a spurious `Tf.ErrorException` at
`pxr/exec/esfUsd/stageData.cpp:361` — the `UsdPrimDefaultPredicate` call, with `GetPrimAtPath` at
`:351` (`appendix-A-evidence-ledger.md` §3.10), MEASURED). A freeze may only be undone in the layer it was authored
into (`research/G-freeze-bake-undo-and-frozen-reentry.md` §1.5).

### 2.10 `UsdGenSculptLayer`

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:sculpt:weight` | `float` | `1.0` | Layer weight `[0,1]`, a host groomer sculpt-layer semantics. Multiple layers stack in chain order and blend additively. | value |
| `usdGen:sculpt:space` | `uniform token` | `"rootFrame"` | `rootFrame \| object`. `rootFrame` is what makes a delta survive surface deformation. | structural |
| `usdGen:sculpt:curveIds` | `uint64[]` | `[]` | Curve ids that carry deltas, **sorted ascending**, matching `primvars:usdGen:curveId` (64-bit, ADR §9 R12). | capture |
| `usdGen:sculpt:cvOffsets` | `int[]` | `[]` | Prefix offsets into `deltas`; size `curveIds.size() + 1`; last element = `deltas.size()`. | capture |
| `usdGen:sculpt:deltas` | `vector3f[]` | `[]` | Per-CV delta in that curve's root frame. | value |
| `usdGen:sculpt:epoch` | `uniform string` | `""` | The epoch the deltas were authored against. A mismatch is a badge plus a "Rebase sculpt" action (one undoable re-match by nearest root UV, in the tool); deltas for ids that no longer exist are kept and ignored, never dropped. | capture |
| `usdGen:sculpt:lockedCurves` | `uint64[]` | `[]` | a host groomer Freeze-brush ids: downstream stylers are zeroed for these curves. | capture |
| `usdGen:sculpt:rootPrims` | `int[]` | `[]` | Optional, parallel to `curveIds`: the parent-mesh face each delta's curve was rooted on. Lets "Rebase sculpt" re-match by nearest root UV without re-running the generator (ADR §9 R8). | capture |
| `usdGen:sculpt:rootUVs` | `texCoord2f[]` | `[]` | Optional, parallel to `curveIds`: the root UV inside that face. | capture |

Deltas live on the layer, *outside* the frozen prim, so a re-freeze does not lose hand work and a
layer can be muted or weighted without touching geometry.

### 2.11 `UsdGenInstance` (v1, M6)

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:primitive` | `uniform token` | `"cards"` | `cards \| archives \| spheres`. | structural |
| `usdGen:prototypes` | `rel` (ordered) | — | Prototype prims under `<Description>/Prototypes`. A prototype may be a subtree (an archive). | structural |
| `usdGen:instance:weights` | `float[]` | `[]` | Prototype choice by hashed weight; empty = uniform. Material variety is multiple prototypes, because Storm has no per-instance material binding (S33). | capture |
| `usdGen:instance:orient` | `uniform token` | `"surfaceFrame"` | `surfaceFrame \| curveTangent \| camera \| world`. | value |
| `usdGen:instance:scale` | `float` | `1.0` | | value |
| `usdGen:instance:scaleRandom` | `float2` | `(1,1)` | | capture |
| `usdGen:instance:twist` | `float` | `0.0` degrees | | value |
| `usdGen:instance:twistRandom` | `float` | `0.0` degrees | | capture |
| `usdGen:instance:normalOffset` | `float` | `0.0` | Push along the surface normal, in stage units. | value |
| `usdGen:instance:width`, `usdGen:instance:length` | `float` | `0.02`, `0.09` | Card dimensions; ignored for `archives`. | value |
| `usdGen:instance:width:knots` | `float2[]` | `[(0,1),(1,1)]` | Card width along the strand. | value |
| `usdGen:instance:variationPrimvars` | `token[]` | `["displayColor"]` | Which baked per-curve values are published as `instance`-interpolated primvars. Storm applies **no** filtering to instance-rate primvars (`pxr/imaging/hdSt/primUtils.cpp:211-221`), so arbitrary names reach the shader. | structural |

An interactive card edit dirties `primvars/hydra:instanceTranslations`, never `instancerTopology`
(S33; `research/G-instancing-cards-archives-and-native-instances.md` §8).

### 2.12 `UsdGenMap` and its subtypes

`UsdGenMap` (abstract, : `UsdTyped`) — common block:

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:map:domain` | `uniform token` | `"root"` | `root` (one sample per curve at the root UV) \| `cv` (one per CV, using the CV's projected UV). | capture |
| `usdGen:map:channel` | `uniform token` | `"r"` | `r \| g \| b \| a \| rgb \| luminance`. | capture |
| `usdGen:map:scale` | `float` | `1.0` | Applied after the channel selection. | capture |
| `usdGen:map:offset` | `float` | `0.0` | | capture |
| `usdGen:map:clamp` | `float2` | `(0,1)` | Equal components disable the clamp. | capture |
| `usdGen:map:default` | `float` | `0.0` | The value a **failed** sample returns — missing file, parse error, unbound face, out-of-range channel. A capture loop never throws, so every failure resolves to this number and raises one `TF_WARN` per map per session. It is the value the error policy of `07-look-maps-expressions.md` §5.1 and §7.7 (error reporting) is built on. | capture |
| `usdGen:label` | `string` | `""` | | *cosmetic* |

Maps have **no** `usdGen:input`, `readPhase`, `space` or `enabled`: they are not chain nodes, they
are fields (this is the second D1 defect the panel flagged in the performance proposal,
`design/judge-evidence.md` §2.3).

| Type | Tier | Properties |
|---|---|---|
| `UsdGenImageMap` | v1 | `asset usdGen:map:file` (**one** `asset`, never `asset[]`) · `token usdGen:map:uvSet = "st"` · `token usdGen:map:wrap = "clamp"` (`clamp \| repeat \| mirror \| black`) · `token usdGen:map:filter = "bilinear"` (allowed **`nearest \| bilinear`**) · `token usdGen:map:colorSpace = "raw"` (`auto \| raw \| sRGB`; the default is `raw` so a scalar mask is never silently transfer-transformed, `07-look-maps-expressions.md` §5.1) |
| `UsdGenPtexMap` | v1 (M4) | `asset usdGen:map:file` (`.ptx`), sampled by the ptex face id derived from the parent-mesh face id and the sub-face quadrant (§2.20 rule 2) · `token usdGen:map:filter = "bilinear"` (allowed **`nearest \| bilinear \| box \| gaussian \| bicubic \| bspline \| catmullrom \| mitchell`** — the eight `PtexFilter::FilterType` names, `07-look-maps-expressions.md` §5.3) · `float usdGen:map:blur = 0` · `int usdGen:map:firstChannel = 0` · `int usdGen:map:channelCount = 1` · `token usdGen:map:borderMode = "clamp"` (`clamp \| black \| periodic`) |
| `UsdGenExprMap` | v1 | `string usdGen:expr:source` (SeExpr text) · `uniform token usdGen:expr:returnType = "float"` (`float \| color`) · `uniform int usdGen:expr:seed = 0` · `rel usdGen:expr:maps` (named `UsdGenMap` prims reachable from `map("<primName>")`) |
| `UsdGenPaintMap` | v1 | `rel usdGen:paint:surface` · `token usdGen:paint:primvar = "usdGen:paint:density"` · `token usdGen:paint:interpolation = "faceVarying"` · `token usdGen:paint:storage = "primvar"` (`primvar \| file` — which of the storage states is live, `07-look-maps-expressions.md` §8.1) · `int usdGen:paint:resolution = 256` (the per-face `Res` a `.ptx` bake writes, §8.3 there; a host groomer's `#3dpaint, N`) · `asset usdGen:paint:bakedFile` (EXR or `.ptx` after an explicit bake) |
| `UsdGenNoiseMap` | v1 | `token usdGen:noise:type = "fbm"` (`perlin \| snoise \| fbm \| turbulence \| cellnoise \| voronoi`) · `float usdGen:noise:frequency = 1` · `float usdGen:noise:lacunarity = 2` · `float usdGen:noise:gain = 0.5` · `int usdGen:noise:octaves = 3` · `uniform int usdGen:noise:seed = 0` · `token usdGen:noise:space = "rest"` (`rest \| deformed`; there is **no** `world` token, because post-flattening deformed space *is* world space — ADR §9 R9, S4) |
| `UsdGenCombineMap` | v1 | `rel usdGen:combine:inputs` (ordered) · `token usdGen:combine:mode = "multiply"` (`multiply \| add \| subtract \| max \| min \| average \| overlay`, usdRig's `RigExecCombineWeight` vocabulary, `research/A1-usdrig-graph.md` §6) |
| `UsdGenGuideProximityMap` | v1 | `rel usdGen:guides` · `float usdGen:proximity:radius = 1.0` · `float usdGen:proximity:decay = 2.0` |

`usdGen:map:filter` is declared by **two** types with **two different** allowed sets. A codeless
schema carries `allowedTokens` per declaring class, so both lists are authored — `nearest | bilinear`
on `UsdGenImageMap`, the eight `PtexFilter::FilterType` names on `UsdGenPtexMap` — exactly as
`usdGen:mode` carries a different set per operator type (§2.7.1, §2.7.2). The §6 **On type** column is what
keeps the two apart at the router. `07-look-maps-expressions.md` §5.1–§5.4 owns the sampling
semantics of every row above and may not disagree with it (ADR §9 R7).

**One `asset` per map prim, never `asset[]`.** `FlagAsAssetPathDependent` is specialised for scalar
`SdfAssetPath` only (`pxr/usdImaging/usdImaging/dataSourceAttribute.h:203-212`, MEASURED
`research/G-stage-free-parameter-and-time-transport.md` §2), so an `asset[]` map list would get no
reload tracking at all. Several maps means several map prims (S13). Overwriting a map file on disk
invalidates nothing; reload is the explicit `UsdGenImaging_ReloadMaps()` action, which sends
`ArNotice::ResolverChanged` and bumps usdGen's own texture generation counter (S13; MEASURED
probe3 H).

A `UsdGenPaintMap` is what the paint brush writes **during** a session: a plain primvar on the
surface mesh (`primvars:usdGen:paint:density`, faceVarying or vertex), which an artist can inspect,
diff and hand-edit. A separate "Bake maps" action turns it into a `.ptx`/EXR when it needs to leave
the session; no image file is written during interaction (`research/A3-usdrig-tools.md` §7.2).

### 2.13 `UsdGenMaskAPI` — the universal mask block

> **SUPERSEDED 2026-09-15.** `UsdGenMaskAPI` was deleted; nothing is auto-applied any more.
> The mask is now one connectable attribute, `float usdGen:mask = 1.0`, declared on the abstract
> `UsdGenStyler` and `UsdGenDeformer` (generators declare none), and a mask is an ordinary
> attribute connection to a `UsdGenExpression` — to the prim or to its `outputs:result`.
> Everything below is history; `docs/freezes/C1.md` §9 is the live account.

Single-apply API schema, **auto-applied to `UsdGenOperator`** and therefore present on every
derived operator type (§7.4). Auto-apply is mandatory rather than a convention: an *unapplied* API
schema's properties are absent from `UsdPrimDefinition::GetPropertyNames()`, which is exactly the
list the adapter's mappings are built from, so a hand-forgotten `prepend apiSchemas` would silently
drop the whole mask (`design/judge-evidence.md` §2.1). The tool also applies it explicitly on
creation until gate **SI-8** proves auto-apply works on this codeless domain (ADR §2.1).

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:mask:source` | `rel` | — | At most one `UsdGenMap`. Composition of several maps is a `UsdGenCombineMap` (usdRig's weight-object shape, `research/A1-usdrig-graph.md` §6). | structural |
| `usdGen:mask:amount` | `float` | `1.0` | Scale on the resolved per-curve weight. | value |
| `usdGen:mask:invert` | `bool` | `false` | Applied after the range remap. | value |
| `usdGen:mask:range` | `float2` | `(0,1)` | Remap of the source value before use: `(s - x) / (y - x)`, clamped. | value |
| `usdGen:mask:rangeMode` | `uniform token` | `"normalized"` | `normalized \| absoluteLength` — the axis the along-curve ramp is evaluated on. | structural |
| `usdGen:mask:combine` | `uniform token` | `"multiply"` | `multiply \| add \| subtract \| max \| min \| average \| replace` — how the random term combines with the map term. Deliberately **not** the same set as `usdGen:combine:mode` on `UsdGenCombineMap` (§2.12), which is usdRig's `RigExecCombineWeight` vocabulary `multiply \| add \| subtract \| max \| min \| average \| overlay` (`research/A1-usdrig-graph.md` §6): `replace` is meaningful only for the random term, `overlay` only for map composition. | structural |
| `usdGen:mask:random` | `float` | `0.0` | Per-curve `rand(randomSeed, curveId)` multiplier amount, a host groomer's `rand()` masks. | capture |
| `usdGen:mask:randomSeed` | `uniform int` | `0` | | capture |
| `usdGen:mask:ramp:knots` | `float2[]` | `[(0,1),(1,1)]` | Along-curve ramp, `(position, value)`, sorted by position (S11). | value |
| `usdGen:mask:ramp:interpolation` | `uniform token` | `"catmullRom"` | `linear \| catmullRom \| bspline \| constant` (ADR §9 R11). §2.17 states how each builds its LUT. | value |
| `usdGen:mask:ramp:spline` | `float` | — | **v2.** The whole-`TsSpline` transport of §2.17. Authored with `.spline`; the adapter publishes it as `HdTypedSampledDataSource<TsSpline>` and never flags it time-varying. This is *not* an animated scalar. | value |
| `usdGen:mask:rangeMin` | `float` | `0.0` | a DCC's four-parameter curve-mask shortcut, used when fewer than two ramp knots are authored. | value |
| `usdGen:mask:rangeMax` | `float` | `1.0` | | value |
| `usdGen:mask:effectPosition` | `float` | `0.5` | | value |
| `usdGen:mask:falloff` | `float` | `0.5` | Shapes the shoulders of the shortcut band. | value |
| `usdGen:mask:influenceWidth` | `float` | `0.5` | `[0,1]`. Width of the band the shortcut's falloff shapes — the fifth a DCC curve-mask parameter (`research/A7-prior-art-grooming.md` §7). Consumed by `04-operators.md` §5.4's shortcut formula. | value |
| `usdGen:mask:noise:amount` | `float` | `0.0` | a DCC "Noise Mask", evaluated on the rest root position. | capture |
| `usdGen:mask:noise:frequency` | `float` | `1.0` | | capture |
| `usdGen:mask:noise:gain` | `float` | `0.5` | | capture |
| `usdGen:mask:noise:bias` | `float` | `0.5` | | capture |
| `usdGen:mask:noise:seed` | `uniform int` | `0` | | capture |
| `usdGen:mask:region` | `rel` | — | A region/parting map (per-face `int` or colour) that constrains membership. Consumed by `UsdGenGuideInterpolate` (guides from a different region never blend) and by `UsdGenClump` (clumps never cross a region boundary) in v1; other operators ignore it and say so in the panel. A parting-line **operator** (`UsdGenPart`) and its brush are v2 (ADR §2.3). | capture |

Resolved weight, computed **once per capture** into one `VtFloatArray` per operator plus one
257-entry ramp LUT (usdRig's falloff-LUT precedent, `research/A1-usdrig-graph.md` §6):

```
remap(s, x, y) = clamp( (s - x) / max(y - x, 1e-6), 0, 1 )      # the guard: x == y must not divide by 0

s      = mask:source ? sample(mask:source, rootUV(c)) : 1.0
s      = remap( s, mask:range.x, mask:range.y )
s      = mask:invert ? 1 - s : s
r      = lerp( 1.0, UsdGenDraw01(mask:randomSeed, curveId(c), kSaltMaskRandom), mask:random )
n      = 1 + mask:noise:amount * (biasGain(fbm(restRoot(c) * mask:noise:frequency,
                                                mask:noise:seed),
                                            mask:noise:bias, mask:noise:gain) * 2 - 1)
curveMask(c) = clamp( combine( mask:combine, mask:amount * s, r )
                      * n * region(c) * lockedCurveSuppression(c), 0, 1 )
rampLUT[i]   = ramp(i / 256)                                   i = 0..256
t            = hairT(i) * 256 ;  j = min(int(t), 255) ;  a = t - j
rampWeight   = (1 - a) * rampLUT[j] + a * rampLUT[j + 1]        # lerp; the 257th entry keeps j+1 in range
w(c, i)      = usdGen:blend * curveMask(c) * rampWeight
```

Five terms are load-bearing and are stated here because this block is canonical for the mask
**arithmetic** as well as its names and tokens (ADR §9 R16), and `04-operators.md` §5.2's pseudocode
must be executable and identical to it: the `max(y - x, 1e-6)` guard in `remap`;
`lockedCurveSuppression(c)` **inside** the clamp (this is how `UsdGenSculptLayer`'s
`usdGen:sculpt:lockedCurves` freezes a curve for every downstream styler at once, §2.10); the
`usdGen:blend` envelope factor in `w(c, i)`; `region(c)`, which is 1 unless `usdGen:mask:region`
rejects the curve; and `UsdGenDraw01`, the call-site spelling of §2.19.1's per-curve draw.
`w(c, i) == 0` is an early-out — the operator writes the input value bitwise.
`04-operators.md` §5.2 owns the `combine` truth table and the `bias`/`gain`/`biasGain` definitions,
and `04-operators.md` §5.4 owns the `rangeMin`/`rangeMax`/`effectPosition`/`influenceWidth`/`falloff`
shortcut.

MEASURED anchors (`appendix-A-evidence-ledger.md` §2.8 rows **EV-067**, **EV-069**, **EV-070**;
`research/A8-seexpr-ptex-libs.md` §1.6, §2.8, S38): SeExpr evaluation is 13–117 ns/eval (EV-067) and
a Ptex lookup 23–26 ns (EV-070), both single-threaded, with ≈50 M evals/s (EV-069) and 228 M
lookups/s (EV-070) aggregate on 8 threads. **DERIVED from EV-067/EV-069**, not measured end to end:
a 1 M-root mask is 13–117 ms of single-threaded **capture** and zero per frame; on the measured
8-thread aggregate it is ≈ 20 ms — the same derivation and the same figure `04-operators.md` §5.2
prints, and the two must stay equal.
The mask capture is measured by gates **L-3/L-4/L-5** (T0–T1, M4 — the map, Ptex and expression
gates; `09-performance-and-benchmarks.md` §5 is the gate registry, ADR §9 R40). Gate **E-4** covers
only the nanoflann kNN capture that dominates `UsdGenGuideInterpolate` and `UsdGenClump`.

### 2.14 `UsdGenLookAPI`

Single-apply, applied to `UsdGenDescription`. It exists so the `UsdPreviewSurface` fallback, the
glslfx preview and the MaterialX chain agree on one baked albedo instead of leaving root/tip colour
on one shader (`design/judge-artist.md` §2.3).

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:look:rootColor` | `color3f` | `(0.035, 0.018, 0.008)` | Root end of the albedo ramp. | capture |
| `usdGen:look:tipColor` | `color3f` | `(0.210, 0.115, 0.045)` | Tip end. | capture |
| `usdGen:look:colorRamp:positions` | `float[]` | `[]` | Optional multi-stop ramp overriding the two colours above. Colour ramps are `positions` + `colors`, never a `TsSpline`, because `TsSpline` supports only `double/float/GfHalf/GfTimeCode` (`pxr/base/ts/types.h:32-37`, ADR §1 S11). | capture |
| `usdGen:look:colorRamp:colors` | `color3f[]` | `[]` | Same length as `positions`. | capture |
| `usdGen:look:colorRamp:interpolation` | `uniform token` | `"catmullRom"` | `linear \| catmullRom \| bspline \| constant` (ADR §9 R11). | capture |
| `usdGen:look:rampExponent` | `float` | `1.0` | Exponent applied to `hairT` before the root→tip lerp; > 1 keeps the root colour longer. Feeds the glslfx input named `colorRamp` (C5); the two names differ on purpose and only the glslfx one is frozen by C5 (`07-look-maps-expressions.md` §1.1, ADR §9 R8). | capture |
| `usdGen:look:colorMapMode` | `uniform token` | `"bake"` | `bake` (CPU at capture) \| `shader` (the tool wires the map to the shader's texture input and bakes nothing). Legal as `shader` only for a `UsdGenImageMap` on the surface's `st` (`07-look-maps-expressions.md` §5.6). | structural |
| `usdGen:look:bakeMode` | `uniform token` | `"perCurve"` | `perCurve \| perCV` (ADR §9 R8). Chooses the **granularity** of the bake — `perCurve` ⇒ one `uniform` value per curve, `perCV` ⇒ one `vertex` value per CV — while `usdGen:look:bakeTarget` chooses its **destination**; the no-bake state is expressed once, by `bakeTarget = "none"`, so `bakeMode` has no `none` token. | structural |
| `usdGen:look:colorMap` | `rel` | — | A `UsdGenMap` sampled at the root UV and multiplied in. | structural |
| `usdGen:look:hueJitter` | `float` | `0.0` | Per-curve, driven by `hairId`. | capture |
| `usdGen:look:valueJitter` | `float` | `0.0` | | capture |
| `usdGen:look:jitterSeed` | `uniform int` | `0` | | capture |
| `usdGen:look:bakeTarget` | `uniform token` | `"displayColor"` | `displayColor \| primvar \| none`. `primvar` bakes per-CV colour into `usdGen:look:bakePrimvar` at 12 B/CV (DERIVED: one `color3f` = 3 × 4 B) and is off by default. | structural |
| `usdGen:look:bakePrimvar` | `token` | `"usdGen:albedo"` | Used when `bakeTarget = "primvar"`. | structural |

Bake order, per curve, at capture (risk §7.3, adopted verbatim):
`albedo = ramp(hairT_root) × colorMap(rootUV) × jitter(hairId)` → `displayColor` (`uniform`).
`07-look-maps-expressions.md` owns the details and the three-terminal `Material`.

### 2.15 `UsdGenRestAPI`

Single-apply, applied to a bound surface `Mesh`. It exists because the deformed points are what the
scene index carries and the rest points are what density, scatter ids, root frames and noise fields
must be computed from — and because downstream of usdRig the stage's default value is not reachable
at all through the chain: `RigExecResultsSceneIndex` **replaces** `primvars/points` with a retained
array (`research/A2-usdrig-imaging.md` §3.2).

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `usdGen:rest:source` | `uniform token` | `"default"` | `default` (sample `points` at `UsdTimeCode::Default()`) \| `primvar` (use `primvars:rest`) \| `asset` (read a rest mesh from a file). | capture |
| `usdGen:rest:primvar` | `token` | `"rest"` | Primvar name for `source = "primvar"`. An authored `primvars:rest` is honoured automatically even under `source = "default"`. | capture |
| `usdGen:rest:file` | `asset` | `@@` | For `source = "asset"`. One scalar `asset`, so reload tracking works. | capture |

The API-schema adapter publishes the container `usdGen/rest/{points, faceVertexCounts,
faceVertexIndices, st}` from a custom `UsdImagingDataSourceMapped::AttributeMapping::factory`
whose data source calls `Get<T>(&r, UsdTimeCode::Default())`
(`pxr/usdImaging/usdImaging/dataSourceMapped.h:83-106`). MEASURED: at stage time 24 the same prim
yields `deformed = [(0,0,5)…]` and `rest = [(0,0,0)…]`, and only the deformed channel is flagged
time-varying — the rest channel costs **no per-frame dirty**
(`research/G-stage-free-parameter-and-time-transport.md` §3, route R2). Capture-on-first-cook is
rejected (S12): it is wrong whenever the first frame drawn is not the rest frame.

**Invalidation is not free.** An adapter must author its own locator mapping for its container to be
dirtiable — `usdImaging/geomSubsetAdapter.cpp:165-185` is the pattern. So
`UsdGenRestAPIAdapter::InvalidateImagingSubprim` maps the *authoring* properties it reads onto its
own container: `points` → `usdGen/rest/points`, `faceVertexCounts` / `faceVertexIndices` → the
matching leaves, `primvars:rest` → `usdGen/rest/points`, and `usdGen:rest:{source,primvar,file}` →
their own leaves. Without this mapping the container is published but never invalidated, and §6.4's
`usdGen/rest/*` row can never fire. §9 test 6 asserts it.

### 2.16 `UsdGenCurveAPI`

Single-apply, applied to every curve prim that participates in contract C3 (§5): guides, freezes,
imports and sim caches. It exists so the C3 primvars have schema fallbacks, appear in
`UsdPrimDefinition::GetPropertyNames()`, and can be validated by one predicate
(`prim.HasAPI<UsdGenCurveAPI>()` in tools; a `TfType` query in the engine).

| Property | Type | Default | Doc | Dirty class |
|---|---|---|---|---|
| `primvars:usdGen:role` | `token` | `"hair"` | `hair \| guide`. Authored `constant`. The only marker that distinguishes a guide set's curves from a freeze's. | structural |
| `primvars:usdGen:curveId` | `uint64[]` | `[]` | Stable per-curve id, `uniform`. **64-bit** (ADR §9 R12). The key sculpt layers, re-freezes and id-hash decimation match on. | structural |
| `primvars:usdGen:frozenEpoch` | `string` | `""` | Staleness digest, `constant`. §8.5. | capture |
| `primvars:usdGen:rootFrame` | `matrix4d[]` | `[]` | Optional rest frame per root, `uniform`, for `UsdGenDeform`'s `rigidFrame` mode. Absent means "recompute from `skinprim`/`skinprimuv`". | capture |

These are `primvars:` rather than plain `usdGen:` attributes on purpose. usdGen uses the
`primvars:usdGen:` form in exactly two places: this C3 curve contract (§5) and the paint-brush
surface primvars a `UsdGenPaintMap` reads (§2.12). Operator *parameters* are never `primvars:` —
that form is the M1 stop-condition fallback only (ADR §7). MEASURED (`research/G-freeze-bake-undo-and-frozen-reentry.md`
§2.3): a custom non-primvar attribute `usdGen:frozenEpoch` on a typed `BasisCurves` reaches the
scene index **not at all** and emits **no notice** on create or on change, while
`primvars:usdGen:frozenEpoch` reaches it as `interp=constant` and dirties precisely as
`primvars/usdGen:frozenEpoch`. The same measurement kills `customData` for this purpose, which has a
second trap: `Usd.Prim.GetCustomDataByKey("usdGen:frozenEpoch")` returns `None` because it splits on
`:` as a nested-dict key path.

Interpolation is authored per prim, not fixed by the schema (as `UsdGeomGprim` does for
`primvars:displayColor`); C3 (§5) fixes it. The three unprefixed primvars C3 also requires —
`primvars:rest`, `primvars:skinprim`, `primvars:skinprimuv` — are **deliberately not** declared
here: they are a DCC convention names (`research/A7-prior-art-grooming.md` §3.6), and a groom
exported from a DCC or a host renderer must satisfy C3 without applying a usdGen schema at all.

### 2.17 Ramp encodings

Ratified by ADR §1 (S11). Three encodings, no others; the choice is by value type, not by taste.

| Encoding | Shape | When |
|---|---|---|
| **Scalar ramp** (default) | `float2[] usdGen:<p>:knots` — x = position `[0,1]`, y = value, sorted by x — plus `uniform token usdGen:<p>:interpolation` ∈ `linear \| catmullRom \| bspline \| constant`, default `"catmullRom"` (ADR §9 R11) | every scalar ramp: mask ramp, clump profile, width taper, noise magnitude |
| **Colour ramp** | `float[] usdGen:<p>:positions` + `color3f[] usdGen:<p>:colors` + the same interpolation token | every colour ramp. A colour ramp **cannot** be a `TsSpline`: `TS_SPLINE_SUPPORTED_VALUE_TYPES` is `double, float, GfHalf, GfTimeCode` (`pxr/base/ts/types.h:32-37`) |
| **Whole-`TsSpline` ramp** (v2, optional) | `float usdGen:<p>:spline` authored with `.spline`; the adapter reads `UsdAttribute::GetSpline()` (`pxr/usd/usd/attribute.h:563`; `HasSpline` at `:552`, `SetSpline` at `:568` — `appendix-A-evidence-ledger.md` §3.8) through an `AttributeMapping::factory` and publishes `HdRetainedTypedSampledDataSource<TsSpline>` **without** calling `FlagAsTimeVarying`. The spline's parameter axis is reinterpreted as root→tip `u`. | when an artist wants native USD spline authoring and full Ts interpolation modes |

The 257-entry LUT for every scalar ramp is built by **usdGen's own** evaluator, not by SeExpr
`curve()`: `constant` holds the previous knot's value, `linear` lerps, `catmullRom` is the
uniform Catmull-Rom spline through the knots with clamped endpoints, `bspline` is the cubic B-spline
approximating them. usdGen therefore never has to map its token onto a SeExpr `curve()` code, and
the four tokens are exactly ADR §9 R11's set — one vocabulary, everywhere.

**A `.spline` authored on a ramp's knot, position or colour property — `usdGen:<p>:knots`,
`usdGen:<p>:positions`, `usdGen:<p>:colors` — or on a bare `usdGen:<p>` where a ramp is expected, is
a compile error naming the property.** The one legal `.spline` inside a ramp is the dedicated
`float usdGen:<p>:spline` of row 3, which the adapter transports through its own
`AttributeMapping::factory` and never flags time-varying. (ADR §1's S11 sentence "a `.spline` on a
*ramp* property is a compile error" means the ramp's value/position properties;
`design/proposal-performance.md` §4.2 states it exactly: "a `.spline` on a ramp **position** is a
compile error".) The reason: a plain `.spline` float attribute is deemed
time-varying by `UsdStage` without analysis (`pxr/usd/usd/stage.cpp:9751-9758`: "all splines are
deemed as possibly time varying") while `GetTimeSamplesInInterval` returns nothing, so
`FlagAsTimeVarying` fires and the parameter is **dirtied on every `SetTime`** — a full recook per
frame on a static ramp (MEASURED, `research/G-stage-free-parameter-and-time-transport.md` §2.1,
probe3 D/E). The whole-spline transport of row 3 avoids this precisely because the custom factory
does not raise that flag (MEASURED, probe6: the terminal index returns the spline intact, 2 knots,
`Eval(u=0) = 0.1`, `Eval(u=24) = 0.9`).

A `.spline` on a **plain scalar parameter** is legal and means "this value animates": global
density, a wind gust, `usdGen:blend` on a shot-specific override. Those are excluded from the
structural digest (S26) and cost one dirty per pulled attribute per `SetTime`, which is correct.

### 2.18 The motion block

Three properties on `UsdGenDescription` (§2.3), one profile each (S32; ADR §2.3):

| `usdGen:motion:mode` | Profile | Published | Cost |
|---|---|---|---|
| `"single"` (default) | P0 | one `points` sample; `velocities`/`accelerations` **blocked** | none. Storm never samples anyway |
| `"velocities"` | P1 | `points` + `velocities` (and `accelerations` when the surface has them); upstream velocities are blocked so they cannot be double-applied | one extra pass over the tail |
| `"samples"` | P2 | `points` at `sampleCount` shutter offsets, **constant CV count across the shutter** | the deformed-space tail re-runs per offset, from `UsdGenMotionCache` keyed by `(graphGen, surfaceGen, absTime)` (ADR §4.1) |

Motion samples are **never baked into the stage** — a freeze authors `velocities`, never per-sample
`points` (S42). P2 is lazy: the samples are computed on the first interval pull or on an
application preflight, never speculatively. `usdGen:motion:forwardSurfaceSamples = false` clamps to
the shutter, because a surface with samples only at frames 1 and 24 returns the *far* bracketing
sample from `GetContributingSampleTimesForInterval(-0.25, 0.25)` (MEASURED,
`research/G-stage-free-parameter-and-time-transport.md` §3).

### 2.19 Versioning properties

| Property | Where | Type / default | Purpose |
|---|---|---|---|
| `usdGen:schemaVersion` | `UsdGenGroom` | `uniform int = 1` | Major version of the `usdGen:` vocabulary the groom was authored against. Refuse-and-warn on newer (§8.4). |
| `usdGen:algorithmVersion` | every `UsdGenOperator` | `uniform int = 0` | `0` = "track the newest kernel". Tool-authored prims, freezes and bakes always carry the explicit current integer, so their look is preserved when a kernel is fixed (§8.3, ADR §9 R17). |
| `primvars:usdGen:frozenEpoch` | C3 curves | `string`, `constant` | `usdgen1:sha1:<40 hex>`. The `usdgen<N>` prefix carries the schema major, so a freeze made by a newer plugin reads as *stale*, never as silently wrong (§8.5). |

#### 2.19.1 The hash family and its salts

Every stable id and every random draw in usdGen comes from one pinned hash (ADR §9 R12). Changing
the function **or any salt** is a look change for every existing asset, so it bumps
`usdGen:schemaVersion`.

```cpp
// usdGenMath/hash.h — SplitMix64 finalizer over the salted key.
inline uint64_t UsdGenHash64(uint64_t key, uint32_t salt) {
    uint64_t z = (key ^ (uint64_t(salt) * 0x9E3779B97F4A7C15ull)) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline uint32_t UsdGenHash32(uint64_t key, uint32_t salt) {           // the high 32 bits
    return uint32_t(UsdGenHash64(key, salt) >> 32);
}
inline float    UsdGenHash01(uint64_t key, uint32_t salt) {           // [0,1)
    return float(UsdGenHash32(key, salt) >> 8) * 0x1.0p-24f;
}
// Multi-key forms fold left over the same salt, one call per key:
//   UsdGenHash64(a, b, salt)    = UsdGenHash64(UsdGenHash64(a, salt) ^ b, salt)
//   UsdGenHash64(a, b, c, salt) = UsdGenHash64(UsdGenHash64(a, b, salt) ^ c, salt)
// and UsdGenHash32 / UsdGenHash01 take the same argument lists, folding with UsdGenHash64
// and converting only at the end. So a scatter root's id is
//   curveId = UsdGenHash64(uint64(seed), uint64(faceIndex), uint64(k), kSaltScatter)
// which `04-operators.md` §0.6 spells UsdGenCurveId(seed, faceIndex, k).
// The call-site form for a per-curve draw inside an operator is named, because the two-argument
// UsdGenHash01 above cannot express it (`04-operators.md` §0.6, the executable spelling):
inline float    UsdGenDraw01(int seed, uint64_t curveId, uint32_t salt) {
    return UsdGenHash01(UsdGenHash64(uint64_t(uint32_t(seed)), salt) ^ curveId, salt);
}
// So the per-curve mask draw of §2.13 is UsdGenDraw01(mask:randomSeed, curveId, kSaltMaskRandom).
```

Salts are per-use **compile-time constants** named `kSalt<Use>` — `kSaltScatter`, `kSaltDensity`,
`kSaltNoise`, `kSaltMaskRandom`, `kSaltClump + level`, … — and `04-operators.md` §0.6 carries the
per-operator-type table. Two derived quantities matter across documents:

* `hairId` (uniform **float** on the published tile) `= UsdGenHash32(curveId, 0) / 2^32` ∈ [0,1);
  the shipped glslfx declares it `float` (ADR §1 S29).
* Density decimation uses `kSaltDensity ≠ 0` (§2.3.1), so the surviving set is never
  `{hairId < keepFraction}` and the shader's per-curve jitter stays uniform at every density.

### 2.20 `usdGen:surface` and `GeomSubset` semantics

`usdGen:surface` accepts `UsdGeomMesh` prims and `UsdGeomSubset` prims, in any mixture, on the
groom, the description or a single operator (ADR §2.3). The rules, settled here because all three
proposals accepted subsets and none defined them (`design/judge-delivery.md` §5, blocker item 4):

1. A `GeomSubset` target restricts the operator to the faces named by its `int[] indices`. The
   subset must author `uniform token elementType = "face"` (`pxr/usd/usdGeom/subset.h:168-226`).
   The engine never sees that token: the adapter converts it, so what arrives is
   `geomSubset/type == HdGeomSubsetSchemaTokens->typeFaceSet` on a Hydra prim of type
   `geomSubset` (`pxr/usdImaging/usdImaging/geomSubsetAdapter.cpp:77-85` maps `face` →
   `typeFaceSet`, `point` → `typePointSet` and `TF_WARN`s on anything else; `:125-131` returns the
   subprim type). That path — `geomSubset/type` — is where the **value** is read; the locator the
   adapter *dirties* is the bare leaf `type` (rule 5). Any value other than `typeFaceSet` is a hard
   diagnostic naming the prim. The
   authored form is the stock one — the subset is a namespace child of the mesh and carries no
   usdGen property:

   ```usda
   def Mesh "Scalp" ( prepend apiSchemas = ["UsdGenRestAPI"] )
   {
       def GeomSubset "Crown"
       {
           uniform token elementType = "face"
           uniform token familyName  = "usdGenSurfaces"   # artist grouping only; never read (rule 5)
           int[]         indices     = [12, 13, 14, 40, 41, 71, 96]   # parent-mesh face indices
       }
   }
   # then, on the description or on one operator:  rel usdGen:surface = </Char/Scalp/Crown>
   ```
2. **Face indices are always indices into the parent mesh's faces.** `primvars:skinprim` and the
   root binding are parent-mesh face indices, and a subset never renumbers them. This is the single
   rule that keeps a freeze valid when an artist later widens the subset. **Ptex is one hop
   further:** a ptex face id is `Far::PtexIndices::GetFaceId(parentFace)` plus the sub-face
   quadrant, so a quad maps 1:1 but an n-gon occupies **n consecutive** ptex ids
   (`opensubdiv/far/ptexIndices.h:46-88`; `research/A8-seexpr-ptex-libs.md` §2.6, S38). usdGen
   builds the mesh-face → (ptexId, quadrant) map once per capture from the parent mesh — through
   `PxOsdRefinerFactory::Create` (`pxr/imaging/pxOsd/refinerFactory.h:34-41`) — never from the
   subset, and supports `mt_triangle` files only for all-triangle meshes, because Storm's
   triangulation is fan triangulation and does not agree with Ptex's tri sub-face rule
   (`research/A8-seexpr-ptex-libs.md` §2.6, last paragraph).
3. Density is computed on the subset's faces only, using their rest area, so
   `usdGen:density` means the same number of hairs per square unit whether it is bound to a mesh or
   to a subset of it.
4. Several subsets of the same mesh on one `usdGen:surface` are unioned; a face named by two
   subsets is scattered once.
5. **A subset edit is a recapture**, not a value edit: `indices` and `elementType` are
   capture-class, routed on the subset's own Hydra prim. The exact locators are the **bare** leaves
   `indices` and `type` — *not* `geomSubset/indices` / `geomSubset/type` — even though the data
   source itself lives under the `geomSubset` container
   (`pxr/usdImaging/usdImaging/geomSubsetAdapter.cpp:165-185` inserts
   `HdDataSourceLocator(HdGeomSubsetSchemaTokens->indices)` and `…->type`, both single-element,
   while `pxr/imaging/hd/geomSubsetSchema.cpp:119-121` makes the container's default locator
   `geomSubset`; `pxr/usdImaging/usdImaging/stageSceneIndex.cpp:855-876` forwards `dirtyLocators`
   unmodified). The adapter's invalidation and its data source therefore disagree — an upstream
   asymmetry usdGen must handle rather than assume away, filed in
   `12-risks-decisions-open-questions.md` beside S46's bug list. The router keys a `geomSubset`-typed
   prim on the leaf locators `indices` and `type`, and **additionally accepts** the prefixed
   spellings in case the asymmetry is fixed upstream (§6.4). Roots on removed faces disappear, roots
   on kept faces retain their ids, and roots on added faces get new ids from
   `UsdGenHash64(seed, faceIndex, k, kSaltScatter)` — so widening a subset does not re-roll the
   existing groom. `familyName` is **not** published to Hydra at all: the adapter builds only
   `HdGeomSubsetSchema::Builder().SetIndices(...).SetType(...)`
   (`geomSubsetAdapter.cpp:135-162`), and the whole Hydra token set is
   `(geomSubset)(type)(indices)(typeFaceSet)(typePointSet)(typeCurveSet)`
   (`pxr/imaging/hd/geomSubsetSchema.h:36-41`). It is therefore authored for artist grouping only
   and is never read by the evaluator — making it load-bearing would need a `UsdStage` downstream
   of the stage scene index, which S8 forbids. The example's `familyName = "usdGenSurfaces"` is a
   convention, not a selector.
6. When the target is an **instance proxy**, S7's metadata-only `UsdImagingSceneIndexPlugin`
   translates it to the prototype path through `ProxyPathTranslationDataSourceNames()`
   (`pxr/usdImaging/usdImaging/sceneIndexPlugin.h:90-92` — a **non-const** virtual; its sibling
   `InstanceDataSourceNames()` is at `:81-83`).
7. A target that resolves to neither a `Mesh` nor a face `GeomSubset` is a hard diagnostic
   (`TF_WARN` naming both the operator and the target) and the description publishes nothing;
   silence here produced the worst debugging experience in the prior-art survey.
8. **Every relationship that needs a mesh accepts a face `GeomSubset` (2026-09-27).** Both
   builders resolve a subset into ONE self-contained `UsdGenSurfaceDesc` — its parent's full
   geometry, `path` = the subset, `isSubset`, and sorted, unique `subsetFaces` — so no consumer
   walks to a parent entry and face ids stay parent ids by construction. An empty subset selects
   no face (it never widens to the mesh); an index outside the parent is a hard diagnostic.
   What a subset restricts, and what still reads the whole parent:

   | Consumer | Restricted to the subset's faces | Whole parent mesh |
   |---|---|---|
   | `usdGen:surface` → Scatter (CPU and the CUDA scatter input), limit scatter, surface-cage fill | roots, rest area / density | limit-surface build, ptex ids |
   | `usdGen:colliders` (and the bound surface) → Collide | collision faces | — |
   | Deform RBF (CPU and CUDA `PrepareCudaSurface`), root frames, Grow | — | RBF samples, rest/posed points, root frames |
   | `usdGen:paint:surface` (PaintMap) | faces outside read `usdGen:map:default` | primvar is read from the parent; `paintSurface` records the parent |
   | expression `input:<name>` (`geoSampler`) | elements visited | `$id`/`$primIndex`/`$primCount` numbering |
   | Storm scalp-shadow occluders | — | the parent occludes, once per mesh |
   | `usdGen:tonic:scalp` (Tonic) | raycast BVH, closest point, root sampling, region fill and uncovered count, tint | geometry, graph face ids, `tonicRegion` primvar, UsdGenRestAPI, ptex bake layout |
   | brush tools (`usdGenBrushApi` face mask + Python twins) | pick, dab spill, smooth, flood, bake/live writes | the primvar (written on the parent, subset corners only) |
   | `usdGenBakePtex` on a subset path | voronoi seeds | the baked file (every parent face) |

   The routing snapshot carries `meshPaths` beside `surfacePaths`: points, topology and paint
   primvar rows key on the parent mesh, and the subset's own prim routes `indices`/`type` (bare
   and `geomSubset/`-prefixed) as a recapture (rule 5). Evidence: `tests/testUsdGenGeomSubset.cpp`.

---

## 3. Reserved layout

### 3.1 Authored namespace (normative)

```
<Groom>/                                UsdGenGroom
  <Description>/                        UsdGenDescription
    Ops/          Scope                 the operator chain (any prim names; order = usdGen:input)
    Guides/       Scope                 UsdGenGuideSet prims + their BasisCurves children
    Maps/         Scope                 UsdGenMap prims
    Prototypes/   Scope                 card/archive prototypes (pruned from the render)
    Frozen/       Scope                 freezes (siblings; the tool never re-authors this scope)
    __usdGenRender/                     HYDRA-ONLY, never authored (§3.2)
```

The five scopes are conventions the tool enforces and the panels group by; wiring is by
relationship, so a hand-written stage that puts an operator elsewhere still evaluates. Two of them
carry a rule rather than a preference:

* **`Prototypes`** must be **explicitly pruned** by the groom scene index. The free
  `primType = ''` forcing applies only to prims that are namespace children of the instancer
  (MEASURED, `research/G-instancing-cards-archives-and-native-instances.md` §3 case **(d)**,
  `prototypes/instancing/out-probe2.txt`; the mechanism is `_MakeUnrenderable` on a *strict prefix*
  of an instancer path, `piPrototypeSceneIndex.cpp:174-208`). An authored prototype that sits
  *outside* the instancer keeps `type = 'mesh'` and is drawn standalone (case **(c)**, MEASURED).
  usdGen's authored prototypes live at `<Description>/Prototypes/<n>` while the instancer is at
  `<Description>/__usdGenRender/inst_<opName>` — case (c)'s topology, not case (d)'s. So usdGen
  re-roots *copies* under `inst_<opName>/Prototypes/<n>`, which get the free hiding, and prunes the
  originals under `<Description>/Prototypes` itself. `06-imaging.md` §3 owns the pruning.
* **`Frozen`** is a dedicated scope so the freeze tool never re-authors a scope that holds anything
  else. Freezes are siblings inside it; a freeze may only be undone in the layer it was authored
  into, and undo of a live freeze is `SetActive(false)` (S41, S42).

### 3.2 The Hydra-only prims usdGen synthesises

Everything below is created by `usdGenImaging` into the scene index, carries
`primOrigin{scenePath}` (absolute outside prototypes, relative inside — ADR §5.3), and never
appears on a stage.

| Path under `<Description>/__usdGenRender/` | Hydra `primType` | Notes |
|---|---|---|
| `tile_0000` … `tile_NNNN` | `basisCurves` | the published tiles; contract C2, `06-imaging.md`. `chunksPerTile = max(1, ceil(nChunks / tileTarget))`; `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` (ADR §9 R21; `03-execution-engine.md` §1.4 carries the same form). The `min` term is what keeps the 32-tile clamp from asking for more tiles than there are chunks; empty tiles are never published, because a zero-curve `basisCurves` has no valid `extent`. Worked (**DERIVED** — pure arithmetic on R21's formula, not a measurement; ADR §9 R42's tags are MEASURED / DERIVED / UNMEASURED / ASSUMPTION and nothing else): 100 k curves at chunk 512 and `tileTarget` 64 → 196 chunks → 4 chunks per tile → **49 tiles**; 1 M curves → 1 954 chunks → 31 per tile → **64 tiles**. The 32-prim floor is therefore a **target**, not a guarantee, below 16 384 curves (**DERIVED** from the 512-curve chunk, ADR §4.1 **I2**, and the 32-tile floor) |
| `guides/<setName>` | `basisCurves` | `purpose = "guide"`, one per `UsdGenGuideSet` |
| `guides/<setName>/cvs` | `points` | CV display for the brushes (S40); widths + displayColor |
| `inst_<opName>` | `instancer` | one per `UsdGenInstance`; `instancerTopology/instanceIndices` is `HdIntArrayVectorSchema` (`pxr/imaging/hd/instancerTopologySchema.h:125-126`) |
| `inst_<opName>/Prototypes/<n>` | (the prototype's own type) | namespace children of the instancer; `instancedBy/paths` holds **exactly one** path or emulation raises `TF_CODING_ERROR` (`hd/sceneIndexAdapterSceneDelegate.cpp:2684-2688`) |
| `material_storm` | `material` | only when `USDGEN_STORM_MATERIAL_OVERRIDE=1` (gate L-1, ADR §5.6) |

An authored prim whose path contains a `__usdGenRender` component is ignored with one `TF_WARN`.
The prim set is allocated once per session and never changes during interaction (S27, S28); it is a
property of the **session**, not of a scene index, so a renderer switch re-attaches and continues
the same generation counter (ADR §4.5). The one exception is a `usdGen:tileTarget` edit, which is a
deliberate session-level re-allocation performed **outside** interaction (§6): it removes and
re-adds only Hydra-synthesized prims, never stage prims, so `RemovePrim` on a stage — S41's trap —
is not involved.

---

## 4. Canonical `.usda` examples

Every property used below is declared in §2 — §2.7.1 for styler parameters — and the types shown
there are normative (ADR §9 R7); `04-operators.md` §2.8–§2.15 restates them. All three blocks are
valid `.usda` as printed once the `[...]` array elisions are filled in; they are the fixtures for
the tier-T0/T1 tests of §9 (test 8).

### 4.1 Example (a) — scatter → guide interpolate → clump → clump → noise on a usdRig-deformed scalp

```usda
#usda 1.0
(
    defaultPrim = "Char"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "Char"
{
    def Mesh "Scalp" (
        prepend apiSchemas = ["UsdGenRestAPI"]
    )
    {
        int[]        faceVertexCounts  = [...]
        int[]        faceVertexIndices = [...]
        point3f[]    points            = [...]      # default = rest, timeSamples = deformed by usdRig (brief R5, S1)
        texCoord2f[] primvars:st       = [...] ( interpolation = "faceVarying" )
        float[]      primvars:usdGen:paint:density = [...] ( interpolation = "faceVarying" )
        uniform token usdGen:rest:source = "default" # usdGen/rest/points at UsdTimeCode::Default() (S12)
    }

    def Scope "Rig" { }                               # usdRig movers; usdGen never reads them

    def UsdGenGroom "Groom"
    {
        rel            usdGen:surface       = </Char/Scalp>
        float          usdGen:densityScale  = 1
        float          usdGen:renderDensityScale = 1
        uniform int    usdGen:schemaVersion = 1
        uniform string usdGen:sessionId     = "char.hair.v001"

        def UsdGenDescription "hair" (
            prepend apiSchemas = ["UsdGenLookAPI", "MaterialBindingAPI"]
        )
        {
            rel           usdGen:surface  = </Char/Scalp>
            rel           usdGen:terminal = </Char/Groom/hair/Ops/op05_frizz>
            rel           usdGen:guides   = </Char/Groom/hair/Guides/head>
            rel           material:binding = </Char/Looks/HairLook>
            uniform int   usdGen:tileTarget = 64
            uniform token usdGen:curve:basis = "bspline"
            float         usdGen:width:default = 0.008
            uniform token usdGen:motion:mode = "single"

            color3f usdGen:look:rootColor = (0.035, 0.018, 0.008)
            color3f usdGen:look:tipColor  = (0.210, 0.115, 0.045)
            float   usdGen:look:hueJitter = 0.06
            rel     usdGen:look:colorMap  = </Char/Groom/hair/Maps/coatTint>

            def Scope "Guides"
            {
                def UsdGenGuideSet "head"
                {
                    rel     usdGen:surface = </Char/Scalp>
                    float[] usdGen:blend   = [1, 1, 0.6, 1]
                    def BasisCurves "curves" ( prepend apiSchemas = ["UsdGenCurveAPI"] )
                    {
                        uniform token type  = "cubic"
                        uniform token basis = "bspline"
                        uniform token wrap  = "pinned"
                        uniform token purpose = "guide"
                        int[]        curveVertexCounts = [8, 8, 8, 8]
                        point3f[]    points  = [ ... 32 CVs ... ]
                        float[]      primvars:widths      = [...] ( interpolation = "vertex" )
                        point3f[]    primvars:rest        = [...] ( interpolation = "vertex" )
                        int[]        primvars:skinprim    = [12, 40, 71, 96] ( interpolation = "uniform" )
                        texCoord2f[] primvars:skinprimuv  = [...] ( interpolation = "uniform" )
                        uint64[]     primvars:usdGen:curveId = [0, 1, 2, 3] ( interpolation = "uniform" )
                        token        primvars:usdGen:role = "guide" ( interpolation = "constant" )
                    }
                }
            }

            def Scope "Maps"
            {
                def UsdGenPaintMap "densityPaint"
                {
                    rel           usdGen:paint:surface       = </Char/Scalp>
                    token         usdGen:paint:primvar       = "usdGen:paint:density"
                    token         usdGen:paint:interpolation = "faceVarying"
                    uniform token usdGen:map:domain          = "root"
                    uniform token usdGen:map:channel         = "r"
                }
                def UsdGenExprMap "lengthVar"
                {
                    uniform token usdGen:map:domain      = "root"
                    uniform token usdGen:expr:returnType = "float"
                    string usdGen:expr:source = '''
                        # length falls off toward the neck, plus 15% per-hair variation
                        $base = 1.0 - smoothstep(0.35, 0.75, $v);
                        $base * (0.85 + 0.30 * rand($id))
                    '''
                }
                def UsdGenImageMap "frizzMask"
                {
                    asset         usdGen:map:file    = @./maps/frizzMask.exr@
                    token         usdGen:map:uvSet   = "st"
                    uniform token usdGen:map:channel = "r"
                    uniform token usdGen:map:domain  = "root"
                }
                # UsdGenPtexMap is a v1 type landing in M4 (ADR §9 R38). The schema declares the
                # whole vocabulary at M1 (§8.1), so a PRE-M4 binary parses this prim, resolves the
                # map to its neutral value and raises one TF_WARN naming it (§8.2). A ptex face id
                # is Far::PtexIndices::GetFaceId(parentFace) plus the sub-face quadrant, so an
                # n-gon occupies n consecutive ptex ids (§2.20 rule 2, S38).
                def UsdGenPtexMap "coatTint"
                {
                    asset         usdGen:map:file         = @./maps/coat_colour.ptx@
                    uniform token usdGen:map:domain       = "root"
                    uniform token usdGen:map:channel      = "rgb"
                    token         usdGen:map:filter       = "bilinear"
                    float         usdGen:map:blur         = 0
                    int           usdGen:map:firstChannel = 0
                    int           usdGen:map:channelCount = 3
                }
                def UsdGenImageMap "regions"
                {
                    asset         usdGen:map:file    = @./maps/regions.exr@
                    uniform token usdGen:map:channel = "r"
                }
            }

            def Scope "Ops"
            {
                def UsdGenScatter "op01_scatter"
                {
                    uniform token usdGen:mode        = "random"
                    float         usdGen:density     = 2400
                    uniform int   usdGen:seed        = 7
                    int           usdGen:relaxIterations = 4
                    bool          usdGen:areaCompensation = 1
                    rel           usdGen:mask:source = </Char/Groom/hair/Maps/densityPaint>
                }

                def UsdGenGuideInterpolate "op02_interp"
                {
                    rel           usdGen:input   = </Char/Groom/hair/Ops/op01_scatter>
                    rel           usdGen:guides  = </Char/Groom/hair/Guides/head>
                    int           usdGen:maxGuides        = 3
                    float         usdGen:influenceRadius  = 6.0
                    float         usdGen:influenceDecay   = 2.0
                    float         usdGen:maxGuideAngle    = 75
                    float         usdGen:blendInSkinSpace = 1.0
                    uniform token usdGen:blendMethod      = "extrudeAndBlend"
                    int           usdGen:cvCount          = 8
                    float         usdGen:clumpCrossover   = 0.0
                    rel           usdGen:mask:region      = </Char/Groom/hair/Maps/regions>
                    rel           usdGen:length:source    = </Char/Groom/hair/Maps/lengthVar>
                }

                def UsdGenClump "op03_clumpBig"
                {
                    rel      usdGen:input          = </Char/Groom/hair/Ops/op02_interp>
                    float    usdGen:clump:amount   = 0.75
                    float    usdGen:clump:size     = 1.6
                    float    usdGen:clump:density  = 900
                    uniform int usdGen:clump:seed  = 12
                    int      usdGen:clump:levels   = 1
                    float    usdGen:clump:volumize = 0.15
                    float    usdGen:preserveLength = 0.9
                    float2[] usdGen:clump:profile:knots = [(0, 0), (0.35, 0.55), (1, 1)]
                    uniform token usdGen:clump:profile:interpolation = "catmullRom"
                    float    usdGen:clump:stray:amount = 0.25
                    float    usdGen:clump:stray:rate   = 0.06
                    float    usdGen:blend             = 1.0
                    float2[] usdGen:mask:ramp:knots    = [(0, 0), (0.2, 1), (1, 1)]
                    rel      usdGen:mask:region        = </Char/Groom/hair/Maps/regions>
                }

                def UsdGenClump "op04_clumpFine"
                {
                    rel      usdGen:input             = </Char/Groom/hair/Ops/op03_clumpBig>
                    float    usdGen:clump:amount      = 0.45
                    float    usdGen:clump:size        = 0.5
                    float    usdGen:clump:density     = 3600
                    uniform int usdGen:clump:seed     = 91
                    float    usdGen:clump:noise:amount      = 0.08
                    float    usdGen:clump:noise:frequency   = 3.0
                    float    usdGen:clump:noise:correlation = 0.7
                }

                def UsdGenNoise "op05_frizz"
                {
                    rel           usdGen:input            = </Char/Groom/hair/Ops/op04_clumpFine>
                    float         usdGen:noise:magnitude  = 0.09
                    float         usdGen:noise:frequency  = 5.0
                    float         usdGen:noise:correlation = 0.35
                    int           usdGen:noise:octaves    = 3
                    float         usdGen:preserveLength   = 1.0
                    uniform token usdGen:space            = "rest"
                    rel           usdGen:mask:source      = </Char/Groom/hair/Maps/frizzMask>
                    float2[]      usdGen:mask:ramp:knots  = [(0, 0), (0.4, 0.2), (1, 1)]
                    float         usdGen:mask:random      = 0.4
                    uniform int   usdGen:mask:randomSeed  = 3
                }
            }
        }
    }

    # A nested PrimSpec must be followed by an Eol, never by its parent's closing brace on the
    # same line (`pxr/usd/sdf/textFileFormatParser.h:1561-1569`).
    def Scope "Looks"
    {
        def Material "HairLook" { }   # three terminals; 07-look-maps-expressions.md
    }
}
```

No `prepend apiSchemas = ["UsdGenMaskAPI"]` appears on any operator: the schema auto-applies it
(§2.13, §7.4). `usdGen:mask:*` is therefore authorable on `op01_scatter` without a hand-applied
schema — the defect the evidence judge found in every proposal
(`design/judge-evidence.md` §2.1). Nothing in the file is written by the evaluator: the tiles, the
`hairT`/`hairId`/`st`/`displayColor` primvars, the `extent` and the material override are all
synthesized (§3.2).

### 4.2 Example (b) — frozen curves deformed with the surface, plus a sculpt layer

```usda
#usda 1.0
(
    defaultPrim = "Char"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "Char"
{
    def Mesh "Body" ( prepend apiSchemas = ["UsdGenRestAPI"] ) { }

    def UsdGenGroom "Groom"
    {
        uniform string usdGen:sessionId = "char.fur.v001"

        def UsdGenDescription "fur" ( prepend apiSchemas = ["MaterialBindingAPI"] )
        {
            rel           usdGen:surface  = </Char/Body>
            rel           usdGen:terminal = </Char/Groom/fur/Ops/op08_sculpt>
            uniform int   usdGen:tileTarget = 128
            uniform token usdGen:motion:mode = "velocities"  # P1: author velocities, never baked samples (S42)

            def Scope "Frozen"
            {
                # A plain BasisCurves under contract C3 (§5). Written by the freeze tool into a
                # sidecar .usdc sublayer (usdGen:frozen:tier = "sublayer"), or converted from an
                # Alembic sim cache. Never .usda.
                def BasisCurves "bake_v003" ( prepend apiSchemas = ["UsdGenCurveAPI"] )
                {
                    uniform token type  = "cubic"
                    uniform token basis = "bspline"
                    uniform token wrap  = "pinned"
                    int[]        curveVertexCounts = [8, 8, 8, ...]      # 120 000 curves
                    point3f[]    points  = [...]                         # Σ counts exactly (S28)
                    float[]      primvars:widths     = [...] ( interpolation = "vertex" )
                    point3f[]    primvars:rest       = [...] ( interpolation = "vertex" )
                    int[]        primvars:skinprim   = [...] ( interpolation = "uniform" )
                    texCoord2f[] primvars:skinprimuv = [...] ( interpolation = "uniform" )
                    uint64[]     primvars:usdGen:curveId = [...] ( interpolation = "uniform" )
                    string       primvars:usdGen:frozenEpoch = "usdgen1:sha1:8d31f0…" ( interpolation = "constant" )
                    token        primvars:usdGen:role = "hair" ( interpolation = "constant" )
                    matrix4d[]   primvars:usdGen:rootFrame = [...] ( interpolation = "uniform" )
                }
            }

            def Scope "Ops"
            {
                # kept, authored, greyed in the stack editor while the freeze is "frozen".
                # A PropertySpec must end in a StatementSeparator, so the one-line form needs the
                # trailing ';' (`pxr/usd/sdf/textFileFormatParser.h:264-265, 1561-1569`).
                def UsdGenScatter          "op01_scatter"  { uniform token usdGen:mode = "random" ; float usdGen:density = 8000 ; }
                def UsdGenGuideInterpolate "op02_interp"   { rel usdGen:input = </Char/Groom/fur/Ops/op01_scatter> ; }
                def UsdGenClump            "op03_clumpBig" { rel usdGen:input = </Char/Groom/fur/Ops/op02_interp> ; }

                def UsdGenFreeze "op06_bake"
                {
                    rel            usdGen:input         = </Char/Groom/fur/Ops/op03_clumpBig>
                    rel            usdGen:frozen:curves = </Char/Groom/fur/Frozen/bake_v003>
                    uniform token  usdGen:frozen:mode   = "frozen"
                    uniform string usdGen:frozen:epoch  = "usdgen1:sha1:8d31f0…"
                    uniform token  usdGen:frozen:tier   = "sublayer"
                }

                def UsdGenDeform "op07_deform"
                {
                    rel           usdGen:input      = </Char/Groom/fur/Ops/op06_bake>
                    uniform token usdGen:readPhase  = "final"   # after every other modifier on the surface
                    uniform token usdGen:mode       = "rigidFrame"
                    bool          usdGen:twistAware = 1
                    float         usdGen:preserveShape = 0.0
                    uniform token usdGen:space      = "deformed"
                }

                def UsdGenSculptLayer "op08_sculpt"
                {
                    rel            usdGen:input             = </Char/Groom/fur/Ops/op07_deform>
                    float          usdGen:sculpt:weight     = 1.0
                    uniform token  usdGen:sculpt:space      = "rootFrame"
                    uniform string usdGen:sculpt:epoch      = "usdgen1:sha1:8d31f0…"
                    uint64[]       usdGen:sculpt:curveIds   = [41, 42, 43, 88, ...]
                    int[]          usdGen:sculpt:cvOffsets  = [0, 8, 16, 24, ...]
                    vector3f[]     usdGen:sculpt:deltas     = [...]
                    uint64[]       usdGen:sculpt:lockedCurves = [88]
                }
            }
        }
    }
}
```

Three properties of this shape matter. **(1)** The frozen prim re-enters the graph straight from
the scene index — no stage read, no special loader (MEASURED,
`research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2), so a `BasisCurves` produced by usdGen, a
sim, an Alembic import or another department is *the same kind of styler input*. That is brief requirement R4's
"rigged or simulated curves" for free; with no freeze operator in the chain, use `UsdGenCurveSource`
(§2.6). **(2)** Sculpt deltas are keyed by `curveId`, so re-running the generator with the same seed
and surface keeps the comb work. **(3)** When `primvars:usdGen:frozenEpoch` stops matching
`usdGen:frozen:epoch`, the freeze is stale: one `TF_WARN` names both epochs, the stack editor
badges it, and the evaluator **keeps rendering the frozen data** — never a silent re-cook (ADR §9
R18). The artist re-freezes, unfreezes, or rebases the sculpt.

### 4.3 Example (c) — a cards / archives instancer description (v1, M6)

```usda
#usda 1.0
(
    defaultPrim = "Bird"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "Bird"
{
    def Mesh "Body" ( prepend apiSchemas = ["UsdGenRestAPI"] ) { }

    def UsdGenGroom "Groom"
    {
        def UsdGenDescription "featherCards"
        {
            rel usdGen:surface  = </Bird/Body>
            rel usdGen:terminal = </Bird/Groom/featherCards/Ops/op02_cards>
            uniform int usdGen:tileTarget = 32

            def Scope "Prototypes"             # pruned from the output, re-rooted under the instancer (S33)
            {
                def Mesh  "cardA" { }          # one quad, uv'd, its own material binding
                def Mesh  "cardB" { }
                def Xform "quill" ( prepend references = @./archives/quill.usda@ ) { }   # a subtree = an archive
            }

            def Scope "Maps"
            {
                def UsdGenImageMap "coverage" { asset usdGen:map:file = @./maps/coverage.exr@ ; }
            }

            def Scope "Ops"
            {
                def UsdGenScatter "op01_scatter"
                {
                    uniform token usdGen:mode        = "random"
                    float         usdGen:density     = 40
                    uniform int   usdGen:seed        = 3
                    rel           usdGen:mask:source = </Bird/Groom/featherCards/Maps/coverage>
                }

                def UsdGenInstance "op02_cards"
                {
                    rel usdGen:input      = </Bird/Groom/featherCards/Ops/op01_scatter>
                    uniform token usdGen:primitive = "cards"
                    rel usdGen:prototypes = [ </Bird/Groom/featherCards/Prototypes/cardA>,
                                              </Bird/Groom/featherCards/Prototypes/cardB>,
                                              </Bird/Groom/featherCards/Prototypes/quill> ]
                    float[]       usdGen:instance:weights      = [0.6, 0.3, 0.1]
                    uniform token usdGen:instance:orient       = "surfaceFrame"
                    float         usdGen:instance:scale        = 1.0
                    float2        usdGen:instance:scaleRandom  = (0.85, 1.2)
                    float         usdGen:instance:twist        = 0.0
                    float         usdGen:instance:twistRandom  = 25.0
                    float         usdGen:instance:normalOffset = 0.02
                    float         usdGen:instance:width        = 0.02
                    float         usdGen:instance:length       = 0.09
                    float2[]      usdGen:instance:width:knots  = [(0, 1), (1, 0.2)]
                    # "usdGen:featherAge" must be baked by an operator in this chain
                    # (04-operators.md §3, UsdGenExprOp output primvars). A name with no baked
                    # source publishes nothing and raises one TF_WARN (§8.2).
                    token[]       usdGen:instance:variationPrimvars = ["displayColor", "usdGen:featherAge"]
                }
            }
        }
    }
}
```

What `usdGenImaging` synthesizes (S33; verified end to end in `prototypes/instancing/`):

```
/Bird/Groom/featherCards/__usdGenRender/inst_op02_cards            primType = "instancer"
    instancerTopology/prototypes      = [ …/inst_op02_cards/Prototypes/cardA, …/cardB, …/quill ]
    instancerTopology/instanceIndices = [ VtIntArray, VtIntArray, VtIntArray ]   # HdIntArrayVectorSchema
    primvars/hydra:instanceTranslations | Rotations | Scales   (interpolation = instance)
    primvars/displayColor                                      (interpolation = instance)
    primOrigin/scenePath = /Bird/Groom/featherCards
/Bird/Groom/featherCards/__usdGenRender/inst_op02_cards/Prototypes/cardA   primType = "mesh"
    instancedBy/paths          = [ …/inst_op02_cards ]     # exactly one path
    instancedBy/prototypeRoots = [ …/inst_op02_cards/Prototypes/cardA ]
```

Hair on a natively instanced scalp is emitted once per **propagated prototype path**
(`…/UsdNiPrototype/…`) with `instancedBy` authored by hand from `__usdPrimInfo.isNiPrototype` /
`.niPrototypePath` and a **relative** `primOrigin` (S34). Propagated prototype names are hashes
(`ForInstancer%zx` from `TfHash::Combine`, `piPrototypePropagatingSceneIndex.cpp:406-433`):
discover them, never construct them. Per-instance groom variation additionally requires S7's
`InstanceDataSourceNames()`, because a custom container is not part of the native-instance
aggregation key (MEASURED, `research/G-stage-free-parameter-and-time-transport.md` §4).

---

## 5. Contract C3 — the curve contract

One contract, four producers: **guides**, **freezes**, **imports** and **sim caches**. It is
S42 plus `primvars:usdGen:role` (ADR §3). It is frozen at the end of **M2**. It removes four future
migrations: a groom exported from a DCC, a bake written by usdGen, a hand-drawn guide set and an
Alembic-converted sim cache are the same kind of input to a styler.

```usda
def BasisCurves "curves_0000" ( prepend apiSchemas = ["UsdGenCurveAPI"] )
{
    uniform token type  = "cubic"                                  # C2/C3 constant (S29)
    uniform token basis = "bspline"                                # bspline | catmullRom only
    uniform token wrap  = "pinned"                                 # never centripetalCatmullRom / bezier
    int[]         curveVertexCounts = [...]                        # Σ == points.size() exactly (S28)
    point3f[]     points            = [...]
    float[]       primvars:widths            ( interpolation = "vertex" )    # or "constant"; never "varying"
    point3f[]     primvars:rest              ( interpolation = "vertex" )    # free while it shares the points buffer
    int[]         primvars:skinprim          ( interpolation = "uniform" )   # parent-mesh face index per curve
    texCoord2f[]  primvars:skinprimuv        ( interpolation = "uniform" )   # NOT named "st"
    uint64[]      primvars:usdGen:curveId    ( interpolation = "uniform" )   # stable 64-bit id
    string        primvars:usdGen:frozenEpoch ( interpolation = "constant" ) # "usdgen1:sha1:…"
    token         primvars:usdGen:role       ( interpolation = "constant" )  # "hair" | "guide"
    matrix4d[]    primvars:usdGen:rootFrame  ( interpolation = "uniform" )   # optional
    uniform token purpose = "guide"                                          # guide sets only
}
```

Six rules a consumer must obey, each with its measurement:

1. **Exact-size arrays.** `points.size() == Σ curveVertexCounts`, asserted by the publisher and by
   the C3 validator. Padding is broken in 26.08 and renders fallback red (S28).
2. **Test for a value, never for presence.** `velocities`, `accelerations` and `normals` are
   *always* listed by the gprim data source, with size 0, even when unauthored; so are
   declared-but-unauthored primvars such as `primvars:usdGen:rootFrame` (MEASURED,
   `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.1). Code that walks
   `HdPrimvarsSchema::GetPrimvarNames()` must skip empty values.
3. **`skinprimuv` must not be named `st`.** Its `TexCoord2f` value type gives it
   `role = textureCoordinate` automatically, which is harmless, but Storm binds `st` by name through
   the material network and a collision would rebind the scalp texture lookup (MEASURED, §2.1 of the
   same report).
4. **`primvars:widths` wins over `widths`.** The `BasisCurves` schema says so
   (`pxr/usd/usdGeom/curves.h:155-168`) and the adapter falls back
   `primvars:widths` → inherited → `widths`. usdGen always authors `primvars:widths`.
5. **`primvars:rest` is nearly free** when it shares the `points` buffer: **+26 bytes** in a crate
   file, against +9 600 046 bytes for a distinct buffer (MEASURED, `appendix-A-evidence-ledger.md`
   §2.6 row **EV-048**; `research/G-freeze-bake-undo-and-frozen-reentry.md` §3). Authoring it is the
   default.
6. **Namespaced primvars survive intact.** The scene-index name is literally `usdGen:curveId`, and
   an edit dirties `primvars/usdGen:curveId` and nothing else. Editing `curveVertexCounts` dirties
   `basisCurves/topology/curveVertexCounts`, which means the id mapping may have changed and every
   downstream sculpt layer must be re-matched by `curveId` (MEASURED, §2.4 of the same report).

`primvars:usdGen:curveId` is `uint64[]` (ADR §9 R12). A foreign exporter that writes a 32-bit
`int[]` id array still satisfies C3: `UsdGenCurveSource` **widens it on import**, zero-extending each
value, and the widened ids are what every sculpt layer, freeze and decimation predicate then matches
on. usdGen itself always authors `uint64[]`.

`primvars:usdGen:role` is the only marker distinguishing a guide set's curves from a freeze's, and
it is what lets `UsdGenCurveSource` accept both. A curve prim that satisfies C3 but has no
`UsdGenCurveAPI` applied is still accepted (an exported a DCC groom will not have it); the API
schema buys fallbacks, `GetPropertyNames()` visibility and a one-call validity predicate, not
admission.

---

## 6. Dirty classification

This is the table `UsdGenDirtyRouter`'s compile-time `(primType, locatorPrefix) → (node, bits,
surfaceId)` table is generated from (`03-execution-engine.md` §5.1). It is **generated from §2's
dirty-class column** by the same script that emits the router header, not hand-maintained: a
property declared in §2 with no §6 row is a build error, and gate **SI-7** asserts that every
`usdGen:*` property of every registered type has exactly one row. Rows are locator **prefixes**
matched longest-first: a dirty on `usdGen/clump` matches every `usdGen:clump:*` leaf, but
`usdGen/mode` does **not** match `usdGen/length/mode` — a nested leaf needs its own row.
`usdGen:` becomes `usdGen/` in the locator because the adapter hands
`UsdImagingDataSourceMapped` a multi-element locator per property (§0.7; MEASURED,
`research/G-stage-free-parameter-and-time-transport.md` §2).

The **On type** column is load-bearing, because prefixes collide across types: `usdGen/blend` is
value on a `UsdGenOperator` and capture on a `UsdGenGuideSet`; `usdGen/direction` is structural on
`UsdGenGrow` (a `uniform token`), value on `UsdGenDirection` and value on `UsdGenWind` (a
`vector3f`); `usdGen/mode` is structural everywhere it appears but reaches four different kernels.

### 6.1 Graph-structural rows — recompile and bump the Merkle digest

| Locator prefix | On type | Engine action |
|---|---|---|
| `usdGen/input` | any `UsdGenOperator` | recompile the sub-graph from this node down; drop its capture caches |
| `usdGen/terminal`, `usdGen/guides`, `usdGen/surface`, `usdGen/prototypes`, `usdGen/curves`, `usdGen/reference`, `usdGen/frozen/curves`, `usdGen/clump/centers`, `usdGen/length/source`, `usdGen/direction/source`, `usdGen/part/curves`, `usdGen/colliders`, `usdGen/mask/source`, `usdGen/displace/map`, `usdGen/look/colorMap`, `usdGen/expr/maps`, `usdGen/combine/inputs`, `usdGen/paint/surface` | the type that declares it (§2) | a relationship retarget changes a graph edge: recompile, re-link the map/surface dependency, re-`Capture()` |
| `usdGen/schemaVersion`, `usdGen/sessionId` | `UsdGenGroom` | re-admit or refuse the whole groom (§8.4); re-key the session (ADR §4.5) |
| `usdGen/space`, `usdGen/readPhase`, `usdGen/algorithmVersion` | any `UsdGenOperator` | three of the digest's own terms (ADR §4.2.1): recompile |
| `usdGen/mode` | `UsdGenScatter`, `UsdGenDeform`, `UsdGenSmooth`, `UsdGenDirection`, `UsdGenDisplace`, `UsdGenExprOp` | recompile (kernel branch and capture set change) |
| `usdGen/axisMode` | `UsdGenCurl`, `UsdGenBend` | recompile: the frame the coil or the bend is built in (§2.7.2) |
| `usdGen/blendMethod`, `usdGen/cvCount` | `UsdGenGuideInterpolate` | recompile; `cvCount` also re-allocates (topology) |
| `usdGen/direction`, `usdGen/segments` | `UsdGenGrow` | `segments` sets the CV count (topology); `direction` selects the initial frame. The nested leaves `usdGen/direction/source` and `usdGen/direction/knots` belong to `UsdGenDirection` and have their own rows — longest prefix wins |
| `usdGen/useRest`, `usdGen/idSource`, `usdGen/lane`, `usdGen/staleAction` | `UsdGenCurveSource` | which buffer is styled, where ids come from, which lane the set enters, what a stale epoch does |
| `usdGen/clump/method`, `usdGen/clump/level` | `UsdGenClump` | the kernel branch; `clump:level` fixes which `clumpId_<n>` this node publishes (ADR §9 R10) |
| `usdGen/distribution` | `UsdGenResample` | recompile |
| `usdGen/length/method`, `usdGen/rebuild` | `UsdGenLength` | the cut/extend kernel branch and how CVs are redistributed |
| `usdGen/replace` | `UsdGenWidth` | set-widths versus multiply-upstream-widths changes the published primvar set |
| `usdGen/widthToo` | `UsdGenScale` | whether `widths` is scaled too — changes the published primvar set |
| `usdGen/expr/returnType` | `UsdGenExprMap` (`float \| color`), `UsdGenExprOp` (`displacement \| width \| color`) | recompile every operator that references the map, or the operator itself |
| `usdGen/combine/mode` | `UsdGenCombineMap` | recompile every operator that references the map |
| `usdGen/rbfSamples`, `usdGen/resolveType` | `UsdGenDeform`, `UsdGenCollide` | recompile |
| `usdGen/sculpt/space` | `UsdGenSculptLayer` | recompile |
| `usdGen/frozen/mode` | `UsdGenFreeze` | the unfreeze gesture: the chain above the freeze re-enters or leaves the graph |
| `usdGen/curve/basis`, `usdGen/motion/mode`, `usdGen/motion/sampleCount`, `usdGen/motion/forwardSurfaceSamples` | `UsdGenDescription` | recompile; the motion block re-plans the tail |
| `usdGen/primitive`, `usdGen/instance/variationPrimvars` | `UsdGenInstance` | recompile; the instancer's prototype set / primvar set changes |
| `usdGen/mask/combine`, `usdGen/mask/rangeMode` | any operator (`UsdGenMaskAPI`) | recompile the mask expression |
| `usdGen/look/bakeTarget`, `usdGen/look/bakePrimvar`, `usdGen/look/bakeMode`, `usdGen/look/colorMapMode` | `UsdGenDescription` (`UsdGenLookAPI`) | which primvar the bake writes, at what granularity, and whether the colour map is baked or wired to the shader; each changes the published primvar set |
| `basisCurves/topology/*` | a C3 curve prim | the frozen/imported buffer changed shape: recompile and re-match sculpt layers by `curveId` |
| `primvars/usdGen:curveId`, `primvars/usdGen:role` | a C3 curve prim | the id space or the hair/guide role moved |

### 6.2 Structural (topology) rows — re-allocate, never touch the digest

`usdGen:enabled`, `usdGen:length:mode`, `usdGen:cullThreshold`, `usdGen:cvCount`,
`usdGen:resampleTo` and `usdGen:tileTarget` change curve, CV or published-prim counts without
changing any term of `d(n)` (ADR §4.2.1; §0.4). The router raises `UsdGenDirtyTopology`, written
unscoped as §2.1 requires (ADR §9 R3).

| Locator prefix | On type | Engine action |
|---|---|---|
| `usdGen/enabled` | any `UsdGenGenerator`, `UsdGenResample`, `UsdGenLength` (unconditionally — its static `TopologyEffect()` is `MayChangeCurveCount`), `UsdGenFreeze`, `UsdGenInstance` | bump this node's topology generation and every downstream node's; **keep this node's own capture** — its ids, kd-trees and map samples are unchanged by a mute (ADR §9 R14); recapture downstream; re-allocate the downstream buffers and the tile partition (for `UsdGenInstance`, the instancer's instance count); one topology publish; do **not** bump the structural digest and do **not** recompile |
| `usdGen/length/mode`, `usdGen/cullThreshold` | `UsdGenLength` | `cull` (or a non-zero threshold) removes curves at capture; `set \| scale` otherwise re-runs the tail. Routed here, never by the `usdGen/mode` prefix |
| `usdGen/cvCount`, `usdGen/resampleTo` | `UsdGenResample`, `UsdGenCurveSource` | CV count changes: re-capture and re-allocate |
| `usdGen/tileTarget` | `UsdGenDescription` | re-allocate the published prim set (`PrimsAdded`/`PrimsRemoved` on `__usdGenRender/tile_*`); **not** a node-digest term and **no recompile** (ADR §9 R21). A session-level re-allocation performed outside interaction (§3.2) |

### 6.3 Toggle rows

| Locator prefix | On type | Engine action |
|---|---|---|
| `usdGen/enabled` | `UsdGenStyler` other than `UsdGenResample` and `UsdGenLength`; `UsdGenDeformer`; `UsdGenSculptLayer` | the router raises `UsdGenDirtyParameter`: the node is aliased to its input (a memcpy pass-through), the tail re-runs, and capture caches, kd-trees and clump ids are kept (ADR §9 R14) |

### 6.4 Capture rows — bump the capture epoch, re-`Capture()` this node and downstream

| Locator prefix | On type | Engine action |
|---|---|---|
| `usdGen/seed`, `usdGen/clump/seed`, `usdGen/mask/randomSeed`, `usdGen/mask/noise/seed`, `usdGen/noise/seed`, `usdGen/expr/seed`, `usdGen/look/jitterSeed` | the declaring type | re-roll every hash; ids and derived arrays are rebuilt |
| `usdGen/density`, `usdGen/spacingU`, `usdGen/spacingV`, `usdGen/jitter`, `usdGen/relaxIterations`, `usdGen/areaCompensation`, `usdGen/rootPrims`, `usdGen/rootUVs`, `usdGen/flip`, `usdGen/perGuide` | `UsdGenScatter` | roots are re-emitted; ids preserved where the face survives (§2.3.1). `usdGen/density` here is the generator's hairs-per-square-unit and **is** capture; the two density **scales** are value and live in §6.5 (ADR §9 R13) |
| `usdGen/length`, `usdGen/lengthRandom`, `usdGen/directionPrimvar` | `UsdGenGrow` | re-emit strand lengths (they may be map-driven); re-sample the named surface primvar per root |
| `usdGen/length/value`, `usdGen/length/random`, `usdGen/minRemainingLength` | `UsdGenLength` | re-derive the per-curve target lengths and the surviving id set |
| `usdGen/expectEpoch`, `usdGen/rebind` | `UsdGenCurveSource` | re-check the epoch; recompute `skinprim`/`skinprimuv` when `rebind` asks for it |
| `usdGen/maxGuides`, `usdGen/influenceRadius`, `usdGen/influenceDecay`, `usdGen/maxGuideAngle`, `usdGen/randomizeGuide`, `usdGen/useUniqueGuide`, `usdGen/clumpCrossover` | `UsdGenGuideInterpolate` | rebuild the kd-tree query result; re-emit `guideIndex`/`guideWeight` |
| `usdGen/searchRadius`, `usdGen/numNeighbors` | `UsdGenSmooth` | reserved for the v3 `neighbours` mode; in v1 the row exists so the property has exactly one class |
| `usdGen/restoreSegmentLengths` | `UsdGenResample` | store (or drop) the source segment lengths at capture |
| `usdGen/scaleRandom` | `UsdGenScale` | re-roll the per-curve length multiplier |
| `usdGen/part/radius` | `UsdGenPart` | rebuild the parting kd-tree query and the per-curve side ids |
| `usdGen/blend` | `UsdGenGuideSet` | the per-guide range-of-influence array feeds the capture-time guide weights |
| `usdGen/clump/size`, `usdGen/clump/density`, `usdGen/clump/levels`, `usdGen/clump/sizeReduction`, `usdGen/clump/goalFeedback`, `usdGen/clump/crossover`, `usdGen/clump/cut`, `usdGen/clump/copyVariance` | `UsdGenClump` | re-derive clump centres, `clumpId_<n>`, the neighbour ids and the per-curve cut/copy coins |
| `usdGen/clump/stray/rate` | `UsdGenClump` | re-roll the stray coin per curve |
| `usdGen/instance/weights`, `usdGen/instance/scaleRandom`, `usdGen/instance/twistRandom` | `UsdGenInstance` | per-instance hashed choices are drawn once at capture, never per frame |
| `usdGen/mask/random`, `usdGen/mask/noise/*`, `usdGen/mask/region` | any operator (`UsdGenMaskAPI`) | re-resolve the mask array and the 257-entry ramp LUT; a `mask/region` **retarget** additionally re-links that map's dependency edge |
| `usdGen/map/*` (including `usdGen/map/default` and `usdGen/map/filter`), `usdGen/expr/source`, `usdGen/paint/primvar`, `usdGen/paint/interpolation`, `usdGen/paint/storage`, `usdGen/paint/resolution`, `usdGen/paint/bakedFile`, `usdGen/noise/*` (including `usdGen/noise/space`), `usdGen/proximity/*` | any `UsdGenMap` | re-sample the map; re-`Capture()` every operator that references it |
| `usdGen/rest/*` (`UsdGenRestAPI`) | a bound surface | rest points changed: re-scatter, rebuild root frames and kd-trees. This row can only fire because `UsdGenRestAPIAdapter::InvalidateImagingSubprim` maps the mesh's authoring properties onto the container (§2.15) |
| `usdGen/look/*` except `bakeTarget`, `bakePrimvar`, `bakeMode`, `colorMapMode`, `colorMap` — including `usdGen/look/rampExponent` and the `colorRamp` arrays | `UsdGenDescription` (`UsdGenLookAPI`) | re-bake `displayColor` |
| `usdGen/sculpt/curveIds`, `usdGen/sculpt/cvOffsets`, `usdGen/sculpt/epoch`, `usdGen/sculpt/lockedCurves`, `usdGen/sculpt/rootPrims`, `usdGen/sculpt/rootUVs` | `UsdGenSculptLayer` | re-match deltas to ids; `rootPrims`/`rootUVs` are what "Rebase sculpt" re-matches on |
| `usdGen/frozen/epoch` | `UsdGenFreeze` | re-check staleness only |
| `primvars/usdGen:frozenEpoch` | a C3 curve prim | re-check staleness |
| `mesh/topology/*` | a bound surface | **recapture trigger**: face count or indices changed — re-scatter every dependent generator; ids on surviving faces are preserved |
| `indices` (the **bare** leaf; the prefixed `geomSubset/indices` is also accepted) | a `geomSubset` prim targeted by a `usdGen:surface` | **recapture trigger**: §2.20 rule 5 — the face set moved; roots on removed faces disappear, surviving ids are kept. The adapter emits the bare locator (`geomSubsetAdapter.cpp:165-185`), not the container-prefixed one |
| `type` (the **bare** leaf; `geomSubset/type` also accepted) | the same prim | **recapture trigger**: a value other than `HdGeomSubsetSchemaTokens->typeFaceSet` is a hard diagnostic (§2.20 rule 1) and the description publishes nothing |

`familyName` has no row because it never reaches Hydra (§2.20 rule 5).

### 6.5 Value, publication and cosmetic rows

| Locator prefix | On type | Class | Engine action |
|---|---|---|---|
| every remaining leaf under `usdGen/` whose §2 row says *value* — the per-frame sliders: `usdGen/blend` on an operator, `usdGen/width/default`, `usdGen/lift`, `usdGen/directionVector`, `usdGen/uvBlend`, `usdGen/lockRoots`, `usdGen/clump/{amount,profile/*,volumize,tightnessReduction,flatness,offset,copy,curl/*,noise/*,stray/{amount,falloff}}`, `usdGen/noise/*` on a styler, `usdGen/cumulative`, `usdGen/preserveLength`, `usdGen/{width,width/knots,taper,taperStart,rootScale,tipScale}`, `usdGen/{direction,direction/knots,amount,tiltU,tiltV,tiltN,aroundN,followSkinContour}` on `UsdGenDirection`, `usdGen/{strength,iterations,lockRoot}`, `usdGen/scale`, `usdGen/scale/knots`, `usdGen/part/strength`, `usdGen/mask/{amount,invert,range,ramp/*,rangeMin,rangeMax,effectPosition,falloff,influenceWidth}`, `usdGen/sculpt/{weight,deltas}`, `usdGen/instance/{orient,scale,twist,normalOffset,width*,length}`, `usdGen/{twistAware,preserveShape*,offset,pushAmount,constStrength,gustStrength,stiffness*}` | the declaring type | value | bump one parameter's value version; re-`Evaluate()` this node's chunks and the tail |
| `usdGen/densityScale`, `usdGen/renderDensityScale` | `UsdGenGroom`, `UsdGenDescription` | value | re-run the salted stable-id decimation predicate over the **already captured** full id set (§2.3.1) and move `UsdGenChunkDesc::liveCount`. **Never re-scatters and never re-chunks**: the id space and the chunk partition are computed once per capture and are untouched (ADR §9 R13; `03-execution-engine.md` §1.2, §3.6). The interactive ceiling is **not** here — it is session-only, through `UsdGenImaging_SetInteractiveLOD` (ADR §9 R8, R31) |
| `primvars/points` | a bound surface | value | the deformed-space tail re-runs; capture untouched — **only when the change is a time sample**. A change to the prim's `Default()` opinion is the *rest* pose and arrives instead as `usdGen/rest/points` from the RestAPI adapter, which is capture (§6.4, §2.15) |
| `primvars/points`, `primvars/rest`, `primvars/widths` | a C3 curve prim | value | re-run the deform tail of the dependents |
| `usdGen/pickTarget` | `UsdGenDescription` | **publication** | re-emit `primOrigin` on every tile of this description; no capture, no evaluate, no engine work |
| `usdGen/label`, `usdGen/frozen/tier` | any / `UsdGenFreeze` | *cosmetic* | nothing; `frozen:tier` is advisory and the evaluator never reads it (ADR §9 R18) |

A leaf under `usdGen/` that matches **no** row is a diagnostic, never a silent value: the generator
that builds this table from §2 fails the build, and at runtime an unknown leaf raises one
`TF_WARN` naming the property and is ignored. Silence here is how a property quietly stops working.

### 6.6 Two rules that are not rows

**First**, `Bind()` pulls **every** mapped locator of a node, regardless of the node's current mode,
once per topology generation. Time-varying and asset-path dependency registration is lazy — it
happens in the data source constructor, so a consumer that never pulls a value never gets its
dirties (MEASURED, `research/G-stage-free-parameter-and-time-transport.md` key facts). Without this
rule, editing `usdGen:clump:noise:frequency` while `noise:amount == 0` would not invalidate. This
closes S14 and is asserted by gate SI-7.

**Second**, a dirty on the *bare* `usdGen` container (an adapter resync) is treated as
graph-structural for that prim, because the adapter emits it when a property appears or disappears.

---

## 7. Schema generation and registration

### 7.1 Source of truth and the `usdGenSchema` target

The domain is authored by hand in `libs/usdGenSchema/schema.usda` and generated into the
**checked-in** `plugin/usdGenSchema/resources/{generatedSchema.usda, plugInfo.json}` by
`bin/gen_schema.sh` — usdRig's script, copied (`bin/gen_schema.sh:1-28`,
`research/B-usdrig-build.md` §7). Regenerate only after editing `schema.usda`, and review the diff.

```usda
#usda 1.0
(
    """usdGen schema domain (02-schema.md §1-§2)."""
    subLayers = [
        @usd/schema.usda@,
        @usdGeom/schema.usda@
    ]
)

over "GLOBAL" (
    customData = {
        string libraryName   = "usdGenSchema"
        string libraryPath   = "usdGenSchema"
        string libraryPrefix = "UsdGen"
        bool   skipCodeGeneration = true
    }
) { }

# Containers. Without the two subLayers above, </Imageable> and </Boundable> do not resolve and
# usdGenSchema fails (usdRig's own file opens the same way: `libs/rigExecSchema/schema.usda:1-16`,
# with `inherits = </Imageable>` at `:96` and `inherits = </Boundable>` at `:198`).
class UsdGenGroom "UsdGenGroom" (
    inherits = </Imageable>
    customData = { string className = "Groom" }
) { ... }
class UsdGenDescription "UsdGenDescription" (
    inherits = </Boundable>
    customData = { string className = "Description" }
) { ... }
class UsdGenGuideSet "UsdGenGuideSet" (
    inherits = </Imageable>
    customData = { string className = "GuideSet" }
) { ... }

class "UsdGenOperator" ( inherits = </Typed> ) { ... }               # abstract: quoted name only
class UsdGenScatter "UsdGenScatter" ( inherits = </UsdGenOperator> ) { ... }   # concrete
class "UsdGenMaskAPI" (
    inherits = </APISchemaBase>
    customData = {
        token apiSchemaType = "singleApply"
        token[] apiSchemaAutoApplyTo = ["UsdGenOperator"]
    }
) { ... }
```

`bin/gen_schema.sh` runs `$USD/bin/usdGenSchema schema.usda ../../plugin/usdGenSchema/resources`
and then performs **three** substitutions on the result, because a codeless domain has no library
for the build placeholders (`bin/gen_schema.sh:23-25`): `"LibraryPath":
"@PLUG_INFO_LIBRARY_PATH@", ` is deleted, `"@PLUG_INFO_RESOURCE_PATH@"` becomes `"."`, and
`"@PLUG_INFO_ROOT@"` becomes `"."`. The checked-in plugin is then a pure `"Type": "resource"` plugin —
exactly the shape proven to register with zero C++
(`prototypes/stage-free-transport/plugin/usdGenProbeSchema/resources/plugInfo.json`, MEASURED).

The schema **classes** stay codeless (`skipCodeGeneration = true`, §0.1) — no generated C++ schema
class exists anywhere. But a `"resource"` plugin is never dlopened, so it cannot register the
compute-extent function `UsdGenDescription : UsdGeomBoundable` needs. ADR §9 R19 rules the shape:
`usdGenSchema` is a **small SHARED CMake target** producing `libusdGenSchema.so`, containing only

* the schema token declarations, and
* one `TF_REGISTRY_FUNCTION(UsdGeomBoundable)` calling `UsdGeomRegisterComputeExtentFunction`
  (§7.2),

and linking the pxr set `tf usd usdGeom` plus the **static** archive `usdGenMath`, which carries the
extent-kernel body and from which the linker therefore pulls exactly one object — no `hd`, no
`usdImaging`, no `usdGen`, no SeExpr, no TBB. `10-build-dependencies-testing.md` §1.2 owns the
target table and is the authority on that dependency list; this document prints it only so §7.2's
registration is readable. That keeps extents working in `usdcat` and `usdrecord` while keeping the
`usdGen` core free of `usd` (ADR §9 R37's link rule, gate B-1). The target is always written as a
CMake target, never resolved from `PATH` — OpenUSD's schema *generator* is also called
`usdGenSchema` (`$USD/bin/usdGenSchema`), which `bin/gen_schema.sh` invokes by absolute path
(`10-build-dependencies-testing.md` §4.2's naming hazard).

```cmake
add_library(usdGenSchema SHARED libs/usdGenSchema/usdGenSchemaTokens.cpp
                                libs/usdGenSchema/computeExtent.cpp)
# pxr: tf usd usdGeom. usdGenMath is a STATIC archive, so only the extent-kernel object is pulled
# in (no SeExpr, no TBB). Nothing from hd/usdImaging/usdGen: gate B-1 checks DT_NEEDED.
# 10-build-dependencies-testing.md §1.2 owns this row.
target_link_libraries(usdGenSchema PRIVATE tf usd usdGeom usdGenMath)
```

The two data files are produced at *configure* time by `file(READ)` / `string(REPLACE)` /
`file(GENERATE)` / `configure_file` into `<build>/usd/usdGenSchema/resources/` and installed by an
explicit `install(FILES …)` rule to `lib/usd/usdGenSchema/resources`. The pattern is usdRig's,
verified correct at `usdRig/CMakeLists.txt:425-474` (`research/B-usdrig-build.md` §7); only the
named library differs — usdRig points at `rigExecImaging`, usdGen at its own `usdGenSchema`. The
checked-in copy is `"Type": "resource"`; only the generated copy is rewritten to
`"Type": "library"`, so that `Plug` will dlopen `libusdGenSchema.so`:

```cmake
# Read the checked-in plugInfo; re-run configure when it changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/plugInfo.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/generatedSchema.usda")
file(READ  "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/plugInfo.json" _pi)

# 1. Plug treats a "resource" plugin as already loaded and never dlopens it, so a domain that
#    declares implementsComputeExtent must be a "library" plugin naming a real library.
string(REPLACE "\"Type\": \"resource\"" "\"Type\": \"library\"" _pi "${_pi}")
string(REPLACE "\"schemaIdentifier\": \"UsdGenDescription\""
       "\"implementsComputeExtent\": true, \"schemaIdentifier\": \"UsdGenDescription\"" _pi "${_pi}")
string(REPLACE "\"Name\": \"usdGenSchema\""
       "\"LibraryPath\": \"../../../$<TARGET_FILE_NAME:usdGenSchema>\", \"Name\": \"usdGenSchema\"" _pi "${_pi}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenSchema/resources/plugInfo.json"
     CONTENT "${_pi}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenSchema/resources/generatedSchema.usda"
               "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenSchema/resources/generatedSchema.usda" COPYONLY)

# Without this nothing is ever installed, and the install-tree hop below has nothing to resolve.
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenSchema/resources/plugInfo.json"
        "${CMAKE_CURRENT_BINARY_DIR}/usd/usdGenSchema/resources/generatedSchema.usda"
    DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenSchema/resources")
```

`../../../$<TARGET_FILE_NAME:usdGenSchema>` is the one relative hop that resolves in both the build
tree (`<build>/usd/usdGenSchema/resources/` → `<build>/`) and the install tree
(`lib/usd/usdGenSchema/resources/` → `lib/`), which is why the file is generated rather than
installed verbatim (S44). Only the generated copy advertises `implementsComputeExtent`: promising
it from the data-only source-tree resources would give a host `UsdGenDescription` prims with
silently empty bounds.

### 7.2 The compute-extent registration

`libusdGenSchema.so` — not `usdGenImaging` — carries one `TF_REGISTRY_FUNCTION(UsdGeomBoundable)`:

```cpp
TF_REGISTRY_FUNCTION(UsdGeomBoundable)
{
    UsdGeomRegisterComputeExtentFunction(
        TfType::FindByName("UsdGenDescription"), &UsdGenSchema_ComputeDescriptionExtent);
}
```

The function computes the description's bound from the authored chain's rest surface and length
bounds; its body is a kernel in the static `usdGenMath` (`10-build-dependencies-testing.md` §1.2),
and it never needs Hydra, which is why it can live in a library that links only `tf usd usdGeom`
plus that archive.

`UsdGeomRegisterComputeExtentFunction` rejects a type that does not derive from `UsdGeomBoundable`
(`pxr/usd/usdGeom/boundableComputeExtent.cpp:271-289`), and `_LoadPluginForType` returns early
unless the plugin metadata carries `implementsComputeExtent` (`:155-173`) — the two facts §7.1's
CMake exists to satisfy.

### 7.3 Adapter registrations

`usdGenImaging`'s generated `plugInfo.json` registers **five prim adapters and one API-schema
adapter** (ADR §2.3 — this is the mandatory correction all three judges made; without it the
container half of the schema is invisible to Hydra and emits no notices, MEASURED S10):

```json
"Types": {
  "UsdGenOperatorAdapter":    { "bases": ["UsdImagingSceneIndexPrimAdapter"],
                                "primTypeName": "UsdGenOperator",
                                "includeDerivedPrimTypes": true },
  "UsdGenMapAdapter":         { "bases": ["UsdImagingSceneIndexPrimAdapter"],
                                "primTypeName": "UsdGenMap",
                                "includeDerivedPrimTypes": true },
  "UsdGenGroomAdapter":       { "bases": ["UsdImagingSceneIndexPrimAdapter"],
                                "primTypeName": "UsdGenGroom" },
  "UsdGenDescriptionAdapter": { "bases": ["UsdImagingSceneIndexPrimAdapter"],
                                "primTypeName": "UsdGenDescription" },
  "UsdGenGuideSetAdapter":    { "bases": ["UsdImagingSceneIndexPrimAdapter"],
                                "primTypeName": "UsdGenGuideSet" },
  "UsdGenRestAPIAdapter":     { "bases": ["UsdImagingAPISchemaAdapter"],
                                "apiSchemaName": "UsdGenRestAPI" }
}
```

`UsdImagingAdapterRegistry` keys one `primTypeName` per registered `TfType`
(`pxr/usdImaging/usdImaging/adapterRegistry.cpp:104-131`), so five entries mean five `TfType`s.
They are five one-line subclasses of a single implementation class `UsdGenPrimAdapterBase`:
all behaviour — building `UsdImagingDataSourceMapped::PropertyMappings` generically from
`UsdPrimDefinition::GetPropertyNames()`, the `usdGen` container, `Invalidate` — lives in the base,
so "one adapter class, five registrations" holds in every sense that matters. The seven class names
are ADR §9 R3's spellings and no document may use a synonym. `Groom`, `Description` and `GuideSet`
need their own entries because they derive from `UsdGeomImageable`, not from `UsdGenOperator`, so
`includeDerivedPrimTypes` on the operator base does not reach them. The plugInfo keys
(`primTypeName`, `includeDerivedPrimTypes`, `apiSchemaName`, and the two `bases` values) are
verified against `usdImaging/adapterRegistry.cpp:104-131, 297-323` and
`usdImaging/plugInfo.json:59-63`.
`06-imaging.md` §2 owns the implementation.

### 7.4 Auto-applying `UsdGenMaskAPI`

Two routes into the **same** generated `plugInfo.json` exist, and usdGen authors both:

* the per-type entry `Types/UsdGenMaskAPI/apiSchemaAutoApplyTo`, which `usdGenSchema` emits from the
  class's `customData = { token[] apiSchemaAutoApplyTo = ["UsdGenOperator"] }`
  (`pxr/usd/usd/usdGenSchema.py:1517-1524`, `_UpdatePlugInfoWithAPISchemaApplyInfo`);
* the plugin-level `Info/AutoApplyAPISchemas` block, which `bin/gen_schema.sh`'s post-pass injects
  into the checked-in `plugInfo.json` so it survives regeneration:

```json
"Info": {
  "AutoApplyAPISchemas": {
    "UsdGenMaskAPI": { "apiSchemaAutoApplyTo": ["UsdGenOperator"] }
  }
}
```

`UsdSchemaRegistry` reads both (`pxr/usd/usd/schemaRegistry.cpp:245-261` for the per-type route,
`:836-914` for the plugin route, with the token names at `:71` and `:84`) and **appends** the two
lists rather than letting one win (`:903-911`), so authoring both is redundant but harmless. usdGen
authors both because the plugin-level block is the one that survives a `gen_schema.sh` regeneration
if the `customData` is ever lost. The registry also **propagates auto-apply to derived types**: it takes
each named schema's `TfType` and calls `GetAllDerivedTypes` (`:922-932`). Whether that works on a
**codeless** domain, where the `TfType`s are declared from plugInfo rather than from a library, is
UNMEASURED and is exactly what gate **SI-8** tests (one afternoon in M0 pre-work, ADR §7). Until
SI-8 passes, the tool also authors `prepend apiSchemas = ["UsdGenMaskAPI"]` explicitly on every
operator it creates; the examples in §4 assume SI-8 has passed.

### 7.5 Gates

* **SI-7 — adapter coverage.** For every registered type, every `usdGen:*` property in
  `UsdPrimDefinition::GetPropertyNames()` appears in the terminal scene index under the expected
  nested locator, and editing it emits exactly that locator and nothing else. Runs over the real
  `UsdImagingCreateSceneIndices` chain, no GL (tier T1). Also asserts the `Bind()`-pulls-everything
  rule of §6 by editing a property that the node's current mode does not read.
* **SI-8 — auto-apply on a codeless type.** `UsdGenScatter` with no authored `apiSchemas` reports
  `prim.HasAPI("UsdGenMaskAPI") == true` and `usdGen:mask:ramp:knots` in `GetPropertyNames()`, and
  the property reaches the scene index. **Tier T1**, M0 pre-work, binding at M1
  (`09-performance-and-benchmarks.md` §5.2, the gate registry, ADR §9 R40): the gate needs the real
  `UsdImagingCreateSceneIndices` chain to prove the property reaches Hydra. The schema-registry half
  of the assertion — `prim.HasAPI` and `GetPropertyNames()` — can also be checked in a T0 Python
  script, but the gate itself is T1.

Both are milestone-**M1** exits and both gate contract C1.

---

## 8. Compatibility and evolution

### 8.1 What C1 freezes, and what stays free

C1 freezes the `usdGen:` **property names** and the **prim type names** at the end of M1 (ADR §3).
It does not freeze the engine: `UsdGenOp`, chunk views, the graph headers and the imaging internals
are deliberately **not installed** and may churn until M7. C2 (the published tile contract), C3
(§5) and C5 (the glslfx `inputs:` names) are the other artist-visible surfaces and freeze at M1,
M2 and M1 respectively. Everything in §2 that belongs to a v2/v3 type is declared in `schema.usda`
from M1 so C1 covers the whole vocabulary in one freeze.

### 8.2 Adding an operator or a property without breaking C1

Adding an **operator type** touches two files: a `class UsdGenX "UsdGenX" ( inherits =
</UsdGenStyler> )` block in `schema.usda`, and a kernel in `usdGenMath` plus its registration in
the internal `UsdGenOpRegistry`. There is no imaging code (`includeDerivedPrimTypes`), no UI code
(panels are generated from `UsdPrimDefinition::GetPropertyNames()`), and no adapter entry. A type
declared but not yet implemented publishes nothing and raises one `TF_WARN` naming the prim.

Adding a **property** inside a major version is legal iff its schema fallback preserves the
existing behaviour exactly. Old assets do not author it, get the fallback, and are unchanged; new
assets opt in. The property appears automatically in the adapter mappings, the dirty router table
(§6, which is generated from the same property list plus a per-prefix class annotation) and the
parameter panel. A property whose fallback would change an existing look is not an addition — it is
a §8.3 behaviour change.

### 8.3 Behaviour changes and `usdGen:algorithmVersion`

An operator type never changes the meaning of an existing property. A kernel whose numerical
behaviour changes bumps that type's current `usdGen:algorithmVersion`; assets authored earlier carry
the older integer and evaluate through the older code path.

The schema fallback is **`0` = "track the newest kernel"** (ADR §9 R17). One fallback, declared once
on `UsdGenOperator`; it is never re-declared per concrete class. Look preservation is guaranteed for
tool-authored assets by a single rule instead: **the tool, every freeze and every bake author the
explicit current integer** on every operator prim they create (`08-tools.md`). A hand-written asset
that omits the property follows the newest kernel — which is the documented, visible behaviour, and
the parameter panel says so on the row. ASSUMPTION (inherited
from `design/proposal-risk.md` §3.9, which marks the whole mechanism one): keeping one branch per
live version is affordable. `12-risks-decisions-open-questions.md` carries the retirement policy;
the release notes state when a version is retired.

Where the change is a genuinely new mode rather than a fix, the mechanism is a new token value
whose *default* is the old behaviour — `usdGen:clump:method = "linearBlend"` today,
`"extrudeAndBlend"` opt-in (both values declared in §2.7, from
`research/A7-prior-art-grooming.md` §9.2 S1) — which is what a shot that must not shift requires.
`usdGen:algorithmVersion` is in the structural digest (ADR §4.2.1), so bumping one on an existing
prim is a recompile, which is correct: it is a different operator.

### 8.4 Refuse-and-warn on a newer `usdGen:schemaVersion`

A binary that reads `usdGen:schemaVersion` greater than the version it knows emits **one**
`TF_WARN` per groom naming both versions, publishes **nothing** for every description under that
groom, and lets the rest of the stage render. It never evaluates a groom it may misinterpret: a
silently partial groom is worse than an absent one, because it looks plausible in a turntable.
Lower versions always evaluate — that is what §8.2's fallback rule buys.

### 8.5 Deprecation

A property is never removed in place. It keeps its schema entry, gains `hidden = true` for at least
one release (so it disappears from the generated panel but still composes), and the evaluator
ignores it while the tool offers a one-click migration to whatever replaced it. Only the next major
`usdGen:schemaVersion` deletes the entry. The same discipline applies to token values: a retired
value is accepted with one `TF_WARN` mapping it to its replacement for one release.

The frozen-curve contract carries its own version in the `frozenEpoch` prefix,
`usdgen<major>:sha1:<40 hex>`. A reader that does not recognise the prefix treats the freeze as
**stale** — one `TF_WARN`, a badge, and it **keeps rendering the frozen data**; it never silently
re-cooks the chain the artist froze (ADR §9 R18). This is what stops a freeze written by a newer
plugin from being silently mis-deformed.

### 8.6 Migration tooling

`usdgen.migrate(stage, toVersion)` — pure Python in the `usdgen` package (ADR §9 R5;
`08-tools.md` §1.1 owns the module layout), no C++ — walks every
`UsdGenGroom`, applies the recorded per-version rewrites inside one `Sdf.ChangeBlock` and bumps
`usdGen:schemaVersion`. It is idempotent, it is the only sanctioned way to raise a groom's version,
and its fixture is example (a) authored at every shipped version.

---

## 9. Testing

Tiers are ADR §7's **T0–T4**, which extend S45's four harness tiers — S45 (1)–(4) become T1–T4 —
with **T0**, the tier the no-`usd`/no-`hd` link contract of ADR §4.2.3 makes possible: **T0**
headless C++/Python with neither Hydra nor a stage; **T1** headless scene-index tests over the real
`UsdImagingCreateSceneIndices` chain (sub-100 ms, no GL); **T2** Storm correctness/timing headlessly
through the EGL device-platform context (`prototypes/storm-hair-look/eglctx.h`); **T3**
`testusdview` scripts; **T4** workstation protocols (release criteria, never milestone exits).

Gate ids below are `09-performance-and-benchmarks.md` §5's, the single gate registry (ADR §9 R40);
the **Contract** column names a contract this test freezes (C1–C5 are contracts, ADR §3, not gates).

| # | What it proves | Tier | Gate | Contract | Milestone |
|---|---|---|---|---|---|
| 1 | `schema.usda` regenerates byte-identically; the checked-in resources match `usdGenSchema` output | T0 | — | — | M0 |
| 2 | Every type in §1 registers: `IsA`, `GetSchemaTypeName`, derived-type queries, fallbacks, with zero C++ | T0 | — | — | M0 |
| 3 | Every property in §2 — §2.7.1 and §2.7.2 included — exists with the stated type, default, `uniform`-ness and per-declaring-type `allowedTokens`, asserted from `UsdPrimDefinition::GetAttributeDefinition`; and every parameter `04-operators.md` §2–§5 and `07-look-maps-expressions.md` §5.1 names matches this document's row, in both directions (no sibling row left unfolded, no 02 row a sibling contradicts). The executable form of C1, and the test that catches a disagreement between §2 here and `04-operators.md` or `07-look-maps-expressions.md` (ADR §9 R7, R8) | T0 | — | freezes **C1** | M1 |
| 4 | `UsdGenMaskAPI` auto-applies to every `UsdGenOperator` subtype on this codeless domain, and the auto-applied properties reach the terminal scene index | T1 | **SI-8** | — | M0 pre-work / M1 |
| 5 | Every `usdGen:*` property of every registered type reaches the terminal scene index under its nested locator and dirties exactly that locator; the `Bind()`-pulls-every-mapped-locator rule holds for mode-gated parameters; §0.7's relationship list is complete | T1 | **SI-7** | — | M1 |
| 6 | `UsdGenRestAPI` publishes `usdGen/rest/points` at `Default()` while the stage time is numeric, with no time-varying flag; **and** editing the mesh's `Default()` `points` opinion arrives as `usdGen/rest/points` (capture) while a time-sample edit arrives as `primvars/points` (value) — the §2.15 invalidation mapping | T1 | — | — | M2 |
| 7 | Ramp knots and the whole-`TsSpline` transport produce **zero** dirties across 100 `SetTime` calls: a `.spline` on `usdGen:mask:ramp:knots` is rejected at compile naming the property, while `usdGen:mask:ramp:spline` transports intact with zero dirties; the four ADR §9 R11 interpolation tokens each build their LUT (§2.17) | T1 | — | — | M1 |
| 8 | The three §4 files parse under `Sdf.Layer.FindOrOpen`, compose, and compile to a graph whose Kahn order matches the authored chain; a cycle and a two-target `usdGen:terminal` are diagnosed by path. The fixture files **are** the §4 blocks, with the `[...]` array elisions filled in and nothing else changed — not paraphrases; a §4 edit that does not parse fails this test | T0+T1 | — | — | M1 |
| 9 | C3 round trip: a freeze authored by the tool re-enters through `UsdGenCurveSource` with identical points, `uint64` ids and epoch; a 32-bit `int[]` id array widens correctly on import; `velocities`/`normals`/unauthored primvars are skipped by value, not by presence | T1 | — | freezes **C3** | M2 |
| 10 | Every row of §6 routes to the class it claims, and every §2 property has exactly one row: a `label` edit costs nothing, a `pickTarget` edit re-emits `primOrigin` and evaluates nothing, a `clump:amount` edit dirties one node's tiles, a `seed` edit re-captures without recompiling, a `densityScale`/`renderDensityScale` edit is **value** — it moves `UsdGenChunkDesc::liveCount` and neither re-scatters nor re-chunks (ADR §9 R13, §6.5), a styler `enabled` toggle keeps its capture caches, a **generator** `enabled` toggle re-allocates while keeping *that node's own* capture and without changing the structural digest, a `length:mode` edit re-captures rather than re-evaluating, and a `tileTarget` edit re-allocates the tile set without a recompile | T1 | SI-3, SI-7 | — | M1/M3 |
| 11 | §2.13's resolved-weight block is executable and `04-operators.md` §5.2's pseudocode is arithmetically identical to it, term for term: the `max(y - x, 1e-6)` guard, `lockedCurveSuppression(c)` inside the clamp, the `usdGen:blend` factor, `region(c)`, and `UsdGenDraw01` (ADR §9 R16). Driven from one shared fixture of parameter values, comparing the two transcriptions numerically | T0 | — | freezes **C1** | M1 |
| 12 | The two density scales decimate by salted stable id and are **exclusive by context** (`densityScale` ignored under `USDGEN_CONTEXT=render`, `renderDensityScale` ignored otherwise); ids, sculpt deltas and clump ids are identical before and after; the product clamps at 1 with one warning; the surviving set's `hairId` stays uniform (`kSaltDensity ≠ 0`, §2.19.1). The interactive ceiling is driven **through the C ABI**, `UsdGenImaging_SetInteractiveLOD(descPath, maxCurves)`, never through an authored property | T1 | — | — | M3 |
| 13 | `GeomSubset` rules: parent-mesh face indices, union of subsets, a ptex id = `GetFaceId(parentFace) + quadrant`, an `indices` edit arriving as the **bare** locator `indices` (not `geomSubset/indices`) on the subset's own Hydra prim and routed as a recapture with surviving ids preserved, a `type` other than `typeFaceSet` diagnosed, and `familyName` proven inert (it reaches no locator) | T1 | — | — | M3 |
| 14 | A `usdGen:schemaVersion` one greater than the binary's publishes nothing and warns once; the rest of the stage still renders. A stale `usdGen:frozen:epoch` warns once and **keeps rendering the frozen data** | T1 | — | — | M1 |
| 15 | `UsdGenDescription`'s compute-extent function is found through the generated plugInfo — which names `libusdGenSchema.so` — in both the build tree and the install tree ("select and press F" frames the hair) | T1+T3 | — | — | M2 |
| 16 | An authored prim under `__usdGenRender` is ignored with one warning and does not reach the renderer; an authored prototype under `<Description>/Prototypes` is explicitly pruned and is **not** drawn standalone | T1 | — | — | M1 |

M1's stop condition applies here directly: if precise `usdGen:*` invalidation cannot be demonstrated
(test 5 / gate SI-7), the fallback is `primvars:usdGen:*` for operator parameters and the plan is
re-cut (ADR §7).

---

## 10. Out of scope

* **Operator kernels and the capture/evaluate split.** §2.6–§2.8 are the normative property
  registry (ADR §9 R7); `04-operators.md` §2 specifies what each kernel does with them.
* **The published tile contract C2** (`hairT`, `hairId`, `st`, `extent`, `displayStyle`,
  `primOrigin`, `__dependencies`) — `06-imaging.md`.
* **The `Material` prim and its three terminals**, the glslfx `inputs:` block (C5) and the bake
  arithmetic — `07-look-maps-expressions.md`.
* **The C ABI and the `pxr_boost` array surface** (C4) — `08-tools.md`.
* **A third-party operator ABI.** `UsdGenOpRegistry` is internal in v1 and v2; an out-of-tree
  operator type is v3 (ADR §3). Until then a TD extends usdGen through `UsdGenExprMap` and, in v2,
  `UsdGenExprOp` (capture-time only).
* **`UsdGenPart`**'s kernel (the parting-line operator) and its brush: v2, shipping in M8. Its prim
  type and its three properties **are** declared here (§1, §2.7.2) from M1, so C1 freezes the name
  once; v1 ships parting as a region map consumed by `GuideInterpolate` and `Clump` through
  `usdGen:mask:region` (§2.13).
* **Multiple-apply API schemas.** Every usdGen API schema is single-apply; nothing in the design
  needs instanced properties, and multiple-apply would break the generic
  `GetPropertyNames()` → mappings path.
* **`UsdGenPrimitive` as a prim type.** Rejected: the terminal operator's type decides what a
  description draws (§2.3).
* **Authoring anything from the evaluator.** No usdGen code path writes to a stage except the tool,
  explicitly, inside an undo bracket (brief requirement R1, S41).

---

## 11. Sources

| Source | Sections used |
|---|---|
| `design/adr-v1.md` | §1 (S11 ramps, S23/S27 chunk≠tile, S29 `hairId`), §2.1 type hierarchy, §2.2 reserved layout, §2.3 contested properties, §3 contracts C1–C5, §4.1–4.5 engine/session, §5.3 tile contract, §7 milestones and gates, §8 document map, and **§9 rulings R1–R3, R5, R6–R19, R21, R24, R25, R29–R31, R35–R38, R40, R42, R43, R45** (binding; where §1–§8 and §9 disagree, §9 wins). The rulings this document is the canonical owner under: **R7/R8** — 02 is the single normative property registry (names, types, defaults, allowed tokens, dirty class) and gains what a sibling needs, never the reverse |
| `design/brief-v1.md` | R1–R9; S4, S5, S9–S16, S22–S32, S33–S34, S37–S38, S40–S45 |
| `design/proposal-artist.md` | §3.1 naming rules, §3.2 containers, §3.3 operator base and mask block, §3.4 generators/stylers/freeze/sculpt, §3.5 maps, §3.6 density and LOD, §3.7–3.9 the three examples, §3.10 versioning |
| `design/proposal-risk.md` | §2 target table (superseded for `usdGenSchema` by ADR §9 R19), §3.1 hierarchy, §3.2–3.4 common properties and mask block, §3.4 mask block and the SeExpr `curve()` codes, §3.5 ramp encodings, §3.6 maps, §3.7 freeze/sculpt, §3.8 contract C3, §3.9 versioning (the ASSUMPTION behind §8.3), §3.10–3.12 examples, §7.2–7.3 the look bake |
| `design/proposal-performance.md` | §4.1 namespace shape, §4.2 core properties, `usdGen:space` tokens, the ramp-position `.spline` rule and `UsdGenRestAPI`, §4.3 structural digest and wiring, §4.4 versioning, §5.6/§7.4 (the estimate behind §0.4's 10–150 ms ASSUMPTION), §11.2 gate ids |
| `04-operators.md` | §0.6 (`UsdGenDraw01`, the executable per-curve draw §2.13 calls), §0.13 (the fold-ins this document absorbed into §2.6–§2.8, §2.7.2 and §2.13), §2.1–§2.15 (the parameter rows now declared here), §3 (the v2 catalogue behind §2.7.2), §5.2 (the `combine` truth table and `bias`/`gain`/`biasGain`), §5.4 (the mask shortcut that consumes `usdGen:mask:influenceWidth`) |
| `07-look-maps-expressions.md` | §5.1 (the four map properties folded into §2.12 and the two `usdGen:map:filter` token sets), §5.3 (`PtexFilter::FilterType`), §8.1, §8.3 (`usdGen:paint:storage`, `usdGen:paint:resolution`) |
| `03-execution-engine.md` | §1.2, §3.6, §12.1 correction A (the density scales are **value**, not capture — §6.5) |
| `06-imaging.md` | §4.1 contract C2 (the unprefixed `primvars/clumpId_<level>`, `primvars/guideIndex`, `primvars/guideWeight` of §2.6 and §2.7.1) |
| `09-performance-and-benchmarks.md` | §5.2 (SI-7, SI-8 — tier and milestone), §5 as the single gate registry |
| `10-build-dependencies-testing.md` | §1.2 (the `usdGenSchema` target row §7.1's CMake fence follows), §4.2 (the `usdGenSchema` naming hazard) |
| `design/judge-evidence.md` | §0 verification table, §1 D1 scores, §2.1 shared defects (adapter coverage, unapplied mask API), §2.2–2.4 per-proposal violations, §3 grafts, §4 unresolved questions |
| `design/judge-artist.md` | §2.1–2.3 D1/D4 readings, §3 constraint violations (`usdGen:active`, `renderDensityScale` vs delegate identity, ramp `.spline` presentation), §4 grafts |
| `design/judge-delivery.md` | §5 phase-1 blockers (notice emission, initial population, session identity, `GeomSubset` semantics), §6 grafts |
| `research/G-stage-free-parameter-and-time-transport.md` | §1 codeless prims stage-free, §1.1 relationship primvars, §2 parameter transport table, §2.1 the ramp question, §3 rest surface, §4 containers through instancing, §5 invalidation, §7 registry, key facts |
| `research/G-freeze-bake-undo-and-frozen-reentry.md` | §1.4–1.5 `RemovePrim` and the edit target, §2.1 the C3 contract dump, §2.2 stage-free re-entry, §2.3 what usdImaging will not carry, §2.4 dirty-locator taxonomy, §3 landing places and the `primvars:rest` cost (`EV-048`), §4.2 the resync-per-stroke rule |
| `research/A4-openusd-hdgp-adapters.md` | §2.1 plugInfo keys, §2.2 Hydra 2.0 virtuals, §2.3 data sources and locators, §2.4 free vs implemented invalidation, §2.5 population predicate |
| `research/A7-prior-art-grooming.md` | §1.1 a host groomer's one Generator with a `Generate Primitives` mode, §1.5 render density multiplier, §3.6 a DCC primvar names, §7 mask survey, §9.1 generators, §9.2 stylers (S1 `Clump.method`), §9.3 freeze/sculpt semantics, §9.4 map prims |
| `research/A3-usdrig-tools.md` | §6 spec/plan conventions, §7.2 paint round trip |
| `research/A2-usdrig-imaging.md` | §3.2 rigExec's points replacement, §5 `__RigExecGenerated`, §6 `UsdGeomBBoxCache` |
| `research/A1-usdrig-graph.md` | §5 blend envelope and failure semantics, §6 weight objects and falloff LUTs |
| `research/A8-seexpr-ptex-libs.md` | §1.6 SeExpr costs, §2.6 `Far::PtexIndices` face-id mapping and the triangulation caveat, §2.8 Ptex costs, the `rand()` addition |
| `research/B-usdrig-build.md` | §7 generated plugInfo artefacts and the env snippet |
| `research/G-storm-throughput-and-prim-granularity.md` | §2 prim granularity and exact-size arrays |
| `research/G-storm-hair-look-prototype.md` | §2.4 primvar binding, §5 the mandatory primvar set |
| `research/G-instancing-cards-archives-and-native-instances.md` | §3 probe-2 cases (c) and (d) — prototype hiding is free only for namespace children of the instancer; §5–§8 and key facts: `instancedBy`, propagated prototype discovery, instancer dirtying |
| `research/G-motion-blur-sampling-strategy.md` | §4 shutter clamping and `forwardSurfaceSamples` |
| `appendix-A-evidence-ledger.md` | §2.6 row **EV-048** (`primvars:rest` +26 B against +9 600 046 B), §2.8 rows **EV-067** (SeExpr 13–117 ns/eval), **EV-069** (VarBlock 159 ns/eval/thread ≈ 50 M evals/s) and **EV-070** (Ptex 23–26 ns, 228 M/s); §2.11 (the 10–150 ms capture-per-gesture ASSUMPTION); §3.8 (`ts/types.h`, `usd/attribute.h`); §3.10 (`esfUsd/stageData.cpp:351, 361`); §2.0's legacy handle map. `EV-nnn` is the only citation handle for a measured number (ADR §9 R1, R43) |
| `research/ENVIRONMENT.md` | host facts and the CORRECTIONS block |
| `prototypes/stage-free-transport/` | `plugin/usdGenProbeSchema/resources/` (the codeless registration proof), `probe.cpp`, `probe6.cpp` (TsSpline transport) |
| `prototypes/freeze-bake/` | `probe4_frozen_reentry_si.cpp`, `probe7_locators_and_reentry.cpp` |
| `prototypes/instancing/` | `instProbe.cpp`, `instProbe3.cpp`, `out-probe1.txt`, `out-probe2.txt` |
| OpenUSD 26.08 (`<openusd-src>`, tag `v26.08`, merge commit `ee47c679a`) | Every file:line cited in this document, re-verified by grep: `usd/primDefinition.h`, `usd/schemaRegistry.cpp`, `usd/usdGenSchema.py`, `usd/attribute.h`, `usd/stage.cpp`, `base/ts/types.h`, `usdGeom/{curves.h, primvar.h, subset.h, boundableComputeExtent.cpp}`, `usdImaging/{adapterRegistry.cpp, dataSourceMapped.h, dataSourceAttribute.h, sceneIndexPrimAdapter.h, sceneIndexPlugin.h, geomSubsetAdapter.cpp, stageSceneIndex.cpp, piPrototypeSceneIndex.cpp, piPrototypePropagatingSceneIndex.cpp, plugInfo.json}`, `sdf/textFileFormatParser.h`, `imaging/hd/{sceneGlobalsSchema.h, geomSubsetSchema.{h,cpp}, instancerTopologySchema.h, sceneIndexObserver.h, sceneIndexAdapterSceneDelegate.cpp}`, `imaging/hdSt/primUtils.cpp`, `imaging/pxOsd/refinerFactory.h`, OpenSubdiv `far/ptexIndices.h`. Consolidated in `appendix-A-evidence-ledger.md`. |
| usdRig (`<usdrig-src>`) | `bin/gen_schema.sh:1-28`; `CMakeLists.txt:425-474` (the generated-plugInfo pattern); `libs/rigExecSchema/schema.usda`; `plugin/rigExecSchema/resources/plugInfo.json`; `docs/superpowers/specs/2026-09-01-graph-editor-design.md` (format) |
