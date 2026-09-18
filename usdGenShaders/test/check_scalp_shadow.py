#!/usr/bin/env python3
"""Sdr + Storm checks for the scalp-shadow material.

`UsdGenScalpShadow` (plan/16-ue-hair-parity.md WS3) is what makes hair shadow
the skin it grows out of, on a renderer that casts no shadows at all. The
engine publishes one synthetic Mesh over the groom's emitting surface carrying
HAIR-ONLY furTauP/furTauN, and this material turns those into the fraction of
light the hair above absorbs, written as black with that alpha so the
translucent pass multiplies the skin underneath.

Two parts:

1. Sdr parses the glslfx and registers the def with the expected inputs,
   defaults, `surface` terminal and primvars metadata, and declares exactly one
   materialTag, `translucent` -- the opaque tag would make the cap paint the
   skin black instead of darkening it.
2. On a real GL context: a grey ground plane with the cap over it, where half
   the cap carries dense hair and half carries none. The half under hair must
   darken, the bare half must not, the effect must follow `strength`, and a
   pale hair colour must darken less than a black one (which a failed glslfx
   compile -- Storm falls back silently -- cannot reproduce).

Needs the plugin search path (bin/record_usd.ps1 / bin/_env.sh set it). Exits
77 (ctest SKIP) when the bindings, NumPy/Pillow or a GL context are missing.

Usage: python3 check_scalp_shadow.py [--usdrecord PATH] [--out DIR]
"""
import argparse
import json
import os
import re
import runpy
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SHADERS = os.path.join(HERE, "..", "resources", "shaders")
FILE = "usdGenScalpShadow.glslfx"
IDENT = "UsdGenScalpShadow"
PRIMVARS = {"furTauP", "furTauN"}
EXPECTED = [
    ("baseColor", [0.035, 0.018, 0.008]),
    ("roughness", 0.35),
    ("strength", 1.0),
    ("ambient", 0.35),
]
NAMES = [n for n, _ in EXPECTED]
DEFAULTS = dict(EXPECTED)


def _skip(reason):
    print("SKIP:", reason)
    sys.exit(77)


def tag_check():
    with open(os.path.join(SHADERS, FILE)) as f:
        source = f.read()
    m = re.search(r"^-- configuration\n(\{.*?\n\})\n", source, re.M | re.S)
    if not m:
        print("FAIL: no configuration block in %s" % FILE)
        return False
    n = len(re.findall(r'"materialTag"\s*:', m.group(1)))
    if n != 1:
        print("FAIL: %s declares %d materialTag entries (want 1)" % (FILE, n))
        return False
    tag = json.loads(m.group(1)).get("metadata", {}).get("materialTag")
    if tag != "translucent":
        print("FAIL: %s materialTag %r (want 'translucent'; the cap has to "
              "blend with the skin, not replace it)" % (FILE, tag))
        return False
    print("OK: %s single materialTag %r" % (FILE, tag))
    return True


def _same_default(val, want):
    def close(a, b):
        return abs(float(a) - float(b)) <= 1e-6 * max(1.0, abs(float(b)))
    if isinstance(want, list):
        try:
            got = tuple(val)
        except TypeError:
            return False
        return len(got) == len(want) and all(close(a, b) for a, b in zip(got, want))
    try:
        return close(val, want)
    except (TypeError, ValueError):
        return False


def sdr_check():
    try:
        from pxr import Sdr
    except ImportError as e:
        print("SKIP: OpenUSD python bindings not importable (%s)." % e)
        return None
    reg = Sdr.Registry()
    ok = True
    want = sorted(NAMES)
    # Only the registered def carries the terminal output; a node parsed
    # straight from the asset has no shaderDefs.usda to declare it.
    for node, label, wantOuts in (
            (reg.GetShaderNodeFromAsset(
                os.path.abspath(os.path.join(SHADERS, FILE)),
                sourceType="glslfx"), FILE, None),
            (reg.GetShaderNodeByIdentifier(IDENT), IDENT, ["surface"])):
        if not node:
            print("FAIL: %s did not parse/register (sdrGlslfx parser or "
                  "usdGenShaders discovery missing from PXR_PLUGINPATH?)" % label)
            ok = False
            continue
        ins = node.GetShaderInputNames()
        pv = set(str(t) for t in node.GetPrimvars())
        bad = [n for n in ins
               if not _same_default(node.GetShaderInput(n).GetDefaultValue(),
                                    DEFAULTS.get(n))]
        outs = list(node.GetShaderOutputNames())
        if (sorted(ins) != want or pv != PRIMVARS or bad or
                (wantOuts is not None and outs != wantOuts)):
            print("FAIL: %s: inputs %s (want %s) | primvars %s (want %s) | "
                  "outputs %s | defaults off %s"
                  % (label, sorted(ins), want, sorted(pv), sorted(PRIMVARS),
                     outs, bad or "ok"))
            ok = False
            continue
        print("OK: %s: %d inputs, outputs %s, primvars %s"
              % (label, len(ins), outs or "(asset parse)",
                 "|".join(sorted(pv))))
    # Adding a seventh def must not displace the others from discovery.
    for ident in ("UsdGenHairPreview", "UsdGenHairPreviewTranslucent",
                  "UsdGenHairPreviewPrimvar", "UsdGenValuePreview",
                  "UsdGenHairStrands", "UsdGenHairStrandsTranslucent"):
        if not reg.GetShaderNodeByIdentifier(ident):
            print("FAIL: pre-existing def '%s' is no longer registered" % ident)
            ok = False
    print("OK: the six pre-existing defs are still registered")
    return ok


