# G — Evaluation scheduling and batching for the hair graph

**Gap key:** `evaluation-scheduling-and-batching`
**Question:** *When*, and on *which thread*, does the hair graph evaluate, and how do the several
per-frame dirty notices coalesce into exactly one run?

Everything below is either (a) cited to `file:line` in `<openusd-src>` (v26.08,
`ee47c679a`) or `<usdrig-src>`, or (b) produced by a probe I built and ran headlessly.
Probe sources and captured output live in
`<session-scratch>`
(`evalSched.cpp`, `models.cpp`, `chainOrder.cpp`, `asyncProbe.cpp`, `noticeCost.cpp`; outputs
`run1.txt`, `models.txt`, `chainOrder.txt`, `chainOrder_hdgp.txt`, `async.txt`, `noticeCost.txt`).
No GPU or display was used; nothing here is a GPU measurement.

---

## 1. The engine's per-frame order — verified, with one correction

`UsdImagingGLEngine::PrepareBatch` (`pxr/usdImaging/usdImagingGL/engine.cpp:483-498`):

| # | Call | Line | Batched? |
|---|------|------|----------|
| 1 | `_PreSetTime(params)` → `SetRefineLevelFallback` then, **inside a `_ScopedHydraNoticeBatch` on the post-instancing batching SI**, `_usdImagingSceneIndex->ApplyPendingUpdates()` | `engine.cpp:485`, `2354-2386` | yes (`2368-2370`) |
| 1b | if exec is on: `_execStageSceneIndex->ApplyPendingUpdates()` then `_noticeBatchingStageSceneIndex->Flush()` | `engine.cpp:2373-2376` | yes, separately |
| 2 | `_usdImagingSceneIndex->SetTime(params.frame)` | `engine.cpp:488` | **no** |
| 2b | if exec is on: `_execStageSceneIndex->SetTime(frame)` + `Flush()` | `engine.cpp:489-492` | yes, separately |
| 3 | `_SetSceneGlobalsCurrentFrame(params.frame)` → `sgsi->SetCurrentFrame(time.GetValue())` | `engine.cpp:496`, `2062-2073` | **cannot be** (see §3) |
| 4 | `_PostSetTime(params)` — **body is empty**, only `HD_TRACE_FUNCTION()` | `engine.cpp:2388-2392` | — |

**Correction to the brief:** the `_ScopedHydraNoticeBatch` wraps only `ApplyPendingUpdates`, not
`SetTime`. `_PostSetTime` is a no-op, so it is *not* a usable "after the last notice" hook.

Downstream of `PrepareBatch`, `RenderBatch` runs collection/AOV/bbox/dome-light setters
(`engine.cpp:707-736`) and then `_Execute` → `renderControl->Execute(taskPaths)`
(`engine.cpp:737`, `2284-2296`) → `HdRenderIndex::SyncAll`.

**The only hooks that exist between the last scene notice and Rprim sync are:**

* `HdRenderDelegate::Update()`, called first thing in `SyncAll` (`renderIndex.cpp:1604`), documented
  as *"Called at the beginning of HdRenderIndex::SyncAll, before render index prim sync, to provide
  the render delegate an opportunity to directly process change notices…"* (`renderDelegate.h:540-546`).
  **No in-tree render delegate overrides it** (`grep "Update() override" pxr/imaging/` → 0 hits), and
  a scene-index plugin cannot reach it.
* Task `Sync` (`renderIndex.cpp:1624-1660`), which runs before Rprim sync — reachable only by owning a task.
* `HdRenderIndex::MergingSceneIndexNoticeBatchBegin/End` (`renderIndex.h:472-480`,
  `renderIndex.cpp:928-938`) — a public app-level batching bracket. **Nothing in OpenUSD calls it**
  (`grep` finds only the declaration/definition), and `UsdImagingGLEngine` does not expose the
  render index to Python, so a *usdview plugin* cannot use it.

⇒ **A renderer-level scene-index plugin has no engine-provided "end of notices" callback.** It must
either cook inside notice handlers, or be given a commit point by the application.

---

## 2. `HdNoticeBatchingSceneIndex` semantics — coalescing, not deduplication

`pxr/imaging/hd/noticeBatchingSceneIndex.cpp`:

* While enabled, notices append into `_batches`, and **a notice merges into the previous batch entry
  only if the previous entry has the same type** (`:41-56` added, `:88-113` dirtied). An interleaved
  `PrimsAdded` splits the dirtied run into two entries.
* `Flush()` replays batch entries in order, one `_SendPrims*` per entry (`:128-149`).
* `SetBatchingEnabled(false)` flushes (`:118-126`).
* **No deduplication and no locator merging**: 3 edits + N time-varying prims produce one call with
  `3+N` entries, with duplicate prim paths preserved (probe S9e below).
