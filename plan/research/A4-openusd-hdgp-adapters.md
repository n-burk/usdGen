# A4 — OpenUSD 26.08: hdGp generative procedurals, usdProc, UsdImaging prim adapters for custom schemas, codeless schemas, Hydra basisCurves/instancer contracts

Evidence base: OpenUSD source at `<openusd-src>` (git tag `v26.08`), install at `$USD`, usdRig at `<usdrig-src>`. All paths below are absolute unless prefixed by `pxr/` (then relative to `<openusd-src>`). Line numbers are from the checked-out v26.08 files. Claims I could not verify are marked **UNVERIFIED**.

---

## 0. Executive orientation (what the plan should take from this)

| Question | Short answer | Evidence |
|---|---|---|
| Is the hdGp resolver in the stock usdview/Storm chain? | Only when `HDGP_INCLUDE_DEFAULT_RESOLVER=1`; default `false`. usdview never sets it. | `pxr/imaging/hdGp/sceneIndexPlugin.cpp:83-85,117-124`; grep of `pxr/usdImaging/usdviewq` and `usdImagingGL` finds no reference outside the test. |
| Where would the default hdGp resolver sit? | In the renderer plugin chain (after the whole UsdImaging chain, after rigExec, after flattening), phase 2, "all renderers". | `sceneIndexPlugin.cpp:87-98`, `pxr/usdImaging/usdImagingGL/engine.cpp:1758-1780`, `pxr/imaging/hdGp/plugInfo.json` |
| Can we instantiate our own resolver with our own target type and position? | Yes: `HdGpGenerativeProceduralResolvingSceneIndex::New(input, TfToken("<ourType>"))` from any scene-index plugin. | `pxr/imaging/hdGp/generativeProceduralResolvingSceneIndex.h:51-63` |
| How do procedural children get their data? | `GetPrim()` on a generated path calls `proc->GetChildPrim(inputScene, path)` **every time**; the resolver caches nothing but the child type map. | `generativeProceduralResolvingSceneIndex.cpp:58-86` |
| Do prims of a custom (codeless) type reach Hydra without an adapter? | Yes: path present, `primType == ""`, data source = `UsdImagingDataSourcePrim` (xform/visibility/purpose/extent/primvars only). Custom (non-primvar) attributes are neither exposed nor invalidated. | `pxr/usdImaging/usdImaging/adapterManager.cpp:71-102,205-225`; `dataSourcePrim.cpp:693-812,864-911` |
| Can emitted curves declare a Hydra dependency on the surface mesh points? | Yes via `__dependencies`; Storm's chain has `HdSt_DependencyForwardingSceneIndexPlugin` (phase 1000, GL only). Discovery is lazy (first `GetPrim`). | `pxr/imaging/hd/dependencyForwardingSceneIndex.cpp:33-58,309-361,487-570`; `pxr/imaging/hdSt/dependencyForwardingSceneIndexPlugin.cpp:26-36` |
| Ordering vs rigExec | rigExec inserts via `UsdImagingSceneIndexPlugin`; that mechanism orders plugins by `std::set<TfType>` (pointer order) — **undefined relative order** between two such plugins. Use the renderer-chain (`HdSceneIndexPlugin`) mechanism to be reliably after rigExec. | `<usdrig-src>/libs/rigExecImaging/sceneIndexPlugin.cpp:23-45`; `pxr/usdImaging/usdImaging/sceneIndexPlugin.cpp:45-55`; `pxr/base/tf/type.h:119` |
| Codeless vs generated schema | Codeless (as usdRig does) is sufficient: adapters key off the schema identifier string; `UsdImagingDataSourceMapped`/`UsdImagingDataSourceAttributeNew` work on untyped `UsdAttribute`. | `pxr/usd/usd/usdGenSchema.py:195-247`, `pxr/usd/usd/docs/generatingSchemas.md:689-703`, `adapterManager.cpp:170-172` |

---

## 1. HdGp generative procedurals — full lifecycle in 26.08

### 1.1 Files and classes

| File | Role |
|---|---|
| `pxr/imaging/hdGp/generativeProcedural.h/.cpp` | `HdGpGenerativeProcedural` base class; tokens |
| `pxr/imaging/hdGp/generativeProceduralPlugin.h`, `generativeProceduralPluginRegistry.h/.cpp` | Hf plugin base + registry (`Construct(primPath)`) |
| `pxr/imaging/hdGp/generativeProceduralResolvingSceneIndex.h/.cpp` | The resolver filtering scene index (cooks, diffs child sets, serves child prims) |
| `pxr/imaging/hdGp/generativeProceduralFilteringSceneIndex.h/.cpp` | Optional allow/skip filter that retypes procedural prims (`skippedHydraGenerativeProcedural`) |
| `pxr/imaging/hdGp/sceneIndexPlugin.h/.cpp`, `plugInfo.json` | `HdGpSceneIndexPlugin` registering the default resolver (env-gated) |
| `pxr/usd/usdProc/schema.usda` | `GenerativeProcedural` USD schema (Boundable + `proceduralSystem`) |
| `pxr/usd/usdHydra/schema.usda:170-201` | `HydraGenerativeProceduralAPI` (adds `primvars:hdGp:proceduralType`, fallback `proceduralSystem = "hydraGenerativeProcedural"`) |
| `pxr/usdImaging/usdProcImaging/generativeProceduralAdapter.h/.cpp`, `plugInfo.json` | Prim adapter mapping USD `GenerativeProcedural` → Hydra prim of type `<proceduralSystem>` |
| `pxr/usdImaging/usdImagingGL/testenv/TestUsdImagingGLHdGpProcedurals.cpp` + `_plugInfo.json`, `testUsdImagingGLHdGp/test.usda` | Reference procedurals (MakeSomeStuff, CubePerMeshPoint, DependsOnChildNames, AsyncTest, DependsOnRemoved) |

Installed: `libusd_hdGp.so`, `libusd_usdProc.so`, `libusd_usdProcImaging.so` and their `lib/usd/{hdGp,usdProc,usdProcImaging}` plugInfo dirs exist in `$USD/lib`; Python module `UsdProc` exists under `lib/python3.12/site-packages/pxr/` (no Python bindings for hdGp itself — no `wrap*.cpp` in `pxr/imaging/hdGp`).

### 1.2 Tokens and the two-level type convention

```cpp
// pxr/imaging/hdGp/generativeProcedural.h:17-22
#define HDGPGENERATIVEPROCEDURAL_TOKENS
    ((generativeProcedural,         "hydraGenerativeProcedural"))
    ((resolvedGenerativeProcedural, "resolvedHydraGenerativeProcedural"))
    ((skippedGenerativeProcedural,  "skippedHydraGenerativeProcedural"))
    ((proceduralType,               "hdGp:proceduralType"))
    ((anyProceduralType,            "*"))
```

- **Hydra prim type** of the procedural prim = the USD `proceduralSystem` attribute value; the adapter returns it verbatim as the Hydra type, or `inertGenerativeProcedural` if empty (`generativeProceduralAdapter.cpp:329-351`). `HydraGenerativeProceduralAPI` supplies the fallback `"hydraGenerativeProcedural"` (`usdHydra/schema.usda:191`), which matches the resolver's default `_targetPrimTypeName` (`generativeProceduralResolvingSceneIndex.cpp:29-34`).
- **Procedural implementation** = primvar `hdGp:proceduralType` (token) read from the prim's `primvars` container (`generativeProceduralResolvingSceneIndex.cpp:696-707`). The registry maps it to a plugin by matching `desc.displayName == proceduralTypeName`, else by TfType id (`generativeProceduralPluginRegistry.cpp:150-172`). Test plugInfo shows the pattern: type `CubePerMeshPointProceduralPlugin`, `bases: ["HdGpGenerativeProceduralPlugin"]`, `displayName: "CubePerMeshPoint"`, `priority: 0` (`TestUsdImagingGLHdGpProcedurals_plugInfo.json`). Registration: `HdGpGenerativeProceduralPluginRegistry::Define<MyPlugin, HdGpGenerativeProceduralPlugin>()` inside `TF_REGISTRY_FUNCTION(TfType)` (`TestUsdImagingGLHdGpProcedurals.cpp:1168-1189`). Env `PXR_HDGP_TEST_PLUGIN_PATH` registers an extra plugin path (`generativeProceduralPluginRegistry.cpp:134-139`).
- A pipeline can run **multiple resolver instances** with different target type names to stage procedurals at different chain positions (`generativeProceduralResolvingSceneIndex.h:28-32`; `usdHydra/schema.usda:192-199`).

USD authoring reference (`testUsdImagingGLHdGp/test.usda:42-50`):

