#!/usr/bin/env python
# testUsdGenPomadeToolsLoops -- T0 for the V2 viewport controller's Qt-free
# half (plan/18 section 4 "T0"): the camera maths, the hotkey table, the
# gizmo state machine and GraphLoop driven end to end over a fake session
# that records every C call.
#
#   python plugin/usdGenPomadeTools/testenv/testUsdGenPomadeToolsLoops.py
#
# No usdview, no pxr, no DLL, no numpy: the modules under test are the ones
# plan/08 section 1.2 keeps Qt-free, and this is the test that keeps them
# honest about it. What it proves that a T3 cannot:
#
#   * every stroke sample costs one Pomade_Raycast and zero Hydra picks
#     (plan/18 finding F6), and every node/edge/region hit is a K11
#     Pomade_PickItem;
#   * a drag is exactly one undo bracket: Begin at press, End at release,
#     Cancel on Escape, and nothing committed in between;
#   * the stroke reaches the ABI with a snap radius the camera converted
#     from the panel's pixels, so the same 8 px means the same thing at
#     any zoom;
#   * the plan/18 section 3.4 key table answers what the table says,
#     including the rows that must NOT be claimed (Alt, F, typing).
import ctypes
import math
import os
import sys
import types

failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def near(a, b, eps=1e-4):
    return abs(float(a) - float(b)) <= eps


def _deref(ref):
    """The object behind a ctypes.byref() the fake DLL was handed."""
    return getattr(ref, "_obj", ref)


# ---------------------------------------------------------------------------
# Fakes
# ---------------------------------------------------------------------------

class FakeDll:
    """Records every entry point the loops call; returns POMADE_OK."""

    def __init__(self):
        self.calls = []
        self.snapRadius = 0.05
        self.nextNodeId = 100
        self.strokeNodes = 4
        self.strokeClosed = 1
        self.faceRegionIds = [0, 0, 1, 1]
        self.surfaceRegionFn = lambda _face, _u, _v: -1
        self.graphNodeHits = {}
        self.graphNodeDisplayHits = {}
        self.graphEdges = {}
        self.closestMiss = False
        self.batchReject = False
        # Nodes Pomade_GraphUnweld creates; 0 is a node no second region
        # shares (nothing to split).
        self.unweldCreated = 2

    # -- bookkeeping -------------------------------------------------------

    def _record(self, name, args=()):
        self.calls.append((name, args))

    def names(self):
        return [name for name, _args in self.calls]

    def count(self, name):
        return sum(1 for entry, _args in self.calls if entry == name)

    def argsOf(self, name):
        return [args for entry, args in self.calls if entry == name]

    def reset(self):
        self.calls = []

    # -- explicit entries (the ones with out-parameters) -------------------

    def Pomade_GetSnapRadius(self, _model):
        self._record("Pomade_GetSnapRadius")
        return self.snapRadius

    def Pomade_GraphStroke(self, _model, faces, uvs, samples, snap, eps, out,
                          _maxOut, count, closed, weldStart, weldEnd):
        self._record("Pomade_GraphStroke",
                     (samples, snap.value, eps.value,
                      [faces[i] for i in range(samples)]))
        for i in range(self.strokeNodes):
            out[i] = i
        _deref(count).value = self.strokeNodes
        _deref(closed).value = self.strokeClosed
        _deref(weldStart).value = 0
        _deref(weldEnd).value = 0
        return 0

    def Pomade_GraphCreateRegion(self, _model, nodeIds, faces, uvs, count,
                                outRegion):
        self._record("Pomade_GraphCreateRegion", (
            [nodeIds[i] for i in range(count)],
            [faces[i] for i in range(count)],
            [uvs[i * 2 + axis] for i in range(count) for axis in (0, 1)]))
        _deref(outRegion).value = 0
        return 0

    def Pomade_GraphGetNode(self, _model, nodeId, outFace, outUV, outP):
        hit = self.graphNodeHits.get(int(nodeId))
        if hit is None:
            return 1
        _deref(outFace).value = hit[0]
        outUV[0], outUV[1] = hit[1], hit[2]
        outP[0], outP[1], outP[2] = hit[3]
        return 0

    def Pomade_GraphGetNodeDisplayPosition(self, _model, nodeId, outP):
        point = self.graphNodeDisplayHits.get(int(nodeId))
        if point is None:
            return 1
        outP[0], outP[1], outP[2] = point
        return 0

    def Pomade_RegionAtSurface(self, _model, face, u, v):
        self._record("Pomade_RegionAtSurface", (face, u.value, v.value))
        return self.surfaceRegionFn(int(face), float(u.value), float(v.value))

    def Pomade_GraphAddNode(self, _model, face, u, v, outId):
        self.nextNodeId += 1
        self._record("Pomade_GraphAddNode", (face, u.value, v.value,
                                            self.nextNodeId))
        _deref(outId).value = self.nextNodeId
        return 0

    def Pomade_GraphMoveNode(self, _model, nodeId, face, u, v):
        self._record("Pomade_GraphMoveNode", (nodeId, face, u.value, v.value))
        return 0

    def Pomade_GraphGetEdge(self, _model, edgeId, outNodeIds):
        endpoints = self.graphEdges.get(int(edgeId))
        self._record("Pomade_GraphGetEdge", (int(edgeId),))
        if endpoints is None:
            return 1
        outNodeIds[0], outNodeIds[1] = endpoints
        return 0

    def Pomade_ClosestPoint(self, _model, point, outHit, outFace, outUV,
                           outP, outN):
        query = tuple(float(point[i]) for i in range(3))
        self._record("Pomade_ClosestPoint", query)
        if self.closestMiss:
            _deref(outHit).value = 0
            return 0
        # The test scalp is the y=0 plane.  Its face/UV convention mirrors
        # FakeSession.raycast so the batch assertion can inspect no-jump UVs.
        x, _y, z = query
        ix = min(max(int(math.floor(x)), 0), 3)
        iz = min(max(int(math.floor(z)), 0), 3)
        _deref(outHit).value = 1
        _deref(outFace).value = ix * 4 + iz
        outUV[0], outUV[1] = z - iz, x - ix
        outP[0], outP[1], outP[2] = x, 0.0, z
        outN[0], outN[1], outN[2] = 0.0, 1.0, 0.0
        return 0

    def Pomade_GraphMoveNodes(self, _model, nodeIds, faces, uvs, count):
        payload = ([int(nodeIds[i]) for i in range(count)],
                   [int(faces[i]) for i in range(count)],
                   [float(uvs[2 * i + axis])
                    for i in range(count) for axis in (0, 1)])
        self._record("Pomade_GraphMoveNodes", payload)
        return 1 if self.batchReject else 0

    def Pomade_GraphWeld(self, _model, keep, drop):
        self._record("Pomade_GraphWeld", (keep, drop))
        return 0

    def Pomade_GraphConnect(self, _model, a, b, outEdge):
        self._record("Pomade_GraphConnect", (a, b))
        _deref(outEdge).value = 7
        return 0

    def Pomade_GraphSplitEdge(self, _model, edgeId, face, u, v, outNode):
        self._record("Pomade_GraphSplitEdge", (edgeId, face))
        _deref(outNode).value = 55
        return 0

    def Pomade_GraphUnweld(self, _model, nodeId, _out, _cap, count):
        self._record("Pomade_GraphUnweld", (nodeId,))
        _deref(count).value = self.unweldCreated
        return 0

    def Pomade_GraphDeleteNode(self, _model, nodeId):
        self._record("Pomade_GraphDeleteNode", (nodeId,))
        return 0

    def Pomade_GraphDeleteEdge(self, _model, edgeId):
        self._record("Pomade_GraphDeleteEdge", (edgeId,))
        return 0

    def Pomade_GraphLinkRegions(self, _model, r0, r1):
        self._record("Pomade_GraphLinkRegions", (r0, r1))
        return 0

    def Pomade_GraphWeldAll(self, _model, radius, outWelds):
        self._record("Pomade_GraphWeldAll", (radius.value,))
        _deref(outWelds).value = 3
        return 0

    def Pomade_ReadFaceRegionIds(self, _model, out, maxOut, count):
        self._record("Pomade_ReadFaceRegionIds")
        _deref(count).value = len(self.faceRegionIds)
        if out is not None:
            for i in range(min(maxOut, len(self.faceRegionIds))):
                out[i] = self.faceRegionIds[i]
        return 0

    # -- everything else ---------------------------------------------------

    def __getattr__(self, name):
        if name.startswith("_") or not name.startswith("Pomade_"):
            raise AttributeError(name)

        def entry(*args):
            self._record(name, args)
            return 0
        return entry


class FakeSession:
    """The PomadeSession surface the loops use, recorded."""

    def __init__(self, dll):
        self.dll = dll
        self.model = "model"
        self.ctx = self.model
        self.events = []
        self.statuses = []
        self.rays = []
        self.picks = []
        self.hovers = []
        self.rects = []
        self.published = []
        self.gestureStack = []
        self.pickFn = lambda mask, x, y: None
        self.selection = {}
        self.surfaceMisses = False
        self.rasteriseOk = True
        # Where the controller measures the display scale (plan/18
        # section 2.4a); None stands for "no scalp bound yet".
        self.scalpCenter = (2.0, 0.0, 2.0)
        self.displayScales = []

    # -- status ------------------------------------------------------------

    def report(self, text):
        self.statuses.append(text)

    # -- display scale -----------------------------------------------------

    def setDisplayScale(self, worldPerPixel):
        self.displayScales.append(float(worldPerPixel))
        return True

    def lastError(self):
        return "fake error"

    def fallbackReason(self):
        return ""

    def graphCounts(self):
        return (4, 4, 1)

    def regionStats(self):
        return (1, 8, 0)

    # -- gestures ----------------------------------------------------------

    def beginGesture(self, label):
        self.events.append(("begin", label))
        self.gestureStack.append(label)
        return True

    def endGesture(self):
        self.events.append(("end", None))
        if self.gestureStack:
            self.gestureStack.pop()
        return True

    def cancelGesture(self):
        self.events.append(("cancel", None))
        if self.gestureStack:
            self.gestureStack.pop()
        return 1

    def endGestureIfChanged(self, changed):
        # PomadeSession's SS-02 close: keep the step only when the model
        # changed, else cancel it and publish what the cancel restored.
        if changed:
            return self.endGesture()
        dirty = self.cancelGesture()
        if dirty:
            self.publish(dirty)
        return False

    @property
    def gestureActive(self):
        return bool(self.gestureStack)

    # -- publish / commit --------------------------------------------------

    def publish(self, dirtyMask=0):
        self.published.append(int(dirtyMask))
        return 1

    def rasterise(self):
        self.events.append(("rasterise", None))
        return self.rasteriseOk

    def ensureRegionTubes(self):
        self.events.append(("ensureRegionTubes", None))
        return 0

    def enqueueCommit(self):
        self.events.append(("enqueueCommit", None))
        return True

    def rebake(self):
        self.events.append(("rebake", None))
        return True

    # -- picking -----------------------------------------------------------

    def raycast(self, origin, direction):
        self.rays.append((tuple(origin), tuple(direction)))
        if self.surfaceMisses:
            return None
        # The fixture scalp is the y = 0 plane over x, z in [0, 4].
        if abs(direction[1]) < 1e-9:
            return None
        t = -origin[1] / direction[1]
        if t <= 0.0:
            return None
        x = origin[0] + t * direction[0]
        z = origin[2] + t * direction[2]
        ix = min(max(int(math.floor(x)), 0), 3)
        iz = min(max(int(math.floor(z)), 0), 3)
        return {"face": ix * 4 + iz, "u": float(z - iz), "v": float(x - ix),
                "point": (x, 0.0, z), "normal": (0.0, 1.0, 0.0)}

    def pickItem(self, camera, x, y, radiusPx, kindMask):
        self.picks.append((int(kindMask), float(x), float(y),
                           float(radiusPx)))
        return self.pickFn(int(kindMask), float(x), float(y))

    def setHover(self, kind=0, ident=-1, subId=-1, subSubId=-1):
        self.hovers.append((int(kind), int(ident)))
        return True

    def selectRect(self, camera, x0, y0, x1, y1, kindMask, mode):
        self.rects.append((x0, y0, x1, y1, int(kindMask), int(mode)))
        return True

    def selectionCount(self, kindMask=0):
        return sum(len(v) for v in self.selection.values())

    def readSelection(self, kind):
        return [(i, -1, -1) for i in self.selection.get(int(kind), [])]

    def select(self, kind, ids, subIds=None, subSubIds=None, mode=0):
        # Graph's two-click pick draws its first pick as a selection
        # (SL-02); SET and ADD are all that path uses.
        import usdGenPomadeTools.pomadeLib as pomadeLib
        current = self.selection.setdefault(int(kind), [])
        if int(mode) == pomadeLib.POMADE_SELECT_SET:
            current[:] = []
        for ident in ids:
            if int(ident) not in current:
                current.append(int(ident))
        return True

    def clearSelection(self, kindMask=0):
        self.selection = {}
        return True


# ---------------------------------------------------------------------------
# Cameras with arithmetic anyone can check by hand
# ---------------------------------------------------------------------------

def orthoCamera(pomadeCamera, width=400, height=300):
    """Camera at the origin looking down -Z; x, y in [-2, 2]; z in [-1, -10].

    View is the identity, so the whole matrix is the projection and every
    expected pixel below is a one-line calculation.
    """
    a = -2.0 / 9.0
    b = -11.0 / 9.0
    viewProj = (0.5, 0.0, 0.0, 0.0,
                0.0, 0.5, 0.0, 0.0,
                0.0, 0.0, a, 0.0,
                0.0, 0.0, b, 1.0)
    return pomadeCamera.PomadeCamera(viewProj, width, height)


def perspectiveCamera(pomadeCamera, width=400, height=400):
    """90-degree square perspective, near 1, far 101, looking down -Z."""
    near, far = 1.0, 101.0
    f = 1.0
    viewProj = (f, 0.0, 0.0, 0.0,
                0.0, f, 0.0, 0.0,
                0.0, 0.0, (far + near) / (near - far), -1.0,
                0.0, 0.0, 2.0 * far * near / (near - far), 0.0)
    return pomadeCamera.PomadeCamera(viewProj, width, height)


def topDownCamera(pomadeCamera, width=400, height=400):
    """Orthographic straight down onto the y = 0 scalp, x, z in [0, 4].

    Pixel (px, py) maps to world x = 4*px/w, z = 4*py/h, which is what the
    GraphLoop tests aim their strokes with.
    """
    # World -> view: x stays x, z becomes view y (flipped so +z is down the
    # screen), y becomes -view z (the camera looks down).
    view = (1.0, 0.0, 0.0, 0.0,
            0.0, 0.0, -1.0, 0.0,
            0.0, -1.0, 0.0, 0.0,
            -2.0, 2.0, -10.0, 1.0)
    # Ortho: view x, y in [-2, 2] -> [-1, 1]; view z in [-1, -21] -> [-1, 1].
    a = -2.0 / 20.0
    b = -1.0 + a * 1.0
    proj = (0.5, 0.0, 0.0, 0.0,
            0.0, 0.5, 0.0, 0.0,
            0.0, 0.0, a, 0.0,
            0.0, 0.0, b, 1.0)
    return pomadeCamera.PomadeCamera(pomadeCamera.matMul(view, proj), width,
                                   height)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def testCamera(pomadeCamera):
    print("-- camera ------------------------------------------------")
    cam = orthoCamera(pomadeCamera)
    check(cam.invertible, "the ortho view-projection inverts")
    centre = cam.worldToPixels((0.0, 0.0, -5.0))
    check(centre is not None and near(centre[0], 200.0) and
          near(centre[1], 150.0),
          "the view centre lands in the middle of the frame (%r)" % (centre,))
    right = cam.worldToPixels((2.0, 0.0, -5.0))
    check(right is not None and near(right[0], 400.0),
          "x = +2 lands on the right edge (%r)" % (right,))
    top = cam.worldToPixels((0.0, 2.0, -5.0))
    check(top is not None and near(top[1], 0.0),
          "y = +2 lands on the TOP edge: pixels are top-left origin (%r)"
          % (top,))
    back = cam.pixelsToWorld(centre[0], centre[1], centre[2])
    check(back is not None and all(near(back[i], (0.0, 0.0, -5.0)[i])
                                   for i in range(3)),
          "pixelsToWorld undoes worldToPixels (%r)" % (back,))
    ray = cam.rayThrough(200.0, 150.0)
    check(ray is not None and near(ray[0][2], -1.0) and
          near(ray[1][2], -1.0) and near(ray[1][0], 0.0),
          "the centre ray starts on the near plane and points down -Z (%r)"
          % (ray,))
    check(near(cam.worldPerPixel((0.0, 0.0, -5.0)), 4.0 / 400.0),
          "orthographic world-per-pixel is the frame width over its pixels")

    persp = perspectiveCamera(pomadeCamera)
    edge = persp.worldToPixels((5.0, 0.0, -5.0))
    check(edge is not None and near(edge[0], 400.0),
          "a 90-degree frustum puts x = z on the right edge (%r)" % (edge,))
    behind = persp.worldToPixels((0.0, 0.0, 5.0))
    check(behind is None, "a point behind the camera does not project")
    check(near(persp.worldPerPixel((0.0, 0.0, -5.0)), 10.0 / 400.0),
          "perspective world-per-pixel grows with depth")
    check(near(persp.worldPerPixel((0.0, 0.0, -10.0)), 20.0 / 400.0),
          "... and doubles when the depth doubles")
    pray = persp.rayThrough(400.0, 200.0)
    check(pray is not None and near(pray[1][0] / abs(pray[1][2]), 1.0),
          "the right-edge ray leaves at 45 degrees (%r)" % (pray,))

    vp = cam.viewProjArray()
    check(len(vp) == 16 and near(vp[0], 0.5),
          "viewProjArray hands the C ABI 16 floats, row-major")

    # The top-down camera the GraphLoop tests use must map pixels to the
    # scalp the way those tests assume, or they prove nothing.
    down = topDownCamera(pomadeCamera)
    for px, py, wx, wz in ((0.0, 0.0, 0.0, 0.0), (400.0, 400.0, 4.0, 4.0),
                           (200.0, 100.0, 2.0, 1.0)):
        ray = down.rayThrough(px, py)
        if ray is None:
            check(False, "the top-down camera casts through (%g, %g)"
                  % (px, py))
            continue
        origin, direction = ray
        t = -origin[1] / direction[1]
        hit = (origin[0] + t * direction[0], origin[2] + t * direction[2])
        check(near(hit[0], wx, 1e-3) and near(hit[1], wz, 1e-3),
              "pixel (%g, %g) hits the scalp at (%g, %g) (got %r)"
              % (px, py, wx, wz, hit))


