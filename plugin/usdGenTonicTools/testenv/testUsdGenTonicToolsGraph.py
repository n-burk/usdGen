#!/usr/bin/env python3
# testUsdGenTonicToolsGraph -- T1: the Qt-free Graph-mode half.
#
# tonicGraph kernels, graph sub-modes and tool state import and behave
# headlessly (no pxr, no Qt, no usdview). Modules load by file path so the
# package __init__ (which imports pxr.Usdviewq) is never executed -- the same
# guarantee the 08-tools Qt-free rule demands of every model module.
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
    import numpy as np

    tonicGraph = _load("tonicGraph")
    tonicModes = _load("tonicModes")
    tonicToolState = _load("tonicToolState")

    # -- Douglas-Peucker ------------------------------------------------
    rect = np.array([[0.0, 0.0, 1.0],
                     [1.0, 0.0, 1.0],
                     [2.0, 0.0, 1.0],
                     [2.0, 0.0, 2.0],
                     [2.0, 0.0, 3.0],
                     [1.0, 0.0, 3.0],
                     [0.0, 0.0, 3.0],
                     [0.0, 0.0, 2.0],
                     [0.0, 0.0, 1.0]])
    keep = tonicGraph.douglasPeucker(rect, 0.05)
    kept = [tuple(p) for p in rect[keep]]
    check(list(keep).count(True) == 5 and kept[0] == (0.0, 0.0, 1.0)
          and kept[-1] == (0.0, 0.0, 1.0)
          and (2.0, 0.0, 1.0) in kept and (2.0, 0.0, 3.0) in kept
          and (0.0, 0.0, 3.0) in kept,
          "corners survive, collinear mids drop (kept %d)" % sum(keep))
    line = np.array([[float(i), 0.0, 0.0] for i in range(10)])
    keep = tonicGraph.douglasPeucker(line, 0.01)
    check(sum(keep) == 2, "a straight line keeps its ends")
    try:
        tonicGraph.douglasPeucker(np.zeros((4, 2)), 0.1)
        check(False, "non-Nx3 input is refused")
    except ValueError:
        check(True, "non-Nx3 input is refused")

    # -- snap conversion --------------------------------------------------
    rest = tonicGraph.pixelsToRest(8.0, 10.0, 1080, 40.0)
    # 8 px of a 1080-line frame at 10 units with a 40-degree fov.
    expect = 8.0 * (2.0 * 10.0 * np.tan(np.radians(40.0) * 0.5)) / 1080.0
    check(abs(rest - expect) < 1e-12, "pixels convert at the pinhole rate")
    check(tonicGraph.pixelsToRest(8.0, 10.0, 0, 40.0) == 0.0,
          "degenerate viewports convert to 0, not NaN")
    check(tonicGraph.pixelsToRest(8.0, -1.0, 1080, 40.0) == 0.0,
          "negative distances convert to 0")

    # -- HUD status ---------------------------------------------------------
    line = tonicGraph.hudStatus((6, 7, 2), (2, 8, 0), 4, 4)
    check("6 nodes" in line and "7 edges" in line and "2 regions" in line
          and "uncovered 8" in line and "intersected 0" in line
          and "behind" not in line and "baking" not in line,
          "the HUD names counts with no skew suffix when caught up")
    line = tonicGraph.hudStatus((6, 7, 2), (2, 8, 0), 5, 4)
    check("map v4 baking v5" in line, "an unswapped bake shows as baking")
    line = tonicGraph.hudStatus((6, 7, 2), (2, 8, 0), 5, 4,
                                stagedVersion=6)
    check("behind" in line, "a staged-but-unbaked map shows as behind")
    line = tonicGraph.hudStatus((6, 7, 2), (2, 8, 0), 4, 4,
                                fallbackReason="device buffer allocation failed")
    check("CPU-only" in line and "allocation" in line,
          "the P6 fallback reason banners CPU-only")

    # -- palette ------------------------------------------------------------
    c0 = tonicGraph.regionPalette(0)
    c1 = tonicGraph.regionPalette(1)
    check(c0 == tonicGraph.regionPalette(0) and c0 != c1
          and all(0.0 <= c <= 1.0 for c in c0),
          "the palette is deterministic with distinct unit colours")
    check(tonicGraph.REGION_UNCOVERED == -1, "uncovered is -1")

    # -- sub-modes + state ----------------------------------------------------
    check(len(tonicModes.GRAPH_SUBMODES) == 9
          and tonicModes.GraphSubModeById("region") is not None
          and tonicModes.GraphSubModeById("draw") is not None
          and tonicModes.GraphSubModeById("reposition") is not None
          and next(mode.hotkey for mode in tonicModes.GRAPH_SUBMODES
                   if mode.id == "reposition") == "M"
          and tonicModes.GraphSubModeById("bogus") is None,
          "nine graph sub-modes including Reposition/M resolve, unknown ids do not")
    state = tonicToolState.TonicToolState()
    check(state.graphSubMode == "region" and state.snapRadiusPx == 8.0
          and state.mirrorX is False and state.mapVersion == 0
          and state.bakedVersion == 0,
          "graph state defaults to click-created regions (mirror off)")
    status = tonicModes.SetActiveGraphSubMode(state, "link")
    check(state.graphSubMode == "link" and "Link" in status,
          "selecting a sub-mode records it with a status line")
    before = state.graphSubMode
    status = tonicModes.SetActiveGraphSubMode(state, "bogus")
    check(state.graphSubMode == before and status == "",
          "unknown sub-modes are rejected, state untouched")
    status = tonicModes.SetActiveGraphSubMode(state, "")
    check(state.graphSubMode == "" and status != "",
          "clearing the sub-mode reports it")


if __name__ == "__main__":
    main()
    print("testUsdGenTonicToolsGraph: %d failure(s)" % failures)
    sys.exit(1 if failures else 0)
