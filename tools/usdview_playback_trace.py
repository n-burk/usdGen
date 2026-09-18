# testusdview script: steps usdview through a scene's frames, repainting the
# viewport after each, and writes a per-frame timing table. Run it through
# bin/trace_playback.ps1 -Usdview, which also passes --traceToFile so the
# Chrome trace covers usdGen and Storm together.
#
# Environment:
#   USDGEN_TRACE_FRAMES  first:last (default: the stage's start/end)
#   USDGEN_TRACE_LOOPS   passes over the range (default 2)
#   USDGEN_TRACE_TABLE   where to write the table (default: stdout only)
#   USDGEN_TRACE_COMPLEXITY  low | medium | high | veryhigh (default high:
#                        the hair shader is bound from refineLevel 2)
import os
import time

from pxr import Trace


def _frames(stage):
    spec = os.environ.get("USDGEN_TRACE_FRAMES", "")
    if spec:
        first, _, last = spec.partition(":")
        first = float(first)
        last = float(last) if last else first
    else:
        first, last = stage.GetStartTimeCode(), stage.GetEndTimeCode()
    frames = []
    frame = first
    while frame <= last + 1e-9:
        frames.append(frame)
        frame += 1.0
    return frames


def testUsdviewInputFunction(appController):
    from pxr.Usdviewq.qt import QtWidgets
    app = QtWidgets.QApplication.instance()
    stage = appController._dataModel.stage
    view = appController._stageView
    loops = max(1, int(os.environ.get("USDGEN_TRACE_LOOPS", "2")))
    frames = _frames(stage)
    rows = []
    from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
    complexity = os.environ.get("USDGEN_TRACE_COMPLEXITY", "high") or "high"
    settings = appController._dataModel.viewSettings
    settings.complexity = RefinementComplexities.fromId(complexity)
    print("usdview playback: %d frames x %d loops at complexity %s"
          % (len(frames), loops, complexity))

    # Time spent inside the viewport's paintGL (Hydra sync and draw), so the
    # table can tell it apart from the rest of the Qt event processing
    # (widget updates, compositing, buffer swap).
    painted = [0.0]
    if view:
        paint = view.paintGL

        def timedPaint():
            begin = time.perf_counter()
            try:
                paint()
            finally:
                painted[0] += time.perf_counter() - begin
        view.paintGL = timedPaint
        fmt = view.format()
        print("usdview playback: swap interval %d (1 = the swap waits for vsync)"
              % fmt.swapInterval())

    app.processEvents()
    if view:
        view.repaint()
    for loop in range(1, loops + 1):
        for frame in frames:
            with Trace.TraceScope("usdview playback: loop %d frame %g" % (loop, frame)):
                painted[0] = 0.0
                start = time.perf_counter()
                appController.setFrame(frame)
                changed = time.perf_counter()
                if view:
                    # repaint() asks for the paint now; a QOpenGLWidget may
                    # still run it from the event loop below.
                    view.repaint()
                app.processEvents()
                done = time.perf_counter()
            paint_ms = painted[0] * 1e3
            rows.append((loop, frame, (changed - start) * 1e3, paint_ms,
                         (done - changed) * 1e3 - paint_ms, (done - start) * 1e3))

    lines = [" loop  frame  setFrame(ms)  paintGL(ms)  other Qt(ms)  total(ms)"]
    for loop, frame, set_ms, paint_ms, qt_ms, total_ms in rows:
        lines.append("%5d %6g %13.2f %12.2f %13.2f %10.2f"
                     % (loop, frame, set_ms, paint_ms, qt_ms, total_ms))
    for loop in range(1, loops + 1):
        picked = [r for r in rows if r[0] == loop]
        if picked:
            def average(column):
                return sum(r[column] for r in picked) / len(picked)
            mean = average(5)
            lines.append("loop %d: mean %.2f ms/frame (%.1f fps), worst %.2f ms; "
                         "setFrame %.2f, paintGL %.2f, other Qt %.2f"
                         % (loop, mean, 1000.0 / mean if mean else 0.0,
                            max(r[5] for r in picked), average(2), average(3),
                            average(4)))
    text = "\n".join(lines)
    print(text)
    table = os.environ.get("USDGEN_TRACE_TABLE")
    if table:
        with open(table, "w") as fh:
            fh.write(text + "\n")
