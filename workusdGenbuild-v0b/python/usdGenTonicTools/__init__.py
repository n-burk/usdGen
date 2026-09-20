# usdGenTonicTools -- usdview plugin container for the Tonic authoring tool.
#
# Discovered by usdview through plugin/usdGenTonicTools/resources/plugInfo.json
# (Type "python"); this package must be importable, which the launchers
# arrange by putting build/python on PYTHONPATH. Everything Qt-dependent is
# imported lazily from the command callbacks so that importing the package
# in a non-GUI process (tests, headless tools) stays cheap and safe.
#
# P0 registers a sibling "usdGen -> Tonic" menu and a mode shelf whose modes
# are empty (plan/17 section 5, phase P0): selecting one records it on the
# shared TonicToolState. Both plugins share the UsdGenToolState conventions
# from plan/08-tools.md section 1.3 (see tonicToolState.py).

from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer

from .tonicModes import GRAPH_SUBMODES, MODES, SetActiveMode
from .tonicModes import HIERARCHY_SUBMODES, SCULPT_SUBMODES
from .tonicModes import SetActiveGraphSubMode
from .tonicModes import SetActiveHierarchySubMode, SetActiveSculptSubMode
from .tonicToolState import TonicToolState


def _CommandId(modeId):
    return "usdGenTonicTools.setMode_%s" % modeId


def _GraphCommandId(subId):
    return "usdGenTonicTools.graph_%s" % subId


def _HierarchyCommandId(subId):
    return "usdGenTonicTools.hierarchy_%s" % subId


def _SculptCommandId(subId):
    return "usdGenTonicTools.sculpt_%s" % subId


# One-shot actions live beside the sub-mode tool loops in usdview's flat
# command namespace, so they take an Action infix: without it an action
# that shares a sub-mode's id (hierarchy subdivide/group) collides and
# usdview refuses to load the plugins. Dict keys carry the same infix.
def _GraphActionId(actionId):
    return "usdGenTonicTools.graphAction_%s" % actionId


def _HierarchyActionId(actionId):
    return "usdGenTonicTools.hierarchyAction_%s" % actionId


def _SculptActionId(actionId):
    return "usdGenTonicTools.sculptAction_%s" % actionId


def _ActionKey(actionId):
    return "action:" + actionId


