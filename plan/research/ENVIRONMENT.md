# Measurement host notes (historical)

Snapshot from 2026-09-04 of one Linux aarch64 workstation used while the plan was written. It is not a requirement for building usdGen, and it is not a setup guide. Current build steps are in the repository [README](../../README.md) and in [AGENTS.md](../../AGENTS.md).

## What that host had

- OpenUSD 26.08 at `$USD` (headers in `include/pxr`, libraries in `lib`, tools in `bin`).
- Python bindings in `$USD/lib/python3.12/site-packages`, not `lib/python`.
- The OpenUSD install did not include Ptex or SeExpr. This repository vendors those libraries under `thirdparty/`.
- MaterialX, OpenSubdiv, and oneTBB were present as part of that OpenUSD build.
- Storm on that host needed either a display or an EGL device-platform context. `usdcat` did not.

## Corrections recorded the same day

These override any older sentence in the plan that says Storm cannot run without an X display on that host.

- A virtualenv at `$VENV` had Python 3.12, PySide6, `pybind11`, and `numpy`.
- Storm was run headlessly through an EGL device-platform context, and also through a software display. Frame times from the software display are CPU numbers, not GPU numbers.
- `usdrecord` opening its own window was unreliable without a real display. The EGL harness was the driver for GPU timings.
- usdRig was built out of tree against the same OpenUSD prefix. Those build directories are not part of this repository. Do not assume they exist.

Citations elsewhere to `research/ENVIRONMENT.md` or its corrections block mean this page.
