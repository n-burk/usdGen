# usdGenPomadeTools.pomadeDockIds -- Qt-free names for the dock's buttons
# (DK-04).
#
# Every button the Pomade dock builds carries a stable objectName
# "<prefix>:<id>" and is reachable as workspace.button(kind, id). The T3
# scripts used to find dock controls by matching button.text(), which an
# icon-only button (or a relabel) silently breaks; a (kind, id) key does
# not change when the art or the wording does. The table lives here,
# away from pomadeWorkspace's Qt import, so the T1 panels test can hold it
# to the mode and action tables without a display.
from __future__ import annotations

try:
    from . import pomadeModes
except ImportError:          # loaded by file path (T0/T1 tests)
    import pomadeModes

# kind -> objectName prefix. "sub" is a mode's sub-mode shelf, "comp" the
# Tube F8-F11 component row, "tool" the Q/W/E/R transform row, "action" a
# mode's one-shot buttons, "file" the always-visible header (bind,
# resume, undo/redo, save/export/import, settings) and "gizmo" the
# Global/Local and group-pivot toggles at the end of the transform row.
PREFIXES = {
    "mode": "pomadeMode",
    "sub": "pomadeSub",
    "comp": "pomadeComp",
    "tool": "pomadeTool",
    "action": "pomadeAction",
    "file": "pomadeFile",
    "gizmo": "pomadeGizmoToggle",
}

# Parity G21: the transform row's two toggles after Q/W/E/R, (id, hotkey).
# "orientation" flips the live tool's Axis Orientation World <-> Tube (L);
# "groupPivot" cycles Rotate/Scale's Individual / Centre pivot (P).
GIZMO_TOGGLES = (("orientation", "L"), ("groupPivot", "P"))

# The glyph each toggle shows for the state it is in (the dock swaps the
# icon on refresh); keyed by pomadeGizmoSettings' tokens.
ORIENT_ICONS = {
    "world": "orient_world",
    "screen": "orient_screen",
    "tube": "orient_tube",
}
PIVOT_ICONS = {
    "individual": "pivot_each",
    "centre": "pivot_centre",
}
# What the toggles show before the dock has read the state: the defaults.
_GIZMO_DEFAULT_ICONS = {
    "orientation": ORIENT_ICONS["world"],
    "groupPivot": PIVOT_ICONS["individual"],
}

# The transform row, in usdRig/a DCC order. Mode-shaped so the dock can
# treat it like any other shelf; the status text is the tooltip body and
# names the gesture, not just the tool (the usdRig toolbar's wording).
TRANSFORM_TOOLS = (
    pomadeModes.Mode("select", "Select", "Q",
                    "no manipulator; click and drag only select."),
    pomadeModes.Mode("move", "Move", "W",
                    "drag an axis, a planar square or the centre."),
    pomadeModes.Mode("rotate", "Rotate", "E",
                    "drag a ring to turn the selection about its pivot."),
    pomadeModes.Mode("scale", "Scale", "R",
                    "drag an axis cube for one axis, the centre for "
                    "uniform."),
)

# Modes whose dock shows the transform row. Hierarchy's Q/W/E/R keys jump to
# Tube with the selected tubes (pomadeViewport._runTubeShortcut), so the
# row there makes the same jump.
TRANSFORM_MODES = ("tube", "hierarchy")

# The header buttons, in the order they are laid out.
FILE_IDS = ("bind", "resume", "undo", "redo", "save", "export", "import",
            "settings")

# Header id -> icon manifest name (pomadePanels.ACTION_ICONS keys by
# command id; these are the dock's own ids for the same commands).
FILE_ICONS = {
    "bind": "act_bind_scalp",
    "resume": "act_resume_groom",
    "undo": "act_undo",
    "redo": "act_redo",
    "save": "act_save",
    "export": "act_export",
    "import": "act_import",
    "settings": "act_settings",
}

# Hierarchy actions that reuse their sub-mode's glyph rather than carry an
# entry of their own in pomadePanels.ACTION_ICONS (see the note there).
_ACTION_SUBMODE_ICONS = {
    "subdivide": ("hierarchy", "subdivide"),
    "mergeChildren": ("hierarchy", "merge"),
    "group": ("hierarchy", "group"),
}

# The sub-mode tuple behind each mode's "sub" shelf. Tube's F8-F11 row is
# the "comp" kind instead (its sub-mode shelf is kept hidden, so it gets
# no objectName of its own).
SUBMODES = {
    "graph": pomadeModes.GRAPH_SUBMODES,
    "fill": pomadeModes.FILL_SUBMODES,
    "hierarchy": pomadeModes.HIERARCHY_SUBMODES,
    "sculpt": pomadeModes.SCULPT_SUBMODES,
}


def objectName(kind, itemId):
    """"<prefix>:<id>" for one dock button; KeyError on an unknown kind."""
    return "%s:%s" % (PREFIXES[kind], itemId)


def parseObjectName(name):
    """(kind, id) for a dock objectName, or None."""
    prefix, sep, itemId = str(name).partition(":")
    if not sep or not itemId:
        return None
    for kind, known in PREFIXES.items():
        if known == prefix:
            return (kind, itemId)
    return None


def iconName(kind, itemId, actionIcons=None):
    """The icon manifest name for a dock button, or None.

    `actionIcons` is pomadePanels.ACTION_ICONS; passed in rather than
    imported because pomadePanels pulls in every mode's helper module."""
    if kind == "mode":
        return pomadeModes.ICONS.get(("mode", itemId))
    if kind == "sub":
        for modeId, subs in SUBMODES.items():
            if any(sub.id == itemId for sub in subs):
                return pomadeModes.ICONS.get((modeId, itemId))
        return None
    if kind == "comp":
        return pomadeModes.ICONS.get(("tube", itemId))
    if kind == "tool":
        return pomadeModes.ICONS.get(("tool", itemId))
    if kind == "file":
        return FILE_ICONS.get(itemId)
    if kind == "gizmo":
        return _GIZMO_DEFAULT_ICONS.get(itemId)
    if kind == "action":
        name = (actionIcons or {}).get(itemId)
        if name is None and itemId in _ACTION_SUBMODE_ICONS:
            name = pomadeModes.ICONS.get(_ACTION_SUBMODE_ICONS[itemId])
        return name
    return None


def dockButtonKeys(actionsFor):
    """Every (kind, id) the dock builds, in build order.

    `actionsFor(modeId)` is pomadePanels.actions. A duplicate in the result
    would be two buttons behind one objectName, which button() could not
    tell apart; the T1 panels test holds this list to be duplicate-free."""
    keys = [("file", fileId) for fileId in FILE_IDS]
    keys.extend(("mode", mode.id) for mode in pomadeModes.MODES)
    for subs in SUBMODES.values():
        keys.extend(("sub", sub.id) for sub in subs)
    keys.extend(("comp", sub.id) for sub in pomadeModes.TUBE_SUBMODES)
    keys.extend(("tool", tool.id) for tool in TRANSFORM_TOOLS)
    keys.extend(("gizmo", toggleId) for toggleId, _key in GIZMO_TOGGLES)
    for mode in pomadeModes.MODES:
        keys.extend(("action", action.id) for action in actionsFor(mode.id))
    return keys
