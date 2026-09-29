# G — Tool loop: array transport, CV picking, CV highlight, path mapping, live primvar paint

Gap key: `tool-loop-array-transport-and-cv-picking`. Answers the mechanics A3 §7
leaves open. Everything below is either **measured on this host** (commands and
probe sources in the appendix), **read in the source** with `file:line`, or
explicitly marked UNMEASURED with a workstation protocol.

Abbrev: `USD` = `<openusd-src>` (tag v26.08), `INST` =
`$USD`, `RIG` = `<usdrig-src>`,
`P` = `<session-scratch>`.

Measurement conditions: aarch64, 20 CPUs, python 3.12.3 (`$VENV`),
`PYTHONPATH=$INST/lib/python3.12/site-packages`. All numbers reported as
**best-of-N** with the median beside them; the runs quoted in the tables were
taken at `load average: 0.37–0.47` (`uptime` before/after each). An earlier
pass at load 43–59 inflated every BLAS-touching number by 10–20x — see §1.6,
this is itself a finding.

---

## 1. (a) Array transport between the Python tool and the C++ evaluator

### 1.1 The venv now has numpy and pybind11 (installed, verified)

```
$ $VENV/bin/pip install numpy pybind11
Successfully installed numpy-2.5.2 pybind11-3.1.0
```
`numpy 2.5.2` (cp312 aarch64 manylinux wheel), `pybind11 3.1.0`. Both import
alongside `pxr` with no conflict. numpy's build config reports gcc 14.2.1, and
its BLAS is thread-parallel by default (§1.6).

Three transport routes were built out-of-tree against the install and measured:

| route | what it is | build |
|---|---|---|
| **ctypes** | `ctypes.CDLL` on a C ABI (`float*`, `int`), the shape `RIG/libs/rigExecImaging/registry.h` already uses | `g++ -O3 -shared cTransport.cpp` |
| **pybind11** | `PYBIND11_MODULE`, `py::array_t<float>` in/out | needs `pybind11.get_include()` + `-lpython3.12` |
| **pxr_boost.python** | a USD-style module (`PXR_BOOST_PYTHON_MODULE`) taking/returning `VtVec3fArray` | `-I$INST/include -lusd_vt -lusd_gf -lusd_tf -lusd_boost -lpython3.12` |

All three built and ran first try (`P/cTransport.cpp`, `P/pbTransport.cpp`,
`P/bpTransport.cpp`). `pxr_boost::python` is reachable from an out-of-tree
plugin: the header is installed (`$INST/include/pxr/external/boost/python.hpp`)
and `libusd_boost.so` + `libusd_python.so` are in `$INST/lib`; `pxr.h:59` defines
`PXR_USE_INTERNAL_BOOST_PYTHON`, `pxr.h:48` `PXR_PYTHON_SUPPORT_ENABLED`.

### 1.2 Measured per-call cost (best-of-N µs; median in parens)

100 000 CVs = 300 000 floats = 1.20 MB; 266 667 CVs = **800 001 floats = 3.20 MB**
(the size the task named); 1 000 000 CVs = 12.0 MB.

| route (push: Python → C++) | 100k CV | 800k float | 1M CV |
|---|---|---|---|
| reference: `np.copy` of the same buffer | 15.0 (17.6) | 47.3 (51.0) | 362 (400) |
| ctypes `Noop` (call overhead only) | 0.24 | 0.24 | 0.27 |
| pybind11 `Noop` / pxr_boost `Noop` | 0.08 / 0.10 | 0.08 / 0.10 | 0.08 / 0.10 |
| ctypes numpy `.ctypes.data_as` + C memcpy | 16.1 (17.4) | 45.2 (46.2) | 339 (352) |
| ctypes cached `float*` + C memcpy | **13.7 (14.0)** | **42.3 (43.1)** | **341 (347)** |
| ctypes `array.array` `from_buffer` + C memcpy | 14.7 | 46.1 | 362 |
| ctypes Vt → `np.frombuffer(memoryview)` → ptr + memcpy | 15.1 | 45.6 | 360 |
| ctypes Vt → `from_buffer_copy` (python-side copy) + memcpy | 69.6 (411) | 739 (1103) | 4236 (4308) |
| pybind11 `py::array_t<float>` + C memcpy | 15.0 (16.2) | 44.7 (50.0) | 360 (401) |
| pybind11 `py::array_t<float>`, nothing copied | 0.19 | 0.21 | 0.21 |
| pybind11 float64 numpy in (forcecast) + memcpy | 93.6 | 240 | 1793 |
| pybind11 `py::object` → `bp::extract<VtVec3fArray&>` (COW) | **0.13** | **0.16** | **0.16** |
| **pxr_boost `VtVec3fArray` in, COW store** | **0.14 (0.16)** | **0.16 (0.16)** | **0.14 (0.18)** |
| pxr_boost `VtVec3fArray` in, memcpy into a float store | 13.5 | 42.2 | 357 |
| pxr_boost python **list of tuples** in (auto sequence convert) | 18 024 | 46 722 | 177 236 |

| route (pull: C++ → Python) | 100k CV | 800k float | 1M CV |
|---|---|---|---|
| ctypes `ReadCurvesPtr` → `np.ctypeslib.as_array` (zero copy) | 1.62 | 1.66 | 1.68 |
| ctypes `ReadCurvesCopy` into a preallocated numpy buffer | 14.9 | 47.0 | 353 |
| pybind11 numpy view over C storage (capsule) | 0.19 | 0.19 | 0.19 |
| pybind11 numpy copy out | 15.2 | 46.6 | 383 |
| **pxr_boost `VtVec3fArray` out, COW** | **0.13** | **0.13** | **0.13** |
| pxr_boost `VtVec3fArray` out, real element copy | 41.5 | 132 | 676 |
| pybind11 **list-of-lists** out (`RIG/python/_rigexec.cpp:165-172` route) | 9 304 (14 837) | 27 034 (41 542) | 109 251 (164 350) |

