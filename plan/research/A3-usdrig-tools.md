# A3 — usdRig usdview tooling patterns: panels, undo, live drag → stage, picking, overlays, testusdview

Research report for the usdGen (hair/fur grooming) plan. Every claim cites
`absolute/path:line`; anything not read directly is marked UNVERIFIED.
Line numbers are for the files as of commit `c92c040` (usdRig) and tag
`v26.08` (OpenUSD source at `/home/burkard/work/OpenUSD`).

Abbreviations: `RIG` = `/home/burkard/work/usdRig`, `USD` =
`/home/burkard/work/OpenUSD`, `UVQ` = `USD/pxr/usdImaging/usdviewq`,
`PLUG` = `RIG/plugin/rigExecUsdview`.

---

## 0. What usdRig ships as usdview tooling (inventory)

| Module (`PLUG/…`) | Lines | Qt? | Role |
|---|---|---|---|
| `rigExecUsdview.py` | 613 | no (imports only `pxr` + `PluginContainer`) | The plugin container: activates the C++ imaging library over ctypes, owns the shared undo stack, lazily installs every panel/overlay |
| `rigExecUndo.py` | 243 | no | Attribute-spec snapshots, `Edit`, bounded `UndoStack`, `EditRecorder` |
| `gizmoMath.py` | 1552 | no | Evaluator replica, edit `Target`s that map world deltas to channels, `Writer` (animation vs default) |
| `gizmoScreen.py` | 762 | no | Projection, handle geometry, hit testing, ray/plane drag math (physical pixels) |
| `gizmoDrag.py` | 624 | no | "What a drag means": `DragState`, `ApplyDrag`, snap precedence, pick throttle |
| `gizmoSnap.py` | 336 | no | Grid/point/edge/surface snap maths, `SnapCandidate` |
| `gizmoSettings.py` | 266 | no | Per-tool settings with listeners |
| `gizmoUI.py` | 3208 | yes | Toolbar, transparent overlay, event-filter controller, hotkeys, hydra pick bypass |
| `graphModel.py` | 1070 | no | Curve discovery and every `Ts.Spline` edit op; `ApplySpline` |
| `graphScreen.py` | 656 | no | Graph editor view transform / hit-testing |
| `graphEditorUI.py` | 2680 | yes | Graph editor window, `_Gesture` undo bracket |
| `curvenetUI.py` | 1557 | yes (module scope) | Curvenet authoring panel: surface picker, per-move stage writes, session-layer display prims |
| `volumeWeightUI.py` | 2297 | yes (module scope; helpers above the "Qt widgets" banner are Qt-free) | Volume weight panel, `SetAtTime`, influence overlay switch |
| `viewCubeMath.py` / `viewCubeUI.py` | 503 / 842 | no / yes | Mouse-accepting child-widget overlay that drives the free camera |
| `plugInfo.json` | — | — | Registers the container type for `Plug` |

Docs that describe them: `RIG/docs/viewport-gizmos.md`, `graph-editor.md`,
`view-cube.md`, `curvenet.md` ("The usdview authoring tools", lines
433–564), `volume-weights.md` (overlay + three invalidation traps, lines
280–417). Design specs and implementation plans live in
`RIG/docs/superpowers/{specs,plans}/` (section 6).

---

## 1. The plugin container pattern and lazy Qt imports

### 1.1 Registration with usdview

`PLUG/plugInfo.json` is a `"Type": "python"` plugin whose only registered
type is `rigExecUsdview.RigExecUsdviewContainer` with
`"bases": ["pxr.Usdviewq.plugin.PluginContainer"]` and a `displayName`;
`"Root": "."`, `"ResourcePath": "."`, `"LibraryPath": ""`. usdview finds it
through `PXR_PLUGINPATH_NAME` (the plugin directory itself is listed:
`RIG/bin/_env.sh:32`), and the same directory is put on `PYTHONPATH`
(`_env.sh:26`) so the module name in the type resolves.

usdview's loader (`UVQ/plugin.py:292-344`) does, in order:
`Plug.Registry.GetAllDerivedTypes(PluginContainerTfType)` → `plugin.Load()`
→ `containerType.pythonClass()` (line 321) → `container.registerPlugins(
registry, usdviewApi)` (333) → `container.configureView(registry,
uiBuilder)` (342). The container class must be a defined `Tf.Type`:
`PLUG/rigExecUsdview.py:613` ends with `Tf.Type.Define(RigExecUsdviewContainer)`
(the base does the same at `UVQ/plugin.py:129`).

Menu API available to a container (`UVQ/plugin.py`): `PluginRegistry.
registerCommandPlugin(name, displayName, callback, description="")`
(221–246; `callback(usdviewApi)` is the only signature, 163–166),
`PluginUIBuilder.findOrCreateMenu(menuName)` (277–289; it also exposes
usdview's built-in menus by title, 267–275), `PluginMenu.addItem(commandPlugin,
shortcut=None)` (177–190), `findOrCreateSubmenu`, `addSeparator`.

### 1.2 The container's shape (`PLUG/rigExecUsdview.py`)

- **Keep-alive.** usdview does not retain a container that registers no
  commands, so the module stores `_container = self` (88–98).
- **Commands registered** (125–167), all under one `RigExec` menu
  (179–186): `RigExecUsdviewContainer.reactivate`, `.volumeWeights`,
  `.curvenets`, `.graphEditor`, `.viewportTools` (a toggle, not a window),
  `.viewCube` (toggle).
- **Signals wired at registration** (169–177): `dataModel.signalStageReplaced
  → _OnStageReplaced`, `dataModel.currentFrameChanged → _OnFrameChanged`;
  if a stage already exists when the plugin loads late, `_OnStageReplaced()`
  runs immediately.
- **Plugins load BEFORE the stage and BEFORE the StageView exists**:
  `UVQ/appController.py:426-432` configures plugins right after creating
  `UsdviewApi`, and the `StageView` is only constructed at
  `appController.py:1863-1882`. Consequently every viewport-attached tool is
  installed from `_OnStageReplaced` (533–539: observe stage → activate C++
  → `_EnsureViewportTools()` → `_EnsureViewCube()`), and
  `gizmoUI.InstallViewportTools` retries itself on a 0 ms `QTimer.singleShot`
  up to 20 times until `StageView(usdviewApi)` is non-None
  (`PLUG/gizmoUI.py:3161-3203`).
- **Lazy Qt.** The container module itself imports no Qt so the C++ tests
  and headless scripts can import it. Each panel module is imported inside
  the menu callback with a `sys.path` fallback (230–246 for
  `volumeWeightUI`, 248–257 `curvenetUI`, 259–276 `graphEditorUI`,
  278–294 `rigExecUndo`, 296–327 `gizmoUI`, 337–366 `viewCubeUI`). A failed
  install is remembered (`_viewportToolsFailed`) so a headless session warns
  once, not on every stage replacement (305–311).
- **Shared undo stack.** `_UndoStack()` creates one `rigExecUndo.UndoStack`
  lazily; the gizmo toolbar and the graph editor both push onto it
  (`_OpenGraphEditor`, 259–276; `docs/graph-editor.md:75-87`).
- **Stage observation independent of the C++ side.** `_ObserveStage` registers
  `Tf.Notice.Register(Usd.Notice.ObjectsChanged, self._OnStageObjectsChanged,
  stage)` (429–436) and re-registers AFTER activation so the Python root-set
  guard runs before the native evaluator's own listener (577–582). The
  handler auto-activates when a `RigExecRoot` is authored into an open
  blank stage, waiting until the root has something to publish (447–501).
- **Frame handling trap.** `_FrameValue` / `_OnFrameChanged` (376–395,
  600–610) use the frame carried by the signal, never
  `dataModel.currentFrame`, because `UVQ/rootDataModel.py:152-162` emits
  `currentFrameChanged(value)` BEFORE assigning `self._currentFrame`.
- **Shutdown.** `atexit.register(self._Shutdown)` (113–121, 397–417)
  deactivates the C++ side and erases the `UsdUtils.StageCache` entry while
  Python is still alive; `_ReleaseCachedStage` explains that `StageCache.
  Insert` transfers strong ownership into the cache (503–531).

### 1.3 Panel conventions

- **Singleton window parented to usdview's main window:**
  `VolumeWeightPanel.GetInstance(usdviewApi, …)` (`PLUG/volumeWeightUI.py:
  1608-1619`), constructed as `QWidget(usdviewApi.qMainWindow,
  QtCore.Qt.WindowType.Window)` (1623–1624), following
  `selection.signalPrimSelectionChanged` and `signalStageReplaced`
  (1639–1642). `OpenVolumeWeightPanel` does `show(); raise_();
  activateWindow()` (2287–2297); curvenet is identical (1552–1557).
- **Edit target discipline:** every panel writes through the plain Usd API
  into `stage.GetEditTarget()`; usdview sets the edit target to the SESSION
  layer on open (`UVQ/appController.py:1283`), so by default nothing persists
  — the volume panel shows a label saying so (`volumeWeightUI.py:1675-1691`;
  `docs/viewport-gizmos.md:72-74`).
- **Qt-only through the shim:** `from pxr.Usdviewq.qt import QtCore, QtGui,
  QtWidgets` (never PySide directly); PySide6's `QAction` lives in
  `QtActionWidgets` with a PySide2 fallback (`gizmoUI.py:76-81`).
- **Qt-free rule:** all maths/model modules are importable without Qt and
  are tested headlessly; only `*UI.py` import Qt (plan Global Constraints,
  section 6).

---

## 2. Undo: `rigExecUndo.EditRecorder` and the live-drag protocol

### 2.1 What is recorded

`PLUG/rigExecUndo.py:1-16`: the stack records what an edit did to the
**edit-target layer**, not to the composed stage — restoring must put the
session spec back exactly, including removing it when it did not exist.

```python
class AttributeSnapshot:                         # rigExecUndo.py:20-104
    Capture(layer, specPath) -> exists, typeName, variability,
        hasDefault/default, spline (a COPY, Ts.Spline(spec.GetSpline())),
        timeSamples {t: v}                        # 41-59
    Restore(): inside Sdf.ChangeBlock(); removes the spec when !exists;
        recreates it via Sdf.CreatePrimInLayer + Sdf.AttributeSpec;
        restores default / spline / erases extra samples / re-sets samples  # 61-91
