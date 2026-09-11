# usdGen build, dependencies and testing

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document defines how usdGen is compiled, what it depends on, how it is installed and
discovered, and what proves it works. It fixes the CMake target graph and the link rule that makes
`S8` ("no design element may require a `UsdStage` downstream of the stage scene index") a link error
rather than a review rule (gate **B-1**, `ADR §9.4 R37`); it pins the three vendored third-party
libraries with their licences and patch list; it owns the single registry of env vars and CMake
cache variables (`ADR §9.4 R35`) and the CTest name registry; and it defines the five test tiers
T0–T4, the harnesses that run them on this host, the golden-data policy and the determinism rules. Everything here is written so a new
engineer can clone an empty repository and reach a green `ctest` without asking anyone a question.

Reads with: `01-architecture.md` (§5.1 the target table, §5.2 the dependency rule),
`03-execution-engine.md` (kernel authoring rules, thread model, §9.3 configuration),
`06-imaging.md` (the four plugin registrations), `07-look-maps-expressions.md` (the glslfx and
SeExpr/Ptex surfaces this document vendors, and gates L-2…L-5), `08-tools.md` (§1.4 the C ABI,
§9 the Python and `testusdview` suites), `09-performance-and-benchmarks.md` (§5, the gate registry
this document schedules), `11-roadmap.md` (milestones M0–M8, pre-work PW-1…PW-13),
`appendix-A-evidence-ledger.md`, `appendix-B-prototype-inventory.md`.

---

## 0. Host and toolchain facts

Everything in this section was checked on this machine on 2026-09-04. `research/ENVIRONMENT.md`'s
CORRECTIONS block overrides its own body; where the two disagree the CORRECTIONS win.

**Number tags.** Every number below carries MEASURED (with its ledger row in
`appendix-A-evidence-ledger.md`), DERIVED from a ledger row (scaled or interpolated — never
MEASURED), UNMEASURED (with the gate that will measure it) or ASSUMPTION (`ADR §9.5 R42`). Ledger
rows are cited as **`EV-nnn`** and by nothing else (`ADR §9.1 R1`, `ADR §9.5 R43`); the retired
`A2.n-Xn` handles are mapped to their `EV-` ids in `appendix-A-evidence-ledger.md` §2.0.

### 0.1 Machine and toolchain

| Fact | Value | Source |
|---|---|---|
| OS / arch | Linux 6.17, aarch64, 20 CPUs = 10× Cortex-X925 + 10× Cortex-A725, heterogeneous (MEASURED) | `appendix-A-evidence-ledger.md` §1.1; `research/G-data-plane-engine-prototype-benchmark.md:27-30` (`lscpu`) |
| GPU | NVIDIA GB10, driver 580.173.02, GL 4.6 compatibility profile through EGL (MEASURED) | `research/G-storm-hair-look-prototype.md` §0, §5 |
| Compiler | GCC 13.3.0 — the compiler the OpenUSD install was built with | `research/A8-seexpr-ptex-libs.md` key facts; `g++ --version` |
| CMake / Ninja | 3.28.3; ninja 1.13.2 **only** at `/home/burkard/.venv/bin/ninja` (pip, not on `PATH`) | `research/ENVIRONMENT.md` CORRECTIONS; `research/B-usdrig-build.md` §1 |
| bison / flex / sed / zlib | bison 3.8.2, flex 2.6.4, sed, zlib 1.3 with headers | `research/A8-seexpr-ptex-libs.md` key facts |
| Privileges | **no sudo**; `/usr/bin/Xorg` exists but cannot be started; **no Xvfb installed** | `research/ENVIRONMENT.md` "Hardware / OS" |
| Network | reachable (`pip`, `apt-get download` both work) | `research/B-usdrig-build.md` §1 |

`-ffp-contract` defaults to `fast` on this GCC (MEASURED, re-verified 2026-09-05:
`g++ -Q --help=optimizers` prints `fast`), which is the root cause of §8.1.

### 0.2 OpenUSD

| Fact | Value |
|---|---|
| Source mirror (read-only, for grepping) | `/home/burkard/work/OpenUSD`, tag v26.08 (`ee47c679a`) |
| Install prefix | `/home/burkard/work/OpenUSD_26_08` (`include/pxr`, `lib`, `bin`) |
| Plugins | `lib/usd/<name>/resources` and `plugin/usd/<name>/resources` |
| Python bindings | `lib/python3.12/site-packages` — **not** `lib/python` |
| Bundled third party | MaterialX 1.39.5, OpenSubdiv 3.6.1, oneTBB 2020.3.1 (interface 11103) |
| Absent from the install | Ptex, OpenImageIO, SeExpr; `PXR_ENABLE_PTEX_SUPPORT` OFF (`cmake/defaults/Options.cmake:36`); no tif, no `.tx` in Hio |
| Hgi backends built | **`hgiGL` only** — no `hgiVulkan`, no `hgiMetal` |
| pxr_boost | imported target name is plain **`boost`** (`cmake/pxrTargets.cmake:57`), library `libusd_boost.so` |

Two link facts this document leans on, verified in the install's export file: `hio`'s interface is
`arch;js;plug;tf;vt;trace;ar;hf` (`cmake/pxrTargets.cmake:483-486`) and `pxOsd`'s is
`tf;gf;vt;OpenSubdiv::osdCPU` (`:499-502`). Neither pulls `usd`, `usdImaging` or `hd`, which is what
makes the §1.3 link rule achievable while still allowing image reads and subdivision refinement
inside the evaluator.

`plugin/usd/plugInfo.json` in the install is `{"Includes": ["*/resources/"]}`, and Plug anchors its
compiled-in `PXR_PLUGIN_BUILD_LOCATION` relative to `libusd_plug.so`
(`pxr/base/plug/initConfig.cpp:25-26, 33-62`). A site may therefore drop usdGen's resource
directories into `<USD>/plugin/usd/` and need no `PXR_PLUGINPATH_NAME` at all (§3.4).

### 0.3 Python

`/home/burkard/.venv/bin/python3` is 3.12.3, with `numpy` 2.5.2, `ninja` 1.13.2, `PySide6` 6.11.2
and PyOpenGL (`research/ENVIRONMENT.md` CORRECTIONS); headers at `/usr/include/python3.12/Python.h`.
usdGen's extension module is **pxr_boost**, not pybind11 (`S39`), so `pybind11` — present because
usdRig uses it — is not a usdGen build dependency.

### 0.4 What runs headlessly here

| Capability | How | Evidence |
|---|---|---|
| Headless C++ against the install | `find_package(pxr CONFIG PATHS /home/burkard/work/OpenUSD_26_08)` | `research/ENVIRONMENT.md` |
| Full `UsdImagingCreateSceneIndices` chain, notices, `GetPrim` pulls, **no GL** | plain executable — `probeImagingPipeline`, 36 assertions in 0.07 s (MEASURED, `appendix-A-evidence-ledger.md` §2.10) | `research/B-usdrig-build.md` decision 4 |
| **Storm on the GB10 GPU** | EGL device-platform context: `EGL_EXT_platform_device`, **compatibility** profile, 64×64 pbuffer, `eglMakeCurrent(d, surf, surf, ctx)`; entry points `dlopen`ed because no EGL headers exist here | `research/G-storm-hair-look-prototype.md` §0; `prototypes/storm-hair-look/eglctx.h` |
| `usdview`, `testusdview`, `usdrecord --renderer GL` | user-space Xvfb, Mesa **llvmpipe** (LLVM 20.1.2), GL 4.5 core, unpacked with `dpkg-deb -x` into a scratch directory (no root) | `research/B-usdrig-build.md` §6; `research/ENVIRONMENT.md` CORRECTIONS |

`usdrecord` itself core-dumps under EGL because its `main` creates a GLX window first; the EGL
harness is our own driver around `UsdAppUtilsFrameRecorder`.

### 0.5 What no measurement on this host may claim

llvmpipe frame times are **CPU numbers** and are never quoted as Storm numbers
(`research/B-usdrig-build.md` §6), so tier 3 asserts behaviour and never milliseconds; GPU-accelerated
GL through Xvfb is impossible (zink needs DRI3). Metal and Vulkan are unreachable —
`HDST_ENABLE_HGI_RESOURCE_GENERATION=1` on GL is gate S-8's proxy check, not proof. There is no
RenderMan install, so hdPrman parity (R-1) is a tier-4 protocol, and with one GPU and one driver so
is AMD/Intel shader compilation.

### 0.6 The reference build cost

usdRig, the sibling project usdGen mirrors, builds here as (all MEASURED,
`appendix-A-evidence-ledger.md` §2.10 rows `EV-076`/`EV-077`/`EV-080`;
`research/B-usdrig-build.md` §2):

| Step | Cost |
|---|---|
| cold `cmake` configure | 0.83 s |
| clean `ninja -j16`, 64 edges | 21.45 s, maxRSS 2.10 GB, 0 errors, 36 `[-Wcpp]` warnings |
| `ctest -j8`, 27 tests | 1.24 s (26 of 27 pass, 1 fail — §8.2, row `EV-080`) |
| incremental: all of `rigExecImaging` + relink dependents | **10.05 s** (`EV-077`) |
| incremental: one TU in `rigExec` | **21.4 s** (`EV-077`), dominated by pybind11's `-flto=auto` relink |
| `cmake --install` | < 1 s, 6.1 MB prefix |

Those last two numbers are why §1's target boundaries are drawn where they are, and why `_usdGen`
is built without LTO.

---

## 1. Repository layout and the CMake target graph

usdGen is a **sibling CMake project** at `/home/burkard/work/usdGen`, consuming the unmodified
OpenUSD install and optionally `find_package(rigExec CONFIG)` (`S44`; proven end to end by
`prototypes/usdrig-linux-build/consumer/`, `research/B-usdrig-build.md` §8).

### 1.1 Directory layout

```
usdGen/
  CMakeLists.txt  LICENSE  NOTICE  README.md
  bin/            _env.sh  build_usdgen.sh  gen_schema.sh  run_python_tests.sh
                  run_testusdview_usdgen_<x>.sh  usdview.sh  xvfb.sh  regen_goldens.sh
  cmake/          usdGenConfig.cmake.in  usdGenPlugin.cmake  usdGenThirdParty.cmake
                  CheckNoStageLink.cmake  CheckNoStageInclude.cmake
                  CheckNoThirdPartyExports.cmake
  libs/
    usdGenMath/     kernels; no USD stage types, no Hydra
    usdGen/         graph, scheduler, operators, maps, expressions, motion cache
    usdGenImaging/  scene indices, registry/session, adapters, publishers, the C ABI
    usdGenSchema/   schema.usda (the single source of the codeless domain) + the minimal
                    extent-registration library of ADR §9.2 R19
  plugin/
    usdGenSchema/resources/    plugInfo.json + generatedSchema.usda (checked in, generated)
    usdGenImaging/resources/   plugInfo.json.in
    usdGenShaders/resources/   plugInfo.json.in, shaders/{shaderDefs.usda, *.glslfx,
                               usdGenHair.mtlx}
  python/         _usdGen.cpp (pxr_boost)
                  usdgen/{__init__.py, usdGenLib.py, usdGenUsdview.py, usdGenUndo.py,
                          usdGenGraphModel.py, usdGenBrushMath.py, usdGenPick.py,
                          usdGenFreezeAuthor.py, usdGen*UI.py, builder.py, migrate.py,
                          plugInfo.json}
  testutils/      usdGenTestUtils:   sampler.h  counters.h  graphDescBuilder.h
                  usdGenTestUtilsHd: eglctx.h  sceneFixture.h  recordingObserver.h  imageDiff.h
  tests/          *.cpp  python/  perf/  scenes/  golden/
  thirdparty/     seexpr/  ptex/  nanoflann/     (offline FetchContent fallback, §2.2)
  docs/           <feature>.md + superpowers/{specs,plans}/  workstation-protocol.md
```

The Python package name is lowercase **`usdgen`** (`ADR §9.1 R5`), mirroring `rigexec`; `usdGenPy`
is not a name. `usdGenUsdview` remains a **CMake target** (`ADR §6 build`) that installs the
`"Type": "python"` plugInfo registering `usdgen.usdGenUsdview.UsdGenUsdviewContainer`
(`08-tools.md` §1.1).

### 1.2 Targets

`ADR §6 build` as amended by `ADR §9` is binding on the target names. Kind, install status, links:

| Target | Kind | Installed | pxr libs | Other links | Contents |
|---|---|---|---|---|---|
| `usdGenMath` | STATIC, PIC, hidden, `-ffp-contract=${USDGEN_FP_CONTRACT}` (default `off`) | no (headers not installed before M7) | `arch tf gf vt work` | `TBB::tbb`, `nanoflann`, `usdGen_seexpr`, optional `rigExec::rigExecMath` | SoA styler kernels, arc-length resample, RMF frames, ribbon transport, **the extent kernel body**, 257-entry ramp LUTs, noise façade, Morton codes, kd-tree façade, Poisson relax, barycentric scatter |
| `usdGen_seexpr` | STATIC, hidden, PIC | **never** | — | `dl`, `pthread` | vendored wdas/SeExpr `main`@`8f8c8f2`; **M0 = `Noise.h`/`Noise.cpp` only**, the interpreter sources land at M4 (`S38`, §1.6) |
| `usdGen_ptex` | STATIC, hidden, PIC | **never** | — | `ZLIB::ZLIB`, `Threads::Threads` | vendored Ptex v2.4.3 (`S38`); the ten `src/ptex/*.cpp` sources compiled by usdGen's own CMake (§2.4 TP-5) |
| `nanoflann` | INTERFACE | no | — | — | pinned 1.12.1 header |
| `usdGen` | SHARED | yes, `lib/` | `arch tf gf vt sdf ts ar work trace js plug hio pxOsd` | `usdGenMath usdGen_seexpr usdGen_ptex TBB::tbb` | `UsdGenGraph`, `UsdGenOp` + internal registry, `UsdGenCurveBuffer`, chunk/tile geometry, digests, scheduler, capture caches, operators, maps, `UsdGenMotionCache`, `UsdGenStats` |
| `usdGenImaging` | SHARED | yes, `lib/` | `hd hdsi hf usd usdGeom usdShade sdr usdImaging sdf tf gf vt trace pxOsd` | `usdGen`, `usdGenSchema` | `UsdGenImagingRegistry`, `UsdGenGroomSceneIndex`, the private pruning wrapper, generation store, publishers, the prim + `UsdGenRestAPI` adapters, `UsdGenMetadataSceneIndexPlugin`, `UsdGenShadersDiscoveryPlugin`, the `extern "C"` ABI (`08-tools.md` §1.4) |
| `usdGenSchema` | SHARED, **minimal** | yes, `lib/` + `lib/usd/usdGenSchema/resources` | `tf usd usdGeom` | `usdGenMath` (a static archive, so the linker pulls only the extent object — no SeExpr, no TBB) | schema tokens; one `TF_REGISTRY_FUNCTION(UsdGeomBoundable)` calling `UsdGeomRegisterComputeExtentFunction(TfType::FindByName("UsdGenDescription"), &UsdGenSchema_ComputeDescriptionExtent)`; the live-generation provider hook (§4.3); the codeless `generatedSchema.usda` + generated `plugInfo.json`. **No generated schema C++ classes** (`S9`, `ADR §9.2 R19`) |
| `usdGenShaders` | plugin resources, `"Type": "library"` (§4.4) | yes, `lib/usd/usdGenShaders/resources` | — | — | `usdGenHairPreview.glslfx` (variant A), `usdGenHairPreviewPrimvar.glslfx` (variant B, `ADR §5.4`), `usdGenHairPreviewTranslucent.glslfx`, `shaderDefs.usda`, `usdGenHair.mtlx` |
| `_usdGen` | MODULE (pxr_boost.python), **no LTO** | yes, `lib/python/usdgen/` | `boost tf gf vt` | `usdGenImaging`, `Python3::Module` | the array surface of `S39` |
| `usdgen` | python package | yes, `lib/python/usdgen/` | — | — | the modules of `08-tools.md` §1.2 plus `builder.py` (the `rigexec.Builder` facade) and `migrate.py` (`02-schema.md` §8.6) |
| `usdGenUsdview` | plugin resources, `"Type": "python"` | yes, `lib/python/usdgen/plugInfo.json` | — | — | registers `usdgen.usdGenUsdview.UsdGenUsdviewContainer` (`08-tools.md` §1.1) |
| `usdGenTestUtils` | STATIC | no | — | `usdGen` | `sampler.h`, `counters.h`, synthetic `UsdGenGraphDesc` builders — **T0 only**; links no `usd`/`hd` and is itself covered by gate B-1 |
| `usdGenTestUtilsHd` | STATIC | no | `hd hdsi usd usdImaging usdImagingGL hdSt hgiGL usdAppUtils` | `usdGenImaging`, `usdGenTestUtils`, EGL via `dlopen` | `sceneFixture.h`, `recordingObserver.h`, `imageDiff.h`, `eglctx.h` — **T1/T2/T3 only** |

