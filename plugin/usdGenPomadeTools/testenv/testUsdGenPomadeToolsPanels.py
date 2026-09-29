#!/usr/bin/env python3
# testUsdGenPomadeToolsPanels -- T0: the Qt-free V3 half of usdGenPomadeTools
# (plan/18 V3 exit: workspace-dock parameter/action descriptors, HUD status
# strip and warnings).
#
# pomadePanels.py and pomadeHud.py import and behave headlessly (no pxr, no
# Qt, no DLL): every descriptor's get/set and every action's handler is
# exercised against a fake session (a plain object carrying `.dll`, a fake
# recording every Pomade_* call, and `.ctx`, a sentinel) instead of the real
# ctypes library. Modules load by file path so the package __init__ (which
# imports pxr.Usdviewq) is never executed -- the same guarantee every other
# Qt-free Pomade test needs. pomadePanels/pomadeHud import their sibling
# helper modules with a package/file-path fallback; this loader satisfies
# the fallback by registering each sibling under its bare name before the
# two files under test are loaded.
import ctypes
import importlib.util
import os
import sys

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenPomadeTools"))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves annotations through sys.modules[__module__]; the
    # bare name also satisfies pomadePanels/pomadeHud's `import <sibling>`
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
    """Records every Pomade_* call and answers the same shapes the real
    ctypes-bound pomadeApi.h entries do, including out-params through
    ctypes.byref() (its CPython `_obj` back-pointer) and ctypes arrays.

    Every entry point is a plain closure assigned onto the instance (never
    a class method), so pomadeHierarchy.py's `entry.argtypes = [...]`
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
            log("Pomade_GetSnapRadius", ctx)
            return self.snapRadius

        def setSnapRadius(ctx, value):
            log("Pomade_SetSnapRadius", ctx, value)
            self.snapRadius = value
            return 0

        def getMirrorX(ctx):
            log("Pomade_GetMirrorX", ctx)
            return self.mirrorX

        def setMirrorX(ctx, value):
            log("Pomade_SetMirrorX", ctx, value)
            self.mirrorX = value
            return 0

        def getSoftSelection(ctx, centerPtr, radiusPtr):
            log("Pomade_GetSoftSelection", ctx)
            centerPtr._obj.value = self.softCenter
            radiusPtr._obj.value = self.softRadius
            return 0

        def setSoftSelection(ctx, center, radius):
            log("Pomade_SetSoftSelection", ctx, center, radius)
            self.softCenter = center
            self.softRadius = radius
            return 0

        def getDisplaySegments(ctx):
            log("Pomade_GetDisplaySegments", ctx)
            return self.displaySegments

        def setDisplaySegments(ctx, value):
            log("Pomade_SetDisplaySegments", ctx, value)
            self.displaySegments = value
            return 0

        def getFillParams(ctx, densityPtr, cvCountPtr, seedPtr, edgeBiasPtr,
                          profileArr, maxFloats, gotPtr):
            log("Pomade_GetFillParams", ctx)
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
            log("Pomade_SetFillParams", ctx, density, cvCount, seed,
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
            log("Pomade_GetPreviewFraction", ctx)
            return self.previewFraction

        def setPreviewFraction(ctx, value):
            log("Pomade_SetPreviewFraction", ctx, value)
            self.previewFraction = value
            return 0

        def getFreezeRoots(ctx):
            log("Pomade_GetFreezeRoots", ctx)
            return self.freezeRoots

        def setFreezeRoots(ctx, value):
            log("Pomade_SetFreezeRoots", ctx, value)
            self.freezeRoots = value
            return 0

        def getLevelDisplay(ctx, level, visiblePtr, xrayPtr):
            log("Pomade_GetLevelDisplay", ctx, level)
            visible, xray = self.levelDisplay.get(level, (1, 0))
            visiblePtr._obj.value = visible
            xrayPtr._obj.value = xray
            return 0

        def setLevelDisplay(ctx, level, visible, xray):
            log("Pomade_SetLevelDisplay", ctx, level, visible, xray)
            self.levelDisplay[level] = (visible, xray)
            return 0

        def readSelection(ctx, kind, outIds, outSub, outSubSub, cap,
                          gotPtr):
            log("Pomade_ReadSelection", ctx, kind)
            ids = self.selection[:cap]
            for i, v in enumerate(ids):
                outIds[i] = v
            gotPtr._obj.value = len(ids)
            return 0

        def selectSet(ctx, kind, idsArr, subIds, subSubIds, n):
            log("Pomade_SelectSet", ctx, kind, [idsArr[i] for i in range(n)])
            return 0

        def getRegionStats(ctx, regionsPtr, uncoveredPtr, intersectedPtr):
            log("Pomade_GetRegionStats", ctx)
            regions, uncovered, intersected = self.regionStats
            regionsPtr._obj.value = regions
            uncoveredPtr._obj.value = uncovered
            intersectedPtr._obj.value = intersected
            return 0

        def readIntersectedTubes(ctx, out, cap, gotPtr):
            log("Pomade_ReadIntersectedTubes", ctx)
            ids = self.intersectedTubes[:cap]
            for i, v in enumerate(ids):
                out[i] = v
            gotPtr._obj.value = len(ids)
            return 0

        def readSmoothnessScores(ctx, out, cap, gotPtr):
            log("Pomade_ReadSmoothnessScores", ctx)
            scores = self.smoothnessScores[:cap]
            for i, v in enumerate(scores):
                out[i] = v
            gotPtr._obj.value = len(scores)
            return 0

        def getDeviceFallbackReason(ctx):
            log("Pomade_GetDeviceFallbackReason", ctx)
            return self.fallbackReason

        def graphWeldAll(ctx, radius, weldsPtr):
            log("Pomade_GraphWeldAll", ctx, radius)
            weldsPtr._obj.value = 0
            return 0

        def matchSurface(ctx):
            log("Pomade_MatchSurface", ctx)
            return 0

        def relaxCenter(ctx, strength, iterations):
            log("Pomade_RelaxCenter", ctx, strength, iterations)
            return 0

        def snapRootToScalp(ctx):
            log("Pomade_SnapRootToScalp", ctx)
            return 0

        def refillGuides(ctx, fraction):
            log("Pomade_RefillGuides", ctx, fraction)
            return 0

        def subdivideTube(ctx, tubeId, count, splitMode, seed, out, outCap,
                          gotPtr):
            if isinstance(splitMode, bytes):
                splitMode = splitMode.decode("ascii")
            log("Pomade_SubdivideTube", ctx, tubeId, count, splitMode, seed)
            ids = [tubeId * 10 + i for i in range(min(count, outCap))]
            for i, v in enumerate(ids):
                out[i] = v
            gotPtr._obj.value = len(ids)
            return 0

        def mergeChildren(ctx, tubeId):
            log("Pomade_MergeChildren", ctx, tubeId)
            return 0

        def mergeSelected(ctx, idsArr, n, keptPtr):
            ids = [idsArr[i] for i in range(n)]
            log("Pomade_MergeSelected", ctx, ids)
            keptPtr._obj.value = ids[0] if ids else -1
            return 0

        def setLockParents(ctx, tubeId, on):
            log("Pomade_SetLockParents", ctx, tubeId, on)
            return 0

        def setLockChildren(ctx, tubeId, on):
            log("Pomade_SetLockChildren", ctx, tubeId, on)
            return 0

        def groupTubes(ctx, idsArr, n, transient, parentPtr):
            ids = [idsArr[i] for i in range(n)]
            log("Pomade_GroupTubes", ctx, ids, transient)
            parentPtr._obj.value = -99
            return 0

        def makePersistent(ctx, tubeId):
            log("Pomade_MakePersistent", ctx, tubeId)
            return 0

        self.Pomade_GetSnapRadius = getSnapRadius
        self.Pomade_SetSnapRadius = setSnapRadius
        self.Pomade_GetMirrorX = getMirrorX
        self.Pomade_SetMirrorX = setMirrorX
        self.Pomade_GetSoftSelection = getSoftSelection
        self.Pomade_SetSoftSelection = setSoftSelection
        self.Pomade_GetDisplaySegments = getDisplaySegments
        self.Pomade_SetDisplaySegments = setDisplaySegments
        self.Pomade_GetFillParams = getFillParams
        self.Pomade_SetFillParams = setFillParams
        self.Pomade_GetPreviewFraction = getPreviewFraction
        self.Pomade_SetPreviewFraction = setPreviewFraction
        self.Pomade_GetFreezeRoots = getFreezeRoots
        self.Pomade_SetFreezeRoots = setFreezeRoots
        self.Pomade_GetLevelDisplay = getLevelDisplay
        self.Pomade_SetLevelDisplay = setLevelDisplay
        self.Pomade_ReadSelection = readSelection
        self.Pomade_SelectSet = selectSet
        self.Pomade_GetRegionStats = getRegionStats
        self.Pomade_ReadIntersectedTubes = readIntersectedTubes
        self.Pomade_ReadSmoothnessScores = readSmoothnessScores
        self.Pomade_GetDeviceFallbackReason = getDeviceFallbackReason
        self.Pomade_GraphWeldAll = graphWeldAll
        self.Pomade_MatchSurface = matchSurface
        self.Pomade_RelaxCenter = relaxCenter
        self.Pomade_SnapRootToScalp = snapRootToScalp
        self.Pomade_RefillGuides = refillGuides
        self.Pomade_SubdivideTube = subdivideTube
        self.Pomade_MergeChildren = mergeChildren
        self.Pomade_MergeSelected = mergeSelected
        self.Pomade_SetLockParents = setLockParents
        self.Pomade_SetLockChildren = setLockChildren
        self.Pomade_GroupTubes = groupTubes
        self.Pomade_MakePersistent = makePersistent

        # DK-06: the Output row's model switch and the Density readout.
        self.amplified = 0
        self.guideCount = 0

        def setAmplifiedHair(ctx, on):
            log("Pomade_SetAmplifiedHair", ctx, on)
            self.amplified = int(on)
            return 0

        def getAmplifiedHair(ctx):
            log("Pomade_GetAmplifiedHair", ctx)
            return self.amplified

        def getGuideCounts(ctx, guidesPtr, cvPtr):
            log("Pomade_GetGuideCounts", ctx)
            guidesPtr._obj.value = self.guideCount
            return 0

        self.Pomade_SetAmplifiedHair = setAmplifiedHair
        self.Pomade_GetAmplifiedHair = getAmplifiedHair
        self.Pomade_GetGuideCounts = getGuideCounts


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


class FakeGestureSession(FakeSession):
    """A FakeSession with PomadeSession's gesture bracket (SS-02).

    The bracket calls land in the FakeDll's call log under their ABI
    names, so a test can read the order Begin / SetFillParams / End.
    """

    def __init__(self, dll, ctx=1):
        FakeSession.__init__(self, dll, ctx)
        self.gestureActive = False

    def beginGesture(self, label):
        self.dll.calls.append(("Pomade_BeginGesture", label))
        self.gestureActive = True
        return True

    def endGesture(self):
        self.dll.calls.append(("Pomade_EndGesture",))
        self.gestureActive = False
        return True

    def cancelGesture(self):
        self.dll.calls.append(("Pomade_CancelGesture",))
        self.gestureActive = False
        return 0

    def endGestureIfChanged(self, changed):
        if changed:
            return self.endGesture()
        self.cancelGesture()
        return False


class FakeBakeSession(object):
    """A session that records what the Output panel asked the bake for.

    `snapTo` stands in for PomadeSession.setBakeTexelResolution rounding a
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
        self.pomadeState = state
        self.session = session
        self.viewport = viewport


