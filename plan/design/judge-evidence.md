# Judge report — evidence and correctness lens

Judge: **correctness against the settled constraints S1–S46 and the measured evidence.**
Scope: `design/proposal-artist.md`, `design/proposal-performance.md`, `design/proposal-risk.md`,
scored per open area D1–D8. Every API name asserted below was re-verified by grep in
`<openusd-src>` (tag v26.08, `ee47c679a`); every number was re-checked against the
report it is quoted from. Findings are stated as file:line or `report §` so they can be audited.

---

## 0. Verification pass — what I re-checked in OpenUSD 26.08

All three proposals lean on the same set of API names. I checked every one that carries weight;
**none of the three invents an API that does not exist**. The checks, for the record:

| Name used by the proposals | Verified at |
|---|---|
| `UsdImagingSceneIndexPrimAdapter` | `pxr/usdImaging/usdImaging/sceneIndexPrimAdapter.h:27` |
| `includeDerivedPrimTypes` plugInfo key | `pxr/usdImaging/usdImaging/adapterRegistry.cpp:137-201` |
| `UsdImagingSceneIndexPlugin::InstanceDataSourceNames` / `ProxyPathTranslationDataSourceNames` | `pxr/usdImaging/usdImaging/sceneIndexPlugin.h:80,89` — **non-const virtuals** |
| `HdSceneIndexPlugin::_AppendSceneIndex(renderInstanceId, inputScene, inputArgs)`, `_IsEnabled` | `pxr/imaging/hd/sceneIndexPlugin.h:120-134` |
| `HdSceneIndexPluginRegistryTokens->allRenderers` (`""`), `->rendererDisplayName` (`"__rendererDisplayName"`) | `pxr/imaging/hd/sceneIndexPluginRegistry.h:28-31` |
| `InsertionOrderAtEnd`, `InsertionPhase`, `RegisterSceneIndexForRenderer` | `pxr/imaging/hd/sceneIndexPluginRegistry.h:89-130` |
| `HdSceneIndexPluginRegistry::Define<T>()` single-arg form | used as such at `hdPrman/dependencySceneIndexPlugin.cpp:60` |
| `hdGp:proceduralResolution` tag | `pxr/imaging/hdGp/plugInfo.json:16` |
| `HdSiExtComputationPrimvarPruningSceneIndex` | `pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h` |
| `HdContainerDataSourceEditor::ComputeDirtyLocators` | `pxr/imaging/hd/containerDataSourceEditor.h` |
| `hydra:instanceTranslations|Rotations|Scales` | `pxr/imaging/hd/tokens.h:125-127` |
| `HdIntArrayVectorSchema`, `HdInstancedBySchema`, `HdPrimOriginSchema` | `hd/schemaTypeDefs.h`, `hd/instancedBySchema.h`, `hd/primOriginSchema.h` |
| `dependedOnPrimPath` / `dependedOnDataSourceLocator` / `affectedDataSourceLocator` | `pxr/imaging/hd/dependencySchema.h:36-38` |
| `minScreenSpaceWidths` as a **constant primvar** claimed as builtin | `pxr/imaging/hdSt/basisCurves.cpp:1371-1385` |
| `UsdImagingDataSourceMapped::AttributeMapping::factory`, relationship factories | `pxr/usdImaging/usdImaging/dataSourceMapped.h:83-106` |
| `UsdGeomRegisterComputeExtentFunction(TfType, fn)` + `implementsComputeExtent` plugin metadata | `usdGeom/boundableComputeExtent.cpp:159-173, 271-289` |
| `primvars:widths` legal and takes precedence over `widths` | `pxr/usd/usdGeom/curves.h:162` |
| `apiSchemaAutoApplyTo` propagates to **derived** types | `pxr/usd/usd/schemaRegistry.cpp:853-910, 922-932` |
| `UsdImagingGLEngine` has **no** `GetRenderIndex()` | absent from `usdImagingGL/engine.h` — the performance proposal's pick-fallback caveat is correct |

Two facts I found that the brief marks open and that the plan should record:

