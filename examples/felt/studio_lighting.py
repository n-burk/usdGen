"""Three-point UsdLux studio for the felt sphere and its motion examples.

Run in the usdGen/OpenUSD Python environment to update the existing scenes.
Uses standard RectLight and textured DomeLight inputs shared by Storm and hdMoonray.
"""
from pathlib import Path
from pxr import Gf, Usd, UsdGeom, UsdLux


def add_studio_lighting(stage, path='/Studio', center=(0, 0, 0),
                        hdr='textures/studio_contrast.exr'):
    stage.RemovePrim(path)
    rig = UsdGeom.Xform.Define(stage, path)
    rig.AddTranslateOp().Set(Gf.Vec3d(*center))
    rig.GetPrim().SetDocumentation(
        'Three-point studio: broad key, weaker fill, rear strip back light, '
        'and a low-intensity contrast HDR environment. '
        'Enable scene lights and scene materials in usdview.')
    specs = (
        ('Key', (-1.8, -2.4, 2.2), (1.5, 1.5), 12., (1., .96, .9)),
        ('Fill', (2.2, -1.2, 1.), (2., 2.), 1.7, (.9, .95, 1.)),
        ('Back', (1.1, 1.8, 2.), (1., 1.8), 16., (1., 1., 1.)),
    )
    for name, position, size, intensity, color in specs:
        light = UsdLux.RectLight.Define(stage, path + '/' + name)
        light.CreateWidthAttr(size[0]); light.CreateHeightAttr(size[1])
        light.CreateIntensityAttr(intensity); light.CreateExposureAttr(0.)
        light.CreateNormalizeAttr(False)
        light.CreateColorAttr(Gf.Vec3f(*color))
        light.CreateDiffuseAttr(1.); light.CreateSpecularAttr(1.)
        UsdLux.ShadowAPI.Apply(light.GetPrim()).CreateShadowEnableAttr(True)
        UsdGeom.Xformable(light).AddTransformOp().Set(Gf.Matrix4d().SetLookAt(
            Gf.Vec3d(*position), Gf.Vec3d(0), Gf.Vec3d(0, 0, 1)).GetInverse())
    environment = UsdLux.DomeLight.Define(stage, path + '/Environment')
    environment.CreateTextureFileAttr(hdr)
    environment.CreateTextureFormatAttr('latlong')
    environment.CreateIntensityAttr(.12)
    environment.CreateExposureAttr(0.)
    environment.OrientToStageUpAxis()


def main():
    here = Path(__file__).resolve().parent
    for file, parent, center in (
        (here / 'felt_sphere.usda', '', (0, 0, 0)),
        (here / '../motion/felt_sphere_animated.usda', '/World', (0, 0, .5)),
        (here / '../motion/felt_sphere_groom_outside_xform.usda', '/World', (0, 0, .5)),
    ):
        stage = Usd.Stage.Open(str(file.resolve()))
        for old in ('Key', 'Fill'):
            stage.RemovePrim(parent + '/' + old)
        add_studio_lighting(stage, parent + '/Studio', center,
                            '../felt/textures/studio_contrast.exr' if parent else
                            'textures/studio_contrast.exr')
        stage.GetRootLayer().Save()
        print(file.resolve())


if __name__ == '__main__':
    main()
