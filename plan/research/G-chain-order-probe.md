# G — chain-order probe: where does usdGen insert, and what does it see?

**Everything below was measured on this machine, headless, with no GL context.**
Probe sources: `/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/probes/G-chain-order/`
(`src/probeChainOrder.cpp`, `src/stub.cpp`, `src/stubRenderer.cpp`, `stubA|stubB|stubR/resources/plugInfo.json`,
`stages/skelAndRig.usda`, `CMakeLists.txt`). Raw run transcript: `.../probes/G-chain-order/logs/transcript.txt`.
Built with `cmake -G Ninja -DCMAKE_PREFIX_PATH=/home/burkard/work/OpenUSD_26_08`; three stub shared libraries
(`libusdGenStubA.so`, `libusdGenStubB.so`, `libusdGenStubRenderer.so`) plus `probeChainOrder`.
Nothing under `/home/burkard/work/usdRig` or `/home/burkard/work/OpenUSD` was modified.

---

## 1. What the probe is

`probeChainOrder` does what `usdRig/tests/probeImagingPipeline.cpp:198-207` does — calls
`UsdImagingCreateSceneIndices` and lets plugins insert themselves — and then adds five things:

| flag | what it does |
|---|---|
| (default) | prints the `std::set<TfType>` iteration order for `UsdImagingSceneIndexPlugin` and the registered `HdSceneIndexPlugin` types |
| `--stage` | builds the real chain and walks it terminal→upstream via `TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>` + `GetInputScenes()` + `GetDisplayName()` |
| `--rigexec` / `--time N` | `RigExecImaging_Activate` / `_SetTime` through the same C surface usdview uses; wall-clock loop |
| `--notices` | attaches an `HdSceneIndexObserver` to the terminal and prints every dirty locator emitted by one `SetTime` |
| `--renderindex --renderer <id>` | creates a display-named render delegate through `HdRendererPluginRegistry`, builds `HdRenderIndex::New`, `InsertSceneIndex`es the UsdImaging terminal, and walks `GetTerminalSceneIndex()` |
| `--bake` / `--comp` | wraps the terminal in `HdSiExtComputationPrimvarPruningSceneIndex`; dumps `extComputation/inputValues/restPoints` |

`stub.cpp` is compiled twice (`-DSTUB_TAG_ID=A|B`) and each copy registers **both** a
`UsdImagingSceneIndexPlugin` (`UsdGenStubUiA/B`) and an `HdSceneIndexPlugin` (`UsdGenStubHdA/B`,
`loadWithRenderer: ""`). Both append a recording `HdSingleInputFilteringSceneIndexBase` that sets its own
display name, so a chain walk shows exactly where each landed.

---

## 2. UsdImagingSceneIndexPlugin order is unspecified and path-order sensitive

`UsdImagingCreateSceneIndices` appends plugin scene indices in one place —
`_AddPluginSceneIndices` at `/home/burkard/work/OpenUSD/pxr/usdImaging/usdImaging/sceneIndices.cpp:68-78`,
called at `:302`, i.e. **after** `UsdImagingMaterialBindingsResolvingSceneIndex` (`:298`) and **before**
`UsdImagingSelectionSceneIndex` (`:305`). The iteration order comes from
`UsdImagingSceneIndexPlugin::GetAllSceneIndexPlugins`
(`/home/burkard/work/OpenUSD/pxr/usdImaging/usdImaging/sceneIndexPlugin.cpp:50-54`), which fills a
`std::set<TfType>` from `PlugRegistry::GetAllDerivedTypes`. `TfType::operator<` compares the private
`_TypeInfo *_info` pointer (`/home/burkard/work/OpenUSD/pxr/base/tf/type.h:119` and `:728`) — a heap address,
not a name. **There is no ordering API and no ordering metadata at this level.**

Measured sweep of all six orderings of the three plugin-path entries (rigExecImaging, stubA, stubB):

| `PXR_PLUGINPATH_NAME` order | resulting append order (first appended → innermost) |
|---|---|
| RIG, A, B | RIG, A, B, SKEL |
| RIG, B, A | RIG, **B, A**, SKEL |
| A, RIG, B | RIG, A, B, SKEL |
| A, B, RIG | RIG, A, B, SKEL |
| B, RIG, A | RIG, **B, A**, SKEL |
| B, A, RIG | RIG, **B, A**, SKEL |

