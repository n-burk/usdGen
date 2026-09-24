#!/usr/bin/env python3
# testUsdGenTonicToolsBridge -- T1: the Qt-free P5 half of
# usdGenTonicTools (plan/17 P5 exit: import/export round-trip test).
#
# Part A is pure Python (no pxr, no dll, no numpy): resampling, ring
# math, the geodesic/Dijkstra check, the BasisCurves spec builders and
# the C ABI quote table. Part B runs the real round trip -- test tube
# -> BasisCurves on an in-memory stage -> locked L3 reimport -> error
# -- when pxr and the usdGenTonic DLL are available, and is strict
# under CTest (USDGENTONIC_DLL set) but a skip for direct runs without
# them. Modules load by file path so the package __init__ (which
# imports pxr.Usdviewq) is never executed.
import ctypes
import importlib.util
import math
import os
import sys

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenTonicTools"))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        "tonic_%s" % name, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


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


def checkRaises(exc, fn, what):
    try:
        fn()
    except exc:
        check(True, what)
    except Exception as other:  # noqa: BLE001 - the test names the type
        check(False, "%s (raised %r)" % (what, other))
    else:
        check(False, "%s (no raise)" % what)


class _NoDll:
    """A dll exporting nothing: every P5 entry is missing."""


def _planeMesh(nx, ny, size=1.0):
    """A triangle-mesh plane grid: (points, faces, counts/indices)."""
    points = []
    for iy in range(ny + 1):
        for ix in range(nx + 1):
            points.append((ix * size / nx, 0.0, iy * size / ny))
    faces = []
    for iy in range(ny):
        for ix in range(nx):
            a = iy * (nx + 1) + ix
            b = a + 1
            c = a + (nx + 1)
            d = c + 1
            faces.append([a, c, b])
            faces.append([b, c, d])
    counts = [3] * len(faces)
    indices = [v for face in faces for v in face]
    return points, faces, counts, indices


