# G — Storm throughput and prim granularity for a hair/fur plugin (OpenUSD 26.08)

**Scope.** What a `basisCurves` prim actually costs Storm per frame — for a
points-only deform, a colour-only edit, and a density (topology) scrub — and how
many prims the groom should be split into. GPU numbers are **impossible on this
host** (no display, GLX-only garch; see `ENVIRONMENT.md`), so Part 1 establishes
the *cost structure* from source plus **CPU-side measurements that do run here**,
and Part 2 is the exact benchmark protocol for a workstation.

**Host for all measured numbers:** Linux aarch64, 20× Cortex-X925, OpenUSD
26.08 install at `$USD`. Probes and raw output:
`<session-scratch>`
(`PROBE_OUTPUT.txt` has everything verbatim). Six probes were built and run;
one GL harness (`hairbench.cpp`) was built and **core-dumps here for want of a
display**, exactly as `ENVIRONMENT.md` predicts.

Absolute paths below are under `<openusd-src>/` unless stated.

---

## 1. The DirtyPoints-only update path

### 1.1 What executes

| Step | File:line | Cost |
|---|---|---|
| Material shader refetch on **any** primvar dirty (incl. DirtyPoints) | `pxr/imaging/hdSt/basisCurves.cpp:171-176` | `HdStGetMaterialNetworkShader` per prim per frame |
| `_UpdateInstancer` + `HdStUpdateInstancerData` | `basisCurves.cpp:183-199`; `hdSt/primUtils.cpp:1114-1127` | early-outs unless `DirtyInstancer` |
| Constant primvars / extent / primId | `basisCurves.cpp:201-218`; `primUtils.cpp:887-911` | skipped unless those bits set |
| **Points fastpath** — deliberately avoids `GetPrimvarDescriptors` | `basisCurves.cpp:930-998` | `GetTerminalSceneIndex()->GetPrim(id)` per prim, then `HdPrimvarsSchema` pull |
| Interpolater buffer source built | `basisCurves.cpp:988-995` → `hdSt/basisCurvesComputations.h:184-280` | see §1.2 |
| BAR reuse test | `basisCurves.cpp:1002-1003` (`HdStCanSkipBARAllocationOrUpdate`, `primUtils.cpp:394-413`) | returns early only if there is nothing to upload |
| BAR update | `basisCurves.cpp:1025-1035`; `hdSt/resourceRegistry.cpp:1226-1268` | **no migration** when the new specs are a subset and the BAR is mutable (`:1250-1268`) — the existing range is reused |
| Queue source | `basisCurves.cpp:1046-1048` | |
| Commit | `hdSt/resourceRegistry.cpp:861-1085` | parallel Resolve (`:862-871`), Resize (`:913-925`), Reallocate (`:961-975`), Copy (`:984-1015`), Flush (`:1018-1036`) |

The fastpath is only taken when `DirtyPoints` is set **and**
`DirtyNormals|DirtyWidths|DirtyPrimvar` are all clear (`basisCurves.cpp:932-935`).
Any co-dirty primvar drops the prim into the full loop at `:875-928`, which pulls
the whole vertex primvar descriptor list.

### 1.2 Is the CPU-side `VtArray` copied? — **No, in the exact-size case**

`HdSt_BasisCurvesPrimvarInterpolaterComputation<T>::Resolve()`
(`hdSt/basisCurvesComputations.h:191-247`) allocates `VtArray<T> primvars(numVertsExpected)`
(`:193`) and then, if `authoredSize == numVertsExpected`, does `primvars = _authoredPrimvar`
(`:202-203`) — a copy-on-write share, **not** a deep copy. Measured (probe A):

```
[exact size] out=800000 fallback=0 sharesInputBuffer=1 resolve=0.22 ms
```

`sharesInputBuffer=1` means the buffer source's `GetData()` pointer *is* the
authored array's. The only real cost is the thrown-away allocation at `:193`:

| points | thrown-away `VtArray(n)` alloc+value-init | alloc + `memcpy` reference |
|---|---|---|
| 50 000 (0.57 MB) | 0.098 ms | 0.134 ms |
| 200 000 (2.29 MB) | 0.481 ms | 0.561 ms |
| 800 000 (9.16 MB) | 2.390 ms | 2.760 ms |

(cold, i.e. fresh mmap + page faults; when the allocator recycles the block it
drops to the 0.22 ms seen in probe A). Either way this is *bandwidth*, not
copying — the plugin cannot avoid it and should not try.

### 1.3 Is the upload a sub-range? — **Per-prim whole-array, into a sub-range of a shared VBO**

`HdStVBOMemoryManager::_StripedBufferArrayRange::CopyData`
(`hdSt/vboMemoryManager.cpp:624-682`) blits `srcSize = bufferSource->GetNumElements() * bytes`
— the **entire** primvar array of that prim — to `destinationByteOffset = bytesPerElement * _elementOffset`
(`:669, 678`). There is no per-CV or per-curve delta path anywhere in `hdSt`.
**The upload granularity is the prim.** That is the single most important fact
for prim-granularity choice: touching one CV re-uploads the whole chunk.

