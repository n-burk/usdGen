# testusdview script for usdGenTools viewport supersampling.
#
# Drives usdview headlessly and checks that rendering the Storm viewport at
# N x the window and resolving it ourselves keeps the frame in one piece:
#
#   testusdview --testScript <this file> examples/expression-width-plane.usda
#
# with PXR_PLUGINPATH_NAME naming the build-tree resources (schema, imaging,
# shaders, tools) and PYTHONPATH naming build/python and the OpenUSD python
# package -- the environment the testUsdGenToolsExprEditor T2 test uses.
#
# The failure this guards against is the one the stock code path forces: an
# N x render buffer presents at N x into the window (dstRegion follows the
# render buffer size, hdx/taskControllerSceneIndex.cpp:2636), so the frame
# would be magnified and cropped to its lower-left corner. A quadrant-mean
# comparison against the 1x frame catches exactly that.
import sys

WINDOW = (400, 300)
# Sparse grid; QImage.pixel() per sample is plenty fast at this density.
STRIDE = 5


def check(cond, msg):
    if not cond:
        print("FAIL: " + msg)
        sys.exit(1)
    print("ok: " + msg)


def _QuadrantMeans(image):
    """Mean luminance of each quadrant, sampled on a grid."""
    width, height = image.width(), image.height()
    sums = [0.0, 0.0, 0.0, 0.0]
    counts = [0, 0, 0, 0]
    for y in range(0, height, STRIDE):
        for x in range(0, width, STRIDE):
            rgb = image.pixel(x, y)
            lum = ((rgb >> 16) & 0xFF) + ((rgb >> 8) & 0xFF) + (rgb & 0xFF)
            q = (0 if x < width // 2 else 1) + (0 if y < height // 2 else 2)
            sums[q] += lum / 765.0
            counts[q] += 1
    return [s / max(1, c) for s, c in zip(sums, counts)]


def _Luminance(image, x0, y0, x1, y1):
    """Mean luminance over a sampled rectangle."""
    total = 0.0
    count = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            rgb = image.pixel(x, y)
            total += (((rgb >> 16) & 0xFF) + ((rgb >> 8) & 0xFF) +
                      (rgb & 0xFF)) / 765.0
            count += 1
    return total / max(1, count)


def _MeanAbsDiff(a, b, box=None):
    """Mean |difference| over a sampled grid, optionally over a sub-rectangle."""
    x0, y0, x1, y1 = box or (0, 0, a.width(), a.height())
    total = 0.0
    count = 0
    for y in range(y0, y1, STRIDE):
        for x in range(x0, x1, STRIDE):
            pa, pb = a.pixel(x, y), b.pixel(x, y)
            for shift in (16, 8, 0):
                total += abs(((pa >> shift) & 0xFF) - ((pb >> shift) & 0xFF))
            count += 3
    return total / max(1, count) / 255.0


def testUsdviewInputFunction(appController):
    from pxr.Usdviewq.qt import QtWidgets
    import usdGenTools.supersample as supersample

    app = QtWidgets.QApplication.instance()
    stageView = appController._stageView
    viewSettings = appController._dataModel.viewSettings

    check(supersample.IsInstalled(),
          "the plugin container installed the StageView patch")
    check(supersample.GetFactor() == 1,
          "supersampling is off until asked for")

    stageView.SetPhysicalWindowSize(*WINDOW)
    viewSettings.showHUD = False
    app.processEvents()

    def frame():
        stageView.update()
        app.processEvents()
        return stageView.grabFrameBuffer()

    base = frame()
    check((base.width(), base.height()) == WINDOW,
          "1x frame is the window size")

    supersample.SetFactor(2)
    check(supersample.GetFactor() == 2, "factor 2 is taken")
    superFrame = frame()
    check((superFrame.width(), superFrame.height()) == WINDOW,
          "2x frame resolves back to the window size")

    baseQuads = _QuadrantMeans(base)
    superQuads = _QuadrantMeans(superFrame)
    worst = max(abs(a - b) for a, b in zip(baseQuads, superQuads))
    print("quadrant means 1x %s  2x %s" %
          (["%.4f" % q for q in baseQuads], ["%.4f" % q for q in superQuads]))
    # Resolving sub-pixel strands really does change the mean a little (that
    # is the point), so this is a shape check, not an equality check: a
    # magnified, cropped frame moves a quadrant by a large fraction of its
    # value, not by a few percent.
    check(worst < 0.08,
          "every quadrant of the 2x frame matches the 1x frame (worst %.4f) "
          "-- the image is resolved, not magnified and cropped" % worst)

    diff = _MeanAbsDiff(base, superFrame)
    print("mean abs difference 1x vs 2x: %.5f" % diff)
    check(diff > 0.0,
          "the 2x frame is not byte-identical to the 1x frame (it is filtered)")

    # Picking is done in window coordinates through a narrowed frustum, so
    # neither the frustum nor the hit may move with the factor. (The pick
    # itself runs at HdxPickTask's own resolution, not the render buffer's.)
    points = [(WINDOW[0] // 4, WINDOW[1] // 4),
              (WINDOW[0] // 2, WINDOW[1] // 2),
              (WINDOW[0] * 3 // 4, WINDOW[1] * 3 // 4)]

    def probe():
        out = []
        for x, y in points:
            inBounds, pickFrustum = stageView.computePickFrustum(x, y)
            hits = stageView.pick(pickFrustum)
            out.append((inBounds,
                        repr(pickFrustum.ComputeViewMatrix()),
                        repr(pickFrustum.ComputeProjectionMatrix()),
                        str(hits[0].hitPrimPath) if hits else None))
        return out

    superProbe = probe()
    supersample.SetFactor(1)
    frame()
    baseProbe = probe()
    check(all(p[0] for p in superProbe),
          "the sampled points are inside the image at 2x")
    check(superProbe == baseProbe,
          "pick frustums and hits are identical at 1x and 2x")

    # Back at 1x the stock path is what runs again.
    restored = frame()
    check(_MeanAbsDiff(base, restored) < 0.002,
          "switching back to 1x restores the stock frame")

    # The HUD is drawn after the resolve, at window resolution, so turning it
    # on must change the top-left corner and nothing in the lower-right.
    supersample.SetFactor(2)
    noHud = frame()
    repeat = frame()
    noise = _MeanAbsDiff(noHud, repeat)
    check(noise < 0.002, "consecutive 2x frames agree (%.5f)" % noise)
    viewSettings.showHUD = True
    # Leave the subtree-info group off: it is a tall left-hand column that
    # reaches the middle of a small window, and the point here is the band
    # between the HUD's corners.
    viewSettings.showHUD_Info = False
    withHud = frame()
    middle = (WINDOW[0] // 2, WINDOW[1] * 2 // 5,
              WINDOW[0] * 17 // 20, WINDOW[1] * 3 // 5)
    # The renderer/Hgi group is top-right, the frame times bottom-left.
    topBand = _MeanAbsDiff(noHud, withHud, (0, 0, WINDOW[0], 60))
    middleDiff = _MeanAbsDiff(noHud, withHud, middle)
    print("HUD on/off: top band %.5f  middle %.5f" % (topBand, middleDiff))
    check(topBand > 0.0, "the HUD draws over the resolved 2x frame")
    check(middleDiff < 0.002, "the HUD does not disturb the image")
    viewSettings.showHUD = False

    viewSettings.showHUD = False

    # Camera masking with a locked aspect ratio is StageView's
    # _cropImageToCameraViewport path: the render viewport is a sub-rectangle
    # of the (now N x) render buffer, and the mask is drawn around it.
    from pxr.Usdviewq.common import CameraMaskModes
    viewSettings.freeCameraAspect = 2.5  # wider than the window: real bands
    viewSettings.lockFreeCameraAspect = True
    viewSettings.cameraMaskMode = CameraMaskModes.FULL
    masked = frame()
    check((masked.width(), masked.height()) == WINDOW,
          "the camera mask crops at 2x without changing the frame size")
    darkest = min(_Luminance(masked, 0, 0, WINDOW[0], 6),
                  _Luminance(masked, 0, WINDOW[1] - 6, WINDOW[0], WINDOW[1]),
                  _Luminance(masked, 0, 0, 6, WINDOW[1]),
                  _Luminance(masked, WINDOW[0] - 6, 0, WINDOW[0], WINDOW[1]))
    print("masked frame: darkest border band %.4f" % darkest)
    check(darkest < 0.1,
          "one masked border band is the mask colour, so the crop applies")
    viewSettings.cameraMaskMode = CameraMaskModes.NONE
    viewSettings.lockFreeCameraAspect = False

    # A resize while supersampling reallocates the N x target.
    resized = (WINDOW[0] - 73, WINDOW[1] - 41)
    stageView.SetPhysicalWindowSize(*resized)
    app.processEvents()
    smaller = frame()
    got = (smaller.width(), smaller.height())
    print("resized to %s, grabbed %s" % (resized, got))
    check(abs(got[0] - resized[0]) <= 2 and abs(got[1] - resized[1]) <= 2,
          "the N x target follows a window resize")
    stageView.SetPhysicalWindowSize(*WINDOW)
    app.processEvents()

    supersample.SetFactor(1)
    frame()

    _TestMenuToggles(appController, app, stageView, frame, base)

    print("ok: usdGenTools viewport supersampling")


def _MenuActions(appController):
    """The Viewport Supersampling submenu's actions, by display name."""
    from pxr.Usdviewq.qt import QtWidgets
    for child in appController._mainWindow.menuBar().children():
        if not isinstance(child, QtWidgets.QMenu):
            continue
        if str(child.title()).replace("&", "") != "usdGen":
            continue
        for action in child.actions():
            submenu = action.menu()
            if submenu is not None and \
                    str(action.text()) == "Viewport Supersampling":
                return {str(a.text()): a for a in submenu.actions()}
    return {}


def _TestMenuToggles(appController, app, stageView, frame, base):
    """The user's path: flip the menu at runtime, Off -> 2x -> 4x -> Off -> 2x.

    Every factor change reallocates the N x target, and that is the frame that
    used to raise a GLError, so each step renders twice and the second frame
    has to be as good as the first."""
    import sys

    import usdGenTools.supersample as supersample
    import pxr.Usdviewq.stageView as stageViewModule

    registry = appController._plugRegistry
    actions = _MenuActions(appController)
    check(sorted(actions) == ["2x", "4x", "Off"],
          "the usdGen menu carries a Viewport Supersampling submenu (%s)"
          % sorted(actions))
    check(all(a.isCheckable() for a in actions.values()),
          "its items are checkable")
    check(actions["Off"].isChecked(),
          "Off is the item checked while the factor is 1")

    # The render buffer size the engine is given. _ComputeCameraFraming is
    # handed exactly the vector that goes to SetRenderBufferSize, and it is
    # plain Python, unlike the engine method itself.
    buffers = []
    origFraming = stageViewModule._ComputeCameraFraming

    def framingSpy(viewport, renderBufferSize):
        buffers.append(tuple(renderBufferSize))
        return origFraming(viewport, renderBufferSize)

    stageViewModule._ComputeCameraFraming = framingSpy

    # Anything usdview catches during a paint is written to stderr; a toggle
    # that raises must not be swallowed by this test.
    renderErrors = []
    realStderr = sys.stderr

    class _Watch(object):
        def write(self, text):
            if "error while rendering" in text or "GLError" in text:
                renderErrors.append(text)
            realStderr.write(text)

        def flush(self):
            realStderr.flush()

    def pickAt(x, y):
        inBounds, pickFrustum = stageView.computePickFrustum(x, y)
        hits = stageView.pick(pickFrustum)
        path = str(hits[0].hitPrimPath) if hits else ""
        return inBounds, path

    # Prefer a point that actually hits a prim. usdGen's synthetic tiles are
    # not pickable in usdview at all (docs/storm-fur.md, "Picking and the ID
    # pass"), so on a groom-only scene there may be none, and then this still
    # checks that the answer does not change with the factor.
    x, y = WINDOW[0] // 2, WINDOW[1] // 2
    basePick = pickAt(x, y)[1]
    if not basePick:
        for py in range(WINDOW[1] // 8, WINDOW[1], WINDOW[1] // 8):
            for px in range(WINDOW[0] // 8, WINDOW[0], WINDOW[0] // 8):
                inBounds, path = pickAt(px, py)
                if inBounds and path:
                    x, y, basePick = px, py, path
                    break
            if basePick:
                break
    print("pick probe at (%d, %d) hits %r at 1x" % (x, y, basePick))

    try:
        sys.stderr = _Watch()
        for name, factor in (("2x", 2), ("4x", 4), ("Off", 1), ("2x", 2)):
            del buffers[:]
            before = len(renderErrors)
            registry.getCommandPlugin(
                "usdGenTools.setSupersample%dx" % factor).run()
            app.processEvents()
            first = frame()
            second = frame()

            check(supersample.GetFactor() == factor,
                  "menu item %s sets the factor to %d" % (name, factor))
            check(actions[name].isChecked() and
                  sum(a.isChecked() for a in actions.values()) == 1,
                  "%s is the only item checked" % name)
            check(len(renderErrors) == before,
                  "no rendering error on the %s toggle or the frame after it"
                  % name)
            check(buffers and all(
                b == (WINDOW[0] * factor, WINDOW[1] * factor)
                for b in buffers),
                  "the engine is given a %dx%d render buffer at %s"
                  % (WINDOW[0] * factor, WINDOW[1] * factor, name))
            check((first.width(), first.height()) == WINDOW and
                  (second.width(), second.height()) == WINDOW,
                  "%s resolves to the window size" % name)
            check(_MeanAbsDiff(first, second) == 0.0,
                  "the frame right after the %s toggle is already correct "
                  "(it used to need a camera move)" % name)
            if factor == 1:
                check(_MeanAbsDiff(base, first) < 0.002,
                      "Off restores the stock frame exactly")

            check(pickAt(x, y)[1] == basePick,
                  "picking (%d, %d) still answers %r at %s"
                  % (x, y, basePick, name))
    finally:
        sys.stderr = realStderr
        stageViewModule._ComputeCameraFraming = origFraming

    supersample.SetFactor(1)
    frame()
