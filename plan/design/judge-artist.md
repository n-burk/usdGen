# Judge report — artist / TD usability lens

**Panel seat:** would an a host groomer or a DCC groomer understand and like this schema and tool set? Is the
operator vocabulary complete for real production (parting lines, clump maps, region maps, LOD,
render density multiplier)? Is the interactive loop believable?

**Read in full:** `design/brief-v1.md`, `ENVIRONMENT.md`, `design/proposal-artist.md`,
`design/proposal-performance.md`, `design/proposal-risk.md`, plus `research/A7-prior-art-grooming.md`
§7–§9 (the reference operator catalogue) and spot checks against
`research/G-data-plane-engine-prototype-benchmark.md`, `research/G-storm-hair-look-prototype.md` and
the OpenUSD 26.08 install.

**Verdict in one line.** *Risk* is the right base — it is the only proposal that freezes the surfaces
an artist actually touches (names, curve contract, C ABI, shader inputs) and the only one that answers
"when can a groomer use this" — but it must take *Artist*'s prim tree, freeze semantics and tool loop
wholesale, and *Performance*'s engine mechanics (chunk≠tile, reference lane, three digests, id-hash
LOD) underneath.

---

## 1. How I scored

For an artist lens the load-bearing areas are **D1 (schema = the thing they read and diff)**,
**D4 (vocabulary = the thing they reach for)** and **D6 (tools = the thing they touch)**. D2/D3/D8 are
scored for whether the machinery *supports the felt experience* — instant A/B of an operator, a
stroke that costs a stroke, a stack whose cost is visible — not for engineering elegance in itself.
D7 is scored on whether an artist gets something usable early and whether the names stop moving.

### 1.1 Score table

| Area | Artist | Performance | Risk |
|---|:--:|:--:|:--:|
| D1 Schema | **9** | 6 | 8 |
| D2 Engine | 7 | **10** | 7 |
| D3 Imaging | 8 | **9** | 8 |
| D4 Operator catalogue | 8 | 6 | **9** |
| D5 Look pipeline | 8 | 8 | **9** |
| D6 Tooling UX | **10** | 6 | 8 |
| D7 Roadmap | 7 | 8 | **9** |
| D8 Libraries | 8 | 8 | **9** |
| **Overall** | **8.1** | **7.6** | **8.4** |

The spread is small and the shapes are complementary, which is the most useful thing about this
panel: none of the three is wrong, and the best synthesis is close to "take each one's strongest two
areas".

---

## 2. Proposal-by-proposal, through the lens

### 2.1 Artist-workflow-first — the tree and the loop are right

**D1 (9).** This is the only document where I can open the `.usda` in §3.7 and *read a groom*. Five
nouns, named once (§2), each with an obvious Hydra consequence; reserved namespace children
`Ops / Guides / Maps / Prototypes / Frozen`; everything the evaluator invents lives in
`<Description>/__usdGenRender`, which the artist never sees in the browser (mirroring usdRig's
`__RigExecGenerated`, A2 §5). Two decisions in here are worth more than they look:

* **One prim type per operator *concept*, with a `usdGen:mode` token** (§1.7). `UsdGenScatter` with
  `mode = random|uniform|points|atGuides` instead of four types means switching from a Poisson
  scatter to "at guides" is a value edit. In the other two proposals it is a prim delete + create:
  a resync, a re-wire of `usdGen:input`, a lost mask block, and a walk straight into S41's
  `RemovePrim`-under-OpenExec trap (`esfUsd/stageData.cpp:360`). a host groomer has one generator with a mode
  for exactly this reason (A7 §9.1 key facts).
* **`usdGen:enabled` is explicitly non-structural** (§4.6): the node stays in the graph and becomes a
  memcpy pass-through, so toggling a modifier costs one tail re-run, not a recompile and not a
  re-capture. Both other proposals put the enable flag in the structural digest, which throws away
  that node's kd-trees, clump ids and map samples — 10–150 ms per §5.6/§7.4 of the performance
  proposal — on the single most frequent artist gesture there is. This is a real, felt difference.

Minor gaps: `usdGen:mask:region` is a relationship with no defined region-map semantics;
`UsdGenDescription : UsdGeomBoundable` needs the compute-extent function registered from
`usdGenImaging`, not from the codeless schema resource (§9.2 is ambiguous); the implicit
preceding-sibling wiring fallback (§1.8) rests on `reorder nameChildren` producing a usable
invalidation, which nothing measured.

