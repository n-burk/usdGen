# usdGen — request, scope and glossary

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document fixes what usdGen is asked to do, how that request was read into nine requirements,
what ships in which version, which constraints are settled, how usdGen relates to usdRig, and the
vocabulary every sibling document uses. It contains no new design: every statement restates
`design/adr-v1.md` or `design/brief-v1.md` in the form an implementer needs on day one. Read it
once, then use §5 and §6 as reference.

Reads with: `01-architecture.md` (the shape the constraints produce), `02-schema.md` (the prim types
and properties named here), `11-roadmap.md` (the milestones M0–M8 the version scope maps onto),
`12-risks-decisions-open-questions.md` (everything §2 and §5 leave open), `README.md` (the index).

Status vocabulary, as in usdRig `docs/spec.md`: **EXISTS** = code or a file is present today ·
**BUILD** = a prototype builds and runs on this host · **MEASURED** = a number produced on this host
during the research rounds, always with its `EV-nnn` row in `appendix-A-evidence-ledger.md` ·
**DERIVED from EV-nnn** = scaled, summed or interpolated from a measurement, never called MEASURED ·
**UNMEASURED** = needs a gate or the workstation protocol · **PLANNED** = specified, not written ·
**ASSUMPTION** = added beyond the evidence. Every number carries exactly one of these tags
(ADR §9 R42); nothing interpolated is ever called MEASURED.

---

## 0. The request, and its reading into R1–R9

### 0.1 The request as recorded

The request, verbatim (2026-09-04):

> create a plan in ./plan to build a xgen like hair/fur instancing plugin for OpenUSD and Hydra 2.0
> (26.08 version). The result should be a grooming system whose nodes are hooked up and dynamically
> executed similar to ../usdRig and expressed as prims in OpenUSD. This should generate curves for
> rendering in hydra during run/rendertime and also be able to quickly load static curves and deform
> them with a deforming surface. They should also have stylers and hair modifiers similar to xgen's
> modifiers or unreal engine's curve modifiers for rigged or simulated curves. This system should
> work on top/after usdRig so the scene index should be expected to pickup the results of any
> rigged/deformed surface from usdRig (or any other geometry modifiers in openUSD). This system
> should also render hair/fur to the storm viewport and look similar to a rendered result with it's
> shader/material properties, artists/users should be able to express color via maps or ptex maps
> painted/referenced on surfaces (these likely will need to be texture loading prims specific to this
> plugin) and use seExpr2 expression to dynamically produce textures. Users should be able to chain
> together operators at will like curve generator -> clump styler -> curve generator -> clump styler
> -> frizz styler and the results should be very fast and interactive in the viewport. The graph
> should also support dirty propogation if a user updates a prim interactively. We should also plan
> for a toolset that users can freeze output results from a prim/operation and then modify manually,
> like groom/comb curves. These tools should be usdview plugins that utilize the storm viewport and
> follow the same app->data modifiction->hydra loop for fast interaction and then commit results to
> stage via the tooling

`design/brief-v1.md` §1 reads that text into the nine requirements below (headed "Requirements (from
the request)"); `design/proposal-artist.md` §0 restates it in one sentence — "an procedural hair/fur
grooming *and instancing* plugin for OpenUSD 26.08 / Hydra 2.0, on top of usdRig". R1–R9 are quoted
unchanged from brief §1 and are normative for this plan; the request's "instancing" half is carried by
`UsdGenInstance` — **v1**, built in M6 (ADR §9 R38) — not by an R-number.

```text
R1 Grooming nodes are prims on the stage, wired explicitly, executed dynamically in memory
   (nothing authored by the evaluator), results published to Hydra — "like usdRig".
R2 Curves are generated at run/render time in Hydra 2.0 (OpenUSD 26.08), in usdview/Storm and in
   usdrecord / hdPrman-class delegates, with one plugin binary.
R3 Static (frozen, cached, imported) curves load quickly and deform with a deforming surface.
R4 a host groomer/a host renderer-style stylers and modifiers, chainable at will
   (generator → clump → generator → clump → frizz …), applicable to generated, rigged or
   simulated curves.
R5 Works on top of / after usdRig: reads deformed surfaces from the scene index, whoever
   produced them (usdRig, UsdSkel, any other modifier).
R6 Storm viewport look close to a rendered result, driven by the material's properties.
R7 Colour from image maps or Ptex maps referenced/painted on surfaces through plugin-specific
   texture prims; SeExpr2 expressions producing textures/attributes dynamically.
R8 Very fast and interactive; precise dirty propagation on interactive prim edits.
R9 usdview tools on the Storm viewport: freeze an operator's output, then comb/groom manually;
   app → data modification → Hydra loop during interaction; commit to the stage via the tool.
```

Working name **usdGen**: sibling repo `<usdgen-src>` (empty today,
`research/ENVIRONMENT.md`), C++ prefix `UsdGen`, namespace `usdGen`, property namespace `usdGen:`,
plugin display name "usdGen". The reference system is usdRig (code name RigExec,
`<usdrig-src>`).

### 0.2 Acceptance per requirement

"Where" names the sibling document that owns the mechanism; the Gate column names the CI test that
decides the requirement is met (gate families in §5.4).

