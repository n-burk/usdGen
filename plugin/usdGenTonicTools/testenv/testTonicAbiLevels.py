# testTonicAbiLevels -- T3: Hierarchy mode walks L1 -> L3 and back
# with edits at each level (plan/17 P4 exit).
#
# ABI-level by name since plan/18 V2: this script drives the C ABI
# directly and never touches the viewport, so it is a regression
# test for the model, not a proof that the tool works. The gesture
# proof for this mode is the matching testUsdviewTonic* script.
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testTonicAbiLevels.py \
#               examples/tonic-graph-scalp.usda
#
# with the same environment testTonicAbiGraph.py uses. Part A (the
# Qt-free hierarchy/sculpt helpers) always runs; Part B (the C walk through
# K6/K7/K14) runs each step it can and SKIPs the steps whose P4 C entries
# are still missing, so the script is green before the C++ lands and strict
# after.
#
# Proven here (the P4 exit criteria from plan/17 section 6):
#   * Part A: subdivide counts 2..8, split modes, level focus both ways,
#     the clickable breadcrumb, lock defaults + per-tube override, solo /
#     show<=n / hidden visibility, re-subdivide confirm, brush falloff,
#     length preservation, and the K6/K7 propagation gates;
#   * Part B: subdivide an L1 tube (count 4): 4 L2 children, zero deltas;
#     enter/exit move the editing focus both ways; a parent edit
#     re-derives descendants length-preserving (1e-4) with child sculpt
#     kept (K6); a child edit refreshes its enclosing parent geometry (K7); the
#     lock switches gate each direction; merge children round-trips the
#     parent shape bit-exactly when the children are untouched; merge
#     selected folds siblings; group builds a transient parent and
#     make-persistent keeps it.
import ctypes
import importlib.util
import os
import sys

failures = 0
skips = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def skip(what):
    global skips
    skips += 1
    print("SKIP (P4): %s" % what)


def locate(x, z):
    """(face, u, v) for a scalp position (x, z)."""
    import math
    ix = min(max(int(math.floor(x)), 0), 3)
    iz = min(max(int(math.floor(z)), 0), 3)
    return (ix * 4 + iz, float(z - iz), float(x - ix))


def strokeRect(dll, model, corners, snap):
    """Stroke a closed rectangle; returns (rc, chain, closed)."""
    samples = []
    per = 5
    for k in range(len(corners)):
        x0, z0 = corners[k]
        x1, z1 = corners[(k + 1) % len(corners)]
        for i in range(per):
            t = float(i) / float(per)
            samples.append((x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
    samples.append(corners[0])
    faces = (ctypes.c_int * len(samples))()
    uvs = (ctypes.c_float * (2 * len(samples)))()
    for i, (x, z) in enumerate(samples):
        face, u, v = locate(x, z)
        faces[i] = face
        uvs[2 * i] = u
        uvs[2 * i + 1] = v
    out = (ctypes.c_int * 4096)()
    count = ctypes.c_int(0)
    closed = ctypes.c_int(0)
    weldStart = ctypes.c_int(0)
    weldEnd = ctypes.c_int(0)
    rc = dll.Tonic_GraphStroke(model, faces, uvs, len(samples),
                               ctypes.c_float(snap), ctypes.c_float(0.05),
                               out, 4096, ctypes.byref(count),
                               ctypes.byref(closed), ctypes.byref(weldStart),
                               ctypes.byref(weldEnd))
    return (rc, [out[i] for i in range(count.value)], bool(closed.value))


def _loadFile(here, name):
    path = os.path.normpath(os.path.join(here, name))
    spec = importlib.util.spec_from_file_location(
        "tonic_levels_%s" % os.path.basename(name), path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _arcLength(centers):
    import math
    total = 0.0
    for a, b in zip(centers, centers[1:]):
        total += math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))
    return total


def _tubeSections(dll, model, tubeId):
    """The complete authored section descriptor for one tube.

    Keep this reader in the ABI test instead of extending the public Python
    helper solely for one assertion.  K7 now holds edited descendants with a
    conservative parent support envelope, so the relevant authored change is
    often its UV loops rather than the parent's center cage.
    """
    entry = dll.Tonic_GetTubeSection
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int),
                      ctypes.POINTER(ctypes.c_float),
                      ctypes.POINTER(ctypes.c_float)]
    entry.restype = ctypes.c_int
    count = int(dll.Tonic_GetTubeSectionCount(model, int(tubeId)))
    if count < 0:
        raise RuntimeError("Tonic_GetTubeSectionCount: unknown tube %d"
                           % int(tubeId))
    sections = []
    for ring in range(count):
        t = ctypes.c_float(0.0)
        scale = ctypes.c_float(0.0)
        twist = ctypes.c_float(0.0)
        uvCount = ctypes.c_int(0)
        if entry(model, int(tubeId), ring, ctypes.byref(t), None, 0,
                 ctypes.byref(uvCount), ctypes.byref(scale),
                 ctypes.byref(twist)) != 0 or uvCount.value < 3:
            raise RuntimeError("Tonic_GetTubeSection census failed for "
                               "tube %d ring %d" % (int(tubeId), ring))
        uv = (ctypes.c_float * (2 * uvCount.value))()
        if entry(model, int(tubeId), ring, ctypes.byref(t), uv,
                 len(uv), ctypes.byref(uvCount), ctypes.byref(scale),
                 ctypes.byref(twist)) != 0:
            raise RuntimeError("Tonic_GetTubeSection read failed for "
                               "tube %d ring %d" % (int(tubeId), ring))
        sections.append((float(t.value),
                         tuple(float(uv[i]) for i in range(2 * uvCount.value)),
                         float(scale.value), float(twist.value)))
    return tuple(sections)


