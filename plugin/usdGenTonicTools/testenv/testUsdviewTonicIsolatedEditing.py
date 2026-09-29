# testUsdviewTonicIsolatedEditing -- child-component isolation through USDView.
#
# This is deliberately an artist workflow: make a region with the Graph
# stroke, subdivide it through the hierarchy dock, click one displayed child
# center CV, and move its real translate manipulator.  Reads use the public
# per-tube ABI so the assertions cover authored and world-space geometry,
# rather than any viewport cache.
import ctypes
import math
import os
import sys


failures = 0
RECT = ((1.0, 1.0), (3.0, 1.0), (3.0, 3.0), (1.0, 3.0))


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


def sectionCV(session, tubeId, ring, slot):
    """World point of an authored section CV through Tonic's public ABI."""
    from usdGenTonicTools import tonicBridge
    section = tonicBridge.tubeSection(session.dll, session.model, tubeId,
                                      ring)
    if slot < 0 or slot >= len(section[1]):
        return None
    entry = session.dll.Tonic_GetTubeSectionFrame
    cfloat3 = ctypes.POINTER(ctypes.c_float)
    entry.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, cfloat3,
                      cfloat3, cfloat3, cfloat3]
    entry.restype = ctypes.c_int
    origin = (ctypes.c_float * 3)()
    frame = (ctypes.c_float * 9)()
    scale = ctypes.c_float(1.0)
    twist = ctypes.c_float(0.0)
    if entry(session.model, int(tubeId), int(ring), origin, frame,
             ctypes.byref(scale), ctypes.byref(twist)) != 0:
        return None
    ct, st = math.cos(twist.value), math.sin(twist.value)
    placed = []
    for u, v in section[1]:
        u, v = u * scale.value, v * scale.value
        placed.append((u * ct - v * st, u * st + v * ct))
    # Tonic_GetTubeSectionFrame returns the ring centroid as origin.  The
    # authored UVs are not centroid-recentered, so reconstruct around that
    # ABI origin by subtracting their transformed mean.
    meanU = sum(pair[0] for pair in placed) / len(placed)
    meanV = sum(pair[1] for pair in placed) / len(placed)
    u, v = placed[slot]
    return tuple(origin[axis] + frame[axis] * (u - meanU) +
                 frame[axis + 3] * (v - meanV) for axis in range(3))


def snapshotTube(session, tubeId):
    """Center, authored section, and derived section-world geometry."""
    from usdGenTonicTools import tonicBridge
    centers = tuple(tuple(float(v) for v in point)
                    for point in tonicBridge.tubeCenters(
                        session.dll, session.model, tubeId))
    sections = []
    world = []
    for ring in range(tonicBridge.tubeSectionCount(session.dll,
                                                    session.model, tubeId)):
        t, uv, scale, twist = tonicBridge.tubeSection(
            session.dll, session.model, tubeId, ring)
        sections.append((float(t), tuple(tuple(float(v) for v in pair)
                                         for pair in uv), float(scale),
                         float(twist)))
        world.extend(sectionCV(session, tubeId, ring, slot)
                     for slot in range(len(uv)))
    return (centers, tuple(sections), tuple(world))


def sameGeometry(left, right, tolerance=1e-5):
    """Comparison tolerates float ABI roundoff but no geometric change."""
    def close(a, b):
        if isinstance(a, (tuple, list)):
            return len(a) == len(b) and all(close(x, y) for x, y in zip(a, b))
        return abs(float(a) - float(b)) <= tolerance
    return close(left, right)


def shapeInvariant(before, after, tolerance=2e-4):
    """Whether a tube kept its authored shape, allowing rigid motion."""
    centersBefore, sectionsBefore, _worldBefore = before
    centersAfter, sectionsAfter, _worldAfter = after
    if len(centersBefore) != len(centersAfter) or \
            not sameGeometry(sectionsBefore, sectionsAfter, tolerance):
        return False
    for i, point in enumerate(centersBefore):
        for j in range(i):
            a = tuple(point[k] - centersBefore[j][k] for k in range(3))
            b = tuple(centersAfter[i][k] - centersAfter[j][k]
                      for k in range(3))
            if not sameGeometry(a, b, tolerance):
                return False
    return True


def translatedRigid(before, after, tolerance=2e-4):
    """Every authored and world point must share one pure-translation delta."""
    centersBefore, sectionsBefore, worldBefore = before
    centersAfter, sectionsAfter, worldAfter = after
    if not centersBefore or len(centersBefore) != len(centersAfter) or \
            len(worldBefore) != len(worldAfter) or \
            not sameGeometry(sectionsBefore, sectionsAfter, tolerance):
        return False
    delta = tuple(centersAfter[0][axis] - centersBefore[0][axis]
                  for axis in range(3))
    for prior, current in zip(centersBefore, centersAfter):
        if not sameGeometry(tuple(prior[axis] + delta[axis]
                                  for axis in range(3)), current, tolerance):
            return False
    for prior, current in zip(worldBefore, worldAfter):
        if not sameGeometry(tuple(prior[axis] + delta[axis]
                                  for axis in range(3)), current, tolerance):
            return False
    return True


def parentBoundarySpan(snapshots):
    """Axis extents of child centers, used to validate the parent boundary."""
    points = [point for snapshot in snapshots.values() for point in snapshot[0]]
    if not points:
        return (0.0, 0.0, 0.0)
    return tuple(max(point[axis] for point in points) -
                 min(point[axis] for point in points) for axis in range(3))