Below **512 KiB** the copy is memcpy'd into a staging buffer and coalesced with
adjacent copies; above it, it goes straight to `CopyBufferCpuToGpu`
(`hdSt/stagingBuffer.cpp:70-76`). 512 KiB / 12 B = **43 691 points = 5 461
curves at 8 CV** — the chunk size at which the staging path is bypassed.

### 1.4 Padding tolerance — **a longer `points` array is NOT tolerated**

`hdSt/basisCurves.cpp:651-654` claims *"The points primvar is permitted to be
larger than the number of CVs implied by the topology"* — that comment is about
the topological-visibility BAR only. The vertex-primvar interpolater
(`basisCurvesComputations.h:202-234`) accepts a longer array **only when
`_topology->HasIndices()`** (`:212-221`); otherwise it falls to
`std::fill(..., _fallbackValue)` at `:223`. Measured (probes B/C/D):

| case | authored | expected | result | resolve |
|---|---|---|---|---|
| exact | 800 000 | 800 000 | shares input buffer, correct | 0.22 ms |
| **padded, no `curveIndices`** | 1 000 000 | 800 000 | **fallback (1,0,0) for every CV** + `TF_WARN` | 0.54 ms |
| padded, with `curveIndices` | 1 000 000 | 800 000 | correct, but `resize()` detaches → full copy | **3.46 ms** |
| fat array + shrunk counts (density scrub) | 1 000 000 | 800/400/200 k | **fallback** | — |

So: **a fixed-capacity points array with a shrinking `curveVertexCounts` renders
the whole prim solid red at width 1.0.** The padding strategy only works if you
also author `curveIndices` covering exactly the live CVs, and that costs 15×
more per update (`primvars.resize()` at `:220` detaches the shared array) *and*
makes `_ComputeNumPoints` an O(n) `max_element` scan
(`hd/basisCurvesTopology.cpp:19-31`) *and* forces the index builder to remap
every patch index through `curveIndices` (`hdSt/basisCurvesComputations.cpp:352-373`).
**Padding is not a viable interactive-generator strategy. Do not use it.**

Corollary for the plugin's publisher: assert `points.size() == Σ curveVertexCounts`.
The failure mode is a `TF_WARN` plus silently wrong geometry.

### 1.5 DirtyPrimvar (displayColor only)

`DirtyPrimvar` is one bit for *every* primvar except points/normals/widths
(`hd/changeTracker.cpp:958-979`). Consequences:

* The points fastpath is skipped (`basisCurves.cpp:932-935`); the full vertex
  loop at `:875-928` runs and calls `HdStGetPrimvarDescriptors`. `points` itself
  is still guarded by `IsPrimvarDirty` → `DirtyPoints`, so **points are not
  re-uploaded** — but *every other* vertex/varying/uniform primvar is
  (`:898-900`, `:1103-1105`, `:1187-1189`).
* `_PopulateVaryingPrimvars` and `_PopulateElementPrimvars` both run
  (`basisCurves.cpp:237-244`).
* `hasDirtyPrimvarDesc` is true, so `HdStGetRemovedPrimvarBufferSpecs` runs over
  the BAR's specs for all three ranges (`:1010-1015`, `:1136-1141`, `:1226-1231`).

**Design consequence: put per-strand colour in `uniform` (per-curve) interpolation
and in its own element-primvar BAR, and never co-dirty it with anything else.**
A uniform `displayColor` for 3 125 curves is 37.5 KB — three orders of magnitude
cheaper than the points BAR of the same prim.

### 1.6 Topology change (density scrub)

| Step | File:line | Measured (100k curves × 8 CV bspline) |
|---|---|---|
| `ComputeHash` over counts+indices | `hd/basisCurvesTopology.cpp:112-131` | 0.017 ms |
| Topology instance registry lookup | `basisCurves.cpp:673-690` | hash only |
| Index build, cubic → `int32[4]` per segment | `hdSt/basisCurvesComputations.cpp:186-393` | **2.6 ms warm / 5.2 ms cold**, 500 000 patches = 7.63 MB |
| Index build, linear → `int32[2]` per segment | `:230-284` | 2.30 ms, 700 000 lines = 5.34 MB |
| BAR `Resize` → **whole aggregated buffer array reallocated** | `hdSt/vboMemoryManager.cpp:605-620` | see below |
| `Reallocate` — new GPU buffer, GPU→GPU relocate every range, destroy old, `IncrementVersion()` | `vboMemoryManager.cpp:272-427` | GPU-side, UNMEASURED |
| Geometric-shader change → `HdStMarkDrawBatchesDirty` | `basisCurves.cpp:405-415` | |
| Deep validation of **all** batches, `_IsAggregated` per draw item | `hdSt/commandBuffer.cpp:398-455`; `hdSt/indirectDrawBatch.cpp:801-834` | O(total draw items) |
| Rprim index version change (prim add/remove) → full dirty-list gather over all rprims | `hd/dirtyList.cpp:226-275` | O(total rprims) |

The load-bearing line is `vboMemoryManager.cpp:605-620`: **any** element-count
change — grow *or* shrink — unconditionally calls `SetNeedsReallocation()`, and
`Reallocate` then rebuilds the *entire* striped buffer array that this prim's
BAR is aggregated into. Since every hair chunk with identical buffer specs
aggregates into the same array (`HdStVBOMemoryManager::ComputeAggregationId`,
`vboMemoryManager.cpp:61-72`), **one chunk's density change relocates the whole
groom's points VBO and bumps its version, invalidating every batch that draws it.**

