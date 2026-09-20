#!/usr/bin/env python3
# testUsdGenTonicToolsMath -- T1: the Qt-free half of usdGenTonicTools.
#
# Modes, tool state and math kernels import and behave headlessly (no pxr, no
# Qt, no usdview). Modules load by file path so the package __init__ (which
# imports pxr.Usdviewq) is never executed -- the same guarantee the 08-tools
# Qt-free rule demands of every model module.
import importlib.util
import os
import sys

SRC = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "python", "usdGenTonicTools"))


def _load(name):
    spec = importlib.util.spec_from_file_location(
        "tonic_%s" % name, os.path.join(SRC, "%s.py" % name))
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves annotations through sys.modules[__module__].
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


failures = 0


def check(ok, what):
    global failures
    if ok:
        print("ok:   %s" % what)
    else:
        failures += 1
        print("FAIL: %s" % what)


def main():
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("SKIP: numpy is not installed")
        return 77

    modes = _load("tonicModes")
    stateModule = _load("tonicToolState")
    math = _load("tonicMath")

    check([m.id for m in modes.MODES] ==
          ["graph", "tube", "fill", "hierarchy", "sculpt", "output"],
          "shelf carries the six plan section 5 modes in order")
    check(modes.ModeById("tube").label == "Tube",
          "ModeById resolves tube")
    check(modes.ModeById("nope") is None,
          "ModeById returns None for unknown ids")

    state = stateModule.TonicToolState()
    check(state.activeMode == "" and state.activated is False
          and state.stageCacheId == -1 and state.undoStack is None
          and state.panels == {},
          "TonicToolState defaults follow the UsdGenToolState shape")
    status = modes.SetActiveMode(state, "tube")
    check(state.activeMode == "tube" and "Tube mode" in status,
          "SetActiveMode records tube with a status line")
    before = state.activeMode
    check(modes.SetActiveMode(state, "nope") == ""
          and state.activeMode == before,
          "SetActiveMode rejects unknown ids without touching state")
    check(modes.SetActiveMode(state, "") == "Tonic: no mode."
          and state.activeMode == "",
          "SetActiveMode('') clears the shelf")

    np = sys.modules["numpy"]
    check(float(math.smoothstep(0.0, 1.0, -1.0)) == 0.0
          and float(math.smoothstep(0.0, 1.0, 2.0)) == 1.0
          and abs(float(math.smoothstep(0.0, 1.0, 0.5)) - 0.5) < 1e-12,
          "smoothstep clamps and centers")
    check(float(math.smoothstep(1.0, 1.0, 0.0)) == 0.0
          and float(math.smoothstep(1.0, 1.0, 1.0)) == 1.0,
          "smoothstep survives a degenerate edge pair")
    check(float(math.softSelection(0.5, 0.5, 0.25)) == 1.0
          and float(math.softSelection(1.0, 0.5, 0.25)) == 0.0
          and float(math.softSelection(0.5, 0.5, 0.0)) == 1.0
          and float(math.softSelection(0.6, 0.5, 0.0)) == 0.0,
          "softSelection peaks, falls off, and handles zero radius")
    ring = math.lerpRing(np.zeros((8, 2)), np.ones((8, 2)), 0.25)
    check(ring.shape == (8, 2) and abs(float(ring[0, 0]) - 0.25) < 1e-12,
          "lerpRing interpolates section rings")
    try:
        math.lerpRing(np.zeros((8, 2)), np.zeros((4, 2)), 0.5)
        check(False, "lerpRing rejects mismatched rings")
    except ValueError:
        check(True, "lerpRing rejects mismatched rings")

    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
