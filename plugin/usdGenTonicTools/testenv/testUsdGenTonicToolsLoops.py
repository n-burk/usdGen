#!/usr/bin/env python
# testUsdGenTonicToolsLoops -- T0 for the V2 viewport controller's Qt-free
# half (plan/18 section 4 "T0"): the camera maths, the hotkey table, the
# gizmo state machine and GraphLoop driven end to end over a fake session
# that records every C call.
#
#   python plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsLoops.py
#
# No usdview, no pxr, no DLL, no numpy: the modules under test are the ones
# plan/08 section 1.2 keeps Qt-free, and this is the test that keeps them
# honest about it. What it proves that a T3 cannot:
#
#   * every stroke sample costs one Tonic_Raycast and zero Hydra picks
#     (plan/18 finding F6), and every node/edge/region hit is a K11
#     Tonic_PickItem;
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
    """Records every entry point the loops call; returns TONIC_OK."""

    def __init__(self):
        self.calls = []
        self.snapRadius = 0.05
        self.nextNodeId = 100
        self.strokeNodes = 4
        self.strokeClosed = 1
        self.faceRegionIds = [0, 0, 1, 1]

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

    def Tonic_GetSnapRadius(self, _model):
        self._record("Tonic_GetSnapRadius")
        return self.snapRadius

    def Tonic_GraphStroke(self, _model, faces, uvs, samples, snap, eps, out,
                          _maxOut, count, closed, weldStart, weldEnd):
        self._record("Tonic_GraphStroke",
                     (samples, snap.value, eps.value,
                      [faces[i] for i in range(samples)]))
        for i in range(self.strokeNodes):
            out[i] = i
        _deref(count).value = self.strokeNodes
        _deref(closed).value = self.strokeClosed
        _deref(weldStart).value = 0
        _deref(weldEnd).value = 0
        return 0

    def Tonic_GraphAddNode(self, _model, face, u, v, outId):
        self.nextNodeId += 1
        self._record("Tonic_GraphAddNode", (face, u.value, v.value,
                                            self.nextNodeId))
        _deref(outId).value = self.nextNodeId
        return 0

    def Tonic_GraphMoveNode(self, _model, nodeId, face, u, v):
        self._record("Tonic_GraphMoveNode", (nodeId, face, u.value, v.value))
        return 0

    def Tonic_GraphWeld(self, _model, keep, drop):
        self._record("Tonic_GraphWeld", (keep, drop))
        return 0

    def Tonic_GraphConnect(self, _model, a, b, outEdge):
        self._record("Tonic_GraphConnect", (a, b))
        _deref(outEdge).value = 7
        return 0

    def Tonic_GraphSplitEdge(self, _model, edgeId, face, u, v, outNode):
        self._record("Tonic_GraphSplitEdge", (edgeId, face))
        _deref(outNode).value = 55
        return 0

    def Tonic_GraphUnweld(self, _model, nodeId, _out, _cap, count):
        self._record("Tonic_GraphUnweld", (nodeId,))
        _deref(count).value = 2
        return 0

    def Tonic_GraphDeleteNode(self, _model, nodeId):
        self._record("Tonic_GraphDeleteNode", (nodeId,))
        return 0

    def Tonic_GraphDeleteEdge(self, _model, edgeId):
        self._record("Tonic_GraphDeleteEdge", (edgeId,))
        return 0

    def Tonic_GraphLinkRegions(self, _model, r0, r1):
        self._record("Tonic_GraphLinkRegions", (r0, r1))
        return 0

    def Tonic_GraphWeldAll(self, _model, radius, outWelds):
        self._record("Tonic_GraphWeldAll", (radius.value,))
        _deref(outWelds).value = 3
        return 0

    def Tonic_ReadFaceRegionIds(self, _model, out, maxOut, count):
        self._record("Tonic_ReadFaceRegionIds")
        _deref(count).value = len(self.faceRegionIds)
        if out is not None:
            for i in range(min(maxOut, len(self.faceRegionIds))):
                out[i] = self.faceRegionIds[i]
        return 0

    # -- everything else ---------------------------------------------------

    def __getattr__(self, name):
        if name.startswith("_") or not name.startswith("Tonic_"):
            raise AttributeError(name)

        def entry(*args):
            self._record(name, args)
            return 0
        return entry


