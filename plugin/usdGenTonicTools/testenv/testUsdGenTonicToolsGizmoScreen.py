#!/usr/bin/env python
# testUsdGenTonicToolsGizmoScreen -- T0 for the vendored RigExec gizmo maths
# (tonic_gizmo_parity section 4 and 6: G08, G09, G11, G16, G20, G23, G24).
#
#   python plugin/usdGenTonicTools/testenv/testUsdGenTonicToolsGizmoScreen.py
#
# No usdview, no Qt, no pxr, no DLL. Ports of usdRig's
# tests/python/test_gizmo_screen.py (TestDragMath, TestMayaScaleHandles,
# TestMayaDragMath), test_gizmo_snap.py (TestConstrain, TestGrid) and
# test_gizmo_settings.py (defaults, per-tool independence, size clamp,
# Reset, listeners, the Global/Local toggle) onto Tonic's tuple-maths
# vendored modules, plus the Tonic adaptations: the ratio-scaled
# AxisDragParameter floor, the World/Screen/Tube orientation frames and a
# HiDPI pick tolerance.
#
# The synthetic camera is an orthographic 400 x 400 view over x, y in
# [-2, 2] looking down -Z, so the gizmo origin (0, 0, -5) projects to
# (200, 200), +X is screen-right, +Y is screen-up and one world unit is
# 100 px both ways.
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


def near(a, b, eps=1e-6):
    return abs(float(a) - float(b)) <= eps


def nearVec(a, b, eps=1e-6):
    return all(near(a[i], b[i], eps) for i in range(3))


ORIGIN = (0.0, 0.0, -5.0)


def squareCamera(tonicCamera, pixelRatio=1.0):
    a = -2.0 / 9.0
    b = -11.0 / 9.0
    viewProj = (0.5, 0.0, 0.0, 0.0,
                0.0, 0.5, 0.0, 0.0,
                0.0, 0.0, a, 0.0,
                0.0, 0.0, b, 1.0)
    return tonicCamera.TonicCamera(viewProj, 400, 400, pixelRatio)