### 1.7 Chunking helps the CPU, and by how much

`HdStResourceRegistry::_Commit` resolves pending buffer sources with
`WorkParallelForTBBRange` (`resourceRegistry.cpp:868-871`), so buffer-source work
parallelises **across prims but never within one**. Measured (probe, 20 cores):

| chunks | pts/chunk | points resolve serial | parallel | patches/chunk | index build serial | parallel |
|---|---|---|---|---|---|---|
| 1 | 800 000 | 0.225 ms | 0.224 ms | 500 000 | 2.607 ms | 2.599 ms |
| 8 | 100 000 | 0.226 | 0.438 | 62 500 | 2.462 | 1.651 |
| **32** | 25 000 | 0.230 | **0.121** | 15 625 | 2.458 | **0.659** |
| **128** | 6 248 | 0.243 | **0.053** | 3 905 | 2.496 | **0.572** |
| 1000 | 800 | 0.370 | 0.070 | 500 | 2.981 | 0.748 |

Points resolve is bandwidth-bound and cheap either way (≤0.4 ms for the whole
groom). **The index build is where chunking pays: 2.6 ms → 0.66 ms at 32 chunks,
4× at 128.** Past ~128 chunks per-source overhead starts to eat the gain.

### 1.8 Per-prim, per-frame CPU overhead (all measured here)

| Cost | Where | µs/prim |
|---|---|---|
| `ComputeDirtyLocators(1 leaf)` → 4 locators | `hd/containerDataSourceEditor.cpp:158-192` | **0.105** |
| Build a `DirtiedPrimEntry` reusing a cached locator set | — | **0.008** (0.26 at N=100k) |
| `HdRetainedSceneIndex::DirtyPrims` → 1 observer | `hd/retainedSceneIndex.cpp` | 0.02 → **0.53** (N=32 → 100k) |
| `HdDependencyForwardingSceneIndex` fan-out, N curve prims → 1 mesh | `hd/dependencyForwardingSceneIndex.cpp:308-410` | 0.17 / 0.38 / 0.90 / **1.30** (N=32/1k/10k/100k) |
| Lazy dependency population (`GetPrim` × N, once) | `:_UpdateDependencies` | **~10** |
| `GetPrim` through K pass-through filters | `hd/sceneIndexAdapterSceneDelegate.cpp:182-190` | 0.17-0.20, flat in K (0…10) |
| Primvar-descriptor recompute (5 primvars) | `:1822-1855` | **0.51** |
| `HdDataSourceLocatorSet::Intersects` | — | 0.013 |

Absolute totals for one deform frame:

| prims | dependency fan-out | notice delivery | entry build | **sum** |
|---|---|---|---|---|
| 32 | 0.005 ms | ~0.001 | ~0.000 | **~0.01 ms** |
| 1 000 | 0.38 ms | 0.03 | 0.01 | **~0.4 ms** |
| 10 000 | 9.0 ms | 1.4 | 0.4 | **~11 ms** |
| 100 000 | 130 ms | 53 | 26 | **~210 ms** |

**100 000 individual curve prims is off the table on the scene-index side alone**,
before Storm does any work.

### 1.9 A trap: `ComputeDirtyLocators` clears the primvar-descriptor cache

`HdSceneIndexAdapterSceneDelegate::_PrimsDirtied` clears the cached primvar
descriptors for a prim whenever a dirtied locator starts with `primvars` and does
**not** end in `primvarValue`/`indexedPrimvarValue`/`indices`
(`hd/sceneIndexAdapterSceneDelegate.cpp:510-523`). `ComputeDirtyLocators` emits
exactly such a locator. Probe N, run against the real predicate:

```
bare leaf set                  -> clears cache? (no)
ComputeDirtyLocators(leaf) set -> clears cache? primvars/__containerDataSource
```

usdRig's `RigExecResultsSceneIndex` uses `ComputeDirtyLocators`
(`<usdrig-src>/libs/rigExecImaging/sceneIndices.cpp:2504-2505`), so
it pays 0.51 µs/prim of descriptor recompute every deform frame. That is correct
and necessary *there*, because it overlays an upstream container that downstream
caching scene indices may hold handles to.

For the grooming plugin the situation differs: the adapter re-pulls `GetPrim`
each access via a 1-entry per-thread memo (`:182-190`), so it never holds a stale
container. **If the grooming scene index is the sole producer of its curve prims
and no caching filter sits between it and the render index, it may emit the bare
leaf `primvars/points/primvarValue` and keep the descriptor cache warm** (saves
0.51 ms/frame at 1 000 prims). Emit the `ComputeDirtyLocators` form only where
the plugin *overlays* an upstream prim (e.g. writing back onto a user's authored
curves).

### 1.10 Tessellation: refineLevel 2 vs 3