ALL_MODES = ("graph", "tube", "fill", "hierarchy", "sculpt", "output")


def _stateOnlyRoundTrip(pomadePanels, modeId, state):
    for d in pomadePanels.descriptors(modeId, state):
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


def parameterWidgetRules(pomadePanels, pomadeToolState, pomadeSculpt):
    """DK-06: ranges, the amplified row, stable level rows, readouts."""
    # The Output row goes to the model, not only to the state field.
    dll = FakeDll()
    session = FakeSession(dll)
    state = pomadeToolState.PomadeToolState()
    state.outputEnabled = True
    amplified = next(d for d in pomadePanels.descriptors("output", state)
                     if d.id == "showAmplifiedHair")
    published = session.publishCount
    amplified.set(state, session, True)
    check(("Pomade_SetAmplifiedHair", 1, 1) in dll.calls and
          dll.amplified == 1 and state.showAmplifiedHair is True and
          session.publishCount == published + 1,
          "output.showAmplifiedHair.set calls Pomade_SetAmplifiedHair and "
          "publishes (%r)" % [c for c in dll.calls if "Amplified" in c[0]])
    dll.amplified = 0
    check(amplified.get(state, session) is False and
          state.showAmplifiedHair is False,
          "output.showAmplifiedHair.get reads the model's switch back")

    # A session with the PomadeSession entry takes that one path.
    class AmplifiedSession(object):
        def __init__(self):
            self.dll = None
            self.ctx = None
            self.calls = []

        def setAmplifiedHair(self, flag):
            self.calls.append(bool(flag))
            return True
    viaSession = AmplifiedSession()
    amplified.set(state, viaSession, True)
    check(viaSession.calls == [True],
          "output.showAmplifiedHair.set prefers session.setAmplifiedHair")

    # Every numeric row starts inside its own range on a fresh model.
    for outputBuilt in (False, True):
        dll = FakeDll()
        session = FakeSession(dll)
        for modeId in ALL_MODES:
            fresh = pomadeToolState.PomadeToolState()
            fresh.outputEnabled = outputBuilt
            for d in pomadePanels.descriptors(modeId, fresh):
                if d.kind not in ("int", "float"):
                    continue
                value = d.get(fresh, session)
                check(float(d.min) <= float(value) <= float(d.max),
                      "%s.%s: fresh value %r lies in [%r, %r]"
                      % (modeId, d.id, value, d.min, d.max))

    # Brush radius and strength ranges come from pomadeSculpt.
    state = pomadeToolState.PomadeToolState()
    sculpt = {d.id: d for d in pomadePanels.descriptors("sculpt", state)}
    check(sculpt["brushRadiusPx"].min == pomadeSculpt.BRUSH_RADIUS_MIN_PX and
          sculpt["brushRadiusPx"].max == pomadeSculpt.BRUSH_RADIUS_MAX_PX,
          "sculpt.brushRadiusPx uses the shared [%g, %g] px range"
          % (pomadeSculpt.BRUSH_RADIUS_MIN_PX,
             pomadeSculpt.BRUSH_RADIUS_MAX_PX))
    check(sculpt["sculptStrength"].max == pomadeSculpt.STRENGTH_MAX,
          "sculpt.sculptStrength max is %g for the grab/comb brushes"
          % pomadeSculpt.STRENGTH_MAX)
    state.sculptSubMode = "smooth"
    smooth = {d.id: d for d in pomadePanels.descriptors("sculpt", state)}
    check(smooth["sculptStrength"].max == 1.0,
          "the Smooth brush's strength row stops at 1.0")

    # The level rows no longer bake the level in: same signature, the
    # level in a tooltip instead.
    state = pomadeToolState.PomadeToolState()
    state.activeLevel = 1
    first = [(d.id, d.label) for d in
             pomadePanels.descriptors("hierarchy", state)]
    state.activeLevel = 3
    third = [(d.id, d.label) for d in
             pomadePanels.descriptors("hierarchy", state)]
    labels = dict(third)
    check(first == third and
          labels.get("levelVisible") == "This level visible" and
          labels.get("levelXray") == "This level see-through",
          "hierarchy level rows keep one label across levels (%r)"
          % [row for row in third if row[0].startswith("level")])
    check("L3" in pomadePanels.levelRowTooltip("levelVisible", state) and
          "L3" in pomadePanels.levelRowTooltip("levelXray", state) and
          pomadePanels.levelRowTooltip("subdivideCount", state) == "",
          "the level rows name the active level in their tooltip")

    # Slider rows are bounded floats; Density carries its unit.
    state = pomadeToolState.PomadeToolState()
    byId = {}
    for modeId in ALL_MODES:
        for d in pomadePanels.descriptors(modeId, state):
            byId.setdefault(d.id, d)
    for rowId in sorted(pomadePanels.SLIDER_PARAM_IDS):
        d = byId.get(rowId)
        check(d is not None and d.kind == "float" and d.max > d.min,
              "slider row %r is a bounded float descriptor" % rowId)
    check(pomadePanels.SLIDER_GESTURE_IDS <= pomadePanels.SLIDER_PARAM_IDS,
          "every gesture-bracketed slider row is a slider row")
    check(byId["density"].max == 1000.0 and
          byId["density"].unit == "/unit²",
          "Fill density reaches 1000 and reads per unit area")
    dll = FakeDll()
    dll.guideCount = 42
    check(pomadePanels.guideCountText(FakeSession(dll)) == "42 guides" and
          pomadePanels.guideCountText(None) == "",
          "the density readout shows the live guide count (%r)"
          % pomadePanels.guideCountText(FakeSession(dll)))
    check(":" in pomadePanels.RAMP_HINT,
          "the ramp hint shows the pos:val form")