The test-utility split is not cosmetic: one library that linked Hydra would drag the whole
Hydra/GL stack into every tier-0 executable and make §5.1's "Needs: nothing" false.

`usdGenMath` **links** `usdGen_seexpr` rather than merely including its headers: SeExpr's
`Noise.cpp` carries explicit template instantiations (`src/SeExpr2/Noise.cpp:219-234`) and only for
`double`. usdGenMath calls the `double` instantiations and narrows once at the buffer boundary.
Adding `float` instantiations is a measured option at M3, not a v1 decision (UNMEASURED; decided by
the E-1 chain-throughput number with and without).

### 1.3 The dependency rule, stated exactly

`usdGen` (the evaluator) links these pxr libraries **directly**:

```
arch  js  plug  tf  gf  vt  sdf  ts  ar  work  trace  hio  pxOsd
```

and acquires `hf`, `pegtl`, `python` and `Python3::Python` **transitively**: `hio` brings `hf`
(`cmake/pxrTargets.cmake:483-486`) and `sdf` brings `ts;pegtl;python;Python3::Python`
(`:176-179`, verified `objdump -p lib/libusd_sdf.so` → `NEEDED libusd_ts.so`, `libusd_python.so`,
`libpython3.12.so.1.0`). `pxOsd` additionally brings `OpenSubdiv::osdCPU` (`:499-502`). No usdGen
code includes an `hf`, `pegtl` or `python` header.

`sdf` is linked for `SdfPath` and `SdfAssetPath` as **values**, not stage access; `ts` for the
whole-`TsSpline` transport on `float <p>:spline` (`ADR §9.2 R11`); `hio` for image reads in map
evaluation; `pxOsd` for `Far::PtexIndices` face-id mapping (`research/A8-seexpr-ptex-libs.md` §2.6).

Forbidden in `usdGen`, `usdGenMath`, `usdGenTestUtils`, `usdGen_seexpr` and `usdGen_ptex`, as a
`NEEDED` entry and as an `#include`, **by prefix family** (`ADR §9.4 R37`) rather than by
enumeration — an enumeration is always narrower than the rule:

* any pxr library whose name matches `^usd` — `usd`, `usdGeom`, `usdShade`, `usdSkel`, `usdLux`,
  `usdRender`, `usdUtils`, `usdImaging*`, `usdSkelImaging`, `usdAppUtils`, `usdviewq`, …;
* `^hd` — `hd`, `hdsi`, `hdSt`, `hdx`, `hdGp`, `hdMtlx`;
* `^hgi` — `hgi`, `hgiGL`, `hgiInterop`;
* plus `glf` and `garch`.

`hio` and `hf` are allowed and do not match any family. The rule is a **prohibition, not an
allow-list**: a transitive dependency of an allowed library is allowed, and only these families are
enforced.

