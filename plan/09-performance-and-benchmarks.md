# Performance model and benchmarks

Date: 2026-09-04. Status: plan v1 (draft, pending review).

usdGen's numeric contract: what "very fast and interactive" (R8) means in milliseconds per frame and
per edit at 4 k / 40 k / 100 k / 200 k / 1 M curves, the numbers measured on this host, the cost and
memory model the engine and imaging designs are sized against, the four benchmark grooms and their
harness, and — in §5 — the **single gate registry** (ADR R40). Every number carries exactly one of ADR
R42's four tags: **MEASURED** (with its `EV-nnn` row), **DERIVED from `EV-nnn`** (scaled, interpolated
or extrapolated; never MEASURED), **UNMEASURED** (with its gate) or **ASSUMPTION**; §0.3 and §0.4
abbreviate them **M / D / U / A**. `P0`/`P1`/`P2` here are always motion profiles; the engine
invariants are `I1`–`I8` (ADR R1).

Reads with: `01-architecture.md` §0.3 (which cites this ledger, ADR R41), `03-execution-engine.md`
(§1.4 tiles, §3.2–§3.3 the digests, §6.4 eviction, §9.2 diagnostics and `UsdGenStats`),
`06-imaging.md`, `05-static-curves-and-deformation.md`, `08-tools.md`,
`10-build-dependencies-testing.md` (§1.4 the link-rule variables, §3.5 the env-var registry, §5.1
tiers, §5.2 the harnesses and the workstation-protocol numbering, §5.6 the CTest name registry, §6.5
the gate→test map that cites §5 below), `11-roadmap.md` (§1 PW-1…PW-6, §2.0 milestones, §5.1–§5.2
SC-1…SC-13, §6.4 RC-1…RC-11), `appendix-A-evidence-ledger.md` (§2, the `EV-001…EV-092` ledger),
`appendix-B-prototype-inventory.md`.

---

## 0. Targets

### 0.1 What "very fast and interactive" means numerically

| Class | Gesture | Budget | usdGen share | Gate |
|---|---|---|---:|---|
| **A** playback / tumble / deform frame | scrub or camera move on a deforming groom | 60 Hz at ≤ 40 k curves (MEASURED draw 5.01 ms, EV-020); ≥ 30 Hz at 100 k; ≥ 10 Hz at 200 k — all **ASSUMPTION** until S-1 (ADR R41); `usdGen:densityScale` is the artist's lever | ≤ 1.0 ms | S-1, S-2 |
| **B** operator-parameter edit | drag `usdGen:blend`, an amount, a ramp knot | one redraw, ≤ 33 ms | ≤ 0.6 ms engine + one tile publish | E-3, S-3 |
| **C** brush move | one `mouseMoveEvent` of a stroke | 16.6 ms, so a stroke holds 60 Hz | ≤ 1.0 ms (Python + engine) | T-1 |
| **D** structural edit | add/remove/reorder an operator, commit a density change, freeze | ≤ 100 ms | ≤ 0.2 ms recompile + one topology publish | E-6, T-4 |
| **E** first cook | activation, stage load, seed change, surface resync | ≤ 1 s at 100 k | capture-dominated | E-4 |

Class A is the hard one and the only one where Storm, not usdGen, is the constraint (§0.2). B–E are
ASSUMPTION budgets chosen so the gesture reads as instant; the gate column holds each. Class D's
100 ms is set by usdview at the **2 205-prim reference stage** (`_resetGUI` 35.4 ms, MEASURED,
EV-051); on an 11 005-prim stage one resync alone is 105.5 ms, over budget before usdGen does
anything — hence one `Sdf.ChangeBlock` per structural edit, and T-4 asserting *exactly one*
`_resetGUI` rather than a wall-clock ceiling.

### 0.2 The frame ledger (100 000 curves × 8 CV, deforming scalp, 1280×720, refineLevel 2, 49 tiles)

The only frame ledger in the plan (ADR R41); siblings cite it. It corrects
`design/proposal-performance.md` §0.2: the deformed tail is DERIVED (it was a budget), the viewport is
1280×720 because that is what was measured, and the total is the sum of the rows.

| Stage | Cost | Tag and source |
|---|---:|---|
| usdRig rig evaluation (`ArmShotAnim.usda`, 95 prims) | 1.35 ms | MEASURED, EV-035 |
| usdGen dirty routing + commit bookkeeping (~200 notice entries) | 0.04 ms | DERIVED from EV-037 (0.2 µs/entry × ~200); the entry count is itself UNMEASURED, gate SI-3 |
| usdGen deformed tail (`UsdGenDeform` + deformed-space stylers), 800 k CVs | 0.4–0.6 ms | DERIVED from EV-008, EV-014/EV-015/EV-016 (styler pass 0.158–0.545 ms single-thread; 5-node chain 1.02 ms at 8 threads). Gate E-1 |
| SoA → AoS interleave of the dirty tiles (worst case: all) | 0.25 ms | DERIVED **floor** from EV-074 (0.49 ms per 19.2 MB single-thread `-O2` copy); a gather is a strided three-stream write, not a memcpy — UNMEASURED, gate S-2 |
| per-tile `extent` over the same pass (49 tiles) | 0.05 ms | ASSUMPTION (min/max fused into the interleave loop); gate S-2 |
| generation diff + publish + notice emission (49 tiles) | 0.02 ms | DERIVED from **EV-085** (0.008 µs/prim entry build) and **EV-084** (0.02–0.53 µs/prim notice delivery) |
| **usdGen total, deform frame** | **0.8–1.0 ms** | DERIVED aggregate of the five rows above |
| Storm draw, refineLevel 2, 720p | ~12.2 ms | **DERIVED from EV-020/EV-021** (linear interpolation between 5.01 ms @40 k and 23.93 ms @200 k); UNMEASURED at 100 k, gate **S-1** |
| Storm points upload (9.6 MB, scene-index publish) | ≈ 1.44 ms | **DERIVED from EV-022**: measured deltas interpolated linearly in curve count (0.83 ms @40 k, 2.46 ms @200 k → 1.44 ms @100 k over 9.6 MB). The deltas include a `UsdAttribute::Set`, so a scene-index publish should land at or below this. Gate **S-2** |
| **Frame total** | **15.8–16.0 ms** | DERIVED: 1.35 + (0.8–1.0) + 12.2 + 1.44 |

**60 Hz at 100 k × 8 CV at refineLevel 2 is not established by this ledger** (ADR R41): its largest
term is derived, not measured. S-1 establishes or refutes it; until then §0.1 class A's three tiers
are the targets and they are ASSUMPTION.

> **Precondition.** This ledger assumes `hairTangent` **variant A** (ADR §5.4): no `hairTangent`
> primvar is published, so a deforming frame keeps Storm's points fastpath. If gate **S-8** selects
> variant B, add **+1.5–2.5 ms** per deforming frame at 100 k (**DERIVED from EV-022** plus the
> full-primvar re-upload path; UNMEASURED, gate S-8) — §5.3, `11-roadmap.md` §5.2 (SC-2).

### 0.3 Per frame, at every scale

`frame total = rig (1.35) + usdGen deform-frame total + Storm draw + points upload`, applied
identically to every row; the deform-frame column is exactly §0.2's aggregate.

| Groom | curves | CVs | points | Storm draw, refine 2, 720p | upload delta | usdGen full chain | deform-frame total | Frame total | Verdict |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| G1 `lash4k` | 4 000 | 32 000 | 0.38 MB | **0.85** M | **+0.22** M | ~0.04 D @8 thr | ~0.1 D | **2.5** D | 60 Hz, 14 ms spare |
| G2 `brow40k` | 40 000 | 320 000 | 3.8 MB | **5.01** M | **+0.83** M | ~0.41 D @8 thr | ~0.4 D | **7.6** D | 60 Hz — the measured anchor of class A's ≤ 40 k tier |
| G3 `head100k` | 100 000 | 800 000 | 9.6 MB | ~12.2 D (S-1) | ≈1.44 D (S-2) | **1.02** M @8 thr (1.72–1.91 M @20 thr) | 0.8–1.0 D | **15.8–16.0** D | **not established** — the draw term is DERIVED (S-1); the target is ≥ 30 Hz at 100 k (ASSUMPTION, ADR R41) |
| G4 `fur200k` | 200 000 | 1 600 000 | 19.2 MB | **23.93** M | **+2.46** M | ~2.0 D @8 thr | ~1.6 D | **29.3** D | **~34 Hz D** — clears both the ≥ 10 Hz and the ≥ 30 Hz tiers on this arithmetic, but only the 23.93 ms draw term is MEASURED (EV-021); every other term is DERIVED, and ~1 ms of drift in any of them puts the row under 30 Hz. S-1 and S-2 settle it; S-9's tumble tier and `usdGen:densityScale` are the levers if they do not |
| `head1M` | 1 000 000 | 8 000 000 | 96 MB | ~118 D (S-11) | ~12.5 D | 8 thr **U**, gate E-1 (7.59–7.67 M @20 thr) | ~6.5 D | **~138** D | **not interactive undecimated**; `densityScale = 0.1` returns it to the G3 row |

`head1M`'s draw term is **DERIVED (extrapolated 5× past the 200 k endpoint) from EV-019/EV-020/EV-021 and
UNMEASURED**; gate **S-11** records it. Thread counts are annotated per cell because they differ: this
host's 20-thread result at 100 k is *worse* than its 8-thread result (EV-001/EV-008), so an 8-thread
1 M figure cannot be inferred either way; every engine budget quotes the 8-thread private-arena number
first (ADR R27). §0.2's `hairTangent` precondition applies to G3 and G4.

The load-bearing conclusion: **above ~106 k drawn curves at 720p on this GPU there is no 60 Hz frame
left to spend** (DERIVED, §2.5). Hence `usdGen:densityScale` is a first-class artist control, not a
debug flag (§3), and the LOD ladder decimates curve **count** — refineLevel 0 is *slower* than 1 at
200 k (MEASURED, EV-021).

### 0.4 Per edit, at every scale

usdGen's share only; each edit also costs one redraw, which is §0.3's draw and which the edit did not
change.

| Groom | sparse edit, 1 % of chunks, all 5 nodes | terminal-parameter edit | brush move, 1 chunk |
|---|---:|---:|---:|
| G1 `lash4k` | ~0.002 D | ~0.008–0.02 D | ~0.1 D |
| G2 `brow40k` | ~0.018 D | ~0.07–0.16 D | ~0.1 D |
| G3 `head100k` | **0.035–0.044** M (EV-002) | **0.18–0.41** M (EV-004) | ~0.1 D (+21 µs Python, M, EV-061) |
| G4 `fur200k` | ~0.08 D | ~0.36–0.82 D | ~0.1 D |
| `head1M` | **0.29** M at 0.5 % (EV-006) | ~1.8–4.1 D | ~0.1 D |