`_UpdateDrawItemGeometricShader` maps refineLevel → shader key
(`basisCurves.cpp:319-343`): `>2` → HALFTUBE/ROUND, `>1` → RIBBON/ROUND, else
RIBBON/HAIR. Constants are hard-coded in `hdSt/shaders/basisCurves.glslfx:278-297`:
`GetMaxTess()=40`, `GetPixelToTessRatio()=20.0`,
`TessLengthFromScreenSize = clamp(len/20, 0, 40)`,
`TessWidthFromScreenSize = clamp(w/20, **1**, 40)`.

* **refineLevel 2** (`Curves.CommonControl.Cubic.Ribbon`, `:524-542`):
  `SetTessFactors(level, 1, level, 1, 1, level)` — width direction is **always 1**.
* **refineLevel 3** (`…Cubic.HalfTube`, `:546-603`): the width factor is
  `TessWidthFromScreenSize(screenWidth * 2.0)`, clamped to a **minimum of 1**.
  It only exceeds 1 when `screenWidth*2 > 20 px`, i.e. **on-screen strand width
  > 10 px**.

**Source-derived conclusion: for hair thinner than ~10 screen px, refineLevel 3
generates the same number of tessellated vertices as refineLevel 2.** The delta
is the halftube's different vertex/normal math and its cross-section, not tess
density. This makes refineLevel 3 a cheap default for hair — but it must be
measured (the shader is longer and the halftube vertex layout differs).

### 1.11 Culling, batching, draw calls

* **GPU frustum culling uses the prim's `extent`.** `HdStPopulateConstantPrimvars`
  writes `bboxLocalMin/bboxLocalMax` from `prim->GetExtent(delegate)`
  (`primUtils.cpp:887-911`) and `frustumCull.glslfx:162-163, 275-276, 365-366, 421-422`
  reads exactly those. The comment at `primUtils.cpp:888-890` is explicit: with no
  authored extent the range is `[FLT_MAX, -FLT_MAX]`, **which disables frustum
  culling for the prim.** Culling granularity therefore *equals* prim granularity,
  and **the plugin must author `extent` per chunk.**
* Optional tiny-prim culling compares the bbox NDC diagonal against `drawRange`
  (`frustumCull.glslfx:53-70`), enabled by `HDST_ENABLE_TINY_PRIM_CULLING` /
  `enableTinyPrimCulling` (`hdSt/renderPass.cpp:341-347`).
* **Batch key** = `TfHash::Combine(geometricShaderHash, bufferArraysHash)`
  (`hdSt/commandBuffer.cpp:164-168`), plus the texture-source hash when texture
  rebinding is disallowed (`:170-177`). Aggregation additionally requires equal
  material-network-shader hash, equal instance levels, and *pointer-equal
  aggregated BARs* for topology / topologyVisibility / vertex / varying / element /
  faceVarying / constant / instanceIndex ranges (`hdSt/drawBatch.cpp:218-262`).
* Therefore **per-prim `refineLevel` or per-prim material fragments the batch.**
  A groom that mixes refineLevel 1 guides with refineLevel 2 hair gets ≥2 batches;
  N distinct materials get ≥N batches.
* On HgiGL, `HdSt_PipelineDrawBatch::IsEnabled` is false by default
  (`hdSt/pipelineDrawBatch.cpp:154-158` → `hdSt/codeGen.cpp:166, 189-197`,
  `HDST_ENABLE_HGI_RESOURCE_GENERATION=false`), so Storm uses
  `HdSt_IndirectDrawBatch`, which issues **one** `DrawIndexedIndirect` for the
  whole batch and increments `drawCalls` exactly once
  (`hdSt/indirectDrawBatch.cpp:1199-1213, 1213-1258`). **Prim count costs GPU
  draw calls only when it fragments batches.**
* Deep validation on any batch-dirty event walks every draw item and calls
  `_IsAggregated` (`indirectDrawBatch.cpp:801-834`) — O(total draw items), CPU.

### 1.12 Instancing curves vs individual prims

`HdStBasisCurves` fully supports instancing: `_UpdateInstancer` +
`HdStUpdateInstancerData` run every sync (`basisCurves.cpp:183-199`), the drawing
coord reserves instance-primvar slots (`:477-480`), and the shader key includes
`Instancing.Transform` (`hdSt/basisCurvesShaderKey.cpp:202`). Instancing is the
right tool for *repeated identical* strands (e.g. a fur "card" library, or a
groom instanced onto many characters). It is the **wrong** tool for procedurally
generated hair, because every strand differs — instanced prototypes share one
points BAR by construction.

### 1.13 How many rprims Storm handles

Structural answer: the render index syncs only **dirty** rprims
(`hd/dirtyList.cpp:82-113`), pre-sync and sync are parallel over prims
(`hd/renderIndex.cpp:1449-1460, 1796-1861`, `parallelRprimSync` defaults true at
`hd/sceneDelegate.cpp:46-53`), and a batch of N prims draws in one indirect call.
The costs that *do* scale with total prim count, dirty or not, are: dirty-list
gather when the rprim index version changes (`dirtyList.cpp:226-275`), batch deep
validation (`commandBuffer.cpp:415-431`), and — for a groom — dependency
forwarding (§1.8). Combined with the measured per-prim costs, **O(10²–10³) prims
is comfortable, O(10⁴) is marginal, O(10⁵) is not.**

---

## 2. Recommendation (source-derived, GPU-UNMEASURED)

