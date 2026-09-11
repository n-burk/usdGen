# Hierarchy, runtime expressions and GPU execution

2026-09-11. Binding user decisions; implementation and verification in progress.
This overlay wins over conflicting statements in `00`–`13`, including the
GPU exclusion in `11` §8. It does not retire unrelated requirements or gates.

## Required end state

1. The composed operator hierarchy under each Description's `Ops` determines
   execution: reverse-sibling post-order, like usdRig. Children execute before
   parents; lower siblings before upper siblings. Grouping scopes do not fork
   the stream. No authored `usdGen:input` or `usdGen:terminal` is required or
   authoritative. Internal dependency edges remain compiler-owned.
2. Every numeric/bool operator attribute can connect to a typed SeExpr output.
   Each attribute has `groom`, `primitive`, or `point` evaluation metadata,
   including inherited controls. Strings remain literal (tokens/assets are
   treated as string-like configuration). Programs compile with the graph;
   values evaluate **during execution** with the consuming operator's inputs.
   The shared variable registry, broadcasting and type rules are detailed in
   [examples/operator-expressions.md](examples/operator-expressions.md).
3. Operators and expression evaluation run on CUDA. Geometry and intermediate
   parameter fields stay in GPU buffers through operators, tools and rendering.
   Explicit bake/export may read back data; small control/diagnostic reads are
   allowed. Ordinary Hydra publication via host `VtArray` is not proof of this
   requirement. See [examples/gpu-execution.md](examples/gpu-execution.md).
4. Animation deformation is rest-bound RBF driven, with persistent GPU binding
   and solve state. Guide-motion transfer is a separate role; no per-frame
   neighbor rebinding or double application of surface motion. Required root
   locking, failure behavior and research distinctions are in
   [examples/rbf-deformation.md](examples/rbf-deformation.md).
5. The editable design example is
   [examples/operator-network.usda](examples/operator-network.usda). It is a
   schema discussion artifact, not evidence that the runtime implements it.

## Original scope remains required

The complete requirement registry remains `00-request-and-scope.md`, the
milestone contents and definitions of done in `11-roadmap.md` §§2, 6, and the
gate registry in `09-performance-and-benchmarks.md` §5, with overlay `13`'s
B-2/SI-12/SI-13 additions. In particular:

| Work family | Required artifacts/evidence; current completion is unproven |
|---|---|
| M0–M1 engine/imaging foundation | B-1/B-2 fences, graph/chunk/tile contracts, all notice/commit/session tests, chain placement, deterministic values and benchmarks |
| M2 static curves/deformation | CurveSource, SculptLayer, Freeze and actual RBF Deform; C3 contract, frozen reentry, rest deformation tests |
| M3 guides/clumps | GuideInterpolate, guide distribution/proximity, Clump; value and invalidation tests, stable rest bindings |
| M4 maps/expressions/look | Image, Ptex, Expr, Paint, Noise, Combine, GuideProximity maps; complete numeric parameter expressions; mask/ramp/space behavior; material terminals and reload |
| M5 tools | Ten brushes, gesture/undo/freeze flows, picking, overrides; nineteen C ABI entry points and app tests; GPU buffer access for tools |
| M6 instancing | Instance operator, prototypes/cards, tools and invalidation; T-INST-1/2 |
| M7 rendering/hardening | Storm and hdPrman, P0/P1/P2 motion, GPU graphics interop/lifetime, memory budgets, all release gates |
| M8 breadth | Every operator in `11` §2.9, progressive generation, measured in-place overlay and sculpt rebase; prior milestone gates remain green |
| Release artifacts | RC-1 through RC-11 in `11` §6.4, canonical workflows/docs, clean offline build, workstation and real RenderMan evidence, upstream reproducers |

Future simulation, collision and third-party operator ABI remain identified as
future work in `11` §6.3; RBF animation does not imply a physics solver. CUDA is
now required, not that section's deferred GPU tail. External issue filing or
workstation sign-off needs appropriate authority/access; implementation work
can proceed without claiming those criteria passed.

## Current integration checkpoint

- The pre-overlay CPU baseline configured and built in `build-codex` with
  OpenUSD 26.08 and RigExec disabled. This is build evidence, not a gate sweep.
- Schema generation, hierarchy transport, Stage-free live Hydra staging,
  isolated/shared sessions, and CUDA RBF foundations are being implemented and
  tested. Until integrated checks finish they are **in progress**.