def dockTruthRules(pomadePanels, pomadeToolState, pomadeHud, pomadeModes):
    """Task D: the dock shows what it commits, and says so before Bind."""
    # -- Lock rows: the selected tubes' own flags, else the default --------
    dll = FakeDll()
    session = FakeSession(dll)
    state = pomadeToolState.PomadeToolState()
    rows = {d.id: d for d in pomadePanels.descriptors("hierarchy", state)}
    lockC = rows["lockChildren"]
    dll.selection = [3]
    lockC.set(state, session, True)
    check(("Pomade_SetLockChildren", 1, 3, 1) in dll.calls and
          state.lockChildren is False,
          "Lock children with T3 selected writes T3 only, not the global "
          "default (%r, global %r)"
          % ([c for c in dll.calls if c[0] == "Pomade_SetLockChildren"],
             state.lockChildren))
    check(lockC.get(state, session) is True,
          "and the row reads T3's flag back (%r)" % lockC.get(state, session))
    dll.selection = [5]
    check(lockC.get(state, session) is False,
          "a tube nobody locked shows unticked (%r)"
          % lockC.get(state, session))
    dll.selection = [3, 5]
    check(lockC.get(state, session) == pomadePanels.MIXED,
          "T3 locked + T5 not shows the mixed (part-checked) state (%r)"
          % lockC.get(state, session))
    del dll.calls[:]
    lockC.set(state, session, True)
    check(sorted(c[2] for c in dll.calls
                 if c[0] == "Pomade_SetLockChildren") == [3, 5] and
          lockC.get(state, session) is True,
          "a click on the mixed row sets both tubes (%r)" % dll.calls)
    dll.selection = []
    check(lockC.get(state, session) is False,
          "with nothing selected the row shows the global default (off)")
    del dll.calls[:]
    lockC.set(state, session, True)
    check(("Pomade_SetLockChildren", 1, -1, 1) in dll.calls and
          state.lockChildren is True and lockC.get(state, session) is True,
          "and writes it through the model's -1 slot (%r)" % dll.calls)
    fresh = {d.id: d for d in pomadePanels.descriptors("hierarchy", state)}
    check("default is on" in fresh["lockChildren"].tooltip and
          "default is on" not in fresh["lockParents"].tooltip,
          "a lock row's tooltip says when the default locks every tube")
    check("rigidly" in rows["lockChildren"].tooltip,
          "Lock children says the children ride rigidly with the parent "
          "(%r)" % rows["lockChildren"].tooltip)
    tubeTips = {a.id: a.tooltip for a in pomadePanels.actions("tube")}
    check("root CV" in tubeTips["matchSurface"] and
          "scalp" in tubeTips["matchSurface"],
          "Match surface says it snaps the root CV onto the scalp (%r)"
          % tubeTips["matchSurface"])

    # -- Relax that moves nothing leaves no undo step (SS-02) ---------------
    class CentersBridge(object):
        centers = {0: [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 2.0, 0.0)]}

        @classmethod
        def tubeCenters(cls, dll, model, tubeId):
            return list(cls.centers[int(tubeId)])

    previousBridge = sys.modules.get("pomadeBridge")
    sys.modules["pomadeBridge"] = CentersBridge
    try:
        relaxDll = FakeDll()
        relaxSession = FakeGestureSession(relaxDll)
        messages = []
        relaxSession.report = lambda text, level="info": messages.append(
            (text, level))
        container = FakeContainer(pomadeToolState.PomadeToolState(),
                                  relaxSession)
        tubeActions = {a.id: a for a in pomadePanels.actions("tube")}
        tubeActions["relax"].handler(container)
        names = [c[0] for c in relaxDll.calls
                 if c[0] in ("Pomade_BeginGesture", "Pomade_RelaxCenter",
                             "Pomade_EndGesture", "Pomade_CancelGesture")]
        check(names == ["Pomade_BeginGesture", "Pomade_RelaxCenter",
                        "Pomade_CancelGesture"],
              "a Relax that moves no CV cancels its bracket: no empty undo "
              "step (%r)" % names)
        check(messages and "Relax" in messages[-1][0] and
              "no change" in messages[-1][0],
              "and the message says Relax made no change (%r)"
              % messages[-1:])
        # A Relax that moves a CV keeps exactly one step.
        del relaxDll.calls[:]

        def relaxMoves(ctx, strength, iterations):
            relaxDll.calls.append(("Pomade_RelaxCenter", ctx))
            CentersBridge.centers = {0: [(0.0, 0.0, 0.0), (0.1, 1.0, 0.0),
                                         (0.0, 2.0, 0.0)]}
            return 0
        relaxDll.Pomade_RelaxCenter = relaxMoves
        tubeActions["relax"].handler(container)
        names = [c[0] for c in relaxDll.calls
                 if c[0] in ("Pomade_BeginGesture", "Pomade_EndGesture",
                             "Pomade_CancelGesture")]
        check(names == ["Pomade_BeginGesture", "Pomade_EndGesture"],
              "a Relax that moves a CV is one undo step (%r)" % names)
        check("no change" not in messages[-1][0] and
              "Relax" in messages[-1][0] and "T0" in messages[-1][0],
              "and the message says what it relaxed (%r)" % messages[-1:])
    finally:
        if previousBridge is None:
            sys.modules.pop("pomadeBridge", None)
        else:
            sys.modules["pomadeBridge"] = previousBridge

    # -- before Bind: no green 'Synced' ------------------------------------
    strip = pomadeHud.statusStrip(pomadeToolState.PomadeToolState(), None)
    got = pomadeHud.syncSummary(strip, {"active": False})
    check(got == ("No scalp bound", pomadeHud.TONE_NEUTRAL),
          "with no model the pill reads 'No scalp bound', neutral (%r)"
          % (got,))
    got = pomadeHud.syncSummary(strip, {"active": False,
                                       "commitError": "stale"})
    check(got[0] == "No scalp bound",
          "no model outranks a stale commit error (%r)" % (got,))
    check(pomadeHud.syncSummary(strip, {"active": True}) == ("Synced", "ok"),
          "a bound, idle model still reads Synced")
    check(pomadeModes.UNBOUND_TITLE == "Bind a scalp mesh to start" and
          pomadeModes.UNBOUND_HINT.startswith(pomadeModes.UNBOUND_TITLE),
          "the unbound HUD title and hint name the first step")

    # -- instruction strings name what the keys and clicks really do -------
    tubeHint = pomadeModes.hintFor("tube", "tube")
    check("whole tubes" in tubeHint and "F8" in tubeHint,
          "Tube's hint says Delete removes whole tubes in Whole tube (F8) "
          "(%r)" % tubeHint)
    hier = dict((m.id, m.status) for m in pomadeModes.HIERARCHY_SUBMODES)
    check("click" in hier["merge"] and "fold" in hier["merge"],
          "Merge's status says a click folds children (%r)" % hier["merge"])
    check("click" in hier["levels"] and "solo" in hier["levels"],
          "Levels' status says a click solos (%r)" % hier["levels"])
    check("box" in hier["group"] and "group" in hier["group"],
          "Group's status says a box groups (%r)" % hier["group"])
    # selectModeFor: a Shift box adds and a Ctrl box REMOVES, so the Group
    # status must not say a Ctrl box gathers tubes for the Group button.
    check("gather" not in hier["group"] and
          "Shift box adds" in hier["group"] and
          "Ctrl box removes" in hier["group"],
          "Group's status says Shift boxes add, Ctrl boxes remove (%r)"
          % hier["group"])
    modes = dict((m.id, m.status) for m in pomadeModes.MODES)
    check("length ramp" in modes["fill"] and "preview" not in modes["fill"],
          "Fill's shelf tooltip names the Length ramp sub-mode (%r)"
          % modes["fill"])
    check("save" not in modes["output"].lower() and
          "maps" not in modes["output"] and
          "hair description" in modes["output"],
          "Output's shelf tooltip names what the Output page holds, not "
          "Save (the file row's) (%r)" % modes["output"])
    hierHint = pomadeModes.hintFor("hierarchy", "navigate")
    check("Q/W/E/R" in hierHint,
          "Hierarchy's hint names Q with W/E/R, as the transform row's "
          "Select (Q) button (%r)" % hierHint)
    # The Length profile field's placeholder agrees with its tooltip.
    check("full length" in pomadePanels.RAMP_PLACEHOLDER and
          "uniform" not in pomadePanels.RAMP_PLACEHOLDER,
          "the empty ramp field reads 'empty = full length' (%r)"
          % pomadePanels.RAMP_PLACEHOLDER)
    # -- warnings rows: the tooltip says what the click does -------------
    retry = pomadeHud._retryCommitAction()
    outline = pomadeHud._highlightUncoveredAction()
    check(pomadeHud.clickHint(retry) == " - click to retry commit",
          "the commit-failure row's tooltip says the click retries "
          "(%r)" % pomadeHud.clickHint(retry))
    check(pomadeHud.clickHint(outline) == " - click to outline the faces",
          "the coverage row's tooltip says the click outlines faces (%r)"
          % pomadeHud.clickHint(outline))
    check(pomadeHud.clickHint(lambda container: None) ==
          pomadeHud.CLICK_TO_SELECT and pomadeHud.clickHint(None) == "",
          "a select action still reads 'click to select'; no action, no "
          "suffix")