def testHotkeys(pomadeModes):
    print("-- the plan/18 3.4 key table -----------------------------")
    action = pomadeModes.HotkeyAction
    rows = (
        ("1", frozenset(), (pomadeModes.ACTION_MODE, "graph")),
        ("4", frozenset(), (pomadeModes.ACTION_MODE, "hierarchy")),
        ("6", frozenset(), (pomadeModes.ACTION_MODE, "output")),
        ("d", frozenset(), (pomadeModes.ACTION_SUBMODE, "D")),
        ("escape", frozenset(), (pomadeModes.ACTION_CANCEL, None)),
        ("z", frozenset(["ctrl"]), (pomadeModes.ACTION_UNDO, None)),
        ("y", frozenset(["ctrl"]), (pomadeModes.ACTION_REDO, None)),
        ("delete", frozenset(), (pomadeModes.ACTION_DELETE, None)),
        ("[", frozenset(), (pomadeModes.ACTION_RADIUS, -1.0)),
        ("]", frozenset(), (pomadeModes.ACTION_RADIUS, 1.0)),
        ("d", frozenset(["shift"]), (pomadeModes.ACTION_SUBDIVIDE, None)),
        ("m", frozenset(["shift"]), (pomadeModes.ACTION_MERGE, None)),
        ("down", frozenset(["ctrl"]), (pomadeModes.ACTION_ENTER_LEVEL, None)),
        ("up", frozenset(["ctrl"]), (pomadeModes.ACTION_EXIT_LEVEL, None)),
        ("backspace", frozenset(), (pomadeModes.ACTION_BACKSPACE, None)),
        ("enter", frozenset(), (pomadeModes.ACTION_COMPLETE, None)),
        ("w", frozenset(["shift"]), (pomadeModes.ACTION_WELD, None)),
        ("u", frozenset(["shift"]), (pomadeModes.ACTION_UNWELD, None)),
        ("s", frozenset(["ctrl", "shift"]), (pomadeModes.ACTION_SAVE, None)),
        # Parity G14: RigExec/a DCC's three redo spellings.
        ("z", frozenset(["ctrl", "shift"]), (pomadeModes.ACTION_REDO, None)),
        ("z", frozenset(["shift"]), (pomadeModes.ACTION_REDO, None)),
        # SL-03: select all / none / invert.
        ("a", frozenset(["ctrl"]), (pomadeModes.ACTION_SELECT_ALL, None)),
        ("a", frozenset(["ctrl", "shift"]),
         (pomadeModes.ACTION_DESELECT_ALL, None)),
        ("i", frozenset(["ctrl"]), (pomadeModes.ACTION_INVERT, None)),
    )
    for key, mods, expected in rows:
        got = action(key, mods)
        check(got == expected, "%s%s -> %r (got %r)"
              % ("+".join(sorted(mods)) + "+" if mods else "", key,
                 expected, got))
    check(action("1", frozenset(["alt"])) is None,
          "Alt always belongs to the camera, even over a mode key")
    check(action("d", frozenset(["meta"])) is None,
          "Meta likewise")
    check(action("d", frozenset(), pointerInside=False) is None,
          "a letter off the viewport is not ours")
    check(action("]", frozenset(), pointerInside=False) is None,
          "nor is a bracket off the viewport")
    check(action("1", frozenset(), pointerInside=False) ==
          (pomadeModes.ACTION_MODE, "graph"),
          "but a mode key works wherever the pointer is")
    check(action("d", frozenset(), textFocus=True) is None,
          "typing into a text field is never a sub-mode switch")
    # SL-03: a focused field owns Escape (it reverts the typing there), and
    # the destructive keys need the viewport.
    check(action("escape", frozenset(), textFocus=True) is None,
          "Escape in a focused text field is the field's, not a cancel")
    check(action("delete", frozenset(), pointerInside=False) is None,
          "Delete off the viewport is not ours")
    check(action("backspace", frozenset(), pointerInside=False) is None,
          "nor is Backspace off the viewport")
    check(action("a", frozenset(["ctrl"]), pointerInside=False) is None,
          "Ctrl+A off the viewport stays a dock list's select-all")
    check(action("a", frozenset(["ctrl"]), textFocus=True) is None,
          "and Ctrl+A in a text field selects its text")
    check(action("i", frozenset(["ctrl", "shift"])) is None,
          "Ctrl+Shift+I is unclaimed")
    check(action("f", frozenset()) == (pomadeModes.ACTION_SUBMODE, "F"),
          "F reaches the controller, which declines it so usdview frames")
    check(action("k", frozenset(["ctrl"])) is None,
          "an unclaimed Ctrl key is left alone")


def testModesShelf(pomadeModes, pomadeLoops):
    print("-- the shelf ---------------------------------------------")
    check([m.hotkey for m in pomadeModes.MODES] ==
          ["1", "2", "3", "4", "5", "6"],
          "the six modes carry the number keys plan/18 3.4 assigns")
    check(sorted(pomadeModes.MODE_KEYS) == ["1", "2", "3", "4", "5", "6"],
          "and the lookup table agrees")
    # Every mode either drives the viewport through a loop or is a panel
    # mode -- and never both, so the shelf can never be silently wrong
    # about what the viewport does. Output is the only panel mode: it has
    # no pointer behaviour by design, not because a phase is missing.
    built = set(pomadeLoops.LOOPS) | set(pomadeLoops._LAZY_LOOPS)
    panels = set(pomadeLoops.PANEL_ONLY_MODES)
    check(built | panels == set(m.id for m in pomadeModes.MODES),
          "every mode either has a loop or is a panel mode (%r / %r)"
          % (sorted(built), sorted(panels)))
    check(not (built & panels), "and never both (%r)"
          % sorted(built & panels))
    check(panels == {"output"},
          "Output is the only panel mode (%r)" % sorted(panels))
    check({"graph", "tube", "fill", "hierarchy", "sculpt"} <= built,
          "the five gesture modes have loops (%r)" % sorted(built))
    for modeId in sorted(panels):
        check(pomadeLoops.makeLoop(modeId, None, None) is None,
              "%s is a panel mode, so it builds no loop" % modeId)
    check(len(pomadeLoops.subModesFor("graph")) == 9 and
          pomadeLoops.subModesFor("output") == (),
          "sub-mode shelves come from pomadeModes, and Output has none")


def testGizmo(pomadeCamera, pomadeGizmo):
    print("-- gizmo -------------------------------------------------")
    cam = orthoCamera(pomadeCamera)
    gizmo = pomadeGizmo.GizmoState()
    check(not gizmo.visible, "a fresh gizmo draws nothing")
    gizmo.place((0.0, 0.0, -5.0), 1.0)
    check(gizmo.visible and gizmo.kind == pomadeGizmo.GIZMO_TRANSLATE,
          "placing one makes it a translate gizmo")
    # The x axis runs from pixel 200 to pixel 300 (1 world unit = 100 px).
    check(gizmo.handleAt(cam, 260.0, 150.0) == pomadeGizmo.HANDLE_U,
          "a pixel along the x axis picks the u handle")
    check(gizmo.handleAt(cam, 200.0, 100.0) == pomadeGizmo.HANDLE_V,
          "a pixel along the y axis picks the v handle")
    check(gizmo.handleAt(cam, 380.0, 40.0) == pomadeGizmo.HANDLE_NONE,
          "a pixel off every handle picks nothing")
    check(gizmo.handleAt(cam, 200.0, 150.0) == pomadeGizmo.HANDLE_CENTER,
          "the centre is the free handle: every axis starts there, so no "
          "axis can claim it (V4)")

    check(gizmo.begin(pomadeGizmo.HANDLE_U, cam, 260.0, 150.0),
          "a drag starts on the u handle")
    delta = gizmo.drag(cam, 310.0, 190.0)
    check(near(delta[0], 0.5) and near(delta[1], 0.0) and near(delta[2], 0.0),
          "an axis drag keeps only the along-axis travel (%r)" % (delta,))
    gizmo.end()
    check(gizmo.abiHandle() == -1 and not gizmo.dragging,
          "ending the drag clears the active handle")

    check(gizmo.begin(pomadeGizmo.HANDLE_CENTER, cam, 200.0, 150.0),
          "a screen-plane drag starts")
    delta = gizmo.drag(cam, 300.0, 50.0)
    # The frame is 400 x 300 over 4 x 4 world units, so a pixel is wider
    # than it is tall here: 100 px right is 1.0, 100 px up is 4/3.
    check(near(delta[0], 1.0) and near(delta[1], 4.0 / 3.0),
          "a plane drag tracks the cursor in both axes (%r)" % (delta,))
    check(gizmo.abiHandle() == pomadeGizmo.HANDLE_CENTER,
          "the centre handle stays active in the ABI so its square highlights")
    gizmo.end()

    # Parity G08: the Scale centre is the vendored scaleFactor centre
    # rule, 1 + dx / manipulator size in pixels (100 px here): right
    # grows, left shrinks, vertical travel does nothing, and past zero it
    # mirrors unless Prevent Negative Scale clamps it at MIN_SCALE_FACTOR.
    from usdGenPomadeTools import pomadeGizmoScreen
    gizmo.place((0.0, 0.0, -5.0), 1.0, pomadeGizmo.GIZMO_SCALE)
    check(gizmo.begin(pomadeGizmo.HANDLE_CENTER, cam, 200.0, 150.0),
          "a Scale centre drag starts on the pivot")
    check(near(gizmo.scaleFactor(cam, 200.0, 150.0), 1.0),
          "no travel is exactly 1x")
    check(near(gizmo.scaleFactor(cam, 250.0, 150.0), 1.5) and
          near(gizmo.scaleFactor(cam, 300.0, 150.0), 2.0),
          "50 px right is 1.5x, one manipulator size is 2x")
    check(near(gizmo.scaleFactor(cam, 200.0, 50.0), 1.0) and
          near(gizmo.scaleFactor(cam, 250.0, 100.0), 1.5),
          "vertical travel does nothing (RigExec reads dx alone)")
    check(near(gizmo.scaleFactor(cam, 160.0, 150.0), 0.6),
          "left shrinks")
    check(near(gizmo.scaleFactor(cam, 50.0, 150.0), -0.5),
          "past zero it mirrors with Prevent Negative Scale off")
    check(near(gizmo.scaleFactor(cam, 50.0, 150.0, allowNegative=False),
               pomadeGizmoScreen.MIN_SCALE_FACTOR),
          "and Prevent Negative Scale clamps it at MIN_SCALE_FACTOR")
    centre = [h for h in gizmo.handles(cam)
              if h.handleId == pomadeGizmo.HANDLE_CENTER][0]
    check(near(gizmo.scaleFactor(cam, 237.0, 150.0),
               pomadeGizmoScreen.scaleFactor(
                   centre, (200.0, 150.0), (200.0, 150.0), (237.0, 150.0),
                   True)),
          "exactly the vendored scaleFactor for the centre handle")
    gizmo.end()

    gizmo.place((0.0, 0.0, -5.0), 1.0, pomadeGizmo.GIZMO_ROTATE)
    check(gizmo.begin(pomadeGizmo.HANDLE_U, cam, 285.0, 150.0),
          "a rotate ring drag starts")
    first = gizmo.rotationDrag(cam, 200.0, 65.0)
    second = gizmo.rotationDrag(cam, 115.0, 150.0)
    check(first is not None and second is not None and
          abs(second[1]) > abs(first[1]) + 0.5,
          "rotation accumulates through successive quarter turns")
    gizmo.end()
    check(gizmo.begin(pomadeGizmo.HANDLE_FREE, cam, 200.0, 150.0),
          "a free trackball drag starts")
    first = gizmo.rotationDrag(cam, 250.0, 150.0)
    second = gizmo.rotationDrag(cam, 250.0, 100.0)
    check(first is not None and second is not None and
          abs(second[0][0] - first[0][0]) > 0.1 and
          abs(second[0][1] - first[0][1]) > 0.1,
          "a curved trackball drag composes its prior turn")
    gizmo.end()
    check(gizmo.begin(pomadeGizmo.HANDLE_FREE, cam, 200.0, 150.0),
          "a reversible free trackball drag starts")
    gizmo.rotationDrag(cam, 250.0, 150.0)
    backtracked = gizmo.rotationDrag(cam, 200.0, 150.0)
    check(backtracked is not None and near(backtracked[1], 0.0),
          "an exact free-trackball backtrack restores the press transform")
    gizmo.end()

    frame = pomadeGizmo.screenFrame(cam, (0.0, 0.0, -5.0))
    check(near(frame[0], 1.0) and near(frame[4], 1.0),
          "the screen frame of a camera looking down -Z is the world frame "
          "(%r)" % (frame,))
    check(near(pomadeGizmo.pointToSegmentPx(5.0, 4.0, 0.0, 0.0, 10.0, 0.0),
               4.0),
          "point-to-segment distance is the perpendicular where it lands")


def _polygonArea(points):
    total = 0.0
    for i in range(len(points)):
        x0, y0 = points[i]
        x1, y1 = points[(i + 1) % len(points)]
        total += x0 * y1 - x1 * y0
    return abs(total) * 0.5


