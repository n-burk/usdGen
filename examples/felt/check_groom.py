"""testusdview: verify loose felt, authored Moonray settings and sampling cost."""
def testUsdviewInputFunction(controller):
    import json
    import sys
    import time
    from pathlib import Path
    from pxr import Usd, Sdf
    from pxr.Usdviewq.qt import QtWidgets
    from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
    from OpenGL import GL
    from PIL import Image, ImageChops, ImageStat

    here = Path(controller._dataModel.stage.GetRootLayer().realPath).parent
    sys.path.insert(0, str(here))
    from configure_felt_sphere import MOONRAY
    out = here / 'groom_validation'
    out.mkdir(exist_ok=True)
    stage = controller._dataModel.stage
    stage.SetEditTarget(stage.GetSessionLayer())
    settings = controller._dataModel.viewSettings
    settings.cameraPrim = stage.GetPrimAtPath('/Camera')
    settings.showHUD = settings.showBBoxes = settings.displayGuide = False
    settings.enableSceneMaterials = settings.enableSceneLights = True
    settings.ambientLightOnly = settings.domeLightEnabled = False
    settings.complexity = RefinementComplexities.VERY_HIGH
    settings.colorCorrectionMode = 'sRGB'
    view = controller._stageView
    view.DrawAxis = lambda matrix: None
    view.SetPhysicalWindowSize(640, 640)
    app = QtWidgets.QApplication.instance()
    prim = stage.GetPrimAtPath('/Render/FeltMoonray')

    def capture(name):
        start = time.monotonic()
        while True:
            view.update(); app.processEvents(); GL.glFinish()
            elapsed = time.monotonic() - start
            if elapsed > 3 and view._renderer.IsConverged(): break
            assert elapsed < 240, 'No convergence: ' + name
            time.sleep(.025)
        assert view.grabFrameBuffer().save(str(out / (name + '.png')))
        print(name + ': %.2fs' % elapsed, flush=True)
        return elapsed

    assert view.SetRendererPlugin('HdStormRendererPlugin')
    capture('storm')
    assert view.SetRendererPlugin('HdMoonrayRendererDebugPlugin')
    tuned_time = capture('moonray')
    import ctypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetModuleHandleW.restype = ctypes.c_void_p
    kernel.GetModuleFileNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint]
    buffer = ctypes.create_unicode_buffer(4096)
    kernel.GetModuleFileNameW(kernel.GetModuleHandleW('hydramoonray.dll'), buffer, len(buffer))
    print('hydramoonray: ' + buffer.value, flush=True)
    print('Active settings: ' + str(view.GetActiveRenderSettingsPrimPath()), flush=True)
    for name, value in MOONRAY.items():
        actual = view._renderer.GetRendererSetting('moonray:sceneVariable:' + name)
        assert actual == value, (name, actual, value)
    assert view._renderer.GetRendererSetting('enableMotionBlur') is False

    # Changing/removing scene settings must recook the renderer without leaking
    # the previous scene's values. The RDL dump verifies the renderer itself.
    dump = here.parents[1] / 'build' / 'felt_settings_applied.rdla'
    view._renderer.SetRendererSetting('rdlOutput', str(dump))
    key = 'moonray:sceneVariable:target_adaptive_error'
    prim.GetAttribute(key).Set(MOONRAY['target_adaptive_error'] + .1)
    capture('moonray_edit')
    assert view._renderer.GetRendererSetting(key) > MOONRAY['target_adaptive_error']
    # Retain just SceneVariables evidence, not the large geometry dump.
    with dump.open() as file:
        header = []
        for line in file:
            header.append(line)
            if line.strip() == '}': break
    text = ''.join(header)
    assert '["sampling_mode"] = "adaptive"' in text
    assert '["max_adaptive_samples"] = %d' % MOONRAY['max_adaptive_samples'] in text
    (out / 'applied_scene_variables.rdla').write_text(text)
    dump.unlink()
    prim.GetAttribute(key).Clear()
    view._renderer.SetRendererSetting('rdlOutput', '')

    defaults = {'sampling_mode': 'uniform', 'min_adaptive_samples': 16,
                'max_adaptive_samples': 4096, 'target_adaptive_error': 10.,
                'light_samples': 2, 'bsdf_samples': 2, 'max_depth': 5,
                'max_diffuse_depth': 2, 'max_glossy_depth': 2}
    for name, value in defaults.items():
        prim.GetAttribute('moonray:sceneVariable:' + name).Set(value)
    default_time = capture('moonray_default_sampling')
    # Four times the baseline primary samples for a quality comparison.
    prim.CreateAttribute('moonray:sceneVariable:pixel_samples', Sdf.ValueTypeNames.Int).Set(16)
    reference_time = capture('moonray_reference')
    reference = Image.open(out / 'moonray_reference.png').convert('RGB')
    errors = {}
    for label in ('moonray', 'moonray_default_sampling'):
        candidate = Image.open(out / (label + '.png')).convert('RGB')
        errors[label] = sum(ImageStat.Stat(ImageChops.difference(reference, candidate)).mean) / 3

    # Remove all authored settings in the session. Host settings are exposed again.
    prim.SetActive(False)
    capture('moonray_removed_settings')
    assert view._renderer.GetRendererSetting('moonray:sceneVariable:sampling_mode') is None
    assert view._renderer.GetRendererSetting('enableMotionBlur') is True
    report = {'settings': MOONRAY, 'converged': True,
              'sceneSettingsReadback': True, 'editAndRemovalVerified': True,
              'seconds': {'tuned': tuned_time, 'default': default_time, 'reference': reference_time},
              'meanPixelErrorAgainst256sppReference': errors}
    (out / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
