# usdGenTonicTools.tonicHierarchy -- Qt-free Hierarchy-mode helpers (plan/17 P4).
#
# Pure-python focus/lock/visibility logic plus the status/HUD text builders
# behind Hierarchy mode (plan/17 sections 2.3, 2.4 and 5.4). Everything here
# imports headlessly (no pxr, no Qt); the C++ model owns the tubes, and the
# gizmo UI owns the event filter. Covered by
# testUsdGenTonicToolsHierarchy.py from day one.
#
# The thin C wrappers at the bottom quote the hierarchy C ABI (K6/K7/K14),
# which shipped in P4/V0b and is unconditionally bound on `dll` today;
# requireEntry's NotImplementedError only fires against a DLL built before
# that landed, so it stays as a defensive quote of the signature rather
# than the live state of the ABI.
from __future__ import annotations

import ctypes

# Subdivide spinner range (plan/17 section 5.4): 2..8, default 4.
SUBDIVIDE_MIN = 2
SUBDIVIDE_MAX = 8
DEFAULT_SUBDIVIDE_COUNT = 4

# Root-region partition rules (plan/17 section 2.3 step 1).
SPLIT_MODES = ("kmeans", "edge")
DEFAULT_SPLIT_MODE = "kmeans"

# Levels start at 1 and have no depth limit (plan/17 section 5.4).
LEVEL_MIN = 1

# Visibility sentinels (match TonicToolState defaults: off, show all).
SOLO_OFF = -1
SHOW_ALL_LEVELS = -1


def validateSubdivideCount(count):
    """The subdivide count as an int; raises ValueError outside 2..8."""
    try:
        value = int(count)
    except (TypeError, ValueError):
        raise ValueError("validateSubdivideCount: want an int in 2..8, got "
                         "%r" % (count,))
    if not SUBDIVIDE_MIN <= value <= SUBDIVIDE_MAX:
        raise ValueError("validateSubdivideCount: want 2..8, got %r"
                         % (count,))
    return value


def clampSubdivideCount(count):
    """Clamp a spinner value into 2..8 (never raises)."""
    try:
        value = int(count)
    except (TypeError, ValueError):
        return DEFAULT_SUBDIVIDE_COUNT
    return min(max(value, SUBDIVIDE_MIN), SUBDIVIDE_MAX)


def validateSplitMode(mode):
    """The split mode token; raises ValueError when unknown."""
    if mode not in SPLIT_MODES:
        raise ValueError("validateSplitMode: want one of %r, got %r"
                         % (SPLIT_MODES, mode))
    return mode


def validateLevel(level):
    """A hierarchy level as an int; raises ValueError below L1."""
    try:
        value = int(level)
    except (TypeError, ValueError):
        raise ValueError("validateLevel: want an int >= 1, got %r"
                         % (level,))
    if value < LEVEL_MIN:
        raise ValueError("validateLevel: want >= 1, got %r" % (level,))
    return value


def deriveChildLevel(parentLevel):
    """The level of a subdivided tube's children (parent + 1)."""
    return validateLevel(parentLevel) + 1


def enterLevel(state):
    """Move the editing focus one level down; returns the status line."""
    state.activeLevel = validateLevel(state.activeLevel) + 1
    return "Tonic Hierarchy: entered L%d (parent draws as x-ray)." \
        % state.activeLevel


def exitLevel(state):
    """Move the editing focus one level up (never above L1)."""
    state.activeLevel = max(validateLevel(state.activeLevel) - 1, LEVEL_MIN)
    return "Tonic Hierarchy: exited to L%d." % state.activeLevel


def focusLevel(state, level):
    """Jump the editing focus to `level` (breadcrumb click)."""
    state.activeLevel = validateLevel(level)
    return "Tonic Hierarchy: focused L%d." % state.activeLevel


def setFocusPath(state, names):
    """Record the L1..focus tube-name path; the focus is its depth.

    `names` is innermost-last (("tube_A", "tube_A0") focuses L2). An empty
    path focuses L1 with no names. Returns the status line.
    """
    path = tuple(str(n) for n in names)
    state.focusNames = path
    state.activeLevel = max(len(path), LEVEL_MIN)
    return "Tonic Hierarchy: %s." % breadcrumb(state)


