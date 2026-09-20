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