class Edit(label, entries)  # 115-141: ONE Sdf.ChangeBlock around all entries
class UndoStack             # 144-200: LIMIT = 200, Push clears redo, AddListener(fn)
class EditRecorder(stage, attrPaths)             # 203-243
    Begin():  layer = stage.GetEditTarget().GetLayer();
              specPath = target.MapToSpecPath(path); capture "before"   # 216-223
    Commit(label) -> Edit | None (only attributes whose after != before)  # 225-235
    Abort(): restore all "before" in one change block                    # 237-243
```

Why a single change block in `Edit._Apply` (122–135): an xform edit writes
op attributes and `xformOpOrder` as separate specs; restoring them in
separate blocks lets the stage see an `xformOpOrder` naming ops that do not
exist yet, and any listener that recomposes per notice reads the half state.
Nested blocks are harmless — USD sends notices when the OUTERMOST block
closes.

`docs/viewport-gizmos.md:90-98`: undo does not remove the empty `over` prim
specs a first drag creates in the session layer (harmless); the stack is
cleared on stage replacement (`gizmoUI.py:1948-1958`).

### 2.2 The drag protocol: many stage writes, one undo entry

Gizmo (`PLUG/gizmoUI.py`):

1. **Press → `_BeginDrag`** (3022–3072): `target.SetPreserveChildren(...)`
   BEFORE `AttributePaths()` (the undo set depends on it), then
   `recorder = rigExecUndo.EditRecorder(stage, target.AttributePaths());
   recorder.Begin(); target.BeginDrag()` (3040–3043). `BeginDrag` snapshots
   the base channel values (`gizmoMath.py:881-884`).
2. **Every mouse move → `_UpdateDrag`** (3074–3095): `gizmoDrag.ApplyDrag(
   drag, point, settings, …)` computes the new values **from the press-time
   base, never incrementally** (`gizmoDrag.py:233-235`), writes them through
   `Target.Apply*` → `Writer.Set` inside **one `Sdf.ChangeBlock` per event**
   (`gizmoMath.py:972-975` for rig targets; `1362-1368` for xform targets,
   with op CREATION deliberately outside the block, 1336–1345), then
   `drag.target.Refresh()`, `_RebuildHandles()`, `_Repaint()`,
   `toolbar.Sync()`, and finally `self.usdviewApi.UpdateViewport()` (3095).
3. **Release → `_EndDrag`** (3112–3131): `edit = drag.recorder.Commit(label)`;
   `undoStack.Push(edit)` only if something changed.
4. **Escape / stage loss → `_AbortDrag`** (3133–3154): `recorder.Abort()`
   restores the pre-drag specs; if the prim is gone, dropping the drag IS
   the abort.
5. **Undo/redo are refused while a drag is live** (1834–1849), and after an
   undo the controller must itself call `usdviewApi.UpdateViewport()`
   because restoring layer specs does not schedule a repaint (1851–1862).

Graph editor: identical bracket (`graphEditorUI.py:19-26`, `_Gesture`
897–940): `EditRecorder.Begin()` over every curve the gesture can touch,
`attr.SetSpline(edited)` on every move — recomputed from the spline the
gesture STARTED with — and `Commit()+Push` once at release. Per-move writes
go through `graphModel.ApplySpline(stage, attrPath, spline, undoStack=None,
label)` so its inner recorder's `Edit` is discarded (`graphEditorUI.py:
1389-1404`); `ApplySpline` itself (`graphModel.py:1034-1070`) clears the
layer opinion instead of authoring an empty spline (`ClearSpline`,
1004–1031).

Older panels (curvenet, volume weights) write per move **without** an undo
bracket: `curvenetUI._OnDrag` calls `MoveKnot`/`SetPoints` on every mouse
move (`curvenetUI.py:1096-1128`), and the volume panel's "mung" drag calls
`SetAtTime` per move (`volumeWeightUI.py:1070-1085`). They pre-date the
stack; `docs/viewport-gizmos.md:82-83` says they "can adopt it later".

### 2.3 Where a value lands (`Writer`)

`gizmoMath.Writer(stage, time, mode).Set(attr, value)` (530–558):
`WRITE_DEFAULT` → `attr.Set(value)` plus a warning when a spline/time samples
outrank the default; `WRITE_ANIMATION` → `SetAnimated` (500–527): uniform →
default; default time / existing time samples / spline-incapable type →
`attr.Set(value, time)`; otherwise a curve-interpolated **Ts spline knot**
authored through `graphModel.AuthorKnot`. `volumeWeightUI.SetAtTime`
(68–111) is the original rule, adapted from Pixar's
`extras/exec/examples/invertibleRigsExample`; `SetVisibleAtTime` (114–145)
documents the "spline outranks default" trap (usdview's current frame is
never Default: `appController.py:1409` per that docstring).

---

## 3. How tools reach the viewport

### 3.1 Getting the StageView and its camera

There is no public API for the widget; every module reaches it through
usdview's own name-mangling: `usdviewApi._UsdviewApi__appController._stageView`
(`gizmoUI.py:158-169`, `curvenetUI.py:488-492`, `viewCubeUI.py:74-82`). The
toolbar is inserted at index 0 of `appController._ui.glFrame.layout()`
(`gizmoUI.py:1195-1210`), the `QVBoxLayout` usdview builds at
`UVQ/appController.py:1879-1882`.

Public `UsdviewApi` surface used (`UVQ/usdviewApi.py`): `dataModel` (23),
`stage` (29), `frame` (35), `prim` (41), `selectedPoint` (63–67, the pick
point stored by `onPrimSelected`), `selectedPrims` (69), `currentGfCamera`
(108–115, a copy of `stageView.gfCamera`), `viewportSize` (117–125, logical),
`qMainWindow` (139–143), `UpdateGUI` (190), `ClearPrimSelection` /
`AddPrimToSelection` (204–208), `GrabWindowShot` / `GrabViewportShot`
(211–219), `UpdateViewport()` → `stageView.updateGL()` (221–225).

Camera: `view.resolveCamera()` returns `(Gf.Camera, aspect)` conformed to the
viewport and **emits `signalFrustumChanged` when the frustum changed**
(`UVQ/stageView.py:1589-1628`), so it is side-effecting — `gizmoUI._Camera`
(1517–1537) calls it only where a camera is really needed, and
`_RebuildHandles` guards against re-entry from the signal (1477–1486).
`view.computeWindowViewport()` is `(0, 0) + physical size` (1586–1587).

Pixel spaces: Qt events are LOGICAL pixels; the pick frustum and
`computeWindowViewport` are PHYSICAL. Both the curvenet picker and the gizmo
multiply by `devicePixelRatioF()` (`curvenetUI.py:846-869`,
`gizmoUI.py:2075-2091`), copying usdview's own `mousePressEvent`
(`stageView.py:2091-2094`). Projection is the exact inverse of
`computePickFrustum` (`curvenetUI.py:494-528`; `gizmoScreen.ProjectPoint`
154–161). Graph editor canvases, being ordinary widgets, stay logical
(`graphEditorUI.py:13-17`).

Signals a tool can hook (all on `usdviewApi.dataModel` unless noted):

| Signal | Where | Used by |
|---|---|---|
| `signalStageReplaced` | `UVQ/rootDataModel.py:25` | container, gizmo, panels |
| `signalPrimsChanged(ChangeNotice, ChangeNotice)` | `rootDataModel.py:26`, emitted from its own `ObjectsChanged` listener (77–102) | usdview GUI refresh (`appController.py:511-512`) |
| `currentFrameChanged(Usd.TimeCode)` | `rootDataModel.py:105` (emit-before-assign 158–160) | container `SetTime`, gizmo, graph playhead |
| `selection.signalPrimSelectionChanged(set, set)`, `signalPropSelectionChanged` | `UVQ/selectionDataModel.py:322-324` | gizmo target, panels, graph curve set |
| `viewSettings.signalSettingChanged`, `signalVisibleSettingChanged`, `signalFreeCameraSettingChanged` | `UVQ/viewSettingsDataModel.py:96-105` | view cube basis refresh |
| `stageView.signalFrustumChanged` | `stageView.py:708`, emitted by `resolveCamera` | gizmo handle rebuild (`gizmoUI.py:1179`) |
| `stageView.signalPrimSelected(path, instIdx, tlPath, tlIdx, point, button, modifiers)`, `signalPrimRollover`, `signalMouseDrag`, `signalSwitchedToFreeCam` | `stageView.py:695-706` | usdview itself; NOT usable for surface points (see 3.2) |
| `freeCamera.signalFrustumChanged` | `UVQ/freeCamera.py:24` | forwarded by view settings; view repaints on it (`stageView.py:855-856`) |

Stage notices: tools register `Tf.Notice.Register(Usd.Notice.ObjectsChanged,
handler, stage)` directly (`gizmoUI.py:1270-1279`, `curvenetUI.py:1454-1464`
which ignores notices on its own display prims). The gizmo filters notices
with `gizmoMath.NoticeAffectsTarget` (314–344) because an unconditional
refresh cost "a full rig walk plus a resolveCamera() … plus a reprojection"
for every notice from every other panel (1960–2015).

### 3.2 Picking

**Stock path.** `StageView.pick(pickFrustum)` (`UVQ/stageView.py:2224-2278`)
does `makeCurrent()`, `GL.glDepthMask(True)`, refreshes
`self._renderParams` from the view settings (including `showGuides =
viewSettings.displayGuide`, 2250) and calls
`renderer.TestIntersection(pickParams, frustum.ComputeViewMatrix(),
frustum.ComputeProjectionMatrix(), stage.GetPseudoRoot(), self._renderParams)`
with `pickParams.resolveMode = "resolveNearestToCenter"` (2263–2274).
`computePickFrustum(x, y)` (2280–2305) normalises the PHYSICAL pixel over
`computeWindowViewport()` and returns `(inImageBounds,
cameraFrustum.ComputeNarrowedFrustum(point, size))` with a 1-pixel size.

usdview's own click handler `pickObject` (2308–2367) OVERWRITES the hit
point's x/y with scaled mouse coordinates before emitting
`signalPrimSelected` (2342–2348), so `signalPrimSelected` / `api.selectedPoint`
cannot recover the surface point (`curvenetUI.py:13-21`, `docs/curvenet.md:
445-451`). Tools therefore call `computePickFrustum` + `pick` themselves from
an event filter installed on the stage view (`curvenetUI.SurfacePicker.Pick`,
530–553).

**Engine API** (`USD/pxr/usdImaging/usdImagingGL/engine.h`):
`TestIntersection(const PickParams&, viewMatrix, projectionMatrix, const
UsdPrim& root, const UsdImagingGLRenderParams&, IntersectionResultVector*)`
(373–379). `PickParams` has ONLY `resolveMode` (348–364).
`IntersectionResult` = `hitPoint`, `hitNormal`, `hitPrimPath`,
`hitInstancerPath`, `hitInstanceIndex`, `instancerContext` (322–343) —
**no element/edge/point index**. The Python wrapping exposes exactly those
(`wrapEngine.cpp:218-233`). Implementation (`engine.cpp:1210-1296`):
releases the GIL (1227), `PrepareBatch` (1229), builds the intersect
collection from the render params' draw mode (1238; repr choice in
`_UpdateHydraCollection` 2397–2430), fills `HdxPickTaskContextParams`
with resolveMode/view/proj/clipPlanes/collection but **never sets
`pickTarget`** (1243–1249) so the default `pickPrimsAndInstances` applies
(`USD/pxr/imaging/hdx/pickTask.h:276`), executes the picking task paths
(1257), and maps hydra ids back to USD paths via `HdxPrimOriginInfo::
FromPickHit(terminalSceneIndex, hit).GetFullPath()` (1277–1280;
`pickTask.cpp:1226-1234, 1297-1317`: with no `primOrigin` data source the
hydra path is left unchanged). Copies `worldSpaceHitPoint`,
`worldSpaceHitNormal`, `instanceIndex` (1286–1288).

**Hdx capability below the engine** (`hdx/pickTask.h`): resolve modes
`resolveNearestToCamera | resolveNearestToCenter | resolveUnique |
resolveAll | resolveDeep` (37–41); pick targets `pickPrimsAndInstances |
pickFaces | pickEdges | pickPoints | pickPointsAndInstances` (51–55);
`HdxPickHit` carries `objectId, instanceIndex, elementIndex, edgeIndex,
pointIndex, worldSpaceHitPoint, worldSpaceHitNormal, normalizedDepth`
(82–107); `HdxPickTaskContextParams` adds `resolution (128x128 default)`,
`maxNumDeepEntries`, `doUnpickablesOcclude`, `pointSize`, `depthBias*`
(269–310). Point picking validity requires a pointId AOV hit
(`pickTask.cpp:1373-1389`); Storm's `basisCurves.glslfx` imports
`pointId.glslfx` (line 16), so curve CVs can emit point ids — but that
`pickTarget` is unreachable from `UsdImagingGL.Engine` in Python. Any
per-CV or per-face pick for usdGen must therefore be either (a) screen-space
in Python (what usdRig does), or (b) a small C++ helper that runs an
`HdxPickTask` with `pickTarget = pickPoints` (needs access to the engine's
render index/task controller — UNVERIFIED that `UsdImagingGLEngine` exposes
enough of it; `GetRenderIndex()` exists per the header grep but was not
read in detail).

**usdRig's snap pick bypass** (`gizmoUI._SnapPick`, 2456–2542): because the
plugin forces `viewSettings.displayGuide = True` on activation
(`rigExecUsdview.py:583-590`), `view.pick()` returns a RigExec guide for
almost every hit; so the gizmo builds a FRESH `UsdImagingGL.RenderParams()`
with `showGuides = False` and calls `view._getRenderer().TestIntersection(
…)` itself, never mutating `view._renderParams` and never toggling
`displayGuide` (which would fire a visibility signal and a repaint).
Measured costs (design spec `RIG/docs/superpowers/specs/2026-09-03-gizmo-
snapping-design.md:129-142, 157-173`): ~1.3 ms per `pick()` offscreen, 1.42
ms via the bypass, 5.7–18 ms Hydra sync on the first pick after a
topology/visibility change. The pick honours visibility, purpose,
complexity; the root is hard-coded to the pseudo-root, so exclusion is by
filtering the returned path (spec:134–136).

**Screen-space selection of components** (`docs/curvenet.md:473-496`,
`curvenetUI._NearestControlPoint` 901–964): project all candidate points
through the view's camera, rank within a pixel radius by (screen distance
in ~2 px buckets, knot-over-handle, nearer-the-camera). Selecting must NOT
require a surface hit (knots float off the surface). `gizmoSnap.NearestPoint
/ NearestSegment` (212–298) do the same for vertices/edges with a
perspective-correct edge parameter; radii are `SNAP_PIXELS = 12` logical and
picks are throttled to `PICK_MOVE_PIXELS = 3` physical px of travel
(`gizmoSnap.py:39-47`, `gizmoDrag._ResolveCandidate` 334–374).

**Rig-deformed geometry is not pickable-by-authored-points**: the authored
`points` are the rest array; Hydra draws the evaluated array published by
the results scene index, and `librigExecImaging` exports no points accessor
(spec:218–233). This is the single most important constraint for a comb
tool (section 7).

### 3.3 Overlays: two techniques

| Technique | Where | Pros / cons (as documented) |
|---|---|---|
| **Transparent, mouse-transparent child `QWidget` of the StageView, painted with `QPainter`** | `GizmoOverlay` (`gizmoUI.py:182-256`): `WA_TransparentForMouseEvents`, `WA_NoSystemBackground`, `setAutoFillBackground(False)`, `NoFocus`; kept exactly over the view by `_SyncOverlay` on Resize/Show/Paint events with `raise_()` skipped on plain repaints (2053–2069) | Screen-constant, unoccluded, no evaluator re-entry on camera moves; works because Qt 6 composites `QOpenGLWidget` through the backing store (`gizmoUI.py:16-23`, verified by smoke grabs). Fallback "wrap paintGL and draw with QPainter on the GL widget" was NOT needed |
| **Mouse-ACCEPTING child widget** | `ViewCubeWidget` (`viewCubeUI.py:15-25, 112-160`): only the cube/home glyph take presses; `Alt`/`Meta` and unclaimed events are `ignore()`d so usdview's navigation still works | For overlays that are themselves controls |
| **Synthesized/authored Hydra prims** | Curvenet display: `UsdGeom.BasisCurves` + `UsdGeom.Points` DEFINED ONCE in the SESSION layer under the curvenet prim, then only attribute-edited and hidden via `visibility` — never removed (`curvenetUI.py:1320-1443`, `docs/curvenet.md:506-522`). Purpose `default`, not `guide` (1347–1349). Notices on those paths are ignored by the panel itself (1454–1464) | Depth-correct, lit, picked by Storm; but every attribute edit is a stage notice that re-evaluates the rig, and a prim add/remove is a resync → full recompile; also a known engine defect: removing any prim on a rigged stage posts `Usd_PrimFlagsPredicate` errors (`curvenet.md:513-522`) |
| **Synthesized in the C++ scene index (no stage)** | RigExec guides (`rigGuide*` children) and the influence overlay `displayColor` primvar, published in the snapshot store (`snapshotStore.h` `RigExecPublishedPrim.hasGuides / hasControlGuide / hasVolumeGuides / hasWeightOverlay`) and announced by `RigExecResultsSceneIndex` | Zero stage traffic; but "a primvar APPEARING is a resync, not a dirty" — Storm's `HdSceneIndexAdapterSceneDelegate` caches primvar descriptors and only rebuilds them on `PrimsAdded`, so the overlay must re-announce the prim on on/off (`sceneIndices.h` `_announcedWeightOverlays` comment; `docs/volume-weights.md:289-311`) |

Hover without GPU picks: the gizmo enables `Qt.WA_Hover` on the stage view
instead of mouse tracking, because mouse tracking would make usdview run
`pickObject` on every mouse move (`gizmoUI.py:1171-1178`; `stageView.py:
2160-2164`).

Event filtering rules shared by all viewport tools: never claim a press
with `Alt`/`Meta` (usdview's camera modifiers, `stageView.py:2096-2109`;
`gizmoUI.py:2104-2106`, `curvenetUI.py:817-827`); return `False` to let
usdview pick when the tool has nothing under the cursor (`gizmoUI.py:
2113-2115`); keys come from an application-level filter because usdview's
`AppEventFilter` refocuses the main window on every mouse move and swallows
`Escape` (`gizmoUI.py:25-64`).

Camera writes: the view cube sets `freeCamera.rotTheta/rotPhi` after
`view.switchToFreeCamera()` and calls `view.updateGL()` explicitly
(`viewCubeUI.py:603-640`); `usdview`'s own tumble rate is 0.25°/physical px
(`stageView.py:2134-2135`).

Screenshots: `appController.GrabViewportShot()` = `stageView.grabFrameBuffer()`
cropped to the physical size (`appController.py:2763-2775`);
`GrabWindowShot()` composes the GL widget over a `mainWindow.render`
(2744–2756); tests use `view.window().grab().save(path)`
(`RIG/tests/testUsdviewGizmo.py:1163`) or `stageView.grabFrameBuffer()` after
`updateGL()` + `processEvents()` (`tests/testUsdviewVolumeWeightOverlay.py:
118-124`).

---

## 4. Driving the C++ imaging library from Python; the latency chain

### 4.1 The C surface (ctypes, no bindings)

`PLUG/rigExecUsdview.py:1-8, 62-85` loads `librigExecImaging.so` with
`ctypes.CDLL(ImagingLibraryPath())` where the path is `RIGEXEC_IMAGING_DLL`
or `<moduleDir>/../../lib` (installed) or `<repo>/build` (source)
(20–59). Exported entry points (`RIG/libs/rigExecImaging/registry.h`, in the
saved header dump):

| C entry point | Signature | Behaviour |
|---|---|---|
| `RigExecImaging_Activate` | `(long long stageCacheId, const char* rigPath, double initialFrame) -> int` (0 = ok) | Stage handed over in-process via `UsdUtils.StageCache.Get().Insert(stage).ToLongInt()` (`rigExecUsdview.py:571-574`); empty rigPath = every `RigExecRoot`; compiles + first evaluation, then atomically publishes |
| `RigExecImaging_SetTime` | `(double frame) -> int` | Serialized evaluate-then-publish, broadcast to every chain |
| `RigExecImaging_Deactivate` | `() -> void` | Drops the bridge; chains keep reading the cleared store |
| `RigExecImaging_GetGeneration` | `() -> long long` | Published generation counter (0 before first publish) — used by tests to prove a publication happened (`tests/testUsdviewRigExec.py:152-170`) |
| `RigExecImaging_GetGuideBoundsAssetSpace` / `_GetAllGuideBoundsAssetSpace` | `(const char*, double[6]) -> int` / `(double[6]) -> int` | Asset-space bounds of synthesized guides, because `UsdGeomBBoxCache` sees nothing for them |
| `RigExecImaging_SetWeightOverlay` | `(const char* weightPrimPath) -> int` | Selects the influence overlay and republishes at the current time; optional symbol, bound in a try/except so an older library only disables the checkbox (`rigExecUsdview.py:72-84, 203-228`) |

There is deliberately **no points/curves accessor** exported. The separate
pybind11 module `_rigexec` (`RIG/python/_rigexec.cpp:598-746`) exposes
`Rig(stage, rigPath).compile()/evaluate(time) -> Pose` with
`joint_frame / control_frame / moved_property / moved_properties /
weight_field / weight_frame`, and `build-python/python` is on `PYTHONPATH`
only for the headless tests (`bin/run_python_tests.sh:12`), not for usdview
(`bin/_env.sh:26`).

### 4.2 The Hydra side (for orientation; details in the imaging report)

`RIG/libs/rigExecImaging/sceneIndexPlugin.cpp:23-46` registers a
`UsdImagingSceneIndexPlugin` (discovered through
`plugin/rigExecImaging/resources/plugInfo.json.in`: `"Type": "library"`,
`"LibraryPath": "../../<lib>"`, type `RigExecUsdImagingSceneIndexPlugin`
with base `UsdImagingSceneIndexPlugin`) whose `AppendSceneIndex` chains
pruning → binding → results filters and registers them with the
process-global `RigExecImagingRegistry`. Results `GetPrim()` only reads the
atomic snapshot (`snapshotStore.h`: `RigExecSnapshotStore::Publish` diffs
per-prim leaves and `std::atomic_store`s the new generation; `Get()` is a
lock-free `std::atomic_load`). The snapshot records the stage (weak) and
sample time so a consumer can check `Describes(stage, time)`.

### 4.3 The latency chain from a Python stage edit to pixels (one drag event)

1. Python writes the attribute(s) inside one `Sdf.ChangeBlock`
   (`gizmoMath.py:972-975 / 1362-1368`). USD dispatches ONE
   `Usd.Notice.ObjectsChanged` when the block closes.
2. Listeners run synchronously on the Qt main thread, **most recently
   registered first** (`Tf_NoticeRegistry::_Register` prepends —
   `docs/volume-weights.md:336-344`). In a usdRig session that is roughly:
   the gizmo's `_onObjectsChanged` (returns early during its own drag,
   `gizmoUI.py:1987-1991`), the container's root-set guard
   (`rigExecUsdview.py:479-501`), usdview's `RootDataModel.__OnPrimsChanged`
   → `signalPrimsChanged` → `appController._onPrimsChanged` →
   `_updateForStageChanges` → `updateGUI()` which starts a 0 ms single-shot
   `_guiResetTimer` → `_resetGUI` (full prim-view rebuild only when a prim
   RESYNC happened) (`UVQ/rootDataModel.py:77-102`, `appController.py:
   511-512, 552-555, 1956-1970, 1983-1988, 2505-2521, 5509-5513`), then
   OpenExec's own invalidation listener, and LAST the registry's
   `_OnObjectsChanged` — registered BEFORE `Compile()` precisely so it sits
   behind exec's listener (`registry.cpp:246-266`; the bug it fixed is
   measured in `docs/volume-weights.md:328-378`).
3. `RigExecImagingRegistry::_OnObjectsChanged` (`registry.cpp:423-471`)
   ignores notices whose paths do not touch an asset root, otherwise calls
   `SetTime(_lastTime)` OUTSIDE its mutex (the mutex is non-recursive;
   `registry.cpp:413-418`). `SetTime` (343–377) evaluates every session,
   `Publish`es the new generation (atomic swap + leaf diff) and
   `_Broadcast`s to every chain → `RigExecResultsSceneIndex::
   NotifyGenerationPublished` → coalesced `PrimsDirtied` (structural
   changes → `PrimsAdded/Removed`) into Hydra's observer chain → Storm marks
   rprims dirty. Nothing is drawn yet.
4. Control returns to the Python handler, which refreshes its own state and
   calls `usdviewApi.UpdateViewport()` → `StageView.updateGL()` (a no-op
   during playback, `stageView.py:1525-1530`) → Qt schedules `paintGL` →
   `_paintGLWithRenderer` sets buffer size/framing/camera (1715–1733) and
   `renderer.Render(...)` executes the Hydra tasks; dirty rprims re-sync from
   the scene index (`GetPrim` reads the atomic snapshot) and draw. While the
   renderer is not converged, `paintGL` re-arms itself every 5 ms
   (`stageView.py:1877-1878`), which is why the gizmo does nothing expensive
   in its Paint-event hook (`gizmoUI.py:2025-2035`).
5. **The scene-index dirty does not schedule a Qt repaint by itself**
   (`volumeWeightUI.py:2276-2277`, `gizmoUI.py:1852-1854`): every tool must
   call `UpdateViewport()` after an edit or an undo; the container's
   `_OnFrameChanged` does not need to because usdview repaints on a frame
   change.

Threading/ordering rules that fall out: all evaluation happens inside the
notice dispatch on the main thread; the registry's `_mutex` is a plain
`std::mutex`, so a re-entrant author-inside-notice would self-deadlock
(`registry.cpp:255-258`); nothing in `Compile()`/evaluation authors to the
stage (`testRigExecNoAuthoring` asserts it); `TestIntersection` releases the
GIL (`engine.cpp:1227`), so a Python pick can overlap Hydra threads but not
the Qt loop.

Per-event budget as the snapping spec states it: "A mouse-move already
costs `ApplyDrag` → target write → `Refresh()` → `_RebuildHandles()` (which
calls `resolveCamera()`) → repaint → `UpdateViewport()`, plus a rig
re-evaluation. At 60 Hz the frame is 16.7 ms; snapping gets at most 4 ms of
it" (`2026-09-03-gizmo-snapping-design.md:174-177`). Absolute rig evaluation
timings are not documented (UNVERIFIED).

Costs usdRig learned the hard way (each is a test now):
- **Prim add/remove = resync = full recompile**, and removing a prim on a
  rigged stage posts a `Usd_PrimFlagsPredicate` error — hence "define once,
  edit attributes, hide with visibility" (`docs/curvenet.md:506-522`;
  `tests/python/test_rigexec_stage_edits.py:1-60`).
- **Primvar appearing needs `PrimsAdded`, not a dirty** (`docs/volume-weights.
  md:289-316`; mutation-verified by the overlay pixel test).
- **Listener order** (`docs/volume-weights.md:328-378`).
- **Frame from the signal, not the property** (`docs/volume-weights.md:
  380-417`; `tests/testUsdviewRigExec.py:172-204` drives one frame both ways
  and compares terminal-scene-index points).

---

## 5. The test patterns and runners

### 5.1 testusdview scripts (`RIG/tests/testUsdview*.py`)

Driver: `USD/pxr/usdImaging/bin/testusdview/testusdview.py`. A script must
define `testUsdviewInputFunction(appController)` with exactly one positional
argument (23, 92–110); `--testScript` is required (28–30);
`--defaultsettings` is forced (35–38); the viewport is pinned to
`SetPhysicalWindowSize(597, 540)` (52–53); the callback runs after initial
loading, then `closeAllWindows()` (55–61). The driver monkey-patches
`AppController._processEvents(iterations=10, waitForConvergence=False)`
(114–131) and `_takeShot(fileName, …)` (134–140). A dirty `Tf.Error.Mark`
fails the run (144–146).

Runner shape (`RIG/bin/run_testusdview_gizmo.sh:14-33`, identical for the
others): source `bin/_env.sh`, require python/testusdview/stage, `cmake
--build` if configured, and
`exec "$PY" "$TESTUSDVIEW" --testScript "$RIG/tests/X.py" [--renderer NAME]
"$STAGE"`. A bare first argument is the renderer display name (e.g.
`Embree`). Each script ends by printing a `RIGEXEC_<FEATURE>_OK …` banner and
optionally saves a grab to `$RIGEXEC_<FEATURE>_SHOT` (`testUsdviewGizmo.py:
1158-1166`).

What the scripts do, and the idioms worth copying:

- **Acquire the installed tool** after pumping events:
  `appController._processEvents(); controller = gizmoUI.GetController()`
  (`testUsdviewGizmo.py:253-262`); assert the edit target is the session
  layer (265–267); activate the main window so Qt application shortcuts
  dispatch (`QApplication.setActiveWindow(appController._mainWindow)`,
  270–280); resize the window (307) and frame/tumble the camera through
  `appController._frameSelection()` + `viewSettings.freeCamera.rotTheta/
  rotPhi/dist` (316–323).
- **Synthetic input in the units the tool uses**: `_Mouse(...)` builds
  `QtGui.QMouseEvent(kind, pos, globalPos, button, buttons, modifiers)` in
  LOGICAL pixels and `QApplication.sendEvent(view, event)` followed by
  `_processEvents()` (`testUsdviewGizmo.py:30-50, 57-101`); keys through
  `PySide6.QtTest.QTest.keyClick/keyPress/keyRelease` on the view (121–161;
  holds need `keyPress`, not `keyClick`). Coordinates come from the tool's
  own projections (`HandleScreenPositions()`, `KeyPixels()`, `ScreenPointForRegion()`).
- **Assert on the stage, not on the controller** (`testUsdviewGizmo.py:1-12`):
  attribute values at the frame, the spline knot in the session layer, the
  Ctrl+Z round trip.
- **Read the terminal scene index** with
  `from pxr.Usdviewq._usdviewq import HydraObserver`;
  `names = HydraObserver.GetRegisteredSceneIndexNames(); observer.
  TargetToNamedSceneIndex(names[-1])` (the last registration is the
  terminal one); `primType, ds = observer.GetPrim(Sdf.Path(p)); ds.Get(
  "primvars").Get("points").Get("primvarValue").GetValue(0.0)`
  (`tests/testUsdviewRigExec.py:16-39`, `testUsdviewVolumeWeightOverlay.py:
  58-98`). The C++/Python API: `GetRegisteredSceneIndexNames`,
  `TargetToNamedSceneIndex`, `TargetToInputSceneIndex(indices)`,
  `GetNestedInputDisplayNames`, `TargetToNestedInputSceneIndex(i)`,
  `GetChildPrimPaths`, `GetPrim`, `HasPendingNotices`, `GetPendingNotices`,
  `ClearPendingNotices` (`UVQ/hydraObserver.h:44-139`, wrapped at
  `wrapHydraObserver.cpp:259-279`). This is the same API the Hydra Scene
  Browser uses.
- **Pixel assertions**: capture before/after with `stageView.updateGL();
  QApplication.processEvents(); stageView.grabFrameBuffer()` and compare a
  subsampled pixel set as a FRACTION of the frame (HiDPI/size independent);
  the overlay test requires > 0.5 % changed and is mutation-verified
  (`testUsdviewVolumeWeightOverlay.py:26-31, 118-162`; `docs/volume-weights.
  md:313-326`). `viewSettings.showHUD = False` before capturing.
- **Drive the C++ side directly** in the same process: `ctypes.CDLL(
  ImagingLibraryPath())` then `RigExecImaging_GetGeneration()` /
  `_SetTime(1024.0)` / `_SetWeightOverlay(b"...")` (`testUsdviewRigExec.py:
  146-170`, `testUsdviewVolumeWeightOverlay.py:43-55`).
- **Fake-object unit tests inside a testusdview script**: the container is
  instantiated with `__new__` and fed stub `dataModel`/library objects to
  test activation branches without a second usdview
  (`testUsdviewRigExec.py:42-143`).
- Session-only stage edits in tests use `with Usd.EditContext(stage,
  stage.GetSessionLayer())` (`testUsdviewVolumeWeightOverlay.py:111-112`).
- `tests/testUsdviewVolumeWeightAuthoring.py` is, despite the name, a plain
  python script that imports the Qt module but constructs no widget (1–45).

### 5.2 Headless python tests (`RIG/tests/python/*.py`)

Plain scripts, no pytest: `_Check(cond, msg)` raising `AssertionError`,
`main()` running `(name, callable)` groups, printing `  ok: <name>` and a
final `RIGEXEC_<X>_OK (<n> groups)` banner, exit 0 (`test_rigexec_undo.py`
as shown in the plan, `RIG/docs/superpowers/plans/2026-09-01-viewport-gizmo-
toolbar.md:44-242`). The first executable lines are `import rigexec_test_env;
rigexec_test_env.SetupPluginTest()` BEFORE `from pxr import …`
(`tests/python/test_rigexec_undo.py:9-19`). `rigexec_test_env.SetupPluginTest`
(`tests/python/rigexec_test_env.py:42-91`) prepends the USD install's
`lib/python*/site-packages` (glob, so 3.12 works), sets
`PXR_PLUGINPATH_NAME` to `<usd>/lib/usd` if unset, registers DLL dirs on
Windows, and prepends `plugin/rigExecUsdview` to `sys.path`. `RIGEXEC_USD_
INSTALL` overrides the sibling `usd-install` guess. Tests that need the
codeless schema `Plug.Registry().RegisterPlugins(sys.argv[1])` with the
GENERATED resources dir (`build/usd/rigExecSchema/resources`). The native
comparison against `_rigexec` is optional/skippable (`test_gizmo_math.py:
87-105`).

CMake registration (`RIG/CMakeLists.txt:312-382`): inside `if
(RIGEXEC_BUILD_PYTHON AND RIGEXEC_BUILD_TESTS AND Python3_FOUND)`, one
`add_test(NAME testX COMMAND <python> tests/python/test_x.py [schema dir])`
per script (325–358), then a `foreach` that sets a SINGLE-entry
`PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python` (Windows ctest cannot carry
`;` in ENVIRONMENT values), appends `PXR_PLUGINPATH_NAME=<build>/usd/
rigExecSchema/resources` and `RIGEXEC_USD_INSTALL=${USD_INSTALL_DIR}`
(366–382). `bin/run_python_tests.sh:15-33` runs the same scripts by name with
`build-python/python` on `PYTHONPATH` and passes the schema dir only to the
tests listed in `SCHEMA_TESTS`.

### 5.3 Environment facts that will bite usdGen on this machine

`RIG/bin/_env.sh:12-32` assumes `usd-install` and `usd-pr4156-venv` as
siblings and hard-codes `PY_SITE="$USD/lib/python3.11/site-packages"` (22).
Here the install is `/home/burkard/work/OpenUSD_26_08` with `lib/python`
for Python 3.12 and the venv is `/home/burkard/.venv` (task statement), and
the README says Linux is "intended, but not yet verified" (`RIG/README.md`
platform table). A usdGen env script should derive the site-packages path
from the interpreter (or glob `lib/python*/site-packages`, as
`rigexec_test_env.py:59-61` already does) and export `USD`, `VENV`, `RIG`
explicitly. Also note `RIG/CMakeLists.txt:476-481` installs only
`plugInfo.json, rigExecUsdview.py, volumeWeightUI.py, curvenetUI.py` — the
gizmo/graph/viewcube/undo modules are missing from the install rule, so an
installed layout would fail the lazy imports (a usdGen plan should install
every module and test the installed layout).

---

## 6. The spec and plan formats in `docs/superpowers/`

Four pairs exist, named `YYYY-MM-DD-<feature>-design.md` (spec) and
`YYYY-MM-DD-<feature>.md` (plan); user docs in `docs/<feature>.md` open with
"Design spec `docs/superpowers/specs/…`. Added <date>." (`docs/viewport-
gizmos.md:3-4`).

### 6.1 Design spec skeleton (`specs/2026-09-01-viewport-gizmo-toolbar-design.md`, 363 lines)

```
# <Feature> for usdview (<one-line qualifier>)
Date: 2026-09-01. Status: approved for implementation under the assumptions
in section 1 (the request was executed autonomously; the assumptions are
the interpretation a careful colleague would make and are the first thing
to revisit if the result feels wrong).                       # lines 3-6
## 0. Request            -- the user's request quoted verbatim   # 8-14
## 1. Assumptions (decisions made without a live user)  -- numbered, each a
     decision with its rationale and file:line evidence          # 16-55
