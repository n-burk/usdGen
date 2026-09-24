#!/usr/bin/env python3
# testUsdGenTonicToolsStage -- T1: the ctypes binding of tonicApiStage.h
# (plan/18 V0b, tonicLibStage.py).
#
# Part A drives the per-tube operations, the per-tube fill params and the
# hierarchy reads over the real DLL. Part B commits a small hierarchy into a
# live layer and hydrates it back through the binding, which is the whole
# point of the stage contract. Both are strict under CTest (USDGENTONIC_DLL
# is set) and a skip for direct runs without the DLL or pxr.
#
# The modules load through a synthetic package so `from . import tonicLib`
# resolves without executing usdGenTonicTools/__init__.py (which imports
# pxr.Usdviewq and therefore Qt).
import ctypes
import importlib.util
import os
import sys
import time
import types

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenTonicTools"))

_PKG = "tonic_stage_pkg"
_pkg = types.ModuleType(_PKG)
_pkg.__path__ = [SRC]
sys.modules[_PKG] = _pkg


def _load(name):
    full = "%s.%s" % (_PKG, name)
    spec = importlib.util.spec_from_file_location(
        full, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    sys.modules[full] = module
    spec.loader.exec_module(module)
    setattr(_pkg, name, module)
    return module


# The repo root, for the example scenes Part C hydrates.
ROOT = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))

failures = 0
skips = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def skip(what):
    global skips
    skips += 1
    print("SKIP: %s" % what)


def _bindExtras(dll):
    cvp = ctypes.c_void_p
    cip = ctypes.POINTER(ctypes.c_int)
    ccp = ctypes.c_char_p
    dll.Tonic_SubdivideTube.argtypes = [cvp, ctypes.c_int, ctypes.c_int, ccp,
                                        ctypes.c_int, cip, ctypes.c_int, cip]
    dll.Tonic_SubdivideTube.restype = ctypes.c_int
    dll.Tonic_MergeChildren.argtypes = [cvp, ctypes.c_int]
    dll.Tonic_MergeChildren.restype = ctypes.c_int
    dll.Tonic_GetTubeLevel.argtypes = [cvp, ctypes.c_int]
    dll.Tonic_GetTubeLevel.restype = ctypes.c_int
    dll.Tonic_GetTubeCenterCount.argtypes = [cvp, ctypes.c_int]
    dll.Tonic_GetTubeCenterCount.restype = ctypes.c_int
    dll.Tonic_GetTubeCenterCV.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                          ctypes.POINTER(ctypes.c_float)]
    dll.Tonic_GetTubeCenterCV.restype = ctypes.c_int
    dll.Tonic_SetLevelDisplay.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                          ctypes.c_int]
    dll.Tonic_SetLevelDisplay.restype = ctypes.c_int
    dll.Tonic_CommitterCreate.argtypes = [cvp, ccp, ccp,
                                          ctypes.POINTER(ctypes.c_void_p)]
    dll.Tonic_CommitterCreate.restype = ctypes.c_int
    dll.Tonic_CommitterEnqueue.argtypes = [cvp, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, ccp]
    dll.Tonic_CommitterEnqueue.restype = ctypes.c_int
    dll.Tonic_CommitterSwap.argtypes = [cvp, ccp, ctypes.c_int]
    dll.Tonic_CommitterSwap.restype = ctypes.c_int
    dll.Tonic_CommitterCommittedVersion.argtypes = [cvp]
    dll.Tonic_CommitterCommittedVersion.restype = ctypes.c_ulonglong
    dll.Tonic_CommitterDestroy.argtypes = [cvp]
    dll.Tonic_CommitterDestroy.restype = ctypes.c_int


