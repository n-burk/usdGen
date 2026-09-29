# testUsdviewTonicNavigation -- T3: camera navigation, focus and the dock's
# open/close hand-over while the Tonic workspace is live (TS-02, navigation
# half).
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicNavigation.py \
#               examples/tonic-graph-scalp.usda
#
# Everything is driven by real QMouseEvents / QWheelEvents at the StageView
# and QtTest keys and clicks at the view and the dock's own widgets, with
# the USD scalp ACTIVE (so a leaked StageView pick has something to hit).
# Checks read what the artist sees: the camera's view-projection and
# usdview's free camera, usdview's prim selection and its rollover /
# pick / context-menu signals, Tonic's published selection and CVs, the
# dock's widgets and TonicToolState.
#
# Covered:
#   * idle hover is Tonic's: no usdview rollover pick, no prim tooltip;
#   * a DCC (default): Alt+LMB orbit, Alt+MMB pan, Alt+RMB dolly move the
#     camera, render once per move, never open a Tonic gesture, never pick
#     or change either selection; plain MMB / RMB are inert (no pick, no
#     context menu, no camera move);
#   * the Display group's Navigation combo, driven by keys, switches to
#     a DCC and a focused (non-editable) combo still lets hotkeys through;
#   * a DCC: MMB orbits, Shift+MMB pans, Ctrl+MMB dollies usdview's free
#     camera, with a CV selected and never as a handle repeat; Alt+LMB
#     still orbits;
#   * an idle wheel dollies; a wheel during a gizmo drag is swallowed and
#     the drag finishes as one undo step;
#   * F frames the Tonic selection after a DCC pan;
#   * a focused dock spin box owns the keys until a viewport click takes
#     them back;
#   * closing the dock with its title-bar close button hands the view back
#     to usdview (prim picks work, Tonic keys are dead, the StageView's own
#     focus policy) and Open workspace brings Tonic back with keys and picks;
#   * before any of that, usdGen > Tonic > Bind selected as scalp with no
#     dock leaves the controller suspended: a click is still usdview's prim
#     pick, no region draft starts, tracking and focus stay usdview's.
import ctypes
import os
import sys
import traceback


def testenvDir():
    """This script's directory, also when testusdview execs it without a
    __file__ (testUsdviewTonicWorkflow.py's pattern)."""
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for arg in sys.argv:
        if arg.endswith("testUsdviewTonicNavigation.py"):
            return os.path.dirname(os.path.abspath(arg))
    return os.getcwd()


RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
STUB_CVS = 5


