# usdGenPomadeTools.pomadeHud -- Qt-free formatting for the workspace dock's
# status strip and warnings list (plan/18 section 3.5 items 5-6).
#
# Pure data in, pure data (or plain strings) out: pomadeWorkspace.py is the
# only module that turns this into QLabel/QListWidget content. The
# per-mode HUD one-liners already existed one per Qt-free helper module
# (pomadeGraph.hudStatus, pomadeHierarchy.hierarchyStatus,
# pomadeSculpt.sculptStatus, pomadeFill.fillStatus, pomadeTube.tubeStatus);
# this module does not re-implement them, it re-exports them, so the dock
# has one place to import every mode's status line from.
#
# statusStrip()/warnings() take a second argument shaped like PomadeSession
# (see pomadePanels.py's module docstring for the exact contract): `.dll`
# (the raw ctypes handle) and `.ctx` (the PomadeModelContext*, None before
# Bind scalp). Every field degrades to a state-only or empty answer when
# `.dll`/`.ctx` are unavailable, so the dock is informative but inert with
# no scalp bound.
from __future__ import annotations

import collections
import ctypes

try:
    from . import pomadeFill, pomadeGraph, pomadeHierarchy, pomadeSculpt, pomadeTube
    from . import pomadeLib
except ImportError:  # file-path test load
    import pomadeFill
    import pomadeGraph
    import pomadeHierarchy
    import pomadeLib
    import pomadeSculpt
    import pomadeTube

# Re-exported per-mode HUD one-liners (plan/18 section 3.1 pomadeHud row).
hudStatus = pomadeGraph.hudStatus
hierarchyStatus = pomadeHierarchy.hierarchyStatus
sculptStatus = pomadeSculpt.sculptStatus
fillStatus = pomadeFill.fillStatus
tubeStatus = pomadeTube.tubeStatus

StatusStrip = collections.namedtuple(
    "StatusStrip",
    ("breadcrumb", "level", "modelVersion", "committedVersion",
     "pendingVersion", "mapVersion", "bakedVersion", "amber", "lastSwapMs",
     "gpuText", "fallbackReason"))

Warning = collections.namedtuple("Warning", ("severity", "text",
                                             "selectAction"))

# Bounded reads for the periodic (250 ms) refresh: generous enough for any
# example scene, small enough to stay cheap every tick.
MAX_INTERSECTED_TUBES = 256
MAX_SMOOTHNESS_SCORES = 4096


def _dll(session):
    return getattr(session, "dll", None) if session is not None else None


def _ctx(session):
    return getattr(session, "ctx", None) if session is not None else None


def _status(session, status=None):
    """The session's status dict (PomadeSession.status(), plan/18 V2).

    A test double may carry the same keys as plain attributes instead of a
    status() method; read those the same way. Never returns a bound method
    as a value.

    `status` short-circuits the read with a dict the caller already has.
    One PomadeSession.status() is fifteen ABI calls and a nineteen-entry
    dict, and the dock used to build three of them per refresh at four
    refreshes per artist op; the dock now reads one and passes it here.
    """
    if status is not None:
        return status
    if session is None:
        return {}
    status = getattr(session, "status", None)
    if callable(status):
        return dict(status() or {})
    out = {}
    for name in ("modelVersion", "committedVersion", "pendingVersion",
                 "lastSwapMs", "fallbackReason", "detached", "commitError"):
        value = getattr(session, name, None)
        if value is not None and not callable(value):
            out[name] = value
    return out


def _get(obj, name, default=None, status=None):
    """`name` from the session's status dict, else default."""
    return _status(obj, status).get(name, default)


def _fallbackReason(session, status=None):
    if status is not None and "fallbackReason" in status:
        return status["fallbackReason"]
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        raw = dll.Pomade_GetDeviceFallbackReason(ctx)
        return raw.decode("utf-8") if raw else ""
    return _get(session, "fallbackReason", "")


def statusStrip(state, session, status=None):
    """The status-strip fields (plan/18 section 3.5 item 6): breadcrumb,
    level, model/stage/map versions, the amber skew flag, last swap time
    and GPU/fallback text."""
    status = _status(session, status)
    modelVersion = int(_get(session, "modelVersion", 0, status))
    committedVersion = int(_get(session, "committedVersion", 0, status))
    pendingVersion = int(_get(session, "pendingVersion", 0, status))
    mapVersion = int(state.mapVersion)
    bakedVersion = int(state.bakedVersion)
    # Amber means work is actually in flight: a map newer than the last
    # bake, or a commit enqueued and not yet swapped. It used to compare
    # against the MODEL version, which every selection publish bumps
    # without ever enqueueing a commit, so the strip sat amber almost
    # permanently and meant nothing (DK-05).
    amber = (bakedVersion < mapVersion) or (committedVersion < pendingVersion)
    fallbackReason = _fallbackReason(session, status)
    gpuText = "GPU" if not fallbackReason else "CPU-only (%s)" % fallbackReason
    return StatusStrip(
        breadcrumb=pomadeHierarchy.breadcrumb(state),
        level=int(state.activeLevel),
        modelVersion=modelVersion, committedVersion=committedVersion,
        pendingVersion=pendingVersion, mapVersion=mapVersion,
        bakedVersion=bakedVersion, amber=amber,
        lastSwapMs=float(_get(session, "lastSwapMs", 0.0, status)),
        gpuText=gpuText, fallbackReason=fallbackReason)


