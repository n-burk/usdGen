# pomadeT3Groom -- the one groom fixture TS-03's mode-coverage T3s share.
#
# Every one of testUsdviewPomadeGraphTools / Fill / HierarchyOps /
# TubeActions starts the same way the artist does: select /Scalp, open the
# workspace, bind it, then press-drag-release a Draw stroke round the
# middle square of examples/pomade-graph-scalp.usda, which leaves
# graphCounts() == (4, 4, 1) and a 3-CV L1 stub (G14). pomadeT3 (TS-01)
# supplies the drivers; this module only strings them into that fixture so
# the four scripts do not each grow their own copy of it.
#
# Nothing here asserts on the tool: a fixture step that fails is reported
# through pomadeT3.check like any other, so a broken bind shows up as a
# FAIL line in whichever script tripped on it.
from __future__ import annotations

import ctypes

import pomadeT3
from pomadeT3 import check, info, wait

# The scalp is the 4x4 quad grid in XZ at y = 0; the stroke is the middle
# square, which covers the four middle faces.
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))
STUB_CVS = 3


class Groom(object):
    """The live handles one T3 drives, plus the fixture helpers."""

    def __init__(self, appController):
        self.appController = appController
        self.dataModel = appController._dataModel
        self.stage = self.dataModel.stage
        self.view = appController._stageView
        self.registry = getattr(appController, "_plugRegistry", None)
        self.container = None
        self.session = None
        self.viewport = None
        self.state = None
        self.workspace = None
        self.mouse = None
        self.camera = None
        self.messages = pomadeT3.statusRecorder()

    # -- lifecycle ---------------------------------------------------------

    def open(self):
        """Top camera, open the workspace, bind /Scalp; True when live."""
        import usdGenPomadeTools
        from usdGenPomadeTools import pomadeCamera
        self.container = usdGenPomadeTools.container()
        if self.view is None or self.registry is None or \
                self.container is None:
            check(False, "usdview built a StageView and the Pomade plugin "
                         "registered")
            return False
        check(pomadeT3.frameScalp(self.stage, self.view),
              "the scene camera looks straight down at the scalp")
        self.view.setFocus()
        wait(50)
        session, viewport, state, workspace, container = \
            pomadeT3.openAndBind(self.appController, "/Scalp")
        self.session, self.viewport = session, viewport
        self.state, self.workspace = state, workspace
        live = (session is not None and session.model is not None and
                viewport is not None and viewport.installed and
                workspace is not None)
        check(live, "Bind scalp made a live model with the controller on "
                    "the StageView and the dock open")
        if not live:
            return False
        self._chainStatus()
        # The region tint is coincident with /Scalp; the model copied the
        # scalp at bind time, so K1 still hits it with the prim off.
        self.stage.GetPrimAtPath("/Scalp").SetActive(False)
        self.mouse = pomadeT3.Mouse(self.view)
        # A hover has no grab, so QtTest's global round trip can miss the
        # hidden test window; direct QMouseEvents go through the same
        # installed filters (pomadeT3.Mouse docstring).
        self.mouse.direct = True
        viewport.setPointerInside(True)
        self.view.update()
        wait(50)
        self.camera = pomadeCamera.resolve(self.view)
        check(self.camera is not None, "the controller's camera resolves")
        return self.camera is not None

    def _chainStatus(self):
        """Record every status line and still hand it to the dock.

        setStatusSink REPLACES the dock's own sink (DK-05); forwarding keeps
        the dock's message area live for the checks that read it.
        """
        previous = getattr(self.session, "_statusFn", None)
        recorder = self.messages

        def sink(text, level="info"):
            recorder(text, level)
            if previous is not None:
                try:
                    previous(text, level)
                except TypeError:
                    previous(text)

        self.session.setStatusSink(sink)

    def close(self):
        if self.viewport is not None:
            self.viewport.uninstall()
        if self.session is not None:
            self.session.deactivate()

    # -- cameras -----------------------------------------------------------

    def top(self):
        from usdGenPomadeTools import pomadeCamera
        pomadeT3.frameScalp(self.stage, self.view)
        self.view.setFocus()
        wait(40)
        self.camera = pomadeCamera.resolve(self.view)
        return self.camera

    def side(self, eye=(15.0, 2.1, 2.0), target=(2.0, 2.1, 2.0)):
        from usdGenPomadeTools import pomadeCamera
        pomadeT3.sideCamera(self.stage, self.view, eye=eye, target=target)
        self.view.setFocus()
        wait(40)
        self.camera = pomadeCamera.resolve(self.view)
        return self.camera

    def pixel(self, point):
        """Physical pixel of a world point under the current camera."""
        projected = self.camera.worldToPixels(point)
        return (projected[0], projected[1]) if projected is not None \
            else None

    def scalpPixel(self, x, z):
        return self.pixel((x, 0.0, z))

    # -- the stroke --------------------------------------------------------

    def stroke(self, corners=RECT, steps=5):
        """A real Draw press-drag-release round `corners` on the scalp."""
        from usdGenPomadeTools import pomadeCamera
        pomadeT3.typeKey(self.view, "d")
        self.view.update()
        wait(30)
        self.camera = pomadeCamera.resolve(self.view)
        perPixel = self.camera.worldPerPixel((2.0, 0.0, 2.0))
        self.state.snapRadiusPx = max(0.1 / max(perPixel, 1e-9), 2.0)
        path = []
        for k in range(len(corners)):
            x0, z0 = corners[k]
            x1, z1 = corners[(k + 1) % len(corners)]
            for i in range(steps):
                t = float(i) / float(steps)
                path.append(self.scalpPixel(x0 + (x1 - x0) * t,
                                            z0 + (z1 - z0) * t))
        path.append(self.scalpPixel(*corners[0]))
        self.mouse.drag(path)
        return self.session.graphCounts()

    def build(self):
        """Bind, then stroke RECT: (4, 4, 1) and the 3-CV stub."""
        if not self.open():
            return False
        counts = self.stroke()
        check(counts == (4, 4, 1),
              "the Draw stroke closed one 4-node region (%r)" % (counts,))
        cvs = self.centerCount(0)
        check(cvs == STUB_CVS,
              "G14: the region got its %d-CV stub (%d, status %r)"
              % (STUB_CVS, cvs, self.messages[-2:]))
        return counts == (4, 4, 1) and cvs == STUB_CVS

    # -- readers -----------------------------------------------------------

    def centerCount(self, tubeId):
        entry = getattr(self.session.dll, "Pomade_GetTubeCenterCount", None)
        if entry is None:
            return int(self.session.dll.Pomade_GetCenterCVCount(
                self.session.model)) if int(tubeId) == 0 else 0
        return int(entry(self.session.model, int(tubeId)))

    def centers(self, tubeId=0):
        return pomadeT3.tubeCenters(self.session, tubeId)

    def undoDepth(self):
        return int(self.session.undoDepth())

    def since(self, mark):
        """Status lines recorded after `mark` (len(messages) earlier)."""
        return [str(m) for m in self.messages[mark:]]

    def commit(self):
        """Enqueue and idle-pump until the committer drains."""
        self.session.enqueueCommit()
        return pomadeT3.pumpUntilCommitted(self.viewport, self.session,
                                          tries=120)

    def clickDock(self, kind, itemId):
        """A real left click on the dock button (kind, id); True if found."""
        from pxr.Usdviewq.qt import QtCore
        button = self.workspace.button(kind, itemId)
        if button is None or not button.isVisible() or \
                not button.isEnabled():
            info("dock button %s:%s is %s" % (
                kind, itemId, "missing" if button is None else
                "hidden" if not button.isVisible() else "disabled"))
            if button is None:
                return False
        pomadeT3._qtTest().QTest.mouseClick(
            button, QtCore.Qt.MouseButton.LeftButton)
        wait(20)
        return True

    def paramWidget(self, descriptorId):
        """(descriptor, widget) of the live dock form row, or (None, None)."""
        for descriptor, widget in getattr(self.workspace, "_paramWidgets",
                                          ()):
            if descriptor.id == descriptorId:
                return descriptor, widget
        return None, None

    def l1Tubes(self):
        ids = (ctypes.c_int * 64)()
        count = ctypes.c_int(0)
        if self.session.dll.Pomade_ReadL1TubeIds(
                self.session.model, ids, 64, ctypes.byref(count)) != 0:
            return []
        return [int(ids[i]) for i in range(count.value)]