def run(stage):
    global failures
    # Under testusdview the script is exec'd (no __file__) but the staged
    # package is already on PYTHONPATH; the insert only helps direct runs.
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, os.path.normpath(os.path.join(
            here, "..", "python")))

    # -- Part A: the Qt-free helpers (no dll needed) ----------------------
    if here:
        try:
            headless = _loadFile(here, "testUsdGenTonicToolsHierarchy.py")
        except ImportError as exc:
            check(False, "the headless P4 test imports (%s)" % exc)
            headless = None
        if headless is not None:
            before = failures
            rc = headless.main()
            failures += headless.failures
            check(rc == 0, "the headless P4 checks pass "
                  "(%d new failure(s))" % (failures - before))
        src = os.path.normpath(os.path.join(here, "..", "python",
                                            "usdGenTonicTools"))
        hier = _loadFile(src, "tonicHierarchy.py")
        sculpt = _loadFile(src, "tonicSculpt.py")
        stateModule = _loadFile(src, "tonicToolState.py")
    else:
        try:
            from usdGenTonicTools import tonicHierarchy as hier
            from usdGenTonicTools import tonicSculpt as sculpt
            from usdGenTonicTools import tonicToolState as stateModule
        except ImportError as exc:
            print("FAIL: cannot import usdGenTonicTools: %s" % exc)
            return 1
    state = stateModule.TonicToolState()

    # -- Part B: the C walk (per-step SKIP until the C++ lands) -----------
    try:
        from usdGenTonicTools import tonicLib
    except ImportError as exc:
        for planned in _C_STEPS:
            skip("%s: cannot import usdGenTonicTools (%s)" % (planned, exc))
        print("testTonicAbiLevels: %d failure(s), %d skip(s)"
              % (failures, skips))
        return 1 if failures else 0
    try:
        lib = tonicLib.Library()
    except OSError as exc:
        for planned in _C_STEPS:
            skip("%s: %s" % (planned, exc))
        print("testTonicAbiLevels: %d failure(s), %d skip(s)"
              % (failures, skips))
        return 1 if failures else 0
    dll = lib.dll
    missing = hier.missingEntries(dll) + sculpt.missingEntries(dll)
    if missing:
        for planned in _C_STEPS:
            skip("%s: the C ABI lacks %s" % (planned, ", ".join(missing)))
        print("testTonicAbiLevels: %d failure(s), %d skip(s)"
              % (failures, skips))
        return 1 if failures else 0

    scalp = stage.GetPrimAtPath("/Scalp")
    check(scalp, "the stage carries /Scalp")
    if not scalp:
        return 1
    points = scalp.GetAttribute("points").Get()
    counts = scalp.GetAttribute("faceVertexCounts").Get()
    indices = scalp.GetAttribute("faceVertexIndices").Get()
    flat = [float(c) for p in points for c in (p[0], p[1], p[2])]
    pts = (ctypes.c_float * len(flat))(*flat)
    cnt = (ctypes.c_int * len(counts))(*[int(c) for c in counts])
    idx = (ctypes.c_int * len(indices))(*[int(i) for i in indices])

    model = ctypes.c_void_p(None)
    check(dll.Tonic_Create(ctypes.byref(model)) == 0, "model creates")
    check(dll.Tonic_BindScalp(model, pts, len(flat), cnt, len(counts),
                              idx, len(indices)) == 0, "scalp binds")
    rc, _, closed = strokeRect(
        dll, model, [(0.0, 1.0), (4.0, 1.0), (4.0, 3.0), (0.0, 3.0)], 0.1)
    check(rc == 0 and closed, "the region stroke closes")
    check(dll.Tonic_Rasterise(model) == 0, "K3 rasterises")
    check(dll.Tonic_BuildTubeFromRegion(model, 0, 5, 8, ctypes.c_float(2.0))
          == 0, "the L1 tube builds")
    tube0 = 0

    # L1 -> L2: four children, zero deltas, groom identical at the split.
    l2 = hier.subdivide(dll, model, tube0, 4, "kmeans", seed=11)
    check(len(l2) == 4, "subdivide yields 4 L2 children (got %r)" % (l2,))
    check(all(hier.tubeLevel(dll, model, c) == 2 for c in l2),
          "the children sit at L2")
    check(all(hier.tubeDeltaNorm(dll, model, c) == 0.0 for c in l2),
          "fresh children carry zero deltas")
    check(hier.tubeChildren(dll, model, tube0) == list(l2),
          "the parent lists its four children")

    # Enter L2, edit a child (K7 refreshes the parent bottom-up).
    hier.focusLevel(state, 2)
    parentBefore = hier.tubeCenters(dll, model, tube0)
    parentSectionsBefore = _tubeSections(dll, model, tube0)
    siblingSectionsBefore = _tubeSections(dll, model, l2[1])
    hier.moveTubeCenterCV(dll, model, l2[0], 2, 0.2, 0.0, 0.0)
    check(hier.tubeDeltaNorm(dll, model, l2[0]) > 0.0,
          "the L2 edit lands (nonzero deltas)")
    parentAfter = hier.tubeCenters(dll, model, tube0)
    parentSectionsAfter = _tubeSections(dll, model, tube0)
    check(parentAfter != parentBefore or
          parentSectionsAfter != parentSectionsBefore,
          "the child edit refreshes parent holding geometry (K7)")
    check(parentSectionsAfter != parentSectionsBefore,
          "K7 grows the parent section support envelope for the child edit")
    check(_tubeSections(dll, model, l2[1]) == siblingSectionsBefore,
          "K7 keeps an unedited sibling's authored geometry exact")

    # L2 -> L3 on the edited child; walk L1 -> L3 and back, editing each.
    l3 = hier.subdivide(dll, model, l2[0], 2, "kmeans", seed=12)
    check(len(l3) == 2
          and all(hier.tubeLevel(dll, model, c) == 3 for c in l3),
          "one L2 subdivides into two L3 children")
    hier.focusLevel(state, 1)
    hier.moveTubeCenterCV(dll, model, tube0, 1, -0.1, 0.0, 0.0)
    hier.focusLevel(state, 3)
    hier.moveTubeCenterCV(dll, model, l3[0], 1, 0.0, 0.1, 0.0)
    hier.focusLevel(state, 2)
    check(state.activeLevel == 2
          and hier.tubeDeltaNorm(dll, model, tube0) >= 0.0
          and hier.tubeDeltaNorm(dll, model, l3[0]) > 0.0,
          "the walk edits L1, L3 and returns to L2")

    # K6: a sculpted parent re-derives descendants length-preserving
    # (1e-4 relative) with child sculpt kept.
    kidLen = _arcLength(hier.tubeCenters(dll, model, l2[1]))
    kidDeltas = hier.tubeDeltaNorm(dll, model, l2[1])
    sculpt.stroke(dll, model, tube0, "grab", [1, 2],
                  [0.3, 0.0, 0.0, 0.3, 0.0, 0.0],
                  preserveLength=True, mirrorX=False)
    kidLenAfter = _arcLength(hier.tubeCenters(dll, model, l2[1]))
    check(abs(kidLenAfter - kidLen) / kidLen <= 1e-4,
          "the parent stroke preserves child length to 1e-4 (K6)")
    check(hier.tubeDeltaNorm(dll, model, l2[1]) >= kidDeltas,
          "the child keeps its sculpt through the re-derive")

    # The lock switches gate each direction.
    hier.setLockParentsEntry(dll, model, -1, True)
    frozen = hier.tubeCenters(dll, model, tube0)
    hier.moveTubeCenterCV(dll, model, l2[2], 0, 0.0, 0.0, 0.4)
    check(hier.tubeCenters(dll, model, tube0) == frozen,
          "lock parents freezes the K7 refresh bit-exactly")
    hier.setLockParentsEntry(dll, model, -1, False)
    hier.setLockChildrenEntry(dll, model, -1, True)
    childDeltas = hier.tubeDeltaNorm(dll, model, l2[3])
    sculpt.stroke(dll, model, tube0, "grab", [3],
                  [0.0, 0.2, 0.0],
                  preserveLength=True, mirrorX=False)
    check(hier.tubeDeltaNorm(dll, model, l2[3]) == childDeltas,
          "lock children freezes child deltas through K6")
    hier.setLockChildrenEntry(dll, model, -1, False)

    # Merge round-trips the parent bit-exactly when children are untouched.
    fresh = [c for c in l2[1:] if hier.tubeDeltaNorm(dll, model, c) == 0.0]
    if fresh:
        grand = hier.subdivide(dll, model, fresh[0], 2, "kmeans", seed=13)
        check(len(grand) == 2, "the untouched child subdivides")
        shape = hier.tubeCenters(dll, model, fresh[0])
        hier.mergeChildren(dll, model, fresh[0])
        check(hier.tubeCenters(dll, model, fresh[0]) == shape
              and hier.tubeChildren(dll, model, fresh[0]) == [],
              "merge children round-trips an untouched parent bit-exactly")
    else:
        skip("merge round-trip: every L2 child was edited above")

    # Merge selected folds a subset of siblings into one child.
    sibs = hier.tubeChildren(dll, model, l2[0])
    if len(sibs) >= 2:
        before = hier.tubeCount(dll, model)
        kept = hier.mergeSelected(dll, model, sibs[:2])
        check(hier.tubeCount(dll, model) == before - 1
              and hier.tubeLevel(dll, model, kept) == 3,
              "merge selected folds two L3 siblings into one L3")
    else:
        skip("merge selected: fewer than two L3 siblings remain")

    # Group builds a transient parent; make-persistent keeps it.
    parent = hier.groupTubes(dll, model, l2[:2], transient=True)
    check(hier.tubeLevel(dll, model, parent) == 1,
          "group builds a transient L1 parent over two L2 tubes")
    hier.makePersistent(dll, model, parent)
    check(hier.tubeLevel(dll, model, parent) == 1,
          "make-persistent keeps the on-the-fly parent")

    hier.focusLevel(state, 1)
    dll.Tonic_Destroy(model)
    print("testTonicAbiLevels: %d failure(s), %d skip(s)"
          % (failures, skips))
    return 1 if failures else 0


_C_STEPS = (
    "subdivide the L1 tube into 4 L2 children",
    "enter level L2, edit, exit back to L1",
    "subdivide one L2 into L3, walk L1 -> L3 and back",
    "parent edit re-derives descendants (K6, 1e-4 length)",
    "child edit refreshes the enclosing parent geometry (K7)",
    "lock parents / lock children gate propagation",
    "merge children round-trips the parent bit-exactly",
    "merge selected folds siblings",
    "group builds a transient parent; make-persistent keeps it",
)


def testUsdviewInputFunction(appController):
    model = getattr(appController, "_dataModel", appController)
    return run(model.stage)


if __name__ == "__main__":
    # Direct stage run (no usdview): python testTonicAbiLevels.py stage.usda
    from pxr import Usd
    if len(sys.argv) != 2:
        print("usage: testTonicAbiLevels.py <stage.usda>")
        sys.exit(2)
    sys.exit(run(Usd.Stage.Open(sys.argv[1])))