* **S36's UNVERIFIED render-context question is probably already answered by the source.**
  `HdStRenderDelegate::GetMaterialRenderContexts()` returns `{glslfx, mtlx}`
  (`hdSt/renderDelegate.cpp:701-707`) and `HdsiMaterialRenderContextFilteringSceneIndex` selects
  "the first render context encountered in `renderContextPriorityOrder`"
  (`hdsi/materialRenderContextFilteringSceneIndex.h:20-45`), implemented in backend emulation at
  `hd/sceneIndexAdapterSceneDelegate.cpp:313, 473`. So `outputs:glslfx:surface` should win over
  plain `outputs:surface` in Storm. All three proposals hedge; the hedge is still worth having, but
  it should be written as a *deletable* path with a one-afternoon gate, which is exactly how the
  risk proposal frames it and not how the artist proposal frames it.
* **The `after: ["hd:sceneGlobals"]` ordering constraint in S1 is vacuous in stock 26.08.** No
  plugin declares an `hd:sceneGlobals` tag — the string appears only as an `after` key in
  `hdPrman/plugInfo.json:190,217`. `HdsiSceneGlobalsSceneIndex` is inserted by the *app* scene-index
  callback (`usdImagingGL/engine.cpp:150-156`), not by a tagged plugin. Placement after it is
  guaranteed by that callback plus phase 0 / `InsertionOrderAtEnd`, not by the tag. Harmless to
  keep, dangerous to rely on. None of the three notices this.

---

## 1. Scores

### D1 Schema — artist 8, performance 7, risk 8

**Artist (8).** The most legible object model and the only one whose `.usda` reads like a groom:
reserved `Ops`/`Guides`/`Maps`/`Prototypes`/`Frozen` scopes, a Hydra-only `<Description>/__usdGenRender`
mirroring usdRig's `__RigExecGenerated`, complete property tables with types *and* defaults, and a
versioning policy that is behavioural, not just numeric (`clump:method` default preserves the old
look). Deductions: the adapter gap in §5.6 (below), an implicit preceding-sibling wiring fallback
that S26 does not sanction and no report measures, and a canonical example that omits
`UsdGenMaskAPI` on the very first operator that uses a mask property.

**Performance (7).** Cleanest hierarchy (`UsdGenNode` → `Operator`/`Primitive`/`Map`), the sharpest
ramp rule in any proposal ("a `.spline` on a ramp *position* is a compile error, because it would
dirty every frame" — the correct reading of S11), and the only one that names the three digests at
schema level. Deductions: no example applies `UsdGenMaskAPI` at all; `UsdGenMap` inherits
`usdGen:input`/`readPhase`/`space` from `UsdGenNode`, which are meaningless on a map; the Groom
carries engine tuning (`chunkSize`, `threadLimit`, `memoryBudgetMB`) as authored stage data, which
means a workstation-specific number gets committed into an asset; `usdGen:primitive` is a config
prim in example (a) and a chain node in example (c).

**Risk (8).** The strongest *contract* thinking: C1–C5 frozen early, "engine headers deliberately
not installed and free to churn", and C3 as **one curve contract with four producers** (freeze,
guide, import, sim cache). It also lands a correctness point neither other proposal makes: a colour
ramp cannot be a `TsSpline` because `TsSpline` supports only `double/float/GfHalf/GfTimeCode`, so
colour ramps are `float[] positions` + `color3f[] colors`. Deductions: an undefined `UsdGenCurveAPI`
in the C3 example, the same adapter-coverage gap, and slice names S0–S8 that collide with the
brief's S-numbering inside the same document.

### D2 Graph/engine — artist 7, performance 9, risk 7

