# testPomadeAbiGraph -- T3: Graph mode draws two adjoining regions by
# stroke, welds them, and checks the shared boundary (plan/17 P2 exit).
#
# ABI-level by name since plan/18 V2: this script drives the C ABI
# directly and never touches the viewport, so it is a regression
# test for the model, not a proof that the tool works. The gesture
# proof for this mode is the matching testUsdviewPomade* script.
#
#   testusdview --testScript plugin/usdGenPomadeTools/testenv/testPomadeAbiGraph.py \
#               examples/pomade-graph-scalp.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the T2 editor test uses,
# plus the staged usdGenPomadeTools package and the usdGenPomade DLL (via
# USDGENPOMADE_DLL or the build tree). The scalp is the 4x4 quad grid in XZ:
# face f = ix*4 + iz covers x in [ix, ix+1], z in [iz, iz+1], and (x, z) on
# that face is (u = z - iz, v = x - ix).
#
# Proven here:
#   * stroke A (the left x in [0, 2], z in [1, 3] rectangle) closes into one
#     region;
#   * stroke B (the right rectangle) welds both ends to A's corners;
#   * removing the one duplicate edge leaves two regions sharing exactly one
#     boundary (watertight: every edge appears in a region loop);
#   * rasterise claims 4 + 4 faces with 8 uncovered and none intersected.
import ctypes
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


def locate(x, z):
    """(face, u, v) for a scalp position (x, z)."""
    import math
    ix = min(max(int(math.floor(x)), 0), 3)
    iz = min(max(int(math.floor(z)), 0), 3)
    return (ix * 4 + iz, float(z - iz), float(x - ix))


def strokeRect(dll, model, corners, snap):
    """Stroke a closed rectangle; returns (nodeIds, closed, weldStart, ...)."""
    samples = []
    per = 5
    for k in range(len(corners)):
        x0, z0 = corners[k]
        x1, z1 = corners[(k + 1) % len(corners)]
        for i in range(per):
            t = float(i) / float(per)
            samples.append((x0 + (x1 - x0) * t, z0 + (z1 - z0) * t))
    samples.append(corners[0])
    faces = (ctypes.c_int * len(samples))()
    uvs = (ctypes.c_float * (2 * len(samples)))()
    for i, (x, z) in enumerate(samples):
        face, u, v = locate(x, z)
        faces[i] = face
        uvs[2 * i] = u
        uvs[2 * i + 1] = v
    out = (ctypes.c_int * 4096)()
    count = ctypes.c_int(0)
    closed = ctypes.c_int(0)
    weldStart = ctypes.c_int(0)
    weldEnd = ctypes.c_int(0)
    rc = dll.Pomade_GraphStroke(model, faces, uvs, len(samples),
                               ctypes.c_float(snap), ctypes.c_float(0.05),
                               out, 4096, ctypes.byref(count),
                               ctypes.byref(closed), ctypes.byref(weldStart),
                               ctypes.byref(weldEnd))
    return (rc, [out[i] for i in range(count.value)], bool(closed.value),
            bool(weldStart.value), bool(weldEnd.value))


def readLoops(dll, model):
    rc = ctypes.c_int(0)
    ic = ctypes.c_int(0)
    dll.Pomade_ReadRegionLoops(model, None, 0, None, 0, ctypes.byref(rc),
                              ctypes.byref(ic))
    counts = (ctypes.c_int * max(rc.value, 1))()
    indices = (ctypes.c_int * max(ic.value, 1))()
    dll.Pomade_ReadRegionLoops(model, counts, max(rc.value, 1), indices,
                              max(ic.value, 1), ctypes.byref(rc),
                              ctypes.byref(ic))
    loops = []
    o = 0
    for r in range(rc.value):
        loops.append([indices[o + k] for k in range(counts[r])])
        o += counts[r]
    return loops


def readEdges(dll, model):
    count = ctypes.c_int(0)
    dll.Pomade_ReadGraphEdges(model, None, 0, ctypes.byref(count))
    pairs = (ctypes.c_int * max(2 * count.value, 2))()
    dll.Pomade_ReadGraphEdges(model, pairs, max(count.value, 1),
                             ctypes.byref(count))
    return [(pairs[2 * i], pairs[2 * i + 1]) for i in range(count.value)]