def breadcrumbSegments(state):
    """Clickable breadcrumb entries: [(level, label)] root-first.

    Labels carry the focus-path tube names when known, else the bare level
    ("L1 tube_A" vs "L2"). The UI binds one click target per entry.
    """
    names = tuple(getattr(state, "focusNames", ()))
    depth = max(validateLevel(state.activeLevel), len(names), LEVEL_MIN)
    segments = []
    for level in range(1, depth + 1):
        name = names[level - 1] if level - 1 < len(names) else ""
        label = "L%d %s" % (level, name) if name else "L%d" % level
        segments.append((level, label))
    return segments


def breadcrumb(state):
    """The one-line level breadcrumb (breadcrumbSegments joined)."""
    return " > ".join(label for _, label in breadcrumbSegments(state))


def _tubeLockRecord(state, tubeId):
    """The per-tube lock record, created on first use (None = inherit)."""
    locks = state.tubeLocks
    key = int(tubeId)
    record = locks.get(key)
    if record is None:
        record = {"lockParents": None, "lockChildren": None}
        locks[key] = record
    return record


def setLockParents(state, on, tubeId=None):
    """Toggle lock-parents globally, or per tube when `tubeId` is given."""
    on = bool(on)
    if tubeId is None:
        state.lockParents = on
        scope = "global"
    else:
        _tubeLockRecord(state, tubeId)["lockParents"] = on
        scope = "tube %d" % int(tubeId)
    return "Tonic Hierarchy: lock parents %s (%s)." \
        % ("on" if on else "off", scope)


def setLockChildren(state, on, tubeId=None):
    """Toggle lock-children globally, or per tube when `tubeId` is given."""
    on = bool(on)
    if tubeId is None:
        state.lockChildren = on
        scope = "global"
    else:
        _tubeLockRecord(state, tubeId)["lockChildren"] = on
        scope = "tube %d" % int(tubeId)
    return "Tonic Hierarchy: lock children %s (%s)." \
        % ("on" if on else "off", scope)


def effectiveLockParents(state, tubeId=None):
    """Whether child edits refresh ancestors for `tubeId` (default off)."""
    if tubeId is not None:
        record = state.tubeLocks.get(int(tubeId))
        if record is not None and record.get("lockParents") is not None:
            return bool(record["lockParents"])
    return bool(state.lockParents)


def effectiveLockChildren(state, tubeId=None):
    """Whether parent edits move descendants rigidly (default off)."""
    if tubeId is not None:
        record = state.tubeLocks.get(int(tubeId))
        if record is not None and record.get("lockChildren") is not None:
            return bool(record["lockChildren"])
    return bool(state.lockChildren)


def levelVisible(level, soloLevel=SOLO_OFF, showMaxLevel=SHOW_ALL_LEVELS,
                 hidden=frozenset()):
    """Whether `level` draws under the Solo / Show<=n / hidden toggles.

    Hidden wins over everything; Solo shows only its level; Show<=n shows
    levels 1..n. Display only (plan/17 section 2.4).
    """
    level = validateLevel(level)
    if level in set(hidden):
        return False
    if int(soloLevel) >= LEVEL_MIN:
        return level == int(soloLevel)
    if int(showMaxLevel) >= LEVEL_MIN:
        return level <= int(showMaxLevel)
    return True


def levelDrawStyle(level, focusLevel, soloLevel=SOLO_OFF,
                   showMaxLevel=SHOW_ALL_LEVELS, hidden=frozenset()):
    """How `level` draws: "full", "x-ray" (non-focused), or "hidden"."""
    if not levelVisible(level, soloLevel, showMaxLevel, hidden):
        return "hidden"
    if validateLevel(level) == validateLevel(focusLevel):
        return "full"
    return "x-ray"


def setSoloLevel(state, level):
    """Solo one level (SOLO_OFF clears); returns the status line."""
    level = int(level)
    if level != SOLO_OFF:
        validateLevel(level)
    state.soloLevel = level
    if level == SOLO_OFF:
        return "Tonic Hierarchy: solo off."
    return "Tonic Hierarchy: solo L%d." % level


def setShowMaxLevel(state, level):
    """Show levels 1..n (SHOW_ALL_LEVELS clears); returns the status line."""
    level = int(level)
    if level != SHOW_ALL_LEVELS:
        validateLevel(level)
    state.showMaxLevel = level
    if level == SHOW_ALL_LEVELS:
        return "Tonic Hierarchy: showing all levels."
    return "Tonic Hierarchy: showing levels <= L%d." % level


