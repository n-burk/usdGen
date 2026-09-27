# T1 -- the Qt-free backends of the usdview brush tool: the tool-side paint
# map and stroke twin (hardness, erase), LiveStroke (native C ABI and pure
# Python, including the cross-face corner smooth), the camera maths, the
# surface pick (both backends), bind/bake authoring with auto texel
# density, the preview client (session path; the Hydra overlay itself is
# covered by the C++ testUsdGenBrushApi), loop smoke tests, and GeomSubset
# face masks (both backends agree; subset-bound descriptions paint, smooth,
# live-write and flood their own faces only). The loop
# cadence, panel descriptors, hotkeys and palette are the UI suite's
# (testUsdGenToolsBrushUI.py).
#
# Run as:  python testUsdGenToolsBrush.py <path to test-plane.usda>
# CMake registers it as testUsdGenToolsBrush. Nothing is written to disk:
# the stage is opened and edited in memory and never saved.

import math
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, Vt

from usdGenTools import (brushApi, brushAuthor, brushCamera, brushLoop,
                         brushMap, brushPanels, brushPick, brushPreview,
                         brushState)

FAILURES = []


def check(condition, message):
    if condition:
        print("ok: " + message)
    else:
        print("FAIL: " + message)
        FAILURES.append(message)


def near(a, b, tolerance=1e-6):
    return abs(a - b) <= tolerance


def spec(faces=1, res=8, channels=1):
    return brushMap.BrushMapSpec(faces, res, channels, 0.0, False)


# -- the map ------------------------------------------------------------

def testMapSpec():
    grid, error = brushMap.BrushMap.create(spec(0))
    check(grid is None and error, "numFaces 0 is rejected")
    grid, _error = brushMap.BrushMap.create(spec(1, 3))
    check(grid is None, "resolution 3 (non-pow2) is rejected")
    grid, _error = brushMap.BrushMap.create(spec(1, 512))
    check(grid is None, "resolution 512 is rejected")
    grid, _error = brushMap.BrushMap.create(spec(1, 0))
    check(grid is None, "resolution 0 is rejected")
    grid, _error = brushMap.BrushMap.create(spec(1, 8, 2))
    check(grid is None, "channels 2 is rejected")
    bad = spec()
    bad.defaultValue = float("nan")
    grid, _error = brushMap.BrushMap.create(bad)
    check(grid is None, "NaN default is rejected")

    grid, error = brushMap.BrushMap.create(spec(4, 16, 3))
    check(grid is not None and not error, "a valid spec opens")
    check((grid.numFaces(), grid.resolution(), grid.channels())
          == (4, 16, 3), "the map reports its spec")
    check(grid.floatCount() == 4 * 16 * 16 * 3, "FloatCount")


def testTexels():
    grid, _error = brushMap.BrushMap.create(spec(2, 8))
    ok, value = grid.getTexel(0, 0, 0, 0)
    check(ok and value == 0.0, "a fresh texel is the default")
    check(grid.setTexel(1, 7, 7, 0, 2.5), "SetTexel in range")
    ok, value = grid.getTexel(1, 7, 7, 0)
    check(ok and value == 2.5, "GetTexel reads back")
    check(not grid.getTexel(2, 0, 0, 0)[0], "face past the end rejected")
    check(not grid.getTexel(0, 8, 0, 0)[0], "texel s past the end rejected")
    check(not grid.getTexel(0, 0, -1, 0)[0], "negative texel t rejected")
    check(not grid.getTexel(0, 0, 0, 1)[0], "channel past the end rejected")
    check(not grid.setTexel(0, 0, 0, 0, float("nan")),
          "a NaN write is rejected")
    grid.fill(0.25)
    check(grid.getTexel(1, 7, 7, 0) == (True, 0.25), "Fill overwrites")
    grid.fill(float("nan"))
    check(grid.getTexel(0, 0, 0, 0) == (True, 0.25),
          "a NaN Fill is ignored")

    clamped = spec()
    clamped.defaultValue = 2.0
    clamped.clamp01 = True
    grid, _error = brushMap.BrushMap.create(clamped)
    check(grid.getTexel(0, 0, 0, 0) == (True, 1.0),
          "the default clamps at create")
    check(grid.setTexel(0, 1, 1, 0, -3.0), "a clamped write is accepted")
    check(grid.getTexel(0, 1, 1, 0) == (True, 0.0),
          "the write clamps to [0, 1]")

    clone = grid.clone()
    clone.setTexel(0, 1, 1, 0, 0.5)
    check(grid.getTexel(0, 1, 1, 0) == (True, 0.0),
          "a clone shares no storage with its original")


def testSampling():
    grid, _error = brushMap.BrushMap.create(spec(1, 2))
    grid.setTexel(0, 0, 0, 0, 0.0)
    grid.setTexel(0, 1, 0, 0, 1.0)
    grid.setTexel(0, 0, 1, 0, 2.0)
    grid.setTexel(0, 1, 1, 0, 3.0)
    check(grid.sample(0, 0.1, 0.1, 0, brushMap.INTERP_NEAREST)
          == (True, 0.0), "nearest corner")
    check(grid.sample(0, 0.9, 0.1, 0, brushMap.INTERP_NEAREST)
          == (True, 1.0), "nearest picks the texel under (u, v)")
    ok, value = grid.sample(0, 0.5, 0.5, 0, brushMap.INTERP_BILINEAR)
    check(ok and near(value, 1.5), "bilinear centre is the corner mean")
    check(grid.sample(0, 0.0, 0.0, 0, brushMap.INTERP_BILINEAR)
          == (True, 0.0), "bilinear corner is exact")
    check(grid.sample(0, 1.0, 1.0, 0, brushMap.INTERP_BILINEAR)
          == (True, 3.0), "bilinear far corner is exact")
    check(grid.sample(0, -2.0, 99.0, 0, brushMap.INTERP_BILINEAR)
          == (True, 2.0), "(u, v) clamps to [0, 1]")
    nodes, _error = brushMap.BrushMap.create(spec(1, 4))
    nodes.setTexel(0, 1, 1, 0, 1.0)
    ok, exact = nodes.sample(0, 1.0 / 3.0, 1.0 / 3.0, 0,
                             brushMap.INTERP_BILINEAR)
    check(ok and exact == 1.0,
          "bilinear is exact on interior grid nodes (%.6f)" % exact)
    ok, between = nodes.sample(0, 0.5, 0.5, 0, brushMap.INTERP_BILINEAR)
    check(ok and near(between, 0.25, 1e-9),
          "and blends evenly between them (%.6f)" % between)
    check(not grid.sample(1, 0.5, 0.5, 0)[0],
          "a face past the end is rejected")
    check(not grid.sample(0, float("nan"), 0.5, 0)[0],
          "a NaN u is rejected")


# -- dab maths ----------------------------------------------------------

def testDabMath():
    grid, _error = brushMap.BrushMap.create(spec(1, 8))
    stroke = brushMap.BrushStroke(grid)
    ok, error = stroke.addDab(brushMap.BrushDab(
        0, 0.5, 0.5, radius=0.25, strength=1.0, value=1.0))
    check(ok and not error, "a valid dab records")
    live = stroke.preview()
    _ok, centre = live.getTexel(0, 3, 3, 0)
    _ok, corner = live.getTexel(0, 0, 0, 0)
    check(centre > 0.5, "the dab paints its centre (%.3f)" % centre)
    check(corner == 0.0, "a texel past the radius is untouched")
    _ok, edge = live.getTexel(0, 5, 5, 0)
    _ok, middle = live.getTexel(0, 5, 4, 0)
    check(centre >= middle >= edge,
          "smooth falloff is monotonic centre to rim")
    check(grid.getTexel(0, 3, 3, 0) == (True, 0.0),
          "Preview leaves the base untouched")

    # Radius 0 paints one texel at full strength, Smooth excepted.
    grid, _error = brushMap.BrushMap.create(spec(1, 8))
    stroke = brushMap.BrushStroke(grid)
    stroke.addDab(brushMap.BrushDab(0, 0.5, 0.5, radius=0.0,
                                    strength=0.5, value=1.0))
    live = stroke.preview()
    check(live.getTexel(0, 4, 4, 0) == (True, 0.5),
          "a radius-0 Set dab hits one texel")
    check(live.getTexel(0, 3, 4, 0) == (True, 0.0),
          "and its neighbour")
    stroke.abort()
    stroke.addDab(brushMap.BrushDab(0, 0.5, 0.5, radius=0.0,
                                    strength=0.5, value=1.0,
                                    mode=brushMap.MODE_SMOOTH))
    check(stroke.preview().getTexel(0, 4, 4, 0) == (True, 0.0),
          "a radius-0 Smooth dab is the identity")

    # Add offsets, strength 0 is a no-op, Smooth relaxes a spike.
    grid, _error = brushMap.BrushMap.create(spec(1, 8))
    dab = brushMap.BrushDab(0, 0.5, 0.5, radius=0.2, strength=1.0,
                            value=0.25, mode=brushMap.MODE_ADD)
    once = brushMap.BrushStroke(grid)
    once.addDab(dab)
    twice = brushMap.BrushStroke(grid)
    twice.addDab(dab)
    twice.addDab(dab)
    _ok, v1 = once.preview().getTexel(0, 4, 4, 0)
    _ok, v2 = twice.preview().getTexel(0, 4, 4, 0)
    check(v1 > 0.1 and near(v2, 2.0 * v1, 1e-9),
          "two Add dabs accumulate exactly twice one (%.4f)" % v2)
    stroke = twice
    stroke.abort()
    stroke.addDab(brushMap.BrushDab(0, 0.5, 0.5, radius=0.2,
                                    strength=0.0, value=1.0))
    check(stroke.preview().getTexel(0, 4, 4, 0) == (True, 0.0),
          "a strength-0 dab is a no-op")

    grid.setTexel(0, 4, 4, 0, 1.0)
    stroke = brushMap.BrushStroke(grid)
    # The dab sits exactly on texel (4, 4)'s grid node, so k == 1.
    stroke.addDab(brushMap.BrushDab(0, 4.0 / 7.0, 4.0 / 7.0, radius=0.2,
                                    strength=1.0, value=0.0,
                                    mode=brushMap.MODE_SMOOTH))
    _ok, relaxed = stroke.preview().getTexel(0, 4, 4, 0)
    check(near(relaxed, 1.0 / 9.0, 1e-6),
          "Smooth relaxes a spike toward its 3x3 mean (%.4f)" % relaxed)

    # Validation fails closed.
    for bad, what in [
            (brushMap.BrushDab(7, 0.5, 0.5), "a face past the end"),
            (brushMap.BrushDab(0, float("nan"), 0.5), "a NaN u"),
            (brushMap.BrushDab(0, 0.5, 0.5, radius=-1.0),
             "a negative radius"),
            (brushMap.BrushDab(0, 0.5, 0.5, strength=2.0),
             "a strength past 1"),
            (brushMap.BrushDab(0, 0.5, 0.5, channel=5),
             "a channel past the end"),
            (brushMap.BrushDab(0, 0.5, 0.5, value=float("inf")),
             "an infinite value")]:
        ok, error = stroke.addDab(bad)
        check(not ok and error, "%s is rejected" % what)


def testStrokeContract():
    grid, _error = brushMap.BrushMap.create(spec(1, 8))
    stroke = brushMap.BrushStroke(grid)
    stroke.addDab(brushMap.BrushDab(0, 0.1, 0.1, radius=0.1,
                                    strength=0.5, value=1.0))
    ok, error = stroke.addMove(0, 0.9, 0.1, 0.1, 0.5, 1.0, -1,
                               brushMap.MODE_SET,
                               brushMap.FALLOFF_SMOOTH, 0.5)
    check(ok and not error, "a move interpolates")
    dabs = [stroke.dab(i) for i in range(stroke.dabCount())]
    gaps = [math.sqrt((b.u - a.u) ** 2 + (b.v - a.v) ** 2)
            for a, b in zip(dabs, dabs[1:])]
    check(dabs[0].u == 0.1 and abs(dabs[-1].u - 0.9) < 1e-6,
          "the trail runs previous centre to endpoint")
    check(max(gaps) <= 0.05 + 1e-6,
          "no gap exceeds spacing * radius (worst %.4f)" % max(gaps))

    before = stroke.dabCount()
    ok, error = stroke.addMove(0, 0.9, 0.9, 0.1, 0.5, 1.0, -1,
                               brushMap.MODE_SET,
                               brushMap.FALLOFF_SMOOTH, 0.0)
    check(not ok and error and stroke.dabCount() == before,
          "a rejected move records nothing")

    p1 = stroke.preview()
    p2 = stroke.preview()
    same = all(p1.getTexel(0, s, t, 0) == p2.getTexel(0, s, t, 0)
               for s in range(8) for t in range(8))
    check(same, "Preview is pure: twice yields identical maps")
    committed = stroke.commit()
    same = all(committed.getTexel(0, s, t, 0)
               == p1.getTexel(0, s, t, 0)
               for s in range(8) for t in range(8))
    check(same, "Commit is the same recomputed map as Preview")
    stroke.abort()
    check(stroke.dabCount() == 0, "Abort forgets every dab")
    check(stroke.preview().getTexel(0, 4, 4, 0) == (True, 0.0),
          "and the base was never touched")

    stroke.addDab(brushMap.BrushDab(0, 0.0, 0.0, radius=0.01,
                                    strength=1.0, value=1.0))
    stroke.addMove(0, 1.0, 1.0, 0.01, 1.0, 1.0, -1,
                   brushMap.MODE_SET, brushMap.FALLOFF_SMOOTH, 0.01)
    check(stroke.dabCount() <= 1 + 1024, "a wild move stamps a bounded "
          "trail (%d)" % stroke.dabCount())


# -- camera + pick ------------------------------------------------------

def testCameraMath():
    viewProj = (0.5, 0.0, 0.0, 0.0,
                0.0, 0.0, 0.5, 0.0,
                0.0, 0.5, 0.0, 0.0,
                0.0, 0.0, 0.5, 1.0)
    camera = brushCamera.BrushCamera(viewProj, 400, 400)
    check(camera.invertible, "the fixture matrix inverts")
    projected = camera.worldToPixels((0.0, 0.0, 0.0))
    check(projected is not None and near(projected[0], 200.0)
          and near(projected[1], 200.0),
          "the origin projects to the centre")
    inv = brushCamera.matInverse(viewProj)
    identity = brushCamera.matMul(viewProj, inv)
    check(all(near(identity[i * 4 + j], 1.0 if i == j else 0.0, 1e-9)
              for i in range(4) for j in range(4)),
          "matMul by matInverse is the identity")
    check(brushCamera.matInverse((0.0,) * 16) is None,
          "a singular matrix has no inverse")
    try:
        brushCamera.BrushCamera((1.0,) * 15, 400, 400)
        check(False, "15 floats are rejected")
    except ValueError:
        check(True, "15 floats are rejected")
    ray = camera.rayThrough(260.0, 130.0)
    check(ray is not None and near(ray[0][0], 0.6)
          and near(ray[0][2], 0.7),
          "rayThrough unprojects the pixel (%.2f, %.2f)"
          % (ray[0][0], ray[0][2]))