```usda
def GenerativeProcedural "cubePerMeshProc" (prepend apiSchemas = ["HydraGenerativeProceduralAPI"]) {
    token primvars:hdGp:proceduralType = "CubePerMeshPoint"
    rel   primvars:sourceMeshPath = </World/myMesh2>
    float primvars:scale = 2
    def Mesh "childMeshToPassThrough" { ... }   # authored children pass through the resolver
}
```

Relationship arguments work because `UsdImagingDataSourcePrimvars::Get` turns `rel primvars:<name>` into a constant primvar whose value is `VtArray<SdfPath>` via `UsdImagingDataSourceRelationship` (`pxr/usdImaging/usdImaging/dataSourcePrimvars.cpp:167-174`; `dataSourceRelationship.h:22-43`). The reference procedural reads it as `VtArray<SdfPath>` or `std::string` (`TestUsdImagingGLHdGpProcedurals.cpp:516-529`).

### 1.3 The virtual interface (contract + threading)

```cpp
// pxr/imaging/hdGp/generativeProcedural.h:45-49
using DependencyMap  = TfDenseHashMap<SdfPath, HdDataSourceLocatorSet, TfHash>;
using ChildPrimTypeMap = TfDenseHashMap<SdfPath, TfToken, TfHash>;

virtual DependencyMap UpdateDependencies(const HdSceneIndexBaseRefPtr &inputScene) = 0;      // :59-60
virtual ChildPrimTypeMap Update(const HdSceneIndexBaseRefPtr &inputScene,
        const ChildPrimTypeMap &previousResult, const DependencyMap &dirtiedDependencies,
        HdSceneIndexObserver::DirtiedPrimEntries *outputDirtiedPrims) = 0;                     // :100-104
virtual HdSceneIndexPrim GetChildPrim(const HdSceneIndexBaseRefPtr &inputScene,
        const SdfPath &childPrimPath) = 0;                                                     // :111-113
static const HdDataSourceLocator &GetChildNamesDependencyKey();   // "__childNames", :118, .cpp:31-36
virtual bool AsyncBegin(bool asyncEnabled);                       // default false, .cpp:38-42
virtual AsyncState AsyncUpdate(const ChildPrimTypeMap &previousResult,
        ChildPrimTypeMap *outputPrimTypes, HdSceneIndexObserver::DirtiedPrimEntries *outputDirtiedPrims); // default Finished
```

Threading rules stated in the header: `UpdateDependencies`/`Update` are never called concurrently for one instance (`:56-57`, `:97-98`); `GetChildPrim` "should expect to be called from multiple threads" (`:109`). Different procedurals **are** cooked in parallel: when ≥ 2 procedurals need cooking the resolver uses `WorkWithScopedParallelism` + `WorkParallelForEach` under `TF_PY_ALLOW_THREADS_IN_SCOPE` (`generativeProceduralResolvingSceneIndex.cpp:277-299`, `:498-519`, `:611-639`). A TODO admits the cook is not fully guarded by the state CAS (`:832-833`), so a single procedural instance must still be robust to being cooked from the notice thread of the moment.

`Update` semantics (`generativeProcedural.h:62-95`): return the **full** child set every time; new paths → added; missing → removed; type change → re-added; unchanged paths are dirty only if pushed into `outputDirtiedPrims` (locator granularity is free). `dirtiedDependencies` carries which declared dependencies changed since last cook; on the initial cook (or after a dependency removal) it contains the full declared set (`:81-86`, resolver `:827-830`). Own-prim changes are not in `dirtiedDependencies` for initial cooks (`:88-89`).

### 1.4 Resolver internals, step by step (with line refs)

**Construction.** `New(input)` / `New(input, targetTypeName)` (`.h:51-63`). Constructor forces plugin discovery and **does not scan an already-populated input** (XXX at `.cpp:39-40, 54-55`) — procedurals are discovered only via `PrimsAdded`. This is fine in usdview because the engine builds the renderer plugin chain first (`engine.cpp:1528`) and only then inserts the UsdImaging scene index into the merging scene index (`engine.cpp:1544`), and `HdMergingSceneIndex::InsertInputScenes` replays the new input's prims as `PrimsAdded` when observed (`pxr/imaging/hd/mergingSceneIndex.cpp:182-211`). If you create your own resolver on an already-populated input you must replay adds yourself.

**PrimsAdded (`.cpp:155-330`).** For each entry: if `primType == target` the outgoing entry is retyped to `resolvedHydraGenerativeProcedural` and the prim is scheduled to cook (`:182-193`); a prim that was previously a procedural but changed type is "cooked" to be removed (`:196-201`); prims that are declared dependencies trigger their dependents (`:207-226`); parents declared with `__childNames` trigger dependents (`:233-267`). Cooks run (parallel if ≥ 2) then `_SendPrimsAdded/_SendPrimsRemoved/_SendPrimsDirtied` (`:317-329`).

**Cook: `_UpdateProcedural` (`:793-857`).** `forceUpdate` resets state to `Uncooked` (`:809-811`). `_UpdateProceduralDependencies` (`:671-791`) re-reads the prim from the input, removes the procedural if its type no longer matches (`:678-681`), reads `hdGp:proceduralType` (`:696-707`), constructs (or reconstructs on type change) the procedural through the registry and calls `AsyncBegin(_attemptAsync)` (`:711-722`); an existing procedural gets another `AsyncBegin(true)` chance if async is now allowed (`:727-734`); then `UpdateDependencies(input)` (`:741-743`) and the reverse dependency map `_dependencies[depPath] -> {procPath}` is diffed/updated (`:755-786`). Then `Update(input, previousChildTypes, dirtiedDeps-or-all, &notices.dirtied)` (`:834-839`), and `_UpdateProceduralResult` (`:1025-1195`) computes added/removed notices, records `childHierarchy` (parent → children) for every ancestor between the child and the procedural (`:1046-1055`), and inserts **intermediate untyped prims** (`TfToken()` type) for hierarchy gaps (`:1149-1153`). Every generated path (children and intermediates) maps to the responsible `_ProcEntry` in `_generatedPrims` (`:1179-1185`).

**Constraint: children must live under the procedural prim path.** Paths that do not have the procedural path as prefix are announced in the added notice but never recorded in `childHierarchy`/`_generatedPrims` (`:1046-1058`, `:1073-1083`, `// TODO, warning, error`), so `GetPrim` will not route them to the procedural — effectively unsupported.

**GetPrim (`:58-86`).** Generated path → `proc->GetChildPrim(input, path)` (called on **every** query; no caching by the resolver). Otherwise pass-through, with procedural prims retyped to `resolvedHydraGenerativeProcedural`. Because `_generatedPrims` is consulted first, a procedural can mask an authored descendant of the same path (`.h:112-116` comment on `GetChildPrimPaths`).

**GetChildPrimPaths (`:106-153`).** Always merges the input's children with the procedural's `childHierarchy` (authored descendants of a procedural pass through; see `test.usda:48-58`).

**PrimsDirtied (`:553-669`).** For each entry: dirtiness on the procedural prim itself is recorded as `deps[proc][proc] = locators` (`:570-573`); dirtiness on any declared dependency path whose locators `Intersects` the declared set is recorded (`:575-596`); each affected procedural is recooked with `forceUpdate=true` and the precise dirtied map (`:600-656`). Input dirtied entries are forwarded unchanged plus whatever the procedurals added (`:606-608`, `:658-667`). Note: **any** locator on the procedural prim (e.g. `visibility`) triggers a recook — `Update` should compare args and return `previousResult` cheaply (pattern in `TestUsdImagingGLHdGpProcedurals.cpp:98-148`).

**PrimsRemoved (`:332-551`).** Root removal clears everything (`:339-356`). Removing a dependency (or an ancestor of it) recooks dependents with the full dependency set (`:397-427`, `:483-547`); removing a procedural (or an ancestor) drops its record (`:465-481`); the dependency record for a removed dependency is intentionally kept so re-adding it re-triggers (`:401-403`).

**Dependency granularity.** `DependencyMap` is per depended-on prim path × `HdDataSourceLocatorSet`; matching is by `HdDataSourceLocatorSet::Intersects` (prefix semantics, `pxr/imaging/hd/dataSourceLocator.h:321-332`). Special key `__childNames` (`generativeProcedural.cpp:34`) reacts to child add/remove of a prim (`:229-267`, `:429-462`). There is no dependency on "all prims of type X" or on value-level changes — only on scene-index notices.

