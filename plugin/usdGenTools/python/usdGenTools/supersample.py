# usdGenTools.supersample -- opt-in supersampling for usdview's Storm viewport.
#
# usdrecord can render the hair at N x and box-downsample in linear light
# (bin/record_usd.ps1 -Supersample, bin/downsample_linear.py); that is the
# converged image the shader work is judged against. This module does the same
# thing interactively, without patching OpenUSD.
#
# WHY IT NEEDS AN FBO OF ITS OWN. Asking the engine for a bigger render buffer
# is one line (StageView._paintGLWithRenderer already calls
# SetRenderBufferSize/SetFraming), but the present step then draws the AOV at
# that size into the window: with a valid framing HdxTaskController* sets
#     dstRegion = (0, 0, renderBufferSize[0], renderBufferSize[1])
# (hdx/taskControllerSceneIndex.cpp:2636, hdx/taskController.cpp:2148), and
# HgiInteropOpenGL::CompositeToInterop does glViewport(*dstRegion) before its
# fullscreen triangle (hgiInterop/opengl.cpp:246). There is no app-facing knob
# for that region, so an N x render buffer composites N x into the widget and
# all but the lower-left 1/N^2 falls off the edge.
#
# What the app *can* choose is the framebuffer it is composited into: the
# present task's dstFramebuffer is empty in usdview (SetPresentationOutput is
# not even Python-wrapped), so HgiInterop composites into whatever is bound
# (hgiInterop/opengl.cpp:152). So: bind our own N x FBO for the whole paint,
# let usdview render, guides, mask and reticles into it at N x, then resolve it
# down into the widget's framebuffer ourselves with an exact N x N box filter
# and draw the HUD afterwards at window resolution.
#
# Colour space: HdxColorCorrectionTask has already applied the sRGB OETF by the
# time we see the pixels, so the buffer is display-referred. Averaging those
# code values is not the average of the light; the resolve shader therefore
# decodes to linear, averages, and re-encodes -- the same filter
# bin/downsample_linear.py applies offline. Set
# USDGEN_USDVIEW_SUPERSAMPLE_LINEAR=0 to average the code values instead (which
# is what a plain glBlitFramebuffer would do), or =1 to force the decode when
# usdview's colour correction is off.
#
# Cost goes as N^2 in fill and in AOV memory (the MSAA colour target is
# sampleCount * N^2 * window pixels), so this is a look-dev switch, not a
# default.
#
#   USDGEN_USDVIEW_SUPERSAMPLE=2 .\bin\launch_usdview.ps1 scene.usda
#
# or the usdGen > Viewport Supersampling menu.

import os

from pxr import Tf

FACTOR_ENV = "USDGEN_USDVIEW_SUPERSAMPLE"
LINEAR_ENV = "USDGEN_USDVIEW_SUPERSAMPLE_LINEAR"

MAX_FACTOR = 8

_factor = None
_installed = False
_origPaintGL = None
_observers = []

_VERTEX_SHADER = """#version 330
void main()
{
    // Fullscreen triangle from gl_VertexID; no attributes, no buffers.
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
"""

_FRAGMENT_SHADER = """#version 330
uniform sampler2D srcTex;
uniform int ssFactor;
uniform int ssDecode;
out vec4 fragColor;

vec3 SrgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 LinearToSrgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(0.0031308, c));
}

void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * ssFactor;
    vec4 acc = vec4(0.0);
    for (int y = 0; y < ssFactor; ++y) {
        for (int x = 0; x < ssFactor; ++x) {
            vec4 s = texelFetch(srcTex, base + ivec2(x, y), 0);
            // The colour arrives premultiplied by coverage (HgiInterop
            // composites with srcColor=ONE over a transparent clear), so the
            // channels average independently -- no alpha weighting here, and
            // no unpremultiply/repremultiply round trip either.
            if (ssDecode != 0) {
                s.rgb = SrgbToLinear(s.rgb);
            }
            acc += s;
        }
    }
    acc /= float(ssFactor * ssFactor);
    if (ssDecode != 0) {
        acc.rgb = LinearToSrgb(acc.rgb);
    }
    fragColor = acc;
}
"""


