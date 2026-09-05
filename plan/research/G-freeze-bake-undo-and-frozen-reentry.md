# G — Freeze / bake / undo, and how a frozen curve prim re-enters the graph

Gap key: `freeze-bake-undo-and-frozen-reentry`.
All paths absolute. `USD` = `/home/burkard/work/OpenUSD` (v26.08 source),
`USDI` = `/home/burkard/work/OpenUSD_26_08` (install), `RIG` = `/home/burkard/work/usdRig`,
`SCR` = `/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad`,
probes in `SCR/probes/freeze-bake/`.

Everything below was **run**, headless, on this machine (llvmpipe Storm under the
`:77` Xvfb the build agent left running). Every number is CPU. Nothing was
modified under `RIG` or `USD`.

Environment for every command: `. SCR/probes/B-build/rigexec_env.sh` (plus
`PYTHONPATH=$RIG/tests/python:$PYTHONPATH` for the two rigged Python probes,
and `DISPLAY=:77` for the `testusdview` runs).

| probe | what it is |
|---|---|
| `probe1_freeze_undo.py` | `SubtreeSnapshot` — the prim-subtree analogue of `rigExecUndo.py:20-104` — plus cost measurement |
| `probe2_undo_resync_exec.cpp` | 9 removal/restore shapes × {no exec, `ExecUsdSystem` attached} |
| `probe3_python_rigged_undo.py` | same matrix through the real `_rigexec.Rig` on `RIG/examples/ArmShotAnim.usda` |
| `probe4_frozen_reentry_si.cpp` | `UsdImagingCreateSceneIndices` on a frozen prim; data-source dump + freeze/deform/undo timings |
| `probe5_bake_size.py`, `probe6_bake_placement.py` | bake file sizes / authoring time / landing place |
| `probe7_locators_and_reentry.cpp` | dirty-locator taxonomy + reading a frozen prim from a filtering SI's input |
| `probe8_undo_fidelity.py` | 11 round-trip fidelity assertions on `SubtreeSnapshot` |
| `probe9_error_containment.py` | is the OpenExec diagnostic harmful? |
| `probe10_resync_blast.cpp` | resync blast radius over 200 sibling curve prims |
| `uv/testUsdviewFreezeCost.py` | one freeze inside a live usdview; `_resetGUI` broken into its panels |

Each C++ probe builds with the `g++ -std=c++17 -O2 -Wno-deprecated … -I$USD/include
-I/usr/include/python3.12 -L$USD/lib -lusd_{tf,sdf,usd,usdGeom,usdImaging,hd,arch,vt,gf,plug,work,python}
-lpython3.12 -ltbb` line recorded at the top of its source.

---

## 1. (a) Prim-subtree undo

### 1.1 The design: `SubtreeSnapshot`, the exact analogue of `AttributeSnapshot`

`rigExecUndo.AttributeSnapshot` (`RIG/plugin/rigExecUsdview/rigExecUndo.py:20-104`)
captures *one attribute spec in one layer* — `exists`, `typeName`, `variability`,
default, spline, time samples — and `Restore()` puts that spec back or removes it
if `exists` is False (`:70-91`). A freeze authors a whole prim subtree, so the
snapshot unit has to be a prim spec. The verified implementation
(`SCR/probes/freeze-bake/probe1_freeze_undo.py:74-121`) keeps the same three-method
contract and needs no new concepts:

```python
class SubtreeSnapshot:
    @classmethod
    def Capture(cls, layer, path):
        snap.stash = Sdf.Layer.CreateAnonymous("undoStash")
        Sdf.CreatePrimInLayer(snap.stash, path.GetParentPath())
        Sdf.CopySpec(layer, path, snap.stash, path)     # SdfCopySpec, copyUtils.h
    def Restore(self):
        with Sdf.ChangeBlock():
            self._Remove(self.layer, self.path)         # del parent.nameChildren[name]
            if self.exists:
                Sdf.CreatePrimInLayer(self.layer, self.path.GetParentPath())
                Sdf.CopySpec(self.stash, self.path, self.layer, self.path)
```

Three API notes established by running it:

* **`SdfLayer` has no `RemovePrimSpec` in Python.** Removal is
  `del parentSpec.nameChildren[name]`, or `del layer.rootPrims[name]` at the root
  (`probe0_api.py` output: `after del, has C: False`, `after del root, has Z: False`).
  `Sdf.Layer` exposes only `GetPrimAtPath / rootPrims / defaultPrim` from the prim
  family — verified by `dir(Sdf.Layer)`.
* **`Sdf.CopySpec` copies the whole subtree**, including nested children,
  relationships, prim metadata, variability, splines and time samples — see §1.2.
* Everything goes in **one `Sdf.ChangeBlock`**, exactly as
  `rigExecUndo.Edit._Apply` argues at `:122-135`: a half-applied freeze (children
  present, parent gone) is visible to any recomposing observer otherwise.

### 1.2 Fidelity — 11/11 (`probe8_undo_fidelity.py`)

The probe authors freeze *v1* (typeName, `customData`, `kind`, a `points` default
**and** 3 time samples, a `Ts.Spline` on a second attribute, a uniform-variability
attribute, a `usdGen:boundSurface` relationship, 2 child prims), re-freezes it as
*v2* with different values and 1 child, then undoes/redoes and diffs a full
recursive digest of the layer.

