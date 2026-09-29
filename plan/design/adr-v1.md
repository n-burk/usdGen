# usdGen — Architecture Decision Record v1 (binding synthesis, 2026-09-04)

Written by the orchestrating thread after reading, in full: the research (`../research/A1..A8`),
the verification reports (`../research/G-*`, `B-usdrig-build`), the three architecture proposals
(`proposal-artist.md`, `proposal-performance.md`, `proposal-risk.md`) and the three judge reports
(`judge-evidence.md`, `judge-artist.md`, `judge-delivery.md`). `brief-v1.md` §2 (S1–S46) remains in
force except where §1 below amends it. This ADR is **binding** for the plan writers: where a proposal
and this ADR disagree, this ADR wins; where this ADR is silent, the source proposal named in §8 wins.

Panel verdict (all three judges independently): **base = `proposal-risk.md`; engine and imaging
chapters = `proposal-performance.md`; schema legibility, operator vocabulary, look block and tool UX =
`proposal-artist.md`.** Overall scores: risk 8.4/8.4/8.8, performance 8.3/7.6/8.4, artist 8.1/8.1/7.6.

---

## 1. Amendments to brief-v1 (recorded so nothing is a silent departure)

| Ref | Amendment | Reason |
|---|---|---|
| S23/S27 | **Chunk ≠ tile.** Engine chunk = 512 curves (dirty/parallel unit); Hydra prim = a *tile* = `chunksPerTile` chunks, `nTiles = clamp(ceil(nChunks/chunksPerTile), 32, 256)`. S23's "chunks are Hydra prims" is superseded. | 512-curve chunks at 1 M curves cannot also be ≤ 256 prims; the split is the only arithmetic that satisfies both measurements (performance P5; all judges). |
| S1 | The JSON `ordering.after: ["hd:sceneGlobals"]` is **decorative** in stock 26.08 (no plugin carries that tag; the scene-globals index is inserted by the engine's app callback, `engine.cpp:150-156`). Placement after it is guaranteed by phase 0 / `InsertionOrderAtEnd`, not by the tag. Keep the tag; never rely on it. | judge-evidence §0 |
| S11 | Ramp encodings ratified: scalar ramp = `float2[] <p>:knots` + `token <p>:interpolation`; colour ramp = `float[] <p>:positions` + `color3f[] <p>:colors` + interpolation (a `TsSpline` cannot hold colours); optional whole-`TsSpline` transport on `float <p>:spline` via the adapter factory (never flagged time-varying). A `.spline` on a *ramp* property is a compile error; a `.spline` on a plain scalar parameter means "animated". | risk §3.5, performance §4.2, judge-evidence |
| S18(c) | The **`GetPrim` backstop is withdrawn.** A reader thread cannot emit notices and a cook that emits no notices is useless (Storm only re-pulls what was dirtied). Replaced by trigger (c) in §4.3: operator-parameter dirties commit synchronously on the notice thread when no application driver is attached. | judge-delivery §5.1, judge-evidence unresolved #1 |
| S24 | `VtArray` copy-on-write remains the default handoff. Performance's double-buffered publish ring is permitted **only** as an optimisation guarded by `VtArray::IsUnique()` on the back buffer; never patch a buffer that another holder references. | judge-delivery §3.4, judge-evidence §2.3 |
| S29 | `hairTangent` publication is a **measured decision, not a mandate** (§5.4 below). `hairId` is a uniform **float** in [0,1) = `hash32(curveId)/2^32` (the shipped glslfx declares it `float`). | judge-delivery §4.1; judge-evidence §2.3 |
| S31 | "LOD = curve decimation" stands, but the *justification* is corrected: refineLevel 1 was measured at 8.17 ms vs 23.93 ms for level 2 at 200 k curves and is a legitimate 2.9× lever whose *switch cost* (cubic index rebuild + batch revalidation) is unmeasured. Gate S-9 decides whether a refineLevel-1 "tumble tier" ships. | all three judges |
| S36 | Source evidence says Storm resolves `outputs:glslfx:surface` before plain `outputs:surface` (`hdSt/renderDelegate.cpp:701-707`, `hdsi/materialRenderContextFilteringSceneIndex.h:20-45`). The per-delegate binding override ships **default OFF** behind `USDGEN_STORM_MATERIAL_OVERRIDE`, decided by gate L-1 in milestone M1. | judge-evidence §0 |

---

## 2. Object model and schema (D1) — decided

### 2.1 Type hierarchy (all codeless, S9)

```
UsdGeomImageable
 ├─ UsdGenGroom              session root; artist-facing globals only (no machine tuning)
 ├─ UsdGenDescription        : UsdGeomBoundable (compute-extent fn lives in usdGenImaging)
 └─ UsdGenGuideSet           a named guide set; children are BasisCurves under contract C3
UsdTyped
 ├─ UsdGenOperator (abstract)            usdGen:input, enabled, seed, space, readPhase, blend, label
 │   ├─ UsdGenGenerator (abstract)       may change curve/CV counts
 │   │   ├─ UsdGenScatter                mode = random | uniform | points | atGuides
 │   │   ├─ UsdGenGrow
 │   │   ├─ UsdGenGuideInterpolate
 │   │   └─ UsdGenCurveSource            frozen / imported / simulated curves enter here
 │   ├─ UsdGenStyler (abstract)          topology-preserving (Resample/Length-cull excepted)
 │   │   └─ Clump · Noise · Curl · Bend · Direction · Length · Width · Smooth · Straighten ·
 │   │      Displace · Wave · Scale · Resample · SculptLayer · ExprOp(v2, capture-time only)
 │   ├─ UsdGenDeformer (abstract)        deformed-space; re-runs per motion sample
 │   │   └─ UsdGenDeform · UsdGenCollide(v3) · UsdGenWind(v3)
 │   ├─ UsdGenFreeze                     caps the chain; frozen:mode = frozen | live
 │   └─ UsdGenInstance                   emits an instancer; primitive = cards | archives | spheres
 └─ UsdGenMap (abstract)                 ImageMap · PtexMap · ExprMap · PaintMap · NoiseMap · CombineMap · GuideProximityMap
API schemas: UsdGenMaskAPI (AUTO-APPLIED to UsdGenOperator via plugInfo AutoApplyAPISchemas;
             the tool also applies it explicitly until gate SI-8 proves auto-apply on codeless types),
             UsdGenLookAPI (on UsdGenDescription), UsdGenRestAPI (on bound surfaces),
             UsdGenCurveAPI (marker on C3 curves: primvars:usdGen:role = hair | guide)
```

Decisions folded in: one prim type per operator **concept** with a `usdGen:mode` token (artist;
mode switches are value edits, not prim delete/create resyncs that trip the `RemovePrim` bug);
`usdGen:enabled` (never `usdGen:active` — it collides with USD prim `active`, which S41 uses);
**explicit `usdGen:input` only** — the implicit preceding-sibling fallback is rejected (S26; and
`reorder nameChildren` invalidation is unmeasured). The stack editor's drag gesture rewrites
`usdGen:input` (one attribute edit per moved node), never `reorder nameChildren`. Namespace order is
only the Kahn tie-break.

### 2.2 Reserved layout (artist, normative)

```
<Groom>/<Description>/Ops/…          the operator chain (any names; order = usdGen:input)
<Groom>/<Description>/Guides/…       UsdGenGuideSet prims + their BasisCurves children
<Groom>/<Description>/Maps/…         UsdGenMap prims
<Groom>/<Description>/Prototypes/…   card/archive prototypes (pruned from the render)
<Groom>/<Description>/Frozen/…       freezes (siblings; the tool never re-authors this scope)
<Groom>/<Description>/__usdGenRender/  HYDRA-ONLY, never authored:
      tile_0000 … tile_NNNN            basisCurves
      guides/<setName>                 basisCurves, purpose = guide
      guides/<setName>/cvs             points (CV display for brushes)
      inst_<opName>[/Prototypes/<n>]   instancer + re-rooted prototypes
      material_storm                   only when USDGEN_STORM_MATERIAL_OVERRIDE=1
```

### 2.3 Properties that were contested — decided

| Property | Decision |
|---|---|
| Description terminal | `rel usdGen:terminal` (exactly one target) |
| Enable | `bool usdGen:enabled = true`; **non-structural** for topology-preserving operators (memcpy pass-through, no recompile, no recapture); structural for generators and `Resample` (the node's topology contribution vanishes) |
| Blend | `float usdGen:blend = 1` (envelope with exact endpoints) |
| Density controls | `float usdGen:densityScale = 1` (preview, always applied) on Groom/Description; `float usdGen:renderDensityScale = 1` applied only when the session context is `render`. Context is **explicit**: `USDGEN_CONTEXT=render` env (batch), `UsdGenImaging_SetContext()` (apps), or a usdview toggle. The renderer display name is NOT intent; `sceneGlobals` has no interactive flag (verified: `sceneGlobalsSchema.h:37-47`). Both scales decimate by stable id (`keep iff hash32(id) < scale·2^32`) so ids, sculpt deltas and clump ids survive. |
| Tile count | `uniform int usdGen:tileTarget = 64` on Description (clamped 32–256; authored because the prim set must be reproducible across sessions) |
| Machine tuning | `chunkSize`, `threadLimit`, `memoryBudgetMB` are **env/config** (`USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`, `USDGEN_MEMORY_BUDGET_MB`), never authored into an asset |
| Motion | `usdGen:motion:mode = single | velocities | samples`, `usdGen:motion:sampleCount = 3` (2..16), `usdGen:motion:forwardSurfaceSamples = false` on Description |
| Versioning | `uniform int usdGen:schemaVersion = 1` on Groom (refuse-and-warn on newer); `uniform int usdGen:algorithmVersion` per operator type (look preservation); frozenEpoch prefix `usdgen1:sha1:` |
| Session identity | `uniform string usdGen:sessionId` on Groom (stage-free registry key when the C API is absent) |
| Units | `usdGen:density` = hairs per square **stage unit on the rest surface** (state it in the schema doc); non-uniform surface scale does not change counts |
| Surface targets | `usdGen:surface` accepts `Mesh` prims and `GeomSubset`s. A subset restricts scatter to its faces; `skinprim` and Ptex face ids are always indices into the **parent mesh's** faces; a subset edit is a recapture. Instance-proxy targets translate via S7. |
| Clump centres | `rel usdGen:clump:centers` (a curve set, a nested scatter, or a map) as an explicit artist-visible input, plus `usdGen:clump:density` when absent |
| Region / parting | `rel usdGen:mask:region` on the mask block (per-face int/colour map) consumed by `GuideInterpolate` and `Clump` in v1; a parting-line *operator* (`UsdGenPart`, curve-set based: radius, strength) is v2, and its brush v2 |
| Guides | per-guide `float[] usdGen:blend` on `UsdGenGuideSet` (a host groomer range-of-influence) |
| Emitted primvars | `Clump` emits `clumpId_<level>` (uniform int); `GuideInterpolate` emits `guideIndex[3]`/`guideWeight[3]` (a host renderer arity) so bakes round-trip |
| Freeze | `UsdGenFreeze { rel frozen:curves; token frozen:mode = frozen|live; string frozen:epoch; token frozen:tier = session|sublayer|payload }` — the freeze **caps** the chain; upstream stays authored (greyed); unfreeze is one token |
| Sculpt | `UsdGenSculptLayer { weight; sculpt:curveIds; sculpt:cvOffsets; sculpt:deltas (root frame); sculpt:epoch; sculpt:lockedCurves }`; stale ⇒ badge + "Rebase sculpt" (re-match by nearest root UV, one undoable action, in the tool) |

Adapter coverage (mandatory correction from all judges): **one adapter class registered under five
plugInfo entries** — `UsdGenOperator` and `UsdGenMap` with `includeDerivedPrimTypes: true`, plus
`UsdGenGroom`, `UsdGenDescription`, `UsdGenGuideSet` — and an API-schema adapter for `UsdGenRestAPI`.
Mappings are built generically from `UsdPrimDefinition::GetPropertyNames()`. Without this the container
half of the schema is invisible to Hydra (measured).

---

## 3. Contracts frozen early (risk C1–C5, adopted verbatim)

| # | Contract | Frozen at |
|---|---|---|
| C1 | `usdGen:` property names and prim type names | end of M1 |
| C2 | the published tile `basisCurves` contract (§5.3) | end of M1 |
| C3 | the curve contract for guides, freezes, imports, sim caches (S42 + `primvars:usdGen:role`) | end of M2 |
| C4 | the C ABI + pxr_boost array surface | end of M5 |
| C5 | glslfx parameter names (the shipped file's `inputs:` block, verbatim) | end of M1 |

Everything else — `UsdGenOp`, chunk views, graph headers, imaging internals — is **not installed**
and may churn until M7. `UsdGenOpRegistry` is internal in v1/v2; a third-party operator ABI is v3.
The non-C++ extension tier in v2 is `UsdGenExprOp` (SeExpr, **capture-time only**).

---

## 4. Engine and evaluation (D2) — performance chapter adopted, with fixes

### 4.1 Adopted verbatim from `proposal-performance.md` §5
P1 planar SoA per-CV / AoS per-curve; P2 chunk = 512 curves curve-aligned; P3 the **reference lane**
(hair chunks never read hair chunks; guides, clump centres and card roots are un-chunked reference
buffers evaluated to completion first; consumers store `guideIdx[3]/guideW[3]`, `clumpId[level]` at
capture); P4 capture/evaluate split with a 128-bit capture epoch; P5 chunk/tile; P7 no cooking in
`GetPrim` or per `_PrimsDirtied`; P8 private `tbb::task_arena` at the measured 8-thread knee with
`USDGEN_THREAD_LIMIT` and a one-shot calibration; the three digests (Merkle structural / capture epoch /
value version); `UsdGenDirtyRouter` with typed `UsdGenDirtyBits` and a compile-time locator→node
table, surface-major chunk order so a surface dirty is a range; the four-hop dirty path
(locator → node → chunk → tile → leaf); the eviction score `recomputeCostMs/bytes` (never evict the
tail base or a live-override target); `UsdGenMotionCache` keyed by `(graphGen, surfaceGen, absTime)`
with lerp+clamp for non-retained times; the kernel authoring rules (`RESTRICT`, 257-entry ramp LUTs,
`-ffp-contract=off`, no intrinsics, deterministic reductions); `UsdGenStats`/`TfTrace` scopes named
after the gates.

### 4.2 Fixes applied to that chapter
1. **Merkle digest hashes `usdGen:input` ancestors**, not namespace children (the proposal's formula
   inverted the prefix-stability claim). `d(n) = H(type, algorithmVersion, mode, readPhase, space,
   sorted(input paths), sorted(reference/map/surface relationship targets), d(inputs...))`. `enabled`
   and `seed` are **not** in the structural digest (`enabled` is a value edit; `seed` bumps the capture
   epoch).
2. **Uniform-CV chunks are the fast path, ragged chunks are supported in v1** (`cvCount == 0` +
   `cvOffsets`), because `UsdGenCurveSource` (imports, sim caches) is v1 and ragged by nature.
   `UsdGenResample` is **v1** and the import tool offers "resample to N" with the measured penalty
   shown. Gate E-1r measures the ragged path.
3. `UsdGenGraphDesc` (risk) is the engine input: a pure-value description built by `usdGenImaging`
   from data sources, so `usdGen` core links **no `usd`, no `usdImaging`, no `hd`** (link-time
   enforcement of S8) and tier-0 tests need neither Hydra nor a stage.
4. Interruption: when a commit is superseded mid-run it returns the previous generation **and leaves
   the session dirty** so the next trigger re-runs; never half-publish.
5. Cross-chunk operators outside the reference lane (`Smooth(neighbours)`, `Collide`) are **v3** and get
   a two-pass gather/scatter node kind on a per-frame grid — never a chunk fan-in, and never "widen
   chunks" (4096-curve chunks are a measured 5× cliff).

### 4.3 Commit triggers (replaces S18's list)
| Trigger | When | Thread |
|---|---|---|
| (a) `UsdGenImaging_Commit()` / `SetTime()` | the usdview plugin, from `currentFrameChanged` (signal's frame) **and** from its own `Usd.Notice.ObjectsChanged` listener for `usdGen:` edits; batch drivers that wrap usdrecord | app thread |
| (b) `/ sceneGlobals/currentFrame` dirty | stock hosts (usdview without the plugin, usdrecord, hdPrman): measured last notice of every un-batched frame | notice thread |
| (c) operator-parameter / map / surface-topology dirty **with no app driver attached** | commit synchronously inside `_PrimsDirtied` (user-paced edits; exactly one cook per edit) | notice thread |
When (a) is attached, (b) and (c) only set the dirty flag. All notices are emitted from the commit path
on the thread that committed; `GetPrim` only `atomic_load`s. Gate SI-3 counts cooks.

### 4.4 Initial population (new decision)
The renderer-level index is constructed before the UsdImaging input is inserted into the engine's
merging index, and `HdMergingSceneIndex::InsertInputScenes` replays `PrimsAdded` — so in usdview/
usdrecord the groom roots arrive as notices. For hosts that hand us an already-populated input (the
legacy `HdRenderIndex::New` path), the index performs **one bounded traversal on its first
`GetChildPrimPaths`/`GetPrim` if no `PrimsAdded` has been seen** (`_populated == false`), scanning only
for `UsdGenGroom` roots. Gate SI-6 asserts both paths populate identically.

### 4.5 Sessions across chains (new decision)
`UsdGenImagingRegistry` keys sessions by (weak stage | `usdGen:sessionId`, groom root). Several scene
index instances (renderer switch, Storm + an hdPrman preflight in one process) **attach to one
session**; the session owns one current frame, one context (interactive/render), one generation; each
attached index republishes the same generation. The prim set is a session property and never differs
per index; a renderer switch re-attaches and continues the generation counter.

---

## 5. Imaging and Storm (D3) — performance §6 adopted, with the following

### 5.1 Registrations (exactly four)
`UsdGenGroomSceneIndexPlugin : HdSceneIndexPlugin` (S1: `loadWithRenderer ""`, phase 0 AtEnd, tags,
`_IsEnabled` env kill-switch `USDGEN_ENABLE`); `UsdGenMetadataSceneIndexPlugin : UsdImagingSceneIndexPlugin`
(returns its input; `InstanceDataSourceNames()`/`ProxyPathTranslationDataSourceNames()` — **non-const**,
as the base declares them); the prim adapter (five entries, §2.3) + `UsdGenRestAPI` adapter; the usdview
`PluginContainer`. Both the plugInfo JSON entry and the C++ `RegisterSceneIndexForRenderer` call ship.

### 5.2 Invalidation discipline (S5 restored, S30 kept)
Bare `primvars/points/primvarValue` + `extent/*` per dirty tile where usdGen owns the prim;
`ComputeDirtyLocators` **and the bare `primvars` locator** where usdGen overlays an upstream prim
(frozen curves deformed in place) — the latter is the measured UsdSkel-freeze rule (S5). Never co-dirty
`displayColor` with `points`. A primvar *appearing* is dirtied as `primvars/<name>` once.

### 5.3 Tile contract (C2)
As performance §6.3, with: `hairId` uniform **float**; `purpose`, `visibility`, `materialBindings`
authored by hand (post-flattening); `primOrigin` absolute outside prototypes and relative inside;
`__dependencies` one edge per tile; `velocities`/`accelerations` blocked except in profile P1;
`displayStyle/refineLevel = 2`; `minScreenSpaceWidths = 1.0`; `extent` every deforming frame.

### 5.4 `hairTangent` — decided as a measured fork (gate S-8, milestone M1)
Two shipped glslfx variants, one primvar policy:
* **Variant A (default on usdGen tiles):** tangent from Storm's own `inData.Neye` (the patch layout's
  tangent for ROUND/HALFTUBE). Legal on usdGen tiles because the SI **pins refineLevel 2, always
  publishes `widths`, and binds the shader only to its own tiles**. No `hairTangent` primvar is
  published, so deforming frames stay on Storm's `DirtyPoints` fastpath (no non-points primvar changes
  per frame; `hairT`, `st`, `hairId`, `displayColor` are static).
* **Variant B (`usdGenHairPreviewPrimvar.glslfx`, for curves usdGen does not own or draws in wire/
  mesh contexts):** tangent from a `hairTangent` vertex primvar with the screen-derivative fallback —
  the variant the prototype rendered.
* Gate S-8 (EGL harness, M1) measures: A vs B frame time on a 100 k deforming groom, and confirms A
  compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` (the Metal/Vulkan proxy). If A fails either
  check, B becomes the default and `hairTangent` is republished per deforming frame at the measured
  cost (≈ +1.5–2.5 ms at 100 k × 8 CV; the DirtyPrimvar path re-uploads every non-points primvar).

### 5.5 refineLevel (gate S-9, M1)
Tiles pin refineLevel 2. Gate S-9 measures level 1 vs 2 frame time **and the switch cost** (cubic index
rebuild 2.6/5.2 ms per 500 k patches + batch revalidation). If a switch costs < 3 ms, a "tumble tier"
(refineLevel 1 while the camera moves, restored on release) ships as an option; LOD by decimation
remains the primary lever either way.

### 5.6 Storm material binding (gate L-1, M1)
Three terminals on one `Material` (S36). `USDGEN_STORM_MATERIAL_OVERRIDE` default **OFF**; L-1 checks
that Storm resolves `outputs:glslfx:surface` first. If it does not, flip the default and synthesize
`<Description>/__usdGenRender/material_storm` (with `primOrigin`) and bind tiles to it.

---

## 6. Operators (D4), look (D5), tools (D6), build (D8)

* **Catalogue v1** = risk §6.1 parameter lists (Clump with the full a host groomer set incl. copy/copyVariance/
  cut/flatness/offset/curl/crossover; `GuideInterpolate` with `regionMap` + `clumpCrossover`) **plus**
  `Direction/Lift` (a host groomer Tilt), `Smooth` (along-curve), `Resample`, `Scale` moved into v1; artist's
  §6.4 operator rules (mask block on every op; `hash(seed, curveId, saltPerOperator)`; `preserveLength`;
  `Space()` declared; topology-bumping list). **v2** = Curl, Bend, Straighten, Displace, Wave, Part,
  Instance (cards/archives/spheres), PtexMap, ExprOp, TsSpline ramps, sculpt rebase, P1 motion.
  **v3** = Collide/Shrinkwrap, Wind/Force, Smooth(neighbours), Braid, SimSource, OpenExec backend,
  third-party operator ABI, GPU tail.
* **Look** = risk §7 (`UsdGenLookAPI` with the explicit bake order, `USDGEN_STORM_MATERIAL_OVERRIDE`,
  `rand()` added to SeExpr, one `PtexFilter` per worker) + artist §7.1 (the `inputs:` block verbatim
  from the shipped glslfx) + performance §9.1 (the MaterialX chain with an explicit world-space
  tangent geomprop). SeExpr `noise` keeps 0..1 semantics; `snoise` documented.
* **Tools** = risk §8 architecture (Qt-free module split; `UsdGenToolState` dataclass built in
  `__init__`; `usdGenLib.py` pins BLAS threads before numpy; two Python surfaces; `SubtreeSnapshot`;
  `UsdGenImaging_ClosestSurfacePoint`) + artist §8 UX (ten-brush shelf with named commit targets; four-
  phase press/move/release/escape; progressive-feedback trio; status line with the edit target;
  hotkeys; stack profiler column from `UsdGenNodeStats`; freeze bar). Parameter panels are generated
  from `UsdPrimDefinition::GetPropertyNames()` (performance). Add a **mask visualisation mode** and a
  "show driving guides" overlay (both read data the design already computes). Symmetry (mirror-X) is
  an M5 requirement, not a late add. CV display = synthesized `points` child prim.
* **Build** = risk §2 targets (`usdGenMath` STATIC `-ffp-contract=off`; `usdGen_seexpr`, `usdGen_ptex`
  static+hidden; `usdGen` SHARED linking **no usd/usdImaging/hd** (+`hio`, `pxOsd`); `usdGenImaging`
  SHARED; `usdGenSchema`; `usdGenShaders`; `_usdGen` pxr_boost MODULE built without LTO; `usdGenUsdview`;
  `usdGenTestUtils` with `eglctx.h`; a `usdGenPy` facade package mirroring `rigexec.Builder`) +
  performance's rebuild-cost rationale + the CMake specifics (double-`pxrConfig` guard, generated
  plugInfo pattern, `FETCHCONTENT_SOURCE_DIR_*` offline fallback, never install third-party `.so`s).

---

## 7. Roadmap (D7), gates and estimates

Milestones are named **M0–M8** (never "S", which collides with the brief's S-numbers):

| M | Name | Freezes | Calendar (3 eng.) |
|---|---|---|---|
| M0 | Skeleton + chain-order proof + test tiers T0–T4 + pre-work benchmarks | — | 2 wk |
| M1 | Straight hair end to end (Scatter → Grow → Noise → Length → Width → tiles → glslfx) | C1, C2, C5 | 5 wk |
| M2 | Deform + freeze (`Deform`, `CurveSource`, `Freeze`, `SculptLayer`, rest adapter, pruning wrapper, T1/T2 tiers, `SubtreeSnapshot`) | C3 | 4 wk |
| M3 | Guides + clumps (reference lane, kd-tree capture, `GuideInterpolate`, multi-level `Clump`, `Smooth`, `Resample`, `Direction`) | — | 5 wk |
| M4 | Maps + expressions (Image/Ptex/Expr/Paint maps, SeExpr set, `LookAPI` bake, reload) | — | 4 wk |
| M5 | Tools (C ABI, `_usdGen`, brushes, freeze/commit flows, undo, panels, symmetry) | C4 | 5 wk |
| M6 | Instancing (cards/archives/spheres, native-instance grooms, S7 hooks) | — | 3 wk |
| M7 | Render time + hardening (P1/P2 motion, hdPrman parity, memory budget, installed headers, workstation protocol) | — | 5 wk |
| M8 | Breadth (v2 catalogue, progressive generation, docs) | — | continuous |

Totals (delivery judge's re-baseline, 3 engineers, prototypes carried in, 30 % contingency on M1/M3/M7
already applied): **M0–M2 ≈ 11–12 weeks** (a deforming, freezable groom); **M0–M5 ≈ 25 weeks** (an artist
can groom); **M0–M7 ≈ 34 weeks** (renderable on a show). Staffing is an ASSUMPTION.

Gates = performance §11.2 (E-1..E-8, SI-1..SI-5, S-1..S-7, T-1..T-5, R-1..R-3) mapped onto milestone
exits, **plus**: E-1r (ragged path ≤ 2× uniform), E-4 promoted to M0 pre-work (nanoflann kNN over
100 k / 1 M rest roots at 8 threads — the number the operator schedule assumes), SI-6 (initial
population, both paths), SI-7 (adapter coverage: every `usdGen:*` property of every registered type
appears and dirties), SI-8 (auto-applied `UsdGenMaskAPI` on a codeless type), S-8 (`hairTangent`
strategy), S-9 (refineLevel switch cost), S-1 re-measured at 100 k (the ~12 ms figure is interpolated),
L-1 (render-context resolution), T-INST-1/2 (instancer pick round-trip, prototype rebasing). Tier-4
(workstation) gates are **release** criteria, never milestone exits. M1's stop condition stands: if
precise `usdGen:*` invalidation cannot be demonstrated, fall back to `primvars:usdGen:*` and re-plan.

Pre-work in M0 (each an afternoon on this host): E-4 kNN benchmark; S-8/S-9/L-1 via the EGL harness;
SI-8 auto-apply check; `reorder nameChildren` notice check (to close the question, not because anything
depends on it).

---

## 8. Document map for the writers (source chapters per plan document)

| Plan document | Primary source | Graft from | ADR sections |
|---|---|---|---|
| `00-request-and-scope.md` | brief §0–1; risk §0–1; artist §0–1 | — | all of §1 |
| `01-architecture.md` | risk §0, §2 (thesis, contracts); performance §0 (frame ledger, P1–P8) | artist §2 (the five nouns) | §1, §3, §4.3–4.5 |
| `02-schema.md` | artist §3 (layout, examples, MaskAPI, maps, freeze/sculpt) | risk §3 (hierarchy, C3, versioning, colour ramps); performance §4.2 (RestAPI, digests) | §2 |
| `03-execution-engine.md` | performance §5 | risk §4.3 (`UsdGenGraphDesc`, Topology/ValueParameters); artist §4.6 (`enabled` non-structural), §4.10 (`UsdGenNodeStats`) | §4 |
| `04-operators.md` | risk §6 (parameter lists, emitted primvars); artist §6 (parity column, §6.4 rules) | performance §7 (capture-cache column, mask arithmetic) | §6 |
| `05-static-curves-and-deformation.md` | performance §8; risk §3.7–3.8, §3.11 | artist §3.8 | §2.3 (freeze/sculpt), §4.2.2 |
| `06-imaging.md` | performance §6 | risk §5 (registrations, `_IsEnabled`, hard diagnostics); artist §5.7 (two extent channels) | §4.3–4.5, §5 |
| `07-look-maps-expressions.md` | risk §7; artist §7 | performance §9 | §5.4–5.6, §6 |
| `08-tools.md` | artist §8 | risk §8 (architecture, landmines); performance §10.1–10.3 | §6 |
| `09-performance-and-benchmarks.md` | performance §0.2, §11 | G-storm-throughput §3 protocol; G-storm-hair-look §5–6 | §5.4–5.5, §7 gates |
| `10-build-dependencies-testing.md` | risk §2, §9.1 | performance §3; A8 §6; B-usdrig-build | §6 build |
| `11-roadmap.md` | risk §1, §9 | performance §12 (carried prototypes, gate mapping) | §7 |
| `12-risks-decisions-open-questions.md` | risk §10, §11; performance §13; all three judges' unresolved lists | — | §1, §7 |
| `appendix-A-evidence-ledger.md` | ENVIRONMENT.md; every MEASURED number with its report §; file:line index | — | — |
| `appendix-B-prototype-inventory.md` | performance appendix; `plan/prototypes/` tree | — | — |
| `README.md` | this ADR §0–§1 as the TL;DR | — | — |

Every document: cite S-numbers and report sections; mark MEASURED / UNMEASURED / ASSUMPTION; use the
owner's `docs/superpowers` conventions (numbered sections, facts with file:line, exact names/
signatures, testing, out of scope); never restate a judge's rejected idea (implicit sibling wiring,
`usdGen:active`, structural `enabled`, `sceneGlobals` interactive flag, "widen chunks", padding,
`RemovePrim` in interactive paths, `.usda` bakes, baked motion samples).

---

## 9. Addendum (2026-09-05) — rulings after the first writing pass

The fifteen writers and their reviewers surfaced questions this ADR had left ambiguous or answered
inconsistently. The rulings below (R1–R45) are binding; where an earlier section of this ADR
disagrees, the ruling wins. Every plan document must conform; the cross-document consistency pass
and the per-file reconcilers check conformance to this section explicitly.

### 9.1 Vocabulary (collisions removed)

| # | Ruling |
|---|---|
| R1 | Engine principles P1–P8 (§4.1) are renamed **I1–I8** ("invariants"). Motion profiles keep S32's **P0/P1/P2**. M0 pre-work items are **PW-1…PW-n** (never `P-n`). Appendix A measurement rows are **EV-001…** and are the only citation handle for a measured number (no `Q1…Q41`, no `C1…/M1…/S1…` row ids in the ledger). Risk ids `RK-nn`, rejected alternatives `X-nn`, open questions `Q-nn`, assumptions `A-nn` stay as `12-…` defined them. Freeze landing tiers are the tokens `session|sublayer|payload` only; S42's "T1/T2/T3 landing tiers" are retired from prose. |
| R2 | Test tiers **T0** engine-only (no Hydra, no stage), **T1** headless scene index, **T2** Storm on the EGL harness, **T3** `testusdview` tool tests, **T4** workstation (release criteria only). Gate families: `E-`, `SI-`, `S-`, `L-`, `T-`, `T-INST-`, `R-`, and new **`B-`** (build checks; `B-1` = the link-rule check). Gate ids are always hyphenated. |
| R3 | Class names: `UsdGenGroomSceneIndexPlugin` / `UsdGenGroomSceneIndex`; `UsdGenSession` (never `UsdGenImagingSession`); `UsdGenImagingRegistry`; `UsdGenTileBuilder` (never `ChunkPrimBuilder`); adapters = `UsdGenPrimAdapterBase` plus the five one-line subclasses `UsdGenOperatorAdapter`, `UsdGenMapAdapter`, `UsdGenGroomAdapter`, `UsdGenDescriptionAdapter`, `UsdGenGuideSetAdapter` (the adapter registry maps one TfType per `primTypeName`, `usdImaging/adapterRegistry.cpp:104-131`) and `UsdGenRestAPIAdapter`. Dirty bits are the unscoped enum `03-execution-engine.md` defines (`UsdGenDirtyNone`, `UsdGenDirtyParameter`, `UsdGenDirtyCapture`, `UsdGenDirtyTopology`, …); no document may write `UsdGenDirtyBits::Value`. |
| R4 | Shader-def identifiers are prim names, so `:` is illegal: **`UsdGenHairPreview`**, **`UsdGenHairPreviewTranslucent`**, **`UsdGenHairPreviewPrimvar`**. `usdGen:HairPreview` may appear only as a display name. `usdGenShaders` ships **three** glslfx files (default, translucent, primvar-tangent variant B); the `inputs:` block (C5) is identical in all three. |
| R5 | The Python package is **`usdgen`** (all lowercase, mirroring `rigexec`), containing `_usdGen` (pxr_boost module), `usdGenLib.py` (ctypes surface), the kernels, commands and UI modules named in `08-tools.md`. `usdGenPy` is not a name. |

### 9.2 Schema

| # | Ruling |
|---|---|
| R6 | Every schema property is `usdGen:`-namespaced. §2.3's shorthand expands to `usdGen:frozen:curves|mode|epoch|tier` and `usdGen:sculpt:weight|space|curveIds|cvOffsets|deltas|epoch|lockedCurves|rootPrims|rootUVs`. The adapter builds mappings from `UsdPrimDefinition::GetPropertyNames()` with **no prefix filter**; SI-7 asserts every declared property of every registered type. |
| R7 | **`02-schema.md` is the single normative property registry** (names, types, defaults, allowed tokens, dirty class). Every other document conforms to it. Where a sibling needs a property 02 lacks, 02 gains it (R8); never the reverse. 02 §2.7's operator parameter rows carry type/default/uniformity at the same depth as `04-operators.md`; 04's tables may not disagree with 02. |
| R8 | Fold-ins to 02: `UsdGenCurveSource` gains `usdGen:lane (token hair|reference, hair)`, `usdGen:staleAction (token warn|ignore|block, warn)`, `usdGen:expectEpoch (string)`, `usdGen:resampleTo (int, 0 = keep)`, `usdGen:rebind (token never|onError|always, onError)`. `UsdGenSculptLayer` gains `usdGen:sculpt:rootPrims (int[])`, `usdGen:sculpt:rootUVs (texCoord2f[])`. `UsdGenLookAPI` gains `usdGen:look:bakeMode (token perCurve|perCV|none, perCurve)` and `usdGen:look:colorMapMode (token bake|shader, bake)`, keeps 02's `bakeTarget`, `jitterSeed`, `bakePrimvar`, and names the scalar exponent `usdGen:look:rampExponent (float, 1.0)` (never `colorRamp`, which is the array pair). **Dropped everywhere:** `usdGen:cacheOutput`, `usdGen:output (tiles|inPlace)`, `usdGen:interactive:maxCurves` (session-only via the C ABI), `usdGen:cache*`, `usdGen:diagnostics`, `usdGen:curve:cvCount`, `usdGen:primitive` on `UsdGenDescription`. |
| R9 | `usdGen:space` tokens `auto | rest | deformed`, default `auto` (= the operator type's declared default space); engine enum `UsdGenSpace { Inherit, Rest, Deformed }` (`Inherit` ⇔ `auto`). There is no `world` token (post-flattening, deformed space is world space, S4). `usdGen:readPhase` default **`final`**; `preceding` is accepted as an alias in v1. |
| R10 | `UsdGenClump` carries **no `usdGen:mode`**; its blend method is `usdGen:clump:method (token linearBlend|extrudeAndBlend, linearBlend)`. `clumpId_<n>` numbering uses `uniform int usdGen:clump:level` (default `-1` = auto: the node's ordinal among **all** Clump nodes of the compiled chain, disabled ones included, so enable/disable never renumbers a published primvar). |
| R11 | Ramps (restates §1 S11 without the contradiction): scalar ramp = `float2[] <p>:knots` + `token <p>:interpolation (linear|catmullRom|bspline|constant, catmullRom)`; colour ramp = `float[] <p>:positions` + `color3f[] <p>:colors` + `<p>:interpolation`; the whole-`TsSpline` transport is `float <p>:spline` authored **with** `.spline`. Rule: `.spline` is legal only on `<p>:spline` and on plain animated scalars; on `:knots`, `:positions` or `:colors` it is a compile error. |
| R12 | `curveId` is **64-bit**: `uint64[] primvars:usdGen:curveId`. Ids come from `UsdGenHash64(seed, faceIndex, k, kSaltScatter)`. The hash is pinned: SplitMix64 finalizer over `key ^ (uint64(salt) * 0x9E3779B97F4A7C15)` — `z = x + 0x9E3779B97F4A7C15; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9; z = (z ^ (z >> 27)) * 0x94D049BB133111EB; z ^ (z >> 31)`; `UsdGenHash32` = the high 32 bits. Salts are per-use compile-time constants (`kSaltScatter`, `kSaltDensity`, `kSaltNoise`, …); **`hairId = UsdGenHash32(curveId, 0) / 2^32`** and **decimation uses `kSaltDensity ≠ 0`**, so the surviving set is never `{hairId < scale}`. Changing the hash or any salt bumps `usdGen:schemaVersion`. |
| R13 | Density: keep iff `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32`, with `keepFraction = clamp(scale_groom · scale_description, 0, 1)` where `scale` is `usdGen:densityScale` in the **interactive** context and `usdGen:renderDensityScale` in the **render** context (the contexts are exclusive; neither scale applies in the other; both default 1). `usdGen:density` (hairs per square stage unit on the rest surface) defines the id space. Scales never re-scatter and never re-chunk: the chunk partition is computed once per capture over the full id set and `UsdGenChunkDesc::liveCount` tracks survivors. Values above 1 clamp with one `TF_WARN`. With hash-threshold decimation the preview set is automatically a subset of any larger scale's set. |
| R14 | `usdGen:enabled` is never a digest term. Router: topology-preserving operators → `UsdGenDirtyParameter` (input aliasing, no recapture); generators, `UsdGenResample`, and `UsdGenLength` (whose **static** `TopologyEffect()` is `MayChangeCurveCount` because cull mode exists) → `UsdGenDirtyTopology` (recapture downstream + one topology publish; **no recompile**). A muted generator keeps its capture, so ids survive mute/unmute. |
| R15 | `GeomSubset`: Hydra exposes subsets as prims of type `geomSubset` with `indices` and `type` (`typeFaceSet`); `familyName` never reaches Hydra and is not load-bearing. `usdGen:surface` → (parent mesh, face-index set); a dirty on the subset prim's `geomSubset/indices` is `UsdGenDirtySurfaceTopo` (recapture). Because ids hash the **parent mesh's** face index, a subset edit preserves ids on faces that remain. `UsdGenRestAPI` may be applied only to the parent `Mesh`. |
| R16 | `UsdGenMaskAPI`: 02's block is canonical for names, tokens and the evaluation arithmetic; `04-operators.md` §5 and `08-tools.md` conform to it (`usdGen:mask:source` is the map relationship; there is no `usdGen:mask:map`). 04 §5.2's pseudocode must be executable and identical in arithmetic to 02's. |
| R17 | `usdGen:algorithmVersion` schema fallback is **0 = "latest kernel"**. The tool, freezes and bakes always author the explicit current version, so look preservation is guaranteed for every tool-authored asset; a hand-written asset that omits it tracks the newest kernel (documented). |
| R18 | Stale `UsdGenFreeze` epoch: **warn and keep rendering the frozen data** (risk §3.7); never silently re-cook a chain the artist froze. `usdGen:staleAction` exists only on `UsdGenCurveSource`. `usdGen:frozen:tier` is advisory (tool-facing); the evaluator never reads it. |
| R19 | `UsdGenDescription : UsdGeomBoundable` needs a registered extent function, and a resource-only plugin cannot register one. Ruling: the `usdGenSchema` plugin's plugInfo has `LibraryPath` → **`libusdGenSchema.so`**, a minimal library (schema tokens + `UsdGeomRegisterComputeExtentFunction` for `UsdGenDescription`, linking `usd`/`usdGeom` only) with `implementsComputeExtent: true` on the type. The schema classes remain codeless (no generated C++ classes). This keeps extents working in `usdcat`/`usdrecord` and keeps `usdGen` core free of `usd`. |

### 9.3 Engine

| # | Ruling |
|---|---|
| R20 | Generation handoff: `std::shared_ptr<const UsdGenGeneration>` published with `std::atomic_store` / read with `std::atomic_load` (the idiom the zero-torn-reads probe measured; `TfRefPtr` has no atomic load). Buffer reuse (optional publish ring, M7): every published `VtArray` wraps a `UsdGenGenerationBuffer : Vt_ArrayForeignDataSource` (`pxr/base/vt/array.h:39-51`) whose detached callback returns the buffer to the session pool; a buffer is reusable only after that callback has fired. **`VtArray::IsUnique()` does not exist and must not appear anywhere** (this supersedes §1 S24's wording). |
| R21 | Tile arithmetic: `chunksPerTile = max(1, ceil(nChunks / tileTarget))`; **`nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))`**. Worked numbers: 100 k curves, chunk 512, `tileTarget` 64 → 196 chunks, 4 chunks per tile, **49 tiles**; 64 tiles is the 1 M-curve figure. `usdGen:tileTarget` is not a node-digest term; a change routes as `UsdGenDirtyTopology` on the description (tile prims added/removed, PrimsAdded/Removed). |
| R22 | Commit order: (1) route dirties; (2) recompile iff the structural digest changed; (3) evaluate the **reference lane** to completion (guides, clump centres, card roots, in dependency order); (4) capture nodes whose capture epoch changed — they read the already-evaluated reference buffers; (5) evaluate hair chunks in the private arena; (6) assemble tiles and extents; (7) publish and emit notices. `Capture()` may allocate and take the graph mutex; it runs on the commit thread or under `WorkParallelForN`, outside the arena's no-allocation rule, which applies to `Evaluate()` kernels only. |
| R23 | `UsdGenGraphDesc` carries curve inputs: `std::vector<UsdGenCurveSetDesc> curveSets` (points, vertex counts, C3 primvars, role, epoch), referenced by node index; guides, frozen and imported curves reach the engine only through it. `UsdGenSurfaceDesc` carries `std::vector<UsdGenSurfaceSample> samples` (time, points) so motion samples have a source; the current-time sample is index 0. |
| R24 | Per-curve extra planes exist: `UsdGenCurveBuffer::extraCurve` (typed `int32` / `float` / `int32×3` / `float×3`) alongside `extraCv`; `UsdGenChunkView` exposes both; tile assembly publishes every plane a node declares in `OutputPrimvars()` (`clumpId_<n>` uniform `int`; `guideIndex` uniform `int[]` with `elementSize = 3`; `guideWeight` uniform `float[]` with `elementSize = 3`). `int[3]`/`float[3]` are not USD type names. |
| R25 | The dirty-router key is the **full locator path** below `usdGen/` (never "first two elements"); the table is generated per property from 02 §6. |
| R26 | Reference-lane sizing assumption: guides are **1–10 %** of hairs (A7 §9.3); design and budget for 10 %. |
| R27 | Baselines: every engine budget and gate quotes the **8-thread private-arena** number, with the 20-thread number in parentheses. E-1 = the 5-op chain at 100 k × 8 CV in the arena **≤ 1.5 ms** (MEASURED 1.02 ms). Scaled or interpolated figures are tagged `DERIVED from EV-nnn`, never MEASURED. |
| R28 | Sessions across indices: the session's current time is whichever `SetTime`/`currentFrame` arrived last from any attached index; attaching a render-context index does not change time; two attached indices at different times are unsupported in v1 (documented; an hdPrman preflight runs in its own process). SI-6's traversal population is bounded at **≤ 5 ms on the 11 005-prim stage** (ASSUMPTION until measured). |

### 9.4 Imaging, look and tools

| # | Ruling |
|---|---|
| R29 | C3 source prims (guides, freezes, imports) are overlaid `visibility = false` by the scene index and the description always publishes tiles. The in-place overlay path (`usdGen:output`) **does not exist in v1**; it is a v2 optimisation behind new gate **S-10** (T2: PrimsRemoved/Added versus in-place points dirty on a frozen prim). S5's bare-`primvars` rule applies to every overlay the index makes on an upstream prim (visibility, CV-display children, any primvar it adds). |
| R30 | Session keys: the weak stage first; `usdGen:sessionId` only when no stage is reachable, namespaced by `renderInstanceId` + root-layer identifier so two stages authoring the same id never share a session. |
| R31 | **`08-tools.md` §1.4 is the single source of the C ABI (contract C4)**; `06-imaging.md` repeats it verbatim. Minimum entry points: `Activate`, `Deactivate`, `SetTime`, `Commit`, `SetContext(const char *context)`, `GetGeneration`, **`GetTopologyGeneration`** (bumps only on a topology change), `BeginLiveOverride`, `SetLiveOverrideIndexed`, `ClearLiveOverride`, `PickCV`, `ClosestSurfacePoint`, `Footprint`, `SetInteractiveLOD(descPath, maxCurves)`, `BuildMirrorMap(descPath, axis)`, `SetMaskVisualisation(opPath)` (empty string clears), `GetStatsJson`, `ReloadMaps`. All are `extern "C"`, return `int` status, and never throw across the boundary. |
| R32 | Commit triggers (amends §4.3): trigger **(c) always applies** — an operator/map/surface-topology dirty commits synchronously at the end of the `_PrimsDirtied` batch that carried it, with or without an app driver (exactly one cook per edit batch, SI-3). The app therefore never calls `Commit()` for a stage edit; a tool's release sequence is: close the live override without republishing → author the edit in one `Sdf.ChangeBlock` → request a repaint → Hydra's `ApplyPendingUpdates` delivers the dirty → the index cooks and publishes. Trigger (a) is for `SetTime` and for explicit `Commit()` after live-override changes; trigger (b) is `currentFrame` when no (a) is attached. No `Usd.Notice` listener ordering rule exists in the design. |
| R33 | `hairTangentWorld` is rejected: the render MaterialX network transforms the object-space `hairTangent` (or derives it from positions) with a node; tiles publish no context-dependent primvars. |
| R34 | Storm material bindings are authored on **every tile** (post-flattening, nothing is inherited). Under hdPrman, usdGen (all-renderers, phase 0, AtEnd) runs **before** hdPrman's renderer-specific phase-0 entries (all-renderers entries sort first within a phase), so the private pruning wrapper is load-bearing under both renderers. |
| R35 | `10-build-dependencies-testing.md` owns the **single env-var registry**: `USDGEN_ENABLE`, `USDGEN_CONTEXT`, `USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`, `USDGEN_MEMORY_BUDGET_MB`, `USDGEN_STORM_MATERIAL_OVERRIDE`, `USDGEN_DIAGNOSTICS`, `USDGEN_IMAGE_CACHE_MB`, `USDGEN_PTEX_CACHE_MB`, `USDGEN_PTEX_MAX_FILES`, plus the `TfDebug` codes. Any other document introducing an env var must add it there. |
| R36 | Hotkeys: freeze is **Shift+F** (usdview binds F to Frame Selected), unfreeze Alt+F. The interactive ceiling is session-only (`SetInteractiveLOD`), decimating by the R13 hash rule. |
| R37 | Dependency rule (`usdGen` core): links `tf gf vt sdf ar work trace hio pxOsd` plus what `sdf` pulls transitively (`ts`, `python`, `pegtl`); **forbidden** `usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`, `usdAppUtils`; gate **B-1** (T0, M0) checks `DT_NEEDED` of `libusdGen.so` against the forbidden list. |

### 9.5 Versions, roadmap, gates, numbers

| # | Ruling |
|---|---|
| R38 | **v1 = the M0–M7 deliverable set** (amends §6): it includes `UsdGenScatter` modes `random` (M1), `atGuides` (M3), `points` (M5); `UsdGenPtexMap` (M4); `UsdGenInstance` cards/archives/spheres (M6); motion profiles P0/P1/P2 (M7). **v2 = M8 breadth**: Curl, Bend, Straighten, Displace, Wave, Part, ExprOp, TsSpline ramps in the UI, sculpt rebase, `Scatter` mode `uniform`, progressive generation (T-5), in-place overlay (S-10). v3 unchanged. |
| R39 | Milestones are **serial** (each is a vertical slice needing all three lanes); lanes parallelise inside a milestone. Calendar = 33 weeks of milestones + 1 week integration float at the C4 freeze = **34 weeks** (ASSUMPTION, 3 engineers). No T4 gate is a milestone exit; T4 gates live in the release criteria `RC-n`. T-5 is a v2 criterion, not a v1 release gate. "`usdrecord` on both renderers" is a release criterion, not an M7 exit. |
| R40 | **`09-performance-and-benchmarks.md` §5 is the single gate registry** (id → metric, pass criterion, tier, milestone, status); `00`, `10`, `11`, `12` and appendix A cite it and must agree. Conflicting assignments are fixed as: E-1 T0 M1 · E-1r T0 M2 · E-2 T0 M1 · E-3 T0 M2 · E-4 T0 M0 pre-work, binding at M3 · E-5 T0 M3, re-run M7 · E-6 T0 M1 · E-7 T0 M1 · E-8 T0 M1, re-run M4 · SI-1…SI-5 T1 M1 · SI-6 T1 M1 · SI-7 T1 M1 · SI-8 T1 M0 pre-work, binding at M1 · **SI-9** (pruning-wrapper cost on a production-density skinned scalp) T1 M2 · S-1 (re-measured at 100 k, 1280×720 and 1920×1080) T2 M1 · S-2 T2 M2 · S-3 T2 M2 · S-4 T2 M2 · S-5 T2 M1 · S-6 T2 M1 · S-7 T2 M5 · S-8 T2 M0 pre-work, binding at M1 · S-9 T2 M0 pre-work, binding at M1 · S-10 T2 M8 · L-1 T2 M0 pre-work, binding at M1 · L-2 T2 M1 · L-3/L-4/L-5 T0–T1 M4 · T-1 T3 M5 · T-2 T1 M5 · T-3 T3 M5 · T-4 T3 M5 (freeze *authoring* ≤ 5 ms; `_resetGUI` excluded) · T-5 T3 M8 · T-INST-1/2 T1 M6 · R-1/R-2/R-3 T4 release · B-1 T0 M0. Where 09 already places a gate not named here, 09 stands. |
| R41 | **09 §0.2 is the only frame ledger**; `01-architecture.md` §0.3 and appendix A cite it rather than restate it. The ledger states: rig 1.35 ms (MEASURED), usdGen deform tail (DERIVED), Storm draw at 100 k × 8 CV ≈ 12 ms (**INTERPOLATED / UNMEASURED, gate S-1**), points upload at 100 k ≈ 1.4 ms (DERIVED from the measured deltas), sum ≈ 16 ms; it is explicitly conditional on S-8 variant A. Conclusion: 60 Hz at 100 k × 8 CV / refineLevel 2 is **not established**. Interactive targets (ASSUMPTION until S-1): 60 Hz at ≤ 40 k curves, ≥ 30 Hz at 100 k, ≥ 10 Hz at 200 k; `usdGen:densityScale` is the artist's lever. |
| R42 | Number tags: MEASURED (with `EV-nnn`), `DERIVED from EV-nnn` (scaled/interpolated; never MEASURED), UNMEASURED (with the gate), ASSUMPTION. Every number carries one. The interpolated 100 k Storm figure, the 10–150 ms capture-per-gesture estimate, the ~40 µs recompile, the 0.05 ms per-tile extent, and the +1.5–2.5 ms `hairTangent` republish are all DERIVED or ASSUMPTION. |
| R43 | Appendix A: rows `EV-001…`; adds the stage-free measurement (schema-declared `usdGen:*` attributes are absent without an adapter, G-stage-free §1); the motion-blur floors from `mbbench.cpp` are listed with the note "prototype not carried into `plan/prototypes/` unless `prototypes/motion-blur/` exists; re-create under PW-n in M7"; `hdSt/renderDelegate.cpp:695-707` is the accepted range; `engine.cpp:148-201` for the scene-globals callback. |
| R44 | `00-request-and-scope.md`'s gate-family list and glossary include the `L-`, `B-`, `SI-6…SI-9`, `S-10` additions and the vocabulary of R1–R2; its version table matches R38. |
| R45 | Cross-references: "see `NN-…md` §X" must point at a section that exists in the current sibling; the reconcilers re-check every cross-reference they touch. |