### Prim granularity — **32 to 256 `basisCurves` prims per groom description**

| driver | says |
|---|---|
| Upload granularity = prim (`vboMemoryManager.cpp:624-682`) | more chunks ⇒ finer re-upload |
| Frustum culling granularity = prim extent (`primUtils.cpp:887-911`, `frustumCull.glslfx:162`) | more chunks ⇒ real culling; 1 prim ⇒ none |
| Buffer-source resolve parallelism (`resourceRegistry.cpp:868-871`) | ≥ #cores chunks; 32→4×, 128→4.6× measured |
| Index build 2.6 ms → 0.66 ms at 32 chunks | ≥32 |
| Staging bypass at 512 KiB (`stagingBuffer.cpp:73`) | ≥5 461 curves/chunk at 8 CV, if you want the direct path |
| Dependency fan-out 0.38 ms @1k, 9 ms @10k | ≤ ~2 000 |
| Batch deep validation O(draw items) | ≤ ~2 000 |
| One indirect draw call per batch (`indirectDrawBatch.cpp:1208`) | prim count is GPU-cheap while batches hold |

For 100 000 strands: **32 chunks × 3 125 curves** is the balanced default
(300 KB points per chunk, staging path); **128 chunks × 780 curves** if
viewport culling of an off-screen groom matters more than upload path. Chunk
by *spatial locality on the surface* (a scalp-UV or face-cluster partition), not
by index order, so frustum and tiny-prim culling actually reject whole chunks.
Keep guides, hair, and each distinct material in **separate chunk sets** —
they cannot batch together anyway (`drawBatch.cpp:218-262`).

### Topology strategy — **exact-size arrays, stable prim set, deferred count changes**

1. **No padding.** Measured: a longer `points` array without `curveIndices`
   renders fallback red (`basisCurvesComputations.h:223`); with `curveIndices` it
   costs 15× more per update. Author `points.size() == Σ curveVertexCounts`
   exactly, and assert it.
2. **Keep the prim set stable across interactive edits.** Adding/removing prims
   bumps the rprim index version and forces a full dirty-list gather over every
   rprim (`dirtyList.cpp:226-275`). Allocate the chunk prims once at the groom's
   maximum chunk count; vary *contents*, not *existence*.
3. **During a density drag, emit `DirtyPoints` only; commit the count change on
   drag-release.** Any element-count change unconditionally reallocates the whole
   aggregated VBO and bumps its version (`vboMemoryManager.cpp:605-620, 272-427`),
   which invalidates every batch drawing the groom. Interactive density should
   therefore either (a) drive a *pre-generated* max-density point set and move the
   culled strands' CVs to a degenerate point with `widths` 0 (keeps counts fixed —
   costs one near-zero-area patch per hidden strand), or (b) quantise to a few
   pre-baked density levels swapped at drag boundaries. Route (a) is the one to
   measure first.
4. **`cubic` + `bspline` + `nonperiodic`, 8 CV.** Cubic gives 5 patches/curve
   (`int32[4]`, 7.63 MB per 100k curves) vs linear's 7 lines/curve (`int32[2]`,
   5.34 MB) — cubic is *smaller per rendered length* and gets GPU tessellation.
   Always author `type`, `basis` and `wrap` (defaults are `linear`/`bezier`/
   `nonperiodic`, `hd/sceneIndexAdapterSceneDelegate.cpp:893-903`).
5. **Interpolation choice:** `points` = vertex (exact size, zero-copy);
   `widths` = **constant** if uniform width, else **vertex** (varying costs a
   CPU expansion — measured **6.48 ms** per 100k curves,
   `basisCurvesComputations.h:238-247` → `HdSt_ExpandVarying`, `:69-158`);
   per-strand colour/ids = **uniform** (length must equal curve count exactly,
   `basisCurves.cpp:1204-1213`).
6. **Author `extent` on every chunk**, every frame it deforms — without it the
   chunk is never frustum-culled.
7. **Narrow invalidation:** emit `primvars/points/primvarValue` alone where the
   plugin owns the prim (keeps the descriptor cache warm), and
   `ComputeDirtyLocators` only where it overlays an upstream prim.

---

## 3. Benchmark protocol (run on a workstation with a display)

All files in `…/scratchpad/probes/storm-throughput/`. `gen_hair_stages.py` and
`bench_usdview.py` were run/compiled here; `hairbench.cpp` compiles here and
core-dumps for want of a display, as expected.

### 3.1 Stages — `gen_hair_stages.py` (runs headless; verified)

```
PYTHONPATH=$USD/lib/python3.12/site-packages python3 gen_hair_stages.py \
    --out ./stages --curves 100000 --cv 8 --frames 24
```
Produces `hair_1prim.usdc` (1 prim), `hair_32chunks.usdc` (32), `hair_1000prims.usdc`
(1000), `hair_1prim_linear.usdc`, `hair_32chunks_anim.usdc` (points time-sampled
over 24 frames) and `hair_32chunks_density{100,050,025}.usdc`. Identical curve
data in every variant so prim granularity is the only variable; every prim
authors `extent`, constant `widths`, uniform `displayColor`. Verified at
`--curves 20000`: `chunk_0000` = 625 curves, `sum(counts)=5000=len(points)`,
`type=cubic basis=bspline wrap=nonperiodic`.

