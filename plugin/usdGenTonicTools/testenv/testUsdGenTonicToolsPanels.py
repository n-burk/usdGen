#!/usr/bin/env python3
# testUsdGenTonicToolsPanels -- T0: the Qt-free V3 half of usdGenTonicTools
# (plan/18 V3 exit: workspace-dock parameter/action descriptors, HUD status
# strip and warnings).
#
# tonicPanels.py and tonicHud.py import and behave headlessly (no pxr, no
# Qt, no DLL): every descriptor's get/set and every action's handler is
# exercised against a fake session (a plain object carrying `.dll`, a fake
# recording every Tonic_* call, and `.ctx`, a sentinel) instead of the real
# ctypes library. Modules load by file path so the package __init__ (which
# imports pxr.Usdviewq) is never executed -- the same guarantee every other
# Qt-free Tonic test needs. tonicPanels/tonicHud import their sibling
# helper modules with a package/file-path fallback; this loader satisfies
# the fallback by registering each sibling under its bare name before the
# two files under test are loaded.
import ctypes
import importlib.util
import os
import sys

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenTonicTools"))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves annotations through sys.modules[__module__]; the
    # bare name also satisfies tonicPanels/tonicHud's `import <sibling>`
    # fallback for a file-path load.
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def checkRaises(exc, fn, what):
    try:
        fn()
    except exc:
        check(True, what)
    except Exception as other:  # noqa: BLE001 - the test names the type
        check(False, "%s (raised %r)" % (what, other))
    else:
        check(False, "%s (no raise)" % what)


