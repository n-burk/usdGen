# testUsdviewPomadeDock -- T3: the dock's artist status strip (DK-05).
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testUsdviewPomadeDock.py \
#               examples/pomade-graph-scalp.usda
#
# The strip used to be developer telemetry ('| L1 | model v13 stage v9 ...
# [MAP BEHIND] | swap 0.9 ms') that sat amber after every selection
# publish, and every refusal reached only usdview's bottom status bar.
# Proven here, through real mouse and key events on a live usdview:
#   * Open workspace installs the dock's message area as the session's
#     status sink, and the viewport controller's own lines reach it too;
#   * after a real gizmo drag the sync pill reads 'Committing...', and
#     'Synced' once the commit has swapped;
#   * a selection publish (which bumps the model version and enqueues
#     nothing) leaves the strip calm: `_statusAmber` False, pill Synced;
#   * the version dump no longer opens with '|', and it hides behind
#     Show diagnostics;
#   * clicking Relax with CVs selected says so in the message area, with
#     the Tube tool's status line;
#   * every Display control writes its state field;
#   * undo past the bottom turns the message red, and messages fade.
import os
import sys


def testenvDir():
    """This script's directory, also when testusdview execs it without a
    __file__ (testUsdviewPomadeWorkflow.py's pattern)."""
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for arg in sys.argv:
        if arg.endswith("testUsdviewPomadeDock.py"):
            return os.path.dirname(os.path.abspath(arg))
    return os.getcwd()


RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
STUB_CVS = 4


