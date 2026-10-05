#!/usr/bin/env python3
"""Storm render check for the strand-hair material.

Proves, on a real GL context, that `UsdGenHairStrands` /
`UsdGenHairStrandsTranslucent` COMPILE and that the pieces the port depends on
are actually live. A glslfx that fails to compile does not raise: Storm falls
back to a fixed shader and the hair silently goes flat, so every assertion here
is differential -- it changes one input (or the light) and requires the pixels
to move by more than noise. A fallback shader ignores all of them.

  1. the material compiles and its inputs reach it (baseColor black vs red)
  2. lights respond (the key light is rotated)
  3. the baked hair count drives dual scattering (selfShadow 0 vs 2, and more
     extinction must darken the coat)
  4. coverage compensation reaches alpha (hairCoverageScale 0 clears the hair)
  5. both materialTag variants draw

The scene is built in memory: 2400 cubic bspline curves on a sphere with the
primvars a usdGen tile publishes (widths, hairT, hairId, st,
minScreenSpaceWidths, furTauP/furTauN). The key light sets
`inputs:normalize = 1`; without it HdStLight multiplies a distant light's
intensity by the solid angle of a 0.53-degree disc and the scene renders black
(pxr/imaging/hdSt/light.cpp:161-209).

Needs the plugin search path (bin/record_usd.ps1 / bin/_env.sh set it), a GL
context, NumPy and Pillow. Exits 77 (ctest SKIP) when any of those is missing.

Usage: python3 check_hair_strands_render.py [--usdrecord PATH] [--out DIR]
"""
import argparse
import math
import os
import random
import runpy
import sys
import tempfile


def _skip(reason):
    print("SKIP:", reason)
    sys.exit(77)


