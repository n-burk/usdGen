# tonicT3 -- the shared driver module every Tonic T3 (testusdview) script
# imports (TS-01).  Before this, Mouse/typeKey/frameScalp/... were copied
# and drifted across six scripts with divergent key tables (Tube L79-219
# vs Workflow L80-107).  This module is the single source of truth for
# them; existing T3 scripts are left untouched (they carry on with their
# own copies) and TS-02/03/04 import from here instead of copying again.
#
# Nothing at module scope touches pxr or Qt -- every function that needs
# either imports it locally, the same discipline tonicViewport.py itself
# uses -- so this module loads fine in a plain interpreter (the T1 half
# below, testUsdGenTonicToolsT3Helpers.py, does exactly that) and only
# pays for pxr/Qt when a T3 actually calls into them.
from __future__ import annotations

import ctypes
import os
import sys
import tempfile

# ---------------------------------------------------------------------------
# check / info counters
# ---------------------------------------------------------------------------
#
# One counter per process: every T3 is its own testusdview subprocess, so a
# module-level counter never leaks between scripts the way a shared object
# would need guarding against.

_failures = 0


def check(ok, what):
    """Print `ok:`/`FAIL:` the way every existing T3 does, and count it."""
    global _failures
    if ok:
        print("ok:   %s" % what)
    else:
        _failures += 1
        print("FAIL: %s" % what)
    return ok


def info(text):
    print("info: %s" % text)


def failureCount():
    return _failures


def resetFailures():
    """Zero the shared counter. Only needed when one process runs more
    than one check() pass (the T1 half below does, per-section)."""
    global _failures
    _failures = 0


# ---------------------------------------------------------------------------
# Qt plumbing
# ---------------------------------------------------------------------------

def _qtTest():
    from pxr.Usdviewq.qt import PySideModule
    import importlib
    return importlib.import_module("%s.QtTest" % PySideModule)


def wait(ms=30):
    _qtTest().QTest.qWait(int(ms))