def _partA(bridge):
    # -- validators ------------------------------------------------------
    check(bridge.validateRingVerts(8) == 8, "ring verts accept 8")
    checkRaises(ValueError, lambda: bridge.validateRingVerts(2),
                "ring verts reject 2")
    check(bridge.validateSectionParams([0.0, 0.5, 1.0]) == [0.0, 0.5, 1.0],
          "section params accept an ascending [0, 1] span")
    for bad in ([0.0], [0.0, 0.0, 1.0], [0.0, 0.7, 0.5, 1.0],
                [0.1, 1.0], [0.0, 0.9], [-0.1, 0.5, 1.0]):
        checkRaises(ValueError,
                    lambda b=bad: bridge.validateSectionParams(b),
                    "section params reject %r" % (bad,))

    # -- resampling --------------------------------------------------------
    line = [(0.0, 0.0, 0.0), (3.0, 0.0, 0.0)]
    samples = bridge.resamplePolyline(line, 4)
    check([round(p[0], 9) for p in samples] == [0.0, 1.0, 2.0, 3.0]
          and all(p[1] == 0.0 and p[2] == 0.0 for p in samples),
          "resampling a straight line is arc-length exact")
    check(bridge.resamplePolyline(line, 2) == [line[0], line[1]],
          "resampling to 2 keeps the endpoints")
    bent = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0)]
    mid = bridge.resamplePolyline(bent, 3)[1]
    check(all(abs(a - b) < 1e-9 for a, b in zip(mid, (1.0, 0.0, 0.0))),
          "resampling rounds the corner at arc-length 1")
    checkRaises(ValueError, lambda: bridge.resamplePolyline(line, 1),
                "resampling to 1 raises")
    checkRaises(ValueError,
                lambda: bridge.resamplePolyline([(0.0, 0.0, 0.0)], 4),
                "resampling one point raises")
    check(abs(bridge.polylineLength(line) - 3.0) < 1e-12,
          "polyline length measures 3")

    # -- ring math ---------------------------------------------------------
    ring = [(1.0, 0.0, 0.0), (0.0, 0.0, 1.0),
            (-1.0, 0.0, 0.0), (0.0, 0.0, -1.0)]
    center = bridge.ringCentroid(ring)
    check(all(abs(v) < 1e-12 for v in center),
          "ring centroid finds the square center")
    check(abs(bridge.ringRadius(ring) - 1.0) < 1e-12,
          "ring radius measures 1")
    checkRaises(ValueError, lambda: bridge.ringCentroid([]),
                "empty ring centroid raises")
    rings = [[(x, 0.0, z) for x, z in [(1.0, 0.0), (-1.0, 0.0), (0.0, 1.0)]],
             [(x, 2.0, z) for x, z in [(1.0, 0.0), (-1.0, 0.0), (0.0, 1.0)]]]
    centers = bridge.sweptRingsToCenters(rings)
    check(len(centers) == 2 and abs(centers[0][1]) < 1e-12
          and abs(centers[1][1] - 2.0) < 1e-12,
          "swept rings reduce to per-ring centroids")
    checkRaises(ValueError, lambda: bridge.sweptRingsToCenters([rings[0]]),
                "one ring is not a sweep")

    # -- round-trip error --------------------------------------------------
    moved = [(x + 0.25, y, z) for x, y, z in line]
    check(bridge.roundTripError(line, line) == 0.0,
          "identical polylines round-trip at zero error")
    check(abs(bridge.roundTripError(line, moved) - 0.25) < 1e-9,
          "a 0.25 shift round-trips at 0.25 error")
    check(abs(bridge.maxDeviation(line, moved) - 0.25) < 1e-12,
          "max deviation measures the shift")
    checkRaises(ValueError,
                lambda: bridge.maxDeviation(line, line + moved),
                "ragged polylines do not compare")

    # -- surface + geodesic math -------------------------------------------
    points, faces, counts, indices = _planeMesh(2, 2, size=2.0)
    above = bridge.surfaceDistance((1.0, 0.5, 1.0), points, faces)
    check(abs(above - 0.5) < 1e-9, "surface distance measures height")
    on = bridge.surfaceDistance((0.5, 0.0, 0.5), points, faces)
    check(on < 1e-9, "a surface point reads zero distance")
    adjacency = bridge.meshAdjacency(indices, counts)
    check(sorted(adjacency[4]) == [1, 2, 3, 5, 6, 7],
          "mesh adjacency links the grid center to edges + diagonals")
    path = bridge.geodesicPath(points, adjacency, 0, 8)
    check(path[0] == 0 and path[-1] == 8 and len(path) == 5,
          "the corner-to-corner geodesic takes 4 grid steps")
    pathLen = bridge.polylineLength([points[v] for v in path])
    check(abs(pathLen - 4.0) < 1e-9,
          "the geodesic length is the Manhattan distance")
    unreachable = bridge.geodesicPath(points, {0: [], 8: []}, 0, 8)
    check(unreachable == [], "unreachable tips return no path")
    curve = [(0.5, 0.0, 0.5), (1.0, 0.0, 1.0), (1.5, 0.0, 0.5)]
    check(bridge.validateCurveOnSurface(curve, points, faces, 1e-6) < 1e-6,
          "a surface-bound curve validates")
    lifted = [(x, 0.1, z) for x, _, z in curve]
    check(abs(bridge.validateCurveOnSurface(lifted, points, faces, 1.0)
              - 0.1) < 1e-9,
          "a lifted curve reports its height")
    checkRaises(ValueError,
                lambda: bridge.validateCurveOnSurface(curve, points, faces,
                                                     -1.0),
                "negative tolerance raises")

    # -- spec builders -------------------------------------------------------
    spec = bridge.basisCurvesSpec("T0", [line, moved], [0.5, 1.0])
    check(spec["name"] == "T0" and spec["basis"] == "catmullRom"
          and spec["vertexCounts"] == [2, 2]
          and len(spec["points"]) == 4 and spec["widths"] == [0.5, 1.0],
          "the curves spec flattens two curves with widths")
    default = bridge.basisCurvesSpec("T0", [line])
    check(default["widths"] == [1.0], "widths default to uniform 1.0")
    checkRaises(ValueError, lambda: bridge.basisCurvesSpec("T0", [line], [],
                                                           "linear"),
                "unknown basis raises")
    checkRaises(ValueError, lambda: bridge.basisCurvesSpec("T0", []),
                "empty curve list raises")
    checkRaises(ValueError,
                lambda: bridge.basisCurvesSpec("T0", [[(0.0, 0.0, 0.0)]]),
                "one-point curves raise")
    checkRaises(ValueError,
                lambda: bridge.basisCurvesSpec("T0", [line, moved], [1.0]),
                "ragged widths raise")
    ordered = bridge.centerCurvesSpec({3: moved, 1: line}, radius=0.5)
    check(ordered["tubeIds"] == [1, 3]
          and ordered["vertexCounts"] == [2, 2]
          and ordered["widths"] == [0.5, 0.5],
          "the center export orders tubes by id")
    checkRaises(ValueError, lambda: bridge.centerCurvesSpec({}),
                "exporting nothing raises")

    # -- C ABI quote table ---------------------------------------------------
    expectedEntries = ("Tonic_ReadTubeIds", "Tonic_GetTubeCenterCount",
                       "Tonic_GetTubeCenterCV", "Tonic_GetTubeCenterHandle",
                       "Tonic_GetTubeSectionCount", "Tonic_GetTubeSection",
                       "Tonic_ImportLockedTube", "Tonic_ImportSweptMesh",
                       "Tonic_IsTubeImported")
    check(tuple(entry[0] for entry in bridge.REQUIRED_C_API) == expectedEntries,
          "the bridge table includes the explicit displayed-center handle ABI")
    check("float *out3" in bridge.cApiQuote("Tonic_GetTubeCenterHandle"),
          "the displayed-center handle ABI has its complete C quote")
    quote = bridge.cApiQuote("Tonic_ImportSweptMesh")
    check("ringCount" in quote and bridge.cApiQuote("Tonic_Nope") == "",
          "cApiQuote resolves known entries and blanks unknown ones")
    check(bridge.missingEntries(_NoDll()) ==
          [name for name, _, _ in bridge.REQUIRED_C_API],
          "every P5 entry is missing on an empty dll")
    try:
        bridge.requireEntry(_NoDll(), "Tonic_ImportLockedTube")
        check(False, "requireEntry raises on a missing entry")
    except NotImplementedError as exc:
        check("Tonic_ImportLockedTube" in str(exc) and "secT" in str(exc),
              "requireEntry quotes the missing signature")

    # -- status line ---------------------------------------------------------
    check(bridge.roundTripStatus(4, 1e-6, 1e-3).startswith("PASS")
          and bridge.roundTripStatus(4, 1.0, 1e-3).startswith("FAIL"),
          "the round-trip status passes and fails on tolerance")