def testPick():
    snapshot = brushPick.MeshSnapshot(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
         (1.0, 0.0, 1.0), (0.0, 0.0, 1.0)],
        [(0, 1, 2, 3)])
    hit = brushPick.pickFace(snapshot, (0.25, 1.0, 0.75), (0.0, -1.0, 0.0))
    check(hit is not None and hit[0] == 0 and near(hit[1], 0.25)
          and near(hit[2], 0.75),
          "a ray from above reports face 0 at (0.25, 0.75), got %r"
          % ((hit[:3] if hit else None),))
    hit = brushPick.pickFace(snapshot, (0.25, -1.0, 0.75), (0.0, 1.0, 0.0))
    check(hit is not None and hit[0] == 0,
          "the pick is two-sided (a ray from below hits)")
    check(brushPick.pickFace(snapshot, (5.0, 1.0, 5.0), (0.0, -1.0, 0.0))
          is None, "a ray off the mesh misses")
    check(brushPick.pickFace(snapshot, (0.25, 1.0, 0.75), (0.0, 1.0, 0.0))
          is None, "a ray pointing away misses")
    check(brushPick.pickFace(None, (0.0, 0.0, 0.0), (0.0, -1.0, 0.0))
          is None, "a null snapshot misses")
    check(near(brushPick.movedPixels((0.0, 0.0), (3.0, 4.0)), 5.0),
          "movedPixels measures physical pixels")


def testPickIndex():
    _pythonOnly(_testPickIndex)


def _pythonOnly(fn, *args):
    """Run fn with the brush ABI forced off (the pure-Python backends)."""
    brushApi.reset(native=False)
    try:
        return fn(*args)
    finally:
        brushApi.reset(native=True)


def _testPickIndex():
    points, faces = [], []
    slot = {}

    def vert(x, z):
        if (x, z) not in slot:
            slot[(x, z)] = len(points)
            points.append((float(x), 0.0, float(z)))
        return slot[(x, z)]

    for iz in range(6):
        for ix in range(6):
            faces.append((vert(ix, iz), vert(ix + 1, iz),
                          vert(ix + 1, iz + 1), vert(ix, iz + 1)))
    base = len(points)
    points.extend([(10.0, 5.0, 10.0), (11.0, 5.0, 10.0),
                   (11.0, 5.0, 11.0), (10.0, 5.0, 11.0)])
    faces.append((base, base + 1, base + 2, base + 3))
    snapshot = brushPick.MeshSnapshot(points, faces)
    rays = []
    for ix in range(7):
        for iz in range(7):
            rays.append((((ix + 0.25), 5.0, (iz + 0.75)),
                         (0.0, -1.0, 0.0)))
    rays.extend([
        ((0.5, 5.0, 0.5), (0.0, -1.0, 0.0)),
        ((1.0, 5.0, 1.0), (0.0, -1.0, 0.0)),
        ((10.5, 9.0, 10.5), (0.0, -1.0, 0.0)),
        ((0.5, -1.0, 0.5), (0.0, 1.0, 0.0)),
        ((0.5, 1.0, 0.5), (1.0, 0.0, 0.0)),
        ((50.0, 5.0, 50.0), (0.0, -1.0, 0.0)),
        ((0.5, 5.0, 0.5), (0.0, 1.0, 0.0)),
        ((0.5, 5.0, 0.5), (0.0, 0.0, 0.0)),
    ])
    mismatches = 0
    for origin, direction in rays:
        want = brushPick._pickBrute(snapshot, origin, direction)
        got = brushPick.pickFace(snapshot, origin, direction)
        if got != want:
            mismatches += 1
    check(mismatches == 0,
          "the index agrees with brute force on %d rays (%d differ)"
          % (len(rays), mismatches))
    grid = brushPick._gridFor(snapshot)
    check(grid is not None, "the snapshot carries its grid")
    tight = grid.raycast((0.5, 5.0, 0.5), (0.0, -1.0, 0.0))
    check(len(tight) < len(faces) and 0 in tight,
          "a tight ray tests %d of %d faces" % (len(tight), len(faces)))
    foot = brushPick.DabFootprint(snapshot)
    saved = brushPick._gridFor
    samples = [(0, 0.9, 0.5, (0.9, 0.0, 0.5), 0.3),
               (7, 0.5, 0.5, (1.5, 0.0, 1.5), 1.5),
               (36, 0.5, 0.5, (10.5, 5.0, 10.5), 2.0)]
    drift = 0
    try:
        for face, u, v, point, radius in samples:
            indexed = foot.expand(face, u, v, point, radius)
            brushPick._gridFor = lambda _snapshot: None
            brute = foot.expand(face, u, v, point, radius)
            brushPick._gridFor = saved
            if indexed != brute:
                drift += 1
    finally:
        brushPick._gridFor = saved
    check(drift == 0,
          "footprints match brute force on %d dabs (%d differ)"
          % (len(samples), drift))
    lone = brushPick.MeshSnapshot(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
         (1.0, 0.0, 1.0), (0.0, 0.0, 1.0)],
        [(0, 1, 2, 3)])
    check(brushPick.pickFace(lone, (0.25, 1.0, 0.75), (0.0, -1.0, 0.0))
          == brushPick._pickBrute(lone, (0.25, 1.0, 0.75),
                                  (0.0, -1.0, 0.0)),
          "a one-face snapshot agrees too")


def testRing():
    viewProj = (0.5, 0.0, 0.0, 0.0,
                0.0, 0.0, 0.5, 0.0,
                0.0, 0.5, 0.0, 0.0,
                0.0, 0.0, 0.5, 1.0)
    camera = brushCamera.BrushCamera(viewProj, 400, 400)
    snapshot = brushPick.MeshSnapshot(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
         (1.0, 0.0, 1.0), (0.0, 0.0, 1.0)],
        [(0, 1, 2, 3)])
    ring = brushPick.ringPixels(snapshot, camera, 250.0, 150.0, 0.2)
    check(ring is not None, "a hovered hit reports a ring, got %r"
          % (ring,))
    check(near(ring[0], 250.0, 0.5) and near(ring[1], 150.0, 0.5),
          "the ring centres on the cursor (%.2f, %.2f)"
          % (ring[0], ring[1]))
    check(near(ring[2], 75.0, 0.5),
          "a sub-corner radius floors to the corner-reach disk (%.2f)"
          % ring[2])
    wide = brushPick.ringPixels(snapshot, camera, 250.0, 150.0, 1.0)
    check(wide is not None and near(wide[2], 100.0, 0.5),
          "while 1.0 world passes through (%.2f)"
          % (wide[2] if wide else -1.0))
    check(brushPick.ringPixels(snapshot, camera, 10.0, 10.0, 0.2) is None,
          "a miss hides the ring")
    dot = brushPick.ringPixels(snapshot, camera, 250.0, 150.0, 0.0)
    check(dot is not None and near(dot[2], 75.0, 0.5),
          "a zero radius still draws the floored disk (%.2f)"
          % (dot[2] if dot else -1.0))
    check(brushPick.ringPixels(None, camera, 250.0, 150.0, 0.2) is None,
          "a null snapshot hides the ring")
    check(brushPick.ringPixels(snapshot, None, 250.0, 150.0, 0.2) is None,
          "a null camera hides the ring")
    check(brushPick.ringPixels(snapshot, camera, 250.0, 150.0,
                               float("nan")) is None,
          "a NaN radius hides the ring")


def testExpand():
    snapshot = brushPick.MeshSnapshot(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0),
         (1.0, 0.0, 1.0), (0.0, 0.0, 1.0),
         (1.0, 0.0, 0.0), (2.0, 0.0, 0.0),
         (2.0, 0.0, 1.0), (1.0, 0.0, 1.0)],
        [(0, 1, 2, 3), (4, 5, 6, 7)])
    check(brushPick.facePoint(snapshot, 0, 0.25, 0.75)
          == (0.25, 0.0, 0.75),
          "facePoint interpolates the corners")
    check(brushPick.facePoint(snapshot, 9, 0.5, 0.5) is None,
          "facePoint fails closed off-range")
    check(brushPick.facePoint(None, 0, 0.5, 0.5) is None,
          "and on a null snapshot")
    footprint = brushPick.DabFootprint(snapshot)
    stamps = footprint.expand(0, 0.9, 0.5, (0.9, 0.0, 0.5), 0.2)
    check(len(stamps) == 2 and stamps[0] == (0, 0.9, 0.5, 0.2),
          "near an edge the footprint spills, primary first (got %r)"
          % (stamps,))
    check(stamps[1][0] == 1 and near(stamps[1][1], -0.1, 1e-6)
          and near(stamps[1][2], 0.5) and near(stamps[1][3], 0.2),
          "onto the neighbour, centred where the dab really is in its "
          "frame (u = -0.1, got %r)" % (stamps[1],))
    check(len(footprint.expand(0, 0.5, 0.5, (0.5, 0.0, 0.5), 0.2)) == 1,
          "mid-face stays on one face")
    check(len(footprint.expand(0, 0.9, 0.5, (0.9, 0.0, 0.5), 0.0)) == 1,
          "a point footprint off the edge stays home")
    degenerate = brushPick.MeshSnapshot(
        snapshot.points + [(0.9, 0.0, 0.5)] * 4,
        snapshot.faces + [(8, 9, 10, 11)])
    stamps = brushPick.DabFootprint(degenerate).expand(
        0, 0.9, 0.5, (0.9, 0.0, 0.5), 0.2)
    check(len(stamps) == 2 and stamps[1][0] == 1,
          "a degenerate neighbour is skipped (got %r)" % (stamps,))
    grid, _error = brushMap.BrushMap.create(spec(2, 8))
    stroke = brushMap.BrushStroke(grid)
    stroke.addDab(brushMap.BrushDab(0, 0.9, 0.5, radius=0.2,
                                    strength=1.0, value=1.0))

    def expand(dab):
        point = brushPick.facePoint(snapshot, dab.face, dab.u, dab.v)
        out = []
        for face, u, v, radius in footprint.expand(
                dab.face, dab.u, dab.v, point, dab.radius):
            stamp = dab.copy()
            stamp.face, stamp.u, stamp.v, stamp.radius = (
                face, u, v, radius)
            out.append(stamp)
        return out

    bare = stroke.commit()
    check(all(bare.getTexel(1, s, t, 0) == (True, 0.0)
              for s in range(8) for t in range(8)),
          "a bare commit never leaves the picked face")
    grown = stroke.commit(expand=expand)
    hit = [grown.getTexel(1, s, t, 0)[1]
           for s in range(8) for t in range(8)]
    check(max(hit) > 0.1,
          "an expanded commit paints the neighbour (max %.3f)"
          % max(hit))
    # The shared border column sits at the same world distance from the
    # dab on both faces, so the falloff is continuous across it.
    seam = max(abs(grown.getTexel(0, 7, t, 0)[1]
                   - grown.getTexel(1, 0, t, 0)[1]) for t in range(8))
    check(seam < 1e-6,
          "the falloff is continuous across the face border (max |d| %.2e)"
          % seam)
    grown2 = stroke.preview(expand=expand)
    check(all(grown.getTexel(f, s, t, 0) == grown2.getTexel(f, s, t, 0)
              for f in range(2) for s in range(8) for t in range(8)),
          "expanded preview and commit agree")


# -- bind ---------------------------------------------------------------

def testBind(stagePath):
    from pxr import Usd
    stage = Usd.Stage.Open(stagePath)
    check(stage is not None, "the plane stage opens")
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)

    binding, error = brushAuthor.BindSurface(stage, "/Plane", resolution=8)
    check(binding is not None and not error,
          "Bind defines the PaintMap (%s)" % (error or binding.mapPath))
    prim = stage.GetPrimAtPath(binding.mapPath)
    check(prim.GetTypeName() == "UsdGenPaintMap",
          "the new prim is typed UsdGenPaintMap")
    targets = prim.GetRelationship("usdGen:paint:surface").GetTargets()
    check(targets == [Sdf.Path("/Plane")],
          "usdGen:paint:surface targets the mesh")
    check((prim.GetAttribute("usdGen:paint:primvar").Get(),
           prim.GetAttribute("usdGen:paint:interpolation").Get(),
           prim.GetAttribute("usdGen:paint:resolution").Get())
          == ("usdGen:paint:density", "faceVarying", 8),
          "the paint properties are authored")

    again, error = brushAuthor.BindSurface(stage, "/Plane", resolution=8)
    check(again is not None and not again.created,
          "rebinding the same surface reuses the map")

    clash, error = brushAuthor.BindSurface(stage, "/Plane", mapName="other")
    check(clash is not None, "a second map name binds too")
    plane = UsdGeom.Mesh(stage.GetPrimAtPath("/Plane"))
    clone = UsdGeom.Mesh.Define(stage, "/PlaneClone")
    clone.CreatePointsAttr(plane.GetPointsAttr().Get())
    clone.CreateFaceVertexCountsAttr(plane.GetFaceVertexCountsAttr().Get())
    clone.CreateFaceVertexIndicesAttr(
        plane.GetFaceVertexIndicesAttr().Get())
    nope, error = brushAuthor.BindSurface(stage, "/PlaneClone",
                                          mapName="other")
    check(nope is None and "already bound" in error,
          "stealing a bound map for another surface fails closed")

    nope, error = brushAuthor.BindSurface(stage, "/Nope")
    check(nope is None and error, "a missing surface is rejected")
    nope, error = brushAuthor.BindSurface(stage, "/BrushMaps/densityPaint")
    check(nope is None and error, "a non-mesh surface is rejected")
    nope, error = brushAuthor.BindSurface(stage, "/Plane", resolution=3)
    check(nope is None and error, "a non-pow2 resolution is rejected")
    nope, error = brushAuthor.BindSurface(stage, "/Plane", channels=2)
    check(nope is None and error, "2 channels are rejected")
    nope, error = brushAuthor.BindSurface(stage, "/Plane",
                                          interpolation="spline")
    check(nope is None and error, "an unknown interpolation is rejected")

    tri = UsdGeom.Mesh.Define(stage, "/Tri")
    tri.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (0, 0, 1)])
    tri.CreateFaceVertexCountsAttr([3])
    tri.CreateFaceVertexIndicesAttr([0, 1, 2])
    nope, error = brushAuthor.BindSurface(stage, "/Tri")
    check(nope is None and "quad" in error,
          "a non-quad mesh is rejected: %s" % error)

    ok, info = loop.bindFromSelection(stage, ["/Nope", "/Plane"])
    check(ok and state.binding is not None and
          state.binding.surfacePath == Sdf.Path("/Plane"),
          "bindFromSelection takes the first mesh (%s)" % info)
    ok, _info = loop.bindFromSelection(stage, [])
    check(not ok, "bindFromSelection with no selection fails closed")


