# Execution graph handoff — remaining work

Updated: 2026-09-14. **The overall execution-graph goal is not complete.**

This file is in `plans/` as requested. The existing design documents are in
`plan/`; links below deliberately point there. Read the current source and the
latest checkpoints before treating historical “in progress” sections as open
tasks or historical passing tests as proof of current completion.

## Start here

1. Read [resource-aware execution requirements](../plan/15-resource-aware-execution.md#outcome-and-non-negotiable-rules),
   its [objective gates](../plan/15-resource-aware-execution.md#phased-implementation-slices-and-objective-gates),
   and the [latest aggregate RBF checkpoint](../plan/15-resource-aware-execution.md#aggregate-rbf-implementation-and-first-native-evidence).
2. Preserve the dirty worktree. **Much of Vulkan is untracked and is not on
   GitHub. A fresh clone does not contain this implementation.** Do not reset,
   clean, stash indiscriminately, or overwrite another worker's edits.
3. Finish item 1 below before building on the current CUDA admission change.
4. The recommended next substantive implementation is item 2, then the broader
   composition/concurrency work. Do not redefine success around a small passing
   operator subset.

## Scope and non-negotiable constraints

- CUDA remains required; Vulkan is in scope. **Metal is excluded** by the
  user's “don't do 8” instruction. This exclusion does not remove the stock-SDK
  renderer-residency requirement (which has a different numbering in plan 15).
- Preserve complete copy-on-write data: points, rest, widths, hairT, stable IDs,
  offsets/topology, root bindings/frames, named point/primitive/groom channels,
  chunks/tiles, private producer owners and retained ancestor generations.
- No CPU production geometry fallback or ordinary geometry D2H. Immutable CPU
  source preparation, compact status/count reads, and test-only readback are
  allowed. A CPU reference test is not a production fallback.
- No OpenUSD patches, custom rprim, replacement renderer, application mutexes,
  spinlock substitutes, global cook lock, or blocking owner callbacks.
- Preserve composed hierarchy semantics, deterministic identity/order, exact
  input/version cache keys, last-good publication and dirty state on failure.
- User requested Astra orchestration with Terra/Luna implementation and root
  validation. There are four active slots including root: use bounded batches,
  not claims of 10–20 simultaneously running agents. Historical Hivemind/local
  Qwen review waves are recorded in plan 15; they do not replace validation.
- One owner runs builds and GPU tests. Run CUDA/Vulkan/Storm suites serially.
  Workers should have explicit non-overlapping file ownership. Review their
  actual diffs; a worker completion message is not test evidence.

## Current authoritative checkpoint

- Branch `main`; last verified published commit:
  `0e51d173ae659db12a2c9c65942e86e943e38a25` (`0e51d17`). Core CUDA/shared work
  was already published; this last commit adds CUDA Length numeric regressions.
  **All subsequent admission work and Vulkan work remain local. No further push
  is authorized by this handoff request.**
- CUDA local implementation now admits one literal RBF Deform with literal
  Width prefix/tail stages through a shared compiler/runtime recipe. It adds
  named-channel bytes and per-Width private/retained bytes to the selected-device
  LU reservation. Static estimates remain partial; runtime refinement is explicit.
- CUDA aggregate test: four accepted shapes; exact named/Width byte deltas;
  cold/warm direct and native async execution at the reported budget; rejection
  one byte below; finalization rollback/retry; independent retained COW reference.
  Passed ten consecutive runs. Later strengthened channel metadata/topology/tile
  comparisons also passed in the broader run.
- Latest complete CUDA build succeeded (159 incremental build steps). CPU-only
  `usdGen` core build succeeded. This is **not** a full CPU-only test-suite result.
- Targeted CUDA graph/RBF run: **9 passed, 1 failed**, the failure being
  `testUsdGenCudaExecutionStages` described below.
- Subsequent broader CUDA-build suite, excluding only that failing test:
  **177 passed, 1 expected `StormSurgery` skip, 0 failures; 178 selected tests,
  97.38 seconds**. There are 179 registered tests including the excluded test.
  Do not report this as a fully green 179-test run.
- Last Vulkan checkpoint: **88 passed, 2 unsupported SYNC_FD skips, 0 failures
  out of 90 tests (73.71s)**. Twelve-shader tests-OFF install/consumer passed.
  This predates the latest CUDA admission edits; no new full Vulkan run occurred.
- All known build/test processes are terminal and all implementation workers
  were frozen for this handoff. Recheck processes/agents before resuming.

### Local files that need special care

- CUDA production: [cudaExecution.cpp](../libs/usdGen/usdGen/cudaExecution.cpp),
  [cudaExecution.h](../libs/usdGen/usdGen/cudaExecution.h).
- CUDA tests: [ExecutionPlan](../tests/testUsdGenCudaExecutionPlan.cpp),
  [RbfTransaction](../tests/testUsdGenCudaRbfTransaction.cpp),
  [ExecutionStages](../tests/testUsdGenCudaExecutionStages.cpp).
- [CMakeLists.txt](../CMakeLists.txt) mixes substantial uncommitted Vulkan
  integration with the new CUDA `testUsdGenCudaRbfAggregateAdmission` registration.
- Vulkan source, shaders and tests are mostly untracked under
  [vulkan/](../libs/usdGen/usdGen/vulkan/), [tests/](../tests/) and
  [scripts/](../scripts/). Package changes also touch `.gitignore`,
  `cmake/usdGenConfig.cmake.in` and `tests/consumer/CMakeLists.txt`.
- [Plan 15](../plan/15-resource-aware-execution.md) has substantial local evidence
  notes. Its last checkpoint predates the broader 177-pass result recorded here.

## Prioritized remaining work

### 1. Finish the interrupted validation/retirement repair

**DONE 2026-09-15 (validated, no new code changes).** Evidence: `testUsdGenCudaExecutionStages`
10/10 `--repeat until-fail:10`, `testUsdGenCudaRbfAggregateAdmission` 10/10, full suite
**189/189, 0 failures, serial, no exclusions** (100.92s; one expected `StormSurgery` skip;
suite grew 179 -> 189 since handoff). Binary newer than all touched sources. Checkpoint
recorded in plan 15 (`2026-09-15 validation`). No budget/identity assertion weakened.

- [x] Inspect `runAutoRootVariant` in
  [testUsdGenCudaExecutionStages.cpp](../tests/testUsdGenCudaExecutionStages.cpp).
  The observed failure was the partial-root variant: correct rejection of a
  **6102-byte** literal-Length reservation, but `usedBytes` fell by **12 bytes**
  between baseline and assertion. Do not weaken the budget/identity assertions.
- [x] Two calls to `waitForCudaNativeRelayRetirementForTesting()` have been
   added by Terra, but **those edits are not built or validated**. They join relay
  worker graphs, not the separate deferred generation/consumer retirement
  service. Finish the missing generation-retirement drain before pressure
  snapshots, with evidence that all accepted stages are terminal.
- [x] Use the new aggregate fixture's explicit ownership/retirement pattern.
  `Queue::Drain()` and a terminal promise do not necessarily mean every local
  callback owner has been destroyed. `SameGeneration`/`AcquireGeometry` can
  enqueue consumer retirement too. Join actual work; do not guess a sleep or
  accept an apparently stable nonzero ledger.
- [x] Rebuild, repeat the failing test, then run the **entire** 179-test suite
   without exclusions. Record results and any genuine skip separately.

Plans: [memory/lifetime/retirement](../plan/15-resource-aware-execution.md#memory-admission-producer-lifetime-and-retirement),
[current admission checkpoint](../plan/15-resource-aware-execution.md#aggregate-rbf-implementation-and-first-native-evidence),
[testing and flake policy](../plan/10-build-dependencies-testing.md).

### 2. Complete aggregate CUDA RBF resource admission beyond linear chains

**DONE 2026-09-15 (validated, no new code changes).** Audit maps every bullet to
implemented code + green batteries: `FindLiteralRbfShape`/`ReserveLiteralRbfExecution`
dispatch (`cudaExecution.cpp:2115-2134`, DAG reserve at `:2244`, scatter at `:2513`);
double-Deform lineage re-verified structurally; per-Width/proof/Deform charging with
per-kind ledger assertions; LU workspace reuse. Proof batteries (all `--repeat
until-fail:5` green 2026-09-15): `RbfDagAdmission` (exact/below-budget, cold/warm,
retained-COW vs independent reference, `differ outside Width` fail-closed unequal join),
`RbfScatterDagAdmission` (Scatter/Grow capture route), `RbfTopologyDagAdmission`
(Length-trunk + C3-Grow DAGs), `RbfParamMapAdmission`, `RbfAggregateAdmission`,
`RbfValueDag`, `ScatterGrowRbfDag`; reference-rooted Deform DAG battery inside the DAG
suite (exact/below/cold/warm/ledger recovery). Full suite **189/189, 0 failures**.
Residual open end ("remaining parameter/map shapes") is bounded by the item-3
composition matrix (`testUsdGenCudaCompositionMatrix`, also 5x green).

- [x] Extend `FindLiteralRbfShape`/`ReserveLiteralRbfExecution` to fixed-cardinality
  source-rooted value DAGs containing Deform, Width and WidthBlend. Existing
  execution supports independent sibling Deforms; it correctly forbids applying
  surface motion twice along one lineage. Do not relax that semantic check.
- [x] Charge source geometry, named channels, source parameters and publication
  once; charge each distinct Deform cache/candidate and evaluator separately.
  Identical surface descriptors do **not** imply shared native cache allocation.
  Deduplicate inherited aliases, not private point/width producers.
- [x] Reuse selected-device LU workspace queries. Width adds output/staging/LUT/
  proof bytes; WidthBlend adds its output and, when required, the full non-width
  comparator's device/pinned status. Keep metadata/runtime eligibility identical.
- [x] Prove exact-budget and below-budget admission, atomic multi-branch RBF
  rollback, equal/unequal sibling joins, retained COW channels and cold/warm
  direct/async parity. Preserve supported non-`rbfSamples` Deform expressions;
  empty compiled programs are not expressions.
- [x] Then cover generated Scatter/Grow sources and topology-changing
  Length/Grow descendants, reference sources, and remaining parameter/map shapes.
  Existing per-allocation success is not whole-graph admission proof.

Plans: [latest recipe](../plan/15-resource-aware-execution.md#aggregate-rbf-implementation-and-first-native-evidence),
[RBF value DAG implementation](../plan/15-resource-aware-execution.md#cuda-rbf-immutable-value-dag-integration-2026-09-14-in-progress),
[RBF semantics](../plan/examples/rbf-deformation.md),
[deformation design](../plan/05-static-curves-and-deformation.md).

### 3. Close supported-composition and expression/map execution gaps

- [ ] Build a current capability/composition matrix from actual code, including
  legal pairs/triples, nested hierarchy, fan-out/fan-in, reference/map edges,
  source roles, topology changes and selected terminals. Distinguish invalid
  authoring from missing math and executor-only limitations.
- [ ] Implement remaining semantically valid combinations of declared-supported
  operators. A blanket “unsupported graph” diagnostic is not completion.
- [ ] Complete consuming-input runtime evaluation for numeric/bool controls and
  inherited controls across groom/primitive/point domains. Preserve typed
  broadcasting, deterministic RNG/stable IDs and versioned immutable inputs.
- [ ] Finish missing operators and their resource/COW contracts based on this
  matrix, not simply by adding names to the capability table.

Plans: [plan 15 objective gates](../plan/15-resource-aware-execution.md#phased-implementation-slices-and-objective-gates),
[hierarchy/CUDA required end state](../plan/14-hierarchy-cuda-implementation.md#required-end-state),
[operators](../plan/04-operators.md), [maps/expressions](../plan/07-look-maps-expressions.md),
[typed expression contract](../plan/examples/operator-expressions.md).

### 4. Finish Vulkan operator/control breadth and complete COW transport

- [ ] Extend the locally implemented Source/Width/WidthBlend/Length paths to
  remaining required controls, especially disabled controls, Length profiles,
  field/expression inputs and maps. Add missing native operators and compositions
  rather than CPU geometry fallback.
- [ ] Maintain exact provenance and owner checks for all point/non-point/named
  channels and topology revisions, including cancellation, rollback, retained
  ancestors, malformed/empty inputs and resource pressure.
- [ ] Preserve old shader capability rejection before allocation/dispatch.
  Current scalar envelope uses a distinct six-buffer, 56-byte ABI; legacy
  modules must not silently accept newer controls.
- [ ] Resolve the documented CPU/CUDA Length minimum/Cull oracle discrepancies
  before claiming broad backend parity. Test full packets, not only point counts.

Already implemented locally: literal Scale/Set/CutExtend KeepParam/Reparam,
minimum, deterministic randomization, scalar blend/mask envelope, Length Cull,
named-channel/rooted source transport and basic value DAGs. Do not redo these.
The integer binary32 helper preserves **final envelope interpolation**; it is
not proof of subnormal parity for all Vulkan geometry arithmetic.

Plans: [Vulkan envelope checkpoint](../plan/15-resource-aware-execution.md#vulkan-scalar-envelope-verified-checkpoint),
[Vulkan work/evidence ledger](../plan/15-resource-aware-execution.md),
[operators](../plan/04-operators.md), [maps/expressions](../plan/07-look-maps-expressions.md).

### 5. Prove real ready-branch GPU concurrency and broader CUDA Graph use

- [ ] Replace Vulkan's single active candidate/stage-index execution with
  dependency-ready per-task state, admission, cancellation and join ownership.
  Inspect `sourceWidthJob.{h,cpp}`, `deviceContext.{h,cpp}` and pipeline barriers.
- [ ] Remove unnecessary global execution barriers only with dependency-scoped
  synchronization proof. Multiple queues are one possible design, not a
  requirement: a single queue may overlap independent work if barriers permit.
- [ ] Record hardware overlap witnesses, not CPU worker counts or early
  submissions. Existing **CUDA Width-DAG overlap evidence already exists**;
  extend it to the remaining legal mixed branches/grooms without claiming that
  all CUDA work is currently serial or that all mixed operators already overlap.
- [ ] Extend/verify CUDA Graph specialization for eligible stable ready sub-DAGs:
  complete keys, charged/evictable graph memory, safe recapture, equivalent
  uncaptured fallback, no admission/allocation/publication inside capture.

Plans: [DAG/CUDA Graph rules](../plan/15-resource-aware-execution.md#dag-legality-and-cuda-graph-use),
[CUDA Width overlap witness](../plan/15-resource-aware-execution.md#cuda-width-dag-device-overlap-witness-checkpoint-2026-09-12),
[execution engine](../plan/03-execution-engine.md),
[scheduling research](../plan/research/G-evaluation-scheduling-and-batching.md).

### 6. Complete stock-OpenUSD GPU-resident tools/imaging/render handoff

- [ ] Establish and implement a stock-SDK route by which unmodified Storm
  consumes accepted immutable GPU generations without ordinary geometry D2H.
  Existing standard `basisCurves` publication/host arrays are not residency proof.
- [ ] Verify tool consumers, version acceptance, lease lifetime, last-good
  visibility and renderer retirement end to end with traces.
- [ ] Keep availability/residency gates explicitly closed until demonstrated.
  Do not restore retired patch-dependent bridges or custom renderer/rprim paths.

Plans: [GPU end-state examples](../plan/examples/gpu-execution.md),
[plan 14 stock-SDK revision](../plan/14-hierarchy-cuda-implementation.md#required-end-state),
[imaging](../plan/06-imaging.md), [tools](../plan/08-tools.md),
[plan 15 publication gate](../plan/15-resource-aware-execution.md#phased-implementation-slices-and-objective-gates).

### 7. Close global resource, cache, liveness and performance gates

- [ ] Audit remaining uncharged usdGen-owned native allocations and incomplete
  peak/time estimates. Preserve explicit unknown estimates; runtime/driver/
  cuSOLVER internals are covered by headroom, not claimed precisely tracked.
- [ ] Stress multi-groom admission, interactive/background fairness and aging,
  safe eviction/recompute, stale/cancelled version handling, pinned generations,
  device loss, callback re-entry, shutdown and resource-deferred outcomes.
- [ ] Audit asynchronous interactive paths for owner-thread stream waits,
  device-wide fences and unscheduled mutable state. Clarify production drain
  semantics if callers need stronger retirement quiescence than publication.
- [ ] Record reproducible device/config-specific latency, overlap, memory and
  soak evidence. Existing bounded mechanisms/tests do not establish all gates.

Plans: [scheduling/liveness](../plan/15-resource-aware-execution.md#scheduling-cancellation-liveness),
[memory/retirement](../plan/15-resource-aware-execution.md#memory-admission-producer-lifetime-and-retirement),
[performance gates](../plan/09-performance-and-benchmarks.md),
[build/test policy](../plan/10-build-dependencies-testing.md).

### 8. Validate portable packaging, additional drivers and release readiness

- [ ] Revalidate CUDA-on, CPU-only and Vulkan opt-in builds, tests-OFF installs,
  exported targets and installed consumers after shared-interface changes.
- [ ] Obtain other relevant Vulkan driver/platform evidence; the current NVIDIA
  Linux result does not establish portable synchronization/notification support.
  Preserve explicit unsupported capability outcomes and distinguish skips.
- [ ] Audit global backend registration/availability before enabling production
  Vulkan selection; injected-provider success is not renderer readiness.
- [ ] Reconcile the requirement-by-requirement completion audit in plan 15,
  update evidence and explicitly list gaps. Commit/push only when requested;
  do not accidentally include unrelated/unvalidated Vulkan work in CUDA commits.

Plans: [build/install/testing](../plan/10-build-dependencies-testing.md),
[performance evidence](../plan/09-performance-and-benchmarks.md),
[roadmap/release context](../plan/11-roadmap.md),
[plan 15 evidence protocol](../plan/15-resource-aware-execution.md#evidence-protocol).

## Resume commands and environment

Workspace: `/home/burkard/work/usdGen`; NVIDIA GB10 ARM64 Linux; CUDA 13 / sm_121;
stock OpenUSD at `/home/burkard/work/OpenUSD_26_08`. Existing build trees are
`build-codex` (CUDA), `build-engine` (CPU), and `build-vulkan`. Revalidate caches
instead of assuming a new machine has these paths. Ninja is available at
`/home/burkard/.venv/bin/ninja`.

```bash
git status --short
git diff --check

# After finishing the incomplete ExecutionStages retirement repair:
cmake --build build-codex --target testUsdGenCudaExecutionStages testUsdGenCudaRbfTransaction testUsdGenCudaExecutionPlan -j 4
ctest --test-dir build-codex -R '^testUsdGenCudaExecutionStages$' --output-on-failure --repeat until-fail:10
ctest --test-dir build-codex -R '^testUsdGenCudaRbfAggregateAdmission$' --output-on-failure --repeat until-fail:10

# Full suite; explicit paths avoid the previously observed schema-env failure.
cmake --build build-codex -j 4
USD=/home/burkard/work/OpenUSD_26_08 GEN=/home/burkard/work/usdGen GENBUILD=/home/burkard/work/usdGen/build-codex ctest --test-dir build-codex --output-on-failure -j 1

# Run separately from CUDA/Storm GPU tests.
cmake --build build-vulkan -j 4
bash scripts/run-vulkan-validation.sh build-vulkan
```

Inspect `build-codex/Testing/Temporary/LastTest.log` and `LastTestsFailed.log`
carefully: a later selective CTest run may replace earlier logs. The recorded
ExecutionStages failure remains unresolved even if an exclusion run is green.
