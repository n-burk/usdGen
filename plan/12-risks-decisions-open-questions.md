# usdGen — Risks, decisions and open questions

Date: 2026-09-05. Status: plan v1 (draft, pending review).

This document is the plan's register: every settled decision, every rejected alternative, every risk
with the gate that retires it, every open question with the default in force until it closes, and
every assumption. It defines no mechanism of its own and it defines no gate; the mechanisms live in
the sibling documents and this document points at them. Its one normative rule is the tagging rule
the whole plan obeys (ADR §9 R42): every number is **MEASURED** (with its `EV-nnn` row in
`appendix-A-evidence-ledger.md`), **DERIVED from EV-nnn** (scaled, summed or interpolated from
measured rows — never MEASURED), **UNMEASURED** (with the gate that will measure it) or
**ASSUMPTION**, and nothing derived or interpolated is ever written as measured.

Reads with: `01-architecture.md` (the thesis and the contracts C1–C5), `03-execution-engine.md`
and `06-imaging.md` (the mechanisms most risks attach to), `09-performance-and-benchmarks.md` (§5,
the single gate registry, and §0.2, the only frame ledger — ADR §9 R40, R41), `11-roadmap.md`
(milestones M0–M8, exit criteria and the stop-condition register SC-1…SC-13),
`10-build-dependencies-testing.md` (test tiers T0–T4, the env-var registry, third-party build),
`appendix-A-evidence-ledger.md` (every measured number with its row id, report section and
file:line).

---

## 0. Decision log

