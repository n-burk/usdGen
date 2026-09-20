# usdGenTonicTools.tonicModes -- Qt-free mode shelf and hotkey table
# (plan/17 section 5, plan/18 section 3.4).
#
# Modes are a shelf; each mode has one tool loop, one gizmo family, one
# hotkey set. Selecting a mode records it on TonicToolState and reports the
# mode's status line. HotkeyAction at the foot of this file is the whole of
# the key table: tonicViewport only normalises a QKeyEvent into the strings
# it takes, so the table itself is testable without a window.
from __future__ import annotations

import collections


Mode = collections.namedtuple("Mode", ("id", "label", "hotkey", "status"))

# Number keys pick the mode and letters pick the sub-mode inside it
# (plan/18 section 3.4). The letter hotkeys the shelf used to carry
# collided with the sub-mode letters of whichever mode was active, which
# is why Mode.hotkey was declared and never installed (plan/18 finding
# F4); the digits do not collide with anything usdview binds.
MODES = (
    Mode("graph",     "Graph",     "1",
         "Graph mode: draw regions on the scalp."),
    Mode("tube",      "Tube",      "2",
         "Tube mode: sculpt center curves and sections."),
    Mode("fill",      "Fill",      "3",
         "Fill mode: guide density and preview."),
    Mode("hierarchy", "Hierarchy", "4",
         "Hierarchy mode: subdivide and navigate levels."),
    Mode("sculpt",    "Sculpt",    "5",
         "Sculpt mode: hierarchical brushes."),
    Mode("output",    "Output",    "6",
         "Output panel: maps, bake, save groom."),
)

# Graph-mode sub-modes (plan/17 section 5.1, P2): one click/drag behaviour
# each, all driving the same C++ graph through tonicLib.
GRAPH_SUBMODES = (
    Mode("draw",    "Draw",    "D",
         "Draw: stroke regions onto the scalp (welds at snapped ends)."),
    Mode("place",   "Place",   "P",
         "Place: click adds a node, drag moves (welds onto nodes/edges)."),
    Mode("connect", "Connect", "C",
         "Connect: click two nodes to join them with a geodesic edge."),
    Mode("weld",    "Weld",    "W",
         "Weld: click two nodes to merge (Shift+W welds a pair)."),
    Mode("unweld",  "Unweld",  "U",
         "Unweld: click a shared node to split it per region (Shift+U)."),
    Mode("delete",  "Delete",  "X",
         "Delete: click a node or edge to remove it."),
    Mode("link",    "Link",    "L",
         "Link: click two regions to share one interpolation id."),
)

# Tube-mode sub-modes (plan/17 section 5.2, P3): one gizmo family each.
TUBE_SUBMODES = (
    Mode("center",  "Center",  "C",
         "Center: translate gizmo, insert/delete CV, length, match surface."),
    Mode("ring",    "Ring",    "R",
         "Ring: per-ring translate/scale/twist gizmo, per-CV drag."),
    Mode("section", "Section", "E",
         "Section: add/remove ring at t, copy ring along, root snap."),
)

# Fill-mode sub-modes (plan/17 section 5.3, P3).
FILL_SUBMODES = (
    Mode("params",  "Params",  "P",
         "Params: density, CV count, length ramp, edge bias, seed."),
    Mode("preview", "Preview", "V",
         "Preview: 25% live preview, full density on release, freeze roots."),
)

# Hierarchy-mode sub-modes (plan/17 section 5.4, P4): one tool loop each.
HIERARCHY_SUBMODES = (
    Mode("navigate",  "Navigate",  "N",
         "Navigate: Enter/Exit level, click the breadcrumb."),
    Mode("subdivide", "Subdivide", "D",
         "Subdivide (Shift+D): split into count children (2..8)."),
    Mode("merge",     "Merge",     "M",
         "Merge (Shift+M): absorb children or fold siblings."),
    Mode("group",     "Group",     "G",
         "Group: K7 on-the-fly parent, transient or persistent."),
    Mode("levels",    "Levels",    "L",
         "Levels: visibility, x-ray, Solo, Show <= n."),
)