def _EnvFactor():
    raw = os.environ.get(FACTOR_ENV, "").strip()
    if not raw:
        return 1
    try:
        value = int(raw)
    except ValueError:
        Tf.Warn("%s=%r is not an integer; supersampling stays off."
                % (FACTOR_ENV, raw))
        return 1
    if value < 1 or value > MAX_FACTOR:
        Tf.Warn("%s=%d is out of range 1..%d; supersampling stays off."
                % (FACTOR_ENV, value, MAX_FACTOR))
        return 1
    return value


def GetFactor():
    """The current supersampling factor; 1 means off."""
    global _factor
    if _factor is None:
        _factor = _EnvFactor()
    return _factor


def SetFactor(factor):
    """Set the supersampling factor. 1 restores the stock path."""
    global _factor
    factor = max(1, min(int(factor), MAX_FACTOR))
    changed = factor != GetFactor()
    _factor = factor
    if changed:
        for observer in list(_observers):
            observer(factor)
    return factor


def AddFactorObserver(observer):
    """Call observer(factor) whenever the factor changes.

    The menu uses this so its check marks follow a change it did not make --
    notably the fall back to 1 when the N x target cannot be allocated."""
    _observers.append(observer)


def _ShouldDecode(stageView):
    """Whether the buffer we are about to average is display-referred."""
    override = os.environ.get(LINEAR_ENV, "").strip()
    if override:
        return override not in ("0", "false", "False", "off")
    try:
        mode = stageView._dataModel.viewSettings.colorCorrectionMode
    except AttributeError:
        return True
    return str(mode) != "disabled"


