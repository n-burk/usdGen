# G — Data-plane engine prototype benchmark: persistent VdfNetwork vs. bespoke TBB DAG

**Everything below was measured on this machine.** Two ~300-line prototypes were built out-of-tree
against `/home/burkard/work/OpenUSD_26_08` and run. Sources, build files and raw logs live in
`/tmp/claude-1000/-home-burkard-work-usdRig/887eb74a-2f4d-45ff-88d7-6c9ab67fd9a7/scratchpad/probes/data-plane-engine-prototype-benchmark/`:

| File | What it is |
|---|---|
| `kernel.h` | the one shared clump/frizz-like kernel (≈16 flops/CV) used by every variant |
| `vdfBench.cpp` | v1 VDF prototype — kept because its bug is the headline finding |
| `vdfBench2.cpp` | v2 VDF prototype — correct, mask-honouring; the one to copy into the plan |
| `tbbBench.cpp` | bespoke `tbb::parallel_for` chunked DAG with per-chunk dirty bits + `VtArray` CoW |
| `simdBench.cpp` | autovectorisation probe (AoS vs SoA), built twice (`simdBench`, `simdBenchNoVec`) |
| `sampler.h` | in-process SIGPROF sampler (this host blocks `perf` and `ptrace` attach) |
| `results_main.txt`, `results_scale.txt` | raw captured output |

Build (verbatim):

```
cmake -S <probedir> -B <probedir>/build -GNinja -DCMAKE_BUILD_TYPE=Release
cmake --build <probedir>/build
# CMakeLists.txt: find_package(pxr CONFIG REQUIRED PATHS /home/burkard/work/OpenUSD_26_08 NO_DEFAULT_PATH)
#                 target_link_libraries(vdfBench2 PRIVATE vdf exec tf gf vt work trace arch)
#                 CMAKE_CXX_FLAGS_RELEASE = "-O3 -DNDEBUG"
```

Host (measured, `lscpu`): aarch64, **NVIDIA GB10 = 10× Cortex-X925 + 10× Cortex-A725** (20 cores,
heterogeneous — this matters for the scaling curves), SVE2 present, g++ 13.3.0, cmake 3.28.3.
`perf` is refused (`/proc/sys/kernel/perf_event_paranoid = 4`) and `gdb -p` is refused by yama
`ptrace_scope`, so profiling was done with an in-process SIGPROF sampler.

---

## 0. Headline

A hand-built persistent `VdfNetwork` **does work** — sparse invalidation, per-node caching,
zero-copy handoff and topology edits all function, and I have numbers for each. But at the workload
this plugin needs (100k–1M curves, 5-node chains, interactive edits) the bespoke TBB DAG beats it by
**4× at 100k curves, 9–15× at 1M curves, 3–15× on a sparse edit, and 2–4× on peak RSS**, while being
~120 lines of code you fully own. Three VDF properties, all verified in source and in the running
prototype, are what cost it:

1. `VdfScheduler` splits every pool output that passes its buffer into `ceil(n/500)` invocations
   (grain size hardcoded, `/home/burkard/work/OpenUSD/pxr/exec/vdf/scheduler.cpp:861`) and calls
   `Compute()` once per invocation with a per-invocation affects mask. **A node that writes its
   whole buffer does O(n²/500) work and races the other invocations.**
2. The **scheduled** affects masks are frozen at schedule time from the request mask, so
   `InvalidateValues` with a 1 %-of-curves mask still recomputes 100 % of the elements. Sparse
   *execution* requires a second `VdfSchedule` built from a narrowed request.
3. Per-run cost scales with **pool size, not with the invalidated subset**: at 1M curves a
   single-node re-run costs 47 ms and a 0.5 %-narrow run costs 72 ms, versus 7.6 ms for a full TBB
   run and 0.29 ms for a TBB sparse run.

**Recommendation: bespoke TBB DAG for the hair data plane.** Keep VDF/ExecUsd for the control plane
exactly as A6 §8 proposed. §7 gives the API shape to commit to.

---

## 1. The workload

100 000 curves × 8 CVs = 800 000 `GfVec3f` (9.6 MB), a chain of 5 "styler" nodes, each with one
scalar `amount` parameter. The kernel (`kernel.h:28-49`, `:69-90`, `:101-117`) is a clump/frizz
stand-in: per CV, lerp toward a target derived from the curve root plus a hash-noise offset,
≈16 flops per CV. All three engines produce **the same checksum, 3355.7599** (VDF cv-mode,
VDF simple executor, TBB DAG) — the comparison is like-for-like arithmetic.

---

## 2. The trap that dominates everything: VDF splits pool nodes into 500-element invocations

v1 (`vdfBench.cpp`) does what the task description implied — one `VdfInputVector<GfVec3f>(n)` pool,
five READWRITE stylers with `SetAffectsMask(VdfMask::AllOnes(n))`, each `Compute()` taking a raw
pointer to the whole buffer and styling all curves. Measured:

```
$ ./build/vdfBench --curves 100000 --stylers 5 --runs 3
run_cold_ms   854.53   run_full_ms   867–968
$ ./build/vdfBench --curves 100000 --stylers 5 --runs 2 --simple-executor 1
run_cold_ms    19.76   run_full_ms     4.60
```

The parallel executor was **200× slower than `VdfSimpleExecutor`**, and superlinear in `n`:

| curves | elements | `run_cold_ms` (parallel, v1) |
|---:|---:|---:|
| 12 500 | 100 000 | 16.6 |
| 25 000 | 200 000 | 51.3 |
| 50 000 | 400 000 | 252.2 |
| 100 000 | 800 000 | 1031.5 |

