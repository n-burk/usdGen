#!/usr/bin/env python3
"""
Gap G (a), Python half: on a *rigged* stage (usdRig evaluator compiled and
attached), which freeze/undo shapes raise Tf.ErrorException in Python?

Mirrors probe2_undo_resync_exec.cpp but through the real usdRig binding, so we
can say whether the fault is RigExec holding a UsdPrimRange or stock OpenExec.
"""
import os, sys, time, traceback
import rigexec_test_env                      # noqa: F401  (sets up plugins)
rigexec_test_env.SetupPluginTest()
from pxr import Plug, Sdf, Tf, Usd, UsdGeom   # noqa: E402

RIG = os.environ["RIG"]
EXAMPLE = os.path.join(RIG, "examples", "ArmShotAnim.usda")


def fresh(rigged):
    stage = Usd.Stage.Open(EXAMPLE)
    rig = None
    if rigged:
        import _rigexec
        rig = _rigexec.Rig(stage, "/Shot/HeroArm/Rig")
        rig.compile()
        rig.evaluate(1001.0)
    stage.SetEditTarget(Usd.EditTarget(stage.GetSessionLayer()))
    return stage, rig


def author_subtree(stage, root="/Shot/Frozen"):
    layer = stage.GetSessionLayer()
    with Sdf.ChangeBlock():
        s = Sdf.CreatePrimInLayer(layer, root)
        s.specifier = Sdf.SpecifierDef
        s.typeName = "BasisCurves"
        for name in ("A", "B"):
            c = Sdf.CreatePrimInLayer(layer, root + "/" + name)
            c.specifier = Sdf.SpecifierDef
            c.typeName = "BasisCurves"
    return layer


def sdf_remove(layer, path):
    path = Sdf.Path(path)
    parent = layer.GetPrimAtPath(path.GetParentPath())
    if parent is None:
        del layer.rootPrims[path.name]
    else:
        del parent.nameChildren[path.name]


CASES = {}


def case(name):
    def deco(fn):
        CASES[name] = fn
        return fn
    return deco


@case("A stage.RemovePrim(subtree root)")
def _a(stage, rig):
    author_subtree(stage)
    stage.RemovePrim("/Shot/Frozen")


@case("B Sdf del nameChildren")
def _b(stage, rig):
    layer = author_subtree(stage)
    sdf_remove(layer, "/Shot/Frozen")


@case("C Sdf del inside ChangeBlock")
def _c(stage, rig):
    layer = author_subtree(stage)
    with Sdf.ChangeBlock():
        sdf_remove(layer, "/Shot/Frozen")


@case("D SetActive(False)")
def _d(stage, rig):
    author_subtree(stage)
    stage.GetPrimAtPath("/Shot/Frozen").SetActive(False)


@case("E remove+recopy in one ChangeBlock (restore)")
def _e(stage, rig):
    layer = author_subtree(stage)
    stash = Sdf.Layer.CreateAnonymous("stash")
    Sdf.CreatePrimInLayer(stash, "/Shot")
    Sdf.CopySpec(layer, "/Shot/Frozen", stash, "/Shot/Frozen")
    with Sdf.ChangeBlock():
        sdf_remove(layer, "/Shot/Frozen")
        Sdf.CopySpec(stash, "/Shot/Frozen", layer, "/Shot/Frozen")


@case("F CopySpec over existing (redo onto live spec)")
def _f(stage, rig):
    layer = author_subtree(stage)
    stash = Sdf.Layer.CreateAnonymous("stash")
    Sdf.CreatePrimInLayer(stash, "/Shot")
    Sdf.CopySpec(layer, "/Shot/Frozen", stash, "/Shot/Frozen")
    Sdf.CopySpec(stash, "/Shot/Frozen", layer, "/Shot/Frozen")


@case("G author freeze (add only)")
def _g(stage, rig):
    author_subtree(stage)


def run(name, fn, rigged):
    stage, rig = fresh(rigged)
    err = None
    t0 = time.perf_counter()
    try:
        fn(stage, rig)
    except Tf.ErrorException as e:
        err = str(e).strip().splitlines()[-1].strip()
    ms = (time.perf_counter() - t0) * 1000.0
    post = ""
    if rig is not None:
        try:
            rig.evaluate(1002.0)
            post = "exec-ok"
        except Exception as e:                       # noqa: BLE001
            post = "exec-BROKEN:%s" % type(e).__name__
    return err, ms, post


def main():
    print("%-46s %-9s %-9s %-8s %s" % ("case", "plain", "rigged", "ms", "after"))
    for name, fn in CASES.items():
        e0, ms0, _ = run(name, fn, False)
        e1, ms1, post = run(name, fn, True)
        print("%-46s %-9s %-9s %8.2f %s" % (
            name, "raise" if e0 else "ok", "RAISE" if e1 else "ok", ms1, post))
        if e1:
            print("      -> %s" % e1[:110])
    return 0


if __name__ == "__main__":
    sys.exit(main())
