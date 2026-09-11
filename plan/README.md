# usdGen — implementation plan

Date: 2026-09-05 v1; overlay 2026-09-11. Status: plan v2 (accepted: v1 history
in `00`–`12` plus overlay `13-codebase-alignment.md`). The subsequent user
decisions in **[14-hierarchy-cuda-implementation.md](14-hierarchy-cuda-implementation.md)**
supersede conflicting explicit-edge, CPU-expression, and GPU-deferred claims
below. Historical measured numbers do not prove the new CUDA implementation.

This directory is the complete plan for **usdGen**, an XGen-like hair/fur grooming and instancing
plugin for OpenUSD 26.08 and Hydra 2.0. usdGen is built as a sibling CMake project of usdRig, runs
*after* usdRig (or any other deformer) in the Hydra scene-index chain, generates curves for Storm and
hdPrman at run/render time, loads and deforms static curves, styles them with a chainable operator
stack, paints and expresses look through maps, Ptex and SeExpr, and is groomed interactively through
usdview plugins that follow the app → data modification → Hydra loop. The verbatim request is quoted
in `00-request-and-scope.md` §0.1. The working name is `usdGen`; the intended repository is
`/home/burkard/work/usdGen`, and this plan lives there as `plan/`.

## 1. TL;DR (the binding decisions, `design/adr-v1.md`)

* **Placement.** One renderer-level `HdSceneIndexPlugin` (`UsdGenGroomSceneIndexPlugin`), phase 0 /
  `InsertionOrderAtEnd`, `loadWithRenderer ""`, so it lands strictly after UsdSkel, RigExec and
  flattening and before every Storm/hdPrman plugin — measured, not assumed (gate SI-5). One binary
  serves Storm, hdPrman and `usdrecord`.
* **Authoring model.** Codeless USD schemas: `UsdGenGroom` → `UsdGenDescription` → an explicit
  `usdGen:input`-wired stack of `UsdGenOperator` prims (Scatter, Grow, GuideInterpolate, CurveSource,
  Clump, Noise, Length, Width, Smooth, Resample, Direction, Scale, SculptLayer, Deform, Freeze,
  Instance …), `UsdGenMap` prims (image, Ptex, SeExpr, paint, noise, combine, guide proximity) and
  the applied schemas `UsdGenMaskAPI`, `UsdGenLookAPI`, `UsdGenRestAPI`, `UsdGenCurveAPI`. Every
  property is `usdGen:`-namespaced and reaches Hydra through prim adapters
  (`UsdImagingDataSourceMapped`), which is what makes an edit a precise dirty instead of a resync.
* **Engine.** A stage-free evaluator (`libusdGen.so` links no `usd*`/`hd*`, enforced by gate B-1)
  over a pure-value `UsdGenGraphDesc`: bespoke TBB DAG, planar SoA buffers, 512-curve chunks as the
  dirty/parallel unit, a reference lane for guides and clump centres, capture/evaluate split, three
  digests, a dirty router, a private 8-thread `tbb::task_arena`, per-node caches under a memory
  budget. Baseline: a 5-operator chain over 100 k curves × 8 CV evaluates in 1.02 ms (MEASURED,
  `EV-008`); gate E-1 is ≤ 1.5 ms.
* **Publication.** Deferred commit (triggers (a) `SetTime`/`Commit`, (b) `currentFrame`, (c) the
  `_PrimsDirtied` batch — always), atomic snapshot publish, diffed dirties, exact-size arrays. A
  description publishes 32–256 `basisCurves` **tiles** (49 at 100 k curves), never one prim per
  chunk; chunk ≠ tile.
* **Static curves.** One curve contract, C3, for guides, freezes, imports and sim caches; frozen
  curves re-enter through `UsdGenCurveSource` and deform with the surface through `UsdGenDeform`;
  sculpt layers and freeze tiers (`session | sublayer | payload`) ride on it.
* **Look.** One `Material` with three terminals; the Storm preview is the plugin's own glslfx
  (`UsdGenHairPreview`, `…Translucent`, `…Primvar`), MaterialX for the render network; all maps,
  Ptex and SeExpr are CPU-at-capture and baked to primvars.
* **Tools.** A usdview `PluginContainer` over a ctypes C ABI (`08-tools.md` §1.4, contract C4) plus
  a pxr_boost array module; live overrides during a drag, one stage write per gesture, undo by
  `SubtreeSnapshot`, freeze by `SetActive(false)`, never `RemovePrim` interactively.