class FakeDll(object):
    """Records every Tonic_* call and answers the same shapes the real
    ctypes-bound tonicApi.h entries do, including out-params through
    ctypes.byref() (its CPython `_obj` back-pointer) and ctypes arrays.

    Every entry point is a plain closure assigned onto the instance (never
    a class method), so tonicHierarchy.py's `entry.argtypes = [...]`
    pattern -- which the hierarchy actions route through -- can set
    attributes on it exactly as it would on a real ctypes function.
    """

    def __init__(self):
        self.calls = []
        self.snapRadius = 8.0
        self.mirrorX = 0
        self.softCenter = 0.0
        self.softRadius = 0.0
        self.displaySegments = 1
        self.fill = {"density": 8.0, "cvCount": 8, "seed": 0,
                    "edgeBias": 0.0, "profile": []}
        self.previewFraction = 0.25
        self.freezeRoots = 0
        self.levelDisplay = {}
        self.selection = []
        self.regionStats = (0, 0, 0)
        self.intersectedTubes = []
        self.smoothnessScores = []
        self.fallbackReason = b""

        def log(name, *args):
            self.calls.append((name,) + args)

        def getSnapRadius(ctx):
            log("Tonic_GetSnapRadius", ctx)
            return self.snapRadius

        def setSnapRadius(ctx, value):
            log("Tonic_SetSnapRadius", ctx, value)
            self.snapRadius = value
            return 0

        def getMirrorX(ctx):
            log("Tonic_GetMirrorX", ctx)
            return self.mirrorX

        def setMirrorX(ctx, value):
            log("Tonic_SetMirrorX", ctx, value)
            self.mirrorX = value
            return 0

        def getSoftSelection(ctx, centerPtr, radiusPtr):
            log("Tonic_GetSoftSelection", ctx)
            centerPtr._obj.value = self.softCenter
            radiusPtr._obj.value = self.softRadius
            return 0

        def setSoftSelection(ctx, center, radius):
            log("Tonic_SetSoftSelection", ctx, center, radius)
            self.softCenter = center
            self.softRadius = radius
            return 0

        def getDisplaySegments(ctx):
            log("Tonic_GetDisplaySegments", ctx)
            return self.displaySegments

        def setDisplaySegments(ctx, value):
            log("Tonic_SetDisplaySegments", ctx, value)
            self.displaySegments = value
            return 0

        def getFillParams(ctx, densityPtr, cvCountPtr, seedPtr, edgeBiasPtr,
                          profileArr, maxFloats, gotPtr):
            log("Tonic_GetFillParams", ctx)
            densityPtr._obj.value = self.fill["density"]
            cvCountPtr._obj.value = self.fill["cvCount"]
            seedPtr._obj.value = self.fill["seed"]
            edgeBiasPtr._obj.value = self.fill["edgeBias"]
            profile = self.fill["profile"]
            n = min(len(profile), maxFloats)
            for i in range(n):
                profileArr[i] = profile[i]
            gotPtr._obj.value = n
            return 0

        def setFillParams(ctx, density, cvCount, seed, edgeBias,
                          profileArr, pairFloats):
            log("Tonic_SetFillParams", ctx, density, cvCount, seed,
                edgeBias, pairFloats)
            self.fill["density"] = density
            self.fill["cvCount"] = cvCount
            self.fill["seed"] = seed
            self.fill["edgeBias"] = edgeBias
            self.fill["profile"] = (
                [profileArr[i] for i in range(pairFloats)]
                if profileArr else [])
            return 0

        def getPreviewFraction(ctx):
            log("Tonic_GetPreviewFraction", ctx)
            return self.previewFraction

        def setPreviewFraction(ctx, value):
            log("Tonic_SetPreviewFraction", ctx, value)
            self.previewFraction = value
            return 0

        def getFreezeRoots(ctx):
            log("Tonic_GetFreezeRoots", ctx)
            return self.freezeRoots

        def setFreezeRoots(ctx, value):
            log("Tonic_SetFreezeRoots", ctx, value)
            self.freezeRoots = value
            return 0

        def getLevelDisplay(ctx, level, visiblePtr, xrayPtr):
            log("Tonic_GetLevelDisplay", ctx, level)
            visible, xray = self.levelDisplay.get(level, (1, 0))
            visiblePtr._obj.value = visible
            xrayPtr._obj.value = xray
            return 0

        def setLevelDisplay(ctx, level, visible, xray):
            log("Tonic_SetLevelDisplay", ctx, level, visible, xray)
            self.levelDisplay[level] = (visible, xray)
            return 0

        def readSelection(ctx, kind, outIds, outSub, outSubSub, cap,
                          gotPtr):
            log("Tonic_ReadSelection", ctx, kind)
            ids = self.selection[:cap]
            for i, v in enumerate(ids):
                outIds[i] = v
            gotPtr._obj.value = len(ids)
            return 0

        def selectSet(ctx, kind, idsArr, subIds, subSubIds, n):
            log("Tonic_SelectSet", ctx, kind, [idsArr[i] for i in range(n)])
            return 0

        def getRegionStats(ctx, regionsPtr, uncoveredPtr, intersectedPtr):
            log("Tonic_GetRegionStats", ctx)
            regions, uncovered, intersected = self.regionStats
            regionsPtr._obj.value = regions
            uncoveredPtr._obj.value = uncovered
            intersectedPtr._obj.value = intersected
            return 0

        def readIntersectedTubes(ctx, out, cap, gotPtr):
            log("Tonic_ReadIntersectedTubes", ctx)
            ids = self.intersectedTubes[:cap]
            for i, v in enumerate(ids):
                out[i] = v
            gotPtr._obj.value = len(ids)
            return 0

        def readSmoothnessScores(ctx, out, cap, gotPtr):
            log("Tonic_ReadSmoothnessScores", ctx)
            scores = self.smoothnessScores[:cap]
            for i, v in enumerate(scores):
                out[i] = v
            gotPtr._obj.value = len(scores)
            return 0

        def getDeviceFallbackReason(ctx):
            log("Tonic_GetDeviceFallbackReason", ctx)
            return self.fallbackReason

        def graphWeldAll(ctx, radius, weldsPtr):
            log("Tonic_GraphWeldAll", ctx, radius)
            weldsPtr._obj.value = 0
            return 0

        def matchSurface(ctx):
            log("Tonic_MatchSurface", ctx)
            return 0

        def relaxCenter(ctx, strength, iterations):
            log("Tonic_RelaxCenter", ctx, strength, iterations)
            return 0

        def snapRootToScalp(ctx):
            log("Tonic_SnapRootToScalp", ctx)
            return 0

        def refillGuides(ctx, fraction):
            log("Tonic_RefillGuides", ctx, fraction)
            return 0

        def subdivideTube(ctx, tubeId, count, splitMode, seed, out, outCap,
                          gotPtr):
            if isinstance(splitMode, bytes):
                splitMode = splitMode.decode("ascii")
            log("Tonic_SubdivideTube", ctx, tubeId, count, splitMode, seed)
            ids = [tubeId * 10 + i for i in range(min(count, outCap))]
            for i, v in enumerate(ids):
                out[i] = v
            gotPtr._obj.value = len(ids)
            return 0

        def mergeChildren(ctx, tubeId):
            log("Tonic_MergeChildren", ctx, tubeId)
            return 0

        def mergeSelected(ctx, idsArr, n, keptPtr):
            ids = [idsArr[i] for i in range(n)]
            log("Tonic_MergeSelected", ctx, ids)
            keptPtr._obj.value = ids[0] if ids else -1
            return 0

        def setLockParents(ctx, tubeId, on):
            log("Tonic_SetLockParents", ctx, tubeId, on)
            return 0

        def setLockChildren(ctx, tubeId, on):
            log("Tonic_SetLockChildren", ctx, tubeId, on)
            return 0

        def groupTubes(ctx, idsArr, n, transient, parentPtr):
            ids = [idsArr[i] for i in range(n)]
            log("Tonic_GroupTubes", ctx, ids, transient)
            parentPtr._obj.value = -99
            return 0

        def makePersistent(ctx, tubeId):
            log("Tonic_MakePersistent", ctx, tubeId)
            return 0

        self.Tonic_GetSnapRadius = getSnapRadius
        self.Tonic_SetSnapRadius = setSnapRadius
        self.Tonic_GetMirrorX = getMirrorX
        self.Tonic_SetMirrorX = setMirrorX
        self.Tonic_GetSoftSelection = getSoftSelection
        self.Tonic_SetSoftSelection = setSoftSelection
        self.Tonic_GetDisplaySegments = getDisplaySegments
        self.Tonic_SetDisplaySegments = setDisplaySegments
        self.Tonic_GetFillParams = getFillParams
        self.Tonic_SetFillParams = setFillParams
        self.Tonic_GetPreviewFraction = getPreviewFraction
        self.Tonic_SetPreviewFraction = setPreviewFraction
        self.Tonic_GetFreezeRoots = getFreezeRoots
        self.Tonic_SetFreezeRoots = setFreezeRoots
        self.Tonic_GetLevelDisplay = getLevelDisplay
        self.Tonic_SetLevelDisplay = setLevelDisplay
        self.Tonic_ReadSelection = readSelection
        self.Tonic_SelectSet = selectSet
        self.Tonic_GetRegionStats = getRegionStats
        self.Tonic_ReadIntersectedTubes = readIntersectedTubes
        self.Tonic_ReadSmoothnessScores = readSmoothnessScores
        self.Tonic_GetDeviceFallbackReason = getDeviceFallbackReason
        self.Tonic_GraphWeldAll = graphWeldAll
        self.Tonic_MatchSurface = matchSurface
        self.Tonic_RelaxCenter = relaxCenter
        self.Tonic_SnapRootToScalp = snapRootToScalp
        self.Tonic_RefillGuides = refillGuides
        self.Tonic_SubdivideTube = subdivideTube
        self.Tonic_MergeChildren = mergeChildren
        self.Tonic_MergeSelected = mergeSelected
        self.Tonic_SetLockParents = setLockParents
        self.Tonic_SetLockChildren = setLockChildren
        self.Tonic_GroupTubes = groupTubes
        self.Tonic_MakePersistent = makePersistent


