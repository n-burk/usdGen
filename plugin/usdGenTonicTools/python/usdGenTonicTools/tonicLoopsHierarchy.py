# usdGenTonicTools.tonicLoopsHierarchy -- Hierarchy mode's viewport loop
# (plan/18 section 3.6 HierarchyLoop, section 3.4, plan/17 section 5.4).
#
# Qt-free like every loop: pixels, modifiers and a camera in, model work
# out. What the artist does here is navigate levels and reshape the tree --
# select, enter, subdivide, merge, group, lock, show -- so almost every
# entry point is a one-shot action the dock's buttons and the hotkeys both
# call, and the only real gestures are a marquee and the edge stroke.
#
# Two rules this module exists to keep:
#   * no menu items and no modal dialogs (plan/18 section 6): the
#     destructive action, Re-subdivide, confirms through the status line --
#     the first call arms it and says what will be lost, the second call
#     does it;
#   * every multi-tube action is ONE undo step: the bracket wraps the whole
#     loop over the selection, not each tube.
from __future__ import annotations

import ctypes

from . import tonicHierarchy
from . import tonicLib
from . import tonicLibStage
from . import tonicLoopsTube
from . import tonicModes
from .tonicLoops import ToolLoop

# A marquee narrower than this many pixels is a click, not a band.
MARQUEE_MIN_PX = 3.0
# An edge stroke shorter than this is not an edge.
EDGE_MIN_PX = 6.0