def hideLevel(state, level):
    """Hide one level; returns the status line."""
    state.hiddenLevels.add(validateLevel(level))
    return "Tonic Hierarchy: L%d hidden." % validateLevel(level)


def showLevel(state, level):
    """Un-hide one level; returns the status line."""
    state.hiddenLevels.discard(validateLevel(level))
    return "Tonic Hierarchy: L%d shown." % validateLevel(level)


def subdivideStatus(count, splitMode, selected):
    """The one-line Subdivide (Shift+D) status."""
    count = validateSubdivideCount(count)
    splitMode = validateSplitMode(splitMode)
    return ("Subdivide (Shift+D): %d tube(s) x count %d (%s); the groom "
            "looks identical, the parent persists."
            % (len(list(selected)), count, splitMode))


def mergeChildrenStatus(tubeLabel):
    """The one-line Merge-children (Shift+M) status."""
    return ("Merge children (Shift+M): %s absorbs its children's aggregate "
            "shape (K7); undoable." % tubeLabel)


def mergeSelectedStatus(count):
    """The one-line Merge-selected status (siblings fold into one child)."""
    return ("Merge selected: %d sibling(s) fold into one child at the same "
            "level; undoable." % len(list(count)))


def resubdivideConfirm(tubeLabel, oldCount, newCount):
    """The Re-subdivide confirm prompt (child deltas are discarded)."""
    return ("Re-subdivide %s from count %d to count %d? The old children's "
            "sculpt deltas will be discarded."
            % (tubeLabel, validateSubdivideCount(oldCount),
               validateSubdivideCount(newCount)))


def groupStatus(childIds, transient):
    """The one-line Group / on-the-fly-parent status."""
    ids = list(childIds)
    kind = "transient" if transient else "persistent"
    return ("Group: K7 builds a %s on-the-fly parent over %d tube(s)."
            % (kind, len(ids)))


def hierarchyStatus(activeLevel, tubeCount, levelsPresent, soloLevel,
                    showMaxLevel, lockParents, lockChildren,
                    fallbackReason=""):
    """The one-line Hierarchy-mode HUD status (plus the P6 CPU-only
    banner when `fallbackReason` is non-empty)."""
    levels = sorted(validateLevel(v) for v in levelsPresent)
    span = ("L%d-L%d" % (levels[0], levels[-1]) if levels else "no levels")
    locks = []
    if lockParents:
        locks.append("lockP")
    if lockChildren:
        locks.append("lockC")
    line = ("Hierarchy: focus L%d, %d tube(s), %s%s."
            % (validateLevel(activeLevel), int(tubeCount), span,
               (", " + "+".join(locks)) if locks else ""))
    if int(soloLevel) >= LEVEL_MIN:
        line += " Solo L%d." % int(soloLevel)
    elif int(showMaxLevel) >= LEVEL_MIN:
        line += " Showing <= L%d." % int(showMaxLevel)
    if fallbackReason:
        line += " (CPU-only: %s)" % fallbackReason
    return line


def childAnnotation(childIndex, count, seed, splitMode, level):
    """The commit annotations making a child reproducible on hydrate.

    Keys match the example groom: childIndex, subdivide:count/seed/
    splitMode, and the derived level (contract 9).
    """
    return {
        "usdGen:tonic:childIndex": int(childIndex),
        "usdGen:tonic:subdivide:count": validateSubdivideCount(count),
        "usdGen:tonic:subdivide:seed": int(seed),
        "usdGen:tonic:subdivide:splitMode": validateSplitMode(splitMode),
        "usdGen:tonic:level": validateLevel(level),
    }


# Mirrors kTonicSmoothnessSpike (tonicCheck.h): 1 - cos(15 deg). A K13
# score at or above this is a spike the HUD warns about.
SMOOTHNESS_SPIKE_THRESHOLD = 0.0340742


def smoothnessWarning(scores, threshold=SMOOTHNESS_SPIKE_THRESHOLD):
    """The HUD warning for spiking center CVs ("" when all quiet)."""
    spikes = [(i, float(s)) for i, s in enumerate(scores)
              if float(s) >= float(threshold)]
    if not spikes:
        return ""
    worst = max(spikes, key=lambda kv: kv[1])
    return ("Smoothness: %d CV(s) spike past %.4g (worst %.4g at CV %d); "
            "relax to settle."
            % (len(spikes), float(threshold), worst[1], worst[0]))


