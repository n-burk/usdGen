# testUsdviewPomadeTubeActions -- T3 (TS-03): the Tube panel's actions
# (Relax, Match surface, Snap root to scalp) driven by REAL dock-button
# clicks over a groom shaped with REAL viewport drags.
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testUsdviewPomadeTubeActions.py \
#               examples/pomade-graph-scalp.usda
#
# Two Draw strokes leave two L1 stubs. In Tube mode a tweak-drag on the
# middle center CV kinks tube 0, and:
#
#   * Relax (dock) pulls the kinked CV back toward its neighbours with the
#     root and tip held, touches only the selected tube, is one undo step,
#     reaches the stage on the next commit and Ctrl+Z restores the kink;
#   * a whole-tube tweak-drag lifts tube 0 off the scalp; Snap root to
#     scalp (dock) puts its root back on the scalp by translating the whole
#     tube (the shape is kept); Match surface (dock) puts only the root CV
#     back; each is one undo step and Ctrl+Z restores the lift;
#   * every action says what it did in the dock's message area.
import math
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


SECOND = ((0.1, 0.1), (0.8, 0.1), (0.8, 0.8), (0.1, 0.8))


def distance(a, b):
    return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))


def lateral(points, cv):
    """How far CV `cv` sits off the chord between its two neighbours."""
    a, p, b = points[cv - 1], points[cv], points[cv + 1]
    mid = tuple(0.5 * (a[k] + b[k]) for k in range(3))
    return distance(p, mid)


