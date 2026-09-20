#!/usr/bin/env python3
# testUsdGenTonicToolsTube -- T1: the Qt-free Tube/Fill half of tools.
#
# Tube/fill helpers and sub-modes import and behave headlessly (no pxr, no
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
        import numpy as np  # noqa: F401
    except ImportError:
        print("SKIP: numpy is not installed")
        return 77

    modes = _load("tonicModes")
    stateModule = _load("tonicToolState")
    tube = _load("tonicTube")
    fill = _load("tonicFill")

    # -- pick kinds mirror TonicPickKind -----------------------------------
    check(tube.PICK_TUBE_VERT == 1, "pick tube-vert bit")
    check(tube.PICK_CENTER_CV == 2, "pick center-CV bit")
    check(tube.PICK_SECTION_CV == 4, "pick section-CV bit")
    check(tube.PICK_GRAPH_NODE == 8, "pick graph-node bit")
    check(tube.PICK_GUIDE == 16, "pick guide bit")
    check(tube.PICK_GRAPH_EDGE == 32, "pick graph-edge bit")
    check(tube.PICK_REGION == 64, "pick region bit")
    check(tube.PICK_SECTION_RING == 128, "pick section-ring bit")
    check(tube.PICK_LEVEL == 256, "pick hierarchy-level bit")
    # Eight screen-pickable kinds; a hierarchy level is a selection kind
    # only, so it is in SELECT_ALL and not in PICK_ALL (plan/17 4.6).
    check(tube.PICK_ALL == 0xFF, "pick all mask")
    check(tube.SELECT_ALL == 0x1FF, "select all mask")
    check(len(tube.PICK_KIND_NAMES) == 9, "all nine kinds are named")

    # -- soft selection -----------------------------------------------------
    w = tube.softWeights(5, 2, 0.0)
    check(list(w) == [0.0, 0.0, 1.0, 0.0, 0.0],
          "zero radius selects the dragged CV exactly")
    w = tube.softWeights(5, 2, 0.5)
    check(abs(w[2] - 1.0) < 1e-12 and w[0] == 0.0 and w[4] == 0.0,
          "radius 0.5 peaks at center, zero at the ends")
    check(bool(np.all(w[1:3] >= w[0:2])) and abs(w[1] - w[3]) < 1e-12,
          "weights are symmetric and rise toward the center")
    try:
        tube.softWeights(5, 5, 0.0)
        check(False, "out-of-range CV raises")
    except ValueError:
        check(True, "out-of-range CV raises")

    # -- ring gizmo preview --------------------------------------------------
    ring = np.array([[1.0, 0.0], [0.0, 1.0], [-1.0, 0.0], [0.0, -1.0]])
    out = tube.transformRingPreview(ring, scale=2.0)
    check(bool(np.allclose(out, ring * 2.0)), "ring scale doubles the ring")
    out = tube.transformRingPreview(ring, twist=np.pi / 2.0)
    check(bool(np.allclose(out[:, 0], -ring[:, 1])) and
          bool(np.allclose(out[:, 1], ring[:, 0])),
          "ring twist rotates by pi/2")
    out = tube.transformRingPreview(ring, offset=(1.0, 2.0))
    check(bool(np.allclose(out, ring + [1.0, 2.0])), "ring offset translates")
    try:
        tube.transformRingPreview(ring, scale=0.0)
        check(False, "non-positive scale raises")
    except ValueError:
        check(True, "non-positive scale raises")

    # -- relax preview --------------------------------------------------------
    centers = np.array([[0.0, 0.0, 0.0], [1.0, 2.0, 0.0], [2.0, 0.0, 0.0]])
    out = tube.relaxPreview(centers, 1.0)
    check(bool(np.allclose(out[0], centers[0])) and
          bool(np.allclose(out[2], centers[2])) and
          bool(np.allclose(out[1], [1.0, 0.0, 0.0])),
          "full relax pulls the kink onto the chord, ends pinned")
    out = tube.relaxPreview(centers, 0.0)
    check(bool(np.allclose(out, centers)), "zero relax is identity")

    # -- length scale ----------------------------------------------------------
    check(abs(tube.lengthScale(centers, 4.0) - 4.0 /
              (2.0 * np.sqrt(5.0))) < 1e-12,
          "length scale matches the arc length ratio")
    check(tube.lengthScale(centers, 0.0) == 0.0,
          "non-positive length returns 0")

    # -- section insert span ----------------------------------------------------
    k, f = tube.sectionInsertSpan([0.0, 0.5, 1.0], 0.75)
    check(k == 1 and abs(f - 0.5) < 1e-12, "insert span brackets t")
    for bad in (0.0, 1.0, -0.1, 1.1):
        try:
            tube.sectionInsertSpan([0.0, 1.0], bad)
            check(False, "t=%r outside the open range raises" % (bad,))
        except ValueError:
            check(True, "t=%r outside the open range raises" % (bad,))

    # -- tube/fill status --------------------------------------------------------
    check("5 center CVs" in tube.tubeStatus(5, 5, 8, -1) and
          "unrooted" in tube.tubeStatus(5, 5, 8, -1),
          "tube status names counts + unrooted")
    check("region 3" in tube.tubeStatus(4, 3, 8, 3),
          "tube status names the root region")
    check(tube.pickStatus(4, 2, 5, 3.2) ==
          "Pick: section CV ring 2 slot 5 (3.2 px).",
          "pick status formats a section hit")
    check(tube.pickStatus(16, 7, 0, 0.4) ==
          "Pick: guide guide 7 cv 0 (0.4 px).",
          "pick status formats a guide hit")

    # -- fill counts ---------------------------------------------------------------
    check(fill.previewCount(100, 0.25) == 25, "preview is 25%")
    check(fill.previewCount(16, 1.0) == 16, "release is full")
    check(fill.previewCount(10, 0.0) == 0, "zero fraction draws nothing")
    check(fill.frozenPrefix(10, 16) == 10, "freeze keeps the stored prefix")
    check(fill.frozenPrefix(20, 16) == 16, "freeze clamps to the target")
    check(fill.refillPlan(100, 0.25, 0, False) == (25, 0),
          "drag plan refills at preview density")
    check(fill.refillPlan(100, 1.0, 25, True) == (100, 25),
          "release plan keeps the frozen prefix")

    # -- length ramp -----------------------------------------------------------------
    check(fill.evalLengthProfile([], 0.7) == 1.0, "empty ramp is uniform")
    check(abs(fill.evalLengthProfile([0.0, 1.0, 1.0, 0.5], 0.5) - 0.75)
          < 1e-12, "ramp interpolates linearly")
    check(fill.evalLengthProfile([0.0, 1.0, 1.0, 0.5], 2.0) == 0.5,
          "ramp clamps past the end")
    try:
        fill.validateLengthProfile([0.0, 1.0, 0.5])
        check(False, "odd profile floats raise")
    except ValueError:
        check(True, "odd profile floats raise")

    # -- edge bias ----------------------------------------------------------------------
    check(fill.edgeBiasRemap(0.25, 0.0) == 0.25, "zero bias is identity")
    check(fill.edgeBiasRemap(0.25, 1.0) > 0.25, "+bias pushes outward")
    check(fill.edgeBiasRemap(0.25, -1.0) < 0.25, "-bias pulls inward")
    try:
        fill.edgeBiasRemap(0.5, 2.0)
        check(False, "bias outside [-1, 1] raises")
    except ValueError:
        check(True, "bias outside [-1, 1] raises")

    # -- sub-modes + state -----------------------------------------------------------------
    check([m.id for m in modes.TUBE_SUBMODES] ==
          ["center", "ring", "section"], "tube sub-mode ids")
    check([m.id for m in modes.FILL_SUBMODES] == ["params", "preview"],
          "fill sub-mode ids")
    state = stateModule.TonicToolState()
    line = modes.SetActiveTubeSubMode(state, "ring")
    check(state.tubeSubMode == "ring" and "Ring" in line,
          "tube sub-mode records + reports")
    check(modes.SetActiveTubeSubMode(state, "") ==
          "Tonic Tube: no sub-mode.", "tube sub-mode clears")
    check(modes.SetActiveTubeSubMode(state, "nope") == "",
          "unknown tube sub-mode is ignored")
    line = modes.SetActiveFillSubMode(state, "params")
    check(state.fillSubMode == "params" and "Params" in line,
          "fill sub-mode records + reports")
    check(state.previewFraction == 0.25 and state.freezeRoots is False,
          "fill state defaults to 25% preview, unfrozen")

    check("density 16.0" in fill.fillStatus(16, 8, 0.5, 3, [], False),
          "fill status names density")
    check("frozen" in fill.fillStatus(16, 8, 0.0, 0, [0, 1, 1, 1], True),
          "fill status names freeze + knots")

    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