## 2. <Evaluator|usdview> facts the design relies on   -- formulas, measured
     numbers, file:line cites, "Measured (date, with a throwaway probe …)"  # 57-91
## 3. Architecture       -- one subsection per module with the exact public
     names/signatures (3.1 rigExecUndo.py (Qt-free) … 3.4 container)  # 93-176
## 4. Data flow of one drag     -- numbered steps                 # 178-187
## 5. Error handling            -- bullet list of failure → behaviour  # 189-196
## 6. Testing                   -- headless test groups + testusdview script,
     runner name, banner text, $…_SHOT env                        # 198-215
## 7. Out of scope (deliberately)                                  # 217-221
## 8. <Parity/extension> (added <date>, user direction)  -- binding
     definition that "supersedes sections … where they differ"    # 223-363
```

The later specs keep the numbering (`## 0. Request`, `## 1. Assumptions and
scope`, `## 2. The behaviour change this makes to a shipped feature`, `## 3.
Facts the design relies on` — "All verified by probe or by reading,
2026-09-03", `## 4. Design` with 4.1 "Where the code lives", 4.2 "The one
rule every mode shares", …, `## 5. Error handling`, `## 6. Testing` with
numbered assertable items and explicit "what a test may and may not assert
on `examples/ArmShotAnim.usda`") (`specs/2026-09-03-gizmo-snapping-design.
md:9-642`; `specs/2026-09-02-view-cube-design.md:9-333`). Facts sections
carry measured tables (e.g. the camera-direction table, `view-cube-design.
md:93-103`) and cite the OpenUSD install by absolute path and line.

### 6.2 Implementation plan skeleton (`plans/2026-09-03-gizmo-snapping.md`, 621 lines; the gizmo plan is 3100 lines with full code listings)

```
# <Feature> Implementation Plan
> **For the executing agent:** work task by task, in order. Each task ends
> with its tests passing; do not start the next on a failing one. Do NOT
> commit — the reviewer commits after checking.          # 3-5 (older: "For
  agentic workers: REQUIRED SUB-SKILL: superpowers:subagent-driven-development
  … Steps use checkbox (`- [ ]`) syntax", gizmo plan:3)