DERIVED cells scale the measured 100 k values linearly in curve count, which §2.4 shows is
conservative above 100 k. The brush move is **size-independent** — it touches one 512-curve chunk and
one tile, which is class C and the whole reason for chunking; the terminal-parameter edit is the
class-B term that runs out first, inside 0.6 ms to ~150 k curves and outside it at 200 k in the worst
measured spread, which makes density an *editing* lever too.

---

## 1. Measured evidence

Measured numbers are cited by their `EV-nnn` handle and by nothing else (ADR R1, R42, R43);
`appendix-A-evidence-ledger.md` §2 is the ledger and carries the probe, raw output and caveats for
each row. Its numbering is **flat — `EV-001`…`EV-092`, independent of subsection** — and its §2.0
publishes the map from the retired `A2.n-Xn` handles earlier drafts of this document used. The
notice-delivery pair below is **`EV-084`** (delivery) and **`EV-085`** (entry build), both in
appendix A §2.4.

Host for every row (`research/ENVIRONMENT.md` with CORRECTIONS): Linux 6.17 aarch64, 20 CPUs
(10× Cortex-X925 + 10× Cortex-A725), NVIDIA GB10, driver 580.173.02, GL 4.6 compatibility profile,
g++ 13.3, OpenUSD v26.08 at `/home/burkard/work/OpenUSD_26_08`. Reports by block:
`research/G-data-plane-engine-prototype-benchmark.md` §3–§6 (EV-001…EV-018);
`research/G-storm-hair-look-prototype.md` §5 (EV-019…EV-023);
`research/G-storm-throughput-and-prim-granularity.md` §1 (EV-024…EV-034 and EV-084…EV-088 from the
same §1.8 table); `research/G-chain-order-probe.md` §5 +
`research/G-evaluation-scheduling-and-batching.md` §5, §9 (EV-035…EV-039);
`research/G-freeze-bake-undo-and-frozen-reentry.md` §1, §3, §4 (EV-040…EV-054);
`research/G-tool-loop-array-transport-and-cv-picking.md` §1–§2 (EV-055…EV-066);
`research/A8-seexpr-ptex-libs.md` §1.6, §2.8 (EV-067…EV-070);
`research/G-motion-blur-sampling-strategy.md` §5 (EV-071…EV-075); `research/B-usdrig-build.md`
§2–§5 (EV-076…EV-080); `research/G-stage-free-parameter-and-time-transport.md` §1–§5
(EV-081…EV-083).

| EV | Measurement | Value |
|---|---|---:|
| **EV-001** | 5 stylers, 100 k × 8 CV, full run, **8 threads** (20 threads in parentheses, ADR R27) | **1.02 ms** (1.72–1.91 ms) |
| **EV-008** | the same at 1 / 2 / 4 / **8** / 20 threads | 3.90 / 3.01 / 1.66 / **1.02** / 1.79 ms |
| **EV-002** | sparse edit: 1 % of chunks dirty on all 5 nodes | **0.035–0.044 ms** |
| **EV-004** | terminal-parameter edit (last node only) | 0.179–0.411 ms |
| **EV-009** | chunk-size sweep | 128→1.55, 512→1.83, 1024→1.89, **4096→9.40**, 16384→2.58 ms |
| **EV-006** | 1 M × 8 CV, full run / 0.5 % sparse, 20 threads | **7.59–7.67 ms** / 0.29 ms |
| **EV-007** | peak RSS at 1 M, per-node point buffers / one shared buffer | **668 MB** / 293 MB |
| **EV-011, EV-013** | VDF `VdfNetwork`, **strip element layout, request-all**, at 100 k (min–median) / 1 M; RSS at 1 M; `VdfScheduler::Schedule` | 3.80–8.32 / 69.9–134.8 ms; 1 263 MB; **1.4 µs/node** |
| **EV-014/EV-015/EV-016** | 800 k-CV pass, 1 thread: AoS / SoA streamed / SoA strip | 0.506 / 0.545 / **0.158 ms** = 25.3 / 23.5 / **80.9 GFLOP/s** |
| **EV-019/EV-020/EV-021** | refineLevel 2 draw at 4 k / 40 k / 200 k curves × 8 CV | 0.85 / **5.01** / **23.93 ms** |
| **EV-021** | the same 200 k scene at refineLevel 0 / 1 / 3 (EV-023: level 3 costs nothing over 2) | 12.43 / **8.17** / 23.69 ms |
| **EV-022** | per-frame `points` re-author + upload delta at 4 k / 40 k / 200 k | +0.22 / **+0.83** / **+2.46 ms** = 0.57 / 0.22 / 0.13 ms per MB (the rate *falls* with size) |
| **EV-026** | padded points without / with `curveIndices` | fallback red, 0.54 ms / correct but **3.46 ms** |
| **EV-027/EV-028** | cubic index build, 500 k patches (7.63 MB); vs prim count | 2.6 ms warm / 5.2 cold; 2.61 (1 prim) → 0.66 (32) → 0.57 ms (128) |
| **EV-030** | per prim per frame: `ComputeDirtyLocators` / primvar-descriptor recompute | 0.105 / **0.51 µs** |
| **EV-031** | dependency-forwarding fan-out at 32 / 1 k / 10 k / 100 k prims | 0.17 / 0.38 / 0.90 / 1.30 µs per prim (→ **130 ms** at 100 k) |
| **EV-032** | scene-index bookkeeping per deform frame at 32 / 1 k / 10 k / 100 k prims | ~0.01 / ~0.4 / ~11 / **~210 ms** |
| **EV-033** | `varying` `widths` CPU expansion, 100 k curves (`HdSt_ExpandVarying`) | 6.48 ms |
| **EV-084** / **EV-085** | `HdRetainedSceneIndex::DirtyPrims` → 1 observer, notice delivery (EV-084) and the `DirtiedPrimEntry` build with a cached locator set (EV-085) (`research/G-storm-throughput-and-prim-granularity.md` §1.8) | **0.02 → 0.53 µs/prim** at N = 32 → 100 k / 0.008 µs/prim (0.145 at 100 k) |
| **EV-035/EV-036** | `RigExecImaging_SetTime`, `ArmShotAnim.usda`, 48 frames | **1.351–1.502 ms/frame**; +0.24 ms for a full terminal traversal |
| **EV-037** | notice cascade, 1 → 5 000 scalps | 2 `PrimsDirtied` calls/frame always; **0.2 µs/entry** |
| **EV-038/EV-039** | cook counts for 10 events; 8 readers × 20 publishes | eager 29 / lazy 10 on 4 worker threads with 0 notices / deferred 10; **16 734 reads, 0 torn** |
| **EV-044** | `RemovePrim` + apply vs `SetActive(false)` + apply at 10 k / 100 k / 1 M | 0.06 / **1.47** / 13.47 ms vs **0.02 ms flat** |
| **EV-050/EV-051** | usdview per resync at 115 / 2 205 / 11 005 prims: author · `_resetGUI` | 1.34 / **3.45** / 12.09 ms · 4.28 / **35.4** / 105.5 ms |
| **EV-055/EV-057/EV-058** | `VtArray` over pxr_boost · `Vt.FromBuffer` / `attr.Set(numpy)` at 1 M · `attr.Set(Vt.Vec3fArray)` | **0.13–0.18 µs** flat · 4 304 / 4 893 µs · **1.63–1.82 µs** flat in N |
| **EV-061/EV-064/EV-065** | brush move Python side · CPU CV pick at 100 k / 1 M · matmul, BLAS unpinned vs pinned | **21.3 µs** · **166 µs / 1.66 ms** · **22 ms vs 0.56 ms** |
| **EV-067/EV-069/EV-070** | SeExpr interpreter / Ptex bilinear lookup, 8 threads | **50 M evals/s** / **228 M lookups/s** aggregate |
| **EV-071…EV-075** | one memory-bound pass over 1.6 M `float3` (19.2 MB), 1 thread, `-O2` (`prototypes/motion-blur/mbbench.cpp`) | 0.49 ms copy (EV-074); 0.97–1.00 ms lerp / extrapolate / difference; 19.2 MB per sample (EV-075) |
| **EV-079** | `RemovePrim` with no exec system vs an `ExecUsdSystem` attached | 0 errors vs **1 error** |
| **EV-081** | a codeless prim with no imaging adapter | hydra `primType` `''`, **every `usdGen:*` attribute absent** |

EV-011/EV-013 are why S21 chose the bespoke DAG — the *rejected* baseline, quoted so nobody re-runs
it; EV-013's 1.4 µs/node is the only compile-time number that exists and it is a different compiler
(gate E-6). EV-011's 100 k figure is the **strip** element layout at `elemBytes = 96`
(`prototypes/data-plane-benchmark/results_main.txt:18`, min 3.797 / median 8.321 / max 13.979 ms);
the `cv` element layout at `elemBytes = 12` is a *different* probe section (`:39`, min 7.656 /
median 10.364 / max 14.478 ms) and the two are never merged into one range.
EV-067/EV-069/EV-070 give the capture budget directly: one SeExpr attribute and one Ptex lookup
per root over 100 k roots costs `100 000/50e6 + 100 000/228e6` ≈ **2.4 ms** on 8 threads (DERIVED),
≈ 24 ms over 1 M; the kNN term of `UsdGenGuideInterpolate`'s capture has no measurement (gate **E-4**).
The **points fastpath** shapes `06-imaging.md` §5.2: taken only when `DirtyPoints` is set and
`DirtyNormals|DirtyWidths|DirtyPrimvar` are all clear (`pxr/imaging/hdSt/basisCurves.cpp:932-935`).
Frustum culling reads the authored `extent` and is disabled outright without one
(`pxr/imaging/hdSt/primUtils.cpp:887-912`).

---

## 2. Cost model

Symbols: `N` curves, `V` CVs/curve, `C = N·V`, `nChunks = ceil(N/chunkSize)` with `chunkSize = 512`
(`USDGEN_CHUNK_SIZE`), `chunksPerTile = max(1, ceil(nChunks/tileTarget))`,
`nTiles = min(nChunks, clamp(ceil(nChunks/chunksPerTile), 32, 256))` from
`uniform int usdGen:tileTarget = 64` (ADR R21; worked table `03-execution-engine.md` §1.4), `W`
worker threads (default 8, `USDGEN_THREAD_LIMIT`).

### 2.1 Per-stage formulas