**D4 (8).** v1 is the right shape — Scatter, Grow, GuideSet, GuideInterpolate, CurveSource, Deform,
Clump, Noise, Length, Width, **Direction** (a host groomer Tilt U/V/N, correctly in v1), Freeze, SculptLayer,
plus the mask block on everything from day one with the stated reason ("retrofitting it later would
change every operator's behaviour"). §6.4's rules are the best-written page in any of the three:
per-operator seed salting so two `UsdGenClump`s with the same seed are uncorrelated, `preserveLength`
after any displacement, a motion badge on `deformed`-space rows so the artist can see what a render
will cost. Weaknesses: `Smooth` and `Resample` are v2 (both are daily); region/parting on
`GuideInterpolate` is v2 and `UsdGenPart` is v3; the `Clump` parameter list drops a host groomer's
`copy`/`copyVariance`/`cut`/`flatness`/`offset`/`curl`/`crossover`.

**D6 (10).** The best tooling section by a wide margin, and it is not close. Ten brushes, each with a
named **commit target** (guide `points` vs a `UsdGenSculptLayer`'s deltas vs a `UsdGenPaintMap`
primvar) — that table alone resolves the question every groomer asks first ("where does my comb go?").
The four-phase press/move/release/escape contract with recompute-from-base (idempotent, abortable).
Hotkeys. A status line that includes **the edit target** — "usdview points the edit target at the
session layer by default and an artist who does not know that loses work" is the single most
production-literate sentence in the three documents. §8.8's progressive feedback (interactive LOD
ceiling, camera-seeded chunk-order progressive publish, capture progress in the stack row, "there is
never a frame with half a groom") is a feature the other two do not have at all. Missing: symmetry is
one word in one table row; there is no mask-visualisation mode.

**D2 (7) / D7 (7).** Correct against S21–S26 but less rigorous than performance: the digest is
described but not structured, threads are named but not arena-isolated, `UpstreamChunks` survives as
an unmeasured fan-in. The roadmap has real exit criteria and stop conditions but assumes one
engineer, and an artist cannot groom anything until Phase 3 of 7.

### 2.2 Performance-and-engine-first — the machine is right, the object model is not

**D2 (10) / D3 (9).** Nothing else comes close. Three named digests with three named consequences
(§4.3): a label edit costs nothing, a clump-amount edit dirties one node's chunks, a seed edit
re-captures without recompiling. The **chunk ≠ tile** split (P5) is the only place in the panel where
S23 (512-curve chunks for the DAG) and S27 (32–256 prims for Storm) are reconciled arithmetically
rather than by hoping one number serves both. The **reference lane** (P3) deletes the one unmeasured
hazard the data-plane report flagged. Morton-sorted roots make per-tile `extent` tight enough that
frustum culling can actually reject something (S29 requires the extent; nothing measured its
tightness) and make a brush footprint touch O(1) chunks. The private `tbb::task_arena` at the measured
8-thread knee stops usdGen stealing Storm's Rprim-sync workers. Interruption that never half-publishes.

**D1 (6).** And then the object model undoes a lot of it. In §4.5's example, `UsdGenDescription
"MainHair"` is a **sibling** of `Scatter`, `Interp`, `ClumpBig`, `ClumpSmall`, `Frizz`, `Deform`,
`Hair`, `DensityMap`, `LengthVariation`, `ClumpMask` and `Guides`, all flat under `/Character/Groom`.
Put three descriptions on a character (hair, brows, lashes — the normal case) and the prim browser
shows one flat bag of ~30 operator prims with nothing but naming discipline to say which chain they
belong to. `usdGen:terminal` is the only structure. That is a legibility regression against a host groomer's
Collection→Description→modifier-stack and against a DCC's chain, and the proposal never argues the
tradeoff (operator sharing between descriptions is a genuine benefit of the flat layout, but it is
not claimed).

Two more D1 problems: four `Scatter*` types instead of one with a mode (see above), and
`UsdGenPrimitive` playing two incompatible roles — in example (a) `UsdGenSplines "Hair"` is a passive
config prim referenced by `usdGen:primitive` and is *not* in the chain, while in example (c)
`UsdGenCards "Cards"` is simultaneously the `usdGen:terminal`, the `usdGen:primitive`, and a node with
`usdGen:input`. An artist cannot form one mental model from those two examples.

**D4 (6).** The thinnest catalogue. v1 has no `Direction`/`Lift` (a host groomer Tilt — v2), no `Smooth` (v2),
no parting lines or region maps *anywhere in v1, v2 or v3* apart from one line of `GuideInterpolate`
capture pseudocode, and a `Clump` parameter list missing copy/cut/flatness/offset/curl/crossover. It
also uses a `UsdGenPtexMap` in its own v1 example (§4.5 `ClumpMask` → `@./maps/clumpmask.ptx@`) while
§7.2 ships `PtexMap` in v2.

**D6 (6).** Eight tools and the correct loop, but no hotkeys, no status line, no edit-target surfacing,
no symmetry, thin freeze flows. One excellent idea: the parameter panel **generated from
`UsdPrimDefinition::GetPropertyNames()`**, so a new operator gets a UI for free — the natural pair to
S10's generic adapter mappings.

### 2.3 Risk-and-delivery-first — the contracts and the vocabulary are right

**D7 (9) / D8 (9).** The five frozen contracts C1–C5 versus "everything else churns freely", enforced
structurally by *not installing* the engine headers (§10.2), is the best single decision in the three
documents from an artist's point of view: the names on their stage, the curve contract, the shader
inputs and the Python surface stop moving early, and the engine stays free to be rewritten. `usdGen`
core **links no `usd`, no `usdImaging`, no `hd`** turns S8 into a link-time property instead of a
review rule. The eight slices are each demonstrable in usdview, with per-slice exit criteria, staffing
stated, and a totals table that answers the question a production asks: *S0–S5, an artist can groom,
20 weeks*.

**D4 (9).** The most faithful implementation of A7 §9. `UsdGenClump` in v1 carries the full
a host groomer/IGS parameter set including `copy`, `copyVariance`, `cut`, `flatness`, `offset`, `curl`,
`crossover`, `goalFeedback`, `sizeReduction`, `tightnessReduction`, `levels`, `stray{Amount,Rate,
Falloff}`, `volumize`, `preserveLength`. `UsdGenGuideInterpolate` carries `regionMap` **and**
`clumpCrossover` in v1 — the only proposal that ships region maps in the first release. `Smooth` and
`Resample` are v1. `UsdGenScale` (a host groomer IGS's global length multiplier) is there. And it is the only
proposal that says operators **emit** `clumpId_<level>` (uniform int) and `guideIndex[3]` /
`guideWeight[3]` primvars — a host renderer's `groom_closest_guides` / `groom_guide_weights` arity — so a bake
round-trips and a shader can drive variation from clump id. Gaps: `Direction`/`Lift` is v2 (should be
v1), and there is still no parting-line *operator* or brush at any tier.

**D5 (9).** `UsdGenLookAPI` on the description with an explicit bake order
(`albedo = ramp(hairT_root) × colorMap(rootUV) × jitter(hairId) → displayColor`) and a `bakeTarget`
token is the missing piece in the other two: it makes the UsdPreviewSurface fallback path agree with
the glslfx and the MaterialX chain instead of leaving root/tip colour only on the shader. The
`USDGEN_STORM_MATERIAL_OVERRIDE=0` kill switch turns S36's UNVERIFIED item into a flag that gets
deleted, not an open risk. The SeExpr set is the most complete, and it is the only one that records
that `rand` is **not** a SeExpr2 builtin ("verified: Function rand has no definition").

**D1 (8) / D6 (8).** C3 — one curve contract for guides, freezes, imports and sim caches, tagged by
`primvars:usdGen:role = "hair"|"guide"` — is elegant and removes four future migrations; guide sets
are first class and carry a per-guide `usdGen:blend` array (a host groomer's range-of-influence, which nobody
else models). Tooling has the per-tool press/move/release table, the missing
`UsdGenImaging_ClosestSurfacePoint` C ABI call that Place-guide and paint actually need, and the
usdview gotchas that only come from having shipped one (state in a `UsdGenToolState` dataclass built
in `__init__`; viewport tools installed from `signalStageReplaced` with a bounded 0 ms `QTimer` retry
because plugins load before `StageView` exists; BLAS threads pinned before `import numpy`). But the
prim tree is only implied by the examples, not made normative like Artist's reserved scopes, and
`usdGen:active` collides with USD's own prim `active` metadata that S41 makes load-bearing.

---

## 3. Constraint violations and evidence contradictions

Listed per proposal; each is either a contradiction of S1–S46, a contradiction of a measured number,
or a mechanism that does not exist in OpenUSD 26.08.

### Artist

1. **§1.8 implicit `usdGen:input` fallback.** S26 settles wiring as "explicit `usdGen:input`
   relationship(s)". The preceding-sibling fallback plus "drag-reorder = one `reorder nameChildren`
   edit" (§8.1) depends on a nameChildren reorder producing usable Hydra invalidation. S10 measured
   that even authored `usdGen:*` relationship retargets produce **no notice** without an adapter, and
   the adapter maps *properties*, not sibling order. Unverified mechanism on the stack editor's
   primary gesture. (Mitigated by the proposal's own R-11: the tool always authors `usdGen:input`.)
2. **§3.2 `usdGen:renderDensityScale` "applied only when the delegate is not Storm".** No S-number
   supports mapping delegate identity to render intent. `usdrecord --renderer GL` is Storm and *is* a
   render; an hdPrman-class interactive viewport is not Storm and *is* interactive. The behaviour
   would be wrong in both directions.
3. **§3.3 `usdGen:mask:ramp` shown as `float` with `.spline`.** The prose is S11-correct (whole
   `TsSpline`, never `FlagAsTimeVarying`), but the property table row reads as the plain `.spline`
   parameter S11 explicitly forbids. Presentation defect that will be implemented wrong.
4. **§3.2 / §9.2 compute-extent registration.** `UsdGenDescription : UsdGeomBoundable` with a
   `UsdGeomComputeExtentFunction` requires the plugInfo to name a real library; §9.2 says the
   *schema* plugInfo is rewritten to `Type: library`, but a codeless schema (S9) has no library. The
   function must live in `usdGenImaging` and the plugInfo must point there (as Performance §3.1 and
   Risk §2 both say). Under-specified rather than wrong.

### Performance

1. **§4.2 `usdGen:renderDensity` — "applied only when `sceneGlobals` says we are not interactive".
   The field does not exist.** `HdSceneGlobalsSchema` in this install carries exactly
   `sceneGlobals, primaryCameraPrim, activeRenderPassPrim, activeRenderSettingsPrim, startTimeCode,
   endTimeCode, timeCodesPerSecond, currentFrame, sceneStateId`
   (`$USD/include/pxr/imaging/hd/sceneGlobalsSchema.h:37-47`). There is no
   interactive flag. This is a hard evidence contradiction on the one a host groomer feature the lens was asked
   to check (Render Density Multiplier, A7 §1.5 / §5).
2. **§4.2 `usdGen:enabled` marked "Structural: bumps the digest"**, combined with §5.3 ("only the
   changed nodes and their descendants are recreated"), means toggling an operator drops that node's
   capture cache. §5.6 and §7.4 put capture at ~10 ms (guide kNN on 8 threads) to ~15 ms (1 M-root
   mask) to "~300 ms at 1 M curves" in the risk proposal's own gate. S25's capture/evaluate split
   exists precisely to keep that off the interactive path. Not a literal S-violation; a violation of
   its intent on the most frequent artist gesture.
3. **§4.5 example (a) uses a `UsdGenPtexMap` in v1** (`ClumpMask` = `@./maps/clumpmask.ptx@`) while
   §7.2 schedules `PtexMap` for v2. Internal inconsistency.
4. **§0.2 frame ledger.** "Storm draw, 100k × 8 CV, refineLevel 2 | ~12 ms" is interpolated between
   the measured 5.0 ms @40 k and 23.9 ms @200 k (`G-storm-hair-look` §5, and the report's own key
   facts line). It is labelled "interpolated", but it sits in a table of MEASURED rows and then
   becomes gate **S-1**'s threshold (≤ 14 ms). Measure it on the EGL harness before it is a contract.
5. **§5.5 interruption.** "`Commit` checks `_generationRequested` between nodes and returns the
   previous generation unchanged" — the published frame then lags the scene's frame with no pending
   dirty, and nothing in the text re-arms the S18(c) `atomic<bool>` backstop. `GetPrim` would serve
   stale points indefinitely if no further notice arrives.
6. **ASSUMPTION 1 (uniform CV count per chunk) vs R3/S42.** Frozen, Alembic-imported and simulated
   grooms — precisely the R3/R4 inputs — routinely have mixed CV counts and drop to an unmeasured
   "slower indexed path". Owned as R-3, but the artist-visible consequence (an imported groom is
   silently slower until someone adds a `Resample`) is not surfaced anywhere in the UI.

### Risk

1. **§3.2 `usdGen:active`.** Collides with USD's prim `active` metadata, which S41 makes load-bearing
   (`undo of a live freeze = SetActive(false)`). Two different "active" flags on the same prim, one of
   which deletes the prim from composition. Rename to `usdGen:enabled`.
2. **§4.3 `usdGen:active` in the structural digest** — same intent violation as Performance #2.
3. **§10.1 R4's stated fallback contradicts a measured number.** "If > 20 % of frame at 1 M curves,
   make chunk size a per-description tuning parameter… widen chunks for clump-heavy graphs (chunk =
   clump neighbourhood)". S23's measurement is 128 → 1.55 ms, 512 → 1.83, 1024 → 1.89, **4096 → 9.40**
   (`G-data-plane` §295-296, §572-573). Widening past 1024 is a 5× cliff, so the fallback is not
   reachable. The real fallback has to be Performance's two-pass gather/scatter node on a per-frame
   grid, or dropping the operator class.
4. **§3.3 `chunkCurves` (512) vs `chunkCountMax` (256) are not reconciled.** The stated allocation is
   `min(chunkCountMax, ceil(maxCurves / chunkCurves))`, so at 1 M curves the cap binds and each
   published prim carries ~3 906 curves, not the documented 512. Performance's explicit
   `chunksPerTile = max(1, ceil(nChunks / tileTarget))` (P5) is the correct reconciliation and Risk
   should adopt it verbatim.
5. **§3.5 colour-ramp encoding** (`float[] positions` + `color3f[] colors`) is a re-reading of S11's
   literal "`float2[] knots` + `float[] values`". The proposal flags it as an ASSUMPTION and gives a
   sound reason (Ts supports only double/float/GfHalf/GfTimeCode). Needs a ratification, not a fix.
6. **§7.1 hedge under-specified.** The SI "overrides `materialBindings/allPurpose` … to a synthesized
   material" — but S1 places usdGen *before* Storm's material-binding-resolving index, so the
   synthesized material must exist as a real prim in usdGen's own output. Where it lives, whether it
   carries `primOrigin`, and whether it survives the resolving index are not stated.
7. **§9.2 S1 exit test 6** ("100 k curves × 8 CV, static, ≤ 12 ms at 1280×720") is the same
   interpolated Storm number as Performance #4, presented as a gate without an UNMEASURED label.

---

## 4. Best ideas to graft

### From *Artist* (schema legibility and the loop)

1. **Reserved namespace children on the Description** — `Ops`, `Guides`, `Maps`, `Prototypes`,
   `Frozen` — plus the Hydra-only reserved scope `<Description>/__usdGenRender`. Makes a `.usda` diff
   readable and keeps machine prims out of the browser.
2. **One prim type per operator concept with a `usdGen:mode` token** (`UsdGenScatter` with
   `mode = random|uniform|points|atGuides`). Mode switching becomes a value edit, not a prim
   delete/create resync.
3. **`usdGen:enabled` explicitly non-structural** — the node stays in the graph as a memcpy
   pass-through; no recompile, no re-capture. A/B-ing a modifier is instant.
4. **`UsdGenFreeze` with `usdGen:frozen:mode = frozen|live` and `frozen:tier = session|sublayer|
   payload`.** Freeze *caps* the chain; upstream operators stay authored and greyed in the stack
   editor; unfreeze is one token. (Performance's freeze *rewires* `usdGen:input`, which is a
   structural stage edit per freeze and per unfreeze, and collides with S41's "a freeze may only be
   undone in the layer it was authored into".)
5. **Two density controls: `usdGen:densityScale` (preview) + `usdGen:renderDensityScale` (render).**
   a host groomer's exact pair; the other two carry only the render half.
6. **The stack profiler column** — `UsdGenNodeStats{captureMs, lastEvalMs, meanEvalMs, chunksDirty,
   captureHits/Misses, warnings}` published per generation, shown per operator row as ms and "% of
   frame", so an artist lowers `clump:levels` instead of guessing.
7. **§8.8 progressive feedback**: interactive LOD ceiling during a drag; chunk-order progressive
   publish seeded from the camera over `asyncAllow`/`asyncPoll`; capture progress in the stack row;
   "the previous generation stays on screen until the new one is complete".
8. **The status line with the edit-target label** — `curves … · chunks … · eval … ms · gen … ·
   LOD 40k · edit target: session`.
9. **`usdGen:clump:centers`** as an explicit relationship (clump centres from a curve set, a nested
   scatter, or a map) — the only proposal that models a host groomer's clump map as an artist-visible input
   rather than an internal detail — and **`usdGen:mask:region`** on the universal mask block.
10. **Ten-brush shelf with a named commit target per brush**, plus `usdGen:sculpt:freezeMask` (a host groomer
    Freeze brush) and a **"Rebase sculpt"** action that re-matches deltas by nearest root UV when
    `frozenEpoch` goes stale.
11. **Extent on two channels**: Hydra `extent` per chunk *and* a `UsdGeomComputeExtentFunction`
    registered on `UsdGenDescription`, so "select the description, press F" frames the hair
    (`UsdGeomBBoxCache` ignores Hydra entirely, A2 §6).
12. **§6.4's operator rules**: per-operator seed salting `hash(seed, curveId, salt)`; `preserveLength`
    after any displacement; a motion badge on `deformed`-space rows.

### From *Performance* (the machine under the loop)

13. **Chunk ≠ tile** (P5): `chunkSize` 512 for the DAG, `tileTarget` 32–256 for Storm,
    `chunksPerTile = max(1, ceil(nChunks / tileTarget))`. The only correct reconciliation of S23 and
    S27; replaces Risk's unreconciled `chunkCurves`/`chunkCountMax`.
14. **The reference lane** (P3): guides and clump centres are `UsdGenRole::Reference` nodes evaluated
    in full before their consumers, so hair chunks never read hair chunks; `Capture` stores
    `guideIdx[3]`/`guideW[3]` as planar arrays in `UsdGenCapture{i0,i1,i2,f0,f1,f2,tree}`. Deletes
    the unmeasured `UpstreamChunks` fan-in for every v1 operator.
15. **Three digests, three consequences**: Merkle **structural digest**; per-node **capture epoch**
    (surface topology hash + rest points hash + seed + capture params + map asset + texture generation
    + upstream epoch); per-parameter **value version**.
16. **LOD by stable-id decimation**: `keep if hash32(id) < ratio·2^32`. Decimating never re-scatters
    and never changes ids, so sculpt deltas, clump ids and frozen epochs survive an LOD change —
    the artist-critical property Risk's `lod:density` and Artist's `interactive:maxCurves` leave
    unstated.
17. **Private `tbb::task_arena`** at the measured 8-thread knee (3.90 ms @1 → 1.02 @8 → 1.79 @20),
    `USDGEN_THREAD_LIMIT`, one-shot calibration; stops usdGen stealing Storm's Rprim-sync workers.
18. **Morton-sorted roots at capture** so a chunk is a spatial cluster → tight per-tile `extent` for
    frustum culling and a brush footprint that touches O(1) chunks; with gate S-4 (camera framing ¼
    of the groom, `itemsDrawn` drops ≥ 3×) and the UV-island-partition fallback.
19. **The numbered gate table** E-1…E-8 / SI-1…SI-5 / S-1…S-7 / T-1…T-5 / R-1…R-3, thresholds tied to
    measured baselines and mapped onto the four test tiers.
20. **`protoSelect = random | byMap | byClumpId`** on the card/archive instancer — clump-driven card
    selection is a real production trick and costs nothing once `clumpId` is a primvar.
21. **The parameter panel generated from `UsdPrimDefinition::GetPropertyNames()`** — a new operator
    gets a UI for free; the natural pair to S10's generic adapter mappings.
22. **Never half-publish**, with `_generationRequested` so a scrub supersedes an in-flight commit
    (fix its backstop re-arm, §3 Performance #5).
23. **Eviction score `recomputeCostMs / bytes`** with "never evict the tail base; never evict a node a
    live override targets".
24. **The explicit MaterialX chain**: `ND_deon_hair_absorption_from_melanin → ND_chiang_hair_roughness
    → ND_chiang_hair_bsdf → ND_surface` with `ND_geompropvalue_vector3(geomprop="hairTangentWorld")`
    into `curve_direction`.

### From *Risk* (contracts, vocabulary, delivery)

25. **C1–C5 frozen contracts vs everything-else-churns**, enforced by *not installing* the engine
    headers. Artist-facing names stop moving at S1/S2; the engine stays rewritable.
26. **C3 = one curve contract for guides, freezes, imports and sim caches** (`UsdGenCurveAPI` +
    `primvars:usdGen:role = "hair"|"guide"`). Guides need no schema of their own; an Alembic sim cache
    is a first-class styler input (R4's "rigged or simulated curves") for free.
27. **`usdGen` core links no `usd`, no `usdImaging`, no `hd`**; `UsdGenGraphDesc` is a pure value type.
    S8 becomes a link-time property and the whole engine is testable with no scene index.
28. **`UsdGenLookAPI`** on the description (`look:rootColor/tipColor/colorRamp/colorMap/hueJitter/
    valueJitter/bakeTarget`) with the explicit bake order — makes the UsdPreviewSurface fallback,
    the glslfx and the MaterialX chain agree.
29. **`usdGen:<op>:algorithmVersion`** (uniform int, default = current) so a kernel fix in a later
    release cannot shift a shipped shot's look.
30. **Clump's full A7 §9.2 S1 parameter set in v1**, and `GuideInterpolate` carrying `regionMap` +
    `clumpCrossover` **in v1**.
31. **Operators emit `clumpId_<level>` (uniform int) and `guideIndex[3]`/`guideWeight[3]` primvars** —
    a host renderer `groom_closest_guides`/`groom_guide_weights` parity, so a bake round-trips and a shader can
    read clump id.
32. **`UsdGenImaging_ClosestSurfacePoint(surfacePath, p, &face, uv, P)`** in the C ABI — the missing
    primitive for Place-guide, density paint and map paint.
33. **The hard-error diagnostic** `usdGen: surface <path> has computed points that could not be
    resolved` instead of silently grooming the bind pose, plus `testUsdGenSkelInterop` asserting the
    S5 bare-`primvars` promotion.
34. **`USDGEN_STORM_MATERIAL_OVERRIDE=0`** — S36's unverified render-context resolution behind a
    switch that gets deleted rather than an open risk.
35. **usdview plugin specifics**: state in a `UsdGenToolState` dataclass built in `__init__`; viewport
    tools installed from `signalStageReplaced` with a bounded 0 ms `QTimer` retry; `usdGenLib.py`
    pinning `OPENBLAS_NUM_THREADS`/`OMP_NUM_THREADS` before `import numpy`.
36. **Per-guide `usdGen:blend`** float array on `UsdGenGuideSet` (a host groomer's range-of-influence) and the
    preserved SeExpr `#3dpaint,N` / `#min,max` annotation convention in the expression field.
37. **The eight-slice plan with the S1 stop condition**: if precise `usdGen:*` invalidation cannot be
    demonstrated, fall back to `primvars:usdGen:*` and re-plan — *do not* proceed to S2 with imprecise
    dirtying.

---

## 5. Unresolved questions

1. **What distinguishes "render" from "interactive preview" for the render-density multiplier?**
   `sceneGlobals` has no interactive flag (`sceneGlobalsSchema.h:37-47`). Candidates: the presence of
   `activeRenderSettingsPrim`, the renderer display name from `inputArgs`, or an explicit
   `usdGen:densityContext` token the app forces. Must behave correctly in usdview *and* in
   `usdrecord --renderer GL`.
2. **Does `reorder nameChildren` produce usable Hydra invalidation, and at what cost?** Artist's stack
   editor and implicit-sibling wiring both depend on it. A parent resync of N operator prims is a very
   different price from a relationship retarget. Measurable in tier 1 in an afternoon.
3. **Must toggling an operator be structural?** Artist says no (memcpy pass-through), Performance and
   Risk say yes. The answer depends on whether a downstream capture (clump ids, guide weights) depends
   on the disabled node's *output topology*. Decide once and measure the A/B cost, because it is the
   most frequent artist gesture in the product.
4. **Region maps and parting lines: what is the data model, and when do they ship?** A per-face
   int/colour map, a curve set (a DCC `guidepartition`: `radius`, `strength`), or both? Which
   operators consume it (GuideInterpolate, Clump, Part)? Risk ships `regionMap` in v1; nobody ships a
   parting-line authoring tool before v3. A hairline part is table stakes for a character groom.
5. **Units.** Is `usdGen:density` hairs per square *stage* unit on the rest surface (Artist assumption
   3), and what happens under a non-uniform surface scale or a `metersPerUnit` change? Only one
   proposal states the rule and none states the scale behaviour. Artists notice this on day one.
6. **Ragged CV counts.** What is the measured penalty of Performance's non-uniform path, and does
   `Length(mode=cutAbsolute, rebuild=keepParam)`, `Braid`, or an imported Alembic groom force it? If
   >2×, does `Resample` become mandatory on import — and is that surfaced in the UI or silent?
7. **Where does the per-delegate synthesized material prim live**, does it need `primOrigin`, and does
   it survive Storm's material-binding-resolving index that runs after usdGen (S1)?
8. **The real fallback for cross-chunk operators** (`Smooth(neighbours)`, Deintersect,
   PreserveClumps). Risk's "widen chunks" is blocked by S23's 4096 cliff (9.40 vs 1.83 ms). Is it
   Performance's two-pass gather/scatter node on a per-frame grid, or is the operator class cut?
9. **Is the published prim's curve count allowed to float?** `chunkCurves` 512 vs a 256-prim cap
   cannot both hold at 1 M curves. Adopt `chunksPerTile` or make `chunkCurves` advisory and say so.
10. **How does an artist debug a mask?** None of the three ships a DCC's `Visualize Masks`
    equivalent, nor a "show the three guides driving this hair" display — even though the resolved
    per-curve mask array and `guideIndex/guideWeight` already exist in all three designs.
11. **Is symmetry (mirror-X grooming) a v1 brush feature?** One word in one table row across three
    documents. It interacts with stable ids, root UVs and the footprint kernel, so it is not a late
    add.
12. **Sculpt-layer rebase policy.** Rebase by nearest root UV (Artist) or keep-and-ignore (all three)?
    Does rebase run in the tool or the evaluator, and is it a single undoable action?
13. **Is `_allowAsync` from a plugin actually reachable** (S20, UNMEASURED), and is the camera
    reachable from a renderer-level SI (`sceneGlobals/primaryCameraPrim`) in both usdview and
    usdrecord? Artist's camera-seeded progressive publish needs both.
14. **Storm cost at 100 k curves** is interpolated (5.0 ms @40 k, 23.9 ms @200 k measured); two
    proposals make it a gate threshold. Measure it on the EGL harness before it becomes a contract.
15. **Can operators be shared between descriptions?** Performance's flat layout allows it (one clump
    setup across brows and lashes — a real want); Artist's and Risk's `Ops` scope forbids it. If
    sharing is wanted, the tree needs a third answer (e.g. a `Presets` scope referenced by
    `usdGen:input` across descriptions).