| # | Accepted when | Where | Gate |
|---|---|---|---|
| R1 | A groom is `UsdGenGroom`/`UsdGenDescription`/`UsdGenOperator` prims wired by `rel usdGen:input`; the evaluator writes to no layer, and generated prims live only in Hydra under `<Description>/__usdGenRender` (ADR §2.2). | `02-schema.md`, `06-imaging.md` | SI-7, plus a tier-T1 no-authoring test: `layerChangeCount == 0` across every commit and no authored prim under `<Description>/__usdGenRender` — the analogue of usdRig's `testRigExecNoAuthoring` (`research/B-usdrig-build.md` §3, ctest row 14) |
| R2 | One `usdGenImaging` binary whose four registrations (ADR §5.1) include exactly one renderer-level `HdSceneIndexPlugin` (`loadWithRenderer: ""`), so the same scene index produces the identical tile prim set under Storm ("GL"), all four RenderMan display names and `usdrecord` (S2), landing after the whole UsdImaging chain and before every Storm/hdPrman plugin (S1). | `06-imaging.md` | SI-5, SI-6 |
| R3 | A plain `UsdGeomBasisCurves` under the C3 primvar contract (S42) re-enters the graph from the scene index as an ordinary source node and deforms with its bound surface inside the deform-tail budget. | `05-static-curves-and-deformation.md` | E-1r, S-2, and `testUsdGenFrozenReentry` (`11-roadmap.md` §2.3, M2) |
| R4 | The request's own example chain — `Scatter → Clump → Scatter → Clump → Noise` — compiles and runs, and every styler accepts generated, frozen, imported or simulated curves through the same C3 input. | `04-operators.md` | E-6; per-operator **tier T1** value-and-locator tests (tier T1, not gate `T-1`) |
| R4b (instancing, from the request framing; no brief R-number) | `UsdGenInstance` emits a real Hydra `instancer` with hand-authored `instancedBy` (exactly one path) and prototypes as namespace children (S33); grooms on natively instanced scalps resolve per propagated prototype (S34); a click on an instance resolves through `primOrigin`. | `06-imaging.md`, `04-operators.md` | T-INST-1, T-INST-2 |
| R5 | usdGen reads `points` after every upstream deformer, including UsdSkel ext-computation points, through its private `HdSiExtComputationPrimvarPruningSceneIndex` wrapper (S3), with no change to usdRig. | `06-imaging.md` | SI-5 and `testUsdGenSkelInterop` (`11-roadmap.md` §2.3, M2) |
| R6 | The shipped `usdGenHairPreview.glslfx` renders from the same `UsdShade` inputs that the MaterialX and `UsdPreviewSurface` terminals carry (S35, S36), with the tangent strategy decided by measurement. | `07-look-maps-expressions.md` | S-8, L-1, L-2 |
| R7 | `UsdGenImageMap`/`UsdGenPtexMap`/`UsdGenExprMap`/`UsdGenPaintMap` sample on the CPU at capture and bake to per-curve/per-CV primvars (S37); SeExpr2 carries the a host groomer variable and function set (S38). | `07-look-maps-expressions.md` | L-3, L-4, L-5; T-EXPR-1, T-PTEX-1; E-8 re-run at M4 + the M4 exit criteria (`11-roadmap.md` §2.5: golden per-curve values for a density map, a length expression and a Ptex mask; paint round-trip primvar → EXR → `UsdGenImageMap` and primvar → `.ptx` → `UsdGenPtexMap` at ≤ 1e-4; `ReloadMaps` changes the groom and nothing else does) |
| R8 | A parameter edit re-runs only the dirty sub-graph on the dirty chunks and dirties only the leaves that changed, with exactly one cook per edit. | `03-execution-engine.md`, `09-performance-and-benchmarks.md` | E-2, E-3, SI-2, SI-3 |
| R9 | A freeze is one token edit, a brush drag costs no stage traffic per move, and release writes once inside an undo bracket. | `08-tools.md` | T-1, T-4 |

Requirement ids are R1–R9 (unhyphenated); gate ids in the R family are R-1…R-3; tiers are T0–T4 and
tool gates are T-1…T-5 plus T-EXPR-1, T-PTEX-1 and T-INST-1/2. **The hyphen is load-bearing.** Two
more letters are reserved by ADR §9 R1: **P0/P1/P2** are the motion profiles and nothing else, and
the engine principles are **I1–I8** (`01-architecture.md` §6); M0 pre-work items are **PW-1…PW-n**.

### 0.3 What the request does not settle

Three readings taken without a live user, all ASSUMPTIONs the plan depends on
(`design/proposal-artist.md` §1 items 1, 2 and 4): grooms are authored **per asset**, in the asset's
layer stack, with shot work limited to overrides, freezes and sim caches; the primary artist
application is **usdview plus the usdGen plugin**, with no DCC bridge in v1–v3; and v1 targets
**≤ 1 M rendered curves and ≤ 200 k interactive curves per description**, which makes the LOD ladder
mandatory above roughly 150 k curves (**ASSUMPTION**, `design/proposal-artist.md` §1 item 4; the
23.9 ms it argues from, 200 k × 8 CV at refineLevel 2, is MEASURED in
`research/G-storm-hair-look-prototype.md` §5).

Instancing is the one thing the request names that has no requirement line of its own: it is carried
as R4b above and as `UsdGenInstance`, which ADR §9 R38 places in **v1**, built in M6, constrained only
by S33 and S34.

---

## 1. What "procedural" and "a host renderer-curve-modifier-like" mean here

### 1.1 The nouns

usdGen takes a host groomer's object model, a DCC's parameter vocabulary and a host renderer's *data* arity where a
bake must round-trip (`design/proposal-artist.md` §2, with the ADR's type names):

| Artist noun | usdGen prim type | Hydra consequence |
|---|---|---|
| "my character's hair, all of it" | `UsdGenGroom` | none; a grouping and defaults scope |
| "the eyebrow description" | `UsdGenDescription` | 32–256 `basisCurves` tile prims, guides, optional instancer (ADR §1 S23/S27 — chunk ≠ tile; S33 for the optional instancer) |
| "the clump modifier" | `UsdGenClump` (a `UsdGenOperator`) | one node in the TBB DAG (S21) |
| "the density map I painted" | `UsdGenPaintMap` (a `UsdGenMap`) | a CPU sample at capture, baked to a per-curve array (S37) |
| "my guides" | `UsdGenGuideSet` + child `BasisCurves` | a `purpose = guide` curves prim plus a CV `points` prim (S40) |
| "the frozen groom" | `BasisCurves` under the C3 contract | a source node in the DAG (S42, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2) |

### 1.2 Parity summary

The catalogue with parameter lists and the a host groomer/a DCC/a host renderer column lives in **`04-operators.md`**;
`research/A7-prior-art-grooming.md` §9 is its source of truth. The summary an outside reader needs:

| Family | What usdGen takes |
|---|---|
| Generators | a host groomer's single generator with a mode becomes `UsdGenScatter`, `usdGen:mode = random \| uniform \| points \| atGuides` (ADR §2.1; A7 §9.1 G1–G4). `UsdGenGrow` = a DCC Guide Initialize (G5); `UsdGenGuideInterpolate` = a host groomer relative interpolation + a DCC radius/decay/angle + a host renderer unique/randomize (G6). |
| Stylers | a host groomer `Clumping` in full plus a DCC's fractal clump levels (`04-operators.md` owns the parameter list); frizz = `UsdGenNoise`; a host groomer Tilt = `UsdGenDirection`; Length, Width, Smooth, Straighten, Scale, Resample per ADR §2.1. |
| Deformers | a DCC Guide Deform and a host renderer's root-triangle binding are one operator, `UsdGenDeform`, `deformedSpace`, so it and its tail alone re-run per motion sample (S25). |
| a host renderer arity, where it is data | `UsdGenGuideInterpolate` emits `int[] primvars:usdGen:guideIndex` and `float[] primvars:usdGen:guideWeight`, both `uniform` with `elementSize = 3` — `int[3]`/`float[3]` are not USD type names (ADR §9 R24; `02-schema.md` §2.6) — matching the arity of a host renderer's `groom_closest_guides` (int32[3]) and `groom_guide_weights` (float[3]) (A7 §9). `UsdGenClump` emits `clumpId_<level>` (uniform int, a host renderer Clump ID; ADR §2.3). A bake round-trips. |
| Masks | a DCC's triad — skin mask, curve mask ramp, noise mask — is the auto-applied `UsdGenMaskAPI` on every `UsdGenOperator` (ADR §2.1). |

"a host renderer-curve-modifier-like" means the *modifier* semantics, not a host renderer's runtime: a chain of
per-curve / per-CV operations with an envelope (`float usdGen:blend = 1`) and stable ids, on the CPU.
a host renderer has no styling modifier stack of its own (`research/A7-prior-art-grooming.md` §6 feature
matrix, a host renderer column: clump, clump noise/copy/cut, noise/frizz, curl, bend/lift/direction and
smooth/straighten are all ○; §7, masks: "none per operator"), so the stack shape comes from a host groomer and
a DCC and only the attribute vocabulary comes from a host renderer.

### 1.3 What is deliberately not copied

a host groomer's Groomable Splines, which "do not follow deforming or animated meshes"
(`research/A7-prior-art-grooming.md` §1.1): the equivalent is `UsdGenFreeze` plus
`UsdGenSculptLayer`, which do. a host groomer's `.xpd`/`.xuv` sidecars: freeze tiers are `.usdc` session layer,
sublayer or payload, never `.usda` (S42).

---

## 2. Scope by version

### 2.1 v1 / v2 / v3

**v1 is the M0–M7 deliverable set and v2 is M8's breadth** (ADR §9 R38, which amends ADR §6's tier
labels). Type names are ADR §2.1's; the milestone in brackets is `11-roadmap.md` §2's.

