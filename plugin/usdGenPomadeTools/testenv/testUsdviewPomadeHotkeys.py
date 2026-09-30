# testUsdviewPomadeHotkeys -- T3: every documented Pomade key, in every mode
# (TS-02, hotkey half).
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testUsdviewPomadeHotkeys.py \
#               examples/pomade-graph-scalp.usda
#
# Every key goes through QtTest at the StageView (or at whichever dock
# widget holds the focus), so it runs the app-level _KeyFilter, the
# ShortcutOverride claim and HotkeyAction exactly as an artist's finger
# does. Every check reads something the artist can see or the tool
# publishes: the PomadeToolState fields the dock mirrors, the dock's shelf
# and toggle buttons, the message area (the session's status sink), the
# published selection and the model's CVs, levels and undo stack.
#
# Covered:
#   * 1-6 modes and every mode's sub-mode letters, F8-F11 in Tube, with the
#     dock shelf following; keys stay mode-scoped (L is Link in Graph and
#     Lengthen in Sculpt, not the Tube orientation; +/- and F8-F11 do
#     nothing outside Tube);
#   * [ / ] in every mode move that mode's documented field one step;
#   * Graph Create region: Backspace drops the last draft CV, Enter refuses
#     a short draft, Escape discards the draft; Ctrl+A / Ctrl+I /
#     Ctrl+Shift+A over graph nodes; Shift+W / Shift+U refusals and no
#     empty undo step;
#   * Tube Q/W/E/R (and each tool's own orientation), L, + / = / -, P,
#     Ctrl+A / Ctrl+I / Ctrl+Shift+A, Escape cancel-only (idle, mid-drag,
#     text focus), undo refused mid-drag, J / X holds during a drag,
#     Delete (and its off-viewport gate), Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z /
#     Shift+Z;
#   * Hierarchy Ctrl+A, Shift+D, Ctrl+Up / Ctrl+Down, Backspace, Shift+M,
#     Ctrl+Z, and W jumping to Tube with the tube kept;
#   * Ctrl+Shift+S reaching the container's one save path (stubbed).
import ctypes
import os
import sys
import traceback


def testenvDir():
    """This script's directory, also when testusdview execs it without a
    __file__ (testUsdviewPomadeWorkflow.py's pattern)."""
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for arg in sys.argv:
        if arg.endswith("testUsdviewPomadeHotkeys.py"):
            return os.path.dirname(os.path.abspath(arg))
    return os.getcwd()


RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
STUB_CVS = 4

# The sub-mode letters each mode documents (pomadeModes tables), as
# (key, sub-mode id). Hierarchy's W/E/R are Tube jumps, not letters.
SUBMODE_KEYS = {
    "graph": (("r", "region"), ("d", "draw"), ("p", "place"),
              ("m", "reposition"), ("c", "connect"), ("w", "weld"),
              ("u", "unweld"), ("x", "delete"), ("l", "link")),
    "tube": (("f8", "tube"), ("f9", "center"), ("f10", "ring"),
             ("f11", "section")),
    "fill": (("p", "params"), ("v", "preview")),
    "hierarchy": (("n", "navigate"), ("d", "subdivide"), ("m", "merge"),
                  ("g", "group"), ("l", "levels")),
    "sculpt": (("g", "grab"), ("s", "smooth"), ("c", "comb"),
               ("l", "lengthen"), ("t", "twist")),
}
SUBMODE_FIELDS = {"graph": "graphSubMode", "tube": "tubeSubMode",
                  "fill": "fillSubMode", "hierarchy": "hierarchySubMode",
                  "sculpt": "sculptSubMode"}
MODE_KEYS = (("1", "graph"), ("2", "tube"), ("3", "fill"),
             ("4", "hierarchy"), ("5", "sculpt"), ("6", "output"))


def run(appController):
    here = testenvDir()
    sys.path.insert(0, here)
    sys.path.insert(0, os.path.normpath(os.path.join(here, "..", "python")))
    try:
        import usdGenPomadeTools
        import pomadeT3
        from pomadeT3 import (Mouse, check, failureCount, frameScalp, info,
                             obliqueCamera, pumpUntilCommitted, typeKey,
                             wait)
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools/pomadeT3: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = appController._plugRegistry
    container = usdGenPomadeTools.container()

    check(frameScalp(stage, view), "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)
    session, viewport, state, workspace, container = pomadeT3.openAndBind(
        appController, "/Scalp")
    check(session is not None and session.model is not None and
          viewport is not None and viewport.installed and
          workspace is not None,
          "Bind scalp created a live model under an installed controller")
    if session is None or session.model is None or viewport is None or \
            workspace is None:
        return 1

    # Record every status line and still feed the dock's message area.
    messages = pomadeT3.statusRecorder()
    dockSink = workspace._onStatus

    def sink(text, level="info"):
        messages(text, level)
        dockSink(text, level)

    session.setStatusSink(sink)
    stage.GetPrimAtPath("/Scalp").SetActive(False)
    mouse = Mouse(view)
    mouse.direct = True
    viewport.setPointerInside(True)

    ctx = dict(stage=stage, view=view, session=session, viewport=viewport,
               state=state, workspace=workspace, container=container,
               mouse=mouse, messages=messages, dataModel=dataModel,
               api=viewport._api, registry=registry)

    if not buildFixture(ctx):
        viewport.uninstall()
        session.deactivate()
        print("testUsdviewPomadeHotkeys: %d failure(s)" % failureCount())
        return 1

    for section in (checkModesAndSubModes, checkBrackets, checkGraphKeys,
                    checkTubeTools, checkTubeSelectionAndEscape,
                    checkTubeHolds, checkTubeDeleteAndRedo,
                    checkHierarchyKeys, checkSaveKey):
        try:
            viewport.setPointerInside(True)
            view.setFocus()
            section(ctx)
        except Exception as exc:          # noqa: BLE001 - report, go on
            for line in traceback.format_exc().splitlines():
                info("  %s" % line.replace("Traceback", "trace"))
            check(False, "%s raised %s: %s"
                  % (section.__name__, type(exc).__name__, exc))
        # Never leave a live gesture or a held key to the next section.
        if viewport.gestureActive:
            viewport.cancelGesture()
        viewport._holds.clear()

    viewport.uninstall()
    session.deactivate()
    print("testUsdviewPomadeHotkeys: %d failure(s)" % failureCount())
    return 1 if failureCount() else 0


# ---------------------------------------------------------------------------
# drivers
# ---------------------------------------------------------------------------

def key(view, name, modifiers=()):
    """pomadeT3.typeKey plus the + / = / - keys it has no table entry for."""
    from pxr.Usdviewq.qt import QtCore
    import pomadeT3
    extra = {"+": QtCore.Qt.Key.Key_Plus, "=": QtCore.Qt.Key.Key_Equal,
             "-": QtCore.Qt.Key.Key_Minus}
    if name in extra:
        mods = QtCore.Qt.KeyboardModifier.NoModifier
        pomadeT3._qtTest().QTest.keyClick(view, extra[name], mods)
    else:
        pomadeT3.typeKey(view, name, modifiers)
    pomadeT3.wait(10)


def holdKey(view, name, down):
    """Press or release one key WITHOUT its other half (a held J / X)."""
    from pxr.Usdviewq.qt import QtCore
    import pomadeT3
    qkey = getattr(QtCore.Qt.Key, "Key_%s" % name.upper())
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    if down:
        pomadeT3._qtTest().QTest.keyPress(view, qkey, mods)
    else:
        pomadeT3._qtTest().QTest.keyRelease(view, qkey, mods)
    pomadeT3.wait(10)


def keyTo(widget, name, modifiers=()):
    """A key at a dock widget (the one holding focus), not the view."""
    from pxr.Usdviewq.qt import QtCore
    import pomadeT3
    table = dict(pomadeT3._KEY_TABLE)
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    names = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}
    for modifier in modifiers:
        mods |= names[modifier]
    pomadeT3._qtTest().QTest.keyClick(
        widget, getattr(QtCore.Qt.Key, table[name]), mods)
    pomadeT3.wait(10)