| conversion / stage leg | 100k CV | 800k float | 1M CV |
|---|---|---|---|
| `np.asarray(Vt.Vec3fArray)` — zero copy, **read-only** | 0.19 | 0.19 | 0.18 |
| `memoryview(Vt.Vec3fArray)` alone | 0.13 | 0.13 | 0.14 |
| `Vt.Vec3fArray.FromBuffer(numpy f32)` | 420 | 1107 | 4304 |
| `Vt.Vec3fArray.FromBuffer(numpy f64)` | 453 | 1218 | 5038 |
| `Vt.Vec3fArray(list of tuples)` | 10 865 | 28 302 | 107 611 |
| `list(Vt.Vec3fArray)` | 10 992 | 31 334 | 132 488 |
| **`attr.Set(Vt.Vec3fArray)`** into a layer | **1.63 (1.89)** | 1.71 | **1.82** |
| `attr.Set` inside `Sdf.ChangeBlock` | 2.21 | 2.27 | — |
| `attr.Set(numpy (N,3) f32)` directly | 480 | 1249 | 4893 |
| `attr.Get() -> Vt.Vec3fArray` | 0.75 | 0.75 | 0.77 |
| `attr.Set(vt, time=1.0)` (time sample) | 2.83 | 2.85 | 2.93 |

Sparse (brush-footprint) updates, 2 000 CVs of a 100k-CV guide set:
ctypes indexed **3.73 µs**, pxr_boost `VtIntArray`+`VtVec3fArray` indexed
**1.17 µs**.

### 1.3 What the numbers mean

1. **`VtArray` across the pxr_boost boundary is O(1), not O(N).** 0.13–0.18 µs
   at every size, in both directions, because `VtArray` is copy-on-write: the
   converter copies a refcounted handle. Any route that touches the elements
   (memcpy, `FromBuffer`, list building) is O(N).
2. **Every `float*` route is memcpy-bound and equal.** ctypes, pybind11-numpy
   and pxr_boost+memcpy all land within 10 % of `np.copy` (13.5–16 µs at 1.2 MB;
   ~350 µs at 12 MB). The choice among them is therefore about *ergonomics and
   safety*, not speed. ctypes' fixed overhead is 0.24 µs/call, pybind11's 0.08,
   pxr_boost's 0.10 — all irrelevant at these sizes.
3. **`Vt.Vec3fArray.FromBuffer` is the trap.** It is 25–30x slower than memcpy
   (420 µs for 1.2 MB) because `USD/pxr/base/vt/arrayPyBuffer.cpp:388, 427-431` walks
   a strided index and calls a per-element convert function. So *never build a
   Vt array from numpy in Python*; have C++ hand back a `VtVec3fArray` (0.13 µs)
   and author that.
4. **Authoring a 1M-CV array to a layer costs 1.8 µs** when the value is already
   a `Vt` array (`attr.Set(vt)`), and 4.9 ms when it is a numpy array (USD then
   runs the same `Vt_CastPyObjToArray`/`FromBuffer` walk,
   `arrayPyBuffer.cpp:447-456`). A 2700x difference on the release write.