**Performance is decisively ahead here** and its lead is where the money is. The eight invariants
P1–P8 are each tied to a measurement; the chunk/tile split (P5) is the only treatment that resolves
the S23-vs-S27 arithmetic; the reference lane (P3) *removes* the one cost the data-plane report left
unmeasured instead of containing it; the three-digest model (Merkle structural / capture epoch /
per-parameter value version) gives a correct three-line answer to "what does a label edit, a slider
edit and a seed edit cost"; `UsdGenDirtyRouter` with typed bits and a compile-time
`(primPath, locatorPrefix) → (node, bits, surfaceId)` table is the only dirty design with no search
in it; and P8's private `tbb::task_arena` capped at 8 is the single largest un-taken win in the
evidence (`G-data-plane §3.3`: 3.90 ms at 1 thread → **1.02 ms at 8** → 1.79 ms at 20 — the default
20-thread arena is 1.75× slower than the knee, and both other proposals silently inherit it).

Artist (7): correct and faithful, with a genuinely good idea (per-operator stats as an artist-facing
stack profiler), but it conflates engine chunk with Hydra prim and keeps `UpstreamChunks` with a
neighbour-smooth exception. Risk (7): the `UsdGenGraphDesc` pure-value description (engine testable
with no Hydra *and* no USD) and the `TopologyParameters()`/`ValueParameters()` split are both
excellent; its `crossChunk` snapshot + serial pre-pass is the weakest cross-chunk answer of the
three, though it is honestly flagged with a 20 %-of-frame stop condition.

### D3 Imaging — artist 7, performance 9, risk 8

Performance's §6.3 is the most complete statement of the published-prim contract in any of the three
— it is the only one that remembers that usdGen runs **after** flattening (S4), so `visibility`,
`purpose` and `materialBindings` must be authored by hand onto synthesized prims rather than
inherited. Its generation store diffs by `VtArray` data-pointer comparison, and its live override as
a *virtual node patching the publish back-buffer* is the cleanest expression of S40.

Risk (8) has the most implementation-ready registration section (exact plugInfo JSON, correct
`_AppendSceneIndex`/`_IsEnabled` signatures, the "computed points that could not be resolved" hard
diagnostic instead of silence) but ships a signature that will not compile (below). Artist (7) is
correct but thinner; its unique contribution is the two extent channels — Hydra `extent` plus a
`UsdGeomComputeExtentFunction` on a Boundable `UsdGenDescription` so "select the description, press
F" frames the hair.

### D4 Operator catalogue — artist 9, performance 8, risk 8

Artist wins on vocabulary parity: every operator carries a column naming its a host groomer/a DCC/a host renderer
equivalent, and §6.4's "rules every operator obeys" is the most transferable half-page in the three
documents — in particular `hash(seed, curveId, salt)` with a **per-operator salt** so two
`UsdGenClump`s with the same seed do not correlate. Performance's catalogue adds a "what the capture
epoch caches" column, which is the column an implementer actually needs. Risk's parameter lists are
the most complete (its `Clump` row carries the whole a host groomer set) and it is the only one that specifies
the **emitted** primvars — `clumpId_<level>`, `guideIndex[3]`/`guideWeight[3]` at a host renderer's arity —
so a bake round-trips.

### D5 Look — artist 9, performance 8, risk 9

Artist's shader block is verbatim-faithful to the prototype that already renders: I diffed its
`inputs:` list against `probes/storm-hair-look/usdGenHairPreview.glslfx` and the names match
(`rootColor, tipColor, colorRamp, diffuseGain, diffuseWrap, specular1Gain/Width/Shift,
specular2Gain/Width/Shift, transmissionGain, transmissionColor, randomHue, randomValue,
widthFalloff`). Risk matches on the hedge engineering — `USDGEN_STORM_MATERIAL_OVERRIDE` with an
explicit "flip the default and delete the code path" exit — and is the only one to record that
`rand()` is **not** a SeExpr2 builtin and that a `PtexFilter` is not reentrant so there must be one
per TBB worker. Performance names the MaterialX chain most concretely
(`ND_deon_hair_absorption_from_melanin → ND_chiang_hair_roughness → ND_chiang_hair_bsdf → ND_surface`
with an explicit `ND_geompropvalue_vector3` tangent) but contradicts the shipped glslfx on `hairId`'s
type (below).

