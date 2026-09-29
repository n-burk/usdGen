# testUsdviewPomadeReposition -- real Qt acceptance for Graph Reposition.
#
# Uses the single-quad fixture to create a shared region pair plus a detached
# region, sculpts/subdivides one root and tweaks one child, then drives M with
# actual StageView press/move/release events. Reads are public graph/tube ABI
# queries; no graph or hierarchy mutation bypasses the UI.
import ctypes
import math
import os
import sys


failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for index, argument in enumerate(sys.argv):
        if argument == "--testScript" and index + 1 < len(sys.argv):
            return os.path.dirname(os.path.abspath(sys.argv[index + 1]))
        if argument.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(argument.split("=", 1)[1]))
    return ""


def typeKey(view, name, modifiers=()):
    from pxr.Usdviewq.qt import QtCore, PySideModule
    import importlib
    QtTest = importlib.import_module("%s.QtTest" % PySideModule)
    keys = {"m": QtCore.Qt.Key.Key_M,
            "r": QtCore.Qt.Key.Key_R,
            "return": QtCore.Qt.Key.Key_Return,
            "escape": QtCore.Qt.Key.Key_Escape,
            "z": QtCore.Qt.Key.Key_Z,
            "y": QtCore.Qt.Key.Key_Y}
    mods = QtCore.Qt.KeyboardModifier.NoModifier
    for modifier in modifiers:
        mods |= {"ctrl": QtCore.Qt.KeyboardModifier.ControlModifier}[modifier]
    QtTest.QTest.keyClick(view, keys[name], mods)


def same(left, right, tolerance=1e-5):
    if left is None or right is None:
        return left is right
    if isinstance(left, (tuple, list)):
        return len(left) == len(right) and all(
            same(a, b, tolerance) for a, b in zip(left, right))
    return abs(float(left) - float(right)) <= tolerance


def graphEdges(session):
    count = ctypes.c_int(0)
    session.dll.Pomade_ReadGraphEdges(session.model, None, 0,
                                     ctypes.byref(count))
    pairs = (ctypes.c_int * max(2 * int(count.value), 2))()
    if session.dll.Pomade_ReadGraphEdges(session.model, pairs,
                                        max(int(count.value), 1),
                                        ctypes.byref(count)) != 0:
        return ()
    return tuple(sorted(tuple(sorted((int(pairs[2 * index]),
                                     int(pairs[2 * index + 1]))))
                        for index in range(int(count.value))))


def displayPoint(session, nodeId):
    point = (ctypes.c_float * 3)()
    entry = getattr(session.dll, "Pomade_GraphGetNodeDisplayPosition", None)
    if entry is None or entry(session.model, int(nodeId), point) != 0:
        return None
    return tuple(float(point[axis]) for axis in range(3))


def graphHover(session):
    """The graph affordance published by a real, press-less mouse move."""
    kind = ctypes.c_uint(0)
    ident = ctypes.c_int(-1)
    sub = ctypes.c_int(-1)
    subsub = ctypes.c_int(-1)
    if session.dll.Pomade_GetHover(session.model, ctypes.byref(kind),
                                  ctypes.byref(ident), ctypes.byref(sub),
                                  ctypes.byref(subsub)) != 0:
        return None
    return (int(kind.value), int(ident.value), int(sub.value),
            int(subsub.value))


def nodeSnapshot(session, nodeIds):
    from testUsdviewPomadeCvRegions import graphNode, regionLoops
    values = []
    for nodeId in sorted(nodeIds):
        node = graphNode(session, nodeId)
        values.append((int(nodeId), node))
    return (tuple(values), graphEdges(session), tuple(regionLoops(session)),
            tuple(session.graphCounts()))


def tubeSnapshot(session, tubeIds):
    from usdGenPomadeTools import pomadeBridge
    out = []
    for tubeId in sorted(tubeIds):
        centers = tuple(tuple(float(value) for value in point)
                        for point in pomadeBridge.tubeCenters(
                            session.dll, session.model, tubeId))
        sections = []
        for ring in range(pomadeBridge.tubeSectionCount(session.dll,
                                                        session.model, tubeId)):
            t, uv, scale, twist = pomadeBridge.tubeSection(
                session.dll, session.model, tubeId, ring)
            sections.append((float(t), tuple(tuple(float(value) for value in p)
                                              for p in uv), float(scale),
                             float(twist)))
        out.append((int(tubeId), centers, tuple(sections)))
    return tuple(out)


def sectionWorldPoint(session, tubeId, ring, slot):
    """One world-space section CV, after its authored frame and twist."""
    from usdGenPomadeTools import pomadeBridge
    _t, uv, scale, twist = pomadeBridge.tubeSection(session.dll, session.model,
                                                    tubeId, ring)
    if slot < 0 or slot >= len(uv):
        return None
    origin = (ctypes.c_float * 3)()
    frame = (ctypes.c_float * 9)()
    frameEntry = session.dll.Pomade_GetTubeSectionFrame
    if frameEntry(session.model, int(tubeId), int(ring), origin, frame,
                  ctypes.byref(ctypes.c_float()),
                  ctypes.byref(ctypes.c_float())) != 0:
        return None
    c, s = math.cos(float(twist)), math.sin(float(twist))
    placed = [(float(u) * float(scale), float(v) * float(scale))
              for u, v in uv]
    placed = [(u * c - v * s, u * s + v * c) for u, v in placed]
    meanU = sum(pair[0] for pair in placed) / len(placed)
    meanV = sum(pair[1] for pair in placed) / len(placed)
    u, v = placed[slot]
    return tuple(float(origin[axis]) + float(frame[axis]) * (u - meanU) +
                 float(frame[axis + 3]) * (v - meanV)
                 for axis in range(3))


def tubeWorldSnapshot(session, tubeIds):
    """Only world geometry: authored UV/delta fields may legitimately rebase."""
    from usdGenPomadeTools import pomadeBridge
    records = []
    for tubeId in sorted(int(tube) for tube in tubeIds):
        centers = tuple(tuple(float(value) for value in point)
                        for point in pomadeBridge.tubeCenters(
                            session.dll, session.model, tubeId))
        sections = []
        for ring in range(pomadeBridge.tubeSectionCount(session.dll,
                                                        session.model, tubeId)):
            _t, uv, _scale, _twist = pomadeBridge.tubeSection(
                session.dll, session.model, tubeId, ring)
            sections.extend(sectionWorldPoint(session, tubeId, ring, slot)
                            for slot in range(len(uv)))
        records.append((tubeId, centers, tuple(sections)))
    return tuple(records)


def rootUpperWorldSnapshot(session, tubeIds):
    """Root centers plus sections above ring 0, excluding the fitted base."""
    from usdGenPomadeTools import pomadeBridge
    records = []
    for tubeId in sorted(int(tube) for tube in tubeIds):
        centers = tuple(tuple(float(value) for value in point)
                        for point in pomadeBridge.tubeCenters(
                            session.dll, session.model, tubeId))
        sections = []
        for ring in range(1, pomadeBridge.tubeSectionCount(
                session.dll, session.model, tubeId)):
            _t, uv, _scale, _twist = pomadeBridge.tubeSection(
                session.dll, session.model, tubeId, ring)
            sections.extend(sectionWorldPoint(session, tubeId, ring, slot)
                            for slot in range(len(uv)))
        records.append((tubeId, centers, tuple(sections)))
    return tuple(records)


