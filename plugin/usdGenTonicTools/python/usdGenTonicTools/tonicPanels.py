# usdGenTonicTools.tonicPanels -- Qt-free per-mode parameter/action
# descriptors for the Tonic workspace dock (plan/18 section 3.5 items 3-4;
# plan/08 section 5.2: panels are generated, never hand-written).
#
# descriptors(modeId, state) returns the ordered parameter rows for one
# mode's QFormLayout; actions(modeId) returns its one-shot buttons. Both are
# pure data plus closures: no Qt, no pxr, so tonicWorkspace.py is the only
# module that turns them into widgets, and testUsdGenTonicToolsPanels.py
# exercises every closure against a fake session.
#
# A descriptor's get(state, dll) / set(state, dll, value) take a second
# argument shaped like TonicSession (plan/18 section 3.1): `.dll` is the raw
# ctypes handle (tonicLib.Library.dll) and `.ctx` is the TonicModelContext*
# the C ABI calls need (None before Bind scalp). Every get/set degrades to
# reading and writing `state` alone when `.dll`/`.ctx` are unavailable, so
# the panel is inert but harmless with no scalp bound -- exactly the state
# testUsdGenTonicToolsPanels.py exercises with a fake session and the state
# tonicWorkspace.refresh() sees for real once `container.session` exists.
# An action's handler(container) gets the whole container so it can reach
# both `container.tonicState` (the params just edited) and
# `container.session` (dll, ctx, publish()).
#
# Sibling helper modules are imported with a package/file-path fallback (the
# same trick testUsdGenTonicToolsHierarchy.py's _load() needs on the far
# side): a real usdview load resolves them as package members, a file-path
# test load pre-registers their bare names in sys.modules.
from __future__ import annotations

import collections
import ctypes

try:
    from . import tonicFill, tonicHierarchy, tonicSculpt, tonicTube
except ImportError:  # file-path test load
    import tonicFill
    import tonicHierarchy
    import tonicSculpt
    import tonicTube

Descriptor = collections.namedtuple(
    "Descriptor",
    ("id", "label", "kind", "min", "max", "step", "choices", "get", "set"))

Action = collections.namedtuple(
    "Action", ("id", "label", "hotkeyLabel", "handler"))

# The maximum length-profile floats a Fill descriptor round-trips in one
# Tonic_GetFillParams call (32 (position, value) knots is generous for a
# hand-authored ramp).
MAX_FILL_PROFILE_FLOATS = 64
# Selection-read and per-tube-op scratch caps (plan/18 section 2.3: a
# marquee or a hierarchy action rarely touches more than a few hundred
# tubes at once).
MAX_SELECTION = 256

# Shared wording for the Graph and Output controls.  The option is a
# per-face Ptex bake resolution; it is independent from guide density in
# Fill.  Keep this on the Qt-free descriptor module so headless callers and
# the dock show the same explanation.
TEXEL_RESOLUTION_HELP = (
    "Ptex texels per face side. Auto uses 64×64 on region boundaries; "
    "higher values resolve finer regions. Guide density is controlled in "
    "Fill."
)


def _descriptor(id, label, kind, get, set, min=0.0, max=0.0, step=0.0,
                choices=()):
    return Descriptor(id, label, kind, min, max, step, tuple(choices),
                      get, set)


# ---- session access (defensive: every call works with session=None) ------

def _dll(session):
    return getattr(session, "dll", None) if session is not None else None


def _ctx(session):
    return getattr(session, "ctx", None) if session is not None else None


def _publish(session):
    if session is not None and hasattr(session, "publish"):
        session.publish()


def _stage(session):
    """The per-tube half of the ABI (plan/18 section 7 G2), or None.

    A session that predates it -- or a test's fake one -- answers None and
    every caller below falls back to the tube-0 spelling of the same
    operation, which is that call with id 0.
    """
    return getattr(session, "stageLib", None) if session is not None else None


def _perTube(session, entry, fallback, *args):
    """Run one operation over the tube selection, or over tube 0.

    `entry` is the StageLibrary method (it takes ctx, tubeId, ...) and
    `fallback` the tonicApi.h name that means the same thing on the
    primary tube. Nothing selected means the primary tube, so a groom with
    one tube needs no click before its buttons work.
    """
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return 0
    stage = _stage(session)
    tubes = selectedTubeIds(session)
    if stage is None or not tubes:
        getattr(dll, fallback)(ctx, *args)
        return 1
    done = 0
    for tubeId in tubes:
        getattr(stage, entry)(ctx, tubeId, *args)
        done += 1
    return done


def _loop(container, modeId):
    """The live loop for `modeId`, or None.

    An action the artist clicks in the dock and the same action on its
    hotkey have to be the same edit -- one undo bracket, one status line,
    one commit -- so a handler prefers the loop and only falls back to the
    bare ABI when the viewport controller is not up (the headless panel
    test, a dock opened before Bind scalp).
    """
    viewport = getattr(container, "viewport", None)
    loop = getattr(viewport, "loop", None) if viewport is not None else None
    return loop if getattr(loop, "modeId", "") == modeId else None


