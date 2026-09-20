# usdGenTonicTools.tonicLoopsFill -- Fill mode's viewport loop (plan/18
# section 3.6 FillLoop, plan/17 section 5.3).
#
# Fill mode is mostly a panel: density, CV count, seed, edge bias and the
# length ramp are numbers, and tonicPanels generates the widgets for them.
# What the VIEWPORT owns is (a) which tubes those numbers apply to, and
# (b) the one gesture the plan gives Fill -- dragging the length profile at
# a point along the tube.
#
#   Params   click selects a tube, marquee selects several; the panel then
#            edits exactly that selection through the per-tube ABI.
#   Preview  press on a tube or its guides and drag up/down to lift or drop
#            the length ramp at the t under the cursor.
#
# The density contract is plan/17 section 4.2 and section 5.3: a drag runs
# at the panel's preview fraction and the release puts the stored fraction
# back and refills at FULL density. Freeze roots is never touched here --
# the artist owns that switch, and Tonic_RefillGuides keeps the frozen
# prefix on its own -- so a drag can never silently re-seed roots the
# artist asked to keep.
from __future__ import annotations

import ctypes

from . import tonicHierarchy
from . import tonicLib
from . import tonicModes
from .tonicLoops import ToolLoop
from .tonicLoopsTube import previewGuides, refillPreview, restoreGuides

# How far, in screen heights, a drag has to travel to double the ramp.
RAMP_PIXELS = 240.0
# The ramp's value range: a profile value scales the guide length.
RAMP_MIN = 0.05
RAMP_MAX = 4.0
# Knot positions are snapped to this grid so a drag edits one knot rather
# than sprinkling a new one on every sample.
KNOT_SNAP = 0.05


def snapKnot(t):
    """A profile position on the knot grid, inside [0, 1]."""
    t = max(0.0, min(1.0, float(t)))
    return round(t / KNOT_SNAP) * KNOT_SNAP


def setProfileKnot(pairs, position, value):
    """The (position, value) ramp with one knot set, ascending by position.

    An empty ramp means "uniform 1" (tonicFill.evalLengthProfile), so the
    first edit lays down the ends as well: a ramp with one knot would
    otherwise flatten the whole tube to that value.
    """
    knots = [(float(pairs[i]), float(pairs[i + 1]))
             for i in range(0, len(pairs) - 1, 2)]
    position = snapKnot(position)
    value = max(RAMP_MIN, min(RAMP_MAX, float(value)))
    if not knots:
        knots = [(0.0, 1.0), (1.0, 1.0)]
    knots = [k for k in knots if abs(k[0] - position) > 1e-6]
    knots.append((position, value))
    knots.sort(key=lambda k: k[0])
    flat = []
    for pos, val in knots:
        flat.extend([pos, val])
    return flat


