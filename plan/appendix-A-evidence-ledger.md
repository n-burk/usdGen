# Appendix A: evidence ledger

Date: 2026-09-04. Status: plan v1 (draft, pending review).

Every number, API name and file:line the usdGen plan leans on is recorded here once, with the probe that produced it, the report section that reports it, and the caveat that limits it. Sibling documents cite this appendix for provenance rather than repeating it; the citation handle is a §2 row id, `EV-nnn` (ADR §9 R1). Nothing here is new design — it is the audit trail. Two rules govern the file: **no number appears without a method**, and **no number that was interpolated, extrapolated or assumed is written as a measurement**.

Reads with: `09-performance-and-benchmarks.md` (the single gate registry and the single frame ledger, ADR §9 R40–R41), `10-build-dependencies-testing.md` (test tiers, EGL/Xvfb harnesses, the env-var registry), `appendix-B-prototype-inventory.md` (what each prototype directory holds and how to build it), `12-risks-decisions-open-questions.md` (which §5 gaps are still open), `README.md` (the index and the status vocabulary).

---

## 0. How to read this ledger

### 0.1 The four tags (ADR §9 R42)

| Tag | Meaning | Obligation |
|---|---|---|
| **MEASURED** | A probe was built and run and printed this number. | Cite the `EV-nnn` row. |
| **DERIVED from `EV-nnn`** | Scaled, interpolated or summed from measured inputs. **Never written as MEASURED** (ADR §9 R27). | Name every input row, and the gate that would measure the derived quantity directly. |
| **UNMEASURED** | The plan depends on the quantity and nobody has produced it at all. | Cite the §5 gate id and the milestone (M0–M8) it exits, or `release` for a tier-4 gate. |
| **ASSUMPTION** | A judgement no gate will settle: staffing, a per-rig-class budget, an extrapolation from someone else's benchmark with no probe planned. | Say what would falsify it. |