```
capture(n)   = cap_topo + cap_sample
   cap_topo  : scatter O(N) + Morton O(N log N); kd-tree O(G log G); kNN O(N log G)   [UNMEASURED: E-4]
   cap_sample: N * (expr_ns + ptex_ns) / W                        [MEASURED EV-067/EV-069/EV-070]
evaluate(n)  = dirtyChunks(n)/nChunks * c(n)
   c(n)      = C * bytesTouched(n) / (bandwidth * W)   -- memory-bound, not flop-bound
   c(styler)~= 0.20 ms per 800k CVs at W=8                        [DERIVED from EV-008: 1.02/5]
interleave   = I * dirtyTiles/nTiles * pointsMB,  I = 0.0255 ms/MB
               [DERIVED FLOOR from EV-074 (0.49 ms / 19.2 MB, 1 thread, -O2). An SoA->AoS gather
                is a strided 3-stream write, not a memcpy: UNMEASURED, gate S-2. Single-threaded
                in v1, hence no /W term.]
extent       = fused into interleave, one min/max per tile              [ASSUMPTION; gate S-2]
publish      = nTiles * (pointer compare + 0.008 us) + delivery 0.02-0.53 us/prim
                                              [MEASURED EV-085 entry build, EV-084 delivery]
upload       = U * dirtyTiles/nTiles * pointsMB,  U = 0.15 ms/MB at 9.6 MB
               [DERIVED from EV-022: the measured deltas interpolated linearly in curve count
                (0.83 ms @40k, 2.46 ms @200k -> 1.44 ms @100k over 9.6 MB). Interpolating the
                per-MB *rates* instead gives 0.19 ms/MB; the delta interpolation is what the
                ledger uses. Gate S-2.]
draw(N)      = 0.379 + 0.1178 * (N/1000) ms @ refine 2, 720p    [DERIVED from EV-019/EV-020/EV-021]
R            = 0.04 ms routing at ~200 notice entries           [DERIVED from EV-037]

frame(deform)= R + sum_{tail} evaluate + interleave(all) + publish + upload(all) + draw(N)
edit(param p)= R + sum_{cone(p)} evaluate + interleave(dirty) + publish + upload(dirty) + draw(N)
edit(brush)  = R + evaluate(terminal)*k/nChunks + interleave(1 tile) + publish + upload(1 tile)
capture(ep)  = sum_{cone} capture      -- once per structural edit, never per frame
motion P2    = H_cached + k*T + (k-1)*m,  m ~ 1.0 ms per 1.6M-point pass  [MEASURED EV-071..EV-073]
```

`edit(brush)` with `k = 1` chunk at 100 k is `0.04 + 0.001 + 0.005 + 0.02 + 0.029 ≈` **0.095 ms of
usdGen** (DERIVED) — the stroke costs the stroke, and the frame is then whatever `draw(N)` costs.

### 2.2 Why the chain is memory-bound

The 5-node chain over 800 k CVs costs **1.02 ms at 8 threads** (MEASURED, EV-008) — about 2× one
single-threaded AoS pass (0.506 ms, EV-014) for five nodes on eight cores; at 20 threads it is
*slower* (EV-001). It saturates bandwidth long before flops. Three consequences: planar SoA internally
(25.3 → 80.9 GFLOP/s for identical arithmetic, EV-014/EV-016, **invariant I1**); no hand-written NEON
(GCC 13.3 autovectorises all three prototype kernels with 128-bit vectors and `-mcpu=native` does not
switch to SVE); and `bytesTouched`, not flops, as the term in `c(n)`.

### 2.3 Chunk sizing, and where the schedule breaks

The measured cliff (EV-009) is a scheduling failure, not per-chunk overhead: 4096-curve chunks give
25 chunks over 20 threads and cost **9.40 ms, 5.1× the 512 figure**. The rule is `nChunks >= 8 * W`
(at chunkSize 512 this holds for `N >= 32 768` with `W = 8`). Below that the chain is sub-millisecond
anyway; above it `USDGEN_CHUNK_SIZE` must stay in the measured 128–1024 band.

### 2.4 Scaling to 1 M

Every row quotes the 8-thread private-arena figure first (ADR R27).

| Term | 100 k | 1 M | Basis |
|---|---:|---:|---|
| full chain, 8 threads | **1.02 ms** | **UNMEASURED, gate E-1** | MEASURED EV-008 at 100 k |
| full chain, 20 threads | 1.72–1.91 ms | **7.59–7.67 ms** | MEASURED EV-001, EV-006 — 4.2×, sub-linear |
| sparse (1 % / 0.5 %) edit | 0.035–0.044 ms | **0.29 ms** | MEASURED EV-002, EV-006 |
| interleave, all tiles | 0.25 ms | 2.45 ms | DERIVED floor from EV-074, linear in bytes |
| points upload, all tiles | ≈1.44 ms | ~12.5 ms | DERIVED from EV-022 |
| Storm draw, refine 2, 720p | ~12.2 ms D (S-1) | ~118 ms D (S-11) | DERIVED from EV-019/EV-020/EV-021 — **the wall** |
| RSS, per-element model | 77–86 MB | 770–860 MB | DERIVED, §2.6 |
| RSS, measured anchor + model | — | **950–1050 MB** | DERIVED from EV-007 + §2.6's other planes — gate **E-5** |

The engine scales; the renderer does not. At 1 M the only lever that reaches the frame budget is
`usdGen:densityScale` (§3.1), which is why it is applied *before* evaluation.

### 2.5 The Storm draw fit, and the maximum interactive curve count

```
draw_ms(N) = 0.379 + 0.1178 * (N/1000)
   [DERIVED from EV-019/EV-020/EV-021: least squares over three measured points, max residual 1.6 %]
   check @ 40k: 0.379 + 4.710 = 5.09 ms  vs 5.01 MEASURED  (+1.6 %)
   upload term: 8 CV x 12 B = 96 B/curve = 9.6e-5 MB/curve x 0.13 ms/MB = 1.25e-5 ms/curve
   solve for 16.6 ms with 1.0 ms of usdGen (class A's share) and 1.35 ms of rig:
       N_max = (16.6 - 1.35 - 1.0 - 0.379) / (0.0001178 + 0.0000125)  ~=  106 000 curves
```

The upload slope uses 0.13 ms/MB, the lowest of the three measured rates (EV-022), so 106 k is the
optimistic end; earlier drafts omitted the upload term and got 118 k, which is withdrawn. The class-A
share and the ledger's derived total are the same 1.0 ms, so there is one `N_max`, not two (at a
2.0 ms share the solve gives ~99 k). **UNMEASURED:** the fit at 1920×1080, at 100 k, and past the
200 k endpoint — **S-1** measures 100 k at both resolutions, **S-11** settles the extrapolation.
Nothing may quote `draw_ms` at another resolution until S-1 lands. The fit is also single-prim; tiling
adds per-prim Sync cost that S-1, S-5 and S-12 measure.

### 2.6 Memory model

Per element in the SoA layout: position 12 B per CV **per node buffer**; `hairT` 4 B per CV (one
shared copy); `widths` 4 B per CV per node that writes it (typically 2); per-curve AoS arrays 60 B
(id 8 + rootPrim 4 + rootUV 8 + root frame 36 + mask 4); capture caches 36 B per curve; publish 12 B
per CV interleaved, plus an optional second buffer from the M7 publish ring. Every `VtArray` published
through that ring wraps a `UsdGenGenerationBuffer : Vt_ArrayForeignDataSource`
(`pxr/base/vt/array.h:39-55`) and returns to the session pool only after its detached callback has
fired (ADR R20). **`VtArray::IsUnique()` does not exist** — the only `_IsUnique()` in 26.08 is private
(`pxr/base/vt/array.h:1023`) — and must not appear anywhere.

Two accountings, because they disagree and E-5 decides between them:

* **Per-element model.** At 100 k × 8 CV (9.6 MB of raw positions), 5-node chain: node buffers
  48.0 MB + `hairT` 3.2 + `widths` 6.4 + per-curve 6.0 + capture caches 3.6 + publish 9.6–19.2 =
  **77–86 MB** (DERIVED), ≈ 8–9× raw positions; ×10 at 1 M = **770–860 MB** (DERIVED).
* **Measured anchor plus model.** EV-007 measured **668 MB** at 1 M with per-node buffers, but the
  probe stores only `VtVec3fArray` per node — the position plane alone. Adding the model's other
  planes at 1 M (`hairT` 32 + `widths` 64 + per-curve 60 + capture caches 36 + publish 96–192 MB)
  gives **950–1050 MB** (DERIVED).

E-5's 800 MB threshold sits below the second accounting, so **E-5 is UNMEASURED for the shipped chain
and at risk**; it decides whether per-node caching becomes opt-*in* above 500 k curves
(`11-roadmap.md` §5.2, SC-7a/SC-7b). Eviction is therefore load-bearing:

```
score(node) = recomputeCostMs(node) / bytes(node)   // both from UsdGenNodeStats; evict lowest first
never evict: the tail base (last restSpace node, re-read every frame); a node a live override
             targets; a capture cache whose rebuild is O(n log n) unless bytes > 4x buffer bytes
```

`USDGEN_MEMORY_BUDGET_MB` sets the ceiling; env/config, never authored (ADR §2.3). GPU-side per
100 k × 8 CV: points 9.6 MB + cubic indices 7.63 MB (MEASURED, EV-027) + primIndices ~2 MB ≈ **20 MB**
before widths and colour (DERIVED); S-1 records `gpuMemoryUsed`, so this is checked, not assumed.

### 2.7 Motion cache

`UsdGenMotionCache` is keyed by `(graphGen, surfaceGen, absTime)` and holds interleaved tile arrays,
19.2 MB per sample at 100 k × 16 CV (MEASURED, EV-075). With `usdGen:motion:mode = samples` and
`sampleCount = 3` the render-time cost is `H + 3·T + 2·m`, `m ≈ 1.0 ms` per memory-bound pass over
1.6 M points (MEASURED, EV-071…EV-073), not `3·(H+T)` — gate **R-1** verifies by counting evaluations.
Storm never asks for samples (`hdSt` has no `SamplePrimvar`;
`pxr/imaging/hdSt/basisCurves.cpp:975-990` reads `GetValue(0.0f)`), so motion profile P2 costs the
viewport nothing.

---

## 3. LOD and density levers

### 3.1 `usdGen:densityScale` and `usdGen:renderDensityScale`