class FakeSession:
    """The TonicSession surface the loops use, recorded."""

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

    @property
    def gestureActive(self):
        return bool(self.gestureStack)

    # -- publish / commit --------------------------------------------------

    def publish(self, dirtyMask=0):
        self.published.append(int(dirtyMask))
        return 1

    def rasterise(self):
        self.events.append(("rasterise", None))
        return True

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

    def clearSelection(self, kindMask=0):
        self.selection = {}
        return True


# ---------------------------------------------------------------------------
# Cameras with arithmetic anyone can check by hand
# ---------------------------------------------------------------------------

def orthoCamera(tonicCamera, width=400, height=300):
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
    return tonicCamera.TonicCamera(viewProj, width, height)


def perspectiveCamera(tonicCamera, width=400, height=400):
    """90-degree square perspective, near 1, far 101, looking down -Z."""
    near, far = 1.0, 101.0
    f = 1.0
    viewProj = (f, 0.0, 0.0, 0.0,
                0.0, f, 0.0, 0.0,
                0.0, 0.0, (far + near) / (near - far), -1.0,
                0.0, 0.0, 2.0 * far * near / (near - far), 0.0)
    return tonicCamera.TonicCamera(viewProj, width, height)


def topDownCamera(tonicCamera, width=400, height=400):
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
    return tonicCamera.TonicCamera(tonicCamera.matMul(view, proj), width,
                                   height)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def testCamera(tonicCamera):
    print("-- camera ------------------------------------------------")
    cam = orthoCamera(tonicCamera)
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

    persp = perspectiveCamera(tonicCamera)
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
    down = topDownCamera(tonicCamera)
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


def testHotkeys(tonicModes):
    print("-- the plan/18 3.4 key table -----------------------------")
    action = tonicModes.HotkeyAction
    rows = (
        ("1", frozenset(), (tonicModes.ACTION_MODE, "graph")),
        ("4", frozenset(), (tonicModes.ACTION_MODE, "hierarchy")),
        ("6", frozenset(), (tonicModes.ACTION_MODE, "output")),
        ("d", frozenset(), (tonicModes.ACTION_SUBMODE, "D")),
        ("escape", frozenset(), (tonicModes.ACTION_CANCEL, None)),
        ("z", frozenset(["ctrl"]), (tonicModes.ACTION_UNDO, None)),
        ("y", frozenset(["ctrl"]), (tonicModes.ACTION_REDO, None)),
        ("delete", frozenset(), (tonicModes.ACTION_DELETE, None)),
        ("[", frozenset(), (tonicModes.ACTION_RADIUS, -1.0)),
        ("]", frozenset(), (tonicModes.ACTION_RADIUS, 1.0)),
        ("d", frozenset(["shift"]), (tonicModes.ACTION_SUBDIVIDE, None)),
        ("m", frozenset(["shift"]), (tonicModes.ACTION_MERGE, None)),
        ("down", frozenset(["ctrl"]), (tonicModes.ACTION_ENTER_LEVEL, None)),
        ("up", frozenset(["ctrl"]), (tonicModes.ACTION_EXIT_LEVEL, None)),
        ("backspace", frozenset(), (tonicModes.ACTION_EXIT_LEVEL, None)),
        ("w", frozenset(["shift"]), (tonicModes.ACTION_WELD, None)),
        ("u", frozenset(["shift"]), (tonicModes.ACTION_UNWELD, None)),
        ("s", frozenset(["ctrl", "shift"]), (tonicModes.ACTION_SAVE, None)),
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
          (tonicModes.ACTION_MODE, "graph"),
          "but a mode key works wherever the pointer is")
    check(action("d", frozenset(), textFocus=True) is None,
          "typing into a text field is never a sub-mode switch")
    check(action("escape", frozenset(), textFocus=True) ==
          (tonicModes.ACTION_CANCEL, None),
          "Escape still cancels while a field has focus")
    check(action("f", frozenset()) == (tonicModes.ACTION_SUBMODE, "F"),
          "F reaches the controller, which declines it so usdview frames")
    check(action("k", frozenset(["ctrl"])) is None,
          "an unclaimed Ctrl key is left alone")