### 3.2 Wall clock — `bench_usdview.py` (a `testusdview --testScript`)

```
export HD_ENABLE_PERFLOG=1                # counters are OFF otherwise
$USD/bin/testusdview --renderer GL --viewportSize 1920 1080 \
    --testScript bench_usdview.py stages/hair_32chunks.usdc
```
It drives `StageView.updateGL()` + `glFinish()` and records median/p90/min over
60 frames for: static draw at complexity Low/Medium/High/VeryHigh (refineLevel
0/1/2/3, `usdAppUtils/complexityArgs.py:40-43`, `usdImagingGL/engine.cpp:2318-2351`),
time-sample deform, live `points` edit on 1 prim, live `points` edit on **all**
prims, `displayColor`-only edit, and a `curveVertexCounts`+`points` density scrub.
Writes one JSON per stage.

### 3.3 Counters — `hairbench.cpp` (**built OK here**)

There is no `pxr.Hd` Python module in 26.08, so `drawCalls`, `drawBatches`,
`rebuildBatches`, `vboRelocated`, `bufferArrayRangeMigrated`, `sourcesCommitted`,
`copyBufferCpuToGpu`, `gpuMemoryUsed` (`hd/tokens.h:160-203`, `hdSt/tokens.h:115-121`)
are unreachable from the usdview driver. `hairbench` creates a `GlfTestGLContext`,
enables `HdPerfLog`, runs the same four phases, and dumps counters between them.

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=$USD && cmake --build build
HD_ENABLE_PERFLOG=1 ./build/hairbench stages/hair_32chunks.usdc 2
```

### 3.4 Traces and debug flags

| what | how |
|---|---|
| Commit phase split (Resolve / Resize / Reallocate / Copy / Flush) + SyncAll phases | `testusdview --traceToFile trace.json --traceFormat chrome` (scopes at `resourceRegistry.cpp:862, 913, 961, 984, 1018`; `renderIndex.cpp:1796, 1819, 1843`) |
| Per-prim shader keys & BAR transitions | `TF_DEBUG=HD_RPRIM_UPDATED` (`basisCurves.cpp:352-359, 394-415`; `primUtils.cpp:590-660`) |
| Batch count, deep-validation triggers, rebuild reasons | `TF_DEBUG=HDST_DRAW_BATCH` (`commandBuffer.cpp:402-408, 462`; `indirectDrawBatch.cpp:780-833`) |
| Dirty-list rebuild vs reuse | `TF_DEBUG=HD_DIRTY_LIST` (`dirtyList.cpp:102, 235, 247, 267`) |
| Every counter change | `HD_ENABLE_PERFLOG=1 TF_DEBUG=HD_COUNTER_CHANGED` (`perfLog.cpp:136-146`) — run at reduced N |
| Culling A/B | `HD_ENABLE_GPU_FRUSTUM_CULLING=0/1`, `HD_ENABLE_GPU_TINY_PRIM_CULLING` |
| Batch fragmentation A/B | `HDST_DRAW_BATCH_TEXTURE_AGGREGATION_THRESHOLD` (`commandBuffer.cpp:62`) |

`run_bench.sh` sequences all six passes.

### 3.5 Numbers to capture and decision thresholds

| # | Measurement | Capture | Decision threshold |
|---|---|---|---|
| B1 | Static median frame ms, 1 vs 32 vs 1000 prims, refineLevel 2 | `bench_usdview` + `drawCalls`/`drawBatches` | If 1000 prims ≤ 1.15× the 1-prim time **and** `drawBatches`==1, prim count is free ⇒ prefer more chunks |
| B2 | Same with camera framing only ¼ of the groom | `itemsDrawn` | If `itemsDrawn` drops ~4× at 32/1000 prims and not at all at 1 prim, extent-culling is worth the chunking (expected from `frustumCull.glslfx:162`) |
| B3 | refineLevel 2 vs 3 at strand width < 10 px | median ms | If Δ ≤ 20 %, ship refineLevel 3 (halftube) as the hair default |
| B4 | Deform frame ms (time samples), 32 vs 1000 prims | median ms, `copyBufferCpuToGpu`, `sourcesCommitted` | Budget: **< 8 ms** at 1920×1080 for 100k×8 CV. `vboRelocated` must be **0** — non-zero means an element count moved |
| B5 | Live edit on 1 prim of N | median ms vs N | Confirms upload granularity = prim: the 1-prim stage should cost ~32× the 32-chunk stage |
| B6 | Live edit on **all** prims | median ms | Sets the ceiling for "every operator re-emits everything"; if > 16 ms, the graph must emit only changed chunks |
| B7 | Density scrub (topology dirty on 1 prim of 32) | median ms, `vboRelocated`, `rebuildBatches`, `bufferArrayRangeMigrated` | If `vboRelocated ≥ 1` and `rebuildBatches ≥ 1` per scrub frame **and** the frame exceeds ~2× B5, adopt the degenerate-CV constant-topology strategy (§2.4/3) |
| B8 | Padding A/B: correct-size vs `curveIndices`-padded | median ms + visual | Expected: padded is slower and offers nothing; confirms §1.4 |
| B9 | displayColor-only edit | median ms | Should be ≪ B5. If not, the uniform primvar is not in its own BAR |
| B10 | Batch fragmentation: uniform vs per-chunk refineLevel/material | `drawBatches`, `drawCalls`, median ms | Each extra batch should cost ≪ 1 ms; if not, force a single refineLevel and a single material per groom |
| B11 | Prim-count sweep 32 / 128 / 512 / 2048 / 8192 at fixed total curves | median ms static + deform | Take the knee; the CPU model (§1.8) predicts it between 2 000 and 10 000 |
| B12 | GPU memory | `GetRenderStats()['gpuMemoryUsed']`, `nonUniformSize` | Sanity: points 9.6 MB + indices 7.6 MB + primIndices 2 MB ≈ 20 MB per 100k×8 CV before widths/colour |

Report every number as median of 60 frames after 10 warm-up frames, with p90,
and always alongside `drawCalls`/`drawBatches` — a frame-time change with an
unchanged batch count is a GPU-side effect; with a changed batch count it is a
CPU-side batching effect.

---

## Key facts

- **The points upload unit is the whole prim**: `CopyData` blits the entire
  buffer source into the range's sub-offset; there is no per-CV delta path —
  `hdSt/vboMemoryManager.cpp:624-682` (`blitOp.byteSize = srcSize` `:678`,
  `vboOffset = bytesPerElement * _elementOffset` `:669`).
- **No CPU-side copy for exact-size points**: `primvars = _authoredPrimvar`
  shares the array (`hdSt/basisCurvesComputations.h:202-203`); measured
  `sharesInputBuffer=1`, 0.22 ms for 800 000 points. The only cost is the
  thrown-away `VtArray<T> primvars(numVertsExpected)` at `:193` (0.10 / 0.48 /
  2.39 ms at 50k / 200k / 800k points, cold).
- **A `points` array longer than Σ`curveVertexCounts` is NOT tolerated without
  `curveIndices`** — it is replaced wholesale by the fallback (1,0,0)
  (`basisCurvesComputations.h:212-234`; measured, with `TF_WARN`). With
  `curveIndices` it works but costs 3.46 ms vs 0.22 ms (the `resize()` at `:220`
  detaches). The comment at `hdSt/basisCurves.cpp:651-654` refers only to the
  topological-visibility BAR. **Padding is not viable.**
- **Any element-count change reallocates the entire aggregated buffer array**,
  grow or shrink, and bumps its version — `vboMemoryManager.cpp:605-620` (the
  `#if 0`/`#else` at `:588-620` makes this unconditional) and `:272-427`
  (`IncrementVersion()` at `:427`).