`float usdGen:densityScale = 1` applies in the **interactive** context, `float
usdGen:renderDensityScale = 1` in the **render** context (`USDGEN_CONTEXT=render`,
`UsdGenImaging_SetContext()`, or the usdview toggle); the contexts are exclusive — neither scale
applies in the other — and both default 1. Both decimate by stable id: keep curve *i* iff
`UsdGenHash32(curveId_i, kSaltDensity) < keepFraction · 2^32`, with
`keepFraction = clamp(densityScale_groom · densityScale_description, 0, 1)` in the interactive context
and the same product of `renderDensityScale` in the render context (ADR R12, R13). `kSaltDensity ≠ 0`,
so the surviving set is never `{hairId < scale}`. Ids, sculpt deltas and clump ids survive a density
change, and the chunk partition is computed once per capture over the **full** id set with
`UsdGenChunkDesc::liveCount` tracking survivors — scales never re-scatter and never re-chunk.
Decimation happens at **capture**, before any styler evaluates, so a scale of 0.1 at 1 M costs the
100 k chain, not the 1 M chain — the only lever that reduces `draw_ms`, `interleave`, `upload` and
`evaluate` at once.

Cost of *changing* it: one whole-VBO reallocation, unconditional on any element-count change
(`pxr/imaging/hdSt/vboMemoryManager.cpp:605-620`), plus a cubic index rebuild of 2.6 ms warm / 5.2 ms
cold per 500 k patches (MEASURED, EV-027). The reallocation's own GPU cost is **UNMEASURED**; gate
**S-7** measures the pair — hence the drag rule below.

### 3.2 Density scrubbing without a topology change

During a density **drag** the buffer stays allocated at the description's maximum curve count and
surplus curves are *parked*: CVs collapsed to the root, `widths` 0, element counts unchanged, only
`points` and `widths` dirtied (S28 route (a)). The real counts commit once on release, with one
compaction pass restoring exact-size arrays (`points.size() == Σ curveVertexCounts`, asserted, SI-1).

Two costs the parked drag pays that the fastpath does not absorb. First, `widths` is in the
fastpath's exclusion set (`basisCurves.cpp:932-935`), so a parked frame is a full-vertex-loop frame
that re-uploads every non-points primvar — which is why S-7's threshold is 1.5×, not 1.0×. Second,
because decimation is by salted stable hash (ADR R13), parked curves spread over **every** chunk; no
chunk is fully parked, so the engine evaluates the max-density chain on every mouse move (1.02 ms at
100 k, EV-008; 7.59–7.67 ms at 1 M at 20 threads, EV-006) unless the kernel skips parked curves
per-CV. Nothing in class B (≤ 0.6 ms) covers the 1 M case, so above 200 k maximum curves the design
takes **S28 route (b)** — pre-baked density levels swapped at drag boundaries. S-7 runs the drag with
and without the per-CV skip.

### 3.3 refineLevel and the tumble tier (gate S-9)

Tiles pin `displayStyle/refineLevel = 2` (C2). The lever is real — 8.17 vs 23.93 ms at 200 k, **2.9×**
(MEASURED, EV-021) — but its *switch* cost is not measured: a refine-level change rebuilds the cubic
index buffer (EV-027) and dirties the geometric shader, triggering deep batch validation over every
draw item (`hdSt/commandBuffer.cpp:398-455`). Under 3 ms a "tumble tier" ships: refineLevel 1 while
the camera moves, restored on release; otherwise decimation is the only interactive lever
(`11-roadmap.md` §5.2, SC-4). Either way `refineLevel 0` is never used — it is *slower* than level 1
at 200 k because cubic curves degrade to a per-CV `LineList`
(`pxr/imaging/hdSt/basisCurves.cpp:292-301`).

### 3.4 Tile count as a culling lever

`nTiles` never changes during interaction (S27/S28) and is clamped to 32–256 (ADR R21). A
`usdGen:tileTarget` edit is a **topology change** — `UsdGenDirtyTopology` on the description, tile
prims added or removed — never an interactive path. At 100 k curves with `chunkSize = 512` the
reachable range is 32 to 196 (`03-execution-engine.md` §1.4). More tiles buy parallel buffer-source
resolve and index build (2.61 → 0.66 ms at 32 prims, 0.57 at 128, MEASURED, EV-028) and frustum
culling, whose granularity *is* the prim (`hdSt/primUtils.cpp:887-912`). They cost dependency fan-out
(MEASURED, EV-031) — hence the 256 ceiling and one `__dependencies` edge per tile, never per curve.
Gate **S-4** proves culling actually rejects; if it does not, the chunk partition changes from
Morton-over-roots to a UV-island partition before M3 ships. Gate **S-12** sweeps prim count against
the one-prim baseline before C2 freezes.

### 3.5 Progressive generation (v2, M8)

`asyncAllow`/`asyncPoll` with the usdview plugin setting `_allowAsync` during `registerPlugins` (S20).
Chunks publish in surface-locality order seeded from the camera and the previous generation stays on
screen until the new one completes. Gate **T-5** proves a plugin can enable it without the CLI flag.

---

## 4. Benchmark protocol

### 4.1 The EGL harness (tier T2 — the primary Storm harness)

Storm runs headlessly here with GPU timings: `HgiGL` only calls `GarchGLApiLoad()`
(`pxr/imaging/hgiGL/hgi.cpp:54`) and no `hdSt`/`hdx`/`usdImagingGL` code references `GlfGLContext`
outside test harnesses, so an `EGL_EXT_platform_device` context suffices. The harness is
`prototypes/storm-hair-look/eglctx.h`, carried into `usdGenTestUtils`; two details are mandatory, a
**compatibility** profile (core yields ~25 `GL_INVALID_ENUM` per frame and a white image) and a
**pbuffer** surface (surfaceless fails in `HgiGL_ScopedStateHolder`). One driver, `benchUsdGenStorm`,
replaces both prototype binaries — frame time from `bench_hair.cpp`, Hydra counters from
`prototypes/storm-throughput/hairbench.cpp` (unreachable from Python: 26.08 ships no `pxr.Hd` module)
— and takes **flags, not positionals**:

```
export LD_LIBRARY_PATH=/home/burkard/work/OpenUSD_26_08/lib
export HD_ENABLE_PERFLOG=1
./build/benchUsdGenStorm --scene bench/head100k.usda --cam /World/Cam --res 1280x720 \
    --refine 2 --frames 60 --warmup 10 --repeats 3 --json out/head100k_S1.json
./build/benchUsdGenStorm --scene bench/head100k.usda --res 1920x1080 --refine 2 --json out/S1_1080.json
./build/benchUsdGenStorm --scene bench/head100k.usda --counters --refine 2
```

The flag CLI deliberately replaces the prototype's positional argv
(`prototypes/storm-hair-look/bench_hair.cpp:25-31`, whose 7th positional is a deform flag, not a
warm-up count: `const bool deform = argc > 7 ? atoi(argv[7]) != 0 : false;`) — the positional form
silently measured a deforming scene when a static one was asked for. `--refine N` sets complexity
internally (`1.0→0 … 1.3→3`, `pxr/usdImaging/usdImagingGL/engine.cpp:2316-2350`). `usdrecord` is not a
driver here: it opens a GLX window and core-dumps.

### 4.2 Xvfb fallback (tier T3 — app loop, CPU numbers only)

`testusdview`, `usdview` and `usdrecord --renderer GL` run under the user-space Xvfb on
`DISPLAY=:77` with llvmpipe (`research/ENVIRONMENT.md` CORRECTIONS). **llvmpipe frame times are CPU
numbers and are never quoted as Storm GPU numbers.** T3 asserts behaviour (call counts, one
`_resetGUI`, one stage write per stroke, generation advance), never GPU milliseconds.

### 4.3 Workstation protocol (tier T4 — release criteria only)

No T4 gate is a milestone exit (ADR R39). The protocol is `docs/workstation-protocol.md`, and its
section numbering is owned by `10-build-dependencies-testing.md` §5.2, not by this document: **§§1–6
are `research/G-storm-hair-look-prototype.md` §6 verbatim**, usdGen appends **§7** and **§8**, and
`08-tools.md` §8.3 appends the three tool-loop checks **§9–§11**. This table is the gate mapping;
§5.4's `R-` rows cite the same numbers.

