#!/usr/bin/env python3
# record_tonic.py -- a headless frame of a Tonic groom with the TOOL LIVE.
#
# usdrecord renders what the stage holds. The Tonic scene index publishes
# nothing until a model is created, activated and published, so a plain
# usdrecord of a committed groom shows the amplified hair and never the
# tubes, level colours, center curves or CV dots the artist actually edits
# (plan/18 F1/F2). This script is usdrecord's offscreen path plus the four
# calls that make the tool live: hydrate the committed groom into a model,
# focus a level, activate, publish.
#
#   python bin/record_tonic.py SCENE OUT.png [--camera /World/Cam]
#          [--width 512] [--complexity veryhigh] [--level 2]
#          [--groom /TonicGroom] [--renderer GL]
#
# With no --camera the frame gets a generated one that fits the stage and
# the live model together: the scene cameras in examples/tonic-*.usda
# were framed for the committed hair, and the tubes the tool publishes
# are bigger than that.
#
# It needs the environment bin/record_usd.ps1 builds (PXR_PLUGINPATH_NAME
# with the usdGenTonic scene-index plugin, PYTHONPATH with build/python,
# PATH with the DLLs); `record_usd.ps1 -Tonic` builds it and calls through
# to here. USDGENTONIC_DLL overrides where tonicLib looks for the ABI.
#
# `record()` is importable, and the golden test
# plugin/usdGenTonicTools/testenv/testTonicGolden.py calls it, so the
# golden and the comparison render through exactly one code path.
import argparse
import os
import sys

# The tool's own draw is the point of this frame, so the scene index's
# static P0 test tube (a convenience for empty scenes) must stay out of
# it. Set before the first Hydra prim is asked for -- TfEnvSetting reads
# the process environment once.
os.environ.setdefault("USDGENTONIC_TEST_TUBE", "0")


def _repoRoot():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _ensureToolsOnPath():
    """Prefer the staged package the build installs, then the sources."""
    for candidate in (os.path.join(_repoRoot(), "build", "python"),
                      os.path.join(_repoRoot(), "plugin", "usdGenTonicTools",
                                   "python")):
        if os.path.isdir(os.path.join(candidate, "usdGenTonicTools")) and \
                candidate not in sys.path:
            sys.path.append(candidate)


def setupOpenGLContext(width, height):
    """An offscreen GL context for FrameRecorder, as usdrecord makes one.

    Returned so the caller can keep it alive: dropping the last reference
    destroys the context under the renderer. A QApplication that already
    exists (the golden test makes none, but a host might) is reused.
    """
    from PySide6.QtOpenGL import QOpenGLFramebufferObject
    from PySide6.QtOpenGL import QOpenGLFramebufferObjectFormat
    from PySide6.QtCore import QSize
    from PySide6.QtGui import QOffscreenSurface, QOpenGLContext, QSurfaceFormat
    from PySide6.QtWidgets import QApplication

    application = QApplication.instance() or QApplication(sys.argv[:1])

    glFormat = QSurfaceFormat()
    glFormat.setSamples(4)
    surface = QOffscreenSurface()
    surface.setFormat(glFormat)
    surface.create()
    context = QOpenGLContext()
    context.setFormat(glFormat)
    context.create()
    context.makeCurrent(surface)
    # FrameRecorder's present task wants a bound framebuffer; it renders
    # through AOVs, so a 1x1 default-format FBO is enough.
    fbo = QOpenGLFramebufferObject(QSize(1, 1), QOpenGLFramebufferObjectFormat())
    fbo.bind()
    return (application, surface, context, fbo)


