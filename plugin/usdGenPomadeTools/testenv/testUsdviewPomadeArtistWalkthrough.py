# testUsdviewPomadeArtistWalkthrough -- TS-04: the live artist walkthrough
# (scratchpad walkthrough_pomade.py, 2026-09-23) promoted to a T3.
#
#   testusdview --testScript \
#       plugin/usdGenPomadeTools/testenv/testUsdviewPomadeArtistWalkthrough.py \
#       examples/pomade-single-quad.usda
#
# One artist session, start to finish, with REAL Qt input on the StageView
# and on the dock's own widgets (QTest clicks, typed keys, direct
# QMouseEvents): open the workspace from the usdGen > Pomade menu, find the
# dock gated until a scalp is bound, bind through the scalp picker, draw two
# regions with Create region clicks, then Tube (F8 body click, F9 keeping
# the owner tube, hover, the gizmo winning over a CV under its handle,
# tweak-drag, Escape mid-drag, Delete, Ctrl+Z/Ctrl+Y), Fill (dock density
# typed into the spin box, its undo, the length ramp drag), Hierarchy
# (Shift+D, Ctrl+Down/Up, Merge then Subdivide again), Sculpt (grab stroke,
# F resize), Output (Build hair description, Show amplified hair), Save
# through the dock (.usdc suffix, a refused .usda), the warnings row, the
# dock closed and reopened, and the sync pill. Every line the walkthrough
# logged as CONFUSING or FAIL is a hard check here.
import ctypes
import math
import os
import shutil
import sys
import tempfile

# Two disjoint triangles inside the single quad (the CV-regions T3's); the
# quad's centre stays uncovered, so the coverage row has something to show.
LEFT = ((-0.82, -0.56), (-0.22, -0.56), (-0.52, 0.34))
RIGHT = ((0.22, -0.56), (0.82, -0.56), (0.52, 0.34))
# A second Mesh far off to the side: with two meshes in the stage the
# dock's Bind button raises the scalp picker instead of binding directly.
DECOY_PATH = "/WalkthroughDecoyMesh"


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


# ---------------------------------------------------------------------------
# small readers and drivers local to this script
# ---------------------------------------------------------------------------

def l1TubeIds(session):
    got = ctypes.c_int(0)
    session.dll.Pomade_ReadL1TubeIds(session.model, None, 0, ctypes.byref(got))
    out = (ctypes.c_int * max(int(got.value), 1))()
    if session.dll.Pomade_ReadL1TubeIds(session.model, out, len(out),
                                       ctypes.byref(got)) != 0:
        return []
    return [int(out[i]) for i in range(int(got.value))]


def childrenOf(session, tubeId):
    got = ctypes.c_int(0)
    out = (ctypes.c_int * 64)()
    if session.dll.Pomade_GetTubeChildren(session.model, int(tubeId), out, 64,
                                         ctypes.byref(got)) != 0:
        return []
    return [int(out[i]) for i in range(int(got.value))]


def hoverOf(session):
    """The model's hover record, set only by a real hover event."""
    kind = ctypes.c_uint(0)
    ident = ctypes.c_int(-1)
    sub = ctypes.c_int(-1)
    subsub = ctypes.c_int(-1)
    if session.dll.Pomade_GetHover(session.model, ctypes.byref(kind),
                                  ctypes.byref(ident), ctypes.byref(sub),
                                  ctypes.byref(subsub)) != 0:
        return None
    return (int(kind.value), int(ident.value), int(sub.value),
            int(subsub.value))


def dist(a, b):
    return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))


def maxDelta(a, b):
    if not a or not b or len(a) != len(b):
        return float("inf")
    return max(dist(a[i], b[i]) for i in range(len(a)))


def menuAction(mainWindow, path):
    """The QAction at menu `path` in the main window's menu bar, or None."""
    actions = mainWindow.menuBar().actions()
    for depth, name in enumerate(path):
        found = None
        for action in actions:
            if str(action.text()).replace("&", "").strip() == name:
                found = action
                break
        if found is None:
            return None
        if depth == len(path) - 1:
            return found
        menu = found.menu()
        if menu is None:
            return None
        actions = menu.actions()
    return None


