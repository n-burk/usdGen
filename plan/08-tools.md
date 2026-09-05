# usdGen usdview tools

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document specifies the usdview tooling for usdGen: the plugin container and its Qt-free module
split, the two Python surfaces that reach the C++ evaluator, the ten-brush shelf and the four-phase
stroke contract behind it, the freeze/commit flows, the panels, picking and selection, the stage-write
and undo model, the loop's performance targets, and the tests that prove all of it. It is the plan for
milestone **M5** (ADR §7), which freezes contract **C4** (the C ABI plus the pxr_boost array surface,
ADR §3). Everything here is downstream of decisions already taken: the tool never authors curve data
the evaluator could compute, it never touches the stage during a drag, and it never asks Hydra for
anything the CPU can answer more cheaply.

Reads with: `01-architecture.md` (the invariants I1–I8), `02-schema.md` (prim
types, the reserved layout, the mask block, freeze and sculpt properties), `03-execution-engine.md`
(capture/evaluate, chunks and tiles, `UsdGenNodeStats`), `05-static-curves-and-deformation.md`
(the frozen-curve contract C3 and the deformer), `06-imaging.md` (the registry, the generation store,
live overrides, `primOrigin`, invalidation), `07-look-maps-expressions.md` (paint maps, bake and
reload), `09-performance-and-benchmarks.md` (the frame ledger §0.2, the gate registry §5),
`10-build-dependencies-testing.md` (the `usdGenUsdview` target, the env-var registry §3.5, the CTest
name registry §5.6, and `docs/workstation-protocol.md` whose section numbering §5.2 owns),
`11-roadmap.md` (M5's exit criteria), `12-risks-decisions-open-questions.md` (RK-08, the
`Tf.Error.Mark` containment),
`appendix-A-evidence-ledger.md` (§2, the `EV-nnn` rows every measured number here cites).

---

## 0. Requirements and evidence

### 0.1 What R9 asks for, and what answers it

R9 is: *"usdview tools on the Storm viewport: freeze an operator's output, then comb/groom manually;
app → data modification → Hydra loop during interaction; commit to the stage via the tool"*
(`design/brief-v1.md` §1).

| R9 clause | Mechanism in this document | Evidence |
|---|---|---|
| freeze an operator's output | `UsdGenFreeze` caps the chain; the freeze bar authors a `UsdGeomBasisCurves` under `<Description>/Frozen/` and points `usdGen:frozen:curves` at it | §4.1; ADR §2.3; S42 |
| then modify manually | the ten-brush shelf; comb/grab/smooth commit to guide `points` or a `UsdGenSculptLayer` | §3; ADR §6 |
| app → data modification → Hydra loop | live overrides through the C ABI into the session; only the affected tiles republish; no stage traffic per move | §2.3; S40; `research/G-tool-loop…` §1.4 |
| commit to the stage via the tool | one write at release, one `Sdf.ChangeBlock`, inside an `EditRecorder` bracket, into the current edit target — which the status line names; the cook happens inside the repaint, at the end of the `_PrimsDirtied` batch that carried the edit, and the tool calls no `Commit()` | §2.4, §7; ADR §9.4 R32; S41, S43 |
| on the Storm viewport | Storm is the only interactive delegate in v1; the shelf drives the session, not the renderer, so it works against any | ADR §4.5 |

### 0.2 The settled decisions this document implements

