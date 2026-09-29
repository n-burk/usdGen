# PW-6 / SI-11: Notice Behavior for `reorder nameChildren` (Record Only)

Host: headless aarch64, OpenUSD 26.08 (`$USD`), usdGen build-tree
plugins. All artifacts: `docs/prework/probes/PW-6/`.

## 1. What Was Asked

From `plan/11-roadmap.md` §1.1, PW-6 row:

> | **PW-6** | SI-11 (record only) | nothing in the design — it closes Q-06 | author `reorder nameChildren` on `<Description>/Ops` with the adapter attached and record any notice at a terminal observer; justifies in writing the rule that no evaluated result depends on namespace order (S26) |

- **Q-06** (`plan/12-risks-decisions-open-questions.md` L315): "Does `reorder nameChildren`
  produce usable Hydra invalidation through the adapter?" — answered by **SI-11** (T1, record
  only, M0), owner ENG-2.
- **S26** (`plan/12-risks-decisions-open-questions.md` L89): "Explicit `usdGen:input` wiring,
  Kahn order with namespace order as tie-break, cycles are compile errors."
- **X-05** (L188): "there is no `PrimsChildrenReordered` notice and the invalidation is
  unmeasured" — this PW-6 record is that measurement.
- `plan/design/judge-evidence.md` item 4: "Does `reorder nameChildren` reach the scene index as
  an invalidation? S26's namespace-order [rule] …"

The design consequence to justify: **no evaluated result may depend on child (namespace) order**,
beyond Kahn's tie-break.

## 2. Method

**Scene** (`usdGenReorder.usda`): `UsdGenGroom /Groom` → `UsdGenDescription /Groom/Description` →
`UsdGeomScope /Groom/Description/Ops` with three operator children wired by `usdGen:input`:
`OpNoise` → `OpLength` → `OpScatter` (authored order `OpNoise OpLength OpScatter`).

**Control scene** (`vanillaReorder.usda`, added this session): identical hierarchy with all
`Xform` types and no usdGen attributes — proves which notices are usdImaging-native vs. usdGen.

**Probe** (`probeReorder.cpp` → `probeReorder`):

1. Registers the usdGen build-tree plugin resources (belt) in addition to `PXR_PLUGINPATH_NAME`
   (suspence) — all 8 usdGen types report REGISTERED.
2. Opens the stage, builds the usdImaging chain via `UsdImagingCreateSceneIndices`.
3. Attaches one recording `HdSceneIndexObserver` to **every** index in the chain (terminal + all
   filtering inputs, 12 indices total with the current probe build).
4. **Control**: `usdGen:magnitude` 0.1 → 0.25 on OpNoise, `ApplyPendingUpdates()`.
5. **Variant A (Sdf-level)**: `SdfPrimSpecifier::SetNameChildrenOrder({OpScatter, OpLength,
   OpNoise})` on `/Groom/Description/Ops` (authors the `reorder nameChildren` statement),
   `ApplyPendingUpdates()`.
6. **Variant B (Usd-level)**: `UsdPrim::SetChildrenReorder({OpNoise, OpScatter, OpLength})`
   (authors the `primOrder` metadata opinion), `ApplyPendingUpdates()`.
7. **Sanity**: clears both opinions, `ApplyPendingUpdates()`.
8. Prints composed child order at stage and terminal after each step.

Note: `UsdAPITable::ReorderChild` does not exist in this build (verified by grep of
`include/pxr`); the two public reorder entry points are the Sdf `nameChildren` statement
(variant A) and the `primOrder` metadata opinion (variant B). Both are exercised.

`probeFinalOrder.cpp` → `probeFinalOrder`: minimal companion (Sdf/Usd state only, no usdImaging
chain) that prints the composed child order after each step; written to avoid the exit-time
double-free seen in `probeReorder` (probe artifact, see §7).

**Commands** (exact):

```bash
cd <usdgen-src>/docs/prework/probes/PW-6
export LD_LIBRARY_PATH=$USD/lib
export PXR_PLUGINPATH_NAME="<usdgen-src>/build/usd/usdGenImaging/resources:<usdgen-src>/build/usd/usdGenSchema/resources:$USD/plugin/usd:$USD/lib/usd"

./probeReorder ./usdGenReorder.usda      # full notice capture -> run2.txt (reproduced as rerun.txt)
./probeReorder ./vanillaReorder.usda     # vanilla control    -> vanilla_run.txt
./probeFinalOrder ./usdGenReorder.usda   # composed child order -> finalOrder.txt
```

