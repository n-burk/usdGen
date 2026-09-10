# usdGen — Hair/Fur Grooming for OpenUSD

An XGen-like procedural hair/fur grooming and instancing plugin for
OpenUSD 26.08 and Hydra 2.0.

**Status: M0 (skeleton).** The repository builds, the codeless schema and its
`UsdGenDescription` compute-extent registration are wired, the chain-order
vertical demo passes, and the test tiers T0–T4 are defined with CI. The
operator engine, tile publisher, shipped shaders, and the usdview tool loop
land in M1 and later — see `plan/11-roadmap.md` (§2.1 is the M0 exit). The
full design document is in `plan/` (`plan/README.md` is the entry point).

## Prerequisites

- OpenUSD v26.08 installed at `../OpenUSD_26_08` (override with
  `-DUSD_INSTALL_DIR=`).
- CMake ≥ 3.26 and a C++17 compiler (GCC, Clang, or MSVC).
- Ninja is recommended. On Unix, the shell wrapper uses it when available;
  on Windows, use `bin/build_usdgen.ps1` from PowerShell.

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
and `USDGEN_BUILD_TESTS`. The POSIX process and ELF inspection harnesses are
enabled by default on Unix and disabled by default on Windows; set
`-DUSDGEN_BUILD_TESTS=ON` when using a compatible test environment.

## Test

At M0 all seven registered ctest tests are T0/T1, so a bare `ctest` is the
complete M0 suite. The tier labels:

    ctest --test-dir build -L T0 --output-on-failure   # build rules: gate B-1 (link/include)
    ctest --test-dir build -L T1 --output-on-failure   # chain order + plugin discovery + extent
    ctest --test-dir build --output-on-failure         # all 7

The T1 tests carry `PXR_PLUGINPATH_NAME` (build-tree plugin resource dirs) and
`LD_LIBRARY_PATH` in their ctest `ENVIRONMENT`, so they run with nothing
sourced. All five gate B-1 tests (`testUsdGenLinkRule_usdGen`,
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