**Goal:** …   **Architecture:** …   **Tech Stack:** …   **Spec:** <path> — read it first
## Global Constraints                                     # 23-61
- **Environment:** `. bin/_env.sh` before every command; headless via
  `bin/run_python_tests.sh [name]`; end-to-end via `bin/run_testusdview_X.sh`;
  sibling runners that share the stage view "must also still pass"
- **Style:** match <existing module>: imports, PascalCase methods, `_onXxx`
  slots, UPPER_SNAKE constants, 79 columns, comments that explain WHY and
  cite `file:line`
- **Qt-free rule:** which files may not import Qt
- **Row-vector convention** (`Gf.Matrix4d`, `v * M`; `M.Transform(p)`,
  `M.TransformDir(d)`), degrees at public boundaries
- **Do not change these signatures** (tests assert kwargs by equality)
- **Never …** invariants pinned by existing end-to-end assertions
- **Tests are plain scripts** (`_Check`, `(name, callable)` groups, `..._OK`)
- **Files you may touch:** only those a task names; never reformat/revert/
  stash/reset; no `git commit`/`git add` (or the commit trailer when the
  agent does commit: `Co-Authored-By:` + `Claude-Session:` lines)
- **Report:** append a section per task to
  `.superpowers/sdd/<date>-<feature>/progress.md` (what was built, exact
  commands and their last output line, deviations with reasons)
