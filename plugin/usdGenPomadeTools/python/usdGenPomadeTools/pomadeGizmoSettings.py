# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
#
# usdGenPomadeTools.pomadeGizmoSettings -- the per-tool transform settings
# behind the Tube panel's gizmo rows.
#
# Adapted for this groom tool. Qt-free and pxr-free: the
# T0/T1 tests import it without either, and the dock, the viewport keys and
# the drag code all read the same fields, so a typo is an AttributeError
# rather than a silent wrong default.
#
# What changed in the port, and why:
#
#   * The tool tokens are Pomade's own spelling (pomadeGizmoScreen's
#     TOOL_TRANSLATE/ROTATE/SCALE plus "select"), and `ToolFor` maps the
#     Tube loop's "move"/"rotate"/"scale" transform tool onto them.
#   * Axis Orientation is per tool, as in RigExec (parity G16): Move and
#     Scale start in World, Rotate in Tube (RigExec's "Object").  The LIVE
#     tool's value is PomadeToolState.transformOrientation -- the one field
#     the dock row, `L` and the gizmo read -- and SwitchTool banks it per
#     tool when the transform tool changes, then restores the new tool's.
#     So there is still one row and one `L`, and each tool remembers its
#     own answer.
#   * Group pivot (parity G17) offers Individual Origins and Selection
#     Centre for Rotate/Scale only.  Individual is Pomade's default, not
#     RigExec's Centre: a strand turns about its root on the scalp, and a
#     centre pivot would lift every selected root off it.  Last Selected is
#     not offered (Pomade's selection has no order).
#   * Dropped: preserveChildren, snapMode (point/edge/surface snapping is
#     rig-geometry specific, parity G12), gimbal/parent orientations.
#   * Scale's step defaults to 0.1, not 1.0. Pomade has no scale channel to
#     quantise; the step applies to the drag's factor itself, and a whole
#     number step would jump a tube from 1x straight to 2x.
#   * GizmoSettings gains RemoveListener: the viewport controller listens
#     while it is installed and must let go when it uninstalls.
from __future__ import annotations

try:
    from . import pomadeGizmoScreen
except ImportError:  # file-path test load (pomadePanels' bare-name fallback)
    import pomadeGizmoScreen

TOOL_SELECT = "select"
TOOL_TRANSLATE = pomadeGizmoScreen.TOOL_TRANSLATE
TOOL_ROTATE = pomadeGizmoScreen.TOOL_ROTATE
TOOL_SCALE = pomadeGizmoScreen.TOOL_SCALE

TOOLS = (TOOL_SELECT, TOOL_TRANSLATE, TOOL_ROTATE, TOOL_SCALE)

# The Tube loop's transform tool names ("move" is what Q/W/E/R and the
# dock row say) -> the settings tokens.
_TUBE_TOOLS = {"select": TOOL_SELECT, "move": TOOL_TRANSLATE,
               "rotate": TOOL_ROTATE, "scale": TOOL_SCALE}

# Pomade's Axis Orientation choices (GZ-05, parity G16).  World is the
# identity frame; Screen lines the handles up with the camera plane (the
# frame Pomade's Centre gizmo always used before this option existed); Tube
# is the owner tube's own frame, w along its root normal -- RigExec's
# "Object".  The section chart of a Ring/Section edit is two dimensional,
# so those sub-modes keep the ring's frame whatever is chosen here.
ORIENT_WORLD = "world"
ORIENT_SCREEN = "screen"
ORIENT_TUBE = "tube"
ORIENTATIONS = (ORIENT_WORLD, ORIENT_SCREEN, ORIENT_TUBE)

# The two the `L` key flips between, in the order it flips them: the pair
# an artist switches all day (RigExec ORIENT_TOGGLE, World <-> Object).
ORIENT_TOGGLE = (ORIENT_WORLD, ORIENT_TUBE)

_ORIENT_LABELS = {
    ORIENT_WORLD: "World",
    ORIENT_SCREEN: "Screen",
    ORIENT_TUBE: "Tube (local)",
}

# What the toolbar/status line calls the toggle pair (RigExec _TOGGLE_LABELS).
_TOGGLE_LABELS = {
    ORIENT_WORLD: "Global",
    ORIENT_TUBE: "Local",
}

# The manipulator's on-screen size, in LOGICAL pixels (RigExec design spec
# 8.1).  The bounds exist because '+' / '-' scale by 10% without an operator
# watching: an unbounded shrink walks the handles down to a point that can
# no longer be grabbed to grow them back.
MANIPULATOR_SIZE_DEFAULT = pomadeGizmoScreen.GIZMO_PIXELS
MANIPULATOR_SIZE_MIN = 20.0
MANIPULATOR_SIZE_MAX = 400.0
# What one '+' / '-' press multiplies the size by.
MANIPULATOR_SIZE_STEP = 1.1

