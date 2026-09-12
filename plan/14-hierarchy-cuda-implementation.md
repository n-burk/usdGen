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
6. User revision: no application mutexes. Mutable state must have an explicit
   scheduled owner; dependencies and parallelism belong to the execution
   framework and CUDA streams/events. Mutex-protected methods, spinlock
   substitutions and a global serialized cook are not the target architecture.

## Scheduled ownership revision

This newer decision supersedes the mutex-based concurrency descriptions in
earlier checkpoints and `03`/`06`. Their functional tests remain necessary,
but do not prove the revised concurrency requirement.

- Each description has a short-running command/publication owner in a TBB flow
  graph. Requests carry immutable descriptor/input snapshots and increasing
  epochs. Accepting a new request does not wait for an old GPU cook.
- Compile/evaluation is separate scheduled work. Persistent mutable RBF and
  operator resources have one execution owner; independent descriptions and
  independent dependency branches can execute concurrently in the private
  arena and on device streams. Publication returns as a completion message,
  checks its epoch, then exposes an immutable generation. Superseded work may
  finish, but cannot publish and retains allocations through device completion.
- GPU leases own their consumer state directly. There is no shared map of
  active consumer tokens requiring a registry lock. A copied lease shares that
  single consumer state; its final release fences the stream and releases its
  retained parent allocations.
- Scene-index reads use immutable published data, never block on cooking and
  never invoke the state owner synchronously. Callback subscriptions and scene
  mutations are owner commands; notifications are ordered completion work.
  Registry changes use the framework's ownership/publication facilities, not
  independent mutex-protected mutable maps.
- Asynchronous APIs are primary. Compatibility waits are allowed only at
  external boundaries and must cooperate with the scheduler. Owner callbacks
  never wait on a command queued behind themselves; CPU/GPU completion is
  represented by dependencies, not by blocking the command node. Every CUDA
  work item explicitly binds its captured device; worker threads have no
  assumed persistent CUDA context affinity.
- Validation must cover simultaneous submissions, independent-description
  overlap, ordered publication, supersession while work is in flight, callback
  re-entry, retained leases and draining shutdown. Audit `libs/` for remaining
  application mutexes. Framework implementation internals are not represented
  as a claim of hardware wait-freedom.

Migration is in progress. At the initial audit, session, imaging, RBF cache and registry
locks were still present; they must be removed through ownership
changes, not simply deleted. The pre-revision tools tree passed 62/62 tests,
and four new CUDA targets passed initcheck/memcheck; those results do not
validate the upcoming scheduling and lease-ownership changes.

The first migration slice now provides `UsdGenExecutionRuntime` and
`UsdGenExecutionPipeline`. The pipeline uses separate serial command and work
nodes within a shared private TBB arena. Command acceptance and publication
are ordered; work for different descriptions can overlap. Non-coalesced owner
commands are distinct from supersedable work requests. Work returns a
publication closure; only the currently accepted epoch may execute it.
Cancellation is cooperative and does not imply CUDA completion. Callback
re-entry enqueues more work rather than waiting. Draining and destruction are
external ownership boundaries after submitters have stopped; concurrent
drainers are not supported. Framework dispatch exceptions are fatal to that
pipeline, not a promise that every queued callback can be recovered.

This is a tested scheduling foundation, **not yet the engine/imaging execution
path**. The consumer-token map in `cudaGeneration.cpp` has been replaced by
per-lease consumer handles. Those handles validate stream/device provenance,
retain parent revisions, and complete idempotently. Final release currently
fences the consumer stream synchronously; nonblocking GPU retirement remains
required. The new scheduler and lease API build with CUDA enabled and disabled;
the first post-change non-benchmark T0/T1 run passes **63/63** tests.

The next ownership migration must separate three kinds of state explicitly:

| Owner | Exclusive mutable state | Published/cross-owner values |
|---|---|---|
| Description command node | pending input commands, accepted epoch, edit reservation, staged revision, publication bookkeeping, subscriptions | copied descriptor/frame/context request; immutable generation/report/diagnostics |
| Description work node | compiler, mutable compiled graph, scheduler chunks, parameter fields, persistent RBF workspace | completed generation candidate and immutable routing/statistics data |
| Scene-index command node | membership, attachment identities, routing/subscriptions, notice batches | immutable scene lookup snapshot consumed by `GetPrim`/`GetChildPrimPaths` |

The current `UsdGenGraph` is noncopyable and includes mutable operator state;
wrapping it in `shared_ptr<const UsdGenGraph>` does not create a safe snapshot
while a worker retains mutation access. Extract routing/diagnostic values
instead. Likewise, old/new compiled plans must not independently execute a
shared mutable RBF solve cache. Reuse rest binding through the description's
single workspace owner, with immutable rest identity separate from posed data.
Completion payloads must carry the original groom attachment identity, not
only its path, to reject delayed callbacks after remove/re-add. Ordered notice
batches and frame/description pairing must survive the migration.

Final validation of this migration slice:

- `cmake --build build-codex -j6` and
  `ctest --test-dir build-codex -L 'T0|T1' -E bench --output-on-failure`:
  **63/63 passed**, including four concurrent host consumers acquiring copied
  leases of the same retained CUDA point revision. Each consumer uses its own
  device-selected nonblocking stream; final lease release fences queued D2D
  copies/test readback, and a retained lease survives all generation handles.
- `testUsdGenExecutionPipeline`: **100 consecutive runs passed**. A separate
  current-source build with `-fsanitize=address,undefined` also passed.
- `compute-sanitizer --tool initcheck --error-exitcode 1` and the equivalent
  `--tool memcheck` each reported **0 errors** for `testUsdGenCudaPicking`,
  `testUsdGenCudaPointOverride`, `testUsdGenCudaTopology`, and the updated
  `testUsdGenCudaTools`.
- The CUDA-disabled `usdGen` core rebuilt successfully in
  `/tmp/usdgen-cpu-check-4XIfVs`. This is a core build, not a CPU-only full-suite
  or release/performance gate claim.
- The new execution-pipeline and device-lease implementation files contain no
  application mutex or spinlock. Session, imaging, RBF and registry migration
  remains unfinished; the full library is not yet mutex-free.

### Immutable CUDA plans and scheduled workspaces

The next implementation slice removes the CUDA execution-plan and shared RBF
cache mutexes through resource ownership changes:

- `CudaParameterProgram` owns only immutable IR, bindings and CPU literals.
  `CudaParameterEvaluator` owns per-execution device contexts, programs and
  fields. Independent evaluators can share one program without overwriting
  each other's output. The old `CudaParameterPlan` is a compatibility facade,
  not the new compiled-graph representation.
- `UsdGenCudaExecutionPlan` owns a copied authored descriptor, immutable
  parameter programs, LUTs and prepared surface inputs. Execution consumes
  that exact plan snapshot, not an independently supplied same-length graph.
  No runtime GPU allocations or mutable RBF state live in the plan.
- `UsdGenCudaExecutionWorkspace` owns one description's parameter evaluators,
  RBF resources and nonblocking CUDA stream. Execution selects its captured
  device on every worker invocation, uses that stream throughout, and restores
  the previous thread device. A workspace cannot execute another description;
  the legacy session allocates a new workspace when its description changes.
- Replacing a plan reconciles the workspace by operator path and exact rest
  key. Identical rest data retains the factorization/binding identity while
  posed inputs are solved again. Changed rest data invalidates the binding.
  Two workspaces never share mutable RBF solve state, even when they use the
  same immutable plan. Stats are immutable snapshots of completed work.