def handlesByName(gs, camera, tool, frame=None, worldLength=1.0,
                  origin=ORIGIN):
    frame = frame or (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    return {h.name: h for h in gs.BuildHandles(
        tool, origin, frame, camera, 1.0, worldLength=worldLength)}


class _Fake(object):
    def __init__(self, **values):
        self.__dict__.update(values)


def testDragMath(gs, tonicCamera):
    print("-- drag maths (usdRig TestDragMath / TestMayaDragMath) -----")
    camera = squareCamera(tonicCamera)
    handles = handlesByName(gs, camera, gs.TOOL_TRANSLATE)
    x = handles["x"]
    check(near(x.points[0][0], 200.0, 1e-3) and
          near(x.points[-1][0], 300.0, 1e-3),
          "the x axis runs 200 -> 300 px (%r)" % (x.points,))
    t = gs.AxisDragParameter(x, (250, 200), (300, 210))
    check(near(t, 0.5, 1e-9), "half the handle length: %s" % t)
    delta = tuple(x.worldAxis[i] * t * x.worldLength for i in range(3))
    moved = camera.worldToPixels(tuple(ORIGIN[i] + delta[i]
                                       for i in range(3)))
    check(near(moved[0], 250.0, 1e-3),
          "the world delta projects back to the mouse travel (%r)"
          % (moved,))
    # G09: a foreshortened axis is floored at MIN_AXIS_PIXELS LOGICAL
    # pixels, and the floor scales with the device pixel ratio.
    short = _Fake(points=[(0.0, 0.0), (6.0, 0.0)])
    check(near(gs.AxisDragParameter(short, (0, 0), (100, 0)),
               100.0 / gs.MIN_AXIS_PIXELS, 1e-9),
          "a 6 px axis reads travel against the 12 px floor")
    check(near(gs.AxisDragParameter(short, (0, 0), (100, 0), 2.0),
               100.0 / (2.0 * gs.MIN_AXIS_PIXELS), 1e-9),
          "on a 2x display the floor is 24 physical px")
    z = handles["z"]
    check(not z.grabbable and
          gs.AxisDragParameter(z, (200, 200), (300, 200)) == 0.0,
          "an axis pointing at the camera is ungrabbable and inert")
    check(gs.AxisDragParameter(_Fake(points=[(0.0, 0.0), (1.0, 0.0)]),
                               (0, 0), (0, 0), 1.0) == 0.0,
          "no travel is no drag")
    delta = gs.RayPlaneDragDelta(camera, ORIGIN, (0.0, 0.0, 1.0),
                                 (200, 200), (210, 190))
    check(near(delta[0], 0.1, 1e-6) and near(delta[1], 0.1, 1e-6) and
          near(delta[2], 0.0, 1e-9),
          "ray/plane on the z plane matches the camera-plane delta (%r)"
          % (delta,))
    # G07: the plane PERPENDICULAR to x -- no x component whatever the
    # cursor does.  Edge-on here (the camera looks along it), so the
    # vendored fallback is the camera-plane delta; either way x is kept.
    delta = gs.RayPlaneDragDelta(camera, ORIGIN, (1.0, 0.0, 0.0),
                                 (200, 200), (200, 190))
    check(delta[1] > 0.0, "the yz plane moves up with the cursor (%r)"
          % (delta,))
    check(near(gs.AccumulateAngle(170.0, 170.0, -175.0), 185.0, 1e-9),
          "a rotate sweep accumulates through the wrap")


def testMayaScale(gs, tonicCamera):
    print("-- MayaScaleFactor (usdRig TestMayaScaleHandles, G08) -------")
    camera = squareCamera(tonicCamera)
    byName = handlesByName(gs, camera, gs.TOOL_SCALE)
    check(set(byName) == {"x", "y", "z", "xy", "yz", "xz", "center"},
          "Maya scale handles: %s" % sorted(byName))
    origin = (200.0, 200.0)
    x = byName["x"]
    check(near(gs.MayaScaleFactor(x, origin, (250, 200), (300, 200), True),
               2.0, 1e-9), "press at half length, drag to the tip: 2x")
    check(near(gs.MayaScaleFactor(x, origin, (250, 200), (150, 200), True),
               -1.0, 1e-9), "through the origin flips the sign")
    check(near(gs.MayaScaleFactor(x, origin, (250, 200), (200, 200), True),
               0.0, 1e-9), "onto the origin is zero")
    check(near(gs.MayaScaleFactor(x, origin, (250, 200), (150, 200), False),
               gs.MIN_SCALE_FACTOR, 1e-12),
          "Prevent Negative Scale clamps at MIN_SCALE_FACTOR")
    xy = byName["xy"]
    d = (xy.worldCenterScreen[0] - 200.0, xy.worldCenterScreen[1] - 200.0)
    f = gs.MayaScaleFactor(xy, origin, (200 + d[0], 200 + d[1]),
                           (200 + 2 * d[0], 200 + 2 * d[1]), True)
    check(near(f, 2.0, 1e-6), "plane handle: ratio along the diagonal")


def testSnapMaths(gs, snap):
    print("-- snapping (usdRig TestMayaDragMath, TestConstrain, TestGrid)")
    check(near(gs.SnapRelative(2.4, 1.0), 2.0) and
          near(gs.SnapRelative(-2.6, 1.0), -3.0) and
          near(gs.SnapRelative(37.0, 15.0), 30.0) and
          near(gs.SnapRelative(0.5, 1.0), 1.0) and
          near(gs.SnapRelative(-0.5, 1.0), -1.0),
          "relative snap rounds halves away from zero")
    v = gs.SnapAbsolute((0.4, 1.6, -0.5), 1.0)
    check(nearVec(v, (0.0, 2.0, -1.0)), "vector snap: %r" % (v,))

    x = _Fake(kind="axis", worldAxis=(1.0, 0.0, 0.0), worldNormal=None)
    xy = _Fake(kind="plane", worldAxis=None, worldNormal=(0.0, 0.0, 1.0))
    centre = _Fake(kind="center", worldAxis=None, worldNormal=None)
    pivot = (1.0, 2.0, 3.0)
    world = (4.0, 5.0, 6.0)
    check(nearVec(snap.ConstrainToHandle(centre, pivot, world), world),
          "centre lands on the candidate")
    check(nearVec(snap.ConstrainToHandle(x, pivot, world), (4.0, 2.0, 3.0)),
          "axis projects onto its line")
    check(nearVec(snap.ConstrainToHandle(xy, pivot, world),
                  (4.0, 5.0, 3.0)), "plane drops the normal component")
    check(nearVec(snap.ConstrainToHandle(x, pivot, world, ctrl=True),
                  (1.0, 5.0, 6.0)), "Ctrl+axis is a plane, not a line")

    offGrid = (0.4, -0.3, 0.25)
    for p0 in ((0.0, 0.0, 0.0), offGrid):
        check(nearVec(snap.GridPoint(centre, p0, (1.4, -2.6, 0.2), 1.0),
                      (1.0, -3.0, 0.0)),
              "centre lands on the world grid from %r" % (p0,))
        check(nearVec(snap.GridPoint(centre, p0, (0.5, -0.5, 1.5), 1.0),
                      (1.0, -1.0, 2.0)),
              "halves away from zero from %r" % (p0,))
        check(nearVec(snap.GridPoint(x, p0, (1.4, 2.6, 0.2), 1.0),
                      (1.0, 2.6, 0.2)),
              "axis rounds the world X, not the travel, from %r" % (p0,))
        check(nearVec(snap.GridPoint(xy, p0, (1.4, -2.6, 5.0), 1.0),
                      (1.0, -3.0, 5.0)),
              "plane rounds the world pair from %r" % (p0,))
        check(nearVec(snap.GridPoint(x, p0, (1.4, 2.6, 3.2), 1.0,
                                     ctrl=True), (1.4, 3.0, 3.0)),
              "Ctrl+axis rounds the in-plane pair from %r" % (p0,))
    check(snap.DirectionIsWorldAligned((1.0, 0.0, 0.0)) == 0 and
          snap.DirectionIsWorldAligned((-1.0, 0.0, 0.0)) == 0 and
          snap.DirectionIsWorldAligned((0.0, 0.0, -1.0)) == 2 and
          snap.DirectionIsWorldAligned((1.0, 1.0, 0.0)) is None,
          "world alignment ignores the sign and rejects diagonals")
    # A tilted axis has no world grid coordinate: the travel quantises.
    s = math.sqrt(0.5)
    tilted = _Fake(kind="axis", worldAxis=(s, 0.0, -s), worldNormal=None)
    unsnapped = tuple(offGrid[i] + (s, 0.0, -s)[i] * 1.4 for i in range(3))
    got = snap.GridPoint(tilted, offGrid, unsnapped, 1.0)
    want = tuple(offGrid[i] + (s, 0.0, -s)[i] * 1.0 for i in range(3))
    check(nearVec(got, want, 1e-9),
          "a tilted axis rounds the travel 1.4 -> 1 (%r)" % (got,))


def testSettings(gset, TonicToolState):
    print("-- tonicGizmoSettings (usdRig test_gizmo_settings, G20) -----")
    settings = gset.GizmoSettings()
    move = settings.For(gset.TOOL_TRANSLATE)
    rotate = settings.For(gset.TOOL_ROTATE)
    scale = settings.For(gset.TOOL_SCALE)
    check(move.stepSnap is False and near(move.stepSize, 1.0) and
          near(rotate.stepSize, 15.0) and near(scale.stepSize, 0.1),
          "step snap starts off; steps 1.0 / 15 deg / 0.1x")
    check(rotate.freeRotate is True and scale.preventNegativeScale is False,
          "Free Rotate on, Prevent Negative Scale off by default")
    check(settings.For("move") is move and settings.For("rotate") is rotate,
          "the Tube tool names map onto the settings tokens")
    move.stepSize = 4.0
    check(near(settings.For(gset.TOOL_TRANSLATE).stepSize, 4.0) and
          near(rotate.stepSize, 15.0), "each tool keeps its own settings")
    check(near(settings.manipulatorSize, 90.0),
          "the manipulator starts at 90 logical pixels")
    settings.manipulatorSize = 0.0
    check(settings.manipulatorSize == gset.MANIPULATOR_SIZE_MIN,
          "the size clamps up to %g" % gset.MANIPULATOR_SIZE_MIN)
    settings.manipulatorSize = 1e6
    check(settings.manipulatorSize == gset.MANIPULATOR_SIZE_MAX,
          "the size clamps down to %g" % gset.MANIPULATOR_SIZE_MAX)
    settings.manipulatorSize = 100.0
    check(near(settings.ScaleManipulator(1.1), 110.0),
          "'+' grows the manipulator by 10%")
    rotate.stepSnap = True
    rotate.freeRotate = False
    move.stepSnap = True
    settings.Reset(gset.TOOL_ROTATE)
    check(settings.For(gset.TOOL_ROTATE) is rotate and
          rotate.stepSnap is False and rotate.freeRotate is True and
          move.stepSnap is True,
          "Reset(rotate) restores rotate in place and leaves Move alone")

    listened = gset.GizmoSettings()
    seen = []

    def listener():
        seen.append(1)

    listened.AddListener(listener)
    listened.For("move").stepSize = 2.0
    listened.manipulatorSize = 100.0
    listened.Reset("move")
    check(len(seen) == 3, "field, size and Reset changes notify (%d)"
          % len(seen))
    listened.manipulatorSize = 100.0
    listened.For("move").stepSize = 1.0
    check(len(seen) == 3, "an unchanged write is silent (%d)" % len(seen))
    listened.RemoveListener(listener)
    listened.gridSize = 0.5
    check(len(seen) == 3 and near(listened.gridSize, 0.5),
          "a removed listener hears nothing more")

    check(gset.NextToggleOrientation(gset.ORIENT_WORLD) == gset.ORIENT_TUBE
          and gset.NextToggleOrientation(gset.ORIENT_TUBE) ==
          gset.ORIENT_WORLD and
          gset.NextToggleOrientation(gset.ORIENT_SCREEN) ==
          gset.ORIENT_WORLD,
          "L flips World <-> Tube, and Screen goes to World")
    check(gset.ToggleLabel(gset.ORIENT_WORLD) == "Global" and
          gset.ToggleLabel(gset.ORIENT_TUBE) == "Local",
          "the toggle speaks Global/Local")
    check(gset.NormalizeOrientation("SCREEN") == "screen" and
          gset.NormalizeOrientation("parent") == "world",
          "unknown orientations normalise to World")
    state = TonicToolState()
    check(state.transformOrientation == "world" and
          state.gizmoSettings is None,
          "a fresh TonicToolState is World with no settings yet")
    first = gset.settingsFor(state)
    check(first is gset.settingsFor(state) and state.gizmoSettings is first,
          "settingsFor creates the settings once and keeps them")


def testOrientationFrames(gs, tonicCamera, tonicGizmo):
    print("-- orientation frames (GZ-05, G16) --------------------------")
    camera = squareCamera(tonicCamera)
    check(tonicGizmo.orientationFrame("world", camera, ORIGIN) ==
          tonicGizmo.IDENTITY_FRAME, "World is the identity frame")
    check(tonicGizmo.orientationFrame("tube", camera, ORIGIN, None) ==
          tonicGizmo.IDENTITY_FRAME,
          "Tube with no root normal falls back to World")
    for normal in ((0.0, 1.0, 0.0), (0.6, 0.8, 0.0), (0.0, 0.0, -1.0)):
        frame = tonicGizmo.orientationFrame("tube", camera, ORIGIN, normal)
        u, v, w = frame[0:3], frame[3:6], frame[6:9]
        dot = lambda a, b: sum(a[i] * b[i] for i in range(3))
        cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                 u[0] * v[1] - u[1] * v[0])
        check(nearVec(w, normal) and near(dot(u, w), 0.0) and
              near(dot(v, w), 0.0) and near(dot(u, u), 1.0) and
              nearVec(cross, w),
              "Tube frame for %r: w is the root normal, right-handed"
              % (normal,))
    again = tonicGizmo.orientationFrame("tube", None, ORIGIN, (0.0, 1.0, 0.0))
    check(again == tonicGizmo.orientationFrame("tube", camera, ORIGIN,
                                               (0.0, 1.0, 0.0)),
          "the Tube frame does not depend on the camera")
    screen = tonicGizmo.orientationFrame("screen", camera, ORIGIN)
    check(near(screen[0], 1.0) and near(screen[4], 1.0),
          "Screen looking down -Z is the camera plane (%r)" % (screen,))
    # G16: the World handles point along world X whatever the ring frame.
    world = handlesByName(gs, camera, gs.TOOL_TRANSLATE,
                          frame=tonicGizmo.IDENTITY_FRAME)
    check(nearVec(world["x"].worldAxis, (1.0, 0.0, 0.0)),
          "World handles: x.worldAxis == (1, 0, 0)")


