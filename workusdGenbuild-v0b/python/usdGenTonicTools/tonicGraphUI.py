# usdGenTonicTools.tonicGraphUI -- Graph-mode viewport controller (P2).
#
# Qt lives here and only here (the 08-tools.md Qt-free rule): an event filter
# over the usdview StageView plus the per-sub-mode press/move/release loops
# from plan/17 section 5.1. All geometry stays in C++ (tonicLib); this module
# passes picks and strokes across and refreshes the status/HUD after every
# gesture. Import this module only from a menu callback.
from __future__ import annotations

import ctypes
import os
import tempfile


def _surfacePick(usdviewApi, x, y):
    """Pick the stage at viewport pixel (x, y); returns a hit point or None.

    Uses the stock Hydra pick (hitPoint only; face/uv come from the model's
    own closest-point, since TestIntersection reports no element index —
    plan/08 section 1.4's ClosestSurfacePoint note owns the same caveat).
    """
    try:
        view = usdviewApi.stageView
    except AttributeError:
        return None
    try:
        pick = view.pick(x, y)
    except Exception:
        return None
    if pick is None:
        return None
    point = getattr(pick, "hitPoint", None)
    if point is None and isinstance(pick, dict):
        point = pick.get("hitPoint")
    if point is None:
        return None
    try:
        return (float(point[0]), float(point[1]), float(point[2]))
    except (TypeError, ValueError, IndexError):
        return None