def lastMessage(messages, since=0):
    """The newest status text recorded after index `since`, or ''."""
    return messages[-1] if len(messages) > since else ""


def newMessages(messages, since):
    return " | ".join(messages[since:])


def centerCount(session):
    return int(session.dll.Pomade_GetCenterCVCount(session.model))


def tubeCVs(session, tubeId=0):
    import pomadeT3
    return pomadeT3.tubeCenters(session, tubeId)


def handles(session, tubeId=0):
    from usdGenPomadeTools import pomadeBridge
    count = len(tubeCVs(session, tubeId))
    return [pomadeBridge.tubeCenterHandle(session.dll, session.model, tubeId,
                                         cv) for cv in range(count)]


def childrenOf(session, tubeId):
    out = (ctypes.c_int * 64)()
    got = ctypes.c_int(0)
    if session.dll.Pomade_GetTubeChildren(session.model, int(tubeId), out, 64,
                                         ctypes.byref(got)) != 0:
        return []
    return [int(out[i]) for i in range(int(got.value))]


def pixelOf(camera, point):
    projected = camera.worldToPixels(point)
    return (projected[0], projected[1])


def close(a, b, tol=1e-5):
    return a is not None and b is not None and \
        all(abs(float(x) - float(y)) <= tol for x, y in zip(a, b))


def pumpIdle(viewport, session, tries=200):
    import pomadeT3
    for _ in range(tries):
        if not session.hasPendingWork():
            return True
        viewport.pumpOnce()
        pomadeT3.wait(20)
    return not session.hasPendingWork()


# ---------------------------------------------------------------------------
# fixture
# ---------------------------------------------------------------------------

def buildFixture(ctx):
    """A real Graph Draw stroke: one region and its five-CV tube stub."""
    from usdGenPomadeTools import pomadeCamera
    from pomadeT3 import check, pumpUntilCommitted, typeKey
    view, session, state = ctx["view"], ctx["session"], ctx["state"]
    viewport, mouse = ctx["viewport"], ctx["mouse"]
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the controller's camera resolves")
    if camera is None:
        return False
    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)
    check(state.activeMode == "graph",
          "binding opens Graph (%r)" % state.activeMode)
    typeKey(view, "d")
    path = []
    for index, (x0, z0) in enumerate(RECT):
        x1, z1 = RECT[(index + 1) % len(RECT)]
        for step in range(5):
            t = float(step) / 5.0
            path.append(pixelOf(camera, (x0 + (x1 - x0) * t, 0.0,
                                         z0 + (z1 - z0) * t)))
    path.append(pixelOf(camera, (RECT[0][0], 0.0, RECT[0][1])))
    mouse.drag(path)
    check(session.graphCounts() == (4, 4, 1) and
          centerCount(session) == STUB_CVS,
          "a real Graph stroke creates the five-CV root tube (%r, %d CVs)"
          % (session.graphCounts(), centerCount(session)))
    check(pumpUntilCommitted(viewport, session), "the stroke's commit swaps")
    return session.graphCounts() == (4, 4, 1) and \
        centerCount(session) == STUB_CVS


# ---------------------------------------------------------------------------
# 1. modes and sub-modes
# ---------------------------------------------------------------------------