# -- the loop: press / move / release / cancel --------------------------

def fixtureCamera():
    return brushCamera.BrushCamera(
        (0.5, 0.0, 0.0, 0.0,
         0.0, 0.0, 0.5, 0.0,
         0.0, 0.5, 0.0, 0.0,
         0.0, 0.0, 0.5, 1.0), 400, 400)


def testLoopStroke(stagePath):
    from pxr import Usd
    stage = Usd.Stage.Open(stagePath)
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    camera = fixtureCamera()

    captured, info = loop.press(stage, camera, 260.0, 130.0)
    check(not captured and state.binding is None,
          "press with no binding captures nothing (%s)" % info)
    ok, info = loop.bindFromSelection(
        stage, ["/Plane"], resolution=8, mapName="densityPaint")
    check(ok, "the loop binds /Plane (%s)" % info)
    state.hardness = 0.0
    # Full strength, and strokes aimed at face corners: the bake
    # corner-samples the grid, so an interior dab would (correctly) bake
    # to zeros at every corner.
    state.strength = 1.0

    captured, info = loop.press(stage, camera, 255.0, 145.0)
    check(captured and loop.gestureActive(),
          "press on the plane starts a stroke (%s)" % info)
    dab = state.gesture.live.calls[0]
    check(dab.face == 45 and near(dab.u, 0.1, 0.01)
          and near(dab.v, 0.1, 0.01),
          "the first dab lands on face 45 at (0.1, 0.1), got (%d, "
          "%.3f, %.3f)" % (dab.face, dab.u, dab.v))
    check(brushPreview.HasPreview(stage, state.binding),
          "the press raises the session preview")
    check(brushAuthor.BakedValues(stage, state.binding) is None,
          "and the edit target is untouched while the stroke is live")

    ok, info = loop.move(stage, 256.0, 145.0)
    check(ok and info == "throttled" and loop.dabCount() == 1,
          "a 1px move throttles (no re-pick, no dab)")
    ok, info = loop.move(stage, 295.0, 145.0)
    check(ok and loop.dabCount() > 1,
          "a 40px move extends the trail (%s)" % info)
    colors = brushPreview.PreviewColors(stage, state.binding)
    check(colors is not None and len(colors) == 4 * 64
          and max(c[0] for c in colors) > 0.5,
          "the preview displayColor shows the hot stroke")

    baked, info = loop.release(stage)
    check(baked and not loop.gestureActive(),
          "release bakes and drops the stroke (%s)" % info)
    values = brushAuthor.BakedValues(stage, state.binding)
    check(values is not None and len(values) == 4 * 64
          and max(values) > 0.5 and min(values) == 0.0,
          "the baked primvar carries painted and untouched corners "
          "(max %.3f)" % (max(values) if values else -1.0))
    check(brushPreview.HasPreview(stage, state.binding),
          "release leaves the baked map drawn")
    drawn = brushPreview.PreviewColors(stage, state.binding)
    check(drawn is not None and len(drawn) == 4 * 64
          and max(c[0] for c in drawn) > 0.5,
          "and the display shows the baked stroke")
    root = stage.GetRootLayer()
    check(root.GetAttributeAtPath(Sdf.Path(
        "/Plane.primvars:usdGen:paint:density")) is not None,
        "the bake lands in the edit target (root here)")

    # A second stroke accumulates over the first bake, not over default.
    captured, _info = loop.press(stage, camera, 105.0, 295.0)
    check(captured, "a second stroke starts over the baked stage")
    loop.release(stage)
    values = brushAuthor.BakedValues(stage, state.binding)
    hot = sum(1 for v in values if v > 0.5)
    check(hot >= 2, "both strokes survive in the baked primvar (%d hot "
          "corners)" % hot)

    # Escape: no stage write ever happened, so the abort is free.
    before = list(brushAuthor.BakedValues(stage, state.binding))
    loop.press(stage, camera, 255.0, 145.0)
    loop.move(stage, 295.0, 145.0)
    check(loop.cancel(stage) and not loop.gestureActive(),
          "Escape drops the live stroke")
    check(brushAuthor.BakedValues(stage, state.binding) == before,
          "and the baked primvar is byte-identical")
    check(brushPreview.HasPreview(stage, state.binding),
          "and the map display is restored")

    # A miss is not a stroke.
    captured, info = loop.press(stage, camera, 500.0, 500.0)
    check(not captured and not loop.gestureActive(),
          "press off the mesh captures nothing (%s)" % info)

    # Undo / redo replay the bakes.
    loop.press(stage, camera, 255.0, 145.0)
    loop.release(stage)
    painted = brushAuthor.BakedValues(stage, state.binding)
    paintedColors = brushPreview.PreviewColors(stage, state.binding)
    ok, _error = loop.undo()
    check(ok and brushAuthor.BakedValues(stage, state.binding) != painted,
          "undo takes the last bake back off")
    check(brushPreview.HasPreview(stage, state.binding)
          and brushPreview.PreviewColors(stage, state.binding)
          != paintedColors,
          "and the map display redraws to the undone map")
    ok, _error = loop.redo()
    check(ok and brushAuthor.BakedValues(stage, state.binding) == painted,
          "redo puts it back")
    check(brushPreview.PreviewColors(stage, state.binding) == paintedColors,
          "and the display redraws the bake back")


def testBrushRadius(stagePath):
    from pxr import Usd
    stage = Usd.Stage.Open(stagePath)
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    camera = fixtureCamera()
    ok, info = loop.bindFromSelection(
        stage, ["/Plane"], resolution=8, mapName="densityPaint")
    check(ok, "radius bind (%s)" % info)
    state.radiusWorld = 0.0
    captured, info = loop.press(stage, camera, 255.0, 145.0)
    check(captured, "floored press (%s)" % info)
    floored = state.gesture.live.calls[0]
    edge = brushPick.faceEdgeLen(state.gesture.snapshot, floored.face)
    check(edge > 0, "the picked face has an edge (%.3f)" % edge)
    check(floored.radius == brushPick.MIN_FACE_RADIUS,
          "a zero radius still floors to the corner-reach disk (%.2f)"
          % floored.radius)
    check(loop.cancel(stage), "floored stroke cancels")
    state.radiusWorld = 2.0 * edge
    captured, info = loop.press(stage, camera, 255.0, 145.0)
    check(captured, "radius press (%s)" % info)
    dab = state.gesture.live.calls[0]
    check(abs(dab.radius - 2.0) < 1e-9,
          "world radius converts by the picked face edge (%.3f -> %.3f)"
          % (2.0 * edge, dab.radius))
    check(loop.cancel(stage), "radius stroke cancels")


def testSmoothSoftens(stagePath):
    from pxr import Usd
    stage = Usd.Stage.Open(stagePath)
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    camera = fixtureCamera()
    ok, info = loop.bindFromSelection(
        stage, ["/Plane"], resolution=8, mapName="densityPaint")
    check(ok, "smooth bind (%s)" % info)
    state.strength = 1.0
    state.radiusWorld = 0.5
    state.value = 1.0
    loop.press(stage, camera, 255.0, 145.0)
    loop.move(stage, 295.0, 145.0)
    baked, info = loop.release(stage)
    check(baked, "the blob bakes (%s)" % info)
    before = list(brushAuthor.BakedValues(stage, state.binding))
    check(sum(1 for v in before if v > 0.5) > 0, "the blob is hot")
    state.activeBrush = "smooth"
    loop.press(stage, camera, 255.0, 145.0)
    loop.move(stage, 295.0, 145.0)
    baked, info = loop.release(stage)
    check(baked, "the smooth stroke bakes (%s)" % info)
    after = list(brushAuthor.BakedValues(stage, state.binding))
    deltas = [abs(a - b) for a, b in zip(before, after)]
    check(sum(1 for d in deltas if d > 0.05) > 0,
          "smoothing moves baked corners (max |d| %.3f)" % max(deltas))
    check(max(after) <= max(before) + 1e-9
          and min(after) >= min(before) - 1e-9,
          "without inventing new extrema")

def testSessionEditTarget(stagePath):
    """usdview's own edit target is the session layer: bake and preview
    share that layer, so release must not clear the bake with the preview,
    and Escape must not eat an earlier session-layer bake."""
    from pxr import Usd
    stage = Usd.Stage.Open(stagePath)
    stage.SetEditTarget(Usd.EditTarget(stage.GetSessionLayer()))
    state = brushState.BrushToolState()
    state.strength = 1.0
    loop = brushLoop.BrushLoop(state)
    camera = fixtureCamera()
    loop.bindFromSelection(stage, ["/Plane"], resolution=8)

    loop.press(stage, camera, 255.0, 145.0)
    loop.move(stage, 295.0, 145.0)
    baked, info = loop.release(stage)
    check(baked, "bake works from the session edit target (%s)" % info)
    values = brushAuthor.BakedValues(stage, state.binding)
    check(values is not None and max(values) > 0.5,
          "release clears the preview but keeps the session-layer bake")
    session = stage.GetSessionLayer()
    check(session.GetAttributeAtPath(Sdf.Path(
        "/Plane.primvars:usdGen:paint:density")) is not None,
        "the bake is in the session layer (the edit target)")

    # Escape mid-stroke must not eat that bake either.
    loop.press(stage, camera, 105.0, 295.0)
    loop.cancel(stage)
    check(brushAuthor.BakedValues(stage, state.binding) == values,
          "Escape leaves the earlier session-layer bake byte-identical")

    ok, _error = loop.undo()
    check(ok and brushAuthor.BakedValues(stage, state.binding) is None,
          "undo removes the session-layer bake")


def testPreviewPrior(stagePath):
    """A session displayColor the artist authored survives a preview."""
    from pxr import Usd
    stage = Usd.Stage.Open(stagePath)
    binding, error = brushAuthor.BindSurface(stage, "/Plane", resolution=8)
    check(binding is not None, "prior bind (%s)" % error)
    surface = stage.GetPrimAtPath("/Plane")
    with Usd.EditContext(stage, stage.GetSessionLayer()):
        UsdGeom.PrimvarsAPI(surface).CreatePrimvar(
            "displayColor", Sdf.ValueTypeNames.Color3fArray,
            "constant").GetAttr().Set(Vt.Vec3fArray([Gf.Vec3f(1, 0, 0)]))
    prior = brushPreview.CapturePrior(stage, binding)
    check(prior is not None and prior.had,
          "the session path captures the authored displayColor")
    base, _error = brushAuthor.BaseGridFromStage(stage, binding)
    ok, info = brushPreview.SetPreview(stage, binding, base)
    colors = brushPreview.PreviewColors(stage, binding)
    check(ok and info == "session" and colors is not None
          and len(colors) == 4 * 64,
          "the preview overrides the authored displayColor (%s)" % info)
    ok, info = brushPreview.ClearPreview(stage, binding, prior)
    colors = brushPreview.PreviewColors(stage, binding)
    check(ok and colors is not None and len(colors) == 1
          and colors[0] == (1.0, 0.0, 0.0),
          "clear restores the authored session displayColor (%s)" % info)
    ok, info = brushPreview.ClearPreview(stage, binding)
    check(ok and not brushPreview.HasPreview(stage, binding),
          "a clear without prior removes it (%s)" % info)


def testPreviewColors():
    check(brushPreview.heatColor(0.0) == (0.0, 0.0, 0.0),
          "heat(0) is black")
    check(brushPreview.heatColor(1.0) == (1.0, 1.0, 1.0),
          "heat(1) is white")
    mid = brushPreview.heatColor(0.5)
    check(mid[0] == 1.0 and mid[1] == 0.0 and mid[2] == 0.0,
          "heat(0.5) is pure red")
    check(brushPreview.grayColor(0.25) == (0.25, 0.25, 0.25),
          "gray(0.25) is flat")
    check(brushPreview.mapColor("nope", 1.0) == (1.0, 1.0, 1.0),
          "an unknown ramp falls back to heat")


# -- incremental session writes -----------------------------------------

def _patchBind(stagePath):
    """(stage, state, loop, binding): a plane bound at res 8."""
    stage = Usd.Stage.Open(stagePath)
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    ok, info = loop.bindFromSelection(
        stage, ["/Plane"], resolution=8, mapName="densityPaint")
    check(ok, "the loop binds /Plane (%s)" % info)
    return (stage, state, loop, state.binding)


def _dabbed(grid, *faces):
    """A clone of `grid` with one corner dab per listed face."""
    work = grid.clone()
    for face in faces:
        brushMap.applyDab(work, brushMap.BrushDab(
            face, 0.9, 0.9, radius=0.2, strength=1.0, value=1.0,
            channel=-1, mode=brushMap.MODE_SET,
            falloff=brushMap.FALLOFF_SMOOTH))
    return work


class _cornerSpy(object):
    """Record the faces a grid is asked for (cornerValues or
    cornerBuffer(faces)), whichever path the caller takes."""

    def __init__(self, grid, seen):
        self._grid = grid
        self._seen = seen

    def __enter__(self):
        grid, seen = self._grid, self._seen
        self._values = grid.cornerValues
        self._buffer = getattr(grid, "cornerBuffer", None)
        values, buffer = self._values, self._buffer
        grid.cornerValues = lambda f, c: (seen.append(f), values(f, c))[1]
        if buffer is not None:
            def spyBuffer(faces=None):
                seen.extend(range(grid.numFaces()) if faces is None
                            else list(faces))
                return buffer(faces)
            grid.cornerBuffer = spyBuffer
        return self

    def __exit__(self, *exc):
        del self._grid.cornerValues
        if self._buffer is not None:
            del self._grid.cornerBuffer
        return False


