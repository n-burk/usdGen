# testUsdviewPomadeHierarchyOps -- T3 (TS-03): Hierarchy mode's operations
# driven by REAL mouse, key and dock-button input.
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testUsdviewPomadeHierarchyOps.py \
#               examples/pomade-graph-scalp.usda
#
# testUsdviewPomadeLevels walks the level keys, the breadcrumb, Delete and
# the Merge/Levels sub-mode clicks. This script covers the rest of the
# Hierarchy shelf through the same real input:
#
#   * the dock's Subdivide count row + Shift+D make four L2 children;
#   * Backspace leaves the entered branch (parent back, children hidden);
#   * a double-click on the parent enters exactly its branch;
#   * a click selects one child and a Shift-click adds a second;
#   * Group (dock) builds an on-the-fly parent over the two, Make
#     persistent keeps it, and the commit writes it as a
#     UsdGenTubeHierarchyAPI prim naming both members;
#   * a plain box in the Group sub-mode groups what it caught;
#   * Merge selected (dock) folds two siblings into one, Ctrl+Z undoes it;
#   * Re-subdivide asks first, Escape disarms the ask, two clicks do it;
#   * the dock's Solo level box hides every other level (it stays staged
#     with Hydra visibility off), and un-ticking brings it back;
#   * the Lock parents / Lock children boxes reach the committed tubes the
#     way the dock shows them;
#   * the edge split: a stroke across the root in Subdivide + edge, Shift+D
#     splits along it.
import ctypes
import os
import sys


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    argv = list(sys.argv)
    for i, arg in enumerate(argv):
        if arg == "--testScript" and i + 1 < len(argv):
            return os.path.dirname(os.path.abspath(argv[i + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return ""


def childrenOf(session, tubeId):
    out = (ctypes.c_int * 64)()
    got = ctypes.c_int(0)
    if session.dll.Pomade_GetTubeChildren(session.model, int(tubeId), out, 64,
                                         ctypes.byref(got)) != 0:
        return []
    return sorted(int(out[i]) for i in range(int(got.value)))


def centerCV(session, tubeId, cv):
    out = (ctypes.c_float * 3)()
    if session.dll.Pomade_GetTubeCenterCV(session.model, int(tubeId), int(cv),
                                         out) != 0:
        return None
    return (float(out[0]), float(out[1]), float(out[2]))


def committedTubes(stage):
    """{prim name: prim} of every committed tube / group prim."""
    out = {}
    root = stage.GetPrimAtPath("/PomadeGroom/Tubes")
    if not root:
        return out
    stack = list(root.GetChildren())
    while stack:
        prim = stack.pop()
        out[prim.GetName()] = prim
        stack.extend(prim.GetChildren())
    return out


def attrValue(prim, name):
    attr = prim.GetAttribute(name) if prim else None
    return attr.Get() if attr else None


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import pomadeT3
    from pomadeT3 import check, info, typeKey, wait
    try:
        from usdGenPomadeTools import pomadeHierarchy, pomadeLib
        import pomadeT3Groom
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools: %s" % exc)
        return 1
    from pxr.Usdviewq.qt import QtCore

    groom = pomadeT3Groom.Groom(appController)
    if not groom.build():
        groom.close()
        print("testUsdviewPomadeHierarchyOps: %d failure(s)"
              % pomadeT3.failureCount())
        return 1
    session, viewport, state = groom.session, groom.viewport, groom.state
    view, mouse, stage = groom.view, groom.mouse, groom.stage
    workspace = groom.workspace
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT
    QTest = pomadeT3._qtTest().QTest

    def selected():
        return sorted(entry[0] for entry in session.readSelection(TUBE))

    def visible(tubeId):
        return pomadeHierarchy.isTubeVisible(session.dll, session.model,
                                            tubeId) is True

    def levelVisible(level):
        shown = ctypes.c_int(1)
        xray = ctypes.c_int(0)
        session.dll.Pomade_GetLevelDisplay(session.model, int(level),
                                          ctypes.byref(shown),
                                          ctypes.byref(xray))
        return bool(shown.value)

    def focus():
        return int(session.dll.Pomade_GetFocusLevel(session.model))

    def clickablePixel(tubeId):
        """A pixel whose Hierarchy pick is `tubeId`, probed through the
        loop's own radius -- never assumed from a projection."""
        radius = viewport.loop.pickRadiusPx()
        for cv in (2, 3, 1, 4, 0):
            point = centerCV(session, tubeId, cv)
            if point is None:
                continue
            base = groom.camera.worldToPixels(point)
            if base is None:
                continue
            for dx, dy in ((0.0, 0.0), (4.0, 0.0), (-4.0, 0.0), (0.0, 4.0),
                           (0.0, -4.0), (8.0, 0.0), (-8.0, 0.0)):
                x, y = base[0] + dx, base[1] + dy
                hit = session.pickItem(groom.camera, x, y, radius, TUBE)
                if hit is not None and int(hit["id"]) == int(tubeId):
                    return (x, y)
        return None

    def setSpin(descriptorId, value):
        """Type `value` + Enter into a live dock spin box row."""
        workspace.refresh()
        descriptor, widget = groom.paramWidget(descriptorId)
        if widget is None or not widget.isEnabled():
            info("spin row %s is %s" % (descriptorId, "missing"
                                        if widget is None else "disabled"))
            return False
        widget.setFocus()
        wait(10)
        from pxr.Usdviewq.qt import QtWidgets
        focused = QtWidgets.QApplication.focusWidget()
        if focused is not widget and (focused is None or
                                      focused.parent() is not widget):
            # Typing digits with the focus elsewhere would reach the
            # viewport's hotkeys (1-6 switch modes), not the row.
            info("spin row %s did not take focus (%r)" % (descriptorId,
                                                          focused))
            return False
        widget.selectAll()
        QTest.keyClicks(widget, str(value))
        QTest.keyClick(widget, QtCore.Qt.Key.Key_Return)
        wait(20)
        view.setFocus()
        return True

    def clickCheck(descriptorId):
        """A real click on a dock checkbox row; its new checked state."""
        workspace.refresh()
        descriptor, widget = groom.paramWidget(descriptorId)
        if widget is None:
            return None
        QTest.mouseClick(widget, QtCore.Qt.MouseButton.LeftButton,
                         QtCore.Qt.KeyboardModifier.NoModifier,
                         QtCore.QPoint(6, widget.height() // 2))
        wait(20)
        view.setFocus()
        return bool(widget.isChecked())

    # A three-quarter view: the four children stand apart on screen.
    groom.side(eye=(11.0, 4.0, 9.0), target=(2.0, 1.5, 2.0))
    state.pickRadiusPx = 4.0

    # -- 4 + a click on the stub -------------------------------------------
    typeKey(view, "4")
    check(state.activeMode == "hierarchy" and viewport.loop is not None and
          viewport.loop.modeId == "hierarchy",
          "the 4 hotkey selects Hierarchy and builds its loop (%r)"
          % state.activeMode)
    spot = clickablePixel(0)
    check(spot is not None, "the stub has a clickable pixel")
    if spot is None:
        groom.close()
        return 1
    mouse.click(spot)
    wait(20)
    check(selected() == [0], "a click selects the stub (%r)" % (selected(),))

    # -- the dock's count row + Shift+D ------------------------------------
    check(setSpin("subdivideCount", 4) and int(state.subdivideCount) == 4,
          "typing 4 into Subdivide count sets the count (%r)"
          % state.subdivideCount)
    depth = groom.undoDepth()
    typeKey(view, "d", ("shift",))
    wait(30)
    kids = childrenOf(session, 0)
    check(len(kids) == 4,
          "Shift+D splits the stub into four L2 children (%r, %r)"
          % (kids, groom.messages[-1:]))
    check(groom.undoDepth() == depth + 1,
          "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))
    check(focus() == 2 and selected() == kids and
          all(visible(k) for k in kids) and not visible(0),
          "and enters the branch: L2 focused, children selected and on "
          "screen, parent off (focus %d, %r)" % (focus(), selected()))
    session.publishAll()
    l2 = pomadeT3.levelInfo(session, 2)
    check(l2 is not None and l2[2] == 4,
          "/__usdGenPomade/tubes/L2 publishes the four (%r)" % (l2,))
    if len(kids) != 4:
        groom.close()
        print("testUsdviewPomadeHierarchyOps: %d failure(s)"
              % pomadeT3.failureCount())
        return 1

    # -- Backspace, then a double-click back in ----------------------------
    typeKey(view, "backspace")
    wait(20)
    check(visible(0) and not any(visible(k) for k in kids) and focus() == 1
          and selected() == [0],
          "Backspace exits: parent back and selected, children hidden, L1 "
          "focused (focus %d, %r)" % (focus(), selected()))
    spot = clickablePixel(0)
    check(spot is not None, "the parent is clickable again")
    if spot is not None:
        mouse.click(spot)
        wait(10)
        mouse.doubleClick(spot)
        wait(30)
        check(all(visible(k) for k in kids) and not visible(0) and
              focus() == 2,
              "a double-click on the parent enters its branch (focus %d)"
              % focus())
        check(selected() == kids,
              "with its children selected (%r)" % (selected(),))

    # -- click one child, Shift-click a second ------------------------------
    pixels = {k: clickablePixel(k) for k in kids}
    clickable = [k for k in kids if pixels[k] is not None]
    info("children with a clickable pixel: %r" % (clickable,))
    check(len(clickable) >= 2,
          "at least two children can be clicked apart (%r)" % (clickable,))
    if len(clickable) < 2:
        groom.close()
        print("testUsdviewPomadeHierarchyOps: %d failure(s)"
              % pomadeT3.failureCount())
        return 1
    a, b = clickable[0], clickable[1]
    mouse.click(pixels[a])
    wait(10)
    check(selected() == [a], "a click selects one child (%r)" % (selected(),))
    mouse.click(pixels[b], ("shift",))
    wait(10)
    check(selected() == sorted([a, b]),
          "a Shift-click adds a second (%r)" % (selected(),))

    # -- Group, Make persistent, commit ------------------------------------
    typeKey(view, "g")
    check(viewport.loop.subMode() == "group",
          "G selects the Group sub-mode (%r)" % viewport.loop.subMode())
    check(selected() == sorted([a, b]),
          "the sub-mode switch keeps the two selected (%r)" % (selected(),))
    depth = groom.undoDepth()
    mark = len(groom.messages)
    check(groom.clickDock("action", "group"), "the dock's Group button")
    group = selected()
    check(len(group) == 1 and group[0] < 0,
          "Group builds an on-the-fly parent and selects it (%r, %r)"
          % (group, groom.since(mark)))
    check(any("Parent T" in m for m in groom.since(mark)),
          "and names it in the status (%r)" % groom.since(mark)[-2:])
    check(groom.undoDepth() == depth + 1,
          "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))
    mark = len(groom.messages)
    check(groom.clickDock("action", "makePersistent"),
          "the dock's Make persistent button")
    check(any("group(s) kept" in m for m in groom.since(mark)),
          "Make persistent keeps it (%r)" % groom.since(mark)[-2:])
    check(groom.commit(), "the idle pump commits the kept group")
    prims = committedTubes(stage)
    groupName = "group%d" % (-group[0]) if group else ""
    groupPrim = prims.get(groupName)
    check(groupPrim is not None,
          "the kept group is committed as %s (%r)"
          % (groupName, sorted(prims)))
    if groupPrim is not None:
        check(bool(attrValue(groupPrim, "usdGen:pomade:persistent")),
              "with usdGen:pomade:persistent on")
        rel = groupPrim.GetRelationship("usdGen:pomade:members")
        targets = [str(p) for p in rel.GetTargets()] if rel else []
        check(len(targets) == 2 and
              all(t.endswith("tube%d" % k) for t, k in
                  zip(sorted(targets), sorted([a, b]))),
              "naming its two members (%r)" % (targets,))
        check("UsdGenTubeHierarchyAPI" in
              [str(s) for s in groupPrim.GetAppliedSchemas()] or
              "UsdGenTubeHierarchyAPI" in
              [str(s) for s in (groupPrim.GetMetadata("apiSchemas")
                                .GetAddedOrExplicitItems()
                                if groupPrim.GetMetadata("apiSchemas")
                                else [])],
              "as a UsdGenTubeHierarchyAPI prim")
    # Back to the four children with nothing grouped.
    for _ in range(4):
        if groom.undoDepth() <= depth:
            break
        typeKey(view, "z", ("ctrl",))
        wait(20)
    check(groom.undoDepth() == depth and
          not any(t < 0 for t in childrenOf(session, 0)) and
          childrenOf(session, 0) == kids,
          "Ctrl+Z takes the group away again (%r)" % (childrenOf(session, 0),))

    # -- a plain box in Group groups what it caught -------------------------
    session.clearSelection(TUBE)
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    xs = [pixels[k][0] for k in clickable]
    ys = [pixels[k][1] for k in clickable]
    start = (min(xs) - 60.0, min(ys) - 80.0)
    end = (max(xs) + 60.0, max(ys) + 80.0)
    probe = session.pickItem(groom.camera, start[0], start[1],
                             viewport.loop.pickRadiusPx(), TUBE)
    check(probe is None, "the band starts on empty space (%r)" % (probe,))
    depth = groom.undoDepth()
    mark = len(groom.messages)
    mouse.drag([start, ((start[0] + end[0]) * 0.5,
                        (start[1] + end[1]) * 0.5), end])
    wait(20)
    boxed = selected()
    check(len(boxed) == 1 and boxed[0] < 0 and
          groom.undoDepth() == depth + 1,
          "a plain box in Group groups the tubes it caught into one parent "
          "(%r, %r)" % (boxed, groom.since(mark)[-2:]))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(childrenOf(session, 0) == kids and groom.undoDepth() == depth,
          "and Ctrl+Z ungroups (%r)" % (childrenOf(session, 0),))

    # -- Merge selected ------------------------------------------------------
    typeKey(view, "n")
    pixels = {k: clickablePixel(k) for k in kids}
    mouse.click(pixels[a])
    wait(10)
    mouse.click(pixels[b], ("shift",))
    wait(10)
    check(selected() == sorted([a, b]),
          "two siblings selected for Merge selected (%r)" % (selected(),))
    depth = groom.undoDepth()
    mark = len(groom.messages)
    check(groom.clickDock("action", "mergeSelected"),
          "the dock's Merge selected button")
    merged = childrenOf(session, 0)
    check(len(merged) == 3,
          "Merge selected folds the two into one sibling (%r, %r)"
          % (merged, groom.since(mark)[-2:]))
    check(any("Kept T" in m for m in groom.since(mark)),
          "and says which tube kept (%r)" % groom.since(mark)[-1:])
    check(groom.undoDepth() == depth + 1,
          "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(childrenOf(session, 0) == kids,
          "Ctrl+Z restores the four siblings (%r)" % (childrenOf(session, 0),))

    # -- Re-subdivide asks first; Escape disarms -----------------------------
    session.select(TUBE, [a])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    typeKey(view, "backspace")
    wait(20)
    check(selected() == [0], "Backspace selects the parent (%r)"
          % (selected(),))
    resub = workspace.button("action", "resubdivide")
    plain = resub.text() if resub is not None else ""
    depth = groom.undoDepth()
    mark = len(groom.messages)
    check(groom.clickDock("action", "resubdivide"),
          "the dock's Re-subdivide button")
    workspace.refresh()
    check(viewport.loop.resubdivideArmed and "Confirm" in resub.text() and
          childrenOf(session, 0) == kids and groom.undoDepth() == depth,
          "one click only asks: armed, the button reads Confirm, nothing "
          "re-split (%r, %r)" % (resub.text(), groom.since(mark)[-1:]))
    typeKey(view, "escape")
    wait(20)
    workspace.refresh()
    check(not viewport.loop.resubdivideArmed and resub.text() == plain,
          "Escape disarms it and the button reads plain again (%r)"
          % resub.text())
    groom.clickDock("action", "resubdivide")
    workspace.refresh()
    check(viewport.loop.resubdivideArmed,
          "after Escape the next click asks again, it does not act")
    mark = len(groom.messages)
    groom.clickDock("action", "resubdivide")
    redone = childrenOf(session, 0)
    check(len(redone) == 4 and groom.undoDepth() == depth + 1 and
          any("re-subdivided" in m for m in groom.since(mark)),
          "the confirming click re-subdivides as one undo step (%r, %r)"
          % (redone, groom.since(mark)[-1:]))
    kids = redone

    # A re-split that refuses after the REAL merge went through: the
    # action rolls back, so the parent keeps its children and no step is
    # left (the split refusal is injected; the merge is the model's own).
    from usdGenPomadeTools import pomadeHierarchy as _hier
    realSplit = _hier.subdivide

    def refusedSplit(*_args, **_kwargs):
        raise RuntimeError("T3: re-split refused")
    session.select(TUBE, [0])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    depth = groom.undoDepth()
    _hier.subdivide = refusedSplit
    try:
        groom.clickDock("action", "resubdivide")
        mark = len(groom.messages)
        groom.clickDock("action", "resubdivide")
        wait(20)
    finally:
        _hier.subdivide = realSplit
    check(sorted(childrenOf(session, 0)) == sorted(kids) and
          groom.undoDepth() == depth and not session.gestureActive,
          "a refused re-split rolls the merge back: children kept, no step "
          "(%r, depth %d -> %d)" % (childrenOf(session, 0), depth,
                                     groom.undoDepth()))
    check(any("rolled back" in m and "nothing changed" in m
              for m in groom.since(mark)),
          "and the status says it was rolled back (%r)"
          % groom.since(mark)[-2:])

    # -- Solo hides every other level from the scene index -----------------
    session.select(TUBE, [0])
    typeKey(view, "down", ("ctrl",))
    wait(20)
    session.publishAll()
    before = pomadeT3.levelInfo(session, 2)
    check(before is not None and before[2] == 4,
          "L2 publishes the four children before Solo (%r)" % (before,))
    _d, soloSpin = groom.paramWidget("soloLevel")
    check(soloSpin is not None and not soloSpin.isEnabled(),
          "Level to solo waits for the Solo level box (disabled)")
    ticked = clickCheck("soloLevelOn")
    check(setSpin("soloLevel", 1), "Level to solo set to 1 in the dock")
    session.publishAll()
    wait(20)
    check(ticked is True and int(state.soloLevel) == 1,
          "ticking Solo level solos L1 (%r, solo %r)"
          % (ticked, state.soloLevel))
    # A hidden level stays staged with Hydra visibility off (pomadePublish
    # stages every level with display.visible), so what Solo changes is
    # the level's display record, not the published census.
    soloed = pomadeT3.levelInfo(session, 2)
    check(not levelVisible(2) and levelVisible(1),
          "and hides L2 while L1 stays drawn (L1 %r, L2 %r)"
          % (levelVisible(1), levelVisible(2)))
    info("L2 stays staged while soloed away: %r" % (soloed,))
    unticked = clickCheck("soloLevelOn")
    session.publishAll()
    wait(20)
    after = pomadeT3.levelInfo(session, 2)
    check(unticked is False and after is not None and after[2] == 4 and
          levelVisible(2) and int(state.soloLevel) < 1,
          "un-ticking brings L2 back (%r, solo %r)"
          % (after, state.soloLevel))

    # -- the lock boxes reach the committed tubes --------------------------
    first, second = kids[0], kids[1]
    session.select(TUBE, [first])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    workspace.refresh()
    lockedChildren = clickCheck("lockChildren")
    lockedParents = clickCheck("lockParents")
    check(lockedChildren is True and lockedParents is True,
          "the Lock children / Lock parents boxes tick (%r, %r)"
          % (lockedChildren, lockedParents))
    session.select(TUBE, [second])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    workspace.refresh()
    _d, childBox = groom.paramWidget("lockChildren")
    _d, parentBox = groom.paramWidget("lockParents")
    shownForSecond = (bool(childBox.isChecked()) if childBox else None,
                      bool(parentBox.isChecked()) if parentBox else None)
    info("with T%d selected the dock shows locks %r" % (second,
                                                      shownForSecond))
    check(groom.commit(), "the idle pump commits the locks")
    prims = committedTubes(stage)
    firstPrim = prims.get("tube%d" % first)
    secondPrim = prims.get("tube%d" % second)
    check(firstPrim is not None and
          attrValue(firstPrim, "usdGen:pomade:lockChildren") is True and
          attrValue(firstPrim, "usdGen:pomade:lockParents") is True,
          "the tube selected when they were ticked commits both locks "
          "(%r, %r)" % (attrValue(firstPrim, "usdGen:pomade:lockChildren"),
                        attrValue(firstPrim, "usdGen:pomade:lockParents")))
    committedSecond = (attrValue(secondPrim, "usdGen:pomade:lockChildren"),
                       attrValue(secondPrim, "usdGen:pomade:lockParents"))
    check(secondPrim is not None and
          committedSecond == shownForSecond,
          "a tube the dock shows as %r commits exactly that (%r)"
          % (shownForSecond, committedSecond))
    # Untick for the next section (the first tube again).
    session.select(TUBE, [first])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    workspace.refresh()
    _d, childBox = groom.paramWidget("lockChildren")
    if childBox is not None and childBox.isChecked():
        clickCheck("lockChildren")
    _d, parentBox = groom.paramWidget("lockParents")
    if parentBox is not None and parentBox.isChecked():
        clickCheck("lockParents")

    # -- the edge split -----------------------------------------------------
    session.select(TUBE, [first])
    typeKey(view, "backspace")
    wait(20)
    typeKey(view, "m", ("shift",))
    wait(20)
    check(childrenOf(session, 0) == [],
          "Shift+M folds the children away for the edge split (%r)"
          % (childrenOf(session, 0),))
    workspace.refresh()
    descriptor, combo = groom.paramWidget("splitMode")
    check(combo is not None, "the dock has a Split mode row")
    if combo is not None:
        index = combo.findData("edge")
        combo.setFocus()
        combo.setCurrentIndex(index)
        wait(20)
        view.setFocus()
    check(state.splitMode == "edge",
          "choosing Edge in the dock sets the split mode (%r)"
          % state.splitMode)
    groom.top()
    typeKey(view, "d")
    check(viewport.loop.subMode() == "subdivide",
          "D selects the Subdivide sub-mode (%r)" % viewport.loop.subMode())
    session.select(TUBE, [0])
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    strokePath = [groom.scalpPixel(0.6 + 2.8 * i / 8.0, 2.0)
                  for i in range(9)]
    mouse.drag(strokePath)
    wait(20)
    check(getattr(viewport.loop, "edge", None) is not None and
          viewport._regionOverlay.isVisible(),
          "a stroke across the root records the split edge, drawn (%r)"
          % (groom.messages[-1:],))
    depth = groom.undoDepth()
    typeKey(view, "d", ("shift",))
    wait(30)
    split = childrenOf(session, 0)
    check(len(split) == 2 and groom.undoDepth() == depth + 1,
          "Shift+D splits the root along it into two, one undo step "
          "(%r, %r)" % (split, groom.messages[-1:]))
    check(getattr(viewport.loop, "edge", 0) is None and
          not viewport._regionOverlay.isVisible(),
          "and consumes the edge")

    typeKey(view, "n")
    groom.close()
    print("testUsdviewPomadeHierarchyOps: %d failure(s)"
          % pomadeT3.failureCount())
    return 1 if pomadeT3.failureCount() else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeHierarchyOps needs testusdview (no live "
          "view)")
    sys.exit(0)