| Version | Contents |
|---|---|
| **v1** (M0–M7) | `UsdGenScatter` — ADR §2.1 collapses risk §6.1's four `UsdGenScatterX` prims into one type with `usdGen:mode = random \| uniform \| points \| atGuides`; `random` [M1], `atGuides` [M3] and `points` [M5, with the Place-guide brush that authors it, `11-roadmap.md` §2.6] are v1, `uniform` is v2 (§2.3). Plus `UsdGenGrow` [M1], `UsdGenGuideInterpolate` [M3], `UsdGenCurveSource` [M2], `UsdGenFreeze` [M2], `UsdGenSculptLayer` [M2], `UsdGenClump` [M3] (full a host groomer set incl. copy/copyVariance/cut/flatness/offset/curl/crossover; `GuideInterpolate` with `regionMap` + `clumpCrossover`), `UsdGenNoise` [M1], `UsdGenLength` [M1], `UsdGenWidth` [M1], `UsdGenDeform` [M2], `UsdGenDirection` (a host groomer Tilt) [M3], `UsdGenSmooth` in `alongCurve` mode [M3], `UsdGenResample` [M3], `UsdGenScale` [M4]; `UsdGenInstance` (cards \| archives \| spheres) [M6]; the map set `UsdGenImageMap`, `UsdGenPtexMap` [M4], `UsdGenExprMap`, `UsdGenPaintMap`, `UsdGenNoiseMap`, `UsdGenCombineMap`, `UsdGenGuideProximityMap` [all M4]; the containers `UsdGenGroom`, `UsdGenDescription`, `UsdGenGuideSet` and the API schemas `UsdGenMaskAPI`, `UsdGenLookAPI`, `UsdGenRestAPI`, `UsdGenCurveAPI`; the mask block on every operator, the salted stable-id hash `hash(seed, curveId, saltPerOperatorType)` pinned by ADR §9 R12 (`UsdGenHash64`/`UsdGenHash32`, `02-schema.md` §2.19.1), `preserveLength`, a declared `Space()` per operator; the ten brushes [M5]; **motion profiles P0 (single), P1 (velocities) and P2 (samples)** [M7]. ADR §6's v2 label on `UsdGenPtexMap`, `UsdGenInstance` and motion profile P1 is superseded by ADR §9 R38 (`11-roadmap.md` §6.1, `04-operators.md` §1.2). |
| **v2** (M8) | `UsdGenCurl`, `UsdGenBend`, `UsdGenStraighten`, `UsdGenDisplace`, `UsdGenWave`, `UsdGenPart` (and its brush), `UsdGenExprOp` (SeExpr, capture-time only), `UsdGenScatter` `mode = uniform`, `TsSpline` ramps in the UI, sculpt rebase, progressive generation (gate T-5), the in-place overlay path (gate S-10). |
| **v3** | `UsdGenCollide`/Shrinkwrap, `UsdGenWind`/Force, `UsdGenSmooth` in `neighbours` mode, Braid, SimSource, an OpenExec backend behind the same operator interface, a third-party operator ABI, a GPU tail. |

### 2.2 User-visible outcome

* **v1 — "an artist can build, instance and render a groom."** Scatter on a deforming scalp,
  interpolate from guides, clump in levels, frizz, cut and taper, paint density and colour with
  image, Ptex, expression and paint maps, freeze any point in the stack and comb it by hand, emit
  cards, archives and spheres as real Hydra instancers, see it in Storm through a hair shader driven
  by the material's own inputs, and render it under hdPrman with motion blur (profiles P0 single,
  P1 velocities, P2 sampled `points`).
* **v2 — "the styling vocabulary fills out."** Curls, bends, waves, displacement, straightening,
  parting lines and their brush, TD-authored SeExpr operators, `UsdGenScatter`'s `uniform` mode,
  `TsSpline`-valued ramps in the UI (edited in usdRig's existing spline graph editor **where usdRig
  is installed**; usdGen ships no ramp editor of its own), sculpt rebase as one undoable action,
  progressive generation (gate T-5) and the measured in-place overlay path (gate S-10).
* **v3 — "the groom reacts."** Collision and shrinkwrap, wind and force, cross-curve smoothing,
  braids, simulation caches as first-class sources, a stable ABI for third-party operators.

### 2.3 How versions and milestones are tied together

Two axes, and ADR §9 R38 pins them to each other. **Milestones (M0–M8, ADR §7) say when the
machinery is built**: M0 skeleton and test tiers, M1 straight hair end to end, M2 deform and freeze,
M3 guides and clumps, M4 maps and expressions, M5 tools, M6 instancing, M7 render time and hardening,
M8 breadth. **Versions say which catalogue a release carries**, and R38 defines them by milestone:
**v1 = the M0–M7 deliverable set, v2 = M8's breadth**. A type's version is therefore read off its
milestone — `UsdGenPtexMap` (M4), `UsdGenInstance` (M6) and motion profiles P0/P1/P2 (M7) are v1,
which supersedes ADR §6's v2 label on them (`11-roadmap.md` §6.1, `04-operators.md` §1.2).

Inside v1 the modes still land in order: `UsdGenScatter`'s `random` at M1, `atGuides` at M3, `points`
at M5 with the Place-guide brush that authors it (`11-roadmap.md` §2.6). `mode = uniform` is **v2,
M8** (`04-operators.md` §1.2–§1.3); the properties it reads (`usdGen:spacingU`, `usdGen:spacingV`,
`usdGen:jitter`) ship in the schema from M1, so the upgrade is a value edit, never a schema change.

Calendar — all **ASSUMPTION**, ADR §7's re-baselined estimate as `11-roadmap.md` §4.1 applies it:
3 engineers, **+25 % on M1/M3/M5/M7 and +33 % on M2/M4, none on M0/M6** — a coarser instrument than
ADR §7's "30 % contingency on M1/M3/M7", with the same intent and the same totals — and no staffing
given (`11-roadmap.md` §4.2). **M0–M2 ≈ 11–12 weeks** for a deforming, freezable groom;
**M0–M5 ≈ 25 weeks** for "an artist can groom"; **M0–M7 ≈ 34 weeks** for "renderable on a show"
(33 weeks of milestones plus one week of integration float at the C4 freeze, ADR §9 R39).

