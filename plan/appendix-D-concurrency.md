# Appendix D — Concurrency and CUDA Graph audit (from code, 2026-09-14)

Working document for TODO Item 5. No behavior is changed by this appendix;
every claim names the owning `file:line`. The implementation sequence in §7
is the binding order: per-task state first, barrier scoping second, overlap
witnesses third, Graph extension last.

## 1. CUDA execution lanes (deliberate boundary)

- One primary stream per workspace plus a fixed pool of 8 Width-branch
  streams (`cudaExecution.cpp:986`, `:21`). Handles are created with the
  workspace, never lazily, so a relay cannot outlive its stream.
- The primary stream remains the sole lane for source, Length, Deform/RBF
  and finalization (`:986-989`, `:7219-7224`). Only `taskDag` Width value
  nodes take branch streams (`IsCudaWidthValueNode`, `:7219`).
- Consequence: sibling Widths may overlap on hardware; sibling Deforms,
  Lengths and blends are stream-serialized today even when independent.
  Any overlap claim beyond Width must first move task kinds onto lanes.

## 2. Relay/slot ownership (already per-task)

- `OperatorRelayService` owns fixed slots with permanent callback
  identities; callbacks carry no job/candidate/workspace ownership
  (`:5954`, `:5952`). `Reserve(job, index, done)` hands one relay per
  operator invocation (`:5992`); source/finalization relays are separate
  services. TBB flow workers complete phases off-launcher (`:5980`).
- Relays already isolate per-task candidates (`OperatorWidthCandidate`,
  `OperatorLengthCandidate`, `OperatorRbfCandidate`, … `:5920`) and
  snapshot predecessor geometry at admission (`:5936`: no sibling may
  rebind `job.state.geometry`).
- What is NOT per-task: stream assignment (see §1), the workspace RBF
  caches (one `CudaRbfCache` per Deform node in `workspace.steps`, shared
  across jobs on that workspace), the rest-surface binding cache (shared
  read-only after bind), and the WidthBlend graph cache (workspace-shared,
  leased per use `:771`).

## 3. Shared native state hazards per operator kind

- Deform/RBF: per-operator cache/state (`CudaRbfCache::State` per Deform
  node, retired only at relay-launch boundaries `:689`). cuSOLVER legacy
  handles are per-binding with per-launch `SetStream` (`gpu/rbf.cu:221`,
  `:260`, `:451`); legacy cuSOLVER may still serialize internally, so
  concurrent solves must be witnessed, never assumed. LU workspace
  elements are queried per sample count and charged per Deform.
- Length: per-candidate compactor/scan temporaries (relay-owned);
  selected-device CUB scan query per admission. Kernels are
  concurrent-capable in principle; no witness exists.
- Width/WidthBlend/noise kernels: concurrent-capable on distinct streams;
  only Width has a witness (see §5).
- WidthBlend graph cache: single-slot workspace cache (`:728`),
  64 KiB conservative charge (`:729`), keyed by path, ordered input
  paths, point count, blend bits, algorithm version and grid blocks
  (`MakeWidthBlendGraphKey`). Keys do NOT name input value versions:
  safe only because the executable never observes generation-owned COW
  planes (stable slots + result copy-out, `:722`). Eviction counted,
  uncaptured fallback mandatory on miss.
- Parameter evaluators: per-step (`Step::parameters`), never shared
  across nodes; scalar readback packets are per-candidate. Safe.
- Test gates (`widthBranchGate`, overlap witness, install-failure
  atomics) are process-global test seams, never production state.

## 4. Host-side serialization inventory (CUDA)

- `cudaStreamSynchronize` appears at: workspace drain (`:1539`),
  per-stream drain (`:1541`), scatter-grow commit (`:4938`), relay
  failure paths (`:6453`, `:6515`), tile/bounds fences (`:8011`,
  `:8162`, `:8202`), source commit (`:8960`), scalar-readback D2H
  (`:9252`, necessary), and direct-path proofs (`:10516`). Failure-path
  and D2H waits are necessary; drain waits are lifecycle boundaries.
- No `cudaDeviceSynchronize` in the execution paths (verify by grep
  before changing any wait). DAG admission must never wait: the CUB
  workspace query and LU query are device-select + query only.
- CUDA Graphs may not contain admission, allocation, publication or
  retirement work (TODO Item 5 gate); the WidthBlend cache upholds this
  by construction (workspace-private slots, copy-out).