| Protocol § | Check | Gate or criterion |
|---|---|---|
| **§1** | usdview visual sign-off at complexity High against `prototypes/storm-hair-look/hair_pv_tangent.png`; the Low/Medium/High/Very High → refineLevel 0/1/2/3 ladder | none (sign-off; L-2 holds the headless half) |
| **§2** | interactive frame time under camera motion, tumbling G3 and G4 — the offscreen harness captures no present or compositor cost | no `R-` id — recorded only, and the frame counter `11-roadmap.md` §6.4's **RC-4** (a comb stroke holding ≥ 30 fps at 1080p) is read from |
| **§3** | MSAA / alpha-to-coverage vs OIT on ~1 px strands at 1080p and 4K | **R-2** |
| **§4** | non-NVIDIA drivers: compile the glslfx once on AMD and once on Intel with `TF_DEBUG=HDST_DUMP_FAILING_SHADER_SOURCE` | **R-3** (the AMD/Intel half) |
| **§5** | `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, the Metal/Vulkan proxy (`pxr/imaging/hdSt/codeGen.cpp:166, 187-196`), zero compile errors | **R-3** (the gate's registered section in `10-…` §6.5) |
| **§6** | Ptex: the CPU-side bake route, Storm builds here with `PXR_ENABLE_PTEX_SUPPORT=OFF` | none (covered headlessly by L-3, L-4, T-PTEX-1) |
| **§7** | hdPrman parity: `usdrecord` with 3 motion samples on G3 | **R-1** |
| **§8** | 4K interactive on G3 and G4 | none (recorded beside §2) |
| **§9** | the stroke: 60-move synthetic stroke on 10 k / 100 k / 1 M grooms, stroke-to-present time against the static frame (`08-tools.md` §8.3) | the end-to-end half of **T-1** (delta ≤ 2.0 ms over static with one tile dirty, UNMEASURED) |
| **§10** | the freeze: `SetActive(True)` to first presented frame; whether a `primvars/points` dirty avoids the topology re-upload (`08-tools.md` §8.3) | the GPU half of **T-4** |
| **§11** | the pick: probes P1 (and P3 if T-3 fails on occlusion) on real hardware (`08-tools.md` §8.3) | confirmation around **T-3**, which itself runs at T3 |

There is one numbering for that file and no `W-` ids anywhere; `08-tools.md` §8.3 specifies §§9–11
and `10-…` §5.2 lists all eleven sections.

### 4.4 The four canonical grooms

Generated by `usdGenTestUtils`' `usdGenBenchScene.py` (from
`prototypes/storm-throughput/gen_hair_stages.py`). Identical curve data across prim-count variants so
granularity is the only variable; every prim authors `extent`, `widths`, `hairT`, `hairId`, `st`,
`displayColor`, `type=cubic basis=bspline wrap=pinned`, `displayStyle/refineLevel = 2` and
`minScreenSpaceWidths = 1.0`.

| Id | File | Curves × CV | Surface | Purpose and gates |
|---|---|---|---|---|
| **G1** | `bench/lash4k.usda` | 4 000 × 8 | one 512-face patch | latency floor; the 0.85 ms measured point; per-edit overhead is visible here and nowhere else |
| **G2** | `bench/brow40k.usda` | 40 000 × 8 | 2 048-face patch, 2 `GeomSubset`s | subset scatter; the two-material **negative** case for S-5 (`drawBatches == 2`); the 5.01 ms point and class A's ≤ 40 k anchor |
| **G3** | `bench/head100k.usda` | 100 000 × 8 | 20 k-face scalp, **deforming** (an `ArmShotAnim`-class rig + a UsdSkel variant) | the ledger scene; S-1, S-2, S-3, S-5, S-6, S-8, S-12, E-1…E-3, T-1, T-2 |
| **G4** | `bench/fur200k.usda` | 200 000 × 8 | 80 k-face body | the 23.93 ms measured point; S-4 (culling at scale); S-9 tumble tier |

Each groom carries a density ladder authored as `usdGen:densityScale ∈ {1.0, 0.5, 0.25, 0.1}`, and G3
also ships a `head1M` variant at 10× base density for the engine and memory gates (E-1 at 1 M, E-5,
E-8, T-2) and for S-11's draw record.

**G3 and G4** ship in three prim-count variants for S-4, S-5 and S-12: **1 prim** (authored directly
by the generator; the clamp floor of 32 makes it unreachable through `tileTarget`), **32 tiles**
(`tileTarget = 32`) and **196 tiles** (`tileTarget = 256`, the maximum at both 100 k and 200 k). G1
and G2 are below the clamp: `nTiles = min(nChunks, …)` gives **8 tiles** for G1 (8 chunks) and **40**
for G2 at the default `tileTarget = 64`, so they ship as 1-prim and native-tile variants only (ADR
R21).

### 4.5 What to record, and the variance rules

Every benchmark writes one `usdGenBench.json` per scene with, at minimum:

```json
{ "scene": "head100k", "tier": "T2", "gate": "S-1", "host": "gb10-aarch64",
  "resolution": [1280, 720], "refineLevel": 2, "tiles": 49, "threads": 8,
  "frames": 60, "warmup": 10, "repeats": 3,
  "ms": { "median": 0.0, "p90": 0.0, "min": 0.0 },
  "counters": { "drawCalls": 0, "drawBatches": 0, "rebuildBatches": 0, "vboRelocated": 0,
                "itemsDrawn": 0, "sourcesCommitted": 0, "copyBufferCpuToGpu": 0, "gpuMemoryUsed": 0 },
  "usdGenStats": { "commits": 0, "cookedChunks": 0, "publishedTiles": 0,
                   "interleavedBytes": 0, "recompiles": 0, "evictions": 0 } }