def testModesShelf(tonicModes, tonicLoops):
    print("-- the shelf ---------------------------------------------")
    check([m.hotkey for m in tonicModes.MODES] ==
          ["1", "2", "3", "4", "5", "6"],
          "the six modes carry the number keys plan/18 3.4 assigns")
    check(sorted(tonicModes.MODE_KEYS) == ["1", "2", "3", "4", "5", "6"],
          "and the lookup table agrees")
    # Every mode either drives the viewport through a loop or is a panel
    # mode -- and never both, so the shelf can never be silently wrong
    # about what the viewport does. Output is the only panel mode: it has
    # no pointer behaviour by design, not because a phase is missing.
    built = set(tonicLoops.LOOPS) | set(tonicLoops._LAZY_LOOPS)
    panels = set(tonicLoops.PANEL_ONLY_MODES)
    check(built | panels == set(m.id for m in tonicModes.MODES),
          "every mode either has a loop or is a panel mode (%r / %r)"
          % (sorted(built), sorted(panels)))
    check(not (built & panels), "and never both (%r)"
          % sorted(built & panels))
    check(panels == {"output"},
          "Output is the only panel mode (%r)" % sorted(panels))
    check({"graph", "tube", "fill", "hierarchy", "sculpt"} <= built,
          "the five gesture modes have loops (%r)" % sorted(built))
    for modeId in sorted(panels):
        check(tonicLoops.makeLoop(modeId, None, None) is None,
              "%s is a panel mode, so it builds no loop" % modeId)
    check(len(tonicLoops.subModesFor("graph")) == 7 and
          tonicLoops.subModesFor("output") == (),
          "sub-mode shelves come from tonicModes, and Output has none")


def testGizmo(tonicCamera, tonicGizmo):
    print("-- gizmo -------------------------------------------------")
    cam = orthoCamera(tonicCamera)
    gizmo = tonicGizmo.GizmoState()
    check(not gizmo.visible, "a fresh gizmo draws nothing")
    gizmo.place((0.0, 0.0, -5.0), 1.0)
    check(gizmo.visible and gizmo.kind == tonicGizmo.GIZMO_TRANSLATE,
          "placing one makes it a translate gizmo")
    # The x axis runs from pixel 200 to pixel 300 (1 world unit = 100 px).
    check(gizmo.handleAt(cam, 260.0, 150.0) == tonicGizmo.HANDLE_U,
          "a pixel along the x axis picks the u handle")
    check(gizmo.handleAt(cam, 200.0, 100.0) == tonicGizmo.HANDLE_V,
          "a pixel along the y axis picks the v handle")
    check(gizmo.handleAt(cam, 380.0, 40.0) == tonicGizmo.HANDLE_NONE,
          "a pixel off every handle picks nothing")
    check(gizmo.handleAt(cam, 200.0, 150.0) == tonicGizmo.HANDLE_PLANE,
          "the centre is the free handle: every axis starts there, so no "
          "axis can claim it (V4)")

    check(gizmo.begin(tonicGizmo.HANDLE_U, cam, 260.0, 150.0),
          "a drag starts on the u handle")
    delta = gizmo.drag(cam, 310.0, 190.0)
    check(near(delta[0], 0.5) and near(delta[1], 0.0) and near(delta[2], 0.0),
          "an axis drag keeps only the along-axis travel (%r)" % (delta,))
    gizmo.end()
    check(gizmo.abiHandle() == -1 and not gizmo.dragging,
          "ending the drag clears the active handle")

    check(gizmo.begin(tonicGizmo.HANDLE_PLANE, cam, 200.0, 150.0),
          "a screen-plane drag starts")
    delta = gizmo.drag(cam, 300.0, 50.0)
    # The frame is 400 x 300 over 4 x 4 world units, so a pixel is wider
    # than it is tall here: 100 px right is 1.0, 100 px up is 4/3.
    check(near(delta[0], 1.0) and near(delta[1], 4.0 / 3.0),
          "a plane drag tracks the cursor in both axes (%r)" % (delta,))
    check(gizmo.abiHandle() == -1,
          "the plane handle is spelled 'none' to the ABI")
    gizmo.end()

    frame = tonicGizmo.screenFrame(cam, (0.0, 0.0, -5.0))
    check(near(frame[0], 1.0) and near(frame[4], 1.0),
          "the screen frame of a camera looking down -Z is the world frame "
          "(%r)" % (frame,))
    check(near(tonicGizmo.pointToSegmentPx(5.0, 4.0, 0.0, 0.0, 10.0, 0.0),
               4.0),
          "point-to-segment distance is the perpendicular where it lands")