The rule exists because `S8` forbids any design element from requiring a `UsdStage` downstream of the
stage scene index, and `ADR §4.2.3` makes `UsdGenGraphDesc` — a pure-value description built by
`usdGenImaging` from data sources — the engine's only input. A **link error** is the cheapest
enforcement available (`design/judge-evidence.md` §1 D8: "the single best build idea in the three
documents"), and it is what lets tier-0 tests run with neither Hydra nor a stage.

### 1.4 Enforcing the rule — gate B-1

Three tests, all tier 0, all in the `build` CTest label; the `usdGen` link check and the include
check carry `gate:B-1` (`ADR §9.1 R2`, `ADR §9.4 R37`):

```cmake
# One regex, two tests. A family added here is enforced in both places with no second edit.
set(USDGEN_FORBIDDEN_LIB_REGEX "^libusd_(usd|hd|hgi|glf|garch)")
set(USDGEN_FORBIDDEN_INC_REGEX "pxr/(usd|imaging|usdImaging)/(usd|hd|hgi|glf|garch)[A-Za-z0-9_]*/")
foreach(_t usdGen usdGenMath usdGenTestUtils)
  add_test(NAME testUsdGenLinkRule_${_t} COMMAND ${CMAKE_COMMAND}
      -DTARGET_FILE=$<TARGET_FILE:${_t}> -DOBJDUMP=${CMAKE_OBJDUMP}
      -DFORBIDDEN=${USDGEN_FORBIDDEN_LIB_REGEX}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoStageLink.cmake)
endforeach()
add_test(NAME testUsdGenIncludeRule COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DFORBIDDEN=${USDGEN_FORBIDDEN_INC_REGEX}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckNoStageInclude.cmake)
set_tests_properties(testUsdGenLinkRule_usdGen testUsdGenIncludeRule
                     PROPERTIES LABELS "T0;build;gate:B-1")
```

`CheckNoStageLink.cmake` runs `${OBJDUMP} -p ${TARGET_FILE}` and fails on a matching `DT_NEEDED`
entry; a static library has no such list, so for `usdGenMath` and `usdGenTestUtils` it runs `nm -u`
and matches undefined symbols against the forbidden families' `pxrInternal` namespaces.
`CheckNoStageInclude.cmake` greps `libs/usdGen*/**.{h,cpp}` and `testutils/`, excluding
`libs/usdGenImaging/` and `testutils/usdGenTestUtilsHd/`. The include test matters because a
header-only misuse (`UsdTimeCode`, `pxr/usd/usdGeom/tokens.h`) produces no `NEEDED` entry at all.
`testUsdGenNoThirdPartyExports` runs `nm -D --defined-only $<TARGET_FILE:usdGen>` and fails if a
symbol demangles into `SeExpr2::` or `Ptex`; what keeps it green is `-Wl,--exclude-libs,ALL` plus
`CXX_VISIBILITY_PRESET hidden` on both vendored statics (§2.3).

### 1.5 Why this split, and what it costs

The boundaries follow the **rebuild cost of the inner loop**, not conceptual tidiness
(`design/proposal-performance.md` §3.1, from §0.6): kernels are a static library with a stable header
so a kernel edit relinks two shared objects; the scene index is its own shared library because that
is the loop an imaging engineer runs (10.05 s, MEASURED, `EV-077`); and the Python module is built
with `INTERPROCEDURAL_OPTIMIZATION OFF` and no `-flto`, because the reference project's 21.4 s
single-TU rebuild was **dominated by** the LTO relink of its Python extension (MEASURED,
`EV-077`). The exact LTO share is UNMEASURED and is one of the M0 build-budget numbers below.

Budgets for usdGen itself (UNMEASURED; first measured at M0 and thereafter a CI timing assertion,
§6.3): clean build ≤ 90 s at `-j16`; incremental `usdGenImaging` ≤ 15 s; incremental one TU in
`usdGenMath` ≤ 20 s; configure ≤ 3 s cold with third-party sources already present.

### 1.6 Which target lands in which milestone

Target **names and directory layout are fixed at M0** so nothing moves later; the targets themselves
appear when their first consumer does (`design/proposal-risk.md` §2 on the ADR's milestones):

| Milestone | Targets added |
|---|---|
| M0 | `usdGenMath`, `usdGen_seexpr` (**noise only** — `Noise.cpp` + its headers), `usdGen`, `usdGenImaging`, `usdGenSchema`, `usdGenTestUtils`, `usdGenTestUtilsHd`, `nanoflann` |
| M1 | `usdGenShaders` |
| M4 | `usdGen_ptex`; the SeExpr **interpreter** sources added to `usdGen_seexpr` (bison/flex outputs, `ExprBuiltins.cpp`, `rand()`) |
| M5 | `_usdGen`, `usdgen`, `usdGenUsdview` |
| M7 | installed headers for `usdGen`/`usdGenImaging` (engine headers are deliberately not installed and may churn until M7, `ADR §3`; this is **not** contract C4, which froze at the end of M5) |

`usdGen_seexpr` lands at M0 because `UsdGenNoise` ships in M1 (`ADR §7`) and `S38` makes SeExpr's
`Noise.h`/`Noise.cpp` the **single** noise implementation for expressions and C++ stylers; only the
interpreter half waits for M4, when `UsdGenExprMap` needs it. `UsdGenPtexMap` is M4
(`ADR §9.5 R38`), so `usdGen_ptex` is too.

---

## 2. Third-party dependencies

### 2.1 What is vendored, and under what licence

| Component | Pin | Licence | Copyleft | Link form | Installed? |
|---|---|---|---|---|---|
| wdas/SeExpr | branch `main` @ `8f8c8f2` (2026-01-27) | Apache-2.0 with §6 Trademarks replaced (no Disney endorsement) | no | STATIC + hidden inside `libusdGen.so` | never |
| Ptex | `v2.4.3` (2024-06-11) | BSD-3-Clause, Disney no-endorsement variant | no | STATIC + hidden | never |
| nanoflann | `1.12.1` | BSD-2-Clause | no | header-only INTERFACE | headers never installed |

Licence evidence: `research/A8-seexpr-ptex-libs.md` §7 (SeExpr `LICENSE:1-11`, Ptex `LICENSE:1-8`,
nanoflann `COPYING`). All three are permissive; the two Disney trademark clauses require a `NOTICE`
entry, which usdGen ships at the repository root and installs to `share/usdGen/NOTICE`.
**KSeExpr is GPL-3.0-or-later and is not usable** (`research/A8-seexpr-ptex-libs.md` §1.9).

Ptex is pinned at 2.4.3, not 2.5.x, because 2.5.0 switched zlib → libdeflate and libdeflate headers
are absent here (`research/A8-seexpr-ptex-libs.md` key facts). An *installed* Ptex 2.4.3 exports
`Ptex::Ptex_static` in `lib/cmake/Ptex/ptex-config.cmake` (same report §2.2) — the package a
Ptex-enabled `hdSt` would find, which is why usdGen must never install one (§2.3). That name exists
only in the installed export; in a build the target is the bare `Ptex_static` and Ptex declares no
`ALIAS`. usdGen reuses neither name (§2.4 TP-5).

Not vendored, deliberately: TBB (use `TBB::tbb` from `pxrConfig`, oneTBB 2020.3 interface 11103 —
never let FetchContent pull oneTBB 2021, `research/A8-seexpr-ptex-libs.md` §6.2 note 4), MaterialX
and OpenSubdiv (from the install), zlib (system), and image IO (Hio, `S38`).

### 2.2 FetchContent with an offline fallback

```cmake
include(FetchContent)
FetchContent_Declare(seexpr
    GIT_REPOSITORY https://github.com/wdas/SeExpr.git
    GIT_TAG        8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc
    SOURCE_SUBDIR  cmake-not-used)          # download only; we never add_subdirectory it
FetchContent_Declare(ptex
    GIT_REPOSITORY https://github.com/wdas/ptex.git GIT_TAG v2.4.3
    SOURCE_SUBDIR  cmake-not-used)
FetchContent_Declare(nanoflann
    GIT_REPOSITORY https://github.com/jlblancoc/nanoflann.git GIT_TAG v1.12.1
    SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(seexpr ptex nanoflann)   # populate only, never configured
```

`SOURCE_SUBDIR` naming a directory with no `CMakeLists.txt` makes `FetchContent_MakeAvailable`
populate the tree and skip `add_subdirectory`, which is how usdGen avoids upstream's build entirely —
for SeExpr (§2.4 TP-1, TP-2) and Ptex (TP-5, TP-8) alike — while still getting a pinned,
hash-checked source tree. **No** vendored tree is configured by CMake, so `PTEX_BUILD_*` is never set
and Ptex's own `install()` rules never run inside usdGen's install (§2.3).

Offline builds set `FETCHCONTENT_SOURCE_DIR_SEEXPR`, `FETCHCONTENT_SOURCE_DIR_PTEX` and
`FETCHCONTENT_SOURCE_DIR_NANOFLANN` to the checked-in `thirdparty/<name>` trees;
`bin/build_usdgen.sh` sets all three by default and adds `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`, so
**the default build never touches the network**. `USDGEN_USE_SYSTEM_SEEXPR`, `USDGEN_USE_SYSTEM_PTEX`
and `USDGEN_USE_SYSTEM_NANOFLANN` exist for packagers and switch to `find_package`
(`research/A8-seexpr-ptex-libs.md` §6.2 note 1). `testUsdGenOfflineConfigure` (T0) configures a
scratch build with the network denied and asserts success.

### 2.3 Static, hidden, never installed

```cmake
set_target_properties(usdGen_seexpr usdGen_ptex PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_options(usdGen PRIVATE "LINKER:--exclude-libs,ALL")
```

The rule is `research/A8-seexpr-ptex-libs.md` §6.2 note 2: a `libPtex.so` in usdGen's `lib/` next to
USD's would be picked up by the `$ORIGIN` rpath of a site's Ptex-enabled `libusd_hdSt.so`, an ABI
hazard with no upside. What makes "never installed" assertable is that **no vendored tree is ever
`add_subdirectory`'d** (§2.2): Ptex's own CMake would otherwise run, inside usdGen's install,
`install(TARGETS Ptex_static EXPORT Ptex …)` (`src/ptex/CMakeLists.txt:27`), its seven public headers
(`:46-55`), `install(EXPORT Ptex …)` plus `ptex-config.cmake` (`src/build/CMakeLists.txt:14, 18`) and
`ptxinfo` (`src/utils/CMakeLists.txt:9`) — `PTEX_BUILD_SHARED_LIBS OFF` suppresses only
`libPtex.so`, none of those.
`testUsdGenInstallTree` (T0) installs into a scratch prefix and fails if `lib/libPtex*`,
`lib/libSeExpr*`, `lib/cmake/Ptex/`, `bin/ptxinfo`, `include/Ptexture.h`, `include/SeExpr2/` or
`include/nanoflann*` exist. It also resolves the installed `LibraryPath` of every shipped
`plugInfo.json` relative to that file's `Root` and `stat`s the result (§3.3).

### 2.4 Patch list (TP-1…TP-8)

Nothing in the vendored sources is edited. The list is what usdGen's own CMake does *instead* of
using upstream's build. The ids are `TP-n` ("third-party patch") because `P0/P1/P2` are the motion
profiles (`ADR §9.1 R1`).

| # | Item | What we do |
|---|---|---|
| TP-1 | SeExpr hardcodes `add_library(SeExpr2 SHARED …)` on non-Windows (`src/SeExpr2/CMakeLists.txt:85-86`); we need STATIC + hidden | never `add_subdirectory` SeExpr; declare `usdGen_seexpr` STATIC over an explicit source list from `${seexpr_SOURCE_DIR}/src/SeExpr2/`, minus `ExprLLVMCodeGeneration.cpp`. At M0 that list is `Noise.cpp` alone (§1.6) |
| TP-2 | SeExpr's parser rules `tee` generated files back into the source tree (`:53, 67`); an in-source write breaks a read-only source dir and reproducible builds | at M4 run `find_package(BISON)`/`find_package(FLEX)` ourselves, generate into `${CMAKE_CURRENT_BINARY_DIR}`, apply the same `sed` symbol renames (`yy`→`SeExpr2`, `YY`→`SeExprYY`) |
| TP-3 | bison/flex are hard requirements on POSIX; `src/SeExpr2/generated/` is empty in git | fall back to the pre-generated sources upstream ships at `windows7/SeExpr/generated/` (verified present in the clone); document `apt install bison flex` |
| TP-4 | `ENABLE_SSE4` defaults TRUE upstream and adds `-msse4.1` (`CMakeLists.txt:98, 202`); this is aarch64 | not carried — we compile the sources ourselves and never set it |
| TP-5 | Ptex 2.4.3 defaults to C++98 (`CMakeLists.txt:11-19`) and its build installs a static library, seven headers, a `Ptex::` package and `ptxinfo` (§2.3) | never `add_subdirectory` Ptex; declare `usdGen_ptex` STATIC over the ten sources in `${ptex_SOURCE_DIR}/src/ptex/` (`PtexCache PtexFilters PtexHalf PtexReader PtexSeparableFilter PtexSeparableKernel PtexTriangleFilter PtexTriangleKernel PtexUtils PtexWriter`), `target_compile_definitions(usdGen_ptex PUBLIC PTEX_STATIC)`, `target_link_libraries(usdGen_ptex PRIVATE ZLIB::ZLIB Threads::Threads)`; usdGen calls `find_package(ZLIB/Threads REQUIRED)` itself (§3.1) because Ptex's top-level CMake, which normally provides them (`CMakeLists.txt:34, 36`), never runs |
| TP-6 | `rand()` is not a SeExpr2 builtin (`ExprBuiltins.cpp:1719-1849` registers `hash`, not `rand`) although XGen users expect it (`S38`) | added **on usdGen's side** through `Expression::resolveFunc` as an `ExprFuncSimple` implemented on `hash` for determinism (`design/proposal-risk.md` §7.3) — no vendor patch |
| TP-7 | SeExpr's Windows build is STATIC already; Ptex builds fine | Windows is deferred (§10); TP-3's fallback is the Windows path |
| TP-8 | Ptex configure-generates a header **into its own source tree** (`src/ptex/CMakeLists.txt:1-2`), dirtying the checked-in `thirdparty/ptex` fallback tree so `bin/regen_goldens.sh`'s clean-tree check refuses (§5.4) | we never run Ptex's CMake (TP-5); usdGen runs `configure_file(… PtexVersion.h.in ${CMAKE_CURRENT_BINARY_DIR}/ptex/PtexVersion.h @ONLY)` itself and puts that dir first on the include path. `.gitignore` carries both generated paths as belt and braces |

### 2.5 NOTICE and licence audit

`NOTICE` lists OpenUSD (Tomorrow Open Source Technology License 1.0), OpenSubdiv, MaterialX, oneTBB,
SeExpr, Ptex, nanoflann, zlib and stb with their SPDX identifiers and the two Disney trademark
clauses verbatim. `testUsdGenNotice` (T0) asserts every entry in `cmake/usdGenThirdParty.cmake`'s pin
table has a matching stanza, so a dependency cannot be added without a licence line.

---

## 3. CMake specifics

### 3.1 Top-level skeleton

```cmake
cmake_minimum_required(VERSION 3.26)
project(usdGen VERSION 0.1.0 LANGUAGES C CXX)   # C for zlib/stb inside the vendored trees
set(CMAKE_CXX_STANDARD 17)                      # must match the install (GCC 13, libstdc++ ABI)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(USD_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../OpenUSD_26_08" CACHE PATH "")
list(APPEND CMAKE_PREFIX_PATH "${USD_INSTALL_DIR}")  # pxrConfig's find_dependency() ignores PATHS
set(USDGEN_FP_CONTRACT off CACHE STRING "off|fast -- usdGenMath kernels only (8.1)")

option(USDGEN_WITH_RIGEXEC "Link rigExec::rigExecMath" ON)
set(RIGEXEC_INSTALL_DIR "" CACHE PATH "rigExec install prefix")
if (USDGEN_WITH_RIGEXEC)
    # FIRST: it brings pxr in transitively (S44). PATHS is required -- CMAKE_PREFIX_PATH
    # holds only USD_INSTALL_DIR at this point, so a bare call never finds rigExec.
    find_package(rigExec CONFIG QUIET PATHS "${RIGEXEC_INSTALL_DIR}")
endif()
if (NOT TARGET usd)                             # the CMake 3.28 double-pxrConfig trap (3.2)
    find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)
endif()
find_package(ZLIB REQUIRED)      # for usdGen_ptex; Ptex's own CMake (which finds them) never runs
find_package(Threads REQUIRED)   # -- see 2.4 TP-5
set(CMAKE_INSTALL_RPATH "$ORIGIN")               # "@loader_path" + INSTALL_NAME_DIR on APPLE
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

add_library(usdGenMath STATIC ${USDGEN_MATH_SOURCES})
target_compile_options(usdGenMath PRIVATE -ffp-contract=${USDGEN_FP_CONTRACT})   # 8.1
```

The flag is a cache option, not a literal, because the `build-fpfast` CI job (§6.2) has to *change*
it: a target-level `-ffp-contract=off` is emitted after `CMAKE_CXX_FLAGS`, and the last
`-ffp-contract` on the command line wins (MEASURED on this host, 2026-09-05:
`g++ -ffp-contract=fast -ffp-contract=off -Q --help=optimizers` → `off`; the reverse order → `fast`).
A global `-DCMAKE_CXX_FLAGS=-ffp-contract=fast` therefore cannot reach the kernels, and a global
`=off` would silently widen the rule beyond `usdGenMath`. The `target_compile_options` call must
follow `add_library(usdGenMath …)`; before it, the target does not exist and configure fails.

`-DRIGEXEC_INSTALL_DIR=<prefix>` is what the `with-rigexec` CI job passes
(`research/B-usdrig-build.md` §8). `USDGEN_WITH_RIGEXEC=OFF` **must** build and pass every non-rig
test — a CI job (§6.2) and risk-register item R9. If `rigExecMath` diverges, the four kernels usdGen
borrows (RMF, ribbon transport, extent, envelope) are copied into `usdGenMath`.

### 3.2 The double-`pxrConfig` guard

CMake 3.28's `find_dependency` short-circuits on a **call hash**, not on `<pkg>_FOUND`
(`/usr/share/cmake-3.28/Modules/CMakeFindDependencyMacro.cmake:57-83`, the hash and short-circuit at
`:58-59`). A consumer calling `find_package(pxr … NO_DEFAULT_PATH)` with a different signature than
`rigExecConfig.cmake`'s `find_dependency(pxr CONFIG PATHS …)` re-executes `pxrConfig.cmake`, whose
`add_library(TBB::tbb SHARED IMPORTED)` at **`pxrConfig.cmake:58`** is unconditional whenever
`PXR_FIND_TBB_IN_CONFIG` is OFF (the default here, verified `:52-58`); the configure dies with
`add_library cannot create imported target "TBB::tbb"` (`research/B-usdrig-build.md` §8). Three
defences ship: (1) `find_package(rigExec)` before any
`find_package(pxr)`, with the pxr call guarded by `if (NOT TARGET usd)`; (2)
`usdGenConfig.cmake.in` wraps **its own** pxr dependency in the same guard, so a downstream consumer
that also finds pxr does not detonate; (3) `-DPXR_FIND_TBB_IN_CONFIG=ON` documented as the escape
hatch. `testUsdGenConsumer` (T0), modelled on `prototypes/usdrig-linux-build/consumer/`, calls
**both** `find_package(usdGen CONFIG REQUIRED)` and `find_package(pxr CONFIG REQUIRED …)`, links
`usdGen::usdGenImaging`, compiles and runs — the only test that can prove defence (2).

`usdGenConfig.cmake` exports `usdGen::usdGen`, `usdGen::usdGenImaging`, `usdGen::usdGenSchema`, plus
`usdGen_PLUGINPATHS` (the three `lib/usd/*/resources` dirs plus `lib/python/usdgen`),
`usdGen_LIBRARY_DIR` and `usdGen_PYTHON_DIR`, with
`write_basic_package_version_file(… COMPATIBILITY SameMinorVersion)`. It records the OpenUSD version
it was built against — `PXR_VERSION` (`pxrConfig.cmake:17`, `"2608"` here) — compares it to the
consumer's and `message(FATAL_ERROR)`s on a mismatch (risk-register item R8).

### 3.3 The generated plugInfo pattern

Taken from usdRig, proven from an arbitrary out-of-tree build directory
(`research/B-usdrig-build.md` decision 7; `usdRig/CMakeLists.txt:410-460, 495-520`), with one
addition: the install-tree hop is computed, not hard-coded. A **library** plugin's `plugInfo.json`
names a platform-dependent filename that only `$<TARGET_FILE_NAME:>` knows, so the file is generated.
Plug anchors `LibraryPath` on the plugin's `Root`: a plugInfo keeping `"Root": ".."`,
`"ResourcePath": "resources"` needs two hops, one post-processed to `Root`/`ResourcePath` = `"."`
needs three (`usdRig/CMakeLists.txt:461-463`; `research/B-usdrig-build.md` §7).

```cmake
# cmake/usdGenPlugin.cmake -- one function, two generated copies per plugin.
# _hop is derived, never literal: it is what makes deployment (b) in 3.4 work.
file(RELATIVE_PATH _hop
     "${CMAKE_INSTALL_PREFIX}/${USDGEN_INSTALL_PLUGINDIR}/usdGenImaging"   # the plugin Root
     "${CMAKE_INSTALL_PREFIX}/${USDGEN_INSTALL_LIBDIR}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugin/usdGenImaging/resources/plugInfo.json.in" _c)
foreach(_v build install)      # build: <build>/usd/...   install: <build>/install/usd/...
    set(_lib "$<TARGET_FILE_NAME:usdGenImaging>")
    set(_pre "")
    if (_v STREQUAL install)
        set(_lib "${_hop}/$<TARGET_FILE_NAME:usdGenImaging>")
        set(_pre "install/")
    endif()
    string(REPLACE "@USDGEN_IMAGING_LIBRARY_FILENAME@" "${_lib}" _out "${_c}")
    file(GENERATE OUTPUT
        "${CMAKE_CURRENT_BINARY_DIR}/${_pre}usd/usdGenImaging/resources/plugInfo.json"
        CONTENT "${_out}")
endforeach()
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/install/usd/usdGenImaging/resources/plugInfo.json"
        DESTINATION "${USDGEN_INSTALL_PLUGINDIR}/usdGenImaging/resources")
```
The schema and shaders plugInfos go through the same function — the schema with `Root` = `"."`,
its own target `usdGenSchema` and therefore one hop more; the shaders with `"Root": ".."` and
`usdGenImaging`.

`testUsdGenInstallTree` (T0) asserts the result directly: for every installed `plugInfo.json` it
resolves `LibraryPath` against `Root` and `stat`s the file. Every source plugInfo is listed in
`CMAKE_CONFIGURE_DEPENDS` so that editing one re-runs configure; without that, a plugInfo edit builds
clean and then fails at stage open against a stale generated copy
(`usdRig/CMakeLists.txt:433-437`).

### 3.4 Install tree

```
<prefix>/
  lib/
    libusdGen.so  libusdGenImaging.so  libusdGenSchema.so
    usd/usdGenSchema/resources/{plugInfo.json, generatedSchema.usda}
    usd/usdGenImaging/resources/plugInfo.json
    usd/usdGenShaders/resources/{plugInfo.json,
        shaders/{shaderDefs.usda, usdGenHairPreview.glslfx,
                 usdGenHairPreviewPrimvar.glslfx, usdGenHairPreviewTranslucent.glslfx,
                 usdGenHair.mtlx}}
    python/usdgen/{__init__.py, usdGenLib.py, usdGenUsdview.py, …, builder.py, migrate.py,
                   plugInfo.json, _usdGen.cpython-312-aarch64-linux-gnu.so}
    cmake/usdGen/{usdGenConfig.cmake, usdGenConfigVersion.cmake, usdGenTargets.cmake}
  include/usdGen{,Math,Imaging}/…        # M7 onward only
  share/usdGen/NOTICE
```

Every shader resource sits **inside `shaders/`**, including `shaderDefs.usda`: the discovery plugin
resolves that directory through `PlugFindPluginResource` from the `"ShaderResources": "shaders"` key
and opens `shaders/shaderDefs.usda` (`07-look-maps-expressions.md` §2.4), and the defs reference
`@./usdGenHairPreview.glslfx@` — a path relative to the layer, so the glslfx files must be siblings
of the layer, not of the plugInfo.

Cache variables `USDGEN_INSTALL_LIBDIR` (`lib`), `USDGEN_INSTALL_PLUGINDIR` (`lib/usd`),
`USDGEN_INSTALL_PYTHONDIR` (`lib/python`), `USDGEN_INSTALL_CMAKEDIR` (`lib/cmake/usdGen`) mirror
usdRig (`usdRig/CMakeLists.txt:31-39`).

**Two supported deployments.** (a) *Beside* the USD install, the default: set `PXR_PLUGINPATH_NAME`
to `usdGen_PLUGINPATHS`. (b) *Into* the USD prefix: `-DUSDGEN_INSTALL_PLUGINDIR=plugin/usd
-DUSDGEN_INSTALL_INTO_USD=ON`, which makes `USDGEN_INSTALL_PYTHONDIR` default to
`USDGEN_USD_SITE_PACKAGES`, derived at configure time and never a literal `python3.12`. In that
layout Plug finds the resources with no env var, because `<USD>/plugin/usd/plugInfo.json` is
`{"Includes": ["*/resources/"]}` anchored relative to `libusd_plug.so`
(`pxr/base/plug/initConfig.cpp:25-26, 33-62`). Deployment (b) moves the plugin resources without
moving the libraries, which is why the `LibraryPath` hop is **derived** (§3.3): with a hard-coded
`../../` it would resolve to `<USD>/plugin/libusdGenSchema.so`, Plug would find the plugInfo, fail to
`dlopen` the library, and `implementsComputeExtent` would silently never register.

```cmake
file(GLOB _usd_site "${USD_INSTALL_DIR}/lib/python*/site-packages")
list(GET _usd_site 0 USDGEN_USD_SITE_PACKAGES)      # never hard-code python3.12
```

### 3.5 The variable registry

**This section is the single registry of usdGen's configuration variables (`ADR §9.4 R35`). Any
document introducing a runtime env var or a CMake cache variable adds it here; nothing else in the
plan may define one.** It has two parts: §3.5.1 the runtime env vars read by the shipped libraries
and scripts, and §3.5.2 the CMake cache variables that configure a build. Machine tuning is
env/config and is **never** authored into an asset (`ADR §2.3`).

#### 3.5.1 Runtime environment variables

| Variable | Default | Meaning | Defined by |
|---|---|---|---|
| `USDGEN_ENABLE` | `1` | scene-index plugin kill switch (`_IsEnabled`) | `06-imaging.md` |
| `USDGEN_CONTEXT` | unset (= interactive) | `render` selects `usdGen:renderDensityScale` | `ADR §2.3`; `03-execution-engine.md` §9.3 |
| `USDGEN_CHUNK_SIZE` | `512` | curves per chunk, clamped [128, 1024] | `03-execution-engine.md` §9.3 |
| `USDGEN_THREAD_LIMIT` | calibrated (8-thread knee) | private `tbb::task_arena` concurrency; set to skip calibration | `03-execution-engine.md` §9.3 |
| `USDGEN_MEMORY_BUDGET_MB` | `4096` | capture-cache eviction budget | `03-execution-engine.md` §9.3 |
| `USDGEN_STORM_MATERIAL_OVERRIDE` | `0` | synthesize `material_storm` and bind tiles to it; gate L-1 | `ADR §5.6`; `07-look-maps-expressions.md` |
| `USDGEN_DIAGNOSTICS` | `0` | `0` / `counters` / `trace` — `UsdGenStats` atomic counters, then the trace scopes of §7.2; replaces the dropped authored `usdGen:diagnostics` (`ADR §9.2 R8`) | this document |
| `USDGEN_IMAGE_CACHE_MB` | `512` | decoded `HioImage` plane cache; ASSUMPTION, settled by gate L-4 | `07-look-maps-expressions.md` §5.4 |
| `USDGEN_PTEX_CACHE_MB` | `256` | `PtexCache` size; ASSUMPTION, settled by gate L-4 | `07-look-maps-expressions.md` §6.4 |
| `USDGEN_PTEX_MAX_FILES` | `32` | `PtexCache` open-file ceiling; ASSUMPTION, settled by gate L-4 | `07-look-maps-expressions.md` §6.4 |
| `USDGEN_OP_CHECKS` | `0` | debug-build operator contract wrapper (never compiled into Release) | `03-execution-engine.md` §8.1; `04-operators.md` §1 |
| `USDGEN_IMAGING_DLL` | unset | absolute path to `libusdGenImaging.so` for the ctypes surface | `08-tools.md` §1.4 |
| `USDGEN_XVFB_ROOT` | **none — the harness fails naming it** | root of an extracted Xvfb userland (§5.2) | this document |
| `USDGEN_DISPLAY` | `:77` | display the T3 harness starts and uses | this document |

The `TfDebug` codes (`USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`,
`USDGEN_CAPTURE`, `USDGEN_MEMORY`) are env symbols too, set through `TF_DEBUG`; §7.2 is their
table and this registry does not duplicate it (`ADR §9.4 R35`).

#### 3.5.2 CMake cache variables

Every `USDGEN_*` cache variable a build may set. They are **not** env vars: nothing reads them at
run time, and a document that needs a new build switch adds it here and to the section named in the
last column.

| Variable | Type / default | Meaning | Defined in |
|---|---|---|---|
| `USD_INSTALL_DIR` | PATH, `../OpenUSD_26_08` | the OpenUSD install to build against | §3.1 |
| `USDGEN_FP_CONTRACT` | STRING, `off` | `-ffp-contract` on `usdGenMath` **only**; the `build-fpfast` job sets `fast` | §3.1, §5.5, §8.1 |
| `USDGEN_MATH_SOURCES` | list | the `usdGenMath` source list | §3.1 |
| `USDGEN_WITH_RIGEXEC` | BOOL, `ON` | `find_package(rigExec CONFIG)` for `rigExec::rigExecMath`; `OFF` must build and pass every non-rig test | §3.1; `00-request-and-scope.md` §4.3 |
| `RIGEXEC_INSTALL_DIR` | PATH, empty | rigExec install prefix passed to that `find_package` | §3.1 |
| `USDGEN_FORBIDDEN_LIB_REGEX` | STRING, `^libusd_(usd\|hd\|hgi\|glf\|garch)` | the one forbidden-family regex gate **B-1** enforces on `DT_NEEDED` | §1.4 |
| `USDGEN_FORBIDDEN_INC_REGEX` | STRING | the same families as an `#include` pattern | §1.4 |
| `USDGEN_USE_SYSTEM_SEEXPR` / `_PTEX` / `_NANOFLANN` | BOOL, `OFF` | packagers' switch from the vendored tree to `find_package` | §2.2 |
| `USDGEN_INSTALL_LIBDIR` | PATH, `lib` | install destination for the shared libraries | §3.4 |
| `USDGEN_INSTALL_PLUGINDIR` | PATH, `lib/usd` | install destination for plugin resource dirs; `plugin/usd` in deployment (b) | §3.4; `02-schema.md` §7.1 |
| `USDGEN_INSTALL_PYTHONDIR` | PATH, `lib/python` | install destination for `usdgen/` and `_usdGen` | §3.4 |
| `USDGEN_INSTALL_CMAKEDIR` | PATH, `lib/cmake/usdGen` | install destination for the package config | §3.4 |
| `USDGEN_INSTALL_INTO_USD` | BOOL, `OFF` | deployment (b): install into the USD prefix | §3.4 |
| `USDGEN_USD_SITE_PACKAGES` | PATH, derived by glob | the install's `lib/python*/site-packages`; never a literal `python3.12` | §3.4 |

`09-performance-and-benchmarks.md` §5.1's B-1 row names the same two regex variables.

### 3.6 `bin/_env.sh`

Modelled on `prototypes/usdrig-linux-build/rigexec_env.sh` with the three defects of usdRig's own
helper fixed (`S46`; `research/B-usdrig-build.md` §7; §8.3 below):

```sh
export USD=${USD:-/home/burkard/work/OpenUSD_26_08}
export GEN=${GEN:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
export GENBUILD=${GENBUILD:-$GEN/build}
export PY=${PY:-${VENV:-/home/burkard/.venv}/bin/python3}   # OpenUSD's tools are python scripts
PY_SITE=$(ls -d "$USD"/lib/python*/site-packages 2>/dev/null | head -1)   # never hard-code 3.12
export PATH="$GENBUILD:$USD/bin:$PATH"
export LD_LIBRARY_PATH="$USD/lib:$GENBUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DYLD_LIBRARY_PATH="$LD_LIBRARY_PATH"          # macOS; Linux needs LD_, usdRig set only DYLD_
export PYTHONPATH="$GENBUILD/python:$PY_SITE${PYTHONPATH:+:$PYTHONPATH}"
export PXR_PLUGINPATH_NAME="$GENBUILD/usd/usdGenSchema/resources:\
$GENBUILD/usd/usdGenImaging/resources:$GENBUILD/usd/usdGenShaders/resources:\
$GENBUILD/python/usdgen${PXR_PLUGINPATH_NAME:+:$PXR_PLUGINPATH_NAME}"
export USDGEN_IMAGING_DLL="$GENBUILD/libusdGenImaging.so"   # required for out-of-tree builds
```

It also defines `usdgen_require_python` and `usdgen_require_usd <path>`, which fail with a one-line
message naming the variable to set rather than dying inside Python — the shape is
`usdRig/bin/_env.sh:41-55`. Every script that runs an OpenUSD tool goes through `"$PY"`; relying on
the tool's shebang works only by accident here, because `head -1 $USD/bin/usdGenSchema` is an
absolute path baked in at USD build time.

### 3.7 Compile flags

`-Wall -Wextra -Werror -Wno-cpp` on usdGen's own targets only; vendored statics are exempt.
`-Wno-cpp` is what keeps `-Werror` green, and **not** a SYSTEM-include trick: pxr's imported targets
are already treated as system by CMake ≥ 3.25
(`/usr/share/cmake-3.28/Help/prop_tgt/SYSTEM.rst:16-19`, "For imported targets, this property
defaults to true"), and GCC reports `#warning` (`-Wcpp`) from a system header anyway — MEASURED on
this host, 2026-09-05: `g++ -std=c++17 -Wall -Wextra -Werror -isystem <dir> …` on a header
containing `#warning` still gives `error: #warning … [-Werror=cpp]`. The 36 `[-Wcpp]` diagnostics of
the reference build are `#warning` directives in `<ext/hash_set>` and `tbb/task.h`, both reached
through OpenUSD's own headers (MEASURED, `EV-076`; `research/B-usdrig-build.md` §2). That 36 is a
**floor, not a ceiling**: it was measured at GCC's default warning level, because usdRig sets no GCC
warning flags at all (its one `add_compile_options` line is MSVC-only,
`usdRig/CMakeLists.txt:14-16`). The M0 exit re-measures the warning set at `-Wall -Wextra` and
records what else must be suppressed.

---

## 4. Schema generation workflow

### 4.1 Source of truth

`libs/usdGenSchema/schema.usda` is the single source of the codeless domain, with the usdRig `GLOBAL`
block shape (`usdRig/libs/rigExecSchema/schema.usda:18-27`):

```usda
over "GLOBAL" (
    customData = {
        string libraryName   = "usdGenSchema"
        string libraryPath   = "usdGenSchema"
        string libraryPrefix = "UsdGen"
        bool   skipCodeGeneration = true
    }
) { }
```

It declares the hierarchy of `ADR §2.1` exactly; property names, types and defaults come from
`02-schema.md`, the normative property registry (`ADR §9.2 R7`). Contract C1 freezes them at the end
of M1.

### 4.2 `bin/gen_schema.sh`

A direct twin of `usdRig/bin/gen_schema.sh`: it sources `bin/_env.sh`, calls `usdgen_require_python`
and `usdgen_require_usd "$USD/bin/usdGenSchema"`, **`cd`s to `$GEN/libs/usdGenSchema`** (the twin
does the same at `usdRig/bin/gen_schema.sh:13`, and without it the relative output path below
resolves against the caller's directory), then runs

```sh
"$PY" "$USD/bin/usdGenSchema" schema.usda ../../plugin/usdGenSchema/resources
```

and post-processes the generated `plugInfo.json` for a codeless plugin (strip
`"LibraryPath": "@PLUG_INFO_LIBRARY_PATH@"`, set `ResourcePath` and `Root` to `"."`). Output is
**checked in** and regenerated only by this script; reviewers read the diff.

> **Naming hazard.** OpenUSD's schema code generator is itself called `usdGenSchema`
> (`$USD/bin/usdGenSchema`), and usdGen's schema target is also `usdGenSchema` (`ADR §6 build`,
> binding). They are unrelated. Every script invokes the generator by full path, never the bare name,
> and no CMake target is ever named from a `PATH` lookup.

`testUsdGenSchemaUpToDate` (T1) re-runs the generator into a temporary directory and diffs against
the checked-in resources, so "edited `schema.usda`, forgot to regenerate" fails in CI rather than at
a user's stage open.

### 4.3 The schema plugInfo rewrite

`UsdGenDescription` derives from `UsdGeomBoundable` and needs a registered extent function
(`pxr/usd/usdGeom/boundableComputeExtent.cpp:271-289`); Plug treats a `"resource"` plugin as already
loaded and never `dlopen`s it, so a codeless schema declaring `implementsComputeExtent` must be a
**library** plugin with a `LibraryPath` (`usdRig/CMakeLists.txt:409-421`). `ADR §9.2 R19` rules that
the library is **`libusdGenSchema.so`** — schema tokens plus the extent registration, linking
`usd`/`usdGeom` only — and **not** `libusdGenImaging.so`, because pointing the schema plugin at the
imaging library makes every `usdcat`/`usdrecord`/`usdview` open of a groom `dlopen` Hydra,
`usdImaging`, `sdr` and `usdShade` to compute a bounding box.

The extent kernel body lives in `usdGenMath` (§1.2), so `usdGenSchema` links no Hydra. When
`usdGenImaging` loads it installs a provider through a setter `usdGenSchema` exports
(`void UsdGenSchema_SetExtentProvider(UsdGenSchemaExtentFn)`); the callback answers from the live
generation **only if it describes this stage at this time**, else from the bound surface's extent.
The dependency direction is `usdGenImaging → usdGenSchema`, never the reverse.

At configure time usdGen reads the checked-in `plugInfo.json` and:

1. replaces `"Type": "resource"` with `"Type": "library"`;
2. inserts `"LibraryPath"` before `"Name"` — `"../../../$<TARGET_FILE_NAME:usdGenSchema>"` in the
   build-tree copy, `"../${_hop}/$<TARGET_FILE_NAME:usdGenSchema>"` in the install-tree copy, the
   extra hop being what `Root`/`ResourcePath` = `"."` costs (§3.3);
3. inserts `"implementsComputeExtent": true` on the `UsdGenDescription` entry **only**;
4. appends the auto-apply block **inside the plugin's `"Info"` dictionary, beside `"Types"`** —
   `CollectAddtionalAutoApplyAPISchemasFromPlugins` reads `plug->GetMetadata()`, which *is* `Info`
   (`pxr/usd/usd/schemaRegistry.cpp:865-867`), so a top-level key is silently ignored:

```json
"Info": {
    "Types": { "…": {} },
    "AutoApplyAPISchemas": {
        "UsdGenMaskAPI": { "apiSchemaAutoApplyTo": [ "UsdGenOperator" ] }
    }
}
```

`generatedSchema.usda` is copied beside it untouched. The mechanism is
`UsdSchemaRegistry::CollectAddtionalAutoApplyAPISchemasFromPlugins`
(`pxr/usd/usd/schemaRegistry.cpp:836-900`), skipped when `USD_DISABLE_AUTO_APPLY_API_SCHEMAS` is set
(`:59, 842`). Gate **SI-8** proves it on a *codeless* type; until it passes the tool also applies
`UsdGenMaskAPI` explicitly, and `testUsdGenAutoApply` asserts both paths give the same
`UsdPrimDefinition::GetPropertyNames()`.

### 4.4 The plugInfo entries usdGen ships

| File | `Type` | Registers |
|---|---|---|
| `usd/usdGenSchema/resources/plugInfo.json` | library → `usdGenSchema` | the codeless types, `AutoApplyAPISchemas`, `implementsComputeExtent` on `UsdGenDescription` (`ADR §9.2 R19`) |
| `usd/usdGenImaging/resources/plugInfo.json` | library → `usdGenImaging` | `UsdGenGroomSceneIndexPlugin` (base `HdSceneIndexPlugin`, `loadWithRenderer: ""`, tags `["usdGen:groom"]`, `ordering: {after: ["hd:sceneGlobals"], before: ["hdGp:proceduralResolution", "hdPrman:motionBlur"]}`); `UsdGenMetadataSceneIndexPlugin` (base `UsdImagingSceneIndexPlugin`); **five** prim-adapter entries — `UsdGenOperator` and `UsdGenMap` with `"includeDerivedPrimTypes": true`, plus `UsdGenGroom`, `UsdGenDescription`, `UsdGenGuideSet` — and one `UsdImagingAPISchemaAdapter` for `UsdGenRestAPI` |
| `usd/usdGenShaders/resources/plugInfo.json` | library → `usdGenImaging` | `UsdGenShadersDiscoveryPlugin` (base `SdrDiscoveryPlugin`) and `"ShaderResources": "shaders"`; `"Root": ".."`, `"ResourcePath": "resources"`, so **two** hops, not the schema's three |
| `lib/python/usdgen/plugInfo.json` | python | `"Name": "usdgen.usdGenUsdview"`, `bases: ["pxr.Usdviewq.plugin.PluginContainer"]` — Plug loads a python plugin with `import <Name>` (`pxr/base/plug/plugin.cpp:218-222`), so `lib/python` must be on `PYTHONPATH` (`08-tools.md` §1.1) |

Two notes on the imaging entry. The `ordering.after: ["hd:sceneGlobals"]` tag is **decorative** in
stock 26.08 — no plugin carries that tag and the scene-globals index is inserted by the application
callback (`ADR §1`, `design/judge-evidence.md` §0); keep it for documentation, because placement is
guaranteed by phase 0 / `InsertionOrderAtEnd` and by the C++ `RegisterSceneIndexForRenderer` call
that ships alongside the JSON. And the five adapter entries are mandatory: with only `UsdGenOperator`
and `UsdGenMap` registered the container half of the schema is invisible to Hydra (MEASURED,
`ADR §2.3`), which gate **SI-7** asserts property by property.

`usdGenShaders` is a **library** plugin, not a resource-only one: a shader-def domain needs a C++
`SdrDiscoveryPlugin` subclass to be discoverable by `info:id`, and the glslfx files need
`"ShaderResources"` so `HioGlslfx` can resolve them — the stock pattern at
`pxr/usd/plugin/usdShaders/plugInfo.json:13` and `discoveryPlugin.cpp:29-48`.
`UsdGenShadersDiscoveryPlugin` (base `SdrDiscoveryPlugin`, `pxr/usd/sdr/discoveryPlugin.h:134`) is
compiled into `usdGenImaging`, which therefore links `sdr` and `usdShade` — as the stock plugin does
(`pxr/usd/plugin/usdShaders/CMakeLists.txt:5-8`). Both are allowed there and forbidden in `usdGen`.

**`discoveryPlugin.cpp` is compiled with `-DPLUG_THIS_PLUGIN_NAME=usdGenShaders`.** The stock code
resolves its resource directory with `static PlugPluginPtr plugin = PLUG_THIS_PLUGIN;`
(`pxr/usd/plugin/usdShaders/discoveryPlugin.cpp:32`), which expands to
`GetPluginWithName(TF_PP_STRINGIZE(PLUG_THIS_PLUGIN_NAME))`, defaulting to the compile-time
`MFB_PACKAGE_NAME` (`pxr/base/plug/thisPlugin.h:20-22, 28-30`) — a macro usdGen never defines,
because it does not use pxr's build system. Without the override the file does not compile; defined
to the library it sits in, it would look for `shaders/` under `usdGenImaging` and find no shader
defs. Equivalent alternative: `PlugRegistry::GetInstance().GetPluginWithName("usdGenShaders")`.

`shaderDefs.usda` declares **three** shader defs, one per shipped glslfx: `UsdGenHairPreview`
(`shaders/usdGenHairPreview.glslfx`), `UsdGenHairPreviewPrimvar` (variant B, `ADR §5.4`) and
`UsdGenHairPreviewTranslucent`. The identifier **is** the prim name and is UpperCamel
(`ADR §9.1 R4`; `pxr/usd/usdShade/shaderDefUtils.cpp:51-52`); only the file names are lowerCamel.
Variant B does not exist in the prototype and is authored during pre-work **PW-2**
(`11-roadmap.md` §1), whose S-8 result decides the default; both ship at M1.

---

## 5. Test tiers T0–T4

### 5.1 Definitions

`ADR §9.1 R2` fixes the tiers:

| Tier | Scope | Needs | Cost | Runs |
|---|---|---|---|---|
| **T0** | engine and build: `usdGenMath` kernels, `UsdGenGraph` over synthetic `UsdGenGraphDesc`, chunk/tile arithmetic, maps, expressions, Ptex, the Qt-free Python model modules, the build-rule checks. **No Hydra, no `UsdStage`** | nothing | ms | every commit |
| **T1** | headless scene index over the **real** `UsdImagingCreateSceneIndices` chain plus the renderer-plugin append, with a recording observer; asserts on `HdBasisCurvesSchema` contents **and on emitted dirty locators** | no GL | < 100 ms each (`S45`) | every commit — the primary regression suite |
| **T2** | Storm correctness and GPU timing through the **EGL harness** on the GB10; golden images with a fractional-pixel tolerance | GPU + EGL | ~1 s each (UNMEASURED; baseline `design/proposal-risk.md` §9.1) | correctness every commit, timing nightly |
| **T3** | `testusdview` scripts under Xvfb / llvmpipe: the app→data→Hydra loop, panels, brushes, undo, picking | Xvfb | seconds | pre-merge |
| **T4** | workstation protocols: MSAA/OIT quality, Metal/Vulkan Hgi, non-NVIDIA drivers, a real hdPrman, 4K interactive | a human at a display | manual | **release criteria only** |

T1 is where the money is: it catches a missing dirty, which a pull-based test structurally cannot
(`design/proposal-risk.md` §9.1). **Every operator ships with a T1 test asserting both the value and
the notice.** T4 gates are release criteria and may **never** be a milestone exit
(`design/judge-delivery.md` §7).

> **Naming hazard.** `T0`–`T4` are *test tiers*. `T-1`…`T-5` and `T-INST-1/2` are *tool gates*
> (`09-performance-and-benchmarks.md` §5). `usdGen:frozen:tier = session | sublayer | payload` are
> *freeze landing tiers* (`ADR §2.3`, `ADR §9.2 R6`; `02-schema.md` §2.9). Three unrelated
> vocabularies; the tier has no hyphen, the gate does, the freeze tier is a token.

### 5.2 Harnesses

**T0** is plain executables linking `usdGen` + `usdGenTestUtils` (neither links `usd` or `hd`, gate
B-1), registered with `add_test`, plus the Python model scripts of §5.6 run by
`bin/run_python_tests.sh`. The graph input is a `UsdGenGraphDesc` built in the test, which is
possible precisely because of §1.3.

**T1 and T2** link `usdGenTestUtilsHd`. T1 uses `sceneFixture.h`: open a stage from `tests/scenes/`,
call `UsdImagingCreateSceneIndices`, append the renderer-level chain the way an application does,
attach a recording observer, assert on `GetPrim` results and on the exact `PrimsDirtied` locator
sets — the shape of `usdRig/tests/probeImagingPipeline.cpp` (0.07 s, 36 assertions, no GL; MEASURED).
These tests need `PXR_PLUGINPATH_NAME`, so CMake passes the build-tree resource dirs through
`set_tests_properties(… PROPERTIES ENVIRONMENT …)` rather than relying on the shell.

**T2** uses `eglctx.h` from `prototypes/storm-hair-look/`. Two details are load-bearing and already
encoded there (MEASURED, `research/G-storm-hair-look-prototype.md` §0): request the **compatibility**
profile (`EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x2`), or HdSt/HgiGL's legacy enum save/restore produces
~25 `invalid enum` errors per frame and an all-white image; and create a 64×64 **pbuffer** via
`eglChooseConfig` with `EGL_PBUFFER_BIT|EGL_OPENGL_BIT`, because a surfaceless context leaves the
default FBO incomplete and `HgiGL_ScopedStateHolder` fails.

**T3** uses `bin/xvfb.sh`. `USDGEN_XVFB_ROOT` is the root of an extracted Xvfb userland and has
**no default — the script fails with a one-line message naming it**, because this host has no system
Xvfb at all (`research/ENVIRONMENT.md` "Hardware / OS" and CORRECTIONS: Xvfb exists only as a
`dpkg-deb -x` extraction in a scratch directory). It starts

```sh
"$USDGEN_XVFB_ROOT/usr/bin/Xvfb" "$USDGEN_DISPLAY" -screen 0 1280x1024x24 -nolisten tcp \
    -xkbdir "$USDGEN_XVFB_ROOT/usr/share/X11/xkb"
```

— the `dpkg-deb -x` layout of `research/B-usdrig-build.md` §6 — then runs
`$USD/bin/testusdview --testScript`. Scripts select the terminal scene index by the `"[Terminal SI]"`
prefix (`S43`). **No timing assertion may appear in a T3 test.**

**T4** is `docs/workstation-protocol.md`, one numbered check per gate, each naming the exact command,
the env vars and the artefact to attach. **This section owns its numbering, and there is exactly
one scheme — no `W-` ids anywhere** (`08-tools.md` §8.3). Its **§§1–6 are
`research/G-storm-hair-look-prototype.md` §6 verbatim** — 1 usdview sign-off, 2 interactive frame
time, 3 MSAA/alpha-to-coverage, 4 non-NVIDIA drivers, 5 Metal/Vulkan under
`HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, 6 Ptex — usdGen adds **§7 hdPrman parity** and **§8 4K
interactive**, and the three tool-loop checks of `08-tools.md` §8.3 are **§9 the stroke, §10 the
freeze, §11 the pick**. Gate mapping: R-1 → §7, R-2 → §3, R-3 → §5 with §4 for the AMD/Intel half —
the mapping `09-performance-and-benchmarks.md` §5.4 already prints; the end-to-end half of T-1 → §9,
the GPU half of T-4 → §10, pick probe P1 → §11. `09-performance-and-benchmarks.md` §4.3 prints
the same §§1–11.

### 5.3 Canonical scenes

`tests/scenes/` holds five hand-authored stages and one generator:

| Scene | Content | Used by |
|---|---|---|
| `scene_a_generate_and_style.usda` | the `ADR §2.2` layout: `Scatter → Grow → Noise → Length → Width` on a usdRig-deformed scalp, one `UsdGenGuideSet`, one image map | T1, T2, T3 |
| `scene_b_frozen_deform.usda` | frozen curves under contract C3 (`primvars:rest`, `primvars:skinprim`, `primvars:skinprimuv`, `primvars:usdGen:curveId` as `uint64[]`, `primvars:usdGen:frozenEpoch`, `primvars:usdGen:role = hair`; `02-schema.md` §5) on a **UsdSkel-skinned** scalp, plus a `UsdGenSculptLayer`. Hand-authored, **not** a freeze product — freezes land as `.usdc` (`S42`) | T1 (`S5` bare-`primvars` rule), T2 |
| `scene_c_cards_archives.usda` | `UsdGenInstance` with card and archive prototypes under `Prototypes/`, one natively instanced scalp | T1, T3 |
| `scene_skel_and_rig.usda` | carried from `prototypes/chain-order/stages/skelAndRig.usda`: UsdSkel **and** RigExec on one stage | T1 chain order (SI-5) |
| `scene_empty_groom.usda` | a `UsdGenGroom` with one empty `UsdGenDescription` | T1 population (SI-6), M0 smoke |
| `tests/perf/gen_hair_stages.py` | carried from `prototypes/storm-throughput/`: emits `hair_1prim`, `hair_32chunks`, `hair_1000prims`, `hair_32chunks_anim`, `hair_32chunks_density{100,050,025}` at any `--curves/--cv/--frames` | T2 and nightly perf |

Large stages are **generated, never checked in**: the 200 k-curve `.usda` used for the look benchmark
is 172 MB (MEASURED, `research/G-storm-hair-look-prototype.md` §6).

### 5.4 Golden data policy

* **Golden images** live in `tests/golden/`, are ≤ 256×256 PNG, and are produced **only** by the EGL
  harness on the GB10. Each carries a sidecar `<name>.json` recording GL vendor/renderer/version,
  driver, refineLevel, complexity and the generation counter. Comparison is a fractional-pixel
  difference: mean absolute per-channel difference ≤ 2/255 **and** fewer than 0.5 % of pixels above
  8/255 (all ASSUMPTION; calibrated at M1 against ten consecutive `testUsdGenStormLook` runs and
  re-stated there as MEASURED noise floors).
* **GB10 goldens are never compared against llvmpipe output.** T3 image checks are *not* goldens:
  they are fraction-of-pixels-changed mutation checks under llvmpipe
  (`08-tools.md` §9.4 — `stageView.grabFrameBuffer()`, HUD off, a subsampled pixel set compared as a
  fraction of the frame, every check mutation-verified), and they carry no timing assertion.
* **Golden text** (T1) is a serialized dump of structure, not values: prim paths, prim types, and
  per-primvar `(name, type, interpolation, elementCount)` triples, plus the sorted locator set of each
  `PrimsDirtied`. Stored as `.txt` beside the test.
* **Float arrays are never goldens.** Numeric assertions use tolerances chosen against the kernel's
  documented epsilon, or exact equality only where §5.5 guarantees it.
* Regeneration is `bin/regen_goldens.sh <test>`; it refuses to run unless the working tree is clean,
  and the diff is reviewed like code. A golden update in a PR that also changes a kernel must say in
  one line why the image moved.

### 5.5 Determinism across thread counts

Gate **E-8** (T0, `testUsdGenKernelDeterminism`): the same `UsdGenGraphDesc` and input buffers,
evaluated at `USDGEN_THREAD_LIMIT` ∈ {1, 2, 4, 8, 20}, must produce **bitwise identical** published
buffers (`memcmp`). Three rules make that achievable:

1. `-ffp-contract=off` on `usdGenMath` (§3.1, §8.1) — otherwise `a*b+c` fuses differently across
   inlining decisions.
2. **Deterministic reductions.** No `tbb::parallel_reduce` over floats whose combine order depends on
   the runtime split; reductions are fixed-order tree reductions over the chunk index (`ADR §4.1`).
3. No intrinsics and no hand-written SIMD; GCC autovectorises and layout is the lever (`S22`), which
   keeps one code path for all thread counts.

The `build-fpfast` CI job builds with `-DUSDGEN_FP_CONTRACT=fast` and runs the **tolerance** suite,
not the bitwise one — `research/B-usdrig-build.md` decision 2's "CI job that also builds with fast to
keep epsilons honest". A failure there means a kernel's epsilons are tuned too tight, not that the
release build is wrong.

Thread scaling itself is gate **E-7**, expressed as a **ratio** (≥ 3× from 1 → 8 threads; baseline
3.8× = 3.90 ms → 1.02 ms, MEASURED, ledger row `EV-008`,
`research/G-data-plane-engine-prototype-benchmark.md` §3.3), never as an absolute, because both
engines regress past ~8–10 threads on this heterogeneous host (risk-register item R12).

### 5.6 CTest names

This section is the CTest name registry. Every test carries its tier label and, where it proves a
gate, `gate:<id>` (§6.1).

| Tier | Tests |
|---|---|
| T0 build | `testUsdGenLinkRule_{usdGen,usdGenMath,usdGenTestUtils}`, `testUsdGenIncludeRule`, `testUsdGenNoThirdPartyExports`, `testUsdGenInstallTree`, `testUsdGenOfflineConfigure`, `testUsdGenConsumer`, `testUsdGenNotice`, `testUsdGenAbiSymbols` (from M5) |
| T0 engine | `testUsdGenMath`, `testUsdGenKernelDeterminism`, `testUsdGenGraph`, `testUsdGenOps`, `testUsdGenChunking`, `testUsdGenHash`, `testUsdGenMasks`, `testUsdGenRamps`, `testUsdGenMotionCache`, `testUsdGenExpr`, `testUsdGenPtex`, `testUsdGenMaps`, `testUsdGenLookBake`, and the static-curve suite of `05-static-curves-and-deformation.md` §9: `testUsdGenCurveLoader`, `testUsdGenRootFrame`, `testUsdGenTransport`, `testUsdGenSculptRoundTrip`, `testUsdGenSculptIndex`, `testUsdGenFreezeSpaceRule`, `testUsdGenEpoch` |
| T0 perf | `benchUsdGenChain`, `benchUsdGenSparse`, `benchUsdGenKnn`, `benchUsdGenMemory`, `benchUsdGenMaps`, `testUsdGenEngineBench` (carries `benchPick`, gate T-2's engine half) |
| T0 python | `test_usdgen_brushmath.py`, `test_usdgen_undo.py`, `test_usdgen_graphmodel.py`, `test_usdgen_freeze_author.py`, `test_usdgen_mask_author.py`, `test_usdgen_toolstate.py`, `test_usdgen_transport.py`, `test_usdgen_liveoverride.py` (the eight of `08-tools.md` §9.1) plus `test_usdgen_builder.py` (the `usdgen.builder` facade and `usdgen.migrate`, `02-schema.md` §8.6) — run by `bin/run_python_tests.sh` and registered with `add_test` so `ctest -L T0` covers them |
| T1 | `testUsdGenPluginDiscovery`, `testUsdGenSchemaUpToDate`, `testUsdGenAdapter`, `testUsdGenAutoApply`, `testUsdGenRestAdapter`, `testUsdGenChainOrder`, `testUsdGenPopulation`, `testUsdGenTileContract`, `testUsdGenInvalidation`, `testUsdGenScheduling`, `testUsdGenSnapshotRace`, `testUsdGenSkelInterop`, `testUsdGenFrozenReentry`, `testUsdGenSessions`, `testUsdGenInstancer`, `testUsdGenInstancerPick`, `testUsdGenInstanceKeys`, `testUsdGenSurfaceResolver`, `testUsdGenMotionSamples`, `testUsdGenPick`, `testUsdGenPruningCost`, `testUsdGenDependencies`, `testUsdGenReorderNotice`, `testUsdGenStageEdits`, `testUsdGenLookInvalidation`, `testUsdGenMapReload`, `testUsdGenContracts`, `testUsdGenArrays`, `testUsdGenAbi`, and from `05-static-curves-and-deformation.md` §9: `testUsdGenC3Contract`, `testUsdGenFreezeEpoch`, `testUsdGenSourceDirtyRouting` |
| T2 | `testUsdGenStormLook`, `testUsdGenStormTangent`, `testUsdGenStormRefine`, `testUsdGenStormMaterial`, `testUsdGenStormHgiResource`, `testUsdGenStormInPlace` (M8), `benchUsdGenStorm` |
| T3 | `testUsdviewUsdGen{Activate,Stack,Comb,Symmetry,Paint,Freeze,Pick,Cards,Undo,Async,Look}.py` — the eleven scripts of `08-tools.md` §9.3, one runner each (`bin/run_testusdview_usdgen_<x>.sh`) |
| docs | `testUsdGenDocs` — the `plan/` reference-integrity and tag-discipline script (`12-risks-decisions-open-questions.md` §5, `appendix-A-evidence-ledger.md` §7, `00-request-and-scope.md` §7); runs beside the tiers and belongs to none of T0–T4 |

Every name here is `testUsdGen*` / `benchUsdGen*` with a capital **G**, matching §7.1's C++
convention; `usdgen` lowercase is the Python package (`ADR §9.1 R5`) and appears only in the
`test_usdgen_*.py` script names. `testUsdGenArrays` and `testUsdGenAbi` are T1, not T0: both load
`libusdGenImaging.so` and so need the imaging plugin registry. The authoring facade needs neither and
is a T0 python script. `testUsdGenReorderNotice` records what, if anything, `reorder nameChildren`
dirties through the adapter — pre-work **PW-6**, gate **SI-11**
(`09-performance-and-benchmarks.md` §5.2); nothing in the design depends on the answer
(`ADR §2.1`).
`testUsdGenStageEdits` is the `RemovePrim` containment test of §8.2.

Five names are additions siblings own and this registry carries so `ctest -L T1` runs them:
`testUsdGenInstanceKeys` (native-instance aggregation key and proxy-path translation),
`testUsdGenSurfaceResolver` (`Mesh` / `GeomSubset` / instance-proxy targets) and
`testUsdGenMotionSamples` (the hdPrman sampled-points contract) come from `06-imaging.md` §10.2;
`testUsdGenContracts` (T1, the C1/C2/C3/C5 name lists as literal data) and `testUsdGenAbiSymbols`
(T0, `nm -D --defined-only libusdGenImaging.so` filtered to `UsdGenImaging_*` against
`08-tools.md` §1.4, which is contract C4) come from `11-roadmap.md` §0.2.

`testUsdGenPluginDiscovery` catches a broken install before anything else: with only `bin/_env.sh`
sourced and **no direct link**, Plug must find every plugin that exists at the current milestone —
`usdGenSchema` and `usdGenImaging` from M0, `usdGenShaders` from M1, the python plugin from M5. The
expected set is a CMake-generated list, so adding a plugin without registering it fails. It further
asserts `Usd.SchemaRegistry().IsConcrete("UsdGenScatter")` and that the `UsdImagingSceneIndexPlugin`
subtype list contains `UsdGenMetadataSceneIndexPlugin` — the probe that validated usdRig's install
(`research/B-usdrig-build.md` §7), where stock 26.08 shows one such subtype (UsdSkel's), rigExec adds
the second and usdGen is the third.

---

## 6. CI and reproducibility

### 6.1 Labels

Every test carries `LABELS` with its tier (`T0`…`T3`), optionally `perf` and `build`, and one
`gate:<id>` per gate it proves. `ctest -L T1` is the pre-commit suite; `ctest -L 'gate:'` is the
milestone-exit suite; `ctest -LE flaky` is what gates a merge.

### 6.2 Jobs

| Job | What | When |
|---|---|---|
| `configure-offline` | configure with `FETCHCONTENT_FULLY_DISCONNECTED=ON` and `USDGEN_WITH_RIGEXEC=OFF` | every push |
| `build-release` | `-DCMAKE_BUILD_TYPE=Release`, `ninja -j16` (no global `-ffp-contract`: the flag is per target, §3.1) | every push |
| `t0` + `t1` | `ctest -L '^T[01]$' -j8` — `-L` takes a **CMake regex**, in which `\|` is an escaped literal pipe and matches nothing; anchored so `T1` never matches a future `T10` | every push |
| `t2-egl` | `ctest -L T2` on the GB10 through the EGL harness | every push (correctness) |
| `t3-xvfb` | `bin/xvfb.sh` then `ctest -L T3` | pre-merge |
| `build-fpfast` | same sources, `-DUSDGEN_FP_CONTRACT=fast`, tolerance suite only (§5.5) | nightly |
| `nightly-perf` | `ctest -L perf` plus `benchUsdGenStorm`; results appended to a CSV with the git sha | nightly |
| `nightly-sanitize` | `-fsanitize=address,undefined` on T0+T1; `-fsanitize=thread` on `testUsdGenSnapshotRace`, `testUsdGenScheduling`, `testUsdGenSessions` | nightly, from M2 |
| `install-check` | `cmake --install` to a scratch prefix, then `testUsdGenInstallTree` + `testUsdGenConsumer` + `testUsdGenPluginDiscovery` against the *installed* tree | every push |
| `with-rigexec` | `USDGEN_WITH_RIGEXEC=ON` against an installed rigExec | every push |

### 6.3 Timing budgets

| Budget | Value | Status |
|---|---|---|
| `t0` + `t1` wall clock | ≤ 60 s at `-j8` | UNMEASURED; asserted from M1 by a CI timing check |
| any single T1 test | ≤ 100 ms | `S45`; `TIMEOUT` catches hangs only, so the budget is asserted by the `t0`+`t1` job, which fails if `ctest --output-junit` reports any `T1` test over 100 ms. The nightly report lists the slowest ten |
| any single T2 test | ≤ 5 s | UNMEASURED; baseline ~1 s per render (`design/proposal-risk.md` §9.1) |
| clean build | ≤ 90 s at `-j16` | UNMEASURED; reference project 21.45 s for 64 edges (MEASURED, `EV-076`) |
| usdGen's own per-frame share | ≤ 1.0 ms at 100 k × 8 CV on a deform frame | the ledger is `09-performance-and-benchmarks.md` §0.2, which owns it (`ADR §9.5 R41`); this row cites it and does not restate it. RigExec's 1.35 ms/frame and the 0.24 ms terminal traversal are MEASURED (`EV-035`/`EV-036`, `research/G-chain-order-probe.md` §5); the "≈ 15 ms/frame left over" of `design/proposal-risk.md` §9.1 is arithmetic (**DERIVED**, tagged ASSUMPTION in `appendix-A-evidence-ledger.md` §2.11) and is *before any GPU time* |

### 6.4 Flake policy

A test that fails intermittently is labelled `flaky` within 24 hours, which removes it from the
merge-gating set (`ctest -LE flaky`) and files an issue naming the milestone it must be fixed in; it
must be fixed or deleted before that milestone exits. **A test carrying a `gate:` label may never be
quarantined**: if a gate test flakes, the gate is red and the milestone does not exit. Two
environment-sensitive classes are kept out of the gating set pre-emptively — Qt font-metric/DPI
assertions and wall-clock animation interpolation, both of which produced false failures in usdRig's
`testusdview` suites here (`research/B-usdrig-build.md` §6).

### 6.5 Gate → test map

`09-performance-and-benchmarks.md` §5 is the gate registry (`ADR §9.5 R40`): it owns each gate's
metric, pass criterion, tier and milestone. **This table restates the registry as reconciled by R40
and adds the CTest name and harness for each id; where the two disagree, 09 §5 as reconciled by R40
wins.** Every id 09 §5 registers has a row here — all fifty-three — so `ctest -L 'gate:'` covers the
whole registry, and no id is minted here that 09 §5 does not carry.

Three pairs of ids are easy to confuse. **SI-9** is the pruning-wrapper cost **only**; the
two-index session agreement is **SI-10** (`testUsdGenSessions`, M2, the id `12-…` RK-09 cites); the
`reorder nameChildren` check is **SI-11** (record only). The tile-count sweep is **S-12**; `S-10` is
R29's v2 in-place overlay and `S-11` is the 1 M-curve record.

| Gate | Tier | Test | Milestone exit |
|---|---|---|---|
| **B-1** link rule (`DT_NEEDED` of `libusdGen.so` carries no forbidden family) | T0 | `testUsdGenLinkRule_*` + `testUsdGenIncludeRule` | **M0** |
| **B-2** staging stage-API fence (no `UsdStage`/`UsdPrim`/`UsdGeom`/`UsdShade` in the Hydra-builder and `_CommitNow` staging TUs) | T0 | `testUsdGenStagingRule` (include-regex + `nm` symbol check, B-1 pattern) | **M2** |
| E-1 chain throughput | T0 | `benchUsdGenChain` | M1 |
| E-1r ragged path ≤ 2× uniform | T0 | `benchUsdGenChain --ragged` | M2 |
| E-2 sparse edit | T0 | `benchUsdGenSparse` | M1 |
| E-3 last-parameter edit | T0 | `benchUsdGenSparse --terminal` | **M2** |
| E-4 kNN capture, linear in roots | T0 | `benchUsdGenKnn` | M0 pre-work (PW-1), binding at M3 |
| E-5 memory at 1 M × 8 | T0 | `benchUsdGenMemory` | M3, re-run M7 |
| E-6 recompile one node of 200 | T0 | `testUsdGenGraph` | M1 |
| E-7 thread-scaling ratio | T0 | `benchUsdGenChain --threads` | M1 |
| E-8 determinism 1 vs 8 threads | T0 | `testUsdGenKernelDeterminism` | **M1**, re-run M4 |
| SI-1 exact-size arrays | T1 | `testUsdGenTileContract` | M1 |
| SI-2 exact locator set | T1 | `testUsdGenInvalidation` | M1, re-run M2 for overlaid prims |
| SI-3 one cook per event | T1 | `testUsdGenScheduling` | M1 |
| SI-4 no torn read | T1 | `testUsdGenSnapshotRace` | M1 |
| SI-5 chain order | T1 | `testUsdGenChainOrder` | **M1** (first green in M0) |
| SI-6 initial population, both paths | T1 | `testUsdGenPopulation` | M1 |
| SI-7 adapter coverage | T1 | `testUsdGenAdapter`, `testUsdGenRestAdapter` | M1 |
| SI-8 auto-applied API on a codeless type | T1 | `testUsdGenAutoApply` | M0 pre-work (PW-5), binding at M1 |
| **SI-9** pruning-wrapper cost on a production-density skinned scalp | T1 | `testUsdGenPruningCost` (`06-imaging.md` §10.2) | **M2** |
| **SI-10** two attached indices agree on generation, prim set and frame for one commit | T1 | `testUsdGenSessions` | **M2** |
| **SI-11** `reorder nameChildren` reaches the scene index as an invalidation | T1 | `testUsdGenReorderNotice` | **M0** pre-work (PW-6) — record only, nothing depends on the answer |
| **SI-12** Hydra/stage builder parity (identical `GraphDesc` on G1–G4) | T1 | `testUsdGenHydraParity` | **M2** |
| **SI-13** Hydra staging cost ≤ 0.2 ms on G3 | T1 | `testUsdGenHydraParity` (timing section) | **M2** |
| S-1 Storm static (re-measured at 100 k) | T2 | `benchUsdGenStorm --static` | M1 |
| S-2 deform frame delta | T2 | `benchUsdGenStorm --deform` | M2 |
| S-3 one-tile edit | T2 | `benchUsdGenStorm --onetile` | M2 |
| S-4 culling | T2 | `benchUsdGenStorm --cull` | **M2** |
| S-5 one batch, one draw call | T2 | `benchUsdGenStorm --batches` | M1 |
| S-6 no VBO relocation | T2 | `benchUsdGenStorm --deform` (counters) | **M1** |
| S-7 density scrub | T2 | `benchUsdGenStorm --scrub` | M5 |
| S-8 `hairTangent` strategy (A vs B) | T2 | `testUsdGenStormTangent` + `testUsdGenStormHgiResource` | M0 pre-work (PW-2), decides M1 |
| S-9 refineLevel 1 vs 2 **and switch cost** | T2 | `testUsdGenStormRefine` | M0 pre-work (PW-3), decides M1 |
| **S-10** in-place overlay vs PrimsRemoved/Added | T2 | `testUsdGenStormInPlace` | **M8** (v2, `ADR §9.4 R29`) |
| **S-11** 1 M-curve static draw, record only (does the linear fit hold past 200 k?) | T2 | `benchUsdGenStorm --scene bench/head1M.usda --static` | **M7** |
| **S-12** tile-count sweep at 100 k × 8 CV: 1 / 32 / 128 / 196 tiles | T2 | `benchUsdGenStorm --tilesweep` | **M1**, before C2 freezes |
| L-1 render-context resolution | T2 | `testUsdGenStormMaterial` | M0 pre-work (PW-4), decides M1 |
| **L-2** glslfx parse/compile/golden + CPU-GPU bake parity | T2 | `testUsdGenStormLook` | **M1** |
| **L-3** map/expression determinism | T0 | `testUsdGenExpr --threads`, `testUsdGenLookBake` | **M4** |
| **L-4** map capture cost + `fileReopens == 0` | T0/T1 | `benchUsdGenMaps` | **M4** |
| **L-5** `ReloadMaps` invalidation set | T1 | `testUsdGenMapReload` | **M4** |
| T-1 brush move | T3 | `testUsdviewUsdGenComb.py` (engine + Python time only; `testUsdviewUsdGenSymmetry.py` re-runs it with symmetry on) | M5 |
| T-2 `PickCV` | T1 | `testUsdGenPick` (engine half `benchPick` in `testUsdGenEngineBench`, T0) | M5 |
| T-3 pick accuracy vs `view.pick()` | T3 | `testUsdviewUsdGenPick.py` | M5 |
| T-4 freeze cost | T3 | `testUsdviewUsdGenFreeze.py` | M5 |
| T-5 async from the plugin | T3 | `testUsdviewUsdGenAsync.py` | **M8** |
| **T-EXPR-1** SeExpr sandbox (`SE_EXPR_PLUGINS` cleared, closed function set, no exception into Hydra) | T0 | `testUsdGenExpr --sandbox` | **M4** |
| **T-PTEX-1** Ptex face-id agreement with `Far::PtexIndices` on quads, n-gons and all-triangle meshes | T0 | `testUsdGenPtex` | **M4** |
| T-INST-1 instancer pick round-trip | **T1** | `testUsdGenInstancerPick` (the app-level round trip stays the unlabelled T3 script `testUsdviewUsdGenCards.py`, `08-tools.md` §9.3) | M6 |
| T-INST-2 prototype rebasing | T1 | `testUsdGenInstancer` | M6 |
| R-1 hdPrman parity | **T4** | workstation protocol **§7**; its T1 precondition, the sampled-points contract without hdPrman, is `testUsdGenMotionSamples` (`06-imaging.md` §10.2) | **release** |
| R-2 MSAA / OIT quality | **T4** | workstation protocol **§3** | **release** |
| R-3 Metal / Vulkan shader compile | **T4** | workstation protocol **§5** | **release** |

The M0 pre-work items are **PW-1** E-4 kNN, **PW-2** S-8 `hairTangent`, **PW-3** S-9 refineLevel
switch, **PW-4** L-1 render-context resolution, **PW-5** SI-8 auto-apply, **PW-6** the
`reorder nameChildren` notice check, gate **SI-11** (`ADR §9.1 R1`; the numbering is
`11-roadmap.md` §1's, which owns it). Each is an afternoon on this host; they run in M0 so the decisions they inform are made before
M1 writes them into the schema, and the gate itself exits at the milestone shown.

**M1's stop condition stands**: if precise `usdGen:*` invalidation cannot be demonstrated (SI-2 red),
fall back to `primvars:usdGen:*` and re-plan — do not proceed (`ADR §7`).

---

## 7. Conventions

### 7.1 C++ naming and structure

`PXR_NAMESPACE_USING_DIRECTIVE` at file scope, then `namespace usdGen { … }` — the usdRig shape
(`usdRig/libs/rigExecImaging/sceneIndices.h:29-31`). Public C++ types are `UsdGen*`; files are
`libs/<target>/<lowerCamel>.{h,cpp}`. Schema prim types are `UsdGen<Concept>` and every property is
`usdGen:`-namespaced exactly as `02-schema.md` spells it — `usdGen:enabled`, never `usdGen:active`,
which collides with USD's prim `active`. Every header that is not installed says so in its first
comment line, because `ADR §3` makes the installed/not-installed split the boundary between what may
churn until M7 and what may not — a different boundary from contract C4, the C ABI plus pxr_boost
array surface, frozen at the end of M5.

### 7.2 Diagnostics

`libs/usdGen/debugCodes.h` declares, via `TF_DEBUG_CODES(...)` and one `TF_DEBUG_ENVIRONMENT_SYMBOL`
each, exactly the six codes the siblings publish (`03-execution-engine.md` §9.2,
`09-performance-and-benchmarks.md` §6.2). The earlier draft of this section named
`USDGEN_GRAPH`/`USDGEN_SESSION`/`USDGEN_MAPS`/`USDGEN_EXPR`/`USDGEN_TIMING`; `USDGEN_GRAPH` is a
synonym of `USDGEN_COMPILE` and the rest are retired:

| Code | Prints |
|---|---|
| `USDGEN_COMPILE` | graph compile, structural digests, recompiles |
| `USDGEN_DIRTY` | locator → node → chunk → tile routing |
| `USDGEN_COMMIT` | commit reason, phase timings, cook counts |
| `USDGEN_PUBLISH` | generation number, tiles published, notices emitted |
| `USDGEN_CAPTURE` | capture epochs, kNN and map capture |
| `USDGEN_MEMORY` | cache sizes, evictions, budget pressure |

A document that needs a seventh code adds it to this table and to `debugCodes.h` in the same change.

Trace scopes are `TRACE_SCOPE("<name>")` and `TRACE_FUNCTION()` (`pxr/base/trace/trace.h:30, 35`; the
`trace` library is already linked, §1.3). The scope **names are plain and phase-shaped, never
gate-shaped**: `UsdGen::Route`, `UsdGen::Compile`, `UsdGen::Capture`, `UsdGen::Evaluate<opType>`,
`UsdGen::Interleave`, `UsdGen::Publish`, `UsdGen::Diff`, `UsdGen::Motion`. The gate each scope is
evidence for is a **column of the declaring table**, not part of the string, so a Chrome trace and a
gate report line up without embedding a gate id that R40 may reassign.
`03-execution-engine.md` §9.2 is the declaration site and owns that table;
`09-performance-and-benchmarks.md` §6.2 prints the identical one. This document does not restate it,
and a scope of the form `UsdGen::E-1::Evaluate` or `UsdGen::SI-3::Commit` is a review failure (§7.5).
(`ADR §4.1` writes the API as "TfTrace"; no such symbol exists in OpenUSD 26.08 —
`grep -rl TfTrace pxr` returns nothing. The two macros above are the API.) The scopes and the
`UsdGenStats` counters are enabled by `USDGEN_DIAGNOSTICS` (§3.5.1).

### 7.3 Error policy

* `TF_CODING_ERROR` for programmer errors only. Authored-data problems are `TF_WARN`, **deduplicated
  once per session per (prim path, condition)** so a bad groom cannot flood a render log.
* A description whose graph fails to compile (a cycle, an unknown operator type, a
  `usdGen:schemaVersion` newer than 1) publishes **zero tiles** and warns once. Partial output is
  worse than none (`design/proposal-risk.md` §11.9).
* **No exceptions cross the C ABI.** Every `extern "C"` entry point wraps its body in
  `try { … } catch (...) { … }` and never lets a `TfErrorMark` escape. Every entry point returns
  `int`, `0` on success and non-zero on error, with the message available from
  `UsdGenImaging_GetLastError()` (thread-local); the exceptions are the two value-returning
  handshakes `UsdGenImaging_GetGeneration()` and `UsdGenImaging_GetTopologyGeneration()`
  (`long long`, `-1` on error) and the two `const char *` accessors. **The full signature list is
  `08-tools.md` §1.4, the single source of contract C4 (`ADR §9.4 R31`); this document does not
  restate it.** The visibility-macro and `extern "C"` block shape is
  `usdRig/libs/rigExecImaging/registry.h:135-156`; the try/catch is usdGen's addition — usdRig has
  none (`grep -n catch usdRig/libs/rigExecImaging/registry.cpp` → no matches). usdGen's visibility
  macro is `USDGEN_IMAGING_C_API`.
* **No exceptions in the evaluator at all.** A throw inside a `tbb::task_arena` body propagates out
  of the join and leaves the session in an undefined state; kernels and operator `Evaluate` bodies
  are `noexcept`.
* Interruption is never a half-publish: a superseded commit returns the previous generation and
  **leaves the session dirty** (`ADR §4.2.4`).
* Python: `usdGenLib.py` pins `OPENBLAS_NUM_THREADS`, `OMP_NUM_THREADS` and `MKL_NUM_THREADS` to `1`
  **before** importing numpy — unpinned BLAS turned a 0.56 ms brush matmul into 22 ms on a loaded
  host (MEASURED, `research/G-tool-loop-array-transport-and-cv-picking.md` §1.5).

### 7.4 Documentation

The owner's `docs/superpowers` convention (`research/A3-usdrig-tools.md` §6): a design spec at
`docs/superpowers/specs/YYYY-MM-DD-<feature>-design.md` (sections `0. Request` … `7. Out of scope`,
facts with file:line, exact names and signatures); an implementation plan at
`docs/superpowers/plans/YYYY-MM-DD-<feature>.md` with Global Constraints and checkbox tasks that each
end with a named test command and its expected last output line; and a user doc at
`docs/<feature>.md` opening with "Design spec `docs/superpowers/specs/…`. Added <date>." Every number
carries its tag and source (§0).

### 7.5 Review checklist

A change is not reviewable until all of these are true:

1. Every new number is tagged MEASURED (with its ledger row), DERIVED, UNMEASURED (with its gate) or
   ASSUMPTION (`ADR §9.5 R42`).
2. No new `#include` matching §1.3's forbidden families outside `libs/usdGenImaging/` and
   `testutils/usdGenTestUtilsHd/` (`testUsdGenIncludeRule` derives its patterns from the one regex).
3. A new operator ships a T0 test (value) **and** a T1 test (value + emitted locator set).
4. A new published primvar states its interpolation and whether it changes per frame; if it does,
   what happens to Storm's `DirtyPoints` fastpath.
5. No `UsdStage::RemovePrim` on an interactive path (§8.2); any float reduction added is order-fixed
   (§5.5).
6. No new authored property that is machine tuning — that is an env var, and it goes in §3.5.1;
   a new CMake switch goes in §3.5.2. A new `TRACE_SCOPE` uses a plain phase name from
   `03-execution-engine.md` §9.2, never a gate id (§7.2).
7. Any change to `usdGen:` names or the tile contract after M1 needs a contract amendment (C1/C2).
8. `schema.usda` changes come with regenerated, reviewed `plugin/usdGenSchema/resources`; new
   third-party code has a `NOTICE` stanza and a pin in `cmake/usdGenThirdParty.cmake`.
9. Nothing in the diff reintroduces a rejected idea: implicit sibling wiring, `usdGen:active`,
   structural `enabled` on topology-preserving operators, a `sceneGlobals` interactive flag, widened
   chunks, array padding, `.usda` bakes, baked motion samples, or a `GetPrim` cook backstop
   (`ADR §8`).
10. Golden updates explain in one line why the image moved.

---

## 8. Known build issues carried from usdRig

### 8.1 `-ffp-contract`

GCC on aarch64 defaults to `-ffp-contract=fast` and fuses `a*b+c` into a single-rounding `FMADD`.
That flipped **3 of 36** geodesic segment traces in usdRig's `testRigExecCurvenet` — same source,
same compiler, identical residual `0.07926`, a discrete branch flip at a tolerance boundary
(MEASURED, ledger row `EV-078`; `research/B-usdrig-build.md` §4). Every usdGen kernel that walks a
surface has that shape: barycentric root tracing, guide-to-surface projection, clump-region
assignment, arc-length resampling. Hence `-ffp-contract=off` on `usdGenMath` by default (§3.1), gate
E-8 depending on it (§5.5), and a nightly `fast` build to keep epsilons honest. usdRig's tolerances
were tuned on AppleClang and are not portable as authored — usdGen states its epsilons with the
kernel.

### 8.2 The stock OpenUSD `RemovePrim` defect

`pxr/exec/esfUsd/stageData.cpp:351` fetches `_stage->GetPrimAtPath(resyncedPath)` and `:361` passes
the possibly-invalid prim straight to `UsdPrimDefaultPredicate`, which posts
`TF_CODING_ERROR("Applying predicate to invalid prim.")` (`pxr/usd/usd/primFlags.cpp:21-25`). With an
OpenExec system attached, **every** `UsdStage::RemovePrim` posts it; in Python it raises
`Tf.ErrorException`. Reproduced with zero usdRig code by
`prototypes/usdrig-linux-build/probeExecResyncRemovePrim.cpp` (`baseline=0 withExec=1 → REPRODUCED`,
MEASURED, ledger row `EV-079`), which is why usdRig's `testRigExecStageEdits` is the one red test
here. Those two line numbers were re-verified for this document; the brief (`S41`, `S46`) and
`research/B-usdrig-build.md` §5 carry the older 350/360 pair, which is off by one.

What usdGen must do: **never `RemovePrim` on an interactive path** (on the ADR's rejected list) —
undo of a live freeze is `SetActive(false)`, and real removal happens only when the entry leaves the
undo stack, in one `Sdf.ChangeBlock`, outside the interactive loop (`S41`); contain the diagnostic
anyway with a `TfErrorMark` in C++ and `try/except` plus a post-condition assert in Python, swallowing
exactly this one commentary; never attach an OpenExec system in usdGen's own tests; and file it
upstream. **Open**: whether the same error fires for `SetActive(False)` and session-layer `over`
deletes was not measured — extending the probe is ten minutes of M0 work.

### 8.3 Four smaller traps, all fixed in usdGen's own scripts

* `ImagingLibraryPath()` searched only `<repo>/` and `<repo>/build/` and raised `OSError` from
  `ctypes.CDLL` out of tree (`research/B-usdrig-build.md` §6) → `bin/_env.sh` sets
  `USDGEN_IMAGING_DLL` and `usdGenLib.py` checks it first, then `usdGen_LIBRARY_DIR`, then the two
  legacy locations, then fails naming the variable (§3.5.1, §3.6).
* `testUsdviewRigExec.py` builds its plugin container with `__new__` and sets 5 of the 12 attributes
  `registerPlugins` sets, so the suite dies in `_EnsureViewportTools` before any assertion (§6) →
  all mutable state in a `UsdGenToolState` dataclass built in `__init__` (`08-tools.md` §1.3), and
  **no test hand-constructs the container**.
* `_env.sh` hard-codes python3.11, exports only `DYLD_LIBRARY_PATH`, and points at sibling paths that
  do not exist here (§7) → §3.6.
* `ninja` is not a system binary here, and USD's bindings are in `lib/python3.12/site-packages` (§1)
  → `bin/build_usdgen.sh` probes `PATH`, then `$VIRTUAL_ENV/bin/ninja`, then falls back to
  `Unix Makefiles`, always passing `-DCMAKE_MAKE_PROGRAM` when it found one off `PATH`; every
  site-packages path is globbed.

### 8.4 Vocabulary collisions to avoid on sight

| Collision | Rule |
|---|---|
| `usdGenSchema` (our target) vs `$USD/bin/usdGenSchema` (OpenUSD's generator) | always invoke the generator by absolute path; never name a target from a `PATH` lookup (§4.2) |
| `T0`–`T4` test tiers vs `T-1`…`T-5` tool gates vs `usdGen:frozen:tier` tokens | tiers have no hyphen, gates do, freeze tiers are tokens (§5.1) |
| brief `S1`–`S46` vs Storm gates `S-1`–`S-12` vs milestones | milestones are **M0–M8** and never "S"; brief numbers are bare (`S23`); gates always carry the hyphen (`09-performance-and-benchmarks.md` §5.3 runs S-1…S-12) |
| risk-register items `R1`–`R16` vs render gates `R-1`–`R-3` | register items are bare (`R9`, used in §3.1, §3.2, §5.5); gates carry the hyphen (`R-1`, §6.5) |
| gate families `E- SI- S- L- T- T-INST- R- B-` | all hyphenated (`ADR §9.1 R2`); `B-` is this document's build family, `L-` the look family of `07-look-maps-expressions.md` §9.1 |
| third-party patches `TP-1`–`TP-8` (§2.4) vs motion profiles `P0/P1/P2` vs engine invariants `I1`–`I8` | patches are `TP-n`; the ADR's old engine principles `P1–P8` are now `I1–I8`, and `P0/P1/P2` mean motion profiles only (`ADR §9.1 R1`) |
| pre-work `PW-1`–`PW-13` (`11-roadmap.md` §1) vs gates | pre-work items are `PW-n`, never `P-n`; a pre-work item *runs* a gate, it is not one. `PW-1`–`PW-6` are the six M0 decision checks (§6.5); `PW-7`–`PW-13` are harness pre-work whose gates bind later |
| `chunk` (512 curves, the dirty/parallel unit) vs `tile` (a Hydra `basisCurves` prim) | never write "chunk prim"; `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` (`ADR §9.3 R21`) |

---

## 9. Testing

What proves *this* document, as opposed to the features it builds. Every row is a tier T0–T4 and a
gate id where one exists.

| Claim | Tier | Test | Gate |
|---|---|---|---|
| `usdGen`, `usdGenMath` and `usdGenTestUtils` link no stage or Hydra library, and include no forbidden header | T0 | `testUsdGenLinkRule_*`, `testUsdGenIncludeRule` | **B-1** (M0) |
| Vendored third party escapes neither `libusdGen.so` nor the install tree | T0 | `testUsdGenNoThirdPartyExports`, `testUsdGenInstallTree` | — (SeExpr M0, Ptex M4) |
| The build is offline and rigExec-optional; a consumer may call `find_package(pxr)` and `find_package(usdGen)` together | T0 | `testUsdGenOfflineConfigure`, `testUsdGenConsumer`; the `configure-offline` and `with-rigexec` jobs | — (M0) |
| Every licence has a `NOTICE` stanza | T0 | `testUsdGenNotice` | — |
| Kernels are deterministic across thread counts | T0 | `testUsdGenKernelDeterminism` | **E-8** |
| `t0`+`t1` wall clock and per-test budgets hold | T0 | the `t0`+`t1` job's `--output-junit` check, plus the `nightly-perf` CSV | — (§6.3) |
| Plug discovers every shipped plugin from `bin/_env.sh` with no direct link | T1 | `testUsdGenPluginDiscovery` | — (M0 schema + imaging, extended M1 and M5) |
| The checked-in schema resources match `schema.usda` | T1 | `testUsdGenSchemaUpToDate` | — |
| Auto-applied `UsdGenMaskAPI` works on a codeless type | T1 | `testUsdGenAutoApply` | **SI-8** |
| Five adapter entries make every `usdGen:*` property visible and dirtyable | T1 | `testUsdGenAdapter`, `testUsdGenRestAdapter` | **SI-7** |
| The scene index lands in the right chain slot | T1 | `testUsdGenChainOrder` | **SI-5** |
| The extent function registers from `libusdGenSchema.so` and `usdcat`/`usdrecord` see a bound | T1 | `testUsdGenPluginDiscovery` (extent leg) | — (M1) |
| The EGL harness renders on the GB10 with zero GL errors and the golden matches | T2 | `testUsdGenStormLook` | **L-2** |
| Storm throughput and batching hold at 100 k | T2 | `benchUsdGenStorm --static`, `--batches` | **S-1**, **S-5** |
| The `testusdview` suites run against both the build tree and the install tree | T3 | the eleven scripts of §5.6 | — (M5) |
| hdPrman parity, MSAA/OIT, Metal/Vulkan compile | T4 | `docs/workstation-protocol.md` §§3, 5, 7 | **R-1**, **R-2**, **R-3** (release) |

§6.5 maps every remaining gate onto its test and milestone. What this document does **not** prove:
any feature behaviour, any threshold value (those live in `09-performance-and-benchmarks.md` §5), and
anything in tier 4, which by construction cannot run here.

---

## 10. Out of scope

* **Windows and macOS builds** beyond keeping the CMake honest. The two known blockers are recorded
  (bison/flex → §2.4 TP-3; the Hgi resource path → gate R-3). A porting slice is post-M8.
* **Packaging**: no `.deb`, `.rpm`, conda recipe or pip wheel. The deliverable is a CMake install
  tree (§3.4).
* **A Ptex-enabled OpenUSD build.** Ptex is CPU-side at capture only; Storm's Ptex path is compiled
  out of this install and is mesh-only anyway (`S37`, `research/A8-seexpr-ptex-libs.md` §2.9).
* **hdPrman in CI.** No RenderMan on this host; R-1 stays a tier-4 protocol.
* **Multi-GPU / multi-vendor CI.** One GB10, one driver.
* **Cross-compilation and static-only distributions.**
* **A third-party operator ABI** and an OpenExec backend — both v3 (`ADR §3`, `S16`); neither appears
  in the target graph.
* **Installed engine headers before M7** (`ADR §3`).

---

## 11. Sources

| Source | Sections used |
|---|---|
| `design/adr-v1.md` | §1 (chunk≠tile, ordering tag), §2.1–§2.3, §3 (contracts C1–C5), §5.1, §5.4, §6 build, §7 (milestones, gates), §8 (document map, rejected ideas), **§9.1 R1–R5, §9.2 R6/R7/R8/R11/R19, §9.3 R21/R27, §9.4 R29/R31/R35/R37, §9.5 R38/R39/R40/R41/R42/R43/R45** |
| `design/brief-v1.md` | S1, S5, S8, S9, S16, S22, S23, S27, S37–S46 |
| `design/proposal-risk.md` | §2 (targets, CMake specifics), §7.3 (`rand()`), §9.1 (tiers, the "≈15 ms before GPU time" arithmetic), §10–§11 |
| `design/proposal-performance.md` | §3.1–§3.3 (rebuild-cost rationale), §5 (`UsdGenStats`), §11.2 |
| `design/judge-evidence.md` / `judge-delivery.md` | §0, §1 D8 (the link rule); §4, §7 (tiers, `_env.sh`, fixture drift, T4 as release criteria) |
| `research/ENVIRONMENT.md` | all, with the CORRECTIONS block overriding the body (no Xvfb, ninja in the venv, bindings in `lib/python3.12/site-packages`) |
| `research/B-usdrig-build.md` | §1, §2 (timings, `-Wcpp`), §4 (`-ffp-contract`), §5 (`RemovePrim`), §6 (headless Storm, Xvfb layout, testusdview), §7 (env snippet, plugin discovery), §8 (double-`pxrConfig`), decisions 2, 4, 7 |
| `research/A8-seexpr-ptex-libs.md` | §1.9, §2.2, §2.6, §2.9, §5, §6.2, §7, key facts |
| `research/A3-usdrig-tools.md` | §5 (test patterns and runners), §6 (spec and plan formats) |
| `research/G-storm-hair-look-prototype.md` | §0 (EGL harness, the ~25 `invalid enum` errors), §2.1, §5, §6 (workstation protocol, the 172 MB stage) |
| `research/G-tool-loop-array-transport-and-cv-picking.md` | §1.5 (BLAS pinning), §2 (pick costs) |
| `research/G-data-plane-engine-prototype-benchmark.md` | §3.3 (thread scaling, the 8-thread knee), host line (the CPU split) |
| `research/G-chain-order-probe.md` | §2–§3 (chain slot), §5 (rig and terminal-traversal frame costs) |
| `appendix-A-evidence-ledger.md` | §1.1, §2.0 (the retired-handle map), §2.1 (`EV-008`), §2.5 (`EV-035`, `EV-036`), §2.10 (`EV-076`…`EV-080`), §2.11 |
| `prototypes/` | `storm-hair-look/{eglctx.h, bench_hair.cpp, render_hair.cpp, *.glslfx}`; `storm-throughput/{gen_hair_stages.py, hairbench.cpp}`; `usdrig-linux-build/{rigexec_env.sh, consumer/, probeExecResyncRemovePrim.cpp}`; `chain-order/stages/skelAndRig.usda`; `data-plane-benchmark/{kernel.h, sampler.h, tbbBench.cpp}`; `thirdparty-bench/` |
| Vendored sources (read for §2.4) | SeExpr `src/SeExpr2/CMakeLists.txt:53, 67, 85-86`, `ExprBuiltins.cpp:1719-1849`, `Noise.cpp:219-234`, `CMakeLists.txt:98, 202`; Ptex `CMakeLists.txt:11-19, 34, 36`, `src/ptex/CMakeLists.txt:1-2, 27, 46-55`, `src/build/CMakeLists.txt:14, 18`, `src/utils/CMakeLists.txt:9` |
| OpenUSD 26.08 source | `plug/initConfig.cpp:25-26, 33-62`; `plug/thisPlugin.h:20-22, 28-30`; `plug/plugin.cpp:218-222`; `usd/schemaRegistry.cpp:59, 836-900, 865-867`; `usdGeom/boundableComputeExtent.cpp:155-165, 271-289`; `usdShade/shaderDefUtils.cpp:51-52`; `plugin/usdShaders/{plugInfo.json:13, CMakeLists.txt:5-8, discoveryPlugin.cpp:29-48}`; `sdr/discoveryPlugin.h:134`; `base/trace/trace.h:30, 35`; `hdSt/renderDelegate.cpp:695-707`; `esfUsd/stageData.cpp:351, 361`; `usd/primFlags.cpp:21-25` |
| OpenUSD 26.08 install and CMake | `pxrConfig.cmake:17, 52-58`; `cmake/pxrTargets.cmake:57, 176-179, 483-486, 499-502`; `objdump -p lib/libusd_sdf.so`; `plugin/usd/plugInfo.json`; `/usr/share/cmake-3.28/Modules/CMakeFindDependencyMacro.cmake:57-83`; `/usr/share/cmake-3.28/Help/prop_tgt/SYSTEM.rst:16-19` |
| usdRig source | `CMakeLists.txt:14-16, 31-39, 409-421, 410-460, 433-437, 461-463, 495-520`; `bin/gen_schema.sh:11-14`; `bin/_env.sh:41-55`; `libs/rigExecSchema/schema.usda:18-27`; `libs/rigExecImaging/{sceneIndices.h:29-31, registry.h:135-156}`; `tests/probeImagingPipeline.cpp` |

**Sibling registries this document depends on, re-checked 2026-09-05.**
`09-performance-and-benchmarks.md` §5 agrees with §6.5 on the tier and milestone of all fifty-three gates,
and its §4.3 prints `docs/workstation-protocol.md` as §§1–11 (§5.2); `02-schema.md` §7.2 and
`06-imaging.md` §8 both route `UsdGeomRegisterComputeExtentFunction` and the schema `LibraryPath` to
`libusdGenSchema.so` (`ADR §9.2 R19`, §1.2, §4.3); `06-imaging.md` §9 prints §7.2's six `TF_DEBUG`
codes, cites §3.5 for the env registry and uses the plain trace-scope names;
`07-look-maps-expressions.md` §9.2 and `08-tools.md` §9.3 use §5.6's test names;
`12-risks-decisions-open-questions.md` and `appendix-A-evidence-ledger.md` §5 cite SI-9, SI-10, SI-11
and S-12 as §6.5 does; and 09 §5.1's B-1 row names `USDGEN_FORBIDDEN_LIB_REGEX` /
`USDGEN_FORBIDDEN_INC_REGEX` as §3.5.2 registers them.