- `UsdGenCudaExecutionQueue` is the actual asynchronous CUDA entry point using
  the TBB pipeline: one serial work owner per workspace, independently queued
  descriptions, epoch-checked publication, per-request diagnostics, and atomic
  immutable result snapshots. It does not invoke the legacy session mutex.
  Completion callbacks may submit follow-up work. Failed/superseded work does
  not replace the last published snapshot. Shutdown drains the pipeline before
  destroying its snapshot storage or device workspace.
- `UsdGenGraph::RoutingSnapshot()` copies owning routing metadata, not a const
  alias to the mutable graph. `UsdGenDirtyRouter` can rebuild from this snapshot
  after the original graph is replaced/destroyed. The current compatibility
  overload extracts a snapshot; live imaging callback payload integration
  and removal of its graph rereads remain next steps.

At this checkpoint, this did not finish the concurrency revision. The legacy engine session and
imaging entry points still use their old synchronization, although their CUDA
execution now uses private workspaces. The new asynchronous queue is not yet
the usdview/imaging/tool-session dispatch path. GPU kernel helpers still use
synchronous validation/completion fences and ordinary allocations; explicit
nonblocking streams are not evidence of measured GPU overlap, interactive
latency or nonblocking device retirement. Authored pose/value refresh currently
produces another plan snapshot; incremental refresh/compile reuse remains due.
CUDA context-loss/allocation-failure injection is not covered by these tests.

Validation of this slice: the full non-benchmark T0/T1 suite passes **64/64**.
The new queue test checks shared-plan/independent-workspace numerical isolation,
frame-dependent expressions, rest-binding reuse across plan replacement,
immutable input/output retention, execution failure and recovery, concurrent
submissions, latest-epoch publication, callback re-entry, cancellation and
shutdown. The updated routing contract test verifies an original path/value
classification after graph destruction. The CUDA-disabled core also builds.
`testUsdGenCudaExecutionQueue` passes both CUDA memcheck and initcheck with
**0 errors**, including queued-request shutdown. These are functional evidence,
not full-plan completion or release/performance sign-off.
The queue test also passed **30 consecutive runs**; the updated CUDA RBF session
and parameter tests each passed memcheck and initcheck with **0 errors**.

### Scheduled engine sessions and paired imaging payloads

The engine `UsdGenSession` now uses the execution framework rather than its
former commit/pending mutexes:

- A short command owner accepts immutable descriptor snapshots, dirties,
  context/device-publication changes and tool reservations. A separate serial
  work owner exclusively holds the compiler, graph, scheduler and CUDA
  workspace. An occupied cook does not prevent owner commands being accepted.
- `CommitAsync` is the primary execution API. `CommitSnapshot`, `Commit` and
  synchronous setters are external compatibility adapters using independent
  TBB reply graphs (`reserve_wait`/`release_wait`), not mutexes, futures or spin
  waits. Callback/work-node synchronous waits are rejected; callbacks can post
  mutations and submit follow-up work. Default pipelines share an arena;
  explicit thread limits still select private runtimes. This is scheduling
  ownership evidence, not a measured global CPU/GPU concurrency-budget gate.
- Input mutations immediately invalidate accepted work. Pending dirt remains
  until successful current publication. The cooker compares immutable input
  identity and the last published/cooked work epochs: stale or failed private
  caches cannot become the baseline for a later incremental evaluation.
  Value-only benchmark invalidation does not require recompilation.
- Built-in operator registration is completed by registry construction, not
  repeated mutable insertion on each compile. Concurrent cold compiles only
  read it. Custom registration remains an explicitly startup-only API; dynamic
  runtime plugin registration is not implemented by this change.
- Candidates are numbered from the last public generation, and stats start
  from that same baseline. Rejected/superseded candidates consume no public
  generation IDs or commit counts. A failed current attempt exposes its
  diagnostics and compiled graph metadata with the retained last-good
  generation and empty dirty report; it completes as `Failed` and stays dirty.
- One immutable session snapshot pairs geometry, report, diagnostics, stats,
  detached routing/graph metadata, node stats and CUDA binding stats. Public
  `Graph()` now returns an owning metadata value, not a reference to a mutable
  compiled graph. Shared-pointer atomic publication is library-managed; no
  claim is made that the standard library implements it lock-free.
- CUDA device selection is captured on the submitting thread and explicitly
  passed to private workspace creation/execution. Existing tool reservation,
  revision-identity/topology checks and press-time override publication now
  execute on the command owner. Shutdown closes acceptance and delivers
  terminal callbacks while session state and retained resources still exist.
- Imaging consumes the paired result, including routing, rather than rereading
  a live graph/report. Each groom router is an immutable snapshot. Republish
  verifies the exact attachment instance (not just the same path), including
  rechecking membership when swapping the published map.

This is **not** the end of the no-application-mutex migration. Imaging's stage,
callback, store, scene-state/frame and auxiliary registry guards remain; their
owner-lane migration must preserve notice ordering and detach/re-adopt behavior.
In particular, the attachment check is not proof of notice ordering across
concurrent removal after the map swap. Stock Hydra device interop, asynchronous
device retirement, full tool dispatch, failure injection and performance gates
also remain open. The original M0–M8/release scope below is unchanged.

Validation: the full CUDA-enabled build and all **65/65 non-benchmark T0/T1**
tests pass. The new async-session test covers a deliberately held capture,
same-path edits accepted during work, supersession, numerical width output,
paired snapshots, failure/retry, concurrent external waits, callback re-entry
and shutdown. It passed **100 consecutive runs**. The pipeline reply tests
cover eight concurrent waiting callers, exceptions, forbidden callback waits,
immediate/idempotent replies and queued-command shutdown; a standalone
AddressSanitizer/UndefinedBehaviorSanitizer run also passed. The CUDA-disabled
core builds. CUDA tools memcheck and RBF-session initcheck each reported
**0 errors** after the session migration. These are functional checks, not
release or full-plan completion claims.

### Imaging command ownership follow-through

The next checkpoint removes imaging-session stage/republish mutexes and
integrates the asynchronous engine rather than waiting from an owner node:

- Engine `CommitRequest` atomically applies optional immutable description and
  context inputs with frame/reason capture. An optional captured CUDA device
  survives relay through another owner; the original caller, not an arbitrary
  imaging worker, supplies the device. `PostDirty` is callback-safe.
- Each imaging session has a serial command owner for staging, frame/context,
  callback registration and publication. It submits one bundled engine request
  and receives one immutable completion message. No imaging command waits for
  an engine cook. Legacy synchronous commits use an external reply boundary;
  callback code must use `CommitAsync` and cannot call synchronous shutdown.
- Store-level time/commit operations submit every live description before
  waiting on a framework completion barrier. Their frame is part of each
  immutable request, not a separate `SetTime`/`Commit` pair. A held-capture
  test verifies that two descriptions both start before either is released.
- Public imaging-handle lifetime is separate from scheduled state. Dropping
  the last handle inside a callback schedules retirement on a separate TBB
  graph. Closing stops acceptance, waits for all engine replies to relay back,
  completes accepted requests, and only then destroys the owner and engine
  reference. Explicit `Shutdown` and `DrainRetired` are single-external-caller
  boundaries. Concurrent shutdown is rejected, not implemented as a waiting
  lock. Framework relay-dispatch failure is fatal rather than a silently lost
  completion; allocation-failure recovery remains unimplemented.