5. **`Vt` arrays expose the buffer protocol read-only and in O(1)**
   (`arrayPyBuffer.cpp:229-235`: "We don't support writable buffers … guaranteed
   O(1) buffer requests"; `view->readonly = 1` at line 245; shape `[N,3]`,
   strides `[12,4]`, itemsize 4 from `Vt_ArrayBufferWrapper`, lines 168-199).
   Measured: `np.asarray(vt)` shares memory (`True`), `flags.writeable False`,
   `dtype float32`, `shape (N,3)`, 0.19 µs. The wrapper holds its own COW copy
   and `Py_INCREF`s the exporter (line 266), so **the numpy view stays valid
   after the Python `Vt` object is dropped** — probe asserts this
   (`P/bench_stroke.py`, "zero-copy view survives dropping the Vt python
   object: True").
6. **Python lists are disqualified everywhere**, including usdRig's own pybind
   route (`RIG/python/_rigexec.cpp:165-172` → list of lists: 9.3 ms at 100k CVs,
   109 ms at 1M); list *inputs* cost 18–177 ms.

### 1.4 Recommended transport (the deliverable)

**Two surfaces, not one.** Keep the ctypes C ABI for control, add a pxr_boost
module for arrays.

*(i) C ABI over ctypes — control plane only, no arrays crossing per element.*
Extends `RIG/libs/rigExecImaging/registry.h:147-202`'s pattern:

```c
int       UsdGenImaging_Activate(long long stageCacheId, const char* rootPath, double frame);
int       UsdGenImaging_SetTime(double frame);
void      UsdGenImaging_Deactivate(void);
long long UsdGenImaging_GetGeneration(void);          /* publication handshake */
int       UsdGenImaging_BeginLiveOverride(const char* primPath);
int       UsdGenImaging_ClearLiveOverride(const char* primPath);
/* screen-space CV queries, computed in C++ (see §2.4) */
int       UsdGenImaging_PickCV(const char* primPath, const float viewProj[16],
                               int w, int h, float x, float y, float radiusPx,
                               int* outIndex, float* outDistPx);
int       UsdGenImaging_FootprintCV(const char* primPath, const float viewProj[16],
                                    int w, int h, float x, float y, float radiusPx,
                                    int* outIdx, int maxOut, int* outCount);
```
ctypes is kept because it needs no build-time Python dependency, is what usdview
already loads (`RIG/plugin/rigExecUsdview/rigExecUsdview.py:62-85`), and survives
a Python/USD ABI change. Everything it carries is scalars.

*(ii) `_usdgenImaging` — a pxr_boost.python module — for arrays.*

```c++
PXR_BOOST_PYTHON_MODULE(_usdgenImaging)
{
    bp::def("ReadCurvePoints",  &ReadCurvePoints);   // (path)          -> VtVec3fArray  0.13 us
    bp::def("ReadCurveCounts",  &ReadCurveCounts);   // (path)          -> VtIntArray
    bp::def("ReadCurveWidths",  &ReadCurveWidths);   // (path)          -> VtFloatArray
    bp::def("SetLiveOverride",  &SetLiveOverride);   // (path, VtVec3fArray)            0.14 us
    bp::def("SetLiveOverrideIndexed", &SetLiveOverrideIndexed); // (path, VtIntArray, VtVec3fArray) 1.2 us/2k
    bp::def("GetGeneration",    &GetGeneration);
}
```
Why pxr_boost and not pybind11 for this: the values the tool moves are exactly
the values USD authors (`points`, `widths`, `curveVertexCounts`,
`primvars:*`), so `VtArray` is the natural currency; the conversion is free in
both directions; and the release write is then `attr.Set(vt)` at 1.8 µs instead
of `FromBuffer` at 4.3 ms. A pybind11 module can do the same thing — the probe
proves `bp::extract<VtVec3fArray&>` works *inside* a pybind11 function
(`P/pbTransport.cpp:SetPointsVtViaBoost`, 0.13–0.16 µs) — so if usdGen wants
one pybind11 module for everything, it can, at the cost of linking both
`libusd_boost` and pybind11.

**numpy is still worth having** — but as the *kernel* language, not the
transport: the brush maths runs on `np.asarray(vt_points)` (a zero-copy
read-only view) and writes a small `(M,3)` result. Measured kernel over a
2 000-CV footprint: **16.6 µs with numpy vs 309 µs in pure Python** (19x).
numpy is a hard requirement for any per-move maths at 100k+ CVs.

*Per-move loop with this transport (measured, `P/bench_stroke.py`, 100k CVs,
2 000-CV footprint):*

| step | best | median |
|---|---|---|
| press: `ReadCurvePoints` + `np.asarray` (zero copy) | 0.34 µs | 0.37 µs |
| move: numpy gather + displace over the footprint | 16.6 µs | 16.9 µs |
| move: sparse push of the footprint (ctypes) | 4.1 µs | 4.3 µs |
| **move total (Python side)** | **21.3 µs** | **21.8 µs** |
| release: C++ `VtArray` → `attr.Set` in one `ChangeBlock` | 2.4 µs | 2.7 µs |
| (bad alternative) release via numpy → `Vt.FromBuffer` → `Set` | 483 µs | 488 µs |

21 µs of a 16.7 ms frame — 0.13 % of the budget. The per-move cost of the
grooming loop is therefore **not** in the transport; it is in the evaluator
republish and Storm's re-sync, which this host cannot measure.

### 1.5 Gotchas that must be in the plan

| gotcha | evidence |
|---|---|
| A pxr_boost module's Vt converters are registered by `pxr/Vt/_vt.so`, not by `libusd_vt.so`. Calling `ReadCurvePoints` before `from pxr import Vt` raises `TypeError: No to_python (by-value) converter found for C++ type: …VtArray<…GfVec3f>`. | measured, `P` smoke test; the plugin module must `from pxr import Vt, Gf` at import |
| `memoryview(Vt…)` is **read-only**; `ctypes.(c_float*n).from_buffer` on it raises, and `from_buffer_copy` costs 70–4200 µs. Use `np.frombuffer(memoryview(a), dtype=np.float32)` (0.19 µs) when a pointer is needed. | `arrayPyBuffer.cpp:229-235`; measured |
| A pxr_boost module is pinned to the USD build (mangled names carry `pxrInternal_v0_26_8__pxrReserved__`). Rebuild per USD version; keep the ctypes surface as the version-tolerant one. | measured error text above |
| `Usd*.Define()` **cannot run inside `Sdf.ChangeBlock`**: `Error … UsdStage::_DefinePrim at line 3893 … 'Failed to define UsdPrim </Frozen>'`. Freeze must define the prim outside the block and author attributes inside. | measured, `P/bench_stroke.py`; matches `RIG/plugin/rigExecUsdview/gizmoMath.py:1336-1345` |
| numpy's BLAS is multi-threaded and destroys latency under load: a `(100k,4)@(4,4)` matmul measured **22 ms** at load 50 and **0.56 ms** with `OPENBLAS_NUM_THREADS=1`. The plugin must pin BLAS threads to 1 at import. | measured, §1.6 |

### 1.6 Freeze/authoring costs (measured, 100k CVs)

| operation | best | median |
|---|---|---|
| `UsdGeom.BasisCurves.Define` + 5 attrs, no ChangeBlock | 70.8 µs | 86.0 µs |
| Define outside + 3 array attrs inside one ChangeBlock | 63.5 µs | 82.1 µs |
| pure `Sdf` API (`CreatePrimInLayer` + `AttributeSpec`) entirely inside one ChangeBlock | 60.0 µs | 77.1 µs |
| `layer.Export()` of the frozen layer to `.usdc` (1.20 MB file) | 1.22 ms | 1.28 ms |

A freeze of a 100k-CV groom is ~0.08 ms of authoring plus one resync. The pure
`Sdf` route is the only one that puts prim creation *and* attributes in a single
change block, which matters because the evaluator recompiles on resync.

---

## 2. (b) CV picking

### 2.1 What `UsdImagingGLEngine` exposes (26.08, read)

`USD/pxr/usdImaging/usdImagingGL/engine.cpp:1209-1296` — `TestIntersection`
fills `HdxPickTaskContextParams` with `resolveMode`, `viewMatrix`,
`projectionMatrix`, `clipPlanes`, `collection`, `outHits` (1243-1249) and
**never sets `pickTarget`**, so the default `pickPrimsAndInstances`
(`USD/pxr/imaging/hdx/pickTask.h:276`) applies. It copies out only
`worldSpaceHitPoint`, `worldSpaceHitNormal`, `instanceIndex` (1286-1288).
The Python wrapping matches: `wrapEngine.cpp:218-236` exposes `PickParams`
with **only** `resolveMode`, and `IntersectionResult` with `hitPoint`,
`hitNormal`, `hitPrimPath`, `hitInstancerPath`, `hitInstanceIndex`,
`instancerContext` — **no `pointIndex`**. Verified live:
`UsdImagingGL.Engine.PickParams` → `['resolveMode']`.

### 2.2 Does `pickTarget=pickPoints` / `DRAW_POINTS` give a CV index?

Piece by piece:

* `HdxPickResult::_ResolveHit` fills `hit->elementIndex/edgeIndex/pointIndex`
  from the id buffers **unconditionally**, whatever the pickTarget
  (`hdx/pickTask.cpp:1042-1044`). `_IsValidHit` only *requires* a pointId when
  `pickTarget == pickPoints` (1373-1389).
* The pointId AOV is written only when the draw item's shader carries the
  pointId mixins, and that is gated on the primitive type being points:
  `hdSt/basisCurvesShaderKey.cpp:189` `isPrimTypePoints`, VS mixins at 208-215,
  FS at 504-508.
* A basisCurves rprim becomes point-typed when its repr desc geomStyle is
  `HdBasisCurvesGeomStylePoints` (`hdSt/basisCurves.cpp:306-311`), which also
  switches the draw item to `PointsTopology` built by
  `GetPointsIndexBuilderComputation` (`hdSt/basisCurves.cpp:464-468, 725-727`).
* `HdBasisCurves::ConfigureRepr(HdReprTokens->points, HdBasisCurvesGeomStylePoints)`
  is registered by default (`hd/renderIndex.cpp:1066-1067`).
* From Python, `UsdImagingGLRenderParams.drawMode = DRAW_POINTS` makes
  `_UpdateHydraCollection` select `HdReprSelector(HdReprTokens->points)`
  (`engine.cpp:2409-2412`) for **both** the render and the intersect collection.
  Verified importable: `UsdImagingGL.DrawMode.DRAW_POINTS` exists and
  `RenderParams().drawMode` is settable.

**Conclusion:** with `DRAW_POINTS` the pick pass *does* render per-CV point ids
and `HdxPickHit::pointIndex` *is* populated — but `UsdImagingGLEngine` drops it
before Python sees it. So the CV index is unreachable through the public engine
API in 26.08, exactly as A3 suspected, and no Python-side flag changes that.

### 2.3 Can a C++ helper run `HdxPickTask` on usdview's engine? — No

`UsdImagingGLEngine` has **no `GetRenderIndex()`** in 26.08: `grep -c
GetRenderIndex engine.h` → **0** (A3's open question assumed one existed; it
does not). `_taskControllerSceneIndex`, `_renderer`, `_intersectCollection` are
`protected` (`engine.h:810-836`) and `_GetTerminalSceneIndex()` /
`_GetLegacyRenderControl()` are `private` (`engine.h:844-847`). usdview
constructs the engine itself (`UVQ/stageView.py` `_getRenderer`), so subclassing
is not available either.

The standalone pattern *does* exist and is the fallback: a private render index
plus `HdxPickTask` driven by `HdEngine::Execute`, exactly as
`hdx/testenv/testHdxPickAndHighlight.cpp:157-180` and
`hdx/testenv/testHdxPickTarget.cpp:154-177` do
(`p.pickTarget = pickTarget; engine->SetTaskContextData(HdxPickTokens->pickParams, …);
engine->Execute(&renderIndex, &tasks)`), with
`HdxUnitTestUtils::TranslateHitsToSelection` turning `pickPoints` hits into
`HdSelection::AddPoints` (`hdx/unitTestUtils.cpp:134-146`). Doing that inside
usdview means a **second** `HdRenderIndex` + `HdStRenderDelegate` populated with
only the guide curves, executed on usdview's GL context — feasible, but it
duplicates Storm resources for the guides and must be kept in sync with the
main engine's camera and framing. Recommend it only as a fallback (§2.5).

### 2.4 The CPU route, measured (recommended)

Screen-space projection of guide CVs needs no GL at all. Measured on this host
(`P/bench_pick_cpp.py`, `P/bench_pick_cpu.py`; 1920×1080; nearest CV within a
24-px radius; footprint within 240 px):

| implementation | 100k CVs | 1M CVs |
|---|---|---|
| **C++ over ctypes, single thread — nearest CV** | **166 µs** (167) | **1.66 ms** (1.67) |
| **C++ over ctypes — footprint (41 560 / 65 536 hits)** | **590 µs** | **1.07 ms** |
| numpy, BLAS pinned to 1 thread — project all | 1.04 ms | 11.1 ms |
| numpy — nearest CV (project + argmin) | 1.77 ms | 17.2 ms |
| numpy — nearest with a **cached** projection (camera static) | 42 µs | 565 µs |
| numpy — nearest guide **root** only (N/10 points) | 116 µs | 1.11 ms |
| pure Python loop (no numpy) | 98 ms | 98 ms (sampled) |
| numpy, BLAS **not** pinned, host at load 50 | 10.5–29 ms | 134–230 ms |

So: **do the projection in C++** (`UsdGenImaging_PickCV` / `_FootprintCV` in
§1.4). At 100k CVs it fits in 1 % of a frame; at 1M CVs it needs either the
root-prefilter (pick the guide root first, then the CV within that curve) or
TBB — the loop is embarrassingly parallel. For comparison, usdRig's Hydra pick
costs 1.3–1.42 ms per call and 5.7–18 ms after a topology change
(`RIG/docs/superpowers/specs/2026-09-03-gizmo-snapping-design.md:129-173`), i.e.
**the C++ CPU CV pick at 100k CVs is ~8x cheaper than one `view.pick()`**.

What the CPU route does not give is occlusion: a CV behind the mesh is as
pickable as one in front. Mitigations (both UNMEASURED): read the depth buffer
around the cursor with PyOpenGL (`glReadPixels(..., GL_DEPTH_COMPONENT, GL_FLOAT)`
inside `stageView.makeCurrent()`) and reject CVs behind it; or accept it, as
usdRig does for snap candidates (`RIG/plugin/rigExecUsdview/gizmoSnap.py:212-298`).

### 2.5 Workstation protocol (UNMEASURED here — no display)

**P1 — does `DRAW_POINTS` change what a stock pick returns?** A `testusdview`
script (`bin/run_testusdview_usdgen_pick.sh`) on a 10k-curve groom: (1) `params = UsdImagingGL.RenderParams(); params.drawMode
= UsdImagingGL.DrawMode.DRAW_POINTS; params.showGuides = False` (fresh params,
never mutate `view._renderParams` — `RIG/plugin/rigExecUsdview/gizmoUI.py:2456-2542`);
(2) `frustum = view.computePickFrustum(x_phys, y_phys)`;
(3) `view._getRenderer().TestIntersection(pickParams, frustum.ComputeViewMatrix(),
frustum.ComputeProjectionMatrix(), stage.GetPseudoRoot(), params)`.
Expected: a hit only when the cursor is within ~1.5 px of a CV (point size
defaults to 3.0 — `hdx/renderSetupTask.h:133`, `hdx/pickTask.h:285`), and
`hitPrimPath` = the curves prim, with **no** CV index. Record wall time of the
call (compare against the 1.3–1.42 ms baseline) and the hit rate over a 21×21
pixel sweep. This confirms §2.2 in the app, and measures the cost of the
"points-repr pick" if it is ever wanted for prim-level hit-through.

**P2 — CPU CV pick accuracy against Hydra.** With the same camera, call
`UsdGenImaging_PickCV` and `view.pick()` at 100 random pixels over the groom;
assert the CPU pick's chosen curve equals `hitPrimPath` whenever the Hydra pick
hits a guide, and log the disagreements (they should all be occlusion cases).
Record `UsdGenImaging_PickCV` wall time in the app (expect ≈ the 166 µs / 100k
measured headlessly, plus `resolveCamera()` — call it once per drag, not per
move: `stageView.py:1589-1628` emits `signalFrustumChanged`).

**P3 — fallback only if P2 fails on occlusion:** private-render-index
`HdxPickTask` with `pickTarget = pickPoints`, built after
`testHdxPickTarget.cpp:154-177`; assert `selState->pointIndices[0]` is non-empty
for a click on a CV.

---

## 3. (c) A per-prim `points` repr + `HdSelection::AddPoints` for CV display

**Drawing CVs without a second prim: yes, from the scene index.**
`HdSceneIndexAdapterSceneDelegate::GetReprSelector` reads
`displayStyle:reprSelector` (a `VtArray<TfToken>` resized to
`HdReprSelector::MAX_TOPOLOGY_REPRS`) and returns `HdReprSelector(ar[0],ar[1],ar[2])`
(`hd/sceneIndexAdapterSceneDelegate.cpp:3089-3108`). `HdRprim::UpdateReprSelector`
caches it on `DirtyRepr` (`hd/rprim.cpp:157-161`), and
`_GetResolvedReprSelector` composites the **prim's opinion over the
collection's** unless `forceColRepr` (`hd/renderIndex.cpp:1287-1298`;
`HdReprSelector::CompositeOver` keeps the prim's non-empty slots,
`hd/repr.h:77-84`). Slot 2 is the points slot (`hd/repr.h:19-30`). So a guide
prim published with `displayStyle:reprSelector = ["", "", "points"]` draws its
normal repr **plus** its CVs, and every other prim in the viewport is
unaffected. The dirty bit is right: `displayStyle/reprSelector` →
`HdChangeTracker::DirtyRepr` (`hd/dirtyBitsTranslator.cpp:747-751`).

