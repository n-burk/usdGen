# usdGen design brief v1 — settled decisions after the verification round (2026-09-04)

Supersedes `brief-v0.md`. Written by the orchestrating thread after reading all research
(`../research/A1..A8`) and verification reports (`../research/G-*`, `B-usdrig-build`). Every
decision below carries the report and section that proves it. Architects and writers treat
section 2 as **constraints** (do not re-litigate; cite them), section 3 as the **design space
still open** (this is where proposals differ), and section 4 as the deliverable.

Working name **usdGen**: sibling repo `<usdgen-src>` (empty today), C++ prefix
`UsdGen`, namespace `usdGen`, property namespace `usdGen:`, plugin display name "usdGen".
The reference system is usdRig (code name RigExec, `<usdrig-src>`).

## 1. Requirements (from the request)

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

## 2. Settled decisions (constraints)

### 2.1 Placement and chain (G-chain-order §2–3, §6; G-evaluation-scheduling §3, §10;
### G-hdprman §2; A2 §2; A4 §7)
S1  usdGen's hair scene index is a **renderer-level `HdSceneIndexPlugin`**, `loadWithRenderer: ""`,
    registered in C++ at **insertion phase 0, `InsertionOrderAtEnd`**, plus a plugInfo JSON
    entry with tags `["usdGen:groom"]` and `ordering: {after: ["hd:sceneGlobals"],
    before: ["hdGp:proceduralResolution", "hdPrman:motionBlur"]}`. Measured: it lands strictly
    after the entire UsdImaging chain (RigExec, UsdSkel, flattening, instancing), immediately
    after `HdsiSceneGlobalsSceneIndex`, and before every Storm/hdPrman plugin (velocity motion,
    implicit surfaces, material-binding resolving, dependency forwarding, motion blur, pinned
    expansion). A second `UsdImagingSceneIndexPlugin` is rejected (pointer-ordered, flips with
    `PXR_PLUGINPATH_NAME`, observed upstream of UsdSkel). A usdRig registry hook is rejected.
    (The G-hdprman report's "phase 1" suggestion is superseded: phase 0/AtEnd is fine because
    of S3, and it keeps usdGen ahead of Storm's material-binding and implicit-surface indices.)
S2  Same binary and registration run under Storm ("GL"), all four RenderMan display names and
    usdrecord; both `AppendSceneIndicesForRenderer` callers funnel into one registry.
S3  usdGen reads bound surfaces through a **private `HdSiExtComputationPrimvarPruningSceneIndex`**
    wrapped around its input (pass-through when nothing is computed), so UsdSkel-skinned scalps
    (blocked `primvars/points`, ext-computation points) work under Storm as well as hdPrman.
    It is never spliced into the shared chain.
S4  Everything usdGen reads is post-flattening: `xform/matrix` is world (or prototype-common)
    space with `resetXformStack=true`; dirtiness is per prim, never hierarchical. Output prims
    carry `xform = surface world matrix, resetXformStack=true`, surface-local points, and
    re-dirty their own `xform` when the surface's is dirtied.
S5  Any usdGen scene index that **overrides a prim's `primvars` container** must dirty the bare
    `primvars` locator as well as its leaves (UsdSkel's resolved prim otherwise freezes — a
    measured usdRig/UsdSkel interop bug to file against usdRig).
S6  hdGp is not the vehicle (gated off, uncached `GetChildPrim`, non-chainable, downstream of
    Storm's velocity motion). usdGen may later host its own resolver for third-party procedurals.
S7  A **metadata-only `UsdImagingSceneIndexPlugin`** (returns its input unchanged) is registered
    solely to provide `InstanceDataSourceNames()` (so instances with different groom bindings
    aggregate separately) and `ProxyPathTranslationDataSourceNames()` (so `usdGen:surface`
    targeting an instance proxy translates to the prototype). Ordering is irrelevant for it.

### 2.2 Stage-free parameter transport (G-stage-free §1–7; A4 §2)
S8  **No design element may require a `UsdStage` downstream of the stage scene index.** The
    evaluator is driven entirely by Hydra data sources; the usdview C API is an optional
    accelerator. `usdrecord`/hdPrman correctness never depends on it.
S9  All usdGen prim types are **codeless schemas** (`skipCodeGeneration`, usdRig's gen_schema
    tooling); a common abstract `UsdGenOperator` (and `UsdGenMap`, `UsdGenMask` …) base.
S10 A **UsdImaging prim adapter** (`UsdImagingSceneIndexPrimAdapter`, plugInfo `primTypeName` +
    `includeDerivedPrimTypes: true`) publishes every `usdGen:*` attribute and relationship as a
    typed `usdGen` container via `UsdImagingDataSourceMapped`, with mappings built generically
    from `UsdPrimDefinition::GetPropertyNames()`; invalidation via
    `UsdImagingDataSourceMapped::Invalidate`. Measured: without an adapter, `usdGen:*`
    attributes and relationships are invisible and produce **no notice** — including
    `usdGen:source` retargets. `primvars:usdGen:*` is the prototyping fallback only.
S11 Ramps are **knot arrays** (`float2[] knots` + `float[] values` + token interp) or a whole
    `TsSpline` transported as `HdTypedSampledDataSource<TsSpline>` by the adapter, never a plain
    `.spline` value on a parameter (that is a per-frame dirty and recook). Animated scalars keep
    `.spline`.
S12 The REST surface comes from an **API-schema adapter data source sampling
    `UsdTimeCode::Default()`** (`usdGen/rest/points`), honouring an authored `primvars:rest` when
    present; capture-on-first-cook is rejected. Same for rest curve points of frozen curves.
S13 Asset attributes deliver resolved paths stage-free; `asset[]` gets no reload tracking (one
    `asset` per map, or a child prim per map). Overwritten map files are **not** picked up
    automatically; usdGen ships an explicit "reload maps" action (`ArNotice::ResolverChanged`
    and/or its own texture generation counter).
S14 Time-varying/asset dependency registration is lazy: the graph must pull every surface/parameter
    data source it depends on at least once per topology generation, or declare `__dependencies`.
S15 Evaluator state lives in a **process-global `UsdGenImagingRegistry`** keyed by stage
    (weak) / groom root; scene indices hold weak handles and re-attach after a renderer switch or
    stage replace; sessions tolerate `-REM /` followed by a full re-add and never free caches
    inside a notice.
S16 The ExecUsd/usdExecImaging control plane is **not plugin-extensible in 26.08** (hard-coded
    adapter list). usdGen ships its own evaluator; an OpenExec backend is a future adapter behind
    the same operator interface. Curve arrays never go through OpenExec.

### 2.3 Evaluation scheduling (G-evaluation-scheduling §4–10)
S17 **Never cook inside `GetPrim`** (lazy pull cooks on 4 worker threads and emits zero downstream
    notices) and **never cook in every `_PrimsDirtied`** (hdGp's model over-cooks 2–2.9×).
S18 **Deferred commit + atomic snapshot publish + diffed dirties** (usdRig/usdExecImaging shape):
    accumulate dirty state in notice handlers (forwarding input notices unchanged); commit points
    in priority order: (a) explicit app `Commit()` from the usdview plugin's `currentFrameChanged`
    handler (signal's frame, not the property), (b) the `/ sceneGlobals/currentFrame` dirty
    (measured: last notice of every un-batched frame; exactly 2 `PrimsDirtied` calls per frame),
    (c) a lock-free `atomic<bool>` backstop on the first `GetPrim` of a generated prim (covers
    parameter-only edits with no frame change and batched ordering inversion).
S19 `GetPrim` only `atomic_load`s the published generation and wraps it in data sources (0 torn
    reads under 8 readers × 20 publishes). Notices go out only from the serialized commit path.
S20 Progressive generation uses `asyncAllow`/`asyncPoll`; the usdview plugin enables the 100 ms
    poll itself by setting `_allowAsync` during `registerPlugins`.

### 2.4 Data plane engine (G-data-plane §0, §4–8)
S21 **Bespoke TBB DAG**, not a persistent `VdfNetwork`: measured 4× faster at 100k curves, 9–15×
    at 1M, 3–250× on sparse edits, half the RSS, scales with threads where VDF regresses. VDF's
    500-element grain does not respect curve boundaries and its scheduled affects masks are frozen
    at schedule time.
S22 Curve buffers are **SoA internally** (`VtFloatArray px, py, pz`, planar per-CV primvars;
    per-curve arrays for counts, ids, rootUV, rootPrim), interleaved to `VtVec3fArray` once at the
    Hydra boundary (25 → 81 GFLOP/s measured for the same kernel). No hand-written SIMD; GCC
    autovectorises; layout is the lever. Kernel library compiled with `-ffp-contract=off`.
S23 **Chunk = 512 curves (128–1024 acceptable), always aligned to curve boundaries**, one dirty
    byte per chunk per node; chunks are the unit of dirtiness, parallelism, and (S29) Hydra prims.
S24 **Every operator node owns its output buffer** (per-node caching default; opt-out for memory).
    Budget ≈ 6× the curve data for a 5-deep chain. Handoff to Hydra is `VtArray` copy-on-write
    (one detach on the next edit). A frozen operator is a source node whose buffer is loaded from
    the scene index instead of computed.
S25 Every operator is split into **capture** (topology-dependent: scatter, stable ids, root
    bindings, kd-trees, guide weights, clump ids, map/Ptex/SeExpr samples; cached by an epoch
    digest) and **evaluate** (per-frame, per-chunk parallel). Operators are classified
    `restSpace | deformedSpace` so motion-blur re-evaluates only the deformed-space tail.
S26 Wiring: explicit `usdGen:input` relationship(s); Kahn topological order with namespace order
    as tie-break; cycles are compile errors; read phases (`base|preceding|final|@prim`) retained
    for surface reads. Structural digest over paths/types/relationships/read phases (animated
    scalars excluded) decides recompiles, scoped to the affected sub-graph.

### 2.5 Output prims, Storm representation, throughput (G-storm-throughput §2; G-storm-hair-look
### §2, §5, §7; A5 §3)
S27 Per groom description: **32–256 `basisCurves` chunk prims** partitioned by surface locality
    (never 1: no culling/parallel resolve/whole-groom re-upload; never 10⁴+: dependency fan-out and
    batch validation), plus a `guides` prim set (purpose `guide`), plus optional card/archive
    instancers (S33). The prim set is allocated once and never changes during interaction.
S28 **Exact-size arrays**: `points.size() == Σ curveVertexCounts`, asserted by the publisher.
    Padding is broken in 26.08 (renders fallback red). Density scrubbing keeps element counts
    fixed during the drag (degenerate CVs + zero width at max-density topology, or pre-baked
    density levels) and commits the real count change on release.
S29 Curve contract: `type=cubic`, `basis=bspline` (catmullRom acceptable), `wrap=pinned`, never
    `centripetalCatmullRom`/`bezier`; `widths` vertex or constant (never varying); **no
    `normals`**; `displayStyle/refineLevel=2` pinned per prim; `minScreenSpaceWidths=1.0`;
    per-strand attributes `uniform`; **mandatory primvars** `points, widths, hairT (vertex float,
    root→tip), hairTangent (vertex vec3, object space), hairId (uniform), st (uniform float2, root
    UV), displayColor (uniform)`; `extent` per chunk every deforming frame; `velocities`/
    `accelerations` blocked when usdGen owns points; `primOrigin{scenePath}` on every synthesized
    prim (else picking selects nothing).
S30 Invalidation discipline: bare `primvars/points/primvarValue` where usdGen owns the prim;
    `HdContainerDataSourceEditor::ComputeDirtyLocators` only where it overlays an upstream prim;
    never co-dirty `displayColor` with `points`; one refineLevel and one material per chunk set;
    `__dependencies` declared per chunk (32–256 edges, not per curve). Live paint dirties
    `primvars/<name>/primvarValue` per move and `primvars/<name>` once on appearance.
S31 Measured Storm on the GB10 (EGL headless): 200k curves × 8 CV at refineLevel 2 = 23.9 ms
    (42 fps) at 720p, one prim; deforming 19.2 MB of points adds 2.5 ms. refineLevel 0 is
    *slower* than 1 at 200k (LineList of all CVs); interaction LOD = decimate curve count.
S32 Motion blur: retained per-offset cache; profiles P0 single (default, Storm never samples),
    P1 velocities (opt-in, blocks upstream velocities), P2 samples (lazy on first interval pull or
    app preflight; ≥2 and ≤16 samples, constant CV count across the shutter; only `points`
    blur). (G-motion-blur §4; G-hdprman §3)

### 2.6 Instancing (G-instancing §0–8)
S33 usdGen emits real `instancer` prims for **cards, archives, spheres and clump-instanced
    geometry**, synthesized downstream (renderer level) with hand-authored `instancedBy` (exactly
    one path) on prototypes that are **namespace children of the instancer**; prototypes may be
    subtrees (archives); per-instance variation via `instance`-interpolated primvars; material
    variety via multiple prototypes (Storm has no per-instance material); interactive card edits
    dirty `primvars/hydra:instanceTranslations`, not `instancerTopology`.
S34 Hair on natively instanced scalps is generated once per propagated prototype path
    (`…/UsdNiPrototype/…`), with `instancedBy` authored by hand from `__usdPrimInfo.isNiPrototype`
    / `niPrototypePath`, and relative `primOrigin` inside prototypes. Propagated prototype names
    are hashes: discover, never construct. Per-instance groom variation requires S7's
    `InstanceDataSourceNames`.

### 2.7 Look, maps, Ptex, expressions (G-storm-hair-look; A5 §4–6; A8 §1–4; A7 §5)
S35 **v1 Storm look = plugin glslfx surface shader** (`usdGenHairPreview.glslfx`, already
    written and rendering: Kajiya-Kay + Marschner-lite R/TRT lobes + TT rim, root/tip colour via
    `hairT`, tangent via `hairTangent` with screen-derivative fallback, per-curve hue/value jitter,
    optional scalp map, soft width edge), shipped as two files (`defaultMaterialTag` with
    alpha-to-coverage; `translucent` OIT for fine fur) and two `UsdShade` shader defs discovered
    from a plugin `shaderDefs.usda`. Never read `inData` from the material.
S36 One `Material` prim carries three terminals: `outputs:surface` = UsdPreviewSurface network
    reading `displayColor`/`st` (universal fallback; verified per-curve root-UV texture lookup),
    `outputs:mtlx:surface` = MaterialX `chiang_hair_bsdf` network with an explicit world-space
    tangent geomprop (render-time look; compiles in Storm but renders near-black there),
    Storm-specific = the glslfx. Whether Storm prefers a `glslfx:` render-context output over
    plain `outputs:surface` is UNVERIFIED; the SI may instead bind per delegate.
S37 All map/Ptex/SeExpr evaluation is **CPU at capture time**, baked to per-curve/per-CV primvars
    (colour, density, length, masks, widths); Storm textures only for UV-mapped scalp colour via
    `st`. Ptex is compiled out of the install and mesh-only in Storm's GLSL.
S38 Third-party: vendor wdas/SeExpr `main` @8f8c8f2 (interpreter only, static + hidden, add
    `rand()`; a host groomer variable set `$u $v $id $faceId $P $N $dPdu $dPdv $Pref $Nref $t $frame
    $cLength …`, functions `map() ptex() rand()`; one thread-safe `VarBlock` per worker; measured
    13–117 ns/eval), Ptex v2.4.3 (zlib, static + hidden; face ids and adjacency via installed
    `Far::PtexIndices`; measured 23 ns/lookup; `PtexWriter` for painting), nanoflann 1.12.1
    (kNN), SeExpr's noise as the single noise implementation for expressions and C++ stylers.
    Hio reads png/jpg/tga/bmp/hdr/exr/avif; paint maps are written as EXR (float) or `.ptx`.

### 2.8 Tools (G-tool-loop; G-freeze-bake; A3 §7–8; B-usdrig-build §5–6)
S39 Two Python surfaces: a **ctypes C ABI** for control/scalars (`Activate(stageCacheId, …)`,
    `Commit/SetTime`, `GetGeneration`, `BeginLiveOverride/ClearLiveOverride`, `PickCV`,
    `FootprintCV`, `ReloadMaps`) and a **pxr_boost.python module** for arrays (`ReadCurvePoints/
    Counts/Widths`, `SetLiveOverride`, `SetLiveOverrideIndexed`) — `VtArray` crosses in O(1)
    (0.13 µs at 1M CVs). numpy is the brush-kernel language over a zero-copy `np.asarray(vt)`
    view; never `Vt.*Array.FromBuffer`/`attr.Set(ndarray)` on groom-sized arrays; pin BLAS threads
    to 1. Per-move Python cost measured at 21 µs.
S40 Comb/paint loop = **live override during the drag** (no stage traffic per move; the C++
    registry publishes a generation with only the touched leaves dirtied), one stage write at
    release inside an undo bracket. CV picking is CPU in C++ (166 µs at 100k CVs, 8× cheaper than
    a Hydra pick); CV display via a synthesized `points` child prim (widths + displayColor) or the
    guide's `displayStyle:reprSelector` points slot; CV highlight through Hydra selection is not
    reachable from usdview.
S41 Undo: `SubtreeSnapshot` (`Sdf.CopySpec` into an anonymous stash; O(1) memory; 0.1–0.24 ms)
    beside usdRig's `AttributeSnapshot`; **undo of a live freeze = `SetActive(false)`**, real
    removal only when the entry leaves the stack; every structural edit in one `Sdf.ChangeBlock`;
    prim `Define` outside the change block. `UsdStage::RemovePrim` with an OpenExec system attached
    raises a spurious `Tf.ErrorException` (stock OpenUSD 26.08 bug at `esfUsd/stageData.cpp:360`):
    contain it; never rely on `RemovePrim` for interactive edits; a freeze may only be undone in
    the layer it was authored into.
S42 Frozen-curve contract = plain `UsdGeomBasisCurves` + `primvars:rest` (vertex; free while it
    shares the points buffer), `primvars:skinprim` (uniform int), `primvars:skinprimuv` (uniform
    texCoord2f; not named `st`), `primvars:usdGen:curveId` (uniform int), `primvars:usdGen:frozenEpoch`
    (**constant string primvar**, never customData/attribute), optional `primvars:usdGen:rootFrame`.
    Frozen prims re-enter the graph straight from the scene index — a frozen prim and a generated
    prim are the same kind of styler input. Landing tiers: T1 session layer (interactive), T2
    sidecar `.usdc` sublayer (commit), T3 `.usdc` payload (heavy). Never `.usda`; never bake motion
    samples (author `velocities`). Freezes are siblings under a dedicated scope; tooling never
    re-authors the parent scope.
S43 usdview plugin conventions: `PluginContainer` with state in an `__init__`-initialised object,
    lazy Qt imports, shared undo stack, signal's frame not the property, `UpdateViewport()` after
    every edit/undo, one `_resetGUI` per batched freeze, `RIGEXEC_IMAGING_DLL`-style override for
    out-of-tree libraries, tests select the terminal SI by the `"[Terminal SI]"` prefix.

### 2.9 Build, deps, testing (B-usdrig-build; A8 §6; G-storm-hair-look §0)
S44 Sibling CMake project against the unmodified OpenUSD install; **optional
    `find_package(rigExec CONFIG)`** for `rigExec::rigExecMath` (RMF, ribbon transport, extent,
    weight fields, envelope) — let it bring pxr in transitively or guard `if(NOT TARGET TBB::tbb)`
    (measured CMake 3.28 double-`pxrConfig` trap); `usdGenConfig.cmake` guards `if(NOT TARGET usd)`.
    usdRig's generated-plugInfo pattern (`LibraryPath` = `$<TARGET_FILE_NAME:>`, generated into
    `<build>/usd/<name>/resources`) copied verbatim. Install layout mirrors USD.
S45 Test harness tiers: (1) headless scene-index tests over the real `UsdImagingCreateSceneIndices`
    chain (sub-100 ms, no GL) as the primary regression suite; (2) **Storm correctness and GPU
    timing headlessly via an EGL device-platform context** (`probes/storm-hair-look/eglctx.h`:
    compatibility profile + pbuffer, renders on the GB10) and, as fallback, the scratchpad Xvfb +
    llvmpipe (`DISPLAY=:77`, CPU numbers only); (3) `testusdview` scripts for the app loop; (4)
    workstation protocols for whatever remains (MSAA quality, Metal/Vulkan Hgi path, non-NVIDIA
    drivers). usdRig builds on this host in 21 s with `-ffp-contract=off`; RigExec costs
    ~1.35 ms/frame on ArmShotAnim, leaving ~15 ms/frame for usdGen at 60 fps.
S46 Bugs to file (not usdGen's to fix, but to contain): OpenUSD `esfUsd/stageData.cpp:360`
    invalid-prim predicate on resync; usdRig `RigExecResultsSceneIndex` must dirty the bare
    `primvars` locator; usdRig `testUsdviewRigExec.py` fixture drift; usdRig `_env.sh` Linux gaps.

## 3. Design space still open (where proposals must differ and judges must score)

D1  **Schema shape.** Concrete prim types and property names for grooms, descriptions, operators
    (generators/stylers/deformers/freeze/sculpt layers), guides, masks, maps (image/Ptex/expr/
    paint), materials/look, and the `usdGen:input` wiring; how a "description" groups chunks; how
    chains are composed (per-operator prims vs. stack scopes); read phases; versioning; `.usda`
    examples for the three canonical workflows (generate-and-style, frozen-and-deform, cards).
D2  **Graph/engine API.** Node/op interfaces (capture/evaluate, chunk views, SoA buffers, masks,
    per-node caches), dirty propagation from adapter locators → nodes → chunks → Hydra leaves,
    epoch digests and sub-graph recompiles, generator topology changes, guide interpolation and
    clump cross-chunk queries, motion-blur profiles, memory budgets, thread model (commit thread,
    TBB workers, Hydra readers), diagnostics.
D3  **Imaging library structure.** Registry/session, the private pruning wrapper, chunk prims,
    instancers, guides, live overrides, generation store, notice emission, primOrigin, extents;
    how surfaces are located (`usdGen:surface` relationship → propagated paths).
D4  **Operator catalogue v1 vs later** (A7 §9 is the reference): which generators/stylers ship in
    each phase, parameter vocab, mask block, ramps, seeds, `restSpace|deformedSpace` classes.
D5  **Look pipeline details**: material authoring by the tool, delegate-specific binding, colour
    baking, Ptex face mapping, SeExpr variable/function set, map reload, paint round-trip.
D6  **Tooling UX and architecture**: groom panel, operator stack editor, brushes (comb, cut,
    length, clump, density paint, map paint, place guides), freeze/bake/commit flows, LOD
    controls, undo, hotkeys, status; C API/pxr_boost surface; testusdview coverage.
D7  **Roadmap**: phases with deliverables, exit criteria, test gates, estimates; what the
    prototypes already de-risked; risks and stop conditions.
D8  **Library decomposition and naming**: `usdGenMath` (kernels), `usdGenSchema`, `usdGen`
    (graph/evaluator), `usdGenImaging` (scene index + adapter + registry), `usdGenUsdview`,
    `usdGenShaders` (glslfx/mtlx/shaderDefs), third-party, Python facade.

## 4. Deliverable: the `./plan` directory (markdown, evidence-cited, cross-linked)

README.md (index, TL;DR, status vocabulary EXISTS / PLANNED / BUILD / MEASURED / UNMEASURED),
00-request-and-scope.md, 01-architecture.md, 02-schema.md, 03-execution-engine.md,
04-operators.md, 05-static-curves-and-deformation.md, 06-imaging.md, 07-look-maps-expressions.md,
08-tools.md, 09-performance-and-benchmarks.md, 10-build-dependencies-testing.md, 11-roadmap.md,
12-risks-decisions-open-questions.md, appendix-A-evidence-ledger.md (environment, probes,
measured numbers, file:line index), appendix-B-prototype-inventory.md (what exists in the
scratchpad and what to carry into the repo).
Style: the owner's `docs/superpowers` spec/plan conventions (numbered sections, assumptions,
facts with file:line, architecture with exact names/signatures, testing, out of scope) and the
`docs/spec.md` voice (status labels; "OpenUSD is silent" → usdGen decision).