### D6 Tooling — artist 9, performance 7, risk 9

Artist and risk are complementary and both strong. Artist owns the UX surface: the brush table with
a "commits to" column (comb on guides → guide `points`; comb on hair → a `UsdGenSculptLayer`), the
status line that shows the **edit target** ("usdview points the edit target at the session layer by
default and an artist who does not know that loses work"), hotkeys, and the three progressive-feedback
mechanisms. Risk owns the architecture: the Qt-free module split, state in a dataclass built in
`__init__` (naming the exact drift that broke usdRig's own fixture), the per-tool press/move/release
table, and the four USD landmines with their containment. Performance is thinner on UX but is the
only one that states the pick fallback correctly (a private render index + `HdxPickTask`, *because*
`UsdImagingGLEngine` exposes no `GetRenderIndex()` — verified).

### D7 Roadmap — artist 8, performance 9, risk 9

Risk's eight **shippable vertical slices**, each with a frozen contract, exit criteria, a stop
condition and a "risk retired" line, is the best delivery shape; its S0 gate that asserts RenderMan
ordering *without RenderMan installed* (synthetic `hdPrman:*` tags + `SetPluginOrderingPolicy`) is
the cheapest high-value test in any of the three. Performance's §11.2 gate matrix (E-1…E-8,
SI-1…SI-5, S-1…S-7, T-1…T-5, R-1…R-3, each with a tier and a numeric threshold) is the best
testability artifact; E-8 (bitwise identical output at 1 vs 8 threads with `-ffp-contract=off`) is a
gate the other two need and lack. Artist's phases are sound but its exit criteria are looser and it
does not tier its gates.

### D8 Libraries and build — artist 8, performance 9, risk 9

Risk's **`usdGen` core links no `usd`, no `usdImaging`, no `hd`** turns S8 from a review rule into a
link error; that is the single best build idea in the three documents. Performance is the only one
that justifies the decomposition with a measurement (rebuilding `rigExecImaging` + relink = 10.05 s;
one TU in `rigExec` = 21.4 s because of the pybind11 `-flto=auto` relink), which is the right way to
choose target boundaries. Artist's table is good but its `usdGen` target links no `hio` while its
§7.3 reads EXR through `HioImage` in that library.

### Overall

| | D1 | D2 | D3 | D4 | D5 | D6 | D7 | D8 | mean |
|---|---|---|---|---|---|---|---|---|---|
| artist | 8 | 7 | 7 | 9 | 9 | 9 | 8 | 8 | **8.1** |
| performance | 7 | 9 | 9 | 8 | 8 | 7 | 9 | 9 | **8.3** |
| risk | 8 | 7 | 8 | 8 | 9 | 9 | 9 | 9 | **8.4** |

---

## 2. Constraint violations and unsupported claims

### 2.1 Shared by all three

* **S14 is unaddressed everywhere.** S14 requires that the graph pull every surface/parameter data
  source it depends on at least once per topology generation, *or* declare `__dependencies`. All
  three declare `__dependencies` for the surface→points edge only (32–256 edges per S30). None says
  what happens to a parameter that is only read in one mode (e.g. `usdGen:clump:noise:*` while
  `noise:amount == 0`): if the graph never pulls it, an edit to it may not invalidate. This needs a
  rule ("`Bind()` pulls every mapped locator of the node regardless of mode") and a test.
* **Adapter coverage is registered only for operator-ish types.** S10 is unambiguous and MEASURED:
  without an adapter, `usdGen:*` attributes and relationships are invisible **and emit no notice**.
  Artist registers `primTypeName: UsdGenOperator` + `includeDerivedPrimTypes` only (§5.6); risk adds
  `UsdGenMap` (§3.1); performance says "per `usdGen*` type" without enumerating. In all three,
  `UsdGenGroom`, `UsdGenDescription` and `UsdGenGuideSet` derive from `UsdGeomImageable`, **not**
  from the operator base — so `usdGen:surface`, `usdGen:terminal`/`usdGen:output`,
  `usdGen:chunkCount`, `usdGen:motion:*`, `usdGen:lod:*` would reach Hydra not at all. This is the
  most consequential shared defect: it silently disables the container half of every schema.
* **The mask API schema is described but rarely applied.** `UsdGenMaskAPI` is a single-apply API
  schema in all three. Artist's example (a) omits it on `op01_scatter` while authoring
  `usdGen:mask:source` there; performance's three examples never apply it anywhere. An unapplied API
  schema means the property is not in `UsdPrimDefinition::GetPropertyNames()`, which is exactly the
  list S10's generic mapping is built from, so the mask is silently dropped. Only risk applies it
  consistently. **Fix for the synthesis:** make it auto-apply —
  `"AutoApplyAPISchemas": {"UsdGenMaskAPI": {"apiSchemaAutoApplyTo": ["UsdGenOperator"]}}`; verified
  to propagate to derived types at `schemaRegistry.cpp:922-932`, and it is plugInfo metadata so it
  works for codeless schemas.

### 2.2 Artist

1. **S10.** Adapter registered only on `UsdGenOperator` (§5.6) — see above. Description/Groom/Map/GuideSet
   properties never reach the evaluator.
2. **S26.** §1.8's implicit "preceding sibling" wiring fallback is an addition to a settled decision
   ("explicit `usdGen:input` relationship(s)"). Worse, it depends on `reorder nameChildren` being
   both visible through the scene index and *invalidating* — no report measures that, and there is
   no `PrimsChildrenReordered` notice in Hydra. The tool always authors `usdGen:input`, so the
   fallback buys terse hand-written `.usda` at the price of an unproven invalidation path.
3. **S10, canonical example.** `op01_scatter` in §3.7 authors `rel usdGen:mask:source` with no
   `prepend apiSchemas = ["UsdGenMaskAPI"]`.
4. **Internal inconsistency, same example.** `Scope "Guides"` holds `UsdGenGuideSet "guides"` with
   `usdGen:guides:source = "children"` and then `BasisCurves "g000"` as its *siblings*, while §3.4
   defines the guide set as reading "child `BasisCurves`".
5. **S37 risk.** `UsdGenExprOp` (§6.2) is "a styler whose kernel is a SeExpr expression … at
   13–117 ns/eval" with no statement that it is capture-time only. S37 confines all SeExpr
   evaluation to capture. At 1.6 M CVs × 100 ns that is ~160 ms *per frame*. Performance's
   ASSUMPTION 8 ("no operator may call an expression or a texture in `Evaluate`") is the correct
   discipline and must be stated wherever `ExprOp` ships.
6. **S23/S27 arithmetic.** `usdGen:chunkCount = 64` (prims) and chunk = 512 curves are used
   interchangeably in §4.5 and §5.3, but 40 000 curves at 512/chunk is 78 chunks, and 1 M curves is
   1 953 — which cannot be 64 prims. Never reconciled.
7. **Build.** §9.1's `usdGen` links omit `hio` although §7.3 reads/writes EXR through `HioImage`
   there. §5.7/§9.2 give the plugInfo `Type: library` rewrite for the extent function but not the
   schema-side requirement `customData = {dictionary extraPlugInfo = {bool implementsComputeExtent = true}}`,
   without which `_LoadPluginForType` returns early (`boundableComputeExtent.cpp:159-163`).

### 2.3 Performance

1. **S5 is absent from the document (zero occurrences).** S5 is a *must*: any usdGen scene index
   that overrides a prim's `primvars` container must dirty the bare `primvars` locator as well as
   its leaves, or UsdSkel's resolved prim freezes (MEASURED). §5.4 explicitly defines the case that
   needs it — "`ComputeDirtyLocators` only where it *overlays* an upstream prim — i.e. frozen curves
   it re-deforms in place" — and then never states the rule. This is a silent wrong-geometry bug.
2. **S29 + S35.** `UsdGenTilePublication` declares `VtIntArray hairId` (§6.2) while the shipped
   glslfx declares `hairId` as **`"type": "float"`**, a uniform per-curve float in [0,1)
   (`probes/storm-hair-look/usdGenHairPreview.glslfx:190-194`). An int primvar will not bind and the
   shader silently falls back to 0.0, killing per-curve hue/value jitter. Risk gets this right
   (`hash(curveId)/2^32`).
3. **S24, letter of.** P6 replaces the settled "handoff to Hydra is `VtArray` copy-on-write" with
   A/B double buffering ("never a copy-on-write detach"). The optimisation is legitimate — it moves
   the measured 0.35 ms detach off the interactive path — but it is only sound if no consumer still
   holds the back buffer; hdPrman retains arrays across a sync. No use-count check or fallback is
   given.
4. **S23, letter of.** P5's chunk≠prim split contradicts S23's "chunks are the unit of dirtiness,
   parallelism, and (S29) Hydra prims". I judge the *proposal* right and the *brief* internally
   inconsistent (512-curve chunks at 1 M curves cannot also be ≤256 prims), but this must be
   recorded as an amendment to S23 in the plan, not carried as a silent departure.
5. **Self-inconsistency.** `usdGen:enabled` is declared "Structural: bumps the digest" (§4.2, §4.3).
   By the proposal's own §5.3, a structural change recreates the node and drops its capture cache
   and every descendant's — and by its own §5.6/§7.4 a capture is 10–150 ms. Muting an operator is
   the most common A/B action an artist performs; making it a recompile contradicts §0.3's own
   edit-cost invariants. Artist's rule (mute is a value edit; the node becomes a memcpy
   pass-through) is correct for topology-preserving operators.