def testPreviewPatch(stagePath):
    stage, _state, _loop, binding = _patchBind(stagePath)
    base, error = brushAuthor.BaseGridFromStage(stage, binding)
    check(base is not None, "the base grid builds (%s)" % error)
    ok, _info = brushPreview.SetPreview(stage, binding, base)
    check(ok, "the full preview paints")
    before = brushPreview.PreviewColors(stage, binding)
    work = _dabbed(base, 0, 1)
    ok, info = brushPreview.PatchPreview(stage, binding, work, [0, 1])
    check(ok, "the patch repaints two faces (%s)" % info)
    patched = brushPreview.PreviewColors(stage, binding)
    ok, _info = brushPreview.SetPreview(stage, binding, work)
    check(ok, "the full repaint paints")
    full = brushPreview.PreviewColors(stage, binding)
    check(patched == full,
          "patching two faces equals the full repaint")
    check(patched != before,
          "and something actually changed")
    brushPreview.ClearPreview(stage, binding)
    ok, _info = brushPreview.PatchPreview(stage, binding, work, [0, 1])
    check(ok and brushPreview.PreviewColors(stage, binding) == full,
          "a patch with no preview falls back to full")
    raw = list(UsdGeom.PrimvarsAPI(stage.GetPrimAtPath("/Plane"))
               .GetPrimvar("displayColor").GetAttr().Get())
    raw[5 * 4] = Gf.Vec3f(0.25, 0.5, 0.75)
    with Usd.EditContext(stage, stage.GetSessionLayer()):
        UsdGeom.PrimvarsAPI(stage.GetPrimAtPath("/Plane")).CreatePrimvar(
            "displayColor", Sdf.ValueTypeNames.Color3fArray,
            "faceVarying").GetAttr().Set(Vt.Vec3fArray(raw))
    ok, _info = brushPreview.PatchPreview(stage, binding, work, [0, 1])
    kept = brushPreview.PreviewColors(stage, binding)[5 * 4]
    check(ok and kept == (0.25, 0.5, 0.75),
          "the patch preserves a mid-stroke external edit")
    seen = []
    with _cornerSpy(work, seen):
        ok, _info = brushPreview.PatchPreview(
            stage, binding, work, [2, 3])
    check(ok and set(seen) == set([2, 3]),
          "the patch samples only its dirty faces (got %r)"
          % (sorted(set(seen)),))


def _liveValues(stage, binding):
    primvar = UsdGeom.PrimvarsAPI(
        stage.GetPrimAtPath("/Plane")).GetPrimvar(binding.primvar)
    raw = primvar.GetAttr().Get()
    return list(raw) if raw is not None else None


def testLivePatch(stagePath):
    stage, _state, _loop, binding = _patchBind(stagePath)
    base, error = brushAuthor.BaseGridFromStage(stage, binding)
    check(base is not None, "the base grid builds (%s)" % error)
    ok, error = brushAuthor.WriteLivePrimvar(stage, binding, base)
    check(ok, "the full live write lands (%s)" % error)
    work = _dabbed(base, 0, 1)
    ok, error = brushAuthor.PatchLivePrimvar(stage, binding, work, [0, 1])
    check(ok, "the patch writes two faces (%s)" % error)
    patched = _liveValues(stage, binding)
    ok, _error = brushAuthor.WriteLivePrimvar(stage, binding, work)
    check(ok, "the full live rewrite lands")
    full = _liveValues(stage, binding)
    check(patched == full,
          "patching two faces equals the full rewrite")
    check(patched != [0.0] * len(patched),
          "and something actually changed")
    brushAuthor.ClearLivePrimvar(stage, binding, (False, None, None))
    ok, _error = brushAuthor.PatchLivePrimvar(stage, binding, work, [0, 1])
    check(ok and _liveValues(stage, binding) == full,
          "a patch with no scratch falls back to full")
    raw = _liveValues(stage, binding)
    raw[5 * 4] = 0.5
    with Usd.EditContext(stage, stage.GetSessionLayer()):
        UsdGeom.PrimvarsAPI(stage.GetPrimAtPath("/Plane")).CreatePrimvar(
            binding.primvar, Sdf.ValueTypeNames.FloatArray,
            "faceVarying").GetAttr().Set(Vt.FloatArray(raw))
    ok, _error = brushAuthor.PatchLivePrimvar(stage, binding, work, [0, 1])
    check(ok and _liveValues(stage, binding)[5 * 4] == 0.5,
          "the patch preserves a mid-stroke external edit")
    seen = []
    with _cornerSpy(work, seen):
        ok, _error = brushAuthor.PatchLivePrimvar(
            stage, binding, work, [2, 3])
    check(ok and set(seen) == set([2, 3]),
          "the patch samples only its dirty faces (got %r)"
          % (sorted(set(seen)),))


def testStrokeHardening(stagePath):
    stage = Usd.Stage.Open(stagePath)
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    camera = fixtureCamera()
    ok, info = loop.bindFromSelection(
        stage, ["/Plane"], resolution=8, mapName="densityPaint")
    check(ok, "the loop binds /Plane (%s)" % info)
    state.strength = 1.0
    captured, info = loop.press(stage, camera, 255.0, 145.0)
    check(captured, "press starts a stroke (%s)" % info)
    loop.move(stage, 295.0, 145.0)
    origCommit = brushMap.LiveStroke.commitGrid
    fails = [True]

    def flakyCommit(self):
        if fails:
            del fails[:]
            raise AttributeError("simulated flake")
        return origCommit(self)

    brushMap.LiveStroke.commitGrid = flakyCommit
    try:
        baked, info = loop.release(stage)
    finally:
        brushMap.LiveStroke.commitGrid = origCommit
    check(not baked and "still live" in info,
          "a failed commit keeps the stroke live (%s)" % info)
    check(loop.gestureActive()
          and brushPreview.HasPreview(stage, state.binding),
          "with the gesture and the preview intact")
    baked, info = loop.release(stage)
    check(baked and not loop.gestureActive(),
          "releasing again bakes after the flake (%s)" % info)
    captured, _info = loop.press(stage, camera, 255.0, 145.0)
    check(captured, "press starts a second stroke")
    origDab = brushMap.LiveStroke.dab
    fails = [True]

    def flakyDab(self, *args, **kwargs):
        if fails:
            del fails[:]
            raise AttributeError("simulated flake")
        return origDab(self, *args, **kwargs)

    brushMap.LiveStroke.dab = flakyDab
    try:
        moved, info = loop.move(stage, 295.0, 145.0)
    finally:
        brushMap.LiveStroke.dab = origDab
    check(not moved and "dropped" in info,
          "a failed move drops the stroke (%s)" % info)
    check(not loop.gestureActive()
          and brushPreview.HasPreview(stage, state.binding),
          "dropping the gesture but keeping the baked map drawn")


# -- panels -------------------------------------------------------------

def testMaskPresets(stagePath):
    from pxr import Usd
    check([p.id for p in brushAuthor.MASK_PRESETS]
          == ["density", "length", "width", "clump", "curl"],
          "the mask presets are the five grooming masks")
    check(brushAuthor.MaskPresetFor("clump").primvar
          == "usdGen:paint:clump"
          and brushAuthor.MaskPresetFor("nope") is None,
          "MaskPresetFor resolves ids and fails closed on unknown ones")
    check(brushAuthor.MaskPresetFor("density").primvar
          == "usdGen:paint:density",
          "the density preset names the primvar scatter reads")
    stage = Usd.Stage.Open(stagePath)
    binding, error = brushAuthor.BindMaskPreset(stage, "/Plane", "length")
    check(binding is not None and not error,
          "BindMaskPreset binds (%s)" % (error or binding.mapPath))
    prim = stage.GetPrimAtPath(binding.mapPath)
    check((prim.GetTypeName(), str(binding.mapPath),
           prim.GetAttribute("usdGen:paint:primvar").Get(),
           prim.GetAttribute("usdGen:map:default").Get())
          == ("UsdGenPaintMap", "/BrushMaps/lengthPaint",
              "usdGen:paint:length", 1.0),
          "the length preset authors its map, primvar and default")
    nope, error = brushAuthor.BindMaskPreset(stage, "/Plane", "nope")
    check(nope is None and "unknown mask preset" in error,
          "an unknown preset fails closed without touching the stage")
    state = brushState.BrushToolState()
    check(state.maskPreset == "density",
          "new tool state paints the density mask")
    rows = dict((d.id, d) for d in brushPanels.descriptors(state))
    check("maskPreset" in rows, "the palette has a mask-preset row")
    rows["maskPreset"].set(state, "curl")
    check(state.maskPreset == "curl", "the row selects a preset")
    rows["maskPreset"].set(state, "nope")
    check(state.maskPreset == "density",
          "an unknown preset falls back to density")