def selectedTubeIds(session):
    """Distinct owning tube ids in the current editable selection.

    A CV or section-ring selection carries its owner in the same first
    tuple slot as a whole TubeVert selection. Resolve all editable kinds so
    panel actions follow a selected child tube instead of falling back to
    tube 0 merely because the whole-tube kind is empty.
    """
    readSelection = getattr(session, "readSelection", None)
    if callable(readSelection):
        seen = set()
        for kind in (tonicTube.PICK_TUBE_VERT,
                     tonicTube.PICK_CENTER_CV,
                     tonicTube.PICK_SECTION_CV,
                     tonicTube.PICK_SECTION_RING):
            try:
                entries = readSelection(kind)
            except (AttributeError, TypeError, RuntimeError):
                continue
            for entry in entries or ():
                try:
                    seen.add(int(entry[0]))
                except (IndexError, TypeError, ValueError):
                    continue
        return sorted(seen)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return []
    seen = set()
    for kind in (tonicTube.PICK_TUBE_VERT,
                 tonicTube.PICK_CENTER_CV,
                 tonicTube.PICK_SECTION_CV,
                 tonicTube.PICK_SECTION_RING):
        outIds = (ctypes.c_int * MAX_SELECTION)()
        outSub = (ctypes.c_int * MAX_SELECTION)()
        outSubSub = (ctypes.c_int * MAX_SELECTION)()
        got = ctypes.c_int(0)
        rc = dll.Tonic_ReadSelection(ctx, kind, outIds, outSub, outSubSub,
                                     MAX_SELECTION, ctypes.byref(got))
        if int(rc) != 0:
            continue
        for i in range(got.value):
            seen.add(int(outIds[i]))
    return sorted(seen)


# ---- generic scalar descriptor (state field, optionally ABI-backed) ------

def _scalarDescriptor(id, label, kind, stateAttr, cast, dllGetName=None,
                      dllSetName=None, dllCast=None, **limits):
    """A descriptor over one TonicToolState field, mirrored to the model
    through a same-shaped Tonic_Get<X>(ctx) / Tonic_Set<X>(ctx, value) pair
    when the session has a live context; state-only otherwise."""
    dllCast = dllCast if dllCast is not None else cast

    def get(state, session):
        dll, ctx = _dll(session), _ctx(session)
        if dllGetName and dll is not None and ctx is not None:
            value = cast(getattr(dll, dllGetName)(ctx))
            setattr(state, stateAttr, value)
            return value
        return cast(getattr(state, stateAttr))

    def set(state, session, value):
        value = cast(value)
        setattr(state, stateAttr, value)
        dll, ctx = _dll(session), _ctx(session)
        if dllSetName and dll is not None and ctx is not None:
            getattr(dll, dllSetName)(ctx, dllCast(value))

    return _descriptor(id, label, kind, get, set, **limits)


def _boolCast(v):
    return bool(v)


def _boolToInt(v):
    return 1 if v else 0


# ---- Graph ----------------------------------------------------------------

def _graphDescriptors(state):
    return [
        _scalarDescriptor(
            "snapRadiusPx", "Snap radius (px)", "float", "snapRadiusPx",
            float, "Tonic_GetSnapRadius", "Tonic_SetSnapRadius",
            min=1.0, max=64.0, step=1.0),
        _scalarDescriptor(
            "mirrorX", "Mirror X", "bool", "mirrorX", _boolCast,
            "Tonic_GetMirrorX", "Tonic_SetMirrorX", dllCast=_boolToInt),
        _descriptor("texelResolution", "Ptex texels per face side", "enum",
                    _texelResolutionGet, _texelResolutionSet,
                    choices=TEXEL_CHOICES),
    ]


def _weldAllHandler(container):
    session = getattr(container, "session", None)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    state = container.tonicState
    welds = ctypes.c_int(0)
    dll.Tonic_GraphWeldAll(ctx, float(state.snapRadiusPx),
                           ctypes.byref(welds))
    _publish(session)


def _rebakeHandler(container):
    session = getattr(container, "session", None)
    if session is not None and hasattr(session, "rebake"):
        session.rebake()


def _graphActions(state):
    return [
        Action("weldAll", "Weld all within radius", "", _weldAllHandler),
        Action("rebake", "Rebake map now", "", _rebakeHandler),
    ]


# ---- Tube -------------------------------------------------------------

def _softRadiusGet(state, session):
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        center = ctypes.c_float(0.0)
        radius = ctypes.c_float(0.0)
        dll.Tonic_GetSoftSelection(ctx, ctypes.byref(center),
                                   ctypes.byref(radius))
        state.softCenter = float(center.value)
        state.softRadius = float(radius.value)
    return float(state.softRadius)


