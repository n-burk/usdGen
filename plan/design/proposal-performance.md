# usdGen — architecture proposal (performance-and-engine-first lens)

Date: 2026-09-04. Status: **proposal**, written against `design/brief-v1.md` (requirements R1–R9,
settled decisions S1–S46, open areas D1–D8). Every settled decision is treated as a constraint and
cited, never re-litigated. Everything I add beyond the evidence is marked **ASSUMPTION**.

Status vocabulary, as in the owner's `docs/spec.md`: **EXISTS** (in usdRig/OpenUSD today),
**MEASURED** (a number produced on this host during the research rounds), **UNMEASURED** (needs the
workstation or a protocol below), **BUILD** (a prototype exists in `scratchpad/probes/`),
**PLANNED** (this proposal).

**The lens.** This proposal optimises the interactive and render-time path first and lets everything
else fall out of it: exact buffer layouts, the chunk/tile split, per-node caches, the dirty route
from adapter locator to Hydra leaf, the capture/evaluate split, epoch digests and sub-graph
recompiles, cross-chunk queries, the motion tail, memory budgets, the thread model, and the
generation store. Where a design choice is legible only through a number, the number is quoted with
its report.

---

## 0. Thesis, and the frame budget that produces it

### 0.1 Thesis

> **The groom evaluator is not the bottleneck; Storm is.** At the scale the request names, a
> five-operator chain over 100k curves costs **1.7–1.9 ms** on 20 cores (MEASURED,
> `G-data-plane §4`), while drawing those same curves at refineLevel 2 costs **~12 ms** at 720p and
> **23.9 ms at 200k curves** (MEASURED, `G-storm-hair-look §5`). Every architectural decision below
> is therefore made to (a) keep the evaluator's *deformed-frame* cost under **2 ms**, (b) publish as
> few bytes into Storm as the edit actually changed, and (c) give the viewport an LOD lever that
> reduces **curve count**, because reducing refineLevel does not help
> (refineLevel 0 is *slower* than 1 at 200k — 12.4 vs 8.2 ms, MEASURED, same report).

The corollary that shapes the engine: the chain is split so that a *frame* re-runs only the
deform-transport tail, not the chain (S25); an *edit* re-runs only the dirty sub-graph on the dirty
chunks; and a *publish* re-uploads only the tiles whose chunks changed, because Storm's upload
granularity is the prim (`hdSt/vboMemoryManager.cpp:624-682`, `G-storm-throughput §1.3`).

### 0.2 The frame ledger (100 000 hairs × 8 CV, 1920×1080-class viewport, 60 Hz target)

| Stage | Cost | Source |
|---|---:|---|
| usdRig rig evaluation (ArmShotAnim) | 1.35 ms | S45, MEASURED |
| usdGen dirty routing + commit bookkeeping | 0.05 ms | 0.2 µs/notice entry × ~200, `G-evaluation-scheduling §9` |
| usdGen **deformed tail** (`DeformWithSurface` + deformed-space stylers) over 800k CVs | 0.4–0.6 ms | one styler pass 0.16–0.55 ms single-thread; 5-node chain 1.72 ms at 20 threads, `G-data-plane §4, §6` |
| SoA → AoS interleave of dirty tiles (worst case: all) | 0.25–0.5 ms | 0.49 ms per 19.2 MB `VtArray` pass, `G-motion-blur §5` |
| per-tile extent (64 tiles) | 0.05 ms | trivial min/max over the same pass |
| generation diff + publish + notice emission (64 tiles) | 0.02 ms | `G-storm-throughput §1.8` |
| **usdGen total, deform frame** | **≈ 0.8–1.2 ms** | |
| Storm draw, 100k × 8 CV, refineLevel 2 | ~12 ms (interpolated between 5.0 ms @40k and 23.9 ms @200k) | `G-storm-hair-look §5`, MEASURED at the endpoints |
| Storm points upload (9.6 MB) | ~1.2 ms | 0.13–0.22 ms/MB, same report |
| **Frame total** | **≈ 15 ms** | fits 60 Hz with ~1.5 ms of headroom |

Three readings drive the whole architecture:

1. **usdGen owns ~7 % of the frame.** Spending 3× more engineering on the evaluator buys ~0.7 ms;
   spending it on *drawing fewer curves* buys 6 ms. Hence: tiles with authored `extent` for frustum
   culling (S29), a decimation LOD (S31), and a hard rule that the engine never publishes a tile
   that did not change.
2. **A parameter edit must not cost a full chain run.** MEASURED: last-styler edit 0.18–0.41 ms vs
   full 1.72 ms vs sparse-chunk 0.035 ms (`G-data-plane §4`). Per-node buffers (S24) and per-chunk
   dirty bits (S23) are what buy that, and they are the reason the memory budget is ~6× the curve
   data.
3. **A brush stroke must cost the stroke, not the groom.** 21 µs of Python per move
   (`G-tool-loop §1.4`), 0.035 ms of engine, and one tile's re-upload — never the whole VBO.

### 0.3 The eight performance invariants (referenced as P1…P8 below)

| # | Invariant | Why (evidence) |
|---|---|---|
| **P1** | Per-CV data is **planar SoA** (`VtFloatArray px, py, pz`), interleaved to `VtVec3fArray` exactly once per publish. Per-*curve* data stays AoS. | 25.3 → 80.9 GFLOP/s for the same kernel (S22, `G-data-plane §6`); per-curve arrays are 1/8 the volume and are read randomly, where AoS wins. |
| **P2** | **Chunk = 512 curves, curve-aligned, with a uniform CV count inside a chunk.** One dirty byte per chunk per node. | S23; 128–1024 measured best, 4096 is a cliff (9.40 ms). Uniform CV count makes the inner loop index arithmetic, not a prefix-sum gather (ASSUMPTION: measured only indirectly). |
| **P3** | **Hair chunks never read other hair chunks.** All cross-curve influence flows through a *reference set* (guides, clump curves) evaluated in full, before its dependents, in a separate un-chunked lane. | Removes the unmeasured `UpstreamChunks` fan-in the data-plane report flagged as an open question; makes `tbb::parallel_for` over chunks provably race-free, the exact failure VDF's 500-element grain has (S21, `G-data-plane §2`). |
| **P4** | **Capture / evaluate split, with capture cached by a 128-bit epoch digest**, and every operator classified `restSpace | deformedSpace`. | S25; motion-blur cost becomes `H + k·T`, not `k·(H+T)` (`G-motion-blur §4.3`). |
| **P5** | **The engine chunk and the Hydra prim are different granularities**, related by an integer `chunksPerTile`. Tiles: 32–256 per description. | S23 wants 512-curve chunks for the DAG; S27 wants 32–256 prims for Storm. Both, not one. |
| **P6** | **Publish is a double-buffered generation swap, never a copy-on-write detach.** Only published (terminal, guide, instancer) buffers are double-buffered. | `VtArray` CoW publish is free (0.00005 ms) but the *next* edit pays one detach: 2.32 vs 1.97 ms for 9.6 MB (`G-data-plane §3.4, §4`). A/B buffers move that 0.35 ms off the interactive path at the cost of one extra terminal buffer. |
| **P7** | **Nothing cooks in `GetPrim`; nothing cooks in `_PrimsDirtied`.** Deferred commit, atomic snapshot publish, diffed dirties. | S17–S19; eager over-cooks 2–2.9×, lazy cooks on 4 worker threads and emits zero notices (`G-evaluation-scheduling §5`). |
| **P8** | **The DAG runs in a private `tbb::task_arena` capped at 8 workers by default.** | MEASURED: TBB DAG 3.90 ms (1 thread) → **1.02 ms (8)** → 1.79 ms (20) on this 10×X925 + 10×A725 host (`G-data-plane §3.3`). Isolation also stops usdGen from stealing Storm's `WorkParallelForN` workers during Rprim sync. |

---

## 1. Assumptions (decisions taken without a live user)

1. **ASSUMPTION — uniform CV count per chunk.** The generator emits chunks whose curves all have the
   same CV count; a chunk that would be ragged (imported/frozen curves with mixed counts) sets
   `cvCount = 0` and takes a slower indexed path. Rationale: the measured 2.84× vectorisation win
   (`G-data-plane §6`) depends on a contiguous inner loop with a compile-time-ish trip count; a
   per-curve offset gather defeats it. Every v1 generator in §7 already produces a uniform count.
2. **ASSUMPTION — 8 worker threads by default**, overridable by `USDGEN_THREAD_LIMIT`. Derived from
   the measured regression past 8–10 threads on this heterogeneous host; it is a *default*, and the
   gate in §11 re-derives it per machine.
3. **ASSUMPTION — root ordering is Morton-sorted at capture.** Curve slots are ordered by a 21-bit-
   per-axis Morton code over the rest-space root position, so a chunk is a spatial cluster. This is
   what makes per-tile `extent` tight enough for frustum culling to reject anything (S29 requires
   `extent`; nothing measured *tightness*), and what makes a brush footprint touch O(1) chunks.
   Stable ids are independent of the ordering (`id = hash64(seed, faceIndex, k)`, A7 §9.1 G1).
4. **ASSUMPTION — instance-prototype surface rebasing.** When a groom lives inside a propagated
   native prototype, `usdGen:surface` is rebased onto the prototype root using the ancestor's
   `__usdPrimInfo.niPrototypePath` and the prim's `primOrigin`. Discovery of those fields is MEASURED
   (`G-instancing §2`); the rebase arithmetic is mine and needs a test (gate T-INST-2).
5. **ASSUMPTION — the interactive LOD ladder is decimation by stable id.** `keep if hash32(id) <
   ratio·2^32`, so decimating never re-scatters and never changes ids. Cheap and stable; not measured
   against artist expectation.