* `GetPrim`/`GetChildPrimPaths` pass straight through (`:23-32`) — **batching delays notices but never
  delays data.** A consumer that pulls during a batch sees post-edit values with no dirty yet.

Two batching scene indices exist upstream of a renderer plugin:

| Instance | Created at | Enabled by |
|---|---|---|
| `postInstancingNoticeBatchingSceneIndex` (inside usdImaging, after Ni-prototype propagation) | `usdImaging/sceneIndices.cpp:286-287` | `_ScopedHydraNoticeBatch` around `ApplyPendingUpdates` / `SetStage` (`engine.cpp:2368-2370`, `537-539`) |
| `"Post-Merging Notice Batching Scene Index"` (in the render index, right after `HdMergingSceneIndex`, **before** `AppendSceneIndicesForRenderer`) | `renderIndex.cpp:141-144`, `194-203` | `MergingSceneIndexNoticeBatchBegin/End` — never called in-tree |

---

## 3. Where a hair plugin lands in the chain — measured

`chainOrder.cpp` calls `HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer("GL", …)` exactly as
`renderIndex.cpp:206-214` does, after registering a stand-in for `UsdImagingGLEngine`'s app scene
indices (phase 0, `InsertionOrderAtStart`, all renderers — `engine.cpp:182-193`) and hair candidates at
phases 0/1/2/3/10 with `InsertionOrderAtEnd`. Resolved chain (terminal first, upstream indented):

```
HdDependencyForwardingSceneIndex
  HdsiUnboundMaterialPruningSceneIndex
    HdSt: declare Storm dependencies
      HAIR-CANDIDATE phase 10
        _RenderPassVisibilitySceneIndex
          HdsiBackPlateSceneIndex
            HdsiMaterialPrimvarTransferSceneIndex
              HAIR-CANDIDATE phase 3
                HAIR-CANDIDATE phase 2
                  [HdGpGenerativeProceduralResolvingSceneIndex]   <- only with HDGP_INCLUDE_DEFAULT_RESOLVER=1
                    HdsiRenderPassPruneSceneIndex
                      HAIR-CANDIDATE phase 1
                        HdsiVelocityMotionResolvingSceneIndex
                          HdsiTetMeshConversionSceneIndex
                            HdsiNurbsApproximatingSceneIndex
                              HdSiNodeIdentifierResolvingSceneIndex (glslfx)
                                HdsiMaterialBindingsResolvingSceneIndex
                                  HdsiImplicitSurfaceSceneIndex
                                    HAIR-CANDIDATE phase 0 (AtEnd)
                                      APP: HdsiSceneGlobalsSceneIndex (phase 0/AtStart)
                                        INPUT (post-merging notice batching SI)
```

Consequences, all decision-relevant:

1. **Any renderer-level plugin is strictly downstream of `HdsiSceneGlobalsSceneIndex`.** The
   scene-globals SI is registered at phase 0 with `InsertionOrderAtStart`
   (`engine.cpp:182-193`), so it is always the first thing after the input, and its
   `_SendPrimsDirtied` for `currentFrame` (`hdsi/sceneGlobalsSceneIndex.cpp:176-192`) is emitted
   **downstream of both batching scene indices**. *The frame notice can never be batched together
   with the geometry notices.*
2. **Phase 0 / `InsertionOrderAtEnd` is the right slot for the hair generator**: immediately after
   scene-globals and *upstream of every Storm plugin* — including
   `HdsiVelocityMotionResolvingSceneIndex`, so generated curves get Storm's velocity-based motion
   blur, and `HdSt_DependencySceneIndexPlugin`/`HdDependencyForwardingSceneIndex`.
3. **hdGp is disabled by default** (`hdGp/sceneIndexPlugin.cpp:25`, `HDGP_INCLUDE_DEFAULT_RESOLVER`
   default `false`) and, when enabled, lands *downstream* of velocity-motion resolution — so
   hdGp-generated curves do not get Storm velocity blur. Another reason not to build on hdGp.
4. Redistributable plugins should ship **both** a `plugInfo.json` entry and a
   `TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)` `RegisterSceneIndexForRenderer(...)` call; the registry
   composes JSON tags with the manufactured `phaseN` tags
   (`sceneIndexPluginRegistry.cpp:829-908`, `_CreateTagFromPhase` at `:822-827`).

---

## 4. Probe 1 — the exact notice sequence per frame (`evalSched.cpp`)

