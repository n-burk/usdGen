"""testusdview capture for any motion example; images go to build/traces."""
def testUsdviewInputFunction(controller):
    from pathlib import Path
    from pxr.Usdviewq.qt import QtWidgets
    from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
    from OpenGL import GL
    stage=controller._dataModel.stage
    scene=Path(stage.GetRootLayer().realPath)
    out=scene.parents[2]/'build'/'traces'/scene.stem/'frames'
    out.mkdir(parents=True,exist_ok=True)
    view=controller._stageView
    settings=controller._dataModel.viewSettings
    settings.complexity=RefinementComplexities.VERY_HIGH
    settings.enableSceneMaterials=True;settings.enableSceneLights=True
    settings.ambientLightOnly=False;settings.domeLightEnabled=False
    settings.showHUD=False;settings.showBBoxes=False
    settings.displayGuide=False
    settings.cameraPrim=controller._dataModel.stage.GetPrimAtPath('/World/Camera')
    view.DrawAxis=lambda matrix:None
    view.SetPhysicalWindowSize(960,540)
    assert view.SetRendererPlugin('HdStormRendererPlugin')
    app=QtWidgets.QApplication.instance()
    for frame in range(1,101):
        controller.setFrame(frame)
        for _ in range(3):
            view.repaint();app.processEvents();GL.glFinish()
        assert view.grabFrameBuffer().save(str(out/('%03d.png'%frame)))
    print('Captured all 100 frames',flush=True)