def testHiDpi(tonicCamera, tonicGizmo):
    print("-- HiDPI: logical pixel constants (G24, GZ-05) --------------")
    gizmo = tonicGizmo.GizmoState()
    gizmo.place(ORIGIN, 1.0)
    single = squareCamera(tonicCamera, 1.0)
    double = squareCamera(tonicCamera, 2.0)
    check(gizmo.handleAt(single, 260.0, 212.0) == tonicGizmo.HANDLE_NONE,
          "12 px off the x axis misses at 1x (8 px tolerance)")
    check(gizmo.handleAt(double, 260.0, 212.0) == tonicGizmo.HANDLE_U,
          "and hits at 2x (16 physical px)")
    check(gizmo.handleAt(single, 260.0, 212.0, pixelRatio=2) ==
          tonicGizmo.HANDLE_U, "an explicit ratio=2 doubles it too")


def testClickSlopAndCentreScale(tonicCamera, tonicGizmo, gs):
    print("-- click slop (G24) and the Scale centre rule (G08) -------")
    single = squareCamera(tonicCamera, 1.0)
    double = squareCamera(tonicCamera, 2.0)
    check(near(tonicGizmo.clickSlopPixels(single), tonicGizmo.CLICK_SLOP_PX)
          and near(tonicGizmo.clickSlopPixels(double),
                   2.0 * tonicGizmo.CLICK_SLOP_PX),
          "the click-versus-drag slop is logical px x the display ratio")
    for camera, label in ((single, "1x"), (double, "2x")):
        gizmo = tonicGizmo.GizmoState()
        gizmo.place(ORIGIN, 1.0, tonicGizmo.GIZMO_SCALE)
        check(gizmo.begin(tonicGizmo.HANDLE_CENTER, camera, 200.0, 200.0),
              "%s: a Scale centre drag starts" % label)
        centre = [h for h in gizmo.handles(camera)
                  if h.handleId == tonicGizmo.HANDLE_CENTER][0]
        for x, y, allow in ((260.0, 200.0, True), (260.0, 120.0, True),
                            (80.0, 200.0, True), (80.0, 200.0, False)):
            want = gs.MayaScaleFactor(centre, (200.0, 200.0), (200.0, 200.0),
                                      (x, y), allow)
            got = gizmo.scaleFactor(camera, x, y, allowNegative=allow)
            check(near(got, want),
                  "%s: centre to (%g, %g) allowNegative=%s is RigExec's "
                  "%.4f (%.4f)" % (label, x, y, allow, want, got))
        gizmo.end()


