# A2 — usdRig (RigExec) Hydra 2.0 imaging: scene indices, chain placement, invalidation, C entry points, guides

Evidence report for the usdGen hair/fur plan. All paths are absolute. Line numbers refer to
`/home/burkard/work/usdRig` (commit c92c040) and `/home/burkard/work/OpenUSD` (tag v26.08).
Claims without a citation are marked UNVERIFIED.

Abbreviations: `R` = `/home/burkard/work/usdRig`, `U` = `/home/burkard/work/OpenUSD/pxr`,
`SI` = scene index, `DS` = data source.

---

## 0. One-paragraph orientation

RigExec publishes into Hydra 2.0 through a single `UsdImagingSceneIndexPlugin`
(`R/libs/rigExecImaging/sceneIndexPlugin.cpp:23-46`) that appends three filtering scene indices
(`pruning -> binding -> results`) inside `UsdImagingCreateSceneIndices`, at the same slot UsdSkel
uses. Evaluation never happens in `GetPrim()`: the usdview Python plugin drives a process-global
registry through a ctypes C API (`RigExecImaging_Activate/SetTime`), the registry evaluates the
rig synchronously on the calling thread, atomically swaps an immutable snapshot generation into a
lock-free `RigExecSnapshotStore`, diffs it against the previous generation, and only then sends
`PrimsAdded/Removed/Dirtied` to every registered chain. Because the plugin slot sits downstream of
the only `HdFlatteningSceneIndex` in the UsdImaging chain, every xform RigExec sees is already
world-space; it therefore publishes transforms as a `(revised local, base local)` pair turned into
a post-multiplied world-space delta, dirties whole subtrees by hand, and synthesizes guide prims
(spheres/cones/curves/meshes) as children of joints/controls that do not exist on the stage.
A hair system placed downstream of this plugin sees deformed `primvars/points` from RigExec as
plain retained `VtVec3fArray` data sources, but it does NOT see UsdSkel-skinned points as plain
primvars (UsdSkel blocks `primvars/points` and publishes GPU/CPU ext computations instead).

---

## 1. The exact scene-index chain in stock usdview 26.08

### 1.1 Construction sites

usdview creates the engine with default `Parameters` (so `sceneDelegateID = "/"` and no
`HdPrefixingSceneIndex`):

```python
# U/usdImaging/usdviewq/stageView.py:965-968
params = UsdImagingGL.Engine.Parameters()
params.allowAsynchronousSceneProcessing = self._allowAsync
params.displayUnloadedPrimsWithBounds = self._bboxstandin
self._renderer = UsdImagingGL.Engine(params)
```

`U/usdImaging/usdImagingGL/engine.h:91` — `SdfPath sceneDelegateID = SdfPath::AbsoluteRootPath();`
`U/usdImaging/usdImagingGL/engine.cpp:78-79` — `USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX` default `true`.

The engine builds the graph in two calls:

| Step | Site | What is created |
|---|---|---|
| A | `engine.cpp:1749-1794` `_CreateSceneIndexChainAndRenderer` | `_mergingSceneIndex = HdMergingSceneIndex::New()` (:1757), then `HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer(rendererDisplayName, sceneIndex, renderInstanceId)` (:1776-1780, **no appName**), optional `HdCachingSceneIndex` (`USDIMAGINGGL_ENGINE_ENABLE_CACHING_SCENE_INDEX`, :1782-1786), `_terminalSceneIndex` (:1788), `plugin->CreateRenderer(_terminalSceneIndex, rendererCreateArgs)` (:1791-1793) |
| B | `engine.cpp:1528-1544` | `_CreateUsdImagingSceneIndices(sceneIndexCreateArgs)` then `_mergingSceneIndex->InsertInputScenes({{sceneIndex, _sceneDelegateId}})` (:1544) |
| C | `engine.cpp:1566-1595` | `HdxTaskControllerSceneIndex` inserted into the same merging SI at `/_UsdImaging_<renderer>_<ptr>` (:1592-1593) |

`_CreateUsdImagingSceneIndices` (`engine.cpp:1677-1700`):

```cpp
sceneIndex = _usdImagingSceneIndex = UsdImagingSceneIndex::New(
    sceneIndexCreateArgs,
    std::bind(&UsdImagingGLEngine::_AppendOverridesSceneIndices, this, _1));   // :1683-1689
sceneIndex = _displayStyleSceneIndex = HdsiLegacyDisplayStyleOverrideSceneIndex::New(sceneIndex); // :1691-1693
if (!_sceneDelegateId.IsAbsoluteRootPath())  sceneIndex = HdPrefixingSceneIndex::New(...);       // :1695-1698 (not in usdview)
```

`UsdImagingSceneIndex` is an encapsulating wrapper (`U/usdImaging/usdImaging/sceneIndex.h:36-38`)
whose ctor calls the (deprecated but still the real implementation) `UsdImagingCreateSceneIndices`
(`sceneIndex.cpp:20,34`) and forwards notices of `_finalSceneIndex` (`sceneIndex.h:117-152`).

### 1.2 Inside `UsdImagingCreateSceneIndices` (`U/usdImaging/usdImaging/sceneIndices.cpp:191-325`)

| # | Line | Scene index | Notes |
|---|---|---|---|
| 1 | 201-206 | `UsdImagingStageSceneIndex` | stage set here only if `USDIMAGING_SET_STAGE_AFTER_CHAINING_SCENE_INDICES=false` (:208-212); default `true` (:42-50) so `SetStage` happens at :318-322 **after** the whole chain is built, so `PrimsAdded` flows through every filter including plugins |
| 2 | 218-220 | `HdsiLocatorCachingSceneIndex` (materials) | |
| 3 | 222-225 | `overridesSceneIndexCallback` | = engine `_AppendOverridesSceneIndices` (`engine.cpp:1612-1675`): optional exec merge (`USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX`, off), `HdsiPrefixPathPruningSceneIndex`, `HdsiPrimTypeAndPathPruningSceneIndex` (lights), `UsdImagingRootOverridesSceneIndex`, `UsdImagingLegacyRenderSettingsSceneIndex` |
| 4 | 227-230 | `UsdImagingUnloadedDrawModeSceneIndex` | only with bbox stand-ins |
| 5 | 232-234 | `UsdImagingExtentResolvingSceneIndex` | uses `extentsHint` when `extent` unauthored (`extentResolvingSceneIndex.h:29-35`) |
| 6 | 236-241 | `UsdImagingPiPrototypePropagatingSceneIndex` | point instancers |
| 7 | 243-284 | `UsdImagingNiPrototypePropagatingSceneIndex` | contains **the only `HdFlatteningSceneIndex::New`** in the chain (`niPrototypePropagatingSceneIndex.cpp:204`); draw-mode SI inserted via callback (:274-279). Plugin `FlattenedDataSourceProviders()` are folded in here (`sceneIndexPlugin.h:58-67`) |
| 8 | 286-287 | `HdNoticeBatchingSceneIndex` (postInstancing) | engine batches around `SetStage`/`ApplyPendingUpdates` (`engine.cpp:536-539`, `2365-2376`) |
| 9 | 295-296 | `UsdImaging_InstanceProxyPathTranslationSceneIndex` | plugin `ProxyPathTranslationDataSourceNames()` folded in (:180-189) |
| 10 | 298-299 | `UsdImagingMaterialBindingsResolvingSceneIndex` | |
| **11** | **301-302** | **`_AddPluginSceneIndices`** | `for plugin in UsdImagingSceneIndexPlugin::GetAllSceneIndexPlugins(): sceneIndex = plugin->AppendSceneIndex(sceneIndex)` (:66-78). **RigExec's three filters live here; so does UsdSkel's `Skeleton/PointsResolving` pair.** |
| 12 | 304-305 | `UsdImagingSelectionSceneIndex` | |
| 13 | 307-308 | `UsdImagingRenderSettingsFlatteningSceneIndex` | |
| 14 | 310-314 | `HdMakeEncapsulatingSceneIndex` | only with `HD_USE_ENCAPSULATING_SCENE_INDICES=1` (default false, `U/imaging/hd/sceneIndexUtil.cpp:18-21`) |

### 1.3 After UsdImaging: merge, renderer-level plugins, Storm

