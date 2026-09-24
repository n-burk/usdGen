# testUsdviewTonicReattach -- T3 acceptance of SS-01: a stage reload keeps
# the live Tonic model.
#
#   testusdview --testScript \
#       plugin/usdGenTonicTools/testenv/testUsdviewTonicReattach.py \
#       examples/tonic-single-quad.usda
#
# File > Reopen replaces usdview's stage under the tool. The model is the
# artist's work and must survive that: reattach used to re-bind the scalp,
# and Tonic_BindScalp clears the graph, the region loops and the undo
# stack, so a reopen committed an empty groom. This script draws a region
# with real clicks, reopens the same file and requires the graph, the
# region stats, the undo stack and the committed /TonicGroom to come back
# unchanged. It then opens a scene with no /Scalp: the session must stay
# detached, say which scalp is missing, and keep the stale groom out of
# the new stage. Reopening the original file afterwards reattaches.
import ctypes
import os
import shutil
import sys
import tempfile

# A counter-clockwise triangle inside the quad's only face (the same one
# testUsdviewTonicCvRegions.py closes first).
TRIANGLE = ((-0.82, -0.56), (-0.22, -0.56), (-0.52, 0.34))

NO_SCALP_SCENE = """#usda 1.0

def Xform "Elsewhere"
{
    def Sphere "Ball"
    {
        double radius = 0.5
    }
}
"""


def testenvDir():
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except NameError:
        pass
    for i, arg in enumerate(list(sys.argv)):
        if arg == "--testScript" and i + 1 < len(sys.argv):
            return os.path.dirname(os.path.abspath(sys.argv[i + 1]))
        if arg.startswith("--testScript="):
            return os.path.dirname(os.path.abspath(arg.split("=", 1)[1]))
    return ""


def undoDepth(session):
    return int(session.dll.Tonic_GetUndoDepth(session.model))


def regionStats(session):
    regions = ctypes.c_int(0)
    uncovered = ctypes.c_int(0)
    intersected = ctypes.c_int(0)
    session.dll.Tonic_GetRegionStats(session.model, ctypes.byref(regions),
                                     ctypes.byref(uncovered),
                                     ctypes.byref(intersected))
    return (regions.value, uncovered.value, intersected.value)


def _valueKey(value):
    """A comparable stand-in for an authored value.

    Arrays compare by length and content; repr of a Vt array is exact,
    so the same commit of the same model gives the same key.
    """
    if value is None:
        return None
    return repr(value)


def groomSignature(stage, root="/TonicGroom"):
    """Every prim under the committed groom with its authored values.

    None when the stage has no groom there. Relationships are included
    because the committed groom points at its scalp.
    """
    from pxr import Usd
    prim = stage.GetPrimAtPath(root)
    if not prim:
        return None
    out = []
    for p in Usd.PrimRange(prim):
        entry = [str(p.GetPath()), str(p.GetTypeName())]
        for attr in p.GetAuthoredAttributes():
            entry.append((attr.GetName(), _valueKey(attr.Get())))
        for rel in p.GetAuthoredRelationships():
            entry.append((rel.GetName(),
                          tuple(str(t) for t in rel.GetTargets())))
        out.append(tuple(entry))
    return tuple(out)


def sessionLayerHasGroom(stage, liveId):
    """(live layer hosted, groom spec in the session layer itself)."""
    from pxr import Sdf
    sessionLayer = stage.GetSessionLayer()
    hosted = liveId in list(sessionLayer.subLayerPaths)
    spec = sessionLayer.GetPrimAtPath(Sdf.Path("/TonicGroom"))
    return hosted, bool(spec)


