# G — Stage-free parameter and time transport for a Hydra 2.0 grooming plugin

**Question.** A renderer-level (or even a UsdImaging-level) scene index has no `UsdStage`. Can every operator
parameter, map, mask, ramp, relationship and *rest* value the grooming plan needs be carried purely as Hydra
data sources? Where must evaluator state live so a renderer switch or stage replace does not lose it? Is the
ExecUsd control plane usable without a bespoke app handoff?

All OpenUSD paths below are relative to `<openusd-src>` (git tag `v26.08`). Probe sources,
scene files and captured output live under
`<session-scratch>`
(see §8). Everything marked **MEASURED** was produced by running one of those probes headless on this
machine; nothing here needs a GPU.

---

## 0. Executive answer

| Question | Answer | Where proved |
|---|---|---|
| Can all operator parameters travel as data sources? | **Yes** — every Sdf value type, asset, asset[], float2[], relationships, and even a whole `TsSpline`. | probe1, probe6 |
| Does an asset attribute carry a *resolved* path stage-free? | **Yes**, `SdfAssetPath::GetResolvedPath()` is filled by `UsdAttribute::Get`; UDIM identifiers additionally get a resolved-prefix form only through UsdImaging's asset-path data source. | probe1 §2, `dataSourceAttribute.cpp:34-100` |
| Is a `.spline` a curve in Hydra? | **No.** `UsdImagingDataSourceAttribute<T>` samples it at `stageGlobals.GetTime()`; `GetTimeSamples()` is empty but `ValueMightBeTimeVarying()` is **true**, so the attribute is flagged time-varying and **dirtied on every `SetTime`**. | probe1 §2, probe3 D/E, `stage.cpp:9751-9758` |
| Is there a stage-free ramp that is *not* dirtied per frame? | **Yes, two:** (i) knot arrays (`float2[]` + `float[]` + a token), (ii) a custom adapter data source holding the whole `TsSpline` from `UsdAttribute::GetSpline()` and *not* flagging time-varying. Both verified. | probe1, probe6 |
| Can a stage-free index get REST geometry? | **Yes, three ways**, all verified: authored `primvars:rest` (free passthrough), a custom `AttributeMapping::factory` that samples `UsdTimeCode::Default()`, or capture-on-first-cook. | probe1 §1e/§5 |
| Does a *custom container* published by an adapter survive to the terminal index? | **Yes**, including inside native-instancing propagated prototypes. | probe5 |
| Is the ExecUsd control plane reachable without an app handoff? | **Partly.** `UsdImagingGLEngine` already creates, feeds and ticks a `UsdExecImagingStageSceneIndex` — but it is off by default (`USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX=false`) and **not extensible from a plugin in 26.08** (hard-coded adapter list). | probe4, `usdExecImaging/adapterRegistry.cpp:28-52` |
| Where must evaluator state live? | In a **process-global registry**, keyed by stage (rigExec's `UsdUtilsStageCache` handoff) or by `renderInstanceId` (OpenUSD's own `HdUtils::RenderInstanceTracker`). Everything owned by the engine is destroyed on a renderer switch. | `engine.cpp:403-451,1449-1546`, `hd/utils.h:44-90` |

---

## 1. What a codeless-schema prim looks like to a stage-free consumer (MEASURED)

The probe registers a **codeless** schema plugin (`plugInfo.json` `"Type": "resource"`, `Root`/`ResourcePath` `"."`,
plus a hand-written `generatedSchema.usda` — the usdRig pattern, `<usdrig-src>/plugin/rigExecSchema/resources/`)
declaring `UsdGenProbeOperator` (bases `UsdTyped`), `UsdGenProbeClumpStyler` (derived), and
`UsdGenProbeImageableOp` (bases `UsdGeomImageable`). No imaging adapter is registered.

Registration works with zero C++ (probe1 §0):

```
prim /World/Clump typeName = UsdGenProbeClumpStyler
GetPrimTypeInfo().GetSchemaTypeName() = UsdGenProbeClumpStyler
IsA("UsdGenProbeOperator") = 1        IsA UsdGeomImageable = 0
UsdPrimDefinition::GetPropertyNames():
   usdGen:map  spec=SdfSpecTypeAttribute type=asset
   usdGen:mapArray  spec=SdfSpecTypeAttribute type=asset[]
   usdGen:ramp:knots  spec=SdfSpecTypeAttribute type=float2[]
   usdGen:surface  spec=SdfSpecTypeRelationship
   ...
```

This **closes A4's open question**: `UsdPrimDefinition::GetPropertyNames()` (`pxr/usd/usd/primDefinition.h:38`)
plus `GetSpecType()` (`:290`) and `GetSchemaAttributeSpec()->GetTypeName()` (`:307`) are the generic API for
building `UsdImagingDataSourceMapped::PropertyMappings` for a codeless type at startup — no generated C++ tokens
needed.

Run through `UsdImagingCreateSceneIndices` with no adapter, the terminal scene index reports (probe1 §1):

| Prim | Hydra `primType` | Prim-level data source names |
|---|---|---|
| `/World/Clump` (codeless, `UsdTyped` base) | `''` | `__usdPrimInfo primOrigin primvars usdMaterialBindings geomModel model __usdUpAxis skelBinding coordSysBinding purpose visibility xform materialBindings` |
| `/World/ImageableOp` (codeless, Imageable base) | `''` | same + non-null `visibility`, `purpose` (`purpose = guide`) |
| `/World/Scalp` | `mesh` | + `mesh`, `extent`, `primvars` |