ASSUMPTION is not limited to staffing. `09-performance-and-benchmarks.md` §0.2 tags the per-tile `extent` term ASSUMPTION and nothing else; the ≈10 ms kNN-capture ASSUMPTION is carried in §2.11 here and in `03-execution-engine.md` §4.4, and the gate that settles it, **E-4**, is registered in `09-performance-and-benchmarks.md` §5.1. ADR §9 R26 (guides are 1–10 % of hairs) and R28 (SI-6's bounded traversal ≤ 5 ms) are ASSUMPTIONs this appendix records in §2.11. Three corollaries the panel enforced:

1. **An interpolated number is not measured.** "Storm ≈ 12.2 ms at 100 k curves" sits between measured endpoints (`EV-020`, `EV-021`): DERIVED, gate **S-1**.
2. **A derived number is not measured** even when every input is. "usdGen total 0.8–1.0 ms" is arithmetic over §2.1–§2.2, §2.4–§2.5 and §2.9: DERIVED, gates **E-1** and **S-2**.
3. **A CPU-rasteriser number is never a GPU number.** Rows `EV-053`–`EV-054` came from llvmpipe; they bound the CPU half of a freeze and say nothing about Storm draw cost.

### 0.2 What "this host" means

All §2 rows were produced on one machine on 2026-09-04, in one of three configurations:

| Id | Configuration | Measures |
|---|---|---|
| **H1** | CPU only, no graphics context. aarch64, 20 cores, GCC 13.3, `-O2`/`-O3`. | Engine, scene index, USD authoring, transport, adapters, third-party libs. |
| **H1-GPU** | Storm on the measurement host through the EGL harness `prototypes/storm-hair-look/eglctx.h` (compatibility profile, 64×64 pbuffer). | Real Storm GPU frame times. |
| **H1-SW** | Storm on Mesa llvmpipe through a user-space Xvfb on `DISPLAY=:77`. | Storm *correctness* and app-loop behaviour only; never a Storm frame time. |

H1 is heterogeneous (10× Cortex-X925 + 10× Cortex-A725). Both the bespoke TBB engine and VDF regress past ~8–10 threads on it (rows `EV-008` and `EV-011`; `research/G-data-plane-engine-prototype-benchmark.md` §3.3), which is why the design pins a private `tbb::task_arena` at 8 workers with a one-shot calibration and `USDGEN_THREAD_LIMIT` (ADR §4.1, invariant I8) rather than hard-coding 8.

### 0.3 Conventions

Paths are relative to `plan/` (`<usdgen-src>/plan`): `research/<file>.md`, `design/<file>.md`, `prototypes/<dir>/<file>`. Siblings are cited **by section**, never by line number; only source files carry `file:line`. OpenUSD citations are relative to `<openusd-src>` at tag **v26.08** (`ee47c679a`), usdRig citations to `<usdrig-src>` at `c92c040`; a citation outside `pxr/` names its full path from the source root (`extras/imaging/docs/…`). Generated headers are cited by their `.in` template (`pxr/pxr.h.in`), with the install copy named where it differs. This appendix carries no probe build lines (`appendix-B-prototype-inventory.md`) and no design rationale (the ADR and the numbered documents). It restates a gate id or a threshold only where the claim is unintelligible without it; **`09-performance-and-benchmarks.md` §5 owns every gate id, tier, milestone and threshold, and wins on conflict** (ADR §9 R40).

---

## 1. Host and toolchain facts

`research/ENVIRONMENT.md` **with its CORRECTIONS block applied**, plus host facts first established in the G-reports, each cited inline. §1.6 lists what was corrected.

### 1.1 Hardware and OS

Linux 6.17, **aarch64**, 20 CPUs = **10× Cortex-X925 + 10× Cortex-A725** (`lscpu`; `research/G-data-plane-engine-prototype-benchmark.md:27-30` — ENVIRONMENT.md says only "20 CPUs"). SVE2 is present but GCC 13.3 emits only 16-byte NEON for these kernels, even with `-mcpu=native` (§2.2). **measurement host**, driver **580.173.02**, GL 4.6 compatibility through EGL (`research/G-storm-hair-look-prototype.md` §5, `:29`, `:97`). No `DISPLAY`, no root. `perf` is refused (`perf_event_paranoid = 4`) and `gdb -p` by yama `ptrace_scope` (same report `:29-30`, §8), so profiling uses the in-process SIGPROF sampler `prototypes/data-plane-benchmark/sampler.h`. Toolchain: g++ 13.3.0, cmake 3.28.3, ninja 1.13.2 (venv only — pass `-DCMAKE_MAKE_PROGRAM`). **GCC defaults to `-ffp-contract=fast` on aarch64**, which flips branches in surface-walking kernels (`EV-078`) — hence `-ffp-contract=off` on `usdGenMath`.

### 1.2 OpenUSD source and install

Source `<openusd-src>` at v26.08, read-only; install `$USD`, `PXR_VERSION "2608"` (`pxrConfig.cmake:17`). Python bindings live in **`lib/python3.12/site-packages`**, not `lib/python`. Bundled: MaterialX 1.39.5 (incl. `libMaterialXGenGlsl`), OpenSubdiv 3.6.1, oneTBB 2020.3.1; **no Ptex, no OpenImageIO, no SeExpr**. Installed plugins are exactly `hdStorm`, `hioAvif`, `hioOpenEXR`, `sdrGlslfx`, `usdShaders` — **hdPrman is not built here** (`PXR_BUILD_PRMAN_PLUGIN` OFF, `cmake/defaults/Options.cmake:27`), so every hdPrman statement in the plan is source-verified, never run-verified. Hio built-in formats are `bmp, jpg, jpeg, png, tga, hdr` (`pxr/imaging/hio/plugInfo.json:8`) plus the EXR and AVIF plugins; no tif, no `.tx`, no Ptex. Hgi: **hgiGL only**, so H1-GPU is always the GLSL resource path (`HDST_ENABLE_HGI_RESOURCE_GENERATION` false, `pxr/imaging/hdSt/codeGen.cpp:166`). `PXR_ENABLE_PTEX_SUPPORT` is OFF (`Options.cmake:36`), so Storm cannot sample `.ptx` and Ptex is a CPU-at-capture library. Exec **is** built (`UsdExecImagingCreateStageSceneIndex()` returns non-null) but gated behind `USDIMAGINGGL_ENGINE_ENABLE_EXEC_SCENE_INDEX`, default false (`engine.cpp:91`).

### 1.3 Python and third-party

Interpreter `$VENV/bin/python3` = 3.12.3; PySide6 6.11.2 and PyOpenGL importable. **numpy 2.5.2 and pybind11 3.1.0 ARE installed**; Python dev headers at `/usr/include/python3.12/Python.h`. `pxr_boost.python` is reachable out of tree: `include/pxr/external/boost/python.hpp`, `lib/libusd_boost.so`, `lib/libusd_python.so`, and `pxr/pxr.h.in:48`, `:59` (generated as `<install>/include/pxr/pxr.h`, identical line numbers here) define `PXR_PYTHON_SUPPORT_ENABLED` / `PXR_USE_INTERNAL_BOOST_PYTHON`. SeExpr `main` @ `8f8c8f2` and Ptex `v2.4.3` were built from source in the scratchpad; bison 3.8.2, flex 2.6.4 and zlib dev are present, **LLVM dev headers are not**, so SeExpr's LLVM backend is unavailable here.

### 1.4 Graphics: what can and cannot run headlessly

| Capability | Status | How |
|---|---|---|
| Headless scene-index work (`UsdImagingCreateSceneIndices`, notices, `GetPrim`) | **Yes**, no GL | the tier-1 harness, sub-100 ms |
| **Storm on the measurement host** | **Yes** | `EGL_EXT_platform_device`, **compatibility** profile, 64×64 **pbuffer** (`prototypes/storm-hair-look/eglctx.h`). A core profile gives ~25 `GL_INVALID_ENUM`/frame and a white image; surfaceless fails in `HgiGL_ScopedStateHolder` (`research/G-storm-hair-look-prototype.md` §0 Key facts, `:37`, `:412`) |
| `usdview` / `testusdview` / `usdrecord --renderer GL` correctness | **Yes**, software | user-space Xvfb `:77` + llvmpipe (LLVM 20.1.2, GL 4.5 core), `dpkg-deb -x`'d, no root |
| `usdrecord` on the GPU | **No** | its `main` opens a GLX window first; link `eglctx.h` into your own driver |
| GPU-accelerated GLX | **No** | zink needs DRI3, which Xvfb lacks |
| Metal / Vulkan Hgi path | **No** | only `libusd_hgiGL.so` is built; `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` is the proxy check (gate R-3) |

Why H1-GPU works: **Storm never creates a GL context.** `HgiGL` only calls `GarchGLApiLoad()` (`imaging/hgiGL/hgi.cpp:54`), which resolves through `glXGetProcAddressARB` (`imaging/garch/glApi.cpp:3142`); under GLVND that dispatches against whatever context is current, including an EGL one. `garch`'s own context creation is GLX-only (`imaging/garch/glPlatformContextGLX.cpp`), but nothing in hdSt/hdx/hgiGL/usdImagingGL references `GlfGLContext` outside test harnesses.

### 1.5 usdRig

Branch `main`, commit `c92c040`, tree clean; read-only except `plan/`. Builds out of tree in 21.45 s (§2.10) and **requires `-DCMAKE_CXX_FLAGS=-ffp-contract=off`**. 26 of 27 CTest suites pass; the failure is a stock OpenUSD defect (§3.10). `bin/_env.sh` hard-codes python3.11 and exports only `DYLD_LIBRARY_PATH`; the working Linux snippet is `prototypes/usdrig-linux-build/rigexec_env.sh`. An out-of-tree build **requires `RIGEXEC_IMAGING_DLL`** (`plugin/rigExecUsdview/rigExecUsdview.py:31-41`).

### 1.6 What was corrected in ENVIRONMENT.md, and why

**Six statements, in three clusters,** in the body of `research/ENVIRONMENT.md` are superseded by its own CORRECTIONS block and by the verification round: rows **K1** (three linked GL claims), **K2** (two toolchain claims) and **K2b** ("No Xvfb installed" — an Xvfb unpacked into user space serves `DISPLAY=:77`, which is what makes tier T3 a CI tier) in §6. Three change the plan's shape.

The EGL harness makes **test tier T2 a CI tier instead of a workstation protocol**, so every `S-` gate and `L-1` become milestone exits instead of release criteria: S-8, S-9 and L-1 as M0 pre-work binding at M1; S-1, S-5, S-6, S-12 and L-2 at M1; S-2, S-3 and S-4 at M2; L-3, L-4 and L-5 at M4; S-7 at M5; S-11 at M7; S-10 at M8. The `S-` family runs S-1…S-12 and the `L-` family L-1…L-5, all tier T2 except L-3 (T0), L-4 (T0/T1) and L-5 (T1); `09-performance-and-benchmarks.md` §5.3–§5.4 is the registry (ADR §9 R40) and these assignments were re-checked against it on 2026-09-05. Only R-1, R-2 and R-3 stay tier-4 release criteria. The user-space Xvfb makes tier T3 (`testusdview`) reachable, which is what gates T-1, T-3, T-4, T-5 and T-INST-1 run on. And numpy's presence is what makes the brush kernel a numpy kernel over a zero-copy `np.asarray(vt)` view (§2.7) instead of a C++ callback.

---

## 2. Measured numbers

**Every row in §2.1–§2.10 and §2.12 is MEASURED on 2026-09-04**, on the host named in each subsection's preamble; a range is min–max over the probe's repetitions **unless the row names another statistic**, which it must where the probe printed min/median/max (`EV-011`). **§2.11 is the deliberate exception:** it lists the DERIVED and ASSUMPTION figures the plan uses, so that nobody promotes one by accident.

Row ids are **`EV-001`…** (ADR §9 R1), flat across the whole section and independent of subsection, because a per-subsection namespace collides with the brief's S-numbers, the contracts C1–C5 and the milestones M0–M8 when a sibling shortens it. Sibling documents cite `EV-nnn` and nothing else. Three earlier schemes are retired and §2.0 maps them row for row: this file's own `A2.n-Xn`, and `09-performance-and-benchmarks.md` §1's `Q<n>` shorthand with its "A-row" column and its later 100-block `EV-1nn`…`EV-9nn` ids (09 §1 now cites the flat ids directly). Gate families are `E-`, `SI-`, `S-`, `L-`, `T-`, `T-INST-`, `R-` and `B-` (ADR §9 R2). Every subsection names its probe path.

### 2.0 Legacy handle map (retired ids → `EV-nnn`)

This is a **row-for-row** map, not a range map: every retired handle resolves to exactly one `EV-nnn` (two, where a row was split), and its last column doubles as a one-line index of the ledger. Column 1 is this file's own retired per-subsection scheme (`A2.n-Xn`) and column 2 the live handle. Column 3 is the **100-block** scheme an earlier draft of `09-performance-and-benchmarks.md` §1 used — block base `EV-0nn` for §2.1, `EV-1nn` §2.2, `EV-2nn` §2.3, `EV-3nn` §2.4, `EV-4nn` §2.5, `EV-5nn` §2.6, `EV-6nn` §2.7, `EV-7nn` §2.8, `EV-8nn` §2.9, `EV-9nn` §2.10, `EV-95n` §2.12, with the ordinal carried over from column 1. No plan document cites a retired handle any more (re-checked 2026-09-05); columns 1 and 3 are kept only so an older copy of a document still resolves. The `Q<n>` / "A-row" shorthand that stood here is retired and is not reproduced.

| retired ledger id | `EV-nnn` | 09's retired 100-block id | measurement |
|---|---|---|---|
| `A2.1-E1` | EV-001 | EV-001 | 5-op chain full run |
| `A2.1-E2` | EV-002 | EV-002 | 1 % contiguous chunk invalidation |
| `A2.1-E3` | EV-003 | EV-003 | 1 % scattered invalidation |
| `A2.1-E4` | EV-004 | EV-004 | last-styler parameter edit |
| `A2.1-E5` | EV-005 | EV-005 | first-styler parameter edit |
| `A2.1-E6` | EV-006 | EV-006 | 1 M curves, full and sparse |
| `A2.1-E7` | EV-007 | EV-007 | peak RSS at 1 M |
| `A2.1-E8` | EV-008 | EV-008 | thread sweep 1→20, the 8-thread knee |
| `A2.1-E9` | EV-009 | EV-009 | chunk-size sweep |
| `A2.1-E10` | EV-010 | EV-010 | `VtArray` publish and the CoW detach |
| `A2.1-E11` | EV-011 | EV-011 | VDF strip/request-all |
| `A2.1-E12` | EV-012 | EV-012 | VDF pool invocations and the affects mask |
| `A2.1-E13` | EV-013 | EV-013 | `VdfScheduler::Schedule` per node |
| `A2.2-V1` | EV-014 | EV-101 | AoS `GfVec3f[]` |
| `A2.2-V2` | EV-015 | EV-102 | SoA, 8 planar arrays |
| `A2.2-V3` | EV-016 | EV-103 | SoA "strip", 3 planar arrays |
| `A2.2-V4` | EV-017 | EV-104 | `-fno-tree-vectorize` |
| `A2.2-V5` | EV-018 | EV-105 | `-mcpu=native` |
| `A2.3-ST1` | EV-019 | EV-201 | 4 000 curves, refine 0/1/2/3 |
| `A2.3-ST2` | EV-020 | EV-202 | 40 000 curves |
| `A2.3-ST3` | EV-021 | EV-203 | 200 000 curves (the 8.17 vs 23.93 ms lever) |
| `A2.3-ST4` | EV-022 | EV-204 | per-frame deform delta, ms per MB |
| `A2.3-ST5` | EV-023 | EV-205 | refineLevel 3 costs nothing over 2 |
| `A2.4-CPU1` | EV-024 | EV-301 | points interpolater resolve |
| `A2.4-CPU2` | EV-025 | EV-302 | thrown-away `VtArray(n)` alloc |
| `A2.4-CPU3` | EV-026 | EV-303 | padded points, with and without `curveIndices` |
| `A2.4-CPU4` | EV-027 | EV-304 | cubic index build, warm and cold |
| `A2.4-CPU5` | EV-028 | EV-305 | index build vs prim count |
| `A2.4-CPU6` | EV-029 | EV-306 | points resolve vs prim count |
| `A2.4-CPU7` | EV-030 | EV-307 | `ComputeDirtyLocators` + descriptor recompute |
| `A2.4-CPU8` | EV-031 | EV-308 | dependency-forwarding fan-out |
| `A2.4-CPU9` | EV-032 | EV-309 | scene-index bookkeeping per deform frame |
| `A2.4-CPU10` | EV-033 | EV-310 | varying-primvar CPU expansion |
| `A2.4-CPU11` | EV-034 | EV-311 | which locators are visible after a primvar appears |
| `A2.4-CPU12` | **EV-084** | EV-312 | `HdRetainedSceneIndex::DirtyPrims` notice delivery (added here, §2.4) |
| `A2.5-RX1` | EV-035 | EV-401 | `RigExecImaging_SetTime` per frame |
| `A2.5-RX2` | EV-036 | EV-402 | + terminal traversal, + two pass-through indices |
| `A2.5-RX3` | EV-037 | EV-403 | notice cascade, 1→5 000 scalps |
| `A2.5-RX4` | EV-038 | EV-404 | cook counts for 10 interactive events |
| `A2.5-RX5` | EV-039 | EV-405 | 16 734 reads, 0 torn |
| `A2.6-FZ1` | EV-040 | EV-501 | `SubtreeSnapshot` capture / undo / redo |
| `A2.6-FZ2` | **EV-041 and EV-042** | EV-502 | freeze author: C++ (EV-041) and Python (EV-042) — one retired row, two probes |
| `A2.6-FZ3` | EV-043 | EV-503 | sublayer insert / remove / mute / unmute |
| `A2.6-FZ4` | EV-044 | EV-504 | `RemovePrim` vs `SetActive(false)` |
| `A2.6-FZ5` | EV-045 | EV-505 | USD→Hydra freeze total 0.55 / 0.59 / 0.59 ms |
| `A2.6-FZ6` | EV-046 | EV-506 | bake sizes and author times |
| `A2.6-FZ7` | EV-047 | EV-507 | reopen + first `points.Get()`, `.usdc` vs `.usda` |
| `A2.6-FZ8` | EV-048 | EV-508 | `primvars:rest` sharing the `points` buffer, +26 B |
| `A2.6-FZ9` | EV-049 | EV-509 | composition: sublayer / reference / payload |
| `A2.6-FZ10` | **EV-050 and EV-051** | EV-510 | usdview author (EV-050) and `_resetGUI` (EV-051) — one retired row, two numbers |
| `A2.6-FZ11` | EV-052 | EV-511 | parent-scope `typeName` resync blast |
| `A2.6-FZ12` | EV-053 | EV-512 | llvmpipe redraw (never a Storm number) |
| `A2.6-FZ13` | EV-054 | EV-513 | live usdview loop |
| `A2.7-TL1` | EV-055 | EV-601 | `VtArray` across pxr_boost.python |
| `A2.7-TL2` | EV-056 | EV-602 | every `float*` route |
| `A2.7-TL3` | EV-057 | EV-603 | `Vt.Vec3fArray.FromBuffer` / `attr.Set(numpy)` |
| `A2.7-TL4` | EV-058 | EV-604 | `attr.Set(Vt.Vec3fArray)` |
| `A2.7-TL5` | EV-059 | EV-605 | `np.asarray(vt)` zero-copy view |
| `A2.7-TL6` | EV-060 | EV-606 | Python list routes |
| `A2.7-TL7` | EV-061 | EV-607 | one brush move, 21.3 µs |
| `A2.7-TL8` | EV-062 | EV-608 | sparse indexed push |
| `A2.7-TL9` | EV-063 | EV-609 | release write |
| `A2.7-TL10` | EV-064 | EV-610 | CV pick in C++ over ctypes |
| `A2.7-TL11` | EV-065 | EV-611 | numpy equivalents, BLAS pinned vs unpinned |
| `A2.7-TL12` | EV-066 | EV-612 | freeze authoring and `layer.Export()` |
| `A2.8-X1` | EV-067 | EV-701 | SeExpr2 ns/eval |
| `A2.8-X2` | EV-068 | EV-702 | expression prep |
| `A2.8-X3` | EV-069 | EV-703 | VarBlock threaded eval |
| `A2.8-X4` | EV-070 | EV-704 | Ptex filter lookups |
| `A2.9-MB1` | EV-071 | EV-801 | velocity extrapolation pass |
| `A2.9-MB2` | EV-072 | EV-802 | `lerp(P0, P1, a)` pass |
| `A2.9-MB3` | EV-073 | EV-803 | finite-difference velocity pass |
| `A2.9-MB4` | EV-074 | EV-804 | copy one sample (the detach floor) |
| `A2.9-MB5` | EV-075 | EV-805 | 19.2 MB per point sample |
| `A2.10-B1` | EV-076 | EV-901 | configure / clean build / ctest / install |
| `A2.10-B2` | EV-077 | EV-902 | incremental builds |
| `A2.10-B3` | EV-078 | EV-903 | `-ffp-contract=fast` flips 3 of 36 traces |
| `A2.10-B4` | EV-079 | EV-904 | `RemovePrim` with an `ExecUsdSystem` attached |
| `A2.10-B5` | EV-080 | EV-905 | 26 / 27 CTest suites pass |
| `A2.12-AD1` | EV-081 | EV-951 | codeless prim with no adapter |
| `A2.12-AD2` | EV-082 | EV-952 | no notice for a schema-declared `usdGen:*` attribute |
| `A2.12-AD3` | EV-083 | EV-953 | `GetNames()` lists attributes only |

Rows added after the retired schemes were dropped have no column-1 or column-3 handle: **EV-084**
(above; the row 09 requested and an older draft of 09 called `EV-312`), **EV-085**–**EV-088** (§2.4),
**EV-089**–**EV-090** (§2.6), **EV-091** (§2.3) and **EV-092** (§2.10).

### 2.1 Engine data plane — bespoke TBB DAG

Host **H1**. Probes `prototypes/data-plane-benchmark/tbbBench.cpp`, `vdfBench2.cpp`; raw output `results_main.txt`, `results_scale.txt`. Report `research/G-data-plane-engine-prototype-benchmark.md` §3–§5. Scene unless stated: 100 000 curves × 8 CV = 800 000 `GfVec3f` (9.6 MB), 5 styler nodes, ≈16 flops/CV, chunk 512. **Rows quote the 8-thread private-arena figure with the 20-thread figure in parentheses (ADR §9 R27); the raw probe default was 20 threads.**

| # | Value | Scene / size | Caveats |
|---|---|---|---|
| EV-001 | **1.02 ms** full chain at 8 threads (1.72–1.91 ms at 20; cold 2.12) | default | The 8-thread number is the design baseline — the arena is pinned at the measured knee (`EV-008`); 20 threads is 1.75× slower. Synthetic kernel: a floor, not a prediction |
| EV-002 | **0.035–0.044 ms** after a 1 % contiguous chunk invalidation | default | 1 % of chunks dirty on all 5 nodes; 20 threads |
| EV-003 | **0.036–0.040 ms** after a 1 % scattered invalidation | default | 20 threads |
| EV-004 | **0.179–0.411 ms** after editing the **last** styler's parameter | default | Requires per-node output buffers (S24) |
| EV-005 | **1.751–1.969 ms** after editing the **first** styler's parameter | default | ≈ a full 20-thread run |
| EV-006 | **7.59–7.67 ms** full (min–max of 4 reps, `prototypes/data-plane-benchmark/results_scale.txt:9-12`); **0.29 ms** at 0.5 % sparse | 1 M curves × 8 CV | 20 threads; the 8-thread figure at 1 M is UNMEASURED (gate **E-1**). The 0.29 ms sparse figure is the report's run (`research/G-data-plane-engine-prototype-benchmark.md` §3.1 and §5) and is not in the carried output |
| EV-007 | **668 MB** peak RSS with per-node buffers; **293 MB** with one shared buffer | 1 M × 8 CV | Per-node caching is what buys `EV-004`; budget ≈ 6× the curve data |
| EV-008 | **3.90 / 3.01 / 1.66 / 1.02 / 1.79 ms** at 1 / 2 / 4 / 8 / 20 threads | default | **The 8-thread knee** and the source of `EV-001`'s baseline; 20 threads is 1.75× slower than 8 |
| EV-009 | chunk sweep **128 → 1.55, 512 → 1.83, 1024 → 1.89, 4096 → 9.40, 16384 → 2.58 ms** | default | 4096 is a load-balance cliff (25 chunks over 20 threads); 128–1024 is the band |
| EV-010 | `VtArray` publish **0.0000–0.0003 ms** — at or below the 0.0001 ms timer resolution (printed `extract_vtarray_cow_ms 0.0003 / 0.0001 / 0.0000 ×5`, `prototypes/data-plane-benchmark/results_main.txt:95-101`). The CoW detach on the next full run is **disputed**; see Caveats | 9.6 MB buffer | **The publish cost is MEASURED; the detach delta is UNMEASURED and disputed — gate E-1 settles it.** Two runs disagree. Carried probe output: `run_full_with_outstanding_ref_ms 3.10` against `run_full_ms 1.72–1.91` (`results_main.txt:102` and `:60-66`) ⇒ a **≈1.2–1.4 ms** delta. Report run: 2.32 ms against 1.97 ms (`research/G-data-plane-engine-prototype-benchmark.md` §3.4, restated in its §4 block) ⇒ ≈0.35 ms. **1.97 ms appears in no carried output**, so the earlier claim that report §4 is the printed source is withdrawn: `prototypes/data-plane-benchmark/results_main.txt` is the printed source, and the report is a second, unreproduced run. `03-execution-engine.md` §0.3 and `appendix-B-prototype-inventory.md` §2.4 already record both. ADR §9 R20 replaces S24's `IsUnique()` wording: a publish ring (M7, optional) wraps each published `VtArray` in a `UsdGenGenerationBuffer : Vt_ArrayForeignDataSource` (`pxr/base/vt/array.h:39-51`) and reuses a buffer only after its detached callback has fired |
| EV-011 | VDF strip/request-all at 100 k **min 3.80 / median 8.32 / max 13.98 ms** (n = 7); at 1 M **min 69.9 / median 116.4 / max 134.8 ms** (n = 5); single-node re-run at 1 M **min 47.2 / median 49.4 / max 51.6 ms**; **72.0 ms** for a 0.5 % narrow schedule at 1 M; RSS 507 MB / 1 263 MB | default and 1 M | The probe printed min/median/max, so these rows name the statistic rather than a range (`prototypes/data-plane-benchmark/results_main.txt:18`, `results_scale.txt:6-7`); an earlier `3.80–8.32 ms` here read as min–max and was in fact min–median. Settled S21. Thread sweep on the strip layout **3.53 / 7.07 / 6.55 / 5.79 / 5.72 ms** at 1/2/4/8/20 — *slower at 20 threads than at 1* (report §3.3); `VdfSimpleExecutor` (serial) is a steady 5.01–5.16 ms |
| EV-012 | VDF pool invocations **6401** for 5 nodes over 800 000 elements; ignoring the affects mask costs **854 ms vs 7.60 ms** | default | Grain is hardcoded 500 (`pxr/exec/vdf/scheduler.cpp:861`) and does not align to curve boundaries |
| EV-013 | `VdfScheduler::Schedule` ≈ **1.4 µs/node**: 0.36–0.53 ms at 220 nodes, **2.93–4.39 ms at 2 200**, 13.7–23.0 ms at 11 000 — min–max over the two printed blocks of n = 7 (`prototypes/data-plane-benchmark/results_main.txt:27-29, 52-54`) | synthetic networks | A *different compiler* from usdGen's; it bounds nothing about `UsdGenCompiler` (gate **E-6**) |

### 2.2 Layout and autovectorisation

Host **H1**, single thread, `-O3`, 800 000 CVs per pass at 16 flops/CV. Probe `prototypes/data-plane-benchmark/simdBench.cpp`; report same, §6.

| # | Value | Layout | Caveats |
|---|---|---|---|
| EV-014 | **25.3 GFLOP/s** (0.506 ms/pass) | AoS `GfVec3f[]` | 1.18× vectorisation win only |
| EV-015 | **23.5 GFLOP/s** (0.545 ms/pass, 64.6 GB/s) | SoA, 8 planar arrays | bandwidth-bound |
| EV-016 | **80.9 GFLOP/s** (0.158 ms/pass, 121 GB/s) | SoA "strip", 3 planar arrays | 2.84× win. **121 GB/s is a partly cache-resident 9.6 MB set — never quote it as DRAM bandwidth** |
| EV-017 | `-fno-tree-vectorize`: **21.4 / 17.4 / 28.5 GFLOP/s** | as EV-014…EV-016 | Layout, not intrinsics, is the lever |
| EV-018 | `-mcpu=native`: **23.1 / 29.8 / 81.3 GFLOP/s** | as EV-014…EV-016 | Still 16-byte vectors; no SVE codegen |

### 2.3 Storm on the GPU

Host **H1-GPU**. Probe `prototypes/storm-hair-look/bench_hair.cpp`; report `research/G-storm-hair-look-prototype.md` §5. 1280×720, `color` AOV, one camera light, 8 warm-up frames, `glFinish()` inside the timed region, `usdGenHairPreview.glslfx` bound, `type=cubic basis=bspline wrap=pinned`, `widths` vertex, `minScreenSpaceWidths=1`. **All hair in one `basisCurves` prim** — the best case for batching. The last row, `EV-091`, is not a frame time: it is the on-disk size of the benchmark stages, measured on **H1** and reported in the same report's §6.

| # | Value (refine 0 / 1 / 2 / 3) | Scene | Caveats |
|---|---|---|---|
| EV-019 | **0.62 / 0.82 / 0.85 / 0.84 ms** | 4 000 curves, 32 000 CVs | complexity 1.0/1.1/1.2/1.3 → refineLevel 0/1/2/3 |
| EV-020 | **2.75 / 4.63 / 5.01 / 4.99 ms** | 40 000 curves, 320 000 CVs | the lower endpoint of the 100 k interpolation |
| EV-021 | **12.43 / 8.17 / 23.93 / 23.69 ms** | 200 000 curves, 1.6 M CVs | refineLevel 2 = **23.93 ms = 42 fps**. **refineLevel 0 is slower than 1** (cubic drawn as a per-CV LineList); reproduced twice |
| EV-022 | deform delta **+0.22 / +0.83 / +2.46 ms** = **0.57 / 0.22 / 0.13 ms per MB of points** | 4 k / 40 k / 200 k, per-frame `pointsAttr.Set()` + render, refineLevel 2 | Includes the `UsdAttribute::Set`; a scene-index publish should be at or below this. **The per-MB rate falls with size**, so applying the 200 k rate at 100 k is a lower bound |
| EV-023 | refineLevel 3 costs **nothing extra** over 2 at all three sizes | as EV-019…EV-021 | Only while on-screen strand width < ~10 px, where the halftube's width tessellation clamps to 1 |
| EV-091 | the benchmark stages as `.usda` on disk: **34 MB** at 40 k curves, **172 MB** at 200 k | `make_scene_40000.py` / `make_scene_200000.py`, `research/G-storm-hair-look-prototype.md` §6 | **A file size on host H1, not a GPU or process number.** It is why `10-build-dependencies-testing.md` §5.3 generates large stages and never checks them in; the two scenes were deleted after the benchmark |

### 2.4 Storm and Hydra CPU-side costs

Host **H1**. Probes `prototypes/storm-throughput/probe_*.cpp` (raw output `PROBE_OUTPUT.txt`) and, for EV-034, `prototypes/tool-loop/probePrimvarAppear2.cpp`. Reports `research/G-storm-throughput-and-prim-granularity.md` §1, `research/G-tool-loop-array-transport-and-cv-picking.md` §5. Rows `EV-084`–`EV-088` cover the remaining measured entries of that report's **§1.8** per-prim, per-frame CPU-overhead table, plus the two build variants only the raw output prints, added here because siblings cite them and every measured number the plan quotes needs a row (ADR §9 R42, R43): `01-architecture.md` §0.3, `03-execution-engine.md` §5.2, `06-imaging.md` §3.3 and `09-performance-and-benchmarks.md` §0.2 and §1 cite these numbers and now have `EV-` ids to cite.

| # | Value | Scene / size | Caveats |
|---|---|---|---|
| EV-024 | points interpolater resolve **0.22 ms**, `sharesInputBuffer=1` | 800 000 exact-size points | No CPU copy in the exact-size case |
| EV-025 | thrown-away `VtArray(n)` alloc **0.098 / 0.481 / 2.390 ms** | 50 k / 200 k / 800 k points, cold | Bandwidth; ~0.22 ms once the allocator recycles |
| EV-026 | padded points, **no `curveIndices`** → fallback `(1,0,0)` everywhere + `TF_WARN`, 0.54 ms; **with** `curveIndices` correct but **3.46 ms** | 1 000 000 authored vs 800 000 expected | **Why padding is banned.** The `resize()` detaches the shared array |
| EV-027 | cubic index build **2.6 ms warm / 5.2 ms cold** (500 000 patches, 7.63 MB); linear 2.30 ms (700 000 lines); `ComputeHash` 0.017 ms | 100 k curves × 8 CV | The switch cost gate S-9 must add to a refineLevel change |
| EV-028 | index build vs prim count **2.607 (1 prim) / 1.651 (8) / 0.659 (32) / 0.572 (128) / 0.748 (1000) ms** | same, 20 cores | Buffer sources resolve in parallel across prims, never within one |
| EV-029 | points resolve vs prim count **0.224 (1) / 0.121 (32) / 0.053 (128) ms** | same | Cheap either way |
| EV-030 | `ComputeDirtyLocators(1 leaf)` **0.105 µs/prim**; the primvar-descriptor recompute it forces **0.51 µs/prim** | per prim | 0.51 ms/frame at 1 000 prims — why the bare leaf locator is preferred where usdGen owns the prim |
| EV-031 | `HdDependencyForwardingSceneIndex` fan-out **0.17 / 0.38 / 0.90 / 1.30 µs** per affected prim at N = 32 / 1 k / 10 k / 100 k | one mesh-points dirty | 130 ms at 100 k dependents. Lazy population ≈10 µs/prim, one-off |
| EV-032 | scene-index bookkeeping per deform frame **~0.01 / ~0.4 / ~11 / ~210 ms** at 32 / 1 000 / 10 000 / 100 000 prims | sum of `EV-031` (fan-out), `EV-084` (notice delivery) and `EV-085` (entry build) | **100 000 curve prims is off the table on the scene-index side alone** |
| EV-033 | varying-primvar CPU expansion **6.48 ms** | 600 000 → 800 000 values | Why `widths` is constant or vertex, never varying |
| EV-034 | after a primvar appears upstream: dirty `primvars` **4.10 µs** (visible), `primvars/<name>` **2.06 µs** (visible), `PrimsAdded` **2.45 µs** (visible); universal `{}` **0.18 µs** and `…/primvarValue` **0.24 µs** are **not** visible | null render delegate | The paint-tool rule: value pushes per move, one `primvars/<name>` on appearance |
| EV-084 | `HdRetainedSceneIndex::DirtyPrims` → 1 observer, notice delivery **0.02 → 0.53 µs/prim** at N = 32 → 100 000 | N = 32 / 1 000 / 10 000 / 100 000 dirtied prims, no dependency index | The report's run (`research/G-storm-throughput-and-prim-granularity.md` §1.8). The carried probe prints **0.02 / 0.02 / 0.03 / 0.26 µs/prim** (`prototypes/storm-throughput/PROBE_OUTPUT.txt` probe K); the two runs differ by ≈2× at 100 k and agree that delivery is flat to 10 000 prims and superlinear past it. Siblings quote the report's 0.02 → 0.53 µs. `09-performance-and-benchmarks.md` §0.2 and §1 cite `EV-084` for delivery and **`EV-085`** for the entry-build half; an earlier draft of 09 called this row `EV-312` |
| EV-085 | `DirtiedPrimEntry` vector build reusing a **cached** locator set **0.008–0.009 µs/prim** at N = 32…10 000, **0.145 µs/prim** at N = 100 000 | prim paths pre-cached | `prototypes/storm-throughput/PROBE_OUTPUT.txt` probe I; report §1.8 quotes the 0.008 µs figure. This is the entry-build term of `EV-032` and of the frame ledger's publish row |
| EV-086 | the same build **recomputing** `ComputeDirtyLocators` per prim **0.111–0.116 µs/prim** at N ≤ 10 000, **0.249 µs/prim** at N = 100 000; the **full** form (`SdfPath` construction + `ComputeDirtyLocators`) **1.77 / 1.16 / 0.90 µs/prim** at N = 1 000 / 10 000 / 100 000, and **0.66 µs/prim** at N = 100 000 with the locator set cached | same probe | The 100 000-prim `SdfPath`-from-string reference is 47.5 ms of the 90.0 ms. `appendix-B-prototype-inventory.md` §8.4 records the same numbers. Why tile counts stay in the 32–256 band |
| EV-087 | `HdDataSourceLocatorSet::Intersects` **0.0119 µs/call** | 1 000 000 calls | The dependency-forwarding inner loop. Report §1.8 rounds it to 0.013 µs |
| EV-088 | `GetPrim` through K pass-through filtering scene indices **0.186 / 0.182 / 0.183 / 0.188 / 0.193 µs/call** at K = 0 / 1 / 3 / 6 / 10 — **flat in K** | one prim per call | `PROBE_OUTPUT.txt` probe L; report §1.8 gives 0.17–0.20 µs. The source-side counterpart of `EV-036`: filtering depth is free, which is what lets usdGen be several small composable indices |

### 2.5 RigExec baseline, notice cascade and commit models

Host **H1**. Probes `prototypes/chain-order/src/probeChainOrder.cpp` (EV-035–EV-036) and `prototypes/evaluation-scheduling/{noticeCost,models}.cpp` with outputs `noticeCost.txt`, `models.txt` (EV-037–EV-039). Reports `research/G-chain-order-probe.md` §5, `research/G-evaluation-scheduling-and-batching.md` §5, §9.

| # | Value | Scene / size | Caveats |
|---|---|---|---|
| EV-035 | `RigExecImaging_SetTime` **1.351–1.502 ms/frame** (65–72 ms over 48 frames), worst non-first frame 1.710 ms | `examples/ArmShotAnim.usda`, frames 1001→1048 | One rig class, not a universal figure. The report's §5 table gives worst frames 1.710 / 1.695 / 8.514 ms (the last a first-touch outlier); its Key-facts line `research/G-chain-order-probe.md:344-345` says 1.99 ms and is unreconciled with its own table — **the §5 table is the printed source**. The carried `prototypes/chain-order/logs/transcript.txt` records notice counts, not per-frame times, so no probe output disputes the table here — unlike `EV-010`, where the carried output does and the report loses |
| EV-036 | + full terminal traversal (95 prims) **1.610 ms/frame**; + two extra pass-through filtering scene indices **1.571 ms/frame** | same | **Per-hop scene-index cost is below measurement noise** — favour many small composable indices |
| EV-037 | notice cascade **0.005 / 0.007 / 0.061 / 0.21 / 1.13 ms per frame** at 1 / 10 / 100 / 1 000 / 5 000 scalps; **exactly 2 `PrimsDirtied` calls per frame** regardless of count or batching | time-varying points | ≈0.2 µs per entry. The plumbing is never the problem; the cook is. **Two runs of the same probe exist and siblings quote the one in the Value column** — `research/G-evaluation-scheduling-and-batching.md` §9, the report's median-of-3 run, which is what `09-performance-and-benchmarks.md` §0.2 and `appendix-B-prototype-inventory.md` §3.4 already cite. The carried output `prototypes/evaluation-scheduling/noticeCost.txt` prints **0.0030 / 0.0108 / 0.0279 / 0.1977 / 1.2376 ms** with batching off and **0.0017 / 0.0096 / 0.0301 / 0.2119 / 1.0777 ms** with it on — up to 2.2× apart at 100 scalps, agreeing at 1 000 and 5 000. The ≈0.2 µs/entry conclusion and the exactly-2-`PrimsDirtied` fact hold in **both** runs, and those are the only two things the design uses |
| EV-038 | cook counts for 10 interactive events: **eager-in-notice 29** (20 batched); **lazy-in-`GetPrim` 10, but on 4 worker threads and 0 downstream notices**; **deferred-on-frame 10**; **deferred-on-commit 10** | 3 meshes + 1 curve prim | The measurement behind S17/S18 and ADR §4.3 |
| EV-039 | **16 734 reads, 0 torn reads** | 8 reader threads × 20 publishes | Validates the `atomic_load` publish pattern under Storm's parallel Rprim sync. Gate **SI-4** re-runs it at 8 × 100 |

### 2.6 Freeze, bake and undo

Probes `prototypes/freeze-bake/probe*.{py,cpp}` and `prototypes/freeze-bake/uv/testUsdviewFreezeCost.py`; report `research/G-freeze-bake-undo-and-frozen-reentry.md` §1, §3, §4. Host **H1** except EV-050, EV-051, EV-053, EV-054 (**H1-SW**).

| # | Value | Scene / size | Caveats |
|---|---|---|---|
| EV-040 | `SubtreeSnapshot.Capture` **0.10 ms**, undo **0.23–0.24 ms**, redo **0.08–0.09 ms**, **zero RSS growth** | 10 k and 100 k curves × 8 CV | `SdfCopySpec` shares the `VtArray` buffers; a 200-deep undo stack costs no extra memory |
| EV-041 | author a freeze into the session layer, **C++** (`probe4_frozen_reentry_si.cpp`, report §4.1) **0.25 / 0.27 / 0.27 ms** | 10 k / 100 k / 1 M curves | Flat in curve count |
| EV-042 | the same edit through **Python** (`probe1_freeze_undo.py`, report §1.3) **0.51 / 0.52 ms** | 10 k / 100 k curves | The extra ~0.25 ms is Python attribute-set overhead, not USD. **Three separate freeze-author probes exist — `EV-041`, this row and `EV-089` (0.4 ms, bake-landing) — and none is a range over the others; never quote them as one 0.25–0.52 ms sweep** |
| EV-043 | sublayer insert **0.21**, remove **0.15**, mute **0.13**, unmute **0.13 ms** | 100 k curves | Flat in curve count; the alternative undo shape |
| EV-044 | `RemovePrim` + apply **0.06 / 1.47 / 13.47 ms**; `SetActive(false)` + apply **0.02 ms flat** | 10 k / 100 k / 1 M | The only scaling term is the `free()` — why undo of a live freeze is `SetActive(false)` |
| EV-045 | USD→Hydra freeze total (author + `ApplyPendingUpdates` + `GetPrim` + full pull) **0.55 / 0.59 / 0.59 ms** | 10 k / 100 k / 1 M | Flat in curve count |
| EV-046 | `.usdc` static **9 601 971 B / 10.6 ms**; + `velocities` **19 202 035 B / 13.4 ms**; 24 point time samples **230 402 598 B / 80.6 ms**; `.usda` static **40 317 359 B / 368 ms** | 100 k curves × 8 CV | Python array construction moved outside every timer (§6 K15) |
| EV-047 | reopen + first `points.Get()`: `.usdc` **0.4–0.8 ms**, `.usda` **532 ms** | same | 660×. **Never bake `.usda`** |
| EV-048 | `primvars:rest` sharing the same `VtArray` object as `points` costs **+26 bytes**; a distinct buffer **+9 600 046 bytes** | same | Crate deduplicates identical buffers — authoring rest at freeze time is free |
| EV-049 | composition (compose / first `points.Get()`): sublayer **0.2 / 0.1 ms**, reference **0.3 / 1.1**, payload **21.5 / 1.5** | 100 k curves | Payload's 21.5 ms is the price of being unloadable |
| EV-050 | usdview freeze **author incl. synchronous notice 1.34 / 3.45 / 12.09 ms** at 115 / 2 205 / 11 005 stage prims | 10 k and 100 k curves | The number gate **T-4** thresholds (≤ 5 ms author; ADR §9 R40) |
| EV-051 | `_resetGUI()` **4.28 / 35.4 / 105.5 ms**, of which `_resetPrimView` **2.64 / 23.0 / 102.2 ms** and `_resetPrimViewVis` **0.12 / 0.94 / 4.31 ms** | same | **O(stage prims), not O(curves)**, ≈9–10 µs/prim; **excluded from T-4** and coalesced to one per batched freeze (S43) |
| EV-052 | re-authoring the parent scope's `typeName` → **203 `PrimsAdded`, 1.3–1.5 ms** of SI work | 200 sibling curve prims | Host **H1** (`probe10_resync_blast.cpp`, no GL). The rule "tooling never re-authors the parent scope" |
| EV-053 | llvmpipe redraw **104 / 136 / 186 ms** | 115 / 2 205 / 11 005 prims | **CPU rasterisation. Never a Storm number** |
| EV-054 | live usdview loop: `SetActive(False)` **45.1 ms**, `SetActive(True)` **161.0 ms**, `RemovePrim` **90.7 ms** | 2 205 prims, 100 k curves | Dominated by EV-053; the residue is EV-044 + EV-050/EV-051 |
| EV-089 | freeze into the **session layer, author only, no file**: **0.4 ms**, **0 bytes** | 100 k curves × 8 CV | Report §3's bake-landing table (`research/G-freeze-bake-undo-and-frozen-reentry.md` §3), the row `05-static-curves-and-deformation.md` §5.4, `08-tools.md` §4.1 and §4.3, and `appendix-B-prototype-inventory.md` §4.4 quote. **Three separate freeze-author probes exist and none is a range over the others:** this one (bake-landing, 0.4 ms), `EV-041` (the scene-index probe's author step, C++, 0.25/0.27/0.27 ms) and `EV-042` (the same edit through Python, 0.51/0.52 ms). Quote the one whose probe you mean |
| EV-090 | first full scene-index pull of a whole C3 prim (topology + every primvar) **0.19 / 0.19 / 0.20 ms**; **second pull 0.01 ms** | 10 k / 100 k / 1 M curves | `research/G-freeze-bake-undo-and-frozen-reentry.md` §4.1, same probe as `EV-041` and `EV-045`; **flat in curve count** because a data source hands back the retained `VtArray`. It is the pull term of `EV-045`'s 0.55/0.59/0.59 ms total, cited as such by `05-static-curves-and-deformation.md` §2.7, §5.9 and §8.1 |