def _softRadiusSet(state, session, value):
    state.softRadius = float(value)
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        dll.Tonic_SetSoftSelection(ctx, float(state.softCenter),
                                   float(state.softRadius))


def _selectionShapeGet(state, _session):
    value = str(getattr(state, "selectionShape", "box")).lower()
    return value if value in ("box", "lasso") else "box"


def _selectionShapeSet(state, _session, value):
    value = str(value).lower()
    state.selectionShape = value if value in ("box", "lasso") else "box"


def _transformToolGet(state, _session):
    value = str(getattr(state, "transformTool", "move")).lower()
    return value if value in ("select", "move", "rotate", "scale") \
        else "move"


def _transformToolSet(state, _session, value):
    value = str(value).lower()
    state.transformTool = (value if value in ("select", "move", "rotate",
                                               "scale") else "move")


def _tubeCache(state):
    return state.panels.setdefault("tube", {"ringCvCount": tonicTube.
                                            DEFAULT_REGION_RING_VERTS})


def _ringCvCountGet(state, session):
    cache = _tubeCache(state)
    value = tonicTube.regionRingVerts(cache.get(
        "ringCvCount", tonicTube.DEFAULT_REGION_RING_VERTS))
    cache["ringCvCount"] = value
    return value


def _ringCvCountSet(state, session, value):
    _tubeCache(state)["ringCvCount"] = tonicTube.regionRingVerts(value)


def _selectedSectionRings(session):
    if session is None or not hasattr(session, "readSelection"):
        return []
    try:
        rings = list(session.readSelection(tonicTube.PICK_SECTION_RING))
        if rings:
            return rings
        # A section-CV selection still identifies its owning ring, so the
        # same absolute scale control remains useful in Section CVs mode.
        cvs = list(session.readSelection(tonicTube.PICK_SECTION_CV))
        return [(tubeId, ring, -1) for tubeId, ring, _slot in cvs]
    except (AttributeError, TypeError, RuntimeError):
        return []


def _readSectionScale(session, tubeId, ring):
    """Read one ring scale through the optional bridge helper."""
    if session is None or getattr(session, "model", None) is None:
        return None
    try:
        try:
            from . import tonicBridge
        except ImportError:
            import tonicBridge
        section = tonicBridge.tubeSection(session.dll, session.model,
                                          int(tubeId), int(ring))
        return float(section[2])
    except (ImportError, AttributeError, RuntimeError, TypeError, ValueError):
        return None


def _uniformScaleGet(state, session):
    cache = _tubeCache(state)
    rings = _selectedSectionRings(session)
    if rings:
        scale = _readSectionScale(session, rings[0][0], rings[0][1])
        if scale is not None and scale > 0.0:
            cache["uniformScale"] = scale
    return float(cache.get("uniformScale", 1.0))


def _uniformScaleSet(state, session, value):
    value = max(0.01, min(100.0, float(value)))
    rings = _selectedSectionRings(session)
    if not rings or session is None or getattr(session, "model", None) is None:
        _tubeCache(state)["uniformScale"] = value
        return
    stage = _stage(session)
    if stage is None:
        return
    targets = []
    seen = set()
    for tubeId, ring, _subSub in rings:
        key = (int(tubeId), int(ring))
        if key in seen:
            continue
        current = _readSectionScale(session, key[0], key[1])
        if current is None or current <= 0.0:
            return
        seen.add(key)
        targets.append((key[0], key[1], current))
    if not targets:
        return
    begin = getattr(session, "beginGesture", None)
    if not callable(begin) or not begin("Tube section scale"):
        return
    ok = True
    refillError = False
    try:
        for tubeId, ring, current in targets:
            result = stage.scaleSectionRing(
                session.ctx, tubeId, ring, value / current)
            if (result is False or
                    isinstance(result, (int, float)) and result != 0):
                ok = False
                break
    except (AttributeError, RuntimeError, TypeError, ValueError):
        ok = False
    if ok:
        session.endGesture()
        _tubeCache(state)["uniformScale"] = value
        try:
            try:
                from . import tonicLoopsTube
            except ImportError:
                import tonicLoopsTube
            tonicLoopsTube.restoreGuides(
                session, None, refill=True)
        except (ImportError, AttributeError, RuntimeError, TypeError):
            refillError = True
        enqueue = getattr(session, "enqueueCommit", None)
        if callable(enqueue):
            enqueue()
        _publish(session)
        if refillError:
            report = getattr(session, "report", None)
            if callable(report):
                report("Tonic Tube: section changed; guide refill unavailable")
    else:
        cancel = getattr(session, "cancelGesture", None)
        dirty = cancel() if callable(cancel) else 0
        publish = getattr(session, "publish", None)
        if callable(publish):
            publish(dirty)
        report = getattr(session, "report", None)
        if callable(report):
            report("Tonic Tube: section scale failed; edit cancelled")