class Mouse:
    """QtTest/direct mouse events on the StageView, in its own pixel space.

    `direct = True` switches to synthetic QMouseEvents sent straight at the
    widget: QTest's global-coordinate round trip can miss a hidden test
    window's child widget entirely, and a missed press would look like a
    tool bug (Tube L106-121). `press`/`move`/`release`/... all take PHYSICAL
    pixels. `button` is "left" (the default everywhere today), "middle" or
    "right" -- FB-03/TS-02's MMB/RMB navigation needs those; `modifiers`
    is any of "shift"/"ctrl"/"alt"/"meta".
    """

    _BUTTON_NAMES = {"left": "LeftButton", "middle": "MiddleButton",
                     "right": "RightButton"}
    _MODIFIER_NAMES = {"shift": "ShiftModifier", "ctrl": "ControlModifier",
                       "alt": "AltModifier", "meta": "MetaModifier"}

    def __init__(self, view):
        from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
        self._view = view
        self._QtCore = QtCore
        self._QtGui = QtGui
        self._QtWidgets = QtWidgets
        try:
            self._ratio = float(view.devicePixelRatioF())
        except AttributeError:
            self._ratio = 1.0
        self.direct = False          # set when QtTest delivery does not land
        self._held = set()           # buttons currently down (for move()'s
                                      # persistent button-state field)

    def _point(self, physical):
        return self._QtCore.QPoint(int(round(physical[0] / self._ratio)),
                                   int(round(physical[1] / self._ratio)))

    def _button(self, name):
        return getattr(self._QtCore.Qt.MouseButton, self._BUTTON_NAMES[name])

    def _modifiers(self, names):
        mods = self._QtCore.Qt.KeyboardModifier.NoModifier
        for name in names:
            mods |= getattr(self._QtCore.Qt.KeyboardModifier,
                            self._MODIFIER_NAMES[name])
        return mods

    def _heldMask(self):
        mask = self._QtCore.Qt.MouseButton.NoButton
        for name in self._held:
            mask |= self._button(name)
        return mask

    def _send(self, kind, physical, mods, button, buttons=None):
        """One synthetic event straight at the widget (see class docstring
        for why `direct` exists). QPointF is QtCore's in PySide2/PySide6."""
        point = self._point(physical)
        local = self._QtCore.QPointF(point)
        globalPos = self._QtCore.QPointF(self._view.mapToGlobal(point))
        if buttons is None:
            buttons = button
        event = self._QtGui.QMouseEvent(kind, local, globalPos, button,
                                        buttons, mods)
        self._QtWidgets.QApplication.sendEvent(self._view, event)

    def press(self, physical, modifiers=(), button="left"):
        from pxr.Usdviewq.qt import QtCore
        btn = self._button(button)
        mods = self._modifiers(modifiers)
        self._held.add(button)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonPress, physical, mods,
                       btn, self._heldMask())
        else:
            _qtTest().QTest.mousePress(self._view, btn, mods,
                                       self._point(physical))

    def move(self, physical, modifiers=()):
        from pxr.Usdviewq.qt import QtCore
        mods = self._modifiers(modifiers)
        # QTest.mouseMove reports NoButton even after QTest.mousePress in
        # this Qt build. A held drag must carry NoButton as the changed
        # button and whichever buttons are actually down in the persistent
        # button-state field.
        self._send(QtCore.QEvent.Type.MouseMove, physical, mods,
                   QtCore.Qt.MouseButton.NoButton, self._heldMask())

    def unheldMove(self, physical, modifiers=()):
        """A real MouseMove reporting lost button capture."""
        from pxr.Usdviewq.qt import QtCore
        mods = self._modifiers(modifiers)
        self._send(QtCore.QEvent.Type.MouseMove, physical, mods,
                   QtCore.Qt.MouseButton.NoButton,
                   QtCore.Qt.MouseButton.NoButton)

    def focusOut(self):
        """Send a keyboard-focus event without ending a held mouse drag."""
        event = self._QtCore.QEvent(self._QtCore.QEvent.Type.FocusOut)
        self._QtWidgets.QApplication.sendEvent(self._view, event)

    def ungrabMouse(self):
        """Send the capture-loss event that has no matching release."""
        event = self._QtCore.QEvent(self._QtCore.QEvent.Type.UngrabMouse)
        self._QtWidgets.QApplication.sendEvent(self._view, event)
        self._held.clear()

    def release(self, physical, modifiers=(), button="left"):
        from pxr.Usdviewq.qt import QtCore
        btn = self._button(button)
        mods = self._modifiers(modifiers)
        self._held.discard(button)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonRelease, physical, mods,
                       btn, self._heldMask())
        else:
            _qtTest().QTest.mouseRelease(self._view, btn, mods,
                                         self._point(physical))

    def click(self, physical, modifiers=(), button="left"):
        self.press(physical, modifiers, button)
        self.release(physical, modifiers, button)

    def doubleClick(self, physical, modifiers=(), button="left"):
        """Deliver Qt's distinct double-click press followed by release."""
        from pxr.Usdviewq.qt import QtCore
        btn = self._button(button)
        mods = self._modifiers(modifiers)
        if self.direct:
            self._send(QtCore.QEvent.Type.MouseButtonDblClick, physical,
                       mods, btn)
            self.release(physical, modifiers, button)
        else:
            _qtTest().QTest.mouseDClick(self._view, btn, mods,
                                        self._point(physical))

    def drag(self, points, modifiers=(), button="left"):
        self.press(points[0], modifiers, button)
        for point in points[1:]:
            self.move(point, modifiers)
        self.release(points[-1], modifiers, button)