**Highlighting *specific* CVs: no, not through usdview's engine.**
`HdSelection::AddPoints(mode, path, VtIntArray, GfVec4f color)` exists and works
(`hdx/testenv/testHdxPickTarget.cpp:470-493` sets per-point colours), and
`hdx/unitTestUtils.cpp:134-146` is the pickPoints→AddPoints translation. But the
selection an application can express through Hydra 2.0 is
`HdSelectionSchema`, whose entire token set is `(fullySelected)
(nestedInstanceIndices)` (`hd/selectionSchema.h:36-41`) — **no element/edge/point
indices** — and `UsdImagingGLEngine` exposes no `AddPoints`-equivalent. The test
also states the precondition: "we currently support picking and selection
highlighting points on prims only when points are rendered"
(`testHdxPickTarget.cpp:424-433`).

**Therefore, for usdGen:** encode CV selection state in **colour**, not in
Hydra selection. Two options, both scene-index-side:
(i) keep one prim: publish the guide with the points repr slot and a
`primvars:displayColor` (vertex interpolation) whose entries are the
selected/hover colours; point size is fixed at 3 px and not settable through
`UsdImagingGLRenderParams` (`usdImagingGL/renderParams.h:64-65` has no
`pointSize`); or
(ii) publish a synthesized `points` rprim as a child, where `widths` gives full
control of the dot size and `displayColor` gives the state. Option (ii) is the
usdRig guide pattern (`RIG/libs/rigExecImaging/sceneIndices.h` `_SyncGuideChildren`)
and is recommended because size control matters for a comb tool.

