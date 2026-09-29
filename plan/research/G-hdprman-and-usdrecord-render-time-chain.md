# G — hdPrman and usdrecord: the render-time scene-index chain (OpenUSD 26.08)

Path abbreviations: `P/` = `<openusd-src>/third_party/renderman/plugin/hdPrman/`,
`U/` = `<openusd-src>/pxr/`. All line numbers are from the v26.08 tree on disk.

## 0. Scope and corrections to A2/A4

A2 (§ "hdPrman is not in this tree", A2:233-236, A2:717) and A4 (A4:315, A4:394) marked hdPrman
behaviour UNVERIFIED. It is in the tree under `third_party/renderman` (built only with
`PXR_BUILD_PRMAN_PLUGIN=ON`, default OFF — `<openusd-src>/CMakeLists.txt:47-52`,
`<openusd-src>/cmake/defaults/Options.cmake:27`). The local install
`$USD/plugin/usd/` contains only `hdStorm`, `hioAvif`, `hioOpenEXR`,
`sdrGlslfx`, `usdShaders` — **no hdPrman binary exists on this machine**, so everything below is
source-verified, not run-verified. The findings resolve the four questions posed:

| Question | Answer (details in the numbered sections) |
|---|---|
| Which `AppendSceneIndicesForRenderer` caller runs for hdPrman under usdview/usdrecord? | The engine path `U/usdImaging/usdImagingGL/engine.cpp:1776-1779`; the legacy `U/imaging/hd/renderIndex.cpp:209-214` path is skipped because the engine passes a terminal scene index (§1). |
| Does an all-renderers phase-1 plugin land before hdPrman's own phases? | It lands **after hdPrman phase 0** (ext-computation pruning, tet-mesh conversion) and **before every hdPrman phase ≥ 1** (§2). |
| What does hdPrman's motion-blur SI require from a hair `points` data source? | ≥2 contributing samples over `[shutterOpen·blurScale, shutterClose·blurScale]`, identical array size at every sample, ≤16 samples, `GetValue(t)` for arbitrary `t` (§3). |
| Do skinned scalps arrive as CPU-evaluated points? | Under hdPrman yes (phase-0 `HdSiExtComputationPrimvarPruningSceneIndex`); under Storm no — the hair SI must prune/evaluate itself (§5). |
| Is usdrecord's time source the scene-globals `currentFrame`? | Yes: `frameRecorder.cpp:460-474` → `engine.cpp:483-498` → `:2062-2073` (§8). |
| Does hdPrman's chain include an hdGp resolver? | No hdPrman-owned one; the universal `HdGpSceneIndexPlugin` sits at all-renderers phase 2 but is a no-op unless `HDGP_INCLUDE_DEFAULT_RESOLVER=1` (§7). |
| How does `HdPrman_DependencyForwarding` compare with Storm's? | Identical class (`HdDependencyForwardingSceneIndex`), identical phase 1000 `AtEnd`; only the upstream dependency-declaring SI differs (§6). |

## 1. Construction path: which caller runs for hdPrman

### 1.1 The renderer plugin and its display names

hdPrman is loaded through a thin loader plugin: `HdPrmanLoaderRendererPlugin` (`P/../hdPrmanLoader/rendererPlugin.h:24-83`) with `displayName: "RenderMan RIS"` (`P/../hdPrmanLoader/plugInfo.json:10`). Its constructor requires `$RMANTREE` (`rendererPlugin.cpp:108-112`), dlopens `libprman` globally (`:153-164`) and the versioned `hdPrman` library locally (`:174-193`), then resolves `HdPrmanLoaderCreateDelegate`/`DeleteDelegate` symbols (`:195-206`); `CreateRenderDelegate` forwards to that symbol (`:349-364`), implemented in `P/rendererPlugin.cpp:23-56` (only one Riley at a time: an existing delegate is `End()`ed first, `:25-33`). XPU variants are separate loader plugins with display names `"RenderMan XPU"`, `"RenderMan XPU - CPU"`, `"RenderMan XPU - GPU"` (`P/../hdPrmanXpu{,Cpu,Gpu}Loader/plugInfo.json:10`). Every `HdPrman_*` scene-index plugin registers once per name in `HdPrman_GetPluginDisplayNames()` (`P/tokens.cpp:21-30`) and lists the same four names under `loadWithRenderer` (`P/plugInfo.json:24` etc.).

Scene-index create args advertised by the loader (`rendererPlugin.cpp:266-289`): `motionBlurSupport=true`, `cameraMotionBlurSupport=true`, plus `legacyRenderDelegateInfo` with `materialBindingPurpose=full`, `materialRenderContexts={ri, mtlx}`, `renderSettingsNamespaces={ri, outputs:ri}`, `isPrimvarFilteringNeeded=false` (`:222-238`), `shaderSourceTypes={OSL, RmanCpp, mtlx}` (`:241-257`). Note: a grep over `U/` finds **no consumer** of `GetMotionBlurSupport()` outside the schema itself, so in 26.08 UsdImaging emits time samples regardless of renderer (UNVERIFIED whether a downstream Pixar-internal consumer exists).

### 1.2 The two `AppendSceneIndicesForRenderer` callers