- The retirement graph is constructed before the process-global session store
  so it outlives stored handles during process teardown. The first regression
  run caught the inverse static-destruction order; the fix then passed 30
  repeated session-process shutdown runs.
- Groom attachment construction and callback registration happen outside the
  scene membership guard. Per-root adoption tickets let removal cancel an
  in-progress candidate; a late candidate cannot overwrite a same-path
  replacement. Removal erases membership first, then unregisters/detaches
  outside that guard. Input dirty delivery no longer synchronously enters the
  engine while holding scene membership state.

Validation includes the full CUDA-enabled build, **66/66 non-benchmark T0/T1**
tests, and **100 consecutive runs** of the new async-imaging test. That test
checks per-request frame/width pairing, owner-ordered unregister and callback
re-entry, last-handle release on a callback, 48 queued shutdown completions,
retained geometry/routing, removal and same-path re-adoption while the old
candidate is held, and independent store-batch execution.

A separate CUDA-disabled AddressSanitizer/UndefinedBehaviorSanitizer build
ran async-imaging, sessions and population with `detect_leaks=0` and
`halt_on_error=1`. This is address/UB evidence only: the initial leak-enabled
run reported allocations in OpenUSD registry/path-cache and diagnostic stacks
(2343, 3367 and 32683 bytes across the three processes). Leak-clean teardown
is **not verified**, and no leak suppression was added to the repository.

The session store, remaining scene-state/frame handling, timing/test-hook and
warning registries still need scheduled ownership. Scene-notice ordering
through removal after a publication map swap, full asynchronous scene/tool
dispatch, device retirement/interop, fault injection and release/performance
gates remain unfinished. This checkpoint does not complete the no-mutex
revision or shrink the original milestone scope.

### Store and auxiliary registry ownership follow-through

The next checkpoint removes the store and auxiliary registry application
mutexes, without serializing description cooking behind those registries:

- The session store owns membership on a short command lane and publishes an
  immutable key-to-strong-handle snapshot. `Find` and `LiveSessions` never
  await that owner. Same-key concurrent attachments return one canonical
  session. Async detach carries the expected session identity; an old release
  cannot decrement a same-key replacement. This is not an idempotent lease
  API: every successful attach still requires exactly one balanced release.
- `AttachAsync`, `DetachAsync`, `SetTimeAsync` and `CommitAsync` are primary
  scheduled requests; synchronous mutation/commit adapters reject owner
  callback re-entry. Store-wide frame/context dispatch is one owner command,
  which submits every description without waiting for its cook. Requests
  carry the original caller's CUDA device through both owner relays. Newly
  attached sessions inherit explicit store frame/context and app-driver state.
- Batches retain their session handles until all reply closures release them.
  Store shutdown waits for this ownership release, not just the user callback:
  the last handles must enqueue session retirement before the process-global
  retirement queue can drain and disappear. A subprocess test returns from
  `main` with a deliberately held completion and verifies all 64 individual
  callback counts after store teardown. Dispatch failure in a required
  lifetime relay remains fatal; general allocation-failure recovery is open.
- The test-hook registry has scheduled mutations and immutable weak-handle
  snapshots. Queued removal compares weak-control identity, not a recyclable
  raw address, and the registry owner never promotes/destroys scene indices.
  Its owner drains before registry storage is destroyed. Test-only external
  drains provide visibility boundaries; callbacks cannot use those waits.
- The timing bridge retains plain shared state in process-owner commands, so
  dropping a bridge from its own timing hook does not destroy a pipeline on
  that callback. Hook changes and copied stats/cursor updates are ordered.
  The Hydra authoring-caller diagnostic is distinct from actual scheduled
  engine execution. Unknown-route warning deduplication also has an owner.
- The adapter mapping cache uses the framework's append-only concurrent map
  to publish immutable per-schema entries. Duplicate builders return the
  canonical insertion; published values are never edited or erased. This
  preserves returned-reference lifetime without an application accessor lock.

Validation: full CUDA-enabled build and **69/69 non-benchmark T0/T1** tests
pass. Store/registry/adapter checks cover concurrent attachment, callback
queries/re-entry, same-key replacement, inherited-frame execution, retained
snapshots, timing-hook destruction/re-entry, queued weak-registry mutation,
and canonical mapping references during concurrent insertions. The four-test
store/exit/registry/adapter stress run passed **50 consecutive executions per
test**, including the held-completion process-exit case. A separate
CUDA-disabled ASan/UBSan build passes the six async-imaging, store, pending-exit,
registry, adapter and population checks with `detect_leaks=0` and
`halt_on_error=1`; the previous leak-clean limitation remains unchanged.
CUDA tools memcheck and initcheck each reported **0 errors**.

The remaining explicit application mutex in `libs/` is scene-index
`_stateMutex`. Its membership, notices, adoption continuation and publication
ordering still need a real owner/snapshot migration. The unused frame mutex
and never-read frame fields were removed, **not** counted as an implemented
scene-global frame channel. Upstream scene reads/adoption and notice forwarding
are not yet fully asynchronous. Render-context density selection remains open:
the cooker currently stores context, but these tests do not demonstrate its
density effect. GPU retirement/interop, full tools/operators/maps, fault
injection, performance gates and all original release requirements remain.
Framework-internal synchronization and standard-library atomic shared-pointer
implementation details are not claims of hardware wait-freedom.

### Scene-index owner and lifetime follow-through

The scene membership guard is now replaced by a per-index framework command
owner. Reads load one immutable membership/tile snapshot; they never initiate
population, cook, or wait for that owner. Ordinary upstream Hydra queries
remain on the read/notice caller and retain upstream affinity requirements.
The implementation does not claim those upstream queries are wait-free.

- `New` eagerly captures population. Notice callers capture owning graph
  descriptors and the original CUDA device before scheduling mutation. A
  reserved ingress sequence plus per-path events/tombstones suppresses delayed
  obsolete captures without blocking unrelated descriptions behind one slow
  source query. Suppression history is pruned when the contiguous completion
  watermark proves that no older capture can still arrive.
- Attachment, descriptor staging/cooking, callback unregistration and exact
  detachment are asynchronous relays. Each scene has its own command owner;
  description cooks retain independent engine work lanes. `Synchronize` is
  an explicit external watermark/reply boundary, not a render-read operation
  or a promise to await future store-driven requests. It includes accepted
  ingress's attach/cook/detach/subscription acknowledgements.
- Publication rebuilds the tile set from the full immutable generation,
  checks exact membership identity and monotonic generation, swaps before
  notifying, and forwards notices on the same owner. Skipped generations use
  full invalidation for surviving tiles instead of trusting an intermediate
  dirty report. A type resync away from Groom/Description retires membership
  even without a separate Removed notice. Authored prim data wins queries at
  colliding paths; complete authored/synthetic collision notice semantics
  still need dedicated coverage.
- Scene destruction from an observer callback schedules plain State
  retirement. Process shutdown closes scene subscriptions, drains source
  callback frames and destroys scene pipelines while holding live States.
  A pre-reserved per-State framework retirement record covers the gap between
  the last strong reference and enqueueing its deletion. Quiesced States can
  outlive the process service through a static public index handle; their
  later deletion does not access the destroyed service/test-hook registry.
  External engine/store drains exist only for quiescent shutdown/test use.