---
### Task N: `<file>.py` — <title>
**Files:**  - Create: … - Modify: `path:lines` (register the test …)
**Interfaces:** - Consumes: … - Produces: exact signatures / tokens
**Notes for the implementer** / numbered "Work in this order" with file:line
- [ ] **Step 1: Write the failing test**   (full listing)
- [ ] **Step 2: Run the test to verify it fails**
  Run: `<command>`   Expected: `<exact error>`
- [ ] **Step 3: Implement …**            (full listing or precise spec)
- [ ] **Step 4: Run the test to verify it passes**
  Run: …  Expected: `five ok: lines then RIGEXEC_UNDO_OK (5 groups)` + how to
  diagnose likely failures
- [ ] **Step 5: Register the test / add the runner / Commit / Report**
### Task N+1: end-to-end test   (runner script + `$…_SHOT` + banner)
### Task N+2: documentation     (`docs/<feature>.md` sections + README bullet)
```

(`plans/2026-09-01-viewport-gizmo-toolbar.md:26-42, 244-249, 485-490,
3012-3100`; `plans/2026-09-02-view-cube.md:1-68`; `plans/2026-09-01-graph-
editor.md:13-24, 24-66`.) The `.superpowers/` directory is not in the
checkout (verified: `ls .superpowers` fails; `.gitignore` has no entry) — it
is a per-run scratch location referenced by the plans only.

---

## 7. Recommendations: brush/comb and map painting for usdGen

### 7.1 Where the in-memory edit state lives during a drag

Two proven options and one recommended hybrid:

**A. "Curvenet-style": author to the stage on every move.** One
`EditRecorder.Begin()` at press over the attributes the brush can touch
(e.g. `<guides>.points`, `<guides>.widths`, or a per-vertex paint primvar),
then per move: compute the new arrays from the press-time base and the
accumulated stroke, write them inside ONE `Sdf.ChangeBlock`, call
`usdviewApi.UpdateViewport()`; `Commit(label)` + `undoStack.Push` at release;
`Abort()` on Escape. This is exactly the gizmo/graph loop (section 2.2) and
requires nothing new in C++. Cost: one `ObjectsChanged` per move → the
usdGen evaluator republishes the affected operators (dirty propagation must
be leaf-precise or this will not be interactive on a real groom), plus
usdview's `_resetGUI` on a 0 ms timer for every notice (`appController.py:
552-555`; only a prim RESYNC rebuilds the prim view, an info-only change is
the cheap branch).

**B. "Snapshot-override": in-memory only until release.** Keep the stroke's
result in the C++ imaging library as a per-prim override on the published
generation (an analogue of `RigExecPublishedPrim.points`/`hasWeightOverlay`
in `snapshotStore.h`), pushed per move through a new C entry point or a
pybind call, e.g. `UsdGenImaging_SetLiveOverride(primPath, const float* xyz,
n)` / `UsdGenImaging_ClearLiveOverride(primPath)`, which publishes a new
generation with only `points`/`widths`/`displayColor` dirtied (leaf-exact
dirtying is what `RigExecSnapshotStore::Publish` + `NotifyGenerationPublished`
already do). No stage notice, no evaluator re-run, no prim-view refresh per
move; the stage is written ONCE at release (still inside an `EditRecorder`
bracket for undo). After the release write, the evaluator republishes from
the stage and the override is cleared in the same call so there is no
double-image frame.

**Recommendation: B for the viewport path, with A's bracket for undo.**
Rationale from the evidence: (1) a comb needs the EVALUATED curve positions
(after the surface deformer and any styler chain) and those live only in the
snapshot — the authored `points` are the rest array (snapping spec:218–233);
(2) usdRig documents that even value edits cost a full re-evaluation per
notice and that prim resyncs recompile; (3) the "primvar appearing is a
resync" trap means any NEW overlay primvar (brush falloff `displayColor`)
must be announced with `PrimsAdded` once, then dirtied — the RigExec overlay
code path (`_announcedWeightOverlays`) is the template.

Concretely for usdGen's Python side:

```python
class BrushController(QtCore.QObject):        # gizmoUI.GizmoController shape
    def _OnPress(self, event):                # gizmoUI.py:2104-2128 rules
        if event.modifiers() & (Alt|Meta): return False   # usdview camera
        hit = self._picker.Pick(x_phys, y_phys)           # curvenetUI.SurfacePicker.Pick
        if hit is None: status "clicked off the model"; return True
        self._recorder = rigExecUndo.EditRecorder(stage, [<attr paths>]); Begin()
        self._base = <read evaluated CVs from the library / terminal SI once>
        self._stroke = []                                 # world hit points
        self._lib.UsdGenImaging_BeginLiveOverride(primPath)
        return True
    def _OnMove(self, event):                 # per move
        hit = self._picker.Pick(...)          # 1.3-1.4 ms budget (spec 3)
        self._stroke.append(hit); new = combKernel(self._base, self._stroke, radius, falloff)
        self._lib.UsdGenImaging_SetLiveOverride(primPath, new)   # publishes generation
        self._overlay.update(); self.api.UpdateViewport()       # required, see 4.3 step 5
        return True
    def _OnRelease(self, event):
        with Sdf.ChangeBlock(): <write final arrays through the writer>
        edit = self._recorder.Commit("Comb %s" % prim.GetName()); undoStack.Push(edit)
        self._lib.UsdGenImaging_ClearLiveOverride(primPath)      # evaluator republishes from stage
        self.api.UpdateViewport()