def newLoop(tonicLoops, TonicToolState, subMode="draw"):
    dll = FakeDll()
    session = FakeSession(dll)
    state = TonicToolState()
    state.snapRadiusPx = 8.0
    loop = tonicLoops.GraphLoop(session, state)
    loop.setSubMode(subMode)
    return dll, session, state, loop


def testGraphDraw(tonicCamera, tonicLoops, TonicToolState):
    print("-- GraphLoop: draw ---------------------------------------")
    dll, session, state, loop = newLoop(tonicLoops, TonicToolState, "draw")
    cam = topDownCamera(tonicCamera)

    def sample(x, y, mods=frozenset()):
        return tonicLoops.Sample(session, cam, x, y, mods)

    check(loop.press(sample(100.0, 100.0)), "press claims the event")
    check(session.gestureStack == ["Graph draw"],
          "press opens exactly one undo bracket (%r)" % session.gestureStack)
    loop.move(sample(300.0, 100.0))
    loop.move(sample(300.0, 300.0))
    loop.move(sample(100.0, 300.0))
    check(loop.release(sample(100.0, 100.0)), "release claims the event")
    check(not session.gestureStack, "release seals the bracket")
    check(len(session.rays) == 5,
          "one Tonic_Raycast per sample, 5 samples (got %d)"
          % len(session.rays))
    check(session.picks == [],
          "a Draw stroke never picks an item (got %r)" % (session.picks,))
    strokes = dll.argsOf("Tonic_GraphStroke")
    check(len(strokes) == 1, "one Tonic_GraphStroke for the whole drag")
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
    check(order[-4:] == ["rasterise", "ensureRegionTubes", "enqueueCommit",
                         "rebake"],
          "release rasterises, gives a closed region its tube stub (G14), "
          "then enqueues the commit and the bake (%r)" % (order,))
    check(session.events.index(("end", None)) <
          session.events.index(("rasterise", None)),
          "the bracket is sealed BEFORE the stage work is enqueued")

    # Escape mid-stroke.
    dll.reset()
    session.events = []
    check(loop.press(sample(100.0, 100.0)), "a second stroke presses")
    loop.move(sample(200.0, 200.0))
    check(loop.cancel(), "Escape cancels the live stroke")
    check(("cancel", None) in session.events,
          "... through Tonic_CancelGesture (%r)" % (session.events,))
    check(dll.count("Tonic_GraphStroke") == 0,
          "a cancelled stroke never reaches Tonic_GraphStroke")
    check(not loop.release(sample(200.0, 200.0)),
          "the release after a cancel is not ours")