def run(stage):
    global failures
    # Under testusdview the script is exec'd (no __file__) but the staged
    # package is already on PYTHONPATH; the insert only helps direct runs.
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, os.path.normpath(os.path.join(
            here, "..", "python")))
    try:
        from usdGenPomadeTools import pomadeLib
    except ImportError as exc:
        print("FAIL: cannot import usdGenPomadeTools: %s" % exc)
        return 1
    try:
        lib = pomadeLib.Library()
    except OSError as exc:
        print("FAIL: %s" % exc)
        return 1
    dll = lib.dll

    scalp = stage.GetPrimAtPath("/Scalp")
    check(scalp, "the stage carries /Scalp")
    if not scalp:
        return 1
    points = scalp.GetAttribute("points").Get()
    counts = scalp.GetAttribute("faceVertexCounts").Get()
    indices = scalp.GetAttribute("faceVertexIndices").Get()
    flat = [float(c) for p in points for c in (p[0], p[1], p[2])]
    pts = (ctypes.c_float * len(flat))(*flat)
    cnt = (ctypes.c_int * len(counts))(*[int(c) for c in counts])
    idx = (ctypes.c_int * len(indices))(*[int(i) for i in indices])

    model = ctypes.c_void_p(None)
    check(dll.Pomade_Create(ctypes.byref(model)) == 0, "model creates")
    check(dll.Pomade_BindScalp(model, pts, len(flat), cnt, len(counts),
                              idx, len(indices)) == 0, "scalp binds")
    dll.Pomade_SetSnapRadius(model, ctypes.c_float(0.1))

    # Stroke A: x in [0, 2], z in [1, 3].
    rc, chainA, closedA, _, _ = strokeRect(
        dll, model, [(0.0, 1.0), (2.0, 1.0), (2.0, 3.0), (0.0, 3.0)], 0.1)
    check(rc == 0 and closedA, "stroke A closes into a region")
    loops = readLoops(dll, model)
    check(len(loops) == 1 and len(loops[0]) == 4,
          "one 4-node region after stroke A (got %r)" % (loops,))

    # Stroke B: x in [2, 4], z in [1, 3]; both ends weld to A's corners.
    rc, chainB, closedB, weldStart, weldEnd = strokeRect(
        dll, model, [(2.0, 1.0), (4.0, 1.0), (4.0, 3.0), (2.0, 3.0)], 0.1)
    check(rc == 0 and closedB, "stroke B closes")
    check(weldStart and weldEnd, "stroke B welds both ends to A")
    loops = readLoops(dll, model)
    check(len(loops) == 2, "two regions after stroke B (got %d)"
          % len(loops))

    # B's closing edge would duplicate A's shared side, so Connect refuses
    # it (already connected) and B's loop closes over A's edge instead: 4 +
    # 3 edges, one of them shared. That refusal is the watertight weld.
    nodes = ctypes.c_int(0)
    edges = ctypes.c_int(0)
    regions = ctypes.c_int(0)
    dll.Pomade_GetGraphCounts(model, ctypes.byref(nodes), ctypes.byref(edges),
                             ctypes.byref(regions))
    check(nodes.value == 6 and edges.value == 7 and regions.value == 2,
          "6 nodes, 7 edges, 2 regions (got %d/%d/%d)"
          % (nodes.value, edges.value, regions.value))
    loops = readLoops(dll, model)
    shared = set(loops[0]) & set(loops[1]) if len(loops) == 2 else set()
    check(len(loops) == 2 and len(shared) == 2,
          "the regions share exactly their boundary nodes (shared %r)"
          % (sorted(shared),))
    # Watertight: every alive edge appears in some region loop.
    loopPairs = set()
    for loop in loops:
        for k in range(len(loop)):
            a, b = loop[k], loop[(k + 1) % len(loop)]
            loopPairs.add((min(a, b), max(a, b)))
    pairs = readEdges(dll, model)
    orphans = [pr for pr in pairs
               if (min(pr[0], pr[1]), max(pr[0], pr[1])) not in loopPairs]
    check(len(pairs) == 7 and len(orphans) == 0,
          "every edge bounds a region (watertight)")

    check(dll.Pomade_Rasterise(model) == 0, "K3 rasterises")
    regions = ctypes.c_int(0)
    uncovered = ctypes.c_int(0)
    intersected = ctypes.c_int(0)
    dll.Pomade_GetRegionStats(model, ctypes.byref(regions),
                             ctypes.byref(uncovered), ctypes.byref(intersected))
    check(regions.value == 2, "two regions rasterised")
    check(uncovered.value == 8, "8 faces uncovered (got %d)"
          % uncovered.value)
    check(intersected.value == 0, "no root intersections")

    dll.Pomade_Destroy(model)
    print("testPomadeAbiGraph: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    model = getattr(appController, "_dataModel", appController)
    return run(model.stage)


if __name__ == "__main__":
    # Direct stage run (no usdview): python testPomadeAbiGraph.py stage.usda
    from pxr import Usd
    if len(sys.argv) != 2:
        print("usage: testPomadeAbiGraph.py <stage.usda>")
        sys.exit(2)
    sys.exit(run(Usd.Stage.Open(sys.argv[1])))