* **Contracts and schedule.** C1 names, C2 tiles and C5 glslfx inputs freeze at M1; C3 at M2; C4 at
  M5. Nine milestones M0–M8, serial vertical slices, 34 weeks with three engineers (ASSUMPTION).
  Every claim is gated: fifty-three gates in `09-performance-and-benchmarks.md` §5, each with a tier,
  a threshold, a milestone and a status.

## 2. Status vocabulary

* **Document status.** `plan v1 (draft, pending review)`: written, adversarially reviewed and
  cross-document reconciled by the planning session (2026-09-04/05); awaiting the owner's read.
  `accepted` and `superseded` are the only other states.
* **Number tags** (ADR §9 R42). **MEASURED** — a probe printed it; cite the `EV-nnn` row.
  **DERIVED from EV-nnn** — scaled, summed or interpolated; never written as measured.
  **UNMEASURED** — with the gate that will measure it. **ASSUMPTION** — no gate will settle it; say
  what would falsify it.
* **Gate status** (09 §5). `MEASURED-pass`, `UNMEASURED`, `record only` (SI-11, S-11). Tier-4 gates
  are release criteria (`RC-n`), never milestone exits.
* **Source facts.** Every OpenUSD or usdRig `file:line` in the plan was re-verified by grep against
  `/home/burkard/work/OpenUSD` at v26.08 (`ee47c679a`) and usdRig at `c92c040`; verification limits
  are listed in `appendix-A-evidence-ledger.md` §3.11.

## 3. Single registries (where a name is owned)

| Thing | Owner |
|---|---|
| Binding decisions and the rulings R1–R45 | `design/adr-v1.md` §1–§8 and §9 |
| Prim types, properties, defaults, tokens, dirty classes | `02-schema.md` |
| The C ABI (contract C4) | `08-tools.md` §1.4 |
| Gate ids, tiers, thresholds, milestones, status | `09-performance-and-benchmarks.md` §5 |
| The frame ledger | `09-performance-and-benchmarks.md` §0.2 |
| Env vars and CMake cache variables | `10-build-dependencies-testing.md` §3.5 |
| Test tiers T0–T4, CTest names, `docs/workstation-protocol.md` §§1–11 | `10-build-dependencies-testing.md` §5.1, §5.6, §5.2 |
| Pre-work PW-1…PW-13, stop conditions SC-1…SC-13, release criteria RC-1…RC-11 | `11-roadmap.md` §1, §5, §6.4 |
| Risks RK-, questions Q-, assumptions A-, rejected alternatives X- | `12-risks-decisions-open-questions.md` |
| Measured numbers `EV-001…EV-092`, verified `file:line` facts, corrections K1–K25 | `appendix-A-evidence-ledger.md` |
| Prototype directories and what each is carried into | `appendix-B-prototype-inventory.md` |
| Plan-vs-codebase alignment (ground truth, kernel-status table, milestone re-baseline, debt register) | `13-codebase-alignment.md` |
| Current user decisions, CUDA/hierarchy implementation work and evidence limits | `14-hierarchy-cuda-implementation.md` |

## 4. Index

| Document | In one sentence |
|---|---|
| `00-request-and-scope.md` | The verbatim request, requirements, version scope, constraints, glossary, document map and reading orders. |
| `01-architecture.md` | The three-part shape, frame budget, engine invariants I1–I8, contracts C1–C5, sessions, commit triggers. |
| `02-schema.md` | The normative property registry, reserved layout, mask block, maps, freeze/sculpt, versioning, three `.usda` examples. |
| `03-execution-engine.md` | `UsdGenGraphDesc`, node interfaces, SoA buffers, chunks, capture/evaluate, digests, dirty routing, threads, memory, diagnostics. |
| `04-operators.md` | The operator catalogue: parameters, mathematics, cost classes, spaces, emitted primvars, v1/v2/v3 split. |
| `05-static-curves-and-deformation.md` | Contract C3, frozen re-entry, rest surfaces, deformation, sculpt layers, freeze tiers. |
| `06-imaging.md` | The four registrations, tile contract C2, invalidation discipline, guides, instancers, live overrides, `primOrigin`, population. |
| `07-look-maps-expressions.md` | Material terminals, the glslfx, MaterialX, colour baking, map types, Ptex face mapping, SeExpr, reload. |
| `08-tools.md` | The usdview plugin: module split, tool state, the C ABI, brushes, freeze/commit flows, undo, panels, picking. |
| `09-performance-and-benchmarks.md` | The performance model, the frame ledger, benchmark protocols and the gate registry. |
| `10-build-dependencies-testing.md` | CMake targets and the link rule, vendored third party, install layout, registries, test tiers and harnesses. |
| `11-roadmap.md` | Pre-work, milestones M0–M8 with exit gates, dependency graph, estimates, stop conditions, definition of done. |
| `12-risks-decisions-open-questions.md` | Settled decisions S1–S46 and rulings, rejected alternatives, risk register, open questions, assumptions. |
| `13-codebase-alignment.md` | **v2 overlay (binding): ground truth vs code, operators-as-prims, relationships-form-the-graph at Hydra runtime, milestone re-baseline, debt register, §9 harness handoff (the current start line). Owns plan-vs-codebase alignment; wins over `00`–`12` where they disagree (§6 lists every superseded claim).** |
| `appendix-A-evidence-ledger.md` | Host facts, every measured number as an `EV-nnn` row, verified `file:line` facts, unmeasured claims, corrections. |
| `appendix-B-prototype-inventory.md` | The twelve prototype directories: files, build lines, what each proved, what it is carried into. |
| `design/` | `brief-v1.md` (requirements R1–R9, decisions S1–S46), the three proposals, the three judge reports, `adr-v1.md`, the four consistency-lens reports. |
| `research/` | Reports A1–A8 (usdRig, OpenUSD, prior art, third-party libraries), `B-usdrig-build.md`, the eleven `G-*` verification reports with probes, `ENVIRONMENT.md` (its CORRECTIONS block overrides its body). |
| `prototypes/` | Source copies of the twelve probe directories the numbers came from (`appendix-B` is the guide). |

