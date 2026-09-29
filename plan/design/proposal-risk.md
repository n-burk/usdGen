# usdGen — architecture proposal (risk-and-delivery lens)

**Date:** 2026-09-04 · **Author lens:** ship vertical slices early, never rewrite.
**Constraints:** `design/brief-v1.md` §2 (S1–S46). Every S-number below is a settled decision and is
cited, not re-argued. Open areas D1–D8 are all answered concretely.

**Status vocabulary** (usdRig `docs/spec.md` voice): **EXISTS** = code or file present in the
scratchpad today · **MEASURED** = a number produced on this host · **BUILD** = builds here ·
**PLANNED** = specified, not written · **UNMEASURED** = needs the workstation protocol ·
**ASSUMPTION** = added beyond the evidence, flagged inline.

---

## 0. The thesis in one page

usdGen is three products fused by one binary: a **data-plane engine** (TBB DAG over SoA curve
buffers), a **Hydra 2.0 publisher** (renderer-level scene index minting chunked `basisCurves` and
instancers), and a **usdview toolset** (comb/paint/freeze). The delivery risk is not that any one
of them is hard — every load-bearing mechanism has already been probed on this machine — but that
the three are coupled by data contracts that are expensive to change once assets exist.

The proposal therefore fixes **five contracts in slice 1 and never changes them**:

| # | Contract | Frozen by | Why it must be first |
|---|---|---|---|
| C1 | The `usdGen:` property namespace and prim-type names (D1) | end of Slice 1 | assets and layers reference them; renaming is a migration |
| C2 | The published `basisCurves` chunk contract (S27–S31) | end of Slice 1 | Storm/hdPrman/tools all read it; it is also the freeze format |
| C3 | The frozen-curve contract (S42) | end of Slice 2 | it is the *same* contract as C2 plus four primvars; guides, freezes, imports, sim caches all use it |
| C4 | The C ABI + pxr_boost array surface (S39) | end of Slice 5 | ctypes has no compile-time check; a signature change is a silent breakage |
| C5 | The glslfx parameter names (S35) | end of Slice 1 | they are `UsdShade` inputs on shipped materials |

Everything else — the `UsdGenOp` v-table, chunk views, the graph headers, the imaging internals —
is **deliberately not installed** and may churn freely. That split is the single most important
delivery decision in this document: it lets slices 2–8 rewrite the engine's guts without touching
one line of an artist's stage.

The chain placement (S1), the evaluation model (S17–S19), the engine choice (S21–S26), the prim
granularity (S27–S31), the instancing contract (S33–S34), the look (S35–S37), the third-party set
(S38) and the tool loop (S39–S43) are all settled. What this proposal adds is: exact names, the
schema, the three canonical `.usda` files, the operator catalogue split across three releases, the
slice boundaries with exit criteria and test gates, and the risk register with stop conditions.

---

## 1. Delivery strategy: eight slices, each shippable

A "slice" here means: an artist can open usdview on a stage, see the feature working, and a CI job
proves it headlessly. Nothing lands that cannot be demonstrated end-to-end.

| Slice | Name | Delivers | Frozen contract | Gate |
|---|---|---|---|---|
| S0 | Skeleton | repo, CMake, codeless schema, both plugin registrations, chain-order assertion | — | `testUsdGenChainOrder` |
| **S1** | **Straight hair** | ScatterRandom → GrowFromRoots → Noise → 32 chunk prims → glslfx | **C1, C2, C5** | headless SI suite + EGL Storm golden |
| S2 | Deform & freeze | DeformWithSurface, CurveSource, freeze T1/T2, rest, ext-comp pruning | **C3** | deformed-scalp SI test, freeze/undo test |
| S3 | Guides & clumps | GuideInterpolate, Clump (multi-level), guide prims, kd-tree capture | — | clump determinism + cross-chunk test |
| S4 | Maps & expressions | Image/Ptex/Expr/Paint maps, SeExpr, CPU colour bake | — | map golden values, SeExpr parity |
| S5 | Tools | comb/paint live-override loop, freeze tool, undo, panels | **C4** | testusdview suite |
| S6 | Instancing | cards, archives, native-instance grooms | — | instancing SI tests + Storm golden |
| S7 | Render-time | motion profiles P0/P1/P2, hdPrman validation, perf hardening | — | workstation protocol |
| S8 | Breadth | v2 operator catalogue, LOD, async | — | per-operator unit tests |

**Why this order.** S1 proves the riskiest *structural* claims (chain placement, exact-size arrays,
adapter invalidation, glslfx binding) on the smallest possible feature. S2 proves the second-riskiest
(reading deformed/skinned surfaces, the freeze re-entry) and, critically, freezes C3 — which is the
contract that guides, freezes, imports and sim caches all share, so getting it right once removes
four future migrations. Only then do we build breadth.

**What the prototypes already de-risked** (this is why the schedule is aggressive):

| De-risked | Artefact | Status |
|---|---|---|
| Renderer-level placement, ordering stability | `probes/G-chain-order/` | MEASURED |
| TBB DAG beats VDF 4–15× | `probes/data-plane-engine-prototype-benchmark/` | MEASURED |
| Storm renders hair headlessly on the GB10 | `probes/storm-hair-look/eglctx.h` | MEASURED |
| The hair glslfx compiles and looks right | `probes/storm-hair-look/usdGenHairPreview.glslfx` | EXISTS |
| Codeless schema + `DataSourceMapped` + `TsSpline` transport | `probes/G-stage-free/` | MEASURED |
| Frozen prim re-enters from the SI, locator taxonomy | `probes/freeze-bake/probe7` | MEASURED |
| Instancer synthesis, `instancedBy`, `primOrigin` | `probes/instancing/` | MEASURED |
| ctypes + pxr_boost transport, CPU CV pick | `probes/tool-loop/` | MEASURED |
| SeExpr + Ptex build and perform | `thirdparty/{seexpr,ptex}` | BUILD |
| usdRig builds and is linkable out of tree | `probes/B-build/consumer/` | BUILD |

Roughly 60 % of the "will it even work" questions are already answered with running code. The
residual risk is concentrated in **Metal/Vulkan Hgi**, **hdPrman on a real RenderMan install**, and
**Storm GPU behaviour at production prim counts** — all three of which are quarantined to S7 and
have workstation protocols already written.

---

## 2. D8 — Library decomposition, naming, CMake

Sibling CMake project at `<usdgen-src>`, consuming the unmodified OpenUSD install,
mirroring usdRig's layout and generated-plugInfo pattern verbatim (S44).

| CMake target | Kind | Installed? | Links | Contents |
|---|---|---|---|---|
| `usdGenMath` | STATIC, PIC, `-fvisibility=hidden`, `-ffp-contract=off` | headers **no** | `tf gf vt work TBB::tbb`, vendored `nanoflann`, optional `rigExec::rigExecMath` | kernels: noise façade, RMF frames, arc-length resample, kd-tree wrappers, barycentric/Poisson scatter, segment-length restore, extent |
| `usdGen_seexpr` | STATIC, hidden | no | `dl pthread` | vendored wdas/SeExpr `main`@8f8c8f2, interpreter only (S38) |
| `usdGen_ptex` | STATIC, hidden | no | `ZLIB::ZLIB Threads::Threads` | vendored Ptex v2.4.3 (S38) |
| `usdGen` | SHARED | **yes** `lib/` | `usdGenMath usdGen_seexpr usdGen_ptex ar sdf vt gf tf work trace hio pxOsd TBB::tbb` | the graph/evaluator: `UsdGenOp`, `UsdGenGraph`, `UsdGenCurveBuffer`, map/expr/Ptex evaluation, motion cache. **No `usd`, no `usdImaging`, no `hd`.** |
| `usdGenImaging` | SHARED | **yes** `lib/` | `usdGen hd hdsi usdImaging usdGeom usd sdf` | scene indices, registry/session, snapshot store, chunk & instancer builders, prim adapter, **the `extern "C"` ABI** |
| `usdGenSchema` | resource plugin | `lib/usd/usdGenSchema/resources` | — | codeless `generatedSchema.usda` + generated `plugInfo.json` (`Type: library`, `LibraryPath` → `usdGenImaging`, for `implementsComputeExtent`) |
| `usdGenShaders` | resource plugin | `lib/usd/usdGenShaders/resources` | — | `usdGenHairPreview.glslfx`, `usdGenHairPreviewTranslucent.glslfx`, `shaderDefs.usda`, `usdGenHair.mtlx` |
| `_usdGen` | MODULE (pxr_boost.python) | `lib/python/usdGen/` | `usdGenImaging usd_vt usd_gf usd_tf usd_boost python3.12` | array transport (S39) |
| `usdGenUsdview` | python package | `lib/python/usdGenUsdview/` | — | `plugInfo.json` (`Type: python`) + every module (see §8) |
| `usdGenTest` | STATIC | no | `usdGen usdGenImaging hd` | SI recording observer, EGL harness, stage builders |

**Why `usdGen` core links no `usd`.** S8 says no design element may require a `UsdStage` downstream
of the stage scene index. Making that a *link-time* property rather than a review rule is the
cheapest possible enforcement: if someone reaches for `UsdStage` in the evaluator, the build breaks.
`sdf` is linked for `SdfPath`/`SdfAssetPath` only — those are values, not stage access.

**CMake specifics that are already known to bite:**

```cmake
cmake_minimum_required(VERSION 3.26)
project(usdGen VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)                      # match the install's GCC 13 / libstdc++ ABI
set(USD_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../OpenUSD_26_08" CACHE PATH "")
list(APPEND CMAKE_PREFIX_PATH "${USD_INSTALL_DIR}")   # pxrConfig's find_dependency() needs this

option(USDGEN_WITH_RIGEXEC "Link rigExec::rigExecMath" ON)
if (USDGEN_WITH_RIGEXEC)
  find_package(rigExec CONFIG QUIET)             # brings pxr in transitively — S44
endif()
if (NOT TARGET usd)                              # the CMake 3.28 double-pxrConfig trap (S44)
  find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)
endif()

# kernels only; the rest of the project keeps the default (B-usdrig-build §4)
target_compile_options(usdGenMath PRIVATE -ffp-contract=off)
```

`usdGenConfig.cmake.in` wraps its own dependency in `if (NOT TARGET usd)` and exports
`usdGen_PLUGINPATHS`, `usdGen_LIBRARY_DIR`, `usdGen_PYTHON_DIR` (S44). Install layout mirrors USD
exactly. `$ORIGIN` rpath + `CMAKE_INSTALL_RPATH_USE_LINK_PATH ON`.

`bin/_env.sh` derives site-packages by globbing `lib/python*/site-packages` (never hard-coding
3.11/3.12), exports `USDGEN_IMAGING_DLL` for out-of-tree builds, and works on Linux — the three
concrete defects found in usdRig's helper (S46, A3 §5.3, B-usdrig-build §6).

**Delivery consequence.** Nine targets is more than a small team wants on day one. S0 ships
`usdGenMath`, `usdGen`, `usdGenImaging`, `usdGenSchema` only; `usdGen_seexpr`/`usdGen_ptex` land in
S4, `usdGenShaders` in S1, `_usdGen`/`usdGenUsdview` in S5. The **target names and directory layout
are fixed at S0** so nothing moves later.

---

## 3. D1 — Schema

All prim types are **codeless** (`skipCodeGeneration = true`, S9), generated with usdRig's
`gen_schema.sh` tooling into `usdGenSchema/resources/generatedSchema.usda`. Every `usdGen:*`
attribute and relationship reaches Hydra through the prim adapter (S10), never through
`primvars:usdGen:*` in shipped code — that form remains legal as a prototyping escape hatch and is
what the S0 slice uses before the adapter exists.

### 3.1 Type hierarchy

```
UsdTyped
├── UsdGeomImageable
│   ├── UsdGenGroom            (concrete)  collection scope; session identity, LOD, defaults
│   ├── UsdGenDescription      (concrete)  one groom description: surface + terminal + output config
│   └── UsdGenGuideSet         (concrete)  a named guide set (purpose = guide)
├── UsdGenOperator             (ABSTRACT)  everything with usdGen:input
│   ├── UsdGenGenerator        (ABSTRACT)  may change curve/CV counts
│   │   ├── UsdGenScatterRandom · UsdGenScatterUniform · UsdGenScatterPoints · UsdGenScatterAtGuides
│   │   ├── UsdGenGrowFromRoots · UsdGenGuideInterpolate
│   │   └── UsdGenCurveSource            (frozen / imported / simulated curves enter here)
│   ├── UsdGenStyler           (ABSTRACT)  no topology change
│   │   ├── UsdGenClump · UsdGenNoise · UsdGenCurl · UsdGenBend · UsdGenDirection
│   │   ├── UsdGenLength · UsdGenWidth · UsdGenSmooth · UsdGenStraighten
│   │   └── UsdGenDisplace · UsdGenWave · UsdGenScale · UsdGenResample
│   ├── UsdGenDeformer         (ABSTRACT)  deformed-space; re-runs per motion sample
│   │   ├── UsdGenDeformWithSurface · UsdGenShrinkwrap · UsdGenWind
│   ├── UsdGenFreeze           (concrete)  snapshot marker; deactivates upstream when live
│   ├── UsdGenSculptLayer      (concrete)  per-CV deltas in the root frame, keyed by curveId
│   ├── UsdGenCardSet          (concrete)  emits an instancer of card prototypes
│   └── UsdGenArchiveSet       (concrete)  emits an instancer of subtree prototypes
└── UsdGenMap                  (ABSTRACT)
    ├── UsdGenImageMap · UsdGenPtexMap · UsdGenExprMap · UsdGenPaintMap

API schemas (single-apply):
    UsdGenMaskAPI    — the universal mask block, applicable to any UsdGenOperator
    UsdGenLookAPI    — colour/width baking config, applicable to UsdGenDescription
    UsdGenRestAPI    — applicable to a bound surface; publishes usdGen/rest/points at Default() (S12)
```