6. **Minor.** `usdGenImaging` link list contains the artefact `usdImagingGL(no)` (§3.1). The
   §0.2 ledger's "usdGen deformed tail 0.4–0.6 ms" is derived, not measured, and should be labelled
   as such (its Storm ~12 ms at 100k *is* correctly labelled as interpolated between the measured
   5.01 ms @40k and 23.93 ms @200k, `G-storm-hair-look §5`).

### 2.4 Risk

1. **Will not compile.** §5.1 declares
   `TfTokenVector InstanceDataSourceNames() const override;` and
   `TfTokenVector ProxyPathTranslationDataSourceNames() const override;`. Both base virtuals are
   **non-const** (`usdImaging/sceneIndexPlugin.h:80, 89`); `const override` is a hard error. Drop
   the `const` (performance has it right).
2. **S35.** §3.10 authors `uniform token info:id = "UsdGenHairPreview"`; S35 fixes the shader-def
   identifiers as `usdGen:HairPreview` / `usdGen:HairPreviewTranslucent`.
3. **Undefined schema.** The C3 example (§3.8) applies `apiSchemas = ["UsdGenCurveAPI"]`, a schema
   that appears nowhere in §3.1's API-schema list.
4. **S10.** Adapter registered on `UsdGenOperator` + `UsdGenMap` only — Groom/Description/GuideSet
   properties invisible (shared defect, §2.1).