# Every name tonicViewport.keyName can hand back to HotkeyAction: the
# `named` dict's values (escape/delete/backspace/enter/up/down/left/right/
# f8-f11/brackets), the Key_A..Key_Z range, and the digit row (reached
# through QKeyEvent.text() there, so not literally in that dict, but a
# real key nonetheless). testUsdGenTonicToolsT3Helpers.py checks this table
# is a superset of tonicViewport.py's `named` dict by reading its source,
# so keep new tonicViewport key names added here too.
_KEY_TABLE = {
    "escape": "Key_Escape", "delete": "Key_Delete",
    "backspace": "Key_Backspace",
    "enter": "Key_Return", "return": "Key_Return",
    "up": "Key_Up", "down": "Key_Down", "left": "Key_Left",
    "right": "Key_Right",
    "[": "Key_BracketLeft", "]": "Key_BracketRight",
    "bracketleft": "Key_BracketLeft", "bracketright": "Key_BracketRight",
}
for _i in range(1, 13):
    _KEY_TABLE["f%d" % _i] = "Key_F%d" % _i
for _digit in "0123456789":
    _KEY_TABLE[_digit] = "Key_%s" % _digit
for _letter in "abcdefghijklmnopqrstuvwxyz":
    _KEY_TABLE[_letter] = "Key_%s" % _letter.upper()
del _i, _digit, _letter


def typeKey(view, name, modifiers=()):
    """One key press through QtTest, seen by the app-level `_KeyFilter`.

    `name` is whatever `tonicViewport.keyName` would turn the resulting
    QKeyEvent back into: a digit, a lowercase letter, f1-f12, delete,
    backspace, enter (or return), escape, up/down/left/right, or a bracket
    (spelled either `[`/`]` or `bracketleft`/`bracketright`).
    """
    if name not in _KEY_TABLE:
        raise KeyError("tonicT3.typeKey: no Qt key mapped for %r" % (name,))
    from pxr.Usdviewq.qt import QtCore
    key = getattr(QtCore.Qt.Key, _KEY_TABLE[name])
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    table = {"shift": QtCore.Qt.KeyboardModifier.ShiftModifier,
             "ctrl": QtCore.Qt.KeyboardModifier.ControlModifier,
             "alt": QtCore.Qt.KeyboardModifier.AltModifier,
             "meta": QtCore.Qt.KeyboardModifier.MetaModifier}
    for modifier in modifiers:
        mods |= table[modifier]
    _qtTest().QTest.keyClick(view, key, mods)


# ---------------------------------------------------------------------------
# Cameras
# ---------------------------------------------------------------------------

def _basisMatrix(eye, xAxis, yAxis, zAxis):
    from pxr import Gf
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    return mat


def _placeCamera(stage, view, primPath, mat, focalLength=35.0):
    from pxr import Gf, Sdf, UsdGeom
    cam = UsdGeom.Camera.Define(stage, Sdf.Path(primPath))
    cam.CreateFocalLengthAttr(focalLength)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = None
    for candidate in xf.GetOrderedXformOps():
        if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform:
            op = candidate
            break
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(primPath)
    return view.getActiveSceneCamera() is not None


def frameScalp(stage, view, eye=(2.0, 12.0, 2.0),
              primPath="/TonicT3TopCamera"):
    """Straight down over an XZ scalp (Tube L230-261 / CvRegions L91-120).

    Straight down is also straight along a G14 tube stub's own axis (its
    region's mean normal is +Y), which is what makes center CVs readable
    through the open, cap-less tip.
    """
    from pxr import Gf
    zAxis = Gf.Vec3d(0.0, 1.0, 0.0)
    xAxis = Gf.Vec3d(1.0, 0.0, 0.0)
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = _basisMatrix(Gf.Vec3d(*eye), xAxis, yAxis, zAxis)
    return _placeCamera(stage, view, primPath, mat)


def obliqueCamera(stage, view, eye=(5.5, 12.0, 2.0), target=(2.0, 2.0, 2.0),
                  up=(0.0, 0.0, 1.0), primPath="/TonicT3ObliqueCamera"):
    """Slightly off dead-centre so overlapping CVs stay separable on screen
    while still seeing through an open tube tip (Selection.py L16-48)."""
    from pxr import Gf
    eyePt, targetPt = Gf.Vec3d(*eye), Gf.Vec3d(*target)
    zAxis = (eyePt - targetPt).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(*up), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = _basisMatrix(eyePt, xAxis, yAxis, zAxis)
    return _placeCamera(stage, view, primPath, mat)