# The world grid spacing for the `X` hold, in world units.  Session-wide
# beside manipulatorSize, not per tool, so Reset(tool) leaves the grid where
# it was: resetting Move must not move the world grid.
GRID_SIZE_DEFAULT = 1.0
GRID_SIZE_MIN = 1e-4
GRID_SIZE_MAX = 1e5

# The fields every ToolSettings carries.  Every tool carries all of them
# even where only one tool shows the row (Free Rotate is Rotate's, Prevent
# Negative Scale is Scale's), so the drag code can read
# settings.freeRotate without first asking which tool it belongs to.
_FIELDS = ("stepSnap", "stepSize", "freeRotate", "preventNegativeScale",
           "groupPivot")

# Group pivot (parity G17): what a MULTI-selection turns and scales about.
# Individual Origins turns each owner (a tube, a ring) about its own pivot
# -- the root core, the ring centroid -- with the gizmo drawn at the lead
# owner's; Selection Centre turns everything about ONE point, the middle
# of the selection bounds, which is where the gizmo is then drawn.
GROUP_PIVOT_INDIVIDUAL = "individual"
GROUP_PIVOT_CENTRE = "centre"
GROUP_PIVOTS = (GROUP_PIVOT_INDIVIDUAL, GROUP_PIVOT_CENTRE)

_GROUP_PIVOT_LABELS = {
    GROUP_PIVOT_INDIVIDUAL: "Individual Origins",
    GROUP_PIVOT_CENTRE: "Selection Centre",
}


def ToolFor(transformTool):
    """The settings token for a Tube transform tool ("move" -> translate)."""
    name = str(transformTool).lower()
    return _TUBE_TOOLS.get(name, name)


def OrientationLabel(orientation):
    """The menu text for an orientation token."""
    return _ORIENT_LABELS.get(orientation, str(orientation).title())


def ToggleLabel(orientation):
    """The toggle's word for an orientation token."""
    return _TOGGLE_LABELS.get(orientation, OrientationLabel(orientation))


def NormalizeOrientation(orientation):
    """A known orientation token; anything else is World."""
    value = str(orientation).lower()
    return value if value in ORIENTATIONS else ORIENT_WORLD


def NextToggleOrientation(orientation):
    """
    What `L` moves to from `orientation`.

    Anything outside the pair -- Screen, a value the panel set -- goes to
    World, so the toggle always has somewhere to go and always lands
    somewhere an artist can name.
    """
    if orientation == ORIENT_WORLD:
        return ORIENT_TUBE
    return ORIENT_WORLD


def DefaultOrientation(tool):
    """A tool's starting Axis Orientation: Rotate Tube, the rest World.

    RigExec's per-tool defaults (Move/Scale World, Rotate Object): a strand
    is bent about its own direction far more often than about world Y.
    """
    return ORIENT_TUBE if ToolFor(tool) == TOOL_ROTATE else ORIENT_WORLD


def SwitchTool(state, tool):
    """Make `tool` the live transform tool, carrying orientations per tool.

    The outgoing tool's PomadeToolState.transformOrientation is banked in
    the state's GizmoSettings and the incoming tool's is restored (its
    DefaultOrientation the first time).  Setting the tool it already has
    changes nothing, so the dock's descriptor write and the loop's
    setTransformTool can both call this for one click.
    """
    new = str(tool).lower()
    old = str(getattr(state, "transformTool", "move")).lower()
    if new == old:
        return False
    settings = settingsFor(state)
    bank = settings._orientations
    bank[ToolFor(old)] = NormalizeOrientation(
        getattr(state, "transformOrientation", ORIENT_WORLD))
    state.transformTool = new
    state.transformOrientation = bank.get(ToolFor(new),
                                          DefaultOrientation(new))
    return True


def GroupPivotLabel(mode):
    """The menu/tooltip text for a group pivot token."""
    return _GROUP_PIVOT_LABELS.get(mode, str(mode).title())


def GroupPivotChoices(tool):
    """The group pivots `tool` offers, default first; empty for Move/Select.

    A Move takes every selected item by the same delta, so where it
    "pivots" makes no difference and the button greys out.
    """
    if ToolFor(tool) in (TOOL_ROTATE, TOOL_SCALE):
        return GROUP_PIVOTS
    return ()


def NextGroupPivot(mode, tool):
    """What `P` moves `tool`'s group pivot to (wrapping); `mode` if none."""
    choices = GroupPivotChoices(tool)
    if not choices:
        return mode
    try:
        return choices[(choices.index(mode) + 1) % len(choices)]
    except ValueError:
        return choices[0]