def _partB(bridge, tonicLib):
    """The live round trip: tube -> curves -> locked L3 -> error."""
    from pxr import Usd

    lib = tonicLib.Library()
    dll = lib.dll
    missing = bridge.missingEntries(dll)
    check(not missing, "the DLL exports the P5 C ABI"
          + ("" if not missing else " (missing %s)" % ", ".join(missing)))
    if missing:
        return

    ctx = ctypes.c_void_p(None)
    check(lib.dll.Tonic_Create(ctypes.byref(ctx)) == 0 and ctx.value,
          "Tonic_Create hands out a model")
    if not ctx.value:
        return
    try:
        check(lib.dll.Tonic_BuildTestTube(ctx, 9, 8, 0.5, 4.0) == 0,
              "the 9-ring test tube builds")
        check(bridge.readTubeIds(dll, ctx) == [0],
              "a fresh model holds only tube 0")
        centers = bridge.tubeCenters(dll, ctx, 0)
        check(len(centers) == 9
              and abs(centers[0][1]) < 1e-6
              and abs(centers[-1][1] - 4.0) < 1e-6,
              "tube 0 centers run 0..4 along +Y")
        handles = bridge.tubeCenterHandles(dll, ctx, 0)
        handleError = bridge.roundTripError(handles, centers)
        check(handleError < 1e-5,
              "a symmetric tube's displayed handles match its raw center cage "
              "within centroid precision (%.3g)" % handleError)
        check(not bridge.isTubeImported(dll, ctx, 0),
              "tube 0 is not a bridge import")

        stage = Usd.Stage.CreateInMemory()
        prim, spec = bridge.exportCenterCurves(stage, dll, ctx, [0],
                                              "/Bridge", radius=0.5)
        check(prim.IsValid()
              and str(prim.GetPath()) == "/Bridge/TonicCenters",
              "the center export authors /Bridge/TonicCenters")
        curves = bridge.readBasisCurves(stage, "/Bridge/TonicCenters")
        check(len(curves) == 1 and len(curves[0]) == 9
              and bridge.maxDeviation(curves[0], centers) < 1e-6,
              "the authored curve carries the 9 centers back")
        checkRaises(RuntimeError,
                    lambda: bridge.readBasisCurves(stage, "/Bridge/Missing"),
                    "reading a missing curves prim raises")

        # Curve reimport: one single-curve locked tube under tube 0.
        [child] = bridge.importCurvesAsLockedTubes(dll, ctx, 0, curves)
        check(child != 0 and child in bridge.readTubeIds(dll, ctx),
              "the curve reimports as a new locked tube T%d" % child)
        check(bridge.isTubeImported(dll, ctx, child),
              "the reimported tube reads back as imported")
        again = bridge.tubeCenters(dll, ctx, child)
        error = bridge.roundTripError(centers, again)
        print("    " + bridge.roundTripStatus(child, error, 1e-3))
        check(error < 1e-3, "curve round-trip error %.3g under 1e-3" % error)
        check(bridge.tubeSectionCount(dll, ctx, child) == 4,
              "the single-curve tube carries 4 unit sections")
        t, ring, scale, twist = bridge.tubeSection(dll, ctx, child, 0)
        check(abs(t) < 1e-6 and len(ring) == 8,
              "section 0 sits at t=0 with 8 ring CVs")

        # Swept-mesh reimport: rings about the tube-0 centers.
        rings = []
        for cx, cy, cz in centers:
            rings.append([(cx + 0.5 * math.cos(2.0 * math.pi * k / 8),
                           cy,
                           cz + 0.5 * math.sin(2.0 * math.pi * k / 8))
                          for k in range(8)])
        swept = bridge.importSweptMesh(dll, ctx, 0, rings)
        check(bridge.isTubeImported(dll, ctx, swept),
              "the swept mesh reimports as locked tube T%d" % swept)
        sweptCenters = bridge.tubeCenters(dll, ctx, swept)
        sweptError = bridge.roundTripError(centers, sweptCenters)
        print("    " + bridge.roundTripStatus(swept, sweptError, 1e-3))
        check(sweptError < 1e-3,
              "swept round-trip error %.3g under 1e-3" % sweptError)
        check(bridge.tubeSectionCount(dll, ctx, swept) == 9,
              "the swept tube carries one section per ring")
        params = [bridge.tubeSection(dll, ctx, swept, r)[0]
                  for r in range(9)]
        check(all(b > a for a, b in zip(params, params[1:]))
              and abs(params[0]) < 1e-6 and abs(params[-1] - 1.0) < 1e-6,
              "swept section params ascend 0..1")
        checkRaises(ValueError,
                    lambda: bridge.importSweptMesh(dll, ctx, 0, [rings[0]]),
                    "one ring is not a swept import")
        checkRaises(ValueError,
                    lambda: bridge.importLockedTube(dll, ctx, 0, centers, []),
                    "importing without sections raises")
    finally:
        check(lib.dll.Tonic_Destroy(ctx) == 0, "Tonic_Destroy releases it")