| Ref | Decision | Where it lands here |
|---|---|---|
| S39 | two Python surfaces — ctypes C ABI for control/scalars, `pxr_boost.python` module for arrays; numpy as the kernel language over `np.asarray(vt)`; BLAS pinned to 1 before `import numpy`; per-move Python cost 21.3 µs | §1.4–§1.7 |
| S40 | live override during the drag, one write at release; CV picking CPU in C++; CV display as a synthesized `points` prim; Hydra CV highlight unreachable | §2, §3, §6 |
| S41 | `SubtreeSnapshot` undo; undo of a live freeze is `SetActive(false)`; one `Sdf.ChangeBlock` per structural edit, `Define` outside it; the `RemovePrim`/OpenExec diagnostic contained, never relied on | §4, §7 |
| S42 | frozen-curve contract; landing tiers `session` / `.usdc` `sublayer` / `.usdc` `payload` (token values only — ADR §9.1 R1 retires S42's "T1/T2/T3" numbering); never `.usda`; never baked motion samples; freezes are siblings under a dedicated scope | §4 |
| S43 | `PluginContainer` conventions: state in an `__init__`-initialised object, lazy Qt, one undo stack, the signal's frame, `UpdateViewport()` after every edit and undo, one `_resetGUI` per batched freeze, DLL override, `"[Terminal SI]"` prefix in tests | §1.2–§1.3, §9 |
| ADR §6 tools | risk §8 architecture + artist §8 UX; generated parameter panels; mask visualisation; "show driving guides"; symmetry (mirror-X) is an M5 requirement | §1, §3.4, §5 |

### 0.3 Vocabulary

Milestones are **M0–M8** (ADR §7); tools are **M5**. Gates carry the ADR §7 ids; the tool gates are
**T-1 … T-5** and **T-INST-1/2**. Test *tiers* are **T0–T4**, a different axis from the `T-` gate ids:
T0 = unit, no Hydra and no stage; T1 = headless scene index over the real
`UsdImagingCreateSceneIndices` chain; T2 = Storm through the EGL harness; T3 = `testusdview`;
T4 = workstation protocol — `docs/workstation-protocol.md`, whose numbered sections
`10-build-dependencies-testing.md` §5.2 owns (S45). M0 pre-work items are **PW-1…PW-13**, never `P-n`
(ADR §9.1 R1; `11-roadmap.md` §1 owns the numbering — §1.1 is PW-1…PW-6, the M0 decision checks;
§1.2 is PW-7…PW-13, the harness pre-work — and `10-…` §6.5 restates PW-1…PW-6). Freeze *landing* tiers
are the tokens `session | sublayer | payload` and are never numbered (ADR §9.1 R1). Every number is tagged **MEASURED** (with its report section and
its `appendix-A-evidence-ledger.md` **`EV-nnn`** row), **DERIVED from `EV-nnn`** (scaled or
interpolated — never MEASURED), **UNMEASURED** (with its gate) or **ASSUMPTION** (ADR §9.5 R42). The
rows this document cites live in `appendix-A-evidence-ledger.md` §2.1 (`EV-002`), §2.3
(`EV-020`–`EV-022`), §2.4 (`EV-034`), §2.6 (`EV-040`–`EV-052`) and §2.7 (`EV-055`–`EV-066`). Row ids
are the `EV-0nn` column of appendix A §2, never the retired `A2.x-` or `EV-6nn` handles its §2.0
legacy map still lists.

---

## 1. Architecture

### 1.1 Where the code lives

Every module named below lives in the Python package **`usdgen`** — all lowercase, mirroring
`rigexec` (ADR §9.1 R5) — installed at `lib/python/usdgen/`. `_usdGen` is therefore `usdgen._usdGen`,
and every intra-package import is relative (`from . import usdGenLib`).

The plugin is the CMake target **`usdGenUsdview`** (ADR §6 build): a `"Type": "python"` plugin whose
only registered type is `usdgen.usdGenUsdview.UsdGenUsdviewContainer` with
`"bases": ["pxr.Usdviewq.plugin.PluginContainer"]`, found through `PXR_PLUGINPATH_NAME` with the
package's parent directory on `PYTHONPATH` (`research/A3-usdrig-tools.md` §1.1). The module ends with
`Tf.Type.Define(UsdGenUsdviewContainer)` because usdview loads containers through `Plug` and the base
is itself a defined `Tf.Type` (`pxr/usdImaging/usdviewq/plugin.py:129`); usdview then calls
`registerPlugins` and `configureView` (`plugin.py:113,121,333,342`).

**Install every module.** usdRig installs three of its fifteen plugin modules
(`research/A3-usdrig-tools.md` §5.3), so an installed layout fails its own lazy imports. The T3 suite
runs against the build tree and against the install tree.

### 1.2 Module split and the Qt-free rule

Only the `*UI.py` modules import Qt, and only inside a menu callback, so every model module is
importable headlessly and testable at tier T0 (S43; `research/A3-usdrig-tools.md` §1.3 "Qt-free rule").

| Module | Qt? | Role |
|---|:--:|---|
| `usdgen/__init__.py` | no | package init; imports nothing, so `import usdgen` stays cheap in a headless test |
| `usdGenUsdview.py` | no | the `PluginContainer`; all mutable state in a `UsdGenToolState` dataclass built in `__init__` (§1.3), never assigned during `registerPlugins` — that drift broke usdRig's own fixture (S43, S46) |
| `usdGenLib.py` | no | ctypes binding of the C ABI; `USDGEN_IMAGING_DLL` override; pins BLAS threads **before** the package's first `import numpy` (§1.4) |
| `usdGenUndo.py` | no | `AttributeSnapshot`, `RelationshipSnapshot`, `SubtreeSnapshot`, `Edit`, `UndoStack(LIMIT=200)`, `EditRecorder` (§7.3) |
| `usdGenGraphModel.py` | no | read and author the chain; Kahn order and cycle detection mirroring the C++ compiler; every `usdGen:input` rewrite |
| `usdGenBrushMath.py` | no | numpy kernels over `np.asarray(vt)`: falloff, comb, grab, smooth, length, cut, paint, symmetry |
| `usdGenPick.py` | no | wrapper over `UsdGenImaging_PickCV` / `_Footprint` / `_ClosestSurfacePoint`; surface pick through a **fresh** `UsdImagingGL.RenderParams` |
| `usdGenFreezeAuthor.py` | no | freeze / unfreeze / import / commit authoring helpers (§4), pure `Sdf`/`Usd` |
| `usdGenPanelUI.py` | yes | groom panel, description tree, stack editor, generated parameter editor, freeze bar |
| `usdGenBrushUI.py` | yes | viewport toolbar, overlay, event filter, brush controllers |
| `usdGenMapPaintUI.py` | yes | paint mode, channel picker, bake dialog |
| `usdGenDiagnosticUI.py` | yes | stats HUD, mask visualisation, "show driving guides" |

Viewport tools are installed from `signalStageReplaced`, not from `registerPlugins`, because plugins
load before the `StageView` exists (`pxr/usdImaging/usdviewq/appController.py:432` configures plugins;
the `StageView` is constructed at `:1863`). The installer retries itself on a bounded 0 ms
`QTimer.singleShot` — usdRig retries up to 20 times — and remembers a failed install so a headless
session warns once (`research/A3-usdrig-tools.md` §1.2).

### 1.3 `UsdGenToolState`

```python
@dataclasses.dataclass
class UsdGenToolState:
    lib:            object = None        # ctypes handle, usdGenLib.Library()
    activated:      bool = False
    stageCacheId:   int = -1
    groomRoot:      str = ""
    generation:     int = 0              # last value seen from GetGeneration()
    topoGeneration: int = 0              # last GetTopologyGeneration(); see 3.4, 6.3
    undoStack:      object = None        # usdGenUndo.UndoStack(LIMIT=200)
    frameKey:       object = None        # currentFrameChanged connection (2.4)
    activeBrush:    str = ""             # "" = no brush; one of the ids in section 3.1
    brushRadiusPx:  float = 48.0
    brushStrength:  float = 0.5
    brushFalloff:   str = "smooth"
    symmetry:       bool = False
    affectHair:     bool = True          # False = guides only
    context:        str = "interactive"  # "interactive" | "render"
    interactiveMax: int = 0              # 0 = no ceiling; see 2.5
    panels:         dict = dataclasses.field(default_factory=dict)
    viewportFailed: bool = False
```

One instance, created in `__init__`, reachable from every module through the container. It is the
whole of the plugin's mutable state; anything else is derived from the stage or the session and is
re-read, never cached.

### 1.4 `usdGenLib.py` — the ctypes surface

`usdGenLib.py` loads `libusdGenImaging.so` with `ctypes.CDLL(ImagingLibraryPath())`, where the path is
`$USDGEN_IMAGING_DLL`, else `<moduleDir>/../../lib` (installed), else `<repo>/build`
(`research/A3-usdrig-tools.md` §4.1). Before that, and before any module in the package imports numpy,
it does:

```python
import os
for _v in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_v, "1")     # S39: must precede the first "import numpy"
```

MEASURED (`appendix-A-evidence-ledger.md` **`EV-065`**): a `(100000,4) @ (4,4)` matmul cost **0.56 ms**
with BLAS pinned and **22 ms** unpinned on a loaded host — a 39× swing inside a mouse-move budget
(`research/G-tool-loop-array-transport-and-cv-picking.md` §1.5, §2.4). Without it the brush is
unusable whenever the machine is busy.

The C ABI (contract C4, frozen at the end of M5; ADR §3). **Nineteen entry points**: the eighteen ADR
§9.4 R31 names plus `UsdGenImaging_GetLastError()`, which R31's minimum list omits and every caller
needs (`11-roadmap.md` §2.6 counts the eighteen R31 names).

```c
/* usdGenImaging/cApi.h — USDGEN_IMAGING_C_API. Control and scalars only; no array
   ever crosses per element. Every entry point is extern "C", returns int status
   (0 = success, non-zero = error) except the two const char * accessors, and
   never throws across the boundary (ADR §9.4 R31).
   UsdGenImaging_GetLastError() returns the message for the calling thread. */

int         UsdGenImaging_Activate(long long stageCacheId, const char *groomRootPath,
                                   double frame);
int         UsdGenImaging_Deactivate(void);
int         UsdGenImaging_SetTime(double frame);
int         UsdGenImaging_Commit(void);   /* R32 trigger (a): after SetTime and after  */
                                          /* live-override changes ONLY. Never for a   */
                                          /* stage edit — trigger (c) owns those (2.4) */
int         UsdGenImaging_SetContext(const char *context); /* "interactive" | "render" */
long long   UsdGenImaging_GetGeneration(void);             /* publication handshake    */
long long   UsdGenImaging_GetTopologyGeneration(const char *descriptionPath);
                     /* bumps ONLY on a topology change (R31): a recapture, a new      */
                     /* curve set, a density or ceiling change. Never on a deform      */
                     /* frame and never on a live-override republish (3.4, 6.3)        */

int         UsdGenImaging_BeginLiveOverride(const char *primPath);
int         UsdGenImaging_SetLiveOverrideIndexed(const char *primPath, const int *cvIndices,
                                                 int count, const float *xyz);
int         UsdGenImaging_ClearLiveOverride(const char *primPath);

int         UsdGenImaging_PickCV(const char *primPath, const float viewProj[16],
                                 int w, int h, float x, float y, float radiusPx,
                                 int *outCurve, int *outCv, float *outDistPx);
int         UsdGenImaging_Footprint(const char *primPath, const float viewProj[16],
                                    int w, int h, float x, float y, float radiusPx,
                                    int *outIdx, int maxOut, int *outCount);
int         UsdGenImaging_ClosestSurfacePoint(const char *surfacePath, const float p[3],
                                              int *outFace, float outUV[2], float outP[3]);
int         UsdGenImaging_BuildMirrorMap(const char *descriptionPath, int axis);

int         UsdGenImaging_SetInteractiveLOD(const char *descriptionPath, int maxCurves);
int         UsdGenImaging_SetMaskVisualisation(const char *opPath);  /* "" clears (5.5) */
int         UsdGenImaging_ReloadMaps(void);
const char *UsdGenImaging_GetStatsJson(void);
const char *UsdGenImaging_GetLastError(void);
```

`SetLiveOverrideIndexed` appears here **and** in §1.5: R31 requires the array form on the C ABI, and
that is what a non-Python host uses. The usdview tool takes the pxr_boost overload of §1.5 instead
(1.17 µs, MEASURED, `EV-062`) because it needs no element copy on either side. `Footprint` is the R31
name; the prose calls it the CV footprint query.

**This listing is the single source for contract C4** (ADR §9.4 R31). `11-roadmap.md` §2.6 assigns it
here ("the `extern "C"` control ABI … exact signatures in `08-tools.md`"), and `06-imaging.md` §3.8
repeats this listing verbatim; if the two ever diverge, this one governs and §3.8 is edited to match
before the C4 freeze review at the end of M5.

Why the five least obvious entry points exist:

| Entry point | Why |
|---|---|
| `SetInteractiveLOD(descPath, int maxCurves)` | the interactive control is a **curve ceiling**, and it is **session-only** — no authored property (ADR §9.2 R8, §9.4 R36); `0` clears it. The engine converts it to a `keepFraction = maxCurves / curvesAtScale1` and applies the R13 hash rule, so ids, sculpt deltas and clump ids survive |
| `GetTopologyGeneration(descPath)` | the tool must tell "topology changed" from "a generation was published". `GetGeneration()` advances on every publish, deform frames and live-override republishes included; this counter bumps only on a topology change, which is what the mirror map (§3.4) and the selection sets (§6.3) key on |
| `BuildMirrorMap(descPath, int axis)` | ADR §6 makes symmetry an M5 requirement, "not a late add" (§3.4) |
| `SetMaskVisualisation(opPath)` | drives the mask-visualisation mode of §5.5; the empty string clears it |
| `ClosestSurfacePoint(...)` | the stock Hydra pick returns `hitPoint`/`hitNormal`/`hitPrimPath` and **no element index** (`pxr/usdImaging/usdImagingGL/engine.cpp:1286-1288`; `wrapEngine.cpp:218-236`), so face and UV are computed against the rest surface the engine already holds |

`PickCV` carries three out-parameters because the brush needs the curve *and* the CV inside it.
Statistics cross as one JSON blob per publish, not one ctypes call per operator row, so a 40-node
stack costs one call per repaint.

### 1.5 `_usdGen` — the pxr_boost array surface

```c++
// python/_usdGen.cpp  — target _usdGen, a pxr_boost.python MODULE (ADR §6 build;
// the path is 10-build-dependencies-testing.md §1.1's directory layout).
// Arrays only. VtArray crosses in O(1) because it is copy-on-write.
PXR_BOOST_PYTHON_MODULE(_usdGen)
{
    bp::def("ReadCurvePoints",        &UsdGen_ReadCurvePoints);        // (primPath) -> VtVec3fArray
    bp::def("ReadCurveCounts",        &UsdGen_ReadCurveCounts);        //            -> VtIntArray
    bp::def("ReadCurveWidths",        &UsdGen_ReadCurveWidths);        //            -> VtFloatArray
    bp::def("ReadCurveIds",           &UsdGen_ReadCurveIds);           //            -> VtUInt64Array
    bp::def("ReadCurveRootUVs",       &UsdGen_ReadCurveRootUVs);       //            -> VtVec2fArray
    bp::def("ReadCurveRootFrames",    &UsdGen_ReadCurveRootFrames);    //   -> VtMatrix4fArray (f32)
    bp::def("ReadMirrorMap",          &UsdGen_ReadMirrorMap);          // (descPath) -> VtIntArray
    bp::def("SetLiveOverride",        &UsdGen_SetLiveOverride);        // (primPath, VtVec3fArray)
    bp::def("SetLiveOverrideIndexed", &UsdGen_SetLiveOverrideIndexed); // (primPath, VtIntArray, VtVec3fArray)
    bp::def("SetLivePrimvar",         &UsdGen_SetLivePrimvar);         // (primPath, name, VtFloatArray)
    bp::def("ReadFrozenBuffer",       &UsdGen_ReadFrozenBuffer);       // (opPath) -> dict of VtArrays
    bp::def("GetGeneration",          &UsdGen_GetGeneration);
}
```

This list is the other half of C4 and no sibling document declares it. Four entries are additions
beyond the source sketch (`research/G-tool-loop-array-transport-and-cv-picking.md` §1.4 (ii)) and
freeze with the rest of C4 at end of M5: **`ReadCurveRootFrames`** (the comb kernel rotates CVs about
the root frame, a per-curve `GfMatrix4f` the engine already holds, `03-execution-engine.md`; the
freeze widens it to the schema's `matrix4d[]` at author time, §4.1 step 7);
**`ReadMirrorMap`** (the symmetry table of §3.4); **`ReadFrozenBuffer`** (§4.1 needs every published
array of one node at once, so a freeze is one call not eight); **`SetLivePrimvar`** (the paint brushes
and the mask visualisation push a `VtFloatArray`, not points).

MEASURED transport costs (`research/G-tool-loop-array-transport-and-cv-picking.md` §1.2, §1.4;
`appendix-A-evidence-ledger.md` §2.7; best-of-N at load 0.37–0.47 on the GB10 host):

| Operation | 100 k CV | 1 M CV | Row |
|---|---:|---:|:--|
| `VtArray` in or out across pxr_boost (COW) | 0.13–0.14 µs | 0.13–0.18 µs | `EV-055` |
| `np.asarray(vt)` zero-copy read-only view | 0.19 µs | 0.18 µs | `EV-059` |
| sparse indexed push (`VtIntArray` + `VtVec3fArray`, 2 000 CVs) | 1.17 µs | 1.17 µs (measured at 100 k; the push is O(footprint), not O(N) — UNMEASURED at 1 M, gate **T-1**) | `EV-062` |
| `attr.Set(vt)` into a layer | 1.63 µs | 1.82 µs | `EV-058` |
| `Vt.Vec3fArray.FromBuffer(numpy f32)` — **banned** | 420 µs | 4 304 µs | `EV-057` |
| `attr.Set(numpy (N,3) f32)` — **banned** | 480 µs | 4 893 µs | `EV-057` |

Three review-enforceable rules, each backed by one of those rows:

1. **Never** `Vt.*Array.FromBuffer(numpy)` and **never** `attr.Set(ndarray)` on a groom-sized array —
   both run a per-element strided walk (`pxr/base/vt/arrayPyBuffer.cpp:388,427-431`). C++ hands back
   `VtArray`s; Python authors them.
2. numpy is the **kernel** language over `np.asarray(vt)`, never the transport. That buffer is
   read-only by construction (`arrayPyBuffer.cpp:229-235`, `view->readonly = 1` at `:245`) and the
   view survives dropping the Python `Vt` object (MEASURED).
3. The module must `from pxr import Vt, Gf` at import: the Vt converters live in `pxr/Vt/_vt.so`, not
   `libusd_vt.so`, and calling a `VtArray`-typed function first raises `TypeError: No to_python
   (by-value) converter found …VtArray<…GfVec3f>` (MEASURED).

The module is pinned to the USD build (mangled names carry `pxrInternal_v0_26_8__pxrReserved__`,
MEASURED), which is why control lives on the version-tolerant ctypes surface.

### 1.6 numpy kernels

Every kernel in `usdGenBrushMath.py` has the shape `kernel(base_view, idx, params) -> (idx,
VtVec3fArray)`: gather from the read-only press-time view, compute an `(M,3)` float32 result, return
it as a `Vt.Vec3fArray` built by C++ or by `Vt.Vec3fArray(M)` + slice assignment — never by
`FromBuffer`. MEASURED (`EV-061`): gather + displace over a 2 000-CV footprint at 100 k CVs is
**16.6 µs** with numpy and **309 µs** in pure Python, so numpy is a hard dependency of the loop
(`research/G-tool-loop-array-transport-and-cv-picking.md` §1.4).

### 1.7 The CPU CV picker

Screen-space projection of CVs needs no GL. MEASURED, single-threaded C++ over ctypes at 1920×1080
(`research/G-tool-loop-array-transport-and-cv-picking.md` §2.4):

| Query | 100 k CVs | 1 M CVs | Row |
|---|---:|---:|:--|
| `UsdGenImaging_PickCV` (nearest CV within 24 px) | **166 µs** | **1.66 ms** | `EV-064` |
| `UsdGenImaging_Footprint` (within 240 px) | 590 µs | 1.07 ms | `EV-064` |
| the same in numpy, BLAS pinned / unpinned on a loaded host | 1.77 ms / 10.5–29 ms | 17.2 ms / 134–230 ms | `EV-065` |
| one `view.pick()`, for comparison (usdRig baseline) | 1.3–1.42 ms | 1.3–1.42 ms | `EV-064` caveat |

The CPU CV pick at 100 k CVs is ~8× cheaper than a Hydra pick, and it is the only route available:
`UsdImagingGLEngine::TestIntersection` never sets `pickTarget` (`engine.cpp:1243-1249`), copies out no
sub-prim index (`:1286-1288`), and the engine has **no** `GetRenderIndex()` in 26.08 (grep count 0 in
`usdImagingGL/engine.h`), so a plugin-side `HdxPickTask` would need its own render index. Above
~300 k CVs the picker takes the root pre-filter path (nearest root first, then the CV inside that
curve) — the crossover is an **ASSUMPTION** interpolated between `EV-064`'s 166 µs at 100 k and
1.66 ms at 1 M,
and gate **T-2** measures where it really is. The 1 M single-thread figure is the ceiling; TBB is
deferred until T-2 says it is needed (`research/G-tool-loop-array-transport-and-cv-picking.md` §2.4:
at 1 M CVs the picker needs either the root pre-filter or TBB).

---

## 2. The interaction loop

### 2.1 The four phases

Every brush obeys one contract (S40, and usdRig's `gizmoDrag` rule that a drag recomputes from the
press-time base, never incrementally, `research/A3-usdrig-tools.md` §2.2):

* **press** — resolve the camera **once** (`view.resolveCamera()` is side-effecting: it emits
  `signalFrustumChanged`, `pxr/usdImaging/usdviewq/stageView.py:1589,1627`); pick; read the base
  arrays; open an `EditRecorder` bracket; `UsdGenImaging_BeginLiveOverride`.
* **move** — recompute the whole affected set from the press-time base; push a sparse live override;
  `usdviewApi.UpdateViewport()`. **No stage traffic.**
* **release** — close the live override **without republishing**; write once, inside one
  `Sdf.ChangeBlock`, into the current edit target; push the undo entry; `UpdateViewport()`. The cook
  runs inside the repaint, at the end of the `_PrimsDirtied` batch that carries the edit (§2.4); the
  tool calls no `Commit()`.
* **escape** — `recorder.Abort()`, `ClearLiveOverride`, `UpdateViewport()`; no stage write ever
  happened, so the abort is free.

Recomputing from the base is what makes a stroke idempotent and abortable, and why a dropped
mouse-move cannot accumulate error.

### 2.2 One move, costed

```
press   hit  = usdGenPick.SurfacePick(x_phys, y_phys)            # 1.3-1.42 ms once  EV-064
        base = _usdGen.ReadCurvePoints(tilePath); b = np.asarray(base)# 0.34 us  EV-061
        rec  = usdGenUndo.EditRecorder(stage, attrPaths).Begin()
        lib.UsdGenImaging_BeginLiveOverride(tilePath)
move    lib.UsdGenImaging_Footprint(tilePath, vp, w, h, x, y, r, buf, N, cnt) # 0.59 ms EV-064
        idx, disp = usdGenBrushMath.Comb(b, idx, stroke, radius, falloff, frames) # 16.6 us EV-061
        _usdGen.SetLiveOverrideIndexed(tilePath, idx, disp)           # 1.17 us  EV-062
        usdviewApi.UpdateViewport()
release lib.UsdGenImaging_ClearLiveOverride(tilePath)          # close, no republish
        with Sdf.ChangeBlock(): attr.Set(vt)                          # 2.4 us   EV-063
        undoStack.Push(rec.Commit("Comb hair"))
        usdviewApi.UpdateViewport()          # the repaint runs ApplyPendingUpdates();
                                             # the index cooks at the end of the
                                             # _PrimsDirtied batch (section 2.4).
                                             # No Commit() call. No timer.
```

MEASURED (`EV-061`; `research/G-tool-loop-array-transport-and-cv-picking.md` §1.4): the Python side
of one move totals **21.3 µs** best / 21.8 µs median (0.34 µs read + 16.6 µs kernel + 4.1 µs sparse
push over **ctypes**). With the pxr_boost push this design chooses (1.17 µs, `EV-062`) it is 18.1 µs;
21.3 µs is quoted throughout as the conservative figure. Either way it is **0.13 % of a 16.7 ms
frame**. The footprint query is the larger term at 0.59 ms (`EV-064`), which is why it is recomputed
only past a travel threshold (§3.3).

Two terms remain, neither measurable on this host. The engine's live-override patch is
**≤ 0.035 ms, UNMEASURED, gate T-1** — bounded above by the E-2 sparse-edit measurement
(`appendix-A-evidence-ledger.md` **`EV-002`**; MEASURED 0.035–0.044 ms for a 1 % sparse re-run of a
five-node chain, `research/G-data-plane-engine-prototype-benchmark.md` §4), which does strictly more
work than a patch that re-runs no node. Storm's re-upload of one tile is, at 100 k curves and the 49
tiles ADR §9.3 R21's arithmetic gives, ~2 040 curves × 8 CV × 12 B ≈ **0.20 MB** of `points`
(the byte count is arithmetic from R21's tile split; its upload cost is **DERIVED from `EV-022`**,
whose 0.13 ms per MB of `points` at 200 k puts it near **0.03 ms** — a lower bound, because `EV-022`'s
per-MB rate falls with size. Topology and static primvars are not re-uploaded on a
`primvars/points/primvarValue` dirty, §2.3) — UNMEASURED end to end, gate **T-1**, workstation
protocol **§9** (§8.3).

### 2.3 What the live override does inside the session

A live override is a virtual node appended after the terminal (`06-imaging.md`): it owns no buffer,
patches CVs into the publish buffer, and reports the chunks it touched so the dirty router raises one
or two **tiles**, not the whole prim set (ADR §1's S23/S27 amendment as ADR §9.3 R21 fixes the
arithmetic: chunk = 512 curves, `chunksPerTile = max(1, ceil(nChunks/tileTarget))`,
`nTiles = min(nChunks, clamp(ceil(nChunks/chunksPerTile), 32, 256))`). Morton-sorted roots
at capture — an **ASSUMPTION** (`design/proposal-performance.md` §1 item 3), adopted by ADR §4.1 under
the name *surface-major chunk order* — make a chunk a spatial cluster, so a brush footprint is
expected to touch O(1) chunks. UNMEASURED, gate **T-1**.

The dirty set for a move is exactly `primvars/points/primvarValue` plus `extent/*` on the touched
tiles (ADR §5.2). Two MEASURED rules matter to the tool (`EV-034`;
`research/G-tool-loop-array-transport-and-cv-picking.md` §5):

* a `primvars/<name>/primvarValue` dirty costs 0.14–0.24 µs of delegate bookkeeping and does **not**
  rebuild primvar descriptors — the narrowing lives at
  `pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:511-522`;
* a **universal** (empty-locator) dirty is silently a no-op for descriptors, because
  `HdDataSourceLocator().GetFirstElement()` is the empty token
  (`pxr/imaging/hd/dataSourceLocator.cpp:101-109`). Never use one to "just refresh".

When a primvar *appears* — the first stroke of a paint brush, or switching on a falloff overlay — the
session dirties `primvars/<name>` **once**; that does clear the descriptor cache and make the primvar
visible (MEASURED, `EV-034`: 2.06 µs, visible; the universal `{}` dirty is 0.18 µs and **not**
visible). No re-`PrimsAdded` is needed on 26.08.

### 2.4 Release: who cooks, and when

The release write is a normal stage edit, and **the tool never calls `UsdGenImaging_Commit()` for it**.
ADR §9.4 R32 rules that commit trigger **(c) always applies**: an operator, map or surface-topology
dirty commits synchronously at the end of the `_PrimsDirtied` batch that carried it, with or without
an application driver attached. Trigger (a) — an explicit `Commit()` — is for `SetTime` and for
live-override changes only; trigger (b) is `sceneGlobals/currentFrame` when no (a) is attached. There
is no dirty-flag-only mode, no commit timer, and **no `Usd.Notice` listener ordering rule anywhere in
the design**.

The mechanism is why that is the only correct arrangement: a `usdGen:` stage edit does **not** reach
the session inside the USD notice dispatch.

> **Rule.** `UsdImagingStageSceneIndex` does not emit `PrimsDirtied` from the USD notice.
> `_OnUsdObjectsChanged` only fills `_usdPropertiesToUpdate` / `_usdPrimsToResync`
> (`pxr/usdImaging/usdImaging/stageSceneIndex.cpp:511`); the only path that reaches
> `_SendPrimsDirtied` for a property change is `ApplyPendingUpdates()`
> (`stageSceneIndex.cpp:754`, the send at `:792`), which `UsdImagingGLEngine::_PreSetTime` calls at
> `pxr/usdImaging/usdImagingGL/engine.cpp:2372`, itself reached from `_PrepareRender` at
> `engine.cpp:485` — i.e. **inside the repaint**. A commit issued from an `ObjectsChanged` handler
> would run against a still-empty dirty set and publish a generation that does not contain the
> stroke. No ordering of USD listeners can change that, which is why the cook belongs to the index at
> the end of the dirty batch and not to the application.

The release sequence, in order:

1. `UsdGenImaging_ClearLiveOverride(primPath)` — close the override **without republishing**. The
   session keeps the generation already on screen, so nothing flickers between here and step 5.
2. author the edit in **one** `Sdf.ChangeBlock`, into the current edit target (§4.6, §7.2);
3. `undoStack.Push(rec.Commit(label))` — one undo entry per stroke (§7.3);
4. `usdviewApi.UpdateViewport()` — request a repaint. A scene-index dirty does not schedule one by
   itself, so this call is mandatory after every edit and every undo (S43;
   `research/A3-usdrig-tools.md` §4.3 step 5);
5. the repaint runs `ApplyPendingUpdates()`; `UsdImagingStageSceneIndex` emits `PrimsDirtied`; the
   usdGen scene index routes the locators to nodes through `UsdGenDirtyRouter` (ADR §4.1) and commits
   **synchronously at the end of that `_PrimsDirtied` batch**, re-running the affected sub-graph once
   and publishing a generation that contains the stroke. One edit batch, one cook — which is exactly
   what gate **SI-3** counts (§8.2).

Because the override is closed before the edit and the cook lands inside the same repaint, there is
no frame showing the override and the committed edit together, and no frame showing neither. If the
commit is superseded mid-run the previous generation stays on screen and the session stays dirty, so
the next trigger re-runs (ADR §4.2.4); the status line goes red and the stroke is still undoable.

The plugin registers no listener inside the session and no `Usd.Notice` listener for `usdGen:` edits,
so nothing depends on `Tf_NoticeRegistry::_Register` prepending (`research/A3-usdrig-tools.md` §4.3).
The one Qt connection it keeps is `currentFrameChanged` → `UsdGenImaging_SetTime()` with the
**signal's** frame (never `dataModel.currentFrame`, which `rootDataModel.py` assigns after emitting)
→ `UsdGenImaging_Commit()`. That is trigger (a), and with the freeze handshake of §4.1 step 1 it is
the only `Commit()` the tool issues.

### 2.5 Progressive feedback

Four mechanisms — three unconditional, one behind gate S-9 — so nothing an artist waits on is a blank
viewport (ADR §6 tools; artist §8.8):

1. **Interactive ceiling.** `UsdGenImaging_SetInteractiveLOD(descPath, maxCurves)` caps the emitted
   curve count. The ceiling exists **only as session state on the C ABI** (ADR §9.2 R8, §9.4 R36):
   nothing is authored, nothing is read back from the stage, and `maxCurves = 0` clears it. It
   decimates with the same predicate `usdGen:densityScale` uses — `keep iff
   UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32` (ADR §9.2 R13) — with the density salt,
   so the preview set is uncorrelated with `hairId` (`= UsdGenHash32(curveId, 0) / 2^32`, R12) and
   ids, sculpt deltas and clump ids survive. The status line says `LOD 40k`.
   **Sequencing.** Engaging the ceiling changes the published curve set and so invalidates the CV
   indices the footprint query returned and the `idx` array `SetLiveOverrideIndexed` is keyed on. The
   ceiling may therefore only change **between** strokes: it engages when a brush is selected or the camera
   starts moving, and is restored when the brush is deselected or the camera settles — never between
   press and release (element counts stay fixed during a drag, S28, §3.1). A ceiling change while a
   stroke is live aborts it through the escape path of §2.1 and the status line says so; the `D`
   hotkey is ignored while a drag is live, exactly as undo/redo are (§7.4).
2. **Chunk-order progressive publish** over `asyncAllow`/`asyncPoll` (S20), seeded from the camera so
   a heavy regeneration fills in from the middle of the screen outward. The plugin enables the 100 ms
   poll by setting `_allowAsync` during `registerPlugins`; read-verified reachable, because
   `_configurePlugins()` runs at `appController.py:432`, before the async timer at `:522-527` and
   before `self._stageView.allowAsync = self._allowAsync` at `:1868`. Whether the poll then fires
   ~10/s is UNMEASURED — gate **T-5**. Camera seeding depends on `sceneGlobals/primaryCameraPrim`
   being populated at the renderer level in usdview, which is **UNMEASURED**
   (`design/judge-artist.md` open question 13); if it is not, the publish falls back to surface-locality
   order, which costs only the fill-in direction, not the mechanism.
3. **Capture progress.** Long captures report through `UsdGenNodeStats` into the stack profiler row,
   which shows a fraction instead of a time while the capture runs. **The previous generation stays on
   screen until the new one is complete** — there is never a frame with half a groom (ADR §4.2.4).
4. **Tumble tier (conditional).** If gate **S-9** (M0 pre-work **PW-3**, binding at M1) shows a refineLevel
   1↔2 switch costs < 3 ms, the plugin drops tiles to refineLevel 1 while the camera moves and
   restores 2 on release (ADR §5.5, amendment S31). It is a session control on the same footing as the
   ceiling and obeys the same never-during-a-stroke rule. If S-9 fails, LOD is hash decimation only
   and M5 loses this control with no schedule change.

---

## 3. The brush shelf

### 3.1 The ten v1 brushes and their commit targets

Guide mode targets the `UsdGenGuideSet`'s child `BasisCurves`; hair mode targets a
`UsdGenSculptLayer` in the chain, created on first use immediately below the terminal unless the
artist chose another insertion point. `UsdGenToolState.affectHair` picks between them and the status
line names the target prim before the stroke starts.

| # | Brush | Key | Reads | Live override per move | Commits to (release) |
|---|---|:--:|---|---|---|
| 1 | **Comb** | `1` | tile/guide `points`, `ReadCurveRootFrames` | `points` of touched tiles, indexed | guide `points`, **or** `usdGen:sculpt:curveIds` + `usdGen:sculpt:cvOffsets` + `usdGen:sculpt:deltas` on a `UsdGenSculptLayer` |
| 2 | **Grab** | `2` | `points` | same | same as Comb |
| 3 | **Smooth** | `3` (or hold `Shift`) | `points` | same | same as Comb |
| 4 | **Length** | `4` | `points`, `widths` | `points`, arc-length scaled; CV counts fixed | guide `points`, or sculpt deltas; the parameter route is Map paint on channel `length` (row 8) |
| 5 | **Cut** | `5` | `points` | `points`, tip CVs collapsed onto the last kept CV (counts fixed, S28) | sculpt deltas that hold the collapse; a real count change happens at the next `UsdGenLength` edit or re-freeze |
| 6 | **Clump paint** | `6` | `clumpId_<level>`, the resolved mask | `primvars/usdGen:paint:clump/primvarValue` on the surface | `primvars:usdGen:paint:clump` on the mesh, read by a `UsdGenPaintMap` wired into the `UsdGenClump`'s `usdGen:mask:source` |
| 7 | **Density paint** | `7` | the resolved density field | `primvars/usdGen:paint:density/primvarValue` | `primvars:usdGen:paint:density` on the mesh |
| 8 | **Map paint** | `8` | any named channel, scalar or colour | `primvars/usdGen:paint:<name>/primvarValue` | `primvars:usdGen:paint:<name>` on the mesh; optional bake to EXR / `.ptx` afterwards |
| 9 | **Place guide** | `9` | surface hit; the interpolated hair at that root | a preview `points` prim only | a **new** `BasisCurves` under `<Description>/Guides/<setName>/`, contract C3, authored **on release only** |
| 10 | **Lock paint** | `0` | curve ids under the cursor | `displayColor` overlay on touched tiles | `usdGen:sculpt:lockedCurves` (`uint64[]` — curve ids, ADR §9.2 R12) on the active `UsdGenSculptLayer` |

Notes that are not optional:

* **Never create or remove a prim per move.** Overlays are defined once and toggled with `visibility`;
  Place guide defines its prim on release. A prim add is a resync, and usdview then rebuilds its prim
  view at ~9–10 µs per stage prim (MEASURED 2.6 / 23.0 / 102.2 ms at 115 / 2 205 / 11 005 prims,
  `EV-051`; `research/G-freeze-bake-undo-and-frozen-reentry.md` §4.3).
* **Element counts stay fixed during a drag** (S28: padded `points` render fallback red in 26.08).
  Density scrubbing and Cut park culled or trimmed strands as degenerate CVs with zero width and
  commit the real count change on release.
* Comb, Grab and Smooth in hair mode *are* the sculpt brush; there is no separate "sculpt" tool.
* Brush 10 is XGen's **Freeze** brush — the name `02-schema.md` §2.10 uses for
  `usdGen:sculpt:lockedCurves` ("XGen Freeze-brush ids") and `04-operators.md` uses for the same
  gesture. This document calls it **Lock paint** so that "freeze" always means `UsdGenFreeze` (§4);
  the shelf label is "Lock paint (Freeze)" and the property is `usdGen:sculpt:lockedCurves` either
  way.

### 3.2 The operator-mask brushes (scale, frizz, and friends)

"Scale", "noise/frizz", "clump amount" and "length" as *artist actions* are one mechanism: Map paint
(row 8) with a preconfigured channel and wiring. The shelf ships presets that, on first use, create a
`UsdGenPaintMap` under `<Description>/Maps/`, set `usdGen:paint:surface`, `usdGen:paint:primvar` (to
`usdGen:paint:<channel>`) and `usdGen:paint:interpolation`, and retarget the selected operator's
`usdGen:mask:source` at it. That first use is a **structural** edit — a prim `Define` outside the
change block plus two attribute edits, one `SubtreeSnapshot`-backed undo entry (§7.1) — and every
later stroke of the same preset is not.

Nothing is invented per channel. The mask block — `usdGen:mask:source`, `:amount`, `:invert`,
`:range`, `:rangeMode`, `:combine`, `:random`, `:randomSeed`, `:ramp:knots`, `:ramp:interpolation`,
`:rangeMin`, `:rangeMax`, `:effectPosition`, `:falloff`, `:noise:{amount,frequency,gain,bias,seed}`,
`:region` (`02-schema.md` §2.13) — is on every operator through the auto-applied `UsdGenMaskAPI`
(ADR §2.1, §2.3), so one paint brush drives the whole catalogue, which is why the shelf is ten brushes
and not thirty. `02-schema.md` §2.13 is canonical for these names (ADR §9.4 R16): presets author
`usdGen:mask:source` and `usdGen:mask:rangeMode`, never `usdGen:mask:map` or
`usdGen:mask:range:mode`.

**Explicit apply until SI-8.** ADR §2.1 auto-applies `UsdGenMaskAPI` through `plugInfo`
`AutoApplyAPISchemas` **and** requires the tool to apply it explicitly until gate **SI-8** proves
auto-apply works on a codeless derived type (M0 pre-work **PW-5**, binding at M1, UNMEASURED;
`09-performance-and-benchmarks.md` §5.2). Every tool action that authors a mask property therefore
calls `prim.ApplyAPI("UsdGenMaskAPI")` first — the `schemaIdentifier` overload,
`pxr/usd/usd/prim.h:1209`. It is idempotent and is deleted in one commit when SI-8 goes green. Without
it, on a build where auto-apply does not reach a codeless derived type, a paint preset authors mask
properties on a prim whose definition lacks them: no adapter mapping, no locator, no invalidation, and
the operator never sees the paint map.

`UsdGenPart` and its parting-line brush are **v2** (ADR §2.3, §6); the shelf reserves the slot and the
hotkey `-` (§3.5).

### 3.3 Radius, strength, falloff and throttling

Radius is a screen-space value in the state (`brushRadiusPx`, default 48), converted to a world
radius once per press through the camera and the hit point, so falloff is measured on the **rest
surface** (root distance) crossed with `hairT` along the curve — a comb that reaches further at the tip
than at the root is what an artist expects. `brushFalloff` selects one of
`{"linear","smooth","sharp","constant"}`, evaluated in numpy over the gathered footprint; it is the
soft-selection falloff of §6.4 and costs nothing extra, being inside the 16.6 µs kernel (`EV-061`).

`Shift` held = smooth modifier; `Ctrl` held = invert. Neither is a shortcut, so neither collides with
usdview. The footprint query is recomputed only when the cursor has moved more than
`PICK_MOVE_PIXELS = 3` physical pixels, copying usdRig's snap throttle
(`research/A3-usdrig-tools.md` §3.2); the kernel still runs every move, because it is ~35× cheaper than
the query.

Qt events are **logical** pixels; the pick frustum and `computeWindowViewport()` are **physical**.
Every screen coordinate is multiplied by `devicePixelRatioF()` before it reaches the picker, exactly
as usdview's own `mousePressEvent` does (`research/A3-usdrig-tools.md` §3.1).

### 3.4 Symmetry (mirror-X) — an M5 requirement

ADR §6 makes mirror-X an M5 requirement, "not a late add", because it interacts with stable ids, root
UVs and the footprint kernel. The design:

1. On demand, `UsdGenImaging_BuildMirrorMap(descriptionPath, axis)` builds a per-curve partner table
   in C++: reflect each curve's **rest** root across the surface's object-space X plane and take the
   nearest rest root from the kd-tree the capture already builds; `-1` when no partner is within
   tolerance. Cost is one kNN pass over rest roots — the number gate **E-4** measures (≤ 25 ms at
   100 k roots on 8 threads, linear in roots).
2. `_usdGen.ReadMirrorMap(descPath)` hands the table to Python as a `VtIntArray` (O(1), COW).
3. Per move, the kernel gathers the partner curves through the table, mirrors each displacement in the
   **root frame** (negate the frame-local X, so a stroke reflects correctly on a curved scalp rather
   than in world space), and pushes both index sets in one `SetLiveOverrideIndexed`.
4. On release both sets are in the same arrays, hence the same single write and the same undo entry.

The table is invalidated by a topology change, so the tool re-reads it when
`UsdGenImaging_GetTopologyGeneration(descPath)` changes — **not** when `GetGeneration()` advances. The
generation advances on every publish, deform frames and live-override republishes included, so keying
the rebuild on it would run one kNN pass per published frame (gate **E-4**: ≤ 25 ms at 100 k roots).
Steps 2–4 add one gather to the 16.6 µs kernel (`EV-061`), so the ASSUMPTION is that symmetry roughly
doubles the kernel cost and leaves the move under 50 µs — UNMEASURED, gate **T-1** runs with symmetry
on and off.

### 3.5 Hotkeys

`B` brush shelf show/hide · `1`…`0` select brush · `[` / `]` radius down/up · `Shift` smooth modifier ·
`Ctrl` invert modifier · `Shift+F` freeze at frame · `Alt+F` unfreeze · `G` toggle guide display ·
`D` toggle the interactive ceiling (ignored while a drag is live, §2.5) · `M` toggle mask
visualisation · `Ctrl+Z` / `Ctrl+Y` undo/redo · `-` **reserved**, unbound in v1, for the v2 parting
brush (§3.2).

**Deviation from artist §8.7, deliberate:** freeze is `Shift+F`, not `F`. usdview binds `F` to
*Frame Selected* (`pxr/usdImaging/usdviewq/mainWindowUI.ui`, `actionFrame_Selected` → `F`); the other
proposed keys are clear (`B`, `G`, `D`, `M`, `[`, `]` are unbound; `Ctrl+B`, `Ctrl+D` are bbox and HUD
and are not touched).

Tools never claim a mouse press carrying `Alt` or `Meta` (usdview's camera modifiers) and return
`False` with nothing under the cursor, so usdview navigation and picking keep working. Key handling is
installed at application level, because usdview's `AppEventFilter` refocuses the main window on every
mouse move and swallows `Escape` (`research/A3-usdrig-tools.md` §3.3).

---

## 4. Freeze and commit flows

### 4.1 Freeze at a node

Freeze **caps** the chain (ADR §2.3): upstream operators stay authored and are drawn greyed in the
stack editor, and unfreeze is one token. A freeze may only cap a `restSpace` node
(`05-static-curves-and-deformation.md` §5.5 Rule 0; `02-schema.md` §2.9): the freeze bar greys the
action on every node at or below the rest/tail split and says why — a frozen-deformed cache is an
export that re-enters through `UsdGenCurveSource` with `usdGen:useRest = false` (§4.5). The action,
in order:

1. `UsdGenImaging_SetTime(frame)` then `UsdGenImaging_Commit()` — trigger (a), the one place the tool
   does commit (§2.4) — and read `GetGeneration()`: the handshake that the buffer about to be frozen
   is the one on screen.
2. `_usdGen.ReadFrozenBuffer(opPath)` → a dict of `VtArray`s (points, counts, widths, `rest`,
   `skinprim`, `skinprimuv`, `primvars:usdGen:curveId`, optional `rootFrame`).
3. `usdGenUndo.SubtreeSnapshot.Capture(layer, freezePath)` for the `Frozen` subtree **and**
   `usdGenUndo.RelationshipSnapshot.Capture` (§7.3) for every `usdGen:input` / `usdGen:terminal`
   relationship step 7 retargets — before anything is authored. It must be the relationship class:
   `AttributeSnapshot.Capture` resolves its spec through `layer.GetAttributeAtPath`
   (`plugin/rigExecUsdview/rigExecUndo.py:41-43`), which returns `None` for a relationship path, so
   `exists` stays `False` and `Restore()` would **delete** the wiring the undo exists to put back.
4. `UsdGeom.BasisCurves.Define(stage, <Description>/Frozen/<opName>_f<NNN>)` **outside** any change
   block — `Usd*.Define()` inside `Sdf.ChangeBlock` fails with `TF_RUNTIME_ERROR("Failed to define
   UsdPrim <%s>")` at `pxr/usd/usd/stage.cpp:3893` (MEASURED, `EV-066`).
5. `stage.DefinePrim(<Description>/Ops/<opName>_freeze, "UsdGenFreeze")`, also outside the block and
   for the same reason. The freeze is a real operator prim on the chain; it does not spring into
   existence inside the change block.
6. `prim.ApplyAPI("UsdGenCurveAPI")` on the `BasisCurves` (ADR §2.1: the marker on every C3 curve
   set), so `primvars:usdGen:role` and `primvars:usdGen:curveId` have schema fallbacks and appear in
   `UsdPrimDefinition::GetPropertyNames()` (`02-schema.md` §2.16).
7. Inside **one** `Sdf.ChangeBlock`, in this order:
   * on the `BasisCurves`: `attr.Set(vt)` for `points`, `curveVertexCounts`, `widths`, and the C3
     primvar set — `primvars:rest` (`point3f[]`, `vertex`, authored from the **same `VtArray` object**
     as `points`), `primvars:skinprim` (`int[]`, `uniform`), `primvars:skinprimuv` (`texCoord2f[]`,
     `uniform`, never named `st`), `primvars:usdGen:curveId` (**`uint64[]`**, `uniform` — ids are
     64-bit, ADR §9.2 R12), `primvars:usdGen:frozenEpoch` (`string`, **`constant`**),
     `primvars:usdGen:role = "hair"` (`token`, `constant`), and — when step 2's `ReadFrozenBuffer`
     returned `rootFrame` — `primvars:usdGen:rootFrame` (`matrix4d[]`, `uniform`), widened at author
     time from the engine's `GfMatrix4f`; omitting it means `UsdGenDeform` recomputes the frame from
     `skinprim`/`skinprimuv` (`05-static-curves-and-deformation.md` §2, `02-schema.md` §2.16);
   * on the `UsdGenFreeze`: `usdGen:frozen:curves` → the `BasisCurves`, `usdGen:frozen:mode =
     "frozen"`, `usdGen:frozen:epoch`, `usdGen:frozen:tier = "session"`, and `usdGen:input` → the
     capped node;
   * **one retarget per consumer**: every operator whose `usdGen:input` named the capped node, plus
     `usdGen:terminal` on the Description if it named it, now names the freeze. Without this the chain
     never reaches the freeze and the groom is unchanged.
8. One `_resetGUI` (usdview coalesces N freezes in one Python call into one rebuild);
   `undoStack.Push` — one entry holding **both** snapshots of step 3, the `Frozen` subtree and the
   retargeted consumers; `UpdateViewport()`.

MEASURED costs. Authoring a 100 k×8 freeze into the session layer was measured by **three separate
probes**, and they are never quoted as one sweep (`appendix-A-evidence-ledger.md` `EV-042` states the
rule):

| Probe | Cost | Row |
|---|---:|---|
| author only, no file, on a bare stage | **0.4 ms** | `research/G-freeze-bake-undo-and-frozen-reentry.md` §3 (no `EV-` row yet — §11) |
| the same edit in **C++**, 10 k / 100 k / 1 M curves | **0.25 / 0.27 / 0.27 ms** | `EV-041` |
| the same edit through **Python**, 10 k / 100 k curves | **0.51 / 0.52 ms** | `EV-042` (the extra ≈0.25 ms is Python attribute-set overhead, not USD) |

Inside those, the attribute writing itself is **60–86 µs** at 100 k CVs (`EV-066`). The whole
USD→Hydra half is **flat in curve count and under 0.6 ms** at 10 k, 100 k and 1 M curves (`EV-045`);
`primvars:rest` costs **26 bytes** as long as the freeze hands crate the same `VtArray` object it
hands `points` (`EV-048`). What is UNMEASURED is Storm's reaction to the `PrimsRemoved`+`PrimsAdded`
pair — gate **T-4** and workstation protocol **§10** (§8.3). T-4's budget is **≤ 5 ms of authoring**
(MEASURED 3.45 ms at 2 205 stage prims, `EV-050`) **and exactly one** `_resetGUI`; usdview's own
`_resetGUI` costs 35.4 ms on that stage (`EV-051`) and is **excluded from the budget** — the gate
asserts that it happens once, not how long it takes (ADR §9.5 R40;
`research/G-freeze-bake-undo-and-frozen-reentry.md` §4.3).

The epoch string is `usdgen1:sha1:<digest>` (ADR §2.3). It must live in a **constant string primvar**:
relationships and custom non-primvar attributes never reach Hydra and never invalidate, and
`customData` is worse — `GetCustomDataByKey("usdGen:frozenEpoch")` returns `None` because the key
splits on `:` (MEASURED, `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.3).

### 4.2 Unfreeze

One token: `usdGen:frozen:mode = "live"`. The frozen prim stays on the stage (it is the sculpt base and
the artist may want it back), the upstream chain un-greys, and the next commit re-evaluates. The
frozen prim is *not* deleted; deleting it is a separate, explicit "Discard freeze" action that goes
through the `SetActive(false)` path of §7.4.

### 4.3 Freeze to sublayer, freeze to payload

`usdGen:frozen:tier` selects the landing place. Its three values are the tokens `session`,
`sublayer` and `payload` — never numbered, because the `T-` gate ids and the `T0`–`T4` test tiers of
§0.3 already own that letter (ADR §9.1 R1). The freeze bar offers all three because they answer
different questions.

| `usdGen:frozen:tier` | Where | Undo | MEASURED cost (row) | When |
|---|---|---|---|---|
| **`session`** | usdview's session layer (the default edit target) | `SubtreeSnapshot` stash | **0.4 ms** author, no I/O (`research/G-freeze-bake-undo-and-frozen-reentry.md` §3; see §4.1 for why this figure never merges with `EV-041`/`EV-042`) | the interactive comb/sculpt loop |
| **`sublayer`** | `<asset>_groomBake.usdc`, sublayered | `SetActive(False)` on the frozen prims — clean, no diagnostic (`12-…` RK-08); detaching the whole sidecar is a mute, 0.13–0.21 ms flat (`EV-043`), under the §7.4 containment | 10.6 ms author+save, 9.60 MB at 100 k×8 (`EV-046`); reopen 0.4–0.8 ms (`EV-047`); sublayer compose 0.2 ms (`EV-049`) | "commit results to the stage" — the artist's Save |
| **`payload`** | same file, arced by payload on a session `over` | `SetActive(False)` on the `over`, **never** `RemovePrim` | 21.5 ms compose, 1.5 ms first `points.Get()` (`EV-049`) | very heavy grooms; the point is that it is unloadable |

**Undo of one freeze never trips the OpenExec diagnostic; detaching a whole sidecar does, and is
contained.** MEASURED (`research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4, and
`12-risks-decisions-open-questions.md` RK-08 which is canonical for the containment): "mute a
sublayer holding the freeze" and "remove a sublayer path holding the freeze" both **raise** with an
exec system attached, because both leave no prim at the resynced path when the change closes;
`SetActive(False)` does not. So the per-freeze undo at every tier is `SetActive(False)` (§7.4
landmine 1), and only the "detach the sidecar" action mutes, wrapped in the RK-08 containment:

```python
m = Tf.Error.Mark()                       # Tf.Error.Mark, NOT Tf.ErrorMark, which
m.SetMark()                               # does not exist (pxr/base/tf/wrapError.cpp:200,
try:                                      # 212-221; testusdview.py:144 uses this class)
    stage.MuteLayer(sidecar.identifier)
finally:
    errs = [e for e in m.GetErrors()          # e.commentary: wrapDiagnosticBase.cpp:31-34
            if "Applying predicate to invalid prim" not in e.commentary]
    m.Clear()                             # drop the stageData.cpp:361 predicate error
    if errs:                              # re-raise anything that is not it
        raise Tf.ErrorException(*errs)    # varargs, tf/__init__.py:165-171
assert sidecar.identifier in stage.GetMutedLayers()      # post-condition
```

`payload`'s undo is `SetActive(False)` on the session `over`, whose arc is discarded only when the
entry is evicted from the 200-deep stack, off the interactive path (§7.4, S41; ADR §8 rejects
`RemovePrim` in interactive paths).

**Never `.usda`**: 368 ms to save, 40.3 MB (`EV-046`), and **532 ms** for the first `points.Get()`
versus 0.8 ms for `.usdc` — a 660× reopen difference (MEASURED, `EV-047`). **Never bake motion
samples**: 24 point samples cost 230 MB versus 19.2 MB for `points` + `velocities` (`EV-046`), and the
velocity route is what the stock velocity-motion scene index consumes.

Promotion `session` → `sublayer` is a re-export, not a re-freeze: `Sdf.CopySpec` the frozen prims into the sidecar,
add the sublayer, flip `usdGen:frozen:tier`. The epoch is preserved, so every sculpt layer stays
valid.

### 4.4 Sculpt rebase

A `UsdGenSculptLayer` carries `usdGen:sculpt:epoch`. When it no longer matches its input's capture
epoch the stack editor draws a **stale** badge on that row, and the deltas are still applied (they are
keyed by `usdGen:sculpt:curveIds`, so surviving ids still match). "Rebase sculpt" re-matches the
orphaned ids by **nearest root UV**, matching against the `usdGen:sculpt:rootUVs` (`texCoord2f[]`)
and `usdGen:sculpt:rootPrims` (`int[]`) the layer stores beside its deltas for exactly this purpose
(ADR §9.2 R8), and rewrites `curveIds`/`cvOffsets`/`deltas` as **one undoable action**, in the tool
and not in the evaluator (ADR §2.3). Without those two arrays a rebase has no stored UV to match
against. The badge ships in M5; the rebase action is v2 (ADR §6), specified here so the badge has
somewhere to link to.

### 4.5 Import curves

"Import curves" points a `UsdGenCurveSource` at an existing `BasisCurves` (an Alembic-derived cache, a
sim result, another artist's freeze). The tool checks the C3 contract and offers, in one dialog:

* apply `UsdGenCurveAPI` (ADR §2.1) and add the missing C3 primvars with the types of §4.1 step 7
  (`primvars:usdGen:role = "hair"`, `primvars:usdGen:curveId`, `primvars:skinprim` /
  `primvars:skinprimuv` from `UsdGenImaging_ClosestSurfacePoint` against the bound surface);
* **resample to N** — ragged CV counts are supported in v1 but are the slow path (ADR §4.2.2), so the
  dialog shows gate **E-1r**'s measured penalty (threshold ragged ≤ 2× uniform) beside the checkbox
  and inserts a `UsdGenResample` when accepted;
* reject `centripetalCatmullRom` and `bezier` bases with a hard diagnostic (S29).

### 4.6 Commit to a different edit target

usdview points the edit target at the **session layer** on open
(`pxr/usdImaging/usdviewq/appController.py:1283`, `stage.SetEditTarget(stage.GetSessionLayer())`), and
an artist who does not know that loses work. So the status line always ends with
`edit target: <layer display name>` (§5.4, not cosmetic), and the freeze bar carries **"Commit to
layer…"** — session layer, root layer or any existing sublayer, written inside
`Usd.EditContext(stage, targetLayer)`. The undo entry records the layer it wrote to (§7.4).

### 4.7 Guardrails

* The tool **never re-authors the `Frozen` scope's own spec** after creating it. Touching a parent
  scope's `typeName` resyncs the whole subtree: MEASURED 203 `PrimsAdded` over 200 siblings, 1.3–1.5 ms
  of scene-index work and 203 delegate destroy/create cycles (`EV-052`;
  `research/G-freeze-bake-undo-and-frozen-reentry.md` §4.2).
* A brush **never** silently redirects its commit into `Frozen/`. Combing hair commits to a
  `UsdGenSculptLayer`; it edits a frozen prim's `points` only when the artist has explicitly selected
  that frozen curve set as the target, and the status line says so before the stroke begins.
* `__usdGenRender` is Hydra-only and is never authored by anything, tool included (ADR §2.2).
* Freezes are always siblings; the tool never nests a freeze inside another freeze's subtree.

---

## 5. Panels

### 5.1 Inventory

Six surfaces over the modules of §1.2: the **groom panel** (description tree with surface binding,
terminal, curve and tile counts, material; the density pair; the motion block; the context toggle;
"Create hair material"; the freeze bar; the status line), the **stack editor** (§5.3), the **generated
parameter editor** (§5.2), the **brush shelf** (§3), the **map editor** (map prim list, the SeExpr
field with a help pane, "Bake maps", "Reload maps") and **diagnostics** (stats HUD, mask
visualisation, "show driving guides", the commit-phase histogram, the eviction log).

### 5.2 Parameter panels are generated, never hand-written

A new operator gets a UI for free. The panel walks
`UsdPrimDefinition::GetPropertyNames()` (`pxr/usd/usd/primDefinition.h:38`) for the prim's type and
applied API schemas, and maps each property to a widget by its `SdfValueTypeName`, its
`allowedTokens`, and a small table of usdGen conventions:

| Property shape | Widget |
|---|---|
| `float` / `int` / `bool` / `token` with `allowedTokens` | slider / spin / checkbox / combo |
| `float2[] <p>:knots` + `token <p>:interpolation` (`linear \| catmullRom \| bspline \| constant`, default `catmullRom`) | scalar ramp curve widget (ADR §9.2 R11; `02-schema.md` §2.17) |
| `float[] <p>:positions` + `color3f[] <p>:colors` + `<p>:interpolation` | colour ramp widget (R11) |
| `float <p>:spline` authored **with** `.spline`, or a plain animated scalar | "animated" badge; the value is read at the current frame and edited through the graph editor (R11) |
| `rel usdGen:*` | a prim picker filtered by the expected type (`UsdGenMap`, `UsdGenGuideSet`, `Mesh`/`GeomSubset`, operator) |
| `asset usdGen:map:file` | file picker plus a "Reload maps" button |
| `string usdGen:expr:source` | the SeExpr text field, preserving the `#3dpaint,N` and `#min,max` annotation convention |

A `.spline` on `:knots`, `:positions` or `:colors` is a compile error, not a widget (ADR §9.2 R11). Generated panels are
the natural pair to the generic adapter mappings in `06-imaging.md`, which are built from the same
`GetPropertyNames()` call — one definition drives both the data path and the UI, which is why gate
**SI-7** (every `usdGen:*` property of every registered type appears and dirties) also protects the UI.

### 5.3 The stack editor

Rows are the operators reachable from `usdGen:terminal` through `usdGen:input`, in Kahn order with
namespace order as the tie-break (S26). Per row: type icon, `usdGen:label`, an enable checkbox
(`usdGen:enabled`), a mode combo (`usdGen:mode`) **on the types that declare one** — `UsdGenClump`
does not; its blend method is `usdGen:clump:method` (ADR §9.2 R10, `02-schema.md` §2.7) — a blend
slider (`usdGen:blend`), the mask badge, a motion badge on deformed-space rows, and the profiler
column.

* **Drag to reorder rewrites `usdGen:input` only** — one attribute edit per moved node, never
  `reorder nameChildren`: there is no `PrimsChildrenReordered` notice in Hydra and nothing measures
  that a namespace reorder invalidates anything (ADR §2.1). Namespace order is only the Kahn tie-break.
* **Enable is a value edit.** `usdGen:enabled` is non-structural for topology-preserving operators
  (the node stays in the graph as a memcpy pass-through; no recompile, no re-capture) and structural
  for generators, `UsdGenResample` **and `UsdGenLength`** — whose *static* `TopologyEffect()` is
  `MayChangeCurveCount` because cull mode exists (ADR §9.2 R14). The row shows which kind it is. A
  muted generator keeps its capture, so ids survive mute/unmute.
* **Mode switches are value edits**, not prim delete/create — the reason one prim type carries a
  `usdGen:mode` token (ADR §2.1), and why ordinary stack editing never trips the `RemovePrim`
  diagnostic.
* **Add / duplicate / delete** author a prim and rewrite at most two `usdGen:input` relationships,
  inside one `Sdf.ChangeBlock` with `Define` outside it and one `SubtreeSnapshot`-backed undo entry.
* **Profiler column.** One `UsdGenNodeStats` per node (`03-execution-engine.md`), arriving as one
  JSON blob from `UsdGenImaging_GetStatsJson()` per publish:

  ```c++
  struct UsdGenNodeStats {
      TfToken  type; SdfPath path;
      double   captureMs, lastEvalMs, meanEvalMs;
      uint64_t curvesIn, curvesOut, chunksDirty, chunksTotal, bytesOwned;
      uint64_t captureHits, captureMisses;
      uint32_t warnings;
  };
  ```

  The panel keys each row by `path` (nothing else identifies which prim a row belongs to), shows
  `captureMs`, `lastEvalMs` and "% of frame", the cache ratio
  `captureHits / (captureHits + captureMisses)` that the eviction score reads, and `chunksDirty` out
  of `chunksTotal`. An artist who can see that `op03_clumpBig` is 60 % of the frame lowers
  `clump:levels` instead of guessing.
* **Badges.** Stale sculpt (§4.4), frozen (upstream rows grey out), mask, motion (deformed space),
  warning (from `UsdGenNodeStats.warnings`, e.g. "guide angle rejected 42 % of candidates").

### 5.4 The groom and description panels, and the status line

The description panel exposes the artist-facing globals only: `usdGen:densityScale` (the
`interactive` context) and `usdGen:renderDensityScale` (the `render` context) as a labelled **pair**
— the contexts are exclusive and neither scale applies in the other (ADR §9.2 R13) —
the motion block (`usdGen:motion:mode`, `:sampleCount`, `:forwardSurfaceSamples`), `usdGen:tileTarget`,
and the surface binding. Machine tuning (`USDGEN_CHUNK_SIZE`, `USDGEN_THREAD_LIMIT`,
`USDGEN_MEMORY_BUDGET_MB` — the registry is `10-build-dependencies-testing.md` §3.5, ADR §9.4 R35) is
env/config and is **shown read-only** in the diagnostics panel, never
offered as an authorable control (ADR §2.3).

The **context toggle** (`interactive` / `render`) calls `UsdGenImaging_SetContext()`. Context is
explicit — `USDGEN_CONTEXT=render` for batch, the C ABI for apps, this toggle in usdview — because
the renderer display name is not intent and `sceneGlobals` has no interactive flag (ADR §2.3; the
token block is `pxr/imaging/hd/sceneGlobalsSchema.h:37-46` — nine tokens, none of them a context or
interactivity flag; ADR §2.3 writes the range as `:37-47`, which includes the blank line after it). Flipping it changes the curve count, so it is a commit, not a live
override. It is **not** on the undo stack: §7.3's undo restores layer specs and has no mechanism for
session state. The toggle is its own inverse, and the status line always names the live context.

The status line is one row:

```
curves 187 432 · tiles 62 · eval 2.1 ms · gen 41 · LOD 40k · ctx interactive · edit target: session
```

The tile count follows ADR §9.3 R21 and is recomputed, never carried: 187 432 curves at a 512-curve
chunk give `nChunks = 367`; with the default `usdGen:tileTarget = 64`,
`chunksPerTile = max(1, ceil(367/64)) = 6` and
`nTiles = min(367, clamp(ceil(367/6), 32, 256)) = 62`. 64 tiles is the 1 M-curve figure, not this one.

### 5.5 Mask visualisation and "show driving guides"

Both read data the design already computes, and both are ADR §6 additions to the source proposals.

* **Mask visualisation** (`M`). `UsdGenImaging_SetMaskVisualisation(opPath)` turns it on for one
  operator; the empty string turns it off (§1.4). The session then publishes that operator's
  resolved per-curve mask as `displayColor` on the tiles (already cached as a `VtFloatArray` at
  capture, `04-operators.md`). It is a live primvar override: one
  `primvars/displayColor/primvarValue` dirty per change, one `primvars/displayColor` dirty when the
  mode is switched on. **Never co-dirty `displayColor` with `points`** (ADR §5.2, S30). Switching off
  restores the look shader's colour.
* **Show driving guides.** For the curve under the cursor, `GuideInterpolate` already emits
  `guideIndex` and `guideWeight` — uniform `int[]` / `float[]` with `elementSize = 3`, since
  `int[3]` is not a USD type name (ADR §9.3 R24). The overlay writes those three guides'
  `displayColor` in the `__usdGenRender/guides/<setName>` publication and draws the weights in the
  HUD. No new evaluation, no new prim.

### 5.6 CV display and guide editing

CV display is a **synthesized `points` child prim** at `__usdGenRender/guides/<setName>/cvs`
(ADR §2.2) carrying `points`, `widths` and `displayColor`. The alternative — the guide prim's own
`displayStyle:reprSelector` points slot, which works, composites over the collection repr
(`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:3088-3105`; `hd/repr.h` slot 2) and dirties as
`HdChangeTracker::DirtyRepr` (`pxr/imaging/hd/dirtyBitsTranslator.cpp:750`) — is rejected because its
dot size is fixed at 3 px, and dot size matters for a comb tool.

**CV selection state is colour, not Hydra selection.** `HdSelectionSchema`'s entire token set is
`(fullySelected) (nestedInstanceIndices)` (`pxr/imaging/hd/selectionSchema.h:37-38`), there are no
point indices, and `UsdImagingGLEngine` exposes no point-selection API. Selected, hovered and locked
CVs are three `displayColor` values on the `cvs` prim.

Guide editing is the same loop with a different target: the guide set's child `BasisCurves` are real
stage prims, so a comb in guide mode writes their `points` at release and no sculpt layer is involved.
Adding a guide is the Place-guide brush (§3.1 row 9); deleting one goes through `SetActive(false)`
(§7.4).

---

## 6. Picking and selection

### 6.1 Surface pick

The stock path, through a **fresh** `UsdImagingGL.RenderParams` with `showGuides = False`, never
mutating `view._renderParams` and never toggling `viewSettings.displayGuide` (which fires a visibility
signal and a repaint). MEASURED usdRig baseline (`EV-064`'s caveat column): ~1.3 ms per offscreen
`pick()`, 1.42 ms via the bypass, 5.7–18 ms on the first pick after a topology or visibility change
(`research/A3-usdrig-tools.md` §3.2). Called **once per press**, never per move.

usdview's own `pickObject` overwrites the hit point's x/y with mouse coordinates before emitting
`signalPrimSelected`, so `api.selectedPoint` cannot recover the surface point. The tool calls
`computePickFrustum` + `pick` itself from its event filter and gets face and UV from
`UsdGenImaging_ClosestSurfacePoint`, because no element index survives the engine.

Every synthesized prim publishes `primOrigin { scenePath }` — absolute outside prototypes, relative
inside (ADR §5.3). Without it a click selects **nothing**: `HdxPrimOriginInfo::GetFullPath` starts
from a default-constructed `SdfPath` and only assigns from `primOrigin`
(`pxr/imaging/hdx/pickTask.cpp:1298-1317`). Pointing a tile's `primOrigin` at the `UsdGenDescription`
makes a click on the hair select the description. Gates **T-INST-1/2** cover the instancer round trip
and prototype rebasing.

### 6.2 CV pick

`usdGenPick.PickCV(x, y)` → `UsdGenImaging_PickCV`, with the numbers in §1.7. The camera's
view-projection matrix is resolved once per drag and passed in as a `float[16]`.

The CPU route has **no occlusion**: a CV behind the mesh is as pickable as one in front. Two
mitigations are documented, neither in v1: read the depth buffer around the cursor with PyOpenGL
inside `stageView.makeCurrent()` and reject CVs behind it; or the private-render-index `HdxPickTask`
with `pickTarget = pickPoints` (`pxr/imaging/hdx/testenv/testHdxPickTarget.cpp:154-177`). Gate **T-3**
(**T3**, **M5** — ADR §9.5 R40) decides. Pick *accuracy* is a behaviour comparison, not a GPU timing,
so it runs here under Xvfb `:77` like every other T3 test (`research/ENVIRONMENT.md` CORRECTIONS:
`usdview`, `testusdview` and `usdrecord --renderer GL` all work there). If it shows occlusion errors
matter, the depth-buffer filter ships first because it needs no second render index.

### 6.3 Selection sets

`usdGenPick` keeps two sets in the tool, never on the stage: a **curve** set (`VtUInt64Array` of
curve ids) and a **CV** set (flat CV indices plus their curve ids). Both are keyed by the stable
64-bit `primvars:usdGen:curveId` (`uint64[]`, `uniform`; `02-schema.md` §2.16, ADR §9.2 R12), so they
survive a re-cook that keeps ids — which the
hash-threshold decimation rule guarantees for density and ceiling changes (ADR §9.2 R13: the smaller
set is always a subset of the larger). They reach the viewport as `displayColor` on the `cvs` prim
(§5.6). They are cleared when `UsdGenImaging_GetTopologyGeneration()` changes, and are otherwise
preserved across generations: `GetGeneration()` advances on every publish, so clearing on it would
drop the artist's selection on every deform frame.

### 6.4 Soft selection falloff

There is no separate falloff API: the kernel computes `w = falloff(d_surface / radius) · ramp(hairT)`
in numpy over the footprint, inside the measured 16.6 µs (`EV-061`). Locked curves
(`usdGen:sculpt:lockedCurves`, `uint64[]` of curve ids) get `w = 0` before displacement, which is what makes the Lock brush a
real freeze rather than a hint.

### 6.5 Region selection

Rubber-band and lasso both resolve to one `UsdGenImaging_Footprint` call with a radius covering the
region's bounding circle, then a numpy point-in-rectangle or point-in-polygon test over the returned
indices. MEASURED footprint cost is 0.59 ms at 100 k and 1.07 ms at 1 M CVs (`EV-064`) — per gesture,
not per move, so v1 optimises it no further.

---

## 7. Stage writes and the undo model

### 7.1 What each action authors

| Action | Authors | Structural? |
|---|---|---|
| Comb/Grab/Smooth/Length/Cut, guide mode | `points` on the guide `BasisCurves` | no |
| Comb/Grab/Smooth/Length/Cut, hair mode | `usdGen:sculpt:curveIds` (`uint64[]`), `:cvOffsets`, `:deltas` (+ `:epoch`, `:rootPrims`, `:rootUVs` on creation — the rebase keys of §4.4, ADR §9.2 R8) on a `UsdGenSculptLayer` | only the first time (creates the prim) |
| Lock paint | `usdGen:sculpt:lockedCurves` (`uint64[]`) | no |
| Paint preset, **first** use | a `UsdGenPaintMap` under `<Description>/Maps/` (`usdGen:paint:surface`, `:primvar`, `:interpolation`) + the operator's `usdGen:mask:source` retarget | yes (`Define` outside the block, one `SubtreeSnapshot`) |
| Density / Clump / Map paint (the preset having run) | `primvars:usdGen:paint:<name>` on the bound `Mesh` | only on first appearance of the primvar |
| Place guide | a new `BasisCurves` under `<Description>/Guides/<setName>/` | yes |
| Stack edit (add/delete/reorder/mode/enable) | `usdGen:input`, `usdGen:enabled`, `usdGen:mode`, prim `Define`/`SetActive` | reorder/enable/mode: no; add/delete: yes |
| Parameter edit | the operator's own property | no |
| Freeze | a `BasisCurves` under `Frozen/`, a `UsdGenFreeze` under `Ops/`, and one `usdGen:input`/`usdGen:terminal` retarget per consumer (§4.1) | yes (two `Define`s outside the block; one undo entry) |
| Unfreeze | `usdGen:frozen:mode` | no |
| Commit to sublayer/payload | a `.usdc` file + a layer arc | yes |
| Context toggle | nothing on the stage (session state; `UsdGenImaging_SetContext`) — **not undoable** (§5.4) | — |

Every structural row goes in **one** `Sdf.ChangeBlock` with `Define` **outside** it; every
non-structural row is a single `attr.Set(vt)`.

### 7.2 Layers and the edit target

Writes go through the plain `Usd` API into `stage.GetEditTarget()`, except "Commit to layer…", which
wraps the write in `Usd.EditContext`. The edit target is displayed permanently (§4.6, §5.4). An undo
entry records the layer it wrote into and `Restore()` is a no-op when `layer.expired`.

### 7.3 The undo module

`usdGenUndo.py` mirrors usdRig's `rigExecUndo` conventions (S41, `research/A3-usdrig-tools.md` §2.1)
and adds two classes:

```python
class AttributeSnapshot:   # Capture(layer, specPath) / Restore()  - as rigExecUndo
class RelationshipSnapshot:   # NEW: one relationship spec in one layer
    @classmethod
    def Capture(cls, layer, specPath):
        spec = layer.GetRelationshipAtPath(specPath)   # None => exists False
        snap.exists  = spec is not None
        snap.targets = list(spec.targetPathList.explicitItems) if spec else []
    def Restore(self):
        with Sdf.ChangeBlock():
            spec = self.layer.GetRelationshipAtPath(self.specPath)
            if not self.exists:
                if spec is not None:
                    spec.owner.RemoveProperty(spec)
                return
            if spec is None:
                primSpec = Sdf.CreatePrimInLayer(self.layer,
                                                 self.specPath.GetPrimPath())
                spec = Sdf.RelationshipSpec(primSpec, self.specPath.name)
            spec.targetPathList.explicitItems = self.targets
class SubtreeSnapshot:     # NEW: whole prim subtree
    @classmethod
    def Capture(cls, layer, path):
        snap.stash = Sdf.Layer.CreateAnonymous("usdGenUndoStash")
        Sdf.CreatePrimInLayer(snap.stash, path.GetParentPath())
        Sdf.CopySpec(layer, path, snap.stash, path)
    def Restore(self):
        with Sdf.ChangeBlock():
            self._Remove(self.layer, self.path)      # del parentSpec.nameChildren[name]
            if self.exists:
                Sdf.CreatePrimInLayer(self.layer, self.path.GetParentPath())
                Sdf.CopySpec(self.stash, self.path, self.layer, self.path)
class Edit(label, entries)      # ONE Sdf.ChangeBlock around all entries
class UndoStack(LIMIT=200)      # Push clears redo; AddListener(fn)
class EditRecorder(stage, attrPaths)   # Begin() / Commit(label) -> Edit|None / Abort()
```

MEASURED (`EV-040`; `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.2–§1.3): 11/11 fidelity
assertions pass, including time samples, `Ts.Spline`, nested children, relationships, `kind` and
uniform variability; at 100 k curves Capture is **0.10 ms**, undo **0.23–0.24 ms**, redo
**0.08–0.09 ms**, with **zero RSS growth** because `Sdf.CopySpec` shares the `VtArray` buffers. A 200-deep stack therefore costs
nothing for the freezes it holds; the ~1 ms of `free()` moves to eviction time.

`RelationshipSnapshot` exists because `AttributeSnapshot` cannot stand in for it: its `Capture`
resolves the spec with `layer.GetAttributeAtPath` (`plugin/rigExecUsdview/rigExecUndo.py:41-43`),
which returns `None` for a relationship path, leaving `exists` `False` so that `Restore()` deletes the
relationship instead of restoring it (`rigExecUndo.py:73-76`). `usdGen:input` and `usdGen:terminal`
are relationships (ADR §2.1, §2.3), and the freeze of §4.1 retargets them, so an undo built on the
attribute class would destroy the chain wiring. The API is verified:
`SdfLayer::GetRelationshipAtPath` (`pxr/usd/sdf/layer.h:1488`), the `Sdf.RelationshipSpec(ownerPrim,
name)` constructor (`pxr/usd/sdf/wrapRelationshipSpec.cpp:41`) and `targetPathList`
(`wrapRelationshipSpec.cpp:52`).

`SdfLayer` has **no** `RemovePrimSpec` in Python; prim removal is `del parentSpec.nameChildren[name]`,
or `del layer.rootPrims[name]` at the root (MEASURED). Property removal is
`primSpec.RemoveProperty(spec)`, as above.

### 7.4 The two USD landmines, and their containment

1. **Undo of a live freeze is `prim.SetActive(False)`, not removal.** It is 0.02 ms flat, Hydra
   treats it as a `PrimsRemoved` exactly like a real removal, and it does not trip the OpenExec
   diagnostic. The stash-backed real removal is the *purge* path, run when the entry falls off the
   200-deep stack, where the 1.47 ms (100 k) / 13.47 ms (1 M) `free()` is off the interactive path
   (MEASURED, `EV-044`).
2. **`UsdStage::RemovePrim` with an OpenExec system attached raises a spurious
   `Tf.ErrorException`** — stock OpenUSD 26.08: `pxr/exec/esfUsd/stageData.cpp:361` applies
   `UsdPrimDefaultPredicate` to a possibly-invalid prim and `pxr/usd/usd/primFlags.cpp:22-27` posts
   `TF_CODING_ERROR("Applying predicate to invalid prim.")`. It fires exactly when the resynced path
   has no prim at the end of the change (9-case matrix, MEASURED), and it is harmless: the removal
   completes, the next USD call is clean, and N removals in one `Sdf.ChangeBlock` coalesce into one
   exception carrying N errors. **Containment is `12-risks-decisions-open-questions.md` RK-08's**, in
   Python the error-mark class **`Tf.Error.Mark`** — bound at `pxr/base/tf/wrapError.cpp:212-221`
   inside the `Tf.Error` scope opened at `:200`, and used by
   `pxr/usdImaging/bin/testusdview/testusdview.py:144`. The name `Tf.ErrorMark` **does not exist**;
   S41/S46 and `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4 say it is unbound and are
   corrected by RK-08. The sequence is `m = Tf.Error.Mark(); m.SetMark(); …; errs = m.GetErrors();
   m.Clear()`, filtering the `stageData.cpp:361` error by message, re-raising anything else, and
   asserting the post-condition that the prim is gone (the worked block is in §4.3). In C++ it is
   `TfErrorMark` + `Clear()`. Batch removals. **Never rely on `RemovePrim` in an interactive path.**

Two further rules from the same probes: a freeze may only be undone **in the layer it was authored
into** (against usdview's session-layer edit target, `RemovePrim` on a root-layer prim is a silent
no-op — MEASURED), and undo/redo are refused while a drag is live.

### 7.5 What the tool must do after every edit and undo

Call `usdviewApi.UpdateViewport()` (§2.4 step 4). The frame handler is the only exception — usdview
repaints on a frame change itself — and it must use the **signal's** frame (§2.4).

---

## 8. Performance targets and the workstation protocol

### 8.1 The budget

The frame ledger is `09-performance-and-benchmarks.md` §0.2 and only that (ADR §9.5 R41); this
document does not restate it. What it says, and what the tool must live inside: usdGen's own share of
a 100 k × 8 CV deform frame is **0.8–1.0 ms (DERIVED)** inside a **≈ 16 ms (DERIVED)** frame whose
largest term — Storm's draw, ≈ 12.2 ms — is **UNMEASURED until gate S-1**. **60 Hz at 100 k × 8 CV at
refineLevel 2 is not established** (R41; `12-risks-decisions-open-questions.md` A-28 records the same
correction). The tool therefore has no "leftover milliseconds" budget of its own; its budget is gate
**T-1**'s **≤ 1 ms per move**, which is a per-gesture cost added on top of whatever frame the ledger
turns out to describe. The tool's share of one move, at 100 k curves:

| Term | Cost | Status |
|---|---:|---|
| Python kernel + transport | 21.3 µs | MEASURED, `EV-061` (`research/G-tool-loop…` §1.4); 18.1 µs with the pxr_boost push this design chooses (§2.2, `EV-062`) |
| `Footprint` (the CV footprint query), throttled to ≥ 3 px of travel | 0.59 ms | MEASURED, `EV-064` (same report, §2.4) |
| engine live-override patch | ≤ 0.035 ms | **UNMEASURED**, gate **T-1**; bounded above by the E-2 sparse-edit row (`appendix-A-evidence-ledger.md` **`EV-002`**; MEASURED 0.035–0.044 ms for a 1 % sparse re-run of a **five-node chain**, `research/G-data-plane-engine-prototype-benchmark.md` §4), which does strictly more work than a patch that re-runs no node |
| Storm re-sync + re-upload of one tile | ~0.20 MB of `points` | byte count from ADR §9.3 R21's tile arithmetic (§2.2); its upload cost is **DERIVED from `EV-022`** (≈ 0.03 ms, a lower bound) and the end-to-end ms is UNMEASURED, gate **T-1**, workstation protocol **§9** |

The transport is not the cost of the loop. Leaf-exact dirties, tiles instead of one prim, no stage
traffic per move and no prim churn all exist to protect the two terms this host cannot measure.

### 8.2 Gates owned by this document

`09-performance-and-benchmarks.md` §5 is the single gate registry; the assignments below are ADR
§9.5 R40's, which supersedes any sibling that disagrees. The tier decides which suite runs a gate and
whether it may be a milestone exit at all (ADR §7, R39: tier-4 gates are **release** criteria, never
milestone exits). Test names are `10-build-dependencies-testing.md` §5.6's CTest registry, which is
canonical for them; `09-…` §5.4 prints the same names.

| Gate | Tier | Milestone | Assertion | Threshold |
|---|:--:|:--:|---|---|
| **T-1** brush move | T3 | M5 | press / move×N / release at 100 k curves, symmetry off and on, in `testUsdviewUsdGenComb.py` (symmetry's own assertions in `testUsdviewUsdGenSymmetry.py`) | ≤ 1 ms Python + engine per move |
| **T-2** pick | **T1** | M5 | `UsdGenImaging_PickCV` at 100 k / 1 M CVs in `testUsdGenPick` | ≤ 0.25 ms / ≤ 2.5 ms |
| **T-3** pick accuracy | **T3** | **M5** | CPU pick vs `view.pick()` at 100 pixels under Xvfb `:77` (`testUsdviewUsdGenPick.py`) | disagreements only where a CV is occluded |
| **T-4** freeze | T3 | M5 | one freeze on a 2 205-prim stage (`testUsdviewUsdGenFreeze.py`) | ≤ 5 ms of **authoring** (MEASURED 3.45 ms, `EV-050`) **and exactly one** `_resetGUI` — usdview's own 35.4 ms `_resetGUI` (`EV-051`) is excluded from the budget; the gate asserts it happens once, not how long it takes (ADR §9.5 R40) |
| **T-5** async | T3 | M8 | the plugin sets `_allowAsync` without the CLI flag (`testUsdviewUsdGenAsync.py`) | 1 `asyncAllow`, ~10 `asyncPoll`/s |
| **T-INST-1** | **T1** | M6 | instancer pick round trip in `testUsdGenInstancerPick`; the app-level round trip is the unlabelled T3 script `testUsdviewUsdGenCards.py` | click selects the description |
| **T-INST-2** | **T1** | M6 | prototype rebasing in `testUsdGenInstancer` | paths resolve; `instancedBy` has exactly one target |
| **SI-3** cook count | T1 | M1 | 10 interactive edit batches, with and without an app driver attached | exactly one commit per batch, all on the commit thread |
| **E-4** kNN capture | **T0** | **M0 pre-work PW-1**, binding at M3 | rest-root kNN at 100 k / 1 M roots, 8 threads | ≤ 25 ms at 100 k and linear — the number symmetry (§3.4) and Place-guide assume |
| **R-2** MSAA / OIT quality | T4 | release | 1-px strands, A2C vs OIT at 1080p and 4K | visual sign-off |

M5's exit set is therefore **T-1, T-2, T-3, T-4** (plus S-7, which
`09-performance-and-benchmarks.md` §5.5 owns). R-2 is a release criterion; T-INST-1/2 exit M6;
T-5 exits M8.

### 8.3 The workstation protocol (release criteria)

None of the **tool-loop** GPU numbers can be taken on this host. Storm itself runs headlessly and
GPU-accelerated here through the EGL device-platform harness, and real GPU frame times were measured
that way (`research/ENVIRONMENT.md`, CORRECTIONS) — but that harness has no Qt event loop and
therefore no usdview session (§9.4); `usdrecord` core-dumps outside Xvfb because it opens a GLX
window; and the Xvfb `:77` route, where `usdview`, `testusdview` and `usdrecord --renderer GL` all do
run, is llvmpipe — correct behaviour, CPU numbers only. **Behaviour** gates therefore run on `:77`
(T-1, T-3, T-4, T-5); only the GPU **timings** below need a workstation.

The workstation protocol is **one document with one numbering**: `docs/workstation-protocol.md`,
whose sections `10-build-dependencies-testing.md` §5.2 owns as **§§1–11**: §§1–8 (1 usdview
sign-off, 2 interactive frame time, 3 MSAA/alpha-to-coverage, 4 non-NVIDIA drivers, 5 Metal/Vulkan
under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, 6 Ptex, 7 hdPrman parity, 8 4K interactive) and the
three **tool-loop** checks specified below — **§9 the stroke, §10 the freeze, §11 the pick**. There is
no separate `W-` id scheme; the earlier drafts' `W1`–`W4` are retired (the fourth was never a new
check: tool-side *quality* is protocol §3 = gate R-2 and protocol §5 = gate R-3, both already
written).

| Protocol | Closes | Note |
|---|---|---|
| **§9** the stroke | the end-to-end half of gate **T-1** | the components are MEASURED (§8.1); only the presented-frame term is not |
| **§10** the freeze | the GPU half of gate **T-4** | the author half is MEASURED 3.45 ms (`appendix-A-evidence-ledger.md` **`EV-050`**; `research/G-freeze-bake-undo-and-frozen-reentry.md` §4.3) and runs at T3 |
| **§11** the pick | pick probe **P1** (and **P3** if needed) around gate **T-3** | T-3 itself is T3/M5 and runs on `:77`; §11 only confirms P1 on real hardware and, if T-3 fails on occlusion, prototypes P3 |
| §3 and §5 (already written) | gates **R-2** and **R-3** | visual sign-off and a shader compile; nothing tool-specific to add |

**§9 — the stroke.** `testusdview --renderer GL --viewportSize 1920 1080` on 10 k / 100 k / 1 M-curve
grooms; drive a synthetic 60-move stroke; record wall time from `SetLiveOverrideIndexed` to the
presented frame with `HD_ENABLE_PERFLOG=1` and a `Trace` scope around `HdStBasisCurves::Sync`, plus
the static frame time of the same scene. **Pass (UNMEASURED; the absolute number can only be set once
S-1 is re-measured at 100 k): the median stroke-to-present time exceeds the static frame time of the
same scene by ≤ 2.0 ms with one tile dirty.** It is a delta, not an absolute, because the measurement
necessarily contains one Storm draw, and the plan's own frame ledger puts a 100 k × 8 refineLevel-2
draw at ~12.2 ms, **DERIVED from `EV-020`/`EV-021`** (interpolated between MEASURED 5.01 ms at 40 k
and 23.93 ms at 200 k) — a number ADR §7 orders re-measured under gate **S-1**
(`09-performance-and-benchmarks.md` §0.2, §2.5). The shape matches gates S-2 (a deform frame ≤ 2.0 ms
over static) and S-3 (a one-tile edit ≤ 1.2× static). It closes the evaluator-republish +
Storm-re-sync term the 21 µs Python loop hands off to.

**§10 — the freeze.** `prototypes/freeze-bake/uv/testUsdviewFreezeCost.py` on 115 / 2 205 / 11 005-prim
stages with 10 k / 100 k / 1 M-curve freezes (it already prints `author_ms`, `resetGUI_only_ms`, the
per-panel split, `undoByDeactivate_ms`, `redoByActivate_ms`, `undoByRemovePrim_ms`; rename the redraw
key to `gpuRedraw_ms`). Record ms from `SetActive(True)` to the first presented frame, peak GPU memory
delta at 1 M curves, and — the load-bearing one — whether a `primvars/points` dirty avoids the topology
re-upload.

**§11 — the pick.** The pick probes of `research/G-tool-loop-array-transport-and-cv-picking.md` §2.5
(the report's own `P1`/`P2`/`P3`, unrelated to the motion profiles P0/P1/P2 of ADR §9.1 R1). **P1**
confirms on real hardware that `DRAW_POINTS` changes nothing a stock pick returns. **P2** is gate
**T-3**, and it is *not* a workstation item: it is a behaviour comparison, not a GPU timing, so it
runs at tier T3 under Xvfb `:77` as an M5 exit (ADR §9.5 R40). **P3**, the private-render-index
fallback, is prototyped here only if T-3 shows occlusion errors matter.

---

## 9. Testing

### 9.1 Tier T0 — Qt-free unit tests, no Hydra, no stage

Plain scripts in `tests/python/`, usdRig's shape: `_Check(cond, msg)`, `(name, callable)` groups,
`  ok: <name>` lines and a final `USDGEN_<X>_OK (<n> groups)` banner, exit 0
(`research/A3-usdrig-tools.md` §5.2). The first executable lines are
`import usdgen_test_env; usdgen_test_env.SetupPluginTest()` before `from pxr import …`, and the env
module globs `lib/python*/site-packages` rather than hard-coding a version (the usdRig `_env.sh` bug,
S46).

| Script | Asserts |
|---|---|
| `test_usdgen_brushmath.py` | kernels are pure functions of (base, idx, params); a stroke applied twice from the base is idempotent; falloff endpoints exact; locked curves get zero displacement; mirroring negates root-frame X |
| `test_usdgen_undo.py` | `SubtreeSnapshot` fidelity (the 11 assertions of `research/G-freeze-bake…` §1.2); **a retargeted `usdGen:input` is restored, not deleted, by one undo** (`RelationshipSnapshot`, §7.3); `EditRecorder` commits only changed attributes; one change block per `Edit`; `Restore()` no-ops on an expired layer |
| `test_usdgen_graphmodel.py` | Kahn order matches the C++ compiler on 20 generated graphs; cycles rejected; a drag rewrites exactly one `usdGen:input` |
| `test_usdgen_freeze_author.py` | both `Define`s outside the change block (§4.1 steps 4–5); `UsdGenCurveAPI` applied; the full C3 primvar set with the interpolations of §4.1 step 7; `frozenEpoch` is a constant string primvar with the `usdgen1:sha1:` prefix; every consumer of the capped node retargeted in the same `Edit` |
| `test_usdgen_mask_author.py` | every code path that authors a `usdGen:mask:*` property calls `prim.ApplyAPI("UsdGenMaskAPI")` first (the explicit apply required until gate **SI-8**, §3.2); the paint preset authors `usdGen:mask:source`, never `usdGen:mask:map` |
| `test_usdgen_toolstate.py` | the container imports with **no** Qt available; `UsdGenToolState` fully constructed in `__init__`; `usdGenLib` sets the BLAS vars before numpy is importable |
| `test_usdgen_transport.py` | `Vt.*Array.FromBuffer` and `attr.Set(ndarray)` appear nowhere in the package (source grep); `np.asarray(vt)` is read-only and survives dropping the `Vt` object |

Gate **T-2** (`UsdGenImaging_PickCV` timing) is **not** in this Python suite at all: it is
`testUsdGenPick`, a tier-**T1** test (ADR §9.5 R40; `09-performance-and-benchmarks.md` §5.4;
`10-build-dependencies-testing.md` §5.6), because the picker is reached through the C ABI on a live
session.

### 9.2 Tier T1 — headless scene index

`test_usdgen_liveoverride.py` (C++ or Python over the real `UsdImagingCreateSceneIndices` chain):
a live override dirties exactly `primvars/points/primvarValue` + `extent/*` on the touched tiles and
nothing else (gate **SI-2**); a paint primvar appearing dirties `primvars/<name>` once and
`primvars/<name>/primvarValue` per move (MEASURED mechanism,
`hd/sceneIndexAdapterSceneDelegate.cpp:511-522`); ten simulated interactive edit batches produce
exactly ten commits, all on the commit thread (gate **SI-3**, counted with and without an app driver
attached — ADR §9.4 R32 makes trigger (c) unconditional). Gate **T-2** (`testUsdGenPick`) and gates
**T-INST-1** (`testUsdGenInstancerPick`) / **T-INST-2** (`testUsdGenInstancer`) are also tier T1
(ADR §9.5 R40, and `09-performance-and-benchmarks.md` §5.4 records them there).

### 9.3 Tier T3 — `testusdview`

One runner per suite, `bin/run_testusdview_usdgen_<x>.sh`, each printing a `USDGEN_<FEATURE>_OK` banner
and optionally saving `$USDGEN_<FEATURE>_SHOT`. Every script (named `testUsdviewUsdGen<X>.py`) defines
`testUsdviewInputFunction(appController)`, asserts on the **stage** and on the terminal scene index —
selected by the `"[Terminal SI]"` **prefix**, never `names[-1]`, because the name registry is an
`unordered_map` (`pxr/imaging/hd/sceneIndex.h:298-302`) — and drives synthetic input in the tool's own
units (logical pixels for `QMouseEvent`, `QTest.keyPress` for held keys).

The script names are `10-build-dependencies-testing.md` §5.6's CTest registry, which is canonical for
them: the eleven scripts **`testUsdviewUsdGen{Activate, Stack, Comb, Symmetry, Paint, Freeze, Pick,
Cards, Undo, Async, Look}.py`**, one runner each. `09-…` §5.4 uses the same names.

| Script | Asserts |
|---|---|
| `testUsdviewUsdGenActivate.py` | activation; the generation counter advances per frame; tile prims in the terminal SI; `primOrigin` resolves to the `UsdGenDescription`; the release sequence of §2.4 — the tool calls no `Commit()` for the stage edit and posts no timer, and the generation advances during the repaint that ran `ApplyPendingUpdates()` |
| `testUsdviewUsdGenStack.py` | the generated parameter panel's rows equal `UsdPrimDefinition::GetPropertyNames()`; stack add / reorder (`usdGen:input` rewritten, `nameChildren` untouched) / disable; a mode switch emits no `PrimsRemoved`; a `UsdGenClump` row shows no mode combo (ADR §9.2 R10) |
| `testUsdviewUsdGenComb.py` | comb: press / move×N / release; **exactly one** stage write; authored values equal the terminal SI's `primvars/points`; Escape writes nothing; **gate T-1** |
| `testUsdviewUsdGenSymmetry.py` | two mirrored sets move; one undo restores both; the mirror map survives a density change and is re-read only when `GetTopologyGeneration()` changes; T-1 re-run with symmetry on |
| `testUsdviewUsdGenPaint.py` | the primvar appears with exactly one `primvars/<name>` dirty; per-move dirties are `primvarValue` only; the count changes on release only; every mask-authoring path called `ApplyAPI("UsdGenMaskAPI")` first |
| `testUsdviewUsdGenFreeze.py` | freeze at frame N; authored `points` == terminal-SI `primvars/points`; upstream greyed; unfreeze by token; undo via `SetActive(False)` under the `Tf.Error.Mark` containment (§7.4, `12-…` RK-08); **gate T-4** |
| `testUsdviewUsdGenUndo.py` | Ctrl+Z / Ctrl+Y after every action class of §7.1; a retargeted `usdGen:input` comes back; undo/redo refused while a drag is live; a freeze undone only in the layer it was authored into |
| `testUsdviewUsdGenPick.py` | **gate T-3**: CPU pick vs `view.pick()` over 100 pixels, disagreeing only where a CV is occluded. Pick probe P1 of §8.3 (a `DRAW_POINTS` pick returns no CV index) runs here too |
| `testUsdviewUsdGenCards.py` | the instancer round trip in the app: instancer present, prototypes re-rooted, a click selects the description. The labelled **T-INST-1** driver is the T1 `testUsdGenInstancerPick`; this script is its unlabelled app-level companion (`10-…` §6.5) |
| `testUsdviewUsdGenAsync.py` | **gate T-5**: `_allowAsync` set from `registerPlugins`; one `asyncAllow`, ~10 `asyncPoll`/s |
| `testUsdviewUsdGenLook.py` | the bound terminal per renderer name; a framebuffer pixel-fraction check against a golden (`07-look-maps-expressions.md`) |

### 9.4 Image checks

Tier T2 (the EGL harness) carries no tool tests — it has no Qt event loop — so the Storm-side terms
the loop depends on come from tier T4 (§8.3). Image checks are T3: capture with
`stageView.updateGL(); QApplication.processEvents(); stageView.grabFrameBuffer()`,
`viewSettings.showHUD = False`, and compare a subsampled pixel set as a **fraction** of the frame, so
the assertion is HiDPI- and size-independent. Every image check is mutation-verified: break the
feature once and confirm the check fails (usdRig's overlay test requires > 0.5 % of pixels changed;
`research/A3-usdrig-tools.md` §5.1). Under the Xvfb fallback these are llvmpipe renders, correctness
checks only — **never** quoted as Storm GPU numbers.

### 9.5 What proves this document

§1 → `test_usdgen_toolstate.py`, `test_usdgen_transport.py` (T0) plus the install-tree run of the T3
suite. §2 → `testUsdviewUsdGenComb.py`, `testUsdviewUsdGenActivate.py`, `test_usdgen_liveoverride.py`,
gates **T-1** (T3), **SI-2/SI-3** (T1). §3 → `test_usdgen_brushmath.py`, `test_usdgen_mask_author.py`
(the explicit `UsdGenMaskAPI` apply, until **SI-8**), `testUsdviewUsdGenComb.py`,
`testUsdviewUsdGenSymmetry.py`, `testUsdviewUsdGenPaint.py`.
§4 → `test_usdgen_freeze_author.py`, `testUsdviewUsdGenFreeze.py`, gate **T-4** (T3), workstation
protocol **§10** (T4). §5 → `test_usdgen_graphmodel.py`, `testUsdviewUsdGenStack.py`, plus a T1 test
that the generated panel's property list equals `UsdPrimDefinition::GetPropertyNames()` for every
registered type (shares gate **SI-7**).
§6 → gate **T-2** (T1, `testUsdGenPick`), gate **T-3** (T3, `testUsdviewUsdGenPick.py`), gates
**T-INST-1/2** (T1, `testUsdGenInstancerPick` / `testUsdGenInstancer`, with
`testUsdviewUsdGenCards.py` as the app-level companion). §7 → `test_usdgen_undo.py` and
`testUsdviewUsdGenUndo.py`. §8 → gates **T-1** (T3), **T-3** (T3), **T-4** (T3), **T-5** (T3),
**R-2/R-3** (T4), and workstation protocol **§§9–11, §3, §5** (T4).

---

## 10. Out of scope

* **Other hosts.** Maya, Houdini, Katana, Nuke, Solaris. The C ABI and `_usdGen` are host-agnostic on
  purpose, but a second host is a v3 conversation.
* **GPU brushes.** Every kernel is CPU numpy over a zero-copy view; a compute-shader brush needs the
  evaluator's buffers on the GPU, which is the v3 "GPU tail" (ADR §6).
* **Occlusion-correct CV picking in v1** — the private render index plus `HdxPickTask` is the
  documented fallback (§6.2; pick probe P3 of `research/G-tool-loop-array-transport-and-cv-picking.md`
  §2.5), taken only if gate T-3 fails on occlusion at M5.
* **A third-party operator ABI and out-of-tree brush plugins**; `UsdGenOpRegistry` is internal in
  v1/v2 (ADR §3).
* **Painting image files during interaction.** Brushes write primvars; "Bake maps" produces EXR or
  `.ptx` afterwards (`07-look-maps-expressions.md`).
* **A parting-line brush**, `UsdGenPart`, an `UsdGenExprOp` authoring UI, TsSpline ramp widgets and
  sculpt rebase — all v2 (ADR §6).
* **Playback-time grooming.** The loop assumes a parked frame; scrubbing during a stroke aborts it.

---

## 11. Sources

**Sibling registries this document depends on, re-checked 2026-09-05.**
`10-build-dependencies-testing.md` §5.2 numbers `docs/workstation-protocol.md` as **§§1–11**, §§9–11
being §8.3's tool-loop checks, and `09-performance-and-benchmarks.md` §4.3 prints the same numbering
(one scheme, no `W-` ids anywhere); `10-…` §5.6 registers all eleven `testusdview` scripts of §9.3,
`testUsdviewUsdGenUndo.py` included, and 09 §5.4 uses those names; `appendix-A-evidence-ledger.md`
§2.6 row **`EV-089`** is the 0.4 ms author-only, no-file freeze probe §4.1 and §4.3 quote, distinct
from `EV-041`/`EV-042`; `11-roadmap.md` §2.6 points at the panels of §5 and counts the **nineteen**
entry points of §1.4 (ADR §9.4 R31's eighteen plus `UsdGenImaging_GetLastError()`).

**ADR and brief.** `design/adr-v1.md` §1 (S23/S27, S24, S29 amendments), §2.1–2.3 (types, reserved
layout, freeze/sculpt/density properties), §3 (C1–C5), §4.5 (sessions), §5.2–5.3 (invalidation, tile
contract), §6 (tools, build), §7 (milestones, gates), §8 (document map), and the §9 addendum
throughout: **R1** (vocabulary, retired landing-tier numbers, `PW-n`, `EV-nnn`), **R2** (test tiers
and gate families), **R5** (the `usdgen` package), **R6/R8** (namespaced properties; the session-only
ceiling and the sculpt rebase keys), **R10** (`UsdGenClump` has no `usdGen:mode`), **R11** (ramp
encodings behind the generated panels), **R12/R13** (64-bit ids, the decimation predicate),
**R14** (`UsdGenLength` is structural), **R21** (tile arithmetic), **R24** (`guideIndex`/`guideWeight`
as `elementSize = 3`), **R31** (this document owns contract C4), **R32** (commit triggers — trigger
(c) always applies), **R35** (`USDGEN_IMAGING_DLL` is registered in `10-…` §3.5, not here), **R36**
(hotkeys, the session-only ceiling), **R38/R39** (v1 = M0–M7; T4 gates are release criteria),
**R40** (gate tiers and milestones), **R41** (`09-…` §0.2 is the only frame ledger), **R42** (number
tags), **R45** (cross-references resolve). `design/brief-v1.md` §1 (R1–R9), §2.8 (S39–S43),
§2.9 (S45–S46 — S41/S46's "`TfErrorMark` is unbound in Python" is corrected by
`12-risks-decisions-open-questions.md` RK-08, §7.4).

**Proposals.** `design/proposal-artist.md` §8.1–8.9 (primary source: panels, the ten brushes, the
live-override path, undo, freeze/bake, hotkeys, progressive feedback, testusdview), §3.4, §3.6, §4.10,
§5.7. `design/proposal-risk.md` §8.1–8.6 (module layout, the two Python surfaces, the USD landmines,
panels, tests), §3.2–3.4. `design/proposal-performance.md` §10.1–10.3 (C ABI, panels, brushes), §6.2,
§6.5, §11.2 (gates).

**Judges.** `design/judge-artist.md` (the missing-symmetry and missing-mask-visualisation findings,
the `usdGen:active` collision, freeze-caps-the-chain, and open question 13 — `_allowAsync` and
`sceneGlobals/primaryCameraPrim` reachability, §2.5). `design/judge-evidence.md` §D6 and §3 (grafts).
`design/judge-delivery.md` §2 (contract freezes), §3 (session identity).

**Research.** `research/A3-usdrig-tools.md` §1–§8 (container and lazy Qt, the drag protocol, StageView
and picking, the C surface, test patterns and env traps, the spec/plan conventions).
`research/G-tool-loop-array-transport-and-cv-picking.md` §1 (transport), §2 (CV picking and its
protocol), §3 (CV display), §4 (`primOrigin`, terminal-SI selection), §5 (live primvar paint).
`research/G-freeze-bake-undo-and-frozen-reentry.md` §1–§5 (`SubtreeSnapshot`, the OpenExec bug and its
9-case matrix, the edit-target trap, landing tiers, freeze cost and blast radius).
`research/G-data-plane-engine-prototype-benchmark.md` §4; `research/G-storm-throughput-and-prim-granularity.md`
§3; `research/ENVIRONMENT.md` with its CORRECTIONS block.

**Siblings this document defers to.** `appendix-A-evidence-ledger.md` §2.1, §2.3, §2.6, §2.7 (every
`EV-nnn` row cited here); `09-performance-and-benchmarks.md` §0.2 (the only frame ledger) and §5 (the
only gate registry); `10-build-dependencies-testing.md` §3.5 (the env-var registry), §5.2
(`docs/workstation-protocol.md` and its numbering) and §5.6 (the CTest name registry);
`02-schema.md` §2.10, §2.13, §2.16 (sculpt, mask block, `UsdGenCurveAPI`);
`12-risks-decisions-open-questions.md` RK-08 (the `Tf.Error.Mark` containment).

**Prototypes carried into the repo.** `prototypes/tool-loop/` (`cTransport.cpp`, `bpTransport.cpp`,
`bench_transport.py`, `bench_stroke.py`, `bench_pick_cpp.py`, `probePrimvarAppear2.cpp`);
`prototypes/freeze-bake/` (`probe1_freeze_undo.py`, `probe8_undo_fidelity.py`,
`probe9_error_containment.py`, `probe10_resync_blast.cpp`, `uv/testUsdviewFreezeCost.py`,
`uv/makeStage.py`).

**OpenUSD 26.08 file:line index** (every one re-verified by reading `/home/burkard/work/OpenUSD/pxr/…`
while writing this document): `usdImaging/usdviewq/plugin.py:113,121,129,333,342`;
`usdImaging/usdviewq/appController.py:381,432,522-527,1283,1863,1868`;
`usdImaging/usdImaging/stageSceneIndex.cpp:511,754,792`;
`usdImaging/usdviewq/stageView.py:1589,1627`; `usdImaging/usdviewq/mainWindowUI.ui`
(`actionFrame_Selected` → `F`); `usd/usd/primDefinition.h:38`; `usd/usd/stage.cpp:3893`;
`usd/usd/primFlags.cpp:22-27`; `exec/esfUsd/stageData.cpp:361` (S41 and both proposals cite `:360`,
which is the last comment line — corrected here);
`base/tf/wrapError.cpp:200` (the `Tf.Error` scope), `:212-221` (`Tf.Error.Mark` with `SetMark`,
`IsClean`, `Clear`, `GetErrors`, `RaiseIfNotClean`); `base/tf/wrapDiagnosticBase.cpp:31-34`
(`TfError.commentary`); `base/tf/__init__.py:165-171` (`Tf.ErrorException(*args)`);
`usdImaging/bin/testusdview/testusdview.py:144` (`errorMark = Tf.Error.Mark()`, the harness pattern);
`imaging/hd/sceneGlobalsSchema.h:37-46` (the nine `sceneGlobals` tokens — no interactivity flag);
`base/vt/arrayPyBuffer.cpp:229-235,245,388,427-431`; `imaging/hd/selectionSchema.h:37-38`;
`imaging/hd/sceneIndexAdapterSceneDelegate.cpp:511-522,3088-3105`;
`imaging/hd/dataSourceLocator.cpp:101-109`; `imaging/hd/dirtyBitsTranslator.cpp:750`;
`imaging/hd/sceneIndex.h:298-302`; `imaging/hd/repr.h`; `imaging/hdx/pickTask.cpp:1298-1317`;
`imaging/hdx/testenv/testHdxPickTarget.cpp:154-177`;
`usdImaging/usdImagingGL/engine.cpp:485,1243-1249,1286-1288,2372`;
`usdImaging/usdImagingGL/engine.h` (no `GetRenderIndex`);
`usdImaging/usdImagingGL/wrapEngine.cpp:218-236`; `usd/usd/prim.h:1209` (the `schemaIdentifier`
`ApplyAPI` overload); `usd/sdf/layer.h:1488` (`GetRelationshipAtPath`);
`usd/sdf/wrapRelationshipSpec.cpp:41,52`; `usd/sdf/wrapPrimSpec.cpp:452`. In the usdRig tree:
`plugin/rigExecUsdview/rigExecUndo.py:41-43,73-76` (why `AttributeSnapshot` cannot capture a
relationship, §7.3).