class _Targets(object):
    """The N x colour/depth target and the resolve program, per StageView."""

    def __init__(self):
        self.width = 0
        self.height = 0
        self.fbo = 0
        self.colorTex = 0
        self.depthRbo = 0
        self.program = 0
        self.vao = 0
        self.locSrc = -1
        self.locFactor = -1
        self.locDecode = -1

    def _CompileProgram(self, GL):
        if self.program:
            return True

        def compile(src, stage):
            shader = GL.glCreateShader(stage)
            GL.glShaderSource(shader, src)
            GL.glCompileShader(shader)
            if not GL.glGetShaderiv(shader, GL.GL_COMPILE_STATUS):
                Tf.Warn("usdGen supersample shader failed to compile: %s"
                        % GL.glGetShaderInfoLog(shader))
                GL.glDeleteShader(shader)
                return 0
            return shader

        vs = compile(_VERTEX_SHADER, GL.GL_VERTEX_SHADER)
        fs = compile(_FRAGMENT_SHADER, GL.GL_FRAGMENT_SHADER)
        if not vs or not fs:
            return False
        program = GL.glCreateProgram()
        GL.glAttachShader(program, vs)
        GL.glAttachShader(program, fs)
        GL.glLinkProgram(program)
        GL.glDeleteShader(vs)
        GL.glDeleteShader(fs)
        if not GL.glGetProgramiv(program, GL.GL_LINK_STATUS):
            Tf.Warn("usdGen supersample program failed to link: %s"
                    % GL.glGetProgramInfoLog(program))
            GL.glDeleteProgram(program)
            return False
        self.program = program
        self.locSrc = GL.glGetUniformLocation(program, "srcTex")
        self.locFactor = GL.glGetUniformLocation(program, "ssFactor")
        self.locDecode = GL.glGetUniformLocation(program, "ssDecode")
        self.vao = GL.glGenVertexArrays(1)
        _CheckGL(GL, "compiling the resolve program")
        return True

    def Resize(self, GL, width, height):
        if self.width == width and self.height == height and self.fbo:
            return True
        self.Release(GL)

        _CheckGL(GL, "Release()")

        self.colorTex = GL.glGenTextures(1)
        _CheckGL(GL, "glGenTextures")
        GL.glBindTexture(GL.GL_TEXTURE_2D, self.colorTex)
        _CheckGL(GL, "glBindTexture")
        GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, GL.GL_RGBA8, width, height, 0,
                        GL.GL_RGBA, GL.GL_UNSIGNED_BYTE, None)
        _CheckGL(GL, "glTexImage2D")
        # texelFetch ignores the filter, but a complete texture needs one that
        # does not ask for mips.
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER,
                           GL.GL_NEAREST)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER,
                           GL.GL_NEAREST)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_S,
                           GL.GL_CLAMP_TO_EDGE)
        GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_T,
                           GL.GL_CLAMP_TO_EDGE)
        GL.glBindTexture(GL.GL_TEXTURE_2D, 0)
        _CheckGL(GL, "the colour texture")

        # The present step depth-tests and writes gl_FragDepth, and the axis
        # and camera guides draw depth-tested, so the target needs depth.
        self.depthRbo = GL.glGenRenderbuffers(1)
        GL.glBindRenderbuffer(GL.GL_RENDERBUFFER, self.depthRbo)
        GL.glRenderbufferStorage(GL.GL_RENDERBUFFER, GL.GL_DEPTH24_STENCIL8,
                                 width, height)
        GL.glBindRenderbuffer(GL.GL_RENDERBUFFER, 0)
        _CheckGL(GL, "the depth renderbuffer")

        self.fbo = GL.glGenFramebuffers(1)
        GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, self.fbo)
        GL.glFramebufferTexture2D(GL.GL_FRAMEBUFFER, GL.GL_COLOR_ATTACHMENT0,
                                  GL.GL_TEXTURE_2D, self.colorTex, 0)
        GL.glFramebufferRenderbuffer(GL.GL_FRAMEBUFFER,
                                     GL.GL_DEPTH_STENCIL_ATTACHMENT,
                                     GL.GL_RENDERBUFFER, self.depthRbo)
        status = GL.glCheckFramebufferStatus(GL.GL_FRAMEBUFFER)
        _CheckGL(GL, "the framebuffer")
        # Leave the binding alone: the caller restores the widget's
        # framebuffer, which is NOT 0 under QOpenGLWidget.
        if status != GL.GL_FRAMEBUFFER_COMPLETE:
            Tf.Warn("usdGen supersample target %dx%d is incomplete (0x%x); "
                    "falling back to the stock viewport."
                    % (width, height, status))
            self.Release(GL)
            return False

        self.width = width
        self.height = height
        return True

    def Release(self, GL):
        if self.fbo:
            GL.glDeleteFramebuffers(1, [self.fbo])
        if self.colorTex:
            GL.glDeleteTextures([self.colorTex])
        if self.depthRbo:
            GL.glDeleteRenderbuffers(1, [self.depthRbo])
        self.fbo = self.colorTex = self.depthRbo = 0
        self.width = self.height = 0


def _GetTargets(stageView):
    targets = getattr(stageView, "_usdGenSupersampleTargets", None)
    if targets is None:
        targets = _Targets()
        stageView._usdGenSupersampleTargets = targets
    return targets


def _Resolve(GL, targets, width, height, factor, decode):
    """Box-filter the N x colour target into the currently bound framebuffer."""
    GL.glDisable(GL.GL_DEPTH_TEST)
    GL.glDisable(GL.GL_BLEND)
    GL.glDisable(GL.GL_SCISSOR_TEST)
    GL.glDepthMask(GL.GL_FALSE)
    GL.glViewport(0, 0, width, height)

    GL.glUseProgram(targets.program)
    GL.glBindVertexArray(targets.vao)
    GL.glActiveTexture(GL.GL_TEXTURE0)
    GL.glBindTexture(GL.GL_TEXTURE_2D, targets.colorTex)
    GL.glUniform1i(targets.locSrc, 0)
    GL.glUniform1i(targets.locFactor, factor)
    GL.glUniform1i(targets.locDecode, 1 if decode else 0)
    GL.glDrawArrays(GL.GL_TRIANGLES, 0, 3)

    GL.glBindVertexArray(0)
    GL.glBindTexture(GL.GL_TEXTURE_2D, 0)
    GL.glUseProgram(0)
    GL.glDepthMask(GL.GL_TRUE)


def _Debugging():
    return bool(os.environ.get("USDGEN_USDVIEW_SUPERSAMPLE_DEBUG", "").strip())