```
undo re-freeze -> v1  PASS      redo re-freeze -> v2  PASS      undo again -> v1  PASS
undo of a first freeze removes the prim  PASS      composed stage sees v1 after undo  PASS
time samples survive  PASS   spline survives  PASS   children restored  PASS
relationship restored  PASS  prim metadata (kind) restored  PASS
uniform variability restored  PASS                                     11/11 pass
```

The only initial mismatch was the Python `repr()` address of a `Ts.Spline` object
— a probe artefact; comparing `Spline.Eval()` at 3 times makes it pass.

### 1.3 Cost — the stash is free, because `VtArray` is copy-on-write

`probe1_freeze_undo.py`, in-memory stage, session-layer edit target:

| step | 10 000 curves × 8 CV | 100 000 curves × 8 CV |
|---|---:|---:|
| author freeze into session layer | 0.51 ms | 0.52 ms |
| `Capture` (CopySpec into anon stash) | 0.10 ms | 0.10 ms |
| **UNDO** (remove subtree) | 0.23 ms | 0.24 ms |
| **REDO** (CopySpec stash → layer) | 0.08 ms | 0.09 ms |
| RSS before → after `Capture` | 98.5 → 98.5 MB | 210.2 → 210.2 MB |

Zero RSS growth across `Capture` proves `SdfCopySpec` shares the `VtArray`
buffers rather than deep-copying them. Consequence: **an undo stack 200 entries
deep (`rigExecUndo.UndoStack.LIMIT = 200`, `:147`) costs nothing extra in memory
for freezes it is *holding*, and the deallocation of a freeze's buffers is
deferred to the moment the entry is evicted.** Measured separately
(`probe6_bake_placement.py`): remove-with-stash-alive 0.15 ms, drop-stash 0.99 ms
for 100 k × 8 — i.e. the ~1 ms of `free()` is moved off the interactive path.
Without a stash the same removal costs 1.45 ms at 100 k curves and 13.44 ms at
1 M curves (`probe4_frozen_reentry_si`, "stage RemovePrim"), which is exactly the
deallocation.

The alternative "the freeze is its own sublayer; undo drops/mutes it" is equally
cheap and does not need a stash at all: insert 0.21 ms, remove 0.15 ms, mute
0.13 ms, unmute 0.13 ms — flat in curve count (`probe1_freeze_undo.py`).

### 1.4 The `Usd_PrimFlagsPredicate` error is **not** RigExec, and it will hit usdGen

The question posed was whether the error comes from RigExec holding a
`UsdPrimRange` across a resync. It does not.

* Root cause: `USD/pxr/exec/esfUsd/stageData.cpp:360`
  `if (!UsdPrimDefaultPredicate(resyncedPrim))` is applied to
  `_stage->GetPrimAtPath(resyncedPath)` (`:351`) *without* checking validity;
  `USD/pxr/usd/usd/primFlags.cpp:22-27` posts `TF_CODING_ERROR("Applying
  predicate to invalid prim.")` **and returns `false`** — which is the branch
  `esfUsd` wanted. Stock OpenUSD 26.08, no RigExec code involved (the build
  agent's `SCR/probes/B-build/probeExecResyncRemovePrim.cpp` reproduces it with
  a bare `ExecUsdSystem`: `baseline=0 withExec=1 -> REPRODUCED`).
