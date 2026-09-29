#!/usr/bin/env python3
"""
Gap G (a), correctness half: does the Sdf.CopySpec stash round-trip EVERYTHING
a freeze authors?  Re-freeze over an existing freeze, undo, redo, and diff.
Covers: time samples, splines, nested children, relationships, metadata,
variability, custom (non-primvar) attributes, prim metadata, and the
"undo must remove a spec that did not exist" case that rigExecUndo.py:70-91
handles for attributes.
"""
import sys
from pxr import Gf, Sdf, Ts, Usd, UsdGeom, Vt

sys.path.insert(0, "<session-scratch>"
                   "887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/probes/"
                   "freeze-bake")
from probe1_freeze_undo import SubtreeSnapshot        # noqa: E402


def author_v(layer, path, tag, nchild=2, samples=True):
    with Sdf.ChangeBlock():
        s = Sdf.CreatePrimInLayer(layer, path)
        s.specifier = Sdf.SpecifierDef
        s.typeName = "BasisCurves"
        s.SetInfo("customData", {"usdGen": {"frozenEpoch": tag}})
        s.SetInfo("kind", "subcomponent")
        a = Sdf.AttributeSpec(s, "points", Sdf.ValueTypeNames.Point3fArray)
        a.default = Vt.Vec3fArray([(1.0, 2.0, 3.0)])
        pe = Sdf.AttributeSpec(s, "primvars:usdGen:frozenEpoch",
                               Sdf.ValueTypeNames.String)
        pe.default = tag
        pe.SetInfo("interpolation", "constant")
        u = Sdf.AttributeSpec(s, "uniformAttr", Sdf.ValueTypeNames.Token,
                              Sdf.VariabilityUniform)
        u.default = tag
        r = Sdf.RelationshipSpec(s, "usdGen:boundSurface")
        r.targetPathList.explicitItems.append("/Skin/" + tag)
        for i in range(nchild):
            c = Sdf.CreatePrimInLayer(layer, "%s/child%d" % (path, i))
            c.specifier = Sdf.SpecifierDef
            c.typeName = "BasisCurves"
            ca = Sdf.AttributeSpec(c, "widths", Sdf.ValueTypeNames.FloatArray)
            ca.default = Vt.FloatArray([float(i)])
    if samples:
        for t in (1.0, 2.0, 3.0):
            layer.SetTimeSample(Sdf.Path(path + ".points"), t,
                                Vt.Vec3fArray([(t, t, t)]))
        sp = Sdf.AttributeSpec(layer.GetPrimAtPath(path), "scalarAnim",
                               Sdf.ValueTypeNames.Double)
        spline = Ts.Spline()
        k = Ts.Knot(); k.SetTime(0.0); k.SetValue(0.0); spline.SetKnot(k)
        k2 = Ts.Knot(); k2.SetTime(10.0); k2.SetValue(1.0); spline.SetKnot(k2)
        sp.SetSpline(spline)


def digest(layer, path):
    """Everything about the subtree that must survive a round trip."""
    out = []
    def walk(spec, depth):
        out.append(("prim", spec.path.pathString, spec.typeName,
                    str(spec.specifier), repr(dict(spec.GetInfo("customData")))
                    if spec.HasInfo("customData") else "-",
                    spec.GetInfo("kind") if spec.HasInfo("kind") else "-"))
        for prop in spec.properties:
            if isinstance(prop, Sdf.AttributeSpec):
                out.append(("attr", prop.path.pathString, str(prop.typeName),
                            str(prop.variability),
                            repr(prop.default) if prop.HasDefaultValue() else "-",
                            repr(sorted(layer.ListTimeSamplesForPath(prop.path))),
                            repr([(t, prop.GetSpline().Eval(t)) for t in (0.0, 5.0, 10.0)]) if prop.HasSpline() else "-",
                            prop.GetInfo("interpolation")
                            if prop.HasInfo("interpolation") else "-"))
            else:
                out.append(("rel", prop.path.pathString,
                            repr(list(prop.targetPathList.explicitItems))))
        for c in spec.nameChildren:
            walk(c, depth + 1)
    root = layer.GetPrimAtPath(path)
    if root is None:
        return None
    walk(root, 0)
    return out


def main():
    stage = Usd.Stage.CreateInMemory()
    UsdGeom.Xform.Define(stage, "/Groom")
    L = stage.GetSessionLayer()
    stage.SetEditTarget(Usd.EditTarget(L))
    P = "/Groom/Frozen"

    # freeze v1
    author_v(L, P, "v1")
    d1 = digest(L, P)
    snapV1 = SubtreeSnapshot.Capture(L, P)

    # re-freeze: v2 REPLACES v1 (fewer children, different values)
    snapBefore = SubtreeSnapshot.Capture(L, P)
    SubtreeSnapshot._Remove(L, Sdf.Path(P))
    author_v(L, P, "v2", nchild=1)
    d2 = digest(L, P)
    snapAfter = SubtreeSnapshot.Capture(L, P)

    checks = []
    snapBefore.Restore()
    dU = digest(L, P)
    if dU != d1:
        import difflib
        a = [repr(x) for x in d1]
        b = [repr(x) for x in dU]
        print("--- DIFF v1(expected) vs after-undo ---")
        for line in list(difflib.unified_diff(a, b, "expected", "undone",
                                              lineterm="", n=0))[:40]:
            print(line)
        print("--- end diff ---")
    checks.append(("undo re-freeze -> v1", dU == d1))
    snapAfter.Restore()
    checks.append(("redo re-freeze -> v2", digest(L, P) == d2))
    snapBefore.Restore()
    checks.append(("undo again -> v1", digest(L, P) == d1))

    # the "did not exist" case
    absent = SubtreeSnapshot.Capture(L, "/Groom/NeverWas")
    author_v(L, "/Groom/NeverWas", "vx")
    absent.Restore()
    checks.append(("undo of a first freeze removes the prim",
                   L.GetPrimAtPath("/Groom/NeverWas") is None
                   and not stage.GetPrimAtPath("/Groom/NeverWas")))

    # composed-stage readback after undo
    prim = stage.GetPrimAtPath(P)
    checks.append(("composed stage sees v1 after undo",
                   prim and prim.GetAttribute(
                       "primvars:usdGen:frozenEpoch").Get() == "v1"))
    checks.append(("time samples survive",
                   sorted(prim.GetAttribute("points").GetTimeSamples())
                   == [1.0, 2.0, 3.0]))
    checks.append(("spline survives",
                   L.GetAttributeAtPath(P + ".scalarAnim").HasSpline()))
    checks.append(("children restored",
                   len(L.GetPrimAtPath(P).nameChildren) == 2))
    checks.append(("relationship restored",
                   list(L.GetPrimAtPath(P).relationships["usdGen:boundSurface"]
                        .targetPathList.explicitItems) == [Sdf.Path("/Skin/v1")]))
    checks.append(("prim metadata (kind) restored",
                   L.GetPrimAtPath(P).GetInfo("kind") == "subcomponent"))
    checks.append(("uniform variability restored",
                   L.GetAttributeAtPath(P + ".uniformAttr").variability
                   == Sdf.VariabilityUniform))

    bad = 0
    for name, ok in checks:
        print("%-42s %s" % (name, "PASS" if ok else "FAIL"))
        bad += 0 if ok else 1
    print("\n%d/%d pass" % (len(checks) - bad, len(checks)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