def testDescriptions(stagePath):
    from pxr import Usd
    stage = Usd.Stage.Open(stagePath)
    check(brushAuthor.ListDescriptions(stage) == [],
          "a fresh stage names no descriptions")
    check(brushAuthor.ListDescriptions(None) == [],
          "and no stage names none either")
    desc, error = brushAuthor.EnsureDescription(stage, "/Plane")
    check(desc is not None and not error,
          "Ensure defines the description (%s)" % (error or desc))
    prim = stage.GetPrimAtPath(desc)
    check((prim.GetTypeName(), str(desc))
          == ("UsdGenDescription", "/Groom/Description"),
          "at /Groom/Description, typed")
    check(prim.GetRelationship("usdGen:surface").GetTargets()
          == [Sdf.Path("/Plane")],
          "usdGen:surface targets the mesh")
    again, error = brushAuthor.EnsureDescription(stage, "/Plane")
    check(str(again) == "/Groom/Description2",
          "a second description takes the next free name")
    check([str(p) for p in brushAuthor.ListDescriptions(stage)]
          == ["/Groom/Description", "/Groom/Description2"],
          "ListDescriptions finds both in path order")
    custom, error = brushAuthor.EnsureDescription(
        stage, "/Plane", descPath="/Groom/Custom")
    check(str(custom) == "/Groom/Custom",
          "an explicit path is honored")
    adopted, error = brushAuthor.EnsureDescription(
        stage, "/Plane", descPath="/Groom/Custom")
    check(adopted is not None and str(adopted) == "/Groom/Custom",
          "the same surface re-adopts the description")
    plane = UsdGeom.Mesh(stage.GetPrimAtPath("/Plane"))
    clone = UsdGeom.Mesh.Define(stage, "/PlaneClone")
    clone.CreatePointsAttr(plane.GetPointsAttr().Get())
    clone.CreateFaceVertexCountsAttr(plane.GetFaceVertexCountsAttr().Get())
    clone.CreateFaceVertexIndicesAttr(
        plane.GetFaceVertexIndicesAttr().Get())
    nope, error = brushAuthor.EnsureDescription(
        stage, "/PlaneClone", descPath="/Groom/Custom")
    check(nope is None and "already grows" in error,
          "another surface cannot steal the description")
    nope, error = brushAuthor.EnsureDescription(
        stage, "/Groom/Custom", descPath="/Groom/Nope")
    check(nope is None and error,
          "a description grows from a mesh, not a description")
    nope, error = brushAuthor.EnsureDescription(stage, "/Nope")
    check(nope is None and error, "a missing surface is rejected")
    nope, error = brushAuthor.EnsureDescription(
        stage, "/Plane", descPath="relative/Path")
    check(nope is None and "absolute prim path" in error,
          "a relative path is rejected")
    nope, error = brushAuthor.EnsureDescription(None, "/Plane")
    check(nope is None and error, "no stage is rejected")
    surface, error = brushAuthor.DescriptionSurface(stage, "/Groom/Custom")
    check(surface == Sdf.Path("/Plane") and not error,
          "DescriptionSurface reads the mesh back")
    stage.DefinePrim("/Groom/Bare", "UsdGenDescription")
    nope, error = brushAuthor.DescriptionSurface(stage, "/Groom/Bare")
    check(nope is None and "names no" in error,
          "a description without a surface names none")
    nope, error = brushAuthor.DescriptionSurface(stage, "/Plane")
    check(nope is None and error, "a mesh is not a description")

    # An in-memory stage: same-file opens share their layer, so only a
    # fresh stage gives exact description names.
    work = Usd.Stage.CreateInMemory()
    quad = UsdGeom.Mesh.Define(work, "/Plane")
    quad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    quad.CreateFaceVertexCountsAttr([4])
    quad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    ok, info = loop.setupDescriptionFromSelection(work, ["/Nope", "/Plane"])
    check(ok and state.activeDescription == "/Groom/Description"
          and state.binding is not None
          and str(state.binding.mapPath)
          == "/Groom/Description/Maps/densityPaint",
          "setup defines, activates and binds (%s)" % info)
    check(state.undoStack.canUndo(), "setup is undoable")
    ops = work.GetPrimAtPath("/Groom/Description/Ops")
    check([c.GetName() for c in ops.GetChildren()]
          == ["width", "curl", "clump", "grow", "scatter"],
          "setup creates terminal-first, so bottom-up is the pipeline")
    check([work.GetPrimAtPath("/Groom/Description/Ops/" + n).GetTypeName()
           for n in ("width", "curl", "clump", "grow", "scatter")]
          == ["UsdGenWidth", "UsdGenCurl", "UsdGenClump", "UsdGenGrow",
              "UsdGenScatter"],
          "the chain reads Scatter -> Grow -> Clump -> Curl -> Width "
          "bottom-up")
    check((work.GetPrimAtPath("/Groom/Description/Ops/scatter").GetAttribute(
               "usdGen:density").Get(),
           work.GetPrimAtPath("/Groom/Description").GetAttribute(
               "usdGen:curve:basis").Get())
          == (600.0, "bspline"),
          "operator and look defaults match the groom")
    amounts = (
        work.GetPrimAtPath("/Groom/Description/Ops/clump").GetAttribute(
            "usdGen:clump:amount").Get(),
        work.GetPrimAtPath("/Groom/Description/Ops/curl").GetAttribute(
            "usdGen:radius").Get(),
        work.GetPrimAtPath("/Groom/Description/Ops/width").GetAttribute(
            "usdGen:width").Get())
    check(all(abs(got - want) < 1e-6
              for got, want in zip(amounts, (0.8, 0.02, 0.005))),
          "clump/curl/width ship at painted-full-effect values")
    check(not work.GetPrimAtPath("/Groom/Description").GetAttribute(
        "usdGen:operatorOrder").IsValid(),
        "execution needs no order metadata (bottom-up hierarchy)")
    reops, error = brushAuthor.SetupDescription(
        work, "/Plane", descPath="/Groom/Description")
    check(reops is not None and not error,
          "re-setup adopts the operators")
    # A legacy two-op description upgrades in place on its own stage.
    legacy = Usd.Stage.CreateInMemory()
    lquad = UsdGeom.Mesh.Define(legacy, "/Plane")
    lquad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    lquad.CreateFaceVertexCountsAttr([4])
    lquad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    ldesc, error = brushAuthor.EnsureDescription(
        legacy, "/Plane", descPath="/Groom/Hair")
    check(ldesc is not None, "the legacy description exists (%s)" % error)
    legacy.DefinePrim("/Groom/Hair/Ops", "Scope")
    legacy.DefinePrim("/Groom/Hair/Ops/grow", "UsdGenGrow")
    legacy.DefinePrim("/Groom/Hair/Ops/scatter", "UsdGenScatter")
    legacy.GetPrimAtPath("/Groom/Hair/Ops/grow").CreateAttribute(
        "usdGen:length", Sdf.ValueTypeNames.Float).Set(0.5)
    legacy.GetPrimAtPath("/Groom/Hair").CreateAttribute(
        "usdGen:operatorOrder", Sdf.ValueTypeNames.StringArray).Set(
            ["stale"])
    up, error = brushAuthor.SetupDescription(
        legacy, "/Plane", descPath="/Groom/Hair")
    check(up is not None and [c.GetName() for c in
          legacy.GetPrimAtPath("/Groom/Hair/Ops").GetChildren()]
          == ["width", "curl", "clump", "grow", "scatter"],
          "upgrade keeps the old ops and orders terminal-first")
    check(legacy.GetPrimAtPath("/Groom/Hair/Ops/grow").GetAttribute(
        "usdGen:length").Get() == 0.5,
        "the reorder preserves the old ops' opinions")
    check(not legacy.GetPrimAtPath("/Groom/Hair").GetAttribute(
        "usdGen:operatorOrder").IsValid(),
        "and the retired order attr is stripped")
    legacy.DefinePrim("/Groom/Hair/Ops/frizz", "UsdGenNoise")
    up, error = brushAuthor.SetupDescription(
        legacy, "/Plane", descPath="/Groom/Hair")
    check([c.GetName() for c in
           legacy.GetPrimAtPath("/Groom/Hair/Ops").GetChildren()]
          == ["width", "curl", "clump", "grow", "scatter", "frizz"],
          "a hand-added op keeps its slot when the chain is ordered")
    # Adopt-if-correct: a pre-ordered chain is left untouched.
    preordered = Usd.Stage.CreateInMemory()
    pquad = UsdGeom.Mesh.Define(preordered, "/Plane")
    pquad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    pquad.CreateFaceVertexCountsAttr([4])
    pquad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    pdesc, error = brushAuthor.EnsureDescription(
        preordered, "/Plane", descPath="/Groom/Hair")
    check(pdesc is not None, "the preordered description exists")
    preordered.DefinePrim("/Groom/Hair/Ops", "Scope")
    for opName, opType in (("width", "UsdGenWidth"),
                           ("curl", "UsdGenCurl"),
                           ("clump", "UsdGenClump"),
                           ("grow", "UsdGenGrow"),
                           ("scatter", "UsdGenScatter")):
        preordered.DefinePrim("/Groom/Hair/Ops/" + opName, opType)
    pup, error = brushAuthor.SetupDescription(
        preordered, "/Plane", descPath="/Groom/Hair")
    check(pup is not None and [c.GetName() for c in
          preordered.GetPrimAtPath("/Groom/Hair/Ops").GetChildren()]
          == ["width", "curl", "clump", "grow", "scatter"],
          "a pre-ordered chain is left untouched")
    # Inactive ops are adopted, never redefined or reordered.
    legacy.GetPrimAtPath("/Groom/Hair/Ops/clump").SetActive(False)
    up, error = brushAuthor.SetupDescription(
        legacy, "/Plane", descPath="/Groom/Hair")
    check(up is not None and [c.GetName() for c in
          legacy.GetPrimAtPath("/Groom/Hair/Ops").GetChildren()]
          == ["width", "curl", "grow", "scatter", "frizz"],
          "re-setup adopts a deactivated op (composed order skips it)")
    legacy.GetPrimAtPath("/Groom/Hair/Ops/clump").SetActive(True)
    up, error = brushAuthor.SetupDescription(
        legacy, "/Plane", descPath="/Groom/Hair")
    check(up is not None and [c.GetName() for c in
          legacy.GetPrimAtPath("/Groom/Hair/Ops").GetChildren()]
          == ["width", "curl", "clump", "grow", "scatter", "frizz"],
          "reactivation restores its slot")
    legacy.GetPrimAtPath("/Groom/Hair/Ops").SetActive(False)
    up, error = brushAuthor.SetupDescription(
        legacy, "/Plane", descPath="/Groom/Hair")
    check(up is not None,
          "setup succeeds while Ops is off")
    legacy.GetPrimAtPath("/Groom/Hair/Ops").SetActive(True)
    up, error = brushAuthor.SetupDescription(
        legacy, "/Plane", descPath="/Groom/Hair")
    check(up is not None and [c.GetName() for c in
          legacy.GetPrimAtPath("/Groom/Hair/Ops").GetChildren()]
          == ["width", "curl", "clump", "grow", "scatter", "frizz"],
          "reactivating Ops restores the chain")
    ok, _info = loop.setupDescriptionFromSelection(work, [])
    check(not ok, "setup with no selection fails closed")
    ok, _info = loop.setupDescriptionFromSelection(work, ["/Plane"])
    check(ok and state.activeDescription == "/Groom/Description",
          "a second setup re-adopts the same description (no duplicate)")
    check([str(p) for p in brushAuthor.ListDescriptions(work)]
          == ["/Groom/Description"],
          "setup never mints a duplicate for the same mesh")
    ok, _info = loop.paintToDescription(work, "/Groom/Description")
    check(ok and state.activeDescription == "/Groom/Description"
          and str(state.binding.mapPath)
          == "/Groom/Description/Maps/densityPaint",
          "paintToDescription switches the target back")
    nope_ok, error = loop.paintToDescription(work, "/Groom/Nope")
    check(not nope_ok and error, "an unknown description is rejected")
    nope_ok, error = loop.paintToDescription(work, "/Plane")
    check(not nope_ok and error, "a mesh is rejected as a target")
    ok, _info = loop.bindFromSelection(work, [])
    check(ok and state.binding is not None,
          "bind with an active description needs no selection")
    check("desc: /Groom/Description" in brushPanels.statusText(state),
          "the status names the paint target")

    # DescriptionForSurface: exactly-one wins, ambiguity mints.
    dupe = Usd.Stage.CreateInMemory()
    quad2 = UsdGeom.Mesh.Define(dupe, "/Plane")
    quad2.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    quad2.CreateFaceVertexCountsAttr([4])
    quad2.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    match, _error = brushAuthor.DescriptionForSurface(dupe, "/Plane")
    check(match is None,
          "a mesh-only stage matches no description")
    first, _error = brushAuthor.EnsureDescription(
        dupe, "/Plane", descPath="/A/One")
    match, _error = brushAuthor.DescriptionForSurface(dupe, "/Plane")
    check(match == Sdf.Path("/A/One"),
          "one groom on the mesh is adopted")
    adopted, _error = brushAuthor.SetupDescription(dupe, "/Plane")
    check(adopted is not None and str(adopted) == "/A/One",
          "setup adopts it instead of minting (%s)" % adopted)
    check([str(x) for x in brushAuthor.ListDescriptions(dupe)]
          == ["/A/One"],
          "no duplicate description was defined")
    second, _error = brushAuthor.EnsureDescription(
        dupe, "/Plane", descPath="/B/Two")
    check(second is not None, "a second groom binds the same mesh")
    match, _error = brushAuthor.DescriptionForSurface(dupe, "/Plane")
    check(match is None,
          "two grooms on one mesh stay ambiguous (no guess)")
    minted, _error = brushAuthor.SetupDescription(dupe, "/Plane")
    check(minted is not None
          and str(minted) not in ("/A/One", "/B/Two"),
          "ambiguous setup mints instead of adopting (%s)" % minted)
    match, _error = brushAuthor.DescriptionForSurface(dupe, "/NoMesh")
    check(match is None, "an unknown mesh matches nothing")
    match, _error = brushAuthor.DescriptionForSurface(None, "/Plane")
    check(match is None, "no stage matches nothing")


def testPaintWiring():
    work = Usd.Stage.CreateInMemory()
    quad = UsdGeom.Mesh.Define(work, "/Plane")
    quad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    quad.CreateFaceVertexCountsAttr([4])
    quad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    desc, error = brushAuthor.SetupDescription(work, "/Plane")
    check(desc is not None, "setup defines the description (%s)" % error)
    ok, info = loop.paintToDescription(work, desc)
    check(ok and state.binding is not None,
          "paint-to binds the density preset (%s)" % info)
    check("wired" not in info,
          "without wiring it: scatter takes no expressions (%s)" % info)
    check(not work.GetPrimAtPath(
        "/Groom/Description/Expressions/densityMask").IsValid(),
          "so no density expression is authored")
    ok, info = brushAuthor.EnsurePaintWiring(work, desc, "density")
    check(not ok, "density has no v1 wiring target (%s)" % info)
    state.maskPreset = "length"
    ok, info = loop.paintToDescription(work, desc)
    check(ok and "wired length paint" in info,
          "length binds and wires to grow (%s)" % info)
    exprPath = Sdf.Path("/Groom/Description/Expressions/lengthScale")
    expr = work.GetPrimAtPath(exprPath)
    check(expr.IsValid() and expr.GetTypeName() == "UsdGenExpression",
          "the length expression exists, typed")
    check(expr.GetAttribute("usdGen:expr:source").Get()
          == 'ptex("lengthPaint") * 0.25',
          "scaling the map by the live grow value")
    check(expr.GetRelationship("input:lengthPaint").GetTargets()
          == [Sdf.Path("/Groom/Description/Maps/lengthPaint")],
          "its input targets the PaintMap")
    grow = work.GetPrimAtPath("/Groom/Description/Ops/grow")
    check(list(grow.GetAttribute("usdGen:length").GetConnections())
          == [exprPath],
          "grow length connects to the expression")
    check(grow.GetAttribute("usdGen:length").GetCustomDataByKey(
        "usdGen:evaluation") == "primitive",
        "the binding evaluates at primitive domain")
    ok, info = loop.paintToDescription(work, desc)
    check(ok, "repainting adopts the wiring (%s)" % info)
    check(loop.undo(), "undo pops the wiring entry")
    check(list(grow.GetAttribute("usdGen:length").GetConnections()) == [],
          "undo takes the connection back off")
    check(not (grow.GetAttribute("usdGen:length").GetCustomDataByKey(
        "usdGen:evaluation") == "primitive"),
        "undo takes the evaluation opinion back too")
    check(loop.redo(), "redo re-applies it")
    check(list(grow.GetAttribute("usdGen:length").GetConnections())
          == [exprPath],
          "redo puts the connection back")
    check(grow.GetAttribute("usdGen:length").GetCustomDataByKey(
        "usdGen:evaluation") == "primitive",
        "redo puts the evaluation opinion back too")
    for presetId, opName, param, exprName, literal in (
            ("width", "width", "usdGen:width", "widthScale", "0.005"),
            ("clump", "clump", "usdGen:clump:amount", "clumpAmountScale",
             "0.8"),
            ("curl", "curl", "usdGen:radius", "curlRadiusScale", "0.02")):
        state.maskPreset = presetId
        ok, info = loop.paintToDescription(work, desc)
        check(ok and "wired %s paint" % presetId in info,
              "%s binds and wires to %s (%s)"
              % (presetId, opName, info))
        wiredExpr = work.GetPrimAtPath(
            "/Groom/Description/Expressions/" + exprName)
        check(wiredExpr.GetAttribute("usdGen:expr:source").Get()
              == 'ptex("%sPaint") * %s' % (presetId, literal),
              "scaling the %s map by the live %s value"
              % (presetId, opName))
        wiredOp = work.GetPrimAtPath(
            "/Groom/Description/Ops/" + opName)
        check(list(wiredOp.GetAttribute(param).GetConnections())
              == [wiredExpr.GetPath()],
              "%s.%s connects to the expression" % (opName, param))
        check(wiredOp.GetAttribute(param).GetCustomDataByKey(
            "usdGen:evaluation") == "primitive",
              "the %s binding evaluates at primitive domain"
              % presetId)


def testUpsample():
    stage = Usd.Stage.CreateInMemory()
    quad = UsdGeom.Mesh.Define(stage, "/Plane")
    quad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    quad.CreateFaceVertexCountsAttr([4])
    quad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    binding, error = brushAuthor.BindSurface(
        stage, "/Plane", mapName="densityPaint",
        primvar="usdGen:paint:density", channels=1,
        defaultValue=0.25, resolution=4)
    check(binding is not None, "upsample bind (%s)" % error)
    pv = UsdGeom.PrimvarsAPI(stage.GetPrimAtPath("/Plane")).CreatePrimvar(
        "usdGen:paint:density", Sdf.ValueTypeNames.FloatArray)
    pv.SetInterpolation("faceVarying")
    pv.Set(Vt.FloatArray([0.0, 1.0, 1.0, 0.0]))
    grid, error = brushAuthor.BaseGridFromStage(stage, binding)
    check(grid is not None and not error, "the base grid builds")
    check(grid.getTexel(0, 0, 0, 0) == (True, 0.0),
          "corner texels carry their corners verbatim")
    ok, centre = grid.sample(0, 0.5, 0.5, 0, brushMap.INTERP_BILINEAR)
    check(ok and near(centre, 0.5), "bilinear centre is the corner mean")
    saved = sys.modules.get("numpy", "absent")
    sys.modules["numpy"] = None
    try:
        plain, error = brushAuthor.BaseGridFromStage(stage, binding)
    finally:
        if saved == "absent":
            del sys.modules["numpy"]
        else:
            sys.modules["numpy"] = saved
    check(plain is not None, "the fallback builds too")
    match = all(
        near(grid.getTexel(0, s, t, 0)[1],
             plain.getTexel(0, s, t, 0)[1], 1e-9)
        for t in range(4) for s in range(4))
    check(match, "numpy and fallback grids agree")
    ok, corners = grid.cornerValues(0, 0)
    check(ok and list(corners) == [0.0, 1.0, 1.0, 0.0],
          "bake -> base -> bake round-trips exactly")
    pv.Set(Vt.FloatArray([float("nan"), 1.0, 1.0, 0.0]))
    grid, error = brushAuthor.BaseGridFromStage(stage, binding)
    check(grid is not None and all(
        grid.getTexel(0, s, t, 0) == (True, 0.25)
        for t in range(4) for s in range(4)),
        "a NaN corner stamps the face to the default")
    made, error = brushMap.BrushMap.create(
        brushMap.BrushMapSpec(1, 4, 1, 0.0, False))
    check(made.setTexelsBulk([0.5] * 16)
          and made.getTexel(0, 3, 3, 0) == (True, 0.5),
          "bulk set stores every texel")
    check(not made.setTexelsBulk([0.5] * 15),
          "a short list is refused")