class FakeSession(object):
    def __init__(self, dll, ctx=1):
        self.dll = dll
        self.ctx = ctx
        self.publishCount = 0
        self.rebakeCount = 0
        self.modelVersion = 10
        self.committedVersion = 10
        self.pendingVersion = 10
        self.lastSwapMs = 1.2
        self.detached = False

    def publish(self):
        self.publishCount += 1

    def rebake(self):
        self.rebakeCount += 1


class FakeBakeSession(object):
    """A session that records what the Output panel asked the bake for.

    `snapTo` stands in for TonicSession.setBakeTexelResolution rounding a
    request down to a power of two: the panel must display the applied
    value, not the requested one.
    """

    def __init__(self):
        self.requested = []
        self.applied = 0
        self.rebakes = 0
        self.snapTo = None

    def setBakeTexelResolution(self, texels):
        self.requested.append(int(texels))
        self.applied = (int(self.snapTo) if self.snapTo is not None
                        and int(texels) > 0 else int(texels))
        return self.applied

    @property
    def bakeTexelResolution(self):
        return self.applied

    def rebake(self):
        self.rebakes += 1


class FakeOutputSession(object):
    def __init__(self):
        self.enabled = False
        self.densityMultiplier = 1.0
        self.strandWidth = 0.01
        self.buildCount = 0
        self.setCalls = []

    def outputSettings(self):
        return (self.enabled, self.densityMultiplier, self.strandWidth)

    def setOutputSettings(self, enabled=None, densityMultiplier=None,
                          strandWidth=None):
        if enabled is not None:
            self.enabled = bool(enabled)
        if densityMultiplier is not None:
            self.densityMultiplier = float(densityMultiplier)
        if strandWidth is not None:
            self.strandWidth = float(strandWidth)
        self.setCalls.append(self.outputSettings())
        return True

    def buildOutputDescription(self):
        self.enabled = True
        self.buildCount += 1
        return True