```

`"tiles": 49` is not a free parameter: at 100 k curves with `chunkSize = 512` and `tileTarget = 64`,
nChunks = 196, chunksPerTile = 4, nTiles = 49.

1. **Median of 60 frames after 10 warm-up frames, plus p90 and min**; repeat 3 times (`--repeats`) and
   report the median of medians. There is no env knob for the repeat count:
   `10-build-dependencies-testing.md` §3.5 is the single env-var registry (ADR R35) and
   benchmark-driver flags are not env vars.
2. **Discard the first cold run** — the first `noticeCost` run was ~5× noisier than the rest, and the
   `SetTime` run-3 outlier was 8.5 ms on a 1.4 ms mean.
3. **Always report `drawCalls` and `drawBatches` beside the frame time.** A time change with an
   unchanged batch count is GPU-side; with a changed count it is CPU-side batching. Counters need
   `HD_ENABLE_PERFLOG=1`.
4. **Any inversion is reproduced twice before it is written down** — the refineLevel 0-vs-1 inversion
   was, and its mechanism is still inferred from source rather than profiled.
5. **Pin the thread count**: `USDGEN_THREAD_LIMIT` for the engine, `PXR_WORK_THREAD_LIMIT` for
   `WorkParallelForN`, BLAS threads to 1 in every Python benchmark (EV-065: 22 vs 0.56 ms).
6. **Never mix tiers.** llvmpipe numbers are labelled `tier: "T3"` and never compared with a T2 number.
7. **`perf` and `gdb -p` are unavailable here**; CPU profiles come from the in-process SIGPROF sampler
   (`prototypes/data-plane-benchmark/sampler.h`).

---

## 5. Gate registry

**This section is the single gate registry (ADR R40): id, metric, pass criterion, driver, tier,
milestone and status today. `00-request-and-scope.md`, `10-build-dependencies-testing.md` §6.5,
`11-roadmap.md` §2.0, `12-risks-decisions-open-questions.md` and `appendix-A-evidence-ledger.md` cite
this table and must agree with it.** Every one of them was re-checked against this table on
2026-09-05 and agrees; a future divergence is an edit to the sibling, not a second opinion. Release criteria
`RC-1…RC-11` live in `11-roadmap.md` §6.4; the T4 gates R-1…R-3 below are their measurable half.

ADR R40 fixes the tier and milestone of every id it names (B-1 included), and this section prints
those assignments verbatim. Six ids R40 does not name are minted here, where R40 leaves 09 standing:
**S-11** (the `head1M` draw record), **S-12** (the tile-count sweep), **SI-10** (session arbitration),
**SI-11** (the `reorder nameChildren` notice check), **T-EXPR-1** and **T-PTEX-1**.

Tiers (ADR R2, `10-…` §5.1): **T0** pure C++ over `UsdGenGraphDesc`, no `usd`/`hd`/stage; **T1**
headless scene index over the real `UsdImagingCreateSceneIndices` chain, sub-100 ms, no GL; **T2**
Storm through the EGL harness (§4.1); **T3** `testusdview` under Xvfb `:77`, behaviour only; **T4**
workstation protocol (§4.3), release only. **MEASURED-pass** = a prototype number already satisfies
the threshold and is carried in as the baseline; **UNMEASURED** = the gate produces the number.

### 5.1 Build and engine gates

| Gate | Tier | Metric | Pass criterion | Test | M | Status today |
|---|:--:|---|---|---|:--:|---|
| **B-1** | T0 | `DT_NEEDED` of `libusdGen.so` against `USDGEN_FORBIDDEN_LIB_REGEX`, and the sources against `USDGEN_FORBIDDEN_INC_REGEX` (both CMake cache variables, `10-build-dependencies-testing.md` §1.4) | zero forbidden entries (`usd*`, `usdImaging*`, `hd*`, `hdSt`, `hdx`, `glf`, `garch`, `hgi*`, `usdAppUtils`) | `testUsdGenLinkRule_{usdGen,usdGenMath,usdGenTestUtils}` + `testUsdGenIncludeRule` | **M0** | UNMEASURED — ADR R2, R37 |
| **E-1** | T0 | 5-op chain, G3, full run | ≤ **1.5 ms** at 8 threads in the private arena; the 20-thread number is quoted in parentheses only (ADR R27) | `benchUsdGenChain --curves 100000 --stylers 5 --chunk 512` | M1 | **MEASURED-pass** — 1.02 ms @8 thr (EV-008), 1.72–1.91 @20 thr (EV-001); 1 M at 8 thr UNMEASURED |
| **E-1r** | T0 | the same chain on a ragged buffer (`cvCount == 0` + `cvOffsets`) | ≤ 2× the uniform path | `benchUsdGenChain --ragged` | M2 | UNMEASURED — the path does not exist yet |
| **E-2** | T0 | 1 % of chunks dirty on all nodes | ≤ 0.10 ms | `benchUsdGenSparse` | M1 | **MEASURED-pass** — 0.035–0.044 ms (EV-002) |
| **E-3** | T0 | terminal-parameter edit only | ≤ 0.6 ms | `benchUsdGenSparse --terminal` | **M2** | **MEASURED-pass** — 0.179–0.411 ms (EV-004) |
| **E-4** | T0 | nanoflann kNN capture, 100 k / 1 M rest roots, 4 k guides, 8 threads | ≤ 25 ms at 100 k **and linear in roots** (ASSUMPTION). Stop: > 300 ms at 1 M ⇒ SC-6 | `benchUsdGenKnn` | M0 pre-work (**PW-1**), binding **M3** | UNMEASURED — the only capture term with no number |
| **E-5** | T0 | RSS at 1 M × 8 CV, 5 nodes, default caching | ≤ 800 MB | `benchUsdGenMemory` | **M3**, re-run M7 | **UNMEASURED for the shipped chain; at risk** — the MEASURED anchor is 668 MB with per-node caching of the **position plane alone** (EV-007); §2.6's other planes give **950–1050 MB DERIVED**, above the threshold. E-5 decides whether per-node caching becomes opt-in above 500 k curves (SC-7a/SC-7b) |
| **E-6** | T0 | append one node to a 200-node groom | ≤ 0.2 ms **and exactly 1 node rebuilt** | `testUsdGenGraph` | M1 | UNMEASURED — VDF's 1.4 µs/node (EV-013) is a different compiler |
| **E-7** | T0 | thread scaling 1 → 8 on E-1 | ≥ 3× | `benchUsdGenChain --threads` | M1 | **MEASURED-pass** — 3.90 → 1.02 ms = 3.8× (EV-008) |
| **E-8** | T0 | determinism at 1/2/4/8/20 threads, `-ffp-contract=off` | bitwise identical | `testUsdGenKernelDeterminism` on G1–G4 and `head1M` | **M1**, re-run M4 | UNMEASURED for usdGen |

### 5.2 Scene-index gates

| Gate | Tier | Metric | Pass criterion | Test | M | Status today |
|---|:--:|---|---|---|:--:|---|
| **SI-1** | T1 | array exactness on every published tile | `points.size() == Σ curveVertexCounts`, hard assert, release builds too | `testUsdGenTileContract` | M1 | UNMEASURED; the failure mode is MEASURED (EV-026) |
| **SI-2** | T1 | invalidation set for a clump-amount edit | exactly `primvars/points/primvarValue` + `extent/*` on the dirty tiles, nothing else | `testUsdGenInvalidation` | M1, re-run M2 for overlaid prims | UNMEASURED |
| **SI-3** | T1 | cook count over 10 interactive events | exactly 10 commits; every commit and notice on the app or notice thread (thread-id assert); **zero cooks inside `GetPrim`**; the same script under `MergingSceneIndexNoticeBatchBegin/End` publishes identical points with at most one extra commit per batched frame (`06-imaging.md` §3.9) | `testUsdGenScheduling` | M1 | UNMEASURED; the 2-`PrimsDirtied`/frame precondition is MEASURED (EV-037) |
| **SI-4** | T1 | 8 readers × 100 publishes | 0 torn reads | `testUsdGenSnapshotRace` | M1 | **MEASURED-pass at 8 × 20** — 16 734 reads, 0 torn (EV-039); the 8 × 100 form is what the gate runs |
| **SI-5** | T1 | chain order | usdGen strictly after UsdSkel/RigExec/flattening, before every Storm/hdPrman plugin | `testUsdGenChainOrder` | **M1** | **MEASURED-pass** (`research/G-chain-order-probe.md` §2–3) — first green in M0, exits at M1 (ADR R40) |
| **SI-6** | T1 | initial population, both paths | identical prim set from `PrimsAdded` replay and from the bounded first-`GetChildPrimPaths` traversal; ≤ 5 ms on the 11 005-prim stage (ASSUMPTION, ADR R28) | `testUsdGenPopulation` | M1 | UNMEASURED |
| **SI-7** | T1 | adapter coverage | every `usdGen:*` property of all five registered types **and** `usdGen/rest/points` from the `UsdGenRestAPI` adapter appears **and dirties** | `testUsdGenAdapter`, `testUsdGenRestAdapter` | M1 | UNMEASURED; the *absence* is MEASURED (EV-081) |
| **SI-8** | T1 | auto-applied `UsdGenMaskAPI` on a codeless type | the API's properties appear on every `UsdGenOperator` subtype | `testUsdGenAutoApply` | M0 pre-work (**PW-5**), binding M1 | UNMEASURED |
| **SI-9** | T1 | cost of the private `HdSiExtComputationPrimvarPruningSceneIndex` wrapper on a **production-density skinned scalp** (the probe mesh had 10 points) | the per-deforming-frame wrapper cost is recorded and stays inside class A's ≤ 1.0 ms usdGen share (§0.1; ASSUMPTION until the gate runs); red ⇒ stop condition SC-13 (`05-static-curves-and-deformation.md` §4.2) | `testUsdGenPruningCost` | **M2** | UNMEASURED — this cost is the metric ADR R40 assigns the id, and nothing else. `testUsdGenSkelInterop` carries the companion assertion that the wrapper resolves `skinnedPoints` with `HD_ENABLE_SCENE_INDEX_EMULATION` **off** (`05-…` §4.2, §9) |
| **SI-10** | T1 | two scene-index instances attached to one session | identical generation, prim set and frame for one commit | `testUsdGenSessions` | **M2** | UNMEASURED — minted here for `12-…` RK-09; SI-9 is the pruning-wrapper gate alone (ADR R40). Cited by `12-…` RK-09/§3/§5, `00-…` §5.4, `05-…` §4.2, `10-…` §6.5 and `11-…` §2.0 |
| **SI-11** | T1 | does `reorder nameChildren` reach the scene index as an invalidation? | recorded; **nothing in the design depends on the answer** (ADR §2.1) | `testUsdGenReorderNotice` | M0 pre-work (**PW-6**, `11-…` §1.1), record only | UNMEASURED — minted here for `appendix-A-evidence-ledger.md` §5 U13 and `12-…` Q-06; `SI-10` is the session gate above |

### 5.3 Storm gates (EGL harness)

| Gate | Tier | Metric | Pass criterion | Test | M | Status today |
|---|:--:|---|---|---|:--:|---|
| **S-1** | T2 | G3 static draw, refineLevel 2, **720p and 1080p** | ≤ **12.8 ms** at 720p (= 16.6 − 1.35 rig − 1.0 usdGen − 1.44 upload, §0.2). `design/proposal-performance.md` §11.2's 14 ms is kept only as a regression ceiling; 1080p is a separate baseline | `benchUsdGenStorm --static` | M1 | UNMEASURED at 100 k — ~12.2 ms is DERIVED from EV-020/EV-021; endpoints MEASURED |
| **S-2** | T2 | deform frame: scene-index `points` publish, 49 tiles | ≤ 2.0 ms delta over static | `benchUsdGenStorm --deform` | M2 | UNMEASURED; expected ≈1.44 ms DERIVED from EV-022. Also measures §2.1's interleave and `extent` |
| **S-3** | T2 | one-tile edit (1 of 49 dirty) | ≤ 1.2× static | `benchUsdGenStorm --onetile` | M2 | UNMEASURED |
| **S-4** | T2 | culling: camera frames ¼ of G4 | `itemsDrawn` drops ≥ 3× at 32 and 196 tiles, not at all at 1 prim | `benchUsdGenStorm --cull` | **M2** | UNMEASURED |
| **S-5** | T2 | batching: G3's 49 tiles one material; G2's two materials as the negative case | `drawBatches == 1, drawCalls == 1` on G3; `drawBatches == 2` on G2 | `benchUsdGenStorm --batches` | M1 | UNMEASURED; one indirect draw per batch is source-verified |
| **S-6** | T2 | no relocation across deform frames | `vboRelocated == 0` | `benchUsdGenStorm --deform` (counters) | **M1** | UNMEASURED — ADR R40 places it at M1, with the first deforming tiles |
| **S-7** | T2 | density scrub: parked-CV drag vs a real count change, with and without the per-CV skip (§3.2) | parked frame ≤ 1.5× a points-only frame | `benchUsdGenStorm --scrub` | M5 | UNMEASURED; the count-change penalty is source-verified, its GPU cost is not |
| **S-8** | T2 | `hairTangent` variant A (`inData.Neye`) vs B (vertex primvar) on a deforming G3 | A wins on frame time **and** compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, else B becomes default | `testUsdGenStormTangent` + `testUsdGenStormHgiResource` | M0 pre-work (**PW-2**), **decides M1** | UNMEASURED. If B wins, +1.5–2.5 ms per deforming frame (DERIVED from EV-022) and §0.2 no longer fits 16.6 ms |
| **S-9** | T2 | refineLevel 1 vs 2 **and the switch cost** | switch < 3 ms ⇒ the tumble tier ships as an option | `testUsdGenStormRefine` on G4 | M0 pre-work (**PW-3**), **decides M1** | UNMEASURED switch; the 8.17 vs 23.93 ms lever is MEASURED (EV-021) |
| **S-10** | T2 | in-place overlay on a frozen prim (v2, ADR R29) | `PrimsRemoved`/`PrimsAdded` versus an in-place `points` dirty | `testUsdGenStormInPlace` | **M8** | UNMEASURED. ADR R29 reserves `S-10` for this v2 gate |
| **S-11** | T2 | G3's `head1M` variant, static draw, refineLevel 2, 720p | record only, no threshold — establishes whether §2.5's linear fit holds past 200 k | `benchUsdGenStorm --scene bench/head1M.usda --static` | M7 | UNMEASURED; ~118 ms is DERIVED (extrapolated 5×) from EV-019/EV-020/EV-021. `S-11` is the `head1M` record; the tile sweep is `S-12` (next row) |
| **S-12** | T2 | prim-count sweep at 100 k × 8 CV: 1 / 32 / 128 / 196 tiles | ≤ 1.15× the one-prim frame time at `tileTarget = 64`, `drawBatches == 1` | `benchUsdGenStorm --tilesweep` | **M1**, before C2 freezes | UNMEASURED; the CPU half is MEASURED (EV-028, EV-031). This is the sweep `12-…` RK-07 / Q-02 and appendix A §2.11 / §5 U1 cite |

### 5.4 Look, tools, instancing, render time

| Gate | Tier | Metric | Pass criterion | Test | M | Status today |
|---|:--:|---|---|---|:--:|---|
| **L-1** | T2 | Storm render-context resolution | `outputs:glslfx:surface` resolves before plain `outputs:surface`; if not, flip `USDGEN_STORM_MATERIAL_OVERRIDE` to default ON | `testUsdGenStormMaterial` | M0 pre-work (**PW-4**), decides M1 | UNMEASURED; source says it should (`hdSt/renderDelegate.cpp:695-707`) |
| **L-2** | T2 | all three shipped `.glslfx` files: Sdr parse, compile, golden image, refineLevel precedence | exactly the 20 `inputs:` of `07-…` §2.2 and each variant's primvars metadata; 0 warnings, 0 GL errors; golden match against `prototypes/storm-hair-look/hair_pv_tangent.png`; the per-prim `displayStyle/refineLevel` wins over app complexity. **Guards C5** | `testUsdGenStormLook` | M1 | UNMEASURED (`07-look-maps-expressions.md` §9.1) |
| **L-3** | T0 | map and expression determinism | bitwise-identical baked primvars at 1 and 8 threads; `Expression::isThreadSafe()` true for every shipped function; `rand()` reproducible | `testUsdGenExpr --threads`, `testUsdGenLookBake` | M4 | UNMEASURED |
| **L-4** | T0/T1 | map capture cost at 1 M roots, 8 threads | ≤ **200 ms** for one image map + one Ptex map + one expression (ASSUMPTION), and `PtexCache::Stats.fileReopens == 0` at the default cache size | `benchUsdGenMaps` | M4 | UNMEASURED; ≈20 ms + ≈4.4 ms are DERIVED from EV-067/EV-069/EV-070 |
| **L-5** | T1 | `UsdGenImaging_ReloadMaps()` invalidation | re-captures exactly the nodes whose maps changed and dirties exactly the baked primvar's `primvarValue` on exactly the affected tiles; `displayColor` never co-dirtied with `points` | `testUsdGenMapReload` | M4 | UNMEASURED |
| **T-1** | T3 | brush move at 100 k, press/move/release | ≤ 1 ms Python + engine per move | `testUsdviewUsdGenComb.py` | M5 | **MEASURED-pass on components** — 21.3 µs Python (EV-061) + 0.035 ms engine (EV-002); end-to-end UNMEASURED |
| **T-2** | T1 | `UsdGenImaging_PickCV` at 100 k / 1 M CVs | ≤ 0.25 / 2.5 ms | `testUsdGenPick` | M5 | **MEASURED-pass** — 166 µs / 1.66 ms (EV-064) |
| **T-3** | T3 | CPU pick vs `view.pick()` at 100 pixels | disagreements only where a CV is occluded | `testUsdviewUsdGenPick.py` | M5 | UNMEASURED — runnable here |
| **T-4** | T3 | one freeze on a 2 205-prim stage | ≤ 5 ms **author** (`_resetGUI` excluded, ADR R40) **and exactly one** `_resetGUI` | `testUsdviewUsdGenFreeze.py` | M5 | **MEASURED-pass** — 3.45 ms author (EV-050); `_resetGUI` is 35.4 ms (EV-051) and must be coalesced |
| **T-5** | T3 | plugin sets `_allowAsync` without the CLI flag | 1 `asyncAllow`, ~10 `asyncPoll`/s | `testUsdviewUsdGenAsync.py` | **M8** | UNMEASURED — progressive generation is v2 (ADR R38) |
| **T-EXPR-1** | T0 | SeExpr sandbox | `SE_EXPR_PLUGINS` cleared before `ExprFunc::init()`; closed function set; a parse error yields a fallback value, never an exception into Hydra | `testUsdGenExpr --sandbox` | M4 | UNMEASURED — filed from `12-…` RK-15 |
| **T-PTEX-1** | T0 | Ptex face-id agreement | ids match `Far::PtexIndices` on quads, n-gons and all-triangle meshes; a `GeomSubset` does not shift them | `testUsdGenPtex` | M4 | UNMEASURED — filed from `12-…` RK-16 |
| **T-INST-1** | **T1** | instancer pick round-trip | a click on an instance resolves to the `UsdGenDescription` through `primOrigin` | `testUsdGenInstancerPick` | M6 | UNMEASURED. ADR R40 assigns **T1**; `10-…` §5.6 registers `testUsdGenInstancerPick` as the T1 binary and keeps the app-level round trip as the unlabelled T3 script `testUsdviewUsdGenCards.py` |
| **T-INST-2** | T1 | prototype rebasing | `usdGen:surface` inside a propagated native prototype rebases correctly; `instancedBy` has exactly one target | `testUsdGenInstancer` | M6 | UNMEASURED (the rebase arithmetic is an ASSUMPTION) |
| **R-1** | T4 | `usdrecord`/hdPrman, 3 motion samples on G3 | correct blur **and** `k·tail` evaluations, not `k·(head+tail)` | workstation protocol **§7** (§4.3) | release (built M7) | UNMEASURED; counted via `UsdGenStats.motionSamples` |
| **R-2** | T4 | MSAA/A2C vs OIT on 1-px strands at 1080p and 4K | visual sign-off, recorded which reads better | workstation protocol **§3** (§4.3) | release | UNMEASURED |
| **R-3** | T4 | `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` and AMD/Intel glslfx compile | 0 errors | workstation protocol **§5**, with §4 for the AMD/Intel half (§4.3) | release | UNMEASURED (only `hgiGL` is built here) |

### 5.5 Milestone exit summary

Derived from §5.1–§5.4 above, which is the registry; `10-…` §6.5 and `11-…` §2.0 conform to it.

| Milestone | Gates that must be green to exit |
|---|---|
| **M0** | B-1; pre-work numbers and decision lines recorded for PW-1 E-4, PW-2 S-8, PW-3 S-9, PW-4 L-1, PW-5 SI-8, PW-6 SI-11 (SI-5 is first green here and exits at M1) |
| **M1** | E-1, E-2, E-6, E-7, E-8; SI-1…SI-8; S-1, S-5, S-6, S-12; L-2; the S-8 / S-9 / L-1 decisions taken |
| **M2** | E-1r, E-3; SI-2 re-run on overlaid prims; SI-9, SI-10; S-2, S-3, S-4 |
| **M3** | E-4 against the real `GuideInterpolate` capture; E-5; E-1 still green with 7 nodes |
| **M4** | E-8 re-run; L-3, L-4, L-5; T-EXPR-1, T-PTEX-1 |
| **M5** | T-1, T-2, T-3, T-4; S-7 |
| **M6** | T-INST-1, T-INST-2 |
| **M7** | E-5 re-run; S-11; full re-run of E-*/SI-*/S-* on G1–G4; R-1 built and running |
| **M8** | T-5; S-10 |
| **release** | R-1, R-2, R-3 (all T4), inside `11-roadmap.md` §6.4's RC-1…RC-11 |

