# Progressive Storm felt capture

`progressive-felt.gif` is a real-time playback of the Storm viewport during a
single asynchronous cook of `examples/felt/felt_sphere.usda`. Its 28 recorded
frames come from `StageView.grabFrameBuffer()` at 640 × 640; `capture.json`
contains each frame's elapsed wall time, cook count, and publication count.
The GIF adds a labelled bare-emitter lead-in and a final hold. The source PNGs
are unmodified.

The capture harness warmed the complete groom, deactivated all three groom
descriptions in one session-layer change, changed their scatter seeds, then
reactivated all three together in one change. This produced a smooth bare
emitter before the fresh cook. The three authored densities were unchanged:
1,650,000, 450,000, and 7,000 strands per unit area. The publication counter
advanced from 124 to 241 during the 5.609-second recording. The contact sheet
shows the bare emitter, early, partial, and complete frames.
The first new publication appeared at 0.171 seconds; the last count change
appeared at 3.078 seconds, followed by a settled final frame.

Reproduce from the repository root:

```powershell
$env:USDGEN_PROGRESS_DIR = Join-Path (Get-Location) 'renders\progressive-felt'
.\bin\launch_usdview.ps1 -TestScript .\tools\capture_progressive_felt.py .\examples\felt\felt_sphere.usda --allow-async --renderer GL
python .\tools\encode_progressive_gif.py .\renders\progressive-felt
```

The installed `testusdview` logged two `Invalid render buffer` verification
errors during its initial render, before the capture callback, and its global
error check returned exit code 1. The subsequent framebuffer images were valid
and showed the bare emitter, progressive fill, and final dense groom. The raw
launcher output is preserved in `capture.log`; this harness does not clear or
suppress the renderer errors.
