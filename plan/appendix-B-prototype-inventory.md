# Appendix B: prototype inventory

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This appendix is the hand-over list for the code that already exists. Twelve prototype directories
live under `plan/prototypes/`; almost every measured number this plan quotes came out of one of
them, and all twelve were built and run on this host during the two research rounds. One exception
is recorded here: the 40 k / 200 k Storm frame times, whose scene generators were not copied (§7.6).
For each directory it records the purpose, the files, the exact commands that build and run it here,
what it proved and which report section carries that proof, which CMake target and milestone (ADR §7)
it is carried into, the caveats, and whether it builds today. §13 lists the prototypes that do **not** exist yet
and that M0 pre-work must write before the remaining gates can be measured.

Reads with: `09-performance-and-benchmarks.md` (the gate table and benchmark protocols these
prototypes implement), `10-build-dependencies-testing.md` (CMake targets, test tiers T0–T4,
third-party vendoring), `11-roadmap.md` (milestone contents M0–M8 and, in its §1, the pre-work
registry PW-1…PW-13 that §13 reproduces), `08-tools.md` §1.4 (contract C4, the C ABI this appendix does
not restate), `appendix-A-evidence-ledger.md` (every MEASURED number with its report section and
file:line).

---

## 0. Index and conventions

### 0.1 Index

| # | Directory | Proves | Feeds report | Carried into | Milestone |
|---|---|---|---|---|---|
| 1 | `chain-order` | S1 chain slot, S3 pruning, S5 bare-`primvars` rule | `research/G-chain-order-probe.md` | `usdGenTestUtilsHd`, `tests/testUsdGenChainOrder.cpp` | M0 harness / M1 gate |
| 2 | `data-plane-benchmark` | S21–S24 engine choice, SoA vs AoS, chunk size | `research/G-data-plane-engine-prototype-benchmark.md` | `benchUsdGenChain`, `usdGenTestUtils/sampler.h` | M0 harness / M1 gates |
| 3 | `evaluation-scheduling` | S17–S20 commit model, notice order, async path | `research/G-evaluation-scheduling-and-batching.md` | `tests/testUsdGenScheduling.cpp` | M0 / M1 |
| 4 | `freeze-bake` | S41 undo, S42 frozen contract C3, freeze cost | `research/G-freeze-bake-undo-and-frozen-reentry.md` | `usdgen/usdGenUndo.py`, `tests/python/test_usdgen_undo.py`, `tests/testUsdGenFrozenReentry.cpp` | M2, M5 |
| 5 | `instancing` | S33–S34 instancer synthesis, `primOrigin` picking | `research/G-instancing-cards-archives-and-native-instances.md` | `tests/testUsdGenInstancer.cpp`, `tests/testUsdGenInstancerPick.cpp` | M6 |
| 6 | `stage-free-transport` | S8–S16 stage-free transport, codeless schema, ramps, rest | `research/G-stage-free-parameter-and-time-transport.md` | `usdGenSchema`, `tests/testUsdGenAdapter.cpp` | M0 / M1 |
| 7 | `storm-hair-look` | S31, S35–S36 look, headless EGL Storm, refineLevel curve | `research/G-storm-hair-look-prototype.md` | `usdGenShaders`, `usdGenTestUtilsHd/eglctx.h` | M0 / M1 |
| 8 | `storm-throughput` | S27–S30 tile contract, invalidation discipline | `research/G-storm-throughput-and-prim-granularity.md` | `benchUsdGenStorm`, `usdGenImaging` (tile publisher) | M1 |
| 9 | `thirdparty-bench` | S38 SeExpr and Ptex embedding and cost | `research/A8-seexpr-ptex-libs.md` §1.6, §2.8 | `usdGen_seexpr`, `usdGen_ptex` | M4 |
| 10 | `tool-loop` | S39–S40 transport, CV picking, live primvars | `research/G-tool-loop-array-transport-and-cv-picking.md` | `_usdGen`, the C ABI in `usdGenImaging` | M5 |
| 11 | `usdrig-linux-build` | S44–S46 build story, `find_package(rigExec)`, resync bug | `research/B-usdrig-build.md` | root `CMakeLists.txt`, `bin/_env.sh` | M0 |
| 12 | `motion-blur` | S32 motion-blur memory floors | `research/G-motion-blur-sampling-strategy.md` | `tests/perf/benchMotionSamples.cpp` | M7 |

Target, CTest and module names in the "Carried into" column are
`10-build-dependencies-testing.md` §1.2 (targets), §1.1 (layout) and §5.6 (CTest names), which own
them; the Python package is the lowercase `usdgen` (ADR §9.1 R5). Where a name here and there differ,
`10-…` wins.

### 0.2 Where the code is

The twelve directories are **source-only copies** of working directories under a local session scratchpad
(`.../scratchpad/probes/<name>/`); build trees, generated stages, images and shader dumps were
dropped. The CMake configure tree that used to sit at `usdrig-linux-build/consumer/build/` is not
in the repository. `rigexec_env.sh` remains and reads `USD`, `RIG`, and `RIGBUILD` from the
environment. Three reference PNGs under `storm-hair-look` (§7.2) stay. The scratchpad
is temporary, so `plan/prototypes/` is the only durable copy and anything a report cites under
`…/scratchpad/…` but absent here is gone — each §x.6 lists what its directory lost. Directories were renamed on the way in: `probes/G-chain-order` → `chain-order`,
`probes/data-plane-engine-prototype-benchmark` → `data-plane-benchmark`, `probes/evalsched` →
`evaluation-scheduling`, `probes/G-stage-free` → `stage-free-transport`, `probes/B-build` →
`usdrig-linux-build`, `thirdparty/bench` → `thirdparty-bench`, and the scratchpad-root
`mbbench.cpp` → `motion-blur/mbbench.cpp`.

### 0.3 Host environment block

Every command below assumes this block — the usdGen analogue of the verified
`prototypes/usdrig-linux-build/rigexec_env.sh` (`research/B-usdrig-build.md` §7). Paths are host facts
from `research/ENVIRONMENT.md`.

```sh
export USD=$USD          # install: include/, lib/, lib/usd/, bin/
export USDSRC=<openusd-src>             # v26.08 source, read-only
export PROTO=<usdgen-src>/plan/prototypes
export PATH="$VENV/bin:$USD/bin:$PATH" # ninja 1.13.2 is in the venv
export LD_LIBRARY_PATH="$USD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$USD/lib/python3.12/site-packages${PYTHONPATH:+:$PYTHONPATH}"
PY=$VENV/bin/python3                   # 3.12.3, numpy 2.5.2, pybind11 3.1.0
```

Which graphics harness a prototype needs (`research/ENVIRONMENT.md` CORRECTIONS;
`research/G-storm-hair-look-prototype.md` §0):

* **EGL — GPU, real Storm numbers.** `prototypes/storm-hair-look/eglctx.h` makes a headless GL 4.6
  **compatibility** context on the GB10 via `EGL_EXT_platform_device` with a 64×64 pbuffer. A core
  profile yields ~25 `invalid enum` per frame (MEASURED, `research/G-storm-hair-look-prototype.md`
  §0) and a white image; a surfaceless context leaves the default FBO incomplete and
  `HgiGL_ScopedStateHolder` fails. Both are handled inside `eglctx.h`.
* **Xvfb — CPU only.** `usdview`, `testusdview` and `usdrecord --renderer GL` need an X display; the
  user-space Xvfb on `DISPLAY=:77` supplies one, but every frame time from it is an llvmpipe CPU
  number, never a Storm GPU number (`research/B-usdrig-build.md` §6). `usdrecord` core-dumps headless
  because its `main` opens a GLX window; the fix is the `eglctx.h` driver that
  `storm-hair-look/render_hair.cpp` demonstrates in 39 lines.
  The X server itself is **not** under `plan/prototypes/`: it is the user-space tree
  `scratchpad/xvfbtry/root/usr/bin/Xvfb`, restarted with `scratchpad/xvfbtry/root/usr/bin/Xvfb :77
  -screen 0 1280x1024x24 -nolisten tcp -xkbdir scratchpad/xvfbtry/root/usr/share/X11/xkb &`
  (`research/ENVIRONMENT.md` CORRECTIONS). There is no packaged Xvfb and no sudo on this host
  (MEASURED: `which Xvfb` returns nothing, `$DISPLAY` is empty). `10-build-dependencies-testing.md`
  §5.2 settles how the shipped harness finds it — `USDGEN_XVFB_ROOT` names an extracted Xvfb userland
  and has no default — but every T3 gate (T-1, T-3, T-4, T-5) stays unreproducible from
  `plan/prototypes/` alone.

---

## 1. `chain-order`

**1.1 Purpose.** Settle S1 (usdGen is a renderer-level `HdSceneIndexPlugin`, phase 0 /
`InsertionOrderAtEnd`), S3 (the private `HdSiExtComputationPrimvarPruningSceneIndex` wrapper) and S5
(the bare-`primvars` dirty rule), by building the real chain and walking it.

**1.2 Files.**

| File | One line |
|---|---|
| `src/probeChainOrder.cpp` | type-order dump, chain walk, RigExec activate/`SetTime`, notice recorder, render-index walk, ext-computation pruning |
| `src/stub.cpp` | one TU built twice (`-DSTUB_TAG_ID=A\|B`); each copy registers a `UsdImagingSceneIndexPlugin` **and** an `HdSceneIndexPlugin` appending a self-naming recording filter |
| `src/stubRenderer.cpp` | a null `HdRendererPlugin` + delegate whose only job is a non-empty renderer display name |
| `stages/skelAndRig.usda` | a slab that is both UsdSkel-skinned and RigExec-moved, sublayering `usdRig/examples/06_LatticeBulge.usda` |
| `CMakeLists.txt` | `libusdGenStub{A,B,Renderer}.so` + `probeChainOrder` |
| `logs/transcript.txt` | the six-permutation sweep and the 24-node chain dump |

**1.3 Build and run.** The three `stub*/resources/plugInfo.json` files are missing from this copy
(§1.6); until they are re-authored only the default type-order dump runs.

```sh
cmake -S $PROTO/chain-order -B $PROTO/chain-order/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$USD
cmake --build $PROTO/chain-order/build
PXR_PLUGINPATH_NAME=$PROTO/chain-order/stubA/resources:$PROTO/chain-order/stubB/resources:\
$PROTO/chain-order/stubR/resources \
  $PROTO/chain-order/build/probeChainOrder --stage $PROTO/chain-order/stages/skelAndRig.usda \
      --renderindex --renderer GL --dump /LatticeAsset/Geom/Slab --pull
```

No GL: the render-index walk uses the null delegate. `--rigexec --time 48 --notices` reproduces the
RigExec numbers after sourcing `rigexec_env.sh`.

**1.4 What it proved** (`research/G-chain-order-probe.md` §2–§5, Key facts).

* A renderer-level plugin lands strictly downstream of the whole UsdImaging chain — MEASURED, a
  24-node dump with the stubs at slots 0–1 (`pxr/imaging/hd/renderIndex.cpp:205-214`, guarded at
  `:208`).
* `UsdImagingSceneIndexPlugin` order is a `std::set<TfType>` over heap addresses and flips with
  `PXR_PLUGINPATH_NAME` — MEASURED over six permutations (the plugin loop is
  `pxr/usdImaging/usdImaging/sceneIndices.cpp:68-78`, called once at `:302`, and sorts nothing).
  That rejects a second UsdImaging plugin.
* An application-constructed delegate has an empty display name
  (`pxr/imaging/hd/renderDelegate.h:584-589`; sole caller `pxr/imaging/hd/rendererPlugin.cpp:76-78`)
  and therefore gets **no** renderer-level plugins — the reason `stubRenderer.cpp` exists.
* A skinned mesh's terminal `primvars:points` is blocked and the private pruning wrapper restores it
  headlessly — MEASURED (§4a–§4b). RigExec upstream of UsdSkelImaging freezes the skin; promoting the
  dirty to the bare `primvars` locator fixes it exactly — MEASURED (§4c), i.e. S5 and bug S46.
* `RigExecImaging_SetTime` on `ArmShotAnim.usda` = 1.35–1.50 ms/frame — MEASURED (§5). It is the rig
  term of the frame ledger `09-performance-and-benchmarks.md` §0.2 owns (ADR §9.5 R41); the ≈ 15 ms
  that remains for usdGen inside a 16.6 ms frame is an ASSUMPTION, and per rig class.

**1.5 Carried into.** The chain walk and recording observer become `usdGenTestUtilsHd`
(`sceneFixture.h`, `recordingObserver.h`; they link `hd`/`usdImaging`, so they may not live in the
T0-only `usdGenTestUtils` — `10-build-dependencies-testing.md` §1.2);
`tests/testUsdGenChainOrder.cpp` is gate **SI-5** (T1), whose harness lands in M0 and which is an
**M1** exit criterion (ADR §9 R40). `stubRenderer.cpp` becomes the
null-delegate fixture that lets tier-T1 tests run `AppendSceneIndicesForRenderer` without Storm. The
pruning wrapper this validated is the `usdGenImaging` private wrapper of S3, delivered in M2
(ADR §7).