def run(appController):
    here = testenvDir()
    sys.path.insert(0, here)
    sys.path.insert(0, os.path.normpath(os.path.join(here, "..", "python")))
    try:
        import usdGenTonicTools              # noqa: F401 - plugin package
        import tonicT3
        from tonicT3 import (Mouse, check, failureCount, frameScalp, info,
                             pumpUntilCommitted, typeKey, wait)
        from usdGenTonicTools import tonicCamera
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools/tonicT3: %s" % exc)
        return 1
    from pxr.Usdviewq.qt import QtCore, QtWidgets

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = appController._plugRegistry

    check(frameScalp(stage, view), "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)
    usdviewFocusPolicy = view.focusPolicy()
    try:
        checkMenuBindWithoutWorkspace(appController)
    except Exception as exc:              # noqa: BLE001 - report, go on
        for line in traceback.format_exc().splitlines():
            info("  %s" % line.replace("Traceback", "trace"))
        check(False, "checkMenuBindWithoutWorkspace raised %s: %s"
              % (type(exc).__name__, exc))
    session, viewport, state, workspace, container = tonicT3.openAndBind(
        appController, "/Scalp")
    check(viewport is not None and viewport.installed and
          not viewport.suspended and
          view.focusPolicy() == QtCore.Qt.FocusPolicy.ClickFocus,
          "Open workspace after the menu bind resumes the controller "
          "(suspended %s)" % (viewport is not None and viewport.suspended,))
    check(session is not None and session.model is not None and
          viewport is not None and viewport.installed and
          workspace is not None,
          "Bind scalp created a live model under an installed controller")
    if session is None or session.model is None or viewport is None or \
            workspace is None:
        return 1
    messages = tonicT3.statusRecorder()
    dockSink = workspace._onStatus

    def sink(text, level="info"):
        messages(text, level)
        dockSink(text, level)

    session.setStatusSink(sink)
    mouse = Mouse(view)
    mouse.direct = True
    viewport.setPointerInside(True)
    check(stage.GetPrimAtPath("/Scalp").IsActive(),
          "the USD scalp stays active for every navigation check")

    # Fixture: a real Draw stroke leaves the five-CV tube stub.
    camera = tonicCamera.resolve(view)
    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)
    typeKey(view, "d")
    path = []
    for index, (x0, z0) in enumerate(RECT):
        x1, z1 = RECT[(index + 1) % len(RECT)]
        for step in range(5):
            t = float(step) / 5.0
            path.append(pixel(camera, (x0 + (x1 - x0) * t, 0.0,
                                       z0 + (z1 - z0) * t)))
    path.append(pixel(camera, (RECT[0][0], 0.0, RECT[0][1])))
    mouse.drag(path)
    check(session.graphCounts() == (4, 4, 1) and
          int(session.dll.Tonic_GetCenterCVCount(session.model)) ==
          STUB_CVS,
          "a real Graph stroke creates the five-CV root tube (%r)"
          % (session.graphCounts(),))
    check(pumpUntilCommitted(viewport, session), "the stroke's commit swaps")

    # usdview-side observers, live for the whole script.
    picks, rollovers, popups = [], [], []
    view.signalPrimSelected.connect(lambda *_a: picks.append(1))
    view.signalPrimRollover.connect(lambda *_a: rollovers.append(1))

    def closePopup():
        popup = QtWidgets.QApplication.activePopupWidget()
        if popup is not None:
            popups.append(type(popup).__name__)
            popup.close()

    closer = QtCore.QTimer()
    closer.setInterval(20)
    closer.timeout.connect(closePopup)
    closer.start()

    ctx = dict(stage=stage, view=view, session=session, viewport=viewport,
               state=state, workspace=workspace, container=container,
               mouse=mouse, messages=messages, dataModel=dataModel,
               api=viewport._api, registry=registry, picks=picks,
               rollovers=rollovers, popups=popups,
               appController=appController,
               usdviewFocusPolicy=usdviewFocusPolicy)

    for section in (checkHover, checkMayaCamera, checkMayaPlainButtons,
                    checkStyleCombo, checkBlenderCamera, checkWheel,
                    checkFraming, checkFocusRecovery, checkDockCloseReopen):
        try:
            viewport.setPointerInside(True)
            view.setFocus()
            section(ctx)
        except Exception as exc:          # noqa: BLE001 - report, go on
            for line in traceback.format_exc().splitlines():
                info("  %s" % line.replace("Traceback", "trace"))
            check(False, "%s raised %s: %s"
                  % (section.__name__, type(exc).__name__, exc))
        if viewport.gestureActive:
            viewport.cancelGesture()

    closer.stop()
    state.navigationStyle = "maya"
    viewport.uninstall()
    session.deactivate()
    print("testUsdviewTonicNavigation: %d failure(s)" % failureCount())
    return 1 if failureCount() else 0


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def checkMenuBindWithoutWorkspace(appController):
    """Review HIGH: the menu bind with no dock.

    Bind selected as scalp installs the controller. With no workspace on
    screen it used to leave the view filter claiming every left click as
    a Graph region press (usdview's click-to-select dead, an invisible
    draft on the scalp) and mouse tracking forced on, which makes
    StageView run a Hydra pick per hover.  It must stay suspended until
    the workspace opens.
    """
    import usdGenTonicTools
    from tonicT3 import Mouse, check, wait
    from usdGenTonicTools import tonicCamera
    dataModel = appController._dataModel
    view = appController._stageView
    registry = appController._plugRegistry
    container = usdGenTonicTools.container()
    state = container.tonicState
    check(container.workspace is None and not state.workspaceOpen,
          "no Tonic dock exists before the menu bind")
    trackingBefore = bool(view.hasMouseTracking())
    policyBefore = view.focusPolicy()
    dataModel.selection.setPrimPath("/Scalp")
    wait(10)
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    wait(30)
    session = container.session
    viewport = container.viewport
    check(session is not None and session.model is not None and
          viewport is not None and viewport.installed,
          "the menu bind makes a live model under an installed controller")
    if viewport is None or session is None or session.model is None:
        return
    check(container.workspace is None and not state.workspaceOpen,
          "and opens no dock")
    check(viewport.suspended,
          "with no workspace on screen the controller is suspended")
    check(bool(view.hasMouseTracking()) == trackingBefore,
          "mouse tracking stays usdview's, so a hover is no Hydra pick "
          "(%r -> %r)" % (trackingBefore, bool(view.hasMouseTracking())))
    check(view.focusPolicy() == policyBefore,
          "the StageView keeps usdview's focus policy (%r -> %r)"
          % (policyBefore, view.focusPolicy()))
    camera = tonicCamera.resolve(view)
    check(camera is not None, "the StageView camera resolves")
    if camera is None:
        return
    dataModel.selection.setPrimPath("/TonicT3TopCamera")
    wait(10)
    mouse = Mouse(view)
    mouse.direct = True
    mouse.click(pixel(camera, (0.4, 0.0, 3.6)))
    wait(30)
    after = [str(path) for path in dataModel.selection.getPrimPaths()]
    check("/Scalp" in after,
          "a click on the scalp is still usdview's prim pick (%r)" % after)
    loop = viewport.loop
    check(not viewport.gestureActive and
          not getattr(loop, "_regionDraft", None),
          "and starts no invisible Graph region draft (%r)"
          % (getattr(loop, "_regionDraft", None),))


