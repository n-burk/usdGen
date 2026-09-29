#!/usr/bin/env python3
# testUsdGenPomadeToolsHierarchy -- T1: the Qt-free P4 half of
# usdGenPomadeTools (plan/17 P4 exit, Hierarchy + Sculpt modes).
#
# Hierarchy/sculpt helpers, shelf wiring, tool state and math kernels import
# and behave headlessly (no pxr, no Qt, no usdview). Modules load by file
# path so the package __init__ (which imports pxr.Usdviewq) is never
# executed -- the same guarantee the 08-tools Qt-free rule demands of every
# model module.
import importlib.util
import math as stdmath
import os
import sys

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenPomadeTools"))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        "pomade_%s" % name, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves annotations through sys.modules[__module__].
    sys.modules[spec.name] = module
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


class _NoDll:
    """A dll exporting nothing: every P4 entry is missing."""


def main():
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("SKIP: numpy is not installed")
        return 77

    np = sys.modules["numpy"]
    modes = _load("pomadeModes")
    stateModule = _load("pomadeToolState")
    math = _load("pomadeMath")
    hier = _load("pomadeHierarchy")
    sculpt = _load("pomadeSculpt")

    # -- shelf wiring ----------------------------------------------------
    check([m.id for m in modes.HIERARCHY_SUBMODES] ==
          ["navigate", "subdivide", "merge", "group", "levels"],
          "hierarchy shelf carries the five plan section 5.4 loops")
    check([m.id for m in modes.SCULPT_SUBMODES] ==
          ["grab", "smooth", "comb", "lengthen", "twist"],
          "sculpt shelf carries the five plan section 5.5 brushes")
    check(modes.HierarchySubModeById("merge").label == "Merge"
          and modes.SculptSubModeById("twist").label == "Twist"
          and modes.HierarchySubModeById("nope") is None
          and modes.SculptSubModeById("nope") is None,
          "sub-mode lookup resolves and returns None when unknown")

    state = stateModule.PomadeToolState()
    check(state.hierarchySubMode == "" and state.sculptSubMode == ""
          and state.subdivideCount == 4 and state.splitMode == "kmeans"
          and state.focusNames == ()
          and state.lockParents is False and state.lockChildren is False
          and state.tubeLocks == {} and state.soloLevel == -1
          and state.showMaxLevel == -1 and state.hiddenLevels == set()
          and state.brushRadiusPx == 24.0 and state.brushTRadius == 0.0
          and state.sculptPreserveLength is True
          and state.sculptMirrorX is False,
          "PomadeToolState P4 defaults: locks off, solo off, "
          "length-preserving on")
    status = modes.SetActiveHierarchySubMode(state, "subdivide")
    check(state.hierarchySubMode == "subdivide" and "Shift+D" in status,
          "SetActiveHierarchySubMode records subdivide with a status line")
    status = modes.SetActiveSculptSubMode(state, "grab")
    check(state.sculptSubMode == "grab" and "Grab" in status,
          "SetActiveSculptSubMode records the grab brush")
    before = (state.hierarchySubMode, state.sculptSubMode)
    check(modes.SetActiveHierarchySubMode(state, "nope") == ""
          and modes.SetActiveSculptSubMode(state, "nope") == ""
          and (state.hierarchySubMode, state.sculptSubMode) == before,
          "unknown sub-modes are rejected without touching state")
    check(modes.SetActiveHierarchySubMode(state, "") ==
          "Pomade Hierarchy: no sub-mode."
          and modes.SetActiveSculptSubMode(state, "") ==
          "Pomade Sculpt: no sub-mode.",
          "SetActive*SubMode('') clears each shelf")

    # -- pomadeMath P4 kernels --------------------------------------------
    check(float(math.brushFalloff(0.0, 24.0)) == 1.0
          and float(math.brushFalloff(24.0, 24.0)) == 0.0
          and float(math.brushFalloff(99.0, 24.0)) == 0.0
          and 0.0 < float(math.brushFalloff(12.0, 24.0)) < 1.0,
          "brushFalloff peaks, rims at zero, and stays inside (0, 1)")
    check(float(math.brushFalloff(0.0, 0.0)) == 1.0
          and float(math.brushFalloff(1.0, 0.0)) == 0.0,
          "brushFalloff with zero radius selects only the exact center")
    both = math.brushFalloff([0.0, 12.0], 24.0,
                             t=[0.5, 0.5], center=0.5, tRadius=0.25)
    peak = math.brushFalloff([0.0, 12.0], 24.0)
    check(bool(np.all(both <= peak)) and float(both[0]) == 1.0,
          "the t-range multiplies (never widens) the screen disc")
    line = np.array([[0.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 3.0, 0.0]])
    check(abs(math.polylineLength(line) - 3.0) < 1e-12
          and math.polylineLength(np.zeros((1, 3))) == 0.0,
          "polylineLength sums segments and tolerates one row")
    grid = math.resamplePolyline(line, 7)
    check(grid.shape == (7, 3)
          and bool(np.allclose(grid[0], line[0]))
          and bool(np.allclose(grid[-1], line[-1]))
          and abs(math.polylineLength(grid) - 3.0) < 1e-12,
          "resamplePolyline pins the ends and keeps arc length")
    twin = math.averagePolylines([line, line + [2.0, 0.0, 0.0]], 5)
    check(twin.shape == (5, 3)
          and abs(float(twin[0, 0]) - 1.0) < 1e-12,
          "averagePolylines resamples then averages (K7 preview)")
    fixed = math.averagePolylines([twin, line, line + [2.0, 0.0, 0.0]], 5)
    check(bool(np.allclose(
        math.averagePolylines([fixed, fixed], 5), fixed)),
        "parent averaging is idempotent at its fixed point")
    checkRaises(ValueError, lambda: math.averagePolylines([], 5),
                "averagePolylines rejects an empty set")
    grown = math.enforceLength(line, 6.0)
    check(abs(math.polylineLength(grown) - 6.0) < 1e-12
          and bool(np.allclose(grown[0], line[0])),
          "enforceLength rescales root-pinned (the K6 rule)")
    checkRaises(ValueError, lambda: math.enforceLength(line, 0.0),
                "enforceLength rejects a non-positive target")

    # -- hierarchy focus + breadcrumb ------------------------------------
    state.activeLevel = 1
    check(hier.enterLevel(state) != "" and state.activeLevel == 2
          and hier.enterLevel(state) != "" and state.activeLevel == 3,
          "enterLevel walks down L1 -> L3")
    check(hier.exitLevel(state) != "" and state.activeLevel == 2,
          "exitLevel walks back up")
    hier.focusLevel(state, 1)
    hier.exitLevel(state)
    check(state.activeLevel == 1, "exitLevel never climbs above L1")
    checkRaises(ValueError, lambda: hier.focusLevel(state, 0),
                "focusLevel rejects levels below L1")
    hier.setFocusPath(state, ("tube_A", "tube_A0"))
    check(state.activeLevel == 2
          and hier.breadcrumb(state) == "L1 tube_A > L2 tube_A0"
          and hier.breadcrumbSegments(state) ==
          [(1, "L1 tube_A"), (2, "L2 tube_A0")],
          "the focus path drives the clickable breadcrumb")
    hier.setFocusPath(state, ())
    check(hier.breadcrumb(state) == "L1", "an empty path crumbs L1")
    hier.setActiveCutFocus(state, 41, (7, 41), ("root", "child"), 3)
    check(state.activeCutEnabled and state.focusParentId == 41 and
          state.focusAncestorIds == (7, 41) and state.activeLevel == 3 and
          hier.breadcrumbSegments(state) ==
          [("tube:7", "L1 root"), ("tube:41", "L2 child"), (None, "L3")],
          "active-cut context binds breadcrumbs to stable tube ids, then "
          "a non-link frontier")
    check(hier.breadcrumbSegments(state, childCount=3)[-1] ==
          (None, "L3 (3 children)") and
          hier.breadcrumbSegments(state, childCount=1)[-1] ==
          (None, "L3 (1 child)"),
          "the frontier counts the entered parent's children (DK-08)")
    # DK-08: an entered L1 tube reads "L1 Tube 0 > L2", ending at L2.
    hier.setActiveCutFocus(state, 0, (0,), (), 2)
    check(hier.breadcrumb(state) == "L1 Tube 0 > L2" and
          hier.breadcrumb(state).endswith("L2"),
          "the breadcrumb at L2 names the tube and ends with L2 (%r)"
          % hier.breadcrumb(state))
    hier.setActiveCutFocus(state, -1, (0,), (), 1)
    check(hier.breadcrumbSegments(state) == [("tube:0", "L1 Tube 0")],
          "a collapsed focus has no frontier: the tube is where you are")
    hier.clearActiveCutFocus(state)
    check(state.focusParentId == -1 and state.focusAncestorIds == () and
          state.activeLevel == 1,
          "clearing only the UI context leaves level compatibility intact")
    check(hier.deriveChildLevel(1) == 2, "children sit at parent.level + 1")

    # -- locks: global default off, per-tube override ---------------------
    check(hier.effectiveLockParents(state) is False
          and hier.effectiveLockChildren(state) is False
          and hier.effectiveLockParents(state, 7) is False,
          "both locks default off, globally and per tube")
    hier.setLockParents(state, True)
    hier.setLockChildren(state, True, tubeId=7)
    check(hier.effectiveLockParents(state) is True
          and hier.effectiveLockParents(state, 7) is True
          and hier.effectiveLockChildren(state) is False
          and hier.effectiveLockChildren(state, 7) is True
          and hier.effectiveLockChildren(state, 8) is False,
          "per-tube locks override the global default per direction")
    hier.setLockParents(state, False)
    hier.setLockChildren(state, False, tubeId=7)
    check(hier.effectiveLockParents(state) is False
          and hier.effectiveLockChildren(state, 7) is False,
          "locks toggle back off")

    # -- level visibility -------------------------------------------------
    check(hier.levelVisible(2) is True
          and hier.levelDrawStyle(2, 2) == "full"
          and hier.levelDrawStyle(1, 2) == "x-ray",
          "the focus draws full, other levels draw as x-ray")
    check(hier.levelVisible(2, soloLevel=2) is True
          and hier.levelVisible(1, soloLevel=2) is False
          and hier.levelDrawStyle(1, 2, soloLevel=2) == "hidden",
          "solo shows only its level")
    check(hier.levelVisible(3, showMaxLevel=2) is False
          and hier.levelVisible(2, showMaxLevel=2) is True,
          "show<=n clips above n")
    check(hier.levelVisible(2, soloLevel=2, hidden={2}) is False,
          "hidden wins over solo")
    hier.setSoloLevel(state, 3)
    hier.setShowMaxLevel(state, 2)
    hier.hideLevel(state, 1)
    check(state.soloLevel == 3 and state.showMaxLevel == 2
          and state.hiddenLevels == {1},
          "solo/show/hide setters record on state")
    hier.setSoloLevel(state, hier.SOLO_OFF)
    hier.setShowMaxLevel(state, hier.SHOW_ALL_LEVELS)
    hier.showLevel(state, 1)
    check(state.soloLevel == -1 and state.showMaxLevel == -1
          and state.hiddenLevels == set(),
          "visibility toggles clear back to show-all")
    checkRaises(ValueError, lambda: hier.setSoloLevel(state, 0),
                "solo rejects levels below L1")

    # -- subdivide / merge / group text + annotations --------------------
    check("Shift+D" in hier.subdivideStatus(4, "kmeans", [0, 1])
          and hier.validateSubdivideCount(2) == 2
          and hier.validateSubdivideCount(8) == 8
          and hier.clampSubdivideCount(99) == 8
          and hier.clampSubdivideCount(0) == 2,
          "subdivide validates and clamps the 2..8 count")
    checkRaises(ValueError, lambda: hier.validateSubdivideCount(1),
                "subdivide rejects count 1")
    checkRaises(ValueError, lambda: hier.validateSubdivideCount(9),
                "subdivide rejects count 9")
    check(hier.validateSplitMode("edge") == "edge",
          "the drawn-edge split mode validates")
    checkRaises(ValueError, lambda: hier.validateSplitMode("voronoi"),
                "unknown split modes are rejected")
    check("Shift+M" in hier.mergeChildrenStatus("tube_A")
          and "same level" in hier.mergeSelectedStatus([3, 4]),
          "merge statuses name the K7 aggregate and the sibling fold")
    check("discarded" in hier.resubdivideConfirm("tube_A", 4, 2),
          "re-subdivide warns that child deltas are discarded")
    check("transient" in hier.groupStatus([1, 2], True)
          and "persistent" in hier.groupStatus([1, 2], False),
          "group names transient vs persistent parents")
    checkRaises(ValueError, lambda: hier.groupParentSpec([]),
                "group needs at least one child")
    ann = hier.childAnnotation(1, 4, 11, "kmeans", 2)
    check(ann == {"usdGen:pomade:childIndex": 1,
                  "usdGen:pomade:subdivide:count": 4,
                  "usdGen:pomade:subdivide:seed": 11,
                  "usdGen:pomade:subdivide:splitMode": "kmeans",
                  "usdGen:pomade:level": 2},
          "child annotations carry the hydrate keys (contract 9)")
    hud = hier.hierarchyStatus(2, 5, (1, 2, 3), -1, -1, False, True)
    check("L2" in hud and "lockC" in hud and "lockP" not in hud,
          "the hierarchy HUD names the focus and the live locks")
    hud = hier.hierarchyStatus(2, 5, (1, 2, 3), -1, -1, False, False,
                               fallbackReason="forced")
    check("CPU-only: forced" in hud,
          "the hierarchy HUD banners the P6 fallback reason")
    check(abs(hier.SMOOTHNESS_SPIKE_THRESHOLD - (1.0 - stdmath.cos(
        stdmath.radians(15.0)))) < 1e-6,
        "the spike threshold mirrors 1 - cos(15 deg)")
    check(hier.smoothnessWarning([0.0, 0.01, 0.0]) == "",
          "quiet scores raise no smoothness warning")
    warn = hier.smoothnessWarning([0.0, 0.5, 0.02])
    check("CV 1" in warn and "Relax" in warn and "Smooth" in warn,
          "a spiking CV names itself and the Smooth/Relax fix (%r)" % warn)
    check("°" in warn and "15°" in warn and "tube 0" in warn and
          "tube" in warn,
          "the kink reads in degrees on a named tube (%r)" % warn)
    check("tube 4" in hier.smoothnessWarning([0.5], tubeId=4) and
          "1 more CV" in hier.smoothnessWarning([0.5, 0.0, 0.6]),
          "the tube id is the caller's and further spikes are counted")
    check(hier.intersectionWarning([]) == "",
          "clear roots raise no intersection warning")
    check(hier.intersectionWarning([4, 2]) ==
          "Roots overlap on 2 tube(s): T2, T4.",
          "flagged tubes list ascending in the warning")

    # -- quoted P4 C ABI (missing until the C++ lands) -------------------
    check(len(hier.REQUIRED_C_API) >= 10
          and all(hier.cApiQuote(n).startswith("int Pomade_")
                  for n, _, _ in hier.REQUIRED_C_API),
          "every hierarchy entry quotes its C signature")
    check(set(hier.missingEntries(_NoDll())) ==
          set(n for n, _, _ in hier.REQUIRED_C_API),
          "a dll without P4 symbols misses every entry")
    checkRaises(NotImplementedError,
                lambda: hier.requireEntry(_NoDll(), "Pomade_SubdivideTube"),
                "requireEntry names the missing symbol")
    checkRaises(NotImplementedError,
                lambda: hier.subdivide(_NoDll(), None, 0, 4),
                "subdivide fails closed without the C++ (K14)")
    checkRaises(NotImplementedError,
                lambda: sculpt.stroke(_NoDll(), None, 0, "grab", [], []),
                "sculpt strokes fail closed without the C++ (K6/K7)")
    checkRaises(NotImplementedError,
                lambda: hier.readSmoothnessScores(_NoDll(), None),
                "smoothness reads fail closed without the C++ (K13)")
    checkRaises(NotImplementedError,
                lambda: hier.checkRootIntersections(_NoDll(), None),
                "intersection checks fail closed without the C++ (K12)")
    check(len(sculpt.REQUIRED_C_API) == 1
          and sculpt.cApiQuote("Pomade_SculptStroke")
          .startswith("int Pomade_SculptStroke"),
          "the sculpt stroke quotes its C signature")

    # -- sculpt brushes ---------------------------------------------------
    check(sculpt.validateBrush("twist") == "twist"
          and sculpt.brushLabel("grab") == "Grab",
          "brush ids validate with display labels")
    checkRaises(ValueError, lambda: sculpt.validateBrush("smudge"),
                "unknown brushes are rejected")
    dists = np.array([0.0, 6.0, 12.0, 24.0, 48.0])
    ts = np.array([0.5, 0.5, 0.5, 0.5, 0.5])
    check(bool(np.allclose(
        sculpt.brushFalloff(dists, 24.0, t=ts, center=0.5, tRadius=0.25),
        math.brushFalloff(dists, 24.0, t=ts, center=0.5, tRadius=0.25))),
        "the sculpt falloff equals the pomadeMath kernel")
    column = np.array([[0.0, 0.0, 0.0], [0.0, 1.0, 0.1], [0.0, 2.0, 0.0],
                       [0.0, 3.0, -0.1], [0.0, 4.0, 0.0]])
    before = math.polylineLength(column)
    w = np.array([0.0, 0.5, 1.0, 0.5, 0.0])
    grabbed = sculpt.grabPreview(column, w, [1.0, 0.0, 0.0])
    check(float(grabbed[2, 0]) > 0.5
          and abs(math.polylineLength(grabbed) - before) / before < 1e-9,
          "grab follows the delta and preserves length by default")
    raw = sculpt.grabPreview(column, w, [1.0, 0.0, 0.0],
                             preserveLength=False)
    check(abs(math.polylineLength(raw) - before) > 1e-6,
          "preserveLength=False previews the raw offset")
    smoothed = sculpt.smoothPreview(column, np.ones(5), 1.0)
    rawSmooth = sculpt.smoothPreview(column, np.ones(5), 1.0,
                                     preserveLength=False)
    check(bool(np.allclose(rawSmooth[0], column[0]))
          and bool(np.allclose(rawSmooth[-1], column[-1]))
          and bool(np.allclose(smoothed[0], column[0]))
          and abs(math.polylineLength(smoothed) - before) / before < 1e-9,
          "smooth pins the ends raw; by default the root pins and the "
          "tip re-seats to preserve length")
    check(bool(np.allclose(
        sculpt.combPreview(column, w, [0.0, 0.0, 0.0]), column)),
        "a zero comb direction is a no-op")
    combed = sculpt.combPreview(column, w, [0.0, 0.0, 2.0])
    check(abs(math.polylineLength(combed) - before) / before < 1e-9,
          "comb preserves length by default")
    grown = sculpt.lengthenPreview(column, before * 1.5)
    check(abs(math.polylineLength(grown) - before * 1.5) < 1e-9
          and bool(np.allclose(grown[0], column[0])),
          "lengthen sets length explicitly, root pinned")
    checkRaises(ValueError, lambda: sculpt.lengthenPreview(column, 0.0),
                "lengthen rejects a non-positive target")
    twisted = sculpt.twistPreview(column, np.ones(5), 0.5)
    check(bool(np.allclose(twisted[0], column[0]))
          and abs(math.polylineLength(twisted) - before) / before < 1e-9,
          "twist pins the root and preserves length")
    check(bool(np.allclose(sculpt.mirrorXPoints(
        sculpt.mirrorXPoints(column)), column)),
        "mirror-X is its own inverse")
    check(bool(np.allclose(sculpt.mirrorXDelta([1.0, 2.0, 3.0]),
                           [-1.0, 2.0, 3.0])),
        "mirror-X negates only x")
    check("length-preserving" in sculpt.sculptStatus(
        "grab", 24.0, 0.0, True, False)
        and "mirror-X" in sculpt.sculptStatus(
            "comb", 24.0, 0.0, True, True),
        "the sculpt HUD names the flags")
    check(sculpt.propagationPlan(False, False) ==
          {"topDown": True, "bottomUp": True}
          and sculpt.propagationPlan(True, True) ==
          {"topDown": False, "bottomUp": False},
          "the lock switches gate the K6/K7 passes")

    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