### 2.7 Tool loop: transport, brush, picking

Host **H1**. Probes `prototypes/tool-loop/{bench_transport,bench_stroke,bench_pick_cpu,bench_pick_cpp}.py` over `cTransport.cpp` / `bpTransport.cpp` / `pbTransport.cpp`; report `research/G-tool-loop-array-transport-and-cv-picking.md` §1–§2. Sizes: 100 000 CVs = 1.20 MB, 800 001 floats = 3.20 MB, 1 000 000 CVs = 12.0 MB. Best-of-N.

| # | Value | Size | Caveats |
|---|---|---|---|
| EV-055 | `VtArray` across a **pxr_boost.python** boundary **0.13–0.18 µs**, push and pull | 100 k / 267 k / 1 M CVs | O(1): a copy-on-write handle copy. The array surface the plan ships |
| EV-056 | every `float*` route (ctypes / pybind11-numpy / pxr_boost+memcpy) **13.5–16 / 42–47 / 340–360 µs** | 1.2 / 3.2 / 12 MB | memcpy-bound and mutually equal; the choice is ergonomics, not speed |
| EV-057 | `Vt.Vec3fArray.FromBuffer` **420 / 1107 / 4304 µs**; `attr.Set(numpy)` **480 / 1249 / 4893 µs** | same | Per-element strided walk. **Banned on groom-sized arrays** |
| EV-058 | `attr.Set(Vt.Vec3fArray)` **1.63–1.82 µs at any size**; in `Sdf.ChangeBlock` 2.21–2.27 µs; `attr.Get()` 0.75 µs | 100 k–1 M CVs | 2 700× cheaper than EV-057 |
| EV-059 | `np.asarray(vt)` **0.19 µs**, zero-copy, read-only, `(N,3) float32`; the view survives dropping the Python `Vt` object | all | numpy is the kernel language, never the transport |
| EV-060 | Python list routes: out **9.3 ms (100 k) – 109 ms (1 M)**; in **18–177 ms** | — | Disqualifies usdRig's `_rigexec` list-of-lists route for groom arrays |
| EV-061 | one brush move **21.3 µs total** = 0.34 (zero-copy read) + 16.6 (numpy kernel) + 4.1 (sparse push); the same kernel in pure Python **309 µs** | 100 k CVs, 2 000-CV footprint | 0.13 % of a 16.7 ms frame — the per-move cost is not in the transport |
| EV-062 | sparse indexed push **3.73 µs** (ctypes) / **1.17 µs** (pxr_boost) | 2 000 CVs of 100 k | vs 13.5 µs for a full push |
| EV-063 | release write **2.4 µs**; the bad alternative (numpy → `FromBuffer` → `Set`) **483 µs** | 100 k CVs | — |
| EV-064 | CV pick in C++ over ctypes **166 µs at 100 k CVs, 1.66 ms at 1 M**; footprint queries **590 µs / 1.07 ms** | 1920×1080, 24 px radius | Single thread, no occlusion test. ≈8× cheaper than one `view.pick()` (usdRig baseline 1.3–1.42 ms) |
| EV-065 | numpy equivalents **1.77 ms / 17.2 ms** with BLAS pinned to 1 thread; **10.5–29 ms / 134–230 ms** unpinned on a loaded host | same | A `(100k,4)@(4,4)` matmul measured **22 ms at load 50** vs **0.56 ms** pinned. **The plugin must pin BLAS threads at import** |
| EV-066 | freeze authoring **60–86 µs**; `layer.Export()` to a 1.20 MB `.usdc` **1.22 ms** | 100 k CVs | `Usd*.Define()` inside `Sdf.ChangeBlock` **fails** (`UsdStage::_DefinePrim`) — define outside, author inside |