## 5. Overlap witnesses (existing and missing)

- Existing: Width-fan-out device witness (`gpu/widthOverlapWitness.h`,
  `cudaExecution.cpp:5759`): branch gates hold admitted launches,
  distinct branch streams required
  (`cudaOperatorAsyncWidthDistinctBranchStreamCountForTesting() == 2`),
  device-timestamped arrivals with a caller-bounded deadline (serial
  streams cannot deadlock the probe), negative control on
  dependency-ordered chains (`testUsdGenCudaWidthDag.cpp:1267`,
  `:1330`). Asserts `maxActive >= 2` only where
  `concurrentKernels != 0`.
- Extended: independent WidthBlend joins
  (`testUsdGenCudaRbfTransaction --blend-overlap`). Blends already take
  branch streams and gate/witness hooks via the shared `widthBranch`
  path, so no scheduler change was needed: arm after the four Widths
  complete, launch both blends on threads, rendezvous, observe. Absolute
  blend values (2.25/4.5) plus parity, ledger recovery, and a witness
  disarm hook for exact baselines.
- Missing: every other operator kind and every mixed branch. Extending
  requires (a) lane assignment for the kind (see §1), (b) a launch-site
  hook mirroring the Width gate/probe, (c) the same negative control.
  Deform solves additionally require proving no library-internal
  serialization (see §3); a `maxActive == 1` result there is a finding,
  not a failure, and must not be "fixed" by weakening the witness.

## 6. Vulkan execution (strictly sequential today)

- `VulkanSourceWidthJob` holds single candidate slots (`width_`,
  `length_`, `cull_`, `blend_`, `compare_`), one `base_`, one
  `stageIndex_`, and advances `FinishStage → ++stageIndex_ → BeginStage`
  (`sourceWidthJob.h:90`, `sourceWidthJob.cpp:424`). Independent
  siblings cannot overlap by construction.
- `DeviceContext` owns exactly one compute queue (`deviceContext.h`).
  Multiple queues are allowed by the TODO but not required: a single
  queue overlaps independent work if barriers permit.
- Barriers use `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` liberally
  (`lengthCompactionPipeline.cpp:24`, `lengthScalePipeline.cpp:241`,
  `:242`, `nonWidthComparePipeline.cpp:142`,
  `widthBlendPipeline.cpp:305`, `:324`, `widthPipeline.cpp:285`,
  `:299`). Inside a single command buffer these order correctly but
  forbid intra-queue overlap by construction. Narrowing them is only
  valid with a dependency-scoped proof per producer→consumer pair;
  several sites already pair precise access masks with the broad
  stages, which is where narrowing starts.
- Submission is per-pipeline-object with its own fence
  (`lengthScalePipeline.cpp:242`); completion flows through
  `VulkanCompletionService` watches with Reserve/Retire ownership,
  cancellation via `Superseded` states, and quarantine retention
  (`planExecutor.cpp`, `sourceWidthJob.cpp:341`). Join ownership is
  per-request (`requestLifetime`), not per-task.
- Per-task design must therefore introduce: per-ready-task candidate
  state (replacing the single slots), dependency-scoped barriers (or a
  second queue), per-task admission/cancellation, and per-task join
  ownership — in that order. Do not reorder the state machine before
  the barrier inventory above is converted to scoped proofs.

## 7. Implementation sequence (binding)

1. CUDA lane assignment: extend branch-stream eligibility beyond
   `IsCudaWidthValueNode` kind by kind, each with its §3 hazard
   disposition and a §5-style witness (Width first (done), then
   WidthBlend joins, then Length, then Noise; Deform last pending a
   cuSOLVER serialization verdict).
2. Vulkan barriers: convert each `ALL_COMMANDS` site in §6 to a
   dependency-scoped proof without changing behavior; record the
   proof per site.
3. Vulkan per-task state: replace single slots/index with
   dependency-ready task state plus per-task admission/cancellation/
   join, reusing the completion-service ownership pattern.
4. Overlap witnesses for every newly parallel lane (never CPU worker
   counts or submission order).
5. CUDA Graph specialization beyond single WidthBlend ops (stable
   ready sub-DAGs): complete keys (including value-version policy),
   charged/evictable graph memory, safe recapture, equivalent
   uncaptured fallback, and the no-admission/allocation/publication
   gate — each with stats-counter proof like the existing
   captures/replays/evictions/quarantines.