5. **Traceability defect.** Slices are named S0–S8 in a document whose citation vocabulary is
   S1–S46. "S1" means both "Slice 1 — straight hair" and "renderer-level `HdSceneIndexPlugin`", and
   both appear in the same tables. Rename the slices (P0–P8) before this becomes the plan.
6. **Weakest cross-chunk design.** §4.4's per-chunk `crossChunk` immutable snapshot plus a serial
   pre-pass retains the fan-in the data-plane report flagged as unmeasured, and adds a serial phase
   in the middle of the parallel region's critical path. The stop condition (>20 % of frame at 1 M)
   is the right guard, but the reference-lane design removes the question rather than measuring it.
7. **Minor.** `usdGen:curve:cvCount` on the Description duplicates `GrowFromRoots.segments` and
   `GuideInterpolate.cvCount` — two authored sources of truth for the same number.

---

## 3. Best ideas worth grafting

From **artist**: the reserved namespace (`Ops`/`Guides`/`Maps`/`Prototypes`/`Frozen` + Hydra-only
`__usdGenRender`); `usdGen:enabled` as a non-structural pass-through for topology-preserving
operators; the operator catalogue's prior-art parity column and §6.4's per-operator seed salt
(`hash(seed, curveId, salt)`); the per-operator stack profiler fed by `UsdGenNodeStats`; the
progressive-feedback triad (interactive LOD → camera-seeded chunk-order publish → keep the previous
generation on screen); the glslfx `inputs:` block that matches the shipped prototype name-for-name;
the Boundable `UsdGenDescription` + registered `UsdGeomComputeExtentFunction` so framing works; the
brush table's "commits to" column.