def testGizmoLook(pomadeCamera, pomadeGizmo, pomadeViewport):
    """GZ-02 / parity G05, G06, G10, G22, G24: the RigExec look.

    The records the Qt overlay paints carry the RigExec palette, the tip
    that tells Move from Scale, the sizes the tips scale with and the
    locked opacity; a rotate drag reports its angle and a pie wedge; the
    overlay paints from a cache rebuilt only when the gizmo or the camera
    changes; pixel constants are logical and scale with the pixel ratio;
    and the C++ Hydra fallback uses the same constants.
    """
    print("-- gizmo look (GZ-02) ------------------------------------")
    cam = orthoCamera(pomadeCamera)
    gizmo = pomadeGizmo.GizmoState()
    origin = (0.0, 0.0, -5.0)

    # G06: RigExec's palette.
    check(pomadeGizmo.AXIS_COLORS == ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                                     (0.0, 0.0, 1.0)) and
          pomadeGizmo.ACTIVE_COLOR == (1.0, 1.0, 0.0) and
          pomadeGizmo.HOVER_COLOR == (1.0, 0.85, 0.4) and
          pomadeGizmo.VIEW_COLOR == (0.4, 0.75, 1.0) and
          pomadeGizmo.SPHERE_COLOR == (0.6, 0.6, 0.6) and
          pomadeGizmo.LOCKED_OPACITY == 0.4 and pomadeGizmo.LINE_WIDTH == 2.0,
          "the gizmo palette is RigExec's (primaries, yellow, pale hover)")

    # G05: the tip tells Move from Scale.
    gizmo.place(origin, 1.0, pomadeGizmo.GIZMO_TRANSLATE)
    records = gizmo.screenHandles(cam)
    axes = [r for r in records if r["kind"] == "axis"]
    check(len(axes) == 3 and all(r["tip"] == "cone" for r in axes),
          "Move axes end in cones %r" % [r.get("tip") for r in axes])
    check([r["color"] for r in axes if r["grabbable"]] ==
          [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
          "the grabbable Move axes are pure red and green")
    locked = [r for r in axes if not r["grabbable"]]
    check(len(locked) == 1 and locked[0]["handle"] == pomadeGizmo.HANDLE_W and
          locked[0]["opacity"] == pomadeGizmo.LOCKED_OPACITY,
          "the axis pointing at the camera is drawn at LOCKED_OPACITY")
    check(all(near(r["sizePx"], 100.0) for r in records),
          "every record carries the manipulator size in pixels (%r)"
          % sorted({round(r["sizePx"], 3) for r in records}))
    xy = [r for r in records if r["handle"] == pomadeGizmo.HANDLE_PLANE_XY]
    # 1 world unit is 100 px across and 75 px down in this camera.
    want = (pomadeGizmo.PLANE_SIDE * 100.0) * (pomadeGizmo.PLANE_SIDE * 75.0)
    check(xy and near(_polygonArea(xy[0]["points"]), want, 0.5) and
          xy[0]["fillAlpha"] == pomadeGizmo.PLANE_FILL_OPACITY,
          "the xy square is PLANE_SIDE of the gizmo and half filled "
          "(%.2f px^2, want %.2f)" % (_polygonArea(xy[0]["points"])
                                       if xy else -1.0, want))
    check([r["kind"] for r in records][0] == "center",
          "the centre record comes first (Pomade's record order)")

    gizmo.setHoverHandle(pomadeGizmo.HANDLE_U)
    uRecord = [r for r in gizmo.screenHandles(cam)
               if r["handle"] == pomadeGizmo.HANDLE_U][0]
    check(uRecord["hovered"] and uRecord["color"] == pomadeGizmo.HOVER_COLOR,
          "a hovered U axis is drawn in the hover colour")
    gizmo.setHoverHandle(pomadeGizmo.HANDLE_NONE)

    gizmo.place(origin, 1.0, pomadeGizmo.GIZMO_SCALE)
    axes = [r for r in gizmo.screenHandles(cam) if r["kind"] == "axis"]
    check(len(axes) == 3 and all(r["tip"] == "cube" for r in axes),
          "Scale axes end in cubes %r" % [r.get("tip") for r in axes])
    check(len([r for r in gizmo.screenHandles(cam)
               if r["kind"] == "plane"]) == 3,
          "Scale has the three planar squares too")
    gizmo.place(origin, 1.0, pomadeGizmo.GIZMO_RING_TRS)
    axes = [r for r in gizmo.screenHandles(cam) if r["kind"] == "axis"]
    check(axes and all(r["tip"] == "cone" for r in axes),
          "the ringTRS gizmo moves along its axes, so it has cones")

    # G10: the rotate readout and the pie wedge.
    gizmo.place(origin, 1.0, pomadeGizmo.GIZMO_ROTATE)
    records = gizmo.screenHandles(cam)
    free = [r for r in records if r["kind"] == "free"]
    check(free and free[0]["fillAlpha"] == pomadeGizmo.SPHERE_FILL_OPACITY,
          "the free-rotate ball is a filled wash")
    check(gizmo.dragAngle() == 0.0 and gizmo.pieSlice() is None,
          "an idle gizmo has no drag angle and no pie")
    # The z ring faces this camera: radius 0.85 is 85 px across, 63.75 down.
    check(gizmo.begin(pomadeGizmo.HANDLE_W, cam, 285.0, 150.0),
          "a z-ring rotate drag starts")
    check(near(gizmo.startParameter, 0.0),
          "the press on the ring's +u point is ring parameter 0 (%r)"
          % gizmo.startParameter)
    gizmo.rotationDrag(cam, 200.0, 150.0 - 63.75)
    angle = gizmo.dragAngle()
    pie = gizmo.pieSlice()
    check(near(angle, 90.0, 0.5),
          "a quarter turn reads 90 deg (%.2f)" % angle)
    check(pie is not None and len(pie[0]) == 14 and
          pie[1] == (0.0, 0.0, 1.0) and
          near(pie[0][0][0], 200.0) and near(pie[0][0][1], 150.0),
          "the pie is the centre plus a quarter of the blue ring (%r)"
          % ((len(pie[0]), pie[1]) if pie else None,))
    gizmo.end()
    check(gizmo.pieSlice() is None and gizmo.dragAngle() == 0.0,
          "the release takes the pie away")

    # G24: pixel constants are logical, scaled by the device ratio.
    viewProj = cam.viewProj
    retina = pomadeCamera.PomadeCamera(viewProj, 400, 300, pixelRatio=2.0)
    gizmo.place(origin, 1.0, pomadeGizmo.GIZMO_TRANSLATE)
    check(gizmo.handleAt(cam, 260.0, 162.0) == pomadeGizmo.HANDLE_NONE and
          gizmo.handleAt(retina, 260.0, 162.0) == pomadeGizmo.HANDLE_U,
          "12 px off the u axis misses at ratio 1 and hits at ratio 2")
    from usdGenPomadeTools import pomadeLoopsTube
    single = pomadeLoopsTube.worldSizeForPixels(cam, origin)
    double = pomadeLoopsTube.worldSizeForPixels(retina, origin)
    check(near(double, 2.0 * single) and
          near(single, pomadeLoopsTube.GIZMO_PIXELS * 0.01),
          "the placed gizmo is GIZMO_PIXELS LOGICAL pixels: twice the "
          "world size at ratio 2 (%r, %r)" % (single, double))

    # G22: the overlay paints from a cache; a repaint resolves no camera.
    class _Loop(object):
        pass

    loop = _Loop()
    loop._gizmo = gizmo
    resolves = []

    def countingResolve(_view):
        resolves.append(1)
        return cam

    oldResolve = pomadeViewport.pomadeCamera.resolve
    try:
        pomadeViewport.pomadeCamera.resolve = countingResolve
        controller = pomadeViewport.ViewportController(
            _toolState(), FakeSession(FakeDll()), None)
        controller._view = object()
        controller._loop = loop
        first, _ = controller.gizmoPaint()
        second, _ = controller.gizmoPaint()
        check(first and second is first and len(resolves) == 1,
              "a second paint reuses the cached records (%d resolves)"
              % len(resolves))
        gizmo.setHoverHandle(pomadeGizmo.HANDLE_V)
        third, _ = controller.gizmoPaint()
        check(len(resolves) == 2 and [r["handle"] for r in third
                                      if r["hovered"]] ==
              [pomadeGizmo.HANDLE_V],
              "a hover change rebuilds them")
        controller.gizmoScreenHandles()
        check(len(resolves) == 3,
              "gizmoScreenHandles (the event-driven sync) always rebuilds")
        gizmo.setHoverHandle(pomadeGizmo.HANDLE_NONE)
    finally:
        pomadeViewport.pomadeCamera.resolve = oldResolve

    # The C++ Hydra fallback uses the same constants and palette.
    import re
    cpp = os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
        "libs", "usdGenPomade", "usdGenPomade", "pomadeGizmo.cpp"))
    if not os.path.isfile(cpp):
        print("info: %s not found; C++ lockstep not checked" % cpp)
        return
    with open(cpp, "r") as handle:
        source = handle.read()

    def scalar(name):
        match = re.search(r"constexpr float %s = ([0-9.]+)f;" % name, source)
        return float(match.group(1)) if match else None

    def triple(text):
        return tuple(float(v) for v in re.findall(r"([0-9.]+)f", text))

    check(scalar("kPlaneOffset") == pomadeGizmo.PLANE_OFFSET and
          scalar("kPlaneSide") == pomadeGizmo.PLANE_SIDE and
          scalar("kCentreSide") == pomadeGizmo.CENTER_SIDE and
          scalar("kCubeSide") == pomadeGizmo.CUBE_SIDE and
          scalar("kConeRadius") == pomadeGizmo.CONE_RADIUS and
          scalar("kConeLengthRatio") == pomadeGizmo.CONE_LENGTH_RATIO,
          "pomadeGizmo.cpp's handle geometry equals the Python constants")
    axisBlock = re.search(r"kAxisColors\[3\]\[3\] = \{(.*?)\};", source,
                          re.S)
    active = re.search(r"kActiveColor\[3\] = \{(.*?)\};", source)
    view = re.search(r"kViewColor\[3\] = \{(.*?)\};", source)
    ring = re.search(r"kRingColor\[3\] = \{(.*?)\};", source)
    flat = triple(axisBlock.group(1)) if axisBlock else ()
    check(flat == sum(pomadeGizmo.AXIS_COLORS, ()) and active and
          triple(active.group(1)) == pomadeGizmo.ACTIVE_COLOR and view and
          triple(view.group(1)) == pomadeGizmo.VIEW_COLOR and ring and
          triple(ring.group(1)) == pomadeGizmo.RING_COLOR,
          "pomadeGizmo.cpp's palette equals the Python one")


def _toolState():
    from usdGenPomadeTools.pomadeToolState import PomadeToolState
    return PomadeToolState()


def newLoop(pomadeLoops, PomadeToolState, subMode="draw"):
    dll = FakeDll()
    session = FakeSession(dll)
    state = PomadeToolState()
    state.snapRadiusPx = 8.0
    loop = pomadeLoops.GraphLoop(session, state)
    loop.setSubMode(subMode)
    return dll, session, state, loop


def testSessionRegionTubeDefaults(pomadeSession, PomadeToolState):
    """Automatic stubs pass Auto; existing roots are never rebuilt."""
    print("-- PomadeSession: region-root ring columns ----------------")

    class BuildDll(object):
        def __init__(self):
            self.existing = {1: 19}
            self.builds = []
            self.refills = []

        def Pomade_TubeForRegion(self, _model, regionId):
            return self.existing.get(int(regionId), -1)

        def Pomade_RefillGuides(self, _model, fraction):
            self.refills.append(float(fraction.value))
            return 0

        def Pomade_BuildTubeFromRegion(self, _model, regionId, rings,
                                      ringVerts, length):
            self.builds.append((int(regionId), int(rings), int(ringVerts),
                                float(length.value)))
            return 0

    class SessionShell(object):
        def __init__(self, state, dll):
            self._state = state
            self._model = "model"
            self.dll = dll
            self.statuses = []

        def regionStats(self):
            return (3, 0, 0)

        def _status(self, text):
            self.statuses.append(text)

    state = PomadeToolState()
    dll = BuildDll()
    shell = SessionShell(state, dll)
    built = pomadeSession.PomadeSession.ensureRegionTubes(shell)
    check(built == 2 and [(item[0], item[2]) for item in dll.builds] ==
          [(0, 0), (2, 0)],
          "new Region stubs pass Auto while existing sculpted roots are skipped")
    # MD-01: new stubs grow their guides at once (one full-density refill),
    # instead of staying bare until a Fill parameter is touched.
    check(dll.refills == [1.0],
          "building stubs refills the guides once at full density (%r)"
          % (dll.refills,))
    dll.refills = []
    dll.existing = {0: 1, 1: 19, 2: 20}
    check(pomadeSession.PomadeSession.ensureRegionTubes(shell) == 0 and
          not dll.refills,
          "no stub built means no refill (%r)" % (dll.refills,))

    dll.existing = {}
    dll.builds = []
    state.panels["tube"] = {"ringCvCount": 12}
    built = pomadeSession.PomadeSession.ensureRegionTubes(shell)
    check(built == 3 and all(item[2] == 12 for item in dll.builds),
          "an explicit 3..32 panel request is passed unchanged to new roots")


def testGraphDraw(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- GraphLoop: draw ---------------------------------------")
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "draw")
    cam = topDownCamera(pomadeCamera)

    def sample(x, y, mods=frozenset()):
        return pomadeLoops.Sample(session, cam, x, y, mods)

    check(loop.press(sample(100.0, 100.0)), "press claims the event")
    check(session.gestureStack == ["Graph draw"],
          "press opens exactly one undo bracket (%r)" % session.gestureStack)
    loop.move(sample(300.0, 100.0))
    loop.move(sample(300.0, 300.0))
    loop.move(sample(100.0, 300.0))
    check(loop.release(sample(100.0, 100.0)), "release claims the event")
    check(not session.gestureStack, "release seals the bracket")
    check(len(session.rays) == 5,
          "one Pomade_Raycast per sample, 5 samples (got %d)"
          % len(session.rays))
    check(session.picks == [],
          "a Draw stroke never picks an item (got %r)" % (session.picks,))
    strokes = dll.argsOf("Pomade_GraphStroke")
    check(len(strokes) == 1, "one Pomade_GraphStroke for the whole drag")
    if strokes:
        samples, snap, eps, faces = strokes[0]
        check(samples == 5, "the stroke carries 5 samples (got %d)" % samples)
        # 4 world units over 400 px is 0.01 per pixel, so 8 px is 0.08.
        check(near(snap, 0.08, 1e-3),
              "the snap radius is the panel's 8 px in rest units (%g)" % snap)
        check(near(eps, 0.04, 1e-3),
              "the simplify tolerance is half the snap radius (%g)" % eps)
        # Corners (1,1), (3,1), (3,3), (1,3) on the 4x4 grid: face ix*4+iz.
        check(faces[0] == 1 * 4 + 1 and faces[1] == 3 * 4 + 1 and
              faces[2] == 3 * 4 + 3 and faces[3] == 1 * 4 + 3,
              "the samples land on the faces the pixels aim at (%r)"
              % (faces,))
    order = [name for name, _ in session.events]
    check(order[-5:] == ["rasterise", "ensureRegionTubes", "end",
                         "enqueueCommit", "rebake"],
          "release rasterises and gives a closed region its tube stub (G14) "
          "inside the stroke's bracket, then enqueues the commit and the "
          "bake (%r)" % (order,))
    check(order.count("begin") == 1 and order.count("end") == 1,
          "the stroke and its stub are one undo step (%r)" % (order,))
    check(session.events.index(("end", None)) <
          session.events.index(("enqueueCommit", None)),
          "the bracket is sealed BEFORE the stage work is enqueued")

    # Escape mid-stroke.
    dll.reset()
    session.events = []
    check(loop.press(sample(100.0, 100.0)), "a second stroke presses")
    loop.move(sample(200.0, 200.0))
    check(loop.cancel(), "Escape cancels the live stroke")
    check(("cancel", None) in session.events,
          "... through Pomade_CancelGesture (%r)" % (session.events,))
    check(dll.count("Pomade_GraphStroke") == 0,
          "a cancelled stroke never reaches Pomade_GraphStroke")
    check(not loop.release(sample(200.0, 200.0)),
          "the release after a cancel is not ours")

    # A refused Begin (another owner's bracket is open): the press is
    # claimed but nothing runs, and neither release nor Escape touches the
    # foreign bracket.
    for sub in ("draw", "place"):
        loop.setSubMode(sub)
        dll.reset()
        session.events = []
        session.gestureStack = ["Dock slider"]
        realBegin = session.beginGesture
        session.beginGesture = lambda label: (
            session.events.append(("begin-refused", label)) or False)
        try:
            check(loop.press(sample(100.0, 100.0)),
                  "%s: a refused Begin still claims the press" % sub)
            loop.move(sample(300.0, 100.0))
            loop.release(sample(300.0, 300.0))
            loop.cancel()
        finally:
            session.beginGesture = realBegin
        names = [name for name, _ in session.events]
        check(session.gestureStack == ["Dock slider"] and
              "end" not in names and "cancel" not in names,
              "%s: the foreign bracket is neither sealed nor rolled back "
              "(%r, %r)" % (sub, session.gestureStack, names))
        check(dll.count("Pomade_GraphStroke") == 0 and
              dll.count("Pomade_GraphAddNode") == 0 and
              dll.count("Pomade_GraphMoveNode") == 0,
              "%s: and nothing reaches the graph (%r)" % (sub, dll.names()))
        check(any("another edit is still open" in s
                  for s in session.statuses[-3:]),
              "%s: the status says why (%r)" % (sub, session.statuses[-3:]))
    session.gestureStack = []
    loop.setSubMode("draw")


def testGraphRegion(pomadeCamera, pomadeLoops, PomadeToolState):
    """Click-authored regions remain a Python draft until they close."""
    print("-- GraphLoop: click-created region -----------------------")
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "region")
    cam = topDownCamera(pomadeCamera)

    def sample(x, y):
        return pomadeLoops.Sample(session, cam, x, y)

    check(state.graphSubMode == "region" and loop.subMode() == "region",
          "Create region is Graph's default sub-mode")
    corners = ((100.0, 100.0), (300.0, 100.0), (300.0, 300.0))
    for x, y in corners:
        check(loop.press(sample(x, y)) and loop.release(sample(x, y)),
              "a region CV click is claimed")
    check(len(loop.draftRegionPreview()["points"]) == 3 and
          dll.count("Pomade_GraphCreateRegion") == 0 and
          not session.gestureStack,
          "three clicked CVs stay transient with no graph edit or undo step")

    check(loop.discardRegionCV(), "Backspace removes the last draft CV")
    check(len(loop.draftRegionPreview()["points"]) == 2 and
          dll.count("Pomade_GraphCreateRegion") == 0,
          "removing a draft CV still does not author the graph")
    loop.press(sample(*corners[-1]))
    loop.release(sample(*corners[-1]))
    check(loop.cancel(), "Escape cancels an idle multi-click draft")
    check(not loop.draftRegionPreview()["points"] and
          dll.count("Pomade_GraphCreateRegion") == 0,
          "Escape leaves no model edit behind")

    for x, y in corners:
        loop.press(sample(x, y))
        loop.release(sample(x, y))
    session.events = []
    check(loop.completeRegionDraft(), "Enter closes a three-CV draft")
    authored = dll.argsOf("Pomade_GraphCreateRegion")[-1]
    check(authored[0] == [-1, -1, -1] and len(authored[1]) == 3,
          "close sends three explicit new CVs to the atomic region ABI")
    names = [name for name, _args in session.events]
    check(names == ["begin", "rasterise", "ensureRegionTubes", "end",
                    "enqueueCommit", "rebake"],
          "region, rasterise and auto tube stub share one sealed undo step "
          "before commit/bake (%r)" % names)
    check(not loop.draftRegionPreview()["points"],
          "Enter clears the transient overlay after its commit")

    # Clicking the highlighted first CV is the mouse equivalent of Enter.
    dll.reset()
    session.events = []
    for x, y in corners:
        loop.press(sample(x, y))
        loop.release(sample(x, y))
    loop.press(sample(*corners[0]))
    loop.release(sample(*corners[0]))
    check(dll.count("Pomade_GraphCreateRegion") == 1 and
          not loop.draftRegionPreview()["points"],
          "clicking the first draft CV closes and commits the contour")

    # K11's stable id is retained in the draft and reaches the atomic ABI;
    # a ray landing anywhere in the visible dot cannot create a duplicate.
    dll.reset()
    dll.graphNodeHits[37] = (3, 0.25, 0.75, (1.25, 0.0, 2.75))
    session.pickFn = (lambda mask, x, _y:
                      {"kind": 8, "id": 37, "subId": -1, "subSubId": -1}
                      if mask == 8 and x < 150.0 else None)
    for x, y in corners:
        loop.press(sample(x, y))
        loop.release(sample(x, y))
    check(loop.completeRegionDraft(), "a draft may include an existing CV")
    shared = dll.argsOf("Pomade_GraphCreateRegion")[-1]
    check(shared[0] == [37, -1, -1] and shared[1][0] == 3 and
          near(shared[2][0], 0.25) and near(shared[2][1], 0.75),
          "the clicked existing CV uses its exact stable id and canonical hit")

    # Region's displayed CV target stays usable at a tiny authoring snap.
    # Before the first click it must prehighlight only the published node
    # (never an edge or region under the glyph), and the same off-centre
    # press must retain that stable id.  A 2.5 px draft hover then crosses
    # into B's handle: identity has to win over A's proximity-close fallback,
    # or the second click is swallowed and a later surface click becomes an
    # overlapping new node.
    dll.reset()
    state.snapRadiusPx = 2.0
    dll.graphNodeHits[38] = (3, 0.35, 0.75, (1.35, 0.0, 2.75))
    dll.graphNodeDisplayHits[37] = (1.25, 0.004, 2.75)
    def adjacentNodePick(mask, x, _y):
        if mask == 8 and x <= 101.0:
            return {"kind": 8, "id": 37, "subId": -1, "subSubId": -1}
        if mask == 8 and x <= 112.0:
            return {"kind": 8, "id": 38, "subId": -1, "subSubId": -1}
        # A generic pre-first-click hover would incorrectly favor this edge.
        if mask != 8:
            return {"kind": 16, "id": 91, "subId": -1, "subSubId": -1}
        return None
    session.pickFn = adjacentNodePick
    # The two approaches are six pixels from their respective glyph centres;
    # their 2.5 px separation is deliberately below the usual hover throttle.
    adjacent = ((100.0, 100.0), (102.5, 100.0), (300.0, 300.0))
    session.hovers = []
    session.picks = []
    loop.hover(sample(*adjacent[0]))
    check(session.hovers[-1] == (8, 37) and not loop._regionDraft and
          all(pick[0] == 8 and near(pick[3], 8.0) for pick in session.picks),
          "tiny-snap Region hover targets only the 8px published CV before "
          "the first click")
    loop.press(sample(*adjacent[0]))
    loop.release(sample(*adjacent[0]))
    previewPoint = loop.draftRegionPreview()["points"][0]
    check(loop._regionDraft[0][6] == 37 and
          loop._regionDraft[0][3] == (1.25, 0.0, 2.75) and
          all(near(previewPoint[i], (1.25, 0.004, 2.75)[i])
              for i in range(3)),
          "the off-centre first click retains canonical id/data and previews "
          "its lifted glyph")
    session.hovers = []
    loop.hover(sample(*adjacent[1]))
    check(session.hovers[-1] == (8, 38) and
          all(near(loop._regionHover[i], (1.35, 0.0, 2.75)[i])
              for i in range(3)),
          "a sub-threshold draft hover highlights the second stable CV")
    loop.press(sample(*adjacent[1]))
    loop.release(sample(*adjacent[1]))
    # A fresh click can arrive without a preceding miss hover; it must clear
    # B's old native highlight rather than leave the cursor suggesting that
    # the new third CV still acts on B.
    loop.press(sample(*adjacent[2]))
    loop.release(sample(*adjacent[2]))
    check(session.hovers[-1] == (0, -1),
          "a direct fresh-CV press clears a prior published-node highlight")
    loop.hover(sample(*adjacent[2]))
    check(session.hovers[-1] == (0, -1),
          "moving between draft CVs clears the native graph-node hover")
    ids = [entry[6] for entry in loop._regionDraft]
    check(ids == [37, 38, -1] and
          dll.count("Pomade_GraphCreateRegion") == 0,
          "adjacent published CVs stay distinct stable draft ids %r" % ids)
    loop.press(sample(*adjacent[0]))
    loop.release(sample(*adjacent[0]))
    reused = dll.argsOf("Pomade_GraphCreateRegion")[-1]
    check(reused[0] == [37, 38, -1],
          "the atomic second-region request reuses both published node ids")
    session.pickFn = lambda _mask, _x, _y: None

    for x, y in corners:
        loop.press(sample(x, y))
        loop.release(sample(x, y))
    check(loop.setSubMode("draw") and not loop.draftRegionPreview()["points"],
          "changing Graph sub-modes cancels the retained draft")
    loop.setSubMode("region")
    loop.press(sample(*corners[0]))
    loop.release(sample(*corners[0]))
    check(loop.deactivate() and not loop.draftRegionPreview()["points"],
          "leaving Graph cancels the retained draft too")


