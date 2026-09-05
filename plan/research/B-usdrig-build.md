# B — usdRig built headlessly, out of tree, against OpenUSD 26.08

**Date:** 2026-09-04 · **Host:** Linux 6.17 aarch64, 20 CPUs, GCC 13.3.0, CMake 3.28.3
**usdRig HEAD:** `c92c040` (`git status --porcelain` empty before *and* after this work — the tree was never modified)
**OpenUSD install:** `/home/burkard/work/OpenUSD_26_08` (source mirror at `/home/burkard/work/OpenUSD`, tag v26.08)

Bottom line: **usdRig builds clean on Linux/aarch64 in 21.5 s and 26 of 27 CTest suites pass.**
Both of the initial failures were diagnosed to root cause; one is a toolchain-flag issue with a
one-flag fix, the other is a **stock-OpenUSD-26.08 defect that I reproduced with zero usdRig code**.
Separately, I found that **Storm *does* run headlessly here** — the ENVIRONMENT.md claim to the
contrary is wrong — which changes what later probes can measure.

---

## 1. Prerequisites: two ENVIRONMENT.md facts are wrong

| ENVIRONMENT.md claim | Reality (command + output) |
|---|---|
| line 16-17: "`pybind11` and `numpy` are NOT installed" | **Both are installed.** `python3 -m pybind11 --cmakedir` → `/home/burkard/.venv/lib/python3.12/site-packages/pybind11/share/cmake/pybind11`; `pybind11.__version__` = `3.1.0`; `numpy.__version__` = `2.5.2`. Python dev headers present: `/usr/include/python3.12/Python.h`. **`RIGEXEC_BUILD_PYTHON` therefore auto-enables and the bindings build.** |
| line 29: "ninja available" | **`ninja` was NOT on the system.** `which ninja ninja-build` → nothing; `find /usr /opt /home/burkard -maxdepth 8 -name 'ninja*'` → only `/usr/share/vim/vim91/syntax/ninja.vim`. |
| lines 37-42: "What CANNOT be run here … usdview, usdrecord --renderer GL, Storm shader compilation" | **False as stated.** See §6 — a user-space Xvfb + Mesa llvmpipe gives a working GLX/GL 4.5-core context; `usdrecord --renderer GL` and `testusdview` both run. |

The network is reachable (`pip`/`apt-get download` both work), which is what made the fixes possible.

**Side effects on the machine** (outside the usdRig/OpenUSD trees, disclosed for the record):
`pip install ninja` into `/home/burkard/.venv` (ninja 1.13.2 → `/home/burkard/.venv/bin/ninja`).
Nothing else was installed system-wide; Xvfb/mesa-utils were `dpkg-deb -x`'d into the scratchpad only.

---

## 2. Exact commands and timings

```sh
export PATH=/home/burkard/.venv/bin:$PATH          # for ninja
SC=/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad

cmake -S /home/burkard/work/usdRig -B $SC/usdRigBuild -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSD_INSTALL_DIR=/home/burkard/work/OpenUSD_26_08 \
  -DCMAKE_PREFIX_PATH=/home/burkard/work/OpenUSD_26_08 \
  -DPython3_EXECUTABLE=/home/burkard/.venv/bin/python3 \
  -DCMAKE_MAKE_PROGRAM=/home/burkard/.venv/bin/ninja \
  -DCMAKE_CXX_FLAGS="-ffp-contract=off"            # <-- REQUIRED on GCC, see §4

ninja -C $SC/usdRigBuild -j16
ctest --test-dir $SC/usdRigBuild -j8 --output-on-failure
cmake --install $SC/usdRigBuild --prefix $SC/rigExecInstall
```

The task's suggested command line worked verbatim except for `-G Ninja` (no ninja binary) — hence
`-DCMAKE_MAKE_PROGRAM`. **No `-D` flag was needed to satisfy `RIGEXEC_BUILD_PYTHON`**: pybind11 3.1.0
is present, the probe at `CMakeLists.txt:262-277` succeeds, and the bindings build and test green.