Two facts fall out:

* **Swapping two plugin-path entries flips the append order of the two plugins they carry** (rows 1 vs 2).
  Same binary, same stage, only the environment variable reordered.
* The order is **not** alphabetical by type name (`UsdGenStubUiB` before `UsdGenStubUiA` in three rows),
  not alphabetical by plugin `Name`, and — for RigExec vs the stubs — not path order either. RigExec sorted
  first and `UsdSkelImagingResolvingSceneIndexPlugin` sorted last in **all six** permutations; why those two
  are pinned while A/B track path order is **UNEXPLAINED** (consistent with `_TypeInfo` allocation landing in
  different malloc bins, but not verified). Do not build on it.

Eight consecutive runs with an identical environment produced an identical order — so it is *reproducible*
within a fixed environment, and *unstable* across environments. That is the worst combination: it will look
fine in dev and reorder itself on a different machine, a different install layout, or after any third party
adds a fourth `UsdImagingSceneIndexPlugin`. Stock OpenUSD 26.08 has exactly two subtypes; usdGen would be the
third, and the RigExec plugin already makes it three in the target deployment.

Actual chain, `06_LatticeBulge.usda`, with rigExec + both stubs (terminal first):

```
 0 UsdImagingRenderSettingsFlatteningSceneIndex
 1 UsdImagingSelectionSceneIndex
 2 UsdSkelImagingPointsResolvingSceneIndex          <- UsdSkelImaging plugin
 3 UsdSkelImagingSkeletonResolvingSceneIndex
 4 UsdGenStub_RecordingSceneIndex ["UsdGenStubUiB"] <- stub B plugin
 5 UsdGenStub_RecordingSceneIndex ["UsdGenStubUiA"] <- stub A plugin
 6 RigExecResultsSceneIndex                          <- RigExec plugin
 7 RigExecBindingResolvingSceneIndex
 8 RigExecInternalPrimPruningSceneIndex
 9 UsdImagingMaterialBindingsResolvingSceneIndex
10 UsdImaging_InstanceProxyPathTranslationSceneIndex
11 HdNoticeBatchingSceneIndex
12 UsdImagingNiPrototypePropagatingSceneIndex
13 UsdImagingPiPrototypePropagatingSceneIndex
14 UsdImagingExtentResolvingSceneIndex
15 HdsiLocatorCachingSceneIndex [material]
16 UsdImagingStageSceneIndex
```

Note that in this (default) arrangement a usdGen plugin at slot 4/5 sits **upstream of UsdSkelImaging** —
i.e. it would read *unskinned* points off every skinned mesh and silently groom the bind pose.

---

## 3. Renderer-level `HdSceneIndexPlugin` lands after the whole UsdImaging chain, deterministically

`HdRenderIndex`'s constructor calls
`HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer` at
`/home/burkard/work/OpenUSD/pxr/imaging/hd/renderIndex.cpp:209-214`, **guarded by
`if (!rendererDisplayName.empty())` at `:208`**. That display name can only be set by
`HdRendererPlugin::CreateDelegate` (`hd/rendererPlugin.cpp:76-78`); `_SetRendererDisplayName` is private with
`friend class HdRendererPlugin` (`hd/renderDelegate.h:584-589`). **A render delegate an application news up
itself has an empty display name and gets NO renderer-level scene index plugins.** The probe therefore
registers a real (null) `HdRendererPlugin` via plugInfo and goes through `GetOrCreateRendererPlugin`.

Result (`--renderindex --renderer UsdGenStubRendererPlugin`, terminal first):

```
 0 UsdGenStub_RecordingSceneIndex ["UsdGenStubHdB"]      <- renderer-level plugin
 1 UsdGenStub_RecordingSceneIndex ["UsdGenStubHdA"]      <- renderer-level plugin
 2 HdNoticeBatchingSceneIndex ["Post-Merging Notice Batching Scene Index"]
 3 HdMergingSceneIndex
     4 HdLegacyGeomSubsetSceneIndex / 5 batching / 6 HdLegacyPrimSceneIndex
     7..23  the ENTIRE UsdImaging chain from §2, unchanged
```