class LiveGroom:
    """A hydrated, activated, published Tonic model for one stage.

    Holds the session for the lifetime of the render: deactivating it
    unpublishes the tubes, so the frame must be recorded first.
    """

    def __init__(self, stage, groomPath, level, scalpPath="", curves=""):
        _ensureToolsOnPath()
        from usdGenTonicTools import tonicSession, tonicToolState
        self.state = tonicToolState.TonicToolState()
        self.messages = []
        self.session = tonicSession.TonicSession(
            self.state, usdviewApi=None, statusFn=self.messages.append)
        if scalpPath:
            # The import path, for a groom TonicHydrateModel refuses: the
            # example ponytail and the reference scene carry hand-posed
            # guides rather than K9 kernel output and no scalp graph
            # shell, so they have nothing to hydrate. The artist opens
            # those the way the menu does -- Bind scalp, then Import
            # curves -- and so does this. Asked for explicitly, never a
            # silent fallback: a hydrate that starts failing has to be
            # seen, not papered over.
            if not self.session.activate(scalpPath, groomPath=groomPath,
                                         stage=stage):
                raise RuntimeError("bind %s failed: %s"
                                   % (scalpPath, "; ".join(self.messages)))
            if curves and not self._importCurves(stage, curves):
                raise RuntimeError("importing %s failed: %s"
                                   % (curves, "; ".join(self.messages)))
        elif not self.session.hydrate(groomPath=groomPath, stage=stage):
            raise RuntimeError("hydrate %s failed: %s"
                               % (groomPath, "; ".join(self.messages)))
        dll = self.session.dll
        model = self.session.model
        if dll.Tonic_SetFocusLevel(model, int(level)) != 0:
            raise RuntimeError("Tonic_SetFocusLevel(%d): %s"
                               % (level, self.session.lastError()))
        # Usually 0 here, and that is correct: FrameRecorder builds its
        # render index (and with it the Tonic scene index) inside
        # Record(), and a scene index publishes whatever is already
        # ACTIVE the moment it is constructed. What proves the frame
        # carries the tool's geometry is publishedLevel() after the
        # render, not this count before it.
        self.published = int(self.session.publishAll())

    def _importCurves(self, stage, curvesPath):
        """Bring the stage's BasisCurves in as locked tubes."""
        from usdGenTonicTools import tonicBridge
        curves = tonicBridge.readBasisCurves(stage, curvesPath)
        if not curves:
            self.messages.append("no BasisCurves at %s" % curvesPath)
            return False
        try:
            ids = tonicBridge.importCurvesAsLockedTubes(
                self.session.dll, self.session.model, 0, curves)
        except RuntimeError as exc:
            self.messages.append(str(exc))
            return False
        self.messages.append("imported %d curve(s) as tubes" % len(ids))
        return bool(ids)

    def counts(self):
        """(tubes, guides) the live model holds."""
        import ctypes
        dll = self.session.dll
        guides = ctypes.c_int(0)
        dll.Tonic_GetGuideCounts(self.session.model, ctypes.byref(guides),
                                 None)
        return (int(dll.Tonic_GetTubeCount(self.session.model)),
                int(guides.value))

    def bounds(self):
        """World (min, max) of every tube the model holds, or None.

        Read through the selection, which is what Tonic_GetSelectionBounds
        answers for: select every tube, take the box, put the selection
        back. Done before the model is published, so no frame ever shows
        the selection highlight this borrows.
        """
        import ctypes
        from usdGenTonicTools import tonicBridge, tonicLib
        dll = self.session.dll
        model = self.session.model
        ids = tonicBridge.readTubeIds(dll, model)
        if not ids:
            return None
        array = (ctypes.c_int * len(ids))(*[int(i) for i in ids])
        if dll.Tonic_SelectSet(model, tonicLib.TONIC_PICK_TUBE_VERT, array,
                               None, None, len(ids)) != 0:
            return None
        lo = (ctypes.c_float * 3)()
        hi = (ctypes.c_float * 3)()
        status = dll.Tonic_GetSelectionBounds(model, lo, hi)
        dll.Tonic_SelectClear(model, 0)
        if status != 0:
            return None
        return ((float(lo[0]), float(lo[1]), float(lo[2])),
                (float(hi[0]), float(hi[1]), float(hi[2])))

    def publishedLevel(self, level):
        """(faces, points, tubes) an attached index staged, or None."""
        import ctypes
        faces = ctypes.c_int(0)
        points = ctypes.c_int(0)
        tubes = ctypes.c_int(0)
        if self.session.dll.Tonic_GetPublishedLevelInfo(
                self.session.model, int(level), ctypes.byref(faces),
                ctypes.byref(points), ctypes.byref(tubes)) != 0:
            return None
        return (int(faces.value), int(points.value), int(tubes.value))

    def close(self):
        self.session.deactivate()