| Caller | Code | When it runs | rendererDisplayName source | appName |
|---|---|---|---|---|
| Engine | `U/usdImaging/usdImagingGL/engine.cpp:1749-1794` (`_CreateSceneIndexChainAndRenderer`): `HdMergingSceneIndex` → `AppendSceneIndicesForRenderer(rendererDisplayName, sceneIndex, renderInstanceId)` (`:1776-1779`) → optional `HdCachingSceneIndex` (`:1782-1786`, env `USDIMAGINGGL_ENGINE_ENABLE_CACHING_SCENE_INDEX`, `:86-88`) → `plugin->CreateRenderer(_terminalSceneIndex, args)` (`:1791-1793`) | usdview, usdrecord, any `UsdImagingGLEngine` host | `plugin->GetDisplayName()` (`:1762`) = plugInfo `displayName` (`U/imaging/hd/rendererPlugin.cpp:110-126`) | empty (default, `sceneIndexPluginRegistry.h:83-87`) |
| Legacy render index | `U/imaging/hd/renderIndex.cpp:176-221`: emulation SI + merging SI + `AppendSceneIndicesForRenderer(rendererDisplayName, sceneIndex, instanceName, appName)` (`:205-214`) | Only when `HdRenderIndex::New(delegate, drivers, instanceName, appName)` (`:249-266`) is called **without** a terminal SI — i.e. DCC hosts that build their own render index | `renderDelegate->GetRendererDisplayName()` (`:205-206`), set by `HdRendererPlugin::CreateDelegate` (`rendererPlugin.cpp:74-78`) | host-provided |

The engine's `CreateRenderer` chain is: `HdRendererPlugin::CreateRenderer` (`rendererPlugin.cpp:84-102`) → `_CreateRenderer` → `_CreateRendererFromRenderDelegate` (`:134-157`, calls `CreateDelegate()` then builds `HdRenderDelegateAdapterRenderer`) → `HdRenderDelegateAdapterRenderer` ctor (`U/imaging/hd/renderDelegateAdapterRenderer.cpp:197-215`) → `HdRenderIndex::NewForBackendEmulation(delegate, drivers, terminalSceneIndex)` (`renderIndex.cpp:268-289`, refuses a null terminal SI `:279-283`) → ctor takes the `if (terminalSceneIndex)` branch (`:172-175`), so the legacy block including line 211 **never executes**; it then wraps the terminal SI in `HdSceneIndexAdapterSceneDelegate` (`:223-228`) and calls `renderDelegate->SetTerminalSceneIndex` (`:229`), which for hdPrman instantiates its `_RileySceneIndices` observer (`P/renderDelegate.cpp:976-989`, compiled whenever `HDPRMAN_USE_SCENE_INDEX_OBSERVER` is defined, which is unconditional for HDSI_API_VERSION ≥ 12 on Linux — `P/sceneIndexObserverApi.h:24-26`; `HDSI_API_VERSION` is 20 in 26.08, `U/imaging/hdsi/version.h:23`).

**Consequence for UsdGen:** one binary, one registration (`loadWithRenderer: ""` + C++ `RegisterSceneIndexForRenderer(allRenderers, …)`), is instantiated identically under Storm ("GL"), all four RenderMan names, and any DCC that uses the legacy path — because both callers funnel into the same `HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer` (`U/imaging/hd/sceneIndexPluginRegistry.cpp:1421-1475`). The only observable difference is `appName` (`loadWithApps` filtering, `:1133-1144`), which is empty under usdview/usdrecord.

## 2. hdPrman's plugin set and the resolved order

### 2.1 Registration inventory

All entries below are registered per RenderMan display name via `TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)`; the JSON `tags`/`ordering` come from `P/plugInfo.json` (which itself documents the phase/order in comments, `:6-16`).

| Plugin (`HdPrman_…`) | Phase / order (C++) | JSON tags → ordering | Scene index instantiated | Evidence |
|---|---|---|---|---|
| ExtComputationPrimvarPruning | 0 / AtStart | `hdPrman:phase0, hdPrman:extComp` → after `hd:sceneAssembly`, firstAfter | `HdSiExtComputationPrimvarPruningSceneIndex` | `P/extComputationPrimvarPruningSceneIndexPlugin.cpp:37-50,59-70`; header `:28-35`; json `:168-180` |
| TetMeshConversion | 0 / AtStart | `hdPrman:phase0` → after `hd:sceneAssembly` | tet→tri mesh | json `:247-259` |
| RenderPassPrune | 1 / AtStart | `hdPrman:phase1` → after `phase0, hd:sceneAssembly, hd:sceneGlobals`; **before `hdGp:proceduralResolution`** | render-pass prune | json `:181-194` |
| RenderSettingsFiltering | 1 / AtStart | `hdPrman:phase1` → after `phase0, hd:sceneGlobals` | | json `:208-220` |
| ParticleFieldConversion | 1 / AtStart | `hdPrman:phase1` → before `phase2, matFilt, matResolve` | | json `:327-339` |
| PreviewSurfacePrimvars | 2 / AtStart | `hdPrman:phase2` → after `phase1, hdGp:proceduralResolution`; before `matPrimvarTransfer` | | json `:287-300` |
| VelocityMotionResolving | 2 / AtEnd | `hdPrman:phase2` → after `phase1, hdGp:proceduralResolution, hdPrman:extComp`; before `phase3, motionBlur` | `_VblurInterpretingSceneIndex` + `HdsiVelocityMotionResolvingSceneIndex` | `P/velocityMotionResolvingSceneIndexPlugin.cpp:1300-1349`; header `:28-34`; json `:48-61` |
| ImplicitSurface | 3 / AtStart | `hdPrman:phase3, implicitSurfaces` → after `hd:sceneAssembly, hdGp:proceduralResolution` | | `P/implicitSurfaceSceneIndexPlugin.cpp:39-47`; json `:88-102` |
| MotionBlur | 3 / AtStart | `hdPrman:phase3, hdPrman:motionBlur` → after `phase2`, firstAfter | `_HdPrmanMotionBlurSceneIndex` | `P/motionBlurSceneIndexPlugin.cpp:1107-1118,1141-1147`; header `:41-49`; json `:35-47` |
| PinnedCurveExpanding | 3 / AtEnd | `hdPrman:phase3` → after `phase2, hdGp:proceduralResolution, extComp`, lastBefore | `HdsiPinnedCurveExpandingSceneIndex` | `P/pinnedCurveExpandingSceneIndexPlugin.cpp:30-50,59-66`; json `:155-167` |
| LightLinking | 4 / AtStart | `hdPrman:phase4, lightLinking` → after `phase3` | | json `:274-286` |
| RenderingColorSpace, DisplayColorSpace | 5 / AtStart | `hdPrman:colorSpace` → after `phase4` | | `P/renderingColorSpaceSceneIndexPlugin.cpp:1099-1107`, `P/displayColorSpaceSceneIndexPlugin.cpp:703-711`; json `:340-375` |
| MatFilt | 100 / AtStart | `hdPrman:phase100, matFilt, matResolve` → after `phase4` | material filtering chain | `P/matfiltSceneIndexPlugins.cpp:58-63`; json `:62-74` |
| RenderPassVisibilityAndMatte | 113 / AtStart | after `phase100, matFilt` | | json `:195-207` |
| Id, MeshLightResolving, PortalLightResolving, UpdateObjectSettings | 115 / AtStart | after `phase113, matFilt` (Id also after `implicitSurfaces`) | | json `:75-87,234-246,301-326` |
| MaterialPrimvarTransfer | max(3,200)=200 / AtEnd | `hdPrman:phase200, matPrimvarTransfer` → after `phase115, hdGp:proceduralResolution, matFilt` | | `P/materialPrimvarTransferSceneIndexPlugin.cpp:43-60`; json `:142-154` |
| Dependency (declares deps) | 900 / AtStart | `hdPrman:phase900, renderDelegateDependencies` → after `phase200, matPrimvarTransfer` | adds `__dependencies` for volume→volumeField, light→light filters, camera→projection | `P/dependencySceneIndexPlugin.cpp:63-81,92-150,217-271,304-335,492-552` |
| CoordSysPrim | 900 / AtEnd | `hdPrman:phase900` → after `phase200, matPrimvarTransfer` | | json `:221-233` |
| Renderer2 | 999 / AtEnd | `hdPrman:phase999, renderer2` → after `phase900`; before `dependencyForwarding` | `RileyGlobalsSceneIndex` always; `RileyFallbackMaterial`+`RileyConversionSceneIndex` only with `HD_PRMAN_EXPERIMENTAL_RILEY_SCENE_INDEX_OBSERVER=1` | `P/renderer2SceneIndexPlugin.cpp:33-68,81-95`; env default false `P/sceneIndexObserverApi.cpp:15-22` |
| WorldOffset | 999 / AtEnd | `hdPrman:phase999` → after `phase900` | | json `:22-34` |
| DependencyForwarding | 1000 / AtEnd | `hdPrman:phase1000, dependencyForwarding` → after `phase999, renderDelegateDependencies`, lastBefore | `HdDependencyForwardingSceneIndex` | `P/dependencyForwardingSceneIndexPlugin.cpp:29-38,44-50`; header `:29-37` |
| RenderTerminalOutputInvalidating | 1000 / AtEnd | `hdPrman:phase1000` → after `phase999` | | json `:129-141` |