def testGraphPlaceAndClicks(tonicCamera, tonicLoops, TonicToolState):
    print("-- GraphLoop: place, connect, delete, link ---------------")
    import usdGenTonicTools.tonicLib as tonicLib
    dll, session, state, loop = newLoop(tonicLoops, TonicToolState, "place")
    cam = topDownCamera(tonicCamera)

    def sample(x, y, mods=frozenset()):
        return tonicLoops.Sample(session, cam, x, y, mods)

    # Press on empty space adds a node; the drop lands on node 42 and welds.
    session.pickFn = lambda mask, x, y: None
    loop.press(sample(100.0, 100.0))
    check(dll.count("Tonic_GraphAddNode") == 1,
          "Place on empty scalp adds a node")
    loop.move(sample(150.0, 150.0))
    check(dll.count("Tonic_GraphMoveNode") == 1,
          "dragging moves it (one move, one call)")
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_GRAPH_NODE, "id": 42,
                       "subId": -1, "subSubId": -1}
                      if mask == tonicLib.TONIC_PICK_GRAPH_NODE else None)
    loop.release(sample(150.0, 150.0))
    welds = dll.argsOf("Tonic_GraphWeld")
    check(welds and welds[0][0] == 42,
          "dropping on another node welds onto it (%r)" % (welds,))
    check(all(mask == tonicLib.TONIC_PICK_GRAPH_NODE
              for mask, _x, _y, _r in session.picks),
          "every node hit went through K11 with the node kind (%r)"
          % (session.picks,))
    check(all(near(r, 8.0) for _m, _x, _y, r in session.picks),
          "the pick radius is the panel's snap radius in pixels")

    # Connect: two clicks, one edge.
    dll.reset()
    loop.setSubMode("connect")
    ids = iter([11, 12])
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_GRAPH_NODE,
                       "id": next(ids), "subId": -1, "subSubId": -1})
    session.events = []
    loop.press(sample(100.0, 100.0))
    loop.release(sample(100.0, 100.0))
    check(dll.count("Tonic_GraphConnect") == 0,
          "one click on a node only arms the connect")
    check([name for name, _a in session.events] == [],
          "an arming click opens no undo bracket at all (%r)"
          % (session.events,))
    loop.press(sample(200.0, 200.0))
    loop.release(sample(200.0, 200.0))
    check(dll.argsOf("Tonic_GraphConnect") == [(11, 12)],
          "the second click connects the pair (%r)"
          % dll.argsOf("Tonic_GraphConnect"))
    brackets = [name for name, _a in session.events
                if name in ("begin", "end")]
    check(brackets == ["begin", "end"],
          "and the connecting click is exactly one bracket (%r)"
          % (brackets,))
    check(not session.gestureStack, "which is closed by the time it ends")

    # Delete sub-mode: a node first, an edge when there is no node.
    dll.reset()
    loop.setSubMode("delete")
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_GRAPH_NODE, "id": 9,
                       "subId": -1, "subSubId": -1}
                      if mask == tonicLib.TONIC_PICK_GRAPH_NODE else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(dll.argsOf("Tonic_GraphDeleteNode") == [(9,)],
          "Delete removes the node under the cursor")
    dll.reset()
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_GRAPH_EDGE, "id": 3,
                       "subId": -1, "subSubId": -1}
                      if mask == tonicLib.TONIC_PICK_GRAPH_EDGE else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(dll.argsOf("Tonic_GraphDeleteEdge") == [(3,)],
          "and the edge when no node is in range")

    # Link: two regions.
    dll.reset()
    loop.setSubMode("link")
    regions = iter([0, 1])
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_REGION,
                       "id": next(regions), "subId": -1, "subSubId": -1}
                      if mask == tonicLib.TONIC_PICK_REGION else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    loop.press(sample(320.0, 120.0))
    loop.release(sample(320.0, 120.0))
    check(dll.argsOf("Tonic_GraphLinkRegions") == [(0, 1)],
          "Link joins the two clicked regions (%r)"
          % dll.argsOf("Tonic_GraphLinkRegions"))

    # Unweld.
    dll.reset()
    loop.setSubMode("unweld")
    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_GRAPH_NODE, "id": 5,
                       "subId": -1, "subSubId": -1}
                      if mask == tonicLib.TONIC_PICK_GRAPH_NODE else None)
    loop.press(sample(120.0, 120.0))
    loop.release(sample(120.0, 120.0))
    check(dll.argsOf("Tonic_GraphUnweld") == [(5,)],
          "Unweld splits the clicked node")


