# Execution engine

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document specifies usdGen's evaluator: the data model curves live in, the pure-value graph
description the evaluator consumes, how a graph is compiled and digested, what is captured once
versus evaluated per frame, how work is scheduled onto threads, how results are published to the
imaging layer, how motion samples are produced, the rules every kernel obeys, and the gates that
prove all of it. It is the engineering statement of ADR §4 and of `proposal-performance.md` §5,
with the fixes in ADR §4.2 applied. Everything here lives in the `usdGen` and `usdGenMath` targets,
which link **no `usd`, no `usdImaging`, no `hd`** (ADR §6, build), so every claim in this document
is testable with neither a stage nor a scene index.

Reads with: `01-architecture.md` (thesis, I1–I8, commit triggers, session model),
`02-schema.md` (prim types, property names and defaults, the dirty classes of its §6),
`04-operators.md` (per-operator parameters, capture columns, `Space()` per type),
`05-static-curves-and-deformation.md` (frozen re-entry, sculpt layers, the deform transport),
`06-imaging.md` (the scene index, the tile contract C2, notice emission),
`09-performance-and-benchmarks.md` (the full gate matrix and benchmark protocols),
`10-build-dependencies-testing.md` (targets, flags, test tiers T0–T4, the env-var registry),
`11-roadmap.md` (milestones M0–M8),
`appendix-A-evidence-ledger.md` (the `EV-nnn` rows every MEASURED number here cites).

---

## 0. Goals and the measured basis

### 0.1 What this document decides, and what it does not

Decides: buffer layout; chunk and tile geometry; the reference lane; `UsdGenGraphDesc` (including
`UsdGenCurveSetDesc`, the only way curve data reaches the engine); the four digests and the recompile
rule; the capture/evaluate split; the dirty router and the four-hop dirty path; the thread model;
the generation store and eviction; the motion cache; the kernel contract; the engine's C++ names;
gates E-1…E-8 and E-1r.

Does not decide: prim types and property names (`02-schema.md`), which operators exist and what
their parameters are called (`04-operators.md`), how a generation becomes Hydra prims and notices
(`06-imaging.md`), how maps and expressions are sampled (`07-look-maps-expressions.md`), or the
Python/C surfaces (`08-tools.md`).

### 0.2 The four goals, in the order they constrain the design

1. **A deform frame costs less than 2 ms of engine time at 100 000 curves × 8 CV.** The expected
   cost is **0.8–1.0 ms of a 15.8–16.0 ms frame — 5.0–6.3 %** — and 2.0 ms is the hard cap. Both
   figures are the ledger's and are cited, never restated here:
   `09-performance-and-benchmarks.md` §0.2 is the **single** frame ledger (R41), its usdGen
   aggregate is a DERIVED sum, and gates E-1…E-3 are what turn it into measurements. Storm owns
   most of the rest. Engineering spent past 2 ms buys nothing a viewer can see.
2. **An edit costs the edit, not the groom.** A terminal-parameter edit must stay in the
   0.18–0.41 ms class and a brush stroke in the 0.035–0.044 ms class (both MEASURED, `EV-004` and
   `EV-002`, §0.3).
3. **Capture never runs per frame.** Everything topology-dependent — scatter, stable ids, kd-trees,
   guide weights, clump ids, map/Ptex/SeExpr samples — is paid once per epoch (S25, ADR §4.1 I4, R1).
4. **The engine is testable with no Hydra and no USD.** The input is a pure value type; tier T0 is
   the fastest gate in CI (`10-build-dependencies-testing.md`).

The settled decisions this chapter executes are S21 (bespoke TBB DAG), S22 (SoA internally, AoS at
the Hydra boundary), S23 (512-curve curve-aligned chunks, one dirty byte per chunk per node), S24
(every node owns its output buffer; `VtArray` copy-on-write handoff), S25 (capture/evaluate split;
`restSpace | deformedSpace`) and S26 (explicit `usdGen:input`, Kahn order with a namespace
tie-break, cycles are compile errors, structural digest over structure only). Each is cited again
where it is executed.

### 0.3 The measured basis: bespoke TBB DAG (S21)

All numbers MEASURED on this host (aarch64, NVIDIA GB10 = 10× Cortex-X925 + 10× Cortex-A725,
20 cores, g++ 13.3.0, `research/ENVIRONMENT.md`), workload 100 000 curves × 8 CV = 800 000 CVs
(9.6 MB), five styler nodes, ≈16 flops/CV, prototype `prototypes/data-plane-benchmark/tbbBench.cpp`,
raw log `prototypes/data-plane-benchmark/results_main.txt`
(`research/G-data-plane-engine-prototype-benchmark.md` §4).

**Read the thread column first.** Every timing the prototype logged was taken in the **default
20-worker arena** (`prototypes/data-plane-benchmark/results_main.txt:57`, `# tbbBench … threads=20`),
but the design runs in a **private 8-thread arena** (§5.3, I8) where the same chain is **1.02 ms**.
Both are kept: 20 threads is the baseline the prototype produced; every budget derived from a chain
cost is derived from the 8-thread number and says so.

Every MEASURED cell names its `EV-nnn` row in `appendix-A-evidence-ledger.md` §2, which R1/R42
make the only citation handle for a measured number; the report section and raw log are kept
beside it so the derivation is checkable without a second lookup.

| Measurement | Threads | Value | Tag |
|---|:--:|---:|---|
| cold run, 5 ops, 100 k | 20 | 2.12 ms | MEASURED, `EV-001` |
| full run, 5 ops, 100 k | 20 | 1.72–1.91 ms | MEASURED, `EV-001` |
| **full run, 5 ops, 100 k** | **8** | **1.02 ms** | MEASURED, `EV-001` / `EV-008` (report §3.3) |
| 1 % of chunks dirty on all 5 nodes (contiguous) | 20 | 0.035–0.044 ms | MEASURED, `EV-002` |
| 1 % of chunks dirty, scattered | 20 | 0.036–0.040 ms | MEASURED, `EV-003` |
| last operator's parameter edited | 20 | 0.179–0.411 ms | MEASURED, `EV-004` |
| first operator's parameter edited | 20 | 1.751–1.969 ms | MEASURED, `EV-005` |
| handoff to Hydra (`VtArray` copy-construct) | 20 | 0.0000–0.0003 ms | MEASURED, `EV-010` (`results_main.txt:95-101`) |
| full run with one outstanding reference (one CoW detach of 9.6 MB) | 20 | 2.32 ms vs 1.97 ms | MEASURED, `EV-010` (report §3.4, §4) — see the note |
| full run, 1 M curves × 8 CV | 20 | 7.59–7.67 ms | MEASURED, `EV-006` |
| 0.5 % sparse edit, 1 M curves | 20 | 0.29 ms | MEASURED, `EV-006` |
| peak RSS, 1 M curves, per-node buffers | — | 668 MB | MEASURED, `EV-007` |
| peak RSS, 1 M curves, single shared buffer | — | 293 MB | MEASURED, `EV-007` |

Two cells cite the report's prose, not the raw log, because they do not reproduce from it. The
CoW-detach pair (2.32 vs 1.97 ms) is stated at
`research/G-data-plane-engine-prototype-benchmark.md` §3.4 and §4, while the log's own
`run_full_with_outstanding_ref_ms` reads **3.10 ms** for a differently-configured run
(`prototypes/data-plane-benchmark/results_main.txt:102`, re-verified 2026-09-05) and 1.97 ms
appears nowhere in the prototype outputs — **gate E-1 re-measures the detach inside the arena**.
`EV-010`'s own caveat column records the same disagreement between the report's prose and its
printed output. The handoff row's maximum is 0.0003 ms (`results_main.txt:95`), not 0.0001.

Thread scaling of the same chain, `PXR_WORK_THREAD_LIMIT` swept
(`research/G-data-plane-engine-prototype-benchmark.md` §3.3): **3.90 ms (1) → 3.01 (2) → 1.66 (4) →
1.02 (8) → 1.79 (20)**. MEASURED, `EV-008`. The 8-thread knee is the whole justification for I8's
private arena (§5.3); the default 20-worker arena is 1.75× slower than the knee on this host.

Chunk-size sweep (S23), 100 k curves, 20 threads (same report §4): **128 → 1.55 ms, 512 → 1.83,
1024 → 1.89, 4096 → 9.40, 16384 → 2.58**. MEASURED, `EV-009`. 4096 is a 5× cliff (25 chunks over 20
threads is a load-balance failure), which is why "widen chunks" is not a fallback for anything
(§10).

Layout (S22), single thread, 800 000 CVs, 16 flops/CV (same report §6, `simdBench.cpp`): AoS `GfVec3f`
**25.3 GFLOP/s**; SoA streamed 23.5; **SoA planar strip 80.9 GFLOP/s** (`EV-014`, `EV-015`,
`EV-016`). `-fno-tree-vectorize` gives 21.4 / 17.4 / 28.5 (`EV-017`), i.e. a 2.84× vectorisation win
for planar and 1.18× for AoS. MEASURED. g++ 13.3
`-O3` autovectorises all three kernels with 128-bit NEON (`-fopt-info-vec-optimized` on
`kernel.h:34`, `kernel.h:59`, `simdBench.cpp:38`); `-mcpu=native` does not switch to SVE.

### 0.4 The measured basis: why not a persistent `VdfNetwork` (S21)

Recorded because the decision is expensive to revisit and because two of the numbers are
correctness facts, not performance facts (`research/G-data-plane-engine-prototype-benchmark.md`
§2, §3, §5, §8).

| Measurement | VDF | TBB DAG | Tag |
|---|---:|---:|---|
| full chain, 100 k (strip element, request-all) | 3.80–8.32 ms | 1.72–1.91 ms | MEASURED, `EV-011` / `EV-001` |
| full chain, 1 M | 69.9–134.8 ms | 7.59–7.67 ms | MEASURED, `EV-011` / `EV-006` |
| single-node re-run / last-param edit, 1 M | 47.2–51.6 ms | 0.22–0.30 ms | VDF MEASURED, `EV-011`; TBB **DERIVED from `EV-004`** (scaled from 100 k in the report, `research/G-data-plane-engine-prototype-benchmark.md` §5) |
| 1 % sparse edit, 100 k | 0.652 ms + 0.060–0.095 ms reschedule | 0.035–0.044 ms | MEASURED, `EV-011` (report §3.1) / `EV-002` |
| 0.5 % sparse edit, 1 M | 72.0 ms (narrowed schedule, + 0.113 ms reschedule) | 0.29 ms | MEASURED, `EV-011` / `EV-006` |
| peak RSS, 1 M, per-node caching | 1 263 MB | 668 MB | MEASURED, `EV-011` / `EV-007` |
| thread scaling 1 → 8 | 3.53 → 5.79 ms (negative) | 3.90 → 1.02 ms | MEASURED, `EV-011` / `EV-008` |
| schedule cost for 1 000 styler nodes (2 200 total) | 2.93–4.39 ms | none (no scheduling phase) | MEASURED, `EV-013` |

Two of these are structural, not tunable. (1) `VdfScheduler` splits every buffer-passing pool
output into `ceil(n/500)` invocations with a hardcoded grain (`pxr/exec/vdf/scheduler.cpp:861`,
verified). 500 elements is 62.5 curves at 8 CV, so the grain **does not respect curve boundaries**:
a kernel that reads a curve's root to style its tip races a concurrent invocation, and a node that
ignores its invocation mask is quadratic (854 ms vs 7.60 ms for the same chain, MEASURED,
`EV-012`).
(2) Scheduled affects masks are frozen at schedule time, so a 1 % `InvalidateValues` is cheap
(13–34 µs) but the following `Run` still touches all 4 000 000 elements; sparse execution needs a
second, narrowed schedule, which pays off only below ~200 k curves, MEASURED.

usdGen therefore owns ~200 lines of scheduling instead of inheriting `VdfScheduler`. ExecUsd/VDF
remains the control plane elsewhere in the pipeline (S16); curve arrays never go through it.

### 0.5 Status vocabulary and where the numbers come from

MEASURED = measured on this host, with its `EV-nnn` ledger row and the report section or prototype
named. DERIVED = scaled or interpolated from a MEASURED number, tagged `DERIVED from EV-nnn` and
never presented as measured. UNMEASURED = a number this plan
needs and does not have, with the gate that will produce it. ASSUMPTION = a decision taken without
evidence, marked so it can be attacked first. Every number in this document carries one of the four
(R42). Storm-side numbers are not the engine's and live in `09-performance-and-benchmarks.md`.