Two ADR sections bind this log: `design/adr-v1.md` §1 (the eight amendments to the brief) and
`design/adr-v1.md` §9 (the addendum of 2026-09-05, rulings R1–R45). **Where ADR §1 and ADR §9
disagree, §9 wins** (ADR §9 preamble: "the rulings below (R1–R45) are binding; where an earlier
section of this ADR disagrees, the ruling wins. Every plan document must conform").

### 0.1 How this design was chosen

Two research rounds (`research/A1`–`A8`, `research/B-usdrig-build.md` and eleven `research/G-*`
reports with real builds and probes on this host) produced `design/brief-v1.md`: R1–R9, S1–S46 and
eight open areas D1–D8. Three proposals were scored by three judges (`design/judge-evidence.md` §1,
`design/judge-artist.md` §1.1, `design/judge-delivery.md` §1), who returned the same verdict:

| Element | Source | Scores (evidence / artist / delivery) |
|---|---|---|
| Base document, contracts, delivery shape, look and tools architecture | `design/proposal-risk.md` | 8.4 / 8.4 / 8.8 |
| Engine (`03-execution-engine.md`) and imaging (`06-imaging.md`) chapters | `design/proposal-performance.md` | 8.3 / 7.6 / 8.4 |
| Schema legibility, operator vocabulary, look block, tool UX | `design/proposal-artist.md` | 8.1 / 8.1 / 7.6 |

Grafted: from performance — the chunk/tile split, the reference lane, the three digests,
`UsdGenDirtyRouter`, the private `tbb::task_arena`, the eviction score, the gate matrix (ADR §4.1);
from artist — the reserved scopes and `__usdGenRender`, one prim type per operator concept with a
`usdGen:mode` token, non-structural `usdGen:enabled`, the ten-brush shelf, the stack profiler and
the freeze bar (ADR §2, §6). `design/adr-v1.md` is binding: where a proposal and the ADR disagree,
the ADR wins.

### 0.2 Settled decisions S1–S46

One line each. These are constraints: cite them, do not re-litigate them. Three markers appear in
the table: **Amended** = changed by `design/adr-v1.md` §1 or §9 and restated in §0.3/§0.4;
**Extended** = unchanged in substance but given wider scope by a later ADR section, cited inline;
**Ratified** = an amendment that fixes an encoding the brief left loose. Every number in this table
is MEASURED unless the row tags it otherwise. The Evidence column names the `appendix-A` ledger row
and the report section that contains it. Ledger rows are cited as **`EV-nnn`** and nothing else:
`EV-001`…`EV-083` is the only citation handle for a measured number (ADR §9 R1, R43), and
`appendix-A-evidence-ledger.md` §2.0 maps the retired per-subsection ids onto them.

| S | Decision | Evidence |
|---|---|---|
| S1 | Renderer-level `HdSceneIndexPlugin`, `loadWithRenderer ""`, C++ registration at insertion phase 0 / `InsertionOrderAtEnd` plus a plugInfo entry. **Amended:** the `after: ["hd:sceneGlobals"]` tag is decorative. | `research/G-chain-order-probe.md` §2–§3, §6 |
| S2 | One binary and one registration serve Storm, all four RenderMan display names and `usdrecord`. | `research/G-hdprman-and-usdrecord-render-time-chain.md` §1.2, §2.1 |
| S3 | Surfaces are read through a **private** `HdSiExtComputationPrimvarPruningSceneIndex`, never spliced into the shared chain. | `research/G-chain-order-probe.md` §4a–4b |
| S4 | Everything read is post-flattening: `xform/matrix` is world space with `resetXformStack=true`; dirtiness is per prim. | `research/G-chain-order-probe.md` §3; `research/A2-usdrig-imaging.md` §2 |
| S5 | Overriding a prim's `primvars` container requires dirtying the **bare** `primvars` locator too, or a UsdSkel prim freezes. **Extended** (ADR §9 R29) to every overlay the index makes on an upstream prim. | `research/G-chain-order-probe.md` §4c (MEASURED) |
| S6 | hdGp is not the vehicle: gated off, uncached, non-chainable, downstream of Storm's velocity motion. | `research/A4-openusd-hdgp-adapters.md` §7 |
| S7 | A metadata-only `UsdImagingSceneIndexPlugin` supplies `InstanceDataSourceNames()` and `ProxyPathTranslationDataSourceNames()`. | `research/A4-openusd-hdgp-adapters.md` §2 |
| S8 | No design element may require a `UsdStage` downstream of the stage scene index. **Extended** (ADR §9 R37) into a link allow-list gated by **B-1**. | `research/G-stage-free-parameter-and-time-transport.md` §0, §6 |
| S9 | All usdGen prim *classes* are codeless with abstract bases. **Amended** (ADR §9 R19): the schema *plugin* is a minimal library, `libusdGenSchema.so`. | `research/G-stage-free-parameter-and-time-transport.md` §1 |
| S10 | A prim adapter publishes every `usdGen:*` property via `UsdImagingDataSourceMapped`; without it they are invisible and emit **no notice**. **Extended** (ADR §2.3, §9 R6) to five plugInfo entries plus a `UsdGenRestAPI` adapter, with no prefix filter. | `research/G-stage-free-parameter-and-time-transport.md` §1, §5 (MEASURED; ledger `EV-081`–`EV-083`) |
| S11 | Ramps are knot arrays or a whole `TsSpline`; a `.spline` on a ramp parameter is forbidden. **Ratified** into scalar and colour encodings (ADR §1, restated without contradiction by §9 R11). | `research/G-stage-free-parameter-and-time-transport.md` §2.1 |
| S12 | The rest surface comes from an API-schema adapter sampling `UsdTimeCode::Default()`; capture-on-first-cook is rejected. | `research/G-stage-free-parameter-and-time-transport.md` §3 |
| S13 | Asset attributes resolve stage-free; overwritten map files are not auto-detected, so usdGen ships an explicit "reload maps". | `research/G-stage-free-parameter-and-time-transport.md` §5 |
| S14 | Dependency registration is lazy: pull every dependency once per topology generation, or declare `__dependencies`. | same §5 (mechanics open, Q-01) |
| S15 | State lives in a process-global `UsdGenImagingRegistry` keyed by weak stage / groom root; caches are never freed inside a notice. **Extended** (ADR §9 R30): `usdGen:sessionId` only when no stage is reachable. | `research/G-stage-free-parameter-and-time-transport.md` §7 |
| S16 | The ExecUsd control plane is not plugin-extensible in 26.08; usdGen ships its own evaluator and curve arrays never enter OpenExec. | same §6; `research/A6-openexec-vdf.md` |
| S17 | Never cook inside `GetPrim`; never cook on every `_PrimsDirtied` (2–2.9× over-cook). | `research/G-evaluation-scheduling-and-batching.md` §5 (ledger `EV-038`) |
| S18 | Deferred commit, atomic snapshot publish, diffed dirties. **Amended:** the `GetPrim` backstop is withdrawn (ADR §1); trigger (c) — an operator/map/surface-topology dirty — commits synchronously at the end of the `_PrimsDirtied` batch that carried it, **always**, with or without an app driver (ADR §9 R32). Trigger (a) covers `SetTime` and an explicit `Commit()` after live-override changes; trigger (b) is `currentFrame` when no (a) is attached. | same §4, §6, §10 |
| S19 | `GetPrim` only `atomic_load`s (ADR §9 R20: `std::atomic_load` over `std::shared_ptr<const UsdGenGeneration>`); notices leave only the serialized commit path. | `research/G-evaluation-scheduling-and-batching.md` §5 (16 734 reads, 0 torn, MEASURED; ledger `EV-039`); the rule itself is §7 |
| S20 | Progressive generation uses `asyncAllow`/`asyncPoll`, enabled by the plugin during `registerPlugins`. **Extended** (ADR §9 R38): v2 / M8. | same §8 |
| S21 | Bespoke TBB DAG, not a persistent `VdfNetwork`: 4× at 100 k, 9–15× at 1 M, 3–250× on sparse edits, half the RSS. | `research/G-data-plane-engine-prototype-benchmark.md` §0, §8 (ledger `EV-001`–`EV-006`, `EV-011`) |
| S22 | SoA internally, interleaved once at the Hydra boundary (25.3 → 80.9 GFLOP/s); no hand-written SIMD; `-ffp-contract=off`. | same §6 (ledger `EV-014`–`EV-018`) |
| S23 | Chunk = 512 curves, curve-aligned, one dirty byte per chunk per node. **Amended:** the Hydra prim is a *tile* of `chunksPerTile` chunks (ADR §1; arithmetic fixed by §9 R21). | same §4 (128 → 1.55 ms, 512 → 1.83, 1024 → 1.89, 4096 → 9.40; ledger `EV-009`) |
| S24 | Every node owns its output buffer; handoff is `VtArray` copy-on-write. **Amended** (ADR §9 R20): the optional publish ring is M7 and retires a buffer on its foreign-data-source detached callback; `VtArray::IsUnique()` does not exist and must not appear — see the §0.3 erratum. | same §3.2, §5 |
| S25 | Every operator splits into capture (epoch-cached) and evaluate (per-frame, per-chunk); operators are `restSpace` or `deformedSpace`. | same §8; `research/A7-prior-art-grooming.md` §8 |
| S26 | Explicit `usdGen:input` wiring, Kahn order with namespace order as tie-break, cycles are compile errors, read phases for surface reads. | same §8; `design/brief-v1.md` §2.4 |
| S27 | 32–256 `basisCurves` prims per description plus guides and optional instancers; the prim set is allocated once. **Amended:** the 32–256 prims are *tiles*, and `nTiles` carries a `min(nChunks, …)` term (ADR §1 S23/S27; §9 R21). | `research/G-storm-throughput-and-prim-granularity.md` §1.7, §1.13, §2 |
| S28 | Exact-size arrays; padding renders fallback red; density scrubbing keeps element counts fixed during the drag. | same §1.4, §1.6 |
| S29 | The curve contract: cubic/bspline/pinned, no `normals`, refineLevel 2, `minScreenSpaceWidths = 1.0`, mandatory primvars, `primOrigin`. **Amended:** `hairTangent` is a measured fork (S-8); `hairId` is a uniform **float**. | `research/G-storm-hair-look-prototype.md` §2.4–2.5; `research/G-storm-throughput-and-prim-granularity.md` §1.11 |
| S30 | Bare `primvars/points/primvarValue` where usdGen owns the prim; `ComputeDirtyLocators` only over an upstream prim; never co-dirty `displayColor` with `points`; `__dependencies` per **tile** (32–256 edges). **Amended** (ADR §1 S23/S27, §5.3): the brief's "per chunk" is the tile, one edge per tile. | `research/G-storm-throughput-and-prim-granularity.md` §1.5, §1.9 |
| S31 | MEASURED on the GB10 through EGL: 200 k curves × 8 CV at refineLevel 2 = 23.93 ms at 720p (one prim); deforming adds 2.46 ms; refineLevel 0 is slower than 1. **Amended:** gate S-9 decides the refineLevel-1 tier. | `research/G-storm-hair-look-prototype.md` §5 (ledger `EV-021`) |
| S32 | Motion profiles P0 single (default), P1 velocities (opt-in), P2 samples (2–16, constant CV count, only `points` blur). These three keep the letter `P` (ADR §9 R1). | `research/G-motion-blur-sampling-strategy.md` §4; `research/G-hdprman-and-usdrecord-render-time-chain.md` §3 |
| S33 | Real `instancer` prims for cards, archives, spheres and clump-instanced geometry, synthesized downstream with hand-authored `instancedBy`; Storm has no per-instance material. | `research/G-instancing-cards-archives-and-native-instances.md` §1–§2, §5, §7 |
| S34 | Hair on natively instanced scalps is generated once per propagated prototype path; prototype names are hashes — discover, never construct. | same §3–§4 |
| S35 | The v1 Storm look is the plugin glslfx surface shader, two files, two shader defs from a plugin `shaderDefs.usda`. **Amended** (ADR §9 R4): **three** glslfx files and the identifiers `UsdGenHairPreview`, `UsdGenHairPreviewTranslucent`, `UsdGenHairPreviewPrimvar`. | `research/G-storm-hair-look-prototype.md` §2, §7 |
| S36 | One `Material`, three terminals. **Amended:** the source says Storm resolves `outputs:glslfx:surface` first, so the override ships OFF behind `USDGEN_STORM_MATERIAL_OVERRIDE`, decided by L-1. | same §3–§4; `pxr/imaging/hdSt/renderDelegate.cpp:695-707` (the `materialRenderContexts` list is `:702-707`; ADR §9 R43's accepted range); `pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45` |
| S37 | All map, Ptex and SeExpr evaluation is CPU at capture time, baked to primvars; Storm textures only for UV scalp colour via `st`. | `research/A8-seexpr-ptex-libs.md` §1.6, §2.8 (ledger `EV-067`–`EV-070`) |
| S38 | Third-party set: wdas/SeExpr `main` @8f8c8f2 (interpreter only, `rand()` added), Ptex 2.4.3, nanoflann 1.12.1, SeExpr noise, Hio for image IO. | `research/A8-seexpr-ptex-libs.md` §1, §2, §3, §5 |
| S39 | Two Python surfaces: a ctypes C ABI for control, a pxr_boost module for arrays (`VtArray` crosses in O(1), 0.13 µs at 1 M CVs); BLAS threads pinned to 1. **Extended** (ADR §9 R31): `08-tools.md` §1.4 is the single source of the ABI. | `research/G-tool-loop-array-transport-and-cv-picking.md` §1.2, §1.4 (ledger `EV-055`, `EV-065`) |
| S40 | Comb/paint is a live override during the drag with one stage write at release; CV picking is CPU in C++ (166 µs at 100 k CVs); CV display is a synthesized `points` child prim. | same §2.4, §3 (ledger `EV-064`) |
| S41 | Undo uses `SubtreeSnapshot` (0.1–0.24 ms); undo of a live freeze is `SetActive(false)`; `RemovePrim` under OpenExec raises a spurious `Tf.ErrorException`. | `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.1–1.4 (ledger `EV-040`, `EV-044`, `EV-079`); `pxr/exec/esfUsd/stageData.cpp:361` (the brief and that report say `:360`; the predicate call `if (!UsdPrimDefaultPredicate(resyncedPrim))` is at `:361` in this checkout, `GetPrimAtPath` at `:351` — corrected here, not re-litigated) |
| S42 | The frozen-curve contract (`rest`, `skinprim`, `skinprimuv`, `usdGen:curveId`, `usdGen:frozen:epoch`); landing tiers `session`/`sublayer`/`payload`; never `.usda`, never baked motion samples. **Amended** (ADR §9 R12): `primvars:usdGen:curveId` is `uint64[]`, not uniform int, and comes from the pinned SplitMix64 `UsdGenHash64(seed, faceIndex, k, kSaltScatter)`. R1 also retires the "T1/T2/T3 landing tier" prose in favour of the three tokens. | same §2.1, §3 |
| S43 | usdview plugin conventions: `PluginContainer`, state built in `__init__`, lazy Qt imports, shared undo stack, the signal's frame, `UpdateViewport()` after every edit. | `research/A3-usdrig-tools.md` §7–§8 |
| S44 | Sibling CMake project against the unmodified install, optional `find_package(rigExec CONFIG)`, the double-`pxrConfig` guard, usdRig's generated-plugInfo pattern. | `research/B-usdrig-build.md` §2 (build), §7 (artifacts and env), §8 (the double-`pxrConfig` trap at `pxrConfig.cmake:58` and the `if (NOT TARGET usd)` guard) |
| S45 | Four test harness tiers including the EGL Storm harness with Xvfb + llvmpipe as its CPU-only fallback (the plan's T0–T4 naming is ADR §7/§9 R2's refinement, §5 below); usdRig builds here in 21.45 s (MEASURED); RigExec costs ~1.35 ms/frame (MEASURED), which leaves ~15 ms of a 16.6 ms budget (**DERIVED**). ADR §9 R41 records that 60 Hz at 100 k × 8 CV at refineLevel 2 is **not established** — see `09-performance-and-benchmarks.md` §0.2, the plan's only frame ledger. | `research/B-usdrig-build.md` §2 (21.45 s clean build, ledger `EV-076`); `research/G-chain-order-probe.md` §5 (RigExec 1.351–1.502 ms/frame, ledger `EV-035`); `research/G-storm-hair-look-prototype.md` §0 and `research/ENVIRONMENT.md` CORRECTIONS (the EGL and Xvfb harnesses) |
| S46 | Four bugs to file and contain, not fix: the OpenExec resync predicate, usdRig's missing bare-`primvars` dirty, fixture drift, `_env.sh` Linux gaps. **Correction:** the report's fifth claim — that `TfErrorMark` has no Python binding — is wrong (RK-08) and needs no filing. **Extended** (`02-schema.md` §2.20 rule 5): a fifth item to file upstream — `UsdImagingGeomSubsetAdapter` invalidates the **bare** `indices` / `type` locators (`pxr/usdImaging/usdImaging/geomSubsetAdapter.cpp:165-185`) while the data source it publishes lives under the `geomSubset` container (`pxr/imaging/hd/geomSubsetSchema.cpp:119-121`); usdGen's dirty router accepts both spellings so the fix, if it lands, changes nothing. | `research/G-chain-order-probe.md` §4c; `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4 |

### 0.3 Amendments to the brief (ADR §1, restated for the index)

| Ref | Amendment | Reason |
|---|---|---|
| S23/S27 | Chunk ≠ tile: engine chunk = 512 curves (dirty/parallel unit); the Hydra prim is a **tile** of `chunksPerTile` chunks. `chunksPerTile = max(1, ceil(nChunks / tileTarget))`; `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))` (ADR §9 R21 — the `min(nChunks, …)` term is what keeps a small description from being forced to 32 tiles). Worked: 100 k curves, chunk 512, `tileTarget = 64` → 196 chunks, 4 chunks/tile, **49 tiles** (MEASURED inputs, arithmetic DERIVED); 64 tiles is the 1 M figure. `usdGen:tileTarget` is not a node-digest term; a change routes as `UsdGenDirtyTopology` on the description. | 512-curve chunks at 1 M curves cannot also be ≤ 256 prims (all three judges); ADR §1's formula omitted the `min` term |
| S1 | The `ordering.after: ["hd:sceneGlobals"]` tag is decorative; placement is guaranteed by phase 0 / `InsertionOrderAtEnd`. Keep the tag, never rely on it. | `design/judge-evidence.md` §0; `pxr/usdImaging/usdImagingGL/engine.cpp:148-201` (ADR §9 R43's accepted range; `_Append` is `:148-166`, `_Register` with phase 0 / `InsertionOrderAtStart` is `:182-201`) |
| S11 | Scalar ramp = `float2[] <p>:knots` + `token <p>:interpolation`; colour ramp = `float[] <p>:positions` + `color3f[] <p>:colors` + interpolation; optional whole-`TsSpline` on `float <p>:spline`. | `TsSpline` holds no colour values |
| S18(c) | The `GetPrim` cook backstop is withdrawn; operator-parameter dirties commit synchronously on the notice thread. | `design/judge-delivery.md` §5.1 — a reader thread cannot emit notices; ADR §9 R32 then made (c) unconditional — exactly one cook per edit batch, asserted by SI-3 |
| S24 | `VtArray` copy-on-write stays the default handoff; the publish ring is an M7 optimisation guarded by the foreign-data-source detached callback (ADR §9 R20). | `design/judge-evidence.md` §2.3.3 — hdPrman retains arrays across a sync |
| S29 | `hairTangent` publication is a measured fork (gate S-8), not a mandate; `hairId` is a uniform **float** in [0,1). | `design/judge-evidence.md` §2.3.2 — the shipped glslfx declares `hairId` as `float` |
| S31 | LOD = curve decimation stands; the justification is corrected and the refineLevel switch cost goes to gate S-9. | all three judges |
| S36 | Source evidence says Storm resolves `outputs:glslfx:surface` first; the override ships default OFF behind `USDGEN_STORM_MATERIAL_OVERRIDE`, decided by gate L-1 in M1. | `design/judge-evidence.md` §0 |

**Erratum on ADR §1 (S24), ratified by ADR §9 R20.** `VtArray::IsUnique()` does not exist
(`_IsUnique()` is private, `pxr/base/vt/array.h:1023`, under the class-scope `private:` at `:984`,
beside `_DetachIfNotUnique` at `:1009`; the only public sharing query is
`bool IsIdentical(VtArray const&) const`, `:945`) and the name may not appear in the plan. R20 fixes
the mechanism: the generation itself is handed off with `std::atomic_store` / `std::atomic_load`
over a `std::shared_ptr<const UsdGenGeneration>`, and every published `VtArray` of the optional M7
publish ring wraps a `UsdGenGenerationBuffer : Vt_ArrayForeignDataSource` (`array.h:39-55`, built
via `VtArray(Vt_ArrayForeignDataSource*, T*, size_t)`, `:269`) whose detached callback returns the
buffer to the session pool. **A buffer is reusable only after that callback has fired** — the
callback, not a poll, is the retirement signal. An `IsExclusive()` read of the protected `_refCount`
(`array.h:53`) survives only as a debug assertion.

### 0.4 Addendum rulings (ADR §9, R1–R45)

The addendum of 2026-09-05 amends S1–S46 further, renames vocabulary, re-assigns gates and fixes
numbers. Rows are grouped where the ADR groups them; every ruling R1–R45 is covered.

| R | Ruling | What it supersedes |
|---|---|---|
| R1 | Engine principles become **I1–I8** ("invariants"); motion profiles keep S32's **P0/P1/P2**; M0 pre-work items are `PW-1…PW-n`; appendix-A rows are **`EV-001…`** and are the only citation handle for a measured number; `RK-`/`X-`/`Q-`/`A-` ids stay as this document defines them; freeze landing tiers are the tokens `session`/`sublayer`/`payload`. | `P1`–`P8` as engine-principle names; ledger row ids `Q1…Q41`; S42's "T1/T2/T3 landing tiers" |
| R2 | Tiers **T0–T4** as §5 states them. Gate families `E-`, `SI-`, `S-`, `L-`, `T-`, `T-INST-`, `R-` and new **`B-`** (build checks; `B-1` = the link-rule check). Gate ids are always hyphenated. `T-EXPR-1` and `T-PTEX-1` are `T-` family ids registered in `09-performance-and-benchmarks.md` §5.4, on the `T-INST-` pattern. | un-hyphenated gate ids (`E1`, `SI3`); tier names outside T0–T4; any gate id a document mints for itself instead of 09 §5 |
| R3 | Class names `UsdGenGroomSceneIndexPlugin`/`UsdGenGroomSceneIndex`, `UsdGenSession`, `UsdGenImagingRegistry`, `UsdGenTileBuilder`, `UsdGenPrimAdapterBase` + five one-line subclasses + `UsdGenRestAPIAdapter`; dirty bits are an unscoped enum. | `UsdGenImagingSession`, `ChunkPrimBuilder`, `UsdGenDirtyBits::Value` |
| R4–R5 | Shader defs `UsdGenHairPreview`, `UsdGenHairPreviewTranslucent`, `UsdGenHairPreviewPrimvar`; **three** glslfx files with an identical C5 `inputs:` block; the Python package is **`usdgen`**. | `usdGen:HairPreview` as an identifier; two glslfx files; `usdGenPy` |
| R6–R11 | Every property `usdGen:`-namespaced; **`02-schema.md` is the single normative property registry**; the §2.3 shorthand expands; fold-ins and drops listed in R8; `usdGen:space` = `auto`/`rest`/`deformed`; `usdGen:readPhase` default `final`; `UsdGenClump` has no `usdGen:mode`; ramp encodings restated without contradiction. | ADR §2.3's shorthand; `usdGen:cacheOutput`, `usdGen:output`, `usdGen:interactive:maxCurves` and the other dropped names |
| R12 | `curveId` is **`uint64[] primvars:usdGen:curveId`** from a pinned SplitMix64 hash; `hairId = UsdGenHash32(curveId, 0) / 2^32`; decimation uses `kSaltDensity ≠ 0` so the surviving set is never `{hairId < scale}`. | S42's `uniform int` curveId; ADR §2.3's unsalted `hash32(id)` |
| R13 | Density: keep iff `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32`, `keepFraction = clamp(scale_groom · scale_description, 0, 1)`; scales never re-scatter and never re-chunk; `UsdGenChunkDesc::liveCount` tracks survivors. | ADR §2.3's decimation formula (A-19) |
| R14 | `usdGen:enabled` is **never** a digest term; topology-preserving ops route `UsdGenDirtyParameter`, generators/`Resample`/`Length` route `UsdGenDirtyTopology` with no recompile; a muted generator keeps its capture. | ADR §2.3's structural/non-structural wording |
| R15–R16, R18 | `GeomSubset` reaches Hydra as a `geomSubset` prim with `indices`/`type` (`familyName` does not); `02`'s mask block is canonical for names and arithmetic; a stale `UsdGenFreeze` epoch warns and keeps rendering. | `usdGen:mask:map`; silent re-cook of a frozen chain |
| R17 | `usdGen:algorithmVersion` schema fallback is **0 = "latest kernel"**; look preservation is guaranteed only for tool-authored assets. | the implicit reading of A-08 that every asset pins a version |
| R19 | The `usdGenSchema` plugInfo's `LibraryPath` is **`libusdGenSchema.so`**, a minimal library (schema tokens + `UsdGeomRegisterComputeExtentFunction`, linking `usd`/`usdGeom` only); the classes stay codeless. | ADR §2.1's "compute-extent fn lives in `usdGenImaging`" (RK-29) |
| R20 | Handoff is `std::atomic_store`/`std::atomic_load`; the publish ring is **M7** and retires a buffer on its detached callback; **`VtArray::IsUnique()` does not exist and must not appear anywhere**. | ADR §1 S24's wording (X-18, Q-09, §0.3 erratum) |
| R21 | `chunksPerTile = max(1, ceil(nChunks / tileTarget))`; `nTiles = min(nChunks, clamp(ceil(nChunks / chunksPerTile), 32, 256))`; 100 k → 49 tiles; `usdGen:tileTarget` is not a node-digest term. | ADR §1 S23/S27's formula |
| R22–R25 | Commit order: route dirties → recompile iff the structural digest changed → reference lane → capture → hair chunks in the arena → tiles and extents → publish (`Capture()` runs outside the arena's no-allocation rule). `UsdGenGraphDesc` carries `curveSets` and `UsdGenSurfaceDesc::samples`; per-curve extra planes exist alongside per-CV; the dirty-router key is the **full** locator path below `usdGen/`. | "first two elements" router keying; `int[3]`/`float[3]` as USD type names |
| R26 | Reference-lane sizing: guides are **1–10 %** of hairs; design and budget for 10 %. | — (registered as A-27) |
| R27 | Every engine budget and gate quotes the **8-thread private-arena** number with the 20-thread number in parentheses. **E-1 ≤ 1.5 ms (MEASURED 1.02 ms).** Scaled or interpolated figures are `DERIVED from EV-nnn`, never MEASURED. | `design/proposal-performance.md` §11.2's ≤ 2.5 ms over a 1.72–1.91 ms 20-thread baseline (§2.1 item 2) |
| R28 | The session's current time is whichever `SetTime`/`currentFrame` arrived last from any attached index; two indices at different times are unsupported in v1; SI-6's traversal population is bounded at **≤ 5 ms on the 11 005-prim stage** (ASSUMPTION until measured). | — (registered as A-29) |
| R29–R31 | C3 source prims are overlaid `visibility = false` and the description always publishes tiles; the in-place overlay path is v2 behind gate **S-10 (T2, M8)**; session keys are namespaced by `renderInstanceId` + root-layer identifier; **`08-tools.md` §1.4 is the single source of the C ABI (contract C4)**, repeated verbatim in `06-imaging.md`. | `usdGen:output`; this document's earlier use of `S-10`, then `S-11`, for the tile-count sweep (it is **S-12**, `09-performance-and-benchmarks.md` §5.3) |
| R32 | Trigger **(c) always applies** — with or without an app driver; the app never calls `Commit()` for a stage edit; (a) is `SetTime` and explicit `Commit()`, (b) is `currentFrame` when no (a) is attached. | ADR §1 S18(c) / §4.3's "when no app driver is attached" |
| R33–R36 | `hairTangentWorld` is rejected (the render network transforms object-space `hairTangent`); material bindings are authored on **every** tile; usdGen sorts before hdPrman's renderer-specific phase-0 entries; `10-build-dependencies-testing.md` owns the **single env-var registry**; freeze is **Shift+F**, unfreeze Alt+F. | a context-dependent published primvar; inherited bindings; scattered env-var definitions; `F` as the freeze hotkey |
| R37 | `usdGen` core links `tf gf vt sdf ar work trace hio pxOsd` (+ what `sdf` pulls); `usd*`, `usdImaging*`, `hd*`, `glf`, `garch`, `hgi*`, `usdAppUtils` are forbidden; gate **B-1** (T0, M0) checks `DT_NEEDED`. | S8 as an untested convention (RK-30) |
| R38 | **v1 = the M0–M7 deliverable set**; **v2 = M8 breadth** (incl. progressive generation T-5 and the in-place overlay S-10); v3 unchanged. | ADR §6's v1/v2 split |
| R39 | Milestones are **serial** vertical slices; 33 weeks + 1 week integration float = **34 weeks** (ASSUMPTION, 3 engineers); **no T4 gate is a milestone exit** — T4 gates are release criteria **RC-n**; T-5 is a v2 criterion. | RK-21's earlier "R-2/R-3 / M7" |
| R40 | **`09-performance-and-benchmarks.md` §5 is the single gate registry** (id → metric, criterion, tier, milestone, status); `00`, `10`, `11`, `12` and appendix A cite it and must agree. The listed assignments are binding. | this document's earlier gate/milestone cells |
| R41 | **09 §0.2 is the only frame ledger.** 60 Hz at 100 k × 8 CV / refineLevel 2 is **not established**; interactive targets (60 Hz ≤ 40 k, ≥ 30 Hz at 100 k, ≥ 10 Hz at 200 k) are ASSUMPTION until S-1. | S45's "~15 ms for usdGen at 60 Hz" read as an established budget (A-28) |
| R42 | Four number tags: MEASURED (`EV-nnn`), **`DERIVED from EV-nnn`**, UNMEASURED (with the gate), ASSUMPTION. Every number carries one. | the three-tag rule this document previously stated |
| R43 | Appendix A rows are `EV-001…`; `hdSt/renderDelegate.cpp:695-707` and `engine.cpp:148-201` are the accepted ranges; the `mbbench.cpp` motion floors carry a "not carried in" note. | `:701-707`, `:150-156`, `09`'s `:698-710` |
| R44 | `00-request-and-scope.md`'s gate-family list, glossary and version table carry R1–R2's vocabulary, the `L-`/`B-`/`SI-6…SI-9`/`S-10` additions and R38's version split. | — |
| R45 | "see `NN-…md` §X" must point at a section that exists in the current sibling. | A-03's "`02-schema.md` §2"; §6's "`04-operators.md` §6" |

---

## 1. Rejected alternatives

Each row was proposed by a proposal, a judge or the brief's own draft, and rejected on evidence. Do
not re-propose these; if new evidence appears, amend this table first. Every number in this table is
MEASURED unless the row tags it otherwise; the Evidence column names the `appendix-A` ledger row
(`EV-nnn`, ADR §9 R1, R43) and the report section that contains it.

| # | Alternative | Why rejected | Evidence |
|---|---|---|---|
| X-01 | Generate hair from a second `UsdImagingSceneIndexPlugin` | UsdImaging plugin order is pointer-ordered and flips with `PXR_PLUGINPATH_NAME`; observed upstream of UsdSkel | S1; `research/G-chain-order-probe.md` §2 |
| X-02 | A usdRig registry hook (usdGen inserted by RigExec) | Couples usdGen's correctness to another plugin's presence and version | S1; `design/brief-v1.md` §2.1 |
| X-03 | hdGp procedural as the generation vehicle | Gated off, uncached `GetChildPrim`, non-chainable, downstream of Storm's velocity motion | S6; `research/A4-openusd-hdgp-adapters.md` §7 |
| X-04 | A persistent `VdfNetwork` / OpenExec as the data plane | 4–15× slower, 3–250× worse on sparse edits, 500-element grain ignores curve boundaries; the control plane is not plugin-extensible | S16, S21; `research/G-data-plane-engine-prototype-benchmark.md` §2, §8 |
| X-05 | Implicit preceding-sibling wiring when `usdGen:input` is unauthored | S26 settles wiring as explicit; there is no `PrimsChildrenReordered` notice and the invalidation is unmeasured | `design/judge-evidence.md` §2.2.2; `design/judge-artist.md` §3 (Artist 1); ADR §2.1 |
| X-06 | `usdGen:active` as the enable flag | Collides with USD prim `active`, which S41 makes load-bearing (undo of a live freeze is `SetActive(false)`) | `design/judge-artist.md` §3 (Risk 1); ADR §2.1 |
| X-07 | `usdGen:enabled` structural (bumps the structural digest) on topology-preserving operators | Drops the node's and every descendant's capture cache (10–150 ms, **ASSUMPTION** — no probe; ADR §9 R42, `appendix-A-evidence-ledger.md` §2.11) on the most frequent artist gesture; ADR §9 R14 removes `enabled` from every digest | `design/judge-evidence.md` §2.3.5; ADR §2.3, §9 R14 |
| X-08 | Read "interactive vs render" from a `sceneGlobals` flag | No such field: the schema carries nine tokens, none an interactive flag | `pxr/imaging/hd/sceneGlobalsSchema.h:37-46`; `design/judge-artist.md` §3 (Performance 1) |
| X-09 | Infer render intent from the renderer display name | `usdrecord --renderer GL` is Storm and is a render; an hdPrman viewport is interactive — wrong both ways | `design/judge-artist.md` §3 (Artist 2); ADR §2.3 |
| X-10 | "Widen chunks" as the cross-chunk fallback | MEASURED 5× cliff at 4096 curves/chunk (9.40 ms vs 1.83 ms at 512): unreachable | S23; `research/G-data-plane-engine-prototype-benchmark.md` §4 (ledger `EV-009`); `design/judge-artist.md` §3 (Risk 3) |
| X-11 | Padding `points` arrays so element counts stay fixed | A longer `points` array is not tolerated; the prim renders fallback red | S28; `research/G-storm-throughput-and-prim-granularity.md` §1.4 |
| X-12 | `UsdStage::RemovePrim` in interactive paths | Raises a spurious `Tf.ErrorException` with OpenExec attached; use `SetActive(false)` | S41; `pxr/exec/esfUsd/stageData.cpp:361` |
| X-13 | `.usda` for frozen/baked curve data | Text bakes are far larger and slower to load than `.usdc` for the same arrays | S42; `research/G-freeze-bake-undo-and-frozen-reentry.md` §3 |
| X-14 | Baking motion samples into a freeze | Author `velocities` instead; sample bakes multiply file size and break on a shutter change | S42; `research/G-freeze-bake-undo-and-frozen-reentry.md` §3 |
| X-15 | The `GetPrim` cook backstop (original S18(c)) | A `GetPrim` cook runs on reader threads and emits no notices; Storm re-pulls only what was dirtied | ADR §1 (S18(c)), §9 R32; `design/judge-delivery.md` §5.1 |
| X-16 | `hairId` as a `VtIntArray` primvar | The shipped glslfx declares it `float`; an int primvar does not bind and the shader falls back to 0.0 | ADR §1 (S29); `design/judge-evidence.md` §2.3.2 |
| X-17 | Cooking inside `GetPrim`, or cooking on every `_PrimsDirtied` | Lazy pull cooks on worker threads with no notices; per-notice cooking over-cooks 2–2.9× | S17; `research/G-evaluation-scheduling-and-batching.md` §5 (ledger `EV-038`) |
| X-18 | Unconditional A/B double-buffered publish replacing copy-on-write | Only sound if no consumer holds the back buffer; hdPrman retains arrays across a sync. `VtArray` exposes no public use count, so retirement is the foreign-data-source detached callback, not a poll (§0.3 erratum; ADR §9 R20) | ADR §1 (S24), §9 R20; `design/judge-evidence.md` §2.3.3; `pxr/base/vt/array.h:39-55, 269, 945, 1023` |
| X-19 | One Hydra prim per groom, or 10⁴+ prims | One prim: no culling, no parallel resolve, whole-groom re-upload. 10⁴+: fan-out and batch validation | S27; `research/G-storm-throughput-and-prim-granularity.md` §1.13, §2 |
| X-20 | A plain `.spline` value on a ramp parameter | A per-frame dirty and recook; ramps are knot arrays or a whole `TsSpline` through the adapter | S11; ADR §9 R11; `research/G-stage-free-parameter-and-time-transport.md` §2.1 |
| X-21 | Capturing the rest surface on first cook | Frame-order dependent; use an API-schema adapter sampling `UsdTimeCode::Default()` | S12; `research/G-stage-free-parameter-and-time-transport.md` §3 |
| X-22 | Four `Scatter*` prim types (one per mode) | A mode switch would be a delete/create resync (X-12); use one type with a `usdGen:mode` token | ADR §2.1; `design/judge-artist.md` §4 |
| X-23 | Registering the prim adapter only on `UsdGenOperator` (+`UsdGenMap`) | Groom/Description/GuideSet derive from `UsdGeomImageable`, so half the schema reaches Hydra not at all | S10; `design/judge-evidence.md` §2.1 |
| X-24 | Chunk fan-in / a serial cross-chunk pre-pass for guides and clumps | Retains an unmeasured fan-in and a serial phase in the critical path; the reference lane removes the question | ADR §4.1 (I3 — the reference lane), §4.2.5; `design/judge-delivery.md` §6 |
| X-25 | `Vt.*Array.FromBuffer` / `attr.Set(ndarray)` on groom-sized arrays | 4.3 ms per call versus 0.13 µs for a `VtArray` crossing | S39; `research/G-tool-loop-array-transport-and-cv-picking.md` §1.2 (ledger `EV-055`, `EV-057`) |
| X-26 | Naming the milestones S0–S8 | "S1" would name both a milestone and a settled decision; milestones are M0–M8 | `design/judge-evidence.md` §2.4.5; ADR §7 |
| X-27 | `const` on `InstanceDataSourceNames()` / `ProxyPathTranslationDataSourceNames()` overrides | Both base virtuals are non-const; `const override` is a compile error | `pxr/usdImaging/usdImaging/sceneIndexPlugin.h:82-83, 91-92` |
| X-28 | A third-party operator ABI in v1/v2 | `UsdGenOpRegistry` stays internal; engine headers are not installed, so they may churn until M7 | ADR §3 |

---

## 2. Risk register

Likelihood and impact are H/M/L, plus **certain** for a defect already reproduced on this host.
"Retired by" is the gate, tier, contract freeze or milestone that retires or contains the risk. Gate
ids, tiers, thresholds and milestones all come from `09-performance-and-benchmarks.md` §5, the single
gate registry (ADR §9 R40); this column reproduces it and may not disagree with it. Every id in the
column is registered there today: the tile-count sweep is **S-12** (09 §5.3 — `S-11` is the `head1M`
static-draw record at M7), session arbitration is **SI-10** (09 §5.2 — two attached indices agree on
generation, prim set and frame; **SI-9** is the pruning-wrapper cost alone and **SI-11** the
`reorder nameChildren` record), the SeExpr sandbox and the Ptex face-id mapping are **T-EXPR-1** and
**T-PTEX-1** (09 §5.4, both
filed from RK-15 and RK-16), and the link rule is **B-1** (09 §5.1). This document does not define
gates; 09 does. Every number in a Mitigation or Early-signal cell carries its own MEASURED / DERIVED /
UNMEASURED / ASSUMPTION tag.

| # | Risk | L | I | Early signal | Mitigation (with evidence) | Retired by |
|---|---|:-:|:-:|---|---|---|
| RK-01 | Adapter-published `usdGen:*` invalidation is imprecise: a parameter edit resyncs the operator prim instead of dirtying one mapped locator, so every edit becomes a recompile | M | **H** | SI-7 finds a property that appears but does not dirty; SI-2's locator set is wider than asserted | `UsdImagingDataSourceMapped::Invalidate` per property; one T1 test per operator asserting both value and notice. **SC-1**: fall back to `primvars:usdGen:*` and re-plan `02-schema.md` (`research/G-stage-free-parameter-and-time-transport.md` §1, §5; S10; `11-roadmap.md` §5.1) | SI-2, SI-7 / **M1** |
| RK-02 | Auto-applied `UsdGenMaskAPI` does not reach a codeless derived type, so the mask block leaves `UsdPrimDefinition::GetPropertyNames()` | M | M | SI-8 red in M0 pre-work | The registry propagates auto-apply to derived types (`pxr/usd/usd/schemaRegistry.cpp:922-940`); until SI-8 is green the tool also applies the schema explicitly | SI-8 / **M0 pre-work, binding at M1** |
| RK-03 | Storm does not resolve `outputs:glslfx:surface` ahead of `outputs:surface`, so the hair shader never binds | L | M | L-1 red | Flip `USDGEN_STORM_MATERIAL_OVERRIDE` to ON, synthesize `<Description>/__usdGenRender/material_storm` with `primOrigin`, bind tiles to it (SC-3) | L-1 / **M0 pre-work, binding at M1** |
| RK-04 | The synthesized Storm material does not survive Storm's material-binding-resolving index, which runs after usdGen (S1) | L | M | L-1's binding assertion at the terminal SI | The material is a real prim in usdGen's own output, never an override on an upstream prim (ADR §5.6); bindings are authored on every tile (ADR §9 R34) | L-1 / **M0 pre-work, binding at M1** |
| RK-05 | Variant A (tangent from `inData.Neye`) fails to compile or looks wrong, so `hairTangent` is republished per deforming frame and the `DirtyPoints` fastpath is lost | M | M | S-8 A/B frame time on a 100 k deforming groom | Ship variant B (`UsdGenHairPreviewPrimvar`, ADR §9 R4) as the fallback default; its per-frame cost is **DERIVED** (≈ +1.5–2.5 ms at 100 k × 8 CV, ADR §5.4 / §9 R42); S-8 measures it (SC-2) | S-8 / **M0 pre-work, binding at M1** |
| RK-06 | A refineLevel switch costs more than it saves, so the tumble tier is not viable | M | L | S-9; the cubic index rebuild is MEASURED at 2.6 ms warm / 5.2 ms cold per 500 000 patches (`research/G-storm-throughput-and-prim-granularity.md` §1.6) | Tiles stay pinned at refineLevel 2; stable-id decimation stays the only interaction lever (ibid. §1.6, §1.10; ADR §5.5; SC-4) | S-9 / **M0 pre-work, binding at M1** |
| RK-07 | Multi-prim Storm **GPU** cost is unmeasured. The CPU side is MEASURED (`research/G-storm-throughput-and-prim-granularity.md` §1.7: cubic index build 2.607 → 0.659 → 0.572 ms at 1 / 32 / 128 prims; §1.8: per-frame notice and dependency fan-out ~0.01 / ~0.4 / ~11 / ~210 ms at 32 / 1 000 / 10 000 / 100 000 prims), but every *frame-time* number came from one draw item (`research/G-storm-hair-look-prototype.md` §5, ledger `EV-019`–`EV-023`), so draw-batch behaviour and per-tile culling at 32–256 tiles are unproven — yet C2 freezes the tile default at M1 | M | M | S-12: a 1 / 32 / 128 / 196-tile EGL sweep before C2 freezes | `usdGen:tileTarget` is authored (ADR §2.3), so a wrong default is a re-tune, not a schema change; S-12's tile counts and its threshold are `09-performance-and-benchmarks.md` §5.3's (UNMEASURED; S-12 produces the number), and S-5 asserts the batch count at the fixed default (ibid.) | S-12, S-5 / **M1, before C2 freezes** |
| RK-08 | The `esfUsd/stageData.cpp:361` resync predicate raises a spurious `Tf.ErrorException` for **any edit that leaves no prim at the resynced path**: `RemovePrim`, `del nameChildren[…]`, and — load-bearing for the S42 sublayer tier — **muting or removing the sublayer that holds a freeze**. Authoring a freeze, `SetActive(False)`, and remove-then-re-`CopySpec` in one `Sdf.ChangeBlock` are clean | **certain** | L | Any sublayer mute or prim removal with an OpenExec system attached | Never `RemovePrim` interactively (X-12); `SetActive(false)`; a sublayer-tier freeze is toggled by `SetActive(false)` on its prims, never by muting the sublayer. Containment uses `TfErrorMark` in C++ and, in Python, **`Tf.Error.Mark`** — bound at `pxr/base/tf/wrapError.cpp:212-221` inside the `Tf.Error` scope opened at `:200` (the name `Tf.ErrorMark` does not exist, which is what `prototypes/freeze-bake/probe9_error_containment.py:27` tested; `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4 and S41/S46 are corrected here, not re-litigated). The tool wraps the edit as `m = Tf.Error.Mark(); m.SetMark(); …; errs = m.GetErrors(); m.Clear()`, filters the `stageData.cpp:361` predicate error by message, re-raises anything else, and asserts the post-condition that the prim is gone. `pxr/usdImaging/bin/testusdview/testusdview.py:144` uses the same class, so the harness pattern is already in the tree; file upstream (S46) | T-4 / **M5** |
| RK-09 | Two scene index instances disagree on frame or context and the fixed prim set of S27/S28 changes under one of them | M | **H** | Two attached indices report different generations for one frame | ADR §4.5 and §9 R28: the session owns one frame, one context, one generation, and every attached index republishes it; two indices at different times are unsupported in v1; `usdGen:tileTarget` is authored so the prim set is reproducible. SI-6 tests initial population only and cannot retire this, so the assertion is its own gate, **SI-10** (`09-performance-and-benchmarks.md` §5.2, `testUsdGenSessions`; `design/judge-delivery.md` §5.3) | SI-10 / **M2** |
| RK-10 | A host hands usdGen a populated input with no `PrimsAdded` replay, so the groom is never discovered — or the covering traversal is too expensive | M | M | SI-6 fails on the legacy `HdRenderIndex::New` path | ADR §4.4: `HdMergingSceneIndex::InsertInputScenes` replays `PrimsAdded` for the usdview/usdrecord path (`pxr/imaging/hd/mergingSceneIndex.cpp:173-245`; the replay is `_SendPrimsAdded(addedEntries)` at `:244` and is **skipped entirely** when `_IsObserved()` is false, `:211-213`, so the covering traversal must also cover an unobserved insert); otherwise one bounded traversal on first access when `_populated == false`, scanning only for `UsdGenGroom` roots. Its cost is bounded at ≤ 5 ms on the 11 005-prim fixture (ASSUMPTION, ADR §9 R28, A-29); SI-6 measures it (`design/judge-delivery.md` §5.2) | SI-6 / **M1** |
| RK-11 | kNN capture dominates: the reference lane assumes an affordable nanoflann build and query over 100 k / 1 M rest roots on 8 threads, extrapolated from the library's benchmarks | M | **H** | E-4 threshold ≤ 25 ms at 100 k roots / 4 k guides and linear in roots (UNMEASURED; E-4 measures it — `09-performance-and-benchmarks.md` §5.1) | If E-4 fails, change the operator schedule before the catalogue is committed: cap guides per description (guides are 1–10 % of hairs, A-27), make capture incremental per surface, or put `GuideInterpolate` behind a progressive publish (SC-6; `design/judge-delivery.md` §5.6) | E-4 / **M0 pre-work, binding at M3** |
| RK-12 | The ragged (mixed CV count) path is much slower than the uniform path, and `UsdGenCurveSource` is v1 and ragged by nature | M | M | E-1r threshold: ragged ≤ 2× uniform (UNMEASURED; E-1r measures it) | `UsdGenResample` ships in v1; the import tool offers "resample to N" with the measured penalty shown, so the cost is never silent (ADR §4.2.2; SC-5) | E-1r / **M2** |
| RK-13 | Per-node caching costs a MEASURED 668 MB at 1 M curves × 8 CV with 5 nodes (`research/G-data-plane-engine-prototype-benchmark.md` §5, ledger `EV-007`) | M | M | E-5 threshold RSS ≤ 800 MB (UNMEASURED gate threshold over a MEASURED 668 MB baseline, `09-performance-and-benchmarks.md` §5.1) | Eviction scored by `recomputeCostMs/bytes` (never the tail base or a live-override target), `USDGEN_MEMORY_BUDGET_MB`, per-node opt-out (single buffer MEASURED at 293 MB, ledger `EV-007`; ADR §4.1). **SC-7a/SC-7b** | E-5 / **M3, re-run M7** |
| RK-14 | hdPrman parity is source-derived only: no RenderMan install exists on this host | M | M | R-1 (tier T4) on a workstation | Ordering is unit-testable with synthetic `hdPrman:*` tags (SI-5, tier T1); usdGen sorts before hdPrman's renderer-specific phase-0 entries (ADR §9 R34); the sampled-points contract follows `research/G-hdprman-and-usdrecord-render-time-chain.md` §3; if hdPrman rejects the tile contract, emit `nonperiodic` curves with duplicated end CVs behind a render flag (SC-8) | R-1 / **release criterion (RC-n), built at M7** |
| RK-15 | SeExpr sandboxing: expression strings are asset data and `ExprFunc::init()` loads plugins from a colon-delimited `SE_EXPR_PLUGINS` variable | M | **H** | Any `.so` load during capture; an expression naming an unknown function | Interpreter only; clear `SE_EXPR_PLUGINS` before `init()`; a closed function set (`map()`, `ptex()`, `rand()` plus builtins, S38) with no file, network or process primitives; capture-time only (S37) under a time budget; a parse error is a diagnostic and a fallback value, never an exception into Hydra (`research/A8-seexpr-ptex-libs.md` §1.3, `ExprFunc.h:49-50`, `ExprFunc.cpp:158-159`; §7 licensing) | **T-EXPR-1** (T0) / **M4** |
| RK-16 | Ptex face ids do not line up: `Far::PtexIndices` maps an n-gon to n sub-faces while Storm triangulates as a fan, and a `GeomSubset` adds a second index space | M | M | A painted map correct on quads and wrong on n-gons or triangles | `Far::PtexIndices` over a `PxOsdRefinerFactory::Create` refiner is canonical; `mt_triangle` only for all-triangle meshes; `skinprim` and Ptex ids always index the **parent mesh's** faces (ADR §2.3, §9 R15); a subset edit is a recapture (`research/A8-seexpr-ptex-libs.md` §2.6; `pxr/imaging/pxOsd/refinerFactory.h:34-41`; `design/judge-delivery.md` §5.4) | **T-PTEX-1** (T0) / **M4** |
| RK-17 | Undo edge cases: a freeze undone outside its authoring layer, a sculpt layer stale after a seed or density change, a snapshot restored over a recomposed prim | M | M | A restore leaves residue; a stale badge that never clears | A freeze is undoable only in its authoring layer (S41); structural edits in one `Sdf.ChangeBlock` with `Define` outside it; `usdGen:frozen:epoch` staleness gives a badge plus one undoable "Rebase sculpt" by nearest root UV; deltas for missing ids are kept (`research/G-freeze-bake-undo-and-frozen-reentry.md` §1.1, §1.5, §2.1; ADR §2.3, §9 R18) | T-4 / **M5** |
| RK-18 | Density scrubbing forces a whole-VBO reallocation and invalidates every batch drawing the groom | **H** | M | S-7 threshold: a parked-CV drag ≤ 1.5× a points-only frame (UNMEASURED gate threshold; `09-performance-and-benchmarks.md` §5.3) | Degenerate CVs at zero width on max-density topology during the drag; the real count change commits on release; pre-baked density levels as fallback. Root cause: any element-count change calls `SetNeedsReallocation()` and rebuilds the whole striped buffer array (`research/G-storm-throughput-and-prim-granularity.md` §1.6; `pxr/imaging/hdSt/vboMemoryManager.cpp:605-620`) | S-7 / **M5** |
| RK-19 | Thread scaling regresses past ~8–10 threads on heterogeneous cores (MEASURED **certain on this host**: 10× Cortex-X925 + 10× Cortex-A725, both engines regress past ~8–10 threads, `research/G-data-plane-engine-prototype-benchmark.md` §0 host block, §3.3, ledger `EV-008`; UNMEASURED on x86-64, where RK-20 carries the same question) | **certain** | L | E-7 stated as a ratio, never an absolute: ≥ 3× from 1 → 8 threads (UNMEASURED gate threshold over a MEASURED 3.8× baseline, `09-performance-and-benchmarks.md` §5.1) | Private `tbb::task_arena` at the measured knee, `USDGEN_THREAD_LIMIT`, one-shot calibration at first commit (ADR §4.1 I8) | E-7 / **M1** |
| RK-20 | Every engine number is aarch64 + GCC 13 autovectorisation; x86-64 may not reproduce the SoA win or bitwise determinism | M | M | E-8 or E-1 red on the first x86-64 runner | No hand-written SIMD (S22); `-ffp-contract=off` on `usdGenMath`; deterministic reductions; thresholds re-derived per machine before they are enforced (`research/G-data-plane-engine-prototype-benchmark.md` §0 host block, §6) | E-1 / **M1**, E-8 / **M1, re-run M4** |
| RK-21 | Every GPU number came from one NVIDIA GB10 through EGL; studio GPUs and drivers differ and no Metal/Vulkan Hgi is built here | M | M | R-2 (MSAA/OIT quality), R-3 (`HDST_ENABLE_HGI_RESOURCE_GENERATION=1` compiles clean on real hardware). **S-8 already compiles the shipping glslfx under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` at M1 (ADR §5.4); R-3 only re-runs it on Metal/Vulkan hardware** | Tier T4 protocol per `research/G-storm-hair-look-prototype.md` §6; the shader avoids `inData` and keeps the derivative-tangent fallback compiled in. Schedule: S-8 at M1 (proxy compile) → written scope decision at **M5** (SC-9) → R-3 on hardware at release | S-8 at **M1**; R-2/R-3 are **release criteria (RC-n), never milestone exits** (ADR §9 R39) |
| RK-22 | usdRig API drift: `rigExec::rigExecMath`, its scene index or its fixtures change under usdGen | L | L | The `USDGEN_WITH_RIGEXEC=OFF` build or the interop test breaks | usdRig stays optional; the OFF build must pass every non-rig test; if `rigExecMath` diverges, copy the four kernels usdGen uses into `usdGenMath` (`research/B-usdrig-build.md` §2, §7, §8; S44) | CI build matrix (both `USDGEN_WITH_RIGEXEC` settings), a `10-build-dependencies-testing.md` job, not a gate id / **M0** |
| RK-23 | An OpenUSD upgrade changes `UsdImagingDataSourceMapped`, pinned-curve expansion, the primvar-descriptor-cache narrowing or the pruning index | M | M | The T1 suite fails on a new install | One T1 test per version-sensitive mechanism, named after it; pin the supported USD version in `usdGenConfig.cmake` and gate CI on it (`research/G-storm-throughput-and-prim-granularity.md` §1.9; `research/G-stage-free-parameter-and-time-transport.md` §1) | full T1 suite / **M7** |
| RK-24 | Third-party ABI and licensing: a site's own Ptex or SeExpr collides with usdGen's copy, or NOTICE omits the two Disney trademark clauses | L | M | A duplicate-symbol crash under a Ptex-enabled `libusd_hdSt.so` | All components permissive (SeExpr Apache-2.0 with §6 replaced, Ptex BSD-3-Clause, nanoflann BSD-2-Clause); KSeExpr (GPL-3.0-or-later) excluded; static + `-fvisibility=hidden`; never install third-party headers or `.so`s beside USD (`research/A8-seexpr-ptex-libs.md` §7 licensing, §6.2) | CI build matrix + the NOTICE check in `10-build-dependencies-testing.md`, not a gate id / **M0** |
| RK-25 | The pxr_boost module is pinned to the exact USD build: mangled names carry `pxrInternal_v0_26_8__pxrReserved__` (`#define PXR_INTERNAL_NS`, `pxr/pxr.h:23`) | **certain** | L | Import error in the tool | Control stays on the ctypes C ABI (`08-tools.md` §1.4, ADR §9 R31); the module is optional and its absence degrades tools, never correctness (`research/G-tool-loop-array-transport-and-cv-picking.md` §1.5; S39) | C4 freeze (a contract, not a gate) / **M5** |
| RK-26 | Progressive generation may not be enablable from a usdview plugin. `_allowAsync` is private and reachable only through usdview's mangled API handle (`usdviewApi._UsdviewApi__appController`, `research/A3-usdrig-tools.md` §3.1). Ordering is **not** the problem: `_configurePlugins()` runs at `pxr/usdImaging/usdviewq/appController.py:432` — before both the `_allowAsync` read at `:523` and the timer creation at `:524-527` — so a write during `registerPlugins` (`pxr/usdImaging/usdviewq/plugin.py:333`) is honoured. The residual risk is the private-name dependency and the fact that the public `StageView.allowAsync` property (`pxr/usdImaging/usdviewq/stageView.py:828-833`) is **not** an equivalent fallback: it only feeds `params.allowAsynchronousSceneProcessing` (`stageView.py:966`) and never creates usdview's 100 ms `_asyncTimer`, so no `asyncPoll` runs (`stageView.py:2438-2445`) | M | L | T-5 | `--allow-async` stays the documented fallback and is the only reliable way to get the poll; nothing else depends on progressive generation (v2, M8, ADR §9 R38) | T-5 / **M8** |
| RK-27 | Staffing: every calendar figure assumes 3 engineers plus a part-time TD | **H** | **H** | Any deviation from the assumed team at M0 | Estimates state their staffing assumption and carry 30 % contingency on M1/M3/M7 (3 engineers, 30 % and the 34-week total are an ASSUMPTION, A-01, A-30; ADR §7, §9 R39); milestones are serial vertical slices, so a smaller team ships fewer of them rather than a broken one | **M0** re-baseline (a milestone, not a gate) |
| RK-28 | Scope creep into simulation under deadline pressure | **H** | **H** | Any request for a solver | Simulation is out of scope for v1 and v2; usdGen consumes a sim cache through `UsdGenCurveSource` and never produces one; `Collide`/`Wind` are v3 (ADR §6, §9 R38; SC-10) | ADR §6 scope statement, re-asserted at every milestone exit / **M8** |
| RK-29 | The `UsdGenDescription` compute-extent function never loads, so `UsdGeomBoundable::ComputeExtentFromPlugins` silently falls back and tiles get no bounds | M | M | A T1 test asserting `UsdGeomBoundable::ComputeExtentFromPlugins` returns true for a `UsdGenDescription` | ADR §9 R19: the `usdGenSchema` plugInfo is a **library** plugin whose `LibraryPath` is `libusdGenSchema.so` — a minimal library holding the schema tokens and one `UsdGeomRegisterComputeExtentFunction(TfType::FindByName("UsdGenDescription"), fn)` call, linking `usd`/`usdGeom` only; the schema classes stay codeless. `ComputeExtentFromPlugins` loads `PlugRegistry::GetPluginForType(type)` (`pxr/usd/usdGeom/boundableComputeExtent.cpp:166-173`), i.e. the plugin that **declares** the type, so the function may not live in `usdGenImaging`, which `usdcat` never loads. The generated schema plugInfo carries `customData = { dictionary extraPlugInfo = { bool implementsComputeExtent = true } }` on `UsdGenDescription` only (the usdLux pattern, `pxr/usd/usdLux/schema.usda:1126-1131`, `pxr/usd/usdLux/plugInfo.json:28`), which is what `_LoadPluginForType`'s early-out at `:159-163` checks. Extents then work in `usdcat` and `usdrecord` with no imaging library loaded. `02-schema.md` §7.1–§7.2 and `10-build-dependencies-testing.md` §4.3 implement R19: both route the schema `LibraryPath` at `libusdGenSchema.so`. | SI-7 / **M1** |
| RK-30 | The S8 link rule erodes: a convenience include pulls `usd`/`hd` into `libusdGen.so`, and the stage-free guarantee becomes untestable at tier T0 | M | **H** | `DT_NEEDED` of `libusdGen.so` names a forbidden library; a T0 test stops linking | ADR §9 R37's allow-list (`tf gf vt sdf ar work trace hio pxOsd` plus what `sdf` pulls) enforced by gate **B-1** at every build, not only at M0; `UsdGenGraphDesc` keeps the engine input a pure value type (ADR §4.2.3) | **B-1** / **M0** |

### 2.1 Stop conditions

Four conditions stop or redirect the work rather than merely being mitigated. `11-roadmap.md` §5 is
the stop-condition register (SC-1…SC-13); all four below are reproduced from it and may not disagree
with it. SC-11 and SC-12 were minted there at this document's request; SC-13, the UsdSkel-pickup
fallback, was minted for `05-static-curves-and-deformation.md` §4.2 and rides on SI-9.

1. **SC-1 — M1 invalidation (RK-01).** If precise `usdGen:*` invalidation cannot be demonstrated
   through the adapter, fall back to `primvars:usdGen:*` and re-plan. This is the ADR's own stop
   condition (ADR §7; `11-roadmap.md` §5.1) and it invalidates `02-schema.md` if it fires.
2. **SC-11 — M1 engine floor (E-1, E-7).** If gate
   E-1 cannot reach **≤ 1.5 ms** for a 5-operator, 100 k × 8 CV chain in the private 8-thread arena
   (MEASURED baseline **1.02 ms** at 8 threads, 1.79 ms at 20;
   `research/G-data-plane-engine-prototype-benchmark.md` §3.3, ledger
   `EV-008`; ADR §9 R27), stop and re-examine chunking before any operator is added; every later
   milestone assumes that floor. The 2.5 ms figure inherited from `design/proposal-performance.md`
   §11.2 is superseded by R27 and survives, if at all, only as a regression ceiling.
3. **SC-12 — chain order (S1, gate SI-5 at M1).** If
   gate SI-5 ever fails after an OpenUSD upgrade, nothing ships until it passes: the whole design
   assumes usdGen runs after every deformer.
4. **SC-7a / SC-7b — memory (RK-13).** If gate E-5 is red (RSS > 800 MB at 1 M curves × 8 CV with
   5 nodes) the budget and the eviction policy are re-tuned — `11-roadmap.md` §5.2's **SC-7a**. If
   RSS exceeds **1.5 GB** (ASSUMPTION threshold, ibid. **SC-7b**) at 1 M curves with the default
   chain, per-node caching becomes opt-in above 500 k curves. The two are different events: E-5's
   threshold is 800 MB, not 1.5 GB.

---

## 3. Open questions

Questions the ADR has **not** closed. Each names the gate or milestone that closes it, the default in
force meanwhile, and who decides. Role names (`ENG-1` engine/kernels, `ENG-2` imaging, `ENG-3`
tools/Python) come from A-01. These are staffing roles, not gate or measurement ids (`E-1` is a gate,
`EV-008` an appendix-A measurement handle, `appendix-A-evidence-ledger.md` §2.1).

Every gate id used below and in §2 is defined by `09-performance-and-benchmarks.md` §5, the single
gate registry (ADR §9 R40); this document defines none of them. Four that this document originally
asked for now live there: **S-12** (09 §5.3 — T2, the 1 / 32 / 128 / 196-tile sweep at 100 k × 8 CV
that neither S-1, one configuration with no prim count, nor S-5, one fixed tile count, performs),
**SI-10** (09 §5.2 — T1, two scene index instances attached to one session report the same generation,
prim set and frame for one commit; SI-6 tests initial population only, and the pruning-wrapper cost is
**SI-9**), and **T-EXPR-1** / **T-PTEX-1** (09 §5.4 — T0 at M4, the SeExpr sandbox and the Ptex face-id
mapping, both filed from RK-15 and RK-16; the mechanisms they assert are
`07-look-maps-expressions.md` §7.6, sandboxing, and §6.3, face ids).

| # | Question | Closed by | Default until then | Decides |
|---|---|---|---|---|
| Q-01 | S14 mechanics: what makes a parameter that the current mode does not read still invalidate — a `Bind()` that pulls every mapped locator, or `__dependencies` on operator prims? | SI-7 (extended to unread parameters) / **M1** | `Bind()` pulls every mapped locator of the node, regardless of mode | ENG-2 |
| Q-02 | What does Storm's *GPU frame time and batch behaviour* cost at 32 / 128 / 196 tiles versus one prim, and is `usdGen:tileTarget = 64` the right default? CPU-side chunking is MEASURED (`research/G-storm-throughput-and-prim-granularity.md` §1.7); the GPU side is not. | **S-12** (1 / 32 / 128 / 196-tile EGL sweep at 100 k × 8 CV, frame time and `drawBatches`; threshold in `09-performance-and-benchmarks.md` §5.3) / **M1**, before C2 freezes | `uniform int usdGen:tileTarget = 64`, clamped 32–256 | ENG-2 |
| Q-03 | Does Storm prefer `outputs:glslfx:surface`, and does the binding survive the material-binding-resolving index that runs after usdGen? | L-1 / **M0 pre-work, binding at M1** | `USDGEN_STORM_MATERIAL_OVERRIDE` default OFF; three terminals on one `Material` | ENG-2 |
| Q-04 | Which `hairTangent` variant ships as the default on usdGen tiles? | S-8 / **M0 pre-work, binding at M1** | Variant A (tangent from `inData.Neye`, no `hairTangent` primvar published) | ENG-2 with the TD |
| Q-05 | Does a refineLevel-1 "tumble tier" ship, given the switch cost? | S-9 / **M0 pre-work, binding at M1** | Tiles pin `displayStyle/refineLevel = 2`; LOD is stable-id decimation | ENG-2 with the TD |
| Q-06 | Does `reorder nameChildren` produce usable Hydra invalidation through the adapter? | **SI-11** (09 §5.2, record only), run as the M0 pre-work check **PW-6** (`11-roadmap.md` §1.1; it closes the question, it unblocks nothing) | Nothing evaluated depends on namespace order beyond the Kahn tie-break; the tool always authors `usdGen:input` | ENG-2 |
| Q-07 | Does an auto-applied `UsdGenMaskAPI` reach a codeless derived type in practice? | SI-8 / **M0 pre-work, binding at M1** | The tool also applies `UsdGenMaskAPI` explicitly on every operator it authors | ENG-2 |
| Q-08 | What is the ragged-buffer penalty, and does `Resample`-on-import become automatic rather than offered? | E-1r / **M2** | Ragged is supported in v1; the import tool *offers* "resample to N" with the penalty shown | ENG-1 with the TD |
| Q-09 | What retires a generation that an in-flight Storm or hdPrman sync still references, if the publish ring is ever enabled? | **SI-4 at M1** proves the torn-read property; the ring's retirement assertion is added to SI-4 when the ring is built (**M7**, ADR §9 R20) | Copy-on-write handoff; the ring stays off. Retirement is the foreign-data-source detached callback, never a poll (§0.3 erratum) | ENG-1 |
| Q-10 | Is the prototype-surface rebase arithmetic correct inside propagated native prototypes? Discovery of `__usdPrimInfo.niPrototypePath` is MEASURED; the rebase is not. | T-INST-2 / **M6** | Rebase `usdGen:surface` onto the prototype root using the ancestor's `niPrototypePath` and the prim's `primOrigin` | ENG-2 |
| Q-11 | Is `_allowAsync` reachable from a plugin, and is `sceneGlobals/primaryCameraPrim` readable from a renderer-level index in usdview and `usdrecord`? | T-5 / **M8** | `--allow-async` is the documented fallback; camera-seeded publish order is an optimisation | ENG-3 |
| Q-12 | What is the Storm frame time at 100 000 curves × 8 CV? The ~12 ms figure is **DERIVED** (interpolated between 5.01 ms at 40 k and 23.93 ms at 200 k, both MEASURED, `research/G-storm-hair-look-prototype.md` §5, ledger `EV-020`/`EV-021`) and cannot stand as a gate threshold. | S-1 re-measured on the EGL harness at 720p and 1080p / **M1** | S-1's threshold and the frame ledger in `09-performance-and-benchmarks.md` §0.2 both carry the DERIVED/UNMEASURED label; 60 Hz at 100 k is not established (ADR §9 R41) | ENG-2 |
| Q-13 | Can operators be shared between descriptions (one clump setup across brows and lashes)? The reserved layout puts `Ops` under each Description. | **M5** (tool UX review with the TD) | Not shared; each Description owns its `Ops` scope. A referenced `Presets` scope is the candidate answer | Owner with the TD |
| Q-14 | What does `usdGen:density` do under a `metersPerUnit` change? The unit (hairs per square stage unit on the rest surface) and non-uniform scale are settled; a stage-unit change is not. | **M3** (documented rule plus a T0 test) | Density is read in the stage's units at capture; a `metersPerUnit` change is a recapture and the count changes | ENG-1 with the TD |
| Q-15 | When are Windows and macOS builds scheduled? bison/flex for SeExpr and the Hgi resource-generation path are the known blockers. | S-8 at **M1** (proxy compile under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, ADR §5.4) → written scope decision at **M5** (SC-9) → R-3 on hardware at **release** | Linux only for v1; the CMake stays portable but no Windows/macOS build is gated | Owner |
| Q-16 | Is the assumed team (3 engineers plus a part-time TD) real? Every calendar number in `11-roadmap.md` rests on it. | **M0** re-baseline | 3 engineers, 30 % contingency applied to M1/M3/M7, milestones serial: 33 weeks + 1 week float = **34 weeks** (all ASSUMPTION, A-01, A-30; ADR §7, §9 R39) | Owner |

---

## 4. Assumptions register

Each row is a design choice whose *justification* is unmeasured. The **Binding** column says whether
`design/adr-v1.md` has already fixed the choice — in which case only new evidence plus an ADR
amendment may change it — or whether it is still this plan's assumption. Rows may contain MEASURED
numbers where the number is solid and only the *policy* built on it is assumed. Other plan documents
tag their own assumptions in place; this table collects the ones inherited from `design/adr-v1.md`
(§1–§7 and the §9 addendum), `design/brief-v1.md` and the three proposals, so a reviewer finds them
all in one pass.

| # | Assumption | Binding | Source | Reversal cost |
|---|---|---|---|---|
| A-01 | Staffing is 3 engineers (`ENG-1` engine/kernels, `ENG-2` imaging, `ENG-3` tools/Python) plus a part-time TD; every calendar estimate follows from it | no | ADR §7; `design/proposal-risk.md` §11.1 | Re-baseline `11-roadmap.md`; no design change |
| A-02 | v1 targets ≤ 1 M rendered curves and ≤ 200 k interactive curves per description; the LOD ladder is mandatory above ~150 k | no | `design/proposal-artist.md` §1.4 | Re-tune LOD defaults |
| A-03 | Grooms are authored per asset in the asset's layer stack; shot work is overrides, freezes and sim caches | no | `design/proposal-artist.md` §1.1 | Layout change in `02-schema.md` **§3** (Reserved layout) |
| A-04 | The primary artist application is usdview plus the usdGen plugin; a DCC bridge is out of scope for v1–v3 | no | `design/proposal-artist.md` §1.2 | New integration work; the C ABI is already the surface |
| A-05 | Operator prims are non-imageable (`UsdTyped` base) so they never acquire `visibility`/`purpose` | yes (ADR §2.1) | `design/proposal-artist.md` §1.6 | Schema change before C1 freezes at M1 |
| A-06 | One prim type per operator *concept* with a `usdGen:mode` token, not one type per mode | yes (ADR §2.1) | ADR §2.1; `design/proposal-artist.md` §1.7 | Schema change before C1 freezes at M1 |
| A-07 | Colour ramps are `float[] positions` + `color3f[] colors`; the whole-`TsSpline` transport in the UI is v2 | no | ADR §1 (S11), §9 R11, R38; `design/proposal-risk.md` §11.3 | Additive: the `TsSpline` form is an added encoding |
| A-08 | `uniform int usdGen:algorithmVersion` per operator type preserves look across releases | no | `design/proposal-risk.md` §11.2 | Drop the property; look reproducibility becomes best-effort |
| A-09 | SeExpr's `noise()` keeps 0..1 semantics (XGen's is −1..1); `snoise` is the signed form and is documented | no | `design/proposal-risk.md` §11.4 | Breaks existing expressions if changed later — decide before M4 |
| A-10 | `UsdGenOpRegistry` is closed in v1/v2; no third-party operator ABI | no | ADR §3; `design/proposal-risk.md` §11.6 | Opening it later is additive |
| A-11 | Default thread limit is `min(hw, 8)`, from the measured regression past 8 threads on this heterogeneous host | no | ADR §4.1 (I8); `design/proposal-risk.md` §11.7 | One env var (`USDGEN_THREAD_LIMIT`); the calibration re-derives it |
| A-12 | Soft memory budget `USDGEN_MEMORY_BUDGET_MB=4096` with an eviction score of `recomputeCostMs/bytes`; the 668 MB figure is MEASURED (ledger `EV-007`), the *policy* is not | yes (ADR §4.1) | ADR §4.1; `design/proposal-performance.md` §5; `design/proposal-risk.md` §11.8 supplies only the `USDGEN_MEMORY_BUDGET_MB=4096` default and proposed oldest-touched-first, which the ADR superseded | Policy tuning only |
| A-13 | `usdGen:schemaVersion` refuse-and-warn on a newer version: a partial groom is worse than an absent one | no | `design/proposal-risk.md` §11.9 | Policy change; keep the property |
| A-14 | `UsdGenRestAPI` is an applied API schema on the surface, not an implicit adapter on every mesh; it applies only to the parent `Mesh` (ADR §9 R15) | yes (ADR §2.1) | `design/proposal-risk.md` §11.10 | Adapter change in `06-imaging.md`; keeps cost off untouched meshes |
| A-15 | The Groom/Description split (XGen Collection/Description) rather than a single prim type | yes (ADR §2.1) | `design/proposal-risk.md` §11.11 | Schema change before C1 freezes at M1 |
| A-16 | Guides reuse the frozen-curve contract C3 rather than having their own schema | no | `design/proposal-risk.md` §11.12 | Contract change before C3 freezes at M2 |
| A-17 | Uniform CV count is the fast path and every v1 *generator* emits it; ragged buffers are correct but slower | yes (ADR §4.2.2) | ADR §4.2.2; `design/proposal-performance.md` §1.1 | Measured at gate E-1r (RK-12, Q-08) |
| A-18 | Chunks are ordered **surface-major** so a surface dirty is a range and not a scan, and Morton-sorted within a surface (21 bits per axis over the rest-space root position). The Morton half is what makes tile extents tight enough to cull and a brush footprint touch O(1) chunks | yes (ADR §4.1) | ADR §4.1; `design/proposal-performance.md` §1.3, §5 | Gate S-4 proves or disproves the culling half; the partition can change to UV islands |
| A-19 | The interactive LOD ladder is decimation by stable id: keep iff `UsdGenHash32(curveId, kSaltDensity) < keepFraction · 2^32`, `keepFraction = clamp(scale_groom · scale_description, 0, 1)` (ADR §9 R13). `curveId` is `uint64[] primvars:usdGen:curveId` from the pinned SplitMix64 hash (R12); `kSaltDensity ≠ 0` so the surviving set is never `{hairId < scale}`. Decimation never re-scatters, never re-chunks and never changes ids | yes (ADR §9 R12, R13) | ADR §9 R12, R13; `design/proposal-performance.md` §1.5 | Artist-expectation risk only; the arithmetic is what makes sculpt deltas and clump ids survive. `11-roadmap.md` §5.2 SC-4 states the same salted predicate and conforms to R13 |
| A-20 | A description's tile count is fixed for the life of the session, computed from the *maximum* density the description can reach | yes (S27/S28) | `design/proposal-performance.md` §1.6 | Required by S27/S28; changing it breaks the fixed prim set |
| A-21 | No operator calls an expression or a texture in `Evaluate`; animation goes through a scalar parameter or a capture-epoch bump | no | `design/proposal-performance.md` §1.8 | Hard rule; relaxing it puts ≈ 160 ms/frame of SeExpr on the interactive path at 1.6 M CVs (**DERIVED** from the MEASURED 13–117 ns/eval, `research/A8-seexpr-ptex-libs.md` §1.6) |
| A-22 | The plugin is enabled by the presence of a `UsdGenGroom` prim, checked once per `PrimsAdded` sweep, plus `USDGEN_ENABLE` | no | `design/proposal-performance.md` §1.9 | One predicate |
| A-23 | Prototype surface rebasing arithmetic for grooms inside propagated native prototypes (discovery is MEASURED, the rebase is not) | yes (ADR §2.3) | `design/proposal-performance.md` §1.4 | Gate T-INST-2 (Q-10) |
| A-24 | Storm resolves `outputs:glslfx:surface` ahead of plain `outputs:surface`, so the per-delegate override can ship OFF | no | ADR §1 (S36) | Gate L-1 (Q-03); the hedge is already written |
| A-25 | The frame ledger's "usdGen deformed tail 0.4–0.6 ms" is **DERIVED** from the measured 5-node chain, not measured directly | no | `09-performance-and-benchmarks.md` §0.2 (the plan's only ledger, ADR §9 R41); `design/judge-evidence.md` §2.3.6 | Gates E-1/S-2 measure it; the ledger must carry the label |
| A-26 | The `USDGEN_STORM_MATERIAL_OVERRIDE` env-var hedge is the right shape for the material-binding fallback, rather than always synthesizing `material_storm` | no | `design/proposal-risk.md` §11.5; ADR §5.6; the var is registered in `10-build-dependencies-testing.md` (ADR §9 R35) | Delete the env var and always synthesize; no schema change |
| A-27 | Guides are 1–10 % of hairs; the reference lane and its memory are designed and budgeted for 10 % | yes (ADR §9 R26) | ADR §9 R26; `research/A7-prior-art-grooming.md` §9.3 | Re-size the reference lane; E-4 re-run |
| A-28 | The interactive frame-rate targets (60 Hz ≤ 40 k curves, ≥ 30 Hz at 100 k, ≥ 10 Hz at 200 k) hold until S-1; 60 Hz at 100 k × 8 CV at refineLevel 2 is **not established** | yes (ADR §9 R41) | ADR §9 R41; `09-performance-and-benchmarks.md` §0.2 | Re-state the targets; `usdGen:densityScale` is the artist's lever |
| A-29 | SI-6's bounded first-access traversal costs ≤ 5 ms on the 11 005-prim stage | no | ADR §9 R28 | SI-6 measures it; if it is worse, population moves behind a lazy per-root scan (RK-10) |
| A-30 | Milestones are serial vertical slices; 33 weeks of milestones + 1 week integration float at the C4 freeze = 34 weeks at 3 engineers | no | ADR §9 R39; ADR §7 | Re-baseline `11-roadmap.md`; no design change |
| A-31 | `usdGen:algorithmVersion` fallback 0 = latest kernel, so look preservation is guaranteed only for tool-authored assets; a hand-written asset that omits the property tracks the newest kernel | yes (ADR §9 R17) | ADR §9 R17 | Change the fallback to the shipping version; a documented behaviour change |

---

## 5. Testing

This document contains no mechanism, so it is proved indirectly: every open question closes on a
named gate, and every rejected alternative that could silently return is pinned by a regression test.
Tiers are the plan's T0–T4 (ADR §7, §9 R2, defined in `10-build-dependencies-testing.md` §5.1), which
refine S45's four harness tiers: **T0** engine, no Hydra or USD; **T1** headless scene index over the
real `UsdImagingCreateSceneIndices` chain; **T2** Storm through the EGL harness (S45's Xvfb + llvmpipe
is the CPU-only fallback for T2, not a tier of its own); **T3** `testusdview`; **T4** workstation,
release criteria only. Gate, tier and milestone in the table below are reproduced from
`09-performance-and-benchmarks.md` §5 and may not disagree with it (ADR §9 R40).

| What is proved | Tier | Gate | Milestone |
|---|:--:|---|---|
| Chain placement has not regressed (X-01, X-02, RK-22, §2.1 item 3) | T1 | SI-5 | M1 |
| Every `usdGen:*` property of every registered type appears and dirties (RK-01, X-23) | T1 | SI-7 | M1 |
| `UsdGeomBoundable::ComputeExtentFromPlugins` returns true for a `UsdGenDescription` through `libusdGenSchema.so` (RK-29) | T1 | SI-7 | M1 |
| An operator-parameter edit dirties only the asserted locator set (RK-01, X-17) | T1 | SI-2 | M1 |
| Exactly one commit per edit batch, all on the expected thread (X-15, X-17, ADR §9 R32) | T1 | SI-3 | M1 |
| `points.size() == Σ curveVertexCounts` on every tile (X-11) | T1 | SI-1 | M1 |
| No torn reads under concurrent publish (X-18) | T1 | SI-4 | M1 |
| Ring retirement: no buffer is reused before its detached callback has fired (Q-09) | T1 | SI-4 (extended) | M7 |
| Auto-applied `UsdGenMaskAPI` reaches a codeless derived type (RK-02, Q-07) | T1 | SI-8 | M0 pre-work, binding at M1 |
| `reorder nameChildren` produces usable Hydra invalidation through the adapter, or does not (Q-06) | T1 | SI-11 (record only, run as PW-6) | M0 |
| Both population paths yield the same prim set, within the A-29 bound (RK-10) | T1 | SI-6 | M1 |
| Two scene index instances attached to one session report the same generation, prim set and frame for one commit (RK-09) | T1 | SI-10 | M2 |
| The private pruning wrapper resolves `skinnedPoints` and is affordable on a production-density skinned scalp (SC-13) | T1 | SI-9 | M2 |
| The `usdGen` core links no `usd`/`usdImaging`/`hd` (S8, ADR §9 R37) (RK-30) | T0 | **B-1** | M0 |
| Engine floor, sparse edits, recompile scope, thread ratio (RK-19, §2.1 item 2) | T0 | E-1, E-2, E-6, E-7 | M1 |
| Terminal-parameter edit only (RK-19) | T0 | E-3 | M2 |
| Bitwise determinism across thread counts (RK-20) | T0 | E-8 | M1, re-run M4 |
| kNN capture is affordable and linear in roots (RK-11) | T0 | E-4 | M0 pre-work, binding at M3 |
| Ragged path within 2× of uniform (RK-12, Q-08) | T0 | E-1r | M2 |
| Memory at 1 M curves (RK-13, SC-7a/SC-7b) | T0 | E-5 | M3, re-run M7 |
| `PickCV` cost at 100 k / 1 M CVs (RK-25's fallback path, S40) | T1 | T-2 | M5 |
| SeExpr sandbox: `SE_EXPR_PLUGINS` cleared before `ExprFunc::init()`, closed function set, a parse error yields a fallback value not an exception (RK-15) | T0 | **T-EXPR-1** | M4 |
| Ptex face ids match `Far::PtexIndices` on quads, n-gons and all-triangle meshes (RK-16) | T0 | **T-PTEX-1** | M4 |
| Storm static draw and batch count (Q-12) | T2 | S-1, S-5 | M1 |
| No VBO relocation across deform frames | T2 | S-6 | M1 |
| Deform frame and one-tile edit | T2 | S-2, S-3 | M2 |
| Per-tile culling (A-18) | T2 | S-4 | M2 |
| Density scrub without a whole-VBO reallocation (RK-18) | T2 | S-7 | M5 |
| Storm frame time and `drawBatches` at 1 / 32 / 128 / 196 tiles versus one prim (RK-07, Q-02) | T2 | S-12 | M1, before C2 freezes |
| `hairTangent` strategy, incl. the `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` proxy compile (RK-05, RK-21, Q-04) | T2 | S-8 | M0 pre-work, binding at M1 |
| refineLevel switch cost (RK-06, Q-05) | T2 | S-9 | M0 pre-work, binding at M1 |
| Render-context material resolution (RK-03, RK-04, Q-03) | T2 | L-1 | M0 pre-work, binding at M1 |
| Brush latency, CV pick accuracy, freeze cost with the `Tf.Error.Mark` containment asserted (RK-08, RK-17) | T3 | T-1, T-3, T-4 | M5 |
| Progressive generation from a plugin (RK-26, Q-11) | T3 | T-5 | M8 |
| Instancer pick round-trip and prototype rebasing (A-23, Q-10) | T1 | T-INST-1, T-INST-2 | M6 |
| Version-sensitive mechanisms still behave on a new USD install (RK-23) | T1 | full T1 suite | M7 |
| hdPrman parity, MSAA/OIT quality, Metal/Vulkan shader compile on real hardware (RK-14, RK-21) | T4 | R-1, R-2, R-3 | release criteria `RC-n`, never milestone exits (ADR §9 R39) |

Every gate, tier and milestone cell above was re-checked against `09-performance-and-benchmarks.md`
§5 on 2026-09-05 and agrees with it, including the R40 assignments **SI-5 M1**, **S-6 M1**, **E-3 M2**,
**S-4 M2** and **T-INST-1/2 T1 M6**; `10-build-dependencies-testing.md` §6.5 and `11-roadmap.md` §2.0
carry rows for **T-EXPR-1** and **T-PTEX-1**.

Two tests belong to this document itself and run in CI at tier T0, as a script over `plan/`:

1. **Reference integrity.** Every `S<n>` cited in `plan/` exists in `design/brief-v1.md` §2; every
   `R<n>` cited exists in `design/adr-v1.md` §9; every gate id exists in
   `09-performance-and-benchmarks.md` §5 and matches its tier and milestone there; every milestone id
   is `M0`–`M8`; every `SC-<n>` exists in `11-roadmap.md` §5; every `EV-nnn` cited in `plan/` exists
   in `appendix-A-evidence-ledger.md`; every "see `NN-…md` §X" points at a section that exists in the
   current sibling (ADR §9 R45); every `research/` or `prototypes/` path resolves.
2. **Tag discipline.** Every numeral with a unit sits in a paragraph, table row or code block
   carrying MEASURED, DERIVED, UNMEASURED or ASSUMPTION; a MEASURED tag names an `EV-nnn` row and a
   DERIVED tag names the `EV-nnn` rows it is derived from (ADR §9 R42). A table may satisfy this once
   in its preamble — §0.2 and §1 do — provided each row that departs from the preamble tags itself.

The register is reviewed at every milestone exit: a risk whose gate is green is struck, a closed
question becomes a line in §0.2, and anything newly learned is added with its evidence.

---

## 6. Out of scope

The designs the risks attach to live elsewhere: schema `02-schema.md`, engine
`03-execution-engine.md`, imaging `06-imaging.md`, gate definitions, thresholds and the frame ledger
`09-performance-and-benchmarks.md` (§5 and §0.2), milestone content, calendar and the SC register
`11-roadmap.md`, measured numbers with file:line `appendix-A-evidence-ledger.md`.

Also not covered: the v1/v2/v3 feature split (`04-operators.md` **§1–§4**; ADR §6 as amended by ADR
§9 **R38**, which defines v1 = the M0–M7 deliverable set and v2 = M8 breadth); a hiring or budget
plan beyond recording A-01 and A-30; a legal review — RK-24 identifies the licences and the NOTICE
obligation, it does not clear them; a security review of OpenUSD itself — RK-15 covers only usdGen's
own expression sandbox; and the risks of features already out of scope for v1 and v2 (simulation and
self-collision, a DCC bridge, GPU evaluation of the chain, XPD/Alembic import, Ptex sampling inside
Storm, an OpenExec backend, a node-graph authoring UI, per-instance material binding, automatic
pickup of overwritten map files).

---

## 7. Sources

| Source | Sections used |
|---|---|
| `design/adr-v1.md` | §1 amendments (whole), §2.1–2.3 vocabulary, §3 contracts, §4.1–4.5, §5.4–5.6, §6, §7 milestones and gates, §8 document map, **§9 addendum R1–R45 (whole)** |
| `design/brief-v1.md` | §1 R1–R9, §2.1–2.9 S1–S46, §3 D1–D8, host facts |
| `design/proposal-risk.md` | §10.1 register, §10.2 API stability boundaries, §10.3 out of scope, §11 assumptions, §9.1 test tiers |
| `design/proposal-performance.md` | §1 assumptions, §2 facts F1–F14, §5 (chunk order, eviction score), §11.1–11.2 cost model and gates (superseded on thresholds by ADR §9 R27/R40), §13 risks, §14 out of scope |
| `design/proposal-artist.md` | §1 assumptions, §11 risks and stop conditions, §12 out of scope |
| `design/judge-evidence.md` | §0 verification pass, §1 scores, §2.1–2.4 constraint violations, §3 grafts, §4 unresolved, §5 recommendation |
| `design/judge-artist.md` | §3 constraint violations, §4 grafts, §5 unresolved, §6 recommendation |
| `design/judge-delivery.md` | §5 phase-1 blockers, §6 grafts, §7 estimates and gates, §8 unresolved, §9 recommendation |
| `research/G-chain-order-probe.md` | §2–§4c, §5 (RigExec `SetTime` baseline), §6 |
| `research/G-data-plane-engine-prototype-benchmark.md` | §0 host block, §2, §3.2, §3.3 (thread sweep), §4, §5, §6, §8 |
| `research/G-evaluation-scheduling-and-batching.md` | §4, §5 (the three models and the torn-read probe), §6, §7 (the thread-safety rule), §8, §10 |
| `research/G-stage-free-parameter-and-time-transport.md` | §0–§3, §5–§7 |
| `research/G-storm-throughput-and-prim-granularity.md` | §1.4–§1.13, §2 |
| `research/G-storm-hair-look-prototype.md` | §0, §2.4–2.6, §3–§5, §6 workstation protocol |
| `research/G-freeze-bake-undo-and-frozen-reentry.md` | §1.1–§1.5 (§1.4's Python-binding claim corrected in RK-08), §2.1–2.3, §3 |
| `research/G-tool-loop-array-transport-and-cv-picking.md` | §1.2–§1.5, §2.4, §3 |
| `research/G-instancing-cards-archives-and-native-instances.md` | §1–§5, §7 |
| `research/G-motion-blur-sampling-strategy.md` | §3.3, §4 |
| `research/G-hdprman-and-usdrecord-render-time-chain.md` | §1.2, §2.1, §3 |
| `research/A2-usdrig-imaging.md` | §2 |
| `research/A3-usdrig-tools.md` | §3.1 (usdview's mangled API handle), §6 (document conventions), §7–§8 |
| `research/A4-openusd-hdgp-adapters.md` | §2, §7 |
| `research/A5-storm-curves-shading.md` | §3, §4, §6 |
| `research/A6-openexec-vdf.md` | control-plane extensibility |
| `research/A7-prior-art-grooming.md` | §8, §9 (§9.3 guide counts, A-27) |
| `research/A8-seexpr-ptex-libs.md` | §1.1–1.3 (`ExprFunc::init()` / `SE_EXPR_PLUGINS`), §1.6, §2.5–2.8 (§2.6 Ptex face ids), §5, §6.2, §7 licensing |
| `research/B-usdrig-build.md` | §2 (build and timings), §7 (artifacts and env), §8 (the double-`pxrConfig` trap) |
| `research/ENVIRONMENT.md` | host facts and the CORRECTIONS block |
| `prototypes/storm-hair-look/` | `eglctx.h`, `usdGenHairPreview.glslfx`, `bench_hair.cpp` |
| `prototypes/data-plane-benchmark/` | chunk-size sweep, SoA/AoS kernel, RSS at 1 M |
| `prototypes/freeze-bake/` | `RemovePrim` reproduction, `probe9_error_containment.py:27` (the `hasattr(Tf, "ErrorMark")` test), `SubtreeSnapshot` fidelity and cost |
| `prototypes/tool-loop/` | array transport and CPU CV picking |
| `prototypes/thirdparty-bench/` | SeExpr and Ptex micro-benchmarks |
| OpenUSD 26.08 (`/home/burkard/work/OpenUSD`) | `pxr/base/tf/wrapError.cpp:200, 212-221` (`Tf.Error.Mark`), `pxr/base/vt/array.h:39-55, 53, 269, 945, 984, 1009, 1023`, `pxr/imaging/hdSt/renderDelegate.cpp:695-707` (ADR §9 R43's range; `materialRenderContexts` at `:702-707`), `pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45`, `pxr/imaging/hd/sceneGlobalsSchema.h:37-46`, `pxr/imaging/hd/mergingSceneIndex.cpp:173-245` (`_IsObserved()` guard `:211-213`, `_SendPrimsAdded` `:244`), `pxr/imaging/hdSt/vboMemoryManager.cpp:605-620`, `pxr/imaging/pxOsd/refinerFactory.h:34-41`, `pxr/usdImaging/usdImaging/sceneIndexPlugin.h:82-92`, `pxr/usdImaging/usdImagingGL/engine.cpp:148-201` (ADR §9 R43's range), `pxr/usdImaging/usdviewq/appController.py:381, 432, 523-529`, `pxr/usdImaging/usdviewq/plugin.py:333`, `pxr/usdImaging/usdviewq/stageView.py:828-833, 966, 2438-2445`, `pxr/usdImaging/bin/testusdview/testusdview.py:144`, `pxr/usd/usd/schemaRegistry.cpp:922-940`, `pxr/usd/usdGeom/boundableComputeExtent.cpp:155-173` (early-out `:159-163`, `GetPluginForType` `:166-173`), `pxr/usd/usdLux/schema.usda:1126-1131`, `pxr/usd/usdLux/plugInfo.json:28`, `pxr/exec/esfUsd/stageData.cpp:361` (the brief S41/S46 and `research/G-freeze-bake-undo-and-frozen-reentry.md` §1.4 write `:360`; `:361` is the predicate call in this checkout), `/home/burkard/work/OpenUSD_26_08/include/pxr/pxr.h:23` (`PXR_INTERNAL_NS`) |