The SIGPROF sampler put 99.4 % of samples in my own kernel, so I instrumented the node with an
atomic call counter:

```
$ ./build/vdfBench --curves 100000 --stylers 5 --runs 1
compute_calls 6401  curves_processed 640100000  (expected calls=5 curves=500000)
$ ./build/vdfBench --curves  25000 --stylers 5 --runs 1
compute_calls 1601  curves_processed  40025000  (expected calls=5 curves=125000)
```

800 000 / 500 = 1600 invocations per node × 4 pool nodes + 1 terminal = **6401**. Source:

- `scheduler.cpp:861` `const size_t grainSize = 500;` with the comment *"empirically determined to
  work well over a broad range of networks … must always be >= 5, and be divisible by 5, so not to
  split up packed transforms in the point pool"*.
- `scheduler.cpp:866-872` — `numPartitions = ceil(partitionSize/grainSize)`; if ≤ 1 no invocations
  are created (which is why a 1-styler chain and a `--curves 2000` chain look fine).
- `scheduler.cpp:889-897` — the pool chain is walked through `GetPassToOutput`, so **only outputs
  that pass their buffer are split**; the terminal (which keeps its data) gets one invocation.
- The per-invocation masks come from `_ComputeInvocationBitsets` / `_ComputePartitionSubset`
  (`scheduler.cpp:644-781`) and reach the callback through `VdfContext::_GetOutputMasks`
  (`iterator.cpp:83-90` → `schedule.h:315-320`).

**Consequences the plan must encode:**

- A pool node's `Compute` **must** read its invocation's affects mask
  (`VdfIterator::_GetOutputMasks`, or just use `VdfReadWriteIterator`, which iterates it —
  `readWriteIterator.h:44-46, 292-309`) and write only those elements. Anything else is both
  quadratic and a data race.
- The 500-element grain **does not respect curve boundaries** (500 / 8 CVs = 62.5 curves). A kernel
  that reads CV0 of a curve to clump the rest would read an element another thread is concurrently
  writing. So either the data-flow element is **one whole curve** (a registered POD strip type), or
  every kernel is strictly per-CV with no neighbour reads.

v2 (`vdfBench2.cpp`) fixes both. `PoolSlice<T>` (a `VdfIterator` subclass) returns the buffer base
pointer plus the scheduled affects mask, and `ForEachAffectedRun` walks the mask's RLE runs with
`TfCompressedBits::GetPlatformsView()` (`/home/burkard/work/OpenUSD/pxr/base/tf/compressedBits.h:1810-1893`)
so the kernel still runs over contiguous ranges. Same run, corrected:

```
$ ./build/vdfBench2 --curves 100000 --stylers 5 --runs 5 --element cv
run_cold_ms 7.60   checksum=3355.7599   compute_calls=6401  elements_touched=4000000
```

7.60 ms instead of 854 ms, and the checksum now matches `VdfSimpleExecutor` and the TBB DAG exactly.

---

## 3. What VDF gives you, measured (100 000 curves × 8 CV, 5 stylers, 20 threads)

`--element strip` makes one **curve** the data-flow element (`probe::HairStrip`, 8 CVs, 96 B,
registered via `TF_REGISTRY_FUNCTION(VdfExecutionTypeRegistry){ VdfExecutionTypeRegistry::Define(probe::HairStrip{}); }`).
`--element cv` keeps one CV per element. `--request-all` puts *every* styler output in the
`VdfRequest`, not just the terminal.

| measurement | VDF cv, request=terminal | VDF cv, request=all | VDF strip, request=all | TBB DAG (chunk 512) |
|---|---:|---:|---:|---:|
| cold run (ms) | 7.74 | 7.16 | 7.45 | 2.12 |
| full run, min/med (ms) | 7.39 / 8.59 | 7.66 / 10.36 | 3.80 / 8.32 | 1.72 / 1.80 |
| run after 1 % contiguous invalidation (ms) | 8.09 / 8.73 | 8.35 / 9.66 | 3.22 / 5.35 | **0.035 / 0.040** |
| run after 1 % scattered invalidation (ms) | 7.89 / 8.41 | 4.39 / 8.77 | 2.09 / 4.57 | **0.036 / 0.039** |
| run after editing the **last** styler's param (ms) | 4.29 / 7.82 | 1.36 / 1.79 | **0.90 / 0.92** | 0.18 / 0.25 |
| run after editing the **first** styler's param (ms) | 8.60 / 9.43 | 5.53 / 8.95 | 3.28 / 5.37 | 1.75 / 1.85 |
| cached no-op run (ms) | 0.001–0.006 | 0.001–0.010 | 0.001–0.014 | n/a (dirty bits) |
| `InvalidateValues` call itself (ms) | 0.013–0.034 | — | — | n/a |
| elements touched, full run | 4 000 000 | 4 000 000 | 500 000 (=4 M CVs) | 4 M CVs |
| elements touched, 1 % invalidation | **4 000 000** | **4 000 000** | **500 000** | **40 000 CVs** |
| elements touched, last-param edit | 4 000 000 | **800 000** | **100 000** | 800 000 CVs |

Commands: `./build/vdfBench2 --curves 100000 --stylers 5 --runs 7 --element {cv,strip} --request-all {0,1}`
and `./build/tbbBench --curves 100000 --stylers 5 --runs 7 --chunk 512 --per-node-buffers 1`
(full transcript in `results_main.txt`).

### 3.1 Sparse invalidation ≠ sparse execution

