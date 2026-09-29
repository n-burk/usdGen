# testUsdviewPomadeFill -- T3 (TS-03): Fill mode driven by REAL mouse and
# key events over the StageView and the dock.
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testUsdviewPomadeFill.py \
#               examples/pomade-graph-scalp.usda
#
# Proven here, through the installed filters and the real dock widgets:
#
#   * `3` opens Fill with guides already on screen (MD-01): nobody has to
#     touch a Fill parameter before the hair appears;
#   * a click on the tube body in Params selects that tube (TUBE_VERT);
#   * the Density spin box shows exactly what the model holds
#     (descriptor.get), and typing a value + Enter into it writes the
#     model, regrows the guides, is one undo step and Ctrl+Z restores it;
#   * `v` Length ramp: the profile is keyed by each guide's ROOT radius in
#     the root ring (0 centre .. 1 wall, edge-bias remapped) and its values
#     are length fractions the kernels clamp to [0, 1] (pomadeTube.cpp /
#     pomadeKernels.cu). So the test first shortens every guide to half
#     length through the Length profile row -- on a full-length ramp "up =
#     longer" cannot be observed, because nothing grows past full length --
#     then a press and a drag UP raises the knot at the radius under the
#     press (the pressed strand's root, or the body hit projected into the
#     ring) toward 1, the tips lengthen, the release refills at FULL
#     density with the preview fraction put back, the status reads
#     'length ramp r=..: ..', the committed tube carries the profile, and
#     one Ctrl+Z puts the old ramp and tip lengths back;
#   * a no-travel ramp click leaves no undo step.
import ctypes
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


def fillParams(session):
    """(density, profile pairs) the model holds for tube 0."""
    density = ctypes.c_float(0.0)
    cvCount = ctypes.c_int(0)
    seed = ctypes.c_int(0)
    edgeBias = ctypes.c_float(0.0)
    profile = (ctypes.c_float * 128)()
    got = ctypes.c_int(0)
    if session.dll.Pomade_GetFillParams(
            session.model, ctypes.byref(density), ctypes.byref(cvCount),
            ctypes.byref(seed), ctypes.byref(edgeBias), profile, 128,
            ctypes.byref(got)) != 0:
        return None, None
    return (float(density.value),
            [float(profile[i]) for i in range(got.value)])


def guideLengths(session):
    """Arc length of every guide in the live preview, in order."""
    guides = ctypes.c_int(0)
    cvs = ctypes.c_int(0)
    if session.dll.Pomade_GetGuideCounts(session.model, ctypes.byref(guides),
                                        ctypes.byref(cvs)) != 0:
        return []
    if guides.value <= 0 or cvs.value <= 0:
        return []
    xyz = (ctypes.c_float * (3 * guides.value * cvs.value))()
    counts = (ctypes.c_int * guides.value)()
    got = ctypes.c_int(0)
    if session.dll.Pomade_ReadGuidePreview(
            session.model, xyz, len(xyz), counts, len(counts),
            ctypes.byref(got)) != 0:
        return []
    out = []
    offset = 0
    for g in range(got.value):
        n = int(counts[g])
        total = 0.0
        for i in range(1, n):
            a = 3 * (offset + i - 1)
            b = 3 * (offset + i)
            total += math.sqrt(sum((xyz[b + k] - xyz[a + k]) ** 2
                                   for k in range(3)))
        out.append(total)
        offset += n
    return out


def mean(values):
    return sum(values) / float(len(values)) if values else 0.0


def rootRadius(session, guide):
    """|(ru, rv)| of one guide's root from the root census, or None."""
    count = ctypes.c_int(0)
    if session.dll.Pomade_ReadGuideRoots(session.model, None, None, None, 0,
                                        ctypes.byref(count)) != 0:
        return None
    n = int(count.value)
    if not 0 <= guide < n:
        return None
    ru = (ctypes.c_float * (2 * n))()
    if session.dll.Pomade_ReadGuideRoots(session.model, None, None, ru, n,
                                        ctypes.byref(count)) != 0:
        return None
    return min(1.0, math.hypot(ru[2 * guide], ru[2 * guide + 1]))