6. **ASSUMPTION — a description's tile count is fixed for the life of the session** at
   `clamp(ceil(maxCurves / (chunkSize · chunksPerTile)), 32, 256)` computed from the *maximum*
   density the description can reach, not the current density. Required by S27/S28 ("the prim set is
   allocated once and never changes").
7. **ASSUMPTION — Storm binds the delegate-specific material by scene-index authoring, not by
   render-context resolution.** S36 records that "whether Storm prefers a `glslfx:` render-context
   output over plain `outputs:surface` is UNVERIFIED". usdGen therefore authors the binding it wants
   from the scene index, which knows the delegate. Gate L-1 flips this to render-context terminals if
   the check passes.
8. **ASSUMPTION — SeExpr and Ptex evaluation are always capture-time and never per-frame.** S37 says
   maps are baked at capture; I extend that to a hard rule: no operator may call an expression or a
   texture in `Evaluate`. Anything that must animate goes through an animated *scalar* parameter or a
   per-frame `UsdGenExprMap` re-capture, which is a capture-epoch bump.
9. **ASSUMPTION — the plugin is enabled by the presence of a `UsdGenGroom` prim**, checked once per
   `PrimsAdded` sweep, plus `USDGEN_ENABLE` (default on). A stage with no groom pays one pointer
   comparison per notice and zero per `GetPrim`.

---

## 2. The facts this design leans on hardest

Condensed; each is a settled decision or a measurement, with its source. This is the set a reviewer
should check first, because the architecture is invalid if any of them is wrong.

| # | Fact | Source |
|---|---|---|
| F1 | Renderer-level `HdSceneIndexPlugin`, phase 0 / `InsertionOrderAtEnd`, lands after the whole UsdImaging chain and before every Storm/hdPrman plugin. | S1, `G-chain-order §2–3` |
| F2 | Custom `usdGen:*` attributes and relationships are invisible to Hydra and emit **no notice** without an adapter. | S10, `G-stage-free §1, §5`; `G-freeze-bake §2.3` |
| F3 | Bespoke TBB DAG beats a persistent `VdfNetwork` 4× at 100k, 9–15× at 1M, 3–250× on sparse edits. | S21, `G-data-plane §0, §8` |
| F4 | SoA vs AoS for the same kernel: 80.9 vs 25.3 GFLOP/s. | S22, `G-data-plane §6` |
| F5 | Storm's upload unit is the prim; `points.size()` must equal `Σ curveVertexCounts` exactly or the prim renders fallback red. | S28, `G-storm-throughput §1.3–1.4` |
| F6 | Any element-count change reallocates the whole aggregated VBO and invalidates every batch drawing the groom. | `G-storm-throughput §1.6` |
| F7 | Frustum culling uses the prim's authored `extent`; with none, the prim is never culled. | S29, `G-storm-throughput §1.11` |
| F8 | 2 `PrimsDirtied` calls per frame regardless of scene size; `sceneGlobals/currentFrame` is the last one un-batched. | S18, `G-evaluation-scheduling §4, §9` |
| F9 | `GetPrim` must be threadsafe; observer callbacks need not be; 8 readers × 20 publishes = 0 torn reads with an atomic snapshot. | S19, `G-evaluation-scheduling §5, §7` |
| F10 | A frozen `BasisCurves` re-enters the graph straight from the scene index with `points/rest/skinprim/skinprimuv/usdGen:curveId` intact. | S42, `G-freeze-bake §2.2` |
| F11 | `VtArray` crosses a pxr_boost boundary in O(1) (0.13–0.18 µs at 1M CVs); `Vt.FromBuffer` costs 4.3 ms. | S39, `G-tool-loop §1.2` |
| F12 | A synthesized prim without `primOrigin` picks as an **empty** path. | S29/S40, `G-tool-loop §4`, `G-instancing §6` |
| F13 | Storm renders headlessly on this host through an EGL device context; the whole imaging half is regression-testable with GPU timings, no workstation. | S45, `G-storm-hair-look §0`, BUILD `probes/storm-hair-look/eglctx.h` |
| F14 | `HdSiExtComputationPrimvarPruningSceneIndex` privately wrapped is what makes UsdSkel scalps readable under Storm. | S3, `G-motion-blur §3.3` |

---

## 3. Library decomposition, naming and build (D8)

### 3.1 Targets

The split is chosen by **rebuild cost of the inner development loop**, not by conceptual tidiness.
MEASURED on this host: rebuilding all of `rigExecImaging` and relinking its dependents is **10.05 s**;
a single TU in `rigExec` costs **21.4 s** because pybind11's `-flto=auto` relink of the Python module
dominates (`B-usdrig-build`, key facts). So: kernels are a static library with a stable header, the
scene index is its own shared library, and the Python module links nothing that iterates often and is
built without LTO.

| CMake target | Kind | Contents | Links |
|---|---|---|---|
| `usdGenMath` | `STATIC` (PIC, `-ffp-contract=off`) | every numeric kernel: SoA styler kernels, arc-length/resample, RMF frames, transport, extent, noise façade over SeExpr's `Noise.h`, ramp evaluation, Morton codes, kd-tree façade over nanoflann, Poisson-disk relax, RBF solve | `arch tf gf vt work` · `TBB::tbb` · `usdGen_seexpr` · `nanoflann` · optional `rigExec::rigExecMath` |
| `usdGenSchema` | resource plugin | codeless `schema.usda`, generated `plugInfo.json` + `generatedSchema.usda` (S9) | — (plugInfo rewritten to `Type: library` pointing at `usdGenImaging` so `implementsComputeExtent` can load, the usdRig pattern, S44) |
| `usdGen` | `SHARED` | the graph/evaluator: `UsdGenGraph`, `UsdGenOp` + registry, `UsdGenCurveBuffer`, chunk/tile geometry, capture caches, digests, scheduler, the operator library, maps (image/Ptex/expr/paint), diagnostics | `usdGenMath` · `tf gf vt sdf work trace` · `TBB::tbb` · `usdGen_ptex` · `hio` (image read) |
| `usdGenImaging` | `SHARED` | `UsdGenImagingRegistry`, `UsdGenGroomSceneIndex`, the private pruning wrapper, `UsdGenGenerationStore`, publishers (tiles, guides, instancers, CV points), the UsdImaging prim/API adapters, `UsdGenMetadataSceneIndexPlugin`, the C ABI | `usdGen` · `hd hdsi usdImaging usdImagingGL(no) hf pxOsd` |
| `usdGenShaders` | resource-only | `usdGenHairPreview.glslfx`, `usdGenHairPreviewTranslucent.glslfx`, `shaderDefs.usda`, `usdGenHairMtlx.mtlx` | — |
| `_usdGen` | `MODULE` | pxr_boost.python array surface (S39) | `usdGenImaging` · `usd_boost usd_python vt gf tf` · `Python3::Module` |
| `usdGenUsdview` | python plugin dir | `PluginContainer`, panels, brushes, undo | — |
| `usdGenTestUtils` | `STATIC` | headless scene-index fixture builder, the EGL Storm harness (`eglctx.h`, BUILD), golden-image compare, a counter scraper | `usdGenImaging` · `hdSt hgiGL usdImagingGL` |
| `usdGen_seexpr`, `usdGen_ptex` | `STATIC`, `-fvisibility=hidden` | vendored SeExpr `main@8f8c8f2` (interpreter only) and Ptex 2.4.3 (S38) | `ZLIB::ZLIB`, `Threads::Threads` |
| `nanoflann` | `INTERFACE` | pinned 1.12.1 header (S38) | — |

Rule (A8 §6.2): third-party statics are **hidden inside** `libusdGen.so`; their headers and shared
objects are never installed next to USD's, or a site's Ptex-enabled `libusd_hdSt.so` would pick ours
through `$ORIGIN`.

### 3.2 CMake skeleton

```cmake
cmake_minimum_required(VERSION 3.26)
project(usdGen VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17 CACHE STRING "")            # must match the USD build

set(USD_INSTALL_DIR "${CMAKE_SOURCE_DIR}/../OpenUSD_26_08" CACHE PATH "")
list(APPEND CMAKE_PREFIX_PATH "${USD_INSTALL_DIR}")   # pxrConfig's find_dependency() needs this
if (NOT TARGET usd)                                   # S44: the CMake 3.28 double-pxrConfig trap
    find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)
endif()
find_package(rigExec CONFIG QUIET)                    # optional: rigExec::rigExecMath (S44)
if (TARGET rigExec::rigExecMath)
    set(USDGEN_HAVE_RIGEXEC_MATH ON)
endif()

# S45: the curvenet failure proves FMA contraction flips surface-walking branches.
target_compile_options(usdGenMath PRIVATE -ffp-contract=off)

# S44: generated library plugInfo, LibraryPath = $<TARGET_FILE_NAME:>, written into
# <build>/usd/<name>/resources so the same relative hop works in build and install trees.
usdgen_add_plugin_resources(usdGenImaging
    PLUGINFO plugin/usdGenImaging/resources/plugInfo.json.in
    DEST     usd/usdGenImaging/resources)
```

`usdGenConfig.cmake.in` wraps its own dependency in `if (NOT TARGET usd)` for exactly the reason
S44 records (`pxrConfig.cmake:58`'s unconditional `add_library(TBB::tbb SHARED IMPORTED)`), and
exports `usdGen_PLUGINPATHS`, `usdGen_LIBRARY_DIR`, `usdGen_PYTHON_DIR`.

An `USDGEN_IMAGING_DLL` env override exists for out-of-tree builds, because usdRig's
`ImagingLibraryPath()` only searches `<repo>/` and `<repo>/build/` and that bit the build agent
(S43, `B-usdrig-build` key facts).

### 3.3 Plugin registrations (exactly four)

| Plugin | Type | Registered as | Purpose |
|---|---|---|---|
| `usdGenSchema` | resource→library | codeless schema | S9 |
| `usdGenImaging` (a) | library | `HdSceneIndexPlugin`, `loadWithRenderer: ""`, phase 0 / `InsertionOrderAtEnd`, tags `["usdGen:groom"]`, `ordering: {after: ["hd:sceneGlobals"], before: ["hdGp:proceduralResolution", "hdPrman:motionBlur"]}` | S1, S2 — the evaluator |
| `usdGenImaging` (b) | library | `UsdImagingSceneIndexPlugin` returning its input unchanged | S7 — `InstanceDataSourceNames()` + `ProxyPathTranslationDataSourceNames()` only |
| `usdGenImaging` (c) | library | `UsdImagingSceneIndexPrimAdapter` per `usdGen*` type (`includeDerivedPrimTypes: true`) + a `UsdImagingAPISchemaAdapter` for `UsdGenRestAPI` | S10, S12 — parameter transport |
| `usdGenUsdview` | python | `pxr.Usdviewq.plugin.PluginContainer` | S43 |

Both the plugInfo JSON entry **and** a `TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)` call are shipped
for (a), because the registry composes JSON tags with the manufactured `phaseN` tags
(`G-evaluation-scheduling §3`).

---

## 4. Schema (D1)

### 4.1 Shape of the namespace

All prim types are **codeless** (S9), `usdGen:`-namespaced, and reach Hydra through the prim adapter
(S10, F2). Property naming is flat inside an operator and nested for reusable blocks
(`usdGen:mask:*`, `usdGen:<name>Ramp:*`), because `UsdImagingDataSourceMapped` turns `:` into nested
locators (`usdGen:mask:map` → `usdGen/mask/map`) and dirty routing keys on the locator prefix
(`G-stage-free §2`, MEASURED).

Type hierarchy (abstract types in *italics*):

```
UsdTyped
 └─ UsdGenNode  (abstract)                     rel usdGen:input, readPhase, enabled, seed, space
     ├─ UsdGenOperator (abstract)              + UsdGenMaskAPI block, usdGen:amount
     │   ├─ UsdGenGenerator (abstract)         topology-creating
     │   │   ├─ UsdGenScatterRandom / ScatterUniform / ScatterPoints / ScatterAtGuides
     │   │   ├─ UsdGenGrow
     │   │   ├─ UsdGenGuideInterpolate
     │   │   └─ UsdGenCurveSource               (frozen / imported curves re-entering, S42)
     │   ├─ UsdGenStyler (abstract)            topology-preserving
     │   │   └─ Clump | Noise | Curl | Bend | Direction | Length | Width | Smooth |
     │   │      Straighten | Displace | Wave | Collide | Resample | Wind | SculptLayer
     │   └─ UsdGenDeformer (abstract)
     │       └─ UsdGenDeformWithSurface
     ├─ UsdGenPrimitive (abstract)             terminal "what do the curves become"
     │   └─ UsdGenSplines | UsdGenCards | UsdGenArchives | UsdGenSpheres
     └─ UsdGenMap (abstract)
         └─ UsdGenImageMap | UsdGenPtexMap | UsdGenExprMap | UsdGenPaintMap
UsdGeomImageable
 ├─ UsdGenGroom                                 the container / session root
 └─ UsdGenDescription                           one chain + one output prim set
API schemas: UsdGenMaskAPI (on operators), UsdGenRestAPI (on surfaces), UsdGenGroomBindingAPI
```

### 4.2 Core properties

**`UsdGenNode` (abstract).**

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` | — | Ordered upstream node(s). The wiring (S26). ≥1 target for stylers, 0 for generators. |
| `usdGen:enabled` | `uniform bool` | `true` | Disabled ⇒ pass-through (usdRig's failure semantics, A1 §5). Structural: bumps the digest. |
| `usdGen:seed` | `uniform int` | `0` | Feeds every `hash64(seed, …)`. Structural for capture, not for evaluate. |
| `usdGen:readPhase` | `uniform token` | `"final"` | `base` (rest surface at `Default()`, S12) · `preceding` (surface before usdGen's own overlays on that prim) · `final` (after them) · `@<primPath>` (a named prim, e.g. a low-res proxy scalp). S26. |
| `usdGen:space` | `uniform token` | `"auto"` | `rest` · `deformed` · `auto`. Overrides the operator's own class; drives the motion tail (S25, P4). |
| `usdGen:label` | `uniform string` | `""` | UI only, excluded from every digest. |

**`UsdGenOperator` adds:** `float usdGen:amount = 1.0` (envelope, exact endpoints at 0 and 1 — the
`RigExecBlendEnvelope` contract, A1 §5) and the mask block.

**`UsdGenMaskAPI`** (one block, on every operator — A7 §7's recommendation):

| Property | Type | Default |
|---|---|---|
| `usdGen:mask:scale` | `float` | `1.0` |
| `usdGen:mask:map` | `rel` | — (→ a `UsdGenMap` prim) |
| `usdGen:mask:channel` | `uniform token` | `"r"` |
| `usdGen:mask:expr` | `string` | `""` (SeExpr; capture-time only, ASSUMPTION 8) |
| `usdGen:mask:random` | `float` | `0.0` (per-curve random multiplier amount) |
| `usdGen:mask:randomSeed` | `uniform int` | `0` |
| `usdGen:mask:combine` | `uniform token` | `"multiply"` (`multiply add subtract max min average`) |
| `usdGen:mask:ramp:knots` | `float2[]` | `[]` — along-curve ramp, `(position, value)` |
| `usdGen:mask:ramp:interpolation` | `uniform token` | `"linear"` (`constant linear smooth bezier`) |
| `usdGen:mask:ramp:tangents` | `float[]` | `[]` (2 per knot, `bezier` only) |

**Ramps** everywhere follow that shape: `float2[] …:knots` (S11's knot arrays), optional
`float[] …:values` for vector/colour ramps where the value does not fit in the `float2`
(this is S11's `values` array), and a `uniform token …:interpolation`. A whole `TsSpline` may be
authored instead on `float usdGen:<name>Ramp:spline` and is transported by the adapter as
`HdTypedSampledDataSource<TsSpline>` **without** the time-varying flag (S11, MEASURED
`G-stage-free §2.1`). A plain `.spline` on a *parameter* is legal and means "this scalar animates";
a `.spline` on a ramp position is a compile error, because it would dirty every frame.

**`UsdGenGroom`** (`UsdGeomImageable`) — the session root and the tuning surface:

| Property | Type | Default | Notes |
|---|---|---|---|
| `usdGen:surface` | `rel` | — | Default surface for descendants. |
| `usdGen:chunkSize` | `uniform int` | `512` | S23; clamped to [128, 1024]. |
| `usdGen:tileTarget` | `uniform int` | `64` | S27; clamped to [32, 256]. |
| `usdGen:threadLimit` | `uniform int` | `0` | 0 = `USDGEN_THREAD_LIMIT` or 8 (P8). |
| `usdGen:memoryBudgetMB` | `uniform int` | `2048` | §5.9. |
| `usdGen:motion:mode` | `uniform token` | `"single"` | `single` · `velocities` · `samples` (S32 P0/P1/P2). |
| `usdGen:motion:sampleCount` | `uniform int` | `3` | 2…16 (S32). |
| `usdGen:motion:forwardSurfaceSamples` | `uniform bool` | `false` | else clamp to the shutter (`G-motion-blur §4.2`). |
| `usdGen:lod:ratio` | `float` | `1.0` | Interaction decimation (S31); animatable so a shot can dial it. |
| `usdGen:progressive` | `uniform bool` | `false` | opt into `asyncAllow`/`asyncPoll` (S20). |
| `usdGen:diagnostics` | `uniform token` | `"off"` | `off · counters · trace`. |

**`UsdGenDescription`** (`UsdGeomImageable`) — one chain, one output prim set:

| Property | Type | Default | Notes |
|---|---|---|---|
| `usdGen:terminal` | `rel` | — | The last node of the chain (exactly one target). |
| `usdGen:surface` | `rel` | — | Overrides the groom's. |
| `usdGen:primitive` | `rel` | — | A `UsdGenPrimitive` prim; absent ⇒ `UsdGenSplines` defaults. |
| `usdGen:maxCurves` | `uniform int` | `0` | 0 ⇒ derived from the generator at first compile; fixes the tile count for the session (ASSUMPTION 6). |
| `usdGen:renderDensity` | `float` | `1.0` | Multiplier applied only when `sceneGlobals` says we are not interactive (XGen's Render Density Multiplier, A7 §8). |
| `usdGen:guideVisibility` | `uniform token` | `"guide"` | purpose of the guide prim set (S27). |
| `material:binding` | `rel` | — | Standard. §9 explains the three-terminal material. |

**`UsdGenRestAPI`** (applied to the surface): `uniform token usdGen:rest:source = "default"`
(`default` · `primvar` · `asset`), `token usdGen:rest:primvar = "rest"`,
`asset usdGen:rest:file`. The adapter data source samples `UsdTimeCode::Default()` and honours an
authored `primvars:rest` when present (S12, MEASURED `G-stage-free §3`).

### 4.3 Wiring, ordering, and what counts as structural

* Order is a **Kahn topological sort over `usdGen:input`**, namespace order as tie-break; cycles are
  compile errors with the offending path pair in the message (S26).
* The **structural digest** is a Merkle hash: `d(n) = H(type, enabled, seed, readPhase, space,
  sorted(paths of usdGen:input, usdGen:guides, usdGen:mask:map, …), sorted(d(children)))`. Animated
  scalars are excluded (S26). Because it is a Merkle hash, a change deep in the chain leaves every
  prefix digest identical and only the suffix recompiles (§5.3).
* **Capture epoch** per node is a second 128-bit digest over exactly what `Capture()` reads: the
  surface's topology hash and rest points hash, the seed, every capture-time parameter, every map's
  resolved asset path + texture generation counter (S13), and the upstream node's capture epoch.
* **Value version** per parameter is a monotone counter bumped by the dirty router.

Three digests, three consequences: a *label* edit costs nothing; a *clump amount* edit bumps one
value version and dirties one node's chunks; a *seed* edit bumps a capture epoch and re-captures
that node and everything downstream of it, but does **not** recompile the graph.

### 4.4 Versioning

`uniform int usdGen:schemaVersion` on `UsdGenGroom`, default `1`, written by the tools. The
evaluator refuses (with one `TF_WARN` per groom, then pass-through) a version greater than it knows.
Property additions inside a major version must be optional with a fallback; a removal or a semantic
change bumps the major. Frozen curves carry `primvars:usdGen:frozenEpoch` (S42) which encodes the
schema version in its prefix (`usdgen1:sha1:…`), so a freeze made by a newer plugin is *stale*, not
*silently wrong*.

### 4.5 Example (a) — generate-and-style on a usdRig-deformed scalp

`scatter → guide interpolate → clump → clump → frizz`, the chain the request names (R4).

```usda
#usda 1.0
(
    defaultPrim = "Character"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "Character"
{
    # ---- the scalp: deformed by usdRig (or UsdSkel, or anything else -- R5, S3) -------------
    def Mesh "Scalp" (
        prepend apiSchemas = ["UsdGenRestAPI"]
    )
    {
        int[]        faceVertexCounts  = [4, 4, 4]
        int[]        faceVertexIndices = [0, 1, 2, 3,  1, 4, 5, 2,  4, 6, 7, 5]
        point3f[]    points            = [(0,0,0), (1,0,0), (1,1,0), (0,1,0), (2,0,0), (2,1,0), (3,0,0), (3,1,0)]
        texCoord2f[] primvars:st       = [...] ( interpolation = "faceVarying" )
        uniform token usdGen:rest:source = "default"   # rest == the Default()-time points (S12)
        # RigExecResultsSceneIndex replaces primvars/points downstream; the rest source does not
        # go through it, so `base` reads still work (G-stage-free 3, R2).
    }

    def BasisCurves "GuideCurves" ( purpose = "guide" )
    {
        uniform token type  = "cubic"
        uniform token basis = "bspline"
        uniform token wrap  = "pinned"
        int[]     curveVertexCounts = [8, 8, 8, 8]
        point3f[] points            = [ ... 32 CVs ... ]
        int[]        primvars:skinprim   = [0, 0, 1, 2] ( interpolation = "uniform" )
        texCoord2f[] primvars:skinprimuv = [(0.2,0.3), (0.7,0.4), (0.5,0.5), (0.1,0.9) ] ( interpolation = "uniform" )
    }

    def UsdGenGroom "Groom"
    {
        rel usdGen:surface       = </Character/Scalp>
        uniform int   usdGen:chunkSize   = 512
        uniform int   usdGen:tileTarget  = 64
        uniform token usdGen:motion:mode = "single"
        float         usdGen:lod:ratio   = 1.0

        # ---- maps (capture-time only, S37) -------------------------------------------------
        def UsdGenImageMap "DensityMap"
        {
            asset         usdGen:file      = @./maps/scalp_density.exr@
            uniform token usdGen:uvPrimvar = "st"
            uniform token usdGen:channel   = "r"
            uniform token usdGen:wrap      = "clamp"
            uniform token usdGen:colorSpace = "raw"
        }
        def UsdGenExprMap "LengthVariation"
        {
            string usdGen:expression = "fit(fbm($Pref * 3, 3, 2, 0.5), -0.5, 0.5, 0.75, 1.25)"
            uniform token usdGen:domain = "root"      # root | cv
        }
        def UsdGenPtexMap "ClumpMask"
        {
            asset         usdGen:file    = @./maps/clumpmask.ptx@
            uniform token usdGen:filter  = "bilinear"
            uniform token usdGen:channel = "r"
        }

        # ---- guides ------------------------------------------------------------------------
        def UsdGenCurveSource "Guides"
        {
            rel           usdGen:curves   = </Character/GuideCurves>
            uniform token usdGen:role      = "reference"   # a reference set (P3), never chunked
            uniform int   usdGen:cvCount   = 8             # rebuild guides to a common CV count (A7 1.2)
        }

        # ---- the chain ---------------------------------------------------------------------
        def UsdGenScatterRandom "Scatter"
        {
            float         usdGen:density         = 120000     # per unit^2 of rest surface
            uniform int   usdGen:seed            = 7
            uniform int   usdGen:relaxIterations = 0
            uniform bool  usdGen:areaCompensation = true
            rel           usdGen:mask:map        = </Character/Groom/DensityMap>
            float         usdGen:mask:scale      = 1.0
            uniform token usdGen:readPhase       = "base"     # scatter is a REST-space operator
        }

        def UsdGenGuideInterpolate "Interp"
        {
            rel           usdGen:input             = </Character/Groom/Scatter>
            rel           usdGen:guides            = </Character/Groom/Guides>
            uniform int   usdGen:cvCount           = 8
            uniform int   usdGen:maxGuides         = 3
            float         usdGen:influenceRadius   = 3.0
            float         usdGen:influenceDecay    = 2.0
            float         usdGen:maxGuideAngle     = 90
            float         usdGen:blendInSkinSpace  = 1.0
            uniform token usdGen:blendMethod       = "extrudeAndBlend"
            uniform bool  usdGen:useUniqueGuide    = false
            float         usdGen:randomizeGuide    = 0.0
            rel           usdGen:mask:map          = </Character/Groom/LengthVariation>
            uniform token usdGen:mask:combine      = "multiply"
        }

        def UsdGenClump "ClumpBig"
        {
            rel        usdGen:input       = </Character/Groom/Interp>
            float      usdGen:clump       = 0.85
            float      usdGen:clumpSize   = 1.4
            uniform int usdGen:clumpSeed  = 11
            float2[]   usdGen:clumpRamp:knots = [(0, 0), (0.25, 0.35), (1, 1)]
            uniform token usdGen:clumpRamp:interpolation = "smooth"
            float      usdGen:stray:amount = 0.15
            float      usdGen:stray:rate   = 0.05
            float      usdGen:volumize     = 0.1
            float      usdGen:preserveLength = 1.0
            rel        usdGen:mask:map     = </Character/Groom/ClumpMask>
        }

        def UsdGenClump "ClumpSmall"
        {
            rel        usdGen:input       = </Character/Groom/ClumpBig>
            float      usdGen:clump       = 0.6
            float      usdGen:clumpSize   = 0.35
            uniform int usdGen:clumpSeed  = 12
            float2[]   usdGen:clumpRamp:knots = [(0, 0), (0.5, 0.8), (1, 1)]
            float      usdGen:goalFeedback = 1.0        # derive level-2 clump curves from ClumpBig's output
        }

        def UsdGenNoise "Frizz"
        {
            rel        usdGen:input     = </Character/Groom/ClumpSmall>
            float      usdGen:magnitude = 0.035
            float      usdGen:frequency = 6.0
            float      usdGen:correlation = 0.35
            uniform int usdGen:octaves  = 3
            uniform token usdGen:noiseSpace = "rest"     # follows the surface for free
            float2[]   usdGen:magnitudeRamp:knots = [(0, 0), (0.3, 0.2), (1, 1)]
            float      usdGen:preserveLength = 1.0
        }

        def UsdGenDeformWithSurface "Deform"
        {
            rel           usdGen:input   = </Character/Groom/Frizz>
            uniform token usdGen:mode    = "rigidFrame"
            uniform bool  usdGen:twistAware = true
            uniform token usdGen:space   = "deformed"    # everything from here is the motion tail (P4)
        }

        def UsdGenSplines "Hair"
        {
            uniform int   usdGen:cvCount        = 8
            float         usdGen:width          = 0.006
            float2[]      usdGen:widthRamp:knots = [(0, 1), (0.8, 0.9), (1, 0.15)]
            uniform token usdGen:basis          = "bspline"
        }

        def UsdGenDescription "MainHair"
        {
            rel usdGen:terminal  = </Character/Groom/Deform>
            rel usdGen:primitive = </Character/Groom/Hair>
            rel usdGen:surface   = </Character/Scalp>
            uniform int usdGen:maxCurves = 140000
            rel material:binding = </Character/Looks/HairMtl>
        }
    }
}
```

What the scene index synthesizes from that, and nothing else:

```
/Character/Groom/MainHair/Curves/tile_0000 … tile_0048    basisCurves  (S27, S29)
/Character/Groom/MainHair/Guides/tile_0000                basisCurves, purpose = guide
```

Each tile carries `points widths hairT hairTangent hairId st displayColor`, `extent`,
`primOrigin{scenePath=/Character/Groom/MainHair}`, `displayStyle/refineLevel=2`,
`minScreenSpaceWidths=1.0`, blocked `velocities`/`accelerations`, `xform` = the scalp's world matrix
with `resetXformStack=true`, points in scalp-local space (S4, S29).

### 4.6 Example (b) — frozen curves deformed with the surface, plus a sculpt layer

A freeze (S42) is a plain `UsdGeomBasisCurves`; the operator that points at it is what is new. Note
that the freeze is **a sibling under a dedicated scope** and the tooling never re-authors that
scope's spec (S41, `G-freeze-bake §4.2`: touching the parent's `typeName` resyncs 203 prims).

```usda
def UsdGenGroom "Groom"
{
    rel usdGen:surface = </Character/Scalp>

    # ---- the freeze: authored by the tool into the session layer (T1) or a sidecar .usdc (T2) --
    def Scope "__UsdGenFrozen"
    {
        def BasisCurves "MainHair_freeze_003"
        {
            uniform token type  = "cubic"
            uniform token basis = "bspline"
            uniform token wrap  = "pinned"
            int[]      curveVertexCounts = [ ... 100000 entries, all 8 ... ]
            point3f[]  points            = [ ... 800000 CVs ... ]
            float[]    primvars:widths   = [ ... ] ( interpolation = "vertex" )

            point3f[]    primvars:rest        = [ ... ] ( interpolation = "vertex" )   # 26 bytes while it shares points (S42)
            int[]        primvars:skinprim    = [ ... ] ( interpolation = "uniform" )
            texCoord2f[] primvars:skinprimuv  = [ ... ] ( interpolation = "uniform" )
            int[]        primvars:usdGen:curveId = [ ... ] ( interpolation = "uniform" )
            string       primvars:usdGen:frozenEpoch = "usdgen1:sha1:9f2c4e…" ( interpolation = "constant" )
            matrix4d[]   primvars:usdGen:rootFrame = [ ... ] ( interpolation = "uniform" )  # optional
        }
    }

    def UsdGenCurveSource "Frozen"
    {
        rel           usdGen:curves      = </Character/Groom/__UsdGenFrozen/MainHair_freeze_003>
        uniform token usdGen:role        = "curves"
        uniform token usdGen:staleAction = "warn"       # warn | ignore | block
        uniform string usdGen:expectEpoch = "usdgen1:sha1:9f2c4e…"
    }

    def UsdGenSculptLayer "Comb01"
    {
        rel        usdGen:input   = </Character/Groom/Frozen>
        float      usdGen:weight  = 1.0
        uniform token usdGen:deltaSpace = "rootFrame"   # rootFrame | rest | world
        int[]      usdGen:deltaCurveIds = [ 12, 4001, 4002, ... ]   # sparse, keyed by stable id
        int[]      usdGen:deltaOffsets  = [ 0, 8, 16, ... ]
        vector3f[] usdGen:deltaValues   = [ ... ]                   # per-CV deltas for those ids
        uniform string usdGen:baseEpoch = "usdgen1:sha1:9f2c4e…"
    }

    def UsdGenDeformWithSurface "Deform"
    {
        rel           usdGen:input      = </Character/Groom/Comb01>
        uniform token usdGen:mode       = "rigidFrame"
        uniform bool  usdGen:twistAware = true
        float         usdGen:preserveShape = 0.0        # Cosserat iterations, v2
        uniform token usdGen:space      = "deformed"
        uniform token usdGen:readPhase  = "final"       # post-usdRig / post-UsdSkel points (S3, R5)
    }

    def UsdGenDescription "MainHair"
    {
        rel usdGen:terminal  = </Character/Groom/Deform>
        rel usdGen:surface   = </Character/Scalp>
        uniform int usdGen:maxCurves = 100000
    }
}
```

Engine consequences, all load-bearing:

* `UsdGenCurveSource` is a **source node whose buffer is loaded from the scene index instead of
  computed** (S24, S42): `Capture()` pulls `points/rest/skinprim/skinprimuv/usdGen:curveId` from
  `_GetInputSceneIndex()->GetPrim(...)` — no `UsdStage` (F10, S8) — and re-lays them into the SoA
  chunk arena. That pull is **flat in curve count and under 0.6 ms end to end** at 10k/100k/1M
  (MEASURED, `G-freeze-bake §4.1`).
* `usdGen:deltaCurveIds` are matched to the current curve slots by a sorted-id binary search built
  once per capture epoch; ids that no longer exist are retained and ignored (A7 §9.3).
* The `frozenEpoch` mismatch check is a string compare on a **constant primvar**, the only channel
  that both reaches Hydra and dirties precisely (S42, MEASURED `G-freeze-bake §2.3`).
* Only `Deform` is `deformedSpace`; `Frozen` and `Comb01` are `restSpace`, so a frame change re-runs
  one node over 800k CVs (≈0.2–0.5 ms), not three (P4).

### 4.7 Example (c) — cards / archives instancer

Cards and archives are emitted as **real `instancer` prims with prototypes as namespace children**
(S33, `G-instancing §7`), synthesized downstream with hand-authored `instancedBy`.

```usda
def UsdGenGroom "Groom"
{
    rel usdGen:surface = </Character/Scalp>

    def UsdGenScatterRandom "CardRoots" { float usdGen:density = 4000  uniform int usdGen:seed = 3 }
    def UsdGenGrow "CardOrient"
    {
        rel usdGen:input = </Character/Groom/CardRoots>
        uniform int usdGen:segments = 2
        float usdGen:length = 0.12
        float usdGen:lift   = 25
    }

    def UsdGenCards "Cards"
    {
        rel           usdGen:input        = </Character/Groom/CardOrient>
        rel           usdGen:prototypes   = [ </Character/Groom/CardLib/Card_A>,
                                              </Character/Groom/CardLib/Card_B> ]
        uniform token usdGen:protoSelect  = "random"      # random | byMap | byClumpId
        rel           usdGen:protoSelect:map = </Character/Groom/RegionMap>
        float         usdGen:width        = 0.09
        float         usdGen:widthVariance = 0.25
        float         usdGen:lengthVariance = 0.2
        float         usdGen:tilt          = 0.0
        float         usdGen:twistJitter   = 12.0
        uniform bool  usdGen:faceCamera    = false
        uniform token usdGen:space         = "deformed"
    }

    def Scope "CardLib"
    {
        def Mesh   "Card_A" { ... a quad, material:binding = </Character/Looks/CardA> ... }
        def Xform  "Card_B" ( prepend references = @./archives/tuft_b.usdc@ ) { }   # an ARCHIVE: a subtree
    }

    def UsdGenDescription "Fuzz"
    {
        rel usdGen:terminal  = </Character/Groom/Cards>
        rel usdGen:primitive = </Character/Groom/Cards>
        uniform int usdGen:maxCurves = 8000
    }
}
```

Synthesized (S33, verified shapes from `G-instancing §1.1, §2, §7`):

```
/Character/Groom/Fuzz/Instancer                       instancer
    instancerTopology/prototypes      = [ …/Instancer/Prototypes/Card_A, …/Prototypes/Card_B ]
    instancerTopology/instanceIndices = [ VtIntArray, VtIntArray ]   (HdIntArrayVectorSchema)
    instancerTopology/mask            = []                            (empty == all true)
    primvars/hydra:instanceTranslations  (instance)   ← the interactive channel (S33)
    primvars/hydra:instanceRotations     (instance)
    primvars/hydra:instanceScales        (instance)
    primvars/displayColor                (instance)   ← baked per-card colour, arbitrary primvars are unfiltered
    primOrigin{scenePath = /Character/Groom/Cards}
/Character/Groom/Fuzz/Instancer/Prototypes/Card_A     mesh, instancedBy{paths=[…/Instancer],
                                                                       prototypeRoots=[…/Prototypes/Card_A]}
/Character/Groom/Fuzz/Instancer/Prototypes/Card_B/…   subtree, each prim instancedBy the same
```

Material variety comes from **multiple prototypes with partitioned `instanceIndices`** — Storm has
no per-instance material (S33, `G-instancing §5`). Continuous variety (tint, root/tip, melanin) rides
on `instance`-interpolated primvars, which Storm passes through unfiltered
(`hdSt/primUtils.cpp:211-221`). Interactive card edits dirty
`primvars/hydra:instanceTranslations` only, never `instancerTopology` (S33, `G-instancing §8`) —
`DirtyPrimvar` re-uploads one BAR instead of rebuilding indices for every prototype rprim.

---

## 5. The engine (D2)

### 5.1 Data model: the chunk arena

```cpp
// usdGen/types.h
using UsdGenNodeId    = uint32_t;   // dense, assigned by the compiler in topological order
using UsdGenChunkId   = uint32_t;   // dense within a description
using UsdGenTileId    = uint16_t;
using UsdGenSurfaceId = uint16_t;   // index into UsdGenSurfaceTable
using UsdGenEpoch     = std::array<uint64_t, 2>;   // 128-bit digest

enum class UsdGenSpace   : uint8_t { Inherit, Rest, Deformed };
enum class UsdGenTopoFx  : uint8_t { None, CurveCount, CvCount, Both };
enum class UsdGenRole    : uint8_t { Curves, Reference, Roots };  // P3
```

```cpp
// usdGen/curveBuffer.h
struct UsdGenChunkDesc {
    uint32_t firstCurve;    // into the per-curve arrays
    uint32_t curveCount;    // <= chunkSize
    uint32_t firstCv;       // into the per-CV arrays
    uint32_t cvCount;       // CVs per curve; 0 == ragged chunk (uses cvOffsets)
    UsdGenTileId    tile;
    UsdGenSurfaceId surface;
    GfRange3f       boundsRest;   // filled at capture, used for brush/footprint rejection
};

/// One node's output. Per-CV data is planar SoA (P1); per-curve data is AoS.
struct UsdGenCurveBuffer {
    // ---- per CV (hot, vectorised) -------------------------------------------------
    VtFloatArray px, py, pz;           // size == totalCvs
    VtFloatArray width;                // optional; empty means "inherit upstream"
    VtFloatArray hairT;                // root->tip parameter, filled once by the generator
    std::vector<VtFloatArray> extraCv; // named by UsdGenCurveBufferLayout

    // ---- per curve (cold, random access) ------------------------------------------
    VtIntArray   curveId;              // stable, hash64(seed, face, k) truncated
    VtIntArray   rootPrim;             // surface face index
    VtVec2fArray rootUV;
    VtVec3fArray rootT, rootN, rootB;  // rest root frame (AoS: 36 B/curve, read once per curve)
    VtIntArray   cvOffsets;            // ragged chunks only
    VtFloatArray curveMask;            // resolved per-curve mask for the owning node

    // ---- geometry -----------------------------------------------------------------
    std::vector<UsdGenChunkDesc> chunks;
    uint32_t totalCurves = 0, totalCvs = 0;
    uint64_t topologyVersion = 0;      // bumped when curveCount / cvCount change
};

/// A view a kernel is allowed to touch. All pointers are chunk-local bases.
struct UsdGenChunkView {
    const UsdGenChunkDesc *desc;
    float *px, *py, *pz;               // out: writable; in: const-cast never
    const float *width, *hairT;
    const int   *curveId, *rootPrim;
    const GfVec2f *rootUV;
    const GfVec3f *rootT, *rootN, *rootB;
    const float *curveMask;            // already resolved (map x expr x random x ramp scale)
    uint32_t curveCount, cvCount;
    inline size_t Cv(uint32_t curve, uint32_t i) const { return size_t(curve) * cvCount + i; }
};
```

**Why this exact layout.**

* `px/py/pz` planar is worth 3.2× on the styler kernels (F4, MEASURED). The `Cv()` helper is
  multiplication, not a lookup, precisely because of ASSUMPTION 1.
* `hairT` is stored, not recomputed, because it is also a **mandatory published primvar** (S29) and
  computing `i/(n-1)` in both places is a divergence risk. 3.2 MB at 800k CVs.
* `rootT/N/B` as three AoS `VtVec3fArray` is 36 B/curve = 3.6 MB at 100k curves — 1/8 the volume of
  the CV data and read exactly once per curve, so planar buys nothing there.
* Everything is a `VtArray` so the boundary to Hydra and to Python is a refcount bump (F11, S39).
* `curveMask` is resolved **once per node per capture** and reused across frames; every kernel then
  reads one float per curve instead of re-evaluating a map/expr (ASSUMPTION 8, S37).

**Chunk and tile geometry (P2, P5).**

```
chunkSize      = usdGen:chunkSize                     (default 512, [128,1024])
nChunks        = ceil(maxCurves / chunkSize)
chunksPerTile  = max(1, ceil(nChunks / usdGen:tileTarget))
nTiles         = clamp(ceil(nChunks / chunksPerTile), 32, 256)
```

At 100 000 curves: 196 chunks, `chunksPerTile = 4`, **49 tiles of 2048 curves**. Each tile's
published `points` array is exactly `Σ curveVertexCounts` over its live curves (F5, S28) and is
written by interleaving its chunks' planar data into one `VtVec3fArray`.

### 5.2 The operator interface

```cpp
// usdGen/op.h
class UsdGenOp {
public:
    virtual ~UsdGenOp();

    // ---- static description (compile time) ----------------------------------------
    virtual TfToken             Type()      const = 0;
    virtual UsdGenSpace         Space()     const { return UsdGenSpace::Rest; }
    virtual UsdGenTopoFx        Topology()  const { return UsdGenTopoFx::None; }
    virtual TfSpan<const TfToken> ParameterNames()  const = 0;   // dirty routing (S10)
    virtual TfSpan<const TfToken> CaptureParameters() const = 0; // subset that bumps the epoch
    virtual TfSpan<const TfToken> ReferenceInputs() const { return {}; }  // P3

    // ---- binding: data sources -> a POD parameter block ----------------------------
    virtual bool Bind(const UsdGenParamView &params,
                      UsdGenDiagnostics *diag) = 0;

    // ---- capture: topology-dependent, cached by epoch (P4, S25) ---------------------
    virtual UsdGenEpoch CaptureDigest(const UsdGenCaptureContext &ctx) const = 0;
    virtual bool        Capture(const UsdGenCaptureContext &ctx,
                                const UsdGenCurveBuffer     &upstream,
                                UsdGenCapture               *out,
                                UsdGenDiagnostics           *diag) = 0;

    // ---- evaluate: per frame, per chunk, parallel, no allocation -------------------
    virtual void Evaluate(const UsdGenEvalContext &ctx,
                          const UsdGenCapture     &cap,
                          const UsdGenChunkView   &in,
                          UsdGenChunkView         *out) const = 0;

    // ---- generators only ------------------------------------------------------------
    virtual bool GenerateTopology(const UsdGenCaptureContext &ctx,
                                  UsdGenCurveBuffer *out,
                                  UsdGenDiagnostics *diag) { return false; }
};

class UsdGenOpRegistry {
public:
    static UsdGenOpRegistry &GetInstance();
    template <class T> void Register(const TfToken &type);
    std::unique_ptr<UsdGenOp> Create(const TfToken &type) const;
    bool IsKnown(const TfToken &type) const;
};
#define USDGEN_REGISTER_OP(cls, token) \
    TF_REGISTRY_FUNCTION(UsdGenOpRegistry) { \
        UsdGenOpRegistry::GetInstance().Register<cls>(TfToken(token)); }
```

Contract rules, all enforced by a debug-build wrapper (`USDGEN_OP_CHECKS=1`):

| Rule | Rationale |
|---|---|
| `Evaluate` must be `const`, must not allocate, must not touch a `UsdStage`, must not read outside `in`/`cap`/`ctx`. | S8; the chunk loop is `tbb::parallel_for` and allocation there is the classic scaling killer. |
| `Evaluate` writes only `out`'s CV range for its own chunk. | The exact rule VDF's 500-element grain violates (S21, `G-data-plane §2`) — quadratic and a data race there; free to get right here. |
| Cross-curve reads come from `cap` (capture-resolved indices) plus a **reference buffer**, never from another chunk. | P3. |
| `Capture` may allocate and may take the graph mutex; it runs on the commit thread or under `WorkParallelForN` over nodes with no shared state. | S17/S18. |
| Kernels live in `usdGenMath`, are free functions over raw pointers, and are compiled `-ffp-contract=off`. | S22, S45 (the curvenet FMA failure). |

`UsdGenCapture` is a small type-erased holder:

```cpp
struct UsdGenCapture {
    UsdGenEpoch  epoch;
    size_t       bytes = 0;                   // for the memory budget (5.9)
    VtIntArray   i0, i1, i2;                  // e.g. guideIdx[3] / clumpId per level
    VtFloatArray f0, f1, f2;                  // e.g. guideW[3], restLengths
    std::shared_ptr<const UsdGenKdTree> tree; // shared between siblings with the same epoch
    std::shared_ptr<void> extra;              // operator-private
};
```

Deliberately concrete: the two things every cross-curve operator needs are *k indices and k weights
per curve*, and they are planar `VtArray`s so the evaluate loop streams them.

### 5.3 Compilation, digests and sub-graph recompiles

```cpp
class UsdGenGraph {
public:
    struct CompileResult {
        bool                       ok = false;
        UsdGenEpoch                structuralDigest {};
        std::vector<UsdGenNodeId>  rebuilt;     // nodes whose UsdGenOp was recreated
        std::vector<UsdGenNodeId>  reordered;   // nodes whose topological index moved
        UsdGenDiagnostics          diag;
    };
    CompileResult Compile(const UsdGenSceneView &scene);
    ...
};
```

`UsdGenSceneView` is a thin read-only façade over the *scene index* (never a stage, S8) that answers
`GetGroomPrims()`, `GetOperatorParams(path)`, `GetSurface(path, readPhase)`. `Compile` walks it and:

1. Collects `usdGen*` prims under each `UsdGenGroom` from the notice-maintained path set (no full
   traversal — a full traversal is what makes usdview's freeze cost O(stage prims),
   `G-freeze-bake §4.3`).
2. Computes the **Merkle structural digest** bottom-up (§4.3). Node digests are memoised from the
   previous compile keyed by prim path.
3. Diffs digests: nodes whose digest is unchanged keep their `UsdGenOp`, their capture cache, and
   their output buffer. Only the changed nodes and their descendants are recreated.
4. Kahn-sorts, detects cycles with an iterative colour DFS (the pattern usdRig uses,
   `rigEvaluator.cpp:3028-3050`, A1 §10.2), and assigns dense `UsdGenNodeId`s in topological order.
5. Rebuilds the **dirty routing table** (§5.4).

Cost model: the digest is a hash over ≤30 tokens per node; a 200-node groom recompiles in
**O(200 × ~200 ns) ≈ 40 µs** (ASSUMPTION — extrapolated from usdRig's digest cost, A1 §3.3; gate
E-6 measures it). Contrast: `VdfScheduler::Schedule` costs ~1.4 µs/node *plus* an `elements/500`
partitioning term — 2.93–4.39 ms for 1000 styler nodes (MEASURED, `G-data-plane §3.5`). Having no
scheduling phase at all is one of the reasons S21 chose this engine.

**Sub-graph recompile is the whole point.** Adding a `Frizz` at the end of a five-node chain leaves
four digests identical, rebuilds one node, and keeps four capture caches and four output buffers —
so the next evaluate touches one node's chunks (0.18–0.41 ms at 100k, MEASURED `G-data-plane §4`)
instead of re-running the chain (1.72 ms) or re-capturing (100+ ms of scatter and kd-trees).

### 5.4 Dirty propagation: locator → node → chunk → Hydra leaf

This is the path R8 names ("precise dirty propagation on interactive prim edits") and it is four
hops. Each hop is a table lookup built at compile time; none is a search.

```cpp
enum UsdGenDirtyBits : uint32_t {
    UsdGenDirtyNone         = 0,
    UsdGenDirtyParameter    = 1u << 0,   // evaluate-only, this node's chunks
    UsdGenDirtyCapture      = 1u << 1,   // re-capture this node (epoch bumped)
    UsdGenDirtyTopology     = 1u << 2,   // curve or CV count changed
    UsdGenDirtySurfacePoints= 1u << 3,   // deformed points of a bound surface
    UsdGenDirtySurfaceXform = 1u << 4,
    UsdGenDirtySurfaceTopo  = 1u << 5,   // resync of the surface
    UsdGenDirtyMap          = 1u << 6,   // asset path / texture generation
    UsdGenDirtyStructural   = 1u << 7,   // recompile
    UsdGenDirtyLiveOverride = 1u << 8,   // brush stroke (chunk-scoped)
};

class UsdGenDirtyRouter {
public:
    void Rebuild(const UsdGenGraph &graph);      // one entry per (primPath, locatorPrefix)
    /// Called from _PrimsDirtied. O(entries), one hash lookup each; NEVER cooks (P7, S17).
    void Route(const HdSceneIndexObserver::DirtiedPrimEntries &entries,
               UsdGenPendingDirty *pending) const;
    void RouteAdded(const HdSceneIndexObserver::AddedPrimEntries &, UsdGenPendingDirty *) const;
    void RouteRemoved(const HdSceneIndexObserver::RemovedPrimEntries &, UsdGenPendingDirty *) const;
private:
    struct Entry { UsdGenNodeId node; UsdGenDirtyBits bits; UsdGenSurfaceId surface; };
    TfHashMap<SdfPath, std::vector<std::pair<HdDataSourceLocator, Entry>>, SdfPath::Hash> _map;
};
```

**Hop 1 — locator → node.** The adapter publishes each `usdGen:*` property under a nested locator
(`usdGen/clump/size`), so the routing key is `(operatorPrimPath, usdGen/<first two elements>)`.
Per-entry cost is one hash lookup plus a short prefix `Intersects` — MEASURED at
0.013 µs for `HdDataSourceLocatorSet::Intersects` and 0.2 µs per notice entry end to end
(`G-storm-throughput §1.8`, `G-evaluation-scheduling §9`). At 5 000 surfaces the whole cascade is
1.13 ms/frame and is *not* a scheduling concern; what matters is that we do not **cook** here (S17).

**Hop 2 — node → chunks.** Four cases, in the order they matter:

| Cause | Chunks dirtied | Cost |
|---|---|---|
| Parameter edit on node *n* | all chunks of *n* and of every descendant | `memset` over `nChunks` bytes per node |
| `primvars/points/primvarValue` on surface *s* | every chunk whose `desc.surface == s`, in every `deformedSpace` node | precomputed `surface → chunk range` (chunks are surface-major, so it is a range, not a scan) |
| Live override / brush (S40) | the chunk ids the tool passed, or the chunks whose `boundsRest` intersect the brush AABB | O(log n) via the per-tile AABB tree, then O(hit chunks) |
| Capture-epoch bump | all chunks of *n* and descendants, plus `UsdGenDirtyCapture` | as above |

**Hop 3 — chunk → tile.** `dirtyTiles |= 1 << desc.tile` while walking the terminal node's dirty
chunks. A tile is republished iff any of its chunks is dirty. This is the whole reason the tile
exists: Storm's upload unit is the prim (F5), so a one-chunk edit must not re-upload the groom.

**Hop 4 — tile → Hydra leaves.** The publisher emits, per dirty tile, exactly:

```
primvars/points/primvarValue            (bare leaf — usdGen owns the prim, S30)
extent/min, extent/max
primvars/widths/primvarValue            only if a width-writing node was dirty
primvars/displayColor/primvarValue      only on a colour-only edit, NEVER co-dirtied with points (S30)
```

Two measured facts make that discipline pay: co-dirtying any other primvar with `points` drops Storm
out of its points fastpath and re-uploads every non-points primvar
(`G-storm-throughput §1.5`), and emitting `ComputeDirtyLocators`' container sentinel clears the
adapter's primvar-descriptor cache at 0.51 µs/prim/frame (`G-storm-throughput §1.9`). usdGen is the
sole producer of its tile prims, so it uses the **bare leaf** and keeps the cache warm; it uses
`HdContainerDataSourceEditor::ComputeDirtyLocators` only where it *overlays* an upstream prim — i.e.
frozen curves it re-deforms in place (S30).

**Announcement rules** (copied from usdRig, S43/A2 §5): a primvar *appearing* is dirtied as
`primvars/<name>` once (MEASURED: a `…/primvarValue` dirty does not make a new primvar visible; a
`primvars/<name>` dirty does — `G-tool-loop §5`); a prim-type change is `PrimsRemoved` + `PrimsAdded`;
announcement history is kept even while unobserved.

### 5.5 Scheduling, threads and the commit

```cpp
class UsdGenSession {                       // one per (stage, groom root); lives in the registry
public:
    // called on the app/notice thread only
    void      AccumulateDirty(UsdGenPendingDirty &&);
    bool      NeedsCommit() const noexcept;              // atomic<bool>, the S18(c) backstop
    UsdGenGenerationRefPtr Commit(double frame, UsdGenCommitReason);
    UsdGenGenerationRefPtr Generation() const noexcept;  // atomic_load, lock-free (S19, F9)
private:
    tbb::task_arena          _arena;         // P8: private, concurrency = threadLimit
    std::mutex               _commitMutex;   // non-recursive, held only by Commit
    std::atomic<bool>        _dirty;
    UsdGenGraph              _graph;
    UsdGenGenerationStore    _store;
};
```

**Commit points, in priority order (S18):**

1. explicit `UsdGenImaging_Commit()` from the usdview plugin's `currentFrameChanged` handler, using
   the *signal's* frame, not the property (S43, `G-evaluation-scheduling §6`);
2. the `/` + `sceneGlobals/currentFrame` dirty — measured to be the last notice of every un-batched
   frame, exactly 2 `PrimsDirtied` per frame (F8);
3. a lock-free `_dirty.load(std::memory_order_acquire)` backstop on the first `GetPrim` of a
   generated prim, which covers parameter-only edits (no frame notice at all) and the batched
   ordering inversion (S18c).

**Inside `Commit` (single thread, one mutex):**

```
 1. recompile if UsdGenDirtyStructural            (§5.3, ~40 µs)
 2. re-capture nodes whose epoch changed          WorkParallelForN over independent nodes
 3. for each node in topological order:
        if node.dirtyChunks empty -> skip
        arena.execute([&]{ tbb::parallel_for(blocked_range<size_t>(0, nChunks, 1), body); })
        body: skip clean chunks; memcpy in->out for the chunk; op->Evaluate(...)
    commit thread then memsets node.dirty (workers never write dirty bytes: no false sharing)
 4. interleave dirty tiles SoA -> AoS, compute extents          WorkParallelForN over tiles
 5. build the new UsdGenGeneration (immutable), _store.Publish (atomic_store)
 6. diff against the previous generation -> UsdGenDirtyReport
 7. emit PrimsRemoved, PrimsAdded, PrimsDirtied in that order   (usdRig's order, A2 §3.4)
```

Steps 3–4 are the only parallel regions and they are inside the private arena. Step 7 is on the
commit thread because observer callbacks are not required to be threadsafe (F9).

**Why a private arena.** Storm syncs Rprims with `WorkWithScopedParallelism` +
`WorkParallelForN` over rprims (`renderIndex.cpp:1830-1862`). If usdGen's `parallel_for` shared the
default arena, a commit that overlaps a sync would fight for the same workers. `tbb::task_arena`
with an explicit concurrency also lets us honour the measured 8-thread knee (P8) without changing
`PXR_WORK_THREAD_LIMIT` globally, which would slow usdRig down.

**Interruption.** A scrub can supersede an in-flight commit: `Commit` checks
`_generationRequested.load()` between nodes and returns the previous generation unchanged if a newer
frame has been requested. It never publishes a partial generation. (The VDF equivalent,
`SetInterruptionFlag`, is documented as UNVERIFIED against a half-published snapshot,
`G-evaluation-scheduling`, open questions — we simply never half-publish.)

### 5.6 Cross-chunk queries: guides, clumps, and the reference lane

P3 says hair chunks never read hair chunks. The mechanism is the **reference lane**:

* A node with `UsdGenRole::Reference` (guides, clump curves, cards' source roots) produces a buffer
  that is **not chunked for dirtiness** — it is small (guides are typically 1 % of hairs, A7 §9.3)
  and is always evaluated in full, before any node that lists it in `ReferenceInputs()`.
* A consuming node's `Capture()` resolves *which* reference curves each hair uses and stores plain
  index/weight arrays in `UsdGenCapture`. `Evaluate()` then does a random read into the reference
  buffer, which is read-only for the whole parallel region.

Concretely for the two operators that need it:

**`GuideInterpolate` (A7 §9.1 G6).**

```
Capture (once per capture epoch, O(n log g)):
  tree = UsdGenKdTree(reference.rootPositionRest)          # nanoflann 1.12.1 (S38)
  parallel over roots r:
     cand = tree.knn(r, maxCandidates=8, radius=influenceRadius)
     drop g if angle(N_rest(r), N_rest(g)) > maxGuideAngle
     drop g if regionId(g) != regionId(r)                  # region map, capture-time
     w_g = (1 - d_g/R)^decay ; normalise ; keep top maxGuides
  store cap.i0/i1/i2 = guideIdx[0..2], cap.f0/f1/f2 = guideW[0..2]
     (== Unreal's groom_closest_guides / groom_guide_weights, A7 §2)

Evaluate (per chunk, O(curveCount * cvCount * maxGuides)):
  for curve c:  F_c = frame(rootT,rootN,rootB)[c]
     for i in 0..cv-1:
        local = w0*ref.local[i0][i] + w1*ref.local[i1][i] + w2*ref.local[i2][i]
        P[c][i] = root[c] + F_c * local
```

`ref.local` is the guide's *root-local* offset array, computed once per frame when the reference
lane is evaluated — so the per-hair inner loop is three multiply-adds over contiguous planar data,
no matrix inverse, no tree.

**`Clump` (A7 §9.1 S1).** Clump centres are a reference set produced by a nested `ScatterRandom`
(density = `1/clumpSize²`) or by an authored curve set or a clump map. Capture assigns
`clumpId[level]` per hair by nearest-centre kd-tree query and a `stray` coin flip; evaluate lerps
each CV toward the clump curve's CV in the root frame. Multi-level clumping (`levels > 1`, XGen and
Houdini both do this, A7 §9.1) makes level L's centres a *derived* reference set: when
`goalFeedback = 1` the level-L clump curves come from level-(L−1)'s output, which means the
reference lane has its own small topological order. Level barriers are `tbb::parallel_for` fences,
not per-chunk dependencies.

**Cost.** kd-tree build over 4 000 guide roots is microseconds; 100 000 kNN queries at ~1 µs each
would be 100 ms **single-threaded**, so capture is parallel over roots and lands at ~10 ms on 8
threads (ASSUMPTION — nanoflann's own benchmarks; gate E-4 measures it). That is a *capture* cost,
paid on a seed/density/guide edit, never per frame. This is exactly why the capture/evaluate split
is P4 and not an optimisation.

**What this buys over the alternative.** The data-plane report left `UpstreamChunks` as "a
placeholder; its cost is unmeasured" and flagged cross-chunk kernels as an open question
(`G-data-plane §7`, open questions). Routing every cross-curve relation through a reference lane
removes the question: there is no fan-in set, no chunk-dependency graph, and no read-write hazard.

### 5.7 Topology changes and the density scrub

Topology changes are the expensive class: F6 (any element-count change reallocates the whole
aggregated VBO and bumps its version, invalidating every batch drawing the groom) plus a cubic index
rebuild measured at 2.6 ms warm / 5.2 ms cold for 500 000 patches (`G-storm-throughput §1.6`).

usdGen's policy, in three tiers:

| Situation | Behaviour |
|---|---|
| **Interactive density drag** | The buffer is allocated at `usdGen:maxCurves`. Curves above the live count are **parked**: their CVs are collapsed to the root and `widths` set to 0, so element counts never change during the drag (S28). Only `points` and `widths` are dirtied. On release, the real counts are committed once. |
| **Parameter edit that changes CV count** (`Resample`, `cvCount`) | Full re-capture of the sub-graph and one topology publish; the prim set does not change (S27). |
| **Generator seed / surface resync** | Re-capture, re-Morton-sort, one topology publish. Curve ids are stable per `(seed, face, k)`, so sculpt layers survive if the seed did not change. |

Parked curves are not free — each is a near-zero-area patch — so the publisher also *compacts* on
commit-after-release: it rewrites the exact-size arrays (F5) and emits one topology dirty. The
degenerate-CV trick is explicitly the strategy S28 prescribes, and gate S-7 (§11) is the measurement
that would overturn it.

### 5.8 The motion tail

`Space()` on each op partitions the topologically-sorted node list into a **rest head** `H` and a
**deformed tail** `T` at the *last* `restSpace` node (S25, P4). Consequences:

* A frame change dirties only `T`'s chunks: measured tail cost 0.4–0.6 ms vs 1.7 ms for the chain
  (§0.2).
* Motion profile **P2 samples** re-evaluates `T` once per offset and re-uses `H` unchanged:
  `k·T + (k−1)·m` instead of `k·(H+T)` (`G-motion-blur §5`). usdRig itself has no such lever and
  re-runs the whole rig per offset (`bridge.cpp:1284-1287`).
* The per-offset cache is keyed by `(graphGeneration, surfaceGeneration, frame + offset)` so whole-
  frame offsets `[-1, 0, +1]` reuse the previous frame's `+1` while scrubbing forward
  (`G-motion-blur §4.4`).

```cpp
class UsdGenMotionCache {
public:
    struct Key { uint64_t graphGen, surfaceGen; double absTime; };
    /// Returns the interleaved tile arrays for absTime, evaluating the tail if absent.
    const UsdGenSampleSet &Fill(UsdGenSession &s, double absTime);   // takes the commit mutex
    void   Invalidate();                                             // any graph/surface dirty
    size_t Bytes() const;                                            // 19.2 MB per sample at 100k x 16
};
```

`GetContributingSampleTimesForInterval` returns `false` in P0/P1 and the retained offsets (≥2) in
P2; `GetValue(t)` lerps between the two bracketing retained offsets and clamps outside, because
hdPrman re-distributes to `ri:object:geosamples` and will ask for non-retained times
(`G-motion-blur §4.5`). `velocities`/`accelerations` are blocked whenever usdGen owns `points`, in
every profile except P1 (S29, S32) — a stale upstream `velocities` would otherwise be extrapolated
from our new points.

### 5.9 Memory budget and eviction

Measured anchor: the TBB DAG with per-node buffers peaks at **668 MB RSS at 1M curves × 8 CV** with
5 stylers, vs 293 MB with a single shared buffer (`G-data-plane §5`). S24 budgets ≈6× the curve data.

Accounting at 100k × 8 CV (800k CVs, 9.6 MB of positions):

| Item | Bytes |
|---|---:|
| per-node position buffers, 5 nodes | 48 MB |
| `hairT` (shared, one copy) | 3.2 MB |
| `width` where written (2 nodes) | 6.4 MB |
| per-curve arrays (id, rootPrim, rootUV, rootT/N/B, mask) | 6.0 MB |
| capture caches (guideIdx/W ×3, clumpId ×2, restLengths) | 6.0 MB |
| publish ring: interleaved AoS ×2 (P6) | 19.2 MB |
| **total** | **≈ 89 MB** ≈ 9× the raw positions |

`usdGen:memoryBudgetMB` is enforced by an eviction pass at the end of each commit:

```
score(node) = recomputeCostMs(node) / bytes(node)
evict lowest score first, subject to:
  - never evict the tail base (the last restSpace node): it is re-read every frame
  - never evict a node that a live override targets
  - never evict a capture cache whose rebuild is O(n log n) unless bytes > 4 * buffer bytes
```

The policy is deliberately biased: **interior rest-space nodes are the cheapest thing to lose**,
because the only edit that needs them re-runs the whole prefix anyway, and a prefix re-run is
bounded by the head cost (1–2 ms at 100k). Evicting the tail would cost a re-run *every frame*.

### 5.10 Diagnostics

| Channel | Content |
|---|---|
| `UsdGenStats` (atomic counters, lock-free, `usdGen:diagnostics = counters`) | `commits`, `cookedNodes`, `cookedChunks`, `capturedNodes`, `publishedTiles`, `interleavedBytes`, `noticeEntries`, `recompiles`, `evictions`, `motionSamples`, and a ring of the last 120 commit durations split by phase |
| `TF_DEBUG` | `USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`, `USDGEN_CAPTURE`, `USDGEN_MEMORY` |
| `TfTrace` scopes | `UsdGen::Compile`, `::Capture`, `::Evaluate<opType>`, `::Interleave`, `::Publish`, `::Diff` — named to match the gate names in §11 so a chrome trace is directly comparable to the budget |
| usdview HUD line | `usdGen 0.9 ms  49/49 tiles  196/196 chunks  gen 1042  89 MB` |
| `UsdGenImaging_GetStatsJson()` (C ABI) | the same counters for `testusdview` assertions |

The in-process SIGPROF sampler from `probes/data-plane-engine-prototype-benchmark/sampler.h` (BUILD)
is carried into `usdGenTestUtils`, because this host refuses `perf` (`perf_event_paranoid = 4`) and
`gdb -p` (yama), a fact any future profiling on this machine has to live with (`G-data-plane` §host).

### 5.11 Kernel authoring rules (`usdGenMath`)

```cpp
// usdGenMath/stylerKernels.h — the shape every styler kernel takes
void UsdGenClumpKernel(const UsdGenClumpParams &p,
                       const float *RESTRICT cx, const float *RESTRICT cy, const float *RESTRICT cz,  // clump curve
                       const float *RESTRICT ramp,       // 257-entry LUT over hairT
                       const int   *RESTRICT clumpId,
                       const float *RESTRICT mask,
                       uint32_t curveCount, uint32_t cvCount,
                       float *RESTRICT px, float *RESTRICT py, float *RESTRICT pz);
```

1. Free functions, `RESTRICT` pointers, no `GfVec3f` in the inner loop (F4).
2. Ramps are baked to a **257-entry float LUT** at capture (usdRig's falloff-LUT pattern,
   A1 §6) so the inner loop is a lerp, not a spline evaluation.
3. No hand-written SIMD. GCC 13.3 `-O3` autovectorises all three prototype kernels with 128-bit NEON
   (MEASURED, `G-data-plane §6`); usdRig's only SSE2 kernel is dead code on ARM anyway. Layout is
   the lever, not intrinsics (S22).
4. `-ffp-contract=off` on the target, plus a CI job that builds with `fast` so epsilons stay honest
   (S45 — the curvenet failure is direct evidence that FMA contraction flips surface-walking
   branches).
5. Deterministic reductions: fixed order, `hash64` rather than any RNG with hidden state, and no
   dependence on unordered-container iteration (usdRig's determinism contract, A1 §5).

---

## 6. The imaging library (D3)

### 6.1 Objects

```cpp
// usdGenImaging/registry.h — process-global, survives renderer switch and stage replace (S15)
class UsdGenImagingRegistry {
public:
    static UsdGenImagingRegistry &GetInstance();

    UsdGenSessionHandle    Attach(const UsdGenSessionKey &key);   // weak stage ptr + groom root
    void                   Detach(const UsdGenSessionKey &key);
    UsdGenSessionHandle    Find(const UsdGenSessionKey &key) const;

    void                   SetTime(double frame);                 // -> Commit on every session
    void                   Commit(UsdGenCommitReason);
    int64_t                Generation() const noexcept;
    void                   ReloadMaps();                          // S13: bumps the texture counter
private:
    mutable std::mutex _mutex;      // non-recursive; released before any re-entrant call
    std::unordered_map<UsdGenSessionKey, std::weak_ptr<UsdGenSession>> _sessions;
};

// usdGenImaging/sceneIndices.h
class UsdGenGroomSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static UsdGenGroomSceneIndexRefPtr New(HdSceneIndexBaseRefPtr in, const HdContainerDataSourceHandle &args);
    HdSceneIndexPrim GetPrim(const SdfPath &) const override;      // atomic_load only (S19)
    SdfPathVector    GetChildPrimPaths(const SdfPath &) const override;
protected:
    void _PrimsAdded(...)   override;   // route + forward unchanged
    void _PrimsRemoved(...) override;
    void _PrimsDirtied(...) override;   // route + forward unchanged; commit on sceneGlobals/currentFrame
    void _SystemMessage(const TfToken &, const HdDataSourceBaseHandle &) override;  // asyncAllow/asyncPoll (S20)
private:
    HdSceneIndexBaseRefPtr        _pruned;    // private HdSiExtComputationPrimvarPruningSceneIndex (S3)
    UsdGenSessionHandle           _session;   // weak; re-attaches after a renderer switch (S15)
    UsdGenDirtyRouter             _router;
    UsdGenPublishedIndex          _published; // path -> (tile | guide | instancer | cvPoints)
};

class UsdGenGroomSceneIndexPlugin final : public HdSceneIndexPlugin { ... };          // S1
class UsdGenMetadataSceneIndexPlugin final : public UsdImagingSceneIndexPlugin {      // S7
    TfTokenVector InstanceDataSourceNames() override;               // {"usdGen"}
    TfTokenVector ProxyPathTranslationDataSourceNames() override;   // {"usdGen"}
    HdSceneIndexBaseRefPtr AppendSceneIndex(...) override { return inputScene; }
};
```

The pruning wrapper is **private and never spliced into the shared chain** (S3): it is constructed
once around `_GetInputSceneIndex()` and is a pass-through when nothing is computed, which is what
makes UsdSkel-skinned scalps readable under Storm as well as hdPrman (F14).

### 6.2 The generation store

```cpp
// usdGenImaging/generationStore.h
struct UsdGenTilePublication {
    VtVec3fArray points;                 // interleaved, exact size (F5, S28)
    VtIntArray   curveVertexCounts;
    VtFloatArray widths, hairT;
    VtVec3fArray hairTangent;
    VtIntArray   hairId;                 // uniform
    VtVec2fArray st;                     // uniform, root UV
    VtVec3fArray displayColor;           // uniform
    GfRange3d    extent;
    uint64_t     topologyVersion;
};
struct UsdGenGeneration {
    int64_t                                   id;
    double                                    frame;
    std::vector<UsdGenTilePublication>        tiles;
    std::vector<UsdGenTilePublication>        guides;
    std::vector<UsdGenInstancerPublication>   instancers;
    UsdGenPrimSetSignature                    signature;   // for the structural diff
};
class UsdGenGenerationStore {
public:
    void                    Publish(UsdGenGenerationRefPtr);        // commit thread only
    UsdGenGenerationRefPtr  Get() const noexcept;                   // std::atomic_load (F9)
    UsdGenDirtyReport       Diff(const UsdGenGeneration *prev, const UsdGenGeneration &next) const;
};
```

`Diff` is the analogue of `RigExecSnapshotStore::_Diff` (A2 §3.1) and is deliberately cheap: tiles
carry a `topologyVersion` and a per-array `VtArray` data pointer, so "did this tile change" is a
pointer comparison, not a memcmp. Structural bits (prim appearing/disappearing, primvar set change,
type change) are computed from `signature`.

`GetPrim` for a tile builds retained data sources over the published arrays. It rebuilds the
containers on every pull, exactly as usdRig does (A2 §3.2), which is why the dirty leaves have to be
correct — but usdGen is the *sole producer* of these prims, so the bare-leaf rule of §5.4 applies
and the descriptor cache stays warm.

### 6.3 What every synthesized prim carries

| Data source | Value | Why |
|---|---|---|
| `basisCurves/topology` | `type=cubic`, `basis=bspline`, `wrap=pinned`, `curveVertexCounts` | S29 |
| `primvars/points` | vertex, role `point`, exact size | S28, F5 |
| `primvars/widths` | vertex (or constant when uniform); **never varying** — varying costs a 6.48 ms CPU expansion per 100k curves | S29, `G-storm-throughput §2.5` |
| `primvars/hairT`, `hairTangent` | vertex float / vec3 (object space) | S29, S35 — the tangent primvar is what makes the glslfx look like hair (`G-storm-hair-look §2.5`) |
| `primvars/hairId`, `st`, `displayColor` | uniform | S29; uniform primvars live in their own element BAR and are 3 orders of magnitude cheaper than points (`G-storm-throughput §1.5`) |
| `primvars/velocities`, `accelerations` | `HdBlockDataSource` (except P1) | S29, S32 |
| `extent/min|max` | per tile, every deforming frame | F7 — without it the tile is never frustum-culled |
| `xform/matrix` + `resetXformStack=true` | the surface's world matrix; points are surface-local | S4 |
| `displayStyle/refineLevel` | `2` | S29; one refineLevel per chunk set or the batch fragments (`G-storm-throughput §1.11`) |
| `primvars/minScreenSpaceWidths` | constant `1.0` | S29 |
| `primOrigin/scenePath` | the `UsdGenDescription` path (absolute) — **relative** inside propagated prototypes | F12, S34, `G-instancing §6` |
| `purpose` | inherited from the description; guides get `guide` | A2 §5 |
| `__dependencies` | one edge per tile onto its surface prim (32–256 edges, never per curve) | S30; fan-out is 0.38 µs/prim at 1k and 130 ms at 100k (`G-storm-throughput §1.8`) |
| `visibility`, material binding | inherited by hand from the description/surface | A2 §5 |

### 6.4 Locating the surface (`usdGen:surface` → propagated paths)

```cpp
class UsdGenSurfaceResolver {
public:
    struct Resolved { SdfPath hydraPath; UsdGenSurfaceId id; bool insidePrototype; };
    Resolved Resolve(const SdfPath &groomPrimPath, const SdfPath &authoredTarget) const;
};
```

Algorithm:

1. If the groom prim path has an ancestor whose `__usdPrimInfo.isNiPrototype` is true, take that
   ancestor's `niPrototypePath` and the prim's `primOrigin.scenePath` (relative inside prototypes)
   and **rebase** the authored target onto the prototype root (ASSUMPTION 4).
2. Otherwise use the target verbatim.
3. Verify the resolved path yields a prim of type `mesh` through the private pruning wrapper; if not,
   emit one diagnostic per groom per compile and pass through.

Propagated prototype names are hashes (`ForInstancer%zx`, `NoPrimvars___usdUpAxis…`) and are
**discovered, never constructed** (S34, `G-instancing` key facts). Authoring-time instance-proxy
targets (`/World/HeadA/Scalp`) are translated by S7's `ProxyPathTranslationDataSourceNames()`; grooms
that differ per instance aggregate separately thanks to `InstanceDataSourceNames()` — the item usdRig
deferred (`G-instancing §5`).

### 6.5 Live overrides (the brush loop)

```cpp
// C++ side of S40; no stage traffic during the drag
class UsdGenLiveOverride {
public:
    void Begin(UsdGenNodeId terminal);
    void SetFull(const VtVec3fArray &points);                              // press / release
    void SetIndexed(const VtIntArray &cvIndices, const VtVec3fArray &pts); // per move
    void Clear();
    TfSpan<const UsdGenChunkId> TouchedChunks() const;
};
```

A live override is a *virtual node* appended after the terminal: it owns no buffer, it patches CVs in
place into the publish ring's back buffer, and it reports the chunks it touched so §5.4's hop 3
dirties one or two tiles. Measured pieces of the loop: 21 µs Python per move (0.34 µs zero-copy read
+ 16.6 µs numpy kernel over a 2 000-CV footprint + 4.1 µs sparse push), 1.2–3.7 µs for the indexed
push, and 0.035 ms of engine (S39, S40, `G-tool-loop §1.4`, `G-data-plane §4`). The rest of the
budget is Storm's tile re-upload, which is one prim, ~0.4 MB.

CV display is a synthesized `points` child prim (`<tile>/cvs`) with `widths` and `displayColor`, not
a Hydra selection: `HdSelectionSchema` carries only `fullySelected`/`nestedInstanceIndices` and the
engine exposes no point-selection API (S40, `G-tool-loop §3`). Selection state is a colour.

### 6.6 The UsdImaging adapters (S10, S12)

Without these, `usdGen:*` attributes and relationships are invisible and produce **no notice at all**
(F2, MEASURED twice: `G-stage-free §1/§5`, `G-freeze-bake §2.3`). Mappings are built **generically**
from `UsdPrimDefinition`, never from a hand-written list — `UsdImagingDataSourceMapped::Get`
`TF_CODING_ERROR`s on a property the prim does not have (`G-stage-free §2`).

```cpp
// usdGenImaging/operatorAdapter.h
class UsdGenOperatorAdapter final : public UsdImagingSceneIndexPrimAdapter {
public:
    TfTokenVector       GetImagingSubprims(const UsdPrim &) override;              // {TfToken()}
    TfToken             GetImagingSubprimType(const UsdPrim &, const TfToken &) override;
    HdContainerDataSourceHandle GetImagingSubprimData(const UsdPrim &, const TfToken &subprim,
                                    const UsdImagingDataSourceStageGlobals &) override;
    HdDataSourceLocatorSet InvalidateImagingSubprim(const UsdPrim &, const TfToken &subprim,
                                    const TfTokenVector &properties,
                                    UsdImagingPropertyInvalidationType) override;
private:
    // built once per schema type from UsdPrimDefinition::GetPropertyNames()/GetSpecType()/
    // GetSchemaAttributeSpec()->GetTypeName(); ramps get a TsSpline factory (S11), asset
    // attributes a resolved-path factory (S13).
    static const UsdImagingDataSourceMapped::PropertyMappingsSharedPtr &_Mappings(const TfToken &type);
};

class UsdGenRestAPIAdapter final : public UsdImagingAPISchemaAdapter {   // S12
    // publishes usdGen/rest/points from a factory that samples UsdTimeCode::Default(),
    // honouring an authored primvars:rest; never flagged time-varying.
};
```

`InvalidateImagingSubprim` forwards to `UsdImagingDataSourceMapped::Invalidate`, which returns the
**absolute** nested locator (`usdGen/ramp/knots`, `usdGen/surface`) — exactly the key §5.4's router
hashes on. Registration is by `primTypeName` with `includeDerivedPrimTypes: true`, so one adapter
serves every concrete operator type (S10).

---

## 7. Operator catalogue (D4)

Columns: **Space** = `R`est / `D`eformed; **Topo** = does it change counts; **Cost** = A (per-CV
parallel), B (needs a spatial structure at capture), C (per-CV surface query); **Capture** = what the
epoch caches. Reference is A7 §9, which this table implements verbatim where it can.

### 7.1 v1 — the chain the request names (R4), plus what R3 needs

| Op (`UsdGen…`) | Space | Topo | Cost | Capture cache | Key parameters (defaults) |
|---|:--:|:--:|:--:|---|---|
| `ScatterRandom` | R | yes | A (+B if relax) | face areas, per-face counts, ids, root frames, Morton order | `density 100000`, `seed 0`, `relaxIterations 0`, `areaCompensation true`, `flip false`, mask block |
| `ScatterPoints` | R | yes | A | ids, root frames | `rootPrims[]`, `rootUVs[]` (authored by the Place tool) |
| `Grow` | R | sets CV count | A | — | `segments 8`, `length 1.0`, `lengthVariance 0`, `direction "normal"`, `lift 0`, `tangentialBlend 0` |
| `GuideInterpolate` | R | sets CV count | B / A | kd-tree, `guideIdx[3]`, `guideW[3]` | `cvCount 0`, `maxGuides 3`, `influenceRadius 1.0`, `influenceDecay 2.0`, `maxGuideAngle 90`, `blendInSkinSpace 1`, `blendMethod "extrudeAndBlend"`, `useUniqueGuide false`, `randomizeGuide 0` |
| `Clump` | R | no | B / A | clump centres, kd-tree, `clumpId[level]`, stray coins, rest lengths | `clump 0.5` + ramp, `clumpSize 1.0` **or** `clumpDensity`, `clumpSeed 0`, `levels 1`, `sizeReduction 0.5`, `tightnessReduction 0.8`, `goalFeedback 1`, `stray:amount/rate/falloff`, `volumize 0`, `preserveLength 1`, `method "linearBlend"`, `crossover 0` |
| `Noise` (frizz) | R | no | A | correlation hashes | `magnitude 0` + ramp, `frequency 1`, `correlation 0`, `octaves 1`, `lacunarity 2`, `gain 0.5`, `noiseSpace "rest"`, `preserveLength 1` |
| `Length` | R | `cull` only | A | rest arc lengths | `mode "multiply"`, `value 1`, `random`, `method "scale"`, `cullThreshold 0`, `rebuild "reparam"` |
| `Width` | R | no | A | — | `width 0.01`, `widthRamp`, `taper 0`, `taperStart 0.5`, `rootScale 1`, `tipScale 1` |
| `DeformWithSurface` | **D** | no | A | root→face binding, rest frames | `mode "rigidFrame"`, `twistAware true`, `preserveShape 0` |
| `CurveSource` | R | yes | A | frozen buffer load, id sort | `curves` rel, `role`, `cvCount`, `expectEpoch`, `staleAction` |
| `SculptLayer` | R | no | A | id→slot map | `weight 1`, `deltaSpace "rootFrame"`, delta arrays |
| `Splines` (primitive) | — | — | — | — | `cvCount 8`, `width`, `widthRamp`, `basis "bspline"` |
| Maps: `ImageMap`, `ExprMap` | — | — | — | sampled per root at capture | see §9 |

v1 also ships `UsdGenGroom`, `UsdGenDescription`, `UsdGenMaskAPI`, `UsdGenRestAPI`.

### 7.2 v2 — the styling vocabulary artists expect

| Op | Space | Topo | Cost | Notes |
|---|:--:|:--:|:--:|---|
| `ScatterUniform` | R | yes | A | rows/cols in UV, `spacingU/V`, `jitter` |
| `ScatterAtGuides` | R | yes | A | one hair per guide root |
| `Curl` | R | no | A | RMF frame from `usdGenMath` (`RigExecSampleCurveRMF` is reusable, A1 §7): `radius` + ramp, `frequency`, `phase`, `taper`, `axisMode` |
| `Bend` | R | no | A | cumulative per-segment rotation, `angle` + random, `axisMode` |
| `Direction` / `Lift` | R | no | A | rigid rotate about the root normal; XGen Tilt U/V/N |
| `Straighten`, `Smooth` (along-curve) | R | no | A | Laplacian, `iterations`, `lockRoot` |
| `Wave` | R | no | A | `frequency`/`amplitude` in T and N |
| `Displace` | R | no | A | map-driven along the root normal |
| `Resample` | R | CV count | A | arc-length resample to `cvCount` |
| `Cards`, `Archives`, `Spheres` (primitives) | D | — | A | §4.7, S33 |
| `PtexMap`, `PaintMap` | — | — | — | §9 |
| Motion P1/P2 profiles | — | — | — | S32, §5.8 |

### 7.3 v3 — the ones that need deformed-space spatial structures or a solver

| Op | Space | Cost | Why later |
|---|:--:|:--:|---|
| `Collide` / `Shrinkwrap` | **D** | C | needs a BVH over the collider rebuilt per surface epoch; the only operator that breaks the "no per-CV surface query" rule |
| `Smooth` (neighbour mode) | **D** | B | needs a per-frame grid over deformed roots |
| `Wind` / `Force` | **D** | A | time-dependent; forces every downstream node into the tail |
| `DeformWithSurface mode="rbf"` / `preserveShape` | **D** | B | Cosserat iterations; needs a solver and per-curve state |
| `Braid` | R | A | topology ×3 |
| OpenExec backend behind `UsdGenOp` | — | — | S16: the control plane is not plugin-extensible in 26.08; revisit when it is |
| GPU (HGI compute) evaluation of the tail | **D** | — | §13 R-7 |

### 7.4 The mask block, evaluated once

Every operator resolves its mask at **capture** into `curveMask` (one float per curve) and a
257-entry along-curve LUT:

```
curveMask[c] = clamp( combine( scale,
                               map(rootUV[c] | faceId[c], channel),
                               expr(varBlock for curve c),
                               lerp(1, hash01(c, randomSeed), random) ), 0, 1 )
rampLUT[i]   = ramp(i / 256)
w(c, i)      = amount * curveMask[c] * rampLUT[round(hairT[i] * 256)]
```

Measured costs that justify doing this at capture and never per frame: SeExpr 13–117 ns/eval
(50 M evals/s aggregate on 8 threads), Ptex bilinear 23 ns/lookup (228 M/s on 8 threads) — so a
1M-root mask is ~0.1 s single-threaded, ~15 ms on 8 threads, once per edit (S38, A8 §1.6, §2.8). Per
frame it would be 6× the entire evaluate budget.

---

## 8. Static curves, freezing and re-entry (R3, R9)

The engine treats a frozen prim and a generated prim as **the same kind of input** (S42, F10) — this
is the single most useful result the freeze research produced, and it is why "freeze an operator's
output and comb it" is a *configuration*, not a mechanism.

| Concern | Decision |
|---|---|
| Contract | plain `UsdGeomBasisCurves` + `primvars:rest` (vertex), `skinprim` (uniform int), `skinprimuv` (uniform texCoord2f, **not** named `st`), `usdGen:curveId` (uniform int), `usdGen:frozenEpoch` (**constant string primvar**), optional `usdGen:rootFrame` (S42) |
| Why a constant primvar | relationships and custom attributes never reach Hydra and never invalidate; a constant primvar does, and dirties precisely (`G-freeze-bake §2.3`, MEASURED) |
| Landing tiers | T1 session layer (0.4 ms, interactive); T2 sidecar `.usdc` sublayer (10.6 ms, 9.6 MB at 100k×8, reopen 0.8 ms); T3 `.usdc` payload (21.5 ms to compose, unloadable). **Never `.usda`** (40 MB, 532 ms to reopen) and **never bake motion samples** (24 samples = 230 MB) (S42, `G-freeze-bake §3`) |
| `primvars:rest` cost | **26 bytes** while it still shares the `points` buffer, because crate deduplicates identical value buffers (MEASURED). Always author it |
| Undo | `SubtreeSnapshot` (`Sdf.CopySpec` into an anonymous stash; 0.10 ms capture, 0.24 ms undo, **zero RSS growth**); undo of a live freeze is `SetActive(false)` (0.02 ms at 1M curves), real removal only on stack eviction (S41) |
| Placement | freezes are siblings under `__UsdGenFrozen`; the tooling never re-authors the parent scope's spec (re-authoring a parent `typeName` resyncs 203 sibling prims, MEASURED) |
| Staleness | `frozenEpoch` = the capture epoch of the frozen node. Mismatch ⇒ the `UsdGenCurveSource` reports *stale* and, per `staleAction`, warns / ignores / blocks. Sculpt deltas keyed by `usdGen:curveId` survive any edit that keeps ids |
| The OpenExec trap | `UsdStage::RemovePrim` with an exec system attached raises a spurious `Tf.ErrorException` (`esfUsd/stageData.cpp:360`); contain with `TfErrorMark` in C++ and `try/except` + a post-condition assert in Python; a freeze may only be undone in the layer it was authored into (S41, S46) |

---

## 9. Look, maps and expressions (D5)

### 9.1 The three-terminal material, and who authors it

One `Material` prim carries three terminals (S36); the **scene index** chooses which binding the
current delegate sees, because render-context preference is UNVERIFIED (ASSUMPTION 7).

| Terminal | Network | Consumer | Status |
|---|---|---|---|
| `outputs:surface` | `UsdPreviewSurface` reading `displayColor` + `st` (uniform, root UV) | universal fallback; every delegate; "plugin not installed" | MEASURED to work on curves, per-curve root-UV texture lookup verified (`G-storm-hair-look §4`) |
| Storm-specific | `usdGen:HairPreview` / `…Translucent` glslfx | Storm | MEASURED: parses in Sdr (20 inputs), compiles and renders, 0 warnings; `$P/usdGenHairPreview.glslfx` EXISTS (S35) |
| `outputs:mtlx:surface` | `ND_deon_hair_absorption_from_melanin` → `ND_chiang_hair_roughness` → `ND_chiang_hair_bsdf` → `ND_surface`, with an explicit `ND_geompropvalue_vector3(geomprop="hairTangentWorld")` into `curve_direction` | hdPrman / Karma / render time | compiles and renders in Storm but near-black there; correct at render time (S36, `G-storm-hair-look §3`) |

The tool authors all three from one panel (`Groom → Create hair material`) with shared parameters
(root/tip colour, melanin, roughness, specular), so the Storm preview and the render agree by
construction. Two glslfx files ship because the material tag is baked into the glslfx metadata and
also splits draw batches: `defaultMaterialTag` (opaque + alpha-to-coverage) for scalp hair,
`translucent` (OIT, `HdxOitRenderTask`) for fine fur (S35, MEASURED).

Hard rules from the prototype: never read `inData` from the material (the wire layout at refineLevel
0 has only `Peye`, and the shader then hard-fails and Storm silently substitutes the fallback);
root→tip variation is the `hairT` **primvar**, because `patchCoord.y` is the *width* coordinate; the
tangent is the `hairTangent` primvar with a screen-derivative fallback (S35, `G-storm-hair-look §2.5`).
One refineLevel and one material per chunk set, or the batch fragments (S30).

### 9.2 Baking: everything is CPU, at capture

S37: all map/Ptex/SeExpr evaluation is CPU at capture time and is baked to per-curve/per-CV primvars
(colour, density, length, masks, widths). Storm textures are used only for UV-mapped scalp colour via
the uniform `st` primvar, which is exactly the path §4 of the look report verified. Ptex is compiled
out of this OpenUSD install and is mesh-only in Storm's GLSL anyway (S37, A8 §2.9).

| Map prim | Backend | Sampling |
|---|---|---|
| `UsdGenImageMap` | Hio (`png jpg tga bmp hdr exr avif` present; no OIIO, no tif) | at the root's `st`, bilinear, `wrap` |
| `UsdGenPtexMap` | vendored Ptex 2.4.3 static+hidden; face ids and adjacency from the installed `Far::PtexIndices` via `PxOsdRefinerFactory` | 23 ns/lookup bilinear, one `PtexFilter` per worker (S38, A8 §2.6–2.8) |
| `UsdGenExprMap` | vendored `wdas/SeExpr` `main@8f8c8f2`, interpreter only, static+hidden, `rand()` added | one thread-safe `VarBlock` per TBB worker; 13–117 ns/eval (S38) |
| `UsdGenPaintMap` | the paint tool's output: EXR (float) via Hio, or `.ptx` via `PtexWriter` | same as the corresponding reader |

SeExpr variable set (XGen dialect, S38, A8 §1.8): `$u $v $id $faceId $patchId $frame $t
$cLength $cWidth $cDepth $P $Pref $N $Nref $dPdu $dPdv $dPduref $dPdvref $Cs $As`, plus functions
`map("name"[,s,t][,channel])`, `ptex("name", faceId, u, v[, channel])`, `rand([min,max][,seed])` and
the full SeExpr builtin set. `${DESC}`-style macros are pre-substituted and resolved through Ar.
**Noise has one implementation** — SeExpr's `Noise.h` templates — used by both the expression layer
and the C++ stylers, so `noise($P)` in an expression and the `Noise` styler agree bit-for-bit (S38).
SeExpr's `noise` is 0..1 while XGen's is −1..1; usdGen keeps SeExpr semantics and documents `snoise`.

### 9.3 Reload and the paint round-trip

An overwritten map file is **not** picked up automatically (S13, MEASURED: `stage->Reload()` produces
no notice). usdGen ships an explicit action:

```
UsdGenImaging_ReloadMaps()  ->  registry bumps _textureGeneration
                            ->  every capture epoch that includes a map digest changes
                            ->  affected nodes re-capture; nothing else moves
```

`ArNotice::ResolverChanged` is *also* honoured when a host sends it (MEASURED to produce exactly the
right per-property dirties), but usdGen never depends on it. `asset[]` gets no reload tracking at
all, so each map is one `asset` attribute on its own prim (S13).

Paint round-trip: the brush writes into the map prim's in-memory texel buffer (dirtying
`primvars/<paintName>/primvarValue` per move for the scalp overlay, `primvars/<paintName>` once on
appearance — S30, MEASURED `G-tool-loop §5`); on commit it writes an EXR or `.ptx` beside the asset,
re-resolves, and bumps the texture generation. Ptex adjacency for authoring comes from
`Far::PtexIndices::GetAdjacency`, which is exactly `FaceInfo::setadjfaces/setadjedges`' input (A8 §2.7).

---

## 10. Tooling (D6)

### 10.1 The two Python surfaces (S39)

```c
/* usdGenImaging/registry.h — ctypes C ABI, control and scalars only */
int       UsdGenImaging_Activate(long long stageCacheId, const char *groomRoot, double frame);
int       UsdGenImaging_SetTime(double frame);
int       UsdGenImaging_Commit(int reason);
void      UsdGenImaging_Deactivate(void);
long long UsdGenImaging_GetGeneration(void);
int       UsdGenImaging_BeginLiveOverride(const char *descPath);
int       UsdGenImaging_ClearLiveOverride(const char *descPath);
int       UsdGenImaging_PickCV(const char *descPath, const float viewProj[16], int w, int h,
                               float x, float y, float radiusPx, int *outCurve, int *outCv, float *outDistPx);
int       UsdGenImaging_FootprintCV(const char *descPath, const float viewProj[16], int w, int h,
                                    float x, float y, float radiusPx, int *outIdx, int maxOut, int *outCount);
int       UsdGenImaging_ReloadMaps(void);
int       UsdGenImaging_SetLod(const char *descPath, float ratio);
const char *UsdGenImaging_GetStatsJson(void);
```

```c++
// _usdGen — pxr_boost.python, arrays only; VtArray crosses in O(1) (F11)
PXR_BOOST_PYTHON_MODULE(_usdGen) {
    bp::def("ReadCurvePoints",       &UsdGen_ReadCurvePoints);        // (descPath, tile) -> VtVec3fArray
    bp::def("ReadCurveCounts",       &UsdGen_ReadCurveCounts);
    bp::def("ReadCurveWidths",       &UsdGen_ReadCurveWidths);
    bp::def("ReadCurveIds",          &UsdGen_ReadCurveIds);
    bp::def("SetLiveOverride",       &UsdGen_SetLiveOverride);        // (descPath, VtVec3fArray)
    bp::def("SetLiveOverrideIndexed",&UsdGen_SetLiveOverrideIndexed); // (descPath, VtIntArray, VtVec3fArray)
    bp::def("ReadPaintTexels",       &UsdGen_ReadPaintTexels);
    bp::def("WritePaintTexels",      &UsdGen_WritePaintTexels);
}
```

Rules that are review-enforceable (S39): the module imports `from pxr import Vt, Gf` at load (Vt
converters live in `pxr/Vt/_vt.so`, not `libusd_vt.so`); numpy is the *kernel* language over a
zero-copy `np.asarray(vt)` view, never the transport; `Vt.*Array.FromBuffer` and `attr.Set(ndarray)`
are banned on groom-sized arrays (420 µs vs 1.8 µs); BLAS threads are pinned to 1 before
`import numpy` (a 0.56 ms matmul became 22 ms on a loaded host).

### 10.2 Panels

| Panel | Contents |
|---|---|
| **Groom** | groom/description tree; per-description curve count, tile count, LOD slider, memory, live commit time; buttons: Reload maps, Freeze, Commit freeze, Bake to sidecar |
| **Operator stack** | the chain in topological order (the order the engine runs, as usdRig's `mover_order()` exposes); enable toggles; drag-to-reorder rewrites `usdGen:input`; per-op cost from `GetStatsJson` |
| **Parameters** | generated from `UsdPrimDefinition::GetPropertyNames()` — no hand-written UI per operator; ramps get a curve widget backed by the knot arrays |
| **Diagnostics** | the commit-phase histogram, chunk/tile dirty counts, and the eviction log |

usdview conventions (S43): `PluginContainer` with state in an `__init__`-initialised object, lazy Qt
imports, one shared undo stack, the *signal's* frame in `currentFrameChanged`, `UpdateViewport()`
after every edit and undo, one `_resetGUI` per batched freeze, `USDGEN_IMAGING_DLL` override.
`registerPlugins` also sets `_allowAsync = True` on the app controller so the 100 ms poll exists
without `--allow-async` (S20; runtime-UNVERIFIED, gate T-5).

### 10.3 Brushes — one loop, eight tools

Every brush runs the same four-phase loop; only the kernel differs.

```
press    : resolveCamera() once; UsdGenImaging_BeginLiveOverride(desc)
           pts = _usdGen.ReadCurvePoints(desc, tile);  base = np.asarray(pts)   # 0.34 us
move     : idx  = UsdGenImaging_FootprintCV(...)                                # 0.59 ms @100k
           new  = kernel(base[idx], brush)                                      # 16.6 us @2k CVs
           _usdGen.SetLiveOverrideIndexed(desc, idx, new)                       # 1.2 us
           usdviewApi.UpdateViewport()                                          # one tile re-upload
release  : one Sdf.ChangeBlock; attr.Set(vt) on the frozen prim's points        # 2.4 us
           undoStack.Push(SubtreeSnapshot); ClearLiveOverride()
escape   : ClearLiveOverride(); no stage write
```

| Tool | Kernel | Authored on release |
|---|---|---|
| Comb | rotate CVs about the root toward the drag direction, falloff by footprint distance × `hairT` | frozen `points` |
| Grab | translate CVs, root-locked | frozen `points` |
| Smooth | Laplacian along the curve | frozen `points` |
| Length / Cut | scale or truncate arc length | frozen `points` (+ `curveVertexCounts` on cut, one topology publish) |
| Clump (paint) | paint `clumpMask` texels | `UsdGenPaintMap` texels |
| Density (paint) | paint density texels; the drag keeps element counts fixed (S28) | `UsdGenPaintMap` texels; count change on release |
| Map paint | generic scalar/colour texel paint | EXR or `.ptx` |
| Place guides | `UsdGenImaging_PickCV` on the surface → append `rootPrims/rootUVs` | `UsdGenScatterPoints` arrays |

CV picking is **CPU, in C++, over the C ABI**: 166 µs at 100k CVs, 1.66 ms at 1M, versus 1.3–1.42 ms
for one Hydra `view.pick()` — 8× cheaper (S40, MEASURED). It does not handle occlusion; the
documented fallback is a private render index with `HdxPickTask` (`UsdImagingGLEngine` has no
`GetRenderIndex()` in 26.08), taken only if gate T-3 shows occlusion errors matter.

### 10.4 Freeze / commit flows

| Flow | What happens |
|---|---|
| **Freeze operator** | The engine writes the node's current buffer to a T1 session-layer `BasisCurves` under `__UsdGenFrozen`, inserts a `UsdGenCurveSource` pointing at it, and re-parents the downstream node's `usdGen:input`. Upstream nodes are left authored but become unreachable — XGen's "Groom Bake deactivates all modifiers below it" (A7 §9.3), expressed as a wiring change instead of a mode flag. All of it in one `Sdf.ChangeBlock`, prims `Define`d outside it (S41) |
| **Unfreeze** | `SetActive(false)` on the frozen prim and restore the wiring — 0.02 ms, no OpenExec diagnostic (S41) |
| **Commit to stage** | T2: export the frozen prims to `<asset>_groomBake.usdc` and sublayer it (10.6 ms + 1.2 ms export at 100k) |
| **Bake for render** | T3 payload for very heavy grooms |

### 10.5 testusdview coverage

`testUsdviewUsdGenGroom.py` (panel + stack edit + dirty count), `…Brush.py` (press/move/release/undo
with an asserted generation sequence), `…Freeze.py` (freeze, comb, unfreeze, redo),
`…Cards.py` (instancer pick round-trip through `primOrigin`), `…Async.py` (the `_allowAsync` gate).
All select the terminal scene index by the `"[Terminal SI] "` prefix, never `names[-1]` (S43, S40).

---

## 11. Performance model and the gates that must hold (the lens' contract)

### 11.1 Cost model

```
commit(frame)      = R + Σ_{n in tail} c(n)·dirtyChunks(n)/nChunks + I·dirtyTiles/nTiles + P
edit(parameter p)  = R + Σ_{n in cone(p)} c(n) + I·dirtyTiles/nTiles + P
edit(brush stroke) = R + c(terminal)·k/nChunks + I·1/nTiles + P          (k = touched chunks)
capture(epoch)     = Σ_{n in cone} cap(n)                                # once per edit, not per frame
motion P2          = H_cached + k·tail + (k-1)·m

R ≈ 0.05 ms (routing)          I ≈ 0.5 ms per full interleave of 800k CVs
P ≈ 0.02 ms (publish + diff)   m ≈ 1.0 ms per memory-bound pass over 1.6M points
c(styler) ≈ 0.35 ms per 800k CVs at 8 threads      (5-node chain 1.72 ms, MEASURED)
```

### 11.2 Gates

Every gate is a CI test with a threshold; a red gate blocks the phase it belongs to. Tier 1
(headless scene index, sub-100 ms, no GL) is the primary suite; tier 2 is Storm through the EGL
harness on this very host (F13); tier 3 is `testusdview` under the scratchpad Xvfb (CPU only);
tier 4 is the workstation protocol.

| Gate | Tier | Assertion | Threshold |
|---|:--:|---|---|
| **E-1** chain throughput | 1 | 5-op chain, 100k×8, full run | ≤ 2.5 ms (MEASURED baseline 1.72–1.91) |
| **E-2** sparse edit | 1 | 1 % of chunks dirty on all nodes | ≤ 0.10 ms (baseline 0.035–0.044) |
| **E-3** last-param edit | 1 | terminal parameter only | ≤ 0.6 ms (baseline 0.18–0.41) |
| **E-4** capture | 1 | `GuideInterpolate` capture, 100k roots, 4k guides, 8 threads | ≤ 25 ms, and **linear** in roots |
| **E-5** memory | 1 | RSS at 1M×8, 5 nodes | ≤ 800 MB (baseline 668) |
| **E-6** recompile | 1 | append a node to a 200-node groom | ≤ 0.2 ms, and only 1 node rebuilt |
| **E-7** thread scaling | 1 | 1 → 8 threads on E-1 | ≥ 3× (baseline 3.8×) |
| **E-8** determinism | 1 | same inputs, 1 vs 8 threads, `-ffp-contract` off | bitwise identical |
| **SI-1** exactness | 1 | `points.size() == Σ curveVertexCounts` on every tile | hard assert (F5) |
| **SI-2** invalidation | 1 | a clump-amount edit dirties only `primvars/points/primvarValue` on the dirty tiles | exact locator set |
| **SI-3** cook count | 1 | 10 interactive events | exactly 10 commits, all on the main thread (S18) |
| **SI-4** no torn read | 1 | 8 readers × 100 publishes | 0 torn reads (F9) |
| **SI-5** chain order | 1 | usdGen lands after UsdSkel/RigExec, before Storm's plugins | exact chain dump (S1) |
| **S-1** Storm static | 2 | 100k×8 at refineLevel 2, 1280×720, EGL | ≤ 14 ms (interpolated baseline ~12) |
| **S-2** deform frame | 2 | scene-index points publish, 49 tiles | ≤ 2.0 ms delta over static |
| **S-3** one-tile edit | 2 | dirty 1 of 49 tiles | ≤ 1.2× static |
| **S-4** culling | 2 | camera framing ¼ of the groom | `itemsDrawn` drops ≥ 3× |
| **S-5** batches | 2 | 49 tiles, one material, one refineLevel | `drawBatches == 1`, `drawCalls == 1` |
| **S-6** no relocation | 2 | deform frames | `vboRelocated == 0` |
| **S-7** density scrub | 2 | parked-CV drag vs real count change | parked ≤ 1.5× a points-only frame |
| **T-1** brush move | 3 | press/move/release at 100k | ≤ 1 ms Python + engine per move |
| **T-2** pick | 1 | `PickCV` at 100k / 1M | ≤ 0.25 / 2.5 ms |
| **T-3** pick accuracy | 3 | CPU pick vs `view.pick()` at 100 pixels | disagreements only on occlusion |
| **T-4** freeze | 3 | one freeze on a 2 205-prim stage | ≤ 5 ms author + one `_resetGUI` |
| **T-5** async | 3 | plugin sets `_allowAsync` without the CLI flag | 1 `asyncAllow`, ~10 `asyncPoll`/s |
| **R-1** hdPrman parity | 4 | `usdrecord` with 3 motion samples | correct blur, `k·tail` evaluations, not `k·chain` |
| **R-2** MSAA / OIT quality | 4 | 1-px strands, A2C vs OIT at 1080p and 4K | visual sign-off |
| **R-3** Metal/Vulkan | 4 | `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` shader compile | 0 errors |

---

## 12. Roadmap (D7)

Estimates are engineer-weeks for one experienced OpenUSD engineer, and assume the prototypes in
`scratchpad/probes/` are carried into the repo rather than rewritten (appendix B of the plan).

| Phase | Deliverable | Carried from prototypes | Exit criteria | Gates | Est. |
|---|---|---|---|---|---:|
| **P0 Skeleton** | repo, CMake, codeless schema, four plugin registrations, headless test harness, EGL Storm harness | `eglctx.h`, `probeImagingPipeline` shape, usdRig's generated-plugInfo pattern | `usdview` loads the plugin; a `UsdGenGroom` prim produces one empty tile prim; chain-order dump matches S1 | SI-5 | 2 |
| **P1 Data plane** | `usdGenMath` + `usdGen`: SoA buffers, chunks/tiles, `UsdGenOp`, registry, compiler with Merkle digests, TBB arena, per-node caches, dirty router | `tbbBench.cpp`, `kernel.h`, `simdBench.cpp`, `sampler.h` | `ScatterRandom → Grow → Noise` runs on synthetic input; E-1/E-2/E-3/E-7/E-8 green | E-1…E-8 | 4 |
| **P2 First pixels** | adapters (prim + `UsdGenRestAPI`), registry/session, generation store, tile publisher, commit model, notice emission | `G-stage-free` probes 1/4/5, `evalsched/models.cpp` | 100k hairs on a static mesh render in Storm at refineLevel 2; SI-1…SI-4 green; S-1/S-5 green | SI-1…4, S-1, S-5 | 4 |
| **P3 Deform + guides** | `GuideInterpolate`, `DeformWithSurface`, reference lane, kd-tree capture, extents, `__dependencies`, pruning wrapper | `probe4_frozen_reentry_si`, `probe7_locators_and_reentry` | hair on a usdRig-deformed and a UsdSkel-skinned scalp deforms at ≤ 2 ms of usdGen per frame; S-2/S-3/S-4/S-6 green | E-4, S-2…S-6 | 4 |
| **P4 Look** | `usdGenShaders`, the three-terminal material, `st`/`displayColor` baking, image maps, the material tool | `usdGenHairPreview.glslfx`, `render_hair.cpp`, `bench_hair.cpp` | Storm look signs off against `$P/hair_pv_tangent.png`; golden images in CI through the EGL harness | L-1 (render-context check), S-1 | 3 |
| **P5 Styling** | `Clump` (multi-level), `Noise`, `Length`, `Width`, mask block, ramps, SeExpr + `UsdGenExprMap` | `seexpr_bench.cpp`, vendored SeExpr | the R4 chain in example (a) evaluates and looks right; E-1 still green with 7 nodes | E-1, E-4 | 4 |
| **P6 Freeze + brushes** | C ABI, `_usdGen` module, live overrides, CV pick, comb/grab/smooth/length, `SubtreeSnapshot` undo, freeze/commit | `cTransport.cpp`, `bpTransport.cpp`, `bench_stroke.py`, `probe1_freeze_undo.py`, `probe8_undo_fidelity.py` | R9 demo: freeze an operator, comb 2 000 CVs at ≥ 30 fps, commit, undo | T-1…T-4 | 5 |
| **P7 Render time** | motion profiles P1/P2, motion cache, `usdrecord`/hdPrman parity, MaterialX terminal | `mbbench.cpp` | one binary renders the same groom in Storm and hdPrman; R-1 green | R-1 | 3 |
| **P8 Instancing** | `Cards`, `Archives`, `Spheres`, instancer publisher, `InstanceDataSourceNames`, prototype rebasing | `instProbe*.cpp` | example (c) renders; pick round-trip resolves through `primOrigin` | T-INST-1/2 | 3 |
| **P9 v2 operators + Ptex + paint** | `Curl Bend Direction Smooth Straighten Wave Displace Resample ScatterUniform ScatterAtGuides`, Ptex read/write, paint brushes | `ptex_test.cpp` | artist-usable groom end to end | all | 5 |
| **P10 Hardening** | progressive generation, memory eviction, diagnostics HUD, workstation protocol, docs | — | R-2/R-3 run on a workstation; the plan's `./plan` directory published | R-2, R-3 | 3 |

**What the prototypes already de-risked** (do not re-litigate, do not re-measure before P2): the
engine choice and its numbers (S21–S24), the chain slot (S1), the commit model (S17–S19), the Storm
curve contract and its throughput (S27–S31), the glslfx look (S35), the instancer contract (S33),
the transport and picking numbers (S39–S40), the freeze contract and its costs (S41–S42), the
dependency and build story (S38, S44–S45).

---

## 13. Risks, and the conditions that stop or redirect the work

| # | Risk | Evidence it is real | Mitigation / stop condition |
|---|---|---|---|
| **R-1** | Storm, not usdGen, blows the frame budget at production density (500k–1M hairs). | 23.9 ms at 200k, MEASURED. 1M would be ~120 ms. | The LOD ladder is a *product* feature, not a fallback: decimate by stable id (S31), cull by tile extent, and expose `usdGen:lod:ratio` per description. **Stop condition:** if S-4 shows tile culling rejects nothing on a real head (tiles too spread out), change the chunk partition from Morton-over-roots to a UV-island partition before shipping P3. |
| **R-2** | Cross-chunk operators someone wants later (neighbour smooth, collide) break P3. | The data-plane report leaves cross-chunk kernels unmeasured. | They are v3 and they get their own lane: a per-frame grid rebuilt on the commit thread, evaluated as a *two-pass* node (gather then scatter), never as a chunk fan-in. If a v1 operator ever needs a hair-to-hair read, that is a design review, not a patch. |
| **R-3** | Uniform CV count per chunk (ASSUMPTION 1) is wrong for imported grooms. | Alembic/Unreal grooms routinely have mixed CV counts. | The ragged path exists (`cvCount == 0` + `cvOffsets`) and is correct but slower; `UsdGenResample` in v2 converts an imported groom to uniform. **Stop condition:** if the ragged path is more than 2× slower on E-1, promote `Resample` to v1 and make it automatic on import. |
| **R-4** | The primvar-descriptor / bare-leaf optimisation breaks if a caching filter is ever inserted between usdGen and the render index. | The optimisation is conditional on usdGen being the sole producer (`G-storm-throughput §1.9`). | SI-2 asserts the exact locator set; a second assertion in tier 2 checks `drawBatches` and re-sync counts. If a host inserts a caching SI, flip a single flag to `ComputeDirtyLocators` and pay 0.51 µs/prim/frame. |
| **R-5** | The `esfUsd/stageData.cpp:360` OpenExec resync bug turns freeze/undo into a diagnostic storm. | Reproduced with bare `ExecUsdSystem` (S41, S46). | Contain it (`TfErrorMark` in C++, `try/except` + post-condition assert in Python), never rely on `RemovePrim` interactively, batch removals in one `Sdf.ChangeBlock`. File upstream. |
| **R-6** | Storm's render-context material resolution does not work as ASSUMPTION 7 hopes, and the scene index has to author per-delegate bindings forever. | S36 marks it UNVERIFIED. | Gate L-1 answers it in P4 in an afternoon on this host. The fallback (author the binding we want) is already the plan, so the risk is only that the material is less portable. |
| **R-7** | The heterogeneous 10×X925 + 10×A725 topology makes any fixed thread count wrong on other hardware. | Both engines regress past ~8–10 threads here (MEASURED). | `USDGEN_THREAD_LIMIT`, a one-shot calibration at first commit (run E-1's kernel at 1/2/4/8/16 and pick the knee), and gate E-7 expressed as a *ratio*, not an absolute. |
| **R-8** | Progressive generation (`asyncAllow`/`asyncPoll`) cannot be enabled from a plugin in stock usdview. | Sound by code order but UNMEASURED (S20). | Gate T-5. If it fails, progressive generation requires `--allow-async` and is documented as such; nothing else in the design depends on it. |
| **R-9** | Memory at production density (1M hairs, 8-node chain) exceeds a workstation. | 668 MB at 1M×8 with 5 nodes, MEASURED. An 8-node chain is ~1.1 GB. | The eviction policy of §5.9 plus an opt-out per node (`usdGen:cache = false`, S24). **Stop condition:** if E-5 exceeds 1.5 GB at 1M with the default chain, make per-node caching opt-*in* above 500k curves. |
| **R-10** | hdGp-hosted third-party procedurals want to consume usdGen's curves. | S6 keeps hdGp out of the pipeline. | usdGen may later host its own resolver *downstream* of its own scene index; nothing in this design forecloses it, and nothing depends on it. |

---

## 14. Out of scope (deliberately)

Hair **simulation** (usdGen consumes simulated curves through `UsdGenCurveSource`, it does not solve
them); a node-graph authoring UI (the operator stack is a list, because the chain is a list — usdRig
has no node-graph editor to reuse either, A1 §8.2); GPU evaluation of the chain (HGI compute is
plausible on a GB10 but every number here is CPU, and Storm dominates the frame anyway — R-1);
grooming on non-mesh surfaces; per-instance material bindings (Storm has none, S33); automatic
pickup of overwritten map files (S13); Windows and macOS builds beyond keeping the CMake honest
(bison/flex and the Hgi resource path are the two known blockers, A8 §6.2, S35 open questions).

---

### Appendix — prototype inventory carried into the repo

| From `scratchpad/probes/` | Into |
|---|---|
| `data-plane-engine-prototype-benchmark/{tbbBench.cpp,kernel.h,simdBench.cpp,sampler.h}` | `usdGenMath/tests/benchChain.cpp`, `usdGenTestUtils/sampler.h` |
| `storm-hair-look/{eglctx.h,render_hair.cpp,bench_hair.cpp,usdGenHairPreview.glslfx}` | `usdGenTestUtils/eglctx.h`, `usdGenShaders/`, `tests/benchStorm.cpp` |
| `storm-throughput/{gen_hair_stages.py,hairbench.cpp,run_bench.sh}` | `tests/perf/` |
| `tool-loop/{cTransport.cpp,bpTransport.cpp,bench_stroke.py,bench_pick_cpp.py}` | `usdGenImaging/registry.cpp` (C ABI), `python/_usdGen.cpp`, `tests/perf/` |
| `freeze-bake/{probe1,probe4,probe7,probe8,probe10}` | `tests/testUsdGenFreeze*.py`, `tests/testUsdGenFrozenReentry.cpp` |
| `instancing/instProbe{,2,3,4}.cpp` | `tests/testUsdGenInstancer.cpp` |
| `G-stage-free/probe{1,4,5,6}.cpp` | `tests/testUsdGenAdapter.cpp` |
| `evalsched/{models.cpp,chainOrder.cpp,noticeCost.cpp}` | `tests/testUsdGenScheduling.cpp`, `tests/testUsdGenChainOrder.cpp` |
