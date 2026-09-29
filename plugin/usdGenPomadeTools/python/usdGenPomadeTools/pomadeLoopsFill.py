# usdGenPomadeTools.pomadeLoopsFill -- Fill mode's viewport loop (plan/18
# section 3.6 FillLoop, plan/17 section 5.3).
#
# Fill mode is mostly a panel: density, CV count, seed, edge bias and the
# length ramp are numbers, and pomadePanels generates the widgets for them.
# What the VIEWPORT owns is (a) which tubes those numbers apply to, and
# (b) the one gesture the plan gives Fill -- dragging the length profile at
# a radial position inside the tube.
#
#   Params   click selects a tube, marquee selects several; the panel then
#            edits exactly that selection through the per-tube ABI.
#   Preview  press on a tube and drag up/down to lift or drop the length
#            ramp at the radial position under the cursor; the pressed tube
#            is the one edited (with the rest of the selection when it is
#            part of it). A click on a guide with no tube under it only says
#            to click its tube: a guide pick cannot name the tube it grows
#            from.
#
# What the length ramp IS (the K9 kernels, pomadeTube.cpp
# PomadeBuildGuideMaterialBindingsCpu / PomadeGuideFillCpu and pomadeKernels.cu,
# which must stay bit-identical): each guide's length fraction is the
# profile evaluated at its ROOT's radial position in the root ring -- 0 at
# the ring's centre, 1 at its wall, after the edge-bias remap r^(1-bias/2)
# -- clamped to [0, 1]. 1 is full length (the tip follows the last ring);
# nothing can grow past it. So the drag edits the knot at the radial
# position the artist pressed (a pressed guide's own root, or the tube-body
# hit projected into the nearest ring), and its value lives in [0, 1]: up
# lengthens toward 1, down shortens toward 0.
#
# The density contract is plan/17 section 4.2 and section 5.3: a drag runs
# at the panel's preview fraction and the release puts the stored fraction
# back and refills at FULL density. Freeze roots is never touched here --
# the artist owns that switch, and Pomade_RefillGuides keeps the frozen
# prefix on its own -- so a drag can never silently re-seed roots the
# artist asked to keep.
from __future__ import annotations

import ctypes
import math
import time

from . import pomadeBridge
from . import pomadeHierarchy
from . import pomadeLib
from . import pomadeModes
from .pomadeLoops import (MIN_PICK_RADIUS_PX, ToolLoop, selectBand,
                         selectItems, selectModeFor)
from .pomadeLoopsTube import previewGuides, refillPreview, restoreGuides

# How far, in pixels, a drag travels to carry a knot across the whole
# [0, 1] length range (up is longer).
RAMP_PIXELS = 240.0
# The live "Length ramp" readout is refreshed at most this often, in
# seconds: a mouse delivers moves far faster than anyone can read them.
RAMP_STATUS_INTERVAL = 1.0 / 30.0
# A guide pick names a guide index, not the tube it grows from, and there is
# no ABI to ask (a Pomade_GetGuideOwner entry). Guessing a tube would edit
# the wrong one, so the click says what to do instead.
GUIDE_PICK_HINT = ("Pomade Fill: strands belong to the tube they grow from; "
                   "click the tube")
# The ramp's value range: a profile value is the fraction of the full guide
# length, and the kernels clamp it to [0, 1] -- a knot above 1 would read
# back as an edit that changes nothing.
RAMP_MIN = 0.0
RAMP_MAX = 1.0
# Knot positions are snapped to this grid so a drag edits one knot rather
# than sprinkling a new one on every sample.
KNOT_SNAP = 0.05
# Said when no radial position can be found under the press (no ring with
# a readable size, or a camera that cannot cast the ray).
RADIAL_MISS_HINT = ("Pomade Fill: cannot tell where inside the tube that is; "
                    "press on the tube body or one of its strands")


def snapKnot(t):
    """A profile position on the knot grid, inside [0, 1]."""
    t = max(0.0, min(1.0, float(t)))
    return round(t / KNOT_SNAP) * KNOT_SNAP


