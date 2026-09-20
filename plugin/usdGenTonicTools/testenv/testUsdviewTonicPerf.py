# testUsdviewTonicPerf -- T3 STUB: the TN-1/TN-2 probes on the fixture,
# with the reference-scene gates (TN-4/TN-7) as skips (plan/17 S7).
# TN-3 lives in testUsdviewTonicPick.py (P6 exit).
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicPerf.py \
#               examples/tonic-graph-scalp.usda
#
# with the same environment testUsdviewTonicGraph.py uses. The probes time
# real C ABI work (Tube-mode moves, K11 picks) on the 4x4 fixture and gate
# it at the S7 budgets with ~1000x headroom, so they are green by
# construction here; the gates themselves run on the S7 reference scene
# (examples/tonic-reference.usda via examples/tools/make_tonic_reference.py
# --wish) and stay TODO until that harness exists.
#
# Proven here:
#   * a Tube-mode move (center CV edit + version poll) completes, timed;
#   * a K11 pick completes, timed;
#   * TN-4 (idle-swap budget) and TN-7 (bake isolation) skip with their
#     harness reasons.
import ctypes
import os
import sys
import time

failures = 0
skips = 0

PICK_GUIDE = 16

# S7 budgets. The fixture gates below reuse the numbers with enormous
# headroom; the reference-scene gates keep them exactly.
TN1_MOVE_MS = 8.0
TN2_PICK_MS = 0.5


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


def locate(x, z):
    """(face, u, v) for a scalp position (x, z)."""
    import math
    ix = min(max(int(math.floor(x)), 0), 3)
    iz = min(max(int(math.floor(z)), 0), 3)
    return (ix * 4 + iz, float(z - iz), float(x - ix))


def strokeRect(dll, model, corners, snap):
    """Stroke a closed rectangle; returns (rc, chain, closed)."""
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
    rc = dll.Tonic_GraphStroke(model, faces, uvs, len(samples),
                               ctypes.c_float(snap), ctypes.c_float(0.05),
                               out, 4096, ctypes.byref(count),
                               ctypes.byref(closed), ctypes.byref(weldStart),
                               ctypes.byref(weldEnd))
    return (rc, [out[i] for i in range(count.value)], bool(closed.value))


