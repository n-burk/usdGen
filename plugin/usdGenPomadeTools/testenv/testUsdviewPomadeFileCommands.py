# testUsdviewPomadeFileCommands -- T3 acceptance of SS-03: a saved groom
# survives File > Reopen and comes back through Resume.
#
#   testusdview --testScript \
#       plugin/usdGenPomadeTools/testenv/testUsdviewPomadeFileCommands.py \
#       examples/pomade-single-quad.usda
#
# The scene is copied into a temp directory first: the root-layer save
# writes the scene file itself, and the example in the repo must never
# change. The script draws a region with real clicks, saves through the
# dock's Save button with "add the groom to the scene" on, checks the root
# layer (in memory and on disk) sublayers the .usdc, drops the live model,
# reopens the file and clicks the dock's Resume groom: the graph comes back
# and /PomadeGroom has one Guides prim. Binding the scalp over a saved groom
# then asks Resume / Start new / Cancel, and each answer does what it says.
# SS-04: the dock's Import with nothing selected adds locked tubes under the
# root tube as one undo step, and a file with a bad curve raises the error
# dialog and adds nothing.
import os
import shutil
import sys
import tempfile

# A counter-clockwise triangle inside the quad's only face (the same one
# testUsdviewPomadeReattach.py closes).
TRIANGLE = ((-0.82, -0.56), (-0.22, -0.56), (-0.52, 0.34))


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


def guidesPrims(stage, root="/PomadeGroom"):
    """Names of the Guides prims directly under the groom."""
    prim = stage.GetPrimAtPath(root)
    if not prim:
        return []
    return [str(c.GetName()) for c in prim.GetChildren()
            if str(c.GetName()).startswith("Guides")]