### 2.8 Third-party libraries

Host **H1**, `-O2`. Probes `prototypes/thirdparty-bench/{seexpr_bench,ptex_test}.cpp`; report `research/A8-seexpr-ptex-libs.md` §1.6, §2.8.

| # | Value | Operation | Caveats |
|---|---|---|---|
| EV-067 | **13 ns/eval** (`$u*$v+1`), **117 ns** (`fit(noise($P*4)+0.5*fbm($P*2,4),…)`), **34 ns** (custom `map()` + `hash`), **106 ns** (cellnoise+voronoi+smoothstep) | SeExpr2 interpreter | Interpreter only; the LLVM backend is not buildable here |
| EV-068 | expression prep **7–53 µs** | same | Cheap enough to clone one `Expression` per worker |
| EV-069 | VarBlock (`threadSafe`) **128 ns** at 1 thread, **159 ns/eval/thread** on 8 → **≈50 M evals/s** | noise+fbm | ~10 % overhead; the copy scales with expression size |
| EV-070 | Ptex `f_bilinear` **23 ns/lookup**, `f_box` **26 ns**; 8 threads with per-thread filters **35 ns/lookup/thread → 228 M/s** | Ptex 2.4.3 | **One `PtexFilter` per thread** — `PtexSeparableFilter` carries per-eval scratch state |

### 2.9 Memory-bound floors for motion

Host **H1**, single thread, `g++ -O2`, 1.6 M `float3` points (100 k curves × 16 CV). Probe **`prototypes/motion-blur/mbbench.cpp`** (carried in; build with `g++ -O2 -std=c++17 mbbench.cpp -o mbbench`, no USD dependency). ADR §9 R43 attaches a conditional to these floors — re-create the prototype under a `PW-` item in M7 *unless* `prototypes/motion-blur/` exists. **It exists** (verified 2026-09-05), so nothing is owed: `appendix-B-prototype-inventory.md` §0.1, §0.2 and §12 carry the same directory, and its **`PW-11`** covers only the retained per-offset cache and the `k·tail` re-evaluation that `mbbench.cpp` does not model. Report `research/G-motion-blur-sampling-strategy.md` §5 and the report's unnumbered **Key facts** section. Caveat for the whole subsection: these are single-thread `-O2` memory-bandwidth floors, not chain costs; the per-offset tail cost `T` is UNMEASURED (gate **E-1**).

| # | Value | Operation | Caveats |
|---|---|---|---|
| EV-071 | **0.97 ms** | `P + (t/fps)·V` velocity extrapolation | one memory-bound pass |
| EV-072 | **0.99 ms** | `lerp(P0, P1, a)` between two retained offsets | — |
| EV-073 | **1.00 ms** | `(P1 − P0)·fps` finite-difference velocity | — |
| EV-074 | **0.49 ms** | copy one sample (`VtArray` detach) | the input to §2.11's interleave floor |
| EV-075 | **19.2 MB** | bytes per point sample at this size | k = 3 offsets ≈ 58 MB per groom, k = 9 ≈ 173 MB — both **ASSUMPTION**, arithmetic over this row |

### 2.10 Build and test

Host **H1**; report `research/B-usdrig-build.md` §2–§5 and its **Decisions this settles** (for `EV-092`).

| # | Value | What | Caveats |
|---|---|---|---|
| EV-076 | configure **0.83 s**; clean build **21.45 s** (64 ninja edges, `-j16`, maxRSS 2.10 GB); `ctest -j8` of 27 suites **1.24 s**; install < 1 s (6.1 MB prefix) | usdRig out of tree against the stock install | Zero `error:`; the only warnings are 36 `[-Wcpp]` from OpenUSD's own headers |
| EV-077 | incremental **10.05 s** for all of `rigExecImaging` + relink; **21.4 s** for one TU in `rigExec` | the dev loop | The 21.4 s is dominated by pybind11's `-flto=auto` relink — why `_usdGen` is built without LTO |
| EV-078 | `testRigExecCurvenet`: **3 of 36 traces fail** with `-ffp-contract=fast`; **0 fail** with `off`, identical residual `0.07926` | same source, same compiler | A discrete branch flip at a tolerance boundary, not drift. Direct evidence for the `usdGenMath` flag |
| EV-079 | `RemovePrim` posts **0 errors** with no exec system and **1 error** with an `ExecUsdSystem` attached | `prototypes/usdrig-linux-build/probeExecResyncRemovePrim.cpp` | Reproduced with **zero usdRig code**. See §3.10 |
| EV-080 | **26 / 27** CTest suites pass | usdRig on Linux/aarch64 | The one failure is EV-079's stock defect |
| EV-092 | `probeImagingPipeline`: **36 assertions in 0.07 s**, exit 0, **no GL at all** | the full `UsdImagingCreateSceneIndices` + `RigExecUsdImagingSceneIndexPlugin` chain over `usdRig/examples` | `research/B-usdrig-build.md` §3 and its "Decisions this settles" 4. The measured shape of the **tier-1 harness**: a plain executable, sub-100 ms, no display. `10-build-dependencies-testing.md` §0.4 and §5.2 cite it |

### 2.11 Numbers used in the plan that are **not** measurements

These rows record the *provenance* of `09-performance-and-benchmarks.md` §0.2's frame ledger and of the other non-measurements the plan leans on; **where a value here and there differ, 09 §0.2 wins** (ADR §9 R41). Listed so nobody promotes one by accident.