---

## 4. (d) HydraObserver path mapping for synthesized prims and prototypes

**Where a `UsdImagingSceneIndexPlugin` sits.** `_AddPluginSceneIndices` runs
**after** `UsdImagingPiPrototypePropagatingSceneIndex` and
`UsdImagingNiPrototypePropagatingSceneIndex`, after
`HdNoticeBatchingSceneIndex`, after `UsdImaging_InstanceProxyPathTranslationSceneIndex`
and after `UsdImagingMaterialBindingsResolvingSceneIndex`
(`USD/pxr/usdImaging/usdImaging/sceneIndices.cpp:236-302`). Consequences for
usdGen:

* The hair scene index sees **propagated** paths. For a native-instanced
  surface, the geometry lives at e.g.
  `/UsdNiPropagatedPrototypes/Bindings_423…/__Prototype_1/UsdNiInstancer/UsdNiPrototype/MyCube`
  (`usdImaging/niPrototypePropagatingSceneIndex.h:100-140`), once per
  *aggregated prototype*, not per instance. Hair generated there is shared by
  every instance of that prototype: per-instance variation requires either
  seeding from the instancer's `instanceId`/`primOrigin`, or generating before
  instancing. Flag this in the plan; it is a design decision, not a bug.
* `UsdImagingSelectionSceneIndex` is appended *after* the plugins
  (`sceneIndices.cpp:303-305`), so usdview's own selection still applies to
  prims usdGen synthesizes — but only whole-prim selection (§3).