# Screen-space gizmo modules. Their headers are project MIT; they do not
# cite a private repository path or commit.
_MIT_MODULES = ("tonicGizmoScreen.py", "tonicGizmoSettings.py",
                "tonicGizmoSnap.py", "tonicGizmoIcons.py")


def _topLevelNames(source):
    import ast
    names = set()
    for node in ast.parse(source).body:
        if isinstance(node, (ast.FunctionDef, ast.ClassDef)):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    names.add(target.id)
    return names


def testModuleHeaders(packageDir):
    print("-- gizmo modules carry the project MIT header --------------")
    for name in _MIT_MODULES:
        text = open(os.path.join(packageDir, name), encoding="utf-8").read()
        head = "\n".join(text.splitlines()[:8])
        check("SPDX-License-Identifier: MIT" in head,
              "%s names the MIT license" % name)
        check("Copyright (c) 2026 Nick Burkard" in head,
              "%s names the project copyright" % name)
        check("D:/" not in head and "/home/" not in head,
              "%s header has no machine path" % name)


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
    from usdGenTonicTools import (tonicCamera, tonicGizmo, tonicGizmoScreen,
                                  tonicGizmoSettings, tonicGizmoSnap)
    from usdGenTonicTools.tonicToolState import TonicToolState
    testDragMath(tonicGizmoScreen, tonicCamera)
    testMayaScale(tonicGizmoScreen, tonicCamera)
    testSnapMaths(tonicGizmoScreen, tonicGizmoSnap)
    testSettings(tonicGizmoSettings, TonicToolState)
    testOrientationFrames(tonicGizmoScreen, tonicCamera, tonicGizmo)
    testHiDpi(tonicCamera, tonicGizmo)
    testClickSlopAndCentreScale(tonicCamera, tonicGizmo, tonicGizmoScreen)
    testModuleHeaders(os.path.dirname(os.path.abspath(
        tonicGizmoScreen.__file__)))
    print("testUsdGenTonicToolsGizmoScreen: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