| Quantity | Tag | How obtained | Gate that would make it MEASURED |
|---|---|---|---|
| Storm draw ≈ **12.2 ms** at 100 k × 8 CV, refineLevel 2 | DERIVED (interpolated) | between `EV-020` (5.01 ms @ 40 k) and `EV-021` (23.93 ms @ 200 k), both at 1280×720; the ledger that consumes it (`09-performance-and-benchmarks.md` §0.2) is also stated at 1280×720, so interpolation is the only error term | **S-1** at 100 k, M1 — 09 §5.3 already runs it at **720p and 1080p** (ADR §9 R40); the 1080p figure is a separate baseline and is never compared with the 720p one |
| usdGen deformed tail **0.4–0.6 ms/frame** | DERIVED | one styler pass 0.158–0.545 ms (`EV-014`–`EV-016`) against the 8-thread 5-node chain `EV-001` (1.02 ms) | **E-1**, **E-3** |
| SoA→AoS interleave **0.25 ms** per publish of 800 k CVs | DERIVED **floor** | scaled from `EV-074` (0.49 ms per 19.2 MB pass); a strided three-stream gather is not a memcpy, so the real figure is higher | **S-2** (E-1 re-derives the pass) |
| usdGen dirty routing + commit bookkeeping **0.04 ms** | DERIVED | 0.2 µs/entry (`EV-037`) × ~200 entries; the entry count is itself unmeasured. The arithmetic gives 0.04 ms and `09-performance-and-benchmarks.md` §0.2 prints 0.04 ms; an earlier ≈0.05 ms here was wrong (ADR §9 R41) | **SI-3** |
| Per-tile `extent` **0.05 ms** (ASSUMPTION: min/max fused into the interleave loop) + generation diff and publish **0.02 ms** (DERIVED from `EV-085`'s 0.008 µs/prim entry build and `EV-084`'s 0.02–0.53 µs/prim notice delivery, the two terms 09 §0.2 names) ≈ **0.07 ms at 49 tiles** | ASSUMPTION + DERIVED | `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` = **49** at 100 k curves / chunk 512 / `tileTarget` 64 (ADR §9 R21); 64 tiles is the 1 M-curve figure | **SI-2**, **S-2** |
| **usdGen total, deform frame 0.8–1.0 ms** | DERIVED aggregate | sum of the deformed-tail, interleave, dirty-routing and `extent`+publish rows above (0.4–0.6 + 0.25 + 0.04 + 0.07), not the Storm row; `09-performance-and-benchmarks.md` §0.2 is the only frame ledger and publishes it (ADR §9 R41); `01-architecture.md` §0.3, `03-execution-engine.md` §0.2 and `05-static-curves-and-deformation.md` §8 cite that ledger. All three printed 0.8–1.2 ms in the first writing pass; re-checked 2026-09-05, all three now print **0.8–1.0 ms** and no correction is outstanding | **E-1** and **S-2** together |
| Storm points upload ≈ **1.44 ms** for 9.6 MB | DERIVED (interpolated) | between `EV-022`'s +0.83 ms @ 40 k and +2.46 ms @ 200 k; the per-MB rate falls with size, so this is a lower bound | **S-2** |
| Frame ledger total **15.8–16.0 ms** | DERIVED | 1.35 (`EV-035`) + 0.8–1.0 + 12.2 + 1.44, conditional on S-8 variant A | E-1 + S-1 + S-2 together |
| `hairTangent` variant B republish ≈ **+1.5–2.5 ms** at 100 k × 8 CV | DERIVED | the `DirtyPrimvar` path re-uploads every non-points primvar (§3.6), scaled from `EV-022` | **S-8** |
| kNN capture **≈10 ms** at 100 k rest roots on 8 threads | ASSUMPTION | extrapolated from nanoflann's published benchmarks; not run here. This row and `03-execution-engine.md` §4.4 are the only places the figure lives; `09-performance-and-benchmarks.md` §0.2 does not carry it | **E-4**, M0 pre-work, binding at M3 (`09-…` §5.1) |
| Capture cost per enable/disable gesture **10–150 ms** | ASSUMPTION | `design/judge-artist.md` §2.1 reading `design/proposal-performance.md` §5.6/§7.4; no probe | **E-4** measures the kNN term; the rest has no gate. Falsified by a capture profile on a real groom |
| Sub-graph recompile of a 200-node groom **≈ 40 µs** | ASSUMPTION | `design/proposal-performance.md` §5.3 (O(200 × ~200 ns), extrapolated from usdRig's digest cost); VDF's measured 1.4 µs/node (`EV-013`) is a different compiler | **E-6** |
| SI-6's bounded first traversal **≤ 5 ms** on the 11 005-prim stage | ASSUMPTION (ADR §9 R28) | no probe; the comparable figure is `EV-051`'s ≈9–10 µs/prim usdview walk | **SI-6** |
| Multi-prim Storm cost at 1 / 32 / 128 / 196 tiles | UNMEASURED | every §2.3 row used **one** prim; the CPU half is MEASURED (`EV-028`, `EV-031`, `EV-084`–`EV-088`) | **S-12** (prim-count sweep at 100 k × 8 CV, frame time and `drawBatches`; T2, **M1**, before C2 freezes — `09-performance-and-benchmarks.md` §5.3), with **S-5** asserting the batch count at the fixed default and **S-3** the one-tile-dirty case. `S-11` is the `head1M` draw record at M7 and is **not** this gate |
| Guides as a fraction of hairs **1–10 %**, budgeted at 10 % | ASSUMPTION (ADR §9 R26) | a design input; `research/A7-prior-art-grooming.md` §9.1–§9.2 describes guide-driven interpolation but names no ratio | none — falsified by a production groom's guide count |
| Interactive targets **60 Hz at ≤ 40 k curves, ≥ 30 Hz at 100 k, ≥ 10 Hz at 200 k** | ASSUMPTION (ADR §9 R41) | the interpolated draw curve, not a target anyone measured | **S-1** |
| usdGen has **≈15 ms/frame** at 60 Hz after usdRig | ASSUMPTION (arithmetic) | 16.6 − `EV-035` (1.35) − `EV-036` delta (0.24) | per rig class; never universal, no gate |
| Staffing = **3 engineers**; M0–M2 ≈ 11–12 wk, M0–M5 ≈ 25, M0–M7 ≈ 34 | ASSUMPTION | ADR §7 and §9 R39, 30 % contingency on M1/M3/M7 | none — a plan input |

### 2.12 Adapter coverage and stage-free transport

Host **H1**, no GL. Probes `prototypes/stage-free-transport/probe.cpp` and `probe2.cpp`, with the codeless schema plugin at `prototypes/stage-free-transport/plugin/usdGenProbeSchema/resources/`. Report `research/G-stage-free-parameter-and-time-transport.md` §1–§5. These rows are the measured basis of S10, of ADR §2.1's adapter-coverage correction ("without this the container half of the schema is invisible to Hydra"), and of gate **SI-7**.

| # | Value | Probe | Caveats |
|---|---|---|---|
| EV-081 | A codeless prim with **no imaging adapter** reports hydra `primType` = `''`, and its prim data source carries only `__usdPrimInfo primOrigin primvars usdMaterialBindings geomModel model __usdUpAxis skelBinding coordSysBinding purpose visibility xform materialBindings` — **every schema-declared `usdGen:*` attribute absent** | probe1 §1 | Registration itself works with zero C++; `UsdPrimDefinition::GetPropertyNames()` sees the properties, Hydra does not |
| EV-082 | Authoring `usdGen:clumpRadius` or `usdGen:seed` on that prim produces **no notice at all**; `primvars:usdGen:rampKnots` is present and dirties precisely (`*DIRTY /World/Clump {'primvars/usdGen:rampKnots'}`) | probe2 A/B/C | Why `primvars:usdGen:*` is only the prototyping fallback and the adapter is the shipping path (M1's stop condition) |
| EV-083 | `UsdImagingDataSourcePrimvars::GetNames()` enumerates **attributes only**, while `Get(name)` falls through to `GetRelationship` — so `rel primvars:usdGen:surface` is `Get()`-able but not listed | probe2 K | A generic `GetNames()` walker silently loses every relationship argument |

---

## 3. Verified OpenUSD 26.08 facts (file:line)

Every row was re-verified by grep in `<openusd-src>` while writing this appendix. Anything with a verification limit is in §3.11.

### 3.1 Scene-index plugin registry and ordering

| Fact | file:line |
|---|---|
| `HdSceneIndexPluginRegistryTokens` = `rendererDisplayName` (`"__rendererDisplayName"`), `allRenderers` (`""`) | `imaging/hd/sceneIndexPluginRegistry.h:27-31` |
| `InsertionOrderAtStart`/`AtEnd`, `using InsertionPhase = int`, `RegisterSceneIndexForRenderer` (two overloads) | `imaging/hd/sceneIndexPluginRegistry.h:91-92, 95, 142, 184` |
| Default ordering policy **Hybrid**; JSON `tags` / `ordering.after` / `before` / `position` compose with the C++ phase and order | `sceneIndexPluginRegistry.cpp:40-48`; `imaging/hd/sceneIndexPlugin.h:30-67` |
| `loadWithRenderer` is **mandatory**; a C++ registration with no matching JSON entry is dropped under Hybrid | `imaging/hd/sceneIndexPlugin.h:37-39`; `sceneIndexPluginRegistry.cpp:862-869` |
| Library preload keys are `allRenderers` then the display name | `sceneIndexPluginRegistry.cpp:1360-1372` |
| `_AppendSceneIndex` (two overloads) and `_IsEnabled` are the plugin virtuals | `imaging/hd/sceneIndexPlugin.h:107, 122, 133` |
| The engine registers its app indices (`HdsiSceneGlobalsSceneIndex` → dome-light camera visibility → scene material pruning) at **phase 0, `InsertionOrderAtStart`, all renderers**, via a callback — not a tagged plugin | `usdImaging/usdImagingGL/engine.cpp:148-201` |
| Renderer plugins are appended by the engine at `engine.cpp:1778` and by the legacy render index at `renderIndex.cpp:211` | as cited |
| `UsdImagingSceneIndexPlugin`s are appended at exactly **one** point: `_AddPluginSceneIndices`, defined at `:68`, called once at `:302` | `usdImaging/usdImaging/sceneIndices.cpp:68, 302` |
| Their order comes from a `std::set<TfType>` filled by `PlugRegistry::GetAllDerivedTypes` and iterated in set order; `TfType::operator<` compares the internal `_TypeInfo*` **heap address**, so the order is not stable across runs | `usdImaging/usdImaging/sceneIndexPlugin.cpp:50-54`; `base/tf/type.h:119, 728` |
| `HdMergingSceneIndex::InsertInputScenes` replays `_SendPrimsAdded` | `imaging/hd/mergingSceneIndex.cpp:173, 244`; header `:57, 70` |
| Storm's display name is `"GL"`; `USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX` defaults **true** | `imaging/plugin/hdStorm/plugInfo.json:10`; `engine.cpp:78` |
| hdGp's resolver is gated off by default | `imaging/hdGp/sceneIndexPlugin.cpp:25, 64` |

### 3.2 sceneGlobals and the velocity fps chain

`HdSceneGlobalsSchemaTokens` are exactly `sceneGlobals, primaryCameraPrim, activeRenderPassPrim, activeRenderSettingsPrim, startTimeCode, endTimeCode, timeCodesPerSecond, currentFrame, sceneStateId` (`imaging/hd/sceneGlobalsSchema.h:37-47`). **There is no "interactive" flag** — the verified basis for ADR §2.3's explicit render-context decision.

`HdsiSceneGlobalsSceneIndex::SetCurrentFrame(double)` is at `imaging/hdsi/sceneGlobalsSceneIndex.h:60`, `SetTimeCodesPerSecond(double)` at `:65`. Nothing in usdImagingGL/usdviewq/usdAppUtils calls `SetTimeCodesPerSecond` (grep) — **but the velocity scene index does not therefore fall back to 24 fps.** `UsdImagingDataSourceStage` publishes the stage's `timeCodesPerSecond` into the `sceneGlobals` container (`usdImaging/usdImaging/dataSourceStage.cpp:57-72`, the call at `:69-71`), served at `SdfPath::AbsoluteRootPath()` (`usdImaging/usdImaging/stageSceneIndex.cpp:247-249`) = `HdSceneGlobalsSchema::GetDefaultPrimPath()` (`imaging/hd/sceneGlobalsSchema.h:104-106`). The scene-globals index **overlays** rather than replaces (`imaging/hdsi/sceneGlobalsSceneIndex.cpp:231-243`), and the overlay falls through when the stronger container returns null (`imaging/hd/overlayContainerDataSource.cpp:74-99`; `sceneGlobalsSceneIndex.cpp:97-99`). So the stage's value reaches `HdsiVelocityMotionResolvingSceneIndex::_GetTimeCodesPerSecond` (`imaging/hdsi/velocityMotionResolvingSceneIndex.cpp:273-287`); the hard-coded 24 (`_fallbackTimeCodesPerSecond`, `:58`) is reached only on a stage with **no** authored `timeCodesPerSecond`. Motion profile P1 depends on this; see §6 K22.

### 3.3 Ext-computation pruning and UsdSkel

| Fact | file:line |
|---|---|
| `HdSiExtComputationPrimvarPruningSceneIndex` presents computed primvars as authored ones, executing the CPU kernel on pull; public `New(input)`; pass-through when nothing is computed | `imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:17-45` |
| UsdSkel replaces a skinned prim's `primvars` with a retained container holding **blocked** `points` and `normals` | `usdImaging/usdSkelImaging/dataSourceResolvedPointsBasedPrim.cpp:298-299` |
| The resolved prim caches `HdPrimvarsSchema const _primvars` and rebuilds only when the dirty set `Contains` the **bare `primvars` locator** | `…/dataSourceResolvedPointsBasedPrim.h:245`; `.cpp:1178-1180` |
| `HD_ENABLE_DEFERRED_SKINNING` defaults **false** | `imaging/hd/skinningSettings.cpp:15, 34` |

Rows 2 and 3 together are S5: a scene index that overrides `primvars` and may sit upstream of UsdSkel must dirty the bare `primvars` locator or the skin silently freezes. A behaviour, not a number, so it has no §2 row; proved in `research/G-chain-order-probe.md` §4c with the ground truth −0.2000 / −0.2269 / −0.2562.

### 3.4 Dependency declaration and forwarding

`HdDependencySchemaTokens` = `dependedOnPrimPath, dependedOnDataSourceLocator, affectedDataSourceLocator` (`imaging/hd/dependencySchema.h:35-38`). `HdDependencyForwardingSceneIndex`'s header carries an explicit large-fan-out performance caveat (`imaging/hd/dependencyForwardingSceneIndex.h:72-80`) — the source-side counterpart of `EV-031` and the reason `__dependencies` is declared per tile, not per curve.

### 3.5 basisCurves, instancer and primOrigin schemas

| Fact | file:line |
|---|---|
| `HdBasisCurvesSchemaTokens` = `basisCurves`, `topology` | `imaging/hd/basisCurvesSchema.h:36-38` |
| `HdBasisCurvesTopologySchemaTokens` = `topology`, `curveVertexCounts`, `curveIndices`, `basis`, `type`, `wrap` — the exact container names contract C2 publishes | `imaging/hd/basisCurvesTopologySchema.h:35-41` |
| Values for those tokens live in `HdTokens`: `bspline` (`:26`), `catmullRom` (`:29`), `cubic` (`:34`), `linear` (`:63`), `nonperiodic` (`:72`), `periodic` (`:76`), `pinned` (`:77`). **`bSpline` (`:108-113`) is a legacy alias whose string value is also `"bspline"`** — author the string `"bspline"`, never the symbol spelling `bSpline` | `imaging/hd/tokens.h:26, 29, 34, 63, 72, 76, 77, 108-113` |
| `HdLegacyDisplayStyleSchemaTokens` = `displayStyle`, `refineLevel` (then `flatShadingEnabled`, `displacementEnabled`, …) — how ADR §5.3's `displayStyle/refineLevel = 2` is published | `imaging/hd/legacyDisplayStyleSchema.h:35-37` |
| `HdInstancerTopologySchema::GetInstanceIndices()` returns `HdIntArrayVectorSchema` (one `VtIntArray` per prototype) — **not** a vector of `HdInstanceIndicesSchema` | `imaging/hd/instancerTopologySchema.h:125-126` |
| Prototypes may be **subtree roots**; `instanceLocations` is meaningless for explicit instancing and must be null; an empty `mask` means all-true | `imaging/hd/instancerTopologySchema.h:60-92, 128-132` |
| `HdInstancedBySchemaTokens` = `instancedBy, paths, prototypeRoots` | `imaging/hd/instancedBySchema.h:35-38` |
| Instance-rate primvar names `hydra:instanceTransforms / Rotations / Scales / Translations` | `imaging/hd/tokens.h:115-127` |
| `HdPrimOriginSchemaTokens` = `primOrigin, scenePath` | `imaging/hd/primOriginSchema.h:35-40` |
| An **absolute** `scenePath` replaces the accumulated path; a **relative** one is appended — the rule for prims inside prototypes | `imaging/hdx/pickTask.cpp:1274-1295` |
| `HdSelectionSchemaTokens` are only `fullySelected, nestedInstanceIndices` — no point/element selection channel a scene index can express | `imaging/hd/selectionSchema.h:36-41` |

### 3.6 Storm: material resolution, points fastpath, refineLevel, uploads

| Fact | file:line |
|---|---|
| Storm's material render contexts are `{glslfx, mtlx}` in that order (mtlx only with MaterialX support); the assignment is `info.materialRenderContexts = { … }` inside `_RenderDelegateInfo()` | `imaging/hdSt/renderDelegate.cpp:695-707` — the plan's citation (ADR §9 R43). The containing function spans `:695-714`; the assignment itself is `:702-707` |
| `HdsiMaterialRenderContextFilteringSceneIndex` selects "the first render context encountered in `renderContextPriorityOrder`" | `imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45` |
| The **points fastpath** is taken only when `DirtyPoints` is set and `DirtyNormals`, `DirtyWidths`, `DirtyPrimvar` are all clear | `imaging/hdSt/basisCurves.cpp:930-935` |
| `DirtyPrimvar` is one bit for **every** primvar except `points`/`velocities`/`accelerations`/`nonlinearSampleCount`/`normals`/`widths` | `imaging/hd/changeTracker.cpp:958-979` |
| refineLevel → shader key: `>2` HALFTUBE/ROUND, `>1` RIBBON/ROUND, else RIBBON/HAIR; authored normals force ORIENTED | `imaging/hdSt/basisCurves.cpp:316-343` |
| At refineLevel 0 a cubic curve is **downcast to linear** and drawn as a polyline through every CV | `imaging/hdSt/basisCurves.cpp:290-301` |
| A vertex primvar longer than `Σ curveVertexCounts` is accepted **only** with topology indices; otherwise it is replaced wholesale by the fallback plus `TF_WARN` | `imaging/hdSt/basisCurvesComputations.h:190-234` |
| An exact-size vertex primvar is **shared, not copied** (`primvars = _authoredPrimvar`) | `imaging/hdSt/basisCurvesComputations.h:201-203` |
| Any element-count change calls `SetNeedsReallocation()` unconditionally — in the **live `#else` branch**; the `#if 0` compacting branch above it is disabled upstream (the file's own comment at `:577-579` says so). `Reallocate` bumps `IncrementVersion()` | `imaging/hdSt/vboMemoryManager.cpp:604-618` (live; dead branch `:595-603`), `:427` |
| The upload unit is the whole prim (`CopyData` blits the entire buffer source into the range's offset) | `imaging/hdSt/vboMemoryManager.cpp:626` |
| Staging bypass threshold **512 KiB** (skipped entirely on unified-memory devices) | `imaging/hdSt/stagingBuffer.cpp:73-76` |
| With no authored `extent` the bbox is `[FLT_MAX, -FLT_MAX]`, "which disables frustum culling for the prim" | `imaging/hdSt/primUtils.cpp:887-891` |
| `minScreenSpaceWidths` is a recognised constant primvar name | `imaging/hdSt/basisCurves.cpp:1371-1382` |
| A dirty locator under `primvars` not ending in `primvarValue`/`indexedPrimvarValue`/`indices` **clears the cached primvar descriptors** | `imaging/hd/sceneIndexAdapterSceneDelegate.cpp:510-522` |
| `HDST_ENABLE_HGI_RESOURCE_GENERATION` defaults false; `HD_ENABLE_PERFLOG` must be `1` for counters | `imaging/hdSt/codeGen.cpp:166`; `imaging/hd/perfLog.cpp:25` |
| `GetPrim`/`GetChildPrimPaths` "expected to be threadsafe"; observer callbacks are **not** | `imaging/hd/sceneIndex.h:98, 110`; `imaging/hd/sceneIndexObserver.h:123, 132, 143, 151` |

The shipped prototype shader declares `hairId` as `"type": "float"` (`prototypes/storm-hair-look/usdGenHairPreview.glslfx:190-194`) — the source of ADR §1's S29 amendment, pinned by ADR §9 R12 (§6 K7).

### 3.7 Adapters and stage-free transport

| Fact | file:line |
|---|---|
| `UsdImagingSceneIndexPrimAdapter` exists, deriving from `UsdImagingPrimAdapter` | `usdImaging/usdImaging/sceneIndexPrimAdapter.h:27` |
| `primTypeName` and `includeDerivedPrimTypes` are the plugInfo keys the adapter registry reads; API-schema adapters use the same mechanism | `usdImaging/usdImaging/adapterRegistry.cpp:104-131, 137-201, 329-338` |
| `UsdImagingSceneIndexPlugin::InstanceDataSourceNames()` and `ProxyPathTranslationDataSourceNames()` are **non-const** virtuals — a `const override` will not compile | `usdImaging/usdImaging/sceneIndexPlugin.h:82-92` |
| `UsdImagingDataSourceMapped::AttributeMapping` carries a `factory` (used for REST-at-`Default()` and whole-`TsSpline` sources); `Invalidate(usdNames, …)` maps USD property names to locators | `usdImaging/usdImaging/dataSourceMapped.h:83-94, 124, 166` |
| Asset-path dependency tracking is specialised for scalar `SdfAssetPath` only, so `asset[]` gets no reload invalidation | `usdImaging/usdImaging/dataSourceAttribute.h:203-211` |
| `AutoApplyAPISchemas` / `apiSchemaAutoApplyTo` is plugInfo metadata and propagates to **derived** types | `usd/usd/schemaRegistry.cpp:84, 836-910, 918-932` |
| `UsdGeomRegisterComputeExtentFunction` requires `implementsComputeExtent` plugin metadata or the plugin is not loaded for the type | `usd/usdGeom/boundableComputeExtent.cpp:159-162, 271` |
| `UsdImagingDataSourcePrimvars::GetNames()` lists attributes only; `Get()` falls through to relationships (the `EV-083` mechanism) | `usdImaging/usdImaging/dataSourcePrimvars.cpp:97, 167-174` |

The measured consequence of these rows — without a registered adapter no `usdGen:*` attribute reaches Hydra and no notice is emitted — is §2.12.

### 3.8 `VtArray`, TsSpline, Python and the boost surface

| Fact | file:line |
|---|---|
| `Vt_ArrayForeignDataSource` — the public hook for a `VtArray` over caller-owned memory. Its constructor takes `void (*detachedFn)(Vt_ArrayForeignDataSource *self)` (`:42-46`); `_ArraysDetached()` calls it (`:51`) when the last array sharing the buffer releases it, which is the only legal signal that a pooled publish buffer may be reused | `pxr/base/vt/array.h:39-51` |
| **There is no public `VtArray::IsUnique()`.** `inline bool _IsUnique() const` is a **private** member used only internally (`:424, 593, 633, 745, 850, 1010`); the only public `IsUnique()` in `pxr/base/vt` is on the refcount holder in `value.h` | `pxr/base/vt/array.h:1023`; `pxr/base/vt/value.h:105` |
| `TS_SPLINE_SUPPORTED_VALUE_TYPES` is `double, float, GfHalf, GfTimeCode` — the reason a **colour ramp cannot be a `TsSpline`** and is encoded as `float[] <p>:positions` + `color3f[] <p>:colors` (ADR §9 R11) | `base/ts/types.h:32-37` |
| `UsdAttribute::HasSpline / GetSpline / SetSpline` | `usd/usd/attribute.h:552, 563, 568` |
| `PXR_PYTHON_SUPPORT_ENABLED` and `PXR_USE_INTERNAL_BOOST_PYTHON` (the source tree ships only the CMake template; the generated copy is `<install>/include/pxr/pxr.h`, identical line numbers here) | `pxr/pxr.h.in:48, 59` |

The installed `include/pxr/external/boost/python.hpp`, `lib/libusd_boost.so` and `lib/libusd_python.so` make a `PXR_BOOST_PYTHON_MODULE` buildable out of tree (`EV-055`).

### 3.9 Platform

`garch` on Linux is GLX-only (`imaging/garch/glPlatformContextGLX.cpp`), but `HgiGL` only calls `GarchGLApiLoad()` (`imaging/hgiGL/hgi.cpp:54`), which resolves through `glXGetProcAddressARB` (`imaging/garch/glApi.cpp:3142`). Hio's built-in image types are `bmp, jpg, jpeg, png, tga, hdr` (`imaging/hio/plugInfo.json:8`). `PXR_ENABLE_PTEX_SUPPORT` defaults OFF (`cmake/defaults/Options.cmake:36`).

### 3.10 Known upstream defects the plan contains

| Defect | file:line | Containment |
|---|---|---|
| **`esfUsd` resync predicate.** `_stage->GetPrimAtPath(resyncedPath)` is passed to `UsdPrimDefaultPredicate` with no validity check; the predicate posts `TF_CODING_ERROR("Applying predicate to invalid prim.")` and returns false | `exec/esfUsd/stageData.cpp:351, 361`; `usd/usd/primFlags.cpp:24` | Fires on **every** `UsdStage::RemovePrim` while an OpenExec system is attached; in Python it **raises**. Never `RemovePrim` in interactive paths; `SetActive(false)` and remove-then-re-`CopySpec` in one `Sdf.ChangeBlock` do not trip it. Contain with `TfErrorMark` (C++) or `try/except Tf.ErrorException` + a post-condition assert (Python). Reproduced with zero usdRig code (`EV-079`) |
| `instanceIndices` documented as a vector of `HdInstanceIndicesSchema` | `extras/imaging/docs/hydra_prim_schemas.dox:233-245`; the file declares itself a Nov-2023 snapshot at `:4-5` | Build against `imaging/hd/instancerTopologySchema.h:125-126` |
| `hdSt/basisCurves.cpp:651-654`'s "points may be larger than the topology implies" applies only to the topological-visibility BAR | contradicted at `basisCurvesComputations.h:212-223` | Assert `points.size() == Σ curveVertexCounts` in the publisher (gate SI-1) |
| **`UsdImagingGeomSubsetAdapter` locator asymmetry.** The adapter invalidates the **bare** `indices` / `type` locators while the data source it publishes lives under the `geomSubset` container, whose default locator is `geomSubset` | `usdImaging/usdImaging/geomSubsetAdapter.cpp:165-185`; `imaging/hd/geomSubsetSchema.cpp:119-121`; `usdImaging/usdImaging/stageSceneIndex.cpp:855-876` forwards the locators unmodified | The dirty router keys a `geomSubset`-typed prim on the bare leaves and additionally accepts the prefixed spellings (`02-schema.md` §2.20 rule 5, §6.4), so an upstream fix changes nothing. Filed with the S46 list (`12-risks-decisions-open-questions.md` §0.2) |

### 3.11 Verification limits: approximate ranges, source-only reads, and verified negatives

* **Approximate sub-range (fact verified).** The reports quote `hdSt/renderDelegate.cpp:701-707` for Storm's render-context order. Re-verified 2026-09-05: the containing function `_RenderDelegateInfo()` spans **`:695-714`** (`:712` is blank, `return info;` is `:713`, the closing brace `:714`), and the `materialRenderContexts` assignment is `:702-707`. The fact is verified; only the sub-range was approximate. **The plan's citation is `:695-707`** (ADR §9 R43); `:695-714` names the containing function and nothing else. Re-checked across `plan/*.md` on 2026-09-05: every document now cites `:695-707`, and no citation of `:701-707`, `:701-710` or `:695-712` survives outside this bullet.
* **Source-verified only, never run.** Every hdPrman line number in `research/G-hdprman-and-usdrecord-render-time-chain.md` is a source read — hdPrman is present in the source tree but **not built or installed here** (§1.2). All hdPrman behaviour is tagged source-verified and gated on **R-1**, a tier-4 release criterion, never a milestone exit (ADR §9 R40).
* **Proved against a null delegate, not Storm.** "Storm inserts nothing between the merging index and the renderer plugins" was exercised with a **null** render delegate (`research/G-chain-order-probe.md` §3). Gate **SI-5** re-runs the chain dump under Storm on the EGL harness.
* **Verified negative.** `HdSceneIndexCreateArgsSchema::GetMotionBlurSupport` (`imaging/hd/sceneIndexCreateArgsSchema.h:88`) has **no consumer** anywhere in `pxr/` (grep). The setter `Builder::SetMotionBlurSupport` is called only by `imaging/plugin/hdEmbree/rendererPlugin.cpp:45` and `imaging/plugin/hdStorm/rendererPlugin.cpp:57`, and hdPrman sets it nowhere — so the bit cannot detect a blur-capable renderer and the plan never uses it.

---

## 4. Verified usdRig facts (file:line)

Verified at `c92c040`.

| Fact | file:line |
|---|---|
| RigExec inserts three scene indices (`InternalPrimPruning` → `BindingResolving` → `Results`) from one `UsdImagingSceneIndexPlugin::AppendSceneIndex`, then registers the chain with the process-global registry | `libs/rigExecImaging/sceneIndexPlugin.cpp:23-45` |
| The C ABI is `extern "C"` — `RigExecImaging_Activate / SetTime / Deactivate / GetGeneration / …` — the ctypes surface usdGen mirrors | `libs/rigExecImaging/registry.h:149-201`; impls `registry.cpp:1160, 1198` |
| The snapshot store publishes with `std::atomic_store`, reads with `std::atomic_load`, and returns a diffed dirty vector | `libs/rigExecImaging/snapshotStore.h:291-315, 326-377` |
| The registry publishes then broadcasts (`_Publish` → `_Broadcast`) | `libs/rigExecImaging/registry.cpp:178-184, 338, 368-376` |
| RigExec **blocks** upstream `velocities` and `accelerations` whenever it owns `points` — ownership, not value comparison | `libs/rigExecImaging/sceneIndices.cpp:1352-1363` |
| RigExec uses `HdContainerDataSourceEditor::ComputeDirtyLocators` because it overlays an upstream container | `libs/rigExecImaging/sceneIndices.cpp:2462, 2505, 2700` |
| `AttributeSnapshot` / `EditEntry` / `Edit` / `UndoStack` (`LIMIT = 200`) / `EditRecorder` — the undo model `SubtreeSnapshot` slots into unchanged | `plugin/rigExecUsdview/rigExecUndo.py:20, 41, 70, 107, 115, 144, 147, 203` |
| The usdview `PluginContainer` is `RigExecUsdviewContainer.registerPlugins`, connecting `dataModel.currentFrameChanged` — the pre-redraw commit point | `plugin/rigExecUsdview/rigExecUsdview.py:94, 96, 174` |
| `ImagingLibraryPath()` honours `RIGEXEC_IMAGING_DLL` and otherwise searches only two in-repo paths | `plugin/rigExecUsdview/rigExecUsdview.py:31-41` |
| Codeless-schema generation: `bin/gen_schema.sh` runs `usdGenSchema` over `libs/rigExecSchema/schema.usda` into the checked-in `plugin/rigExecSchema/resources`, then strips `LibraryPath` and rewrites the resource/root placeholders to `"."` | `bin/gen_schema.sh:1-29`; `libs/rigExecSchema/schema.usda:17-24` (`bool skipCodeGeneration = true`) |
| The generated-plugInfo pattern: `LibraryPath` is `$<TARGET_FILE_NAME:…>`, generated into `<build>/usd/<name>/resources` so one relative hop serves the build and install trees | `CMakeLists.txt:418-426, 462` |
| `CMAKE_INSTALL_RPATH_USE_LINK_PATH ON`; `RIGEXEC_BUILD_PYTHON` auto-detects pybind11 | `CMakeLists.txt:54, 301-312` |
| usdRig's only SIMD kernel is SSE2 + scalar fallback and takes the scalar path on aarch64 — nothing to inherit | `libs/rigExecMath/simdKernels.cpp:12-20` |

---

## 5. Unmeasured claims the plan relies on, and the gate that measures each

**`09-performance-and-benchmarks.md` §5 is the single gate registry** (ADR §9 R40): it owns every id, tier, milestone and threshold, and this table cites it. **This appendix mints no gate id.** The ids this section leans on, re-checked against 09 §5 on 2026-09-05:

| Id | What 09 §5 registers | Tier / M |
|---|---|---|
| **S-10** | the v2 in-place overlay on a frozen prim (ADR §9 R29) | T2 / M8 |
| **S-11** | the `head1M` static-draw record | T2 / M7 |
| **S-12** | the prim-count (tile) sweep, before C2 freezes | T2 / M1 |
| **SI-9** | cost of the private pruning wrapper on a production-density skinned scalp | T1 / M2 |
| **SI-10** | two scene-index instances attached to one session agree on generation, prim set and frame | T1 / M2 |
| **SI-11** | does `reorder nameChildren` reach the scene index as an invalidation (record only) | T1 / M0 pre-work, run as **PW-6** (`11-roadmap.md` §1.1) |
| **B-1** | the link rule, `DT_NEEDED` of `libusdGen.so` (09 §5.1) | T0 / M0 |

`T-EXPR-1` and `T-PTEX-1` (09 §5.4) are `12-risks-decisions-open-questions.md`'s and are not disputed. `12-…` §2, §3 and §5 cite the same assignments.

| # | Unmeasured claim | Gate | M | If it fails |
|---|---|---|---|---|
| U1 | 32–256 tiles draw as fast as 1 prim once batched (every §2.3 row used one prim) | **S-5** M1, **S-1** M1, **S-3** M2, **S-12** M1 | M1–M2 | Tile count drops toward 32; contract C2 is not frozen until S-5, S-1 and S-12 are green |
| U2 | Storm draw at 100 k × 8 CV, refineLevel 2, at 720p and 1080p | **S-1** at 100 k | M1 | The frame ledger is re-derived; LOD decimation becomes mandatory sooner |
| U3 | A points-only publish avoids a topology re-upload and keeps `vboRelocated == 0` | **S-6** M1, **S-2** M2 | M1–M2 | The publish contract changes; density scrubbing is re-opened |
| U4 | `hairTangent`: variant A (`inData.Neye`, no primvar) vs variant B (published primvar) on a deforming groom, and whether A compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` | **S-8** | M0 pre-work, binding at M1 | B becomes the default and `hairTangent` is republished per deforming frame at ≈ +1.5–2.5 ms (§2.11, DERIVED). ADR §5.4 makes the *decision* an M1 exit |
| U5 | The **switch cost** of refineLevel 1 ↔ 2 (the frame times themselves are measured, `EV-021`) | **S-9** | M0 pre-work, binding at M1 | No "tumble tier"; LOD stays decimation-only |
| U6 | Storm resolves `outputs:glslfx:surface` before plain `outputs:surface` (source says yes, §3.6; never run) | **L-1** | M0 pre-work, binding at M1 | `USDGEN_STORM_MATERIAL_OVERRIDE` flips to default ON and the synthesized `material_storm` prim ships |
| U7 | nanoflann kNN over 100 k / 1 M rest roots at 8 threads | **E-4** | **M0** pre-work, **binding at M3** | The reference lane's capture budget is re-planned; > 300 ms at 1 M triggers SC-6 |
| U8 | The ragged-CV path's cost relative to the uniform path (threshold in `09-performance-and-benchmarks.md` §5.1) | **E-1r** | **M2** | `UsdGenCurveSource` imports are resampled on entry by default. The ragged path does not exist before M2 (ADR §4.2.2 puts `UsdGenCurveSource` and `UsdGenResample` in v1/M2) |
| U9 | Initial population: a renderer-level index over an already-populated input finds the groom roots identically to the notice path, within the ≤ 5 ms ASSUMPTION of ADR §9 R28 | **SI-6** | M1 | The bounded first traversal is replaced by a documented host requirement |
| U10 | Adapter coverage: every `usdGen:*` property of every registered type appears in Hydra and dirties (the "without an adapter, nothing appears" baseline is §2.12) | **SI-7** | M1 | Registrations are added until it passes — the panel's most consequential shared defect |
| U11 | `UsdGenMaskAPI` auto-applies to a **codeless** `UsdGenOperator` (the registry propagates auto-apply to derived types, §3.7; codeless untested) | **SI-8** | M0 pre-work, binding at M1 | The tool keeps applying the API schema explicitly, as it does until SI-8 is green |
| U12 | Exactly one commit per interactive edit batch, each on the thread ADR §4.3 and §9 R32 assign its trigger — **app thread for (a); notice thread for (b) and (c)** — and never on a Hydra reader thread (`GetPrim` only `atomic_load`s) | **SI-3** | M1 | The trigger table is revised; M1's stop condition may fire, **and SI-3's inherited "all on the main thread" threshold (`design/proposal-performance.md` §11.2) is corrected in `09-performance-and-benchmarks.md`** |
| U13 | `reorder nameChildren` reaches the scene index as an invalidation | **SI-11** (T1, record only; 09 §5.2), run as pre-work item **PW-6** (`11-roadmap.md` §1.1) | M0 | Nothing depends on it — the check exists to close Q-06 and to justify in writing that no evaluated result depends on namespace order (S26). `SI-10` is the session-arbitration gate |
| U14 | hdPrman motion parity: `k · tail` evaluations, not `k · chain`, with correct blur | **R-1** | release | R-1 is tier 4 and hdPrman is not built here (§1.2); M7 ships the P1/P2 motion work R-1 later validates. Motion profile P2 is re-costed if it fails |
| U15 | MSAA / alpha-to-coverage vs OIT quality on 1-px strands at 1080p and 4K | **R-2** | release | The default material tag changes |
| U16 | Metal/Vulkan Hgi path compiles the glslfx (only the env-var proxy is available here) | **R-3** | release | The shader is reworked before a Mac ships |
| U17 | Instancer pick round-trip and prototype rebasing for cards/archives | **T-INST-1 / T-INST-2** | M6 | `primOrigin` authoring for instancers is revised |
| U18 | A usdview plugin can enable async polling by setting `_allowAsync` in `registerPlugins` (sound by code order; never run) | **T-5** | M8 | Progressive generation needs the `--allow-async` flag |
| U19 | Cost of `HdSiExtComputationPrimvarPruningSceneIndex` on a production-density skinned scalp (the probe mesh had 10 points) | **SI-9** (T1, M2; ADR §9 R40) | M2 | The private pruning wrapper gets a cache |
| U20 | `libusdGen.so` links none of `usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`, `usdAppUtils` (link-time enforcement of S8 / ADR §4.2.3, §9 R37) | **B-1** (T0) | M0 | The `UsdGenGraphDesc` boundary has leaked; tier-0 tests would need a stage and the render-time claim of S8 is void |

---

## 6. Corrections history

Each row was wrong in an earlier document. The plan cites the corrected statement; the original stays in place in its report so the audit trail survives.

| # | Wrong statement (where) | Corrected statement (where the correction lives) |
|---|---|---|
| K1 | Three linked claims in `research/ENVIRONMENT.md`'s body: "garch is GLX-only, therefore Storm needs an X display"; "anything needing a GL context cannot run"; "all Storm numbers must be a workstation protocol, labelled UNMEASURED" | The premise is true, the conclusions false. Storm renders on the measurement host through an EGL device-platform context; correctness runs under Xvfb + llvmpipe; only `usdrecord`'s own binary core-dumps. §2.3 is MEASURED for single-prim draw and deform. The protocol caveat survives only for multi-prim cost, MSAA/OIT and Metal/Vulkan (§5 U1, U15, U16) — `research/G-storm-hair-look-prototype.md` §0, §5–§6; §1.4 here |
| K2 | "`pybind11` and `numpy` are NOT installed"; "ninja available" — `research/ENVIRONMENT.md` body | Both packages are installed; ninja was not and is now in the venv — `research/B-usdrig-build.md` §1; §1.3 here |
| K2b | "No Xvfb installed" — `research/ENVIRONMENT.md` body | An Xvfb unpacked into user space with `dpkg-deb -x` serves `DISPLAY=:77` with llvmpipe and no root; `usdview`, `testusdview` and `usdrecord --renderer GL` all run there. This is what makes **test tier T3** a CI tier (ADR §9 R2) — `research/ENVIRONMENT.md` CORRECTIONS; §1.4 here |
| K3 | "The `Usd_PrimFlagsPredicate` error comes from RigExec holding a `UsdPrimRange` across a resync" — `usdRig/docs/curvenet.md:513-522` | Stock OpenUSD 26.08 (`exec/esfUsd/stageData.cpp:361`), reproduced with zero usdRig code — `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4; §3.10 here |
| K4 | "Chunks are the unit of dirtiness, parallelism **and Hydra prims**" (S23) — `design/brief-v1.md` §2.4 | **Chunk ≠ tile.** Chunk = 512 curves (the dirty/parallel unit); the Hydra prim is a *tile*. `chunksPerTile = max(1, ceil(nChunks / usdGen:tileTarget))`; `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))`. At 100 k curves / chunk 512 / `tileTarget` 64: 196 chunks → 4 chunks per tile → **49 tiles**; 64 tiles is the 1 M-curve figure — ADR §1 (S23) as superseded by **ADR §9 R21** |
| K5 | `ordering.after: ["hd:sceneGlobals"]` guarantees placement after the scene-globals index (S1) — `design/brief-v1.md` §2.1 | The tag is **decorative**: no plugin carries it and the index is inserted by the engine's app callback. Placement comes from phase 0 / `InsertionOrderAtEnd`. Keep the tag, never rely on it — ADR §1; `design/judge-evidence.md` §0; §3.1 here |
| K6 | A `GetPrim` backstop commits when no app driver is attached (S18(c)) — `design/brief-v1.md` §2.3 | **Withdrawn**: a reader thread cannot emit notices and a cook that emits none is useless. Replaced by trigger (c), committing synchronously on the notice thread at the end of the `_PrimsDirtied` batch — ADR §1, §4.3, §9 R32; `design/judge-delivery.md` §5.1; §5 U12 here |
| K7 | `hairId` is an int primvar — `design/proposal-performance.md` §6.2 | Uniform **float** in [0,1): **`hairId = UsdGenHash32(curveId, 0) / 2^32`** over a 64-bit `uint64[] primvars:usdGen:curveId` (ADR §9 R12, which also supersedes S42's `uniform int` curve id). Decimation uses `kSaltDensity ≠ 0`, so the kept set is never `{hairId < scale}`. The shipped glslfx declares `"type": "float"` — `prototypes/storm-hair-look/usdGenHairPreview.glslfx:190-194` (verified) |
| K8 | "refineLevel is not a useful lever" — all three proposals | refineLevel 1 measured **8.17 ms** vs **23.93 ms** for level 2 at 200 k curves: a 2.9× lever whose *switch cost* is unmeasured — ADR §1 (S31); row `EV-021`; gate S-9 |
| K9 | `instanceIndices` is a vector of `HdInstanceIndicesSchema` — `extras/imaging/docs/hydra_prim_schemas.dox:233-245` | `HdIntArrayVectorSchema` (`imaging/hd/instancerTopologySchema.h:125-126`); the dox is a self-declared Nov-2023 snapshot (`:4-5`) — `research/G-instancing-cards-archives-and-native-instances.md` §1; §3.5 here |
| K10 | A `points` array longer than the topology implies is tolerated — `hdSt/basisCurves.cpp:651-654` comment | Only with `curveIndices`, at 15× the update cost; otherwise the whole prim renders the fallback colour. **Padding is banned** — row `EV-026`; §3.6 |
| K11 | `float[2]`/`float[4]` glslfx parameters do not bind — `research/A5-storm-curves-shading.md` open question, and the stale hint at `testUsdImagingGLSdr/surface_texture.glslfx:59` | They bind as `vec2`/`vec4`; verified in the generated GLSL — `research/G-storm-hair-look-prototype.md` §2.4 |
| K12 | "With no `primOrigin` the hydra path is left unchanged" — `research/A3-usdrig-tools.md` §3.2 | `GetFullPath` returns an **empty** `SdfPath` and usdview selects nothing — `research/G-tool-loop-array-transport-and-cv-picking.md` §4; §3.5 here |
| K13 | Tests can select the terminal scene index with `names[-1]` — `usdRig/tests/testUsdviewRigExec.py:16-39` | The name registry is an unordered map; select by the `"[Terminal SI] "` prefix — same report, §4 |
| K14 | "hdPrman behaviour is UNVERIFIED / hdPrman is not in this tree" — `research/A2`, `research/A4` | hdPrman **is** in the source tree under `third_party/renderman`, built only with `PXR_BUILD_PRMAN_PLUGIN=ON` and **not installed here**; its chain is source-verified — `research/G-hdprman-and-usdrecord-render-time-chain.md` §0; §1.2, §3.11 here |
| K15 | "12 329.5 ms to author a 24-sample `.usdc`" — first run of `probe5_bake_size.py` | 80.6 ms; the 12.3 s was Python building 24 `Vt.Vec3fArray`s outside USD. All array construction moved outside the timers — row `EV-046` |
| K16 | Tool-loop numbers taken at host load 43–59 — first pass of `bench_pick_cpu.py` | Discarded: unpinned multi-threaded BLAS inflated every number 10–20×. Re-taken at load 0.37–0.47 with BLAS pinned — rows `EV-064`–`EV-065` |
| K17 | Slices named `S0`–`S8`, colliding with the brief's `S1`–`S46` — `design/proposal-risk.md` §9 | Milestones are **M0–M8**; gate families are `E-`/`SI-`/`S-`/`L-`/`T-`/`T-INST-`/`R-`/`B-` — ADR §7, §9 R1–R2; `11-roadmap.md` |
| K18 | "The `ExecTypeRegistry::GetInstance()` trap always applies" — `usdRig/docs/mover-graph-cutover.md:511-519` | It does not reproduce in a single-binary VDF program; it is real for plugin-registered and exec-level types. Keep the forced `GetInstance()` in library init — `research/G-data-plane-engine-prototype-benchmark.md` §7 |
| K19 | "Ramps are `float2[] knots` + `float[] values` + a token interpolation" (S11) — `design/brief-v1.md` §2.2 | A **scalar** ramp is `float2[] <p>:knots` + `token <p>:interpolation`, the `float2` carrying `(t, value)`. A **colour** ramp cannot be a `TsSpline` (`TS_SPLINE_SUPPORTED_VALUE_TYPES`, §3.8) and is `float[] <p>:positions` + `color3f[] <p>:colors`. Whole-`TsSpline` transport rides the adapter's `AttributeMapping::factory` on `float <p>:spline` — ADR §9 R11; §3.7–§3.8 here |
| K20 | "Handoff to Hydra is a double-buffered publish ring" — `design/proposal-performance.md` §5 (P6, now invariant I6) | `VtArray` copy-on-write is the **default** handoff (publish 0.0000–0.0003 ms, MEASURED; the CoW detach on the next edit is **disputed and UNMEASURED** — 3.10 ms vs 1.72–1.91 ms in the carried run, 2.32 vs 1.97 ms in the report's — row `EV-010`, gate **E-1**). The ring is an optimisation only; the guard is the `Vt_ArrayForeignDataSource` detached callback, **not** `VtArray::IsUnique()`, which does not exist (ADR §9 R20; `pxr/base/vt/array.h:1023` is a private `_IsUnique()`) — §3.8 here |
| K21 | "Whether Storm prefers a `glslfx:` render-context output over plain `outputs:surface` is UNVERIFIED" (S36) — `design/brief-v1.md` §2.7 | Source-verified: `materialRenderContexts` are `{glslfx, mtlx}` in that order and the filtering scene index takes the first match (§3.6). Still never **run**, so `USDGEN_STORM_MATERIAL_OVERRIDE` ships default OFF pending gate L-1 — ADR §1 (S36); §3.11 here; §5 U6 |
| K22 | "Net: 24 fps unless the plan sets it" — `research/G-hdprman-and-usdrecord-render-time-chain.md:146`, `:215` | The premise (nobody calls `SetTimeCodesPerSecond`) is true; the conclusion is false. The stage's `timeCodesPerSecond` reaches the velocity scene index through the sceneGlobals **overlay**; the hard-coded 24 is the no-stage-opinion fallback only — `research/G-motion-blur-sampling-strategy.md` §2 and Key facts; §3.2 here |
| K23 | "The publish ring is permitted when guarded by `VtArray::IsUnique()` on the back buffer" (S24) — `design/adr-v1.md` §1; `design/judge-delivery.md` §3.4 | **`VtArray::IsUnique()` does not exist** — only a private `_IsUnique()` (`pxr/base/vt/array.h:1023`). The guard is a `UsdGenGenerationBuffer : Vt_ArrayForeignDataSource` whose detached callback returns the buffer to the session pool (`pxr/base/vt/array.h:39-51`) — ADR §9 R20; §3.8 and row `EV-010` here |
| K24 | `nTiles = clamp(ceil(nChunks/chunksPerTile), 32, 256)` — `design/adr-v1.md` §1 (S23/S27 amendment) | The `min(nChunks, …)` term was missing: without it a groom with fewer than 32 chunks is asked for more tiles than it has chunks. The correct form is `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` — ADR §9 R21; K4 above |
| K25 | Row ids `A2.1-E3` / `A2.3-ST2` / `A2.4-CPU7` (this file, first draft) and, in `09-performance-and-benchmarks.md` §1, first the `Q<n>` / "A-row" shorthand and then the 100-block `EV-1nn`…`EV-9nn` ids | Measurement rows are **`EV-001`…** and are the only citation handle for a measured number — ADR §9 R1, R43. §2.0 here maps the retired ids |

---

## 7. Testing

This appendix is a reference document, so what is tested is that its contents stay true.

| What is proved | Tier | Gate / mechanism |
|---|---|---|
| Every `file:line` cited in §3 and §4 still resolves to the quoted symbol after an OpenUSD or usdRig upgrade | **T0** | a `check_citations` script (grep each symbol at the cited file, fail on drift), run with the tier-0 unit tests |
| Every §2 row with a live probe still reproduces within its stated range | **T0** (engine rows §2.1–§2.2 and third-party rows §2.8 — no Hydra, no stage), **T1** (scene-index, transport and adapter rows §2.4–§2.5, §2.7, §2.12), **T2** (Storm rows §2.3 via the EGL harness, except `EV-091`, a **T0** file-size check), **T3** (the H1-SW rows `EV-050`, `EV-051`, `EV-053`, `EV-054`) | the gate that owns the number: **E-1, E-2, E-3, E-5, E-7, E-1r** for §2.1–§2.2; **SI-1…SI-5** for §2.4–§2.5; **S-1…S-12** for §2.3; **T-1…T-5**, **T-INST-1/2** for §2.6–§2.7; **SI-7** for §2.12; **B-1** for §2.10 (`EV-092` is the tier-1 harness's own cost and re-runs with every T1 suite); **R-1…R-3** and **L-1** for the tier-4 and look rows. §2.9's floors re-run from `prototypes/motion-blur/mbbench.cpp`; **E-1** re-derives the transport pass over 1.6 M `float3` |
| Every §5 claim names a gate that `09-performance-and-benchmarks.md` §5 registers, and every gate in that registry is named somewhere in this appendix (§2.11, §5 or §7) | **T0** | a cross-reference check; a §5 row with no gate id, a gate id this appendix uses that the registry does not define, or a registered gate mentioned nowhere here, fails. **Gates whose §2 baseline is a *partial* one, where the gate re-runs a larger or different form:** SI-3 (`EV-038`, a model probe, not the shipping index), SI-4 (`EV-039`, 8 readers × 20 publishes; the gate runs 8 × 100), T-4 (`EV-050`, 3.45 ms author at 2 205 prims), E-6 (`EV-013`, VDF's 1.4 µs/node — a different compiler), E-1r (`EV-001`, the uniform path), S-1 (`EV-020`/`EV-021`, the interpolation endpoints), S-2 (`EV-022`, which includes a `UsdAttribute::Set`), S-9 (`EV-021`'s 8.17 vs 23.93 ms, but not the switch cost), T-1 (`EV-061` + `EV-002` as components), T-2 (`EV-064`, 166 µs / 1.66 ms — the gate re-runs it through the shipped C ABI), L-4 (`EV-067`, `EV-069`, `EV-070` — per-eval floors, not a 1 M-root capture). **Gates with no §2 baseline row at all, because they are pure assertions, behaviours or captures nobody has run:** SI-1, SI-2, SI-5, SI-6, SI-7, SI-8, SI-9, SI-10, SI-11, E-4 (§2.11 records only the extrapolation), E-8, S-3, S-4, S-5, S-6, S-7, S-8, S-10, S-11, S-12, L-1, L-2, L-3, L-5, T-3, T-5, T-EXPR-1, T-PTEX-1, T-INST-1/2, R-1…R-3, B-1 — the check must allow these |
| No plan document quotes a §2.11 number without its tag | **T0** | grep the bare figures (`12.2 ms`, `0.4–0.6 ms`, `0.04 ms`, `0.25 ms`, `0.8–1.0 ms`, `1.44 ms`, `15.8–16.0 ms`, `10 ms`, `10–150 ms`, `40 µs`, `5 ms`) across `plan/*.md` outside this file and require an adjacent `DERIVED`/`UNMEASURED`/`ASSUMPTION` token |

Tier definitions (T0 engine-only, no Hydra and no stage / T1 headless scene index / T2 Storm via EGL / T3 `testusdview` under Xvfb / T4 workstation) are ADR §9 R2 and are owned by `10-build-dependencies-testing.md`. Tier-4 gates (**R-1**, **R-2**, **R-3**) are release criteria, never milestone exits (ADR §9 R39–R40).

---

## 8. Out of scope

* **Design rationale** — why a number leads to a decision belongs in the document that makes the decision (`01-architecture.md`, `03-execution-engine.md`, `06-imaging.md`).
* **Gate ids, tiers, milestones and pass/fail criteria** — `09-performance-and-benchmarks.md` §5, the single registry (ADR §9 R40). This appendix mints nothing and cites it; §5.1 already carries **B-1**.
* **The frame ledger itself** — `09-performance-and-benchmarks.md` §0.2 (ADR §9 R41); §2.11 records only the provenance of its non-measured rows.
* **How to build and run the probes** — `appendix-B-prototype-inventory.md`; its §0.1, §0.2, §12 and **`PW-11`** row already record `prototypes/motion-blur/mbbench.cpp` as carried in, which §2.9 here agrees with.
* **Upstream bug reports** — the four S46 issues and the `geomSubset` adapter asymmetry to file live in `12-risks-decisions-open-questions.md` §0.2; §3.10 and §4 record only the evidence.
* **Renderers other than Storm and hdPrman** — hdEmbree declares no deformation blur; hdArnold and hdCycles are not on this machine.

---

## 9. Sources

Reports, with the sections drawn on: `research/ENVIRONMENT.md` (whole file, CORRECTIONS overriding the body) → §1, §6. `research/B-usdrig-build.md` §1–§6, §8 and its **Decisions this settles** (item 4, the headless tier-1 harness) → §1, §2.10, §3.10. `research/G-chain-order-probe.md` §2–§5 → §2.5, §3.1, §3.3. `research/G-data-plane-engine-prototype-benchmark.md` §2–§8 → §1.1, §2.1, §2.2. `research/G-evaluation-scheduling-and-batching.md` §4–§5, §7, §9 → §2.5. `research/G-freeze-bake-undo-and-frozen-reentry.md` §1–§4 (§3's bake-landing table is `EV-089`; §4.1 is `EV-041`, `EV-045` and `EV-090`; §4.3 is the table behind `EV-050`/`EV-051`) → §2.6, §3.10. `research/G-hdprman-and-usdrecord-render-time-chain.md` §0, §2, §3, §5 → §1.2, §3.11, §5, and §6 K22 as the superseded side. `research/G-instancing-cards-archives-and-native-instances.md` §1–§4, §6 → §3.5. `research/G-motion-blur-sampling-strategy.md` §1–§2, §4–§5 and its unnumbered **Key facts** → §2.9, §3.2 (the report has no §7). `research/G-stage-free-parameter-and-time-transport.md` §1–§3, §5–§7 → §2.12, §3.7, §3.8. `research/G-storm-hair-look-prototype.md` §0, §2, §5–§6 (§6 also carries the benchmark stages' on-disk sizes, `EV-091`) → §1.1, §1.4, §2.3, §3.6. `research/G-storm-throughput-and-prim-granularity.md` §1–§3 (**§1.8** is the per-prim, per-frame CPU-overhead table behind `EV-030`–`EV-032` and `EV-084`–`EV-088`) → §2.4, §2.11, §3.6. `research/G-tool-loop-array-transport-and-cv-picking.md` §1–§5 → §2.4, §2.7, §3.5. `research/A3-usdrig-tools.md` §6 (the conventions this file follows). `research/A5-storm-curves-shading.md` (the open question corrected by K11). `research/A7-prior-art-grooming.md` §9 (the operator catalogue behind ADR §9 R26's guide-fraction assumption). `research/A8-seexpr-ptex-libs.md` §0, §1.6, §2.8–§2.9 → §1.3, §2.8.

Design documents: `design/brief-v1.md` (S1–S46); `design/adr-v1.md` §1 (amendments, mined for K4–K8 and K19–K21), §2, §7, §8, and **§9 (the 2026-09-05 addendum, R1–R45)** — R1 fixes this file's row ids, R2 the gate families and tiers, R12 `hairId` and `curveId`, R20 the buffer-reuse guard, R21 the tile arithmetic, R26/R28 the two engine ASSUMPTIONs, R27 the 8-thread baseline, R37 the link rule and B-1, R40 the gate registry, R41 the frame ledger, R42 the number tags, R43 this appendix's contents. `design/judge-evidence.md` §0 (the API re-verification pass, the S1 and S36 findings) and §2; `design/judge-delivery.md` §3.4 (the buffer-reuse guard, superseded by ADR §9 R20), §4.1 (`hairTangent`/fastpath) and §5 (phase-1 blockers); `design/judge-artist.md` §2.1 (the 10–150 ms capture estimate) and its schema-legibility sections; `design/proposal-performance.md` §0.2 (the frame ledger whose provenance is §2.11), §5.3 (the ~40 µs recompile ASSUMPTION), §5.6/§7.4 (the capture estimate) and §11.2 (the gate matrix §5 maps onto, and SI-3's superseded threshold); `design/proposal-risk.md` §9 (the slice names corrected by K17).

usdRig documents cited only as the superseded side of a correction: `usdRig/docs/curvenet.md:513-522` (K3), `usdRig/docs/mover-graph-cutover.md:511-519` (K18).

Prototypes consulted: `prototypes/chain-order/`, `data-plane-benchmark/`, `evaluation-scheduling/`, `freeze-bake/`, `instancing/`, `motion-blur/`, `stage-free-transport/`, `storm-hair-look/`, `storm-throughput/`, `thirdparty-bench/`, `tool-loop/`, `usdrig-linux-build/`.

OpenUSD source read at `<openusd-src>` (v26.08, `ee47c679a`) and usdRig at `<usdrig-src>` (`c92c040`); every citation in §3 and §4 was re-verified by grep on 2026-09-05.
