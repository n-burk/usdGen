# usdGenTonicTools.tonicHud -- Qt-free formatting for the workspace dock's
# status strip and warnings list (plan/18 section 3.5 items 5-6).
#
# Pure data in, pure data (or plain strings) out: tonicWorkspace.py is the
# only module that turns this into QLabel/QListWidget content. The
# per-mode HUD one-liners already existed one per Qt-free helper module
# (tonicGraph.hudStatus, tonicHierarchy.hierarchyStatus,
# tonicSculpt.sculptStatus, tonicFill.fillStatus, tonicTube.tubeStatus);
# this module does not re-implement them, it re-exports them, so the dock
# has one place to import every mode's status line from.
#
# statusStrip()/warnings() take a second argument shaped like TonicSession
# (see tonicPanels.py's module docstring for the exact contract): `.dll`
# (the raw ctypes handle) and `.ctx` (the TonicModelContext*, None before
# Bind scalp). Every field degrades to a state-only or empty answer when
# `.dll`/`.ctx` are unavailable, so the dock is informative but inert with
# no scalp bound.
from __future__ import annotations

import collections
import ctypes

try:
    from . import tonicFill, tonicGraph, tonicHierarchy, tonicSculpt, tonicTube
except ImportError:  # file-path test load
    import tonicFill
    import tonicGraph
    import tonicHierarchy
    import tonicSculpt
    import tonicTube

# Re-exported per-mode HUD one-liners (plan/18 section 3.1 tonicHud row).
hudStatus = tonicGraph.hudStatus
hierarchyStatus = tonicHierarchy.hierarchyStatus
sculptStatus = tonicSculpt.sculptStatus
fillStatus = tonicFill.fillStatus
tubeStatus = tonicTube.tubeStatus

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
    """The session's status dict (TonicSession.status(), plan/18 V2).

    A test double may carry the same keys as plain attributes instead of a
    status() method; read those the same way. Never returns a bound method
    as a value.

    `status` short-circuits the read with a dict the caller already has.
    One TonicSession.status() is fifteen ABI calls and a nineteen-entry
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
                 "lastSwapMs", "fallbackReason", "detached"):
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
        raw = dll.Tonic_GetDeviceFallbackReason(ctx)
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
    amber = (bakedVersion < mapVersion) or (committedVersion < modelVersion)
    fallbackReason = _fallbackReason(session, status)
    gpuText = "GPU" if not fallbackReason else "CPU-only (%s)" % fallbackReason
    return StatusStrip(
        breadcrumb=tonicHierarchy.breadcrumb(state),
        level=int(state.activeLevel),
        modelVersion=modelVersion, committedVersion=committedVersion,
        pendingVersion=pendingVersion, mapVersion=mapVersion,
        bakedVersion=bakedVersion, amber=amber,
        lastSwapMs=float(_get(session, "lastSwapMs", 0.0, status)),
        gpuText=gpuText, fallbackReason=fallbackReason)


def _selectTubesAction(tubeIds):
    ids = [int(v) for v in tubeIds]

    def action(container):
        session = getattr(container, "session", None)
        dll, ctx = _dll(session), _ctx(session)
        if dll is None or ctx is None or not ids:
            return
        idArr = (ctypes.c_int * len(ids))(*ids)
        dll.Tonic_SelectSet(ctx, tonicTube.PICK_TUBE_VERT, idArr, None,
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
        dll.Tonic_SelectSet(ctx, tonicTube.PICK_CENTER_CV, ids, subIds,
                            None, len(idxs))
        if session is not None and hasattr(session, "publish"):
            session.publish()
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
            bool(_get(session, "detached", False, status)))


def warnings(state, dll, status=None):
    """The dock's warnings list: (severity, text, selectAction) rows for
    coverage gaps, root intersections, kink spikes, device fallback and a
    detached committer (plan/18 section 3.5 item 5). `dll` is the
    session-shaped object tonicPanels.py's get/set closures also take.
    """
    session = dll
    status = _status(session, status)
    out = []
    d, ctx = _dll(session), _ctx(session)
    if d is not None and ctx is not None:
        regions = ctypes.c_int(0)
        uncovered = ctypes.c_int(0)
        intersectedRegions = ctypes.c_int(0)
        rc = d.Tonic_GetRegionStats(ctx, ctypes.byref(regions),
                                    ctypes.byref(uncovered),
                                    ctypes.byref(intersectedRegions))
        if int(rc) == 0 and uncovered.value > 0:
            out.append(Warning(
                "warning",
                "%d face(s) uncovered by any region." % uncovered.value,
                None))

        ids = (ctypes.c_int * MAX_INTERSECTED_TUBES)()
        got = ctypes.c_int(0)
        rc = d.Tonic_ReadIntersectedTubes(ctx, ids, MAX_INTERSECTED_TUBES,
                                          ctypes.byref(got))
        if int(rc) == 0 and got.value > 0:
            # Slicing a ctypes array builds the list in C; the per-index
            # comprehension it replaces was the measurable half of this
            # function at 4 096 scores.
            tubeIds = ids[:got.value]
            text = tonicHierarchy.intersectionWarning(tubeIds)
            if text:
                out.append(Warning("warning", text,
                                   _selectTubesAction(tubeIds)))

        scores = (ctypes.c_float * MAX_SMOOTHNESS_SCORES)()
        got = ctypes.c_int(0)
        rc = d.Tonic_ReadSmoothnessScores(ctx, scores,
                                          MAX_SMOOTHNESS_SCORES,
                                          ctypes.byref(got))
        if int(rc) == 0 and got.value > 0:
            values = scores[:got.value]
            text = tonicHierarchy.smoothnessWarning(values)
            if text:
                spikes = [i for i, s in enumerate(values)
                         if s >= tonicHierarchy.SMOOTHNESS_SPIKE_THRESHOLD]
                out.append(Warning("warning", text,
                                   _selectCentersAction(spikes)))

    fallbackReason = _fallbackReason(session, status)
    if fallbackReason:
        out.append(Warning("info", "CPU-only: %s" % fallbackReason, None))

    if _get(session, "detached", False, status):
        out.append(Warning(
            "error",
            "Committer detached: edits are not reaching the stage.", None))

    return out