**Async (`:954-1023`).** `_SystemMessage(asyncAllow)` flips `_attemptAsync` (`:961-966`); `asyncPoll` iterates `_activeSyncProcedurals` calling `AsyncUpdate`, applying results with `_UpdateProceduralResult` and sending notices (`:972-1022`). Only `UsdImagingGLEngine` sends these messages: `asyncAllow` right after chain construction when `Parameters::allowAsynchronousSceneProcessing` is true (`engine.cpp:1497-1500`), `asyncPoll` from `PollForAsynchronousUpdates()` (`engine.cpp:2648-2655`). usdview: `params.allowAsynchronousSceneProcessing = self._allowAsync` (`pxr/usdImaging/usdviewq/stageView.py:966`), default `False` (`stageView.py:944`), and a 100 ms `QTimer` calls `PollForAsynchronousUpdates` (`appController.py:524-527`, `:5515-5519`). So async procedurals work in usdview only when the app enables async (property at `stageView.py:829-833`); with it, a procedural can publish incremental results (e.g. progressive hair generation) every 100 ms on the main thread.

### 1.5 Where the default resolver is inserted and how it orders against Storm

```cpp
// pxr/imaging/hdGp/sceneIndexPlugin.cpp:83-98
TF_DEFINE_ENV_SETTING(HDGP_INCLUDE_DEFAULT_RESOLVER, false, "...");
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        HdSceneIndexPluginRegistryTokens->allRenderers, TfToken("HdGpSceneIndexPlugin"),
        nullptr, HdGpSceneIndexPlugin::GetInsertionPhase() /* 2 */, InsertionOrderAtStart);
}
```

`_IsEnabled` is a static read of the env setting (`:117-124`); `_AppendSceneIndex` honours an optional `proceduralPrimTypeName` token in `inputArgs` (`:126-142`). plugInfo tags: `"tags": ["hdGp:proceduralResolution"]`, `"ordering": {"after": "hd:sceneAssembly"}` (`pxr/imaging/hdGp/plugInfo.json`). Note `hd:sceneAssembly`/`hd:sceneGlobals` are not defined anywhere in C++ (grep over `pxr/` finds them only in plugInfo files); with no plugin carrying the tag they are inert constraints (`sceneIndexPluginRegistry.cpp:606-613`).

Ordering policy env: `HD_SCENE_INDEX_PLUGIN_ORDERING_POLICY_DEFAULT` = `Hybrid` (`pxr/imaging/hd/sceneIndexPluginRegistry.cpp:41-49`); registration order sorts by phase, then insertion order, then plugin id, and within a phase "all renderers" entries precede renderer-specific ones (`:496-560`). Storm (renderer display name `"GL"`) plugins relevant to us (`pxr/imaging/hdSt/plugInfo.json`):

| Plugin | Phase | Relative to hdGp (phase 2) |
|---|---|---|
| `HdSt_MaterialBindingResolvingSceneIndexPlugin`, `HdSt_NodeIdentifierResolving…`, `HdSt_ImplicitSurface…`, `HdSt_NurbsApproximating…` | 0 | before |
| `HdSt_RenderPassPruneSceneIndexPlugin` | 1, `"before": "hdGp:proceduralResolution"` | before |
| `HdSt_MaterialPrimvarTransferSceneIndexPlugin` | 3 (`"after": [..., "hdGp:proceduralResolution"]`, `materialPrimvarTransferSceneIndexPlugin.cpp:35-38`) | after |
| `HdSt_DependencySceneIndexPlugin` | 100 | after |
| `HdSt_DependencyForwardingSceneIndexPlugin` | 1000, `InsertionOrderAtEnd` | after (terminal) |

Consequences for generated prims: implicit-surface conversion and NURBS approximation are **not** applied to hdGp output (emit `basisCurves`/`mesh` directly); Storm's phase-0 material binding resolver, which maps `preview` → allPurpose (`pxr/imaging/hdSt/materialBindingResolvingSceneIndexPlugin.cpp:54-57`), has already run, so generated prims must carry the allPurpose binding themselves (`materialBindings/""` — see §5); material primvar transfer and dependency forwarding **do** apply.

The plugin chain is appended by `UsdImagingGLEngine::_CreateSceneIndexChainAndRenderer` onto `_mergingSceneIndex` (`engine.cpp:1758-1780`) — i.e. after the entire UsdImaging chain (`pxr/usdImaging/usdImaging/sceneIndices.cpp:191-325`), which includes `_AddPluginSceneIndices` (`:301-302`) where rigExec lives. `appName` is not passed by the engine (`engine.cpp:1778-1780`), so `loadWithApps` filtering is not available in usdview.

### 1.6 Cost and limitation summary for hdGp