# Sculpt-mode sub-modes (plan/17 section 5.5, P4): one brush each.
SCULPT_SUBMODES = (
    Mode("grab",     "Grab",     "G",
         "Grab: move CVs along the stroke."),
    Mode("smooth",   "Smooth",   "S",
         "Smooth: relax curvature under the brush."),
    Mode("comb",     "Comb",     "C",
         "Comb: push CVs along a direction."),
    Mode("lengthen", "Lengthen", "L",
         "Lengthen: grow/shorten, root pinned."),
    Mode("twist",    "Twist",    "T",
         "Twist: rotate offsets about the chord axis."),
)

_IDS = frozenset(m.id for m in MODES)


def ModeById(modeId):
    """The Mode for `modeId`, or None when unknown."""
    for mode in MODES:
        if mode.id == modeId:
            return mode
    return None


def GraphSubModeById(subId):
    """The graph sub-mode for `subId`, or None when unknown."""
    for mode in GRAPH_SUBMODES:
        if mode.id == subId:
            return mode
    return None


def SetActiveGraphSubMode(state, subId):
    """Record `subId` on `state`; "" clears. Returns the status line."""
    if subId == "":
        state.graphSubMode = ""
        return "Tonic Graph: no sub-mode."
    mode = GraphSubModeById(subId)
    if mode is None:
        return ""
    state.graphSubMode = mode.id
    return "Tonic Graph: %s" % mode.status


def TubeSubModeById(subId):
    """The tube sub-mode for `subId`, or None when unknown."""
    for mode in TUBE_SUBMODES:
        if mode.id == subId:
            return mode
    return None


def FillSubModeById(subId):
    """The fill sub-mode for `subId`, or None when unknown."""
    for mode in FILL_SUBMODES:
        if mode.id == subId:
            return mode
    return None


def SetActiveTubeSubMode(state, subId):
    """Record `subId` on `state`; "" clears. Returns the status line."""
    if subId == "":
        state.tubeSubMode = ""
        return "Tonic Tube: no sub-mode."
    mode = TubeSubModeById(subId)
    if mode is None:
        return ""
    state.tubeSubMode = mode.id
    return "Tonic Tube: %s" % mode.status


def SetActiveFillSubMode(state, subId):
    """Record `subId` on `state`; "" clears. Returns the status line."""
    if subId == "":
        state.fillSubMode = ""
        return "Tonic Fill: no sub-mode."
    mode = FillSubModeById(subId)
    if mode is None:
        return ""
    state.fillSubMode = mode.id
    return "Tonic Fill: %s" % mode.status


def HierarchySubModeById(subId):
    """The hierarchy sub-mode for `subId`, or None when unknown."""
    for mode in HIERARCHY_SUBMODES:
        if mode.id == subId:
            return mode
    return None


def SculptSubModeById(subId):
    """The sculpt sub-mode (brush) for `subId`, or None when unknown."""
    for mode in SCULPT_SUBMODES:
        if mode.id == subId:
            return mode
    return None


def SetActiveHierarchySubMode(state, subId):
    """Record `subId` on `state`; "" clears. Returns the status line."""
    if subId == "":
        state.hierarchySubMode = ""
        return "Tonic Hierarchy: no sub-mode."
    mode = HierarchySubModeById(subId)
    if mode is None:
        return ""
    state.hierarchySubMode = mode.id
    return "Tonic Hierarchy: %s" % mode.status


def SetActiveSculptSubMode(state, subId):
    """Record `subId` on `state`; "" clears. Returns the status line."""
    if subId == "":
        state.sculptSubMode = ""
        return "Tonic Sculpt: no sub-mode."
    mode = SculptSubModeById(subId)
    if mode is None:
        return ""
    state.sculptSubMode = mode.id
    return "Tonic Sculpt: %s" % mode.status


