# usdGenTonicTools.tonicLibStage -- ctypes binding of tonicApiStage.h (V0b).
#
# The stage-contract half of the C ABI: hydrate, the per-tube forms of the
# section/center/fill operations, and the bake enqueue that carries the
# hierarchy. It binds onto a tonicLib.Library that is already loaded, so
# there is one DLL handle per process and one place that finds it.
#
# Qt-free, like every module under this package (plan/08 section 1.2).
from __future__ import annotations

import ctypes

from . import tonicLib

TONIC_OK = tonicLib.TONIC_OK
TONIC_ERROR = tonicLib.TONIC_ERROR


class StageLibrary:
    """Stage-contract entry points bound onto an existing Library."""

    def __init__(self, library=None):
        self.library = library if library is not None else tonicLib.Library()
        self._dll = self.library._dll
        self._bind()

    def _bind(self):
        dll = self._dll
        cvp = ctypes.c_void_p
        cip = ctypes.POINTER(ctypes.c_int)
        cfp = ctypes.POINTER(ctypes.c_float)
        ccp = ctypes.c_char_p

        dll.Tonic_StageGetLastError.argtypes = []
        dll.Tonic_StageGetLastError.restype = ctypes.c_char_p

        dll.Tonic_Hydrate.argtypes = [cvp, ccp, ccp, cip, cip, cip]
        dll.Tonic_Hydrate.restype = ctypes.c_int

        for name, extra in (
                ("Tonic_InsertTubeCenterCV", [ctypes.c_int]),
                ("Tonic_DeleteTubeCenterCV", [ctypes.c_int]),
                ("Tonic_SetTubeLengthFor", [ctypes.c_float]),
                ("Tonic_MatchTubeSurface", []),
                ("Tonic_SnapTubeRootToScalp", []),
                ("Tonic_RelaxTubeCenter", [ctypes.c_float, ctypes.c_int]),
                ("Tonic_MoveTubeSectionRing",
                 [ctypes.c_int, ctypes.c_float, ctypes.c_float]),
                ("Tonic_ScaleTubeSectionRing", [ctypes.c_int, ctypes.c_float]),
                ("Tonic_TwistTubeSectionRing", [ctypes.c_int, ctypes.c_float]),
                ("Tonic_MoveTubeSectionCV",
                 [ctypes.c_int, ctypes.c_int, ctypes.c_float,
                  ctypes.c_float]),
                ("Tonic_AddTubeSectionRing", [ctypes.c_float]),
                ("Tonic_RemoveTubeSectionRing", [ctypes.c_int]),
                ("Tonic_CopyTubeSectionRing", [ctypes.c_int, ctypes.c_int]),
        ):
            entry = getattr(dll, name)
            entry.argtypes = [cvp, ctypes.c_int] + extra
            entry.restype = ctypes.c_int

        dll.Tonic_SetTubeFillParams.argtypes = [
            cvp, ctypes.c_int, ctypes.c_float, ctypes.c_int, ctypes.c_int,
            ctypes.c_float, cfp, ctypes.c_int]
        dll.Tonic_SetTubeFillParams.restype = ctypes.c_int
        dll.Tonic_GetTubeFillParams.argtypes = [
            cvp, ctypes.c_int, cfp, cip, cip, cfp, cip]
        dll.Tonic_GetTubeFillParams.restype = ctypes.c_int
        dll.Tonic_IsTubeFillSuspended.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_IsTubeFillSuspended.restype = ctypes.c_int
        dll.Tonic_GetTubeParent.argtypes = [cvp, ctypes.c_int, cip, cip]
        dll.Tonic_GetTubeParent.restype = ctypes.c_int
        dll.Tonic_IsTubePersistent.argtypes = [cvp, ctypes.c_int]
        dll.Tonic_IsTubePersistent.restype = ctypes.c_int
        dll.Tonic_BakeEnqueueLevels.argtypes = [cvp]
        dll.Tonic_BakeEnqueueLevels.restype = ctypes.c_int
        dll.Tonic_GetTubeSectionFrame.argtypes = [
            cvp, ctypes.c_int, ctypes.c_int, cfp, cfp, cfp, cfp]
        dll.Tonic_GetTubeSectionFrame.restype = ctypes.c_int
        bindV5(dll)

    # -- errors ------------------------------------------------------------

    def lastError(self):
        text = self._dll.Tonic_StageGetLastError()
        return text.decode("utf-8") if text else ""

    def _check(self, status, what):
        if status != TONIC_OK:
            raise RuntimeError("%s failed: %s" % (what, self.lastError()))

    # -- hydrate -----------------------------------------------------------

    def hydrate(self, ctx, layerOrStage, groomPath):
        """Hydrate `ctx` from a committed groom.

        `layerOrStage` is an open layer identifier or a file path. Returns
        (tubeCount, guideCount, importedCount).
        """
        tubes = ctypes.c_int(0)
        guides = ctypes.c_int(0)
        imported = ctypes.c_int(0)
        status = self._dll.Tonic_Hydrate(
            ctx, layerOrStage.encode("utf-8"), groomPath.encode("utf-8"),
            ctypes.byref(tubes), ctypes.byref(guides), ctypes.byref(imported))
        self._check(status, "Tonic_Hydrate")
        return tubes.value, guides.value, imported.value

    # -- per-tube center operations ---------------------------------------

    def insertCenterCV(self, ctx, tubeId, atIndex):
        self._check(self._dll.Tonic_InsertTubeCenterCV(ctx, tubeId, atIndex),
                    "Tonic_InsertTubeCenterCV")

    def deleteCenterCV(self, ctx, tubeId, index):
        self._check(self._dll.Tonic_DeleteTubeCenterCV(ctx, tubeId, index),
                    "Tonic_DeleteTubeCenterCV")

    def setLength(self, ctx, tubeId, length):
        self._check(self._dll.Tonic_SetTubeLengthFor(ctx, tubeId, length),
                    "Tonic_SetTubeLengthFor")

    def matchSurface(self, ctx, tubeId):
        self._check(self._dll.Tonic_MatchTubeSurface(ctx, tubeId),
                    "Tonic_MatchTubeSurface")

    def snapRootToScalp(self, ctx, tubeId):
        self._check(self._dll.Tonic_SnapTubeRootToScalp(ctx, tubeId),
                    "Tonic_SnapTubeRootToScalp")

    def relaxCenter(self, ctx, tubeId, strength, iterations):
        self._check(
            self._dll.Tonic_RelaxTubeCenter(ctx, tubeId, strength, iterations),
            "Tonic_RelaxTubeCenter")

    # -- per-tube section operations --------------------------------------

    def moveSectionRing(self, ctx, tubeId, ring, du, dv):
        self._check(
            self._dll.Tonic_MoveTubeSectionRing(ctx, tubeId, ring, du, dv),
            "Tonic_MoveTubeSectionRing")

    def scaleSectionRing(self, ctx, tubeId, ring, scale):
        self._check(
            self._dll.Tonic_ScaleTubeSectionRing(ctx, tubeId, ring, scale),
            "Tonic_ScaleTubeSectionRing")

    def twistSectionRing(self, ctx, tubeId, ring, radians):
        self._check(
            self._dll.Tonic_TwistTubeSectionRing(ctx, tubeId, ring, radians),
            "Tonic_TwistTubeSectionRing")

    def moveSectionCV(self, ctx, tubeId, ring, slot, du, dv):
        self._check(
            self._dll.Tonic_MoveTubeSectionCV(ctx, tubeId, ring, slot, du, dv),
            "Tonic_MoveTubeSectionCV")

    def addSectionRing(self, ctx, tubeId, t):
        self._check(self._dll.Tonic_AddTubeSectionRing(ctx, tubeId, t),
                    "Tonic_AddTubeSectionRing")

    def removeSectionRing(self, ctx, tubeId, ring):
        self._check(self._dll.Tonic_RemoveTubeSectionRing(ctx, tubeId, ring),
                    "Tonic_RemoveTubeSectionRing")

    def copySectionRing(self, ctx, tubeId, src, dst):
        self._check(self._dll.Tonic_CopyTubeSectionRing(ctx, tubeId, src, dst),
                    "Tonic_CopyTubeSectionRing")

    # -- per-tube fill -----------------------------------------------------

    def setFillParams(self, ctx, tubeId, density, cvCount, seed, edgeBias,
                      lengthProfile=()):
        profile = None
        count = 0
        if lengthProfile:
            count = len(lengthProfile)
            profile = (ctypes.c_float * count)(*lengthProfile)
        self._check(
            self._dll.Tonic_SetTubeFillParams(ctx, tubeId, density, cvCount,
                                              seed, edgeBias, profile, count),
            "Tonic_SetTubeFillParams")

    def fillParams(self, ctx, tubeId):
        density = ctypes.c_float(0.0)
        cvCount = ctypes.c_int(0)
        seed = ctypes.c_int(0)
        edgeBias = ctypes.c_float(0.0)
        profileCount = ctypes.c_int(0)
        self._check(
            self._dll.Tonic_GetTubeFillParams(
                ctx, tubeId, ctypes.byref(density), ctypes.byref(cvCount),
                ctypes.byref(seed), ctypes.byref(edgeBias),
                ctypes.byref(profileCount)),
            "Tonic_GetTubeFillParams")
        return {"density": density.value, "cvCount": cvCount.value,
                "seed": seed.value, "edgeBias": edgeBias.value,
                "profileCount": profileCount.value}

    def isFillSuspended(self, ctx, tubeId):
        status = self._dll.Tonic_IsTubeFillSuspended(ctx, tubeId)
        if status < 0:
            raise RuntimeError("Tonic_IsTubeFillSuspended failed: %s"
                               % self.lastError())
        return status == 1

    # -- hierarchy reads ---------------------------------------------------

    def tubeParent(self, ctx, tubeId):
        parent = ctypes.c_int(0)
        childIndex = ctypes.c_int(0)
        self._check(
            self._dll.Tonic_GetTubeParent(ctx, tubeId, ctypes.byref(parent),
                                          ctypes.byref(childIndex)),
            "Tonic_GetTubeParent")
        return parent.value, childIndex.value

    def isPersistent(self, ctx, tubeId):
        status = self._dll.Tonic_IsTubePersistent(ctx, tubeId)
        if status < 0:
            raise RuntimeError("Tonic_IsTubePersistent failed: %s"
                               % self.lastError())
        return status == 1

    # -- bake --------------------------------------------------------------

    def bakeEnqueueLevels(self, bake):
        self._check(self._dll.Tonic_BakeEnqueueLevels(bake),
                    "Tonic_BakeEnqueueLevels")

    # -- V4: where a section ring sits -------------------------------------

    def sectionFrame(self, ctx, tubeId, ring):
        """The world placement of one section ring, or None.

        {"origin", "u", "v", "w", "scale", "twist"} with the three axes as
        (x, y, z) tuples. None (rather than a raise) when the tube or the
        ring is gone: a gesture asks for this on every move and a ring that
        vanished under an undo is a normal answer, not an error.
        """
        origin = (ctypes.c_float * 3)()
        frame = (ctypes.c_float * 9)()
        scale = ctypes.c_float(1.0)
        twist = ctypes.c_float(0.0)
        if self._dll.Tonic_GetTubeSectionFrame(
                ctx, int(tubeId), int(ring), origin, frame,
                ctypes.byref(scale), ctypes.byref(twist)) != TONIC_OK:
            return None
        return {"origin": (origin[0], origin[1], origin[2]),
                "u": (frame[0], frame[1], frame[2]),
                "v": (frame[3], frame[4], frame[5]),
                "w": (frame[6], frame[7], frame[8]),
                "scale": float(scale.value), "twist": float(twist.value)}

    def worldToChart(self, ring, delta):
        """A world delta as the (du, dv) Tonic_MoveTubeSectionRing takes.

        `ring` is a sectionFrame() dict. The tangent component is dropped:
        the chart is the ring plane, and a section CV that left it would no
        longer be on the ring it belongs to (plan/17 section 5.2).
        """
        import math
        u, v = ring["u"], ring["v"]
        a = sum(delta[i] * u[i] for i in range(3))
        b = sum(delta[i] * v[i] for i in range(3))
        scale = float(ring["scale"])
        if not abs(scale) > 1e-9:
            return (0.0, 0.0)
        ct = math.cos(float(ring["twist"]))
        st = math.sin(float(ring["twist"]))
        return ((a * ct + b * st) / scale, (-a * st + b * ct) / scale)