---

## 6. Profiling and observability

### 6.1 `UsdGenStats` and `UsdGenNodeStats`

`03-execution-engine.md` §9.2 is the normative declaration of `UsdGenStats` and `UsdGenNodeStats`;
this document does not redeclare them. `bytesOwned` and `lastEvalMs` are load-bearing, not decoration:
§2.6's eviction score is `recomputeCostMs(node)/bytes(node)` and both terms come from
`UsdGenNodeStats` (`03-execution-engine.md` §6.4, the `score(node)` block). `UsdGenNodeStats` is read
three ways from one source of truth: the groom panel's stack-profiler column (`08-tools.md`), the
usdview HUD line
(`usdGen 0.9 ms  49/49 tiles  196/196 chunks  gen 1042  86 MB` — 196 chunks and 49 tiles are the same
100 k groom, §4.5), and CI through `UsdGenImaging_GetStatsJson()`, which is what `testusdview` scripts
and §4.5's JSON assert against. Counters are **always compiled in**; `USDGEN_DIAGNOSTICS` only raises
the `TfDebug` verbosity of the same counters (registry: `10-build-dependencies-testing.md` §3.5, ADR
R35, which carries it).

### 6.2 Trace scopes and the gates they evidence

The list is `03-execution-engine.md` §9.2's — one table, two documents, identical (re-checked
2026-09-05). Scope names are phase-shaped (`TRACE_SCOPE` / `TRACE_FUNCTION`; 26.08 has no `TfTrace`
symbol) and the gate is a column, never part of the string (`10-build-dependencies-testing.md`
§7.2); `UsdGen::Capture` names **L-4** beside E-4 because the map bakes inside that scope are what
L-4 measures.

| Scope | Covers | Gate |
|---|---|---|
| `UsdGen::Route` | `_PrimsDirtied` accumulation, locator → node → chunk → tile routing | SI-2, SI-3 |
| `UsdGen::Compile` | Merkle + graph digests, Kahn sort, sub-graph rebuild | E-6 |
| `UsdGen::Capture` | scatter, Morton sort, kd-tree, guide weights, clump ids, map bakes | E-4, L-4 |
| `UsdGen::Evaluate<opType>` | one node's chunk sweep | E-1, E-1r, E-2, E-3, E-7 |
| `UsdGen::Interleave` | SoA → AoS + per-tile extent; the `I` term of §2.1 | E-1, S-2 |
| `UsdGen::Publish`, `UsdGen::Diff` | generation swap, prev/next diff, notice emission | SI-2, SI-3, SI-4 |
| `UsdGen::Motion` | `UsdGenMotionCache::Fill` | R-1 |

`TF_DEBUG` channels: `USDGEN_COMPILE`, `USDGEN_DIRTY`, `USDGEN_COMMIT`, `USDGEN_PUBLISH`,
`USDGEN_CAPTURE`, `USDGEN_MEMORY`. `USDGEN_COMMIT` additionally routes per-node `UsdGenNodeStats` into
`TraceCollector`.

### 6.3 Capturing a trace

```
HD_ENABLE_PERFLOG=1 TF_DEBUG=USDGEN_COMMIT $USD/bin/testusdview --renderer GL \
  --testScript bench/traceScript.py --traceToFile trace.json --traceFormat chrome bench/head100k.usda
HD_ENABLE_PERFLOG=1 TF_DEBUG=HDST_DRAW_BATCH ./build/benchUsdGenStorm --scene bench/head100k.usda ...
TF_DEBUG=HD_RPRIM_UPDATED   # per-prim shader keys and BAR transitions
TF_DEBUG=HD_DIRTY_LIST      # dirty-list rebuild vs reuse
HD_ENABLE_GPU_FRUSTUM_CULLING=0/1   # culling A/B for S-4
```

Engine CPU profiles come from the SIGPROF sampler, not `perf` (§4.5 rule 7). Storm's commit-phase
splits are already scoped (`hdSt/resourceRegistry.cpp:862, 913, 961, 984, 1018`) and appear in the
same trace.

---

## 7. Known cliffs, and how the design avoids each

Every cost cell carries its ADR R42 tag.

| # | Cliff | Cost, tagged | How the design avoids it |
|---|---|---|---|
| 1 | 4096-curve chunks | 9.40 vs 1.83 ms at 512 — 5.1× (MEASURED, EV-009) | `chunkSize = 512`, band 128–1024, `nChunks ≥ 8·W` (§2.3) |
| 2 | Padded `points` | fallback red without `curveIndices`; **3.46 ms** with them (MEASURED, EV-026) against a 0.22 ms exact-size resolve (MEASURED, EV-024) | exact-size arrays, asserted by the publisher (SI-1) |
| 3 | Too many prims | fan-out 130 ms at 100 k prims (MEASURED, EV-031); ~210 ms total scene-index cost per deform frame (MEASURED, EV-032) | tiles clamped 32–256, one `__dependencies` edge per tile, prim set fixed per session |
| 4 | A non-points primvar dirtied per frame (`widths`, `normals` included) | drops the prim off the points fastpath (`basisCurves.cpp:932-935`) and re-uploads every non-points primvar — **UNMEASURED, gate S-8**; the 0.51 µs/prim descriptor recompute is MEASURED (EV-030) | variant A publishes no `hairTangent`; `hairT`, `st`, `hairId`, `displayColor` are static. §3.2 is where `widths` still bites |
| 5 | `ComputeDirtyLocators` where usdGen owns the prim | clears the primvar-descriptor cache, 0.51 µs/prim (MEASURED, EV-030) → 0.51 ms/frame at 1 000 prims (DERIVED) | bare `primvars/points/primvarValue` where usdGen owns the prim; `ComputeDirtyLocators` **and** the bare `primvars` locator only on overlaid upstream prims (S5, S30) |
| 6 | A persistent `VdfNetwork` | 4× slower at 100 k, 9–15× at 1 M, 47 ms for one node re-run at 1 M, 1 263 MB RSS (MEASURED, EV-011, EV-013) | bespoke TBB DAG (S21); VDF stays in the control plane |
| 7 | Cooking in `GetPrim` | cooks on 4 worker threads and emits zero notices; the eager alternative over-cooks 2–2.9× (MEASURED, EV-038) | deferred commit + atomic snapshot publish (**I7**, ADR §4.3); the backstop is withdrawn (ADR §1, S18(c)) |
| 8 | Element-count change during a drag | index rebuild 2.6 / 5.2 ms per 500 k patches (MEASURED, EV-027); the whole-VBO reallocation's GPU cost is **UNMEASURED, gate S-7** | parked CVs during the drag, one topology publish on release; pre-baked levels above 200 k (§3.2) |
| 9 | `RemovePrim` in an interactive path | 1.47 ms at 100 k, 13.47 ms at 1 M (MEASURED, EV-044); a spurious `Tf.ErrorException` with OpenExec attached (MEASURED, EV-079) | undo of a live freeze is `SetActive(false)` — 0.02 ms flat (MEASURED, EV-044) |
| 10 | Per-stroke prim resync in usdview | `_resetGUI` 35.4 ms at 2 205 prims, 105.5 at 11 005 (MEASURED, EV-051) | brushes edit `points` on an existing prim; structural edits batch into one `Sdf.ChangeBlock` |
| 11 | Python array traps | `Vt.FromBuffer` / `attr.Set(ndarray)` 4.3 / 4.9 ms at 1 M CVs (MEASURED, EV-057) vs 1.63–1.82 µs for `attr.Set(VtArray)` (MEASURED, EV-058); 22 vs 0.56 ms per matmul under load (MEASURED, EV-065) | C++ hands back a `VtVec3fArray` (0.13–0.18 µs, EV-055) and Python authors *that*; `usdGenLib.py` pins BLAS threads to 1 before importing numpy |
| 12 | `varying` widths | 6.48 ms CPU expansion per 100 k curves (MEASURED, EV-033) | `widths` is vertex, or constant when uniform; never varying |
| 13 | refineLevel 0 as "the fast mode"; per-prim material or refineLevel | 12.43 vs 8.17 ms at 200 k (MEASURED, EV-021); a differing shader hash or BAR pointer fragments the batch (source-verified) | LOD decimates curve count, level 0 is never a tier; one material and one refineLevel per tile set |