def artistFacingRules(pomadePanels, pomadeToolState, pomadeHierarchy,
                      pomadeModes, pomadeFill, pomadeSculpt):
    """DK-07: every row explains itself, in words, with no kernel jargon."""
    import re
    kernelJargon = re.compile(r"\bK[0-9]")
    placeholderLabel = re.compile(r"\(0 =|\bt\)")

    # Every row in every state variant the builders branch on.
    variants = []
    for outputBuilt in (False, True):
        for brushReach in (0.0, 0.3):
            for solo in (-1, 2):
                state = pomadeToolState.PomadeToolState()
                state.outputEnabled = outputBuilt
                state.brushTRadius = brushReach
                state.soloLevel = solo
                state.showMaxLevel = solo
                variants.append(state)
    seen = {}
    for state in variants:
        for modeId in ALL_MODES:
            for d in pomadePanels.descriptors(modeId, state):
                seen.setdefault((modeId, d.id), []).append(d)
    noTip = sorted("%s.%s" % key for key, rows in seen.items()
                   if not all(str(d.tooltip).strip() for d in rows))
    check(not noTip, "every descriptor has a non-empty tooltip (%r)" % noTip)
    badLabels = sorted(d.label for rows in seen.values() for d in rows
                       if placeholderLabel.search(d.label))
    check(not badLabels,
          "no label reads '(0 = ...)' or '(t)' (%r)" % badLabels)
    enums = [d for rows in seen.values() for d in rows if d.kind == "enum"]
    check(enums and all(len(d.choiceLabels) == len(d.choices)
                        for d in enums),
          "every enum has one label per choice")
    rows = {d.id: d for (m, _i), ds in seen.items() for d in ds}
    check(rows["selectionShape"].choiceLabels == ("Box", "Lasso") and
          rows["splitMode"].choiceLabels == ("K-means", "Edge"),
          "enums show human names: %r / %r"
          % (rows["selectionShape"].choiceLabels,
             rows["splitMode"].choiceLabels))
    texel = rows["texelResolution"]
    check(texel.label == "Bake resolution" and
          texel.choiceLabels[0] == "Auto (64 on boundaries)" and
          "4 x 4" in texel.choiceLabels and texel.choices[0] == "auto",
          "the texel row is 'Bake resolution' with sized choices (%r)"
          % (texel.choiceLabels[:3],))
    ring = rows["ringCvCount"]
    check(ring.label == "Ring CVs for new tubes" and
          ring.choiceLabels[0] == "Match region CVs (Auto)",
          "ringCvCount reads 'Ring CVs for new tubes' with an Auto entry")
    check(rows["softRadius"].label == "Soft selection falloff" and
          rows["displaySegments"].label == "Curve smoothness (display)" and
          rows["outputDensityMultiplier"].label ==
          "Strand density multiplier",
          "renamed rows use the artist wording")
    check(all(str(d.unit) for d in (rows["density"], rows["brushRadiusPx"],
                                    rows["snapRadiusPx"])),
          "density and pixel rows carry a unit")

    # Output: the rows exist, greyed, before the first Build.
    state = pomadeToolState.PomadeToolState()
    before = {d.id: d for d in pomadePanels.descriptors("output", state)}
    check("outputDensityMultiplier" in before and
          "outputStrandWidth" in before and
          before["outputDensityMultiplier"].enabled is False and
          before["outputStrandWidth"].enabled is False and
          "Build" in before["outputDensityMultiplier"].tooltip,
          "Output shows density/width disabled before Build, with a hint")
    state.outputEnabled = True
    after = {d.id: d for d in pomadePanels.descriptors("output", state)}
    check(after["outputDensityMultiplier"].enabled is True and
          after["outputStrandWidth"].enabled is True and
          [d for d in before] == [d for d in after],
          "Build enables the same Output rows in the same order")
    build = [a for a in pomadePanels.actions("output")
             if a.id == "buildDescription"]
    check(build and build[0].label == "Build hair description" and
          build[0].tooltip,
          "the Output action is 'Build hair description' with a tooltip")
    noActionTip = sorted(a.id for m in ALL_MODES
                         for a in pomadePanels.actions(m) if not a.tooltip)
    check(not noActionTip, "every action has a tooltip (%r)" % noActionTip)

    # Preview density is a percentage over the 0-1 fraction.
    state = pomadeToolState.PomadeToolState()
    preview = next(d for d in pomadePanels.descriptors("fill", state)
                   if d.id == "previewFraction")
    preview.set(state, None, 40.0)
    check(preview.unit == "%" and preview.max == 100.0 and
          abs(state.previewFraction - 0.4) < 1e-9 and
          abs(preview.get(state, None) - 40.0) < 1e-9,
          "Preview density while dragging reads 0-100 %% (%r)"
          % state.previewFraction)

    # Brush reach: Whole strand is the old 0, and unticking brings a reach.
    state = pomadeToolState.PomadeToolState()
    sculpt = {d.id: d for d in pomadePanels.descriptors("sculpt", state)}
    whole, reach = sculpt["brushWholeStrand"], sculpt["brushTRadius"]
    check(whole.get(state, None) is True and reach.enabled is False and
          reach.min > 0.0,
          "a fresh brush reaches the whole strand; Brush reach is greyed")
    reach.set(state, None, 0.4)
    check(state.brushTRadius == 0.0,
          "setting a reach while Whole strand is ticked keeps it unbounded")
    whole.set(state, None, False)
    check(abs(state.brushTRadius - 0.4) < 1e-9 and
          next(d for d in pomadePanels.descriptors("sculpt", state)
               if d.id == "brushTRadius").enabled is True,
          "unticking Whole strand applies the remembered reach (%r)"
          % state.brushTRadius)
    whole.set(state, None, True)
    check(state.brushTRadius == 0.0 and
          abs(reach.get(state, None) - 0.4) < 1e-9,
          "ticking it again unbounds the brush and keeps the reach shown")

    # Solo level / Show levels up to: a checkbox and a level each.
    state = pomadeToolState.PomadeToolState()
    hier = {d.id: d for d in pomadePanels.descriptors("hierarchy", state)}
    hier["soloLevel"].set(state, None, 3)
    check(state.soloLevel == pomadeHierarchy.SOLO_OFF,
          "picking a solo level with Solo unticked does not solo")
    hier["soloLevelOn"].set(state, None, True)
    check(state.soloLevel == 3 and hier["soloLevelOn"].get(state, None),
          "ticking Solo level solos the picked level (%r)" % state.soloLevel)
    hier["soloLevelOn"].set(state, None, False)
    check(state.soloLevel == pomadeHierarchy.SOLO_OFF and
          hier["soloLevel"].get(state, None) == 3,
          "unticking turns Solo off and keeps the level shown")
    hier["showAllLevels"].set(state, None, False)
    hier["showMaxLevel"].set(state, None, 2)
    check(state.showMaxLevel == 2 and
          not hier["showAllLevels"].get(state, None),
          "Show levels up to 2 applies once Show all levels is unticked")
    hier["showAllLevels"].set(state, None, True)
    check(state.showMaxLevel == pomadeHierarchy.SHOW_ALL_LEVELS,
          "Show all levels clears the limit")

    # No kernel numbers in anything the artist reads.
    texts = []
    for rows_ in seen.values():
        for d in rows_:
            texts.extend((d.label, d.tooltip) + tuple(d.choiceLabels))
    for modeId in ALL_MODES:
        for a in pomadePanels.actions(modeId):
            texts.extend((a.label, a.tooltip))
    for table in (pomadeModes.MODES, pomadeModes.GRAPH_SUBMODES,
                  pomadeModes.TUBE_SUBMODES, pomadeModes.FILL_SUBMODES,
                  pomadeModes.HIERARCHY_SUBMODES,
                  pomadeModes.SCULPT_SUBMODES):
        texts.extend(m.status for m in table)
        texts.extend(m.label for m in table)
    texts.extend(pomadeModes.HINTS.values())
    state = pomadeToolState.PomadeToolState()
    texts.extend((
        pomadeHierarchy.groupStatus([1, 2], True),
        pomadeHierarchy.groupStatus([1, 2], False),
        pomadeHierarchy.mergeChildrenStatus("T1"),
        pomadeHierarchy.mergeSelectedStatus([1, 2]),
        pomadeHierarchy.subdivideStatus(4, "kmeans", [1]),
        pomadeHierarchy.resubdivideConfirm("T1", 2, 4),
        pomadeHierarchy.hierarchyStatus(1, 3, [1, 2], 2, -1, True, True),
        pomadeHierarchy.setSoloLevel(state, 2),
        pomadeHierarchy.setShowMaxLevel(state, 2),
        pomadeFill.fillStatus(8.0, 8, 0.0, 0, [], False),
        pomadeSculpt.sculptStatus("grab", 24.0, 0.0, True, False),
    ))
    jargon = sorted(t for t in texts if kernelJargon.search(str(t)))
    check(not jargon, "no K-number reaches an artist string (%r)" % jargon)