def build_stage(Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, Vt):
    """A grey plane with the scalp cap over it: dense hair on -x, none on +x."""
    stage = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())

    n = 24                                   # cap resolution across the plane
    half = 1.0

    def grid(prim, y, with_depth):
        pts, counts, indices, normals = [], [], [], []
        tauP, tauN = [], []
        for j in range(n + 1):
            for i in range(n + 1):
                x = -half + 2 * half * i / n
                z = -half + 2 * half * j / n
                pts.append(Gf.Vec3f(x, y, z))
                normals.append(Gf.Vec3f(0, 1, 0))
                # Dense hair over the -x half, none over the +x half, with a
                # hard step so the two regions are unambiguous.
                d = 6.0 if x < -0.1 else 0.0
                tauP.append(Gf.Vec3f(0.6 * d, d, 0.6 * d))
                tauN.append(Gf.Vec3f(0.6 * d, 0.0, 0.6 * d))
        for j in range(n):
            for i in range(n):
                a = j * (n + 1) + i
                counts.append(4)
                indices += [a, a + n + 1, a + n + 2, a + 1]
        mesh = UsdGeom.Mesh.Define(stage, prim)
        mesh.CreateFaceVertexCountsAttr(Vt.IntArray(counts))
        mesh.CreateFaceVertexIndicesAttr(Vt.IntArray(indices))
        mesh.CreatePointsAttr(Vt.Vec3fArray(pts))
        mesh.CreateDoubleSidedAttr(True)
        api = UsdGeom.PrimvarsAPI(mesh.GetPrim())
        api.CreatePrimvar("normals", Sdf.ValueTypeNames.Normal3fArray,
                          UsdGeom.Tokens.vertex).Set(Vt.Vec3fArray(normals))
        if with_depth:
            api.CreatePrimvar("furTauP", Sdf.ValueTypeNames.Vector3fArray,
                              UsdGeom.Tokens.vertex).Set(Vt.Vec3fArray(tauP))
            api.CreatePrimvar("furTauN", Sdf.ValueTypeNames.Vector3fArray,
                              UsdGeom.Tokens.vertex).Set(Vt.Vec3fArray(tauN))
        else:
            api.CreatePrimvar("displayColor", Sdf.ValueTypeNames.Color3fArray,
                              UsdGeom.Tokens.constant).Set(
                                  Vt.Vec3fArray([Gf.Vec3f(0.75, 0.7, 0.65)]))
        return mesh

    grid("/World/Skin", 0.0, False)
    cap = grid("/World/Cap", 0.004, True)

    material = UsdShade.Material.Define(stage, "/World/Looks/Cap")
    shader = UsdShade.Shader.Define(stage, "/World/Looks/Cap/Surface")
    shader.GetPrim().CreateAttribute("info:id", Sdf.ValueTypeNames.Token,
                                     True).Set(IDENT)
    shader.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),
                                                   "surface")
    UsdShade.MaterialBindingAPI.Apply(cap.GetPrim()).Bind(material)

    key = UsdLux.DistantLight.Define(stage, "/World/Lights/Key")
    key.CreateIntensityAttr(3.0)
    key.GetPrim().CreateAttribute("inputs:normalize", Sdf.ValueTypeNames.Bool,
                                  True).Set(True)
    UsdGeom.Xformable(key).AddRotateXYZOp().Set(Gf.Vec3f(-70, 15, 0))

    camera = UsdGeom.Camera.Define(stage, "/World/Cam")
    camera.CreateFocalLengthAttr(35.0)
    camera.CreateHorizontalApertureAttr(24.0)
    camera.CreateVerticalApertureAttr(24.0)
    camera.CreateClippingRangeAttr(Gf.Vec2f(0.05, 100.0))
    xf = UsdGeom.Xformable(camera)
    xf.AddTranslateOp().Set(Gf.Vec3d(0, 3.2, 0.001))
    xf.AddRotateXYZOp().Set(Gf.Vec3f(-90, 0, 0))
    return stage, shader, camera