def testGraphHoverMarqueeKeys(tonicCamera, tonicLoops, TonicToolState):
    print("-- GraphLoop: hover, marquee, keys -----------------------")
    import usdGenTonicTools.tonicLib as tonicLib
    dll, session, state, loop = newLoop(tonicLoops, TonicToolState, "draw")
    cam = topDownCamera(tonicCamera)

    def sample(x, y, mods=frozenset()):
        return tonicLoops.Sample(session, cam, x, y, mods)

    session.pickFn = (lambda mask, x, y:
                      {"kind": tonicLib.TONIC_PICK_GRAPH_NODE, "id": 6,
                       "subId": -1, "subSubId": -1})
    check(not loop.hover(sample(120.0, 120.0)),
          "hover never claims the event")
    check(session.hovers[-1] == (tonicLib.TONIC_PICK_GRAPH_NODE, 6),
          "a hover over a node highlights it (%r)" % (session.hovers,))
    check(session.published[-1] == tonicLib.TONIC_DIRTY_SELECTION,
          "and republishes only the selection locator (%r)"
          % (session.published,))
    session.pickFn = lambda mask, x, y: None
    loop.hover(sample(300.0, 300.0))
    check(session.hovers[-1] == (0, -1),
          "hovering nothing clears the highlight")

    # Marquee: Shift-drag selects nodes through Tonic_SelectRect.
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
        check(kind == tonicLib.TONIC_PICK_GRAPH_NODE and
              mode == tonicLib.TONIC_SELECT_SET and
              near(x0, 100.0) and near(y1, 320.0),
              "the band is the press-to-release rectangle over nodes (%r)"
              % (session.rects[-1],))
    session.rects = []
    loop.press(sample(100.0, 100.0, frozenset(["shift", "ctrl"])))
    loop.release(sample(200.0, 200.0, frozenset(["shift", "ctrl"])))
    check(session.rects and session.rects[-1][5] == tonicLib.TONIC_SELECT_ADD,
          "Ctrl+Shift adds to the selection instead of replacing it")

    # Delete key over a selection.
    dll.reset()
    session.selection = {tonicLib.TONIC_PICK_GRAPH_NODE: [2, 3],
                         tonicLib.TONIC_PICK_GRAPH_EDGE: [7]}
    check(loop.deleteSelection(), "Delete acts on the selection")
    check(dll.count("Tonic_GraphDeleteNode") == 2 and
          dll.count("Tonic_GraphDeleteEdge") == 1,
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
    session.selection = {tonicLib.TONIC_PICK_GRAPH_NODE: [4, 5]}
    check(loop.weldSelected() and dll.argsOf("Tonic_GraphWeld") == [(4, 5)],
          "Shift+W welds exactly two selected nodes")
    session.selection = {tonicLib.TONIC_PICK_GRAPH_NODE: [4, 5, 6]}
    check(not loop.weldSelected(), "three nodes is not a weld")
    session.selection = {tonicLib.TONIC_PICK_GRAPH_NODE: [4]}
    check(loop.unweldSelected(), "Shift+U unwelds the one selected node")


def testSurfaceMiss(tonicCamera, tonicLoops, TonicToolState):
    print("-- GraphLoop: a stroke off the scalp ---------------------")
    dll, session, state, loop = newLoop(tonicLoops, TonicToolState, "draw")
    session.surfaceMisses = True
    cam = topDownCamera(tonicCamera)
    loop.press(tonicLoops.Sample(session, cam, 10.0, 10.0))
    loop.move(tonicLoops.Sample(session, cam, 60.0, 60.0))
    loop.release(tonicLoops.Sample(session, cam, 90.0, 90.0))
    check(dll.count("Tonic_GraphStroke") == 0,
          "a stroke that never hit the scalp authors nothing")
    check(not session.gestureStack,
          "and still closes its bracket (%r)" % session.gestureStack)


def testRingDisplayFollowsMode(tonicViewport, TonicToolState):
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
    # testUsdGenTonicIndex, where it can be read back off the published
    # prims.
    dll = FakeDll()
    session = FakeSession(dll)
    state = TonicToolState()
    controller = tonicViewport.ViewportController(state, session, None)

    def policyArgs():
        return [(a[1].decode("utf-8"), a[2].decode("utf-8"), a[3])
                for a in dll.argsOf("Tonic_SetDisplayPolicy")]

    controller.setMode("graph")
    check(policyArgs()[-1:] == [("graph", "draw", 1)],
          "Graph mode pushes its own id, sub-mode and level (%r)"
          % policyArgs())
    check(not dll.argsOf("Tonic_SetRingDisplay"),
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
          tonicViewport.tonicLib.TONIC_DIRTY_DISPLAY,
          "each change publishes the display dirty and nothing heavier (%r)"
          % session.published[-3:])
    dll.reset()
    state.activeLevel = 2
    controller._pushFocusLevel()
    check(policyArgs()[-1:] == [("sculpt", "grab", 2)],
          "entering a level re-resolves the table at the new focus (%r)"
          % policyArgs())


def testDisplayScaleHook(tonicCamera, tonicViewport, TonicToolState):
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
    state = TonicToolState()
    controller = tonicViewport.ViewportController(state, session, None)
    cam = topDownCamera(tonicCamera)

    check(controller.syncDisplayScale(cam) is True,
          "syncDisplayScale takes a camera and answers True")
    expected = cam.worldPerPixel(session.scalpCenter)
    check(expected > 0.0, "the fixture camera has a real pixel size (%r)"
          % expected)
    check(session.displayScales[-1:] == [expected],
          "and pushes exactly that into the model (%r)"
          % session.displayScales[-1:])

    # Half the pixels across the same world: twice the world per pixel.
    zoomed = topDownCamera(tonicCamera, width=200, height=200)
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


def testOutputModeSwitches(tonicLoops, tonicViewport, TonicToolState):
    """V7: Output is a panel mode, and switching to it is not a refusal.

    It carried a "not built yet: V3" status from the phase when the mode
    shelf existed and the dock did not. The dock is built, so the status
    is the mode's own line and the state records the mode like any
    other."""
    print("-- Output mode -------------------------------------------")
    dll = FakeDll()
    session = FakeSession(dll)
    state = TonicToolState()
    controller = tonicViewport.ViewportController(state, session, None)
    controller.setMode("tube")
    status = controller.setMode("output")
    check(state.activeMode == "output",
          "the shelf records Output as active (%r)" % state.activeMode)
    check(status == "Tonic: Output panel: maps, bake, save groom.",
          "and reports the mode's own status line (%r)" % status)
    check("not built" not in status,
          "with no stale refusal in it (%r)" % status)
    check(controller.loop is None,
          "Output runs no loop (%r)" % (controller.loop,))
    # Output must not acquire a sub-mode: it has no shelf to put one on.
    check(tonicLoops.subModesFor("output") == () and
          getattr(state, "outputSubMode", "") == "",
          "Output takes no sub-mode (%r)"
          % (getattr(state, "outputSubMode", ""),))


def main():
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import tonicTestPackage
    tonicTestPackage.install()
    from usdGenTonicTools import (tonicCamera, tonicGizmo, tonicLoops,
                                  tonicModes, tonicViewport)
    from usdGenTonicTools.tonicToolState import TonicToolState
    testCamera(tonicCamera)
    testHotkeys(tonicModes)
    testModesShelf(tonicModes, tonicLoops)
    testGizmo(tonicCamera, tonicGizmo)
    testGraphDraw(tonicCamera, tonicLoops, TonicToolState)
    testGraphPlaceAndClicks(tonicCamera, tonicLoops, TonicToolState)
    testGraphHoverMarqueeKeys(tonicCamera, tonicLoops, TonicToolState)
    testSurfaceMiss(tonicCamera, tonicLoops, TonicToolState)
    testRingDisplayFollowsMode(tonicViewport, TonicToolState)
    testDisplayScaleHook(tonicCamera, tonicViewport, TonicToolState)
    testOutputModeSwitches(tonicLoops, tonicViewport, TonicToolState)
    print("testUsdGenTonicToolsLoops: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
