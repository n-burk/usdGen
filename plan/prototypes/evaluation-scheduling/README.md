# Probes for gap `evaluation-scheduling-and-batching`

Build (headless, no GL needed):

    cmake -S . -B build -G Ninja && cmake --build build

| binary | what it answers | captured output |
|---|---|---|
| `evalSched` | exact `PrimsAdded/Removed/Dirtied` sequence a renderer-level plugin sees per frame change / per attribute edit, replaying `UsdImagingGLEngine::PrepareBatch` (engine.cpp:483-498) | `run1.txt` |
| `chainOrder` | where a renderer scene-index plugin lands relative to `HdsiSceneGlobalsSceneIndex`, hdGp and Storm's plugins | `chainOrder.txt`, `chainOrder_hdgp.txt` (run with `HDGP_INCLUDE_DEFAULT_RESOLVER=1`) |
| `models` | cook counts + cook threads for eager / lazy / deferred-on-frame / deferred-on-commit, plus an atomic-snapshot tearing test | `models.txt` |
| `asyncProbe` | whether `asyncAllow`/`asyncPoll` reach a buried filtering scene index and whether notices it emits during the poll trigger a redraw | `async.txt` |
| `noticeCost` | CPU cost of the per-frame notice cascade at 1..5000 scalps, batched vs not | `noticeCost.txt` |