class GraphController:
    """Owns the model/committer/bake handles and the gesture state."""

    def __init__(self, state, usdviewApi):
        self._state = state
        self._usdviewApi = usdviewApi
        self._model = None
        self._committer = None
        self._bake = None
        self._liveId = ""
        self._groomPath = "/TonicGroom"
        self._descPath = ""
        self._scalpPath = ""
        self._mapPath = "/TonicGroom/RegionMap"
        self._stroke = []  # (face, u, v) samples of the live Draw stroke
        self._lastXY = None
        self._dragNode = -1
        self._clickNode = -1
        self._clickRegion = -1
        self._gestureActive = False

    # -- lifetime --------------------------------------------------------

    @property
    def model(self):
        return self._model

    def _status(self, text):
        try:
            self._usdviewApi.PrintStatus(text)
        except AttributeError:
            pass

    def _refresh(self):
        try:
            self._usdviewApi.UpdateViewport()
        except AttributeError:
            pass

    def _hud(self):
        from .tonicGraph import hudStatus
        dll = self._state.lib.dll
        nodes = ctypes.c_int(0)
        edges = ctypes.c_int(0)
        regions = ctypes.c_int(0)
        uncovered = ctypes.c_int(0)
        intersected = ctypes.c_int(0)
        dll.Tonic_GetGraphCounts(self._model, ctypes.byref(nodes),
                                 ctypes.byref(edges), ctypes.byref(regions))
        dll.Tonic_GetRegionStats(self._model, None, ctypes.byref(uncovered),
                                 ctypes.byref(intersected))
        self._status(hudStatus((nodes.value, edges.value, regions.value),
                               (regions.value, uncovered.value,
                                intersected.value),
                               self._state.mapVersion,
                               self._state.bakedVersion))

    def activate(self, scalpPath, groomPath="/TonicGroom", descPath=""):
        """Create the model, bind the stage scalp, start commit + bake."""
        from pxr import Sdf
        state = self._state
        dll = state.lib.dll
        model = ctypes.c_void_p(None)
        if dll.Tonic_Create(ctypes.byref(model)) != 0:
            self._status("Tonic Graph: " + state.lib.lastError())
            return False
        self._model = model
        self._groomPath = groomPath or "/TonicGroom"
        self._descPath = descPath or ""
        self._scalpPath = scalpPath
        self._mapPath = self._groomPath + "/RegionMap"
        if not self._bindScalpFromStage(scalpPath):
            dll.Tonic_Destroy(model)
            self._model = None
            return False
        dll.Tonic_SetSnapRadius(model, ctypes.c_float(0.05))
        dll.Tonic_SetMirrorX(model, 1 if state.mirrorX else 0)
        committer = ctypes.c_void_p(None)
        if dll.Tonic_CommitterCreate(
                model, self._groomPath.encode("utf-8"),
                self._descPath.encode("utf-8") if self._descPath else None,
                ctypes.byref(committer)) != 0:
            self._status("Tonic Graph: " + state.lib.lastError())
            dll.Tonic_Destroy(model)
            self._model = None
            return False
        self._committer = committer
        dll.Tonic_CommitterSetScalpPath(committer, scalpPath.encode("utf-8"))
        live = Sdf.Layer.CreateAnonymous("usdGenTonic-live")
        self._liveId = live.identifier
        stage = self._usdviewApi.stage
        stage.GetSessionLayer().InsertSubLayerPath(self._liveId, 0)
        bake = ctypes.c_void_p(None)
        outDir = os.path.join(tempfile.gettempdir(), "usdGenTonicBake")
        if dll.Tonic_BakeCreate(model, outDir.encode("utf-8"), None,
                                ctypes.byref(bake)) != 0:
            self._status("Tonic Graph: " + state.lib.lastError())
            return False
        self._bake = bake
        state.activated = True
        self._hud()
        self._refresh()
        return True

    def _bindScalpFromStage(self, scalpPath):
        state = self._state
        dll = state.lib.dll
        stage = self._usdviewApi.stage
        prim = stage.GetPrimAtPath(scalpPath)
        if not prim:
            self._status("Tonic Graph: no prim at %s" % scalpPath)
            return False
        points = prim.GetAttribute("points").Get() or []
        counts = prim.GetAttribute("faceVertexCounts").Get() or []
        indices = prim.GetAttribute("faceVertexIndices").Get() or []
        flat = [float(c) for p in points for c in (p[0], p[1], p[2])]
        pts = (ctypes.c_float * len(flat))(*flat)
        cnt = (ctypes.c_int * len(counts))(*[int(c) for c in counts])
        idx = (ctypes.c_int * len(indices))(*[int(i) for i in indices])
        if dll.Tonic_BindScalp(self._model, pts, len(flat), cnt, len(counts),
                               idx, len(indices)) != 0:
            self._status("Tonic Graph: " + state.lib.lastError())
            return False
        return True

    def deactivate(self):
        dll = self._state.lib.dll if self._state.lib else None
        if dll is not None:
            if self._bake is not None:
                dll.Tonic_BakeDestroy(self._bake)
            if self._committer is not None:
                dll.Tonic_CommitterDestroy(self._committer)
            if self._model is not None:
                dll.Tonic_Destroy(self._model)
        self._model = None
        self._committer = None
        self._bake = None
        self._state.activated = False

    # -- gesture plumbing -------------------------------------------------

    def _locate(self, point):
        """Closest-point (face, u, v) for a stage hit point, or None."""
        dll = self._state.lib.dll
        p = (ctypes.c_float * 3)(*point)
        hit = ctypes.c_int(0)
        face = ctypes.c_int(-1)
        uv = (ctypes.c_float * 2)(0.0, 0.0)
        xyz = (ctypes.c_float * 3)(0.0, 0.0, 0.0)
        nrm = (ctypes.c_float * 3)(0.0, 1.0, 0.0)
        if dll.Tonic_ClosestPoint(self._model, p, ctypes.byref(hit),
                                  ctypes.byref(face), uv, xyz, nrm) != 0:
            return None
        if not hit.value:
            return None
        return (face.value, float(uv[0]), float(uv[1]))

    def _snapRest(self):
        """Snap radius in rest units for the current camera distance."""
        from .tonicGraph import pixelsToRest
        try:
            view = self._usdviewApi.stageView
            camera = view.resolveCamera()
            frustum = camera.frustum
            dist = float(frustum.position.GetLength())
            height = int(view.height or 1080)
            import math
            fov = math.degrees(
                2.0 * math.atan(1.0 / float(frustum.projectionMatrix[1][1])))
        except Exception:
            return 0.05
        return pixelsToRest(self._state.snapRadiusPx, dist, height, fov)

    def _releaseCommon(self):
        """End-of-gesture: rasterise, enqueue commit + bake, HUD, repaint."""
        from . import tonicLib
        dll = self._state.lib.dll
        if dll.Tonic_Rasterise(self._model) != 0:
            self._status("Tonic Graph: " + self._state.lib.lastError())
            return
        if dll.Tonic_CommitterEnqueue(self._committer, 0, 0, 0, None) \
                != tonicLib.TONIC_OK:
            self._status("Tonic Graph: " + self._state.lib.lastError())
        if dll.Tonic_BakeEnqueue(self._bake) == tonicLib.TONIC_OK:
            self._state.mapVersion = int(
                dll.Tonic_GetMapVersion(self._model))
        self._gestureActive = False
        self._hud()
        self._refresh()
        self._pumpIdle()

    def _pumpIdle(self):
        """One idle slot: committer swap + bake swap (never during a drag)."""
        from . import tonicLib
        dll = self._state.lib.dll
        dll.Tonic_CommitterSwap(self._committer,
                                self._liveId.encode("utf-8"), 0)
        version = ctypes.c_ulonglong(0)
        path = ctypes.create_string_buffer(4096)
        while dll.Tonic_BakeTakeCompleted(self._bake, ctypes.byref(version),
                                          path, 4096) == 1:
            filePath = path.value.decode("utf-8")
            if dll.Tonic_BakeSwap(self._bake, version,
                                  self._liveId.encode("utf-8"),
                                  self._mapPath.encode("utf-8"),
                                  filePath.encode("utf-8")) \
                    == tonicLib.TONIC_OK:
                self._state.bakedVersion = int(version.value)
        self._hud()

    def scheduleIdle(self):
        """Ask Qt for an idle slot (QTimer.singleShot(0), bounded)."""
        from pxr.Usdviewq.qt import QtCore
        QtCore.QTimer.singleShot(0, self._pumpIdle)

    # -- event filter -----------------------------------------------------

    def install(self, stageView):
        from pxr.Usdviewq.qt import QtCore
        controller = self

        class _Filter(QtCore.QObject):
            def eventFilter(self, _obj, event):
                try:
                    t = event.type()
                except Exception:
                    return False
                if t == QtCore.QEvent.MouseButtonPress:
                    return controller.onPress(event.x(), event.y())
                if t == QtCore.QEvent.MouseMove:
                    return controller.onMove(event.x(), event.y())
                if t == QtCore.QEvent.MouseButtonRelease:
                    return controller.onRelease(event.x(), event.y())
                return False

        self._filter = _Filter(stageView)
        stageView.installEventFilter(self._filter)

    def onPress(self, x, y):
        if self._model is None or self._state.activeMode != "graph":
            return False
        point = _surfacePick(self._usdviewApi, x, y)
        if point is None:
            return False
        sub = self._state.graphSubMode or "draw"
        self._gestureActive = True
        self._lastXY = (x, y)
        if sub == "draw":
            loc = self._locate(point)
            self._stroke = [loc] if loc else []
            return True
        if sub == "place":
            return self._pressPlace(point)
        if sub in ("connect", "weld"):
            return self._pressClickNode(point, sub)
        if sub == "unweld":
            return self._pressUnweld(point)
        if sub == "delete":
            return self._pressDelete(point)
        if sub == "link":
            return self._pressLink(point)
        return False

    def onMove(self, x, y):
        if self._model is None or not self._gestureActive:
            return False
        sub = self._state.graphSubMode or "draw"
        if sub == "draw":
            if self._lastXY is not None and \
                    abs(x - self._lastXY[0]) + abs(y - self._lastXY[1]) < 3:
                return True
            self._lastXY = (x, y)
            point = _surfacePick(self._usdviewApi, x, y)
            if point is None:
                return True
            loc = self._locate(point)
            if loc:
                self._stroke.append(loc)
            return True
        if sub == "place" and self._dragNode >= 0:
            point = _surfacePick(self._usdviewApi, x, y)
            if point is None:
                return True
            self._lastDropPoint = point
            loc = self._locate(point)
            if loc:
                self._state.lib.dll.Tonic_GraphMoveNode(
                    self._model, self._dragNode, loc[0],
                    ctypes.c_float(loc[1]), ctypes.c_float(loc[2]))
                self._refresh()
            return True
        return False

    def onRelease(self, x, y):
        if self._model is None or not self._gestureActive:
            return False
        sub = self._state.graphSubMode or "draw"
        handled = True
        if sub == "draw":
            point = _surfacePick(self._usdviewApi, x, y)
            if point is not None:
                loc = self._locate(point)
                if loc:
                    self._stroke.append(loc)
            self._commitStroke()
        elif sub == "place" and self._dragNode >= 0:
            self._finishPlaceDrag()
        self._stroke = []
        self._lastXY = None
        self._dragNode = -1
        if sub in ("draw", "place"):
            self._releaseCommon()
        else:
            self._gestureActive = False
        return handled

    # -- sub-mode actions ---------------------------------------------------

    def _commitStroke(self):
        if len(self._stroke) < 2:
            return
        dll = self._state.lib.dll
        faces = (ctypes.c_int * len(self._stroke))(
            *[s[0] for s in self._stroke])
        uvs = (ctypes.c_float * (2 * len(self._stroke)))(
            *[c for s in self._stroke for c in (s[1], s[2])])
        out = (ctypes.c_int * 4096)()
        count = ctypes.c_int(0)
        closed = ctypes.c_int(0)
        ws = ctypes.c_int(0)
        we = ctypes.c_int(0)
        if dll.Tonic_GraphStroke(
                self._model, faces, uvs, len(self._stroke),
                ctypes.c_float(self._snapRest()), ctypes.c_float(0.01),
                out, 4096, ctypes.byref(count), ctypes.byref(closed),
                ctypes.byref(ws), ctypes.byref(we)) != 0:
            self._status("Tonic Graph: " + self._state.lib.lastError())

    def _pressPlace(self, point):
        from . import tonicLib
        dll = self._state.lib.dll
        p = (ctypes.c_float * 3)(*point)
        node = dll.Tonic_GraphSnapNode(self._model, p,
                                       ctypes.c_float(self._snapRest()))
        if node >= 0:
            self._dragNode = node  # drag an existing node
            return True
        loc = self._locate(point)
        if loc is None:
            return True
        nodeId = ctypes.c_int(-1)
        if dll.Tonic_GraphAddNode(self._model, loc[0],
                                  ctypes.c_float(loc[1]),
                                  ctypes.c_float(loc[2]),
                                  ctypes.byref(nodeId)) == tonicLib.TONIC_OK:
            self._dragNode = nodeId.value
        self._refresh()
        return True

    def _finishPlaceDrag(self):
        """A drag that ends on another node/edge welds (plan/17 section 5.1).

        The drop target is found by re-snapping at the dragged position
        (never by snapshot order: ReadGraphNodes reports snapshot order,
        not stable ids).
        """
        dll = self._state.lib.dll
        point = getattr(self, "_lastDropPoint", None)
        if point is not None:
            p = (ctypes.c_float * 3)(*point)
            other = dll.Tonic_GraphSnapNode(self._model, p,
                                            ctypes.c_float(self._snapRest()))
            if other >= 0 and other != self._dragNode:
                dll.Tonic_GraphWeld(self._model, other, self._dragNode)
                self._dragNode = other
                return
            edge = dll.Tonic_GraphSnapEdge(self._model, p,
                                           ctypes.c_float(self._snapRest()))
            if edge >= 0:
                loc = self._locate(point)
                if loc is not None:
                    mid = ctypes.c_int(-1)
                    if dll.Tonic_GraphSplitEdge(
                            self._model, edge, loc[0],
                            ctypes.c_float(loc[1]), ctypes.c_float(loc[2]),
                            ctypes.byref(mid)) == 0:
                        dll.Tonic_GraphWeld(self._model, mid.value,
                                            self._dragNode)
                        self._dragNode = mid.value

    def _pressClickNode(self, point, action):
        from . import tonicLib
        dll = self._state.lib.dll
        p = (ctypes.c_float * 3)(*point)
        node = dll.Tonic_GraphSnapNode(self._model, p,
                                       ctypes.c_float(self._snapRest()))
        if node < 0:
            self._status("Tonic Graph %s: no node here" % action)
            return True
        if self._clickNode < 0:
            self._clickNode = node
            self._status("Tonic Graph %s: node %d ..." % (action, node))
            return True
        first, self._clickNode = self._clickNode, -1
        if first == node:
            return True
        if action == "connect":
            edge = ctypes.c_int(-1)
            ok = dll.Tonic_GraphConnect(self._model, first, node,
                                        ctypes.byref(edge)) \
                == tonicLib.TONIC_OK
            self._status("Tonic Graph: connected (%s)" %
                         ("ok" if ok else self._state.lib.lastError()))
        else:
            ok = dll.Tonic_GraphWeld(self._model, first, node) \
                == tonicLib.TONIC_OK
            self._status("Tonic Graph: welded (%s)" %
                         ("ok" if ok else self._state.lib.lastError()))
        self._releaseCommon()
        return True

    def _pressUnweld(self, point):
        dll = self._state.lib.dll
        p = (ctypes.c_float * 3)(*point)
        node = dll.Tonic_GraphSnapNode(self._model, p,
                                       ctypes.c_float(self._snapRest()))
        if node < 0:
            return True
        out = (ctypes.c_int * 64)()
        count = ctypes.c_int(0)
        dll.Tonic_GraphUnweld(self._model, node, out, 64, ctypes.byref(count))
        self._status("Tonic Graph: unwelded into %d" % (count.value + 1))
        self._releaseCommon()
        return True

    def _pressDelete(self, point):
        from . import tonicLib
        dll = self._state.lib.dll
        p = (ctypes.c_float * 3)(*point)
        snap = ctypes.c_float(self._snapRest())
        node = dll.Tonic_GraphSnapNode(self._model, p, snap)
        if node >= 0:
            ok = dll.Tonic_GraphDeleteNode(self._model, node) \
                == tonicLib.TONIC_OK
            self._status("Tonic Graph: node deleted (%s)" %
                         ("ok" if ok else self._state.lib.lastError()))
            self._releaseCommon()
            return True
        edge = dll.Tonic_GraphSnapEdge(self._model, p, snap)
        if edge >= 0:
            ok = dll.Tonic_GraphDeleteEdge(self._model, edge) \
                == tonicLib.TONIC_OK
            self._status("Tonic Graph: edge deleted, regions merged (%s)" %
                         ("ok" if ok else self._state.lib.lastError()))
            self._releaseCommon()
            return True
        return True

    def _pressLink(self, point):
        from . import tonicLib
        dll = self._state.lib.dll
        loc = self._locate(point)
        if loc is None:
            return True
        count = ctypes.c_int(0)
        dll.Tonic_ReadFaceRegionIds(self._model, None, 0, ctypes.byref(count))
        n = max(int(count.value), 0)
        if loc[0] < 0 or loc[0] >= n:
            return True
        ids = (ctypes.c_int * n)()
        got = ctypes.c_int(0)
        if dll.Tonic_ReadFaceRegionIds(self._model, ids, n,
                                       ctypes.byref(got)) != tonicLib.TONIC_OK:
            return True
        region = int(ids[loc[0]])
        if region < 0:
            self._status("Tonic Graph link: no region here")
            return True
        if self._clickRegion < 0:
            self._clickRegion = region
            self._status("Tonic Graph link: region %d ..." % region)
            return True
        first, self._clickRegion = self._clickRegion, -1
        if first == region:
            return True
        ok = dll.Tonic_GraphLinkRegions(self._model, first, region) \
            == tonicLib.TONIC_OK
        self._status("Tonic Graph: linked (%s)" %
                     ("ok" if ok else self._state.lib.lastError()))
        self._releaseCommon()
        return True

    # -- shelf actions --------------------------------------------------------

    def weldAll(self):
        from . import tonicLib
        dll = self._state.lib.dll
        welds = ctypes.c_int(0)
        if dll.Tonic_GraphWeldAll(self._model,
                                  ctypes.c_float(self._snapRest()),
                                  ctypes.byref(welds)) == tonicLib.TONIC_OK:
            self._status("Tonic Graph: %d welds" % welds.value)
            self._releaseCommon()

    def rebake(self):
        from . import tonicLib
        dll = self._state.lib.dll
        if dll.Tonic_BakeEnqueue(self._bake) == tonicLib.TONIC_OK:
            self._state.mapVersion = int(dll.Tonic_GetMapVersion(self._model))
            self._status("Tonic Graph: rebaking map v%d ..."
                         % self._state.mapVersion)
            self.scheduleIdle()

    def toggleMirror(self):
        dll = self._state.lib.dll
        on = not self._state.mirrorX
        self._state.mirrorX = on
        dll.Tonic_SetMirrorX(self._model, 1 if on else 0)
        self._status("Tonic Graph: mirror-X %s" % ("on" if on else "off"))