def pixel(camera, point):
    projected = camera.worldToPixels(point)
    return (projected[0], projected[1])


def viewProj(view):
    from usdGenTonicTools import tonicCamera
    camera = tonicCamera.resolve(view)
    return tuple(camera.viewProj) if camera is not None else None


def usdSelection(ctx):
    return [str(path) for path in (ctx["api"].selectedPaths or [])]


def tipHandle(session):
    from usdGenTonicTools import tonicBridge
    count = int(session.dll.Tonic_GetCenterCVCount(session.model))
    return tonicBridge.tubeCenterHandle(session.dll, session.model, 0,
                                        count - 1)


def tipCV(session):
    import tonicT3
    return tonicT3.tubeCenters(session, 0)[-1]


def freeCamera(view):
    return view._dataModel.viewSettings.freeCamera


def freeState(view):
    """(theta, phi, dist, center) of usdview's free camera, or None."""
    free = freeCamera(view)
    if free is None:
        return None
    return (float(free.rotTheta), float(free.rotPhi), float(free.dist),
            tuple(float(v) for v in free.center))


def cameraDrag(ctx, start, delta, button, modifiers=(), steps=10,
               during=None, afterPress=None):
    """A real button drag straight at the StageView; `afterPress()` runs
    once the press was delivered and `during(i)` after each move, so a
    check can watch the live state (or count renders over the moves only).
    """
    from pxr.Usdviewq.qt import QtWidgets
    mouse = ctx["mouse"]
    mouse.press(start, modifiers, button)
    QtWidgets.QApplication.processEvents()
    if afterPress is not None:
        afterPress()
    for index in range(steps):
        f = float(index + 1) / steps
        mouse.move((start[0] + delta[0] * f, start[1] + delta[1] * f),
                   modifiers)
        QtWidgets.QApplication.processEvents()
        if during is not None:
            during(index)
    mouse.release((start[0] + delta[0], start[1] + delta[1]), modifiers,
                  button)
    import tonicT3
    tonicT3.wait(20)


def selectTip(ctx):
    """A real click on the tip centre CV with the Select tool."""
    from usdGenTonicTools import tonicCamera, tonicLib
    from tonicT3 import typeKey, wait
    view, session = ctx["view"], ctx["session"]
    typeKey(view, "2")
    typeKey(view, "f9")
    typeKey(view, "q")
    typeKey(view, "a", ("ctrl", "shift"))
    wait(10)
    camera = tonicCamera.resolve(view)
    ctx["mouse"].click(pixel(camera, tipHandle(session)))
    wait(10)
    return session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) == \
        [(0, STUB_CVS - 1, -1)]


def wheel(view, physical, notches=1, held=False):
    """One real QWheelEvent at the StageView (a dolly notch)."""
    from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
    try:
        ratio = float(view.devicePixelRatioF())
    except AttributeError:
        ratio = 1.0
    local = QtCore.QPointF(physical[0] / ratio, physical[1] / ratio)
    globalPos = QtCore.QPointF(view.mapToGlobal(local.toPoint()))
    buttons = (QtCore.Qt.MouseButton.LeftButton if held
               else QtCore.Qt.MouseButton.NoButton)
    try:
        event = QtGui.QWheelEvent(
            local, globalPos, QtCore.QPoint(0, 0),
            QtCore.QPoint(0, 120 * int(notches)), buttons,
            QtCore.Qt.KeyboardModifier.NoModifier,
            QtCore.Qt.ScrollPhase.NoScrollPhase, False)
    except (TypeError, AttributeError):
        return False
    QtWidgets.QApplication.sendEvent(view, event)
    return True


