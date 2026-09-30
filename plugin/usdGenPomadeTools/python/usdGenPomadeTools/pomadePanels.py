# usdGenPomadeTools.pomadePanels -- Qt-free per-mode parameter/action
# descriptors for the Pomade workspace dock (plan/18 section 3.5 items 3-4;
# plan/08 section 5.2: panels are generated, never hand-written).
#
# descriptors(modeId, state) returns the ordered parameter rows for one
# mode's QFormLayout; actions(modeId) returns its one-shot buttons. Both are
# pure data plus closures: no Qt, no pxr, so pomadeWorkspace.py is the only
# module that turns them into widgets, and testUsdGenPomadeToolsPanels.py
# exercises every closure against a fake session.
#
# A descriptor's get(state, dll) / set(state, dll, value) take a second
# argument shaped like PomadeSession (plan/18 section 3.1): `.dll` is the raw
# ctypes handle (pomadeLib.Library.dll) and `.ctx` is the PomadeModelContext*
# the C ABI calls need (None before Bind scalp). Every get/set degrades to
# reading and writing `state` alone when `.dll`/`.ctx` are unavailable, so
# the panel is inert but harmless with no scalp bound -- exactly the state
# testUsdGenPomadeToolsPanels.py exercises with a fake session and the state
# pomadeWorkspace.refresh() sees for real once `container.session` exists.
# An action's handler(container) gets the whole container so it can reach
# both `container.pomadeState` (the params just edited) and
# `container.session` (dll, ctx, publish()).
#
# Sibling helper modules are imported with a package/file-path fallback (the
# same trick testUsdGenPomadeToolsHierarchy.py's _load() needs on the far
# side): a real usdview load resolves them as package members, a file-path
# test load pre-registers their bare names in sys.modules.
from __future__ import annotations

import collections
import ctypes

try:
    from . import pomadeFill, pomadeHierarchy, pomadeSculpt, pomadeTube
except ImportError:  # file-path test load
    import pomadeFill
    import pomadeHierarchy
    import pomadeSculpt
    import pomadeTube

# The artist-facing half of a row (DK-07): `tooltip` says what the row does
# in the artist's words, `unit` is the spin box suffix ('px', '%'),
# `choiceLabels` are the combo texts for an enum's `choices` (same length,
# same order: the choice stays the stored value, the label is only shown),
# and `enabled` is False for a row that exists but cannot be used yet (the
# Output rows before Build). They default so a positional nine-field
# constructor still works; tooltip and enabled may change with state, so
# the dock re-reads them on refresh and they are not part of a page's
# signature.
Descriptor = collections.namedtuple(
    "Descriptor",
    ("id", "label", "kind", "min", "max", "step", "choices", "get", "set",
     "tooltip", "unit", "choiceLabels", "enabled"),
    defaults=("", "", (), True))

Action = collections.namedtuple(
    "Action", ("id", "label", "hotkeyLabel", "handler", "tooltip"),
    defaults=("",))

# Icon glyph per Action.id / dock command id / status pill (IC-01,
# plugin/usdGenPomadeTools/resources/icons/README.md). pomadeIcons.loadIcon
# resolves the PNG; kept here rather than on Action so the table stays
# reachable without constructing every mode's actions() list first. A
# sub-mode action that reuses its sub-mode glyph (Subdivide, Merge
# children, Group) is not repeated here -- see pomadeModes.ICONS instead.
ACTION_ICONS = {
    "weldAll": "act_weld_all",
    "rebake": "act_rebake",
    "matchSurface": "act_match_surface",
    "relax": "act_relax",
    "snapRoot": "act_snap_root",
    # Reset transform tool (parity G20): the settings glyph, as the
    # RigExec Tool Settings window's Reset Tool sits under its Settings.
    "resetTransformTool": "act_settings",
    "refill": "act_refill",
    "clearGeneratedCurves": "act_clear_generated",
    "mergeSelected": "act_merge_selected",
    "resubdivide": "act_resubdivide",
    "makePersistent": "act_make_persistent",
    "enterLevel": "act_enter_level",
    "exitLevel": "act_exit_level",
    "buildDescription": "act_build_description",

    "saveGroom": "act_save",
    "exportCenterCurves": "act_export",
    "importCurves": "act_import",
    "bindScalp": "act_bind_scalp",
    "resumeGroom": "act_resume_groom",
    "undo": "act_undo",
    "redo": "act_redo",
    "deselectAll": "act_deselect_all",
    "frameSelected": "act_frame_selected",
    "settings": "act_settings",

    "showGeneratedCurves": "toggle_show_curves",
    "showAmplifiedHair": "toggle_show_hair",

    "statusSynced": "status_synced",
    "statusPending": "status_pending",
    "statusDetached": "status_detached",
    "statusWarning": "status_warning",
    "statusError": "status_error",
    "statusInfo": "status_info",
    "statusLadder": "status_ladder",

    "pivotCentre": "pivot_centre",
    "pivotEach": "pivot_each",
}

# The maximum length-profile floats a Fill descriptor round-trips in one
# Pomade_GetFillParams call (32 (position, value) knots is generous for a
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
    "How finely the region map is baked, in texels per scalp face side. "
    "Auto uses 64 x 64 on faces a region border crosses and one texel "
    "inside a region; higher values follow finer region borders. Guide "
    "density is set in Fill."
)

# Bounded float rows the dock pairs with a slider (DK-06). A slider drag
# is one gesture (one undo step), so only rows whose whole range is worth
# scrubbing are listed; the dock maps a range spanning two decades or more
# logarithmically so the low end is reachable.
SLIDER_PARAM_IDS = frozenset((
    "density", "sculptStrength", "brushRadiusPx", "previewFraction",
    "edgeBias", "softRadius"))

# The slider rows whose drag is bracketed in one gesture (one undo step).
# Only Fill's density and edge bias write state the undo snapshot restores
# (the tube's fill params and its refilled guides); brush radius and
# strength are tool settings and preview fraction / soft radius are model
# display values, so a bracket around those would push an undo step that
# changes nothing.
SLIDER_GESTURE_IDS = frozenset(("density", "edgeBias"))

# Fill density's unit (DK-06): guides per unit of scalp area; the bare
# number read like a count. Descriptor.unit carries it to the spin box.
DENSITY_UNIT = "/unit²"

# The tooltip of a row that exists before it can be used: Output's
# density and width rows are shown greyed until the first Build (DK-07),
# rather than appearing from nowhere after it.
BUILD_FIRST_TIP = "Build the hair description first (Build hair description)."

# What the length-profile field accepts; shown as its tooltip when a typo
# is refused instead of raised (DK-06).
RAMP_HINT = "pos:val pairs, e.g. 0:1, 0.5:0.6, 1:0.8"
# The empty field's placeholder: an empty profile is every guide at full
# length, as the row's tooltip and the manual say -- not "uniform" at
# whatever length was last set.
RAMP_PLACEHOLDER = "pos:val, pos:val ... (empty = full length)"


def guideCount(session):
    """The live guide count (Pomade_GetGuideCounts), or None when unbound."""
    dll, ctx = _dll(session), _ctx(session)
    entry = (getattr(dll, "Pomade_GetGuideCounts", None)
             if dll is not None and ctx is not None else None)
    if entry is None:
        return None
    guides = ctypes.c_int(0)
    if int(entry(ctx, ctypes.byref(guides), None)) != 0:
        return None
    return int(guides.value)