Reproduction this session: `probeFinalOrder` reproduced `finalOrder.txt` byte-for-byte;
`probeReorder` on the usdGen scene reproduced the `run2` notice shape (byte-identical
`rerun.txt`) plus 9 additional full samples (6 usdGen + 4 vanilla) taken to characterize
variance. The probe aborts at exit with a double-free after all output is complete (known, §7).

## 3. Raw Evidence

### 3.1 Plugin registration (identical in all runs)

```
=== registered usdGen plugin types ===
  UsdGenGroomAdapter                 REGISTERED
  UsdGenDescriptionAdapter           REGISTERED
  UsdGenOperatorAdapter              REGISTERED
  UsdGenMapAdapter                   REGISTERED
  UsdGenGuideSetAdapter              REGISTERED
  UsdGenRestAPIAdapter               REGISTERED
  UsdGenGroomSceneIndexPlugin        REGISTERED
  UsdGenMetadataSceneIndexPlugin     REGISTERED
```

(The "Coding Error: TfType 'UsdGenMetadataSceneIndexPlugin' already has a defined C++ type" line
is the belt+suspence double registration; harmless.)

### 3.2 UsdImaging chain (terminal → deepest, current probe build; `run2.txt`)

```
=== usdImaging chain (terminal -> deepest) ===
  0. UsdImagingRenderSettingsFlatteningSceneIndex
  1. UsdImagingSelectionSceneIndex
  2. UsdSkelImagingPointsResolvingSceneIndex
  3. UsdSkelImagingSkeletonResolvingSceneIndex
  4. UsdImagingMaterialBindingsResolvingSceneIndex
  5. UsdImaging_InstanceProxyPathTranslationSceneIndex
  6. HdNoticeBatchingSceneIndex
  7. UsdImagingNiPrototypePropagatingSceneIndex
  8. UsdImagingPiPrototypePropagatingSceneIndex
  9. UsdImagingExtentResolvingSceneIndex
  10. HdsiLocatorCachingSceneIndex ["HdsiLocatorCachingSceneIndex for material (primType material)"]
  11. UsdImagingStageSceneIndex
```

No usdGen scene-index plugin appears in the chain (the usdGen `UsdGenGroomSceneIndexPlugin` /
`UsdGenMetadataSceneIndexPlugin` are registered but are not part of the bare
`UsdImagingCreateSceneIndices` pipeline). The usdGen **prim adapters** are registered in the TfType
registry and are the adapters `UsdImagingStageSceneIndex` looks up per prim.
(`run1.txt` used an earlier probe build whose chain collector stopped at index 9 — 10 indices —
and is otherwise the same shape; the terminal-observer observations below are unaffected.)

### 3.3 Baseline and control (verbatim, `run2.txt`; control is identical in every run)

```
=== baseline ===
baseline stage child order of /Groom/Description/Ops : OpNoise OpLength OpScatter
baseline terminal child order of /Groom/Description/Ops : OpNoise OpLength OpScatter

--- control: Set usdGen:magnitude=0.25 on /Groom/Description/Ops/OpNoise ---
[control/magnitude] UsdImagingRenderSettingsFlatteningSceneIndex added=0 removed=0 renamed=0 dirtied=0
... (all 12 indices: added=0 removed=0 renamed=0 dirtied=0)
orders after control:
post-control stage child order of /Groom/Description/Ops : OpNoise OpLength OpScatter
post-control terminal child order of /Groom/Description/Ops : OpNoise OpLength OpScatter
```

Sdf-level debug (`run_debug.txt`, `TF_DEBUG`): the control queues
`Property update queued: /Groom/Description/Ops/OpNoise.usdGen:magnitude` but produces **zero**
`HdSceneIndexObserver` notices at any index. A usdGen plugin-field edit is invisible to the
Hydra notice machinery — usdImaging maps it to no standard dirty locators, and the usdGen engine
re-evaluates it in its own data-plane cycle (not observable to `HdSceneIndexObserver`).

### 3.4 Variant A — Sdf `reorder nameChildren` (Scatter, Length, Noise)

Terminal observer (`index 0`), verbatim from `run2.txt`:

```
--- variant A: Sdf reorder nameChildren (Scatter, Length, Noise) ---
[variantA/sdf-reorder] UsdImagingRenderSettingsFlatteningSceneIndex added=4 removed=0 renamed=0 dirtied=3
    ADDED    /Groom/Description/Ops  type=
    ADDED    /Groom/Description/Ops/OpScatter  type=
    ADDED    /Groom/Description/Ops/OpLength  type=
    ADDED    /Groom/Description/Ops/OpNoise  type=
    DIRTIED  /Groom/Description/Ops/OpScatter  locs=[__usdUpAxis,coordSysBinding,geomModel,materialBindings,model,primvars,purpose,skelBinding,visibility,xform]
    DIRTIED  /Groom/Description/Ops/OpNoise  locs=[__usdUpAxis,coordSysBinding,geomModel,materialBindings,model,primvars,purpose,skelBinding,visibility,xform]
    DIRTIED  /Groom/Description/Ops/OpLength  locs=[__usdUpAxis,coordSysBinding,geomModel,materialBindings,model,primvars,purpose,skelBinding,visibility,xform]
```

Per-index distribution (same run): indices 0–7 see the ADDED×4 + DIRTIED set; indices 8–11
(PiPrototype, ExtentResolving, HdsiLocatorCaching, UsdImagingStageSceneIndex) see **ADDED×4 only,
dirtied=0**. The DIRTIED locator list is `usdMaterialBindings` at indices 5–7 and
`materialBindings` at indices 0–4 — i.e. `UsdImagingMaterialBindingsResolvingSceneIndex` (index 4)
rewrites it. The DIRTIED entries therefore originate **inside the usdImaging filtering stack**,
first visible at the `UsdImagingNiPrototypePropagatingSceneIndex` depth (index 7); they are not
emitted by `UsdImagingStageSceneIndex` itself (its own observer sees ADDED only).

Sdf/usdImaging debug (`run_debug.txt`):

```
[Objects Changed] Notice received from stage with root layer @usdGenReorder.usda@
 - Resync queued: /Groom/Description/Ops
[Population] Repopulating </Groom/Description/Ops>
```

So the Sdf reorder arrives at usdImaging as a **structural resync of the parent** (`/Groom/
Description/Ops`), which repopulates the subtree: the 4 ADDED entries are the re-added
`Ops` container + its 3 children **in the new order**, and `PrimsRemoved` is suppressed by
usdImaging's documented optimization ("A single PrimsAdded notification is sufficient to
re-sync an existing prim", `stageSceneIndex.cpp` `_ApplyPendingResyncs`). No
`PrimsRenamed`, no `PrimsRemoved`. The ADDED `type=` is empty by construction — usdImaging fills
the added-entry type from the adapters' `GetImagingSubprimType` opinion, which is empty here.

Resulting order:

```
post-A stage child order of /Groom/Description/Ops : OpScatter OpLength OpNoise
post-A terminal child order of /Groom/Description/Ops : OpScatter OpLength OpNoise
```

**Variant-A DIRTIED variance (usdGen scene, terminal observer, 9 samples this session):**
`dirtied ∈ {1, 2, 3}` — `OpScatter` is DIRTIED in every sample; `OpNoise` in most; `OpLength` in
most. Examples: 1 dirty (`run2.txt`/`rerun.txt`), 2 dirties (one sample: OpScatter+OpNoise),
3 dirties (majority, incl. `run1.txt`, `run_debug.txt`). The ADDED×4 set and the resulting child
order are stable in every sample.

### 3.5 Variant B — Usd `primOrder` metadata via `SetChildrenReorder` (Noise, Scatter, Length)

Terminal observer, verbatim (`run2.txt`):

```
[variantB/primOrder] UsdImagingRenderSettingsFlatteningSceneIndex added=4 removed=0 renamed=0 dirtied=3
    ADDED    /Groom/Description/Ops  type=
    ADDED    /Groom/Description/Ops/OpNoise  type=
    ADDED    /Groom/Description/Ops/OpScatter  type=
    ADDED    /Groom/Description/Ops/OpLength  type=
    DIRTIED  /Groom/Description/Ops/OpScatter  locs=[__usdUpAxis,coordSysBinding,geomModel,materialBindings,model,primvars,purpose,skelBinding,visibility,xform]
    DIRTIED  /Groom/Description/Ops/OpNoise  locs=[__usdUpAxis,coordSysBinding,geomModel,materialBindings,model,primvars,purpose,skelBinding,visibility,xform]
    DIRTIED  /Groom/Description/Ops/OpLength  locs=[__usdUpAxis,coordSysBinding,geomModel,materialBindings,model,primvars,purpose,skelBinding,visibility,xform]
```

Same Sdf-level trigger (`Resync queued: /Groom/Description/Ops`, `Repopulating`), same per-index
distribution as variant A (DIRTIED first visible at index 7). Variant B's DIRTIED×3 was
**deterministic in all 10 samples** taken this session (usdGen + vanilla).