class FillLoop(ToolLoop):
    """Selection for the Fill panel, plus the length-ramp drag."""

    modeId = "fill"
    label = "Fill"
    subModes = tonicModes.FILL_SUBMODES
    defaultSubMode = "params"
    # A guide is pickable, and clicking one means the tube it grows from:
    # picking guides is how an artist points at a dense tube whose surface
    # is hidden behind its own hair.
    pickMask = tonicLib.TONIC_PICK_TUBE_VERT | tonicLib.TONIC_PICK_GUIDE

    def __init__(self, session, state):
        super(FillLoop, self).__init__(session, state)
        self._marquee = None
        self._bracketOpen = False
        self._ramp = None            # (tubeId, t, basePairs, baseValue)
        self._pressY = 0.0
        self._storedPreview = None
        self._pendingEdit = False
        self._lastStatus = ""

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.fillSubMode or self.defaultSubMode

    def setSubMode(self, subId):
        return tonicModes.SetActiveFillSubMode(self.state, subId)

    # -- status ------------------------------------------------------------

    def _status(self, text):
        self._lastStatus = text
        self.session.report(text)

    def statusLine(self):
        from .tonicFill import fillStatus
        params = self._fillParams(self._firstTube())
        if params is None:
            return "Fill: no tube."
        guides = ctypes.c_int(0)
        self.session.dll.Tonic_GetGuideCounts(self.session.model,
                                              ctypes.byref(guides), None)
        return "%s %d guide(s)." % (
            fillStatus(params["density"], params["cvCount"],
                       params["edgeBias"], params["seed"],
                       params["profile"], bool(self.state.freezeRoots)),
            int(guides.value))

    # -- reads -------------------------------------------------------------

    def selectedTubes(self):
        """Tube ids the panel and the drag apply to, ascending.

        Nothing selected means the primary tube: a groom with one tube
        should not need a click before its density can be typed.
        """
        ids = sorted({t for t, _s, _ss in self.session.readSelection(
            tonicLib.TONIC_PICK_TUBE_VERT)})
        return ids if ids else [0]

    def _firstTube(self):
        return self.selectedTubes()[0]

    def _fillParams(self, tubeId):
        """The fill params of one tube, profile included, or None.

        The per-tube read (plan/18 section 7 G2) hands back a profile
        COUNT, not the profile itself, so the values come from the tube-0
        entry -- which is the same storage for tube 0 and the only tube a
        pick can name today. A child tube reports its own count, and when
        that disagrees the loop starts the drag from a uniform ramp rather
        than from another tube's knots.
        """
        dll, model = self.session.dll, self.session.model
        if model is None:
            return None
        density = ctypes.c_float(0.0)
        cvCount = ctypes.c_int(0)
        seed = ctypes.c_int(0)
        edgeBias = ctypes.c_float(0.0)
        profile = (ctypes.c_float * 128)()
        got = ctypes.c_int(0)
        if dll.Tonic_GetFillParams(model, ctypes.byref(density),
                                   ctypes.byref(cvCount), ctypes.byref(seed),
                                   ctypes.byref(edgeBias), profile, 128,
                                   ctypes.byref(got)) != tonicLib.TONIC_OK:
            return None
        values = {"density": float(density.value),
                  "cvCount": int(cvCount.value), "seed": int(seed.value),
                  "edgeBias": float(edgeBias.value),
                  "profile": [float(profile[i]) for i in range(got.value)]}
        stage = getattr(self.session, "stageLib", None)
        if int(tubeId) != 0 and stage is not None:
            try:
                perTube = stage.fillParams(model, int(tubeId))
            except RuntimeError:
                return values
            values.update({k: perTube[k] for k in
                           ("density", "cvCount", "seed", "edgeBias")})
            if perTube["profileCount"] != len(values["profile"]):
                values["profile"] = []
        return values

    def _writeFill(self, tubeId, values):
        """Push one tube's fill params through the per-tube ABI."""
        stage = getattr(self.session, "stageLib", None)
        model = self.session.model
        if model is None:
            return False
        if stage is None:
            pairs = [float(v) for v in values["profile"]]
            arr = (ctypes.c_float * len(pairs))(*pairs) if pairs else None
            return self.session.dll.Tonic_SetFillParams(
                model, ctypes.c_float(values["density"]),
                int(values["cvCount"]), int(values["seed"]),
                ctypes.c_float(values["edgeBias"]), arr,
                len(pairs)) == tonicLib.TONIC_OK
        try:
            stage.setFillParams(model, int(tubeId), float(values["density"]),
                                int(values["cvCount"]), int(values["seed"]),
                                float(values["edgeBias"]),
                                [float(v) for v in values["profile"]])
        except RuntimeError as exc:
            self._status("Tonic Fill: %s" % exc)
            return False
        return True

    def _centerT(self, sample, tubeId):
        """The center-curve parameter nearest the cursor, or None.

        The ramp is a function of t along the tube, so the drag needs the t
        the artist is pointing at. The center curve is a handful of CVs and
        the camera is already resolved, so this is a projection of at most
        a few dozen points -- no pick, no Hydra.
        """
        camera = sample.camera
        if camera is None:
            return None
        try:
            centers = tonicHierarchy.tubeCenters(self.session.dll,
                                                 self.session.model,
                                                 int(tubeId))
        except (RuntimeError, NotImplementedError):
            return None
        if len(centers) < 2:
            return None
        best = None
        for i, point in enumerate(centers):
            projected = camera.worldToPixels(point)
            if projected is None:
                continue
            dist = ((projected[0] - sample.x) ** 2 +
                    (projected[1] - sample.y) ** 2)
            if best is None or dist < best[0]:
                best = (dist, float(i) / float(len(centers) - 1))
        return None if best is None else best[1]

    # -- gesture -----------------------------------------------------------

    def press(self, sample):
        if self.session.model is None:
            return False
        self._pendingEdit = False
        item = sample.item(self.pickMask, self.pickRadiusPx())
        if self.subMode() == "preview" and item is not None:
            return self._beginRamp(sample)
        if item is None:
            self._marquee = (sample.x, sample.y)
            return True
        mode = tonicLib.TONIC_SELECT_SET
        if sample.has("shift"):
            mode = tonicLib.TONIC_SELECT_ADD
        elif sample.has("ctrl"):
            mode = tonicLib.TONIC_SELECT_TOGGLE
        tubeId = (item["id"] if item["kind"] == tonicLib.TONIC_PICK_TUBE_VERT
                  else self._firstTube())
        self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, [tubeId], None,
                            None, mode)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        self._status("Tonic Fill: %d tube(s) selected"
                     % len(self.session.readSelection(
                         tonicLib.TONIC_PICK_TUBE_VERT)))
        return True

    def move(self, sample):
        if self.session.model is None:
            return False
        if self._marquee is not None:
            x0, y0 = self._marquee
            mode = (tonicLib.TONIC_SELECT_ADD if sample.has("shift")
                    else tonicLib.TONIC_SELECT_SET)
            self.session.selectRect(sample.camera, x0, y0, sample.x, sample.y,
                                    tonicLib.TONIC_PICK_TUBE_VERT, mode)
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            return True
        if self._ramp is None:
            return False
        # Up is longer: pixels grow downward, so the sign flips here once.
        travel = (self._pressY - sample.y) / RAMP_PIXELS
        value = self._ramp[3] * (1.0 + travel)
        self._applyRamp(value)
        refillPreview(self.session, self.state)
        self._pendingEdit = True
        return True

    def release(self, sample):
        if self._marquee is not None:
            self.move(sample)
            self._marquee = None
            self._status("Tonic Fill: %d tube(s) selected"
                         % len(self.session.readSelection(
                             tonicLib.TONIC_PICK_TUBE_VERT)))
            return True
        if self._ramp is None:
            return False
        if self._bracketOpen:
            self.session.endGesture()
            self._bracketOpen = False
        self._ramp = None
        if self._pendingEdit:
            restoreGuides(self.session, self._storedPreview)
            self.session.enqueueCommit()
            self._status(self.statusLine())
        self._storedPreview = None
        self._pendingEdit = False
        return True

    def cancel(self):
        if self._marquee is not None:
            self._marquee = None
            return True
        if self._ramp is None:
            return False
        dirty = 0
        if self._bracketOpen:
            dirty = self.session.cancelGesture()
            self._bracketOpen = False
        self._ramp = None
        restoreGuides(self.session, self._storedPreview,
                      refill=self._pendingEdit)
        self._storedPreview = None
        self._pendingEdit = False
        self.session.publish(dirty)
        self._status("Tonic Fill: cancelled")
        return True

    def hover(self, sample):
        if self.session.model is None:
            return False
        item = sample.item(self.pickMask, self.pickRadiusPx())
        if item:
            changed = self.session.setHover(item["kind"], item["id"],
                                            item["subId"], item["subSubId"])
        else:
            changed = self.session.setHover(0, -1, -1, -1)
        if changed:
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return False

    def deactivate(self):
        self.session.setHover(0, -1, -1, -1)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    # -- the ramp drag -----------------------------------------------------

    def _beginRamp(self, sample):
        # tonicFill is the numpy half of the package; the loops keep their
        # import graph free of it until a ramp is actually dragged.
        from .tonicFill import evalLengthProfile
        tubeId = self._firstTube()
        t = self._centerT(sample, tubeId)
        params = self._fillParams(tubeId)
        if t is None or params is None:
            return False
        base = evalLengthProfile(params["profile"], t)
        self._ramp = (tubeId, snapKnot(t), list(params["profile"]),
                      float(base))
        self._pressY = sample.y
        self._storedPreview = previewGuides(self.session, self.state)
        self.session.beginGesture("Fill length profile")
        self._bracketOpen = True
        self._status("Tonic Fill: length at t = %.2f" % self._ramp[1])
        return True

    def _applyRamp(self, value):
        tubeId, t, base, _baseValue = self._ramp
        pairs = setProfileKnot(base, t, value)
        for target in self.selectedTubes() or [tubeId]:
            params = self._fillParams(target)
            if params is None:
                continue
            params["profile"] = pairs
            self._writeFill(target, params)

    # -- what the panel drives --------------------------------------------

    def refill(self, fraction=1.0):
        """The panel's Refill button, and the release path's full refill."""
        if self.session.model is None:
            return False
        ok = self.session.dll.Tonic_RefillGuides(
            self.session.model,
            ctypes.c_float(float(fraction))) == tonicLib.TONIC_OK
        if ok:
            self.session.enqueueCommit()
            self.session.publish()
            self._status(self.statusLine())
        else:
            self._status("Tonic Fill: " + self.session.lastError())
        return ok

    def adjustRadius(self, delta):
        """`[` / `]`: the live preview fraction."""
        value = max(0.0, min(1.0, float(self.state.previewFraction) +
                             0.05 * float(delta)))
        self.state.previewFraction = value
        if self.session.model is not None:
            self.session.dll.Tonic_SetPreviewFraction(
                self.session.model, ctypes.c_float(value))
        status = "Tonic Fill: preview %.0f%%" % (value * 100.0)
        self._status(status)
        return status