def guideCountText(session):
    """The density row's readout: what the density produced ('' unbound)."""
    count = guideCount(session)
    if count is None:
        return ""
    return "%d guide%s" % (count, "" if count == 1 else "s")


def _descriptor(id, label, kind, get, set, min=0.0, max=0.0, step=0.0,
                choices=(), tooltip="", unit="", choiceLabels=None,
                enabled=True):
    choices = tuple(choices)
    # An enum always has one shown label per choice; a builder that names
    # none gets the choice's own text rather than a short list.
    labels = (tuple(str(c) for c in choices) if choiceLabels is None
              else tuple(str(c) for c in choiceLabels))
    if len(labels) != len(choices):
        raise ValueError("pomadePanels: %s has %d choice labels for %d "
                         "choices" % (id, len(labels), len(choices)))
    return Descriptor(id, label, kind, min, max, step, choices, get, set,
                      str(tooltip), str(unit), labels, bool(enabled))


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


def _undoStep(session, label, write, snapshot):
    """Run one dock edit as exactly one undo step (SS-02).

    A panel write used to be no undo step at all, so Ctrl+Z after a
    density change skipped it and restored an older snapshot -- one taken
    before the guides were grown. Bracketing the write in a gesture makes
    the edit its own step; `snapshot()` is read before and after so a
    write of the value already held leaves no empty step behind
    (endGestureIfChanged cancels it); a snapshot of None means "cannot
    tell" and always keeps the step. Inside an open gesture -- the dock's
    slider drag brackets itself -- the write joins that step instead, and
    a session without the bracket (a test's fake) just writes.
    """
    begin = getattr(session, "beginGesture", None) \
        if session is not None else None
    if (not callable(begin) or _ctx(session) is None or
            getattr(session, "gestureActive", False) is True):
        return write()
    before = snapshot()
    if not begin(label):
        return write()
    try:
        result = write()
    except Exception:
        cancel = getattr(session, "cancelGesture", None)
        dirty = cancel() if callable(cancel) else 0
        if dirty and hasattr(session, "publish"):
            session.publish(dirty)
        raise
    changed = before is None or snapshot() != before
    finish = getattr(session, "endGestureIfChanged", None)
    if callable(finish):
        finish(changed)
    else:
        session.endGesture()
    return result


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
        for kind in (pomadeTube.PICK_TUBE_VERT,
                     pomadeTube.PICK_CENTER_CV,
                     pomadeTube.PICK_SECTION_CV,
                     pomadeTube.PICK_SECTION_RING):
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
    for kind in (pomadeTube.PICK_TUBE_VERT,
                 pomadeTube.PICK_CENTER_CV,
                 pomadeTube.PICK_SECTION_CV,
                 pomadeTube.PICK_SECTION_RING):
        outIds = (ctypes.c_int * MAX_SELECTION)()
        outSub = (ctypes.c_int * MAX_SELECTION)()
        outSubSub = (ctypes.c_int * MAX_SELECTION)()
        got = ctypes.c_int(0)
        rc = dll.Pomade_ReadSelection(ctx, kind, outIds, outSub, outSubSub,
                                     MAX_SELECTION, ctypes.byref(got))
        if int(rc) != 0:
            continue
        for i in range(got.value):
            seen.add(int(outIds[i]))
    return sorted(seen)


# ---- generic scalar descriptor (state field, optionally ABI-backed) ------

def _scalarDescriptor(id, label, kind, stateAttr, cast, dllGetName=None,
                      dllSetName=None, dllCast=None, **limits):
    """A descriptor over one PomadeToolState field, mirrored to the model
    through a same-shaped Pomade_Get<X>(ctx) / Pomade_Set<X>(ctx, value) pair
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
        # State only: the row is screen pixels, the model's
        # Pomade_Get/SetSnapRadius is rest units. The loops convert at the
        # point of use (pomadeGraph.snapRadiusRest); mirroring the row into
        # the model would hand it pixels as world units, and every dock
        # refresh would read the world radius back into the pixel field.
        _scalarDescriptor(
            "snapRadiusPx", "Snap radius", "float", "snapRadiusPx",
            float, min=1.0, max=64.0, step=1.0, unit="px",
            tooltip="How close, in screen pixels, a click or stroke end "
                    "has to land to weld onto an existing CV or edge."),
        _scalarDescriptor(
            "mirrorX", "Mirror X", "bool", "mirrorX", _boolCast,
            "Pomade_GetMirrorX", "Pomade_SetMirrorX", dllCast=_boolToInt,
            tooltip="Repeat every region edit on the other side of the "
                    "scalp (mirrored across X = 0)."),
        _texelResolutionDescriptor(),
    ]


def _viewportCamera(container):
    """The viewport's camera as it is now, or None without a viewport."""
    viewport = getattr(container, "viewport", None)
    view = getattr(viewport, "view", None)
    if view is not None:
        from . import pomadeCamera
        camera = pomadeCamera.resolve(view)
        if camera is not None:
            return camera
    return getattr(viewport, "camera", None)


def _weldAllHandler(container):
    session = getattr(container, "session", None)
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    camera = _viewportCamera(container)
    viewport = getattr(container, "viewport", None)
    loop = getattr(viewport, "loop", None)
    if getattr(loop, "modeId", None) == "graph" and \
            callable(getattr(loop, "weldAll", None)):
        # The Graph loop welds as one undo step and runs K3, the stubs
        # and the commit after it, like any other Graph edit.
        loop.weldAll(camera)
        getattr(viewport, "scheduleIdle", lambda: None)()
        return
    # The Snap radius row is screen pixels; the ABI wants rest units.
    try:
        from . import pomadeGraph
    except ImportError:  # file-path test load
        import pomadeGraph
    radius = pomadeGraph.snapRadiusRest(session, container.pomadeState, camera)
    welds = ctypes.c_int(0)
    dll.Pomade_GraphWeldAll(ctx, float(radius), ctypes.byref(welds))
    _publish(session)


def _rebakeHandler(container):
    session = getattr(container, "session", None)
    if session is not None and hasattr(session, "rebake"):
        session.rebake()


def _graphActions(state):
    return [
        Action("weldAll", "Weld all within radius", "", _weldAllHandler,
               "Merge every pair of region CVs closer than the snap "
               "radius."),
        Action("rebake", "Rebake map now", "", _rebakeHandler,
               "Bake the region map again now instead of after the next "
               "region edit."),
    ]


# ---- Tube -------------------------------------------------------------

def _softRadiusGet(state, session):
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        center = ctypes.c_float(0.0)
        radius = ctypes.c_float(0.0)
        dll.Pomade_GetSoftSelection(ctx, ctypes.byref(center),
                                   ctypes.byref(radius))
        state.softCenter = float(center.value)
        state.softRadius = float(radius.value)
    return float(state.softRadius)