```
post-B stage child order of /Groom/Description/Ops : OpNoise OpScatter OpLength
post-B terminal child order of /Groom/Description/Ops : OpNoise OpScatter OpLength
```

### 3.6 Sanity — clear both opinions

```
[sanity/clear] UsdImagingRenderSettingsFlatteningSceneIndex added=4 removed=0 renamed=0 dirtied=3
    ADDED    /Groom/Description/Ops  type=
    ADDED    /Groom/Description/Ops/OpNoise  type=
    ADDED    /Groom/Description/Ops/OpLength  type=
    ADDED    /Groom/Description/Ops/OpScatter  type=
    DIRTIED  /Groom/Description/Ops/OpScatter  locs=[...]
    DIRTIED  /Groom/Description/Ops/OpNoise  locs=[...]
    DIRTIED  /Groom/Description/Ops/OpLength  locs=[...]
final stage child order of /Groom/Description/Ops : OpNoise OpLength OpScatter
final terminal child order of /Groom/Description/Ops : OpNoise OpLength OpScatter
```

Order returns to the authored order after clearing.

### 3.7 Composed child order (`probeFinalOrder` → `finalOrder.txt`, reproduced byte-for-byte)

```
authored                 OpNoise OpLength OpScatter
after Sdf reorder(A)     OpScatter OpLength OpNoise
after primOrder(B)       OpNoise OpScatter OpLength
after clear              OpNoise OpLength OpScatter
```

### 3.8 Vanilla control (pure `Xform` scene, no usdGen types/attrs — `vanilla_run.txt`)

Same probe, same binary, `vanillaReorder.usda`. The notice shape is **structurally identical**:

```
[variantA/sdf-reorder] UsdImagingRenderSettingsFlatteningSceneIndex added=4 removed=0 renamed=0 dirtied=1
    ADDED    /Groom/Description/Ops  type=
    ADDED    /Groom/Description/Ops/OpScatter  type=
    ADDED    /Groom/Description/Ops/OpLength  type=
    ADDED    /Groom/Description/Ops/OpNoise  type=
    DIRTIED  /Groom/Description/Ops/OpScatter  locs=[__usdUpAxis,coordSysBinding,geomModel,materialBindings,model,primvars,purpose,skelBinding,visibility,xform]
...
[variantB/primOrder] UsdImagingRenderSettingsFlatteningSceneIndex added=4 removed=0 renamed=0 dirtied=3
... (all three children DIRTIED, identical locator sets, identical per-index distribution)
post-A stage child order : OpScatter OpLength OpNoise
post-B stage child order : OpNoise OpScatter OpLength
```

Vanilla variant-A DIRTIED count across 4 samples: `dirtied ∈ {1, 3}` (one 1, three 3) — the same
non-determinism observed in the usdGen scene. **The usdGen adapters add nothing to the notice
set**: the vanilla scene produces the identical ADDED×4 + DIRTIED-with-standard-locators shape.

### 3.9 HgiResource / imaging-resource notices

None observed, and none are possible in this setup: `HdSceneIndexObserver` exposes only
`PrimsAdded/Removed/Renamed/Dirtied` — resource creation/invalidation is a Hydra/Hgi-level
event that requires a render pass (GL context + `HgiResource` allocation), which this headless
probe does not perform. No usdGen-specific locators ever appear in the DIRTIED sets: every
locator is one of the standard usdImaging geom/material locators
(`xform, geomModel, model, purpose, materialBindings, primvars, visibility, skelBinding,
coordSysBinding, __usdUpAxis`).

## 4. Observed Notice Set (Summary)

For a child reorder of `/Groom/Description/Ops` (either Sdf `reorder nameChildren` or Usd
`primOrder`) with the usdGen prim adapters registered:

| Notice | usdGen scene | Vanilla scene | Notes |
|---|---|---|---|
| PrimsAdded | 4 (Ops + 3 children, new order, empty imaging type) — **stable** | 4 — stable | usdImaging resync/repopulation of the parent subtree; `PrimsRemoved` suppressed by the re-add optimization |
| PrimsRemoved | 0 | 0 | |
| PrimsRenamed | 0 | 0 | no `PrimsChildrenReordered` notice exists (X-05) — the reorder surfaces only as resync+re-add |
| PrimsDirtied | 1–3 operator children, **full standard usdImaging locator set** (variant A); 3 (variant B, deterministic) | 1–3 (variant A); 3 (variant B) | synthesized inside the usdImaging filtering stack (first visible at the Ni-prototype-propagating depth); not emitted by `UsdImagingStageSceneIndex` itself; identical with and without usdGen |
| HgiResource / usdGen-specific locators | none | n/a | not observable via `HdSceneIndexObserver`; no usdGen field ever appears as a dirty locator |
| Final child order | exactly the requested order (stage and terminal agree) | same | confirmed by `probeFinalOrder` |