def guideCount(session):
    guides = ctypes.c_int(0)
    session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(guides),
                                     None)
    return int(guides.value)


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import (tonicBridge, tonicCamera, tonicGizmo,
                                      tonicHierarchy, tonicLib, tonicLoops)
        from testUsdviewTonicTube import (Mouse, frameScalp,
                                          pumpUntilCommitted, typeKey, wait)
        from testUsdviewTonicReposition import (centerResidual,
                                                centerResidualNorm,
                                                rootBaseConforms,
                                                rootBaseFootprint,
                                                rootUpperWorldSnapshot,
                                                translatedWorld,
                                                tubeDeltas)
    except ImportError as exc:
        print("FAIL: cannot import Tonic isolated-edit helpers: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    registry = getattr(appController, "_plugRegistry", None)
    container = usdGenTonicTools.container()
    check(view is not None and registry is not None and container is not None,
          "usdview supplies the Tonic workspace and StageView")
    if view is None or registry is None or container is None:
        return 1
    check(frameScalp(stage, view), "a stable top camera frames the scalp")
    view.setFocus()
    wait(30)

    dataModel.selection.setPrimPath("/Scalp")
    registry.getCommandPlugin("usdGenTonicTools.openWorkspace").run()
    registry.getCommandPlugin("usdGenTonicTools.bindScalp").run()
    workspace = container.workspace
    session = container.session
    viewport = container.viewport
    state = container.tonicState
    check(workspace is not None and session is not None and
          session.model is not None and viewport is not None and
          viewport.installed,
          "opening the dock binds a model and installs its input filter")
    if workspace is None or session is None or session.model is None or \
            viewport is None:
        return 1

    def shutdown():
        try:
            viewport.uninstall()
        finally:
            session.deactivate()

    # The model retains its copied scalp after bind; hide the USD mesh so it
    # cannot z-fight the visible tube and component dots.
    stage.GetPrimAtPath("/Scalp").SetActive(False)
    camera = tonicCamera.resolve(view)
    check(camera is not None, "the live camera resolves for real mouse input")
    if camera is None:
        shutdown()
        return 1

    def liveCamera():
        """Match the camera the StageView will resolve for the next press."""
        return tonicCamera.resolve(view) or camera

    def pixel(point):
        result = liveCamera().worldToPixels(point)
        return (result[0], result[1]) if result is not None else None

    def clickControl(actionId):
        """Use an actual dock control, never a direct hierarchy call."""
        button = workspace.button("action", actionId)
        if button is None:
            return False
        button.click()
        wait(15)
        return True

    def choiceControl(identifier, value):
        """Drive a dock descriptor's Qt combobox by its public descriptor.

        The transform tool is the Q/W/E/R icon row, not a combo (DK-04)."""
        if identifier == "transformTool":
            button = workspace.button("tool", value)
            if button is None or not button.isEnabled():
                return False
            button.click()
            wait(10)
            return True
        for descriptor, widget in getattr(workspace, "_paramWidgets", ()):
            if descriptor.id == identifier:
                index = widget.findData(value)
                if index >= 0:
                    widget.setCurrentIndex(index)
                    wait(10)
                    return True
        return False

    def capture(suffix):
        """Optional whole-window evidence, including transparent gizmos."""
        path = os.environ.get("USDGEN_TONIC_ISOLATED_CAPTURE")
        if not path:
            return
        stem, extension = os.path.splitext(path)
        output = stem + "-" + suffix + (extension or ".png")
        window = getattr(appController, "_mainWindow", None)
        shot = window.grab() if window is not None else view.grab()
        check(shot.save(output, "PNG"),
              "the optional isolated-edit %s screenshot was written" % suffix)

    state.snapRadiusPx = max(
        0.1 / max(liveCamera().worldPerPixel((2.0, 0.0, 2.0)), 1e-9), 2.0)
    mouse = Mouse(view)
    mouse.direct = True
    viewport.setPointerInside(True)

    def eventPixel(physical):
        """The physical pixel delivered after Mouse's Qt logical rounding."""
        logical = mouse._point(physical)
        return (float(logical.x()) * mouse._ratio,
                float(logical.y()) * mouse._ratio)

    # Graph stroke is the normal region/tube creation path.
    typeKey(view, "d")
    path = []
    for index, (x0, z0) in enumerate(RECT):
        x1, z1 = RECT[(index + 1) % len(RECT)]
        for step in range(5):
            t = float(step) / 5.0
            point = pixel((x0 + (x1 - x0) * t, 0.0,
                           z0 + (z1 - z0) * t))
            if point is not None:
                path.append(point)
    first = pixel((RECT[0][0], 0.0, RECT[0][1]))
    if first is not None:
        path.append(first)
    mouse.drag(path)
    check(session.graphCounts() == (4, 4, 1) and
          tonicBridge.readTubeIds(session.dll, session.model) == [0],
          "a real Graph stroke creates one root tube")

    # Select the displayed tube and invoke the dock's Subdivide action.  The
    # action must select every new child and focus their editable level.
    workspace.button("mode", "hierarchy").click()
    wait(15)
    surface = sectionCV(session, 0, 1, 0)
    if surface is not None:
        mouse.click(pixel(surface))
    selectedRoot = session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
    check(selectedRoot == [(0, -1, -1)],
          "a visible root surface click selects the root for subdivision")
    check(clickControl("subdivide"), "the hierarchy dock exposes Subdivide")
    children = [tube for tube in tonicBridge.readTubeIds(session.dll,
                                                         session.model)
                if tube != 0]
    childSelection = session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
    check(len(children) >= 2 and childSelection ==
          [(tube, -1, -1) for tube in children] and
          int(state.activeLevel) >= 2,
          "UI subdivision selects every child at its editable level %r"
          % (childSelection,))
    if len(children) < 2:
        shutdown()
        return 1

    target, sibling = int(children[0]), int(children[1])
    targetCv = min(1, len(tonicBridge.tubeCenters(session.dll,
                                                   session.model, target)) - 1)
    targetPoint = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                               target, targetCv)
    targetPixel = pixel(targetPoint)
    check(targetPixel is not None and targetCv > 0,
          "a displayed inner child center CV has a projectable position")
    if targetPixel is None or targetCv <= 0:
        shutdown()
        return 1

    # Component selection and the manipulator are real StageView events.
    workspace.button("mode", "tube").click()
    wait(15)
    hasCenterTool = workspace.button("comp", "center") is not None
    check(hasCenterTool, "the Tube component toolbar exposes Center CV selection")
    if not hasCenterTool:
        shutdown()
        return 1
    workspace.button("comp", "center").click()
    mouse.click(targetPixel)
    selected = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
    forbidden = (session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) +
                 session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) +
                 session.readSelection(tonicLib.TONIC_PICK_SECTION_CV))
    check(selected == [(target, targetCv, -1)] and not forbidden,
          "a plain child-CV click is an exact component-only selection %r"
          % (selected,))
    check(bool(getattr(viewport.loop, "_gizmo", None)) and
          viewport.loop._gizmo.visible,
          "the selected child CV raises its integrated translate gizmo")
    overlay = getattr(viewport, "_gizmoOverlay", None)
    check(overlay is not None and overlay.isVisible(),
          "the selected manipulator is visibly overlaid above the StageView")

    def componentClickPixel(tube, cv):
        """A pixel that picks center CV (tube, cv) and is off every handle.

        A visible gizmo handle wins the press (GZ-01, RigExec parity), so a
        selection click has to land where no handle answers.  Scan the CV's
        own pick footprint; when a handle covers all of it, clear the
        selection so the gizmo hides and the dot itself is clickable.
        """
        handleCamera = liveCamera()
        point = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                             tube, cv)
        centre = pixel(point)
        if centre is None:
            return None
        gizmo = viewport.loop._gizmo
        radius = viewport.loop.componentPickRadiusPx()
        for fraction in (0.0, 0.4, 0.7, 0.95):
            for k in range(16 if fraction else 1):
                angle = 2.0 * math.pi * k / 16.0
                candidate = eventPixel(
                    (centre[0] + fraction * radius * math.cos(angle),
                     centre[1] + fraction * radius * math.sin(angle)))
                if gizmo.visible and gizmo.handleAt(
                        handleCamera, candidate[0], candidate[1]) != \
                        tonicGizmo.HANDLE_NONE:
                    continue
                item = tonicLoops.Sample(
                    session, handleCamera, candidate[0], candidate[1]).item(
                        viewport.loop.componentMask, radius)
                if item is not None and \
                        (item["kind"], item["id"], item["subId"]) == \
                        (tonicLib.TONIC_PICK_CENTER_CV, tube, cv):
                    return candidate
        session.clearSelection()
        viewport.loop._placeGizmo(handleCamera)
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        wait(10)
        return centre

    # GZ-01 (RigExec/a DCC parity): a visible gizmo handle wins the press
    # even over an unselected child CV under it, with no intervening hover.
    # Deliberately lay the existing target gizmo over the sibling's displayed
    # CV.  This changes only the transient overlay; the component targets and
    # all model geometry remain untouched until the actual direct press.
    conflictCv = min(targetCv, len(tonicBridge.tubeCenters(
        session.dll, session.model, sibling)) - 1)
    conflictPoint = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                                 sibling, conflictCv)
    conflictPixel = pixel(conflictPoint)
    oldGizmo = viewport.loop._gizmo
    conflictBefore = {tube: snapshotTube(session, tube) for tube in children}
    if conflictPixel is not None and conflictCv >= 0:
        oldGizmo.origin = tuple(float(value) for value in conflictPoint)
        oldGizmo.push(session)
        conflictCamera = liveCamera()
        direct = tonicLoops.Sample(session, conflictCamera, conflictPixel[0],
                                   conflictPixel[1]).item(
            viewport.loop.componentMask, viewport.loop.pickRadiusPx())
        overlapsGizmo = oldGizmo.handleAt(conflictCamera, conflictPixel[0],
                                           conflictPixel[1]) \
            != tonicGizmo.HANDLE_NONE
        check(overlapsGizmo and direct is not None and
              direct["kind"] == tonicLib.TONIC_PICK_CENTER_CV and
              (direct["id"], direct["subId"], direct["subSubId"]) ==
              (sibling, conflictCv, -1) and
              not viewport.loop._itemSelected(direct),
              "the no-hover press has both an old gizmo hit and an unselected child CV")
        # Do not dispatch hover or move at this pixel: this is precisely the
        # press-time priority path, not a cached hover-assisted selection.
        setattr(viewport.loop, "_hoveredComponent", None)
        session.setHover()
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        # The controller re-places an idle gizmo from the selection at every
        # press (syncDisplayScale -> refreshGizmo).  Hold the relocated one
        # still for this one press so the overlap is the one checked above.
        pressLoop = viewport.loop
        pressLoop.refreshGizmo = lambda _camera: False
        try:
            mouse.press(conflictPixel)
        finally:
            del pressLoop.refreshGizmo
        conflictSelection = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
        conflictForbidden = (
            session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) +
            session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) +
            session.readSelection(tonicLib.TONIC_PICK_SECTION_CV))
        check(conflictSelection == [(target, targetCv, -1)] and
              not conflictForbidden and viewport.loop._dragging and
              oldGizmo.dragging,
              "a press on the old gizmo's handle drags it and leaves the "
              "selection alone, even over an unselected CV %r"
              % (conflictSelection,))
        # Escape: the handle drag cancels with no travel, nothing moved.
        typeKey(view, "escape")
        wait(10)
        check(not viewport.loop._dragging and not oldGizmo.dragging,
              "Escape cancels the handle drag")
        check(all(sameGeometry(snapshotTube(session, tube),
                               conflictBefore[tube]) for tube in children),
              "the cancelled handle press leaves both child geometries exact")
        mouse.release(conflictPixel)

        # Select the sibling where no handle answers (the gizmo is back on
        # the target after the cancel).
        siblingPixel = componentClickPixel(sibling, conflictCv)
        check(siblingPixel is not None, "the sibling CV has a clickable pixel")
        if siblingPixel is not None:
            mouse.click(siblingPixel)
        check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
              [(sibling, conflictCv, -1)],
              "a click off every handle selects the unselected sibling CV %r"
              % (session.readSelection(tonicLib.TONIC_PICK_CENTER_CV),))
        check(all(sameGeometry(snapshotTube(session, tube),
                               conflictBefore[tube]) for tube in children),
              "the selection click leaves both child geometries exact")
        conflictPixel = pixel(tonicBridge.tubeCenterHandle(
            session.dll, session.model, sibling, conflictCv))

        # The newly selected child owns the next real transform; its sibling
        # and the previous gizmo owner must stay fixed.
        siblingBefore = {tube: snapshotTube(session, tube) for tube in children}
        mouse.press(conflictPixel)
        for step in range(1, 5):
            mouse.move((conflictPixel[0] + 18.0 * step / 4.0,
                        conflictPixel[1]))
        mouse.release((conflictPixel[0] + 18.0, conflictPixel[1]))
        siblingAfter = {tube: snapshotTube(session, tube) for tube in children}
        check(not sameGeometry(siblingAfter[sibling], siblingBefore[sibling]) and
              all(sameGeometry(siblingAfter[tube], siblingBefore[tube])
                  for tube in children if tube != sibling),
              "the new child gizmo moves only the directly selected CV owner")

        # Restore the original target for the remaining move/rotate/scale
        # workflow, resolving its current public-model position after the
        # independent sibling edit.
        # The sibling's gizmo may cover the target dot, so aim off-handle.
        targetPoint = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                                   target, targetCv)
        targetClick = componentClickPixel(target, targetCv)
        if targetClick is not None:
            mouse.click(targetClick)
        targetPixel = pixel(targetPoint)
        selected = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
        forbidden = (session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) +
                     session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) +
                     session.readSelection(tonicLib.TONIC_PICK_SECTION_CV))
        check(selected == [(target, targetCv, -1)] and not forbidden,
              "the original child CV can be reselected after the priority case")
    else:
        check(False, "a sibling child center CV is projectable for the no-hover priority case")

    def exposedHandle(kind):
        """A handle outside the exact component-priority hit footprint."""
        handleCamera = liveCamera()
        gizmo = viewport.loop._gizmo
        for record in gizmo.screenHandles(handleCamera):
            if record["kind"] != kind or not record.get("grabbable", False):
                continue
            points = record["points"]
            candidates = points if len(points) > 2 else [points[-1]]
            for rawCandidate in candidates:
                candidate = eventPixel(rawCandidate)
                component = tonicLoops.Sample(
                    session, handleCamera, candidate[0], candidate[1]).item(
                        viewport.loop.componentMask,
                        viewport.loop.componentPickRadiusPx())
                if (gizmo.handleAt(handleCamera, candidate[0], candidate[1]) ==
                        record["handle"] and
                        (component is None or
                         viewport.loop._itemSelected(component))):
                    return candidate
        return None

    viewport.loop._placeGizmo(liveCamera())
    moveStart = exposedHandle("axis")
    check(moveStart is not None,
          "the selected child exposes a Move axis outside unselected CV priority")
    before = {tube: snapshotTube(session, tube) for tube in children}
    moveDragging = False
    moveTrace = None
    if moveStart is not None:
        moveCentre = liveCamera().worldToPixels(viewport.loop._gizmo.origin)
        if moveCentre is None:
            moveEnd = (moveStart[0] + 38.0, moveStart[1])
        else:
            dx, dy = moveStart[0] - moveCentre[0], moveStart[1] - moveCentre[1]
            length = max(math.hypot(dx, dy), 1.0)
            moveEnd = (moveStart[0] + 38.0 * dx / length,
                       moveStart[1] + 38.0 * dy / length)
        mouse.press(moveStart)
        moveDragging = bool(viewport.loop._dragging)
        moveTrace = {"selected": session.readSelection(
                         tonicLib.TONIC_PICK_CENTER_CV),
                     "owners": dict(getattr(viewport.loop, "_centerDrag", {})),
                     "handle": getattr(viewport.loop._gizmo,
                                       "activeHandle", None),
                     "tool": viewport.loop.transformTool()}
        for step in range(1, 7):
            mouse.move((moveStart[0] + (moveEnd[0] - moveStart[0]) * step / 6.0,
                        moveStart[1] + (moveEnd[1] - moveStart[1]) * step / 6.0))
        moveTrace["pending"] = bool(viewport.loop._pendingEdit)
        moveTrace["status"] = getattr(viewport.loop, "_lastStatus", "")
        moveTrace["error"] = session.lastError()
        mouse.release(moveEnd)
    after = {tube: snapshotTube(session, tube) for tube in children}
    check(moveDragging and not sameGeometry(after[target], before[target]),
          "the real exposed-axis gizmo drag changes the targeted child component %r"
          % moveTrace)
    check(all(sameGeometry(after[tube], before[tube])
              for tube in children if tube != target),
          "the drag leaves every sibling's center, sections and world points unchanged")
    capture("move")
    check(pumpUntilCommitted(viewport, session),
          "the child edit completes its idle commit")
    committed = {tube: snapshotTube(session, tube) for tube in children}
    check(all(sameGeometry(committed[tube], after[tube]) for tube in children),
          "the committed model preserves the isolated child edit")
    typeKey(view, "z", ("ctrl",))
    wait(25)
    undone = {tube: snapshotTube(session, tube) for tube in children}
    check(all(sameGeometry(undone[tube], before[tube]) for tube in children),
          "one undo restores the target and leaves sibling geometry exact")
    typeKey(view, "y", ("ctrl",))
    wait(25)
    redone = {tube: snapshotTube(session, tube) for tube in children}
    check(all(sameGeometry(redone[tube], after[tube]) for tube in children),
          "one redo reapplies only the committed child edit")

    # Rotate and Scale are distinct user-selected transform tools.  They
    # operate on the already selected child CV around its child root; each
    # drag feeds several samples so their press-time snapshots cannot drift.
    hasRotate = choiceControl("transformTool", "rotate")
    check(hasRotate, "the Tube panel exposes the Rotate transform tool")
    if not hasRotate:
        shutdown()
        return 1
    viewport.loop._placeGizmo(liveCamera())
    rotateStart = exposedHandle("view")
    check(rotateStart is not None,
          "the selected child exposes a visible Rotate view ring")
    if rotateStart is not None:
        rotateBefore = {tube: snapshotTube(session, tube) for tube in children}
        rotateEnd = (rotateStart[0] + 25.0, rotateStart[1] - 20.0)
        mouse.press(rotateStart)
        for step in range(1, 6):
            mouse.move((rotateStart[0] + 25.0 * step / 5.0,
                        rotateStart[1] - 20.0 * step / 5.0))
        mouse.release(rotateEnd)
        rotateAfter = {tube: snapshotTube(session, tube) for tube in children}
        check(not sameGeometry(rotateAfter[target], rotateBefore[target]) and
              all(sameGeometry(rotateAfter[tube], rotateBefore[tube])
                  for tube in children if tube != target),
              "Rotate changes only the selected child and leaves siblings exact")

    hasScale = choiceControl("transformTool", "scale")
    check(hasScale, "the Tube panel exposes the Scale transform tool")
    if not hasScale:
        shutdown()
        return 1
    viewport.loop._placeGizmo(liveCamera())
    scaleStart = exposedHandle("axis")
    check(scaleStart is not None,
          "the selected child exposes a visible Scale axis")
    if scaleStart is not None:
        scaleBefore = {tube: snapshotTube(session, tube) for tube in children}
        scaleCenter = liveCamera().worldToPixels(viewport.loop._gizmo.origin)
        if scaleCenter is None:
            scaleEnd = (scaleStart[0] + 20.0, scaleStart[1])
        else:
            scaleEnd = (scaleStart[0] + 0.35 * (scaleStart[0] - scaleCenter[0]),
                        scaleStart[1] + 0.35 * (scaleStart[1] - scaleCenter[1]))
        mouse.press(scaleStart)
        for step in range(1, 6):
            mouse.move((scaleStart[0] + (scaleEnd[0] - scaleStart[0]) * step / 5.0,
                        scaleStart[1] + (scaleEnd[1] - scaleStart[1]) * step / 5.0))
        mouse.release(scaleEnd)
        scaleAfter = {tube: snapshotTube(session, tube) for tube in children}
        check(not sameGeometry(scaleAfter[target], scaleBefore[target]) and
              all(sameGeometry(scaleAfter[tube], scaleBefore[tube])
                  for tube in children if tube != target),
              "Scale changes only the selected child and leaves siblings exact")
        capture("rotate-scale")

    # Redo restores the selected component at its moved location.  Resolve
    # fresh pixels from the public model before building a selection band;
    # the original screen coordinate is deliberately stale after the drag.
    targetPoint = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                               target, targetCv)
    targetPixel = pixel(targetPoint)

    # Both selection bands intentionally cross visible child curves and
    # surfaces.  Center mode must emit components only, never stale tubes or
    # section entries underneath them.
    otherPoint = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                              sibling, targetCv)
    otherPixel = pixel(otherPoint)
    if targetPixel is not None and otherPixel is not None:
        # A mode switch can leave the all-child hierarchy selection behind.
        # Recreate that public state before each band: the component band has
        # to replace it rather than carrying its TubeVert entries forward.
        session.select(tonicLib.TONIC_PICK_TUBE_VERT, children)
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        lo = (min(targetPixel[0], otherPixel[0]) - 28.0,
              min(targetPixel[1], otherPixel[1]) - 28.0)
        hi = (max(targetPixel[0], otherPixel[0]) + 28.0,
              max(targetPixel[1], otherPixel[1]) + 28.0)
        mouse.drag([lo, hi], ("shift",))
        band = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
        extras = (session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) +
                  session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) +
                  session.readSelection(tonicLib.TONIC_PICK_SECTION_CV))
        check((target, targetCv, -1) in band and
              (sibling, targetCv, -1) in band and not extras,
              "a Shift box across child surfaces selects center CVs only %r"
              % (band,))

        hasLasso = choiceControl("selectionShape", "lasso")
        check(hasLasso, "the Tube panel exposes the Lasso selection shape")
        if not hasLasso:
            shutdown()
            return 1
        session.select(tonicLib.TONIC_PICK_TUBE_VERT, children)
        session.publish(tonicLib.TONIC_DIRTY_SELECTION)
        polygon = [lo, (hi[0], lo[1]), hi, (lo[0], hi[1]), lo]
        mouse.press(polygon[0], ("shift",))
        for point in polygon[1:]:
            mouse.move(point, ("shift",))
        mouse.release(polygon[-1], ("shift",))
        lasso = session.readSelection(tonicLib.TONIC_PICK_CENTER_CV)
        extras = (session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) +
                  session.readSelection(tonicLib.TONIC_PICK_SECTION_RING) +
                  session.readSelection(tonicLib.TONIC_PICK_SECTION_CV))
        check((target, targetCv, -1) in lasso and
              (sibling, targetCv, -1) in lasso and not extras,
              "a Shift lasso across child surfaces selects center CVs only %r"
              % (lasso,))
    else:
        check(False, "two child center CVs are projectable for component bands")

    # Generated curves are a display choice.  Hiding them must not steal a
    # component pick.  Clearing is an authored operation: it must stay clear
    # through another child edit until the artist explicitly refills it.
    generatedControl = getattr(workspace, "_generatedCheck", None)
    check(generatedControl is not None,
          "the dock exposes generated-curves visibility")
    if generatedControl is None:
        shutdown()
        return 1
    generatedControl.setChecked(False)
    wait(10)
    session.clearSelection()
    mouse.click(targetPixel)
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(target, targetCv, -1)],
          "hidden generated curves do not obstruct a child-CV pick")
    generatedControl.setChecked(True)
    check(guideCount(session) > 0,
          "the pre-clear generated preview contains live curves")
    workspace.button("mode", "fill").click()
    wait(15)
    hasClear = clickControl("clearGeneratedCurves")
    check(hasClear, "the Fill panel exposes Clear generated curves")
    if not hasClear:
        shutdown()
        return 1
    check(guideCount(session) == 0,
          "Clear removes generated curves")
    typeKey(view, "z", ("ctrl",))
    wait(20)
    check(guideCount(session) > 0,
          "undoing Clear restores the generated preview")
    typeKey(view, "y", ("ctrl",))
    wait(20)
    check(guideCount(session) == 0,
          "redoing Clear restores the explicit suppression")

    workspace.button("mode", "tube").click()
    wait(15)
    if workspace.button("comp", "center") is not None:
        workspace.button("comp", "center").click()
    choiceControl("transformTool", "move")
    viewport.loop._placeGizmo(liveCamera())
    targetPoint = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                               target, targetCv)
    targetPixel = pixel(targetPoint)
    session.clearSelection()
    if targetPixel is not None:
        mouse.click(targetPixel)
        mouse.press(targetPixel)
        mouse.move((targetPixel[0] + 12.0, targetPixel[1]))
        mouse.release((targetPixel[0] + 12.0, targetPixel[1]))
    check(guideCount(session) == 0,
          "a later isolated child edit does not silently repopulate Clear")
    workspace.button("mode", "fill").click()
    wait(15)
    hasRefill = clickControl("refill")
    check(hasRefill, "the Fill panel exposes an explicit guide refill")
    if not hasRefill:
        shutdown()
        return 1
    check(guideCount(session) > 0,
          "explicit Refill restores generated curves")

    # -- two-branch active-cut workflow ---------------------------------
    # The first region/root A already has real L2 children and a sculpted
    # child edit from the workflow above.  Add an independent root B through
    # the Graph UI, then exercise mixed-depth navigation using only dock,
    # keyboard, and StageView gestures.  Native active-cut reads below are
    # observations of the published model, not test-side selection setup.
    workspace.button("mode", "graph").click()
    wait(15)
    typeKey(view, "d")
    second = ((0.1, 0.1), (0.8, 0.1), (0.8, 0.8), (0.1, 0.8))
    secondPath = []
    for index, (x0, z0) in enumerate(second):
        x1, z1 = second[(index + 1) % len(second)]
        for step in range(5):
            t = float(step) / 5.0
            point = pixel((x0 + (x1 - x0) * t, 0.0,
                           z0 + (z1 - z0) * t))
            if point is not None:
                secondPath.append(point)
    secondPath.append(pixel((second[0][0], 0.0, second[0][1])))
    mouse.drag(secondPath)
    levelOne = [tube for tube in tonicBridge.readTubeIds(
        session.dll, session.model)
                if tonicHierarchy.tubeLevel(session.dll, session.model,
                                            tube) == 1]
    rootsOk = 0 in levelOne and len(levelOne) >= 2
    check(rootsOk,
          "the UI Graph draw leaves independent L1 roots A/B (%r)"
          % (levelOne,))
    rootA = 0
    rootB = next((tube for tube in levelOne if tube != rootA), -1)
    childrenA = tonicHierarchy.tubeChildren(session.dll, session.model,
                                             rootA)
    check(rootB >= 0 and len(childrenA) >= 2,
          "root A retains its subdivided branch beside root B (%r/%r)"
          % (rootA, rootB))

    activeCut = tonicHierarchy.supportsActiveCut(session.dll)
    check(activeCut, "the model exposes per-branch active-cut visibility")
    if activeCut:
        active = tonicHierarchy.getActiveCutEnabled(session.dll,
                                                    session.model)
        check(active is True,
              "the real hierarchy UI enables model-owned active-cut state")

    workspace.button("mode", "hierarchy").click()
    wait(15)
    # The first branch may still be expanded after the preceding child edit.
    # Exit it through the current child selection before picking its parent;
    # this keeps the test on the same branch-local UI path as an artist.
    if activeCut and tonicHierarchy.getTubeExpanded(session.dll,
                                                    session.model, rootA):
        exitPoint = sectionCV(session, childrenA[0], 1, 0)
        exitPixel = pixel(exitPoint) if exitPoint is not None else None
        if exitPixel is not None:
            mouse.click(exitPixel)
        check(session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT) ==
              [(childrenA[0], -1, -1)],
              "the current child is selected before branch exit")
        check(clickControl("exitLevel"),
              "the Hierarchy dock exits the already expanded branch")
        check(tonicHierarchy.getTubeExpanded(session.dll, session.model,
                                              rootA) is False,
              "explicit branch exit collapses only root A")
    rootPoint = sectionCV(session, rootA, 1, 0)
    rootPixel = pixel(rootPoint) if rootPoint is not None else None
    if rootPixel is not None:
        mouse.click(rootPixel)
    rootSelection = session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
    check(rootSelection == [(rootA, -1, -1)],
          "a real parent-surface click selects root A before branch entry %r"
          % (rootSelection,))
    check(clickControl("enterLevel"),
          "the Hierarchy dock exposes explicit branch entry")
    if activeCut:
        check(tonicHierarchy.getTubeExpanded(session.dll, session.model,
                                              rootA) is True,
              "Enter level expands only root A's branch")
        visibleA = [tonicHierarchy.isTubeVisible(
            session.dll, session.model, tube) for tube in childrenA]
        check(tonicHierarchy.isTubeVisible(session.dll, session.model,
                                           rootA) is False and
              all(visibleA) and
              tonicHierarchy.isTubeVisible(session.dll, session.model,
                                           rootB) is True,
              "the mixed frontier shows A children while B remains at L1")

    # Edit one A child through the real Tube CV/gizmo path at L2, then return
    # through the Hierarchy dock and verify parent selection is restored.
    childA = childrenA[0]
    childCv = min(1, len(tonicBridge.tubeCenters(
        session.dll, session.model, childA)) - 1)
    childPoint = tonicBridge.tubeCenterHandle(session.dll, session.model,
                                              childA, childCv)
    childPixel = pixel(childPoint)
    childBefore = snapshotTube(session, childA)
    workspace.button("mode", "tube").click()
    wait(15)
    if workspace.button("comp", "center") is not None:
        workspace.button("comp", "center").click()
    if childPixel is not None:
        mouse.click(childPixel)
    check(session.readSelection(tonicLib.TONIC_PICK_CENTER_CV) ==
          [(childA, childCv, -1)],
          "the L2 branch edit selects one child CV through Tube UI")
    moveHandle = exposedHandle("axis")
    if moveHandle is not None:
        moveEnd = (moveHandle[0] + 18.0, moveHandle[1])
        mouse.press(moveHandle)
        mouse.move(moveEnd)
        mouse.release(moveEnd)
    childEdited = snapshotTube(session, childA)
    check(not sameGeometry(childBefore, childEdited),
          "the selected A child changes through its visible gizmo")
    workspace.button("mode", "hierarchy").click()
    wait(15)
    check(clickControl("exitLevel"),
          "the Hierarchy dock exposes explicit branch exit")
    parentSelection = session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
    check(parentSelection == [(rootA, -1, -1)],
          "exiting L2 restores the edited branch's L1 parent %r"
          % (parentSelection,))

    # A parent Move carries the A parent boundary.  B is an independent branch
    # and must remain geometrically untouched.
    childrenBefore = {tube: snapshotTube(session, tube) for tube in childrenA}
    rootBaseAnchor = rootBaseFootprint(session, rootA)
    childSpan = parentBoundarySpan(childrenBefore)
    check(any(value > 1e-4 for value in childSpan),
          "root A has a non-degenerate asymmetric parent boundary %r"
          % (childSpan,))
    check(rootBaseAnchor is not None,
          "root A retains a slot-to-edge base mapping before its whole-tube Move")
    rootBBefore = snapshotTube(session, rootB)
    workspace.button("mode", "tube").click()
    wait(15)
    if workspace.button("comp", "tube") is not None:
        workspace.button("comp", "tube").click()
    parentPoint = sectionCV(session, rootA, 1, 0)
    parentPixel = pixel(parentPoint) if parentPoint is not None else None
    if parentPixel is not None:
        mouse.click(parentPixel)
    parentWholeSelection = session.readSelection(tonicLib.TONIC_PICK_TUBE_VERT)
    check(parentWholeSelection == [(rootA, -1, -1)],
          "the parent whole-tube surface is selected before its Move %r"
          % (parentWholeSelection,))
    parentHandle = exposedHandle("axis")
    check(parentHandle is not None,
          "the selected parent exposes a Move axis handle %r" %
          (parentHandle,))
    if parentHandle is not None:
        parentEnd = (parentHandle[0] + 16.0, parentHandle[1])
        mouse.press(parentHandle)
        mouse.move(parentEnd)
        mouse.release(parentEnd)
    childrenAfter = {tube: snapshotTube(session, tube) for tube in childrenA}
    childMoved = {tube: not sameGeometry(childrenAfter[tube],
                                          childrenBefore[tube])
                  for tube in childrenA}
    childShape = {tube: translatedRigid(childrenBefore[tube],
                                        childrenAfter[tube])
                  for tube in childrenA}
    print("info: parent whole move selection=%r handle=%r moved=%r shape=%r" %
          (parentWholeSelection, parentHandle, childMoved, childShape))
    check(parentWholeSelection == [(rootA, -1, -1)] and
          parentHandle is not None and all(childMoved.values()) and
          all(childShape.values()),
          "the parent boundary applies one rigid translation to every A child")
    check(sameGeometry(snapshotTube(session, rootB), rootBBefore),
          "the parent A edit leaves independent root B unchanged")

    # Graph Reposition reshapes the graph region.  It must conform the L1
    # base to that polygon, transport the L1 upper sculpt, refresh generated
    # child roots from the conformed attachment, and leave B exact.
    def upperRootSculpt(snapshot):
        # K14 intentionally replaces the L1 attachment center CV0 from the
        # conformed base.  Its upper centers and all sections above ring 0
        # are the sculpted material that must travel with the region support.
        return tuple((tube, tuple(centers[1:]), sections)
                     for tube, centers, sections in snapshot)

    def upperMotion(before, after):
        """Compact evidence when a region reshape is not a rigid transport."""
        old = dict((int(tube), tuple(centers) + tuple(sections))
                   for tube, centers, sections in before)
        new = dict((int(tube), tuple(centers) + tuple(sections))
                   for tube, centers, sections in after)
        rows = []
        for tube in sorted(old):
            prior, current = old[tube], new.get(tube, ())
            if len(prior) != len(current) or not prior:
                rows.append((tube, "layout"))
                continue
            deltas = [tuple(float(now[axis]) - float(was[axis])
                            for axis in range(3))
                      for was, now in zip(prior, current)]
            first = deltas[0]
            spread = max(math.sqrt(sum((delta[axis] - first[axis]) ** 2
                                       for axis in range(3)))
                         for delta in deltas)
            magnitude = max(math.sqrt(sum(value * value for value in delta))
                            for delta in deltas)
            rows.append((tube, first, spread, magnitude))
        return tuple(rows)

    def channelMotion(pointsBefore, pointsAfter):
        if len(pointsBefore) != len(pointsAfter) or not pointsBefore:
            return "layout"
        deltas = [tuple(float(now[axis]) - float(was[axis])
                        for axis in range(3))
                  for was, now in zip(pointsBefore, pointsAfter)]
        first = deltas[0]
        return {"first": first,
                "spread": max(math.sqrt(sum((delta[axis] - first[axis]) ** 2
                                              for axis in range(3)))
                              for delta in deltas),
                "maxMagnitude": max(math.sqrt(sum(value * value
                                                   for value in delta))
                                    for delta in deltas)}

    childrenBefore = {tube: snapshotTube(session, tube) for tube in childrenA}
    rootBBefore = snapshotTube(session, rootB)
    rootUpperRawBefore = rootUpperWorldSnapshot(session, (rootA,))
    rootUpperBefore = upperRootSculpt(rootUpperRawBefore)
    rootSectionsBefore = snapshotTube(session, rootA)[1]
    childRootsBefore = {tube: childrenBefore[tube][0][0] for tube in childrenA}
    sculptWitnessBefore = centerResidual(tubeDeltas(session, target), targetCv)
    workspace.button("mode", "graph").click()
    wait(15)
    typeKey(view, "m")
    graphStart = pixel((RECT[0][0], 0.0, RECT[0][1]))
    graphEnd = (graphStart[0] + 14.0, graphStart[1] - 9.0) \
        if graphStart is not None else None
    if graphStart is not None and graphEnd is not None:
        mouse.press(graphStart)
        mouse.move(graphEnd)
        mouse.release(graphEnd)
    childrenAfter = {tube: snapshotTube(session, tube) for tube in childrenA}
    rootBaseConformed = (rootBaseConforms(session, rootBaseAnchor)
                          if rootBaseAnchor is not None else (False, None, None))
    rootUpperRawAfter = rootUpperWorldSnapshot(session, (rootA,))
    rootUpperAfter = upperRootSculpt(rootUpperRawAfter)
    rootSectionsAfter = snapshotTube(session, rootA)[1]
    rootUpperDelta = translatedWorld(rootUpperBefore, rootUpperAfter, (rootA,))
    rootUpperMotion = upperMotion(rootUpperBefore, rootUpperAfter)
    rootUpperChannels = {"centers": channelMotion(
        rootUpperRawBefore[0][1][1:], rootUpperRawAfter[0][1][1:]),
        "sectionsAboveBase": channelMotion(rootUpperRawBefore[0][2],
                                             rootUpperRawAfter[0][2]),
        "authoredSectionsSame": sameGeometry(rootSectionsBefore,
                                               rootSectionsAfter)}
    childRootsAfter = {tube: childrenAfter[tube][0][0] for tube in childrenA}
    childRootsRefreshed = all(not sameGeometry(childRootsBefore[tube],
                                               childRootsAfter[tube])
                              for tube in childrenA)
    sculptWitnessAfter = centerResidual(tubeDeltas(session, target), targetCv)
    sculptWitnessRetained = (sculptWitnessBefore is not None and
                             centerResidualNorm(sculptWitnessBefore) > 1e-5 and
                             sculptWitnessAfter is not None and
                             centerResidualNorm(sculptWitnessAfter) > 1e-5)
    rootUpperTransported = (rootUpperDelta is not None and
                            math.sqrt(sum(component * component
                                          for component in rootUpperDelta)) > 1e-5)
    check(rootBaseConformed[0] and rootUpperTransported and
          childRootsRefreshed and sculptWitnessRetained,
          "Graph Reposition conforms A's L1 base to the live region polygon, "
          "transports upper sculpt, and refreshes child roots %r" %
           {"baseError": rootBaseConformed[1:], "upperDelta": rootUpperDelta,
           "upperMotion": rootUpperMotion,
           "upperChannels": rootUpperChannels,
           "rootsRefreshed": childRootsRefreshed,
           "sculptWitness": (sculptWitnessBefore, sculptWitnessAfter)})
    check(sameGeometry(snapshotTube(session, rootB), rootBBefore),
          "Graph Reposition leaves unrelated root B unchanged")

    shutdown()
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicIsolatedEditing needs testusdview")
    sys.exit(0)