def _tubeDescriptors(state):
    return [
        _descriptor("selectionShape", "Selection shape", "enum",
                   _selectionShapeGet, _selectionShapeSet,
                   choices=("box", "lasso")),
        _descriptor("transformTool", "Transform", "enum",
                   _transformToolGet, _transformToolSet,
                   choices=("move", "rotate", "scale", "select")),
        _descriptor("softRadius", "Soft-selection radius (t)", "float",
                   _softRadiusGet, _softRadiusSet, min=0.0, max=1.0,
                   step=0.01),
        _scalarDescriptor(
            "displaySegments", "Display segments", "int", "displaySegments",
            int, "Tonic_GetDisplaySegments", "Tonic_SetDisplaySegments",
            min=1, max=8, step=1),
        _descriptor("ringCvCount", "Ring CVs (next region tube)", "enum",
                   _ringCvCountGet, _ringCvCountSet,
                   choices=tonicTube.REGION_RING_VERT_CHOICES),
        _descriptor("uniformScale", "Selected section scale", "float",
                   _uniformScaleGet, _uniformScaleSet,
                   min=0.01, max=100.0, step=0.05),
    ]


def _matchSurfaceHandler(container):
    session = getattr(container, "session", None)
    _perTube(session, "matchSurface", "Tonic_MatchSurface")
    _publish(session)


def _relaxHandler(container):
    session = getattr(container, "session", None)
    _perTube(session, "relaxCenter", "Tonic_RelaxCenter", 1.0, 1)
    _publish(session)


def _snapRootHandler(container):
    session = getattr(container, "session", None)
    _perTube(session, "snapRootToScalp", "Tonic_SnapRootToScalp")
    _publish(session)


def _tubeActions(state):
    return [
        Action("matchSurface", "Match surface", "", _matchSurfaceHandler),
        Action("relax", "Relax", "", _relaxHandler),
        Action("snapRoot", "Snap root to scalp", "", _snapRootHandler),
    ]


# ---- Fill -------------------------------------------------------------

def _fillCache(state):
    return state.panels.setdefault("fill", {
        "density": 8.0, "cvCount": 8, "seed": 0, "edgeBias": 0.0,
        "profile": [],
    })


def _readFillParams(session):
    """The fill params the panel shows, or None with no live context.

    Per plan/17 section 5.3 the numbers belong to a SELECTION, so what the
    panel shows is the first selected tube's (the primary tube's when
    nothing is selected). The per-tube read hands back a profile count
    rather than the profile itself, so the ramp comes from the tube-0
    entry and is dropped when the selected tube's count disagrees -- a
    ramp from another tube would be a lie in the widget.
    """
    values = _readPrimaryFillParams(session)
    stage = _stage(session)
    tubes = selectedTubeIds(session)
    if values is None or stage is None or not tubes or tubes[0] == 0:
        return values
    try:
        perTube = stage.fillParams(_ctx(session), tubes[0])
    except RuntimeError:
        return values
    values.update({k: perTube[k]
                   for k in ("density", "cvCount", "seed", "edgeBias")})
    if int(perTube["profileCount"]) != len(values["profile"]):
        values["profile"] = []
    return values


def _readPrimaryFillParams(session):
    """The live Tonic_GetFillParams() dict, or None with no live context."""
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return None
    density = ctypes.c_float(0.0)
    cvCount = ctypes.c_int(0)
    seed = ctypes.c_int(0)
    edgeBias = ctypes.c_float(0.0)
    profile = (ctypes.c_float * MAX_FILL_PROFILE_FLOATS)()
    got = ctypes.c_int(0)
    rc = dll.Tonic_GetFillParams(
        ctx, ctypes.byref(density), ctypes.byref(cvCount),
        ctypes.byref(seed), ctypes.byref(edgeBias), profile,
        MAX_FILL_PROFILE_FLOATS, ctypes.byref(got))
    if int(rc) != 0:
        return None
    return {
        "density": float(density.value), "cvCount": int(cvCount.value),
        "seed": int(seed.value), "edgeBias": float(edgeBias.value),
        "profile": [float(profile[i]) for i in range(got.value)],
    }


def _writeFillParams(session, values):
    """Write the params to every selected tube, then refill the guides.

    Guides that do not follow the density the panel shows are a lie on
    screen, so the write ends in a full-density Tonic_RefillGuides
    (plan/17 section 5.3: preview during a drag, full otherwise). Freeze
    roots is left exactly as the artist set it; the refill keeps the
    frozen prefix by itself.
    """
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    pairs = [float(v) for v in values["profile"]]
    stage = _stage(session)
    tubes = selectedTubeIds(session)
    if stage is None or not tubes:
        arr = (ctypes.c_float * len(pairs))(*pairs) if pairs else None
        dll.Tonic_SetFillParams(ctx, float(values["density"]),
                                int(values["cvCount"]), int(values["seed"]),
                                float(values["edgeBias"]), arr, len(pairs))
    else:
        for tubeId in tubes:
            stage.setFillParams(ctx, tubeId, float(values["density"]),
                                int(values["cvCount"]), int(values["seed"]),
                                float(values["edgeBias"]), pairs)
    dll.Tonic_RefillGuides(ctx, 1.0)