def intersectionWarning(tubeIds):
    """The HUD warning for root-overlapping tubes ("" when clear)."""
    ids = sorted(set(int(v) for v in tubeIds))
    if not ids:
        return ""
    return ("Roots overlap on %d tube(s): %s."
            % (len(ids), ", ".join("T%d" % v for v in ids)))


def groupParentSpec(childIds, transient=True):
    """The UI/commit spec for an on-the-fly parent (pure data, no USD)."""
    ids = [int(v) for v in childIds]
    if not ids:
        raise ValueError("groupParentSpec: want at least one child tube")
    return {"childIds": ids, "transient": bool(transient)}


# The hierarchy C ABI the wrappers below need (name, quoted C signature,
# kernel). tonicLib.Library binds these unconditionally today, next to
# _bindTube; requireEntry's NotImplementedError is a defensive quote for a
# DLL built before that, not the live state of the ABI.
REQUIRED_C_API = (
    ("Tonic_SubdivideTube",
     "int Tonic_SubdivideTube(void *model, int tubeId, int count, "
     "const char *splitMode, int seed, int *outChildIds, int outCap, "
     "int *outCount)",
     "K14"),
    ("Tonic_MergeChildren",
     "int Tonic_MergeChildren(void *model, int tubeId)",
     "K14 inverse + K7"),
    ("Tonic_MergeSelected",
     "int Tonic_MergeSelected(void *model, const int *tubeIds, "
     "int tubeCount, int *outKept)",
     "K14 inverse (partial)"),
    ("Tonic_GetTubeCount",
     "int Tonic_GetTubeCount(void *model)",
     "hierarchy"),
    ("Tonic_GetTubeLevel",
     "int Tonic_GetTubeLevel(void *model, int tubeId)",
     "hierarchy"),
    ("Tonic_GetTubeChildren",
     "int Tonic_GetTubeChildren(void *model, int tubeId, int *out, "
     "int outCap, int *outCount)",
     "hierarchy"),
    ("Tonic_GetTubeCenterCount",
     "int Tonic_GetTubeCenterCount(void *model, int tubeId)",
     "hierarchy"),
    ("Tonic_GetTubeCenterCV",
     "int Tonic_GetTubeCenterCV(void *model, int tubeId, int cv, float *out3)",
     "hierarchy"),
    ("Tonic_MoveTubeCenterCV",
     "int Tonic_MoveTubeCenterCV(void *model, int tubeId, int cv, float dx, "
     "float dy, float dz)",
     "K6/K7"),
    ("Tonic_ReadTubeDeltas",
     "int Tonic_ReadTubeDeltas(void *model, int tubeId, float *out, "
     "int outCap, int *outCount)",
     "sculpt"),
    ("Tonic_SetLockParents",
     "int Tonic_SetLockParents(void *model, int tubeId, int on)",
     "K7 gate"),
    ("Tonic_SetLockChildren",
     "int Tonic_SetLockChildren(void *model, int tubeId, int on)",
     "K6 gate"),
    ("Tonic_GroupTubes",
     "int Tonic_GroupTubes(void *model, const int *tubeIds, int tubeCount, "
     "int transientParent, int *outParent)",
     "K7"),
    ("Tonic_MakePersistent",
     "int Tonic_MakePersistent(void *model, int tubeId)",
     "commit"),
    ("Tonic_ReadSmoothnessScores",
     "int Tonic_ReadSmoothnessScores(void *model, float *out, int outCap, "
     "int *outCount)",
     "K13"),
    ("Tonic_CheckRootIntersections",
     "int Tonic_CheckRootIntersections(void *model)",
     "K12"),
    ("Tonic_ReadIntersectedTubes",
     "int Tonic_ReadIntersectedTubes(void *model, int *out, int outCap, "
     "int *outCount)",
     "K12"),
)


def cApiQuote(name):
    """The quoted C signature for `name`, or "" when unknown."""
    for entry, signature, _ in REQUIRED_C_API:
        if entry == name:
            return signature
    return ""


def requireEntry(dll, name):
    """The C entry `name` on `dll`, or NotImplementedError quoting it."""
    try:
        entry = getattr(dll, name, None)
    except AttributeError:
        entry = None
    if entry is None:
        raise NotImplementedError(
            "P4 C ABI: missing %s -- %s" % (name, cApiQuote(name)))
    return entry


