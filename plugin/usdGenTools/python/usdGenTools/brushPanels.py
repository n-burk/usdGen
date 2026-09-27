# Qt-free parameter/action descriptors for the brush palette dock.
#
# plan/08 section 5.2: panels are generated, never hand-written. descriptors()
# returns the ordered rows for the palette, grouped into sections (target,
# brush, preview, resolution); actions() the one-shot buttons; brushPalette.py
# is the only module that turns them into widgets. Both are pure data plus
# closures over the BrushToolState, so the T1 suites exercise every closure
# without a window.
#
# A descriptor's get(state)/set(state, value) touch `state` only -- nothing
# here reaches the stage, which is why the panel stays live with no surface
# bound (the loop fails a stroke closed until there is one). An action's
# handler(palette) gets the palette so it can reach the state, the loop and
# the viewport controller together.
#
# The viewport's hotkey and ring arithmetic lives here too (hotkeyFor,
# stepRadius, stepHardness, adjustValue, ringRadii), so it is covered
# headless and brushViewport.py only routes Qt events into it.

import collections
import math
import os

try:
    from . import brushAuthor, brushMap, brushState
except ImportError:  # file-path test load
    import brushAuthor
    import brushMap
    import brushState

Descriptor = collections.namedtuple(
    "Descriptor",
    ("id", "label", "kind", "min", "max", "step", "choices", "get", "set",
     "group", "tooltip", "icons"),
    defaults=("", "", ()))

Action = collections.namedtuple(
    "Action", ("id", "label", "hotkeyLabel", "handler", "icon", "tooltip",
               "group"),
    defaults=("", "", ""))

# Section order in the palette.
GROUPS = (
    ("target", "Target"),
    ("brush", "Brush"),
    ("preview", "Preview"),
    ("resolution", "Resolution"),
)

# Hotkey labels shown in tooltips (the viewport claims them only while the
# pointer is over the view with a surface bound and strokes armed).
HOTKEY_RADIUS = "F drag / [ ]"
HOTKEY_STRENGTH = "Shift+F"
HOTKEY_HARDNESS = "Ctrl+F / Shift+[ ]"

# (keys, what) for the palette's hotkey tooltip and the report.
HOTKEYS = (
    ("F", "drag-adjust radius"),
    ("Shift+F", "drag-adjust strength"),
    ("Ctrl+F", "drag-adjust hardness (inner ring)"),
    ("[ / ]", "radius -10% / +10%"),
    ("Shift+[ / Shift+]", "hardness -0.1 / +0.1"),
    ("Click / Enter", "confirm a drag-adjust"),
    ("Esc / right click", "cancel a drag-adjust or a live stroke"),
)

HOTKEY_NOTE = (
    "Viewport keys work only while the pointer is over the viewport, a "
    "surface is bound and strokes are armed; elsewhere F stays usdview's "
    "Frame Selected. Undo/Redo stay on these buttons (Ctrl+Z/Ctrl+Y are "
    "usdview's).")

# Drag-adjust sensitivity: logical pixels of horizontal travel.
ADJUST_RADIUS_PIXELS_PER_DOUBLING = 150.0
ADJUST_UNIT_PIXELS = 300.0   # strength/hardness: 300 px spans 0..1
RADIUS_STEP = 0.10           # [ / ] multiplies by 1 -/+ this
HARDNESS_STEP = 0.10         # Shift+[ / ] adds -/+ this
RADIUS_FLOOR = 1e-4          # a stepped or adjusted radius never reaches 0

# Radius slider span (log10 world units); the spinbox stays unbounded.
RADIUS_SLIDER_LOG_RANGE = (-3.0, 1.0)


def _descriptor(id, label, kind, get, set, min=0.0, max=0.0, step=0.0,
                choices=(), group="", tooltip="", icons=()):
    return Descriptor(id, label, kind, min, max, step, tuple(choices),
                      get, set, group, tooltip, tuple(icons))


def _clamped(value, lo, hi):
    """float(value) within [lo, hi]; hi=None leaves the top open."""
    try:
        value = float(value)
    except (TypeError, ValueError):
        return lo
    if value != value:
        return lo
    if hi is None:
        return max(lo, value)
    return min(hi, max(lo, value))