def testPresetSwitch(stagePath):
    stage = Usd.Stage.Open(stagePath)
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    ok, info = loop.bindFromSelection(
        stage, ["/Plane"], resolution=8, mapName="densityPaint",
        primvar="usdGen:paint:density", channels=1, defaultValue=1.0)
    check(ok, "the loop binds density (%s)" % info)
    ok, info = loop.rebindPreset(stage, "length")
    check(ok and state.maskPreset == "length"
          and state.binding.primvar == "usdGen:paint:length"
          and str(state.binding.mapPath) == "/BrushMaps/lengthPaint",
          "rebind switches the map and primvar (%s)" % info)
    check(state.binding.resolution == 8,
          "keeping the surface options of the live binding")
    drawn = brushPreview.PreviewColors(stage, state.binding)
    check(drawn is not None and len(drawn) == 4 * 64
          and min(c[0] for c in drawn) == 1.0,
          "and the unpainted length map is drawn (white)")
    ok, info = loop.rebindPreset(stage, "length")
    check(ok and "already painting" in info,
          "rebinding the live preset is a no-op (%s)" % info)
    nope, error = loop.rebindPreset(stage, "nope")
    check(nope is False and "unknown mask preset" in error
          and state.maskPreset == "length",
          "an unknown preset fails closed")
    # A live stroke owns the binding.
    camera = fixtureCamera()
    state.strength = 1.0
    captured, _info = loop.press(stage, camera, 255.0, 145.0)
    check(captured, "press starts a stroke")
    refused, error = loop.rebindPreset(stage, "width")
    check(refused is False and "live stroke" in error
          and state.binding.primvar == "usdGen:paint:length",
          "a preset switch mid-stroke is refused")
    loop.cancel(stage)
    # Switching types switches what is drawn: an in-memory quad with a
    # white density primvar and a black length primvar flips exactly.
    flip = Usd.Stage.CreateInMemory()
    quad = UsdGeom.Mesh.Define(flip, "/Plane")
    quad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    quad.CreateFaceVertexCountsAttr([4])
    quad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    surface = flip.GetPrimAtPath("/Plane")
    api = UsdGeom.PrimvarsAPI(surface)
    api.CreatePrimvar("usdGen:paint:density",
                      Sdf.ValueTypeNames.FloatArray,
                      "faceVarying").GetAttr().Set(
        Vt.FloatArray([1.0, 1.0, 1.0, 1.0]))
    api.CreatePrimvar("usdGen:paint:length",
                      Sdf.ValueTypeNames.FloatArray,
                      "faceVarying").GetAttr().Set(
        Vt.FloatArray([0.0, 0.0, 0.0, 0.0]))
    state3 = brushState.BrushToolState()
    loop3 = brushLoop.BrushLoop(state3)
    ok, info = loop3.bindFromSelection(
        flip, ["/Plane"], resolution=8, mapName="densityPaint",
        primvar="usdGen:paint:density", channels=1, defaultValue=1.0)
    check(ok, "the loop binds the flip quad (%s)" % info)
    shown = brushPreview.PreviewColors(flip, state3.binding)
    check(shown is not None and min(c[0] for c in shown) == 1.0,
          "density shows white")
    ok, info = loop3.rebindPreset(flip, "length")
    check(ok, "the switch rebinds (%s)" % info)
    shown = brushPreview.PreviewColors(flip, state3.binding)
    check(shown is not None and max(c[0] for c in shown) == 0.0,
          "length shows black instead")
    ok, info = loop3.rebindPreset(flip, "density")
    shown = brushPreview.PreviewColors(flip, state3.binding)
    check(ok and shown is not None
          and min(c[0] for c in shown) == 1.0,
          "and back to white on the way home (%s)" % info)
    # Paint-to: the switch lands in the description Maps and wires.
    work = Usd.Stage.CreateInMemory()
    quad = UsdGeom.Mesh.Define(work, "/Plane")
    quad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    quad.CreateFaceVertexCountsAttr([4])
    quad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    state2 = brushState.BrushToolState()
    loop2 = brushLoop.BrushLoop(state2)
    ok, info = loop2.setupDescriptionFromSelection(work, ["/Plane"])
    check(ok, "setup binds density into the description (%s)" % info)
    ok, info = loop2.rebindPreset(work, "length")
    check(ok and str(state2.binding.mapPath)
          == "/Groom/Description/Maps/lengthPaint",
          "paint-to rebind lands in the description Maps (%s)" % info)
    expr = work.GetPrimAtPath("/Groom/Description/Expressions/lengthScale")
    check(expr and expr.IsValid()
          and "lengthPaint" in
          (expr.GetAttribute("usdGen:expr:source").Get() or ""),
          "the switch wires the length expression")
    # Unbound: the preset is recorded, or paints to the description.
    state4 = brushState.BrushToolState()
    loop4 = brushLoop.BrushLoop(state4)
    ok, info = loop4.rebindPreset(work, "clump")
    check(ok and state4.maskPreset == "clump"
          and state4.binding is None,
          "unbound with no target records the preset (%s)" % info)
    state4.activeDescription = "/Groom/Description"
    ok, info = loop4.rebindPreset(work, "width")
    check(ok and state4.binding is not None
          and state4.binding.primvar == "usdGen:paint:width"
          and str(state4.binding.mapPath)
          == "/Groom/Description/Maps/widthPaint",
          "unbound with a target paints to it (%s)" % info)
    # A blocked bind fails closed on the live binding and preset.
    lock = Usd.Stage.CreateInMemory()
    quad = UsdGeom.Mesh.Define(lock, "/Plane")
    quad.CreatePointsAttr([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)])
    quad.CreateFaceVertexCountsAttr([4])
    quad.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    lock.DefinePrim("/BrushMaps/widthPaint", "Scope")
    state5 = brushState.BrushToolState()
    loop5 = brushLoop.BrushLoop(state5)
    ok, _info = loop5.bindFromSelection(
        lock, ["/Plane"], resolution=8, mapName="densityPaint",
        primvar="usdGen:paint:density", channels=1, defaultValue=1.0)
    check(ok, "the loop binds the locked quad")
    failed, error = loop5.rebindPreset(lock, "width")
    check(failed is False and error
          and state5.binding.primvar == "usdGen:paint:density"
          and state5.maskPreset == "density",
          "a blocked rebind fails closed on the live binding")


# -- flood value ------------------------------------------------------

def testFlood(stagePath):
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    ok, error = loop.flood(None)
    check(ok is False and error, "flood with no stage fails closed")
    stage = Usd.Stage.Open(stagePath)
    ok, error = loop.flood(stage)
    check(ok is False and "binding" in error,
          "flood with no binding fails closed")
    ok, info = loop.bindFromSelection(
        stage, ["/Plane"], resolution=8, mapName="densityPaint",
        primvar="usdGen:paint:density", channels=1, defaultValue=1.0)
    check(ok, "the loop binds density (%s)" % info)
    state.value = 0.25
    ok, info = loop.flood(stage)
    check(ok, "flood fills the bound map (%s)" % info)
    values = brushAuthor.BakedValues(stage, state.binding)
    check(values is not None and len(values) == 4 * 64
          and min(values) == max(values)
          and abs(values[0] - 0.25) < 1e-6,
          "every corner floods uniformly (%.6f)" % (values[0] if values else -1.0))
    check(state.undoStack.undoLabel() == "Flood usdGen:paint:density",
          "the flood pushes a labelled undo entry")
    drawn = brushPreview.PreviewColors(stage, state.binding)
    check(drawn is not None and min(drawn) == max(drawn),
          "and the display shows the flood")
    state.value = 5.0
    ok, _info = loop.flood(stage)
    values = brushAuthor.BakedValues(stage, state.binding)
    check(ok and min(values) == max(values) == 1.0,
          "the flood clamps to the value range")
    state.value = 0.0
    state.channel = 3
    ok, error = loop.flood(stage)
    check(ok is False and "channel" in error,
          "an out-of-range channel fails closed")
    state.channel = 0
    ok, _info = loop.flood(stage)
    check(ok and min(brushAuthor.BakedValues(stage, state.binding)) == 0.0,
          "channel 0 floods a one-channel binding")
    state.channel = -1
    ok, _error = loop.undo()
    values = brushAuthor.BakedValues(stage, state.binding)
    check(ok and min(values) == max(values) == 1.0,
          "undo restores the pre-flood map")
    ok, _error = loop.redo()
    check(ok and min(brushAuthor.BakedValues(stage, state.binding)) == 0.0,
          "redo re-floods it")
    camera = fixtureCamera()
    state.value = 0.5
    captured, _info = loop.press(stage, camera, 255.0, 145.0)
    check(captured, "press starts a stroke")
    refused, error = loop.flood(stage)
    check(refused is False and "live stroke" in error,
          "a flood mid-stroke is refused")
    loop.cancel(stage)

# -- hardness / erase / LiveStroke / resolution / backends -------------

def testHardnessErase():
    check(brushMap.brushWeight(brushMap.FALLOFF_SMOOTH, 0.5, 0.2, 1.0)
          == 1.0, "inside the inner radius the weight is 1")
    check(near(brushMap.brushWeight(brushMap.FALLOFF_LINEAR, 0.5, 0.75,
                                    1.0), 0.5),
          "the falloff runs from the inner radius to the rim")
    check(brushMap.brushWeight(brushMap.FALLOFF_LINEAR, 0.0, 1.5, 1.0)
          == 0.0, "past the rim the weight is 0")
    check(near(brushMap.brushWeight(brushMap.FALLOFF_LINEAR, 0.0, 0.25,
                                    1.0), 0.75),
          "hardness 0 is the plain falloff")
    grid, _error = brushMap.BrushMap.create(spec(1, 8))
    soft = brushMap.BrushStroke(grid)
    soft.addDab(brushMap.BrushDab(0, 0.5, 0.5, radius=0.5, strength=1.0,
                                  value=1.0))
    hard = brushMap.BrushStroke(grid)
    hard.addDab(brushMap.BrushDab(0, 0.5, 0.5, radius=0.5, strength=1.0,
                                  value=1.0, hardness=0.9))
    _ok, softEdge = soft.preview().getTexel(0, 1, 3, 0)
    _ok, hardEdge = hard.preview().getTexel(0, 1, 3, 0)
    check(hardEdge > softEdge + 0.2,
          "a hard dab paints nearer the rim (%.3f vs %.3f)"
          % (hardEdge, softEdge))
    ok, error = hard.addDab(brushMap.BrushDab(0, 0.5, 0.5, hardness=1.5))
    check(not ok and error, "a hardness past 1 is rejected")
    ok, _error = hard.addMove(0, 0.6, 0.5, 0.5, 1.0, 1.0, -1,
                              brushMap.MODE_SET, brushMap.FALLOFF_SMOOTH,
                              0.5, 0.9)
    check(ok and hard.dab(hard.dabCount() - 1).hardness == 0.9,
          "addMove carries hardness")

    base, _error = brushMap.BrushMap.create(
        brushMap.BrushMapSpec(1, 8, 1, 0.25, False))
    stroke = brushMap.BrushStroke(base)
    stroke.addDab(brushMap.BrushDab(0, 0.5, 0.5, radius=0.6, strength=1.0,
                                    value=1.0, hardness=1.0))
    stroke.addDab(brushMap.BrushDab(0, 0.5, 0.5, radius=0.6, strength=1.0,
                                    value=9.0, mode=brushMap.MODE_ERASE,
                                    hardness=1.0))
    check(stroke.preview().getTexel(0, 4, 4, 0) == (True, 0.25),
          "erase returns paint to the map default, ignoring value")
    check(brushMap.MODE_ERASE in brushMap.MODES, "erase is a mode")


def _gridPlane(n, faceMask=None):
    """n x n unit quads on y = 0: face (i, j) spans x [i, i+1], z [j, j+1]."""
    points, faces = [], []
    for j in range(n + 1):
        for i in range(n + 1):
            points.append((float(i), 0.0, float(j)))
    for j in range(n):
        for i in range(n):
            v = j * (n + 1) + i
            faces.append((v, v + 1, v + n + 2, v + n + 1))
    return brushPick.MeshSnapshot(points, faces, faceMask)


def _hardEdgeBase(n, res=8):
    import numpy as np
    corners = np.zeros((n * n, 4, 1))
    for j in range(n):
        for i in range(n):
            corners[j * n + i] = 1.0 if j <= 1 else 0.0
    grid, error = brushMap.CornerGrid.create(
        brushMap.BrushMapSpec(n * n, res, 1, 1.0, False), corners)
    check(grid is not None, "the hard-edge base builds (%s)" % error)
    return grid


def _smoothEdge(native):
    n = 4
    snapshot = _gridPlane(n)
    base = _hardEdgeBase(n)
    live, error = brushMap.LiveStroke.create(snapshot, base, native=native)
    check(live is not None and live.native == native,
          "a %s live stroke builds (%s)"
          % ("native" if native else "python", error))
    if live is None:
        return None
    # One stroke along the edge on the upper rows' v = 1 border.
    for i in range(n):
        count, error = live.dab(n + i, 0.5, 1.0, 1.0, 0.5, 1.0, 0.0, 0,
                                brushMap.MODE_SMOOTH,
                                brushMap.FALLOFF_SMOOTH, False)
        check(count > 0, "smooth dab %d applies (%s)" % (i, error))
    corners = live.workingCorners()
    worst = 1.0
    for i in (1, 2):
        upper = corners[n + i, 3, 0]
        lower = corners[2 * n + i, 0, 0]
        worst = min(worst, 1.0 - upper, lower)
    check(worst > 0.15,
          "%s smooth softens a corner-only hard edge by > 0.15 on both "
          "sides (min %.3f)" % ("native" if native else "python", worst))
    touched = live.takeTouched()
    check(n + 1 in touched and 2 * n + 1 in touched
          and touched == sorted(touched),
          "smooth reports the faces it moved, ascending")
    committed = brushMap.cornerBufferOf(live.commitGrid())
    check(abs(committed - corners).max() < 1e-6,
          "commit reproduces the smoothed working grid")
    out = corners.copy()
    live.close()
    return out