def testGraphReposition(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- GraphLoop: reposition -------------------------------")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    cam = topDownCamera(pomadeCamera)

    def makeSample(session):
        return lambda x, y: pomadeLoops.Sample(session, cam, x, y)

    # A visible/lifted CV is pressed at scalp (1, 1), but its canonical
    # position is (1.5, 1).  Reposition must apply the drag delta (+1, 0),
    # giving v=.5 in face x=2 rather than jumping it to the cursor's v=0.
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.press(sample(100.0, 100.0))
    check(session.statuses[-1] == "Pomade Graph: repositioning CV (Esc cancels)",
          "a valid reposition press replaces stale miss guidance")
    loop.move(sample(200.0, 100.0))
    loop.release(sample(200.0, 100.0))
    batch = dll.argsOf("Pomade_GraphMoveNodes")
    check(len(batch) == 1 and batch[0][0] == [10] and
          batch[0][1] == [9] and near(batch[0][2][0], 0.0) and
          near(batch[0][2][1], 0.5),
          "CV reposition uses frozen canonical point plus cursor delta (%r)"
          % (batch,))
    check(dll.count("Pomade_GraphMoveNode") == 0 and
          dll.count("Pomade_GraphAddNode") == 0 and
          dll.count("Pomade_GraphWeld") == 0 and
          dll.count("Pomade_GraphSplitEdge") == 0,
          "Reposition never creates, welds, or splits topology")
    events = [name for name, _args in session.events]
    check(events == ["begin", "rasterise", "ensureRegionTubes", "end",
                     "enqueueCommit", "rebake"],
          "one accepted CV drag rasterises inside its one undo bracket (%r)"
          % (events,))
    liveDirty = (pomadeLib.POMADE_DIRTY_POINTS |
                 pomadeLib.POMADE_DIRTY_TOPOLOGY |
                 pomadeLib.POMADE_DIRTY_GRAPH |
                 pomadeLib.POMADE_DIRTY_REGIONS |
                 pomadeLib.POMADE_DIRTY_GUIDES)
    check(liveDirty in session.published,
          "accepted CV samples publish points, topology, graph, regions and "
          "guides live")
    check(session.hovers[-1] == (pomadeLib.POMADE_PICK_GRAPH_NODE, 10),
          "press-time reposition highlights the picked CV without hover")

    # Reposition accessibility has fixed physical-pixel handles, independent
    # of the small graph snap preference.  Its hover and press resolver must
    # agree: CV first, then edge, and never the region behind them.
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState,
                                        "reposition")
    sample = makeSample(session)
    state.snapRadiusPx = 1.0
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    def repositionPick(mask, x, _y):
        if mask == pomadeLib.POMADE_PICK_GRAPH_NODE and x < 110.0:
            return {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                    "subId": -1, "subSubId": -1}
        if mask == pomadeLib.POMADE_PICK_GRAPH_EDGE and 110.0 <= x < 130.0:
            return {"kind": pomadeLib.POMADE_PICK_GRAPH_EDGE, "id": 77,
                    "subId": -1, "subSubId": -1}
        # A broad graph-region candidate must never become a Reposition
        # prehighlight or press target.
        if mask == pomadeLib.POMADE_PICK_REGION:
            return {"kind": pomadeLib.POMADE_PICK_REGION, "id": 3,
                    "subId": -1, "subSubId": -1}
        return None
    session.pickFn = repositionPick
    loop.hover(sample(106.0, 100.0))
    loop.hover(sample(120.0, 100.0))
    loop.hover(sample(150.0, 100.0))
    masks = [mask for mask, _x, _y, _radius in session.picks]
    radii = [radius for _mask, _x, _y, radius in session.picks]
    check(session.hovers[-3:] == [(pomadeLib.POMADE_PICK_GRAPH_NODE, 10),
                                  (pomadeLib.POMADE_PICK_GRAPH_EDGE, 77),
                                  (0, -1)] and
          pomadeLib.POMADE_PICK_REGION not in masks and
          8.0 in radii and 5.0 in radii,
          "Reposition hover uses fixed CV-first 8px/edge 5px targets")
    loop.press(sample(106.0, 100.0))
    picksBeforeActiveHover = len(session.picks)
    hoversBeforeActiveHover = len(session.hovers)
    loop.hover(sample(120.0, 100.0))
    check(len(session.picks) == picksBeforeActiveHover and
          len(session.hovers) == hoversBeforeActiveHover,
          "active reposition drag keeps its press target highlight")
    loop.cancel()

    # Releasing without travel is an arming click only: it never moves or
    # cooks, even though the CV itself was successfully picked.
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.press(sample(100.0, 100.0))
    loop.release(sample(100.0, 100.0))
    check(dll.count("Pomade_GraphMoveNodes") == 0 and
          "rasterise" not in [name for name, _args in session.events] and
          "enqueueCommit" not in [name for name, _args in session.events] and
          [name for name, _args in session.events] == ["begin", "cancel"],
          "a no-op reposition click has no movement, bake, commit, or undo")

    # K3 failure rolls back the still-open gesture rather than ending an
    # edited graph with stale maps and no committable snapshot.
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    session.rasteriseOk = False
    loop.press(sample(100.0, 100.0))
    loop.release(sample(200.0, 100.0))
    check([name for name, _args in session.events] == ["begin", "rasterise",
                                                        "cancel"] and
          "enqueueCommit" not in [name for name, _args in session.events],
          "failed reposition rasterisation restores the gesture snapshot")

    # An edge hit resolves stable endpoint ids atomically.  The node branch
    # runs first, so an overlapping node must never expand into an edge drag.
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphEdges[77] = (20, 21)
    dll.graphNodeHits[20] = (5, 0.0, 0.25, (1.25, 0.0, 1.0))
    dll.graphNodeHits[21] = (9, 0.0, 0.25, (2.25, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_EDGE, "id": 77,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_EDGE else None)
    loop.press(sample(100.0, 100.0))
    loop.release(sample(200.0, 100.0))
    batch = dll.argsOf("Pomade_GraphMoveNodes")
    check(dll.argsOf("Pomade_GraphGetEdge") == [(77,)] and len(batch) == 1
          and batch[0][0] == [20, 21],
          "edge reposition gets stable endpoints and moves them as one batch")
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphEdges[77] = (20, 21)
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else
                      ({"kind": pomadeLib.POMADE_PICK_GRAPH_EDGE, "id": 77,
                        "subId": -1, "subSubId": -1}
                       if mask == pomadeLib.POMADE_PICK_GRAPH_EDGE else None))
    loop.press(sample(100.0, 100.0))
    loop.release(sample(200.0, 100.0))
    check(dll.count("Pomade_GraphGetEdge") == 0 and
          dll.argsOf("Pomade_GraphMoveNodes")[0][0] == [10],
          "an overlapping CV has priority over the edge")

    # A rejected batch keeps the press baseline and permits a later valid
    # sample.  Returning from that valid position to the press point calls
    # the batch with the original coordinates, then cancels the no-op undo.
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.press(sample(100.0, 100.0))
    dll.batchReject = True
    loop.move(sample(200.0, 100.0))
    dll.batchReject = False
    loop.move(sample(200.0, 100.0))
    loop.move(sample(100.0, 100.0))
    loop.release(sample(100.0, 100.0))
    batch = dll.argsOf("Pomade_GraphMoveNodes")
    check(len(batch) == 3 and near(batch[1][2][1], 0.5) and
          near(batch[2][2][1], 0.5) and
          [name for name, _args in session.events] == ["begin", "cancel"],
          "rejected moves recover from the frozen baseline; backtrack cancels")

    # Escape restores the native gesture snapshot and never starts a cook.
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.press(sample(100.0, 100.0))
    loop.move(sample(200.0, 100.0))
    loop.cancel()
    check([name for name, _args in session.events] == ["begin", "cancel"]
          and "enqueueCommit" not in [name for name, _args in session.events],
          "Escape restores a reposition gesture without baking it")

    # Switching tools mid-drag must not strand the native undo bracket under
    # the new sub-mode's release implementation.
    dll, session, _state, loop = newLoop(pomadeLoops, PomadeToolState,
                                         "reposition")
    sample = makeSample(session)
    dll.graphNodeHits[10] = (5, 0.0, 0.5, (1.5, 0.0, 1.0))
    session.pickFn = (lambda mask, _x, _y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 10,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.press(sample(100.0, 100.0))
    loop.move(sample(200.0, 100.0))
    loop.setSubMode("region")
    check([name for name, _args in session.events] == ["begin", "cancel"]
          and not session.gestureStack and loop.subMode() == "region",
          "switching sub-modes cancels an active reposition bracket")


def testGraphPlaceAndClicks(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- GraphLoop: place, connect, delete, link ---------------")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "place")
    cam = topDownCamera(pomadeCamera)

    def sample(x, y, mods=frozenset()):
        return pomadeLoops.Sample(session, cam, x, y, mods)

    # Press on empty space adds a node; the drop lands on node 42 and welds.
    session.pickFn = lambda mask, x, y: None
    loop.press(sample(100.0, 100.0))
    check(dll.count("Pomade_GraphAddNode") == 1,
          "Place on empty scalp adds a node")
    loop.move(sample(150.0, 150.0))
    check(dll.count("Pomade_GraphMoveNode") == 1,
          "dragging moves it (one move, one call)")
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 42,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.release(sample(150.0, 150.0))
    welds = dll.argsOf("Pomade_GraphWeld")
    check(welds and welds[0][0] == 42,
          "dropping on another node welds onto it (%r)" % (welds,))
    check(all(mask == pomadeLib.POMADE_PICK_GRAPH_NODE
              for mask, _x, _y, _r in session.picks),
          "every node hit went through K11 with the node kind (%r)"
          % (session.picks,))
    check(all(near(r, 8.0) for _m, _x, _y, r in session.picks),
          "the pick radius is the panel's snap radius in pixels")

    # Releasing back over an edge incident to the dragged node is still an
    # ordinary move.  The edge reader must prevent Place from splitting and
    # welding it back, which would replace the node's stable id.
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "place")
    cam = topDownCamera(pomadeCamera)
    phase = ["press"]
    dll.graphEdges[21] = (7, 9)
    def incidentPick(mask, _x, _y):
        if mask == pomadeLib.POMADE_PICK_GRAPH_NODE and phase[0] == "press":
            return {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 7,
                    "subId": -1, "subSubId": -1}
        if mask == pomadeLib.POMADE_PICK_GRAPH_EDGE:
            return {"kind": pomadeLib.POMADE_PICK_GRAPH_EDGE, "id": 21,
                    "subId": -1, "subSubId": -1}
        return None
    session.pickFn = incidentPick
    loop.press(pomadeLoops.Sample(session, cam, 100.0, 100.0))
    loop.move(pomadeLoops.Sample(session, cam, 150.0, 150.0))
    phase[0] = "release"
    loop.release(pomadeLoops.Sample(session, cam, 150.0, 150.0))
    check(dll.count("Pomade_GraphSplitEdge") == 0 and
          dll.count("Pomade_GraphWeld") == 0,
          "dropping on an edge incident to the dragged node preserves it")

    # A genuinely non-incident edge remains a valid Place drop target.
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "place")
    cam = topDownCamera(pomadeCamera)
    phase = ["press"]
    dll.graphEdges[22] = (10, 11)
    def separatePick(mask, _x, _y):
        if mask == pomadeLib.POMADE_PICK_GRAPH_NODE and phase[0] == "press":
            return {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 7,
                    "subId": -1, "subSubId": -1}
        if mask == pomadeLib.POMADE_PICK_GRAPH_EDGE:
            return {"kind": pomadeLib.POMADE_PICK_GRAPH_EDGE, "id": 22,
                    "subId": -1, "subSubId": -1}
        return None
    session.pickFn = separatePick
    loop.press(pomadeLoops.Sample(session, cam, 100.0, 100.0))
    loop.move(pomadeLoops.Sample(session, cam, 150.0, 150.0))
    phase[0] = "release"
    loop.release(pomadeLoops.Sample(session, cam, 150.0, 150.0))
    check(dll.count("Pomade_GraphSplitEdge") == 1 and
          dll.argsOf("Pomade_GraphWeld") == [(55, 7)],
          "dropping on a non-incident edge still splits and welds (%r)"
          % (dll.argsOf("Pomade_GraphWeld"),))

    # Connect: two clicks, one edge.
    dll.reset()
    loop.setSubMode("connect")
    ids = iter([11, 12])
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE,
                       "id": next(ids), "subId": -1, "subSubId": -1})
    session.events = []
    loop.press(sample(100.0, 100.0))
    loop.release(sample(100.0, 100.0))
    check(dll.count("Pomade_GraphConnect") == 0,
          "one click on a node only arms the connect")
    check([name for name, _a in session.events] == [],
          "an arming click opens no undo bracket at all (%r)"
          % (session.events,))
    loop.press(sample(200.0, 200.0))
    loop.release(sample(200.0, 200.0))
    check(dll.argsOf("Pomade_GraphConnect") == [(11, 12)],
          "the second click connects the pair (%r)"
          % dll.argsOf("Pomade_GraphConnect"))
    brackets = [name for name, _a in session.events
                if name in ("begin", "end")]
    check(brackets == ["begin", "end"],
          "and the connecting click is exactly one bracket (%r)"
          % (brackets,))
    check(not session.gestureStack, "which is closed by the time it ends")
    names = [name for name, _a in session.events]
    check("ensureRegionTubes" in names and
          names.index("ensureRegionTubes") < names.index("end") <
          names.index("enqueueCommit"),
          "the stub of a region the click closed is built inside that "
          "bracket, the commit after it (%r)" % (names,))

    # Delete sub-mode: a node first, an edge when there is no node.
    dll.reset()
    loop.setSubMode("delete")
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 9,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(dll.argsOf("Pomade_GraphDeleteNode") == [(9,)],
          "Delete removes the node under the cursor")
    dll.reset()
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_EDGE, "id": 3,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_EDGE else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(dll.argsOf("Pomade_GraphDeleteEdge") == [(3,)],
          "and the edge when no node is in range")

    # Link: two regions.
    dll.reset()
    loop.setSubMode("link")
    # Region linking is a K1 face/UV containment query, rather than K11's
    # center-proximity pick or the lossy per-face map.
    dll.surfaceRegionFn = lambda face, _u, _v: 0 if face == 5 else 1
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    loop.press(sample(320.0, 120.0))
    loop.release(sample(320.0, 120.0))
    check(dll.argsOf("Pomade_GraphLinkRegions") == [(0, 1)],
          "Link joins the two clicked regions (%r)"
          % dll.argsOf("Pomade_GraphLinkRegions"))

    # Unweld.
    dll.reset()
    loop.setSubMode("unweld")
    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 5,
                       "subId": -1, "subSubId": -1}
                      if mask == pomadeLib.POMADE_PICK_GRAPH_NODE else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(dll.argsOf("Pomade_GraphUnweld") == [(5,)],
          "Unweld splits the clicked node")