def knotValue(profile, position):
    """The value of the knot at `position` in flattened pairs, or None."""
    for i in range(0, len(profile) - 1, 2):
        if abs(profile[i] - position) < 1e-3:
            return profile[i + 1]
    return None


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    import pomadeT3
    from pomadeT3 import check, info, typeKey, wait
    try:
        from usdGenPomadeTools import pomadeLib, pomadeLoopsFill, pomadePanels
        import pomadeT3Groom
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools: %s" % exc)
        return 1
    from pxr.Usdviewq.qt import QtCore

    groom = pomadeT3Groom.Groom(appController)
    if not groom.build():
        groom.close()
        print("testUsdviewPomadeFill: %d failure(s)" % pomadeT3.failureCount())
        return 1
    session, viewport, state = groom.session, groom.viewport, groom.state
    view, mouse, stage = groom.view, groom.mouse, groom.stage
    workspace = groom.workspace
    TUBE = pomadeLib.POMADE_PICK_TUBE_VERT
    QTest = pomadeT3._qtTest().QTest

    # Side on, so the stub stands up across the frame.
    camera = groom.side()
    check(camera is not None, "the side camera resolves")
    if camera is None:
        groom.close()
        return 1
    centers = groom.centers(0)
    check(len(centers) == pomadeT3Groom.STUB_CVS,
          "the stub has its center CVs (%d)" % len(centers))

    # -- 3: Fill, with guides already there -------------------------------
    typeKey(view, "3")
    loop = viewport.loop
    check(state.activeMode == "fill" and loop is not None and
          loop.modeId == "fill",
          "the 3 hotkey selects Fill mode and builds FillLoop (%r)"
          % state.activeMode)
    check(state.fillSubMode in ("", "params") and loop.subMode() == "params",
          "opening in Params (%r)" % loop.subMode())
    fullGuides = pomadeT3.guideCount(session)
    check(fullGuides > 0,
          "MD-01: guides are on screen without touching a Fill control "
          "(%d)" % fullGuides)
    workspace.refresh()
    check(workspace.activeMode == "fill",
          "the dock followed the hotkey into Fill (%r)" % workspace.activeMode)

    # -- a click on the tube body selects it ------------------------------
    session.clearSelection()
    session.publish(pomadeLib.POMADE_DIRTY_SELECTION)
    body = groom.pixel((centers[2][0], centers[2][1], centers[2][2]))
    probe = session.pickItem(camera, body[0], body[1], loop.pickRadiusPx(),
                             TUBE)
    check(probe is not None and int(probe["id"]) == 0,
          "the tube body is under the click pixel (%r)" % (probe,))
    mark = len(groom.messages)
    clickDepth = groom.undoDepth()
    mouse.click(body)
    wait(20)
    picked = session.readSelection(TUBE)
    check([entry[0] for entry in picked] == [0],
          "a Params click on the tube body selects it (%r, %r)"
          % (picked, groom.since(mark)))
    check(any("1 tube(s) selected" in m for m in groom.since(mark)),
          "and the status says so (%r)" % groom.since(mark)[-1:])
    check(not viewport.gestureActive and groom.undoDepth() == clickDepth,
          "the selecting click leaves no gesture and no undo step (%d -> %d)"
          % (clickDepth, groom.undoDepth()))

    # -- the Density spin box agrees with the model -----------------------
    workspace.refresh()
    descriptor, spin = groom.paramWidget("density")
    check(descriptor is not None and spin is not None,
          "the Fill form has a Density row (%r)" % (workspace.parameterIds(),))
    if descriptor is None or spin is None:
        groom.close()
        return 1
    modelDensity, baseProfile = fillParams(session)
    shown = float(spin.value())
    check(abs(shown - descriptor.get(state, session)) < 1e-3 and
          abs(shown - modelDensity) < 1e-3,
          "the spin box shows what the model holds (widget %g, descriptor "
          "%g, model %g)" % (shown, descriptor.get(state, session),
                              modelDensity))
    check(spin.minimum() <= modelDensity <= spin.maximum(),
          "and the model's value is inside the row's range [%g, %g]"
          % (spin.minimum(), spin.maximum()))
    target = round(modelDensity * 1.5) if modelDensity >= 2.0 else 150.0
    depth = groom.undoDepth()
    mark = len(groom.messages)
    spin.setFocus()
    wait(10)
    spin.selectAll()
    QTest.keyClicks(spin, "%d" % int(target))
    QTest.keyClick(spin, QtCore.Qt.Key.Key_Return)
    wait(30)
    density, _profile = fillParams(session)
    grown = pomadeT3.guideCount(session)
    check(abs(density - target) < 1e-3,
          "typing %d + Enter into Density writes the model (%g, %r)"
          % (int(target), density, groom.since(mark)[-2:]))
    check(grown > fullGuides,
          "and the denser fill regrows more guides (%d -> %d)"
          % (fullGuides, grown))
    check(groom.undoDepth() == depth + 1,
          "as exactly one undo step (%d -> %d, %r)"
          % (depth, groom.undoDepth(), session.undoLabel(0)))
    check(not spin.hasFocus(),
          "Enter hands the keyboard back to the viewport")
    view.setFocus()
    wait(10)
    typeKey(view, "z", ("ctrl",))
    wait(30)
    workspace.refresh()
    density, _profile = fillParams(session)
    check(abs(density - modelDensity) < 1e-3 and
          pomadeT3.guideCount(session) == fullGuides,
          "Ctrl+Z restores the density and its guides (%g, %d guides)"
          % (density, pomadeT3.guideCount(session)))
    check(abs(float(spin.value()) - modelDensity) < 1e-3,
          "and the spin box follows the undo (%g)" % float(spin.value()))

    # -- v: the length ramp ------------------------------------------------
    typeKey(view, "v")
    check(state.fillSubMode == "preview" and loop.subMode() == "preview",
          "V selects the Length ramp sub-mode (%r)" % state.fillSubMode)
    storedFraction = float(session.dll.Pomade_GetPreviewFraction(
        session.model))
    fullLengths = guideLengths(session)
    # Every guide at half length first: the kernels clamp a length
    # fraction to [0, 1], so on the default full-length ramp a drag up has
    # nothing to lengthen. The panel's Length profile row is the artist's
    # route to that ramp.
    HALF = 0.5
    profileRow = [d for d in pomadePanels.descriptors("fill", state)
                  if d.id == "lengthProfile"]
    check(len(profileRow) == 1, "the Fill form has a Length profile row")
    if profileRow:
        profileRow[0].set(state, session, [0.0, HALF, 1.0, HALF])
        wait(20)
    _d, halfProfile = fillParams(session)
    check(halfProfile == [0.0, HALF, 1.0, HALF],
          "the Length profile row wrote a uniform half-length ramp (%r)"
          % (halfProfile,))
    baseLengths = guideLengths(session)
    check(abs(mean(baseLengths) - HALF * mean(fullLengths)) <
          0.1 * mean(fullLengths),
          "and the guides grew to about half length (mean %.4f of %.4f)"
          % (mean(baseLengths), mean(fullLengths)))
    check(len(baseLengths) == fullGuides and mean(baseLengths) > 0.0,
          "the guide preview reads back (%d guides, mean length %.4f)"
          % (len(baseLengths), mean(baseLengths)))
    _d, baseProfile = fillParams(session)
    tipCV = centers[3]
    press = groom.pixel(tipCV)
    probe = session.pickItem(camera, press[0], press[1], loop.pickRadiusPx(),
                             TUBE)
    check(probe is not None and int(probe["id"]) == 0,
          "CV 3's pixel is on the tube body (%r)" % (probe,))

    # A click without travel first: no step, nothing changed.
    depth = groom.undoDepth()
    mouse.click(press)
    wait(20)
    _d, clickProfile = fillParams(session)
    check(groom.undoDepth() == depth and clickProfile == baseProfile,
          "a ramp click without travel leaves no undo step and no ramp "
          "(%d -> %d, %r)" % (depth, groom.undoDepth(), clickProfile))
    check(abs(float(session.dll.Pomade_GetPreviewFraction(session.model)) -
              storedFraction) < 1e-6,
          "and puts the preview fraction back")

    # Where the knot must land: the radius under the press. The loop picks
    # strands and the tube together; a strand names its own root, the tube
    # body at CV 3's pixel (on the center line) is the ring's centre.
    hit = session.pickItem(camera, press[0], press[1], loop.pickRadiusPx(),
                           loop.pickMask)
    edgeBias = pomadePanels.descriptors("fill", state)
    edgeBias = [d for d in edgeBias if d.id == "edgeBias"][0].get(state,
                                                                  session)
    if hit is not None and hit["kind"] == pomadeLib.POMADE_PICK_GUIDE and \
            rootRadius(session, int(hit["id"])) is not None:
        want = pomadeLoopsFill.snapKnot(pomadeLoopsFill.profilePosition(
            rootRadius(session, int(hit["id"])), edgeBias))
        slack = 1e-3
        where = "strand %d's root" % int(hit["id"])
    else:
        want, slack, where = 0.0, 0.1, "the ring centre"
    mark = len(groom.messages)
    depth = groom.undoDepth()
    mouse.press(press)
    check(viewport.gestureActive and session.gestureActive,
          "the ramp press opens one gesture")
    r = loop._ramp[1] if loop._ramp is not None else None
    check(r is not None and abs(r - want) <= slack,
          "the knot is the radius under the press, %s (r=%r, want %.2f)"
          % (where, r, want))
    if r is None:
        r = want
    duringFraction = float(session.dll.Pomade_GetPreviewFraction(
        session.model))
    # 40% of RAMP_PIXELS: half length + 0.4 = 0.9, short of the 1.0 cap so
    # the proportionality is observable.
    travel = 0.4 * pomadeLoopsFill.RAMP_PIXELS
    for step in range(1, 7):
        mouse.move((press[0], press[1] - travel * step / 6.0))
        wait(5)
    liveGuides = pomadeT3.guideCount(session)
    # The readout is throttled (RAMP_STATUS_INTERVAL), so the last line
    # shown may trail the cursor; it must name this knot and a length that
    # has already risen above the half-length start.
    prefix = "length ramp r=%.2f: " % r
    readout = [m for m in groom.since(mark) if prefix in m]
    shown = None
    if readout:
        try:
            shown = float(readout[-1].split(prefix, 1)[1].split()[0])
        except (IndexError, ValueError):
            shown = None
    check(shown is not None and HALF < shown <= 1.0,
          "the drag reads the radius and the live length out (%r)"
          % readout[-1:])
    mouse.release((press[0], press[1] - travel))
    wait(30)
    check(not viewport.gestureActive and not session.gestureActive,
          "the release closes the gesture")
    _d, rampProfile = fillParams(session)
    raised = knotValue(rampProfile, r)
    check(raised is not None and raised > HALF,
          "dragging up raised the profile knot at r=%.2f above the half "
          "length (%r)" % (r, rampProfile))
    check(raised is not None and
          abs(raised - min(1.0, HALF + travel /
                           pomadeLoopsFill.RAMP_PIXELS)) < 0.05,
          "by the drag's travel over RAMP_PIXELS (%r)" % (raised,))
    rampLengths = guideLengths(session)
    check(mean(rampLengths) > mean(baseLengths) * 1.02,
          "the guide tips lengthened (mean %.4f -> %.4f)"
          % (mean(baseLengths), mean(rampLengths)))
    info("preview fraction during the drag %.3f (stored %.3f), %d live "
         "guides" % (duringFraction, storedFraction, liveGuides))
    check(pomadeT3.guideCount(session) == fullGuides,
          "the release refilled at FULL density (%d guides, want %d)"
          % (pomadeT3.guideCount(session), fullGuides))
    check(abs(float(session.dll.Pomade_GetPreviewFraction(session.model)) -
              storedFraction) < 1e-6,
          "and put the stored preview fraction back (%.3f)"
          % float(session.dll.Pomade_GetPreviewFraction(session.model)))
    check(groom.undoDepth() == depth + 1,
          "the drag is exactly one undo step (%d -> %d, %r)"
          % (depth, groom.undoDepth(), session.undoLabel(0)))
    check([entry[0] for entry in session.readSelection(TUBE)] == [0],
          "and the pressed tube is the selection")

    check(groom.commit(), "the idle pump commits the ramped groom")
    tubePrim = stage.GetPrimAtPath("/PomadeGroom/Tubes/tube0")
    committed = None
    if tubePrim:
        attr = tubePrim.GetAttribute("usdGen:pomade:fill:lengthProfile")
        committed = list(attr.Get()) if attr and attr.Get() is not None \
            else None
    check(committed is not None and knotValue(committed, r) is not None and
          knotValue(committed, r) > HALF,
          "/PomadeGroom/Tubes/tube0 commits the raised profile (%r)"
          % (committed,))

    typeKey(view, "z", ("ctrl",))
    wait(30)
    _d, undoneProfile = fillParams(session)
    undoneLengths = guideLengths(session)
    check(undoneProfile == baseProfile,
          "Ctrl+Z restores the old ramp (%r)" % (undoneProfile,))
    check(len(undoneLengths) == len(baseLengths) and
          abs(mean(undoneLengths) - mean(baseLengths)) < 1e-4,
          "and the old tip lengths (mean %.4f, want %.4f)"
          % (mean(undoneLengths), mean(baseLengths)))

    # -- a drag DOWN shortens (the other half of the contract) ------------
    depth = groom.undoDepth()
    mouse.press(press)
    for step in range(1, 7):
        mouse.move((press[0], press[1] + travel * step / 6.0))
        wait(5)
    mouse.release((press[0], press[1] + travel))
    wait(30)
    _d, lowProfile = fillParams(session)
    lowered = knotValue(lowProfile, r)
    lowLengths = guideLengths(session)
    check(lowered is not None and lowered < HALF and
          abs(lowered - max(0.0, HALF - travel /
                            pomadeLoopsFill.RAMP_PIXELS)) < 0.05,
          "dragging down lowered the knot at r=%.2f below the half length "
          "(%r)" % (r, lowProfile))
    check(mean(lowLengths) < mean(baseLengths) * 0.98,
          "and the guide tips shortened (mean %.4f -> %.4f)"
          % (mean(baseLengths), mean(lowLengths)))
    check(groom.undoDepth() == depth + 1,
          "as one undo step (%d -> %d)" % (depth, groom.undoDepth()))
    typeKey(view, "z", ("ctrl",))
    wait(30)
    _d, undoneProfile = fillParams(session)
    check(undoneProfile == baseProfile and
          abs(mean(guideLengths(session)) - mean(baseLengths)) < 1e-4,
          "Ctrl+Z restores the ramp and the tips (%r)" % (undoneProfile,))

    # -- Escape mid-drag cancels the ramp ----------------------------------
    depth = groom.undoDepth()
    mouse.press(press)
    mouse.move((press[0], press[1] - 60.0))
    wait(5)
    typeKey(view, "escape")
    wait(10)
    mouse.release((press[0], press[1] - 60.0))
    _d, cancelledProfile = fillParams(session)
    check(not session.gestureActive and cancelledProfile == baseProfile and
          groom.undoDepth() == depth,
          "Escape mid-drag drops the ramp edit with no undo step (%r)"
          % (cancelledProfile,))
    check(pomadeT3.guideCount(session) == fullGuides,
          "and the guides come back at full density (%d)"
          % pomadeT3.guideCount(session))

    typeKey(view, "p")
    groom.close()
    print("testUsdviewPomadeFill: %d failure(s)" % pomadeT3.failureCount())
    return 1 if pomadeT3.failureCount() else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view (the gesture proof needs a live usdview)")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewPomadeFill needs testusdview (no live view)")
    sys.exit(0)
