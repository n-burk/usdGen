#
# Gap G part (d): what does ONE freeze cost usdview?
#
# Authors a 100k-curve BasisCurves into the session layer of a live usdview
# and times, separately:
#   * the Sdf authoring,
#   * the Usd.Notice.ObjectsChanged -> _updateForStageChanges -> updateGUI
#     coalescing hop,
#   * each of the five panel refreshes _resetGUI() drives
#     (appController.py:1956-1981),
#   * the Storm redraw (llvmpipe here - NOT a GPU number).
#
import time
from pxr import Sdf, Usd, UsdGeom, Vt

import os
N = int(os.environ.get("FREEZE_N", "100000"))
CVS = 8


def _t():
    return time.perf_counter()


def _build(n, cvs):
    pts = Vt.Vec3fArray([((i // cvs % 100) * 0.01, (i % cvs) * 0.02,
                          (i // cvs // 100) * 0.01) for i in range(n * cvs)])
    return pts, Vt.IntArray([cvs] * n), Vt.FloatArray([0.002] * (n * cvs))


def _authorFreeze(layer, path, pts, counts, widths):
    with Sdf.ChangeBlock():
        s = Sdf.CreatePrimInLayer(layer, path)
        s.specifier = Sdf.SpecifierDef
        s.typeName = "BasisCurves"
        def a(name, tn, v, interp=None):
            sp = Sdf.AttributeSpec(s, name, tn)
            sp.default = v
            if interp:
                sp.SetInfo("interpolation", interp)
        a("points", Sdf.ValueTypeNames.Point3fArray, pts)
        a("curveVertexCounts", Sdf.ValueTypeNames.IntArray, counts)
        a("widths", Sdf.ValueTypeNames.FloatArray, widths, "vertex")
        a("primvars:rest", Sdf.ValueTypeNames.Point3fArray, pts, "vertex")
        a("type", Sdf.ValueTypeNames.Token, "cubic")
        a("basis", Sdf.ValueTypeNames.Token, "bspline")
        a("wrap", Sdf.ValueTypeNames.Token, "pinned")


def testUsdviewInputFunction(appController):
    from pxr.Usdviewq.qt import QtWidgets
    app = QtWidgets.QApplication.instance()
    stage = appController._dataModel.stage
    session = stage.GetSessionLayer()
    stage.SetEditTarget(Usd.EditTarget(session))
    nPrims = len(list(stage.TraverseAll()))

    pts, counts, widths = _build(N, CVS)
    print("FREEZECOST stagePrims=%d curves=%d cvs=%d" % (nPrims, N, CVS))

    # --- 1. author + let usdview's own coalesced path run ----------------
    t0 = _t()
    _authorFreeze(session, "/Groom/Frozen", pts, counts, widths)
    t1 = _t()
    # _updateForStageChanges has already run synchronously in the notice
    # handler; processEvents fires the 0 ms _guiResetTimer -> _resetGUI AND
    # the Qt repaint (Storm on llvmpipe here).
    app.processEvents()
    t2 = _t()
    print("FREEZECOST author_ms=%.2f noticeToGuiResetPlusPaint_ms=%.2f" %
          ((t1 - t0) * 1e3, (t2 - t1) * 1e3))
    # isolate _resetGUI itself (no paint) by calling it directly
    appController._hasPrimResync = True
    t3 = _t()
    appController._resetGUI()
    t4 = _t()
    print("FREEZECOST resetGUI_only_ms=%.2f" % ((t4 - t3) * 1e3))

    # --- 2. break _resetGUI into its five panel refreshes ----------------
    parts = []
    for label, fn in (
            ("_resetPrimView", lambda: appController._resetPrimView()),
            ("_resetPrimViewVis",
             lambda: appController._resetPrimViewVis(selItemsOnly=False)),
            ("_updatePropertyView",
             lambda: appController._updatePropertyView()),
            ("_populatePropertyInspector",
             lambda: appController._populatePropertyInspector()),
            ("_updateMetadataView", lambda: appController._updateMetadataView()),
            ("_updateLayerStackView",
             lambda: appController._updateLayerStackView()),
            ("_updateCompositionView",
             lambda: appController._updateCompositionView()),
    ):
        t = _t()
        fn()
        parts.append((label, (_t() - t) * 1e3))
    for label, ms in parts:
        print("FREEZECOST part %-28s %8.2f ms" % (label, ms))

    # --- 3. select the frozen prim, then re-time the property panels -----
    appController._dataModel.selection.setPrim(
        stage.GetPrimAtPath("/Groom/Frozen"))
    app.processEvents()
    t = _t()
    appController._updatePropertyView()
    appController._populatePropertyInspector()
    tSel = (_t() - t) * 1e3
    print("FREEZECOST propertyPanelsWithFrozenSelected_ms=%.2f" % tSel)

    # --- 4. redraw (llvmpipe; NOT a GPU number) --------------------------
    sv = appController._stageView
    t = _t()
    sv.updateView(resetCam=False, forceComputeBBox=False)
    app.processEvents()
    print("FREEZECOST llvmpipeRedraw_ms=%.2f" % ((_t() - t) * 1e3))

    # --- 5. UNDO by deactivate vs by remove ------------------------------
    prim = stage.GetPrimAtPath("/Groom/Frozen")
    t = _t()
    prim.SetActive(False)
    app.processEvents()
    print("FREEZECOST undoByDeactivate_ms=%.2f" % ((_t() - t) * 1e3))
    t = _t()
    prim.SetActive(True)
    app.processEvents()
    print("FREEZECOST redoByActivate_ms=%.2f" % ((_t() - t) * 1e3))
    t = _t()
    stage.RemovePrim("/Groom/Frozen")
    app.processEvents()
    print("FREEZECOST undoByRemovePrim_ms=%.2f" % ((_t() - t) * 1e3))

    print("FREEZECOST_OK")