def _partAImportValidation(bridge):
    """SS-04, pure Python: a bad curve is refused before the DLL is used.

    `_NoDll` exports nothing, so reaching importLockedTube would raise
    NotImplementedError; a ValueError proves every curve was validated
    before the first tube could have been added.
    """
    good = [(0.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 2.0, 0.0)]
    cases = (
        ([good, [(1.0, 0.0, 0.0)]], "a one-point second curve"),
        ([good, [(1.0, 0.0, 0.0), (1.0, 0.0, 0.0)]], "a zero-length curve"),
        ([good, [(1.0, 0.0, 0.0), (1.0, float("nan"), 0.0)]],
         "a non-finite point"),
        ([good, [(1.0, 0.0), (1.0, 1.0)]], "a two-component point"),
        ([], "an empty curve list"),
    )
    for curves, what in cases:
        try:
            bridge.importCurvesAsLockedTubes(_NoDll(), None, 0, curves)
        except ValueError as exc:
            check("curve" in str(exc),
                  "import validation refuses %s up front (%s)" % (what, exc))
        except Exception as other:  # noqa: BLE001 - the test names the type
            check(False, "import validation refuses %s up front (raised %r)"
                  % (what, other))
        else:
            check(False, "import validation refuses %s up front (no raise)"
                  % what)