`InvalidateValues` with a 1 %-of-curves mask is fast (13–34 µs) and its traversal is correctly
sparse — but the following `Run` touches **every** element. The scheduled affects mask per
invocation is computed once from the request mask (`scheduler.cpp:737-781`), and
`VdfExecutorInterface::Run(schedule, computeRequest)` only narrows *which outputs* to compute
(`executorInterface.cpp:53-60`), not which elements.

The only way to get sparse execution is a **second schedule built from a narrowed request**. That
works, and it is cheap to build:

| curves | `narrow_schedule_ms` | `narrow_run_ms` | elements touched | full run for reference |
|---:|---:|---:|---:|---:|
| 100 000 (cv, 1 %) | 0.095 | 0.652 | 40 000 / 4 000 000 | 7.7–10.4 ms |
| 100 000 (strip, 1 %) | 0.060 | 0.654 | 5 000 / 500 000 | 3.8–8.3 ms |
| 1 000 000 (strip, 0.5 %) | 0.113 | **72.0** | 50 000 / 5 000 000 | 70–135 ms |

At 100k it is a ~10× win over a full run (fixed per-`Run` overhead ≈ 0.65 ms dominates). At 1M it
is **no win at all** — the narrow run costs as much as the full run. The TBB DAG's equivalent
numbers are 0.035–0.040 ms at 100k and 0.29 ms at 1M.

### 3.2 Per-node result caching is a scheduling lever, and it is not free

In a pool chain with buffer passing, an intermediate output's buffer is *moved* into the next
node's output, so invalidating anything downstream forces a re-run **from the source**. That is why
`--request-all 0` shows `elements_touched = 4 000 000` even for a last-styler parameter edit.

The lever is `VdfScheduler::_ScheduleBufferPasses`
(`/home/burkard/work/OpenUSD/pxr/exec/vdf/scheduler.cpp:426-432`): *"In order to avoid passing
buffers for outputs, which are requested, we set the keep mask to include the whole request mask at
each output in the request."* Putting each styler's output in the `VdfRequest` turns that node into
a durable cache. Measured effect (100k, cv mode): last-param edit drops from 4 000 000 touched /
7.8 ms to **800 000 touched / 1.79 ms**. Cost: peak RSS at 1M curves rises from **507 MB** to
**1 263 MB** (§5).

This is exactly the mechanism the plan's "freeze the output of an operator and comb it by hand"
tool needs — but note it is a *schedule-wide* decision, so freezing one operator changes the memory
profile of the whole chain.

### 3.3 The parallel executor does not scale on this workload

```
$ for t in 1 2 4 8 20; do ./build/vdfBench2 --curves 100000 --stylers 5 --runs 5 --element strip --threads $t; done
```

| threads | VDF strip `run_full_ms` (min) | TBB DAG `run_full_ms` (`PXR_WORK_THREAD_LIMIT`) |
|---:|---:|---:|
| 1 | 3.53 | 3.90 |
| 2 | 7.07 | 3.01 |
| 4 | 6.55 | 1.66 |
| 8 | 5.79 | **1.02** |
| 20 | 5.72 | 1.79 |

VDF's parallel engine is *slower with 20 threads than with 1* here; `VdfSimpleExecutor` (serial,
copies READWRITE buffers, `simpleExecutor.cpp:136-147`) does the same work in a steady **5.01–5.16 ms**.
The TBB DAG scales 3.8× from 1→8 threads and then regresses at 20 — consistent with the
heterogeneous 10×X925 + 10×A725 topology and with the kernel being partly bandwidth-bound.

### 3.4 Extraction to `VtArray`, and `Share()`

```
$ ./build/vdfBench2 --curves 100000 --stylers 5 --runs 7 --element cv --request-all 1
vector sharable=1 shared=0 -> Share()=1
extract_copy_ms         min 0.185  med~ 0.636  max 3.225
extract_after_share_ms  min 0.000  med~ 0.000  max 0.000
run_full_after_share_ms 6.41
```

- `ExtractAsVtArray<GfVec3f>(800000, 0)` on an unshared buffer copies 9.6 MB in **0.185–0.64 ms**
  (≈ 15–52 GB/s).
- `VdfVector::Share()` **is callable on the executor's terminal buffer through the `const VdfVector*`
  returned by `GetOutputValue`** (`Share()` is declared `const`, `vdf/vector.h:334`), succeeded
  (`sharable=1` — the ≥ 5000-element rule at `vdf/vectorData.h:42` is satisfied), and made extraction
  **free** (`VtArray` over a foreign data source, `vdf/vector.h:398-401`).
- Crucially, the **next full `Run` after sharing still worked and cost 6.41 ms** — the same as an
  unshared run. This answers A6's open question: sharing a requested/kept terminal output does not
  break the executor (`GetReadWriteAccessor` detaches, `vdf/vector.h:456-470`). It is documented
  *not thread safe*, so it must be done on the update thread before publishing.
- The TBB DAG's equivalent handoff is a `VtVec3fArray` copy-construct: **0.00005 ms** (refcount
  bump), and a subsequent full run with an outstanding reference cost 2.32 ms vs 1.97 ms — i.e. one
  CoW detach of 9.6 MB.

### 3.5 Topology edits and schedule cost

```
schedule_valid_after_topology_edit  1      <-- see note
reschedule_ms 0.341 (strip) / 1.824 (cv)   invalidate_topo_ms 0.005   run_after_edit_ms 0.87 / 1.34
```

| network | nodes (incl. input vectors) | `VdfScheduler::Schedule` min/med/max (ms) |
|---|---:|---:|
| 20 chains × 5 stylers, 8192 elems | 220 | 0.363 / 0.412 / 0.525 |
| **200 chains × 5 stylers** (1000 styler nodes) | **2200** | **2.93 / 3.24 / 4.39** |
| 1000 chains × 5 stylers | 11000 | 13.68 / 15.28 / 23.04 |