class ToolSettings(object):
    """
    One tool's options.

    Assigning a field notifies the owning GizmoSettings, which is how a
    dock checkbox reaches the viewport without either knowing about the
    other.  Writing a field the value it already holds is silent: the
    controller re-places the gizmo on every notification and the dock
    refreshes its widgets from the model, so a self-notifying write would
    be an endless loop the first time a widget re-emitted its value.
    """

    def __init__(self, tool, owner=None, **values):
        object.__setattr__(self, "tool", tool)
        object.__setattr__(self, "_owner", owner)
        for name in _FIELDS:
            object.__setattr__(self, name, values[name])

    def __setattr__(self, name, value):
        if name in _FIELDS:
            if getattr(self, name) == value:
                return
            object.__setattr__(self, name, value)
            owner = getattr(self, "_owner", None)
            if owner is not None:
                owner.Notify()
            return
        object.__setattr__(self, name, value)

    def CopyFrom(self, other):
        """
        Take every field from `other` in one notification.

        Reset() goes through this rather than replacing the object: the
        dock and the drag code hold references to the ToolSettings for
        their tool, and swapping it out from under them would leave them
        writing into an orphan.
        """
        changed = False
        for name in _FIELDS:
            value = getattr(other, name)
            if getattr(self, name) != value:
                object.__setattr__(self, name, value)
                changed = True
        if changed:
            owner = getattr(self, "_owner", None)
            if owner is not None:
                owner.Notify()
        return changed

    def __repr__(self):
        return "<ToolSettings %s %s>" % (
            self.tool, " ".join("%s=%s" % (n, getattr(self, n))
                                for n in _FIELDS))


def ToolDefaults(tool, owner=None):
    """A fresh ToolSettings carrying the defaults for `tool`."""
    if tool == TOOL_ROTATE:
        stepSize = 15.0
    elif tool == TOOL_SCALE:
        stepSize = 0.1
    else:
        stepSize = 1.0
    values = {
        "stepSnap": False,
        "stepSize": stepSize,
        "freeRotate": True,
        "preventNegativeScale": False,
        # Every tool carries the field; only Rotate/Scale offer a choice
        # (GroupPivotChoices).  See the module note for why not Centre.
        "groupPivot": GROUP_PIVOT_INDIVIDUAL,
    }
    return ToolSettings(tool, owner, **values)


class GizmoSettings(object):
    """
    Every tool's options plus the session-wide manipulator size and grid,
    with a listener list the viewport controller uses to re-place the gizmo.

    Session only: nothing here is written to disk, matching RigExec.
    """

    def __init__(self):
        object.__setattr__(self, "_listeners", [])
        object.__setattr__(
            self, "_tools", dict((t, ToolDefaults(t, self)) for t in TOOLS))
        object.__setattr__(self, "manipulatorSize", MANIPULATOR_SIZE_DEFAULT)
        object.__setattr__(self, "gridSize", GRID_SIZE_DEFAULT)
        # Settings-token -> the Axis Orientation that tool last had while
        # another tool was live (SwitchTool).  The live tool's own value is
        # PomadeToolState.transformOrientation, never kept here as well.
        object.__setattr__(self, "_orientations", {})

    def __setattr__(self, name, value):
        if name == "manipulatorSize":
            value = max(MANIPULATOR_SIZE_MIN,
                        min(MANIPULATOR_SIZE_MAX, float(value)))
            if value == self.manipulatorSize:
                return
            object.__setattr__(self, name, value)
            self.Notify()
            return
        if name == "gridSize":
            value = max(GRID_SIZE_MIN,
                        min(GRID_SIZE_MAX, float(value)))
            if value == self.gridSize:
                return
            object.__setattr__(self, name, value)
            self.Notify()
            return
        object.__setattr__(self, name, value)

    def For(self, tool):
        """
        The live ToolSettings for `tool` (a settings token or a Tube tool
        name), created on demand.

        An unknown tool gets a plain default set rather than a KeyError: a
        tool added later must not be able to crash the dock or a drag.
        """
        tool = ToolFor(tool)
        settings = self._tools.get(tool)
        if settings is None:
            settings = ToolDefaults(tool, self)
            self._tools[tool] = settings
        return settings

    def Reset(self, tool):
        """Restore the defaults for one tool, in place."""
        tool = ToolFor(tool)
        return self.For(tool).CopyFrom(ToolDefaults(tool))

    def ScaleManipulator(self, factor):
        """
        Grow or shrink the manipulator ('+' / '-').
        Returns the size actually taken, after clamping.
        """
        self.manipulatorSize = self.manipulatorSize * factor
        return self.manipulatorSize

    def AddListener(self, fn):
        if fn not in self._listeners:
            self._listeners.append(fn)

    def RemoveListener(self, fn):
        try:
            self._listeners.remove(fn)
        except ValueError:
            pass

    def Notify(self):
        for fn in list(self._listeners):
            fn()


def settingsFor(state):
    """The GizmoSettings a PomadeToolState carries, created on first use.

    PomadeToolState keeps the field as a plain None default so it stays a
    dependency-free dataclass (several tests load it as a bare file); every
    reader goes through here instead of touching the field directly.
    """
    settings = getattr(state, "gizmoSettings", None)
    if settings is None:
        settings = GizmoSettings()
        try:
            state.gizmoSettings = settings
        except AttributeError:
            pass
    return settings