def _loadSessionPackage():
    """tonicSession & co. through a synthetic package.

    tonicSession uses relative imports, so it cannot load through the
    flat `_load`; a package named after this test keeps the real
    usdGenTonicTools/__init__.py (which imports Qt) out of the process.
    """
    import types
    pkgName = "tonic_bridge_pkg"
    pkg = sys.modules.get(pkgName)
    if pkg is None:
        pkg = types.ModuleType(pkgName)
        pkg.__path__ = [SRC]
        sys.modules[pkgName] = pkg
    loaded = {}
    for name in ("tonicLib", "tonicBridge", "tonicToolState",
                 "tonicSession"):
        full = "%s.%s" % (pkgName, name)
        module = sys.modules.get(full)
        if module is None:
            spec = importlib.util.spec_from_file_location(
                full, os.path.join(SRC, "%s.py" % name))
            module = importlib.util.module_from_spec(spec)
            sys.modules[full] = module
            spec.loader.exec_module(module)
            setattr(pkg, name, module)
        loaded[name] = module
    return loaded


def _partC():
    """SS-04: TonicSession import/export are atomic and honest.

    A bad file imports nothing and raises nothing; a C++ refusal part-way
    through rolls the tubes already added back; export refuses folders
    and non-USD names out loud and succeeds only with a file on disk.
    """
    import shutil
    import tempfile
    folder = tempfile.mkdtemp(prefix="tonicBridgeImport")
    try:
        _partCIn(folder)
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def _writeCurves(path, curves):
    from pxr import Usd, UsdGeom
    stage = Usd.Stage.CreateNew(path)
    prim = UsdGeom.BasisCurves.Define(stage, "/Strands")
    prim.CreateTypeAttr(UsdGeom.Tokens.linear)
    prim.CreatePointsAttr([p for curve in curves for p in curve])
    prim.CreateCurveVertexCountsAttr([len(curve) for curve in curves])
    stage.GetRootLayer().Save()


def _partCIn(folder):
    from pxr import Usd, UsdGeom
    mods = _loadSessionPackage()
    tonicSession = mods["tonicSession"]
    tonicToolState = mods["tonicToolState"]
    pkgBridge = mods["tonicBridge"]

    stage = Usd.Stage.CreateInMemory("tonicBridgeImport")
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

    heard = []
    session = tonicSession.TonicSession(tonicToolState.TonicToolState())
    session.setStatusSink(lambda text, level="info": heard.append(
        (text, level)))
    if not session.activate("/Scalp", stage=stage):
        check(False, "import: a session binds the plane scalp (%r)" % heard)
        return
    try:
        _partCSession(folder, session, heard, pkgBridge)
    finally:
        session.deactivate()