def frameCamera(stage, boxes, path="/TonicRecordCamera"):
    """Author a camera that frames `boxes`, and hand back its prim.

    The scene cameras in examples/tonic-*.usda were framed for the
    committed hair alone; the tubes the tool publishes are Hydra-only
    prims that no UsdGeomBBoxCache can see, and on the braid groom they
    are large enough to fill the frame edge to edge from that camera. So
    the recorded frame gets its own camera, aimed at the union of what
    the stage holds and what the model holds, from a fixed three-quarter
    direction -- a pure function of the two boxes, and therefore as
    reproducible as the geometry.
    """
    from pxr import Gf, Sdf, UsdGeom
    lo = [min(box[0][i] for box in boxes) for i in range(3)]
    hi = [max(box[1][i] for box in boxes) for i in range(3)]
    centre = Gf.Vec3d(*[0.5 * (lo[i] + hi[i]) for i in range(3)])
    extent = max(max(hi[i] - lo[i] for i in range(3)), 1e-3)
    direction = Gf.Vec3d(0.55, 0.42, 0.72).GetNormalized()
    eye = centre + direction * (2.2 * extent)
    zAxis = direction
    xAxis = Gf.Cross(Gf.Vec3d(0, 1, 0), zAxis).GetNormalized()
    yAxis = Gf.Cross(zAxis, xAxis)
    matrix = Gf.Matrix4d(1.0)
    matrix.SetRow(0, Gf.Vec4d(xAxis[0], xAxis[1], xAxis[2], 0.0))
    matrix.SetRow(1, Gf.Vec4d(yAxis[0], yAxis[1], yAxis[2], 0.0))
    matrix.SetRow(2, Gf.Vec4d(zAxis[0], zAxis[1], zAxis[2], 0.0))
    matrix.SetRow(3, Gf.Vec4d(eye[0], eye[1], eye[2], 1.0))
    cam = UsdGeom.Camera.Define(stage, Sdf.Path(path))
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateHorizontalApertureAttr(24.0)
    cam.CreateVerticalApertureAttr(24.0)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.05, float(200.0 * extent)))
    UsdGeom.Xformable(cam.GetPrim()).AddTransformOp().Set(matrix)
    return cam


def applyDisplayScale(live, usdCamera, width, boxes, verbose=True):
    """Size the tool's overlay dots for THIS frame's pixels.

    Storm takes `widths` in world units, so a CV dot is only a fixed
    number of pixels across if the publisher multiplies its pixel target
    by the world-units-per-pixel of the camera that is about to render
    (plan/18 section 2.4a). In usdview the viewport controller measures
    that on every camera change; here the camera is authored a few lines
    above, so the measurement happens once, right before the frame.

    Returns the scale that was applied, or 0.0 when there is no live
    model (a --no-tool frame has no overlays to size).
    """
    from pxr import Usd
    _ensureToolsOnPath()
    from usdGenTonicTools import tonicCamera
    if live is None:
        return 0.0
    frustum = usdCamera.GetCamera(Usd.TimeCode.Default()).frustum
    size = frustum.window.GetSize()
    aspect = (size[0] / size[1]) if size[1] else 1.0
    height = max(int(round(float(width) / aspect)), 1)
    resolved = tonicCamera.TonicCamera(
        tonicCamera.viewProjFromFrustum(frustum), int(width), height)
    lo = [min(box[0][i] for box in boxes) for i in range(3)]
    hi = [max(box[1][i] for box in boxes) for i in range(3)]
    centre = tuple(0.5 * (lo[i] + hi[i]) for i in range(3))
    perPixel = resolved.worldPerPixel(centre)
    if not perPixel > 0.0:
        return 0.0
    live.session.setDisplayScale(perPixel)
    if verbose:
        print("record_tonic: %dx%d, %.6f world units per pixel at %r"
              % (int(width), height, perPixel, centre))
    return perPixel


def stageBounds(stage):
    """The stage's world box from USD alone, or None."""
    from pxr import Usd, UsdGeom
    cache = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                              [UsdGeom.Tokens.default_,
                               UsdGeom.Tokens.render,
                               UsdGeom.Tokens.proxy])
    box = cache.ComputeWorldBound(stage.GetPseudoRoot()).ComputeAlignedRange()
    if box.IsEmpty():
        return None
    lo, hi = box.GetMin(), box.GetMax()
    return ((lo[0], lo[1], lo[2]), (hi[0], hi[1], hi[2]))