- Throwing observers are diagnosed without stranding completion holds.
  Unexpected allocation/framework failures during nontransactional ownership
  updates are fatal; allocation-failure recovery is not implemented. No
  application mutex or spinlock remains in the library C++/CUDA sources.
  Standard-library atomic shared-pointer and framework internals are not
  claims of hardware lock-freedom.

Validation so far: full CUDA-enabled build and **72/72 non-benchmark T0/T1**
tests pass. Five CUDA-disabled ASan/UBSan checks (scene owner, static process
exit, actual tile publication, async imaging, population) pass with
`detect_leaks=0` and `halt_on_error=1`. The publication test verifies exact
`.02 -> .08` widths through a legacy CPU uniform fixture, a held synthetic
notice with reads still available, and a second independently executing scene.
It is not GPU renderer-interoperability evidence. The CPU reference Width
operator still does not evaluate ragged chunks; the fixture explicitly
resamples uniformly and does not introduce a production CPU fallback.
The four async-imaging/scene-owner/scene-exit/publication tests also passed
50 consecutive runs each, and 20 each in the ASan/UBSan build. The owner test
additionally drops its last external scene-index handle inside a synthetic
addition callback and verifies balanced retirement.

This was not completion of the execution/imaging milestones. At that checkpoint capture
rescans the input and rebuilds descriptions on every notice; incremental
source capture, filtering/coalescing and the actual performance thresholds
remain required. The scene retirement registry retains weak bookkeeping
until process shutdown. GPU renderer interop, render-context density effects,
complete operators/maps/tools, robust fault recovery, collision-notice
coverage and all original release requirements remain open. Local Qwen gave
an inspected visible audit; the two Hivemind audit requests returned no usable
visible answers and are not counted as completed model reviews.

### Incremental source and operator capture follow-through

Dirty-only notices now select affected groom roots from an immutable,
owner-published dependency catalog instead of discovering the entire input
scene. A recording facade tracks builder queries, including absent targets
and GeomSubset parent meshes; expression and reference paths transported by
adapter aggregates supplement those queries. Owning-namespace dirties and
exact/ancestor dependency dirties select a root. Unrelated dirty notices
perform no source reads and do not recook an unrelated session.

- Initial and structural notices still perform full discovery. Incremental
  selection requires the catalog's contiguous applied-capture watermark to
  equal the immediately preceding ingress sequence. A delayed earlier query
  forces conservative discovery. Failed captures retain existing membership
  and mark the catalog untrusted until a later successful full discovery;
  sequence ordering prevents an older successful packet restoring trust over
  a newer failure. Partial captures never prune unselected members.
- A new opaque immutable operator-value cache supports explicit reuse within
  the same input scene. Description identity, composed order and sample
  offset must match; dirty equal/ancestor paths force operator reads. Full
  topology captures still pull every mapped operator property (S14).
  Operator validation errors and guide-role contributions survive reuse.
  Hierarchy inputs and inherited surfaces are rebuilt, not cached as authored
  dependencies. No live Hydra handles enter the cache or scene owner.
- The scene caller disables node reuse when the absolute frame changes.
  Hydra sampling itself remains at shutter offset zero (current stage frame),
  not at an absolute frame offset. This distinction is documented at the API.
  The graph descriptor header now directly includes its required `GfVec2f`
  definition instead of relying on transitive client includes.

Validation: full CUDA build and **74/74 non-benchmark T0/T1 tests pass**.
The new `testUsdGenCaptureCache` checks exact upstream read counts, mapped
values, old-descriptor immutability, inherited surfaces, diagnostic retention
and correction, guide role removal, topology reorder, and sample-offset/full
capture invalidation. `testUsdGenIncrementalCapture` checks unrelated/local
selection, external mesh/subset and curve dependencies, operator reuse and
rereads, relationship retargets, failed-capture recovery, and a deterministic
delayed-capture/full-discovery case using separate framework reply graphs.
The actual-stage publication test now checks animated widths `.04` and `.12`
at frames 1 and 2 in addition to its static edit and lifetime cases. Five
CUDA-disabled ASan/UBSan tests (both capture tests, publication, async imaging,
population) pass with `detect_leaks=0` and `halt_on_error=1`.
Incremental capture and animated publication also passed 30 consecutive
runs each in the CUDA-enabled build and 20 each in the sanitizer build.

This does not establish the performance gates: geometry/maps/description
sections are still rebuilt for affected descriptions, structural changes
still scan the full scene, and cache/snapshot copying and coalescing need
measurement and further work. No GPU graphics interop or production CPU
fallback is introduced. The original milestone/release scope remains open.
Native Terra/Luna lanes supplied implementation/review assistance; local Qwen
returned visible review suggestions which were checked against the actual
Hydra contracts. The two Hivemind requests again exhausted their reasoning
budgets without visible answers and are not counted as successful reviews.

### CUDA to Storm-owned OpenGL buffer bridge

`UsdGenCudaGlComputation` now implements the actual `HdStComputation`
interface. The graphics commit caller allocates an unpublished Storm BAR,
registers this computation with `HdStResourceRegistry::AddComputation`, and
checks `Succeeded()` after registry commit before exposing the destination.
The bridge registers the backing `HgiGLBuffer` with CUDA, maps it, copies
device-to-device with the BAR byte offset plus resource field offset and
resource stride, then unmaps/fences/unregisters. It supports points, rest,
widths, hairT, curve offsets, stable IDs, root primitive IDs and root UVs.
Stable IDs are copied as two exact uint32 words, not converted to float.

This follows the local OpenUSD 26.08 `HdStComputation`, resource registry and
`HdStCopyComputationGPU` layout contracts. NVIDIA specifies the graphics/CUDA
ordering guarantees of [map and unmap](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__INTEROP.html)
and [OpenGL resource registration](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__OPENGL.html).
The registry's ordinary CPU `HdBufferSource` upload is not used for generated
channels; a CUDA address is never disguised as a host array.

- GL/CUDA device compatibility, backend, channel metadata, exact tuple/count,
  stride, signed offsets, arithmetic overflow and mapped allocation capacity
  are checked. Registration does not use write-discard: other fields/ranges
  may share the same GL buffer and must survive the copy.
- The source generation and its consumer lease remain alive through transfer
  completion; the stream is destroyed only after lease completion. Caller
  CUDA device selection is restored. Once copied, the independently owned GL
  allocation remains renderable after the CUDA source is released.
- No application mutex or geometry readback is introduced. This initial
  bridge is an **external graphics-commit boundary**, not the asynchronous
  steady-state renderer implementation: it creates/registers/fences per
  transfer. Engine command-owner execution is explicitly rejected. An unsafe
  unmap/fence/unregister failure is fatal; ordinary validation/copy failures
  return an error and do not authorize publication. Partial copies do not
  have rollback semantics, so callers must use unpublished destination BARs.
- CUDA builds export `USDGEN_HAS_CUDA_GL_INTEROP` and the public HdSt link
  dependency through the imaging CMake target. CUDA-disabled builds omit the
  implementation and continue to build without the toolkit dependency.

`testUsdGenCudaGlInterop` is a real T2 handoff test: an actual CUDA Width
operator produces widths for two ragged source curves, all eight channels
are copied into Storm-allocated buffers, and GLSL validates their contents
before rasterizing hair strips from GPU offsets/points/widths. It exercises
adjacent aggregated ranges and padded interleaved fields, checks exact 64-bit
IDs, rejects invalid inputs/tuples/command-owner calls, and redraws after CUDA
source retirement. Only final framebuffer pixels cross to the host. Measured
green coverage is 304/456/760 pixels for widths .1/.2/.3, with zero error-red
pixels; the earlier thin result remains unchanged after later copies.

