# Verified environment facts (checked 2026-09-04, do not re-derive)

## Hardware / OS
- Linux 6.17 aarch64 (arm64). 20 CPUs. NVIDIA GB10 GPU present (`nvidia-smi -L` ok).
- **No DISPLAY, no WAYLAND_DISPLAY. No Xvfb installed. No sudo (password required).**
- `/usr/bin/Xorg` exists but cannot be started without root.

## OpenUSD
- SOURCE: `/home/burkard/work/OpenUSD` at git tag **v26.08** (`ee47c679a`). Read-only for us.
- INSTALL: `/home/burkard/work/OpenUSD_26_08` (headers in `include/pxr`, libs in `lib`,
  plugins in `lib/usd/*/resources` and `plugin/usd`, tools in `bin`).
- Python bindings: **`/home/burkard/work/OpenUSD_26_08/lib/python3.12/site-packages`**
  (NOT `lib/python`). Import with:
  `PYTHONPATH=/home/burkard/work/OpenUSD_26_08/lib/python3.12/site-packages /home/burkard/.venv/bin/python3`
  Verified working: `pxr.Usd`, `pxr.UsdImagingGL`, `pxr.Usdviewq`.
- Python interpreter: `/home/burkard/.venv/bin/python3` = **3.12.3**. PySide6 6.11.2 and PyOpenGL
  are importable. `pybind11` and `numpy` are NOT installed (checked by an earlier agent).
- Bundled third party in the install: MaterialX 1.39.5 (incl. `libMaterialXGenGlsl`),
  OpenSubdiv 3.6.1, oneTBB 2020.3.1. **No Ptex, no OpenImageIO, no SeExpr** in the install.
- Hio image plugins present: `hioAvif`, `hioOpenEXR` (plus hio's built-in stb formats).
  `hioOiio` and `hioImageIO` are NOT built.
- Hgi backends built: **hgiGL only** (`libusd_hgiGL.so`). No hgiVulkan, no hgiMetal.
- `garch` on Linux is **GLX-only** in 26.08 (`glPlatformContextGLX.cpp`; there is no EGL
  context path). Therefore Storm needs an X display.
- `PXR_ENABLE_PTEX_SUPPORT` defaults OFF (`cmake/defaults/Options.cmake:36`) and this
  install was built without it.

## What CAN be run here
- Headless C++ built against the install (`find_package(pxr CONFIG PATHS /home/burkard/work/OpenUSD_26_08)`).
  g++ 13.3, cmake 3.28.3, ninja available.
- Headless scene-index work: `UsdImagingCreateSceneIndices`, `HdMergingSceneIndex`,
  `HdFlatteningSceneIndex`, GetPrim pulls, notice observers. No render delegate needed.
- Headless Python USD: stage authoring, `Usd.Stage`, `Sdf`, `Ts`, `UsdShade`, `Sdr` registry,
  MaterialX ShaderGen (offline GLSL generation).
- Building third-party libs from source (an earlier agent already built SeExpr2 and Ptex here).

## What CANNOT be run here
- **Anything that needs a GL context**: `usdview`, `testusdview`, `usdrecord --renderer GL`,
  `UsdImagingGLEngine::Render`, Storm shader compilation, GPU timings, picking, screenshots.
  `usdrecord` was tried and it core-dumps (no X display).
- Therefore all Storm *runtime* numbers must be written as a **benchmark protocol to run on a
  workstation with a display**, and clearly labelled UNMEASURED in the plan. Never invent numbers.

## usdRig
- `/home/burkard/work/usdRig`, branch `main`, working tree clean at session start. **Do not modify it.**
  Build only out-of-tree: `cmake -S /home/burkard/work/usdRig -B <scratch>/usdRigBuild -DUSD_INSTALL_DIR=/home/burkard/work/OpenUSD_26_08 -DCMAKE_PREFIX_PATH=/home/burkard/work/OpenUSD_26_08`.
- `bin/_env.sh` hard-codes python3.11 and sibling `usd-install`/`usd-pr4156-venv` paths that do
  not exist here; set `USD=/home/burkard/work/OpenUSD_26_08` and override `PY_SITE` yourself.
- README marks Linux as "intended, not yet verified".

## Target project
- `/home/burkard/work/usdGen` exists and is EMPTY. The new plugin is expected to live there as a
  sibling project (working name **usdGen**, C++ namespace/prefix `UsdGen`, USD property namespace
  `usdGen:`), consuming the unmodified OpenUSD install, and optionally `find_package(rigExec)`.

## CORRECTIONS (2026-09-04, after the verification round — these override the sections above)
- `pybind11` 3.1.0 and `numpy` 2.5.2 ARE installed in `/home/burkard/.venv` (installed during
  the verification round). `ninja` 1.13.2 is installed in the venv too
  (`/home/burkard/.venv/bin/ninja`; pass `-DCMAKE_MAKE_PROGRAM` or put the venv bin on PATH).
- **Storm CAN run headlessly here, two ways:**
  1. GPU-accelerated on the NVIDIA GB10 via an EGL device-platform context
     (`EGL_EXT_platform_device`, compatibility profile, 64x64 pbuffer). Harness:
     `scratchpad/probes/storm-hair-look/eglctx.h` + `render_hair.cpp` / `bench_hair.cpp`.
     Real GPU frame times were measured this way. `usdrecord` itself still core-dumps (it opens a
     GLX window), so use the harness as the driver.
  2. Software (llvmpipe) through a user-space Xvfb on `DISPLAY=:77` (pid may change; restart with
     `scratchpad/xvfbtry/root/usr/bin/Xvfb :77 -screen 0 1280x1024x24 -nolisten tcp -xkbdir
     scratchpad/xvfbtry/root/usr/share/X11/xkb &`). `usdview`, `testusdview`, `usdrecord --renderer GL`
     all work there. **llvmpipe frame times are CPU numbers, never Storm GPU numbers.**
- usdRig is built out of tree at `scratchpad/usdRigBuild` (26/27 ctest pass; needs
  `-DCMAKE_CXX_FLAGS=-ffp-contract=off`); env snippet `scratchpad/probes/B-build/rigexec_env.sh`;
  installed prefix `scratchpad/rigExecInstall` (find_package(rigExec) works).
- SeExpr2 and Ptex are built in `scratchpad/thirdparty/install` (see A8).