* The `UsdPrimRange` at `stageData.cpp:396` is only reached in the *valid-prim*
  branch, so it is not the fault; `RIG/docs/curvenet.md:513-522` ("Origin not yet
  located") can be closed.

`probe2_undo_resync_exec.cpp` (C++, bare `ExecUsdSystem`) and
`probe3_python_rigged_undo.py` (Python, a compiled `_rigexec.Rig` on
`ArmShotAnim.usda`) agree exactly:

| undo/redo shape | plain stage | exec/rig attached | exec after |
|---|---|---|---|
| `UsdStage::RemovePrim(subtree root)` | ok | **raises** | still evaluates |
| `del parentSpec.nameChildren[name]` | ok | **raises** | still evaluates |
| … same, inside `Sdf.ChangeBlock` | ok | **raises** | still evaluates |
| `prim.SetActive(False)` | ok | **ok** | still evaluates |
| remove **and** re-`CopySpec` in ONE `Sdf.ChangeBlock` | ok | **ok** | still evaluates |
| `Sdf.CopySpec` over a live spec (redo onto an existing prim) | ok | **ok** | still evaluates |
| authoring a freeze (add only) | ok | **ok** | still evaluates |
| mute a sublayer holding the freeze | ok | **raises** | — |
| remove a sublayer path holding the freeze | ok | **raises** | — |

The pattern is exact: **anything that leaves no prim at the resynced path at the
end of the change round trips through the buggy branch.** The remove-then-recopy
case is clean because the prim exists again when the change block closes.

`probe9_error_containment.py` settles how bad it is:

```
removal raised: True     spec gone from layer: True     prim gone from stage: True
rig still evaluates after the raise: True
next USD call clean: True
removals batched in one ChangeBlock -> errors in one exception: 3
all three gone: True
```

So the edit is **correct**; only the diagnostic is spurious, it does not leak into
the next call, and batching N removals in one `Sdf.ChangeBlock` coalesces them
into one `Tf.ErrorException` carrying N errors. `Tf.ErrorMark` is **not** bound in
Python (`hasattr(Tf, "ErrorMark") == False`), so the containment in a usdview tool
is a `try/except Tf.ErrorException` around the removal, guarded by a re-assert
that the prim really is gone. In C++ (the SI / evaluator side) `TfErrorMark` +
`Clear()` works, as `probe2` does.

### 1.5 The edit-target trap (found by accident, load-bearing)

`probe10_resync_blast.cpp` (`RemovePrim on a ROOT-layer sibling (session ET)`):
with usdview's default edit target — the **session layer**
(`USD/pxr/usdImaging/usdviewq/appController.py:1283`,
`stage.SetEditTarget(stage.GetSessionLayer())`) — `UsdStage::RemovePrim` on a
prim whose spec lives in the root layer **does nothing at all**: `still on stage:
1`, zero scene-index notices, 0.00 ms. Removing the *session-layer* freeze at the
same edit target works (`still on stage: 0`, 1 `PrimsRemoved`).

**Rule for the plan: a freeze may only be undone in the layer it was authored
into.** The undo entry must therefore record the layer, exactly as
`rigExecUndo.EditEntry` does (`:107-112`), and `Restore()` must be a no-op if
`entry.layer.expired` (`:133-134`).

---

## 2. (b) How a frozen curves prim re-enters the graph

### 2.1 The contract (verified reachable end-to-end)

A freeze is a plain `UsdGeomBasisCurves` — no new prim type is required for the
*data*, only for the operator that points at it. `probe4_frozen_reentry_si.cpp`
authors it and dumps the **terminal** scene index of
`UsdImagingCreateSceneIndices` (`addDrawModeSceneIndex=false`):

```
terminal prim /Groom/Frozen type=basisCurves
  primvar                    interp       role                type              size
  rest                       vertex       point               VtArray<GfVec3f>  80000
  skinprim                   uniform      -                   VtArray<int>      10000
  skinprimuv                 uniform      textureCoordinate   VtArray<GfVec2f>  10000
  usdGen:curveId             uniform      -                   VtArray<int>      10000
  usdGen:rootFrame           (none)       -                   -                     0
  points                     vertex       point               VtArray<GfVec3f>  80000
  widths                     vertex       -                   VtArray<float>    80000
  velocities / accelerations / normals    -                   -                     0
  topology: type=cubic basis=bspline wrap=pinned curves=10000
```

| authored | type / interpolation | why | verified |
|---|---|---|---|
| `points`, `curveVertexCounts`, `type`, `basis`, `wrap` | schema | the curves themselves | topology line above; `wrap=pinned` survives (`USD/pxr/usdImaging/usdImaging/basisCurvesAdapter.cpp:367-368` in the 1.0 path) |
| `widths` | `float[]`, `vertex` | Storm needs it; the adapter falls back `primvars:widths` → inherited → `widths` (`basisCurvesAdapter.cpp:140-263`, `:230-239`) | reaches SI as primvar `widths` interp `vertex` |
| `primvars:rest` | `point3f[]`, `vertex` | Houdini Guide Deform / Hair Procedural `primvars:rest` (A7 §3.6, §3.7) | role `point`, 80 000 elements |
| `primvars:skinprim` | `int[]`, `uniform` | Houdini `Prim Num Attribute` (A7 §3.6) — the bound face index per curve | uniform, 10 000 |
| `primvars:skinprimuv` | `texCoord2f[]`, `uniform` | Houdini `Prim UVW Attribute` — barycentric/uv within that face | role `textureCoordinate` |
| `primvars:usdGen:curveId` | `int[]`, `uniform` | stable per-curve id: the key sculpt layers and re-freezes match on (A7 §9.1: "Frozen data should carry frozenEpoch … so a mismatch marks the freeze stale") | namespace **preserved** — the SI name is literally `usdGen:curveId` |
| `primvars:usdGen:frozenEpoch` | `string`, **`constant`** | staleness digest of (scatter seed, surface topology, guide ids) | see §2.3 — reaches the SI and dirties precisely |
| `primvars:usdGen:rootFrame` | `matrix4d[]`, `uniform`, optional | rest frame per root for the rigid-transport mode of `DeformWithSurface` (A7 §9.1 G7) | declared-but-unauthored primvars *do* appear, with no interpolation and no value — the consumer must test for a value, not for presence |

Two cautions the dump forces:

* `velocities`, `accelerations`, `normals` are **always** listed by the gprim data
  source even when unauthored (size 0). Any usdGen code walking
  `HdPrimvarsSchema::GetPrimvarNames()` must skip primvars whose
  `GetPrimvarValue()` yields an empty/absent value.
* `skinprimuv` picks up `role = textureCoordinate` purely from its `TexCoord2f`
  value type. That is harmless for Storm (which binds `st` by *name* through the
  material network) but means the name must not be `st`.

### 2.2 A frozen prim re-enters **without going back to the stage**

`probe7_locators_and_reentry.cpp` inserts a `HdSingleInputFilteringSceneIndexBase`
(the stand-in for the usdGen hair SI) after `UsdImagingCreateSceneIndices` and
calls `_GetInputSceneIndex()->GetPrim(frozenPath)`:

```
== 2. frozen prim read from the filtering SI's INPUT ==
    input-SI primvar points           size=8   interp=vertex
    input-SI primvar rest             size=8   interp=vertex
    input-SI primvar skinprim         size=2   interp=uniform
    input-SI primvar skinprimuv       size=2   interp=uniform
    input-SI primvar usdGen:curveId   size=2   interp=uniform
```

So `DeformWithSurface` and downstream stylers read the frozen buffer straight out
of the scene index — the same place they read a *generated* buffer from — and no
`UsdStage` access is needed on the Hydra side. This is the single most useful
result for the plan: a frozen prim and a generated prim are the **same kind of
input** to a styler.

### 2.3 What usdImaging will *not* carry — and the consequence for `usdGen:source`

`probe7` cases 1d / 1g / 1h / 1i:

| authored on the prim | appears in the SI? | dirties on change? |
|---|---|---|
| relationship `usdGen:source` on an **untyped** operator prim | **no** — `primType=''`, names are only the stock containers | **no notice at all** |
| relationship `usdGen:boundSurface` on the **typed** `BasisCurves` | **no** (`1i` names list has no such entry) | **no notice at all**, on create *or* retarget |
| custom non-primvar attribute `usdGen:frozenEpoch` (`string`) | **no** | **no notice at all**, on create *or* value change |
| primvar `primvars:usdGen:frozenEpoch` (`string`, `constant`) | **yes**, readable as `sha1:bbb`, `interp=constant` | `[primvars]` on create, `[primvars/usdGen:frozenEpoch]` on change |

Two design consequences, both firm:

1. **The freeze's staleness digest must be a `constant` primvar**, not a custom
   attribute and not `customData`. (`customData` has a second trap:
   `Usd.Prim.GetCustomDataByKey("usdGen:frozenEpoch")` returns `None` because it
   splits on `:` as a nested-dict key path — `probe1_freeze_undo.py` prints
   `GetCustomDataByKey('usdGen:frozenEpoch') -> None` while
   `GetCustomData()["usdGen:frozenEpoch"]` is correct.)
2. **usdGen must register its own `UsdImagingPrimAdapter` for every operator prim
   type**, because `usdGen:source` reaches Hydra only if an adapter puts it there
   and only invalidates if that adapter's `InvalidateImagingSubprim` maps the
   property to a locator (the pattern
   `USD/pxr/usdImaging/usdImaging/basisCurvesAdapter.cpp:85-93` shows). usdRig
   sidesteps this differently: `RIG/libs/rigExecImaging/sceneIndexPlugin.cpp:29-40`
   feeds its scene indices from a `RigExecImagingRegistry` store filled by the
   evaluator that owns the stage, not from operator prims in the SI. usdGen can
   copy either shape, but "author a relationship and expect the SI to notice" is
   not an option.

**How the SI learns that `usdGen:source` now targets a frozen prim** therefore has
exactly two mechanisms, and neither is free:
either the operator adapter republishes the resolved target (and the hair SI keeps
`frozenPath → dependent operator prims` as a reverse map, re-emitting
`PrimsDirtied` on the dependents when it sees a dirty on a frozen path), or the
evaluator that owns the stage does the resolution and pushes into a store as
rigExec does. In both cases the reverse map keys on the locators below.

### 2.4 Dirty-locator taxonomy for a frozen prim (`probe7`)

| edit to the frozen prim | locator(s) emitted |
|---|---|
| `points` | `primvars/points` |
| `primvars:rest` | `primvars/rest` |
| `primvars:usdGen:curveId` | `primvars/usdGen:curveId` |
| `curveVertexCounts` | `basisCurves/topology/curveVertexCounts` |
| **adding** a new primvar | `primvars` **then** `primvars/<name>` (two notices) |
| relationship / custom attribute | *(nothing)* |

Locators are per-primvar and namespace-preserving, so the hair SI's dirty
propagation can be precise: a `primvars/points` dirty on a frozen prim invalidates
only the deform-transport tail of its dependents; a
`primvars/usdGen:frozenEpoch` dirty means *re-check staleness*; a
`basisCurves/topology/...` dirty means the id mapping may have changed and every
downstream sculpt layer must be re-matched by `curveId`.

---

## 3. (c) Where frozen data lands

100 000 curves × 8 CV = 800 000 CVs. **All Python array construction was moved
outside every timer** — the 12.3 s figure a naive measurement produces
(`probe5_bake_size.py`, `bake_anim.usdc author 12329.5 ms`) is Python building 24
`Vt.Vec3fArray`s, not USD (`probe6_bake_placement.py` prints it separately:
`python array construction (24 samples): 12349.7 ms`).

| bake | author + save | bytes |
|---|---:|---:|
| session layer, static (author only, no file) | **0.4 ms** | 0 |
| sidecar `.usdc`, static | 10.6 ms | 9 601 971 |
| sidecar `.usdc`, static + `velocities` | 13.4 ms | 19 202 035 |
| sidecar `.usdc`, 24 point time samples | 80.6 ms | **230 402 598** |
| sidecar `.usda`, static | 368 ms | 40 317 359 |
| session layer `ExportToString()` (usda text) | 389 ms | 40 317 359 |

Reopen + first `points.Get()`: `.usdc` **0.4–0.8 ms**, `.usda` **532 ms** —
a 660× difference. **Never bake to `.usda`.**

Crate's size model, isolated attribute by attribute on *random* point data
(same probe, second block):

| content | bytes | note |
|---|---:|---|
| 800 k `Vec3f` points only | 9 600 668 | raw 9 600 000 → ~12 B/point, no win on random data |
| + `widths` all identical | +867 | LZ4 flattens a constant array |
| + `widths` all different | +3 200 054 | 4 B/float |
| + `curveVertexCounts` (100 k identical ints) | +184 | crate integer compression |
| + `primvars:usdGen:curveId` (100 k sequential ints) | +192 | crate integer compression |
| + `primvars:rest` **sharing the same `VtArray` object** | **+26** | crate deduplicates identical value buffers |
| + `primvars:rest` as a distinct buffer, same size | +9 600 046 | no dedup once contents differ |

So **authoring `primvars:rest` at freeze time is free** as long as the freeze
hands crate the same array object it hands `points` (which it naturally does: at
the moment of freezing, rest *is* the current pose). It only costs a second
9.6 MB once the curves are actually deformed away from rest.

**Recommendation.** Three tiers, and the plan should offer all three because they
answer different questions:

| tier | where | undo | survives | when |
|---|---|---|---|---|
| **T1 session freeze** | usdview session layer (the default edit target, `appController.py:1283`) | `SubtreeSnapshot` stash, §1 | dies with the session | the interactive comb/sculpt loop; 0.4 ms per freeze, no I/O |
| **T2 sidecar `.usdc` + sublayer** | `<asset>_groomBake.usdc`, sublayered under the session layer or an explicit bake layer | drop/mute the sublayer (0.13–0.21 ms, flat) | yes; reopen 0.8 ms | "commit results to stage via the tooling" — the artist's Save |
| **T3 sidecar `.usdc` + payload** | same file, arced by payload on a session `over` | `RemovePrim` on the over | yes, and it is *unloadable* | very heavy grooms / shot-level assembly |

Measured composition cost of the arc itself (`probe6_bake_placement.py`, 100 k
curves): sublayer 0.2 ms compose / 0.1 ms first `points.Get()`; reference 0.3 /
1.1 ms; payload **21.5** / 1.5 ms. Payload's 21.5 ms is the payload-arc
recomposition and is the price of being unloadable. Undo of the arc is 4.9–5.4 ms
in all three (that *is* the deallocation, since no stash holds the buffers).

**Motion samples: do not bake them.** 24 point samples cost 24 × the static size
(230 MB) with zero compression, because crate stores float arrays raw; the
`velocities` route costs 2 × (19.2 MB) and is what
`HdsiVelocityMotionResolvingSceneIndex` consumes
(`G-motion-blur-sampling-strategy.md` §2.1: Storm never asks for more than one
sample; hdPrman's phase-2 SI turns `points` + `velocities` into `{open, close}`).

---

## 4. (d) What one freeze costs the app

### 4.1 Scene index (`probe4_frozen_reentry_si.cpp`, in-memory stage, no delegate)

| step | 10 k curves | 100 k | 1 M |
|---|---:|---:|---:|
| author into session layer | 0.25 ms | 0.27 ms | 0.27 ms |
| `UsdImagingStageSceneIndex::ApplyPendingUpdates` | 0.11 ms | 0.13 ms | 0.13 ms |
| terminal `GetPrim` | 0.001 ms | 0.001 ms | 0.001 ms |
| first full data pull (topology + every primvar) | 0.19 ms | 0.19 ms | 0.20 ms |
| **edit → pulled, total** | **0.55 ms** | **0.59 ms** | **0.59 ms** |
| second full pull | 0.01 ms | 0.01 ms | 0.01 ms |
| points-only edit → `ApplyPendingUpdates` | 0.06 ms | 0.08 ms | 0.13 ms |
| `RemovePrim` + apply | 0.06 ms | 1.47 ms | 13.47 ms |
| `SetActive(false)` + apply | 0.02 ms | 0.02 ms | 0.02 ms |
| `SetActive(true)` + apply + re-pull | 0.04 ms | 0.04 ms | 0.06 ms |

**The USD→Hydra half of a freeze is flat in curve count and under a millisecond.**
It is flat because a data source hands back the retained `VtArray`; the only term
that scales is `RemovePrim`'s `free()`. `_ApplyPendingResyncs`
(`USD/pxr/usdImaging/usdImaging/stageSceneIndex.cpp:635-700`) coalesces resync
paths by prefix (`:654-657`), emits one `RemovedPrimEntry` and re-walks only that
subtree with `_PopulateSubtree` (`:686`).

### 4.2 Blast radius (`probe10_resync_blast.cpp`, 200 sibling curve prims)

Counts halved (the probe observes two scene indices):

| edit | added | removed | dirtied | apply |
|---|---:|---:|---:|---:|
| add a new sibling freeze under `/Groom` | 1 | 0 | 0 | 0.05 ms |
| edit `points` on one sibling | 0 | 0 | 2 | 0.09–0.10 ms |
| add a child under an existing sibling | 1 | 0 | 0 | 0.01 ms |
| **re-author the parent scope's `typeName`** | **203** | 0 | **58–101** | **1.3–1.5 ms** |
| deactivate one sibling | 0 | 1 | 0 | 0.02 ms |
| `RemovePrim` on a root-layer sibling with the session edit target | 0 | 0 | 0 | 0.00 ms (**no-op**, §1.5) |
| `RemovePrim` on the session-layer freeze | 0 | 1 | 0 | 0.00 ms |

All of it independent of curve count (200×500 vs 200×5000 differ only in
authoring time). **Design rule: freezes are siblings under a dedicated scope and
the tooling never re-authors the parent scope's spec** — a `typeName` touch on the
parent resyncs the entire groom (203 re-adds → 203 rprim destroy/recreate cycles
in the delegate).

### 4.3 usdview (`uv/testUsdviewFreezeCost.py`, real `testusdview --renderer GL` on `:77`)

| stage prims | author (incl. synchronous notice) | `_resetGUI()` alone | `_resetPrimView` | `_resetPrimViewVis` | llvmpipe redraw |
|---:|---:|---:|---:|---:|---:|
| 115 | 1.34 ms | 4.28 ms | 2.64 ms | 0.12 ms | 104 ms |
| 2 205 | 3.45 ms | 35.4 ms | 23.0 ms | 0.94 ms | 136 ms |
| 11 005 | 12.09 ms | 105.5 ms | 102.2 ms | 4.31 ms | 186 ms |

The other four panels `_resetGUI` drives are noise on all three stages:
`_updatePropertyView` 0.4–0.7 ms, `_populatePropertyInspector` 0.04 ms,
`_updateMetadataView` 0.17–0.30 ms, `_updateLayerStackView` 0.09–0.18 ms,
`_updateCompositionView` 0.04–0.08 ms (`appController.py:1962-1981`).
Selecting the frozen prim and re-running the property panels: 2.0 ms.

Two curve counts on the same 115-prim stage: 10 k curves → `_resetPrimView`
2.62 ms, 100 k curves → 2.64 ms. **The usdview freeze cost is a function of stage
prim count only, ≈9–10 µs per prim, not of how many curves were frozen.**

Two sources, both in the *synchronous* notice path:

* `_updateForStageChanges` (`appController.py:2504-2521`) sets
  `self._allSceneCameras = None` (`:2516`) and calls `_clearCaches` (`:1429-1437`)
  → `_refreshCameraListAndMenu` → `Utils._GetAllPrimsOfType(stage,
  UsdGeom.Camera)` (`:3048-3050`) — **a full stage traversal per freeze**. That is
  the 1.3 → 12.1 ms growth in the "author" column.
* `_resetGUI` (`:1956-1981`) calls `_resetPrimView` (`:1930-1954`) whenever
  `_hasPrimResync` (set at `:2513` from `ChangeNotice.RESYNC`, `:5509-5513`),
  which does `primView.clear()` + `_populateRoots()` + `_expandToDepth(3)` —
  the 102 ms at 11 005 prims. It is at least coalesced: `updateGUI()` starts a
  0-interval single-shot `_guiResetTimer` (`:551-555`, `:1983-1988`), so N freezes
  in one Python call produce one `_resetGUI`.

**Design consequence:** batch every freeze into one `Sdf.ChangeBlock` so usdview
pays one `_resetGUI` and one camera traversal; freeze into a *few* prims, not
per-clump (100 ms of prim-browser rebuild per freeze on a 10 k-prim rig); and an
interactive comb/sculpt brush must never author a resync per stroke — it edits
`points` on an existing frozen prim (`dirtied`, 0.02–0.13 ms, no
`_resetPrimView`, since `_onPrimsChanged` passes `hasPrimResync=False` for a
non-RESYNC notice, `:5509-5513`).

Undo/redo through the whole live usdview loop (2 205-prim stage, 100 k curves,
including the llvmpipe redraw): `SetActive(False)` 45.1 ms, `SetActive(True)`
161.0 ms, `RemovePrim` 90.7 ms. Subtract the redraw (§5) and the residue is
§4.1 + §4.3.

---

## 5. Benchmark protocol for the parts that are UNMEASURED here

Everything above is CPU. The freeze's real cost in a shipped viewport is the
delegate reacting to the `PrimsRemoved` + `PrimsAdded` pair that
`_ApplyPendingResyncs` emits (`stageSceneIndex.cpp:685-686`): Storm destroys the
rprim and reallocates every VBO. Storm here runs only on llvmpipe (`glxinfo -B` →
`llvmpipe (LLVM 20.1.2)`, `Accelerated: no`), so the 104–186 ms redraws above are
**CPU rasterisation and must never be quoted as Storm GPU numbers**. On a GPU
workstation:

1. **Scene.** `SCR/probes/freeze-bake/uv/makeStage.py N groomHost.usda` for
   N ∈ {100, 2 000, 10 000} rig prims, plus a frozen `BasisCurves` of
   {10 k, 100 k, 1 M} curves × 8 CV, `wrap=pinned`, `basis=bspline`, with the §2.1
   primvar set.
2. **Driver.** `testusdview --renderer GL --testScript uv/testUsdviewFreezeCost.py
   <stage>` — it already prints `FREEZECOST author_ms / resetGUI_only_ms /
   part <panel> / llvmpipeRedraw_ms / undoByDeactivate_ms / redoByActivate_ms /
   undoByRemovePrim_ms`. Rename the redraw key to `gpuRedraw_ms`.
3. **Counters.** usdview HUD → GPU stats, plus `hdSt/resourceRegistry` allocation
   counters and `HD_MDI` draw-item counts before/after each freeze. Expected: one
   rprim destroy + one create per freeze, with
   `points`/`widths`/`curveVertexCounts` reuploaded in full.
4. **Record.** (i) ms from `SetActive(True)` to first presented frame, per curve
   count; (ii) the same for the `RemovePrim` + re-`CopySpec` redo; (iii) peak GPU
   memory delta for a 1 M-curve freeze; (iv) whether a points-only edit (dirty
   `primvars/points`) avoids the topology re-upload — it should, and that is the
   whole argument for decision 10.
5. **Control.** Repeat (i) with the freeze muted/unmuted as a sublayer instead of
   deactivated/activated as a prim; both emit `PrimsRemoved`/`PrimsAdded`, so
   they should match — if they do not, the sublayer route pays an extra
   recomposition.

---

## Key facts

* **The `Applying predicate to invalid prim` error is stock OpenUSD 26.08, not
  RigExec, and not a `UsdPrimRange`.** `USD/pxr/exec/esfUsd/stageData.cpp:360`
  applies `UsdPrimDefaultPredicate` to a possibly-invalid prim;
  `USD/pxr/usd/usd/primFlags.cpp:22-27` posts `TF_CODING_ERROR` and returns
  `false` — the branch the caller wanted. Verified in C++
  (`probe2_undo_resync_exec.cpp`) and in Python against a compiled `_rigexec.Rig`
  (`probe3_python_rigged_undo.py`). It closes `RIG/docs/curvenet.md:513-522`.
* **It fires exactly when the resynced path has no prim at the end of the
  change.** `RemovePrim`, `del nameChildren[…]`, mute-sublayer and
  remove-sublayer raise; `SetActive(False)`, `CopySpec`-over-live, and
  remove+re-`CopySpec` inside one `Sdf.ChangeBlock` do not (9-case matrix, §1.4).
* **It is harmless.** The removal completes, the rig still evaluates, the next USD
  call is clean, and N removals batched in one `Sdf.ChangeBlock` coalesce into one
  exception (`probe9_error_containment.py`). `Tf.ErrorMark` is not bound in
  Python; contain with `try/except Tf.ErrorException` + a post-condition assert.
* **`SdfCopySpec` into an anonymous stash is an O(1)-memory, sub-millisecond
  whole-subtree snapshot.** 100 k curves × 8 CV: Capture 0.10 ms, undo 0.24 ms,
  redo 0.09 ms, **zero RSS growth** (COW `VtArray` sharing). 11/11 fidelity
  assertions pass, including time samples, `Ts.Spline`, nested children,
  relationships, `kind`, and uniform variability (`probe8_undo_fidelity.py`).
* **`Sdf.Layer` has no `RemovePrimSpec` in Python** (removal is
  `del parentSpec.nameChildren[name]` / `del layer.rootPrims[name]`), and
  **`UsdStage::RemovePrim` is a silent no-op** when the spec lives outside the
  current edit target — with usdview's session-layer default
  (`appController.py:1283`), root-layer prims cannot be removed at all
  (`probe10_resync_blast.cpp`: `still on stage: 1`, zero notices).
* **A frozen `BasisCurves` re-enters the graph straight from the scene index**:
  a filtering SI's `_GetInputSceneIndex()->GetPrim(frozenPath)` yields `points`,
  `rest`, `skinprim`, `skinprimuv` and `usdGen:curveId` with correct
  interpolations (`probe7`, case 2). No stage access needed.
* **Namespaced primvars survive verbatim** (`primvars:usdGen:curveId` → SI name
  `usdGen:curveId`, dirty `[primvars/usdGen:curveId]`), but **relationships and
  custom non-primvar attributes never reach Hydra and never invalidate**, on
  typed *or* untyped prims (`probe7`, 1d/1g/1h/1i). The staleness digest must be
  `primvars:usdGen:frozenEpoch` (`string`, `constant`) — it reaches the SI
  (`readback: 'sha1:bbb' interp=constant`) and dirties precisely. `customData` is
  worse still: `GetCustomDataByKey("usdGen:frozenEpoch")` returns `None` because
  it splits on `:` as a nested-dict key path.
* **Crate deduplicates identical value buffers**: `primvars:rest` sharing the same
  `VtArray` object as `points` costs **26 bytes**; a distinct buffer costs a full
  9.6 MB per 800 k CVs.
* **Bake to `.usdc`, never `.usda`**: 9.6 MB vs 40.3 MB, 10.6 ms vs 368 ms to
  write, **0.8 ms vs 532 ms** to reopen and read `points`.
* **24 point time samples cost 24×** the static size (230 MB for 100 k×8);
  `velocities` costs 2× (19.2 MB) and is what the stock velocity-motion SI wants.
* **USD→Hydra freeze cost is flat in curve count and < 0.6 ms** (author + apply +
  `GetPrim` + full pull) at 10 k, 100 k and 1 M curves. The only scaling term is
  `RemovePrim`'s deallocation: 0.06 / 1.47 / 13.47 ms.
* **usdview's per-freeze cost is O(stage prims), not O(curves)**: `_resetPrimView`
  2.6 / 23.0 / 102.2 ms at 115 / 2 205 / 11 005 prims, identical for 10 k and
  100 k curves; plus a full-stage camera traversal in the synchronous notice
  handler (`appController.py:2516`, `:3048-3050`).
* **Re-authoring a parent scope's `typeName` resyncs its whole subtree**: 203
  `PrimsAdded` over 200 siblings, 1.3–1.5 ms of SI work and 203 delegate
  destroy/create cycles.

## Decisions this settles

1. **Undo unit = `SubtreeSnapshot`** (`Sdf.CopySpec` into an anonymous stash;
   `del parentSpec.nameChildren[name]` to remove; one `Sdf.ChangeBlock` per
   `Edit`), slotted into `rigExecUndo`'s existing `EditEntry` / `Edit` /
   `UndoStack` unchanged. `EditEntry` keeps its `layer` field and its
   `layer.expired` guard, which §1.5 makes load-bearing.
2. **The primary undo of a live freeze is `active = false`, not removal.** It is
   O(1) (0.02 ms at 1 M curves), it does not trip the OpenExec diagnostic, and
   Hydra treats it as a `PrimsRemoved` exactly like a real removal. The stash-backed
   real removal is the *purge* path, run when the undo entry falls off the
   200-deep stack, where the 1–13 ms `free()` and the spurious diagnostic are
   both off the interactive path.
3. **Do not wait for an upstream fix to the OpenExec resync bug.** Contain it:
   C++ `TfErrorMark` + `Clear()` in usdGen's own code, `try/except
   Tf.ErrorException` + post-condition assert in the usdview tools, and batch
   removals into one `Sdf.ChangeBlock`. File it upstream against
   `esfUsd/stageData.cpp:360` (guard `resyncedPrim` with `if (!resyncedPrim ||
   !UsdPrimDefaultPredicate(resyncedPrim))`).
4. **Frozen-curve schema contract** = a plain `UsdGeomBasisCurves` carrying
   `points / curveVertexCounts / type / basis / wrap / widths` plus
   `primvars:rest` (`point3f[]`, `vertex`), `primvars:skinprim` (`int[]`,
   `uniform`), `primvars:skinprimuv` (`texCoord2f[]`, `uniform`),
   `primvars:usdGen:curveId` (`int[]`, `uniform`) and
   `primvars:usdGen:frozenEpoch` (`string`, `constant`). Optional
   `primvars:usdGen:rootFrame` (`matrix4d[]`, `uniform`). Consumers must test for
   a *value*, not for the primvar's presence.
5. **`primvars:rest` is authored at every freeze** — it is 26 bytes while it
   still shares the points buffer, and it is the input `DeformWithSurface`
   needs (A7 §3.7 / §9.1 G7).
6. **The staleness digest lives in a `constant` primvar**, never in `customData`
   or a custom attribute — those are invisible to Hydra and produce no
   invalidation.
7. **usdGen registers its own `UsdImagingPrimAdapter` per operator prim type**
   (or drives its SI from an evaluator-owned store, as
   `RIG/libs/rigExecImaging/sceneIndexPlugin.cpp:29-40` does). Authoring
   `usdGen:source` as a relationship and expecting the scene index to see it does
   not work.
8. **Freeze landing places, in order:** T1 session layer for the interactive loop
   (0.4 ms, no I/O); T2 sidecar `.usdc` sublayered for "commit to stage"; T3
   sidecar `.usdc` payload for very heavy grooms (21.5 ms to compose, but
   unloadable). Never `.usda`. Never bake motion samples — author `velocities`.
9. **Freeze prims are siblings under a dedicated scope** and the tooling never
   touches the parent scope's spec.
10. **The comb/sculpt brush edits `points` on a live frozen prim**, producing a
    `primvars/points` dirty (0.02–0.13 ms, no `_resetPrimView`), and re-freezes
    only on commit. One `Sdf.ChangeBlock` per stroke-commit so usdview pays one
    coalesced `_resetGUI`.

## Open questions

* **GPU cost of the `PrimsRemoved`+`PrimsAdded` pair.** Unmeasurable here
  (llvmpipe only). §5 is the protocol. Until it is run, the claim "a freeze is
  sub-millisecond" is true only up to the delegate boundary.
* **Whether a `primvars/points` dirty really avoids a topology re-upload in
  Storm** — expected but unverified; it is the load-bearing assumption behind
  decision 10.
* **`SetActive(false)` and instancing/prototypes.** Deactivation was tested on
  ordinary prims. If usdGen freezes point-instanced hair, deactivating an
  instance master may behave differently.
* **`Sdf.CopySpec` and composition arcs.** The fidelity test covered a
  session-layer subtree with no arcs; a tier-T3 freeze carrying a payload has not
  been round-tripped through a stash.
* **Whether the OpenExec bug is already fixed upstream past 26.08.** Not checked.
* **Multi-layer freezes.** If an artist has changed usdview's edit target away
  from the session layer, tier T1 silently changes meaning (§1.5). The tool
  should probably pin its own edit target rather than trust
  `stage.GetEditTarget()`; not prototyped.