Validation: full CUDA build and 74/74 non-benchmark T0/T1 tests pass. The
interop test passed 50 consecutive runs, then 20 more with a clean-Tf-error
assertion enabled; CUDA Compute Sanitizer memcheck
reports zero errors. The CUDA-disabled ASan/UBSan incremental-capture and
publication checks pass. One earlier rerun failed in source setup before the
handoff; subsequent stress runs did not reproduce it. More specific source
diagnostics are now present; no root cause or general fault-recovery claim is
made for that observation.

Broader T2 validation is **not green**: 4/6 selected non-benchmark tests pass;
`testUsdGenStormLook` and `testUsdGenStormMaterial` fail. Logs include notices
arriving on background threads during Hydra `SyncAll`, and a missing
`surfaceShader` link in the dual-material case. These are open integration
failures, not waived gates. The scene owner needs the framework's
`asyncAllow`/`asyncPoll` rendering-thread publication protocol (and correct
behavior for clients that never opt in). Actual BasisCurves publisher wiring,
GPU topology adaptation/culling/motion samples, persistent asynchronous
resource retirement, multi-device tests, hdPrman ingestion and performance
gates also remain required. This test draws directly from Storm's resources;
it does not claim stock Storm BasisCurves consumes a device generation yet.

### Frontend publication and asynchronous shutdown follow-through

The scene owner no longer calls Hydra observers from its TBB worker. It
queues immutable snapshot/notice packets through a TBB concurrent queue.
The serialized Hydra frontend installs the matching synthetic snapshot
immediately before delivering each packet's removals, additions and dirties.
The owner/capture catalog and frontend-visible snapshot are distinct: a
background cook cannot silently change generated geometry before its notice.
Authored input prims still pass through the upstream scene index; this is
not a historical snapshot of the entire input scene.

- Without `asyncAllow`, each upstream notice boundary waits for its causal
  owner/attach/cook work and delivers on the calling frontend thread.
- After opt-in, `asyncPoll` publishes already-ready packets without awaiting
  cooking. An acquired ready-count watermark fixes the batch at poll entry;
  new owner packets cannot extend that poll indefinitely. This is a finite
  batch, not a measured frame-time budget or a bound on observer cost.
- Explicit frontend `Synchronize()` waits and delivers. Observer re-entry
  into that explicit wait is rejected. Reentrant default-mode edits queue
  behind the current packet and are flushed by the outer frontend turn.
- Public index references are pinned across frontend delivery, including
  callbacks which release the last external reference. Queries read immutable
  snapshots without owner waits. No application mutex/spinlock is introduced;
  framework queues and library-managed shared-pointer publication are not
  claimed to be universally lock-free internally.
- A background-only store/tool republish has no legal spontaneous delivery
  point for a non-async host. Such a host must call `Synchronize()` or cause
  another frontend ingress. Async hosts poll. Frontend operations are
  serialized by the host; concurrent read-only queries remain supported.

`testUsdGenAsyncScenePublication` covers opt-in, caller-thread identity,
default-mode reentrant edits, removal-before-replacement ordering and
synthetic namespace visibility. The real-stage publication test additionally
keeps width `.12` visible before polling and obtains newly cooked width `.16`
through `asyncPoll` alone, rather than testing only structural population.

Sanitizer stress exposed an additional exit-time use-after-free in
`Graph::RoutingSnapshot`: CurveSource's lazily initialized static parameter
vector was destroyed before pending asynchronous work finished. CurveSource
and Deform parameter tables now belong to the operator instance. CPU
Width/Noise/Grow/Length evaluation tokens likewise have operator lifetime;
unused flat-ramp static vectors are removed. Compiler classification tokens
are initialized with the library before runtime services, preserving cached
token comparisons while ordering destruction after service shutdown. The
empty graph-output fallback is graph-owned. The real-stage test deliberately
leaves a final async frame edit pending when it exits. This does not remove
the host's existing obligation to stop new submissions before shutdown.

Validation after these changes: the full CUDA Release build and all 75
non-benchmark T0/T1 tests pass. All six selected non-benchmark/non-Surgery T2
tests pass five consecutive runs each, including the previously failing
StormLook and StormMaterial tests. The seven targeted CUDA-disabled
ASan/UBSan tests (async imaging, async scene publication, incremental capture,
scene exit, scene ownership, real-stage publication, Width) each pass 30
consecutive runs with `detect_leaks=0` and `halt_on_error=1`. The instrumented
Graph test passes its structural assertions but fails its 0.2 ms Release
timing threshold (43.65 ms); that timing test is not claimed green under
sanitizers. KernelDeterminism passes one instrumented run (73 seconds); its
second redundant stress repetition was deliberately terminated, not counted
as a pass. No production performance/release gate is waived by these checks.

Terra/Luna supplied bounded implementation and review assistance. Local Qwen
returned visible but generic/speculative reviews that were checked against
the actual source rather than accepted as proof. Both Hivemind review lanes
again returned empty visible answers; those calls are not successful reviews.

### Remaining native Storm integration boundary

Inspection of the local OpenUSD 26.08 source establishes that adding retained
scene-index data sources alone cannot connect the current CUDA BAR bridge to
stock BasisCurves. `HdStBasisCurves` is final (`hdSt/basisCurves.h`), and its
`_PopulateVertexPrimvars` method owns the computation/spec/BAR setup and
`HdStResourceRegistry::AddComputation` calls (`basisCurves.cpp`). Stock GPU
external computations use Hgi shader computations and input BARs; their
primvar descriptor is not an arbitrary CUDA allocation import API.

The candidate integration needs a renderer-side extension at that BAR
computation boundary, plus immutable device-generation publication from
usdGen. A static-topology source could retain its original authored CPU
counts while all deformed points/widths remain GPU-resident. That is only an
intermediate slice, **not** the required dynamic-topology end state:
`HdBasisCurvesTopology` currently owns CPU `VtIntArray` counts/indices, and
`HdSt_BasisCurvesTopology::GetPointsIndexBuilderComputation` constructs CPU
index arrays. GPU-generated/compacted offsets cannot be read back and renamed
"metadata" to claim compliance. Dynamic topology requires a GPU-native index
generation/consumption path and coherent draw-count, range, culling and
topology-identity handling. No OpenUSD dependency source was modified during
this investigation; the existing direct-BAR draw test is not stock
BasisCurves integration proof.

### GPU-native curve index generation

`gpu/curveIndices.{h,cu}` now generates Storm-layout index records and owning
curve IDs directly from device ragged offsets. It supports linear strips,
loops and segmented pairs, cubic Bezier/BSpline/Catmull-Rom/centripetal
Catmull-Rom, pinned endpoints, hulls and points. Pinned endpoints repeat
indices, not geometry. Counts and prefix offsets are computed on-device with
CUB; the resulting record count remains a GPU scalar.

The preparation boundary derives conservative capacities from scalar shape
metadata and binds requirements to a device and live stream. Execution uses
borrowed, caller-owned buffers, without allocation, host waits, readback or
application locks, and supports CUDA graph capture and replay. Stream-device
validation happens before capture: Compute Sanitizer caught
`cudaStreamGetDevice` rejecting an actively captured stream during initial
development. Invalid device offsets or odd segmented counts produce a zero
GPU record count and leave index/primitive outputs untouched. Shape, enum,
capacity and signed Storm index limits are checked before enqueueing.