class HierarchyLoop(ToolLoop):
    """Selection, level navigation and the tree-shaping actions."""

    modeId = "hierarchy"
    label = "Hierarchy"
    pickMask = tonicLib.TONIC_PICK_TUBE_VERT
    subModes = tonicModes.HIERARCHY_SUBMODES
    defaultSubMode = "navigate"

    def __init__(self, session, state):
        super(HierarchyLoop, self).__init__(session, state)
        self._marquee = None        # (x0, y0) while a band is live
        self._edge = None           # [worldA, worldB] of the drawn edge
        self._edgeLive = None       # the stroke being drawn
        self._edgeTube = -1         # which tube the edge was drawn over
        self._edgeStartPx = None    # where the edge stroke was pressed
        self._active = False
        self._resubdivideArmed = ()  # the tubes the next call would redo
        self._lastStatus = ""

    def activate(self):
        """Opt in to the model's per-branch visible frontier.

        The native API is deliberately optional so external callers running
        an older DLL retain the historical level-only navigation behaviour.
        Once enabled, an empty expansion set is the useful initial cut: all
        roots are visible and every descendant is hidden until its own
        parent is entered.
        """
        if self._enableActiveCut():
            # Tube mode may leave the artist with a selected CV/ring.  In
            # Hierarchy the owner is the editable unit, so promote every
            # still-visible component owner to TubeVert selection.  Hidden
            # descendants are deliberately dropped; guide selection remains
            # independent and is not touched by _selectFrontierTubes.
            owners = []
            for kind in (tonicLib.TONIC_PICK_TUBE_VERT,
                         tonicLib.TONIC_PICK_CENTER_CV,
                         tonicLib.TONIC_PICK_SECTION_RING,
                         tonicLib.TONIC_PICK_SECTION_CV):
                for tubeId, _subId, _subSubId in self.session.readSelection(
                        kind):
                    try:
                        if tonicHierarchy.isTubeVisible(
                                self.session.dll, self.session.model,
                                tubeId):
                            owners.append(int(tubeId))
                    except RuntimeError:
                        pass
            self._selectFrontierTubes(owners)
        return True

    # -- sub-modes ---------------------------------------------------------

    def subMode(self):
        return self.state.hierarchySubMode or self.defaultSubMode

    def setSubMode(self, subId):
        status = tonicModes.SetActiveHierarchySubMode(self.state, subId)
        if status:
            self._resubdivideArmed = ()
        return status

    # -- status ------------------------------------------------------------

    def _status(self, text):
        self._lastStatus = text
        self.session.report(text)
        return text

    def statusLine(self):
        tubes = self.selectedTubes()
        levels = self._levels()
        return tonicHierarchy.hierarchyStatus(
            self.state.activeLevel, self._tubeCount(), levels or [1],
            self.state.soloLevel, self.state.showMaxLevel,
            self.state.lockParents, self.state.lockChildren,
            fallbackReason=self.session.fallbackReason()) + \
            (" %d selected." % len(tubes) if tubes else "")

    # -- model reads --------------------------------------------------------

    def _dll(self):
        return self.session.dll

    def selectedTubes(self):
        """The selected tube ids, ascending."""
        return [i for i, _s, _ss in self.session.readSelection(
            tonicLib.TONIC_PICK_TUBE_VERT)]

    def _tubeIds(self):
        dll, model = self._dll(), self.session.model
        if dll is None or model is None:
            return []
        count = ctypes.c_int(0)
        dll.Tonic_ReadTubeIds(model, None, 0, ctypes.byref(count))
        n = max(int(count.value), 0)
        if n <= 0:
            return []
        out = (ctypes.c_int * n)()
        got = ctypes.c_int(0)
        if dll.Tonic_ReadTubeIds(model, out, n,
                                 ctypes.byref(got)) != tonicLib.TONIC_OK:
            return []
        return [int(out[i]) for i in range(min(n, int(got.value)))]

    def _tubeCount(self):
        dll, model = self._dll(), self.session.model
        if dll is None or model is None:
            return 0
        return int(dll.Tonic_GetTubeCount(model))

    def _levelOf(self, tubeId):
        dll, model = self._dll(), self.session.model
        if dll is None or model is None:
            return 1
        return max(int(dll.Tonic_GetTubeLevel(model, int(tubeId))), 1)

    def _levels(self):
        return sorted({self._levelOf(t) for t in self._tubeIds()})

    def _tubesAtLevel(self, level):
        level = int(level)
        return [tubeId for tubeId in self._tubeIds()
                if self._levelOf(tubeId) == level]

    # -- gesture ------------------------------------------------------------

    def press(self, sample):
        if self.session.model is None:
            return False
        self._active = True
        if sample.has("shift"):
            self._marquee = (sample.x, sample.y)
            return True
        if self._wantsEdgeStroke():
            return self._pressEdge(sample)
        return self._pressSelect(sample)

    def move(self, sample):
        if not self._active or self.session.model is None:
            return False
        if self._marquee is not None:
            return self._moveMarquee(sample)
        if self._edgeLive is not None:
            hit = sample.surface()
            if hit:
                self._edgeLive[1] = hit["point"]
            return True
        return False

    def release(self, sample):
        if not self._active:
            return False
        claimed = True
        if self._marquee is not None:
            self._releaseMarquee(sample)
        elif self._edgeLive is not None:
            self._releaseEdge(sample)
        else:
            claimed = True
        self._marquee = None
        self._edgeLive = None
        self._active = False
        return claimed

    def cancel(self):
        if not self._active and self._marquee is None and \
                self._edgeLive is None:
            return False
        self._marquee = None
        self._edgeLive = None
        self._active = False
        self._status("Tonic Hierarchy: cancelled")
        return True

    def hover(self, sample):
        if self.session.model is None:
            return False
        item = sample.item(self.pickMask, self.pickRadiusPx())
        if item:
            changed = self.session.setHover(item["kind"], item["id"],
                                            item["subId"], item["subSubId"])
        else:
            changed = self.session.setHover(0, -1, -1, -1)
        if changed:
            self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return False

    def doubleClick(self, sample):
        """Enter only the branch of the tube under the cursor."""
        item = sample.item(self.pickMask, self.pickRadiusPx())
        if not item:
            return False
        tubeId = int(item["id"])
        self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, [tubeId])
        if self._activeCutAvailable():
            _parents, children = self._enterParentsActiveCut([tubeId])
            if not children:
                self._selectFrontierTubes([tubeId])
                self._setCollapsedFocus(tubeId)
                self._pushFocus()
        else:
            children = self._childrenOf(tubeId)
            target = (self._levelOf(children[0]) if children
                      else self._levelOf(tubeId))
            self.focusLevel(target)
            if children:
                self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, children)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    # -- selection ----------------------------------------------------------

    def _pressSelect(self, sample):
        item = sample.item(self.pickMask, self.pickRadiusPx())
        mode = (tonicLib.TONIC_SELECT_TOGGLE if sample.has("ctrl")
                else tonicLib.TONIC_SELECT_SET)
        if not item:
            if mode == tonicLib.TONIC_SELECT_SET:
                self.session.clearSelection(tonicLib.TONIC_PICK_TUBE_VERT)
                self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
            return True
        self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, [int(item["id"])],
                            mode=mode)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        self._status("Tonic Hierarchy: tube %d (L%d)"
                     % (int(item["id"]), self._levelOf(int(item["id"]))))
        return True

    def _moveMarquee(self, sample):
        x0, y0 = self._marquee
        mode = (tonicLib.TONIC_SELECT_ADD if sample.has("ctrl")
                else tonicLib.TONIC_SELECT_SET)
        self.session.selectRect(sample.camera, x0, y0, sample.x, sample.y,
                                tonicLib.TONIC_PICK_TUBE_VERT, mode)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        return True

    def _releaseMarquee(self, sample):
        x0, y0 = self._marquee
        if abs(sample.x - x0) + abs(sample.y - y0) < MARQUEE_MIN_PX:
            return self._pressSelect(sample)
        self._moveMarquee(sample)
        self._status("Tonic Hierarchy: %d tube(s) selected"
                     % len(self.selectedTubes()))
        return True

    # -- the edge stroke ----------------------------------------------------

    def _wantsEdgeStroke(self):
        """Edge split mode draws its line before Subdivide runs."""
        return (self.subMode() == "subdivide" and
                self.state.splitMode == "edge")

    def _pressEdge(self, sample):
        hit = sample.surface()
        item = sample.item(self.pickMask, self.pickRadiusPx())
        tubes = self.selectedTubes()
        self._edgeTube = (int(item["id"]) if item
                          else (tubes[0] if tubes else -1))
        if hit is None:
            self._status("Tonic Hierarchy: draw the split across the root "
                         "region")
            return True
        self._edgeLive = [hit["point"], hit["point"]]
        self._edgeStartPx = (sample.x, sample.y)
        return True

    def _releaseEdge(self, sample):
        hit = sample.surface()
        if hit is not None and self._edgeLive is not None:
            self._edgeLive[1] = hit["point"]
        start = self._edgeStartPx
        if self._edgeLive is None or start is None or \
                abs(sample.x - start[0]) + abs(sample.y - start[1]) < \
                EDGE_MIN_PX:
            self._status("Tonic Hierarchy: that stroke is too short to "
                         "split along")
            return False
        self._edge = list(self._edgeLive)
        self._status("Tonic Hierarchy: split edge recorded over tube %d -- "
                     "Shift+D splits along it" % self._edgeTube)
        return True

    @property
    def edge(self):
        """The recorded split edge, or None (read by the T0 test)."""
        return tuple(self._edge) if self._edge else None

    # -- shared plumbing ----------------------------------------------------

    def endOfEdit(self):
        """Publish, enqueue the commit, rebake the map."""
        # Hierarchy operations can rewrite the primary descriptor, which
        # invalidates its guide cache even though leaves still have guides.
        # The Tube helper is intentionally unconditional for that case.
        tonicLoopsTube.restoreGuides(self.session, None)
        self.session.publish()
        self.session.enqueueCommit()
        self.session.rebake()
        return True

    def _childrenOf(self, tubeId):
        dll, model = self._dll(), self.session.model
        if dll is None or model is None:
            return []
        try:
            return tonicHierarchy.tubeChildren(dll, model, int(tubeId))
        except (RuntimeError, NotImplementedError):
            return []

    # -- per-branch active-cut navigation ---------------------------------

    def _activeCutAvailable(self):
        return (bool(getattr(self.state, "activeCutEnabled", False)) and
                self.session.model is not None and
                tonicHierarchy.supportsActiveCut(self.session.dll))

    def _enableActiveCut(self):
        """Enable the native frontier once, returning whether it is live."""
        if (self.session.model is None or
                not tonicHierarchy.supportsActiveCut(self.session.dll)):
            return False
        try:
            tonicHierarchy.setActiveCutEnabled(self.session.dll,
                                                self.session.model, True)
        except RuntimeError as exc:
            self._status("Tonic Hierarchy: %s" % exc)
            return False
        self.state.activeCutEnabled = True
        return True

    def _setExpanded(self, tubeId, expanded):
        if not self._enableActiveCut():
            return False
        try:
            tonicHierarchy.setTubeExpanded(self.session.dll,
                                            self.session.model, tubeId,
                                            expanded)
        except RuntimeError as exc:
            self._status("Tonic Hierarchy: %s" % exc)
            return False
        return True

    def _isExpanded(self, tubeId):
        if not self._activeCutAvailable():
            return False
        try:
            return bool(tonicHierarchy.getTubeExpanded(
                self.session.dll, self.session.model, tubeId))
        except RuntimeError:
            return False

    def _ancestorPath(self, tubeId):
        """Root-first tube ids for one branch, with a corruption guard."""
        path = []
        current = int(tubeId)
        seen = set()
        while current >= 0 and current not in seen:
            seen.add(current)
            path.append(current)
            parent = self._parentOf(current)
            if parent is None:
                break
            current = int(parent)
        path.reverse()
        return tuple(path)

    def _setExpandedFocus(self, parentId):
        """Focus the direct children of an expanded parent for UI styling."""
        parentId = int(parentId)
        path = self._ancestorPath(parentId)
        tonicHierarchy.setActiveCutFocus(
            self.state, parentId, path,
            names=tuple("T%d" % tubeId for tubeId in path),
            level=tonicHierarchy.deriveChildLevel(self._levelOf(parentId)))

    def _setCollapsedFocus(self, tubeId):
        """Focus a just-collapsed tube without changing unrelated cuts."""
        tubeId = int(tubeId)
        parent = self._parentOf(tubeId)
        level = self._levelOf(tubeId)
        if parent is not None and self._isExpanded(parent):
            path = self._ancestorPath(parent)
            tonicHierarchy.setActiveCutFocus(
                self.state, parent, path,
                names=tuple("T%d" % value for value in path), level=level)
            return
        path = self._ancestorPath(tubeId)
        tonicHierarchy.setActiveCutFocus(
            self.state, -1, path,
            names=tuple("T%d" % value for value in path), level=level)

    def _enterParentsActiveCut(self, parents):
        """Expand each selected parent and select their direct children."""
        parents = sorted({int(tubeId) for tubeId in parents})
        children = []
        expanded = []
        for tubeId in parents:
            direct = self._childrenOf(tubeId)
            if not direct:
                continue
            if self._setExpanded(tubeId, True):
                expanded.append(tubeId)
                children.extend(direct)
        children = sorted(set(children))
        if not children:
            return [], []
        self._selectFrontierTubes(children)
        # A multi-root enter changes every selected branch.  The first
        # branch merely supplies the single breadcrumb/status context; it
        # does not affect the model-owned cuts for the other branches.
        self._setExpandedFocus(expanded[0])
        self._pushFocus()
        return expanded, children

    def _minimalCollapseParents(self, parents):
        """Drop a selected parent that lies below another selected parent.

        A mixed-depth selection can contain an L2 child and an L3 grandchild
        from the same branch.  Collapsing both would leave the hidden L2
        record selected after its L1 ancestor already hid the whole subtree.
        The native frontier needs only the shallowest parent in each branch.
        """
        candidates = {int(tubeId) for tubeId in parents}
        result = []
        for tubeId in sorted(candidates):
            ancestor = self._parentOf(tubeId)
            keep = True
            seen = set()
            while ancestor is not None and ancestor not in seen:
                if ancestor in candidates:
                    keep = False
                    break
                seen.add(ancestor)
                ancestor = self._parentOf(ancestor)
            if keep:
                result.append(tubeId)
        return result

    def _selectFrontierTubes(self, tubeIds):
        """Select visible frontier owners and retire hidden component state."""
        # A Center/Section/Ring selection can survive a mode change.  Once
        # its owner was hidden by an active-cut collapse it must not leave a
        # stale Tube gizmo target behind.  Guides carry no selection record
        # and intentionally need no special treatment here.
        for kind in (tonicLib.TONIC_PICK_CENTER_CV,
                     tonicLib.TONIC_PICK_SECTION_RING,
                     tonicLib.TONIC_PICK_SECTION_CV):
            self.session.clearSelection(kind)
        self.session.select(tonicLib.TONIC_PICK_TUBE_VERT,
                            sorted({int(v) for v in tubeIds}))
        self.session.setHover(0, -1, -1, -1)
        self.session.publish(tonicLib.TONIC_DIRTY_SELECTION)

    # -- actions: subdivide / merge / group --------------------------------

    def subdivideSelection(self):
        """Shift+D / the dock's Subdivide over the selected tubes."""
        session = self.session
        tubes = self.selectedTubes()
        if session.model is None or not tubes:
            self._status("Tonic Hierarchy: select a tube to subdivide")
            return False
        count = tonicHierarchy.clampSubdivideCount(self.state.subdivideCount)
        splitMode = self.state.splitMode
        seed = int(self.state.panels.get("hierarchy", {})
                   .get("subdivideSeed", 0))
        if splitMode == "edge" and self._edge is None:
            self._status("Tonic Hierarchy: draw one stroke across the root "
                         "region first -- edge mode splits along it")
            return False
        session.beginGesture("Subdivide")
        made = 0
        children = []
        childrenByParent = {}
        failed = ""
        for tubeId in tubes:
            try:
                if splitMode == "edge":
                    created = self._subdivideAlongEdge(tubeId, seed)
                else:
                    created = tonicHierarchy.subdivide(
                        session.dll, session.model, tubeId, count, splitMode,
                        seed)
                made += len(created)
                children.extend(created)
                childrenByParent[int(tubeId)] = list(created)
            except (RuntimeError, NotImplementedError) as exc:
                failed = str(exc)
        session.endGesture()
        self.endOfEdit()
        if made == 0 and failed:
            self._status("Tonic Hierarchy: %s" % failed)
            return False
        # TonicSelection remaps a selected parent itself, but make the UI
        # contract explicit for a dock-button invocation too: subdivision
        # enters the newly-created editing level with its children selected.
        if children:
            if self._activeCutAvailable():
                # The geometry action completed before changing the view
                # frontier.  Every selected parent is expanded; a single
                # primary branch supplies the breadcrumb context.
                expanded = [tubeId for tubeId in tubes
                            if childrenByParent.get(int(tubeId)) and
                            self._setExpanded(tubeId, True)]
                if expanded:
                    self._selectFrontierTubes(
                        child for tubeId in expanded
                        for child in childrenByParent[int(tubeId)])
                    self._setExpandedFocus(expanded[0])
                    self._pushFocus()
            else:
                session.select(tonicLib.TONIC_PICK_TUBE_VERT, children)
                levels = {self._levelOf(tubeId) for tubeId in children}
                if len(levels) == 1:
                    self.focusLevel(next(iter(levels)))
        self._status("Tonic Hierarchy: %d child tube(s) from %d parent(s)%s"
                     % (made, len(tubes),
                        " along the drawn edge" if splitMode == "edge"
                        else ""))
        return True

    def _subdivideAlongEdge(self, tubeId, seed):
        """Tonic_SubdivideTubeEdge over the recorded stroke (audit 267)."""
        tonicLibStage.bindV5(self.session.dll)
        return tonicLibStage.subdivideTubeEdge(
            self.session.dll, self.session.model, tubeId, self._edge[0],
            self._edge[1], seed)

    def mergeChildrenOfSelection(self):
        """Shift+M / the dock's Merge children."""
        session = self.session
        tubes = self.selectedTubes()
        if session.model is None or not tubes:
            self._status("Tonic Hierarchy: select a parent tube to merge")
            return False
        # A selected child is a request to merge its PARENT's children: the
        # subdivide moved the selection down, so Shift+M right after
        # Shift+D has to find its way back up.
        targets = []
        for tubeId in tubes:
            merged = tubeId if self._childrenOf(tubeId) else \
                self._parentOf(tubeId)
            if merged is not None and merged not in targets:
                targets.append(merged)
        if not targets:
            self._status("Tonic Hierarchy: nothing under those tubes to "
                         "merge")
            return False
        # Set the frontier while the parents still have children: the native
        # API rightly rejects expanding/collapsing a leaf after the merge.
        if self._activeCutAvailable():
            for tubeId in targets:
                self._setExpanded(tubeId, False)
        session.beginGesture("Merge children")
        for tubeId in targets:
            try:
                tonicHierarchy.mergeChildren(session.dll, session.model,
                                             tubeId)
            except (RuntimeError, NotImplementedError) as exc:
                self._status("Tonic Hierarchy: %s" % exc)
        session.endGesture()
        if targets:
            if self._activeCutAvailable():
                self._selectFrontierTubes(targets)
                self._setCollapsedFocus(targets[0])
                self._pushFocus()
            else:
                self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, targets)
                self.focusLevel(self._levelOf(targets[0]))
        self.endOfEdit()
        self._status(tonicHierarchy.mergeChildrenStatus(
            "%d tube(s)" % len(targets)))
        return True

    def _parentOf(self, tubeId):
        dll, model = self._dll(), self.session.model
        entry = getattr(dll, "Tonic_GetTubeParent", None) if dll else None
        if entry is None or model is None:
            return None
        parent = ctypes.c_int(-1)
        childIndex = ctypes.c_int(-1)
        if entry(model, int(tubeId), ctypes.byref(parent),
                 ctypes.byref(childIndex)) != tonicLib.TONIC_OK:
            return None
        return int(parent.value) if int(childIndex.value) >= 0 else None

    def mergeSelected(self):
        """Fold the selected siblings into one tube at their level."""
        session = self.session
        tubes = self.selectedTubes()
        if session.model is None or len(tubes) < 2:
            self._status("Tonic Hierarchy: select two or more siblings to "
                         "merge")
            return False
        session.beginGesture("Merge selected")
        try:
            kept = tonicHierarchy.mergeSelected(session.dll, session.model,
                                                tubes)
        except (RuntimeError, NotImplementedError) as exc:
            session.endGesture()
            self._status("Tonic Hierarchy: %s" % exc)
            return False
        session.endGesture()
        self.endOfEdit()
        self._status(tonicHierarchy.mergeSelectedStatus(tubes) +
                     " Kept T%d." % kept)
        return True

    def resubdivide(self):
        """Merge then split again at the panel's count.

        Destructive -- the children's sculpt deltas go -- so the first call
        arms it with the plan/17 confirm text on the status line and the
        second call within the same selection carries it out. A status
        prompt, not a modal (plan/18 section 6).
        """
        session = self.session
        tubes = self.selectedTubes()
        if session.model is None or not tubes:
            self._status("Tonic Hierarchy: select a parent to re-subdivide")
            return False
        parents = []
        for tubeId in tubes:
            parent = tubeId if self._childrenOf(tubeId) else \
                self._parentOf(tubeId)
            if parent is not None and parent not in parents:
                parents.append(parent)
        if not parents:
            self._status("Tonic Hierarchy: those tubes have no children to "
                         "re-subdivide")
            return False
        count = tonicHierarchy.clampSubdivideCount(self.state.subdivideCount)
        if tuple(parents) != tuple(self._resubdivideArmed):
            self._resubdivideArmed = tuple(parents)
            self._status(tonicHierarchy.resubdivideConfirm(
                "%d tube(s)" % len(parents),
                max(len(self._childrenOf(parents[0])),
                    tonicHierarchy.SUBDIVIDE_MIN), count) +
                " Press Re-subdivide again to confirm.")
            return False
        self._resubdivideArmed = ()
        seed = int(self.state.panels.get("hierarchy", {})
                   .get("subdivideSeed", 0))
        session.beginGesture("Re-subdivide")
        made = 0
        children = []
        for tubeId in parents:
            try:
                tonicHierarchy.mergeChildren(session.dll, session.model,
                                             tubeId)
                created = tonicHierarchy.subdivide(
                    session.dll, session.model, tubeId, count,
                    self.state.splitMode, seed)
                made += len(created)
                children.extend(created)
            except (RuntimeError, NotImplementedError) as exc:
                self._status("Tonic Hierarchy: %s" % exc)
        session.endGesture()
        if children:
            if self._activeCutAvailable():
                expanded = [tubeId for tubeId in parents
                            if self._childrenOf(tubeId) and
                            self._setExpanded(tubeId, True)]
                if expanded:
                    visibleChildren = [child for tubeId in expanded
                                       for child in self._childrenOf(tubeId)]
                    self._selectFrontierTubes(visibleChildren)
                    self._setExpandedFocus(expanded[0])
                    self._pushFocus()
            else:
                session.select(tonicLib.TONIC_PICK_TUBE_VERT, children)
                levels = {self._levelOf(tubeId) for tubeId in children}
                if len(levels) == 1:
                    self.focusLevel(next(iter(levels)))
        self.endOfEdit()
        self._status("Tonic Hierarchy: re-subdivided %d parent(s) into %d "
                     "child tube(s)" % (len(parents), made))
        return True

    def group(self, transient=True):
        """K7 on-the-fly parent over the selection."""
        session = self.session
        tubes = self.selectedTubes()
        if session.model is None or len(tubes) < 2:
            self._status("Tonic Hierarchy: select two or more tubes to "
                         "group")
            return False
        session.beginGesture("Group")
        try:
            parent = tonicHierarchy.groupTubes(session.dll, session.model,
                                               tubes, transient=transient)
        except (RuntimeError, NotImplementedError) as exc:
            session.endGesture()
            self._status("Tonic Hierarchy: %s" % exc)
            return False
        session.endGesture()
        self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, [parent])
        self.endOfEdit()
        self._status(tonicHierarchy.groupStatus(tubes, transient) +
                     " Parent T%d." % parent)
        return True

    def makePersistent(self):
        """Keep an on-the-fly parent (it commits as a HierarchyAPI prim)."""
        session = self.session
        tubes = self.selectedTubes()
        if session.model is None or not tubes:
            self._status("Tonic Hierarchy: select the group to keep")
            return False
        session.beginGesture("Make persistent")
        for tubeId in tubes:
            try:
                tonicHierarchy.makePersistent(session.dll, session.model,
                                              tubeId)
            except (RuntimeError, NotImplementedError) as exc:
                self._status("Tonic Hierarchy: %s" % exc)
        session.endGesture()
        self.endOfEdit()
        self._status("Tonic Hierarchy: %d group(s) kept" % len(tubes))
        return True

    # -- actions: locks ------------------------------------------------------

    def setLockParents(self, on, perTube=True):
        status = tonicHierarchy.setLockParents(self.state, on)
        self._pushLocks(perTube)
        return self._status(status)

    def setLockChildren(self, on, perTube=True):
        status = tonicHierarchy.setLockChildren(self.state, on)
        self._pushLocks(perTube)
        return self._status(status)

    def _pushLocks(self, perTube):
        session = self.session
        if session.model is None:
            return
        targets = self.selectedTubes() if perTube else []
        if not targets:
            targets = [-1]          # -1 is the model's global default
        for tubeId in targets:
            try:
                tonicHierarchy.setLockParentsEntry(
                    session.dll, session.model, tubeId,
                    tonicHierarchy.effectiveLockParents(self.state, tubeId))
                tonicHierarchy.setLockChildrenEntry(
                    session.dll, session.model, tubeId,
                    tonicHierarchy.effectiveLockChildren(self.state, tubeId))
            except (RuntimeError, NotImplementedError):
                return
        session.publish()

    # -- actions: levels -----------------------------------------------------

    def enterLevel(self):
        if self._activeCutAvailable():
            parents, children = self._enterParentsActiveCut(
                self.selectedTubes())
            if not children:
                return self._status("Tonic Hierarchy: selected tube has no "
                                    "children to enter")
            return self._status("Tonic Hierarchy: entered %d branch%s; "
                                "%d child tube(s) selected."
                                % (len(parents), "es" if len(parents) != 1
                                   else "", len(children)))
        target = int(self.state.activeLevel) + 1
        selected = self.selectedTubes()
        children = []
        for tubeId in selected:
            children.extend(self._childrenOf(tubeId))
        children = sorted({tubeId for tubeId in children
                           if self._levelOf(tubeId) == target})
        if not children:
            return self._status("Tonic Hierarchy: selected tube has no "
                                "children at L%d" % target)
        self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, children)
        return self.focusLevel(target)

    def exitLevel(self):
        if self._activeCutAvailable():
            parents = self._minimalCollapseParents(
                parent for parent in (self._parentOf(tubeId)
                                      for tubeId in self.selectedTubes())
                if parent is not None)
            if not parents:
                return self._status("Tonic Hierarchy: selected tube has no "
                                    "parent to exit")
            collapsed = []
            for parent in parents:
                if self._setExpanded(parent, False):
                    collapsed.append(parent)
            if not collapsed:
                return self._status("Tonic Hierarchy: could not collapse "
                                    "the selected branch")
            self._selectFrontierTubes(collapsed)
            self._setCollapsedFocus(collapsed[0])
            self._pushFocus()
            return self._status("Tonic Hierarchy: returned to %d parent "
                                "tube%s."
                                % (len(collapsed), "s" if len(collapsed) != 1
                                   else ""))
        target = max(int(self.state.activeLevel) - 1, 1)
        if target == int(self.state.activeLevel):
            return self._status("Tonic Hierarchy: already at L1")
        parents = sorted({parent for parent in
                          (self._parentOf(tubeId)
                           for tubeId in self.selectedTubes())
                          if parent is not None and
                          self._levelOf(parent) == target})
        if parents:
            self.session.select(tonicLib.TONIC_PICK_TUBE_VERT, parents)
        return self.focusLevel(target)

    def focusLevel(self, level):
        status = tonicHierarchy.focusLevel(self.state, level)
        self._pushFocus()
        return self._status(status)

    def focusTube(self, tubeId):
        """Follow a per-branch breadcrumb back to one ancestor tube."""
        tubeId = int(tubeId)
        if not self._activeCutAvailable():
            return False
        # A leaf can still appear as a context crumb.  It has no expansion
        # bit to clear, so selecting it is a valid no-op navigation rather
        # than an avoidable native "tube is a leaf" error.
        if self._childrenOf(tubeId) and not self._setExpanded(tubeId, False):
            return False
        self._selectFrontierTubes([tubeId])
        self._setCollapsedFocus(tubeId)
        self._pushFocus()
        self._status("Tonic Hierarchy: focused T%d (L%d)."
                     % (tubeId, self._levelOf(tubeId)))
        return True

    def _pushFocus(self):
        session = self.session
        if session.model is None:
            return
        session.dll.Tonic_SetFocusLevel(session.model,
                                        int(self.state.activeLevel))
        self.syncLevelDisplay()

    def syncLevelDisplay(self):
        """Push Solo / Show<=n / hidden / x-ray to the model, per level.

        The toggles were Python-only state (audit section 5.4: "the toggles
        exist only as Python state"); this is the one place that turns them
        into Tonic_SetLevelDisplay calls, so the viewport agrees with the
        panel for every level, not just the focused one.
        """
        session = self.session
        if session.model is None:
            return False
        focus = int(self.state.activeLevel)
        xrayMap = self.state.panels.setdefault("xray", {})
        # Every level the model holds, the focused one, and any level
        # the artist has an opinion about: a hidden or x-rayed level with
        # no tube in it yet still has to be told.
        levels = (set(self._levels()) | {focus} | set(xrayMap) |
                  set(self.state.hiddenLevels))
        for level in sorted(level for level in levels if level >= 1):
            # The native active cut can show L1 in one root and L2/L3 in
            # another simultaneously.  Treat every visible frontier level
            # as focused for the default style; only the artist's explicit
            # x-ray/solo/show/hide controls then change its appearance.
            styleFocus = (level if self._activeCutAvailable() else focus)
            style = tonicHierarchy.levelDrawStyle(
                level, styleFocus, self.state.soloLevel,
                self.state.showMaxLevel,
                self.state.hiddenLevels)
            visible = style != "hidden"
            xray = bool(xrayMap.get(level, style == "x-ray"))
            session.dll.Tonic_SetLevelDisplay(session.model, int(level),
                                              1 if visible else 0,
                                              1 if xray else 0)
        session.publish(tonicLib.TONIC_DIRTY_DISPLAY)
        return True

    def setSolo(self, level):
        status = tonicHierarchy.setSoloLevel(self.state, level)
        self.syncLevelDisplay()
        return self._status(status)

    def setShowMaxLevel(self, level):
        status = tonicHierarchy.setShowMaxLevel(self.state, level)
        self.syncLevelDisplay()
        return self._status(status)

    def setLevelVisible(self, level, visible):
        status = (tonicHierarchy.showLevel(self.state, level) if visible
                  else tonicHierarchy.hideLevel(self.state, level))
        self.syncLevelDisplay()
        return self._status(status)

    def setLevelXray(self, level, xray):
        self.state.panels.setdefault("xray", {})[int(level)] = bool(xray)
        self.syncLevelDisplay()
        return self._status("Tonic Hierarchy: L%d %s"
                            % (int(level), "x-ray" if xray else "opaque"))

    # -- keys ----------------------------------------------------------------

    def adjustRadius(self, delta):
        """`[` / `]`: the pick radius Hierarchy selects with."""
        value = max(1.0, min(128.0, float(self.state.snapRadiusPx) +
                             float(delta)))
        self.state.snapRadiusPx = value
        return self._status("Tonic Hierarchy: pick radius %.0f px" % value)