---

## 8. Testing

The perf suite is a **regression** suite: every gate records its number into `usdGenBench.json` and CI
fails on a threshold breach, not on a delta. Every test name below is
`10-build-dependencies-testing.md` §5.6's — that section is the CTest name registry and this document
does not mint names.

| Tier | Binaries and suites | Gates covered | Budget |
|---|---|---|---|
| **T0** | `testUsdGenLinkRule_{usdGen,usdGenMath,usdGenTestUtils}`, `testUsdGenIncludeRule`, `benchUsdGenChain`, `benchUsdGenSparse`, `benchUsdGenKnn`, `benchUsdGenMemory`, `benchUsdGenMaps`, `testUsdGenGraph`, `testUsdGenKernelDeterminism`, `testUsdGenExpr`, `testUsdGenPtex`, `testUsdGenMaps`, `testUsdGenLookBake` | B-1; E-1…E-8 over G1–G4 + `head1M`; L-3, L-4; T-EXPR-1, T-PTEX-1. No `usd`, no `hd` — B-1 proves it (S8) | ≤ 60 s |
| **T1** | `testUsdGenTileContract`, `testUsdGenInvalidation`, `testUsdGenScheduling`, `testUsdGenSnapshotRace`, `testUsdGenChainOrder`, `testUsdGenPopulation`, `testUsdGenAdapter`, `testUsdGenRestAdapter`, `testUsdGenAutoApply`, `testUsdGenPruningCost`, `testUsdGenSessions`, `testUsdGenReorderNotice`, `testUsdGenMapReload`, `testUsdGenPick`, `testUsdGenInstancer`, `testUsdGenInstancerPick` | SI-1…SI-11, L-5, T-2, T-INST-1, T-INST-2 | ≤ 100 ms each |
| **T2** | `benchUsdGenStorm`, `testUsdGenStormTangent`, `testUsdGenStormRefine`, `testUsdGenStormMaterial`, `testUsdGenStormHgiResource`, `testUsdGenStormLook`, `testUsdGenStormInPlace` (M8) | S-1…S-12, L-1, L-2; golden images against `prototypes/storm-hair-look/hair_pv_tangent.png` | ≤ 5 min sweep |
| **T3** | `testUsdviewUsdGenComb.py`, `testUsdviewUsdGenFreeze.py`, `testUsdviewUsdGenPick.py`, `testUsdviewUsdGenCards.py`, `testUsdviewUsdGenAsync.py` under Xvfb `:77` | T-1, T-3, T-4, T-5; `testUsdviewUsdGenCards.py` is the unlabelled app-level companion of T-INST-1, which is a T1 gate proved by `testUsdGenInstancerPick`. **Behaviour only, never GPU ms** | ≤ 30 s each |
| **T4** | workstation protocol (§4.3) | R-1, R-2, R-3 — release criteria, run by a human at a display | manual |

Three properties are asserted rather than timed, because a slow-but-correct result is still a bug:
**exactness** (SI-1, hard assert on every publish, release builds included), **determinism** (E-8 and
L-3, bitwise identity across thread counts with `-ffp-contract=off`) and **cook count** (SI-3, one
commit per user-paced edit, on the app or notice thread).

Every DERIVED or UNMEASURED number in §0–§2 is owned by a gate, and this document is wrong until that
gate runs. Five would change the plan's shape if they came back badly: **E-4** (capture cost sets the
operator schedule, SC-6), **E-5** (§2.6's second accounting already exceeds the threshold, SC-7a),
**S-1** (the whole ledger hangs off it; class A's targets are ASSUMPTION until it lands), **S-2**
(upload and interleave) and **S-8** (a `hairTangent` republish per deforming frame costs the fastpath,
SC-2).

---

## 9. Out of scope

GPU evaluation of the operator chain (every number here is CPU and Storm dominates the frame — v3 at
the earliest). Benchmarks of renderers other than Storm and hdPrman. Windows and macOS timings.
Multi-GPU. A performance *model* of hdPrman's render time (R-1 asserts the evaluation count and the
blur's correctness, not prman's throughput). Automated bisection of perf regressions. Numbers at any
resolution other than 1280×720 until S-1 records 1920×1080. The release criteria themselves —
`RC-1…RC-11` are `11-roadmap.md` §6.4's, and §5 registers only their measurable half. Anything about
interactive quality that is not a millisecond: MSAA and OIT legibility are a human sign-off (R-2), not
a metric.

---

## 10. Sources

**Design.** `design/adr-v1.md` §1 (chunk≠tile, S29 `hairId` float, the S31 refineLevel correction),
§2.3 (density controls, tile count, machine tuning as env), §4.1–4.3 (invariants **I1–I8**, commit
triggers, the thread rule), §5.4–5.5, §7, §8, and the §9 rulings R1 (vocabulary and `EV-nnn`), R2
(tiers, the `B-` family), R12/R13 (hash, density), R20 (publish ring; `VtArray::IsUnique()` does not
exist), R21 (tile arithmetic), R27 (8-thread baselines, E-1 ≤ 1.5 ms), R28, R29 (S-10), R35 (env-var
registry), R37 (B-1), R39 (T4 gates are release criteria), R40 (§5 is the gate registry),
R41 (the only frame ledger; 60 Hz at 100 k is not established), R42 (the four tags), R43 (accepted
file:line ranges), R45 (cross-references must resolve). `design/brief-v1.md` S21–S32, S37–S45 (S28's
two density-scrub routes). `design/proposal-performance.md` §0.2 (the ledger corrected here),
§5.7–5.11, §6.2–6.3, §11.1–11.2 (the gate matrix this registry supersedes).
`design/judge-evidence.md` §2.3–2.4; `design/judge-delivery.md` §3 item 5, §4.1, §7.

**Plan siblings this document defers to** (ADR R45 — each section was read while writing this one).
`10-build-dependencies-testing.md` §1.4 (`USDGEN_FORBIDDEN_LIB_REGEX`, `USDGEN_FORBIDDEN_INC_REGEX`),
§3.5 (the env-var registry), §5.2 (the harnesses and the `docs/workstation-protocol.md` §§1–11
numbering), §5.6 (the CTest name registry), §6.5 (the gate→test map that restates §5);
`03-execution-engine.md` §3.2–§3.3 (the four digests, hence `UsdGen::Compile`'s scope) and §9.2
(`UsdGenStats`, `UsdGenNodeStats`, the trace-scope table §6.2 mirrors);
`appendix-A-evidence-ledger.md` §2 (the flat `EV-001…EV-092` ledger) and §2.0 (the retired-handle
map); `11-roadmap.md` §1 (PW-1…PW-6), §2.0 (gate → milestone), §6.4 (RC-1…RC-11);
`12-risks-decisions-open-questions.md` §3 (RK-07, RK-09, RK-15, RK-16).

**Research.** The reports named per `EV-nnn` block in §1, plus
`research/G-storm-throughput-and-prim-granularity.md` §2–§3 (the protocol §4 extends),
`research/A3-usdrig-tools.md` §6 (conventions) and `research/ENVIRONMENT.md` incl. CORRECTIONS.

**Prototypes carried in.** `prototypes/data-plane-benchmark/{tbbBench.cpp,kernel.h,simdBench.cpp,sampler.h}`
→ `benchUsdGenChain`/`benchUsdGenSparse` + `usdGenTestUtils/sampler.h`;
`prototypes/storm-hair-look/{eglctx.h,bench_hair.cpp,usdGenHairPreview.glslfx,hair_pv_tangent.png}` and
`prototypes/storm-throughput/hairbench.cpp` → `usdGenTestUtils/eglctx.h`, `benchUsdGenStorm`,
`usdGenShaders/`, the golden image; `prototypes/storm-throughput/gen_hair_stages.py` →
`usdGenBenchScene.py`; `prototypes/tool-loop/*` → `tests/perf/` and the two Python surfaces;
`prototypes/freeze-bake/uv/testUsdviewFreezeCost.py` → `testUsdviewUsdGenFreeze.py`;
`prototypes/thirdparty-bench/{seexpr_bench.cpp,ptex_test.cpp}` → `tests/perf/benchMaps.cpp`;
`prototypes/motion-blur/mbbench.cpp` → `tests/perf/benchMotionSamples.cpp` — §2.7's terms are reproducible
from it (single thread, `-O2`, 1.6 M `float3`).

**OpenUSD 26.08 source.** Every file:line was re-verified by grep against
`/home/burkard/work/OpenUSD` while this document was written. The load-bearing ones:
`hdSt/vboMemoryManager.cpp:605-620` (unconditional reallocation on any element-count change),
`hdSt/basisCurves.cpp:932-935` (the points fastpath), `:292-301` (refineLevel-0 downcast to
`HdTokens->linear`), `:975-990` (`GetValue(0.0f)` — Storm never samples in time),
`hdSt/basisCurvesComputations.h:238-247` (`HdSt_ExpandVarying`), `hdSt/primUtils.cpp:887-912` (no
extent, no culling), `hdSt/commandBuffer.cpp:398-455` (batch revalidation),
`hd/sceneIndexAdapterSceneDelegate.cpp:510-523` (descriptor-cache clear),
`hdSt/renderDelegate.cpp:695-707` (`materialRenderContexts`, the range ADR R43 accepts),
`hdSt/codeGen.cpp:166, 187-196`, `usdImagingGL/engine.cpp:2316-2350` (complexity → refine level) and
`base/vt/array.h:39-55` (`Vt_ArrayForeignDataSource`), with the private `_IsUnique()` at `:1023`.