The optional native oracle compares exact indices and primitive IDs against
the actual OpenUSD 26.08 Storm builder in 136 basis/wrap/mode/shape cases,
including empty, very short, long and ragged curves. It needs matching OpenUSD
private source headers; SDK-only builds skip that oracle explicitly, without
adding a source-tree dependency to production targets. A separate test feeds
actual GPU-compacted offsets directly into captured index generation, without
downloading or re-uploading those offsets. The existing compaction `Finish`
remains an explicit completion/count boundary; this does not claim the whole
compaction chain is asynchronous.

Validation: all 79 T0/T1 tests pass, including the two performance-labelled
tests; all six selected non-benchmark/non-Surgery T2 tests pass. Both new
index tests pass 30 consecutive runs. Compute Sanitizer memcheck is clean for
both the native oracle and the validation/capture test; the latter's final
compacted-input version also passes racecheck with zero errors or warnings.
Local Qwen's latest review request returned an empty answer, which is not
counted as successful model review.

This is the GPU topology prerequisite, **not native renderer integration**.
Stock BasisCurves still needs the renderer-side admission/BAR publication,
GPU draw-count, culling and topology-identity changes described above. There
is no arbitrary USD indexed-curve remapping, tile-offset rebasing or measured
production frame-throughput claim in this checkpoint.

### Renderer-owned post-commit publication boundary

The first native renderer extension is now a repository-owned patch under
`patches/openusd/`, applied and built only in a private OpenUSD v26.08
worktree. The original OpenUSD checkout and installed SDK remain unchanged.
`HdStResourceRegistry::AddPostCommitCallback` accepts parallel Sync-phase
registration and invokes a detached batch on the serialized commit context,
after computation submission and pending-source/computation cleanup. Nested
callback registration is deferred to the next commit, callback exceptions are
isolated, and recursive commit is rejected. Destruction drops pending
closures before registry/Hgi garbage collection. No usdGen/CUDA dependency or
application mutex is introduced into the renderer hook.

The hook is deliberately **not** a GPU completion fence or a blanket success
signal. The client must check its complete candidate and independently
establish transfer completion. `tests/storm-extension` rebuilds the actual
HdSt implementation against the matching SDK and selects that library only
for its test processes. This narrow harness is not a production SDK build;
the changed registry layout requires a consistent renderer/client rebuild.

- The generic test uses real BAR allocation and `HdStUpdateDrawItemBAR`, not
  a mocked callback queue. It verifies retention of the prior range on failed
  computation, commit-time draw-batch invalidation for replacement offsets in
  the same aggregate, 1,024 concurrent registrations delivered exactly once
  on the commit owner, exception isolation, deferral, recursion diagnostics,
  and cancellation/reference release on registry destruction.
- The CUDA test compiles the production transfer bridge against the private
  renderer. A single callback checks all eight candidate channels before
  publishing the whole buffer set. A deliberately incompatible last channel
  leaves the prior frame drawable despite earlier successful candidate
  copies. Superseded candidates are discarded, source owners expire after
  completed transfers, and retained old/new GL snapshots redraw correctly.
  Width .1 and .3 produce 304 and 760 green pixels respectively, with zero
  error-red pixels; only framebuffer pixels are downloaded.
- Review also found an ABI bug in the original interop test: it allocated
  `HdStResourceRegistry` on the stack without the SDK's MaterialX feature
  definition, which changes the class's header layout. The test now obtains
  its registry from `HdStRenderDelegate`, letting the SDK allocate it. Earlier
  interop passes did not prove host allocation safety. The private harness
  explicitly shares its matching MaterialX definition with its consumers.

Validation: both private renderer tests pass 30 consecutive Release runs each;
the real CUDA publication test passes Compute Sanitizer memcheck with zero
errors. The corrected original interop test passes 30 repetitions. All 79
T0/T1 tests and all six selected non-benchmark/non-Surgery T2 tests pass on the
final test sources. A separate ASan/UBSan build of the private HdSt, tests and
CUDA-GL bridge passes both tests ten consecutive times with
`detect_leaks=0` and `halt_on_error=1`; the remaining SDK and core CUDA build
are not instrumented by this harness. The patch passes `git apply --check`
against the clean original v26.08 checkout. Local Qwen returned generic but
visible review guidance that was checked against source; both Hivemind lanes
again returned empty visible answers and are not counted as reviews.

The production scene index still declines live device-generation publication.
The isolated native BasisCurves provider, pending-bundle lifetime, GPU draw
counts, and framebuffer path are now validated. Remaining work is wiring that
provider into live groom publication, including coherent topology/shader/extent/
motion updates, asynchronous interop lifetime, multi-tile atomicity, and the
unchanged full renderer/performance/release gates.

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
- At this early checkpoint `ops/deform.cpp` still had a placeholder. The CUDA
  RBF checkpoint below replaces it; standalone tests alone were not accepted
  as proof that a groom renders correctly.
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

### CUDA Width execution checkpoint

The source-only checkpoint above is now extended to `CurveSource -> Width`
chains. [cuda-width-network.usda](examples/cuda-width-network.usda) is a small
executable schema example for an explicitly device-aware tool consumer. The
larger discussion network remains unsupported as a complete cook/render.

- Width has a staged CUDA kernel with width/mask profile LUTs, root/tip scales,
  taper, replace/multiply, blend and enabled behavior. Supported scalar controls
  use groom/primitive/point fields; `enabled` is groom-only and boolean outputs
  retain byte typing. Exact zero-envelope/disabled passthrough is tested.
  Maps, non-identity mask random/noise/range controls, shaped ramp expressions,
  and connected CurveSource controls still fail explicitly.
- The graph owns immutable SeExpr IR compiled on the CPU without evaluating
  values. Execution builds incoming-geometry contexts and evaluates typed CUDA
  fields immediately before each Width operator. Frame-only commits reuse the
  compiled plan. Literal staging is lifetime-safe; half, scalar and vector
  native types are checked, and 64-bit integer literals outside the currently
  exact double range (2^53) are rejected. Arrays/matrices remain unsupported.
  GPU instruction/context allocations are currently rebuilt at execution;
  caching and incremental dirty execution remain open.
- Width changes only the published device width channel; points/rest/topology
  remain in the source allocation. The final width allocation is retained with
  the generation's source owner and consumer leases. Source uploads still
  repeat per commit; there is no renderer interop or complete GPU tool suite.
- A live `UsdGenCurveAPI` adapter supplies Default-time rest independently of
  animated points. Explicit empty rest is preserved; absent rest uses Default
  points, not current-frame points. Rest dirties and live edits are tested.
  Stage rate metadata is transported with USD's time-code/FPS/24 precedence,
  so runtime `$time` is `$frame / timeCodesPerSecond`. Live rate edits notify;
  zero/nonfinite rates are rejected.
- The real Hydra-to-CUDA test exposed expression-binding metadata incorrectly
  entering the operator parameter sweep; those derived roots are now excluded.
  Runtime tests also caught and fixed empty-domain rejection in both Width and
  expression contexts. Empty primitive/point contexts have zero invocations
  while preserving their declared domain and validating the offset sentinel.
