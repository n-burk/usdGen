"""Sphere-specific open clumping and Moonray lookdev sampling preset."""
from pathlib import Path
from pxr import Gf, Sdf, Usd, UsdRender

# More primary samples for subpixel fibres, fewer branching rays per hit.
MOONRAY = {
    'sampling_mode': 'adaptive',
    'min_adaptive_samples': 32,
    'max_adaptive_samples': 128,
    'target_adaptive_error': 3.0,
    'light_samples': 1,
    'bsdf_samples': 1,
    'max_depth': 4,
    'max_diffuse_depth': 2,
    'max_glossy_depth': 2,
}


def configure(stage):
    for coat, amount, profile in (
        ('Felt', .18, [(0, 0), (.25, .04), (.7, .5), (1, .35)]),
        ('FeltCurls', .45, [(0, 0), (.25, .08), (.7, .65), (1, .5)]),
    ):
        base = '/FeltSphere/Groom/' + coat
        stage.GetPrimAtPath(base + '/Expressions/tightness').GetAttribute(
            'usdGen:expr:source').Set(str(amount) + ' * ptex("map")')
        clump = stage.GetPrimAtPath(base + '/Ops/microclump')
        clump.GetAttribute('usdGen:clump:amount').Set(amount)
        clump.GetAttribute('usdGen:clump:profile:knots').Set(profile)
        # A fraction of the existing fibres bridge clumps, without new roots.
        for name, value in (('rate', .25), ('amount', .65), ('falloff', .7)):
            clump.CreateAttribute('usdGen:clump:stray:' + name,
                                  Sdf.ValueTypeNames.Float).Set(value)

    settings = UsdRender.Settings.Define(stage, '/Render/FeltMoonray')
    stage.SetMetadata('renderSettingsPrimPath', str(settings.GetPath()))
    settings.CreateCameraRel().SetTargets(['/Camera'])
    settings.CreateResolutionAttr(Gf.Vec2i(800, 800))
    settings.CreateDisableMotionBlurAttr(True)
    settings.CreateDisableDepthOfFieldAttr(True)
    settings.GetPrim().SetDocumentation(
        'Felt lookdev: adaptive primary sampling, single light/BSDF samples, '
        'two diffuse bounces; no motion blur or depth of field on this static study.')
    for name, value in MOONRAY.items():
        typ = (Sdf.ValueTypeNames.Token if isinstance(value, str) else
               Sdf.ValueTypeNames.Int if isinstance(value, int) else Sdf.ValueTypeNames.Float)
        attr_name = 'moonray:sceneVariable:' + name
        attr = settings.GetPrim().GetAttribute(attr_name)
        if attr and attr.GetTypeName() != typ:
            settings.GetPrim().RemoveProperty(attr_name)
        settings.GetPrim().CreateAttribute(attr_name, typ).Set(value)


def main():
    path = Path(__file__).with_name('felt_sphere.usda')
    stage = Usd.Stage.Open(str(path))
    configure(stage)
    stage.GetRootLayer().Save()
    print(path)


if __name__ == '__main__':
    main()