def _softRadiusSet(state, session, value):
    state.softRadius = float(value)
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        dll.Pomade_SetSoftSelection(ctx, float(state.softCenter),
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
    value = value if value in ("select", "move", "rotate", "scale") \
        else "move"
    # Parity G16: each tool keeps its own Axis Orientation (Rotate starts
    # in Tube), so a tool change swaps the live orientation too.
    _gizmoSettingsModule().SwitchTool(state, value)
    state.transformTool = value


# ---- Tube gizmo settings (GZ-05, parity G20) ------------------------------
#
# The vendored RigExec Tool Settings panel in the Pomade idiom: extra Tube
# rows over pomadeGizmoSettings.  A write notifies the GizmoSettings
# listeners, which is how the viewport controller re-places the gizmo
# without the dock knowing it exists.

def _gizmoSettingsModule():
    try:
        from . import pomadeGizmoSettings
    except ImportError:  # file-path test load
        import pomadeGizmoSettings
    return pomadeGizmoSettings


def _gizmoSettings(state):
    return _gizmoSettingsModule().settingsFor(state)


def _toolSettings(state, tool=None):
    return _gizmoSettings(state).For(
        tool or getattr(state, "transformTool", "move"))


def _orientationGet(state, _session):
    return _gizmoSettingsModule().NormalizeOrientation(
        getattr(state, "transformOrientation", "world"))


def _orientationSet(state, _session, value):
    value = _gizmoSettingsModule().NormalizeOrientation(value)
    if value == getattr(state, "transformOrientation", None):
        return
    state.transformOrientation = value
    # The live tool's orientation lives on the state (SwitchTool banks the
    # others), not in a ToolSettings, so announce it the way a
    # ToolSettings write would.
    _gizmoSettings(state).Notify()


def _toolFieldDescriptor(id, label, kind, field, tool=None, cast=None,
                         **ranges):
    """A row over one ToolSettings field: `tool`'s, or the live tool's."""
    convert = cast or (bool if kind == "bool" else float)

    def get(state, _session):
        return convert(getattr(_toolSettings(state, tool), field))

    def set(state, _session, value):
        setattr(_toolSettings(state, tool), field, convert(value))

    return _descriptor(id, label, kind, get, set, **ranges)


def _globalFieldDescriptor(id, label, field, **ranges):
    """A float row over one session-wide GizmoSettings field."""
    def get(state, _session):
        return float(getattr(_gizmoSettings(state), field))

    def set(state, _session, value):
        setattr(_gizmoSettings(state), field, float(value))

    return _descriptor(id, label, "float", get, set, **ranges)


def _resetTransformToolHandler(container):
    """Reset transform tool: the live tool's step/rotate/scale defaults,
    its group pivot and its Axis Orientation (parity G16/G17)."""
    state = getattr(container, "pomadeState", None)
    if state is None:
        return
    tool = getattr(state, "transformTool", "move")
    settings = _gizmoSettings(state)
    orientation = _gizmoSettingsModule().DefaultOrientation(tool)
    moved = getattr(state, "transformOrientation", None) != orientation
    state.transformOrientation = orientation
    # Reset notifies when a field changed; the orientation is on the state,
    # so a change there alone still has to tell the gizmo listeners.
    if not settings.Reset(tool) and moved:
        settings.Notify()


def _tubeCache(state):
    return state.panels.setdefault("tube", {"ringCvCount": pomadeTube.
                                            DEFAULT_REGION_RING_VERTS})


def _ringCvCountGet(state, session):
    cache = _tubeCache(state)
    value = pomadeTube.regionRingVerts(cache.get(
        "ringCvCount", pomadeTube.DEFAULT_REGION_RING_VERTS))
    cache["ringCvCount"] = value
    return value


def _ringCvCountSet(state, session, value):
    _tubeCache(state)["ringCvCount"] = pomadeTube.regionRingVerts(value)


def _selectedSectionRings(session):
    if session is None or not hasattr(session, "readSelection"):
        return []
    try:
        rings = list(session.readSelection(pomadeTube.PICK_SECTION_RING))
        if rings:
            return rings
        # A section-CV selection still identifies its owning ring, so the
        # same absolute scale control remains useful in Section CVs mode.
        cvs = list(session.readSelection(pomadeTube.PICK_SECTION_CV))
        return [(tubeId, ring, -1) for tubeId, ring, _slot in cvs]
    except (AttributeError, TypeError, RuntimeError):
        return []


def _readSectionScale(session, tubeId, ring):
    """Read one ring scale through the optional bridge helper."""
    if session is None or getattr(session, "model", None) is None:
        return None
    try:
        try:
            from . import pomadeBridge
        except ImportError:
            import pomadeBridge
        section = pomadeBridge.tubeSection(session.dll, session.model,
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
                from . import pomadeLoopsTube
            except ImportError:
                import pomadeLoopsTube
            pomadeLoopsTube.restoreGuides(
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
                report("Pomade Tube: section changed; guide refill unavailable")
    else:
        cancel = getattr(session, "cancelGesture", None)
        dirty = cancel() if callable(cancel) else 0
        publish = getattr(session, "publish", None)
        if callable(publish):
            publish(dirty)
        report = getattr(session, "report", None)
        if callable(report):
            report("Pomade Tube: section scale failed; edit cancelled")


# The section-scale row is only live with section rings (or section CVs)
# selected; the dock swaps these two tooltips as it greys the row.
UNIFORM_SCALE_TIP = ("Scale the selected section rings to this size "
                     "(1 = as built).")
UNIFORM_SCALE_EMPTY_TIP = ("Scale selected section rings. Select rings or "
                           "section CVs first (F10 / F11).")


def _tubeDescriptors(state):
    return [
        _descriptor("selectionShape", "Selection shape", "enum",
                   _selectionShapeGet, _selectionShapeSet,
                   choices=("box", "lasso"), choiceLabels=("Box", "Lasso"),
                   tooltip="What a drag on empty space draws to select: a "
                           "rectangle or a free-hand loop."),
        _descriptor("transformTool", "Transform", "enum",
                   _transformToolGet, _transformToolSet,
                   choices=("move", "rotate", "scale", "select"),
                   choiceLabels=("Move (W)", "Rotate (E)", "Scale (R)",
                                 "Select (Q)"),
                   tooltip="The handle the selection gets: move, rotate, "
                           "scale, or none (select only)."),
        # GZ-05 / parity G16, G20, G23, G08, G11: the gizmo's settings.
        # Step snap / step size follow the live transform tool, as the
        # RigExec panel is rebuilt per tool; the Rotate- and Scale-only
        # options name their tool.
        _descriptor("transformOrientation", "Axis orientation (L)", "enum",
                   _orientationGet, _orientationSet,
                   choices=_gizmoSettingsModule().ORIENTATIONS,
                   choiceLabels=tuple(
                       _gizmoSettingsModule().OrientationLabel(o)
                       for o in _gizmoSettingsModule().ORIENTATIONS),
                   tooltip="Which way the live tool's handle axes point: "
                           "the scene axes, the view, or the tube's own "
                           "root direction. Each tool keeps its own (Move "
                           "and Scale start World, Rotate Tube). L flips "
                           "World <-> Tube."),
        _toolFieldDescriptor("stepSnap", "Step snap (hold J)", "bool",
                             "stepSnap",
                             tooltip="Move, turn or scale in whole steps "
                                     "of the step size. Holding J does "
                                     "the same for one drag."),
        _toolFieldDescriptor("stepSize", "Step size", "float", "stepSize",
                             min=0.001, max=360.0, step=0.5,
                             tooltip="The step Step snap uses: scene "
                                     "units for Move, degrees for Rotate, "
                                     "a factor for Scale."),
        _toolFieldDescriptor("freeRotate", "Free rotate ball (Rotate)",
                             "bool", "freeRotate", tool="rotate",
                             tooltip="Show the Rotate handle's inner ball, "
                                     "which turns freely about any axis."),
        _toolFieldDescriptor("preventNegativeScale",
                             "Prevent negative scale (Scale)", "bool",
                             "preventNegativeScale", tool="scale",
                             tooltip="Stop a Scale drag at zero instead "
                                     "of flipping the selection inside "
                                     "out."),
        _globalFieldDescriptor("gridSize", "Grid size (hold X)", "gridSize",
                               min=0.001, max=1000.0, step=0.1,
                               tooltip="The spacing a Move snaps to while "
                                       "X is held, in scene units."),
        _globalFieldDescriptor("manipulatorSize",
                               "Manipulator size (+ / -)",
                               "manipulatorSize", min=20.0, max=400.0,
                               step=10.0, unit="px",
                               tooltip="How big the transform handle "
                                       "draws on screen. + and - resize "
                                       "it."),
        _descriptor("softRadius", "Soft selection falloff", "float",
                   _softRadiusGet, _softRadiusSet, min=0.0, max=1.0,
                   step=0.01,
                   tooltip="How far along the strand (0 = root end, 1 = "
                           "whole length) a move fades out around the "
                           "selected CVs. Off at the left end: only the "
                           "selection moves."),
        _scalarDescriptor(
            "displaySegments", "Curve smoothness (display)", "int",
            "displaySegments", int, "Pomade_GetDisplaySegments",
            "Pomade_SetDisplaySegments", min=1, max=8, step=1,
            tooltip="Drawn spans between each pair of section rings. "
                    "4, the default, interpolates the chord between those "
                    "rings and holds a hard edge at each authored section. "
                    "Display only: the saved groom does not change, and the "
                    "selectable rings stay on the authored sections."),
        _descriptor("ringCvCount", "Ring CVs for new tubes", "enum",
                   _ringCvCountGet, _ringCvCountSet,
                   choices=pomadeTube.REGION_RING_VERT_CHOICES,
                   choiceLabels=tuple(
                       "Match region CVs (Auto)" if int(c) == 0 else str(c)
                       for c in pomadeTube.REGION_RING_VERT_CHOICES),
                   tooltip="How many CVs go round each section ring of the "
                           "next tube a region builds. Auto matches the "
                           "region's own outline."),
        _descriptor("uniformScale", "Selected section scale", "float",
                   _uniformScaleGet, _uniformScaleSet,
                   min=0.01, max=100.0, step=0.05,
                   tooltip=UNIFORM_SCALE_TIP),
    ]


# A center CV that moved less than this (scene units) did not move: the
# closest-point query can land a root already on the scalp a float ulp
# away, and that is not an edit worth an undo step.
_ACTION_EPSILON_DIGITS = 6


def _tubeCentersSnapshot(session, tubeIds):
    """Every target tube's center CVs, rounded; None when unreadable.

    What a tube action is compared by (SS-02): the same snapshot before
    and after means the click changed nothing. None means "cannot tell",
    which keeps the undo step, as _undoStep documents.
    """
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return None
    try:
        try:
            from . import pomadeBridge
        except ImportError:
            import pomadeBridge
        return tuple(
            (int(tubeId),
             tuple(tuple(round(float(c), _ACTION_EPSILON_DIGITS)
                         for c in point)
                   for point in pomadeBridge.tubeCenters(dll, ctx, tubeId)))
            for tubeId in tubeIds)
    except (ImportError, AttributeError, NotImplementedError, RuntimeError,
            TypeError, ValueError, ctypes.ArgumentError):
        return None


def _tubeNames(tubeIds, selected):
    names = ", ".join("T%d" % int(t) for t in tubeIds)
    if not selected:
        return "%s (nothing selected: the primary tube)" % names
    return names


def _runTubeAction(container, label, entry, fallback, doneText,
                   unchangedText, *args):
    """One Tube-panel action as one undo step, or none when nothing moved.

    Relax, Match surface and Snap root each used to call the ABI bare: the
    model pushed a step per tube whether or not a CV moved, so a Relax over
    a straight tube left an empty step for Ctrl+Z to spend. Now the whole
    click is one bracket (SS-02): the target tubes' centers are read before
    and after, and endGestureIfChanged drops the bracket when they match.
    The message area says what happened either way.
    """
    session = getattr(container, "session", None)
    report = getattr(session, "report", None) if session is not None \
        else None

    def say(text, level="info"):
        # The Tube tool's own line follows, as the dock adds it to an
        # action that reports nothing (pomadeWorkspace._onAction).
        loop = _loop(container, "tube")
        try:
            line = str(loop.statusLine() or "") if loop is not None else ""
        except Exception:  # noqa: BLE001 - a status read must not raise
            line = ""
        if line:
            text = "%s -- %s" % (text, line)
        if callable(report):
            report(text, level)

    if _dll(session) is None or _ctx(session) is None:
        say("Pomade: %s needs a bound scalp" % label, "warning")
        return
    selected = selectedTubeIds(session)
    targets = selected or [0]
    names = _tubeNames(targets, bool(selected))
    failures = []

    def write():
        stage = _stage(session)
        dll, ctx = _dll(session), _ctx(session)
        if stage is None or not selected:
            rc = getattr(dll, fallback)(ctx, *args)
            if isinstance(rc, int) and rc != 0:
                lastError = getattr(session, "lastError", None)
                failures.append(lastError() if callable(lastError)
                                else "%s returned %d" % (fallback, rc))
            return
        for tubeId in selected:
            try:
                getattr(stage, entry)(ctx, tubeId, *args)
            except RuntimeError as exc:
                failures.append("T%d: %s" % (int(tubeId), exc))

    def snapshot():
        return _tubeCentersSnapshot(session, targets)

    before = snapshot()
    _undoStep(session, label, write, snapshot)
    after = snapshot()
    changed = before is None or after is None or after != before
    _publish(session)
    if failures and not changed:
        say("Pomade: %s failed -- %s" % (label, "; ".join(failures)),
            "warning")
    elif not changed:
        say("Pomade: %s -- no change: %s" % (label, unchangedText % names))
    elif failures:
        say("Pomade: %s %s; %s" % (label, doneText % names,
                                   "; ".join(failures)), "warning")
    else:
        say("Pomade: %s %s." % (label, doneText % names))


def _matchSurfaceHandler(container):
    _runTubeAction(container, "Match surface", "matchSurface",
                   "Pomade_MatchSurface",
                   "put the root CV of %s on the scalp",
                   "the root CV of %s is already on the scalp")


def _relaxHandler(container):
    _runTubeAction(container, "Relax", "relaxCenter", "Pomade_RelaxCenter",
                   "smoothed the center curve of %s",
                   "%s has no kink to relax", 1.0, 1)


def _snapRootHandler(container):
    _runTubeAction(container, "Snap root", "snapRootToScalp",
                   "Pomade_SnapRootToScalp",
                   "moved %s so its root sits on the scalp",
                   "%s already starts on the scalp")


def _tubeActions(state):
    return [
        Action("matchSurface", "Match surface", "", _matchSurfaceHandler,
               "Snap the selected tubes' root CV onto the scalp; the rest "
               "of the curve stays where it is."),
        Action("relax", "Relax", "", _relaxHandler,
               "Smooth kinks out of the selected tubes' center curves."),
        Action("snapRoot", "Snap root to scalp", "", _snapRootHandler,
               "Put the selected tubes' first CV back on the scalp."),
        Action("resetTransformTool", "Reset transform tool", "",
               _resetTransformToolHandler,
               "Restore the current transform tool's step and snap "
               "settings to their defaults."),
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
    """The live Pomade_GetFillParams() dict, or None with no live context."""
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return None
    density = ctypes.c_float(0.0)
    cvCount = ctypes.c_int(0)
    seed = ctypes.c_int(0)
    edgeBias = ctypes.c_float(0.0)
    profile = (ctypes.c_float * MAX_FILL_PROFILE_FLOATS)()
    got = ctypes.c_int(0)
    rc = dll.Pomade_GetFillParams(
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
    screen, so the write ends in a full-density Pomade_RefillGuides
    (plan/17 section 5.3: preview during a drag, full otherwise). Freeze
    roots is left exactly as the artist set it; the refill keeps the
    frozen prefix by itself. The write and its refill are one undo step
    labelled 'Fill params' (SS-02), so Ctrl+Z puts the density and the
    guides it grew back together.
    """
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return
    tubes = selectedTubeIds(session)
    _undoStep(session, "Fill params",
              lambda: _applyFillParams(session, values, tubes),
              lambda: _fillSnapshot(session, tubes, values))


def _fillSnapshot(session, tubes, values):
    """Every fill param a write to `tubes` can change, for change checks.

    None when a no-op cannot be proven: a child tube's read hands back
    its profile COUNT only, so a same-length profile edit there would
    compare equal and the edit would be cancelled away.
    """
    snap = [_readPrimaryFillParams(session)]
    stage = _stage(session)
    if stage is not None:
        if values["profile"] and any(int(t) != 0 for t in tubes):
            return None
        for tubeId in tubes:
            try:
                snap.append(stage.fillParams(_ctx(session), tubeId))
            except (RuntimeError, AttributeError, TypeError):
                snap.append(None)
    return snap


def _applyFillParams(session, values, tubes):
    dll, ctx = _dll(session), _ctx(session)
    pairs = [float(v) for v in values["profile"]]
    stage = _stage(session)
    if stage is None or not tubes:
        arr = (ctypes.c_float * len(pairs))(*pairs) if pairs else None
        dll.Pomade_SetFillParams(ctx, float(values["density"]),
                                int(values["cvCount"]), int(values["seed"]),
                                float(values["edgeBias"]), arr, len(pairs))
    else:
        for tubeId in tubes:
            stage.setFillParams(ctx, tubeId, float(values["density"]),
                                int(values["cvCount"]), int(values["seed"]),
                                float(values["edgeBias"]), pairs)
    dll.Pomade_RefillGuides(ctx, 1.0)


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
        pairs = list(pomadeFill.validateLengthProfile(value)
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
    # The model's own default is 100 guides per unit area (pomadeModel.h
    # FillParams), so a max below that made the spinbox clamp and show a
    # number the model did not hold.
    return [
        _descriptor("density", "Density", "float", densityGet, densitySet,
                   min=0.1, max=1000.0, step=0.5, unit=DENSITY_UNIT,
                   tooltip="Guides per unit of scalp area in the selected "
                           "tubes (every tube when none is selected). The "
                           "line under it counts the guides it grew."),
        _descriptor("cvCount", "CVs per guide", "int", cvCountGet,
                   cvCountSet, min=2, max=64, step=1,
                   tooltip="Points along each guide curve; more follow a "
                           "bent tube more closely."),
        _descriptor("edgeBias", "Edge bias", "float", edgeBiasGet,
                   edgeBiasSet, min=-1.0, max=1.0, step=0.05,
                   tooltip="Where guide roots gather inside a tube: +1 "
                           "toward its wall, -1 toward its middle, 0 even."),
        _descriptor("seed", "Seed", "int", seedGet, seedSet, min=0,
                   max=9999, step=1,
                   tooltip="A different seed scatters the same number of "
                           "guides in a different pattern."),
        _descriptor("lengthProfile", "Length profile", "ramp", profileGet,
                   profileSet,
                   tooltip="Guide length by where the guide's root sits "
                           "across the tube, centre (0) to wall (1), as "
                           "%s; each value is the fraction of full length "
                           "(0 to 1, above 1 acts as 1). Empty = every "
                           "guide full length. The Length ramp tool (V) "
                           "edits it by dragging." % RAMP_HINT),
        _previewPercentDescriptor(),
        _scalarDescriptor(
            "freezeRoots", "Freeze roots", "bool", "freezeRoots", _boolCast,
            "Pomade_GetFreezeRoots", "Pomade_SetFreezeRoots",
            dllCast=_boolToInt,
            tooltip="Keep the existing guides' roots where they are when "
                    "the guides refill, so a density change adds or "
                    "removes guides instead of re-scattering them all."),
    ]


def _previewPercentDescriptor():
    """Preview density as 0-100 % over the model's 0-1 preview fraction.

    The fraction is what the C ABI and the state hold; artists read a
    percentage, and 'fraction' of what was never said on the row.
    """
    def get(state, session):
        dll, ctx = _dll(session), _ctx(session)
        if dll is not None and ctx is not None:
            state.previewFraction = float(dll.Pomade_GetPreviewFraction(ctx))
        # Rounded so a float32 0.3 shows as 30, not 30.0000012.
        return round(float(state.previewFraction) * 100.0, 4)

    def set(state, session, value):
        fraction = min(max(float(value), 0.0), 100.0) / 100.0
        state.previewFraction = fraction
        dll, ctx = _dll(session), _ctx(session)
        if dll is not None and ctx is not None:
            dll.Pomade_SetPreviewFraction(ctx, fraction)

    return _descriptor(
        "previewFraction", "Preview density while dragging", "float", get,
        set, min=0.0, max=100.0, step=5.0, unit="%",
        tooltip="How many of the guides draw while a drag is live, as a "
                "percentage of the full density. Lower keeps big drags "
                "smooth; the full set returns on release.")


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
    generate = getattr(dll, "Pomade_GenerateGuides", None)
    if generate is not None:
        generate(ctx, 1.0)
    else:
        dll.Pomade_RefillGuides(ctx, 1.0)
    _publish(session)


def _clearGeneratedCurvesHandler(container):
    session = getattr(container, "session", None)
    clear = getattr(session, "clearGeneratedCurves", None)
    if callable(clear):
        clear()
        return
    report = getattr(session, "report", None)
    if callable(report):
        report("Pomade: clear generated curves is unavailable")


def _fillActions(state):
    return [Action("refill", "Refill guides", "", _refillHandler,
                   "Grow the guides again at full density from the current "
                   "settings."),
            Action("clearGeneratedCurves", "Clear generated curves", "",
                   _clearGeneratedCurvesHandler,
                   "Remove the generated guide curves; Refill guides "
                   "brings them back.")]


# ---- Hierarchy ----------------------------------------------------------

def _hierarchyCache(state):
    return state.panels.setdefault("hierarchy", {"subdivideSeed": 0})


def _subdivideSeedGet(state, session):
    return int(_hierarchyCache(state)["subdivideSeed"])


def _subdivideSeedSet(state, session, value):
    _hierarchyCache(state)["subdivideSeed"] = int(value)


# A bool row's value when the tubes it speaks for disagree: the dock shows
# the box part-checked, and a click sets every one of them.
MIXED = "mixed"


def _tubeLockFlag(state, tubeId, key):
    """The tube's OWN lock flag, as the dock last wrote it.

    The model keeps each tube's flag beside the global default (the -1
    slot); propagation honours either, and the commit writes the tube's
    own flag. A tube nobody set has it off, as PomadeModel does.
    """
    record = state.tubeLocks.get(int(tubeId))
    value = record.get(key) if record is not None else None
    return bool(value) if value is not None else False


def _lockRowGet(state, session, key):
    """What a lock row shows: the selected tubes' flags, else the default.

    The row used to read the global flag whatever was selected while its
    setter wrote only the tubes selected at the time, so a tube selected
    later showed ticked and committed unticked. With tubes selected it now
    shows theirs (MIXED when they disagree); with none it shows the
    model's global default.
    """
    tubes = selectedTubeIds(session)
    if not tubes:
        return bool(getattr(state, key))
    values = set(_tubeLockFlag(state, tubeId, key) for tubeId in tubes)
    return values.pop() if len(values) == 1 else MIXED


def _lockRowSet(state, session, key, value):
    """Write the lock where the row reads it: the selection, else the
    global default."""
    value = bool(value)
    if key == "lockParents":
        setFlag = pomadeHierarchy.setLockParents
        entry = pomadeHierarchy.setLockParentsEntry
    else:
        setFlag = pomadeHierarchy.setLockChildren
        entry = pomadeHierarchy.setLockChildrenEntry
    dll, ctx = _dll(session), _ctx(session)
    for tubeId in selectedTubeIds(session) or [None]:
        setFlag(state, value, tubeId)
        if dll is not None and ctx is not None:
            # -1 is the model's global default (HierarchyLoop._pushLocks).
            entry(dll, ctx, -1 if tubeId is None else tubeId, value)


def _lockParentsGet(state, session):
    return _lockRowGet(state, session, "lockParents")


def _lockParentsSet(state, session, value):
    _lockRowSet(state, session, "lockParents", value)


def _lockChildrenGet(state, session):
    return _lockRowGet(state, session, "lockChildren")


def _lockChildrenSet(state, session, value):
    _lockRowSet(state, session, "lockChildren", value)


def _lockTooltip(state, key, text):
    """A lock row's tooltip: what it does, and what it applies to."""
    scope = (" Applies to the selected tubes; with none selected it sets "
             "the default for every tube.")
    if bool(getattr(state, key, False)):
        scope += " The default is on, so every tube is locked this way."
    return text + scope


# The level spin boxes' ceiling. Levels have no depth limit in the model,
# but a groom deeper than this is not something a spin box helps with.
MAX_PANEL_LEVEL = 8

# Solo and Show-levels-up-to are each a checkbox plus a level spin box
# (DK-07). The state keeps its -1 'off' sentinel; the level the spin box
# shows while the checkbox is off is remembered here, so ticking the box
# brings back the level the artist last picked rather than 0 -- which the
# old single '(0 = off)' spin box made the artist type to turn it off.
_LEVEL_PICK_KEYS = {"soloLevel": "soloPick", "showMaxLevel": "showPick"}


def _clampLevel(level):
    return min(max(int(level), pomadeHierarchy.LEVEL_MIN), MAX_PANEL_LEVEL)


def _levelPick(state, attr):
    """The level `attr`'s spin box shows: the live one, else the last pick."""
    live = int(getattr(state, attr))
    cache = _hierarchyCache(state)
    key = _LEVEL_PICK_KEYS[attr]
    if live >= pomadeHierarchy.LEVEL_MIN:
        cache[key] = _clampLevel(live)
    return _clampLevel(cache.get(
        key, max(int(getattr(state, "activeLevel", 1)), 1)))


def _soloOnGet(state, session):
    return int(state.soloLevel) >= pomadeHierarchy.LEVEL_MIN


def _soloOnSet(state, session, value):
    level = _levelPick(state, "soloLevel")
    pomadeHierarchy.setSoloLevel(
        state, level if value else pomadeHierarchy.SOLO_OFF)
    _syncLevels(state, session)


def _soloLevelGet(state, session):
    return _levelPick(state, "soloLevel")


def _soloLevelSet(state, session, value):
    level = _clampLevel(value)
    _hierarchyCache(state)["soloPick"] = level
    if _soloOnGet(state, session):
        pomadeHierarchy.setSoloLevel(state, level)
        _syncLevels(state, session)


def _showAllGet(state, session):
    return int(state.showMaxLevel) < pomadeHierarchy.LEVEL_MIN


def _showAllSet(state, session, value):
    level = _levelPick(state, "showMaxLevel")
    pomadeHierarchy.setShowMaxLevel(
        state, pomadeHierarchy.SHOW_ALL_LEVELS if value else level)
    _syncLevels(state, session)


def _showMaxLevelGet(state, session):
    return _levelPick(state, "showMaxLevel")


def _showMaxLevelSet(state, session, value):
    level = _clampLevel(value)
    _hierarchyCache(state)["showPick"] = level
    if not _showAllGet(state, session):
        pomadeHierarchy.setShowMaxLevel(state, level)
        _syncLevels(state, session)


def _syncLevels(state, session):
    """Turn Solo / Show<=n / hidden into Pomade_SetLevelDisplay calls.

    The toggles were Python-only state (audit section 5.4), so a Solo that
    the panel recorded never reached the viewport. The maths lives in
    pomadeHierarchy.levelDrawStyle; this walks the levels the model holds
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
        style = pomadeHierarchy.levelDrawStyle(
            level, focus, state.soloLevel, state.showMaxLevel,
            state.hiddenLevels)
        xray = bool(xrayMap.get(level, style == "x-ray"))
        dll.Pomade_SetLevelDisplay(ctx, int(level),
                                  _boolToInt(style != "hidden"),
                                  _boolToInt(xray))
    _publish(session)


def _modelLevels(session):
    """The levels the model's tubes sit at, ascending ([] when headless)."""
    dll, ctx = _dll(session), _ctx(session)
    if dll is None or ctx is None:
        return []
    count = ctypes.c_int(0)
    if int(dll.Pomade_ReadTubeIds(ctx, None, 0, ctypes.byref(count))) != 0:
        return []
    n = min(max(int(count.value), 0), MAX_SELECTION)
    if n <= 0:
        return []
    ids = (ctypes.c_int * n)()
    got = ctypes.c_int(0)
    if int(dll.Pomade_ReadTubeIds(ctx, ids, n, ctypes.byref(got))) != 0:
        return []
    levels = {int(dll.Pomade_GetTubeLevel(ctx, ids[i]))
              for i in range(min(n, int(got.value)))}
    return sorted(level for level in levels if level >= 1)


def _levelDisplay(session, level):
    """(visible, xray) for `level`, from the model when live else state."""
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        visible = ctypes.c_int(1)
        xray = ctypes.c_int(0)
        dll.Pomade_GetLevelDisplay(ctx, int(level), ctypes.byref(visible),
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
        pomadeHierarchy.showLevel(state, level)
    else:
        pomadeHierarchy.hideLevel(state, level)
    dll, ctx = _dll(session), _ctx(session)
    if dll is not None and ctx is not None:
        _, xray = _levelDisplay(session, level)
        dll.Pomade_SetLevelDisplay(ctx, level, _boolToInt(value),
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
        dll.Pomade_SetLevelDisplay(ctx, level, _boolToInt(visible),
                                  _boolToInt(value))


def levelRowTooltip(descriptorId, state):
    """The tooltip naming the level a 'This level ...' row edits, or ''.

    The level used to be baked into the two row labels, which changed the
    page signature and rebuilt the whole hierarchy form (and dropped any
    focused widget) on every Enter/Exit level. The labels are now fixed
    (DK-06); this is the rows' Descriptor.tooltip, which the dock re-reads
    on refresh (DK-07).
    """
    template = LEVEL_ROW_TOOLTIPS.get(descriptorId)
    if template is None:
        return ""
    return template % int(getattr(state, "activeLevel", 0))


LEVEL_ROW_TOOLTIPS = {
    "levelVisible": "Show or hide L%d, the level you are editing.",
    "levelXray": "Draw L%d, the level you are editing, see-through so the "
                 "levels behind it stay visible.",
}


SPLIT_MODE_LABELS = {"kmeans": "K-means", "edge": "Edge"}


def _hierarchyDescriptors(state):
    soloOn = int(getattr(state, "soloLevel", -1)) >= pomadeHierarchy.LEVEL_MIN
    showAll = (int(getattr(state, "showMaxLevel", -1)) <
               pomadeHierarchy.LEVEL_MIN)
    return [
        _scalarDescriptor(
            "subdivideCount", "Subdivide count", "int", "subdivideCount",
            int, min=pomadeHierarchy.SUBDIVIDE_MIN,
            max=pomadeHierarchy.SUBDIVIDE_MAX, step=1,
            tooltip="How many child tubes Subdivide (Shift+D) splits each "
                    "selected tube into."),
        _scalarDescriptor(
            "splitMode", "Split mode", "enum", "splitMode", str,
            choices=pomadeHierarchy.SPLIT_MODES,
            choiceLabels=tuple(SPLIT_MODE_LABELS.get(m, m)
                               for m in pomadeHierarchy.SPLIT_MODES),
            tooltip="How Subdivide shares a tube's roots out: K-means makes "
                    "even clumps; Edge cuts it in two along a line you "
                    "draw."),
        _descriptor("subdivideSeed", "Subdivide seed", "int",
                   _subdivideSeedGet, _subdivideSeedSet, min=0, max=9999,
                   step=1,
                   tooltip="A different seed splits the same tube into "
                           "differently shaped children."),
        _descriptor("lockParents", "Lock parents", "bool", _lockParentsGet,
                   _lockParentsSet,
                   tooltip=_lockTooltip(
                       state, "lockParents",
                       "Editing a child leaves its parent tubes where they "
                       "are instead of refitting them to follow.")),
        _descriptor("lockChildren", "Lock children", "bool",
                   _lockChildrenGet, _lockChildrenSet,
                   tooltip=_lockTooltip(
                       state, "lockChildren",
                       "Editing a parent carries its child tubes along "
                       "rigidly, their sculpt unchanged, instead of "
                       "re-deriving them from the new parent shape.")),
        _descriptor("soloLevelOn", "Solo level", "bool",
                   _soloOnGet, _soloOnSet,
                   tooltip="Show only one level of the hierarchy (the "
                           "level below)."),
        _descriptor("soloLevel", "Level to solo", "int",
                   _soloLevelGet, _soloLevelSet, min=pomadeHierarchy.LEVEL_MIN,
                   max=MAX_PANEL_LEVEL, step=1, enabled=soloOn,
                   tooltip="The level Solo level shows on its own."
                   if soloOn else "Tick Solo level to use this."),
        _descriptor("showAllLevels", "Show all levels", "bool",
                   _showAllGet, _showAllSet,
                   tooltip="Untick to hide every level deeper than the one "
                           "below."),
        _descriptor("showMaxLevel", "Show levels up to", "int",
                   _showMaxLevelGet, _showMaxLevelSet,
                   min=pomadeHierarchy.LEVEL_MIN, max=MAX_PANEL_LEVEL,
                   step=1, enabled=not showAll,
                   tooltip="The deepest level drawn; deeper levels hide."
                   if not showAll else "Untick Show all levels to use this."),
        _descriptor("levelVisible", "This level visible", "bool",
                   _levelVisibleGet, _levelVisibleSet,
                   tooltip=levelRowTooltip("levelVisible", state)),
        _descriptor("levelXray", "This level see-through", "bool",
                   _levelXrayGet, _levelXraySet,
                   tooltip=levelRowTooltip("levelXray", state)),
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
    state = container.pomadeState
    seed = _subdivideSeedGet(state, session)
    for tubeId in selectedTubeIds(session):
        pomadeHierarchy.subdivide(dll, ctx, tubeId, state.subdivideCount,
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
        pomadeHierarchy.mergeChildren(dll, ctx, tubeId)
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
        pomadeHierarchy.mergeSelected(dll, ctx, ids)
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
        pomadeHierarchy.groupTubes(dll, ctx, ids, transient=True)
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
        pomadeHierarchy.makePersistent(dll, ctx, tubeId)
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
        Action("subdivide", "Subdivide", "Shift+D", _subdivideHandler,
               "Split each selected tube into Subdivide count children. "
               "The groom looks the same until you edit a child."),
        Action("mergeChildren", "Merge children", "Shift+M",
              _mergeChildrenHandler,
              "Fold the selected tube's children back into it."),
        Action("mergeSelected", "Merge selected", "", _mergeSelectedHandler,
               "Fold the selected sibling tubes into one child."),
        Action("resubdivide", "Re-subdivide", "", _resubdivideHandler,
               "Split the selected tube again with the current count; "
               "click twice to confirm, as the children's sculpting is "
               "lost."),
        Action("group", "Group", "", _groupHandler,
               "Make a temporary parent tube over the selected tubes so "
               "they move together."),
        Action("makePersistent", "Make persistent", "",
              _makePersistentHandler,
              "Keep a temporary Group parent as a real tube in the "
              "groom."),
        Action("enterLevel", "Enter level", "Ctrl+Down",
              _enterLevelHandler,
              "Work on the selected tubes' children."),
        Action("exitLevel", "Exit level", "Ctrl+Up", _exitLevelHandler,
               "Go back up to the parent level."),
    ]


# ---- Sculpt -------------------------------------------------------------

def _sculptDescriptors(state):
    # The strength row's range follows the brush: Smooth stops at 1. The
    # max is part of the page signature, so a brush switch rebuilds the
    # one sculpt page and nothing else.
    smooth = str(getattr(state, "sculptSubMode", "") or "") == "smooth"
    wholeStrand = not float(getattr(state, "brushTRadius", 0.0)) > 0.0
    return [
        # One range for the row, the [ ] keys and the F-drag (pomadeSculpt):
        # a 200 px row maximum used to clamp a 300 px brush the keys made.
        _scalarDescriptor(
            "brushRadiusPx", "Brush radius", "float", "brushRadiusPx",
            float, min=pomadeSculpt.BRUSH_RADIUS_MIN_PX,
            max=pomadeSculpt.BRUSH_RADIUS_MAX_PX, step=1.0, unit="px",
            tooltip="The brush circle's size on screen. [ and ] or "
                    "F+drag resize it."),
        _descriptor("brushWholeStrand", "Whole strand", "bool",
                   _wholeStrandGet, _wholeStrandSet,
                   tooltip="The brush moves the whole length of every "
                           "strand it touches. Untick to limit it with "
                           "Brush reach."),
        _descriptor("brushTRadius", "Brush reach", "float",
                   _brushReachGet, _brushReachSet, min=BRUSH_REACH_MIN,
                   max=1.0, step=0.01, enabled=not wholeStrand,
                   tooltip="How far along the strand the brush reaches from "
                           "the point under it, as a fraction of the "
                           "strand's length."
                   if not wholeStrand else
                   "Untick Whole strand to limit how far along the strand "
                   "the brush reaches."),
        _scalarDescriptor(
            "sculptPreserveLength", "Preserve length", "bool",
            "sculptPreserveLength", _boolCast,
            tooltip="Keep each strand's length while it is pulled, so "
                    "brushing bends hair instead of stretching it."),
        _scalarDescriptor(
            "sculptStrength", "Brush strength", "float", "sculptStrength",
            float, min=0.0,
            max=(pomadeSculpt.SMOOTH_STRENGTH_MAX if smooth
                 else pomadeSculpt.STRENGTH_MAX), step=0.05,
            tooltip="How strongly one stroke moves the strands. Smooth "
                    "stops at 1 (fully relaxed)."),
        _scalarDescriptor(
            "sculptMirrorX", "Mirror X", "bool", "sculptMirrorX",
            _boolCast,
            tooltip="Repeat every stroke on the other side of the groom "
                    "(mirrored across X = 0)."),
    ]


# 'Brush reach' is state.brushTRadius, where 0 has always meant 'no bound
# along the strand'. The row now shows that as a Whole strand checkbox
# (DK-07) and keeps a real reach for when it is unticked.
BRUSH_REACH_MIN = 0.01
DEFAULT_BRUSH_REACH = 0.25


def _sculptCache(state):
    return state.panels.setdefault("sculpt", {})


def _brushReachGet(state, session):
    live = float(state.brushTRadius)
    cache = _sculptCache(state)
    if live > 0.0:
        cache["reach"] = live
    return min(max(float(cache.get("reach", DEFAULT_BRUSH_REACH)),
                   BRUSH_REACH_MIN), 1.0)


def _brushReachSet(state, session, value):
    reach = min(max(float(value), BRUSH_REACH_MIN), 1.0)
    _sculptCache(state)["reach"] = reach
    if float(state.brushTRadius) > 0.0:
        state.brushTRadius = reach


def _wholeStrandGet(state, session):
    return not float(state.brushTRadius) > 0.0


def _wholeStrandSet(state, session, value):
    if value:
        _brushReachGet(state, session)       # remember the reach first
        state.brushTRadius = 0.0
    else:
        state.brushTRadius = _brushReachGet(state, session)



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


# What the Bake resolution combo shows for each TEXEL_CHOICES entry; the
# choice itself stays the stored value.
TEXEL_CHOICE_LABELS = tuple(
    "Auto (64 on boundaries)" if c == "auto" else "%s x %s" % (c, c)
    for c in TEXEL_CHOICES)


def _texelResolutionDescriptor():
    """The one Bake resolution row Graph and Output share."""
    return _descriptor("texelResolution", "Bake resolution", "enum",
                       _texelResolutionGet, _texelResolutionSet,
                       choices=TEXEL_CHOICES,
                       choiceLabels=TEXEL_CHOICE_LABELS,
                       tooltip=TEXEL_RESOLUTION_HELP)


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


def _setOutputScalar(session, **settings):
    """One Output scalar write as one labelled undo step (SS-02).

    The model already pushes a step per real change; the bracket gives it
    the name the Edit strip shows and keeps a same-value write stepless.
    Returns the setter's answer (True with no setter to call).
    """
    setter = getattr(session, "setOutputSettings", None) \
        if session is not None else None
    if not callable(setter):
        return True
    getter = getattr(session, "outputSettings", None)
    snapshot = ((lambda: tuple(getter())) if callable(getter)
                else (lambda: None))
    ok = _undoStep(session, "Output settings",
                   lambda: setter(**settings), snapshot)
    # The setter queued its commit inside the bracket, and closing the
    # bracket bumps the model version once more; queue again so the
    # committed version catches up instead of reading as unsynced.
    enqueue = getattr(session, "enqueueCommit", None)
    if ok and callable(enqueue) and callable(
            getattr(session, "beginGesture", None)):
        enqueue()
    return ok


def _outputDensitySet(state, session, value):
    value = max(float(value), 1e-6)
    if not _setOutputScalar(session, densityMultiplier=value):
        return
    state.outputDensityMultiplier = value


def _outputWidthGet(state, session):
    return _outputSettings(state, session)[2]


def _outputWidthSet(state, session, value):
    value = max(float(value), 0.0)
    if not _setOutputScalar(session, strandWidth=value):
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
        report("Pomade: Output description is unavailable")


def _amplifiedHairGet(state, session):
    """The model's amplified-hair switch when live, else the state's."""
    dll, ctx = _dll(session), _ctx(session)
    entry = (getattr(dll, "Pomade_GetAmplifiedHair", None)
             if dll is not None and ctx is not None else None)
    if entry is not None:
        state.showAmplifiedHair = bool(entry(ctx))
    return bool(state.showAmplifiedHair)


def _amplifiedHairSet(state, session, value):
    """Show amplified hair through the model, not the state field alone.

    The row used to write only state.showAmplifiedHair, so it and the
    Display checkbox both read ticked while the viewport kept showing
    guides. PomadeSession.setAmplifiedHair is the one path (model switch +
    publish); a session without it -- the headless panel test's fake --
    gets the same ABI call directly.
    """
    value = bool(value)
    setter = getattr(session, "setAmplifiedHair", None) \
        if session is not None else None
    if callable(setter):
        setter(value)
        return
    dll, ctx = _dll(session), _ctx(session)
    entry = (getattr(dll, "Pomade_SetAmplifiedHair", None)
             if dll is not None and ctx is not None else None)
    if entry is not None:
        if int(entry(ctx, _boolToInt(value))) != 0:
            return
        state.showAmplifiedHair = value
        _publish(session)
        return
    state.showAmplifiedHair = value


BUILD_DESCRIPTION_TIP = (
    "Turn the guides into the renderable hair description (usdGen hair "
    "under /PomadeGroom/Output) and keep it updated as you groom. Strand "
    "density and width work once it exists.")


def _outputActions(_state):
    return [Action("buildDescription", "Build hair description", "",
                   _buildDescriptionHandler, BUILD_DESCRIPTION_TIP)]


def _outputDescriptors(state):
    # The density and width rows are always there, greyed until the first
    # Build (DK-07): rows that appeared from nowhere after Build gave no
    # hint that Build was what they were waiting for.
    built = bool(getattr(state, "outputEnabled", False))
    return [
        _texelResolutionDescriptor(),
        _descriptor("outputDensityMultiplier", "Strand density multiplier",
                    "float", _outputDensityGet, _outputDensitySet,
                    min=0.01, max=100.0, step=0.1, enabled=built,
                    tooltip="Rendered strands per guide-density unit: 2 "
                            "grows twice the hair Fill's density gives."
                    if built else BUILD_FIRST_TIP),
        _descriptor("outputStrandWidth", "Strand width", "float",
                    _outputWidthGet, _outputWidthSet,
                    min=0.0, max=1.0, step=0.001, unit="units",
                    enabled=built,
                    tooltip="Rendered strand thickness, in scene units."
                    if built else BUILD_FIRST_TIP),
        _descriptor("showAmplifiedHair", "Show amplified hair", "bool",
                    _amplifiedHairGet, _amplifiedHairSet,
                    tooltip="Draw the full rendered hair in the viewport "
                            "instead of only the guides. The same switch "
                            "as Display > Show amplified hair."),
    ]


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
    Qt, so pomadeWorkspace.py wires those three directly against
    `container.session`."""
    builder = _ACTION_BUILDERS.get(modeId)
    return builder(None) if builder is not None else []