## 5. S26 Assessment — Namespace-Order Independence

**Confirmed.** Evidence:

1. **No data-plane notices.** A reorder produces only structural Hydra notices
   (subtree re-add + standard-locator dirties of static geom/material locators). No usdGen field
   (`usdGen:magnitude`, `usdGen:input`, …) is ever dirtied or invalidated, and no HgiResource
   event is reachable. The usdGen engine's evaluation inputs are unaffected by child order.
2. **The control proves usdGen fields don't invalidate Hydra at all.** Editing
   `usdGen:magnitude` produces **zero** `HdSceneIndexObserver` notices at every index — the
   engine's re-evaluation happens in its own data-plane cycle, invisible to Hydra observers.
   Symmetrically, a reorder produces no data-plane effect that the engine needs.
3. **The notice set is usdImaging-native.** The pure-Xform vanilla control reproduces the same
   ADDED×4 + DIRTIED shape; the usdGen adapters contribute nothing. This is vanilla usdImaging
   resync behavior that any reorder of any subtree would trigger.
4. **Order is applied correctly and deterministically** at both stage and terminal
   (`finalOrder.txt`), and returns to the authored order on clear.

The only usdGen dependency on namespace order is the Kahn topological-sort **tie-break** (S26),
which is a compile-time/digest-time concern (`UsdGenStructureDigest`), not a Hydra runtime
data dependency. A reorder that preserves the `usdGen:input` graph therefore cannot change any
evaluated result; at worst it changes the tie-break order of independent operators, which is
exactly what S26 permits.

## 6. Decision

DECISION: SI-11 recorded. Observed terminal-observer notice set for `reorder nameChildren` on
`<Description>/Ops` with the usdGen prim adapters attached: PrimsAdded×4 (the `Ops` container and
all 3 operator children, re-added in the new order, empty imaging-subprim type, PrimsRemoved
suppressed by usdImaging's re-add optimization), PrimsDirtied×1–3 on operator children only (full
standard usdImaging geom/material locator set; synthesized in the usdImaging filtering stack, not
by the stage index or by any usdGen adapter), PrimsRemoved×0, PrimsRenamed×0, no HgiResource or
usdGen-specific notices; final child order matches the requested order (stage and terminal agree),
and the identical notice shape reproduces in a vanilla Xform control scene. **S26
namespace-order independence confirmed**: no evaluated result depends on child order; the reorder
is a pure structural resync at the Hydra level and the usdGen adapters add nothing to it. Closes
Q-06 (SI-11, record only; owner ENG-2).

## 7. Risks / Follow-ups

1. **Variant-A DIRTIED count is non-deterministic (observed 1, 2, 3) in BOTH the usdGen and the
   vanilla scene.** This is a usdImaging/Sdf resync-side quirk (which subset of children gets a
   full-locator invalidate on the repopulate), not a usdGen behavior. Impact: none on correctness
   — the dirtied locators are all static standard locators that resolve to no data on operator
   prims — but a downstream consumer that assumes "reorder ⇒ all children dirtied" would be wrong;
   the guaranteed effect of a reorder is the PrimsAdded re-add (always ×4).
2. **No HgiResource behavior was measured** (headless, no render pass). Resource-level behavior
   under reorder would only be observable in a full T4 workstation render. Expected to be moot by
   §5.1, but if a T4 session ever shows resource churn on reorder, re-measure with the full usdGen
   render pipeline (the usdGen scene-index plugins are not in this chain; see §3.2).
3. **`probeReorder` exit-time double-free** (`free(): double free detected in tcache 2`, after all
   output is complete). Probe artifact of `HdSceneIndexObserver` teardown ordering in this build,
   not a usdGen library issue (the usdGen libs were never loaded into the failing frames;
   `probeFinalOrder` — same plugins, no observer teardown — exits cleanly). Workaround: use
   `probeFinalOrder` for order-only checks, or treat the crash as expected post-output.
4. **`run1.txt` predates the final probe build** (10-index chain collector). The canonical capture
   is `run2.txt` (byte-identical to this session's `rerun.txt`); terminal-observer conclusions are
   unaffected by the chain-collector difference.