class FakeScaleStage(object):
    def __init__(self, failAt=None):
        self.calls = []
        self.failAt = failAt

    def scaleSectionRing(self, ctx, tubeId, ring, scale):
        if self.failAt is not None and len(self.calls) == self.failAt:
            raise RuntimeError("scale failed")
        self.calls.append((tubeId, ring, scale))


class FakeScaleSession(object):
    def __init__(self, stage, begin=True):
        self.model = 1
        self.ctx = 2
        self.dll = object()
        self.stageLib = stage
        self.begin = begin
        self.selected = [(3, 0, -1), (3, 1, -1)]
        self.beginCount = 0
        self.endCount = 0
        self.cancelCount = 0
        self.published = []

    def readSelection(self, kind):
        return list(self.selected)

    def beginGesture(self, label):
        self.beginCount += 1
        return self.begin

    def endGesture(self):
        self.endCount += 1

    def cancelGesture(self):
        self.cancelCount += 1
        return 77

    def publish(self, dirty=0):
        self.published.append(dirty)


class FakeContainer(object):
    def __init__(self, state, session=None, viewport=None):
        self.tonicState = state
        self.session = session
        self.viewport = viewport


ALL_MODES = ("graph", "tube", "fill", "hierarchy", "sculpt", "output")


def _stateOnlyRoundTrip(tonicPanels, modeId, state):
    for d in tonicPanels.descriptors(modeId, state):
        check(d.kind in ("int", "float", "bool", "enum", "ramp"),
              "%s.%s: kind %r is a known widget kind"
              % (modeId, d.id, d.kind))
        value = d.get(state, None)
        if d.kind == "bool":
            d.set(state, None, not value)
            check(d.get(state, None) == (not value),
                  "%s.%s: state-only round trip" % (modeId, d.id))
        elif d.kind == "float":
            step = d.step or 0.1
            target = min(max(value + step, d.min), d.max)
            d.set(state, None, target)
            check(abs(d.get(state, None) - target) < 1e-6,
                  "%s.%s: state-only round trip" % (modeId, d.id))
        elif d.kind == "int":
            target = int(min(max(value + 1, d.min), d.max))
            d.set(state, None, target)
            check(d.get(state, None) == target,
                  "%s.%s: state-only round trip" % (modeId, d.id))
        elif d.kind == "enum":
            target = d.choices[-1]
            d.set(state, None, target)
            check(d.get(state, None) == target,
                  "%s.%s: state-only round trip" % (modeId, d.id))
        elif d.kind == "ramp":
            d.set(state, None, [0.0, 1.0, 1.0, 2.0])
            check(d.get(state, None) == [0.0, 1.0, 1.0, 2.0],
                  "%s.%s: state-only round trip" % (modeId, d.id))