- Validation: `cmake --build build-codex -j6` and the expanded non-benchmark
  `ctest --test-dir build-codex -L 'T0|T1' -E bench --output-on-failure`
  passed **52/52**. The CUDA-disabled core target also rebuilt successfully
  in `/tmp/usdgen-cpu-check-4XIfVs`. Earlier targeted runs encountered
  intermittent CUDA stream-allocation failures; no services were stopped or
  reset. These functional checks are not performance or full-release gates.
  Native workers were Luna, with local Qwen and Hivemind review lanes; root
  reviewed, corrected and ran the integrated code.

### CUDA RBF execution checkpoint

[cuda-rbf-network.usda](examples/cuda-rbf-network.usda) is now an executable
small `CurveSource -> Deform(rbf) -> Width` hierarchy for an explicitly
device-aware consumer. The larger schema discussion network is still not a
fully supported cook/render. Width operators can precede or follow the one
rest-to-animated Deform step; a second deformation is rejected.

- The CUDA executor integrates persistent surface sampling and RBF solve state.
  Deterministic GPU farthest-point sampling uses O(vertices × samples) work,
  parallel distance updates and a deterministic single-block argmax reduction.
  Duplicate centers are suppressed. Rest positions, topology, surface path,
  algorithm version and evaluated sample budget govern rebinding; pose changes
  and frame scrubbing reuse the rest factorization. Sample count is a permitted
  scalar control readback, not a geometry readback.
- Deform evaluates incoming styled points, retaining canonical C3 rest as a
  separate channel. Triangle barycentric and quad bilinear root targets are
  sampled on the GPU. Root correction translates the entire warped strand,
  then point/groom/primitive blend and mask fields apply. Runtime SeExpr can
  control groom `rbfSamples`/`enabled`, groom or primitive `lockRoots`, and all
  three domains for scalar blend/mask amount. Shaped controls and the remaining
  non-identity mask controls are not implemented.
- Output points are privately staged and retained by the published immutable
  device generation and its leases. Source topology/rest/IDs and previous Width
  outputs remain independent. Invalid surfaces, failed/rank-deficient bindings,
  wrong root-binding surfaces and already-deformed source input cannot replace
  the last good generation. The former fake CPU Deform path now rejects rather
  than fabricating geometry. Device-loss cleanup quarantines the new binding
  and deformation buffers; actual fault-injection coverage is still due.
- Hydra surface descriptors now consume `usdGen/rest/*` from `UsdGenRestAPI`
  instead of binding to the current animated points. Live Default-time leaves
  survive cached handles and preserve explicit empty rest. Current/rest topology
  mismatch is rejected. Unsupported alternative rest source modes fail closed;
  asset/named-primvar rest sources remain to implement. C3/mesh object transforms
  must currently be identity; subsets, general polygons, automatic C3 root
  rebinding and source resampling remain unsupported.
- Functional tests cover rest identity, affine motion, nonlinear root targets,
  repeated poses/scrubbing without rebinding, rest edits, runtime sample-count
  changes, typed expression domains, retained older leases and failure retention.
  The real USD/Hydra example also checks Default-time rest edits, their actual
  scene-index dirty notice, changed binding identity and invalid topology.
  An intermediate full run passed **55/55** non-benchmark T0/T1 tests; subsequent
  hardening and stronger numerical oracles require the final rerun below.
  The CUDA-disabled core target also rebuilt successfully.
- Source and animated driver uploads still repeat; incremental refresh and
  persistent instruction/context allocation remain open. Full-rank affine
  support is required; no planar/rank-deficient fallback is silently applied.
  Guide-motion transfer, remaining operators, brushes, renderer graphics
  interoperability, production memory/performance measurements and release
  gates remain unfinished. A passing tool-consumer test is not a render claim.
- Root review also corrected worker test oracles that used stale last-good
  generations, assumed absolute cache counts and accepted errors larger than
  the expected motion. A paired unlocked nonlinear cook now proves primitive
  root locking actually changes the intended strand while leaving the other
  unchanged. Root caught an unwritten sample-index tail after deduplication;
  CUDA initcheck then exposed nine uninitialized staging/control copies in
  failed-bind tests. Both were corrected. `compute-sanitizer --tool initcheck
  --error-exitcode 1` and `--tool memcheck --error-exitcode 1` each now report
  **0 errors** for both `testUsdGenCudaSurfaceBinding` and
  `testUsdGenCudaRbfSession`. These are bounded memory checks, not full fault
  injection or a performance gate. Native workers were Luna; local Qwen
  supplied review, while the later Hivemind attempts returned empty responses
  or timed out and are not counted as completed substantive reviews.
- Final checkpoint rerun: `cmake --build build-codex -j6` succeeded and
  `ctest --test-dir build-codex -L 'T0|T1' -E bench --output-on-failure`
  passed **55/55** after the memory-check fixes. The CUDA-disabled core target
  built successfully in `/tmp/usdgen-cpu-check-4XIfVs`. No full-plan, renderer,
  performance or release completion is implied.

### CUDA Length and topology revision checkpoint

[cuda-length-network.usda](examples/cuda-length-network.usda) exercises a real
hierarchy-derived `CurveSource -> Length(cull) -> Width` device cook. Length can
also precede RBF or another Length; downstream expression contexts and root
bindings consume the surviving, reordered GPU channels.

- Length now implements scale/set and actual curve removal, plus `cutExtend`
  with `keepParam` (preserve interior arc positions and collapse past the cut)
  and `reparam` (redistribute CV samples over the target arc). Extension follows
  the last nondegenerate tangent. Positive requested length on a zero-length
  curve rejects because no direction can be inferred; a disabled/zero-envelope
  operator remains an exact no-op. Min-length floors apply before the envelope.
  Binary culling uses the resulting arc length; all-zero envelopes protect a
  strand. Culling does not fabricate zero-length parked curves.
- Scalar values, floors, blend and mask amount support groom/primitive/point
  CUDA expression fields. Random ranges are native float2 at groom/primitive
  rates and use the pinned seeded stable-ID draw, checked against the existing
  CPU hash oracle in a test. Thresholds are groom/primitive and enabled is
  groom-level in graph admission. Named mask profiles are supported; maps,
  non-identity random/noise/range masks, shaped expressions and connected seed
  controls still require implementation.
- `CudaCurveCompaction` uses GPU CUB prefix scans and stable scatter, retaining
  points, rest, widths, hairT, curve IDs, root faces and root UVs together. Only
  output curve/point counts and diagnostic flags are read back. Offsets always
  include the sentinel, including the zero-curve case. Published generations
  own the complete topology revision; later points/widths may independently
  override its channels, and old consumer leases retain prior allocations.
- Root review corrected point fields inadvertently evaluated only at roots,
  scale floors being ignored, keepParam accidentally behaving like reparam,
  disabled/zero-mask culling, an overlapping offset write and capped-grid
  kernels missing large-array tails. It also corrected a reversed USDA stack,
  missing C3 API/type metadata, mismatched hairT/count goldens, inherited
  expression overrides invalidating cut tests, and weak output-retention tests.
  These corrections were checked in execution, not accepted from worker prose.
- `cmake --build build-codex -j6` succeeded. The full non-benchmark T0/T1 command
  passed **58/58**. Both `compute-sanitizer --tool initcheck --error-exitcode 1`
  and `--tool memcheck --error-exitcode 1` reported **0 errors** for
  `testUsdGenCudaLength`, `testUsdGenCudaCurveCompaction`, and
  `testUsdGenCudaLengthSession`. The CUDA-disabled core target also rebuilt.
  The live Hydra fixture observes a precise threshold dirty and cooks an empty
  revision on the same scene index. Session checks cover two successive Length
  compactions, translated downstream RBF and retained earlier leases. The final
  rerun also covers a frame-dependent threshold on the same compiled session
  and an active primitive float2 random-range expression.