def testLiveStroke():
    python = _smoothEdge(False)
    native = _smoothEdge(True) if brushApi.available() else None
    if native is not None and python is not None:
        check(abs(native - python).max() < 1e-5,
              "native and python smooth agree (max |d| %.2e)"
              % abs(native - python).max())
    elif not brushApi.available():
        print("ok: native LiveStroke skipped (%s)" % brushApi.reason())

    # Set / erase / move trail, both backends, same answer.
    results = []
    for native in ((False, True) if brushApi.available() else (False,)):
        snapshot = _gridPlane(3)
        base, _error = brushMap.CornerGrid.create(
            brushMap.BrushMapSpec(9, 8, 1, 0.0, False))
        live, error = brushMap.LiveStroke.create(snapshot, base,
                                                 native=native)
        check(live is not None, "a live stroke builds (%s)" % error)
        live.dab(4, 0.2, 0.3, 0.8, 0.5, 1.0, 1.0, 0, brushMap.MODE_SET,
                 brushMap.FALLOFF_SMOOTH)
        live.dab(4, 0.9, 0.8, 0.8, 0.5, 1.0, 1.0, 0, brushMap.MODE_SET,
                 brushMap.FALLOFF_SMOOTH, True, 0.5)
        live.dab(4, 0.5, 0.5, 0.4, 1.0, 0.5, 0.0, 0, brushMap.MODE_ERASE,
                 brushMap.FALLOFF_LINEAR)
        count, error = live.dab(99, 0.5, 0.5, 0.4, 0.0, 1.0, 1.0, 0,
                                brushMap.MODE_SET, brushMap.FALLOFF_SMOOTH)
        check(count < 0 and error, "a bad face is rejected (%s)" % error)
        check(len(live.calls) == 3 and live.calls[0].face == 4
              and live.calls[2].mode == brushMap.MODE_ERASE,
              "the call record keeps accepted calls only")
        check(live.dabCount() > 3, "the move recorded an interpolated trail")
        grid = live.workingGrid()
        ok, corners = grid.cornerValues(4, 0)
        check(ok and max(corners) > 0.3,
              "the working grid carries paint (%r)" % (corners,))
        check(live.takeTouched() and not live.takeTouched(),
              "takeTouched drains")
        colors = live.previewColors(0, "gray", 0.0, 1.0)
        check(colors.shape == (36, 3)
              and near(float(colors[16, 0]), corners[0], 1e-6),
              "preview colours follow the working corners")
        results.append(live.workingCorners())
        live.abort()
        check(live.dabCount() == 0 and float(abs(
            live.workingCorners()).max()) == 0.0,
            "abort restores the base")
        live.close()
    if len(results) == 2:
        check(abs(results[0] - results[1]).max() < 1e-5,
              "native and python set/erase/trail agree (max |d| %.2e)"
              % abs(results[0] - results[1]).max())


def testSuggestResolution(stagePath):
    small = _gridPlane(4)
    res, info = _pythonOnly(brushPick.suggestResolution, small)
    check(res == 32 and info.startswith("32 px/face"),
          "python: a small mesh gets 32 px/face (%s)" % info)
    res, info = _pythonOnly(brushPick.suggestResolution, small, 16 * 64)
    check(res == 8, "python: the budget lowers it (%s)" % info)
    if brushApi.available():
        nres, ninfo = brushPick.suggestResolution(_gridPlane(4), 16 * 64)
        check(nres == 8 and ninfo.split(" ")[0] == info.split(" ")[0],
              "native agrees (%s)" % ninfo)
    stage = Usd.Stage.Open(stagePath)
    binding, error = brushAuthor.BindSurface(stage, "/Plane")
    check(binding is not None and binding.resolutionAuto
          and binding.resolution == 32,
          "a bind without resolution uses the suggestion (%s, %s)"
          % (error, binding.resolutionInfo if binding else ""))
    fixed, _error = brushAuthor.BindSurface(stage, "/Plane", resolution=8)
    check(fixed is not None and not fixed.resolutionAuto
          and fixed.resolution == 8, "an explicit resolution wins")


def testNativePick():
    if not brushApi.available():
        print("ok: native pick skipped (%s)" % brushApi.reason())
        return
    snapshot = _gridPlane(6)
    rays = [((x + 0.37, 3.0, z + 0.61), (0.0, -1.0, 0.0))
            for x in range(6) for z in range(6)]
    rays.append(((2.5, -2.0, 2.5), (0.0, 1.0, 0.0)))
    rays.append(((50.0, 3.0, 50.0), (0.0, -1.0, 0.0)))
    drift = 0
    for origin, direction in rays:
        want = brushPick._pickBrute(snapshot, origin, direction)
        got = brushPick.pickFace(snapshot, origin, direction)
        if (want is None) != (got is None):
            drift += 1
        elif want is not None and (
                want[0] != got[0] or not near(want[1], got[1], 1e-5)
                or not near(want[2], got[2], 1e-5)):
            drift += 1
    check(drift == 0 and snapshot.nativeMesh,
          "the native pick agrees with brute force on %d rays"
          % len(rays))
    foot = brushPick.DabFootprint(snapshot)
    native = foot.expand(8, 0.95, 0.5, (2.95, 0.0, 1.5), 0.3)
    python = _pythonOnly(lambda: brushPick.DabFootprint(
        _gridPlane(6)).expand(8, 0.95, 0.5, (2.95, 0.0, 1.5), 0.3))
    check([f[0] for f in native] == [f[0] for f in python]
          and all(near(a[1], b[1], 1e-5) and near(a[2], b[2], 1e-5)
                  and near(a[3], b[3], 1e-5)
                  for a, b in zip(native, python)),
          "native and python footprints agree (%d faces)" % len(native))


def _bandStroke(n=12, res=8, native=False, perStamp=False):
    """One straight strength-1 set stroke along the z = 6 vertex row.

    World radius 3 faces, smooth falloff, hardness 0; returns the corner-0
    values of faces (6, 6..9): distance 0..3 from the centreline.
    perStamp=True replays the same trail the old way -- the "before":
    the per-stamp kernel (applyDab on every overlapping stamp) with spill
    stamps centred at the clamped closest point on each neighbour."""
    snapshot = _gridPlane(n)
    base, _error = brushMap.CornerGrid.create(
        brushMap.BrushMapSpec(n * n, res, 1, 0.0, False))
    calls = []
    for i in range(2, 10):
        calls.append((5 * n + i, 0.0, 1.0, False, 0.5))
        calls.append((5 * n + i, 1.0, 1.0, True, 0.25))
    if perStamp:
        grid = base.dense().clone()
        trail = brushMap.BrushStroke(grid)
        foot = brushPick.DabFootprint(snapshot)
        for face, u, v, isMove, spacing in calls:
            before = trail.dabCount()
            if isMove:
                trail.addMove(face, u, v, 3.0, 1.0, 1.0, 0, brushMap.MODE_SET,
                              brushMap.FALLOFF_SMOOTH, spacing, 0.0)
            else:
                trail.addDab(brushMap.BrushDab(face, u, v, 3.0, 1.0, 1.0, 0,
                                               brushMap.MODE_SET,
                                               brushMap.FALLOFF_SMOOTH, 0.0))
            for index in range(before, trail.dabCount()):
                dab = trail.dab(index)
                point = brushPick.facePoint(snapshot, dab.face, dab.u, dab.v)
                for f, su, sv, r in foot.expand(dab.face, dab.u, dab.v, point,
                                                dab.radius):
                    if f != dab.face:
                        # The old spill: centred at the CLAMPED closest
                        # point on the neighbour's border.
                        _d, su, sv = brushPick.quadClosest(snapshot, f, point)
                    stamp = dab.copy()
                    stamp.face, stamp.u, stamp.v, stamp.radius = f, su, sv, r
                    brushMap.applyDab(grid, stamp)
        corners = brushMap.cornerBufferOf(grid)
        return ([float(corners[(6 + d) * n + 6, 0, 0]) for d in range(4)],
                None)
    live, error = brushMap.LiveStroke.create(snapshot, base, native=native)
    for face, u, v, isMove, spacing in calls:
        live.dab(face, u, v, 3.0, 0.0, 1.0, 1.0, 0, brushMap.MODE_SET,
                 brushMap.FALLOFF_SMOOTH, isMove, spacing)
    corners = live.workingCorners()
    committed = brushMap.cornerBufferOf(live.commitGrid())
    same = abs(committed - corners).max() < 1e-6
    live.close()
    return ([float(corners[(6 + d) * n + 6, 0, 0]) for d in range(4)], same)


def testFalloffBand():
    """Overlapping set stamps keep the falloff (max weight per stroke)."""
    before, _same = _pythonOnly(_bandStroke, 12, 8, False, True)
    print("ok: falloff band BEFORE (per-stamp re-apply, clamped spill), "
          "distance 0..3: "
          + " ".join("%.4f" % x for x in before))
    backends = [False] + ([True] if brushApi.available() else [])
    levels = {}
    for native in backends:
        name = "native" if native else "python"
        if native:
            after, same = _bandStroke(native=True)
        else:
            after, same = _pythonOnly(_bandStroke, 12, 8, False, False)
        levels[name] = after
        print("ok: falloff band AFTER (%s), distance 0..3: %s"
              % (name, " ".join("%.4f" % x for x in after)))
        distinct = len(set(round(x, 4) for x in after))
        check(after[0] == 1.0 and after[0] > after[1] > after[2] > after[3]
              and distinct >= 3,
              "%s: a strength-1 set stroke decreases monotonically with "
              "distance (%d levels)" % (name, distinct))
        check(same, "%s: the band commits exactly the working grid" % name)
    check(before[1] > 0.99 and before[2] > 0.99,
          "the per-stamp kernel saturates the band (the reported bug)")
    if len(levels) == 2:
        check(max(abs(a - b) for a, b in zip(levels["python"],
                                              levels["native"])) < 1e-5,
              "native and python bands agree")


def testPreviewFallback(stagePath):
    """No render chain in a T1 process: previews use the session path."""
    stage = Usd.Stage.Open(stagePath)
    binding, _error = brushAuthor.BindSurface(stage, "/Plane", resolution=8)
    check(not brushPreview.overlayActive(),
          "no preview scene index is live in a plain interpreter")
    base, _error = brushAuthor.BaseGridFromStage(stage, binding)
    check(isinstance(base, brushMap.CornerGrid),
          "the base grid is corner-backed")
    ok, info = brushPreview.SetPreview(stage, binding, base)
    check(ok and info == "session", "SetPreview falls back (%s)" % info)
    snapshot, _error = brushPick.snapshotMesh(stage.GetPrimAtPath("/Plane"))
    live, _error = brushMap.LiveStroke.create(snapshot, base)
    live.dab(10, 0.5, 0.5, 1.0, 1.0, 1.0, 1.0, 0, brushMap.MODE_SET,
             brushMap.FALLOFF_SMOOTH)
    ok, info = brushPreview.SetPreview(stage, binding, live)
    colors = brushPreview.PreviewColors(stage, binding)
    check(ok and max(c[0] for c in colors) > 0.9,
          "a LiveStroke previews its working grid (%s)" % info)
    ok, error = brushAuthor.WriteLivePrimvar(stage, binding, live)
    values = _liveValues(stage, binding)
    check(ok and max(values) > 0.9, "and live-writes it (%s)" % error)
    ok, error = brushAuthor.BakeStroke(stage, binding, live.commitGrid())
    check(ok and max(brushAuthor.BakedValues(stage, binding)) > 0.9,
          "and bakes its commit grid (%s)" % error)
    live.close()
    ok, _info = brushPreview.ClearPreview(stage, binding)
    check(ok, "the session preview clears")
    if not brushApi.available():
        return
    # The overlay path (forced: no render chain here). The registry still
    # takes the colours; the client remembers what it pushed.
    saved = brushPreview.overlayActive
    brushPreview.overlayActive = lambda: True
    try:
        ok, info = brushPreview.SetPreview(stage, binding, base)
        shown = brushPreview.ShownColors(stage, binding)
        check(ok and info == "overlay" and shown is not None
              and len(shown) == 4 * 64
              and brushPreview.PreviewColors(stage, binding) == shown,
              "the overlay reports its pushed colours (%s)" % info)
        check(stage.GetSessionLayer().GetAttributeAtPath(Sdf.Path(
            "/Plane.primvars:displayColor")) is None,
            "and writes nothing to the stage")
        check(brushPreview.HasPreview(stage, binding),
              "HasPreview sees the overlay")
        ok, info = brushPreview.ClearPreview(stage, binding)
        check(ok and not brushPreview.HasPreview(stage, binding)
              and brushPreview.ShownColors(stage, binding) is None,
              "clearing the overlay forgets its colours (%s)" % info)
    finally:
        brushPreview.overlayActive = saved



# -- GeomSubsets: the face mask and subset-bound descriptions -------------

def _leftColumns(n, cols):
    """Face ids of columns 0..cols-1 of an n x n grid (face = j * n + i)."""
    return [j * n + i for j in range(n) for i in range(cols)]


def _maskedPick():
    n = 4
    mask = _leftColumns(n, 2)
    snapshot = _gridPlane(n, mask)
    down = (0.0, -1.0, 0.0)
    inside = brushPick.pickFace(snapshot, (1.5, 3.0, 1.5), down)
    outside = brushPick.pickFace(snapshot, (2.5, 3.0, 1.5), down)
    brute = brushPick._pickBrute(snapshot, (2.5, 3.0, 1.5), down)
    foot = brushPick.DabFootprint(snapshot).expand(
        5, 0.95, 0.5, (1.95, 0.0, 1.5), 0.6)
    return (inside, outside, brute, foot)