def _sub(left, right):
    return tuple(float(left[axis]) - float(right[axis]) for axis in range(3))


def _add(left, right):
    return tuple(float(left[axis]) + float(right[axis]) for axis in range(3))


def _scale(point, amount):
    return tuple(float(amount) * float(point[axis]) for axis in range(3))


def _dot(left, right):
    return sum(float(left[axis]) * float(right[axis]) for axis in range(3))


def _distance(left, right):
    return math.sqrt(_dot(_sub(left, right), _sub(left, right)))


def _normalise(vector):
    length = math.sqrt(_dot(vector, vector))
    return None if length <= 1e-12 else _scale(vector, 1.0 / length)


def _sectionPlane(session, tubeId, ring=0):
    """Origin/normal of one public K5 section frame."""
    origin = (ctypes.c_float * 3)()
    frame = (ctypes.c_float * 9)()
    entry = getattr(session.dll, "Pomade_GetTubeSectionFrame", None)
    if entry is None or entry(session.model, int(tubeId), int(ring), origin,
                              frame, ctypes.byref(ctypes.c_float()),
                              ctypes.byref(ctypes.c_float())) != 0:
        return None
    u = tuple(float(frame[axis]) for axis in range(3))
    v = tuple(float(frame[axis + 3]) for axis in range(3))
    normal = _normalise((u[1] * v[2] - u[2] * v[1],
                         u[2] * v[0] - u[0] * v[2],
                         u[0] * v[1] - u[1] * v[0]))
    if normal is None:
        return None
    return (tuple(float(origin[axis]) for axis in range(3)), normal)


def _projectToPlane(point, plane):
    origin, normal = plane
    return _sub(point, _scale(normal, _dot(_sub(point, origin), normal)))


def _closestBoundaryParameter(point, boundary):
    """Closest stable edge/alpha for one base slot on a closed boundary."""
    if len(boundary) < 3:
        return None
    best = None
    for edge, start in enumerate(boundary):
        end = boundary[(edge + 1) % len(boundary)]
        vector = _sub(end, start)
        length2 = _dot(vector, vector)
        alpha = (0.0 if length2 <= 1e-14 else
                 max(0.0, min(1.0, _dot(_sub(point, start), vector) /
                              length2)))
        candidate = _add(start, _scale(vector, alpha))
        error = _distance(point, candidate)
        if best is None or error < best[2]:
            best = (edge, alpha, error)
    return best


def rootBaseFootprint(session, tubeId):
    """Stable base-slot mapping from an L1 root to its live region boundary."""
    from testUsdviewPomadeCvRegions import graphNode, regionLoops
    from usdGenPomadeTools import pomadeBridge
    region = int(session.dll.Pomade_RegionForTube(session.model, int(tubeId)))
    loops = regionLoops(session)
    if region < 0 or region >= len(loops):
        return None
    plane = _sectionPlane(session, tubeId, 0)
    _t, uv, _scaleValue, _twist = pomadeBridge.tubeSection(
        session.dll, session.model, tubeId, 0)
    if plane is None or not uv:
        return None
    corners = tuple(_projectToPlane(graphNode(session, node)[2], plane)
                    for node in loops[region])
    base = tuple(sectionWorldPoint(session, tubeId, 0, slot)
                 for slot in range(len(uv)))
    if any(point is None for point in base):
        return None
    mapping = tuple(_closestBoundaryParameter(point, corners) for point in base)
    if any(item is None for item in mapping):
        return None
    return {"tube": int(tubeId), "region": region, "base": base,
            "corners": corners, "mapping": mapping,
            "mapError": max(item[2] for item in mapping)}


def rootBaseConforms(session, before, tolerance=4e-4):
    """The moved root base follows its region's current polygon exactly.

    The initial slot-to-edge mapping is retained, so an auto-resampled base
    with slots between graph corners remains tested instead of silently
    reducing the comparison to a centroid or a bounding box.
    """
    current = rootBaseFootprint(session, before["tube"])
    if current is None or current["region"] != before["region"] or \
            len(current["base"]) != len(before["mapping"]) or \
            len(current["corners"]) != len(before["corners"]):
        return (False, None, None)
    expected = []
    for edge, alpha, _oldError in before["mapping"]:
        start = current["corners"][edge]
        end = current["corners"][(edge + 1) % len(current["corners"])]
        expected.append(_add(start, _scale(_sub(end, start), alpha)))
    slotError = max(_distance(actual, target)
                    for actual, target in zip(current["base"], expected))
    cornerError = max(min(_distance(corner, slot) for slot in current["base"])
                      for corner in current["corners"])
    return (slotError <= tolerance and cornerError <= tolerance,
            slotError, cornerError)


def tubeLayout(snapshot):
    """Identity/count layout remains stable while K6 refreshes child layouts."""
    return tuple((int(tube), len(centers),
                  tuple(len(section[1]) for section in sections))
                 for tube, centers, sections in snapshot)


def tubeDeltas(session, tubeId):
    """Public K6 residual vector, read as an upper-sculpt presence witness."""
    count = ctypes.c_int(0)
    entry = getattr(session.dll, "Pomade_ReadTubeDeltas", None)
    if entry is None or entry(session.model, int(tubeId), None, 0,
                              ctypes.byref(count)) != 0:
        return None
    values = (ctypes.c_float * max(1, int(count.value)))()
    if entry(session.model, int(tubeId), values, int(count.value),
             ctypes.byref(count)) != 0:
        return None
    return tuple(float(values[index]) for index in range(int(count.value)))


def centerResidual(deltas, cv):
    """One authored K6 center residual; root CV 0 is intentionally excluded."""
    start = 3 * int(cv)
    if deltas is None or start < 3 or start + 3 > len(deltas):
        return None
    return tuple(float(deltas[start + axis]) for axis in range(3))


def centerResidualNorm(residual):
    return (None if residual is None else
            math.sqrt(sum(float(value) * float(value) for value in residual)))


def finiteWorld(snapshot):
    return all(math.isfinite(float(value))
               for _tube, centers, sections in snapshot
               for point in tuple(centers) + tuple(sections)
               for value in point)


def sameWorld(left, right, tolerance=1e-5):
    return same(left, right, tolerance)


def worldSubset(snapshot, tubeIds):
    wanted = set(int(tube) for tube in tubeIds)
    return tuple(record for record in snapshot if int(record[0]) in wanted)


def translatedWorld(before, after, tubeIds, tolerance=3e-4):
    """Shared translation for every center and world section point in a subtree."""
    old = dict((int(tube), tuple(centers) + tuple(sections))
               for tube, centers, sections in before)
    new = dict((int(tube), tuple(centers) + tuple(sections))
               for tube, centers, sections in after)
    deltas = []
    for tubeId in tubeIds:
        oldPoints, newPoints = old.get(int(tubeId), ()), new.get(int(tubeId), ())
        if not oldPoints or len(oldPoints) != len(newPoints):
            return None
        delta = tuple(float(newPoints[0][axis]) - float(oldPoints[0][axis])
                      for axis in range(3))
        if any(any(abs((float(newPoint[axis]) - float(oldPoint[axis])) -
                       delta[axis]) > tolerance for axis in range(3))
               for oldPoint, newPoint in zip(oldPoints, newPoints)):
            return None
        deltas.append(delta)
    if not deltas:
        return None
    reference = deltas[0]
    if any(any(abs(delta[axis] - reference[axis]) > tolerance
               for axis in range(3)) for delta in deltas[1:]):
        return None
    return reference