Chain: `UsdImagingCreateSceneIndices` → `HdMergingSceneIndex` → post-merging
`HdNoticeBatchingSceneIndex` → `HdsiSceneGlobalsSceneIndex` → recording SI (the hair-plugin stand-in)
→ terminal observer. Stage: 3 meshes with time-sampled `points`, a non-varying `primvars:density`,
and one `BasisCurves` carrying a custom `usdGen:clumpScale`. `engineFrame()` replays
`engine.cpp:483-498` literally.

| Scenario | Post-merge batch | `PrimsDirtied` calls | Entries | Order |
|---|---|---|---|---|
| S1/S2 frame change | off | **2** | 3 points + 1 `sceneGlobals/currentFrame` | points **then** currentFrame |
| S3 frame change | **on** | **2** | same | **currentFrame first**, then the flushed points batch |
| S4 same frame again | off | **0** | — | `SetTime` and `SetCurrentFrame` both early-out |
| S6 three `primvars:density` edits, no frame change | off | **1** | 3 | one `ApplyPendingUpdates` dirty |
| S9d 3 edits + frame change | off | **3** | 3 density + 3 points + 1 frame | density, points, currentFrame |
| S9e 3 edits + frame change | **on** | **2** | 1 + **6** | currentFrame first; density+points **coalesced into one call** |
| S9 new prim + frame change | off | 1 `PrimsAdded` + 2 `PrimsDirtied` | — | added, points, currentFrame |

**S1–S9 before a warm pull produced *zero* time-varying dirties.** Time dependencies are registered
lazily by data-source *pulls* (`_StageGlobals::FlagAsTimeVarying`, `stageSceneIndex.cpp:882-888`,
called from `dataSourcePrim.cpp:43,236,327,486`); `SetTime` then replays the accumulated
`_timeVaryingLocators` in **one** `_SendPrimsDirtied` (`stageSceneIndex.cpp:357-372`, `905-917`).
So the "N scalps" case is **one notice with N entries**, not N notices.

**S5 is the sleeper finding: editing `usdGen:clumpScale` on a `BasisCurves` produced no Hydra notice
at all.** `_ComputeDirtiedEntries` asks the adapter to translate USD property names into locators
(`stageSceneIndex.cpp:796-878`); `UsdImagingBasisCurvesAdapter::InvalidateImagingSubprim`
(`basisCurvesAdapter.cpp:82-94`) returns an empty set for unknown properties.
**⇒ every `usdGen:` property that must drive re-cooking needs a UsdImaging prim- or API-schema
adapter implementing `InvalidateImagingSubprim`, or interactive edits silently never reach Hydra.**

**S10b — the new time is already visible inside the first notice handler.** Reading
`/World/Scalp0` `primvars/points` from inside the *first* `PrimsDirtied` callback of a `SetTime(2.0)`
returned `points[2].z = 0.2`, the frame-2 value (`_stageGlobals._time` is assigned before
`_SendPrimsDirtied`, `stageSceneIndex.cpp:909-916`). A synchronous cook in the notice handler reads
correct post-`SetTime` data.

**S10 — the frame notice is identifiable:** prim path `/`
(`HdSceneGlobalsSchema::GetDefaultPrimPath()`), locator `sceneGlobals/currentFrame`.

---

## 5. Probe 2 — the three models measured head-to-head (`models.cpp`)

10 interactive events (one `primvars:density` edit + one frame change each), then an 8-thread
`GetPrim` storm standing in for Storm's parallel Rprim sync. **Ideal cook count = 10.**

