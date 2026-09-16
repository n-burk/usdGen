# usdGen — Hair/Fur Grooming for OpenUSD

An XGen-like procedural hair/fur grooming and instancing plugin for
OpenUSD 26.08 and Hydra 2.0.

**Status: M1 near-exit (plan v2).** The repository builds; the codeless schema
(C1 frozen), the 7-kernel operator engine (Scatter/Grow/Noise/Length/Width +
CurveSource/Deform), the groom scene index with dirty router, tile publisher
(C2 frozen) and sessions, and the shipped shaders (C5 frozen, variant A) are
wired; 15/21 M1 gates pass (open: E-1 perf, S-1/S-5/S-6/S-12 bench runs).
The usdview tool loop (M5) is not started — `cApi.h` declares the C ABI but
no `cApi.cpp` implements it. The full design document is in `plan/`
(`plan/README.md` is the entry point); `plan/13-codebase-alignment.md` is the
binding plan-vs-code re-baseline and owns the current start line (M2:
SculptLayer + Freeze kernels, C3 freeze, Hydra-sourced graph builder).

## Prerequisites

- OpenUSD v26.08 installed at `../OpenUSD_26_08` (override with
  `-DUSD_INSTALL_DIR=`).
- CMake ≥ 3.26 and a C++17 compiler (GCC, Clang, or MSVC).
- Ninja is recommended. On Unix, the shell wrapper uses it when available;
  on Windows, use `bin/build_usdgen.ps1` from PowerShell. The supplied
  OpenUSD prefix must be a Windows/MSVC build; Linux and macOS prefixes are
  not binary-compatible. The PowerShell wrapper initializes Visual Studio's
  build environment when needed and reports a missing `pxrConfig.cmake`
  before configuring.

      export PATH=/home/burkard/.venv/bin:$PATH

## Build

    export PATH=/home/burkard/.venv/bin:$PATH
    cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DUSD_INSTALL_DIR=/home/burkard/work/OpenUSD_26_08 \
        -DUSDGEN_WITH_RIGEXEC=OFF
    ninja -C build -j16

`bin/build_usdgen.sh` wraps configure + build (and `--test` runs ctest).
On Windows, the equivalent is
`.\bin\build_usdgen.ps1` (add `-Test` to run ctest). Pass `-Build`,
`-UsdInstallDir`, or `-Generator` when the defaults do not fit the machine.
Useful cache vars: `USDGEN_FP_CONTRACT` (`off` default; `fast` for the
`usdGenMath` FP-contract experiment), `USDGEN_WITH_RIGEXEC` (default `OFF`),
and `USDGEN_BUILD_TESTS` (default `ON` on Unix, `OFF` on Windows). The portable
tests build on every platform; the POSIX process/loader/ELF and EGL/Storm
harnesses are Unix-only and register as honest ctest skips on Windows. Pass
`-Test` to `build_usdgen.ps1` (it enables `USDGEN_BUILD_TESTS`) or set
`-DUSDGEN_BUILD_TESTS=ON` directly.

### CUDA

`USDGEN_ENABLE_CUDA` builds the CUDA execution backend. Without it usdGen
still builds and runs, but any commit that asks for the CUDA backend is
refused at runtime with *"CUDA support was not enabled in this build"*.
The sources require **CUDA 12.8 or newer** (`cudaStreamGetDevice`, and CUB's
separate-output `DeviceReduce::ArgMin`).

`build_usdgen.ps1` turns the backend on by itself when the host can run the
result: it picks the newest installed toolkit whose `crt/host_config.h`
accepts the active MSVC toolset, and requires `nvidia-smi` to report a
compute capability — which is also what CMake's arch probe reads to choose
`CMAKE_CUDA_ARCHITECTURES` (sm_89 on an Ada workstation, sm_121 on the GB10
host), falling back to the GB10 pair when no GPU answers. Pass `-Cuda` to
make a missing toolkit an error instead of a silent CPU-only build,
`-NoCuda` to opt out, or `-CudaToolkitDir` to name a specific toolkit root.
CMake cannot retarget a configured tree's CUDA compiler, so after installing
a new toolkit delete the build directory; the script detects the mismatch and
says so rather than building against the stale one.

