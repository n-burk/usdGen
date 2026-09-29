#!/usr/bin/env python3
"""
Gap G freeze/bake, part (a): whole-subtree undo for a freeze.

Extends the shape of usdRig's attribute-only EditRecorder
(<usdrig-src>/plugin/rigExecUsdview/rigExecUndo.py:203-243)
to a whole prim subtree, using Sdf.CopySpec into an anonymous stash layer
plus nameChildren removal.  Measures author / stash / undo / redo for a
10k-curve BasisCurves freeze, and compares against the "freeze lives in
its own sublayer, undo = drop the sublayer" strategy.
"""
import gc, os, resource, sys, time
from pxr import Gf, Sdf, Usd, UsdGeom, Vt

N_CURVES = int(os.environ.get("N_CURVES", "10000"))
CVS = int(os.environ.get("CVS", "8"))


def rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0


class T(object):
    def __init__(self, label, out):
        self.label, self.out = label, out
    def __enter__(self):
        gc.collect(); self.t = time.perf_counter(); return self
    def __exit__(self, *a):
        self.out.append((self.label, (time.perf_counter() - self.t) * 1000.0))


def make_curve_arrays(n, cvs):
    """Deterministic curve buffer: points, widths, ids, root bindings."""
    pts = []
    for i in range(n):
        x = (i % 100) * 0.01
        z = (i // 100) * 0.01
        for c in range(cvs):
            pts.append((x, c * 0.02, z))
    return (Vt.Vec3fArray(pts),
            Vt.IntArray([cvs] * n),
            Vt.FloatArray([0.002] * (n * cvs)),
            Vt.IntArray(list(range(n))),
            Vt.IntArray([i % 977 for i in range(n)]),          # skinprim
            Vt.Vec2fArray([((i % 31) / 31.0, (i % 17) / 17.0)
                           for i in range(n)]))                 # skinprimuv


def author_frozen(layer, path, arrays, withRest=True):
    pts, counts, widths, ids, skinprim, skinuv = arrays
    with Sdf.ChangeBlock():
        spec = Sdf.CreatePrimInLayer(layer, path)
        spec.specifier = Sdf.SpecifierDef
        spec.typeName = "BasisCurves"
        def attr(name, tn, val, interp=None):
            a = Sdf.AttributeSpec(spec, name, tn)
            a.default = val
            if interp:
                a.SetInfo("interpolation", interp)
            return a
        attr("points", Sdf.ValueTypeNames.Point3fArray, pts)
        attr("curveVertexCounts", Sdf.ValueTypeNames.IntArray, counts)
        attr("widths", Sdf.ValueTypeNames.FloatArray, widths)
        attr("type", Sdf.ValueTypeNames.Token, "cubic")
        attr("basis", Sdf.ValueTypeNames.Token, "bspline")
        attr("wrap", Sdf.ValueTypeNames.Token, "pinned")
        attr("primvars:usdGen:curveId", Sdf.ValueTypeNames.IntArray, ids,
             "uniform")
        attr("primvars:skinprim", Sdf.ValueTypeNames.IntArray, skinprim,
             "uniform")
        attr("primvars:skinprimuv", Sdf.ValueTypeNames.TexCoord2fArray, skinuv,
             "uniform")
        if withRest:
            attr("primvars:rest", Sdf.ValueTypeNames.Point3fArray, pts,
                 "vertex")
        spec.SetInfo("customData", {"usdGen:frozenEpoch": "sha1:deadbeef",
                                    "usdGen:frozenFrom": "/Groom/Gen1"})
    return spec


class SubtreeSnapshot(object):
    """The authored state of one prim subtree in one layer, stashed."""
    def __init__(self, layer, path):
        self.layer = layer
        self.path = Sdf.Path(path)
        self.exists = False
        self.stash = None          # anonymous Sdf.Layer holding the copy

    @classmethod
    def Capture(cls, layer, path):
        snap = cls(layer, path)
        if layer.GetPrimAtPath(snap.path) is None:
            return snap
        snap.exists = True
        snap.stash = Sdf.Layer.CreateAnonymous("undoStash")
        Sdf.CreatePrimInLayer(snap.stash, snap.path.GetParentPath())
        Sdf.CopySpec(layer, snap.path, snap.stash, snap.path)
        return snap

    @staticmethod
    def _Remove(layer, path):
        spec = layer.GetPrimAtPath(path)
        if spec is None:
            return
        parent = layer.GetPrimAtPath(path.GetParentPath())
        if parent is None or path.GetParentPath() == Sdf.Path.absoluteRootPath:
            del layer.rootPrims[path.name]
        else:
            del parent.nameChildren[path.name]

    def Restore(self):
        with Sdf.ChangeBlock():
            self._Remove(self.layer, self.path)
            if self.exists:
                Sdf.CreatePrimInLayer(self.layer, self.path.GetParentPath())
                Sdf.CopySpec(self.stash, self.path, self.layer, self.path)


def main():
    out = []
    print("N_CURVES=%d CVS=%d  (%d CVs total)" % (N_CURVES, CVS, N_CURVES*CVS))
    print("rss at start: %.1f MB" % rss_mb())

    with T("build arrays", out):
        arrays = make_curve_arrays(N_CURVES, CVS)

    stage = Usd.Stage.CreateInMemory()
    UsdGeom.Xform.Define(stage, "/Groom")
    session = stage.GetSessionLayer()
    stage.SetEditTarget(Usd.EditTarget(session))
    path = Sdf.Path("/Groom/Frozen")

    # --- record BEFORE (the freeze target does not exist yet) -------------
    with T("Capture before (absent)", out):
        before = SubtreeSnapshot.Capture(session, path)

    with T("author freeze into session layer", out):
        author_frozen(session, path, arrays)
    print("rss after author: %.1f MB" % rss_mb())

    with T("stage traversal sees frozen prim", out):
        p = stage.GetPrimAtPath(path)
        assert p and p.IsA(UsdGeom.BasisCurves), "frozen prim composed"
        n = len(UsdGeom.BasisCurves(p).GetCurveVertexCountsAttr().Get())
        assert n == N_CURVES, n

    with T("Capture after (10k curves)", out):
        after = SubtreeSnapshot.Capture(session, path)
    print("rss after stash: %.1f MB" % rss_mb())

    with T("UNDO  (remove subtree)", out):
        before.Restore()
    assert not stage.GetPrimAtPath(path), "undo removed the prim"

    with T("REDO  (copy stash back)", out):
        after.Restore()
    p = stage.GetPrimAtPath(path)
    assert p, "redo restored the prim"
    assert len(UsdGeom.BasisCurves(p).GetPointsAttr().Get()) == N_CURVES*CVS
    assert p.GetAttribute("primvars:skinprimuv").Get() is not None
    cd = p.GetCustomData()
    assert cd.get("usdGen:frozenEpoch") == "sha1:deadbeef", \
        "customData survives CopySpec: %r" % (cd,)
    # NOTE: GetCustomDataByKey() splits on ':' as a nested-dict key path, so a
    # flat "usdGen:frozenEpoch" customData key is NOT reachable by that call.
    print("GetCustomDataByKey('usdGen:frozenEpoch') ->",
          repr(p.GetCustomDataByKey("usdGen:frozenEpoch")))

    with T("UNDO again", out):
        before.Restore()
    with T("REDO again", out):
        after.Restore()

    # --- alternative: freeze in its own sublayer -------------------------
    stage2 = Usd.Stage.CreateInMemory()
    UsdGeom.Xform.Define(stage2, "/Groom")
    with T("[sublayer] create+author freeze layer", out):
        bake = Sdf.Layer.CreateAnonymous("bake.usdc")
        author_frozen(bake, path, arrays)
    with T("[sublayer] insert sublayer (=REDO)", out):
        stage2.GetSessionLayer().subLayerPaths.append(bake.identifier)
    assert stage2.GetPrimAtPath(path), "sublayer freeze composes"
    with T("[sublayer] remove sublayer (=UNDO)", out):
        del stage2.GetSessionLayer().subLayerPaths[-1]
    assert not stage2.GetPrimAtPath(path)
    with T("[sublayer] re-insert (=REDO)", out):
        stage2.GetSessionLayer().subLayerPaths.append(bake.identifier)
    with T("[sublayer] mute layer (=UNDO by mute)", out):
        stage2.MuteLayer(bake.identifier)
    assert not stage2.GetPrimAtPath(path), "muted"
    with T("[sublayer] unmute (=REDO by mute)", out):
        stage2.UnmuteLayer(bake.identifier)
    assert stage2.GetPrimAtPath(path)

    print()
    print("%-42s %10s" % ("step", "ms"))
    for label, ms in out:
        print("%-42s %10.2f" % (label, ms))
    print("rss peak: %.1f MB" % rss_mb())
    return 0


if __name__ == "__main__":
    sys.exit(main())