| Model | batch | cooks | dirty callbacks | dirties emitted downstream | distinct cook threads |
|---|---|---|---|---|---|
| **M1 EAGER** — cook in every relevant `_PrimsDirtied` (hdGp's model) | off | **29** | 29 | 29 | 1 (main) |
| M1 EAGER | on | **20** | 20 | 20 | 1 |
| **M2 LAZY** — cook inside `GetPrim` (forbidden by usdRig `docs/spec.md:1584`) | off | 10 | 29 | **0** | **4 (worker threads)** |
| M2 LAZY | on | 10 | 20 | **0** | 4 |
| **M3a DEFERRED** — cook on the `sceneGlobals/currentFrame` notice | off | **10** | 29 | 10 | 1 |
| M3a DEFERRED | on | **10** | 20 | 10 | 1 |
| **M3b DEFERRED** — cook on an explicit app `Commit()` (usdRig's model) | off/on | **10** | 29 / 20 | 10 | 1 |

Readings:

* **M1 over-cooks 2–2.9×.** hdGp cooks synchronously inside `_PrimsDirtied`
  (`hdGp/generativeProceduralResolvingSceneIndex.cpp:555-666`, parallel across procedurals with
  `WorkParallelForEach` at `:632`) and again on `_PrimsAdded` (`:157-320`, parallel loop at `:292`) and `_PrimsRemoved` (`:334-551`, parallel loop at `:512`), with no
  frame-level coalescing. For a 5-operator hair chain this is the difference between 30 fps and 12 fps.
  hdGp's own author flags the cook path as unguarded: *"TODO, move this within the
  compare_exchange_strong so that only one side cooks or add pre-proc entry mutex"* (`:832-833`).
* **M2 is disqualified on two counts, not one.** It cooks on **4 distinct worker threads** (measured),
  serialising Storm's parallel Rprim sync behind a cook mutex; and — decisively — it emits **zero
  downstream notices**, so a generator that *adds or removes curve prims* can never announce that.
  OpenUSD says the same thing twice in hdGp: *"Cooking of procedurals is driven by notices. Don't cook
  the procedural in response to scene queries."*
  (`generativeProceduralResolvingSceneIndex.cpp:63-65` and `:120-122`).
* **M3a and M3b both hit the ideal 10 cooks under both batching modes**, and both cook on the main
  thread only.

**Snapshot safety (usdRig's pattern) validated:** with 8 reader threads calling
`std::atomic_load(&_published)` while the main thread published 20 generations,
**16 734 reads, 0 torn reads** — a reader never saw a snapshot whose `frame` and `density` came from
different generations. This is exactly `RigExecSnapshotStore::Publish`/`Get`
(`<usdrig-src>/libs/rigExecImaging/snapshotStore.h:326-378`).
(Caveat: `std::atomic_load(std::shared_ptr*)` is deprecated in C++20; prefer
`std::atomic<std::shared_ptr<T>>` if the project ever moves to C++20.)

---

## 6. "Evaluate once after the last notice" — is it implementable?

**Yes, in two forms, and the choice is forced by whether the app cooperates.**

### M3a — commit on the scene-globals `currentFrame` notice (works with *stock* usdview/usdrecord)

Because the scene-globals SI is always upstream-adjacent to renderer plugins and is *never* batched,
the `currentFrame` dirty is a reliable once-per-frame signal that the plugin can key on. It arrives
**last** with no app batching (the default in every in-tree app, since nothing calls
`MergingSceneIndexNoticeBatchBegin`) and **first** if an app ever does batch. So the rule must be:

> accumulate dirty state in `_PrimsDirtied`; when a `currentFrame` dirty on `/` arrives **and**
> accumulated state is non-empty, cook once, publish, and emit the generated dirties.

Residual hazard: if an app batches, the frame notice arrives *before* the geometry notices, and the
cook would run on the previous frame's geometry. Guard by **also** committing on the first
`GetPrim` of a generated prim if state is still dirty (a cheap `std::atomic<bool>` check that never
cooks in the common case) — this is the "read-through-consistency backstop", not the primary path.
A parameter-only edit with no frame change (S6) produces *no* `currentFrame` notice at all, so the
backstop is load-bearing there too.

### M3b — commit from an explicit application call (the strong form; usdRig's shipped design)

usdRig does not key on Hydra notices at all. `RigExecImagingRegistry::SetTime` evaluates every
session, builds a combined snapshot, publishes it atomically and then broadcasts the diff as Hydra
dirties (`<usdrig-src>/libs/rigExecImaging/registry.cpp:342-378`); it is reached from
(a) an `extern "C"` entry point `RigExecImaging_SetTime(double)` (`registry.cpp:1198-1201`) called by
the usdview plugin via `ctypes` on `dataModel.currentFrameChanged`
(`plugin/rigExecUsdview/rigExecUsdview.py:174`, `600-610`), and (b) a `UsdNotice::ObjectsChanged`
listener that re-runs `SetTime(_lastTime)` when the edit touches an asset root (`registry.cpp:421-470`).

Verified usdview ordering that makes (a) sound: `RootDataModel.currentFrame`'s setter emits
`currentFrameChanged` **before** assigning `_currentFrame`
(`usdviewq/rootDataModel.py:152-162` — so a handler must use the *signal's* value, as usdRig's
`_FrameValue` docstring warns), and `AppController._setFrameIndex` assigns `currentFrame` and only
then calls `_updateOnFrameChange()`, which is what renders (`appController.py:3872-3878`,
`3899-3915`). **A usdview plugin therefore has a guaranteed pre-redraw commit point.**

### The strongest precedent: OpenExec's own Hydra integration is exactly M3b

`UsdImagingGLEngine::_AppendOverridesSceneIndices` (`engine.cpp:1611-1644`) merges
`UsdExecImaging_StageSceneIndex` over the stage scene index and puts an
`HdNoticeBatchingSceneIndex` immediately downstream with **batching permanently on**
(`engine.cpp:1638-1642`), with the comment: *"This ensures the exec scene index is able to refresh
its exec request before notices are flushed to downstream scene indices."* The engine then calls
`SetTime`/`ApplyPendingUpdates` on it and explicitly `Flush()`es (`engine.cpp:489-492`, `2373-2376`).

And exec is **not lazy**, contrary to the framing in the brief:
`UsdExecImaging_StageSceneIndex::SetTime`/`ApplyPendingUpdates` call `_request->Refresh()`
(`usdExecImaging/stageSceneIndex.cpp:52-70`), which calls `_Recompute()` →
`_cacheView.emplace(_system->Compute(*_request))` (`request.cpp:96-110`, `283-292`) →
`_runtime->ComputeValues(schedule, computeRequest)` (`exec/system.cpp:84-99`). The data source's
`GetValue` is then a pure extraction from the already-computed cache view
(`computedDataSource.cpp:49-52`; `GetComputedValue` `TF_VERIFY`s `!_requiresRecompute`,
`request.cpp:124-145`). **Evaluate eagerly on the app thread inside `SetTime`/`ApplyPendingUpdates`;
publish a snapshot; read-only from data sources.** That is M3b, endorsed by Pixar.

---

## 7. Thread-safety rule (settled)

| Contract | Evidence |
|---|---|
| `GetPrim` / `GetChildPrimPaths` **must** be threadsafe | `hd/sceneIndex.h:98`, `:109` ("This function is expected to be threadsafe.") |
| Observer callbacks (`PrimsAdded/Removed/Dirtied/Renamed`) are **not** expected to be threadsafe | `hd/sceneIndexObserver.h:123, 132, 143, 151` |
| `AddObserver`/`RemoveObserver` are **not** threadsafe | `hd/sceneIndex.h:67-80` |
| Storm's Rprim sync is parallel across rprims, calling into the scene delegate → terminal scene index `GetPrim` from many threads | `renderIndex.cpp:1830-1862` (`WorkWithScopedParallelism` + `WorkParallelForN` over `r.rprims`), gated by `HdOptionTokens->parallelRprimSync` |
| A6 §7 leaves "VdfExecutor `GetOutputValue` while `Run` mutates" UNVERIFIED and calls it a data race | `research/A6-openexec-vdf.md:470` |

⇒ **Rule for the hair plugin:** all mutation of evaluator state happens on the app/notice thread
inside the commit; `GetPrim` only `atomic_load`s the published generation and wraps it in
data sources. Never call the evaluator, block, change time, or lock the authoring stage from
`GetPrim`. This is verbatim usdRig `docs/spec.md:1584`, and my tearing test shows the pattern holds
under 8 concurrent readers.

Note also that `HdNoticeBatchingSceneIndex` **does not** delay data (`:23-32`), so an evaluator that
publishes a snapshot inside a batch and dirties later is still self-consistent only if `GetPrim`
serves the *published* generation, not upstream values it has not yet consumed.

---

## 8. The asynchronous / progressive path (`asyncProbe.cpp`)

`HdSceneIndexBase::SystemMessage` walks **inputs first, then self** (`hd/sceneIndex.cpp:181-195`), so
a message sent at the terminal reaches every filtering scene index in the chain. Measured on a
4-node chain:

```
asyncAllow:  upstreamA, HAIR-PLUGIN, downstreamB, terminal   (each got exactly 1)
asyncPoll x4: same order; HAIR-PLUGIN emitted PrimsDirtied on polls 0..2
  poll 0/1/2 -> PollForAsynchronousUpdates would return TRUE (redraw)
  poll 3     -> false (no redraw)
```

Mechanics and gating:

* `asyncAllow` is sent **once**, at engine creation, only if `Parameters::allowAsynchronousSceneProcessing`
  is true (`engine.cpp:1497-1500`, `engine.h:106-109`).
* `PollForAsynchronousUpdates()` attaches a temporary observer to the terminal SI, sends `asyncPoll`,
  removes the observer, and returns whether *anything* was emitted (`engine.cpp:2608-2659`) — which is
  what my probe reproduces.
* usdview drives it from a 100 ms `QTimer` (`appController.py:522-527`) whose slot calls
  `stageView.PollForAsynchronousUpdates()` and `UpdateViewport()` (`appController.py:5515-5519`,
  `stageView.py:2438-2445`).
* **Gating chain, all verified:** `--allow-async` CLI flag (`usdviewq/__init__.py:194-196`) →
  `AppController._allowAsync` (`appController.py:381`) → the timer (`:522-527`) →
  `stageView.allowAsync` (`:1868`; default `False` at `stageView.py:944`) →
  `params.allowAsynchronousSceneProcessing` at engine construction (`stageView.py:966`).
* **A usdview plugin *can* turn it on without the CLI flag**: `_configurePlugins()` runs at
  `appController.py:432`, i.e. **before** the async timer is created at `:522-527` and long before the
  `StageView`/engine exist. A plugin's `registerPlugins` can set
  `usdviewApi._UsdviewApi__appController._allowAsync = True` (there is no public accessor —
  `UsdviewApi.__appController` is name-mangled, `usdviewApi.py:20-21`). **UNVERIFIED at runtime** —
  it needs a display; see the benchmark protocol.
* Only `HdGpGenerativeProceduralResolvingSceneIndex` implements `_SystemMessage` in the whole tree
  (`hdGp/generativeProceduralResolvingSceneIndex.cpp:955-1015`); its procedural API is `AsyncBegin(bool)` +
  `AsyncUpdate(...) -> {Continuing, Finished, ContinuingWithNewChanges, FinishedWithNewChanges}`
  (`hdGp/generativeProcedural.h:130-177`). That enum is a good template for a progressive hair
  generator's contract.
* **Contract:** notices may only be *sent* during the poll, on the app thread. Background threads may
  only stage results (`hd/systemMessages.h:22-27`).

---

## 9. Cost of the notice cascade itself (`noticeCost.cpp`, CPU-only, measured here)

Wall time of the whole `ApplyPendingUpdates` + `SetTime` + `SetCurrentFrame` block, plus a terminal
plugin that scans **every** dirty entry for a `primvars/points` intersection. 20 frames, warmed;
median of 3 runs (the very first cold run was ~5× noisier and is discarded).

| scalps | post-merge batch | `PrimsDirtied` calls/frame | entries/frame | ms/frame |
|---|---|---|---|---|
| 1 | off / on | 2 / 2 | 2 | 0.005 / 0.002 |
| 10 | off / on | 2 / 2 | 11 | 0.007 / 0.006 |
| 100 | off / on | 2 / 2 | 101 | 0.061 / 0.062 |
| 1 000 | off / on | 2 / 2 | 1 001 | **0.21 / 0.21** |
| 5 000 | off / on | 2 / 2 | 5 001 | **1.13 / 1.13** |

**The call count is exactly 2 per frame regardless of scalp count or batching** (points batch +
scene-globals). The per-entry tax is ~0.2 µs, so the notice plumbing is ≪ 1 ms even at 5 000 scalps
and is not a scheduling concern. What *is* a concern is that M1 (eager) multiplies the **cook**, not
the notice, by 2–3×.

---

## 10. Recommendation

**Primary: M3b — deferred, app-committed, snapshot-published, main-thread evaluation.**

1. Register the hair scene index as a renderer plugin at **phase 0, `InsertionOrderAtEnd`** (measured
   §3): downstream of scene-globals, upstream of all Storm plugins.
2. Accumulate dirty state in `_PrimsDirtied`/`_PrimsAdded`/`_PrimsRemoved`, **forward the input notices
   immediately and unchanged**, and cook nothing there.
3. Commit points, in priority order:
   * an explicit `Commit()` exposed on a small C ABI (usdRig's `RigExecImaging_SetTime` shape,
     `registry.cpp:1198-1201`) that the usdview tool plugin calls from `currentFrameChanged`
     (fires before the redraw, `appController.py:3872-3878`);
   * otherwise, the `/` + `sceneGlobals/currentFrame` dirty (measured to be the last notice of a frame
     in every un-batched configuration);
   * plus a cheap `atomic<bool>` read-through backstop on the first `GetPrim` of a generated prim, for
     parameter-only edits and for the batched ordering inversion.
4. Cook → build an immutable generation → `std::atomic_store` publish → diff against the previous
   generation → emit one `PrimsAdded`/`PrimsRemoved`/`PrimsDirtied` per change class
   (`snapshotStore.h:326-378` is the reference implementation).
5. `GetPrim` only `atomic_load`s the generation. No locks, no compute, no stage access.
6. For very expensive generators, opt into `asyncAllow`/`asyncPoll` (§8) and return
   `ContinuingWithNewChanges`-style progressive results; the plugin enables it by setting
   `_allowAsync` on the app controller during `registerPlugins`.
7. Ship UsdImaging adapters for every `usdGen:` property that must drive re-cooking — otherwise S5's
   silent no-op bites.

Consider *also* wrapping the frame block in `MergingSceneIndexNoticeBatchBegin/End` in any C++ host we
own (not usdview): §5 shows it halves the callback count and merges 6 entries into 1 call. It does
**not** help usdview and it inverts the frame/geometry order, so the commit logic must not depend on it.

---

## Key facts

* Engine per-frame order is `_PreSetTime`(batched `ApplyPendingUpdates`) → `SetTime` (**unbatched**) →
  `_SetSceneGlobalsCurrentFrame` → `_PostSetTime` (**empty body**) — `engine.cpp:483-498`, `2354-2392`.
* A renderer-level plugin receives **exactly 2 `PrimsDirtied` calls per frame change** — one with all N
  time-varying prims, one with `/ {sceneGlobals/currentFrame}` — measured for N = 1…5 000
  (`noticeCost.txt`); the scene-globals one is **last** unbatched, **first** if the post-merging batch is
  used (`run1.txt` S1/S2/S3/S7/S8).
* The frame notice can never be coalesced with the geometry notices: `HdsiSceneGlobalsSceneIndex` is
  registered at phase 0/`InsertionOrderAtStart` (`engine.cpp:182-193`) and therefore sits **downstream**
  of both batching scene indices (`sceneIndices.cpp:286-287`, `renderIndex.cpp:194-203`) — confirmed by
  `chainOrder.txt`.
* `HdNoticeBatchingSceneIndex` merges only *contiguous same-type* notices and never dedupes
  (`noticeBatchingSceneIndex.cpp:88-113`, `128-149`); it delays notices but **not** data (`:23-32`).
  Measured: 3 edits + a frame change go from 3 calls to 2 calls / 1+6 entries (`run1.txt` S9d vs S9e).
* Time-varying dirties are registered lazily by data-source pulls
  (`stageSceneIndex.cpp:882-888`; `dataSourcePrim.cpp:43,236,327,486`) and replayed as **one** notice
  (`stageSceneIndex.cpp:357-372`). Before a warm pull the probe saw zero time dirties.
* Editing a custom `usdGen:` attribute on a stock prim type produces **no Hydra notice**
  (`run1.txt` S5) — `UsdImagingBasisCurvesAdapter::InvalidateImagingSubprim` returns an empty locator
  set for unknown properties (`basisCurvesAdapter.cpp:82-94`, dispatcher at `stageSceneIndex.cpp:796-878`).
* Cook counts for 10 interactive events: eager-in-notice **29** (20 batched), lazy-in-`GetPrim` 10 but
  on **4 worker threads** and with **0 downstream notices**, deferred-on-frame **10**, deferred-on-commit
  **10** (`models.txt`).
* hdGp cooks synchronously in `_PrimsDirtied` in parallel over procedurals
  (`generativeProceduralResolvingSceneIndex.cpp:555-666`) and explicitly refuses to cook from `GetPrim`
  (`:63-65`, `:120-122`); its cook path carries an unresolved race TODO (`:832-833`).
* `usdExecImaging` is **eager, not lazy**: `Refresh()` → `_system->Compute()` →
  `VdfExecutor` run during `SetTime`/`ApplyPendingUpdates`; data sources only extract from the finished
  cache view (`usdExecImaging/stageSceneIndex.cpp:52-70`, `request.cpp:96-110,124-145,283-292`,
  `computedDataSource.cpp:49-52`, `exec/system.cpp:84-99`).
* OpenUSD's own precedent for "evaluate once, then flush": an always-on `HdNoticeBatchingSceneIndex`
  in front of the exec scene index plus an explicit `Flush()` after `SetTime`
  (`engine.cpp:1633-1642`, `489-492`, `2373-2376`).
* The only post-notice/pre-sync hooks are `HdRenderDelegate::Update()` (`renderIndex.cpp:1604`;
  `renderDelegate.h:540-546`; **no in-tree overrides**), task Sync, and the never-called
  `HdRenderIndex::MergingSceneIndexNoticeBatchBegin/End` (`renderIndex.cpp:928-938`).
* `GetPrim` must be threadsafe (`sceneIndex.h:98,109`); observer callbacks need not be
  (`sceneIndexObserver.h:123,132,143,151`); Storm syncs Rprims in parallel
  (`renderIndex.cpp:1830-1862`). Atomic snapshot swap survived 8 readers × 20 publishes with
  **0 torn reads** (`models.txt`).
* `asyncAllow`/`asyncPoll` reach every filtering SI in the chain (inputs first, then self —
  `sceneIndex.cpp:181-195`), and notices emitted during `asyncPoll` are seen by the engine's temporary
  observer, which is what triggers a redraw (`engine.cpp:2608-2659`; probe `async.txt`).
* usdview gates async on `--allow-async` (`usdviewq/__init__.py:194-196` → `appController.py:381` →
  `:522-527` → `stageView.py:944,966`), but `_configurePlugins()` at `appController.py:432` runs
  **before** the timer is created, so a plugin can enable it.
* usdview's `currentFrameChanged` fires **before** `_currentFrame` is assigned
  (`rootDataModel.py:152-162`) and before `_updateOnFrameChange()` renders
  (`appController.py:3872-3878`) — a usable pre-redraw commit point, which is precisely how usdRig
  drives its evaluation (`plugin/rigExecUsdview/rigExecUsdview.py:174,600-610`;
  `libs/rigExecImaging/registry.cpp:342-378,1198-1201`).
* Notice-cascade CPU cost is ~0.21 ms/frame at 1 000 scalps and ~1.13 ms/frame at 5 000
  (`noticeCost.txt`) — negligible next to cooking.

## Decisions this settles

1. **Evaluate on the application/notice thread, never inside `GetPrim`.** Lazy pull is disqualified by
   measurement (worker-thread cooks, zero downstream notices for structural change), by usdRig
   `docs/spec.md:1584`, and by hdGp's own comments.
2. **Never cook inside every `_PrimsDirtied`.** The hdGp model over-cooks 2–2.9× per interactive event.
3. **Adopt the deferred commit + atomic snapshot publish + diffed dirty model** (usdRig / usdExecImaging
   shape). It hits the ideal cook count under every batching configuration tested.
4. **Primary commit point = an explicit app call**, wired from the usdview plugin's
   `currentFrameChanged` handler (using the *signal's* value, not the property). **Secondary** =
   the `/ sceneGlobals/currentFrame` dirty. **Backstop** = a lock-free dirty check on the first
   `GetPrim` of a generated prim.
5. **Register the hair scene index at phase 0 / `InsertionOrderAtEnd`** so it is downstream of
   scene-globals (sees the frame) and upstream of Storm's velocity-motion, implicit-surface and
   dependency scene indices.
6. **Do not build on hdGp**: disabled by default, no frame-level coalescing, unresolved cook race, and
   it sits downstream of velocity-motion resolution.
7. **Ship UsdImaging adapters for `usdGen:` properties.** Without `InvalidateImagingSubprim` coverage,
   interactive parameter edits produce no Hydra dirty at all.
8. **Progressive generation uses `asyncAllow`/`asyncPoll`**, with all notices emitted on the app thread
   during the poll; the plugin enables usdview's 100 ms poll itself during `registerPlugins`.
9. **Do not rely on app-level notice batching.** It is never enabled in usdview, and enabling it
   inverts the frame/geometry notice order.

## Open questions

* **Runtime confirmation that a usdview plugin can enable async.** Setting
  `usdviewApi._UsdviewApi__appController._allowAsync = True` in `registerPlugins` is sound by code
  order (`appController.py:432` < `:522-527`), but is **UNMEASURED** here (needs a display).
  *Protocol:* on a workstation, `usdview` (no `--allow-async`) with a plugin that sets the flag and
  installs a scene index logging `_SystemMessage`; expected: exactly one `asyncAllow` and ~10
  `asyncPoll` per second in the log. Compare against `usdview --allow-async` as the control.
* **Whether a custom `HdTask` prim inserted through the merging scene index gives a scene-index-visible
  "pre-Rprim-sync" hook** (task Sync runs at `renderIndex.cpp:1624-1660`, before Rprim sync). This would
  be a batching-independent commit point that needs no app cooperation. UNVERIFIED.
* **Whether hdPrman's render-time chain preserves the same 2-notices-per-frame shape.** `usdrecord`
  and the hdPrman path were covered in `G-hdprman-and-usdrecord-render-time-chain.md`; the interaction
  of a single-frame render (`SetTime` once, no scrubbing) with the commit logic should be re-checked
  there. Not measured in this probe.
* **Cook cost under interruption.** `VdfExecutorInterface::SetInterruptionFlag(const std::atomic_bool*)`
  (A6 §4, `vdf/executorInterface.h:261-277`) would let a scrub cancel an in-flight cook, but the
  interaction with a half-published snapshot is unspecified. UNVERIFIED.
* **GPU-side numbers.** Everything about actual frame time (Storm curve draw, shader compile stalls on
  a newly added curve prim, whether the extra `PrimsAdded` for generated prims forces a repr rebuild)
  is **UNMEASURED** here. *Protocol:* on a workstation, `usdview --timing` on a stage with 1 / 100 /
  1 000 scalps and a 5-operator chain; scrub 200 frames with `HD_ENABLE_MULTITHREADED_RPRIM_SYNC`
  on and off; capture `TfTrace` (`--traceToFile`) and read the `Cooking …`, `Parallel Rprim Sync`,
  and `HdStRenderPass` scopes; report cooks/frame from the plugin's own counter and compare to the
  ideal count.