def checkModesAndSubModes(ctx):
    from usdGenPomadeTools import pomadeGizmoSettings, pomadeModes
    from pomadeT3 import check
    view, state, viewport = ctx["view"], ctx["state"], ctx["viewport"]
    workspace = ctx["workspace"]

    instructions = set()
    for digit, modeId in MODE_KEYS:
        key(view, digit)
        button = workspace.button("mode", modeId)
        loopMode = getattr(viewport.loop, "modeId", None)
        check(state.activeMode == modeId,
              "'%s' selects %s (%r)" % (digit, modeId, state.activeMode))
        check(button is not None and button.isChecked(),
              "and the dock's %s shelf button is checked" % modeId)
        if modeId == "output":
            check(loopMode is None,
                  "Output is a panel: no viewport loop (%r)" % (loopMode,))
        else:
            check(loopMode == modeId,
                  "the controller built the %s loop (%r)" % (modeId,
                                                             loopMode))
        text = workspace._instructionLabel.text()
        check(bool(text.strip()),
              "the dock's instruction line is filled for %s" % modeId)
        instructions.add(text)
    check(len(instructions) == len(MODE_KEYS),
          "every mode shows its own instruction line (%d distinct)"
          % len(instructions))

    for digit, modeId in MODE_KEYS[:5]:
        key(view, digit)
        field = SUBMODE_FIELDS[modeId]
        for letter, subId in SUBMODE_KEYS[modeId]:
            key(view, letter)
            kind = "comp" if modeId == "tube" else "sub"
            button = workspace.button(kind, subId)
            check(getattr(state, field) == subId,
                  "%s: '%s' picks %s (%s=%r)" % (modeId, letter, subId,
                                                  field,
                                                  getattr(state, field)))
            check(button is not None and button.isChecked(),
                  "%s: the dock's %s button follows '%s'"
                  % (modeId, subId, letter))
            hint = pomadeModes.hintFor(modeId, subId)
            check(hint and hint in workspace._instructionLabel.text(),
                  "%s: the instruction line reads the %s hint"
                  % (modeId, subId))
        if modeId == "tube":
            key(view, "f9")                      # back to Center

    # Keys stay inside their mode.
    settings = pomadeGizmoSettings.settingsFor(state)
    key(view, "1")
    orientation = state.transformOrientation
    size = settings.manipulatorSize
    tubeSub = state.tubeSubMode
    tool = state.transformTool
    key(view, "l")
    check(state.graphSubMode == "link" and
          state.transformOrientation == orientation,
          "Graph: L is Link, not the Tube orientation (%r, %r)"
          % (state.graphSubMode, state.transformOrientation))
    key(view, "+")
    key(view, "-")
    check(settings.manipulatorSize == size,
          "Graph: + / - leave the manipulator size alone (%g)"
          % settings.manipulatorSize)
    key(view, "f10")
    check(state.tubeSubMode == tubeSub and state.activeMode == "graph",
          "Graph: F10 does not reach Tube's component row (%r)"
          % state.tubeSubMode)
    key(view, "q")
    check(state.transformTool == tool and state.graphSubMode == "link",
          "Graph: Q is not a Graph key and changes nothing (%r)"
          % state.transformTool)
    key(view, "5")
    key(view, "l")
    check(state.sculptSubMode == "lengthen" and
          state.transformOrientation == orientation,
          "Sculpt: L is Lengthen (%r)" % state.sculptSubMode)
    key(view, "1")
    key(view, "d")


# ---------------------------------------------------------------------------
# 2. [ and ] in every mode
# ---------------------------------------------------------------------------

def checkBrackets(ctx):
    from usdGenPomadeTools import pomadeLoopsTube, pomadeSculpt
    from pomadeT3 import check
    view, state, workspace = ctx["view"], ctx["state"], ctx["workspace"]

    def stepCase(digit, label, field, expect, minimum=None):
        key(view, digit)
        workspace.refresh()
        before = float(getattr(state, field))
        if minimum is not None:
            check(before >= minimum,
                  "%s: %s holds a value in its own range after a dock "
                  "refresh (%.4f, minimum %.4f)"
                  % (label, field, before, minimum))
        key(view, "]")
        grown = float(getattr(state, field))
        check(abs(grown - expect(before)) < 1e-4,
              "%s: ] steps %s once (%.4f -> %.4f, want %.4f)"
              % (label, field, before, grown, expect(before)))
        workspace.refresh()
        kept = float(getattr(state, field))
        check(abs(kept - grown) < 1e-4,
              "%s: the stepped %s survives the dock's next refresh "
              "(%.4f -> %.4f)" % (label, field, grown, kept))
        key(view, "[")
        workspace.refresh()
        back = float(getattr(state, field))
        check(abs(back - before) < 1e-3,
              "%s: [ steps it back (%.4f, was %.4f)" % (label, back, before))

    # The Graph row is labelled 'Snap radius (px)' with a 1 px minimum.
    stepCase("1", "Graph", "snapRadiusPx",
             lambda v: max(1.0, min(128.0, v + 1.0)), minimum=1.0)
    stepCase("2", "Tube", "softRadius",
             lambda v: max(0.0, min(1.0, v + pomadeLoopsTube.SOFT_RADIUS_STEP)))
    stepCase("3", "Fill", "previewFraction",
             lambda v: max(0.0, min(1.0, v + 0.05)))
    stepCase("4", "Hierarchy", "pickRadiusPx",
             lambda v: max(1.0, min(128.0, v + 1.0)))
    stepCase("5", "Sculpt", "brushRadiusPx",
             lambda v: pomadeSculpt.steppedBrushRadius(v, 1.0))
    key(view, "6")
    fields = ("snapRadiusPx", "softRadius", "previewFraction",
              "pickRadiusPx", "brushRadiusPx")
    before = [getattr(state, f) for f in fields]
    key(view, "]")
    key(view, "[")
    check([getattr(state, f) for f in fields] == before,
          "Output: [ / ] have no field to move and change nothing")
    key(view, "1")
    # Give the Graph pixel snap back the value the fixture strokes with.
    state.snapRadiusPx = max(state.snapRadiusPx, 2.0)


# ---------------------------------------------------------------------------
# 3. Graph keys
# ---------------------------------------------------------------------------