`render delegate display name = "UsdGenStubRenderer"` was printed, confirming the guard was passed.

This is structurally guaranteed, not luck: the UsdImaging chain is an *input* to `HdMergingSceneIndex`
(`renderIndex.cpp:196-198` + `HdRenderIndex::InsertSceneIndex` at `:314-335`), and renderer plugins are
appended *on top of* the merge. `UsdImagingGLEngine` — what usdview actually uses — does the same thing with
its own merging index: `AppendSceneIndicesForRenderer` at
`/home/burkard/work/OpenUSD/pxr/usdImaging/usdImagingGL/engine.cpp:1776-1779`, and the UsdImaging chain is
inserted into that merge at `engine.cpp:1544`. (`USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX` defaults **true**,
`engine.cpp:78`.)

**Renderer-level ordering is controllable and path-order stable.** Swapping stubA/stubB in
`PXR_PLUGINPATH_NAME` did **not** change the renderer-level order (B outermost both times), unlike §2. And
adding `"ordering": {"after": ["UsdGenStubHdB"]}` to stubA's plugInfo flipped A to outermost, again for both
path orders. The default ordering policy is `Hybrid`
(`hd/sceneIndexPluginRegistry.cpp:40-48`, env `HD_SCENE_INDEX_PLUGIN_ORDERING_POLICY_DEFAULT`), which honours
JSON `tags`/`ordering`/`position` (`hd/sceneIndexPlugin.h:47-67`) as well as the C++
`RegisterSceneIndexForRenderer` phase/order arguments. `loadWithRenderer: ""` means "all renderers"
(`hd/sceneIndexPluginRegistry.cpp:1364-1368`); the probe's stubs loaded under a renderer named
`UsdGenStubRenderer`, so they would equally load under Storm (`displayName "GL"`,
`pxr/imaging/plugin/hdStorm/plugInfo.json:10`) and hdPrman. `loadWithApps` additionally allows a
usdview-only plugin (`hd/sceneIndexPluginRegistry.cpp:1317-1325`).

Renderer-level plugins that are registered but disabled do not appear (HdGp and hdsiDebugging were absent —
both gate on env settings in `_IsEnabled`), so the mechanism also gives a clean on/off switch.

---

## 4. The two-modifier case: a mesh that is both UsdSkel-skinned and RigExec-moved

Test stage `stages/skelAndRig.usda` sublayers the unmodified `usdRig/examples/06_LatticeBulge.usda`, retypes
`/LatticeAsset/Geom` to `SkelRoot`, adds a one-joint `Skeleton`+`SkelAnimation`, and applies `SkelBindingAPI`
to `/LatticeAsset/Geom/Slab` — which the example's `RigExecLatticeMover` already drives via
`rel rigExec:moves = </LatticeAsset/Geom/Slab.points>` (`examples/06_LatticeBulge.usda:67`).
A `SkelRoot` is mandatory: skinning is gated on `_hasSkelRoot`
(`usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:1101-1106`).

### 4a. Points at the terminal are BLOCKED, not retained

