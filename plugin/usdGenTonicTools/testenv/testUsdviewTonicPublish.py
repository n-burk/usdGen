# testUsdviewTonicPublish -- T3: the viewport actually draws the hierarchy
# (plan/18 V0 exit).
#
#   testusdview --testScript plugin/usdGenTonicTools/testenv/testUsdviewTonicPublish.py \
#               examples/tonic-graph-scalp.usda
#
# with the same PXR_PLUGINPATH_NAME / PYTHONPATH the other T3 tonic scripts
# use, plus the staged usdGenTonicTools package and the usdGenTonic DLL (via
# USDGENTONIC_DLL or the build tree). The scalp is the 4x4 quad grid in XZ:
# face f = ix*4 + iz covers x in [ix, ix+1], z in [iz, iz+1], and (x, z) on
# that face is (u = z - iz, v = x - ix).
#
# This is the script that closes plan/18 finding F1: before V0 the tools and
# the scene index each owned their own TonicModel and nothing joined them, so
# every edit was invisible. Here one model is created through the C ABI,
# edited, activated and published, and the assertions are made against what
# Hydra ended up with -- not against the model that was edited.
#
# Proven here:
#   * a stroked region builds an L1 tube and a subdivide adds four L2
#     children, all in one model;
#   * after Tonic_Activate + Tonic_Publish the scene index publishes
#     /__usdGenTonic/tubes/L1 and /__usdGenTonic/tubes/L2 with exactly the
#     face and point counts the model's own census predicts;
#   * the static test tube is drawn while no model is active and is gone
#     from the frame once one is;
#   * Hydra really rasterises the levels: the framebuffer carries the
#     clump colour where the tubes are, hiding both levels clears it, and
#     showing either one alone brings it back;
#   * V1: selecting the tube through Tonic_SelectSet brightens exactly
#     those pixels, which is the `selected` primvar arriving at the
#     shader, and clearing the selection puts the frame back.
#
# The proof reads PIXELS rather than picks. UsdImagingGLEngine resolves a
# pick hit back to a USD prim path, and the Tonic prims are published by a
# scene index with no USD origin, so their hits carry no path to report:
# view.pick() can never name /__usdGenTonic/tubes/L1 however well it is
# drawn. That is also why plan/18 section 3.2 routes tool picking through
# K11 (Tonic_Pick) instead of view.pick().
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


def locate(x, z):
    """(face, u, v) for a scalp position (x, z)."""
    ix = min(max(int(math.floor(x)), 0), 3)
    iz = min(max(int(math.floor(z)), 0), 3)
    return (ix * 4 + iz, float(z - iz), float(x - ix))


def strokeRect(dll, model, corners, snap):
    """Stroke a closed rectangle on the scalp; returns (rc, closed)."""
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
    return (rc, bool(closed.value))


def levelInfo(dll, model, level):
    """(faces, points, tubes) the scene index published for `level`."""
    faces = ctypes.c_int(0)
    points = ctypes.c_int(0)
    tubes = ctypes.c_int(0)
    rc = dll.Tonic_GetPublishedLevelInfo(model, level, ctypes.byref(faces),
                                         ctypes.byref(points),
                                         ctypes.byref(tubes))
    if rc != 0:
        return None
    return (faces.value, points.value, tubes.value)


def tubeGrid(dll, model, tubeId):
    """(ringCount, ringVerts) the model's census predicts for one tube."""
    segments = dll.Tonic_GetDisplaySegments(model)
    sections = dll.Tonic_GetTubeSectionCount(model, tubeId)
    if sections < 2:
        return None
    t = ctypes.c_float(0.0)
    uv = (ctypes.c_float * 128)()
    count = ctypes.c_int(0)
    scale = ctypes.c_float(0.0)
    twist = ctypes.c_float(0.0)
    rc = dll.Tonic_GetTubeSection(model, tubeId, 0, ctypes.byref(t), uv, 128,
                                  ctypes.byref(count), ctypes.byref(scale),
                                  ctypes.byref(twist))
    if rc != 0 or count.value < 3:
        return None
    return ((sections - 1) * segments + 1, count.value)