def descendants(session, tubeId):
    out = (ctypes.c_int * 64)()
    count = ctypes.c_int(0)
    if session.dll.Pomade_GetTubeChildren(session.model, int(tubeId), out, 64,
                                         ctypes.byref(count)) != 0:
        return ()
    direct = tuple(int(out[index]) for index in range(int(count.value)))
    return direct + tuple(child for node in direct
                          for child in descendants(session, node))


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..", "python")))
    try:
        import usdGenPomadeTools
        from usdGenPomadeTools import pomadeCamera, pomadeLib, pomadeLoops, pomadePanels
        from testUsdviewPomadeCvRegions import (EDGE_NEIGHBOUR, LEFT, RIGHT,
                                                _bindGeometryFromDock,
                                                frameScalp, graphNode,
                                                l1TubeIds, regionLoops, wait)
        from testUsdviewPomadeGraph import Mouse
    except ImportError as exc:
        print("FAIL: cannot import Reposition helpers: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenPomadeTools.container()
    if view is None or registry is None or container is None:
        print("FAIL: usdview did not provide a StageView, registry and container")
        return 1
    check(frameScalp(stage, view), "the stable graph camera is active")
    view.setFocus()
    wait(30)

    registry.getCommandPlugin("usdGenPomadeTools.openWorkspace").run()
    workspace = container.workspace
    if workspace is None:
        print("FAIL: Pomade workspace did not open")
        return 1
    _bindGeometryFromDock(workspace)
    session, viewport, state = container.session, container.viewport, container.pomadeState
    messages = []
    if session is not None:
        session.setStatusSink(messages.append)

    def shutdown():
        try:
            viewport.uninstall()
        finally:
            session.deactivate()

    check(session is not None and session.model is not None and viewport is not None,
          "Bind Geometry created a live graph session")
    if session is None or session.model is None or viewport is None:
        return 1
    camera = pomadeCamera.resolve(view)
    check(camera is not None, "the StageView camera resolves for real events")
    if camera is None:
        shutdown()
        return 1

    def pixel(point):
        projected = camera.worldToPixels(point)
        return (projected[0], projected[1]) if projected is not None else None

    def nodePixel(nodeId):
        point = displayPoint(session, nodeId)
        return pixel(point) if point is not None else None

    def capture(path):
        if not path:
            return
        directory = os.path.dirname(os.path.abspath(path))
        if directory and not os.path.isdir(directory):
            os.makedirs(directory)
        # Whole-window capture retains the Qt overlay, dock mode state and
        # the live graph region paint instead of only Hydra's framebuffer.
        view.window().grab().save(path)

    def captureHeldEdge():
        capture(os.environ.get("USDGEN_POMADE_REPOSITION_CAPTURE", ""))

    def capturePrehighlight():
        path = os.environ.get("USDGEN_POMADE_REPOSITION_PREHIGHLIGHT_CAPTURE", "")
        if not path:
            edgePath = os.environ.get("USDGEN_POMADE_REPOSITION_CAPTURE", "")
            if edgePath:
                stem, extension = os.path.splitext(edgePath)
                path = stem + "-prehighlight" + (extension or ".png")
        capture(path)

    def sourceIdAt(loop, wanted):
        candidates = []
        for nodeId in loop:
            node = graphNode(session, nodeId)
            if node is not None:
                x, _y, z = node[2]
                candidates.append(((x - wanted[0]) ** 2 + (z - wanted[1]) ** 2,
                                   int(nodeId)))
        return min(candidates)[1] if candidates and min(candidates)[0] < 2.5e-3 else -1

    mouse = Mouse(view)
    mouse.direct = True

    def eventPixel(projected):
        """The physical pixel actually delivered by Mouse's Qt event."""
        if projected is None:
            return None
        point = mouse._point(projected)
        return (float(point.x()) * mouse._ratio,
                float(point.y()) * mouse._ratio)

    def pointerEvent(kind):
        """Send the StageView's real idle enter/leave event through Qt."""
        event = mouse._QtCore.QEvent(kind)
        return mouse._QtWidgets.QApplication.sendEvent(view, event)

    def clickDockControl(actionId):
        """Invoke one dock action button through Qt, never a hierarchy API.

        Found by Action.id through workspace.button (DK-04): by text,
        "Subdivide" is also the Hierarchy sub-mode button's label."""
        button = workspace.button("action", actionId)
        if button is None:
            return False
        button.click()
        wait(20)
        return True

    # Region mode starts by creating LEFT, then creates its neighbour from
    # the same two committed endpoint dots.  These are real clicks and a
    # closure click, never GraphCreateRegion calls from the test.
    for point in LEFT:
        mouse.click(pixel((point[0], 0.0, point[1])))
    viewport.setPointerInside(True)
    view.setFocus()
    typeKey(view, "return")
    firstLoops = regionLoops(session)
    check(len(firstLoops) == 1 and session.graphCounts() == (3, 3, 1),
          "three Region clicks commit the first artist region")
    typeKey(view, "m")
    check(state.graphSubMode == "reposition",
          "M routes the real StageView into Graph Reposition")
    # M is intentionally selected only after the first region. Restore
    # Region via the visible shelf so the neighbour construction is UI-only.
    workspace._subModeButtons["region"].click()
    firstLoop = firstLoops[0] if firstLoops else ()
    sharedA, sharedB = sourceIdAt(firstLoop, LEFT[0]), sourceIdAt(firstLoop, LEFT[1])
    aPixel, bPixel = nodePixel(sharedA), nodePixel(sharedB)
    if aPixel is not None and bPixel is not None:
        mouse.click(aPixel)
        mouse.click(bPixel)
        mouse.click(pixel((EDGE_NEIGHBOUR[2][0], 0.0, EDGE_NEIGHBOUR[2][1])))
        mouse.click(aPixel)
    # A disconnected third region proves that attachment transport stays
    # local to moved contours, rather than shifting every groom root.
    for point in RIGHT:
        mouse.click(pixel((point[0], 0.0, point[1])))
    mouse.click(pixel((RIGHT[0][0], 0.0, RIGHT[0][1])))
    sharedLoops = regionLoops(session)
    allNodes = {node for loop in sharedLoops for node in loop}
    sharedEdge = tuple(sorted((sharedA, sharedB)))
    check(len(sharedLoops) == 3 and session.graphCounts() == (7, 8, 3) and
          sum(1 for loop in sharedLoops if sharedA in loop and sharedB in loop) == 2 and
          sharedEdge in graphEdges(session),
          "real Region clicks create a shared pair plus one disconnected region")
    roots = tuple(sorted(int(tube) for tube in l1TubeIds(session)))
    attachedRegions = tuple(index for index, loop in enumerate(sharedLoops)
                            if sharedA in loop or sharedB in loop)
    attachedRoots = tuple(int(session.dll.Pomade_TubeForRegion(
        session.model, region)) for region in attachedRegions)
    unrelatedRegions = tuple(index for index in range(len(sharedLoops))
                             if index not in attachedRegions)
    unrelatedRoots = tuple(int(session.dll.Pomade_TubeForRegion(
        session.model, region)) for region in unrelatedRegions)
    check(len(roots) == 3 and len(attachedRoots) == 2 and
          all(root >= 0 for root in attachedRoots) and
          len(unrelatedRoots) == 1 and unrelatedRoots[0] >= 0,
          "the shared pair and disconnected region own distinct root tubes")
    if len(sharedLoops) != 3 or sharedA < 0 or sharedB < 0 or \
            len(attachedRoots) != 2 or not unrelatedRoots:
        shutdown()
        return 1

    # Discover an actual visible root of the shared pair through Hierarchy's
    # normal surface picker.  The picked owner must be the candidate whose
    # exposed section point we clicked: accepting another attached owner here
    # makes the following side-view Sculpt check test a different tube.
    from testUsdviewPomadeSculpt import aimCamera
    from usdGenPomadeTools import pomadeBridge
    transportedRoot = -1
    rootPickTrace = []
    for candidate in attachedRoots:
        # Slot 0 is on the shared boundary and legitimately loses to the
        # neighbouring surface.  Probe the third authored corner first, then
        # retain the other slots as a real visible-surface fallback.
        ring = min(1, pomadeBridge.tubeSectionCount(session.dll, session.model,
                                                   candidate) - 1)
        _t, candidateUV, _scale, _twist = pomadeBridge.tubeSection(
            session.dll, session.model, candidate, ring)
        slots = list(range(len(candidateUV)))
        if len(slots) > 2:
            slots.insert(0, slots.pop(2))
        # Keep the stable top camera here. Centering a side camera on either
        # root makes the unrelated tube project over both compact rings.
        if ring < 0 or not frameScalp(stage, view):
            continue
        wait(35)
        workspace._modeButtons["hierarchy"].click()
        wait(15)
        camera = pomadeCamera.resolve(view)
        for slot in slots:
            candidateSurface = sectionWorldPoint(session, candidate, ring, slot)
            candidatePixel = eventPixel(pixel(candidateSurface)) \
                if candidateSurface is not None else None
            preflight = (session.pickItem(
                camera, candidatePixel[0], candidatePixel[1],
                state.brushRadiusPx, pomadeLib.POMADE_PICK_TUBE_VERT)
                if camera is not None and candidatePixel is not None else None)
            if preflight is None or int(preflight["id"]) != candidate:
                rootPickTrace.append((int(candidate), ring, slot,
                                      candidatePixel, preflight, ()))
                continue
            mouse.click(candidatePixel)
            selected = session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT)
            rootPickTrace.append((int(candidate), ring, slot, candidatePixel,
                                  preflight, tuple(selected)))
            if selected == [(candidate, -1, -1)]:
                transportedRoot = int(candidate)
                break
        if transportedRoot >= 0:
            break
    check(transportedRoot >= 0,
          "a real Hierarchy surface click resolves one visible shared-region root %r %r"
          % (rootPickTrace, [(root,
                              int(session.dll.Pomade_RegionForTube(session.model, root)),
                              sectionWorldPoint(session, root, 1, 0))
                             for root in roots]))
    if transportedRoot < 0:
        shutdown()
        return 1

    # Sculpt that actually selected root, then subdivide it through the
    # dock, and finally leave a real independent child-CV tweak. No direct
    # Graph or hierarchy mutator appears in this workflow.
    rootCenters = pomadeBridge.tubeCenters(session.dll, session.model,
                                          transportedRoot)
    sculptRing = min(1, pomadeBridge.tubeSectionCount(
        session.dll, session.model, transportedRoot) - 1)
    _sculptT, sculptUV, _sculptScale, _sculptTwist = pomadeBridge.tubeSection(
        session.dll, session.model, transportedRoot, sculptRing)
    # Auto now retains a triangular region's three authored columns. Slot 0
    # is its shared boundary corner, where the neighbour legitimately wins a
    # depth pick; use the exposed third corner for this root-only sculpt.
    sculptSlot = 2 if len(sculptUV) > 2 else max(0, len(sculptUV) - 1)
    sculptTarget = sectionWorldPoint(session, transportedRoot, sculptRing,
                                     sculptSlot)
    lookAt = (rootCenters[min(1, len(rootCenters) - 1)] if rootCenters else
              sculptTarget)
    # The compact three-region rig can project another root over an exposed
    # corner from one side. Pick an ordinary side view where K11 sees the
    # selected root, then send the actual Qt brush events to that pixel.
    sideReady = False
    sculptPixel = None
    sculptItem = None
    sideTrace = []
    state.brushRadiusPx = 32.0
    if lookAt is not None and sculptTarget is not None:
        # -X/-Z puts this region's third corner in front of the compact
        # neighbour arrangement. The read-only K11 result below keeps that
        # camera fact explicit rather than assuming a world point is visible.
        dx, dz = -6.0, -3.0
        if aimCamera(stage, view,
                     (lookAt[0] + dx, lookAt[1] + 2.5, lookAt[2] + dz),
                     lookAt):
            wait(30)
            camera = pomadeCamera.resolve(view)
            candidatePixel = (eventPixel(pixel(sculptTarget))
                              if camera is not None else None)
            candidateItem = (session.pickItem(
                camera, candidatePixel[0], candidatePixel[1],
                state.brushRadiusPx, pomadeLib.POMADE_PICK_TUBE_VERT)
                if candidatePixel is not None else None)
            sideTrace.append(((dx, dz), candidatePixel, candidateItem))
            if candidateItem is not None and int(candidateItem["id"]) == transportedRoot:
                sideReady = True
                sculptPixel = candidatePixel
                sculptItem = candidateItem
    camera = pomadeCamera.resolve(view)
    check(sideReady and camera is not None and sculptTarget is not None,
          "a side camera exposes the picked root for its real Sculpt workflow %r"
          % sideTrace)
    if sideReady and camera is not None and sculptTarget is not None:
        workspace._modeButtons["sculpt"].click()
        wait(20)
        sculptBefore = tubeWorldSnapshot(session, (transportedRoot,))
        sculptActive = (state.activeMode == "sculpt" and
                        getattr(viewport.loop, "modeId", "") == "sculpt")
        gestureOpened = False
        if sculptPixel is not None:
            mouse.press(sculptPixel)
            gestureOpened = bool(viewport.gestureActive)
            for step in range(1, 4):
                mouse.move((sculptPixel[0] + 12.0 * step,
                            sculptPixel[1] + 3.0 * step))
            mouse.release((sculptPixel[0] + 36.0, sculptPixel[1] + 9.0))
        sculptAfter = tubeWorldSnapshot(session, (transportedRoot,))
        check(sculptActive and sculptPixel is not None and
              sculptItem is not None and
              int(sculptItem["id"]) == transportedRoot and gestureOpened and
              not sameWorld(sculptAfter, sculptBefore),
              "a real Sculpt drag changes the attached root before subdivision "
              "(%r; target=%r; picked=%r)"
              % (messages[-2:], (sculptRing, sculptSlot, sculptTarget),
                 sculptItem))

    workspace._modeButtons["hierarchy"].click()
    wait(20)
    # The prior top-view Hierarchy click selected this exact root. Preserve
    # it through the Sculpt-to-Hierarchy mode switch; a second side-view
    # click would be an ambiguous front-tube selection in this compact rig.
    selectedRoot = session.readSelection(pomadeLib.POMADE_PICK_TUBE_VERT)
    subdivided = clickDockControl("subdivide") if selectedRoot == [
        (transportedRoot, -1, -1)] else False
    check(selectedRoot == [(transportedRoot, -1, -1)] and subdivided,
          "the real hierarchy surface click and Subdivide control create child tubes "
          "(%r)" % messages[-2:])
    children = descendants(session, transportedRoot)
    check(len(children) >= 2,
          "the sculpted attached root now has editable child descendants")
    if not children:
        shutdown()
        return 1

    child = int(children[0])
    workspace._modeButtons["tube"].click()
    wait(15)
    centerControl = getattr(workspace, "_tubeSelectionButtons", {}).get("center")
    if centerControl is not None:
        centerControl.click()
    childCenters = pomadeBridge.tubeCenters(session.dll, session.model, child)
    childCv = min(1, len(childCenters) - 1)
    # The public authored cage remains the geometry oracle below.  Mouse
    # coordinates, however, must use the centered core handle artists see.
    childHandle = (pomadeBridge.tubeCenterHandle(session.dll, session.model,
                                                child, childCv)
                   if childCenters else None)
    childPixel = pixel(childHandle) if childHandle is not None else None
    childSubtree = (transportedRoot,) + descendants(session, transportedRoot)
    childBefore = tubeWorldSnapshot(session, childSubtree)
    if childPixel is not None and childCv > 0:
        mouse.click(childPixel)
        mouse.press(childPixel)
        for step in range(1, 4):
            mouse.move((childPixel[0] + 10.0 * step, childPixel[1]))
        mouse.release((childPixel[0] + 30.0, childPixel[1]))
    childAfter = tubeWorldSnapshot(session, childSubtree)
    check(centerControl is not None and childPixel is not None and childCv > 0 and
          not sameWorld(worldSubset(childAfter, (child,)),
                        worldSubset(childBefore, (child,))) and
          all(sameWorld(worldSubset(childAfter, (sibling,)),
                        worldSubset(childBefore, (sibling,)))
              for sibling in descendants(session, transportedRoot)
              if sibling != child),
          "a real child-CV transform leaves one independent descendant shape to transport")

    # Return to the graph camera before testing the actual Reposition drag.
    check(frameScalp(stage, view), "the graph camera returns after sculpt/subdivide editing")
    wait(20)
    camera = pomadeCamera.resolve(view)
    if camera is None:
        shutdown()
        return 1

    workspace._modeButtons["graph"].click()
    wait(15)
    reposition = workspace._subModeButtons.get("reposition")
    viewport.setPointerInside(True)
    view.setFocus()
    typeKey(view, "r")
    typeKey(view, "m")
    check(reposition is not None and reposition.isChecked() and
          state.graphSubMode == "reposition",
          "the Reposition shelf button is visible, checked and M-routable")

    transportedSubtree = (transportedRoot,) + descendants(session, transportedRoot)
    otherAttachedSubtrees = tuple((root,) + descendants(session, root)
                                  for root in attachedRoots
                                  if root != transportedRoot)
    unrelatedSubtrees = tuple((root,) + descendants(session, root)
                              for root in unrelatedRoots)
    allTubeIds = tuple(sorted({tube for subtree in
                               (transportedSubtree,) + otherAttachedSubtrees +
                               unrelatedSubtrees for tube in subtree}))
    initialGraph = nodeSnapshot(session, allNodes)
    initialTubes = tubeSnapshot(session, allTubeIds)
    initialWorld = tubeWorldSnapshot(session, allTubeIds)
    initialBases = dict((root, rootBaseFootprint(session, root))
                         for root in attachedRoots)
    initialUpper = rootUpperWorldSnapshot(session, attachedRoots)
    initialChildResidual = tubeDeltas(session, child)

    def _nonzeroTranslation(delta):
        return (delta is not None and
                math.sqrt(sum(component * component for component in delta)) > 1e-5)

    def shapedAttachmentState(beforeWorld, beforeBases, beforeUpper,
                               beforeResidual, beforeLayout, afterWorld):
        """Base conforms while upper root controls and child sculpt remain valid."""
        baseResults = dict((root, rootBaseConforms(session, base))
                           for root, base in beforeBases.items()
                           if base is not None)
        basesReady = (len(baseResults) == len(attachedRoots) and
                      all(base is not None and base["mapError"] <= 4e-4
                          for base in beforeBases.values()))
        basesConform = (basesReady and
                        all(result[0] for result in baseResults.values()))
        upperAfter = rootUpperWorldSnapshot(session, attachedRoots)
        upperDeltas = tuple(translatedWorld(beforeUpper, upperAfter, (root,))
                            for root in attachedRoots)
        upperRigid = all(_nonzeroTranslation(delta) for delta in upperDeltas)
        residual = centerResidual(tubeDeltas(session, child), childCv)
        residualBefore = centerResidual(beforeResidual, childCv)
        # K6 re-expresses this residual in the fresh child frame after
        # replacing CV0 and section 0 with the conformed attachment. Raw
        # delta components therefore may rebase; the public GUI witness is
        # that the artist's selected upper CV remains materially sculpted.
        childUpperSculptPresent = (residualBefore is not None and
                                   centerResidualNorm(residualBefore) > 1e-5 and
                                   residual is not None and
                                   centerResidualNorm(residual) > 1e-5)
        afterLayout = tubeLayout(tubeSnapshot(session, allTubeIds))
        childLayoutFinite = (afterLayout == beforeLayout and finiteWorld(afterWorld))
        unrelatedExact = all(sameWorld(worldSubset(afterWorld, subtree),
                                       worldSubset(beforeWorld, subtree))
                             for subtree in unrelatedSubtrees)
        return {"basesReady": basesReady, "basesConform": basesConform,
                "baseResults": baseResults, "upperRigid": upperRigid,
                "upperDeltas": upperDeltas,
                "childUpperSculptPresent": childUpperSculptPresent,
                "residual": residual, "childLayoutFinite": childLayoutFinite,
                "unrelatedExact": unrelatedExact}

    def pureTranslationState(before, after):
        """A topology-preserving whole-region translation remains rigid."""
        deltas = [translatedWorld(before, after, transportedSubtree)]
        deltas.extend(translatedWorld(before, after, subtree)
                      for subtree in otherAttachedSubtrees)
        return (all(_nonzeroTranslation(delta) for delta in deltas) and
                all(sameWorld(worldSubset(after, subtree),
                              worldSubset(before, subtree))
                    for subtree in unrelatedSubtrees))

    check(all(base is not None and base["mapError"] <= 4e-4
              for base in initialBases.values()),
          "the initial attached root bases map each retained slot to their live region boundaries")

    aPixel = nodePixel(sharedA)
    check(aPixel is not None, "the shared CV has a displayed Reposition target")
    if aPixel is not None:
        # Reposition affordances deliberately stay reachable while graph
        # topology snapping is set to its smallest 1 px preference.  Move
        # without a button down from an incident edge into the CV's 8 px
        # displayed target: the 2 px boundary crossing must be delivered,
        # and a node must beat both the edge and its containing regions.
        # The row is screen pixels; the model's snap radius is rest units
        # and is never overwritten with the pixel number.
        nativeBefore = float(session.dll.Pomade_GetSnapRadius(session.model))
        snapDescriptor = next((descriptor for descriptor in
                               pomadePanels.descriptors("graph", state)
                               if descriptor.id == "snapRadiusPx"), None)
        if snapDescriptor is not None:
            snapDescriptor.set(state, session, 1.0)
        nativeSnap = float(session.dll.Pomade_GetSnapRadius(session.model))
        check(snapDescriptor is not None and
              abs(nativeSnap - nativeBefore) < 1e-7 and
              abs(float(state.snapRadiusPx) - 1.0) < 1e-5,
              "the real Graph Snap radius control applies its 1 px minimum "
              "to the pixel preference, not the model's world radius")
        bPixel = nodePixel(sharedB)
        edgeLength = (math.hypot(bPixel[0] - aPixel[0],
                                 bPixel[1] - aPixel[1])
                      if bPixel is not None else 0.0)
        if bPixel is not None and edgeLength > 12.0:
            direction = ((bPixel[0] - aPixel[0]) / edgeLength,
                         (bPixel[1] - aPixel[1]) / edgeLength)
            edgePrehighlight = (aPixel[0] + 9.0 * direction[0],
                                aPixel[1] + 9.0 * direction[1])
            cvBoundary = (aPixel[0] + 7.0 * direction[0],
                          aPixel[1] + 7.0 * direction[1])
            cvApproach = (aPixel[0] + 6.0 * direction[0],
                          aPixel[1] + 6.0 * direction[1])
            blankHover = pixel((0.88, 0.0, 0.88))
            if blankHover is not None:
                mouse.move(blankHover)
                wait(20)
                check(graphHover(session) == (0, -1, -1, -1),
                      "a press-less blank Reposition hover clears the published target")
            mouse.move(edgePrehighlight)
            wait(25)
            edgeHover = graphHover(session)
            check(state.snapRadiusPx == 1.0 and edgeHover is not None and
                  edgeHover[0] == pomadeLib.POMADE_PICK_GRAPH_EDGE and
                  edgeHover[1] >= 0,
                  "at snap 1 px, an incident-edge hover outside the CV prehighlights GraphEdge")
            # This only moves two physical pixels. It catches hover event
            # coalescing that would leave the edge highlighted at the CV.
            mouse.move(cvBoundary)
            wait(25)
            check(graphHover(session) == (pomadeLib.POMADE_PICK_GRAPH_NODE,
                                          sharedA, -1, -1),
                  "a sub-3 px no-button boundary move promotes the exact CV over its edge")
            mouse.move(cvApproach)
            wait(25)
            check(not viewport.gestureActive and
                  graphHover(session) == (pomadeLib.POMADE_PICK_GRAPH_NODE,
                                          sharedA, -1, -1),
                  "a no-button hover 6 px along the incident edge prehighlights only the exact CV")
            left = pointerEvent(mouse._QtCore.QEvent.Type.Leave)
            wait(20)
            check(left and not viewport.gestureActive and
                  graphHover(session) == (0, -1, -1, -1),
                  "an idle StageView leave clears the CV prehighlight without changing drag state")
            entered = pointerEvent(mouse._QtCore.QEvent.Type.Enter)
            mouse.move(cvApproach)
            wait(25)
            check(entered and graphHover(session) ==
                  (pomadeLib.POMADE_PICK_GRAPH_NODE, sharedA, -1, -1),
                  "StageView re-entry re-acquires the exact CV at the same pixel")
            capturePrehighlight()
        else:
            check(False, "the shared edge has enough displayed length for CV/edge hover boundaries")
        nodeHit = pomadeLoops.Sample(session, camera, aPixel[0],
                                    aPixel[1]).item(
            pomadeLib.POMADE_PICK_GRAPH_NODE, state.snapRadiusPx)
        check(nodeHit is not None and nodeHit["id"] == sharedA,
              "the shared CV wins over its incident edge at the Reposition press")
        # Press six pixels into the prehighlighted CV, directly over its
        # incident edge. This is the hard-CV priority regression: it cannot
        # fall through to the edge just because graph snapping is 1 px.
        press = cvApproach if bPixel is not None and edgeLength > 12.0 else aPixel
        mouse.press(press)
        check(tuple(getattr(viewport.loop, "_repositionIds", ())) == (sharedA,),
              "the off-centre CV press arms only its stable node, never the edge")
        mouse.move(press)
        check(same(nodeSnapshot(session, allNodes), initialGraph) and
              sameWorld(tubeWorldSnapshot(session, allTubeIds), initialWorld),
              "an off-centre shared-CV press with zero travel does not jump graph or tubes")
        redraws = []
        redraw = getattr(workspace._api, "UpdateViewport", None)
        redrawInstalled = False
        if callable(redraw):
            def countedRedraw(*args, **kwargs):
                redraws.append(1)
                return redraw(*args, **kwargs)
            try:
                workspace._api.UpdateViewport = countedRedraw
                redrawInstalled = True
            except (AttributeError, TypeError):
                pass
        smallMove = (press[0] + 3.0, press[1])
        mouse.move(smallMove)
        check(not same(nodeSnapshot(session, allNodes)[0], initialGraph[0]),
              "a 3 px held shared-CV move updates the live region boundary")
        for distance in (10.0, 18.0, 24.0):
            mouse.move((press[0] + distance, press[1]))
        if redrawInstalled:
            workspace._api.UpdateViewport = redraw
        cvLive = nodeSnapshot(session, allNodes)
        cvLiveWorld = tubeWorldSnapshot(session, allTubeIds)
        cvShape = shapedAttachmentState(initialWorld, initialBases, initialUpper,
                                        initialChildResidual,
                                        tubeLayout(initialTubes), cvLiveWorld)
        initialNodes, liveNodes = dict(initialGraph[0]), dict(cvLive[0])
        check(not same(liveNodes[sharedA], initialNodes[sharedA]) and
              all(same(liveNodes[node], initialNodes[node])
                  for node in allNodes if node != sharedA) and
              cvLive[1:] == initialGraph[1:] and
              sum(1 for loop in cvLive[2] if sharedA in loop) == 2 and redraws and
              cvShape["basesConform"] and cvShape["upperRigid"] and
              cvShape["childUpperSculptPresent"] and cvShape["childLayoutFinite"] and
              cvShape["unrelatedExact"],
              "a held shared-CV reshape conforms both root bases, keeps the selected upper sculpt present and leaves the detached hierarchy exact (%r)"
              % cvShape)
        mouse.release((press[0] + 24.0, press[1]))
        cvFinal = nodeSnapshot(session, allNodes)
        cvFinalTubes = tubeSnapshot(session, allTubeIds)
        cvFinalWorld = tubeWorldSnapshot(session, allTubeIds)
        cvFinalShape = shapedAttachmentState(initialWorld, initialBases,
                                             initialUpper, initialChildResidual,
                                             tubeLayout(initialTubes), cvFinalWorld)
        check(same(cvFinal, cvLive) and sameWorld(cvFinalWorld, cvLiveWorld) and
              cvFinalShape["basesConform"] and cvFinalShape["upperRigid"] and
              cvFinalShape["childUpperSculptPresent"] and
              cvFinalShape["childLayoutFinite"],
              "releasing the shared-CV reshape retains the live conformed bases and upper sculpt")
        typeKey(view, "z", ("ctrl",))
        check(same(nodeSnapshot(session, allNodes), initialGraph) and
              tubeSnapshot(session, allTubeIds) == initialTubes,
              "one undo restores the shared-CV graph and every transported tube exactly")
        typeKey(view, "y", ("ctrl",))
        check(same(nodeSnapshot(session, allNodes), cvFinal) and
              tubeSnapshot(session, allTubeIds) == cvFinalTubes,
              "one redo restores the exact shared-CV graph and subtree transport")

        # Escape owns the still-live gesture: temporarily move the same CV,
        # then restore the prior committed graph with no new topology.
        aPixel = nodePixel(sharedA)
        mouse.press((aPixel[0] + 1.0, aPixel[1]))
        mouse.move((aPixel[0] + 18.0, aPixel[1] - 12.0))
        check(not same(nodeSnapshot(session, allNodes)[0], cvFinal[0]),
              "the second shared-CV gesture changes the live graph before Escape")
        typeKey(view, "escape")
        mouse.release((aPixel[0] + 18.0, aPixel[1] - 12.0))
        check(same(nodeSnapshot(session, allNodes), cvFinal) and
              tubeSnapshot(session, allTubeIds) == cvFinalTubes,
              "Escape restores the exact pre-gesture graph and every attached descendant")

    # Blank Reposition must neither create/weld nor contribute an undo item.
    blank = pixel((0.88, 0.0, 0.88))
    blankBefore = nodeSnapshot(session, allNodes)
    if blank is not None:
        mouse.press(blank)
        mouse.move((blank[0] - 12.0, blank[1] - 8.0))
        mouse.release((blank[0] - 12.0, blank[1] - 8.0))
    check(same(nodeSnapshot(session, allNodes), blankBefore),
          "a blank Reposition drag creates, welds and moves nothing")
    typeKey(view, "z", ("ctrl",))
    check(same(nodeSnapshot(session, allNodes), initialGraph),
          "blank Reposition did not insert an undo item before the CV drag")
    typeKey(view, "y", ("ctrl",))
    check(same(nodeSnapshot(session, allNodes), blankBefore),
          "redo restores the one preceding shared-CV drag after a blank miss")

    # The shared-edge midpoint must hit GraphEdge (not either endpoint).
    aPixel, bPixel = nodePixel(sharedA), nodePixel(sharedB)
    midpoint = ((aPixel[0] + bPixel[0]) * 0.5, (aPixel[1] + bPixel[1]) * 0.5)
    endpointDistance = min(math.hypot(midpoint[0] - aPixel[0], midpoint[1] - aPixel[1]),
                           math.hypot(midpoint[0] - bPixel[0], midpoint[1] - bPixel[1]))
    edgeHit = pomadeLoops.Sample(session, camera, midpoint[0], midpoint[1]).item(
        pomadeLib.POMADE_PICK_GRAPH_EDGE, state.snapRadiusPx)
    check(endpointDistance > state.snapRadiusPx and edgeHit is not None and
          edgeHit["kind"] == pomadeLib.POMADE_PICK_GRAPH_EDGE,
          "the midpoint is an exposed GraphEdge target farther than either CV")
    edgeBefore = nodeSnapshot(session, allNodes)
    edgeBeforeTubes = tubeSnapshot(session, allTubeIds)
    edgeBeforeWorld = tubeWorldSnapshot(session, allTubeIds)
    edgeBeforeBases = dict((root, rootBaseFootprint(session, root))
                           for root in attachedRoots)
    edgeBeforeUpper = rootUpperWorldSnapshot(session, attachedRoots)
    edgeBeforeResidual = tubeDeltas(session, child)
    edgeBeforeLayout = tubeLayout(edgeBeforeTubes)
    if edgeHit is not None and endpointDistance > state.snapRadiusPx:
        # This is a real, no-button edge hover. It must publish the same
        # affordance the following press will grab, then a blank move must
        # remove it rather than leaving a stale edge highlight behind.
        mouse.move(midpoint)
        wait(25)
        midpointHover = graphHover(session)
        check(midpointHover is not None and
              midpointHover[0] == pomadeLib.POMADE_PICK_GRAPH_EDGE and
              midpointHover[1] == edgeHit["id"],
              "the exposed edge midpoint prehighlights its exact GraphEdge")
        blankHover = pixel((0.88, 0.0, 0.88))
        if blankHover is not None:
            mouse.move(blankHover)
            wait(25)
            check(graphHover(session) == (0, -1, -1, -1),
                  "a blank no-button hover clears the edge prehighlight")
        mouse.press(midpoint)
        for step in range(1, 4):
            mouse.move((midpoint[0], midpoint[1] + 26.0 * step / 3.0))
        edgeLive = nodeSnapshot(session, allNodes)
        edgeLiveWorld = tubeWorldSnapshot(session, allTubeIds)
        edgeShape = shapedAttachmentState(edgeBeforeWorld, edgeBeforeBases,
                                          edgeBeforeUpper, edgeBeforeResidual,
                                          edgeBeforeLayout, edgeLiveWorld)
        nodeMapBefore = dict(edgeBefore[0])
        nodeMapLive = dict(edgeLive[0])
        check(not same(nodeMapLive[sharedA], nodeMapBefore[sharedA]) and
              not same(nodeMapLive[sharedB], nodeMapBefore[sharedB]) and
              all(same(nodeMapLive[node], nodeMapBefore[node])
                  for node in allNodes if node not in (sharedA, sharedB)) and
              edgeLive[1:] == edgeBefore[1:] and
              all(graphNode(session, node)[0] == 0 and
                  all(0.0 <= uv <= 1.0 for uv in graphNode(session, node)[1])
                  for node in (sharedA, sharedB)) and
              edgeShape["basesConform"] and edgeShape["upperRigid"] and
              edgeShape["childUpperSculptPresent"] and
              edgeShape["childLayoutFinite"] and edgeShape["unrelatedExact"],
              "a held shared-edge reshape conforms root polygons and keeps selected upper sculpt and detached roots (%r)"
              % edgeShape)
        captureHeldEdge()
        mouse.release((midpoint[0], midpoint[1] + 26.0))
        edgeFinal = nodeSnapshot(session, allNodes)
        edgeFinalTubes = tubeSnapshot(session, allTubeIds)
        edgeFinalWorld = tubeWorldSnapshot(session, allTubeIds)
        edgeFinalShape = shapedAttachmentState(edgeBeforeWorld, edgeBeforeBases,
                                               edgeBeforeUpper, edgeBeforeResidual,
                                               edgeBeforeLayout, edgeFinalWorld)
        check(same(edgeFinal, edgeLive) and sameWorld(edgeFinalWorld, edgeLiveWorld) and
              edgeFinalShape["basesConform"] and edgeFinalShape["upperRigid"] and
              edgeFinalShape["childUpperSculptPresent"] and
              edgeFinalShape["childLayoutFinite"],
              "release retains the live conformed shared-edge bases and upper sculpt")
        typeKey(view, "z", ("ctrl",))
        check(same(nodeSnapshot(session, allNodes), edgeBefore) and
              tubeSnapshot(session, allTubeIds) == edgeBeforeTubes,
              "one undo restores both shared-edge endpoints and all subtree geometry")
        typeKey(view, "y", ("ctrl",))
        check(same(nodeSnapshot(session, allNodes), edgeFinal) and
              tubeSnapshot(session, allTubeIds) == edgeFinalTubes,
              "one redo restores the exact shared-edge transport")

        # Reposition must refuse an invalid collapsed region as a whole,
        # rather than welding/splitting a shared node or leaving one of its
        # neighbours half moved.  Return to a valid location in the same
        # held gesture, then Escape restores that frozen baseline exactly.
        collapseNode = next((node for node in firstLoop
                             if node not in (sharedA, sharedB)), -1)
        aPixel, collapsePixel = nodePixel(sharedA), nodePixel(collapseNode)
        collapseBefore = nodeSnapshot(session, allNodes)
        if aPixel is not None and collapsePixel is not None:
            mouse.press(aPixel)
            mouse.move(collapsePixel)
            check(same(nodeSnapshot(session, allNodes), collapseBefore) and
                  tubeSnapshot(session, allTubeIds) == edgeFinalTubes,
                  "an invalid collapsed shared region is rejected atomically")
            valid = (aPixel[0] + 18.0, aPixel[1] - 8.0)
            mouse.move(valid)
            check(not same(nodeSnapshot(session, allNodes)[0], collapseBefore[0]),
                  "the same Reposition gesture recovers at a valid surface point")
            typeKey(view, "escape")
            mouse.release(valid)
            check(same(nodeSnapshot(session, allNodes), collapseBefore) and
                  tubeSnapshot(session, allTubeIds) == edgeFinalTubes,
                  "Escape restores the recovered shared-node gesture exactly")

    # Place remains the artist's quick click-or-drag tool.  Dragging an
    # existing shared CV must use the same attachment transport as
    # Reposition: it may not leave the selected region's established tube
    # hierarchy behind while only the graph point moves.
    place = workspace._subModeButtons.get("place")
    if place is not None:
        place.click()
        wait(20)
    check(place is not None and place.isChecked() and
          state.graphSubMode == "place",
          "the Place shelf selects its existing-CV drag workflow")
    placeBefore = nodeSnapshot(session, allNodes)
    placeBeforeTubes = tubeSnapshot(session, allTubeIds)
    placeBeforeWorld = tubeWorldSnapshot(session, allTubeIds)
    placeBeforeBases = dict((root, rootBaseFootprint(session, root))
                            for root in attachedRoots)
    placeBeforeUpper = rootUpperWorldSnapshot(session, attachedRoots)
    placeBeforeResidual = tubeDeltas(session, child)
    placeBeforeLayout = tubeLayout(placeBeforeTubes)
    aPixel = nodePixel(sharedA)
    placeTarget = ((aPixel[0] + 28.0, aPixel[1] - 16.0)
                   if aPixel is not None else None)
    placeHover = None
    placeArmed = False
    placeHeld = None
    placeHeldWorld = None
    if aPixel is not None and placeTarget is not None:
        # Preflight the visible target, then capture the actual loop state
        # while the left button is held. This makes a missed node hit or an
        # unarmed gesture distinct from a hierarchy transport failure.
        mouse.move(aPixel)
        wait(10)
        placeHover = graphHover(session)
        mouse.press(aPixel)
        placeArmed = (bool(viewport.gestureActive) and
                      getattr(viewport.loop, "_dragNode", -1) == sharedA and
                      bool(getattr(viewport.loop, "_bracketOpen", False)))
        mouse.move((aPixel[0] + 12.0, aPixel[1] - 7.0))
        mouse.move(placeTarget)
        placeHeld = nodeSnapshot(session, allNodes)
        placeHeldWorld = tubeWorldSnapshot(session, allTubeIds)
        mouse.release(placeTarget)
    placeFinal = nodeSnapshot(session, allNodes)
    placeFinalTubes = tubeSnapshot(session, allTubeIds)
    placeFinalWorld = tubeWorldSnapshot(session, allTubeIds)
    placeNodesBefore, placeNodesHeld = (dict(placeBefore[0]),
                                        dict(placeHeld[0]) if placeHeld else {})
    placeNodesAfter = dict(placeFinal[0])
    placeNodeMoved = (aPixel is not None and
                      not same(placeNodesAfter.get(sharedA),
                               placeNodesBefore.get(sharedA)))
    placeOnlyNodeMoved = (placeNodeMoved and
                          all(same(placeNodesAfter.get(node),
                                   placeNodesBefore.get(node))
                              for node in allNodes if node != sharedA))
    placeTopologyStable = placeFinal[1:] == placeBefore[1:]
    placeHeldNodeMoved = (placeHeld is not None and
                          not same(placeNodesHeld.get(sharedA),
                                   placeNodesBefore.get(sharedA)))
    placeHeldShape = (shapedAttachmentState(
        placeBeforeWorld, placeBeforeBases, placeBeforeUpper,
        placeBeforeResidual, placeBeforeLayout, placeHeldWorld)
        if placeHeldWorld is not None else None)
    placeFinalShape = shapedAttachmentState(
        placeBeforeWorld, placeBeforeBases, placeBeforeUpper,
        placeBeforeResidual, placeBeforeLayout, placeFinalWorld)
    placeReleased = not viewport.gestureActive
    if not (placeArmed and placeHeldNodeMoved and placeOnlyNodeMoved and
            placeTopologyStable and placeHeldShape is not None and
            placeHeldShape["basesConform"] and placeHeldShape["upperRigid"] and
            placeHeldShape["childUpperSculptPresent"] and
            placeHeldShape["childLayoutFinite"] and
            placeHeldShape["unrelatedExact"] and
            placeFinalShape["basesConform"] and placeFinalShape["upperRigid"] and
            placeFinalShape["childUpperSculptPresent"] and
            placeFinalShape["childLayoutFinite"] and
            placeFinalShape["unrelatedExact"] and placeReleased):
        print("info: Place trace hover=%r armed=%r heldNodeMoved=%r "
              "nodeMoved=%r onlyNode=%r topology=%r heldShape=%r "
              "finalShape=%r released=%r lastError=%r messages=%r "
              "before=%r held=%r after=%r"
              % (placeHover, placeArmed, placeHeldNodeMoved, placeNodeMoved,
                 placeOnlyNodeMoved, placeTopologyStable, placeHeldShape,
                 placeFinalShape, placeReleased, session.lastError(),
                 messages[-4:], placeBefore, placeHeld, placeFinal))
    check(placeArmed,
          "the exact existing-CV Place press arms its graph gesture")
    check(placeHeldNodeMoved and placeNodeMoved and placeOnlyNodeMoved,
          "the held and released Place drag changes only its selected shared CV")
    check(placeTopologyStable,
          "releasing Place away from graph targets preserves graph topology")
    check(placeHeldShape is not None and placeHeldShape["basesConform"] and
          placeHeldShape["upperRigid"] and
          placeHeldShape["childUpperSculptPresent"] and
          placeHeldShape["childLayoutFinite"] and
          placeHeldShape["unrelatedExact"],
          "the held Place reshape conforms every affected root base while retaining material upper sculpt")
    check(placeFinalShape["basesConform"] and placeFinalShape["upperRigid"] and
          placeFinalShape["childUpperSculptPresent"] and
          placeFinalShape["childLayoutFinite"] and
          placeFinalShape["unrelatedExact"],
          "releasing Place retains the conformed bases, child layout and detached hierarchy")
    check(placeReleased,
          "releasing Place closes its graph gesture")
    typeKey(view, "z", ("ctrl",))
    check(same(nodeSnapshot(session, allNodes), placeBefore) and
          tubeSnapshot(session, allTubeIds) == placeBeforeTubes,
          "one undo restores the graph and all transported whole tubes before Place")
    typeKey(view, "y", ("ctrl",))
    check(same(nodeSnapshot(session, allNodes), placeFinal) and
          tubeSnapshot(session, allTubeIds) == placeFinalTubes,
          "one redo reapplies exactly the existing-CV Place transport")

    shutdown()
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeReposition needs testusdview")
    sys.exit(0)
