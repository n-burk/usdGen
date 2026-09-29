# usdGenPomadeTools.pomadeModes -- Qt-free mode shelf and hotkey table
# (plan/17 section 5, plan/18 section 3.4).
#
# Modes are a shelf; each mode has one tool loop, one gizmo family, one
# hotkey set. Selecting a mode records it on PomadeToolState and reports the
# mode's status line. HotkeyAction at the foot of this file is the whole of
# the key table: pomadeViewport only normalises a QKeyEvent into the strings
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
         "Fill mode: guide density and the length ramp."),
    Mode("hierarchy", "Hierarchy", "4",
         "Hierarchy mode: subdivide and navigate levels."),
    Mode("sculpt",    "Sculpt",    "5",
         "Sculpt mode: hierarchical brushes."),
    Mode("output",    "Output",    "6",
         "Output mode: build the hair description, bake resolution, "
         "strand density and width."),
)

# Graph-mode sub-modes (plan/17 section 5.1, P2): one click/drag behaviour
# each, all driving the same C++ graph through pomadeLib.
GRAPH_SUBMODES = (
    Mode("region",  "Create region", "R",
         "Create region: click scalp CVs; click the first CV or Enter to close."),
    Mode("draw",    "Draw",    "D",
         "Draw: stroke regions onto the scalp (welds at snapped ends)."),
    Mode("place",   "Place",   "P",
         "Place: click adds a node, drag moves (welds onto nodes/edges)."),
    Mode("reposition", "Reposition", "M",
         "Reposition: drag an existing CV or whole edge along the scalp; "
         "empty clicks do not create or weld."),
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

# Tube-mode sub-modes (plan/17 section 5.2, P3): the component kind the
# viewport picks. Their hotkeys are the F8--F11 row: Q/W/E/R belong to the
# transform tool (pomadeViewport._runTubeShortcut eats them before the
# letter table), so the C/R/E letters this table used to print never
# reached a sub-mode and the dock advertised keys that did something else.
TUBE_SUBMODES = (
    Mode("tube",    "Whole tube", "F8",
         "Whole tube: select and transform whole tubes."),
    Mode("center",  "Center CV",  "F9",
         "Center CV: select and transform centre-curve CVs."),
    Mode("ring",    "Ring",       "F10",
         "Ring: select and transform whole section rings."),
    Mode("section", "Section CV", "F11",
         "Section CV: select and move CVs within a ring."),
)

# Fill-mode sub-modes (plan/17 section 5.3, P3). The `preview` id is kept
# for state and scripts; what the artist does in it is edit the length
# ramp of the tube under the cursor -- keyed by the radial position of the
# roots there, not by t along the tube -- so that is what it is called.
FILL_SUBMODES = (
    Mode("params",  "Params",  "P",
         "Params: density, CV count, length ramp, edge bias, seed."),
    Mode("preview", "Length ramp", "V",
         "Length ramp: press on a tube or strand, drag up = longer, down = "
         "shorter at that radius (centre 0 .. wall 1, up to full length)"),
)

# Hierarchy-mode sub-modes (plan/17 section 5.4, P4): one tool loop each.
HIERARCHY_SUBMODES = (
    Mode("navigate",  "Navigate",  "N",
         "Navigate: Enter/Exit level, click the breadcrumb."),
    Mode("subdivide", "Subdivide", "D",
         "Subdivide (Shift+D): split into count children (2..8)."),
    Mode("merge",     "Merge",     "M",
         "Merge: click a parent to fold its children back in, or a child "
         "to fold its siblings (Shift+M does the same for the "
         "selection)."),
    Mode("group",     "Group",     "G",
         "Group: drag a box round tubes to group them under a new parent "
         "(a Shift box adds to and a Ctrl box removes from the tubes the "
         "Group button will group; Make persistent keeps it)."),
    Mode("levels",    "Levels",    "L",
         "Levels: click a tube to solo its level, again to un-solo; the "
         "dock shows, hides or sees through levels."),
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

# The dock's per-tool instruction line (DK-02): what the pointer and the
# keys do in the active (mode, sub-mode), in the words the shelf uses.
# Keyed by (mode id, sub-mode id); (mode, "") is the mode-wide line a
# sub-mode without its own falls back to. Kept here, Qt-free, beside the
# hotkey table it describes so a T0 test can hold the two in step.
SELECTION_HINT = ("Click selects · Shift toggles · Ctrl removes · "
                  "Ctrl+Shift adds · drag empty space boxes")

_GRAPH_PAIR_HINT = ("Click two nodes (regions for Link) · Esc cancels the "
                    "first pick")
_GRAPH_NODE_HINT = "Click a node (or edge for Delete)"
_TUBE_HINT = ("Q/W/E/R Select/Move/Rotate/Scale · F8-F11 component kind · "
              "drag a handle to transform · Delete removes CVs/rings, or "
              "whole tubes in Whole tube (F8)")
_HIERARCHY_HINT = ("Click selects · double-click enters children · "
                   "Ctrl+Down/Up or Backspace enter/exit · Shift+D "
                   "subdivide · Q/W/E/R transform in Tube")
_SCULPT_TAIL = " · F+drag or [ ] radius · Esc cancels"

HINTS = {
    ("graph", ""): "Pick a Graph tool: " + " ".join(
        m.hotkey for m in GRAPH_SUBMODES),
    ("graph", "region"): ("Click CVs · click the first CV or Enter closes · "
                          "Backspace removes · Esc cancels"),
    ("graph", "draw"): ("Drag a stroke on the scalp; ends weld within the "
                        "snap radius"),
    ("graph", "place"): "Click adds a node; drag onto a node or edge welds",
    ("graph", "reposition"): "Drag a CV or an edge along the scalp",
    ("graph", "connect"): _GRAPH_PAIR_HINT,
    ("graph", "weld"): _GRAPH_PAIR_HINT,
    ("graph", "link"): _GRAPH_PAIR_HINT,
    ("graph", "unweld"): _GRAPH_NODE_HINT,
    ("graph", "delete"): _GRAPH_NODE_HINT,
    ("tube", ""): _TUBE_HINT,
    ("tube", "tube"): _TUBE_HINT,
    ("tube", "center"): _TUBE_HINT,
    ("tube", "ring"): _TUBE_HINT,
    ("tube", "section"): _TUBE_HINT,
    ("fill", ""): "Click a tube, then set Density · Refill rebuilds guides",
    ("fill", "params"): ("Click a tube, then set Density · Refill rebuilds "
                         "guides"),
    ("fill", "preview"): ("Length ramp: press on a tube or strand, drag up "
                          "= longer, down = shorter at that radius (centre "
                          "0 .. wall 1, up to full length)"),
    ("hierarchy", ""): _HIERARCHY_HINT,
    ("hierarchy", "navigate"): _HIERARCHY_HINT,
    ("hierarchy", "subdivide"): _HIERARCHY_HINT,
    ("hierarchy", "merge"): _HIERARCHY_HINT,
    ("hierarchy", "group"): _HIERARCHY_HINT,
    ("hierarchy", "levels"): _HIERARCHY_HINT,
    ("sculpt", ""): ("Pick a brush: " + " ".join(
        m.hotkey for m in SCULPT_SUBMODES) + _SCULPT_TAIL),
    ("sculpt", "grab"): "Grab: drag to pull CVs" + _SCULPT_TAIL,
    ("sculpt", "smooth"): "Smooth: drag to relax" + _SCULPT_TAIL,
    ("sculpt", "comb"): "Comb: drag along the direction" + _SCULPT_TAIL,
    ("sculpt", "lengthen"): ("Lengthen: drag up = longer, down = shorter"
                             + _SCULPT_TAIL),
    ("sculpt", "twist"): "Twist: drag right/left" + _SCULPT_TAIL,
    # The label is the Output action's own ("Build hair description").
    ("output", ""): ("Build hair description creates the renderable hair "
                     "and unlocks its density and width · Save writes the "
                     "groom .usdc"),
}

# Before a scalp is bound every tool is gated (DK-01): the viewport HUD and
# the dock's instruction line say what to do first instead of naming a tool
# that cannot act yet.
UNBOUND_TITLE = "Bind a scalp mesh to start"
UNBOUND_HINT = ("Bind a scalp mesh to start: select the scalp, then click "
                "Bind scalp mesh... in the Pomade dock")

# The modes whose viewport clicks select something, so the dock shows the
# shared modifier line under their tool hint. Graph's click tools and
# Sculpt's brushes use the click for authoring, not selection, and Output
# has no viewport behaviour at all.
SELECTION_MODES = frozenset(("tube", "fill", "hierarchy"))


def hintFor(mode, subMode=""):
    """The instruction line for (mode, subMode); "" for an unknown mode."""
    mode = str(mode or "")
    subMode = str(subMode or "")
    text = HINTS.get((mode, subMode))
    if text is None:
        text = HINTS.get((mode, ""), "")
    return text

# The mode-shelf / sub-mode-shelf icon glyphs (IC-01, plugin/usdGenPomadeTools
# /resources/icons/README.md has the naming convention). Kept as a plain
# dict of (kind, id) -> icon basename rather than a Mode field so it stays
# Qt-free and additive: pomadeIcons.loadIcon(name) resolves the PNG,
# pomadeIcons.iconFor(kind, id) is the (kind, id) -> QIcon path callers use.
# `kind` is "mode" for MODES and the sub-mode tuple's own name (graph, tube,
# fill, hierarchy, sculpt) for its ids, plus "tool"/"shape"/"orient" for the
# manipulator toolbar (DK-04). Names without a PNG yet resolve to None
# through pomadeIcons until IC-02 lands the rest of the manifest.
ICONS = {
    ("mode", "graph"): "mode_graph",
    ("mode", "tube"): "mode_tube",
    ("mode", "fill"): "mode_fill",
    ("mode", "hierarchy"): "mode_hierarchy",
    ("mode", "sculpt"): "mode_sculpt",
    ("mode", "output"): "mode_output",

    ("graph", "region"): "sub_graph_region",
    ("graph", "draw"): "sub_graph_draw",
    ("graph", "place"): "sub_graph_place",
    ("graph", "reposition"): "sub_graph_reposition",
    ("graph", "connect"): "sub_graph_connect",
    ("graph", "weld"): "sub_graph_weld",
    ("graph", "unweld"): "sub_graph_unweld",
    ("graph", "delete"): "sub_graph_delete",
    ("graph", "link"): "sub_graph_link",

    ("tube", "tube"): "comp_tube",
    ("tube", "center"): "comp_center_cv",
    ("tube", "ring"): "comp_ring",
    ("tube", "section"): "comp_section_cv",

    ("fill", "params"): "sub_fill_params",
    ("fill", "preview"): "sub_fill_length_ramp",

    ("hierarchy", "navigate"): "sub_hier_navigate",
    ("hierarchy", "subdivide"): "sub_hier_subdivide",
    ("hierarchy", "merge"): "sub_hier_merge",
    ("hierarchy", "group"): "sub_hier_group",
    ("hierarchy", "levels"): "sub_hier_levels",

    ("sculpt", "grab"): "sub_sculpt_grab",
    ("sculpt", "smooth"): "sub_sculpt_smooth",
    ("sculpt", "comb"): "sub_sculpt_comb",
    ("sculpt", "lengthen"): "sub_sculpt_lengthen",
    ("sculpt", "twist"): "sub_sculpt_twist",

    ("tool", "select"): "tool_select",
    ("tool", "move"): "tool_move",
    ("tool", "rotate"): "tool_rotate",
    ("tool", "scale"): "tool_scale",

    ("shape", "box"): "shape_box",
    ("shape", "lasso"): "shape_lasso",

    ("orient", "world"): "orient_world",
    ("orient", "screen"): "orient_screen",
    ("orient", "tube"): "orient_tube",
}

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
        return "Pomade Graph: no sub-mode."
    mode = GraphSubModeById(subId)
    if mode is None:
        return ""
    state.graphSubMode = mode.id
    return "Pomade Graph: %s" % mode.status


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
        return "Pomade Tube: no sub-mode."
    mode = TubeSubModeById(subId)
    if mode is None:
        return ""
    state.tubeSubMode = mode.id
    return "Pomade Tube: %s" % mode.status


def SetActiveFillSubMode(state, subId):
    """Record `subId` on `state`; "" clears. Returns the status line."""
    if subId == "":
        state.fillSubMode = ""
        return "Pomade Fill: no sub-mode."
    mode = FillSubModeById(subId)
    if mode is None:
        return ""
    state.fillSubMode = mode.id
    return "Pomade Fill: %s" % mode.status


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
        return "Pomade Hierarchy: no sub-mode."
    mode = HierarchySubModeById(subId)
    if mode is None:
        return ""
    state.hierarchySubMode = mode.id
    return "Pomade Hierarchy: %s" % mode.status


def SetActiveSculptSubMode(state, subId):
    """Record `subId` on `state`; "" clears. Returns the status line."""
    if subId == "":
        state.sculptSubMode = ""
        return "Pomade Sculpt: no sub-mode."
    mode = SculptSubModeById(subId)
    if mode is None:
        return ""
    state.sculptSubMode = mode.id
    return "Pomade Sculpt: %s" % mode.status


def SetActiveMode(state, modeId):
    """Record `modeId` on `state`; "" clears. Returns the status line.

    Unknown ids are rejected (empty status, state untouched) so a stale menu
    callback can never wedge the shelf.
    """
    if modeId == "":
        state.activeMode = ""
        return "Pomade: no mode."
    mode = ModeById(modeId)
    if mode is None:
        return ""
    state.activeMode = mode.id
    return "Pomade: %s" % mode.status


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
ACTION_COMPLETE = "complete"
ACTION_BACKSPACE = "backspace"
# SL-03: Ctrl+A / Ctrl+Shift+A / Ctrl+I over the active mode's selectable
# kinds on the focused level; the values are the loop method names.
ACTION_SELECT_ALL = "selectAll"
ACTION_DESELECT_ALL = "deselectAll"
ACTION_INVERT = "invertSelection"

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
    # A focused field owns every key, Escape included: it reverts the
    # typing there, and must not also cancel (or deselect) in the viewport.
    if textFocus:
        return None
    if key == "escape":
        return (ACTION_CANCEL, None)
    if ctrl and shift and key == "s":
        return (ACTION_SAVE, None)
    # Selection commands act on what the artist is looking at; off the
    # viewport a dock list keeps its own Ctrl+A.
    if ctrl and key in ("a", "i") and pointerInside:
        if shift and key == "a":
            return (ACTION_DESELECT_ALL, None)
        if not shift:
            return (ACTION_SELECT_ALL if key == "a" else ACTION_INVERT,
                    None)
    # Parity G14: redo answers Ctrl+Shift+Z and Shift+Z as well as Ctrl+Y,
    # the three RigExec (and a DCC) bind.
    if shift and key == "z":
        return (ACTION_REDO, None)
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
    if key == "enter":
        return (ACTION_COMPLETE, None)
    if key in MODE_KEYS:
        return (ACTION_MODE, MODE_KEYS[key])
    if not pointerInside:
        # Letters and brackets need the view, and so do the destructive
        # keys: Delete/Backspace with the pointer over a dock tree or the
        # outliner belong to that widget, not to the Pomade selection.
        return None
    if key == "backspace":
        return (ACTION_BACKSPACE, None)
    if key == "delete":
        return (ACTION_DELETE, None)
    if key == "[":
        return (ACTION_RADIUS, -1.0)
    if key == "]":
        return (ACTION_RADIUS, 1.0)
    if key in _LETTERS:
        return (ACTION_SUBMODE, key.upper())
    return None