def _fillField(field, cast):
    def get(state, session):
        cache = _fillCache(state)
        live = _readFillParams(session)
        if live is not None:
            cache.update(live)
        return cast(cache[field])

    def set(state, session, value):
        cache = _fillCache(state)
        live = _readFillParams(session)
        if live is not None:
            cache.update(live)
        cache[field] = cast(value)
        _writeFillParams(session, cache)

    return get, set


def _fillProfile():
    def get(state, session):
        cache = _fillCache(state)
        live = _readFillParams(session)
        if live is not None:
            cache.update(live)
        return list(cache["profile"])

    def set(state, session, value):
        pairs = list(tonicFill.validateLengthProfile(value)
                     .reshape(-1).tolist())
        cache = _fillCache(state)
        live = _readFillParams(session)
        if live is not None:
            cache.update(live)
        cache["profile"] = pairs
        _writeFillParams(session, cache)

    return get, set


def _fillDescriptors(state):
    densityGet, densitySet = _fillField("density", float)
    cvCountGet, cvCountSet = _fillField("cvCount", int)
    edgeBiasGet, edgeBiasSet = _fillField("edgeBias", float)
    seedGet, seedSet = _fillField("seed", int)
    profileGet, profileSet = _fillProfile()
    return [
        _descriptor("density", "Density", "float", densityGet, densitySet,
                   min=0.1, max=64.0, step=0.5),
        _descriptor("cvCount", "CV count", "int", cvCountGet, cvCountSet,
                   min=2, max=64, step=1),
        _descriptor("edgeBias", "Edge bias", "float", edgeBiasGet,
                   edgeBiasSet, min=-1.0, max=1.0, step=0.05),
        _descriptor("seed", "Seed", "int", seedGet, seedSet, min=0,
                   max=9999, step=1),
        _descriptor("lengthProfile", "Length profile", "ramp", profileGet,
                   profileSet),
        _scalarDescriptor(
            "previewFraction", "Preview fraction", "float",
            "previewFraction", float, "Tonic_GetPreviewFraction",
            "Tonic_SetPreviewFraction", min=0.0, max=1.0, step=0.05),
        _scalarDescriptor(
            "freezeRoots", "Freeze roots", "bool", "freezeRoots", _boolCast,
            "Tonic_GetFreezeRoots", "Tonic_SetFreezeRoots",
            dllCast=_boolToInt),
    ]


def _refillHandler(container):
    session = getattr(container, "session", None)
    refill = getattr(session, "refillGeneratedCurves", None)
    if callable(refill):
        refill()
        return
    loop = _loop(container, "fill")
    if loop is not None:
        loop.refill(1.0)          # the loop commits and reports as well
        return
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    generate = getattr(dll, "Tonic_GenerateGuides", None)
    if generate is not None:
        generate(ctx, 1.0)
    else:
        dll.Tonic_RefillGuides(ctx, 1.0)
    _publish(session)


def _clearGeneratedCurvesHandler(container):
    session = getattr(container, "session", None)
    clear = getattr(session, "clearGeneratedCurves", None)
    if callable(clear):
        clear()
        return
    report = getattr(session, "report", None)
    if callable(report):
        report("Tonic: clear generated curves is unavailable")


def _fillActions(state):
    return [Action("refill", "Refill guides", "", _refillHandler),
            Action("clearGeneratedCurves", "Clear generated curves", "",
                   _clearGeneratedCurvesHandler)]


# ---- Hierarchy ----------------------------------------------------------

def _hierarchyCache(state):
    return state.panels.setdefault("hierarchy", {"subdivideSeed": 0})


def _subdivideSeedGet(state, session):
    return int(_hierarchyCache(state)["subdivideSeed"])


def _subdivideSeedSet(state, session, value):
    _hierarchyCache(state)["subdivideSeed"] = int(value)


def _lockParentsGet(state, session):
    return bool(tonicHierarchy.effectiveLockParents(state))


def _lockParentsSet(state, session, value):
    tonicHierarchy.setLockParents(state, value)
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        for tubeId in selectedTubeIds(session):
            tonicHierarchy.setLockParentsEntry(dll, ctx, tubeId, value)


def _lockChildrenGet(state, session):
    return bool(tonicHierarchy.effectiveLockChildren(state))


def _lockChildrenSet(state, session, value):
    tonicHierarchy.setLockChildren(state, value)
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        for tubeId in selectedTubeIds(session):
            tonicHierarchy.setLockChildrenEntry(dll, ctx, tubeId, value)


def _levelOrOff(rawLevel):
    """0 (the spinbox floor) <-> the -1 SOLO_OFF/SHOW_ALL_LEVELS sentinel.

    tonicHierarchy.validateLevel rejects 0 outright (levels start at 1), so
    a contiguous 0..8 spinbox range needs 0 translated to -1 at the state
    boundary rather than exposing the -1..8 range with its unusable 0 gap.
    """
    return -1 if int(rawLevel) <= 0 else int(rawLevel)