def run(stage):
    global failures
    try:
        here = os.path.dirname(os.path.abspath(__file__))
    except NameError:
        here = ""
    if here:
        sys.path.insert(0, os.path.normpath(os.path.join(
            here, "..", "python")))
    try:
        from usdGenTonicTools import tonicLib
    except ImportError as exc:
        print("FAIL: cannot import usdGenTonicTools: %s" % exc)
        return 1
    try:
        lib = tonicLib.Library()
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
    check(dll.Tonic_Create(ctypes.byref(model)) == 0, "model creates")
    check(dll.Tonic_BindScalp(model, pts, len(flat), cnt, len(counts),
                              idx, len(indices)) == 0, "scalp binds")
    rc, _, closed = strokeRect(
        dll, model, [(0.0, 1.0), (4.0, 1.0), (4.0, 3.0), (0.0, 3.0)], 0.1)
    check(rc == 0 and closed, "the region stroke closes")
    check(dll.Tonic_Rasterise(model) == 0, "K3 rasterises")
    check(dll.Tonic_BuildTubeFromRegion(model, 0, 5, 8, ctypes.c_float(2.0))
          == 0, "the tube builds")
    check(dll.Tonic_SetFillParams(model, ctypes.c_float(50.0), 8, 11,
                                  ctypes.c_float(0.0), None, 0) == 0,
          "fill params set")
    check(dll.Tonic_RefillGuides(model, ctypes.c_float(1.0)) == 0,
          "the refill runs")

    # -- TN-1 probe: one Tube-mode move -----------------------------------
    # A move is a center edit plus the version poll the shelf does after
    # every gesture step; the K6/K7 passes it will carry land with P4.
    moves = 200
    t0 = time.perf_counter()
    for i in range(moves):
        dll.Tonic_MoveCenterCV(model, 2, ctypes.c_float(0.01),
                               ctypes.c_float(0.0), ctypes.c_float(0.0))
        dll.Tonic_GetVersion(model)
    perMove = (time.perf_counter() - t0) / moves * 1000.0
    print("info: TN-1 probe (fixture): %.4f ms/move, budget %.1f ms "
          "(gate runs on the S7 reference scene)" % (perMove, TN1_MOVE_MS))
    check(perMove < TN1_MOVE_MS, "TN-1 probe under budget on the fixture")

    # -- TN-2 probe: one K11 pick ------------------------------------------
    guides = ctypes.c_int(0)
    cv = ctypes.c_int(0)
    dll.Tonic_GetGuideCounts(model, ctypes.byref(guides), ctypes.byref(cv))
    if guides.value > 0:
        xyz = (ctypes.c_float * (3 * guides.value * cv.value))()
        gcounts = (ctypes.c_int * guides.value)()
        got = ctypes.c_int(0)
        dll.Tonic_ReadGuidePreview(model, xyz, len(xyz), gcounts,
                                   len(gcounts), ctypes.byref(got))
        viewProj = (ctypes.c_float * 16)(1, 0, 0, 0, 0, 1, 0, 0,
                                         0, 0, 1, 0, 0, 0, 0, 1)
        px = (xyz[0] * 0.5 + 0.5) * 400.0
        py = (1.0 - (xyz[1] * 0.5 + 0.5)) * 400.0
        hit = ctypes.c_int(0)
        kind = ctypes.c_uint(0)
        index = ctypes.c_int(0)
        sub = ctypes.c_int(0)
        dist = ctypes.c_float(0)
        depth = ctypes.c_float(0)
        picks = 500
        t0 = time.perf_counter()
        for _ in range(picks):
            dll.Tonic_Pick(model, viewProj, 400, 400, ctypes.c_float(px),
                           ctypes.c_float(py), ctypes.c_float(5.0), PICK_GUIDE,
                           ctypes.byref(hit), ctypes.byref(kind),
                           ctypes.byref(index), ctypes.byref(sub),
                           ctypes.byref(dist), ctypes.byref(depth))
        perPick = (time.perf_counter() - t0) / picks * 1000.0
        print("info: TN-2 probe (fixture): %.4f ms/pick, budget %.1f ms "
              "(gate runs at 770 K tube verts)" % (perPick, TN2_PICK_MS))
        check(perPick < TN2_PICK_MS, "TN-2 probe under budget on the fixture")
        check(hit.value == 1, "the probe pick hits")
    else:
        skip("TN-2 probe: the refill yielded no guides")

    # -- reference-scene gates (stubs) --------------------------------------
    # TN-3 moved out: testUsdviewTonicPick.py gates K11 vs view.pick() at
    # 100 pixels (P6 exit); this stub keeps the fixture probes only.
    skip("TN-3 pick accuracy: see testUsdviewTonicPick")
    # TODO(TN-4): main-thread TransferContent of the reference groom in
    # <= 5 ms per idle slot; needs tonic-reference.usda at --wish scale
    # plus the idle-swap harness (the T1 TN-4 section in
    # tests/testUsdGenTonicCommit.cpp proves the slot budget meanwhile).
    skip("TN-4 swap: needs the S7 reference scene + idle-swap harness")
    # TODO(TN-7): a Tube-mode drag while a full bake is in flight; needs
    # the bake worker under the reference scalp.
    skip("TN-7 bake isolation: needs the reference bake in flight")

    dll.Tonic_Destroy(model)
    print("testUsdviewTonicPerf: %d failure(s), %d skip(s)"
          % (failures, skips))
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    model = getattr(appController, "_dataModel", appController)
    return run(model.stage)


if __name__ == "__main__":
    # Direct stage run (no usdview): python testUsdviewTonicPerf.py stage.usda
    from pxr import Usd
    if len(sys.argv) != 2:
        print("usage: testUsdviewTonicPerf.py <stage.usda>")
        sys.exit(2)
    sys.exit(run(Usd.Stage.Open(sys.argv[1])))