- Integration evidence from this working session: `build-codex` built with
  `USDGEN_ENABLE_CUDA=ON`, `USE_GPU_NOISE=OFF`, and `USDGEN_WITH_RIGEXEC=OFF`.
  A focused run passed B-2 (including both gate fixtures), Stage/Hydra parity,
  CUDA RBF, session/isolation, adapter, population, snapshot, and expression
  descriptor checks. The live hierarchy reorder test initially failed on a
  retained order array; after replacement by a dynamic adapter data source,
  the same-index test exited 0. A separate selected T0/T1 run passed 33 tests:
  `ctest --test-dir build-codex -L 'T0|T1' -E
  'bench|SchemaUpToDate|Cuda|Hierarchy' --output-on-failure`.
  These are intermediate-tree results, not release-commit or complete gate
  evidence. Subsequent expression transport changes still require reruns.
- `ops/deform.cpp` still has a placeholder implementation. A standalone RBF
  library/test does not replace it or prove a groom renders correctly.
- Later integration checkpoint: `ctest --test-dir build-codex -L 'T0|T1'
  -E 'bench' --output-on-failure` passed **41/41** tests, including schema
  regeneration drift, expression transport, hierarchy, B-1/B-2, sessions,
  CUDA expression/RBF/deformation and existing imaging tests. This remains
  intermediate-tree functional evidence, not performance or release sign-off.
  The Stage oracle is now a separate translation unit rather than a macro
  inclusion of the production builder.
- CUDA oracle correction: the RBF/deformation tests previously used `assert`
  with side effects, so Release `NDEBUG` disabled significant work. Targets
  now force assertions on, and the deformation test uses always-on checks.
  Earlier Release passes are not accepted as CUDA validation. The corrected
  deformation test covers exact affine motion, nonlinear whole-curve root
  correction, zero/half/full groom/primitive/point envelopes, cross-stream
  completion, malformed offsets and non-finite fields. One earlier RBF run
  failed with CUDA out-of-memory under concurrent model-service memory use;
  a separate rerun and the subsequent 41-test run passed without resetting
  the GPU or stopping services. No performance conclusions follow.
- `exprEval.cpp` is excluded from the core build and references the wrong
  frontend API. A separate full SeExpr v3.0.1 core is now vendored under
  `thirdparty/seexprFrontend`, pinned to commit
  `6b0fc581bd2284aa5593eb19762795a6847049a7`; see
  [research/seexpr-cuda-frontend.md](research/seexpr-cuda-frontend.md).
  A private namespaced frontend now lowers an explicit scalar/vector subset
  to checked SSA instructions executed by a CUDA interpreter. Numerical tests
  cover the mock expressions, grain broadcasting, vector fields/constructors,
  constant indexing, changing-frame conditionals, malformed IR and strict
  bool/integer conversion. Local assignments, many SeExpr builtins, shaped
  arrays/matrices and exact full-range 64-bit arithmetic remain unfinished.
  Most importantly, this evaluator is not yet connected to the core operator
  scheduler: schema connections and descriptor validation still fail closed
  there. Standalone GPU execution is not a claim that the mock groom renders.
- Host publication and the old noise upload/readback path still exist.
  Persistent CUDA buffers are a foundation, not end-to-end GPU residency.
- The expanded source/generated schema audit covers **202 authored numeric
  attributes**, including role vectors, colors and arrays. It caught and fixed
  24 missing defaults that representative scalar checks missed. Array/ramp
  payloads default to groom evaluation; random ranges and directional/color
  vectors default to primitive evaluation. These defaults do not imply shaped
  arrays already have runtime lowering. The audit is now a CTest target.
- WidthRamp, root-anchored Length and straight Grow have device-only primitive
  implementations with numeric fields at all three domains, offset/finite
  validation and staged publication. These are not complete schema operators:
  masks, ramps beyond the primitive API and scheduler wiring remain required.
  Noise reports unsupported in this API; the previous hash-jitter placeholder
  was removed. Exact existing SeExpr float FBM helpers were extracted into a
  shared device header; legacy noise compilation was checked with
  `nvcc -arch=sm_121 --fmad=false`, not treated as an end-to-end noise gate.
- A subsequent non-benchmark T0/T1 run passed **43/43** tests with the new
  style and schema tests. Expression coverage now includes half conversion
  and scalar/vector results compared against the actual vendored SeExpr CPU
  interpreter in test-only code. Production evaluation has no CPU fallback.
- Refreshing Hydra value snapshots currently requires descriptor staging and
  full recompilation. Incremental value/surface refresh, frame sampling,
  session identity edits and multi-description ownership need dedicated tests
  and implementation. Existing performance claims do not cover these changes.