Compile-time guards: `P/sceneIndexPluginOrdering.cpp:17-27` static-asserts `extComp(0) < velocity(2) < motionBlur(3)`.

### 2.2 How the registry orders an all-renderers plugin relative to these (Hybrid policy)

Default policy is `Hybrid` (`HD_SCENE_INDEX_PLUGIN_ORDERING_POLICY_DEFAULT`, `sceneIndexPluginRegistry.cpp:40-48`; enum `.h:225-239`). Mechanics that matter for UsdGen:

1. `_LoadPluginsForRenderer` preloads libraries whose `loadWithRenderer` contains `""` or the display name (`:1358-1402`), so a `loadWithRenderer: ""` UsdGen library is loaded under every RenderMan name.
2. Registration entries are turned into manufactured tags `phaseN` with an `after: phase<prev>` edge (`:914-958`); then the "all-renderers first within a phase" rule is enforced by renaming renderer-specific tags to `phaseN_` and adding `after: phaseN` (`:966-1005`). So for the same phase, all-renderers entries precede renderer-specific ones; across phases, numeric order holds.
3. JSON entries (one per `loadWithRenderer` name, `:1300-1356`) are merged with the matching registration entry by plugin id (`:830-912`). **A plugin-id registration without a JSON entry is dropped under Hybrid** (only callback entries survive the merge, `:862-869`), so `loadWithRenderer` is mandatory (also stated in `U/imaging/hd/sceneIndexPlugin.h:35-39`).
4. Kahn topological sort with `(position, pluginId)` tie-break; unconnected "islands" go last (`:732-822`); cycles are broken by isolating one plugin with a `TF_WARN` (`:639-730`). Debug: `HD_SCENE_INDEX_PLUGIN_ORDERING`, `HD_SCENE_INDEX_PLUGIN_REGISTRY`.
5. `IsEnabled` is consulted per append; a disabled plugin returns the input scene (`:1196-1199`).

Applying this to the RenderMan display names with the engine's callback at all-renderers phase 0 (`engine.cpp:183-202`: `HdsiSceneGlobalsSceneIndex` → `HdsiDomeLightCameraVisibilitySceneIndex` → `HdsiSceneMaterialPruningSceneIndex`, `:148-166`), `HdGpSceneIndexPlugin` at all-renderers phase 2 (`U/imaging/hdGp/sceneIndexPlugin.h:30-37`, `.cpp:29-40`), and a hypothetical **UsdGen plugin at all-renderers phase 1 `AtStart`** (A2's recommendation), the resolved chain is:

```
merging SI
 → [all,0]   engine callback: sceneGlobals, domeLightCamVis, sceneMaterialPruning
 → [RIS,0]   HdPrman_ExtComputationPrimvarPruning, HdPrman_TetMeshConversion
 → [all,1]   UsdGenSceneIndexPlugin                      <-- sees pruned (CPU-skinned) points
 → [RIS,1]   ParticleFieldConversion, RenderPassPrune, RenderSettingsFiltering
 → [all,2]   HdGpSceneIndexPlugin (no-op unless HDGP_INCLUDE_DEFAULT_RESOLVER=1)
 → [RIS,2]   PreviewSurfacePrimvars (AtStart) … VelocityMotionResolving (AtEnd)
 → [RIS,3]   ImplicitSurface, MotionBlur (AtStart) … PinnedCurveExpanding (AtEnd)
 → [RIS,4]   LightLinking
 → [RIS,5]   RenderingColorSpace, DisplayColorSpace
 → [RIS,100] MatFilt → [113] RenderPassVisibilityAndMatte → [115] Id, MeshLight, PortalLight, UpdateObjectSettings
 → [RIS,200] MaterialPrimvarTransfer
 → [RIS,900] Dependency (AtStart) … CoordSysPrim (AtEnd)
 → [RIS,999] Renderer2, WorldOffset
 → [RIS,1000] DependencyForwarding, RenderTerminalOutputInvalidating
 → (optional HdCachingSceneIndex) → HdSceneIndexAdapterSceneDelegate → HdPrman rprims
```

Rules that fall out of this:

- **Phase 0 all-renderers would be too early**: it would precede hdPrman's ext-computation pruning (all-renderers entries come first within a phase), so skinned scalps would still be `extComputationPrimvars`. Phase ≥ 1 is required to inherit hdPrman's pruning; phase 1 keeps UsdGen ahead of velocity/motion-blur/pinned (phases 2-3), which is what a generator of curve geometry needs (those three then post-process the generated curves).
- JSON `ordering.after: ["hdPrman:extComp"]` and `before: ["hdPrman:motionBlur", "hdGp:proceduralResolution"]` on the UsdGen entry make the intent explicit and survive a future renumbering of hdPrman's phases; unknown tags are ignored (`:606-615`), so the same JSON is harmless under Storm.
- The registry never re-runs: the chain is built once per `UsdImagingGLEngine` renderer creation (`engine.cpp:1493`), so changing `HDGP_INCLUDE_DEFAULT_RESOLVER` or ordering policy requires a restart.

## 3. Motion blur: what hdPrman requires from a hair `points` data source

### 3.1 Shutter plumbing (why the interval you are given is not the one used)

`HdPrman_RenderParam::_UpdateShutterInterval` stores `Ri:Shutter` and pushes it into a **static** pair inside the motion-blur plugin via `HdPrman_MotionBlurSceneIndexPlugin::SetShutterInterval` (`P/renderParam.cpp:5098-5110`; `P/motionBlurSceneIndexPlugin.cpp:1123-1136`, statics `:64-68`). Defaults are `0,0` (`P/renderParam.h:72-73`, `:254`) = blur disabled until the render camera syncs (`P/renderParam.cpp:4514-4545`, legacy setting `disableMotionBlur`). The plugin's `_GetContributingSampleTimesForInterval` **ignores the interval passed by the caller and uses the static shutter** (`:247-250` comment; `:328-330`). Rprims then pull points with `SamplePrimvar(id, points, shutterOpen, shutterClose, &boxedPoints)` (`P/renderParam.cpp:367-377`, via `HdPrman_ConvertPointsPrimvar` `:444-455`, called from `P/basisCurves.cpp:129-135`), and other primvars likewise (`:813-819`).

### 3.2 The per-primvar wrapper contract

Every prim with an `xform` schema (except `renderSettings`/`integrator`, `:1001-1020`) gets `_PrimDataSource` (`:898-931`), which wraps `primvars` (`_PrimvarsDataSource` `:741-764` — **all** primvars, so non-blurable ones are forced to a single sample) and `xform.matrix` (`:820-845`). Each `primvars.<name>.primvarValue` becomes `_MotionBlurTypedSampledDataSource<T>` via `HdCopySampledDataSourceType` (`:656-688`). Its `GetContributingSampleTimesForInterval` (`:246-414`) runs these checks in order, returning `false` (→ single sample at 0 in the adapter) on the first failure:

| # | Check | Lines | Default / knob |
|---|---|---|---|
| 1 | source non-null | `:257-263` | |
| 2 | shutter not `(0,0)` and `close-open ≥ 1e-10` | `:268-287` | `SetShutterInterval`; `_minimumShutterInterval` `:62` |
| 3 | key is `xform.matrix` or a blurable primvar: `points`, `instanceTranslations/Rotations/Scales/Transforms` | `:219-244`, `:289-295` | **only `points` on rprims** — `widths`, `normals`, any custom primvar never blur |
| 4 | `ri:object:mblur` primvar `[true]` | `:206-217`, `:297-303` | default true `:60` |
| 5 | `blurScale` primvar ≠ 0 | `:195-204`, `:305-312` | default 1.0 `:57` |
| 6 | `ri:object:geosamples` (points) / `ri:object:xformsamples` unauthored (-1) or ≥ 2 | `:177-193`, `:314-326` | default -1 `:58-59` |
| 7 | underlying `GetContributingSampleTimesForInterval(open·blurScale, close·blurScale)` returns true with ≥ 2 samples | `:328-351` | |
| 8 | **ordinality**: `GetValue(t).GetArraySize()` identical at every returned sample time | `:353-363` | this pulls the full value at every sample just to compare sizes |
| 9 | if `geosamples` authored and ≠ returned count, resample uniformly between first/last returned time (may miss authored samples) | `:365-389` | |
| 10 | divide times by `blurScale` | `:391-397` | |

`GetValue(t)` forwards to the source at `t·blurScale` (`:416-436`). Dirtying: changes under `primvars.{ri:object:mblur, ri:object:geosamples, ri:object:xformsamples, blurScale}.primvarValue` invalidate all `primvars` / `xform` of that prim (`:1032-1095`); hdPrman refreshes all primvars on any primvar dirty anyway (`:1041-1043`).

### 3.3 Sample-count cap in the adapter