def _partCSession(folder, session, heard, pkgBridge):
    from pxr import Usd, UsdGeom
    dll = session.dll
    model = session.model

    def lastLine():
        return heard[-1] if heard else ("", "")

    def tubes():
        return pkgBridge.readTubeIds(dll, model)

    # -- export with nothing to export ------------------------------------
    del heard[:]
    empty = os.path.join(folder, "empty.usda")
    check(not tubes() and session.exportCenterCurves(empty) is False and
          not os.path.exists(empty) and
          "no tubes to export" in lastLine()[0],
          "export: an empty model exports nothing and says so (%r)"
          % (lastLine(),))

    check(dll.Tonic_BuildTestTube(model, 0, 0, 0.0, 0.0) == 0,
          "import: the test tube builds over the scalp")
    before = tubes()
    depth = session.undoDepth()

    # -- a two-curve file whose second curve has one point ----------------
    bad = os.path.join(folder, "oneTooShort.usda")
    _writeCurves(bad, [[(1.0, 0.0, 1.0), (1.0, 0.5, 1.0), (1.0, 1.0, 1.0)],
                       [(3.0, 0.0, 3.0)]])
    del heard[:]
    try:
        ok = session.importCurves(bad, 0)
        raised = None
    except Exception as exc:  # noqa: BLE001 - the test names the escape
        ok, raised = None, exc
    check(raised is None and ok is False,
          "import: a one-point second curve returns False without raising "
          "(%r, %r)" % (ok, raised))
    check(tubes() == before and session.undoDepth() == depth,
          "import: and adds no tube and no undo step (%r -> %r)"
          % (before, tubes()))
    check(lastLine()[1] == "error" and "curve 1" in lastLine()[0],
          "import: the error names the bad curve (%r)" % (lastLine(),))

    # -- files that are not curves at all ---------------------------------
    notUsd = os.path.join(folder, "notes.txt")
    with open(notUsd, "w") as handle:
        handle.write("hello\n")
    broken = os.path.join(folder, "broken.usda")
    with open(broken, "w") as handle:
        handle.write("#usda 1.0\nthis is not { usd\n")
    noCurves = os.path.join(folder, "noCurves.usda")
    plain = Usd.Stage.CreateNew(noCurves)
    plain.DefinePrim("/Nothing", "Scope")
    plain.GetRootLayer().Save()
    del plain
    for path, what in ((os.path.join(folder, "missing.usda"), "a missing"),
                       (notUsd, "a non-USD"), (broken, "a malformed"),
                       (noCurves, "a curve-less"), (folder, "a folder as")):
        del heard[:]
        try:
            ok = session.importCurves(path, 0)
            raised = None
        except Exception as exc:  # noqa: BLE001
            ok, raised = None, exc
        check(ok is False and raised is None and tubes() == before and
              lastLine()[1] == "error",
              "import: %s file is refused with an error line (%r)"
              % (what, lastLine()))

    # -- a C++ refusal part-way through rolls back -------------------------
    good = os.path.join(folder, "twoCurves.usda")
    _writeCurves(good, [[(1.0, 0.0, 1.0), (1.0, 0.5, 1.1), (1.0, 1.0, 1.2)],
                        [(3.0, 0.0, 3.0), (3.0, 0.5, 3.1), (3.0, 1.0, 3.2),
                         (3.0, 1.5, 3.3)]])
    realImport = pkgBridge.importLockedTube
    calls = []

    def failSecond(*args, **kwargs):
        calls.append(1)
        if len(calls) == 2:
            raise RuntimeError("Tonic_ImportLockedTube failed with code 1")
        return realImport(*args, **kwargs)

    pkgBridge.importLockedTube = failSecond
    del heard[:]
    try:
        ok = session.importCurves(good, 0)
    finally:
        pkgBridge.importLockedTube = realImport
    check(ok is False and len(calls) == 2 and tubes() == before and
          session.undoDepth() == depth,
          "import: a failure on the second tube removes the first "
          "(%r -> %r, %d call(s))" % (before, tubes(), len(calls)))
    check(lastLine()[1] == "error" and "cannot import" in lastLine()[0],
          "import: and reports it (%r)" % (lastLine(),))

    # -- an unknown parent is the library's refusal, with its reason -------
    del heard[:]
    check(session.importCurves(good, 987) is False and tubes() == before,
          "import: an unknown parent imports nothing (%r)" % (lastLine(),))
    check(session.lastError() and session.lastError() in lastLine()[0],
          "import: the error line carries lastError() (%r)"
          % (lastLine(),))

    # -- the good file lands whole, as one undo step ------------------------
    del heard[:]
    check(session.importCurves(good, 0) is True and
          len(tubes()) == len(before) + 2 and
          session.undoDepth() == depth + 1,
          "import: two curves land as two tubes in one undo step "
          "(%r, depth %d)" % (tubes(), session.undoDepth()))
    check(lastLine() == ("Tonic: Imported 2 curves under tube 0", "info"),
          "import: the status says 'Imported 2 curves under tube 0' (%r)"
          % (lastLine(),))

    # -- export ------------------------------------------------------------
    del heard[:]
    check(session.exportCenterCurves(folder) is False and
          lastLine()[1] == "error" and "cannot write" in lastLine()[0],
          "export: a folder path is refused with a status (%r)"
          % (lastLine(),))
    del heard[:]
    textFile = os.path.join(folder, "centers.txt")
    check(session.exportCenterCurves(textFile) is False and
          not os.path.exists(textFile) and "cannot write" in lastLine()[0],
          "export: a non-USD extension is refused (%r)" % (lastLine(),))
    del heard[:]
    out = os.path.join(folder, "centers.usda")
    check(session.exportCenterCurves(out) is True and os.path.isfile(out),
          "export: a temp .usda is written (%r)" % (lastLine(),))
    reread = Usd.Stage.Open(out)
    found = [p for p in reread.Traverse() if p.IsA(UsdGeom.BasisCurves)]
    counts = (list(UsdGeom.BasisCurves(found[0])
                   .GetCurveVertexCountsAttr().Get()) if found else [])
    check(len(found) == 1 and len(counts) == len(tubes()),
          "export: the file parses with one BasisCurves, a curve per tube "
          "(%d prim(s), %d curve(s))" % (len(found), len(counts)))
    del heard[:]
    bare = os.path.join(folder, "bare")
    check(session.exportCenterCurves(bare) is True and
          os.path.isfile(bare + ".usda"),
          "export: a bare name gets .usda (%r)" % (lastLine(),))


