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
    for _ in range(20000):
        dll.Tonic_CommitterSwap(committer, live.identifier.encode("utf-8"), 0)
        committed = dll.Tonic_CommitterCommittedVersion(committer)
        if committed >= want:
            break
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


if __name__ == "__main__":
    code = main()
    print("%d failure(s), %d skip(s)" % (failures, skips))
    if failures:
        sys.exit(1)
    sys.exit(code)