# -- V5 (plan/18 section 3.6/3.7): the shaped sculpt stroke, the drawn-edge
# subdivide and the per-level draw mode ------------------------------------
#
# Bound by a module function rather than only in StageLibrary._bind because
# the tool loops hold the raw ctypes handle (TonicSession.dll), not a
# StageLibrary, and there must be exactly one place that says what these
# entries take. Idempotent: binding twice writes the same argtypes.

def bindV5(dll):
    """Bind the V5 stage entries onto a raw ctypes handle."""
    cvp = ctypes.c_void_p
    cip = ctypes.POINTER(ctypes.c_int)
    cfp = ctypes.POINTER(ctypes.c_float)
    dll.Tonic_SculptStrokeShaped.argtypes = [
        cvp, ctypes.c_int, ctypes.c_char_p, cfp, ctypes.c_int, ctypes.c_int,
        ctypes.c_float, ctypes.c_float, ctypes.c_float, cfp, ctypes.c_float,
        ctypes.c_float, ctypes.c_float, ctypes.c_int, ctypes.c_int, cip]
    dll.Tonic_SculptStrokeShaped.restype = ctypes.c_int
    dll.Tonic_SubdivideTubeEdge.argtypes = [
        cvp, ctypes.c_int, cfp, cfp, ctypes.c_int, cip, ctypes.c_int, cip]
    dll.Tonic_SubdivideTubeEdge.restype = ctypes.c_int
    dll.Tonic_SetLevelDrawMode.argtypes = [
        cvp, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    dll.Tonic_SetLevelDrawMode.restype = ctypes.c_int
    dll.Tonic_GetLevelDrawMode.argtypes = [cvp, ctypes.c_int, cip, cip, cip]
    dll.Tonic_GetLevelDrawMode.restype = ctypes.c_int
    return dll


def sculptStrokeShaped(dll, ctx, tubeId, brush, camera, x, y, radiusPx,
                       deltaWorld, amount=0.0, tCenter=0.5, tRadius=0.0,
                       preserveLength=True, mirrorX=False):
    """One shaped stroke (plan/18 section 7 G13); returns the CVs moved.

    `camera` is a tonicCamera.TonicCamera: the loop resolved it once at
    press, and the brush falloff is measured in ITS pixels, which is what
    makes a screen-radius brush mean what the artist sees.
    """
    delta = (ctypes.c_float * 3)(*[float(v) for v in deltaWorld])
    touched = ctypes.c_int(0)
    status = dll.Tonic_SculptStrokeShaped(
        ctx, int(tubeId), str(brush).encode("ascii"),
        camera.viewProjArray(), int(camera.width), int(camera.height),
        ctypes.c_float(float(x)), ctypes.c_float(float(y)),
        ctypes.c_float(float(radiusPx)), delta, ctypes.c_float(float(amount)),
        ctypes.c_float(float(tCenter)), ctypes.c_float(float(tRadius)),
        1 if preserveLength else 0, 1 if mirrorX else 0,
        ctypes.byref(touched))
    if status != TONIC_OK:
        raise RuntimeError("Tonic_SculptStrokeShaped failed")
    return int(touched.value)


def subdivideTubeEdge(dll, ctx, tubeId, worldA, worldB, seed=0):
    """Split `tubeId` along a drawn edge; returns the two child ids."""
    a = (ctypes.c_float * 3)(*[float(v) for v in worldA])
    b = (ctypes.c_float * 3)(*[float(v) for v in worldB])
    out = (ctypes.c_int * 2)()
    got = ctypes.c_int(0)
    if dll.Tonic_SubdivideTubeEdge(ctx, int(tubeId), a, b, int(seed), out, 2,
                                   ctypes.byref(got)) != TONIC_OK:
        raise RuntimeError("Tonic_SubdivideTubeEdge failed")
    return [int(out[i]) for i in range(int(got.value))]


def levelDrawMode(dll, ctx, level):
    """(visible, xray, centersOnly) for `level`, or None."""
    visible = ctypes.c_int(1)
    xray = ctypes.c_int(0)
    centers = ctypes.c_int(0)
    if dll.Tonic_GetLevelDrawMode(ctx, int(level), ctypes.byref(visible),
                                  ctypes.byref(xray),
                                  ctypes.byref(centers)) != TONIC_OK:
        return None
    return (bool(visible.value), bool(xray.value), bool(centers.value))


def setLevelDrawMode(dll, ctx, level, visible, xray, centersOnly):
    """Drive the per-level draw mode (the ladder's centers-only rung)."""
    return dll.Tonic_SetLevelDrawMode(ctx, int(level), 1 if visible else 0,
                                      1 if xray else 0,
                                      1 if centersOnly else 0) == TONIC_OK
