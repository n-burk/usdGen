# testUsdviewTonicGraphTools -- T3 (TS-03): the Graph click tools driven by
# REAL mouse and key events over the StageView.
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicGraphTools.py \
#               examples/tonic-graph-scalp.usda
#
# testUsdviewTonicGraph covers Draw, Place and Weld. This script covers the
# rest of the shelf, each through its hotkey and real clicks, asserting on
# graphCounts(), the selection, the undo stack, the status line, what the
# scene index published and what the committer wrote:
#
#   * C Connect: a first click arms (and draws) one node, a second joins
#     it to another; a diagonal splits the square into two regions and the
#     new region gets its own L1 stub (G14);
#   * L Link: two region clicks share one interpolation id, which reaches
#     /TonicGroom/ScalpGraph.linkedRegions; Escape drops an armed region;
#   * U Unweld: a click on a node two regions share splits it per region;
#   * Shift+W over a box-selected coincident pair welds it back, Shift+U
#     over one Shift-clicked node unwelds it again;
#   * X Delete: an edge click merges the regions it separated, a node click
#     takes the node and its edges, a click on nothing says so;
#   * every edit is one undo step and Ctrl+Z walks them back.
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


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import tonicT3
    from tonicT3 import check, info, typeKey, wait
    try:
        from usdGenTonicTools import tonicLib
        import tonicT3Groom
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1

    groom = tonicT3Groom.Groom(appController)
    if not groom.build():
        groom.close()
        print("testUsdviewTonicGraphTools: %d failure(s)"
              % tonicT3.failureCount())
        return 1
    session, viewport, state = groom.session, groom.viewport, groom.state
    view, mouse, stage = groom.view, groom.mouse, groom.stage
    RECT = tonicT3Groom.RECT
    NODE = tonicLib.TONIC_PICK_GRAPH_NODE
    REGION = tonicLib.TONIC_PICK_REGION

    def counts():
        return session.graphCounts()

    def selected(kind):
        return sorted(item[0] for item in session.readSelection(kind))

    def at(x, z):
        return groom.scalpPixel(x, z)

    def undoTo(depth):
        """Ctrl+Z until the undo stack is back at `depth` (max 8 presses)."""
        for _ in range(8):
            if groom.undoDepth() <= depth:
                break
            typeKey(view, "z", ("ctrl",))
            wait(20)
        return groom.undoDepth() == depth

    def redoTo(depth):
        """Ctrl+Y until the undo stack is back at `depth` (max 8)."""
        for _ in range(8):
            if groom.undoDepth() >= depth or session.redoDepth() <= 0:
                break
            typeKey(view, "y", ("ctrl",))
            wait(20)
        return groom.undoDepth() == depth

    groom.top()
    baseDepth = groom.undoDepth()

    # -- C: Connect a diagonal ---------------------------------------------
    typeKey(view, "c")
    check(state.graphSubMode == "connect",
          "C selects the Connect sub-mode (%r)" % state.graphSubMode)
    mark = len(groom.messages)
    mouse.click(at(*RECT[0]))
    wait(10)
    armed = selected(NODE)
    check(len(armed) == 1 and counts() == (4, 4, 1),
          "a first Connect click arms one node and draws it selected "
          "(%r, %r)" % (armed, counts()))
    check(any("Esc cancels" in m for m in groom.since(mark)),
          "and the status says how to back out (%r)" % groom.since(mark))
    check(groom.undoDepth() == baseDepth,
          "arming leaves no undo step (%d -> %d)"
          % (baseDepth, groom.undoDepth()))
    mouse.click(at(*RECT[2]))
    wait(20)
    check(counts() == (4, 5, 2),
          "the second click joins the diagonal: one more edge and the "
          "square split in two (%r, %r)" % (counts(), groom.since(mark)))
    check(groom.undoDepth() == baseDepth + 1,
          "as exactly one undo step (%d)" % groom.undoDepth())
    check(selected(NODE) == [],
          "the spent first pick leaves the selection (%r)"
          % (selected(NODE),))
    check(any("connected (ok)" in m for m in groom.since(mark)),
          "and the status reports the join (%r)" % groom.since(mark)[-2:])
    roots = groom.l1Tubes()
    check(len(roots) == 2,
          "G14: the new region grew its own L1 stub (%r)" % (roots,))
    session.publishAll()
    published = tonicT3.levelInfo(session, 1)
    check(published is not None and published[2] == 2,
          "/__usdGenTonic/tubes/L1 publishes both stubs (%r)"
          % (published,))
    # One click, one step: the first Ctrl+Z must take the whole Connect
    # back, not just the stub the edit grew behind it.
    connectDepth = groom.undoDepth()
    label = session.undoLabel(0)
    info("undo labels after the Connect click: %r"
         % ([session.undoLabel(i) for i in range(connectDepth)],))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    info("after one Ctrl+Z: counts %r, L1 roots %r, status %r"
         % (counts(), groom.l1Tubes(), groom.messages[-1:]))
    check(counts() == (4, 4, 1),
          "one Ctrl+Z undoes the whole Connect click (after it %r; the "
          "step undone was %r)" % (counts(), label))
    check(redoTo(connectDepth) and counts() == (4, 5, 2) and
          len(groom.l1Tubes()) == 2,
          "Ctrl+Y brings the diagonal and its stub back (%r, %r)"
          % (counts(), groom.l1Tubes()))

    # -- L: Link the two regions -------------------------------------------
    typeKey(view, "l")
    check(state.graphSubMode == "link",
          "L selects the Link sub-mode (%r)" % state.graphSubMode)
    below = (2.6, 1.4)          # z < x: one side of the (1,1)-(3,3) cut
    above = (1.4, 2.6)          # z > x: the other
    mark = len(groom.messages)
    mouse.click(at(*below))
    wait(10)
    check(len(selected(REGION)) == 1,
          "a first Link click arms and draws one region (%r, %r)"
          % (selected(REGION), groom.since(mark)))
    typeKey(view, "escape")
    wait(10)
    check(selected(REGION) == [] and
          any("link cancelled" in m for m in groom.since(mark)),
          "Escape drops the armed region and says so (%r)"
          % groom.since(mark)[-1:])
    mark = len(groom.messages)
    mouse.click(at(*below))
    wait(10)
    firstRegion = selected(REGION)
    mouse.click(at(*above))
    wait(20)
    check(any("linked (ok)" in m for m in groom.since(mark)),
          "two region clicks link them (%r)" % groom.since(mark)[-2:])
    check(counts() == (4, 5, 2) and groom.undoDepth() == connectDepth + 1,
          "without touching the graph, as one undo step (%r, depth %d)"
          % (counts(), groom.undoDepth()))
    check(groom.commit(), "the idle pump commits the linked groom")
    graphPrim = stage.GetPrimAtPath("/TonicGroom/ScalpGraph")
    linked = None
    if graphPrim:
        attr = graphPrim.GetAttribute("usdGen:tonic:linkedRegions")
        linked = attr.Get() if attr else None
    check(linked is not None and len(linked) >= 1,
          "/TonicGroom/ScalpGraph.linkedRegions carries the pair "
          "(%r, first pick %r)" % (linked, firstRegion))
    faceIds = (graphPrim.GetAttribute("usdGen:tonic:nodeFaceIds").Get()
               if graphPrim else None)
    check(faceIds is not None and len(faceIds) == 4,
          "and the committed graph still has four nodes (%r)"
          % (None if faceIds is None else len(faceIds),))
    linkDepth = groom.undoDepth()

    # -- U: Unweld a node both regions share -------------------------------
    typeKey(view, "u")
    check(state.graphSubMode == "unweld",
          "U selects the Unweld sub-mode (%r)" % state.graphSubMode)
    mark = len(groom.messages)
    mouse.click(at(*RECT[0]))
    wait(20)
    check(counts()[0] == 5,
          "an Unweld click on the shared corner splits it per region "
          "(%r, %r)" % (counts(), groom.since(mark)))
    check(any("unwelded into 2" in m for m in groom.since(mark)),
          "and the status counts the pieces (%r)" % groom.since(mark)[-1:])
    check(groom.undoDepth() == linkDepth + 1,
          "as one undo step (%d)" % groom.undoDepth())
    info("the unweld left %r (the diagonal still joins the two halves)"
         % (counts(),))

    # -- Shift+W: box the coincident pair, weld it -------------------------
    corner = at(*RECT[0])
    start = (corner[0] - 25.0, corner[1] - 25.0)
    end = (corner[0] + 25.0, corner[1] + 25.0)
    probe = session.pickItem(groom.camera, start[0], start[1],
                             viewport.loop.pickRadiusPx(), NODE)
    check(probe is None, "the band starts off every node (%r)" % (probe,))
    mouse.drag([start, ((start[0] + end[0]) * 0.5,
                        (start[1] + end[1]) * 0.5), end])
    wait(10)
    pair = selected(NODE)
    check(len(pair) == 2 and counts()[0] == 5,
          "a plain drag from empty space in Unweld boxes both halves of "
          "the corner without editing (%r, %r)" % (pair, counts()))
    mark = len(groom.messages)
    depth = groom.undoDepth()
    typeKey(view, "w", ("shift",))
    wait(20)
    check(counts()[0] == 4,
          "Shift+W welds the two selected nodes back into one (%r, %r)"
          % (counts(), groom.since(mark)))
    check(any("welded (ok)" in m for m in groom.since(mark)),
          "and says so (%r)" % groom.since(mark)[-2:])
    check(groom.undoDepth() == depth + 1,
          "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))

    # -- Shift+U: one Shift-clicked node ------------------------------------
    typeKey(view, "a", ("ctrl", "shift"))
    wait(10)
    check(selected(NODE) == [],
          "Ctrl+Shift+A clears the node selection (%r)" % (selected(NODE),))
    mouse.click(corner, ("shift",))
    wait(10)
    one = selected(NODE)
    check(len(one) == 1 and counts()[0] == 4,
          "a Shift-click in Unweld only selects the node (%r, %r)"
          % (one, counts()))
    mark = len(groom.messages)
    depth = groom.undoDepth()
    typeKey(view, "u", ("shift",))
    wait(20)
    check(counts()[0] == 5 and
          any("unwelded into 2" in m for m in groom.since(mark)),
          "Shift+U unwelds the selected shared node (%r, %r)"
          % (counts(), groom.since(mark)))
    check(groom.undoDepth() == depth + 1,
          "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))
    # Back to the linked, connected square.
    check(undoTo(linkDepth) and counts() == (4, 5, 2),
          "Ctrl+Z walks the unweld/weld/unweld back to the linked square "
          "(%r, depth %d)" % (counts(), groom.undoDepth()))

    # -- X: Delete an edge, then a node ------------------------------------
    typeKey(view, "x")
    check(state.graphSubMode == "delete",
          "X selects the Delete sub-mode (%r)" % state.graphSubMode)
    mark = len(groom.messages)
    mouse.click(at(0.3, 3.7))
    wait(10)
    check(counts() == (4, 5, 2) and
          any("no node or edge here" in m for m in groom.since(mark)),
          "a Delete click on bare scalp says there is nothing there "
          "(%r, %r)" % (counts(), groom.since(mark)[-1:]))
    depth = groom.undoDepth()
    mark = len(groom.messages)
    mouse.click(at(1.6, 1.6))
    wait(20)
    check(counts() == (4, 4, 1),
          "a click on the diagonal deletes the edge and merges its two "
          "regions (%r, %r)" % (counts(), groom.since(mark)))
    check(any("edge deleted" in m for m in groom.since(mark)),
          "and names what went (%r)" % groom.since(mark)[-2:])
    check(groom.undoDepth() == depth + 1,
          "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))
    mark = len(groom.messages)
    mouse.click(at(*RECT[1]))
    wait(20)
    # The graph is polygon-style: a deleted loop node's two neighbours are
    # joined, so the square becomes a triangle rather than opening.
    check(counts() == (3, 3, 1),
          "a corner click deletes the node and bridges its neighbours: "
          "the square becomes a closed triangle (%r, %r)"
          % (counts(), groom.since(mark)))
    check(any("node deleted" in m for m in groom.since(mark)),
          "and names what went (%r)" % groom.since(mark)[-2:])
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(counts() == (4, 4, 1),
          "Ctrl+Z puts the node and its edges back (%r)" % (counts(),))
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(counts() == (4, 5, 2),
          "a second Ctrl+Z restores the deleted diagonal (%r)"
          % (counts(),))
    check(undoTo(baseDepth) and counts() == (4, 4, 1),
          "and Ctrl+Z back past Link and Connect returns the stroked square "
          "(%r, depth %d)" % (counts(), groom.undoDepth()))

    # -- a Draw press while another owner's bracket is open ----------------
    # (a dock slider whose release was lost, say): the stroke is refused
    # with a reason and the other bracket is neither sealed nor rolled back.
    check(session.beginGesture("T3 foreign bracket"),
          "the T3 opens a foreign session bracket")
    depth = groom.undoDepth()
    mark = len(groom.messages)
    drawn = groom.stroke(((0.1, 0.1), (0.8, 0.1), (0.8, 0.8), (0.1, 0.8)))
    check(drawn == (4, 4, 1),
          "a Draw stroke under a foreign bracket authors nothing (%r)"
          % (drawn,))
    check(session.gestureActive and groom.undoDepth() == depth,
          "and leaves that bracket open and unchanged (active %r, depth "
          "%d -> %d)" % (session.gestureActive, depth, groom.undoDepth()))
    check(any("another edit is still open" in m for m in groom.since(mark)),
          "the status says why (%r)" % groom.since(mark)[-2:])
    typeKey(view, "escape")
    wait(20)
    check(session.gestureActive,
          "an idle Escape does not close the foreign bracket either")
    session.publish(session.cancelGesture())
    check(not session.gestureActive and undoTo(baseDepth),
          "the owner's cancel closes it (depth %d)" % groom.undoDepth())

    typeKey(view, "d")
    groom.close()
    print("testUsdviewTonicGraphTools: %d failure(s)" % tonicT3.failureCount())
    return 1 if tonicT3.failureCount() else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicGraphTools needs testusdview (no live view)")
    sys.exit(0)