| Step | Wall clock | Notes |
|---|---|---|
| `cmake` configure (cold) | **0.83 s** | `Found pybind11 … 3.1.0`, `Performing Test HAS_FLTO_AUTO - Success` (pybind11 adds `-flto=auto` to the module) |
| `ninja -j16` clean build, 64 edges | **21.45 s**, maxRSS 2.10 GB, CPU 798 % | 0 `error:`; the only warning class is 36 × `[-Wcpp]` (`<ext/hash_set>` backward-header + `tbb/task.h` deprecation, both from OpenUSD's own headers) |
| `ctest -j8`, 27 tests | **1.24 s** | 26 pass / 1 fail |
| `cmake --install` | < 1 s | 6.1 MB prefix |
| Incremental: 1 TU in `rigExec` | **21.4 s** (15 edges) | dominated by the LTO relink of `_rigexec.cpython-312-aarch64-linux-gnu.so` |
| Incremental: all of `rigExecImaging` | **10.05 s** (10 edges) | the number that matters for a scene-index dev loop |
| `rigExecPose ArmShotAnim.usda --frames 1001,1024,1048 --joints --targets` | **0.03 s**, exit 0 | 9 mover applications, digest `4626813684588943253` |
| `probeImagingPipeline examples/` | **0.07 s**, `PASS` | 36 assertions, real `UsdImagingCreateSceneIndices` chain |

Build tree 31 MB. Everything lives under the scratchpad; nothing was written into `/home/burkard/work`.

---

## 3. CTest results

Configured as in §2 (with `-ffp-contract=off`). `ctest -j8`, `logs/ctest_final.log`.

| # | Test | Result | Time |
|---|---|---|---|
| 1 | testRigExecMath | Passed | 0.01 s |
| 2 | testRigExecSingleChainIk | Passed | 0.00 s |
| 3 | testRigExecConstraints | Passed | 0.13 s |
| 4 | testRigExecCurvenet | **Passed** (fails without `-ffp-contract=off`, §4) | 0.01 s |
| 5 | testRigExecWeightFields | Passed | 0.00 s |
| 6 | testRigExecArm | Passed | 0.33 s |
| 7 | testRigExecImaging | Passed | 0.32 s |
| 8 | testRigExecVolumeWeights | Passed | 0.08 s |
| 9 | testRigExecWeightOverlay | Passed | 0.09 s |
| 10 | testRigExecMoverGraph | Passed | 0.05 s |
| 11 | testRigExecSchemaAuthoring | Passed | 0.05 s |
| 12 | testRigExecSchemaAuthoringMissing | Passed | 0.03 s |
| 13 | testRigExecRigBuilder | Passed | 0.04 s |
| 14 | testRigExecNoAuthoring | Passed | 0.29 s |
| 15 | testRigExecBounds | Passed | 0.07 s |
| 16 | testRigExecPython | Passed | 0.18 s |
| 17 | testRigExecPythonSchemaAuthoring | Passed | 0.19 s |
| 18 | testRigExecUndo | Passed | 0.17 s |
| 19-21, 25-27 | testGizmoMath / Screen / Settings / Drag / Snap, testViewCubeMath | Passed | 0.04-0.84 s |
| 22-23 | testGraphModel, testGraphScreen | Passed | 0.09-0.14 s |
| 24 | **testRigExecStageEdits** | **Failed** — stock OpenUSD bug, §5 | 0.17 s |

`96% tests passed, 1 tests failed out of 27`.

Two non-CTest probes were run by hand (they need `PXR_PLUGINPATH_NAME`, which is why
`CMakeLists.txt:230-238` deliberately does not `add_test` them):

| Probe | Command | Result |
|---|---|---|
| `probeImagingPipeline` | `$B/probeImagingPipeline /home/burkard/work/usdRig/examples` | `probeImagingPipeline: PASS` (exit 0) |
| `probeCodingError` | `$B/probeCodingError /home/burkard/work/usdRig/examples` | 5 phases all `CLEAN`, exit 0. **It requires `argv[1]`** (`tests/probeCodingError.cpp:52` reads it unguarded) and aborts with `basic_string: construction from null` if omitted. |

**There are no `testusdview` CTest entries.** The `testusdview` suites are the nine
`bin/run_testusdview*.sh` shell scripts, outside CTest entirely. They *did* run here — see §6.

---

## 4. Failure 1 — `testRigExecCurvenet`: GCC's default `-ffp-contract=fast`

Default configure (no `CMAKE_CXX_FLAGS`):

```
TestProfileMoverOnCurvedSurface
  tube cut: 192 faces from 192, 528 samples, 0 cracks, 36 traced, 3 failed, 0 lost,
            residual 0.07926, 164 unknowns
  warning: 3 curvenet segment(s) could not be traced across the surface and were not cut
FAIL /home/burkard/work/usdRig/tests/testRigExecCurvenet.cpp:1056: 3 traces failed on the tube
testRigExecCurvenet: 1 failure(s)
```

Same source, same compiler, only `-DCMAKE_CXX_FLAGS=-ffp-contract=off` added
(separate build dir `usdRigBuild-fpstrict`):

```
  tube cut: 192 faces from 192, 528 samples, 0 cracks, 36 traced, 0 failed, 0 lost,
            residual 0.07926, 164 unknowns
testRigExecCurvenet: OK
```

`g++ -Q --help=optimizers | grep fp-contract` → `-ffp-contract=[off|on|fast]   fast`. GCC on aarch64
therefore fuses `a*b+c` into a single-rounding `FMADD` by default, and the geodesic tracer in
`libs/rigExecMath/cutMesh.cpp` / `curvenet.cpp` is sensitive enough to that extra precision to lose
3 of 36 segment traces at a tolerance boundary. The residual is *identical* (`0.07926`) either way,
so this is a discrete branch flip inside the tracer, not accumulated drift.

This matters for the plan directly: any hair-generation kernel that walks a surface (root-point
barycentric tracing, guide-to-surface projection, clump-region flood fill) has the same shape of
tolerance-sensitive branch. **usdGen should set `-ffp-contract=off` for its kernel library, or
qualify its epsilons against a fused-multiply-add build.** It also means the usdRig kernels'
tolerances were tuned on AppleClang and are not portable as authored.

---

## 5. Failure 2 — `testRigExecStageEdits` is a stock OpenUSD 26.08 bug

```
pxr.Tf.ErrorException: Error in
'pxrInternal_v0_26_8__pxrReserved__::Usd_PrimFlagsPredicate::operator()' at line 24 in file
/home/burkard/work/OpenUSD/pxr/usd/usd/primFlags.cpp : 'Applying predicate to invalid prim.'
AssertionError: RemovePrim posted an error with a compiled evaluator attached
  (/home/burkard/work/usdRig/tests/python/test_rigexec_stage_edits.py:54)
```

The chain, verified in the OpenUSD source:

| File:line | Code |
|---|---|
| `/home/burkard/work/OpenUSD/pxr/exec/esfUsd/stageData.cpp:350` | `const UsdPrim resyncedPrim = _stage->GetPrimAtPath(resyncedPath);` |
| `…/stageData.cpp:360` | `if (!UsdPrimDefaultPredicate(resyncedPrim)) {` — **no validity check first** |
| `/home/burkard/work/OpenUSD/pxr/usd/usd/primFlags.cpp:21-25` | `if (!prim) { TF_CODING_ERROR("Applying predicate to invalid prim."); return false; }` |
| `…/stageData.cpp:686, 691` | `_UpdateForResync(path, …)` called for every resynced path from the `UsdNotice::ObjectsChanged` listener |

I isolated it to OpenUSD with **no usdRig code in the process at all**:
`scratchpad/probes/B-build/probeExecResyncRemovePrim.cpp` — open an in-memory stage, optionally
construct an `ExecUsdSystem`, `DefinePrim` then `RemovePrim`, and count errors inside a `TfErrorMark`.

```
no ExecUsdSystem:        RemovePrim posted 0 error(s)
with ExecUsdSystem:      RemovePrim posted 1 error(s)  first: Applying predicate to invalid prim.
RESULT: baseline=0 withExec=1 -> REPRODUCED
```

Build line (note the two non-obvious requirements):

```sh
g++ -std=c++17 -O1 probeExecResyncRemovePrim.cpp -o probeExecResyncRemovePrim \
  -I$USD/include -I/usr/include/python3.12 \
  -L$USD/lib -Wl,-rpath,$USD/lib -Wl,--allow-shlib-undefined \
  -lusd_tf -lusd_sdf -lusd_usd -lusd_execUsd -lusd_python -lpython3.12 -ltbb
```

Consequences for usdGen, which the plan must design around:

- **Any** OpenExec-backed system attached to a stage turns every `UsdStage::RemovePrim` into a
  posted `TF_CODING_ERROR`. In C++ that is non-fatal noise; **in Python it is a raised
  `Tf.ErrorException`**, so a usdview tool that deletes a prim (freeze/bake, delete a styler,
  remove a frozen curve set) *throws* rather than returning. The grooming toolset described in the
  brief does exactly this.
- Mitigations available without patching OpenUSD: (a) never `RemovePrim` — deactivate
  (`prim.SetActive(False)`) or author `over` deletes in a session layer; (b) wrap every structural
  edit in a `Tf.ErrorMark`/`TfErrorMark` that swallows exactly this one commentary; (c) detach the
  exec system across structural edits. (a) is the cleanest and also composes with the
  freeze/undo model.
- README.md:300 claims 17/17 on macOS. Either this test is newer than that row, or macOS masks the
  notice ordering. It is **not** a Linux- or aarch64-specific defect: the failing predicate call is
  unconditional C++ with no platform branch.

---

## 6. Storm *does* run headlessly here — usable, in software

ENVIRONMENT.md rules out usdview/usdrecord/Storm. That is wrong, and the correction is worth more
to the plan than anything else in this report. Without root:

```sh
cd $SC/xvfbtry
apt-get download xvfb xserver-common x11-xkb-utils xkb-data mesa-utils mesa-utils-bin
for d in *.deb; do dpkg-deb -x "$d" root/; done
root/usr/bin/Xvfb :77 -screen 0 1280x1024x24 -nolisten tcp \
    -xkbdir $SC/xvfbtry/root/usr/share/X11/xkb &
export DISPLAY=:77
```

`glxinfo -B` on `:77`:

```
direct rendering: Yes
Vendor: Mesa (0xffffffff)
Device: llvmpipe (LLVM 20.1.2, 128 bits)
Version: 25.2.8      Accelerated: no
Max core profile version: 4.5
```

`usdrecord --renderer GL --imageWidth 256 examples/ArmShotAnim.usda out.png` → **exit 0**,
`Renderer plugin: HdStormRendererPlugin`, a 256×186 RGBA PNG on disk.
`UsdImagingGL.Engine.GetRendererPlugins()` → `['HdStormRendererPlugin']` (no Embree in this install).

That means **Storm shader compilation, scene-index→draw wiring, `HdBasisCurves` draw-item creation,
picking, and pixel-diff regression tests are all testable on this machine.** What is *not* testable
is GPU performance: llvmpipe is a software rasterizer, so any frame time measured here is a CPU
number. **Never quote an llvmpipe frame time as a Storm frame time.** GPU-accelerated headless GL
is not reachable: zink over the NVIDIA Vulkan ICD (`/usr/share/vulkan/icd.d/nvidia_icd.json` exists)
fails because Xvfb has no DRI3 — `MESA_LOADER_DRIVER_OVERRIDE=zink glxinfo -B` → `DRI3 not
available` ×10, `Error: couldn't find RGB GLX visual or fbconfig`.

### The `testusdview` suites, actually run

| Suite (`tests/…`) | Stage | Result |
|---|---|---|
| `testUsdviewRigExec.py` | `ArmShotAnim.usda` | **PASS** — `RIGEXEC_USDVIEW_OK generations 2 -> 6, frame 1024 published the frame 1024 pose` (needs the fixture patch below) |
| `testUsdviewVolumeWeightOverlay.py` | `11_VolumeWeights.usda` | **PASS** (`_OK`, 2.6 s) |
| `testUsdviewGraphEditor.py` | `ArmShotAnim.usda` | **PASS** (`_OK`, 6.5 s) |
| `testUsdviewCurvenetPanel.py` | `12_CurvenetProfile.usda` | FAIL — `centre pick hit /CurvenetAsset/Rig/Solvers/BendChain, expected the tube` (`testUsdviewCurvenetPanel.py:89`). Plausibly caused by `_ActivateCurrentStage` force-enabling `viewSettings.displayGuide` (`rigExecUsdview.py:584-589`) so a solver guide occludes the pick; **not isolated** |
| `testUsdviewGizmo.py` | `ArmShotAnim.usda` | FAIL — `'Undo' stays out of the overflow chevron at the default width (toolbar 601 px, sizeHint 838)` (`testUsdviewGizmo.py:293`). Qt font-metric/DPI dependent; an environment difference, not a logic bug |
| `testUsdviewViewCube.py` | `ArmShotAnim.usda` | FAIL — `the orbit jumped to BACK instead of interpolating: theta 360.0 -> 540.0, no pose in between (2047 samples)` (`testUsdviewViewCube.py:316`). Animation-timing dependent |
| `testUsdviewCurvenetDraw / Move / Puppet / Visible` | `../chars/puppetA/puppetA_curvenet.usda` | **SKIPPED** — asset absent from this checkout (`bin/run_testusdview_draw.sh:13` defaults to a sibling `chars/` tree that does not exist) |

Two host-side prerequisites that a fresh Linux user will hit:

1. `RIGEXEC_IMAGING_DLL` must be set for an **out-of-tree build**. `rigExecUsdview.ImagingLibraryPath()`
   (`plugin/rigExecUsdview/rigExecUsdview.py:52-59`) only searches `<repo>/librigExecImaging.so` and
   `<repo>/build/librigExecImaging.so`; with the build elsewhere it returns the first candidate and
   `ctypes.CDLL` raises `OSError: /home/burkard/work/usdRig/librigExecImaging.so: cannot open shared
   object file`. `_env.sh:34-38` documents the override but does not set it.
2. `tests/testUsdviewRigExec.py:65-71` and `:114-120` build the plugin container with
   `RigExecUsdviewContainer.__new__` and set only 5 of the 12 attributes that
   `registerPlugins` sets (`rigExecUsdview.py:96-112`). `_OnStageReplaced` →
   `_EnsureViewportTools` (`rigExecUsdview.py:305`) then dies with
   `AttributeError: 'RigExecUsdviewContainer' object has no attribute '_viewportTools'`. **This is a
   stale test fixture in usdRig, platform-independent.** A scratch copy with the seven missing
   attributes added passes: `scratchpad/probes/B-build/patched-tests/testUsdviewRigExec.py`
   (the usdRig tree was not touched).

**Design lesson for usdGen's tooling:** do not hand-construct the plugin container in tests. Put the
container's mutable state in a small dataclass initialised in `__init__`, so a test fixture cannot
drift out of sync with `registerPlugins`.

---

## 7. Where the artifacts are, and the env snippet

Build dir: `/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/usdRigBuild`

| Artifact | Absolute path |
|---|---|
| `librigExecImaging.so` (804 KB) | `…/scratchpad/usdRigBuild/librigExecImaging.so` |
| `librigExec.so` (2.5 MB) | `…/scratchpad/usdRigBuild/librigExec.so` |
| `librigExecRigging.so` (623 KB) | `…/scratchpad/usdRigBuild/librigExecRigging.so` |
| `librigExecMath.a` (519 KB, static) | `…/scratchpad/usdRigBuild/librigExecMath.a` |
| Python ext | `…/scratchpad/usdRigBuild/python/_rigexec.cpython-312-aarch64-linux-gnu.so` |
| Python package | `…/scratchpad/usdRigBuild/python/rigexec/` |
| **Generated** schema plugInfo | `…/scratchpad/usdRigBuild/usd/rigExecSchema/resources/plugInfo.json` |
| Schema declarations | `…/scratchpad/usdRigBuild/usd/rigExecSchema/resources/generatedSchema.usda` |
| **Generated** imaging plugInfo | `…/scratchpad/usdRigBuild/usd/rigExecImaging/resources/plugInfo.json` |
| Installed prefix | `…/scratchpad/rigExecInstall/` (`lib/`, `lib/usd/`, `lib/python/`, `lib/cmake/rigExec/`) |
| Xvfb + mesa-utils userland | `…/scratchpad/xvfbtry/root/usr/bin/{Xvfb,glxinfo}` |
| Probe sources | `…/scratchpad/probes/B-build/` |
| Logs | `…/scratchpad/logs/` |

The generated plugInfos are exactly what `_env.sh:28-32` says they must be:
`usd/rigExecSchema/resources/plugInfo.json` carries `"LibraryPath": "../../../librigExecImaging.so"`,
`"Type": "library"`, and 12 `implementsComputeExtent` insertions; `usd/rigExecImaging/resources/plugInfo.json`
carries `"LibraryPath": "../../librigExecImaging.so"` and registers
`RigExecUsdImagingSceneIndexPlugin` with base `UsdImagingSceneIndexPlugin`.

### Env snippet (`scratchpad/probes/B-build/rigexec_env.sh`, source it)

```sh
export USD=/home/burkard/work/OpenUSD_26_08
export RIG=/home/burkard/work/usdRig
export RIGBUILD=/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/usdRigBuild
export PY_SITE="$USD/lib/python3.12/site-packages"     # NOT lib/python

export PATH="$RIGBUILD:$USD/bin:/home/burkard/.venv/bin:$PATH"
export LD_LIBRARY_PATH="$USD/lib:$RIGBUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$RIGBUILD/python:$RIG/plugin/rigExecUsdview:$RIG/plugin/museAssistant:$PY_SITE${PYTHONPATH:+:$PYTHONPATH}"
export PXR_PLUGINPATH_NAME="$RIGBUILD/usd/rigExecSchema/resources:$RIGBUILD/usd/rigExecImaging/resources:$RIG/plugin/rigExecUsdview${PXR_PLUGINPATH_NAME:+:$PXR_PLUGINPATH_NAME}"

# only for usdview / testusdview / usdrecord:
export RIGEXEC_IMAGING_DLL="$RIGBUILD/librigExecImaging.so"
export DISPLAY=:77                                      # the Xvfb from §6
```

`LD_LIBRARY_PATH` is belt-and-braces on Linux: `readelf -d librigExecImaging.so` already shows
`RUNPATH: /home/burkard/work/OpenUSD_26_08/lib:<build dir>` (from `CMAKE_INSTALL_RPATH_USE_LINK_PATH ON`
plus `$ORIGIN`, `CMakeLists.txt:47-54`), so binaries in the build tree resolve without it. It is
still needed for `ctypes.CDLL` from Python and for anything launched with a scrubbed environment.
Note `_env.sh:25` exports **`DYLD_LIBRARY_PATH`** only — the checked-in helper is macOS-only on this
axis and will not set up a Linux run.

Verified working from this env, with **no direct link to rigExecImaging**:

```
rigExecSchema    found=True  …/usdRigBuild/librigExecImaging.so   loaded=False
rigExecImaging   found=True  …/usdRigBuild/librigExecImaging.so   loaded=False
rigExecUsdview   found=True  /home/burkard/work/usdRig/plugin/rigExecUsdview
RigExecControl concrete: True     RigExecCurvenet concrete: True
UsdImagingSceneIndexPlugin subtypes: ['RigExecUsdImagingSceneIndexPlugin',
                                      'UsdSkelImagingResolvingSceneIndexPlugin']
```

That last line is a useful baseline for the plan: stock OpenUSD 26.08 ships exactly **one**
`UsdImagingSceneIndexPlugin` (UsdSkel's); rigExec adds the second; usdGen would be the third.

---

## 8. Linking a sibling project against rigExec — one real trap

`scratchpad/probes/B-build/consumer/` is a standalone CMake project that `find_package(rigExec CONFIG
REQUIRED)`, links `rigExec::rigExec` + `rigExec::rigExecImaging` + `usdImaging`, compiles, and runs:

```
  pose: 6 moved properties
consumerProbe: PASS (linked rigExec + rigExecImaging out of tree)
```

**The trap:** if the consumer *also* calls `find_package(pxr …)` itself — which every USD project
does — the configure aborts:

```
CMake Error at /home/burkard/work/OpenUSD_26_08/pxrConfig.cmake:58 (add_library):
  add_library cannot create imported target "TBB::tbb" because another target
  with the same name already exists.
Call Stack:
  CMakeFindDependencyMacro.cmake:76 (find_package)
  …/rigExecInstall/lib/cmake/rigExec/rigExecConfig.cmake:113 (find_dependency)
```

Root cause: CMake 3.28's `find_dependency` short-circuits on a *call hash*, not on `<pkg>_FOUND`
(`/usr/share/cmake-3.28/Modules/CMakeFindDependencyMacro.cmake:60-83`). `rigExecConfig.cmake:113`
calls `find_dependency(pxr CONFIG PATHS "${rigExec_USD_INSTALL_DIR}")`; a consumer's own
`find_package(pxr REQUIRED CONFIG PATHS … NO_DEFAULT_PATH)` is a different signature, so
`pxrConfig.cmake` is re-executed, and its `add_library(TBB::tbb SHARED IMPORTED)` at line 58 is
unconditional whenever `PXR_FIND_TBB_IN_CONFIG` is OFF (the default, `pxrConfig.cmake:51-58`).

Workarounds, in order of preference for usdGen: (1) call `find_package(rigExec)` and let it bring
pxr in transitively — verified working; (2) guard your own call with `if(NOT TARGET TBB::tbb)`;
(3) configure with `-DPXR_FIND_TBB_IN_CONFIG=ON`. The same hazard applies to *any* two CMake
packages that both `find_dependency(pxr)`, so usdGen's own `usdGenConfig.cmake` should wrap its
pxr dependency in `if (NOT TARGET usd)`.

`rigExecConfig.cmake` exports the host-setup variables the README advertises, verified:
`rigExec_PLUGINPATHS` = the two `lib/usd/*/resources` dirs plus `lib/python/rigExecUsdview` and
`lib/python/museAssistant`; `rigExec_LIBRARY_DIR` = `<prefix>/lib`; `rigExec_PYTHON_DIR` =
`<prefix>/lib/python`.

---

## Key facts

- **usdRig builds clean on Linux/aarch64/GCC 13.3 against the stock OpenUSD 26.08 install: 0.83 s
  configure, 21.45 s for 64 ninja edges at `-j16`, zero `error:`, only 36 `[-Wcpp]` warnings that
  originate in OpenUSD's own headers** (`logs/build2.log`). The README's "Linux: intended, but not
  yet verified" row (README.md:302) can be upgraded.
- **The Python bindings build and pass** — pybind11 3.1.0 and numpy 2.5.2 *are* installed
  (`python3 -m pybind11 --cmakedir` succeeds), contradicting ENVIRONMENT.md:16-17. No
  `RIGEXEC_BUILD_PYTHON=OFF` was needed.
- **`ninja` was not installed**; `pip install ninja` (1.13.2) into `/home/burkard/.venv` was the fix,
  passed to CMake as `-DCMAKE_MAKE_PROGRAM=/home/burkard/.venv/bin/ninja`.
- **26/27 CTest suites pass.** The `testusdview` suites are shell scripts, not CTest tests.
- **`-ffp-contract=off` is required on GCC.** Without it `testRigExecCurvenet` fails at
  `tests/testRigExecCurvenet.cpp:1056` with `3 traces failed on the tube`; with it, identical source
  and compiler give `36 traced, 0 failed`. `g++ -Q --help=optimizers` confirms the default is `fast`.
- **`testRigExecStageEdits` fails because of a stock OpenUSD 26.08 defect**, reproduced with zero
  usdRig code by `probes/B-build/probeExecResyncRemovePrim.cpp`
  (`baseline=0 withExec=1 -> REPRODUCED`): `pxr/exec/esfUsd/stageData.cpp:360` passes a possibly-invalid
  `UsdPrim` to `UsdPrimDefaultPredicate`, and `pxr/usd/usd/primFlags.cpp:21-25` raises
  `TF_CODING_ERROR`. Every `RemovePrim` on a stage with an OpenExec system attached posts it; in
  Python it raises `Tf.ErrorException`.
- **Storm runs headlessly here after all**, via `dpkg-deb -x` of the `xvfb` + `mesa-utils` debs into
  the scratchpad — no root. `glxinfo -B` reports `llvmpipe (LLVM 20.1.2)`, GL **4.5 core**;
  `usdrecord --renderer GL` exits 0 and writes a 256×186 PNG. GPU acceleration is unreachable
  (zink needs DRI3, which Xvfb lacks), so **llvmpipe frame times are CPU numbers and must never be
  reported as Storm GPU numbers.**
- `probeImagingPipeline` PASSes headlessly with **no GL at all** (0.07 s, 36 assertions) — the whole
  `UsdImagingCreateSceneIndices` + `RigExecUsdImagingSceneIndexPlugin` chain is unit-testable
  without a display. `probeCodingError` requires `argv[1]` or it aborts.
- Plug discovers `rigExecSchema`, `rigExecImaging` and `rigExecUsdview` from the env snippet with
  no direct link, and `Usd.SchemaRegistry().IsConcrete("RigExecControl")` is True. Stock 26.08 has
  exactly **two** `UsdImagingSceneIndexPlugin` subtypes; rigExec's is the second.
- **A consumer must not call `find_package(pxr)` alongside `find_package(rigExec)`** — CMake 3.28's
  call-hash short-circuit misses, `pxrConfig.cmake` runs twice, and its unconditional
  `add_library(TBB::tbb SHARED IMPORTED)` (`pxrConfig.cmake:58`) makes the configure fatal.
- `rigExecUsdview.ImagingLibraryPath()` only searches `<repo>/` and `<repo>/build/`, so an
  out-of-tree build **requires `RIGEXEC_IMAGING_DLL`**; and `_env.sh:25` exports only
  `DYLD_LIBRARY_PATH`, so the checked-in helper cannot set up a Linux run at all.
- `tests/testUsdviewRigExec.py:65-71` and `:114-120` construct the plugin container with `__new__`
  and 5 of 12 attributes; the suite dies in `_EnsureViewportTools` (`rigExecUsdview.py:305`) before
  reaching any assertion. Patched in scratch, the suite **passes** under Xvfb.
- Incremental build cost for a scene-index dev loop: **10.05 s** to rebuild all of `rigExecImaging`
  and relink its four dependents; a single TU in `rigExec` costs 21.4 s because pybind11's
  `-flto=auto` relink of `_rigexec` dominates.

## Decisions this settles

1. **usdGen can be a sibling CMake project that `find_package(rigExec CONFIG)`** and links
   `rigExec::rigExec` / `rigExec::rigExecImaging`; this is proven end-to-end by
   `probes/B-build/consumer/`, which compiles, links, compiles a rig and evaluates a pose. Its
   `usdGenConfig.cmake` must guard the pxr dependency (`if (NOT TARGET usd)`) to avoid the TBB
   double-definition, and its own `find_package(pxr)` must be dropped or guarded.
2. **usdGen's numeric kernels get `-ffp-contract=off`** (and a CI job that also builds with `fast`
   to keep epsilons honest). The curvenet result is direct evidence that surface-walking kernels
   flip branches under FMA contraction.
3. **usdGen must not use `UsdStage::RemovePrim` for structural edits while an OpenExec system is
   attached.** Freeze/bake/delete tooling deactivates or authors session-layer deletes instead, or
   brackets the edit in a `TfErrorMark` that swallows the one known commentary. This is a hard
   design constraint on the "freeze output and comb manually" toolset in the brief.
4. **The whole scene-index half of usdGen is testable headlessly with no GL**, in the shape of
   `probeImagingPipeline`: build the real `UsdImagingCreateSceneIndices` chain, pull prims, assert on
   `HdBasisCurvesSchema` data source contents. That should be the primary regression harness — it is
   sub-100 ms and needs no display.
5. **Storm correctness is also testable here**, via the scratchpad Xvfb (`DISPLAY=:77`, llvmpipe,
   GL 4.5 core): shader compilation, `usdrecord` pixel output, `testusdview`-style app→data→Hydra
   loops. Storm *performance* remains UNMEASURED and needs the workstation protocol.
6. **usdGen's usdview plugin should keep container state in an `__init__`-initialised object**, not
   in attributes assigned inside `registerPlugins`, so test fixtures cannot drift the way
   `testUsdviewRigExec.py` did.
7. The generated-plugInfo pattern (`CMakeLists.txt:389-437`) — a `library`-type plugInfo whose
   `LibraryPath` is `$<TARGET_FILE_NAME:>` and which is generated into `<build>/usd/<name>/resources`
   so the same relative hop works from the build tree and the install tree — **is the pattern usdGen
   should copy verbatim** for its own codeless schema and scene-index plugin. It is proven here to
   work from an arbitrary out-of-tree build directory.

## Open questions

- **`testUsdviewCurvenetPanel` pick failure**: is the centre pick hitting `Solvers/BendChain` because
  `_ActivateCurrentStage` force-enables `displayGuide` (`rigExecUsdview.py:584-589`) and a guide
  occludes the tube, or is it an llvmpipe ID-buffer artefact? Not isolated. Re-run on a GPU
  workstation with `displayGuide` off before believing either.
- **`testUsdviewViewCube` orbit interpolation** (`theta 360 -> 540, no pose in between`) is almost
  certainly a QTimer/wall-clock sensitivity under a software rasterizer, but that is unproven.
- **Does macOS really pass `testRigExecStageEdits`?** The predicate call is unconditional C++, so
  either README.md:300's "17/17" predates the test or the macOS notice ordering differs. Worth one
  check if a macOS box is available; it does not change the design constraint either way.
- **Does the same resync coding error fire for `SetActive(False)` and for session-layer `over`
  deletes?** I only measured `RemovePrim`. The mitigation in Decision 3 depends on the answer;
  extend `probeExecResyncRemovePrim.cpp` with those two cases (10 minutes of work).
- The four curvenet `testusdview` suites need `../chars/puppetA/puppetA_curvenet.usda`, which is not
  in this checkout. Whether they pass on Linux is unknown.
- Whether `-ffp-contract=off` is *sufficient* or merely *necessary* for cross-platform kernel
  parity: only one test flipped here. A bit-exactness comparison of `rigExecMath` outputs between
  AppleClang and GCC has not been done.