From **performance**: the chunk/tile split (`chunksPerTile = max(1, ceil(nChunks/tileTarget))`,
`nTiles = clamp(…, 32, 256)`); the reference lane (`UsdGenRole::Reference`, `ReferenceInputs()`)
that abolishes chunk fan-in; the private `tbb::task_arena` at the measured 8-thread knee plus
`USDGEN_THREAD_LIMIT` and a first-commit calibration; the three digests; `UsdGenDirtyRouter` with
typed `UsdGenDirtyBits` and a compile-time locator→node table, with surface→chunk as a *range*
because chunks are surface-major; §6.3's complete published-prim table including the
post-flattening `visibility`/`purpose`/`materialBindings` authoring; eviction scored by
`recomputeCostMs/bytes` with "never evict the tail base or a live-override target"; the interruption
rule ("check `_generationRequested` between nodes, never half-publish"); the §11.2 gate matrix,
especially E-8 determinism; the rebuild-cost justification for the library split.

From **risk**: the C1–C5 contract freeze plus the API-stability table ("engine headers deliberately
not installed"); `usdGen` core linking no `usd`/`usdImaging`/`hd` as link-time enforcement of S8;
eight shippable vertical slices with per-slice frozen contracts and stop conditions; the S0 gate
that proves RenderMan ordering with synthetic `hdPrman:*` tags and `SetPluginOrderingPolicy`, no
RenderMan needed; C3 as one curve contract with four producers; `USDGEN_STORM_MATERIAL_OVERRIDE` as
a deletable hedge; the colour-ramp encoding rule derived from `TsSpline`'s value-type restriction;
`rand()` is not a SeExpr2 builtin and one `PtexFilter` per worker; `UsdGenGraphDesc` as a pure-value
description so the engine is testable with neither Hydra nor USD; the usdview `PluginContainer`
dataclass-in-`__init__` rule and the `_env.sh` `lib/python*/site-packages` glob;
`usdGen:<op>:algorithmVersion` for look preservation.

---

## 4. Unresolved questions

1. **S14 mechanics.** What guarantees a parameter that is not read in the current mode still
   invalidates? Pull-every-mapped-locator in `Bind()`, or `__dependencies` on operator prims?
2. **Multi-prim Storm cost is unmeasured.** Every Storm number in the evidence used **one** prim.
   The 32–256 tile default, and therefore contract C2, should not be frozen before a 1/32/128/512-prim
   EGL benchmark.
3. **Does Storm prefer `outputs:glslfx:surface`?** The source says yes
   (`hdSt/renderDelegate.cpp:701-707`, `hdsi/materialRenderContextFilteringSceneIndex.h:20-45`).
   One EGL test settles S36; if positive, delete the per-delegate binding override entirely.
4. **Does `reorder nameChildren` reach the scene index as an invalidation?** S26's namespace-order
   tie-break and artist's implicit wiring both depend on it.
5. **Can `UsdGenMaskAPI` auto-apply to `UsdGenOperator` in a codeless schema?** The registry
   propagates auto-apply to derived types (`schemaRegistry.cpp:922-932`); needs one test.
6. **How many adapter registrations?** One per abstract root (`UsdGenOperator`, `UsdGenMap`, and a
   third covering `UsdGenGroom`/`UsdGenDescription`/`UsdGenGuideSet`), or one class registered
   against several `primTypeName`s?
7. **Is `enabled`/`active` structural?** Decide with the measured capture cost of the node in hand;
   likely "value edit for `Topology()==None`, structural otherwise".
8. **A/B publish vs S24's CoW.** Does any delegate retain the previous generation's arrays long
   enough to force the detach anyway? Needs a `VtArray::IsUnique()` assertion in the publish path.
9. **Uniform CV count per chunk** (performance ASSUMPTION 1): measure the ragged path before the
   fast path is assumed; imported and Alembic grooms are routinely ragged.
10. **Cross-chunk beyond guides and clumps.** Is the reference lane enough for neighbour-mode
    `Smooth` and `Collide`, or does a two-pass gather/scatter node kind have to exist in the v1
    `UsdGenOp` v-table? (Cheap to defer only under risk's "headers not installed" policy.)
11. **Where do engine tuning knobs live?** `chunkSize`/`threadLimit`/`memoryBudgetMB` as authored
    stage properties bakes a workstation's numbers into an asset.
12. **Prototype surface rebasing** for grooms inside propagated native prototypes (performance
    ASSUMPTION 4) is unverified arithmetic on top of MEASURED discovery.
13. **Sculpt rebase policy** after a seed or density change: only the artist proposal offers one
    (re-match by nearest root UV). It needs to be a single agreed rule, since sculpt deltas are the
    artist's hand work.

---

## 5. Recommendation

**Base: `proposal-risk.md`. Engine and imaging chapters: `proposal-performance.md`. Schema
legibility, operator catalogue, look block and tool UX: `proposal-artist.md`.**

The three documents differ less in what they know than in what they are *organised around*, and the
deliverable is a `./plan` directory that a team executes. Risk's skeleton is the one that survives
contact: the C1–C5 contract freeze answers the question the other two do not ask ("what can we
change later?"), and its answer — freeze the `usdGen:` namespace, the chunk contract, the frozen
contract, the C ABI and the glslfx parameter names; install no engine headers — is exactly right for
a system whose three halves are joined by data contracts. Its eight vertical slices map cleanly onto
`11-roadmap.md`, its API-stability table onto `12-risks-decisions-open-questions.md`, and its
link-time enforcement of S8 (`usdGen` core links no `usd`) onto `10-build-dependencies-testing.md`.

But risk's engine is its weakest chapter and performance's is its strongest, so
`03-execution-engine.md` and `06-imaging.md` should be performance's §5 and §6 nearly verbatim:
the chunk/tile split, the reference lane, the three digests, the typed dirty router, the private
8-thread arena, the complete published-prim table, the eviction score, and the no-half-publish
interruption rule. Performance's §11.2 gate matrix should become `09-performance-and-benchmarks.md`
and should supply the numeric thresholds for risk's slice exit criteria, which are currently prose.

Artist supplies what neither of the others does: a stage an artist can read. Take its reserved
scopes and `__usdGenRender` into `02-schema.md`, its parity-annotated catalogue and §6.4 operator
rules into `04-operators.md`, its glslfx `inputs:` block into `07-look-maps-expressions.md`, and its
brush/panel/hotkey/status design into `08-tools.md` on top of risk's module architecture. Adopt
artist's non-structural `enabled` (qualified to topology-preserving operators) over risk's and
performance's structural one.

Four corrections are mandatory before any of this is written up. (1) Register prim adapters for
`UsdGenGroom`, `UsdGenDescription` and `UsdGenGuideSet`, not only for the operator and map bases —
without it half of every schema is invisible (S10, MEASURED). (2) Make `UsdGenMaskAPI` auto-apply to
`UsdGenOperator` so it cannot be forgotten, and fix every `.usda` example accordingly. (3) Restore
S5's bare-`primvars` dirty rule into the imaging chapter, which performance drops. (4) Fix
`hairId` to a uniform **float** and drop the `const` from the two `UsdImagingSceneIndexPlugin`
overrides. Then record two amendments to the brief: S23's "chunks are Hydra prims" is superseded by
the chunk/tile split, and S1's `after: ["hd:sceneGlobals"]` is decorative in stock 26.08.