def main():
    strict = bool(os.environ.get("USDGENTONIC_DLL", ""))
    tonicLib = _load("tonicLib")
    tonicLibStage = _load("tonicLibStage")
    try:
        lib = tonicLib.Library()
    except OSError as error:
        if strict:
            raise
        skip("no usdGenTonic DLL (%s)" % error)
        return 77
    stage = tonicLibStage.StageLibrary(lib)
    dll = lib.dll
    _bindExtras(dll)

    ctx = ctypes.c_void_p()
    check(dll.Tonic_Create(ctypes.byref(ctx)) == tonicLib.TONIC_OK,
          "stage: the model creates")
    check(dll.Tonic_BuildTestTube(ctx, 0, 0, 0.0, 0.0) == tonicLib.TONIC_OK,
          "stage: the test tube builds")
    ids = (ctypes.c_int * 8)()
    got = ctypes.c_int(0)
    check(dll.Tonic_SubdivideTube(ctx, 0, 4, b"kmeans", 2, ids, 8,
                                  ctypes.byref(got)) == tonicLib.TONIC_OK and
          got.value == 4,
          "stage: the tube subdivides into four")
    child = ids[0]

    # Per-tube operations over the binding.
    stage.moveSectionRing(ctx, child, 1, 0.04, -0.01)
    stage.scaleSectionRing(ctx, child, 1, 1.2)
    stage.twistSectionRing(ctx, child, 1, 0.05)
    stage.moveSectionCV(ctx, child, 1, 0, 0.01, 0.0)
    stage.copySectionRing(ctx, child, 0, 1)
    stage.relaxCenter(ctx, child, 0.5, 1)
    stage.setLength(ctx, child, 3.25)
    check(True, "stage: every per-tube operation runs over the binding")
    try:
        stage.addSectionRing(ctx, child, 0.5)
        check(False, "stage: a layout change on a derived child is refused")
    except RuntimeError as error:
        check("derived child" in str(error),
              "stage: a layout change on a derived child is refused (%s)"
              % str(error).split(": ")[-1])

    stage.setFillParams(ctx, child, 9.0, 6, 3, 0.1, (0.0, 1.0, 1.0, 0.5))
    params = stage.fillParams(ctx, child)
    check(params["density"] == 9.0 and params["cvCount"] == 6 and
          params["seed"] == 3 and params["profileCount"] == 4,
          "stage: per-tube fill params round-trip")
    check(stage.isFillSuspended(ctx, 0) and
          not stage.isFillSuspended(ctx, child),
          "stage: the subdivided parent's fill is suspended, the leaf's is not")
    check(stage.tubeParent(ctx, child) == (0, 0),
          "stage: the parent link reads back")

    # V4: where a ring sits, and the world <-> chart conversion the Tube
    # loop's gizmo drags through. The round trip is the real assertion --
    # convert a world delta, push it through the per-tube ABI, and the
    # ring's centroid must have moved by exactly that delta.
    ring = stage.sectionFrame(ctx, 0, 1)
    check(ring is not None, "stage: a section ring reports its frame")
    if ring is not None:
        axes = (ring["u"], ring["v"], ring["w"])
        unit = all(abs(sum(a[i] * a[i] for i in range(3)) - 1.0) < 1e-4
                   for a in axes)
        ortho = all(abs(sum(axes[p][i] * axes[q][i] for i in range(3)))
                    < 1e-4 for p, q in ((0, 1), (1, 2), (0, 2)))
        check(unit and ortho,
              "stage: its u, v, w axes are orthonormal (%r)" % (axes,))
        want = tuple(0.1 * ring["u"][i] for i in range(3))
        du, dv = stage.worldToChart(ring, want)
        stage.moveSectionRing(ctx, 0, 1, du, dv)
        moved = stage.sectionFrame(ctx, 0, 1)
        got = tuple(moved["origin"][i] - ring["origin"][i]
                    for i in range(3))
        check(all(abs(got[i] - want[i]) < 1e-5 for i in range(3)),
              "stage: a world delta converted to (du, dv) moves the ring "
              "by exactly that delta (%r vs %r)" % (got, want))
        stage.moveSectionRing(ctx, 0, 1, -du, -dv)
        check(stage.sectionFrame(ctx, 0, 1) is not None and
              abs(stage.sectionFrame(ctx, 0, 1)["origin"][0] -
                  ring["origin"][0]) < 1e-5,
              "stage: and the inverse delta puts it back")
        check(stage.sectionFrame(ctx, 0, 99) is None and
              stage.sectionFrame(ctx, 4242, 0) is None,
              "stage: a missing ring or tube answers None, not an error")
    check(not stage.isPersistent(ctx, child),
          "stage: a subdivided child is not an on-the-fly parent")

    # V5: the three entries the viewport loops added (plan/18 section 7
    # G13 and audit section 5.4 row 267), over the real DLL.
    #
    # The draw mode first, because it is the simplest: a level that draws
    # its centers and nothing else is the ladder's fourth rung, and
    # Tonic_SetLevelDisplay's visible/x-ray pair cannot say it.
    check(tonicLibStage.setLevelDrawMode(dll, ctx, 2, True, False, True),
          "stage: a level takes the centers-only draw mode")
    check(tonicLibStage.levelDrawMode(dll, ctx, 2) == (True, False, True),
          "stage: and reads it back (%r)"
          % (tonicLibStage.levelDrawMode(dll, ctx, 2),))
    check(dll.Tonic_SetLevelDisplay(ctx, 2, 1, 1) == tonicLib.TONIC_OK and
          tonicLibStage.levelDrawMode(dll, ctx, 2) == (True, True, True),
          "stage: the two-argument form leaves centers-only alone")
    tonicLibStage.setLevelDrawMode(dll, ctx, 2, True, False, False)

    # The shaped sculpt stroke. The projection is the identity, so a CV at
    # world (x, y) sits at pixel ((x + 1) * 500, (1 - y) * 500) and the
    # cursor below is aimed by hand.
    viewProj = (ctypes.c_float * 16)(1.0, 0.0, 0.0, 0.0,
                                     0.0, 1.0, 0.0, 0.0,
                                     0.0, 0.0, 1.0, 0.0,
                                     0.0, 0.0, 0.0, 1.0)

    class _Cam:
        width = 1000
        height = 1000

        @staticmethod
        def viewProjArray():
            return viewProj

    def _centers(tubeId):
        n = int(dll.Tonic_GetTubeCenterCount(ctx, tubeId))
        out = []
        for cv in range(max(n, 0)):
            xyz = (ctypes.c_float * 3)()
            dll.Tonic_GetTubeCenterCV(ctx, tubeId, cv, xyz)
            out.append((float(xyz[0]), float(xyz[1]), float(xyz[2])))
        return out

    def _length(points):
        total = 0.0
        for i in range(1, len(points)):
            a, b = points[i - 1], points[i]
            total += sum((b[k] - a[k]) ** 2 for k in range(3)) ** 0.5
        return total

    before = _centers(child)
    check(len(before) >= 3, "stage: the child has a center curve (%d CVs)"
          % len(before))
    mid = before[len(before) // 2]
    cursor = ((mid[0] * 0.5 + 0.5) * 1000.0, (1.0 - (mid[1] * 0.5 + 0.5))
              * 1000.0)
    touched = tonicLibStage.sculptStrokeShaped(
        dll, ctx, child, "grab", _Cam, cursor[0], cursor[1], 400.0,
        (0.05, 0.0, 0.0), 0.0, 0.5, 0.0, True, False)
    after = _centers(child)
    check(touched > 0, "stage: a shaped grab stroke moves CVs (%d)" % touched)
    moved = max(abs(after[i][k] - before[i][k])
                for i in range(len(after)) for k in range(3))
    check(moved > 1e-5, "stage: and the model shows it (max %.6f)" % moved)
    relative = abs(_length(after) - _length(before)) / max(_length(before),
                                                           1e-9)
    check(relative <= 1e-4,
          "stage: length-preserving in C++, not in the caller (%.2e)"
          % relative)
    check(all(abs(after[0][k] - before[0][k]) < 1e-6 for k in range(3)),
          "stage: with the root pinned (%r -> %r)" % (before[0], after[0]))
    # Twist takes no delta at all, only the angle: the shaping is the
    # model's now, so a brush with no direction still does something.
    turned = tonicLibStage.sculptStrokeShaped(
        dll, ctx, child, "twist", _Cam, cursor[0], cursor[1], 400.0,
        (0.0, 0.0, 0.0), 0.3, 0.5, 0.0, True, False)
    check(turned > 0, "stage: twist shapes a stroke from its angle alone "
                      "(%d CVs)" % turned)
    quiet = tonicLibStage.sculptStrokeShaped(
        dll, ctx, child, "grab", _Cam, -5000.0, -5000.0, 10.0,
        (0.05, 0.0, 0.0), 0.0, 0.5, 0.0, True, False)
    check(quiet == 0, "stage: a brush over nothing touches nothing (%d)"
          % quiet)
    check(tonicLibStage.sculptStrokeShaped(
        dll, ctx, child, "grab", _Cam, cursor[0], cursor[1], 400.0,
        (0.02, 0.0, 0.0), 0.0, 0.5, 0.0, True, True) > 0,
        "stage: and mirror-X is the model's business, not the caller's")

    # The drawn-edge subdivide: two WORLD points across the child's root
    # region become the split line the audit found hard-coded to (1, 0, 0).
    root = _centers(child)[0]
    kids = tonicLibStage.subdivideTubeEdge(
        dll, ctx, child, (root[0] - 1.0, root[1], root[2]),
        (root[0] + 1.0, root[1], root[2]), 0)
    check(len(kids) == 2, "stage: an edge split makes two children (%r)"
          % (kids,))
    check(all(int(dll.Tonic_GetTubeLevel(ctx, k)) ==
              int(dll.Tonic_GetTubeLevel(ctx, child)) + 1 for k in kids),
          "stage: one level below their parent")
    try:
        tonicLibStage.subdivideTubeEdge(dll, ctx, child, (0.0, 0.0, 0.0),
                                        (0.0, 0.0, 0.0), 0)
        check(False, "stage: a degenerate edge is refused")
    except RuntimeError:
        check(True, "stage: a degenerate edge is refused")
    check(dll.Tonic_MergeChildren(ctx, child) == tonicLib.TONIC_OK,
          "stage: and the edge children merge back")

    # Part B: commit and hydrate through the binding.
    try:
        from pxr import Sdf, Usd
    except ImportError:
        if strict:
            raise
        skip("no pxr: the commit/hydrate half needs a USD python")
        dll.Tonic_Destroy(ctx)
        return 77
    usdStage = Usd.Stage.CreateInMemory("tonicStageBinding")
    usdStage.DefinePrim("/Groom/Hair", "UsdGenDescription")
    usdStage.DefinePrim("/Groom/Hair/Ops", "Scope")
    live = Sdf.Layer.CreateAnonymous("tonic-stage-live")
    usdStage.GetRootLayer().subLayerPaths.insert(0, live.identifier)
    committer = ctypes.c_void_p()
    check(dll.Tonic_CommitterCreate(ctx, b"/TonicGroom", b"/Groom/Hair",
                                    ctypes.byref(committer)) ==
          tonicLib.TONIC_OK,
          "stage: the committer creates")
    check(dll.Tonic_CommitterEnqueue(committer, 1, 1, 1,
                                     b"/Groom/Hair/Ops/tonicInterp") ==
          tonicLib.TONIC_OK,
          "stage: the commit enqueues")
    want = dll.Tonic_GetVersion(ctx)
    committed = 0
    # The worker thread needs a few ms when idle and far longer under
    # load, so wait on a wall-clock deadline rather than a spin count.
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        dll.Tonic_CommitterSwap(committer, live.identifier.encode("utf-8"), 0)
        committed = dll.Tonic_CommitterCommittedVersion(committer)
        if committed >= want:
            break
        time.sleep(0.001)
    check(committed >= want, "stage: the commit swaps into the live layer")

    fresh = ctypes.c_void_p()
    dll.Tonic_Create(ctypes.byref(fresh))
    tubes, guides, imported = stage.hydrate(fresh, live.identifier,
                                            "/TonicGroom")
    check(tubes == 5 and guides > 0 and imported == 0,
          "stage: hydrate rebuilds the hierarchy over the binding "
          "(%d tubes, %d guides)" % (tubes, guides))
    check(stage.tubeParent(fresh, child) == (0, 0),
          "stage: the hydrated child keeps its parent link")
    rehydrated = stage.fillParams(fresh, child)
    check(rehydrated == params,
          "stage: the hydrated child keeps its fill params")
    try:
        stage.hydrate(fresh, live.identifier, "/NoSuchGroom")
        check(False, "stage: hydrating a missing groom fails")
    except RuntimeError as error:
        check("UsdGenTonicGroom" in str(error),
              "stage: hydrating a missing groom fails honestly")

    dll.Tonic_Destroy(fresh)
    dll.Tonic_CommitterDestroy(committer)
    dll.Tonic_Destroy(ctx)

    partC()
    partD()
    partE()
    partF()
    partG()
    return 0


def partC():
    """V9: TonicSession.hydrate keeps the scalp graph the groom carries.

    Tonic_Hydrate binds the scalp AND restores the scalp graph, its region
    loops and the per-face region ids. TonicSession used to follow it with
    a second bindScalpFromStage, and TonicModel::BindScalp clears the
    graph on every call -- so a hydrated groom came up with no regions at
    all and the scene index painted every scalp face the "uncovered" dark
    red instead of the clump colours of plan/18 section 2.4a.

    This is the regression guard: hydrate the committed braid and assert
    the graph and its regions are still there, and that rasterising them
    covers scalp faces.
    """
    from pxr import Usd
    scene = os.path.join(ROOT, "examples", "tonic-braid-hierarchy.usda")
    if not os.path.exists(scene):
        skip("no %s" % scene)
        return
    tonicSession = _load("tonicSession")
    tonicToolState = _load("tonicToolState")
    stage = Usd.Stage.Open(scene)
    state = tonicToolState.TonicToolState()
    session = tonicSession.TonicSession(state)
    if not session.hydrate(groomPath="/TonicGroom", stage=stage):
        check(False, "session: the braid hydrates")
        return
    nodes, edges, regions = session.graphCounts()
    check(regions == 2 and nodes > 0 and edges > 0,
          "session: hydrate keeps the committed scalp graph "
          "(%d node(s), %d edge(s), %d region(s))" % (nodes, edges, regions))
    check(session.scalpPath == "/World/Scalp",
          "session: and still records the scalp path (%r)"
          % session.scalpPath)
    idleWakes = []
    session.setIdleHook(lambda: idleWakes.append("commit"))
    check(session.enqueueCommit() and idleWakes == ["commit"],
          "session: a queued commit wakes the owning idle pump")
    session.setIdleHook(None)
    session.dll.Tonic_Rasterise(session.model)
    total, uncovered, intersected = session.regionStats()
    check(total > 0 and uncovered < 16,
          "session: the restored regions cover scalp faces "
          "(%d region(s), %d uncovered, %d doubly claimed)"
          % (total, uncovered, intersected))
    groom = ctypes.create_string_buffer(256)
    if session.dll.Tonic_GetGroomPath(session.model, groom, 256) == 0:
        check(groom.value.decode("utf-8") == "/TonicGroom",
              "session: and hands the model the groom path, so the index "
              "hides the committed Guides (%r)"
              % groom.value.decode("utf-8"))
    else:
        check(False, "session: Tonic_GetGroomPath failed")
    session.deactivate()


def partD():
    """SS-02: undo hygiene through a real TonicSession and the real DLL.

    An empty stack answers 'Nothing to undo' at warning level and touches
    neither the viewport nor the committer; a labelled step shows up in
    status(); undo puts a Fill density back together with the guides it
    grew; endGestureIfChanged(False) leaves no step behind.
    """
    from pxr import Usd, UsdGeom
    tonicLib = sys.modules.get(_PKG + ".tonicLib") or _load("tonicLib")
    tonicSession = sys.modules.get(_PKG + ".tonicSession") or \
        _load("tonicSession")
    tonicToolState = sys.modules.get(_PKG + ".tonicToolState") or \
        _load("tonicToolState")
    stage = Usd.Stage.CreateInMemory("tonicUndoHygiene")
    mesh = UsdGeom.Mesh.Define(stage, "/Scalp")
    # A 2x2-quad plane over [0, 4] in XZ, facing +Y.
    points = [(float(x), 0.0, float(z)) for z in (0, 2, 4) for x in (0, 2, 4)]
    indices = []
    for row in range(2):
        for col in range(2):
            a = row * 3 + col
            indices += [a, a + 3, a + 4, a + 1]
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr([4] * 4)
    mesh.CreateFaceVertexIndicesAttr(indices)

    state = tonicToolState.TonicToolState()
    session = tonicSession.TonicSession(state)
    heard = []
    session.setStatusSink(lambda text, level="info": heard.append(
        (text, level)))
    if not session.activate("/Scalp", stage=stage):
        check(False, "undo: a session binds the plane scalp (%r)" % heard)
        return
    dll = session.dll
    model = session.model

    session.enqueueCommit()
    pending = session.pendingVersion
    published = []
    session.setPublishHook(lambda: published.append(1))
    del heard[:]
    check(session.undo() is False and not published,
          "undo: a fresh model's undo() answers False and publishes nothing")
    check(("Nothing to undo", "warning") in heard,
          "undo: and the sink hears ('Nothing to undo', 'warning') (%r)"
          % heard)
    check(session.pendingVersion == pending,
          "undo: with nothing enqueued (pending %d -> %d)"
          % (pending, session.pendingVersion))
    check(session.redo() is False and
          ("Nothing to redo", "warning") in heard and not published,
          "undo: redo on an empty stack says so too (%r)" % heard)
    session.setPublishHook(None)
    textOnly = []
    session.setStatusSink(textOnly.append)
    session.undo()
    check(textOnly == ["Nothing to undo"],
          "undo: a one-argument sink still gets the text alone (%r)"
          % textOnly)
    session.setStatusSink(lambda text, level="info": heard.append(
        (text, level)))

    def guides():
        count = ctypes.c_int(0)
        dll.Tonic_GetGuideCounts(model, ctypes.byref(count), None)
        return int(count.value)

    def density():
        value = ctypes.c_float(0.0)
        cvs = ctypes.c_int(0)
        seed = ctypes.c_int(0)
        bias = ctypes.c_float(0.0)
        got = ctypes.c_int(0)
        profile = (ctypes.c_float * 64)()
        dll.Tonic_GetFillParams(model, ctypes.byref(value), ctypes.byref(cvs),
                                ctypes.byref(seed), ctypes.byref(bias),
                                profile, 64, ctypes.byref(got))
        return float(value.value)

    check(dll.Tonic_BuildTestTube(model, 0, 0, 0.0, 0.0) ==
          tonicLib.TONIC_OK, "undo: the test tube builds over the scalp")
    dll.Tonic_SetFillParams(model, 20.0, 8, 0, 0.0, None, 0)
    dll.Tonic_RefillGuides(model, ctypes.c_float(1.0))
    sparse = guides()
    sparseDensity = density()
    check(sparse > 0, "undo: the tube grows guides (%d)" % sparse)

    check(session.beginGesture("Density test"),
          "undo: a labelled gesture opens")
    dll.Tonic_SetFillParams(model, 80.0, 8, 0, 0.0, None, 0)
    dll.Tonic_RefillGuides(model, ctypes.c_float(1.0))
    session.endGesture()
    dense = guides()
    status = session.status()
    check(status["undoLabel"] == "Density test" and
          status["undoDepth"] >= 1 and status["redoDepth"] == 0 and
          status["redoLabel"] == "",
          "undo: status() names the step Ctrl+Z would take (%r, depth %d)"
          % (status["undoLabel"], status["undoDepth"]))
    check(dense > sparse, "undo: the denser fill grew more guides (%d -> %d)"
          % (sparse, dense))

    del heard[:]
    check(session.undo() is True, "undo: the density step undoes")
    check(abs(density() - sparseDensity) < 1e-4,
          "undo: Ctrl+Z puts the Fill density back (%g, want %g)"
          % (density(), sparseDensity))
    check(guides() == sparse,
          "undo: and the guides it grew, not an empty set (%d, want %d)"
          % (guides(), sparse))
    check(("Undo: Density test", "info") in heard,
          "undo: the status line names the undone step (%r)" % heard)
    status = session.status()
    check(status["redoLabel"] == "Density test" and status["redoDepth"] == 1,
          "undo: and status() offers it for redo (%r)" % status["redoLabel"])
    check(session.redo() is True and abs(density() - 80.0) < 1e-4 and
          guides() == dense,
          "undo: redo re-applies the density and its guides (%g, %d)"
          % (density(), guides()))

    # endGestureIfChanged(False) must route through Tonic_CancelGesture:
    # End would keep the step Begin pushed, and Ctrl+Z would then spend a
    # keypress on a gesture that changed nothing.
    original = dll.Tonic_CancelGesture
    cancelled = []

    def recordCancel(*args):
        cancelled.append(args)
        return original(*args)

    depth = session.undoDepth()
    dll.Tonic_CancelGesture = recordCancel
    try:
        session.beginGesture("No-op click")
        kept = session.endGestureIfChanged(False)
    finally:
        dll.Tonic_CancelGesture = original
    check(len(cancelled) == 1 and kept is False,
          "undo: endGestureIfChanged(False) calls Tonic_CancelGesture")
    check(session.undoDepth() == depth and not session.gestureActive and
          session.status()["undoLabel"] == "Density test",
          "undo: and leaves no empty step (depth %d, want %d)"
          % (session.undoDepth(), depth))

    # Ctrl+Z / Ctrl+Y with a session gesture open (a dock slider drag)
    # must refuse and say so: popping Begin's step from under the open
    # bracket lost the press-time state, and Cancel then popped an
    # unrelated older step.
    check(session.beginGesture("Slider drag"),
          "undo: a slider-style session gesture opens")
    dll.Tonic_SetFillParams(model, 50.0, 8, 0, 0.0, None, 0)
    del heard[:]
    check(session.undo() is False and session.gestureActive and
          session.undoDepth() == depth + 1,
          "undo: Ctrl+Z mid-gesture refuses and keeps Begin's step "
          "(depth %d, want %d)" % (session.undoDepth(), depth + 1))
    check(any("finish the drag" in text for text, _level in heard),
          "undo: and says 'finish the drag' (%r)" % heard)
    check(dll.Tonic_Undo(model, None) != tonicLib.TONIC_OK and
          session.undoDepth() == depth + 1,
          "undo: Tonic_Undo itself refuses inside the bracket")
    check(session.redo() is False,
          "undo: redo mid-gesture refuses the same way")
    check(session.cancelGesture() != 0 and
          not session.gestureActive and session.undoDepth() == depth and
          session.status()["undoLabel"] == "Density test",
          "undo: Cancel after the refused Ctrl+Z drops only its own step "
          "(depth %d, want %d, label %r)" % (
              session.undoDepth(), depth,
              session.status()["undoLabel"]))
    check(abs(density() - 80.0) < 1e-4,
          "undo: and restores the press-time density (%g)" % density())
    session.beginGesture("Real edit")
    check(session.endGestureIfChanged(True) is True and
          session.undoDepth() == depth + 1,
          "undo: endGestureIfChanged(True) keeps the step")
    session.deactivate()


def partE():
    """SS-03: a saved groom survives a reopen and hydrates back whole.

    saveGroom(addToRootLayer=True) puts the .usdc in the scene's root
    layer (relative, saved to disk), so a stage reopened from the file
    composes it and hydrate() brings the same tubes, guides and imports
    back. Two grooms saved into one directory keep their own region maps;
    a bare name gets .usdc; the save waits for the committer only, gives
    up on time and says so, and refuses while detached.
    """
    import shutil
    import tempfile
    folder = tempfile.mkdtemp(prefix="tonicSaveGroom")
    try:
        _partE(folder)
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def _partE(folder):
    from pxr import Sdf, Usd, UsdGeom
    tonicLib = sys.modules.get(_PKG + ".tonicLib") or _load("tonicLib")
    tonicSession = sys.modules.get(_PKG + ".tonicSession") or \
        _load("tonicSession")
    tonicToolState = sys.modules.get(_PKG + ".tonicToolState") or \
        _load("tonicToolState")
    tonicBridge = sys.modules.get(_PKG + ".tonicBridge") or \
        _load("tonicBridge")

    def writeScalpScene(path):
        # A 2x2-quad plane over [0, 4] in XZ, facing +Y.
        authoring = Usd.Stage.CreateNew(path)
        mesh = UsdGeom.Mesh.Define(authoring, "/Scalp")
        points = [(float(x), 0.0, float(z))
                  for z in (0, 2, 4) for x in (0, 2, 4)]
        indices = []
        for row in range(2):
            for col in range(2):
                a = row * 3 + col
                indices += [a, a + 3, a + 4, a + 1]
        mesh.CreatePointsAttr(points)
        mesh.CreateFaceVertexCountsAttr([4] * 4)
        mesh.CreateFaceVertexIndicesAttr(indices)
        authoring.GetRootLayer().Save()

    scenePath = os.path.join(folder, "scene.usda")
    writeScalpScene(scenePath)

    curvesPath = os.path.join(folder, "curves.usda")
    curvesStage = Usd.Stage.CreateNew(curvesPath)
    curves = UsdGeom.BasisCurves.Define(curvesStage, "/Strand")
    curves.CreateTypeAttr(UsdGeom.Tokens.linear)
    curves.CreatePointsAttr([(3.0, 0.0, 3.0), (3.0, 0.5, 3.1),
                             (3.0, 1.0, 3.2), (3.0, 1.5, 3.3)])
    curves.CreateCurveVertexCountsAttr([4])
    curvesStage.GetRootLayer().Save()
    del curvesStage

    def guideCount(session):
        count = ctypes.c_int(0)
        session.dll.Tonic_GetGuideCounts(session.model, ctypes.byref(count),
                                         None)
        return int(count.value)

    stage = Usd.Stage.Open(scenePath)
    heard = []

    def sink(text, level="info"):
        heard.append((text, level))

    session = tonicSession.TonicSession(tonicToolState.TonicToolState())
    session.setStatusSink(sink)
    if not session.activate("/Scalp", stage=stage):
        check(False, "save: a session binds the scene's scalp (%r)" % heard)
        return
    dll = session.dll
    model = session.model
    check(dll.Tonic_BuildTestTube(model, 0, 0, 0.0, 0.0) ==
          tonicLib.TONIC_OK, "save: the test tube builds over the scalp")
    dll.Tonic_SetFillParams(model, 30.0, 8, 0, 0.0, None, 0)
    dll.Tonic_RefillGuides(model, ctypes.c_float(1.0))
    imported = 1 if session.importCurves(curvesPath, 0) else 0
    check(imported == 1, "save: one curve imports as a locked tube (%r)"
          % heard[-1:])
    tubes = len(tonicBridge.readTubeIds(dll, model))
    guides = guideCount(session)
    graph = session.graphCounts()
    check(tubes >= 2 and guides > 0,
          "save: the groom has tubes and guides to save (%d, %d)"
          % (tubes, guides))

    # -- save into the scene and reopen it --------------------------------
    # The reference is what the live commit hydrates to, composed the way
    # usdview composes it (root + session): the saved file, reopened, must
    # hydrate to exactly that.
    check(session.flushLatest(), "save: the live layer commits")
    probe = ctypes.c_void_p()
    dll.Tonic_Create(ctypes.byref(probe))
    try:
        reference = session.stageLib.hydrateFromLayers(
            probe, [stage.GetRootLayer().identifier,
                    stage.GetSessionLayer().identifier], "/TonicGroom")
    finally:
        dll.Tonic_Destroy(probe)
    check(reference[1] == guides,
          "save: the live commit carries every guide (%r, %d)"
          % (reference, guides))
    del heard[:]
    groomFile = os.path.join(folder, "hair.usdc")
    check(session.saveGroom(groomFile, stage=stage, addToRootLayer=True),
          "save: saveGroom(addToRootLayer=True) succeeds (%r)" % heard[-1:])
    check(os.path.isfile(groomFile), "save: hair.usdc is on disk")
    check(any("saved" in text and "no region map baked yet" in text and
              level == "warning" for text, level in heard),
          "save: with no bake yet the status warns about the map (%r)"
          % heard[-1:])
    onDisk = Sdf.Layer.OpenAsAnonymous(scenePath)
    check(onDisk is not None and
          "./hair.usdc" in list(onDisk.subLayerPaths),
          "save: the scene FILE sublayers ./hair.usdc (%r)"
          % (list(onDisk.subLayerPaths) if onDisk else None,))
    sessionSubs = list(stage.GetSessionLayer().subLayerPaths)
    check(not any(p.endswith("hair.usdc") for p in sessionSubs) and
          sessionSubs[:1] == [session.liveLayerId],
          "save: the live overlay stays the only session sublayer (%r)"
          % sessionSubs)
    check(session.saveGroom(groomFile, stage=stage, addToRootLayer=True) and
          list(stage.GetRootLayer().subLayerPaths).count("./hair.usdc") == 1,
          "save: saving the same file again adds no second sublayer")
    session.deactivate()

    reopened = Usd.Stage.Open(scenePath)
    reopened.Reload()
    check(bool(reopened.GetPrimAtPath("/TonicGroom")),
          "save: the reopened scene composes /TonicGroom from hair.usdc")
    fresh = tonicSession.TonicSession(tonicToolState.TonicToolState())
    del heard[:]
    fresh.setStatusSink(sink)
    if not fresh.hydrate(groomPath="/TonicGroom", stage=reopened):
        check(False, "save: the reopened groom hydrates (%r)" % heard)
        return
    check(tuple(fresh.hydratedCounts) == tuple(reference),
          "save: the reopened file hydrates the same tube/guide/imported "
          "counts as the live commit (%r, want %r)"
          % (fresh.hydratedCounts, reference))
    check(len(tonicBridge.readTubeIds(fresh.dll, fresh.model)) == tubes,
          "save: every tube, the imported one included, comes back (%d, "
          "want %d)" % (len(tonicBridge.readTubeIds(fresh.dll, fresh.model)),
                        tubes))
    check(fresh.graphCounts() == graph,
          "save: and the same scalp graph (%r, want %r)"
          % (fresh.graphCounts(), graph))

    mapsDir = os.path.join(folder, "maps")
    os.makedirs(mapsDir)
    baked = os.path.join(folder, "baked.v1.ptx")
    with open(baked, "wb") as handle:
        handle.write(b"not really a ptex, but a file to copy")
    fresh._mapFile = baked
    del heard[:]
    first = os.path.join(mapsDir, "first.usdc")
    second = os.path.join(mapsDir, "second.usdc")
    check(fresh.saveGroom(first, stage=reopened) and
          fresh.saveGroom(second, stage=reopened),
          "maps: two grooms save into one directory (%r)" % heard[-1:])
    for name in ("first", "second"):
        ptx = os.path.join(mapsDir, "%s.regionMap.ptx" % name)
        layer = Sdf.Layer.FindOrOpen(os.path.join(mapsDir, "%s.usdc" % name))
        spec = layer.GetAttributeAtPath(
            "/TonicGroom/RegionMap.usdGen:map:file") if layer else None
        value = spec.default.path if spec is not None else None
        check(os.path.isfile(ptx) and value == "./%s.regionMap.ptx" % name,
              "maps: %s.usdc names its own %s.regionMap.ptx (%r)"
              % (name, name, value))
    check(not os.path.isfile(os.path.join(mapsDir, "regionMap.ptx")),
          "maps: nothing is written to the shared regionMap.ptx name")
    sessionOnlyGroom = [p for p in reopened.GetSessionLayer().subLayerPaths
                        if p.endswith("second.usdc")]
    check(len(sessionOnlyGroom) == 1,
          "maps: a default save sublayers the groom in the session layer")

    bare = os.path.join(mapsDir, "x")
    check(fresh.saveGroom(bare, stage=reopened) and
          os.path.isfile(bare + ".usdc"),
          "save: saveGroom('x') writes x.usdc")
    del heard[:]
    check(not fresh.saveGroom(os.path.join(mapsDir, "y.usda"),
                              stage=reopened) and
          not os.path.isfile(os.path.join(mapsDir, "y.usda")) and
          any(level == "error" and ".usdc" in text for text, level in heard),
          "save: a .usda name is refused out loud (%r)" % heard[-1:])

    # -- the committer never catches up: the save gives up on time --------
    dll = fresh.dll
    dll.Tonic_SetFillParams(fresh.model, 55.0, 8, 0, 0.0, None, 0)
    dll.Tonic_RefillGuides(fresh.model, ctypes.c_float(1.0))
    check(fresh.modelVersion > fresh.committedVersion,
          "timeout: the edit leaves a version to commit (%d > %d)"
          % (fresh.modelVersion, fresh.committedVersion))
    original = dll.Tonic_CommitterSwap
    ticks = []
    stuck = os.path.join(mapsDir, "stuck.usdc")
    del heard[:]
    dll.Tonic_CommitterSwap = \
        lambda *args: tonicLib.TONIC_COMMITTER_NOTHING_PENDING
    try:
        started = time.monotonic()
        saved = fresh.saveGroom(stuck, stage=reopened, timeout=0.2,
                                tick=lambda: ticks.append(1))
        elapsed = time.monotonic() - started
    finally:
        dll.Tonic_CommitterSwap = original
    check(saved is False and not os.path.isfile(stuck),
          "timeout: a committer that never swaps fails the save")
    check(elapsed < 0.5, "timeout: and gives up in %.3f s (< 0.5 s)"
          % elapsed)
    check(any("timed out" in text and level == "error"
              for text, level in heard),
          "timeout: the status names the timeout (%r)" % heard[-1:])
    check(len(ticks) > 0, "timeout: tick() ran while waiting (%d)"
          % len(ticks))
    check(fresh.saveGroom(stuck, stage=reopened, timeout=10.0) and
          os.path.isfile(stuck),
          "timeout: with the committer back the same save succeeds")

    fresh._detached = True
    del heard[:]
    try:
        refused = fresh.saveGroom(os.path.join(mapsDir, "detached.usdc"),
                                  stage=reopened)
    finally:
        fresh._detached = False
    check(refused is False and
          any("detached" in text for text, _level in heard),
          "save: a detached session refuses to save (%r)" % heard[-1:])
    fresh.deactivate()

    # -- a session-layer groom hydrates through the composed stage --------
    # The default save sublayers the file beside the live overlay in the
    # SESSION layer; the scalp lives in the root layer. A stage opened on
    # either layer alone cannot hydrate it; root + session (what usdview
    # shows) can.
    plainPath = os.path.join(folder, "plain.usda")
    writeScalpScene(plainPath)
    plain = Usd.Stage.Open(plainPath)
    overlay = tonicSession.TonicSession(tonicToolState.TonicToolState())
    overlay.setStatusSink(sink)
    if not overlay.activate("/Scalp", stage=plain):
        check(False, "overlay: a session binds plain.usda (%r)" % heard)
        return
    overlay.dll.Tonic_BuildTestTube(overlay.model, 0, 0, 0.0, 0.0)
    overlay.dll.Tonic_RefillGuides(overlay.model, ctypes.c_float(1.0))
    want = (len(tonicBridge.readTubeIds(overlay.dll, overlay.model)),
            guideCount(overlay))
    check(overlay.saveGroom(os.path.join(folder, "overlay.usdc"),
                            stage=plain),
          "overlay: the default save succeeds")
    overlay.deactivate()
    check(bool(plain.GetPrimAtPath("/TonicGroom")) and
          not list(plain.GetRootLayer().subLayerPaths),
          "overlay: only the session layer composes the saved groom")
    again = tonicSession.TonicSession(tonicToolState.TonicToolState())
    again.setStatusSink(sink)
    del heard[:]
    ok = again.hydrate(groomPath="/TonicGroom", stage=plain)
    got = ((len(tonicBridge.readTubeIds(again.dll, again.model)),
            again.hydratedCounts[1]) if ok else None)
    check(ok and got == want,
          "overlay: hydrate() composes root + session and restores it "
          "(%r, want %r; %r)" % (got, want, heard[-1:]))
    again.deactivate()


def partF():
    """SS-05: committer failures and swap errors reach the artist.

    A worker build that fails parks its reason in the committer; the pump
    takes it once into status()["commitError"], says so once, and the
    warnings list offers a retry. A swap into a live layer that is gone
    answers TONIC_COMMITTER_ERROR (never PARTIAL) with one status line. An
    artist-owned Output prim refuses the enqueue out loud. A failed
    hierarchy bake prints the stage error buffer it actually wrote.
    """
    import types as _types
    from pxr import Usd, UsdGeom
    tonicLib = sys.modules.get(_PKG + ".tonicLib") or _load("tonicLib")
    tonicSession = sys.modules.get(_PKG + ".tonicSession") or \
        _load("tonicSession")
    tonicToolState = sys.modules.get(_PKG + ".tonicToolState") or \
        _load("tonicToolState")
    tonicHud = sys.modules.get(_PKG + ".tonicHud") or _load("tonicHud")

    stage = Usd.Stage.CreateInMemory("tonicCommitFailures")
    mesh = UsdGeom.Mesh.Define(stage, "/Scalp")
    # A 2x2-quad plane over [0, 4] in XZ, facing +Y.
    points = [(float(x), 0.0, float(z)) for z in (0, 2, 4) for x in (0, 2, 4)]
    indices = []
    for row in range(2):
        for col in range(2):
            a = row * 3 + col
            indices += [a, a + 3, a + 4, a + 1]
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr([4] * 4)
    mesh.CreateFaceVertexIndicesAttr(indices)

    state = tonicToolState.TonicToolState()
    session = tonicSession.TonicSession(state)
    heard = []
    session.setStatusSink(lambda text, level="info": heard.append(
        (text, level)))
    if not session.activate("/Scalp", stage=stage):
        check(False, "commit: a session binds the plane scalp (%r)" % heard)
        return
    dll = session.dll
    container = _types.SimpleNamespace(session=session)

    def failures():
        return [(t, lv) for t, lv in heard if "commit failed" in t]

    def pumpUntil(predicate, seconds=10.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            session.pump()
            if predicate():
                return True
            time.sleep(0.005)
        return predicate()

    # -- a scalp with no tube: nothing to commit is not a failure ---------
    del heard[:]
    check(session.enqueueCommit() is True and session.pendingVersion == 0
          and not session.hasPendingWork() and not failures(),
          "commit: a fresh groom with no tube enqueues nothing and "
          "reports nothing (%r)" % heard)

    # -- a real worker failure reaches status(), the log and the list ----
    # Straight through the C ABI: the worker refuses a tubeless snapshot,
    # which is the one build failure a test can cause on purpose.
    check(dll.Tonic_CommitterEnqueue(session._committer, 0, 0, 0, None) ==
          tonicLib.TONIC_OK, "commit: the tubeless version is enqueued")
    got = pumpUntil(lambda: bool(session.status()["commitError"]))
    status = session.status()
    check(got and "no tubes" in status["commitError"],
          "commit: the failed build lands in status()['commitError'] (%r)"
          % status["commitError"])
    check(status["failedVersion"] == session.modelVersion and
          not session.hasPendingWork(),
          "commit: the failed version is parked and the pump has no "
          "work left (failed v%d, model v%d)"
          % (status["failedVersion"], session.modelVersion))
    session.pump()
    session.pump()
    check(len(failures()) == 1 and failures()[0][1] == "error",
          "commit: exactly one 'commit failed' line, at error level (%r)"
          % failures())
    rows = tonicHud.warnings(state, session)
    check(bool(rows) and rows[0].severity == "error" and
          rows[0].text.startswith("Commit failed: ") and
          "no tubes" in rows[0].text,
          "commit: the warnings list leads with 'Commit failed: <reason>' "
          "(%r)" % [(r.severity, r.text) for r in rows])
    retry = rows[0].selectAction if rows else None
    check(retry is not None and
          getattr(retry, "label", "") == "Retry commit",
          "commit: and the row's action is 'Retry commit'")
    strip = tonicHud.statusStrip(state, session, status)
    check(tonicHud.syncSummary(strip, status)[1] == tonicHud.TONE_ERROR,
          "commit: the sync pill turns to the error tone (%r)"
          % (tonicHud.syncSummary(strip, status),))
    keyBefore = tonicHud.warningsKey(state, session, status)

    # Retry after the groom grows a tube: the row's action enqueues, the
    # commit lands, and the failure is forgotten.
    check(dll.Tonic_BuildTestTube(session.model, 0, 0, 0.0, 0.0) ==
          tonicLib.TONIC_OK, "commit: the test tube builds over the scalp")
    if retry is not None:
        retry(container)
    got = pumpUntil(lambda: session.committedVersion > 0 and
                    not session.hasPendingWork())
    status = session.status()
    check(got and status["commitError"] == "" and
          status["failedVersion"] == 0,
          "commit: 'Retry commit' lands the groom and clears the failure "
          "(committed v%d, error %r)"
          % (status["committedVersion"], status["commitError"]))
    check(not any(r.text.startswith("Commit failed")
                  for r in tonicHud.warnings(state, session)) and
          tonicHud.warningsKey(state, session) != keyBefore,
          "commit: the row goes and the warnings key moves with it")

    # -- a swap into a live layer that is gone ---------------------------
    # The stage's layer stack keeps the anonymous live layer alive, so the
    # session's handle on it is what the test drops: an identifier that
    # SdfLayer::Find no longer resolves is exactly what a dead layer looks
    # like from the committer's side.
    liveId = session._liveId
    session._liveId = "anon:tonicSS05:gone"
    del heard[:]
    try:
        dll.Tonic_MoveCenterRing(session.model, 1, 0.1, 0.0)
        session.enqueueCommit()
        first = session.pump()
        second = session.pump()
    finally:
        session._liveId = liveId
    check(first["swapCode"] == tonicLib.TONIC_COMMITTER_ERROR and
          second["swapCode"] == tonicLib.TONIC_COMMITTER_ERROR and
          tonicLib.TONIC_COMMITTER_ERROR != tonicLib.TONIC_COMMITTER_PARTIAL,
          "swap: a dead live layer answers TONIC_COMMITTER_ERROR (%r, %r)"
          % (first["swapCode"], second["swapCode"]))
    check(len(heard) == 1 and heard[0][1] == "error" and
          "unknown live layer" in heard[0][0],
          "swap: two pumps, exactly one status line naming it (%r)" % heard)
    check("unknown live layer" in session.status()["commitError"] and
          not first["pending"] and not second["pending"],
          "swap: status() carries it and the idle pump stops")
    session.enqueueCommit()
    got = pumpUntil(lambda: not session.hasPendingWork() and
                    session.committedVersion == session.pendingVersion)
    check(got and session.status()["commitError"] == "",
          "swap: with the layer back the next commit lands and clears it")

    # -- an artist-owned Output prim refuses the enqueue out loud ---------
    if session.outputSettingsAvailable():
        check(session.setOutputSettings(enabled=True, publish=False,
                                        enqueue=False),
              "guard: Output enables while nothing collides")
        stage.DefinePrim("/TonicGroom/OutputCurves", "BasisCurves")
        del heard[:]
        pendingBefore = session.pendingVersion
        dll.Tonic_MoveCenterRing(session.model, 1, -0.1, 0.0)
        check(session.enqueueCommit() is False and
              session.pendingVersion == pendingBefore,
              "guard: the enqueue is refused and nothing is queued")
        status = session.status()
        check("artist-owned output at /TonicGroom/OutputCurves" in
              status["commitError"] and len(failures()) == 1,
              "guard: status() and one status line name the artist's prim "
              "(%r, %r)" % (status["commitError"], heard))
        check(any(r.severity == "error" and "artist-owned" in r.text
                  for r in tonicHud.warnings(state, session, status)),
              "guard: and the warnings list shows the row")
        stage.RemovePrim("/TonicGroom/OutputCurves")
        session.setOutputSettings(enabled=False, publish=False,
                                  enqueue=False)
    else:
        skip("guard: this library has no Output settings")

    # -- a failed hierarchy bake prints the buffer it wrote --------------
    # Tonic_BakeEnqueueLevels is a tonicApiStage.h entry: its reason sits
    # in Tonic_StageGetLastError, while Tonic_GetLastError holds whatever
    # the model half failed on last. Seed both with different text, then
    # fail the enqueue without touching either.
    real = getattr(dll, "Tonic_BakeEnqueueLevels", None)
    if real is None:
        skip("bake: this library has no Tonic_BakeEnqueueLevels")
    else:
        real(None)
        dll.Tonic_Create(None)
        modelError = session.lastError()
        dll.Tonic_BakeEnqueueLevels = lambda bake: tonicLib.TONIC_ERROR
        del heard[:]
        try:
            ok = session.rebake()
        finally:
            dll.Tonic_BakeEnqueueLevels = real
        check(ok is False and len(heard) == 1 and
              "Tonic_BakeEnqueueLevels: null bake context" in heard[0][0] and
              modelError not in heard[0][0] and heard[0][1] == "error",
              "bake: the status names the stage error, not the model's "
              "(%r; model buffer %r)" % (heard, modelError))
    session.deactivate()
    check(session.status()["commitError"] == "",
          "commit: deactivate forgets the failure")


def _planeStage(name):
    """An in-memory stage with a 2x2-quad plane /Scalp over [0, 4] in XZ."""
    from pxr import Usd, UsdGeom
    stage = Usd.Stage.CreateInMemory(name)
    mesh = UsdGeom.Mesh.Define(stage, "/Scalp")
    points = [(float(x), 0.0, float(z)) for z in (0, 2, 4) for x in (0, 2, 4)]
    indices = []
    for row in range(2):
        for col in range(2):
            a = row * 3 + col
            indices += [a, a + 3, a + 4, a + 1]
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr([4] * 4)
    mesh.CreateFaceVertexIndicesAttr(indices)
    return stage


def partG():
    """SS-06: session lifecycle hygiene.

    A failed activate leaves no model, committer or live sublayer behind;
    deactivate forgets every per-model flag, so detach -> deactivate ->
    activate comes back attached with no phantom work; publish and idle
    hooks are listener lists; the scalp arrays convert through numpy;
    bakeDirectory() sweeps the directories of dead processes only.
    """
    import shutil
    import subprocess
    import tempfile
    tonicLib = sys.modules.get(_PKG + ".tonicLib") or _load("tonicLib")
    tonicSession = sys.modules.get(_PKG + ".tonicSession") or \
        _load("tonicSession")
    tonicToolState = sys.modules.get(_PKG + ".tonicToolState") or \
        _load("tonicToolState")

    stage = _planeStage("tonicLifecycle")
    state = tonicToolState.TonicToolState()
    session = tonicSession.TonicSession(state)
    heard = []
    session.setStatusSink(lambda text, level="info": heard.append(
        (text, level)))
    session._ensureLibrary()
    dll = session.dll

    def liveSublayers():
        return list(stage.GetSessionLayer().subLayerPaths)

    # -- a bake worker that will not start tears the whole start down ----
    realBake = dll.Tonic_BakeCreate
    realDestroy = dll.Tonic_CommitterDestroy
    destroyed = []

    def recordDestroy(committer):
        destroyed.append(committer)
        return realDestroy(committer)

    dll.Tonic_BakeCreate = lambda *args: tonicLib.TONIC_ERROR
    dll.Tonic_CommitterDestroy = recordDestroy
    try:
        ok = session.activate("/Scalp", stage=stage)
    finally:
        dll.Tonic_BakeCreate = realBake
        dll.Tonic_CommitterDestroy = realDestroy
    check(ok is False and session.model is None and
          session.committer is None and session.bake is None,
          "lifecycle: a failed Tonic_BakeCreate leaves no model, committer "
          "or bake (%r)" % heard)
    check(len(destroyed) == 1,
          "lifecycle: and destroys the committer it had started (%d)"
          % len(destroyed))
    check(liveSublayers() == [] and session.liveLayerId == "",
          "lifecycle: and takes the live sublayer back out (%r)"
          % liveSublayers())
    check(not state.activated and not session.hasPendingWork(),
          "lifecycle: and the tool is not left looking activated")
    check(any(level == "error" for _t, level in heard),
          "lifecycle: the failure is an error line (%r)" % heard)

    # -- Tonic_Activate failing after the workers started ---------------
    realActivate = dll.Tonic_Activate
    dll.Tonic_Activate = lambda model: tonicLib.TONIC_ERROR
    try:
        ok = session.activate("/Scalp", stage=stage)
    finally:
        dll.Tonic_Activate = realActivate
    check(ok is False and session.model is None and
          session.committer is None and liveSublayers() == [],
          "lifecycle: a failed Tonic_Activate tears the workers down too")

    # -- detach -> deactivate -> activate: nothing carries over -----------
    check(session.activate("/Scalp", stage=stage),
          "lifecycle: the session activates (%r)" % heard)
    check(dll.Tonic_BuildTestTube(session.model, 0, 0, 0.0, 0.0) ==
          tonicLib.TONIC_OK, "lifecycle: the test tube builds")
    session.rebake()
    session.stageLib
    # What an Output toggle and a finished bake leave behind; set by hand
    # because their triggers need a full stage round trip.
    session._pendingOutputReveal = True
    session._mapFile = os.path.join(session.bakeDir or ".", "old.v1.ptx")
    check(session.detach() and session.status()["detached"],
          "lifecycle: detach marks the session detached")
    session.deactivate()
    leftovers = {
        "_bakeInFlight": session._bakeInFlight,
        "_detached": session._detached,
        "_lastSwapCode": session._lastSwapCode,
        "_mapFile": session._mapFile,
        "_pendingOutputReveal": session._pendingOutputReveal,
        "_stage": session._stage,
        "_stageLib": session._stageLib,
        "_scalpMissing": session._scalpMissing,
        "_commitError": session._commitError,
    }
    check(leftovers == {
        "_bakeInFlight": False, "_detached": False,
        "_lastSwapCode": tonicLib.TONIC_COMMITTER_NOTHING_PENDING,
        "_mapFile": "", "_pendingOutputReveal": False, "_stage": None,
        "_stageLib": None, "_scalpMissing": "", "_commitError": ""},
        "lifecycle: deactivate resets every per-model flag (%r)" % leftovers)
    check(session.activate("/Scalp", stage=stage),
          "lifecycle: the session activates again")
    session.pump()
    check(session.status()["detached"] is False and
          not session.hasPendingWork(),
          "lifecycle: it comes back attached with no pending work after "
          "one pump (detached %r, pending %r)"
          % (session.status()["detached"], session.hasPendingWork()))

    # -- publish and idle hooks are listener lists ------------------------
    first, second, hooked = [], [], []

    def one():
        first.append(1)

    def two():
        second.append(1)

    session.addPublishListener(one)
    session.addPublishListener(two)
    session.addPublishListener(one)          # a second add is a no-op
    session.setPublishHook(lambda: hooked.append(1))
    session.publishAll()
    check(first == [1] and second == [1] and hooked == [1],
          "listeners: two publish listeners and the hook all fire once "
          "(%d, %d, %d)" % (len(first), len(second), len(hooked)))
    session.setPublishHook(None)
    session.removePublishListener(one)
    session.publishAll()
    check(first == [1] and second == [1, 1] and hooked == [1],
          "listeners: setPublishHook(None) and removePublishListener drop "
          "only their own entry")
    session.removePublishListener(two)
    woken = []
    session.addIdleListener(lambda: woken.append("a"))
    session.setIdleHook(lambda: woken.append("hook"))
    session.setIdleHook(lambda: woken.append("hook2"))
    session._wakeIdle()
    check(woken == ["a", "hook2"],
          "listeners: the idle hook replaces only itself (%r)" % woken)
    session.setIdleHook(None)

    # -- the scalp arrays convert through numpy -------------------------
    try:
        import numpy
    except ImportError:
        numpy = None
    mesh = session._scalpMesh("/Scalp", stage)
    if numpy is None:
        skip("fast bind: numpy is not importable here")
    else:
        check(isinstance(mesh[0], numpy.ndarray) and
              mesh[0].dtype == numpy.float32 and len(mesh[0]) == 27 and
              list(mesh[1]) == [4] * 4 and len(mesh[2]) == 16,
              "fast bind: the Vt arrays convert to numpy buffers")
    check(tuple(round(c, 5) for c in mesh[3]) == (2.0, 0.0, 2.0) and
          tuple(round(c, 5) for c in session.scalpCenter) == (2.0, 0.0, 2.0),
          "fast bind: and the scalp centre is the bounding-box centre (%r)"
          % (mesh[3],))
    session.deactivate()

    # -- bakeDirectory() sweeps dead processes' directories ------------
    base = tempfile.mkdtemp(prefix="tonicBakeSweep")
    savedTemp = tempfile.tempdir
    savedOverride = os.environ.pop("USDGENTONIC_BAKE_DIR", None)
    try:
        # A pid that is certainly dead: a child that has exited and been
        # waited on.
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        child.wait()
        root = os.path.join(base, "usdGenTonicBake")
        dead = os.path.join(root, "p%d" % child.pid)
        live = os.path.join(root, "p%d" % os.getpid())
        other = os.path.join(root, "notAPid")
        for folder in (dead, live, other):
            os.makedirs(folder)
            open(os.path.join(folder, "map.v1.ptx"), "w").close()
        tempfile.tempdir = base
        mine = tonicSession.bakeDirectory()
        check(mine == live,
              "sweep: bakeDirectory() is still this process's (%r)" % mine)
        check(not os.path.exists(dead),
              "sweep: a dead process's p<pid> directory is swept")
        check(os.path.isdir(live) and os.path.isdir(other),
              "sweep: a live process's directory and foreign names stay")
    finally:
        tempfile.tempdir = savedTemp
        if savedOverride is not None:
            os.environ["USDGENTONIC_BAKE_DIR"] = savedOverride
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    code = main()
    print("%d failure(s), %d skip(s)" % (failures, skips))
    if failures:
        sys.exit(1)
    sys.exit(code)