def snapResolution(value):
    """The power of two in [RESOLUTION_MIN, RESOLUTION_MAX] nearest value."""
    try:
        value = float(value)
    except (TypeError, ValueError):
        return 32
    if value != value or value <= 0.0:
        return brushState.RESOLUTION_MIN
    exponent = int(round(math.log(value, 2.0)))
    out = 1 << max(0, exponent)
    return int(min(brushState.RESOLUTION_MAX,
                   max(brushState.RESOLUTION_MIN, out)))


def resolutionChoices():
    """Every selectable bind resolution, ascending."""
    out = []
    r = brushState.RESOLUTION_MIN
    while r <= brushState.RESOLUTION_MAX:
        out.append(r)
        r *= 2
    return out


def _presetIcon(presetId):
    return "preset_%s" % presetId


def descriptors(state):
    """Ordered parameter rows for the palette."""
    channels = [("All channels", -1), ("Channel 0", 0)]
    bound = getattr(state, "binding", None)
    if bound is not None and getattr(bound, "channels", 1) == 3:
        channels.append(("Channel 1", 1))
        channels.append(("Channel 2", 2))
    presets = [(p.label, p.id) for p in brushAuthor.MASK_PRESETS]
    rows = [
        _descriptor(
            "maskPreset", "Mask", "enum",
            lambda s: s.maskPreset,
            lambda s, v: setattr(
                s, "maskPreset",
                v if brushAuthor.MaskPresetFor(v) is not None else "density"),
            choices=presets, group="target",
            tooltip="Grooming mask to paint: switching rebinds the surface "
                    "to that mask's PaintMap and redraws it.",
            icons=[_presetIcon(p.id) for p in brushAuthor.MASK_PRESETS]),
        # No upper bound: a dab spills onto every face its world-space
        # footprint touches, so big radii are the normal way to cover
        # ground on dense scalps. Tiny radii floor to the corner-reach
        # disk (every dab paints visibly).
        _descriptor(
            "radiusWorld", "Radius", "float",
            lambda s: s.radiusWorld,
            lambda s, v: setattr(s, "radiusWorld", _clamped(v, 0.0, None)),
            min=0.0, max=None, step=0.01, group="brush",
            tooltip="Brush radius in world units, the outer ring (%s)."
                    % HOTKEY_RADIUS,
            icons=["radius"]),
        _descriptor(
            "strength", "Strength", "float",
            lambda s: s.strength,
            lambda s, v: setattr(s, "strength", _clamped(v, 0.0, 1.0)),
            min=0.0, max=1.0, step=0.05, group="brush",
            tooltip="How far one dab moves the map toward its target (%s)."
                    % HOTKEY_STRENGTH,
            icons=["strength"]),
        _descriptor(
            "hardness", "Hardness", "float",
            lambda s: s.hardness,
            lambda s, v: setattr(s, "hardness", _clamped(v, 0.0, 1.0)),
            min=0.0, max=1.0, step=0.05, group="brush",
            tooltip="Inner ring as a fraction of the radius: full weight "
                    "inside it, falloff out to the rim (%s)."
                    % HOTKEY_HARDNESS,
            icons=["falloff"]),
        _descriptor(
            "falloff", "Falloff", "enum",
            lambda s: s.falloff,
            lambda s, v: setattr(
                s, "falloff", v if v in brushMap.FALLOFFS else "smooth"),
            choices=[("Constant", "constant"), ("Linear", "linear"),
                     ("Smooth", "smooth")], group="brush",
            tooltip="Weight curve from the inner ring to the outer ring."),
        _descriptor(
            "value", "Value", "float",
            lambda s: s.value,
            lambda s, v: setattr(s, "value", _clamped(v, -1.0, 2.0)),
            min=-1.0, max=2.0, step=0.05, group="brush",
            tooltip="Paint target value (Paint, Flood) or offset (Add)."),
        _descriptor(
            "channel", "Channel", "enum",
            lambda s: s.channel,
            lambda s, v: setattr(s, "channel", int(v)),
            choices=channels, group="brush",
            tooltip="Paint every channel of the map, or one."),
        _descriptor(
            "previewMap", "Show map", "bool",
            lambda s: s.previewMap,
            lambda s, v: setattr(s, "previewMap", bool(v)),
            group="preview",
            tooltip="Show the bound map as a colour overlay on the surface.",
            icons=["preview_on", "preview_off"]),
        _descriptor(
            "colorMap", "Ramp", "enum",
            lambda s: s.colorMap,
            lambda s, v: setattr(s, "colorMap",
                                 v if v in ("heat", "gray") else "heat"),
            choices=[("Heat", "heat"), ("Gray", "gray")], group="preview",
            tooltip="Colour ramp of the map overlay."),
        _descriptor(
            "liveGroom", "Live groom", "bool",
            lambda s: s.liveGroom,
            lambda s, v: setattr(s, "liveGroom", bool(v)),
            group="preview",
            tooltip="Re-cook the groom while dragging (throttled); the "
                    "release bake is the source of truth either way.",
            icons=["live_groom"]),
        _descriptor(
            "strokesArmed", "Viewport strokes", "bool",
            lambda s: s.strokesArmed,
            lambda s, v: setattr(s, "strokesArmed", bool(v)),
            group="preview",
            tooltip="Capture left-drag strokes and the brush hotkeys in the "
                    "viewport. Off: the viewport behaves like stock usdview.",
            icons=["strokes_armed"]),
        _descriptor(
            "resolutionAuto", "Auto", "bool",
            lambda s: s.resolutionAuto,
            lambda s, v: setattr(s, "resolutionAuto", bool(v)),
            group="resolution",
            tooltip="Bind at the suggested texel density for the mesh."),
        _descriptor(
            "resolution", "Texels", "pow2",
            lambda s: s.resolution,
            lambda s, v: setattr(s, "resolution", snapResolution(v)),
            min=brushState.RESOLUTION_MIN, max=brushState.RESOLUTION_MAX,
            step=2, group="resolution",
            tooltip="Manual bind resolution, texels per face edge (power "
                    "of two). Applies to the next bind of a new map."),
    ]
    return rows