def testGraphHoverMarqueeKeys(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- GraphLoop: hover, marquee, keys -----------------------")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "draw")
    cam = topDownCamera(pomadeCamera)

    def sample(x, y, mods=frozenset()):
        return pomadeLoops.Sample(session, cam, x, y, mods)

    session.pickFn = (lambda mask, x, y:
                      {"kind": pomadeLib.POMADE_PICK_GRAPH_NODE, "id": 6,
                       "subId": -1, "subSubId": -1})
    check(not loop.hover(sample(120.0, 120.0)),
          "hover never claims the event")
    check(session.hovers[-1] == (pomadeLib.POMADE_PICK_GRAPH_NODE, 6),
          "a hover over a node highlights it (%r)" % (session.hovers,))
    check(session.published[-1] == pomadeLib.POMADE_DIRTY_SELECTION,
          "and republishes only the selection locator (%r)"
          % (session.published,))
    session.pickFn = lambda mask, x, y: None
    loop.hover(sample(300.0, 300.0))
    check(session.hovers[-1] == (0, -1),
          "hovering nothing clears the highlight")

    # Marquee: Shift-drag selects nodes through Pomade_SelectRect.
    session.rects = []
    check(loop.press(sample(100.0, 100.0, frozenset(["shift"]))),
          "Shift-press starts a marquee")
    check(not session.gestureStack,
          "a marquee is a selection, not an undoable gesture")
    loop.move(sample(300.0, 300.0, frozenset(["shift"])))
    loop.release(sample(320.0, 320.0, frozenset(["shift"])))
    check(len(session.rects) == 2,
          "the band selects live and again on release (%r)"
          % (session.rects,))
    if session.rects:
        x0, y0, x1, y1, kind, mode = session.rects[-1]
        # SL-01's band column: a Shift band adds (a DCC's extend).
        check(kind == pomadeLib.POMADE_PICK_GRAPH_NODE and
              mode == pomadeLib.POMADE_SELECT_ADD and
              near(x0, 100.0) and near(y1, 320.0),
              "the band is the press-to-release rectangle over nodes, "
              "adding (%r)" % (session.rects[-1],))
    session.rects = []
    loop.press(sample(100.0, 100.0, frozenset(["shift", "ctrl"])))
    loop.release(sample(200.0, 200.0, frozenset(["shift", "ctrl"])))
    check(session.rects and session.rects[-1][5] == pomadeLib.POMADE_SELECT_ADD,
          "Ctrl+Shift adds to the selection instead of replacing it")

    # Delete key over a selection.
    dll.reset()
    session.selection = {pomadeLib.POMADE_PICK_GRAPH_NODE: [2, 3],
                         pomadeLib.POMADE_PICK_GRAPH_EDGE: [7]}
    check(loop.deleteSelection(), "Delete acts on the selection")
    check(dll.count("Pomade_GraphDeleteNode") == 2 and
          dll.count("Pomade_GraphDeleteEdge") == 1,
          "it removes both selected nodes and the selected edge")
    check(session.gestureStack == [],
          "the whole delete is one sealed bracket")
    session.selection = {}
    check(not loop.deleteSelection(),
          "Delete with nothing selected is not ours")

    # Bracket keys.
    state.snapRadiusPx = 8.0
    loop.adjustRadius(1.0)
    check(near(state.snapRadiusPx, 9.0), "] grows the snap radius")
    loop.adjustRadius(-20.0)
    check(near(state.snapRadiusPx, 1.0),
          "[ shrinks it and clamps at 1 px (got %g)" % state.snapRadiusPx)

    # Shift+W / Shift+U want an exact selection.
    dll.reset()
    session.selection = {pomadeLib.POMADE_PICK_GRAPH_NODE: [4, 5]}
    check(loop.weldSelected() and dll.argsOf("Pomade_GraphWeld") == [(4, 5)],
          "Shift+W welds exactly two selected nodes")
    session.selection = {pomadeLib.POMADE_PICK_GRAPH_NODE: [4, 5, 6]}
    check(not loop.weldSelected(), "three nodes is not a weld")
    session.selection = {pomadeLib.POMADE_PICK_GRAPH_NODE: [4]}
    session.events = []
    session.statuses = []
    check(loop.unweldSelected(), "Shift+U unwelds the one selected node")
    names = [name for name, _a in session.events]
    check(names[:4] == ["begin", "rasterise", "ensureRegionTubes", "end"]
          and names.count("end") == 1,
          "and the split, K3 and the stubs are one undo step (%r)"
          % (names,))
    check(any("unwelded into 3 nodes" in s for s in session.statuses),
          "and the status counts the pieces (%r)" % (session.statuses,))

    # SS-02: a node no second region shares has nothing to split.
    dll.unweldCreated = 0
    session.events = []
    session.statuses = []
    check(not loop.unweldSelected(),
          "Shift+U on an unshared node is not an unweld")
    names = [name for name, _a in session.events]
    check(names == ["begin", "cancel"],
          "and leaves no undo step and no commit (%r)" % (names,))
    check(session.statuses and "not shared" in session.statuses[-1] and
          not any("unwelded into" in s for s in session.statuses),
          "and says so instead of 'unwelded into 1' (%r)"
          % (session.statuses,))
    dll.unweldCreated = 2

    # SS-02: a refused weld leaves no step either.
    session.selection = {pomadeLib.POMADE_PICK_GRAPH_NODE: [4, 5]}
    session.events = []
    dll.Pomade_GraphWeld = lambda _model, _keep, _drop: 1
    check(not loop.weldSelected(), "a weld the model refuses is not a weld")
    names = [name for name, _a in session.events]
    check(names == ["begin", "cancel"],
          "and leaves no undo step and no commit (%r)" % (names,))
    del dll.Pomade_GraphWeld

    # Weld all: the panel's pixels become rest units at the scalp (4 world
    # units over 400 px is 0.01 per pixel, so 8 px is 0.08), never 8.0.
    state.snapRadiusPx = 8.0
    dll.reset()
    session.events = []
    check(loop.weldAll(cam), "Weld all runs")
    radii = dll.argsOf("Pomade_GraphWeldAll")
    check(len(radii) == 1 and near(radii[0][0], 0.08, 1e-3),
          "Weld all hands the ABI the snap radius in rest units (%r)"
          % (radii,))
    names = [name for name, _a in session.events]
    check(names[:4] == ["begin", "rasterise", "ensureRegionTubes", "end"]
          and names.count("end") == 1,
          "and welds, rasterises and grows stubs as one undo step (%r)"
          % (names,))
    dll.reset()
    check(loop.weldAll(None) and
          near(dll.argsOf("Pomade_GraphWeldAll")[0][0], 0.05, 1e-6),
          "with no camera and no recorded scale it falls back to the "
          "model's world radius, not the pixel number (%r)"
          % dll.argsOf("Pomade_GraphWeldAll"))
    session.displayScale = lambda: 0.002
    dll.reset()
    check(loop.weldAll(None) and
          near(dll.argsOf("Pomade_GraphWeldAll")[0][0], 0.016, 1e-6),
          "with no camera the recorded display scale converts the pixels "
          "(%r)" % dll.argsOf("Pomade_GraphWeldAll"))
    del session.displayScale


# ---------------------------------------------------------------------------
# SL-01: one selection-modifier table for every loop
# ---------------------------------------------------------------------------

class MatrixSession(FakeSession):
    """FakeSession with Pomade's selection arithmetic (pomadeSelection.cpp).

    SET replaces the kinds it names (an empty SET is a no-op), a band SET
    clears its whole mask first, and a Python-only mode reaching an ABI
    call is a test failure. Every band covers `bandHits`.
    """

    def __init__(self, dll):
        super(MatrixSession, self).__init__(dll)
        self.items = {}
        self.bandHits = {}
        self.abiModes = []

    def select(self, kind, ids, subIds=None, subSubIds=None, mode=0):
        import usdGenPomadeTools.pomadeLib as pomadeLib
        self.abiModes.append(int(mode))
        rows =[(int(ids[i]), int(subIds[i]) if subIds else -1,
                 int(subSubIds[i]) if subSubIds else -1)
                for i in range(len(ids))]
        current = self.items.setdefault(int(kind), [])
        if int(mode) == pomadeLib.POMADE_SELECT_SET:
            if rows:
                current[:] = list(dict.fromkeys(rows))
        for row in rows:
            if int(mode) == pomadeLib.POMADE_SELECT_TOGGLE and row in current:
                current.remove(row)
            elif row not in current:
                current.append(row)
        return True

    def _band(self, kindMask, mode):
        import usdGenPomadeTools.pomadeLib as pomadeLib
        self.abiModes.append(int(mode))
        if int(mode) == pomadeLib.POMADE_SELECT_SET:
            self.clearSelection(kindMask)
        for kind, rows in self.bandHits.items():
            if kind & kindMask and rows:
                self.select(kind, [r[0] for r in rows], [r[1] for r in rows],
                            [r[2] for r in rows],
                            pomadeLib.POMADE_SELECT_ADD
                            if int(mode) == pomadeLib.POMADE_SELECT_SET
                            else mode)
        return True

    def selectRect(self, camera, x0, y0, x1, y1, kindMask, mode):
        self.rects.append((x0, y0, x1, y1, int(kindMask), int(mode)))
        return self._band(kindMask, mode)

    def selectPolygon(self, camera, points, kindMask, mode):
        return self._band(kindMask, mode)

    def clearSelection(self, kindMask=0):
        for kind in list(self.items):
            if not kindMask or kind & kindMask:
                self.items.pop(kind)
        return True

    def readSelection(self, kind):
        return sorted(self.items.get(int(kind), []))

    def selectionCount(self, kindMask=0):
        return sum(len(v) for k, v in self.items.items()
                   if not kindMask or k & kindMask)


# Start from {A, B} selected and C not. A click lands on A (selected) or C
# (not); a band covers A and C. The table (backlog "Selection-modifier
# convention"): click none/Shift/Ctrl/Ctrl+Shift = SET/TOGGLE/REMOVE/ADD,
# band = SET/ADD/REMOVE/ADD.
MATRIX_MODIFIERS = ((), ("shift",), ("ctrl",), ("ctrl", "shift"))
MATRIX_CLICK = {
    ((), "A"): "A", ((), "C"): "C",
    (("shift",), "A"): "B", (("shift",), "C"): "ABC",
    (("ctrl",), "A"): "B", (("ctrl",), "C"): "AB",
    (("ctrl", "shift"), "A"): "AB", (("ctrl", "shift"), "C"): "ABC",
}
MATRIX_BAND = {(): "AC", ("shift",): "ABC", ("ctrl",): "B",
               ("ctrl", "shift"): "ABC"}


def runSelectionMatrix(label, names, reset, read, click=None, band=None,
                       skip=(), skipBand=()):
    """Drive the matrix; `names` maps 'A'/'B'/'C' to selection entries.

    `skip` drops a modifier row entirely, `skipBand` only its band.
    """
    for mods in MATRIX_MODIFIERS:
        if mods in skip:
            continue
        spelled = "+".join(mods) or "none"
        for target in ("A", "C"):
            if click is None:
                break
            reset()
            click(names[target], frozenset(mods))
            want = sorted(names[c] for c in MATRIX_CLICK[(mods, target)])
            got = read()
            check(got == want, "%s: %s-click on %s -> %s (%r)"
                  % (label, spelled, target, MATRIX_CLICK[(mods, target)],
                     got))
        if band is not None and mods not in skipBand:
            reset()
            band(frozenset(mods))
            want = sorted(names[c] for c in MATRIX_BAND[mods])
            got = read()
            check(got == want, "%s: %s-band over A and C -> %s (%r)"
                  % (label, spelled, MATRIX_BAND[mods], got))


def testSelectModeTable(pomadeLoops):
    print("-- SL-01: the selection-modifier table ------------------")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    SET, ADD, TOGGLE, REMOVE = (pomadeLib.POMADE_SELECT_SET,
                                pomadeLib.POMADE_SELECT_ADD,
                                pomadeLib.POMADE_SELECT_TOGGLE,
                                pomadeLib.POMADE_SELECT_REMOVE)
    check(REMOVE not in (SET, ADD, TOGGLE),
          "POMADE_SELECT_REMOVE is its own Python-only sentinel")
    rows = (((), SET, SET), (("shift",), TOGGLE, ADD),
            (("ctrl",), REMOVE, REMOVE), (("ctrl", "shift"), ADD, ADD),
            (("alt",), SET, SET))
    for mods, click, band in rows:
        check(pomadeLoops.selectModeFor(frozenset(mods)) == click and
              pomadeLoops.selectModeFor(frozenset(mods), band=True) == band,
              "%s: click %d, band %d" % ("+".join(mods) or "none", click,
                                         band))
    sample = pomadeLoops.Sample(None, None, 0, 0, frozenset(["ctrl"]))
    check(pomadeLoops.selectModeFor(sample) == REMOVE,
          "a Sample is read through its modifiers")

    # applyRemove toggles only what is selected: Ctrl on an unselected
    # item must never add it.
    session = MatrixSession(FakeDll())
    kind = pomadeLib.POMADE_PICK_CENTER_CV
    session.items = {kind: [(0, 1, -1), (0, 2, -1)]}
    check(pomadeLoops.applyRemove(session, kind, [(0, 1, -1), (0, 3, -1)])
          and session.readSelection(kind) == [(0, 2, -1)],
          "click-remove drops the selected item and ignores the other")
    check(not pomadeLoops.applyRemove(session, kind, [(0, 3, -1)]) and
          session.readSelection(kind) == [(0, 2, -1)],
          "removing an unselected item changes nothing")
    tube = pomadeLib.POMADE_PICK_TUBE_VERT
    session.items = {tube: [(4, -1, -1)]}
    pomadeLoops.applyRemove(session, tube,
                           [{"kind": tube, "id": 4, "subId": 17,
                             "subSubId": -1}])
    check(session.readSelection(tube) == [],
          "a whole tube is removed by id, whatever vertex was picked")

    # Band-remove is S - B, and a failed band query leaves S alone.
    session.items = {kind: [(0, 1, -1), (0, 2, -1)],
                     tube: [(4, -1, -1)]}
    session.bandHits = {kind: [(0, 2, -1), (0, 3, -1)]}
    check(pomadeLoops.selectBand(session, kind, REMOVE,
                                lambda mode: session.selectRect(
                                    None, 0, 0, 1, 1, kind, mode)),
          "a Ctrl band applies")
    check(session.readSelection(kind) == [(0, 1, -1)] and
          session.readSelection(tube) == [(4, -1, -1)],
          "it removes what it covers and nothing outside its mask (%r)"
          % (session.items,))
    check(not pomadeLoops.selectBand(session, kind, REMOVE,
                                    lambda mode: False) and
          session.readSelection(kind) == [(0, 1, -1)],
          "a band query that fails leaves the selection as it was")


def testGraphSelectionMatrix(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- SL-01: Graph click and band modifier matrix -----------")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    kind = pomadeLib.POMADE_PICK_GRAPH_NODE
    dll = FakeDll()
    session = MatrixSession(dll)
    state = PomadeToolState()
    state.snapRadiusPx = 8.0
    loop = pomadeLoops.GraphLoop(session, state)
    loop.setSubMode("connect")
    cam = topDownCamera(pomadeCamera)
    names = {"A": (1, -1, -1), "B": (2, -1, -1), "C": (3, -1, -1)}
    session.bandHits = {kind: [names["A"], names["C"]]}

    def reset():
        session.items = {kind: [names["A"], names["B"]]}

    def read():
        return session.readSelection(kind)

    def click(entry, mods):
        session.pickFn = (lambda mask, x, y:
                          {"kind": kind, "id": entry[0], "subId": -1,
                           "subSubId": -1}
                          if mask & kind else None)
        loop.press(pomadeLoops.Sample(session, cam, 120.0, 120.0, mods))
        loop.release(pomadeLoops.Sample(session, cam, 120.0, 120.0, mods))

    def band(mods):
        session.pickFn = lambda mask, x, y: None
        loop.press(pomadeLoops.Sample(session, cam, 100.0, 100.0, mods))
        loop.move(pomadeLoops.Sample(session, cam, 200.0, 200.0, mods))
        loop.release(pomadeLoops.Sample(session, cam, 220.0, 220.0, mods))

    # A plain click on a node in connect arms it (drawn as a SET selection,
    # so it would match the table) but the next plain click would connect
    # the pair, so the plain row runs its band only: since SL-02 an empty
    # plain drag in a click sub-mode is a node marquee.
    runSelectionMatrix("Graph", names, reset, read, click, band,
                       skip=((),))
    runSelectionMatrix("Graph plain", names, reset, read, None, band,
                       skip=MATRIX_MODIFIERS[1:])
    check(not dll.count("Pomade_GraphConnect"),
          "no modifier click or band armed or ran a connect")
    check(session.abiModes and
          set(session.abiModes) <= {pomadeLib.POMADE_SELECT_SET,
                                    pomadeLib.POMADE_SELECT_ADD,
                                    pomadeLib.POMADE_SELECT_TOGGLE},
          "only real Pomade_Select* modes reached the ABI (%r)"
          % sorted(set(session.abiModes)))


def testGraphEmptyDragAndTwoClickCancel(pomadeCamera, pomadeLoops,
                                        PomadeToolState):
    print("-- SL-02: Graph empty-drag marquee, Shift-click, cancel --")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    kind = pomadeLib.POMADE_PICK_GRAPH_NODE
    cam = topDownCamera(pomadeCamera)
    dll = FakeDll()
    session = MatrixSession(dll)
    state = PomadeToolState()
    state.snapRadiusPx = 8.0
    loop = pomadeLoops.GraphLoop(session, state)

    def sample(x, y, mods=frozenset()):
        return pomadeLoops.Sample(session, cam, x, y, mods)

    def onNode(ident):
        return (lambda mask, x, y:
                {"kind": kind, "id": ident, "subId": -1, "subSubId": -1}
                if mask & kind else None)

    # Shift press/release on a node pixel grows the selection by one, in an
    # authoring sub-mode too (Shift is the selection override there).
    for sub in ("draw", "connect"):
        loop.setSubMode(sub)
        session.items = {kind: [(1, -1, -1)]}
        session.pickFn = onNode(2)
        loop.press(sample(120.0, 120.0, frozenset(["shift"])))
        loop.release(sample(120.0, 120.0, frozenset(["shift"])))
        check(session.readSelection(kind) == [(1, -1, -1), (2, -1, -1)],
              "%s: a Shift-click on a node adds exactly it (%r)"
              % (sub, session.readSelection(kind)))
    check(not dll.count("Pomade_GraphStroke") and
          not dll.count("Pomade_GraphConnect"),
          "and authors nothing")

    # A plain press on empty space in a click sub-mode is a node marquee.
    for sub in ("connect", "weld", "unweld", "delete", "link"):
        loop.setSubMode(sub)
        dll.reset()
        session.rects = []
        session.items = {kind: [(1, -1, -1)]}
        session.bandHits = {kind: [(3, -1, -1)]}
        session.pickFn = lambda mask, x, y: None
        dll.surfaceRegionFn = lambda _face, _u, _v: -1
        check(loop.press(sample(100.0, 100.0)) and
              loop.marqueeRect() == (100.0, 100.0),
              "%s: a plain press on empty space starts a marquee" % sub)
        loop.move(sample(200.0, 200.0))
        check(session.rects and session.rects[-1][4] == kind and
              session.rects[-1][5] == pomadeLib.POMADE_SELECT_SET,
              "%s: dragging it boxes nodes with SET (%r)"
              % (sub, session.rects[-1:]))
        loop.release(sample(210.0, 210.0))
        check(session.readSelection(kind) == [(3, -1, -1)] and
              loop.marqueeRect() is None,
              "%s: and the band replaced the selection (%r)"
              % (sub, session.readSelection(kind)))
        authored = [name for name in dll.names() if name.startswith(
            "Pomade_Graph") and name not in ("Pomade_GraphGetNode",
                                            "Pomade_GraphGetEdge")]
        check(not authored and not session.gestureStack,
              "%s: the band authored nothing (%r)" % (sub, authored))
    dll.surfaceRegionFn = lambda _face, _u, _v: 0

    # A plain click on empty space (no travel) deselects nodes.
    loop.setSubMode("delete")
    session.items = {kind: [(1, -1, -1)]}
    session.bandHits = {}          # a zero-area band covers nothing
    loop.press(sample(100.0, 100.0))
    loop.release(sample(100.0, 100.0))
    check(session.readSelection(kind) == [] and
          not dll.count("Pomade_GraphDeleteNode") and
          not dll.count("Pomade_GraphDeleteEdge"),
          "a plain empty click in Delete deselects and deletes nothing")

    # Arm connect: the first pick is drawn as the selection.
    loop.setSubMode("connect")
    dll.reset()
    session.items = {}
    session.pickFn = onNode(11)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(session.readSelection(kind) == [(11, -1, -1)],
          "the armed connect node is selected so it is drawn (%r)"
          % (session.readSelection(kind),))
    # An armed miss keeps the pick (a short second click is not a cancel).
    session.rects = []
    session.pickFn = lambda mask, x, y: None
    loop.press(sample(300.0, 300.0))
    loop.release(sample(300.0, 300.0))
    check(not session.rects and
          session.readSelection(kind) == [(11, -1, -1)],
          "a miss while armed neither boxes nor drops the pick")
    # Escape between the clicks drops the pick.
    session.statuses = []
    check(loop.cancel(), "cancel() with an armed first pick is claimed")
    check(session.readSelection(kind) == [] and
          session.hovers and session.hovers[-1] == (0, -1),
          "it clears the pick's selection and the hover (%r, %r)"
          % (session.readSelection(kind), session.hovers[-1:]))
    check(session.statuses and "connect cancelled" in session.statuses[-1],
          "and says so (%r)" % (session.statuses[-1:],))
    check(not loop.cancel(), "a second Escape has nothing left to cancel")
    session.pickFn = onNode(12)
    loop.press(sample(200.0, 200.0))
    loop.release(sample(200.0, 200.0))
    check(not dll.count("Pomade_GraphConnect"),
          "the next node click arms afresh instead of connecting")
    session.pickFn = onNode(13)
    loop.press(sample(220.0, 220.0))
    loop.release(sample(220.0, 220.0))
    check(dll.argsOf("Pomade_GraphConnect") == [(12, 13)] and
          session.readSelection(kind) == [],
          "and the pair after it connects, spending the pick (%r)"
          % (dll.argsOf("Pomade_GraphConnect"),))

    # Link arms a region the same way, and Escape drops it too.
    loop.setSubMode("link")
    dll.reset()
    dll.surfaceRegionFn = lambda _face, _u, _v: 4
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(session.readSelection(pomadeLib.POMADE_PICK_REGION) ==
          [(4, -1, -1)], "the armed link region is selected")
    session.statuses = []
    check(loop.cancel() and
          session.readSelection(pomadeLib.POMADE_PICK_REGION) == [] and
          "link cancelled" in session.statuses[-1],
          "and Escape cancels the link (%r)" % (session.statuses[-1:],))
    dll.surfaceRegionFn = lambda _face, _u, _v: 5
    loop.press(sample(320.0, 120.0))
    loop.release(sample(320.0, 120.0))
    check(not dll.count("Pomade_GraphLinkRegions"),
          "a click after the cancel only arms")

    # A sub-mode switch drops an armed pick as well.
    loop.setSubMode("connect")
    check(session.readSelection(pomadeLib.POMADE_PICK_REGION) == [],
          "leaving Link takes its armed region back")


def testGraphPolish(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- MD-05: close snap, honest no-ops, hover masks ---------")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    NODE = pomadeLib.POMADE_PICK_GRAPH_NODE
    EDGE = pomadeLib.POMADE_PICK_GRAPH_EDGE
    REGION = pomadeLib.POMADE_PICK_REGION
    cam = topDownCamera(pomadeCamera)

    # -- Region: the hover snaps onto the first CV and arms the close ------
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "region")

    def sample(x, y, mods=frozenset()):
        return pomadeLoops.Sample(session, cam, x, y, mods)

    def samePoint(a, b):
        return (a is not None and b is not None and
                all(near(a[i], b[i]) for i in range(3)))

    loop.press(sample(100.0, 100.0))
    loop.release(sample(100.0, 100.0))
    loop.hover(sample(104.0, 100.0))
    preview = loop.draftRegionPreview()
    check(samePoint(preview["hover"], preview["points"][0]) and
          not preview["closeArmed"],
          "a one-CV draft snaps its rubber band to the first CV but cannot "
          "close yet (%r)" % (preview,))
    for x, y in ((300.0, 100.0), (300.0, 300.0)):
        loop.press(sample(x, y))
        loop.release(sample(x, y))
    session.statuses = []
    loop.hover(sample(104.0, 100.0))
    preview = loop.draftRegionPreview()
    check(samePoint(preview["hover"], preview["points"][0]) and
          preview["closeArmed"] is True,
          "a hover 4 px from the first CV of a 3-CV draft snaps onto it "
          "and arms the close (%r)" % (preview,))
    check(any("click to close" in text for text in session.statuses),
          "and the status says a click closes it (%r)" % (session.statuses,))
    loop.hover(sample(103.0, 101.0))
    check(sum("click to close" in text for text in session.statuses) == 1,
          "the close status is said once per approach, not per move")
    loop.hover(sample(200.0, 200.0))
    preview = loop.draftRegionPreview()
    check(not preview["closeArmed"] and
          not samePoint(preview["hover"], preview["points"][0]),
          "moving away disarms the close and frees the rubber band")
    loop.hover(sample(104.0, 100.0))
    loop.press(sample(104.0, 100.0))
    loop.release(sample(104.0, 100.0))
    check(dll.count("Pomade_GraphCreateRegion") == 1 and
          not loop.draftRegionPreview()["closeArmed"],
          "clicking the armed first CV closes the region and disarms")

    # -- Place: a grabbed node that never moves is no edit ------------------
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "place")
    session.pickFn = (lambda mask, x, y:
                      {"kind": NODE, "id": 7, "subId": -1, "subSubId": -1}
                      if mask == NODE else None)
    session.events = []
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    names = [name for name, _args in session.events]
    check(names == ["begin", "cancel"] and not session.gestureStack,
          "a Place press on a node released without a move cancels its "
          "bracket (%r)" % (names,))
    check("rasterise" not in names and "enqueueCommit" not in names and
          not dll.count("Pomade_GraphMoveNode"),
          "and neither rasterises nor commits")
    # A move the model refuses is no edit either.
    dll.Pomade_GraphMoveNode = lambda *_args: 1
    session.events = []
    loop.press(sample(120.0, 120.0))
    loop.move(sample(160.0, 160.0))
    loop.release(sample(160.0, 160.0))
    names = [name for name, _args in session.events]
    check(names == ["begin", "cancel"],
          "a drag whose every move is refused cancels too (%r)" % (names,))
    del dll.Pomade_GraphMoveNode
    session.events = []
    loop.press(sample(120.0, 120.0))
    loop.move(sample(160.0, 160.0))
    loop.release(sample(160.0, 160.0))
    names = [name for name, _args in session.events]
    check(names[:4] == ["begin", "rasterise", "ensureRegionTubes", "end"]
          and names.count("end") == 1,
          "an accepted move still seals one step, rasterising inside it "
          "(%r)" % (names,))

    # -- per-sub-mode hover masks -------------------------------------------
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "delete")
    items = {}

    def maskedPick(mask, _x, _y):
        # Everything is under the cursor; only the mask decides.
        for kind in (NODE, EDGE, REGION):
            if mask & kind and kind in items:
                return {"kind": kind, "id": items[kind], "subId": -1,
                        "subSubId": -1}
        return None
    session.pickFn = maskedPick
    dll.surfaceRegionFn = lambda _face, _u, _v: 4

    items = {REGION: 9}
    session.picks = []
    loop.hover(sample(150.0, 150.0))
    check(session.hovers[-1] == (0, -1) and
          not any(mask & REGION for mask, _x, _y, _r in session.picks),
          "Delete hover never highlights a region (%r, %r)"
          % (session.hovers[-1:], session.picks))
    items = {REGION: 9, EDGE: 3}
    loop.hover(sample(150.0, 150.0))
    check(session.hovers[-1] == (EDGE, 3),
          "Delete hover falls back to the edge (%r)" % (session.hovers[-1:],))
    items = {REGION: 9, EDGE: 3, NODE: 6}
    loop.hover(sample(150.0, 150.0))
    check(session.hovers[-1] == (NODE, 6),
          "and prefers the node, as its press does (%r)"
          % (session.hovers[-1:],))
    for sub in ("connect", "weld", "unweld"):
        loop.setSubMode(sub)
        items = {REGION: 9, EDGE: 3}
        session.picks = []
        loop.hover(sample(150.0, 150.0))
        check(session.hovers[-1] == (0, -1) and
              all(mask == NODE for mask, _x, _y, _r in session.picks),
              "%s hover targets nodes only (%r)" % (sub, session.picks))
        items = {NODE: 6}
        loop.hover(sample(150.0, 150.0))
        check(session.hovers[-1] == (NODE, 6),
              "%s hover highlights the node it would act on" % sub)
    loop.setSubMode("link")
    items = {NODE: 6, EDGE: 3}
    session.picks = []
    loop.hover(sample(150.0, 150.0))
    check(session.hovers[-1] == (REGION, 4) and not session.picks,
          "Link hover is the region containing the cursor, never a node "
          "or edge (%r)" % (session.hovers[-1:],))

    # -- honest no-ops -------------------------------------------------------
    loop.setSubMode("delete")
    items = {}
    session.statuses = []
    loop.press(sample(150.0, 150.0))
    loop.release(sample(150.0, 150.0))
    check(session.statuses and
          "no node or edge here" in session.statuses[-1] and
          not dll.count("Pomade_GraphDeleteNode") and
          not dll.count("Pomade_GraphDeleteEdge"),
          "a Delete click on empty space says so (%r)"
          % (session.statuses[-1:],))
    loop._pressDelete((EDGE, -1))
    check("no node or edge here" in session.statuses[-1],
          "so does a direct Delete press on nothing")
    loop.press(sample(150.0, 150.0))
    loop.move(sample(250.0, 250.0))
    loop.release(sample(250.0, 250.0))
    check("node(s) selected" in session.statuses[-1],
          "a real box over empty space still reports the selection (%r)"
          % (session.statuses[-1:],))
    loop.setSubMode("link")
    dll.surfaceRegionFn = lambda _face, _u, _v: -1
    loop.press(sample(150.0, 150.0))
    loop.release(sample(150.0, 150.0))
    check("no region here" in session.statuses[-1],
          "a Link click on no region says so (%r)" % (session.statuses[-1:],))