`HdSceneIndexAdapterSceneDelegate::_SamplePrimvar` (`U/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:2369-2470`): calls `GetContributingSampleTimesForInterval(start, end)`; `false` → `times={0}` (`:2449-2451`); `true` with empty times → coding error and `{0}` (`:2440-2448`); **more than `maxSampleCount` → truncated, not resampled** (`:2454-2457`); then `GetValue(times[i])` per time (`:2459-2461`). hdPrman's capacity is `HDPRMAN_MAX_TIME_SAMPLES = 16` for prman ≥ 26, else 4 (`P/renderParam.h:60-70`). Riley receives `SetTimes(n)` + one `P` detail per sample (`P/renderParam.cpp:433-439`); samples whose size differs from the first are skipped with a warning (`:410-431`).

### 3.4 Velocity-based motion (runs before motion blur)

`HdPrman_VelocityMotionResolvingSceneIndexPlugin` (phase 2 AtEnd) appends `_VblurInterpretingSceneIndex` (maps `primvars:ri:object:vblur` values "No Velocity Blur"/"Velocity Blur"/"Acceleration Blur" to `__velocityMotionMode` `ignore`/`noAcceleration`/`enable`, `P/velocityMotionResolvingSceneIndexPlugin.cpp:67-74,1160-1184,1200-1234`) then `HdsiVelocityMotionResolvingSceneIndex` (`:1324-1349`). The hdsi class (`U/imaging/hdsi/velocityMotionResolvingSceneIndex.h:43-93`) applies to `points, basisCurves, nurbsCurves, nurbsPatch, tetMesh, mesh, instancer` (`.cpp:776-788`). For `points`, if `primvars:velocities` exists and is **valid** — same frame-relative left-bracketing sample time as `points` (compared via `GetContributingSampleTimesForInterval(0,0)`, `:386-410`), `VtVec3fArray`, at least as many entries as points (`:411-425`) — it **replaces** the source's sample times with `{start, end}` (plus `max(3, nonlinearSampleCount)-1` intermediates when `accelerations` are present and mode is `enable`, `:177-190`; `nonlinearSampleCount` default 3, `:457-471`) and returns `p + v·Δt/fps (+ a·Δt²/2)` from `GetValue` (`:205-270`). `fps` comes from `HdSceneGlobalsSchema.timeCodesPerSecond` on the root prim, else a **hard-coded 24** (`:273-287`, `:58`); the hdsi ctor ignores `inputArgs` (`:760-773`), so hdPrman's `fps` inputArg (`P/…Plugin.cpp:1339-1346`, `SetFPS` `:1314-1319`, never called elsewhere in hdPrman) is dead code, and nothing in `usdImagingGL`, `usdviewq`, `usdAppUtils` or `usdImaging` calls `HdsiSceneGlobalsSceneIndex::SetTimeCodesPerSecond` (`U/imaging/hdsi/sceneGlobalsSceneIndex.h:62-65`; grep empty). Net: 24 fps unless the plan sets it. Storm runs the same hdsi class at phase 0 AtEnd (`U/imaging/hdSt/plugInfo.json:176-185`).

### 3.5 Rules for the UsdGen curve data source under hdPrman