def sideCamera(stage, view, eye=(15.0, 2.1, 2.0), target=(2.0, 2.1, 2.0),
              up=(0.0, 1.0, 0.0), primPath="/TonicT3SideCamera"):
    """Round to the side, so sibling tubes or a lateral drag span the frame
    apart (Workflow.aimSideCamera L286-313)."""
    from pxr import Gf
    eyePt, targetPt = Gf.Vec3d(*eye), Gf.Vec3d(*target)
    zAxis = (eyePt - targetPt).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(*up), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = _basisMatrix(eyePt, xAxis, yAxis, zAxis)
    return _placeCamera(stage, view, primPath, mat)


def grabFrame(view):
    """Redraw and hand back the framebuffer image."""
    view.update()
    view.repaint()
    view.updateGL()
    return view.grabFrameBuffer()


def whiteFraction(view, camera, point, halfPx=6):
    """Fraction of near-white pixels in a small window around `point`.

    A selected CV dot publishes as white (plan/18 section 2.4a); every
    other thing in frame -- clump colour, region tint, grey backdrop -- is
    either coloured or dark, so "white" is the one unambiguous reading.
    """
    image = grabFrame(view)
    projected = camera.worldToPixels(point)
    if projected is None:
        return 0.0
    width, height = image.width(), image.height()
    white = 0
    total = 0
    for dy in range(-halfPx, halfPx + 1):
        for dx in range(-halfPx, halfPx + 1):
            px = int(min(max(projected[0] + dx, 0), width - 1))
            py = int(min(max(projected[1] + dy, 0), height - 1))
            rgb = image.pixel(px, py)
            r = (rgb >> 16) & 0xFF
            g = (rgb >> 8) & 0xFF
            b = rgb & 0xFF
            total += 1
            if min(r, g, b) > 200 and (max(r, g, b) - min(r, g, b)) < 30:
                white += 1
    return float(white) / float(total) if total else 0.0


# ---------------------------------------------------------------------------
# Bind / session lifecycle
# ---------------------------------------------------------------------------

def openAndBind(appController, scalpPath, viaDock=False):
    """Select `scalpPath`, open the workspace and bind it.

    The default route matches every existing T3: select the prim, then run
    the openWorkspace/bindScalp command plugins directly. `viaDock=True`
    instead drives the dock's own Bind Geometry button and its modal mesh
    picker (CvRegions L219-289), which is the route DK-01's first-run gate
    expects an artist to take.

    Returns `(session, viewport, state, workspace, container)`; any of the
    first three come back None if bind did not produce a live model, which
    callers must check before using them.
    """
    import usdGenTonicTools
    dataModel = appController._dataModel
    registry = appController._plugRegistry
    container = usdGenTonicTools.container()
    dataModel.selection.setPrimPath(scalpPath)
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    workspace = container.workspace
    if viaDock:
        bindGeometryFromDock(workspace, scalpPath)
    else:
        registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    return session, viewport, state, workspace, container