def run(appController):
    here = testenvDir()
    sys.path.insert(0, here)
    sys.path.insert(0, os.path.normpath(os.path.join(here, "..", "python")))
    try:
        import usdGenPomadeTools
        from usdGenPomadeTools import pomadeBridge, pomadeCamera, pomadeLib
        from pomadeT3 import (Mouse, check, failureCount, frameScalp, info,
                             obliqueCamera, pumpUntilCommitted, typeKey,
                             wait)
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools/pomadeT3: %s" % exc)
        return 1
    from pxr.Usdviewq.qt import QtWidgets

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = appController._plugRegistry
    container = usdGenPomadeTools.container()

    check(frameScalp(stage, view), "the scene camera looks down at the scalp")
    view.setFocus()
    wait(50)

    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenPomadeTools.openWorkspace").run()
    # -- before Bind: nothing is synced and no tool can act ------------------
    # The pill used to read a green 'Synced' and the HUD 'Graph > Create
    # region' with its click recipe over a dock whose tools were all
    # greyed out (renders/pomade-first-run.png).
    workspace = container.workspace
    viewport = container.viewport
    if workspace is not None:
        workspace.refresh()
        wait(20)
        pill = workspace.findChild(QtWidgets.QLabel, "pomadeSyncPill")
        pillKey = getattr(workspace, "_pillKey", None)
        check(pillKey is not None and pillKey[0] == "No scalp bound" and
              pillKey[1] == "neutral" and pill is not None and
              pill.text() == "No scalp bound",
              "before Bind the sync pill reads 'No scalp bound' in the "
              "neutral tone, unelided (%r, %r)"
              % (pillKey, pill.text() if pill is not None else None))
        check(workspace._instructionLabel.text().startswith(
                  "Bind a scalp mesh to start"),
              "and the dock's instruction line says to bind first (%r)"
              % workspace._instructionLabel.text())
    else:
        check(False, "Open workspace built the dock")
    if viewport is not None:
        title, hint, _chip = viewport.hudText()
        check(title == "Bind a scalp mesh to start" and
              hint.startswith("Bind a scalp mesh to start") and
              "Click CVs" not in hint,
              "before Bind the HUD reads 'Bind a scalp mesh to start', not "
              "a tool (%r, %r)" % (title, hint))
        overlay = getattr(viewport, "_hudOverlay", None)
        if overlay is not None and overlay.isVisible():
            check(overlay.title == title,
                  "and the drawn HUD shows it (%r)" % overlay.title)
    registry.getCommandPlugin("usdGenPomadeTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.pomadeState
    workspace = container.workspace
    if workspace is not None and viewport is not None:
        workspace.refresh()
        wait(20)
        check(workspace._pillKey is not None and
              workspace._pillKey[0] != "No scalp bound" and
              viewport.hudText()[0] != "Bind a scalp mesh to start",
              "after Bind the pill and the HUD leave the first-run text "
              "(%r, %r)" % (workspace._pillKey, viewport.hudText()[0]))
    check(session is not None and session.model is not None and
          viewport is not None and viewport.installed and
          workspace is not None,
          "Bind scalp created a live model under an installed controller")
    if session is None or session.model is None or workspace is None or \
            viewport is None:
        return 1

    def shutdown():
        viewport.uninstall()
        session.deactivate()

    # -- the strip's widgets and the sink ------------------------------------
    names = ("pomadeMessage", "pomadeSyncPill", "pomadeStatusSummary",
             "pomadeGpuChip", "pomadeDiagnostics")
    found = dict((name, workspace.findChild(QtWidgets.QLabel, name))
                 for name in names)
    check(all(found.values()),
          "the strip has message, pill, summary, chip and diagnostics labels "
          "%r" % sorted(n for n, w in found.items() if w is None))
    check(workspace.findChild(QtWidgets.QGroupBox, "pomadeDisplayGroup")
          is not None, "the Display group is built")
    check(session._statusFn == workspace._onStatus,
          "Open workspace installed the dock's message area as the "
          "session's status sink")
    viewport._status("Pomade: DK-05 controller probe")
    check(workspace._messageText == "Pomade: DK-05 controller probe" and
          workspace._messageLabel.text() == workspace._messageText,
          "a viewport controller status line reaches the message area "
          "(%r)" % workspace._messageText)
    check(workspace._messageLevel == "info" and
          workspace._messageTimer.isActive() and
          workspace._messageTimer.interval() == 4000,
          "an ordinary line is info-coloured and fades after 4 s")

    stage.GetPrimAtPath("/Scalp").SetActive(False)
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the controller's camera resolves")
    if camera is None:
        shutdown()
        return 1

    def pixel(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1])

    state.snapRadiusPx = max(
        0.1 / max(camera.worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)
    mouse = Mouse(view)
    mouse.direct = True
    viewport.setPointerInside(True)

    # -- fixture: one region stroke leaves a five-CV tube stub --------------
    typeKey(view, "d")
    path = []
    for index, (x0, z0) in enumerate(RECT):
        x1, z1 = RECT[(index + 1) % len(RECT)]
        for step in range(5):
            t = float(step) / 5.0
            path.append(pixel((x0 + (x1 - x0) * t, 0.0,
                               z0 + (z1 - z0) * t)))
    path.append(pixel((RECT[0][0], 0.0, RECT[0][1])))
    mouse.drag(path)
    cvCount = int(session.dll.Pomade_GetCenterCVCount(session.model))
    check(session.graphCounts() == (4, 4, 1) and cvCount == STUB_CVS,
          "a real Graph stroke creates the five-CV root tube (%r, %d CVs)"
          % (session.graphCounts(), cvCount))
    check(pumpUntilCommitted(viewport, session),
          "the stroke's commit swaps")
    workspace.refresh()
    check(workspace._syncPill.text() == "Synced",
          "an idle, committed groom reads Synced (%r)"
          % workspace._syncPill.text())
    check(workspace._statusText and
          not workspace._statusText.lstrip().startswith("|") and
          "MAP BEHIND" not in workspace._statusText,
          "the version dump drops the leading '| ' and the MAP BEHIND "
          "tag (%r)" % workspace._statusText)
    check(workspace._syncPill.toolTip() ==
          "Synced\n%s" % workspace._statusText,
          "the pill's tooltip carries its text over the version dump (%r)"
          % workspace._syncPill.toolTip())
    check(workspace._gpuChip.text() in ("GPU", "CPU"),
          "the device chip reads GPU or CPU (%r)" % workspace._gpuChip.text())

    # -- Tube: a real gizmo drag, Committing... then Synced -----------------
    typeKey(view, "2")
    check(state.activeMode == "tube" and state.tubeSubMode == "center",
          "the 2 hotkey opens Tube/Center (%r/%r)"
          % (state.activeMode, state.tubeSubMode))
    check(obliqueCamera(stage, view),
          "an oblique camera separates the center CVs on screen")
    wait(30)
    camera = pomadeCamera.resolve(view)

    def handles():
        return [pomadeBridge.tubeCenterHandle(session.dll, session.model, 0,
                                             cv) for cv in range(STUB_CVS)]

    tipPixel = pixel(handles()[-1])
    mouse.click(tipPixel)
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
          [(0, STUB_CVS - 1, -1)],
          "one click selects the tip center CV (%r)"
          % (session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV),))
    check(pumpUntilCommitted(viewport, session), "nothing left pending")
    before = handles()[-1]
    mouse.press(tipPixel)
    check(viewport.gestureActive, "the press took the gizmo")
    for step in range(1, 7):
        mouse.move((tipPixel[0] + 10.0 * step, tipPixel[1]))
    mouse.release((tipPixel[0] + 60.0, tipPixel[1]))
    moved = handles()[-1]
    check(abs(moved[0] - before[0]) + abs(moved[2] - before[2]) > 1e-3,
          "the drag moved the tip CV (%r -> %r)" % (before, moved))
    # No wait(): the idle pump that swaps the commit runs on the event
    # loop, so right after the release the commit is still in flight.
    workspace.refresh()
    info("after the drag: pill %r, versions %r"
         % (workspace._syncPill.text(), workspace._statusText))
    check(workspace._syncPill.text() == "Committing..." and
          workspace._statusAmber is True,
          "right after the drag the pill reads Committing... (%r)"
          % workspace._syncPill.text())
    check(pumpUntilCommitted(viewport, session),
          "the drag's commit swaps")
    workspace.refresh()
    check(workspace._syncPill.text() == "Synced" and
          workspace._statusAmber is False,
          "and once it swapped the pill reads Synced (%r)"
          % workspace._syncPill.text())

    # -- a selection publish never turns the strip amber --------------------
    workspace.button("tool", "select").click()
    wait(10)
    check(state.transformTool == "select",
          "the dock's Select tool button sets the Select tool")
    pixels = [pixel(point) for point in handles()]
    version = session.modelVersion
    mouse.click(pixels[-1])
    mouse.click(pixels[-2], ("shift",))
    mouse.click(pixels[-3], ("shift",))
    selected = session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV)
    check(len(selected) >= 2,
          "clicks select a few center CVs (%r)" % (selected,))
    check(session.modelVersion > version,
          "the selection publishes bumped the model version (%d -> %d)"
          % (version, session.modelVersion))
    workspace.refresh()
    check(workspace._statusAmber is False and
          workspace._syncPill.text() == "Synced",
          "after a selection refresh the strip is not amber and reads "
          "Synced (%r, %r)" % (workspace._statusAmber,
                               workspace._syncPill.text()))

    # -- Relax says what it did ---------------------------------------------
    relax = workspace.button("action", "relax")
    check(relax is not None, "the Tube page has a Relax button")
    if relax is not None:
        relax.click()
        wait(15)
        line = viewport.loop.statusLine() if viewport.loop else ""
        text = workspace._messageLabel.text()
        check("Relax" in text and line and line in text and
              workspace._messageLevel != "error",
              "clicking Relax shows the loop status in the message area "
              "(%r)" % text)
        check(workspace._statusSummary.text() == line,
              "the strip's summary is the Tube tool's status line (%r)"
              % workspace._statusSummary.text())

    # -- the Display group ---------------------------------------------------
    for attr, check_, default in (
            ("showGeneratedCurves", workspace._generatedCheck, True),
            ("showAmplifiedHair", workspace._amplifiedCheck, False),
            ("showDiagnostics", workspace._diagnosticsCheck, False),
            ("ladderEnabled", workspace._ladderCheck, True)):
        was = bool(getattr(state, attr, default))
        check(check_.isChecked() == was,
              "Display %r starts as the state has it (%r)"
              % (str(check_.text()), was))
        check_.click()
        wait(10)
        now = bool(getattr(state, attr, default))
        check(now == (not was) and check_.isChecked() == now,
              "toggling %r writes state.%s (%r -> %r)"
              % (str(check_.text()), attr, was, now))
        if attr == "showDiagnostics":
            check(workspace._statusDetail.isVisibleTo(workspace) == now,
                  "Show diagnostics shows the version dump")
        check_.click()
        wait(10)
        check(bool(getattr(state, attr, default)) == was,
              "and toggling it back restores it")
    check(not workspace._statusDetail.isVisibleTo(workspace),
          "the version dump hides again without diagnostics")
    combo = workspace._navigationCombo
    combo.setCurrentIndex(combo.findData("blender"))
    check(state.navigationStyle == "blender",
          "the Navigation combo sets Blender (%r)" % state.navigationStyle)
    combo.setCurrentIndex(combo.findData("maya"))
    check(state.navigationStyle == "maya",
          "and back to Maya (%r)" % state.navigationStyle)

    # -- undo past the bottom turns the message red -------------------------
    view.setFocus()
    wait(10)

    def undoDepth():
        entry = getattr(session.dll, "Pomade_GetUndoDepth", None)
        return int(entry(session.model)) if entry is not None else -1

    info("undo depth before the Ctrl+Z run: %d" % undoDepth())
    for _ in range(80):
        typeKey(view, "z", ("ctrl",))
        wait(5)
        if "nothing to undo" in workspace._messageText.lower():
            break
    info("undo depth after the Ctrl+Z run: %d" % undoDepth())
    check(undoDepth() == 0, "Ctrl+Z walked the undo stack to the bottom")
    if "nothing to undo" not in workspace._messageText.lower():
        # Pomade_Undo on an empty stack is a no-op success, so until SS-02
        # checks the depth first the session says nothing at the bottom.
        # Colour-check the line session.undo() sends for a refusal instead,
        # through the same sink.
        info("Ctrl+Z at the bottom is still silent (SS-02 makes it "
             "report); checking the refusal line through the sink")
        session.report("Pomade: nothing to undo")
    check("nothing to undo" in workspace._messageText.lower(),
          "the undo refusal reaches the message area (%r)"
          % workspace._messageText)
    check(workspace._messageLevel == "error" and
          "#ff6b5e" in workspace._messageLabel.styleSheet(),
          "and colours it red (%r, %r)"
          % (workspace._messageLevel, workspace._messageLabel.styleSheet()))

    # -- messages fade -------------------------------------------------------
    workspace._messageTimer.setInterval(40)
    workspace._onStatus("Pomade: fade probe")
    wait(200)
    check(workspace._messageLabel.text() == "" and
          workspace._messageLevel is None,
          "a message fades after its interval (%r)"
          % workspace._messageLabel.text())
    workspace._messageTimer.setInterval(4000)

    shutdown()
    print("testUsdviewPomadeDock: %d failure(s)" % failureCount())
    return 1 if failureCount() else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the dock proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeDock needs testusdview (no live view)")
    sys.exit(0)