def actions():
    """One-shot palette buttons: setup, paint-to, bind, flood, undo, redo."""

    def setupDescription(palette):
        palette.setupDescription()

    def paintToDescription(palette):
        palette.paintToDescription()

    def bind(palette):
        palette.bindFromSelection()

    def flood(palette):
        palette.flood()

    def undo(palette):
        palette.undo()

    def redo(palette):
        palette.redo()

    return [
        Action("setupDescription", "Setup", "", setupDescription,
               "setup",
               "Setup description: grow a ready-to-cook groom on the "
               "selected mesh and paint to it.", "target"),
        Action("paintToDescription", "Paint to", "", paintToDescription,
               "paint_to",
               "Paint to description: pick which description's maps the "
               "brush paints.", "target"),
        Action("bind", "Bind", "", bind, "bind",
               "Bind the paint map on the selected mesh (or the active "
               "description's surface).", "target"),
        Action("flood", "Flood", "", flood, "flood",
               "Fill the whole bound map with Value (one undoable bake).",
               "brush"),
        Action("undo", "Undo", "", undo, "undo",
               "Undo the last brush bake.", "history"),
        Action("redo", "Redo", "", redo, "redo",
               "Redo the last undone brush bake.", "history"),
    ]


def statusText(state, extra=""):
    """One status line for the palette: binding, brush, gesture, target."""
    bound = getattr(state, "binding", None)
    active = getattr(state, "activeDescription", "")
    if bound is None:
        if active:
            text = "unbound: Bind paints to %s" % active
        else:
            text = "unbound: select a mesh, Bind paint map"
    else:
        # The subset for a GeomSubset binding (its parent owns the primvar).
        text = "bound: %s -> %s (%s)" % (
            getattr(bound, "targetPath", bound.surfacePath), bound.mapPath,
            bound.primvar)
        if active:
            text += " | desc: %s" % active
    text += " | brush: %s r=%.3g s=%.2f h=%.2f" % (
        getattr(state, "activeBrush", "?"),
        float(getattr(state, "radiusWorld", 0.0)),
        float(getattr(state, "strength", 0.0)),
        float(getattr(state, "hardness", 0.0)))
    gesture = getattr(state, "gesture", None)
    if gesture is not None:
        count = getattr(gesture, "dabCount", None)
        if count is None:
            count = gesture.stroke.dabCount
        text += " | stroke: %d dabs" % count()
    if extra:
        text += " | " + extra
    return text


# Status strip states and their dot colours.
STATUS_KINDS = ("idle", "stroke", "cooking", "ok", "error")
STATUS_COLORS = {
    "idle": "#6b6b73",
    "stroke": "#4d9be6",
    "cooking": "#e0a33a",
    "ok": "#5fbf60",
    "error": "#e5534b",
}