def render_check(args):
    try:
        import numpy as np
        from PIL import Image
    except ImportError as e:
        _skip("NumPy/Pillow not importable (%s)" % e)
    try:
        from pxr import Gf, Sdf, Usd, UsdAppUtils, UsdGeom, UsdLux, UsdShade, Vt
    except ImportError as e:
        _skip("OpenUSD python bindings not importable (%s)" % e)
    if not os.path.exists(args.usdrecord):
        _skip("usdrecord not found at %r; pass --usdrecord or set USD"
              % args.usdrecord)
    try:
        # The returned context object owns the GL context: dropping it here
        # collects it and every renderer plugin then reports "Hgi backend not
        # supported".
        context = runpy.run_path(args.usdrecord)["_SetupOpenGLContext"]()
    except Exception as e:                                    # noqa: BLE001
        _skip("no usable GL context (%s)" % e)
    assert context is not None

    out = args.out or tempfile.mkdtemp(prefix="usdGenScalpShadow_")
    os.makedirs(out, exist_ok=True)
    stage, shader, camera = build_stage(Gf, Sdf, Usd, UsdGeom, UsdLux,
                                        UsdShade, Vt)
    recorder = UsdAppUtils.FrameRecorder("HdStormRendererPlugin", True)
    recorder.SetImageWidth(args.width)
    recorder.SetComplexity(1.3)
    recorder.SetCameraLightEnabled(False)
    recorder.SetColorCorrectionMode("sRGB")

    def render(name):
        path = os.path.join(out, name + ".png")
        assert recorder.Record(stage, camera, Usd.TimeCode.Default(), path), \
            "FrameRecorder.Record failed for " + name
        return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)

    failures = []

    def check(condition, message):
        print(("OK:   " if condition else "FAIL: ") + message)
        if not condition:
            failures.append(message)

    strength = shader.CreateInput("strength", Sdf.ValueTypeNames.Float)
    base = shader.CreateInput("baseColor", Sdf.ValueTypeNames.Color3f)
    base.Set(Gf.Vec3f(0.02, 0.01, 0.005))

    strength.Set(0.0)
    off = render("strength0")
    strength.Set(1.0)
    on = render("strength1")

    # The plane fills the frame; sample well inside each half, away from the
    # step at x = -0.1 and from the edges.
    h, w = off.shape[:2]
    haired = (slice(h // 4, 3 * h // 4), slice(w // 8, w * 3 // 8))
    bare = (slice(h // 4, 3 * h // 4), slice(w * 5 // 8, w * 7 // 8))
    lit = off[haired].mean()
    check(lit > 30.0, "the plane renders lit without the cap (%.1f/255)" % lit)
    shaded_delta = on[haired].mean() - off[haired].mean()
    bare_delta = on[bare].mean() - off[bare].mean()
    print("      under hair %.1f -> %.1f, bare %.1f -> %.1f"
          % (off[haired].mean(), on[haired].mean(),
             off[bare].mean(), on[bare].mean()))
    check(shaded_delta < -15.0,
          "skin under dense hair darkens (%.1f/255)" % shaded_delta)
    check(abs(bare_delta) < 2.0,
          "bare skin is untouched (%.1f/255)" % bare_delta)

    # Half the strength, less than half again as much shadow: the effect is
    # driven by the input, not baked in. A fallback shader ignores it.
    strength.Set(0.4)
    partial = render("strength04")
    partial_delta = partial[haired].mean() - off[haired].mean()
    check(shaded_delta < partial_delta < 0.0,
          "strength scales the shadow (%.1f between %.1f and 0)"
          % (partial_delta, shaded_delta))

    # Pale hair absorbs less, so it must shadow less. Only a compiled shader
    # that actually evaluates a_f(baseColor) can show this.
    strength.Set(1.0)
    base.Set(Gf.Vec3f(0.9, 0.88, 0.85))
    pale = render("pale")
    pale_delta = pale[haired].mean() - off[haired].mean()
    check(pale_delta > shaded_delta + 5.0,
          "a pale coat shadows less than a black one (%.1f vs %.1f), so "
          "baseColor reaches the shader and it compiled"
          % (pale_delta, shaded_delta))

    print("images in", out)
    return not failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--usdrecord", default=os.environ.get(
        "USDGEN_USDRECORD",
        os.path.join(os.environ.get("USD", ""), "bin", "usdrecord")))
    parser.add_argument("--out", default="")
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--no-render", action="store_true")
    args = parser.parse_args()

    ok = tag_check()
    if not ok:
        print("SCALP SHADOW CHECK: FAIL")
        return 1
    res = sdr_check()
    if res is None:
        print("SCALP SHADOW CHECK: PARTIAL -- materialTag PASS, Sdr half "
              "SKIPPED (no pxr)")
        return 77
    ok = res
    if not args.no_render:
        ok = render_check(args) and ok
    print("SCALP SHADOW CHECK:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