def same(a, b, eps=1e-5):
    return len(a) == len(b) and all(
        abs(a[i][k] - b[i][k]) <= eps for i in range(len(a))
        for k in range(3))


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import pomadeT3
    from pomadeT3 import check, info, typeKey, wait
    try:
        from usdGenPomadeTools import pomadeBridge, pomadeLib
        import pomadeT3Groom
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools: %s" % exc)
        return 1

    groom = pomadeT3Groom.Groom(appController)
    if not groom.build():
        groom.close()
        print("testUsdviewPomadeTubeActions: %d failure(s)"
              % pomadeT3.failureCount())
        return 1
    session, viewport, state = groom.session, groom.viewport, groom.state
    view, mouse, stage = groom.view, groom.mouse, groom.stage
    workspace = groom.workspace
    CENTER = pomadeLib.POMADE_PICK_CENTER_CV
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT

    counts = groom.stroke(SECOND)
    check(counts == (8, 8, 2),
          "a second stroke closes a second region (%r)" % (counts,))
    roots = groom.l1Tubes()
    check(len(roots) == 2 and 0 in roots,
          "and grows its own L1 stub (%r)" % (roots,))
    other = [t for t in roots if t != 0]
    other = other[0] if other else -1
    otherBase = groom.centers(other)

    def handlePixel(tubeId, cv):
        point = pomadeBridge.tubeCenterHandle(session.dll, session.model,
                                             tubeId, cv)
        return groom.pixel(point) if point is not None else None

    def dockMessage():
        label = getattr(workspace, "_messageLabel", None)
        return label.text() if label is not None else ""

    def action(actionId):
        """Click the dock action; (undo depth delta, message) after it."""
        depth = groom.undoDepth()
        clicked = groom.clickDock("action", actionId)
        wait(20)
        return clicked, groom.undoDepth() - depth, dockMessage()

    # Side on: the stubs stand up across the frame, and screen-right is
    # world -Z, so a sideways drag bends a center curve in Z.
    groom.side(eye=(14.0, 1.6, 2.0), target=(2.0, 1.6, 2.0))
    typeKey(view, "2")
    check(state.activeMode == "tube" and viewport.loop is not None and
          viewport.loop.modeId == "tube",
          "the 2 hotkey selects Tube mode and its loop (%r)" % state.activeMode)
    check(state.tubeSubMode == "center",
          "opening in Center CV (%r)" % state.tubeSubMode)
    for button in ("relax", "matchSurface", "snapRoot"):
        check(workspace.button("action", button) is not None,
              "the Tube page has its %s button" % button)

    # -- kink tube 0 with a real tweak-drag --------------------------------
    session.clearSelection()
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    straight = groom.centers(0)
    mid = handlePixel(0, 2)
    check(mid is not None, "tube 0's middle CV projects (%r)" % (mid,))
    if mid is None:
        groom.close()
        return 1
    depth = groom.undoDepth()
    mouse.drag([mid] + [(mid[0] + 12.0 * s, mid[1]) for s in range(1, 6)])
    wait(20)
    kinked = groom.centers(0)
    check(session.readSelection(CENTER) == [(0, 2, -1)],
          "the press selected the middle CV (%r)"
          % (session.readSelection(CENTER),))
    check(lateral(kinked, 2) > lateral(straight, 2) + 0.2,
          "and the drag bent the curve sideways (off-chord %.3f -> %.3f)"
          % (lateral(straight, 2), lateral(kinked, 2)))
    check(groom.undoDepth() == depth + 1,
          "one tweak, one undo step (%d -> %d)" % (depth, groom.undoDepth()))

    # -- Relax -------------------------------------------------------------
    clicked, steps, message = action("relax")
    relaxed = groom.centers(0)
    check(clicked, "the Relax button was clicked")
    check(lateral(relaxed, 2) < 0.8 * lateral(kinked, 2),
          "Relax pulls the kinked CV back toward its neighbours "
          "(off-chord %.3f -> %.3f)"
          % (lateral(kinked, 2), lateral(relaxed, 2)))
    check(distance(relaxed[0], kinked[0]) < 1e-5 and
          distance(relaxed[-1], kinked[-1]) < 1e-5,
          "with the root and tip held")
    check(same(groom.centers(other), otherBase),
          "and the unselected tube T%d untouched" % other)
    check(steps == 1, "as exactly one undo step (+%d)" % steps)
    check("Relax" in message,
          "the dock's message area says Relax ran (%r)" % message)
    check(groom.commit(), "the idle pump settles after Relax")
    tubePrim = stage.GetPrimAtPath("/PomadeGroom/Tubes/tube0")
    points = None
    if tubePrim:
        attr = tubePrim.GetAttribute("usdGen:pomade:centerPoints")
        points = attr.Get() if attr else None
    committed = ([tuple(float(v) for v in p) for p in points]
                 if points is not None else [])
    check(same(committed, relaxed, 1e-4),
          "the committed tube0 carries the relaxed center curve "
          "(committed CV2 %r, model %r)"
          % (committed[2] if len(committed) > 2 else None, relaxed[2]))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(same(groom.centers(0), kinked),
          "Ctrl+Z restores the kink exactly")
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(same(groom.centers(0), straight),
          "and a second Ctrl+Z the straight stub")

    # -- lift tube 0 off the scalp with a whole-tube tweak -----------------
    typeKey(view, "f8")
    check(state.tubeSubMode == "tube",
          "F8 selects Whole tube (%r)" % state.tubeSubMode)
    session.clearSelection()
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    body = handlePixel(0, 2)
    probe = session.pickItem(groom.camera, body[0], body[1],
                             viewport.loop.pickRadiusPx(), TUBE)
    check(probe is not None and int(probe["id"]) == 0,
          "the middle of tube 0 picks the tube (%r)" % (probe,))
    depth = groom.undoDepth()
    mouse.drag([body] + [(body[0], body[1] - 12.0 * s) for s in range(1, 6)])
    wait(20)
    lifted = groom.centers(0)
    rise = lifted[0][1] - straight[0][1]
    check([entry[0] for entry in session.readSelection(TUBE)] == [0],
          "the press selected tube 0 (%r)" % (session.readSelection(TUBE),))
    check(rise > 0.2 and
          all(abs((lifted[i][1] - straight[i][1]) - rise) < 1e-3
              for i in range(len(lifted))) and
          groom.undoDepth() == depth + 1,
          "GZ-03 tweak in Whole tube: the same press-drag lifted the whole "
          "tube off the scalp as one undo step (root y %.3f, depth %d -> "
          "%d)" % (lifted[0][1], depth, groom.undoDepth()))
    if rise <= 0.2:
        # Lift it through the gizmo's own V handle instead, so the actions
        # below still have a lifted tube to act on.
        from usdGenPomadeTools import pomadeCamera, pomadeGizmo
        camera = pomadeCamera.resolve(view)
        gizmo = viewport.loop._gizmo
        grip = None
        for record in gizmo.screenHandles(camera):
            if record.get("handle") != pomadeGizmo.HANDLE_V:
                continue
            pts = record.get("points") or []
            if len(pts) >= 2:
                a, b = pts[0], pts[-1]
                for f in (0.7, 0.6, 0.8, 0.5):
                    candidate = (a[0] + (b[0] - a[0]) * f,
                                 a[1] + (b[1] - a[1]) * f)
                    if gizmo.handleAt(camera, candidate[0], candidate[1])                             == pomadeGizmo.HANDLE_V:
                        grip = candidate
                        break
        check(grip is not None,
              "the selected tube's gizmo offers a grabbable V handle")
        if grip is not None:
            depth = groom.undoDepth()
            mouse.drag([grip] + [(grip[0], grip[1] - 12.0 * s)
                                 for s in range(1, 6)])
            wait(20)
            lifted = groom.centers(0)
            rise = lifted[0][1] - straight[0][1]
            check(rise > 0.2 and
                  all(abs((lifted[i][1] - straight[i][1]) - rise) < 1e-3
                      for i in range(len(lifted))),
                  "a V-handle drag lifts the whole tube off the scalp "
                  "(root y %.3f)" % lifted[0][1])
            check(groom.undoDepth() == depth + 1,
                  "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))
    if rise <= 0.2:
        groom.close()
        print("testUsdviewPomadeTubeActions: %d failure(s)"
              % pomadeT3.failureCount())
        return 1

    # -- Snap root to scalp ------------------------------------------------
    clicked, steps, message = action("snapRoot")
    snapped = groom.centers(0)
    check(clicked, "the Snap root button was clicked")
    check(abs(snapped[0][1]) < 1e-3,
          "Snap root puts the root back on the scalp (y %.4f)"
          % snapped[0][1])
    shift = [snapped[0][k] - lifted[0][k] for k in range(3)]
    check(all(abs((snapped[i][k] - lifted[i][k]) - shift[k]) < 1e-4
              for i in range(len(snapped)) for k in range(3)),
          "by translating the whole tube: the shape is kept")
    check(same(groom.centers(other), otherBase),
          "the unselected tube T%d untouched" % other)
    check(steps == 1 and "Snap root" in message,
          "one undo step, and the message area says so (+%d, %r)"
          % (steps, message))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(same(groom.centers(0), lifted), "Ctrl+Z restores the lift")

    # -- Match surface -----------------------------------------------------
    clicked, steps, message = action("matchSurface")
    matched = groom.centers(0)
    check(clicked, "the Match surface button was clicked")
    check(abs(matched[0][1]) < 1e-3,
          "Match surface puts the root CV on the scalp (y %.4f)"
          % matched[0][1])
    check(same(matched[1:], lifted[1:]),
          "and leaves the rest of the curve where it was")
    check(same(groom.centers(other), otherBase),
          "the unselected tube T%d untouched" % other)
    check(steps == 1 and "Match surface" in message,
          "one undo step, and the message area says so (+%d, %r)"
          % (steps, message))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(same(groom.centers(0), lifted), "Ctrl+Z restores the lift")
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(same(groom.centers(0), straight),
          "and one more Ctrl+Z the stub on the scalp")

    # -- with nothing selected an action says which tube it used ------------
    session.clearSelection()
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    clicked, steps, message = action("relax")
    info("Relax with nothing selected: +%d step(s), %r" % (steps, message))
    check(same(groom.centers(0), straight) and steps == 0,
          "SS-02: a Relax over an already-straight stub changes nothing and "
          "so leaves no undo step (+%d, %r)" % (steps, session.undoLabel(0)))
    check(same(groom.centers(other), otherBase),
          "with nothing selected Relax leaves T%d alone (the primary tube "
          "is the documented target)" % other)

    # -- Delete down to the 2-CV minimum, then a fully refused Delete ------
    typeKey(view, "f9")
    check(state.tubeSubMode == "center",
          "F9 goes back to Center CV (%r)" % state.tubeSubMode)
    cvCount = len(groom.centers(0))

    def selectCenters(count):
        session.select(CENTER, [0] * count, list(range(count)),
                       [-1] * count, pomadeLib.POMADE_SELECT_SET)
        session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
        wait(10)

    selectCenters(cvCount)
    depth = groom.undoDepth()
    mark = len(groom.messages)
    typeKey(view, "delete")
    wait(20)
    check(len(groom.centers(0)) == 2 and groom.undoDepth() == depth + 1,
          "Delete over every CV removes down to the 2-CV minimum as one "
          "step (%d CVs, depth %d -> %d, %r)"
          % (len(groom.centers(0)), depth, groom.undoDepth(),
             groom.since(mark)))
    check(any("refused" in line for line in groom.since(mark)),
          "and says the rest were refused (%r)" % groom.since(mark))
    selectCenters(2)
    depth = groom.undoDepth()
    mark = len(groom.messages)
    typeKey(view, "delete")
    wait(20)
    check(len(groom.centers(0)) == 2 and groom.undoDepth() == depth and
          not session.gestureActive,
          "a Delete whose every removal is refused leaves no undo step "
          "(depth %d -> %d)" % (depth, groom.undoDepth()))
    check(len(session.readSelection(CENTER)) == 2,
          "and keeps the refused CVs selected (%r)"
          % (session.readSelection(CENTER),))
    check(any("nothing removed" in line for line in groom.since(mark)),
          "the message says nothing was removed (%r)" % groom.since(mark))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(same(groom.centers(0), straight),
          "so one Ctrl+Z brings every deleted CV back")

    # -- a loop cancel that raises still closes the drag's bracket --------
    session.clearSelection()
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    mid = handlePixel(0, 2)
    loop = viewport.loop
    realCancel = loop.cancel

    def explodingCancel():
        raise RuntimeError("T3: cancel exploded")
    depth = groom.undoDepth()
    if mid is not None:
        mouse.press(mid)
        mouse.move((mid[0] + 12.0, mid[1]))
        mouse.move((mid[0] + 24.0, mid[1]))
        wait(10)
        check(session.gestureActive,
              "a live tweak-drag holds the session bracket")
        loop.cancel = explodingCancel
        try:
            typeKey(view, "escape")
            wait(20)
        finally:
            loop.cancel = realCancel
        mouse.release((mid[0] + 24.0, mid[1]))
        wait(20)
        check(not session.gestureActive and groom.undoDepth() == depth and
              same(groom.centers(0), straight),
              "Escape over a raising loop cancel still rolls the drag back "
              "and closes its bracket (active %r, depth %d -> %d)"
              % (session.gestureActive, depth, groom.undoDepth()))
        mouse.drag([mid] + [(mid[0] + 12.0 * s, mid[1])
                            for s in range(1, 4)])
        wait(20)
        check(groom.undoDepth() == depth + 1,
              "and the next drag opens its own step (%d -> %d)"
              % (depth, groom.undoDepth()))
        typeKey(view, "z", ("ctrl",))
        wait(20)

    typeKey(view, "f9")
    groom.close()
    print("testUsdviewPomadeTubeActions: %d failure(s)"
          % pomadeT3.failureCount())
    return 1 if pomadeT3.failureCount() else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeTubeActions needs testusdview (no live "
          "view)")
    sys.exit(0)