# syncSummary tones: the dock maps each to one pill colour.
TONE_OK = "ok"
TONE_BUSY = "busy"
TONE_INFO = "info"
TONE_ERROR = "error"
# Nothing to report on yet (no scalp bound): neither good nor bad.
TONE_NEUTRAL = "neutral"
NO_MODEL_TEXT = "No scalp bound"


def syncSummary(strip, status=None):
    """The sync pill: (text, tone) for a statusStrip() result.

    One artist-facing word for what the version dump says (DK-05). The
    order is severity first: a failed commit outranks work in flight,
    work in flight outranks a standing condition (CPU fallback, reduced
    detail), and only then is the groom "Synced". `status` is the
    session's status() dict; `commitError` (SS-05) and `ladderStep` come
    from it, everything else from the strip.

    Before a scalp is bound there is no model and nothing to sync, so a
    status whose `active` is False reads 'No scalp bound' in the neutral
    tone rather than a green 'Synced' over a dock whose tools are all
    greyed out. A status without the key (a test double, None) keeps the
    old matrix.
    """
    status = status or {}
    if "active" in status and not status.get("active"):
        return (NO_MODEL_TEXT, TONE_NEUTRAL)
    error = str(status.get("commitError") or "")
    if error:
        return ("Commit failed: %s" % error, TONE_ERROR)
    if status.get("detached") or status.get("committerDetached"):
        return ("Commit failed: committer detached", TONE_ERROR)
    if strip.committedVersion < strip.pendingVersion:
        return ("Committing...", TONE_BUSY)
    if strip.bakedVersion < strip.mapVersion:
        return ("Baking map...", TONE_BUSY)
    if strip.fallbackReason:
        return ("CPU fallback: %s" % strip.fallbackReason, TONE_INFO)
    if int(status.get("ladderStep", 0) or 0) > 0:
        return ("Reduced detail", TONE_INFO)
    return ("Synced", TONE_OK)


def diagnosticsText(strip, status=None):
    """The version dump behind the sync pill (its tooltip and the
    Show diagnostics line): level, model/stage/pending/map/baked
    versions, the last swap time, the device and the ladder step.

    No leading '| ': the strip used to open with a bare separator."""
    status = status or {}
    ladder = int(status.get("ladderStep", 0) or 0)
    parts = ["L%d" % strip.level,
             "model v%d stage v%d pending v%d" % (
                 strip.modelVersion, strip.committedVersion,
                 strip.pendingVersion),
             "map v%d baked v%d" % (strip.mapVersion, strip.bakedVersion),
             "swap %.1f ms" % strip.lastSwapMs,
             strip.gpuText]
    if ladder:
        parts.append("fidelity step %d" % ladder)
    return " | ".join(parts)


def deviceChip(strip):
    """The GPU/CPU chip text: 'GPU', or 'CPU' when the device fell back."""
    return "CPU" if strip.fallbackReason else "GPU"


def refillDropWarning(drops):
    """The warning text for tubes the last refill skipped, or "".

    `drops` is pomadeLib.readRefillDrops' [(tubeId, reason)]. Only the
    first reason is quoted: they almost always share one cause.
    """
    if not drops:
        return ""
    names = ", ".join("T%d" % int(t) for t, _ in drops[:6])
    if len(drops) > 6:
        names += ", ..."
    reason = drops[0][1] or "unknown reason"
    return ("%d tube(s) produced no guides (%s): %s"
            % (len(drops), names, reason))


def _selectTubesAction(tubeIds):
    ids = [int(v) for v in tubeIds]

    def action(container):
        session = getattr(container, "session", None)
        dll, ctx = _dll(session), _ctx(session)
        if dll is None or ctx is None or not ids:
            return
        idArr = (ctypes.c_int * len(ids))(*ids)
        dll.Pomade_SelectSet(ctx, pomadeTube.PICK_TUBE_VERT, idArr, None,
                            None, len(ids))
        if session is not None and hasattr(session, "publish"):
            session.publish()
    return action