def emptySpot(view):
    """A pixel well inside the view, off the tube (upper-left quarter)."""
    try:
        ratio = float(view.devicePixelRatioF())
    except AttributeError:
        ratio = 1.0
    return (view.width() * ratio * 0.2, view.height() * ratio * 0.2)


# ---------------------------------------------------------------------------
# 1. hover
# ---------------------------------------------------------------------------

def checkHover(ctx):
    from pxr.Usdviewq.qt import QtWidgets
    from usdGenTonicTools import tonicCamera
    from tonicT3 import check, frameScalp, typeKey, wait
    view, session, mouse = ctx["view"], ctx["session"], ctx["mouse"]
    frameScalp(ctx["stage"], view)
    view.setFocus()
    wait(30)
    typeKey(view, "2")
    camera = tonicCamera.resolve(view)
    tip = pixel(camera, tipHandle(session))
    before = len(ctx["rollovers"])
    for index in range(20):
        mouse.unheldMove((tip[0] + (index % 5) * 4.0 - 8.0,
                          tip[1] + (index // 5) * 30.0 - 45.0))
        wait(5)
    check(len(ctx["rollovers"]) == before,
          "20 idle moves over the groom never run usdview's rollover pick "
          "(%d)" % (len(ctx["rollovers"]) - before))
    check(not QtWidgets.QToolTip.isVisible(),
          "and never show usdview's prim tooltip")


# ---------------------------------------------------------------------------
# 2. a DCC: Alt drags
# ---------------------------------------------------------------------------

def checkMayaCamera(ctx):
    from tonicT3 import check
    view, session, viewport = ctx["view"], ctx["session"], ctx["viewport"]
    state = ctx["state"]
    check(state.navigationStyle == "maya" and
          ctx["workspace"]._navigationCombo.currentData() == "maya",
          "Maya is the default style, and the Display combo says so (%r)"
          % state.navigationStyle)
    # usdview's own Alt+MMB truck and Alt+RMB zoom scale with the free
    # camera's orbit distance, which FreeCamera.FromGfCamera takes from the
    # camera prim's focusDistance; the helper camera leaves it at 0 (the
    # pivot is then the eye and a DCC pan / dolly cannot move), so give it
    # the scalp's depth, as a real shot camera would carry.
    from pxr import UsdGeom
    from tonicT3 import frameScalp, wait
    frameScalp(ctx["stage"], view)
    UsdGeom.Camera(ctx["stage"].GetPrimAtPath("/TonicT3TopCamera")) \
        .CreateFocusDistanceAttr(12.0)
    view.setFocus()
    wait(30)
    check(selectTip(ctx), "a click selects the tip CV before the camera "
          "drags")
    from usdGenTonicTools import tonicLib
    selected = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    usdBefore = usdSelection(ctx)
    tipBefore = tipCV(session)
    start = emptySpot(view)

    MOVES = 12
    for button, name in (("left", "orbit"), ("middle", "pan"),
                         ("right", "dolly")):
        label = "Alt+%sMB" % button.upper()[0]
        picksBefore, popupsBefore = len(ctx["picks"]), len(ctx["popups"])
        cameraBefore = viewProj(view)
        freeBefore = freeState(view)
        live = []
        modes = []
        stageViewClass = type(view)
        originalPaint = stageViewClass.paintGL
        paints = [0]

        def countingPaint(self, *args, **kwargs):
            paints[0] += 1
            return originalPaint(self, *args, **kwargs)

        def armCount():
            # StageView's own camera mode after the press it was handed.
            modes.append(getattr(view, "_cameraMode", None))
            stageViewClass.paintGL = countingPaint

        def onMove(index):
            live.append(viewport.gestureActive)
            if index == MOVES - 1:
                # Count the moves only: the release re-runs one Tonic hover
                # against the camera's new position, by design (FB-01).
                stageViewClass.paintGL = originalPaint

        try:
            cameraDrag(ctx, start, (60.0, 24.0), button, ("alt",),
                       steps=MOVES, afterPress=armCount, during=onMove)
        finally:
            stageViewClass.paintGL = originalPaint
        freeAfter = freeState(view)
        check(not any(live),
              "%s (%s) never opens a Tonic gesture" % (label, name))
        check(viewProj(view) != cameraBefore,
              "%s %ss the camera (StageView mode %r, free camera %r -> %r)"
              % (label, name, modes[:1], freeBefore, freeAfter))
        check(len(ctx["picks"]) == picksBefore and
              len(ctx["popups"]) == popupsBefore and
              usdSelection(ctx) == usdBefore,
              "%s picks no prim and opens no menu (picks %d, popups %r)"
              % (label, len(ctx["picks"]) - picksBefore,
                 ctx["popups"][popupsBefore:]))
        check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
              selected and tipCV(session) == tipBefore,
              "%s leaves the Tonic selection and the CVs alone" % label)
        # FB-01: StageView's own render per camera move and no Tonic hover
        # render on top of it.
        check(paints[0] == MOVES,
              "%s renders exactly once per move (%d for %d moves)"
              % (label, paints[0], MOVES))
    check(freeCamera(view) is not None and
          view._dataModel.viewSettings.cameraPrim is None,
          "the Alt drags moved usdview's free camera, not the scene camera")


def checkMayaPlainButtons(ctx):
    from tonicT3 import check, typeKey
    view, viewport = ctx["view"], ctx["viewport"]
    typeKey(view, "5")               # Sculpt: no handle for MMB to repeat
    start = emptySpot(view)
    picksBefore, popupsBefore = len(ctx["picks"]), len(ctx["popups"])
    usdBefore = usdSelection(ctx)
    cameraBefore = viewProj(view)
    live = []
    cameraDrag(ctx, start, (40.0, 10.0), "middle",
               during=lambda _i: live.append(viewport.gestureActive))
    cameraDrag(ctx, start, (40.0, 10.0), "right",
               during=lambda _i: live.append(viewport.gestureActive))
    ctx["mouse"].click(start, button="right")
    check(len(ctx["picks"]) == picksBefore,
          "Maya: plain MMB / RMB never reach usdview's prim pick (%d)"
          % (len(ctx["picks"]) - picksBefore))
    check(len(ctx["popups"]) == popupsBefore,
          "Maya: a right click opens no usdview context menu (%r)"
          % (ctx["popups"][popupsBefore:],))
    check(usdSelection(ctx) == usdBefore,
          "Maya: the usdview prim selection is untouched")
    check(viewProj(view) == cameraBefore and not any(live),
          "Maya: plain MMB / RMB drags leave the camera alone and start no "
          "Tonic gesture")


# ---------------------------------------------------------------------------
# 3. the Display group's Navigation combo
# ---------------------------------------------------------------------------

def checkStyleCombo(ctx):
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    from tonicT3 import check, typeKey, wait
    import tonicT3
    view, state, workspace = ctx["view"], ctx["state"], ctx["workspace"]
    viewport = ctx["viewport"]
    combo = workspace.findChild(QtWidgets.QComboBox, "tonicNavigationStyle")
    check(combo is not None and combo.isVisible(),
          "the Display group shows the Navigation combo")
    if combo is None:
        return
    tip = combo.toolTip()
    check("Maya" in tip and "Blender" in tip and "MMB" in tip,
          "its tooltip explains both styles (%r)" % tip)
    combo.setFocus(QtCore.Qt.FocusReason.TabFocusReason)
    wait(10)
    qtTest = tonicT3._qtTest()
    index = combo.findData("blender")
    steps = 0
    while combo.currentIndex() != index and steps < combo.count():
        qtTest.QTest.keyClick(combo, QtCore.Qt.Key.Key_Down)
        wait(10)
        steps += 1
    check(combo.currentData() == "blender" and
          state.navigationStyle == "blender",
          "arrowing the focused combo to Blender sets the style (%r)"
          % state.navigationStyle)
    # A plain (non-editable) choice list is not typing: a mode digit sent
    # while it holds the focus still reaches the viewport table (FB-01).
    mode = state.activeMode
    qtTest.QTest.keyClick(combo, QtCore.Qt.Key.Key_3)
    wait(10)
    check(state.activeMode == "fill" and combo.currentData() == "blender",
          "with the combo focused, 3 still picks Fill and the combo keeps "
          "Blender (%r -> %r)" % (mode, state.activeMode))
    workspace.refresh()
    check(combo.currentData() == "blender",
          "a dock refresh keeps the combo on the state's style")
    ctx["mouse"].click(emptySpot(view))
    wait(10)
    check(view.hasFocus() or viewport.viewHasKeys(),
          "a viewport click takes the keys back from the combo")


# ---------------------------------------------------------------------------
# 4. a DCC: MMB orbit / Shift+MMB pan / Ctrl+MMB dolly
# ---------------------------------------------------------------------------

def checkBlenderCamera(ctx):
    from usdGenTonicTools import tonicLib
    from tonicT3 import check
    view, session, viewport = ctx["view"], ctx["session"], ctx["viewport"]
    state = ctx["state"]
    if state.navigationStyle != "blender":
        state.navigationStyle = "blender"
    check(selectTip(ctx), "Blender: a click selects the tip CV")
    typeKey = __import__("tonicT3").typeKey
    typeKey(view, "w")              # a gizmo on screen, MMB must not grab it
    selected = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    tipBefore = tipCV(session)
    start = emptySpot(view)

    for modifiers, name in (((), "orbit"), (("shift",), "pan"),
                            (("ctrl",), "dolly")):
        before = freeState(view)
        projBefore = viewProj(view)
        picksBefore = len(ctx["picks"])
        live = []
        pressed = []
        cameraDrag(ctx, start, (48.0, 20.0), "middle", modifiers,
                   afterPress=lambda: pressed.append(viewProj(view)),
                   during=lambda _i: live.append(viewport.gestureActive))
        check(pressed and pressed[0] is not None and projBefore is not None
              and all(abs(a - b) < 1e-6 for a, b in zip(pressed[0],
                                                         projBefore)),
              "the Blender %s press does not jump the view" % name)
        after = freeState(view)
        check(before is not None and after is not None,
              "Blender %s runs on usdview's free camera" % name)
        if before is None or after is None:
            continue
        rotated = (abs(after[0] - before[0]) > 1e-3 or
                   abs(after[1] - before[1]) > 1e-3)
        dollied = abs(after[2] - before[2]) > 1e-4
        panned = any(abs(a - b) > 1e-5 for a, b in zip(after[3], before[3]))
        label = "+".join(m.title() for m in modifiers) + \
            ("+" if modifiers else "") + "MMB"
        if name == "orbit":
            # The pivot may be re-seated at the groom's depth on the press
            # (the eye stays put), so only the rotation is the orbit's own.
            ok = rotated
        elif name == "pan":
            ok = panned and not rotated and not dollied
        else:
            ok = dollied and not rotated
        check(ok, "Blender %s %ss the free camera (rot %s, dist %s, "
              "centre %s)" % (label, name, rotated, dollied, panned))
        check(not any(live) and len(ctx["picks"]) == picksBefore,
              "Blender %s is no Tonic gesture and picks no prim" % label)
        check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
              selected and tipCV(session) == tipBefore,
              "Blender %s never repeats a gizmo handle: the selected CV "
              "stays put" % label)
    before = viewProj(view)
    cameraDrag(ctx, start, (40.0, 0.0), "left", ("alt",))
    check(viewProj(view) != before,
          "Blender: Alt+LMB still orbits as in usdview")


# ---------------------------------------------------------------------------
# 5. the wheel
# ---------------------------------------------------------------------------

def checkWheel(ctx):
    from usdGenTonicTools import tonicCamera
    from tonicT3 import check, typeKey, wait
    view, session, viewport = ctx["view"], ctx["session"], ctx["viewport"]
    mouse = ctx["mouse"]
    check(selectTip(ctx), "the tip CV is selected for the wheel checks")
    typeKey(view, "w")
    before = freeState(view)
    check(wheel(view, emptySpot(view), 1), "a wheel event can be built")
    wait(20)
    after = freeState(view)
    check(before is not None and after is not None and
          abs(after[2] - before[2]) > 1e-5,
          "an idle wheel notch dollies usdview's camera (%r -> %r)"
          % (before and before[2], after and after[2]))

    camera = tonicCamera.resolve(view)
    start = pixel(camera, tipHandle(session))
    home = tipCV(session)
    depth = session.undoDepth()
    mouse.press(start)
    check(viewport.gestureActive, "a press on the tip's gizmo starts a drag")
    for step in range(1, 4):
        mouse.move((start[0] + 10.0 * step, start[1]))
    cameraMid = viewProj(view)
    freeMid = freeState(view)
    wheel(view, (start[0] + 30.0, start[1]), 2, held=True)
    wait(10)
    check(viewProj(view) == cameraMid and freeState(view) == freeMid,
          "a wheel under a live drag is swallowed: the camera holds")
    check(viewport.gestureActive, "and the drag goes on")
    for step in range(4, 7):
        mouse.move((start[0] + 10.0 * step, start[1]))
    mouse.release((start[0] + 60.0, start[1]))
    moved = tipCV(session)
    check(any(abs(a - b) > 1e-3 for a, b in zip(moved, home)) and
          session.undoDepth() == depth + 1,
          "the drag lands as one undo step (%r -> %r, depth %d -> %d)"
          % (home, moved, depth, session.undoDepth()))
    typeKey(view, "z", ("ctrl",))
    wait(10)
    check(all(abs(a - b) < 1e-5 for a, b in zip(tipCV(session), home)),
          "Ctrl+Z puts the tip back")


# ---------------------------------------------------------------------------
# 6. F frames the Tonic selection
# ---------------------------------------------------------------------------

def checkFraming(ctx):
    from usdGenTonicTools import tonicCamera
    from tonicT3 import check, info, typeKey, wait
    view, session, viewport = ctx["view"], ctx["session"], ctx["viewport"]
    state = ctx["state"]
    state.navigationStyle = "blender"
    check(selectTip(ctx), "the tip CV is selected for framing")
    point = tipHandle(session)

    def offCentre():
        camera = tonicCamera.resolve(view)
        projected = camera.worldToPixels(point)
        return (abs(projected[0] - 0.5 * camera.width) / camera.width,
                abs(projected[1] - 0.5 * camera.height) / camera.height)

    cameraDrag(ctx, emptySpot(view), (-220.0, 140.0), "middle", ("shift",))
    away = offCentre()
    check(max(away) > 0.1, "Shift+MMB panned the CV off centre (%.2f, %.2f)"
          % away)
    check(viewport.hasFrameTarget(), "Tonic has a selection to frame")
    typeKey(view, "f")
    wait(30)
    framed = offCentre()
    check(framed[0] < 0.1 and framed[1] < 0.1,
          "F centres the selected CV (%.3f, %.3f of the view)" % framed)
    typeKey(view, "a", ("ctrl", "shift"))
    wait(10)
    check(not viewport.hasFrameTarget(),
          "with nothing selected F is left to usdview's Frame Selected")
    info("style back to Maya for the remaining checks")
    state.navigationStyle = "maya"


# ---------------------------------------------------------------------------
# 7. keys come back from a dock field
# ---------------------------------------------------------------------------

def checkFocusRecovery(ctx):
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    from tonicT3 import check, typeKey, wait
    import tonicT3
    view, state, workspace = ctx["view"], ctx["state"], ctx["workspace"]
    viewport = ctx["viewport"]
    typeKey(view, "2")
    spins = [spin for spin in
             workspace.findChildren(QtWidgets.QAbstractSpinBox)
             if spin.isVisible() and spin.isEnabled()]
    check(bool(spins), "the Tube page shows a spin box")
    if not spins:
        return
    spin = spins[0]
    qtTest = tonicT3._qtTest()
    qtTest.QTest.mouseClick(spin, QtCore.Qt.MouseButton.LeftButton,
                            QtCore.Qt.KeyboardModifier.NoModifier,
                            QtCore.QPoint(8, spin.height() // 2))
    wait(20)
    focus = QtWidgets.QApplication.focusWidget()
    check(focus is spin or (focus is not None and spin.isAncestorOf(focus)),
          "a click on the dock spin box gives it the keys")
    check(viewport._textFocus(), "the controller sees a text field focused")
    qtTest.QTest.keyClick(focus or spin, QtCore.Qt.Key.Key_5)
    wait(10)
    check(state.activeMode == "tube",
          "5 typed into the spin box stays the field's (%r)"
          % state.activeMode)
    # The pump / publish refresh must not steal the field's focus either.
    viewport.pumpOnce()
    wait(60)
    check(QtWidgets.QApplication.focusWidget() is focus,
          "the dock's refresh leaves the focused field focused")
    ctx["mouse"].click(emptySpot(view))
    wait(20)
    check(view.hasFocus() and not viewport._textFocus(),
          "a viewport click takes keyboard focus back")
    typeKey(view, "5")
    check(state.activeMode == "sculpt",
          "and 5 reaches Sculpt again (%r)" % state.activeMode)
    # Put the edited field's value back through its own keys is not
    # needed: the soft radius typed as '5' clamps and the model keeps it;
    # the checks above only read focus and mode.


# ---------------------------------------------------------------------------
# 8. closing and reopening the dock
# ---------------------------------------------------------------------------

def checkDockCloseReopen(ctx):
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    from usdGenTonicTools import tonicCamera, tonicLib
    from tonicT3 import check, frameScalp, typeKey, wait
    import tonicT3
    view, state, workspace = ctx["view"], ctx["state"], ctx["workspace"]
    viewport, session = ctx["viewport"], ctx["session"]
    typeKey(view, "2")
    typeKey(view, "f9")
    frameScalp(ctx["stage"], view)
    view.setFocus()
    wait(30)
    closeButton = workspace.findChild(QtWidgets.QAbstractButton,
                                      "qt_dockwidget_closebutton")
    check(closeButton is not None and closeButton.isVisible(),
          "the dock's title bar has its close button")
    if closeButton is None:
        return
    tonicT3._qtTest().QTest.mouseClick(closeButton,
                                       QtCore.Qt.MouseButton.LeftButton)
    wait(40)
    check(not workspace.isVisible() and not state.workspaceOpen and
          viewport.suspended and viewport.installed,
          "the close button hides the dock and suspends the controller, "
          "keeping the model (visible %s, open %s, suspended %s)"
          % (workspace.isVisible(), state.workspaceOpen, viewport.suspended))
    check(view.focusPolicy() == ctx["usdviewFocusPolicy"],
          "the closed dock gives the StageView usdview's focus policy back "
          "(%r, usdview %r)" % (view.focusPolicy(), ctx["usdviewFocusPolicy"]))

    typeKey(view, "3")
    check(state.activeMode == "tube",
          "with the dock closed 3 is not a Tonic key (%r)" % state.activeMode)
    camera = tonicCamera.resolve(view)
    session.clearSelection(0)
    session.publish(tonicLib.TONIC_DIRTY_SELECTION)
    ctx["dataModel"].selection.setPrimPath("/TonicT3TopCamera")
    wait(10)
    before = usdSelection(ctx)
    scalpSpot = pixel(camera, (0.4, 0.0, 3.6))
    viewport.setPointerInside(True)
    ctx["mouse"].click(scalpSpot)
    wait(30)
    check(usdSelection(ctx) != before and "/Scalp" in usdSelection(ctx),
          "a closed-dock click is usdview's prim pick (%r -> %r)"
          % (before, usdSelection(ctx)))
    check(session.selectionCount(0) == 0,
          "and never touches the Tonic selection")

    ctx["registry"].getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    wait(40)
    check(workspace.isVisible() and state.workspaceOpen and
          not viewport.suspended,
          "Open workspace shows the dock again and resumes the controller")
    check(view.focusPolicy() == QtCore.Qt.FocusPolicy.ClickFocus,
          "and a viewport click takes the keys again (ClickFocus, %r)"
          % (view.focusPolicy(),))
    check(view.hasFocus() or viewport.viewHasKeys(),
          "and the viewport has the keys (focus %s, pointer %s)"
          % (view.hasFocus(), viewport.pointerInside))
    typeKey(view, "3")
    check(state.activeMode == "fill",
          "after reopening, 3 picks Fill again (%r)" % state.activeMode)
    typeKey(view, "2")
    typeKey(view, "f9")
    typeKey(view, "q")
    ctx["dataModel"].selection.setPrimPath("/TonicT3TopCamera")
    wait(10)
    before = usdSelection(ctx)
    camera = tonicCamera.resolve(view)
    # Straight down the stub's axis every centre CV overlaps; any hit on
    # the stack is a Tonic pick.
    ctx["mouse"].click(pixel(camera, tipHandle(session)))
    wait(20)
    check(session.selectionCount(tonicLib.TONIC_PICK_CENTER_CV) >= 1,
          "a click over the tube picks a Tonic centre CV again (%r)"
          % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))
    check(usdSelection(ctx) == before,
          "and is no longer a usdview prim pick (%r)" % (usdSelection(ctx),))


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (navigation needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicNavigation needs testusdview (no live view)")
    sys.exit(0)