1. Implement `GetContributingSampleTimesForInterval(start,end)` on `primvars:points:primvarValue` honestly: return `true` and 2..16 times (≤ 16 or the tail is silently dropped) whose union covers `[start,end]`; return `false` when the groom is static (then a single `GetValue(0)`). The cheapest correct implementation is `HdGetMergedContributingSampleTimesForInterval` over the scalp `points` source and any animated operator inputs (`U/imaging/hd/dataSource.h:386-394`), which is exactly what `HdSiExtComputationPrimvarPruningSceneIndex` does for computed primvars (`U/imaging/hdsi/extComputationPrimvarPruningSceneIndex.cpp:212-236`).
2. Guarantee a constant CV count across the shutter (check #8): topology-changing operators (density by time, culling) must evaluate their topology decision once per frame, not per shutter sample.
3. `GetValue(t)` will be called at *every* returned time, twice (once for the ordinality check, once for the real pull), and at arbitrary `t` when `geosamples` or `blurScale` are authored. Cache per `(prim, t)` inside the SI; do not recompute the whole groom per call.
4. Do **not** emit a `velocities` primvar unless it is consistent with the points at the same sample time; otherwise the velocity SI silently overrides the deformation samples. Emitting none is simplest ("Fewer velocities"/"No velocities" → defer to source, `:364-370`).
5. Only `points` blurs; `widths`/`normals`/`displayColor` are sampled once at offset 0 (check #3). Width animation is therefore invisible to blur under hdPrman.
6. Blur is disabled entirely in interactive hdPrman until the camera syncs (defaults `0,0`); a hair SI must not assume a non-empty interval.

## 4. Pinned curves under hdPrman

hdPrman's Riley conversion (`P/basisCurves.cpp:45-149`) knows only `periodic` vs `nonperiodic` (`:118-122`); wrap `pinned` is treated as `nonperiodic` and segment counts are `(n-4)/vstep+1` (`:72-80`), so unexpanded pinned curves would lose their end segments. Accepted bases: `cubic`, `bSpline`, `bezier`, `catmullRom` (`:98-111`); **`centripetalCatmullRom` hits `TF_CODING_ERROR("Unknown curveBasis")` and the prim is skipped** (`:108-110`; no "centripetal" anywhere in hdPrman). Linear curves are supported (`:81-86, :112-113`).

`HdPrman_PinnedCurveExpandingSceneIndexPlugin` (phase 3 AtEnd, after motion blur) wraps every `basisCurves` prim in `HdsiPinnedCurveExpandingSceneIndex` (`U/imaging/hdsi/pinnedCurveExpandingSceneIndex.cpp:760-771`). When `wrap == pinned` and basis ∈ {bspline, catmullRom, centripetalCatmullRom} (`:627-635`): `curveVertexCounts` grow by `2·numExtraEnds` (2 per end for bspline, 1 for catmullRom, `:639-640`), `wrap` is rewritten to `nonperiodic` (`:506-509`), `curveIndices` are expanded if authored (`:486-499`), vertex primvars are expanded by replicating end values unless `curveIndices` exist (`:303-311`), varying primvars are expanded to match the new segment count (`:312-345`), and geomSubset indices on child subsets are remapped (`:774-794`). The expanding data source forwards `GetContributingSampleTimesForInterval` to the input and expands each `GetValue` (`:162-189`), i.e. one extra copy of every vertex primvar per shutter sample. It caches `curveVertexCounts` sampled at time 0 per `GetPrim` and has an explicit TODO that time-varying topology is unsupported (`:32`, `:644-646`).

**Compatibility with A5:** A5 recommends `basis=catmullRom` (or bspline) with `wrap=pinned` for Storm (A5:141-142). That is compatible with hdPrman via this plugin at the cost of a per-sample expansion copy; a UsdGen "render" mode that emits `nonperiodic` with duplicated end CVs itself would avoid the copy under hdPrman but lose Storm's native pinned index path. Avoid `centripetalCatmullRom` entirely (hdPrman rejects it). If `curveIndices` are authored, only the index buffer is expanded (`:657-668`) — a cheap way to make expansion O(curves) instead of O(CVs).

## 5. Skinned scalps: CPU-evaluated under hdPrman, not under Storm

UsdSkel imaging in 26.08 is scene-index based: `UsdSkelImagingResolvingSceneIndexPlugin` appends `UsdSkelImagingSkeletonResolvingSceneIndex` then `UsdSkelImagingPointsResolvingSceneIndex` inside `UsdImagingSceneIndex` (`U/usdImaging/usdSkelImaging/resolvingSceneIndexPlugin.cpp:28-41`, plugInfo `:42-47`). For a mesh/basisCurves/points prim under a SkelRoot bound to a Skeleton, `UsdSkelImagingDataSourceResolvedPointsBasedPrim` **replaces `primvars:points` with an `extComputationPrimvars:points` entry and blocks the authored points** (`dataSourceResolvedPointsBasedPrim.h:28-34`; `.cpp:487-505`), unless `HD_ENABLE_DEFERRED_SKINNING=1` (default false, `U/imaging/hd/skinningSettings.cpp:15-35`; `HasExtComputations()` `.cpp:1085-1107`). The generated `extComputation` prims carry **both** a GLSL kernel and a CPU callback (`dataSourceResolvedExtComputationPrim.cpp:443-449`); `USDSKELIMAGING_FORCE_CPU_COMPUTE=1` only nulls the GLSL kernel (`extComputations.cpp:538-547`). The CPU callback is `UsdSkelImagingExtComputationCpuCallback(skinningMethod)` and the exported invoker is `UsdSkelImagingInvokeExtComputation(skinningMethod, ctx)` (`extComputations.h:23-33`).

Under hdPrman, `HdPrman_ExtComputationPrimvarPruningSceneIndexPlugin` (phase 0) inserts `HdSiExtComputationPrimvarPruningSceneIndex`, which for mesh/basisCurves/points (`U/imaging/hdsi/extComputationPrimvarPruningSceneIndex.cpp:760-776`) merges `extComputationPrimvars` into `primvars` (`:604-633`), hides `extComputationPrimvars` (`:724-728`), executes the CPU-callback network lazily on every `GetValue` (`:243-…`, callback required at `:383-398` — a GPU-only computation is warned and skipped), derives sample times from all computation `inputValues` (`:212-236`), and rewrites dirty locators `extComputationPrimvars` → `primvars` (`:802-825`). There is no cache: each pull re-runs skinning (`_ExecuteComputationNetwork` builds a fresh value store). hdPrman's own rprim path only consults ext computations for non-`points` primvars in ≤ 23.11 builds (`P/renderParam.cpp:358-365`, `:509-529` excludes `points`), confirming it relies on the pruning SI for skinned points.

Storm has **no** pruning plugin (grep of `hdSt/`, `hdx/`, `usdImagingGL/`, `usdImaging/` for `ExtComputationPrimvarPruning` is empty); Storm executes ext computations natively at sync time. Therefore a UsdGen SI at all-renderers phase 1 sees:

| Renderer | Scalp `primvars:points` seen by UsdGen | Action needed |
|---|---|---|
| RenderMan (any variant) | authored-looking, CPU-skinned, multi-sampled via merged input sample times | none; but cache — every pull re-skins |
| Storm | **absent** (blocked); `extComputationPrimvars:points` present | wrap the SI's input in `HdSiExtComputationPrimvarPruningSceneIndex::New(input)` (public `HDSI_API`, `.h:35-41`) — it is a pass-through when no computed primvars exist (`.cpp:701-722`), so double-wrapping under hdPrman is harmless — or call `UsdSkelImagingInvokeExtComputation` directly |

The same applies to any other ext-computation-producing modifier; RigExec (A2) publishes plain primvars, so it is unaffected.

## 6. Dependency forwarding: hdPrman vs Storm

| | hdPrman | Storm |
|---|---|---|
| Forwarding SI | `HdDependencyForwardingSceneIndex::New(input)` (`P/dependencyForwardingSceneIndexPlugin.cpp:44-50`) | same class (`U/imaging/hdSt/dependencyForwardingSceneIndexPlugin.cpp:42-46`) |
| Phase / order | 1000 / AtEnd (`:29-38`; header `:29-37`), tags `hdPrman:phase1000, hdPrman:dependencyForwarding` | 1000 / AtEnd, tags `hdSt:phase1000, hdSt:dependencyForwarding` (`hdSt/plugInfo.json:31-43`) |
| Dependency-declaring SI upstream | `HdPrman_DependencySceneIndexPlugin` phase 900 AtStart: volume→volumeField, light→`__dependenciesToFilters`, camera→`__dependenciesToProjection` (`P/dependencySceneIndexPlugin.cpp:63-81,92-150,217-271,304-335,492-552`) | `HdSt_DependencySceneIndexPlugin` phase 100 AtStart: materialBindings→material, volumeFieldBinding (`hdSt/dependencySceneIndexPlugin.cpp:44-57,69-120`) |
| Also at phase 1000 | `RenderTerminalOutputInvalidating` (sorted after forwarding by plugin-id tie-break) | — |

So `__dependencies` authored by a UsdGen SI (e.g. hair prim → scalp `primvars/points`, hair prim → its operator prims) are fanned out identically in both renderers because the forwarding SI is the terminal filter in both chains and UsdGen sits far upstream. The forwarding SI stores per-locator pairs and is documented as potentially slow for large fan-out (`U/imaging/hd/dependencyForwardingSceneIndex.h:72-80`); keep dependency counts per hair prim small (one per upstream prim, coarse locators).

## 7. hdGp in hdPrman's chain

`CHANGELOG.md:2188-2191` is confirmed by source: hdPrman contains no resolver of its own (only ordering comments and a CMake link to `hdGp`, `P/CMakeLists.txt:81`). Five hdPrman entries order themselves `after: hdGp:proceduralResolution` and `RenderPassPrune` `before:` it (`P/plugInfo.json:57,97-99,151,164,191,296`), which bind only when `HdGpSceneIndexPlugin`'s JSON entry (`loadWithRenderer: ""`, tag `hdGp:proceduralResolution`, `U/imaging/hdGp/plugInfo.json:12-21`) is present — it always is, but the plugin's `_IsEnabled` returns `HDGP_INCLUDE_DEFAULT_RESOLVER` (default false, `hdGp/sceneIndexPlugin.cpp:25-27,60-66`), so it appends nothing unless the env var is set. Neither usdrecord, usdview, `usdImagingGL` nor hdPrman set it (grep empty). **An HdGp-procedural-based hair system would render nothing under `usdrecord -r "RenderMan RIS"` out of the box**; an `HdSceneIndexPlugin` has no such gate.

## 8. usdrecord and the time source

`usdrecord.py:373-394` builds `UsdAppUtils.FrameRecorder(rendererPluginId, gpuEnabled, drawModeEnabled)` and calls `Record(stage, camera, timeCode, path)` per frame. `UsdAppUtilsFrameRecorder` owns a `UsdImagingGLEngine` (`U/usdImaging/usdAppUtils/frameRecorder.cpp:38-60`), disables presentation and sets `enableInteractive=false` (`:68-74`). `Record` sets `renderParams.frame = timeCode` (`:460-461`) and loops `_imagingEngine.Render(pseudoRoot, renderParams)` until `IsConverged()` (`:473-484`). `Render` → `PrepareBatch` (`engine.cpp:741-750`) → `_PreSetTime` (`ApplyPendingUpdates`, `:2353-2386`), `_usdImagingSceneIndex->SetTime(params.frame)`, optionally `_execStageSceneIndex->SetTime` (only with `USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX=1`, default false, `:90-96`, `:1617-1643`), then `_SetSceneGlobalsCurrentFrame(params.frame)` (`:483-498`) → `HdsiSceneGlobalsSceneIndex::SetCurrentFrame` (`:2062-2073`). usdview does the same with `self._renderParams.frame = self._dataModel.currentFrame` (`U/usdImaging/usdviewq/stageView.py:1467, 2247`).

hdPrman itself reads that value from the terminal SI: `HdUtils::GetCurrentFrame(terminalSi, &frame)` → `Ri:Frame = floor(frame)` (`P/renderSettings.cpp:278-291`; `U/imaging/hd/utils.cpp:87-108`), and the Renderer2 `RileyGlobalsSceneIndex` maps `sceneGlobals.currentFrame` to a Riley option (`P/rileyGlobalsSceneIndex.cpp:183-198`). Because the scene-globals SI is at all-renderers phase 0, a phase-1 UsdGen SI reads the identical `HdSceneGlobalsSchema::GetFromSceneIndex(input).GetCurrentFrame()` in usdview and usdrecord — one code path for interactive and batch. Note `timeCodesPerSecond` is *not* populated by anyone (§3.4).

## 9. Practical notes for the plan

- Test without RenderMan: hdPrman's plugin ordering can be exercised with `HdSceneIndexPluginRegistry::LoadAndGetSceneIndexPluginIds("RenderMan RIS", "")` (`sceneIndexPluginRegistry.cpp:1505-1529`) **only if** hdPrman's plugInfo is on `PXR_PLUGINPATH_NAME`; without the library the JSON entries are absent. The UsdGen plugin's own ordering relative to synthetic tags can be unit-tested with `SetPluginOrderingPolicy` (`:1551-1559`).
- Motion-blur debugging: `TF_DEBUG=HDPRMAN_MOTION_BLUR` prints per-primvar decisions (`P/motionBlurSceneIndexPlugin.cpp:259-263` etc.); `HDSI_VELOCITY_MOTION` for the velocity SI.
- `IsPrimvarFilteringNeeded=false` for hdPrman (`hdPrmanLoader/rendererPlugin.cpp:237-238`): all hair primvars pass through to Riley; keep the set small (each is sampled and converted per rprim, `P/renderParam.cpp:813-830`).
- hdPrman's supported rprims include `basisCurves` and `points` (`P/renderDelegate.cpp:238-254`); `extComputation` sprims are still registered (`:277`) but the pruning SI makes them irrelevant for points.

## Key facts

- usdview/usdrecord build hdPrman's chain through `engine.cpp:1776-1779` and then `plugin->CreateRenderer(terminalSI)` (`:1791-1793`) → `HdRenderIndex::NewForBackendEmulation` (`renderDelegateAdapterRenderer.cpp:203-207`, `renderIndex.cpp:268-289`), whose ctor skips the legacy `AppendSceneIndicesForRenderer` at `renderIndex.cpp:209-214` because `terminalSceneIndex` is set (`:172-175`).
- Both callers invoke the same `HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer` (`sceneIndexPluginRegistry.cpp:1421-1475`); the display names are `"RenderMan RIS"`, `"RenderMan XPU"`, `"RenderMan XPU - CPU"`, `"RenderMan XPU - GPU"` (`hdPrmanLoader/plugInfo.json:10`, `hdPrmanXpu*/plugInfo.json:10`, `P/tokens.cpp:21-30`).
- Under the default Hybrid policy, all-renderers entries precede renderer entries of the same phase (`:966-1005`, `:528-564`); an all-renderers phase-1 plugin lands after hdPrman phase 0 (ext-comp pruning `P/extComputationPrimvarPruningSceneIndexPlugin.h:28-35`) and before hdPrman phases 1-1000.
- A C++ registration without a matching JSON entry is silently dropped under Hybrid (`:862-869`, `:871-878`); `loadWithRenderer` is mandatory (`sceneIndexPlugin.h:38`).
- hdPrman motion blur: static shutter from `renderParam.cpp:5098-5110`; blurable keys are only `points` and instancer arrays (`motionBlurSceneIndexPlugin.cpp:219-244`); requires ≥2 samples (`:345-351`) and constant array size (`:353-363`); knobs `ri:object:mblur`, `ri:object:geosamples`, `blurScale` (`:177-217`, `:365-397`); adapter truncates to 16 samples (`sceneIndexAdapterSceneDelegate.cpp:2454-2457`, `renderParam.h:66-70`).
- Velocity SI runs at phase 2 before blur; valid `velocities` replace deformation samples with `{start,end}` (+ nonlinear samples) (`hdsi/velocityMotionResolvingSceneIndex.cpp:133-204`, `:349-455`); fps = scene-globals `timeCodesPerSecond` else 24 (`:273-287`, `:58`); nobody sets `timeCodesPerSecond` in usdview/usdrecord (grep empty); hdPrman's `fps` inputArg is ignored by hdsi (`:760-773`).
- Pinned curves: hdPrman maps `pinned`→`nonperiodic` (`P/basisCurves.cpp:118-122`); `HdPrman_PinnedCurveExpandingSceneIndexPlugin` (phase 3 AtEnd, `P/pinnedCurveExpandingSceneIndexPlugin.cpp:38-48`) expands bspline (+2/end) and catmullRom (+1/end) (`hdsi/pinnedCurveExpandingSceneIndex.cpp:627-671`) per shutter sample (`:162-189`); `centripetalCatmullRom` is rejected by hdPrman (`P/basisCurves.cpp:100-110`); time-varying topology unsupported (`:32`).
- Skinned scalps: UsdSkel imaging blocks `primvars:points` and emits `extComputationPrimvars` with both GLSL and CPU callbacks (`dataSourceResolvedPointsBasedPrim.h:28-34`, `.cpp:1085-1107`, `dataSourceResolvedExtComputationPrim.cpp:443-449`; `HD_ENABLE_DEFERRED_SKINNING` default false `skinningSettings.cpp:15-35`). hdPrman's phase-0 pruning SI presents them as authored primvars evaluated via the CPU callback on every pull (`hdsi/extComputationPrimvarPruningSceneIndex.cpp:383-398`, `:701-722`); Storm has no such plugin (grep empty), so the hair SI must wrap its input in `HdSiExtComputationPrimvarPruningSceneIndex` (pass-through when nothing is computed, `:703-721`).
- Dependency forwarding is the same `HdDependencyForwardingSceneIndex` at phase 1000 AtEnd in hdPrman (`P/dependencyForwardingSceneIndexPlugin.cpp:29-50`) and Storm (`hdSt/dependencyForwardingSceneIndexPlugin.cpp:42-46`, `hdSt/plugInfo.json:31-43`); upstream declarers differ (`P/dependencySceneIndexPlugin.cpp:63-81` phase 900 vs `hdSt/dependencySceneIndexPlugin.cpp:44-57` phase 100).
- No hdPrman-owned hdGp resolver (`CHANGELOG.md:2188-2191`; grep of `P/` shows only comments and CMake link `P/CMakeLists.txt:81`); the universal one is gated by `HDGP_INCLUDE_DEFAULT_RESOLVER` default false (`hdGp/sceneIndexPlugin.cpp:25-27,60-66`) and unset by usdrecord/usdview.
- usdrecord: `usdrecord.py:373-394` → `frameRecorder.cpp:460-484` → `engine.cpp:741-750`, `:483-498`, `:2062-2073`; hdPrman reads `currentFrame` via `HdUtils::GetCurrentFrame` (`P/renderSettings.cpp:278-291`, `hd/utils.cpp:87-108`) and `rileyGlobalsSceneIndex.cpp:183-198`.
- hdPrman is not built or installed locally (`$USD/plugin/usd/` lacks it; `PXR_BUILD_PRMAN_PLUGIN` default OFF, `Options.cmake:27`).

## Open questions

- No consumer of `HdSceneIndexCreateArgsSchema::GetMotionBlurSupport` exists in `pxr/` (grep); whether UsdImaging is meant to suppress time samples for Storm based on it is UNVERIFIED (currently it does not).
- Whether the Hybrid ordering actually resolves as traced in §2.2 when hdPrman's JSON `after: hd:sceneAssembly`/`hd:sceneGlobals` tags (which no entry carries) are present was derived by reading the algorithm, not by running `LoadAndGetSceneIndexPluginIds` with hdPrman loaded — no hdPrman binary exists here.
- Cost of `HdSiExtComputationPrimvarPruningSceneIndex` re-skinning on every `GetValue` under hdPrman for large scalps is asserted from code structure (`:243-…` builds a fresh value store); not measured.
- Whether Solaris/Katana hosts pass a non-empty `appName` on the legacy path (affecting `loadWithApps`) is outside this tree.
- The hdPrman 2.0 observer path (`HD_PRMAN_EXPERIMENTAL_RILEY_SCENE_INDEX_OBSERVER=1`, `RileyConversionSceneIndex`) was not analysed for its own points/motion sampling; the legacy rprim path documented in §3 is the default.