---

## 3. Constraints and non-goals

Settled. Cite it; do not re-argue it. The ADR's amendments to the brief are in ADR §1, folded in
below.

### 3.1 OpenUSD 26.08 only

The target is the unmodified install at `$USD`, source tag v26.08
(`research/ENVIRONMENT.md`). Codeless schemas (`skipCodeGeneration = true`,
`pxr/usd/usd/usdGenSchema.py:208,246`) mean usdGen ships no generated schema C++ (S9). The install
has no Ptex, no OpenImageIO and no SeExpr, and `PXR_ENABLE_PTEX_SUPPORT` is OFF, so those are
vendored, static and hidden (S38). Only `hgiGL` is built here, which is why the Metal/Vulkan proxy
check (`HDST_ENABLE_HGI_RESOURCE_GENERATION=1`) is a gate, not an assumption (ADR §5.4).

### 3.2 usdGen runs after everything that moves geometry

usdGen's hair scene index is a **renderer-level `HdSceneIndexPlugin`** with `loadWithRenderer: ""`,
registered in C++ at insertion **phase 0, `InsertionOrderAtEnd`** (S1, MEASURED in
`research/G-chain-order-probe.md` §2–3): strictly after the entire UsdImaging chain — RigExec,
UsdSkel, flattening, instancing — and before every Storm and hdPrman plugin. The plugInfo entry ships
with `tags: ["usdGen:groom"]` and
`ordering: {after: ["hd:sceneGlobals"], before: ["hdGp:proceduralResolution", "hdPrman:motionBlur"]}`
(S1; the generated file is quoted verbatim in `06-imaging.md` §1.1), but ADR §1 records that the
`after` entry is **decorative** in stock 26.08 — no plugin carries that tag — so placement is
guaranteed by phase 0 / `InsertionOrderAtEnd`, never by the tag. A second
`UsdImagingSceneIndexPlugin` in the value path is rejected: pointer-ordered, and observed upstream of
UsdSkel (S1). The only `UsdImagingSceneIndexPlugin` usdGen registers is metadata-only, returning its
input unchanged, for `InstanceDataSourceNames()` and `ProxyPathTranslationDataSourceNames()` (S7,
ADR §5.1).

Three consequences bind every document. Everything usdGen reads is post-flattening, so
`xform/matrix` is world (or prototype-common) space with `resetXformStack = true` and dirtiness is
per prim, never hierarchical (S4). Skinned scalps are read through a **private**
`HdSiExtComputationPrimvarPruningSceneIndex` wrapped around usdGen's own input, never spliced into
the shared chain (S3, `pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:19,35`). Where
usdGen overrides another prim's `primvars` container it must dirty the bare `primvars` locator too,
or UsdSkel's resolved prim freezes (S5, ADR §5.2).

### 3.3 Storm preview parity is a product requirement

R6 is not "a shader that looks nice"; it is "the viewport is driven by the material's properties".
One `Material` prim carries three terminals: `outputs:surface` (a `UsdPreviewSurface` network, the
universal fallback), `outputs:mtlx:surface` (a MaterialX `chiang_hair_bsdf` network for render time)
and the Storm-specific glslfx (S36). Storm's render contexts are `glslfx` and, with MaterialX
compiled in, `mtlx` (`pxr/imaging/hdSt/renderDelegate.cpp:695-707` — `_RenderDelegateInfo()` opens at
`:695` and assigns `info.materialRenderContexts` at `:702-707`; ADR §9 R43), filtered by
`HdsiMaterialRenderContextFilteringSceneIndex`
(`pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45`). Whether Storm resolves
`outputs:glslfx:surface` ahead of plain `outputs:surface` is decided by **gate L-1** in M1; the
per-delegate binding override ships default **OFF** behind `USDGEN_STORM_MATERIAL_OVERRIDE`
(ADR §1 S36, ADR §5.6).

### 3.4 No changes to usdRig or OpenUSD source

usdGen changes no source in either tree (S44); it is a sibling CMake project against the unmodified
install. Three of the four defects found during research are usdRig's — the
`RigExecResultsSceneIndex` bare-`primvars` dirty (S5), the `testUsdviewRigExec.py` fixture drift and
the `bin/_env.sh` Linux gaps — filed, never patched here (S46). The fourth is stock OpenUSD's: the
resync predicate at `pxr/exec/esfUsd/stageData.cpp:361`
(`if (!UsdPrimDefaultPredicate(resyncedPrim)) {`; S41 and S46 cite line 360, the predicate is at 361)
makes `UsdStage::RemovePrim` raise a spurious `Tf.ErrorException` when an OpenExec system is
attached. Containment is a hard rule forced by OpenUSD, not by usdRig: **`RemovePrim` is never used
in an interactive path**; undo of a live freeze is `SetActive(false)` (S41).

### 3.5 Hydra 2.0 scene indices only

* **No hdGp.** Generative procedurals are gated off by default, their `GetChildPrim` is uncached,
  they are not chainable, and they sit downstream of Storm's velocity motion (S6). usdGen may later
  host its own resolver for third-party procedurals; hdGp is not the vehicle.
* **No OpenExec/VDF in the data plane.** MEASURED
  (`research/G-data-plane-engine-prototype-benchmark.md` §0, §4–8): a bespoke TBB DAG is **4× faster
  at 100 k curves, 9–15× at 1 M, 3–250× on sparse edits**, uses half the RSS, and scales with threads
  where VDF regresses; VDF's 500-element grain ignores curve boundaries and its scheduled affects
  masks freeze at schedule time (S21). The ExecUsd/usdExecImaging control plane is separately **not
  plugin-extensible in 26.08** — the adapter list is hard-coded (S16). Curve arrays never go through
  OpenExec; an OpenExec backend behind the same operator interface is a v3 option.

### 3.6 Stage-free transport

**No design element may require a `UsdStage` downstream of the stage scene index** (S8). The
evaluator runs entirely on Hydra data sources; the usdview C API is an optional accelerator and
`usdrecord`/hdPrman correctness never depends on it. Enforced at **link time**: the `usdGen` core
target links no `usd`, no `usdImaging` and no `hd` (ADR §6 build, stated exactly by ADR §9 R37 and
checked by gate **B-1**, which reads `DT_NEEDED` of `libusdGen.so`), and its input is
`UsdGenGraphDesc`, a pure-value description built by `usdGenImaging` from data sources
(ADR §4.2.3). Custom `usdGen:*`
properties are otherwise invisible to Hydra and emit **no notice** (S10, MEASURED in
`research/G-stage-free-parameter-and-time-transport.md` §1, §5), so a UsdImaging prim adapter
publishes them via `UsdImagingDataSourceMapped`
(`pxr/usdImaging/usdImaging/dataSourceMapped.h:26,43`), mappings built generically from
`UsdPrimDefinition::GetPropertyNames()` (`pxr/usd/usd/primDefinition.h:38`) — one adapter
implementation, `UsdGenPrimAdapterBase`, behind five registered one-line subclasses plus a
`UsdGenRestAPIAdapter` (ADR §2.3 as ADR §9 R3 states it; `06-imaging.md` §2).