def missingEntries(dll):
    """The REQUIRED_C_API names `dll` does not export yet."""
    missing = []
    for name, _, _ in REQUIRED_C_API:
        try:
            found = getattr(dll, name, None) is not None
        except AttributeError:
            found = False
        if not found:
            missing.append(name)
    return missing


def _check(rc, name):
    if int(rc) != 0:
        raise RuntimeError("%s failed with code %d" % (name, int(rc)))
    return int(rc)


def subdivide(dll, model, tubeId, count, splitMode="kmeans", seed=0):
    """Subdivide `tubeId` into `count` children (K14); returns [childIds]."""
    entry = requireEntry(dll, "Tonic_SubdivideTube")
    count = validateSubdivideCount(count)
    splitMode = validateSplitMode(splitMode)
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                      ctypes.c_char_p, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    out = (ctypes.c_int * count)()
    got = ctypes.c_int(0)
    _check(entry(model, int(tubeId), count, splitMode.encode("ascii"),
                 int(seed), out, count, ctypes.byref(got)),
           "Tonic_SubdivideTube")
    return [out[i] for i in range(got.value)]


def mergeChildren(dll, model, tubeId):
    """Merge `tubeId`'s children back into it (K7 last pass + removal)."""
    entry = requireEntry(dll, "Tonic_MergeChildren")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int]
    entry.restype = ctypes.c_int
    _check(entry(model, int(tubeId)), "Tonic_MergeChildren")


def mergeSelected(dll, model, tubeIds):
    """Fold sibling `tubeIds` into one child; returns the kept tube id."""
    entry = requireEntry(dll, "Tonic_MergeSelected")
    ids = [int(v) for v in tubeIds]
    if not ids:
        raise ValueError("mergeSelected: want at least one tube id")
    entry.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                      ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    arr = (ctypes.c_int * len(ids))(*ids)
    kept = ctypes.c_int(-1)
    _check(entry(model, arr, len(ids), ctypes.byref(kept)),
           "Tonic_MergeSelected")
    return kept.value


def tubeCount(dll, model):
    """The live tube count."""
    entry = requireEntry(dll, "Tonic_GetTubeCount")
    entry.argtypes = [ctypes.c_void_p]
    entry.restype = ctypes.c_int
    return int(entry(model))


def tubeLevel(dll, model, tubeId):
    """The hierarchy level of `tubeId`."""
    entry = requireEntry(dll, "Tonic_GetTubeLevel")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int]
    entry.restype = ctypes.c_int
    return int(entry(model, int(tubeId)))


def tubeChildren(dll, model, tubeId):
    """The child tube ids of `tubeId` (empty when childless)."""
    entry = requireEntry(dll, "Tonic_GetTubeChildren")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    cap = max(tubeCount(dll, model), 1)
    out = (ctypes.c_int * cap)()
    got = ctypes.c_int(0)
    _check(entry(model, int(tubeId), out, cap, ctypes.byref(got)),
           "Tonic_GetTubeChildren")
    return [out[i] for i in range(got.value)]


def tubeCenterCount(dll, model, tubeId):
    """The center-CV count of `tubeId`."""
    entry = requireEntry(dll, "Tonic_GetTubeCenterCount")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int]
    entry.restype = ctypes.c_int
    return int(entry(model, int(tubeId)))


def tubeCenterCV(dll, model, tubeId, cv):
    """The (x, y, z) center CV `cv` of `tubeId`."""
    entry = requireEntry(dll, "Tonic_GetTubeCenterCV")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float)]
    entry.restype = ctypes.c_int
    out = (ctypes.c_float * 3)()
    _check(entry(model, int(tubeId), int(cv), out), "Tonic_GetTubeCenterCV")
    return (float(out[0]), float(out[1]), float(out[2]))


def tubeCenters(dll, model, tubeId):
    """All center CVs of `tubeId` as [(x, y, z)]."""
    return [tubeCenterCV(dll, model, tubeId, cv)
            for cv in range(tubeCenterCount(dll, model, tubeId))]


def moveTubeCenterCV(dll, model, tubeId, cv, dx, dy, dz):
    """Move one center CV of `tubeId` (drives K6/K7 unless locked)."""
    entry = requireEntry(dll, "Tonic_MoveTubeCenterCV")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                      ctypes.c_float, ctypes.c_float, ctypes.c_float]
    entry.restype = ctypes.c_int
    _check(entry(model, int(tubeId), int(cv), float(dx), float(dy),
                 float(dz)),
           "Tonic_MoveTubeCenterCV")