def main():
    bridge = _load("tonicBridge")
    _partA(bridge)
    _partAImportValidation(bridge)

    # Part B needs pxr and the DLL. Under CTest (USDGENTONIC_DLL set)
    # both must be there; direct runs without them skip Part B.
    strict = bool(os.environ.get("USDGENTONIC_DLL", ""))
    try:
        import pxr.Usd  # noqa: F401
    except ImportError as exc:
        if strict:
            check(False, "pxr imports under CTest (%s)" % exc)
        else:
            skip("Part B: pxr is not importable (%s)" % exc)
        print("testUsdGenTonicToolsBridge: %d failure(s), %d skip(s)"
              % (failures, skips))
        return 1 if failures else 0
    try:
        tonicLib = _load("tonicLib")
        tonicLib.Library()
    except OSError as exc:
        if strict:
            check(False, "the DLL loads under CTest (%s)" % exc)
        else:
            skip("Part B: %s" % exc)
        print("testUsdGenTonicToolsBridge: %d failure(s), %d skip(s)"
              % (failures, skips))
        return 1 if failures else 0
    try:
        _partB(bridge, tonicLib)
    except Exception as exc:  # noqa: BLE001 - the test reports, not raises
        import traceback
        traceback.print_exc()
        check(False, "Part B raises no exception (%s)" % exc)
    try:
        _partC()
    except Exception as exc:  # noqa: BLE001 - the test reports, not raises
        import traceback
        traceback.print_exc()
        check(False, "Part C raises no exception (%s)" % exc)
    print("testUsdGenTonicToolsBridge: %d failure(s), %d skip(s)"
          % (failures, skips))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