def frameCamera(stage, view, lo, hi):
    """Aim a scene camera at the world box (lo, hi) and activate it.

    The eye sits above and to the side, so the box fills the frame with
    the scalp below it. The pixel probe then samples the UPPER band of the
    frame, where only the tube can be: the scalp region tint deliberately
    carries the same clump colour as the tube rooted in it, so a band that
    included the scalp could not tell the two apart.
    """
    from pxr import Gf, Sdf, UsdGeom
    cx = 0.5 * (lo[0] + hi[0])
    cy = 0.5 * (lo[1] + hi[1])
    cz = 0.5 * (lo[2] + hi[2])
    extent = max(max(hi[i] - lo[i] for i in range(3)), 1.0)
    cam = UsdGeom.Camera.Define(stage, Sdf.Path("/TonicPublishCamera"))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000.0))
    eye = Gf.Vec3d(cx + 1.1 * extent, cy + 1.4 * extent, cz + 1.6 * extent)
    zAxis = (eye - Gf.Vec3d(cx, cy, cz)).GetNormalized()
    xAxis = Gf.Cross(Gf.Vec3d(0, 1, 0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    mat = Gf.Matrix4d(1.0)
    mat.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    mat.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    mat.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    mat.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    # Re-aiming the same camera must reuse its transform op, not stack a
    # second one (and not clear the order, which leaves the prim with no
    # transform at all).
    xf = UsdGeom.Xformable(cam.GetPrim())
    op = None
    for candidate in xf.GetOrderedXformOps():
        if candidate.GetOpType() == UsdGeom.XformOp.TypeTransform:
            op = candidate
            break
    if op is None:
        op = xf.AddTransformOp()
    op.Set(mat)
    view._dataModel.viewSettings.cameraPrim = stage.GetPrimAtPath(
        "/TonicPublishCamera")
    return view.getActiveSceneCamera() is not None


# The vertical band of the frame the probe samples, as a fraction of the
# frame height from the top. The camera puts the tube's upper half here and
# the scalp below it.
BAND0 = 0.08
BAND1 = 0.34


def clumpFraction(view, steps=24, window=0.3):
    """Fraction of the frame centre carrying a clump colour.

    The clump palette entry for region 0 is #2F6BFF, so a tube pixel is
    strongly blue-dominant however the headlight shades it, while the grey
    scalp, the grey background and the region tint seen edge-on are not.
    Sampling a central window keeps the scalp (below the tube) out of it.
    """
    view.update()
    view.repaint()
    view.updateGL()
    img = view.grabFrameBuffer()
    w, h = img.width(), img.height()
    blue = 0
    total = 0
    for iy in range(steps):
        for ix in range(steps):
            px = int(w * (0.5 + window * ((ix + 0.5) / steps - 0.5)))
            py = int(h * (BAND0 + (BAND1 - BAND0) * (iy + 0.5) / steps))
            rgb = img.pixel(min(px, w - 1), min(py, h - 1))
            r = (rgb >> 16) & 0xFF
            g = (rgb >> 8) & 0xFF
            b = rgb & 0xFF
            total += 1
            if b > r + 40 and b > g + 40:
                blue += 1
    print("info:   band %dx%d blue %d/%d" % (w, h, blue, total))
    return float(blue) / float(total) if total else 0.0


def bandLuminance(view, steps=24, window=0.3):
    """Mean luminance of the same band clumpFraction samples.

    Selection is a shading change, not a geometry change, so the way to see
    it from a test is that the same pixels get brighter: tonicTube.glslfx
    mixes a selected tube's albedo toward white and adds a rim.
    """
    view.update()
    view.repaint()
    view.updateGL()
    img = view.grabFrameBuffer()
    w, h = img.width(), img.height()
    total = 0.0
    n = 0
    for iy in range(steps):
        for ix in range(steps):
            px = int(w * (0.5 + window * ((ix + 0.5) / steps - 0.5)))
            py = int(h * (BAND0 + (BAND1 - BAND0) * (iy + 0.5) / steps))
            rgb = img.pixel(min(px, w - 1), min(py, h - 1))
            r = (rgb >> 16) & 0xFF
            g = (rgb >> 8) & 0xFF
            b = rgb & 0xFF
            total += 0.2126 * r + 0.7152 * g + 0.0722 * b
            n += 1
    return total / n if n else 0.0


def pinLighting(view):
    """Pin the lighting this script measures luminance under.

    Every assertion below "the frame got brighter" is an ABSOLUTE step
    in luminance, so it is only meaningful if the light is the same one
    each time. usdview's own default is a camera headlight and no dome,
    and testusdview passes --defaultsettings, but the defaults are not
    the whole story: one run in roughly twenty-five of the full suite
    came out with the same geometry and the same coverage shaded with a
    ~5x weaker lit term (base 105.52 against 111.50, selection lift 1.68
    against 7.99 -- the same 1.8 ratio between the selection and hover
    lifts, so the shader was right and the light was not). Setting them
    here costs nothing and takes the viewer's lighting state out of the
    measurement. reportViewSettings() below prints what was actually in
    force, so a failure that survives this names its own cause.
    """
    settings = view._dataModel.viewSettings
    for name, value in (("ambientLightOnly", True),
                        ("domeLightEnabled", False),
                        ("enableSceneLights", False),
                        ("enableSceneMaterials", True),
                        ("defaultMaterialAmbient", 0.2),
                        ("defaultMaterialSpecular", 0.1)):
        if hasattr(settings, name):
            setattr(settings, name, value)


def reportViewSettings(view):
    """The viewer state the shading depends on, as one line."""
    settings = view._dataModel.viewSettings
    names = ("complexity", "renderMode", "ambientLightOnly",
             "domeLightEnabled", "enableSceneLights", "enableSceneMaterials",
             "defaultMaterialAmbient", "defaultMaterialSpecular",
             "displayPrimId", "cullBackfaces", "showAABBox", "showOBBox",
             "lightingEnabled")
    parts = []
    for name in names:
        value = getattr(settings, name, "<none>")
        parts.append("%s=%s" % (name, getattr(value, "name", value)))
    print("info: view settings: %s" % " ".join(parts))


def run(stage, view):
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
    global TONIC_PICK_TUBE_VERT, TONIC_GIZMO_TRANSLATE, TONIC_GIZMO_NONE
    TONIC_PICK_TUBE_VERT = tonicLib.TONIC_PICK_TUBE_VERT
    TONIC_GIZMO_TRANSLATE = tonicLib.TONIC_GIZMO_TRANSLATE
    TONIC_GIZMO_NONE = tonicLib.TONIC_GIZMO_NONE

    scalp = stage.GetPrimAtPath("/Scalp")
    check(bool(scalp), "the stage carries /Scalp")
    if not scalp:
        return 1
    points = scalp.GetAttribute("points").Get()
    counts = scalp.GetAttribute("faceVertexCounts").Get()
    indices = scalp.GetAttribute("faceVertexIndices").Get()
    flat = [float(c) for p in points for c in (p[0], p[1], p[2])]
    pts = (ctypes.c_float * len(flat))(*flat)
    cnt = (ctypes.c_int * len(counts))(*[int(c) for c in counts])
    idx = (ctypes.c_int * len(indices))(*[int(i) for i in indices])

    # -- one model, edited through the C ABI -------------------------------
    model = ctypes.c_void_p(None)
    check(dll.Tonic_Create(ctypes.byref(model)) == 0, "model creates")
    check(dll.Tonic_GetModelId(model) > 0, "the model registers")
    check(dll.Tonic_BindScalp(model, pts, len(flat), cnt, len(counts),
                              idx, len(indices)) == 0, "scalp binds")
    dll.Tonic_SetSnapRadius(model, ctypes.c_float(0.1))
    rc, closed = strokeRect(
        dll, model, [(0.0, 1.0), (2.0, 1.0), (2.0, 3.0), (0.0, 3.0)], 0.1)
    check(rc == 0 and closed, "the stroke closes into a region")
    check(dll.Tonic_Rasterise(model) == 0, "K3 rasterises the region")
    check(dll.Tonic_BuildTubeFromRegion(model, 0, 5, 8,
                                        ctypes.c_float(3.0)) == 0,
          "a tube builds from the region")
    kids = (ctypes.c_int * 4)()
    kidCount = ctypes.c_int(0)
    check(dll.Tonic_SubdivideTube(model, 0, 4, b"kmeans", 7, kids, 4,
                                  ctypes.byref(kidCount)) == 0 and
          kidCount.value == 4,
          "the tube subdivides into four children")
    childIds = [kids[i] for i in range(kidCount.value)]

    # -- the test tube is on screen until a model takes over ---------------
    # It is the record harness's convenience, and proving it renders here
    # is what makes "gone after activate" mean something.
    check(frameCamera(stage, view, (-0.5, 0.0, -0.5), (0.5, 4.0, 0.5)),
          "the scene camera frames the test tube")
    beforeFraction = clumpFraction(view)
    print("info: clump pixels before activate: %.2f" % beforeFraction)
    check(beforeFraction > 0.5,
          "Hydra draws the test tube while no model is active")

    # -- publish it --------------------------------------------------------
    check(dll.Tonic_Activate(model) == 0, "the model activates")
    published = dll.Tonic_Publish(model, 0)
    check(published >= 1,
          "Tonic_Publish reached %d scene index(es)" % published)
    if published < 1:
        print("FAIL: no tonic scene index in this usdview "
              "(is USDGENTONIC_ENABLE off, or the plugin unregistered?)")
        dll.Tonic_Destroy(model)
        return 1

    # -- what the index published, against the model's own census ----------
    grid1 = tubeGrid(dll, model, 0)
    check(grid1 is not None, "the L1 tube reports its section grid")
    info1 = levelInfo(dll, model, 1)
    check(info1 is not None, "/__usdGenTonic/tubes/L1 is published")
    if grid1 and info1:
        rings, verts = grid1
        check(info1 == ((rings - 1) * verts, rings * verts, 1),
              "tubes/L1 is one tube of %d faces / %d points (got %r)"
              % ((rings - 1) * verts, rings * verts, info1))

    expectFaces2 = 0
    expectPoints2 = 0
    for childId in childIds:
        grid = tubeGrid(dll, model, childId)
        check(grid is not None, "child %d reports its section grid" % childId)
        if grid:
            rings, verts = grid
            expectFaces2 += (rings - 1) * verts
            expectPoints2 += rings * verts
    info2 = levelInfo(dll, model, 2)
    check(info2 is not None, "/__usdGenTonic/tubes/L2 is published")
    if info2:
        check(info2 == (expectFaces2, expectPoints2, 4),
              "tubes/L2 is four tubes of %d faces / %d points (got %r)"
              % (expectFaces2, expectPoints2, info2))
    check(levelInfo(dll, model, 3) is None,
          "no level 3 is published (the groom has two)")

    # -- Hydra has it: pick the viewport -----------------------------------
    xyz = (ctypes.c_float * (3 * dll.Tonic_GetVertexCount(model)))()
    check(dll.Tonic_ReadTubePoints(model, xyz, len(xyz)) == 0,
          "the L1 tube's staged points read back")
    verts = [(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2])
             for i in range(len(xyz) // 3)]
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    # Frame the tube's upper half side-on: the test tube stood at the
    # origin, so the same frame also shows whether it is still there.
    check(frameCamera(stage, view, lo, hi), "the scene camera reframes")
    print("info: L1 world box %s .. %s" % (lo, hi))

    both = clumpFraction(view)
    print("info: clump pixels, both levels visible: %.2f" % both)
    check(both > 0.5, "Hydra rasterises the published levels")

    # Hiding both levels must clear the frame; showing either one alone
    # must fill it again. That is the level-visibility contract proven
    # through the renderer, and it is also what says L2 is a real prim
    # rather than pixels belonging to its parent.
    check(dll.Tonic_SetLevelDisplay(model, 1, 0, 0) == 0, "hide L1")
    check(dll.Tonic_SetLevelDisplay(model, 2, 0, 0) == 0, "hide L2")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish both hidden")
    none = clumpFraction(view)
    print("info: clump pixels, both levels hidden: %.2f" % none)
    check(none < 0.05,
          "hiding every level empties the frame (the test tube did not "
          "come back)")

    check(dll.Tonic_SetLevelDisplay(model, 2, 1, 0) == 0, "show L2 only")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish L2 only")
    onlyL2 = clumpFraction(view)
    print("info: clump pixels, L2 only: %.2f" % onlyL2)
    check(onlyL2 > 0.4, "Hydra draws tubes/L2 on its own")

    check(dll.Tonic_SetLevelDisplay(model, 2, 0, 0) == 0, "hide L2 again")
    check(dll.Tonic_SetLevelDisplay(model, 1, 1, 0) == 0, "show L1 only")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish L1 only")
    onlyL1 = clumpFraction(view)
    print("info: clump pixels, L1 only: %.2f" % onlyL1)
    check(onlyL1 > 0.5, "Hydra draws tubes/L1 on its own")

    check(dll.Tonic_SetLevelDisplay(model, 2, 1, 0) == 0, "show both again")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish the restore")
    check(clumpFraction(view) > 0.5, "both levels come back")

    # -- the selection reaches the shader (plan/18 V1) ---------------------
    #
    # The published `selected` primvar cannot be read from Python: this USD
    # build exposes no terminal scene index binding (UsdImagingGL.Engine has
    # no GetTerminalSceneIndex here), so there is no way to ask Hydra for a
    # primvar value from a test script. What CAN be proven, and is stronger,
    # is that the value arrived where it is used: tonicTube.glslfx mixes a
    # selected tube's albedo 15 per cent toward white and adds a Fresnel
    # rim, so the frame gets measurably brighter the moment the model's
    # selection changes and no geometry moves at all. The model-side round
    # trip below pins the other half: what was selected is what was asked
    # for.
    # What the frame is being shaded WITH. The selection lift is an
    # absolute luminance step, so anything that changes the shading
    # changes the step: this line is what names it when the step comes
    # out short.
    pinLighting(view)
    reportViewSettings(view)
    base = bandLuminance(view)
    print("info: band luminance, nothing selected: %.2f" % base)
    tubeIds = (ctypes.c_int * 1)(0)
    check(dll.Tonic_SelectSet(model, TONIC_PICK_TUBE_VERT, tubeIds, None,
                              None, 1) == 0,
          "the L1 tube is selected through the C ABI")
    outIds = (ctypes.c_int * 8)()
    outCount = ctypes.c_int(0)
    check(dll.Tonic_ReadSelection(model, TONIC_PICK_TUBE_VERT, outIds, None,
                                  None, 8, ctypes.byref(outCount)) == 0 and
          outCount.value == 1 and outIds[0] == 0,
          "the model reports exactly tube 0 selected")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish the selection")
    selected = bandLuminance(view)
    print("info: band luminance, tube selected: %.2f" % selected)
    check(selected > base + 2.0,
          "the selected tube renders brighter (the `selected` primvar "
          "reached the shader): %.2f -> %.2f" % (base, selected))

    check(dll.Tonic_SetHover(model, TONIC_PICK_TUBE_VERT, 0, -1, -1) == 0,
          "the tube is hovered")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish the hover")
    hovered = bandLuminance(view)
    print("info: band luminance, tube hovered: %.2f" % hovered)
    check(hovered > base + 0.5,
          "a hovered tube is lifted too: %.2f -> %.2f" % (base, hovered))
    check(hovered < selected,
          "hover lifts less than selection (8 per cent against 15 plus "
          "the rim): %.2f < %.2f" % (hovered, selected))
    check(dll.Tonic_SetHover(model, 0, -1, -1, -1) == 0, "hover clears")

    check(dll.Tonic_SelectClear(model, 0) == 0, "the selection clears")
    check(dll.Tonic_GetSelectionCount(model, 0) == 0,
          "the model reports nothing selected")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish the cleared selection")
    cleared = bandLuminance(view)
    print("info: band luminance, selection cleared: %.2f" % cleared)
    check(abs(cleared - base) < 1.0,
          "clearing the selection puts the frame back: %.2f vs %.2f"
          % (cleared, base))

    # The gizmo is geometry like everything else: publishing one adds a
    # prim the renderer draws, and clearing it takes the prim away.
    origin = (ctypes.c_float * 3)(*[0.5 * (lo[i] + hi[i]) for i in range(3)])
    frame = (ctypes.c_float * 9)(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)
    size = max(max(hi[i] - lo[i] for i in range(3)), 1.0)
    check(dll.Tonic_SetGizmo(model, TONIC_GIZMO_TRANSLATE, origin, frame,
                             ctypes.c_float(size), -1) == 0,
          "a translate gizmo is set on the selection")
    check(dll.Tonic_Publish(model, 0) >= 1, "publish the gizmo")
    kind = ctypes.c_int(-1)
    check(dll.Tonic_GetGizmo(model, ctypes.byref(kind), None, None, None,
                             None) == 0 and kind.value == TONIC_GIZMO_TRANSLATE,
          "the gizmo record round-trips through the ABI")
    check(dll.Tonic_SetGizmo(model, TONIC_GIZMO_NONE, origin, frame,
                             ctypes.c_float(size), -1) == 0 and
          dll.Tonic_Publish(model, 0) >= 1,
          "the gizmo clears and republishes")

    check(dll.Tonic_Deactivate(model) == 0, "the model deactivates")
    dll.Tonic_Destroy(model)
    print("testUsdviewTonicPublish: %d failure(s)" % failures)
    return 1 if failures else 0


def testUsdviewInputFunction(appController):
    model = getattr(appController, "_dataModel", appController)
    view = getattr(appController, "_stageView", None)
    if view is None:
        print("FAIL: no stage view (the publication proof needs a live "
              "usdview)")
        return 1
    return run(model.stage, view)


if __name__ == "__main__":
    # No live view on a direct run; this proof is T3-only by nature.
    print("SKIP: testUsdviewTonicPublish needs testusdview (no live view)")
    sys.exit(0)