### 3.7 Non-goals

No DCC bridge (a DCC, a DCC, a DCC) in v1–v3; the C ABI and the pxr_boost module are the
integration surface if one is ever written. No simulation solver — `UsdGenCurveSource` consumes sim
caches, it does not produce them. No GPU evaluation in v1/v2. No third-party operator ABI before v3
(`UsdGenOpRegistry` is internal, ADR §3). No authored output from the evaluator, so no `.usda` bakes
and no baked motion samples: a freeze is `.usdc`, motion is `velocities` or sampled `points` at
render time (S42, S32). Nothing on the stage tunes the machine: chunk size, thread limit and memory
budget are env/config only — `USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`, `USDGEN_MEMORY_BUDGET_MB`
(ADR §2.3) — and the scene index has one kill switch, `USDGEN_ENABLE` (ADR §5.1). The complete
env-var registry is `10-build-dependencies-testing.md` §3.5 and lives there alone (ADR §9 R35); no
other document, this one included, may introduce a `USDGEN_*` name.

---

## 4. Relationship to usdRig

usdRig is the reference system, not a dependency. Three relationships, kept apart on purpose.

### 4.1 Consumed at runtime — through Hydra, not through an API

usdGen never calls RigExec. It consumes RigExec's *results* exactly as it consumes UsdSkel's or any
other modifier's: post-flattening `points` on a mesh prim in its input scene, because usdGen's index
is inserted after the whole UsdImaging chain (S1, §3.2). The inbound invalidation contract —
`primvars/points/primvarValue`, per-prim `xform/matrix`, `UniversalSet` as a surface resync,
`PrimsAdded` on an existing path treated as all-dirty, `extComputationPrimvars` from UsdSkel — is
enumerated in `research/A2-usdrig-imaging.md` §9 and implemented in `06-imaging.md`. If usdRig is
absent, or a third-party deformer replaces it, nothing changes.

### 4.2 Reused as pattern — copied, not linked

| Pattern | Source | What usdGen copies |
|---|---|---|
| Generated-prim ownership | `<rig>/__RigExecGenerated` (`research/A2-usdrig-imaging.md` §5) | `<Description>/__usdGenRender`, Hydra-only, never authored (ADR §2.2) |
| Deferred commit, atomic snapshot publish, diffed dirties | `research/A1-usdrig-graph.md`; `research/G-evaluation-scheduling-and-batching.md` §5 | ADR §4.3's commit triggers; `GetPrim` only `atomic_load`s (S19) |
| Codeless schema tooling | `libs/rigExecSchema/schema.usda:23` (`skipCodeGeneration = true`) | the `usdGenSchema` **library** plugin (`"Type": "library"`, `LibraryPath = libusdGenSchema.so`) holding a codeless `generatedSchema.usda` generated by `usdGenSchema.py` — a resource-only plugin cannot register `UsdGenDescription`'s compute-extent function (S9, ADR §9 R19; `10-build-dependencies-testing.md` §4.3) |
| Generated plugInfo | `LibraryPath = $<TARGET_FILE_NAME:>` written into `<build>/usd/<name>/resources` | copied verbatim (S44) |
| usdview plugin conventions | `plugin/rigExecUsdview/rigExecUsdview.py` | `PluginContainer`, state in an `__init__`-initialised object, lazy Qt imports, the signal's frame not the property, `UpdateViewport()` after every edit (S43) |
| Undo | `plugin/rigExecUsdview/rigExecUndo.py:20,115,144,203` (`AttributeSnapshot`, `Edit`, `UndoStack`, `EditRecorder`) | the same shared stack and recorder, plus a new `SubtreeSnapshot` for prim-spec edits (S41) |
| Python facade shape | `python/rigexec/__init__.py:525` (`rigexec.Builder`) | the `usdgen` package's `builder.py` facade — the package name is all lowercase, mirroring `rigexec`; `usdGenPy` is not a name (ADR §9 R5; `10-build-dependencies-testing.md` §1.1, §1.2) |
| Document format | `docs/superpowers/specs/`; `research/A3-usdrig-tools.md` §6 | this plan's section skeleton and citation style |

### 4.3 Optionally linked

`find_package(rigExec CONFIG)` is **optional** and brings `rigExec::rigExecMath` (RMF frames, ribbon
transport, extent, weight fields, envelope) into `usdGenMath` (S44). The known trap is CMake 3.28's
`find_dependency` call-hash short-circuit: if usdGen also calls `find_package(pxr …)` itself, the
configure aborts on a duplicate `TBB::tbb` imported target (MEASURED, `research/B-usdrig-build.md`
§8). Let `find_package(rigExec)` bring pxr in transitively, or guard with `if(NOT TARGET TBB::tbb)`;
`usdGenConfig.cmake` wraps its own pxr dependency in `if(NOT TARGET usd)`. usdGen must build and run
with `USDGEN_WITH_RIGEXEC=OFF`.

### 4.4 Not reused

RigExec's OpenExec/ExecUsd control plane (S16); its `VdfNetwork` mover graph (S21); its namespace
ordering rule — usdGen wires explicitly with `usdGen:input`, and namespace order is only the Kahn
tie-break (S26); and `AttributeSnapshot`-only undo, since `EditRecorder` snapshots attribute specs
only (`research/A3-usdrig-tools.md` §2).

---

## 5. Glossary

### 5.1 Objects

* **Groom** — `UsdGenGroom`, the session root. Artist-facing globals only (`usdGen:densityScale`,
  `usdGen:renderDensityScale`, `uniform int usdGen:schemaVersion = 1`,
  `uniform string usdGen:sessionId`); never machine tuning.
* **Description** — `UsdGenDescription : UsdGeomBoundable`, one grooming unit ("the eyebrows"),
  owning the chain, guides, maps, prototypes, freezes and published tiles. Terminal:
  `rel usdGen:terminal`, exactly one target.
* **Operator** — `UsdGenOperator`, the abstract base of every node: `rel usdGen:input`,
  `bool usdGen:enabled = true`, seed, space, read phase, `float usdGen:blend = 1`, label, plus the
  auto-applied `UsdGenMaskAPI` mask block. `usdGen:enabled` is **non-structural** for
  topology-preserving operators (a memcpy pass-through: no recompile, no recapture) and routes as
  `UsdGenDirtyTopology` — recapture downstream and one topology publish, still no recompile — for
  generators, `UsdGenResample` and `UsdGenLength` (whose static `TopologyEffect()` is
  `MayChangeCurveCount` because cull mode exists). It is never a digest term, and a muted generator
  keeps its capture, so ids survive mute/unmute (ADR §2.3 as ADR §9 R14 amends it;
  `01-architecture.md` §6).