| stage | `primvars:points` at terminal | `extComputationPrimvars` |
|---|---|---|
| rig only (`06_LatticeBulge`) | container, n=10 (RigExec's published points) | absent |
| rig + skel | **blocked → `Get` returns nullptr** | `points` |
| skel only (rig not activated) | **blocked** | `points` |

`UsdSkelImagingDataSourceResolvedPointsBasedPrim::Get` intercepts the `primvars` token and returns a
*static retained container holding only blocked `points` and `normals`*
(`usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:501-508`, builder at `:290-304`) — it does **not**
overlay the input primvars there. Downstream, `HdOverlayContainerDataSource` resolves the block to null.
`velocities`/`accelerations` are separately blocked by RigExec itself
(`usdRig/libs/rigExecImaging/sceneIndices.cpp:1357-1363`), and those come out null too — so the "blocked"
convention is used by both parties and it works.

**Consequence for usdGen:** a curve generator that reads `primvars/points` off a skinned surface, placed
anywhere downstream of `UsdSkelImagingPointsResolvingSceneIndex`, gets **nothing**. Skinning is not deferred
by default — `HD_ENABLE_DEFERRED_SKINNING` defaults false (`hd/skinningSettings.cpp:15-34`) — so the skinned
points live only in `extComputationPrimvars:points`, produced by the
`skinningPointsComputation` / `skinningPointsInputAggregatorComputation` child prims.

### 4b. `HdSiExtComputationPrimvarPruningSceneIndex` restores them, headlessly

Wrapping the terminal in `HdSiExtComputationPrimvarPruningSceneIndex`
(`/home/burkard/work/OpenUSD/pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:19-33`) turns the
computed primvar back into an ordinary authored primvar, executing the CPU kernel on pull. Measured: with
`--bake`, `primvars:points` reappears as a container with n=10 and the values track the animation
(`p[0] = (-0.2000,0,0.3) → (-0.2269,…) → (-0.2562,…)` for t=1001/1012/1024). No render delegate, no GL.
This scene index is **not** in any stock chain (only referenced in a comment at
`usdSkelImaging/dataSourceResolvedSkeletonPrim.cpp:93`) — Storm consumes ExtComputations natively — so usdGen
should instantiate it **privately, for its own reads**, not splice it into the main chain, or the whole scene
loses GPU skinning. One wart to expect: the baked container's `GetNames()` listed `points` **twice**.

### 4c. **A real interop bug: RigExec upstream of UsdSkelImaging produces a FROZEN skin**

With RigExec at slot 6 and UsdSkelImaging at slots 2-3, `RigExecImaging_SetTime` no longer changes the
skinned result:

```
--- extComputation inputValues at .../skinningPointsInputAggregatorComputation ---
  t=1001.0  restPoints n=10  [0]=(-0.2000,0.0000,0.3000)
  t=1012.0  restPoints n=10  [0]=(-0.2000,0.0000,0.3000)   <- should be -0.2269
  t=1024.0  restPoints n=10  [0]=(-0.2000,0.0000,0.3000)   <- should be -0.2562
```

It is not a missing notice. `--notices` shows RigExec's dirty *is* correctly translated by UsdSkelImaging:

```
/LatticeAsset/Geom/Slab : ...,primvars/__containerDataSource,primvars/points/primvarValue,...
/LatticeAsset/Geom/Slab/skinningPointsInputAggregatorComputation : extComputation/inputValues/restPoints
/LatticeAsset/Geom/Slab/skinningPointsComputation : extComputation/dispatchCount,extComputation/elementCount
/LatticeAsset/Geom/Slab : primvars/points/primvarValue
```

It is a **stale captured container**. Building the chain at three different activation times gives three
different (correct) values, proving the pull path itself works:

| `--start` (activation time, then read with no `SetTime`) | `restPoints[0]` |
|---|---|
| 1001 | (-0.2000, 0, 0.3000) |
| 1012 | (-0.2269, 0, 0.3000) |
| 1024 | (-0.2562, 0, 0.3000) |

Mechanism, all cited: `UsdSkelImagingDataSourceResolvedPointsBasedPrim` stores
`HdPrimvarsSchema const _primvars` (`dataSourceResolvedPointsBasedPrim.h:245`) captured when
`_AddResolvedPrim` ran (`pointsResolvingSceneIndex.cpp:611-631`); `restPoints` reads through it
(`dataSourceResolvedExtComputationPrim.cpp:108-111` → `GetPoints()` → `_primvars.GetPrimvar(points)`).
`RigExecResultsSceneIndex::GetPrim` builds a **fresh retained snapshot container per call**
(`usdRig/libs/rigExecImaging/sceneIndices.cpp:1341-1356`, `HdRetainedTypedSampledDataSource<VtVec3fArray>`),
so the captured one never changes. The only path that rebuilds the resolved prim is
`pointsResolvingSceneIndex.cpp:180-181`, reached when `_ProcessDirtyLocators` returns true — and the relevant
test is `dirtyLocators.Contains(HdPrimvarsSchema::GetDefaultLocator())` at
`dataSourceResolvedPointsBasedPrim.cpp:1178-1180`, i.e. the **bare `primvars` locator**. RigExec sends
`primvars/__containerDataSource` and `primvars/points/primvarValue`, which are *children* of `primvars`, so
`Contains()` is false and no rebuild happens.

**Verified fix, measured.** A four-line filtering scene index inserted between RigExec and UsdSkelImaging that
adds the bare `primvars` locator whenever a dirty intersects the primvars subtree
(`src/stub.cpp`, `USDGEN_STUB_PROMOTE_PRIMVARS_DIRTY=1`) makes the skin track the animation exactly:

| promotion | t=1001 | t=1012 | t=1024 |
|---|---|---|---|
| off | -0.2000 | -0.2000 | -0.2000 |
| **on** | -0.2000 | **-0.2269** | **-0.2562** |

So: **any scene index that replaces a mesh's `primvars` container and may sit upstream of UsdSkelImaging must
dirty the bare `primvars` locator, not only leaf locators.** This is a usdGen design rule and, separately, a
usdRig bug worth filing (it is latent today because no usdRig example combines UsdSkel with a RigExec mover).

Caveat: the probe drives only RigExec's time; `UsdImagingStageSceneIndex::SetTime` is never called, so the
`SkelAnimation` itself contributes an identity transform throughout. The experiment isolates the `restPoints`
input, which is exactly the coupling under test.

---

## 5. Interactive baseline: RigExec `SetTime` cost

`examples/ArmShotAnim.usda` (a shot layer referencing `ArmRig.usda`, `rigComplexity="animation"`), 48 frames
1001→1048, `RigExecImaging_Activate` then a wall-clock loop over `RigExecImaging_SetTime`. aarch64, 20 CPUs,
no GPU involved.

| configuration | total (48 frames) | mean / frame | worst frame |
|---|---|---|---|
| `SetTime` only, run 1 | 65.89 ms | **1.373 ms** | 1.710 ms |
| `SetTime` only, run 2 | 64.84 ms | 1.351 ms | 1.695 ms |
| `SetTime` only, run 3 | 72.11 ms | 1.502 ms | 8.514 ms (first-touch outlier) |
| `SetTime` + full terminal traversal, 95 prims, 195 points pulled | 77.30 ms | **1.610 ms** | — |
| same, with two extra pass-through filtering scene indices in the chain | 75.43 ms | 1.571 ms | — |

Reading: RigExec costs ~1.35 ms per frame on this rig; a complete pull of every prim and every `points`
primvar through the whole 17-deep chain adds ~0.24 ms. **Two extra filtering scene indices in the chain cost
nothing measurable** (75.4 vs 77.3 ms is inside run-to-run noise) — the per-hop overhead of a pass-through
`HdSingleInputFilteringSceneIndexBase` is not a design constraint. That leaves roughly 15 ms of a 16.6 ms
frame budget for usdGen's own work at 60 fps on this class of rig, before any GPU time. These are CPU numbers
on this machine only; Storm draw cost is UNMEASURED here (llvmpipe only — see `B-usdrig-build.md`).

---

## 6. Recommendation

**Ship usdGen's scene indices as a renderer-level `HdSceneIndexPlugin` with `loadWithRenderer: ""`, plus
explicit `tags`/`ordering` metadata. Do not use a second `UsdImagingSceneIndexPlugin`. Do not ask usdRig for
a registry hook.**

| option | ordering guarantee | sees skinned surfaces? | couples usdGen to | verdict |
|---|---|---|---|---|
| second `UsdImagingSceneIndexPlugin` | none — `std::set<TfType>` on heap addresses, flips with `PXR_PLUGINPATH_NAME` (§2) | **no** — lands upstream of UsdSkelImaging in the observed default (§2 chain) and would read the bind pose | nothing | reject |
| renderer-level `HdSceneIndexPlugin` | strong — always downstream of the entire UsdImaging chain (§3), plus JSON `ordering.after/before` that was measured to work and to be path-order stable | **yes** — after UsdSkelImaging; needs a private `HdSiExtComputationPrimvarPruningSceneIndex` to read points (§4b) | nothing (works under Storm, hdPrman, any renderer) | **adopt** |
| rigExec registry hook | orders usdGen w.r.t. **rigExec only**; says nothing about UsdSkelImaging, which is the ordering that actually matters | no better than option 1 | hard dependency on usdRig, and a usdRig source change | reject |

The minimal usdRig change, for the record, would be: add
`void RegisterAppender(std::function<HdSceneIndexBaseRefPtr(HdSceneIndexBaseRefPtr)>)` to
`RigExecImagingRegistry` (`usdRig/libs/rigExecImaging/registry.h:28-41`) and run the appenders at the end of
`RigExecUsdImagingSceneIndexPlugin::AppendSceneIndex`
(`usdRig/libs/rigExecImaging/sceneIndexPlugin.cpp:36-39`) — about ten lines. It is cheap but buys the wrong
guarantee, and it makes usdGen unusable without usdRig.

Concrete plan items that follow:

1. usdGen registers `UsdGenSceneIndexPlugin : HdSceneIndexPlugin`, plugInfo `loadWithRenderer: ""`, tags
   `["usdGen:groom"]`, `ordering: {"after": ["hd:sceneAssembly"]}` (the tag HdGp already uses,
   `pxr/imaging/hdGp/plugInfo.json:15-21`). Verify placement in CI with the walk this probe implements.
2. Inside that plugin, wrap the input in a **private** `HdSiExtComputationPrimvarPruningSceneIndex` used only
   for reading bound surfaces' `points`/`normals`. Never expose it downstream.
3. Any usdGen scene index that replaces a prim's `primvars` container must dirty the **bare `primvars`
   locator** alongside its leaf locators (§4c).
4. File a usdRig issue: `RigExecResultsSceneIndex` should add `HdPrimvarsSchema::GetDefaultLocator()` to its
   dirty set when it republishes points, so UsdSkel-skinned + RigExec-moved meshes stop freezing.
5. Application integration in usdview goes through `UsdImagingGLEngine`, which already builds the merge and
   calls `AppendSceneIndicesForRenderer` (`engine.cpp:1544, 1776-1779`) — no fork, no override callback needed.
6. Budget: usdGen gets ~15 ms/frame at 60 fps after RigExec on an `ArmShotAnim`-class rig; extra filtering
   scene indices are free, so favour many small composable operators over one fused one.

---

## Key facts

- `UsdImagingCreateSceneIndices` inserts plugin scene indices at exactly one point — `sceneIndices.cpp:302`,
  between `UsdImagingMaterialBindingsResolvingSceneIndex` (`:298`) and `UsdImagingSelectionSceneIndex` (`:305`)
  — via `_AddPluginSceneIndices` (`sceneIndices.cpp:68-78`).
- That order comes from a `std::set<TfType>` (`usdImaging/sceneIndexPlugin.cpp:50-54`) keyed on the private
  `_TypeInfo*` (`tf/type.h:119`, `:728`). **Measured:** swapping two `PXR_PLUGINPATH_NAME` entries flipped the
  append order of the two plugins they carry (6-permutation table, §2); 8 identical-env runs were stable.
- **Measured:** a renderer-level `HdSceneIndexPlugin` lands strictly downstream of the entire UsdImaging chain
  — `HdRenderIndex` ctor `renderIndex.cpp:209-214`, guarded by `:208`; the UsdImaging chain is an input to
  `HdMergingSceneIndex` (`:196-198`, `InsertSceneIndex` `:314-335`). Probe printed a 24-node chain with the
  stubs at slots 0-1 and the whole UsdImaging chain at 7-23.
- `_SetRendererDisplayName` is private with `friend class HdRendererPlugin` (`hd/renderDelegate.h:584-589`) and
  the only caller is `hd/rendererPlugin.cpp:76-78`. **An application-constructed render delegate gets no
  renderer-level scene index plugins at all.**
- **Measured:** renderer-level order is stable under `PXR_PLUGINPATH_NAME` reordering, and JSON
  `"ordering": {"after": [...]}` deterministically reorders it (default policy `Hybrid`,
  `hd/sceneIndexPluginRegistry.cpp:40-48`).
- **Measured:** on a UsdSkel-skinned mesh, terminal `primvars:points` and `primvars:normals` are **blocked**
  (`Get` returns nullptr); points exist only as `extComputationPrimvars:points`
  (`usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:501-508`, `:290-304`). `HD_ENABLE_DEFERRED_SKINNING`
  defaults false (`hd/skinningSettings.cpp:15`).
- **Measured:** `HdSiExtComputationPrimvarPruningSceneIndex` restores `primvars:points` headlessly with real
  animated values; it appears in no stock chain. Its `GetNames()` listed `points` twice.
- **Measured bug:** RigExec upstream of UsdSkelImaging freezes the skin (`restPoints` identical at t=1001/1012/1024
  while a rig-only stage animates). Dirty notices *are* correct; the resolved prim's captured
  `HdPrimvarsSchema _primvars` (`dataSourceResolvedPointsBasedPrim.h:245`) is stale because the refresh test is
  `Contains(primvars)` (`:1178-1180`) and RigExec only sends child locators.
- **Measured fix:** promoting the dirty to the bare `primvars` locator in an interposed scene index unfreezes it
  exactly (-0.2000 / -0.2269 / -0.2562, matching the rig-only ground truth).
- **Measured baseline:** `RigExecImaging_SetTime` on `ArmShotAnim.usda` = 1.35-1.50 ms/frame over 48 frames
  (65-72 ms total, worst non-first frame 1.99 ms). Full terminal traversal of 95 prims + 195 points adds
  ~0.24 ms/frame. Two extra pass-through filtering scene indices cost nothing measurable.
- Storm's renderer display name is `"GL"` (`pxr/imaging/plugin/hdStorm/plugInfo.json:10`);
  `USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX` defaults true (`usdImagingGL/engine.cpp:78`).

## Decisions this settles

1. **usdGen ships as a renderer-level `HdSceneIndexPlugin`** (`loadWithRenderer: ""`, explicit `tags` +
   `ordering`), not as a second `UsdImagingSceneIndexPlugin` and not behind a usdRig registry hook.
2. **usdGen must never assume it runs after rigExec or after UsdSkelImaging by virtue of the UsdImaging plugin
   mechanism** — that order is a heap-address accident. Renderer level makes the guarantee structural.
3. **usdGen reads bound surfaces through a private `HdSiExtComputationPrimvarPruningSceneIndex`**, because a
   skinned mesh's `primvars/points` is blocked at the terminal. It must not be spliced into the shared chain.
4. **Every usdGen scene index that overrides a `primvars` container dirties the bare `primvars` locator**, or
   downstream UsdSkelImaging silently freezes.
5. **The operator graph can be many small scene indices.** Per-hop cost is below measurement noise; the budget
   is dominated by the operators' own work.
6. usdGen needs no fork of `UsdImagingGLEngine` and no `overridesSceneIndexCallback` for viewport integration.
7. A usdRig issue should be filed for the `primvars` dirty-locator granularity (§4c, item 4).

## Open questions

- Why `RigExecUsdImagingSceneIndexPlugin` sorted first and `UsdSkelImagingResolvingSceneIndexPlugin` last in
  all six path permutations, while the two stubs tracked path order, is **UNEXPLAINED**. It does not change
  the recommendation but it means the failure mode may be subtler than "path order wins".
- The `HdRenderIndex`/`UsdImagingGLEngine` chains were exercised with a *null* render delegate. Whether Storm
  specifically inserts anything between the merge and the renderer plugins is **UNMEASURED** here (Storm needs
  a GL context; llvmpipe under Xvfb is available per `B-usdrig-build.md` but was not used for this probe).
  Protocol to close it: on a display, `usdview --renderer GL` with
  `HD_SCENE_INDEX_PLUGIN_ORDERING=1 HD_USE_ENCAPSULATING_SCENE_INDICES=1`, read the resolved entry list printed
  by `_PrintEntries` (`sceneIndexPluginRegistry.cpp:1404-1419`), and confirm `UsdGenSceneIndexPlugin` appears
  after `HdSt_*`.
- Cost of `HdSiExtComputationPrimvarPruningSceneIndex` on a production-density skinned character (100k+ points,
  hundreds of joints) is **UNMEASURED** — the test mesh has 10 points. Protocol: extend
  `probeChainOrder --pull` to a character asset and report ms/frame for the baked pull versus the raw pull.
- Whether usdGen should instead consume `extComputationPrimvars` directly (registering its own ExtComputation
  that chains off the skinning one, keeping the GPU path intact) is **not settled here**; it would avoid the
  CPU bake but requires the generator to be expressible as an ExtComputation kernel.
- Interaction of the renderer-level placement with native instancing (usdGen sees post-`NiPrototypePropagating`
  prototypes) is untested.