class ModalWatcher(object):
    """Answer the next modal QMessageBox from a QTimer (exec_ blocks).

    `seen` records each box's text; the box is accepted (its OK) so a
    warning can never hang the run.
    """

    def __init__(self):
        self.seen = []
        self.active = True
        self._arm()

    def _arm(self):
        from pxr.Usdviewq.qt import QtCore
        QtCore.QTimer.singleShot(40, self._poll)

    def _poll(self):
        from pxr.Usdviewq.qt import QtWidgets
        if not self.active:
            return
        candidates = [QtWidgets.QApplication.activeModalWidget()]
        candidates += list(QtWidgets.QApplication.topLevelWidgets())
        for widget in candidates:
            if isinstance(widget, QtWidgets.QMessageBox) and \
                    widget.isVisible():
                self.seen.append(str(widget.text()))
                widget.accept()
                break
        self._arm()

    def stop(self):
        self.active = False


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenPomadeTools
        from pxr import Gf, Sdf, Usd, UsdGeom
        from pxr.Usdviewq.qt import QtCore, QtWidgets
        from usdGenPomadeTools import (pomadeBridge, pomadeCamera, pomadeDockIds,
                                      pomadeGizmo, pomadeLib, pomadeLoops,
                                      pomadeModes, pomadePanels)
        import pomadeT3
        from pomadeT3 import (Mouse, check, failureCount, frameScalp,
                             guideCount, info, levelInfo, pumpUntilCommitted,
                             sideCamera, statusRecorder, tubeCenters,
                             typeKey, wait)
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools/pomadeT3: %s" % exc)
        return 1

    qtTest = pomadeT3._qtTest().QTest
    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    mainWindow = appController._mainWindow
    container = usdGenPomadeTools.container()
    tmpdir = tempfile.mkdtemp(prefix="pomadeWalkthrough")
    # The Save/Open dialogs remember their folder: in the test's own .ini,
    # never the artist's QSettings, even when this run dies half way.
    restoreSettings = pomadeT3.isolateSettings(container, tmpdir)
    priorToScene = container.saveGroomToScene()
    originalSaveDialog = QtWidgets.QFileDialog.getSaveFileName
    messages = statusRecorder()

    def tee(text, level="info"):
        """The test's recorder AND the dock's message area (DK-05)."""
        messages(text, level)
        workspace = getattr(container, "workspace", None)
        onStatus = getattr(workspace, "_onStatus", None)
        if onStatus is not None:
            onStatus(text, level)

    def recent(n=4):
        return list(messages)[-n:]

    def section(name):
        print("info: ==== %s" % name)

    def shutdown():
        if not _CLEANUP:
            return                      # already ran (idempotent)
        del _CLEANUP[:]
        QtWidgets.QFileDialog.getSaveFileName = originalSaveDialog
        container.setSaveGroomToScene(priorToScene)
        check(restoreSettings(),
              "the artist's own Pomade QSettings are untouched by the run "
              "(%r)" % (pomadeT3.realSettingsSnapshot(container),))
        viewport = getattr(container, "viewport", None)
        session = getattr(container, "session", None)
        try:
            if viewport is not None and viewport.installed:
                viewport.uninstall()
        finally:
            if session is not None:
                session.deactivate()
        shutil.rmtree(tmpdir, ignore_errors=True)

    # testUsdviewInputFunction runs this in a finally, so an exception in
    # the walkthrough still drops the model and the temp folder.
    _CLEANUP.append(shutdown)

    def finish():
        shutdown()
        return 1 if failureCount() else 0

    # ------------------------------------------------------------------
    section("fixture")
    check(stage is not None and bool(stage.GetPrimAtPath("/Scalp")),
          "the single-quad scalp is loaded")
    # Walkthrough CONFUSING: the fixture shipped an empty /Groom scope next
    # to the /PomadeGroom the tool commits, two groom scopes in the tree.
    check(not stage.GetPrimAtPath("/Groom"),
          "the fixture has no stray /Groom scope")
    with Usd.EditContext(stage, stage.GetSessionLayer()):
        decoy = UsdGeom.Mesh.Define(stage, Sdf.Path(DECOY_PATH))
        decoy.CreatePointsAttr([Gf.Vec3f(40, 0, 40), Gf.Vec3f(41, 0, 40),
                                Gf.Vec3f(40, 0, 41)])
        decoy.CreateFaceVertexCountsAttr([3])
        decoy.CreateFaceVertexIndicesAttr([0, 2, 1])
    check(frameScalp(stage, view, eye=(0.0, 7.0, 0.0),
                     primPath="/WalkthroughTopCamera"),
          "a top-down scene camera is active")
    view.setFocus()
    wait(50)

    # ------------------------------------------------------------------
    section("open the workspace from the usdGen > Pomade menu")
    action = menuAction(mainWindow, ("usdGen", "Pomade", "Open workspace"))
    check(action is not None, "usdGen > Pomade > Open workspace is in the "
                              "menu bar")
    if action is None:
        return finish()
    action.trigger()
    wait(100)
    workspace = container.workspace
    state = container.pomadeState
    viewport = container.viewport
    check(workspace is not None and workspace.isVisible() and
          state.workspaceOpen, "the menu opened the Pomade dock")
    if workspace is None or viewport is None:
        return finish()
    session = container.session
    if session is not None:
        session.setStatusSink(tee)

    # Walkthrough CONFUSING: 2 entered Tube before anything was bound.
    section("the dock is gated until a scalp is bound")
    workspace.refresh()
    check(workspace._firstRunHint.isVisible(),
          "the first-run hint tells the artist to bind a scalp")
    check(workspace._geometryPathLabel.text() == "No scalp bound",
          "the scalp line reads 'No scalp bound' (%r)"
          % workspace._geometryPathLabel.text())
    modeButtons = {m.id: workspace.button("mode", m.id)
                   for m in pomadeModes.MODES}
    check(all(b is not None and not b.isEnabled()
              for b in modeButtons.values()),
          "every mode button is disabled before a bind")
    bindButton = workspace.button("file", "bind")
    check(bindButton is not None and bindButton.isEnabled() and
          bindButton.isVisible(), "the Bind scalp button is live")
    modeBefore = state.activeMode
    qtTest.mouseClick(modeButtons["tube"], QtCore.Qt.LeftButton)
    wait(20)
    check(state.activeMode == modeBefore,
          "clicking the disabled Tube button does nothing (%r)"
          % state.activeMode)
    viewport.setPointerInside(True)
    del messages[:]
    typeKey(view, "2")
    wait(20)
    check(state.activeMode == modeBefore and state.activeMode != "tube",
          "2 does not enter Tube before a bind (%r)" % state.activeMode)
    check(any("bind a scalp mesh first" in m for m in messages),
          "the refusal says to bind a scalp first (%r)" % recent())
    viewport.setPointerInside(False)

    # ------------------------------------------------------------------
    section("bind through the dock's scalp picker")
    picker = {"dialog": False, "items": [], "chosen": False}

    def choose():
        dialog = QtWidgets.QApplication.activeModalWidget()
        picker["dialog"] = isinstance(dialog, QtWidgets.QDialog)
        if not picker["dialog"]:
            return
        combo = dialog.findChild(QtWidgets.QComboBox, "pomadeGeometryChoices")
        if combo is not None:
            picker["items"] = [combo.itemText(i)
                               for i in range(combo.count())]
            index = combo.findText("/Scalp", QtCore.Qt.MatchExactly)
            if index >= 0:
                combo.setCurrentIndex(index)
                picker["chosen"] = True
        box = dialog.findChild(QtWidgets.QDialogButtonBox,
                               "pomadeGeometryPickerButtons")
        ok = box.button(QtWidgets.QDialogButtonBox.Ok) if box else None
        if picker["chosen"] and ok is not None:
            qtTest.mouseClick(ok, QtCore.Qt.LeftButton)
        else:
            dialog.reject()

    QtCore.QTimer.singleShot(0, choose)
    qtTest.mouseClick(bindButton, QtCore.Qt.LeftButton)
    wait(150)
    check(picker["dialog"] and "/Scalp" in picker["items"] and
          DECOY_PATH in picker["items"],
          "Bind raised the modal picker listing both meshes (%r)"
          % (picker["items"],))
    session = container.session
    viewport = container.viewport
    check(session is not None and session.model is not None and
          session.scalpPath == "/Scalp" and viewport.installed,
          "the picker bound /Scalp and created the live model")
    if session is None or session.model is None:
        return finish()
    session.setStatusSink(tee)
    check(state.activeMode == "graph" and state.graphSubMode == "region",
          "binding lands in Graph / Create region (%r/%r)"
          % (state.activeMode, state.graphSubMode))
    workspace.refresh()
    check(not workspace._firstRunHint.isVisible() and
          all(b.isEnabled() for b in modeButtons.values()),
          "the hint goes and the mode buttons come alive")
    check(workspace._geometryPathLabel.text() == "Scalp: /Scalp",
          "the scalp line names the bound mesh (%r)"
          % workspace._geometryPathLabel.text())
    # Straight after the picker, without clicking the viewport.
    typeKey(view, "2")
    wait(20)
    check(state.activeMode == "tube",
          "a mode key works right after the picker closed")
    typeKey(view, "1")
    wait(20)
    check(state.activeMode == "graph", "1 returns to Graph")

    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the controller camera resolves")
    if camera is None:
        return finish()
    state.snapRadiusPx = max(
        0.05 / max(camera.worldPerPixel((0.0, 0.0, 0.0)), 1e-9), 2.0)
    mouse = Mouse(view)
    mouse.direct = True
    viewport.setPointerInside(True)
    view.setFocus()
    typeKey(view, "r")
    wait(10)
    check(state.graphSubMode == "region", "R keeps Create region")

    def pixelXZ(point):
        p = camera.worldToPixels((point[0], 0.0, point[1]))
        return (p[0], p[1])

    def pixel3(point):
        p = camera.worldToPixels(point)
        return (p[0], p[1])

    # ------------------------------------------------------------------
    section("region A: Create region clicks, Enter closes")
    for point in LEFT:
        mouse.click(pixelXZ(point))
        wait(15)
    draft = viewport.regionDraftPreview()["points"]
    check(len(draft) == 3, "three clicks made a 3-CV draft (%d)" % len(draft))
    typeKey(view, "return")
    wait(60)
    counts = session.graphCounts()
    ids = l1TubeIds(session)
    check(counts[2] == 1 and len(ids) == 1,
          "Enter closed region A and grew one L1 tube (%r, %r)"
          % (counts, ids))
    info1 = levelInfo(session, 1)
    check(info1 is not None and info1[2] == 1,
          "/__usdGenPomade publishes one L1 tube (%r)" % (info1,))
    guidesA = guideCount(session)
    # MD-01: guides appear by default once a region closes.
    check(guidesA > 0, "guides exist as soon as region A closed (%d)"
          % guidesA)

    section("region B: closed by clicking its first CV")
    for point in RIGHT:
        mouse.click(pixelXZ(point))
        wait(15)
    mouse.click(pixelXZ(RIGHT[0]))
    wait(60)
    counts = session.graphCounts()
    ids = l1TubeIds(session)
    check(counts[2] == 2 and len(ids) == 2,
          "clicking the first CV closed region B (%r, tubes %r)"
          % (counts, ids))
    if len(ids) != 2:
        return finish()
    guidesAB = guideCount(session)
    check(guidesAB > guidesA,
          "region B grew guides of its own (%d -> %d)" % (guidesA, guidesAB))
    check(session.enqueueCommit() is not False and
          pumpUntilCommitted(viewport, session, tries=200),
          "the idle pump drains the committer")
    check(bool(stage.GetPrimAtPath("/PomadeGroom")) and
          not stage.GetPrimAtPath("/Groom"),
          "the groom committed to /PomadeGroom, the only groom scope")

    def pillSynced(tries=160):
        """Pump until the dock's pill reads Synced; the last text."""
        text = ""
        for _ in range(tries):
            viewport.pumpOnce()
            workspace.refresh()
            text = workspace._syncPill.text()
            if text == "Synced" and not session.hasPendingWork():
                return text
            wait(25)
        return text

    pill = pillSynced()
    check(pill == "Synced" and not workspace._statusAmber,
          "idle after the two regions, the sync pill reads Synced (%r)"
          % pill)

    # ------------------------------------------------------------------
    section("Tube (2): side camera, dock rows, instruction")
    sideCamera(stage, view, eye=(0.0, 2.0, 12.0), target=(0.0, 2.0, 0.0),
               up=(0.0, 1.0, 0.0), primPath="/WalkthroughSideCamera")
    wait(60)
    camera = pomadeCamera.resolve(view)
    spacing = 1.0 / max(camera.worldPerPixel((0.0, 2.0, 0.0)), 1e-9)
    # ~55 px between CVs: a whole 4-unit tube in frame, and the next CV
    # inside the 90 px gizmo axis (the contested press below).
    z = 12.0 * spacing / 55.0
    sideCamera(stage, view, eye=(0.0, 2.0, z), target=(0.0, 2.0, 0.0),
               up=(0.0, 1.0, 0.0), primPath="/WalkthroughSideCamera")
    wait(60)
    camera = pomadeCamera.resolve(view)
    info("side camera at z=%.1f: %.1f px per unit" %
         (z, 1.0 / max(camera.worldPerPixel((0.0, 2.0, 0.0)), 1e-9)))
    viewport.setPointerInside(False)
    typeKey(view, "2")
    wait(30)
    check(state.activeMode == "tube" and viewport.loop is not None and
          viewport.loop.modeId == "tube",
          "2 selects Tube with the pointer outside the view")
    check(state.tubeSubMode == "center",
          "Tube opens in Center CV (%r)" % state.tubeSubMode)
    workspace.refresh()
    check(workspace._tubeSelectionRow.isVisible() and
          not workspace._subModeStack.isVisible(),
          "Tube shows ONE component row, the F8-F11 row")
    check(bool(workspace._instructionLabel.text().strip()),
          "Tube shows an instruction line (%r)"
          % workspace._instructionLabel.text())

    handles = {t: pomadeBridge.tubeCenterHandles(session.dll, session.model,
                                                t) for t in ids}
    rightTube = max(ids, key=lambda t: pixel3(handles[t][2])[0])
    leftTube = min(ids, key=lambda t: pixel3(handles[t][2])[0])
    cvCount = len(tubeCenters(session, rightTube))
    check(cvCount >= 4, "the right tube has a center column (%d CVs)"
          % cvCount)
    tip = cvCount - 1

    # Whole-tube body click (F8 from the dock's row).
    section("F8 body click, F9 keeps the owner tube")
    f8 = workspace.button("comp", "tube")
    check(f8 is not None and f8.isVisible(), "the dock's F8 button is shown")
    qtTest.mouseClick(f8, QtCore.Qt.LeftButton)
    wait(20)
    check(state.tubeSubMode == "tube", "the dock's F8 button picks Whole "
                                       "tube (%r)" % state.tubeSubMode)
    h1 = handles[rightTube][1]
    wall = None
    for off in ((0.15, 0.15), (0.0, 0.3), (-0.15, 0.15), (0.2, 0.0),
                (0.0, 0.2)):
        cand = (h1[0] + off[0], h1[1] + 0.5, h1[2] + off[1])
        cp = pixel3(cand)
        probe = session.pickItem(camera, cp[0], cp[1], 8.0,
                                 pomadeLib.POMADE_PICK_TUBE_VERT)
        if probe is not None and int(probe["id"]) == rightTube:
            wall = cand
            break
    check(wall is not None, "a visible wall point of tube %d resolves"
          % rightTube)
    if wall is None:
        return finish()
    viewport.setPointerInside(True)
    mouse.click(pixel3(wall))
    wait(30)
    selTube = session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT)
    check(selTube == [(rightTube, -1, -1)],
          "a body click selects the whole tube (%r)" % (selTube,))
    # Walkthrough CONFUSING: F9 ignored unless the pointer is over the
    # view. Keys now follow the view's keyboard focus as well (FB-01).
    view.setFocus()
    wait(10)
    viewport.setPointerInside(False)
    typeKey(view, "f9")
    wait(20)
    check(state.tubeSubMode == "center",
          "F9 with the view focused and the pointer outside picks Center "
          "CV (%r)" % state.tubeSubMode)
    check((rightTube, -1, -1) in session.readSelection(
        pomadeLib.POMADE_PICK_TUBE_VERT),
          "F9 converted the selection and kept the owner tube (%r)"
          % (session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT),))
    workspace.refresh()
    check([k for k, b in workspace._tubeSelectionButtons.items()
           if b.isChecked()] == ["center"],
          "the dock's F9 button is the checked one")
    viewport.setPointerInside(True)

    for key, tool in (("e", "rotate"), ("r", "scale"), ("q", "select"),
                      ("w", "move")):
        typeKey(view, key)
        wait(15)
        workspace.refresh()
        button = workspace.button("tool", tool)
        check(viewport.loop.transformTool() == tool and
              button is not None and button.isChecked(),
              "%s picks %s and the dock's tool row follows"
              % (key.upper(), tool))

    # ------------------------------------------------------------------
    section("select a center CV, hover highlight")
    gizmo = viewport.loop._gizmo

    def cvPixel(cv):
        centers = pomadeBridge.tubeCenterHandles(session.dll, session.model,
                                                rightTube)
        return pixel3(centers[cv])

    def selectCV(cv):
        mouse.click(cvPixel(cv))
        wait(25)
        return session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV)

    session.clearSelection(pomadeLib.POMADE_PICK_ALL)
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    viewport.refreshGizmo()
    sel = selectCV(1)
    check(sel == [(rightTube, 1, -1)],
          "one click selected tube %d CV 1 (%r)" % (rightTube, sel))
    overlay = getattr(viewport, "_gizmoOverlay", None)
    check(gizmo.visible and gizmo.kind == pomadeGizmo.GIZMO_TRANSLATE and
          overlay is not None and overlay.isVisible(),
          "a Move gizmo is drawn at the CV (kind %r)" % gizmo.kind)
    # Hover over the tip (off the gizmo): the model's hover names it.
    tipPx = cvPixel(tip)
    check(gizmo.handleAt(camera, tipPx[0], tipPx[1]) ==
          pomadeGizmo.HANDLE_NONE, "the tip CV is off the gizmo")
    mouse.unheldMove((tipPx[0] + 30.0, tipPx[1]))
    wait(15)
    mouse.unheldMove(tipPx)
    wait(20)
    hovered = hoverOf(session)
    check(hovered == (pomadeLib.POMADE_PICK_CENTER_CV, rightTube, tip, -1),
          "hovering the tip CV highlights it (%r)" % (hovered,))
    # Hover over CV2, which sits on the V axis: the handle lights instead.
    p2 = cvPixel(2)
    handleAtP2 = gizmo.handleAt(camera, p2[0], p2[1])
    component = viewport.loop._componentItem(
        pomadeLoops.Sample(session, camera, p2[0], p2[1]))
    check(handleAtP2 == pomadeGizmo.HANDLE_V and component is not None and
          (component["id"], component["subId"]) == (rightTube, 2),
          "CV 2's pixel is on the V handle and on the unselected CV 2 "
          "(%r, %r)" % (handleAtP2, component))
    mouse.unheldMove(p2)
    wait(20)
    check(gizmo.hoverHandle == pomadeGizmo.HANDLE_V and
          hoverOf(session) in (None, (0, -1, -1, -1)),
          "hovering there prehighlights the handle, not the CV (%r, %r)"
          % (gizmo.hoverHandle, hoverOf(session)))

    section("Delete a CV, Ctrl+Z restores it")
    before = tubeCenters(session, rightTube)
    typeKey(view, "delete")
    wait(40)
    check(len(tubeCenters(session, rightTube)) == len(before) - 1,
          "Delete removed the selected center CV")
    typeKey(view, "z", ("ctrl",))
    wait(40)
    check(maxDelta(tubeCenters(session, rightTube), before) < 1e-4,
          "Ctrl+Z put the CV back")
    check(guideCount(session) > 0, "the guides survive the undo (%d)"
          % guideCount(session))

    # ------------------------------------------------------------------
    section("drag the gizmo's U axis; Ctrl+Z / Ctrl+Y")
    sel = selectCV(1)
    check(sel == [(rightTube, 1, -1)], "CV 1 selected again (%r)" % (sel,))
    camera = pomadeCamera.resolve(view)
    records = viewport.gizmoScreenHandles()
    axis = next((r for r in records if r["kind"] == "axis" and
                 r["handle"] == pomadeGizmo.HANDLE_U), None)
    check(axis is not None and axis["grabbable"],
          "the U axis is drawn and grabbable")
    before = tubeCenters(session, rightTube)
    afterAxis = before
    if axis is not None:
        a, b = axis["points"][0], axis["points"][-1]
        pressAt = (a[0] + 0.6 * (b[0] - a[0]), a[1] + 0.6 * (b[1] - a[1]))
        norm = math.hypot(b[0] - a[0], b[1] - a[1]) or 1.0
        ux, uy = (b[0] - a[0]) / norm, (b[1] - a[1]) / norm
        mouse.press(pressAt)
        check(viewport.gestureActive and gizmo.dragging and
              gizmo.activeHandle == pomadeGizmo.HANDLE_U,
              "the press on the axis grabbed HANDLE_U")
        for s in range(1, 6):
            mouse.move((pressAt[0] + ux * 10.0 * s, pressAt[1] + uy * 10.0 * s))
            wait(5)
        mouse.release((pressAt[0] + ux * 50.0, pressAt[1] + uy * 50.0))
        wait(30)
        afterAxis = tubeCenters(session, rightTube)
        moved = [dist(afterAxis[i], before[i]) for i in range(len(before))]
        want = 50.0 * camera.worldPerPixel(before[1])
        dx = afterAxis[1][0] - before[1][0]
        check(abs(abs(dx) - want) < 0.25 * want and
              abs(afterAxis[1][1] - before[1][1]) < 0.1 * want,
              "CV 1 moved along X by the drag (%.4f vs %.4f)" % (dx, want))
        check(sum(1 for m in moved if m > 1e-5) == 1,
              "only the selected CV moved (%r)" % [round(m, 4) for m in moved])
        check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
              [(rightTube, 1, -1)], "the selection survived the drag")
    typeKey(view, "z", ("ctrl",))
    wait(30)
    check(maxDelta(tubeCenters(session, rightTube), before) < 1e-4,
          "Ctrl+Z restored the pre-drag CVs")
    check(guideCount(session) > 0, "guides survive Ctrl+Z (%d)"
          % guideCount(session))
    typeKey(view, "y", ("ctrl",))
    wait(30)
    check(maxDelta(tubeCenters(session, rightTube), afterAxis) < 1e-4,
          "Ctrl+Y re-applied the drag")
    check(guideCount(session) > 0, "guides survive Ctrl+Y (%d)"
          % guideCount(session))
    typeKey(view, "z", ("ctrl",))
    wait(30)

    # Walkthrough FAIL: a press on an unselected CV lying under a gizmo
    # handle selected the CV instead of dragging the handle.
    section("a CV under the gizmo's handle: the handle wins")
    sel = selectCV(1)
    check(sel == [(rightTube, 1, -1)], "CV 1 selected (%r)" % (sel,))
    camera = pomadeCamera.resolve(view)
    p2 = cvPixel(2)
    check(gizmo.handleAt(camera, p2[0], p2[1]) == pomadeGizmo.HANDLE_V,
          "CV 2's pixel is still on the V handle")
    before = tubeCenters(session, rightTube)
    mouse.unheldMove(p2)
    wait(10)
    mouse.press(p2)
    check(viewport.gestureActive and gizmo.dragging and
          gizmo.activeHandle == pomadeGizmo.HANDLE_V,
          "the press drags the V handle (active %r)" % gizmo.activeHandle)
    for s in range(1, 6):
        mouse.move((p2[0], p2[1] - 10.0 * s))
        wait(5)
    mouse.release((p2[0], p2[1] - 50.0))
    wait(30)
    after = tubeCenters(session, rightTube)
    want = 50.0 * camera.worldPerPixel(before[1])
    check(abs((after[1][1] - before[1][1]) - want) < 0.1 * want and
          dist(after[2], before[2]) < 1e-5 and
          session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
          [(rightTube, 1, -1)],
          "CV 1 rose by the travel, CV 2 stayed, CV 1 still selected "
          "(%.4f vs %.4f)" % (after[1][1] - before[1][1], want))
    typeKey(view, "z", ("ctrl",))
    wait(30)
    check(maxDelta(tubeCenters(session, rightTube), before) < 1e-4,
          "one Ctrl+Z undoes the handle drag")

    section("tweak-drag an unselected CV")
    viewport.refreshGizmo()
    camera = pomadeCamera.resolve(view)
    tipPx = cvPixel(tip)
    check(gizmo.handleAt(camera, tipPx[0], tipPx[1]) ==
          pomadeGizmo.HANDLE_NONE, "the tip is off the gizmo")
    before = tubeCenters(session, rightTube)
    depth = session.undoDepth()
    mouse.drag([tipPx] + [(tipPx[0], tipPx[1] + 10.0 * s)
                          for s in range(1, 7)])
    wait(30)
    after = tubeCenters(session, rightTube)
    want = 60.0 * camera.worldPerPixel(before[tip])
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
          [(rightTube, tip, -1)] and
          abs((before[tip][1] - after[tip][1]) - want) < 0.1 * want,
          "one press selected the tip and dragged it down (%.4f vs %.4f)"
          % (before[tip][1] - after[tip][1], want))
    check(session.undoDepth() == depth + 1,
          "the tweak is one undo step (%d -> %d)"
          % (depth, session.undoDepth()))
    typeKey(view, "z", ("ctrl",))
    wait(30)
    check(maxDelta(tubeCenters(session, rightTube), before) < 1e-4,
          "Ctrl+Z undoes the whole tweak")

    section("Escape mid-drag restores the press-time shape")
    sel = selectCV(1)
    check(sel == [(rightTube, 1, -1)], "CV 1 selected (%r)" % (sel,))
    p1 = cvPixel(1)
    before = tubeCenters(session, rightTube)
    mouse.press(p1)
    check(viewport.gestureActive, "the press on the selected CV opened a "
                                  "drag")
    for s in range(1, 5):
        mouse.move((p1[0] + 12.0 * s, p1[1]))
        wait(5)
    check(maxDelta(tubeCenters(session, rightTube), before) > 1e-4,
          "the CV follows the cursor mid-drag")
    typeKey(view, "escape")
    wait(20)
    check(not viewport.gestureActive and
          maxDelta(tubeCenters(session, rightTube), before) < 1e-6,
          "Escape ended the drag and restored the CVs exactly")
    mouse.move((p1[0] + 70.0, p1[1]))
    mouse.release((p1[0] + 70.0, p1[1]))
    wait(20)
    check(maxDelta(tubeCenters(session, rightTube), before) < 1e-6,
          "the trailing release changed nothing")
    check(session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV) ==
          [(rightTube, 1, -1)] and gizmo.visible,
          "Escape kept the selection and the gizmo (%r)"
          % (session.readSelection(pomadeLib.POMADE_PICK_CENTER_CV),))

    # ------------------------------------------------------------------
    section("Fill (3): the dock's density against the model")
    typeKey(view, "3")
    wait(30)
    check(state.activeMode == "fill", "3 selects Fill (%r)"
          % state.activeMode)
    if state.fillSubMode != "params":
        typeKey(view, "p")
        wait(10)
    check(state.fillSubMode == "params", "Fill is in Params (%r)"
          % state.fillSubMode)
    session.clearSelection(pomadeLib.POMADE_PICK_ALL)
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    mouse.click(pixel3(wall))
    wait(30)
    check(session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) ==
          [(rightTube, -1, -1)],
          "a click on the tube selects it for the Fill panel (%r)"
          % (session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT),))
    workspace.refresh()
    density = next((w for d, w in workspace._paramWidgets
                    if d.id == "density"), None)
    check(density is not None and density.isEnabled(),
          "the Fill page has a live Density field")

    def modelDensity():
        return float(session.stageLib.fillParams(session.model,
                                                 rightTube)["density"])

    if density is not None:
        check(abs(density.value() - modelDensity()) < 1e-3,
              "the Density field shows the model's density (%r vs %r)"
              % (density.value(), modelDensity()))

        def typeDensity(text):
            qtTest.mouseClick(density, QtCore.Qt.LeftButton)
            wait(10)
            qtTest.keyClick(density, QtCore.Qt.Key_A,
                            QtCore.Qt.ControlModifier)
            qtTest.keyClicks(density, text)
            qtTest.keyClick(density, QtCore.Qt.Key_Return)
            wait(60)
            workspace.refresh()

        typeDensity("4")
        gLow = guideCount(session)
        check(abs(modelDensity() - 4.0) < 1e-3 and
              abs(density.value() - 4.0) < 1e-3,
              "typing 4 + Enter reaches the model (%r)" % modelDensity())
        typeDensity("40")
        gHigh = guideCount(session)
        check(abs(modelDensity() - 40.0) < 1e-3 and gHigh > gLow > 0,
              "typing 40 grows more guides (%d -> %d)" % (gLow, gHigh))
        check(not isinstance(QtWidgets.QApplication.focusWidget(),
                             QtWidgets.QAbstractSpinBox),
              "Enter hands the keyboard back from the spin box")
        # Walkthrough CONFUSING: Ctrl+Z after a density edit made every
        # guide vanish.
        view.setFocus()
        viewport.setPointerInside(True)
        typeKey(view, "z", ("ctrl",))
        wait(40)
        workspace.refresh()
        check(guideCount(session) == gLow and guideCount(session) > 0,
              "Ctrl+Z of the density edit keeps the guides (%d, want %d)"
              % (guideCount(session), gLow))
        check(abs(modelDensity() - 4.0) < 1e-3 and
              abs(density.value() - 4.0) < 1e-3,
              "Ctrl+Z put density 4 back in the model and the field "
              "(%r, %r)" % (modelDensity(), density.value()))
    check(bool(workspace._instructionLabel.text().strip()),
          "Fill shows an instruction line")

    section("Fill: the length ramp drag")
    typeKey(view, "v")
    wait(20)
    check(state.fillSubMode == "preview", "V picks Length ramp (%r)"
          % state.fillSubMode)

    def profileCount():
        return int(session.stageLib.fillParams(session.model,
                                               rightTube)["profileCount"])

    profileBefore = profileCount()
    rampAt = pixel3((wall[0], wall[1] + 1.5, wall[2]))
    probe = session.pickItem(camera, rampAt[0], rampAt[1], 8.0,
                             pomadeLib.POMADE_PICK_TUBE_VERT)
    if probe is None or int(probe["id"]) != rightTube:
        rampAt = pixel3(wall)
    del messages[:]
    depth = session.undoDepth()
    # DOWN: the fresh groom's ramp is full length, and a length fraction
    # cannot grow past 1 (the kernels clamp it), so an up-drag there
    # rightly changes nothing. Shortening is the edit an artist makes.
    mouse.drag([rampAt] + [(rampAt[0], rampAt[1] + 12.0 * s)
                           for s in range(1, 6)])
    wait(40)
    check(profileCount() > profileBefore,
          "the ramp drag wrote a length profile on the pressed tube "
          "(%d -> %d)" % (profileBefore, profileCount()))
    check(any("length" in m.lower() for m in messages),
          "the status line reads the ramp (%r)" % recent(3))
    check(session.undoDepth() == depth + 1 and guideCount(session) > 0,
          "the ramp is one undo step and the guides are back at full "
          "density (%d)" % guideCount(session))
    typeKey(view, "z", ("ctrl",))
    wait(30)
    check(profileCount() == profileBefore and guideCount(session) > 0,
          "Ctrl+Z takes the ramp back and keeps the guides")

    # ------------------------------------------------------------------
    section("Hierarchy (4): Shift+D, Ctrl+Down/Up, Merge, Subdivide again")
    typeKey(view, "4")
    wait(30)
    check(state.activeMode == "hierarchy" and
          viewport.loop.modeId == "hierarchy", "4 selects Hierarchy")
    workspace.refresh()
    check(bool(workspace._instructionLabel.text().strip()),
          "Hierarchy shows an instruction line (%r)"
          % workspace._instructionLabel.text())
    typeKey(view, "a", ("ctrl", "shift"))
    wait(20)
    check(session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) == [],
          "Ctrl+Shift+A clears the selection")
    mouse.click(pixel3(wall))
    wait(30)
    check(session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) ==
          [(rightTube, -1, -1)], "a click on the tube wall selects it")
    count = int(state.subdivideCount)
    typeKey(view, "d", ("shift",))
    wait(80)
    kids = childrenOf(session, rightTube)
    info2 = levelInfo(session, 2)
    check(len(kids) == count and info2 is not None and info2[2] == count,
          "Shift+D published %d children at L2 (%r, %r)"
          % (count, kids, info2))
    check(state.activeLevel == 2, "focus moved to L2 (%r)"
          % state.activeLevel)
    typeKey(view, "up", ("ctrl",))
    wait(40)
    check(state.activeLevel == 1 and
          session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) ==
          [(rightTube, -1, -1)],
          "Ctrl+Up exits to L1 with the parent selected")
    typeKey(view, "down", ("ctrl",))
    wait(40)
    check(state.activeLevel == 2 and
          sorted(t for t, _s, _ss in session.readSelection(
              pomadeLib.POMADE_PICK_TUBE_VERT)) == sorted(kids),
          "Ctrl+Down re-enters L2 with the children selected")
    typeKey(view, "down", ("ctrl",))
    wait(40)
    check(state.activeLevel == 2,
          "Ctrl+Down at the leaf level stays at L2 (%r)" % state.activeLevel)
    typeKey(view, "up", ("ctrl",))
    wait(40)
    check(state.activeLevel == 1, "Ctrl+Up back to L1")
    del messages[:]
    typeKey(view, "m", ("shift",))
    wait(80)
    check(childrenOf(session, rightTube) == [],
          "Shift+M merged the children back (%r, %r)"
          % (childrenOf(session, rightTube), recent(2)))
    if state.activeLevel != 1:
        typeKey(view, "up", ("ctrl",))
        wait(40)
    if session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT) != \
            [(rightTube, -1, -1)]:
        mouse.click(pixel3(wall))
        wait(30)
    del messages[:]
    typeKey(view, "d", ("shift",))
    wait(80)
    kids = childrenOf(session, rightTube)
    info2 = levelInfo(session, 2)
    check(len(kids) == count and info2 is not None and info2[2] == count and
          not any("not a simple polygon" in m or "collapsed" in m
                  for m in messages),
          "Subdivide after Merge splits the tube again (%r, %r, %r)"
          % (kids, info2, recent(2)))
    check(guideCount(session) > 0, "the guides survive merge and "
                                   "re-subdivide (%d)" % guideCount(session))
    while state.activeLevel > 1:
        before = state.activeLevel
        typeKey(view, "up", ("ctrl",))
        wait(40)
        if state.activeLevel == before:
            break

    # ------------------------------------------------------------------
    section("Sculpt (5): grab stroke and F resize")
    typeKey(view, "5")
    wait(30)
    check(state.activeMode == "sculpt" and viewport.loop.modeId == "sculpt",
          "5 selects Sculpt")
    loop = viewport.loop
    typeKey(view, "g")
    wait(10)
    workspace.refresh()
    check(bool(workspace._instructionLabel.text().strip()),
          "Sculpt shows an instruction line")
    state.brushRadiusPx = 40.0
    state.sculptPreserveLength = True
    state.sculptMirrorX = False
    state.brushTRadius = 0.0
    camera = pomadeCamera.resolve(view)
    hl = pomadeBridge.tubeCenterHandles(session.dll, session.model, leftTube)
    baseL = tubeCenters(session, leftTube)
    start = pixel3(hl[-2])
    mouse.unheldMove(start)
    wait(20)
    mouse.press(start)
    check(viewport.gestureActive, "a press on the tube opened a grab stroke")
    for s in range(1, 5):
        mouse.move((start[0] + 12.0 * s, start[1]))
        wait(5)
    mouse.release((start[0] + 48.0, start[1]))
    wait(30)
    movedL = tubeCenters(session, leftTube)
    check(maxDelta(movedL, baseL) > 1e-3,
          "the grab stroke moved the tube's centers (%.4f)"
          % maxDelta(movedL, baseL))
    check(dist(movedL[0], baseL[0]) < 1e-4, "the root stayed pinned")
    beforeF = tubeCenters(session, leftTube)
    r0 = float(state.brushRadiusPx)
    qtTest.keyPress(view, QtCore.Qt.Key_F)
    wait(10)
    check(viewport._brushResizeArmed, "holding F arms the brush resize")
    mouse.press(start)
    check(viewport.gestureActive and viewport._brushResizeActive,
          "F+drag starts a resize, not a stroke")
    mouse.move((start[0] + 60.0, start[1]))
    wait(10)
    live = loop.brushRadiusPx()
    mouse.release((start[0] + 60.0, start[1]))
    qtTest.keyRelease(view, QtCore.Qt.Key_F)
    wait(20)
    check(live > r0 and abs(state.brushRadiusPx - live) < 1e-5,
          "the F-drag grew the brush %.1f -> %.1f and kept it"
          % (r0, state.brushRadiusPx))
    check(maxDelta(tubeCenters(session, leftTube), beforeF) < 1e-6,
          "the F-drag did not sculpt")
    workspace.refresh()
    radiusWidget = next((w for d, w in workspace._paramWidgets
                         if d.id == "brushRadiusPx"), None)
    check(radiusWidget is not None and
          abs(radiusWidget.value() - state.brushRadiusPx) < 0.5,
          "the dock's Brush radius shows the new radius")

    # ------------------------------------------------------------------
    section("Output (6): Build hair description, Show amplified hair")
    typeKey(view, "6")
    wait(30)
    check(state.activeMode == "output", "6 selects Output")
    workspace.refresh()
    check(not workspace._subModeStack.isVisible(),
          "Output shows no sub-mode shelf")
    check(bool(workspace._instructionLabel.text().strip()),
          "Output shows an instruction line")
    build = workspace.button("action", "buildDescription")
    check(build is not None and build.isVisible() and build.isEnabled(),
          "the Build hair description button is live")
    if build is not None:
        qtTest.mouseClick(build, QtCore.Qt.LeftButton)
        built = False
        for _ in range(200):
            viewport.pumpOnce()
            wait(25)
            if stage.GetPrimAtPath("/PomadeGroom/Output") and \
                    not session.hasPendingWork():
                built = True
                break
        check(built, "/PomadeGroom/Output committed after Build")
    workspace.refresh()
    amplified = workspace._amplifiedCheck

    def modelAmplified():
        return bool(session.dll.Pomade_GetAmplifiedHair(session.model))

    check(amplified.isChecked() and modelAmplified(),
          "Build turned Show amplified hair on, in the dock and the model")
    qtTest.mouseClick(amplified, QtCore.Qt.LeftButton)
    wait(30)
    check(not amplified.isChecked() and not modelAmplified(),
          "clicking Show amplified hair hides it in the model")
    qtTest.mouseClick(amplified, QtCore.Qt.LeftButton)
    wait(30)
    check(amplified.isChecked() and modelAmplified(),
          "clicking it again shows the amplified hair")

    # ------------------------------------------------------------------
    section("Save groom through the dock")
    container.setSaveGroomToScene(False)
    chosen = {"path": ""}

    def fakeDialog(*_args, **_kwargs):
        return chosen["path"], "USD crate (*.usdc)"

    QtWidgets.QFileDialog.getSaveFileName = fakeDialog
    save = workspace.button("file", "save")
    check(save is not None and save.isEnabled(),
          "the dock's Save button is live")
    try:
        chosen["path"] = os.path.join(tmpdir, "walkGroom")
        watcher = ModalWatcher()
        if save is not None:
            qtTest.mouseClick(save, QtCore.Qt.LeftButton)
        wait(50)
        watcher.stop()
        check(os.path.isfile(os.path.join(tmpdir, "walkGroom.usdc")) and
              not watcher.seen,
              "a bare 'walkGroom' saves walkGroom.usdc with no warning "
              "(%r, %r)" % (os.listdir(tmpdir), watcher.seen))
        # Walkthrough CONFUSING: a .usda failed with only a status line.
        chosen["path"] = os.path.join(tmpdir, "walkGroom.usda")
        watcher = ModalWatcher()
        if save is not None:
            qtTest.mouseClick(save, QtCore.Qt.LeftButton)
        wait(50)
        watcher.stop()
        check(not os.path.isfile(chosen["path"]) and len(watcher.seen) == 1,
              "a .usda name is refused with a warning box (%r)"
              % watcher.seen)
        chosen["path"] = os.path.join(tmpdir, "hotkeyGroom")
        view.setFocus()
        viewport.setPointerInside(True)
        watcher = ModalWatcher()
        typeKey(view, "s", ("ctrl", "shift"))
        wait(50)
        watcher.stop()
        check(os.path.isfile(os.path.join(tmpdir, "hotkeyGroom.usdc")) and
              not watcher.seen,
              "Ctrl+Shift+S takes the same dialog and suffix")
    finally:
        QtWidgets.QFileDialog.getSaveFileName = originalSaveDialog
        container.setSaveGroomToScene(priorToScene)

    # ------------------------------------------------------------------
    # Walkthrough CONFUSING: the coverage row did nothing on a click.
    section("the coverage warnings row")
    workspace._warningsKey = None
    workspace.refresh()
    lst = workspace._warningsList
    rows = [lst.item(i) for i in range(lst.count())]
    info("warnings rows: %r" % ([r.text() for r in rows],))
    coverage = next((r for r in rows if "no region" in r.text()), None)
    check(coverage is not None and not workspace._warningsBox.isHidden(),
          "the uncovered quad centre shows a coverage row")
    if coverage is not None:
        check(coverage.data(QtCore.Qt.UserRole) is not None,
              "the coverage row carries a click action")
        rect = lst.visualItemRect(coverage)
        del messages[:]
        qtTest.mouseClick(lst.viewport(), QtCore.Qt.LeftButton,
                          QtCore.Qt.NoModifier, rect.center())
        wait(30)
        check(len(workspace.highlightedFaces()) > 0 and
              "outlined" in workspace._messageText,
              "clicking it outlines the uncovered faces (%r, %r)"
              % (workspace.highlightedFaces(), workspace._messageText))
        qtTest.mouseClick(lst.viewport(), QtCore.Qt.LeftButton,
                          QtCore.Qt.NoModifier,
                          lst.visualItemRect(coverage).center())
        wait(30)
        check(workspace.highlightedFaces() == [],
              "a second click clears the outline")

    # ------------------------------------------------------------------
    section("every mode and tool: an instruction line and an icon")
    for mode in pomadeModes.MODES:
        button = workspace.button("mode", mode.id)
        qtTest.mouseClick(button, QtCore.Qt.LeftButton)
        wait(30)
        workspace.refresh()
        check(state.activeMode == mode.id and
              bool(workspace._instructionLabel.text().strip()),
              "the %s button switches mode and shows an instruction (%r)"
              % (mode.id, workspace._instructionLabel.text()))
        subs = pomadeDockIds.SUBMODES.get(mode.id, ())
        kind = "sub"
        if mode.id == "tube":
            subs, kind = pomadeModes.TUBE_SUBMODES, "comp"
        for sub in subs:
            subButton = workspace.button(kind, sub.id)
            if subButton is None or not subButton.isVisible():
                check(False, "%s/%s has a visible dock button"
                      % (mode.id, sub.id))
                continue
            qtTest.mouseClick(subButton, QtCore.Qt.LeftButton)
            wait(15)
            workspace.refresh()
            check(bool(workspace._instructionLabel.text().strip()),
                  "%s/%s shows an instruction (%r)"
                  % (mode.id, sub.id, workspace._instructionLabel.text()))
    keys = [("mode", m.id) for m in pomadeModes.MODES]
    for subs in pomadeDockIds.SUBMODES.values():
        keys.extend(("sub", s.id) for s in subs)
    keys.extend(("comp", s.id) for s in pomadeModes.TUBE_SUBMODES)
    keys.extend(("tool", t.id) for t in pomadeDockIds.TRANSFORM_TOOLS)
    missing = []
    for kind, itemId in keys:
        button = workspace.button(kind, itemId)
        if button is None or button.icon().isNull():
            missing.append("%s:%s" % (kind, itemId))
    check(not missing, "every mode, sub-mode, component and tool button "
                       "has an icon (missing %r)" % missing)

    # ------------------------------------------------------------------
    section("close the dock, reopen it from the menu")
    typeKey(view, "6")
    wait(20)
    modeBefore = state.activeMode
    closeButton = workspace.findChild(QtWidgets.QAbstractButton,
                                      "qt_dockwidget_closebutton")
    check(closeButton is not None, "the dock has its title-bar close button")
    if closeButton is not None:
        qtTest.mouseClick(closeButton, QtCore.Qt.LeftButton)
    else:
        workspace.close()
    wait(50)
    check(not workspace.isVisible() and state.workspaceOpen is False and
          viewport.suspended,
          "the close button hid the dock and gave the view back")
    typeKey(view, "2")
    wait(20)
    check(state.activeMode == modeBefore,
          "mode keys are quiet while the dock is closed (%r)"
          % state.activeMode)
    action = menuAction(mainWindow, ("usdGen", "Pomade", "Open workspace"))
    action.trigger()
    wait(100)
    workspace = container.workspace
    session.setStatusSink(tee)
    check(workspace is not None and workspace.isVisible() and
          state.workspaceOpen and not viewport.suspended,
          "Open workspace re-shows the dock and retakes the view")
    check(session.scalpPath == "/Scalp" and session.model is not None and
          state.activeMode == modeBefore,
          "the binding and the mode survived the close")
    check(viewport.viewHasFocus(),
          "the keyboard focus is on the viewport after the reopen")
    viewport.setPointerInside(False)
    typeKey(view, "1")
    wait(20)
    check(state.activeMode == "graph",
          "hotkeys work straight after the reopen (%r)" % state.activeMode)

    # ------------------------------------------------------------------
    section("idle: the pill reads Synced")
    session.enqueueCommit()
    pill = pillSynced()
    check(pill == "Synced" and not workspace._statusAmber,
          "with nothing in flight the sync pill reads Synced (%r)" % pill)

    return finish()


# The run's shutdown(), registered once its fixture exists.
_CLEANUP = []


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view")
        return 1
    try:
        return run(appController)
    finally:
        for cleanup in list(_CLEANUP):
            cleanup()


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeArtistWalkthrough needs testusdview")
    sys.exit(0)
