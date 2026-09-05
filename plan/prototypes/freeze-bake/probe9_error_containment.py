#!/usr/bin/env python3
"""
Gap G (a): if the OpenExec resync error fires on a real removal, is the removal
still correct, and can the tool contain the diagnostic?
"""
import os, sys, time
import rigexec_test_env
rigexec_test_env.SetupPluginTest()
from pxr import Sdf, Tf, Usd, UsdGeom     # noqa: E402

RIG = os.environ["RIG"]
EXAMPLE = os.path.join(RIG, "examples", "ArmShotAnim.usda")

if len(sys.argv) > 1:
    from pxr import Plug
    Plug.Registry().RegisterPlugins(sys.argv[1])

import _rigexec                            # noqa: E402

stage = Usd.Stage.Open(EXAMPLE)
rig = _rigexec.Rig(stage, "/Shot/HeroArm/Rig")
rig.compile()
rig.evaluate(1001.0)
stage.SetEditTarget(Usd.EditTarget(stage.GetSessionLayer()))
L = stage.GetSessionLayer()

print("Tf has ErrorMark:", hasattr(Tf, "ErrorMark"))

# 1. does the removal complete despite the raise?
c = Sdf.CreatePrimInLayer(L, "/Shot/Frozen")
c.specifier = Sdf.SpecifierDef
c.typeName = "BasisCurves"
raised = None
try:
    stage.RemovePrim("/Shot/Frozen")
except Tf.ErrorException as e:
    raised = str(e).strip().splitlines()[-1][:80]
print("removal raised:", bool(raised))
print("spec gone from layer:", L.GetPrimAtPath("/Shot/Frozen") is None)
print("prim gone from stage:", not stage.GetPrimAtPath("/Shot/Frozen"))
rig.evaluate(1002.0)
print("rig still evaluates after the raise: True")

# 2. is a SECOND, unrelated USD call after the raise clean?
try:
    stage.DefinePrim("/Shot/AfterRaise", "Xform")
    print("next USD call clean: True")
except Tf.ErrorException as e:
    print("next USD call clean: False ->", str(e).splitlines()[-1][:80])

# 3. two removals in one Sdf.ChangeBlock -> how many raises?
for i in range(3):
    s = Sdf.CreatePrimInLayer(L, "/Shot/F%d" % i)
    s.specifier = Sdf.SpecifierDef
n = 0
try:
    with Sdf.ChangeBlock():
        for i in range(3):
            p = L.GetPrimAtPath("/Shot")
            del p.nameChildren["F%d" % i]
except Tf.ErrorException as e:
    n = str(e).count("Applying predicate to invalid prim")
print("removals batched in one ChangeBlock -> errors in one exception:", n)
print("all three gone:", all(L.GetPrimAtPath("/Shot/F%d" % i) is None
                             for i in range(3)))

# 4. deactivate-then-later-purge: cost of the purge on a big buffer
from pxr import Vt
pts = Vt.Vec3fArray([(0.0, 0.0, 0.0)] * 800000)
big = Sdf.CreatePrimInLayer(L, "/Shot/Big")
big.specifier = Sdf.SpecifierDef
big.typeName = "BasisCurves"
a = Sdf.AttributeSpec(big, "points", Sdf.ValueTypeNames.Point3fArray)
a.default = pts
t0 = time.perf_counter()
stage.GetPrimAtPath("/Shot/Big").SetActive(False)
t1 = time.perf_counter()
print("SetActive(False) on 800k-point prim: %.2f ms (no raise)" % ((t1-t0)*1e3))
t0 = time.perf_counter()
try:
    p = L.GetPrimAtPath("/Shot")
    del p.nameChildren["Big"]
except Tf.ErrorException:
    pass
t1 = time.perf_counter()
print("later purge (spec removal): %.2f ms" % ((t1-t0)*1e3))
del pts