**1.6 Caveats.** The three stub `plugInfo.json` resource directories (`stubA/`, `stubB/`, `stubR/`,
plus a `plugInfo.ordering.json` variant) were **not copied**; without them only the type-order dump
runs. M0 re-authors them to the shape S1 fixes: two `UsdImagingSceneIndexPlugin` entries
(`UsdGenStubUiA`/`UsdGenStubUiB`), two `HdSceneIndexPlugin` entries (`UsdGenStubHdA`/`UsdGenStubHdB`,
`loadWithRenderer: ""`, phase 0 `InsertionOrderAtEnd`, tags `["usdGen:groom"]` per S1), one
`HdRendererPlugin` entry with a non-empty `displayName`, and the `plugInfo.ordering.json` variant that
adds `"ordering": {"after": ["UsdGenStubHdB"]}` to stubA
(`research/G-chain-order-probe.md` §3). `CMakeLists.txt` wants `-DRIGEXEC_IMAGING_LIB=<path>`.
`stages/skelAndRig.usda` hard-codes a usdRig example as a sublayer. The render-index walk used a
**null** delegate, so what Storm itself inserts between the merge and the renderer plugins is
UNMEASURED — the EGL harness of §7 closes it in M0.

**1.7 Status.** Builds today; `logs/transcript.txt` is its output and was re-read for this appendix.
A full re-run is blocked only by the missing plugInfo files.

---

## 2. `data-plane-benchmark`

**2.1 Purpose.** Decide S21 (bespoke TBB DAG over a persistent `VdfNetwork`), S22 (SoA internally,
AoS only at the Hydra boundary), S23 (chunk = 512 curves) and S24 (per-node buffers, `VtArray` CoW
handoff) with one shared kernel so the arithmetic is identical across engines.

**2.2 Files.**

| File | One line |
|---|---|
| `kernel.h` | the shared clump/frizz styler kernel (~16 flops/CV — MEASURED, `research/G-data-plane-engine-prototype-benchmark.md` §6): `StyleAoS`, `StyleSoA`, `HashNoise` |
| `tbbBench.cpp` | the chunked TBB DAG: per-chunk dirty bits, per-node buffers, `VtArray` CoW extract; `--curves --stylers --chunk --per-node-buffers --sparse-permil --serial` |
| `vdfBench2.cpp` | the corrected, mask-honouring VDF prototype; `--element strip\|cv --request-all` |
| `simdBench.cpp` | autovectorisation probe, built twice (`simdBench`, `simdBenchNoVec`) |
| `sampler.h` | in-process SIGPROF stack sampler (this host refuses `perf` and `ptrace` attach) |
| `results_main.txt`, `results_scale.txt` | captured output at 100 k, and at 1 k / 1 M curves |
| `CMakeLists.txt` | five executables, `-O3 -DNDEBUG`; VDF targets link `vdf exec tf gf vt work trace arch` |

**2.3 Build and run.** First delete the stale `add_executable(vdfBench vdfBench.cpp)`,
`target_link_libraries(vdfBench …)` and `target_link_options(vdfBench PRIVATE -rdynamic)` lines from
`CMakeLists.txt:14-16,26`: `vdfBench.cpp` was not copied (§2.6) and a clean configure fails without
that edit.

```sh
cmake -S $PROTO/data-plane-benchmark -B $PROTO/data-plane-benchmark/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release && cmake --build $PROTO/data-plane-benchmark/build
B=$PROTO/data-plane-benchmark/build
$B/tbbBench  --curves 100000 --stylers 5 --runs 7 --chunk 512 --per-node-buffers 1
$B/vdfBench2 --curves 100000 --stylers 5 --runs 7 --element strip --request-all 1
$B/simdBench 100000 200 ; $B/simdBenchNoVec 100000 200
/usr/bin/time -f "%M KB %e s" $B/tbbBench --curves 1000000 --stylers 5 --runs 1 --chunk 512
```

The install path is pinned inside `CMakeLists.txt` with `NO_DEFAULT_PATH`; edit that line, not the
command.

**2.4 What it proved** (`research/G-data-plane-engine-prototype-benchmark.md` §3–§6).

* 5-op chain, 100 k × 8 CV, 20 threads: TBB **1.72–1.91 ms** vs VDF **3.80 ms min / 8.32 ms median**
  (max 13.98 over n=7) — MEASURED (report §3; the max column is only in
  `prototypes/data-plane-benchmark/results_main.txt:18`). At 1 M curves TBB **7.59–7.67 ms** vs VDF
  69.9–134.8 ms, and one VDF node re-run alone costs 47 ms.
* Sparse edit (1 % of chunks) **0.035–0.044 ms**; terminal-parameter edit **0.18–0.41 ms** — MEASURED;
  the E-2 and E-3 baselines. Every chain timing in this section is the **20-thread** figure; ADR §9
  R27 requires budgets and gates to quote the **8-thread private-arena** number instead, where the
  same 5-op chain is 1.02 ms (MEASURED) against E-1's ≤ 1.5 ms.
* Peak RSS at 1 M × 8 CV with five per-node buffers **668 MB** (VDF 1 263 MB) — MEASURED; the E-5
  baseline.
* `VtArray` CoW extract of 800 000 CVs: **≤ 0.0003 ms** (`results_main.txt:95-101`). Holding an
  outstanding reference across the next full run costs one CoW detach of 9.6 MB — MEASURED twice:
  2.32 ms vs 1.97 ms in the report's run (report §3.4) and 3.10 ms vs 1.86 ms in this copy's captured
  `results_main.txt:60,102`. That is why ADR §9 R20 routes any buffer reuse (the optional publish
  ring, M7) through a `UsdGenGenerationBuffer : Vt_ArrayForeignDataSource`
  (`pxr/base/vt/array.h:39-51`) whose detached callback returns the buffer to the session pool — a
  buffer is reusable only after that callback has fired. `VtArray` has no public uniqueness query
  (`_IsUnique()` is private, `pxr/base/vt/array.h:1023`).
* GCC 13.3 autovectorises with 128-bit NEON: AoS `GfVec3f` **25.3 GFLOP/s**, planar SoA strip
  **80.9 GFLOP/s** — MEASURED (§6). That 3.2× is S22; `-mcpu=native` does not reach SVE, so layout,
  not intrinsics, is the lever.

**2.5 Carried into.** `kernel.h` + `tbbBench.cpp` become the tier-T0 perf target
**`benchUsdGenChain`** (`10-build-dependencies-testing.md` §5.6), the harness behind gates **E-1**,
**E-1r** (`--ragged`), **E-2**, **E-3**, **E-5**, **E-7** and **E-8** (thresholds and
milestones per `09-performance-and-benchmarks.md` §5, the single gate registry; ADR §9 R40);
`sampler.h` becomes `usdGenTestUtils/sampler.h`; `simdBench.cpp` stays as the layout regression that
guards the ADR §4.1 kernel rules (`RESTRICT`, `-ffp-contract=off`). The harness lands in M0 so the
baselines exist before the engine does. Gate milestones per ADR §9 R40: E-1, E-2, E-6, E-7 and E-8
at the M1 exit, measuring the M1 chain (Scatter → Grow → Noise → Length → Width) — E-6's own harness
is written in M1 (§2.6); E-3 and E-1r at M2; E-5 at M3, re-run at M7. E-4 is **not** this harness's
gate: it belongs to PW-1's `benchUsdGenKnn` (§13; M0 pre-work, binding at M3).

**2.6 Caveats.** `vdfBench.cpp` (the v1 prototype whose bug is §2 of the report) was **not copied**,
yet `CMakeLists.txt` still declares its target, so a clean configure fails until that is fixed. Every
timing is GB10-only (10× Cortex-X925 + 10× Cortex-A725); both engines regress past ~8–10 threads,
hence `USDGEN_THREAD_LIMIT`, the one-shot calibration, and E-7 stated as a ratio. The kernel is
uniform-CV only: the ragged path (`cvCount == 0` + `cvOffsets`, ADR §4.2) is unimplemented and gate
E-1r has no harness (§13, PW-9). Gate **E-6** (append a node to a 200-node groom: ≤ 0.2 ms, exactly one
node rebuilt) also has no harness here — `tbbBench.cpp` builds its DAG once and never recompiles a
sub-graph, so M1 writes that test against the real Merkle digest of ADR §4.2. Cross-chunk kernels are
unmeasured by design — they are v3.

**2.7 Status.** Builds and runs today apart from the stale target. Verified by re-reading
`results_main.txt` / `results_scale.txt`, which carry the host banner and every number above.

---

## 3. `evaluation-scheduling`

**3.1 Purpose.** Settle S17 (never cook in `GetPrim`, never cook in every `_PrimsDirtied`),
S18/ADR §4.3 (deferred commit, atomic snapshot publish, diffed dirties), S19 (no torn reads) and S20
(the progressive path).

**3.2 Files.**

| File | One line |
|---|---|
| `evalSched.cpp` | replays `UsdImagingGLEngine::PrepareBatch` order and prints the exact notice sequence per frame change and per edit |
| `models.cpp` | cook counts and cook threads for eager / lazy / deferred-on-frame / deferred-on-commit, plus an atomic-snapshot tearing test |
| `chainOrder.cpp` | where a renderer-level plugin lands relative to `HdsiSceneGlobalsSceneIndex`, hdGp and Storm's plugins, without a render index |
| `noticeCost.cpp` | CPU cost of the per-frame notice cascade at 1…5 000 scalps, batched and not |
| `asyncProbe.cpp` | whether `asyncAllow`/`asyncPoll` reach a buried filtering index, and whether notices emitted during the poll trigger a redraw |
| `run1.txt`, `models.txt`, `chainOrder.txt`, `noticeCost.txt` | captured output |
| `README.md`, `CMakeLists.txt` | the five targets |

**3.3 Build and run.**

```sh
cmake -S $PROTO/evaluation-scheduling -B $PROTO/evaluation-scheduling/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release
cmake --build $PROTO/evaluation-scheduling/build && B=$PROTO/evaluation-scheduling/build
$B/evalSched > run1.txt; $B/models > models.txt; $B/chainOrder > chainOrder.txt
$B/noticeCost > noticeCost.txt; $B/asyncProbe > async.txt
HDGP_INCLUDE_DEFAULT_RESOLVER=1 $B/chainOrder > chainOrder_hdgp.txt
```

All five run headless with no render delegate. `HDGP_INCLUDE_DEFAULT_RESOLVER` is a real env setting
(`pxr/imaging/hdGp/sceneIndexPlugin.cpp:25`, default false).

**3.4 What it proved** (`research/G-evaluation-scheduling-and-batching.md` §1, §4–§9).

* Every un-batched frame produces exactly **two `PrimsDirtied` calls**, and `/ sceneGlobals/currentFrame`
  is the **last** — MEASURED (`run1.txt` S1/S2/S7). That is commit trigger (b) of ADR §4.3. Post-merge
  batching inverts the order (S3/S8), so commit logic must not depend on it.
* Editing `usdGen:clumpScale` on a prim with no adapter produces **no notices at all** — MEASURED
  (`run1.txt` S5; `usdGen:clumpScale` is the probe's own ad-hoc attribute name, not a shipped
  property — the normative property registry is `02-schema.md`, ADR §9.2 R7). That is the measurement behind S10 and the ADR §2.3 adapter-coverage correction.
* Ten interactive events: eager cooks 20–29 times, lazy cooks 10 times **on 4 render threads**,
  deferred cooks exactly 10 times on one thread — MEASURED (`models.txt`); gate **SI-3**, which under
  ADR §9.4 R32 counts **exactly one cook per `_PrimsDirtied` edit batch**, with or without an app
  driver attached.
* Atomic-snapshot publish: **0 torn reads** under 8 concurrent readers — MEASURED twice: 16 734 reads
  in the report's run (`research/G-evaluation-scheduling-and-batching.md` §5) and 2 035 reads in this
  copy's captured `models.txt`. Gate **SI-4** re-runs it at 8 readers × 100 publishes.
* Renderer-level placement resolves as phase 0 immediately after `HdsiSceneGlobalsSceneIndex` and
  upstream of every `HdSt_*` plugin — MEASURED (`chainOrder.txt`). The app callback inserts scene
  globals at phase 0 `AtStart`, which is why ADR §1's S1 amendment calls the JSON
  `ordering.after: ["hd:sceneGlobals"]` tag decorative. The schema's own token list
  (`pxr/imaging/hd/sceneGlobalsSchema.h:37-47`) is `primaryCameraPrim`, `activeRenderPassPrim`,
  `activeRenderSettingsPrim`, `startTimeCode`, `endTimeCode`, `timeCodesPerSecond`, `currentFrame`
  and `sceneStateId` — there is no mode flag on it to read, which is why the design keys the render
  context off `USDGEN_CONTEXT` and `SetContext` instead (ADR §2.3).