def testFillCancelRestores(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- SL-02: Fill marquee cancel restores the selection -----")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    from usdGenPomadeTools import pomadeLoopsFill
    tube = pomadeLib.POMADE_PICK_TUBE_VERT
    session = MatrixSession(FakeDll())
    state = PomadeToolState()
    loop = pomadeLoopsFill.FillLoop(session, state)
    cam = topDownCamera(pomadeCamera)

    def sample(x, y, mods=frozenset()):
        return pomadeLoops.Sample(session, cam, x, y, mods)

    session.pickFn = (lambda mask, x, y:
                      {"kind": tube, "id": 0, "subId": -1, "subSubId": -1}
                      if mask & tube else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(session.readSelection(tube) == [(0, -1, -1)],
          "a click selects tube 0 (%r)" % (session.readSelection(tube),))
    session.pickFn = lambda mask, x, y: None
    session.bandHits = {tube: [(1, -1, -1)]}
    loop.press(sample(50.0, 50.0))
    loop.move(sample(250.0, 250.0))
    check(session.readSelection(tube) == [(1, -1, -1)],
          "a live band over tube 1 selects it (%r)"
          % (session.readSelection(tube),))
    session.published = []
    session.statuses = []
    check(loop.cancel(), "cancel() takes the live band")
    check(session.readSelection(tube) == [(0, -1, -1)],
          "and restores the press-time selection (%r)"
          % (session.readSelection(tube),))
    check(pomadeLib.POMADE_DIRTY_SELECTION in session.published,
          "publishing the selection locator (%r)" % (session.published,))
    check(session.statuses and
          "selection cancelled" in session.statuses[-1],
          "and says so (%r)" % (session.statuses[-1:],))
    # Cancelling a band that started from nothing selected clears it.
    session.items = {}
    loop.press(sample(50.0, 50.0))
    loop.move(sample(250.0, 250.0))
    loop.cancel()
    check(session.readSelection(tube) == [],
          "a cancelled band from an empty selection leaves it empty")
    # A completed band is not undone by a later Escape.
    loop.press(sample(50.0, 50.0))
    loop.move(sample(250.0, 250.0))
    loop.release(sample(250.0, 250.0))
    check(not loop.cancel() and session.readSelection(tube) == [(1, -1, -1)],
          "after release the band is kept")


def testSurfaceMiss(pomadeCamera, pomadeLoops, PomadeToolState):
    print("-- GraphLoop: a stroke off the scalp ---------------------")
    dll, session, state, loop = newLoop(pomadeLoops, PomadeToolState, "draw")
    session.surfaceMisses = True
    cam = topDownCamera(pomadeCamera)
    loop.press(pomadeLoops.Sample(session, cam, 10.0, 10.0))
    loop.move(pomadeLoops.Sample(session, cam, 60.0, 60.0))
    loop.release(pomadeLoops.Sample(session, cam, 90.0, 90.0))
    check(dll.count("Pomade_GraphStroke") == 0,
          "a stroke that never hit the scalp authors nothing")
    check(not session.gestureStack,
          "and still closes its bracket (%r)" % session.gestureStack)


def testRingDisplayFollowsMode(pomadeViewport, PomadeToolState):
    """V6 / plan/18 section 2.4a: rings and ring CV dots draw in Tube
    mode's Ring and Section sub-modes and on selected tubes everywhere
    else. One model flag, set by the controller on every mode and
    sub-mode change, so the publisher lays a level out once and a
    selection click still dirties nothing but the two ring prims."""
    print("-- the display policy follows the mode --------------------")
    # V9 (plan/18 section 2.4a): the whole viewport look -- x-ray strengths,
    # centers, rings -- is one C++ table, and the controller's only job is
    # to say which mode, sub-mode and level the artist is in. So this checks
    # the arguments, never the look: the table itself is asserted in
    # testUsdGenPomadeIndex, where it can be read back off the published
    # prims.
    dll = FakeDll()
    session = FakeSession(dll)
    state = PomadeToolState()
    controller = pomadeViewport.ViewportController(state, session, None)

    def policyArgs():
        return [(a[1].decode("utf-8"), a[2].decode("utf-8"), a[3])
                for a in dll.argsOf("Pomade_SetDisplayPolicy")]

    controller.setMode("graph")
    check(policyArgs()[-1:] == [("graph", "region", 1)],
          "Graph mode pushes its own id, sub-mode and level (%r)"
          % policyArgs())
    check(not dll.argsOf("Pomade_SetRingDisplay"),
          "and no longer sets the ring display behind the policy's back")
    dll.reset()
    controller.setMode("tube")
    check(policyArgs()[-1:] == [("tube", "center", 1)],
          "Tube mode arrives in its default sub-mode (%r)" % policyArgs())
    dll.reset()
    controller.setSubMode("ring")
    check(policyArgs()[-1:] == [("tube", "ring", 1)],
          "the Ring sub-mode re-resolves the table (%r)" % policyArgs())
    dll.reset()
    controller.setSubMode("section")
    check(policyArgs()[-1:] == [("tube", "section", 1)],
          "and so does the Section sub-mode (%r)" % policyArgs())
    dll.reset()
    controller.setSubMode("center")
    check(policyArgs()[-1:] == [("tube", "center", 1)],
          "leaving them re-resolves it again (%r)" % policyArgs())
    dll.reset()
    controller.setMode("sculpt")
    check(policyArgs()[-1:] == [("sculpt", "grab", 1)],
          "another mode pushes its own row (%r)" % policyArgs())
    check(session.published and session.published[-1] ==
          pomadeViewport.pomadeLib.POMADE_DIRTY_DISPLAY,
          "each change publishes the display dirty and nothing heavier (%r)"
          % session.published[-3:])
    dll.reset()
    state.activeLevel = 2
    controller._pushFocusLevel()
    check(policyArgs()[-1:] == [("sculpt", "grab", 2)],
          "entering a level re-resolves the table at the new focus (%r)"
          % policyArgs())


def testDisplayScaleHook(pomadeCamera, pomadeViewport, PomadeToolState):
    """V8 / plan/18 section 2.4a: the controller measures world units per
    screen pixel at the groom and hands it to the model.

    Storm sizes points and curves in WORLD units, so this number is the
    only thing that can make a CV dot a fixed number of pixels across.
    The controller is where it comes from because the camera is: it
    re-measures on press, on every idle tick and on the StageView's
    frustum signal."""
    print("-- the display-scale hook --------------------------------")
    dll = FakeDll()
    session = FakeSession(dll)
    state = PomadeToolState()
    controller = pomadeViewport.ViewportController(state, session, None)
    cam = topDownCamera(pomadeCamera)

    check(controller.syncDisplayScale(cam) is True,
          "syncDisplayScale takes a camera and answers True")
    expected = cam.worldPerPixel(session.scalpCenter)
    check(expected > 0.0, "the fixture camera has a real pixel size (%r)"
          % expected)
    check(session.displayScales[-1:] == [expected],
          "and pushes exactly that into the model (%r)"
          % session.displayScales[-1:])

    # Half the pixels across the same world: twice the world per pixel.
    zoomed = topDownCamera(pomadeCamera, width=200, height=200)
    controller.syncDisplayScale(zoomed)
    check(abs(session.displayScales[-1] - 2.0 * expected) < 1e-9,
          "a camera showing the same groom in half the pixels doubles it "
          "(%r)" % session.displayScales[-1])

    before = list(session.displayScales)
    session.scalpCenter = None
    check(controller.syncDisplayScale(cam) is False and
          session.displayScales == before,
          "with no scalp bound there is nothing to measure at (%r)"
          % session.displayScales[len(before):])
    session.scalpCenter = (2.0, 0.0, 2.0)
    session.model = None
    check(controller.syncDisplayScale(cam) is False and
          session.displayScales == before,
          "and with no model there is nothing to tell (%r)"
          % session.displayScales[len(before):])


def testOutputModeSwitches(pomadeLoops, pomadeViewport, PomadeToolState):
    """V7: Output is a panel mode, and switching to it is not a refusal.

    It carried a "not built yet: V3" status from the phase when the mode
    shelf existed and the dock did not. The dock is built, so the status
    is the mode's own line and the state records the mode like any
    other."""
    print("-- Output mode -------------------------------------------")
    dll = FakeDll()
    session = FakeSession(dll)
    state = PomadeToolState()
    controller = pomadeViewport.ViewportController(state, session, None)
    controller.setMode("tube")
    status = controller.setMode("output")
    check(state.activeMode == "output",
          "the shelf records Output as active (%r)" % state.activeMode)
    check(status == "Pomade: Output mode: build the hair description, bake "
          "resolution, strand density and width.",
          "and reports the mode's own status line (%r)" % status)
    check("save" not in status.lower(),
          "which no longer sends the artist to Output for Save (it is on "
          "the always-visible file row, DK-03) (%r)" % status)
    check("not built" not in status,
          "with no stale refusal in it (%r)" % status)
    check(controller.loop is None,
          "Output runs no loop (%r)" % (controller.loop,))
    # Output must not acquire a sub-mode: it has no shelf to put one on.
    check(pomadeLoops.subModesFor("output") == () and
          getattr(state, "outputSubMode", "") == "",
          "Output takes no sub-mode (%r)"
          % (getattr(state, "outputSubMode", ""),))


def testViewportExceptionRecovery(pomadeViewport, PomadeToolState):
    """The controller owns cleanup when a Qt callback raises mid-gesture.

    This is deliberately Qt-stubbed rather than a loop-only test: the
    exception handlers in `onPress` and `onRelease` are the boundary that
    used to leave the next real click permanently captured.
    """
    print("-- viewport: callback exception recovery ------------------")

    class _MouseButton(object):
        LeftButton = 1
        RightButton = 2
        MiddleButton = 4
        NoButton = 0

    class _KeyboardModifier(object):
        ShiftModifier = 1 << 0
        ControlModifier = 1 << 1
        AltModifier = 1 << 2
        MetaModifier = 1 << 3

    qtCore = types.SimpleNamespace(
        Qt=types.SimpleNamespace(MouseButton=_MouseButton,
                                 KeyboardModifier=_KeyboardModifier))
    pxr = types.ModuleType("pxr")
    usdviewq = types.ModuleType("pxr.Usdviewq")
    qt = types.ModuleType("pxr.Usdviewq.qt")
    qt.QtCore = qtCore
    oldModules = {name: sys.modules.get(name) for name in
                  ("pxr", "pxr.Usdviewq", "pxr.Usdviewq.qt")}
    sys.modules["pxr"] = pxr
    sys.modules["pxr.Usdviewq"] = usdviewq
    sys.modules["pxr.Usdviewq.qt"] = qt

    class _View(object):
        def devicePixelRatioF(self):
            return 1.0

    class _Event(object):
        def button(self):
            return _MouseButton.LeftButton

        def modifiers(self):
            return 0

        def x(self):
            return 20.0

        def y(self):
            return 30.0

    class _FailingLoop(object):
        modeId = "tube"

        def __init__(self, failure):
            self.failure = failure
            self.cancelCalls = 0

        def press(self, _sample):
            if self.failure == "press":
                raise RuntimeError("press exploded")
            return True

        def release(self, _sample):
            if self.failure == "release":
                raise RuntimeError("release exploded")
            return True

        def cancel(self):
            self.cancelCalls += 1
            return True

    oldResolve = pomadeViewport.pomadeCamera.resolve
    try:
        pomadeViewport.pomadeCamera.resolve = lambda _view: object()

        dll = FakeDll()
        session = FakeSession(dll)
        controller = pomadeViewport.ViewportController(PomadeToolState(),
                                                       session, None)
        controller.syncDisplayScale = lambda _camera: False
        pressLoop = _FailingLoop("press")
        controller._loop = pressLoop
        check(not controller.onPress(_View(), _Event()) and
              pressLoop.cancelCalls == 1 and not controller.gestureActive,
              "a press exception force-cancels its pre-gesture loop state")

        idleDraftLoop = _FailingLoop("")
        controller._loop = idleDraftLoop
        check(controller.cancelGesture() and idleDraftLoop.cancelCalls == 1,
              "Escape reaches an idle loop draft without mouse capture")

        releaseLoop = _FailingLoop("release")
        controller._loop = releaseLoop
        controller._gesture = True
        controller._lastXY = (20.0, 30.0)
        check(not controller.onRelease(_View(), _Event()) and
              releaseLoop.cancelCalls == 1 and not controller.gestureActive
              and controller._lastXY is None,
              "a release exception cancels and resets controller capture")
        check(len(session.published) >= 2,
              "both exception paths republish their recovered state %r" %
              session.published)

        # A loop whose cancel raises after its press opened the session
        # bracket: the controller still closes that bracket, or every
        # later Begin is refused until a rebind.
        class _LeakyLoop(_FailingLoop):
            def press(self, _sample):
                session.beginGesture("Tube move")
                return True

            def cancel(self):
                self.cancelCalls += 1
                raise RuntimeError("cancel exploded")

        leaky = _LeakyLoop("")
        controller._loop = leaky
        session.events = []
        session.published = []
        check(controller.onPress(_View(), _Event()) and
              session.gestureActive, "the leaky loop's press opens a bracket")
        controller.cancelGesture()
        check(leaky.cancelCalls == 1 and not session.gestureActive and
              ("cancel", None) in session.events and
              not controller.gestureActive,
              "Escape over a raising cancel still rolls the bracket back "
              "(%r)" % (session.events,))
        check(session.published,
              "and publishes what the cancel restored (%r)"
              % (session.published,))
        check(any("rolled it back" in text for text in session.statuses),
              "the status says the edit was rolled back (%r)"
              % (session.statuses[-2:],))
        check(controller.onPress(_View(), _Event()) and
              session.gestureStack == ["Tube move"],
              "and the next press can open its own bracket (%r)"
              % (session.gestureStack,))
        session.gestureStack = []
        controller._resetGestureState()

        # An idle Escape (no capture) whose loop cancel returns normally
        # leaves another owner's bracket (a held dock slider) alone.
        session.gestureStack = ["Dock slider"]
        controller._loop = _FailingLoop("")
        controller.cancelGesture()
        check(session.gestureStack == ["Dock slider"],
              "an idle Escape leaves a foreign bracket open (%r)"
              % (session.gestureStack,))
        session.gestureStack = []
    finally:
        pomadeViewport.pomadeCamera.resolve = oldResolve
        for name, prior in oldModules.items():
            if prior is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = prior


def testViewportFallbackHierarchyKeys(pomadeViewport, PomadeToolState):
    """Shift+D / Shift+M from Tube/Graph/Fill/Sculpt run the controller's
    own forms; they follow HierarchyLoop's bracket rules: a refused Begin
    runs nothing, and an action that changed nothing leaves no step."""
    print("-- controller: fallback Shift+D / Shift+M brackets --------")
    from usdGenPomadeTools import pomadeLib, pomadeModes

    class HierDll(FakeDll):
        def __init__(self):
            FakeDll.__init__(self)
            self.children = {}
            self.subdivideRc = 0
            # Plain functions, not bound methods: pomadeHierarchy writes
            # ctypes argtypes/restype onto the entry it is handed.
            for name in ("Pomade_GetTubeCount", "Pomade_GetTubeChildren",
                         "Pomade_MergeChildren", "Pomade_SubdivideTube"):
                bound = getattr(self, "_" + name)

                def entry(*args, _bound=bound):
                    return _bound(*args)
                setattr(self, name, entry)

        def _Pomade_GetTubeCount(self, _model):
            return 8

        def _Pomade_GetTubeChildren(self, _model, tubeId, out, cap, count):
            self._record("Pomade_GetTubeChildren", (int(tubeId),))
            kids = self.children.get(int(tubeId), [])
            for i, value in enumerate(kids[:cap]):
                out[i] = value
            getattr(count, "_obj", count).value = min(len(kids), cap)
            return 0

        def _Pomade_MergeChildren(self, _model, tubeId):
            self._record("Pomade_MergeChildren", (int(tubeId),))
            self.children.pop(int(tubeId), None)
            return 0

        def _Pomade_SubdivideTube(self, _model, tubeId, *rest):
            self._record("Pomade_SubdivideTube", (int(tubeId),))
            if self.subdivideRc:
                return self.subdivideRc
            got = rest[-1]
            getattr(got, "_obj", got).value = 0
            return 0

    dll = HierDll()
    session = FakeSession(dll)
    state = PomadeToolState()
    controller = pomadeViewport.ViewportController(state, session, None)
    controller._loop = None             # the Tube-mode fallback route
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT
    session.selection = {TUBE: [0, 3]}

    # Shift+M over leaves: the merges succeed doing nothing -> no step.
    session.events = []
    check(not controller.runAction(pomadeModes.ACTION_MERGE),
          "Shift+M over leaf tubes reports nothing done")
    names = [name for name, _ in session.events]
    check("cancel" in names and "end" not in names and
          "enqueueCommit" not in names and not session.gestureActive,
          "and cancels its bracket instead of sealing an empty step (%r)"
          % (names,))
    check(dll.count("Pomade_MergeChildren") == 0,
          "a leaf is never handed to Pomade_MergeChildren")
    check("nothing merged" in session.statuses[-1],
          "the status says nothing merged (%r)" % session.statuses[-1:])

    # Shift+M over a real parent: one sealed step and a commit.
    dll.children = {0: [1, 2]}
    session.events = []
    check(controller.runAction(pomadeModes.ACTION_MERGE),
          "Shift+M over a parent merges")
    names = [name for name, _ in session.events]
    check(names.count("begin") == 1 and names.count("end") == 1 and
          "cancel" not in names and "enqueueCommit" in names,
          "as one sealed undo step and a commit (%r)" % (names,))

    # Shift+D where every split is refused: no step.
    dll.subdivideRc = 5
    session.events = []
    check(not controller.runAction(pomadeModes.ACTION_SUBDIVIDE),
          "an all-refused Shift+D reports failure")
    names = [name for name, _ in session.events]
    check("cancel" in names and "end" not in names and
          "enqueueCommit" not in names and not session.gestureActive,
          "and cancels its bracket (%r)" % (names,))
    check("nothing split" in session.statuses[-1] and
          "T0" in session.statuses[-1],
          "with the refusal in the status (%r)" % session.statuses[-1:])

    # A refused Begin (a drag's bracket is open): nothing runs, and the
    # other owner's bracket is left alone.
    dll.subdivideRc = 0
    dll.children = {0: [1, 2]}
    dll.reset()
    session.events = []
    session.gestureStack = ["Tube move"]
    realBegin = session.beginGesture
    session.beginGesture = lambda label: False
    try:
        check(not controller.runAction(pomadeModes.ACTION_SUBDIVIDE) and
              not controller.runAction(pomadeModes.ACTION_MERGE),
              "a refused Begin refuses Shift+D and Shift+M")
    finally:
        session.beginGesture = realBegin
    names = [name for name, _ in session.events]
    check(session.gestureStack == ["Tube move"] and "end" not in names and
          "cancel" not in names,
          "without sealing or cancelling the drag's bracket (%r, %r)"
          % (session.gestureStack, names))
    check(dll.count("Pomade_SubdivideTube") == 0 and
          dll.count("Pomade_MergeChildren") == 0,
          "and without reaching the ABI (%r)" % (dll.names(),))
    check("undo step" in session.statuses[-1],
          "the status says why (%r)" % session.statuses[-1:])
    session.gestureStack = []


def testSculptBrushResizeKeyLifecycle(pomadeViewport, PomadeToolState):
    """F remains owned throughout a held Sculpt brush-width gesture."""
    print("-- viewport: Sculpt F brush-width key lifecycle ------------")

    class _Loop(object):
        modeId = "sculpt"

    class _Event(object):
        def __init__(self, repeating=False):
            self._repeating = repeating

        def isAutoRepeat(self):
            return self._repeating

    oldKeyName = pomadeViewport.keyName
    oldModifiers = pomadeViewport.modifierSet
    try:
        pomadeViewport.keyName = lambda _event: "f"
        pomadeViewport.modifierSet = lambda _event: frozenset()
        state = PomadeToolState(workspaceOpen=True, activeMode="sculpt")
        controller = pomadeViewport.ViewportController(state, FakeSession(FakeDll()),
                                                       None)
        controller._installed = True
        controller._pointerInside = True
        controller._loop = _Loop()
        controller._textFocus = lambda: False

        check(controller.onKey(_Event()) and controller._brushResizeArmed,
              "F arms brush-width resize only in the active Sculpt viewport")
        controller._gesture = True
        controller._brushResizeActive = True
        check(controller.onKey(_Event()),
              "F key-repeat stays consumed during a live resize")
        check(controller.onKeyRelease(_Event(True)) and
              controller._brushResizeArmed,
              "an auto-repeat F release cannot disarm an active resize")
        controller._gesture = False
        controller._brushResizeActive = False
        check(controller.onKeyRelease(_Event()) and
              not controller._brushResizeArmed,
              "the physical F release clears the resize arm")
    finally:
        pomadeViewport.keyName = oldKeyName
        pomadeViewport.modifierSet = oldModifiers


def testKeyPressLatch(pomadeViewport, PomadeToolState):
    """One physical press runs onKey once, however many widgets the
    declined KeyPress propagates through (usdRig _claimedKey)."""
    print("-- viewport: once-per-press key latch ----------------------")

    class _Event(object):
        def __init__(self, code, mods=(), repeating=False):
            self.code = code
            self.mods = frozenset(mods)
            self._repeating = repeating

        def key(self):
            return self.code

        def isAutoRepeat(self):
            return self._repeating

    ENTER, Z, SHIFT = 0x01000004, 0x5a, 0x01000020
    oldModifiers = pomadeViewport.modifierSet
    try:
        pomadeViewport.modifierSet = lambda event: event.mods
        controller = pomadeViewport.ViewportController(
            PomadeToolState(workspaceOpen=True), FakeSession(FakeDll()), None)
        calls = []
        answers = {}

        def onKey(event):
            calls.append((event.code, event.mods))
            answer = answers.get(event.code, False)
            if isinstance(answer, Exception):
                raise answer
            return answer

        controller.onKey = onKey
        controller.onKeyRelease = lambda _event: False

        # A declined Enter: Qt's override, then the KeyPress bubbling up
        # seven widgets.  onKey runs once; every copy passes on (False).
        controller.keyOverride(_Event(ENTER))
        results = [controller.deliverKeyPress(_Event(ENTER))
                   for _ in range(7)]
        check(len(calls) == 1 and not any(results),
              "a declined Enter delivered 7 times runs onKey once and "
              "passes every copy on to usdview (%d calls, %r)"
              % (len(calls), results))
        controller.deliverKeyRelease(_Event(ENTER))
        controller.keyOverride(_Event(ENTER))
        controller.deliverKeyPress(_Event(ENTER))
        check(len(calls) == 2, "the next Enter press runs onKey again (%d)"
              % len(calls))
        controller.deliverKeyRelease(_Event(ENTER))

        # Ctrl+Z then Shift+Z (both refused mid-drag): two presses, two
        # runs, with QTest's modifier-key presses in between.
        del calls[:]
        for mods in (("ctrl",), ("shift",)):
            controller.keyOverride(_Event(Z, mods))
            for _ in range(7):
                controller.deliverKeyPress(_Event(Z, mods))
            controller.deliverKeyRelease(_Event(Z, mods))
        check(calls == [(Z, frozenset(("ctrl",))), (Z, frozenset(("shift",)))],
              "Ctrl+Z and Shift+Z run onKey once each (%r)" % (calls,))

        # A claimed key answers True to a repeat delivery without acting.
        del calls[:]
        answers[Z] = True
        controller.keyOverride(_Event(Z))
        check(controller.deliverKeyPress(_Event(Z)) and
              controller.deliverKeyPress(_Event(Z)) and len(calls) == 1,
              "a claimed key stays swallowed for its later deliveries")
        # OS auto-repeat: an override precedes every repeated press.
        controller.keyOverride(_Event(Z, repeating=True))
        controller.deliverKeyPress(_Event(Z, repeating=True))
        controller.deliverKeyRelease(_Event(Z, repeating=True))
        controller.deliverKeyPress(_Event(Z, repeating=True))
        check(len(calls) == 2,
              "each auto-repeat press acts once; an auto-repeat release "
              "keeps the latch (%d)" % len(calls))
        controller.deliverKeyRelease(_Event(Z))
        answers[Z] = False

        # Straight-at-the-view events with no override: a different key
        # or modifier set, or the key's release, starts a new press.
        del calls[:]
        controller.deliverKeyPress(_Event(SHIFT, ("shift",)))
        controller.deliverKeyPress(_Event(Z, ("shift",)))
        controller.deliverKeyPress(_Event(Z, ("shift",)))
        controller.deliverKeyPress(_Event(Z))
        controller.deliverKeyRelease(_Event(Z))
        controller.deliverKeyPress(_Event(Z))
        check(len(calls) == 4,
              "no override: key, modifiers or a release separate presses "
              "(%d calls)" % len(calls))
        controller.deliverKeyRelease(_Event(Z))

        # A raising onKey is still its press's one run.
        del calls[:]
        answers[ENTER] = RuntimeError("boom")
        raised = 0
        controller.keyOverride(_Event(ENTER))
        for _ in range(3):
            try:
                controller.deliverKeyPress(_Event(ENTER))
            except RuntimeError:
                raised += 1
        check(raised == 1 and len(calls) == 1,
              "a raising onKey reports once per press (%d raised, %d calls)"
              % (raised, len(calls)))

        # The Qt-free latch on its own.
        latch = pomadeViewport.KeyPressLatch()
        ran = []
        latch.press((1, frozenset()), lambda: ran.append(1) or False)
        latch.press((1, frozenset()), lambda: ran.append(1) or False)
        check(latch.latched == (1, frozenset()) and len(ran) == 1,
              "KeyPressLatch acts on the first delivery only")
        latch.release(2)
        check(latch.latched is not None, "another key's release keeps it")
        latch.release(1)
        check(latch.latched is None, "its own release clears it")
    finally:
        pomadeViewport.modifierSet = oldModifiers


def testGizmoHoldsAndUndoGuard(pomadeViewport, PomadeToolState):
    """Parity G11 / G14: the J/X holds belong to a live gizmo drag only;
    undo/redo refuse mid-drag."""
    print("-- controller: snap holds, undo refused mid-drag --------")
    from usdGenPomadeTools import pomadeModes

    class HoldLoop(object):
        modeId = "tube"

        def __init__(self):
            self.dragging = True
            self.moves = []

        def gizmoDragActive(self):
            return self.dragging

        def move(self, sample):
            self.moves.append((sample.x, sample.y, sample.modifiers))
            return True

    class UndoSession(FakeSession):
        def __init__(self, dll):
            FakeSession.__init__(self, dll)
            self.undos = 0

        def undo(self):
            self.undos += 1
            return True

        def redo(self):
            self.undos += 1
            return True

    state = PomadeToolState()
    state.workspaceOpen = True
    session = UndoSession(FakeDll())
    controller = pomadeViewport.ViewportController(state, session, None)
    loop = HoldLoop()
    controller._loop = loop
    controller._installed = True
    controller._textFocus = lambda: False    # no Qt in a T1
    controller._camera = object()
    check(not controller._pressHold("j", frozenset()),
          "J is usdview's while no drag is live")
    controller._gesture = True
    controller._lastXY = (40.0, 30.0)
    check(controller._pressHold("j", frozenset()) and
          controller.holdActive("stepSnap") and not loop.moves,
          "J during a gizmo drag is claimed; an unmoved drag is left alone")
    controller._gestureMoved = True
    check(controller._pressHold("x", frozenset(["ctrl"])) and
          loop.moves and loop.moves[-1][:2] == (40.0, 30.0) and
          {"stepSnap", "grid", "ctrl"} <= set(loop.moves[-1][2]),
          "X re-applies the live drag at once with both holds (%r)"
          % (loop.moves[-1:],))
    count = len(loop.moves)
    check(controller._pressHold("x", frozenset()) and
          len(loop.moves) == count,
          "OS key repeat of a held key does not re-apply")
    check(controller._releaseHold("x", frozenset(), autoRepeat=True) and
          controller.holdActive("grid"),
          "an auto-repeat release keeps the hold")
    check(controller._releaseHold("x", frozenset()) and
          not controller.holdActive("grid") and
          "grid" not in loop.moves[-1][2] and
          "stepSnap" in loop.moves[-1][2],
          "releasing X un-snaps at once and keeps J (%r)"
          % (loop.moves[-1:],))
    controller._holds.clear()
    check(not controller._pressHold("j", frozenset(["alt"])) and
          not controller.holdActive("stepSnap"),
          "Alt+J stays the camera's")
    loop.dragging = False
    check(not controller._pressHold("j", frozenset()),
          "a marquee (no gizmo drag) leaves J to usdview")
    loop.dragging = True
    controller._pressHold("j", frozenset())
    check(not controller.runAction(pomadeModes.ACTION_UNDO) and
          not controller.runAction(pomadeModes.ACTION_REDO) and
          session.undos == 0,
          "undo and redo refuse while the drag is live")
    check(session.statuses and "finish the drag" in session.statuses[-1],
          "and say why (%r)" % (session.statuses[-1:],))
    controller._resetGestureState()
    check(not controller._holds and not controller._gestureMoved,
          "the drag's end clears every hold")
    check(controller.runAction(pomadeModes.ACTION_UNDO) and
          session.undos == 1, "after the drag undo runs")


def testSelectionKeys(pomadeViewport, pomadeModes, pomadeLoopsTube,
                      PomadeToolState):
    """SL-03: Escape keeps the selection; Ctrl+A/Ctrl+Shift+A/Ctrl+I; the
    Backspace route through the loop's exitLevel."""
    print("-- viewport: Escape / select all / invert / Backspace -----")
    import usdGenPomadeTools.pomadeLib as pomadeLib
    NODE = pomadeLib.POMADE_PICK_GRAPH_NODE
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT

    class _Camera(object):
        width = 400
        height = 300

    class _Loop(object):
        modeId = "graph"
        label = "Graph"

        def __init__(self):
            self.disarms = []
            self.exits = 0

        def cancel(self):
            return False                 # nothing live

        def _disarm(self, clearSelection=True):
            self.disarms.append(clearSelection)
            return False

        def exitLevel(self):
            self.exits += 1
            return True

    def rows(*ids):
        return [(i, -1, -1) for i in ids]

    oldResolve = pomadeViewport.pomadeCamera.resolve
    try:
        pomadeViewport.pomadeCamera.resolve = lambda _view: _Camera()
        session = MatrixSession(FakeDll())
        controller = pomadeViewport.ViewportController(PomadeToolState(),
                                                       session, None)
        controller._view = object()
        loop = _Loop()
        controller._loop = loop

        session.items = {NODE: rows(1, 2)}
        check(not controller.cancelGesture() and
              session.readSelection(NODE) == rows(1, 2),
              "Escape with nothing live keeps the selection (%r)"
              % session.readSelection(NODE))

        session.bandHits = {NODE: rows(2, 3)}
        session.rects = []
        check(controller.runAction(pomadeModes.ACTION_SELECT_ALL) and
              session.readSelection(NODE) == rows(2, 3),
              "Ctrl+A selects every node a whole-view band covers (%r)"
              % session.readSelection(NODE))
        check(session.rects and session.rects[-1][:5] ==
              (0.0, 0.0, 400.0, 300.0, NODE),
              "over the whole view and only Graph's node kind (%r)"
              % session.rects[-1:])
        check(loop.disarms == [False],
              "and it disarms a pending two-click pick without clearing")

        session.items = {NODE: rows(1, 2)}
        check(controller.runAction(pomadeModes.ACTION_INVERT) and
              session.readSelection(NODE) == rows(1, 3),
              "Ctrl+I flips what the view covers, keeps the rest (%r)"
              % session.readSelection(NODE))

        session.items = {NODE: rows(1, 2), TUBE: rows(0)}
        check(controller.runAction(pomadeModes.ACTION_DESELECT_ALL) and
              session.selectionCount(0) == 0,
              "Ctrl+Shift+A clears every kind (%r)" % session.items)

        # A whole tube is hit once per tessellated vertex; the invert must
        # still flip it exactly once.
        session.items = {TUBE: rows(0)}
        session.bandHits = {TUBE: rows(0, 0, 0, 16, 16)}
        check(pomadeLoopsTube.invertBand(
            session, TUBE, pomadeLoopsTube.viewBand(session, _Camera(), TUBE))
              and session.readSelection(TUBE) == rows(16),
              "invert over repeated per-vertex tube hits flips each tube once"
              " (%r)" % session.readSelection(TUBE))

        loop.modeId = "sculpt"
        session.items = {NODE: rows(1)}
        check(not controller.runAction(pomadeModes.ACTION_SELECT_ALL) and
              session.readSelection(NODE) == rows(1),
              "Sculpt has no selection, so Ctrl+A is not its")

        check(controller.runAction(pomadeModes.ACTION_BACKSPACE) and
              loop.exits == 1,
              "Backspace goes through the loop's exitLevel (the active-cut"
              " collapse), not the plain level step")
    finally:
        pomadeViewport.pomadeCamera.resolve = oldResolve


def testLadderControlsAndFeedback(pomadeViewport, PomadeToolState):
    """FB-02: the ladder honours ladderEnabled/moveBudgetMs, its hover-off
    rung is a documented post-release cool-down, the HUD chip reads the
    rung; the cursor table, band tint and HUD title are Qt-free."""
    print("-- FB-02: ladder controls, cursors, band tint, HUD --------")
    from usdGenPomadeTools import pomadeLadder
    tv = pomadeViewport

    # The hover-off rung is kept and given a meaning: MAX_STEP stays 6 and
    # the release from it holds hover off for HOVER_COOLDOWN_MS.
    with open(pomadeLadder.__file__, encoding="utf-8") as stream:
        documented = "cool-down" in stream.read()
    check(pomadeLadder.MAX_STEP == pomadeLadder.STEP_HOVER_OFF == 6 and
          pomadeLadder.HOVER_COOLDOWN_MS == 500.0 and documented,
          "the hover-off rung is kept with a documented 500 ms cool-down")
    check(pomadeLadder.chipLabel(0) == "" and
          pomadeLadder.chipLabel(2) == "Preview 10 % (auto)" and
          pomadeLadder.chipLabel(99) == "Hover off (auto)",
          "the HUD chip names the rung: %r" % pomadeLadder.chipLabel(2))

    # No session: the rungs have nothing to write, the stepping is pure.
    state = PomadeToolState()
    state.ladderEnabled = False
    ladder = pomadeLadder.FallbackLadder(None, state)
    check(not ladder.arm(1) and not ladder.armed,
          "with ladderEnabled off a press does not arm the ladder")
    check(not any(ladder.noteMove(50.0) for _ in range(9)) and
          ladder.step == 0,
          "so a slow drag keeps full fidelity (step %d)" % ladder.step)
    check(not ladder.restore(), "and its release has nothing to restore")

    state.ladderEnabled = True
    state.moveBudgetMs = 0.0
    check(ladder.arm(1) and ladder.budgetMs == 0.0,
          "the next press arms with the state's moveBudgetMs (%g)"
          % ladder.budgetMs)
    for _ in range(6):
        ladder.noteMove(0.5)
    check(ladder.step == 2 and
          ladder.chipLabel() == "Preview 10 % (auto)" and
          int(state.ladderStep) == 2,
          "a zero budget steps on any move time (step %d, %r)"
          % (ladder.step, ladder.chipLabel()))
    check(ladder.restore(1000.0) and not ladder.hoverCoolingDown(1000.0),
          "a release short of the hover-off rung starts no cool-down")
    ladder.arm(1)
    for _ in range(3 * pomadeLadder.MAX_STEP):
        ladder.noteMove(0.5)
    check(ladder.hoverSuppressed, "a heavy drag reaches hover off")
    ladder.restore(2000.0)
    check(not ladder.hoverSuppressed and ladder.hoverCoolingDown(2200.0) and
          not ladder.hoverCoolingDown(2500.0) and
          abs(ladder.hoverCooldownRemainingMs(2100.0) - 400.0) < 1e-6,
          "its release keeps hover off for 500 ms, then gives it back")
    state.moveBudgetMs = 8.0
    fixed = pomadeLadder.FallbackLadder(None, state, budgetMs=3.0)
    fixed.arm(1)
    check(fixed.budgetMs == 3.0,
          "an explicit constructor budget still pins the ladder")

    # The cursor table: (mode, subMode, tool, hoverHandle) plus the drag.
    rows = (
        (("graph", "region", "move", -1), {}, tv.CURSOR_CROSS),
        (("graph", "draw", "move", -1), {}, tv.CURSOR_CROSS),
        (("graph", "place", "move", -1), {}, tv.CURSOR_CROSS),
        (("graph", "reposition", "move", -1), {}, tv.CURSOR_CROSS),
        (("graph", "connect", "move", -1), {}, tv.CURSOR_ARROW),
        (("graph", "reposition", "move", -1), {"dragging": True},
         tv.CURSOR_CLOSED_HAND),
        (("hierarchy", "subdivide", "move", -1), {"edgeStroke": True},
         tv.CURSOR_CROSS),
        (("hierarchy", "subdivide", "move", -1), {}, tv.CURSOR_ARROW),
        (("sculpt", "grab", "move", -1), {}, tv.CURSOR_CROSS),
        (("sculpt", "grab", "move", -1), {"ring": True}, tv.CURSOR_BLANK),
        (("tube", "center", "move", 2), {}, tv.CURSOR_SIZE_ALL),
        (("tube", "center", "select", 2), {}, tv.CURSOR_ARROW),
        (("tube", "center", "move", -1), {}, tv.CURSOR_ARROW),
        (("tube", "center", "move", 2), {"dragging": True},
         tv.CURSOR_CLOSED_HAND),
        (("tube", "center", "move", -1), {"band": True}, tv.CURSOR_CROSS),
        (("fill", "params", "move", -1), {}, tv.CURSOR_ARROW),
        (("output", "", "move", -1), {}, tv.CURSOR_ARROW),
    )
    for args, flags, want in rows:
        got = tv.cursorFor(*args, **flags)
        check(got == want, "cursor %r %r -> %s (%s)" % (args, flags, want,
                                                        got))

    for mods, want in ((frozenset(), tv.BAND_REPLACE),
                       (frozenset(["shift"]), tv.BAND_ADD),
                       (frozenset(["ctrl"]), tv.BAND_REMOVE),
                       (frozenset(["ctrl", "shift"]), tv.BAND_ADD)):
        check(tv.bandRecordFor(mods) == want,
              "a band with %s is tinted %s" % (sorted(mods) or "no key",
                                               want))
    check(tv.BAND_GLYPH[tv.BAND_ADD] == "+" and
          tv.BAND_GLYPH[tv.BAND_REMOVE] == "−" and
          not tv.BAND_GLYPH[tv.BAND_REPLACE],
          "add/remove bands carry a +/- glyph, replace none")

    hud = PomadeToolState(activeMode="tube", tubeSubMode="center",
                         transformTool="move")
    check(tv.hudTitle(hud) == "Tube › Center CV › Move",
          "the HUD title is Mode > Sub-mode > Tool (%r)" % tv.hudTitle(hud))
    check(tv.hudHint(hud) == tv.pomadeModes.hintFor("tube", "center"),
          "the HUD hint is the dock's instruction line")
    check(tv.hudTitle(PomadeToolState(activeMode="sculpt",
                                     sculptSubMode="smooth")) ==
          "Sculpt › Smooth" and tv.hudTitle(PomadeToolState()) == "",
          "only Tube names a transform tool; no mode, no title")

    # The controller's chip: the live rung, then ~1 s marked restored.
    controller = tv.ViewportController(PomadeToolState(moveBudgetMs=0.0),
                                       None, None)
    ladder = controller._ladder
    ladder.arm(1)
    for _ in range(6):
        ladder.noteMove(0.5)
    check(controller.ladderChipText() == "Preview 10 % (auto)",
          "the controller's chip shows the live rung (%r)"
          % controller.ladderChipText())
    controller._resetGestureState()
    lingering = controller.ladderChipText()
    check(lingering.startswith("Preview 10 % (auto)") and
          controller._ladderChipLinger is not None,
          "after the release it lingers, marked restored (%r)" % lingering)
    controller._ladderChipLinger = (lingering, 0.0)
    check(controller.ladderChipText() == "",
          "and goes once its linger has run out")


def main():
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import pomadeTestPackage
    pomadeTestPackage.install()
    from usdGenPomadeTools import (pomadeCamera, pomadeGizmo, pomadeLoops,
                                  pomadeModes, pomadeSession, pomadeViewport)
    from usdGenPomadeTools.pomadeToolState import PomadeToolState
    testCamera(pomadeCamera)
    testHotkeys(pomadeModes)
    testModesShelf(pomadeModes, pomadeLoops)
    testGizmo(pomadeCamera, pomadeGizmo)
    testGizmoLook(pomadeCamera, pomadeGizmo, pomadeViewport)
    testSessionRegionTubeDefaults(pomadeSession, PomadeToolState)
    testGraphDraw(pomadeCamera, pomadeLoops, PomadeToolState)
    testGraphRegion(pomadeCamera, pomadeLoops, PomadeToolState)
    testGraphReposition(pomadeCamera, pomadeLoops, PomadeToolState)
    testGraphPlaceAndClicks(pomadeCamera, pomadeLoops, PomadeToolState)
    testGraphHoverMarqueeKeys(pomadeCamera, pomadeLoops, PomadeToolState)
    testSelectModeTable(pomadeLoops)
    testGraphSelectionMatrix(pomadeCamera, pomadeLoops, PomadeToolState)
    testGraphEmptyDragAndTwoClickCancel(pomadeCamera, pomadeLoops,
                                        PomadeToolState)
    testGraphPolish(pomadeCamera, pomadeLoops, PomadeToolState)
    testFillCancelRestores(pomadeCamera, pomadeLoops, PomadeToolState)
    testSurfaceMiss(pomadeCamera, pomadeLoops, PomadeToolState)
    testRingDisplayFollowsMode(pomadeViewport, PomadeToolState)
    testDisplayScaleHook(pomadeCamera, pomadeViewport, PomadeToolState)
    testOutputModeSwitches(pomadeLoops, pomadeViewport, PomadeToolState)
    testViewportExceptionRecovery(pomadeViewport, PomadeToolState)
    testViewportFallbackHierarchyKeys(pomadeViewport, PomadeToolState)
    testSculptBrushResizeKeyLifecycle(pomadeViewport, PomadeToolState)
    testKeyPressLatch(pomadeViewport, PomadeToolState)
    testGizmoHoldsAndUndoGuard(pomadeViewport, PomadeToolState)
    from usdGenPomadeTools import pomadeLoopsTube
    testSelectionKeys(pomadeViewport, pomadeModes, pomadeLoopsTube,
                      PomadeToolState)
    testLadderControlsAndFeedback(pomadeViewport, PomadeToolState)
    print("testUsdGenPomadeToolsLoops: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