def statusKind(state, kind=None):
    """The status dot state: a live gesture always reads as stroke."""
    if getattr(state, "gesture", None) is not None and kind != "error":
        return "stroke"
    return kind if kind in STATUS_KINDS else "idle"


READOUT_SEPARATOR = " \u00b7 "


def _shortSuggestion(info):
    """("32 px/face", "0.1 MTexel") from a suggestResolution info string.

    The info reads like "32 px/face (median edge 0.5) -> 0.1 MTexel"; the
    median-edge aside is dropped. Anything unparsable comes back whole as
    the size with no texel total."""
    text = str(info or "").strip()
    if not text:
        return ("", "")
    total = ""
    if "->" in text:
        text, total = [part.strip() for part in text.split("->", 1)]
    if "(" in text:
        text = text.split("(", 1)[0].strip()
    return (text.rstrip(","), total)


def resolutionText(state, suggestion=None):
    """The resolution readout, short enough to wrap in a narrow dock.

    "bound 32 px/face \u00b7 auto suggests 32 px/face \u00b7 0.1 MTexel":
    the bound map's size, then the auto suggestion (passed in, or the
    binding's own resolutionInfo from an auto bind) and its texel total."""
    bound = getattr(state, "binding", None)
    parts = []
    if bound is not None:
        res = getattr(bound, "resolution", None)
        if res is not None:
            parts.append("bound %d px/face" % int(res))
        if not suggestion:
            suggestion = getattr(bound, "resolutionInfo", "")
    else:
        parts.append("unbound")
    size, total = _shortSuggestion(suggestion)
    if size:
        parts.append("auto suggests " + size)
    if total:
        parts.append(total)
    return READOUT_SEPARATOR.join(parts)


# -- viewport hotkey / ring arithmetic -----------------------------------

def hotkeyFor(key, shift=False, ctrl=False, alt=False):
    """The brush hotkey action for a key, or None when it is not ours.

    `key` is the typed character: "F", "[", "]", or "{"/"}" (Shift+[ and
    Shift+] on US layouts). Alt/Meta combinations are never claimed
    (camera modifiers)."""
    if alt:
        return None
    key = str(key or "")
    if key in ("f", "F"):
        if ctrl and shift:
            return None
        if ctrl:
            return "adjustHardness"
        if shift:
            return "adjustStrength"
        return "adjustRadius"
    if ctrl:
        return None
    if key == "{" or (key == "[" and shift):
        return "hardnessDown"
    if key == "}" or (key == "]" and shift):
        return "hardnessUp"
    if key == "[":
        return "radiusDown"
    if key == "]":
        return "radiusUp"
    return None


HINT_UNBOUND = "bind a surface to use brush hotkeys"
HINT_DISARMED = "turn on Viewport strokes to use brush hotkeys"

# Where keyboard focus sits, for hotkeyDecision.
FOCUS_NONE = "none"          # nothing, or a non-text widget outside the palette
FOCUS_PALETTE = "palette"    # any widget inside the brush palette dock
FOCUS_FOREIGN = "foreign"    # a text widget outside the palette (prim search...)


def hotkeyDecision(pointerOver, bound, armed, busy=False, focus=FOCUS_NONE):
    """Whether a brush hotkey is claimed: (claim, stealFocus, hint).

    usdview's AppEventFilter keeps focus on any spinbox, slider, combo or
    line edit ("jealous focus"), so after the artist edits a palette value
    the focus never comes back on its own. A palette-held focus therefore
    does NOT block: the key is claimed and focus moves to the viewport (the
    editor commits its value on focus-out). A text widget elsewhere in
    usdview still blocks, silently. With the pointer over the view but no
    binding, or strokes disarmed, the key passes through with a hint.
    `busy` covers a live gesture, a live adjust, and an open popup/modal."""
    if not pointerOver or busy:
        return (False, False, "")
    if focus == FOCUS_FOREIGN:
        return (False, False, "")
    if not bound:
        return (False, False, HINT_UNBOUND)
    if not armed:
        return (False, False, HINT_DISARMED)
    return (True, focus == FOCUS_PALETTE, "")


# The state field each drag-adjust edits.
ADJUST_FIELDS = {
    "adjustRadius": "radiusWorld",
    "adjustStrength": "strength",
    "adjustHardness": "hardness",
}