* Notice-cascade tax: 0.003 ms at 1 scalp, **1.08–1.24 ms at 5 000** (this copy's carried
  `noticeCost.txt`; the report's median-of-3 run gives 1.13 / 1.13,
  `research/G-evaluation-scheduling-and-batching.md` §9, and that is the run
  `appendix-A-evidence-ledger.md` §2.5 registers) — MEASURED, two runs of the same probe. The call count is exactly 2 `PrimsDirtied` per frame at every scalp count and under both
  batching modes.
* `asyncAllow`/`asyncPoll` reach every filtering index, inputs first
  (`pxr/imaging/hd/sceneIndex.cpp:181-195`) — MEASURED (§8).

**3.5 Carried into.** `models.cpp` → `tests/testUsdGenScheduling.cpp` (gates **SI-3**, **SI-4**);
`chainOrder.cpp` merges with §1 into `tests/testUsdGenChainOrder.cpp` (**SI-5**); `evalSched.cpp`
becomes the fixture tier-T1 invalidation tests replay. All three land as harnesses in M0 and are M1
exit criteria (ADR §9 R40). `asyncProbe.cpp` becomes the T3 script `testUsdviewUsdGenAsync.py` (`10-…` §5.6) and gate **T-5** (T3), which is an **M8 / v2**
criterion, never a v1 milestone exit (ADR §9 R39, R40; `11-roadmap.md` §2.9).

**3.6 Caveats.** `async.txt`, `chainOrder_hdgp.txt` and `cfg.log` were not copied. The models probe
emulates the commit rather than driving a real session, so "exactly 10 commits" must be re-derived
against `UsdGenImagingRegistry` in M1. The `CMakeLists.txt` sets no default build type: without
`-DCMAKE_BUILD_TYPE=Release` the `noticeCost` wall-clock numbers do not reproduce (the cook counts
still do). Notice cost was measured over scalp meshes, not over 32–256
published tiles — §8 owns the tile side.

**3.7 Status.** Builds and runs today; four of five captured outputs are present and were re-read.

---

## 4. `freeze-bake`

**4.1 Purpose.** Settle S41 (`SubtreeSnapshot` undo, the `RemovePrim` landmine), S42 (the frozen-curve
contract C3 and the session / sublayer / payload landing tiers) and what one freeze costs the scene
index and usdview.

**4.2 Files.**

| File | One line |
|---|---|
| `probe1_freeze_undo.py` | `SubtreeSnapshot` (`Sdf.CopySpec` into an anonymous stash) plus author/capture/undo/redo timings and the sublayer alternative |
| `probe8_undo_fidelity.py` | 11 round-trip assertions (time samples, splines, children, relationships, metadata, variability, "spec did not exist") |
| `probe2_undo_resync_exec.cpp` | 9 removal/restore shapes × {no exec, `ExecUsdSystem` attached} |
| `probe3_python_rigged_undo.py` | the same matrix through the real `_rigexec.Rig` on `ArmShotAnim.usda` |
| `probe9_error_containment.py` | is the OpenExec diagnostic harmful, and is the removal still correct? |
| `probe4_frozen_reentry_si.cpp` | a frozen prim through `UsdImagingCreateSceneIndices`: data-source dump plus freeze/deform/remove timings at 10 k / 100 k / 1 M |
| `probe7_locators_and_reentry.cpp` | dirty-locator taxonomy; reading a frozen prim from a filtering index's **input**; whether a `usdGen:` relationship survives |
| `probe10_resync_blast.cpp` | resync blast radius over 200 sibling curve prims |
| `probe5_bake_size.py`, `probe6_bake_placement.py` | `.usdc` / `.usda` / session-layer sizes, authoring cost, landing place, stash-drop cost |
| `uv/makeStage.py`, `uv/testUsdviewFreezeCost.py` | a 115 / 2 205 / 11 005-prim host stage; one freeze in a live usdview with `_resetGUI` split by panel |

**4.3 Build and run.** None of the four C++ probes carries a build line; use the common form below
(it links every symbol all four need):

```sh
g++ -std=c++17 -O2 -Wno-deprecated -I$USD/include -I/usr/include/python3.12 \
    probe4_frozen_reentry_si.cpp -o probe4 \
    -L$USD/lib -Wl,-rpath,$USD/lib -lusd_tf -lusd_sdf -lusd_usd -lusd_usdGeom \
    -lusd_usdImaging -lusd_hd -lusd_arch -lusd_vt -lusd_gf -lusd_plug -lusd_work \
    -lusd_python -lpython3.12 -ltbb
N_CURVES=100000 CVS=8 $PY probe1_freeze_undo.py   # defaults are 10000/8; §4.4 quotes the 100 k column
DISPLAY=:77 FREEZE_N=100000 $USD/bin/testusdview --renderer GL \
    --testScript $PROTO/freeze-bake/uv/testUsdviewFreezeCost.py <hostStage>.usda
```

Library flags must follow the translation unit: this host's gcc passes `--as-needed` (MEASURED,
`gcc -dumpspecs` carries it three times), so a `-l` written before the source is silently dropped
and the link fails with undefined references. Of that line `probe2` additionally needs
`-lusd_execUsd`; `probe4`, `probe7` and `probe10` all need `-lusd_usdImaging -lusd_hd` — probe10
opens `UsdImagingCreateSceneIndices` too (`probe10_resync_blast.cpp:10-12`) — and only `probe2`
needs `-lusd_execUsd`. `probe3` and `probe9` additionally need
`. $PROTO/usdrig-linux-build/rigexec_env.sh` — after adding back its two missing exports (§11.2) —
and `PYTHONPATH=$RIG/tests/python:$PYTHONPATH`.

**4.4 What it proved** (`research/G-freeze-bake-undo-and-frozen-reentry.md` §1–§4).

* `SubtreeSnapshot` at 100 k × 8 CV: author 0.52 ms, capture **0.10 ms**, undo 0.24 ms, redo 0.09 ms,
  with **zero RSS growth** across capture — MEASURED (§1.3). The stash shares the `VtArray` buffers,
  so a deep undo stack of freezes is free and the ~1 ms `free()` moves off the interactive path.
  11/11 fidelity assertions pass (§1.2).
* `UsdStage::RemovePrim` with an `ExecUsdSystem` attached posts a coding error from stock OpenUSD
  (the `UsdPrimDefaultPredicate` call at `pxr/exec/esfUsd/stageData.cpp:361`, via
  `pxr/usd/usd/primFlags.cpp:21-25`) — MEASURED, reproduced
  with zero usdRig code. Hence S41: contain it, never rely on `RemovePrim` interactively, undo of a
  live freeze is `SetActive(false)`.