def SetActiveMode(state, modeId):
    """Record `modeId` on `state`; "" clears. Returns the status line.

    Unknown ids are rejected (empty status, state untouched) so a stale menu
    callback can never wedge the shelf.
    """
    if modeId == "":
        state.activeMode = ""
        return "Tonic: no mode."
    mode = ModeById(modeId)
    if mode is None:
        return ""
    state.activeMode = mode.id
    return "Tonic: %s" % mode.status


# ---------------------------------------------------------------------------
# The hotkey table (plan/18 section 3.4)
#
# Qt-free on purpose. `key` is a lower-case spelling of one key ("1", "d",
# "escape", "delete", "backspace", "up", "down", "[", "]"); `modifiers` is a
# set drawn from {"shift", "ctrl", "alt", "meta"}. The answer is an
# (action, argument) pair the controller executes, or None for "not ours,
# let usdview have it".
#
# Two constraints come from usdview rather than from taste (plan/08 section
# 3.5): `Alt` and `Meta` always belong to the camera, and a letter is only
# ours while the pointer is over the viewport and no text field has focus,
# because usdview's AppEventFilter refocuses on every mouse move.
# ---------------------------------------------------------------------------

MODE_KEYS = {mode.hotkey: mode.id for mode in MODES}

ACTION_MODE = "mode"
ACTION_SUBMODE = "subMode"
ACTION_CANCEL = "cancel"
ACTION_UNDO = "undo"
ACTION_REDO = "redo"
ACTION_DELETE = "delete"
ACTION_RADIUS = "radius"
ACTION_SUBDIVIDE = "subdivide"
ACTION_MERGE = "mergeChildren"
ACTION_ENTER_LEVEL = "enterLevel"
ACTION_EXIT_LEVEL = "exitLevel"
ACTION_WELD = "weld"
ACTION_UNWELD = "unweld"
ACTION_SAVE = "saveGroom"

_LETTERS = frozenset("abcdefghijklmnopqrstuvwxyz")


def HotkeyAction(key, modifiers=frozenset(), pointerInside=True,
                 textFocus=False):
    """The (action, argument) for one key press, or None.

    `pointerInside` is whether the cursor is over the viewport and
    `textFocus` whether a text widget holds focus; both gate the keys that
    would otherwise steal typing.
    """
    key = str(key).lower()
    mods = frozenset(modifiers)
    if "alt" in mods or "meta" in mods:
        return None                      # the camera's modifiers, always
    shift = "shift" in mods
    ctrl = "ctrl" in mods
    if key == "escape":
        return (ACTION_CANCEL, None)
    if textFocus:
        return None
    if ctrl and shift and key == "s":
        return (ACTION_SAVE, None)
    if ctrl and not shift:
        if key == "z":
            return (ACTION_UNDO, None)
        if key == "y":
            return (ACTION_REDO, None)
        if key == "down":
            return (ACTION_ENTER_LEVEL, None)
        if key == "up":
            return (ACTION_EXIT_LEVEL, None)
        return None
    if ctrl:
        return None
    if shift:
        if key == "d":
            return (ACTION_SUBDIVIDE, None)
        if key == "m":
            return (ACTION_MERGE, None)
        if key == "w":
            return (ACTION_WELD, None)
        if key == "u":
            return (ACTION_UNWELD, None)
        return None
    if key == "backspace":
        return (ACTION_EXIT_LEVEL, None)
    if key == "delete":
        return (ACTION_DELETE, None)
    if key in MODE_KEYS:
        return (ACTION_MODE, MODE_KEYS[key])
    if not pointerInside:
        return None                      # letters and brackets need the view
    if key == "[":
        return (ACTION_RADIUS, -1.0)
    if key == "]":
        return (ACTION_RADIUS, 1.0)
    if key in _LETTERS:
        return (ACTION_SUBMODE, key.upper())
    return None
