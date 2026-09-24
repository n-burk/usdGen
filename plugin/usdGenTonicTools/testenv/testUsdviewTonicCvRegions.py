# testUsdviewTonicCvRegions -- T3 acceptance of the click-by-click Graph
# region workflow on a single scalp quad.
#
#   testusdview --testScript \
#       plugin/usdGenTonicTools/testenv/testUsdviewTonicCvRegions.py \
#       examples/tonic-single-quad.usda
#
# This is intentionally a real usdview/Qt test.  It opens the Tonic dock,
# binds its mesh through the dock picker, changes Output's Ptex density in
# its actual widget, and feeds mouse/key events through the installed
# viewport filter.  The two closed triangles are both inside face 0: graph
# regions are sub-face polygons, not one region per mesh face.
import ctypes
import math
import os
import sys

failures = 0

# Disjoint, counter-clockwise triangles inside the only quad, face 0.
LEFT = ((-0.82, -0.56), (-0.22, -0.56), (-0.52, 0.34))
# The second triangle begins on LEFT[1], so it must reuse that existing graph
# node and add only its other two CVs.  EDGE_NEIGHBOUR is below LEFT's base
# and is reserved for the adjacent shared-edge check once that path lands.
SHARED_VERTEX = (LEFT[1], (0.42, -0.54), (0.20, 0.32))
EDGE_NEIGHBOUR = (LEFT[0], LEFT[1], (-0.52, -0.88))
# A second shared-edge triangle which exercises the same artist operation
# through a deliberately tilted, close camera.  Its first two points are
# committed CVs of SHARED_VERTEX; only this third point is new.
TILTED_EDGE_NEIGHBOUR = (0.10, -0.90)
# The capture's final beauty shot keeps its wider, disconnected pair.
RIGHT = ((0.22, -0.56), (0.82, -0.56), (0.52, 0.34))


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def info(text):
    print("info: %s" % text)


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for i, arg in enumerate(list(sys.argv)):
        if arg == "--testScript" and i + 1 < len(sys.argv):
            return os.path.dirname(os.path.abspath(sys.argv[i + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return ""


def _qtTest():
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    return importlib.import_module("%s.QtTest" % PySideModule)


def wait(ms=30):
    _qtTest().QTest.qWait(int(ms))


def typeKey(view, name, modifiers=()):
    from pxr.Usdviewq.qt import QtCore
    keys = {
        "r": QtCore.Qt.Key.Key_R,
        "2": QtCore.Qt.Key.Key_2,
        "return": QtCore.Qt.Key.Key_Return,
        "backspace": QtCore.Qt.Key.Key_Backspace,
        "escape": QtCore.Qt.Key.Key_Escape,
        "z": QtCore.Qt.Key.Key_Z,
        "y": QtCore.Qt.Key.Key_Y,
    }
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    for modifier in modifiers:
        mods |= {"ctrl": QtCore.Qt.KeyboardModifier.ControlModifier,
                 "shift": QtCore.Qt.KeyboardModifier.ShiftModifier}[modifier]
    _qtTest().QTest.keyClick(view, keys[name], mods)


def frameScalp(stage, view):
    """Put the unit quad under a stable top camera for event coordinates."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicCvRegionCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(0.0, 7.0, 0.0)
    zAxis = Gf.Vec3d(0.0, 1.0, 0.0)
    xAxis = Gf.Vec3d(1.0, 0.0, 0.0)
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = None
    for candidate in xf.GetOrderedXformOps():
        if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform:
            op = candidate
            break
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicCvRegionCamera")
    return view.getActiveSceneCamera() is not None


def frameTiltedZoomScalp(stage, view):
    """Aim closely at the shared edge so display lift has real parallax."""
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicCvRegionTiltCamera"))
    cam.CreateFocalLengthAttr(90.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(0.0, 4.5, 3.0)
    target = Gf.Vec3d(0.0, 0.0, -0.60)
    zAxis = (eye - target).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0.0, 1.0, 0.0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = next((candidate for candidate in xf.GetOrderedXformOps()
               if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform),
              None)
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicCvRegionTiltCamera")
    return view.getActiveSceneCamera() is not None


def graphNode(session, nodeId):
    """Canonical authored graph data for an existing stable node id."""
    face = ctypes.c_int(-1)
    uv = (ctypes.c_float * 2)()
    point = (ctypes.c_float * 3)()
    if session.dll.Tonic_GraphGetNode(session.model, int(nodeId),
                                      ctypes.byref(face), uv, point) != 0:
        return None
    return (int(face.value), (float(uv[0]), float(uv[1])),
            (float(point[0]), float(point[1]), float(point[2])))


def graphHover(session):
    """The Graph CV affordance published by a real pointer move."""
    kind = ctypes.c_uint(0)
    ident = ctypes.c_int(-1)
    sub = ctypes.c_int(-1)
    subsub = ctypes.c_int(-1)
    if session.dll.Tonic_GetHover(session.model, ctypes.byref(kind),
                                  ctypes.byref(ident), ctypes.byref(sub),
                                  ctypes.byref(subsub)) != 0:
        return None
    return (int(kind.value), int(ident.value), int(sub.value),
            int(subsub.value))


def guideCount(session):
    """The live guide count (Tonic_GetGuideCounts), -1 on an error."""
    guides = ctypes.c_int(0)
    if session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                        None) != 0:
        return -1
    return int(guides.value)


def l1TubeIds(session):
    got = ctypes.c_int(0)
    session.dll.Tonic_ReadL1TubeIds(session.model, None, 0,
                                    ctypes.byref(got))
    out = (ctypes.c_int * max(1, int(got.value)))()
    rc = session.dll.Tonic_ReadL1TubeIds(session.model, out, len(out),
                                         ctypes.byref(got))
    return ([int(out[i]) for i in range(int(got.value))]
            if rc == 0 else [])


def stagedTubePoints(session):
    """Published tube mesh positions, in ring-major order (public ABI)."""
    count = int(session.dll.Tonic_GetVertexCount(session.model))
    if count < 0:
        return None
    entry = session.dll.Tonic_ReadTubePoints
    entry.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_float),
                      ctypes.c_int]
    entry.restype = ctypes.c_int
    values = (ctypes.c_float * max(3, count * 3))()
    if entry(session.model, values, len(values)) != 0:
        return None
    return [tuple(float(values[index * 3 + axis]) for axis in range(3))
            for index in range(count)]


def samePointSet(actual, expected, tolerance=3e-4):
    """One-to-one unordered point comparison, preserving duplicate checks."""
    if len(actual) != len(expected):
        return False
    used = [False] * len(actual)
    for target in expected:
        match = next((index for index, point in enumerate(actual)
                      if not used[index] and all(
                          abs(point[axis] - target[axis]) <= tolerance
                          for axis in range(3))), None)
        if match is None:
            return False
        used[match] = True
    return True


def regionLoops(session):
    """Closed regions as their stable graph-node id loops."""
    regionCount = ctypes.c_int(0)
    indexCount = ctypes.c_int(0)
    session.dll.Tonic_ReadRegionLoops(
        session.model, None, 0, None, 0, ctypes.byref(regionCount),
        ctypes.byref(indexCount))
    counts = (ctypes.c_int * max(int(regionCount.value), 1))()
    indices = (ctypes.c_int * max(int(indexCount.value), 1))()
    rc = session.dll.Tonic_ReadRegionLoops(
        session.model, counts, len(counts), indices, len(indices),
        ctypes.byref(regionCount), ctypes.byref(indexCount))
    if rc != 0:
        return []
    offset = 0
    loops = []
    for region in range(int(regionCount.value)):
        size = int(counts[region])
        loops.append(tuple(int(indices[offset + i]) for i in range(size)))
        offset += size
    return loops


def _widgetFor(workspace, predicate):
    """The generated form's live widget matching a descriptor predicate."""
    for descriptor, widget in workspace._paramWidgets:
        if predicate(descriptor):
            return widget
    return None


def _pickMalformedFromDock(workspace, badPath):
    """With two Meshes the Bind button opens the picker.

    The picker lists every Mesh by type (the malformed one included) and
    only the accepted choice is run through the full topology check, which
    must refuse it with a message box instead of replacing the groom.
    """
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    QtTest = _qtTest()
    result = {"listed": [], "label": "", "error": ""}

    def dismissError(attempt=0):
        box = QtWidgets.QApplication.activeModalWidget()
        if isinstance(box, QtWidgets.QMessageBox):
            result["error"] = box.text()
            box.accept()
        elif attempt < 100:
            QtCore.QTimer.singleShot(
                20, lambda: dismissError(attempt + 1))

    def chooseBad():
        dialog = QtWidgets.QApplication.activeModalWidget()
        if not isinstance(dialog, QtWidgets.QDialog):
            return
        labels = [w.text() for w in dialog.findChildren(QtWidgets.QLabel)]
        result["label"] = " ".join(labels)
        combo = dialog.findChild(QtWidgets.QComboBox, "tonicGeometryChoices")
        box = dialog.findChild(QtWidgets.QDialogButtonBox,
                               "tonicGeometryPickerButtons")
        ok = box.button(QtWidgets.QDialogButtonBox.Ok) if box else None
        if combo is None or ok is None:
            dialog.reject()
            return
        result["listed"] = [combo.itemText(i) for i in range(combo.count())]
        index = combo.findText(badPath, QtCore.Qt.MatchExactly)
        if index < 0:
            dialog.reject()
            return
        combo.setCurrentIndex(index)
        QtCore.QTimer.singleShot(20, dismissError)
        QtTest.QTest.mouseClick(ok, QtCore.Qt.MouseButton.LeftButton)

    QtCore.QTimer.singleShot(0, chooseBad)
    QtTest.QTest.mouseClick(workspace.button("file", "bind"),
                            QtCore.Qt.MouseButton.LeftButton)
    wait(30)
    check(sorted(result["listed"]) == sorted(["/Scalp", badPath]),
          "the scalp picker lists every Mesh by type (%r)"
          % (result["listed"],))
    check("Choose the scalp Mesh:" in result["label"],
          "the picker asks for the scalp Mesh (%r)" % result["label"])
    check("not a valid scalp mesh" in result["error"],
          "accepting a malformed Mesh reports it is not a valid scalp "
          "(%r)" % result["error"])


def _checkUnboundDock(workspace, container, view):
    """DK-01: before a scalp is bound the dock offers only Bind.

    Every tool control is greyed out, the first-run hint says what to do,
    and the mode hotkeys refuse (with a status line) instead of silently
    entering a tool that has no model to edit.
    """
    state = container.tonicState
    workspace.refresh()
    check(not workspace.button("mode", "tube").isEnabled() and
          not workspace._paramsStack.isEnabled() and
          not workspace._actionsStack.isEnabled(),
          "unbound, the mode shelf, parameters and actions are disabled")
    check(workspace.button("file", "bind").isEnabled() and
          workspace.button("file", "bind").text() == "Bind scalp mesh...",
          "unbound, Bind scalp mesh stays live (%r)"
          % workspace.button("file", "bind").text())
    hint = workspace._firstRunHint
    check(hint.isVisible() and hint.text().startswith("Step 1"),
          "unbound, the first-run hint is visible (%r)" % hint.text())
    check(workspace._geometryPathLabel.text() == "No scalp bound",
          "unbound, the dock says No scalp bound (%r)"
          % workspace._geometryPathLabel.text())
    api = getattr(container.viewport, "_api", None)
    printStatus = getattr(api, "PrintStatus", None)
    messages = []

    def recordStatus(text, *args, **kwargs):
        messages.append(str(text))
        return printStatus(text, *args, **kwargs)

    instrumented = False
    if callable(printStatus):
        try:
            api.PrintStatus = recordStatus
            instrumented = True
        except (AttributeError, TypeError):
            pass
    try:
        view.setFocus()
        wait(10)
        typeKey(view, "2")
        wait(10)
    finally:
        if instrumented:
            api.PrintStatus = printStatus
    check(state.activeMode == "graph",
          "unbound, pressing 2 leaves the mode on Graph (%r)"
          % state.activeMode)
    check(not instrumented or
          any("bind a scalp mesh first" in m for m in messages),
          "unbound, pressing 2 says to bind a scalp mesh first (%r)"
          % (messages[-2:],))


def _bindGeometryFromDock(workspace):
    """Use the visible Bind scalp mesh button (and its picker, if any).

    The picker deliberately accepts a list, tree, combo or path edit.  The
    dock supplies the concrete view, while this test expresses the artist
    contract: choose /Scalp then press its affirmative button.  With exactly
    one Mesh on the stage (tonic-graph-scalp.usda) there is nothing to
    choose, so the button binds it directly and its tooltip names it.
    """
    from pxr import UsdGeom
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    QtTest = _qtTest()
    button = workspace.button("file", "bind")
    check(button is not None and
          "bind scalp mesh" in button.text().lower(),
          "the dock exposes Bind scalp mesh")
    if button is None:
        return False
    stage = workspace._api.stage
    meshes = [str(p.GetPath()) for p in stage.Traverse()
              if p.IsA(UsdGeom.Mesh)]
    if meshes == ["/Scalp"]:
        check("/Scalp" in button.toolTip(),
              "with one Mesh the Bind tooltip names it (%r)"
              % button.toolTip())
        modal = []
        # A picker would block in exec_(); this callback only records one
        # (and rejects it so the test cannot hang) -- none must appear.
        def noPicker():
            dialog = QtWidgets.QApplication.activeModalWidget()
            if isinstance(dialog, QtWidgets.QDialog):
                modal.append(dialog.objectName())
                dialog.reject()
        QtCore.QTimer.singleShot(0, noPicker)
        QtTest.QTest.mouseClick(button, QtCore.Qt.MouseButton.LeftButton)
        wait(50)
        session = getattr(workspace._container, "session", None)
        check(not modal, "the only Mesh binds without a picker (%r)"
              % (modal,))
        bound = (session is not None and session.model is not None and
                 session.scalpPath == "/Scalp")
        check(bound, "one click on Bind scalp mesh binds /Scalp")
        return bound
    result = {"dialog": False, "chosen": False}

    # QDialog.exec_() starts its nested event loop synchronously inside the
    # button callback. Queue its interaction first, then perform the real
    # click which opens the picker and runs this callback in that loop.
    def chooseScalp():
        dialog = QtWidgets.QApplication.activeModalWidget()
        result["dialog"] = isinstance(dialog, QtWidgets.QDialog)
        if not result["dialog"]:
            return
        combo = dialog.findChild(QtWidgets.QComboBox, "tonicGeometryChoices")
        if combo is not None:
            index = combo.findText("/Scalp", QtCore.Qt.MatchExactly)
            if index >= 0:
                combo.setCurrentIndex(index)
                result["chosen"] = True
        box = dialog.findChild(QtWidgets.QDialogButtonBox,
                               "tonicGeometryPickerButtons")
        ok = box.button(QtWidgets.QDialogButtonBox.Ok) if box else None
        if result["chosen"] and ok is not None:
            QtTest.QTest.mouseClick(ok, QtCore.Qt.MouseButton.LeftButton)
        elif isinstance(dialog, QtWidgets.QDialog):
            dialog.reject()

    QtCore.QTimer.singleShot(0, chooseScalp)
    QtTest.QTest.mouseClick(button, QtCore.Qt.MouseButton.LeftButton)
    check(result["dialog"], "Bind scalp mesh opens a modal scalp picker")
    check(result["chosen"], "the picker lists /Scalp as a bindable mesh")
    wait(50)
    return bool(result["chosen"])


def _setPtexDensity(workspace):
    """Set the shared Ptex density to exactly 128 through its widget."""
    from pxr.Usdviewq.qt import QtWidgets
    widget = _widgetFor(
        workspace, lambda d: d.id == "texelResolution" or
        ("ptex" in d.label.lower() and "density" in d.label.lower()))
    check(widget is not None, "Output exposes a Ptex density widget")
    if widget is None:
        return None
    if isinstance(widget, QtWidgets.QComboBox):
        index = widget.findData("128")
        check(index >= 0, "Ptex density offers an explicit 128 setting")
        if index < 0:
            return None
        widget.setCurrentIndex(index)
        return "128"
    if isinstance(widget, (QtWidgets.QSpinBox, QtWidgets.QDoubleSpinBox)):
        value = 128
        check(widget.minimum() <= value <= widget.maximum(),
              "Ptex density accepts 128")
        widget.setValue(value)
        return 128
    check(False, "Ptex density uses a combo or numeric widget")
    return None


def _verifyButtonRedraw(workspace, state):
    """A real shelf click requests a viewport repaint before the idle pump.

    The dock owns this request: neither this helper nor its caller paints the
    StageView.  Wrapping usdview's API lets the integration test distinguish
    the immediate callback from a coincidental later timer refresh.
    """
    from pxr.Usdviewq.qt import QtCore
    api = getattr(workspace, "_api", None)
    update = getattr(api, "UpdateViewport", None)
    tube = workspace.button("mode", "tube")
    graph = workspace.button("mode", "graph")
    if not callable(update) or tube is None or graph is None:
        check(False, "the mode shelf exposes an instrumentable viewport redraw")
        return
    calls = []

    def countedUpdate(*args, **kwargs):
        calls.append(1)
        return update(*args, **kwargs)

    try:
        api.UpdateViewport = countedUpdate
    except (AttributeError, TypeError):
        check(False, "the usdview API accepts viewport redraw instrumentation")
        return
    try:
        QtTest = _qtTest()
        before = len(calls)
        QtTest.QTest.mouseClick(tube, QtCore.Qt.MouseButton.LeftButton)
        check(state.activeMode == "tube" and len(calls) > before,
              "clicking a mode button immediately calls UpdateViewport")
        before = len(calls)
        QtTest.QTest.mouseClick(graph, QtCore.Qt.MouseButton.LeftButton)
        check(state.activeMode == "graph" and len(calls) > before,
              "returning to Graph immediately calls UpdateViewport")
    finally:
        api.UpdateViewport = update


def run(appController):
    global failures
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import tonicCamera, tonicLib, tonicPanels
        from testUsdviewTonicGraph import Mouse
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenTonicTools.container()

    def shutdown():
        """Always release testusdview's live controller/model on a failure."""
        viewport = getattr(container, "viewport", None)
        session = getattr(container, "session", None)
        try:
            if viewport is not None:
                viewport.uninstall()
        finally:
            if session is not None:
                session.deactivate()

    if view is None or registry is None or container is None:
        print("FAIL: no stage view / plugin registry / Tonic container")
        return 1
    check(frameScalp(stage, view), "the top camera is active")
    view.setFocus()
    wait(30)

    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    workspace = container.workspace
    check(workspace is not None and workspace.isVisible(),
          "Open workspace shows the Tonic dock")
    if workspace is None:
        shutdown()
        return 1
    _checkUnboundDock(workspace, container, view)
    _bindGeometryFromDock(workspace)
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(session is not None and session.model is not None,
          "Bind scalp mesh creates the live Tonic model")
    workspace.refresh()
    check(workspace.button("mode", "tube").isEnabled() and
          workspace._paramsStack.isEnabled(),
          "binding enables the mode shelf and the parameters")
    check(not workspace._firstRunHint.isVisible(),
          "the first-run hint hides once a scalp is bound")
    check(workspace._geometryPathLabel.text() == "Scalp: /Scalp",
          "the dock names the bound scalp (%r)"
          % workspace._geometryPathLabel.text())
    check(session is not None and session.scalpPath == "/Scalp",
          "the selected mesh is persistently bound as /Scalp")
    if session is None or session.model is None or viewport is None:
        shutdown()
        return 1

    # Ptex density is shared grooming state: set Graph's visible 128 value,
    # then require Output to show the same value and the session to apply it.
    check(state.activeMode == "graph" and state.graphSubMode == "region",
          "binding enters Graph's default Region sub-mode")
    workspace.refresh()
    chosenDensity = _setPtexDensity(workspace)
    check(chosenDensity == "128" and session.bakeTexelResolution == 128,
          "Graph's Ptex density widget applies 128 texels per face side")
    viewport.setMode("output")
    workspace.refresh()
    densityWidget = _widgetFor(
        workspace, lambda d: d.id == "texelResolution" or
        ("ptex" in d.label.lower() and "density" in d.label.lower()))
    if chosenDensity is not None and densityWidget is not None:
        if hasattr(densityWidget, "currentData"):
            shown = densityWidget.currentData() or densityWidget.currentText()
        else:
            shown = densityWidget.value()
        check(str(shown) == str(chosenDensity),
              "Output synchronizes Graph's chosen Ptex density (%r)"
              % shown)

    # Closing/reopening must preserve the persistent binding and the dock.
    workspace.close()
    wait(20)
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    workspace = container.workspace
    check(workspace is not None and workspace.isVisible() and
          session.scalpPath == "/Scalp",
          "reopening the workspace preserves its mesh binding")

    viewport.setMode("graph")
    workspace.refresh()
    # The Output combo was the last real widget edited.  The viewport key
    # filter intentionally leaves typed fields alone, so restore focus to
    # the StageView before testing Enter/Backspace/Escape hotkeys.
    view.setFocus()
    wait(10)
    _verifyButtonRedraw(workspace, state)
    check(state.graphSubMode == "region",
          "Graph defaults to the point CV Region sub-mode")
    typeKey(view, "r")
    check(state.graphSubMode == "region", "R selects Region")
    check(workspace.button("mode", "graph").isChecked() and
          workspace.button("sub", "region").isChecked(),
          "the Graph and Region shelf buttons visibly remain checked")

    camera = tonicCamera.resolve(view)
    check(camera is not None, "the viewport controller camera resolves")
    if camera is None:
        shutdown()
        return 1
    state.snapRadiusPx = max(
        0.05 / max(camera.worldPerPixel((0.0, 0.0, 0.0)), 1e-9), 2.0)

    def pixel(point):
        x, z = point
        p = camera.worldToPixels((x, 0.0, z))
        return (p[0], p[1])

    mouse = Mouse(view)
    mouse.direct = True                 # all clicks target the StageView

    def pointerEvent(kind):
        """Send an idle enter/leave through the same StageView filter."""
        event = mouse._QtCore.QEvent(kind)
        return mouse._QtWidgets.QApplication.sendEvent(view, event)

    baseline = session.graphCounts()
    check(baseline == (0, 0, 0), "the single quad starts without regions")

    # A draft does not change graph topology.  Backspace removes one CV;
    # therefore Enter with only two left cannot close anything.
    for point in LEFT:
        mouse.click(pixel(point))
    check(session.graphCounts() == baseline,
          "unclosed CV clicks remain a draft and author no graph")
    typeKey(view, "backspace")
    typeKey(view, "return")
    check(session.graphCounts() == baseline,
          "Backspace removes the last draft CV before Enter closure")
    check(len(viewport.regionDraftPreview()["points"]) == 2,
          "the rejected two-CV closure retains precisely its two draft CVs")
    # Clear that deliberately incomplete draft before starting Escape's own
    # three-CV case.  Otherwise its first point is still the close target.
    typeKey(view, "escape")
    check(session.graphCounts() == baseline and
          not viewport.regionDraftPreview()["points"],
          "Escape clears the rejected two-CV draft")

    # Escape drops a fresh draft entirely.
    for point in LEFT:
        mouse.click(pixel(point))
    check(len(viewport.regionDraftPreview()["points"]) == 3,
          "three CV clicks populate the transient region contour")
    typeKey(view, "escape")
    check(session.graphCounts() == baseline and
          not viewport.regionDraftPreview()["points"],
          "Escape cancels the CV draft without a graph edit")

    # Enter closes the first triangle and creates exactly one auto root.
    for point in LEFT:
        mouse.click(pixel(point))
    # MD-05: hovering 6 px from the first draft CV snaps the rubber band onto
    # it and arms the close, which the overlay draws as a closing edge.
    firstPixel = pixel(LEFT[0])
    mouse.move((firstPixel[0] + 6.0, firstPixel[1]))
    wait(25)
    closing = viewport.regionDraftPreview()
    firstPoint = closing["points"][0] if closing["points"] else None
    snapped = (closing["hover"] is not None and firstPoint is not None and
               all(abs(closing["hover"][i] - firstPoint[i]) < 1e-6
                   for i in range(3)))
    check(snapped and closing.get("closeArmed") is True,
          "a 6 px hover over the first draft CV snaps onto it and arms the "
          "close (%r)" % (closing,))
    check(viewport._regionOverlay is not None and
          viewport._regionOverlay.isVisible(),
          "the draft overlay is showing the armed closing edge")
    middle = pixel((-0.52, -0.26))
    mouse.move(middle)
    wait(25)
    check(not viewport.regionDraftPreview().get("closeArmed"),
          "moving off the first CV disarms the close")
    typeKey(view, "return")
    afterFirst = session.graphCounts()
    firstRoots = l1TubeIds(session)
    check(afterFirst[2] == 1 and len(firstRoots) == 1,
          "Enter closes one CV polygon and creates its one L1 tube (%r/%r)"
          % (afterFirst, firstRoots))
    # MD-01: the new stub grows its guides at once. No Fill panel value
    # has been touched in this run, which is exactly the case that used to
    # show a bare tube until the artist nudged a Fill parameter.
    guidesAfterFirst = guideCount(session)
    check(guidesAfterFirst > 0,
          "the closed region shows guides without a Fill panel edit (%d)"
          % guidesAfterFirst)

    # This is the artist path, not a direct GraphCreateRegion fixture. The
    # auto Region ring keeps one published root section CV for every authored
    # triangle corner; read the canonical graph nodes and staged K5 mesh only
    # through public ABI queries.
    from usdGenTonicTools import tonicBridge
    firstArtistLoops = regionLoops(session)
    rootRing = (tonicBridge.tubeSection(session.dll, session.model,
                                        firstRoots[0], 0)[1]
                if firstRoots else [])
    staged = stagedTubePoints(session)
    authoredCorners = ([graphNode(session, nodeId)[2]
                        for nodeId in firstArtistLoops[0]]
                       if len(firstArtistLoops) == 1 else [])
    check(len(rootRing) == len(LEFT) and staged is not None and
          samePointSet(staged[:len(rootRing)], authoredCorners),
          "artist-created Region auto root publishes one base CV at each graph corner")

    # Binding an invalid replacement must fail before session.activate(),
    # which would otherwise tear this edited groom down.  The deliberately
    # malformed face index is enough to reject the Mesh topology.
    from pxr import Sdf, UsdGeom
    malformedPath = Sdf.Path("/TonicMalformedMesh")
    malformed = UsdGeom.Mesh.Define(stage, malformedPath)
    malformed.CreatePointsAttr().Set([
        (-0.1, 0.0, -0.1), (0.1, 0.0, -0.1), (0.0, 0.0, 0.1)])
    malformed.CreateFaceVertexCountsAttr().Set([3])
    malformed.CreateFaceVertexIndicesAttr().Set([0, 1, 99])
    modelBeforeInvalid = session.model
    invalidResult = container.bindGeometry(workspace._api,
                                           str(malformedPath), replace=True)
    check(not invalidResult and session.model is modelBeforeInvalid and
          session.scalpPath == "/Scalp" and
          session.graphCounts() == afterFirst,
          "a malformed replacement is rejected before it can discard the "
          "bound groom")
    _pickMalformedFromDock(workspace, str(malformedPath))
    check(session.model is modelBeforeInvalid and
          session.scalpPath == "/Scalp",
          "picking the malformed Mesh in the dock keeps the bound groom")
    stage.RemovePrim(malformedPath)
    view.setFocus()
    wait(10)

    # Re-selecting the exact same mesh is intentionally a no-op: it must
    # not clear the graph merely because the artist reopened the picker.
    sameResult = container.bindGeometry(workspace._api, "/Scalp",
                                        replace=True)
    check(sameResult and session.model is modelBeforeInvalid and
          session.scalpPath == "/Scalp" and
          session.graphCounts() == afterFirst,
          "binding the same mesh preserves the graph and live model")

    # The entire region-and-auto-tube transaction is one undo item.
    typeKey(view, "z", ("ctrl",))
    check(session.graphCounts() == baseline and not l1TubeIds(session),
          "one undo removes the region and its automatic tube")
    typeKey(view, "y", ("ctrl",))
    check(session.graphCounts()[2] == 1 and len(l1TubeIds(session)) == 1,
          "redo restores the region and its automatic tube")

    # Start the next contour on a CV of the closed first triangle. The graph
    # snap preference is screen pixels and the model's snap radius is rest
    # units, so the row keeps its 1 px in the tool state (the loops convert
    # at the point of use) and must survive the dock's next refresh; it
    # never lands in the model as 1.0 world unit.
    firstLoops = regionLoops(session)
    firstCounts = session.graphCounts()
    nativeBefore = float(session.dll.Tonic_GetSnapRadius(session.model))
    snapDescriptor = next((descriptor for descriptor in
                           tonicPanels.descriptors("graph", state)
                           if descriptor.id == "snapRadiusPx"), None)
    if snapDescriptor is not None:
        snapDescriptor.set(state, session, 1.0)
    workspace.refresh()
    nativeSnap = float(session.dll.Tonic_GetSnapRadius(session.model))
    check(snapDescriptor is not None and
          abs(float(state.snapRadiusPx) - 1.0) < 1e-5 and
          abs(nativeSnap - nativeBefore) < 1e-7,
          "the real Graph Snap radius control keeps 1 px through a dock "
          "refresh and leaves the model's world radius alone (%.4f px, "
          "native %.4f -> %.4f)" % (float(state.snapRadiusPx),
                                    nativeBefore, nativeSnap))

    def stableNodeAt(point, loops=None):
        """The stable id at an authored fixture point, independent of winding."""
        best = (float("inf"), -1)
        ids = {nodeId for loop in (loops if loops is not None else
                                   regionLoops(session)) for nodeId in loop}
        for nodeId in ids:
            node = graphNode(session, nodeId)
            if node is None:
                continue
            x, _y, z = node[2]
            candidate = (x - point[0]) ** 2 + (z - point[1]) ** 2
            if candidate < best[0]:
                best = (candidate, int(nodeId))
        return best[1] if best[0] < 2.5e-3 else -1

    def displayedPixel(nodeId):
        point = (ctypes.c_float * 3)()
        entry = getattr(session.dll, "Tonic_GraphGetNodeDisplayPosition", None)
        if (entry is None or entry(session.model, int(nodeId), point) != 0):
            return None
        return camera.worldToPixels((float(point[0]), float(point[1]),
                                     float(point[2])))

    sharedId = stableNodeAt(SHARED_VERTEX[0], firstLoops)
    sharedPixel = displayedPixel(sharedId)
    # Six physical pixels deliberately misses the native 1 px topology
    # preference. Region's displayed-CV target must prehighlight and reuse
    # the stable id before the first draft click, rather than creating a
    # near-coincident replacement from the K1 scalp sample beneath it.
    sharedClick = (sharedPixel[0] + 6.0, sharedPixel[1]) \
        if sharedPixel is not None else pixel(SHARED_VERTEX[0])
    check(sharedId >= 0 and sharedPixel is not None,
          "the first closed region exposes its shared CV stable id")
    if sharedClick is not None:
        mouse.move(sharedClick)
        wait(25)
        check(graphHover(session) == (tonicLib.TONIC_PICK_GRAPH_NODE,
                                      sharedId, -1, -1),
              "a 6 px Region hover prehighlights the existing first CV at native snap 1")
        # This only moves two physical pixels. The small move must retain the
        # target even if normal hover throttling coalesces it.
        mouse.move((sharedClick[0], sharedClick[1] + 2.0))
        wait(25)
        check(graphHover(session) == (tonicLib.TONIC_PICK_GRAPH_NODE,
                                      sharedId, -1, -1),
              "a 2 px Region hover move retains the first CV prehighlight")
        mouse.move(sharedClick)
        wait(15)
    mouse.click(sharedClick)
    draft = tuple(viewport.loop._regionDraft)
    check(len(draft) == 1 and draft[0][6] == sharedId,
          "the offset first Region click records the existing stable CV id")
    for point in SHARED_VERTEX[1:]:
        mouse.click(pixel(point))
    mouse.click(sharedClick)
    secondCounts = session.graphCounts()
    secondRoots = l1TubeIds(session)
    secondLoops = regionLoops(session)
    secondRegions = [int(session.dll.Tonic_RegionForTube(session.model, tube))
                     for tube in secondRoots]
    sharedNodes = (set(secondLoops[0]) & set(secondLoops[1])
                   if len(secondLoops) == 2 else set())
    check(secondCounts == (firstCounts[0] + 2, firstCounts[1] + 3,
                           firstCounts[2] + 1) and len(secondRoots) == 2,
          "a shared-CV triangle adds only two nodes and three edges (%r)"
          % (secondCounts,))
    check(len(set(secondRegions)) == 2 and min(secondRegions) >= 0,
          "the two same-face polygons keep distinct region-root bindings %r"
          % secondRegions)
    check(len(sharedNodes) == 1 and firstLoops and
          any(set(loop) == set(firstLoops[0]) for loop in secondLoops),
          "the second region reuses exactly one original graph CV %r"
          % sorted(sharedNodes))

    # The second transaction remains atomic: undo restores the first region
    # and every original CV; redo restores the same shared-node topology.
    typeKey(view, "z", ("ctrl",))
    check(session.graphCounts() == firstCounts and
          regionLoops(session) == firstLoops and len(l1TubeIds(session)) == 1,
          "undo removes only the shared region and preserves the original "
          "region CVs")
    typeKey(view, "y", ("ctrl",))
    redoLoops = regionLoops(session)
    redoShared = (set(redoLoops[0]) & set(redoLoops[1])
                  if len(redoLoops) == 2 else set())
    check(session.graphCounts() == secondCounts and len(l1TubeIds(session)) == 2
          and len(redoShared) == 1,
          "redo restores the shared-CV region, root and topology")

    # The same Region tool also accepts both ends of an existing edge. The
    # new triangle is deliberately on the opposite side of LEFT's base, so
    # it adds only its third CV and two boundary edges while reusing that
    # base as its shared edge.
    edgeBeforeCounts = session.graphCounts()
    edgeBeforeLoops = regionLoops(session)
    edgeAId = stableNodeAt(EDGE_NEIGHBOUR[0], edgeBeforeLoops)
    edgeBId = stableNodeAt(EDGE_NEIGHBOUR[1], edgeBeforeLoops)
    edgeA = displayedPixel(edgeAId)
    edgeB = displayedPixel(edgeBId)
    edgeAClick = (edgeA[0] + 6.0, edgeA[1]) \
        if edgeA is not None else pixel(EDGE_NEIGHBOUR[0])
    edgeBClick = (edgeB[0] - 6.0, edgeB[1]) \
        if edgeB is not None else pixel(EDGE_NEIGHBOUR[1])
    check(edgeAId >= 0 and edgeBId >= 0 and edgeA is not None and
          edgeB is not None,
          "both existing shared-edge CVs have visible stable ids")

    # Region creation exposes the next CV before the very first click and
    # keeps that identity through the draft. This is deliberately six pixels
    # off each displayed dot while the actual Graph Snap setting is 1.
    mouse.move(edgeAClick)
    wait(25)
    check(graphHover(session) == (tonicLib.TONIC_PICK_GRAPH_NODE,
                                  edgeAId, -1, -1),
          "a 6 px pre-click Region hover identifies the first shared-edge CV")
    mouse.move((edgeAClick[0], edgeAClick[1] + 2.0))
    wait(25)
    check(graphHover(session) == (tonicLib.TONIC_PICK_GRAPH_NODE,
                                  edgeAId, -1, -1),
          "a 2 px Region hover move keeps the first shared-edge CV identity")
    left = pointerEvent(mouse._QtCore.QEvent.Type.Leave)
    wait(20)
    check(left and graphHover(session) == (0, -1, -1, -1),
          "an idle StageView leave clears the Region CV prehighlight")
    entered = pointerEvent(mouse._QtCore.QEvent.Type.Enter)
    mouse.move(edgeAClick)
    wait(25)
    check(entered and graphHover(session) ==
          (tonicLib.TONIC_PICK_GRAPH_NODE, edgeAId, -1, -1),
          "StageView re-entry restores the first Region CV prehighlight")
    mouse.click(edgeAClick)
    draft = tuple(viewport.loop._regionDraft)
    check(len(draft) == 1 and draft[0][6] == edgeAId,
          "the offset first shared-edge click retains its stable id")
    mouse.move(edgeBClick)
    wait(25)
    check(graphHover(session) == (tonicLib.TONIC_PICK_GRAPH_NODE,
                                  edgeBId, -1, -1),
          "a 6 px draft Region hover identifies the second shared-edge CV")
    mouse.click(edgeBClick)
    draft = tuple(viewport.loop._regionDraft)
    check(len(draft) == 2 and tuple(entry[6] for entry in draft) ==
          (edgeAId, edgeBId),
          "the offset second shared-edge click retains its stable id")
    mouse.click(pixel(EDGE_NEIGHBOUR[2]))
    mouse.move(edgeAClick)
    wait(20)
    check(graphHover(session) == (tonicLib.TONIC_PICK_GRAPH_NODE,
                                  edgeAId, -1, -1),
          "the matching offset close click rehighlights the first shared-edge CV")
    mouse.click(edgeAClick)
    edgeCounts = session.graphCounts()
    edgeLoops = regionLoops(session)
    newEdgeLoops = [loop for loop in edgeLoops if loop not in edgeBeforeLoops]
    edgeShared = (set(edgeBeforeLoops[0]) & set(newEdgeLoops[0])
                  if len(newEdgeLoops) == 1 else set())
    check(edgeCounts == (edgeBeforeCounts[0] + 1,
                         edgeBeforeCounts[1] + 2,
                         edgeBeforeCounts[2] + 1) and
          len(l1TubeIds(session)) == 3 and
          edgeShared == {edgeAId, edgeBId},
          "an adjacent region reuses its shared edge (one node/two edges) "
          "with two stable IDs %r" % sorted(edgeShared))
    typeKey(view, "z", ("ctrl",))
    check(session.graphCounts() == edgeBeforeCounts and
          regionLoops(session) == edgeBeforeLoops and len(l1TubeIds(session)) == 2,
          "undo removes the shared-edge region without changing its neighbours")
    typeKey(view, "y", ("ctrl",))
    redoEdgeLoops = regionLoops(session)
    redoEdgeShared = [set(a) & set(b)
                      for index, a in enumerate(redoEdgeLoops)
                      for b in redoEdgeLoops[index + 1:]]
    check(session.graphCounts() == edgeCounts and len(l1TubeIds(session)) == 3
          and any(len(shared) == 2 for shared in redoEdgeShared),
          "redo restores the shared-edge topology and its auto tube")

    # The earlier shared-edge case is top-down.  This one starts a new
    # contour at already committed A, then clicks committed B through their
    # *rendered* dots after a close, tilted camera move.  The graph overlay
    # lifts dots by 0.004 times the scalp bounding diagonal along +Y.  On
    # this +Y single-quad fixture that is exact scene-index math, not a
    # synthetic pick or an authoring-coordinate shortcut.
    tiltedSource = next((loop for loop in secondLoops
                         if set(loop) != set(firstLoops[0])), ())

    def sourceIdAt(point):
        """Find the source-loop ID at an authored fixture coordinate."""
        matches = []
        for nodeId in tiltedSource:
            node = graphNode(session, nodeId)
            if node is None:
                continue
            x, _y, z = node[2]
            matches.append(((x - point[0]) ** 2 + (z - point[1]) ** 2,
                            int(nodeId)))
        # Qt delivers integer pixels; the initial wide camera therefore
        # quantizes the canonical K1 point by a few hundredths of a unit.
        # The three fixture CVs are much farther apart than this window.
        return min(matches)[1] if matches and min(matches)[0] < 2.5e-3 else -1

    # Region loop winding is canonicalized, so its storage order is not the
    # authoring order. Select the actual shared base by canonical locations:
    # A (-.22,-.56) to B (.42,-.54), whose existing third CV is above it.
    tiltedA = sourceIdAt(SHARED_VERTEX[0])
    tiltedB = sourceIdAt(SHARED_VERTEX[1])
    nodeA = graphNode(session, tiltedA)
    nodeB = graphNode(session, tiltedB)
    check(nodeA is not None and nodeB is not None,
          "two consecutive committed CV ids are available for tilted sharing")
    check(frameTiltedZoomScalp(stage, view),
          "a close tilted camera is active for rendered-CV reuse")
    wait(30)
    camera = tonicCamera.resolve(view)
    # Keep the model and state synchronized for the later rendered-dot
    # camera test as well; only direct UI-equivalent descriptor writes are
    # allowed in this viewport workflow.
    if snapDescriptor is not None:
        snapDescriptor.set(state, session, 2.0)
    graphLift = 0.004 * math.sqrt(8.0)  # diagonal of [-1,1] x [-1,1]

    def renderedPixel(node):
        if node is None or camera is None:
            return None
        point = node[2]
        return camera.worldToPixels((point[0], point[1] + graphLift, point[2]))

    def rawPixel(node):
        return None if node is None or camera is None else \
            camera.worldToPixels(node[2])

    tiltedAPixel, tiltedBPixel = renderedPixel(nodeA), renderedPixel(nodeB)
    tiltedRawB = rawPixel(nodeB)
    tiltedThird = (camera.worldToPixels((TILTED_EDGE_NEIGHBOUR[0], 0.0,
                                         TILTED_EDGE_NEIGHBOUR[1]))
                   if camera is not None else None)
    liftPixels = (math.hypot(tiltedBPixel[0] - tiltedRawB[0],
                             tiltedBPixel[1] - tiltedRawB[1])
                  if tiltedBPixel is not None and tiltedRawB is not None else 0.0)
    def inViewport(point):
        return (point is not None and 0.0 <= point[0] < camera.width and
                0.0 <= point[1] < camera.height)

    sourceOther = next((nodeId for nodeId in tiltedSource
                        if nodeId not in (tiltedA, tiltedB)), -1)
    otherNode = graphNode(session, sourceOther)
    def sideOfAB(point):
        if nodeA is None or nodeB is None or point is None:
            return 0.0
        ax, _ay, az = nodeA[2]
        bx, _by, bz = nodeB[2]
        return (bx - ax) * (point[1] - az) - (bz - az) * (point[0] - ax)

    existingSide = sideOfAB((otherNode[2][0], otherNode[2][2])
                            if otherNode is not None else None)
    newSide = sideOfAB(TILTED_EDGE_NEIGHBOUR)
    ready = (camera is not None and tiltedA >= 0 and tiltedB >= 0 and
             tiltedAPixel is not None and tiltedBPixel is not None and
             tiltedThird is not None and inViewport(tiltedAPixel) and
             inViewport(tiltedBPixel) and inViewport(tiltedThird) and
             liftPixels > state.snapRadiusPx and existingSide * newSide < 0.0)
    check(ready,
          "rendered A/B and the new third are visible; B lift is %.2fpx "
          "and the third is opposite the committed source edge" % liftPixels)
    if ready:
        tiltedBeforeCounts = session.graphCounts()
        tiltedBeforeLoops = regionLoops(session)
        mouse.click((tiltedAPixel[0], tiltedAPixel[1]))
        draft = tuple(viewport.loop._regionDraft)
        check(len(draft) == 1 and draft[0][6] == tiltedA,
              "the rendered committed A click retains its stable node id %d"
              % tiltedA)
        mouse.click((tiltedBPixel[0], tiltedBPixel[1]))
        draft = tuple(viewport.loop._regionDraft)
        check(len(draft) == 2 and tuple(entry[6] for entry in draft) ==
              (tiltedA, tiltedB),
              "the next rendered committed B click reuses its exact id %d"
              % tiltedB)
        mouse.click((tiltedThird[0], tiltedThird[1]))
        mouse.click((tiltedAPixel[0], tiltedAPixel[1]))
        tiltedCounts = session.graphCounts()
        tiltedLoops = regionLoops(session)
        tiltedShared = [set(loop) & set(tiltedSource)
                        for loop in tiltedLoops if tuple(loop) != tuple(tiltedSource)]
        check(tiltedCounts == (tiltedBeforeCounts[0] + 1,
                               tiltedBeforeCounts[1] + 2,
                               tiltedBeforeCounts[2] + 1) and
              any(shared == {tiltedA, tiltedB} for shared in tiltedShared),
              "tilted A-to-B shared edge adds one node, two edges and one region")
        typeKey(view, "z", ("ctrl",))
        check(session.graphCounts() == tiltedBeforeCounts and
              regionLoops(session) == tiltedBeforeLoops,
              "undo removes only the tilted shared-edge region")
        typeKey(view, "y", ("ctrl",))
        redoTiltedLoops = regionLoops(session)
        redoTiltedShared = [set(loop) & set(tiltedSource)
                            for loop in redoTiltedLoops
                            if tuple(loop) != tuple(tiltedSource)]
        check(session.graphCounts() == tiltedCounts and
              any(shared == {tiltedA, tiltedB} for shared in redoTiltedShared),
              "redo restores the exact tilted A/B shared node ids")

    shutdown()
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicCvRegions needs testusdview")
    sys.exit(0)