```
UsdImagingSceneIndex (all of §1.2)
  -> HdsiLegacyDisplayStyleOverrideSceneIndex                      engine.cpp:1691-1693
  -> HdMergingSceneIndex  [input 0 = UsdImaging @ "/", input 1 = HdxTaskControllerSceneIndex]   engine.cpp:1544, 1592
  -> HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer("GL", ...)                       engine.cpp:1776-1780
       phase 0, AtStart, callback: engine _AppSceneIndices -> HdsiSceneGlobalsSceneIndex,
                                    HdsiDomeLightCameraVisibilitySceneIndex,
                                    HdsiSceneMaterialPruningSceneIndex                          engine.cpp:147-162, 183-202
       all-renderers plugins (loadWithRenderer ""): HdGpSceneIndexPlugin (phase 2, gated OFF by
                                    HDGP_INCLUDE_DEFAULT_RESOLVER=false), HdsiDebuggingSceneIndexPlugin (gated by env)
       Storm plugins (loadWithRenderer "GL"): HdSt_* at phases 0,1,3,4,100,900,1000               U/imaging/hdSt/plugInfo.json
  -> [HdCachingSceneIndex, optional]                                                              engine.cpp:1782-1786
  -> _terminalSceneIndex -> plugin->CreateRenderer(terminal, args)                               engine.cpp:1788-1793
       -> HdRendererPlugin::_CreateRendererFromRenderDelegate                                    U/imaging/hd/rendererPlugin.cpp:145-156
       -> HdRenderDelegateAdapterRenderer -> HdRenderIndex::NewForBackendEmulation(delegate, drivers, terminal)
                                                                                                U/imaging/hd/renderDelegateAdapterRenderer.cpp:197-207
       -> HdRenderIndex ctor: terminalSceneIndex given => NO second AppendSceneIndicesForRenderer,
          emulation API disabled; HdSceneIndexAdapterSceneDelegate(terminal) feeds Storm's legacy
          HdRenderDelegate; renderDelegate->SetTerminalSceneIndex(terminal)                    U/imaging/hd/renderIndex.cpp:172-175, 223-230
```

Storm's renderer display name is `"GL"` (`U/imaging/plugin/hdStorm/plugInfo.json:10`), which is
the `loadWithRenderer` key its plugins use (`U/imaging/hdSt/plugInfo.json`). Storm in 26.08 is a
**legacy render delegate** consumed through `HdSceneIndexAdapterSceneDelegate`; there is no
`HdsiPrimManagingSceneIndexObserver` in hdSt (grep of `U/imaging/hdSt` for it: no hits; the only
users are `hdsi/primManagingSceneIndexObserver.{h,cpp}`).

Only when a render index is built **without** a terminal scene index (legacy apps calling
`HdRenderIndex::New(delegate, drivers, instanceName, appName)`, `renderIndex.cpp:249-266`) does
the render index itself call `AppendSceneIndicesForRenderer(rendererDisplayName, sceneIndex,
instanceName, appName)` (`renderIndex.cpp:203-213`) on its own emulation merging SI. In usdview
that branch is not taken.

### 1.4 Where the hair system's competitors sit