```

Brush radius: pick the surface point with the stock pick, convert a
screen-space radius to world via `gizmoScreen.WorldPerPixel(camera,
viewport, hitPoint)` (189–200), and select affected guide roots by world
distance on the SURFACE (root positions come from the library, not the
stage). Per-CV picking (for a "select CVs" mode) must be screen-space in
Python (`gizmoSnap.NearestPoint` pattern) or a C++ `HdxPickTask` with
`pickTarget = pickPoints` — the Python engine API cannot request it
(section 3.2).

Undo of a comb needs only attribute snapshots (points/widths arrays) —
`AttributeSnapshot` already captures defaults, splines and time samples
(`rigExecUndo.py:41-59`); a per-frame comb on animated guides would author a
time sample via the `SetAtTime` rule.

Reading the base positions: either (a) a new accessor on the imaging
library (pybind11 with `VtVec3fArray` return is the least friction; the
`_rigexec` module already shows the pybind pattern), or (b) the terminal
scene index through `HydraObserver.GetPrim(path).dataSource.Get("primvars")
.Get("points").Get("primvarValue").GetValue(0.0)` — proven in tests
(`testUsdviewRigExec.py:28-39`) but it copies the array through Python and
would be per-press, not per-move.

### 7.2 Painting a map (per-face / per-vertex values)

Follow the influence-overlay design, inverted: RigExec PUBLISHES a resolved
field as a vertex `displayColor` (`RigExecImaging_SetWeightOverlay`,
`docs/volume-weights.md:280-287`); usdGen would author one. Recommended
representation: a primvar on the surface mesh in the edit target
(`primvars:usdGen:<map>` with `vertex`, `faceVarying` or `uniform`
interpolation, `VtFloatArray`/`VtVec3fArray`), painted with the drag protocol
above (per-move live override of the mesh's `displayColor` in the imaging
library to visualise; one `Sdf.ChangeBlock` write at release; `EditRecorder`
over the primvar attribute path). Because turning the visualisation on adds
a primvar to the mesh, the imaging library must re-announce the prim
(`PrimsAdded` with its upstream type) on on/off, exactly as
`_announcedWeightOverlays` does. Ptex/texture "map prims" would then be
produced by a freeze/bake operator from these primvars (section 8), so the
paint tool never has to write image files during interaction. For picking
the face/vertex under the brush: the stock pick gives `hitPrimPath`,
`hitPoint`, `hitNormal` only; find the face by closest triangle in Python
(`curvenetUI.ClosestPointOnMesh` / `_MeshTriangles`, 1475–1550) or via a
C++ helper — do not rely on `elementIndex` (unavailable through
`UsdImagingGL`).

Two hard rules to carry over: never create/remove prims per move (resync →
recompile → known `RemovePrim` error class, `docs/curvenet.md:506-522`), and
compute every move from the press-time base so the stroke is idempotent and
abortable (`gizmoDrag.py:233-235`).

---

## 8. Recommendations: freeze/bake reading evaluated curves and authoring `UsdGeomBasisCurves`

1. **Source of truth for "what is drawn".** Either the C++ generation
   (`RigExecImagingSnapshot.prims[path]` → `points`, `normals`, `extent`,
   `sampleOffsets/pointsSamples` for motion blur; `generation`, `stage`,
   `sampleTime` with `Describes(stage, time)` — `snapshotStore.h`) or the
   terminal scene index via `HydraObserver` (`GetPrim(path)` → `primType ==
   "basisCurves"`, data sources `basisCurves/topology` (`curveVertexCounts`,
   `basis`, `type`, `wrap`) and `primvars/{points,widths,normals,displayColor}`
   with `interpolation`). usdRig has no C accessor for arrays, so usdGen
   should add one (pybind11 `_usdgen.Imaging.curves(primPath) -> (counts,
   points, widths, …)`), and additionally expose `GetGeneration()` so the
   tool can assert it read the generation it expects.
2. **Author with the stage's edit target, as one undoable unit.**
   `UsdGeom.BasisCurves.Define(stage, path)` +
   `CreateCurveVertexCountsAttr / CreatePointsAttr / CreateWidthsAttr /
   SetWidthsInterpolation / CreateTypeAttr / CreateBasisAttr / CreateWrapAttr`
   (the curvenet display code is a working template: `curvenetUI.py:
   1379-1389`). Do it inside a single `Sdf.ChangeBlock` so the evaluator sees
   one notice. **`EditRecorder` only snapshots ATTRIBUTE specs** (`rigExecUndo.
   py:203-243`); a freeze creates prim specs, so usdGen's undo module needs a
   `PrimSnapshot` (existed? `Sdf.CopySpec` of the whole subtree from/to the
   layer; remove on undo) — extend `rigExecUndo`-style code rather than
   reuse it verbatim. Prim creation is a RESYNC: expect usdview to rebuild
   its prim view (`_resetGUI`) and the evaluator to recompile; the plan
   should make structural recompiles cheap or incremental (RigExec's
   limitation list: "Structural edits currently trigger a full in-memory
   recompile", `RIG/README.md`).
3. **Frozen output must re-enter the graph deliberately.** Author the frozen
   prim with a relationship/metadata the graph reads (e.g. the operator's
   `usdGen:source` relationship re-targeted to the frozen curves, or a
   `usdGen:frozen = true` uniform on the operator), so the evaluator stops
   generating that stage of the chain and the frozen curves become the input
   to the next operator (and to the comb tool). Because the results scene
   index overlays evaluated points ONLY on prims it publishes
   (`RigExecBindingResolvingSceneIndex::BindingEpoch.publishedPrims`), a
   frozen prim not in the published set is drawn from its authored arrays —
   which is the desired behaviour for "modify manually" until a deformer is
   bound to it.
4. **Time.** Freeze at `usdviewApi.dataModel.currentFrame` for a single
   pose, or loop `UsdGenImaging_SetTime(f)` per frame and author time samples
   — the ctypes `SetTime` is synchronous and returns after publication
   (`registry.cpp:343-377`), and `RigExecImaging_GetGeneration()` increments
   per publication (`testUsdviewRigExec.py:157-170`), which is the handshake
   to use. Write time samples with `attr.Set(value, Usd.TimeCode(f))`; do not
   use the spline rule for arrays (`Ts` is scalar-only, `graphModel.py:20-23`).
5. **Where it lands.** The session layer by default (usdview) — show the
   edit-target label the volume panel shows (`volumeWeightUI.py:1675-1691`)
   and offer "write to a new layer / the root layer" in the tool, because a
   frozen groom is exactly the kind of work an artist expects to persist.
6. **Test it the usdRig way.** A `tests/testUsdviewUsdGenFreeze.py` that
   freezes at frame N, then compares the authored `points` with the terminal
   scene index's `primvars/points` for the same prim (`_Points(observer)`
   idiom), runs Ctrl+Z, and asserts the prim spec is gone from the session
   layer; a headless `tests/python/test_usdgen_freeze.py` for the pure
   authoring helper with `_Check` groups and an `USDGEN_FREEZE_OK` banner.

---

## Key facts

- usdview plugin containers are `Tf.Type`-defined subclasses of
  `pxr.Usdviewq.plugin.PluginContainer` discovered via `plugInfo.json`
  (`"Type": "python"`) and instantiated/registered in `loadPlugins`
  (`USD/pxr/usdImaging/usdviewq/plugin.py:292-344`; `RIG/plugin/rigExecUsdview/
  plugInfo.json`; `rigExecUsdview.py:613`).
- Plugins load before the stage and before the `StageView` exists
  (`appController.py:426-432` vs `1863-1882`); usdRig installs viewport tools
  from `signalStageReplaced` with a bounded 0 ms `QTimer` retry
  (`rigExecUsdview.py:533-539`, `gizmoUI.py:3161-3203`).
- The container keeps a module-global strong reference to itself because
  usdview drops containers with no retained callbacks (`rigExecUsdview.py:
  88-98`); every Qt panel is imported lazily inside its menu callback so the
  container stays importable headlessly (`rigExecUsdview.py:230-366`).
- The C++ library is reached via `ctypes.CDLL` on an exported C surface and
  the stage is handed over by `UsdUtils.StageCache` id
  (`rigExecUsdview.py:62-85, 571-574`; entry points in `registry.h`); a
  generation counter `RigExecImaging_GetGeneration()` is the publication
  handshake tests rely on (`tests/testUsdviewRigExec.py:152-170`).
- `RootDataModel.currentFrame` emits `currentFrameChanged` BEFORE assigning
  the property, so handlers must use the signal's frame
  (`UVQ/rootDataModel.py:152-162`; `rigExecUsdview.py:376-395, 600-610`).
- usdview's edit target is the session layer (`appController.py:1283`);
  every panel writes to `stage.GetEditTarget()` and says so
  (`volumeWeightUI.py:1675-1691`).
- Undo = per-attribute spec snapshots in the edit-target layer (default,
  spline copy, time samples; absent spec restored by removal), bracketed by
  `EditRecorder.Begin/Commit/Abort`, bounded stack of 200, one
  `Sdf.ChangeBlock` per restore (`rigExecUndo.py:20-243`).
- Live drags write to the stage on EVERY mouse move inside one
  `Sdf.ChangeBlock`, recomputed from press-time base values, and push one
  `Edit` at release (`gizmoUI.py:3022-3131`, `gizmoMath.py:972-975,
  1318-1368`, `gizmoDrag.py:233-235`; graph editor `graphEditorUI.py:19-26,
  897-940, 1389-1404`).
- After an edit or undo the tool must call `usdviewApi.UpdateViewport()`
  (→ `StageView.updateGL()`): scene-index dirties do not schedule a Qt
  repaint (`gizmoUI.py:1851-1862, 3095`; `volumeWeightUI.py:2265-2277`;
  `usdviewApi.py:221-225`).
- The results scene index republishes synchronously from inside the
  `ObjectsChanged` dispatch; its listener is registered BEFORE `Compile()` so
  it runs after OpenExec's invalidation listener (`registry.cpp:246-266,
  423-471`; `docs/volume-weights.md:328-378`).
- A primvar appearing on an already-synced rprim must be re-announced with
  `PrimsAdded`; a dirty is not enough (`sceneIndices.h` `_announcedWeightOverlays`
  comment; `docs/volume-weights.md:289-311`).
- Prim add/remove per redraw is a resync → full recompile, and `RemovePrim`
  on a rigged stage posts a `Usd_PrimFlagsPredicate` error; viewport aids are
  defined once and toggled with `visibility` (`curvenetUI.py:1320-1336`;
  `docs/curvenet.md:506-522`; `tests/python/test_rigexec_stage_edits.py:1-15`).
- Viewport picking: `view.computePickFrustum(x_phys, y_phys)` + `view.pick(
  frustum)` returns `hitPrimPath/hitPoint/hitNormal`; `pickObject`'s
  `signalPrimSelected` overwrites the point with mouse coords (`stageView.py:
  2224-2367`; `curvenetUI.py:13-21, 530-553`). Qt logical pixels must be
  scaled by `devicePixelRatioF()` (`stageView.py:2091-2094`; `curvenetUI.py:
  846-869`; `gizmoUI.py:2075-2091`).
- `UsdImagingGL.Engine.PickParams` exposes only `resolveMode`, and
  `IntersectionResult` has no element/edge/point index; `pickTarget` is never
  set by the engine (`engine.h:322-364`; `wrapEngine.cpp:218-233`;
  `engine.cpp:1243-1249`) although Hdx supports `pickPoints/pickEdges/pickFaces`
  (`hdx/pickTask.h:51-55, 82-107`). Component picking in usdRig is
  screen-space in Python (`curvenetUI.py:901-964`; `gizmoSnap.py:212-298`).
- When guides are forced on (`rigExecUsdview.py:583-590`), `view.pick()`
  returns guides; the gizmo bypasses it with a fresh `RenderParams(showGuides
  =False)` and `view._getRenderer().TestIntersection` (`gizmoUI.py:2456-2542`);
  measured ~1.3–1.4 ms per pick, 5.7–18 ms Hydra sync after topology
  changes (`specs/2026-09-03-gizmo-snapping-design.md:129-173`).
- Overlays are transparent mouse-transparent child widgets of the
  `QOpenGLWidget` stage view (`gizmoUI.py:16-23, 182-203`), or
  mouse-accepting ones for controls (`viewCubeUI.py:15-25`); `paintGL`
  re-arms every 5 ms until converged so Paint hooks must be cheap
  (`stageView.py:1877-1878`; `gizmoUI.py:2025-2035`).
- `resolveCamera()` emits `signalFrustumChanged` as a side effect; call it
  only when a camera is needed and guard re-entry (`stageView.py:1589-1628`;
  `gizmoUI.py:1477-1486, 1517-1537`).
- Tools never claim `Alt`/`Meta` presses and return `False` to let usdview
  pick when they have nothing under the cursor; keys are read from an
  application-level event filter (`stageView.py:2096-2112`; `gizmoUI.py:
  25-64, 2104-2115`).
- testusdview scripts define `testUsdviewInputFunction(appController)`, run
  with `--testScript`, a 597x540 viewport, `--defaultsettings`, and
  `appController._processEvents()`; runners are `exec "$PY" "$TESTUSDVIEW"
  --testScript … [--renderer NAME] "$STAGE"` (`USD/pxr/usdImaging/bin/
  testusdview/testusdview.py:23-140`; `bin/run_testusdview_gizmo.sh:14-33`).
- Tests read the terminal scene index with `pxr.Usdviewq._usdviewq.
  HydraObserver` (`TargetToNamedSceneIndex(names[-1])`, `GetPrim`)
  and assert on stage values, session-layer specs and framebuffer pixel
  fractions (`tests/testUsdviewRigExec.py:16-39`; `testUsdviewVolumeWeightOverlay.
  py:58-162`; `hydraObserver.h:44-139`).
- Headless tests are plain scripts (`_Check`, `(name, callable)` groups,
  `RIGEXEC_*_OK` banner) bootstrapped by `rigexec_test_env.SetupPluginTest()`
  and registered in CMake with a single-entry `PYTHONPATH` plus
  `PXR_PLUGINPATH_NAME`/`RIGEXEC_USD_INSTALL` (`tests/python/rigexec_test_env.
  py:42-91`; `CMakeLists.txt:312-382`; `bin/run_python_tests.sh`).
- Specs follow `0 Request / 1 Assumptions / 2 Facts (with file:line and
  measurements) / 3 Architecture / 4 Data flow / 5 Error handling / 6
  Testing / 7 Out of scope`; plans follow `Goal / Architecture / Tech Stack
  / Spec / Global Constraints / ### Task N (Files, Interfaces, - [ ] Steps
  with Run:/Expected:)` and report to `.superpowers/sdd/<date>-<feature>/
  progress.md` (`specs/2026-09-01-viewport-gizmo-toolbar-design.md:1-221`;
  `plans/2026-09-03-gizmo-snapping.md:1-63, 395-621`).
- `bin/_env.sh` hard-codes `lib/python3.11/site-packages` and sibling
  `usd-install`/`usd-pr4156-venv` (`_env.sh:12-22`); the install rule ships
  only four of the plugin's modules (`CMakeLists.txt:476-481`).

## Open questions

- Absolute per-move cost of a rig re-evaluation on `ArmShotAnim.usda` (the
  spec gives only the pick timings and the 16.7 ms budget); no profile of
  `SetTime` was found in the repo.
- Whether `UsdImagingGLEngine` exposes enough (render index / task
  controller scene index) for a plugin-side C++ `HdxPickTask` with
  `pickTarget = pickPoints` on the same engine; `GetRenderIndex()` appears
  in `engine.h` but the picking-task wiring (`_taskControllerSceneIndex->
  GetPickingTaskPaths()`, `engine.cpp:1257`) was not traced further.
- Whether the checked-in POSIX helpers run unmodified on this Linux/3.12
  install (README marks Linux unverified; `_env.sh` hard-codes 3.11).
- How Storm's `HdSceneIndexAdapterSceneDelegate` treats a `points` primvar
  whose ELEMENT COUNT changes without a topology change (relevant to a live
  override that resamples curves) — not exercised by usdRig, which keeps
  counts fixed within a generation.
- The exact behaviour of `HydraObserver.GetPrim` for prims synthesized by a
  scene index (no `primOrigin`) when used for freeze: paths are hydra paths
  and, with `usdview`'s scene-index path, should equal stage paths, but
  native-instancing/prototype propagation renames were not checked.
- Whether `EditRecorder`-style undo can be extended to prim specs safely on
  a stage where the evaluator recompiles on resync (undoing a freeze removes
  a prim → the same `RemovePrim` error class `docs/curvenet.md:513-522`
  reports on rigged stages; the root cause is described as unlocated).