def main():
    _load("pomadeFill")
    _load("pomadeGraph")
    _load("pomadeHierarchy")
    _load("pomadeSculpt")
    _load("pomadeTube")
    # pomadeHud reads the refill drop list through pomadeLib (Qt- and
    # DLL-free at import), so its bare-name fallback needs it registered.
    _load("pomadeLib")
    # The Tube gizmo rows (GZ-05, parity G20) read pomadeGizmoSettings,
    # which reads pomadeGizmoScreen's tool tokens.
    _load("pomadeGizmoScreen")
    pomadeGizmoSettings = _load("pomadeGizmoSettings")
    pomadeToolState = _load("pomadeToolState")
    pomadePanels = _load("pomadePanels")
    pomadeHud = _load("pomadeHud")

    check(callable(pomadePanels.descriptors) and callable(pomadePanels.actions),
          "pomadePanels exposes descriptors() and actions()")
    check(callable(pomadeHud.statusStrip) and callable(pomadeHud.warnings),
          "pomadeHud exposes statusStrip() and warnings()")
    check(pomadeHud.hudStatus is not None
          and pomadeHud.hierarchyStatus is not None
          and pomadeHud.sculptStatus is not None
          and pomadeHud.fillStatus is not None
          and pomadeHud.tubeStatus is not None,
          "pomadeHud re-exports the five per-mode HUD one-liners")

    # -- descriptors: shape + no-session (state-only) round trip --------
    for modeId in ALL_MODES:
        state = pomadeToolState.PomadeToolState()
        descs = pomadePanels.descriptors(modeId, state)
        check(len(descs) > 0, "%s: descriptors() is non-empty" % modeId)
        ids = [d.id for d in descs]
        check(len(ids) == len(set(ids)),
              "%s: descriptor ids are unique" % modeId)
        _stateOnlyRoundTrip(pomadePanels, modeId, state)

    checkRaises(
        ValueError,
        lambda: next(d for d in pomadePanels.descriptors(
            "fill", pomadeToolState.PomadeToolState())
            if d.id == "lengthProfile").set(
                pomadeToolState.PomadeToolState(), None, [1.0]),
        "an odd length-profile write raises")

    # -- descriptors: through a live fake session ------------------------
    dll = FakeDll()
    session = FakeSession(dll)
    state = pomadeToolState.PomadeToolState()

    graphDescs = {d.id: d for d in pomadePanels.descriptors("graph", state)}
    # The row is screen pixels; the model's snap radius is rest units, so
    # the row is state-only (the loops convert px at the point of use). A
    # write-through would hand the model pixels as world units, and every
    # refresh would read the world radius back into the pixel field.
    modelRadius = dll.snapRadius
    graphDescs["snapRadiusPx"].set(state, session, 12.0)
    check(state.snapRadiusPx == 12.0
          and graphDescs["snapRadiusPx"].get(state, session) == 12.0,
          "graph.snapRadiusPx round-trips its pixels through the state")
    check(dll.snapRadius == modelRadius and
          not any(c[0] in ("Pomade_SetSnapRadius", "Pomade_GetSnapRadius")
                  for c in dll.calls),
          "and never writes (or reads back) the model's world radius")
    graphDescs["mirrorX"].set(state, session, True)
    check(dll.mirrorX == 1 and graphDescs["mirrorX"].get(state, session)
          is True, "graph.mirrorX writes through Pomade_SetMirrorX")

    tubeDescs = {d.id: d for d in pomadePanels.descriptors("tube", state)}
    tubeDescs["softRadius"].set(state, session, 0.4)
    check(abs(dll.softRadius - 0.4) < 1e-6,
          "tube.softRadius writes through Pomade_SetSoftSelection")
    tubeDescs["displaySegments"].set(state, session, 3)
    check(dll.displaySegments == 3,
          "tube.displaySegments writes through Pomade_SetDisplaySegments")
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

    # -- GZ-05 / parity G20: the Tube gizmo settings rows -----------------
    gizmoState = pomadeToolState.PomadeToolState()
    gizmoDescs = {d.id: d for d in pomadePanels.descriptors("tube",
                                                           gizmoState)}
    wanted = ("transformOrientation", "stepSnap", "stepSize", "freeRotate",
              "preventNegativeScale", "gridSize", "manipulatorSize")
    check(all(name in gizmoDescs for name in wanted),
          "the Tube panel carries the gizmo settings rows (%r)"
          % [name for name in wanted if name not in gizmoDescs])
    orientation = gizmoDescs["transformOrientation"]
    check(orientation.kind == "enum" and
          tuple(orientation.choices) == ("world", "screen", "tube") and
          orientation.get(gizmoState, None) == "world",
          "Axis orientation offers world/screen/tube and starts at World")
    notified = []
    settings = pomadeGizmoSettings.settingsFor(gizmoState)
    settings.AddListener(lambda: notified.append(1))
    for value in ("tube", "screen", "world"):
        orientation.set(gizmoState, None, value)
        check(gizmoState.transformOrientation == value and
              orientation.get(gizmoState, None) == value,
              "transformOrientation round-trips %r" % value)
    check(len(notified) == 3,
          "each orientation change tells the gizmo listeners (%d)"
          % len(notified))
    orientation.set(gizmoState, None, "sideways")
    check(gizmoState.transformOrientation == "world",
          "an unknown orientation falls back to World")
    gizmoState.transformTool = "rotate"
    check(abs(gizmoDescs["stepSize"].get(gizmoState, None) - 15.0) < 1e-9,
          "Step size follows the live tool: Rotate starts at 15 degrees")
    gizmoDescs["stepSize"].set(gizmoState, None, 5.0)
    gizmoState.transformTool = "move"
    check(abs(gizmoDescs["stepSize"].get(gizmoState, None) - 1.0) < 1e-9 and
          settings.For("rotate").stepSize == 5.0,
          "each tool keeps its own step size")
    gizmoDescs["freeRotate"].set(gizmoState, None, False)
    check(settings.For("rotate").freeRotate is False,
          "Free rotate writes the Rotate tool's option from any tool")
    gizmoDescs["preventNegativeScale"].set(gizmoState, None, True)
    check(settings.For("scale").preventNegativeScale is True,
          "Prevent negative scale writes the Scale tool's option")
    gizmoDescs["manipulatorSize"].set(gizmoState, None, 1000.0)
    check(gizmoDescs["manipulatorSize"].get(gizmoState, None) ==
          pomadeGizmoSettings.MANIPULATOR_SIZE_MAX,
          "the manipulator size row clamps like the '+' key")
    gizmoState.transformTool = "rotate"
    reset = {a.id: a for a in pomadePanels.actions("tube")}
    check("resetTransformTool" in reset, "Tube offers Reset transform tool")

    class _Container(object):
        pomadeState = gizmoState
        session = None

    reset["resetTransformTool"].handler(_Container())
    check(settings.For("rotate").stepSize == 15.0 and
          settings.For("rotate").freeRotate is True and
          settings.For("scale").preventNegativeScale is True,
          "Reset transform tool restores only the live tool's defaults")

    fillDescs = {d.id: d for d in pomadePanels.descriptors("fill", state)}
    # MD-01: a fresh model holds the pomadeModel.h default density (100); a
    # descriptor range that excludes it makes the spinbox show a clamped
    # number the model does not hold.
    freshDll = FakeDll()
    freshDll.fill["density"] = 100.0
    density = fillDescs["density"]
    shown = density.get(pomadeToolState.PomadeToolState(),
                        FakeSession(freshDll))
    check(density.min <= shown <= density.max and shown == 100.0,
          "fill.density range [%g, %g] holds a fresh model's %g"
          % (density.min, density.max, shown))
    fillDescs["density"].set(state, session, 16.0)
    check(dll.fill["density"] == 16.0,
          "fill.density writes through Pomade_SetFillParams")
    fillDescs["lengthProfile"].set(state, session, [0.0, 1.0, 1.0, 0.5])
    check(dll.fill["profile"] == [0.0, 1.0, 1.0, 0.5],
          "fill.lengthProfile writes through Pomade_SetFillParams")

    # SS-02: a dock Fill edit is ONE undo step -- Begin, the write and its
    # refill, End -- and a write of the value already held leaves none.
    gestureDll = FakeDll()
    gestureSession = FakeGestureSession(gestureDll)
    gestureState = pomadeToolState.PomadeToolState()
    gestureFill = {d.id: d for d in pomadePanels.descriptors("fill",
                                                            gestureState)}
    bracket = ("Pomade_BeginGesture", "Pomade_SetFillParams",
               "Pomade_RefillGuides", "Pomade_EndGesture",
               "Pomade_CancelGesture")

    def bracketCalls():
        names = [c for c in gestureDll.calls if c[0] in bracket]
        del gestureDll.calls[:]
        return names

    gestureFill["density"].set(gestureState, gestureSession, 24.0)
    calls = bracketCalls()
    check([c[0] for c in calls] == ["Pomade_BeginGesture",
                                    "Pomade_SetFillParams",
                                    "Pomade_RefillGuides",
                                    "Pomade_EndGesture"] and
          calls[0][1] == "Fill params" and gestureDll.fill["density"] == 24.0,
          "fill.density set() records Pomade_BeginGesture('Fill params') / "
          "Pomade_EndGesture around Pomade_SetFillParams (%r)" % (calls,))
    gestureFill["density"].set(gestureState, gestureSession, 24.0)
    calls = [c[0] for c in bracketCalls()]
    check(calls == ["Pomade_BeginGesture", "Pomade_SetFillParams",
                    "Pomade_RefillGuides", "Pomade_CancelGesture"],
          "re-writing the density already held cancels the bracket, so no "
          "empty undo step is left (%r)" % (calls,))
    gestureSession.gestureActive = True       # the dock's slider bracket
    gestureFill["edgeBias"].set(gestureState, gestureSession, 0.25)
    calls = [c[0] for c in bracketCalls()]
    gestureSession.gestureActive = False
    check(calls == ["Pomade_SetFillParams", "Pomade_RefillGuides"],
          "inside an open gesture the write joins it instead of nesting "
          "(%r)" % (calls,))

    class _GestureOutputSession(FakeOutputSession):
        ctx = 1

        def __init__(self):
            FakeOutputSession.__init__(self)
            self.log = []
            self.gestureActive = False

        def beginGesture(self, label):
            self.log.append(("begin", label))
            return True

        def endGestureIfChanged(self, changed):
            self.log.append(("end" if changed else "cancel",))
            return bool(changed)

        def enqueueCommit(self):
            self.log.append(("enqueue",))
            return True

    outputSession = _GestureOutputSession()
    outputState = pomadeToolState.PomadeToolState()
    outputDescs = {d.id: d for d in pomadePanels.descriptors("output",
                                                            outputState)}
    widthRow = next((outputDescs[k] for k in ("strandWidth", "width",
                                              "outputStrandWidth")
                     if k in outputDescs), None)
    if widthRow is None:
        check(False, "the Output panel has a strand width row (%r)"
              % sorted(outputDescs))
    else:
        widthRow.set(outputState, outputSession, 0.02)
        check(outputSession.log[:2] == [("begin", "Output settings"),
                                        ("end",)] and
              outputSession.strandWidth == 0.02,
              "an Output width edit is one 'Output settings' undo step (%r)"
              % (outputSession.log,))
        del outputSession.log[:]
        widthRow.set(outputState, outputSession, 0.02)
        check(("cancel",) in outputSession.log and
              ("end",) not in outputSession.log,
              "and the same width again leaves no step (%r)"
              % (outputSession.log,))
    # The row reads 0-100 % (DK-07); the ABI still takes the fraction.
    fillDescs["previewFraction"].set(state, session, 50.0)
    check(dll.previewFraction == 0.5 and
          fillDescs["previewFraction"].get(state, session) == 50.0,
          "fill.previewFraction writes 50 % through Pomade_SetPreviewFraction "
          "as 0.5")
    fillDescs["freezeRoots"].set(state, session, True)
    check(dll.freezeRoots == 1,
          "fill.freezeRoots writes through Pomade_SetFreezeRoots")

    hierDescs = {d.id: d for d in pomadePanels.descriptors("hierarchy",
                                                          state)}
    state.activeLevel = 1
    hierDescs["levelVisible"].set(state, session, False)
    check(dll.levelDisplay.get(1, (None, None))[0] == 0,
          "hierarchy.levelVisible writes through Pomade_SetLevelDisplay")
    hierDescs["levelXray"].set(state, session, True)
    check(dll.levelDisplay.get(1, (None, None))[1] == 1,
          "hierarchy.levelXray writes through Pomade_SetLevelDisplay")
    dll.selection = [3, 5]
    hierDescs["lockParents"].set(state, session, True)
    check(any(c[0] == "Pomade_SetLockParents" for c in dll.calls),
          "hierarchy.lockParents pushes to the tube selection")
    dll.selection = []

    # -- Tube section scale: one gesture, atomic failure -----------------
    class FakeBridge(object):
        @staticmethod
        def tubeSection(dll, model, tubeId, ring):
            return (0.0, [], 1.0, 0.0)

    previousBridge = sys.modules.get("pomadeBridge")
    sys.modules["pomadeBridge"] = FakeBridge
    try:
        scaleStage = FakeScaleStage(failAt=1)
        scaleSession = FakeScaleSession(scaleStage)
        scaleState = pomadeToolState.PomadeToolState()
        scaleDescriptor = next(
            d for d in pomadePanels.descriptors("tube", scaleState)
            if d.id == "uniformScale")
        scaleDescriptor.set(scaleState, scaleSession, 2.0)
        check(scaleSession.beginCount == 1 and scaleSession.endCount == 0
              and scaleSession.cancelCount == 1,
              "section scale validates and brackets the selected rings")
        check(len(scaleStage.calls) == 1 and scaleSession.published == [77],
              "a failed section scale cancels the atomic gesture")
    finally:
        if previousBridge is None:
            sys.modules.pop("pomadeBridge", None)
        else:
            sys.modules["pomadeBridge"] = previousBridge

    # -- actions: every action calls its documented ABI and publishes ---
    dll = FakeDll()
    session = FakeSession(dll)
    state = pomadeToolState.PomadeToolState()
    container = FakeContainer(state, session)

    graphActions = {a.id: a for a in pomadePanels.actions("graph")}
    # No viewport, no camera: the session's recorded world-per-pixel at
    # the scalp converts the row's pixels (8 px x 0.01 = 0.08 rest units).
    state.snapRadiusPx = 8.0
    session.displayScale = lambda: 0.01
    graphActions["weldAll"].handler(container)
    welds = [c for c in dll.calls if c[0] == "Pomade_GraphWeldAll"]
    check(welds and session.publishCount == 1,
          "graph weldAll calls Pomade_GraphWeldAll and publishes")
    check(welds and abs(float(welds[-1][2]) - 0.08) < 1e-6,
          "and hands it the snap radius in rest units, not pixels (%r)"
          % (welds[-1:],))
    del session.displayScale
    graphActions["rebake"].handler(container)
    check(session.rebakeCount == 1, "graph rebake calls session.rebake()")

    tubeActions = {a.id: a for a in pomadePanels.actions("tube")}
    tubeActions["matchSurface"].handler(container)
    check(any(c[0] == "Pomade_MatchSurface" for c in dll.calls),
          "tube matchSurface calls Pomade_MatchSurface")
    tubeActions["relax"].handler(container)
    check(any(c[0] == "Pomade_RelaxCenter" for c in dll.calls),
          "tube relax calls Pomade_RelaxCenter")
    tubeActions["snapRoot"].handler(container)
    check(any(c[0] == "Pomade_SnapRootToScalp" for c in dll.calls),
          "tube snapRoot calls Pomade_SnapRootToScalp")

    fillActions = {a.id: a for a in pomadePanels.actions("fill")}
    fillActions["refill"].handler(container)
    check(any(c[0] == "Pomade_RefillGuides" for c in dll.calls),
          "fill refill calls Pomade_RefillGuides")

    dll.selection = [4, 7]
    hierActions = {a.id: a for a in pomadePanels.actions("hierarchy")}
    hierActions["subdivide"].handler(container)
    check(any(c[0] == "Pomade_SubdivideTube" and c[2] == 4 for c in dll.calls)
          and any(c[0] == "Pomade_SubdivideTube" and c[2] == 7
                 for c in dll.calls),
          "hierarchy subdivide subdivides every selected tube")
    hierActions["mergeChildren"].handler(container)
    check(any(c[0] == "Pomade_MergeChildren" for c in dll.calls),
          "hierarchy mergeChildren calls Pomade_MergeChildren")
    hierActions["mergeSelected"].handler(container)
    check(any(c[0] == "Pomade_MergeSelected" for c in dll.calls),
          "hierarchy mergeSelected calls Pomade_MergeSelected")
    hierActions["group"].handler(container)
    check(any(c[0] == "Pomade_GroupTubes" for c in dll.calls),
          "hierarchy group calls Pomade_GroupTubes")
    hierActions["makePersistent"].handler(container)
    check(any(c[0] == "Pomade_MakePersistent" for c in dll.calls),
          "hierarchy makePersistent calls Pomade_MakePersistent")
    dll.selection = []

    check(pomadePanels.actions("sculpt") == [],
          "sculpt has no one-shot actions")
    outputActions = pomadePanels.actions("output")
    check([a.id for a in outputActions] == ["buildDescription"],
          "Output exposes Build hair description beside file commands")

    # -- Output: the texel override reaches the bake (V7) ----------------
    # The descriptor's job is to hand the session a texel count and show
    # back what the session actually applied, then rebake so the map on
    # disk matches the panel. The session's own snapping (powers of two,
    # clamped) is proven against the real bake in
    # testUsdGenPomadeRegionBake; here the wiring is what is under test.
    bakeSession = FakeBakeSession()
    outputDescs = {d.id: d for d in pomadePanels.descriptors("output", state)}
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
    outputDescs = {d.id: d for d in pomadePanels.descriptors("output", state)}
    density = outputDescs["outputDensityMultiplier"]
    width = outputDescs["outputStrandWidth"]
    density.set(state, outputSession, 2.5)
    width.set(state, outputSession, 0.02)
    check(outputSession.outputSettings() == (True, 2.5, 0.02),
          "Output density and strand width reach the session")
    outputActions[0].handler(FakeContainer(state, outputSession))
    check(outputSession.buildCount == 1 and outputSession.enabled,
          "Build hair description enables the committed Output")

    # -- no session: every action no-ops instead of raising --------------
    inertContainer = FakeContainer(pomadeToolState.PomadeToolState(), None)
    for modeId in ("graph", "tube", "fill", "hierarchy"):
        for action in pomadePanels.actions(modeId):
            action.handler(inertContainer)
    check(True, "every action no-ops harmlessly with no session")

    # -- pomadeHud.statusStrip: the amber skew cases -----------------------
    state = pomadeToolState.PomadeToolState()
    state.mapVersion = 5
    state.bakedVersion = 5
    session = FakeSession(FakeDll())
    strip = pomadeHud.statusStrip(state, session)
    check(strip.amber is False, "matched map/baked versions are not amber")

    state.bakedVersion = 3
    strip = pomadeHud.statusStrip(state, session)
    check(strip.amber is True,
          "a baked version behind the map version is amber")

    state.bakedVersion = 5
    session.committedVersion = 7
    strip = pomadeHud.statusStrip(state, session)
    check(strip.amber is True,
          "a committed version behind the pending (enqueued) version is "
          "amber")

    # DK-05: a selection publish bumps the MODEL version and enqueues
    # nothing, so it must not turn the strip amber.
    session.committedVersion = 10
    session.pendingVersion = 10
    session.modelVersion = 13
    strip = pomadeHud.statusStrip(state, session)
    check(strip.amber is False,
          "a model version ahead of an idle committer is not amber")

    check(pomadeHud.statusStrip(state, FakeSession(FakeDll())).gpuText
          == "GPU", "no fallback reason reads as plain GPU")
    fallbackDll = FakeDll()
    fallbackDll.fallbackReason = b"no CUDA device"
    check("CPU-only"
          in pomadeHud.statusStrip(state, FakeSession(fallbackDll)).gpuText,
          "a device fallback reason shows CPU-only in the status strip")
    check(pomadeHud.statusStrip(state, None).breadcrumb != "",
          "statusStrip works with session=None (breadcrumb from state "
          "alone)")

    # -- pomadeHud.syncSummary: the sync pill matrix (DK-05) ---------------
    def pill(mapV=5, bakedV=5, committed=10, pending=10, model=10,
             fallback=b"", status=None):
        pillState = pomadeToolState.PomadeToolState()
        pillState.mapVersion = mapV
        pillState.bakedVersion = bakedV
        pillDll = FakeDll()
        if fallback:
            pillDll.fallbackReason = fallback
        pillSession = FakeSession(pillDll)
        pillSession.committedVersion = committed
        pillSession.pendingVersion = pending
        pillSession.modelVersion = model
        pillStrip = pomadeHud.statusStrip(pillState, pillSession)
        return pillStrip, pomadeHud.syncSummary(pillStrip, status)

    _s, got = pill()
    check(got == ("Synced", "ok"), "idle and matched reads Synced (%r)"
          % (got,))
    _s, got = pill(model=14)
    check(got == ("Synced", "ok"),
          "a selection-only model bump still reads Synced (%r)" % (got,))
    _s, got = pill(pending=11)
    check(got == ("Committing...", "busy"),
          "an enqueued, unswapped commit reads Committing... (%r)" % (got,))
    _s, got = pill(mapV=6)
    check(got == ("Baking map...", "busy"),
          "a map newer than the last bake reads Baking map... (%r)" % (got,))
    _s, got = pill(fallback=b"no CUDA device")
    check(got == ("CPU fallback: no CUDA device", "info"),
          "a device fallback reads CPU fallback: <reason> (%r)" % (got,))
    _s, got = pill(status={"ladderStep": 2})
    check(got == ("Reduced detail", "info"),
          "a stepped ladder reads Reduced detail (%r)" % (got,))
    _s, got = pill(pending=11, status={"commitError": "disk full"})
    check(got == ("Commit failed: disk full", "error"),
          "a commit error outranks work in flight (%r)" % (got,))
    _s, got = pill(pending=11, mapV=6, fallback=b"x")
    check(got[0] == "Committing...",
          "a commit in flight outranks a bake and a fallback (%r)" % (got,))
    _s, got = pill(mapV=6, fallback=b"x", status={"ladderStep": 1})
    check(got[0] == "Baking map...",
          "a bake in flight outranks a standing condition (%r)" % (got,))
    for kwargs in ({}, {"pending": 11}, {"mapV": 6},
                   {"fallback": b"no CUDA device"},
                   {"status": {"ladderStep": 3}}):
        dumpStrip, _pill = pill(**kwargs)
        dump = pomadeHud.diagnosticsText(dumpStrip, kwargs.get("status"))
        check(dump and not dump.lstrip().startswith("|"),
              "the diagnostics line never starts with '|' (%r)" % dump)
    check(pomadeHud.deviceChip(pill()[0]) == "GPU" and
          pomadeHud.deviceChip(pill(fallback=b"x")[0]) == "CPU",
          "the device chip reads GPU, or CPU after a fallback")

    # -- pomadeHud.warnings -------------------------------------------------
    dll = FakeDll()
    session = FakeSession(dll)
    container = FakeContainer(state, session)
    dll.regionStats = (4, 2, 0)
    warns = pomadeHud.warnings(state, session)
    # DK-08: an info row an artist can read and click to see the faces.
    check(any(w.text == "2 scalp faces have no region" and
              w.severity == "info" and w.selectAction is not None
              for w in warns),
          "a coarse uncovered-face count produces a coverage warning")

    dll.intersectedTubes = [2, 5]
    warns = pomadeHud.warnings(state, session)
    intersectWarn = next(w for w in warns if "overlap" in w.text)
    check(intersectWarn.selectAction is not None,
          "the root-intersection warning carries a selectAction")
    intersectWarn.selectAction(container)
    check(any(c[0] == "Pomade_SelectSet" and c[3] == [2, 5]
             for c in dll.calls),
          "clicking the root-intersection warning selects the tubes")

    dll.smoothnessScores = [0.0, 0.05, 0.0]
    warns = pomadeHud.warnings(state, session)
    check(any(w.text.startswith("Kink on tube 0 at CV 1") for w in warns),
          "a smoothness spike produces a warning")

    dll.fallbackReason = b"no CUDA device"
    warns = pomadeHud.warnings(state, session)
    check(any("CPU-only" in w.text for w in warns),
          "a device fallback reason produces a warning")

    session.detached = True
    warns = pomadeHud.warnings(state, session)
    check(any("detached" in w.text for w in warns),
          "a detached committer produces a warning")

    check(pomadeHud.warnings(pomadeToolState.PomadeToolState(), None) == [],
          "warnings() with no session returns no ABI-backed rows")

    # -- pomadeHud.warningsKey (plan/18 V9-perf memoization) ----------------
    # The dock refreshes on every publish, pump and 250 ms tick; the key
    # lets it skip the warnings re-read when nothing it reads has moved.
    keyDll = FakeDll()
    keySession = FakeSession(keyDll)
    keyState = pomadeToolState.PomadeToolState()
    keyDll.regionStats = (4, 2, 0)
    first = pomadeHud.warningsKey(keyState, keySession)
    check(first is not None,
          "warningsKey() returns a key for a versioned session")
    check(pomadeHud.warningsKey(keyState, keySession) == first,
          "warningsKey() is stable across identical inputs")
    rowsFirst = [w.text for w in pomadeHud.warnings(keyState, keySession)]
    rowsAgain = [w.text for w in pomadeHud.warnings(keyState, keySession)]
    check(rowsFirst == rowsAgain,
          "an unchanged key means unchanged warning rows")
    keySession.modelVersion = 11
    check(pomadeHud.warningsKey(keyState, keySession) != first,
          "warningsKey() moves with the model version")
    keySession.modelVersion = 10
    keySession.detached = True
    check(pomadeHud.warningsKey(keyState, keySession) != first,
          "warningsKey() moves when the committer detaches")
    keySession.detached = False
    keyDll.fallbackReason = b"no CUDA device"
    check(pomadeHud.warningsKey(keyState, keySession) != first,
          "warningsKey() moves on a device fallback")
    check(pomadeHud.warningsKey(keyState, None) is None,
          "warningsKey() with no session is None (force a re-read)")

    # DK-01: one word for the scalp. The menu, the dock, the picker and the
    # status lines used to say scalp, geometry and mesh for one thing.
    stale = []
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".py"):
            continue
        with open(os.path.join(SRC, name), encoding="utf-8") as handle:
            text = handle.read().lower()
        for phrase in ("bind geometry", "no geometry bound"):
            if phrase in text:
                stale.append((name, phrase))
    check(not stale,
          "no package module names the scalp 'geometry' (%r)" % (stale,))

    # DK-02: hotkey truth and per-tool instruction lines.
    import re
    pomadeModes = _load("pomadeModes")
    shelves = {
        "graph": pomadeModes.GRAPH_SUBMODES,
        "tube": pomadeModes.TUBE_SUBMODES,
        "fill": pomadeModes.FILL_SUBMODES,
        "hierarchy": pomadeModes.HIERARCHY_SUBMODES,
        "sculpt": pomadeModes.SCULPT_SUBMODES,
    }
    missing = []
    for mode in pomadeModes.MODES:
        subs = shelves.get(mode.id, ())
        for subId in [""] + [sub.id for sub in subs]:
            if not pomadeModes.hintFor(mode.id, subId).strip():
                missing.append((mode.id, subId))
    check(not missing,
          "every (mode, sub-mode) has a non-empty hint (%r)" % (missing,))
    check(all(pomadeModes.hintFor(m, s) == pomadeModes.HINTS[(m, s)]
              for (m, s) in pomadeModes.HINTS),
          "hintFor() answers the HINTS table")
    tubeKeys = [sub.hotkey.lower() for sub in pomadeModes.TUBE_SUBMODES]
    check(tubeKeys == ["f8", "f9", "f10", "f11"] and
          not set(tubeKeys) & {"q", "w", "e", "r", "c"},
          "Tube sub-modes are F8-F11, never a transform letter (%r)"
          % (tubeKeys,))
    kNumbers = []
    for mode in pomadeModes.MODES + tuple(
            sub for subs in shelves.values() for sub in subs):
        if re.search(r"K[0-9]", mode.status):
            kNumbers.append(mode.status)
    kNumbers.extend(text for text in pomadeModes.HINTS.values()
                    if re.search(r"K[0-9]", text))
    check(not kNumbers,
          "no kernel K-number in any mode status or hint (%r)" % (kNumbers,))
    check(pomadeModes.SELECTION_HINT ==
          "Click selects · Shift toggles · Ctrl removes · Ctrl+Shift adds "
          "· drag empty space boxes",
          "SELECTION_HINT is the shared modifier line")
    check(next(sub.label for sub in pomadeModes.FILL_SUBMODES
               if sub.id == "preview") == "Length ramp",
          "Fill's preview sub-mode is labelled Length ramp")
    # Every letter a hint advertises for a sub-mode has to be the one the
    # hotkey table routes: the Graph/Sculpt fallbacks list the shelf keys.
    for modeId in ("graph", "sculpt"):
        text = pomadeModes.hintFor(modeId, "")
        check(all((" %s " % sub.hotkey) in (text + " ")
                  or text.endswith(" " + sub.hotkey)
                  for sub in shelves[modeId]),
              "%s's fallback hint lists its sub-mode keys" % modeId)

    # DK-04: every dock button has one stable objectName and one glyph.
    pomadeDockIds = _load("pomadeDockIds")
    keys = pomadeDockIds.dockButtonKeys(pomadePanels.actions)
    names = [pomadeDockIds.objectName(kind, itemId) for kind, itemId in keys]
    dupes = sorted(set(n for n in names if names.count(n) > 1))
    check(not dupes, "every dock button has its own objectName (%r)"
          % (dupes,))
    check(all(pomadeDockIds.parseObjectName(n) == k
              for n, k in zip(names, keys)),
          "parseObjectName inverts objectName for every dock button")
    for mode in pomadeModes.MODES:
        check(names.count("pomadeMode:%s" % mode.id) == 1,
              "Mode %r maps to exactly one objectName" % mode.id)
        for action in pomadePanels.actions(mode.id):
            check(names.count("pomadeAction:%s" % action.id) == 1,
                  "Action %r (%s) maps to exactly one objectName"
                  % (action.id, mode.id))
    noIcon = [k for k in keys
              if not pomadeDockIds.iconName(k[0], k[1],
                                           pomadePanels.ACTION_ICONS)]
    check(not noIcon, "every dock button names an icon (%r)" % (noIcon,))
    check([t.hotkey for t in pomadeDockIds.TRANSFORM_TOOLS] ==
          ["Q", "W", "E", "R"] and
          [t.id for t in pomadeDockIds.TRANSFORM_TOOLS] ==
          ["select", "move", "rotate", "scale"],
          "the transform row is Select/Move/Rotate/Scale on Q/W/E/R")
    tubeTool = [d for d in pomadePanels.descriptors(
        "tube", pomadeToolState.PomadeToolState()) if d.id == "transformTool"]
    check(len(tubeTool) == 1 and
          set(tubeTool[0].choices) ==
          set(t.id for t in pomadeDockIds.TRANSFORM_TOOLS),
          "the transform row covers the transformTool descriptor's choices")

    parameterWidgetRules(pomadePanels, pomadeToolState,
                         sys.modules["pomadeSculpt"])
    artistFacingRules(pomadePanels, pomadeToolState,
                      sys.modules["pomadeHierarchy"], pomadeModes,
                      sys.modules["pomadeFill"], sys.modules["pomadeSculpt"])
    dockTruthRules(pomadePanels, pomadeToolState, pomadeHud, pomadeModes)

    print("testUsdGenPomadeToolsPanels: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