def _selectCentersAction(cvIndices):
    idxs = [int(v) for v in cvIndices]

    def action(container):
        session = getattr(container, "session", None)
        dll, ctx = _dll(session), _ctx(session)
        if dll is None or ctx is None or not idxs:
            return
        ids = (ctypes.c_int * len(idxs))(*([0] * len(idxs)))
        subIds = (ctypes.c_int * len(idxs))(*idxs)
        dll.Pomade_SelectSet(ctx, pomadeTube.PICK_CENTER_CV, ids, subIds,
                            None, len(idxs))
        if session is not None and hasattr(session, "publish"):
            session.publish()
    return action


def coverageWarning(uncovered):
    """The coverage row's text: how many scalp faces no region claims."""
    count = int(uncovered)
    if count <= 0:
        return ""
    if count == 1:
        return "1 scalp face has no region"
    return "%d scalp faces have no region" % count


def readUncoveredFaces(dll, ctx):
    """Scalp face indices no region claims (the -1 entries of the K3 map).

    Parent-mesh ids. A face GeomSubset scalp's other faces read -1 too, but
    they are not scalp (plan/02 section 2.20), so they are never reported.
    """
    if dll is None or ctx is None:
        return []
    count = ctypes.c_int(0)
    if int(dll.Pomade_ReadFaceRegions(ctx, None, 0, ctypes.byref(count))) \
            != 0 or count.value <= 0:
        return []
    out = (ctypes.c_int * count.value)()
    if int(dll.Pomade_ReadFaceRegions(ctx, out, count.value,
                                     ctypes.byref(count))) != 0:
        return []
    active = _readFaceActive(dll, ctx, count.value)
    return [i for i, region in enumerate(out[:count.value])
            if region < 0 and (active is None or active[i])]


def _readFaceActive(dll, ctx, faceCount):
    """Per parent face 1/0 (on the growth surface), or None for "all"."""
    read = getattr(dll, "Pomade_ReadScalpFaceActive", None)
    if read is None:
        return None
    active = (ctypes.c_int * faceCount)()
    got = ctypes.c_int(0)
    if int(read(ctx, active, faceCount, ctypes.byref(got))) != 0 or \
            got.value != faceCount:
        return None
    return active


# The commit-failure row's action, named for the dock's tooltip/menus.
RETRY_COMMIT_LABEL = "Retry commit"
OUTLINE_FACES_LABEL = "Outline the faces"
CLICK_TO_SELECT = " - click to select"


def clickHint(action):
    """The tooltip suffix a warnings row's click earns: what it does.

    Rows whose click selects tubes or CVs read ' - click to select'; a row
    whose action carries a `.label` (Retry commit, Outline the faces) says
    that instead, so the hover never promises a selection the click will
    not make. No action, no suffix.
    """
    if action is None:
        return ""
    label = str(getattr(action, "label", "") or "")
    return (" - click to %s" % (label[:1].lower() + label[1:])
            if label else CLICK_TO_SELECT)


def commitErrorWarning(commitError):
    """The commit-failure row's text, or "" when the commit is healthy."""
    reason = str(commitError or "")
    return ("Commit failed: %s" % reason) if reason else ""


def _retryCommitAction():
    """The commit-failure row's click: enqueue the model again (SS-05).

    The committer never retries a failed version on its own (it would
    spin on a persistent failure), so the artist's retry is an explicit
    enqueue; the session forgets the old reason and reports anew if the
    retry fails too.
    """

    def action(container):
        session = getattr(container, "session", None)
        enqueue = getattr(session, "enqueueCommit", None)
        if callable(enqueue):
            enqueue()
    action.label = RETRY_COMMIT_LABEL
    return action


def _highlightUncoveredAction():
    """The coverage row's click: outline the uncovered scalp faces.

    Neither the Hydra publish nor usdview's selection can highlight a
    face subset, so the dock draws them as a viewport overlay
    (PomadeWorkspace.highlightScalpFaces); the action only reads which
    faces those are.
    """

    def action(container):
        session = getattr(container, "session", None)
        faces = readUncoveredFaces(_dll(session), _ctx(session))
        workspace = getattr(container, "workspace", None)
        highlight = getattr(workspace, "highlightScalpFaces", None)
        if highlight is not None:
            highlight(faces)
    action.label = OUTLINE_FACES_LABEL
    return action


