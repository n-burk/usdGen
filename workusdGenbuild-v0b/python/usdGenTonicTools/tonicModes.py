# usdGenTonicTools.tonicModes -- Qt-free mode shelf (plan/17 section 5, P0).
#
# Modes are a shelf; each mode has one tool loop, one gizmo family, one hotkey
# set. P0 registers the shelf with empty modes: selecting a mode records it in
# TonicToolState and reports the mode's status line. Gizmos and loops land
# with P2-P5.
from __future__ import annotations

import collections


Mode = collections.namedtuple("Mode", ("id", "label", "hotkey", "status"))

MODES = (
    Mode("graph",     "Graph",     "G",
         "Graph mode: draw regions on the scalp."),
    Mode("tube",      "Tube",      "T",
         "Tube mode: sculpt center curves and sections."),
    Mode("fill",      "Fill",      "F",
         "Fill mode: guide density and preview."),
    Mode("hierarchy", "Hierarchy", "H",
         "Hierarchy mode (P4): subdivide and navigate levels."),
    Mode("sculpt",    "Sculpt",    "S",
         "Sculpt mode (P4): hierarchical brushes."),
    Mode("output",    "Output",    "O",
         "Output panel (P5): maps, bake, save groom."),
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