- **BAR is reused (no migration) when the specs are a subset and the range is
  mutable** — `hdSt/resourceRegistry.cpp:1250-1268`.
- **Staging threshold is 512 KiB** (`hdSt/stagingBuffer.cpp:73-76`) = 43 691
  `GfVec3f` = 5 461 curves at 8 CV.
- **`DirtyPrimvar` is one bit for all primvars but points/normals/widths**
  (`hd/changeTracker.cpp:958-979`), so a `displayColor` edit skips the points
  fastpath, re-pulls all primvar descriptors, and re-uploads every non-points
  primvar — but not points (`hdSt/basisCurves.cpp:875-928, 932-935`).
- **Topology change costs**: `ComputeHash` 0.017 ms; cubic index build 2.6 ms
  warm / 5.2 ms cold for 500 000 patches (7.63 MB); then whole-VBO relocation
  plus O(all draw items) batch deep validation
  (`hdSt/commandBuffer.cpp:398-455`, `hdSt/indirectDrawBatch.cpp:801-834`).
- **Chunking parallelises the CPU work**: `_Commit` resolves sources with
  `WorkParallelForTBBRange` (`hdSt/resourceRegistry.cpp:868-871`); measured index
  build 2.607 ms (1 prim) → 0.659 ms (32) → 0.572 ms (128) on 20 cores.
- **`ComputeDirtyLocators` costs 0.105 µs/prim** (`hd/containerDataSourceEditor.cpp:158-192`)
  and emits 4 locators for one leaf — negligible, but it **clears the adapter's
  primvar-descriptor cache** (`hd/sceneIndexAdapterSceneDelegate.cpp:510-523`),
  costing 0.51 µs/prim of recompute per frame; the bare leaf locator does not.
- **`HdDependencyForwardingSceneIndex` fan-out is 0.17 / 0.38 / 0.90 / 1.30
  µs per affected prim at N = 32 / 1 000 / 10 000 / 100 000** — 130 ms for one
  mesh-points dirty at 100k dependent prims (`hd/dependencyForwardingSceneIndex.cpp:308-410`).
  Lazy dependency population is ~10 µs/prim, one-off.
- **Frustum culling uses the prim's authored `extent`**; with no extent the
  bbox is `[FLT_MAX,-FLT_MAX]` and the prim is never culled — `hdSt/primUtils.cpp:887-891`,
  `hdSt/shaders/frustumCull.glslfx:162-163`.
- **refineLevel 3's width tessellation is clamped to 1 below ~10 px on-screen
  strand width**, so for hair it generates the same vertex count as refineLevel 2
  — `hdSt/shaders/basisCurves.glslfx:278-300, 523-542, 546-603`.