def warningsKey(state, session, status=None):
    """The cheap signature of everything `warnings()` reads.

    The dock refreshes on every publish, on every idle pump and on a
    250 ms timer -- about four times per artist op in the TN-5 controller
    soak, where the full re-read cost 2.6-5.9 ms per op on its own. Every
    input warnings() has is a function of the model version (region
    stats, root intersections, smoothness scores), the device fallback or
    the committer's attachment, so an unchanged key means an unchanged
    list and the dock can skip the read and the widget rebuild entirely.
    """
    status = _status(session, status)
    version = _get(session, "modelVersion", None, status)
    if version is None:
        # Nothing to key on (no session, or a stand-in that reports no
        # version): say so rather than return a constant, so the caller
        # re-reads every time instead of caching the first answer for
        # ever.
        return None
    return (int(version), _fallbackReason(session, status),
            bool(_get(session, "detached", False, status)),
            str(_get(session, "scalpMissing", "", status) or ""),
            # A commit failure arrives from the worker without a model
            # version bump, so it must move the key on its own (SS-05).
            str(_get(session, "commitError", "", status) or ""))


def warnings(state, dll, status=None):
    """The dock's warnings list: (severity, text, selectAction) rows for
    tubes the last refill skipped (no guides), coarse centroid coverage
    gaps, root intersections, kink spikes, device
    fallback and a
    detached committer (plan/18 section 3.5 item 5). `dll` is the
    session-shaped object pomadePanels.py's get/set closures also take.
    """
    session = dll
    status = _status(session, status)
    out = []
    # First, because nothing else on the list matters while the stage is
    # not receiving the groom (SS-05); clicking the row retries.
    text = commitErrorWarning(_get(session, "commitError", "", status))
    if text:
        out.append(Warning("error", text, _retryCommitAction()))
    d, ctx = _dll(session), _ctx(session)
    if d is not None and ctx is not None:
        # Tubes the last refill skipped: the refill itself succeeded (some
        # tube filled), so without this row they vanish silently.
        drops = pomadeLib.readRefillDrops(d, ctx)
        if drops:
            out.append(Warning("warning", refillDropWarning(drops),
                               _selectTubesAction([t for t, _ in drops])))

        regions = ctypes.c_int(0)
        uncovered = ctypes.c_int(0)
        intersectedRegions = ctypes.c_int(0)
        rc = d.Pomade_GetRegionStats(ctx, ctypes.byref(regions),
                                    ctypes.byref(uncovered),
                                    ctypes.byref(intersectedRegions))
        if int(rc) == 0 and uncovered.value > 0:
            # Info, not a warning: a scalp is only partly groomed for
            # most of a session, and the row is there to find the gap.
            out.append(Warning("info", coverageWarning(uncovered.value),
                               _highlightUncoveredAction()))

        ids = (ctypes.c_int * MAX_INTERSECTED_TUBES)()
        got = ctypes.c_int(0)
        rc = d.Pomade_ReadIntersectedTubes(ctx, ids, MAX_INTERSECTED_TUBES,
                                          ctypes.byref(got))
        if int(rc) == 0 and got.value > 0:
            # Slicing a ctypes array builds the list in C; the per-index
            # comprehension it replaces was the measurable half of this
            # function at 4 096 scores.
            tubeIds = ids[:got.value]
            text = pomadeHierarchy.intersectionWarning(tubeIds)
            if text:
                out.append(Warning("warning", text,
                                   _selectTubesAction(tubeIds)))

        scores = (ctypes.c_float * MAX_SMOOTHNESS_SCORES)()
        got = ctypes.c_int(0)
        rc = d.Pomade_ReadSmoothnessScores(ctx, scores,
                                          MAX_SMOOTHNESS_SCORES,
                                          ctypes.byref(got))
        if int(rc) == 0 and got.value > 0:
            values = scores[:got.value]
            # Pomade_ReadSmoothnessScores reads tube 0 (pomadeApi.h).
            text = pomadeHierarchy.smoothnessWarning(values, tubeId=0)
            if text:
                spikes = [i for i, s in enumerate(values)
                         if s >= pomadeHierarchy.SMOOTHNESS_SPIKE_THRESHOLD]
                out.append(Warning("warning", text,
                                   _selectCentersAction(spikes)))

    fallbackReason = _fallbackReason(session, status)
    if fallbackReason:
        out.append(Warning("info", "CPU-only: %s" % fallbackReason, None))

    scalpMissing = str(_get(session, "scalpMissing", "", status) or "")
    if scalpMissing:
        # A reopen that lost the scalp (SS-01): the committer is detached
        # too, but the artist can only fix it by binding a scalp again.
        out.append(Warning(
            "error",
            "Scalp %s not found in the new stage: bind a scalp mesh to "
            "continue." % scalpMissing, None))
    elif _get(session, "detached", False, status):
        out.append(Warning(
            "error",
            "Committer detached: edits are not reaching the stage.", None))

    return out