def bindGeometryFromDock(workspace, scalpPath):
    """Drive the dock's Bind Geometry button and its modal mesh picker.

    The picker accepts a list, tree, combo or path edit (CvRegions
    L219-260); this expresses the artist contract -- choose `scalpPath`,
    then press the affirmative button -- rather than any one concrete
    widget. Returns True once the picker accepted the path.
    """
    from pxr.Usdviewq.qt import QtCore, QtWidgets
    qtTest = _qtTest()
    # DK-04's stable (kind, id) hook first: DK-01 relabelled the button
    # 'Bind scalp mesh...', so the old text match no longer finds it.
    lookup = getattr(workspace, "button", None)
    button = lookup("file", "bind") if callable(lookup) else None
    if button is None:
        for candidate in workspace.findChildren(QtWidgets.QPushButton):
            text = candidate.text().lower()
            if "bind geometry" in text or "bind scalp" in text:
                button = candidate
                break
    if button is None:
        return False
    result = {"dialog": False, "chosen": False}

    # QDialog.exec_() runs its own nested event loop synchronously inside
    # the button callback below, so the interaction has to be queued before
    # the click that opens it, not written after.
    def choose():
        dialog = QtWidgets.QApplication.activeModalWidget()
        result["dialog"] = isinstance(dialog, QtWidgets.QDialog)
        if not result["dialog"]:
            return
        combo = dialog.findChild(QtWidgets.QComboBox, "tonicGeometryChoices")
        if combo is not None:
            index = combo.findText(scalpPath, QtCore.Qt.MatchExactly)
            if index >= 0:
                combo.setCurrentIndex(index)
                result["chosen"] = True
        box = dialog.findChild(QtWidgets.QDialogButtonBox,
                               "tonicGeometryPickerButtons")
        ok = box.button(QtWidgets.QDialogButtonBox.Ok) if box else None
        if result["chosen"] and ok is not None:
            qtTest.QTest.mouseClick(ok, QtCore.Qt.MouseButton.LeftButton)
        elif isinstance(dialog, QtWidgets.QDialog):
            dialog.reject()

    QtCore.QTimer.singleShot(0, choose)
    qtTest.QTest.mouseClick(button, QtCore.Qt.MouseButton.LeftButton)
    wait(50)
    return bool(result["chosen"])


def pumpUntilCommitted(viewport, session, tries=60):
    for _ in range(tries):
        viewport.pumpOnce()
        if not session.hasPendingWork():
            return True
        wait(25)
    return False


class _StatusRecorder(list):
    """A callable status sink that also reads like the plain list every
    existing T3 passes as `messages.append`.

    Accepts today's `setStatusSink` calling convention, `sink(text)`, and
    SS-02's planned `sink(text, level)` -- either way the list itself (and
    `messages[-1]`) holds the text alone; `.entries` keeps the full
    `(text, level)` pairs for a script that cares about level.
    """

    def __init__(self):
        list.__init__(self)
        self.entries = []

    def __call__(self, text, level=None):
        self.entries.append((text, level))
        self.append(text)


def statusRecorder():
    """A fresh sink for `session.setStatusSink(...)` -- see
    `_StatusRecorder`."""
    return _StatusRecorder()


# ---------------------------------------------------------------------------
# Model / dock readers
# ---------------------------------------------------------------------------

def levelInfo(session, level):
    """(faces, points, tubes) the scene index published for `level`."""
    faces = ctypes.c_int(0)
    points = ctypes.c_int(0)
    tubes = ctypes.c_int(0)
    rc = session.dll.Tonic_GetPublishedLevelInfo(
        session.model, int(level), ctypes.byref(faces), ctypes.byref(points),
        ctypes.byref(tubes))
    if rc != 0:
        return None
    return (faces.value, points.value, tubes.value)


def tubeCenters(session, tubeId, limit=64):
    """Every center CV of one tube, stopping at the first miss."""
    points = []
    for cv in range(limit):
        out = (ctypes.c_float * 3)()
        if session.dll.Tonic_GetTubeCenterCV(session.model, int(tubeId),
                                             int(cv), out) != 0:
            break
        points.append((float(out[0]), float(out[1]), float(out[2])))
    return points


def guideCount(session):
    guides = ctypes.c_int(0)
    session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                     None)
    return int(guides.value)


def gizmoKind(session, viewport=None):
    """Read the controller-owned gizmo when the Qt overlay is active.

    The native scene-index record is deliberately cleared while usdview
    draws the unoccluded transparent overlay, so this is a suppression
    check rather than the source of an interactive gizmo's state.
    """
    loop = getattr(viewport, "loop", None) if viewport is not None else None
    gizmo = getattr(loop, "_gizmo", None)
    if gizmo is not None and gizmo.visible:
        return (int(gizmo.kind), tuple(float(value) for value in gizmo.origin))
    kind = ctypes.c_int(0)
    origin = (ctypes.c_float * 3)()
    frameArr = (ctypes.c_float * 9)()
    size = ctypes.c_float(0.0)
    handle = ctypes.c_int(0)
    if session.dll.Tonic_GetGizmo(session.model, ctypes.byref(kind), origin,
                                  frameArr, ctypes.byref(size),
                                  ctypes.byref(handle)) != 0:
        return (0, None)
    return (int(kind.value),
            (float(origin[0]), float(origin[1]), float(origin[2])))