| Component | Level | Position |
|---|---|---|
| UsdSkel skinning (`UsdSkelImagingResolvingSceneIndexPlugin`) | UsdImaging plugin slot (#11) | same slot as RigExec; relative order unspecified (§2.1) |
| RigExec (`RigExecUsdImagingSceneIndexPlugin`) | UsdImaging plugin slot (#11) | same |
| hdGp procedural resolver | renderer level, all renderers, phase 2 (`U/imaging/hdGp/sceneIndexPlugin.h:29-37`), after `hd:sceneAssembly` tag | **disabled by default** (`HDGP_INCLUDE_DEFAULT_RESOLVER=false`, `hdGp/sceneIndexPlugin.cpp:25-27`, `_IsEnabled` :60-66) |
| Storm's own SIs | renderer level, "GL" only | phases 0..1000 |

---

## 2. Candidate hook points for the hair system

### 2.1 (a) A second `UsdImagingSceneIndexPlugin` — the ordering problem, precisely

Enumeration:

```cpp
// U/usdImaging/usdImaging/sceneIndexPlugin.cpp:174-222
std::set<TfType> pluginTypes;
PlugRegistry::GetAllDerivedTypes(TfType::Find<UsdImagingSceneIndexPlugin>(), &pluginTypes);
for (const TfType &pluginType : pluginTypes) { ... plugin->Load() ... factory->Create() ... }
```

`std::set<TfType>` orders by `TfType::operator<`, which compares the internal `_TypeInfo*`
pointer (`U/base/tf/type.h:119`: `inline bool operator <(const TfType& t) const { return _info < t._info; }`).
`_TypeInfo` objects are heap-allocated on declaration (`U/base/tf/type.cpp:260`
`new TfType::_TypeInfo(typeName)`), which happens in `PlugPlugin::_DeclareTypes` after plugInfo
discovery (`U/base/plug/registry.cpp:152-156`) iterating the `Types` `JsObject` (a `std::map`,
so alphabetical **within** one plugInfo, `plug/plugin.cpp:542-552`), for plugins discovered from
the path list in order (`Plug_ReadPlugInfo`, `plug/info.cpp:688-731`, `pathsAreOrdered=true` from
`registry.cpp:102`) but read in parallel within a path via a task arena.

**Conclusion:** the relative order of RigExec, UsdSkel, and a usdGen `UsdImagingSceneIndexPlugin`
is a heap-address comparison. There is no priority, no tag, no `after` key, and no registration
API in this slot. `sceneIndexPlugin.h:51-129` exposes only `AppendSceneIndex`,
`FlattenedDataSourceProviders`, `InstanceDataSourceNames`, `ProxyPathTranslationDataSourceNames`.
RigExec's own design doc records the same conclusion and accepts it
(`R/docs/imaging-datasource-redesign.md` §0.4 "Ordering: accept whatever the plugin system gives",
and the "Known exposure" paragraph). Empirically the order will usually follow allocation order
(plugin discovery order × alphabetical type name) but nothing guarantees that.

Consequences for hair:
- If usdGen lands before RigExec in this slot, it reads **pre-rig** points: hair stays on the
  bind pose. Silent wrong geometry.
- If it lands before UsdSkel it reads rest points (or nothing — see §8).
- Pros: zero application glue; runs in usdview, usdrecord (`U/usdImaging/usdAppUtils/frameRecorder.cpp:38-45`
  uses `UsdImagingGLEngine`), any UsdImaging host; sees pre-selection, pre-render-settings prims;
  gets to extend flattening/instancing via the three virtuals.
- Cons: unordered relative to the deformers it must follow. Not acceptable as the *only* placement
  for a plugin whose entire input is other plugins' output.

### 2.2 (b) An `HdSceneIndexPlugin` registered for all renderers (renderer level)

Registration shape (`U/imaging/hdGp/sceneIndexPlugin.cpp:20-40`, `hdGp/plugInfo.json:4-23`):

```jsonc
"UsdGenSceneIndexPlugin": {
  "bases": ["HdSceneIndexPlugin"], "displayName": "...", "priority": 0,
  "loadWithRenderer": "",                 // "" == HdSceneIndexPluginRegistryTokens->allRenderers; triggers library preload
  "loadWithApps": [],                     // optional; absent => all apps  (sceneIndexPluginRegistry.cpp:1133-1146)
  "tags": ["usdGen:groomResolution"],
  "ordering": { "after": ["hd:sceneAssembly", "hd:sceneGlobals"], "before": ["hdGp:proceduralResolution"], "position": "firstAfter" }
}
```

```cpp
TF_REGISTRY_FUNCTION(TfType) { HdSceneIndexPluginRegistry::Define<UsdGenSceneIndexPlugin>(); }
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
  HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
      HdSceneIndexPluginRegistryTokens->allRenderers, TfToken("UsdGenSceneIndexPlugin"),
      nullptr, /*insertionPhase*/ 1, HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}
// override _AppendSceneIndex(renderInstanceId, inputScene, inputArgs)   hd/sceneIndexPlugin.h:107-125
// override _IsEnabled(inputArgs)                                         hd/sceneIndexPlugin.h:127-134 (default true, .cpp:54-57)
```

Facts about this level (all `U/imaging/hd/sceneIndexPluginRegistry.cpp`):
- JSON keys parsed: `loadWithRenderer`, `loadWithApps`, `tags`, `ordering{after,before,position}`
  with `position ∈ {firstAfter, lastBefore, doesNotMatter}` (tokens :53-65; parser :1251-1297;
  metadata collection :1300-1356). A JSON entry is only created for renderers named in
  `loadWithRenderer` (:1339-1342), so **`loadWithRenderer` is mandatory** for JSON ordering.
- Ordering policy env `HD_SCENE_INDEX_PLUGIN_ORDERING_POLICY_DEFAULT` default `"Hybrid"` (:40-48).
  Hybrid = registration phase becomes a manufactured tag `phaseN` with an `after: phase<prev>`
  constraint (:915-960), merged with JSON tags (:831-912), then Kahn's topological sort with
  tie-break `(position, sceneIndexPluginId)` (:760-822). Islands (no edges) go last (:775-806).
  Cycles: one offending plugin is isolated, else fallback to registration order (:635-720).
- "All renderers" entries precede same-phase renderer-specific entries (registration path :494-560;
  hybrid path :980-1005 via `phaseN_` tags).
- The tags `hd:sceneAssembly` and `hd:sceneGlobals` referenced by hdSt/hdGp plugInfo are **not
  defined anywhere in C++** (grep of `U/**/*.{cpp,h,py}` for `sceneAssembly`: no hits); an
  `after` tag with no tagged entry is silently ignored (:606-613). They are documentation only.
- `inputArgs` always carries `__rendererDisplayName` (:1435-1439); the engine passes nothing else
  (`engine.cpp:1776-1780`), so **no stage, no time, no create-args reach this plugin**.
- `appName` is empty in usdview (`engine.cpp:1778-1780` omits it), so `loadWithApps` cannot be
  used to target usdview; plugins without `loadWithApps` load everywhere.

Placement: after the whole UsdImaging chain **and** after `HdMergingSceneIndex`, i.e. after
RigExec, UsdSkel, flattening, instancing, selection, render settings, and with task-controller
prims present. Relative to Storm's own indices it can be pinned with `before/after` tags
(Storm's `HdSt_ImplicitSurfaceSceneIndexPlugin` etc. are phase 0 `firstAfter`;
`HdSt_RenderPassPruneSceneIndexPlugin` phase 1 is `before: hdGp:proceduralResolution`;
`HdSt_MaterialPrimvarTransferSceneIndexPlugin` phase 3 is `after: hdGp:proceduralResolution`
— `U/imaging/hdSt/plugInfo.json:51-56, 116-121, 64-69`).

Does it run elsewhere? Yes: `loadWithRenderer: ""` loads for every renderer display name that
calls `AppendSceneIndicesForRenderer` — the engine (usdview, usdrecord) and any
`HdRenderIndex::New` without terminal SI (`renderIndex.cpp:203-213`). hdPrman is not in this
tree (`third_party/renderman` exists as a directory but hdPrman sources were not inspected —
UNVERIFIED whether hdPrman's own construction path calls the registry; it must, since its
`HdPrman_*` scene index plugins are registered the same way).

Pros: deterministic ordering relative to every UsdImaging deformer (strictly after all of #11);
`tags/ordering` control relative to Storm/hdGp; renderer name available to gate GPU vs CPU
paths. Cons: sees propagated prototype paths (`/…/__Prototype_N/…`) and task-controller prims;
cannot receive the stage or time (same as (a)); `HdxTaskControllerSceneIndex` prims and
`HdsiSceneGlobalsSceneIndex` are in the input — the latter is actually useful: the scene-globals
prim carries `currentFrame` (`U/imaging/hd/sceneGlobalsSchema.h:44-45,188`), set by
`UsdImagingGLEngine::_SetSceneGlobalsCurrentFrame` on every `Render` (`engine.cpp:496, 2062-2073`),
so a phase-≥1 all-renderers plugin can read the frame off `HdSceneGlobalsSchema::GetDefaultPrimPath()`
and is dirtied when it changes (a legitimate time source that (a) does not have).

### 2.3 (c) hdGp generative procedural

Placement only (the parallel hdGp report covers the API): `HdGpSceneIndexPlugin` is an
all-renderers `HdSceneIndexPlugin` at insertion phase 2 (`U/imaging/hdGp/sceneIndexPlugin.h:29-37`),
`InsertionOrderAtStart` (`sceneIndexPlugin.cpp:34-39`), tagged `hdGp:proceduralResolution` with
`after: hd:sceneAssembly` (`hdGp/plugInfo.json:15-21`). It is **off unless
`HDGP_INCLUDE_DEFAULT_RESOLVER=1`** (`sceneIndexPlugin.cpp:25-27, 60-66`), so stock usdview does
not resolve procedurals without that env var. hdGp resolves at the same level as (b), i.e.
strictly after UsdImaging plugins; a procedural's `GetChildTypes/Update` reads the input scene
(deformed points included, subject to §8).

### 2.4 (d) Explicit hand-off: RigExec's plugin appends the hair scene index itself

RigExec's `AppendSceneIndex` (`R/libs/rigExecImaging/sceneIndexPlugin.cpp:26-40`) ends with
`registry.RegisterChain(pruning, binding, results); return results;`. A registry hook (e.g.
`RigExecImagingRegistry::SetPostChainCallback(std::function<HdSceneIndexBaseRefPtr(HdSceneIndexBaseRefPtr)>)`
consulted before `return results;`) would let usdGen append after RigExec *deterministically*
inside slot #11. It does not exist today (no such member in `registry.h:28-126`). Pros: strict
"after RigExec" at the UsdImaging level, pre-selection; can share RigExec's generation/notice
serialization; no renderer-level path translation. Cons: requires modifying usdRig (out of scope
for a sibling project unless coordinated); still unordered relative to UsdSkel and any third
plugin; couples usdGen's lifetime to RigExec's chain registry (`registry.cpp:65-80` stores weak
pointers to chains); does nothing when RigExec is not installed — so usdGen would need a fallback
registration anyway (which reintroduces the ordering question).

### 2.5 Summary table

| | (a) UsdImaging plugin | (b) Hd plugin, all renderers | (c) hdGp | (d) RigExec hand-off |
|---|---|---|---|---|
| Strictly after RigExec | no (pointer order) | **yes** | yes | yes |
| Strictly after UsdSkel | no | **yes** | yes | no |
| After flattening | yes | yes | yes | yes |
| Before selection / render-settings flattening | yes | no | no | yes |
| Stage/time handle | none | none (but `currentFrame` on scene-globals prim) | none | via RigExec registry (stage-owning) |
| Ordering controls | none | phase + JSON tags | fixed phase 2 | code |
| Runs in usdrecord / other engines | yes | yes | only with env | only with RigExec |
| Prototype paths | propagated | propagated | propagated | propagated |
| Needs changes to OpenUSD | no | no | no | no (changes usdRig) |

---

## 3. How `RigExecResultsSceneIndex` computes, caches, threads, and dirties

### 3.1 Data flow and the generation fence

```
usdview frame signal (Python)            R/plugin/rigExecUsdview/rigExecUsdview.py:174, 600-610
  -> RigExecImaging_SetTime(frame)       R/libs/rigExecImaging/registry.cpp:1197-1203   (ctypes)
  -> RigExecImagingRegistry::SetTime     registry.cpp:342-377   (holds _mutex; non-recursive)
     -> _EvaluateSessions               registry.cpp:82-179    (per rig: bridge->EvaluateAndPublishResult(time))
        -> RigExecImagingBridge::EvaluateAndPublishResult   bridge.cpp:1130-1230
           1. pose = _evaluator->Evaluate(time)              :1134   (synchronous, OpenExec + mover graph)
           2. build RigExecImagingSnapshot from pose.movedProperties ("points"|"normals"|"extent") :1169-1204
              + _FillProviderXforms/_FillGuides/_FillControlGuides/_FillVolumeGuides/_FillWeightOverlay :1207-1211
           3. epoch digest changed -> new BindingEpoch{id, publishedPrims}     :1213-1223
           4. result.dirtied = _store->Publish(snapshot)                        :1227
     -> registry merges per-rig snapshots into ONE combined generation (cross-rig overlap is an error) :151-166
     -> _Publish: snapshot->generation = ++_generation; _store->Publish; epoch if id changed          :181-196
     -> _Broadcast: for each live chain: binding->SetBindingEpoch(epoch); results->NotifyGenerationPublished(dirtied) :497-516
```

`RigExecSnapshotStore::Publish` (`R/libs/rigExecImaging/snapshotStore.h:326-372`) takes
`_writeMutex`, derives `hasDrivenXforms`, diffs every prim against the previous generation with
`_Diff` (:385-500), records prims that left the set as structural (:362-368), then
`std::atomic_store(&_current, snapshot)` (:369-370). Readers call `Get()` =
`std::atomic_load(&_current)` (:376-378). One pull takes one `shared_ptr<const Snapshot>` and
resolves everything against it (`sceneIndices.cpp:2134-2166` insists on this for guide placement
so a pull never mixes generations).

Spec statement of the rule: `R/docs/spec.md` §10.3 — "GetPrim() only reads an atomic immutable
snapshot and constructs/returns cached data-source overlays. It never calls Compute, waits for
evaluation, changes time, or locks the authoring stage."

### 3.2 `GetPrim` (`R/libs/rigExecImaging/sceneIndices.cpp:1545-1709`)

1. Guide-name interception (`rigGuideSphere_N`, `rigGuideCone_N`, `rigGuideCtrl`, `rigGuideVol_N`)
   — only when the upstream has **no** prim at that path and the parent exists upstream (:1552-1660).
2. Otherwise `prim = _GetInputSceneIndex()->GetPrim(primPath)`; a null upstream `dataSource` means
   no publication (:1662-1666).
3. Driven-xform delta for **every** prim (not only published ones) via `_ComputeDrivenXform`
   (:1676-1694) → overlay `{xform: {matrix, resetXformStack=true}}` with
   `HdOverlayContainerDataSource::OverlayedContainerDataSources(strong, prim.dataSource)` (:1689-1691).
4. If the prim is in the snapshot: `_BuildStrongRoot(published)` (:1322-1441) overlaid as the
   stronger container (:1699-1707).

`_BuildStrongRoot` publishes only standard schemas:

| Leaf | Data source | Line |
|---|---|---|
| `primvars/points/{primvarValue,interpolation=vertex,role=point}` | `HdRetainedTypedSampledDataSource<VtVec3fArray>` or `_SampledPointsDataSource` (multi-sample) | :1336-1357 |
| `primvars/velocities`, `primvars/accelerations` | `HdBlockDataSource` whenever points are owned | :1358-1363 |
| `primvars/normals/…` | retained `VtVec3fArray`, vertex, role normal | :1365-1375 |
| `primvars/displayColor` (weight overlay only) | retained `VtVec3fArray`, vertex, role color | :1405-1417 |
| `extent/min`, `extent/max` | `HdExtentSchema::Builder` with retained `GfVec3d` | :1424-1433 |
| `xform` | **not here** — computed in `GetPrim` against the upstream world matrix | comment :1327-1333 |

Data sources are **rebuilt on every pull** (no per-generation cache of container handles); that
is why every dirty notice is expanded with `HdContainerDataSourceEditor::ComputeDirtyLocators`
(§3.4). `_SampledPointsDataSource::GetContributingSampleTimesForInterval` returns the retained
offsets only when ≥2 samples exist (:305-312); single-sample prims return `false`.

### 3.3 Threading

- `GetPrim`/`GetChildPrimPaths` are lock-free readers: one atomic load of the snapshot, no mutex
  (`sceneIndices.cpp:1662`, `1716`). Hydra requires these to be thread-safe
  (`U/imaging/hd/sceneIndex.h:98,109`); `HdSceneIndexAdapterSceneDelegate` pulls from the
  render index sync (parallel `_primCache.ParallelForEach`, `sceneIndexAdapterSceneDelegate.cpp:2973`).
- Notice emission is single-threaded by contract (`sceneIndex.h:185-215` "not threadsafe; some
  observers expect it to be called from a single thread"). RigExec sends notices from whichever
  thread called `SetTime`/`Activate`/`SetWeightOverlay`/`_OnObjectsChanged` — in usdview always
  the Qt main thread (frame signal, stage edit notices).
- The registry serializes evaluate-then-publish under one non-recursive `std::mutex`
  (`registry.cpp:71,198,345,385,428,467,477`); `SetWeightOverlay` and `_OnObjectsChanged` release
  the lock before calling `SetTime` to avoid self-deadlock (:419, :466-470).
- Edit-driven re-evaluation: `TfNotice::Register(... UsdNotice::ObjectsChanged, stage)` is
  registered **before** `Compile()` so it is delivered *after* OpenExec's own listener
  (`registry.cpp:225-262` explains why: `Tf_NoticeRegistry` prepends, so the last registrant runs
  first, and evaluating before exec invalidated would publish a one-edit-stale generation).
- Hidden bookkeeping mutation on the notice path: `_announcedGuides`, `_announcedDrivenXforms`,
  `_announcedWeightOverlays` are plain `std::map/std::set` mutated only in `NotifyGenerationPublished`
  / `_PrimsAdded` / `_PrimsRemoved` (declared `sceneIndices.h:280-324`), never in `GetPrim`.
- Evaluation happens on the render/UI thread synchronously; the redesign doc flags that moving it
  off-thread needs an `HdNoticeBatchingSceneIndex` of its own (`imaging-datasource-redesign.md` §6.1).

### 3.4 Dirty notices: locators and structural vs value

`RigExecPublishedChange` bits (`snapshotStore.h:291-308`): `Xform`, `Points`, `Normals`,
`Extent`, `Structural`, `Guides`, `WeightOverlay`. `_Diff` (:385-500) reports **structural** for:
prim entering/leaving the set; any `has*` flag flipping (ownership change); guide element count;
volume-guide prim type; asset root change on guide-bearing prims; control guide shape/drawMode.
Everything else is a value bit.

`NotifyGenerationPublished` (`sceneIndices.cpp:2178-2516`) maps bits to notices:

| Change | Emitted |
|---|---|
| Structural | `DirtiedPrimEntry(path, UniversalSet())` (:2207-2208); if weight overlay appeared/disappeared, **re-`PrimsAdded`** the prim with its upstream type (:2236-2257 — because `HdSceneIndexAdapterSceneDelegate` caches primvar descriptors and only rebuilds them on `PrimsAdded`, see `sceneIndexAdapterSceneDelegate.cpp:287-298`); if a driven xform appeared/disappeared, `_DirtySubtree(path, UniversalSet)` over every upstream descendant + guide children (:2272-2280); reconcile guide children with exact `PrimsAdded/PrimsRemoved` and universal-dirty survivors (:2287-2352) |
| Guides | universal dirty on each announced synthesized child, or `PrimsAdded` if never announced (:2354-2442) |
| Xform | leaf `xform/matrix`; plus `_DirtySubtree(path, ComputeDirtyLocators({xform/matrix}))` over all upstream descendants (:2444-2464) |
| Points | leaves `primvars/points/primvarValue`, `primvars/velocities`, `primvars/accelerations` (:2466-2476) |
| Normals | `primvars/normals/primvarValue` (:2478-2481) |
| WeightOverlay | `primvars/displayColor/primvarValue` (:2483-2491) |
| Extent | `extent/min`, `extent/max` (:2493-2499) |
| all value leaves | expanded with `HdContainerDataSourceEditor::ComputeDirtyLocators(leaves)` (:2504-2505) |

Order of emission: `_SendPrimsRemoved(removedGuides)`, `_SendPrimsAdded(addedGuides)`,
`_SendPrimsDirtied(entries)` (:2507-2515). `RigExecBindingResolvingSceneIndex::SetBindingEpoch`
separately dirties every previously and newly published prim universally (:166-190).

Upstream notices are forwarded verbatim except: the pruning SI drops entries under owned
`__RigExecGenerated` scopes (:93-160); the results SI, on any upstream `visibility` or `xform`
dirty of a guide parent, adds `ComputeDirtyLocators({visibility, xform})` entries for its
synthesized children (:2686-2741). `_PrimsAdded/_PrimsRemoved` keep the announcement history
even while unobserved (:2518-2685) so a late observer's traversal and later removals agree.

Why this matters for hair (invalidation contract, §9): the hair SI will receive
`primvars/points/primvarValue` (plus container sentinels from `ComputeDirtyLocators`) on the
deformed surface, `xform/matrix` on every descendant of a driven Xform, `UniversalSet` on
structural changes, and occasionally a **re-`PrimsAdded`** of an existing mesh (type unchanged)
which the adapter delegate treats as "invalidate everything" (`sceneIndexAdapterSceneDelegate.cpp:244-264, 300-310`).

---

## 4. The flattening / world-space lesson and what it implies for hair

Verified chain fact: the only `HdFlatteningSceneIndex::New` outside tests is inside
`UsdImagingNiPrototypePropagatingSceneIndex` (`niPrototypePropagatingSceneIndex.cpp:204`),
constructed at `sceneIndices.cpp:281-283`, 21 lines before `_AddPluginSceneIndices` (:301-302).
`HdFlattenedXformDataSourceProvider` marks every flattened matrix `resetXformStack=true`
(`U/imaging/hd/flattenedXformDataSourceProvider.cpp:118-135`), and
`HdSceneIndexAdapterSceneDelegate::GetTransform` reads the prim's own `xform/matrix` with no
ancestor accumulation (`sceneIndexAdapterSceneDelegate.cpp:2478-2496`).

RigExec's response (`R/docs/hydra-flattening-position.md`; `sceneIndices.cpp:1443-1543`):
- Publish `(xform, xformBase)` (`snapshotStore.h:94-105`) and compute
  `delta = W_old^-1 * L_new * L_old^-1 * W_old`, post-multiplied, innermost driven ancestor first
  (:1521-1537); fail closed on non-invertible matrices via `_TryInvert` (:1264-1305).
- Stamp `resetXformStack=true` on the published matrix (:1682-1687) — downstream of flattening the
  flag means "already composed", not "ignore ancestors" (known limitation :1496-1503).
- Dirty descendants by hand because Hydra dirtiness is not hierarchical
  (`U/imaging/hd/sceneIndexObserver.h:76-79`: "This notice only affects the named prim;
  descendants of primPath are unaffected").

Implications for a hair scene index that sits after RigExec (any of §2's options):
1. **Every `xform/matrix` it reads is world (or prototype-common) space.** Surface points are in
   the mesh's local space; `world = points * xform`. Curve outputs must be published either
   (i) as a child/sibling prim with its own `xform = surface world xform, resetXformStack=true`
   and points in the surface's local space, or (ii) with identity xform (`resetXformStack=true`)
   and world-space points. Option (i) keeps hair invariant under rigid motion of the whole
   asset (no re-grooming when only the transform animates) and is what RigExec's guides do
   (`_BuildGuidePrim` :581-593: `xform = xform * assetRootWorld`, reset=true).
2. **A driven-Xform change reaches the hair prim only if the hair SI listens for `xform` dirties
   on the surface and re-dirties its own output prims** — nothing downstream flattens for it
   (same reason RigExec calls `_DirtySubtree`). Emitting hair prims as *children* of the surface
   prim does not buy inheritance either, for the same reason.
3. **Never publish a "local" matrix.** RigExec's first bug (`hydra-flattening-position.md` "1.")
   was exactly this and it was invisible on identity-ancestor test scenes; usdGen tests must
   place the asset with non-identity ancestors at two levels, as RigExec's probe does
   (`R/tests/probeImagingPipeline.cpp:216-248`, `R/examples/10_AimXformTurret.usda`).
4. Inside native/point instancing the "world" is prototype-common space and paths are propagated
   (`/…/__Prototype_N/…`); RigExec has this deferred (`imaging-datasource-redesign.md` §3.5,
   §6.2, using `HdPrimOriginSchema`). Hair on instanced surfaces inherits the same caveat.
5. `resetXformStack` downstream is not authored intent; a hair SI must not treat it as a reset
   boundary when walking ancestors (RigExec's first `_ComputeDrivenXform` broke on this,
   `hydra-flattening-position.md` "3.").
6. Test harness rule: build `HdFlatteningSceneIndex::New(upstream, HdFlattenedDataSourceProviders())`
   **upstream** of the SI under test, with non-null providers (`R/tests/testRigExecImaging.cpp:3094-3108`);
   and assert invalidation with a recording observer (`testRigExecImaging.cpp:77-101`) because
   pull-based checks cannot detect a missing dirty.

---

## 5. Guides / synthesized prims — the template for emitting curve prims that do not exist on the stage

RigExec synthesizes children under stage prims:

| Child name | Parent | Hydra prim type | Builder |
|---|---|---|---|
| `rigGuideSphere_<i>` / `rigGuideCone_<i>` | joint or aggregate solver | `sphere` / `cone` | `_BuildGuidePrim` `sceneIndices.cpp:545-636` |
| `rigGuideCtrl` | control | `basisCurves` (wire) / `mesh` / `sphere` / `cube` (geometry) | `_BuildControlGuidePrim` :902-1064, shape table :654-900 |
| `rigGuideVol_<i>` | volume weight | per element `basisCurves`/`mesh`/`sphere` | `_BuildVolumeGuidePrim` :1092-1215 |

Rules that a hair SI emitting `basisCurves` prims should copy:
- **Topology** — `GetChildPrimPaths` appends the synthesized names only if the parent exists
  upstream and the name is not already taken by an authored prim ("authored prims win",
  :1711-1771). `GetPrim` serves a synthesized prim only when the upstream has **no** dataSource
  at that path (:1552-1556, 1590-1593, 1626-1631).
- **Announcement history** — `_announcedGuides/_announcedControlGuides/_announcedVolumeGuides`
  record what was announced per parent; reconciled with exact `PrimsAdded/PrimsRemoved` in
  `_SyncGuideChildren` etc. (:1889-2063). A prim-type change is a `PrimsRemoved`+`PrimsAdded`
  (comment `sceneIndices.h:285-292`). History is maintained even while `!_IsObserved()`
  (:2178-2192, :2518-2535, :2568-2600) so late observers get correct removals.
- **Inherited state by hand** — `_AppendInheritedGuideState` copies the parent's `visibility`
  and `primOrigin` containers onto the child (:496-523; rationale: nothing upstream knows the
  child, and picking must resolve to a stage prim; mirrors `usdImaging/drawModeStandin.cpp`).
  For hair: copy `visibility`, `primOrigin`, and probably `purpose` and material binding from
  the surface unless the groom overrides them.
- **Purpose/render tag** — `HdPurposeSchema` set from the parent's resolved purpose
  (`_GuideRenderTag` :526-543; joints/controls are `guide`, which is why the usdview plugin
  forces `viewSettings.displayGuide = True`, `rigExecUsdview.py:582-589`).
- **Styling as primvars** — constant `displayColor`/`displayOpacity`, optional constant `widths`
  for curves, `points` vertex, `normals` (`_BuildGuideStylePrimvars` :407-494). Wire curves are
  linear, nonperiodic `basisCurves` with `curveVertexCounts` (`_BuildControlGuideShapes` :717-887).
- **Extent** — mesh/curve guides publish `HdExtentSchema` from their unit-shape bound
  (:1042-1050, :1190-1198); implicits do not (Storm's implicit-surface SI supplies geometry).
- **Placement** — asset-space frame × asset-root world (`_ResolveAssetRootWorld` :2134-2166,
  resolved against the *same* snapshot and through the driven-xform delta so a driven asset root
  carries its guides).
- **Pruning of internal prims** — `RigExecInternalPrimPruningSceneIndex` removes exactly the
  reserved scope `<rig>/__RigExecGenerated` (`bridge.cpp:659-662`; predicate `HasPrefix`
  :60-68) from `GetPrim`, `GetChildPrimPaths`, and all three notice kinds (:71-160);
  `SetOwnedScopes` emits `PrimsRemoved` for the old scopes (:45-58). Ownership comes from the
  compiler's reserved scope, never from a naming convention (`spec.md` §10.1).
- The redesign doc argues these should become **UsdImaging adapter subprims** upstream of
  flattening (`imaging-datasource-redesign.md` §3.3, §7.3-7.4), using
  `UsdImagingPrimAdapter::GetImagingSubprims/GetImagingSubprimType/GetImagingSubprimData`
  (as `UsdIrImagingJointScopeAdapter` does), which removes the hand-inheritance. That only works
  when the subprim *count* is derivable from authored data (§6.5 of that doc). For hair, whose
  curve prims per surface are usually a fixed small number (one `basisCurves` per groom node) but
  whose *content* depends on evaluation, the adapter-subprim route is viable for the prim shells
  and a filtering SI for the values — the hybrid the redesign doc itself lands on (§8.2).

Naming for usdGen: choose a reserved, parseable child-name scheme (RigExec uses string prefixes
`rigGuide…` with a strict digit parser :345-371) or, better, a reserved scope that the pruning
SI style predicate can own.

---

## 6. How extents/bounds are published

Two independent channels:

1. **Hydra** — `extent/min`, `extent/max` as `HdRetainedTypedSampledDataSource<GfVec3d>` in the
   strong root (`sceneIndices.cpp:1424-1433`) taken from the evaluator's `extent` moved property
   (`bridge.cpp:1194-1201`, a 2-element `VtVec3fArray`). Dirtied as `extent/min|max`
   (:2493-2499). The adapter delegate reads it in `GetExtent`
   (`U/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:708-716`). This is safe at the plugin slot
   because `UsdImagingExtentResolvingSceneIndex` runs upstream (`sceneIndices.cpp:232-234`), so
   a downstream overlay wins.
2. **USD side (framing, `UsdGeomBBoxCache`)** — a `UsdGeomComputeExtentFunction` registered for
   the codeless RigExec Boundable types via `TF_REGISTRY_FUNCTION(UsdGeomBoundable)` +
   `UsdGeomRegisterComputeExtentFunction(type, _ComputeRigExecGuideExtent)`
   (`registry.cpp:1131-1155`). The callback reads the **live snapshot only if
   `snapshot->Describes(prim.GetStage(), time)`** (stage object identity + sample time,
   `snapshotStore.h:266-281`, `registry.cpp:1077-1083`) else falls back to rest-pose bounds from
   authored attributes (:1085-1098). It exists because usdview's frame-selection goes through
   `UsdGeomBBoxCache`, which ignores Hydra entirely (`registry.h:158-174`). Also exposed to Python
   as `RigExecImaging_GetGuideBoundsAssetSpace` / `_GetAllGuideBoundsAssetSpace` (:1219-1298).

For hair: synthesized curve prims have no USD counterpart, so (2) applies verbatim — either
register a compute-extent function on the usdGen groom prim types (so "frame selected" frames
the hair) or accept that framing uses the surface's extent. For (1), publish `extent` on every
curve prim from the computed points (cheap relative to generation) and dirty `extent` alongside
`points`.

---

## 7. The C entry surface used by Python (ctypes) and why

`R/libs/rigExecImaging/registry.h:147-202` (exported with `RIGEXEC_IMAGING_C_API`, :130-145):

```c
int  RigExecImaging_Activate(long long stageCacheId, const char *rigPath, double initialFrame); // registry.cpp:1159-1195
int  RigExecImaging_SetTime(double frame);                                                      // :1197-1203
void RigExecImaging_Deactivate();                                                               // :1205-1209
long long RigExecImaging_GetGeneration();                                                       // :1211-1217
int  RigExecImaging_GetGuideBoundsAssetSpace(const char *primPath, double outMinMax[6]);        // :1219-1246
int  RigExecImaging_GetAllGuideBoundsAssetSpace(double outMinMax[6]);                           // :1256-1297
int  RigExecImaging_SetWeightOverlay(const char *weightPrimPath);                               // :1248-1254
```

Mechanics and rationale:
- The stage crosses the language boundary as a `UsdUtilsStageCache` id
  (`registry.cpp:1163-1166` `UsdUtilsStageCache::Get().Find(UsdStageCache::Id::FromLongInt(id))`);
  Python inserts it with `UsdUtils.StageCache.Get().Insert(stage).ToLongInt()`
  (`rigExecUsdview.py:571`). No boost-python/pybind module is needed, so the imaging library has
  no Python ABI dependency (`rigExecUsdview.py:7-8` "no custom Python bindings are required").
- Library located via `RIGEXEC_IMAGING_DLL` or a default name (`rigExecUsdview.py:20-60`);
  loaded with `ctypes.CDLL` and explicit `argtypes/restype` (:62-84).
- usdview `PluginContainer` (`plugin/rigExecUsdview/plugInfo.json`, type `python`) connects
  `dataModel.signalStageReplaced` and `dataModel.currentFrameChanged` (`rigExecUsdview.py:173-174`)
  — usdview loads containers **before** opening the stage (`U/usdImaging/usdviewq/appController.py:426-432`).
  On frame change it calls `RigExecImaging_SetTime(self._FrameValue(frame))` using the signal's
  frame, not `dataModel.currentFrame` (which is updated *after* the signal is emitted,
  `rootDataModel.py:152-160`) — a one-frame-stale trap documented at `rigExecUsdview.py:600-610`.
- Activation is transactional: compile + first evaluation happen on a candidate session list and
  the registry commits only on success (`registry.cpp:264-338`).
- The redesign doc plans to delete the whole C surface in favour of adapter-published
  `rigExec/session` and `rigExec/time` data sources (`imaging-datasource-redesign.md` §3.1-3.2,
  Phase 3), but that is not implemented in the tree (`sceneIndexPlugin.cpp` still uses the registry).

Hair-system relevance: the same pattern (usdview PluginContainer + ctypes + `UsdUtilsStageCache`)
is the only stock-usdview way to get a `UsdStage` and the timeline into a scene index without
Python bindings; if usdGen needs the stage (SeExpr texture prims referencing stage assets, freeze/
commit tools writing back), this is the proven transport. If it only needs Hydra data sources, a
renderer-level plugin can get the frame from the scene-globals prim (§2.2) instead.

---

## 8. How UsdSkel-skinned (or otherwise modified) points appear to a downstream scene index

`UsdSkelImagingResolvingSceneIndexPlugin::AppendSceneIndex` inserts
`UsdSkelImagingSkeletonResolvingSceneIndex` then `UsdSkelImagingPointsResolvingSceneIndex`
(`U/usdImaging/usdSkelImaging/resolvingSceneIndexPlugin.cpp:28-41`), registered in
`usdSkelImaging/plugInfo.json:42-47` (present in the install:
`/home/burkard/work/OpenUSD_26_08/lib/usd/usdSkelImaging/resources/plugInfo.json` contains
`UsdSkelImagingResolvingSceneIndexPlugin`). It is in the same unordered slot as RigExec (§2.1).

What a skinned `mesh|points|basisCurves` looks like downstream (`dataSourceResolvedPointsBasedPrim.cpp`):

| Mode | Env | `primvars/points` | Where skinning happens |
|---|---|---|---|
| default | `HD_ENABLE_DEFERRED_SKINNING=false` (`U/imaging/hd/skinningSettings.cpp:15-19`) | **blocked** (`HdBlockDataSource`) together with `normals` (:498-506, `_BlockPointsAndNormalsPrimvars` :288-303) | `extComputationPrimvars/points` → source computation `<mesh>/skinningPointsComputation`, output `skinnedPoints` (:225-247). Two child `extComputation` prims per mesh (`pointsResolvingSceneIndex.cpp:105-140`); GLSL kernel unless `USDSKELIMAGING_FORCE_CPU_COMPUTE=1` (`extComputations.cpp:36-37`, :541-545), CPU callback always attached (:457-505) |
| deferred | `HD_ENABLE_DEFERRED_SKINNING=true` | replaced by `_SkinningPrimvarsDataSource` (all skinning inputs as primvars) (:463-481) | in the renderer (Storm vertex shader) — no CPU points anywhere |

Consequences:
- A downstream SI **cannot read skinned points as a plain primvar** in either mode. It must
  (a) execute the ext computation itself — the inputs are exposed on the aggregator computation
  prim's `extComputation/inputValues` (`restPoints, geomBindXform, influences,
  numInfluencesPerComponent, hasConstantInfluences, blendShapeOffsets, blendShapeOffsetRanges,
  blendShapeWeights, skinningXforms, skelLocalToWorld, primWorldToLocal`;
  `dataSourceResolvedExtComputationPrim.cpp:80-160, 188-260`, tokens `usdSkelImaging/tokens.h:44-97`)
  and the kernel is public (`UsdSkelSkinPoints`, `UsdSkelSkinTransform` in `U/usd/usdSkel/utils.h`;
  the CPU path is `UsdSkelImagingInvokeExtComputation`, `extComputations.cpp:100-240`); or
  (b) publish hair as its own ext computation chained on `skinnedPoints` (renderer executes;
  Storm supports GPU ext computations, but CPU groom operators would then not have the points);
  or (c) require `USDSKELIMAGING_FORCE_CPU_COMPUTE=1` and still re-run the CPU callback because
  the SI sees a callback data source, not values.
- RigExec **replaces** `primvars/points` with a retained array (§3.2) — a hair SI downstream of
  RigExec reads deformed points directly with `HdPrimvarsSchema::GetFromParent(ds).GetPrimvar(points).GetPrimvarValue()->GetValue(0)`.
  RigExec blocks `velocities/accelerations` when it owns points (:1358-1363) — hair should do the
  same on its curve prims unless it computes them.
- Order between RigExec and UsdSkel on the same mesh is undefined; the redesign doc's "publish
  deltas" plan (§9 there) is not implemented (`_BuildStrongRoot` publishes absolute points).
  If RigExec runs after UsdSkel it *replaces* the block with real points (hair then works); if
  UsdSkel runs after RigExec, points are blocked and the ext computation skins rest points.
- `HdSkinningSettings::IsSkinningDeferred()` also changes `ProxyPathTranslationDataSourceNames`
  (`resolvingSceneIndexPlugin.cpp:61-81`).
- Other modifiers: `HdsiVelocityMotionResolvingSceneIndex` (Storm phase 0 `lastBefore`) and
  hdGp are renderer-level and after any UsdImaging plugin; an hdGp procedural's outputs are
  visible to a phase-≥3 `HdSceneIndexPlugin` ordered `after: hdGp:proceduralResolution`.

Practical rule for usdGen: read `primvars/points`; if it is null/blocked and
`extComputationPrimvars/points` exists, resolve the computation chain on CPU (option (a)) and
subscribe to dirties on the computation prims (`extComputation/inputValues/…`) as well as the mesh.
UsdSkel dirties `skinningXforms` on the computation prim when the skeleton animates
(`pointsResolvingSceneIndex.cpp` `_ProcessDirtyLocators`, header :77-81).

---

## 9. Recommendation and the invalidation contract usdGen must honour

**Placement.** Use **(b)**: one `HdSceneIndexPlugin` with `loadWithRenderer: ""`, C++
registration at insertion phase 1 `InsertionOrderAtStart`, JSON tags
`["usdGen:groomResolution"]`, `ordering: {after: ["hd:sceneGlobals"], before:
["hdGp:proceduralResolution", "hdSt:phase3"], position: "firstAfter"}` (edit tags to taste —
only tags that real entries carry create edges). This is the only stock mechanism that is
provably downstream of every UsdImaging deformer (RigExec, UsdSkel, any third-party
`UsdImagingSceneIndexPlugin`) and gives ordering keys against Storm/hdGp. Gate with
`_IsEnabled` on an env setting plus the presence of usdGen prims. Keep a secondary
`UsdImagingSceneIndexPlugin` **only** for the `InstanceDataSourceNames`/`FlattenedDataSourceProviders`
hooks if grooms must aggregate across native instances (it can be a pure metadata plugin that
returns `inputScene` unchanged). Do not rely on (d) unless usdRig adopts a hook; do not choose
(a) as the value path.

**Time.** Read `currentFrame` from the scene-globals prim (§2.2) or accept it from a usdview
container through a C API as RigExec does (§7). In `usdrecord` only the former exists.

**Invalidation contract (inputs).** The hair SI must handle, per surface prim it depends on:

| Incoming notice | Source | Required reaction |
|---|---|---|
| `PrimsDirtied primvars/points/primvarValue` (+`__containerDataSource` sentinels) | RigExec value change (:2466-2476, 2504) or `UsdImagingStageSceneIndex::SetTime` for authored time samples (`stageSceneIndex.cpp:357-371, 905-916`) | re-generate/deform curves; dirty output `primvars/points/primvarValue` + `extent` |
| `PrimsDirtied xform/matrix` on any prim (descendants included, sent individually) | RigExec driven Xform (:2444-2464), UsdImaging time-varying xforms | re-dirty output prims' `xform` (if hair carries the surface xform) — do not assume hierarchy |
| `PrimsDirtied UniversalSet()` | RigExec structural (:2207), epoch swap (:166-190), draw-mode/instancing changes | treat as resync of the surface: re-read type, topology, points |
| `PrimsAdded` for an **existing** path (type unchanged) | RigExec weight-overlay transition (:2236-2257); adapter delegate treats as all-dirty (`sceneIndexAdapterSceneDelegate.cpp:244-264, 300-310`) | same as universal dirty; do not re-create output prims unless their type changes |
| `PrimsRemoved` of a surface | stage edit / deactivation | remove dependent outputs with `PrimsRemoved`; drop bookkeeping by prefix (RigExec :2601-2640) |
| `primvars/<name>` dirty where the last element is not `primvarValue/indexedPrimvarValue/indices` | any | the adapter delegate drops its primvar descriptor cache (`sceneIndexAdapterSceneDelegate.cpp:511-522`); mirror that for cached descriptor decisions |
| `extComputationPrimvars` / `extComputation/inputValues/*` on `<mesh>/skinningPoints*Computation` | UsdSkel | re-skin on CPU (§8) |

**Invalidation contract (outputs).** Emit exactly what RigExec emits, in the same order
(`Removed`, `Added`, `Dirtied`): narrowest leaves expanded with
`HdContainerDataSourceEditor::ComputeDirtyLocators` whenever container handles are rebuilt per
generation; `UniversalSet` (or `PrimsRemoved`+`PrimsAdded`) when a prim's owned-leaf set or type
changes; explicit per-descendant xform dirties; never assume descendants are dirtied by a parent
notice. Block `velocities`/`accelerations` on curve prims when publishing points you own.
Send notices only from the serialized publish path, never from `GetPrim`, and keep announcement
history while unobserved.

**Spaces.** Output curve prims with `xform = surface world matrix, resetXformStack = true` and
points in surface-local space; re-dirty `xform` whenever the surface (or any driven ancestor)
xform is dirtied.

**Testing.** Copy the two RigExec harness rules (§4 item 6) and add a "plugin order" probe over
the real `UsdImagingCreateSceneIndices` chain with both usdSkelImaging and rigExecImaging loaded,
asserting that hair reads post-deformation points; make it loud if the order changes.

---

## Key facts

- The only public plugin slot inside UsdImaging is `_AddPluginSceneIndices` at `U/usdImaging/usdImaging/sceneIndices.cpp:301-302`, iterating `UsdImagingSceneIndexPlugin::GetAllSceneIndexPlugins()` (`sceneIndexPlugin.cpp:174-222`).
- That iteration is over `std::set<TfType>`, and `TfType::operator<` compares `_TypeInfo*` pointers (`U/base/tf/type.h:119`) allocated with `new` at declaration (`type.cpp:260`): plugin order in the slot is unspecified; no priority/tag/after exists in `sceneIndexPlugin.h:51-129`.
- The only `HdFlatteningSceneIndex::New` in the UsdImaging chain is inside `UsdImagingNiPrototypePropagatingSceneIndex` (`niPrototypePropagatingSceneIndex.cpp:204`) built at `sceneIndices.cpp:281-283`, before the plugin slot; flattened matrices are stamped `resetXformStack=true` (`hd/flattenedXformDataSourceProvider.cpp:118-135`).
- RigExec inserts `pruning -> binding -> results` via `RigExecUsdImagingSceneIndexPlugin::AppendSceneIndex` (`R/libs/rigExecImaging/sceneIndexPlugin.cpp:26-40`), declared in `R/plugin/rigExecImaging/resources/plugInfo.json.in:5-10` with `bases: ["UsdImagingSceneIndexPlugin"]` and a CMake-substituted `LibraryPath` (`R/CMakeLists.txt:492-518`).
- Renderer-level plugins are appended once by the engine at `U/usdImaging/usdImagingGL/engine.cpp:1776-1780` on the merging SI (after UsdImaging + task controller), before `HdCachingSceneIndex` and `CreateRenderer` (:1782-1793); the render index does not append again when given a terminal SI (`hd/renderIndex.cpp:172-175, 203-213`; `hd/renderDelegateAdapterRenderer.cpp:197-207`).
- `HdSceneIndexPluginRegistry` ordering: env `HD_SCENE_INDEX_PLUGIN_ORDERING_POLICY_DEFAULT="Hybrid"` (`hd/sceneIndexPluginRegistry.cpp:40-48`); JSON keys `loadWithRenderer/loadWithApps/tags/ordering{after,before,position}` (:53-65, :1251-1297, :1300-1356); topological sort with `(position, pluginId)` tie-break (:760-822); all-renderers entries precede same-phase renderer entries (:494-560).
- `hd:sceneAssembly` / `hd:sceneGlobals` tags are referenced by hdSt/hdGp plugInfo but defined nowhere in C++ (repo grep); unknown `after` tags are ignored (:606-613).
- hdGp's resolver is an all-renderers plugin at phase 2 (`hdGp/sceneIndexPlugin.h:29-37`) but disabled unless `HDGP_INCLUDE_DEFAULT_RESOLVER=1` (`hdGp/sceneIndexPlugin.cpp:25-27, 60-66`).
- Storm's display name is `"GL"` (`imaging/plugin/hdStorm/plugInfo.json:10`); its 13 scene index plugins are `loadWithRenderer: "GL"` at phases 0/1/3/4/100/900/1000 (`imaging/hdSt/plugInfo.json`).
- RigExec `GetPrim` is lock-free: one `std::atomic_load` of the snapshot (`snapshotStore.h:376-378`, `sceneIndices.cpp:1662`), overlays a retained strong root (`_BuildStrongRoot` :1322-1441) via `HdOverlayContainerDataSource::OverlayedContainerDataSources` (:1699-1707); it never evaluates (`spec.md` §10.3).
- Evaluate-then-publish is serialized in `RigExecImagingRegistry::SetTime` under a non-recursive mutex (`registry.cpp:342-377`), then `_Broadcast` calls `SetBindingEpoch` + `NotifyGenerationPublished` on every live chain (:497-516); generation diff is `RigExecSnapshotStore::Publish/_Diff` (`snapshotStore.h:326-372, 385-500`).
- Dirty locators: `primvars/points/primvarValue` + blocked `velocities/accelerations`, `primvars/normals/primvarValue`, `extent/min|max`, `xform/matrix` (+ every descendant), all expanded with `HdContainerDataSourceEditor::ComputeDirtyLocators` (`sceneIndices.cpp:2444-2505`); structural → `UniversalSet` (:2207) and, for primvar-set changes, a re-`PrimsAdded` (:2236-2257).
- Hydra dirtiness is per-prim only (`hd/sceneIndexObserver.h:76-79`) and `HdSceneIndexAdapterSceneDelegate::GetTransform` reads the prim's own matrix (`sceneIndexAdapterSceneDelegate.cpp:2478-2496`), so RigExec dirties subtrees by hand (`_DirtySubtree` :2100-2115).
- Driven transforms are published as `(xform, xformBase)` and resolved as a post-multiplied world delta (`sceneIndices.cpp:1479-1543`; `snapshotStore.h:94-105`), fail-closed on singular matrices (`_TryInvert` :1264-1305).
- Synthesized guide prims: names `rigGuideSphere_N/rigGuideCone_N/rigGuideCtrl/rigGuideVol_N`, served only where upstream has no prim (:1552-1660), announced/reconciled with exact add/remove (:1889-2063), inheriting `visibility`/`primOrigin` by hand (:496-523), placed with `xform * assetRootWorld` and `resetXformStack=true` (:581-593).
- Internal prims are pruned by exact reserved scope `<rig>/__RigExecGenerated` (`bridge.cpp:659-662`; `sceneIndices.cpp:60-160`).
- Bounds: Hydra `extent` overlay (:1424-1433) plus a `UsdGeomComputeExtentFunction` registered on codeless Boundable types that reads the snapshot only when `snapshot->Describes(stage, time)` (`registry.cpp:1077-1083, 1131-1155`; `snapshotStore.h:266-281`).
- C API: `RigExecImaging_Activate(stageCacheId, rigPath, frame)`, `_SetTime`, `_Deactivate`, `_GetGeneration`, `_GetGuideBoundsAssetSpace`, `_GetAllGuideBoundsAssetSpace`, `_SetWeightOverlay` (`registry.h:147-202`, `registry.cpp:1157-1298`); stage crosses via `UsdUtilsStageCache` id (:1163-1166; `rigExecUsdview.py:571-574`).
- The usdview plugin must use the signal's frame, not `dataModel.currentFrame`, because the model updates after emitting (`rootDataModel.py:152-160`; `rigExecUsdview.py:600-610`).
- UsdSkel 26.08 blocks `primvars/points` and `normals` on skinned prims and publishes `extComputationPrimvars/points` sourced from `<mesh>/skinningPointsComputation` (`usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:225-247, 288-303, 498-506`); GLSL kernel unless `USDSKELIMAGING_FORCE_CPU_COMPUTE=1` (`extComputations.cpp:36-37, 541-545`); `HD_ENABLE_DEFERRED_SKINNING` moves skinning to the renderer (`hd/skinningSettings.cpp:15-19`).
- Skinning inputs (`restPoints, influences, skinningXforms, geomBindXform, blendShape*, skelLocalToWorld, primWorldToLocal`) are readable on the computation prims' `extComputation/inputValues` (`dataSourceResolvedExtComputationPrim.cpp:80-160, 188-260`) and the CPU kernel is public (`UsdSkelSkinPoints`, `U/usd/usdSkel/utils.h`), so a downstream SI can re-skin on CPU.
- `UsdImagingGLEngine::Render` calls `_usdImagingSceneIndex->SetTime(params.frame)` and sets `currentFrame` on `HdsiSceneGlobalsSceneIndex` (`engine.cpp:484-498, 2062-2073`); `UsdImagingStageSceneIndex::SetTime` dirties only locators flagged time-varying (`stageSceneIndex.cpp:357-371, 882-916`).
- `usdrecord` builds the same engine (`U/usdImaging/usdAppUtils/frameRecorder.cpp:38-45`), so both plugin levels run there; `appName` is empty in both, so `loadWithApps` cannot target usdview (`engine.cpp:1778-1780`; `sceneIndexPluginRegistry.cpp:1133-1146`).
- RigExec's redesign (not implemented) would move stage/time/guides to UsdImaging adapters and publish points as deltas; today points are absolute replacements (`imaging-datasource-redesign.md` §0, §3, §9 vs `sceneIndices.cpp:1336-1357`).

## Open questions

- Actual observed order of `RigExecUsdImagingSceneIndexPlugin` vs `UsdSkelImagingResolvingSceneIndexPlugin` on this machine (a runtime probe over `UsdImagingCreateSceneIndices` with both plugin paths set would settle it; not run — usdRig has no `build/` directory here).
- Whether hdPrman's construction path (not in this tree) calls `AppendSceneIndicesForRenderer` with an `appName`, and whether it consumes all-renderers plugins before or after its own `HdPrman_*` phases — UNVERIFIED.
- Whether Storm honours `extent` on `basisCurves` for culling/framing in 26.08 (`hdSt/basisCurves.cpp` grep only shows `DirtyExtent` bit handling at :1336; no consumer inspected).
- Behaviour of the merging SI when a renderer-level plugin emits prims under the UsdImaging input's root vs. the task-controller root (activeInputSceneRoot semantics, `hd/mergingSceneIndex.h:20-46`) — irrelevant for a filtering SI but relevant if usdGen ever adds a second input scene.
- Native-instancing path translation for hair: RigExec defers it (`imaging-datasource-redesign.md` §3.5, §6.2); whether `HdPrimOriginSchema` is sufficient for a renderer-level SI to map propagated prototype paths back to groom prims was not verified.
- Cost of `HdContainerDataSourceEditor::ComputeDirtyLocators` per prim per frame for thousands of curve prims — RigExec computes it per published prim per generation (`sceneIndices.cpp:2504-2505`); no measurement exists in the tree.
- Whether `HdSceneIndexAdapterSceneDelegate` re-reads `extComputationPrimvars` descriptors on a `UniversalSet` dirty without a `PrimsAdded` (it clears `extCmpPrimvarDescriptors` on an `extComputationPrimvars` intersect, `sceneIndexAdapterSceneDelegate.cpp:524-528`; universal set intersects everything, so probably yes — UNVERIFIED by test).