def record(scene, output, camera="", width=512,
           complexity="veryhigh", level=2, groom="/TonicGroom",
           renderer="GL", scalp="", curves="", tool=True, verbose=True):
    """Render one frame of `scene` with the tool live.

    Returns (tubes, guides, stagedTubesAtLevel1). Raises RuntimeError
    when the groom cannot be hydrated or when no scene index staged the
    tubes -- a silently empty frame would make a golden that proves the
    opposite of what it is for.
    """
    from pxr import Usd, UsdAppUtils, UsdGeom

    stage = Usd.Stage.Open(scene)
    if not stage:
        raise RuntimeError("could not open %s" % scene)

    held = setupOpenGLContext(width, width)
    # tool=False records the stage alone, for a groom the tool cannot
    # open at all: examples/tonic-ponytail.usda carries hand-posed
    # guides and no UsdGenScalpGraph shell, so TonicHydrateModel refuses
    # it by design and the import path has no root tube to hang the
    # curves under. That frame still gates the P3 exit (the amplified
    # ponytail renders), and the flag says out loud that no tool
    # geometry is in it.
    live = (LiveGroom(stage, groom, level, scalpPath=scalp, curves=curves)
            if tool else None)
    try:
        tubes, guides = live.counts() if live else (0, 0)
        if verbose:
            print("record_tonic: %s -> %s" % (scene, output))
            print("record_tonic: %d tube(s), %d guide(s), L%d focused%s"
                  % (tubes, guides, level,
                     "" if live else " (TOOL OFF: stage only)"))

        boxes = [box for box in (stageBounds(stage),
                                 live.bounds() if live else None)
                 if box is not None]
        if camera:
            usdCamera = UsdAppUtils.GetCameraAtPath(stage, camera)
            if not usdCamera:
                raise RuntimeError("no camera at %s" % camera)
        else:
            if not boxes:
                raise RuntimeError("nothing to frame in %s" % scene)
            usdCamera = frameCamera(stage, boxes)
            if verbose:
                print("record_tonic: framing %r" % (boxes,))
        if boxes:
            applyDisplayScale(live, usdCamera, width, boxes, verbose)
        complexities = UsdAppUtils.complexityArgs.RefinementComplexities
        chosen = complexities.fromId(complexity)
        recorder = UsdAppUtils.FrameRecorder(
            UsdAppUtils.rendererArgs.GetPluginIdFromArgument(renderer) or "",
            True, True)
        recorder.SetImageWidth(int(width))
        recorder.SetComplexity(chosen.value)
        recorder.SetPrimaryCameraPrimPath(usdCamera.GetPath())
        directory = os.path.dirname(os.path.abspath(output))
        if directory:
            os.makedirs(directory, exist_ok=True)
        recorder.Record(stage, usdCamera, Usd.TimeCode.Default(), output)
        # The recorder still holds its render index, so the Tonic scene
        # index it built is still attached and can say what it staged.
        staged = live.publishedLevel(1) if live else None
        if live and (staged is None or staged[2] < 1):
            raise RuntimeError(
                "no tonic scene index staged the tubes (is the usdGenTonic "
                "plugin on PXR_PLUGINPATH_NAME, or USDGENTONIC_ENABLE off?)")
        if verbose and staged:
            print("record_tonic: L1 staged %d face(s) / %d point(s) / "
                  "%d tube(s)" % staged)
        recorder = None
    finally:
        if live:
            live.close()
    del held
    return (tubes, guides, staged[2] if staged else 0)


def main():
    parser = argparse.ArgumentParser(
        description="Record a Tonic groom with the tool live")
    parser.add_argument("scene")
    parser.add_argument("output")
    parser.add_argument("--camera", default="",
                        help="a camera on the stage; empty frames the "
                             "scene and the model together")
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--complexity", default="veryhigh",
                        choices=("low", "medium", "high", "veryhigh"))
    parser.add_argument("--level", type=int, default=2,
                        help="the focused level (thick center curves, "
                             "large CV dots)")
    parser.add_argument("--groom", default="/TonicGroom")
    parser.add_argument("--renderer", default="GL")
    parser.add_argument("--scalp", default="",
                        help="bind this mesh and IMPORT instead of "
                             "hydrating (for a groom TonicHydrateModel "
                             "refuses)")
    parser.add_argument("--curves", default="",
                        help="with --scalp: the BasisCurves to import as "
                             "locked tubes")
    parser.add_argument("--no-tool", dest="tool", action="store_false",
                        help="record the stage alone (for a groom the "
                             "tool cannot open)")
    opts = parser.parse_args()
    try:
        record(opts.scene, opts.output, camera=opts.camera, width=opts.width,
               complexity=opts.complexity, level=opts.level, groom=opts.groom,
               renderer=opts.renderer, scalp=opts.scalp, curves=opts.curves,
               tool=opts.tool)
    except RuntimeError as exc:
        sys.stderr.write("record_tonic: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