* The USD→Hydra half of a freeze is **flat in curve count**: edit→pulled 0.55/0.59/0.59 ms at
  10 k/100 k/1 M; `SetActive(false)` + apply 0.02 ms; `RemovePrim` + apply 0.06/1.47/**13.47 ms** —
  MEASURED (§4.1). Only deallocation scales.
* Blast radius: a new sibling freeze dirties one prim, but re-authoring the **parent scope's**
  `typeName` re-adds 203 — MEASURED (§4.2). That is ADR §2.2's rule that `Frozen/` is a dedicated
  sibling scope the tool never re-authors.
* usdview cost is a function of **stage prim count only**, ≈ 9–10 µs/prim: `_resetGUI` 4.3 ms at 115
  prims, 105 ms at 11 005 — MEASURED (§4.3). Hence one `Sdf.ChangeBlock` per batched freeze.
* Bake landing: session layer 0.4 ms; sidecar `.usdc` 10.6 ms / 9.6 MB; `.usda` 368 ms / 40 MB with a
  **660×** slower reopen — MEASURED (§3).

**4.5 Carried into.** `SubtreeSnapshot` is lifted verbatim into `usdgen/usdGenUndo.py` — the
lowercase `usdgen` package of ADR §9.1 R5, `10-build-dependencies-testing.md` §1.1
(required by M2's freeze work; the tool-side flows that consume it land in M5, ADR §7). `probe8` →
`tests/python/test_usdgen_undo.py` (the fidelity assertions of `08-tools.md` §9.1);
`probe4`/`probe7` → `tests/testUsdGenFrozenReentry.cpp` (M2, contract C3); `probe10` becomes the
blast-radius regression; `uv/testUsdviewFreezeCost.py` becomes gate **T-4** (T3, M5): freeze
*authoring* ≤ 5 ms on a 2 205-prim stage, and exactly one `_resetGUI` per batched freeze, with the
`_resetGUI` cost itself excluded from the budget (ADR §9 R40; it is 35.4 ms at 2 205 prims —
MEASURED, `research/G-freeze-bake-undo-and-frozen-reentry.md` §4.3). `probe5`/`probe6` document the freeze landing tiers
(`session | sublayer | payload`, ADR §9.1 R1) in `05-static-curves-and-deformation.md` §5.4.

**4.6 Caveats.** `probe0_api.py` and the generated `uv/groomHost*.usda` stages were not copied
(regenerate with `$PY uv/makeStage.py <n> <out.usda>`). `probe8` hard-codes a scratchpad path in
`sys.path.insert`. `probe3` and `probe9` are the only probes here needing a built usdRig. The usdview
**redraw** column (104–186 ms) is llvmpipe CPU work and must never be quoted as a Storm cost; the
authoring and `_resetGUI` columns are valid. Sculpt-layer rebase (ADR §2.3) has no probe at all — it is v2 / M8 work (ADR §9.5 R38).

**4.7 Status.** The Python probes run today; the four C++ probes compile with the line above.
Verified by re-reading the report's measurement tables against the sources.

---

## 5. `instancing`

**5.1 Purpose.** Settle S33 (usdGen emits real `instancer` prims at renderer level, hand-authored
`instancedBy`, prototypes as namespace children) and S34 (hair on natively instanced scalps;
propagated prototype names are hashes — discover, never construct).

**5.2 Files.**

| File | One line |
|---|---|
| `makeStage.py` | writes `probe.usda` + `probe_scalp.usda`: two natively instanced heads plus a `UsdGeomPointInstancer` with a card prototype |
| `instProbe.cpp` | what a downstream index sees for native and point instancing, and whether it can synthesize a well-formed `HdInstancer` |
| `instProbe2.cpp` | a generator inserted **upstream** of pi/ni propagation via `overridesSceneIndexCallback` |
| `instProbe3.cpp` | a **nested** instancer synthesized downstream, inside a propagated native prototype |
| `instProbe4.cpp` | the picking round-trip through `HdxPrimOriginInfo`, with and without a `primOrigin` data source |
| `out-probe1.txt` … `out-probe4.txt` | captured output |
| `probe.usda`, `probe_scalp.usda`, `README.md`, `CMakeLists.txt` | fixtures and build |

**5.3 Build and run.**

```sh
$PY $PROTO/instancing/makeStage.py $PROTO/instancing/probe.usda
cmake -S $PROTO/instancing -B $PROTO/instancing/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build $PROTO/instancing/build
for p in instProbe instProbe2 instProbe3 instProbe4; do
  $PROTO/instancing/build/$p $PROTO/instancing/probe.usda; done
```

All four run headless — `instProbe4` links `hdx`, but `HdxPrimOriginInfo` only walks data sources.

**5.4 What it proved** (`research/G-instancing-cards-archives-and-native-instances.md` §0–§8).

* A downstream-synthesized instancer with prototypes as **namespace children** and hand-authored
  `instancedBy` resolves through Hydra 1.0 emulation, including nested inside a propagated native
  prototype — MEASURED (`out-probe3.txt`: `hasInstancer=1`, `GetRprim(synth proto) = FOUND`, parent
  chain intact).
* Propagated prototype paths are hashes
  (`/UsdNiPropagatedPrototypes/NoPrimvars___usdUpAxisd827b8678191286/__Prototype_1/UsdNiInstancer/UsdNiPrototype`)
  — MEASURED. They must be discovered from `__usdPrimInfo.isNiPrototype` / `niPrototypePath`. That
  is S34.
* Without `primOrigin` a pick returns an empty instancer context; with it, `(/World/Groom/CardInstancer, 2)`
  — MEASURED (`out-probe4.txt` (a) vs (b)). That is S29's "`primOrigin{scenePath}` on every
  synthesized prim, else picking selects nothing".

**5.5 Carried into.** The four probes become `tests/testUsdGenInstancer.cpp` and
`tests/testUsdGenInstancerPick.cpp` (`10-build-dependencies-testing.md` §5.6) and gates **T-INST-1**
(instancer pick round-trip through `primOrigin`) and **T-INST-2** (prototype rebasing), both T1 at the
M6 exit (ADR §9.5 R40). `instProbe3`'s discovery walk is the template for the `UsdGenInstance` publisher's
native-instance path; `makeStage.py` becomes the M6 fixture builder.

**5.6 Caveats.** This is the only directory that lost nothing in the copy. But the prototype is a
single card mesh, not an archive **subtree**, so multi-prim archive prototypes are UNMEASURED;
per-instance primvar variation and S7's `InstanceDataSourceNames()` aggregation are exercised by
neither probe; and the Storm draw cost of an instanced card groom is UNMEASURED (no instancer stage
exists in §8's stage set).

**5.7 Status.** Builds and runs today; all four captured outputs are present and were re-read.

---

## 6. `stage-free-transport`

**6.1 Purpose.** Settle S8 (no `UsdStage` downstream of the stage scene index), S9 (codeless schemas),
S10 (the prim adapter and `UsdImagingDataSourceMapped`), S11 (ramps), S12 (the rest surface at
`UsdTimeCode::Default()`), S13 (asset reload) and S16 (OpenExec is not plugin-extensible in 26.08).

**6.2 Files.**

| File | One line |
|---|---|
| `plugin/usdGenProbeSchema/resources/plugInfo.json` | a codeless schema plugin, `"Type": "resource"`, `Root`/`ResourcePath` `"."` — the usdRig pattern |
| `plugin/usdGenProbeSchema/resources/generatedSchema.usda` | `UsdGenProbeOperator` (bases `UsdTyped`), `UsdGenProbeClumpStyler` (derived), `UsdGenProbeImageableOp` (bases `UsdGeomImageable`) |
| `probe.cpp` → `probe-out.txt` | codeless registration, terminal dump, per-type data-source values, `DataSourceMapped`, the REST factory |
| `probe2.cpp` → `probe2-out.txt` | notices for schema-attr vs primvar edits, prim add/retype, relationship primvars, `SetStage` cycle |
| `probe3.cpp` | the same after a **deep** pull: lazy time-varying / asset registration, `ArNotice::ResolverChanged` |
| `probe4.cpp` | is `UsdExecImagingCreateStageSceneIndex()` constructible, and what does it expose |
| `probe5.cpp` | does a custom prim-level container survive the whole chain, including native-instance propagation |
| `probe6.cpp` | a whole `TsSpline` carried as `HdTypedSampledDataSource<TsSpline>` and evaluated as a ramp |
| `scene.usda`, `scene_inst.usda` | fixtures with `.spline`, time samples, a UDIM asset, a rest primvar, native instances |

**6.3 Build and run.**

```sh
cmake -S $PROTO/stage-free-transport -B $PROTO/stage-free-transport/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release
cmake --build $PROTO/stage-free-transport/build && cd $PROTO/stage-free-transport
PXR_PLUGINPATH_NAME=$PWD/plugin/usdGenProbeSchema/resources ./build/probe  scene.usda
PXR_PLUGINPATH_NAME=$PWD/plugin/usdGenProbeSchema/resources ./build/probe5 scene_inst.usda
```

Run from the probe directory: relative asset paths resolve against the scene's layer.

**6.4 What it proved** (`research/G-stage-free-parameter-and-time-transport.md` §0–§6).

* A codeless schema needs **zero C++**: `plugInfo.json` + `generatedSchema.usda` gives a working
  `IsA`, derived-type chain and `UsdPrimDefinition::GetPropertyNames()` — MEASURED (§1). That is S9,
  and it is the call ADR §2.3 builds its adapter mappings from generically.
* Every value type travels as a data source, including `asset`, `asset[]`, `float2[]`, relationships
  and a whole `TsSpline` — MEASURED (§2, probe1/probe6).
* A `.spline` on a plain attribute is sampled at scene time and **flagged time-varying**, so it is
  dirtied on every `SetTime` — MEASURED (§2.1). Hence ADR §1's S11 amendment: no `.spline` on a ramp
  property; the adapter-factory `TsSpline` transport never flags time-varying.
* A rest surface is reachable three ways, all verified — authored `primvars:rest`, a custom
  `AttributeMapping::factory` at `UsdTimeCode::Default()`, or capture-on-first-cook (rejected by S12)
  — MEASURED (§3).
* A custom container published by an adapter survives the entire chain including native-instance
  propagation — MEASURED (§4, probe5). That is the `usdGen`/`usdGen:rest` container of ADR §2.3.
* The ExecUsd control plane is off by default with a hard-coded adapter list, so it is not
  plugin-extensible in 26.08 — MEASURED (§6, probe4); that is S16.

**6.5 Carried into.** `plugin/usdGenProbeSchema/` is the literal template for `usdGenSchema` in M0,
renamed and with `plugInfo.json` rewritten to `"Type": "library"`, `"LibraryPath":
"../../../libusdGenSchema.so"` and `implementsComputeExtent: true` on `UsdGenDescription`
(ADR §9 R19). `libusdGenSchema.so` is a minimal library carrying the schema tokens and the
`UsdGeomRegisterComputeExtentFunction` call, linking `usd`/`usdGeom` only, so extents work in
`usdcat`/`usdrecord` without loading imaging; the schema classes stay codeless. usdRig points its
schema plugInfo at its *imaging* library instead (`research/B-usdrig-build.md` §7) — copy the
plugInfo shape, not that half of the pattern, which would drag `usd` into the `usdGen` core
(ADR §9 R37). `probe.cpp`, `probe5.cpp` and `probe6.cpp` become `tests/testUsdGenAdapter.cpp`, the harness for gate
**SI-7** — every **declared** property of every registered type appears and dirties, the mappings
built from `UsdPrimDefinition::GetPropertyNames()` with **no prefix filter** (ADR §9.2 R6) — at the
M1 exit, contract C1.

**6.6 Caveats.** `maps/*.exr` (empty placeholders that make the asset-path tests resolve) and
`cmake.log` were not copied — create exactly **two** zero-byte files, `maps/density.exr` and
`maps/clump_1001.exr`. **Do not create `maps/does_not_exist.exr`**: probe1 asserts that an
unresolvable `asset[]` element yields an empty resolved path (`probe-out.txt:127`), and
`maps/clump_<UDIM>.exr` resolves to an absolute path with the `<UDIM>` token carried through
verbatim rather than expanded (`probe-out.txt:68`) — tile expansion is the consumer's job, by
design. The probe registers **no imaging adapter**: probe1/probe2 show what is
*invisible* without one, so nothing here exercises
`UsdImagingSceneIndexPrimAdapter` or `UsdImagingDataSourceMapped::Invalidate` and SI-7 needs new code
(§13, PW-7). Auto-applied API schemas on a codeless type (gate SI-8) are not exercised: the generated
schema declares none. The type names are deliberately `UsdGenProbe*`, distinct from the ADR §2.1
vocabulary, so nothing here can be copied into a shipping `schema.usda` without renaming.

**6.7 Status.** Builds and runs today; `probe-out.txt` and `probe2-out.txt` are present and were
re-read. `probe3`–`probe6` have no captured output in the copy.

---

## 7. `storm-hair-look`

**7.1 Purpose.** Settle S35 (the plugin glslfx is the v1 Storm look) and S36 (the three-terminal
material) — and, the finding that changed what the whole plan can verify, prove Storm runs headless
on the GB10 through an EGL device-platform context.

**7.2 Files.**

| File | One line |
|---|---|
| `eglctx.h` | 78 lines (MEASURED, `wc -l`): a headless GL 4.6 compatibility context on an EGL device with a 64×64 pbuffer (`MakeHeadlessGLContext(major=4, minor=5, profileBit=0x2)`, `eglctx.h:16`), entry points `dlopen`ed from `libEGL.so.1` (no EGL headers here) |
| `usdGenHairPreview.glslfx` | the deliverable: Kajiya-Kay diffuse + Marschner-lite R/TRT lobes + TT rim, root→tip colour via `hairT`, tangent via `hairTangent` with a screen-derivative fallback, per-curve jitter via `hairId`, scalp map via `st`; 20 `inputs:`, 4 attributes, `defaultMaterialTag` |
| `usdGenHairPreview_translucent.glslfx` | the same shader with the `translucent` tag for OIT on fine fur |
| `render_hair.cpp` | 39 lines (MEASURED, `wc -l`) around `UsdAppUtilsFrameRecorder` — the replacement for `usdrecord` on a headless host |
| `bench_hair.cpp` | `UsdImagingGLEngine` + `color` AOV, 8 warm-up frames, `glFinish()` inside the timed region, optional per-frame `points` re-author |
| `make_scene.py`, `make_scene_preview.py`, `make_scene_mtlx2.py` | the 4 000-curve scalp scene, the UsdPreviewSurface variant, the MaterialX variant with an explicit `hairTangent` geomprop |
| `sdr_parse_test.py` | `Sdr.Registry().GetShaderNodeFromAsset` on the glslfx, plus a `Hio.Glslfx` round trip |
| `hair_scene*.usda`, `hair_*.png` | three fixtures and three reference renders |
| `CMakeLists.txt` | `render_hair`, `bench_hair` |

**7.3 Build and run.**

```sh
cmake -S $PROTO/storm-hair-look -B $PROTO/storm-hair-look/build -G Ninja \
      -DCMAKE_PREFIX_PATH=$USD -DCMAKE_BUILD_TYPE=Release
cmake --build $PROTO/storm-hair-look/build && cd $PROTO/storm-hair-look
HAIR_CAMLIGHT=1 ./build/render_hair hair_scene.usda out.png /World/Cam 800
./build/bench_hair hair_scene.usda /World/Cam 1280 720 1.2 60 0      # static, refineLevel 2
./build/bench_hair hair_scene.usda /World/Cam 1280 720 1.2 40 1      # deforming
$PY sdr_parse_test.py
TF_DEBUG=HDST_DUMP_SHADER_SOURCEFILE ./build/render_hair hair_scene.usda out.png /World/Cam 800
```

No `DISPLAY`, no Xvfb. Complexity maps to refineLevel 1.0→0, 1.1→1, **1.2→2**, 1.3→3.

**7.4 What it proved** (`research/G-storm-hair-look-prototype.md` §0, §2, §5).

* Storm renders headlessly at GL 4.6 compat with **zero GL errors and zero warnings** — MEASURED
  (§0, §2.2). The glslfx parses through `SdrGlslfxParserPlugin`, compiles and renders (§2.1).
* Frame times at 1280×720, refineLevel 2: 4 k curves **0.85 ms**, 40 k **5.01 ms**, 200 k
  **23.93 ms (42 fps)** — MEASURED (§5). Deforming adds +0.22 / +0.83 / **+2.46 ms**, i.e. 0.13 ms
  per MB of points at 200 k.
* **refineLevel 0 is not the fast mode**: at 200 k it costs 12.43 ms against 8.17 ms at level 1,
  because level 0 draws a `LineList` through every CV — MEASURED. Hence S31 (interaction LOD is curve
  decimation) and ADR §1's S31 amendment making the level-1 tumble tier gate **S-9** rather than an
  assumption.
* MaterialX `chiang_hair_bsdf` compiles in Storm and renders near-black; UsdPreviewSurface with a
  uniform `st` does a correct per-curve root-UV lookup — MEASURED (§3, §4). That is S36's split.

**7.5 Carried into.** `eglctx.h` becomes `usdGenTestUtilsHd/eglctx.h` in **M0**
(`10-build-dependencies-testing.md` §1.2 — it links `hgiGL`/`usdImagingGL`, so it belongs to the Hydra
half of the test utilities, never to the T0-only `usdGenTestUtils`) — it is the tier-T2
harness every S-gate depends on. `usdGenShaders` ships exactly **three** glslfx files —
`usdGenHairPreview.glslfx` (variant A), `usdGenHairPreviewPrimvar.glslfx` (variant B) and
`usdGenHairPreviewTranslucent.glslfx` (`10-build-dependencies-testing.md` §1.2) — with shader
defs `UsdGenHairPreview`, `UsdGenHairPreviewTranslucent` and `UsdGenHairPreviewPrimvar` (prim names,
so no `:` in them, ADR §9.1 R4), all sharing one 20-entry `inputs:` block. Both copied files are ADR §5.4 **variant B** (tangent from
the `hairTangent` primvar with a screen-derivative fallback; the file's own header refuses to read
`inData.Neye`): `usdGenHairPreview.glslfx` becomes `usdGenHairPreviewPrimvar.glslfx`, and
`usdGenHairPreview_translucent.glslfx` becomes `usdGenHairPreviewTranslucent.glslfx` (shader def
`UsdGenHairPreviewTranslucent`) once its tangent block is switched to variant A. **Variant A** — tangent from `inData.Neye`, the intended default on usdGen
tiles (ADR §5.4) — does not exist yet and is written as part of PW-2 (§13); gate **S-8** (M0 pre-work, deciding M1)
picks which ships as the default. That shared `inputs:` block is contract **C5**, frozen at the M1
exit. `bench_hair.cpp` becomes **`benchUsdGenStorm`** (`10-…` §5.6; the harness of
`09-performance-and-benchmarks.md` §4.1) behind gates **S-1**, **S-2**, **S-3**, **S-12** and, through
`testUsdGenStormTangent` and `testUsdGenStormRefine`, **S-8** and **S-9**; `render_hair.cpp` becomes
the golden-image driver `testUsdGenStormLook` (`10-…` §5.6), which is gate **L-2** (T2, M1).

**7.6 Caveats.** Three PNGs and three scenes survive of roughly 12 and 8 in the scratchpad
(ASSUMPTION — the originals are gone; the three of each that survive are MEASURED by `ls`), and none
of the four `dump*/` GLSL directories were copied; `eglprobe.c`, `make_scene_mtlx.py`,
`make_scene_40000.py`, `make_scene_200000.py` and the two `probe_*.glslfx` files are gone — and `make_scene_mtlx2.py` edits the uncopied
`hair_scene_mtlx.usda`, so the MaterialX variant cannot be regenerated as-is. Measured counts are
4 k / 40 k / 200 k, so the plan's headline **100 k** Storm draw (≈ **12.2 ms**) is **DERIVED**
(interpolated between the 40 k and 200 k rows), never MEASURED — `09-performance-and-benchmarks.md`
§0.2 is the only frame ledger that publishes it (ADR §9.5 R41, R42) and gate **S-1** re-measures it at
100 k in **both 720p and 1080p** (09 §5.3; ADR §9.5 R40). The deform numbers came from `pointsAttr.Set()` + `Render()`, not a
scene index publishing `primvars/points`; that path is UNMEASURED. All numbers are one prim, the best
case for batching. GB10 + driver 580.173.02 only, and
`HDST_ENABLE_HGI_RESOURCE_GENERATION=1` (`pxr/imaging/hdSt/codeGen.cpp:166`), the Metal/Vulkan proxy
for gate R-3, has not been run against this shader.

**7.7 Status.** Builds and runs today. Verified by re-reading `eglctx.h`, the glslfx configuration
block (19 `parameters` + 1 `textures` entry = the 20 Sdr `inputs:` of contract C5, 4 `attributes`,
`materialTag: defaultMaterialTag`) and the frame-time table.

---

## 8. `storm-throughput`

**8.1 Purpose.** Settle S27 (32–256 `basisCurves` prims per description), S28 (exact-size arrays),
S29 (the curve contract) and S30 (the invalidation discipline), and supply the workstation protocol
for what needs GPU counters.

**8.2 Files.**

| File | One line |
|---|---|
| `probe_curves_cpu.cpp` | `HdStBasisCurves` primvar/topology cost: exact-size vs padded vs `curveIndices`, varying expansion, index rebuild, topology hash, memcpy reference |
| `probe_locators_cpu.cpp` | `ComputeDirtyLocators` isolated from `SdfPath` construction; per-frame entry build with cached paths |
| `probe_sceneindex_cpu.cpp` | per-prim `ComputeDirtyLocators` and `HdDependencyForwardingSceneIndex` fan-out for N curve prims onto one mesh |
| `probe_primvardesc_cpu.cpp` | the `GetPrim` fastpath through K filtering indices, and the primvar-descriptor cache clear |
| `probe_vtalloc_cpu.cpp` | the thrown-away `VtArray` allocation in the exact-size case; the 512 KiB staging threshold |
| `probe_chunkparallel_cpu.cpp` | 100 k × 8 CV as 1 / 8 / 32 / 128 / 1000 buffer sources, points and topology |
| `hairbench.cpp` | the counter-accurate Storm harness (`drawCalls`, `drawBatches`, `vboRelocated`, `copyBufferCpuToGpu`, `gpuMemoryUsed`) — there is no `pxr.Hd` Python module in 26.08 |
| `gen_hair_stages.py` | the eight benchmark stages (1 / 32 / 1000 prims, linear, animated, three density levels) with identical curve data throughout |
| `bench_usdview.py` | the `testusdview --testScript` driver: static / deform / edit-one / edit-all / displayColor / density scrub |
| `run_bench.sh` | sequences all six passes (wall clock, counters, batch structure, per-prim sync, trace, culling A/B) |
| `PROBE_OUTPUT.txt`, `CMakeLists.txt` | captured CPU output; the `hairbench` target |

**8.3 Build and run.** The six `probe_*_cpu` binaries are direct `g++` builds and include two
**private** hdSt headers:

```sh
mkdir -p $PROTO/storm-throughput/priv_include/pxr/imaging/hdSt
cp $USDSRC/pxr/imaging/hdSt/basisCurvesComputations.h \
   $USDSRC/pxr/imaging/hdSt/basisCurvesTopology.h \
   $PROTO/storm-throughput/priv_include/pxr/imaging/hdSt/