class UsdGenTonicToolsPluginContainer(PluginContainer):
    """Registers the usdGen -> Tonic menu, mode commands and Graph shelf."""

    def __init__(self):
        super(UsdGenTonicToolsPluginContainer, self).__init__()
        # The whole of the plugin's mutable state, built here and never
        # assigned during registerPlugins (the 08-tools.md section 1.3 rule).
        self._state = TonicToolState()
        self._modeCommands = {}
        self._graphCommands = {}
        self._hierarchyCommands = {}
        self._sculptCommands = {}
        self._graphController = None

    @property
    def tonicState(self):
        return self._state

    @property
    def graphController(self):
        return self._graphController

    def registerPlugins(self, plugRegistry, plugCtx):
        # Guard the dicts below: a duplicate command id would silently
        # overwrite its handle (usdview only warns on its side).
        seen = set()

        def register(commands, key, commandId, *args):
            if commandId in seen:
                raise ValueError("duplicate tonic command id: %s"
                                 % commandId)
            seen.add(commandId)
            commands[key] = plugRegistry.registerCommandPlugin(
                commandId, *args)

        for mode in MODES:
            register(self._modeCommands, mode.id, _CommandId(mode.id),
                     mode.label, self._makeModeCallback(mode.id),
                     mode.status)
        for sub in GRAPH_SUBMODES:
            register(self._graphCommands, sub.id, _GraphCommandId(sub.id),
                     "Graph: " + sub.label,
                     self._makeGraphSubModeCallback(sub.id), sub.status)
        for actionId, label, status in (
                ("bindScalp", "Graph: Bind scalp (selection)",
                 "Bind the selected mesh as the Tonic scalp."),
                ("weldAll", "Graph: Weld all within radius",
                 "Weld every node pair within the snap radius."),
                ("toggleMirror", "Graph: Mirror-X on/off",
                 "Toggle symmetric node creation across x = 0."),
                ("rebake", "Graph: Rebake map now",
                 "Enqueue a region-map bake of the current graph.")):
            register(self._graphCommands, _ActionKey(actionId),
                     _GraphActionId(actionId), label,
                     self._makeGraphActionCallback(actionId), status)
        for sub in HIERARCHY_SUBMODES:
            register(self._hierarchyCommands, sub.id,
                     _HierarchyCommandId(sub.id),
                     "Hierarchy: " + sub.label,
                     self._makeHierarchySubModeCallback(sub.id),
                     sub.status)
        for actionId, label, status in (
                ("subdivide", "Hierarchy: Subdivide (Shift+D)",
                 "Split the selected tubes into count children."),
                ("mergeChildren", "Hierarchy: Merge children (Shift+M)",
                 "Absorb children's aggregate shape into the parent."),
                ("mergeSelected", "Hierarchy: Merge selected",
                 "Fold the selected siblings into one child."),
                ("enterLevel", "Hierarchy: Enter level (Ctrl+Down)",
                 "Move the editing focus one level down."),
                ("exitLevel", "Hierarchy: Exit level (Ctrl+Up)",
                 "Move the editing focus one level up."),
                ("toggleLockParents", "Hierarchy: Lock parents on/off",
                 "Gate the K7 bottom-up refresh."),
                ("toggleLockChildren", "Hierarchy: Lock children on/off",
                 "Gate the K6 top-down re-derive."),
                ("group", "Hierarchy: Group (on-the-fly parent)",
                 "Build a K7 parent over the selection.")):
            register(self._hierarchyCommands, _ActionKey(actionId),
                     _HierarchyActionId(actionId), label,
                     self._makeHierarchyActionCallback(actionId), status)
        for sub in SCULPT_SUBMODES:
            register(self._sculptCommands, sub.id,
                     _SculptCommandId(sub.id),
                     "Sculpt: " + sub.label,
                     self._makeSculptSubModeCallback(sub.id),
                     sub.status)
        for actionId, label, status in (
                ("togglePreserveLength", "Sculpt: Length-preserving on/off",
                 "Toggle the length-preserving sculpt default."),
                ("toggleMirrorX", "Sculpt: Mirror-X on/off",
                 "Toggle symmetric sculpt strokes across x = 0.")):
            register(self._sculptCommands, _ActionKey(actionId),
                     _SculptActionId(actionId), label,
                     self._makeSculptActionCallback(actionId), status)

    def configureView(self, plugRegistry, plugUIBuilder):
        menu = plugUIBuilder.findOrCreateMenu("usdGen")
        tonicMenu = menu.findOrCreateSubmenu("Tonic")
        for mode in MODES:
            tonicMenu.addItem(self._modeCommands[mode.id])
        graphMenu = tonicMenu.findOrCreateSubmenu("Graph")
        for sub in GRAPH_SUBMODES:
            graphMenu.addItem(self._graphCommands[sub.id])
        for actionId in ("bindScalp", "weldAll", "toggleMirror", "rebake"):
            graphMenu.addItem(self._graphCommands[_ActionKey(actionId)])
        hierarchyMenu = tonicMenu.findOrCreateSubmenu("Hierarchy")
        for sub in HIERARCHY_SUBMODES:
            hierarchyMenu.addItem(self._hierarchyCommands[sub.id])
        for actionId in ("subdivide", "mergeChildren", "mergeSelected",
                         "enterLevel", "exitLevel", "toggleLockParents",
                         "toggleLockChildren", "group"):
            hierarchyMenu.addItem(
                self._hierarchyCommands[_ActionKey(actionId)])
        sculptMenu = tonicMenu.findOrCreateSubmenu("Sculpt")
        for sub in SCULPT_SUBMODES:
            sculptMenu.addItem(self._sculptCommands[sub.id])
        for actionId in ("togglePreserveLength", "toggleMirrorX"):
            sculptMenu.addItem(self._sculptCommands[_ActionKey(actionId)])

    def _makeModeCallback(self, modeId):
        def callback(usdviewApi):
            status = SetActiveMode(self._state, modeId)
            if status and usdviewApi is not None:
                try:
                    usdviewApi.PrintStatus(status)
                except AttributeError:
                    pass
        return callback

    def _makeGraphSubModeCallback(self, subId):
        def callback(usdviewApi):
            SetActiveMode(self._state, "graph")
            status = SetActiveGraphSubMode(self._state, subId)
            if status and usdviewApi is not None:
                try:
                    usdviewApi.PrintStatus(status)
                except AttributeError:
                    pass
        return callback

    def _makeHierarchySubModeCallback(self, subId):
        def callback(usdviewApi):
            SetActiveMode(self._state, "hierarchy")
            status = SetActiveHierarchySubMode(self._state, subId)
            if status and usdviewApi is not None:
                try:
                    usdviewApi.PrintStatus(status)
                except AttributeError:
                    pass
        return callback

    def _makeSculptSubModeCallback(self, subId):
        def callback(usdviewApi):
            SetActiveMode(self._state, "sculpt")
            status = SetActiveSculptSubMode(self._state, subId)
            if status and usdviewApi is not None:
                try:
                    usdviewApi.PrintStatus(status)
                except AttributeError:
                    pass
        return callback

    def _makeHierarchyActionCallback(self, actionId):
        # Pure-state actions (navigate, locks) execute here; model
        # mutations (subdivide, merge, group) arm their sub-mode and report
        # the op, since they need a tube selection plus the P4 C ABI
        # (tonicHierarchy.REQUIRED_C_API), neither of which exists yet.
        def callback(usdviewApi):
            from . import tonicHierarchy
            state = self._state
            SetActiveMode(state, "hierarchy")
            if actionId == "enterLevel":
                SetActiveHierarchySubMode(state, "navigate")
                status = tonicHierarchy.enterLevel(state)
            elif actionId == "exitLevel":
                SetActiveHierarchySubMode(state, "navigate")
                status = tonicHierarchy.exitLevel(state)
            elif actionId == "toggleLockParents":
                status = tonicHierarchy.setLockParents(
                    state, not state.lockParents)
            elif actionId == "toggleLockChildren":
                status = tonicHierarchy.setLockChildren(
                    state, not state.lockChildren)
            elif actionId == "subdivide":
                SetActiveHierarchySubMode(state, "subdivide")
                status = ("Tonic Hierarchy: %s Needs the P4 C ABI "
                          "(Tonic_SubdivideTube)." % tonicHierarchy
                          .subdivideStatus(state.subdivideCount,
                                           state.splitMode, []))
            elif actionId == "mergeChildren":
                SetActiveHierarchySubMode(state, "merge")
                status = ("Tonic Hierarchy: select a parent tube, then %s "
                          "Needs the P4 C ABI (Tonic_MergeChildren)."
                          % tonicHierarchy
                          .mergeChildrenStatus("the parent"))
            elif actionId == "mergeSelected":
                SetActiveHierarchySubMode(state, "merge")
                status = ("Tonic Hierarchy: select siblings, then %s Needs "
                          "the P4 C ABI (Tonic_MergeSelected)."
                          % tonicHierarchy.mergeSelectedStatus([]))
            elif actionId == "group":
                SetActiveHierarchySubMode(state, "group")
                status = ("Tonic Hierarchy: select tubes, then %s Needs the "
                          "P4 C ABI (Tonic_GroupTubes)."
                          % tonicHierarchy.groupStatus([], True))
            else:
                status = ""
            if status and usdviewApi is not None:
                try:
                    usdviewApi.PrintStatus(status)
                except AttributeError:
                    pass
        return callback

    def _makeSculptActionCallback(self, actionId):
        def callback(usdviewApi):
            state = self._state
            SetActiveMode(state, "sculpt")
            if actionId == "togglePreserveLength":
                state.sculptPreserveLength = not state.sculptPreserveLength
                status = "Tonic Sculpt: length-preserving %s." % (
                    "on" if state.sculptPreserveLength else "off")
            elif actionId == "toggleMirrorX":
                state.sculptMirrorX = not state.sculptMirrorX
                status = "Tonic Sculpt: mirror-X %s." % (
                    "on" if state.sculptMirrorX else "off")
            else:
                status = ""
            if status and usdviewApi is not None:
                try:
                    usdviewApi.PrintStatus(status)
                except AttributeError:
                    pass
        return callback

    def _ensureController(self, usdviewApi):
        # Qt enters only here, inside the menu callback (the lazy-Qt rule).
        from . import tonicLib
        from .tonicGraphUI import GraphController
        if self._state.lib is None:
            try:
                self._state.lib = tonicLib.Library()
            except OSError as exc:
                usdviewApi.PrintStatus("Tonic Graph: %s" % exc)
                return None
        if self._graphController is None:
            self._graphController = GraphController(self._state, usdviewApi)
        return self._graphController

    def _makeGraphActionCallback(self, actionId):
        def callback(usdviewApi):
            controller = self._ensureController(usdviewApi)
            if controller is None:
                return
            if actionId == "bindScalp":
                paths = []
                try:
                    paths = list(usdviewApi.dataModel.selection
                                 .getSelectedPrimPaths())
                except AttributeError:
                    pass
                if not paths:
                    usdviewApi.PrintStatus(
                        "Tonic Graph: select the scalp mesh first")
                    return
                scalpPath = paths[0]
                try:
                    stageView = usdviewApi.stageView
                except AttributeError:
                    stageView = None
                if controller.activate(str(scalpPath)):
                    SetActiveMode(self._state, "graph")
                    SetActiveGraphSubMode(self._state, "draw")
                    if stageView is not None:
                        try:
                            controller.install(stageView)
                        except Exception as exc:  # noqa: BLE001 - UI install
                            usdviewApi.PrintStatus(
                                "Tonic Graph: viewport install failed: %s"
                                % exc)
            elif actionId == "weldAll":
                if controller.model is not None:
                    controller.weldAll()
            elif actionId == "toggleMirror":
                if controller.model is not None:
                    controller.toggleMirror()
            elif actionId == "rebake":
                if controller.model is not None:
                    controller.rebake()
        return callback


Tf.Type.Define(UsdGenTonicToolsPluginContainer)
