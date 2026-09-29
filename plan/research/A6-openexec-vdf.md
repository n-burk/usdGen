# A6 — OpenExec / VDF / Esf in OpenUSD 26.08: what is usable for a dynamic hair operator graph with sparse invalidation

Scope: `<openusd-src>/pxr/exec/{vdf,ef,esf,esfUsd,exec,execUsd,execGeom,execIr}` (tag v26.08),
`<openusd-src>/pxr/usdImaging/usdExecImaging`, `<openusd-src>/extras/exec/examples`,
and usdRig's own use of these libraries (`<usdrig-src>/libs/rigExec`, `libs/rigExecImaging`,
`docs/exec-api-notes.md`, `docs/execusd-api-notes.md`, `docs/mover-graph-cutover.md`).
All paths below are absolute; `file:line` cites the source as of v26.08. Claims I could not verify in source are
marked UNVERIFIED.

---

## 0. TL;DR for the architects

1. **OpenExec is three layers**: `vdf` (a vectorized data-flow engine you can build networks in by hand), `ef`
   (time, leaf nodes, page caches, executors on top of vdf) and `exec/execUsd` (a compiler from a UsdStage plus
   schema-registered computations into a vdf network, with change-driven invalidation). Only `execUsd` + the
   registration DSL in `exec` are documented as public; `esf`/`esfUsd` carry a "not meant for public use" note
   (`<openusd-src>/pxr/exec/esf/README.md:3-4`, `esfUsd/README.md:3-4`). All eight libraries and
   their headers *are* installed (`$USD/include/pxr/exec/{ef,esf,esfUsd,exec,execGeom,execIr,execUsd,vdf}`,
   `lib/libusd_{vdf,ef,esf,esfUsd,exec,execUsd,execGeom,execIr}.so`), so linking against `vdf`/`ef` directly is
   possible today (usdRig already does: `libs/rigExec/moverGraph.cpp:18-30`).
2. **Through ExecUsd, a `VtArray` attribute is ONE data-flow element (a `Vdf_BoxedContainer`), not N vectorized
   elements** (`exec/typeRegistry.h:219-240`, `exec/attributeInputNode.cpp:75-79` sets `VdfMask::AllOnes(1)`).
   Per-element sparse invalidation of a 100k-curve array is therefore *not* available through ExecUsd: any edit
   invalidates the whole array and every downstream node. True N-element vectorization with per-element masks only
   exists for hand-built networks (`VdfInputVector<T>(network, n)`, `vdf/inputVector.h:73-79`, as usdRig does at
   `libs/rigExec/moverGraph.cpp:1387-1401`).
3. **ExecUsd cannot express what a hair graph needs at the network level**: no reverse-relationship accessor, no
   "attribute of a relationship target" accessor (`exec/computationBuilders.h:840` `XXX:TODO Property,
   NamespaceParent, NamespaceChildren`), array overrides are rejected as type mismatches
   (`exec/system.cpp:168-181`; usdRig boxed arrays into `RigExecPointsPacket` to work around it,
   `docs/mover-graph-cutover.md` "Pass 1.5"), and callbacks may not read scene state directly. Variable output
   counts are fine (boxed values carry their own length), but every result is a whole-array VtValue copy on
   extraction (`VdfVector::ExtractAsVtArray` copies unless the vector was `Share()`d, and nothing in
   `vdf`/`ef`/`exec` production code calls `Share()` — only `vdf/testenv/testVdfVector.cpp`).
4. **usdRig does NOT execute its hand-built mover graph incrementally.** `RigExecMoverGraph` is constructed fresh
   for every chain on every `Evaluate(time)` (`libs/rigExec/rigEvaluator.cpp:7009`), and
   `RigExecMoverGraph::Evaluate` builds a new `VdfSchedule` and a new `VdfSimpleExecutor` per call
   (`libs/rigExec/moverGraph.cpp:1436-1443`). Node parameters are baked as `VdfInputVector` constants
   (`moverGraph.cpp:1411-1421`). So "dynamically like usdRig" currently means "rebuild-and-run per frame"; the
   VDF machinery for persistent executors, sparse invalidation and parallel evaluation is available but unused there.
5. **VDF does support what a hair graph needs if you build the network yourself**: persistent executors with
   per-output validity masks (`vdf/executorInvalidationData.h:105-163`), `InvalidateValues` with a replayable
   dependency traversal (`vdf/executorInvalidator.cpp:27-55`), affects masks and READWRITE buffer passing
   (`vdf/output.h:94-95`, `vdf/inputSpec.h:35-38`), a TBB task-graph parallel engine
   (`vdf/parallelExecutorEngineBase.h:648-700`; instantiate as
   `VdfExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>`, `vdf/testenv/testVdfSpeculation.cpp:319-320`),
   schedules that auto-invalidate on topology edits (`vdf/scheduleInvalidator.h:40-77`), and a time-keyed page
   cache (`ef/pageCacheStorage.h:69-72`, `exec/runtime.cpp:45-49`).
6. **Time and scene-index inputs**: `EfTime` is `UsdTimeCode` + 8 bits of spline flags (`ef/time.h:36-165`), fed
   into the network by an `EfTimeInputNode` root (`ef/timeInputNode.h:26-46`) whose value is set with
   `executor->SetOutputValue` (`exec/runtime.cpp:81-111`). A hand-built network can take *any* external value
   (Hydra scene-index data included) the same way: root/input-vector nodes plus `InvalidateValues`
   (`ef/inputValueBlock.h:92-101` is the packaged form). There is no scene-index-backed `EsfStage`; writing one
   is structurally possible (`ExecSystem` ctor takes any `EsfStage`, `exec/system.h:53-54`; change delivery via
   `ExecSystem::_ChangeProcessor`, `exec/systemChangeProcessor.h:23-61`) but touches the "not public" `esf` API and
   would need a private `Exec_RequestImpl` subclass (`exec/requestImpl.h:83-113`).