The adapter is registered **once**, on the abstract `UsdGenOperator` with
`includeDerivedPrimTypes: true`, and once on `UsdGenMap` (S10). One adapter class serves every
concrete type because the mappings are built generically from
`UsdPrimDefinition::GetPropertyNames()` (G-stage-free §1, closes A4's open question). Adding an
operator type in S8 therefore requires **zero imaging code** — only a schema entry and a kernel.
That is the single biggest lever on the cost of the v2/v3 catalogue.

### 3.2 Common operator properties (`UsdGenOperator`)

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` (ordered) | — | upstream operator prim(s). Empty = source. Kahn order, namespace order tie-break (S26) |
| `usdGen:readPhase` | `uniform token` | `preceding` | `base \| preceding \| final \| <abs prim path>` — which generation of the *surface* this op reads (S26) |
| `usdGen:active` | `bool` | `true` | false = pass-through (identity), still in the graph |
| `usdGen:seed` | `int` | `0` | per-operator random seed |
| `usdGen:cacheOutput` | `uniform bool` | `true` | false = opt out of the per-node buffer to save memory (S24) |
| `usdGen:label` | `string` | `""` | UI only |

### 3.3 `UsdGenGroom` and `UsdGenDescription`

| `UsdGenGroom` property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:sessionId` | `uniform string` | `""` | stable key for the process-global registry when the stage-cache accelerator is absent (S15, G-stage-free §7) |
| `usdGen:lod:mode` | `token` | `adaptive` | `off \| fixed \| adaptive` |
| `usdGen:lod:density` | `float` | `1.0` | curve-count multiplier for interaction. **Never** a refineLevel change (S31) |
| `usdGen:schemaVersion` | `uniform int` | `1` | forward-compat gate; see §3.9 |

| `UsdGenDescription` property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:surface` | `rel` | — | one or more bound `Mesh` prims (or instance proxies, translated by S7) |
| `usdGen:terminal` | `rel` | — | exactly one operator prim whose output is published |
| `usdGen:primitiveType` | `uniform token` | `splines` | `splines \| cards \| archives \| spheres` |
| `usdGen:chunkCurves` | `uniform int` | `512` | curves per chunk prim; clamped to [128,1024] (S23) |
| `usdGen:chunkCountMax` | `uniform int` | `256` | prim-set size cap; clamped to [32,256] (S27) |
| `usdGen:curve:type` | `uniform token` | `cubic` | S29 |
| `usdGen:curve:basis` | `uniform token` | `bspline` | `bspline \| catmullRom`; `centripetalCatmullRom`/`bezier` rejected at compile (S29) |
| `usdGen:curve:wrap` | `uniform token` | `pinned` | S29 |
| `usdGen:curve:cvCount` | `int` | `8` | CVs per strand |
| `usdGen:display:refineLevel` | `uniform int` | `2` | pinned per chunk prim (S29) |
| `usdGen:display:minScreenSpaceWidths` | `float` | `1.0` | S29 |
| `usdGen:motion:mode` | `uniform token` | `single` | `single \| velocities \| samples` = P0/P1/P2 (S32) |
| `usdGen:motion:sampleCount` | `uniform int` | `3` | clamped [2,16] (S32) |
| `usdGen:motion:forwardSurfaceSamples` | `uniform bool` | `false` | forward the surface's own contributing times instead of clamping to the shutter |
| `usdGen:guides` | `rel` | — | `UsdGenGuideSet` prims visible to this description |
| `usdGen:renderDensity` | `float` | `1.0` | render-time-only multiplier (a host groomer `Render Density Multiplier`, A7 §1.5) |

Chunk prims are minted as `<Description>/Chunks/chunk_NNNN`, guides as
`<Description>/Guides/<guideSetName>`, instancers as `<Description>/Instancers/<name>` with
prototypes as their **namespace children** (S33). The prim set is allocated once from
`min(chunkCountMax, ceil(maxCurves / chunkCurves))` and never changes during interaction (S27, S28).

### 3.4 The mask block (`UsdGenMaskAPI`)

One block on every operator — the single most-reused piece of the schema, so it is worth getting
exactly right once (A7 §7 recommendation).

| Property | Type | Default |
|---|---|---|
| `usdGen:mask:map` | `rel` | — (a `UsdGenMap` prim) |
| `usdGen:mask:amount` | `float` | `1.0` |
| `usdGen:mask:invert` | `bool` | `false` |
| `usdGen:mask:combine` | `token` | `multiply` (`multiply\|add\|max\|min\|replace`) |
| `usdGen:mask:ramp:knots` | `float2[]` | `[(0,1),(1,1)]` — `(position, value)` |
| `usdGen:mask:ramp:interpolation` | `token` | `linear` (`none\|linear\|smooth\|spline\|monotone`, SeExpr `curve()` codes 0–4) |
| `usdGen:mask:range:mode` | `token` | `normalized` (`normalized\|absoluteLength`) |
| `usdGen:mask:range` | `float2` | `(0,1)` |
| `usdGen:mask:effectPosition` | `float` | `0.5` |
| `usdGen:mask:falloff` | `float` | `1.0` |
| `usdGen:mask:noise:amount` | `float` | `0.0` |
| `usdGen:mask:noise:frequency` | `float` | `1.0` |
| `usdGen:mask:noise:gain` | `float` | `0.5` |
| `usdGen:mask:noise:bias` | `float` | `0.5` |
| `usdGen:mask:noise:seed` | `int` | `0` |

Semantics: per-curve weight `w(h) = clamp(amount × map(rootUV) × noise(rootRest) , 0, 1)`, inverted
if asked; per-CV weight `w(h) × ramp(t)` with the a DCC four-parameter shortcut applied when the
ramp has fewer than two authored knots. Evaluated **once per capture** and cached as a
`VtFloatArray` (S25).

### 3.5 Ramps — the shape that avoids a per-frame recook

S11 forbids a plain `.spline` on a parameter. Two encodings ship, both stage-free and both
verified in `probes/G-stage-free`:

* **Scalar ramp (default).** `float2[] usdGen:<p>:knots` (x = position 0..1, y = value) +
  `token usdGen:<p>:interpolation`. Never time-varying, never dirtied on `SetTime`.
* **Colour ramp.** `float[] usdGen:<p>:positions` + `color3f[] usdGen:<p>:colors` + the same
  interpolation token. (`TsSpline` supports only `double/float/GfHalf/GfTimeCode`, so a colour ramp
  cannot be a spline — G-stage-free §2.1.) **ASSUMPTION:** S11's literal "`float2[] knots` +
  `float[] values`" is read as "one knot array per ramp channel"; the two forms above are that rule
  specialised for scalars and colours.
* **Optional `TsSpline` ramp.** Any property named `usdGen:<p>:spline` (`float`, authored with
  `.spline`) is published by the adapter as `HdTypedSampledDataSource<TsSpline>` via a custom
  `AttributeMapping::factory` that does **not** call `FlagAsTimeVarying`; the spline's parameter
  axis is reinterpreted as root→tip `u` (probe6, MEASURED). This buys native USD authoring and full
  Ts interpolation modes at zero per-frame cost.

Genuinely animated scalars (global density, a wind gust) keep `.spline` on the plain property and
are excluded from the structural digest (S26).

### 3.6 Maps

| Type | Properties |
|---|---|
| `UsdGenImageMap` | `asset usdGen:file`, `token usdGen:uvSet = "st"`, `token usdGen:channel = "r"` (`r\|g\|b\|a\|rgb`), `token usdGen:wrap = "clamp"`, `float usdGen:scale = 1`, `float usdGen:bias = 0`, `token usdGen:colorSpace = "raw"` |
| `UsdGenPtexMap` | `asset usdGen:file`, `token usdGen:filter = "bilinear"`, `float usdGen:blur = 0`, `int usdGen:firstChannel = 0`, `int usdGen:channelCount = 1`, `token usdGen:borderMode = "clamp"` |
| `UsdGenExprMap` | `string usdGen:expression`, `token usdGen:returnType = "float"` (`float\|float3`), `int usdGen:seed = 0`, `rel usdGen:maps` (named `UsdGenMap` prims reachable from `map("<primName>")`) |
| `UsdGenPaintMap` | `rel usdGen:surface`, `token usdGen:primvarName`, `token usdGen:storage = "primvar"` (`primvar\|file`), `asset usdGen:file`, `int usdGen:resolution = 256` |

**One `asset` per map prim, never `asset[]`** (S13): an `asset[]` gets no reload tracking because
`FlagAsAssetPathDependent` is specialised for scalar `SdfAssetPath` only. Multiple maps means
multiple map prims. Reload is the explicit `UsdGenImaging_ReloadMaps()` action (S13, §7.5).

### 3.7 Freeze and sculpt

`UsdGenFreeze` — the a host groomer *Groom Bake* analogue (A7 §1.4): everything upstream is deactivated while
the freeze is live; everything downstream reads the frozen buffer (A7 §9.3).

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` | — | the operator whose output was frozen |
| `usdGen:frozenCurves` | `rel` | — | the authored `UsdGeomBasisCurves` sibling holding the snapshot |
| `usdGen:frozen` | `bool` | `false` | `true` = read `usdGen:frozenCurves`, do not evaluate upstream |
| `usdGen:frozenEpoch` | `uniform string` | `""` | must equal the frozen prim's `primvars:usdGen:frozenEpoch` or the freeze is *stale* (warn, keep rendering the frozen data) |

`UsdGenCurveSource` — the same read path with no upstream at all: imports, Alembic-converted sim
caches, hand-drawn curves. `rel usdGen:curves`, `bool usdGen:useRest = true`.

`UsdGenSculptLayer` — deltas in the root frame keyed by stable `curveId` so surface deformation and
upstream parameter tweaks still apply (A7 §9.3).

| Property | Type | Meaning |
|---|---|---|
| `usdGen:weight` | `float` (default 1.0) | layer weight 0..1 (a host groomer sculpt-layer semantics) |
| `usdGen:sculpt:curveIds` | `int[]` | which curves have deltas, sorted |
| `usdGen:sculpt:cvOffsets` | `int[]` | prefix offsets into `deltas`, size `curveIds.size()+1` |
| `usdGen:sculpt:deltas` | `vector3f[]` | per-CV delta in that curve's root frame |
| `usdGen:sculpt:epoch` | `uniform string` | the epoch the deltas were authored against; mismatch = "rebase needed" warning, deltas for missing ids are kept and ignored |
| `usdGen:sculpt:lockedCurves` | `int[]` | Freeze-brush ids: downstream stylers are zeroed for these curves |

Sculpt deltas are authored *outside* the frozen prim, so a re-freeze does not lose hand work, and a
layer can be muted/weighted without touching the geometry — the property that makes S41's
"undo of a live freeze = `SetActive(false)`" cheap.

### 3.8 The frozen / guide / imported curve contract (C3)

Verbatim S42, and it is the *same* prim shape usdGen publishes for chunks (C2) plus four primvars.
One contract, four producers.

```
def BasisCurves "guides_0000" (prepend apiSchemas = ["UsdGenCurveAPI"]) {
    uniform token   type  = "cubic"
    uniform token   basis = "bspline"
    uniform token   wrap  = "pinned"
    int[]           curveVertexCounts = [...]              # Σ == points.size()  (S28)
    point3f[]       points            = [...]
    float[]         primvars:widths (interpolation = "vertex")
    point3f[]       primvars:rest   (interpolation = "vertex")            # free while it shares points (S42)
    int[]           primvars:skinprim   (interpolation = "uniform")       # bound face index
    texCoord2f[]    primvars:skinprimuv (interpolation = "uniform")       # NOT named "st"
    int[]           primvars:usdGen:curveId (interpolation = "uniform")
    string          primvars:usdGen:frozenEpoch (interpolation = "constant")   # constant primvar, never customData (S42)
    matrix4d[]      primvars:usdGen:rootFrame (interpolation = "uniform")      # optional
    token           primvars:usdGen:role (interpolation = "constant")          # "hair" | "guide"
    uniform token   purpose = "guide"                                          # guide sets only
}
```

Consumers must test for a *value*, not for the primvar's presence: `velocities`, `accelerations`
and `normals` are always listed by the gprim data source with size 0 (freeze-bake §2.1).

### 3.9 Versioning and forward compatibility

* `usdGen:schemaVersion` on the groom. A binary refuses to evaluate a groom whose version exceeds
  its own and emits one `TF_WARN` per groom, then publishes nothing for that description (the rest
  of the stage renders). Rationale: a silent partial groom is worse than an absent one.
* Property removal is never done in place: a removed property keeps its schema entry marked
  `hidden = true` for one release and the evaluator ignores it.
* Every kernel that changes numerical behaviour bumps `usdGen:<op>:algorithmVersion`
  (`uniform int`, default = the current version). Old assets keep their look; new assets get the
  new default. This is the mechanism that lets us fix a clump or a noise kernel in S8 without
  re-rendering a show. **ASSUMPTION** — no S-number covers versioning; it is a delivery-risk
  addition and it costs one integer per operator type.

### 3.10 Canonical example (a) — generate and style on a usdRig-deformed scalp

Chain: `scatter → guide interpolate → clump → clump → frizz`, exactly the shape R4 asks for.

```usda
#usda 1.0
(
    defaultPrim = "Char"
    upAxis = "Y"
)

def Xform "Char" {

    # ---- the deforming scalp. usdRig (or UsdSkel, or anything) owns its points at
    #      render time; usdGen reads whatever the scene index carries. (S5, R5)
    def Mesh "Scalp" (prepend apiSchemas = ["UsdGenRestAPI"]) {
        int[]        faceVertexCounts = [...]
        int[]        faceVertexIndices = [...]
        point3f[]    points = [...]                       # default = rest; timeSamples = deformed
        texCoord2f[] primvars:st (interpolation = "faceVarying") = [...]
        # usdGen:rest/points is published by the UsdGenRestAPI adapter, sampling
        # UsdTimeCode::Default(); an authored primvars:rest wins if present.  (S12)
    }

    def "Rig" ( references = @./ArmRig.usda@</Rig> ) { }   # usdRig drives Scalp.points

    # ---- the groom
    def UsdGenGroom "Groom" {
        uniform string usdGen:sessionId = "char.hair.v001"
        float          usdGen:lod:density = 1.0

        def UsdGenGuideSet "HeadGuides" {
            rel usdGen:curves = </Char/Groom/HeadGuides/Curves>
            float[] usdGen:blend = []                     # per-guide range-of-influence (a host groomer "Blend")
            def BasisCurves "Curves" {
                uniform token type = "cubic" ; uniform token basis = "bspline" ; uniform token wrap = "pinned"
                uniform token purpose = "guide"
                int[]      curveVertexCounts = [8, 8, 8, 8]
                point3f[]  points = [ ... 32 CVs ... ]
                float[]    primvars:widths (interpolation = "vertex") = [...]
                point3f[]  primvars:rest (interpolation = "vertex") = [...]
                int[]      primvars:skinprim (interpolation = "uniform") = [12, 40, 71, 96]
                texCoord2f[] primvars:skinprimuv (interpolation = "uniform") = [...]
                int[]      primvars:usdGen:curveId (interpolation = "uniform") = [0, 1, 2, 3]
                token      primvars:usdGen:role (interpolation = "constant") = "guide"
            }
        }

        def UsdGenDescription "Hair" (
            prepend apiSchemas = ["UsdGenLookAPI", "MaterialBindingAPI"]
        ) {
            rel usdGen:surface  = </Char/Scalp>
            rel usdGen:terminal = </Char/Groom/Hair/Ops/Frizz>
            rel usdGen:guides   = </Char/Groom/HeadGuides>
            rel material:binding = </Char/Groom/Hair/Look/HairMat>

            uniform token usdGen:primitiveType = "splines"
            uniform int   usdGen:chunkCurves   = 512
            uniform int   usdGen:chunkCountMax = 128
            uniform int   usdGen:curve:cvCount = 8
            uniform token usdGen:motion:mode   = "single"       # P0; Storm never samples (S32)

            color3f usdGen:look:rootColor = (0.035, 0.018, 0.008)
            color3f usdGen:look:tipColor  = (0.210, 0.115, 0.045)
            float   usdGen:look:hueJitter = 0.06

            def Scope "Maps" {
                def UsdGenImageMap "DensityMap" {
                    asset usdGen:file = @./maps/scalp_density.exr@
                    token usdGen:uvSet = "st"
                    token usdGen:channel = "r"
                }
                def UsdGenExprMap "LengthVar" {
                    string usdGen:expression = "0.8 + 0.4*rand($id) + 0.3*fbm($Pref*3, 3)"
                }
            }

            def Scope "Ops" {

                # 1. scatter: stable ids from (seed, faceIndex, k)   (A7 §9.1 G1)
                def UsdGenScatterRandom "Scatter" (prepend apiSchemas = ["UsdGenMaskAPI"]) {
                    float usdGen:density        = 120000
                    int   usdGen:seed           = 7
                    int   usdGen:relaxIterations = 4
                    bool  usdGen:areaCompensate = true
                    rel   usdGen:mask:map = </Char/Groom/Hair/Maps/DensityMap>
                }

                # 2. guide interpolation: the "curve generator"    (A7 §9.1 G6)
                def UsdGenGuideInterpolate "Interp" {
                    rel   usdGen:input = </Char/Groom/Hair/Ops/Scatter>
                    int   usdGen:maxGuides       = 3
                    float usdGen:influenceRadius = 4.0
                    float usdGen:influenceDecay  = 2.0
                    float usdGen:maxGuideAngle   = 90
                    float usdGen:blendInSkinSpace = 1.0
                    token usdGen:blendMethod     = "linearBlend"
                    int   usdGen:cvCount         = 8
                    float usdGen:randomizeGuide  = 0.15
                    float2[] usdGen:lengthScale:knots = [(0,1),(1,1)]
                    rel   usdGen:lengthScale:map = </Char/Groom/Hair/Maps/LengthVar>
                }

                # 3. coarse clump                                   (A7 §9.2 S1)
                def UsdGenClump "ClumpBig" (prepend apiSchemas = ["UsdGenMaskAPI"]) {
                    rel   usdGen:input = </Char/Groom/Hair/Ops/Interp>
                    float usdGen:clump          = 0.65
                    float usdGen:clumpDensity   = 900
                    int   usdGen:clumpSeed      = 11
                    float usdGen:volumize       = 0.15
                    float usdGen:preserveLength = 0.9
                    float usdGen:strayAmount    = 0.4
                    float usdGen:strayRate      = 0.03
                    float2[] usdGen:clumpScale:knots = [(0,0.05),(0.35,0.7),(1,1)]
                    token    usdGen:clumpScale:interpolation = "smooth"
                }

                # 4. fine clump on top of the coarse one — the "chain at will" case
                def UsdGenClump "ClumpSmall" {
                    rel   usdGen:input = </Char/Groom/Hair/Ops/ClumpBig>
                    float usdGen:clump        = 0.5
                    float usdGen:clumpDensity = 3600          # a host groomer: 2x-4x the first map (A7 §1.4)
                    int   usdGen:clumpSeed    = 12
                    float usdGen:noise        = 0.08
                    float usdGen:noiseFrequency = 6.0
                    float usdGen:noiseCorrelation = 0.4
                }

                # 5. frizz                                          (A7 §9.2 S2)
                def UsdGenNoise "Frizz" (prepend apiSchemas = ["UsdGenMaskAPI"]) {
                    rel   usdGen:input = </Char/Groom/Hair/Ops/ClumpSmall>
                    float usdGen:magnitude   = 0.035
                    float usdGen:frequency   = 9.0
                    float usdGen:correlation = 0.2
                    int   usdGen:octaves     = 3
                    token usdGen:space       = "rest"          # follows the deforming surface
                    float usdGen:preserveLength = 1.0
                    float2[] usdGen:magnitudeScale:knots = [(0,0),(0.4,0.3),(1,1)]
                }
            }

            def Scope "Look" {
                def Material "HairMat" {
                    token outputs:surface.connect        = </Char/Groom/Hair/Look/HairMat/Preview.outputs:surface>
                    token outputs:glslfx:surface.connect = </Char/Groom/Hair/Look/HairMat/Storm.outputs:surface>
                    token outputs:mtlx:surface.connect   = </Char/Groom/Hair/Look/HairMat/MtlxHair.outputs:surface>

                    def Shader "Storm" {                          # discovered from usdGenShaders/shaderDefs.usda
                        uniform token info:id = "UsdGenHairPreview"
                        color3f inputs:rootColor = (0.035, 0.018, 0.008)
                        color3f inputs:tipColor  = (0.210, 0.115, 0.045)
                        float   inputs:specular1Gain = 0.30
                        float   inputs:specular2Gain = 0.16
                        float   inputs:randomHue     = 0.06
                        token   outputs:surface
                    }
                    def Shader "Preview" { uniform token info:id = "UsdPreviewSurface" ; ... }
                    def Shader "MtlxHair" { uniform token info:id = "ND_chiang_hair_bsdf" ; ... }
                }
            }
        }
    }
}
```

The three chunk prims, the `st`/`hairT`/`hairTangent`/`hairId`/`displayColor` primvars and the
`extent` are **not authored** — the renderer-level scene index mints them (§5). Nothing in this file
is written by the evaluator (R1).

### 3.11 Canonical example (b) — frozen curves deformed with the surface, plus a sculpt layer

```usda
def UsdGenGroom "Groom" {
    def UsdGenDescription "Fur" (prepend apiSchemas = ["MaterialBindingAPI"]) {
        rel usdGen:surface  = </Char/Body>
        rel usdGen:terminal = </Char/Groom/Fur/Ops/Sculpt>
        rel material:binding = </Char/Groom/Fur/Look/FurMat>
        uniform int usdGen:chunkCurves = 512

        # The frozen data. A plain BasisCurves — S42's contract. It is what the freeze tool
        # wrote (tier T2, a sidecar .usdc sublayer), or what an Alembic sim cache converted to.
        def Scope "Frozen" {
            def BasisCurves "fur_v003" {
                uniform token type = "cubic" ; uniform token basis = "bspline" ; uniform token wrap = "pinned"
                int[]        curveVertexCounts = [...]                    # 120 000 curves
                point3f[]    points = [...]                               # deformed at freeze time
                float[]      primvars:widths (interpolation = "vertex") = [...]
                point3f[]    primvars:rest (interpolation = "vertex") = [...]
                int[]        primvars:skinprim (interpolation = "uniform") = [...]
                texCoord2f[] primvars:skinprimuv (interpolation = "uniform") = [...]
                int[]        primvars:usdGen:curveId (interpolation = "uniform") = [...]
                string       primvars:usdGen:frozenEpoch (interpolation = "constant") = "sha1:9f3c1e…"
                token        primvars:usdGen:role (interpolation = "constant") = "hair"
            }
        }

        def Scope "Ops" {
            # source: the frozen prim re-enters the graph straight from the scene index.
            # A frozen prim and a generated prim are the same kind of styler input. (S42)
            def UsdGenCurveSource "Source" {
                rel  usdGen:curves = </Char/Groom/Fur/Frozen/fur_v003>
                bool usdGen:useRest = true
            }

            # deform: rest -> animated, via the per-root frame from skinprim/skinprimuv.  (A7 §9.1 G7)
            def UsdGenDeformWithSurface "Deform" {
                rel   usdGen:input = </Char/Groom/Fur/Ops/Source>
                token usdGen:readPhase = "final"           # read the surface AFTER every other modifier
                token usdGen:mode      = "rigidFrame"      # rigidFrame | rbf | pointDeform
                bool  usdGen:twistAware = true
                int   usdGen:preserveShape:iterations = 0  # Cosserat pass, off by default
            }

            # hand-combed deltas on top, in the root frame, keyed by curveId
            def UsdGenSculptLayer "Sculpt" {
                rel     usdGen:input  = </Char/Groom/Fur/Ops/Deform>
                float   usdGen:weight = 1.0
                uniform string usdGen:sculpt:epoch = "sha1:9f3c1e…"
                int[]     usdGen:sculpt:curveIds  = [17, 18, 44, ...]
                int[]     usdGen:sculpt:cvOffsets = [0, 8, 16, 24, ...]
                vector3f[] usdGen:sculpt:deltas   = [...]
                int[]     usdGen:sculpt:lockedCurves = [44]     # Freeze brush: no downstream styling
            }
        }
    }
}
```

Load cost (R3): reopening the sidecar `.usdc` and reading `points` is **0.4–0.8 ms** for 800 k CVs;
the USD→Hydra half of the freeze is **flat in curve count and under 0.6 ms** at 10 k / 100 k / 1 M
curves (freeze-bake §3, §4.1, MEASURED). The deform is one memory-bound pass per frame.

### 3.12 Canonical example (c) — cards and archives

```usda
def UsdGenGroom "Groom" {
    def UsdGenDescription "Feathers" {
        rel usdGen:surface  = </Bird/Body>
        rel usdGen:terminal = </Bird/Groom/Feathers/Ops/Archives>
        uniform token usdGen:primitiveType = "archives"

        def Scope "Ops" {
            def UsdGenScatterRandom "Scatter" {
                float usdGen:density = 240
                int   usdGen:seed = 3
            }
            def UsdGenArchiveSet "Archives" {
                rel   usdGen:input = </Bird/Groom/Feathers/Ops/Scatter>
                # Prototype variety = multiple prototypes; Storm has no per-instance material (S33)
                rel   usdGen:prototypes = [ </Bird/Groom/Feathers/Protos/FeatherA>,
                                            </Bird/Groom/Feathers/Protos/FeatherB> ]
                int   usdGen:prototypeSeed = 5
                float usdGen:scale = 1.0
                float2 usdGen:scaleRandom = (0.85, 1.25)
                float usdGen:tiltN = 12.0                   # a host groomer Tilt N, degrees
                float usdGen:aroundN = 360.0
                token usdGen:orient = "surfaceFrame"        # surfaceFrame | camera | guide
            }
        }

        def Scope "Protos" {
            def Xform "FeatherA" ( prepend references = @./assets/featherA.usda@ ) {}
            def Xform "FeatherB" ( prepend references = @./assets/featherB.usda@ ) {}
        }
    }

    def UsdGenDescription "Cards" {
        rel usdGen:surface  = </Bird/Body>
        rel usdGen:terminal = </Bird/Groom/Cards/Ops/Cards>
        uniform token usdGen:primitiveType = "cards"
        def Scope "Ops" {
            def UsdGenScatterUniform "Rows" { float usdGen:spacingU = 0.02 ; float usdGen:spacingV = 0.02 ; float usdGen:jitter = 0.4 }
            def UsdGenCardSet "Cards" {
                rel   usdGen:input = </Bird/Groom/Cards/Ops/Rows>
                float usdGen:width  = 0.02
                float usdGen:length = 0.09
                float2[] usdGen:widthScale:knots = [(0,1),(1,0.2)]
                rel   usdGen:colorMap = </Bird/Groom/Cards/Maps/Tint>   # -> per-instance primvar
            }
        }
    }
}
```

What the scene index synthesizes for the archive description (S33, all MEASURED in
`probes/instancing/`):

```
/Bird/Groom/Feathers/Instancers/archives              primType = "instancer"
    instancerTopology/prototypes      = [ …/archives/Proto_0, …/archives/Proto_1 ]
    instancerTopology/instanceIndices = [ VtIntArray, VtIntArray ]   # HdIntArrayVectorSchema
    primvars/hydra:instanceTranslations|Rotations|Scales  (interpolation = instance)
    primvars/displayColor                                  (interpolation = instance)
    primOrigin { scenePath = /Bird/Groom/Feathers }        # absolute -> click selects the description
/Bird/Groom/Feathers/Instancers/archives/Proto_0      # namespace CHILD of the instancer (S33)
    instancedBy/paths = [ …/Instancers/archives ]     # exactly one path
    instancedBy/prototypeRoots = [ …/archives/Proto_0 ]
```

Interactive card edits dirty `primvars/hydra:instanceTranslations` (→ `DirtyPrimvar`, one BAR
re-upload) and **never** `instancerTopology` (→ index rebuild for every prototype rprim) (S33,
G-instancing §8).

---

## 4. D2 — Graph and engine API

Library `usdGen`, namespace `usdGen`, headers under `usdGen/` but **not installed** until S7
(§1, C-contract split). The measured basis is S21–S26 and `probes/data-plane-engine-prototype-benchmark`.

### 4.1 Buffers — SoA internally, AoS only at the Hydra boundary (S22)

```cpp
// usdGen/curveBuffer.h
struct UsdGenChunkTable {                 // curve-aligned chunking, 512 curves default (S23)
    VtIntArray curveBegin;                // size nChunks+1
    VtIntArray cvBegin;                   // size nChunks+1
    size_t NumChunks() const { return curveBegin.size() - 1; }
};

struct UsdGenCurveBuffer {
    // per-CV, planar
    VtFloatArray px, py, pz;
    VtFloatArray width;
    VtFloatArray hairT;                   // root->tip, mandatory (S29)
    VtFloatArray tx, ty, tz;              // hairTangent, object space, mandatory (S29)
    TfDenseHashMap<TfToken, VtFloatArray, TfHash> cvFloat;   // extra per-CV channels
    // per-curve
    VtIntArray   vertexCounts;
    VtIntArray   curveId;                 // stable across edits: hash64(seed, faceIndex, k)
    VtIntArray   rootPrim;                // bound face index  (== primvars:skinprim)
    VtVec2fArray rootUV;                  // == primvars:skinprimuv, and the "st" primvar
    VtIntArray   clumpId;                 // level 0; further levels in curveInt
    TfDenseHashMap<TfToken, VtFloatArray, TfHash> curveFloat;
    TfDenseHashMap<TfToken, VtVec3fArray, TfHash> curveVec3;
    TfDenseHashMap<TfToken, VtIntArray,   TfHash> curveInt;

    UsdGenChunkTable chunks;
    uint64_t topologyVersion = 0;         // bumped when curve/CV counts change
    uint64_t valueVersion    = 0;
};

// The one AoS conversion, at the Hydra boundary. 25 -> 81 GFLOP/s is the prize (S22, MEASURED)
VtVec3fArray UsdGenInterleavePoints(const UsdGenCurveBuffer &, size_t chunkIndex);
VtVec3fArray UsdGenInterleaveTangents(const UsdGenCurveBuffer &, size_t chunkIndex);
GfRange3d    UsdGenComputeChunkExtent(const UsdGenCurveBuffer &, size_t chunkIndex);
```

```cpp
struct UsdGenChunkView {                  // non-owning; what a kernel sees
    float *px, *py, *pz, *width, *hairT, *tx, *ty, *tz;
    const int     *vertexCounts;          // per curve, chunk-local
    const int     *cvOffset;              // per curve, chunk-local prefix
    const int     *curveId, *rootPrim, *clumpId;
    const GfVec2f *rootUV;
    const float   *mask;                  // per-curve, captured (may be null == 1.0)
    size_t curveBegin, curveCount, cvBegin, cvCount;
};
```

`memcpy` of the input chunk into the node's own buffer, then the kernel in place — the shape
`tbbBench.cpp` measured at **1.72–1.91 ms** for 5 stylers × 100 k curves and **0.035–0.044 ms** for a
1 %-sparse edit.

### 4.2 Operators — capture / evaluate (S25)

```cpp
// usdGen/op.h
enum class UsdGenSpace { Rest, Deformed };          // motion-blur tail classification (S25, S32)

struct UsdGenCaptureContext {
    const UsdGenSurfaceSet   *surfaces;   // rest topology, adjacency, kd-trees, PtexIndices, rest points
    const UsdGenParamView    *params;     // resolved scalars, ramps, tokens for THIS op
    const UsdGenMapSet       *maps;       // resolved map prims (image/ptex/expr/paint)
    uint64_t                  epochDigest;// (surfaceTopology, seed, guideIds, upstream topologyVersion)
    UsdGenDiagnostics        *diag;
};
struct UsdGenEvalContext {
    float                     time;
    float                     shutterOffset;        // 0 for P0/P1
    const UsdGenSurfaceSet   *surfaces;             // deformed points at time+offset
    const UsdGenParamView    *params;
    uint32_t                  seed;
};

class UsdGenOp {
public:
    virtual ~UsdGenOp();
    virtual const TfToken &GetType()  const = 0;
    virtual UsdGenSpace    GetSpace() const { return UsdGenSpace::Rest; }
    virtual bool           ChangesTopology() const { return false; }

    /// Topology-dependent, run once per epoch: scatter, stable ids, root bindings,
    /// kd-trees, guide weights, clump ids, map/Ptex/SeExpr samples, mask fields.
    /// May resize *out (generators only). Runs on the commit thread; may use TBB internally.
    virtual bool Capture(const UsdGenCaptureContext &ctx,
                         const UsdGenCurveBuffer    &in,
                         UsdGenCurveBuffer          *out,
                         UsdGenCaptureCache         *cache) = 0;

    /// Per-frame, per-chunk. MUST NOT allocate, MUST NOT touch anything outside its chunk
    /// except through cache->crossChunk (see 4.4). Called from tbb::parallel_for.
    virtual void Evaluate(const UsdGenEvalContext  &ctx,
                          const UsdGenChunkView    &in,
                          UsdGenChunkView          *out,
                          const UsdGenCaptureCache &cache,
                          size_t                    chunkIndex) const = 0;

    /// Identity for per-curve stylers; a fan-in set for clump/guide/smooth ops.
    virtual void UpstreamChunks(size_t myChunk, const UsdGenCaptureCache &,
                                std::vector<uint32_t> *out) const;

    virtual const TfTokenVector &TopologyParameters() const = 0;  // edit -> recapture + downstream
    virtual const TfTokenVector &ValueParameters()    const = 0;  // edit -> re-evaluate only
};

using UsdGenOpFactory = std::function<std::unique_ptr<UsdGenOp>()>;
class UsdGenOpRegistry {                              // TfSingleton
public:
    static UsdGenOpRegistry &GetInstance();
    void Define(const TfToken &primType, UsdGenOpFactory);
    std::unique_ptr<UsdGenOp> Create(const TfToken &primType) const;
    TfTokenVector GetTypes() const;
};
#define USDGEN_DEFINE_OP(PrimType, Class) /* TF_REGISTRY_FUNCTION(UsdGenOpRegistry) ... */
```

**Delivery note.** `UsdGenOpRegistry` is internal-only in v1 (registrations live in `libusdGen.so`).
Opening it as a public plugin ABI is deferred to v3 — usdRig's spec is explicit that it has no public
kernel registration ABI (A1 key facts), and committing to one before the v-table has settled is how
you end up unable to change `Evaluate`'s signature. Third-party operators are a **stated
non-goal for v1 and v2**.

### 4.3 The graph

```cpp
// usdGen/graph.h
class UsdGenGraph {
public:
    using NodeId = uint32_t;
    static constexpr NodeId InvalidNode = ~0u;

    static UsdGenGraphPtr New();

    /// Kahn topological order, namespace order as tie-break; cycles are compile errors (S26).
    /// Scoped recompiles: only the sub-graph whose structural digest changed is rebuilt.
    UsdGenCompileResult Compile(const UsdGenGraphDesc &desc);

    // ---- invalidation, all cheap and all callable from a notice handler
    void DirtyParameter(NodeId, const TfToken &param);        // value or topology, per 4.2
    void DirtyTopology (NodeId);
    void DirtySurface  (const SdfPath &surface, UsdGenSurfaceDirty bits);
    void DirtyChunks   (NodeId, TfSpan<const uint32_t> chunks);   // brush stroke, deform tail
    void DirtyMap      (const SdfPath &mapPrim);                  // reload / repaint

    /// Executes only dirty chunks, in topological order, tbb::parallel_for per node.
    /// Called ONLY from the serialized commit path (S18).
    const UsdGenCurveBuffer &Evaluate(NodeId terminal, const UsdGenEvalContext &);

    /// P2 motion: evaluate only the deformedSpace tail at this offset (S25, S32).
    const UsdGenCurveBuffer &EvaluateSample(NodeId terminal, const UsdGenEvalContext &, float offset);

    UsdGenGraphStats GetStats() const;      // per-node ms, chunks touched, RSS estimate
};

struct UsdGenGraphStats {
    struct Node { TfToken type; SdfPath path; double captureMs, evalMs;
                  uint64_t chunksEvaluated, chunksTotal, bytesOwned; };
    std::vector<Node> nodes;
    double totalMs; uint64_t generation; uint64_t bytesTotal;
};
```

`UsdGenGraphDesc` is a pure-value description built by `usdGenImaging` from data sources — node
paths, types, parameter blobs, `usdGen:input` edges, read phases. The engine never sees Hydra or USD.
That is what makes the whole engine unit-testable with no scene index at all, which is why the
headless engine suite is the fastest gate in CI.

**Structural digest** (S26): `UsdGenStructureDigest` hashes node paths, prim types, `usdGen:input`
targets, `usdGen:readPhase`, `usdGen:active`, `usdGen:cacheOutput`, `algorithmVersion`, surface
bindings, guide-set membership — and *excludes* every animated scalar (usdRig's rule,
`rigEvaluator.cpp:1175-1183`). A digest mismatch recompiles **only the affected sub-graph**, i.e. the
changed node and everything downstream of it; upstream nodes keep their buffers and capture caches.

### 4.4 Dirty propagation, end to end

The chain that has to be right for R8 ("precise dirty propagation on interactive prim edits"):

| Stage | Input | Mechanism | Output |
|---|---|---|---|
| 1 | USD edit to `usdGen:clump` | `UsdImagingDataSourceMapped::Invalidate` in the operator adapter (S10) | dirty locator `usdGen/clump` on the operator prim |
| 2 | locator | `UsdGenHairSceneIndex::_PrimsDirtied` maps `usdGen/<param>` → `(NodeId, TfToken)` via a compile-time map | `graph->DirtyParameter(node, param)`; **no cook** (S17, S18) |
| 3 | node dirty | `ValueParameters()` → mark all chunks of this node + all downstream nodes' chunks; `TopologyParameters()` → mark the node for recapture, which implies all chunks downstream | per-node `std::vector<uint8_t> dirty`, one byte per chunk (S23) |
| 4 | commit | `Evaluate(terminal)` walks topological order, `tbb::parallel_for` over dirty chunks only | new `UsdGenCurveBuffer` |
| 5 | publish | diff old vs new snapshot per chunk prim | `atomic_store` the generation (S19), then emit dirties |
| 6 | Hydra | per changed chunk prim: `primvars/points/primvarValue`, `extent/min`, `extent/max`; `xform/matrix` when the surface's xform moved (S4) | Storm's points fastpath |

Rule from S30, and it is worth 0.51 µs/prim/frame: emit the **bare leaf**
`primvars/points/primvarValue` where usdGen owns the prim (chunk prims — it always does), and
`HdContainerDataSourceEditor::ComputeDirtyLocators` only where it overlays an upstream prim (the
static-curve deform case, where the frozen `BasisCurves` is authored). Never co-dirty `displayColor`
with `points` (S30) — that drops Storm out of the points fastpath and re-uploads every other primvar.

Surface dirties: a `primvars/points/primvarValue` on a bound surface marks **only the deformed-space
tail** of the graph dirty for every chunk whose roots live on that surface. A `xform/matrix` dirty on
the surface (or any driven ancestor) re-dirties the chunk prims' own `xform` only (S4). A
`UniversalSet` dirty or a re-`PrimsAdded` of the surface is treated as a surface resync: re-read
topology, recapture.

**Cross-chunk queries** (clump to a guide in another chunk, `Smooth` in neighbours mode) are the one
unmeasured cost in the engine (G-data-plane open questions). The design contains them:
`UsdGenCaptureCache::crossChunk` holds, per chunk, a *read-only, immutable* snapshot of the CVs it
needs from other chunks, gathered once at capture and refreshed by a serial pre-pass at the start of
each `Evaluate` for the chunks whose sources changed. `UpstreamChunks()` declares the fan-in so the
scheduler can order the pre-pass. This keeps the parallel phase free of cross-chunk reads — a
correctness property, given that VDF's 500-element grain gets exactly this wrong (S21).

### 4.5 Thread model and memory

| Thread | May do | May not do |
|---|---|---|
| App / notice thread (the **commit thread**) | accumulate dirties, run `Compile`, `Capture`, `Evaluate`, publish the snapshot, emit notices | block on I/O without a timeout |
| TBB workers | run `Evaluate` per chunk; run capture sub-tasks | allocate into the buffer, emit notices, touch other chunks |
| Hydra readers (Storm's parallel Rprim sync) | `atomic_load` the published generation, wrap in data sources | evaluate, lock, change time, touch the stage (S19) |

Commit points, in priority order (S18): (a) explicit `UsdGenImaging_Commit()` from the usdview
plugin's `currentFrameChanged` handler using the *signal's* frame; (b) the `/`
`sceneGlobals/currentFrame` dirty; (c) a lock-free `atomic<bool>` backstop on the first `GetPrim` of
a generated prim. All three are needed: (a) is the only one that exists in a batch tool that wraps
us, (b) is the stock-usdview path, (c) covers parameter-only edits with no frame change and the
batched ordering inversion.

Memory: per-node output buffers by default, ≈ **6× the curve data** for a 5-deep chain (S24), i.e.
668 MB at 1 M curves × 8 CV (MEASURED). `usdGen:cacheOutput = false` on a node drops that node's
buffer and makes an edit downstream of it re-run from the nearest cached ancestor. The imaging
registry enforces a soft budget (`USDGEN_MEMORY_BUDGET_MB`, default 4096) by clearing
`cacheOutput` on the *oldest-touched* interior nodes first and logging one warning.

### 4.6 Diagnostics

`TF_DEBUG` codes: `USDGEN_GRAPH_COMPILE`, `USDGEN_CAPTURE`, `USDGEN_COOK`, `USDGEN_CHUNKS`,
`USDGEN_PUBLISH`, `USDGEN_MOTION`, `USDGEN_MAPS`. A per-session counter block reachable from Python
as `UsdGenImaging_GetStatsJson()` (cooks/frame, chunks/cook, ms per node, bytes per node,
generation) — the number a plan can be held to. **Delivery rationale:** without cooks-per-frame
visible from S1, the M1-over-cook failure mode (2–2.9×, S17) is invisible until it is a performance
bug in S7.

---

## 5. D3 — Imaging library structure

Library `usdGenImaging`. Two registrations, one binary (S1, S2, S7).

### 5.1 Registrations

```cpp
// The value path. Renderer level, all renderers.  (S1)
class UsdGenSceneIndexPlugin : public HdSceneIndexPlugin {
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const std::string &renderInstanceId,
                                             const HdSceneIndexBaseRefPtr &inputScene,
                                             const HdContainerDataSourceHandle &inputArgs) override;
    bool _IsEnabled(const HdContainerDataSourceHandle &inputArgs) const override;   // env kill-switch
};
TF_REGISTRY_FUNCTION(TfType) { HdSceneIndexPluginRegistry::Define<UsdGenSceneIndexPlugin>(); }
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers,   // ""
        TfToken("UsdGenSceneIndexPlugin"), /*inputArgs*/ nullptr,
        /*insertionPhase*/ 0, HdSceneIndexPluginRegistry::InsertionOrderAtEnd);   // S1
}
```

```jsonc
// usdGenImaging/resources/plugInfo.json  (generated; LibraryPath = $<TARGET_FILE_NAME:usdGenImaging>)
"UsdGenSceneIndexPlugin": {
  "bases": ["HdSceneIndexPlugin"], "displayName": "usdGen groom resolution",
  "loadWithRenderer": "",                       // mandatory: a C++ registration with no JSON
                                                // entry is dropped under the Hybrid policy
  "tags": ["usdGen:groom"],
  "ordering": { "after":  ["hd:sceneGlobals"],
                "before": ["hdGp:proceduralResolution", "hdPrman:motionBlur"] }
},
// Metadata only: returns its input unchanged. Exists solely for the two hooks.  (S7)
"UsdGenUsdImagingSceneIndexPlugin": { "bases": ["UsdImagingSceneIndexPlugin"], "displayName": "usdGen metadata" }
```

```cpp
class UsdGenUsdImagingSceneIndexPlugin : public UsdImagingSceneIndexPlugin {
    HdSceneIndexBaseRefPtr AppendSceneIndex(const HdSceneIndexBaseRefPtr &in) override { return in; }
    TfTokenVector InstanceDataSourceNames() const override;            // {"usdGen"}  (S7)
    TfTokenVector ProxyPathTranslationDataSourceNames() const override;// {"usdGen"}  (S7)
};
```

Phase 0 / `InsertionOrderAtEnd` puts usdGen strictly after `HdsiSceneGlobalsSceneIndex` (so it sees
`currentFrame` in usdview *and* usdrecord) and before every Storm and hdPrman plugin — velocity
motion, implicit surfaces, material-binding resolving, dependency forwarding, motion blur, pinned
expansion (S1, MEASURED in `probes/G-chain-order`). Under hdPrman, phase 0 all-renderers precedes
hdPrman's own phase 0 ext-computation pruning, which is why S3's **private** pruning wrapper is
mandatory rather than optional (G-hdprman §2.2, §5).

### 5.2 Classes

| Class | Role |
|---|---|
| `UsdGenImagingRegistry` | process-global `TfSingleton`; owns sessions keyed by (weak stage, groom root) or by `usdGen:sessionId`; survives renderer switch and stage replace (S15) |
| `UsdGenImagingSession` | one groom: the `UsdGenGraph`, the surface readers, the map cache, the SeExpr/Ptex caches, the snapshot store |
| `UsdGenSnapshot` | immutable published generation: per chunk prim the interleaved `VtVec3fArray`s, primvars, extent, xform; per instancer the topology + instance primvars |
| `UsdGenSnapshotStore` | `Publish()` diffs vs the previous generation and returns per-prim change bits; `Get()` = one `atomic_load` (S19; 0 torn reads under 8 readers × 20 publishes, MEASURED) |
| `UsdGenHairSceneIndex` | `HdSingleInputFilteringSceneIndexBase`: `GetPrim`/`GetChildPrimPaths` serve the published generation; `_PrimsDirtied/_PrimsAdded/_PrimsRemoved` forward the input notice **unchanged** and accumulate dirty state |
| `UsdGenSurfaceReader` | wraps `_GetInputSceneIndex()` in a **private** `HdSiExtComputationPrimvarPruningSceneIndex` (pass-through when nothing is computed) and caches per-surface topology, adjacency, `Far::PtexIndices`, kd-trees, rest points (S3) |
| `UsdGenChunkPrimBuilder` | builds one chunk prim's container: `basisCurves/topology`, `primvars`, `extent`, `xform`, `displayStyle`, `primOrigin`, `__dependencies` |
| `UsdGenInstancerBuilder` | instancer + prototype containers, `instancedBy` authoring, propagated-prototype discovery (S33, S34) |
| `UsdGenLiveOverrideStore` | per-prim, per-generation point overrides for the brush loop; indexed sparse pushes (S40) |
| `UsdGenSampledPointsDataSource` | `HdTypedSampledDataSource<VtVec3fArray>` implementing the P0/P1/P2 contract (S32) |
| `UsdGenOperatorAdapter` | `UsdImagingSceneIndexPrimAdapter` publishing the typed `usdGen` container (S10) |
| `UsdGenRestAdapter` | API-schema adapter publishing `usdGen/rest/points` at `UsdTimeCode::Default()` (S12) |

### 5.3 What a chunk prim publishes (contract C2)

```
/<Description>/Chunks/chunk_0007
  primType                              = "basisCurves"
  basisCurves/topology/curveVertexCounts = VtIntArray            # Σ == points.size(), asserted (S28)
  basisCurves/topology/basis             = "bspline"
  basisCurves/topology/type              = "cubic"
  basisCurves/topology/wrap              = "pinned"
  primvars/points        (vertex, role=point)     VtVec3fArray   # sampled data source (S32)
  primvars/widths        (vertex)                 VtFloatArray   # never "varying" (S29)
  primvars/hairT         (vertex)                 VtFloatArray
  primvars/hairTangent   (vertex)                 VtVec3fArray   # object space (S29, S35)
  primvars/hairId        (uniform)                VtFloatArray
  primvars/st            (uniform, role=textureCoordinate) VtVec2fArray   # root UV
  primvars/displayColor  (uniform, role=color)    VtVec3fArray
  primvars/minScreenSpaceWidths (constant)        1.0
  primvars/velocities                             HdBlockDataSource   # (P0/P2) (S29, S32)
  primvars/accelerations                          HdBlockDataSource
  extent/min, extent/max                                          # every deforming frame (S29)
  xform/matrix = surface world matrix, xform/resetXformStack = true   (S4)
  displayStyle/refineLevel = 2                                    (S29)
  materialBindings/allPurpose = <the description's material>
  primOrigin/scenePath = /<Description>                           # absolute (S29); relative inside prototypes (S34)
  __dependencies = { pointsDep: { dependedOnPrimPath: <surface>,
                                  dependedOnDataSourceLocator: primvars/points,
                                  affectedDataSourceLocator:  primvars/points } }   # per chunk, 32-256 edges (S30)
```

The publisher **asserts** `points.size() == Σ curveVertexCounts` (S28) — padding is broken in 26.08
and fails as fallback-red geometry with only a `TF_WARN` (G-storm-throughput §1.4, MEASURED). During
a density drag, element counts are held fixed by parking culled strands' CVs at a degenerate point
with zero width, and the real count change commits on release (S28).

**Guides** get their own chunk set (`<Description>/Guides/...`, `purpose = guide`,
`primvars:usdGen:role = "guide"`), because a different refineLevel or material fragments the draw
batch anyway (S30).

### 5.4 Surfaces: how `usdGen:surface` resolves to a path we can read

1. The adapter publishes `usdGen/surface` as `VtArray<SdfPath>` of *forwarded* targets (S10).
2. `UsdGenSurfaceReader::Resolve(path)`:
   * if the terminal scene index has a prim at `path` with a `mesh` data source → use it directly;
   * else look for `__usdPrimInfo.isNiPrototype` / `niPrototypePath` /
     `piPropagatedPrototypes` and follow the propagated path. **Propagated names are hashes:
     discover, never construct** (S34).
   * instance-proxy targets (`/World/HeadA/Scalp`) are translated by S7's
     `ProxyPathTranslationDataSourceNames`.
3. Points are read through the private pruning wrapper (S3). If `primvars/points` is still null and
   `extComputationPrimvars/points` exists, that is a hard error with a named diagnostic
   (`usdGen: surface <path> has computed points that could not be resolved`), not silence.
4. Rest points come from `usdGen/rest/points` (S12) or an authored `primvars:rest`.

Hair on a natively instanced scalp is generated **once per propagated prototype path**, with
`instancedBy` authored by hand and relative `primOrigin` (S34). Per-instance groom variation requires
S7's `InstanceDataSourceNames`, which is exactly why the metadata-only UsdImaging plugin ships from
day one instead of being deferred as usdRig deferred it (G-instancing decision 7).

### 5.5 The publish path

```cpp
// serialized; runs on the commit thread only
void UsdGenImagingSession::Commit(double frame) {
    if (!_dirty.exchange(false)) return;
    _graph->Compile(_BuildDesc());                    // no-op unless the digest changed
    const UsdGenCurveBuffer &buf = _graph->Evaluate(_terminal, _MakeEvalContext(frame));
    UsdGenSnapshotRefPtr snap = _BuildSnapshot(buf);  // interleave, extents, instancer topology
    UsdGenPublishedChange changed = _store->Publish(snap);   // atomic_store + diff
    _BroadcastToChains(changed);                      // PrimsRemoved, PrimsAdded, PrimsDirtied — in that order
}
```

Notice discipline mirrors usdRig exactly (A2 §3.4) because that shape is proven: removed/added/dirtied
in order, narrowest leaves, `UniversalSet` only on a structural change, announcement history kept
even while unobserved. **And S5's rule: any usdGen scene index that overrides a prim's `primvars`
container must also dirty the bare `primvars` locator** — otherwise a downstream UsdSkel resolved
prim freezes (MEASURED, G-chain-order §4c). For chunk prims usdGen mints itself this does not apply;
it applies to the static-curve overlay case, and forgetting it there is a silent wrong-geometry bug,
so it is asserted in the publisher and covered by `testUsdGenSkelInterop`.

---

## 6. D4 — Operator catalogue, split across three releases

Source of truth for parameters and maths: A7 §9. Cost classes: **A** = O(CV) per-curve parallel;
**B** = needs a spatial structure over roots (kd-tree at capture, query per curve); **C** = surface /
SDF queries per CV. Space: `R` = restSpace, `D` = deformedSpace (re-runs per motion sample, S25).

### 6.1 v1 — Slices 1–4 (the shippable groom)

| Prim type | Class | Cost | Key parameters | Slice |
|---|---|---|---|---|
| `UsdGenScatterRandom` | gen R | A (+B relax) | `density`, `seed`, `relaxIterations`, `areaCompensate`, `flip`, mask | S1 |
| `UsdGenGrowFromRoots` | gen R | A | `segments`, `length`(+map,+random), `direction`(`surfaceNormal\|attribute\|wind`), `lift`, `uvBlend`, `tangentialToSkin` | S1 |
| `UsdGenNoise` | styler R | A | `magnitude`(+ramp), `frequency`, `correlation`, `octaves`, `lacunarity`, `gain`, `space`, `preserveLength`, `cumulative` | S1 |
| `UsdGenLength` | styler R | A | `mode`(set/add/mul/cutAbsolute/cutRelative), `value`(+random,+map), `method`(scale/cutExtend), `minRemainingLength`, `cullThreshold`, `rebuild` | S1 |
| `UsdGenWidth` | styler R | A | `width`, `widthRamp`, `taper`, `taperStart`, `rootScale`, `tipScale`, `replace` | S1 |
| `UsdGenDeformWithSurface` | deformer **D** | A (rbf: B) | `mode`(`rigidFrame\|rbf\|pointDeform`), `rbfSamples`, `twistAware`, `preserveShape:{iterations,stretch,bend,refPos,lockRoots}` | S2 |
| `UsdGenCurveSource` | gen R | — | `curves`, `useRest` | S2 |
| `UsdGenFreeze` | gen R | — | `frozen`, `frozenCurves`, `frozenEpoch` | S2 |
| `UsdGenSculptLayer` | styler R | A | `weight`, delta arrays, `lockedCurves` | S2 |
| `UsdGenScatterAtGuides` | gen R | A | `perGuide` | S3 |
| `UsdGenGuideInterpolate` | gen R | capture B, eval A | `maxGuides`(3), `influenceRadius`, `influenceDecay`, `maxGuideAngle`, `blendInSkinSpace`, `blendMethod`, `useUniqueGuide`, `randomizeGuide`, `cvCount`, `clumpCrossover`, `regionMap` | S3 |
| `UsdGenClump` | styler R | capture B, eval A×levels | `clump`(+ramp), `clumpDensity`\|`clumpSize`, `clumpSeed`, `volumize`, `preserveLength`, `stray{Amount,Rate,Falloff}`, `copy`,`copyVariance`, `cut`, `noise`,`noiseFrequency`,`noiseCorrelation`, `levels`, `sizeReduction`, `tightnessReduction`, `goalFeedback`, `method`, `flatness`, `offset`, `curl` | S3 |
| `UsdGenSmooth` | styler R | A (along) / B (neighbours) | `strength`, `iterations`, `mode`, `searchRadius`, `numNeighbors`, `lockRoot` | S3 |
| `UsdGenResample` | styler R | A | `cvCount`, `mode`(uniform/keepParam), `restoreSegmentLengths` | S3 |
| `UsdGenImageMap`/`PtexMap`/`ExprMap`/`PaintMap` | maps | A | §3.6 | S4 |
| `UsdGenScale` | styler R | A | `scale` | S4 |

Emitted primvars: `Clump` writes `clumpId_<level>` (uniform int) for shading (a host renderer `Clump ID`);
`GuideInterpolate` writes `guideIndex[3]`/`guideWeight[3]` (the a host renderer `groom_closest_guides` /
`groom_guide_weights` arity, A7 §2.1) so a bake round-trips.

### 6.2 v2 — Slices 6–8

| Prim type | Class | Cost | Notes |
|---|---|---|---|
| `UsdGenScatterUniform` | gen R | A | rows/cols, `spacingU/V`, `jitter` |
| `UsdGenScatterPoints` | gen R | A | explicit `rootPrims[]`/`rootUVs[]` — what the Place brush authors |
| `UsdGenCurl` | styler R | A | `radius`(+ramp), `frequency`, `phase`, `taper`, `axisMode`, minimal-twist frame |
| `UsdGenBend` | styler R | A | `angle`(±random,bias), `axisMode`, ramp |
| `UsdGenDirection` | styler R | A | `direction`, `amount`, `lift`, `mode`, `followSkinContour` (a host groomer Tilt U/V/N) |
| `UsdGenStraighten` | styler R | A | `tangentStraightness`, `normalStraightness` |
| `UsdGenDisplace` | styler R | A | `amount`, `map`, `base`, `scale`, `offset`, `coordSys` |
| `UsdGenWave` | styler R | A | `frequencyX/Y`, `amplitudeX/Y` |
| `UsdGenCardSet` / `UsdGenArchiveSet` | gen R | A | §3.12 |
| `UsdGenShrinkwrap` | deformer **D** | C | `target`, `offset`, `pushRange`, `pushAmount`, `iterations`, `resolveType` — needs a BVH per surface epoch |

### 6.3 v3 — after the first show

`UsdGenWind` (D, time-dependent), `UsdGenForce`, `UsdGenPreserveClumps`, `UsdGenControlWires`,
`UsdGenBraid` (topology ×3), `UsdGenRoll`, `UsdGenMeshCut`, `UsdGenCollide` (Alembic colliders),
`UsdGenSphereSet`, XPD/Alembic import, an OpenExec backend behind the same `UsdGenOp` interface
(S16), and third-party operator registration.

**Why this split, from a delivery standpoint.** v1 is the smallest set that reproduces the request's
own example chain (`generator → clump → generator → clump → frizz`) *and* the two other canonical
workflows. Everything in v2 is a variation on a kernel shape v1 already has (per-CV displacement in a
frame), so v2 is measured in days per operator, not weeks. Everything in v3 needs a mechanism v1 does
not have — a collider BVH, a time integrator, a topology multiplier, or a plugin ABI — and each of
those is its own risk, so none of them is allowed to block a release.

---

## 7. D5 — The look pipeline

### 7.1 The three terminals, and the one hedge

S36 fixes the shape: one `Material` prim, three terminals — `outputs:surface` (UsdPreviewSurface,
universal fallback), `outputs:mtlx:surface` (MaterialX `chiang_hair_bsdf`, the render-time look),
Storm-specific (the glslfx). Route 1 is the v1 Storm look and **the file already exists, parses in
Sdr, and renders with zero warnings** on the GB10 (`probes/storm-hair-look/usdGenHairPreview.glslfx`,
`hair_pv_tangent.png`).

The one unverified link is whether Storm prefers `outputs:glslfx:surface` over the plain
`outputs:surface` (S36, G-storm-hair-look §7, open question). The hedge, and it costs almost nothing:

* The material authors all three terminals as in §3.10.
* `UsdGenHairSceneIndex` knows the renderer display name from `inputArgs.__rendererDisplayName`
  (`sceneIndexPluginRegistry.cpp:1435-1439`). When the name is `"GL"` and the bound material has a
  `UsdGenHairPreview` shader, the SI **overrides** `materialBindings/allPurpose` on its chunk prims
  to a synthesized material whose plain `outputs:surface` is the glslfx. When the name is a RenderMan
  variant, it leaves the authored binding alone.
* `USDGEN_STORM_MATERIAL_OVERRIDE=0` disables the override, so the day the render-context resolution
  is verified we flip the default and delete the code path.

This turns an UNVERIFIED dependency into a switch, which is the whole point of the risk lens: it is
tested in S1, not discovered in S7.

### 7.2 The mandatory primvar set and where each value comes from

| Primvar | Interp | Produced by | Cost |
|---|---|---|---|
| `points` | vertex | terminal node's buffer, interleaved | the whole chain |
| `widths` | vertex | `UsdGenWidth` / description default | A |
| `hairT` | vertex | generator (arc-length normalised at capture) | free |
| `hairTangent` | vertex | central differences of `points` at publish, object space | one pass |
| `hairId` | uniform | `hash(curveId) / 2^32` at capture | free |
| `st` | uniform | `rootUV` | free |
| `displayColor` | uniform | `UsdGenLookAPI` bake (§7.3) | capture-time |
| `minScreenSpaceWidths` | constant | `1.0` | free |

`hairTangent` costs 12 B/CV — 19 MB at 1.6 M CVs (G-storm-hair-look §2.5). It is worth it: the
screen-derivative fallback is visibly sparkly on 1–2 px strands, and reading `inData.Neye` **hard
fails to compile** at refineLevel 0 / no widths / on any mesh (MEASURED). The glslfx uses
`#ifdef HD_HAS_hairTangent` with the derivative path as the graceful fallback, so a groom that omits
the primvar still renders.

### 7.3 Colour, maps and expressions — all CPU, all at capture (S37)

Everything an artist paints is baked at capture time into per-curve or per-CV primvars. Storm
textures are used **only** for UV-mapped scalp colour via the per-curve `st` (verified working: the
checker maps correctly through a uniform `st` + `UsdPrimvarReader_float2` + `UsdUVTexture`,
`hair_preview_tex.png`).

`UsdGenLookAPI` on the description:

| Property | Type | Meaning |
|---|---|---|
| `usdGen:look:rootColor`, `:tipColor` | `color3f` | ends of the albedo ramp |
| `usdGen:look:colorRamp:positions/colors` | arrays | optional multi-stop ramp overriding the two above |
| `usdGen:look:colorMap` | `rel` | a `UsdGenMap` sampled at the root UV, multiplied in |
| `usdGen:look:hueJitter`, `:valueJitter` | `float` | per-curve, driven by `hairId` |
| `usdGen:look:bakeTarget` | `token` | `displayColor` (default) or a named primvar |

Bake order per curve, at capture: `albedo = ramp(hairT_root) × colorMap(rootUV) × jitter(hairId)` →
`displayColor` (uniform). Per-CV colour is available (`bakeTarget` = a vertex primvar) but costs 12
B/CV and is off by default.

**Ptex** (S37, A8 §2.6): file → `PtexCache::create(maxFiles, maxMem)` shared per session; one
`PtexFilter` **per TBB worker** (`PtexSeparableFilter` carries per-eval scratch, so a filter instance
is not reentrant — A8 §2.4). Face id comes from `Far::PtexIndices::GetFaceId(coarseFace)` built from
`PxOsdRefinerFactory::Create(topology, name)`; quads map 1:1, an n-gon occupies n consecutive face
ids. usdGen supports `mt_quad` files against `PtexIndices` as the canonical mapping and `mt_triangle`
only for all-triangle meshes. Measured: **23 ns/lookup**, 228 M lookups/s on 8 threads — cheaper than
the SeExpr expression that consumes it. Ptex is compiled *out of the OpenUSD install* and mesh-only
in Storm's GLSL, so a `.ptx` never reaches a Storm material.

**SeExpr** (S38, A8 §1): vendored `wdas/SeExpr` main@8f8c8f2, interpreter only, static + hidden.
One `VarBlockCreator` per compiled expression; **one thread-safe `VarBlock` per TBB worker**.

Variable set (the a host groomer dialect artists expect, A7 §1.3, A8 §1.8):

```
$u $v            surface parameters at the root
$id $faceId $patchId $descId
$P $N $dPdu $dPdv                (deformed)
$Pref $Nref $dPduref $dPdvref    (rest)
$Pw $Prefw                       (world)
$t                               root(0) -> tip(1), when evaluated per CV
$frame $cLength $cWidth $cDepth
$Cs $As                          bound colour / alpha at the root
```

Functions: the full SeExpr2 builtin set (noise/fbm/turbulence/voronoi/cellnoise, `curve`/`spline`
with interp codes 0–4, `fit`/`remap`/`smoothstep`/`gaussstep`, `pick`/`choose`/`wchoose`, `hash`),
plus three usdGen additions:

* `map("<mapPrimName>" [, s, t] [, channel])` — an `ExprFuncSimple` whose `evalConstant` resolves the
  named `UsdGenMap` prim once at prep and caches the handle (the `CurveFuncX` pattern, A8 §1.3);
* `ptex("<mapPrimName>" [, faceId, u, v] [, channel])`;
* **`rand([min],[max],[seed])`** — **not** a SeExpr2 builtin (verified: "Function rand has no
  definition"), so usdGen must add it, implemented on `hash` for determinism.

`noise()` keeps SeExpr's 0..1 range, not a host groomer's −1..1; `snoise` is the signed form and the difference
is documented in one line of the user docs. **ASSUMPTION**: matching SeExpr semantics beats matching
a host groomer's, because SeExpr's own noise is the single implementation shared by the C++ stylers (S38) and
having `noise()` mean two things in one product is worse than a one-line porting note.

Measured cost: 13 ns (`$u*$v+1`), 34 ns (`map()` + `hash`), 106–117 ns (noise + fbm) per eval; 50 M
evals/s on 8 threads. A million roots × 10 attributes ≈ 0.15 s on 8 threads — acceptable at capture,
not acceptable per frame, which is exactly why maps and expressions are capture-only.

### 7.4 Storm delivery details that are already settled

* Two glslfx files, not one: `usdGenHairPreview.glslfx` (`defaultMaterialTag`, opaque +
  alpha-to-coverage — the default for scalp hair) and `usdGenHairPreviewTranslucent.glslfx`
  (`translucent`, routed to `HdxOitRenderTask` — for fine fur). The tag is baked into the glslfx
  metadata and also splits draw batches, so they must be separate files (S35, MEASURED).
* `displayOpacity` alone is a trap: with a material bound it does nothing; with no material it
  promotes to `masked`, i.e. screen-door dither (MEASURED).
* Array-typed glslfx parameters (`float[2]`, `float[4]`) **do** bind as `vec2`/`vec4` (MEASURED,
  reversing A5's guess) — available if a future parameter wants packed pairs.
* `usdrecord`/`UsdAppUtilsFrameRecorder` with `SetCameraLightEnabled(false)` renders black even with
  `UsdLux` lights in the stage. The golden-image harness therefore sets lighting explicitly.

### 7.5 Map reload and the paint round-trip

Overwriting a map file on disk invalidates **nothing** (MEASURED: `stage->Reload()` produces no
notice). usdGen ships an explicit action (S13):

```
UsdGenImaging_ReloadMaps()  ->  bump the session's texture generation
                                purge the PtexCache and the Hio image cache for the affected assets
                                mark every node whose TopologyParameters() include a map dirty
                                optionally ArNotice::ResolverChanged().Send()   (measured to produce
                                exactly the right per-property dirties)
```

Paint round-trip: the Map-paint brush writes a `primvars:usdGen:<name>` on the **surface mesh** in
the edit target (per-vertex or per-face); a `UsdGenPaintMap` with `storage = "primvar"` reads it
straight from the scene index. "Bake to file" converts it to EXR (float, via Hio's built-in nanoexr —
`hioOpenEXR` is present) for UV maps or `.ptx` (via `PtexWriter` + `Far::PtexIndices::GetAdjacency`
for `adjfaces`/`adjedges`) for per-face maps, and rewrites `storage = "file"`. Painting therefore
never writes an image file during interaction — the S4 property that keeps the brush loop at
Hydra-only cost.

---

## 8. D6 — Tooling: architecture, the two Python surfaces, and each tool's loop

### 8.1 Module layout (`usdGenUsdview/`)

Every convention here is copied from a usdRig pattern that shipped, plus the four fixes B-usdrig-build
found (S43):

| Module | Qt? | Role |
|---|---|---|
| `usdGenUsdview.py` | no | `PluginContainer`; **all mutable state in a `UsdGenToolState` dataclass built in `__init__`**, never attributes assigned in `registerPlugins` (this is the exact drift that broke usdRig's own test fixture) |
| `usdGenLib.py` | no | ctypes binding of the C ABI; `USDGEN_IMAGING_DLL` override; pins `OPENBLAS_NUM_THREADS=OMP_NUM_THREADS=1` **before** importing numpy (S39) |
| `usdGenUndo.py` | no | `AttributeSnapshot`, **`SubtreeSnapshot`**, `Edit`, `UndoStack(LIMIT=200)`, `EditRecorder` |
| `usdGenGraphModel.py` | no | read/author the operator chain; cycle detection mirroring the C++ compiler |
| `usdGenBrushMath.py` | no | numpy kernels over a zero-copy `np.asarray(vt)` view; falloff, comb, clump, length, density |
| `usdGenPick.py` | no | thin wrapper over `UsdGenImaging_PickCV` / `_FootprintCV`; surface pick via a fresh `RenderParams` (never mutate `view._renderParams`) |
| `usdGenPanelUI.py` | yes | Groom panel: description tree, operator stack, parameter editor, freeze/bake actions |
| `usdGenBrushUI.py` | yes | viewport toolbar + transparent overlay + event filter; the brush controllers |
| `usdGenMapPaintUI.py` | yes | map paint mode, channel picker, bake dialog |
| `usdGenLodUI.py` | yes | density/decimation slider, chunk-count display, stats HUD |

Qt-free rule: only the two `*UI.py` families import Qt, and only inside menu callbacks (so the
container imports headlessly in tests). Viewport tools are installed from `signalStageReplaced` with
a bounded 0 ms `QTimer` retry, because plugins load before the `StageView` exists.

### 8.2 The two Python surfaces (contract C4, S39)

*(i) ctypes C ABI — control and scalars only.* No array ever crosses per element.

```c
/* usdGenImaging/cApi.h  — USDGEN_IMAGING_C_API */
int       UsdGenImaging_Activate(long long stageCacheId, const char *groomRootPath, double frame);
int       UsdGenImaging_SetTime(double frame);
int       UsdGenImaging_Commit(void);                      /* the S18(a) commit point */
void      UsdGenImaging_Deactivate(void);
long long UsdGenImaging_GetGeneration(void);               /* publication handshake */
int       UsdGenImaging_ReloadMaps(void);
int       UsdGenImaging_SetLodDensity(const char *descriptionPath, float density);

int       UsdGenImaging_BeginLiveOverride(const char *primPath);
int       UsdGenImaging_ClearLiveOverride(const char *primPath);

int       UsdGenImaging_PickCV(const char *primPath, const float viewProj[16],
                               int w, int h, float x, float y, float radiusPx,
                               int *outCurve, int *outCv, float *outDistPx);
int       UsdGenImaging_FootprintCV(const char *primPath, const float viewProj[16],
                                    int w, int h, float x, float y, float radiusPx,
                                    int *outIdx, int maxOut, int *outCount);
int       UsdGenImaging_ClosestSurfacePoint(const char *surfacePath, const float p[3],
                                            int *outFace, float outUV[2], float outP[3]);
const char *UsdGenImaging_GetStatsJson(void);
const char *UsdGenImaging_GetLastError(void);
```

*(ii) `usdGen._usdGen` — a pxr_boost.python module — for arrays.* `VtArray` crosses in **O(1)**:
0.13–0.18 µs at 100 k, 267 k and 1 M CVs, in both directions (MEASURED).

```c++
PXR_BOOST_PYTHON_MODULE(_usdGen)
{
    bp::def("ReadCurvePoints",        &ReadCurvePoints);        // (primPath) -> VtVec3fArray
    bp::def("ReadCurveCounts",        &ReadCurveCounts);        // -> VtIntArray
    bp::def("ReadCurveWidths",        &ReadCurveWidths);        // -> VtFloatArray
    bp::def("ReadCurveIds",           &ReadCurveIds);           // -> VtIntArray
    bp::def("ReadCurveRootUVs",       &ReadCurveRootUVs);       // -> VtVec2fArray
    bp::def("SetLiveOverride",        &SetLiveOverride);        // (primPath, VtVec3fArray)
    bp::def("SetLiveOverrideIndexed", &SetLiveOverrideIndexed); // (primPath, VtIntArray, VtVec3fArray)
    bp::def("SetLivePrimvar",         &SetLivePrimvar);         // (primPath, name, VtFloatArray)
    bp::def("ReadFrozenBuffer",       &ReadFrozenBuffer);       // (opPath) -> dict of VtArrays, for freeze
    bp::def("GetGeneration",          &GetGeneration);
}
```

Three rules that go into code review, each backed by a measurement:

1. **Never** `Vt.*Array.FromBuffer(numpy)` or `attr.Set(ndarray)` on groom-sized arrays — 420 µs at
   1.2 MB, 4.9 ms at 12 MB, versus 1.8 µs for `attr.Set(vt)`. C++ hands back `VtArray`s; Python
   authors them.
2. numpy is the **kernel** language over `np.asarray(vt)` (0.19 µs, zero-copy, read-only, and the
   view survives dropping the Python `Vt` object), never the transport.
3. The module must `from pxr import Vt, Gf` at import — the Vt converters are registered by
   `pxr/Vt/_vt.so`, not by `libusd_vt.so`.

The pxr_boost module is pinned to the USD build (mangled names carry `pxrInternal_v0_26_8__`); the
ctypes surface is the version-tolerant one, which is why control lives there.

### 8.3 The interaction loop, per tool

The universal shape is S40: **live override during the drag, one stage write at release inside an
undo bracket.** Measured per-move Python cost: **21 µs** on a 2 000-CV footprint at 100 k CVs — 0.13 %
of a 16.7 ms frame. The cost that matters is the republish + Storm re-sync, which is why the override
dirties only the touched leaves.

| Tool | Press | Move | Release |
|---|---|---|---|
| **Comb** | surface pick → root; `ReadCurvePoints` + `np.asarray`; `BeginLiveOverride`; `EditRecorder.Begin([points])` | `FootprintCV` (C++, 0.59 ms/100 k) → numpy comb kernel from the **press-time base** → `SetLiveOverrideIndexed` → `UpdateViewport()` | write `points` on the target prim (frozen prim, or a new `UsdGenSculptLayer`'s delta arrays) in one `Sdf.ChangeBlock`; `Commit`+`Push`; `ClearLiveOverride` |
| **Grab / Smooth** | as Comb | same, different kernel (Laplacian / rigid translate with falloff) | as Comb |
| **Length / Cut** | as Comb, base = `widths`+`points` | recompute arc-length truncation per move; counts stay fixed (degenerate CVs) | authors either `usdGen:length` on a `UsdGenLength` op, or per-curve deltas |
| **Clump brush** | pick; read `clumpId` | adjust `usdGen:clump` locally by writing a paint primvar on the surface | one `attr.Set(vt)` on the paint primvar |
| **Density paint** | pick surface; ensure the paint primvar exists → dirty `primvars/<name>` **once** | per move dirty `primvars/<name>/primvarValue` only (0.14–0.24 µs; a universal dirty is silently a no-op for descriptors) | `attr.Set(vt)` on the surface primvar; recapture |
| **Map paint** | as Density paint, with a channel selector | same | same, plus optional bake to EXR/`.ptx` |
| **Place guides** | surface pick | accumulate root (face, uv) in the live override | append to a `UsdGenScatterPoints` op's `rootPrims`/`rootUVs`, or author a new guide curve |
| **Freeze** | — | — | `SubtreeSnapshot.Capture`; define the `BasisCurves` **outside** the change block, author attributes inside; set `usdGen:frozen = true` + `usdGen:frozenEpoch`; one `_resetGUI` |

CV picking is CPU, in C++: **166 µs at 100 k CVs, 1.66 ms at 1 M** single-threaded — 8× cheaper than
one `view.pick()` (1.3–1.42 ms). It has no occlusion; the mitigation (read the depth buffer around
the cursor) is a documented option, not a v1 requirement. CV *highlighting* through Hydra selection
is unreachable from usdview (`HdSelectionSchema` carries only `fullySelected`), so **CV state is
colour**: a synthesized `points` child prim with `widths` + `displayColor`, which also gives dot-size
control (S40).

### 8.4 Undo, and the two USD landmines

* `SubtreeSnapshot` = `Sdf.CopySpec` into an anonymous stash. **0.10 ms to capture, 0.24 ms to undo,
  0.09 ms to redo at 100 k curves, with zero RSS growth** (COW `VtArray` sharing, MEASURED) — an
  undo stack 200 deep costs nothing for the freezes it holds.
* **Undo of a live freeze is `SetActive(false)`**, not removal (0.02 ms at 1 M curves, and it does
  not trip the OpenExec diagnostic). Real removal happens only when the entry leaves the stack, off
  the interactive path (S41).
* `UsdStage::RemovePrim` with an OpenExec system attached raises a spurious `Tf.ErrorException`
  (stock OpenUSD 26.08 bug at `esfUsd/stageData.cpp:360`, reproduced with zero usdRig code). Contain
  it: `try/except Tf.ErrorException` + a post-condition assert in Python, `TfErrorMark`+`Clear()` in
  C++, and batch removals into one `Sdf.ChangeBlock` (they coalesce into one exception) (S41).
* **A freeze may only be undone in the layer it was authored into**: with usdview's default
  session-layer edit target, `RemovePrim` on a root-layer prim is a **silent no-op** (MEASURED).
  `EditEntry` records the layer and `Restore()` no-ops on `layer.expired` (S41).
* `Usd*.Define()` cannot run inside `Sdf.ChangeBlock`.
* Freezes are siblings under a dedicated scope and the tooling **never** re-authors the parent
  scope's spec — a `typeName` touch on the parent resyncs 203 siblings (MEASURED, S42).

### 8.5 What the panels show

**Groom panel**: description tree (surface binding, terminal, chunk count, curve count, material) →
operator stack per description (drag to reorder = rewrite `usdGen:input`; solo/mute = `usdGen:active`;
per-operator mask summary) → parameter editor generated from the prim definition, with ramp widgets
for the `:knots` pairs and a SeExpr text field with the `#3dpaint,N` / `#min,max` annotation
convention preserved (SeExpr keeps comments). **Stats HUD**: cooks/frame, ms/node, curves, chunks,
bytes — straight from `UsdGenImaging_GetStatsJson()`.

**LOD**: `usdGen:lod:density` decimates the **curve count**, never the refine level — refineLevel 0
is *slower* than 1 at 200 k curves (12.4 vs 8.2 ms, MEASURED, because level 0 draws a LineList
through every CV). The slider drives the live override during the drag and commits on release.

### 8.6 testusdview coverage

`bin/run_testusdview_usdgen_<x>.sh` per suite, each printing a `USDGEN_<FEATURE>_OK` banner and
optionally saving `$USDGEN_<FEATURE>_SHOT`. Suites: `Groom` (activate, generation counter advances,
terminal SI carries the chunk prims), `Comb` (synthetic mouse drag → points changed → Ctrl+Z restores
the spec), `Paint` (primvar appears once, values change per move), `Freeze` (authored `points` equal
the terminal SI's `primvars/points`; undo removes the spec), `Cards` (instancer prim, instance count,
click selects the description), `Lod` (density slider changes the drawn curve count). Every suite
selects the terminal scene index by the `"[Terminal SI]"` **prefix**, not `names[-1]` (S43).

---

## 9. D7 — Roadmap: slices, exit criteria, test gates, estimates

### 9.1 The test harness, built before any feature (S45)

Four tiers, in the order a change hits them:

| Tier | What | Cost | Where it runs |
|---|---|---|---|
| **T0 engine** | `testUsdGenGraph`, `testUsdGenKernels`, `testUsdGenOps` — pure `UsdGenGraph` over synthetic buffers, no Hydra, no USD | ms | every commit |
| **T1 scene index** | headless tests over the **real** `UsdImagingCreateSceneIndices` chain + the renderer-plugin append, with a recording observer; assert on `HdBasisCurvesSchema` contents and on **emitted dirty locators** | < 100 ms each | every commit — the primary regression suite |
| **T2 Storm** | the **EGL device-platform harness** (`probes/storm-hair-look/eglctx.h`: compatibility profile + 64×64 pbuffer) renders on the GB10 headlessly; golden images with a fractional-pixel-difference tolerance, plus GPU frame timing | ~1 s each | every commit (correctness), nightly (timing) |
| **T3 app** | `testusdview` scripts under the scratchpad Xvfb (`DISPLAY=:77`, llvmpipe). **llvmpipe frame times are CPU numbers and are never quoted as Storm numbers.** | seconds | pre-merge |
| **T4 workstation** | MSAA/alpha-to-coverage quality, Metal/Vulkan Hgi (`HDST_ENABLE_HGI_RESOURCE_GENERATION=1`), non-NVIDIA drivers, a real hdPrman install, 4K interactive | manual | per slice exit |

T1 is where the money is: it catches a missing dirty, which a pull-based test structurally cannot.
Every operator ships with a T1 test asserting *both* the value and the notice.

Budget context: RigExec costs ~1.35 ms/frame on `ArmShotAnim` and a full terminal traversal adds
0.24 ms, leaving roughly **15 ms/frame** for usdGen at 60 fps before any GPU time; two extra
pass-through filtering scene indices cost nothing measurable (MEASURED). Storm draws 200 k curves ×
8 CV at refineLevel 2 in **23.9 ms** at 720p, and streaming 19.2 MB of new points adds **2.5 ms**
(MEASURED on the GB10).

### 9.2 Slices

Estimates assume **3 engineers** (E1 engine/kernels, E2 imaging/scene index, E3 tools/Python) plus a
part-time TD for content. **ASSUMPTION** — no staffing was given. Numbers are calendar weeks with all
three working; the "eng-weeks" column is the total effort.

---

**S0 — Skeleton · 2 weeks · 4 eng-weeks**

Deliver: repo, CMake with all nine target names, codeless `usdGenSchema` with the abstract bases and
three concrete stubs, both plugin registrations, `usdGenImaging` that returns its input unchanged,
`bin/_env.sh`, CI.

*Exit criteria.* (1) `cmake --build` clean on Linux/aarch64 with `-ffp-contract=off` on
`usdGenMath`. (2) `Usd.SchemaRegistry().IsConcrete("UsdGenScatterRandom")` is True from a plain
Python session with only `PXR_PLUGINPATH_NAME` set. (3) `testUsdGenChainOrder` asserts, over the real
chain, that `UsdGenSceneIndexPlugin` appears **after** `HdsiSceneGlobalsSceneIndex` and **before**
every `HdSt_*` — the walk `probes/G-chain-order/src/probeChainOrder.cpp` already implements. (4) A
second assertion using `SetPluginOrderingPolicy` + synthetic `hdPrman:*` tags proves the ordering
holds against RenderMan's phase numbers without RenderMan installed.

*Risk retired:* placement. This is the one thing that, if wrong, invalidates the whole design, and it
is cheap to prove.

---

**S1 — Straight hair, end to end · 4 weeks · 11 eng-weeks · freezes C1, C2, C5**

Deliver: `UsdGenScatterRandom` + `UsdGenGrowFromRoots` + `UsdGenNoise` + `UsdGenLength` +
`UsdGenWidth`; the `UsdGenGraph` with chunking, per-node buffers, capture/evaluate and the digest;
`UsdGenOperatorAdapter`; the chunk publisher with the full C2 primvar set; `usdGenShaders` with both
glslfx files and `shaderDefs.usda`; the Storm material override hedge (§7.1); the stats block.

*Exit criteria.*
1. T1: a 20 k-curve groom on a static mesh produces 40 chunk prims; every prim satisfies
   `points.size() == Σ curveVertexCounts`; the mandatory primvar set is present with the right
   interpolations; `primOrigin/scenePath` is the description.
2. T1: editing `usdGen:magnitude` on the `UsdGenNoise` prim emits exactly one `PrimsDirtied` per
   affected chunk prim with locator `primvars/points/primvarValue` (+ `extent/*`) and **nothing
   else** — no `displayColor`, no `UniversalSet`.
3. T1: `cooksPerFrame == 1` over a 200-frame scrub with a parameter edit on every tenth frame
   (the M3b ideal, S18).
4. T2: golden image against the EGL harness at refineLevel 2, camera light, ≤ 0.5 % changed pixels.
5. T2: the glslfx compiles with **zero** warnings under both `defaultMaterialTag` and `translucent`,
   and once more with `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` (the Metal/Vulkan proxy).
6. T2 timing: 100 k curves × 8 CV, static, ≤ 12 ms at 1280×720; deforming, ≤ 16 ms.
7. T0: the 5-node chain at 100 k curves evaluates in ≤ 3 ms and a single-parameter edit in ≤ 0.6 ms
   (the TBB prototype measured 1.72–1.91 / 0.18–0.41 ms, so this is 1.5× headroom).

*Stop condition.* If (2) cannot be met — i.e. the adapter cannot deliver precise invalidation for
`usdGen:*` properties — fall back to `primvars:usdGen:*` for parameters (S10's stated prototyping
fallback) and re-plan; do **not** proceed to S2 with imprecise dirtying, because every later slice
multiplies the cost.

---

**S2 — Deform and freeze · 3 weeks · 8 eng-weeks · freezes C3**

Deliver: `UsdGenDeformWithSurface`, `UsdGenCurveSource`, `UsdGenFreeze`, `UsdGenSculptLayer`;
`UsdGenSurfaceReader` with the private pruning wrapper and the rest adapter; freeze tiers T1/T2;
`SubtreeSnapshot` in `usdGenUndo.py`; the bare-`primvars` dirty rule.

*Exit criteria.*
1. T1: a groom on a **usdRig-deformed** scalp (`examples/ArmShotAnim`-class) tracks the deformation;
   assert points at three frames against a reference.
2. T1: a groom on a **UsdSkel-skinned** scalp does the same. This requires the private
   `HdSiExtComputationPrimvarPruningSceneIndex`; without it the surface's `primvars/points` is
   blocked and the test fails loudly rather than grooming the bind pose.
3. T1 `testUsdGenSkelInterop`: usdGen overlaying a `primvars` container upstream of UsdSkelImaging
   dirties the **bare `primvars`** locator; the skinned result changes across three frames.
   (Without the promotion the values freeze — MEASURED.)
4. T1: freeze → the authored `BasisCurves` matches the terminal SI's points bit-for-bit; the frozen
   prim re-enters via `UsdGenCurveSource` and a downstream styler modifies it; `usdGen:frozenEpoch`
   mismatch produces one warning and still renders.
5. Freeze cost: ≤ 1 ms of authoring + apply at 1 M curves (measured ceiling 0.59 ms), and the
   sidecar `.usdc` reopens in ≤ 1 ms.
6. `testUsdGenFreezeUndo`: `SetActive(false)` undo, redo, and the layer-expired guard.

---

**S3 — Guides and clumps · 4 weeks · 10 eng-weeks**

Deliver: `UsdGenGuideSet`, `UsdGenScatterAtGuides`, `UsdGenGuideInterpolate`, `UsdGenClump`
(multi-level, stray, noise, volumize, preserveLength), `UsdGenSmooth`, `UsdGenResample`;
`UsdGenCaptureCache::crossChunk` and `UpstreamChunks`; nanoflann kd-trees.

*Exit criteria.* (1) The §3.10 example renders. (2) Determinism: the same seed produces
bit-identical output across 20 runs and across thread counts 1/4/8/20 — the property that makes
clumping renderable on a farm. (3) A guide edit dirties only the chunks whose curves reference that
guide (assert chunk counts). (4) Cross-chunk clumping is correct when the clump centre is in another
chunk (the case VDF gets wrong by construction). (5) T0 timing: capture ≤ 300 ms at 1 M curves;
per-frame evaluate of the 5-node chain ≤ 10 ms at 1 M curves (prototype: 7.6 ms).

*Risk gate.* Cross-chunk cost is the one unmeasured number in the engine. If the serial pre-pass
exceeds 20 % of the frame at 1 M curves, the fallback is to widen chunks for clump-heavy graphs
(chunk = clump neighbourhood) at the cost of coarser dirtying — a parameter change, not a redesign.

---

**S4 — Maps and expressions · 3 weeks · 8 eng-weeks**

Deliver: vendored SeExpr + Ptex (static, hidden, `-ffp-contract=off` not needed there);
`UsdGenImageMap`/`PtexMap`/`ExprMap`/`PaintMap`; the `map()`/`ptex()`/`rand()` functions and the a host groomer
variable set; `UsdGenLookAPI` colour baking; `UsdGenImaging_ReloadMaps`.

*Exit criteria.* (1) A density map, a length expression and a Ptex mask each change the groom, with
golden per-curve values. (2) Expression evaluation is deterministic across thread counts (one
thread-safe `VarBlock` per worker). (3) `ReloadMaps` after overwriting a file changes the groom;
nothing changes without it (the documented behaviour, S13). (4) A paint primvar round-trips
primvar → EXR → `UsdGenImageMap` and primvar → `.ptx` → `UsdGenPtexMap` with ≤ 1e-4 error.
(5) Build gate: SeExpr and Ptex build offline from `thirdparty/` via `FETCHCONTENT_SOURCE_DIR_*`.

---

**S5 — Tools · 4 weeks · 10 eng-weeks · freezes C4**

Deliver: the C ABI, `_usdGen`, the container, undo, the Groom panel, Comb/Grab/Smooth/Length,
Density paint, Freeze, LOD, the stats HUD, six `testusdview` suites.

*Exit criteria.* (1) A comb stroke on a 100 k-CV groom holds ≥ 30 fps end to end at 1080p on the
workstation (the Python side is 21 µs; the budget is the republish + Storm re-sync). (2) Ctrl+Z
restores the pre-stroke spec exactly, and a stroke aborted with Escape leaves no spec. (3) No stage
write happens during a drag (assert layer change count == 0 between press and release).
(4) `UsdGenImaging_GetGeneration()` advances exactly once per commit.

---

**S6 — Instancing · 3 weeks · 7 eng-weeks**

Deliver: `UsdGenCardSet`, `UsdGenArchiveSet`, `UsdGenInstancerBuilder`, native-instance grooms, the
`InstanceDataSourceNames`/`ProxyPathTranslationDataSourceNames` hooks.

*Exit criteria.* (1) The §3.12 example produces a valid instancer through Hydra-1.0 emulation:
`GetInstancerId`, `GetInstanceIndices`, `GetInstancerPrototypes`, four instance-rate primvars.
(2) A click on a card resolves to the description (`ComputeInstancerContext` non-empty — it is empty
without `primOrigin`). (3) Hair on an `instanceable` scalp referenced 8× produces **one** prototype's
worth of curves and 8 instances. (4) Two scalps with different groom bindings aggregate separately.
(5) Editing a card transform dirties `primvars/hydra:instanceTranslations` and **not**
`instancerTopology`.

---

**S7 — Render-time and hardening · 4 weeks · 9 eng-weeks**

Deliver: motion profiles P0/P1/P2 with the retained per-offset cache; `EvaluateSample` restricted to
the deformed-space tail; the app preflight entry point; hdPrman validation; the workstation
benchmark protocol; the memory budget enforcement; installed public headers.

*Exit criteria.* (1) `usdrecord --renderer GL` and a real `usdrecord -r "RenderMan RIS"` both render
the §3.10 stage with **no usdview and no C API involved** — the S8 correctness property.
(2) Under hdPrman with a non-zero camera shutter, `GetContributingSampleTimesForInterval` returns
2–16 stable times, `GetValue(t)` is served from cache (assert zero extra chain evaluations), and CV
counts are constant across the shutter. (3) P2 at k=3 costs ≤ 1× head + 3× tail, verified by the
stats block. (4) The B1–B12 Storm benchmark set (G-storm-throughput §3.5) is run and recorded.
(5) `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` produces identical golden images.

---

**S8 — Breadth · continuous**

The v2 catalogue, async/progressive generation (`asyncAllow`/`asyncPoll`, plugin-enabled 100 ms
poll), LOD tiers, documentation.

### 9.3 Totals

| | Weeks (calendar) | Eng-weeks |
|---|---|---|
| S0–S2 (a usable deforming groom) | 9 | 23 |
| S0–S5 (an artist can groom) | 20 | 51 |
| S0–S7 (renderable on a show) | 27 | 67 |

**ASSUMPTION**: these are engineering estimates with no contingency; the risk register below is what
the contingency is for. A 30 % buffer on S3 and S7 (the two with unmeasured dependencies) is the
honest planning number.

---

## 10. Risks, stop conditions, and what is deliberately deferred

### 10.1 Register

| # | Risk | Likelihood | Impact | Early signal | Mitigation | Stop condition |
|---|---|---|---|---|---|---|
| R1 | Storm does not prefer `outputs:glslfx:surface` over `outputs:surface` (S36, UNVERIFIED) | medium | low | S1 exit test 4 | the SI-side material override, §7.1, behind an env flag | none — the hedge is cheaper than the verification |
| R2 | Metal/Vulkan Hgi resource generation reshapes interstage blocks and breaks the glslfx | medium | **high** (no Mac support) | S1 exit test 5 (`HDST_ENABLE_HGI_RESOURCE_GENERATION=1` on GL is the proxy) | the shader already avoids `inData`; keep the derivative-tangent fallback compiled in | if the proxy fails and no Mac is available by S5, declare macOS out of scope for v1 in writing |
| R3 | hdPrman behaviour differs from the source reading (no RenderMan on the dev host) | medium | medium | S7 exit test 1–2 | plugin ordering is unit-testable with synthetic tags; the sampled-points contract is implemented to the letter of §3 of the hdPrman report | if hdPrman rejects the chunk contract, emit `nonperiodic` with duplicated end CVs in a "render" mode (costs the Storm pinned index path, a per-description flag) |
| R4 | Cross-chunk clump/guide queries cost more than the parallel win | medium | medium | S3 exit test 5 | widen chunks for clump-heavy graphs; the serial pre-pass is already isolated | if > 20 % of frame at 1 M curves, make chunk size a per-description tuning parameter and document it |
| R5 | Density scrubbing forces whole-VBO reallocation (S28) | high | medium | S7 gate B7 | degenerate CVs + zero width at max-density topology during the drag; pre-baked density levels as the fallback | if degenerate CVs cost more than the reallocation, quantise to 4 pre-baked levels |
| R6 | Dependency fan-out at high chunk counts | low | medium | T1 assertion on chunk count | `__dependencies` declared **per chunk** (32–256 edges), never per curve (S30) | hard cap `chunkCountMax` at 256 |
| R7 | SeExpr/Ptex vendoring: bison/flex on Linux, pre-generated parser on Windows, symbol clash with a DCC's own copy | medium | medium | S4 build gate | static + `-fvisibility=hidden`, never install their headers or `.so`s next to USD | if a Windows build blocks, ship KSeExpr's `USE_PREGENERATED_FILES` pattern |
| R8 | An OpenUSD upgrade changes `UsdImagingDataSourceMapped`, pinned-curve expansion, the primvar-descriptor-cache narrowing (`baa8f9cf4`), or `HdSiExtComputationPrimvarPruningSceneIndex` | medium | medium | T1 suite fails on the new install | keep every version-sensitive assumption in **one** T1 test each, named after the mechanism | pin the supported USD version in `usdGenConfig.cmake` and gate CI on it |
| R9 | usdRig coupling: optional `find_package(rigExec)`, the CMake double-`pxrConfig` trap, the bare-`primvars` bug (S46) | low | low | S0 configure, S2 exit test 3 | `USDGEN_WITH_RIGEXEC=OFF` must build and pass every non-rig test | if `rigExecMath` diverges, copy the four kernels usdGen uses (RMF, ribbon transport, extent, envelope) into `usdGenMath` |
| R10 | `esfUsd/stageData.cpp:360` spurious `Tf.ErrorException` on `RemovePrim` | **certain** (reproduced) | low | any freeze/delete | never `RemovePrim` interactively; `SetActive(false)`; contain with `TfErrorMark` / `try/except` + post-condition assert; file upstream | none — contained |
| R11 | numpy's multi-threaded BLAS destroys brush latency on a loaded host (0.56 ms → 22 ms, MEASURED) | high | medium | S5 exit test 1 on a busy machine | pin `OPENBLAS_NUM_THREADS`/`OMP_NUM_THREADS` to 1 **before** importing numpy | none — one line |
| R12 | Thread scaling regresses past ~8–10 threads on heterogeneous cores (10×X925 + 10×A725) | high on this class of host | low | T0 timing at 1/4/8/20 threads | expose `USDGEN_THREAD_LIMIT`; default to `min(hw, 8)` unless overridden; never assume symmetric cores | none |
| R13 | The pxr_boost module is pinned to the USD build (mangled `pxrInternal_v0_26_8__`) | certain | low | import error | control stays on the ctypes surface; the module is rebuilt per USD version and its absence degrades tools, never correctness | none |
| R14 | Async/progressive generation cannot be enabled from a usdview plugin (`_allowAsync` is name-mangled; UNVERIFIED at runtime) | medium | low | S8 | `--allow-async` remains a documented CLI fallback | drop the plugin-side enable; it is a convenience |
| R15 | Live-override with a changing CV count is not tolerated by Storm (UNVERIFIED) | medium | low | S5 | the override keeps counts fixed and resamples only at press/release | none — the constraint is already in the design |
| R16 | Scope creep into simulation | **high** (it always is) | high | any request for collisions, self-collision, wind on a deadline | v3 list, §6.3, and this sentence | simulation is out of scope for v1 and v2; usdGen consumes a sim cache through `UsdGenCurveSource`, it does not produce one |

### 10.2 API stability boundaries

| Surface | Status at v1 | Change policy |
|---|---|---|
| `usdGen:` property names, prim type names | **frozen at S1/S2** | additive only; removal = one release of `hidden = true` first |
| Chunk-prim contract (C2) and frozen-curve contract (C3) | **frozen** | additive primvars only |
| C ABI (`extern "C"`) | **frozen at S5** | additive functions only; never change a signature |
| glslfx parameter names | **frozen at S1** | additive inputs only |
| `_usdGen` pxr_boost functions | frozen at S5 | additive; rebuilt per USD version |
| `UsdGenOp`, `UsdGenChunkView`, `UsdGenGraph`, all engine headers | **not installed, not stable** | free to change until S7 |
| `UsdGenOpRegistry` as a third-party plugin ABI | **not offered** | v3, if ever |

### 10.3 Deliberately out of scope for v1

Simulation and self-collision; XPD/Alembic import (the `UsdGenCurveSource` path takes a converted
`BasisCurves` instead); Ptex sampling *in Storm* (compiled out of the install; the CPU bake is the
route, S37); an OpenExec backend (not plugin-extensible in 26.08, S16); GPU compute kernels; a
node-graph UI (the operator stack is a list, as in a host groomer and a DCC's Guide Process); hdGp hosting of
third-party procedurals (S6 leaves the door open, it is not a v1 feature); Windows and macOS builds
(explicitly deferred to a slice of their own, gated by R2 and R7).

---

## 11. Assumptions added beyond the evidence

Everything below is mine, not the brief's, and each is cheap to reverse.

1. **Staffing (3 engineers)** and therefore every calendar estimate in §9.
2. **`usdGen:<op>:algorithmVersion`** per operator type (§3.9) — a look-preservation mechanism no
   S-number requires.
3. **Ramp encoding specialised into scalar `float2[] knots` and colour `positions`+`colors`**
   (§3.5), as a reading of S11's "knot arrays".
4. **SeExpr's `noise()` 0..1 semantics kept over a host groomer's −1..1** (§7.3), with `snoise` as the signed
   form.
5. **The Storm material-binding override behind `USDGEN_STORM_MATERIAL_OVERRIDE`** (§7.1) as the
   hedge against S36's unverified render-context resolution.
6. **`UsdGenOpRegistry` closed in v1/v2** — no third-party operator ABI.
7. **`min(hw, 8)` default thread limit** (R12), from the measured regression past 8 threads on this
   heterogeneous host.
8. **Soft memory budget `USDGEN_MEMORY_BUDGET_MB=4096`** with oldest-touched-first cache eviction
   (§4.5); the 6× figure is measured, the policy is not.
9. **The `usdGen:schemaVersion` refuse-and-warn policy** (§3.9): a partial groom is worse than an
   absent one.
10. **`UsdGenRestAPI` as an applied API schema on the surface** rather than an implicit adapter on
    every mesh — S12 fixes the mechanism, not where it is applied; making it explicit keeps the cost
    off untouched meshes.
11. **The description/groom split** (a host groomer Collection/Description) rather than a single prim type —
    D1 leaves the grouping open; this shape matches the prior art artists know (A7 §1.1).
12. **Guides reuse the frozen-curve contract C3** rather than having their own schema — one on-disk
    format for guides, freezes, imports and sim caches.

---

## 12. Evidence index

| Claim class | Source |
|---|---|
| Chain placement, ordering stability, UsdSkel blocking, the frozen-skin bug | `research/G-chain-order-probe.md`; `probes/G-chain-order/` |
| Engine choice, chunk size, SoA gain, per-node caching, sparse-edit cost | `research/G-data-plane-engine-prototype-benchmark.md` §3–§8; `probes/data-plane-engine-prototype-benchmark/` |
| Commit model, cook counts, notice cost, async | `research/G-evaluation-scheduling-and-batching.md` §4–§9; `probes/evalsched/` |
| Prim granularity, exact-size arrays, padding failure, invalidation discipline | `research/G-storm-throughput-and-prim-granularity.md` §1–§2 |
| The glslfx, tangent decision, material tags, Storm timings, EGL harness | `research/G-storm-hair-look-prototype.md`; `probes/storm-hair-look/` |
| Stage-free transport, codeless schemas, `TsSpline` ramps, rest at `Default()`, map reload | `research/G-stage-free-parameter-and-time-transport.md`; `probes/G-stage-free/` |
| Freeze contract, undo cost, bake sizes, resync blast radius, the OpenExec bug | `research/G-freeze-bake-undo-and-frozen-reentry.md`; `probes/freeze-bake/` |
| Instancer contract, `instancedBy`, `primOrigin`, propagated prototypes | `research/G-instancing-cards-archives-and-native-instances.md`; `probes/instancing/` |
| Array transport, CV picking, live paint dirtying, CV display | `research/G-tool-loop-array-transport-and-cv-picking.md`; `probes/tool-loop/` |
| Motion profiles, velocity SI, hdPrman's sampled-points contract | `research/G-motion-blur-sampling-strategy.md`; `research/G-hdprman-and-usdrecord-render-time-chain.md` |
| Operator catalogue, parameter vocabulary, mask model | `research/A7-prior-art-grooming.md` §1–§9 |
| SeExpr/Ptex/Hio/nanoflann APIs, licences, measured costs, CMake shape | `research/A8-seexpr-ptex-libs.md` |
| usdRig patterns: scene indices, notices, C API, usdview conventions, tests | `research/A1`, `A2`, `A3` |
| Build, `-ffp-contract`, the CMake trap, headless Storm, test tiers | `research/B-usdrig-build.md` |
| Host facts and corrections | `ENVIRONMENT.md` |