## Storm fur shading

The hair materials now use shared R/TT/TRT scattering and geometry-derived
directional self-shadowing. Procedural CPU grooms cache optical depth across
their tiles; native PointInstancer fur can be baked with `usdGenBakeFur` while
remaining instanced in Storm. See [usage, measurements, and rendering limits](docs/storm-fur.md).

## Test


At M0 all seven registered ctest tests are T0/T1, so a bare `ctest` is the
complete M0 suite. The tier labels:

    ctest --test-dir build -L T0 --output-on-failure   # build rules: gate B-1 (link/include)
    ctest --test-dir build -L T1 --output-on-failure   # chain order + plugin discovery + extent
    ctest --test-dir build --output-on-failure         # all 7

The T1 tests carry `PXR_PLUGINPATH_NAME` (build-tree plugin resource dirs) in
their ctest `ENVIRONMENT_MODIFICATION`, and *every* test carries the host's
dynamic-loader variable (`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, or `PATH` on
Windows) pointing at the OpenUSD prefix and the build tree, so they run with
nothing sourced. These use `ENVIRONMENT_MODIFICATION` rather than
`ENVIRONMENT` on purpose: the loader variable is extended instead of replaced
(on Windows it *is* `PATH`, so replacing it strips the inherited system path),
and `path_list_*` joins with the host's native separator, so no path list has
to embed a `;` — which CMake would otherwise parse as a list separator. All five gate B-1 tests (`testUsdGenLinkRule_usdGen`,
`testUsdGenLinkRule_usdGenMath`, `testUsdGenLinkRule_usdGenTestUtils`,
`testUsdGenIncludeRule`, `testUsdGenNoThirdPartyExports`) carry the `T0`
label, so the canonical run `ctest --test-dir build -L '^T[01]$' -j8` covers
every M0 test — no by-name bridge is needed.

## Smoke test (usdcat)

The M0 fixture flattens with the codeless types resolved. `usdcat` is a native
binary in the OpenUSD prefix, so no Python is involved:

    export USD=/home/burkard/work/OpenUSD_26_08
    export PXR_PLUGINPATH_NAME="$PWD/build/usd/usdGenSchema/resources:$PWD/build/usd/usdGenImaging/resources:$USD/plugin/usd"
    export LD_LIBRARY_PATH="$USD/lib:$PWD/build"
    "$USD/bin/usdcat" --flatten tests/scenes/scene_empty_groom.usda

(`source bin/_env.sh` sets these variables and puts `usdcat` on `PATH` — see
plan/10 §3.6; `usdview`/`usdrecord`/`testusdview` are Python scripts whose
shebangs name the build machine's venv — launch them as
`"$PY" "$USD/bin/usdview" …` with `PY` exported by `bin/_env.sh`.)

`usdcat` does not print an extent; the non-empty `extent` on
`UsdGenDescription` (proven by Plug dlopen'ing `libusdGenSchema.so`) is
asserted by `testUsdGenPluginDiscovery` (T1), with the full negative/positive
control evidence in `docs/prework/m0-usdcat-extent-evidence.md`.

## SeExpr expression editor (usdview plugin)

`plugin/usdGenTools` adds a **usdGen** menu to usdview with a **SeExpr
Expression Editor** dock (`Ctrl+Shift+E`) for the `usdGen:expr:source` of a
`UsdGenExpression` prim. Select the expression, or the operator whose
attribute is connected to it, and the dock follows:

    .\bin\launch_usdview.ps1 plan\examples\expression-width-plane.usda

It follows SeExpr2's own Qt editor: syntax highlighting, line numbers, bracket
matching, a completion popup for `$variables` and functions, validation
against the real engine frontend (errors reported as line and column),
browsable function and variable references grouped by category, the evaluation
domain of the binding, and connect/disconnect for the selected operator's
attributes.

The **Controls** panel is driven by the expression TEXT, not by scanning for
numbers. A top-level statement declares a widget:

    $tip     = 0.15;                                  # 0, 1      slider
    $segs    = 4;                                     # 1, 10     integer slider
    $tint    = [1, 0.5, 0.2];                         # color     swatch + sliders
    $profile = curve($t, 0, 1, 4, 0.5, 0.7, 4, 1, 0, 4);  # curve  editable knots
    $value * $tip * $profile

Editing a control rewrites exactly that value in the text, and editing the
text rebuilds the controls. **Add Widget…** writes one of those lines for you.
Numbers no variable holds still get a plain slider, under *Loose numbers*.

**Apply** writes to the current edit target, **Preview** to the session layer
so the viewport shows the text without touching the layer being authored, and
**Revert** drops either. The **Library** tab loads and saves `.se` expressions:
shipped presets under `plugin/usdGenTools/resources/expressions`, and your own
in `~/.usdGenExpressions` or wherever `USDGEN_EXPRESSION_PATH` points.

Everything it knows about the language comes from the engine through the
`UsdGenTools_*` C ABI in `libs/usdGenImaging/usdGenImaging/usdGenToolsApi.h`,
which is separate from the frozen C4 session ABI in `cApi.h`. The panel is
documented in `plan/08-tools.md` §5.7.

## Kill switch

The groom scene-index plugin registers for all renderers but can be disabled
per-process without touching plugInfo (ADR §5.1):

    export USDGEN_ENABLE=0    # UsdGenGroomSceneIndexPlugin._IsEnabled() -> false

## Regenerating the schema resources

Only after editing `libs/usdGenSchema/schema.usda`:

    bin/gen_schema.sh

This runs OpenUSD's `usdGenSchema` generator (note: that is OpenUSD's tool,
unrelated to our `usdGenSchema` CMake target) and post-processes
`plugin/usdGenSchema/resources/plugInfo.json` into its data-only form. Review
the diff before committing.

## Install

    cmake --install build --prefix /home/burkard/work/usdGen-install

Produces `lib/libusdGen*.so`, `include/usdGen*`, and the plugin resources under
`lib/usd/usdGenSchema/resources` and `lib/usd/usdGenImaging/resources`.

## Test tiers and CI

`plan/10-build-dependencies-testing.md` §5 defines tiers T0–T4 and §6 the CI.
M0 exercises T0 (engine + build rules, no Hydra/`UsdStage`) and T1 (headless
scene index over the real `UsdImagingCreateSceneIndices` chain).
`.github/workflows/usdgen.yml` implements the M0 slice of §6.2 as six jobs:

* `openusd` — OpenUSD v26.08 from the GitHub tag (`build_scripts/build_usd.py`,
  Release + Ninja, `--usd-imaging --no-usdview --no-python --no-materialx`),
  cached on OS + arch + resolved commit + recipe epoch; the prefix ships to
  other jobs as a **tarball** so executable bits survive artifact round-trips.
* `configure-offline` — configure with `FETCHCONTENT_FULLY_DISCONNECTED=ON`
  (offline-configure rule, plan §3.2/§6.2).
* `build-release` — Release configure + `ninja -j16` + the usdcat smoke;
  uploads the build tree.
* `t0-t1` — the canonical `ctest -L '^T[01]$' -j8` against the build-release
  tree; all five gate B-1 tests are labelled `T0`, so the label run alone
  covers the whole M0 suite.
* `install-check` — `cmake --install` into a scratch prefix, layout
  verification (`lib/libusdGen*.so`, `include/usdGen*`, usd plugin
  resources), and python3 validation of both installed `plugInfo.json`
  files (schema: `Type==library`, `LibraryPath` → installed
  `libusdGenSchema.so`, `implementsComputeExtent` on `UsdGenDescription`,
  `AutoApplyAPISchemas` inside `Info`; imaging: `LibraryPath` → installed
  `libusdGenImaging.so`).
* `build-fpfast` — nightly/dispatch stub, `-DUSDGEN_FP_CONTRACT=fast`.

`plan/10` §6.2 also schedules `t2-egl`, `t3-xvfb`, `nightly-perf`,
`nightly-sanitize`, and `with-rigexec`; at M0 those are workstation/manual or
later milestones — T2 (Storm/EGL on the GB10), T3 (Xvfb), and the T4
workstation protocols (`docs/workstation-protocol.md` §§1–11, written at M0;
runner built at M7) run on real hardware, never GitHub.