class PromptAnswer(object):
    """Press one button of the next modal Resume prompt from a QTimer.

    `buttonName` is a button's objectName, "cancel" (reject) or "accept"
    (OK on a warning box); `seen`/`texts` record each box it answered.

    QMessageBox.exec_ blocks in its own event loop, so the answer has to
    be queued before the call that raises the box.
    """

    def __init__(self, buttonName):
        self.buttonName = buttonName
        self.seen = []
        self.texts = []
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
            if not isinstance(widget, QtWidgets.QMessageBox) or \
                    not widget.isVisible():
                continue
            self.seen.append(str(widget.objectName()))
            self.texts.append(str(widget.text()))
            self.active = False
            if self.buttonName == "cancel":
                widget.reject()
                return
            if self.buttonName == "accept":
                # The warning box's only button is OK.
                widget.accept()
                return
            button = widget.findChild(QtWidgets.QAbstractButton,
                                      self.buttonName)
            if button is not None:
                button.click()
            else:
                widget.reject()
            return
        self._arm()

    def stop(self):
        self.active = False


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..", "..",
                                                         "..", "tools", "docs")))
    try:
        import usdGenPomadeTools
        from pxr import Sdf, Usd, UsdGeom
        from pxr.Usdviewq.qt import QtCore, QtWidgets
        from usdGenPomadeTools import pomadeBridge, pomadeCamera, pomadeHierarchy
        from pomadeT3 import (Mouse, check, failureCount, frameScalp,
                             guideCount, info, isolateSettings, openAndBind,
                             pumpUntilCommitted, realSettingsSnapshot,
                             statusRecorder, typeKey, wait)
        from pomade_ui_capture import capture as captureUi
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools/pomadeT3: %s" % exc)
        return 1

    dataModel = appController._dataModel
    view = appController._stageView
    container = usdGenPomadeTools.container()
    originalFile = appController._parserData.usdFile
    tempDir = tempfile.mkdtemp(prefix="pomadeFileCommands")
    scenePath = os.path.join(tempDir, "scene.usda")
    shutil.copyfile(originalFile, scenePath)
    # Every dialog the dock opens remembers its folder, and the Save option
    # is remembered too: both go to an .ini in the temp folder, never the
    # artist's own QSettings, whatever happens to this run.
    restoreSettings = isolateSettings(container, tempDir)
    priorToScene = container.saveGroomToScene()
    originalDialog = QtWidgets.QFileDialog.getSaveFileName
    originalOpenDialog = QtWidgets.QFileDialog.getOpenFileName

    def dropModel():
        """What quitting usdview does to the model: nothing is left live."""
        viewport = getattr(container, "viewport", None)
        session = getattr(container, "session", None)
        try:
            if viewport is not None and viewport.installed:
                viewport.uninstall()
        finally:
            if session is not None:
                session.deactivate()

    def shutdown():
        if not _CLEANUP:
            return                      # already ran (idempotent)
        del _CLEANUP[:]
        try:
            dropModel()
        finally:
            QtWidgets.QFileDialog.getSaveFileName = originalDialog
            QtWidgets.QFileDialog.getOpenFileName = originalOpenDialog
            container.setSaveGroomToScene(priorToScene)
            check(restoreSettings(),
                  "the artist's own Pomade QSettings are untouched by the "
                  "run (%r)" % (realSettingsSnapshot(container),))
            appController._parserData.usdFile = originalFile
            shutil.rmtree(tempDir, ignore_errors=True)

    # testUsdviewInputFunction runs this in a finally: an exception in the
    # body still drops the model, the patched dialogs and the temp folder.
    _CLEANUP.append(shutdown)

    def reopen(path):
        """File > Open's own path: point usdview at `path` and reopen."""
        appController._parserData.usdFile = path
        appController._reopenStage()
        wait(50)
        return dataModel.stage

    stage = reopen(scenePath)
    check(stage is not None and bool(stage.GetPrimAtPath("/Scalp")),
          "usdview opened the temp copy of the scene")
    check(frameScalp(stage, view, eye=(0.0, 7.0, 0.0)),
          "the top camera is active")
    view.setFocus()
    wait(30)

    session, viewport, state, workspace, _c = openAndBind(appController,
                                                          "/Scalp")
    check(session is not None and session.model is not None and
          viewport is not None and workspace is not None,
          "binding /Scalp creates the live model and the dock")
    if session is None or session.model is None or viewport is None or \
            workspace is None:
        shutdown()
        return 1
    api = workspace._api
    messages = statusRecorder()
    session.setStatusSink(messages)

    # -- draw one region the artist's way ---------------------------------
    view.setFocus()
    wait(10)
    typeKey(view, "r")
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the viewport controller camera resolves")
    if camera is None:
        shutdown()
        return 1
    state.snapRadiusPx = max(
        0.05 / max(camera.worldPerPixel((0.0, 0.0, 0.0)), 1e-9), 2.0)
    mouse = Mouse(view)
    viewport.setPointerInside(True)
    mouse.direct = True
    for x, z in TRIANGLE:
        mouse.click(camera.worldToPixels((x, 0.0, z)))
    typeKey(view, "return")
    check(session.graphCounts()[2] == 1,
          "three clicks and Enter close one region (%r)"
          % (session.graphCounts(),))
    check(pumpUntilCommitted(viewport, session),
          "the region commits to the stage")
    counts = session.graphCounts()
    guides = guideCount(session)
    info("before save: counts %r guides %d guide prims %r"
         % (counts, guides, guidesPrims(stage)))

    # -- Save through the dock with "add to the scene" on ------------------
    groomFile = os.path.join(tempDir, "hair.usdc")
    calls = []

    def fakeDialog(*args, **kwargs):
        calls.append(args)
        return os.path.join(tempDir, "hair"), "USD crate (*.usdc)"

    container.setSaveGroomToScene(True)
    check(container.saveGroomToScene(),
          "the save-to-scene option reads back on")
    del messages[:]
    closer = PromptAnswer("cancel")      # a failure box must not hang
    QtWidgets.QFileDialog.getSaveFileName = fakeDialog
    try:
        save = workspace.button("file", "save")
        check(save is not None and save.isEnabled(),
              "the dock's Save button is live once bound")
        if save is not None:
            save.click()
        wait(20)
    finally:
        QtWidgets.QFileDialog.getSaveFileName = originalDialog
        closer.stop()
        container.setSaveGroomToScene(priorToScene)
    check(not closer.seen, "the save raised no warning box (%r)"
          % closer.seen)
    remembered = str(container._settings().value(container._DIALOG_DIR_KEY,
                                                 "") or "")
    check(os.path.normcase(os.path.abspath(remembered)) ==
          os.path.normcase(os.path.abspath(tempDir)) and
          realSettingsSnapshot(container) == restoreSettings.before,
          "the dialog folder is remembered in the test's own settings "
          "file, not the artist's (%r)" % remembered)
    check(len(calls) == 1 and os.path.isfile(groomFile),
          "the dock save of a bare 'hair' writes hair.usdc")
    rootSubs = list(stage.GetRootLayer().subLayerPaths)
    check("./hair.usdc" in rootSubs,
          "the root layer sublayers the groom by a relative path (%r)"
          % rootSubs)
    onDisk = Sdf.Layer.OpenAsAnonymous(scenePath)
    check(onDisk is not None and
          "./hair.usdc" in list(onDisk.subLayerPaths),
          "and the scene FILE on disk says so too")
    check(not any(p.endswith("hair.usdc")
                  for p in stage.GetSessionLayer().subLayerPaths),
          "the groom is not ALSO sublayered in the session layer")
    check(any("saved" in m and "added it to scene.usda" in m
              for m in messages),
          "the status line says where the groom went (%r)"
          % list(messages)[-1:])

    # -- quit, reopen, Resume ---------------------------------------------
    dropModel()
    workspace.refresh()
    check(session.model is None, "the live model is gone")
    stage = reopen(scenePath)
    workspace.refresh()
    check(bool(stage.GetPrimAtPath("/PomadeGroom")),
          "the reopened scene composes the saved /PomadeGroom")
    check(container.canResumeGroom(api),
          "canResumeGroom sees the saved groom")
    resume = workspace.button("file", "resume")
    check(resume is not None and resume.isVisible() and resume.isEnabled(),
          "the dock shows Resume groom")
    check(frameScalp(stage, view, eye=(0.0, 7.0, 0.0)),
          "the reopened scalp is framed for the resume screenshot")
    captureUi(appController, "pomade-13-resume-offered")
    session.setStatusSink(messages)
    del messages[:]
    if resume is not None:
        resume.click()
        wait(30)
    session = container.session
    viewport = container.viewport
    check(session is not None and session.model is not None,
          "Resume groom hydrates a live model (%r)" % list(messages)[-2:])
    if session is None or session.model is None:
        shutdown()
        return 1
    check(session.graphCounts() == counts,
          "the resumed graph matches the saved one (%r vs %r)"
          % (session.graphCounts(), counts))
    check(session.scalpPath == "/Scalp",
          "the resumed groom is bound to /Scalp (%r)" % session.scalpPath)
    check(viewport is not None and viewport.installed,
          "Resume puts the viewport tool back")
    captureUi(appController, "pomade-14-resumed-groom")
    check(pumpUntilCommitted(viewport, session),
          "the resumed model commits over the saved groom")
    prims = guidesPrims(stage)
    check(prims == ["Guides"] if guides else len(prims) <= 1,
          "/PomadeGroom holds one Guides prim, not a duplicate (%r)" % prims)
    check(session.hydratedCounts[1] == guides,
          "the saved guides hydrate back (%d vs %d)"
          % (session.hydratedCounts[1], guides))
    check(not container.canResumeGroom(api),
          "Resume is not offered while the model is live")
    workspace.refresh()
    check(resume is None or not resume.isVisible(),
          "and the dock hides the Resume button")

    # -- Bind over a saved groom asks Resume / Start new / Cancel ----------
    dropModel()
    workspace.refresh()
    answer = PromptAnswer("cancel")
    result = container.bindGeometry(api, "/Scalp")
    answer.stop()
    check(answer.seen == ["pomadeResumePrompt"] and result is None and
          container.session.model is None,
          "Cancel on the prompt binds nothing (%r, %r)"
          % (answer.seen, result))

    # DOC-01: the same Cancel through the dock's own Bind button, a real
    # click. The dock used to treat the cancelled bind (None) as a failure
    # and raise a "could not bind" error box on top of it. Every modal is
    # answered from a timer (reject the prompt, OK anything after it) so a
    # regression records the extra box instead of hanging the run.
    class ModalLog(object):
        def __init__(self):
            self.boxes = []
            self.active = True
            QtCore.QTimer.singleShot(40, self._poll)

        def _poll(self):
            if not self.active:
                return
            modal = QtWidgets.QApplication.activeModalWidget()
            if isinstance(modal, QtWidgets.QMessageBox) and \
                    modal.isVisible():
                self.boxes.append((str(modal.objectName()),
                                   str(modal.windowTitle())))
                if str(modal.objectName()) == "pomadeResumePrompt":
                    modal.reject()
                else:
                    modal.accept()
            QtCore.QTimer.singleShot(40, self._poll)

    import importlib
    from pxr.Usdviewq.qt import PySideModule
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    workspace.refresh()
    bindButton = workspace.button("file", "bind")
    modals = ModalLog()
    if bindButton is not None:
        QtTest.QTest.mouseClick(bindButton, QtCore.Qt.LeftButton)
    wait(100)
    modals.active = False
    check(bindButton is not None and
          [name for name, _title in modals.boxes] == ["pomadeResumePrompt"]
          and container.session.model is None,
          "Cancel on the dock's Bind prompt raises no error box and binds "
          "nothing (%r)" % (modals.boxes,))

    answer = PromptAnswer("pomadeResumeButton")
    result = container.bindGeometry(api, "/Scalp")
    answer.stop()
    session = container.session
    check(answer.seen == ["pomadeResumePrompt"] and result is True and
          session.model is not None and session.graphCounts() == counts,
          "Resume on the prompt hydrates the saved graph (%r, %r)"
          % (answer.seen, session.graphCounts()))

    # -- SS-04: dock Import is atomic and honest ---------------------------
    def writeCurves(path, curves):
        out = Usd.Stage.CreateNew(path)
        prim = UsdGeom.BasisCurves.Define(out, "/Strands")
        prim.CreateTypeAttr(UsdGeom.Tokens.linear)
        prim.CreatePointsAttr([p for curve in curves for p in curve])
        prim.CreateCurveVertexCountsAttr([len(curve) for curve in curves])
        out.GetRootLayer().Save()

    def importThroughDock(path, answerWith):
        """Click the dock's Import with the open dialog answering `path`."""
        opened = []

        def fakeOpen(*args, **kwargs):
            opened.append(args)
            return path, "USD (*.usda *.usdc *.usd)"

        box = PromptAnswer(answerWith)
        QtWidgets.QFileDialog.getOpenFileName = fakeOpen
        try:
            button = workspace.button("file", "import")
            if button is not None:
                button.click()
            wait(20)
        finally:
            QtWidgets.QFileDialog.getOpenFileName = originalOpenDialog
            box.stop()
        return opened, box

    if session.model is not None:
        strand = [(0.3, 0.0, 0.3), (0.3, 0.3, 0.35), (0.3, 0.6, 0.4)]
        goodCurves = os.path.join(tempDir, "strands.usda")
        writeCurves(goodCurves, [strand, [(0.6, 0.0, -0.4),
                                          (0.6, 0.3, -0.35),
                                          (0.6, 0.6, -0.3)]])
        badCurves = os.path.join(tempDir, "oneTooShort.usda")
        writeCurves(badCurves, [strand, [(0.6, 0.0, -0.4)]])
        session.setStatusSink(messages)
        session.clearSelection()
        before = pomadeBridge.readTubeIds(session.dll, session.model)
        depth = session.undoDepth()
        info("before import: tubes %r undo depth %d" % (before, depth))
        check(0 in before and container.importParentTubeId() == 0,
              "with nothing selected the import parent is the root tube 0 "
              "(%r)" % (before,))

        del messages[:]
        opened, box = importThroughDock(goodCurves, "cancel")
        after = pomadeBridge.readTubeIds(session.dll, session.model)
        added = [t for t in after if t not in before]
        check(len(opened) == 1 and not box.seen,
              "the dock Import opens one dialog and raises no warning (%r)"
              % (box.texts,))
        check(len(added) == 2 and
              all(pomadeHierarchy.tubeParent(session.dll, session.model, t)
                  == 0 for t in added) and
              all(pomadeBridge.isTubeImported(session.dll, session.model, t)
                  for t in added),
              "two curves land as two locked tubes under the root (%r)"
              % (added,))
        check(session.undoDepth() == depth + 1,
              "the import is one undo step (%d -> %d)"
              % (depth, session.undoDepth()))
        check(any("Imported 2 curves under tube 0" in m for m in messages),
              "the status says 'Imported 2 curves under tube 0' (%r)"
              % list(messages)[-1:])

        del messages[:]
        opened, box = importThroughDock(badCurves, "accept")
        again = pomadeBridge.readTubeIds(session.dll, session.model)
        check(len(opened) == 1 and len(box.seen) == 1 and
              any("curve 1" in text for text in box.texts),
              "a bad file shows the error dialog naming the bad curve (%r)"
              % (box.texts,))
        check(again == after and session.undoDepth() == depth + 1,
              "and adds nothing: no tube, no undo step (%r -> %r)"
              % (after, again))
        check(any("cannot import" in m for m in messages),
              "the status line says the import failed (%r)"
              % list(messages)[-1:])

    dropModel()
    workspace.refresh()
    answer = PromptAnswer("pomadeStartNewButton")
    result = container.bindGeometry(api, "/Scalp")
    answer.stop()
    session = container.session
    check(answer.seen == ["pomadeResumePrompt"] and result is True and
          session.model is not None and
          session.graphCounts() == (0, 0, 0),
          "Start new binds an empty groom over it (%r, %r)"
          % (answer.seen, session.graphCounts()))

    shutdown()
    return 1 if failureCount() else 0


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
    print("SKIP: testUsdviewPomadeFileCommands needs testusdview")
    sys.exit(0)