def _soloLevelGet(state, session):
    return max(int(state.soloLevel), 0)


def _soloLevelSet(state, session, value):
    tonicHierarchy.setSoloLevel(state, _levelOrOff(value))
    _syncLevels(state, session)


def _showMaxLevelGet(state, session):
    return max(int(state.showMaxLevel), 0)


def _showMaxLevelSet(state, session, value):
    tonicHierarchy.setShowMaxLevel(state, _levelOrOff(value))
    _syncLevels(state, session)


def _syncLevels(state, session):
    """Turn Solo / Show<=n / hidden into Tonic_SetLevelDisplay calls.

    The toggles were Python-only state (audit section 5.4), so a Solo that
    the panel recorded never reached the viewport. The maths lives in
    tonicHierarchy.levelDrawStyle; this walks the levels the model holds
    and pushes the answer for each one.
    """
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    focus = int(state.activeLevel)
    xrayMap = state.panels.setdefault("xray", {})
    levels = (set(_modelLevels(session)) | {focus} | set(xrayMap) |
              set(state.hiddenLevels))
    for level in sorted(level for level in levels if level >= 1):
        style = tonicHierarchy.levelDrawStyle(
            level, focus, state.soloLevel, state.showMaxLevel,
            state.hiddenLevels)
        xray = bool(xrayMap.get(level, style == "x-ray"))
        dll.Tonic_SetLevelDisplay(ctx, int(level),
                                  _boolToInt(style != "hidden"),
                                  _boolToInt(xray))
    _publish(session)


def _modelLevels(session):
    """The levels the model's tubes sit at, ascending ([] when headless)."""
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return []
    count = ctypes.c_int(0)
    if int(dll.Tonic_ReadTubeIds(ctx, None, 0, ctypes.byref(count))) != 0:
        return []
    n = min(max(int(count.value), 0), MAX_SELECTION)
    if n <= 0:
        return []
    ids = (ctypes.c_int * n)()
    got = ctypes.c_int(0)
    if int(dll.Tonic_ReadTubeIds(ctx, ids, n, ctypes.byref(got))) != 0:
        return []
    levels = {int(dll.Tonic_GetTubeLevel(ctx, ids[i]))
              for i in range(min(n, int(got.value)))}
    return sorted(level for level in levels if level >= 1)


def _levelDisplay(session, level):
    """(visible, xray) for `level`, from the model when live else state."""
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        visible = ctypes.c_int(1)
        xray = ctypes.c_int(0)
        dll.Tonic_GetLevelDisplay(ctx, int(level), ctypes.byref(visible),
                                  ctypes.byref(xray))
        return bool(visible.value), bool(xray.value)
    return None, None


def _levelVisibleGet(state, session):
    live, _ = _levelDisplay(session, state.activeLevel)
    if live is not None:
        return live
    return int(state.activeLevel) not in state.hiddenLevels


def _levelVisibleSet(state, session, value):
    level = int(state.activeLevel)
    if value:
        tonicHierarchy.showLevel(state, level)
    else:
        tonicHierarchy.hideLevel(state, level)
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        _, xray = _levelDisplay(session, level)
        dll.Tonic_SetLevelDisplay(ctx, level, _boolToInt(value),
                                  _boolToInt(bool(xray)))


def _levelXrayGet(state, session):
    _, live = _levelDisplay(session, state.activeLevel)
    if live is not None:
        return live
    return bool(state.panels.setdefault("xray", {})
               .get(int(state.activeLevel), False))


def _levelXraySet(state, session, value):
    level = int(state.activeLevel)
    state.panels.setdefault("xray", {})[level] = bool(value)
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        visible, _ = _levelDisplay(session, level)
        if visible is None:
            visible = level not in state.hiddenLevels
        dll.Tonic_SetLevelDisplay(ctx, level, _boolToInt(visible),
                                  _boolToInt(value))


def _hierarchyDescriptors(state):
    level = int(state.activeLevel)
    return [
        _scalarDescriptor(
            "subdivideCount", "Subdivide count", "int", "subdivideCount",
            int, min=tonicHierarchy.SUBDIVIDE_MIN,
            max=tonicHierarchy.SUBDIVIDE_MAX, step=1),
        _scalarDescriptor(
            "splitMode", "Split mode", "enum", "splitMode", str,
            choices=tonicHierarchy.SPLIT_MODES),
        _descriptor("subdivideSeed", "Subdivide seed", "int",
                   _subdivideSeedGet, _subdivideSeedSet, min=0, max=9999,
                   step=1),
        _descriptor("lockParents", "Lock parents", "bool", _lockParentsGet,
                   _lockParentsSet),
        _descriptor("lockChildren", "Lock children", "bool",
                   _lockChildrenGet, _lockChildrenSet),
        _descriptor("soloLevel", "Solo level (0 = off)", "int",
                   _soloLevelGet, _soloLevelSet, min=0, max=8, step=1),
        _descriptor("showMaxLevel", "Show <= level (0 = all)", "int",
                   _showMaxLevelGet, _showMaxLevelSet, min=0, max=8,
                   step=1),
        _descriptor("levelVisible", "L%d visible" % level, "bool",
                   _levelVisibleGet, _levelVisibleSet),
        _descriptor("levelXray", "L%d x-ray" % level, "bool",
                   _levelXrayGet, _levelXraySet),
    ]