**Picking a synthesized prim returns an EMPTY path unless it carries
`primOrigin`.** `UsdImagingGLEngine::TestIntersection` maps hydra→scene with
`HdxPrimOriginInfo::FromPickHit(...).GetFullPath()` (`engine.cpp:1275-1281`).
`GetFullPath` starts from a **default-constructed (empty) `SdfPath`** and only
assigns from `primOrigin` data sources (`hdx/pickTask.cpp:1297-1317`, and
`_AppendPrimOriginToPath` at 1275-1294 returns `false` and leaves the path
untouched when there is no `HdPrimOriginSchema`). A3's "the hydra path is left
unchanged" is wrong in effect: with no `primOrigin` anywhere the result is
`SdfPath()`, i.e. usdview selects nothing. **usdGen's synthesized prims must
publish `primOrigin { scenePath = <absolute stage path> }`** — an absolute path
*replaces* the accumulated path (`pickTask.cpp:1288-1290`), so pointing it at
the generator prim makes a click on the hair select the generator, which is what
an artist wants. `UsdImagingStageSceneIndex` does exactly this for real prims
(`usdImaging/dataSourcePrim.cpp:526, 563-568, 703`).

**Reading the terminal scene index from a test/tool.** The name registry is an
`unordered_map` (`hd/sceneIndex.h:298-302`) and `GetRegisteredNames()` returns
its iteration order, pruning expired entries (`hd/sceneIndex.cpp:257-273`), so
usdRig's `names[-1]` (`RIG/tests/testUsdviewRigExec.py:16-39`) is **not
guaranteed** to be the terminal SI — it works because there is normally one
entry. The registered name is `"[Terminal SI] " + parentIndex->GetInstanceName()`
and it is registered in the `HdSceneIndexAdapterSceneDelegate` constructor
(`hd/sceneIndexAdapterSceneDelegate.cpp:158-165`), i.e. one per render index; a
renderer switch adds a second. usdGen's tests should select by the
`"[Terminal SI]"` prefix and, when several match, by the render index instance
name — and must query prims at **propagated** hydra paths, not stage paths, on
instanced assets.

---

## 5. (e) Painting a mesh primvar live — the re-`PrimsAdded` trick is *conditional*

A3 carries usdRig's rule ("a primvar APPEARING is a resync, not a dirty … even a
universal one", `RIG/libs/rigExecImaging/sceneIndices.h:310-324`). In 26.08 the
rule is sharper, and I measured it headlessly with a null render delegate and a
scene index whose prim data source flips **without** any `PrimsAdded`
(`P/probePrimvarAppear2.cpp`, no GL required):