Two vocabulary rules that are easy to trip over. **`I1`–`I8` are the engine invariants** (R1,
renamed from `proposal-performance.md`'s P1–P8); **`P0`/`P1`/`P2` are the motion profiles** of §7
(S32) and nothing else. And R1/R42 make `EV-nnn` — a row of `appendix-A-evidence-ledger.md` §2 —
the only citation handle for a measured number. That ledger carries **`EV-001`…`EV-083`** (read
2026-09-05), so every MEASURED cell in §0.3, §0.4 and §5.5 names its row; the report section and
raw log stay beside it because they are where the row's derivation is checkable. No number in this
document carries a retired `Q<n>` handle.

---

## 1. Data model

### 1.1 Planar SoA per CV, AoS per curve (I1, S22)

Per-CV channels are **planar** `VtFloatArray`s (`px`, `py`, `pz`, `width`, `hairT`, named extras).
Per-*curve* channels stay AoS (`VtVec2fArray rootUV`, `VtVec3fArray rootT/rootN/rootB`,
`VtUInt64Array curveId`, …). The split is not aesthetic: planar buys 3.2× on the styler kernels
(80.9 vs 25.3 GFLOP/s, MEASURED §0.3), and per-curve data is 1/8 the volume and is read once per
curve at random, where AoS wins. Interleaving to `VtVec3fArray` happens exactly once, at
publication (§6.3).

Everything is a `VtArray`, so the boundary to the imaging layer and to Python is a refcount bump
(0.00005 ms MEASURED; 0.13–0.18 µs across a pxr_boost boundary at any size,
`research/G-tool-loop-array-transport-and-cv-picking.md` §1.2).

`hairT` is **stored**, not recomputed, because it is also a published primvar (S29) and computing
`i/(n-1)` in two places is a divergence risk. 3.2 MB at 800 k CVs.

No tangent planes are stored: ADR §5.4's default (variant A) publishes no `hairTangent`, and
variant B derives it from `points` at interleave time. `tx/ty/tz` per node would cost 9.6 MB per
node for data the default look never reads (§12.1 deviation 3).

### 1.2 The buffers

```cpp
// usdGen/types.h
using UsdGenNodeId    = uint32_t;          // dense, assigned by the compiler in topological order
using UsdGenChunkId   = uint32_t;          // dense within a description
using UsdGenTileId    = uint16_t;
using UsdGenSurfaceId = uint16_t;          // index into the graph's surface table (§2.2)
using UsdGenEpoch     = std::array<uint64_t, 2>;   // 128-bit digest

enum class UsdGenSpace    : uint8_t { Inherit, Rest, Deformed };   // Inherit <=> the token `auto` (R9)
enum class UsdGenTopoFx   : uint8_t { None, CurveCount, CvCount, Both };  // CurveCount == R14's MayChangeCurveCount
enum class UsdGenRole     : uint8_t { Curves, Reference };         // Reference = guides, clump centres, card roots
enum class UsdGenReadPhase: uint8_t { Base, Preceding, Final, Explicit };  // Preceding resolves to Final in v1 (R9)
```

```cpp
// usdGen/curveBuffer.h
/// A named planar channel. Per-CV planes have totalCvs entries, per-curve planes totalCurves.
/// The payload is typed because ADR §2.3 mandates integer emitted primvars alongside float ones:
/// `clumpId_<level>` is a uniform `int`, `guideIndex` a uniform `int[]` with `elementSize = 3`,
/// `guideWeight` a uniform `float[]` with `elementSize = 3` (R24 -- `int[3]` / `float[3]` are not
/// USD type names; §6.3 step 5 states the published form). Exactly one of `f` / `i` is populated.
struct UsdGenPlane {
    TfToken      name;            // "clumpId_0", "guideIndex", "guideWeight", "displayColor:r", ...
    TfToken      interpolation;   // vertex | uniform | constant
    TfToken      type;            // float | int
    uint8_t      arity = 1;       // 3 for guideIndex / guideWeight (a host renderer arity, ADR §2.3)
    VtFloatArray f;
    VtIntArray   i;
};

struct UsdGenChunkDesc {
    uint32_t        firstCurve;    // index into the per-curve arrays
    uint32_t        curveCount;    // allocated curves, <= chunkSize
    uint32_t        liveCount;     // curves surviving density decimation; <= curveCount
    uint32_t        firstCv;       // index into the per-CV arrays
    uint32_t        cvCount;       // CVs per curve; 0 == ragged chunk (use cvOffsets)
    UsdGenTileId    tile;
    UsdGenSurfaceId surface;
    GfRange3f       boundsRest;    // filled at capture; brush/footprint rejection, tile extents
};

/// One node's output. Per-CV data planar (I1); per-curve data AoS.
struct UsdGenCurveBuffer {
    // ---- per CV ---------------------------------------------------------------
    VtFloatArray px, py, pz;                 // size == totalCvs
    VtFloatArray width;                      // empty == inherit upstream
    VtFloatArray hairT;                      // root->tip parameter, written once by the generator
    std::vector<UsdGenPlane> extraCv;        // named per-CV planes, sorted by name

    // ---- per curve ------------------------------------------------------------
    VtUInt64Array curveId;                   // primvars:usdGen:curveId (uint64[], uniform, R12):
                                             //   UsdGenHash64(seed, faceIndex, k, kSaltScatter), §1.7
    VtIntArray   rootPrim;                   // surface face index (== primvars:skinprim)
    VtVec2fArray rootUV;                     // == primvars:skinprimuv, and the "st" primvar
    VtVec3fArray rootT, rootN, rootB;        // rest root frame, 36 B/curve
    VtIntArray   cvOffsets;                  // size totalCurves+1; present iff any chunk is ragged
    VtFloatArray curveMask;                  // resolved per-curve mask for the owning node
    std::vector<UsdGenPlane> extraCurve;     // named per-CURVE planes, sorted by name:
                                             //   clumpId_<level> (uniform int, arity 1),
                                             //   guideIndex (uniform int, arity 3),
                                             //   guideWeight (uniform float, arity 3)

    // ---- geometry -------------------------------------------------------------
    std::vector<UsdGenChunkDesc> chunks;     // surface-major, then Morton order
    uint32_t totalCurves = 0, totalCvs = 0;
    uint64_t topologyVersion = 0;            // bumped when curve or CV counts change
    uint64_t valueVersion    = 0;            // bumped by every write to this buffer
};

/// What a kernel is allowed to touch. All pointers are chunk-local bases.
struct UsdGenChunkView {
    const UsdGenChunkDesc *desc;
    float       *px, *py, *pz;               // out: writable
    const float *inPx, *inPy, *inPz;         // in: read-only, never const_cast
    const float *width, *hairT;
    const uint64_t *curveId;
    const int   *rootPrim;
    const int   *cvOffsets;                  // null on the uniform fast path
    const GfVec2f *rootUV;
    const GfVec3f *rootT, *rootN, *rootB;
    const float *curveMask;                  // resolved once at capture; may be null == 1.0
    const float *rampLut;                    // 257 entries over hairT, or null
    // Extra planes this node declared in OutputPrimvars(), bound by slot at compile.
    // Per-curve slots have curveCount entries * arity; per-CV slots have curveCount * cvCount.
    float       **outF;                      // writable float planes,   null when the node emits none
    int         **outI;                      // writable int planes
    const float **inF;                       // upstream planes this node reads
    const int   **inI;
    uint32_t      outCount, inCount;
    uint32_t curveCount, cvCount;            // cvCount == 0 on the ragged path
    inline size_t Cv(uint32_t c, uint32_t i) const { return size_t(c) * cvCount + i; }
    inline size_t CvRagged(uint32_t c, uint32_t i) const { return size_t(cvOffsets[c]) + i; }
    inline float *OutF(uint32_t slot) const { return outF[slot]; }
    inline int   *OutI(uint32_t slot) const { return outI[slot]; }
    inline const float *InF(uint32_t slot) const { return inF[slot]; }
    inline const int   *InI(uint32_t slot) const { return inI[slot]; }
};

/// A read-only window over one tile's chunk range, used by the interleaver (§6.3).
struct UsdGenTileView {
    UsdGenTileId tile;
    uint32_t     firstChunk, chunkCount;
    uint32_t     totalLiveCurves, totalLiveCvs;
    GfRange3f    extent;                      // written by the interleave pass
};
```

`liveCount` is an addition to `proposal-performance.md` §5.1 and it is load-bearing. R13 makes both
density **scales** decimate by stable id:

```
keep(curve) iff UsdGenHash32(curveId, kSaltDensity) < keepFraction * 2^32
keepFraction = clamp(scale_groom * scale_description, 0, 1)
```

`scale` is `usdGen:densityScale` in the **interactive** context and `usdGen:renderDensityScale` in
the **render** context; the two contexts are **exclusive** and neither scale applies in the other,
and both default 1 (R13). `kSaltDensity != 0`, so the surviving set is never `{hairId < scale}` —
that is exactly why R12 salts it, since `hairId = UsdGenHash32(curveId, 0) / 2^32` would otherwise
correlate decimation with shading. A `keepFraction` above 1 clamps with one `TF_WARN`. Ids, sculpt
deltas and clump ids therefore survive every scale change: the chunk partition is computed once per
capture over the **full** id set and decimation only lowers `liveCount` inside existing chunks
(R13).

Which knob was turned matters, because the two cost different things. A **density-scale** change
(`usdGen:densityScale` or `usdGen:renderDensityScale` on Groom/Description) never re-chunks, never
re-scatters and never invalidates a capture cache — it only moves `liveCount` (R13). A
`usdGen:density` edit on a *generator* (hairs per square stage unit on the rest surface) changes how
many roots exist and **is** a capture-epoch bump (§2.3, §3.6).

### 1.3 Chunk = 512 curves; the uniform-CV fast path and the ragged path (I2, S23, ADR §4.2.2)

A chunk is `USDGEN_CHUNK_SIZE` curves, default **512**, clamped to [128, 1024], **always aligned to
curve boundaries**, with one dirty byte per chunk per node. The sweep in §0.3 is the whole
justification, and the curve alignment is the property VDF cannot give (§0.4).

Two paths:

* **Uniform-CV fast path** (`desc.cvCount != 0`). Every curve in the chunk has the same CV count;
  the inner loop index is a multiply, which is what the 2.84× vectorisation win depends on. Every v1
  generator (`UsdGenScatter` → `UsdGenGrow`, `UsdGenGuideInterpolate`) produces uniform chunks.
* **Ragged path** (`desc.cvCount == 0`, `cvOffsets` non-empty). `UsdGenCurveSource` (imports, sim
  caches, frozen curves) is v1 and ragged by nature, so the ragged path ships in v1 — it is not an
  optimisation deferred to later. Kernels take both paths from one source file via a
  `USDGEN_FOR_EACH_CV` macro that expands to the multiply form and the offset form; there is exactly
  one kernel body per operator.
* The import tool offers "resample to N CVs" (`UsdGenResample` is v1, ADR §4.2.2) and shows the
  measured penalty. **Gate E-1r** measures that penalty and holds it at ≤ 2× the uniform path.
  UNMEASURED until E-1r (M2).

Chunks whose curves span more than one surface are not created: chunking is **surface-major**, so a
surface dirty maps to a contiguous chunk *range* rather than a scan (§5.2, hop 2). Within a surface,
roots are Morton-ordered (§1.7).

### 1.4 Tiles: the Hydra prim granularity (I5, ADR §1 S23)

The engine chunk and the Hydra prim are different granularities. S23 wants 512-curve chunks for the
DAG; S27 wants 32–256 prims for Storm; 512-curve chunks at 1 M curves is 1 954 chunks, which cannot
also be ≤ 256 prims. The reconciliation is an integer:

```
chunkSize     = USDGEN_CHUNK_SIZE                         # default 512, clamped [128, 1024]
nChunks       = ceil(maxCurves / chunkSize)
chunksPerTile = max(1, ceil(nChunks / usdGen:tileTarget))  # tileTarget default 64, clamped [32,256]
nTiles        = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))
```

Worked, at `chunkSize = 512`, `tileTarget = 64`:

| maxCurves | nChunks | chunksPerTile | nTiles | curves/tile |
|---:|---:|---:|---:|---:|
| 10 000 | 20 | 1 | 20 | 512 |
| 100 000 | 196 | 4 | 49 | 2 048 |
| 250 000 | 489 | 8 | 62 | 4 096 |
| 1 000 000 | 1 954 | 31 | 64 | 15 872 |
| 8 000 000 | 15 625 | 245 | 64 | 125 440 |

`maxCurves` is the terminal node's curve count with **all** density scales at 1, not the current
count (performance ASSUMPTION 6): the prim set is allocated once and never changes during
interaction (S27, S28). `usdGen:tileTarget` is `uniform int` on `UsdGenDescription`, default 64,
because the prim set must be reproducible across sessions (ADR §2.3); `chunkSize` is env, never
authored.

The arithmetic is **ADR §9.3 R21 verbatim**, including the `min(nChunks, …)` term and the worked
numbers (100 k curves, chunk 512, `tileTarget` 64 → 196 chunks, 4 chunks per tile, 49 tiles; 64
tiles is the 1 M figure). The `min` matters because the lower clamp of 32 is not reachable when a
description has fewer than 32 chunks, and an empty tile prim would publish a zero-length `points`
array for the life of the session. Grooms under 16 384 curves therefore publish fewer than 32 tiles.
`usdGen:tileTarget` is not a node-digest term; a change routes as `UsdGenDirtyTopology` on the
description (R21, §3.6).

### 1.5 The reference lane (I3)

**Hair chunks never read hair chunks.** Every cross-curve influence flows through a *reference set*:

* A node whose `Role()` is `UsdGenRole::Reference` — guide sets, clump centres, card/archive source
  roots — produces a buffer that is **not chunked for dirtiness**. It is small — **1–10 % of hairs**: a host groomer
  and a DCC IGS both default to creating guides at 10 % of hair density
  (`research/A7-prior-art-grooming.md` §9.3, and §1.5 for a DCC IGS's "guides at 10% default hair
  density"; R26 says design and budget for 10 %), and gate E-4's fixture is 4 000 guides against
  100 000 roots, i.e. 4 %. At 10 % the reference lane costs about 10 % of a chain run, ≈ 0.1–0.2 ms
  at 100 k (DERIVED from §0.3's 1.02 ms @8 threads), which is why it is affordable as a **fence**
  rather than a per-chunk dependency. It is always evaluated **to completion** before any node that
  names it in `ReferenceInputs()`, and its own `Capture()` runs first (§5.4 step 2).
* A consuming node's `Capture()` resolves *which* reference curves each hair uses and stores plain
  index/weight arrays. `Evaluate()` then does a random read into the reference buffer, which is
  read-only for the whole parallel region.

This removes, rather than contains, the one cost the data-plane report left open ("`UpstreamChunks`
is a placeholder; its cost is unmeasured",
`research/G-data-plane-engine-prototype-benchmark.md` open questions). There is no fan-in set, no
chunk-dependency graph, and no read-write hazard inside `tbb::parallel_for`.

Reference nodes have their own small topological order, because level *L* of a multi-level
`UsdGenClump` may consume level *L−1*'s output as its centres. Level barriers are dispatcher fences,
not per-chunk dependencies.

```cpp
struct UsdGenReferenceSet {                 // one per reference node, per generation
    UsdGenCurveBuffer buffer;               // evaluated in full, never chunk-dirty
    VtFloatArray      localX, localY, localZ;  // root-local offsets, refreshed per frame
    VtFloatArray      guideBlend;           // per-guide usdGen:blend, a host groomer range of influence
    uint64_t          generation;
};
```

A reference node's own input is not another operator: it is a set of authored `BasisCurves` under
contract C3 (a `UsdGenGuideSet`'s children, a freeze, an import). Those arrive in the engine as
`UsdGenCurveSetDesc` entries in `UsdGenGraphDesc::curveSets` (§2.2, R23) — the reference node's
`Capture()` reads them and nothing else.

`localX/Y/Z` is what makes `UsdGenGuideInterpolate`'s inner loop three multiply-adds over
contiguous planar data: no matrix inverse, no tree, no per-hair frame reconstruction beyond the
hair's own root frame.

### 1.6 Spaces, and the motion tail

`Space()` partitions the topologically sorted node list into a **rest head** `H` and a **deformed
tail** `T` (S25, I4). This document owns that arithmetic and states it once, in the
**downstream-closure** form; `04-operators.md` §0.7 and `05-static-curves-and-deformation.md` §4.4
cite this paragraph rather than restate it.

> `T` = the **first** node in topological order whose resolved space is `UsdGenSpace::Deformed`,
> **and every node at or downstream of it** in the compiled DAG. `H` = every other node.

The closure term is load-bearing, because the two candidate readings differ. "Split at the last
`UsdGenSpace::Rest` node" — `design/proposal-performance.md` §5.8's wording — agrees with the
closure only while spaces are monotone along the chain, and they need not be: a rest-class
`UsdGenNoise` placed *after* a `UsdGenDeform` keeps its rest-class **arithmetic** (the `Inherit`
rule below) but its input changes every frame, so it belongs in `T`. Closure is what makes "a frame
change dirties only `T`" exactly true instead of approximately true, and it is why the split is
computed over the sorted node list rather than by scanning for a boundary node.

`usdGen:space` has exactly **three** tokens (R9, `02-schema.md` §2.5): `rest → UsdGenSpace::Rest`,
`deformed → ::Deformed`, `auto → ::Inherit`. **There is no `world` token**: after flattening,
deformed space *is* world space (S4, R9), so a v3 `UsdGenWind` that wants world coordinates is
simply a `deformed`-class operator. The authored default is **`auto`**, which resolves to
`UsdGenSpace::Inherit`, and `Inherit` means **the operator type's own declared class** — the value
its `UsdGenOp::Space()` returns, i.e. its `restSpace | deformedSpace` classification (S25). It does
**not** mean "take the class of the input": the `UsdGenNoise` above stays rest-class and keeps
computing in the hair's rest root frame; only its *schedule* moves into the tail.

Consequences, all of which are why §7 is cheap: a frame change dirties only `T`'s chunks; motion
profile P2 costs `H_cached + k·T + (k−1)·m` instead of `k·(H+T)`
(`research/G-motion-blur-sampling-strategy.md` §4.3, §5). usdRig has no such lever and re-runs the
whole rig per offset (`usdRig libs/rigExecImaging/bridge.cpp:1284-1288`, verified — the loop calls
`_evaluator->Evaluate(sampleTime)` once per shutter offset; `research/A1-usdrig-graph.md`).

`ReadPhase()` selects which sample of the bound surface an operator reads (S26, retained from
usdRig's read phases). Resolution:

| Authored `usdGen:readPhase` | `UsdGenReadPhase` | Engine reads |
|---|---|---|
| `base` | `Base` | the rest surface (`usdGen/rest/points` from the `UsdGenRestAPI` adapter, S12); time-independent |
| `preceding` | `Preceding`, resolved to `Final` at compile | v1 **alias for `final`** (R9), not a distinct phase. Retained so usdRig-shaped assets load; the compiler rewrites it to `UsdGenReadPhase::Final` and emits one `TF_WARN` naming the prim on first use. `02-schema.md` §2.5 declares the same alias and records that the tool never authors it |
| `final` (**authored default**) | `Final` | the deformed surface at `time + shutterOffset` |
| `@<absolute prim path>` | `Explicit` | that surface prim, at the phase its own `Space()` implies |

The authored default on `UsdGenOperator` is **`final`**, not `preceding` (`02-schema.md` §2.5).
`UsdGenOp::ReadPhase()` (§8.1) returns the *type-level fallback*, used only when a `UsdGenNodeDesc`
carries no `readPhase` token at all (a T0 literal that omits it); its own default is **`Final`**, so
the type-level fallback and the authored default (R9) can never disagree, and an authored token
always wins. `UsdGenReadPhase::Explicit` is selected when the token begins with `@`; the remainder
is parsed as an `SdfPath` and must be absolute, or the node is a compile error naming the prim.

Read phases are part of the structural digest (§3.3): changing one changes which buffer a node
reads, which is a graph edge, not a value.

### 1.7 Morton-sorted roots and surface-major chunk order

At capture, curve slots are ordered first by `UsdGenSurfaceId`, then by a 21-bit-per-axis Morton
code over the **rest-space** root position. Two properties follow, both of which other parts of the
design assume:

* A chunk is a spatial cluster, so a tile's `extent` is tight enough for Storm's frustum culling to
  reject something (S29 requires the extent; nothing has measured its tightness — gate **S-4**
  does, in M3: `itemsDrawn` must drop ≥ 3× when the camera frames a quarter of the groom), and a
  brush footprint touches O(1) chunks instead of every chunk (S40).
* A surface dirty is a contiguous chunk range, not a scan (§5.2 hop 2).

Stable ids are **independent** of the ordering. `curveId` is **64-bit** — `uint64[]
primvars:usdGen:curveId`, `uniform` (R12) — and is minted by a pinned hash, not by a bit packing:

```
UsdGenHash64(seed, faceIndex, k, salt):
    key = canonical 64-bit packing of (seed, faceIndex, k)     # lengths before bytes, fixed width
    x   = key ^ (uint64(salt) * 0x9E3779B97F4A7C15)            # per-use compile-time salt
    z   = x + 0x9E3779B97F4A7C15                               # SplitMix64 finalizer, pinned by R12
    z   = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9
    z   = (z ^ (z >> 27)) * 0x94D049BB133111EB
    return z ^ (z >> 31)

curveId               = UsdGenHash64(seed, faceIndex, k, kSaltScatter)
UsdGenHash32(v, salt) = high 32 bits of UsdGenHash64 over v with that salt
```

`faceIndex` is a **description-wide** index: each bound surface gets a disjoint base offset,
assigned at capture in `UsdGenSurfaceId` order, so two surfaces never feed the hash the same
`(faceIndex, k)`; `k` is the root's per-face ordinal. Salts (`kSaltScatter`, `kSaltDensity`,
`kSaltNoise`, …) are per-use compile-time constants, and **changing the hash or any salt bumps
`usdGen:schemaVersion`** (R12).

Uniqueness is therefore a probability, not a construction: at 8 M curves the chance that any pair
collides over 2^64 is ≈ 1.7e-6 (**ASSUMPTION**, birthday arithmetic `n²/2^65`, not a measurement).
The duplicate-id check of `05-static-curves-and-deformation.md` §2.3 row 10 stays a **hard error**,
so a collision is named and reported rather than silently making sculpt deltas ambiguous, and T0
test 8b (§13.1) scatters 8 M curves on a 200 k-face scalp and asserts uniqueness.

A fixed-width hash has **no width parameter**, and that is load-bearing. The 31-bit packing this
document carried in its first draft reserved `B = ceil(log2(maxPerFace + 1))` bits for `k`, and
`maxPerFace` is a function of `usdGen:density` — so every density edit would have changed `B` and
renamed **every** curve in the description, breaking sculpt deltas, clump ids and frozen epochs
keyed by id. That is the third independent reason R12 rules the packing out, alongside the 32-bit
collision rate and the correlation between `hairId` and decimation.

The seed enters the hash directly, so a re-seed deliberately renames every curve (what §11.1 says a
seed edit costs), while a density-**scale**, LOD, chunking or Morton-order change renames nothing
(`research/A7-prior-art-grooming.md` §9.1). `hairId = UsdGenHash32(curveId, 0) / 2^32` (ADR §1 S29,
R12) is a shading float in [0,1) and may collide harmlessly; decimation uses `kSaltDensity ≠ 0`, so
the surviving set is never `{hairId < scale}` (R12, §1.2).

ASSUMPTION: Morton ordering is worth its capture cost; it is a sort over `totalCurves` keys and is
bounded by the scatter it follows.

### 1.8 Memory, accounted

At 100 000 curves × 8 CV (800 000 CVs, 9.6 MB of positions), five nodes, one guide set:

| Item | Arithmetic | Bytes | Tag |
|---|---|---:|---|
| per-node position buffers, 5 nodes × 9.6 MB | 5 × 800 k × 12 B | 48 MB | DERIVED (layout) |
| `hairT`, one shared copy | 800 k × 4 B | 3.2 MB | DERIVED (layout) |
| `width` where written (2 nodes) | 2 × 800 k × 4 B | 6.4 MB | DERIVED (layout) |
| per-curve arrays, one copy: `curveId` **8 B** (`uint64`, R12) + `rootPrim` 4 + `rootUV` 8 + root frame 36 + resolved mask 4 | 100 k × 60 B | 6.0 MB | DERIVED (layout) |
| capture caches: `guideIdx[3]`+`guideW[3]` 24 B/curve, `clumpId` 4, ramp/mask bookkeeping 8 | 100 k × 36 B | 3.6 MB | DERIVED (`09-performance-and-benchmarks.md` §2.6) |
| published generation (interleaved AoS, one copy; two with the optional M7 publish ring) | 800 k × 12 B | 9.6–19.2 MB | DERIVED (layout) |
| **total** | 48 + 3.2 + 6.4 + 6.0 + 3.6 + (9.6–19.2) | **77–86 MB** | DERIVED — **≈ 8–9×** the raw positions |

The composition and the total are `09-performance-and-benchmarks.md` §2.6's per-element model
restated at this fixture, not re-derived, so the two cannot drift; 09 §2.6 also carries the second
accounting (the model's other planes added to `EV-007`'s measured 668 MB anchor at 1 M gives
950–1050 MB DERIVED) and the reason **E-5 is at risk**. Two terms this five-node chain also carries
and the per-element model does not count are per-**segment** rest lengths for `preserveLength`
(7 × 4 B per curve ≈ 2.8 MB) and one resolved `curveMask` per node beyond the first (≈ 0.4 MB
each); both are DERIVED, both are eviction candidates (§6.4), and E-5 is where they appear or do
not.

Every row is arithmetic over the layout above, not a measurement. The only MEASURED anchor is the
whole-process figure: the same shape at 1 M curves × 8 CV with 5 stylers peaks at **668 MB RSS**
(293 MB with a single shared buffer), MEASURED, `EV-007`,
`research/G-data-plane-engine-prototype-benchmark.md` §5.

**8–9× is over S24's "≈ 6× the curve data" budget, and that is admitted, not hidden.** Per-node
buffers are what buy the 0.18–0.41 ms parameter edit (§0.3); the excess is held down by §6.4's
eviction pass under `USDGEN_MEMORY_BUDGET_MB`, and gate **E-5** is what fails if it is not.

---

## 2. `UsdGenGraphDesc`: the engine's input

### 2.1 Why a pure-value description

`usdGenImaging` builds a **pure value** description from Hydra data sources and hands it to the
engine; the engine never sees `HdSceneIndexBase`, `UsdStage`, `UsdPrim` or an `SdfLayer`
(risk §4.3, ADR §4.2.3). Three things fall out:

1. S8 ("no design element may require a `UsdStage` downstream of the stage scene index") is enforced
   at **link time**, not by review: the `usdGen` target links `tf gf vt sdf ar work trace hio
   pxOsd` and nothing else (R37, `10-build-dependencies-testing.md`). It is asserted by gate
   **B-1** (T0, M0), which checks `DT_NEEDED` of `libusdGen.so` against the forbidden list `usd*`,
   `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`, `usdAppUtils` (R37).
2. Tier T0 tests construct a `UsdGenGraphDesc` in C++ literal form and run the whole engine with no
   chain, no adapter and no stage — sub-millisecond gates, run on every commit.
3. The alternative — a façade over the live scene index — would leak a scene-index dependency into
   the evaluator one `GetPrim` at a time.

### 2.2 The structs

```cpp
// usdGen/graphDesc.h
struct UsdGenParamValue {                  // one authored usdGen:* property, resolved
    TfToken            name;               // "clump:size", "mask:ramp:knots", "surface"
    VtValue            value;              // scalar, token, array, or resolved asset path
    bool               animated = false;   // authored with a .spline / time samples
};

struct UsdGenRampDesc {                    // S11 encodings, already resolved by the adapter
    VtVec2fArray knots;                    // scalar ramp: (position, value)
    VtFloatArray positions;                // colour ramp
    VtVec3fArray colors;
    TfToken      interpolation;            // linear | catmullRom | bspline | constant
                                           //   (default catmullRom, R11; 02-schema.md §2.17)
};

struct UsdGenNodeDesc {
    SdfPath                      path;         // the operator prim's scene path (identity + tie-break)
    TfToken                      type;         // "UsdGenClump", "UsdGenScatter", ...
    TfToken                      mode;         // usdGen:mode, when the type has one
    int                          algorithmVersion = 0;   // 0 == "latest kernel" (R17); the tool,
                                                        //   freezes and bakes always author the
                                                        //   explicit current version
    bool                         enabled = true;
    int                          seed = 0;
    float                        blend = 1.0f;
    TfToken                      space;        // auto | rest | deformed (auto == the type's Space(), R9)
    TfToken                      readPhase;    // base | preceding | final | @<absolute prim path>
    SdfPathVector                inputs;       // usdGen:input targets, authored order
    SdfPathVector                references;   // guide sets, clump centres, card roots
    SdfPathVector                curves;       // usdGen:guides, usdGen:curves, usdGen:frozen:curves
                                               //   -> indices into UsdGenGraphDesc::curveSets
    SdfPathVector                surfaces;     // usdGen:surface targets (Mesh or GeomSubset)
    SdfPathVector                maps;         // usdGen:mask:source, per-parameter map targets
    std::vector<UsdGenParamValue> params;      // EVERY mapped locator of this prim (see 2.5)
    std::vector<UsdGenRampDesc>   ramps;
};

/// One authored C3 `BasisCurves` input: a UsdGenGuideSet child, a freeze, an import, a sim cache.
/// Related to `05-static-curves-and-deformation.md` §2.2's `UsdGenSourceArrays` by construction, not
/// by field identity (§4.5): that struct is the loader's argument and carries the array subset only.
/// This is the only way curve data reaches the engine. Without it neither the reference lane (§1.5)
/// nor `UsdGenCurveSource` (§4.5) has an input. Named per R23.
struct UsdGenCurveSetDesc {
    SdfPath         path;
    UsdGenRole      role;              // Reference (guides, clump centres) | Curves (a CurveSource)
    TfToken         curveRole;         // primvars:usdGen:role: hair | guide (C3 marker)
    VtIntArray      curveVertexCounts;
    VtVec3fArray    points, rest;      // rest == primvars:rest; may share points' buffer
    VtFloatArray    widths;
    VtIntArray      skinPrim;          // primvars:skinprim (uniform int)
    VtUInt64Array   curveId;           // primvars:usdGen:curveId (uniform uint64[], R12)
    VtVec2fArray    skinPrimUv;        // primvars:skinprimuv (uniform texCoord2f, not "st")
    VtMatrix4dArray rootFrame;         // primvars:usdGen:rootFrame; may be empty
    std::string     frozenEpoch;       // constant string primvar, "usdgen1:sha1:..." (S42)
    VtFloatArray    guideBlend;        // per-guide usdGen:blend on UsdGenGuideSet (ADR §2.3)
    uint64_t        curveGeneration;   // bumped by any points/topology/id change on the prim
};

struct UsdGenSurfaceSample { double time; VtVec3fArray points; };   // R23

struct UsdGenSurfaceDesc {
    SdfPath        path;
    UsdGenSurfaceId id;
    VtIntArray     faceVertexCounts, faceVertexIndices;
    VtVec3fArray   restPoints;        // usdGen/rest/points (S12), UsdTimeCode::Default()
    VtVec3fArray   points;            // deformed, at UsdGenGraphDesc::time
    /// Sorted by time; `samples[0].time == UsdGenGraphDesc::time` -- the current-time sample is
    /// always index 0 (R23). A single entry == static over the shutter. Filled by usdGenImaging from
    /// the surface's own GetContributingSampleTimesForInterval when
    /// usdGen:motion:forwardSurfaceSamples is true, else from the clamped set {open, 0, close}
    /// (research/G-motion-blur-sampling-strategy.md §4.2, §5). This is the ONLY source of surface
    /// geometry at a non-current time; §7.2 interpolates it for offsets that fall between samples.
    std::vector<UsdGenSurfaceSample> samples;
    VtVec3fArray   velocities;        // motion profile P1 only (v1, M7); empty otherwise
    VtVec2fArray   uv;                // the surface's primary uv set
    VtIntArray     subsetFaces;       // empty == whole mesh; a GeomSubset restricts scatter
    GfMatrix4d     worldMatrix;       // post-flattening, resetXformStack (S4)
    uint64_t       surfaceGeneration; // bumped by any points/topology change
};

struct UsdGenMapDesc {
    SdfPath      path;
    TfToken      type;                // UsdGenImageMap | UsdGenPtexMap | UsdGenExprMap | ...
    std::string  resolvedAssetPath;   // stage-free (S13)
    uint64_t     textureGeneration;   // bumped by ReloadMaps(); see 07-look-maps-expressions.md
    std::vector<UsdGenParamValue> params;
};

struct UsdGenGraphDesc {
    SdfPath                        description;   // the UsdGenDescription prim
    SdfPath                        terminal;      // usdGen:terminal's single target
    std::vector<UsdGenNodeDesc>    nodes;         // namespace order, for the Kahn tie-break
    std::vector<UsdGenCurveSetDesc> curveSets;    // every C3 BasisCurves the graph names (R23)
    std::vector<UsdGenSurfaceDesc> surfaces;
    std::vector<UsdGenMapDesc>     maps;
    float    densityScale = 1.0f, renderDensityScale = 1.0f;
    int      tileTarget = 64;
    TfToken  motionMode; int motionSampleCount = 3; bool forwardSurfaceSamples = false;
    int      schemaVersion = 1;
    double   time = 0.0;
};
```

### 2.3 `TopologyParameters()` vs `ValueParameters()` (risk §4.2)

Every `UsdGenOp` classifies its own parameters into exactly two disjoint sets:

| Set | Meaning | Cost of an edit |
|---|---|---|
| `TopologyParameters()` | edit changes what capture produced — `usdGen:density` (never the density *scales*), seed-dependent counts, CV counts, guide selection radius, map bindings, mask sources | bump the node's capture epoch, re-capture the node and its descendants, re-evaluate them (§3.4) |
| `ValueParameters()` | edit changes only per-frame arithmetic — amounts, ramps, widths, noise magnitude, blend | mark the node's chunks and its descendants' chunks dirty, re-evaluate (§5.2) |

The union must equal the node's mapped property set; a debug build asserts it
(`USDGEN_OP_CHECKS=1`, default 0, registered in R35's single env-var registry,
`10-build-dependencies-testing.md` §3.5). A parameter in neither set is a silent staleness bug,
which is why the assertion exists rather than a comment.

### 2.4 Relationship targets, maps and surfaces are resolved buffers

The description carries **values**, never handles. `usdGen:surface` is already resolved through
S7's proxy-path translation and native-instance rebasing (S34) when it reaches `UsdGenSurfaceDesc`;
a `GeomSubset` target arrives as `subsetFaces` on the parent mesh's face indices, because
`primvars:skinprim` and `Far::PtexIndices` face ids are always indices into the **parent mesh's**
faces (ADR §2.3). Map assets arrive as resolved paths plus a `textureGeneration` counter (S13), so
the engine never touches `ArResolver`. Rest points arrive sampled at `UsdTimeCode::Default()` from
the `UsdGenRestAPI` adapter (S12); capture-on-first-cook is rejected.

### 2.5 The pull-everything rule (S14)

**`usdGenImaging` pulls every mapped `usdGen:*` locator of every node in the graph at least once per
topology generation, whether or not the operator's current mode reads it.** S14 requires exactly
this ("the graph must pull every surface/parameter data source it depends on at least once per
topology generation, or declare `__dependencies`"), and all three proposals left it unaddressed
(`design/judge-evidence.md` §2.1). Without the rule, a parameter that is only consulted in one mode
— `clump:noise:frequency` while `noise:amount == 0` — may never register a time dependency, and an
edit to it may not invalidate. The rule costs one `VtValue` extraction per property per topology
generation and is asserted by gate SI-7 (`06-imaging.md`).

Its complement is the router (§5.1): every mapped locator has a routing entry, so every authored
property is a `TopologyParameter` or a `ValueParameter` of some node.

---

## 3. Compile

### 3.1 Order

`UsdGenCompiler::Compile()` walks `UsdGenGraphDesc::nodes` and:

1. builds edges from `usdGen:input` only — there is no implicit preceding-sibling edge (S26,
   ADR §2.1); a node with no `usdGen:input` is a source;
2. Kahn-sorts with **namespace order as the tie-break**, so an ambiguous order is still
   deterministic across machines and sessions (the pattern usdRig uses,
   `usdRig libs/rigExec/rigEvaluator.cpp:4025-4068`, `research/A1-usdrig-graph.md` §10);
3. detects cycles with an iterative colour DFS (`usdRig libs/rigExec/rigEvaluator.cpp:3028-3050`) and reports the cycle
   on the offending prim as a compile error — a cycle never partially evaluates;
4. assigns dense `UsdGenNodeId`s in topological order, so "node and all descendants" is an id range
   test in the common single-chain case and a bitset otherwise;
5. resolves `usdGen:space`/`auto` against each type's `Space()` and `usdGen:readPhase` against the
   table of §1.6, and computes the rest/tail split;
6. resolves reference edges and orders the reference lane, and binds each node's `usdGen:curves` /
   `usdGen:guides` / `usdGen:frozen:curves` targets to indices into
   **`UsdGenGraphDesc::curveSets`** (R23; there is no `UsdGenGraphDesc::curves` field);
7. binds each node's `OutputPrimvars()` names to output plane slots on `UsdGenChunkView` (§1.2),
   allocating an `extraCurve` or `extraCv` plane of the declared type and arity on its buffer;
8. computes the graph digest (§3.3) and, if it moved, re-runs the tile arithmetic of §1.4;
9. rebuilds the dirty routing table (§5.1).

Nothing in the compiled graph depends on namespace order beyond the tie-break, because no evidence
says `reorder nameChildren` produces a Hydra invalidation at all (ADR §2.1; an M0 pre-work check
closes the question). The stack editor's drag rewrites `usdGen:input`, one attribute edit per moved
node (ADR §2.1).

### 3.2 Four digests, four consequences

| Digest | Width | Covers | Changing it costs |
|---|---|---|---|
| **structural digest** (per node, Merkle) | 128-bit | type, algorithmVersion, mode, readPhase, space, input paths, reference/map/surface/curve targets, the digests of the inputs | recompile of the changed node and its descendants; their `UsdGenOp`s are recreated |
| **graph digest** (per description) | 128-bit | the description-level fields of `UsdGenGraphDesc` that change compilation but belong to no node | recompile of the tile partition; for `tileTarget`, re-allocation of the published prim set |
| **capture epoch** (per node) | 128-bit | surface topology hash, rest-points hash, `seed`, every `TopologyParameter`, map asset path + `textureGeneration`, `UsdGenCurveSetDesc::curveGeneration` of every curve input, the upstream node's epoch | re-capture of that node and its descendants |
| **value version** (per node, per parameter) | 64-bit counter | every `ValueParameter` | re-evaluate that node's chunks and its descendants' chunks |

This is the short answer to "what does a label edit, a slider edit and a seed edit cost": a
label edit costs nothing (`usdGen:label` is in no digest and no parameter set); a slider edit costs
`edit(parameter)`; a seed edit costs a re-capture but **not** a recompile.

### 3.3 The Merkle structural digest, and the graph digest (ADR §4.2.1)

```
d(n) = H( type,
          algorithmVersion,
          mode,
          readPhase,
          space,
          sorted(input paths),
          sorted(reference ++ map ++ surface ++ curve targets),
          d(input_1), … , d(input_k) )
```

Hashed over `usdGen:input` **ancestors**, not namespace children. This is the ADR's correction of
`proposal-performance.md` §5.3: a chain wired by relationship has its stability in the ancestor
direction, so hashing children inverts the claim that "a change deep in the chain leaves every
prefix digest identical" (`design/judge-delivery.md` §3.3).

`enabled` and `seed` are **not** in the structural digest — indeed `usdGen:enabled` is **never a
term of any digest** (R14): it routes as `UsdGenDirtyParameter` or `UsdGenDirtyTopology` (§3.5).
`seed` bumps the capture epoch. Animated scalars are excluded
by construction, because only the *shape* of a parameter reaches the digest and an animated scalar's
shape does not change per frame — usdRig's rule, and its stated reason: "hashing them would
recompile every frame an artist scrubs one" (`usdRig libs/rigExec/rigEvaluator.cpp:1175-1183`,
`research/A1-usdrig-graph.md` §3.2).

`H` is a 128-bit non-cryptographic hash over a canonical byte encoding (lengths before bytes, sorted
path lists, fixed-width integers, no locale, no pointer values). Node digests are memoised from the
previous compile keyed by prim path, so an unchanged prefix costs one map lookup per node.

**The graph digest** is the fourth digest of §3.2, and it exists because `usdGen:tileTarget`,
`usdGen:motion:*` and `usdGen:schemaVersion` are `UsdGenDescription` properties that `02-schema.md`
§6 classes **structural** while belonging to no node — `d(n)` above cannot move when they change:

```
d(graph) = H( d(terminal node), tileTarget, motionMode, motionSampleCount,
              forwardSurfaceSamples, schemaVersion )
```

A graph-digest change re-runs the tile arithmetic of §1.4 and, when `tileTarget` moved, re-allocates
the published prim set (a session-level resync, `06-imaging.md`). It changes **no** node digest, so
no capture cache and no node output buffer is dropped: the same curves are re-partitioned.

Cost: the digest is a hash over ≤ 30 tokens per node, so a 200-node groom recompiles in **≈ 40 µs**
(ASSUMPTION, extrapolated from usdRig's digest walk, `research/A1-usdrig-graph.md` §3.3; **gate E-6**
measures it at ≤ 0.2 ms with exactly one node rebuilt). `VdfScheduler::Schedule` costs 2.93–4.39 ms
for the same node count (§0.4); having no scheduling phase is one reason S21 chose this engine.

### 3.4 Capture epoch and value version

The capture epoch is a 128-bit digest (`UsdGenEpoch`) so that a surface's rest points and topology
can be hashed into it without collision anxiety at 1 M-vertex scalps. It is the cache key of
`UsdGenCapture` (§4.3): two sibling nodes with the same epoch share one `std::shared_ptr<const
UsdGenKdTree>`.

The value version is a plain monotone counter per node, bumped by the router. It exists so that a
node can be re-evaluated for a *parameter* change without consulting any digest, and so that
`UsdGenNodeStats` can report cache hits honestly.

### 3.5 Sub-graph recompile, and what `enabled` costs

Compile diffs digests: nodes whose structural digest is unchanged keep their `UsdGenOp`, capture
cache and output buffer, and only changed nodes and their descendants are recreated. Appending a
`UsdGenNoise` to a five-node chain leaves four digests identical, rebuilds one node and keeps four
caches and four buffers — the next evaluate costs 0.18–0.41 ms (MEASURED) instead of 1.72 ms for the
chain or 100 ms+ of scatter and kd-trees.

`usdGen:enabled = false` (never `usdGen:active`, which collides with USD prim activation that S41
uses) is **never a term of any digest** (R14, `02-schema.md` §2.5). The router splits it in two:

* **`UsdGenDirtyParameter`** for topology-preserving operators (R14; ADR §2.3, from artist §4.6).
  The node stays in the graph and becomes a pass-through. Semantics are "copy input to output"; the
  implementation is **input aliasing** — the scheduler binds the node's input buffer as its output
  for that generation and writes nothing, which is strictly cheaper than the memcpy the ADR
  describes and observationally identical because every node's input is read-only. A mute/solo A/B
  is one tail re-run, not a recompile and not a re-capture.
* **`UsdGenDirtyTopology`** for the **topology-structural** set: generators, `UsdGenResample`, a
  `UsdGenLength` in cull mode (its **static** `TopologyEffect()` is `UsdGenTopoFx::CurveCount` —
  R14's `MayChangeCurveCount` — because cull mode exists, so the classification does not depend on
  the current parameter value), `UsdGenFreeze` and `UsdGenInstance` (`02-schema.md` §2.5). The node
  routes one `UsdGenDirtyTopology`: downstream nodes re-capture and the description publishes one
  topology change. It is **not a recompile** and **not a digest term**, and the muted node **keeps
  its own capture**, so unmuting restores the identical id set (R14) — a mute/unmute cycle never
  renames a curve, and sculpt deltas keyed by id survive it.

### 3.6 What each dirty class costs

Keyed by the dirty classes of `02-schema.md` §6. "Recompile" is §3.1, "Re-capture" is §4, "Evaluate"
is §5.4, "Publish" is §6.

| Dirty class | Structural digest | Capture epoch | Value version | Work |
|---|:--:|:--:|:--:|---|
| `usdGen:label` edit | — | — | — | none; the router has no entry |
| value parameter (amount, ramp knots, width, blend) | — | — | ✓ | evaluate node + descendants, publish dirty tiles |
| `usdGen:enabled` on a topology-preserving op | — | — | ✓ | `UsdGenDirtyParameter`: evaluate tail, node becomes an input alias (R14) |
| `usdGen:enabled` on a generator, `UsdGenResample`, a culling `UsdGenLength`, `UsdGenFreeze` or `UsdGenInstance` | — | ✓ downstream | ✓ | `UsdGenDirtyTopology`: re-capture downstream + one topology publish. **No recompile**, no digest term; the muted node keeps its capture (R14) |
| `usdGen:seed` | — | ✓ | ✓ | re-capture node + descendants, evaluate, publish |
| topology parameter (`usdGen:density`, cvCount, guide radius) | — | ✓ | ✓ | as above |
| `usdGen:input` rewrite / node add / node remove | ✓ | ✓ downstream | ✓ | recompile sub-graph; unchanged prefix keeps buffers |
| `usdGen:guides` / `usdGen:curves` / `usdGen:frozen:curves` retarget | ✓ | ✓ | ✓ | the target is in the digest's relationship list; the new `UsdGenCurveSetDesc` is captured |
| `points`/`widths` on a bound C3 curve prim (`curveGeneration` bump) | — | ✓ (that node) | ✓ | re-`Capture()` the `UsdGenCurveSource` or reference node; downstream evaluates |
| `usdGen:readPhase` or `usdGen:space` edit | ✓ | ✓ | ✓ | recompile; the rest/tail split moves |
| `usdGen:algorithmVersion` bump | ✓ | ✓ | ✓ | recompile; deliberate look change (§11) |
| map asset path or `ReloadMaps()` | — | ✓ (map consumers) | ✓ | re-capture the consuming nodes only |
| mask source retarget | ✓ | ✓ | ✓ | the target is in the digest's relationship list |
| surface `primvars/points` | — | — | ✓ (tail) | evaluate the deformed tail over that surface's chunk range |
| surface `xform/matrix` | — | — | — | re-publish the tiles' own `xform` (S4); no evaluation |
| surface topology resync or `GeomSubset` edit | — | ✓ | ✓ | re-capture (re-scatter, re-Morton-sort), topology publish |
| `usdGen:tileTarget`, `usdGen:motion:*`, `usdGen:schemaVersion` | ✓ **graph digest** (no node digest moves; `tileTarget` is not a node-digest term, R21) | — | ✓ | recompile the tile partition; `tileTarget` routes as `UsdGenDirtyTopology` on the description — tile prims added/removed, `PrimsAdded`/`PrimsRemoved` (R21) |
| `usdGen:densityScale` / `usdGen:renderDensityScale` drag | — | — | ✓ | decimation only (R13): `liveCount` moves, chunks and capture caches are kept. The drag runs in §6.3's **parked** publication mode, so element counts are fixed and only `points` + `widths` dirty; the real count is published once on release (S27, S28) |
| brush stroke / live override | — | — | ✓ (terminal) | terminal node, touched chunks only |

**This table is advisory; `02-schema.md` §6 is normative.** R7 makes `02-schema.md` the single
normative property registry and R25 generates the router table **per property from 02 §6**, so the
shipped router is generated from 02 and never from this document. The two agree row for row as of
2026-09-05: 02 §6.5 classes `usdGen/densityScale` and `usdGen/renderDensityScale` as **value** —
"re-run the salted stable-id decimation predicate over the **already captured** full id set and
move `UsdGenChunkDesc::liveCount`; never re-scatters and never re-chunks" — which is R13's rule and
this table's density-scale row. Where a future edit makes them disagree, 02 is what ships and this
table is what changes.

---

## 4. Capture versus evaluate

### 4.1 What capture owns

Capture is topology-dependent work, run once per capture epoch, on the commit thread or under
a parallel loop over independent nodes with no shared state (S25, I4, R22):

* **roots** — scatter positions, `curveId` (the 64-bit `UsdGenHash64` of §1.7), `rootPrim`, `rootUV`;
* **root frames** — `rootT/rootN/rootB` in **rest** space, from the surface's `dPdu`/`dPdv`/normal;
* **face ids** — the parent mesh's face index for `primvars:skinprim` and for Ptex face lookup;
* **guide bindings** — `guideIdx[3]` and `guideW[3]` per curve (a host renderer's arity, so a bake
  round-trips; ADR §2.3), from a kd-tree over guide roots;
* **clump ids** — `clumpId[level]` per curve per level, from a kd-tree over clump centres plus a
  stray coin flip;
* **kd-tree neighbours** — the shared `UsdGenKdTree` itself, keyed by epoch;
* **map samples in rest space** — every image/Ptex/expression/paint sample, baked to per-curve or
  per-CV floats (S37);
* **the resolved mask** — one `VtFloatArray` per node per curve plus a 257-entry ramp LUT (§8.3);
* **the chunk partition** — surface-major, Morton-ordered, `boundsRest` per chunk;
* **rest lengths** — for `preserveLength`.

### 4.2 What evaluate owns

Per frame, per chunk, in parallel, with no allocation: the arithmetic that turns captured bindings
plus current parameters plus the current surface into CV positions. That is all. The hard rule
(performance ASSUMPTION 8, promoted here to a contract): **no operator may call an expression, a
texture, a resolver or a kd-tree in `Evaluate`.** SeExpr is 13–117 ns/eval and a Ptex bilinear
lookup 23 ns (MEASURED, `research/A8-seexpr-ptex-libs.md` §1, §2), so 1.6 M CVs of expression
evaluation is ~160 ms per frame — 100× the entire evaluate budget. Anything that must animate goes
through an animated *scalar* parameter, or through a per-frame `UsdGenExprMap` re-capture, which is
an explicit capture-epoch bump the artist can see in the stack profiler.

### 4.3 `UsdGenCapture`

```cpp
struct UsdGenCapture {
    UsdGenEpoch  epoch;
    size_t       bytes = 0;                    // for the memory budget (§6.4)
    VtIntArray   i0, i1, i2;                   // guideIdx[0..2], or clumpId per level
    VtFloatArray f0, f1, f2;                   // guideW[0..2], restLengths, ...
    VtFloatArray curveMask;                    // resolved once, reused across frames
    VtFloatArray rampLut;                      // 257 entries
    std::shared_ptr<const UsdGenKdTree> tree;  // shared between nodes with the same epoch
    std::shared_ptr<void> extra;               // operator-private, sized into `bytes`
};
```

Deliberately concrete: the two things every cross-curve operator needs are *k indices and k weights
per curve*, and they are planar `VtArray`s so the evaluate loop streams them.

### 4.4 The kNN capture, and the number the operator schedule assumes

`UsdGenGuideInterpolate::Capture()`, the shape all cross-curve capture follows:

```
tree = UsdGenKdTree(reference.rootPositionRest)             # nanoflann 1.12.1, vendored (S38)
parallel over roots r in the private arena:
   cand = tree.knn(r, maxCandidates = 8, radius = influenceRadius)
   drop g if angle(restNormal(r), restNormal(g)) > maxGuideAngle
   drop g if regionId(g) != regionId(r)                     # usdGen:mask:region, capture-time
   w_g = (1 - d_g / R)^decay ; normalise ; keep top maxGuides
store cap.i0/i1/i2 = guideIdx[0..2], cap.f0/f1/f2 = guideW[0..2]
```

Evaluate is then, per chunk, `O(curveCount · cvCount · maxGuides)`:

```
for curve c:  F_c = frame(rootT, rootN, rootB)[c]
   for i in 0..cv-1:
      local = w0·ref.local[i0][i] + w1·ref.local[i1][i] + w2·ref.local[i2][i]
      P[c][i] = root[c] + F_c · local
```

Cost: a kd-tree build over 4 000 guide roots is microseconds; 100 000 kNN queries at ~1 µs each is
100 ms **single-threaded**, so capture runs parallel over roots and is expected to land at ~10 ms on
8 threads. **ASSUMPTION**, extrapolated from nanoflann's own benchmarks; nothing on this host has
measured it (`design/judge-delivery.md` §5.6 calls it a phase-1 blocker). **Gate E-4** measures it
over 100 k and 1 M rest roots at 8 threads and is **M0 pre-work** (ADR §7), because M3's operator
schedule assumes the number. UNMEASURED until E-4. It is paid on a seed, density, guide-set or
region-map edit — never per frame, which is why the split is an invariant and not an optimisation.

### 4.5 Frozen and imported curves enter as source nodes

A `UsdGenCurveSource` is a node whose `Capture()` loads its buffer from the description rather than
computing it. The description entry is the `UsdGenCurveSetDesc` of §2.2, reached through
`UsdGenNodeDesc::curves` (the resolved `usdGen:curves` / `usdGen:frozen:curves` targets, indices
into `UsdGenGraphDesc::curveSets`), and it carries exactly the C3 contract: `curveVertexCounts`,
`points`, `rest`, `widths`, `skinPrim`, `skinPrimUv`, `curveId`, the optional `rootFrame` and the
constant `frozenEpoch` string (S42, C3).

`05-static-curves-and-deformation.md` §2.2's `UsdGenSourceArrays` is the **loader's** argument. The
two are related **by construction, not by field identity**: `UsdGenCurveLoader::Load` takes the
array subset (`curveVertexCounts`, `points`, `rest`, `widths`, `skinPrim`, `skinPrimUv`, `curveId`,
`rootFrame`, `frozenEpoch`) and the description adds `path`, `role`, `curveRole`, `guideBlend` and
`curveGeneration`, which are graph-level, not loader-level. `05-static-curves-and-deformation.md`
§2.2 owns the loader signature; contract C3 (end of M2, ADR §3) freezes the **array subset** only.
§12.1 correction B asks 05 to replace its `std::string role` with `TfToken curveRole` so the C3
marker has one type across the two documents.

`UsdGenCurveSource`'s `TopologyEffect()` is `Both`, and its epoch includes both the incoming
`frozenEpoch` and `UsdGenCurveSetDesc::curveGeneration`. Everything
downstream is unaware that the curves were not generated — "freeze an operator's output and comb it"
is a configuration, not a mechanism (`research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2).
`05-static-curves-and-deformation.md` owns the rest of that story.

---

## 5. Scheduling

### 5.1 `UsdGenDirtyRouter` and typed dirty bits

```cpp
enum UsdGenDirtyBits : uint32_t {
    UsdGenDirtyNone          = 0,
    UsdGenDirtyParameter     = 1u << 0,   // evaluate-only, this node's chunks
    UsdGenDirtyCapture       = 1u << 1,   // re-capture this node (epoch bumped)
    UsdGenDirtyTopology      = 1u << 2,   // curve or CV count changed
    UsdGenDirtySurfacePoints = 1u << 3,   // deformed points of a bound surface
    UsdGenDirtySurfaceXform  = 1u << 4,
    UsdGenDirtySurfaceTopo   = 1u << 5,   // resync of the surface
    UsdGenDirtyMap           = 1u << 6,   // asset path / texture generation
    UsdGenDirtyStructural    = 1u << 7,   // recompile
    UsdGenDirtyLiveOverride  = 1u << 8,   // brush stroke, chunk-scoped
};

class UsdGenDirtyRouter {
public:
    void Rebuild(const UsdGenGraph &graph);   // one entry per (primPath, locator prefix)
    /// Called from _PrimsDirtied. O(entries), one hash lookup each; NEVER cooks (I7, S17).
    void Route(const HdSceneIndexObserver::DirtiedPrimEntries &,  UsdGenPendingDirty *) const;
    void RouteAdded(const HdSceneIndexObserver::AddedPrimEntries &, UsdGenPendingDirty *) const;
    void RouteRemoved(const HdSceneIndexObserver::RemovedPrimEntries &, UsdGenPendingDirty *) const;
private:
    struct Entry { UsdGenNodeId node; UsdGenDirtyBits bits; UsdGenSurfaceId surface; };
    TfHashMap<SdfPath, std::vector<std::pair<HdDataSourceLocator, Entry>>, SdfPath::Hash> _map;
};
```

The router lives in `usdGenImaging` (it is the one piece of the scheduling story that touches Hydra
types); the engine consumes `UsdGenPendingDirty`, a pure value type. The table is built **at
compile** from every node's `TopologyParameters()`/`ValueParameters()` and its relationship targets,
so routing is a hash lookup plus a short prefix `Intersects`, never a search.

### 5.2 The four-hop dirty path (R25)

**Hop 1 — locator → node.** The adapter publishes each `usdGen:*` property under a nested locator
(`usdGen/clump/size`). The routing key is the **operator prim path alone**; its value is the
compile-time list of `(locatorPrefix, Entry)` pairs generated from `02-schema.md` §6, held **longest
prefix first**. `Route()` hashes the path once, walks that list, and takes the first entry whose
prefix `Intersects` the dirtied locator.

A fixed-depth key cannot express this table. Under `usdGen/clump` alone sit structural
(`usdGen/clump/centers`), capture (`usdGen/clump/density`, `/levels`, `/seed`) and value
(`usdGen/clump/size`) entries; under `usdGen/mask` sit structural (`/source`, `/combine`), capture
(`/random`, `/noise/*`, `/region`) and value entries. Prefixes are 1–4 elements deep, so truncating
to two would route a clump-size slider as a structural recompile.

MEASURED per-entry cost: `HdDataSourceLocatorSet::Intersects` 0.013 µs
(`research/G-storm-throughput-and-prim-granularity.md` §1.8), so a ≤ 8-entry walk is under 0.1 µs
per notice entry (DERIVED), and ~0.2 µs per notice entry end to end
(`research/G-evaluation-scheduling-and-batching.md` §9); at 5 000 surfaces the whole cascade is
1.13 ms/frame and the call count is exactly 2 `PrimsDirtied` per frame regardless of scene size
(MEASURED, same §9). Routing is not a scheduling concern; **cooking here** would be (S17).

**Hop 2 — node → chunks.**

| Cause | Chunks dirtied | Cost |
|---|---|---|
| value parameter on node *n* | all chunks of *n* and of every descendant | `memset` over `nChunks` bytes per node |
| `primvars/points/primvarValue` on surface *s* | every chunk with `desc.surface == s`, in every `Deformed` node | a precomputed `surface → chunk range`; chunks are surface-major so it is a range, not a scan |
| live override / brush (S40) | the chunk ids the tool passed, or the chunks whose `boundsRest` intersects the brush AABB | a linear scan of `UsdGenChunkDesc::boundsRest` over the surface's chunk range — 1 954 `GfRange3f` tests at 1 M curves, ≈ 5 µs DERIVED, below the 0.035 ms brush budget (§0.3). **No acceleration structure is built in v1.** Past 8 M curves, a per-tile bounds array (64–256 entries) is scanned first: one level, not a tree |
| capture-epoch bump | all chunks of *n* and descendants, plus `UsdGenDirtyCapture` | as above |

**Hop 3 — chunk → tile.** `dirtyTiles.set(desc.tile)` while walking the terminal node's dirty
chunks. A tile is republished iff one of its chunks is dirty. This is the whole reason the tile
exists: Storm's upload unit is the prim
(`pxr/imaging/hdSt/vboMemoryManager.cpp:624-682`,
`research/G-storm-throughput-and-prim-granularity.md` §1.3), so a one-chunk edit must not re-upload
the groom.

**Hop 4 — tile → Hydra leaves.** Owned by `06-imaging.md` §5.1 (bare
`primvars/points/primvarValue` + `extent/*` where usdGen owns the prim; `ComputeDirtyLocators`
**and** the bare `primvars` locator where it overlays an upstream prim, per S5). The engine's
contribution is the `UsdGenDirtyReport` the generation diff produces (§6.1).

### 5.3 Threads: a private `tbb::task_arena` at 8 (I8)

```cpp
class UsdGenScheduler {
public:
    explicit UsdGenScheduler(int threadLimit = 0);   // 0 == calibrate
    void Run(UsdGenGraph &, const UsdGenEvalContext &, UsdGenRunResult *);
    int  ThreadLimit() const noexcept;
private:
    tbb::task_arena _arena;    // tbb::task_arena(int max_concurrency, unsigned reserved_for_masters)
};
```

`tbb::task_arena` is constructed with an explicit concurrency
(`OpenUSD_26_08/include/tbb/task_arena.h:282-283`) and every parallel region runs inside
`_arena.execute([&]{ … })`. Two reasons, both measured:

1. **The knee is 8 on this host** (3.90 / 1.02 / 1.79 ms at 1 / 8 / 20 threads, MEASURED §0.3). The
   host is heterogeneous (10× Cortex-X925 + 10× Cortex-A725), so thread tuning must not assume
   symmetric cores.
2. Storm syncs Rprims with `WorkWithScopedParallelism` + `WorkParallelForN` over rprims
   (`pxr/imaging/hd/renderIndex.cpp:1830-1862`, verified). A shared default arena would let a commit
   that overlaps a sync fight for the same workers. A private arena also avoids changing
   `PXR_WORK_THREAD_LIMIT` globally, which would slow usdRig down in the same process.

**One caveat the arena does not fix.** `WorkParallelForN` short-circuits to `WorkSerialForN` when
`WorkHasConcurrency()` is false (`pxr/base/work/loops.h:170-190`, verified), and that predicate
reads the **process-global** `PXR_WORK_THREAD_LIMIT`, not the arena
(`pxr/base/work/threadLimits.h:40-44`, verified: it "returns true if
`WorkGetPhysicalConcurrencyLimit()` returns a number greater than 1 and `PXR_WORK_THREAD_LIMIT` was
not set in an attempt to limit the process to a single thread"). Under `PXR_WORK_THREAD_LIMIT=1` — a
common batch-render setting — capture and interleave would run **serially inside an 8-slot arena**.
Every usdGen parallel region therefore calls `tbb::parallel_for` directly, exactly as the chunk
sweep does; `WorkParallelForN` appears in this document only as shorthand for "a parallel loop",
never as the shipped call.

**Calibration.** `USDGEN_THREAD_LIMIT`, when set, wins and skips calibration. Otherwise
`UsdGenCalibrateThreads()` runs **once per process** before the first commit: a fixed synthetic
styler pass over 64 chunks (≈2 MB working set) at concurrencies {2, 4, 8, 16,
min(20, `WorkGetPhysicalConcurrencyLimit()`)}, taking the smallest concurrency within 5 % of the
best time, clamped to [2, physical]. Process-global, nothing on disk, budget ≤ 5 ms once.
UNMEASURED until **gate E-7**, which also asserts ≥ 3× scaling from 1 → 8 threads on E-1.

The worker contract differs by **phase**, because capture and evaluate are not the same kind of
work. Splitting the row is what makes the contract satisfiable: `Capture()` legitimately allocates
(§8.1), `Evaluate()` legitimately may not.

| Thread | May do | May not do |
|---|---|---|
| app / notice thread (**the commit thread**) | accumulate dirties, compile, drive capture, drive evaluate, publish, emit notices | block on I/O without a timeout |
| arena workers, **capture** phase | run one node's `Capture()`; allocate into *that node's own* capture and buffer; take the graph mutex (R22) | touch another node's capture, emit notices |
| arena workers, **evaluate** phase | run `Evaluate` for one chunk | allocate, emit notices, touch another chunk, take any mutex |
| Hydra readers (Storm's parallel Rprim sync) | `std::atomic_load` the published generation and wrap it in data sources | evaluate, lock, change time, touch a stage |

Verified basis: `GetPrim`/`GetChildPrimPaths` are documented threadsafe
(`pxr/imaging/hd/sceneIndex.h:98`, `:109`); observer callbacks are documented **not** threadsafe
(`pxr/imaging/hd/sceneIndexObserver.h:123, 132, 143, 151`). MEASURED: 8 readers × 20 publishes gave
16 734 reads and **0 torn reads** with an atomic snapshot — `std::atomic_load` on a
`std::shared_ptr`, which is exactly the pattern usdRig ships
(`usdRig libs/rigExecImaging/snapshotStore.h:330, 369-370, 376-377`),
`research/G-evaluation-scheduling-and-batching.md` §5. §6.1 declares the same type for that reason.

### 5.4 The commit, step by step

`UsdGenSession::Commit()` runs on one thread under one non-recursive mutex. Commit *triggers* are
`01-architecture.md`'s, as amended by **R32**: trigger **(c)** — an operator-parameter / map /
surface-topology dirty — commits synchronously at the end of the `_PrimsDirtied` batch that carried
it, **with or without an app driver attached**, so an edit batch is exactly one cook (SI-3). The
application therefore **never calls `Commit()` for a stage edit**; a tool's release sequence is
close the live override without republishing → author the edit in one `Sdf.ChangeBlock` → request a
repaint → `ApplyPendingUpdates` delivers the dirty → the index cooks and publishes. Trigger **(a)**
is `SetTime()` and an explicit `UsdGenImaging_Commit()` after a live-override change; trigger
**(b)** is the `/ sceneGlobals/currentFrame` dirty when no (a) is attached. Nothing cooks in
`GetPrim` and nothing cooks in every `_PrimsDirtied` (I7, S17; the `GetPrim` backstop is withdrawn,
ADR §1 S18(c)). `06-imaging.md` §3.9 states the same three triggers, with (c) unconditional
(verified 2026-09-05); it is the imaging-side owner of the wording and this section conforms to
it.

```
 1. if UsdGenDirtyStructural: UsdGenCompiler::Compile           (§3, ~40 µs ASSUMPTION, gate E-6)
 2. REFERENCE LANE, in its own topological order (§1.5):
        _arena.execute([&]{
            tbb::parallel_for(0, nDirtyRefNodes, captureRefBody); // ref nodes are independent
            for (r : refOrder) evaluate r TO COMPLETION;          // whole buffers, no chunk dirtiness
        });
 3. re-capture consumer nodes whose epoch changed
        _arena.execute([&]{ tbb::parallel_for(0, nDirtyNodes, captureBody); });
 4. for each node in topological order:
        if node.dirtyChunks is empty            -> skip
        if !node.enabled && TopologyEffect()==None -> alias input buffer, clear dirty, continue
        //  a muted node with TopologyEffect() != None keeps its capture and contributes nothing (R14)
        _arena.execute([&]{
            tbb::parallel_for(tbb::blocked_range<size_t>(0, nChunks, 1), body);
        });
        // body: skip clean chunks; memcpy in->out for this chunk; op->Evaluate(ctx, cap, view);
        //       then apply the envelope if blend < 1 (§8.5)
    the commit thread then memsets node.dirty     (workers never write dirty bytes: no false sharing)
 5. interleave dirty tiles SoA -> AoS, compute per-tile extents
        _arena.execute([&]{ tbb::parallel_for(0, nDirtyTiles, interleaveBody); });
 6. build the immutable UsdGenGeneration, store.Publish()        (std::atomic_store)
 7. diff against the previous generation -> UsdGenDirtyReport
 8. imaging emits PrimsRemoved, PrimsAdded, PrimsDirtied in that order
```

**The reference lane goes first, before any consumer's `Capture()`.** A cross-curve consumer's
capture — the kd-tree over guide roots, the clump-centre binding, `guideIdx[3]`/`guideW[3]` — reads
reference *buffers*, and `UsdGenCaptureContext::references` is a contract that they are already
evaluated (§1.5, §8.1). Capturing a seed or guide-set edit before step 2 would bind against stale or
empty buffers; steps 2 and 3 are separate for the same reason.

Steps 2–5 are the only parallel regions and every one is wrapped in `_arena.execute`. That wrapper
is what confines them: `WorkImpl_ParallelForN` calls plain `tbb::parallel_for` with an isolated
`task_group_context` and no arena of its own (`pxr/base/work/workTBB/loops_impl.h:31-56`,
verified), so a loop runs in the **calling thread's current** arena and a bare call outside
`_arena.execute` would run in the default 20-worker arena and give up the 8-thread knee (§0.3,
§5.3). Every loop is written as `tbb::parallel_for` directly, for the `WorkHasConcurrency()` reason
§5.3 gives.

Step 8 is on the commit thread because observer callbacks are not required to be threadsafe; the
order Removed → Added → Dirtied is usdRig's
(`usdRig libs/rigExecImaging/sceneIndices.cpp:2507-2515`, verified).

### 5.5 What the schedule costs

The four cases that matter are §0.3's rows and are not restated here: cold 2.12 ms, full chain
1.72–1.91 ms, terminal parameter 0.179–0.411 ms, 1 % of chunks 0.035–0.044 ms — all at 100 k × 8 CV
and **20 threads**, all MEASURED, with the 1 M column at 7.59–7.67 / 0.22–0.30 (DERIVED) / 0.29 ms.
Per R27 every budget quotes the **8-thread arena** number with the 20-thread number in parentheses,
so the `c(n)` constant below is derived from the 100 k full chain at **8 threads = 1.02 ms**
(MEASURED, `EV-001` / `EV-008`, `research/G-data-plane-engine-prototype-benchmark.md` §3.3), not
from 1.72 ms. The cost model that `09-performance-and-benchmarks.md` sets its thresholds from
follows; its constants are the ones `09-performance-and-benchmarks.md` §2.1 carries, and each is
DERIVED from the ledger row named beside it:

```
commit(frame)      = R + Σ_{n in tail} c(n)·dirtyChunks(n)/nChunks + I·dirtyTiles/nTiles + P
edit(parameter p)  = R + Σ_{n in cone(p)} c(n) + I·dirtyTiles/nTiles + P
edit(brush stroke) = R + c(terminal)·k/nChunks + I·1/nTiles + P
capture(epoch)     = Σ_{n in cone} cap(n)                       # per edit, never per frame
motion P2          = H_cached + k·T + (k-1)·m

R = 0.04 ms routing         DERIVED from EV-037 (0.2 µs/entry, MEASURED) × ~200 entries; the entry
                            count is itself UNMEASURED, gate SI-3
I = 0.0255 ms/MB interleave DERIVED FLOOR from EV-074 (0.49 ms per 19.2 MB VtArray pass, one thread,
                            -O2, MEASURED) → 0.25 ms for the 9.6 MB one 100 k × 8 CV publish moves,
                            0.5 ms at 1.6 M CVs. A strided three-stream gather is not a memcpy, so
                            the real figure is higher: UNMEASURED, gates E-1 and S-2
P = 0.02 ms publish + diff  DERIVED from the per-prim costs at this fixture's 49 tiles
                            (G-storm-throughput §1.8, MEASURED); 64 prims is the 1 M-curve row of
                            §1.4's table, never the 100 k one (R21)
m ≈ 1.0 ms per memory-bound pass over 1.6 M points   MEASURED, EV-071…EV-073 (G-motion-blur §5)
c(styler) ≈ 0.20 ms per 800 k CVs at 8 threads       DERIVED from EV-001/EV-008, the 5-node chain at
                                                     1.02 ms @8 threads (MEASURED, G-data-plane-engine
                                                     §3.3); at 20 threads the same term is ≈ 0.35 ms
                                                     (1.72 / 5)
```

### 5.6 Interruption and supersession (ADR §4.2.4)

A scrub can supersede an in-flight commit. `UsdGenSession` keeps
`std::atomic<uint64_t> _generationRequested`; `Commit` compares it **between nodes** and, if a newer
request exists:

1. abandons the run without touching the published generation — **never a half-publish**;
2. returns the previous generation unchanged;
3. **leaves `_dirty` set**, so the next trigger re-runs.

Point 3 is the ADR's fix to `proposal-performance.md` §5.5, which returned the previous generation
and left nothing armed (`design/judge-artist.md` §3, Performance #5). A supersession happens only
because a newer request exists, so that request is the next commit; if it is withdrawn, the dirty
state is still pending for trigger (a) or (b). The published frame may lag the scene's frame for one
commit; it is never inconsistent. usdGen simply never half-publishes — a correctness property, not a
measurement.

---

## 6. Generation store and publication

### 6.1 Generations

```cpp
struct UsdGenGeneration {                    // NOT a TfRefBase: see the note below
    int64_t                                 id;
    double                                  frame;
    std::vector<UsdGenTilePublication>      tiles;
    std::vector<UsdGenTilePublication>      guides;
    std::vector<UsdGenInstancerPublication> instancers;
    UsdGenPrimSetSignature                  signature;   // for the structural diff
};
using UsdGenGenerationConstPtr = std::shared_ptr<const UsdGenGeneration>;

class UsdGenGenerationStore {
public:
    void                     Publish(UsdGenGenerationConstPtr);  // commit thread only, std::atomic_store
    UsdGenGenerationConstPtr Get() const noexcept;               // std::atomic_load, lock-free
    UsdGenDirtyReport        Diff(const UsdGenGeneration *prev,
                                  const UsdGenGeneration &next) const;
private:
    UsdGenGenerationConstPtr _current;
};
```

**Why `std::shared_ptr` and not `TfRefPtr`.** The publish/read pair must be a lock-free atomic load
and store, and `TfRefPtr` has neither: a grep of `pxr/base/tf/refPtr.h` and `pxr/base/tf/refBase.h`
in 26.08 finds no `atomic_load`/`atomic_store` overload (verified). The measured evidence for this
design — 16 734 reads, 0 torn reads (§5.3) — was gathered on `std::atomic_load` over a
`std::shared_ptr`, which is also what usdRig ships
(`usdRig libs/rigExecImaging/snapshotStore.h:284-285`, `:330`, `:369-370`). C++20 caveat: the free
`std::atomic_load(shared_ptr*)` overloads are deprecated there, so on a C++20 move the member
becomes `std::atomic<UsdGenGenerationConstPtr> _current` with no change to the contract.

A generation is immutable once published. `Diff` is the analogue of usdRig's
`RigExecSnapshotStore::_Diff` (`usdRig libs/rigExecImaging/snapshotStore.h:326-378`,
`research/A1-usdrig-graph.md` §4) and is deliberately cheap: each tile carries a `topologyVersion`
and its arrays are `VtArray`s, so "did this tile change" is `!a.IsIdentical(b)` — a data-pointer
comparison (`pxr/base/vt/array.h:945-950`, verified), not a memcmp. Structural bits (a prim
appearing or disappearing, a primvar set change, a type change) come from `signature`.

### 6.2 Handoff: `VtArray` copy-on-write is the default (S24, R20)

Publication hands the imaging layer `VtArray`s by value. Copy-construct is a refcount bump
(0.00005 ms MEASURED, §0.3). The next edit pays one **CoW detach**, and that number is disputed:
`research/G-data-plane-engine-prototype-benchmark.md` §3.4 states 2.32 ms against a 1.97 ms
baseline for a 9.6 MB buffer, but the raw log's `run_full_with_outstanding_ref_ms` reads **3.10 ms**
for a differently-configured run (`prototypes/data-plane-benchmark/results_main.txt:102`) and
1.97 ms is not reproducible from any prototype output. Treat the detach cost as **UNMEASURED until gate
E-1**, which re-measures it inside the arena (§0.3). The detach is the price of giving readers an
immutable snapshot for free, and it is the discipline usdRig already follows.

Buffer reuse — the optional publish ring, I6 — is an **M7 optimisation measured against E-1**, and
R20 fixes its mechanism: it uses `Vt_ArrayForeignDataSource`, not any uniqueness test. Every
published `VtArray` wraps a `class UsdGenGenerationBuffer : public Vt_ArrayForeignDataSource`
constructed with a static `_Detached` callback and a `UsdGenBufferPool *`. The base class takes a
`void (*detachedFn)(Vt_ArrayForeignDataSource *)` and invokes it from `_ArraysDetached()` "when no
more arrays share this data source" (`pxr/base/vt/array.h:39-54`, verified); the callback returns
the buffer to the session pool. A buffer becomes reusable **only after that callback has fired**,
so a buffer another holder still references is never patched — which is exactly the property ADR §1
S24 wanted.

**`VtArray::IsUnique()` does not exist in OpenUSD 26.08 and appears nowhere in this design** (R20,
which supersedes S24's wording). Verified: `VtArray` has a private `_IsUnique()`
(`pxr/base/vt/array.h:1023-1026`) and the public identity test is `IsIdentical()` (`:945-950`),
which is what §6.1's generation diff uses and all it is used for.

### 6.3 Tile assembly

Per dirty tile, one pass over its chunk range:

1. interleave planar `px/py/pz` of the terminal buffer's **live** curves into one
   `VtVec3fArray points`, and write `curveVertexCounts` — `points.size()` must equal
   `Σ curveVertexCounts` exactly (S28; a padded array renders fallback red,
   `research/G-storm-throughput-and-prim-granularity.md` §1.4). Gate SI-1 asserts it;
2. accumulate `extent/min|max` in the same pass — ≈ 0.05 ms for the **49 tiles** a 100 k-curve
   description publishes at the default partition (`chunkSize = 512`, `tileTarget = 64` ⇒ 196
   chunks, 4 chunks per tile, 49 tiles; R21 and §1.4 — **64 tiles is the 1 M-curve row**).
   ASSUMPTION (min/max fused into the interleave loop), **UNMEASURED**: gate S-2 covers it
   explicitly ("also measures the interleave and per-tile `extent` terms",
   `09-performance-and-benchmarks.md` §5.3) and gate E-1 covers the `I` term of §5.5.
   `09-performance-and-benchmarks.md` §0.2 and `01-architecture.md` §0.3 carry the same 0.05 ms at
   the same 49 tiles;
3. copy the uniform per-curve channels: `st` (root UV), `displayColor`, and `hairId` as a uniform
   **float** in [0,1) computed as `UsdGenHash32(curveId, 0) / 2^32` (ADR §1 S29, R12 — the shipped
   glslfx declares `hairId` as `float`; salt 0 here, `kSaltDensity` for decimation, §1.2);
4. copy the vertex channels `widths` and `hairT`; derive `hairTangent` only under variant B (ADR
   §5.4);
5. copy **every plane named by any node's `OutputPrimvars()`** into the tile at its declared
   interpolation, type and arity: `clumpId_<level>` uniform `int`; `guideIndex` uniform `int[]`
   with `elementSize = 3`; `guideWeight` uniform `float[]` with `elementSize = 3` (R24 —
   `int[3]`/`float[3]` are **not** USD type names; ADR §2.3, `04-operators.md` §1.2's emitted-primvar
   column and §2.3). Per-curve
   planes are decimated by `liveCount` exactly as `st` is, or parked exactly as `points` is (below).
   A plane appearing for the first time is dirtied as `primvars/<name>` once, never co-dirtied with
   `points` (`06-imaging.md` §5.1).

**Two publication modes.** *Committed* (the default): only **live** curves are interleaved;
`points.size()` and `curveVertexCounts` change and the tile publishes a topology change. *Parked*
(entered by `UsdGenSession::BeginDensityDrag()` and held for the duration of a `usdGen:densityScale`
or `usdGen:renderDensityScale` drag): every curve of the **full** id set is interleaved at the
description's max-density topology, and a curve the R13 predicate rejects is written with collapsed
CVs (every CV at its root) and zero `widths`. `points.size()` and `curveVertexCounts` are then
byte-identical to the previous generation, so only `primvars/points/primvarValue` and
`primvars/widths/primvarValue` dirty and the element count is fixed for the whole drag (S27, S28).
`EndDensityDrag()` republishes once in committed mode with the real counts. This is what makes
§3.6's density-scale drag row a pure **value** change; without the parked mode the same drag would
publish a topology change on every move.

Interleaving is the one AoS conversion in the system (I1) and it is one parallel loop over tiles
inside the arena. **DERIVED anchor**: a full interleave of 800 k CVs is 9.6 MB, so ≈ 0.25 ms, and
≈ 0.5 ms at 1.6 M CVs — from the MEASURED 0.49 ms per 19.2 MB pass (`EV-074`,
`research/G-motion-blur-sampling-strategy.md` §5). That is §5.5's `I` term at this fixture
(0.0255 ms/MB); gate E-1 measures it directly.

### 6.4 Memory budget and eviction

`USDGEN_MEMORY_BUDGET_MB` (env, default 4096; never authored on a prim, ADR §2.3) is enforced by one
pass at the end of each commit:

```
score(node) = recomputeCostMs(node) / bytes(node)        # from UsdGenNodeStats, measured, not modelled
evict lowest score first, until bytes <= budget
```

Never evicted:

* **the tail base** — the last `UsdGenSpace::Rest` node, because it is re-read every deforming
  frame; evicting it would cost a head re-run *per frame*;
* **any node a live override targets** — the brush loop's whole point is that a move costs 0.035 ms;
* **a capture cache whose rebuild is O(n log n)** (kd-trees, guide bindings) unless its bytes exceed
  4× the node's buffer bytes — a 10 ms+ rebuild is never worth 6 MB;
* **the published generation** and the one previous generation (readers may still hold them).

The policy is deliberately biased toward losing **interior rest-space nodes**: the only edit that
needs one back re-runs the whole prefix anyway, and a prefix re-run is bounded by the head cost
(1–2 ms at 100 k, **DERIVED** from the 1.02 ms five-node chain at 8 threads,
`research/G-data-plane-engine-prototype-benchmark.md` §3.3, MEASURED — no head-only measurement
exists). One `TF_WARN` per eviction pass, not per node. Gate **E-5** holds RSS
at ≤ 800 MB for 1 M × 8 CV with 5 nodes (MEASURED baseline 668 MB). Risk §4.5's authored
`usdGen:cacheOutput` opt-out is dropped: ADR §2.3 puts memory tuning in env/config, and eviction
achieves the same effect per machine (§12.1 deviation 2).

---

## 7. Motion

### 7.1 The three profiles (S32, ADR §2.3)

| Profile | `usdGen:motion:mode` | `GetContributingSampleTimesForInterval` | Cost per frame |
|---|---|---|---|
| **P0 single** (default) | `single` | `false` | 1 × tail |
| **P1 velocities** (v1, M7) | `velocities` | `false`; publishes `primvars/velocities`, blocks upstream ones | 1 × tail + one finite-difference pass (0.97–1.00 ms per 1.6 M points, MEASURED) |
| **P2 samples** | `samples` | `true` + the retained offsets (≥ 2, ≤ 16) | `k·T + (k−1)·m` |

Storm never asks for samples (no `SamplePrimvar` in hdSt; `basisCurves` reads `GetValue(0.0f)`,
`pxr/imaging/hdSt/basisCurves.cpp:975-990`), so P2 costs the viewport nothing. `velocities` and
`accelerations` are blocked whenever usdGen owns `points`, in every profile except P1 (S29, S32) — a
stale upstream `velocities` would otherwise be extrapolated from our new points. **Never bake motion
samples into a layer** (S42).

### 7.2 `UsdGenMotionCache`

```cpp
class UsdGenMotionCache {
public:
    struct Key { uint64_t graphGen; uint64_t surfaceGen; double absTime; };
    /// `descAtTime` is the graph description whose UsdGenSurfaceDesc::points have been resolved to
    /// `absTime` — from `UsdGenSurfaceDesc::samples` when the surface has them (index 0 is the
    /// current-time sample, R23), by linear interpolation between the two bracketing samples when
    /// `absTime` is not itself a sample, clamped outside the sample range. Without it the deformed
    /// tail has no surface to read at time + offset.
    const UsdGenSampleSet &Fill(UsdGenSession &, double absTime,
                                const UsdGenGraphDesc &descAtTime);   // takes the commit mutex
    void   Invalidate();                                              // any graph or surface dirty
    size_t Bytes() const noexcept;
};
```

`usdGenImaging` builds `descAtTime` by rewriting only `UsdGenSurfaceDesc::points` and `worldMatrix`
from `UsdGenSurfaceDesc::samples` (sorted, `samples[0].time == UsdGenGraphDesc::time`, R23); every
other field is a refcount bump, so a per-offset description is cheap.
`usdGen:motion:forwardSurfaceSamples` (ADR §2.3) selects which times land in `samples`: `true`
forwards the surface's own `GetContributingSampleTimesForInterval` list (UsdImaging typically
reports `[-1, 0, +1]`); `false`, the default, clamps to `{open, 0, close}`, which is cheaper and
closer to the shutter (`research/G-motion-blur-sampling-strategy.md` §4.2, §5).

Keyed by `(graphGeneration, surfaceGeneration, absTime = frame + offset)`, exactly as
`research/G-motion-blur-sampling-strategy.md` §4.4 prescribes. Keying by **absolute** time makes
whole-frame offsets `[-1, 0, +1]` reuse the previous frame's `+1` as this frame's `0` for free while
scrubbing forward; sub-frame offsets never coincide and simply evict.

Per sample, only the **deformer re-runs**: `Fill` re-evaluates the deformed tail `T` at
`time + offset` and reuses the cached rest head `H`. Capacity is `k+1` samples per description;
19.2 MB per sample at 100 k curves × 16 CV (MEASURED), so `k = 3` ≈ 58 MB and `k = 9` (hdPrman
`geosamples 9`) ≈ 173 MB. `Bytes()` counts against `USDGEN_MEMORY_BUDGET_MB`.

### 7.3 Non-retained times

`GetValue(t)` **lerps between the two bracketing retained offsets and clamps outside**, because
hdPrman re-distributes the shutter to `ri:object:geosamples` and will ask for times usdGen never
retained (`research/G-motion-blur-sampling-strategy.md` §4.5). One lerp over 1.6 M points is 0.99 ms
(MEASURED). Only `points` and the prim `xform/matrix` need samples; hdPrman collapses every other
primvar to offset 0, so widths, `hairT`, `st`, `hairId` and `displayColor` are single-sample
retained sources.

Lazy fill happens inside `HdRenderIndex::SyncAll`, which is multithreaded across prims, so it is
serialised behind the commit mutex and keyed by the generation digest: every tile of the same
description pulled in the same sync waits once, then reads the cache. Gate **R-1** (tier T4) asserts
`k·tail` evaluations, not `k·chain`.

---

## 8. Kernel authoring rules and the operator interface

### 8.1 `UsdGenOp`

```cpp
// usdGen/op.h
struct UsdGenCaptureContext {
    const UsdGenGraphDesc     *desc;        // §2.2; surfaces, maps, density scales
    const UsdGenParamView     *params;      // this node's resolved parameters and ramps
    const UsdGenReferenceSet **references;  // resolved ReferenceInputs(), already evaluated
    UsdGenSurfaceId            surface;
    UsdGenReadPhase            readPhase;
    uint32_t                   seed;
    WorkDispatcher            *dispatcher;  // capture may parallelise inside the private arena
    UsdGenDiagnostics         *diag;
};

struct UsdGenEvalContext {
    double                     time;
    float                      shutterOffset;   // 0 in P0/P1 (§7)
    const UsdGenGraphDesc     *desc;            // the desc whose surface points are already
                                                // resolved to time + shutterOffset (§7.2)
    const UsdGenParamView     *params;
    const UsdGenReferenceSet **references;
    uint32_t                   seed;
};

class UsdGenOp {
public:
    virtual ~UsdGenOp();

    // ---- static description, read at compile ------------------------------------
    virtual TfToken        Type()           const = 0;
    virtual UsdGenSpace    Space()          const { return UsdGenSpace::Rest; }   // what `auto` (Inherit) resolves to
    virtual UsdGenReadPhase ReadPhase()     const { return UsdGenReadPhase::Final; } // type fallback; matches R9's authored default
    virtual UsdGenTopoFx   TopologyEffect() const { return UsdGenTopoFx::None; }
    virtual UsdGenRole     Role()           const { return UsdGenRole::Curves; }
    virtual TfSpan<const TfToken> TopologyParameters() const = 0;
    virtual TfSpan<const TfToken> ValueParameters()    const = 0;
    virtual TfSpan<const TfToken> ReferenceInputs()    const { return {}; }
    /// Names of the extra planes this operator writes, in slot order. The compiler allocates one
    /// UsdGenPlane per name on the node's buffer (per-curve in `extraCurve`, per-CV in `extraCv`)
    /// and binds `UsdGenChunkView::outF/outI` slot i to name i; the interleaver publishes them
    /// (§1.2, §6.3). Declared types are fixed per name by R24: `clumpId_<level>` uniform `int`;
    /// `guideIndex` uniform `int[]` with elementSize = 3; `guideWeight` uniform `float[]` with
    /// elementSize = 3. `int[3]`/`float[3]` are not USD type names.
    virtual TfSpan<const TfToken> OutputPrimvars()     const { return {}; }
    /// Names of the extra planes this operator reads from upstream, bound to `inF`/`inI`.
    virtual TfSpan<const TfToken> InputPrimvars()      const { return {}; }

    // ---- binding: description values -> a POD parameter block -------------------
    virtual bool Bind(const UsdGenParamView &, UsdGenDiagnostics *) = 0;

    // ---- capture: topology-dependent, cached by epoch ---------------------------
    virtual UsdGenEpoch CaptureDigest(const UsdGenCaptureContext &) const = 0;
    virtual bool        Capture(const UsdGenCaptureContext &,
                                const UsdGenCurveBuffer &upstream,
                                UsdGenCapture *out, UsdGenDiagnostics *) = 0;

    // ---- evaluate: per frame, per chunk, parallel, no allocation ----------------
    virtual void Evaluate(const UsdGenEvalContext &,
                          const UsdGenCapture &,
                          UsdGenChunkView *view) const = 0;

    // ---- generators only ---------------------------------------------------------
    virtual bool GenerateTopology(const UsdGenCaptureContext &,
                                  UsdGenCurveBuffer *out, UsdGenDiagnostics *) { return false; }
};
```

Renames against `proposal-performance.md` §5.2, adopting this plan's engine vocabulary:
`Topology()` becomes `TopologyEffect()`; `ParameterNames()`/`CaptureParameters()` become risk §4.2's
`ValueParameters()`/`TopologyParameters()` pair, which is the classification §2.3 and §3.6 depend
on; `ReadPhase()` and `Role()` are declared because §1.5 and §1.6 need them at compile time.
`Space()` is the value the authored token `auto` (`UsdGenSpace::Inherit`) resolves to (§1.6), so an
operator type declares its `restSpace | deformedSpace` class once, in C++, and the schema's default
defers to it.
`UpstreamChunks()` does **not** exist: the reference lane removes the fan-in (§1.5, §10).

Contract rules, enforced by a debug wrapper (`USDGEN_OP_CHECKS=1`):

| Rule | Why |
|---|---|
| `Evaluate` is `const`, allocates nothing, touches no stage, reads nothing outside `view`/`capture`/`ctx` | S8; allocation inside `tbb::parallel_for` is the classic scaling killer |
| `Evaluate` writes only its own chunk's CV range | the exact rule VDF's 500-element grain violates (§0.4) |
| cross-curve reads come from `capture` plus a reference buffer, never from another chunk | I3 |
| no expression, texture, resolver or tree query in `Evaluate` | §4.2 |
| `Capture` may allocate into **its own** node's capture and buffer, and may take the graph mutex; it runs on the commit thread or under a `tbb::parallel_for` over independent nodes inside the arena (R22; §5.3, capture phase, including the `WorkHasConcurrency()` caveat). It may not touch another node's capture or emit a notice | S17/S18, R22 |
| `TopologyParameters() ∪ ValueParameters()` equals the node's mapped property set | §2.5, S14 |
| kernels live in `usdGenMath`, are free functions over raw pointers, compiled `-ffp-contract=off` | S22, S45 |

### 8.2 `UsdGenOpRegistry` — internal in v1 and v2

```cpp
using UsdGenOpFactory = std::function<std::unique_ptr<UsdGenOp>()>;
class UsdGenOpRegistry {                       // TfSingleton
public:
    static UsdGenOpRegistry &GetInstance();
    void Define(const TfToken &primType, UsdGenOpFactory);
    std::unique_ptr<UsdGenOp> Create(const TfToken &primType) const;
    bool IsKnown(const TfToken &primType) const;
    TfTokenVector GetTypes() const;
};
#define USDGEN_DEFINE_OP(PrimTypeString, Class) \
    TF_REGISTRY_FUNCTION(UsdGenOpRegistry) { \
        UsdGenOpRegistry::GetInstance().Define(TfToken(PrimTypeString), \
            []{ return std::unique_ptr<UsdGenOp>(new Class); }); }
```

All registrations live inside `libusdGen.so`. A third-party operator ABI is **v3** (ADR §3):
committing to a public `Evaluate` signature before the v-table has run at production scale is how a
project ends up unable to change it. The v2 non-C++ tier is `UsdGenExprOp` (SeExpr, capture-time
only, §4.2). The tool's "Add operator" menu is driven by the *schema* registry, not `GetTypes()`.

### 8.3 Kernel rules (`usdGenMath`)

```cpp
// usdGenMath/stylerKernels.h — the shape every styler kernel takes
void UsdGenClumpKernel(const UsdGenClumpParams &p,
                       const float *RESTRICT cx, const float *RESTRICT cy, const float *RESTRICT cz,
                       const float *RESTRICT ramp,        // 257-entry LUT over hairT
                       const int   *RESTRICT clumpId,
                       const float *RESTRICT mask,
                       uint32_t curveCount, uint32_t cvCount,
                       float *RESTRICT px, float *RESTRICT py, float *RESTRICT pz);
```

1. **Free functions, `RESTRICT` pointers, no `GfVec3f` in the inner loop.** The layout, not
   intrinsics, is the lever: 80.9 vs 25.3 GFLOP/s (MEASURED §0.3).
2. **Ramps are baked to a 257-entry float LUT at capture**, so the inner loop is a lerp, not a
   spline evaluation (usdRig's falloff-LUT precedent: falloff profiles are epoch-structural and
   baked to a 257-entry LUT — `usdRig libs/rigExecMath/weightFields.h:43-45`, verified:
   `constexpr size_t RigExecFalloffLutSize = 257;` with the comment "A power of two plus one so both
   endpoints land exactly on a sample"; `usdRig libs/rigExec/rigEvaluator.cpp:1211-1229`,
   `research/A1-usdrig-graph.md` §3, §6).
3. **`-ffp-contract=off`** on `usdGenMath`, plus a CI job that builds with `fast` so epsilons stay
   honest. Direct evidence: usdRig's `testRigExecCurvenet` fails without it because FMA contraction
   flips surface-walking branches (S45, `research/B-usdrig-build.md`).
4. **No hand-written SIMD.** g++ 13.3 `-O3` autovectorises all three prototype kernels with 128-bit
   NEON (MEASURED §0.3); usdRig's only SSE2 kernel takes the scalar path on aarch64 anyway
   (`usdRig libs/rigExecMath/simdKernels.cpp:14-45`).
5. **Deterministic reductions**: fixed order, `hash64` rather than any RNG with hidden state, no
   dependence on unordered-container iteration — usdRig's determinism contract
   (`usdRig docs/spec.md:1199-1208`, `research/A1-usdrig-graph.md` §5).
6. **Seed salting**: every hash is `hash(usdGen:seed, curveId, saltPerOperator)` where the salt is a
   per-operator-type constant, so two `UsdGenClump`s with the same seed do not correlate
   (ADR §6, artist §6.4). The salt is part of the operator's identity and changing it is an
   `algorithmVersion` bump (§11).
7. **Mask arithmetic.** `02-schema.md` §2.13 is canonical for the mask's names, tokens and
   resolved per-curve weight, and `04-operators.md` §5.2 owns the `combine` truth table and
   `biasGain` (R16). This rule states only what a *kernel* sees: `curveMask[c]`, resolved once per
   capture into one `VtFloatArray` per node (§4.1), and the 257-entry LUT.

```
curveMask[c] = the resolved per-curve weight of 02-schema.md §2.13   # capture, never evaluate
rampLut[i]   = ramp(i / 256)                        # i in [0, 256]; 257 entries

# per CV, in the inner loop
t            = hairT[view.Cv(c, i)] * 256.0f        # CvRagged(c, i) on the ragged path
j            = min(int(t), 255);  a = t - float(j)
rampWeight   = (1 - a) * rampLut[j] + a * rampLut[j + 1]
w(c, i)      = usdGen:blend * curveMask[c] * rampWeight
```

The `usdGen:blend` factor is applied **once**, by the framework's envelope pass (§8.5); a kernel
multiplies only `curveMask[c] * rampWeight` and never applies blend a second time. It is written
into the formula so that the weight is term for term the one `02-schema.md` §2.13 and
`04-operators.md` §5.2 define.

The interpolated read is the reason the LUT has a 257th entry: `j + 1` is then always in range and
both endpoints are exact samples, while a nearest read would quantise the ramp to 257 steps and band
a slow taper. `02-schema.md` §2.13 and `04-operators.md` §5.2 carry the same lerp, term for term
(R16). `hairT` is a **per-CV** plane, so it is indexed through `view.Cv(c, i)` (or
`view.CvRagged(c, i)`), never by the intra-curve index `i`.

8. **`preserveLength`**: after any displacement, segment lengths are restored root-locked (walk from
   the root, renormalise each segment to its captured rest length), as a host groomer and a DCC both do. It
   is a per-operator `bool`, applied inside the kernel because it must see the operator's own
   displacement.

### 8.4 Ragged kernels

One kernel body serves both paths. `USDGEN_FOR_EACH_CV(view, c, i)` expands to
`for (i = 0; i < view.cvCount; ++i)` with `view.Cv(c, i)` on the fast path and to a bounded loop
over `cvOffsets[c] .. cvOffsets[c+1]` on the ragged path. Two instantiations per kernel, one source.
Gate **E-1r** holds the ragged path at ≤ 2× the uniform path.

### 8.5 The envelope

`usdGen:blend` (`float`, default 1) is applied by the **framework**, not by kernels, using
`UsdGenBlendEnvelope(in, out, w)` with exact endpoints: `w <= 0` skips `Evaluate` entirely and
aliases the input; `w >= 1` runs no blend pass; otherwise one lerp over the chunk. The
exact-endpoint rule is usdRig's (`usdRig libs/rigExecMath/envelope.h:16-30`,
`research/A1-usdrig-graph.md` §5) and it matters because an artist at `blend = 1` must get bit-exact
operator output. `blend` is ignored with one diagnostic on nodes whose `TopologyEffect() != None`.

---

## 9. Engine API sketch

### 9.1 The five objects

```cpp
// usdGen/session.h
class UsdGenSession {
public:
    // called on the app / notice thread only
    void  AccumulateDirty(UsdGenPendingDirty &&);
    bool  NeedsCommit() const noexcept;
    UsdGenGenerationConstPtr Commit(double frame, UsdGenCommitReason);
    UsdGenGenerationConstPtr Generation() const noexcept;     // std::atomic_load, lock-free
    void  SetGraphDesc(UsdGenGraphDesc &&);                   // from usdGenImaging
    void  SetContext(UsdGenContext);                          // interactive | render (ADR §2.3)
    /// Publication mode for a density-scale drag (§6.3). While parked, tiles publish the full id
    /// set at max-density topology with collapsed CVs and zero widths for non-survivors, so
    /// element counts are fixed for the whole drag (S27, S28). End republishes the real counts.
    void  BeginDensityDrag();
    void  EndDensityDrag();
    const UsdGenStats     &Stats() const noexcept;
    const UsdGenNodeStats &NodeStats(UsdGenNodeId) const;
    UsdGenMotionCache     &Motion() noexcept;
private:
    std::mutex             _commitMutex;      // non-recursive; held only by Commit
    std::atomic<bool>      _dirty;
    std::atomic<uint64_t>  _generationRequested;
    UsdGenCompiler         _compiler;
    UsdGenGraph            _graph;
    UsdGenScheduler        _scheduler;
    UsdGenGenerationStore  _store;
    UsdGenMotionCache      _motion;
};

// usdGen/compiler.h
struct UsdGenCompileResult {
    bool                      ok = false;
    UsdGenEpoch               structuralDigest {};
    std::vector<UsdGenNodeId> rebuilt;        // nodes whose UsdGenOp was recreated
    std::vector<UsdGenNodeId> reordered;      // nodes whose topological index moved
    UsdGenDiagnostics         diag;
};
class UsdGenCompiler {
public:
    UsdGenCompileResult Compile(const UsdGenGraphDesc &, UsdGenGraph *out);
};

// usdGen/graph.h
class UsdGenGraph {
public:
    using NodeId = UsdGenNodeId;
    static constexpr NodeId InvalidNode = ~0u;

    void DirtyParameter(NodeId, const TfToken &param);
    void DirtyCapture  (NodeId);
    void DirtyTopology (NodeId);
    void DirtySurface  (UsdGenSurfaceId, UsdGenDirtyBits);
    void DirtyChunks   (NodeId, TfSpan<const UsdGenChunkId>);
    /// Bumps the capture epoch of every node whose UsdGenNodeDesc::maps names this prim
    /// (asset path edit, ReloadMaps(), textureGeneration bump). §3.6, UsdGenDirtyMap.
    void DirtyMap      (const SdfPath &mapPrim);
    /// Bumps the capture epoch of every node whose UsdGenNodeDesc::curves names this prim
    /// (a C3 BasisCurves changed: guide edit, re-freeze, re-import). §3.6, §4.5.
    void DirtyCurves   (const SdfPath &curvePrim);

    NodeId TerminalNode() const noexcept;
    const UsdGenCurveBuffer &Output(NodeId) const;
    /// That node's own chunk partition (UsdGenCurveBuffer::chunks); every node in one description
    /// shares the partition unless a generator changed the curve count.
    TfSpan<const UsdGenChunkDesc> Chunks(NodeId) const;
    /// The TERMINAL node's tile partition -- the one the publisher walks (§6.3).
    TfSpan<const UsdGenTileView>  Tiles()  const;
};
```

`UsdGenScheduler` is §5.3; `UsdGenGenerationStore` and `UsdGenGeneration` are §6.1;
`UsdGenChunkView` and `UsdGenTileView` are §1.2. `UsdGenTilePublication`,
`UsdGenInstancerPublication`, `UsdGenPrimSetSignature` and `UsdGenDirtyReport` are the imaging
boundary and are defined by contract C2 in `06-imaging.md`; the engine only fills them.
Nothing in these headers is installed until M7
(ADR §3), which is what lets the v-table churn while operators are written on top of it.

### 9.2 Diagnostics

```cpp
struct UsdGenNodeStats {                    // artist-facing: the stack profiler column
    TfToken  type; SdfPath path;
    double   captureMs, lastEvalMs, meanEvalMs;
    uint64_t curvesIn, curvesOut, chunksDirty, chunksTotal, bytesOwned;
    uint64_t captureHits, captureMisses;
    uint32_t warnings;                      // e.g. "guide angle rejected 42% of candidates"
};

struct UsdGenStats {                        // atomic counters, lock-free
    uint64_t commits, cookedNodes, cookedChunks, capturedNodes;
    uint64_t publishedTiles, interleavedBytes, noticeEntries;
    uint64_t recompiles, evictions, motionSamples, supersessions;
    std::array<UsdGenCommitTiming, 120> ring;   // last 120 commits, split by phase
};
```

`UsdGenNodeStats` (artist §4.10) is published per generation and feeds both the groom panel's stack
profiler ("`op03_clumpBig` is 60 % of your frame") and the CI gates, through
`UsdGenImaging_GetStatsJson()` on the C ABI — one number source, two consumers, so gate thresholds
and the artist's panel can never disagree.

`TF_DEBUG` codes: `USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`,
`USDGEN_CAPTURE`, `USDGEN_MEMORY`.

Trace scopes (`TRACE_SCOPE` / `TRACE_FUNCTION`, `pxr/base/trace/trace.h:30, 35` — 26.08 has no
`TfTrace` symbol), named after the **phase**; the gate each scope is evidence for is the last column
of this table and never part of the string (`10-build-dependencies-testing.md` §7.2), so a chrome
trace is directly comparable to the budget without embedding an id R40 may reassign:

| Scope | Covers | Gate |
|---|---|---|
| `UsdGen::Route` | `_PrimsDirtied` accumulation; locator → node → chunk → tile (§5.2) | SI-2, SI-3 |
| `UsdGen::Compile` | Merkle + graph digests, Kahn sort, sub-graph rebuild (§3) | E-6 |
| `UsdGen::Capture` | scatter, Morton sort, kd-tree, guide weights, clump ids, map bakes (§4) | E-4, L-4 |
| `UsdGen::Evaluate<opType>` | one node's chunk sweep (§5.4 step 4) | E-1, E-1r, E-2, E-3, E-7 |
| `UsdGen::Interleave` | SoA → AoS + per-tile extent (§6.3); the `I` term of §5.5 | E-1, S-2 |
| `UsdGen::Publish`, `UsdGen::Diff` | generation swap, prev/next diff, notice emission (§6.1) | SI-2, SI-3, SI-4 |
| `UsdGen::Motion` | `UsdGenMotionCache::Fill` (§7.2) | R-1 |

This is `09-performance-and-benchmarks.md` §6.2's table, and the two must stay identical or a
chrome trace stops matching the gate matrix (both re-checked row for row on 2026-09-05).

Profiling on this host uses the in-process SIGPROF sampler carried into `usdGenTestUtils` from
`prototypes/data-plane-benchmark/sampler.h`, because the host refuses `perf`
(`perf_event_paranoid = 4`) and `gdb -p` (yama `ptrace_scope`) —
`research/G-data-plane-engine-prototype-benchmark.md` host notes.

### 9.3 Configuration, none of it authored

| Knob | Default | Meaning |
|---|---|---|
| `USDGEN_CHUNK_SIZE` | 512 | curves per chunk, clamped [128, 1024] (§1.3) |
| `USDGEN_THREAD_LIMIT` | calibrated | arena concurrency; set to skip calibration (§5.3) |
| `USDGEN_MEMORY_BUDGET_MB` | 4096 | eviction budget (§6.4) |
| `USDGEN_CONTEXT` | unset | `render` applies `usdGen:renderDensityScale` (ADR §2.3) |
| `USDGEN_ENABLE` | 1 | scene-index plugin kill switch (`_IsEnabled`, `06-imaging.md`) |
| `USDGEN_OP_CHECKS` | 0 | debug-build operator contract wrapper (§8.1) |

The **complete** environment-variable registry is `10-build-dependencies-testing.md` §3.5 (R35);
this table is the engine subset, and every row above appears in that registry — `USDGEN_OP_CHECKS`
included, at default 0 (verified 2026-09-05). Machine tuning is env/config and **never** authored
into an asset (ADR §2.3):
a `chunkSize` in a layer is one workstation's number shipped with a character.

---

## 10. Cross-chunk operators (v3)

Two v3 operators genuinely need information the reference lane does not carry:
`UsdGenSmooth` in `neighbours` mode (smooth a hair against its spatial neighbours, not along its own
length) and `UsdGenCollide`/`UsdGenWind` (deformed-space, neighbour-dependent). For these, and only
these, a **two-pass gather/scatter node kind** is added in v3:

```
pass 1 (gather):   parallel over cells of a per-frame uniform grid built from the terminal buffer's
                   live roots; each cell reduces the CV data it owns into a small per-cell record.
                   Cells are disjoint, so the pass is race-free without locks.
barrier
pass 2 (scatter):  parallel over chunks; each chunk reads the (now immutable) cell records for its
                   own cell and its 26 neighbours, and writes only its own CVs.
```

Both passes are `tbb::parallel_for` inside the private arena; the grid is rebuilt per frame from
`boundsRest`-sorted roots, which is cheap precisely because roots are Morton-ordered (§1.7).

Three alternatives are rejected on evidence. **Chunk fan-in** (`UpstreamChunks()` returning a
neighbour set): cost unmeasured, and it reintroduces the read-write hazard VDF's 500-element grain
has (§0.4); all three judges rejected it. **A serial pre-pass** before each `Evaluate`: a serial
phase in the per-frame critical path does not survive the measured 3.8× TBB scaling. **Widening
chunks**: the MEASURED cliff of §0.3 (4096 → 9.40 ms) makes anything past 1024 unreachable, so it
is not a fallback for anything.

The gather/scatter kind's cost is UNMEASURED and sits behind the v3 gate in `11-roadmap.md`;
nothing in v1 or v2 depends on it.

---

## 11. Determinism and reproducibility

### 11.1 The guarantees

1. **Same inputs, same output, bitwise** — across runs, across thread counts, across machines with
   the same ISA. Gate **E-8** asserts bitwise identity at 1 / 2 / 4 / 8 / 20 threads with
   `-ffp-contract=off` (`09-performance-and-benchmarks.md` §5.1).
2. **Thread count never changes a result.** Every reduction has a fixed order; no result depends on
   unordered-container iteration; `tbb::parallel_for` is used only over disjoint chunk ranges.
3. **Curve identity is stable.** `curveId = UsdGenHash64(seed, faceIndex, k, kSaltScatter)` —
   64-bit, the SplitMix64 finalizer pinned by R12 (§1.7) — so it is independent of Morton order, of
   chunking, of the density **scales** and of LOD, and it has no width parameter that a
   `usdGen:density` edit could move. Sculpt deltas, clump ids and frozen epochs keyed by id survive
   any edit that does not change `usdGen:seed` or the surface's face indexing, and a mute/unmute of
   a generator preserves them too because the muted node keeps its capture (R14, §3.5). Uniqueness
   is probabilistic — ≈ 1.7e-6 chance of any collision at 8 M curves (ASSUMPTION, birthday
   arithmetic) — and the duplicate-id check of `05-static-curves-and-deformation.md` §2.3 row 10 is
   a **hard error**, so a collision is reported, never silently absorbed.
4. **The density scales never re-scatter.** Both decimate by
   **keep iff `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32`**, with
   `keepFraction = clamp(scale_groom · scale_description, 0, 1)`, `usdGen:densityScale` in the
   interactive context and `usdGen:renderDensityScale` in the render context, the two exclusive
   (R13). `kSaltDensity ≠ 0`, so the surviving set is never `{hairId < scale}` (R12): decimation is
   uncorrelated with shading. A lower scale is a strict subset of a higher one, not a new groom.
5. **Order is deterministic even when ambiguous.** Kahn with a namespace tie-break (§3.1).
6. **Digests are canonical.** Fixed-width encodings, sorted path lists, lengths before bytes; no
   pointer values, no locale, no iteration order.

### 11.2 What may change a look, and what may not

| Change | Look may change? | Mechanism |
|---|---|---|
| `uniform int usdGen:algorithmVersion` on an operator type | **yes, deliberately** | the version is in the structural digest (§3.3); a fixed kernel ships under a new version and old assets keep the old behaviour |
| a new operator type, a new parameter with a neutral default | no | absent parameters take defaults that reproduce the previous result |
| chunk size, thread limit, memory budget, arena calibration | **no — this is the hard rule** | scheduling and layout must never be observable in output; gate E-8 and the E-1/E-2 sweeps are what enforce it |
| tile count / `usdGen:tileTarget` | no (geometry), yes (prim set) | tiles partition the same curves; the curves are identical |
| eviction | no | eviction only discards caches that can be recomputed identically |
| a compiler upgrade or `-ffp-contract=fast` | possible | the CI job that builds with `fast` exists to keep epsilons honest, not to ship |

Kernel bug fixes therefore land as an `algorithmVersion` bump with the old path retained, so a shot
mid-render is never re-lit by an engine patch.

---

## 12. Gates

Tier assignment uses the T0–T4 vocabulary of `10-build-dependencies-testing.md`.
`proposal-performance.md` §11.2 labelled these "Tier 1"; under the T0–T4 naming they are **T0**,
because they need neither Hydra nor a stage — that is exactly what `UsdGenGraphDesc` buys (§2.1).
Recorded as a naming reconciliation, not a change of content. All thresholds are anchored to the
MEASURED baselines of §0.3 and their `EV-nnn` rows, and each cell names the thread count its
baseline was taken at: E-1's baseline is the 8-thread number because that is the arena the engine
runs in (§5.3); E-2's and E-3's sparse baselines exist only at 20 threads and are re-measured inside
the arena at the milestone each gate runs (E-2 at M1, E-3 at M2). The threshold itself is unchanged
either way — it is a ceiling, not a target. **The values below are
`09-performance-and-benchmarks.md` §5.1's, cited not re-decided** (R40).

| Gate | Tier | Assertion | Threshold | Runs at |
|---|:--:|---|---|---|
| **E-1** chain throughput | T0 | 5-op chain, 100 k × 8 CV, full run in the private arena at **8 threads** | **≤ 1.5 ms** (R27, `09-performance-and-benchmarks.md` §5.1; baseline **1.02 ms @8 thr** MEASURED, `EV-001` / `EV-008`; 1.72–1.91 ms @20 thr) | M1 (`09-performance-and-benchmarks.md` §5.1, R40); re-run at every later milestone as a regression policy |
| **E-1r** ragged path | T0 | the same chain over a ragged buffer (mixed CV counts, `cvCount == 0`) | ≤ 2× the E-1 number on the same host | M2 |
| **E-2** sparse edit | T0 | 1 % of chunks dirty on all 5 nodes | ≤ 0.10 ms (baseline 0.035–0.044 **@20 thr**, `EV-002`; re-measured in the arena at M1) | M1 |
| **E-3** last-parameter edit | T0 | terminal `ValueParameter` only | ≤ 0.6 ms (baseline 0.179–0.411 **@20 thr**, `EV-004`; re-measured in the arena at M2) | **M2** (`09-performance-and-benchmarks.md` §5.1, R40) |
| **E-4** capture | T0 | `UsdGenGuideInterpolate` capture, 100 k roots, 4 k guides, 8 threads; nanoflann kNN | ≤ 25 ms and **linear in roots**; 1 M roots reported | **M0 pre-work**, enforced M3 |
| **E-5** memory | T0 | RSS at 1 M × 8 CV, 5 nodes, per-node buffers | ≤ 800 MB (measured anchor 668 MB for the point buffers alone, `EV-007`; §1.8's other planes put the shipped chain at 950–1050 MB DERIVED, so the gate is **at risk**, `09-performance-and-benchmarks.md` §2.6, §5.1) | **M3, re-run M7** (`09-performance-and-benchmarks.md` §5.1, R40) |
| **E-6** recompile | T0 | append one node to a 200-node groom | ≤ 0.2 ms **and exactly 1 node rebuilt** | M1 |
| **E-7** thread scaling | T0 | 1 → 8 threads on the E-1 workload; calibration ≤ 5 ms | ≥ 3× (baseline 3.8×, `EV-008`) | M1 |
| **E-8** determinism | T0 | same inputs at **1 / 2 / 4 / 8 / 20 threads**, `-ffp-contract=off` (`09-performance-and-benchmarks.md` §5.1) | **bitwise identical** | **M1, re-run M4** (`09-performance-and-benchmarks.md` §5.1, R40). Re-runs at later milestones are regression policy, not a second gate assignment |

Notes that make these runnable rather than aspirational:

* E-1, E-2, E-3, E-8 reuse `prototypes/data-plane-benchmark/tbbBench.cpp`'s workload shape and its
  checksum discipline (three engines agreed on `3355.7599`), so a regression is visible as a number
  and as a checksum.
* E-4 is the only gate whose baseline does not exist yet. It is M0 pre-work precisely because the
  M3 operator schedule assumes ~10 ms and nothing has measured it (§4.4).
* E-6's "exactly 1 node rebuilt" is asserted from `UsdGenCompileResult::rebuilt`, not from a timing —
  timings drift, the count does not.
* E-5 is measured with `/usr/bin/time -f "%M KB"` on a single-run process, the way the 668 MB
  baseline was measured.
* **`09-performance-and-benchmarks.md` §5 is the single gate registry** (R40); the rows above cite
  it and must agree with it. SI-1…**SI-9** (exactness, invalidation, cook count, torn reads, chain
  order, initial population, adapter coverage, auto-apply, sessions and pruning-wrapper cost) are
  `06-imaging.md`'s and are registered in 09 §5.2; **S-1…S-12** are the Storm gates of 09 §5.3 and
  **L-1…L-5** the look gates of 09 §5.4; T-1…T-5, T-EXPR-1 and T-PTEX-1 are `08-tools.md`'s and
  `07-look-maps-expressions.md`'s; **B-1** (the `DT_NEEDED` link-rule check, T0, M0, R37) is
  `10-build-dependencies-testing.md`'s; R-1…R-3 are release criteria and never a milestone exit
  (R39, R40).

### 12.1 Deviations from the ADR and the source proposals, recorded

Four earlier entries have been withdrawn because the ADR addendum ratified them and they are no
longer deviations: the `min(nChunks, …)` tile term is **R21 verbatim** (§1.4); the graph digest is
where R21's description-level terms land, not an extra digest the ADR forbids (§3.3); the 31-bit
`curveId` packing is **replaced** by R12's 64-bit hash (§1.7); and `VtArray::IsUnique()` is replaced
by R20's `Vt_ArrayForeignDataSource` (§6.2). What remains:

| # | Deviation | Why |
|---|---|---|
| 1 | `UsdGenChunkDesc::liveCount` added | R13's decimation-by-stable-id needs a fixed chunk partition with a variable live count, or a density-scale change would re-chunk and drop capture caches (§1.2) |
| 2 | risk §4.5's authored `usdGen:cacheOutput` is dropped | ADR §2.3 puts memory tuning in env/config; eviction (§6.4) achieves the same effect per machine, and R8 drops the property everywhere |
| 3 | `UsdGenOp` has no tangent planes; risk §4.1's `tx/ty/tz` are not stored | ADR §5.4 variant A publishes no `hairTangent` primvar; variant B derives it at interleave (§1.1) |
| 4 | performance §5.2's `Topology()`/`CaptureParameters()` renamed to `TopologyEffect()`/`TopologyParameters()`, and its "tier 1" gates are **T0** | this plan's engine vocabulary; risk §4.2's two-set classification is what §3.6 keys on (§8.1), and R2 makes T0 the Hydra-free engine tier (§12) |
| 5 | `usdGen:enabled` pass-through is implemented as input aliasing, not a memcpy | observationally identical, strictly cheaper, and inputs are read-only by contract (§3.5, R14) |
| 6 | `UsdGenGeneration` is a `std::shared_ptr`, not a `TfRefBase`/`TfRefPtr` | R20 requires `std::atomic_store`/`std::atomic_load`; `TfRefPtr` has neither in 26.08 (verified by grep of `pxr/base/tf/refPtr.h`, `refBase.h`), and the measured 0-torn-read evidence and usdRig's shipped store both use `std::atomic_load` on a `std::shared_ptr` (§6.1) |
| 7 | `UsdGenSpace::Inherit` is the C++ spelling of the token `auto`; the engine has no `world` | R9 fixes three tokens and names the enumerator `Inherit`; after flattening, deformed space *is* world space (S4) (§1.6) |

**Sibling registries this document depends on, re-checked 2026-09-05.** R7 makes `02-schema.md`
the normative property registry and R40 makes `09-performance-and-benchmarks.md` §5 the single gate
registry, so where this document and a registry disagree, the registry is what ships. Every
dependency below now reads as this document assumes: `02-schema.md` §6.5 classes both density
scales as **value** (§3.6); `10-build-dependencies-testing.md` §3.5 registers `USDGEN_OP_CHECKS` at
default 0 (§9.3); `02-schema.md` §2.5 declares `preceding` an accepted v1 alias for `final` (§1.6);
`02-schema.md` §2.17 fixes the ramp tokens at `linear | catmullRom | bspline | constant`, default
`catmullRom` (§2.2); `06-imaging.md` §3.9 makes trigger (c) unconditional (§5.4);
`09-performance-and-benchmarks.md` §5.1 carries E-1 ≤ 1.5 ms, E-3 at M2 and E-8 at M1 with a re-run
at M4, and its §6.2 `UsdGen::Compile` row covers the Merkle **and** graph digests while
`UsdGen::Interleave` names E-1 (§9.2); `05-static-curves-and-deformation.md` §2.2's
`UsdGenSourceArrays` carries `TfToken curveRole` (§4.5) and its §4.4 schedules a rest-class node
downstream of a deformed-class node in the tail under §1.6's closure rule rather than rejecting it;
and `02-schema.md` §2.13 and `04-operators.md` §5.2 evaluate the along-curve ramp with the same
`rampLut[j]`/`rampLut[j + 1]` lerp as §8.3 rule 7.

---

## 13. Testing

What proves this document, by tier and gate. `10-build-dependencies-testing.md` owns the harness;
this section owns the assertions.

### 13.1 T0 — engine, no Hydra, no USD (every commit)

Binaries: `testUsdGenGraph`, `testUsdGenMath`, `testUsdGenOps`, `testUsdGenChunking`
(`10-build-dependencies-testing.md` §5.6's T0 engine names; the scheduler's T0 assertions run in
`testUsdGenGraph`). Inputs are `UsdGenGraphDesc` literals and synthetic buffers.

| # | Assertion | Gate |
|---|---|---|
| 1 | Kahn order with a namespace tie-break is stable across 100 shuffled input orders | — |
| 2 | a cycle in `usdGen:input` is a compile error naming the offending prim, and nothing evaluates | — |
| 3 | appending a node leaves every prefix digest identical and rebuilds exactly one node | E-6 |
| 4 | a `usdGen:label` edit produces no routing entry, no dirty chunk and no commit | — |
| 5 | `usdGen:enabled=false` on a styler: no recompile, no re-capture, output equals the input buffer bit for bit | — |
| 6 | `usdGen:enabled=false` on a generator: **no recompile**; the muted node keeps its own capture; downstream re-captures; topology version bumped; the id set after unmute is identical to the id set before (R14) | — |
| 7 | `usdGen:seed` edit re-captures and does **not** recompile | — |
| 8 | `TopologyParameters() ∪ ValueParameters()` equals the mapped property set, for every registered op | — |
| 8b | `curveId`s (`uint64`, R12) are **unique** across 8 M curves scattered on a 200 k-face scalp — a collision fails the test and must raise the hard error of `05-static-curves-and-deformation.md` §2.3 row 10 — and are unchanged by a density-scale drag, a `usdGen:density` edit on faces that did not change, a chunk-size change and a Morton re-sort | — |
| 9 | 5-op chain, 100 k × 8, full run; sparse run; last-param run | E-1, E-2, E-3 |
| 10 | ragged buffer (mixed CV counts) through the same chain, same checksum as the resampled uniform equivalent within tolerance | E-1r |
| 11 | `UsdGenGuideInterpolate` capture over 100 k roots / 4 k guides, and over 1 M roots | E-4 |
| 12 | RSS at 1 M × 8 CV with 5 nodes | E-5 |
| 13 | 1 → 8 thread scaling; calibration cost | E-7 |
| 14 | bitwise identical output at 1 / 2 / 4 / 8 / 20 threads | E-8 |
| 15 | interruption: a superseded commit publishes nothing, returns generation N−1, and leaves the session dirty | — |
| 16 | eviction never drops the tail base, a live-override target, or the published generation | — |
| 17 | `UsdGenMotionCache` hit on a whole-frame offset while scrubbing forward; lerp between bracketing offsets; clamp outside | — |
| 18 | envelope endpoints: `blend=1` is bit-exact operator output, `blend=0` is bit-exact input | — |
| 19 | `preserveLength` restores captured rest lengths to within 1e-6 after a displacement | — |
| 20 | two operators of the same type with the same `usdGen:seed` produce uncorrelated masks (salt); and the set surviving `UsdGenHash32(curveId, kSaltDensity)` decimation is uncorrelated with `hairId = UsdGenHash32(curveId, 0)/2^32` (R12) | — |
| 21 | `DT_NEEDED` of `libusdGen.so` names none of `usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`, `usdAppUtils` (R37) | **B-1** |
| 22 | `usdGen:space = "world"` is not a token: an authored `world` is a compile error naming the prim, and `auto` resolves to the type's `Space()` (R9) | — |

### 13.2 T1 — scene index (every commit, the primary regression suite)

Owned by `06-imaging.md`, but four assertions belong to this document's contracts and must exist:
a value-parameter edit dirties exactly `primvars/points/primvarValue` + `extent/*` on the affected
tiles and nothing else (SI-2); ten interactive events produce exactly ten commits, **each on the
thread its trigger assigns** — the app thread for trigger (a), the notice thread for (b) and (c) —
and never on a Hydra reader thread, with zero cooks inside `GetPrim` (SI-3, whose criterion is
`09-performance-and-benchmarks.md` §5.2's; trigger (c) is unconditional, R32);
`points.size() == Σ curveVertexCounts` on every published tile (SI-1); and a
density-scale drag in **parked** mode (§6.3) publishes a byte-identical `curveVertexCounts` on
every move, dirtying only `points` and `widths` (S28). The router's
locator→node table is asserted here because it is where the locators exist.

### 13.3 T2 — Storm through the EGL harness

No engine gate is T2. The engine's contribution is that the tile the harness draws came from a
generation whose diff said it changed; S-2 (deform frame ≤ 2.0 ms over static) is the number that
catches an engine that publishes tiles it did not change.

### 13.4 T3 — `testusdview`

T-1 (brush move ≤ 1 ms Python + engine at 100 k) exercises the live-override path down to
`DirtyChunks`, which is the only engine surface the app touches per move.

### 13.5 T4 — workstation

R-1 (hdPrman, three motion samples) asserts `k·tail` evaluations rather than `k·chain`, read from
`UsdGenStats::motionSamples` and the per-node stats. T4 gates are **release** criteria, never
milestone exits (ADR §7).

---

## 14. Out of scope

* **A public operator ABI.** `UsdGenOpRegistry` is internal in v1 and v2; third-party operators are
  v3 (ADR §3). Engine headers are not installed until M7.
* **GPU evaluation.** Everything here is CPU. A GB10-class machine could plausibly run these kernels
  in HGI compute, but nothing on this host has exercised it and the v3 "GPU tail" is a roadmap item,
  not a design (`research/G-data-plane-engine-prototype-benchmark.md`, open questions).
* **An OpenExec/VDF backend.** S16 keeps it as a future adapter behind the same operator interface;
  curve arrays never go through OpenExec (§0.4).
* **Progressive/async generation.** `asyncAllow`/`asyncPoll` (S20) is `06-imaging.md`'s and M8's.
* **Cross-chunk operators in v1/v2.** §10 states the v3 shape and the rejected alternatives; no v1 or
  v2 operator may need it.
* **The double-buffered publish ring (I6).** An M7 optimisation using
  `Vt_ArrayForeignDataSource`'s detached callback (R20); CoW is the shipped v1 handoff (§6.2).
  Motion profiles P0/P1/P2 are **v1** (M7, R38) and are in scope; only the ring is deferred.
* **Hydra data sources, notice emission, prim naming, the tile contract C2.** `06-imaging.md`.
* **Operator parameters and their defaults.** `04-operators.md`; this document names only the
  properties whose *engine* meaning it defines (`usdGen:input`, `enabled`, `seed`, `space`,
  `readPhase`, `blend`, `algorithmVersion`, `tileTarget`).

---

## 15. Sources

**Binding.** `design/adr-v1.md` §9 addendum, rulings **R1** (I1–I8 vs P0/P1/P2, `EV-nnn`), R3
(class and dirty-bit names), R7/R8 (02 is the normative registry), R9 (`space`/`readPhase` tokens),
R11 (ramps), R12/R13 (`curveId`, `hairId`, decimation), R14 (`usdGen:enabled`), R17
(`algorithmVersion`), R20 (generation handoff and buffer reuse), R21 (tile arithmetic), R22 (commit
order), R23 (`curveSets`, `UsdGenSurfaceSample`), R24 (`elementSize = 3`), R25 (router generated
from 02 §6), R26 (guide sizing), R27 (E-1 ≤ 1.5 ms and the 8-thread baseline rule), R32 (commit
triggers), R35 (env registry), R37 (link rule, gate B-1), R38 (v1 = M0–M7), R40 (09 §5 is the gate
registry), R41 (09 §0.2 is the frame ledger), R42 (number tags).
`design/adr-v1.md` §1 (amendments S23/S24/S29), §2.1–§2.3 (types, properties),
§3 (contracts C1–C5), §4 (engine: §4.1 adopted invariants, §4.2 the five fixes, §4.3 commit
triggers, §4.4 initial population, §4.5 sessions), §5.4 (`hairTangent` fork), §6 (operator rules,
build), §7 (milestones M0–M8, gates), §8 (document map). `design/brief-v1.md` §1 (R1–R9), §2.3
(S17–S20), §2.4 (S21–S26), §2.5 (S27–S32), §2.7 (S37–S38), §2.9 (S44–S45).

**Sibling registries this chapter cites rather than restates.**
`appendix-A-evidence-ledger.md` §2 — rows `EV-001`…`EV-083`, the only citation handle for a
measured number (R1, R42); every MEASURED cell of §0.3, §0.4 and §5.5 names its row, and §2.11
carries the DERIVED and ASSUMPTION provenance of the numbers this chapter budgets against.
`09-performance-and-benchmarks.md` §0.2 (the single frame ledger, R41), §2.1 (the cost-model
constants §5.5 shares), §2.6 (the memory model §1.8 restates), §5.1–§5.4 (the single gate registry,
R40) and §6.2 (the trace-scope table §9.2 must equal). `02-schema.md` §2.5, §2.13, §2.17 and
§6 (the normative property registry and dirty classes, R7, R25).
`10-build-dependencies-testing.md` §3.5 (the single env-var registry, R35) and §5.1 (tiers T0–T4).
`06-imaging.md` §3.9 (commit triggers) and §5.1 (invalidation discipline).
`04-operators.md` §0.7 (spaces), §1.2 (emitted primvars) and §5.2 (mask arithmetic).
`05-static-curves-and-deformation.md` §2.2 (`UsdGenCurveLoader`), §2.3 (the validation ladder) and
§4.4 (where `UsdGenDeform` sits).

**Proposals.** `design/proposal-performance.md` §0.2–§0.3 (ledger; its engine principles P1–P8 are
**I1–I8** here, R1), §5.1–§5.11 (the engine chapter — this document's primary source), §6.2, §6.5,
§7.4, §11.1–§11.2. `design/proposal-risk.md` §4.1–§4.3 (`UsdGenGraphDesc`,
`TopologyParameters`/`ValueParameters`, `UsdGenOpRegistry`), §4.5, §9.1 (tiers T0–T4).
`design/proposal-artist.md` §4.6, §4.10 (`UsdGenNodeStats`), §6.4 (seed salt, `preserveLength`,
`Space()`).

**Judge reports (defects avoided).** `design/judge-evidence.md` §2.1 (S14 unaddressed; adapter
coverage), §2.3, §4. `design/judge-delivery.md` §3 (digest inversion, the I6 torn-read hazard,
ragged buffers, machine tuning in the asset), §5, §6. `design/judge-artist.md` §3 (interruption
re-arm, `usdGen:active`, the "widen chunks" cliff), §4.

**Research.** `research/G-data-plane-engine-prototype-benchmark.md` §0, §2, §3.1–§3.5, §4, §5, §6,
§8 (every engine number in §0.3 and §0.4).
`research/G-evaluation-scheduling-and-batching.md` §4, §5, §7, §9, §10 (notice counts, cook models,
thread-safety rule, cascade cost). `research/G-motion-blur-sampling-strategy.md` §4.1–§4.5, §5
(profiles, cache key, lerp/clamp, 19.2 MB/sample, 0.97–1.00 ms passes).
`research/G-storm-throughput-and-prim-granularity.md` §1.3–§1.6, §1.8–§1.9 (upload unit, padding,
topology cost, per-prim overhead). `research/G-tool-loop-array-transport-and-cv-picking.md`
§1.2–§1.4. `research/G-freeze-bake-undo-and-frozen-reentry.md` §2.2 (frozen re-entry as a source
node). `research/A1-usdrig-graph.md` §3.2–§3.3, §4, §5, §10 (digest content and cost, snapshot
store, determinism contract, Kahn/cycle detection, falloff LUT).
`research/A7-prior-art-grooming.md` §1.5, §9.1, §9.3 (guide/clump capture shapes; the 10 % guide
density R26 budgets for). `research/A8-seexpr-ptex-libs.md` §1, §2 (SeExpr 13–117 ns/eval, Ptex
23 ns/lookup, nanoflann 1.12.1). `research/A3-usdrig-tools.md` §6 (document conventions).
`research/ENVIRONMENT.md` (host facts and the CORRECTIONS block).

**Prototypes.** `prototypes/data-plane-benchmark/{tbbBench.cpp, vdfBench2.cpp, simdBench.cpp,
kernel.h, sampler.h, results_main.txt, results_scale.txt}` — carried into `usdGenTestUtils` and into
gates E-1, E-2, E-3, E-5, E-7, E-8.

**OpenUSD 26.08** (`<openusd-src>`, tag v26.08), each verified by grep for this
document: `pxr/imaging/hd/renderIndex.cpp:1830-1862` (parallel Rprim sync);
`pxr/imaging/hd/sceneIndex.h:98,109` (`GetPrim` threadsafe);
`pxr/imaging/hd/sceneIndexObserver.h:123,132,143,151` (observers not threadsafe);
`pxr/imaging/hdSt/basisCurves.cpp:932-935` (points fastpath), `:975-990` (Storm reads `GetValue(0)`);
`pxr/imaging/hd/changeTracker.cpp:955-979` (`DirtyPrimvar` is one bit for everything else);
`pxr/imaging/hd/dataSourceLocator.h:321-332` (`Intersects`);
`pxr/base/vt/array.h:39-54` (`Vt_ArrayForeignDataSource` and its `detachedFn` / `_ArraysDetached`
— the buffer-reuse mechanism R20 pins), `:945-950` (`IsIdentical`), `:1023-1026` (private
`_IsUnique`; there is no public `VtArray::IsUnique()`);
`pxr/base/tf/refPtr.h`, `pxr/base/tf/refBase.h` (no `atomic_load`/`atomic_store` anywhere — the
reason §6.1 uses `std::shared_ptr`);
`pxr/base/work/threadLimits.h:31-63` (`WorkGetPhysicalConcurrencyLimit`, `WorkSetConcurrencyLimit`),
`:40-44` (`WorkHasConcurrency` reads the process-global `PXR_WORK_THREAD_LIMIT`);
`pxr/base/work/loops.h:170-190` (`WorkParallelForN` short-circuits to `WorkSerialForN` when
`WorkHasConcurrency()` is false);
`pxr/base/work/workTBB/loops_impl.h:31-56` (`WorkImpl_ParallelForN` is a plain `tbb::parallel_for`
with an isolated `task_group_context` — it inherits the calling thread's arena, it does not make
one);
`pxr/exec/vdf/scheduler.cpp:855-872` (the hardcoded 500-element grain);
`OpenUSD_26_08/include/tbb/task_arena.h:282-283` (`tbb::task_arena(int, unsigned)`).

**usdRig** (`<usdrig-src>`), each verified by grep for this document:
`libs/rigExecImaging/snapshotStore.h:284-285` (`std::shared_ptr<const RigExecImagingSnapshot>`),
`:330` / `:369-370` / `:376-377` (`std::atomic_load` / `std::atomic_store` publish-read pair),
`:326-378` (the snapshot diff §6.1 copies);
`libs/rigExecImaging/sceneIndices.cpp:2507-2515` (notice order Removed → Added → Dirtied);
`libs/rigExecImaging/bridge.cpp:1284-1288` (one full rig evaluation per shutter offset — the lever
§1.6 has and usdRig does not);
`libs/rigExecMath/weightFields.h:43-45` (`RigExecFalloffLutSize = 257`);
`libs/rigExecMath/envelope.h:16-30` (exact envelope endpoints);
`libs/rigExecMath/simdKernels.cpp:14-45` (SSE2-only kernel, scalar on aarch64);
`libs/rigExec/rigEvaluator.cpp:1175-1183` (animated scalars excluded from the digest),
`:1211-1229` (falloff LUT bake), `:3028-3050` (cycle DFS), `:4025-4068` (Kahn tie-break);
`docs/spec.md:1199-1208` (the determinism contract).