def testMaskedPick():
    python = _pythonOnly(_maskedPick)
    inside, outside, brute, foot = python
    check(inside is not None and inside[0] == 5,
          "python: a pick inside the face mask hits face 5")
    check(outside is None and brute is None,
          "python: a pick on a masked face misses (index and brute force)")
    check([f[0] for f in foot] == [5, 1, 9],
          "python: the footprint spills onto subset faces only (%r)"
          % [f[0] for f in foot])
    snapshot = _gridPlane(4)
    check(snapshot.faceMask is None and snapshot.paintable(6),
          "an unmasked snapshot paints every face")
    if not brushApi.available():
        print("ok: native masked pick skipped (%s)" % brushApi.reason())
        return
    native = _maskedPick()
    check(native[0] is not None and native[0][0] == 5
          and native[1] is None,
          "native: the same hit and the same masked miss")
    check([f[0] for f in native[3]] == [f[0] for f in foot]
          and all(near(a[1], b[1], 1e-5) and near(a[2], b[2], 1e-5)
                  and near(a[3], b[3], 1e-5)
                  for a, b in zip(native[3], foot)),
          "native and python masked footprints agree")
    masked = brushApi.meshFor(_gridPlane(4, [0, 1]))
    check(masked is not None and masked.faceInMask(1)
          and not masked.faceInMask(2),
          "the native mesh carries the snapshot's face mask")


def _maskedStroke(native):
    """Set, move trail, smooth and erase on a 4 x 4 grid masked to its
    two left columns; masked faces start at 0.75, subset faces at 0.25."""
    import numpy as np
    n = 4
    mask = _leftColumns(n, 2)
    snapshot = _gridPlane(n, mask)
    corners = np.full((n * n, 4, 1), 0.25)
    for face in range(n * n):
        if face not in mask:
            corners[face] = 0.75
    base, error = brushMap.CornerGrid.create(
        brushMap.BrushMapSpec(n * n, 8, 1, 0.0, False), corners)
    live, error = brushMap.LiveStroke.create(snapshot, base, native=native)
    label = "native" if native else "python"
    check(live is not None and live.native == native,
          "a masked %s live stroke builds (%s)" % (label, error))
    if live is None:
        return None
    count, error = live.dab(6, 0.5, 0.5, 0.5, 1.0, 1.0, 1.0, 0,
                            brushMap.MODE_SET, brushMap.FALLOFF_SMOOTH)
    check(count < 0 and error and live.dabCount() == 0,
          "%s: a dab on a masked face is rejected (%s)" % (label, error))
    live.dab(5, 1.0, 0.5, 1.5, 0.5, 1.0, 1.0, 0, brushMap.MODE_SET,
             brushMap.FALLOFF_SMOOTH)
    live.dab(9, 1.0, 0.9, 1.5, 0.5, 1.0, 1.0, 0, brushMap.MODE_SET,
             brushMap.FALLOFF_SMOOTH, True, 0.5)
    live.dab(5, 1.0, 0.5, 1.0, 0.5, 1.0, 0.0, 0, brushMap.MODE_SMOOTH,
             brushMap.FALLOFF_SMOOTH)
    live.dab(1, 0.9, 0.2, 0.6, 0.0, 0.5, 0.0, 0, brushMap.MODE_ERASE,
             brushMap.FALLOFF_LINEAR)
    touched = live.takeTouched()
    check(touched and all(f in mask for f in touched),
          "%s: only subset faces come back touched (%r)" % (label, touched))
    working = live.workingCorners()
    outside = [f for f in range(n * n) if f not in mask]
    check(float(abs(working[outside] - 0.75).max()) == 0.0,
          "%s: masked faces keep their base corners exactly" % label)
    check(float(working[mask].max()) > 0.9,
          "%s: subset faces carry the paint" % label)
    committed = brushMap.cornerBufferOf(live.commitGrid())
    check(float(abs(committed - working).max()) < 1e-6,
          "%s: the masked stroke commits its working grid" % label)
    live.close()
    return working


def testMaskedLiveStroke():
    python = _pythonOnly(_maskedStroke, False)
    if not brushApi.available():
        print("ok: native masked stroke skipped (%s)" % brushApi.reason())
        return
    native = _maskedStroke(True)
    if python is not None and native is not None:
        check(abs(native - python).max() < 1e-5,
              "native and python agree under a face mask (max |d| %.2e)"
              % abs(native - python).max())


def _defineSubset(stage, path, indices, elementType="face"):
    subset = UsdGeom.Subset.Define(stage, path)
    subset.CreateElementTypeAttr(elementType)
    subset.CreateIndicesAttr(Vt.IntArray([int(i) for i in indices]))
    return subset


def _outsideSame(values, before, faces, n=64):
    """True when every corner of the faces NOT in `faces` is unchanged."""
    inside = set(faces)
    return all(values[4 * f + k] == before[4 * f + k]
               for f in range(n) if f not in inside for k in range(4))


def _subsetFlow(stagePath, label):
    """Descriptions on /Plane's left and right halves (face GeomSubsets):
    binds, strokes, live writes, smooth and flood touch only their half."""
    stage = Usd.Stage.Open(Sdf.Layer.OpenAsAnonymous(stagePath))
    # The fixture's points run z-fastest: face = xColumn * 8 + zRow, so
    # faces 0..31 are the x < 0 half.
    left = list(range(32))
    right = list(range(32, 64))
    _defineSubset(stage, "/Plane/Left", left)
    _defineSubset(stage, "/Plane/Right", right)
    # Paint someone else already put on the whole mesh.
    UsdGeom.PrimvarsAPI(stage.GetPrimAtPath("/Plane")).CreatePrimvar(
        "usdGen:paint:density", Sdf.ValueTypeNames.FloatArray,
        "faceVarying").GetAttr().Set(Vt.FloatArray([0.3] * 256))
    state = brushState.BrushToolState()
    loop = brushLoop.BrushLoop(state)
    camera = fixtureCamera()

    target, error = brushAuthor.ResolveSurface(stage, "/Plane/Left")
    check(target is not None and target.meshPath == Sdf.Path("/Plane")
          and target.faces == tuple(left),
          "%s: a face subset resolves to its parent and faces (%s)"
          % (label, error))
    ok, info = loop.setupDescriptionFromSelection(stage, ["/Plane/Left"])
    check(ok, "%s: setup grows a description from a subset (%s)"
          % (label, info))
    descLeft = Sdf.Path(state.activeDescription)
    binding = state.binding
    check(binding is not None and binding.surfacePath == Sdf.Path("/Plane")
          and binding.subsetPath == Sdf.Path("/Plane/Left")
          and binding.faces == tuple(left),
          "%s: the binding paints the parent's primvar on the subset faces"
          % label)
    desc = stage.GetPrimAtPath(descLeft)
    check(desc.GetRelationship("usdGen:surface").GetTargets()
          == [Sdf.Path("/Plane/Left")],
          "%s: usdGen:surface targets the subset" % label)
    paintMap = stage.GetPrimAtPath(binding.mapPath)
    check(paintMap.GetRelationship("usdGen:paint:surface").GetTargets()
          == [Sdf.Path("/Plane/Left")],
          "%s: the PaintMap's usdGen:paint:surface targets the subset"
          % label)
    check(brushAuthor.DescriptionSurface(stage, descLeft)
          == (Sdf.Path("/Plane/Left"), ""),
          "%s: DescriptionSurface reads the subset back" % label)
    check("bound: /Plane/Left" in brushPanels.statusText(state, ""),
          "%s: the status line names the subset" % label)

    before = brushAuthor.BakedValues(stage, binding)
    state.strength = 1.0
    state.hardness = 1.0
    state.radiusWorld = 0.6
    state.value = 1.0
    # Face 45 (x column 5) is on the right half: a miss, not a stroke.
    captured, info = loop.press(stage, camera, 255.0, 145.0)
    check(not captured and not loop.gestureActive(),
          "%s: a press outside the subset is a miss (%s)" % (label, info))
    # Face 28, just left of x = 0: a disk that reaches well past the border.
    captured, info = loop.press(stage, camera, 195.0, 175.0)
    check(captured, "%s: a press inside the subset strokes (%s)"
          % (label, info))
    loop.move(stage, 195.0, 160.0)
    session = stage.GetSessionLayer().GetAttributeAtPath(
        "/Plane.primvars:usdGen:paint:density")
    live = list(session.default) if session is not None else None
    check(live is not None and _outsideSame(live, before, left),
          "%s: the live groom write keeps the other half's corners" % label)
    baked, info = loop.release(stage)
    check(baked, "%s: the subset stroke bakes (%s)" % (label, info))
    values = brushAuthor.BakedValues(stage, binding)
    check(_outsideSame(values, before, left),
          "%s: the bake never writes a face outside the subset" % label)
    check(max(values[4 * f + k] for f in left for k in range(4)) > 0.9,
          "%s: and paints inside it" % label)
    check(not [p.GetName() for p in
               stage.GetPrimAtPath("/Plane/Left").GetAuthoredProperties()
               if p.GetName().startswith("primvars:")],
          "%s: nothing is authored on the GeomSubset itself" % label)

    state.activeBrush = "smooth"
    painted = list(values)
    loop.press(stage, camera, 195.0, 175.0)
    loop.move(stage, 195.0, 160.0)
    baked, info = loop.release(stage)
    values = brushAuthor.BakedValues(stage, binding)
    check(baked and _outsideSame(values, before, left)
          and values != painted,
          "%s: smooth relaxes the subset and nothing else (%s)"
          % (label, info))
    state.activeBrush = "paint"

    state.value = 0.8
    ok, info = loop.flood(stage)
    values = brushAuthor.BakedValues(stage, binding)
    check(ok and _outsideSame(values, before, left)
          and all(abs(values[4 * f + k] - 0.8) < 1e-6
                  for f in left for k in range(4)),
          "%s: flood fills the subset faces only (%s)" % (label, info))

    # A second description on the other half: its own target, its own faces.
    state.activeDescription = ""
    ok, info = loop.setupDescriptionFromSelection(stage, ["/Plane/Right"])
    descRight = Sdf.Path(state.activeDescription)
    check(ok and descRight != descLeft
          and state.binding.faces == tuple(right),
          "%s: a second subset grows its own description (%s)"
          % (label, info))
    check(brushAuthor.DescriptionForSurface(stage, "/Plane/Left")[0]
          == descLeft
          and brushAuthor.DescriptionForSurface(stage, "/Plane/Right")[0]
          == descRight
          and brushAuthor.DescriptionForSurface(stage, "/Plane")[0] is None,
          "%s: DescriptionForSurface tells subsets of one mesh apart"
          % label)
    leftPaint = brushAuthor.BakedValues(stage, state.binding)
    state.value = 0.1
    ok, info = loop.flood(stage)
    values = brushAuthor.BakedValues(stage, state.binding)
    check(ok and _outsideSame(values, leftPaint, right)
          and all(abs(values[4 * f + k] - 0.1) < 1e-6
                  for f in right for k in range(4)),
          "%s: flooding the right half keeps the left half's paint"
          % label)
    state.activeDescription = ""
    ok, info = loop.setupDescriptionFromSelection(stage, ["/Plane/Left"])
    check(ok and Sdf.Path(state.activeDescription) == descLeft
          and len(brushAuthor.ListDescriptions(stage)) == 2,
          "%s: setup on a subset adopts its description (%s)"
          % (label, info))

    # Widening the subset widens the next flood (the binding re-reads it).
    rightPaint = brushAuthor.BakedValues(stage, state.binding)
    UsdGeom.Subset(stage.GetPrimAtPath("/Plane/Left")).GetIndicesAttr().Set(
        Vt.IntArray(list(range(40))))
    state.value = 0.5
    ok, info = loop.flood(stage)
    values = brushAuthor.BakedValues(stage, state.binding)
    check(ok and values[4 * 32] == values[0]
          and abs(values[4 * 32] - 0.5) < 1e-6
          and _outsideSame(values, rightPaint, list(range(40))),
          "%s: a widened subset floods its new faces too (%s)"
          % (label, info))

    # Invalid subsets fail with their reason.
    _defineSubset(stage, "/Plane/Points", [0, 1], "point")
    _defineSubset(stage, "/Plane/Wide", [0, 64])
    stage.DefinePrim("/Loose", "Xform")
    _defineSubset(stage, "/Loose/Sub", [0])
    nope, error = brushAuthor.BindSurface(stage, "/Plane/Points")
    check(nope is None and "\"face\"" in error,
          "%s: a point subset is rejected (%s)" % (label, error))
    nope, error = brushAuthor.BindSurface(stage, "/Plane/Wide")
    check(nope is None and "names face 64" in error,
          "%s: an out-of-range subset index is rejected (%s)"
          % (label, error))
    nope, error = brushAuthor.EnsureDescription(stage, "/Loose/Sub")
    check(nope is None and "not the child of a Mesh" in error,
          "%s: a subset off a non-mesh is rejected (%s)" % (label, error))
    state.activeDescription = ""
    ok, error = loop.bindFromSelection(stage, ["/Plane/Points"])
    check(not ok and "face" in error,
          "%s: binding a selected point subset says why (%s)"
          % (label, error))


def testSubsetBinding(stagePath):
    _pythonOnly(_subsetFlow, stagePath, "python")
    if brushApi.available():
        _subsetFlow(stagePath, "native")
    else:
        print("ok: native subset flow skipped (%s)" % brushApi.reason())


def main():
    if len(sys.argv) < 2:
        print("FAIL: expected the path of test-plane.usda")
        return 1
    testMapSpec()
    testTexels()
    testSampling()
    testDabMath()
    testStrokeContract()
    testCameraMath()
    testPick()
    testPickIndex()
    testRing()
    testExpand()
    testBind(sys.argv[1])
    testMaskPresets(sys.argv[1])
    testLoopStroke(sys.argv[1])
    testBrushRadius(sys.argv[1])
    testSmoothSoftens(sys.argv[1])
    testSessionEditTarget(sys.argv[1])
    testPreviewPrior(sys.argv[1])
    testDescriptions(sys.argv[1])
    testPaintWiring()
    testUpsample()
    testPreviewColors()
    testPreviewPatch(sys.argv[1])
    testLivePatch(sys.argv[1])
    testStrokeHardening(sys.argv[1])
    testPresetSwitch(sys.argv[1])
    testFlood(sys.argv[1])
    testHardnessErase()
    testLiveStroke()
    testSuggestResolution(sys.argv[1])
    testNativePick()
    testPreviewFallback(sys.argv[1])
    testFalloffBand()
    testMaskedPick()
    testMaskedLiveStroke()
    testSubsetBinding(sys.argv[1])
    if FAILURES:
        print("FAIL: %d check(s) failed" % len(FAILURES))
        return 1
    print("PASS: usdGenTools brush map, pick, bind, loop, presets, descriptions, upsample, patching, hardening, display, preset switch, flood, hardness/erase, LiveStroke (%s), auto resolution, preview fallback and GeomSubset face masks"
          % ("native + python" if brushApi.available() else "python only: " + brushApi.reason()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