def checkGraphKeys(ctx):
    from usdGenPomadeTools import pomadeCamera, pomadeLib
    from pomadeT3 import check, frameScalp, info
    view, session, viewport = ctx["view"], ctx["session"], ctx["viewport"]
    mouse, messages, stage = ctx["mouse"], ctx["messages"], ctx["stage"]
    NODE = pomadeLib.POMADE_PICK_GRAPH_NODE

    frameScalp(stage, view)
    view.setFocus()
    key(view, "1")
    key(view, "r")
    camera = pomadeCamera.resolve(view)
    loop = viewport.loop
    counts = session.graphCounts()

    def draft():
        return len(loop.draftRegionPreview()["points"])

    mouse.click(pixelOf(camera, (0.4, 0.0, 0.4)))
    mouse.click(pixelOf(camera, (0.4, 0.0, 3.6)))
    check(draft() == 2, "Create region: two clicks leave a two-CV draft (%d)"
          % draft())
    key(view, "backspace")
    check(draft() == 1 and session.graphCounts() == counts,
          "Backspace drops the last draft CV and edits no graph (%d)"
          % draft())
    mouse.click(pixelOf(camera, (3.6, 0.0, 3.6)))
    since = len(messages)
    # Which widgets the one KeyPress reached the controller through: a
    # declined key propagates up the StageView's parents, and the
    # application-level filter sees every delivery.
    receivers = []
    realOnKey = viewport.onKey

    def countingOnKey(event):
        focus = __import__("pxr.Usdviewq.qt", fromlist=["QtWidgets"]) \
            .QtWidgets.QApplication.focusWidget()
        receivers.append(type(focus).__name__ if focus else None)
        return realOnKey(event)

    viewport.onKey = countingOnKey
    try:
        key(view, "enter")
    finally:
        del viewport.onKey
    info("one Enter press reached ViewportController.onKey %d time(s)"
         % len(receivers))
    check(draft() == 2 and session.graphCounts() == counts,
          "Enter refuses to close a two-CV draft (%d CVs, %r)"
          % (draft(), newMessages(messages, since)))
    refusals = [text for text in messages[since:] if "at least 3" in text]
    check(len(refusals) == 1 and len(receivers) == 1,
          "one Enter press is handled and reported once, not once per "
          "widget the declined key propagates through (%d lines, %d "
          "onKey calls)" % (len(refusals), len(receivers)))
    key(view, "escape")
    check(draft() == 0 and session.graphCounts() == counts,
          "Escape discards the draft and nothing else (%d CVs, %r)"
          % (draft(), session.graphCounts()))
    check(not viewport._regionOverlay.isVisible(),
          "and the draft overlay goes away")

    # Ctrl+A / Ctrl+I / Ctrl+Shift+A over the graph nodes.
    session.clearSelection(0)
    key(view, "a", ("ctrl",))
    nodes = session.readSelection(NODE)
    check(len(nodes) == 4, "Graph Ctrl+A selects the four nodes (%r)"
          % (nodes,))
    key(view, "i", ("ctrl",))
    check(session.selectionCount(NODE) == 0,
          "Ctrl+I inverts that to none (%d)" % session.selectionCount(NODE))
    key(view, "i", ("ctrl",))
    check(session.selectionCount(NODE) == 4,
          "and Ctrl+I again back to all four (%d)"
          % session.selectionCount(NODE))

    # Shift+W / Shift+U say why they refuse, and change nothing.
    depth = session.undoDepth()
    since = len(messages)
    key(view, "w", ("shift",))
    check(session.graphCounts() == counts and
          "two nodes" in newMessages(messages, since),
          "Shift+W with four nodes selected refuses and says why (%r)"
          % newMessages(messages, since))
    since = len(messages)
    key(view, "u", ("shift",))
    check(session.graphCounts() == counts and
          "one node" in newMessages(messages, since),
          "Shift+U with four nodes selected refuses and says why (%r)"
          % newMessages(messages, since))
    check(session.undoDepth() == depth,
          "the refusals push no undo step (%d -> %d)"
          % (depth, session.undoDepth()))
    key(view, "a", ("ctrl", "shift"))
    check(session.selectionCount(0) == 0,
          "Ctrl+Shift+A clears the node selection (%d)"
          % session.selectionCount(0))

    # One unshared node: Shift+U has nothing to split, so no undo step.
    key(view, "p")                       # Place: a Shift-click is a select
    mouse.click(pixelOf(camera, (1.0, 0.0, 1.0)), ("shift",))
    one = session.readSelection(NODE)
    check(len(one) == 1, "a Shift-click selects one graph node (%r)"
          % (one,))
    if len(one) == 1:
        depth = session.undoDepth()
        since = len(messages)
        key(view, "u", ("shift",))
        info("Shift+U on an unshared node said %r"
             % newMessages(messages, since))
        check(session.graphCounts() == counts,
              "Shift+U on an unshared node leaves the graph alone (%r)"
              % (session.graphCounts(),))
        check(session.undoDepth() == depth,
              "SS-02: and pushes no empty undo step (%d -> %d, label %r)"
              % (depth, session.undoDepth(),
                 session.status().get("undoLabel")))
    key(view, "a", ("ctrl", "shift"))
    key(view, "d")


# ---------------------------------------------------------------------------
# 4. Tube tools, orientation, size, pivot
# ---------------------------------------------------------------------------

def _tubeCamera(ctx):
    from usdGenPomadeTools import pomadeCamera
    from pomadeT3 import obliqueCamera, wait
    obliqueCamera(ctx["stage"], ctx["view"])
    ctx["view"].setFocus()
    wait(30)
    return pomadeCamera.resolve(ctx["view"])


def _selectTip(ctx, camera):
    """A real click on the tip centre CV with the Select tool (no gizmo)."""
    from usdGenPomadeTools import pomadeLib
    view, session, state = ctx["view"], ctx["session"], ctx["state"]
    tool = state.transformTool
    key(view, "q")
    key(view, "a", ("ctrl", "shift"))
    tip = len(handles(session)) - 1
    ctx["mouse"].click(pixelOf(camera, handles(session)[tip]))
    toolKey = {"select": "q", "move": "w", "rotate": "e", "scale": "r"}
    key(view, toolKey.get(tool, "w"))
    return session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) == \
        [(0, tip, -1)]