## 5. Reading orders

* **Engine engineer** — 00 → 01 → 03 → 02 (operator properties, versioning) → 05 → 04 → 09 → 10 → 11.
* **Imaging engineer** — 00 → 01 → 06 → 02 → 05 → 09 → 07 → 10.
* **Tools engineer** — 00 → 01 → 08 → 02 → 05 → 06 → 10.
* **TD / artist reviewer** — 00 → 02 (the `.usda` examples first) → 04 → 07 → 08 → 11 → 12.
* **Anyone disputing a number** — `appendix-A` §2 for the row, then 09 §0.2 for how it is used.

## 6. Vocabulary hazards

* **chunk ≠ tile.** A chunk is 512 curves, the engine's dirty and parallel unit; a tile is one Hydra
  `basisCurves` prim holding `chunksPerTile` chunks (`nTiles = min(nChunks, clamp(ceil(nChunks /
  chunksPerTile), 32, 256))`).
* **T0–T4** are test tiers; **T-1…T-5**, **T-INST-n**, **T-EXPR-1**, **T-PTEX-1** are tool gates;
  `usdGen:frozen:tier` is a token.
* **S1–S46** are the brief's settled decisions; **S-1…S-12** are Storm gates; milestones are
  **M0–M8**, never S.
* **R1–R9** are the brief's requirements; **R-1…R-3** are render gates; **ADR §9 Rnn** are rulings;
  a proposal's risk item is bare (`R9`) and the plan's risk register is **RK-nn**.
* **I1–I8** are the engine invariants; **P0/P1/P2** are motion profiles; **PW-n** is pre-work.

## 7. How this plan was produced

Two research rounds on this host (Linux aarch64, 20 cores, NVIDIA GB10, OpenUSD 26.08 installed at
`/home/burkard/work/OpenUSD_26_08`, no display) produced `research/`; a brief with requirements and
settled decisions; three independent architecture proposals scored by three judges; the binding ADR
and its 2026-09-05 addendum; then the fifteen documents, each adversarially reviewed and fixed, four
cross-document consistency passes, per-file reconciliation against the addendum, and a final
read-through with a mechanical lint (cross-references, gate ids, `EV` ids, `SC`/`PW`/`RK`/`Q`/`A`/`X`
ids, test names, forbidden names) on 2026-09-05. usdRig and OpenUSD were read but not modified.

## 8. What happens next

M0 is done; M1 is at near-exit (open: E-1 perf, S-1/S-5/S-6/S-12 bench runs —
`.omp/gate-status.md`). Next, in order: tag M1, then M2 (SculptLayer + Freeze
kernels, C3 freeze, and the Hydra-sourced graph builder of `13-…` §3.3, which
removes the stage-sourced staging debt). `13-codebase-alignment.md` §4 owns
the milestone re-baseline; **§9 is the current start line** (M2 mid-V2-11
handoff, 2026-09-11: split the stage builder out of the B-2 fence, migrate
`_CommitNow` to `BuildGraphDescFromHydra`, unplug the stage side channels,
close gate B-2). `.omp/handoff-20260911.md` is the harness entry pointer.
`11-roadmap.md` §2.3+ milestone contents stand.