- Cook is synchronous inside the notice handler (main thread during `PrepareBatch` → `_PreSetTime` → `ApplyPendingUpdates`, `engine.cpp:468-498`, `:2354-2386`), possibly on TBB workers when ≥ 2 procedurals cook. Keep `Update` cheap; defer heavy value work to lazily evaluated data sources returned by `GetChildPrim` (header advice `:91-95`; reference `_XformFromMeshPointDataSource` `TestUsdImagingGLHdGpProcedurals.cpp:436-495` which also forwards `GetContributingSampleTimesForInterval` from the mesh points for motion blur).
- No result caching in the resolver: `GetChildPrim` must be cheap or return cached handles (the reference procedural caches `_meshPointsDs` and `_primMatrixDs` and rebuilds only when its dependency is in `dirtiedDependencies`, `:267-333`).
- Not recursive; all procedurals see the same input scene (`.h:39-42`). A chain "generator → clump → generator → clump" cannot be expressed as nested hdGp procedurals inside one resolver instance; it must be one procedural that owns the whole operator chain, or multiple resolver instances with distinct target types.
- Children must be under the procedural path (§1.4).
- Because the default resolver sits after flattening, child `xform/matrix` must be **world-space** (no flattening scene index follows in Storm's plugin chain; grep of `pxr/imaging/hdSt/plugInfo.json` for "Flattening" is empty).
- The retyped `resolvedHydraGenerativeProcedural` prim is not a supported rprim/sprim/bprim type, so emulation ignores it silently (`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:268-286`).
- The GL test for hdGp (`testUsdImagingGLHdGp.cpp`) is not registered in `pxr/usdImaging/usdImagingGL/CMakeLists.txt` in 26.08 (grep empty) — treat the reference procedurals as examples, not as CI-covered behaviour.

---

## 2. Writing a UsdImaging prim adapter for custom schema types (Hydra 2.0)

### 2.1 Discovery and plugInfo keys

`UsdImagingAdapterRegistry` (`pxr/usdImaging/usdImaging/adapterRegistry.cpp`) enumerates `PlugRegistry::GetAllDerivedTypes(UsdImagingPrimAdapter)` from plugInfo metadata without loading libraries (`:50-54`) and reads:

| Key | Meaning | Evidence |
|---|---|---|
| `primTypeName` (string, required) | USD **schema identifier** the adapter handles; duplicates warn and last wins | `:104-131` |
| `isInternal` (bool) | when `USDIMAGING_ENABLE_PLUGINS=0` only internal adapters load | `:42-45`, `:77-100` |
| `includeDerivedPrimTypes` (bool) | also register for all `UsdSchemaRegistry` derived types (works with codeless bases) | `:137-147`, `:199-245` |
| `includeSchemaFamily` (bool) | register for all versions in the schema family | `:150-191`; example `UsdImagingCapsuleAdapter` in `usdImaging/plugInfo.json:55-57` |
| `apiSchemaName` (string) | for `UsdImagingAPISchemaAdapter` subclasses; empty string = keyless adapter run for every prim | `:297-323`, `ConstructKeylessAPISchemaAdapters` `:474-490` |

The library is `Load()`ed lazily on first `ConstructAdapter` (`:392-436`); the adapter class must define a TfType with `SetFactory<UsdImagingPrimAdapterFactory<Adapter>>` (`generativeProceduralAdapter.cpp:33-38`). A plugInfo entry example (`pxr/usdImaging/usdProcImaging/plugInfo.json`):

```json
"UsdProcImagingGenerativeProceduralAdapter": {
    "bases": ["UsdImagingInstanceablePrimAdapter"],
    "primTypeName": "GenerativeProcedural"
}
```

Lookup key per prim is `UsdPrimTypeInfo::GetSchemaTypeName()` (`adapterManager.cpp:170-171`), i.e. the `schemaIdentifier` of a codeless type (e.g. `RigExecAimConstraint` in `<usdrig-src>/plugin/rigExecSchema/resources/plugInfo.json`), so `primTypeName` must be that identifier (for a `UsdGen` family: whatever identifiers `schema.usda` declares).

### 2.2 The Hydra 2.0 virtuals

```cpp
// pxr/usdImaging/usdImaging/primAdapter.h:63-81,113,120-125
virtual TfTokenVector GetImagingSubprims(UsdPrim const&);                              // default: TF_WARN + empty (primAdapter.cpp:81-86)
virtual TfToken GetImagingSubprimType(UsdPrim const&, TfToken const& subprim);         // default empty
virtual HdContainerDataSourceHandle GetImagingSubprimData(UsdPrim const&, TfToken const& subprim,
        const UsdImagingDataSourceStageGlobals&);                                        // default nullptr
virtual HdDataSourceLocatorSet InvalidateImagingSubprim(UsdPrim const&, TfToken const& subprim,
        TfTokenVector const& properties, UsdImagingPropertyInvalidationType);           // default: UsdImagingDataSourcePrim::Invalidate
virtual PopulationMode GetPopulationMode();     // RepresentsSelf | RepresentsSelfAndDescendents | RepresentedByAncestor
virtual HdDataSourceLocatorSet InvalidateImagingSubprimFromDescendent(...);
```

Use `UsdImagingSceneIndexPrimAdapter` as the base: it makes the four scene-index methods pure and provides `final` no-op implementations of the Hydra 1.0 pure virtuals (`Populate`, `TrackVariability`, `UpdateForTime`, `ProcessPropertyChange`, `MarkDirty`, `_RemovePrim`) (`pxr/usdImaging/usdImaging/sceneIndexPrimAdapter.h:14-112`). UsdSkelImaging uses it for `SkelAnimation`/`BlendShape` (`pxr/usdImaging/usdSkelImaging/plugInfo.json`). The usdProc adapter instead derives from `UsdImagingInstanceablePrimAdapter` and carries a full Hydra 1.0 implementation (`generativeProceduralAdapter.h:15-114`) — unnecessary for a Hydra-2.0-only plugin since scene-index mode is the default (`USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=true`, `engine.cpp:78-84`).

**Subprims**: return `{TfToken()}` for one Hydra prim per USD prim; extra tokens create child hydra prims at `<primPath>.<subprim>` property paths (`stageSceneIndex.cpp:326-338`; comment in `sceneIndexPrimAdapter.h:50-59`). Multiple adapters (prim + API schema adapters) contribute: keyless API adapters first (strongest), then the prim adapter, then applied API schemas in application order; data sources are overlaid in that order, type = first non-empty, invalidation = union (`adapterManager.cpp:149-186`; `stageSceneIndex.cpp:100-186`).

**PopulationMode**: `RepresentsSelfAndDescendents` prunes population of the subtree and routes descendant property changes to `InvalidateImagingSubprimFromDescendent` (`stageSceneIndex.cpp:326-338,395-403,655-674,812-846`). This is the mechanism for an operator-graph root prim that wants to own its children (e.g. a `UsdGenGroom` scope whose operator children are not individually imaged).

### 2.3 Data sources and locators for operator parameters and relationships

Building blocks (all in `pxr/usdImaging/usdImaging/`):

| Class | What it gives | Evidence |
|---|---|---|
| `UsdImagingDataSourcePrim` | prim container with `xform` (`matrix`, `resetXformStack`), `visibility`, `purpose`, `extent`, `primvars`, `model`, `extentsHint`, `__usdPrimInfo`, `primOrigin` (holds the `UsdPrim`) | `dataSourcePrim.cpp:693-812`; `dataSourcePrim.h:344-400` |
| `UsdImagingDataSourcePrim::Invalidate` | property name → locator: `visibility`, `purpose`, xformOp attrs → `xform`, `extent`, `extentsHint`, `primvars:X` → `primvars/X` (Update) or `primvars` (Resync) | `dataSourcePrim.cpp:864-911` |
| `UsdImagingDataSourceGprim` | adds `primvars` (with custom mappings like `points`, `normals`) | `dataSourceGprim.cpp:74-140` |
| `UsdImagingDataSourceAttribute<T>` / `UsdImagingDataSourceAttributeNew(attr, globals, sceneIndexPath, timeVaryingFlagLocator)` | sampled data source evaluating at `stageGlobals.GetTime() + shutterOffset`, falling back to the schema default; `GetContributingSampleTimesForInterval` from USD time samples; flags the locator as time-varying at construction if the attribute might vary | `dataSourceAttribute.h:41-66,71-126,225-243` |
| `UsdImagingDataSourceRelationship` | `HdPathArrayDataSource` from a `UsdRelationship` (never time-varying) | `dataSourceRelationship.h:22-59` |
| `UsdImagingDataSourceMapped` | declarative attribute/relationship → (nested) locator mapping + static `Invalidate(usdNames, mappings)`; relationship factories `GetPathFromRelationshipDataSourceFactory()` / `GetPathArrayFromRelationshipDataSourceFactory()` | `dataSourceMapped.h:43-193` |
| `UsdImagingDataSourcePrimvars` | `primvars:*` attributes **and relationships** → `HdPrimvarSchema` containers | `dataSourcePrimvars.cpp:83-174` |
| `UsdImagingDataSourceStageGlobals` | `GetTime()`, `FlagAsTimeVarying(hydraPath, locator)`, `FlagAsAssetPathDependent(usdPath)` | `dataSourceStageGlobals.h:32-58` |

Reference implementation of a custom-schema prim data source (UsdSkel Skeleton): mappings built once from `GetSchemaAttributeNames(false)` into `HdDataSourceLocator(usdName)` under the schema's default locator; `Get(name)` returns `UsdImagingDataSourceMapped::New(prim, path, mappings, globals)`; `Invalidate` unions `UsdImagingDataSourceMapped::Invalidate(properties, mappings)` with the gprim invalidation (`pxr/usdImaging/usdSkelImaging/dataSourceSkeletonPrim.cpp:23-45,84-90,114-129`). Also used by `dataSourceNurbsCurves.cpp`, `dataSourceAnimationPrim.cpp`, `dataSourceBindingAPI.cpp`, `geomModelAPIAdapter.cpp`.

Recommended layout for an operator prim: Hydra type `"usdGenOperator"` (or per-operator type), prim data source = overlay of `UsdImagingDataSourcePrim` (free xform/vis/purpose/primvars) + a `usdGen` container built with `UsdImagingDataSourceMapped` holding every operator attribute (`usdGen:*`) as sampled data sources and relationships (`usdGen:inputs`, `usdGen:surface`, `usdGen:maps`) as path arrays. Time-varying attributes get dirtied for free on `SetTime` (`stageSceneIndex.cpp:369-384`, `_StageGlobals::SetTime` `:905-918`).

### 2.4 Invalidation you get for free vs. what you must implement

USD → Hydra change flow (`stageSceneIndex.cpp`):
- `_OnUsdObjectsChanged` (`:500-590`): prim resyncs → repopulate subtree (`removed`+`added`); property resyncs → `Resync` invalidation; info-only property changes → `Update` invalidation; **prim-level metadata changes only resync if the field is a plugin field** (`:552-566`) — note for `apiSchemas`/custom metadata edits; asset-path dependents are tracked separately (`:577-589`).
- `ApplyPendingUpdates` (`:754-800`) runs `_ApplyPendingResyncs` then `_ComputeDirtiedEntries` (`:803-873`) which calls each adapter's `InvalidateImagingSubprim`; returning the locator `UsdImagingTokens->stageSceneIndexRepopulate` (`"__usdStageSceneIndexRepopulate"`, `tokens.h:44`) forces a resync of that prim — the usdProc adapter uses this when `proceduralSystem` changes the Hydra type (`generativeProceduralAdapter.cpp:86-95`).
- The engine calls `ApplyPendingUpdates()` in `_PreSetTime` on every `PrepareBatch`/`Render` (`engine.cpp:2354-2386`, `:468-498`); usdview redraws on stage-change notices, so a USD edit becomes Hydra notices at the next frame. Edits made from the usdview plugin (Python) therefore round-trip through the stage; there is no direct "poke Hydra" API — rigExec instead keeps an out-of-stage store and its own results scene index (`<usdrig-src>/libs/rigExecImaging/registry.h:355-475`).

Free: xform/visibility/purpose/extent/primvar invalidation, time-varying dirtying, resync on prim type/apiSchemas (plugin-field) changes, native/point instancing propagation, flattening of `xform`, `visibility`, `purpose`, `materialBindings`, `model`, coordSys (`pxr/usdImaging/usdImaging/flattenedDataSourceProviders.cpp:28-78`, applied inside `UsdImagingNiPrototypePropagatingSceneIndex` at `niPrototypePropagatingSceneIndex.cpp:196-206`).

Yours: invalidation for custom attributes/relationships (`UsdImagingDataSourceMapped::Invalidate` covers 1:1 mappings); cross-prim dependencies (a relationship target's change does **not** dirty the referring prim — nothing in the stage scene index tracks relationship targets; use `__dependencies` in a downstream scene index, §5, or your own evaluation graph).

### 2.5 Population predicate

Stage scene index population uses `UsdPrimIsActive && !UsdPrimIsAbstract [&& UsdPrimIsLoaded]` — not `IsDefined`, not imageable (`stageSceneIndex.cpp:481-503`). Non-Imageable `Typed` prims (as rigExec's movers) are populated with their adapter (or an empty type). Instance proxies are excluded (`:250-256`); prototypes are populated under `/__Prototype_N` (`:334-338`, `:409-412`).

---

## 3. What a downstream scene index sees for prims with NO adapter

From `adapterManager.cpp:205-225` and `:71-102`: with no registered prim adapter, `_ComputeWrappedPrimAdapter` installs `_BasePrimAdapterAPISchemaAdapter`, whose `GetImagingSubprimData("")` returns `UsdImagingDataSourcePrim::New(path, prim, globals)` and whose `InvalidateImagingSubprim` is `UsdImagingDataSourcePrim::Invalidate`. `GetImagingSubprims` defaults to `{""}` (`stageSceneIndex.cpp:47-56`), `GetImagingSubprimType` returns `TfToken()` (`apiSchemaAdapter.cpp:21-28`).

Therefore, for a codeless `UsdGenClumpStyler` prim inheriting `Typed` (non-Imageable):
- It **is present**: appears in `PrimsAdded` (type `""`), in `GetChildPrimPaths` of its parent, and `GetPrim` returns `{"", <UsdImagingDataSourcePrim>}` (`stageSceneIndex.cpp:237-285`; empty container substituted if null, `:273-277`).
- Data source names: `__usdPrimInfo`, `primOrigin`, plus `primvars` only if authored primvars exist; `visibility`/`purpose` only if `IsA<UsdGeomImageable>`; `xform` only if `IsA<UsdGeomXformable>`; `extent` only if Boundable (`dataSourcePrim.cpp:693-739`).
- **Custom attributes (`usdGen:*`) are not exposed** and changes to them produce **no dirty notice** (`Invalidate` handles only the standard names and `primvars:` `dataSourcePrim.cpp:872-905`). A downstream scene index can obtain the `UsdPrim` from `primOrigin` (`dataSourcePrim.h:386-400`) and read attributes itself, but it would learn about edits only through its own `TfNotice` subscription (which is what rigExec does: `registry.h:445-450` `_OnObjectsChanged`).
- Parameters authored as `primvars:usdGen:*` **are** exposed (as `primvars/usdGen:*` with interpolation from `UsdGeomPrimvar`, relationships as constant path-array primvars) and dirtied per-primvar (`dataSourcePrimvars.cpp:134-174`; `dataSourcePrim.cpp:894-904`). This is exactly the usdProc convention and is the cheapest way to get free invalidation without writing an adapter; the cost is that parameters look like primvars to other consumers (harmless on a non-rprim type, since emulation ignores unknown types).
- `PrimsDirtied` for such prims are forwarded through the chain to Storm's emulation, which ignores prims of unknown type (`sceneIndexAdapterSceneDelegate.cpp:268-286`): no cost beyond notice traffic.

---

## 4. Valid Hydra 2.0 `basisCurves` and `instancer` prims (what Storm needs)

Storm in 26.08 is still a Hydra 1.0 render delegate wrapped by `HdRenderDelegateAdapterRenderer` (`pxr/imaging/hd/rendererPlugin.cpp:144-157`) and consumes the terminal scene index through `HdSceneIndexAdapterSceneDelegate` (`pxr/imaging/hd/renderIndex.cpp:223-230`). That delegate defines the required data sources.

### 4.1 basisCurves

| Data source (locator) | Type / values | Required? | Evidence |
|---|---|---|---|
| prim type | `HdPrimTypeTokens->basisCurves` (`"basisCurves"`) | yes | `pxr/imaging/hd/tokens.h:283`; `usdImaging/basisCurvesAdapter.cpp:55-64` |
| `basisCurves/topology/curveVertexCounts` | `VtIntArray` | **yes** (empty topology if absent) | `sceneIndexAdapterSceneDelegate.cpp:865-885`; `basisCurvesTopologySchema.h:35-41` |
| `basisCurves/topology/curveIndices` | `VtIntArray` | optional | `:886-891` |
| `basisCurves/topology/basis` | `bezier`(default) / `bspline` / `catmullRom` / `centripetalCatmullRom` | optional | `:892-896`; `tokens.h:25-30` |
| `basisCurves/topology/type` | `linear`(default) / `cubic` | optional | `:897-901`; `tokens.h:34,63` |
| `basisCurves/topology/wrap` | `nonperiodic` / `periodic` / `pinned` | optional | `tokens.h:72,76,77` |
| `primvars/points` | `VtVec3fArray`, interpolation `vertex`, role `point` | **yes** | `HdPrimvarsSchema::GetPointsLocator` `primvarsSchema.h:110`; example `TestUsdImagingGLHdGpProcedurals.cpp:603-615` |
| `primvars/widths` | `VtFloatArray`; `constant`/`uniform`/`varying`/`vertex`; if `varying` Storm uses linear width interpolation, otherwise basis | optional | `pxr/imaging/hdSt/basisCurves.cpp:1083-1098` |
| `primvars/normals` | `VtVec3fArray`, role `normal`; `varying` → linear normal interpolation; absent → implicit camera-facing/basis | optional | `basisCurves.cpp:1084-1098` |
| `primvars/displayColor`, `primvars/displayOpacity` | role `color`; any interpolation | optional (fallback material) | `tokens.h:38-39` |
| `primvars/velocities`, `accelerations` | for renderers with velocity blur (not Storm) | optional | `tokens.h:19,101` |
| `xform/matrix`, `xform/resetXformStack` | `GfMatrix4d`, `bool` | optional (identity) — **world-space** when emitted after flattening | `xformSchema.h:36-38`; rigExec note `<usdrig-src>/libs/rigExecImaging/sceneIndices.cpp:1453-1460` |
| `visibility/visibility` | `bool` | optional | `visibilitySchema.h:36` |
| `purpose/purpose` | `geometry`/`render`/`proxy`/`guide` (Hydra render tag tokens) | optional | `purposeSchema.h:36-38`; skeleton example uses `HdRenderTagTokens->guide` (`dataSourceSkeletonPrim.cpp:96-105`) |
| `extent/min`, `extent/max` | `GfVec3d` | optional | `extentSchema.h:36-38` |
| `materialBindings/""/path` | `SdfPath` to material prim (allPurpose key is the empty token) | needed for a non-fallback material | `materialBindingsSchema.h:36-39`; Storm reads `GetMaterialBinding()` allPurpose after its resolver ran (`dependencySceneIndexPlugin.cpp:87-102`) |
| `displayStyle/refineLevel` | int (curve refinement) | optional | `basisCurves.cpp:290-335,635` |
| `primOrigin` | for pick/selection round-trip to USD | optional | `dataSourcePrim.h:386-400` |

Primvar container contract (`HdPrimvarSchema`, `primvarSchema.h:35-57`): `primvarValue` (flattened) **or** `indexedPrimvarValue` + `indices`; `interpolation` token ∈ {`constant`,`uniform`,`varying`,`vertex`,`faceVarying`,`instance`}; `role` ∈ {`point`,`normal`,`vector`,`color`,`pointIndex`,`edgeIndex`,`faceIndex`,`textureCoordinate`}; optional `colorSpace`, `elementSize`. Builders: `HdPrimvarSchema::Builder().SetPrimvarValue(ds).SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(tok)).SetRole(HdPrimvarSchema::BuildRoleDataSource(tok)).Build()` (`:164-231`; the `Build*DataSource` helpers return shared static token sources). Emulation converts these to `HdPrimvarDescriptor`s per interpolation and warns/skips on an unknown interpolation (`sceneIndexAdapterSceneDelegate.cpp:1785-1852`).

Dirty translation for curves: `basisCurves/topology` → `DirtyTopology`; `primvars/points` → `DirtyPoints`, etc. (`pxr/imaging/hd/dirtyBitsTranslator.cpp:121-123,638-642`), so emitting locator-precise dirties (points only per frame) avoids topology rebuilds.

Retained-value helpers: `HdRetainedContainerDataSource::New(name, ds, ...)` (`retainedDataSource.h:35-95`), `HdRetainedTypedSampledDataSource<T>::New(v)` (`:149-191`), `HdRetainedSmallVectorDataSource` (`:372-400`), `HdCreateTypedRetainedDataSource(VtValue)` (`:408`). For per-frame recomputed values implement `HdTypedSampledDataSource<T>` with `GetValue/GetTypedValue(shutterOffset)` and `GetContributingSampleTimesForInterval` (`pxr/imaging/hd/dataSource.h:196-228`; motion-blur example `TestUsdImagingGLHdGpProcedurals.cpp:443-450`).

### 4.2 instancer (for guide/interpolated hair as instanced prototypes, or clump instancing)

| Data source | Content | Evidence |
|---|---|---|
| prim type | `"instancer"` | `tokens.h:117,315`; emulation inserts `_InsertInstancer` (`sceneIndexAdapterSceneDelegate.cpp:274-275`) |
| `instancerTopology/prototypes` | `VtArray<SdfPath>` of prototype prim (or subtree root) paths | `instancerTopologySchema.h:36-41,64-70,122-123` |
| `instancerTopology/instanceIndices` | vector data source of `VtIntArray`, one per prototype (multiplicity + index into instance-rate arrays) | `:126`; `GetInstanceIndices` uses `ComputeInstanceIndicesForProto` (`sceneIndexAdapterSceneDelegate.cpp:2623-2640`); USD reference builds it with `HdRetainedSmallVectorDataSource` (`usdImaging/dataSourcePointInstancer.cpp:339-378`) |
| `instancerTopology/mask` | `VtBoolArray` (empty = all true) | `:70-72,129` |
| `instancerTopology/instanceLocations` | only for implicit (native) instancing; leave null | `:83-90` |
| `primvars/hydra:instanceTransforms` | `VtArray<GfMatrix4d>` (or `GfMatrix4f`), interpolation `instance` | `tokens.h:124`; `hdSt/instancer.cpp:110-126` |
| `primvars/hydra:instanceTranslations|instanceRotations|instanceScales` | vec3f / quath / vec3f, interpolation `instance` (USD point instancer emits these instead of matrices) | `tokens.h:125-127`; `dataSourcePointInstancer.cpp:249-297` |
| other `instance`-interpolated primvars | any per-instance data (e.g. per-hair color) | `HdInterpolationInstance` mapping `sceneIndexAdapterSceneDelegate.cpp:1730-1731` |
| `xform`, `visibility` | instancer transform/visibility | `dirtyBitsTranslator.cpp:442-447` |

Prototype prims must carry `instancedBy/paths = [<instancerPath>]` (and `prototypeRoots`) — `GetInstancerId` reads `paths[0]` and coding-errors on more than one (`sceneIndexAdapterSceneDelegate.cpp:2665-2690`; `instancedBySchema.h:36-38`). A prim with an instancer id is drawn only through the instancer. In the UsdImaging chain this is authored by the point-instancer prototype propagating scene index; if we emit instancers ourselves (hdGp or our own scene index after usdImaging) we must author `instancedBy` on our prototypes ourselves and keep the prototypes under a path that no other instancer claims. Dirty translation: `instancedBy` → `DirtyInstancer`, `instancerTopology` → `DirtyInstanceIndex`, `primvars` → `DirtyPrimvar` (`dirtyBitsTranslator.cpp:426-455`).

---

## 5. Dependencies schema and dependency forwarding

Schema (`pxr/imaging/hd/dependenciesSchema.h:36-40`, `dependencySchema.h`): prim-level container `__dependencies` whose children are arbitrarily named `HdDependencySchema` entries with `dependedOnPrimPath` (path; **empty = self**), `dependedOnDataSourceLocator`, `affectedDataSourceLocator`. Builder pattern from Storm:

```cpp
// pxr/imaging/hdSt/dependencySceneIndexPlugin.cpp:92-97
HdDependencySchema::Builder()
     .SetDependedOnPrimPath(pathDs)                 // e.g. /Char/Body (mesh)
     .SetDependedOnDataSourceLocator(materialLocDs)  // e.g. primvars/points
     .SetAffectedDataSourceLocator(materialBindingsLocDs) // e.g. primvars/points on our curve
     .Build();
```

`HdDependencyForwardingSceneIndex` (`pxr/imaging/hd/dependencyForwardingSceneIndex.cpp`):
- Registers dependencies **lazily** when a prim is first pulled through `GetPrim` (`:33-41`) and rebuilds them when `PrimsAdded` hits the affected prim or when `__dependencies` itself is dirtied (`:84-92`, `:319-346`). It blocks `__dependencies` from downstream consumers (`:43-55`).
- On `PrimsDirtied` of a depended-on prim with a locator intersecting `dependedOnDataSourceLocator`, it appends a dirtied entry `(affectedPrim, affectedLocator)` (`:363-399+`) and forwards everything (`:352-360`). Transitive chains are handled via the visited set (`:315-331`).
- Present in usdview's Storm chain: `HdSt_DependencyForwardingSceneIndexPlugin`, `loadWithRenderer: "GL"`, phase 1000, `InsertionOrderAtEnd` (`dependencyForwardingSceneIndexPlugin.cpp:26-36`; `hdSt/plugInfo.json:33-43`), preceded by `HdSt_DependencySceneIndexPlugin` (phase 100) which adds material/volume dependencies (`dependencySceneIndexPlugin.cpp:44-57`). It is **not** part of the UsdImaging chain (`sceneIndices.cpp:191-325` contains no forwarding index); other renderers must add their own (hdPrman is **UNVERIFIED** here).

Answer to the plan's question: yes — curve prims we emit anywhere upstream of Storm's phase-1000 plugin can declare `__dependencies` entries against the deformed surface mesh's `primvars/points` (and `xform`) and Storm's chain will dirty our curves' `primvars/points` when rigExec's results scene index dirties the mesh points. Two caveats: (1) forwarding only dirties; our points data source must lazily re-pull the mesh points from its input scene on the next `GetValue` (or key a cache on a version counter bumped by our own observer); (2) the dependency is only known after the first `GetPrim` of our curve reached the forwarding index (fine after the first frame; for a brand new prim Storm pulls it during sync anyway). For hdGp procedurals the resolver's `DependencyMap` already recooks on mesh-point dirties and the procedural pushes `outputDirtiedPrims` itself (`TestUsdImagingGLHdGpProcedurals.cpp:239-252,341-361`), so `__dependencies` is a complementary mechanism mostly useful for non-hdGp emitters (e.g. a retained results scene index like rigExec's) or for cheap per-frame point invalidation without recooking.

---

## 6. Codeless vs generated schemas — recommendation

### 6.1 Mechanics

- Codeless: `bool skipCodeGeneration = true` in `/GLOBAL` `customData`; `usdGenSchema` then emits only `generatedSchema.usda` + `plugInfo.json` (`pxr/usd/usd/usdGenSchema.py:195-209,241-247`; docs `generatingSchemas.md:670-703`). Any module that includes a codeless module is also codeless (`:673-677`). `UsdSchemaRegistry` registers types from plugInfo `Types` entries with `schemaKind`/`schemaIdentifier`/`bases` (`pxr/usd/usd/schemaRegistry.cpp:75-76,112-209`), so `prim.IsA(TfType::FindByName("UsdGenX"))`, `GetPrimTypeInfo().GetSchemaTypeName()`, applied API schema lookup, fallbacks/allowedTokens and derived-type queries all work without code.
- usdRig practice: `libs/rigExecSchema/schema.usda:18-26` (`libraryName rigExecSchema`, `libraryPrefix RigExec`, `skipCodeGeneration = true`); `bin/gen_schema.sh` runs `usdGenSchema` into `plugin/rigExecSchema/resources` and strips `LibraryPath`/rewrites `ResourcePath`/`Root` to `"."` so Plug accepts a code-free plugin; `CMakeLists.txt:409-475` regenerates a `"Type": "library"` variant pointing `LibraryPath` at `rigExecImaging` solely so `implementsComputeExtent` can be honoured. Imaging then registers `UsdGeomBoundable` compute-extent functions in `libs/rigExecImaging/registry.cpp:1127-1131`. The imaging plugin has its own generated `plugInfo.json.in` with the `UsdImagingSceneIndexPlugin` type (`plugin/rigExecImaging/resources/plugInfo.json.in`; `CMakeLists.txt:492-518`).
- Generated: full C++ classes + tokens + Python wraps (`pxr/usd/usdProc/{generativeProcedural.h,tokens.h,wrap*.cpp}`), requiring a build against the installed USD (the `pxr_library` macros are not installed; a consumer would hand-write CMake for the generated files, which is what usdRig avoids).

### 6.2 Trade-offs for the hair plugin

| Concern | Codeless | Generated C++ |
|---|---|---|
| Iteration speed on ~40 operator types | Edit `schema.usda`, rerun `gen_schema.sh`, no compile | Regenerate + recompile + Python wrap |
| Imaging adapter needs | Adapter keys off identifier string; `UsdImagingDataSourceMapped` builds mappings from attribute names (`GetSchemaAttributeNames` is the typed convenience; for codeless, iterate `UsdPrimDefinition::GetPropertyNames()` — **UNVERIFIED** API name, check `pxr/usd/usd/primDefinition.h`) | Typed accessors (`GetProceduralSystemAttr()`, `generativeProceduralAdapter.cpp:338`) |
| Python authoring/tools | Generic `UsdPrim` API; rigExec wraps this in `rigexec.schema` and `Builder` (README "Authoring from Python") | `pxr.UsdGen.ClumpStyler(prim).CreateRadiusAttr()` |
| Token safety | Hand-maintained token header (rigExec: `libs/rigExecRigging/schemaAuthoring.cpp`) | `UsdGenTokens` generated |
| Consumers outside the plugin (DCCs) | Load plugin path only | Need the library |
| Versioning | Schema versioning via plugInfo works; `includeDerivedPrimTypes` in imaging plugInfo covers subclasses | same |

**Recommendation:** codeless schema for all `UsdGen*` prim and API types (mirrors usdRig, reuses its `gen_schema` tooling and `Builder` pattern), plus a small hand-written C++ header of tokens and a `UsdGenSchemaInfo` helper that reads property definitions from `UsdSchemaRegistry`/`UsdPrimDefinition` at startup to build `UsdImagingDataSourceMapped::PropertyMappings` generically (one adapter class serving every operator type via `includeDerivedPrimTypes: true` on a common abstract base such as `UsdGenOperator`). Generate C++ only if a DCC integration later demands typed C++ API.

---

## 7. 26.08-specific gotchas

1. Scene-index mode is the default and the Hydra 1.0 `UsdImagingDelegate` path is deprecated with a warning when disabled (`engine.cpp:78-84`); `HD_ENABLE_SCENE_INDEX_EMULATION` env vars were removed and Hydra 1.0 classes (`HdxTaskController`, engine methods) marked deprecated (`CHANGELOG.md:254-269`). Do not implement Hydra 1.0 adapter methods; derive from `UsdImagingSceneIndexPrimAdapter`.
2. Storm still consumes via emulation (`HdSceneIndexAdapterSceneDelegate`); the renderer is created through `HdRendererPlugin::CreateRenderer(sceneIndex, HdRendererCreateArgsSchema)` (`rendererPlugin.h:94-96`, `engine.cpp:1789-1791`), falling back to `HdRenderDelegateAdapterRenderer` (`rendererPlugin.cpp:139-157`). Prim types not in rprim/sprim/bprim/instancer/geomSubset/task are silently ignored (`sceneIndexAdapterSceneDelegate.cpp:268-286`).
3. `HDGP_INCLUDE_DEFAULT_RESOLVER` is off; usdview does not enable it; async needs `allowAsynchronousSceneProcessing` (off by default in usdview, `stageView.py:944`).
4. `UsdImagingSceneIndexPlugin` ordering is `std::set<TfType>` iteration (`sceneIndexPlugin.cpp:45-55`), and `TfType::operator<` compares `_info` pointers (`pxr/base/tf/type.h:119`): the relative order of rigExec's plugin, UsdSkel's, and a new one is not specifiable. All of them run after prototype propagation/flattening (`sceneIndices.cpp:286-302`), so prims they emit must be world-space and instancing-aware (rigExec's own note at `sceneIndices.cpp:1453-1460`). To be reliably after rigExec, insert via `HdSceneIndexPluginRegistry` (renderer chain, e.g. `loadWithRenderer: ""`/`"GL"`, phase 2 with `"ordering": {"after": "RigExec..."}` is not applicable since rigExec is not there; simply any phase ≥ 0 in the renderer chain is after the whole UsdImaging chain). Downside: the renderer chain is built per renderer (`engine.cpp:1758-1780`) and cannot use `UsdImagingSceneIndexPlugin::FlattenedDataSourceProviders`/`InstanceDataSourceNames` (`sceneIndexPlugin.h:58-92`).
5. The engine wraps a `HdNoticeBatchingSceneIndex` after instancing (`sceneIndices.cpp:286-287`) and Storm's chain is followed by an optional `HdCachingSceneIndex` (`HD_ENABLE_TERMINAL_CACHING_SCENE_INDEX`/`USDIMAGINGGL_ENGINE_ENABLE_CACHING_SCENE_INDEX`, default off, `renderIndex.cpp:54-55`, `engine.cpp:86-88`). Since no caching is on by default, every data source pull hits our code — cache computed arrays inside our scene index/procedural.
6. The terminal scene index is registered by name `"[Terminal SI] <instanceName>"` in `HdSceneIndexNameRegistry` (`sceneIndexAdapterSceneDelegate.cpp:160-165`), which is what usdview's Hydra Scene Browser uses; our plugin should `SetDisplayName` on its scene indices for debuggability (`pxr/imaging/hd/sceneIndex.h:151`).
7. `UsdImagingDataSourceAttribute` evaluates at `stageGlobals.GetTime()` — a scene index downstream cannot change time per prim; motion blur samples come from `GetContributingSampleTimesForInterval` (`dataSourceAttribute.h:71-126`).
8. Emitting prims from an adapter (inside the stage scene index) is the only place where local `xform` and inherited visibility/purpose/material bindings are resolved for free; everything after `UsdImagingNiPrototypePropagatingSceneIndex` sees flattened values.
9. `HdGpGenerativeProceduralFilteringSceneIndex` exists to allow/skip procedural types per pipeline stage (`generativeProceduralFilteringSceneIndex.h:187-234`, `SetAllowedProceduralTypes` added per `CHANGELOG.md:900`).

---

## 8. Concrete pattern sketches derived from the evidence

**A. Operator prims on stage → Hydra (adapter route).** plugInfo `Types`: `"UsdGenImagingOperatorAdapter": {"bases": ["UsdImagingSceneIndexPrimAdapter"], "primTypeName": "UsdGenOperator", "includeDerivedPrimTypes": true}` (keys per `adapterRegistry.cpp:104-147`). `GetImagingSubprims → {""}`; `GetImagingSubprimType → TfToken("usdGenOperator")`; `GetImagingSubprimData → HdOverlayContainerDataSource::New(UsdImagingDataSourcePrim::New(...), HdRetainedContainerDataSource::New(TfToken("usdGen"), UsdImagingDataSourceMapped::New(prim, path, mappings, globals)))` (pattern `dataSourceSkeletonPrim.cpp:84-90`); `InvalidateImagingSubprim → UsdImagingDataSourceMapped::Invalidate ∪ UsdImagingDataSourcePrim::Invalidate` (`:114-129`). Adding/removing an operator prim is a resync (`stageSceneIndex.cpp:598-720`) that the downstream graph scene index sees as `PrimsRemoved/PrimsAdded`.

**B. Generating curves.** Either (B1) an hdGp procedural whose `UpdateDependencies` declares `{surfaceMeshPath: {primvars/points, xform}, operatorPrimPaths: {usdGen}}` and whose `Update` returns `<proc>/curves` (type `basisCurves`) and `<proc>/guides` (type `basisCurves`, purpose guide), dirtying `primvars/points` per recook (`TestUsdImagingGLHdGpProcedurals.cpp:239-252,341-361`); host it with our own `HdGpGenerativeProceduralResolvingSceneIndex::New(input, TfToken("usdGenProcedural"))` inside an `HdSceneIndexPlugin` so it is always after rigExec and independent of `HDGP_INCLUDE_DEFAULT_RESOLVER`. Or (B2) a rigExec-style retained results scene index (`HdRetainedSceneIndex::AddPrims/RemovePrims/DirtyPrims`, `pxr/imaging/hd/retainedSceneIndex.h:26-65`) merged with `HdMergingSceneIndex` (first input = strongest, `hydra-integration-notes.md §1.2`), publishing `basisCurves` prims with `__dependencies` on the mesh points for Storm's forwarding index.

**C. Static curves deformed by a surface.** Curves are authored `BasisCurves` prims on stage (free adapter, `basisCurvesAdapter.cpp:50-94`); a filtering scene index downstream of rigExec overlays `primvars/points` with a lazily evaluated `HdVec3fArrayDataSource` that reads the deformed mesh points from its input on each pull and declares `__dependencies` → Storm dirties `primvars/points` when the mesh dirties (§5). Locator-precise dirtying keeps Storm at `DirtyPoints` (`dirtyBitsTranslator.cpp:121-123`).

---

## Key facts

- The default hdGp resolver is registered for all renderers at insertion phase 2 but only enabled when `HDGP_INCLUDE_DEFAULT_RESOLVER` is set; usdview never sets it. (`pxr/imaging/hdGp/sceneIndexPlugin.cpp:83-124`; grep of `pxr/usdImaging/usdviewq`, `usdImagingGL` has no non-test reference)
- Renderer scene-index plugins are appended after the entire UsdImaging chain (including `UsdImagingSceneIndexPlugin`s like rigExec's) by `UsdImagingGLEngine::_CreateSceneIndexChainAndRenderer`; the engine builds that chain before inserting the UsdImaging scene index, and `HdMergingSceneIndex::InsertInputScenes` replays added prims so hdGp sees them. (`engine.cpp:1528-1544,1758-1780`; `mergingSceneIndex.cpp:182-211`)
- A resolver instance can be created with a custom target prim type: `HdGpGenerativeProceduralResolvingSceneIndex::New(input, TfToken)`; procedural implementation is chosen by primvar `hdGp:proceduralType` matched against plugin `displayName`. (`generativeProceduralResolvingSceneIndex.h:57-63`, `.cpp:696-722`; `generativeProceduralPluginRegistry.cpp:150-172`)
- The resolver caches only child types/hierarchy; `GetPrim` of a generated path calls `GetChildPrim` every time; children must be under the procedural path; procedurals are not recursive and all see the same input. (`generativeProceduralResolvingSceneIndex.cpp:58-86,1041-1059`; `.h:39-42`)
- Cooking happens synchronously in notice handlers, in parallel across ≥ 2 procedurals with `WorkParallelForEach`; per instance `Update`/`UpdateDependencies` are serialized while `GetChildPrim` must be thread-safe. (`.cpp:277-299,611-639`; `generativeProcedural.h:56-57,97-98,109`)
- Any dirty on the procedural prim itself or on a declared dependency (locator-set intersection) forces a recook with a precise `dirtiedDependencies` map; `__childNames` tracks child add/remove of a prim. (`.cpp:553-669,229-267`; `generativeProcedural.cpp:31-36`)
- Async procedurals are polled through `asyncPoll` system messages sent by `UsdImagingGLEngine::PollForAsynchronousUpdates`; usdview polls every 100 ms only when `allowAsynchronousSceneProcessing` is on (default off). (`engine.cpp:1497-1500,2648-2655`; `stageView.py:944,966`; `appController.py:524-527,5515-5519`)
- A prim adapter for a custom schema is discovered via plugInfo keys `primTypeName` (= schema identifier), `isInternal`, `includeDerivedPrimTypes`, `includeSchemaFamily`; API schema adapters via `apiSchemaName`. (`adapterRegistry.cpp:42-147,297-323`; `adapterManager.cpp:170-172`)
- `UsdImagingSceneIndexPrimAdapter` is the Hydra-2.0-only base (four pure scene-index virtuals; Hydra 1.0 virtuals are `final` no-ops). (`sceneIndexPrimAdapter.h:14-112`)
- The usdProc adapter returns `UsdImagingDataSourcePrim` as data source and uses the `__usdStageSceneIndexRepopulate` locator to force a resync when the Hydra type would change. (`generativeProceduralAdapter.cpp:63-105`; `tokens.h:44`; `stageSceneIndex.cpp:855-870`)
- Prims without an adapter are still populated with empty type and a `UsdImagingDataSourcePrim` data source; only `xform/visibility/purpose/extent/primvars` are exposed and invalidated; custom attributes are neither exposed nor dirtied. (`adapterManager.cpp:71-102,205-225`; `dataSourcePrim.cpp:693-812,864-911`)
- `rel primvars:<name>` becomes a constant primvar holding `VtArray<SdfPath>` — the usdProc convention for relationship parameters. (`dataSourcePrimvars.cpp:167-174`)
- `UsdImagingDataSourceMapped` + `UsdImagingDataSourceAttributeNew` give sampled data sources, time-varying flagging and locator invalidation for arbitrary attributes/relationships of a codeless schema. (`dataSourceMapped.h:43-193`; `dataSourceAttribute.h:41-126,225-243`; `dataSourceSkeletonPrim.cpp:23-45,84-90,114-129`)
- USD edits reach Hydra when the engine calls `ApplyPendingUpdates` in `_PreSetTime` during `PrepareBatch`; prim-level metadata changes only resync for plugin fields. (`engine.cpp:468-498,2354-2386`; `stageSceneIndex.cpp:552-566`)
- A valid `basisCurves` prim needs type `basisCurves`, `basisCurves/topology/curveVertexCounts`, and `primvars/points` (vertex, role point); `basis` defaults to bezier and `type` to linear; widths/normals with `varying` interpolation switch Storm to linear interpolation. (`sceneIndexAdapterSceneDelegate.cpp:865-901`; `hdSt/basisCurves.cpp:1083-1098`; `tokens.h:25-77`)
- A valid `instancer` needs `instancerTopology/{prototypes,instanceIndices[,mask]}` and `instance`-interpolated primvars (`hydra:instanceTransforms` or T/R/S); prototypes must carry `instancedBy/paths=[instancer]`. (`instancerTopologySchema.h:36-132`; `hdSt/instancer.cpp:110-126`; `sceneIndexAdapterSceneDelegate.cpp:2623-2690`)
- Storm's plugin chain includes `HdSt_DependencySceneIndexPlugin` (phase 100) and `HdSt_DependencyForwardingSceneIndexPlugin` (phase 1000, GL only) which honour `__dependencies` declared by any upstream prim; dependencies are discovered lazily on first `GetPrim`. (`dependencyForwardingSceneIndexPlugin.cpp:26-36`; `dependencyForwardingSceneIndex.cpp:33-58,487-570`; `hdSt/plugInfo.json:33-43`)
- No flattening happens after the UsdImaging chain's `UsdImagingNiPrototypePropagatingSceneIndex`; anything emitted later (UsdImagingSceneIndexPlugins, hdGp, renderer plugins) must author world-space `xform/matrix`. (`niPrototypePropagatingSceneIndex.cpp:196-206`; `sceneIndices.cpp:286-302`; `<usdrig-src>/libs/rigExecImaging/sceneIndices.cpp:1453-1460`)
- `UsdImagingSceneIndexPlugin`s are instantiated in `std::set<TfType>` order (pointer comparison), so the relative order of rigExec's and another such plugin is undefined. (`sceneIndexPlugin.cpp:45-55`; `pxr/base/tf/type.h:119`; `<usdrig-src>/libs/rigExecImaging/sceneIndexPlugin.cpp:23-45`)
- Codeless schemas (`skipCodeGeneration = true`) emit only `generatedSchema.usda` + `plugInfo.json`; usdRig regenerates a "library"-typed plugInfo pointing at rigExecImaging solely for `implementsComputeExtent`. (`usdGenSchema.py:195-247`; `generatingSchemas.md:689-703`; `usdRig/bin/gen_schema.sh`; `usdRig/CMakeLists.txt:409-475`)
- 26.08 removed `HD_ENABLE_SCENE_INDEX_EMULATION`, introduced `HdRendererPlugin::CreateRenderer`/`HdRendererCreateArgsSchema`, and deprecates Hydra 1.0 engine APIs; Storm still runs through `HdRenderDelegateAdapterRenderer` + `HdSceneIndexAdapterSceneDelegate`, ignoring unknown prim types. (`CHANGELOG.md:254-269`; `rendererPlugin.cpp:139-157`; `renderIndex.cpp:223-230`; `sceneIndexAdapterSceneDelegate.cpp:268-286`)

## Open questions

- Whether hdPrman (or other production delegates) include a dependency-forwarding scene index and an hdGp resolver in their own chains in 26.08 (`CHANGELOG.md:2188` says hdPrman removed its own hdGp resolver in an earlier release) — not checked; only Storm's `plugInfo.json` was read.
- Exact API to enumerate property definitions of a codeless type for building `UsdImagingDataSourceMapped` mappings generically (`UsdPrimDefinition::GetPropertyNames()` presumed) — not verified in `pxr/usd/usd/primDefinition.h`.
- Whether `HdSceneIndexPluginRegistry` JSON `"ordering"` can reference a `UsdImagingSceneIndexPlugin` (it cannot, different registry) — the practical consequence is that "after rigExec" can only be guaranteed by using the renderer chain; whether rigExec would accept a later change to expose an ordering hook is a design question for the plan.
- `HdMergingSceneIndex::InsertInputScenes` replay of existing prims was confirmed by structure (`mergingSceneIndex.cpp:182-211`) but the full function body was not read line-by-line.
- Storm's default width when `primvars/widths` is absent (a constant fallback value in `HdStBasisCurves`) — the fallback mechanism exists (`basisCurves.cpp:772-778`) but the numeric default was not verified.
- Performance of `HdDependencyForwardingSceneIndex` with tens of thousands of curve prims each declaring a dependency on the same mesh (the header notes fan-out concerns, `dependencyForwardingSceneIndex.h:72-90,157-164`); a single instancer/aggregate curve prim per groom is likely required but not measured.
- The hdGp GL test is not registered in 26.08's `usdImagingGL/CMakeLists.txt`; whether `PXR_HDGP_TEST_PLUGIN_PATH`/`HDGP_INCLUDE_DEFAULT_RESOLVER` are exercised elsewhere in CI is unknown.
- No Python bindings exist for hdGp; whether the usdview tooling needs to drive procedurals from Python (it likely should go through stage edits or a C API like rigExec's `RigExecImaging_Activate`) is an architecture decision.