def _CheckGL(GL, where):
    """Report (and clear) pending GL errors. USDGEN_USDVIEW_SUPERSAMPLE_DEBUG."""
    if not _Debugging():
        return
    err = GL.glGetError()
    while err != GL.GL_NO_ERROR:
        print("usdGen supersample: GL error 0x%x after %s" % (err, where))
        err = GL.glGetError()


def _WindowFramebuffer(stageView, GL):
    """The framebuffer usdview's widget renders into.

    QOpenGLWidget answers this directly; only a real QGLWidget (the PySide2
    path in Usdviewq/qt.py) has no such method, and there the draw binding at
    the top of a paint is the widget's."""
    getter = getattr(stageView, "defaultFramebufferObject", None)
    if getter is not None:
        return int(getter())
    return int(GL.glGetIntegerv(GL.GL_DRAW_FRAMEBUFFER_BINDING))


def _PaintGLSupersampled(self, renderer):
    factor = GetFactor()
    if factor <= 1:
        return _origPaintGL(self, renderer)

    from OpenGL import GL

    _CheckGL(GL, "entering the paint (left by someone else)")

    width, height = self.GetPhysicalWindowSize()
    if width <= 0 or height <= 0:
        return _origPaintGL(self, renderer)

    # The framebuffer to put the resolved image back into. Take it before any
    # GL call of ours can change the binding, and take it from Qt: under
    # QOpenGLWidget the widget renders into an FBO of its own, so this is not
    # 0, and a paint that ends with 0 bound draws into the wrong place.
    windowFbo = _WindowFramebuffer(self, GL)

    targets = _GetTargets(self)
    if not (targets._CompileProgram(GL) and
            targets.Resize(GL, width * factor, height * factor)):
        # Anything that cannot be set up falls back to the stock viewport
        # rather than leaving a black window.
        GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, windowFbo)
        SetFactor(1)
        return _origPaintGL(self, renderer)

    savedPhysicalSize = self._physicalWindowSize
    savedDrawHUD = self.__dict__.get("drawHUD")
    deferredHUD = []

    # The HUD and the render stats overlay are laid out in window pixels
    # (HUD.draw divides by qglwidget.width()), so drawing them into the N x
    # target would magnify them and the resolve would then blur them back.
    # Hold them for after the resolve.
    self.drawHUD = lambda r: deferredHUD.append(r)
    # Everything that derives from GetPhysicalWindowSize -- the window
    # viewport, the camera viewport, the render buffer size and the framing --
    # follows from this one value.
    self._physicalWindowSize = (width * factor, height * factor)

    try:
        GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, targets.fbo)
        GL.glViewport(0, 0, width * factor, height * factor)
        _origPaintGL(self, renderer)
    finally:
        self._physicalWindowSize = savedPhysicalSize
        if savedDrawHUD is None:
            del self.drawHUD
        else:
            self.drawHUD = savedDrawHUD
        GL.glBindFramebuffer(GL.GL_FRAMEBUFFER, windowFbo)

    _CheckGL(GL, "the supersampled paint")
    _Resolve(GL, targets, width, height, factor, _ShouldDecode(self))
    _CheckGL(GL, "the resolve")

    if deferredHUD:
        GL.glEnable(GL.GL_BLEND)
        GL.glBlendFunc(GL.GL_SRC_ALPHA, GL.GL_ONE_MINUS_SRC_ALPHA)
        self.drawHUD(deferredHUD[0])
        GL.glDisable(GL.GL_BLEND)
        _CheckGL(GL, "the HUD")


def Install():
    """Patch StageView so the viewport can be supersampled. Idempotent."""
    global _installed, _origPaintGL
    if _installed:
        return True
    from pxr.Usdviewq.stageView import StageView
    if not hasattr(StageView, "_paintGLWithRenderer"):
        Tf.Warn("This usdview's StageView has no _paintGLWithRenderer; "
                "usdGen viewport supersampling is unavailable.")
        return False
    _origPaintGL = StageView._paintGLWithRenderer
    StageView._paintGLWithRenderer = _PaintGLSupersampled
    _installed = True
    return True


def IsInstalled():
    return _installed