The **schema-declared `usdGen:*` attributes are absent** from the data source, and — measured, probe2 A/B —
authoring `usdGen:clumpRadius` or `usdGen:seed` produces **no notice at all**. `primvars:usdGen:*` attributes
*are* present and *are* dirtied precisely (probe2 C: `*DIRTY /World/Clump {'primvars/usdGen:rampKnots'}`).

`primOrigin` contains **only** `scenePath` (probe1 §1c) — confirming the gap statement and that
`dataSourcePrim.h:386-400`'s comment about a `usdPrim` entry is stale. There is no `UsdPrim`, hence no
`UsdStage`, downstream.

**Conclusion:** parameters must reach Hydra either as `primvars:` (free, but they masquerade as primvars) or
through **our own prim/API-schema adapter** (typed container, precise invalidation). The plan should use an
adapter; `primvars:` is the fallback for prototyping.

### 1.1 Gotcha: relationship primvars are gettable but not enumerable

`UsdImagingDataSourcePrimvars::GetNames()` enumerates via `UsdGeomPrimvarsAPI::GetAuthoredPrimvars()`
(`dataSourcePrimvars.cpp:97`) which lists **attributes only**, while `Get(name)` falls through to
`_usdPrim.GetRelationship(propName)` and builds a constant path-array primvar (`:167-174`). Measured
(probe2 K):

```
primvars GetNames(): usdGen:clumpRadius usdGen:map usdGen:rampKnots usdGen:udimMap
primvars->Get('usdGen:surface') = NON-NULL   primvarValue = [/World/Scalp]  interpolation = constant
```

So the hdGp convention (`rel primvars:sourceMeshPath`) works only if the consumer asks **by name**. A generic
graph walker that iterates `GetNames()` will silently lose every relationship argument.

---

## 2. Parameter transport: what each type yields (MEASURED, probe1 §2–§4)

| USD type | Hydra data source | Value at `t=1` | `GetContributingSampleTimesForInterval(-0.25,0.25)` | Flags raised |
|---|---|---|---|---|
| `asset` | `UsdImagingDataSourceAssetPathAttribute` (`dataSourceAttribute.cpp:34`) | `SdfAssetPath{path='maps/density.exr', resolved='/…/maps/density.exr'}` | false | `FlagAsAssetPathDependent(/World/Clump.usdGen:map)` |
| `asset[]` | `UsdImagingDataSourceAttribute<VtArray<SdfAssetPath>>` | per-element resolved path; **unresolvable element → `resolved=''`** | false | none (specialisation is on scalar `SdfAssetPath` only, `dataSourceAttribute.h:203-212`) |
| `asset` with `<UDIM>` | asset-path DS | `resolved='/…/maps/clump_<UDIM>.exr'` — resolved **prefix** with the token retained | false | asset-path dependent |
| `float2[]` (ramp knots) | typed DS | `[(0,0),(0.5,1),(1,0.2)]` | false | none |
| `float[]` | typed DS | `[0,1,0.2]` | false | none |
| `int`, `string`, `token` | typed DS | `7`, `"$density * cellnoise($P)"`, `clump` | false | none |
| `float` with `.timeSamples` | typed DS | `0.25` @1, `0.75` @24 | **true**, `{-0.25, 0, 23}` | `FlagAsTimeVarying` |
| `float` with `.spline` | typed DS | `0.1` @1, `0.9` @24 | **true**, `{-0.25, 0.25}` | `FlagAsTimeVarying` |
| unauthored schema attr | typed DS | schema fallback (`1`) | false | none |
| `rel` (1 target) | `UsdImagingDataSourceRelationship` | `VtArray<SdfPath>[/World/Scalp]` | false (never time-varying) | none |
| `rel` (2 targets) | same | `[/World/Gen, /World/Scalp]` | false | none |
| `rel` via `GetPathFromRelationshipDataSourceFactory` | retained `SdfPath` | `/World/Gen` (first forwarded target) | n/a | none |

Notes and consequences:

* **Resolution is free.** `UsdAttribute::Get<SdfAssetPath>` resolves; the UsdImaging wrapper adds only UDIM
  handling (`UsdShadeUdimUtils::IsUdimIdentifier` / `ResolveUdimPath`, `dataSourceAttribute.cpp:58-68`). Pure
  Usd left `resolvedPath=''` for `clump_<UDIM>.exr`; the Hydra data source returned the resolved prefix. **A
  Ptex/painted-map loader downstream therefore needs no resolver context and no stage** — it consumes
  `SdfAssetPath::GetResolvedPath()`.
* **Asset-array attributes are not tracked.** `FlagAsAssetPathDependent` is only specialised for scalar
  `SdfAssetPath` (`dataSourceAttribute.h:203-212`), so an `asset[]` map list gets *no* reload invalidation.
  Prefer one `asset` per map, or a child prim per map.
* `UsdImagingDataSourceAttributeNew` `TF_WARN`s and returns null for a type outside `SDF_VALUE_TYPES`
  (`dataSourceAttribute.cpp:178`). There is no Sdf type for a spline, a curve or an expression AST — only
  `string`/`token` for SeExpr source text (which transports fine, above).