def _subdivideHandler(container):
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.subdivideSelection()
        return
    session = getattr(container, "session", None)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    state = container.tonicState
    seed = _subdivideSeedGet(state, session)
    for tubeId in selectedTubeIds(session):
        tonicHierarchy.subdivide(dll, ctx, tubeId, state.subdivideCount,
                                 state.splitMode, seed)
    _publish(session)


def _mergeChildrenHandler(container):
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.mergeChildrenOfSelection()
        return
    session = getattr(container, "session", None)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    for tubeId in selectedTubeIds(session):
        tonicHierarchy.mergeChildren(dll, ctx, tubeId)
    _publish(session)


def _mergeSelectedHandler(container):
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.mergeSelected()
        return
    session = getattr(container, "session", None)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    ids = selectedTubeIds(session)
    if ids:
        tonicHierarchy.mergeSelected(dll, ctx, ids)
    _publish(session)


def _resubdivideHandler(container):
    """Merge and split again; the loop confirms on the status line.

    Only the loop has the two-step confirm (plan/18 section 6 forbids a
    modal), so with no viewport up this button does nothing rather than
    silently discarding the children's sculpt deltas.
    """
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.resubdivide()


def _groupHandler(container):
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.group(transient=True)
        return
    session = getattr(container, "session", None)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    ids = selectedTubeIds(session)
    if ids:
        tonicHierarchy.groupTubes(dll, ctx, ids, transient=True)
    _publish(session)


def _makePersistentHandler(container):
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.makePersistent()
        return
    session = getattr(container, "session", None)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    for tubeId in selectedTubeIds(session):
        tonicHierarchy.makePersistent(dll, ctx, tubeId)
    _publish(session)


def _enterLevelHandler(container):
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.enterLevel()


def _exitLevelHandler(container):
    loop = _loop(container, "hierarchy")
    if loop is not None:
        loop.exitLevel()


def _hierarchyActions(state):
    return [
        Action("subdivide", "Subdivide", "Shift+D", _subdivideHandler),
        Action("mergeChildren", "Merge children", "Shift+M",
              _mergeChildrenHandler),
        Action("mergeSelected", "Merge selected", "", _mergeSelectedHandler),
        Action("resubdivide", "Re-subdivide", "", _resubdivideHandler),
        Action("group", "Group", "", _groupHandler),
        Action("makePersistent", "Make persistent", "",
              _makePersistentHandler),
        Action("enterLevel", "Enter level", "Ctrl+Down",
              _enterLevelHandler),
        Action("exitLevel", "Exit level", "Ctrl+Up", _exitLevelHandler),
    ]


# ---- Sculpt -------------------------------------------------------------

def _sculptDescriptors(state):
    return [
        _scalarDescriptor(
            "brushRadiusPx", "Brush radius (px)", "float", "brushRadiusPx",
            float, min=2.0, max=200.0, step=1.0),
        _scalarDescriptor(
            "brushTRadius", "Brush t radius (0 = unbounded)", "float",
            "brushTRadius", float, min=0.0, max=1.0, step=0.01),
        _scalarDescriptor(
            "sculptPreserveLength", "Preserve length", "bool",
            "sculptPreserveLength", _boolCast),
        _scalarDescriptor(
            "sculptStrength", "Brush strength", "float", "sculptStrength",
            float, min=0.0, max=4.0, step=0.05),
        _scalarDescriptor(
            "sculptMirrorX", "Mirror X", "bool", "sculptMirrorX",
            _boolCast),
    ]



# ---- Output ---------------------------------------------------------------

# The per-face bake resolutions the Output panel offers, in texels per
# side. The bake stores the resolution as an exponent (plan/17 section
# 4.5), so the choices are powers of two and nothing in between; "auto" is
# the bake's own plan, which sizes each face by its area and collapses a
# face that lies inside one region to a single texel.
# Every power of two the session will apply is on this list, so what
# the bake took is always a choice the combo box can show. The
# session's floor and ceiling (4 and 4096 texels per side) keep that
# true.
TEXEL_CHOICES = ("auto", "4", "8", "16", "32", "64", "128", "256",
                 "512", "1024", "2048", "4096")