- **Batch key = geometric-shader hash + buffer-arrays hash** (+ texture hash)
  (`hdSt/commandBuffer.cpp:164-177`), aggregation additionally needs equal
  material hash and pointer-equal BARs (`hdSt/drawBatch.cpp:218-262`); per-prim
  refineLevel or material therefore fragments batches.
- **One batch = one indirect draw call** on HgiGL (`HdSt_IndirectDrawBatch`,
  since `HDST_ENABLE_HGI_RESOURCE_GENERATION` defaults false —
  `hdSt/codeGen.cpp:166, 189-197`); `drawCalls` incremented once per batch at
  `hdSt/indirectDrawBatch.cpp:1208`.
- **`HD_ENABLE_PERFLOG=1` is required** for any counter to be non-zero
  (`hd/perfLog.cpp:22-33`), and there is no `pxr.Hd` Python module — counters
  need the C++ harness.
- **Rprim sync is parallel over prims** and only dirty prims are synced
  (`hd/renderIndex.cpp:1449-1460, 1828-1861`; `hd/dirtyList.cpp:82-113`), but a
  change in the rprim index version forces an O(all rprims) dirty-list gather
  (`hd/dirtyList.cpp:226-275`) — so adding/removing curve prims is expensive.
- **Varying primvars on cubic curves are CPU-expanded**: 6.48 ms for 600 000 →
  800 000 values on 100k curves (`basisCurvesComputations.h:69-158, 238-247`).
- Instancing works for curves (`hdSt/basisCurves.cpp:183-199`,
  `hdSt/basisCurvesShaderKey.cpp:202`) but is useless for procedural hair, where
  every strand differs.

## Decisions this settles

1. **Prim granularity: 32–256 `basisCurves` prims per groom description**
   (≈3 000 / ≈800 curves each for a 100k-strand groom), partitioned by surface
   locality. Not 1 (no culling, no resolve parallelism, 32× re-upload
   amplification) and not 10⁴–10⁵ (dependency fan-out and deep validation).
2. **No padding.** Exact `points.size() == Σ curveVertexCounts`, asserted by the
   publisher. The fixed-capacity/shrinking-counts trick is broken in 26.08.
3. **The prim set is allocated once and never changes during interaction.**
   Density and generator edits vary contents, not prim existence.
4. **Interactive density scrubbing must not change element counts.** Prefer
   degenerate CVs + zero width at max-density topology during the drag, and
   commit the real count change on release. B7 decides whether this is needed.
5. **`cubic`/`bspline`/`nonperiodic`; `widths` constant or vertex (never
   varying); per-strand attributes `uniform`; `extent` authored per chunk every
   deforming frame.**
6. **Invalidation discipline:** bare `primvars/points/primvarValue` where the
   plugin owns the prim; `HdContainerDataSourceEditor::ComputeDirtyLocators` only
   where it overlays an upstream prim (usdRig's pattern). Never co-dirty
   `displayColor` with `points`.
7. **One refineLevel and one material per chunk set**, since either fragments the
   draw batch; guides get their own chunk set anyway.
8. **The dependency on the deforming surface is declared per chunk, not per
   curve** — 32–256 dependency edges instead of 100 000.
9. Curve **instancing is reserved for repeated grooms**, not for strands.
10. All GPU claims stay UNMEASURED until §3 is run; §3.5 lists the thresholds
    that would overturn any of the above.

## Open questions

- All GPU-side numbers: static/deform/edit/scrub frame times, the real cost of a
  whole-VBO relocation on a density scrub, and whether 1 000 prims really draws
  as fast as 1 prim once batched (B1, B4, B7, B11). **UNMEASURED — no display.**
- Whether refineLevel 3's halftube costs measurably more than refineLevel 2 for
  sub-10-px hair, given the tess factors are identical (B3).
- Whether the degenerate-CV trick (§2.4/3) is actually cheaper than eating the
  VBO relocation — depends on how much a near-zero-area patch costs in the
  rasteriser, which needs B7 plus a variant with N hidden strands.
- Whether `HgiDeviceCapabilitiesBitsUnifiedMemory` is set on the GB10 under
  HgiGL; if it is, `StageCopy` never takes the direct path
  (`hdSt/stagingBuffer.cpp:74`) and the 512 KiB chunk threshold is moot.
- Whether a Hydra-2 scene index can populate `invisibleCurves` (A5's open
  question) — if it can, per-curve topological visibility becomes a
  constant-topology density mechanism strictly better than degenerate CVs.
- The cost of `HdDirtyBitsTranslator::RprimLocatorSetToDirtyBits` +
  `_MarkRprimDirty` per prim per frame in the adapter (`hd/sceneIndexAdapterSceneDelegate.cpp:440-540`)
  — not isolated here; folded into the "notice delivery" row of §1.8.
- Whether the plugin's own scene index can be placed after
  `HdSt_DependencyForwardingSceneIndexPlugin` (insertion phase 1000,
  `hdSt/dependencyForwardingSceneIndexPlugin.cpp:28-35`) or must live before it —
  affects whether it can use the dependency mechanism at all.