def profilePosition(radius, edgeBias):
    """The ramp position of a root at normalised ring radius `radius`.

    The kernels evaluate the profile at r^(1 - bias/2) (the edge-bias
    remap, pomadeFill.edgeBiasRemap), not at the raw radius, so the knot a
    press edits has to go through the same remap to land on the guides
    under the cursor.
    """
    r = max(0.0, min(1.0, float(radius)))
    bias = max(-1.0, min(1.0, float(edgeBias)))
    if r <= 0.0:
        return 0.0
    return float(r ** (1.0 - 0.5 * bias))


def chartRadius(uv):
    """Mean distance of a ring chart's points from their centroid.

    The kernels normalise a root's radius by the same mean vertex distance
    (_K9PolygonCenterRadius), so a round ring and a squashed one both run
    0 at the centre to about 1 at the wall.
    """
    if not uv:
        return 0.0
    cu = sum(p[0] for p in uv) / float(len(uv))
    cv = sum(p[1] for p in uv) / float(len(uv))
    return sum(math.hypot(p[0] - cu, p[1] - cv) for p in uv) / float(len(uv))


def rayAxisDistance(origin, direction, point, axis):
    """Shortest distance between a view ray's line and a tube axis line.

    Looking across the tube it is the cursor's sideways offset from the
    centre line; looking down the tube (ray parallel to the axis) it is the
    distance from the axis in the ring plane. Either way it is the radius
    the cursor points at, in world units. None for a degenerate axis.
    """
    alen = math.sqrt(sum(a * a for a in axis))
    dlen = math.sqrt(sum(d * d for d in direction))
    if not alen > 1e-12 or not dlen > 1e-12:
        return None
    a = [c / alen for c in axis]
    d = [c / dlen for c in direction]
    w = [point[i] - origin[i] for i in range(3)]
    n = (d[1] * a[2] - d[2] * a[1], d[2] * a[0] - d[0] * a[2],
         d[0] * a[1] - d[1] * a[0])
    nlen = math.sqrt(sum(c * c for c in n))
    if nlen < 1e-6:
        along = sum(w[i] * d[i] for i in range(3))
        return math.sqrt(sum((w[i] - along * d[i]) ** 2 for i in range(3)))
    return abs(sum(w[i] * n[i] for i in range(3))) / nlen


def _centerCurveAt(centers, t):
    """(point, tangent) of the center polyline at ring parameter `t`.

    The model spaces t uniformly over the CV spans (PomadeEvalCenter); the
    straight span is close enough to aim a radius at, and it is only the
    fallback for a stage library that cannot read the ring's frame.
    """
    if len(centers) < 2:
        return None
    seg = max(0.0, min(1.0, float(t))) * float(len(centers) - 1)
    i = min(int(seg), len(centers) - 2)
    f = seg - float(i)
    a, b = centers[i], centers[i + 1]
    point = tuple(a[k] + (b[k] - a[k]) * f for k in range(3))
    tangent = tuple(b[k] - a[k] for k in range(3))
    return point, tangent