def run(appController):
    here = testenvDir()
    if here:
        sys.path.insert(0, here)
        sys.path.insert(0, os.path.normpath(os.path.join(here, "..",
                                                         "python")))
    try:
        import usdGenTonicTools
        from usdGenTonicTools import tonicCamera, tonicHud
        from tonicT3 import (EXPIRED_CALLBACK, Mouse, StderrCapture, check,
                             failureCount, frameScalp, info, openAndBind,
                             pumpUntilCommitted, statusRecorder, typeKey,
                             wait)
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools/tonicT3: %s" % exc)
        return 1

    dataModel = appController._dataModel
    stage = dataModel.stage
    view = appController._stageView
    container = usdGenTonicTools.container()
    originalFile = appController._parserData.usdFile
    tempDir = tempfile.mkdtemp(prefix="tonicReattach")
    # Every Tf diagnostic of the bind -> edit -> reopen (rebind) -> close
    # cycle, to pin that no Tf.Notice listener outlives its Python callable
    # ("Tried to call an expired python callback" once per commit before).
    tfStderr = StderrCapture()

    def shutdown():
        viewport = getattr(container, "viewport", None)
        session = getattr(container, "session", None)
        try:
            if viewport is not None:
                viewport.uninstall()
        finally:
            if session is not None:
                session.deactivate()
            appController._parserData.usdFile = originalFile
            shutil.rmtree(tempDir, ignore_errors=True)
            tfStderr.stop()
            expired = tfStderr.count(EXPIRED_CALLBACK)
            check(expired == 0,
                  "the bind-edit-reopen-close cycle prints no Tf "
                  "expired-callback warning (%d)" % expired)

    def reopen(path):
        """File > Open's own path: point usdview at `path` and reopen."""
        appController._parserData.usdFile = path
        appController._reopenStage()
        wait(50)
        return dataModel.stage

    check(frameScalp(stage, view, eye=(0.0, 7.0, 0.0)),
          "the top camera is active")
    view.setFocus()
    wait(30)

    tfStderr.__enter__()
    session, viewport, state, workspace, _c = openAndBind(appController,
                                                          "/Scalp")
    check(session is not None and session.model is not None and
          viewport is not None and viewport.installed,
          "binding /Scalp creates the live model and installs the viewport")
    if session is None or session.model is None or viewport is None:
        shutdown()
        return 1
    messages = statusRecorder()
    session.setStatusSink(messages)

    # Draw one region the artist's way: Graph/Region clicks, Enter closes.
    view.setFocus()
    wait(10)
    typeKey(view, "r")
    camera = tonicCamera.resolve(view)
    check(camera is not None, "the viewport controller camera resolves")
    if camera is None:
        shutdown()
        return 1
    state.snapRadiusPx = max(
        0.05 / max(camera.worldPerPixel((0.0, 0.0, 0.0)), 1e-9), 2.0)
    mouse = Mouse(view)
    viewport.setPointerInside(True)
    mouse.direct = True
    for x, z in TRIANGLE:
        mouse.click(camera.worldToPixels((x, 0.0, z)))
    typeKey(view, "return")
    check(session.graphCounts()[2] == 1,
          "three clicks and Enter close one region (%r)"
          % (session.graphCounts(),))
    check(pumpUntilCommitted(viewport, session),
          "the region commits to the stage")
    # Bind scanned the stage for a saved groom and cached the answer; the
    # commit's ObjectsChanged must reach the container's live listener and
    # drop that cache (a dead listener left it stale forever).
    check(getattr(container, "_groomScan", None) is None,
          "the commit's stage notice invalidated the saved-groom scan")

    counts = session.graphCounts()
    stats = regionStats(session)
    depth = undoDepth(session)
    groom = groomSignature(stage)
    liveId = session.liveLayerId
    info("before reload: counts %r stats %r undo %d groom prims %d"
         % (counts, stats, depth, len(groom) if groom else 0))
    check(depth > 0, "the region is on the undo stack (%d)" % depth)
    check(groom is not None and len(groom) > 1,
          "the committed /TonicGroom holds the region's groom")

    # -- reopen the same file ---------------------------------------------
    model = session.model
    newStage = reopen(originalFile)
    check(newStage is not None and newStage is not stage,
          "File > Reopen replaced the stage")
    stage = newStage
    check(pumpUntilCommitted(viewport, session),
          "the reattached committer drains its re-commit")
    status = session.status()
    check(session.model is model, "the live model survives the reopen")
    check(not status["detached"] and not status["scalpMissing"],
          "the session is attached to the reopened stage (%r, %r)"
          % (status["detached"], status["scalpMissing"]))
    check(session.graphCounts() == counts,
          "graph counts are unchanged by the reopen (%r vs %r)"
          % (session.graphCounts(), counts))
    check(regionStats(session) == stats,
          "region stats are unchanged by the reopen (%r vs %r)"
          % (regionStats(session), stats))
    check(undoDepth(session) == depth,
          "the undo stack survives the reopen (%d vs %d)"
          % (undoDepth(session), depth))
    # Reattach starts a new live-layer lineage at committed version 0, so a
    # non-zero version here is the re-commit itself landing. (The model
    # version may sit ahead of it: the viewport's display-scale refresh on
    # the new stage bumps it without changing the groom.)
    check(session.committedVersion > 0 and not session.hasPendingWork(),
          "the reattached committer re-committed the model (%d/%d)"
          % (session.committedVersion, session.modelVersion))
    hosted, _spec = sessionLayerHasGroom(stage, liveId)
    check(hosted, "the live layer is re-hosted in the new session layer")
    after = groomSignature(stage)
    check(after == groom,
          "the committed /TonicGroom is unchanged by the reopen (%d vs %d "
          "prims)" % (len(after) if after else 0, len(groom) if groom else 0))
    check(not any("hydrat" in m.lower() for m in messages),
          "the reopen did not rebuild the model")

    # The undo stack is live, not merely the same depth: one Ctrl+Z takes
    # the region away and Ctrl+Y brings it back.
    view.setFocus()
    wait(10)
    typeKey(view, "z", ("ctrl",))
    check(session.graphCounts()[2] == 0,
          "Ctrl+Z after the reopen undoes the pre-reopen region (%r)"
          % (session.graphCounts(),))
    typeKey(view, "y", ("ctrl",))
    check(session.graphCounts() == counts,
          "Ctrl+Y restores it (%r)" % (session.graphCounts(),))
    pumpUntilCommitted(viewport, session)

    # -- open a scene with no /Scalp --------------------------------------
    noScalp = os.path.join(tempDir, "noScalp.usda")
    with open(noScalp, "w") as handle:
        handle.write(NO_SCALP_SCENE)
    del messages[:]
    stage = reopen(noScalp)
    check(stage is not None and not stage.GetPrimAtPath("/Scalp"),
          "the replacement scene has no /Scalp")
    viewport.pumpOnce()
    wait(25)
    status = session.status()
    check(status["detached"],
          "the session stays detached without its scalp")
    check(status["scalpMissing"] == "/Scalp",
          "status() names the missing scalp (%r)" % status["scalpMissing"])
    check(session.model is model and session.graphCounts() == counts,
          "the model is kept, not re-bound, while the scalp is missing")
    hosted, spec = sessionLayerHasGroom(stage, liveId)
    check(not hosted and not spec and
          not stage.GetPrimAtPath("/TonicGroom"),
          "no /TonicGroom reaches the scalp-less stage (hosted=%r spec=%r)"
          % (hosted, spec))
    check(any("Scalp /Scalp not found in the new stage" in m
              for m in messages),
          "the status line says the scalp is missing (%r)" % list(messages))
    rows = tonicHud.warnings(state, session)
    check(any(r.severity == "error" and "/Scalp" in r.text and
              "not found" in r.text for r in rows),
          "the warnings list carries a missing-scalp error row (%r)"
          % [r.text for r in rows])
    if workspace is not None:
        workspace.refresh()

    # -- back to the file with the scalp ----------------------------------
    stage = reopen(originalFile)
    check(pumpUntilCommitted(viewport, session),
          "reopening the scalp's file drains the re-commit")
    status = session.status()
    check(not status["detached"] and not status["scalpMissing"],
          "reopening a stage with /Scalp reattaches")
    check(session.graphCounts() == counts and
          groomSignature(stage) == groom,
          "the groom comes back unchanged after the detour")
    rows = tonicHud.warnings(state, session)
    check(not any("not found" in r.text for r in rows),
          "the missing-scalp row clears on reattach")

    # -- an uninstall/install cycle must not stack the stage-swap slot -----
    # Qt keeps every connect(); install() used to connect
    # signalStageReplaced again with nothing ever disconnecting it, so a
    # re-installed controller ran detach/reattach twice per reopen.
    detaches = []
    realDetach = session.detach

    def countingDetach(*args, **kwargs):
        detaches.append(1)
        return realDetach(*args, **kwargs)

    session.detach = countingDetach
    try:
        # usdview's reopen replaces the stage more than once (close, then
        # open), so the baseline is measured, not assumed.
        stage = reopen(originalFile)
        pumpUntilCommitted(viewport, session)
        baseline = len(detaches)
        check(baseline >= 1, "a reopen runs the stage swap (%d)" % baseline)
        del detaches[:]
        for _cycle in range(2):
            viewport.uninstall()
            check(viewport.install() and viewport.installed and
                  not viewport.suspended,
                  "the controller re-installs over the open workspace")
        stage = reopen(originalFile)
        check(pumpUntilCommitted(viewport, session),
              "the reopen after the re-installs drains its re-commit")
        check(len(detaches) == baseline,
              "after two uninstall/install cycles a reopen runs the stage "
              "swap as often as before (%d vs %d)"
              % (len(detaches), baseline))
        check(session.graphCounts() == counts,
              "and the groom is still the one drawn (%r)"
              % (session.graphCounts(),))
    finally:
        del session.detach

    shutdown()
    return 1 if failureCount() else 0


def testUsdviewInputFunction(appController):
    if getattr(appController, "_stageView", None) is None:
        print("FAIL: no stage view")
        return 1
    return run(appController)


if __name__ == "__main__":
    print("SKIP: testUsdviewTonicReattach needs testusdview")
    sys.exit(0)