def checkTubeTools(ctx):
    from usdGenPomadeTools import pomadeGizmoSettings, pomadeLib
    from pomadeT3 import check
    view, session, state = ctx["view"], ctx["session"], ctx["state"]
    viewport, workspace = ctx["viewport"], ctx["workspace"]
    messages = ctx["messages"]
    CENTER = pomadeLib.POMADE_PICK_CENTER_CV

    key(view, "2")
    key(view, "f9")
    camera = _tubeCamera(ctx)
    settings = pomadeGizmoSettings.settingsFor(state)

    for letter, tool, orient in (("q", "select", None),
                                 ("w", "move", "world"),
                                 ("e", "rotate", "tube"),
                                 ("r", "scale", "world")):
        since = len(messages)
        key(view, letter)
        button = workspace.button("tool", tool)
        check(state.transformTool == tool and state.tubeSubMode == "center",
              "Tube: '%s' picks %s and keeps Center (%r, %r)"
              % (letter, tool, state.transformTool, state.tubeSubMode))
        check(button is not None and button.isChecked(),
              "the dock's %s tool button follows '%s'" % (tool, letter))
        check(tool.title() in newMessages(messages, since),
              "the status line names the %s tool (%r)"
              % (tool, newMessages(messages, since)))
        if orient is not None:
            check(state.transformOrientation == orient,
                  "G16: %s starts in its own orientation (%r, want %r)"
                  % (tool, state.transformOrientation, orient))

    key(view, "w")
    check(_selectTip(ctx, camera), "a click selects the tip centre CV (%r)"
          % (session.readSelection(CENTER),))
    gizmo = viewport.loop._gizmo
    check(gizmo.visible and viewport._gizmoOverlay.isVisible(),
          "the Move gizmo is on screen for the selected CV")

    # +/=/- resize the manipulator by 10 %.
    size = settings.manipulatorSize
    since = len(messages)
    key(view, "+")
    check(abs(settings.manipulatorSize - size * 1.1) < 1e-3 and
          "manipulator size" in newMessages(messages, since),
          "G13: '+' grows the manipulator 10%% (%g -> %g, %r)"
          % (size, settings.manipulatorSize, newMessages(messages, since)))
    key(view, "-")
    check(abs(settings.manipulatorSize - size) < 1e-3,
          "'-' shrinks it back (%g)" % settings.manipulatorSize)
    key(view, "=")
    check(abs(settings.manipulatorSize - size * 1.1) < 1e-3,
          "'=' grows it like '+' (%g)" % settings.manipulatorSize)
    key(view, "-")
    check(abs(settings.manipulatorSize - size) < 1e-3,
          "and '-' again restores %g (%g)" % (size, settings.manipulatorSize))

    # L flips World <-> Tube, and the dock's Global/Local toggle follows.
    orientButton = workspace.button("gizmo", "orientation")
    key(view, "l")
    workspace.refresh()
    check(state.transformOrientation == "tube" and
          orientButton is not None and orientButton.isChecked(),
          "L flips Move to the Tube frame and checks the dock toggle "
          "(%r, %r)" % (state.transformOrientation,
                        orientButton and orientButton.isChecked()))
    key(view, "l")
    workspace.refresh()
    check(state.transformOrientation == "world" and
          not orientButton.isChecked(),
          "and L flips back to World (%r)" % state.transformOrientation)

    # P: Move has no group pivot; Rotate cycles Individual <-> Centre.
    pivotButton = workspace.button("gizmo", "groupPivot")
    since = len(messages)
    key(view, "p")
    workspace.refresh()
    check("Rotate and Scale" in newMessages(messages, since) and
          pivotButton is not None and not pivotButton.isEnabled(),
          "G17: P under Move says the pivot is Rotate/Scale's; the dock "
          "toggle is greyed (%r)" % newMessages(messages, since))
    key(view, "a", ("ctrl",))
    count = session.selectionCount(CENTER)
    check(count == len(handles(session)),
          "Ctrl+A selects every centre CV (%d)" % count)
    key(view, "e")
    # The gizmo sits on the section-area centroid handles, not on the raw
    # centre CVs (pomade-tool.md: "Centered section-core handles").
    cvs = handles(session)
    root = cvs[0]
    loop = viewport.loop
    individual = tuple(loop._gizmo.origin)
    lo = [min(p[i] for p in cvs) for i in range(3)]
    hi = [max(p[i] for p in cvs) for i in range(3)]
    middle = tuple(0.5 * (lo[i] + hi[i]) for i in range(3))
    check(loop.groupPivot() == pomadeGizmoSettings.GROUP_PIVOT_INDIVIDUAL and
          close(individual, root, 0.05),
          "Rotate starts on Individual Origins, drawn at the root (%r vs %r)"
          % (individual, root))
    since = len(messages)
    key(view, "p")
    workspace.refresh()
    centre = tuple(loop._gizmo.origin)
    check(loop.groupPivot() == pomadeGizmoSettings.GROUP_PIVOT_CENTRE and
          close(centre, middle, 0.05) and pivotButton.isChecked() and
          "Selection Centre" in newMessages(messages, since),
          "P switches Rotate to Selection Centre: the gizmo moves to the "
          "selection's middle and the dock toggle checks (%r vs %r, %r)"
          % (centre, middle, newMessages(messages, since)))
    key(view, "p")
    workspace.refresh()
    check(loop.groupPivot() == pomadeGizmoSettings.GROUP_PIVOT_INDIVIDUAL and
          close(tuple(loop._gizmo.origin), root, 0.05) and
          not pivotButton.isChecked(),
          "and P again returns to Individual Origins")
    key(view, "w")
    check(state.transformOrientation == "world",
          "back on Move the orientation is Move's own World (%r)"
          % state.transformOrientation)


# ---------------------------------------------------------------------------
# 5. Tube selection keys and Escape
# ---------------------------------------------------------------------------