cd $PROTO/storm-throughput
for p in probe_curves_cpu probe_locators_cpu probe_sceneindex_cpu \
         probe_primvardesc_cpu probe_vtalloc_cpu probe_chunkparallel_cpu; do
  g++ -std=c++17 -O2 -I priv_include -I$USD/include $p.cpp -o $p \
      -L$USD/lib -Wl,-rpath,$USD/lib \
      -lusd_hd -lusd_hdSt -lusd_tf -lusd_vt -lusd_gf -lusd_sdf -lusd_work -lusd_trace -ltbb \
      && ./$p; done
$PY gen_hair_stages.py --out ./stages --curves 100000 --cv 8 --frames 24
cmake -S . -B build -DCMAKE_PREFIX_PATH=$USD && cmake --build build
HD_ENABLE_PERFLOG=1 ./build/hairbench stages/hair_32chunks.usdc 2   # needs a display today, §8.6
DISPLAY=:77 USD=$USD OUT=/tmp/hairbench ./run_bench.sh             # llvmpipe: CPU numbers only
```

**8.4 What it proved** (`research/G-storm-throughput-and-prim-granularity.md` §1, §3).

* Exact-size `points` shares the input buffer and resolves in 0.22 ms; a **longer** array falls back
  to red (`fallback=1`, the `std::fill(_fallbackValue)` at
  `pxr/imaging/hdSt/basisCurvesComputations.h:223`), and a `curveIndices`-padded array — the
  `HasIndices()` branch at `:210-220` — costs 3.50 ms (MEASURED,
  `prototypes/storm-throughput/PROBE_OUTPUT.txt` probe C; the report's run gives 3.46 ms, §1.4). That
  is S28, and the reason neither padding nor wider chunks is in the design.
* `HdContainerDataSourceEditor::ComputeDirtyLocators` emits `primvars/__containerDataSource`, which
  **clears** the primvar-descriptor cache; a bare leaf set does not — MEASURED (probe N). That is S30
  and ADR §5.2: bare locators where usdGen owns the prim, `ComputeDirtyLocators` plus the bare
  `primvars` locator only where it overlays an upstream prim.
* Per-prim invalidation build: 0.008 µs/prim with a cached locator set at N ≤ 10 000 (0.145 µs at
  N=100 000) vs 0.11 µs recomputed (0.249 µs at N=100 000); the full `ComputeDirtyLocators` entry
  build is **0.90–1.77 µs/prim** (1.77 at N=1 000, 1.16 at N=10 000, 0.90 at N=100 000), and
  0.66 µs/prim when the locator set is cached at N=100 000 — MEASURED (`PROBE_OUTPUT.txt` probe I and
  `probe_locators_cpu`).
* `HdDependencyForwardingSceneIndex` fan-out is 0.17–0.65 µs per affected prim in this copy's run
  (`PROBE_OUTPUT.txt` probe J; the report's run peaks at **1.30 µs** at N=100 000, §1.8), but lazy
  dep population costs 496 ms at 100 000 prims — MEASURED. Hence `__dependencies` per tile
  (32–256 edges), never per curve.
* `GetPrim` through 0/1/3/6/10 filtering indices is flat at 0.18–0.19 µs — MEASURED; filtering depth
  is free, which is what lets the operator graph be many small indices.
* Chunk parallelism: points resolve 3.20× faster at 32 chunks and 4.79× at 128, then regresses at
  1000; topology index build keeps improving to 3.38× at 1000 — MEASURED. With the 200 k frame time
  this is the arithmetic behind ADR §1's chunk≠tile split: `chunksPerTile = max(1, ceil(nChunks /
  tileTarget))`, `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))`
  (ADR §9 R21). At 100 k curves, chunk 512, `tileTarget` 64: 196 chunks → 4 chunks/tile → **49
  tiles**, which is the prim count gates S-3/S-5 assert against.
* Cubic index-buffer rebuild at 100 k curves: 5.04 ms cold — MEASURED (`PROBE_OUTPUT.txt` probe F).
  The report separates the same work into **2.6 ms warm / 5.2 ms cold** (§1.6) and ADR §5.5 states
  gate S-9's CPU half as 2.6/5.2 ms per 500 k patches — quote the ADR pair. Staging threshold 512 KiB
  (`pxr/imaging/hdSt/stagingBuffer.cpp:73`) = 5 461 curves at 8 CV (derived: 512 KiB ÷ 12 B per
  `GfVec3f` ÷ 8 CV).

**8.5 Carried into.** `gen_hair_stages.py` becomes the generator of the four canonical grooms G1–G4
(`09-performance-and-benchmarks.md` §4.4); `hairbench.cpp` and `run_bench.sh` fold into
**`benchUsdGenStorm`** and the workstation protocol; `bench_usdview.py` becomes the tier-T3 driver;
all of it lands in M1 under `tests/perf/`. The six CPU probes become tier-T1 regressions for the tile
publisher and gate **SI-2**. The tile contract these probes pin down is contract **C2** (ADR §3, §5.3),
frozen at the M1 exit. §3.5's B1–B12 thresholds are the source of gates **S-3** (one-tile edit ≤ 1.2×
static, M2), **S-4** (`itemsDrawn` drops ≥ 3×, M2), **S-5** (`drawBatches == 1`, M1), **S-6**
(`vboRelocated == 0`, M1), **S-7** (density scrub ≤ 1.5×, M5) and **S-12** (the 1 / 32 / 128 / 196-tile
prim-count sweep, M1, before C2 freezes) — milestones per ADR §9.5 R40 and 09 §5.3. The publisher's
exact-size assertion is **SI-1**.

**8.6 Caveats.** `priv_include/` and the generated `stages/*.usdc` were not copied; §8.3 restores
both. Copying private headers is a prototype-only expedient — shipped `tests/perf/` must assert on
public `HdBasisCurvesTopology` instead. `hairbench.cpp` creates a `GlfTestGLContext` and therefore
still needs a display: it predates the EGL finding of §7, and the one-line M0 fix (include `eglctx.h`,
call `MakeHeadlessGLContext()` first — §13's **PW-12**) moves gates S-4, S-5 and S-6 from tier T4 to
tier T2 on this host.
`run_bench.sh` still defaults `USD=/path/to/OpenUSD_install` and claims Storm cannot run headless —
both stale. Every number in `PROBE_OUTPUT.txt` is CPU; §7 owns the GPU numbers.

**8.7 Status.** The six CPU probes build and run today once `priv_include/` is restored; `hairbench`
builds and core-dumps without a display. Verified by re-reading `PROBE_OUTPUT.txt`.

---

## 9. `thirdparty-bench`

**9.1 Purpose.** Settle S38: which SeExpr and Ptex versions, what the embedding API costs, and whether
both are safe from TBB workers.

**9.2 Files.**

| File | One line |
|---|---|
| `se_min.cpp` | the minimum viable embedding: an `Expression` subclass, an `ExprVarRef`, parse-error reporting, construct/eval/destroy |
| `seexpr_bench.cpp` | the a host groomer-shaped harness: `$u $v $id $P` custom vars, a custom `map()` `ExprFuncSimple`, 8-thread evaluation with one `VarBlock` per worker |
| `ptex_test.cpp` | writes a quad `.ptx` (per-face resolutions, adjacency, mipmaps) and a triangle `.ptx`, reads back through `PtexCache`, filters across the shared edge, times 8-thread lookups |

**9.3 Build and run.** No CMake here; both libraries are built first (verified commands,
`research/A8-seexpr-ptex-libs.md` §1.2, §2.2):

```sh
TP=<thirdparty root>
git clone https://github.com/wdas/SeExpr $TP/seexpr && git -C $TP/seexpr checkout 8f8c8f2
git clone https://github.com/wdas/ptex   $TP/ptex   && git -C $TP/ptex   checkout v2.4.3
cmake -S $TP/seexpr -B $TP/build-seexpr -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LLVM_BACKEND=OFF -DENABLE_QT5=OFF -DENABLE_SSE4=OFF -DUSE_PYTHON=OFF \
  -DBUILD_UTILS=OFF -DBUILD_DEMOS=OFF -DBUILD_DOC=OFF -DBUILD_TESTS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=$TP/install
cmake -S $TP/ptex -B $TP/build-ptex -DCMAKE_BUILD_TYPE=Release \
  -DPTEX_BUILD_DOCS=OFF -DCMAKE_CXX_STANDARD=17 -DCMAKE_INSTALL_PREFIX=$TP/install
cmake --build $TP/build-seexpr -j && cmake --install $TP/build-seexpr
cmake --build $TP/build-ptex   -j && cmake --install $TP/build-ptex
g++ -O2 -std=c++17 -I$TP/install/include -L$TP/install/lib -Wl,-rpath,$TP/install/lib \
    seexpr_bench.cpp -o seexpr_bench -lSeExpr2 -lpthread && ./seexpr_bench
g++ -O2 -std=c++17 -I$TP/install/include -L$TP/install/lib -Wl,-rpath,$TP/install/lib \
    ptex_test.cpp -o ptex_test -lPtex -lz -lpthread && ./ptex_test
```

`-DENABLE_SSE4=OFF` is **mandatory on aarch64** (SeExpr appends `-msse4.1` unconditionally when it is
on, `CMakeLists.txt:201-202`); bison, flex and sed are effectively required, since the repo ships no
pre-generated parser.

**9.4 What it proved** (`research/A8-seexpr-ptex-libs.md` §1.6, §2.8, Key facts).

* SeExpr interpreter, one thread: `$u*$v+1` **13 ns/eval**, a noise+fbm+fit expression **117 ns**, a
  custom `map()` expression **34 ns** — MEASURED. Eight threads with one `VarBlock` each: 159
  ns/eval/thread → **50 M evals/s aggregate**.
* Ptex bilinear filter **23 ns/lookup** single-threaded, `f_box` 26 ns, **228 M lookups/s** across 8
  threads with a per-thread `PtexSeparableFilter` and a shared cache — MEASURED.
* Both export usable CMake packages (`seexpr2::SeExpr2`; `Ptex::Ptex_static` / `Ptex::Ptex_dynamic`);
  Ptex's target names are the ones OpenUSD itself expects
  (`pxr/imaging/hdSt/CMakeLists.txt:35-38`).
* `ExprFunc::define` is not thread-safe and must run before any parallel prep; one
  `PtexSeparableFilter` per worker is required (scratch state in `PtexSeparableFilter.h:80-81`).

**9.5 Carried into.** `seexpr_bench.cpp`'s `GroomExpr` / `SimpleVar` / `MapFuncX` shapes become the
`UsdGenExprMap` evaluator inside `usdGen`, with the benchmark as the `benchUsdGenMaps` /
`testUsdGenExpr` half of gates **L-3** (determinism, T0) and **L-4** (capture cost at 1 M roots,
T0/T1), both at M4 (`09-performance-and-benchmarks.md` §5.4; ADR §9.5 R40);
`ptex_test.cpp` becomes the `usdGen_ptex` round-trip test (`testUsdGenPtex`) including the
`PtexWriter` half, which is the paint round-trip of S37. Both land in **M4**. Both statics build `-fvisibility=hidden` and hide
inside `libusdGen.so`; their headers and shared objects are never installed next to USD's (A8 §6.2).

**9.6 Caveats.** There is **no CMake and no vendoring scaffolding** here — no `FetchContent`, no
`FETCHCONTENT_SOURCE_DIR_*` offline fallback, no hidden-visibility wrapper; M0 writes those. The
`rand()` builtin ADR §6 requires is not implemented in any prototype, and SeExpr's 0..1 `noise` vs
signed `snoise` distinction is not encoded. This install has `PXR_ENABLE_PTEX_SUPPORT` OFF, so nothing
tests Ptex *inside* Storm — consistent with S37, which keeps Ptex CPU-only at capture. Only the two
clone hashes are durable; the scratchpad install prefix is not. ADR §9 R38 settles the catalogue
question: `UsdGenPtexMap` is **v1, milestone M4** (R38 amends ADR §6's v2 listing), so both
`usdGen_seexpr` and `usdGen_ptex` deliverables land in M4 and there is no open discrepancy for
`04-operators.md` or `11-roadmap.md`. **No captured output was copied** — the numbers
in §9.4 live only in `research/A8-seexpr-ptex-libs.md` §1.6/§2.8, so re-running the two benchmarks is
the only way to reproduce them.

**9.7 Status.** Both benchmarks built and ran during the research round; they build today once the two
libraries are rebuilt, because the install prefix was scratchpad-local. Verified by re-reading the
three sources against the report's tables; no captured output exists in this copy.

---

## 10. `tool-loop`

**10.1 Purpose.** Settle S39 (two Python surfaces: a ctypes C ABI for control, a `pxr_boost.python`
module for arrays) and S40 (live override during the drag, CPU CV picking, CV display as a synthesized
`points` child prim).

**10.2 Files.**

| File | One line |
|---|---|
| `cTransport.cpp` | the C ABI in usdRig's `registry.h` shape: `Resize`, `SetLiveOverride`, `SetLiveOverrideIndexed`, `ReadCurvesPtr`, `ReadCurvesCopy`, `PickCV`, `FootprintCV` — raw `float*`, no USD types in the ABI |
| `bpTransport.cpp` | a `PXR_BOOST_PYTHON_MODULE` taking and returning `VtVec3fArray` with USD's own converters |
| `pbTransport.cpp` | the pybind11 comparison, incl. the pybind11 → `pxr_boost::python` hybrid extracting a `VtVec3fArray&` from a `py::object` |
| `bench_transport.py` | per-call cost of every viable route, push and pull, at 100 k / 800 k float / 1 M |
| `bench_stroke.py` | one stroke end to end: press (zero-copy read), 60 moves (kernel on the footprint + sparse push), release (one `attr.Set`) |
| `bench_pick_cpu.py`, `bench_pick_cpp.py` | numpy vs C++ screen-space nearest-CV and footprint picking at 1920×1080 |
| `probePrimvarAppear2.cpp` | which dirty notice makes a **new** primvar visible to `HdSceneIndexAdapterSceneDelegate` when the data source flips with no `PrimsAdded` |

**10.3 Build and run.**

```sh
cd $PROTO/tool-loop
g++ -O3 -std=c++17 -fPIC -shared -o libUsdGenTransportC.so cTransport.cpp
g++ -O2 -std=c++17 -fPIC -shared -o _usdgenTransport.so bpTransport.cpp \
    -I$USD/include -I/usr/include/python3.12 -L$USD/lib -Wl,-rpath,$USD/lib \
    -lusd_vt -lusd_gf -lusd_tf -lusd_arch -lusd_boost -lpython3.12
g++ -O2 -std=c++17 -fPIC -shared -o _usdgenPb.so pbTransport.cpp \
    -I$USD/include -I/usr/include/python3.12 \
    $($PY -c 'import pybind11;print(pybind11.get_include())' | sed 's/^/-I/') \
    -L$USD/lib -Wl,-rpath,$USD/lib -lusd_vt -lusd_gf -lusd_tf -lusd_arch -lusd_boost -lpython3.12
g++ -O2 -std=c++17 -I$USD/include probePrimvarAppear2.cpp -o probePrimvarAppear2 \
    -L$USD/lib -Wl,-rpath,$USD/lib -lusd_hd -lusd_hf \
    -lusd_sdf -lusd_tf -lusd_vt -lusd_gf -lusd_trace -lusd_python && ./probePrimvarAppear2
export PYTHONPATH=$USD/lib/python3.12/site-packages:.
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1   # pin BLAS, see below
REPS=101 $PY bench_transport.py 100000
$PY bench_stroke.py 100000 2000
$PY bench_pick_cpu.py 100000
$PY bench_pick_cpp.py 100000
```

Library flags must follow the translation unit here (§4.3: this host's gcc passes `--as-needed`).
`_usdgenPb.so` is required to *run* the two array benchmarks even though pybind11 never enters the
usdGen build (§10.6): `bench_transport.py:13` and `bench_stroke.py:14` both `import _usdgenPb as PB`
with no `try`/`except`, and `pbTransport.cpp:96` is the `PYBIND11_MODULE(_usdgenPb, m)` that
satisfies them. Pin BLAS threads before importing numpy — no benchmark script here pins anything
itself: at a host load of 43–59 an unpinned numpy inflated every BLAS-touching number by
10–20×, which is itself a finding
(`research/G-tool-loop-array-transport-and-cv-picking.md`, preamble and §1.5).

**10.4 What it proved** (`research/G-tool-loop-array-transport-and-cv-picking.md` §1.2, §2.4, §5).

* `VtArray` across the `pxr_boost.python` boundary is **O(1)**: 0.13–0.18 µs at 100 k, 800 k float and
  1 M — MEASURED. A ctypes memcpy of the same data is 13.7–341 µs; a list-of-lists round trip is
  9 304–164 350 µs.
* `Vt.Vec3fArray.FromBuffer` costs 420–4 304 µs for float32 and 453–5 038 µs for float64, and
  `attr.Set(numpy)` 480–4 893 µs — MEASURED; all three are forbidden on groom-sized arrays (S39). `attr.Set(Vt.Vec3fArray)` is 1.63–1.82 µs.
* `np.asarray(Vt.Vec3fArray)` is a zero-copy read-only view at 0.19 µs — MEASURED; that is the brush
  kernel's input. A sparse footprint push of 2 000 of 100 k CVs is 1.17 µs via `pxr_boost`.
* CPU CV picking in C++ over ctypes: **166 µs at 100 k CVs**, 1.66 ms at 1 M, footprint 590 µs —
  MEASURED, ~8× cheaper than one Hydra `view.pick()`.
* Making a **new** primvar visible with no `PrimsAdded` requires dirtying `primvars/<name>` once;
  per-move edits need only `primvars/<name>/primvarValue` — MEASURED (§5). That is S30's live-paint
  rule.

**10.5 Carried into.** `cTransport.cpp` is the template for the `extern "C"` ABI inside
`usdGenImaging`. The entry-point list is contract **C4** and lives in `08-tools.md` §1.4, the single
source (ADR §9 R31); this appendix does not restate it. The prototype implements only the array and
pick subset — `UsdGenImaging_{Resize, SetLiveOverride, SetLiveOverrideIndexed, ReadCurvesPtr,
ReadCurvesCopy, PickCV, FootprintCV, GetGeneration}` (`cTransport.cpp:21-124`) — and its
`UsdGenImaging_FootprintCV` (`cTransport.cpp:124`) is the shipped `UsdGenImaging_Footprint` under its
prototype name. Everything R31 adds beyond that subset (`Activate`, `Deactivate`, `SetTime`,
`Commit`, `SetContext`, `GetTopologyGeneration`, `BeginLiveOverride`, `ClearLiveOverride`,
`ClosestSurfacePoint`, `SetInteractiveLOD`, `BuildMirrorMap`, `SetMaskVisualisation`, `GetStatsJson`,
`ReloadMaps`) has no prototype and is written in M5. `bpTransport.cpp` is the template for
the `_usdGen` pxr_boost module (`ReadCurvePoints/Counts/Widths`, `SetLiveOverride`,
`SetLiveOverrideIndexed`), built **without LTO**
(ADR §6). `bench_stroke.py` and `bench_pick_cpp.py` become the harnesses behind gates **T-1** (≤ 1 ms of
Python plus engine per brush move, tier T3) and **T-2** (`UsdGenImaging_PickCV` ≤ 0.25 ms at 100 k and
≤ 2.5 ms at 1 M, tier T1, `testUsdGenPick`), both at the M5 exit
(`09-performance-and-benchmarks.md` §5.4; ADR §9.5 R40).
`probePrimvarAppear2.cpp` becomes a tier-T1 invalidation test. All of it lands in **M5**; the ABI is
contract **C4**, frozen at the M5 exit.

**10.6 Caveats.** There is **no CMakeLists.txt** — only the `g++` lines above, reconstructed from the
report appendix; none of the ten `bench_*.log` captures, the built `.so`s, `frozen.usdc` or the
earlier `probePrimvarAppear.cpp` were copied. `pbTransport.cpp` is a comparison, not a deliverable:
the ADR chooses `pxr_boost.python`, so pybind11 never enters the usdGen build — though `_usdgenPb.so`
is still needed to re-run the two array benchmarks (§10.3). The CPU pick gives no
occlusion — depth-buffer rejection is UNMEASURED, and pick accuracy against a real `view.pick()` is
gate **T-3**, still unrun. The stores in all three transports are `std::unordered_map` toys, so
nothing here exercises the atomic publish of ADR §4.3.

**10.7 Status.** All three shared objects and the probe built and ran during the research round and
build today with the lines above; the Python benchmarks then run unchanged. Verified by re-reading the
eight sources against the report's tables; no captured output exists in this copy (§10.6).

---

## 11. `usdrig-linux-build`

**11.1 Purpose.** Prove S44 (a sibling CMake project against the unmodified install with an optional
`find_package(rigExec)`), S45 (the `-ffp-contract=off` requirement and the harness tiers) and S46 (the
bugs to contain), by building usdRig out of tree and linking a consumer against it.

**11.2 Files.**

| File | One line |
|---|---|
| `rigexec_env.sh` | the sourced env block: `USD`, `RIG`, `RIGBUILD`, `PY_SITE`, `PATH`, `LD_LIBRARY_PATH`, `PYTHONPATH`, `PXR_PLUGINPATH_NAME`. **The copy is missing the last two lines of the report's snippet** (`research/B-usdrig-build.md` §7): `export RIGEXEC_IMAGING_DLL="$RIGBUILD/librigExecImaging.so"` and `export DISPLAY=:77`. Add them back before running anything through usdview/testusdview or `probe3`/`probe9`: `RIGEXEC_IMAGING_DLL` is required out of tree (§6; `plugin/rigExecUsdview/rigExecUsdview.py:39-43`) |
| `consumer/CMakeLists.txt` | a standalone project that `find_package(rigExec CONFIG REQUIRED)` and links `rigExec::rigExec` + `rigExec::rigExecImaging` + `usdImaging`, with the comment explaining why it must **not** also call `find_package(pxr)` |
| `consumer/main.cpp` | opens a stage, compiles and evaluates a rig, touches `RigExecImagingRegistry`, prints PASS |
| `consumer/build/` | the captured configure/build artefacts (`CMakeCache.txt`, `build.ninja`, `consumerProbe`, `.ninja_log`) |
| `probeExecResyncRemovePrim.cpp` | 56 lines (MEASURED, `wc -l`): `RemovePrim` with and without an `ExecUsdSystem`; prints `REPRODUCED` |

**11.3 Build and run.**

```sh
. $PROTO/usdrig-linux-build/rigexec_env.sh     # fix RIGBUILD first (scratchpad-absolute); add
                                               # RIGEXEC_IMAGING_DLL and DISPLAY back, see 11.2
cmake -S <usdrig-src> -B $RIGBUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUSD_INSTALL_DIR=$USD -DCMAKE_PREFIX_PATH=$USD \
  -DPython3_EXECUTABLE=$VENV/bin/python3 \
  -DCMAKE_MAKE_PROGRAM=$VENV/bin/ninja -DCMAKE_CXX_FLAGS="-ffp-contract=off"
ninja -C $RIGBUILD -j16 && ctest --test-dir $RIGBUILD -j8 --output-on-failure
cmake --install $RIGBUILD --prefix <prefix>
cmake -S $PROTO/usdrig-linux-build/consumer -B <b> -DCMAKE_PREFIX_PATH=<prefix> && cmake --build <b>
g++ -std=c++17 probeExecResyncRemovePrim.cpp -o probeExecResyncRemovePrim \
    -I$USD/include -L$USD/lib -Wl,-rpath,$USD/lib -lusd_tf -lusd_sdf -lusd_usd -lusd_execUsd
```

**11.4 What it proved** (`research/B-usdrig-build.md` §2, §4, §5, §7, §8, Key facts).

* usdRig builds clean on Linux/aarch64/GCC 13.3 against the stock install: **0.83 s** configure,
  **21.45 s** for 64 ninja edges at `-j16`, 26/27 ctest pass — MEASURED.
* `-ffp-contract=off` is **required**: without it `testRigExecCurvenet` fails with
  `3 traces failed on the tube`; with it, `36 traced, 0 failed` — MEASURED. That is why ADR §6 puts it
  on `usdGenMath` and why kernel determinism is gate **E-8**.
* The remaining failure is a stock OpenUSD defect, reproduced with **zero** usdRig code
  (`baseline=0 withExec=1 -> REPRODUCED`) — MEASURED (the `UsdPrimDefaultPredicate` call at
  `pxr/exec/esfUsd/stageData.cpp:361`).
* A consumer must **not** call `find_package(pxr)` alongside `find_package(rigExec)`: CMake 3.28's
  `find_dependency` call-hash misses, `pxrConfig.cmake` runs twice, and its unconditional
  `add_library(TBB::tbb SHARED IMPORTED)` at `pxrConfig.cmake:58` aborts the configure — MEASURED.
  Hence the `if (NOT TARGET usd)` guard in ADR §6's CMake skeleton.
* Inner-loop rebuild cost: **10.05 s** for all of `rigExecImaging` and its dependents; **21.4 s** for
  one TU in `rigExec` because pybind11's `-flto=auto` relink dominates — MEASURED. That is the whole
  rationale for usdGen's target split and for `_usdGen` being built without LTO.
* `ImagingLibraryPath()` searches only `<repo>/` and `<repo>/build/`, and `_env.sh:25` exports only
  `DYLD_LIBRARY_PATH` — MEASURED defects, hence `USDGEN_IMAGING_DLL`, now carried in the single
  env-var registry `10-build-dependencies-testing.md` §3.5 owns (ADR §9.4 R35: any document
  introducing an env var adds it there), and a `bin/_env.sh` that globs `lib/python*/site-packages`.

**11.5 Carried into.** `rigexec_env.sh` becomes `usdGen/bin/_env.sh` in **M0** with the three usdRig
defects fixed; `consumer/CMakeLists.txt` becomes the `usdGenConfig.cmake` consumer smoke test CI runs
on every install, and seeds gate **B-1** (T0, M0; ADR §9.4 R37; the shipped check is
`10-build-dependencies-testing.md` §1.4's `testUsdGenLinkRule_*`) — a one-line
`readelf -d $<TARGET_FILE:usdGen> | grep NEEDED` assertion in the same CI step, checking that
`libusdGen.so` carries no `usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*` or
`usdAppUtils` entry; `probeExecResyncRemovePrim.cpp` becomes `tests/testUsdGenStageEdits.cpp` (the CTest name of
`10-build-dependencies-testing.md` §5.6, §8.2), the regression proving the `TfErrorMark` containment
still works and the upstream report of S46. The
rebuild costs feed the target split in `10-build-dependencies-testing.md`.

**11.6 Caveats.** `rigexec_env.sh` hard-codes the scratchpad `RIGBUILD` and, against the very S44 rule
it demonstrates, hard-codes `python3.12` in `PY_SITE`; both must be fixed when it becomes
`bin/_env.sh`. The copy is also **two lines short** of the report's snippet — it exports neither
`RIGEXEC_IMAGING_DLL` (required out of tree, `research/B-usdrig-build.md` §6) nor `DISPLAY=:77`;
restore both from §7 of that report before sourcing it. `consumer/build/` is a captured tree with
absolute scratchpad paths inside `CMakeCache.txt` and `build.ninja` — evidence, not something to
re-use.
`patched-tests/testUsdviewRigExec.py` (the fixture-drift fix of S46) was not copied. No prototype
implements gate **B-1** today; M0 writes it beside the consumer smoke test. Everything here
is Linux/aarch64/GCC 13.3/CMake 3.28.3.

**11.7 Status.** The consumer probe binary is present and was built from this tree (22.9 s — MEASURED,
`consumer/build/.ninja_log`); the usdRig build is reproducible with the command above.

---

## 12. `motion-blur`

**12.1 Purpose.** Settle S32's cost floors: what one velocity extrapolation, one lerp, one
finite-difference velocity and one `VtArray` detach cost over 1.6 M points, so S32's motion-profile
cost model (`research/G-motion-blur-sampling-strategy.md` §5) has a measured `m` — the memory-bound
pass — to build on.

**12.2 Files.**

| File | One line |
|---|---|
| `mbbench.cpp` | 19 lines (MEASURED, `wc -l`): four `std::vector<V3>` passes over 100 000 × 16 points, 20 reps each, no USD dependency |

**12.3 Build and run.**

```sh
g++ -O2 -std=c++17 $PROTO/motion-blur/mbbench.cpp -o $PROTO/motion-blur/mbbench
$PROTO/motion-blur/mbbench
```

No USD, no GL, no env block: it is the only prototype here that needs none of them.

**12.4 What it proved** (`research/G-motion-blur-sampling-strategy.md` §5).

* Memory-bound floors over 1.6 M `float3` points, single thread: `P + (t/fps)·V` **0.97 ms**,
  `lerp(P0, P1, a)` **0.99 ms**, `(P1 − P0)·fps` **1.00 ms**, one-sample copy (the `VtArray` detach
  cost) **0.49 ms**, 19.2 MB per sample — MEASURED. Round them to ≈ 1.0 ms per pass; that is the `m`
  in the profile model, and the reason profile P1 (velocity) is affordable per frame while P2
  (`k` re-evaluations) is not.

**12.5 Carried into.** `tests/perf/benchMotionSamples.cpp`, written in **M7** beside motion profiles
P1/P2 (`11-roadmap.md` §2.8 owns the file name; ADR §7). The gate it feeds, **R-1**, is tier T4 and a release criterion, never a milestone
exit (ADR §9 R39, R40).

**12.6 Caveats.** The benchmark uses plain `float` structs, not `VtVec3fArray`, so allocator and
foreign-data-source effects are excluded. It measures no retained per-offset cache and no `k·tail`
re-evaluation — the two things PW-11 (§13) must add. There is no hdPrman binary on this host
(`research/G-hdprman-and-usdrecord-render-time-chain.md` §0), so the parity half of R-1 stays a
workstation protocol.

**12.7 Status.** Compiles and runs today; no captured output was copied, so the four numbers above
live only in `research/G-motion-blur-sampling-strategy.md` §5.

---

## 13. Prototypes M0 pre-work still has to write

`11-roadmap.md` §1 owns the pre-work numbering and registers all thirteen items (ADR §9.1 R1: the
ids are `PW-n`, never `P-n`): §1.1 **PW-1…PW-6**, the M0 decision checks, and §1.2 **PW-7…PW-13**,
the harness pre-work for the seven gates that no existing prototype covers.
`10-build-dependencies-testing.md` §6.5 records that 11 §1 owns it. All thirteen are reproduced below
unchanged; the table gives each item's gate, starting point and tier.

| PW | Prototype to write | Gate | Starts from | Tier |
|---|---|---|---|---|
| PW-1 | nanoflann kNN over 100 k and 1 M rest roots at 8 threads, with the linearity check | **E-4** (≤ 25 ms, linear in roots) | nothing; nanoflann 1.12.1 is not yet vendored | T0 |
| PW-2 | `hairTangent` fork: two glslfx variants on one 100 k deforming groom, plus a compile under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` | **S-8** | `storm-hair-look/bench_hair.cpp` + `eglctx.h`; needs a variant-A glslfx reading `inData.Neye` and a `make_scene_100000.py` | T2 |
| PW-3 | refineLevel 1 vs 2 frame time **and switch cost** (cubic index rebuild + batch revalidation) | **S-9** | `bench_hair.cpp`; the **2.6 ms warm / 5.2 ms cold** rebuild figures (ADR §5.5, `research/G-storm-throughput-and-prim-granularity.md` §1.6; `PROBE_OUTPUT.txt` probe F reads 5.04 ms cold) are the CPU half | T2 |
| PW-4 | Storm render-context resolution: does `outputs:glslfx:surface` win over plain `outputs:surface`? | **L-1** | `storm-hair-look/make_scene_preview.py` + a three-terminal material, read against `pxr/imaging/hdSt/renderDelegate.cpp:695-707` (`_RenderDelegateInfo`'s `materialRenderContexts`) and `pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45` | T2 |
| PW-5 | auto-applied `UsdGenMaskAPI` on a codeless prim type via plugInfo `AutoApplyAPISchemas` | **SI-8** | `stage-free-transport/plugin/usdGenProbeSchema/` | T1 |
| PW-6 | `reorder nameChildren` notice check (`10-…` §5.6's `testUsdGenReorderNotice`) | **SI-11** (record only; `09-…` §5.2 mints the id, nothing in the design depends on the answer) | `evaluation-scheduling/evalSched.cpp` | T1 |
| PW-7 | prim-adapter coverage: every **declared** property (no prefix filter, ADR §9.2 R6) of all five registered types appears and dirties | **SI-7** | `stage-free-transport/probe.cpp` + a real `UsdImagingSceneIndexPrimAdapter` (no prototype registers one) | T1 |
| PW-8 | initial population both ways: notice-driven (`InsertInputScenes` replays `PrimsAdded`) and the bounded first-`GetChildPrimPaths` traversal (budget **≤ 5 ms** on the 11 005-prim stage — ASSUMPTION until this runs, ADR §9.3 R28) | **SI-6** | `chain-order/src/probeChainOrder.cpp --renderindex` plus a legacy `HdRenderIndex::New` path | T1 |
| PW-9 | ragged chunk path (`cvCount == 0` + `cvOffsets`) against the uniform path | **E-1r** (≤ 2× uniform) | `data-plane-benchmark/tbbBench.cpp` | T0 |
| PW-10 | S-1 re-measured **at 100 k** curves × 8 CV, refineLevel 2, **720p and 1080p** | **S-1** | `bench_hair.cpp` + a new `make_scene_100000.py`, plus rewrites of `make_scene_40000.py` / `make_scene_200000.py` (one parameter off `make_scene.py`; neither was copied, §7.6) | T2 |
| PW-11 | motion profiles P1/P2: retained per-offset cache, `k·tail` re-evaluation, hdPrman parity | **R-1** | `prototypes/motion-blur/mbbench.cpp` (the memory floors only, §12); extend it with the retained per-offset cache and the `k·tail` re-evaluation | T4 |
| PW-12 | fold `eglctx.h` into `hairbench.cpp` so GPU counters run headless here | **S-4, S-5, S-6** | `storm-throughput/hairbench.cpp` | T2 |
| PW-13 | pruning-wrapper cost on a production-density UsdSkel-skinned scalp | **SI-9** | `chain-order/src/probeChainOrder.cpp` (it proves the wrapper works headlessly but never times it) | T1 |

Ids are `PW-n` per ADR §9.1 R1; `P0`/`P1`/`P2` in this document always mean the S32 motion profiles,
and the engine invariants are `I1`–`I8` (`03-execution-engine.md`), never `P1`–`P8`. PW-1…PW-6 above
are exactly `11-roadmap.md` §1's six: PW-1 E-4, PW-2 S-8, PW-3 S-9, PW-4 L-1, PW-5 SI-8, PW-6 the
`reorder nameChildren` check (gate SI-11, record only).

Milestone placement follows ADR §9.5 R40 and `09-performance-and-benchmarks.md` §5, the single gate
registry. The five pre-work **gates** are recorded in M0 and bind later: S-8, S-9, L-1 and SI-8 at
**M1** (PW-2, PW-3, PW-4, PW-5), E-4 at **M3** (PW-1); PW-6 binds nothing. Of the harness range,
PW-7 (SI-7), PW-8 (SI-6), PW-10 (S-1) and the batching half of PW-12 (**S-5** and **S-6**) are harness
work that can be written in M0 but whose gates bind at **M1**; PW-12's culling gate **S-4** and
PW-9's **E-1r** bind at **M2**. PW-13 (SI-9) is **M2** work; PW-11 (R-1) is **M7** work whose gate is
a release criterion, never a milestone exit (ADR §9.5 R39).
Notes: PW-1's number is what **M3**'s reference lane and the `GuideInterpolate` / `Clump` capture
schedule assume, and it is an ASSUMPTION until run. PW-2 decides
whether variant B and a per-frame `hairTangent` republish ship (ADR §5.4); PW-3 decides whether a refineLevel-1 tumble tier ships (ADR §5.5); PW-4 flips the default of
`USDGEN_STORM_MATERIAL_OVERRIDE` (ADR §5.6); PW-10 replaces a DERIVED baseline (09 §0.2's ≈ 12.2 ms
Storm draw) with a measured one. PW-9 is measured once `UsdGenCurveSource` exists in M2, though its
harness extension can be written in M0. PW-6 closes a question rather than unblocking anything — the
stack editor rewrites `usdGen:input`, never `reorder nameChildren` (ADR §2.1). PW-11 is a workstation
protocol because **no hdPrman binary exists on this host**
(`research/G-hdprman-and-usdrecord-render-time-chain.md` §0);
only the memory-bound floors are measured (§12: ~1.0 ms per pass over 1.6 M points, 0.49 ms copy). Nothing
here changes an ADR decision: each item supplies a number the plan labels UNMEASURED or ASSUMPTION,
or converts a workstation gate into a headless one.

---

## 14. Testing

This appendix is a catalogue, so what proves it is that every entry rebuilds and re-runs from the
commands recorded here. The check belongs in M0 and is cheap.

| Tier | What it proves about this appendix | Gates |
|---|---|---|
| **T0** (no USD, no Hydra) | `data-plane-benchmark` rebuilds and reproduces the numbers in its captured `results_main.txt` / `results_scale.txt`; `thirdparty-bench` rebuilds and is checked against `research/A8-seexpr-ptex-libs.md` §1.6/§2.8 (it carries no captured output); `motion-blur` rebuilds in one `g++` line (§12.3) | B-1, E-1, E-1r, E-2, E-3, E-4, E-5, E-7, E-8 baselines; the T0 halves of L-3 and L-4 |
| **T1** (headless scene index over the real `UsdImagingCreateSceneIndices` chain) | `chain-order`, `evaluation-scheduling`, `stage-free-transport`, `instancing`, the `storm-throughput` CPU probes and `freeze-bake`'s C++ probes all build and run with no GL | SI-1…SI-9 and SI-11 (through PW-6), T-2, T-INST-1/2. **SI-10** (two scene-index instances on one session, T1 M2, `09-…` §5.2) has no prototype |
| **T2** (EGL Storm on the GB10) | `storm-hair-look` renders `hair_scene.usda` with zero GL errors and `bench_hair` reproduces the **4 k** row (0.85 ms). The 40 k and 200 k rows need `make_scene_40000.py` / `make_scene_200000.py`, which were not copied (§7.6) — M0 rewrites them from `make_scene.py` (one parameter) alongside `make_scene_100000.py` for PW-10 (§13) | S-1…S-12, L-1, L-2 |
| **T3** (`testusdview` under Xvfb on `DISPLAY=:77`) | `freeze-bake/uv/testUsdviewFreezeCost.py` and `storm-throughput/bench_usdview.py` run, and every number from them is labelled CPU. The X server is **not** in `plan/prototypes/` (§0.3; the shipped harness finds it through `USDGEN_XVFB_ROOT`, `10-…` §5.2), so no T3 gate is reproducible from this directory alone | T-1, T-3, T-4, T-5 |
| **T4** (workstation protocol) | `storm-throughput/run_bench.sh` and the hdPrman parity run are documented well enough for a human at a display to execute them without this conversation | R-1, R-2, R-3 |

Gate **E-6** (append a node to a 200-node groom) has no harness in any prototype; M1 writes it against
the real Merkle digest of ADR §4.2 (§2.6). Gates **E-4**, **E-1r** and **SI-8** have no harness either
— PW-1, PW-9 and PW-5 of §13 write them. Gate **B-1** (the `DT_NEEDED` link-rule check, T0, M0,
ADR §9.4 R37) has no harness in any prototype either; M0 writes it against `libusdGen.so` beside the
consumer smoke test (§11.5). Gate **SI-9** (pruning-wrapper cost, T1, M2) has no harness either: §13's
**PW-13** (`11-roadmap.md` §1.2) is the item that writes it.
Gate **S-10** (in-place overlay) is v2 (M8) and has no prototype; **S-11** (the `head1M` static draw,
M7) and **S-12** (the tile-count sweep, M1) are run on `benchUsdGenStorm` once the four canonical
grooms of `09-performance-and-benchmarks.md` §4.4 exist. Thresholds, tiers and milestones for every id
named here are 09 §5's, not this appendix's (ADR §9.5 R40).

Two assertions are specific to this document and belong in M0 CI, both one shell script: every file
named in a §x.2 table exists under `plan/prototypes/`, and every "carried into" name matches either a
target declared in the root `CMakeLists.txt` or a CTest name registered in
`10-build-dependencies-testing.md` §5.6. Tier-T4 gates are release criteria `RC-n`, never
milestone exits (ADR §9 R39).

---

## 15. Out of scope

* **Re-litigating what the prototypes settled.** S21–S24, S1, S17–S19, S27–S31, S32's cost floors,
  S35, S33, S39–S40, S41–S42, S38 and S44–S45 are constraints; this appendix records how to re-run their proofs, not
  whether to.
* **Defining the CMake targets.** The prototype→target mapping is here; the definitions live in
  `10-build-dependencies-testing.md`.
* **Preserving the prototypes' APIs.** `UsdGenProbeOperator`, `UsdGenStubUiA`, the toy transport
  stores and the `probe*` binaries are scaffolding. Only `eglctx.h`, `kernel.h`, `sampler.h`,
  `SubtreeSnapshot`, the **two copied glslfx files** (renamed into two of the three shipped ones,
  §7.5) and the `cTransport.cpp` / `bpTransport.cpp` ABI shapes are meant to survive largely intact.
* **The scratchpad build trees**, which are evidence, not artefacts to ship.
* **Windows and macOS** (every command here is Linux/aarch64/GCC 13.3/CMake 3.28.3), and **hdPrman**,
  for which no binary exists on this host.

---

## 16. Sources

| Source | Sections used |
|---|---|
| `design/adr-v1.md` | §1 (S1, S11, S18(c), S24, S29, S31, S36 amendments), §2.1–§2.3, §4.1–§4.4, §5.1–§5.6, §6, §7, §8, and §9 rulings R1, R2, R4, R5, R7, R19, R20, R21, R27, R29, R31, R35, R37, R38, R39, R40, R41, R42, R43, R45 |
| `design/brief-v1.md` | §2.1–§2.9 (S1–S46), §4 |
| `design/proposal-performance.md` | §3.1 targets, §11.2 gates, §12 roadmap and carried-prototype table, §13 risks, Appendix |
| `design/proposal-risk.md` | §2 (targets, `USDGEN_IMAGING_DLL`, `bin/_env.sh` defects), §9.1 |
| `design/judge-delivery.md`, `design/judge-evidence.md` | the foreign-data-source buffer-return rule (ADR §9 R20), the withdrawn `GetPrim` backstop, the decorative `ordering.after` tag, `hairId` as float |
| `research/ENVIRONMENT.md` | host facts and the whole CORRECTIONS block |
| `research/B-usdrig-build.md` | §2, §4, §5, §6, §7, §8, Key facts |
| `research/G-chain-order-probe.md` | §1–§5, Key facts, Open questions |
| `research/G-data-plane-engine-prototype-benchmark.md` | intro, §3, §4, §5, §6 |
| `research/G-evaluation-scheduling-and-batching.md` | §1, §4, §5, §7, §8, §9, Key facts |
| `research/G-freeze-bake-undo-and-frozen-reentry.md` | intro, §1.2–§1.5, §2, §3, §4.1–§4.3 |
| `research/G-instancing-cards-archives-and-native-instances.md` | §0–§8 |
| `research/G-stage-free-parameter-and-time-transport.md` | §0, §1, §2.1, §3, §4, §6, §8 |
| `research/G-storm-hair-look-prototype.md` | §0, §2.1–§2.6, §3, §4, §5, §6 |
| `research/G-storm-throughput-and-prim-granularity.md` | §1.2–§1.4, §1.6–§1.9, §2, §3.1–§3.5 |
| `research/G-tool-loop-array-transport-and-cv-picking.md` | preamble, §1.1–§1.6, §2.4, §5, Appendix |
| `research/G-motion-blur-sampling-strategy.md`, `research/G-hdprman-and-usdrecord-render-time-chain.md` | §5 (the `mbbench.cpp` floors); §0 (no hdPrman binary here) |
| `research/A8-seexpr-ptex-libs.md` | §1.2, §1.6, §2.2, §2.8, §6.2, Key facts |
| `research/A3-usdrig-tools.md` | §6 (the document conventions this appendix follows) |
| OpenUSD v26.08 (verified by grep) | `vt/array.h:39-51` and the private `_IsUnique()` at `vt/array.h:1023`, `hd/renderIndex.cpp:205-214`, `hd/renderDelegate.h:584-589`, `hd/rendererPlugin.cpp:76-78`, `hd/sceneIndex.cpp:181-195`, `hd/sceneGlobalsSchema.h:37-47`, `hdSt/renderDelegate.cpp:695-707`, `hdSt/codeGen.cpp:166`, `hdSt/stagingBuffer.cpp:73`, `hdSt/basisCurvesComputations.h:210-223`, `hdSt/CMakeLists.txt:35-38`, `hdGp/sceneIndexPlugin.cpp:25`, `hdsi/materialRenderContextFilteringSceneIndex.h:20-45`, `usdImaging/usdImaging/sceneIndices.cpp:68-78,302`, `exec/esfUsd/stageData.cpp:361` (the `UsdPrimDefaultPredicate` call — `:351` is `GetPrimAtPath` and `:360` is the preceding comment line; re-verified by grep for this pass), `usd/usd/primFlags.cpp:21-25` |
| Sibling plan documents (names, gates, milestones this appendix cites rather than restates) | `09-performance-and-benchmarks.md` §0.2 (the frame ledger), §4.1, §4.4, §5–§5.4 (the gate registry); `10-build-dependencies-testing.md` §1.1, §1.2, §3.5, §5.1, §5.6, §6.5; `11-roadmap.md` §1 (the PW registry), §2.0, §2.8, §2.9; `08-tools.md` §1.4 (contract C4), §9.1; `05-static-curves-and-deformation.md` §5.4; `02-schema.md` (the property registry) |
| Prototype trees | all twelve directories under `plan/prototypes/`; every file listed in a §x.2 table was read for this appendix |