def setProfileKnot(pairs, position, value):
    """The (position, value) ramp with one knot set, ascending by position.

    An empty ramp means "uniform 1" (pomadeFill.evalLengthProfile), so the
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
    subModes = pomadeModes.FILL_SUBMODES
    defaultSubMode = "params"
    # A guide is pickable so a click on a dense tube's hair is answered
    # (GUIDE_PICK_HINT) instead of falling through to a marquee: the hair
    # hides the surface an artist is pointing at.
    pickMask = pomadeLib.POMADE_PICK_TUBE_VERT | pomadeLib.POMADE_PICK_GUIDE

    def __init__(self, session, state):
        super(FillLoop, self).__init__(session, state)
        self._marquee = None
        # The tube selection at the marquee press: the band selects live,
        # so Escape has to put this back rather than keep a partial band.
        self._marqueeBase = None
        self._bracketOpen = False
        # (tubeId, radial knot position, basePairs, baseValue)
        self._ramp = None
        self._rampValue = 0.0        # the knot value last written
        self._pressY = 0.0
        self._storedPreview = None
        self._pendingEdit = False
        self._lastStatus = ""
        self._writeErrorShown = False    # one fill-write error per gesture
        self._rampStatusAt = None        # time of the last ramp readout

    # -- lifetime ----------------------------------------------------------

    def activate(self):
        """Grow the guides when Fill opens over a groom that has none.

        Tubes built before any refill show bare surfaces, and the Fill
        panel is where an artist looks for the hair. An explicit Clear is
        respected by the model itself: after Pomade_ClearGeneratedCurves
        RefillGuides is a no-op until Generate, so the count stays 0 and
        nothing is committed.
        """
        session = self.session
        if session.model is None:
            return False
        if self._guideCount() != 0:
            return False
        if session.dll.Pomade_RefillGuides(
                session.model, ctypes.c_float(1.0)) != pomadeLib.POMADE_OK:
            return False                 # no tube yet: nothing to grow from
        if self._guideCount() <= 0:
            return False
        session.enqueueCommit()
        session.publish()
        return True

    def _guideCount(self):
        guides = ctypes.c_int(0)
        if self.session.dll.Pomade_GetGuideCounts(
                self.session.model, ctypes.byref(guides),
                None) != pomadeLib.POMADE_OK:
            return -1
        return int(guides.value)

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.fillSubMode or self.defaultSubMode

    def setSubMode(self, subId):
        return pomadeModes.SetActiveFillSubMode(self.state, subId)

    # -- status ------------------------------------------------------------

    def _status(self, text):
        self._lastStatus = text
        self.session.report(text)

    def statusLine(self):
        from .pomadeFill import fillStatus
        params = self._fillParams(self._firstTube())
        if params is None:
            return "Fill: no tube."
        guides = ctypes.c_int(0)
        self.session.dll.Pomade_GetGuideCounts(self.session.model,
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
            pomadeLib.POMADE_PICK_TUBE_VERT)})
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
        if dll.Pomade_GetFillParams(model, ctypes.byref(density),
                                   ctypes.byref(cvCount), ctypes.byref(seed),
                                   ctypes.byref(edgeBias), profile, 128,
                                   ctypes.byref(got)) != pomadeLib.POMADE_OK:
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
            if self.session.dll.Pomade_SetFillParams(
                    model, ctypes.c_float(values["density"]),
                    int(values["cvCount"]), int(values["seed"]),
                    ctypes.c_float(values["edgeBias"]), arr,
                    len(pairs)) != pomadeLib.POMADE_OK:
                self._writeError(self.session.lastError())
                return False
            return True
        try:
            stage.setFillParams(model, int(tubeId), float(values["density"]),
                                int(values["cvCount"]), int(values["seed"]),
                                float(values["edgeBias"]),
                                [float(v) for v in values["profile"]])
        except RuntimeError as exc:
            self._writeError(exc)
            return False
        return True

    def _writeError(self, reason):
        """Report a refused fill write once per gesture.

        A ramp drag writes on every move sample; a model that refuses the
        first write refuses them all, and a status line per sample would
        bury the one message that matters.
        """
        if self._ramp is not None:
            if self._writeErrorShown:
                return
            self._writeErrorShown = True
        self._status("Pomade Fill: %s" % reason)

    def _radialPosition(self, sample, tubeId, guideId=None):
        """The normalised ring radius (0 centre .. 1 wall) under the press.

        The kernels key the length ramp by each guide's ROOT radius, so the
        drag needs the radius the artist is pointing at: a pressed guide's
        own root when the root census can name it, else the tube-body hit
        projected into the ring nearest the cursor. None when neither can
        be read. The edge-bias remap is applied by the caller
        (profilePosition).
        """
        if guideId is not None:
            radius = self._guideRootRadius(int(guideId))
            if radius is not None:
                return radius
        return self._ringRadiusUnderCursor(sample, tubeId)

    def _guideRootRadius(self, guideId):
        """|(ru, rv)| of guide `guideId`'s root, or None.

        Pomade_ReadGuideRoots hands back each root's coordinate in its root
        ring, normalised by the ring radius. A guide pick names a guide
        index; it is also the root index only while the census agrees on
        the count, so a mismatch reads as "unknown" rather than a guess.
        """
        dll, model = self.session.dll, self.session.model
        count = ctypes.c_int(0)
        try:
            if dll.Pomade_ReadGuideRoots(model, None, None, None, 0,
                                        ctypes.byref(count)) \
                    != pomadeLib.POMADE_OK:
                return None
            n = int(count.value)
            if n <= 0 or not 0 <= guideId < n or self._guideCount() != n:
                return None
            ru = (ctypes.c_float * (2 * n))()
            if dll.Pomade_ReadGuideRoots(model, None, None, ru, n,
                                        ctypes.byref(count)) \
                    != pomadeLib.POMADE_OK:
                return None
        except (AttributeError, TypeError, ctypes.ArgumentError):
            return None
        return min(1.0, math.hypot(float(ru[2 * guideId]),
                                   float(ru[2 * guideId + 1])))

    def _ringRadiusUnderCursor(self, sample, tubeId):
        """The cursor ray's distance from the tube axis over the ring radius.

        The ring is the section ring whose centre projects nearest the
        cursor; its axis is the center tangent there (the section frame's
        w, or the center curve's segment when the frame cannot be read)
        and its radius the chart's mean vertex distance times its scale.
        """
        camera = sample.camera
        if camera is None:
            return None
        ray = camera.rayThrough(sample.x, sample.y)
        if ray is None:
            return None
        dll, model = self.session.dll, self.session.model
        stage = getattr(self.session, "stageLib", None)
        try:
            count = pomadeBridge.tubeSectionCount(dll, model, int(tubeId))
            centers = pomadeHierarchy.tubeCenters(dll, model, int(tubeId))
        except (RuntimeError, NotImplementedError, AttributeError):
            return None
        best = None
        for ring in range(max(0, count)):
            try:
                t, uv, scale, _twist = pomadeBridge.tubeSection(
                    dll, model, int(tubeId), ring)
            except (RuntimeError, NotImplementedError):
                continue
            radius = chartRadius(uv) * abs(float(scale))
            if not radius > 1e-9:
                continue
            frame = None
            if stage is not None:
                try:
                    frame = stage.sectionFrame(model, int(tubeId), ring)
                except (RuntimeError, AttributeError):
                    frame = None
            if frame is not None:
                center, axis = frame["origin"], frame["w"]
            else:
                placed = _centerCurveAt(centers, t)
                if placed is None:
                    continue
                center, axis = placed
            projected = camera.worldToPixels(center)
            if projected is None:
                continue
            dist = ((projected[0] - sample.x) ** 2 +
                    (projected[1] - sample.y) ** 2)
            if best is None or dist < best[0]:
                best = (dist, center, axis, radius)
        if best is None:
            return None
        offset = rayAxisDistance(ray[0], ray[1], best[1], best[2])
        if offset is None:
            return None
        return min(1.0, offset / best[3])

    # -- gesture -----------------------------------------------------------

    def press(self, sample):
        if self.session.model is None:
            return False
        self._pendingEdit = False
        item = sample.item(self.pickMask, self.pickRadiusPx())
        if item is None:
            self._marquee = (sample.x, sample.y)
            self._marqueeBase = self.session.readSelection(
                pomadeLib.POMADE_PICK_TUBE_VERT)
            return True
        guideId = None
        if item["kind"] != pomadeLib.POMADE_PICK_TUBE_VERT:
            # A guide does not say which tube it grows from (no
            # Pomade_GetGuideOwner yet); the old fallback to the selected
            # tube edited a tube the artist was not pointing at. A tube
            # surface under the same cursor is still an honest answer.
            # The guide itself still says WHERE in the ring it grows: the
            # ramp drag keys its knot by that guide's root radius.
            if item["kind"] == pomadeLib.POMADE_PICK_GUIDE:
                guideId = int(item["id"])
            tube = sample.item(pomadeLib.POMADE_PICK_TUBE_VERT,
                               self.pickRadiusPx())
            if tube is None or tube["kind"] != pomadeLib.POMADE_PICK_TUBE_VERT:
                self._status(GUIDE_PICK_HINT)
                return True
            item = tube
        tubeId = int(item["id"])
        if self.subMode() == "preview":
            return self._pressRamp(sample, tubeId, guideId)
        selectItems(self.session, pomadeLib.POMADE_PICK_TUBE_VERT,
                    [(tubeId, -1, -1)], selectModeFor(sample))
        self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        self._status("Pomade Fill: %d tube(s) selected"
                     % len(self.session.readSelection(
                         pomadeLib.POMADE_PICK_TUBE_VERT)))
        return True

    def _pressRamp(self, sample, tubeId, guideId=None):
        """Select the pressed tube, then start its ramp drag.

        The ramp edits the tube under the cursor. Pressing a tube that is
        already part of the selection keeps the selection, so one drag can
        still shape several selected tubes together (the usual
        drag-a-selected-item rule); pressing any other tube replaces it.
        `guideId` is the strand the press landed on, if any: its root
        radius names the knot.
        """
        selected = {t for t, _s, _ss in self.session.readSelection(
            pomadeLib.POMADE_PICK_TUBE_VERT)}
        if tubeId not in selected:
            self.session.select(pomadeLib.POMADE_PICK_TUBE_VERT, [tubeId],
                                None, None, pomadeLib.POMADE_SELECT_SET)
            self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        self._beginRamp(sample, tubeId, guideId)
        # The click selected the tube even when no drag could start.
        return True

    def move(self, sample):
        if self.session.model is None:
            return False
        if self._marquee is not None:
            x0, y0 = self._marquee
            selectBand(self.session, pomadeLib.POMADE_PICK_TUBE_VERT,
                       selectModeFor(sample, band=True),
                       lambda mode: self.session.selectRect(
                           sample.camera, x0, y0, sample.x, sample.y,
                           pomadeLib.POMADE_PICK_TUBE_VERT, mode))
            self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
            return True
        if self._ramp is None:
            return False
        # Up is longer: pixels grow downward, so the sign flips here once.
        # The travel moves the knot's length fraction linearly: RAMP_PIXELS
        # spans the whole [0, 1] range.
        travel = (self._pressY - sample.y) / RAMP_PIXELS
        if not self._pendingEdit and abs(travel) < 1e-9:
            # Qt repeats the press position; writing it would turn a click
            # into an edit (an empty ramp becomes an explicit one).
            return True
        value = max(RAMP_MIN, min(RAMP_MAX, self._ramp[3] + travel))
        if abs(value - self._rampValue) < 1e-6:
            # Pinned at a limit (already full length and still going up,
            # or already zero): a write would change nothing but the knot
            # list, so the readout just says where the knot stands.
            self._rampStatus(value)
            return True
        if not self._applyRamp(value):
            return True                  # refused: _writeError said why
        self._rampValue = value
        refillPreview(self.session, self.state)
        self._pendingEdit = True
        self._rampStatus(value)
        return True

    def _rampStatus(self, value):
        """'Length ramp r=0.35: 0.80', at most every RAMP_STATUS_INTERVAL.

        r is the knot's radial position (0 centre, 1 wall) and the value
        the fraction of full length the guides there grow to.
        """
        text = "Pomade Fill: length ramp r=%.2f: %.2f" % (self._ramp[1],
                                                        value)
        if value >= RAMP_MAX:
            text += " (full length)"
        if text == self._lastStatus:
            return
        now = time.monotonic()
        if (self._rampStatusAt is not None and
                now - self._rampStatusAt < RAMP_STATUS_INTERVAL):
            return
        self._rampStatusAt = now
        self._status(text)

    def release(self, sample):
        if self._marquee is not None:
            self.move(sample)
            self._marquee = None
            self._marqueeBase = None
            self._status("Pomade Fill: %d tube(s) selected"
                         % len(self.session.readSelection(
                             pomadeLib.POMADE_PICK_TUBE_VERT)))
            return True
        if self._ramp is None:
            return False
        wrote = self._pendingEdit
        # A drag that wrote but came back to the press-time value leaves
        # the length curve as it was: cancelling puts the exact old knots
        # back instead of keeping a step that only re-spells them.
        edited = wrote and abs(self._rampValue - self._ramp[3]) > 1e-6
        if self._bracketOpen:
            self._bracketOpen = False
            # SS-02: a click without travel (or a round trip) changed
            # nothing, and sealing the bracket would leave an empty step.
            self.session.endGestureIfChanged(edited)
        self._ramp = None
        # The press lowered the preview fraction whether or not the drag
        # moved, so it always goes back; any write needs the refill (a
        # cancelled round trip restored the model under the preview).
        restoreGuides(self.session, self._storedPreview, refill=wrote)
        if edited:
            self.session.enqueueCommit()
            self._status(self.statusLine())
        self._storedPreview = None
        self._pendingEdit = False
        return True

    def cancel(self):
        if self._marquee is not None:
            self._marquee = None
            self._restoreMarqueeBase()
            self._status("Pomade Fill: selection cancelled")
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
        self._status("Pomade Fill: cancelled")
        return True

    def _restoreMarqueeBase(self):
        """Put back the tube selection the cancelled band started from."""
        base, self._marqueeBase = self._marqueeBase, None
        if base is None or self.session.model is None:
            return False
        kind = pomadeLib.POMADE_PICK_TUBE_VERT
        # Clear then ADD: an empty SET would leave the band's tubes.
        self.session.clearSelection(kind)
        if base:
            self.session.select(kind, [e[0] for e in base],
                                [e[1] for e in base], [e[2] for e in base],
                                pomadeLib.POMADE_SELECT_ADD)
        self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
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
            self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        return False

    def deactivate(self):
        self.session.setHover(0, -1, -1, -1)
        self.session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        return True

    # -- the ramp drag -----------------------------------------------------

    def _beginRamp(self, sample, tubeId, guideId=None):
        """Start the ramp drag on `tubeId`, the tube under the press.

        The knot is the ramp position of the radius under the press (see
        _radialPosition), edge-bias remapped exactly as the kernels key
        each guide; its starting value is the profile there, clamped to
        [0, 1] as the kernels clamp it.
        """
        # pomadeFill is the numpy half of the package; the loops keep their
        # import graph free of it until a ramp is actually dragged.
        from .pomadeFill import evalLengthProfile
        params = self._fillParams(tubeId)
        if params is None:
            return False
        radius = self._radialPosition(sample, tubeId, guideId)
        if radius is None:
            self._status(RADIAL_MISS_HINT)
            return False
        if not self.session.beginGesture("Fill length profile"):
            # The session reported why. Edits outside a bracket could not
            # be undone or cancelled, so there is no drag at all.
            return False
        self._bracketOpen = True
        position = snapKnot(profilePosition(radius, params["edgeBias"]))
        base = max(RAMP_MIN, min(RAMP_MAX, float(
            evalLengthProfile(params["profile"], position))))
        self._ramp = (tubeId, position, list(params["profile"]), base)
        self._rampValue = base
        self._pressY = sample.y
        self._writeErrorShown = False
        self._rampStatusAt = None
        self._storedPreview = previewGuides(self.session, self.state)
        self._status("Pomade Fill: length ramp r=%.2f: %.2f%s"
                     % (position, base,
                        " (full length)" if base >= RAMP_MAX else ""))
        return True

    def _applyRamp(self, value):
        """Write the ramp to the selection; the number of tubes written."""
        tubeId, position, base, _baseValue = self._ramp
        pairs = setProfileKnot(base, position, value)
        written = 0
        for target in self.selectedTubes() or [tubeId]:
            params = self._fillParams(target)
            if params is None:
                continue
            params["profile"] = pairs
            if self._writeFill(target, params):
                written += 1
        return written

    # -- what the panel drives --------------------------------------------

    def refill(self, fraction=1.0):
        """The panel's Refill button, and the release path's full refill."""
        if self.session.model is None:
            return False
        ok = self.session.dll.Pomade_RefillGuides(
            self.session.model,
            ctypes.c_float(float(fraction))) == pomadeLib.POMADE_OK
        if ok:
            self.session.enqueueCommit()
            self.session.publish()
            self._status(self.statusLine())
        else:
            self._status("Pomade Fill: " + self.session.lastError())
        return ok

    def adjustRadius(self, delta):
        """`[` / `]`: the live preview fraction."""
        value = max(0.0, min(1.0, float(self.state.previewFraction) +
                             0.05 * float(delta)))
        self.state.previewFraction = value
        if self.session.model is not None:
            self.session.dll.Pomade_SetPreviewFraction(
                self.session.model, ctypes.c_float(value))
        status = "Pomade Fill: preview %.0f%%" % (value * 100.0)
        self._status(status)
        return status

    def pickRadiusPx(self):
        # MD-04: tubes are picked with the shared pick radius, not Graph's
        # weld distance -- a wide Graph snap made Fill clicks grab tubes
        # the artist was not over.
        return max(float(self.state.pickRadiusPx), MIN_PICK_RADIUS_PX)