def checkTubeSelectionAndEscape(ctx):
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    from usdGenPomadeTools import pomadeLib
    from pomadeT3 import check, wait
    view, session, state = ctx["view"], ctx["session"], ctx["state"]
    viewport, workspace = ctx["viewport"], ctx["workspace"]
    mouse = ctx["mouse"]
    CENTER = pomadeLib.POMADE_PICK_CENTER_CV

    key(view, "2")
    key(view, "f9")
    key(view, "w")
    camera = _tubeCamera(ctx)
    total = len(handles(session))

    key(view, "a", ("ctrl",))
    check(session.selectionCount(CENTER) == total,
          "Tube Ctrl+A selects all %d centre CVs (%d)"
          % (total, session.selectionCount(CENTER)))
    key(view, "a", ("ctrl", "shift"))
    check(session.selectionCount(0) == 0,
          "Ctrl+Shift+A selects nothing (%d)" % session.selectionCount(0))
    check(_selectTip(ctx, camera), "a click selects the tip CV again")
    key(view, "i", ("ctrl",))
    inverted = sorted(item[1] for item in session.readSelection(CENTER))
    check(inverted == list(range(total - 1)),
          "Ctrl+I inverts the tip into every other centre CV (%r)"
          % (inverted,))

    # Idle Escape is cancel-only: the selection stays (SL-03).
    check(_selectTip(ctx, camera), "the tip CV is selected for Escape")
    selected = session.readSelection(CENTER)
    key(view, "escape")
    check(session.readSelection(CENTER) == selected,
          "Escape with nothing live keeps the selection (%r)"
          % (session.readSelection(CENTER),))

    # Escape mid-drag restores the press-time shape, keeps the selection
    # and leaves no undo step; the release after it does nothing.
    tip = total - 1
    before = tubeCVs(session)[tip]
    depth = session.undoDepth()
    start = pixelOf(camera, handles(session)[tip])
    mouse.press(start)
    check(viewport.gestureActive, "a press on the tip's gizmo starts a drag")
    for step in range(1, 7):
        mouse.move((start[0] + 10.0 * step, start[1] - 4.0 * step))
    moved = tubeCVs(session)[tip]
    check(not close(moved, before, 1e-4),
          "the drag moves the tip live (%r -> %r)" % (before, moved))
    key(view, "escape")
    check(not viewport.gestureActive and close(tubeCVs(session)[tip],
                                                before, 1e-6),
          "Escape mid-drag cancels it: the tip is back exactly (%r)"
          % (tubeCVs(session)[tip],))
    check(session.readSelection(CENTER) == selected,
          "and the selection survives the cancel (%r)"
          % (session.readSelection(CENTER),))
    mouse.move((start[0] + 80.0, start[1] - 30.0))
    mouse.release((start[0] + 80.0, start[1] - 30.0))
    check(close(tubeCVs(session)[tip], before, 1e-6) and
          session.undoDepth() == depth,
          "the release after Escape edits nothing and adds no undo step "
          "(depth %d -> %d)" % (depth, session.undoDepth()))

    # A focused dock spin box owns Escape (and every other key).
    spins = [spin for spin in
             workspace.findChildren(QtWidgets.QAbstractSpinBox)
             if spin.isVisible() and spin.isEnabled()]
    check(bool(spins), "the Tube page shows a spin box to type in")
    if spins:
        spin = spins[0]
        qtTest = __import__("pomadeT3")._qtTest()
        qtTest.QTest.mouseClick(spin, QtCore.Qt.MouseButton.LeftButton,
                                QtCore.Qt.KeyboardModifier.NoModifier,
                                QtCore.QPoint(8, spin.height() // 2))
        wait(20)
        focus = QtWidgets.QApplication.focusWidget()
        check(focus is spin or (focus is not None and
                                spin.isAncestorOf(focus)),
              "a click puts the keyboard in the spin box (%r)"
              % (type(focus).__name__ if focus else None,))
        keyTo(focus or spin, "escape")
        check(session.readSelection(CENTER) == selected,
              "Escape in a focused dock field leaves the selection alone "
              "(%r)" % (session.readSelection(CENTER),))
        # usdview's AppEventFilter hands focus to the widget under the
        # mouse on every Escape; click the field again before typing.
        qtTest.QTest.mouseClick(spin, QtCore.Qt.MouseButton.LeftButton,
                                QtCore.Qt.KeyboardModifier.NoModifier,
                                QtCore.QPoint(8, spin.height() // 2))
        wait(20)
        focus = QtWidgets.QApplication.focusWidget()
        mode = state.activeMode
        keyTo(focus or spin, "4")
        check(state.activeMode == mode and
              session.readSelection(CENTER) == selected,
              "typing 4 into a focused dock field stays the field's "
              "(mode %r)" % state.activeMode)
        keyTo(focus or spin, "escape")
        mouse.click((start[0] + 200.0, start[1] + 200.0))
        wait(20)
        check(view.hasFocus(), "a viewport click takes the keys back")
        # That click on empty space is a Pomade deselect-click; reselect.
        _selectTip(ctx, camera)


# ---------------------------------------------------------------------------
# 6. J / X holds and undo during a drag
# ---------------------------------------------------------------------------

def checkTubeHolds(ctx):
    from usdGenPomadeTools import (pomadeCamera, pomadeGizmoSettings, pomadeLib,
                                  pomadePanels)
    from pomadeT3 import check, info, sideCamera, wait
    view, session, state = ctx["view"], ctx["session"], ctx["state"]
    viewport, messages, mouse = ctx["viewport"], ctx["messages"], \
        ctx["mouse"]

    key(view, "2")
    key(view, "f9")
    key(view, "w")
    # Oblique enough that a screen drag has x, y and z world components.
    sideCamera(ctx["stage"], view, eye=(9.0, 8.0, 10.0),
               target=(2.0, 3.0, 2.0), primPath="/PomadeT3HoldCamera")
    view.setFocus()
    wait(40)
    camera = pomadeCamera.resolve(view)
    check(_selectTip(ctx, camera), "the tip CV is selected for the holds")
    settings = pomadeGizmoSettings.settingsFor(state)
    rows = {row.id: row for row in pomadePanels.descriptors("tube", state)}
    if "stepSize" in rows:
        rows["stepSize"].set(state, ctx["session"], 0.5)
    else:
        settings.For("move").stepSize = 0.5
    check(abs(settings.For("move").stepSize - 0.5) < 1e-9,
          "the Move tool's Step size row takes 0.5")
    loop = viewport.loop
    tip = len(handles(session)) - 1
    home = tubeCVs(session)[tip]
    depth = session.undoDepth()

    start = pixelOf(camera, handles(session)[tip])
    viewProj = tuple(camera.viewProj)
    mouse.press(start)
    for step in range(1, 5):
        mouse.move((start[0] + 11.0 * step, start[1] + 3.0 * step))
    raw = [tubeCVs(session)[tip][i] - home[i] for i in range(3)]
    check(any(abs(v) > 1e-3 for v in raw), "the drag moved the tip (%r)"
          % ([round(v, 3) for v in raw],))

    # Undo / redo refuse while the drag is live (G14).
    since = len(messages)
    key(view, "z", ("ctrl",))
    key(view, "z", ("shift",))
    check(viewport.gestureActive and
          close([tubeCVs(session)[tip][i] - home[i] for i in range(3)], raw,
                1e-6) and "finish the drag" in newMessages(messages, since),
          "Ctrl+Z and Shift+Z mid-drag are refused and say so (%r)"
          % newMessages(messages, since))
    refusals = [text for text in messages[since:]
                if "finish the drag" in text]
    check(len(refusals) == 2,
          "two refused key presses report two lines, not one per widget "
          "the key propagates through (%d lines)" % len(refusals))

    holdKey(view, "j", True)
    stepped = [tubeCVs(session)[tip][i] - home[i] for i in range(3)]
    check(viewport.holdActive("stepSnap") and
          all(abs(v / 0.5 - round(v / 0.5)) < 1e-3 for v in stepped) and
          not close(stepped, raw, 1e-4),
          "holding J snaps the live drag to 0.5 steps at once (%r -> %r)"
          % ([round(v, 3) for v in raw], [round(v, 3) for v in stepped]))
    check(tuple(pomadeCamera.resolve(view).viewProj) == viewProj,
          "and usdview's own J (Toggle Framed View) did not fire")
    readout = viewport.gizmoReadout() if hasattr(viewport,
                                                 "gizmoReadout") else None
    info("drag readout with J held: %r" % (readout,))
    holdKey(view, "j", False)
    unsnapped = [tubeCVs(session)[tip][i] - home[i] for i in range(3)]
    check(not viewport.holdActive("stepSnap") and
          close(unsnapped, raw, 1e-4),
          "releasing J un-snaps at once (%r)"
          % ([round(v, 3) for v in unsnapped],))
    holdKey(view, "x", True)
    check(viewport.holdActive("grid") and
          all(abs(v - round(v)) < 1e-3 for v in loop._gizmo.origin),
          "holding X lands the pivot on the world grid (%r)"
          % (tuple(loop._gizmo.origin),))
    end = (start[0] + 44.0, start[1] + 12.0)
    mouse.release(end)
    check(not viewport.gestureActive and not viewport.holdActive("grid"),
          "the drag's release clears the hold")
    holdKey(view, "x", False)
    check(session.undoDepth() == depth + 1,
          "the snapped drag is exactly one undo step (%d -> %d)"
          % (depth, session.undoDepth()))
    key(view, "z", ("ctrl",))
    check(close(tubeCVs(session)[tip], home, 1e-5),
          "Ctrl+Z puts the tip back (%r)" % (tubeCVs(session)[tip],))
    settings.For("move").stepSize = 1.0

    # With no drag live J / X are usdview's, never a Pomade hold.
    viewBefore = tuple(pomadeCamera.resolve(view).viewProj)
    key(view, "j")
    key(view, "x")
    check(not viewport.holdActive("stepSnap") and
          not viewport.holdActive("grid") and
          close(tubeCVs(session)[tip], home, 1e-5),
          "J and X with no drag are not claimed as holds and edit nothing")
    info("idle J reached usdview (camera changed: %s)"
         % (tuple(pomadeCamera.resolve(view).viewProj) != viewBefore))


# ---------------------------------------------------------------------------
# 7. Delete, its gate, and the three redo keys
# ---------------------------------------------------------------------------

def checkTubeDeleteAndRedo(ctx):
    from usdGenPomadeTools import pomadeLib
    from pomadeT3 import check
    view, session, viewport = ctx["view"], ctx["session"], ctx["viewport"]
    workspace, messages = ctx["workspace"], ctx["messages"]
    CENTER = pomadeLib.POMADE_PICK_CENTER_CV

    key(view, "2")
    key(view, "f9")
    camera = _tubeCamera(ctx)
    check(_selectTip(ctx, camera), "the tip CV is selected for Delete")
    count = centerCount(session)

    # Off the viewport (pointer over the dock, a dock button focused)
    # Delete belongs to the dock, never to the Pomade selection.
    button = workspace.button("mode", "tube")
    viewport.setPointerInside(False)
    button.setFocus()
    keyTo(button, "delete")
    check(centerCount(session) == count and
          session.selectionCount(CENTER) == 1,
          "SL-03: Delete off the viewport deletes nothing (%d CVs)"
          % centerCount(session))
    viewport.setPointerInside(True)
    view.setFocus()

    depth = session.undoDepth()
    since = len(messages)
    key(view, "delete")
    check(centerCount(session) == count - 1 and
          session.undoDepth() == depth + 1,
          "Delete removes the selected tip CV as one undo step (%d -> %d, "
          "%r)" % (count, centerCount(session),
                   newMessages(messages, since)))
    key(view, "z", ("ctrl",))
    check(centerCount(session) == count, "Ctrl+Z restores it (%d)"
          % centerCount(session))
    for mods, name in ((("ctrl",), "Ctrl+Y"),
                       (("ctrl", "shift"), "Ctrl+Shift+Z"),
                       (("shift",), "Shift+Z")):
        letter = "y" if name == "Ctrl+Y" else "z"
        key(view, letter, mods)
        check(centerCount(session) == count - 1,
              "%s redoes the delete (%d CVs)" % (name, centerCount(session)))
        key(view, "z", ("ctrl",))
        check(centerCount(session) == count,
              "and Ctrl+Z undoes it again (%d CVs)" % centerCount(session))
    workspace.refresh()
    redo = workspace.button("file", "redo")
    check(redo is not None and redo.isEnabled(),
          "the dock's Redo is live with a step to redo")


# ---------------------------------------------------------------------------
# 8. Hierarchy keys
# ---------------------------------------------------------------------------

def checkHierarchyKeys(ctx):
    from usdGenPomadeTools import pomadeHierarchy, pomadeLib
    from pomadeT3 import check, frameScalp, wait
    view, session, state = ctx["view"], ctx["session"], ctx["state"]
    viewport, messages, stage = ctx["viewport"], ctx["messages"], \
        ctx["stage"]
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT

    def focus():
        return int(session.dll.Pomade_GetFocusLevel(session.model))

    def selectedTubes():
        return sorted(item[0] for item in session.readSelection(TUBE))

    frameScalp(stage, view)
    view.setFocus()
    wait(30)
    # Fill takes Ctrl+A / Ctrl+I over tubes; Sculpt selects nothing, so
    # the keys are not its and change nothing there.
    key(view, "3")
    key(view, "a", ("ctrl", "shift"))
    key(view, "a", ("ctrl",))
    check(selectedTubes() == [0], "Fill Ctrl+A selects the tube (%r)"
          % (selectedTubes(),))
    key(view, "i", ("ctrl",))
    check(selectedTubes() == [], "Fill Ctrl+I inverts it away (%r)"
          % (selectedTubes(),))
    key(view, "5")
    key(view, "a", ("ctrl",))
    check(session.selectionCount(0) == 0,
          "Sculpt Ctrl+A selects nothing (%d)" % session.selectionCount(0))
    key(view, "4")
    key(view, "n")
    key(view, "a", ("ctrl",))
    check(selectedTubes() == [0], "Hierarchy Ctrl+A selects the tube (%r)"
          % (selectedTubes(),))
    state.subdivideCount = 4
    since = len(messages)
    key(view, "d", ("shift",))
    kids = childrenOf(session, 0)
    check(len(kids) == 4 and focus() == 2 and state.activeLevel == 2,
          "Shift+D subdivides into four children and focuses L2 (%r, L%d, "
          "%r)" % (kids, focus(), newMessages(messages, since)))
    if len(kids) != 4:
        return
    key(view, "up", ("ctrl",))
    check(focus() == 1 and selectedTubes() == [0],
          "Ctrl+Up exits to L1 with the parent selected (L%d, %r)"
          % (focus(), selectedTubes()))
    key(view, "down", ("ctrl",))
    check(focus() == 2 and selectedTubes() == sorted(kids),
          "Ctrl+Down enters L2 with the children selected (L%d, %r)"
          % (focus(), selectedTubes()))
    key(view, "backspace")
    visible = pomadeHierarchy.isTubeVisible(session.dll, session.model, 0)
    check(focus() == 1 and visible is True and
          not any(pomadeHierarchy.isTubeVisible(session.dll, session.model,
                                               kid) for kid in kids),
          "Backspace exits the level: L%d, parent visible, children "
          "hidden" % focus())
    key(view, "m", ("shift",))
    check(not childrenOf(session, 0),
          "Shift+M merges the children away (%r)" % (childrenOf(session, 0),))
    key(view, "z", ("ctrl",))
    check(len(childrenOf(session, 0)) == 4,
          "Ctrl+Z brings the four children back (%r)"
          % (childrenOf(session, 0),))
    key(view, "y", ("ctrl",))
    check(not childrenOf(session, 0),
          "Ctrl+Y merges them again (%r)" % (childrenOf(session, 0),))

    # W / E / R in Hierarchy jump to Tube with the selected tube kept.
    session.select(TUBE, [0])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    since = len(messages)
    key(view, "e")
    check(state.activeMode == "tube" and state.transformTool == "rotate" and
          state.tubeSubMode == "tube" and selectedTubes() == [0],
          "Hierarchy E jumps to Tube / Whole tube / Rotate with the tube "
          "kept (%r, %r, %r, %r)" % (state.activeMode, state.transformTool,
                                     state.tubeSubMode, selectedTubes()))
    check("from Hierarchy" in newMessages(messages, since),
          "and the status line says where it came from (%r)"
          % newMessages(messages, since))
    # Q too: the transform row's Select button reads "Select (Q)" in
    # Hierarchy, so the key must make the same jump the button does.
    key(view, "4")
    session.select(TUBE, [0])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    check(state.activeMode == "hierarchy", "back in Hierarchy (%r)"
          % state.activeMode)
    key(view, "q")
    check(state.activeMode == "tube" and state.transformTool == "select" and
          state.tubeSubMode == "tube" and selectedTubes() == [0],
          "Hierarchy Q jumps to Tube / Whole tube / Select with the tube "
          "kept (%r, %r, %r, %r)" % (state.activeMode, state.transformTool,
                                     state.tubeSubMode, selectedTubes()))
    # Tube mode's Shift+M is the controller fallback. Over the (now leaf)
    # tube it merges nothing, so it must not spend an undo step.
    undoDepth = int(session.dll.Pomade_GetUndoDepth(session.model))
    since = len(messages)
    key(view, "m", ("shift",))
    check(int(session.dll.Pomade_GetUndoDepth(session.model)) == undoDepth
          and not session.gestureActive,
          "Tube-mode Shift+M over a leaf leaves no undo step (%d -> %d)"
          % (undoDepth, int(session.dll.Pomade_GetUndoDepth(session.model))))
    check("nothing merged" in newMessages(messages, since),
          "and says nothing merged (%r)" % newMessages(messages, since))
    key(view, "w")
    key(view, "f9")


# ---------------------------------------------------------------------------
# 9. Ctrl+Shift+S
# ---------------------------------------------------------------------------

def checkSaveKey(ctx):
    from pomadeT3 import check
    view, container, api = ctx["view"], ctx["container"], ctx["api"]
    calls = []

    def fakeSave(usdviewApi, parent=None, *args, **kwargs):
        calls.append(usdviewApi)
        return None

    original = vars(container).get("saveGroomInteractive")
    container.saveGroomInteractive = fakeSave
    try:
        key(view, "1")
        key(view, "s", ("ctrl", "shift"))
    finally:
        if original is not None:
            container.saveGroomInteractive = original
        else:
            del container.saveGroomInteractive
    check(len(calls) == 1 and calls[0] is api,
          "Ctrl+Shift+S runs the container's one save path once (%d)"
          % len(calls))
    mode = ctx["state"].activeMode
    check(mode == "graph", "and changes no mode (%r)" % mode)


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the hotkey sweep needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeHotkeys needs testusdview (no live view)")
    sys.exit(0)