---

## 6. Recommendation

**Base: `proposal-risk.md`.** From an artist and TD standpoint the most valuable property of a plan is
that the names on the stage stop moving, and Risk is the only proposal that makes that a *structural*
commitment: C1–C5 frozen at S1/S2/S5, everything else deliberately not installed and free to churn.
It also has the most production-faithful operator vocabulary (A7 §9 implemented rather than
summarised — Clump's full parameter set, `regionMap` and `clumpCrossover` in v1, `Smooth`/`Resample`
in v1, `clumpId_<level>` and `guideIndex/guideWeight` emitted as primvars), the only description-level
look/bake schema (`UsdGenLookAPI`), the one curve contract shared by guides, freezes, imports and sim
caches (C3), and the only roadmap that says when a groomer can groom (S0–S5, 20 weeks). Its weaknesses
are exactly the other two proposals' strengths, so the graft is additive rather than structural.

**Graft from Artist — the object model and the loop.** Take the reserved Description scopes
(`Ops/Guides/Maps/Prototypes/Frozen`) and the Hydra-only `__usdGenRender` as normative; take one prim
type per operator *concept* with a `usdGen:mode` token (killing Risk's and Performance's four
`Scatter*` types and the delete/create resync they force); rename `usdGen:active` to
`usdGen:enabled` and make it **non-structural**; take the `UsdGenFreeze` mode/tier tokens instead of
Performance's rewiring freeze; take the preview/render density pair, the stack profiler column, the
progressive-feedback trio, the status line with the edit-target label, the ten-brush table with named
commit targets, `usdGen:clump:centers`, `usdGen:mask:region`, the two-channel extent, and §6.4's
operator rules (seed salting, `preserveLength`, motion badge).

**Graft from Performance — the machine.** Take the chunk≠tile split with `chunksPerTile` (it fixes
Risk's unreconciled `chunkCurves`/`chunkCountMax`); the reference lane (it deletes the cross-chunk
hole that Risk's own R4 cannot escape, because S23's 4096 cliff blocks the "widen chunks" fallback);
the three-digest model; stable-id-hash LOD decimation (which makes LOD safe for sculpt deltas); the
private task arena; Morton-sorted roots with the culling gate; the numbered gate table wired into
Risk's slice exits; `protoSelect = byClumpId`; the auto-generated parameter panel; and the MaterialX
chain.

**Fix before writing the plan.** Drop the `sceneGlobals`-interactive mechanism (it does not exist) and
decide question 1; label the interpolated 100 k Storm number UNMEASURED wherever it appears as a gate;
correct Risk's R4 fallback; specify where the synthesized per-delegate material prim lives; and pull
`Direction`/`Lift` (a host groomer Tilt) into v1 in every catalogue — it is a daily tool in all three reference
systems and it is a trivial kernel.