* **Generator / Styler / Deformer** — `UsdGenGenerator` may change curve or CV counts;
  `UsdGenStyler` is topology-preserving (`UsdGenResample` and length-culling are the declared
  exceptions); `UsdGenDeformer` is deformed-space and re-runs per motion sample (S25).
* **Guide / GuideSet** — `UsdGenGuideSet` names a set; its children are `BasisCurves` under C3, with
  a per-guide `float[] usdGen:blend` (a host groomer's range of influence, ADR §2.3).
* **Map** — `UsdGenMap`, a field prim: Image, Ptex, Expr, Paint, Noise, Combine, GuideProximity. The
  map is the *field*; the *slot* consuming it is the operator's mask block.
* **Freeze** — `UsdGenFreeze`, which **caps** the chain: `rel usdGen:frozen:curves`,
  `uniform token usdGen:frozen:mode = "frozen"` (`frozen | live`),
  `uniform string usdGen:frozen:epoch`, `uniform token usdGen:frozen:tier = "session"`
  (`session | sublayer | payload`, advisory). Upstream stays authored and greyed; unfreeze is one
  token edit (`02-schema.md` §2.9 owns the rows).
* **SculptLayer** — `UsdGenSculptLayer`: per-curve/per-CV deltas in the root frame keyed by stable id,
  with a weight, an epoch and a locked-curve set. A stale layer gets a badge and a "Rebase sculpt"
  action, never silent misalignment.

### 5.2 Engine vocabulary

* **Session** — evaluator state for one (stage or `usdGen:sessionId`, groom root) pair in the
  process-global `UsdGenImagingRegistry`. Many scene index instances attach to one session; it owns
  one current frame, one context and one generation, and the prim set never differs per index
  (ADR §4.5, S15).
* **Generation** — one immutable published snapshot of the output buffers. Readers `atomic_load` it
  in `GetPrim`; notices leave only from the commit path (S19).
* **Capture vs evaluate** — **capture** is topology-dependent work (scatter, stable ids, root
  bindings, kd-trees, guide weights, clump ids, map/Ptex/SeExpr samples), cached by an epoch digest;
  **evaluate** is per-frame, per-chunk parallel. A frame re-runs the deformed-space tail only
  (S25, I4).
* **Chunk vs tile** — a **chunk** is 512 curves, curve-aligned (128–1024 acceptable,
  `USDGEN_CHUNK_SIZE`): the unit of dirtiness and parallelism, one dirty byte per chunk per node. 512
  is a decision (S23, ADR §1) whose basis is MEASURED in
  `research/G-data-plane-engine-prototype-benchmark.md` §5. A **tile** is one published `basisCurves`
  prim = `chunksPerTile` chunks, with the arithmetic of ADR §9 R21:
  `chunksPerTile = max(1, ceil(nChunks / tileTarget))` and
  **`nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))`**. The outer `min` is
  load-bearing: without it a groom with fewer than 32 chunks is asked for more tiles than it has
  chunks (`appendix-A-evidence-ledger.md` §6 K24). Worked: 100 k curves at chunk 512 and
  `tileTarget = 64` → 196 chunks, 4 chunks per tile, **49 tiles**; 64 tiles is the 1 M-curve figure.
  The 32–256 bound is **source-derived** (`research/G-storm-throughput-and-prim-granularity.md` §2,
  whose heading marks it GPU-UNMEASURED — the CPU half is MEASURED in §1.7, the GPU half is open
  question Q-02 in `12-risks-decisions-open-questions.md` §3), and
  `uniform int usdGen:tileTarget = 64` is an **ASSUMPTION** default. S23's "chunks are Hydra prims"
  is superseded by ADR §1 (invariant I5): 512-curve chunks at 1 M curves cannot also be ≤ 256 prims.
* **Reference lane** — hair chunks never read hair chunks. Guides, clump centres and card roots are
  un-chunked reference buffers evaluated to completion first; consumers store `guideIdx[3]`,
  `guideW[3]` and `clumpId[level]` at capture (I3, ADR §4.1). Cross-chunk operators outside the lane
  are v3 and get a two-pass gather/scatter node kind — never a chunk fan-in.
* **Epoch / digest** — three hashes (ADR §4.1, §4.2.1). The **Merkle structural digest**
  `d(n) = H(type, algorithmVersion, mode, readPhase, space, sorted(input paths),
  sorted(reference/map/surface targets), d(inputs...))` decides recompiles and hashes `usdGen:input`
  **ancestors**, not namespace children; `enabled` and `seed` are excluded. The 128-bit **capture
  epoch** keys the capture cache and `seed` bumps it; the **value version** decides re-evaluation.
  `usdGen:frozen:epoch` carries the `usdgen1:sha1:` prefix.
* **Commit trigger** — the three ADR §4.3 events, as ADR §9 R32 amends them, that turn accumulated
  dirty state into a new generation: (a) `UsdGenImaging_Commit()`/`SetTime()` from an application
  driver, (b) a `/ sceneGlobals/currentFrame` dirty in stock hosts, (c) an operator-parameter, map or
  surface-topology dirty, which **always** commits synchronously at the end of the `_PrimsDirtied`
  batch that carried it, driver attached or not — exactly one cook per edit batch, so an application
  never calls `Commit()` for a stage edit. With (a) attached, only (b) degrades to setting the dirty
  flag. Nothing cooks in `GetPrim` (S17, I7; `01-architecture.md` §4.1, `06-imaging.md` §3.9).
* **Context** — `interactive` or `render`, set **explicitly**: `USDGEN_CONTEXT=render` (batch),
  `UsdGenImaging_SetContext()` (apps), or a usdview toggle. The renderer's display name is not
  intent, and `HdSceneGlobalsSchema` has no interactive flag
  (`pxr/imaging/hd/sceneGlobalsSchema.h:37-47`). The contexts are exclusive: `usdGen:densityScale`
  applies in `interactive`, `usdGen:renderDensityScale` in `render`, and neither applies in the
  other (ADR §9 R13, which amends ADR §2.3's "always applied").

### 5.3 Contracts C1–C5

Frozen early because assets reference them; everything else may churn until M7 (ADR §3).

| # | Contract | Frozen at |
|---|---|---|
| C1 | `usdGen:` property names and prim type names | end of M1 |
| C2 | the published tile `basisCurves` contract (ADR §5.3) | end of M1 |
| C3 | the curve contract for guides, freezes, imports and sim caches (S42 + `primvars:usdGen:role`) | end of M2 |
| C4 | the C ABI + pxr_boost array surface (`08-tools.md` §1.4 is its single source; `06-imaging.md` §3.8 repeats it verbatim — ADR §9 R31) | end of M5 |
| C5 | glslfx parameter names (the shipped file's `inputs:` block, verbatim) | end of M1 |

### 5.4 Tiers T0–T4 and gate ids

Test tiers, in the order a change hits them (ADR §9 R2; `design/proposal-risk.md` §9.1, S45; the
definitive statement is `10-build-dependencies-testing.md` §5.1):

| Tier | What | Where it runs |
|---|---|---|
| **T0** | pure engine: `UsdGenGraph`/kernels/operators over synthetic buffers, no Hydra, no USD | every commit, milliseconds |
| **T1** | headless scene-index tests over the real `UsdImagingCreateSceneIndices` chain with a recording observer, asserting values **and** emitted dirty locators | every commit; the primary regression suite, target < 100 ms each (**ASSUMPTION** — design budget, `design/proposal-risk.md` §9.1) |
| **T2** | Storm through the EGL device-platform harness (`prototypes/storm-hair-look/eglctx.h`) on the GB10: golden images and GPU frame timing | every commit for correctness, nightly for timing |
| **T3** | `testusdview` scripts under the scratchpad Xvfb (`DISPLAY=:77`, llvmpipe — **CPU numbers, never Storm numbers**) | pre-merge |
| **T4** | workstation protocols: MSAA/OIT quality, Metal/Vulkan Hgi, non-NVIDIA drivers, a real hdPrman install, 4K | manual; **release criteria, never a milestone exit** (ADR §7) |

Gate ids carry a family prefix and are always hyphenated (ADR §9 R2). The complete registry — id,
metric, pass criterion, driver, tier, milestone and status — is
**`09-performance-and-benchmarks.md` §5**, and this list is a transcription of its ids, not a second
opinion (ADR §9 R40, R44):

| Family | Ids and what each measures | Tier |
|---|---|:--:|
| **B-** build | **B-1** the link rule: `DT_NEEDED` of `libusdGen.so` against the forbidden list (`usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`, `usdAppUtils`), M0 | T0 |
| **E-** engine | **E-1** chain throughput · **E-1r** the ragged path · **E-2** sparse edit · **E-3** terminal-parameter edit · **E-4** capture/kNN · **E-5** memory · **E-6** recompile · **E-7** thread scaling · **E-8** determinism | T0 |
| **SI-** scene index | **SI-1** array exactness · **SI-2** invalidation locator set · **SI-3** cook count · **SI-4** no torn read · **SI-5** chain order · **SI-6** initial population, both paths · **SI-7** adapter coverage · **SI-8** auto-applied `UsdGenMaskAPI` on a codeless type · **SI-9** the pruning-wrapper cost on a production-density skinned scalp · **SI-10** two indices on one session agree on generation, prim set and frame · **SI-11** the `reorder nameChildren` notice check, record only | T1 |
| **S-** Storm | **S-1** static draw at 720p and 1080p · **S-2** deform publish · **S-3** one-tile edit · **S-4** culling · **S-5** batching · **S-6** no VBO relocation · **S-7** density scrub · **S-8** `hairTangent` strategy · **S-9** refineLevel switch cost · **S-10** the v2 in-place overlay · **S-11** `head1M`, record only · **S-12** the tile-count sweep before C2 freezes | T2 |
| **L-** look | **L-1** render-context resolution · **L-2** all three shipped `.glslfx` files (parse, compile, golden, refineLevel precedence; guards C5) · **L-3** map and expression determinism · **L-4** map capture cost · **L-5** `ReloadMaps` invalidation | T2 (L-1, L-2), T0–T1 (L-3, L-4, L-5) |
| **T-** tools | **T-1** brush move · **T-2** `UsdGenImaging_PickCV` cost · **T-3** pick accuracy · **T-4** freeze authoring · **T-5** progressive generation (v2) · **T-EXPR-1** the SeExpr sandbox · **T-PTEX-1** Ptex face-id agreement · **T-INST-1/2** instancer pick round-trip and prototype rebasing | T3 (T-1, T-3, T-4, T-5), T1 (T-2, T-INST-1/2), T0 (T-EXPR-1, T-PTEX-1) |
| **R-** render time and workstation | **R-1** hdPrman parity · **R-2** MSAA/OIT quality · **R-3** Metal/Vulkan Hgi and non-NVIDIA compile | T4, release criteria only |

Thresholds and tiers are in `09-performance-and-benchmarks.md` §5; the milestone each gate exits is
in `11-roadmap.md` §2.0, with the release criteria RC-1…RC-11 in `11-roadmap.md` §6.4. Five gates are
recorded as M0 pre-work and become binding later: E-4 (M3), S-8, S-9, L-1 and SI-8 (all M1). T-5 and
S-10 are v2 (M8) criteria, never v1 release gates (ADR §9 R38, R39).

---

## 6. Document map and reading orders

### 6.1 The documents

| Document | In one sentence |
|---|---|
| `README.md` | Index, TL;DR, status vocabulary. |
| `00-request-and-scope.md` | This document: request, requirements, version scope, constraints, glossary, map. |
| `01-architecture.md` | Three-part shape, the frame budget, **engine invariants I1–I8** (never P — P0/P1/P2 are the motion profiles, ADR §9 R1), contracts, sessions, commit triggers. |
| `02-schema.md` | **The single normative property registry** (ADR §9 R7): every prim type and property with type, default, allowed tokens and dirty class, plus the reserved layout, mask block, maps, freeze/sculpt, versioning and the `.usda` examples. |
| `03-execution-engine.md` | `UsdGenGraphDesc`, node interfaces, SoA buffers, chunks, capture/evaluate, digests, dirty routing, threads, memory, diagnostics. |
| `04-operators.md` | The catalogue: parameters, mathematics, cost classes, spaces, emitted primvars, parity, v1/v2/v3 split. |
| `05-static-curves-and-deformation.md` | C3 curve contract, frozen re-entry, rest surfaces, deformation modes, sculpt layers, freeze tiers. |
| `06-imaging.md` | The four registrations, tile contract, invalidation discipline, guides, instancers, live overrides, `primOrigin`, population. |
| `07-look-maps-expressions.md` | Three material terminals, the glslfx, MaterialX, colour baking, map types, Ptex face mapping, SeExpr, reload. |
| `08-tools.md` | The usdview plugin: module split, `UsdGenToolState`, two Python surfaces, brushes, freeze/commit, undo, panels. §1.4 is **the single source of the C ABI** (contract C4, ADR §9 R31). |
| `09-performance-and-benchmarks.md` | Performance model and measurement protocols. §0.2 is **the only frame ledger** and §5 is **the single gate registry** — id, metric, pass criterion, tier, milestone, status (ADR §9 R40, R41). |
| `10-build-dependencies-testing.md` | CMake targets and options, vendored third party, install layout, test harness by tier. §3.5 is **the single env-var registry** (ADR §9 R35) and §5.1 states the T0–T4 tier definitions (ADR §9 R2). |
| `11-roadmap.md` | M0–M8 with deliverables, exit criteria, gates, estimates, stop conditions. |
| `12-risks-decisions-open-questions.md` | Risk register, decisions taken and rejected, everything still open. |
| `appendix-A-evidence-ledger.md` | Environment facts, every MEASURED number as an **`EV-nnn`** row with its report section (the only citation handle for a measurement, ADR §9 R1, R43), and the file:line index. |
| `appendix-B-prototype-inventory.md` | What exists in `plan/prototypes/` and what is carried into the repo. |

### 6.2 Reading orders

* **Engine engineer** — `00` → `01` → `03` → `02` §§ on operator properties and versioning (the
  fields the structural digest hashes, ADR §4.2.1) → `05` → `04` → `09` → `10` → `11`. `01` sets the
  frame budget; `03` is what you implement; `04` is the workload it runs.
* **Imaging engineer** — `00` → `01` (sessions, commit triggers) → `06` → `02` (which properties must
  appear as data sources) → `05` → `09` (S- gates) → `07` → `10`.
* **Tools engineer** — `00` → `01` → `08` → `02` (what the panels generate from) → `05` (freeze and
  sculpt semantics) → `06` (live overrides, `primOrigin`, CV prims) → `10`.
* **TD / artist reviewer** — `00` → `02` (the three `.usda` examples first) → `04` → `07` → `08` →
  `11` → `12`. Skip `03`, `06` and `09` unless a number is disputed.

---

## 7. Testing

This document defines no mechanism, so nothing here is tested directly; what it asserts is proved by
the sibling documents' tests. Three properties of *this* document are checked mechanically in CI by a
repo docs lint (`testUsdGenDocs`), which runs beside the test tiers and belongs to none of T0–T4:

1. **Name agreement.** Every prim type, property, env var, CMake target, test name and gate id named
   in §0, §1, §2 and §5 also appears in the sibling document that owns it, and every `UsdGen*` type
   name appears in `02-schema.md`. Any name here that `02-schema.md` does not define is an error.
2. **Citation resolution.** Every `research/`, `design/`, `prototypes/` path and every OpenUSD
   `file:line` in this document resolves, and the cited line still contains the symbol claimed.
3. **Cross-reference integrity.** Every sibling document named in §6 exists; every "see `NN-….md`
   §X" points at a section that exists in the current sibling (ADR §9 R45); and every gate id in §5.4
   appears in `09-performance-and-benchmarks.md` §5 with a threshold, a tier and a milestone, and in
   `11-roadmap.md` §2.0 with a milestone exit **or** in §6.4 as a release criterion — the latter being
   exactly the tier-T4 gates R-1, R-2 and R-3, whose milestone column in
   `09-performance-and-benchmarks.md` §5.4 reads "release" (R-1 is built in M7 and signed off at
   release; `11-roadmap.md` §6.4 RC-2). T-1, T-3 and T-4 are tier-T3 milestone exits at M5; T-2 is
   tier T1 and also exits M5.

The acceptance statements in §0.2 are the release checklist: R1–R9 and R4b are met when their named
gates are green at the tier each gate names, with T4 gates as release criteria rather than milestone
exits (ADR §7).

## 8. Out of scope

No schema definitions (`02-schema.md`), no engine or imaging APIs (`03-execution-engine.md`,
`06-imaging.md`), no operator parameters (`04-operators.md`), no thresholds
(`09-performance-and-benchmarks.md`), no build files (`10-build-dependencies-testing.md`), no
schedule (`11-roadmap.md`). It does not re-argue any settled decision S1–S46 or any ADR decision;
disagreements go to `12-risks-decisions-open-questions.md` as open questions, never into a
redefinition here. It does not list the rejected alternatives in full — ADR §1 and §8 hold that list.

## 9. Sources

| Source | Sections used |
|---|---|
| `design/brief-v1.md` | preamble and §1 (request record, R1–R9); §2 S1–S46 (constraints, incl. §2.6 S33/S34); §3 D1–D8; §4 (deliverables) |
| `design/adr-v1.md` | §1 (amendments), §2 (object model, layout, contested properties), §3 (C1–C5), §4.1–4.5, §5 (registrations, invalidation, tiles, S-8/S-9/L-1), §6 (look, tools, build), §7 (M0–M8, gates, estimates), §8 (map), **§9 R1–R45** (vocabulary, the property and gate registries, the v1/v2 split R38, the number tags R42, the accepted file:line ranges R43) |
| `design/proposal-risk.md` | §0 (thesis, C1–C5), §1 (slice ordering), §2 (targets), §6.1–6.3 (catalogue, incl. the four `UsdGenScatterX` prims ADR §2.1 collapses), §9.1 (tiers) |
| `design/proposal-performance.md` | §0.1–0.3 (thesis, frame ledger, and the engine principles this plan renames **I1–I8**, ADR §9 R1), §1 (assumptions), §2 (F1–F14), §11.2 (gate table) |
| `design/proposal-artist.md` | §0 (request and lens), §1 items 1–9 (assumptions), §2 (the five nouns) |
| `design/judge-evidence.md`, `judge-artist.md`, `judge-delivery.md` | the rejected-idea lists folded into ADR §1 and §8 |
| `research/A1-usdrig-graph.md` §0; `research/A2-usdrig-imaging.md` §5, §9; `research/A3-usdrig-tools.md` §2, §6, §7–8 | RigExec's two engines and deferred publish; generated-prim ownership and the invalidation contract; undo, the spec/plan format, plugin conventions |
| `research/A7-prior-art-grooming.md` | §1.1 (a host groomer data model), §6 (feature matrix), §7 (masks per system), §9 (reference operator catalogue) |
| `research/B-usdrig-build.md` §3, §8; `research/ENVIRONMENT.md` | ctest roster incl. `testRigExecNoAuthoring`; the double-`pxrConfig` trap; host facts and the CORRECTIONS block |
| `research/G-chain-order-probe.md` §2–3; `research/G-data-plane-engine-prototype-benchmark.md` §0, §4–8 | measured renderer-level placement; TBB DAG vs VDF; the 512-curve chunk basis (§5) |
| `research/G-evaluation-scheduling-and-batching.md` §4–5, §9; `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2–2.3; `research/G-stage-free-parameter-and-time-transport.md` §1, §5 | commit model, notice counts; frozen re-entry; no notice without an adapter |
| `research/G-storm-hair-look-prototype.md` §5; `research/G-storm-throughput-and-prim-granularity.md` §1.3–1.6, §2 | 200 k curves at refineLevel 2 = 23.9 ms; the prim as upload unit, exact-size arrays, the 32–256 bound |
| `prototypes/storm-hair-look/` | `eglctx.h`, `usdGenHairPreview.glslfx` (the T2 harness and the shipped shader) |
| OpenUSD 26.08 source | `pxr/imaging/hdSt/renderDelegate.cpp:695-707`; `pxr/imaging/hd/sceneGlobalsSchema.h:37-47`; `pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45`; `pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:19,35`; `pxr/usdImaging/usdImaging/dataSourceMapped.h:26,43`; `pxr/usd/usd/primDefinition.h:38`; `pxr/usd/usd/usdGenSchema.py:208,246`; `pxr/exec/esfUsd/stageData.cpp:361` |
| usdRig source | `libs/rigExecSchema/schema.usda:23`; `plugin/rigExecUsdview/rigExecUndo.py:20,115,144,203`; `plugin/rigExecUsdview/rigExecUsdview.py`; `python/rigexec/__init__.py:525`; `docs/superpowers/specs/2026-09-01-graph-editor-design.md` |