- The next integration checkpoint adds a real **single-CurveSource** CUDA
  session path, not just a standalone upload test. A device-aware consumer can
  explicitly opt into immutable device generations and acquire CUDA geometry
  leases. Stable IDs and every associated source channel are sorted together;
  constant widths expand and absent widths use the transported Description
  default. Empty sources and already-deformed cache provenance are represented.
  Last-good generations survive invalid geometry, unsupported operators and
  unavailable renderer admission. No host tile publication runs on this path.
  Leases retain older allocations across newer commits and session destruction;
  consumer reclamation currently synchronizes its stream rather than using a
  nonblocking retirement queue. Device/stream provenance is checked. Recovery
  from failed CUDA synchronization quarantines storage rather than freeing
  allocations with unproven completion; fault-injection coverage remains due.
- This does **not** finish CurveSource, tools, or the CUDA executor. Source-only
  commits currently upload anew; capture reuse, shared-channel revisions,
  multi-operator execution, connected parameters, resampling and CUDA surface
  rebinding remain to integrate. `onError` only passes when existing root
  bindings validate against a resolved surface. Unsupported rebind work fails
  explicitly. The current-frame rest fallback in the compatibility Hydra
  builder is marked and rejected by CUDA `useRest`; a proper Default-time C3
  adapter path is still required. Authored root frames are not yet supported.
  The stock renderer keeps device publication disabled, and its publisher has
  a second device-payload guard. A caller opt-in is a tool capability, not proof
  of a renderer graphics-interoperability implementation.
- Compiler/session hardening now preserves the previous compiled graph on
  factory/validation errors, retains expression/backend/default-width fields
  during descriptor copying, and prevents capture errors from publishing
  partial generations. Diagnostics remain observable after rejected commits.
  Strict dedicated-scalar transport errors are carried into compilation;
  unknown backend values cannot silently choose CPU reference execution.
- CUDA expression contexts now build owner maps and incoming arc lengths on
  the GPU, expose only initialized domain-appropriate fields, and require a
  successful validation finish before exposing inputs to an evaluator. Root
  validation caught worker-reported fixes absent from the actual source/test:
  a primitive ownership overwrite, unwritten point-only fields, invalid hairT
  endpoints, and an allegedly empty context that was actually a groom
  invocation. Direct regression checks now cover these cases, uneven-CV arc
  length, and extreme malformed offsets. Worker compile-only results were not
  accepted as execution evidence.
- An intermediate expanded run passed **48/48** non-benchmark T0/T1 tests,
  including source preparation, source/context CUDA components, neutral
  device-generation leases and the real CUDA session test. A separate fresh
  CUDA-disabled configuration built the `usdGen` target successfully in
  `/tmp/usdgen-cpu-check-4XIfVs` with `USDGEN_BUILD_TESTS=OFF`; this proves the
  core still builds without CUDA, not that the entire install/release passes.
  Subsequent admission/transport refinements require the final rerun recorded
  below. No performance, complete operator, renderer or release claim follows.
- Final rerun for this checkpoint: `cmake --build build-codex -j 6` and
  `ctest --test-dir build-codex -L 'T0|T1' -E bench --output-on-failure`
  passed **48/48**, followed by a successful rebuild of the CUDA-disabled
  `usdGen` target. The six-malformed-attribute regression initially failed:
  typed Hydra mapping suppressed incompatible values before builder validation.
  An independent dynamic adapter diagnostic now carries those errors. The
  regression verifies all six errors, a live repair notice, and cleared errors
  on the same scene index. Backend tests cover unknown tokens, the composed
  schema CUDA fallback, and a genuinely undeclared legacy backend. Root checked
  the source and actual executions; local Qwen and both Hivemind lanes supplied
  usable reviews during this checkpoint. All native workers were Luna.

## Validation discipline

Record exact commands/results against the current tree. Every operator needs
both value tests and notification tests; pulling an updated value alone cannot
prove a dirty was emitted. Test hostile authored legacy edges, composed child
reorders, inactive/load changes, edits after first publication, and upstream
deformation. Expression tests must cover all supported types/granularities and
invalid/non-finite results, not just the mock's float scalar.

GPU tests need identity, rigid/nonlinear RBF motion, rank-deficient bindings,
cross-stream lifetime, repeated frames without rebind, root locks, and actual
render/tool integration with transfer tracing. Quiesce inference before
performance measurements; functional results are not performance evidence.
Every gate in `09` remains outstanding until its actual threshold/protocol is
verified for the new implementation; old CPU numbers are historical only.

## Delegation

Root integrates and validates. Native workers use Terra or Luna, as requested.
One lane uses local Qwen (`qwen3.8-flash-next`); two use the Hivemind LMStudio
endpoint (`qwen3.8-27b`) for substantive implementation/review tasks. A model
request is only counted as used when its response was received and inspected.
Missing process handles permit a new request; observation timeouts do not.
The native session currently permits three workers plus root, not twenty
simultaneous native workers. No larger worker pool is claimed.