7. **26.08 ships `usdExecImaging`** — an exec-driven initial scene index that UsdImagingGL merges in front of the
   stage scene index when `USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX=1` (default false,
   `<openusd-src>/pxr/usdImaging/usdImagingGL/engine.cpp:90-96,1617-1645`). Its adapter registry is
   hard-coded (UsdGeomXformable, ExecIrXformable; `usdExecImaging/adapterRegistry.cpp:28-51`), so it is a *pattern*
   to copy (request per stage, lazy `HdSampledDataSource` over `ExecUsdCacheView`, invalidation callbacks → dirty
   locators, `SetTime`/`ApplyPendingUpdates` from the engine's main-thread update), not an extension point.
8. **Recommendation**: (a) hand-built VdfNetwork for the per-curve data plane is the only OpenUSD-native route to
   sparse, vectorized, parallel, incremental evaluation; (b) ExecUsd for scalar/small per-prim parameters and for
   any value that already comes out of usdRig's exec (transforms, weight packets); (c) a bespoke TBB DAG is the
   pragmatic alternative if the team is unwilling to own VDF's undocumented executor/data-manager surface. Details
   and trade-offs in §8.

---

## 1. Layer map (what each library gives you, and its public/private status)

| Library | Purpose (from README) | Public? | Key headers for us |
|---|---|---|---|
| `vdf` | "foundation for building and evaluating vectorized dataflow networks" (`vdf/README.md`) | no README caveat; headers installed | `network.h`, `node.h`, `context.h`, `vector.h`, `mask.h`, `maskedOutput.h`, `request.h`, `schedule.h`, `scheduler.h`, `executorInterface.h`, `simpleExecutor.h`, `executor.h`, `parallelExecutorEngine.h`, `parallelDataManagerVector.h`, `inputVector.h`, `readIterator.h`, `readWriteIterator.h`, `isolatedSubnetwork.h`, `extensibleNode.h`, `executionStats.h` |
| `ef` | "extends vdf: VdfNode types, VdfExecutorInterface impls, caching structures" (`ef/README.md`) | same | `time.h`, `timeInterval.h`, `timeInputNode.h`, `leafNode.h`, `leafNodeCache.h`, `inputValueBlock.h`, `pageCacheExecutor.h`, `pageCacheBasedExecutor.h`, `pageCacheStorage.h`, `maskedSubExecutor.h` |
| `esf` | scene abstraction used by the exec compiler | **"not meant for public use"** (`esf/README.md:3-4`) | `stage.h`, `prim.h`, `attribute.h`, `attributeQuery.h`, `relationship.h`, `object.h`, `journal.h`, `editReason.h` |
| `esfUsd` | `EsfStageInterface` over `UsdStage` | **"not meant for public use"** (`esfUsd/README.md:3-4`) | `sceneAdapter.h`, `stageData.h` |
| `exec` | compiler + registration DSL + `ExecSystem` base | DSL public; system internals private (`Exec_*`) | `registerSchema.h`, `computationBuilders.h`, `builtinComputations.h`, `typeRegistry.h`, `request.h`, `system.h`, `systemDiagnostics.h` |
| `execUsd` | "primary entry point for OpenExec" (`execUsd/README.md`) | yes | `system.h`, `request.h`, `cacheView.h`, `valueKey.h`, `valueOverride.h` |
| `execGeom`, `execIr` | sample registrations (Xformable local-to-world; invertible rigs) | yes | `execGeom/xformable.cpp`, `execIr/*` |
| `usdExecImaging` (in `pxr/usdImaging`) | exec-driven initial Hydra scene index | public interface + factory; adapters private | `stageSceneIndexInterface.h`, `stageSceneIndexFactory.h`, `computedDataSource.h`, `primAdapterInterface.h` |

Design doc (`exec/docs/executionSystemDesign.md`): three phases — compilation (expensive, incremental after first),
scheduling (per request; invalidated by topology change), evaluation (pull-based from leaves, "truncating once any
node returns a cache hit", parallel by default). Data managers hold "validity masks that track the dirty state of
outputs"; executors "can be arranged in a hierarchy" (sub-executors read parents' results).

---

## 2. The ExecUsd public surface

### 2.1 API table (verbatim signatures)

| Class / function | Signature | Source |
|---|---|---|
| `ExecUsdSystem` ctor | `explicit ExecUsdSystem(const UsdStageConstRefPtr &stage)`; non-copyable | `execUsd/system.h:48-55` |
| time | `void ChangeTime(UsdTimeCode time)` — one current time for the whole system; no per-Compute time | `execUsd/system.h:74-75`; `exec/system.cpp:47-81` |
| request | `ExecUsdRequest BuildRequest(std::vector<ExecUsdValueKey>&&, ExecRequestComputedValueInvalidationCallback&& = {}, ExecRequestTimeChangeInvalidationCallback&& = {})` | `execUsd/system.h:104-110` |
| prepare | `void PrepareRequest(const ExecUsdRequest&)` = Compile + Schedule | `execUsd/system.h:119-120`, `execUsd/system.cpp:85-98` |
| compute | `ExecUsdCacheView Compute(const ExecUsdRequest&)` (implicitly compiles/schedules) | `execUsd/system.h:129-130`, `execUsd/system.cpp:100-117` |
| overrides | `ExecUsdCacheView ComputeWithOverrides(const ExecUsdRequest&, ExecUsdValueOverrideVector&&)` | `execUsd/system.h:151-154` |
| value key | `ExecUsdValueKey(const UsdAttribute&)`, `(const UsdAttribute&, const TfToken&)`, `(const UsdPrim&, const TfToken&)` | `execUsd/valueKey.h:61-70` |
| request handle | move-only, `bool IsValid() const` | `execUsd/request.h:31-63` |
| cache view | `VtValue Get(int index) const` | `execUsd/cacheView.h:38-39` |
| callbacks | `using ExecRequestIndexSet = pxr_tsl::robin_set<int>;` `std::function<void(const ExecRequestIndexSet&, const EfTimeInterval&)>`; `std::function<void(const ExecRequestIndexSet&)>` | `exec/request.h:19,28-31,40-41` |
| override | `struct ExecUsdValueOverride { ExecUsdValueKey valueKey; VtValue overrideValue; }` | `execUsd/valueOverride.h:27-36` |
| diagnostics | `ExecSystem::Diagnostics(&system).GraphNetwork("file.dot")`, `.InvalidateAll()` | `exec/systemDiagnostics.h` (used by `usdExecImaging/request.cpp:274-275`) |

Mechanics worth knowing (all verified in source):

- `Compute()` always re-runs compilation (`Exec_RequestImpl::_Compile`: "Even if the request is already compiled, we
  always need to perform recompilation, because doing so might make new connections", `exec/requestImpl.cpp:385-388`),
  in parallel (`WorkWithScopedDispatcher` + `WorkParallelForN`, `exec/requestImpl.cpp:395-422`; compiler tasks in
  `exec/compiler.cpp:54-101`). The schedule is rebuilt only if invalid (`exec/requestImpl.cpp:479-485`,
  `VdfScheduler::Schedule(..., /*topologicallySort*/ false)`).
- `_Compute` clears "interest" (`_lastInvalidatedIndices.ClearAll(); _lastInvalidatedInterval.Clear();`,
  `exec/requestImpl.cpp:496-500`) and returns a view over the main executor's data manager
  (`exec/requestImpl.cpp:506-507`; `exec/cacheView.h:78-81` holds a `VdfDataManagerFacade` or a unique executor
  for the overrides path). Invalidation callbacks are one-shot per index until the next Compute
  (`exec/requestImpl.cpp:644-651`).
- Value invalidation is delivered **synchronously inside the `UsdNotice::ObjectsChanged` dispatch**
  (`execUsd/system.cpp:140-190` → `ExecSystem::_ChangeProcessor` → `exec/systemChangeProcessor.cpp:111-132` →
  `exec/system.cpp:286-322` → `requestTracker->DidInvalidateComputedValues`). Which fields count: `Default`,
  `Spline`, `TimeSamples` → value invalidation with an `EfTimeInterval`; `TargetPaths` / `ConnectionPaths` →
  uncompile (`exec/systemChangeProcessor.cpp:73-90`); resyncs → uncompile + request expiration
  (`execUsd/system.cpp:150-169`).
- `ChangeTime` invalidates only outputs whose attribute inputs are actually time-varying between old and new time
  (`exec/system.cpp:47-81`; `exec/attributeInputNode.cpp:62-67` `IsTimeVarying(from,to)`), and fires the time
  callback (`exec/requestImpl.cpp:261-306`).
- The main executor is `EfPageCacheExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>` when
  `VDF_ENABLE_PARALLEL_EVALUATION_ENGINE` (default true, `vdf/types.cpp:13-16`), else a serial
  `EfPageCacheExecutor<VdfPullBasedExecutorEngine, VdfDataManagerVector<Background>>`
  (`exec/runtime.cpp:51-70`). Its page cache is keyed by the `EfTimeInputNode` output (`exec/runtime.cpp:45-49`),
  so revisiting a time hits cache.
- `ComputeWithOverrides` builds a masked sub-executor + a temporary sub-executor per call; the override value's
  `VtValue` type must equal the compiled output's `TfType` (`exec/system.cpp:168-181`), which is why array
  overrides fail (element type vs `VtArray`). It also invalidates "outputs that are not present in the schedule"
  (`exec/runtime.cpp:288-295` TODO) — a second evaluation pass per frame (usdRig pays this: `docs/mover-graph-cutover.md`
  "it costs a second evaluation pass").

### 2.2 Registering computations for a custom (codeless) schema

Everything in usdRig's `docs/exec-api-notes.md` §1–§7 holds; the load-bearing facts, re-verified:

- `EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA(SchemaTypeName)` expands to a `TF_REGISTRY_FUNCTION(ExecDefinitionRegistryTag)`
  and resolves `#SchemaTypeName` with `TfType::FindByName` (`exec-api-notes.md` §1.1, citing
  `computationBuilders.cpp:428`). The schema library can be a codeless `"Type": "resource"` plugin; the
  computation library must be a `"Type": "library"` plugin with `Info.Exec.Schemas` (`exec-api-notes.md` §1.2-1.3;
  real examples `execGeom/plugInfo.json`, `execIr/plugInfo.json`).
- Provider resolution options are exactly `Local`, `RelationshipTargetedObjects` (with relationship forwarding),
  `ConnectionTargetedObjects`, `IncomingConnectionOwningAttributes`, `NamespaceAncestor`
  (`exec/providerResolution.h:41-59`). Relationship targets are traversed in target order and each target that has
  the computation contributes its own output/connection (`exec/inputResolver.cpp:319-345`).
- Input from another prim's computed value: `Relationship(rel).TargetedObjects<T>(computationToken)`; from an
  attribute of the same prim: `AttributeValue<T>(attr)`; from a namespace parent: `NamespaceAncestor<T>(token)`.
  Missing: attribute-of-target, reverse relationships, namespace children, "Property" accessor
  (`exec/computationBuilders.h:840` `// XXX:TODO Property, NamespaceParent, NamespaceChildren, etc.`;
  `:584,1138` `AnimSpline`).
- `ExecTypeRegistry::RegisterType<T>` rejects `VtArray` (`exec/typeRegistry.h:101-104`) and requires equality
  comparison (`:105-107`). Custom structs (usdRig's `RigExecPointsPacket`, `RigExecMoverParameters`) are the way to
  move arrays as one value.

### 2.3 The vectorization story (this matters most for 100k curves)

How an authored `VtArray<T>` attribute enters exec:

```cpp
// exec/typeRegistry.h:219-240
template <typename T> VdfVector ExecTypeRegistry::_CreateVector<T>::Create(const T &value) {
    if constexpr (!VtIsArray<T>::value) { VdfVector v = VdfTypedVector<T>(); v.Set(value); return v; }
    else {
        using ElementType = typename T::value_type;
        const size_t size = value.size();
        Vdf_BoxedContainer<ElementType> execValue(size);
        std::copy_n(value.cdata(), size, execValue.data());     // full copy
        VdfVector v = VdfTypedVector<ElementType>(); v.Set(std::move(execValue)); return v;
    }
}
```

and `Exec_AttributeInputNode::Compute` publishes it under `VdfMask::AllOnes(1)` (`exec/attributeInputNode.cpp:75-79`).
`Vdf_BoxedContainer` is documented as "stores multiple values that flow through the network as a single data flow
element ... without encoding the length of that data in the topology" (`vdf/boxedContainer.h:75-81`). Consequences:

| Property | ExecUsd (attribute arrays, computed results) | Hand-built VDF with `VdfInputVector<T>(n)` |
|---|---|---|
| Mask granularity | 1 bit per array (boxed) | 1 bit per element (`VdfMask` = `TfCompressedBits`, `vdf/mask.h:43`) |
| Variable length | yes, trivially (boxed length is data) | topology change: recreate the input vector node (size fixed at ctor, `vdf/inputVector.h:73-79`) |
| Read in a callback | `VdfReadIterator<T>` walks boxed elements transparently (`vdf/readIterator.h:158-170` `_boxedIndex`) | same iterator |
| Write | `VdfReadWriteIterator<T>::Allocate(ctx, n)` → `Vdf_AllocateBoxedValue` (`vdf/readWriteIterator.h:215-234`) | in place via READWRITE connector, affects-mask driven (`:292-309`) |
| Sparse recompute | no (whole array invalidated) | yes (`VdfExecutorInvalidationData` sparse masks, `vdf/executorInvalidationData.h:105-163`) |
| Extraction to `VtArray` | `ExtractAsVtArray` — zero-copy only if `Share()`d; **no production caller of `Share()`** exists in `vdf/ef/exec` (only `vdf/testenv/testVdfVector.cpp:436-833`), so `ExecUsdCacheView::Get` copies the array (`vdf/vector.h:375-404`, `exec/typeRegistry.h:242-280`) | you own the `VdfVector`; `GetReadAccessor<T>()` is a pointer view (`vdf/vector.h:520-524`) |

Is 100k curves × 8 CVs (800k `GfVec3f`, ~9.6 MB) practical as an exec computed value? Mechanically yes (boxed values
are `TfSmallVector` heap buffers, `vdf/boxedContainer.h:95-97`), but every hop costs a full copy: input node copy
(`typeRegistry.h:231-232`), `ctx.SetOutput(const T&)` "always performs a full copy" (`vdf/context.h:114-115`;
use the `T&&` overload, `:140-142`), and extraction copy. Per-element invalidation is unavailable, so a single
attribute edit re-runs the entire chain. For scalar/small parameters (clump ratio, frizz amplitude, seed) ExecUsd is
fine; for the curve data plane it is not the right tool.

---

## 3. Constraints usdRig hit (evidence)

| Constraint | Evidence | Effect on a hair graph |
|---|---|---|
| No reverse-relationship accessor; a computation on a joint cannot reach the solver that names it | `docs/mover-graph-cutover.md` "The OpenExec constraint that shapes all of this"; accessor list `exec/computationBuilders.h:840` | Operators must be wired **downstream → upstream** (styler names its generator), which happens to be natural for hair (like a host groomer modifiers referencing the description). Any "surface → hair" push direction must be resolved outside exec. |
| Relationship accessor can only request *computations* on targets, not a named attribute of them | `mover-graph-cutover.md` "Pass 1.5 ... points are unreachable from the ribbon"; `computationBuilders.h:840` | A styler that wants `scalp.points` via `rel groom:surface` cannot; you would register a computation on the scalp schema (e.g. `computePoints`) or box the array into a custom type. |
| Array overrides rejected (type-checked against element type) | `exec/system.cpp:168-181`; `mover-graph-cutover.md` "Pass 1.5" | Supplying externally-computed arrays (e.g. from usdRig's deformed surface, or a simulation) into exec requires a registered boxed struct (usdRig: `RigExecPointsPacket`). |
| Callbacks must be pure; all scene reads declared as inputs ("cache safety") | `exec-api-notes.md` §3.2, §8 item 11; `vdf/node.h:350-353` thread-safety | Fine for math kernels; awkward for texture/ptex lookups (must be inputs or side tables). |
| One time per system, `ChangeTime` global | `execUsd/system.h:74-75`; `execusd-api-notes.md` §2 | Motion-blur sub-samples = N `ChangeTime`+`Compute` rounds (usdRig: `libs/rigExecImaging/bridge.cpp:1287` per sample time). |
| Invalidation callbacks run synchronously on the authoring thread; must not compute inside | `execusd-api-notes.md` §6; `execUsd/system.cpp:140-190` | usdRig only flips a dirty flag (`libs/rigExec/tapSet.cpp:80-88`) and evaluates on the next pull. |
| Listener ordering: `ExecUsdSystem` registers its notice listener in its ctor and Tf prepends listeners, so any listener registered *after* the system sees stale exec values inside the same notice dispatch | `libs/rigExecImaging/registry.cpp:225-262` (measured on `11_VolumeWeights`) | Register the app's re-evaluation listener *before* constructing the `ExecUsdSystem`, or defer re-evaluation to after the dispatch. |
| `VdfReadWriteIterator::Allocate` on a READWRITE output fails ("output cannot hold a boxed value") and degrades to pass-through | `mover-graph-cutover.md` "Two traps already paid for"; `vdf/readWriteIterator.h:215-234` allocates a **boxed** value | On a READWRITE connector construct the iterator on the *input* name and write in place; allocate only on plain outputs. |
| Anything talking to VDF directly must touch `ExecTypeRegistry::GetInstance()` first or registered types are not distinguishable | `mover-graph-cutover.md` "Two traps"; `vdf/executionTypeRegistry.h:64-93` (`Define`, `CheckForRegistration` fatal) | Force the registry in your library init. |
| Multiple targets / duplicate input names deliver multiple values on one input; `GetInputValue` sees only the first | `exec-api-notes.md` §3.3, §4(c); `vdf/context.h:301-314` | Always use `VdfReadIterator` for relationship-fed inputs. |

Why usdRig built its own VdfNetwork for movers (from `moverGraph.h` header comment): expressing the write-set chain as
generated prims "cost a recomposition, put engine machinery on the authoring surface -- while still not being able to
express ... splitting one revision across face sets, cloning legs, per-element masks driving sparse recomputation".
Note the last item is aspirational: the current graph evaluates with all-ones masks and no persistence (§4.6).

---

## 4. Executing a hand-built VdfNetwork incrementally

### 4.1 Building the network

```cpp
// vdf/network.h:180-197
VdfConnection *Connect(VdfOutput *output, VdfNode *inputNode, const TfToken &inputName,
                       const VdfMask &mask, int atIndex = AppendConnection);
VdfConnection *Connect(const VdfMaskedOutput &maskedOutput, VdfNode *inputNode,
                       const TfToken &inputName, int atIndex = AppendConnection);
bool Delete(VdfNode *node);              // node must be fully disconnected      :209
void Disconnect(VdfConnection *connection);                                       :214
bool DisconnectAndDelete(VdfNode *node);                                          :221
void ReorderInputConnections(VdfInput*, const TfSpan<const size_t>&);            :236-238
void RegisterEditMonitor(EditMonitor*); size_t GetVersion() const;               :245,259-261
```

- Nodes are created with `new MyNode(&network, VdfInputSpecs()..., VdfOutputSpecs()...)`; the network takes
  ownership (`vdf/node.h:109-111`, `vdf/network.h:307-313`). Connector specs:
  `.ReadConnector<T>(name)`, `.ReadWriteConnector<T>(inName, outName)` (associated output → in-place buffer
  passing), and the `prerequisite` flag (`vdf/connectorSpecs.h:28-38`, `vdf/inputSpec.h:35-38,89-96`).
- Per-node override points: `Compute(const VdfContext&)` (must be thread-safe for concurrent calls on one node,
  `vdf/node.h:350-353`), `_ComputeOutputDependencyMask` / `_ComputeInputDependencyMask` to make invalidation and
  scheduling sparse (default: everything depends on everything, `vdf/node.h:601-605,650-653`),
  `GetRequiredInputsPredicate` for conditional pulls (`vdf/node.h:369-371`, `vdf/requiredInputsPredicate.h:28-45`).
- Output affects masks: `VdfOutput::SetAffectsMask(const VdfMask&)` (`vdf/output.h:94-95`); a READWRITE output with
  an affects mask is a "pool" output — the pattern used for point pools in Pixar's rigs (`vdf/testenv/testVdfNetworkThreading.cpp:46-61`).
- Sources: `VdfInputVector<T>(network, n)` with `SetValue(i, v)` (`vdf/inputVector.h:73-104`; `IsValueEqual` for
  change detection `:108-115`), `VdfRootNode` (no inputs, `vdf/rootNode.h:29-58`), `EfTimeInputNode` (`ef/timeInputNode.h:26-46`).
- Dynamic connector counts: `VdfExtensibleNode::AddInputSpecs/AddOutputSpecs` (`vdf/extensibleNode.h:45-56`), or
  `VdfNode::_ReplaceInputSpecs` (`vdf/node.h:575`).
- Bulk deletion of a dead subgraph: `VdfIsolatedSubnetwork::IsolateBranch(node|connection, canDelete)`
  (`vdf/isolatedSubnetwork.h:77-99`); `VdfNetwork::Clear()` (`:172`).
- Concurrent construction is supported: `_nodes` is a `tbb::concurrent_vector`, free ids a `tbb::concurrent_queue`
  (`vdf/network.h:387-399`), output connection lists are guarded by a `tbb::spin_mutex` (`vdf/output.h:187`,
  `vdf/output.cpp:147,157`), and `testVdfNetworkThreading.cpp:74-111` creates 50k nodes and connects them from
  `WorkParallelForN`. The exec compiler relies on this (`exec/compiler.cpp:54-101`).

### 4.2 Scheduling

```cpp
VdfRequest request(VdfMaskedOutput(output, mask));            // vdf/request.h:41
VdfSchedule schedule;
VdfScheduler::Schedule(request, &schedule, /*topologicallySort*/ true);   // vdf/scheduler.h:48-50
schedule.IsValid();                                              // vdf/schedule.h:172-174
```

Schedules register with the network and are invalidated automatically by `Vdf_ScheduleInvalidator` on node
deletion, connection change and affects-mask change (`vdf/network.h:351-378`, `vdf/scheduleInvalidator.h:40-77`);
this registration is documented thread-safe and callable during execution (`vdf/network.h:355-358`). Re-schedule
only when `!schedule.IsValid()` (this is exactly what `Exec_RequestImpl::_Schedule` does, `exec/requestImpl.cpp:479-485`).
Scheduling cost scales with the number of nodes reachable from the request; usdRig reschedules every frame
(`moverGraph.cpp:1438-1440`).

### 4.3 Executors and partial re-execution

| Executor | Engine / data manager | Parallel? | Buffer passing | Notes |
|---|---|---|---|---|
| `VdfSimpleExecutor` | serial loop over schedule nodes; `VdfParallelDataManagerVector` | no | **no** ("does not support buffer passing", `vdf/simpleExecutor.cpp:136-147`; copies READWRITE inputs) | what usdRig uses (`moverGraph.cpp:1441`) |
| `VdfExecutor<VdfPullBasedExecutorEngine, VdfDataManagerVector<Background\|Immediate>>` | pull-based recursive | no | yes (`vdf/pullBasedExecutorEngine.h:204-213`) | mung-buffer locking, SMBL |
| `VdfExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>` | `WorkTaskGraph` + `WorkIsolatingDispatcher` (`vdf/parallelExecutorEngineBase.h:595-598,648-700`) | **yes** | yes | releases the Python GIL (`:660`); instantiation at `vdf/testenv/testVdfSpeculation.cpp:319-320` |
| `EfPageCacheExecutor<Engine, DM>` | as above + `EfPageCacheStorage` keyed by an output's value (time) | as engine | yes | what `Exec_Runtime` uses (`exec/runtime.cpp:53-70`); `SetOutputValue` on the key output selects the page (`ef/pageCacheBasedExecutor.h:147-162`) |
| `VdfSubExecutor<...>` / `EfMaskedSubExecutor` | child reading a parent's data | as engine | yes | how `ComputeWithOverrides` isolates overrides (`exec/runtime.cpp:220-296`) |

Partial re-execution is the *default* behaviour of a persistent executor: `Run(schedule)` skips requested outputs that
already have a valid cache (`vdf/pullBasedExecutorEngine.h:348-356`; parallel: `_RunOutput`), and the pull truncates at
any cached output (`executionSystemDesign.md` "Evaluation"). Validity is stored per output as a `VdfExecutorBufferData`
(cache pointer + `_mask` of computed elements, `vdf/executorBufferData.h:184-187`) and a
`VdfExecutorInvalidationData` that tracks `AllOnes | AllZeros | Sparse` invalid state with a mask
(`vdf/executorInvalidationData.h:94-99,105-134`). A read succeeds only if the cached mask `Contains` the requested
mask (`vdf/executorDataManager.h:273-284`).

Invalidation API (`vdf/executorInterface.h:188-190`):

```cpp
void InvalidateValues(const VdfMaskedOutputVector &invalidationRequest);  // "optimized vectorized traversal"
void InvalidateTopologicalState();   // must follow network edits (:191-203, marked XXX by the authors)
void ClearData(); void ClearDataForOutput(VdfId outputId, VdfId nodeId);  // :207-213
void SetOutputValue(const VdfOutput&, const VdfVector&, const VdfMask&);   // :121-124 (inject a value)
bool TakeOutputValue(const VdfOutput&, VdfVector*, const VdfMask&);        // :132-135 (steal a result, no copy)
const VdfVector *GetOutputValue(const VdfOutput&, const VdfMask&) const;   // :141-145
```

`VdfExecutorInvalidator` walks output→dependent outputs using `ComputeOutputDependencyMask(s)` and stops where an
output is already invalid for that mask (`vdf/executorInvalidator.cpp:256-269`); it memoizes dependencies per
`(output, mask)` and keeps an LRU of 16 replayable traversals keyed by the request (`vdf/executorInvalidator.h:51,98-103,166-173`),
so repeated edits of the same parameter are cheap. Pool outputs are visited in pool-chain order (`:239-247`).
`Exec_Runtime::InvalidateExecutor` shows the required discipline: compare `network.GetVersion()` against the last
seen version and call `InvalidateTopologicalState()` before `InvalidateValues` (`exec/runtime.cpp:119-142`).
`testVdfEdit.cpp` demonstrates the loop with a persistent `VdfSimpleExecutor _exec` (`:350`): schedule → `Run` →
`InvalidateValues(outputs with affects masks)` → `Run` again (`:302-332`).

Masks for sparse recomputation: an invalidation request carries a `VdfMask` per output; if a node's
`_ComputeOutputDependencyMask` maps input elements to output elements (identity for per-curve stylers), only those
elements downstream go invalid, and `VdfReadWriteIterator` on a pool output "will be limited to those set in the
affects mask" (`vdf/readWriteIterator.h:44-46,306-309`). Note a subtlety in the parallel data manager: a computed
element mask is per output; the engine publishes `private → public` buffers (`vdf/parallelDataManagerVector.h:124-168`).

Interruption and stats: `SetInterruptionFlag(const std::atomic_bool*)` lets a render thread abort a long run
(`vdf/executorInterface.h:261-277`; engines check `HasBeenInterrupted`, `vdf/pullBasedExecutorEngine.h:362-365`);
`SetExecutionStats(VdfExecutionStats*)` records per-node evaluate/copy events (`vdf/executionStats.h:55-70,140-162`).

### 4.4 Getting "dirty" out of a hand-built network

ExecUsd's request invalidation is implemented with `EfLeafNode`s: "a terminal node, which is never executed ...
visited during invalidation" (`ef/leafNode.h:31-37`); the compiler makes one per value key
(`exec/leafCompilationTask.cpp:98-99`), and `EfLeafNodeCache` (an `EfDependencyCache` wrapper) answers "which leaf
nodes depend on these outputs" with incremental updates from `VdfNetwork::EditMonitor` callbacks
(`ef/leafNodeCache.h:37-100`; wiring example `ef/testenv/testEfLeafNodeCache.cpp:54-79`). A hair engine can attach
one `EfLeafNode` per Hydra-visible result (e.g. final curves of a description) and use
`EfLeafNodeCache::FindNodes(invalidatedOutputs, true)` to produce the `PrimsDirtied` set — the same mapping
`Exec_RequestImpl::_BuildLeafNodeToIndexMap` builds (`exec/requestImpl.cpp:592-620`). Caveat:
`EfDependencyCache::FindOutputs/FindNodes` are documented **not thread safe** (`ef/dependencyCache.h:81-87`).

### 4.5 Feeding external inputs (attribute values, scene-index data, time)

Three equivalent mechanisms, all verified:

1. Mutate a `VdfInputVector<T>` (`SetValue`, `vdf/inputVector.h:84-104`) and call `executor.InvalidateValues({ {node->GetOutput(), mask} })`.
2. `executor.SetOutputValue(output, VdfTypedVector<T>(value), VdfMask::AllOnes(1))` on a `VdfRootNode` output —
   exactly how `Exec_Runtime::SetTime` injects `EfTime` into the `EfTimeInputNode` (`exec/runtime.cpp:81-111`),
   followed by an invalidation of time-dependent outputs (`exec/system.cpp:47-81`).
3. `EfInputValueBlock::AddOutputValuePair(maskedOutput, value)` + `Apply(executor, &invalidationRequest)`
   (`ef/inputValueBlock.h:72-101`; note "currently only supports single valued outputs", `:74-75` — use
   `AddOutputVectorPair` with a prepared `VdfVector` for arrays, `:84-90`).

None of this cares where the value came from: a Hydra `HdSampledDataSource::GetValue(t)` result converted with
`ExecTypeRegistry::GetInstance().CreateVector(vtValue)` (`exec/typeRegistry.h:128`) is a valid `VdfVector`.

### 4.6 What usdRig actually does (and does not do)

- Build: `RigExecMoverGraph::AddPointSource` creates a `VdfInputVector<GfVec3f>(&_network, count)` and returns
  `VdfMaskedOutput(source->GetOutput(), VdfMask::AllOnes(count))` (`libs/rigExec/moverGraph.cpp:1387-1401`).
  `AddRevision` bakes parameters and status into two `VdfInputVector<..>(…,1)` nodes and connects the
  `previous` READWRITE input with the previous head's mask (`:1403-1434`). `_RevisionNode` declares
  `.ReadConnector<RigExecMoverParameters>(parameters) .ReadConnector<RigExecMoverStatus>(status)
  .ReadWriteConnector<GfVec3f>(previous, out)` (`:52-73`) and passes through with
  `ctx.SetOutputToReferenceInput(previous)` when disabled (`:88-91`).
- Evaluate: a **new** `VdfSchedule` and **new** `VdfSimpleExecutor` per call, result copied into a `VtVec3fArray`
  (`:1436-1452`). The evaluator constructs `RigExecMoverGraph graph;` per target chain per `Evaluate(time)`
  (`libs/rigExec/rigEvaluator.cpp:7009`) and even re-evaluates the partial chain for "current-phase" weights
  (`:7104`). Kernels copy the READWRITE buffer into a scratch `std::vector` and back (`moverGraph.cpp:87-130`).
- So: no persistent executor, no `InvalidateValues`, no per-element masks, serial execution, and one
  `ExecUsdSystem` per rig via `RigExecTapSet` for the transform side (`libs/rigExec/tapSet.h:122-177`,
  `tapSet.cpp:20,80-88,126,150-151`). This is a correct but per-frame-rebuild design; the hair plan should not
  inherit "dynamic like usdRig" literally.

---

## 5. Time in VDF/ef, and evaluating at time t with scene-index inputs

- `EfTime { UsdTimeCode _timeCode; uint8_t _splineFlags; }` (`ef/time.h:36-165`); default-constructed = Default time.
  Hashable, ordered (default < all numeric times, `:111-128`). No "pre-time"/left-side evaluation in 26.08
  (`execusd-api-notes.md` §2).
- `EfTimeInterval` = `GfMultiInterval` + a `defaultTime` bit (`ef/timeInterval.h:35-224`) with `Contains(EfTime)`,
  `IsFullInterval()`, `|=`, `&=`, `Extend(left,right)`. Value-invalidation callbacks deliver one.
- `EfTimeInputNode` is a `VdfRootNode` with one `EfTime` output (`ef/timeInputNode.h:26-46`); `Exec_Program` owns
  exactly one and forbids creating more via `CreateNode` (`exec/program.h:455-457`). In a hand-built network you
  create your own.
- Attribute inputs are nodes with a `time` input of type `EfTime` and one `out` (`exec/attributeInputNode.cpp:27-43`);
  `Compute` calls `EsfAttributeQuery::Get(&value, time)` (`:69-81`). Time dependency is a per-node flag from
  `ValueMightBeTimeVarying()` (`:54-60`) and the output→time dependency mask is empty when not time varying
  (`:103-117`), which is what makes `ChangeTime` invalidate sparsely.
- The page cache: `EfPageCacheStorage::New<EfTime>(timeOutput, &leafNodeCache)` (`exec/runtime.cpp:45-49`);
  values are committed per requested output on each run when caching is enabled and under the memory limit
  (`ef/pageCacheBasedExecutor.h:232-268`, `EfPageCacheStorage::SetMemoryUsageLimit`, `ef/pageCacheStorage.h:94-95`);
  invalidation is by predicate over page keys (`exec/runtime.cpp:144-155`). `Resize` is not thread-safe
  (`ef/pageCacheStorage.h:159-164`).

Feeding scene-index (not stage) inputs: two options.

- **Bypass exec entirely** (recommended for the data plane): a hand-built network with root nodes per Hydra input
  (surface points/normals/topology from the upstream scene index, painted maps, etc.); on `PrimsDirtied` for those
  locators, re-pull the data source at the current time, `SetOutputValue`/`SetValue`, `InvalidateValues`. Time is
  just another root value (`EfTimeInputNode`) or is folded into the pull.
- **Custom scene adapter for exec** (possible, not endorsed): implement `EsfStageInterface` (`esf/stage.h:41-99`),
  `EsfPrimInterface` (`esf/prim.h:36-76`), `EsfAttributeInterface` + `EsfAttributeQueryInterface`
  (`esf/attribute.h:35-60`, `esf/attributeQuery.h:33-112`: `_Get(VtValue*, UsdTimeCode)`, `_GetSpline`,
  `_ValueMightBeTimeVarying`, `_IsTimeVarying(from,to)`), `EsfRelationshipInterface` (`esf/relationship.h:39-50`),
  `EsfObjectInterface` (`esf/object.h:44-151` incl. `GetSchemaConfigKey`, `GetIncomingConnections`, metadata), each
  fitting the fixed-size holders (48 bytes for objects, 176 for queries, `esf/object.h:159`, `esf/attributeQuery.h:122`);
  derive `ExecSystem` (protected ctor takes `EsfStage&&`, `exec/system.h:53-54`) and push changes through
  `ExecSystem::_ChangeProcessor::{DidResync, DidChangeInfoOnly, DidChangeIncomingConnections}`
  (`exec/systemChangeProcessor.h:43-61`); derive `Exec_RequestImpl` (`exec/requestImpl.h:83-113`, all `EXEC_API`).
  Schema/type resolution still goes through `UsdSchemaRegistry` (`esf/stage.h:71-81`), so prims must have real
  TfTypes. The work is real (esfUsd is ~10 files plus `EsfUsdStageData` connection tables, `esfUsd/stageData.h:54-246`)
  and the interfaces are explicitly unstable. UNVERIFIED: whether a scene index can supply `GetIncomingConnections`
  cheaply enough; esfUsd builds a whole-stage table for it.

`usdExecImaging` shows Pixar's intended Hydra loop (26.08): `UsdExecImaging_Request` owns one `ExecUsdSystem` and
one `ExecUsdRequest` built by traversing the stage and asking per-type adapters for value keys
(`usdExecImaging/request.cpp:206-257`); data sources are lazy `HdSampledDataSource`s that call
`_cacheView->Get(index)` (`request.cpp:124-149`, `computedDataSource.cpp:49-53`); invalidation callbacks map
indices → adapters → `HdDataSourceLocatorSet` per prim (`request.cpp:294-330`); the engine calls `SetTime` (→
`ChangeTime`+`Compute`+`PrimsDirtied`, `stageSceneIndex.cpp:53-61`) and `ApplyPendingUpdates`
(`stageSceneIndex.cpp:63-70`) from `UsdImagingGLEngine::_PreSetTime`/`Render` on the main thread
(`usdImagingGL/engine.cpp:485-497,2372-2374`), and merges it *in front of* the stage scene index via
`HdMergingSceneIndex` + `HdNoticeBatchingSceneIndex` (`engine.cpp:1617-1645`). Motion samples: the data source
returns `false` from `GetContributingSampleTimesForInterval` (`computedDataSource.cpp:55-62`) — single sample only.
The adapter registry is a hard-coded `if` chain (`adapterRegistry.cpp:28-51`) — no plugin extension in 26.08.

---

## 6. Memory management

| Topic | Fact | Source |
|---|---|---|
| `VdfVector` storage impls | Empty, Single, Contiguous (dense), Compressed (sparse index mapping), Boxed, Shared | `vdf/vector.h:18-23` |
| Setting a value | `Set(T&&)` moves into a `Vdf_VectorImplSingle<T>`; `Set(Vdf_BoxedContainer<T>&&)` moves boxed; `Resize<T>(n)` allocates dense default-initialized (Gf types stay uninitialized) | `vdf/vector.h:161-172,203-209,217-235` |
| Copies | `VdfVector(const VdfVector&)` clones; `Copy(rhs, mask)` clones a subset; `Merge` writes elements under a mask; `operator=` clones ("expensive if not shared") | `vdf/vector.h:61-86,281-325,556-566` |
| Zero-copy sharing | `Share()` wraps the impl in a ref-counted `Vdf_VectorImplShared` (not thread safe); only allowed when `size >= _VectorSharingSize = 5000` elements (dense or boxed); `GetReadWriteAccessor` detaches (copy-on-write) | `vdf/vector.h:334-351,456-470`; `vdf/vectorImpl_Boxed.h:106-108`; `vdf/vectorImpl_Contiguous.h:283-286`; `vdf/vectorData.h:42` |
| `VtArray` extraction | `ExtractAsVtArray<T>(size, offset)`: shared → `VtArray` with foreign data source (no copy); else copies. Compressed vectors always copy. | `vdf/vector.h:375-404,655-677`; `vdf/vectorImpl_Shared.h:130,167-203` |
| Who shares? | No production code in `vdf`/`ef`/`exec` calls `Share()` (only tests). So the exec cache-view path copies arrays on every `Get`. A hand-built engine can call `Share()` itself on big outputs before handing them to Hydra. | grep result over `pxr/exec` |
| Per-output caches | one `VdfExecutorBufferData` per output (`std::atomic<VdfVector*>` + flags + mask); parallel DM keeps private/scratch/public buffers and publishes; caches are retained across runs and re-used (`CreateExecutorCache` reuses an owned buffer) | `vdf/executorBufferData.h:184-217`; `vdf/parallelDataManagerVector.h:124-168` |
| Buffer passing | READWRITE outputs pass the upstream buffer forward instead of copying (`_PrepareReadWriteBuffer`, "mung buffer locking" keeps a buffer at invalidation edges); `VdfSimpleExecutor` copies instead | `vdf/pullBasedExecutorEngine.h:204-213,442-477`; `vdf/simpleExecutor.cpp:136-147` |
| Pass-through | `ctx.SetOutputToReferenceInput(inputName)` makes the output reference the input's cache without copying | `vdf/context.h:176-187`; used by usdRig `moverGraph.cpp:88-91` |
| `SetOutput` | `SetOutput(const T&)` always copies; `SetOutput(T&&)` moves | `vdf/context.h:109-162` |
| Deallocation | `VdfDataManagerVector<Background>` frees old data vectors on a background thread; `Immediate` frees inline | `vdf/dataManagerVector.h:23-33,274-282` |
| Masks | `VdfMask` wraps `TfCompressedBits` (RLE), interned in a global registry with a lock — the read/write iterator avoids the registry for temporary bit sets "to avoid contention on the mask registry lock" | `vdf/mask.h:43,605-674`; `vdf/readWriteIterator.h:186-189` |
| Page cache budget | `EfPageCacheStorage::SetMemoryUsageLimit(bytes)`; caching stops at the limit (`HasReachedMemoryLimit`) | `ef/pageCacheStorage.h:76-95`; `ef/pageCacheBasedExecutor.h:276-283` |
| Estimating | `VdfVector::EstimateElementMemory()`, `VdfNode::GetMemoryUsage()`, `VdfNetwork::DumpStats` | `vdf/vector.h:592-595`; `vdf/node.h:339-340`; `vdf/network.h:273-274` |

Holding `VtArray`s inside a `VdfVector`: forbidden as an exec value type (`exec/typeRegistry.h:101-104`), but VDF
itself only requires default- and copy-constructibility plus a registered fallback (`vdf/vector.h:49-53`,
`vdf/executionTypeRegistry.h:64-93`), so a hand-built network may register a struct that *contains* a `VtArray`
(copy-on-write semantics then make node-to-node copies cheap). usdRig's `RigExecMoverParameters` already carries
`std::vector` members through VDF (`moverGraph.cpp:52-73`).

---

## 7. Concurrency rules (verified)

| Rule | Evidence |
|---|---|
| `VdfNode::Compute` may be called concurrently on the same node; dependency-mask virtuals likewise | `vdf/node.h:350-353,610-613,659-662` |
| Parallel engine runs inside an isolating dispatcher and a private task graph; posts transported errors on the calling thread; releases the GIL | `vdf/parallelExecutorEngineBase.h:648-700` |
| Concurrent network *construction/connection* is supported (concurrent containers + spin mutex on outputs); `Delete` may reindex nodes | `vdf/network.h:203-204,387-399`; `vdf/output.h:187`; `testVdfNetworkThreading.cpp` |
| Editing topology while an executor runs is **not** safe: schedules referencing the network are invalidated on edits (the registration path is thread-safe, the executor engine is not guarded); `InvalidateTopologicalState` "must be called after changes" | `vdf/executorInterface.h:191-203`; `vdf/scheduleInvalidator.h:52-72` |
| `VdfVector::Share()` not thread safe; `EfPageCacheStorage::Resize` not thread safe; `EfDependencyCache::Find*` not thread safe | `vdf/vector.h:332`; `ef/pageCacheStorage.h:159-164`; `ef/dependencyCache.h:81-87` |
| `ExecUsdCacheView::Get` may be called from many threads concurrently (test extracts under `WorkParallelForN`) | `execUsd/testenv/testExecUsdRequest.cpp:154-157` |
| ExecUsd invalidation callbacks run synchronously on the thread that authored / called `ChangeTime`; do not compute inside them | `execUsd/system.cpp:140-190`; `execusd-api-notes.md` §6 |
| `ExecUsdSystem` subscribes through `EsfUsdStageData::RegisterStage` (one shared listener per stage, listeners notified under a mutex) | `execUsd/system.cpp:31-58`; `esfUsd/stageData.h:84-101,239-241` |
| Hydra: `HdSceneIndexBase::GetPrim` can be called from render/sync threads; `usdExecImaging` only *reads* the last cache view there and does all Compute in `SetTime`/`ApplyPendingUpdates` on the engine thread; usdRig likewise serializes evaluate-then-publish under a mutex and its results scene index "never computes, waits, changes time, or locks the authoring stage" in `GetPrim` | `usdExecImaging/stageSceneIndex.cpp:23-70`; `usdImagingGL/engine.cpp:485-497,2372-2374`; `libs/rigExecImaging/registry.cpp:343-345`; `_rigExecImagingHeaders.txt` (RigExecResultsSceneIndex doc) |
| Can VDF executors run while Hydra pulls? Yes if the executor's data is not what Hydra reads: publish immutable snapshots (usdRig's `RigExecSnapshotStore`) or a separate executor per generation; reading `GetOutputValue` from one executor while `Run` mutates it is a data race (buffer publish is per output, `vdf/parallelDataManagerVector.h:154-168`) | inference from the above; UNVERIFIED for any hidden internal locking |

---

## 8. Recommendation and trade-offs

Requirements restated: relationship-wired operator chains; topology-changing generators (variable curve counts);
per-node result caching; dirty propagation from attribute edits; evaluation on a render/update thread; 30+ fps.

### (a) Hand-built `VdfNetwork` (like usdRig movers, but persistent)

Fits: per-element masks and sparse invalidation (§4.3), READWRITE buffer passing so a 5-styler chain touches one
buffer (`vdf/pullBasedExecutorEngine.h:204-213`), parallel evaluation across independent descriptions/patches via
`VdfExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>`, persistent executor with per-output caches
(per-node result caching for free), dependency-replay invalidation (`vdf/executorInvalidator.h:98-103`), leaf nodes →
`PrimsDirtied` (§4.4), external inputs from a scene index (§4.5), interruption flag for cancel-on-scrub.
Variable curve counts: generators produce a *boxed* output (length in data) or you re-create the downstream
`VdfInputVector`/pool sizes on count change and treat it as a topology edit (`InvalidateTopologicalState`,
reschedule). Costs/risks: the executor/data-manager API is undocumented beyond headers, has `XXX` notes
(`vdf/executorInterface.h:191-200`), and is versioned with OpenUSD (26.08 marks exec "experimental"); you must
implement `_ComputeOutputDependencyMask` per node to get sparsity; every node type is a C++ class (fine — operators
are C++ anyway); mask registry lock contention on many small masks (`vdf/readWriteIterator.h:186-189`). usdRig
proves the build side works; the incremental side is unproven in this codebase.

### (b) ExecUsd computations registered per schema (like usdRig transforms)

Fits: parameters and small per-prim values; automatic invalidation on authoring; time-keyed page cache; works
with codeless schemas; `NamespaceAncestor`/`Relationship` accessors express "styler → generator → description"
chains as `Relationship(inputs:source).TargetedObjects<HairPacket>(computeCurves)`. Does not fit the data plane:
whole-array boxed values with copies on entry (`typeRegistry.h:231-232`) and extraction (§6), no per-element
invalidation, no attribute-of-target accessor, array overrides impossible, one global time, and recompilation on
every `Compute` (`exec/requestImpl.cpp:385-388`). Also all inputs must come from the *stage*; anything deformed by
usdRig or another scene-index modifier is invisible unless usdRig's own exec computation is the provider (it is for
transforms; deformed points come out of usdRig's VDF mover graph and would have to be pushed in as a boxed
override, `ComputeWithOverrides`, paying a second pass).

### (c) Bespoke DAG executor on TBB (no VDF)

Fits: complete control, `VtArray` copy-on-write throughout, trivial per-curve chunking with `tbb::parallel_for`,
easy per-node caching keyed on input hashes, easy variable-count topology, no undocumented dependency. Costs: you
re-implement dependency masks, replayable invalidation, buffer passing/aliasing and scheduling — the parts VDF
already has and Pixar has tuned. Sparse per-element invalidation is the hard one; a chunked design (per-patch or
per-N-curves task granularity with per-chunk dirty bits) gets most of the benefit with far less machinery.

### Suggested split

- **Control plane (prims, parameters, wiring)**: ExecUsd computations on the hair schemas producing a small
  registered "operator descriptor" struct per prim (like usdRig's `RigExecMoverParameters`), with relationship
  accessors doing the chain resolution and exec's notices doing dirty detection. This is (b) used for what it is good
  at, and it reuses usdRig's transform/skin outputs where they are exec computations.
- **Data plane (curves)**: (a) if the team accepts owning the VDF executor surface — build one persistent
  `VdfNetwork` per stage, one parallel executor, pool outputs for CV buffers with affects masks per description,
  `EfLeafNode`s per Hydra output, and treat generator count changes as topology edits. Otherwise (c) with a
  chunked, hash-cached TBB graph. Either way, do *not* copy usdRig's per-frame rebuild + `VdfSimpleExecutor`.
- **Hydra integration**: follow `usdExecImaging`'s loop (compute on the engine's update path, lazy data sources
  reading an immutable cache/snapshot, dirty locators from invalidation callbacks) and usdRig's immutable-snapshot
  publication for the render thread; never compute inside `GetPrim`.

---

## Key facts

- `esf`/`esfUsd` are marked not for public use — `<openusd-src>/pxr/exec/esf/README.md:3-4`, `esfUsd/README.md:3-4`; yet all exec libs/headers are installed — `$USD/include/pxr/exec/*`, `lib/libusd_vdf.so` etc.
- ExecUsd public surface is `ExecUsdSystem{ChangeTime, BuildRequest, PrepareRequest, Compute, ComputeWithOverrides}`, `ExecUsdRequest::IsValid`, `ExecUsdCacheView::Get(int)`, `ExecUsdValueKey` — `execUsd/system.h:48-154`, `execUsd/request.h:31-63`, `execUsd/cacheView.h:38-39`, `execUsd/valueKey.h:61-70`.
- Invalidation callback types: `ExecRequestIndexSet = pxr_tsl::robin_set<int>`; value callback gets an `EfTimeInterval` — `exec/request.h:19,28-41`.
- `Compute()` always recompiles the request; schedule rebuilt only when invalid; interest reset per compute — `exec/requestImpl.cpp:385-388,479-485,496-500`.
- Scene changes reach exec synchronously in the `ObjectsChanged` dispatch; Default/Spline/TimeSamples → value invalidation, TargetPaths/ConnectionPaths → uncompile — `execUsd/system.cpp:140-190`, `exec/systemChangeProcessor.cpp:68-109`.
- `ChangeTime` invalidates only attribute inputs that are time-varying between old and new time — `exec/system.cpp:47-81`, `exec/attributeInputNode.cpp:54-67,103-117`.
- Main exec executor is `EfPageCacheExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>` (parallel by default via `VDF_ENABLE_PARALLEL_EVALUATION_ENGINE`) with a page cache keyed by `EfTime` — `exec/runtime.cpp:45-70`, `vdf/types.cpp:13-16`.
- `VtArray` attributes become a single boxed data-flow element (`Vdf_BoxedContainer`, mask size 1), copied on entry — `exec/typeRegistry.h:219-240`, `exec/attributeInputNode.cpp:75-79`, `vdf/boxedContainer.h:75-81`.
- `ExecTypeRegistry::RegisterType` forbids `VtArray` and requires equality — `exec/typeRegistry.h:101-107`.
- Extraction copies unless the `VdfVector` was `Share()`d (≥5000 elements); no production `Share()` caller exists — `vdf/vector.h:375-404`, `vdf/vectorImpl_Boxed.h:106-108`, `vdf/vectorData.h:42`, grep of `pxr/exec`.
- Override values must match the compiled output type exactly (arrays cannot be overridden) — `exec/system.cpp:168-181`; `ComputeWithOverrides` runs a sub-executor and over-invalidates — `exec/runtime.cpp:200-303`.
- Accessor gaps: `// XXX:TODO Property, NamespaceParent, NamespaceChildren` and `AnimSpline` — `exec/computationBuilders.h:584,840,1084,1138`; provider traversals are exactly five — `exec/providerResolution.h:41-59`.
- usdRig rebuilds `RigExecMoverGraph` per chain per `Evaluate` and uses a fresh `VdfSchedule` + `VdfSimpleExecutor` per call — `libs/rigExec/rigEvaluator.cpp:7009`, `libs/rigExec/moverGraph.cpp:1436-1443`.
- usdRig's revision node uses a READWRITE `previous→out` connector and `SetOutputToReferenceInput` pass-through — `libs/rigExec/moverGraph.cpp:52-73,88-91`.
- `VdfSimpleExecutor` is serial and copies READWRITE buffers ("does not support buffer passing") — `vdf/simpleExecutor.cpp:69-118,136-147`.
- Parallel standalone executor: `VdfExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>` — `vdf/executor.h:35-63`, `vdf/testenv/testVdfSpeculation.cpp:319-320`; engine uses `WorkTaskGraph` + `WorkIsolatingDispatcher` and releases the GIL — `vdf/parallelExecutorEngineBase.h:595-598,648-700`.
- Executor API for incremental use: `InvalidateValues`, `InvalidateTopologicalState`, `SetOutputValue`, `TakeOutputValue`, `GetOutputValue`, `SetInterruptionFlag`, `SetExecutionStats` — `vdf/executorInterface.h:62-76,121-145,188-213,261-277,290-297`.
- Per-output validity is a sparse mask state machine; reads require the cached mask to contain the requested mask — `vdf/executorInvalidationData.h:105-163`, `vdf/executorDataManager.h:273-284`.
- Invalidation traversal memoizes dependencies and replays an LRU of 16 requests; stops at already-invalid outputs — `vdf/executorInvalidator.h:51,98-103`, `vdf/executorInvalidator.cpp:27-55,256-269`.
- Schedules are auto-invalidated on topology/affects-mask edits (thread-safe registration) — `vdf/network.h:351-378`, `vdf/scheduleInvalidator.h:40-77`; `Exec_Runtime` checks `network.GetVersion()` before invalidating — `exec/runtime.cpp:119-142`.
- Network edits: `Connect`/`Disconnect`/`Delete`/`DisconnectAndDelete`/`ReorderInputConnections`/`EditMonitor`; concurrent node creation supported — `vdf/network.h:138-261,387-399`, `vdf/testenv/testVdfNetworkThreading.cpp:74-111`; subgraph deletion via `VdfIsolatedSubnetwork` — `vdf/isolatedSubnetwork.h:77-99`; dynamic connectors via `VdfExtensibleNode` — `vdf/extensibleNode.h:45-56`.
- `VdfReadWriteIterator::Allocate` allocates a boxed value (wrong on READWRITE pools; usdRig trap) and iterates by affects mask — `vdf/readWriteIterator.h:215-234,292-309`; `docs/mover-graph-cutover.md` "Two traps".
- Time: `EfTime = UsdTimeCode + spline flags`; `EfTimeInputNode` is a root node; value set with `SetOutputValue` — `ef/time.h:36-165`, `ef/timeInputNode.h:26-46`, `exec/runtime.cpp:81-111`.
- Leaf nodes and `EfLeafNodeCache` provide "which outputs depend on X" for dirty notification; `EfDependencyCache::Find*` not thread safe — `ef/leafNode.h:31-37`, `ef/leafNodeCache.h:37-100`, `ef/dependencyCache.h:81-87`.
- External value injection: `EfInputValueBlock::Apply` (single-valued outputs only via `AddOutputValuePair`) — `ef/inputValueBlock.h:72-101`.
- A non-USD scene adapter is structurally possible (`ExecSystem(EsfStage&&)` protected ctor; `_ChangeProcessor` public API; `Exec_RequestImpl` derivable) but uses non-public interfaces — `exec/system.h:53-54`, `exec/systemChangeProcessor.h:23-61`, `exec/requestImpl.h:83-113`, `esf/stage.h:41-99`.
- 26.08 ships `usdExecImaging`: exec-driven initial scene index merged in front of the stage scene index when `USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX=1` (default false); hard-coded adapters; compute on `SetTime`/`ApplyPendingUpdates` from the engine thread; lazy data sources read the cache view — `usdImagingGL/engine.cpp:90-96,485-497,1617-1645,2372-2374`, `usdExecImaging/request.cpp:206-330`, `usdExecImaging/adapterRegistry.cpp:19-51`, `usdExecImaging/stageSceneIndex.cpp:53-70`.
- Debug/env knobs: `VDF_ENABLE_PARALLEL_EVALUATION_ENGINE`; `TF_DEBUG=EXEC_REQUEST_INVALIDATION,EXEC_REQUEST_EXPIRATION,VDF_SCHEDULING,VDF_MUNG_BUFFER_LOCKING,VDF_SPARSE_INPUT_PATH_FINDER,USDEXECIMAGING_REQUEST,USDEXECIMAGING_GRAPH_AFTER_REBUILD`; `USDEXECIMAGING_ENABLE_USDGEOM_XFORMABLE_ADAPTER` — `vdf/types.cpp:13`, `exec/debugCodes.cpp:15-18`, `vdf/debugCodes.cpp:13-19`, `usdExecImaging/debugCodes.cpp:15-19`, `usdExecImaging/adapterRegistry.cpp:19-26`.
- Notice-listener ordering pitfall: register the app's re-evaluation listener before constructing `ExecUsdSystem` — `libs/rigExecImaging/registry.cpp:225-262`.

## Open questions

- Whether a persistent `VdfExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>` on a hand-built network
  with pool outputs actually delivers sparse (per-curve) recomputation end-to-end without exec's compiler-provided
  dependency masks; nothing in this repo or in `pxr/exec` tests exercises a large pool with sparse `InvalidateValues`
  plus parallel `Run` (only `testVdfEdit.cpp` with `VdfSimpleExecutor`). Needs a prototype benchmark.
- Whether any internal executor lock makes `GetOutputValue` safe concurrently with `Run` (assumed unsafe; UNVERIFIED).
- The exact cost of `VdfScheduler::Schedule` for ~thousands of nodes (relevant if generators change curve counts often); no measurements exist here.
- Whether `VdfVector::Share()` can be used from a node callback on its own output (to make Hydra extraction zero-copy) without breaking mung-buffer locking; only tests use it.
- Whether Pixar intends `usdExecImaging`'s adapter registry to become pluggable (26.08 is a hard-coded `if` chain), and whether a plugin can legally add an initial scene index in front of `UsdImagingStageSceneIndex` outside `usdImagingGL` (usdview uses `UsdImagingGLEngine`, so the env setting path exists but a second exec scene index would need its own merging).
- Feasibility/cost of an `EsfStageInterface` over a Hydra scene index: `GetIncomingConnections` and `GetSchemaConfigKey` semantics for non-stage prims are unclear (UNVERIFIED), and the API is declared non-public.
- Whether `EfPageCacheStorage`'s global memory limit default (static `_numBytesLimit`) is set anywhere; `SetMemoryUsageLimit` exists but I did not find the default initializer value.
- `Vdf_BoxedRanges` "logical groups" (`vdf/boxedContainer.h:18-71`) and `VdfSubrangeView` might allow per-curve grouping inside a boxed value (curve = subrange); whether invalidation can be scoped per subrange is UNVERIFIED (masks are per data-flow element, so probably not).