def main():
    _load("tonicFill")
    _load("tonicGraph")
    _load("tonicHierarchy")
    _load("tonicSculpt")
    _load("tonicTube")
    tonicToolState = _load("tonicToolState")
    tonicPanels = _load("tonicPanels")
    tonicHud = _load("tonicHud")

    check(callable(tonicPanels.descriptors) and callable(tonicPanels.actions),
          "tonicPanels exposes descriptors() and actions()")
    check(callable(tonicHud.statusStrip) and callable(tonicHud.warnings),
          "tonicHud exposes statusStrip() and warnings()")
    check(tonicHud.hudStatus is not None
          and tonicHud.hierarchyStatus is not None
          and tonicHud.sculptStatus is not None
          and tonicHud.fillStatus is not None
          and tonicHud.tubeStatus is not None,
          "tonicHud re-exports the five per-mode HUD one-liners")

    # -- descriptors: shape + no-session (state-only) round trip --------
    for modeId in ALL_MODES:
        state = tonicToolState.TonicToolState()
        descs = tonicPanels.descriptors(modeId, state)
        check(len(descs) > 0, "%s: descriptors() is non-empty" % modeId)
        ids = [d.id for d in descs]
        check(len(ids) == len(set(ids)),
              "%s: descriptor ids are unique" % modeId)
        _stateOnlyRoundTrip(tonicPanels, modeId, state)

    checkRaises(
        ValueError,
        lambda: next(d for d in tonicPanels.descriptors(
            "fill", tonicToolState.TonicToolState())
            if d.id == "lengthProfile").set(
                tonicToolState.TonicToolState(), None, [1.0]),
        "an odd length-profile write raises")

    # -- descriptors: through a live fake session ------------------------
    dll = FakeDll()
    session = FakeSession(dll)
    state = tonicToolState.TonicToolState()

    graphDescs = {d.id: d for d in tonicPanels.descriptors("graph", state)}
    graphDescs["snapRadiusPx"].set(state, session, 12.0)
    check(dll.snapRadius == 12.0
          and graphDescs["snapRadiusPx"].get(state, session) == 12.0,
          "graph.snapRadiusPx writes through Tonic_SetSnapRadius")
    graphDescs["mirrorX"].set(state, session, True)
    check(dll.mirrorX == 1 and graphDescs["mirrorX"].get(state, session)
          is True, "graph.mirrorX writes through Tonic_SetMirrorX")

    tubeDescs = {d.id: d for d in tonicPanels.descriptors("tube", state)}
    tubeDescs["softRadius"].set(state, session, 0.4)
    check(abs(dll.softRadius - 0.4) < 1e-6,
          "tube.softRadius writes through Tonic_SetSoftSelection")
    tubeDescs["displaySegments"].set(state, session, 3)
    check(dll.displaySegments == 3,
          "tube.displaySegments writes through Tonic_SetDisplaySegments")
    ringCount = tubeDescs["ringCvCount"]
    check(ringCount.kind == "enum" and ringCount.get(state, session) == 0
          and ringCount.choices[0] == 0 and 1 not in ringCount.choices
          and 2 not in ringCount.choices,
          "tube ring columns default to Auto and offer only 0 or 3..32")
    ringCount.set(state, session, 12)
    check(ringCount.get(state, session) == 12,
          "tube.ringCvCount retains an explicit next-build request")
    ringCount.set(state, session, 1)
    check(ringCount.get(state, session) == 3,
          "an invalid explicit 1-column request cannot reach the builder")
    ringCount.set(state, session, 0)
    check(ringCount.get(state, session) == 0,
          "tube.ringCvCount restores Auto/Match region CVs without an ABI call")

    fillDescs = {d.id: d for d in tonicPanels.descriptors("fill", state)}
    fillDescs["density"].set(state, session, 16.0)
    check(dll.fill["density"] == 16.0,
          "fill.density writes through Tonic_SetFillParams")
    fillDescs["lengthProfile"].set(state, session, [0.0, 1.0, 1.0, 0.5])
    check(dll.fill["profile"] == [0.0, 1.0, 1.0, 0.5],
          "fill.lengthProfile writes through Tonic_SetFillParams")
    fillDescs["previewFraction"].set(state, session, 0.5)
    check(dll.previewFraction == 0.5,
          "fill.previewFraction writes through Tonic_SetPreviewFraction")
    fillDescs["freezeRoots"].set(state, session, True)
    check(dll.freezeRoots == 1,
          "fill.freezeRoots writes through Tonic_SetFreezeRoots")

    hierDescs = {d.id: d for d in tonicPanels.descriptors("hierarchy",
                                                          state)}
    state.activeLevel = 1
    hierDescs["levelVisible"].set(state, session, False)
    check(dll.levelDisplay.get(1, (None, None))[0] == 0,
          "hierarchy.levelVisible writes through Tonic_SetLevelDisplay")
    hierDescs["levelXray"].set(state, session, True)
    check(dll.levelDisplay.get(1, (None, None))[1] == 1,
          "hierarchy.levelXray writes through Tonic_SetLevelDisplay")
    dll.selection = [3, 5]
    hierDescs["lockParents"].set(state, session, True)
    check(any(c[0] == "Tonic_SetLockParents" for c in dll.calls),
          "hierarchy.lockParents pushes to the tube selection")
    dll.selection = []

    # -- Tube section scale: one gesture, atomic failure -----------------
    class FakeBridge(object):
        @staticmethod
        def tubeSection(dll, model, tubeId, ring):
            return (0.0, [], 1.0, 0.0)

    previousBridge = sys.modules.get("tonicBridge")
    sys.modules["tonicBridge"] = FakeBridge
    try:
        scaleStage = FakeScaleStage(failAt=1)
        scaleSession = FakeScaleSession(scaleStage)
        scaleState = tonicToolState.TonicToolState()
        scaleDescriptor = next(
            d for d in tonicPanels.descriptors("tube", scaleState)
            if d.id == "uniformScale")
        scaleDescriptor.set(scaleState, scaleSession, 2.0)
        check(scaleSession.beginCount == 1 and scaleSession.endCount == 0
              and scaleSession.cancelCount == 1,
              "section scale validates and brackets the selected rings")
        check(len(scaleStage.calls) == 1 and scaleSession.published == [77],
              "a failed section scale cancels the atomic gesture")
    finally:
        if previousBridge is None:
            sys.modules.pop("tonicBridge", None)
        else:
            sys.modules["tonicBridge"] = previousBridge

    # -- actions: every action calls its documented ABI and publishes ---
    dll = FakeDll()
    session = FakeSession(dll)
    state = tonicToolState.TonicToolState()
    container = FakeContainer(state, session)

    graphActions = {a.id: a for a in tonicPanels.actions("graph")}
    graphActions["weldAll"].handler(container)
    check(any(c[0] == "Tonic_GraphWeldAll" for c in dll.calls)
          and session.publishCount == 1,
          "graph weldAll calls Tonic_GraphWeldAll and publishes")
    graphActions["rebake"].handler(container)
    check(session.rebakeCount == 1, "graph rebake calls session.rebake()")

    tubeActions = {a.id: a for a in tonicPanels.actions("tube")}
    tubeActions["matchSurface"].handler(container)
    check(any(c[0] == "Tonic_MatchSurface" for c in dll.calls),
          "tube matchSurface calls Tonic_MatchSurface")
    tubeActions["relax"].handler(container)
    check(any(c[0] == "Tonic_RelaxCenter" for c in dll.calls),
          "tube relax calls Tonic_RelaxCenter")
    tubeActions["snapRoot"].handler(container)
    check(any(c[0] == "Tonic_SnapRootToScalp" for c in dll.calls),
          "tube snapRoot calls Tonic_SnapRootToScalp")

    fillActions = {a.id: a for a in tonicPanels.actions("fill")}
    fillActions["refill"].handler(container)
    check(any(c[0] == "Tonic_RefillGuides" for c in dll.calls),
          "fill refill calls Tonic_RefillGuides")

    dll.selection = [4, 7]
    hierActions = {a.id: a for a in tonicPanels.actions("hierarchy")}
    hierActions["subdivide"].handler(container)
    check(any(c[0] == "Tonic_SubdivideTube" and c[2] == 4 for c in dll.calls)
          and any(c[0] == "Tonic_SubdivideTube" and c[2] == 7
                 for c in dll.calls),
          "hierarchy subdivide subdivides every selected tube")
    hierActions["mergeChildren"].handler(container)
    check(any(c[0] == "Tonic_MergeChildren" for c in dll.calls),
          "hierarchy mergeChildren calls Tonic_MergeChildren")
    hierActions["mergeSelected"].handler(container)
    check(any(c[0] == "Tonic_MergeSelected" for c in dll.calls),
          "hierarchy mergeSelected calls Tonic_MergeSelected")
    hierActions["group"].handler(container)
    check(any(c[0] == "Tonic_GroupTubes" for c in dll.calls),
          "hierarchy group calls Tonic_GroupTubes")
    hierActions["makePersistent"].handler(container)
    check(any(c[0] == "Tonic_MakePersistent" for c in dll.calls),
          "hierarchy makePersistent calls Tonic_MakePersistent")
    dll.selection = []

    check(tonicPanels.actions("sculpt") == [],
          "sculpt has no one-shot actions")
    outputActions = tonicPanels.actions("output")
    check([a.id for a in outputActions] == ["buildDescription"],
          "Output exposes Build/update description beside file commands")

    # -- Output: the texel override reaches the bake (V7) ----------------
    # The descriptor's job is to hand the session a texel count and show
    # back what the session actually applied, then rebake so the map on
    # disk matches the panel. The session's own snapping (powers of two,
    # clamped) is proven against the real bake in
    # testUsdGenTonicRegionBake; here the wiring is what is under test.
    bakeSession = FakeBakeSession()
    outputDescs = {d.id: d for d in tonicPanels.descriptors("output", state)}
    texel = outputDescs["texelResolution"]
    check(texel.kind == "enum" and "auto" in texel.choices,
          "the texel override is an enum with an auto entry (%r)"
          % (texel.choices,))
    texel.set(state, bakeSession, "1024")
    check(bakeSession.requested == [1024],
          "choosing 1024 asks the session for 1024 texels (%r)"
          % (bakeSession.requested,))
    check(bakeSession.rebakes == 1,
          "and rebakes at once rather than waiting for the next edit "
          "(%d)" % bakeSession.rebakes)
    check(texel.get(state, bakeSession) == "1024",
          "the panel reads back what the bake took (%r)"
          % texel.get(state, bakeSession))
    # A session that snapped the request down must win over the request.
    bakeSession.snapTo = 512
    texel.set(state, bakeSession, "1024")
    check(texel.get(state, bakeSession) == "512",
          "a snapped resolution is what the panel shows (%r)"
          % texel.get(state, bakeSession))
    texel.set(state, bakeSession, "auto")
    check(bakeSession.requested[-1] == 0 and
          texel.get(state, bakeSession) == "auto",
          "and auto asks for 0, which reads back as auto (%r)"
          % (bakeSession.requested[-1],))

    outputSession = FakeOutputSession()
    # The controls are shown for an already committed Output description;
    # keep the fake native state aligned with the state mirror before testing
    # density and width setters.
    outputSession.enabled = True
    state.outputEnabled = True
    outputDescs = {d.id: d for d in tonicPanels.descriptors("output", state)}
    density = outputDescs["outputDensityMultiplier"]
    width = outputDescs["outputStrandWidth"]
    density.set(state, outputSession, 2.5)
    width.set(state, outputSession, 0.02)
    check(outputSession.outputSettings() == (True, 2.5, 0.02),
          "Output density and strand width reach the session")
    outputActions[0].handler(FakeContainer(state, outputSession))
    check(outputSession.buildCount == 1 and outputSession.enabled,
          "Build/update description enables the committed Output")

    # -- no session: every action no-ops instead of raising --------------
    inertContainer = FakeContainer(tonicToolState.TonicToolState(), None)
    for modeId in ("graph", "tube", "fill", "hierarchy"):
        for action in tonicPanels.actions(modeId):
            action.handler(inertContainer)
    check(True, "every action no-ops harmlessly with no session")

    # -- tonicHud.statusStrip: the amber skew cases -----------------------
    state = tonicToolState.TonicToolState()
    state.mapVersion = 5
    state.bakedVersion = 5
    session = FakeSession(FakeDll())
    strip = tonicHud.statusStrip(state, session)
    check(strip.amber is False, "matched map/baked versions are not amber")

    state.bakedVersion = 3
    strip = tonicHud.statusStrip(state, session)
    check(strip.amber is True,
          "a baked version behind the map version is amber")

    state.bakedVersion = 5
    session.committedVersion = 7
    strip = tonicHud.statusStrip(state, session)
    check(strip.amber is True,
          "a committed version behind the live model version is amber")

    check(tonicHud.statusStrip(state, FakeSession(FakeDll())).gpuText
          == "GPU", "no fallback reason reads as plain GPU")
    fallbackDll = FakeDll()
    fallbackDll.fallbackReason = b"no CUDA device"
    check("CPU-only"
          in tonicHud.statusStrip(state, FakeSession(fallbackDll)).gpuText,
          "a device fallback reason shows CPU-only in the status strip")
    check(tonicHud.statusStrip(state, None).breadcrumb != "",
          "statusStrip works with session=None (breadcrumb from state "
          "alone)")

    # -- tonicHud.warnings -------------------------------------------------
    dll = FakeDll()
    session = FakeSession(dll)
    container = FakeContainer(state, session)
    dll.regionStats = (4, 2, 0)
    warns = tonicHud.warnings(state, session)
    check(any("face center(s) outside regions" in w.text for w in warns),
          "a coarse uncovered-face count produces a coverage warning")

    dll.intersectedTubes = [2, 5]
    warns = tonicHud.warnings(state, session)
    intersectWarn = next(w for w in warns if "overlap" in w.text)
    check(intersectWarn.selectAction is not None,
          "the root-intersection warning carries a selectAction")
    intersectWarn.selectAction(container)
    check(any(c[0] == "Tonic_SelectSet" and c[3] == [2, 5]
             for c in dll.calls),
          "clicking the root-intersection warning selects the tubes")

    dll.smoothnessScores = [0.0, 0.05, 0.0]
    warns = tonicHud.warnings(state, session)
    check(any("Smoothness" in w.text for w in warns),
          "a smoothness spike produces a warning")

    dll.fallbackReason = b"no CUDA device"
    warns = tonicHud.warnings(state, session)
    check(any("CPU-only" in w.text for w in warns),
          "a device fallback reason produces a warning")

    session.detached = True
    warns = tonicHud.warnings(state, session)
    check(any("detached" in w.text for w in warns),
          "a detached committer produces a warning")

    check(tonicHud.warnings(tonicToolState.TonicToolState(), None) == [],
          "warnings() with no session returns no ABI-backed rows")

    # -- tonicHud.warningsKey (plan/18 V9-perf memoization) ----------------
    # The dock refreshes on every publish, pump and 250 ms tick; the key
    # lets it skip the warnings re-read when nothing it reads has moved.
    keyDll = FakeDll()
    keySession = FakeSession(keyDll)
    keyState = tonicToolState.TonicToolState()
    keyDll.regionStats = (4, 2, 0)
    first = tonicHud.warningsKey(keyState, keySession)
    check(first is not None,
          "warningsKey() returns a key for a versioned session")
    check(tonicHud.warningsKey(keyState, keySession) == first,
          "warningsKey() is stable across identical inputs")
    rowsFirst = [w.text for w in tonicHud.warnings(keyState, keySession)]
    rowsAgain = [w.text for w in tonicHud.warnings(keyState, keySession)]
    check(rowsFirst == rowsAgain,
          "an unchanged key means unchanged warning rows")
    keySession.modelVersion = 11
    check(tonicHud.warningsKey(keyState, keySession) != first,
          "warningsKey() moves with the model version")
    keySession.modelVersion = 10
    keySession.detached = True
    check(tonicHud.warningsKey(keyState, keySession) != first,
          "warningsKey() moves when the committer detaches")
    keySession.detached = False
    keyDll.fallbackReason = b"no CUDA device"
    check(tonicHud.warningsKey(keyState, keySession) != first,
          "warningsKey() moves on a device fallback")
    check(tonicHud.warningsKey(keyState, None) is None,
          "warningsKey() with no session is None (force a re-read)")

    print("testUsdGenTonicToolsPanels: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