ADJUST_LABELS = {
    "adjustRadius": "Radius",
    "adjustStrength": "Strength",
    "adjustHardness": "Hardness",
}


def stepRadius(radius, direction):
    """[ / ]: radius times 1 -/+ RADIUS_STEP, never below RADIUS_FLOOR."""
    radius = _clamped(radius, 0.0, None)
    if radius <= 0.0:
        radius = RADIUS_FLOOR
    factor = (1.0 + RADIUS_STEP) if direction > 0 else (1.0 - RADIUS_STEP)
    return max(RADIUS_FLOOR, radius * factor)


def stepHardness(hardness, direction):
    """Shift+[ / ]: hardness -/+ HARDNESS_STEP within [0, 1]."""
    hardness = _clamped(hardness, 0.0, 1.0)
    step = HARDNESS_STEP if direction > 0 else -HARDNESS_STEP
    # Round to the step grid so repeated taps land on 0.1 multiples.
    return _clamped(round((hardness + step) * 1000.0) / 1000.0, 0.0, 1.0)


def adjustValue(action, start, dxPixels):
    """The drag-adjust value after `dxPixels` of horizontal travel.

    Radius is multiplicative (doubles every
    ADJUST_RADIUS_PIXELS_PER_DOUBLING px right, halves left), so it feels
    the same on a 0.01 and a 10-unit brush; strength and hardness are
    linear over ADJUST_UNIT_PIXELS and clamp to [0, 1]."""
    try:
        dx = float(dxPixels)
    except (TypeError, ValueError):
        dx = 0.0
    if dx != dx:
        dx = 0.0
    if action == "adjustRadius":
        start = _clamped(start, 0.0, None)
        if start <= 0.0:
            start = RADIUS_FLOOR
        return max(RADIUS_FLOOR,
                   start * 2.0 ** (dx / ADJUST_RADIUS_PIXELS_PER_DOUBLING))
    return _clamped(_clamped(start, 0.0, 1.0) + dx / ADJUST_UNIT_PIXELS,
                    0.0, 1.0)


def ringRadii(outerPixels, hardness):
    """(outer, inner) ring radii in pixels; inner = hardness * outer."""
    outer = _clamped(outerPixels, 0.0, None)
    return (outer, outer * _clamped(hardness, 0.0, 1.0))


def radiusToSlider(radius, steps=1000):
    """Radius slider position (log scale over RADIUS_SLIDER_LOG_RANGE)."""
    lo, hi = RADIUS_SLIDER_LOG_RANGE
    radius = _clamped(radius, 0.0, None)
    if radius <= 0.0:
        return 0
    t = (math.log10(radius) - lo) / (hi - lo)
    return int(round(min(1.0, max(0.0, t)) * steps))


def sliderToRadius(position, steps=1000):
    lo, hi = RADIUS_SLIDER_LOG_RANGE
    t = min(1.0, max(0.0, float(position) / float(steps)))
    return 10.0 ** (lo + t * (hi - lo))


# -- icons ----------------------------------------------------------------

ICON_NAMES = (
    "brush_paint", "brush_add", "brush_smooth", "brush_erase", "flood",
    "bind", "setup", "paint_to", "undo", "redo", "preview_on",
    "preview_off", "live_groom", "strokes_armed", "radius", "strength",
    "falloff", "preset_density", "preset_length", "preset_width",
    "preset_clump", "preset_curl", "palette",
)


def iconDirectories():
    """Where the plugin's icons may sit, most specific first.

    The package is staged into <build>/python/usdGenTools with resources
    under <build>/usd/usdGenTools/resources; the source tree keeps them at
    plugin/usdGenTools/resources (the exprLibrary arrangement)."""
    here = os.path.dirname(os.path.abspath(__file__))
    up = os.path.dirname(os.path.dirname(here))
    return [
        os.path.join(here, "icons"),
        os.path.join(up, "usd", "usdGenTools", "resources", "icons"),
        os.path.join(up, "resources", "icons"),
        os.path.join(os.path.dirname(up), "usdGenTools", "resources",
                     "icons"),
    ]


def iconPath(name):
    """The PNG for icon `name`, or None when no directory has it."""
    if not name:
        return None
    for directory in iconDirectories():
        path = os.path.join(directory, name + ".png")
        if os.path.isfile(path):
            return path
    return None