def build_stage(Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, Vt, identifier,
                environment):
    stage = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())

    ncurves, ncv, length = 2400, 8, 0.55
    rng = random.Random(7)
    pts, widths, hairT, counts, hairId, st = [], [], [], [], [], []
    tauP, tauN = [], []
    for _ in range(ncurves):
        while True:
            n = Gf.Vec3f(rng.uniform(-1, 1), rng.uniform(0.1, 1.0),
                         rng.uniform(-1, 1))
            if n.GetLength() > 1e-3:
                break
        n = n.GetNormalized()
        root = n * 1.0
        for i in range(ncv):
            t = i / (ncv - 1.0)
            p = root + n * (length * t) + Gf.Vec3f(0.0, -0.9 * t * t * length,
                                                   0.0)
            pts.append(Gf.Vec3f(p))
            widths.append(0.0075 * (1.0 - 0.7 * t))
            hairT.append(t)
            # A plausible optical depth: dense toward the root, thinning out
            # at the tip, and deeper downward (into the coat) than upward.
            depth = 6.0 * (1.0 - t)
            tauP.append(Gf.Vec3f(0.5 * depth, 0.15 * depth, 0.5 * depth))
            tauN.append(Gf.Vec3f(0.5 * depth, depth, 0.5 * depth))
        counts.append(ncv)
        hairId.append(rng.random())
        st.append(Gf.Vec2f(0.5 + 0.5 * n[0], 0.5 + 0.5 * n[2]))

    curves = UsdGeom.BasisCurves.Define(stage, "/World/Hair")
    curves.CreateTypeAttr(UsdGeom.Tokens.cubic)
    curves.CreateBasisAttr(UsdGeom.Tokens.bspline)
    curves.CreateWrapAttr(UsdGeom.Tokens.pinned)
    curves.CreateCurveVertexCountsAttr(Vt.IntArray(counts))
    curves.CreatePointsAttr(Vt.Vec3fArray(pts))
    curves.CreateWidthsAttr(Vt.FloatArray(widths))
    curves.SetWidthsInterpolation(UsdGeom.Tokens.vertex)
    api = UsdGeom.PrimvarsAPI(curves.GetPrim())
    api.CreatePrimvar("hairT", Sdf.ValueTypeNames.FloatArray,
                      UsdGeom.Tokens.vertex).Set(Vt.FloatArray(hairT))
    api.CreatePrimvar("hairId", Sdf.ValueTypeNames.FloatArray,
                      UsdGeom.Tokens.uniform).Set(Vt.FloatArray(hairId))
    api.CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                      UsdGeom.Tokens.uniform).Set(Vt.Vec2fArray(st))
    api.CreatePrimvar("minScreenSpaceWidths", Sdf.ValueTypeNames.FloatArray,
                      UsdGeom.Tokens.constant).Set(Vt.FloatArray([1.0]))
    api.CreatePrimvar("furTauP", Sdf.ValueTypeNames.Vector3fArray,
                      UsdGeom.Tokens.vertex).Set(Vt.Vec3fArray(tauP))
    api.CreatePrimvar("furTauN", Sdf.ValueTypeNames.Vector3fArray,
                      UsdGeom.Tokens.vertex).Set(Vt.Vec3fArray(tauN))
    # NOTE: no displayColor -- the shader would take it as the root albedo and
    # the baseColor assertion below would then be testing nothing.

    material = UsdShade.Material.Define(stage, "/World/Looks/Hair")
    shader = UsdShade.Shader.Define(stage, "/World/Looks/Hair/Surface")
    shader.GetPrim().CreateAttribute("info:id", Sdf.ValueTypeNames.Token,
                                     True).Set(identifier)
    shader.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    material.CreateSurfaceOutput().ConnectToSource(
        shader.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(curves.GetPrim()).Bind(material)

    key = UsdLux.DistantLight.Define(stage, "/World/Lights/Key")
    key.CreateIntensityAttr(3.0)
    key.GetPrim().CreateAttribute("inputs:normalize", Sdf.ValueTypeNames.Bool,
                                  True).Set(True)
    rotate = UsdGeom.Xformable(key).AddRotateXYZOp()
    rotate.Set(Gf.Vec3f(-35, 25, 0))

    dome = UsdLux.DomeLight.Define(stage, "/World/Lights/Sky")
    dome.CreateIntensityAttr(0.4)
    dome.GetPrim().CreateAttribute("inputs:texture:file",
                                   Sdf.ValueTypeNames.Asset,
                                   True).Set(Sdf.AssetPath(environment))

    camera = UsdGeom.Camera.Define(stage, "/World/Cam")
    camera.CreateFocalLengthAttr(35.0)
    camera.CreateHorizontalApertureAttr(24.0)
    camera.CreateVerticalApertureAttr(18.0)
    camera.CreateClippingRangeAttr(Gf.Vec2f(0.05, 100.0))
    UsdGeom.Xformable(camera).AddTranslateOp().Set(Gf.Vec3d(0, 0.35, 4.6))
    return stage, shader, rotate, camera


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--usdrecord", default=os.environ.get(
        "USDGEN_USDRECORD",
        os.path.join(os.environ.get("USD", ""), "bin", "usdrecord")))
    parser.add_argument("--out", default="")
    parser.add_argument("--width", type=int, default=480)
    args = parser.parse_args()

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

    out = args.out or tempfile.mkdtemp(prefix="usdGenHairStrands_")
    os.makedirs(out, exist_ok=True)
    environment = os.path.join(out, "white_environment.png")
    # A constant white environment: exercises the dome path without an HDRI
    # and without Storm's per-frame "dome light has no texture" warning.
    Image.new("RGB", (8, 4), (255, 255, 255)).save(environment)

    try:
        ctx = runpy.run_path(args.usdrecord)["_SetupOpenGLContext"]()
    except Exception as e:                                    # noqa: BLE001
        _skip("no usable GL context (%s)" % e)

    # One recorder per stage: UsdImagingGLEngine populates on its first Render
    # and keeps that stage, so a second stage handed to the same recorder is
    # never drawn and every image silently repeats the first stage's.
    def new_recorder():
        recorder = UsdAppUtils.FrameRecorder("HdStormRendererPlugin", True)
        recorder.SetImageWidth(args.width)
        recorder.SetComplexity(1.3)
        recorder.SetCameraLightEnabled(False)
        recorder.SetColorCorrectionMode("sRGB")
        return recorder

    failures = []

    def check(condition, message):
        print(("OK:   " if condition else "FAIL: ") + message)
        if not condition:
            failures.append(message)

    recorder = [None]

    def render(stage, camera, name):
        path = os.path.join(out, name + ".png")
        assert recorder[0].Record(stage, camera, Usd.TimeCode.Default(), path), \
            "FrameRecorder.Record failed for " + name
        return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)

    for identifier in ("UsdGenHairStrands", "UsdGenHairStrandsTranslucent"):
        tag = "opaque" if identifier == "UsdGenHairStrands" else "translucent"
        stage, shader, rotate, camera = build_stage(
            Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, Vt, identifier,
            environment)
        recorder[0] = new_recorder()

        base = shader.CreateInput("baseColor", Sdf.ValueTypeNames.Color3f)
        tip = shader.CreateInput("tipColor", Sdf.ValueTypeNames.Color3f)
        self_shadow = shader.CreateInput("selfShadow",
                                         Sdf.ValueTypeNames.Float)
        coverage = shader.CreateInput("hairCoverageScale",
                                      Sdf.ValueTypeNames.Float)
        base.Set(Gf.Vec3f(0.5, 0.5, 0.5))
        tip.Set(Gf.Vec3f(0.5, 0.5, 0.5))
        self_shadow.Set(1.0)
        coverage.Set(1.0)

        grey = render(stage, camera, tag + "_grey")
        check(float(grey.max()) > 20.0,
              "%s: the scene renders something (max %.1f)"
              % (tag, grey.max()))

        base.Set(Gf.Vec3f(0.8, 0.02, 0.02))
        tip.Set(Gf.Vec3f(0.8, 0.02, 0.02))
        red = render(stage, camera, tag + "_red")
        # A failed glslfx compile leaves Storm's fallback shader, which knows
        # nothing about baseColor: the two images would be identical.
        channel_swing = float(np.mean(red[..., 0] - red[..., 2]) -
                              np.mean(grey[..., 0] - grey[..., 2]))
        check(channel_swing > 3.0,
              "%s: baseColor reaches the shader, so it compiled "
              "(R-B swing %.2f/255)" % (tag, channel_swing))
        base.Set(Gf.Vec3f(0.5, 0.5, 0.5))
        tip.Set(Gf.Vec3f(0.5, 0.5, 0.5))

        rotate.Set(Gf.Vec3f(-35, 155, 0))
        relit = render(stage, camera, tag + "_relit")
        check(float(np.mean(np.abs(relit - grey))) > 1.0,
              "%s: rotating the key light changes the coat (mean |d| %.2f/255)"
              % (tag, float(np.mean(np.abs(relit - grey)))))
        rotate.Set(Gf.Vec3f(-35, 25, 0))

        self_shadow.Set(0.0)
        unshadowed = render(stage, camera, tag + "_unshadowed")
        self_shadow.Set(2.0)
        shadowed = render(stage, camera, tag + "_shadowed")
        delta = float(np.mean(unshadowed - shadowed))
        check(abs(delta) > 0.5,
              "%s: the baked hair count drives dual scattering "
              "(mean delta %.2f/255)" % (tag, delta))
        check(delta > 0.0,
              "%s: more extinction darkens the coat (mean delta %.2f/255)"
              % (tag, delta))
        self_shadow.Set(1.0)

        coverage.Set(0.0)
        cleared = render(stage, camera, tag + "_nocoverage")
        # With zero coverage every strand is fully transparent: the frame must
        # collapse to the (unlit, empty) background.
        check(float(np.mean(np.abs(cleared - grey))) >
              float(np.mean(np.abs(unshadowed - grey))),
              "%s: hairCoverageScale reaches alpha (clearing coverage moves "
              "the frame more than the whole extinction range)" % tag)
        coverage.Set(1.0)

        recorder[0] = None
        del stage

    print("wrote renders to", out)
    print("HAIR STRANDS RENDER CHECK:",
          "PASS" if not failures else "FAIL (%d)" % len(failures))
    del ctx
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