def _texelResolutionGet(state, session):
    """The resolution in force, preferring what the bake actually took."""
    panel = state.panels.setdefault("output", {})
    applied = getattr(session, "bakeTexelResolution", None) \
        if session is not None else None
    if applied is not None:
        panel["texelResolution"] = (str(int(applied)) if int(applied) > 0
                                    else "auto")
        return panel["texelResolution"]
    # No session yet: the panel is showing a preference, and an
    # unrecognised one came from somewhere that is not this widget.
    stored = panel.get("texelResolution", "auto")
    return stored if stored in TEXEL_CHOICES else "auto"


def _texelResolutionSet(state, session, value):
    """Apply the override to the bake worker and rebake at once.

    Waiting for the next graph edit would leave the map on disk at the old
    resolution while the panel claimed the new one.
    """
    text = str(value)
    texels = int(text) if text.isdigit() else 0
    setter = getattr(session, "setBakeTexelResolution", None) \
        if session is not None else None
    if setter is None:
        state.panels.setdefault("output", {})["texelResolution"] = (
            text if text in TEXEL_CHOICES else "auto")
        return
    applied = int(setter(texels))
    state.panels.setdefault("output", {})["texelResolution"] = (
        str(applied) if applied > 0 else "auto")
    rebake = getattr(session, "rebake", None)
    if rebake is not None:
        rebake()


def _outputSettings(state, session):
    getter = getattr(session, "outputSettings", None) \
        if session is not None else None
    if callable(getter):
        enabled, multiplier, width = getter()
        state.outputEnabled = bool(enabled)
        state.outputDensityMultiplier = float(multiplier)
        state.outputStrandWidth = float(width)
    return (bool(getattr(state, "outputEnabled", False)),
            float(getattr(state, "outputDensityMultiplier", 1.0)),
            float(getattr(state, "outputStrandWidth", 0.01)))


def _outputDensityGet(state, session):
    return _outputSettings(state, session)[1]


def _outputDensitySet(state, session, value):
    value = max(float(value), 1e-6)
    setter = getattr(session, "setOutputSettings", None) \
        if session is not None else None
    if callable(setter):
        if not setter(densityMultiplier=value):
            return
    state.outputDensityMultiplier = value


def _outputWidthGet(state, session):
    return _outputSettings(state, session)[2]


def _outputWidthSet(state, session, value):
    value = max(float(value), 0.0)
    setter = getattr(session, "setOutputSettings", None) \
        if session is not None else None
    if callable(setter):
        if not setter(strandWidth=value):
            return
    state.outputStrandWidth = value


def _buildDescriptionHandler(container):
    session = getattr(container, "session", None)
    build = getattr(session, "buildOutputDescription", None) \
        if session is not None else None
    if callable(build):
        build()
        return
    report = getattr(session, "report", None) if session is not None else None
    if callable(report):
        report("Tonic: Output description is unavailable")


def _outputActions(_state):
    return [Action("buildDescription", "Build/update description", "",
                   _buildDescriptionHandler)]


def _outputDescriptors(state):
    descriptors = [
        _descriptor("texelResolution",
                   "Ptex texels per face side", "enum",
                   _texelResolutionGet, _texelResolutionSet,
                   choices=TEXEL_CHOICES),
        _scalarDescriptor(
            "showAmplifiedHair", "Show amplified hair", "bool",
            "showAmplifiedHair", _boolCast),
    ]
    if bool(getattr(state, "outputEnabled", False)):
        descriptors[1:1] = [
            _descriptor("outputDensityMultiplier", "Description density",
                        "float", _outputDensityGet, _outputDensitySet,
                        min=0.01, max=100.0, step=0.1),
            _descriptor("outputStrandWidth", "Strand width", "float",
                        _outputWidthGet, _outputWidthSet,
                        min=0.0, max=1.0, step=0.001),
        ]
    return descriptors


# ---- Dispatch ---------------------------------------------------------

_DESCRIPTOR_BUILDERS = {
    "graph": _graphDescriptors,
    "tube": _tubeDescriptors,
    "fill": _fillDescriptors,
    "hierarchy": _hierarchyDescriptors,
    "sculpt": _sculptDescriptors,
    "output": _outputDescriptors,
}

_ACTION_BUILDERS = {
    "graph": _graphActions,
    "tube": _tubeActions,
    "fill": _fillActions,
    "hierarchy": _hierarchyActions,
    "output": _outputActions,
    # Sculpt has no one-shot action: every sculpt edit is a stroke, and
    # the brush ring leaves with the mode (SculptLoop.deactivate).
}


def descriptors(modeId, state):
    """The ordered parameter descriptors for `modeId`'s panel.

    Unknown mode ids return an empty list, so a stale mode never crashes
    the generated form.
    """
    builder = _DESCRIPTOR_BUILDERS.get(modeId)
    return builder(state) if builder is not None else []


def actions(modeId):
    """The one-shot action buttons for `modeId` (plan/18 section 3.5 item
    4). Save/Export/Import are not here: they need a file dialog, which is
    Qt, so tonicWorkspace.py wires those three directly against
    `container.session`."""
    builder = _ACTION_BUILDERS.get(modeId)
    return builder(None) if builder is not None else []