def dockButton(workspace, label):
    """The dock's visible QAbstractButton whose text (before any trailing
    " (hotkey)" suffix) equals `label`, or None (Tube L504-512)."""
    from pxr.Usdviewq.qt import QtWidgets
    for button in workspace.findChildren(QtWidgets.QAbstractButton):
        if button.text().split(" (", 1)[0] == label:
            return button
    return None


# ---------------------------------------------------------------------------
# Tf diagnostics on stderr
# ---------------------------------------------------------------------------

EXPIRED_CALLBACK = "Tried to call an expired python callback"


def _flushCStderr():
    """fflush(NULL) in the C runtime the USD DLLs print through, so a
    diagnostic written just before the fd swap lands on the right side."""
    for name in ("ucrtbase", "msvcrt", None):
        try:
            libc = ctypes.CDLL(name) if name else ctypes.CDLL(None)
            libc.fflush(None)
            return
        except (OSError, AttributeError, TypeError):
            continue


class StderrCapture:
    """Capture file descriptor 2 -- Tf's TF_WARN/TF_CODING_ERROR go there
    through fprintf(stderr), below Python's sys.stderr -- for the length of
    a `with` block, then replay it to the real stderr so the ctest log
    still shows every line. `count(text)` counts occurrences afterwards.

    Used by the T3s to pin "no Tf warning of kind X during this cycle"
    (EXPIRED_CALLBACK: a Tf.Notice listener whose Python callable died
    while the C++ side still called it).
    """

    def __init__(self):
        self.text = ""
        self._saved = None
        self._file = None

    def __enter__(self):
        try:
            sys.stderr.flush()
        except (AttributeError, ValueError):
            pass
        _flushCStderr()
        self._file = tempfile.TemporaryFile(mode="w+b")
        self._saved = os.dup(2)
        os.dup2(self._file.fileno(), 2)
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    def stop(self):
        if self._saved is None:
            return self.text
        try:
            sys.stderr.flush()
        except (AttributeError, ValueError):
            pass
        _flushCStderr()
        os.dup2(self._saved, 2)
        os.close(self._saved)
        self._saved = None
        self._file.seek(0)
        data = self._file.read()
        self._file.close()
        self._file = None
        if data:
            os.write(2, data)
        self.text = data.decode("utf-8", "replace")
        return self.text

    def count(self, needle):
        return self.text.count(needle)


# ---------------------------------------------------------------------------
# The artist's settings store
# ---------------------------------------------------------------------------

def realSettingsSnapshot(container):
    """What the artist's own Tonic QSettings (the registry on Windows) hold
    for every key the file commands remember; read-only."""
    from pxr.Usdviewq.qt import QtCore
    real = QtCore.QSettings(container.SETTINGS_ORG, container.SETTINGS_APP)
    return tuple((key, repr(real.value(key, None)))
                 for key in (container._DIALOG_DIR_KEY,
                             container._SAVE_TO_SCENE_KEY))


def isolateSettings(container, directory):
    """Point the container's QSettings at an .ini inside `directory`.

    Every T3 that drives Save/Export/Import through patched dialogs makes
    the plugin remember a temp folder as the dialog directory; with this the
    write lands in the test's own file, so a run that dies half way leaves
    the artist's store exactly as it was.  Returns restore(), which drops
    the override and answers whether the artist's store is unchanged.
    """
    before = realSettingsSnapshot(container)
    container.SETTINGS_FILE = os.path.join(directory, "tonicSettings.ini")

    def restore():
        vars(container).pop("SETTINGS_FILE", None)
        return realSettingsSnapshot(container) == before

    restore.before = before
    return restore
