"""testusdview: capture the felt studio in Storm and in-process Moonray.

Scene lights only; each light is also disabled separately to verify that all
three area lights and the HDR affect the rendered image. Writes adjacent studio_validation/.
"""
def testUsdviewInputFunction(controller):
    import json
    import time
    from pathlib import Path
    from pxr import Usd, UsdLux
    from pxr.Usdviewq.qt import QtWidgets
    from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
    from OpenGL import GL
    from PIL import Image, ImageChops, ImageStat

    stage = controller._dataModel.stage
    scene = Path(stage.GetRootLayer().realPath)
    out = scene.parent / 'studio_validation' / scene.stem
    out.mkdir(parents=True, exist_ok=True)
    parent = '' if scene.stem == 'felt_sphere' else '/World'
    lights = [UsdLux.RectLight(stage.GetPrimAtPath(parent + '/Studio/' + n))
              for n in ('Key', 'Fill', 'Back')]
    assert all(lights)
    lights.append(UsdLux.DomeLight(stage.GetPrimAtPath(parent + '/Studio/Environment')))
    assert lights[-1] and lights[-1].GetTextureFileAttr().Get().resolvedPath
    assert len([p for p in stage.Traverse() if p.HasAPI(UsdLux.LightAPI)]) == 4
    stage.SetEditTarget(stage.GetSessionLayer())
    settings = controller._dataModel.viewSettings
    settings.cameraPrim = stage.GetPrimAtPath(parent + '/Camera')
    settings.showHUD = settings.showBBoxes = settings.displayGuide = False
    settings.enableSceneMaterials = settings.enableSceneLights = True
    settings.ambientLightOnly = settings.domeLightEnabled = False
    settings.complexity = RefinementComplexities.VERY_HIGH
    settings.colorCorrectionMode = 'sRGB'
    view = controller._stageView
    view.DrawAxis = lambda matrix: None
    view.SetPhysicalWindowSize(640, 640 if not parent else 360)
    app = QtWidgets.QApplication.instance()
    result = {'scene': scene.name, 'sceneLightsOnly': True, 'renderers': {}}

    def capture(name):
        start = time.monotonic()
        while True:
            view.update(); app.processEvents(); GL.glFinish()
            elapsed = time.monotonic() - start
            if elapsed > 3 and view._renderer.IsConverged(): break
            assert elapsed < 180, 'Renderer did not converge: ' + name
            time.sleep(.03)
        file = out / (name + '.png')
        assert view.grabFrameBuffer().save(str(file))
        return Image.open(file).convert('RGB')

    for label, plugin in (('storm', 'HdStormRendererPlugin'),
                          ('moonray', 'HdMoonrayRendererDebugPlugin')):
        assert view.SetRendererPlugin(plugin)
        full = capture(label)
        changes = {}
        for light in lights:
            attr = light.GetIntensityAttr()
            value = attr.Get()
            attr.Set(0.)
            muted = capture(label + '_without_' + light.GetPrim().GetName().lower())
            diff = sum(ImageStat.Stat(ImageChops.difference(full, muted)).mean) / 3
            assert diff > .05, 'Light has no visible effect: ' + str(light.GetPath())
            changes[light.GetPrim().GetName()] = diff
            attr.Set(value)
        result['renderers'][label] = {'plugin': plugin, 'converged': True,
                                     'lightRemovalMeanPixelDifferences': changes}
        print(scene.name + ': ' + label + ' studio verified', flush=True)
    (out / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