- This is not a complete Length/tools milestone. Stable capture-time cull sets
  across animation and gesture-time maximum-count parking (S28/S-7) remain to
  integrate. The current CUDA path recomputes keep decisions at execution, so
  changed evaluated controls or incoming shape can change topology. Incremental
  reuse and the planned performance thresholds are unverified. Source uploads
  and full-channel copies still repeat; no zero-transfer rendering claim follows.

### CUDA picking and device-stroke publication checkpoint

- `CudaPicking` projects CVs in parallel and uses device ArgMin for nearest
  selection, with the lowest flat CV index breaking ties. Footprints use
  uint32 flags, a device prefix scan and ordered scatter; their indices remain
  on the GPU. Host results contain only the scalar hit record or count/error.
  The matrix uses USD row-vector convention and pixels have a top-left origin.
  Nonpositive clip w and out-of-range depth are rejected; mesh occlusion/depth
  buffer integration remains unimplemented.
- `CudaPointOverride` validates device indices and absolute replacement
  positions, rejects duplicates deterministically with device radix sort, and
  copies/scatters into a private GPU point allocation. Invalid edits retain the
  last completed result. Empty edits and empty completed allocations are valid.
  Published point revisions share rest, widths, topology, stable IDs and root
  bindings with their immutable base; nested consumer leases retain both parent
  channels and edited points through completion on the consumer stream.
- `CudaToolSession` is a caller-serialized C++ bridge to the engine, not a
  usdview brush. Begin reserves the engine's single active device stroke using
  a token. Every move uses the press-time base; active pick/footprint queries
  also use that base. Moves stage immutable revisions without cooking operators
  or writing USD. `Commit(LiveOverride)` publishes through the existing imaging
  callback, rechecking the expected generation and pending graph work. Cancel
  stages the base restoration; Close releases without republishing and returns
  the completed edit for a future explicit authoring boundary. Independent
  wrappers cannot replace each other's strokes. External graph/frame cooks
  invalidate the current stroke rather than silently applying stale indices.
- Exact GPU comparison of ordered offsets and stable IDs now preserves
  `topologyVersion` for point-only graph updates and brush publications. Changed
  membership or CV layout changes the version. This compares layout identity,
  not a complete capture/rebase epoch: capture changes that retain the same
  layout still need the planned explicit epoch contract.
- Root review replaced a one-thread picker with parallel reduction, corrected
  uninitialized pick result/padding readback, transactional footprint replacement,
  uint8 scan accumulation risk, empty point-buffer transfers, async failure
  fencing, and stale-release/invalidation handling. Tests exercise 1025 ordered
  footprint hits, translation/perspective/depth projection, duplicate rejection,
  repeated and replaced moves, cancellation, rival tool wrappers, imaging
  callbacks, topology edits and leases retained after session/tool destruction.
  Local Qwen contributed inspected topology/lifetime review; both Hivemind
  lanes reached the loaded model but again received no visible final answer.
- The first full non-benchmark T0/T1 run passed **62/62**. CUDA initcheck reported
  **0 errors** for `testUsdGenCudaPicking`, `testUsdGenCudaPointOverride`,
  `testUsdGenCudaTopology` and `testUsdGenCudaTools`. These four targets also
  passed memcheck with **0 errors**. Final invalidation hardening subsequently
  passed the same 62-test suite and the CUDA-disabled core rebuild, before the
  scheduling/lease revision described above.
- M5 remains incomplete: `cApi.h` still only declares its nineteen entry points.
  The ten brushes, CUDA brush math/falloff/root-frame/locked-curve handling,
  app-thread dispatch, usdview controls, release-time authoring and undo, GPU
  surface picking, symmetry, overlays and frozen re-entry remain required.
  Preserving an active stroke across animation with a changed incoming snapshot
  is not implemented. The stock tile publisher still refuses device geometry;
  renderer interop, transfer tracing and the actual T-1/T-2/T-3 thresholds remain
  open. Allocations, full base copies and synchronous fences are not yet a
  measured interactive-performance implementation. No CPU geometry readback
  fallback is provided by the new runtime path; tests read back numeric oracles.

## Validation discipline

## Native-provider / draw-count contract (isolated validation)

The private OpenUSD native patch artifact is generated and passes clean
baseline/application and reverse-equivalence checks. Its isolated native
provider, draw-count, and retained-scene-index framebuffer fixtures are
validated; live groom/device publication and multi-tile integration remain
open. When a valid CUDA device generation provider is present, it is authoritative: the renderer must not
silently instantiate or select a CPU geometry fallback. A Storm candidate is
publishable only as one complete bundle of all required channels; any partial
or failed channel invalidates the candidate and leaves the prior visible
bundle selected.

The private post-commit callback state is independently owned by the pending
bundle and is invalidated when that bundle is finalized or rejected. Callbacks
must not retain the registry, re-enter Commit, or outlive their owner state.
The draw-count packer writes the actual `uint32` draw count
`records * indexArity`, never allocation capacity. Host code may validate
scalar shape, arity, and capacity before enqueue; device validation consumes
the upstream status and overflow conditions. Neither generated geometry nor
draw counts may be read back to the host.

Conservative visibility/culling behavior and interactive performance remain
open; this contract makes no throughput or latency claim. The isolated native
fixture validates scalar metadata, device-side structural validation, and
framebuffer output. It is not evidence that live groom `Publish`, atomic
multi-tile publication, or full end-to-end Storm integration is complete.

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
endpoint (currently loaded as `qwen3.8-27b@q4_0`, verified through `/v1/models`)
for substantive implementation/review tasks. A model
request is only counted as used when its response was received and inspected.
The Length/compaction audit requests reached this loaded model, but exhausted
their token budgets in reasoning without visible answer content despite
thinking-disable options. They are not counted as completed reviews.
Missing process handles permit a new request; observation timeouts do not.
The native session currently permits three workers plus root, not twenty
simultaneous native workers. No larger worker pool is claimed.

### Current evidence checkpoint (2026-09-11)

Core CUDA curve-index validation passed in 10 repeated runs and memcheck,
along with the nonbenchmark T0/T1 session/publication tests. The private Storm
helper test covers count-word copying, malformed count rejection, and
conservative GPU-count visibility. Provider ingress rejection handling is
covered. The isolated native provider and retained-scene-index framebuffer
paths are validated; successful live groom/device publication remains
unverified.

Latest checkpoint: private Storm CMake build passed and all five selected tests
passed; the nonbenchmark T0/T1 suite passed 78/78. Abstract draw-count rendering
produced 18 lit pixels for count 2 and 35 for count 4; rejected candidates
retained exact finite framebuffers. Production CUDA generation 0 rendered 50
pixels, rejected candidates retained the prior result, and generation 1 changed
geometry/width to 51 pixels. Native CUDA memcheck reported zero errors. Both
patch checks passed. A fresh initial-generation regression passed in both
Release and ASAN/UBSAN configurations. The ASAN/UBSAN build instrumented
private HdSt, bridge, and fixture/test translation units; the linked usdGen core
remained Release and leak detection was disabled, so this is not a leak check.
Live device Publish, multi-tile atomic publication, persistent async
interop, motion, culling performance, and full SDK gates remain open.