* `UsdImagingDataSourceMapped` gives nested locators and 1:1 invalidation for free. Measured (probe1 §4):
  container names `clumpRadius inputs map ramp surface`, nested `ramp/{knots,values}`, and
  `Invalidate({usdGen:ramp:knots, usdGen:surface}) -> 'usdGen/ramp/knots' 'usdGen/surface'`. The leaf mapping
  stores the **absolute** locator (`dataSourceMapped.cpp:334-345`), so the time-varying flag is registered at
  the right place automatically.
* **`UsdImagingDataSourceMapped::Get` `TF_CODING_ERROR`s** if the named attribute/relationship does not exist
  on the prim (`dataSourceMapped.cpp:170-186, 194-209`). Build mappings from `UsdPrimDefinition`, never from a
  hand-written superset list, or an operator type that lacks an attribute will spam coding errors.

### 2.1 The ramp question, settled

A `.spline` on a `float` attribute is *only* a time-varying scalar to Hydra. Worse, `UsdStage` deems **every**
spline time-varying without analysing it (`stage.cpp:9751-9758`: "all splines are deemed as possibly time
varying"), while `GetTimeSamplesInInterval` returns **nothing** (probe1: `usd GetTimeSamplesInInterval(0,30) = {}`).
Measured consequences:

* `FlagAsTimeVarying` fires → **every `SetTime` dirties the parameter** (probe3 D/E:
  `*DIRTY /World/Clump {'primvars/usdGen:clumpRadius/primvarValue'}` at both `SetTime(24)` and `SetTime(2)`).
  For an hdGp procedural or an operator-graph scene index, that is a **full recook per frame** on a static
  ramp.
* `GetContributingSampleTimesForInterval` returns the two interval *edges* `{-0.25, 0.25}` — motion samples
  that are not real knots.

Two stage-free ramp representations that avoid this, both verified:

1. **Knot arrays.** `float2[] usdGen:ramp:knots` + `float[] usdGen:ramp:values` (+ a `token` interpolation).
   Never time-varying, never dirtied, nested cleanly under `usdGen/ramp/*`.
2. **The whole `TsSpline` as a data source** (probe6, MEASURED). An adapter reads `UsdAttribute::GetSpline()`
   (`pxr/usd/usd/attribute.h:563`) and publishes `HdRetainedTypedSampledDataSource<TsSpline>`, *without*
   flagging time-varying. The terminal scene index returns it intact:

   ```
   cast to HdTypedSampledDataSource<TsSpline>: OK
   VtValue type = TsSpline  isHoldingTsSpline = 1
   knot count = 2  valueType = float
   Eval(u=0) ok=1 value=0.1 … Eval(u=24) ok=1 value=0.9
   ```

   The spline's parameter axis is then reinterpreted as normalized root-to-tip `u`. This keeps USD-native
   authoring (`float usdGen:clumpRamp.spline = { 0: 0; 1: 1 }`), full Ts interpolation modes and
   extrapolation, and costs zero per-frame invalidation. Only supported value types are
   `double/float/GfHalf/GfTimeCode` (`pxr/base/ts/types.h:32-37`), so a *color* ramp still needs 3 splines or
   knot arrays.

---

## 3. Rest / reference surface without a stage (MEASURED)

The stage's `points` attribute has a `default` (= rest) **and** `timeSamples` (= deformed) in the probe scene.
`UsdImagingDataSourceAttribute` evaluates at `stageGlobals.GetTime() + shutterOffset` only
(`dataSourceAttribute.h:48-51`), so the terminal index shows deformed points (probe1 §1e):

```
t=1  points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]
t=24 points = [(0,0,5),(1,0,5),(1,1,5),(0,1,5)]
```

Downstream of rigExec the situation is worse: rigExec **replaces** `primvars/points` with a retained array
(A2 §3.2, `<usdrig-src>/libs/rigExecImaging/sceneIndices.cpp:1336-1357`), so the stage's default
value is not reachable at all through the chain. Three verified routes:

| Route | Mechanism | Measured result | Cost |
|---|---|---|---|
| **R1 — authored `primvars:rest`** | a DCC convention; `UsdImagingDataSourcePrimvars` passes it through untouched, and rigExec only owns `points/velocities/accelerations/normals` (A2 §3.2, :387) | probe1 §1e: `primvars on Scalp: rest st points velocities accelerations normals`; `primvars:rest @t=24 = [(0,0,0),(1,0,0),(1,1,0),(0,1,0)]` | zero code, but requires the asset to author it |
| **R2 — adapter data source at `UsdTimeCode::Default()`** | custom `UsdImagingDataSourceMapped::AttributeMapping::factory` returning a DS whose `GetTypedValue` calls `_q.Get<T>(&r, UsdTimeCode::Default())` | probe1 §5, at stage time 24: `deformed = [(0,0,5)…]`, `rest = [(0,0,0)…]`; stage globals show only `usdGenRest/deformed` flagged time-varying — the rest DS costs **no per-frame dirty** | ~30 lines; needs an API-schema adapter on the scalp |
| **R3 — capture on first cook** | procedural/graph stores the first-seen `primvars/points` | not measured (design) | fragile if the first frame is not the rest frame |

**R2 is the recommended primary**, with R1 honoured when authored. Both are stage-free from the consumer's
point of view: the *adapter* runs inside `UsdImagingStageSceneIndex` where a `UsdPrim` exists, and publishes a
plain container that survives everything downstream (§4). Same trick applies to rest-time *curve* points for
"load static curves and deform them": author the groom's curves once and expose `usdGen:rest/points` at
`Default()`.

Note the motion-blur trap seen in the same run: with samples only at frames 1 and 24, at `t=1`
`GetContributingSampleTimesForInterval(-0.25,0.25)` returns `{-0.25, 0, 23}` — the *far* bracketing sample is
pulled in. Downstream curve emitters must clamp their own shutter sampling rather than trusting the surface's
contributing times (cross-check with G-motion-blur-sampling-strategy).

---

## 4. Custom containers survive the whole chain, including instancing (MEASURED, probe5)

A filtering scene index inserted through `UsdImagingCreateSceneIndicesInfo::overridesSceneIndexCallback`
overlays a `usdGen` container onto every prim. At the **terminal** scene index (after draw mode, native
instance aggregation + flattening, selection, render-settings flattening):

```
/World/Proto/Scalp                    primType='mesh'      usdGen container = PRESENT {restPoints=[(1,2,3)] opType=clumpStyler}
/World/Proto/Groom                    primType=''          usdGen container = PRESENT
/World/InstA                          primType=''          usdGen container = PRESENT
/UsdNiPropagatedPrototypes/NoPrimvars___usdUpAxisd827…/__Prototype_1/UsdNiInstancer          usdGen = MISSING  (synthetic)
/UsdNiPropagatedPrototypes/…/UsdNiInstancer/UsdNiPrototype/Scalp   primType='mesh'  usdGen = PRESENT
/UsdNiPropagatedPrototypes/…/UsdNiInstancer/UsdNiPrototype/Groom   primType=''      usdGen = PRESENT
```

So an adapter-published `usdGen` container (parameters, rest points, ramp splines, map paths) reaches a
renderer-level scene index unchanged, and also reaches the **propagated prototype** copy so grooms on
instanced characters work.

⚠️ The propagated-prototype name encodes the instance-aggregation key
(`NoPrimvars___usdUpAxis<hash>`). Our `usdGen` container is **not** part of that key, so two instances whose
grooms differ only in `usdGen:*` values would be aggregated into one prototype. The fix exists only on the
UsdImaging-chain plugin: `UsdImagingSceneIndexPlugin::InstanceDataSourceNames()`
(`usdImaging/sceneIndexPlugin.h:60-83`) — **not** available to a renderer-chain `HdSceneIndexPlugin`. Any
per-instance groom variation must therefore either be authored outside the instance, or the plugin must
register on the UsdImaging chain (accepting A4's undefined ordering vs. rigExec).

---

## 5. Invalidation and reload semantics (MEASURED, probe2 + probe3)

| Event | Notices at the terminal index |
|---|---|
| author `usdGen:clumpRadius` (schema attr, no adapter) | **none** |
| author `usdGen:seed` | **none** |
| author `primvars:usdGen:rampKnots` | `*DIRTY /World/Clump {'primvars/usdGen:rampKnots'}` |
| author `/World/Scalp.points` @2 | `*DIRTY /World/Scalp {'primvars/points'}` |
| `SetTime(24)` **before** any value pull | **none** |
| `SetTime(24)` **after** a deep pull | `*DIRTY /World/Scalp {'primvars/points/primvarValue'}`, `*DIRTY /World/Clump {'primvars/usdGen:clumpRadius/primvarValue'}` |
| `SetTime(24)` twice with the same value | none (`stageSceneIndex.cpp:363-365` early-outs) |
| `ArNotice::ResolverChanged().Send()` | `*DIRTY /World/Clump {'primvars/usdGen:map' 'primvars/usdGen:udimMap'}` |
| `stage->Reload()` (files unchanged) | **none** |
| `DefinePrim("/World/Frizz", "UsdGenProbeOperator")` | `+ADD /World/Frizz type=''` |
| `SetTypeName("UsdGenProbeOperator")` on an existing prim | `+ADD /World/Clump type=''` (resync, re-added in place) |
| `SetStage(nullptr)` then `SetStage(stage)` | `-REM /` then a full re-add of every prim, plus a coarse `*DIRTY` per prim over `{__usdUpAxis coordSysBinding geomModel materialBindings model primvars purpose skelBinding visibility xform}` |

Two facts that shape the whole design:

**(a) Time-varying and asset-path registration is LAZY.** `FlagAsTimeVarying` / `FlagAsAssetPathDependent` are
called in the *data source constructor* (`dataSourceAttribute.h:205-212, 228-243`). Before the deep pull, no
`SetTime` produced any notice; after it, both prims dirtied correctly. So a downstream operator graph that
caches values and stops pulling will **stop receiving time dirties for what it stopped pulling**. Corollary: a
groom scene index must pull the surface/parameter data sources it depends on at least once per topology
generation, or declare `__dependencies` explicitly (A4 §5).

**(b) A repainted map file on disk invalidates nothing.** `_assetPathDependents` is only consulted from
`notice.GetResolvedAssetPathsResyncedPaths()` (`stageSceneIndex.cpp:586-595`), and that path range is populated
only by (i) `UsdStage::_HandleResolverDidChange` on an `ArNotice::ResolverChanged` that affects the stage's
context — which stamps `SdfPath::AbsoluteRootPath()`, i.e. the whole stage (`stage.cpp:4957-5002`) — or
(ii) a layer-stack expression-variable change (`stage.cpp:4718-4724`). `stage->Reload()` does neither
(measured). **The grooming plugin must therefore expose its own "reload maps" action** that sends an
`ArNotice::ResolverChanged` (measured to produce exactly the right per-property dirties) or bumps a texture
generation counter in its own registry. Do not promise automatic pickup of an overwritten Ptex/EXR file.

---

## 6. The ExecUsd control plane in 26.08 (MEASURED, probe4)

A1/A6 proposed an ExecUsd control plane. 26.08 ships one **inside the engine**, which changes the analysis:

* `UsdImagingGLEngine::_AppendOverridesSceneIndices` (`engine.cpp:1612-1660`) creates
  `UsdExecImagingCreateStageSceneIndex()` when `USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX` is set
  (`engine.cpp:90-96`, default **false**), merges it as the *strongest* input of an `HdMergingSceneIndex`, and
  wraps the pair in a batching index.
* The engine feeds it the stage (`engine.cpp:534-545`), the time (`:489-490`) and `ApplyPendingUpdates`
  (`:2373-2374`). So an exec-computed value can override any data source with **no app-specific handoff at
  all** — just an env var.
* This install **has exec built**: probe4 prints
  `UsdExecImagingCreateStageSceneIndex() -> NON-NULL (PXR_BUILD_EXEC=ON)`, `displayName =
  UsdExecImaging_StageSceneIndex`, and it serves `xform` for `/World`, `/World/Scalp`, `/World/Hair`
  (Xformables) and `null` for the codeless `/World/Clump`.
* **But it is not extensible.** `UsdExecImaging_AdapterRegistry::GetPrimAdapter`
  (`usdExecImaging/adapterRegistry.cpp:28-52`) hard-codes exactly two adapters (`UsdGeomXformable`,
  `ExecIrXformable`); the header states "This class remains largely un-implemented … This will change in the
  future when we add the ability to register prim adapters from separate plugins"
  (`adapterRegistry.h:20-24`). `usdExecImaging/plugInfo.json` declares no `Types`. There is no plugInfo key a
  grooming plugin could use.

**Settled:** the ExecUsd plane is the *right long-term target* (the interface
`UsdExecImagingPrimAdapterInterface::{BuildRequest,GetPrimData,InvalidatePrimData}` is exactly a groom operator
graph), but in 26.08 it is **not usable by a third-party plugin**. The plan must treat exec as optional and
future-facing, and ship its own evaluator.

---

## 7. Where state must live across renderer switch / stage replace

Nothing that the engine owns survives a renderer switch. `UsdImagingGLEngine::SetRendererPlugin`
(`engine.cpp:1449-1546`) calls `_DestroyHydraObjects()` (`:403-451`), which nulls `_sceneDelegate`,
`_renderer`, `_terminalSceneIndex`, `_cachingSceneIndex`, `_appSceneIndices`, `_mergingSceneIndex`,
`_usdImagingSceneIndex`, `_displayStyleSceneIndex`, `_rootOverridesSceneIndex`,
`_noticeBatchingStageSceneIndex`, `_execStageSceneIndex`, `_taskControllerSceneIndex`, and sets
`_isPopulated=false`. It then rebuilds the renderer chain *and* the UsdImaging chain from scratch and re-calls
`SetStage` on the next `PrepareBatch`. usdview drives this from `stageView.py:1019-1027`
(`SetRendererPlugin` → `_handleRendererChanged` → `updateGL`).

Channels available to a plugin:

| Channel | Reaches | Limits | Evidence |
|---|---|---|---|
| `HdSceneIndexPlugin` `inputArgs` (renderer chain) | static registration args + `rendererDisplayName` only | no stage, no app objects | `sceneIndexPluginRegistry.cpp:1434-1439`, args overlay `:1451-1457` |
| `HdSceneIndexPluginRegistry::SceneIndexAppendCallback` + `HdUtils::RenderInstanceTracker<T>` | a per-engine `renderInstanceId` string, `"UsdImagingGLEngine_<renderer>_<ptr>"` | the **app** must register the instance before render index construction; the id is not exposed to Python | `hd/utils.h:44-90`; OpenUSD's own use: `engine.cpp:108-192` |
| `UsdImagingSceneIndexPlugin::AppendSceneIndex(inputScene)` (usdImaging chain) | **no** args at all | but grants `FlattenedDataSourceProviders` / `InstanceDataSourceNames` / `ProxyPathTranslationDataSourceNames` | `usdImaging/sceneIndexPlugin.h:54-56,60-92`; `sceneIndices.cpp:67-79` |
| `UsdImagingSceneIndexCreateArgsSchema` | carries a real `UsdStageRefPtrDataSource` … | consumed *only* inside `UsdImagingCreateSceneIndices` (`sceneIndices.cpp:327-345`) and stored as opaque `stageSceneIndexInputArgs`; there is no getter | `sceneIndexCreateArgsSchema.h:86` |
| `UsdImagingStageSceneIndex` | `SetStage`/`SetTime`/`ApplyPendingUpdates` are app-facing; **no `GetStage()`** | confirmed by header read | `stageSceneIndex.h:67,74,86` (no `GetStage`) |
| `HdSceneIndexNameRegistry` (process singleton) | a tool can *find* a scene index by name from Python (usdview's Hydra browser does: `usdviewq/hydraObserver.cpp:28,65`) | read-only `HdSceneIndexBase`; must be registered manually | `hd/sceneIndex.h:260-304` |
| **`UsdUtilsStageCache` + a C entry point** | the full stage, plus arbitrary plugin state | requires a Python usdview plugin to insert the stage and call the C API; must be re-run on stage replace | usdRig: `libs/rigExecImaging/registry.cpp:1157-1195`; `plugin/rigExecUsdview/rigExecUsdview.py:571-577` |

**Recommendation for the plan.** Two-tier state:

1. A **process-global `UsdGenImagingRegistry` singleton** owning all groom sessions, keyed by
   `UsdStage*` (weak) — the rigExec shape (`registry.h:355-475`). It survives renderer switches because it is
   not owned by the engine, and it is where caches (generated curves, texture handles, SeExpr compilations,
   frozen/combed edits) live.
2. Scene indices created by the plugin (renderer-chain `HdSceneIndexPlugin` for "after rigExec" ordering, per
   A4) hold only a **weak handle** to a session and re-attach on construction. Because they cannot see the
   stage, the *session key* must arrive out-of-band. Two viable keys:
   * **Stage-cache handoff** (rigExec pattern): usdview plugin does
     `UsdUtils.StageCache.Get().Insert(stage)` and calls `UsdGenImaging_Activate(cacheId, …)`. Works in
     usdview; **does not work in `usdrecord`/`hdPrman` batch** unless the batch tool is wrapped.
   * **Self-identifying scene**: the groom root prim carries a stable `usdGen:sessionId` string attribute
     (or the plugin keys on the terminal index's root layer identifier read from `__usdPrimInfo`/stage
     metadata mirrored into a Hydra container by our adapter). This is the only route that works
     **stage-free in batch**, and is the reason §1–§4 matter: *everything the evaluator needs must be in the
     data sources.*

Therefore: **make the data-source path the only required path**; make the stage-cache C API an *optional*
accelerator for interactive usdview tooling (freeze/comb, direct poking without a stage round-trip), exactly
as usdRig does. Design so that disabling the C API degrades performance, never correctness.

Also note the measured `SetStage(nullptr)` behaviour (probe2 L): the removal notice is sent while `_stage` is
still live (`stageSceneIndex.cpp:388-396` sends `_SendPrimsRemoved` before `_stage = stage`), and downstream
indices re-pulled and re-added the whole scene during the notice. A groom session must be tolerant of a
remove-then-immediate-re-add of `/` and must not free caches synchronously inside `PrimsRemoved`.

---

## 8. Probe inventory (all runnable headless)

Directory: `…/scratchpad/probes/G-stage-free/`

| File | What it does |
|---|---|
| `plugin/usdGenProbeSchema/resources/{plugInfo.json,generatedSchema.usda}` | hand-written **codeless** schema: `UsdGenProbeOperator`, `UsdGenProbeClumpStyler`, `UsdGenProbeImageableOp` |
| `scene.usda`, `scene_inst.usda`, `maps/*.exr` (empty files) | test scenes incl. `.spline`, timeSamples, UDIM asset, rest primvar, native instances |
| `probe.cpp` → `probe-out.txt` | codeless registration, terminal-index dump, per-type data source values, `DataSourceMapped`, REST factory |
| `probe2.cpp` → `probe2-out.txt` | notices for schema-attr vs primvar edits, prim add/retype, relationship primvars, `SetStage` cycle |
| `probe3.cpp` | same, but with a **deep pull** first — proves lazy time-varying/asset registration and `ArNotice::ResolverChanged` behaviour |
| `probe4.cpp` | `UsdExecImagingCreateStageSceneIndex()` availability and contents |
| `probe5.cpp` | custom container survival through the full chain and through native-instance propagation |
| `probe6.cpp` | `TsSpline` transported whole as `HdTypedSampledDataSource<TsSpline>` and evaluated as a ramp |
| `CMakeLists.txt`, `build/` | `find_package(pxr CONFIG PATHS $USD)`; build with `cmake -S . -B build -G Ninja && ninja -C build` |

Run any probe with
`PXR_PLUGINPATH_NAME=<probedir>/plugin/usdGenProbeSchema/resources ./build/probeN scene.usda` from the probe
directory (relative asset paths resolve against the scene's layer).

---

## Key facts

- A codeless schema needs no C++ at all: `plugInfo.json` + `generatedSchema.usda` gives working
  `GetTypeName`, `IsA`, prim definitions, fallbacks and derived-type queries. (probe1 §0; usdRig pattern at
  `<usdrig-src>/plugin/rigExecSchema/resources/plugInfo.json`)
- `UsdPrimDefinition::GetPropertyNames()` / `GetSpecType()` / `GetSchemaAttributeSpec()->GetTypeName()` is the
  generic way to build `UsdImagingDataSourceMapped` mappings for a codeless type. (`pxr/usd/usd/primDefinition.h:38,290,307`; probe1 §0)
- A codeless prim with no adapter reaches the terminal scene index with `primType == ""`, data sources
  `__usdPrimInfo primOrigin primvars usdMaterialBindings geomModel model __usdUpAxis skelBinding
  coordSysBinding purpose visibility xform materialBindings`, and **no schema attributes and no notices** for
  edits to them. (probe1 §1, probe2 A/B)
- `primOrigin` contains only `scenePath`; there is no `UsdPrim` and no `UsdStage` downstream.
  (probe1 §1c; `pxr/usdImaging/usdImaging/dataSourcePrim.cpp:520-575`)
- Asset attributes deliver a **resolved** path stage-free; unresolvable paths give `resolved=''`; UDIM
  identifiers get a resolved prefix only via UsdImaging's asset-path data source.
  (probe1 §2; `dataSourceAttribute.cpp:34-100`)
- `FlagAsAssetPathDependent` is specialised for scalar `SdfAssetPath` only — `asset[]` gets no reload
  tracking. (`dataSourceAttribute.h:203-212`; probe1 §2 stage-globals report lists only `usdGen:map`)
- Relationships yield `VtArray<SdfPath>` of *forwarded* targets and are never time-varying; a single-path
  factory also exists. (`dataSourceRelationship.h:22-59`; `dataSourceMapped.cpp:51-83`; probe1 §3/§4)
- Relationship primvars are returned by `UsdImagingDataSourcePrimvars::Get(name)` but are **not listed** by
  `GetNames()` (which uses `UsdGeomPrimvarsAPI::GetAuthoredPrimvars`).
  (`dataSourcePrimvars.cpp:97,167-174`; probe2 K)
- A `.spline` float attribute is sampled at scene time, has **no** USD time samples, is always deemed
  time-varying, and is therefore dirtied on every `SetTime`.
  (`stage.cpp:9751-9758`; probe1 §2, probe3 D/E)
- A whole `TsSpline` **can** be carried through Hydra as `HdTypedSampledDataSource<TsSpline>` and evaluated at
  an arbitrary parameter — a stage-free root-to-tip ramp with zero per-frame invalidation. Supported value
  types are `double/float/GfHalf/GfTimeCode`. (probe6; `pxr/base/ts/types.h:32-37`; `pxr/usd/usd/attribute.h:563`)
- A custom `AttributeMapping::factory` sampling `UsdTimeCode::Default()` gives REST values while the stage
  globals time is numeric, and does **not** register a time-varying flag. (probe1 §5)
- `primvars:rest` authored on the scalp passes through the whole chain untouched and is readable at any time.
  (probe1 §1e)
- Custom prim-level containers survive draw-mode, native-instance propagation + flattening, selection and
  render-settings flattening — including into `/UsdNiPropagatedPrototypes/.../UsdNiPrototype/*`. (probe5)
- Custom containers do **not** participate in native-instance aggregation keys; only a
  `UsdImagingSceneIndexPlugin` can extend those via `InstanceDataSourceNames()`.
  (probe5 prototype name `NoPrimvars___usdUpAxis…`; `usdImaging/sceneIndexPlugin.h:60-83`)
- Time-varying and asset-path dependency registration is **lazy** — it happens in the data source constructor,
  so a consumer that never pulls a value never gets its dirties. (probe2 D/E/F vs probe3 D/E;
  `dataSourceAttribute.h:205-212,228-243`)
- Overwriting a painted map on disk invalidates nothing; only `ArNotice::ResolverChanged` (whole-stage) or a
  layer-stack expression-variable change reaches `_assetPathDependents`; `stage->Reload()` does not.
  (probe3 H/H2; `stage.cpp:4718-4724,4957-5002`; `stageSceneIndex.cpp:586-595`)
- `ArNotice::ResolverChanged().Send()` produces exactly `*DIRTY /World/Clump {'primvars/usdGen:map'
  'primvars/usdGen:udimMap'}` — a usable "reload maps" trigger. (probe3 H)
- `UsdImagingStageSceneIndex` exposes `SetStage`/`SetTime`/`ApplyPendingUpdates` but **no `GetStage()`**;
  `UsdImagingSceneIndexCreateArgsSchema` carries a `UsdStageRefPtr` data source that is consumed only inside
  `UsdImagingCreateSceneIndices`. (`stageSceneIndex.h:67,74,86` (no `GetStage`); `sceneIndexCreateArgsSchema.h:86`;
  `sceneIndices.cpp:327-345`)
- `UsdImagingSceneIndexPlugin::AppendSceneIndex` takes **no** input args at all.
  (`usdImaging/sceneIndexPlugin.h:54-56`; `sceneIndices.cpp:67-79`)
- Renderer-chain plugins receive only their static registration args overlaid with `rendererDisplayName`, plus
  a `renderInstanceId` string in the callback form. (`sceneIndexPluginRegistry.cpp:1434-1457`)
- OpenUSD's own app↔plugin channel is `HdUtils::RenderInstanceTracker<T>` keyed by `renderInstanceId`
  (`hd/utils.h:44-90`), used by `UsdImagingGLEngine_Impl::_AppSceneIndices` (`engine.cpp:108-192`).
- `SetRendererPlugin` destroys the entire chain (including `_usdImagingSceneIndex` and `_execStageSceneIndex`)
  and rebuilds it, re-calling `SetStage` on the next `PrepareBatch`.
  (`engine.cpp:1449-1546`, `_DestroyHydraObjects` `:403-451`, `:534-545`)
- 26.08 ships an ExecUsd control plane in the engine — `USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX` (default
  false) merges `UsdExecImagingStageSceneIndex` as the strongest input and the engine hands it the stage and
  time — and it is **built** in this install (probe4: non-null). (`engine.cpp:90-96,1612-1660,489-490,534-545,2373-2374`)
- That plane is **not plugin-extensible in 26.08**: `UsdExecImaging_AdapterRegistry::GetPrimAdapter` hard-codes
  `UsdGeomXformable` and `ExecIrXformable`, with an explicit TODO.
  (`usdExecImaging/adapterRegistry.cpp:28-52`, `adapterRegistry.h:20-24`)
- The proven interactive handoff is rigExec's: usdview Python inserts the stage into `UsdUtilsStageCache` and
  passes the id through a `extern "C"` entry point to a process-global registry.
  (`<usdrig-src>/libs/rigExecImaging/registry.cpp:1157-1195`;
  `<usdrig-src>/plugin/rigExecUsdview/rigExecUsdview.py:571-577`)
- `HdSceneIndexNameRegistry` is a process singleton reachable from usdview's Python via `HydraObserver`, so a
  tool can *discover* our scene indices by name (read-only). (`hd/sceneIndex.h:260-304`;
  `usdviewq/hydraObserver.cpp:28,65`)

## Decisions this settles

1. **Every operator parameter is carried as Hydra data sources.** No design element may require a `UsdStage`
   downstream of the stage scene index. Verified sufficient for asset, asset[], float2[]/float[] knot arrays,
   int/string/token, relationships (single and array), and whole `TsSpline`s.
2. **Write a UsdImaging adapter; do not rely on `primvars:`.** Adapters give a typed `usdGen` container with
   nested locators, precise `UsdImagingDataSourceMapped::Invalidate`, custom factories (REST, splines), and no
   collision with real primvars. Build the mappings generically from `UsdPrimDefinition::GetPropertyNames()`
   so one adapter class serves every operator type (`includeDerivedPrimTypes: true` on an abstract
   `UsdGenOperator`).
3. **Ramps are knot arrays or transported `TsSpline`s, never a plain `.spline` value.** A `.spline` on a
   parameter attribute is a per-frame dirty and a recook; forbid it in the schema docs for ramp-shaped
   parameters (keep it for genuinely animated scalars such as global density).
4. **REST comes from an API-schema adapter sampling `UsdTimeCode::Default()`,** honouring an authored
   `primvars:rest` when present. Capture-on-first-cook is rejected as the primary mechanism.
5. **The ExecUsd control plane is optional and deferred.** It is not plugin-extensible in 26.08. The plugin
   ships its own evaluator; an `openexec` backend is a future adapter behind the same operator interface, to be
   revisited when `UsdExecImaging_AdapterRegistry` becomes plugin-driven.
6. **Evaluator state lives in a process-global session registry keyed by stage**, mirroring
   `RigExecImagingRegistry`; scene indices hold weak handles and re-attach after a renderer switch or stage
   replace. Sessions must survive `-REM /` + immediate re-add and must not free caches inside a notice.
7. **The usdview C-API handoff (`UsdGenImaging_Activate(stageCacheId, …)`) is an optional accelerator**, used
   for freeze/comb tooling and out-of-stage interaction. Correctness in `usdrecord`/hdPrman must not depend on
   it — those runs are driven entirely by authored data sources.
8. **Map reload is an explicit action.** Ship a "reload textures" command that sends
   `ArNotice::ResolverChanged` (and/or bumps our own generation counter). Do not document automatic pickup of
   an overwritten file.
9. **Pull what you depend on.** Because time-varying registration is lazy, the groom scene index must pull the
   surface points and every time-varying parameter it consumes at least once, or declare `__dependencies`.
10. **Per-instance groom variation requires the UsdImaging chain** (for `InstanceDataSourceNames`), or must be
    authored outside instanced scopes. Document the constraint.

## Open questions

- Does usdview's property editor offer a spline curve editor for `.spline` attributes in 26.08? If not, the
  "author a ramp as a spline" ergonomics argument weakens and knot arrays plus a custom usdview widget may be
  preferable. Not checked (needs a display).
- `hdPrman`'s chain: does a custom container published by a UsdImaging adapter reach hdPrman's terminal index
  the same way it reaches Storm's? Probe5 exercised only the UsdImaging chain (renderer-independent), so the
  renderer-plugin segment is unverified — cross-check with `G-hdprman-and-usdrecord-render-time-chain.md`.
- Exact cost of the per-frame dirty storm if a production groom authors many `.spline` parameters: measured
  qualitatively (one dirty per pulled spline attribute per `SetTime`), not benchmarked.
- Whether `UsdExecImaging_AdapterRegistry` gains plugin registration in a 26.11/27.x release, and whether
  `UsdExecImagingPrimAdapterInterface` is ABI-stable enough to target speculatively.
- The double-add observed after `SetStage(nullptr)` (probe2 L) comes from downstream indices re-pulling during
  the removal notice; the exact index responsible was not isolated. Worth confirming that a groom session
  keyed by stage is robust to it.
- `HdSceneIndexNameRegistry` gives Python a read-only handle to a named scene index; whether that is enough for
  the freeze/comb tools to avoid the stage-cache C API (e.g. by pulling frozen curve data straight out of a
  named results index) was not prototyped.
- Whether an `asset[]` map list can be given reload tracking by calling `FlagAsAssetPathDependent` manually from
  a custom factory (the API is on the public `UsdImagingDataSourceStageGlobals` interface, so it looks possible)
  — not tested.