def tubeDeltaNorm(dll, model, tubeId):
    """The L2 norm of `tubeId`'s flattened center deltas (0 when unedited)."""
    entry = requireEntry(dll, "Tonic_ReadTubeDeltas")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    got = ctypes.c_int(0)
    _check(entry(model, int(tubeId), None, 0, ctypes.byref(got)),
           "Tonic_ReadTubeDeltas")
    if got.value <= 0:
        return 0.0
    out = (ctypes.c_float * got.value)()
    _check(entry(model, int(tubeId), out, got.value, ctypes.byref(got)),
           "Tonic_ReadTubeDeltas")
    return float(sum(float(v) * float(v) for v in out) ** 0.5)


def setLockParentsEntry(dll, model, tubeId, on):
    """Drive the K7 gate for `tubeId` (-1 = global default)."""
    entry = requireEntry(dll, "Tonic_SetLockParents")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    entry.restype = ctypes.c_int
    _check(entry(model, int(tubeId), 1 if on else 0), "Tonic_SetLockParents")


def setLockChildrenEntry(dll, model, tubeId, on):
    """Drive the K6 gate for `tubeId` (-1 = global default)."""
    entry = requireEntry(dll, "Tonic_SetLockChildren")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    entry.restype = ctypes.c_int
    _check(entry(model, int(tubeId), 1 if on else 0), "Tonic_SetLockChildren")


def groupTubes(dll, model, tubeIds, transient=True):
    """Build a K7 on-the-fly parent over `tubeIds`; returns its tube id."""
    entry = requireEntry(dll, "Tonic_GroupTubes")
    ids = [int(v) for v in tubeIds]
    if not ids:
        raise ValueError("groupTubes: want at least one tube id")
    entry.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
                      ctypes.c_int, ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    arr = (ctypes.c_int * len(ids))(*ids)
    parent = ctypes.c_int(-1)
    _check(entry(model, arr, len(ids), 1 if transient else 0,
                 ctypes.byref(parent)),
           "Tonic_GroupTubes")
    return parent.value


def makePersistent(dll, model, tubeId):
    """Keep a transient on-the-fly parent (UsdGenTubeHierarchyAPI on commit)."""
    entry = requireEntry(dll, "Tonic_MakePersistent")
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int]
    entry.restype = ctypes.c_int
    _check(entry(model, int(tubeId)), "Tonic_MakePersistent")


def readSmoothnessScores(dll, model):
    """K13 per-CV kink scores of tube 0 ([] when no tube is built)."""
    entry = requireEntry(dll, "Tonic_ReadSmoothnessScores")
    entry.argtypes = [ctypes.c_void_p,
                      ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    got = ctypes.c_int(0)
    _check(entry(model, None, 0, ctypes.byref(got)),
           "Tonic_ReadSmoothnessScores")
    if got.value <= 0:
        return []
    out = (ctypes.c_float * got.value)()
    _check(entry(model, out, got.value, ctypes.byref(got)),
           "Tonic_ReadSmoothnessScores")
    return [float(out[i]) for i in range(got.value)]


def checkRootIntersections(dll, model):
    """Run the K12 root-overlap check (post-gesture; caches the flags)."""
    entry = requireEntry(dll, "Tonic_CheckRootIntersections")
    entry.argtypes = [ctypes.c_void_p]
    entry.restype = ctypes.c_int
    _check(entry(model), "Tonic_CheckRootIntersections")


def readIntersectedTubes(dll, model):
    """Tube ids flagged by the last K12 check, ascending ([] when clear)."""
    entry = requireEntry(dll, "Tonic_ReadIntersectedTubes")
    entry.argtypes = [ctypes.c_void_p,
                      ctypes.POINTER(ctypes.c_int), ctypes.c_int,
                      ctypes.POINTER(ctypes.c_int)]
    entry.restype = ctypes.c_int
    got = ctypes.c_int(0)
    _check(entry(model, None, 0, ctypes.byref(got)),
           "Tonic_ReadIntersectedTubes")
    if got.value <= 0:
        return []
    out = (ctypes.c_int * got.value)()
    _check(entry(model, out, got.value, ctypes.byref(got)),
           "Tonic_ReadIntersectedTubes")
    return [int(out[i]) for i in range(got.value)]