≈ **1.4 µs per node**, linear. For the task's "1000 nodes after a topology edit" the answer is
**~3 ms**. Rescheduling the *hair* network (11 nodes, 800k elements) costs 1.8 ms in cv mode and
0.34 ms in strip mode — the difference is the pool partitioning work (1600 vs 200 partitions), so
schedule cost is driven by `elements/500` as much as by node count.

Note on `schedule_valid_after_topology_edit  1`: after `net.Connect(...)` added a node **downstream**
of the existing terminal, `schedule.IsValid()` still returned `1`. `Vdf_ScheduleInvalidator`
(`vdf/scheduleInvalidator.h:40-77`) invalidates schedules that *contain* the changed connection;
appending below the requested output does not touch the scheduled subgraph. **Do not use
`IsValid()` as the "did my graph change" signal** — track `VdfNetwork::GetVersion()` yourself, the
way `Exec_Runtime::InvalidateExecutor` does (`exec/runtime.cpp:119-142`). After the edit,
`InvalidateTopologicalState()` cost 5 µs and the re-run correctly executed only the new node
(`touched=100000` = 1 node's worth).

---

## 4. The bespoke TBB DAG

`tbbBench.cpp`, ~200 lines. Storage = `VtVec3fArray` per node; dirty state = one `uint8_t` per
chunk of `--chunk` curves; execution = `tbb::parallel_for(blocked_range<size_t>(0, nChunks, 1), body)`
where the body skips clean chunks, `memcpy`s the input chunk into the node's own buffer and runs the
kernel. Handoff to Hydra = copy-construct the `VtArray` (CoW).

```
$ ./build/tbbBench --curves 100000 --stylers 5 --runs 7 --chunk 512 --per-node-buffers 1
run_cold_ms 2.12  checksum=3355.7599
run_full_ms 1.72–1.91
run_after_sparse_ms 0.035–0.044     (1 % of chunks dirty on all 5 nodes)
run_after_scatter_ms 0.036–0.040
run_after_lastparam_ms 0.179–0.411  (only node 5 dirty)
run_after_firstparam_ms 1.751–1.969
extract_vtarray_cow_ms 0.00000–0.0001
run_full_with_outstanding_ref_ms 2.32
```

Chunk-size sensitivity (100k curves, 20 threads, `run_full_ms` first sample):
128 → 1.55, 512 → 1.83, 1024 → 1.89, 4096 → 9.40 (25 chunks over 20 threads: bad balance),
16384 → 2.58. **128–1024 curves per chunk is the sweet spot**; the design should size chunks by
curve count, not by a fixed byte budget.

A `--per-node-buffers 0` mode (one buffer passed down the chain, VDF-like) is 1.17–1.28 ms full run
and 293 MB RSS at 1M curves, but its "last param" number (0.22–0.30 ms) is **semantically invalid** —
without a per-node copy there is nothing to re-run *from*. Included only to show the memory/latency
cost of per-node caching: 668 MB vs 293 MB at 1M curves, 1.8 ms vs 1.2 ms per full run.

---

## 5. Scale and memory (1 000 000 curves × 8 CV = 8 M CVs, 96 MB of curve data, 5 stylers)

```
$ ./build/vdfBench2 --curves 1000000 --stylers 5 --runs 5 --element strip --request-all 1
$ ./build/tbbBench  --curves 1000000 --stylers 5 --runs 5 --chunk 512
$ /usr/bin/time -f "%M KB  %e s" <each of the above with --runs 1>
```

| | full run (ms) | last-param edit (ms) | 0.5 % sparse (ms) | peak RSS |
|---|---:|---:|---:|---:|
| VDF strip, request-all | 69.9 / 116.4 / 134.8 (min/med/max) | 47.2 / 49.4 / 51.6 | 72.0 (narrow schedule) | **1 263 MB** |
| VDF strip, request=terminal | — | — | — | 507 MB |
| TBB DAG, per-node buffers | **7.59 / 7.65 / 7.67** | 0.22–0.30 (scaled from 100k) | **0.29** | 668 MB |
| TBB DAG, single buffer | 7.67 (chunk 512) | n/a | 0.10 | 293 MB |

**9–15× throughput gap, and VDF's per-run floor scales with pool size**: a single-node re-run over a
1M-curve pool costs 47 ms — 6× the *entire* TBB chain. That is buffer preparation
(`VdfParallelDataManagerVector` keeps private + scratch + public buffers per output,
`vdf/parallelDataManagerVector.h:124-168`), not arithmetic; the 1 263 MB RSS for 96 MB of curve data
is the same effect measured a second way (≈ 687 MB of executor overhead above the 6 × 96 MB the
design itself needs).

---

## 6. SIMD: does g++ -O3 autovectorise a strip kernel on aarch64?

usdRig's only SIMD kernel is SSE2 with a scalar fallback — on this aarch64 host it takes the scalar
path (`/home/burkard/work/usdRig/libs/rigExecMath/simdKernels.cpp:14-45`: `RIGEXEC_HAS_SSE2` is
defined only for `__SSE2__ / _M_X64 / _M_IX86_FP>=2`, and the `#else` branch loops
`RigExecApplyWeightedMatrix` scalar). So there is nothing to inherit; the question is what the
compiler does unaided.

**Yes, GCC 13.3 autovectorises all three kernels**, using 128-bit NEON:

```
$ g++ -O3 -std=gnu++17 -I. -fopt-info-vec-optimized -c simdBench.cpp -o /dev/null
kernel.h:34:27:      optimized: loop vectorized using 16 byte vectors   (StyleAoS, inner CV loop)
kernel.h:59:26:      optimized: loop vectorized using 16 byte vectors   (StyleSoA)
simdBench.cpp:38:26: optimized: loop vectorized using 16 byte vectors   (SoAStrip)
```

800 000 CVs per pass, 16 flops/CV = 12.8 MFLOP/pass, 200 passes, single-threaded:

| kernel | layout | `-O3` | `-O3 -fno-tree-vectorize` | vec speedup | `-O3 -mcpu=native` |
|---|---|---:|---:|---:|---:|
| `aos_interleaved` (`StyleAoS`, `GfVec3f[]`) | AoS float3 | 0.506 ms/pass, **25.3 GFLOP/s** | 0.599 ms, 21.4 | 1.18× | 0.555 ms, 23.1 |
| `soa_streamed` (8 planar arrays in) | SoA, bandwidth-bound | 0.545 ms, **23.5 GFLOP/s**, 64.6 GB/s | 0.737 ms, 17.4 | 1.35× | 0.429 ms, **29.8 GFLOP/s** |
| `soa_strip` (3 planar arrays, `t` from index) | SoA, cache-resident | 0.158 ms, **80.9 GFLOP/s**, 121 GB/s | 0.449 ms, 28.5 | **2.84×** | 0.157 ms, 81.3 |

Commands: `./build/simdBench 100000 200`, `./build/simdBenchNoVec 100000 200`,
`g++ -O3 -std=gnu++17 -mcpu=native -I. simdBench.cpp -o simdBenchNative && ./simdBenchNative 100000 200`.

Readings for the plan:

- **AoS `GfVec3f` costs you ~3.2× against SoA.** The interleaved layout only gets a 1.18× vector
  speedup (GCC vectorises the 8-iteration CV loop, not across curves); the planar strip layout gets
  2.84× and runs at 80 GFLOP/s. 25 → 81 GFLOP/s is the prize for keeping x/y/z planar internally and
  interleaving only at the Hydra boundary.
- `-mcpu=native` does **not** switch GCC 13 to SVE (still "16 byte vectors") but helps the
  bandwidth-bound SoA case by 27 %. There is no reason to hand-write NEON intrinsics for kernels of
  this shape; there *is* a reason to control data layout.
- The 121 GB/s figure for `soa_strip` is a 9.6 MB working set that is partly cache-resident; do not
  quote it as DRAM bandwidth.
- Single-thread ceiling ≈ 0.16–0.55 ms per styler pass over 800k CVs. The measured 5-styler TBB
  chain at 20 threads is 1.72–1.91 ms, i.e. roughly the AoS single-thread cost of *one* pass — the
  chain is memory-bound well before it is flop-bound.

---

## 7. The two usdRig traps, re-tested

| Trap (`/home/burkard/work/usdRig/docs/mover-graph-cutover.md:511-519`) | Result here |
|---|---|
| *"A revision uses a READWRITE connector … must not `Allocate`. Allocating fails with 'output cannot hold a boxed value' and silently degrades to pass-through."* | **Reproduced exactly.** `./build/vdfBench2 --curves 5000 --stylers 2 --element cv --allocate-trap 1` → `Coding Error: in Vdf_AllocateBoxedValueVector at line 75 of .../vdf/allocateBoxedValue.cpp -- Output '.pool' cannot hold a boxed value.` once per invocation, exit code 0. Root cause: `VdfReadWriteIterator<T>::Allocate` routes to `Vdf_AllocateBoxedValue` (`readWriteIterator.h:215-234`), and a pool output cannot hold a boxed value. |
| *"Anything talking to VDF directly must force `ExecTypeRegistry::GetInstance()`, or the executor cannot distinguish registered value types."* | **Did not reproduce in a single-binary, VDF-only program.** `--skip-type-registry 1` ran correctly for `GfVec3f`, `float` and my custom `probe::HairStrip`, because `VdfOutputSpecs::Connector<T>()` reaches `VdfExecutionTypeRegistry` (`executionTypeRegistry.h:85-101`), whose `GetInstance()` fires the `TF_REGISTRY_FUNCTION(VdfExecutionTypeRegistry)` **in the same binary**. The trap is real when the registrations live in a *plugin library* that has not been loaded yet, and for `ExecTypeRegistry` (exec-level) types such as usdRig's packets (`/home/burkard/work/usdRig/libs/rigExec/types.cpp:96-106`). Keep the forced `GetInstance()` in library init; it costs nothing. |

One trap this probe **adds** to that list, and it is worse than either: **never write outside your
invocation's scheduled affects mask on a pool output** (§2). It is silent (no error, no crash),
quadratic, and a data race.

A fourth, smaller one: `VdfExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>` does
not compile from `vdf/executor.h` + `vdf/parallelExecutorEngine.h` alone — the factory instantiates
`VdfSpeculationExecutor<…::SpeculationEngineType, …>`, so you must also
`#include "pxr/exec/vdf/parallelSpeculationExecutorEngine.h"` or you get
`'VdfSpeculationExecutor<E, D>::_engine' has incomplete type`.

---

## 8. Recommendation

### Engine: bespoke TBB DAG for the data plane

| criterion | persistent VdfNetwork | bespoke TBB DAG |
|---|---|---|
| full chain, 100k curves | 3.8–10.4 ms | **1.7–1.9 ms** |
| full chain, 1M curves | 70–135 ms | **7.6 ms** |
| 1 % sparse edit, 100k | 0.65 ms (+0.06–0.10 ms reschedule) | **0.035–0.044 ms** |
| 0.5 % sparse edit, 1M | 72 ms | **0.29 ms** |
| single-node re-edit, 1M | 47 ms | **~0.3 ms** |
| peak RSS, 1M curves | 507 MB (no per-node cache) / 1 263 MB (per-node cache) | 293 MB / **668 MB** |
| thread scaling 1→8 | 3.53 → 5.79 ms (negative) | 3.90 → **1.02 ms** (3.8×) |
| zero-copy to Hydra | `VdfVector::Share()` — works, 0 ms, but "not thread safe" and undocumented | `VtArray` CoW — 0.00005 ms, first-class |
| per-node result cache | yes, via `VdfRequest` membership (`scheduler.cpp:426-432`) — schedule-wide, 2.5× RSS | yes, per node, opt-in |
| topology edit | `Schedule` ≈ 1.4 µs/node **plus** `elements/500` partitioning; `IsValid()` is not a reliable change signal | rebuild the node vector; no scheduling phase |
| element granularity | forced to interact with a hardcoded grain of 500 (`scheduler.cpp:861`) | your choice; 128–1024 curves measured best |
| API stability | undocumented executor/data-manager surface with `XXX` notes (`executorInterface.h:191-203`), exec is "experimental" in 26.08 | yours |
| lines of code to own | ~130 (nodes + masks + executor lifecycle) but the semantics above are all implicit | ~200, all explicit |

VDF is engineered for a *rig* point pool: tens of thousands of elements, hundreds of nodes, deep
chains, sparse per-point masks driven by a compiler that knows the dependency structure. A groom is
the mirror image: millions of elements, a handful of nodes, and edits that are either "one operator's
parameter" or "1 % of curves under the brush". Every VDF mechanism that pays off in the first shape
(500-element invocations, triple-buffered data manager, mask-interned invalidation replay) is
overhead in the second.

Keep the split A6 §8 recommended: **ExecUsd for the control plane** (operator parameters, chain
resolution through relationship accessors, dirty detection from `UsdNotice::ObjectsChanged`,
consuming usdRig's transform/skin computations), **bespoke TBB DAG for the curve data plane**.

### API shape the plan should commit to

Node:

```cpp
struct GenContext {                 // per-evaluation, immutable
    float                time;
    const SurfaceSample *surface;   // from the upstream scene index (usdRig's result)
    uint32_t             seed;
};

class UsdGenOp {                    // one per operator prim
public:
    virtual ~UsdGenOp();
    // Chunk granularity is fixed by the graph, not the op (128-1024 curves).
    virtual void Execute(const GenContext &ctx,
                         const CurveChunkView &in,   // read-only upstream chunk
                         CurveChunkView *out,        // this node's own buffer
                         size_t chunkIndex) const = 0;
    // Which upstream chunks this node's chunk depends on. Identity for
    // per-curve stylers; a generator that resamples faces returns a fan-in set.
    virtual void UpstreamChunks(size_t myChunk, std::vector<size_t> *out) const;
    // Which parameter tokens, when edited, dirty this node.
    virtual const TfTokenVector &ParameterNames() const = 0;
};
```

Buffers — **SoA internally, AoS only at the Hydra boundary** (§6: 25 → 81 GFLOP/s):

```cpp
struct CurveBuffer {                // one per node output
    VtFloatArray  px, py, pz;       // planar CV positions, curveCount * cvCount
    VtFloatArray  width;            // and other per-CV primvars, planar
    VtIntArray    vertexCounts;     // per curve
    VtVec2fArray  rootUV;           // per curve
    uint64_t      topologyVersion;  // bumped when curveCount / vertexCounts change
};
VtVec3fArray Interleave(const CurveBuffer &);   // once, into the scene index
```

Invalidation:

```cpp
class UsdGenGraph {
    struct NodeState {
        std::unique_ptr<UsdGenOp> op;
        CurveBuffer               out;       // per-node cache; opt out for memory
        std::vector<uint8_t>      dirty;     // one byte per chunk
        uint64_t                  paramVersion;
    };
    void DirtyParameter(NodeId, const TfToken &);   // all chunks of this node + all downstream
    void DirtyChunks(NodeId, TfSpan<const size_t>); // brush stroke / surface deform
    void DirtyTopology(NodeId);                     // generator changed curve count
    // Executes only dirty chunks, in topological order, tbb::parallel_for per node.
    const CurveBuffer &Evaluate(NodeId terminal, const GenContext &);
};
```

Extraction: hand Hydra the `VtArray`s by value. Copy-construct is a refcount bump (0.00005 ms
measured); the next edit pays one CoW detach (2.32 vs 1.97 ms for a 9.6 MB buffer) — cheap, and it
gives the render thread an immutable snapshot for free, which is the discipline usdRig already
follows (`libs/rigExecImaging/registry.cpp:343-345`).

Chunking: **512 curves per chunk** as the default (measured 1.72–1.91 ms at 100k; 128 is marginally
faster at 1.55 ms, 4096 is a cliff at 9.40 ms). Chunk boundaries are curve boundaries, always — this
is the one thing VDF gets wrong for this domain and it is free to get right.

Freeze/comb tooling: a frozen operator becomes a source node whose `CurveBuffer` is loaded from the
stage instead of computed; the graph already caches per node, so freezing is "stop executing this
node and everything above it", not a new mechanism.

### If the team still wants VDF

The prototype shows it can be made to work; the mandatory rules are:

1. Data-flow element = **one curve** (a registered POD strip, `--element strip`), never one CV.
   Measured 5.8 ms vs 7.7 ms cold and 801 vs 6401 `Compute` calls at 100k curves, and it is the only
   way a clump kernel can legally read the curve root.
2. Every pool `Compute` reads `VdfIterator::_GetOutputMasks` and writes only the affects-mask runs.
3. Put every operator output in the `VdfRequest` if you want per-node caching, and budget 2.5× RSS.
4. Keep a second, narrow `VdfSchedule` for brush-sized edits; it only pays off below ~200k curves.
5. Track `VdfNetwork::GetVersion()`, not `VdfSchedule::IsValid()`, and call
   `InvalidateTopologicalState()` (5 µs) after every edit.
6. `Share()` the terminal buffer before handing it to the scene index; it is free and the executor
   survives it.

---

## Key facts

- VDF splits every buffer-passing pool output into `ceil(n/500)` invocations; grain size is hardcoded
  at `/home/burkard/work/OpenUSD/pxr/exec/vdf/scheduler.cpp:861` and the partitioning is at
  `scheduler.cpp:644-781, 866-872`. Measured: `compute_calls 6401` for 5 nodes over 800 000 elements
  (`./build/vdfBench --curves 100000 --stylers 5`).
- A pool node that ignores its invocation's affects mask is quadratic: 854 ms vs 7.6 ms for the same
  chain (v1 vs v2 prototype), and superlinear in n (16.6 / 51.3 / 252 / 1032 ms for 12.5k / 25k / 50k /
  100k curves).
- The 500-element grain does not align to 8-CV curves, so a cross-CV kernel on a per-CV pool races
  concurrent invocations. Element = whole curve is the only safe granularity (verified by the
  matching checksums 3355.7599 / 26792.6545 across engines).
- `InvalidateValues` is sparse and cheap (13–34 µs for a 1 % mask) but the following `Run` still
  touches every element: `elements_touched: full=4000000 contig=4000000 scatter=4000000`. Scheduled
  affects masks are frozen at schedule time; `Run(schedule, computeRequest)` narrows outputs only
  (`executorInterface.cpp:53-60`).
- Sparse execution needs a narrowed second schedule: 0.060–0.113 ms to build, 0.652 ms to run 1 % at
  100k curves (vs 7.7–10.4 ms full) — but **72 ms to run 0.5 % at 1M curves**, i.e. no better than a
  full run.
- Per-node result caching in VDF = putting the output in the `VdfRequest`
  (`scheduler.cpp:426-432` sets `keepMask = requestMask` for requested outputs, which suppresses
  buffer passing). Measured: last-param edit drops from 4 000 000 to 800 000 elements touched and
  from 7.8 ms to 1.79 ms; peak RSS at 1M curves rises 507 MB → 1 263 MB.
- `VdfExecutor<VdfParallelExecutorEngine, VdfParallelDataManagerVector>` gets *slower* with threads
  on this workload: 3.53 ms (1 thread) → 5.72 ms (20 threads); `VdfSimpleExecutor` does it in
  5.01–5.16 ms. The TBB DAG goes 3.90 → 1.02 ms (1 → 8 threads).
- Head-to-head at 100k curves × 8 CVs × 5 stylers, 20 threads: VDF strip 3.80–8.32 ms full run,
  0.90 ms last-param, 2.09–5.35 ms after a 1 % invalidation; TBB DAG **1.72–1.91 ms**, **0.18–0.41 ms**,
  **0.035–0.044 ms**.
- Head-to-head at 1M curves: VDF strip 69.9–134.8 ms full, 47.2–51.6 ms single-node; TBB DAG
  **7.59–7.67 ms** full, 0.29 ms sparse. Peak RSS 1 263 MB vs 668 MB (`/usr/bin/time -f "%M KB"`).
- `VdfVector::Share()` is `const` (`vdf/vector.h:334`), succeeds on the executor's terminal buffer
  (≥ 5000 elements, `vdf/vectorData.h:42`), makes `ExtractAsVtArray` free (0.185–0.64 ms → 0.000 ms),
  and the next full `Run` still works (6.41 ms). Documented not thread safe.
- `VdfScheduler::Schedule` costs ≈ 1.4 µs/node, linear: 0.36–0.53 ms for 220 nodes, **2.93–4.39 ms for
  2200 nodes (1000 styler nodes)**, 13.7–23.0 ms for 11 000 nodes. Pool partitioning adds a term in
  `elements/500`: rescheduling an 11-node network costs 1.82 ms at 800k CV elements, 0.34 ms at 100k
  strip elements.
- `VdfSchedule::IsValid()` returned `1` after appending a node **downstream** of the requested output
  (`schedule_valid_after_topology_edit 1`). Use `VdfNetwork::GetVersion()` as the change signal, as
  `exec/runtime.cpp:119-142` does. `InvalidateTopologicalState()` costs 5 µs.
- `Allocate` on a READWRITE pool output reproduces usdRig's trap verbatim:
  `Coding Error: in Vdf_AllocateBoxedValueVector at line 75 of .../vdf/allocateBoxedValue.cpp --
  Output '.pool' cannot hold a boxed value.` (`--allocate-trap 1`).
- The `ExecTypeRegistry::GetInstance()` trap did **not** reproduce in a single-binary VDF program:
  `VdfOutputSpecs::Connector<T>` reaches `VdfExecutionTypeRegistry::GetInstance()`, which fires
  same-binary `TF_REGISTRY_FUNCTION`s. It remains necessary for plugin-registered and exec-level types
  (`/home/burkard/work/usdRig/libs/rigExec/types.cpp:96-106`).
- Instantiating the parallel executor also needs `#include "pxr/exec/vdf/parallelSpeculationExecutorEngine.h"`;
  without it g++ reports `'VdfSpeculationExecutor<E, D>::_engine' has incomplete type`.
- g++ 13.3 `-O3` autovectorises all three kernels with 128-bit NEON (`-fopt-info-vec-optimized`:
  `kernel.h:34`, `kernel.h:59`, `simdBench.cpp:38`). Throughput for 800 000 CVs at 16 flops/CV,
  single thread: AoS `GfVec3f` **25.3 GFLOP/s** (0.506 ms/pass), SoA streamed **23.5 GFLOP/s**
  (64.6 GB/s), SoA strip **80.9 GFLOP/s** (0.158 ms/pass). `-fno-tree-vectorize` gives 21.4 / 17.4 /
  28.5 GFLOP/s — a 2.84× vectorisation win for the SoA strip layout, 1.18× for AoS.
- `-mcpu=native` does not enable SVE codegen in GCC 13 here (still "16 byte vectors") but lifts the
  bandwidth-bound SoA case from 23.5 to 29.8 GFLOP/s.
- usdRig's only SIMD kernel is SSE2 + scalar fallback and takes the scalar path on aarch64
  (`/home/burkard/work/usdRig/libs/rigExecMath/simdKernels.cpp:14-45`); nothing to reuse.
- TBB chunk sizing: 128 → 1.55 ms, 512 → 1.83, 1024 → 1.89, 4096 → 9.40, 16384 → 2.58 (100k curves,
  20 threads). Use 128–1024 **curves**, aligned to curve boundaries.
- Host is heterogeneous (10× Cortex-X925 + 10× Cortex-A725); both engines regress past ~8–10 threads,
  so thread-count tuning must not assume symmetric cores.
- This host blocks `perf` (`perf_event_paranoid = 4`) and `gdb -p` (yama `ptrace_scope`); the probe
  ships an in-process SIGPROF sampler (`sampler.h`) that works and should be reused for any future
  CPU profiling here.

## Decisions this settles

1. **The hair/fur data plane is a bespoke TBB DAG, not a persistent `VdfNetwork`.** 4× at 100k
   curves, 9–15× at 1M, 3–250× on sparse edits, 2× on memory, and it scales with threads where VDF
   does not. VDF's design point (rig point pools) is the inverse of a groom's.
2. **Curve buffers are structure-of-arrays internally** (`VtFloatArray px, py, pz`), interleaved to
   `VtVec3fArray` once at the Hydra boundary. Measured 25 → 81 GFLOP/s for the same arithmetic.
3. **Chunk granularity is 512 curves, always aligned to curve boundaries**, with one dirty byte per
   chunk per node.
4. **Every operator node owns its output buffer** (per-node caching is the default), which is what
   makes "edit the last styler" cost 0.2 ms instead of 1.8 ms and what makes the freeze/comb tool a
   configuration rather than a new mechanism. Budget ~6× the curve data in RAM for a 5-deep chain.
5. **Handoff to Hydra is `VtArray` copy-on-write**, not a custom sharing scheme. 0.00005 ms to
   publish, one 9.6 MB detach on the next edit.
6. **No hand-written SIMD intrinsics.** `-O3` autovectorises these kernels; the win is in layout, not
   in intrinsics, and usdRig's SSE2 kernel is dead code on ARM anyway.
7. **ExecUsd/VDF stays in the control plane** (parameters, chain wiring via relationship accessors,
   `ObjectsChanged` → dirty), exactly as A6 §8 proposed; nothing measured here changes that half.
8. If the plan nonetheless keeps a VDF path (e.g. to reuse usdRig's mover graph for guide curves),
   the six rules in §8 are mandatory, and rule 1 — element = one whole curve — is a correctness
   requirement, not a performance tip.

## Open questions

- **Why does VDF's parallel engine regress with thread count here?** The kernel is partly
  bandwidth-bound, but `VdfSimpleExecutor` (serial) matching the 20-thread parallel executor suggests
  contention in the data manager or the mask registry rather than bandwidth alone. Not chased; the
  decision does not depend on the answer, but anyone reviving the VDF path should profile
  `_ProcessOutput` / `_AbsorbPublicBuffer` (`parallelExecutorEngineBase.h:1949-1990`).
- **Where exactly does the 47 ms single-node re-run at 1M curves go?** Attributed to buffer
  preparation from the RSS evidence, but not isolated. `PEE_TRACE_SCOPE` is compiled out
  (`parallelExecutorEngineBase.h:43`) and cannot be re-enabled from a client TU.
- **Does the TBB DAG's advantage survive a real generator?** Everything measured here is a fixed-count
  styler chain. A generator that changes curve counts per frame (scatter density, LOD) reallocates
  buffers; neither prototype measures allocation-dominated frames.
- **Cross-chunk kernels.** Clumping to a *guide* curve owned by another chunk, and any neighbour
  search, are not modelled. The `UpstreamChunks` hook in §7 is a placeholder; its cost is unmeasured.
- **GPU.** Everything here is CPU. A GB10-class machine could plausibly run these kernels in HGI
  compute and skip the CPU chain entirely for playback, but Storm/HGI could not be exercised on this
  host (no display, GLX-only garch — see `ENVIRONMENT.md`).
- **Motion blur.** Neither prototype evaluates at multiple sample times; see
  `G-motion-blur-sampling-strategy.md`. The per-node cache design in §7 has no second time slot yet.
- **`VdfVector::Share()` thread safety in a live scene index.** It worked and the executor survived,
  but it is documented not thread safe and I only exercised it single-threaded between runs.