| notice sent after the primvar appears upstream | new primvar visible to the delegate? | cost of the following `GetPrimvarDescriptors` |
|---|---|---|
| `PrimsDirtied {}` (universal/root locator) | **NO** | 0.18 µs |
| `PrimsDirtied {primvars}` | **YES** | 4.10 µs |
| `PrimsDirtied {primvars/usdGen_density}` | **YES** | 2.06 µs |
| `PrimsDirtied {primvars/usdGen_density/primvarValue}` | **NO** | 0.24 µs |
| `PrimsDirtied {primvars/points/primvarValue}` (the per-move push) | **NO** | 0.14 µs |
| `PrimsAdded` with the same prim type (usdRig's trick) | **YES** | 2.45 µs |

The mechanism: `PrimsDirtied` clears the cached primvar descriptors only for a
locator whose **first** element is `primvars` and whose **last** element is not
`primvarValue` / `indexedPrimvarValue` / `indices`
(`hd/sceneIndexAdapterSceneDelegate.cpp:511-522`); `HdDataSourceLocator().GetFirstElement()`
returns the empty token (`hd/dataSourceLocator.cpp:101-109`), which is why a
universal dirty does **not** clear it. That narrowing landed in
`baa8f9cf4` (2025-10-21, "hold onto cached primvar descriptors across primvar
value changes"), i.e. it is in v26.08 — before it, *any* primvars-intersecting
dirty cleared the cache. `PrimsAdded` with the same type takes the resync branch
which clears both descriptor caches unconditionally
(`sceneIndexAdapterSceneDelegate.cpp:236-241, 287-297`).

**Rules for usdGen's paint tool:**

1. Per stroke move, dirty **`primvars/<paintName>/primvarValue`** only —
   0.14–0.24 µs of delegate bookkeeping, no descriptor rebuild, and the dirty
   bit is `DirtyPrimvar` (`hd/dirtyBitsTranslator.cpp:836-848`; note
   `primvars/points` maps to `DirtyPoints`, not `DirtyPrimvar`, lines 826-838).
2. The **first** time the paint primvar appears (and when it disappears), dirty
   `primvars/<paintName>` (or `primvars`) once. No re-`PrimsAdded` needed on
   26.08. Keep the "which prims have I announced" set anyway — you still need it
   to know when the primvar *disappears*, exactly as
   `_announcedWeightOverlays` does.
3. Do **not** rely on a universal dirty for anything primvar-shaped; it is
   silently a no-op for descriptors.

Per-stroke transport for painting is the same as §1.4: the paint value array is
a `VtFloatArray`/`VtVec3fArray` pushed COW (0.14 µs), the release write is
`attr.Set(vt)` at 1.6–1.8 µs.

---

## Appendix — probe sources and how to re-run

All under `P` = `…/scratchpad/probes/tool-loop/`:

| file | what |
|---|---|
| `cTransport.cpp` | C ABI transport + `UsdGenImaging_PickCV` / `_FootprintCV`. `g++ -O3 -std=c++17 -fPIC -shared -o libUsdGenTransportC.so cTransport.cpp` |
| `bpTransport.cpp` | pxr_boost module. `g++ -O2 -std=c++17 -fPIC -shared -o _usdgenTransport.so bpTransport.cpp -I$INST/include -I/usr/include/python3.12 -L$INST/lib -lusd_vt -lusd_gf -lusd_tf -lusd_arch -lusd_boost -lpython3.12 -Wl,-rpath,$INST/lib` |
| `pbTransport.cpp` | pybind11 module incl. the pybind11↔pxr_boost hybrid. Same flags plus `-I$(python -c 'import pybind11;print(pybind11.get_include())')` |
| `probePrimvarAppear2.cpp` | §5 table. Add `-lusd_hd -lusd_hf -lusd_sdf -lusd_trace -lusd_python` |
| `bench_transport.py` (§1.2), `bench_stroke.py` (§1.4, §1.6), `bench_pick_cpu.py` / `bench_pick_cpp.py` (§2.4) | run as `REPS=101 python bench_transport.py 100000`; raw output in `bench_*.log` |

Every python run needs
`PYTHONPATH=$USD/lib/python3.12/site-packages:.`
and `$VENV/bin/python3`.

---

## Key facts

- `numpy 2.5.2` and `pybind11 3.1.0` installed cleanly into `$VENV`
  (`pip install numpy pybind11`) and coexist with `pxr`; a pxr_boost.python
  module also builds out-of-tree against the install
  (`$INST/include/pxr/external/boost/python.hpp`, `libusd_boost.so`,
  `libusd_python.so`; `pxr.h:48,59`).
- **`VtArray` transport across a pxr_boost boundary is O(1)**: 0.13–0.18 µs for
  push and pull at 100k, 267k and 1M CVs (copy-on-write). Every `float*` route
  (ctypes / pybind11-numpy / pxr_boost+memcpy) is memcpy-bound and mutually
  equal: 13.5–16 µs at 1.2 MB, 42–47 µs at 3.2 MB, 340–360 µs at 12 MB.
- **`Vt.Vec3fArray.FromBuffer` costs 420 µs / 1.2 MB, 4.3 ms / 12 MB**
  (per-element strided walk, `vt/arrayPyBuffer.cpp:388, 427-431`), and
  `attr.Set(numpy)` pays the same. `attr.Set(Vt.Vec3fArray)` is **1.6–1.8 µs at
  any size**; `attr.Set` inside `Sdf.ChangeBlock` 2.2 µs.
- `Vt` arrays expose the buffer protocol **read-only** and in O(1)
  (`vt/arrayPyBuffer.cpp:229-235, 245`); `np.asarray(vt)` is a 0.19 µs
  zero-copy read-only `(N,3) float32` view that stays valid after the Python
  `Vt` object is dropped (measured).
- The usdRig `_rigexec.cpp:165-172` list-of-lists array route costs 9.3 ms
  (100k) to 109 ms (1M) per call; python-list inputs cost 18–177 ms. Both are
  disqualified.
- One brush move with the recommended transport is **21 µs** total on the Python
  side (0.34 µs zero-copy read + 16.6 µs numpy kernel over 2 000 CVs + 4.1 µs
  sparse push); the same kernel in pure Python is 309 µs.
- `Usd*.Define()` inside `Sdf.ChangeBlock` fails (`UsdStage::_DefinePrim`,
  stage.cpp:3893); freeze = define outside, author inside (60–86 µs for a
  100k-CV BasisCurves; 1.2 ms to export the 1.2 MB `.usdc`).
- `UsdImagingGLEngine::TestIntersection` never sets `pickTarget`
  (`engine.cpp:1243-1249`) and copies no sub-prim index
  (`engine.cpp:1286-1288`); `wrapEngine.cpp:218-236` confirms Python sees only
  `hitPoint/hitNormal/hitPrimPath/hitInstancerPath/hitInstanceIndex/instancerContext`
  and `PickParams(resolveMode)` only.
- `DRAW_POINTS` *does* select `HdReprTokens->points` for the intersect
  collection (`engine.cpp:2409-2412`), which does make basisCurves emit point
  ids (`hdSt/basisCurves.cpp:306-311, 464-468`; `basisCurvesShaderKey.cpp:189, 208-215, 504-508`; default repr configured at `hd/renderIndex.cpp:1066-1067`),
  and `_ResolveHit` fills `pointIndex` regardless of pickTarget
  (`hdx/pickTask.cpp:1042-1044`) — the engine simply discards it.
- **`UsdImagingGLEngine` has no `GetRenderIndex()`** in 26.08 (grep count 0 in
  `engine.h`); the task controller scene index and renderer are `protected`,
  the terminal scene index and legacy render control are `private`
  (`engine.h:810-847`). A plugin-side `HdxPickTask` therefore needs its **own**
  render index (pattern: `hdx/testenv/testHdxPickTarget.cpp:154-177`).
- Measured CPU CV pick in C++ over ctypes: **166 µs for 100k CVs, 1.66 ms for
  1M** (single thread); footprint queries 0.59 / 1.07 ms. numpy equivalents are
  1.8 ms / 17.2 ms with BLAS pinned to one thread, and 10–230 ms when BLAS is
  left multi-threaded on a loaded host — **the plugin must pin BLAS threads**.
- A per-prim `displayStyle:reprSelector = ["","","points"]` composites *over*
  the collection repr (`hd/sceneIndexAdapterSceneDelegate.cpp:3089-3108`;
  `hd/rprim.cpp:157-161`; `hd/renderIndex.cpp:1287-1298`; `hd/repr.h:77-84`) and
  dirties as `DirtyRepr` (`hd/dirtyBitsTranslator.cpp:747-751`), so CVs can be
  drawn on the guide prim itself.
- CV *highlighting* through Hydra selection is not reachable: `HdSelectionSchema`
  carries only `fullySelected` and `nestedInstanceIndices`
  (`hd/selectionSchema.h:36-41`), and the engine exposes no point-selection API.
  `HdSelection::AddPoints` with per-point colour works only inside a private
  render index (`hdx/testenv/testHdxPickTarget.cpp:470-493`).
- Plugin scene indices run **after** Pi/Ni prototype propagation
  (`usdImaging/sceneIndices.cpp:236-302`): hair synthesized on an instanced
  surface lives at `/UsdNiPropagatedPrototypes/…/UsdNiPrototype/…`, once per
  aggregated prototype (`niPrototypePropagatingSceneIndex.h:100-140`).
- A picked prim with **no `primOrigin`** yields an **empty** `hitPrimPath`:
  `HdxPrimOriginInfo::GetFullPath` starts from an empty `SdfPath` and only
  assigns from `primOrigin` (`hdx/pickTask.cpp:1275-1294, 1297-1317`).
  Synthesized prims must publish `primOrigin{scenePath}` (absolute paths replace,
  relative ones append).
- `HdSceneIndexNameRegistry` is an `unordered_map` pruned on read
  (`hd/sceneIndex.h:298-302`, `sceneIndex.cpp:251-273`), registered as
  `"[Terminal SI] <instanceName>"` per adapter delegate
  (`sceneIndexAdapterSceneDelegate.cpp:158-165`) — `names[-1]` is not a
  guaranteed terminal-SI selector.
- **Measured**: after a primvar appears upstream, a `PrimsDirtied` on
  `primvars` or `primvars/<name>` makes it visible; a universal `{}` dirty and a
  `…/primvarValue` dirty do **not**; `PrimsAdded` does. Mechanism at
  `hd/sceneIndexAdapterSceneDelegate.cpp:511-522` (+ `hd/dataSourceLocator.cpp:101-109`),
  introduced by commit `baa8f9cf4` (2025-10-21).
- A pxr_boost module's Vt converters come from `pxr/Vt/_vt.so`: calling a
  Vt-typed function before `from pxr import Vt` raises
  `TypeError: No to_python (by-value) converter found …VtArray<…GfVec3f>` (measured).

## Decisions this settles

1. **Install numpy + pybind11 into the venv (done) but transport arrays with
   pxr_boost `VtArray`.** usdGen ships two Python surfaces: the ctypes C ABI for
   control/scalars (activate, set time, generation, live-override begin/clear,
   CV pick queries) and a `_usdgenImaging` pxr_boost module for arrays
   (`ReadCurvePoints/Counts/Widths`, `SetLiveOverride`,
   `SetLiveOverrideIndexed`). numpy is the *kernel* language over a zero-copy
   `np.asarray(vt)` view, never the transport for the release write.
2. **Never build a `Vt` array from numpy in Python.** C++ returns `VtArray`s;
   Python authors them with `attr.Set(vt)` (1.8 µs at 1M CVs) inside one
   `Sdf.ChangeBlock`. Ban `Vt.*Array.FromBuffer` and `attr.Set(ndarray)` on
   groom-sized arrays in review.
3. **Per-move pushes are sparse and indexed** (`VtIntArray` + `VtVec3fArray`, or
   `int*`+`float*`): 1.2–3.7 µs for a 2 000-CV footprint versus 13.5 µs for a
   full 100k-CV push. Full pushes are for the press/release boundaries.
4. **CV picking is CPU, in C++, exported over the C ABI** — 166 µs at 100k CVs,
   8x cheaper than one `view.pick()`. No `HdxPickTask` and no engine patch. The
   private-render-index `HdxPickTask` route is documented as the fallback for
   occlusion-correct CV picks only.
5. **The plugin pins BLAS threads to 1** (`OPENBLAS_NUM_THREADS`/`OMP_NUM_THREADS`
   set before `import numpy`, or `threadpoolctl`), because the default
   multi-threaded BLAS turned a 0.56 ms matmul into 22 ms on a busy host.
6. **CV display uses colour, not Hydra selection**: a synthesized `points` child
   prim (widths + `displayColor`) — or the guide's own
   `displayStyle:reprSelector` points slot when a fixed 3-px dot is acceptable.
   No plan depends on `HdSelection::AddPoints` reaching usdview.
7. **Every prim usdGen synthesizes carries `primOrigin{scenePath=<stage path>}`**,
   or clicking it in usdview selects nothing.
8. **Live paint dirties `primvars/<name>/primvarValue` per move and
   `primvars/<name>` once on appearance/disappearance.** No re-`PrimsAdded`
   per stroke; keep an "announced" set only to detect disappearance. A universal
   dirty is never a substitute.
9. **Freeze authors prims outside `Sdf.ChangeBlock`** (or uses the pure `Sdf`
   API entirely inside one), and the undo module needs a `PrimSnapshot` beside
   `AttributeSnapshot`.
10. **Tests select the terminal scene index by the `"[Terminal SI]"` prefix**,
    not `names[-1]`, and expect propagated hydra paths on instanced assets.

## Open questions

- Cost of the **evaluator republish + Storm re-sync** per move (the part the
  21 µs Python loop hands off to). UNMEASURED here: needs a display. Protocol:
  time `UsdGenImaging_SetLiveOverride` → `UpdateViewport()` → converged frame in
  `testusdview`, at 10k / 100k / 1M CVs, with `HD_ENABLE_TRACE`/`Trace` around
  `HdStBasisCurves::Sync`.
- Whether Storm tolerates a **changing CV count** within a live override without
  a topology dirty (usdRig never varies counts within a generation). Read
  `hdSt/basisCurves.cpp` `_PopulateVertexPrimvars` or test on a workstation;
  until answered, the live override must keep counts fixed and resample only at
  press/release.
- Whether the drawn point size for the points repr can be raised without a
  second prim (no `pointSize` in `usdImagingGL/renderParams.h`;
  `HdxRenderTaskParams::pointSize` defaults to 3.0 at
  `hdx/renderSetupTask.h:133` but usdview never sets it).
- Whether a **second render index + `HdxPickTask`** can share usdview's GL
  context cleanly (Hgi ownership, `makeCurrent` reentrancy) — only if the CPU
  pick's lack of occlusion proves unacceptable in P2.
- Whether `HdSt` point-picking of basisCurves resolves *CV* or *segment-start*
  indices on a refined curve (points index buffer from
  `GetPointsIndexBuilderComputation`, `hdSt/basisCurves.cpp:725-727`); relevant
  only if the fallback is taken.
- Whether TBB parallelisation of `UsdGenImaging_PickCV` is worth it at 1M CVs
  (1.66 ms single-thread) or whether the root-prefilter (measured 116 µs for
  N/10 roots in numpy) is enough.
