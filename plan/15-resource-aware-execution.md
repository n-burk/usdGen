# Resource-aware execution graph

Publication scope (2026-09-14): the user requested the existing CUDA work on
main. The CUDA/shared execution, source, COW, lifecycle, imaging-contract and
test changes are being published independently of the opt-in Vulkan backend.
Vulkan implementation/build/test/toolchain files remain local for a separate
publication. Vulkan validation checkpoints below record local development
history, not Vulkan code included in this CUDA-only publication. The full
execution-graph objective remains open.

Latest scope clarification (2026-09-13): after initially excluding non-CUDA
implementation, the user explicitly requested Vulkan implementation with ten
additional Terra/Luna agents. Vulkan is therefore back in scope; Metal remains
excluded. CUDA implementation and backend-neutral public interfaces remain
required. The session's four-active-agent cap still applies, so additional
Vulkan tasks must run in worker batches under Astra rather than ten extra
simultaneous agents. Historical checkpoint scope statements below do not
override this latest clarification.

2026-09-12. **Implementation evidence: OPEN.** The resource-admission DSO has
not yet been built or loader-verified on Windows; its explicit import/export
annotations are source-level portability work, not Windows validation. This is the current execution
overlay for the remaining end state. It refines the binding requirements in
[14-hierarchy-cuda-implementation.md](14-hierarchy-cuda-implementation.md);
it preserves its historical checkpoints rather than rewriting them. It makes
no claim that Metal or Vulkan is implemented, that stock Storm accepts a
GPU-resident `basisCurves` handoff, or that the gates below currently pass.

## Outcome and non-negotiable rules

The runtime must schedule every semantically valid composition of declared
supported groom operators, expressions, maps, source/deformation roles and
tools, including required fan-in, fan-out and reference edges. A composed
`Description/Ops` hierarchy remains the semantic source of ordering:
reverse-sibling post-order is the authored stream; children precede their
parent and lower siblings precede upper siblings. Compiler-owned prerequisite
edges may expose parallel work only when they cannot change that observable
order, diagnostics, random identity, topology, or final value.

CUDA is the first implementation target. The public execution, resource,
logical-completion-token, buffer and generation contracts use backend-neutral
identities and capabilities, so Metal/Vulkan backends can be added later
without a CUDA type in the contract. A completion token does **not** assert
that CUDA events, Vulkan synchronization primitives, and Metal shared events
have identical visibility or ownership semantics: each adapter must enforce
its backend's real rules ([CUDA stream-ordered allocation](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/stream-ordered-memory-allocation.html),
[Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html),
[Metal shared events](https://developer.apple.com/documentation/metal/synchronizing-events-between-a-gpu-and-the-cpu)). CUDA streams/events and CUDA Graphs are implementation
details. This is not permission to add a CPU production fallback. No OpenUSD
source/binary patches, custom rprim, renderer replacement, or host geometry
readback is permitted to satisfy execution or renderer interoperability.

There are no application mutexes, spinlocks, global cook locks, blocking
owner callbacks, or unsynchronized cross-owner mutable cache maps. An
owner-local mutable cache map is allowed only behind its scheduled owner; all
other readers consume immutable snapshots. Framework
internals and atomic shared-pointer implementation are not claims of hardware
lock-freedom.

## Initial source baseline and gaps

This baseline predates the implementation checkpoints below. Those checkpoints
record subsequent changes without closing the remaining end-state gates.

The repository already has useful foundations but not the complete graph.
`UsdGenExecutionPipeline` gives an epoch-bearing serial owner/work lane;
`UsdGenCudaExecutionQueue` currently owns one workspace and pipeline per
description. `UsdGenSessionCooker` owns one CUDA workspace per description.
This prevents races within one workspace but does not provide a global
per-device admission controller, fair multi-groom dispatch, cross-groom cache
policy, or a bounded ingress policy.

`CompileCudaGraph` makes an immutable linear CUDA step sequence and
`ExecuteCudaGraph` walks it on one stream. The CPU scheduler likewise walks
nodes in topological order and parallelizes only chunks inside a node. That
does not yet construct a dependency-ready task DAG for arbitrary supported
compositions, nor prove concurrency across legal ready branches/grooms.
Current CUDA validation intentionally rejects some combinations (including a
CurveSource with upstream/reference/map). That is a scheduler/executor gap
when the composition is semantically valid under declared operator/resource
contracts; it must not redefine the end state by classifying the graph
unsupported. Only invalid authoring or an explicitly unimplemented operator's
mathematics/capability may receive an unsupported diagnostic.

The current code has no application mutex/lock-type occurrence under `libs/`.
Older plan-14 mutex migration statements are historical, not evidence of a
current mutex gap. Conversely, CUDA code still contains synchronous stream
fences and small host status/metadata reads, workspace destruction synchronizes
its stream, and `gpu::DeviceBuffer` uses direct allocation/free. Those facts
mean latency, asynchronous retirement, global memory safety, and GPU overlap
remain OPEN. Existing device generation/lease identities are a sound seam but
do not yet charge all producer, scratch, cache, or retired-generation bytes.

## Required model

### Immutable planning and versioning

Every accepted request carries an immutable `ExecutionRequest`:
`{descriptionInstance, attachmentInstance, requestEpoch, frame/sample,
backendDevice, compiledPlanKey, inputVersions, dirtySet, priority, deadline}`.
`descriptionInstance` and `attachmentInstance` are non-recyclable identities;
paths alone never authorize a delayed publication.

Compilation produces an immutable `ExecutionPlan` with:

- authored order keys and an explicit capability result for every operator;
- tasks, typed resource reads/writes, dependency edges, topology barriers,
  cancellation safe points, and estimated bytes/time;
- immutable program/map/source/rest signatures and cache lookup keys;
- a deterministic terminal/publication task whose acceptance predicate is
  `(attachment identity, request epoch, input/version tuple)`.

Each cache entry is immutable and is keyed by backend identity plus the exact
producer signature, not a mutable graph pointer. Minimum signatures are:

| Cache | Required signature |
| --- | --- |
| compiled plan | composed hierarchy/order, operator capability versions, typed expression IR, map/source bindings, backend capability version |
| rest/RBF binding | rest geometry/topology, root/stable-ID ordering, binding controls, sample budget/algorithm version, backend/device |
| parameter/map field | program/map asset+content/version, domain, frame/sample if time-varying, upstream field versions, backend/device |
| geometry intermediate | plan task key, all input versions, topology/layout, evaluation frame/sample, precision, backend/device |
| terminal generation | terminal task signature, attachment identity, topology/layout, renderer/publication capability—not a Hydra host-array identity |

Dirty propagation is version-based: an edit allocates a new input version;
the description command owner merges dirt only for its identity and accepts a
candidate only if every captured version still matches. A dirty bit is cleared
only by successful publication of that exact version tuple. Superseded, failed,
evicted, or cancelled candidates retain dirty state and cannot become an
incremental baseline. Topology/layout/version changes invalidate dependents;
value-only changes reuse only signatures that explicitly exclude that value.
Cross-groom reuse is allowed only for immutable content-addressed assets/rest
artifacts with compatible backend/device and no groom-local input in the key.

### DAG legality and CUDA Graph use

The compiler first emits the semantic authored stream and then adds internal
data edges. It may run tasks concurrently only if there is no authored-order
path requiring an observation, no shared exclusive workspace/resource, and
the result is deterministic under the operator's declared reduction and RNG
rules. Topology-changing, compaction, ordering, source, and publication tasks
are barriers unless a capability declaration proves a partition-preserving
split/join equivalent. A join must use the stable authored order key, never
completion order.

CUDA Graph capture is legal only for a maximal ready sub-DAG whose topology,
allocation addresses/layout, launch shapes, parameter ABI, stream/event
dependencies and backend capability match an immutable capture key. Capture
cannot reorder authored hierarchy semantics or contain admission, host
callback, dynamic allocation, unbounded retry, external producer wait, or
publication acceptance. Recapture/invalidate on any key mismatch; cache graph
executables under the same eviction and byte accounting as other caches.

### Scheduling, cancellation, liveness

Per-description command owners accept/merge requests quickly. Per-device
dispatch owners choose ready tasks from bounded queues; per-workspace owners
serialize only truly exclusive mutable workspaces. CPU dependency preparation,
independent descriptions, independent immutable-plan branches, transfers and
CUDA streams overlap where dependencies permit. No queue consumer waits while
holding ownership of a queue or callback lane.

Use at least interactive and background classes, FIFO within each class by
arrival sequence, plus aging so continuous interactive traffic cannot starve
background work. Interactive work receives a bounded reserved share but does
not preempt an unsafe kernel. Admission may coalesce an unstarted pending
request to the newest compatible epoch; it must report `Superseded`, preserve
the newer dirty tuple, and release its reservation. The initial queue slice's
one-running/one-pending coalescing is a local mechanism, not the global
ingress/admission policy.

Cancellation safe points are before cache materialization, before each DAG
task launch, after producer event recording, before a dependent launch, and
before publication. Once a CUDA launch is submitted it is not forcibly
cancelled: its candidate becomes retired, event-fenced work. A stale task may
finish and populate only a cache entry whose full immutable signature matches;
it may never publish, clear dirt, or replace a newer cache value.

Under the declared assumption that the backend either signals submitted
completion tokens or reports a terminal device error, finite queues, bounded
admission attempts, completion relays and a non-blocking retirement owner
provide liveness. Device loss,
allocation failure, compile failure, invalid capability, or event failure
fails the candidate, preserves last-good publication and dirt, fences/quarantines
affected allocations, and wakes the dispatcher. It must not strand a queue
reservation or depend on a global recovery mutex.

### Memory admission, producer lifetime, and retirement

For every device/backend, the owner maintains a charged ledger split into
`published`, `running producer`, `pending reservation`, `scratch peak`,
`cache resident`, `retired pinned`, and `display/shared headroom`. Each task
declares `steadyBytes`, `scratchPeakBytes`, output bytes, and producer-retention
bytes. Admission reserves the worst simultaneous peak:

`charged + pending + task steady + task scratch peak + retained producer peak
 <= admission cap`, where `admission cap` has already subtracted the
display/shared headroom exactly once.

The usable budget is explicit policy derived from backend-reported available
memory, a configured cap, and a nonzero display/shared headroom. The initial
CUDA policy is configurable before first use and defaults to 80% of observed
free memory with at least 1 GiB retained as display/shared headroom; a lower
configured cap wins. Driver,
CUDA-runtime, cuSolver and renderer-internal allocations are excluded from
charged bytes and covered only by headroom; their exact allocation is not
claimed tracked. All usdGen-owned allocations must go through the
backend-neutral resource interface; direct raw device allocation is a migration
failure once the relevant slice lands.

An output retains every producer allocation it reads until its producer event
and all consumer lease events complete. Retired generations are pinned in the
ledger until those events signal; dropping a public shared pointer is not a
free permission. A retirement owner receives backend completion signals and only
then returns memory to the pool only after backend release succeeds, or keeps
its charge permanently quarantined after device failure/release failure. It
never calls a device-wide synchronize. Cache eviction is owner-scheduled and
only selects unpinned immutable entries. Eviction discards device buffers and
recomputes from retained immutable descriptors/programs/source data; it never
uses GPU geometry readback as a cache refill.

If admission cannot make room through safe eviction/retirement, interactive
work may supersede an unstarted older request; otherwise it returns a visible
resource-deferred/failed outcome according to deadline policy while retaining
dirty state. It does not exceed budget, evict a leased generation, block the
render/read path, or silently fall back to CPU geometry.

## Phased implementation slices and objective gates

All phases are **OPEN** until the listed evidence is recorded. They are ordered
to keep changes reviewable; later phases must not be inferred from an earlier
unit test.

1. **Contract and observability.** Add backend-neutral `DeviceResource`,
   `DeviceEvent`, `MemoryReservation`, task cost/peak declarations, immutable
   plan/request/cache-key structs and diagnostic counters. CUDA adapts them;
   Metal/Vulkan remain unimplemented adapters. Gate: unit tests prove signature
   equality/inequality, non-recycled identity rejection, overflow-safe byte
   arithmetic and diagnostics for each rejection path.
2. **Charged allocation and nonblocking retirement.** Introduce one
   process/DSO-shared per-device registry and ledger, then charge primary
   topology/geometry direct allocations first (including reset/move/release
   provenance). Preserve driver-library headroom; add logical-token retirement
   and permanently charged quarantine on failed release. A pooled allocator is
   a later optimization, not a claim of this initial slice. Gate: forced-cap
   tests prove the authoritative total never undercounts under concurrent
   allocation/release, while category observations are either an exact coherent
   snapshot or explicitly labelled conservative/transient in this primitive.
   They also prove a leased old generation cannot be reused early, retirement
   occurs after completion without `cudaDeviceSynchronize`, and OOM retains
   last-good/dirty state. A later snapshot gate must prove exact category/total
   coherence. This slice does not prove cache eviction or DAG work.
3. **Bounded ingress and acceptance.** Integrate local one-running/one-pending
   request coalescing with a per-device admission owner, request classes,
   cancellation-safe release, and attachment/version acceptance predicates.
   Gate: held-kernel tests show independent grooms enter distinct ready queues,
   newer same-groom work supersedes only pending work, no callback waits, and
   failure/cancellation releases every reservation exactly once.
4. **Dependency-ready compiler.** Replace linear execution scheduling with
   semantic stream plus task DAG lowering, capability matrix, barriers,
   deterministic joins, task estimates and explicit safe points. Gate: every
   semantically valid supported operator pair/triple, fan-in/fan-out/reference
   case, and representative nested hierarchy compiles; invalid authoring and
   mathematically unimplemented operators get distinct precise diagnostics.
   Randomized hierarchy permutations match the authored reference semantics
   bitwise/tolerance-defined. A scheduler limitation cannot satisfy this gate
   by rejecting a valid composition.
5. **Multi-groom dispatch and cache correctness.** Run legal ready tasks across
   descriptions/streams, introduce immutable cache publication/eviction and
   versioned dirty propagation. Gate: simultaneous independent grooms overlap
   by event trace; edits to A never invalidate/reuse B's groom-local cache;
   stale/failed/cancelled A cannot clear or seed later A dirt; eviction followed
   by recompute has no GPU geometry readback and matches a cold cook.
6. **Asynchronous CUDA execution.** Replace execution-path fences/readbacks
   with events and compact asynchronous status relays where possible; turn
   temporary producer/scratch buffers into declared resources. Gate: timeline
   traces show no device-wide fence or owner-thread stream synchronize on the
   interactive path, retain correctness under injected event/allocation/device
   failures, and show legal stream overlap rather than assumed overlap.
7. **CUDA Graph specialization.** Capture only legal stable sub-DAGs and use
   ordinary stream DAG dispatch otherwise. Gate: captured and uncaptured paths
   produce equivalent generations/diagnostics; key changes recapture safely;
   capture memory is charged/evictable; hierarchy order remains unchanged.
8. **Publication and stock-SDK boundary.** Carry immutable accepted device
   generations through tools/imaging without host geometry readback; establish
   a standard-Hydra, unpatched-OpenUSD GPU-residency mechanism or keep this
   gate OPEN. Gate: end-to-end tracing proves renderer consumption of the
   device generation, no custom rprim/renderer/patch, no geometry D2H in the
   ordinary path, and last-good visibility under rejected candidates.
9. **Performance, fairness, and soak.** Establish hardware/config-specific
   budgets and latency thresholds, then test contention, long-running kernels,
   retirement pressure and shutdown. Gate: reproducible traces demonstrate
   bounded interactive acceptance latency under declared load, bounded memory
   at every ledger category, background progress via aging, no leaked pinned
   generations, and clean event-driven shutdown. No percentage claim substitutes
   for the recorded measurements.

## Evidence protocol

Each implementation PR/test record must name the source slice, backend/device,
budget/headroom configuration, exact supported capability set, command/output,
event/memory trace and failure injections. A passing CUDA-disabled core build
is contract coverage only; it is not a CPU execution fallback or GPU proof.
Metal/Vulkan remain design targets until their adapters and equivalent gates
exist. The current plan's evidence state stays **OPEN**.

## Bounded implementation checkpoint: ingress and charged CUDA buffers

2026-09-12. This checkpoint records only the completed narrow implementation
slice below. It does not complete any numbered phase, does not establish a
global execution graph/cache policy, and does not alter the OPEN evidence
status for the full resource-aware end state.

`UsdGenExecutionPipeline` now retains at most one running work item and the
latest pending work item per pipeline, with configurable `Submit` capacity
4096. This bounds the work slot itself and coalesces same-pipeline queued work;
it does **not** cap arbitrary closure byte sizes, `PostCommand` ingress, or
`CancelPending` ingress. Consequently it is not a global admission mechanism
or a complete interactive latency claim.

The new shared backend-neutral resource DSO keys one resource pool by backend
and physical device. CUDA `DeviceBuffer` allocation now acquires a real byte
charge for its requested payload bytes, including the primary topology path;
there is no thread-local bypass. Allocation/event creation failure preserves
the readable old buffer. A failed/unproven CUDA release quarantines the
allocation and retains its charge rather than refunding unproven storage.
Resource categories currently default to `Active`; later lifetime
reclassification, cache eviction, asynchronous allocation and event-driven
retirement are not implemented or implied by this checkpoint.

CUDA's initial per-device policy is host-configured before first use. In the
absence of that configuration it admits against 80% of initially observed
free memory and retains at least 1 GiB as display/shared headroom (a lower
available-memory result fails closed). This ledger is usdGen-owned allocation
accounting, not a claim to charge CUDA driver, cuSolver, renderer, or other
external allocations. The CUDA queue's forced-quota failure retains a readable
last snapshot and subsequently recovers when quota permits.

The final selected T0/T1 suite passed **93/93** in 20.08 s
(`/tmp/usdgen-resource-execution-verified-t0t1.log`) and T2 passed **6/6** in
9.54 s (`/tmp/usdgen-resource-execution-verified-t2.log`). The focused
CUDA-disabled resource test passed **1/1**. A final export build, reinstall,
and installed-header/shared-resource-consumer rerun pass; core `ldd` confirms
the resource DSO resolution and `nm` finds no exported internal TBB symbols.
These are bounded source/build evidence. `testInstallTree` currently returns
zero while printing `SKIP`, so it is explicitly not credited as a full install
gate.

The latest production pipeline 100-consecutive-run check passed (`bb21af`).
A latest single ASan/UBSan run passed, and the follow-up current 100/100 and
exact-HEAD-baseline 100/100 leak-enabled runs passed. However, an earlier root
100-repeat loop did report LeakSanitizer 1560 bytes in 3 allocations rooted at
SDK TBB `task_stream<3>::initialize` through arena/runtime construction
(`ec8319`). The follow-up audit at `/tmp/usdgen-pipeline-leak-audit-TiLg0r`
did not reproduce it, but the cause is **UNCLASSIFIED**, not exonerated, and
no suppression was added; the sanitizer-repeat gate remains OPEN. CUDA
allocator and CUDA queue memcheck/initcheck each reported zero errors in
`/tmp/usdgen-resource-{cuda,queue}-{memcheck,initcheck}.log`. The API-export
only changes came after those CUDA checks, so they are not a broad post-export
CUDA-coverage claim.

### Ready-task and CUDA-stage checkpoint (limited validation)

2026-09-12. Current source adds a backend-neutral `UsdGenExecutionTaskGraph`,
async pipeline completion path, and CUDA queue lowering into source, operator,
and terminal stages. The bounded build/test/sanitizer evidence recorded below
validates that source slice only. It completes **no** numbered phase and does
not establish end-to-end performance, full GPU-resource bounds, arbitrary
semantic DAG coverage, cache/dirty ownership, or interactive-responsiveness.

The task graph currently bounds admitted jobs, task count, dependency edges,
and active task count, and has interactive/background selection plus aging.
It is not yet per-task peak-byte admission: tasks declare no steady/scratch
peak reservation and this checkpoint makes no GPU-memory-bound claim for the
dispatcher. Existing `DeviceBuffer` allocation charging remains the narrow
allocation accounting described above; it is not evidence of complete task
admission or event-fenced retirement.

Owner relay/worker enqueue failure is fail-fast (`std::terminate`) rather than
silently dropping an accepted terminal completion. Catastrophic framework
dispatch recovery therefore remains an explicit **OPEN** gate, not a recovery
solution. `PostCommand` byte ingress is still uncapped. `UsdGenSessionCooker`
is not integrated with this dispatcher. CUDA synchronous stream fences remain,
and arbitrary semantic DAG compiler lowering is still restricted; the current
CUDA stage chain must not be presented as proof of arbitrary ready-branch
parallelism, nonblocking retirement, or complete cache/dirty integration.

The first local-Qwen requests for this follow-on were malformed: they used an
unsupported top-level `thinking` field and omitted both
`chat_template_kwargs.enable_thinking=false` and a completion cap. Metrics
showed they were actively generating; root terminated their exact curl handles
at elapsed 952 s. Reused retry status/stderr paths are not treated as evidence
of the original requests' terminal outcome. Corrected, uniquely stored local
requests used `qwen3.8-flash-next`, `stream:false`,
`chat_template_kwargs:{enable_thinking:false}`, and
`max_completion_tokens:1800`; both returned HTTP 200, IDs
`chatcmpl-ab33a7771ce4283e` and `chatcmpl-94ab5bf2ee14380a`, with
`finish_reason=length`. Their artifacts, including requests, responses,
statuses, stderr, and curl handles, are under
`/tmp/usdgen-local-qwen-corrected-Iwfnak`. Their only accepted limited finding
is the already-known absence of per-task peak-byte pre-admission and
event-retirement accounting; speculative cancellation, stream-order, queue
destruction, and broad OOM/accounting claims were rejected by source review.

Root validation of this source slice recorded a clean build in
`/tmp/usdgen-stages-clean-build.log`; the selected T0/T1 suite passed
**96/96** in 12.32 s (`/tmp/usdgen-stages-t0t1.log`), and focused scheduling
tests passed **6/6** (`/tmp/usdgen-stages-focused-tests-5.log`). The existing
install test that returns success while printing `SKIP` remains an explicit
caveat and is not upgraded to installation evidence. Standalone
ASan/UBSan/LSan loops passed TaskGraph **100/100** and stress **100/100** in
`/tmp/usdgen-stages-sanitizer-H4WbjG`. This does not clear the earlier pipeline
recurrence: run 18 after 17 passes again reported 1560 bytes in 3 SDK TBB
`task_stream`/arena allocations. That leak remains **UNCLASSIFIED** and the
sanitizer-repeat gate remains OPEN. Root additionally verified that archived
HEAD `executionPipeline.cpp` and its test exactly match `git show HEAD`, and
that its sanitized binary links the same SDK `libtbb.so.2`, `libasan`, and
`libubsan`. That existing HEAD binary passed 130 repetitions before run 131
reported 1040 bytes in 2 `task_stream::initialize` → arena/runtime-constructor
allocations (`/tmp/usdgen-stages-sanitizer-H4WbjG/head-pipeline-run-131.log`).
This supports a preexisting leak *class*, not an exact byte-count match or a
cause/ownership conclusion; current-change impact remains **UNCLASSIFIED**.
No suppression or TBB/OpenUSD patch was added, and the repeat gate remains
OPEN.

CUDA-stage validation initially observed a CUDA queue init-stream failure in
`/tmp/usdgen-cuda-stages-tests-1.log`; a verbose rerun passed, followed by the
focused 6/6 and T0/T1 96/96 results above. Error-name/string diagnostics were
added for the observed condition, but this checkpoint makes no engine-fix or
stability claim: the initial failure is transient and **UNCLASSIFIED** pending
further CUDA evidence. The CUDA source, per-operator, and final math are now
extracted and shared by the compatibility driver, but stages still include
synchronous CUDA fences.

`UsdGenCudaExecutionQueue` now also has an explicit four-argument
`RequestClass` submission overload; the previous API remains interactive by
default. The background-queue regression passed. A final clean build is
recorded in `/tmp/usdgen-stages-background-build.log`, and final T0/T1 passed
**96/96** in 12.36 s (`/tmp/usdgen-stages-background-t0t1.log`). Final queue
compute-sanitizer memcheck and initcheck each reported zero errors in
`/tmp/usdgen-stages-background-queue-{memcheck,initcheck}.log`. Earlier stage
GPU sanitizer, T2, and CPU-off evidence remains applicable because this
priority API change did not alter stage math. The final priority API was also
rebuilt with CUDA disabled (`/tmp/usdgen-stages-background-cpu-build.log`);
the four selected core tests passed **4/4** in 0.07 s
(`/tmp/usdgen-stages-background-cpu-tests.log`). This is limited API/regression
evidence only; it completes no phase.

Source audit additionally finds that retiring TaskGraph captures and atomically
replacing last snapshots may destroy GPU owners on owner callbacks, where
`cudaFree` can synchronize. Event-driven off-owner retirement is therefore
not implemented, and this slice must not claim fully nonblocking owner paths
or producer retirement. At that checkpoint `UsdGenSessionCooker` remained a
direct compatibility wrapper; the session-stage checkpoint below supersedes
that integration gap. All numbered phases and the overall evidence state
remain **OPEN**.

Selected T2 tests passed **6/6** in 9.73 s
(`/tmp/usdgen-stages-t2.log`): CUDA GL interop plus five stock-Storm tests.
This is not device-generation renderer-handoff proof. CUDA compute-sanitizer
memcheck and initcheck reported zero errors for both queue and stages tests:
`/tmp/usdgen-stages-testUsdGenCudaExecution{Queue,Stages}-{memcheck,initcheck}.log`.
A CPU-off build passed in `/tmp/usdgen-stages-cpu-build-2.log`, followed by
four core/pipeline/taskgraph/stress/resources tests **4/4** in 0.05 s
(`/tmp/usdgen-stages-cpu-tests.log`). These results neither add a CPU fallback
nor prove renderer handoff outside their selected scope.

No OpenUSD source/binary patch is part of this work: root confirmed a clean
OpenUSD tree, with `patches/openusd` retaining only its README. The next
executable slices are `UsdGenSessionCooker` integration, event-driven
off-owner retirement, per-task peak resource admission, and semantic branch
lowering with versioned dirty/cache ownership and eviction. They must preserve
the full hierarchy, fairness, admission, producer-retirement, and no-readback
requirements above rather than treating this checkpoint as completion.

## Bounded external review record

Two concurrent reviews were submitted to the required Hivemind endpoint using
only `qwen3.8-27b@q4_0`, with thinking disabled: one asked for dependency-ready
dirty/cache architecture and one for memory/retirement/interactive/backend
constraints. Artifacts are preserved at
`/tmp/usdgen-hivemind-architecture-request.json`,
`/tmp/usdgen-hivemind-architecture-status.txt`, and their `resource` peers.
The two request files exist (690 and 654 bytes); each status file records
curl transport code `000` (4 bytes including newline), and neither response
file was created. Both bounded calls therefore had no response body.
They contributed no findings and were not retried; the plan's task boundaries
and cache signatures above are derived from current repository inspection.

Two local Qwen responses were received with HTTP 200, IDs
`89535ee69c33d034` and `8f8db199143dac71`. Their useful bounded contribution
was owner-state and test-idea review only. Source-Qwen claims that `try_put`
backpressure, cancellation, or await behavior had been established were
rejected by coordinator/root inspection and are not evidence in this plan.

### Provider record: native Hivemind `qwen3.8-27b@q4_k_m`

Two concurrent read-only reviews used the native `/api/v1/chat` endpoint with
only `qwen3.8-27b@q4_k_m`, `reasoning: "off"`, `max_output_tokens: 1800`,
`temperature: 0`, `stream: false`, and `store: false`. Both curl requests
completed with HTTP 200 and exit status 0; their exact request, response,
stderr, status, and handle artifacts are in
`/tmp/usdgen-hivemind-q4km-0fLcfH` (also named by
`/tmp/usdgen-hivemind-q4km-dir.txt`). The native response schema contains no
request ID or finish-reason field. It identifies the same model instance for
both reviews, reports zero reasoning tokens, and reports exactly 1800 output
tokens for each, so these are cap-limited review outputs rather than complete
architecture evidence. The recorded request packets were 19,999 and 29,962
bytes, respectively; they exceeded the intended compact-context target and
must not be used as a precedent for future bounded reviews.

Repository inspection corroborates one limited CUDA finding: every valid
`UsdGenCudaExecutionQueue` obtains `GetOrCreate(runtime, "cuda", device)`,
so queues sharing a runtime and physical device address the same dispatcher.
The current stress tests intentionally use separate backend keys and therefore
do **not** demonstrate low-capacity contention, failure recovery, or
publication isolation across two real CUDA queues on that key. That is a
useful future regression slice. The session review correctly leaves
`UsdGenSessionCooker` integration OPEN, but its proposed owner-race and
publication claims were not independently established and are not accepted as
findings. All numbered phases and the overall plan remain **OPEN**.

### Session-stage integration checkpoint (limited validation)

`UsdGenSession::CommitAsync` now routes CUDA requests through `SubmitAsync`
and the shared backend/device task dispatcher. `UsdGenSessionCooker` prepares
the immutable plan and exclusive workspace, then submits real source,
individual operator, and final-generation tasks. It does not wait for that
task graph inside a pipeline callback. The pipeline retains its single active
cooker lane until asynchronous completion and submission-call return, so
superseded work cannot overlap a later cook on the same mutable workspace.
CPU reference cooking still uses its existing scheduler. Both lanes share
preparation, candidate construction, and terminal reply handling.

Failed stages and dispatcher rejection produce a retained-last-good candidate
with diagnostics, unchanged published commit/binding/node counters, and
uncleared dirt. Successful publication remains guarded by the pipeline epoch.
The async preparation epoch has one scope-guard writer, not competing writes
from submission and task completion. Terminal diagnostic construction is
best-effort: failure to allocate a diagnostic must not prevent the external
completion callback. This local guard is not proof of general host-allocation
failure recovery throughout the framework.

The new `testUsdGenCudaSessionTaskGraph` uses eight externally completed
sentinel roots and bounded atomic rendezvous, with no application mutex or
condition variable. It verifies:

- two sessions admitted on the same physical-device dispatcher, with exactly
  3 jobs / 17 tasks / 14 edges including the sentinel; a single wrapped cook
  cannot satisfy the stage-count assertion;
- Source → Length(.5) → Width(.03) device geometry via explicit test readback;
- an intermediate pending edit completing exactly once as superseded while
  old and newest requests are held, followed by only the newest publication;
- a successfully admitted CUDA job failing on a runtime non-finite expression,
  preserving the last good generation and dirty state, and recovering;
- real session rejection at the shared 256-job capacity, while tasks remain
  held, followed by capacity release and successful retry.

The dispatcher exposes independently sampled admission counters for
observability. These include reservations under validation, ingress, queued
and running work; they are neither an atomic snapshot nor a synchronous
acceptance acknowledgement.

This test first failed before any session submission: a concurrency-one
runtime did not service its sentinel without a caller entering a wait
(`/tmp/usdgen-session-stages-focused-1.log`, 10.38 s for the failed test).
The stock TBB arena constructor reserves one master-only slot by default.
The repo-local runtime now explicitly reserves zero master-only slots while
retaining the requested total concurrency. The generic one-worker task-graph
regression also now requires autonomous task and terminal completion before
calling `Drain`. No TBB or OpenUSD patch was added.

Root validation of the final source:

- clean main build: `/tmp/usdgen-session-stages-build-final.log`;
- T0/T1: **97/97**, 12.61 s,
  `/tmp/usdgen-session-stages-t0t1.log`;
- selected T2 stock-Storm/GL-interop regressions: **6/6**, 9.61 s,
  `/tmp/usdgen-session-stages-t2.log`;
- CUDA-disabled core build and selected tests: **4/4**, 0.07 s,
  `/tmp/usdgen-session-stages-cpu-{build,tests}.log`;
- new CUDA session regression: compute-sanitizer memcheck and initcheck both
  report **zero errors**, `/tmp/usdgen-session-stages-cuda-{memcheck,initcheck}.log`;
- freshly compiled standalone ASan/UBSan/LSan task-graph and stress tests:
  **100/100 each**, `/tmp/usdgen-session-stages-sanitizer-O3Zujp`.

The pipeline sanitized loop again failed: after 19 passes, run 20 reported
1040 bytes in two `task_stream::initialize` → arena/runtime allocations
(`testUsdGenExecutionPipeline-run-20.log` in that directory). This matches
the allocation-stack class and byte count observed in the previously checked
HEAD baseline, but does not identify ownership/cause or exclude a current
change's contribution. The leak remains **UNCLASSIFIED**; no suppression was
added, and the sanitizer-repeat gate remains **OPEN**. The existing install
test's successful `SKIP` and the lack of renderer device-handoff proof remain
explicit caveats to the suite counts above.

Actual local Spark Qwen reviews also completed in this slice, model
`qwen3.8-flash-next`, two concurrent requests, HTTP 200/curl exit 0:
`/tmp/usdgen-local-qwen-session-retire-pgz3Af`. Session review
`chatcmpl-8f3208fc77bc8c04` reached its output cap; retirement review
`chatcmpl-84ca5af85f064e88` finished normally. Only source-corroborated findings
were used. The alternate Hivemind `qwen3.8-27b-km` returned only question marks
in bounded probes; the user-selected `qwen3.8-27b@q4_k_m` subsequently passed
its probe and supplied the two reviews recorded above. Root verified only
that latter model was loaded on Hivemind. Neither provider was replaced with
an OpenAI inference endpoint.

This closes a session integration gap, **not a numbered phase**. Stages still
contain synchronous CUDA fences; owner-side capture/snapshot destruction can
still reach synchronous frees. Off-owner event-driven retirement, task peak
byte admission, bounded command ingress, semantic branch lowering, versioned
cache/dirty ownership and eviction, CUDA Graph specialization, device renderer
handoff, and performance/soak validation remain required. Metal/Vulkan adapters
remain unimplemented. All phases and the overall objective stay **OPEN**.

## Published CUDA generation retirement checkpoint

2026-09-12. This advances phases **2 and 6**, without closing either phase.
The implementation is repo-local; the stock OpenUSD source tree remains
unchanged and no renderer or custom rprim was introduced.

`executionRetirement.{h,cpp}` adds a backend-neutral retirement service to the
shared `usdGenExecutionResources` DSO. Each backend/device service preallocates
a fixed slot array (CUDA defaults to 1024 slots). A move-only ticket reserves
capacity before a published generation or consumer is accepted; a separate
copyable completion signal can arrive before or after payload retirement.
Retire returns acceptance of the retained payload, not immediate dispatch.
One packed generation/flags CAS state prevents stale callbacks from modifying
reused slots; the first terminal success/failure wins. Abandoned tickets also
advance generation, and generation exhaustion prevents reuse.

Completed payloads are dispatched to a dedicated TBB cleanup arena, without
application mutexes, polling, or recurring cleanup scans. The drain graph is
constructed inside that arena. Payload destruction and slot recovery precede
release of its drain credit; nested retirements remain covered by the same
external drain. Explicit external `Drain()` cooperates with the arena and may
execute cleanup on its caller. Ordinary signal/retirement calls do not wait for
CUDA completion or invoke payload cleanup inline. TBB enqueue/allocation
internals are not claimed allocation-free or hardware lock-free.

`Shutdown()` closes admission only. Pre-admitted tickets remain valid and must
eventually report their backend outcome. Unsignalled retired work keeps Drain
open; callers must quiesce/resolve producers. The process-lifetime registry
and quarantined payloads cannot be unloaded while native callbacks are live.
Drain rejects retirement-worker reentry. A shared guard spanning retirement
and all core pipeline/task callbacks is not yet implemented.

`cudaGeneration.cpp` now transfers published owners' preallocated payloads to
retirement on final release. Consumer payloads retain the source owner and its
transitive base generations, record a terminal event, and signal through the
private CUDA callback adapter. The callback copies its state before signalling
and makes no CUDA calls. The cleanup worker binds the owning device, verifies
the event once, destroys the event, and releases retained owners. Successful
backend frees return byte permits; unproven completion/free failures retain
resources conservatively. Full retirement admission rejects a completed,
unpublished factory input with ordinary safe factory-side cleanup, **not** a
permanent resource leak.

The adapter uses `cudaStreamAddCallback`, outside capture, because CUDA reports
device errors to that callback whereas `cudaLaunchHostFunc` may not execute
after a context error. NVIDIA lists AddCallback for eventual deprecation and
removal; this is an explicit adapter-maintenance risk, not an API guarantee
for future CUDA versions. Native callbacks must not call CUDA APIs or wait on
dependent work. See the [CUDA stream callback contract](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__STREAM.html)
and [host-function contract](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__EXECUTION.html).
Acquisition checks capture state before querying stream provenance; the
opposite ordering failed the capture-preservation regression on this CUDA
runtime. This boundary does not implement CUDA Graph specialization.

Root-reviewed regression coverage:

- neutral early/delayed and first-wins signals; completed and abandoned stale
  signals; capacity reuse; incomplete/failed-destroy quarantine; 1000 concurrent
  signal/Retire handoffs; autonomous off-caller cleanup observed before Drain;
  nested retirement; and completion of pre-admitted work after shutdown;
- CUDA charged-owner retention behind a bounded, test-only host callback gate
  followed by queued device memset; release returning before that gate opens;
  completed factory rejection recovering its bytes; stream destruction before
  retirement drain; and capture rejection preserving an otherwise valid graph;
- existing session weak-owner assertions now synchronize/drain before expecting
  final expiry; they do not assume asynchronous cleanup happens immediately.

The initial CUDA test used a wait on an unrecorded event, which does not hold a
stream; that fixture was replaced before acceptance. Gate teardown opens and
synchronizes before CUDA-owning locals are destroyed, including assertion
failure paths. An initial arena-affinity test also incorrectly assumed an
external cooperative Drain could not execute cleanup; the corrected test
observes autonomous completion before entering Drain.

Root validation on the final implementation:

- main build without warnings/errors:
  `/tmp/usdgen-retirement-build-final3.log`;
- T0/T1 **99/99**, 13.09 s:
  `/tmp/usdgen-retirement-t0t1-final3.log`;
- CUDA-disabled build and selected core tests **4/4**, 0.10 s:
  `/tmp/usdgen-retirement-cpu-hygiene-{build,tests}.log`;
- selected stock-Storm/GL interop tests **6/6**, 9.58 s:
  `/tmp/usdgen-retirement-t2.log` (before the allocator-only registry change;
  these remain regressions, not native device renderer-handoff proof);
- CUDA retirement memcheck and initcheck report **zero errors**:
  `/tmp/usdgen-retirement-cuda-{memcheck,initcheck}-final.log`;
- neutral and CUDA retirement tests each pass **50/50** repeated executions,
  21.66 s total: `/tmp/usdgen-retirement-repeat.log` (before the final test-only
  change to use a wall-clock deadline and drain safely on affinity-test failure;
  the final full suite above includes that change).

The standalone ASan/UBSan/LSan check is **NOT clean**. Artifacts are in
`/tmp/usdgen-retirement-sanitizer.hKVzUf`. Its initial report was 163992 bytes /
161 allocations. Constructing the drain graph in the cleanup arena removed
the unnecessary default-arena attachment; an allocator-only comparison then
made the registry's retained service/control-block tree visible to LSan.
The registry now explicitly uses `std::allocator` for map metadata; ownership
and process-lifetime retention are unchanged. The final behavioral test has
no assertion failure, but `run-final.log` still reports **9504 bytes in 36
`task_stream::initialize` / arena allocations**. That remaining report is
unresolved; no leak suppression was added and the sanitizer gate stays OPEN.
It does not resolve the separately recorded core-pipeline leak.

Real model reviews used local Spark `qwen3.8-flash-next` and Hivemind
`qwen3.8-27b@q4_k_m`. The complete-source batch used two concurrent requests
per provider, all curl exit 0 / HTTP 200, in
`/tmp/usdgen-retirement4.l7hmqc`. Local completions
`chatcmpl-9d721c9b370bea54` and `chatcmpl-aa97f6d4ee606dcf` both ended normally.
Earlier batches with missing/truncated implementation snippets were not
accepted as lifecycle-review evidence. Model allegations without a reachable
source-level interleaving were not treated as established bugs. Terra performed
implementation and a separate LSan audit; root reviewed sources and ran builds
and tests. No OpenAI inference endpoint replaced either requested provider.

Remaining scope is substantial: raw scratch/workspace destruction and CUDA
stage fences can still synchronize; task peak-byte admission, eviction,
categorized accounting, bounded command ingress, semantic branch/reference
lowering, versioned multi-groom cache/dirty ownership, CUDA Graphs, stock-renderer
device handoff, platform adapters and performance/soak gates remain required.
This checkpoint proves neither arbitrary-graph coverage nor full GPU occupancy
or an interactive-latency bound. All numbered phases and the full objective
remain **OPEN**.

## Bounded owner-command ingress integration checkpoint

The command side of phase 3 now has separate bounded admission from work
requests. `UsdGenExecutionPipeline` defaults to 4096 request credits and 4096
command credits. Ordinary commands and externally queued cancellation consume
command credits; already-admitted work retains its existing request credit
through its terminal reply. A move-only command ticket reserves delivery
before an external producer starts. A durable latest-value mailbox reserves
one slot for a persistent producer. Owner commands release their one-shot
credit before invoking user code, including cancellation callbacks, so
capacity-one reentry does not require an unbounded internal bypass.

This is a **count bound, not a byte bound**. Arbitrary closure captures,
descriptors, per-scene populations and aggregate pipeline counts are not yet
covered by a complete CPU peak-byte budget. Framework allocation can still
fail after admission; required terminal paths retain explicit fatal-invariant
handling, and a broken mailbox discards its latest pending closure. No
allocation-free or hardware-lock-free claim follows from these APIs.

Integration reserves lifecycle capacity on both sides of cross-owner actions:
Store attach/detach, session callback registration/unregistration, Groom
acknowledgments, cook completion and close. Context updates and persistent
republish callbacks use durable mailboxes. Public core mutations, staging,
context and callback-unregistration wrappers report admission failure instead
of silently dropping ordinary full-queue mutations. A staged descriptor is
consumed only when the core accepts it: retaining it after acceptance caused
the next CUDA tool commit to re-send a structural descriptor and invalidate
the active edit; the CUDA tool regression caught this and the fix preserves
staging on rejection without replaying it after acceptance.

Groom ingress reserves a command before allocating its sequence. A rejected
source notice leaves a deferred full-capture obligation, not a missing
sequence that synchronization will wait for forever. Source-namespace recovery
uses an immutable owner-queued path/type shadow plus a full resync diff,
delivered together with the synthetic catalog. Regression tests now cover
source changes outside groom dependencies, ancestor removals/type changes,
and a rejected ingress followed by a later accepted ingress. They check
actual delivered notices, not just queries of the live upstream scene. The
deferred obligation is consumed at admitted frontend capture, never cleared
by an older owner packet that could erase a newer rejection. `asyncPoll`
remains a frontend publication boundary, not a capture/cook entry point.

Validation so far (before the final source-recovery integration audit):

- Focused pipeline/core/imaging/store/CUDA-tool tests: **5/5**, 1.88 s,
  `/tmp/usdgen-command-focused-tests-4.log`.
- CUDA-disabled core build and selected tests: **4/4**, 0.21 s,
  `/tmp/usdgen-command-cpu-{build,tests}.log`.
- Selected stock-Storm/GL tests: **6/6**, 14.77 s,
  `/tmp/usdgen-command-t2.log`; not native GPU renderer-handoff proof.
- CUDA tool memcheck: **zero errors**,
  `/tmp/usdgen-command-cuda-tools-memcheck.log`.
- Full T0/T1 build-5 run: **98/99**, 34.95 s,
  `/tmp/usdgen-command-t0t1-5.log`. The pressure-recovery test passed;
  its later async assertion still expected the pre-recovery width and needed
  its baseline corrected. Earlier compilation also caught a missing private
  hook definition and mixed const/mutable iterator declarations.
- Repeated command tests exposed a new fixture race: the callback-cleanup
  test submitted an ordinary command before its reserved unregister had
  released the saturated source-owner credit, then waited for a rejected
  command. The test now waits for unregister acknowledgment before verifying
  a successful publication with no callback. This checkpoint does **not**
  count the failed repeat run as a passing soak gate.

An additional phase-3 blocker is explicit: Groom's frontend publication FIFO
is still unbounded when an async consumer stops polling. Command admission
does not bound that backlog. The next required change is a bounded latest
presentation snapshot with a frontend net namespace/tile diff against the
actually delivered snapshot. Retaining/merging historical notice vectors is
not sufficient: they too can grow with the number of edits, and skipped
generation reports are not valid diffs from the displayed baseline. The
replacement must prove ancestor/type-change reconstruction, authored-versus-
synthetic collisions, release of superseded generations, and no capture or
backend execution from getters/asyncPoll. Source shadows should be shared by
unchanged snapshots rather than copied for every empty completion packet.

Recovery also reconstructs retained synthetic scopes/tiles below a removed
source ancestor, re-adds surviving source descendants after an ancestor type
change, preserves authored tile collisions, and deduplicates/parent-orders
additions. Surviving source paths are universally dirtied because dropped
value notices cannot be reconstructed precisely. An owner-side source
sequence guards against a delayed full capture replacing a newer namespace
baseline; that source guard has been reviewed, but a dedicated forced stale
full-recovery fixture remains to be added. Existing delayed ordinary-capture
coverage is not a substitute for that exact interleaving.

Failed capture recovery now occurs at the next explicit `Synchronize()`
boundary, or the next admitted ingress. The incremental-capture regression
was updated to assert full traversal and stable session identities at that
earlier recovery boundary, then zero unrelated groom traversal afterward.
Scope/Xform fixture assertions compare the real source Hydra prim type and
membership: a USD schema name is not necessarily a Hydra prim type token.

Phase 3, byte admission, asynchronous CUDA-stage completion, multi-groom cache
ownership, semantic graph lowering, stock-renderer device handoff and the
other numbered gates remain **OPEN**. Previous sanitizer leak findings remain
unresolved; this command checkpoint does not supersede them.

Actual model review attempts used two concurrent requests per provider. The
corrected local Spark batch is `/tmp/usdgen_spark_native_review_2543008`
(`qwen3.8-flash-next`, thinking disabled): ticket response
`chatcmpl-a64e5ecbc8da112b` ended with `stop`; cleanup response
`chatcmpl-aad0fdf304e81264` hit its 2400-token limit. The corrected Hivemind
native batch is `/tmp/usdgen_hivemind_native_review_2540464`
(`qwen3.8-27b@q4_k_m`): both curl processes completed successfully with
HTTP 200, but both outputs hit 2400 tokens mid-analysis. Initial requests
using the wrong native payload/thinking configuration were rejected or
produced no usable answer and are not review evidence. No OpenAI inference
endpoint substituted for either provider. Root rejected the model claims
about an incorrect Await graph and ticket leaks: they contradicted the
external `reserve_wait` completion protocol and existing explicit/RAII credit
release. This batch yielded no newly verified defect; its successful HTTP
responses are not a correctness gate.

Final root validation for this command/source-recovery checkpoint:

- Main build succeeds with no warning/error diagnostic in
  `/tmp/usdgen-command-build-final.log`.
- Full T0/T1 **99/99**, 13.53 s:
  `/tmp/usdgen-command-t0t1-final2.log`. The preceding full run in
  `/tmp/usdgen-command-t0t1-final.log` was **98/99** because its old
  incremental-capture assertion expected recovery after, rather than during,
  the explicit synchronization boundary. The final run includes the corrected
  full-recovery-and-resumed-incrementality assertions.
- Pipeline, core async session, imaging async session, Store owner and scene
  publication each pass **30/30** repetitions (150 executions, 21.39 s):
  `/tmp/usdgen-command-repeat-final.log`.
- Updated incremental capture passes **30/30**, 5.02 s:
  `/tmp/usdgen-command-incremental-repeat.log`.
- Final selected stock-Storm/GL interop regression run **6/6**, 9.63 s:
  `/tmp/usdgen-command-t2-final.log`.
- The CUDA-disabled **4/4** and CUDA tool memcheck **zero-error** results
  recorded above cover the unchanged core command/tool implementation;
  later source-recovery edits were confined to imaging and its tests.
- `git diff --check` passes; the external OpenUSD source worktree remains
  clean. No OpenUSD patch or renderer replacement was added.

This is progress within phase 3, **not completion of phase 3 or the full
execution goal**. The next implementation is the bounded latest-snapshot
frontend publication protocol described above, including tests that stop
polling while generations keep completing and verify superseded owners are
released. Subsequent byte-budget, cache, semantic DAG, asynchronous CUDA,
platform and renderer-handoff gates remain unchanged.

### Latest-target publication and bounded sequence window checkpoint

Frontend publication now retains one atomically exchanged pending snapshot,
not a FIFO of completed generations. An ordinary `asyncPoll` installs one
target and derives net source/synthetic namespace notices against the visible
snapshot. Source stamps, absolute-root stamps, synthetic identity/generation,
and source-versus-synthetic ownership changes determine resyncs; unchanged
paths are not universally dirtied on every cook. Parent removals re-add
surviving descendants. Unpolled remove/re-add history intentionally coalesces
to the final net resync. The visible snapshot is installed before callbacks.
Authored upstream `GetPrim` data sources and child passthrough remain intact;
the namespace diff itself performs no upstream query, capture or cook.

A separate neutral `UsdGenExecutionSequenceWindow` bounds outstanding
sequence span with a fixed completion ring (default 4096 in Groom). A held
early capture previously pinned history while later completed commands
recycled admission credits indefinitely. Now window exhaustion rejects ingress
before capture or sequence allocation and retains one full-recovery request.
One scheduled owner advances the contiguous prefix; concurrent producers
reserve through atomic counters. This uses no application mutex or waiting
producer loop and makes no hardware-lock-free claim.

Regression evidence includes 4098 distinct CPU-reference width generations
with frontend polling stopped: pending count stays at most one, superseded
snapshot/TileMap weak owners expire, old visible geometry stays unchanged,
and one poll exposes width 4098 without increasing capture/cook counts.
4098 source notices coalesce without unrelated universal dirties. A separate
capacity-eight stalled-prefix fixture proves bounded issued span and
event/tombstone counts, no source reads or sequence allocation when full,
then recovery of final source state and reusable capacity after the gap closes.
These are count/ownership checks, not total CPU/GPU byte-budget or native
renderer-handoff proofs.

Root validation for this checkpoint:

- Main build succeeds: `/tmp/usdgen-publication-build-2.log`.
- Full T0/T1 **100/100**, 13.45 s:
  `/tmp/usdgen-publication-t0t1.log`.
- Four focused tests each pass ten repetitions (40 executions), 32.94 s:
  `/tmp/usdgen-publication-repeat.log`.
- Selected stock-Storm/GL interop tests **6/6**, 9.55 s:
  `/tmp/usdgen-publication-t2.log`.
- CUDA-disabled sequence window, pipeline and async session **3/3**, 0.06 s:
  `/tmp/usdgen-publication-cpu-tests.log`.
- Neutral window alone passes 50 CTest repetitions and 30 standalone
  ASan/UBSan/LSan repetitions: `/tmp/usdgen-sequence-window-repeat.log`,
  `/tmp/usdgen-sequence-window-sanitizer-repeat.log`, executable under
  `/tmp/usdgen-sequence-window-check.J3LpzM`. This does **not** resolve the
  previously recorded TBB arena/pipeline sanitizer leaks.

Two real Spark and two real Hivemind reviews of the pre-change baseline are
recorded under `/tmp/usdgen_groom_provider_review_2564190`. All four curl
requests completed successfully without truncation; the verified backlog and
stalled-prefix findings informed the implementation. These were baseline
reviews, not an independent audit of the final code. No OpenAI inference
endpoint substituted for either provider.

Phase 3 and the full goal remain **OPEN**. The next verified retention gaps
are owner `usedSessions` duplicate/expired weak entries, completed scene
retirement records, and registration history retained until an optional
external drain. Fixing these requires scheduled lifecycle reclamation that
preserves the final-reference-to-cleanup-enqueue gap, not merely pruning at
optional drains. Aggregate live-population and closure/notice byte budgets,
semantic DAG/cache work, asynchronous CUDA stages, platform adapters,
stock-renderer GPU handoff and performance/soak gates remain outstanding.

### Scheduled scene-lifecycle reclamation checkpoint

2026-09-12. The historical scene-service registration queue/vector is replaced
by an event-driven registry actor with an owner-only keyed map. Construction
publishes a complete registration before returning. Final-reference deletion
still runs on the separate unlimited cleanup node: waiting destruction never
occupies the short serial registry lane. After deletion, a registry event
erases the entry and only then signals its retirement record. Thus normal
retirement releases registry history without an optional external drain, and
an explicit drain's completion includes erasure of the records it waited for.

External snapshots use a separate reserve/release reply graph for each
request, with exception propagation and guaranteed reply signalling on copy
failure. They do not wait/reset the shared live registry graph. The registered
record remains present across final-reference loss and the gap before cleanup
enqueue; it cannot be discarded merely because its weak State has expired.
Exclusive process shutdown finishes cleanup and then registry work before
destroying either catalog or service. The existing single-external-drainer
contract is explicit in the header: this change does **not** claim concurrent
`DrainRetired()` waits on a shared per-record TBB graph are supported.

The State owner now fully prunes expired `usedSessions` weak references and
deduplicates live session identities at attachment. Finding a duplicate does
not stop pruning the rest of the vector. Distinct still-live historical
sessions remain available for coordinated process shutdown.

`testUsdGenSceneOwner` now verifies:

- 256 serial scene create/destroy cycles plus four concurrent producers with
  32 cycles each reclaim actual registry entries and retirement-record objects
  to zero without calling `DrainRetired` to trigger reclamation. Test-only
  bounded polling observes the scheduled result; the registry barrier merely
  observes its lane and does not force cleanup or perform reclamation.
- Two separate source indexes share one Store key. An anchored live session
  survives real target detach/reattach cycles without duplicate bookkeeping.
  A separate expired tail is observed before the first reattachment, then
  immediately disappears while the live session is retained exactly once.
- A single-claim test gate pauses the final deleter before cleanup enqueue.
  The registered weak State is expired while its record remains. A second
  gate proves the external drain actually reached that pending expired record;
  it cannot return until deletion is released. All records are then reclaimed.

These are lifecycle fixtures, including deliberately empty/rejected cook
descriptors, not valid-geometry throughput or GPU residency tests. Actual
record counters cover retained callback copies as well as catalog entries.
The registry's in-flight event queue and live scene population still need
aggregate admission/byte budgets; this removes historical accumulation, not
all possible memory pressure. No allocator-failure/OOM guarantee or hardware
lock-free claim follows from using scheduled actors and atomic ownership.

Root validation:

- Main build succeeds: `/tmp/usdgen-lifecycle-build-1.log`.
- Focused scene/registry/publication tests **6/6**, 3.54 s:
  `/tmp/usdgen-lifecycle-focused-1.log`.
- SceneOwner, SceneExit and RegistryOwners each pass **50/50** repetitions
  (150 executions), 20.84 s: `/tmp/usdgen-lifecycle-repeat.log`.
- CUDA-disabled build and the same lifecycle tests **3/3**, 0.22 s:
  `/tmp/usdgen-lifecycle-cpu-build.log`,
  `/tmp/usdgen-lifecycle-cpu-tests.log`.
- Final full T0/T1 run **100/100**, 24.84 s:
  `/tmp/usdgen-lifecycle-t0t1-final.log`. The earlier parallel run overlapping
  the separate CPU build was **97/100**: CUDA stream creation failed in
  CurveIndicesStorm, CurveSource and Style, including an explicit out-of-memory
  diagnostic (`/tmp/usdgen-lifecycle-t0t1.log`). All three immediately passed
  standalone (`/tmp/usdgen-lifecycle-cuda-recheck.log`) and in the final full
  run without a concurrent build. The exact transient resource cause was not
  established. Local Spark remained running and was not reconfigured.
- Selected stock-Storm/GL interop tests **6/6**, 9.61 s:
  `/tmp/usdgen-lifecycle-t2.log`. Assertion-level lifecycle output is in
  `/tmp/usdgen-lifecycle-owner-verbose.log`.
- `git diff --check` passes and the external OpenUSD source remains clean.
  Previous sanitizer leak findings remain unresolved and were not superseded
  by these non-sanitized lifecycle tests.

Actual provider review used two concurrent requests each to local Spark
`qwen3.8-flash-next` and native Hivemind `qwen3.8-27b@q4_k_m`, with no OpenAI
endpoint substitution. Artifacts: `/tmp/usdgen_lifecycle_provider_review_2585556`.
Spark responses `chatcmpl-b4550d22a9f12f50` and
`chatcmpl-8a74d48ef03b5640` finished with `stop`; both Hivemind requests
returned HTTP 200 (1024 and 1157 output tokens), and all curl processes exited
zero. Baseline reviews supported preserving per-record gap protection and
session deduplication. The claim that the old states vector was consumed by
drain was rejected against source. Recommendations to prune only at optional
drains did not meet automatic reclamation requirements; root/Terra developed
and reviewed the scheduled registry implementation instead.

Phase 3 and the full goal remain **OPEN**. Next concrete CUDA execution slice:
`PrepareCudaJobSource` calls `CudaCurveSource::Set` and `Finish`; the latter
still performs two stream synchronizations in `gpu/curveSource.cu`. Both
`cudaExecutionQueue.cpp` and `sessionCooker.cpp` reach this source boundary
from their production task graphs. Add a shared asynchronous source-upload
completion API and wire both paths to delayed task completion, preserving job,
staging, diagnostics and workspace lifetime through success/failure. CUDA
callbacks must only signal/schedule a retained completion, not free CUDA
resources or execute CUDA work. Keep any existing synchronous API for explicit
external consumers. Resampling, operator finishes, final tile/status metadata
readbacks and scratch/workspace retirement need subsequent asynchronous work;
removing source fences alone will not close phase 6. Semantic DAG, cache,
platform-adapter, stock-renderer handoff and performance gates remain unchanged.

### Asynchronous CUDA source-upload completion checkpoint

2026-09-12. Both production source tasks (`cudaExecutionQueue.cpp` and
`sessionCooker.cpp`) now call the shared `ExecuteCudaJobSourceAsync` primitive.
Preparation validates/captures the source and enqueues `Set`, then returns
without calling the two-fence legacy source `Finish`. A status-bearing stream
callback signals a reserved completion slot. A separate scheduled worker
commits the fresh source, performs the existing optional resampling, and
completes the task once. Dependent operators cannot run before this completion.
Legacy synchronous stage/graph APIs remain available and unchanged in purpose.

The source relay has **1024 process-global slots**, a separate **eight-worker
TBB arena with no reserved master slot**, and no application mutex. Admission
precedes source upload. Terminal status and launcher return use an atomic
two-stage handoff; status is published before readiness and duplicate terminal
signals are rejected. Native callbacks see stable slot storage, do not own the
last job reference, and perform no CUDA query, free, user callback or wait.
Completion workers select and restore the correct CUDA device. A normal slot
is reclaimed before delivering successful task completion.

`cudaStreamAddCallback` is used for its device-error notification contract;
capture is rejected before upload in the high-level API. The low-level fresh
API also detects capture before stream-device queries. This callback API is
slated for eventual removal, so its replacement remains an adapter maintenance
requirement; `cudaLaunchHostFunc` alone would not provide the same device-error
notification behavior. See [NVIDIA stream callback documentation](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__STREAM.html).

Callback-install failure after upload, callback error, or an unproven
post-upload failure produces one scheduled failure result while permanently
retaining its charged slot and source payload. The workspace is poisoned
before user completion, all execution paths reject its reuse, and destroying
its wrapper detaches the native implementation rather than freeing unproven
buffers. Retained jobs clear borrowed workspace/diagnostic pointers and the
completion closure, so quarantine does not retain a dispatcher callback or
later dereference a destroyed wrapper. Last-good queue publication survives.
This is conservative quarantine, not automatic device recovery or eviction.

The relay service and its fixed framework state intentionally have process
lifetime, covering late callbacks during static scene/session teardown.
Normal job payloads are reclaimed; fault slots are never recycled. This avoids
an unsafe static-destruction boundary, but does **not** establish unload-safe
library teardown or a clean sanitizer/leak baseline. Slot count is not a byte
budget, and queued framework task allocation can still fail. Aggregate
workspace, staging, cache and driver headroom accounting remain open.

The low-level `CudaCurveSource` now distinguishes fresh async commit from
generic replacement. A fresh commit cannot replace an existing active source;
legacy `Finish` still fences active consumers before replacement. An unsafe
upload marker records missing completion proof. A later successful legacy
finish clears that marker only after both existing fences prove completion.

New regression coverage:

- Queue and session tasks hold a test-only stream callback after upload while
  an unrelated CPU pipeline runs on their **one-worker runtime**, proving the
  source launcher returned before GPU completion. Completion remains absent
  until release, then occurs once with valid geometry.
- Direct async source stages match sequential Length/Width/RBF output;
  capacity-one pressure rejects a second source before upload and capacity
  is reusable after completion. Cancellation preserves the previous queue
  snapshot and reports one superseded outcome.
- Injected callback-install rejection and error status from a real callback
  each fail once, retain charged quarantine, and prevent workspace reuse.
  Queue failure retains last-good geometry. These are controlled injections,
  not a real device-loss recovery test.
- Low-level fresh/nonfresh rejection, capture rejection and subsequent
  proven legacy recovery are tested. Gate callbacks use only test-only bounded
  atomic/yield waiting; they do not enter TBB's cooperative wait, which could
  execute unrelated CUDA work on the native callback thread.

This advances phase **6**, but leaves it and the full goal **OPEN**. Source
expression evaluation/scalar reads, optional resampling, operator finish
methods, final tile/status metadata readbacks and scratch/workspace retirement
still contain fences. Pageable upload staging and allocation can also block
inside the CUDA driver; this change proves removal of the explicit source
completion fences, not a fence-free or latency-bounded interactive path.
Metal/Vulkan adapters, semantic DAG/cache work, CUDA Graph specialization,
stock-renderer GPU handoff and performance/soak gates remain unchanged.

Source-upload checkpoint validation (2026-09-12):

- Full CUDA build passed. The first focused run exposed capture-check ordering
  in the low-level fresh API; moving capture detection before stream-device
  queries fixed that regression. The rebuilt focused set passed **6/6**.
- Full T0/T1 passed **100/100** (24.33 s). Queue, stage, session-task-graph and
  low-level source tests each passed **30 consecutive repetitions** (120 test
  executions, 48.31 s).
- CUDA Compute Sanitizer memcheck reported **zero errors** for the queue and
  execution-stage tests. This is not a leak-clean claim: fault injection
  deliberately retains quarantine and older framework leak findings remain open.
- Selected stock-Storm/GL checks passed **6/6** (9.49 s); the CUDA-disabled
  build and execution-pipeline/async-session/scene-owner checks passed **3/3**.
- Evidence: `/tmp/usdgen-async-source-build-2.log`,
  `/tmp/usdgen-async-source-focused-2.log`,
  `/tmp/usdgen-async-source-t0t1.log`, `/tmp/usdgen-async-source-repeat.log`,
  `/tmp/usdgen-async-source-memcheck-{stages,queue}.log`,
  `/tmp/usdgen-async-source-t2.log`, and
  `/tmp/usdgen-async-source-cpu-{build,tests}.log`.
  `git diff --check` passed; external OpenUSD source remains clean.

Actual baseline reviews ran two requests each to local Spark
`qwen3.8-flash-next` and native Hivemind `qwen3.8-27b@q4_k_m` without OpenAI
endpoint substitution. All four returned HTTP 200 and curl exit zero.
Artifacts use prefix `/tmp/usdgen_cuda_async_review_20260912_`;
Spark response IDs were `chatcmpl-ba46539a199977a3` and
`chatcmpl-a94d2430ec04e04d` (both `stop`). Hivemind returned 1021 and 1118
output tokens. Reviews informed lifetime/status/capture checks; they do not
constitute an independent audit of the final implementation.

The next phase-6 slice is optional resampling. Its existing `Apply` synchronizes
for input validation, and `Finish` synchronizes for kernel status (and previous
output consumers). A fresh-only asynchronous API must retain full input
validation, guard resampling on the preceding device validation result, and
copy terminal status into retained pinned host storage. The source relay must
keep the same admission slot through both phases, with separate callback
identities and launcher/terminal gates so the first callback cannot race a reset
second-phase gate. No source completion or dependent operator admission may
occur until resampling has completed successfully. Legacy replacement fences
remain for synchronous callers; removing them from the fresh production path
does not close the other phase-6 fences or the full goal.

### Asynchronous CUDA resampling checkpoint

2026-09-12. The production async source path now retains its original relay
admission through optional resampling. The upload completion worker commits
the source, submits fresh resampling and returns; a second scheduled completion
commits the resampled result before admitting dependent operators. Both queue
and session task graphs use this shared path. It does not call the legacy
resample `Apply`/`Finish` waits. Legacy explicit synchronous callers retain
their validated replacement behavior.

`CudaCurveResample::ApplyFresh` performs host shape checks, allocates all output,
device status and pinned host status storage, then submits validation and
resampling in stream order. Every resampling thread checks the device error
atomically before dereferencing input offsets. Invalid device topology and
nonfinite values therefore remain rejected without a host validation fence or
a trusted-input bypass. `FinishFreshAsync` enqueues the scalar status D2H into
retained pinned storage before a status-bearing stream callback.
`CommitFreshFinish` runs only after successful launcher return and native CUDA
completion, checks semantic status separately, and commits ownership without
CUDA calls. Geometry remains in GPU buffers; only the scalar status crosses
to the host. See [NVIDIA asynchronous execution guidance](https://docs.nvidia.com/cuda/cuda-programming-guide/02-basics/asynchronous-execution.html)
and [stream callback ordering](https://docs.nvidia.com/cuda/cuda-runtime-api/cuda_runtime_api/group__CUDART__STREAM.html).

The fixed source relay still has **1024 slots and eight scheduled workers**.
Each slot now contains separate permanent upload/resample callback identities,
status words and launcher/terminal gates. Worker messages identify the phase;
the first phase never resets the second phase's gate or reserves another slot.
Device selection encloses resample submission and is restored before the
launcher-return gate is published. Native callbacks only signal/schedule, with
no slot/payload access after posting can permit recycling. No production mutex,
spinlock, polling loop or retry queue was introduced.

Resource and failure behavior:

- The pinned host status payload obtains a `Scratch` resource permit before
  allocation. Successful host free releases it; failed/unproven free retains
  the charge. Payload bytes are accounted; driver/page-rounding overhead is
  covered only by headroom, not measured exactly.
- Outstanding fresh work is marked before the first submission. Failure to
  install a terminal callback after submission, native CUDA failure, and other
  missing-proof paths retain the existing slot/job/resampler, poison the
  workspace and report one failure while preserving last-good publication.
- A completed semantic validation failure, or a rejected resample before
  submission, can fail and release admission without poisoning. A normal
  superseded request is not a device failure and releases its slot after
  proven completion. Fault quarantine is still permanent, not device recovery.
- A resampler abandoned with unproven fresh work quarantines device storage and
  pinned status without CUDA queries, waits or frees. The earlier ready event
  alone cannot prove completion of the later D2H/callback. Generic `Apply` and
  `Finish` reject mixed-API reuse while fresh work is outstanding; a proven
  fresh commit permits subsequent normal synchronous replacement.

Regression coverage includes async resample plus Length/Width/RBF parity with
the sequential path, runtime-one queue/session CPU progress while a resample
stream callback is held, capacity-one admission held through resampling,
cancellation with last-good retention and reclamation, and phase-two callback
installation/native-status fault injections. A separate test-only host gate
holds the resample launcher after a real native callback has set terminal
status: the slot cannot be reused or task completed until launcher return.
Together the two gates exercise both arrival orders. Test gates use bounded
atomic/yield waits only in explicitly armed tests. Low-level checks include
early commit and duplicate finish rejection, capture rejection, fresh/legacy
API isolation, NaN status, and `{0, UINT32_MAX, 6}` malformed device offsets.

All four baseline review requests completed using two concurrent requests to
local Spark `qwen3.8-flash-next` and two to native Hivemind
`qwen3.8-27b@q4_k_m`, without OpenAI endpoint substitution. Each returned HTTP
200 and curl exit zero. Artifacts: `/tmp/usdgen-resample-review.wSiLwJ`.
Spark IDs: `chatcmpl-8dbf53bdcb74c8c8`, `chatcmpl-9a8963e2aea065fc` (both
`stop`, 1360/1365 output tokens). Hivemind returned 1142/849 output tokens.
Reviews informed lifetime and phase-order tests. Root rejected suggestions to
copy from CUDA callbacks, require mapped status memory, skip generic input
validation, free unproven allocations, or treat cancellation as device loss.
Terra implemented the isolated low-level, relay and test changes; root reviewed
the actual code and owns build/test validation.

Phase **6** and all full-goal gates remain **OPEN**. This removes explicit
resampling completion/validation fences from the fresh production path, not
all CUDA/driver blocking. Allocation and ordinary destructor/consumer cleanup
still have blocking behavior. Source-expression reads, operator stages,
compaction/RBF solves, finalization metadata and asynchronous scratch/workspace
retirement remain unfinished. Metal/Vulkan adapters, semantic DAG/cache work,
CUDA Graph specialization, stock-renderer GPU handoff, aggregate byte/peak
admission, fair interactive scheduling and performance/soak proof are unchanged.

Validation exposed an additional lifecycle defect before acceptance of this
checkpoint. The first full build, focused **4/4**, and T0/T1 **100/100** passed,
but repeated runs intermittently crashed in the stage and session executables.
The failure is not dismissed by the green single run. A low-overhead crash
tracer and an attach-after-failure debugger captured the main thread in
`_dl_fini`/cuSolver finalization while a retirement worker was executing
`CudaOwnerRetirementPayload::Destroy -> cudaGetDevice`. The leaked retirement
registry allowed backend cleanup to continue during CUDA library teardown.
Evidence: `/tmp/usdgen-async-resample-repeat.log`,
`/tmp/usdgen-async-resample-session-trace-2.log`, and
`/tmp/usdgen-async-resample-stopped-allthreads.log` (threads 1 and 6).
The next required correction is an irreversible backend-retirement quiescence
boundary: await already-entered cleanup before runtime teardown, and quarantine
later ticket releases without backend calls. This also needs explicit embedding
contracts for device reset/unload; a successful process-exit test alone cannot
prove arbitrary plugin-unload safety.

### Backend retirement quiescence checkpoint (limited validation)

The neutral retirement service now exposes an irreversible backend-wide close
shared through the execution-resources DSO. Close rejects future admission,
quarantines unscheduled retired payloads without payload virtual calls, and
framework-waits for cleanup that already crossed its backend gate. Its guarantee
is **no remaining backend cleanup after close returns**, not an instantaneous
ban on calls at the closed-flag store. Scheduled cleanup retains its drain credit;
the close scan cannot steal that credit. Retire reserves its credit before
publishing the installation state, so a paused installer cannot be omitted from
the drain. Late pre-admitted tickets retain their payload and release their
credit without CUDA calls. Registry insertion racing close rejects admission
without starting a second concurrent graph wait.

The CUDA adapter registers a process-exit fallback after initializing the
requested CUDA device, restores the caller's device, and fails admission if
registration fails. Duplicate first-use registrations are harmless when invoked
serially at exit. Explicit close is also available to embeddings. Callers must
stop CUDA clients before backend teardown and externally serialize close/drain;
the open checks in consumer APIs are not execution leases protecting arbitrary
concurrent device reset or library unload. Runtime-wide shutdown and clean
quarantine recovery remain separate requirements.

Deterministic neutral regressions cover unsignalled retirement, late tickets,
held active cleanup, and a paused installing ticket. CUDA regressions cover late
consumer release and a separately registered `testUsdGenCudaRetirementExit`
which drops a real generation and exits without a test-side drain. The first
build succeeded and six CUDA focused tests passed; the neutral fixture initially
failed because its held payload omitted its destructor counter increment. That
fixture was corrected; the subsequent repeat validation is recorded below.

Shutdown review used two concurrent Spark requests and two concurrent native
Hivemind requests, all HTTP 200/curl exit zero, archived in
`/tmp/usdgen-retirement-close-review.bJvblF`. Spark used
`qwen3.8-flash-next` (`chatcmpl-ab949f551727ca90`,
`chatcmpl-8318d1c8fa6a681b`); Hivemind used only
`qwen3.8-27b@q4_k_m`. Review suggestions were checked against the credit protocol
and observed teardown trace; they are not independent final-code validation.

The next operator slice is asynchronous Width **including runtime expression
parameters**. Removing Width's own status/publication waits alone would leave
context construction, program upload and expression validation blocking. The
candidate must retain these resources through terminal callbacks and publish
only after semantic validation, on both queue and session execution paths.
All numbered phase gates remain OPEN.

Root validation of the shutdown/resampling checkpoint, before compiling the
subsequent fresh Width/parameter work:

- CUDA configure/full build succeeded (`/tmp/usdgen-retirement-close-configure.log`,
  `/tmp/usdgen-retirement-close-build.log`). The corrected neutral test target
  rebuilt successfully and passed **100 consecutive runs** in 4.21 seconds
  (`/tmp/usdgen-retirement-close-neutral-{build,repeat}.log`). After an additional
  test-only early-failure cleanup guard, it rebuilt and passed another **100
  consecutive runs**, 4.40 seconds, in
  `/tmp/usdgen-retirement-close-neutral-final-{build,repeat}.log`.
- Ordinary, uninstrumented process-exit, execution-stage and session-task-graph
  executables each passed **100 consecutive runs**: 300 total, 240.50 seconds,
  `/tmp/usdgen-retirement-close-repeat-100.log`. No crash-tracer/preload or
  debugger altered these runs. This addresses the observed repeat failure;
  it is not exhaustive process-lifecycle or unload proof.
- T0/T1 passed **101/101**, 24.75 seconds, and the selected rendering/interop
  regressions passed **6/6**, 9.57 seconds:
  `/tmp/usdgen-retirement-close-t0t1.log` and
  `/tmp/usdgen-retirement-close-t2.log`. Rendering regressions do not establish
  the still-open native GPU generation handoff.
- CUDA Compute Sanitizer memcheck reported **zero errors** for resampling
  (including malformed-offset validation), execution stages and the queue:
  `/tmp/usdgen-retirement-close-memcheck-{resample,stages,queue}.log`.
  This is not a clean leak-sanitizer result; earlier TBB/host leak findings
  remain unresolved.
- CUDA-disabled execution-pipeline, async-session and scene-owner targets
  rebuilt and passed **3/3**, 0.23 seconds:
  `/tmp/usdgen-retirement-close-cpuoff-{build,test}.log`.
- `git diff --check` passed; the external OpenUSD source worktree remained
  clean. No renderer, custom rprim or OpenUSD patch was introduced.

The next Width implementation uses parent-controlled **context validation →
expression evaluation → Width** completion phases on one bounded operator
admission. A context batch must be proven valid before expressions can read
its fields; invalid geometry may leave those fields uninitialized. Fresh
context/program helpers enqueue retained scalar status copies; the parent
installs one terminal callback after each batch and requires both successful
native completion and launcher return before host-only commit. Program upload
must retain its host IR rather than synchronize to release borrowed input.
Low-level helpers and the candidate evaluator alone do not satisfy production
integration or the full asynchronous execution gate.

### Fresh Width and parameter primitives (limited validation)

Isolated implementation lanes now cover `gpu/width`, `gpu/expressionContext`,
`gpu/expression`, and `cudaParameters`, with corresponding low-level and staged
evaluator tests. These are not yet production async operator dispatch. Fresh
Width conditionally publishes on-device only after the validation kernels
report success; semantic errors leave the candidate output unchanged. Expression
contexts withhold `Inputs()` until proven host commit. Fresh expression upload
owns its host IR staging through terminal proof instead of synchronizing to
release the compiler's borrowed IR.

Root review rejected two unsafe intermediate designs before building them:
callback userdata pointing at a destructible Width object, and evaluator
destruction which could free context/literal/output buffers still borrowed by
pending expressions. Fresh native callbacks must use externally retained
identities. An unproven evaluator must retain the complete candidate, including
contexts whose own construction has finished but whose fields are still being
read by expressions. Literal upload itself counts as submitted work even if a
later program allocation fails. Failed host-memory frees must retain their
resource permits. These are review requirements, not assumptions that tests
have already established the final implementation.

The review artifacts are in
`/tmp/usdgen-width-parameter-review-20260912-161341`. Two original Spark requests
timed out at 180 seconds without responses. Two compact retries returned HTTP
200 (`chatcmpl-a39388f3614cc161`, `chatcmpl-a73c98644a3e17bd`), but exhausted their
1600-token budgets in internal reasoning with no answer content; they supplied
no usable review conclusion. Both Hivemind requests initially returned HTTP 400
because the launcher supplied the wrong native input key. Corrected native
requests returned HTTP 200 and usable phase/lifetime/test suggestions. Models
remained local Spark `qwen3.8-flash-next` and Hivemind
`qwen3.8-27b@q4_k_m`, at most two requests per provider, with no OpenAI inference
endpoint substitution. Root rejected recommendations to poll, make dynamic RBF
expression inputs compile-time constants, poison ordinary semantic failures, or
reclaim quarantine without lifetime proof. Runtime expression evaluation and
backend-neutral scheduled continuations remain required.

Root inspected the Spark request artifacts and identified the reasoning-only
cause: the launcher had put `enable_thinking` at the top level rather than in
`chat_template_kwargs`. A final compact pair with the correct nested setting
returned HTTP 200 and actual answer content: `chatcmpl-851ccf9fcfb9eeb0` (1301
completion tokens) and `chatcmpl-b5875cf35168af49` (1488). Raw `retry2` artifacts
remain alongside the earlier failures. Useful lifetime/barrier/test suggestions
were checked against the source; claims that the old context/program loops
were interleaved and suggestions to cancel native CUDA work were rejected.

The first primitive compile encountered a GCC 13 internal compiler error under
NVCC on an aggregate-temporary reset in `expressionContext.cu`. Replacing the
two reset temporaries with named local values compiled successfully, without
compiler flags, SDK changes or OpenUSD patches. The full CUDA build then
succeeded. Initial focused validation passed **6/7**; the staged evaluator test
identified a diagnostic mismatch (capture rejection returned `CudaError` instead
of the specified `InvalidArgument`). That API diagnostic was corrected
before accepting this checkpoint; the test was not weakened.

The corrected implementation passed root's full CUDA build, focused **7/7**
(2.85 seconds), T0/T1 **101/101** (24.57 seconds), and **30 consecutive runs
each** of Width, expression-context, expression-program and parameter-evaluator
tests (120 runs, 45.69 seconds). Logs:
`/tmp/usdgen-fresh-width-parameters-build-final.log`,
`/tmp/usdgen-fresh-width-parameters-focused-final.log`,
`/tmp/usdgen-fresh-width-parameters-t0t1.log`, and
`/tmp/usdgen-fresh-width-parameters-repeat.log`. Compute Sanitizer memcheck
reported **zero errors** for all four executables; initcheck reported **zero
errors** for Width, contexts and the staged evaluator. Artifacts:
`/tmp/usdgen-fresh-{width,context,expression,parameters}-memcheck.log` and
`/tmp/usdgen-fresh-{width,context,parameters}-initcheck.log`.
The six selected stock rendering/interop regressions also passed **6/6**, 9.51
seconds, in `/tmp/usdgen-fresh-width-parameters-t2.log`; this is regression
coverage, not native GPU renderer-handoff proof.

Both evaluator Commit methods now preserve GPU-owning candidates and move only
host metadata/ownership. A proven rejected candidate or the previous accepted
candidate occupies one bounded retired slot and is destroyed by the next Begin
or evaluator teardown, not by Commit. The immutable program retains pageable
literal-upload storage; program IR and scalar diagnostics have charged pinned
storage. Pageable literal H2D staging, ordinary allocation and next-Begin/
destructor cleanup may still block. Aggregate byte/peak limits and asynchronous
scratch retirement are not proved by these tests.

Production Width integration is the next separate checkpoint: an independently
bounded three-phase operator relay plus queue/session task-completion wiring
and cancellation/lifetime/admission tests. The above evidence precedes that
integration and must not be cited as proof of asynchronous production Width.
Phase 6, all other numbered gates, and the overall objective remain OPEN.

### Production asynchronous Width integration (validation in progress)

Both queue and session task-graph operator lambdas now use an accepted-callback
operator entrypoint. Width holds one independent fixed relay admission across
context, expression-program and Width terminal phases; other operators retain
their synchronous implementations. Each phase has a permanent callback identity
and separate terminal/launcher-return state. Admission is bounded to 1024 slots
with an eight-worker TBB arena. A job cannot start another operator while its
Width continuation is active. Device selection is restored before publishing a
phase's launcher-return gate or invoking user completion.

Profile/mask uploads belong to the retained Width candidate before the first
DMA. Ordinary invalid literal controls, rejected by Width before its own GPU
submission, still obtain terminal proof for those preceding uploads and fail
without poisoning the workspace. Missing native proof instead retains the
fixed slot, candidate, job and poisoned workspace; borrowed owner/diagnostic
pointers and completion closures are detached before reporting failure.
Previous width storage is retained across publication rather than destroyed by
the pointer swap. Native callbacks do not read shared relay payload ownership.

Tests cover mixed-domain numeric/bool expression parity, all three held phases,
capacity-one admission, native-terminal-before-launcher-return ordering, six
phase-specific callback-install/native-status faults, literal/expression semantic
failure recovery, one-worker CPU progress, cancellation/supersession, last-good
visibility and normal slot reuse. The initial integration build needed an exact
workspace friend declaration; no visibility broadening or SDK patch was used.
After that correction, focused **3/3**, T0/T1 **101/101**, and **100 consecutive
runs each** of queue, stages and session task graph passed (300 runs, 140.57
seconds). Logs: `/tmp/usdgen-async-width-integration-build-2.log`,
`/tmp/usdgen-async-width-integration-focused-1.log`,
`/tmp/usdgen-async-width-integration-t0t1.log`, and
`/tmp/usdgen-async-width-integration-repeat-100.log`.

Source review found an additional recovery gap: a rejected program allocation
before any literal upload left a prepared evaluator phase behind, rejecting
later jobs on the same workspace. `DiscardFreshProven()` now parks only a
proven, context-committed candidate in the bounded retired slot and returns the
evaluator to idle without CUDA calls or destruction. Published fields remain
owned and unchanged. Unproven context/program work cannot be discarded.
The production relay invokes this transition after a pre-submit program failure.
New primitive and production tests inject rejection before program allocation/H2D,
prove normal slot reclamation without quarantine, and successfully retry the
same workspace while retaining the last-good generation.

The final rebuild first exposed a missing `USDGEN_ENABLE_CUDA` definition on
the stages test target, which hid the hook declaration; the target now declares
its CUDA dependency explicitly. The corrected full build passed, followed by
focused **4/4**, T0/T1 **101/101** (52.28 seconds), and **30 consecutive runs
each** of parameters, stages, queue and session task graph (120 runs, 53.21
seconds). Unlike the earlier repeats, these include allocation-rejection retry.
Logs: `/tmp/usdgen-async-width-integration-build-final.log` (initial failure),
`/tmp/usdgen-async-width-integration-build-final-2.log`,
`/tmp/usdgen-async-width-integration-focused-final.log`,
`/tmp/usdgen-async-width-integration-t0t1-final.log`, and
`/tmp/usdgen-async-width-integration-repeat-final.log`.

On those final Width binaries, CUDA memcheck reported **zero errors** for
stages, queue and parameters; initcheck reported **zero errors** for stages
and parameters. Selected stock-SDK T2 tests passed **6/6** (9.56 seconds).
Logs: `/tmp/usdgen-async-width-final-memcheck-{stages,queue,parameters}.log`,
`/tmp/usdgen-async-width-final-initcheck-{stages,parameters}.log`, and
`/tmp/usdgen-async-width-final-t2.log`. These tests do not prove arbitrary
host-OOM recovery, leak-free TBB teardown, or GPU-resident renderer handoff.
At the subsequent coherent Length integration source freeze, the CPU-off
execution-pipeline, async-session and scene-owner targets rebuilt and passed
**3/3** (0.14 seconds). Logs:
`/tmp/usdgen-async-length-cpuoff-build-1.log` and
`/tmp/usdgen-async-length-cpuoff-test-1.log`. CUDA-only primitive changes do
not add a CUDA dependency to those backend-neutral paths.

The inherited relay-post allocation failure remains fail-fast on failed TBB
dispatch, not recovery under arbitrary host OOM. Pageable uploads, ordinary
buffer destruction/allocation, remaining operators/finalization and resource
retirement can still block. This checkpoint does not close phase 6 or the
aggregate memory, compiler, cache, renderer, backend-adapter or performance gates.

### Phase-6 checkpoint: asynchronous Length and exact-size compaction

Implementation has **limited validation**, not full phase-6 acceptance. The
bounded operator relay now uses five phases for Length: expression contexts,
expression programs,
Length output/status, compaction counts/status, and compaction scatter/status.
Each continuation requires native success and launcher-return proof; native
callbacks only signal permanent relay identities. Length conditionally publishes
candidate points and keep flags only when device validation succeeds. Compaction
returns only compact scalar status/counts, then allocates exact survivor storage
on the worker before scatter. Geometry and auxiliary channels remain on device.
No input-capacity output allocation, production polling, application mutex, or
CUDA operation from a native callback substitutes for these transitions.

The count phase must allocate CUB scratch before submission, safely handle
malformed offsets without out-of-bounds scan inputs, and prevent any scatter
until validated count proof. The scatter phase must distinguish proven
pre-submit allocation failure from partial submission. Final publication retains
prior geometry owners, and no operator advancement occurs before scatter proof.
Required tests include empty/all-culled geometry, stable survivor/channel parity,
invalid input, capture/mixed API rejection, missing proof, allocation retry,
held-phase scheduling progress, supersession, and last-good visibility.

The frozen low-level implementation provides `CudaLength::ApplyFresh`,
`FinishFreshAsync`, `CommitFreshFinish` and `HasUnprovenWork`, plus compaction's
`ApplyFreshCounts`, `FinishFreshCountsAsync`, `CommitFreshCounts`,
`ApplyFreshScatter`, `FinishFreshScatterAsync`, `CommitFreshFinish` and
`HasUnprovenWork`. Compaction uses a fresh replacement object; it does not
silently replace an already published generation through its fresh API.
The scatter-allocation failure hook rejects before allocation/submission,
leaving proven counts available for retry. Native proof loss quarantines all
owned storage and keeps pinned Scratch permits charged.

Review corrected pin-permit abandonment on failed destructor device selection,
removed storage-clear work from host-only final commit, and made duplicate
scatter-finish rejection non-mutating. An early-count-commit assertion was
moved before status enqueue so it cannot race DMA or assume callback timing.
The first primitive build exposed a missing `noexcept` on the test callback;
the corrected build passed and focused tests passed **2/2** (0.63 seconds).
Fresh survivor tests compare actual points, rest points, widths, stable IDs,
hairT and root bindings. Both all-culled and genuinely empty input are covered.
CUDA memcheck and initcheck each reported **zero errors** for both primitives.
Logs: `/tmp/usdgen-async-length-compaction-build-{1,2}.log`,
`/tmp/usdgen-async-length-compaction-focused-1.log`,
`/tmp/usdgen-async-{length,compaction}-memcheck-1.log`, and
`/tmp/usdgen-async-{length,compaction}-initcheck-1.log`.

The production implementation adds five permanent relay callback identities,
shares expression phases with Width, proves preceding mask DMA after a rejected
Length launch, and records compaction use before final ownership publication.
New stage/queue/session regressions cover held Length/count/scatter phases,
capacity rejection, runtime-expression semantic rejection, scatter allocation
retry, six install/native faults, single-worker CPU progress and supersession.
Partial-cull retained channels are compared with a synchronous same-plan oracle.
There is not yet a dedicated forced terminal-before-launcher-return test for
Length/count/scatter; the shared relay ordering primitive has Width coverage.
Width and Length intentionally share phase-2 test-hook storage, so those
one-shot controls must not be armed concurrently for different operator types.

The first full integration build passed (113 targets), but focused tests were
**5/6**: the scatter allocation regression called the GPU archive's test hook
from the executable, while production used the separate hidden copy in
`libusdGen.so`. Symbol inspection confirmed distinct hook/storage instances;
the requested failure was not reaching production. An execution-API forwarding
hook now invokes the injector inside the plugin, with a CPU-off no-op. No symbol
visibility or archive-linking boundary was widened. The corrected full build
passed (110 targets), and focused tests passed **6/6** (2.55 seconds), including
the real plugin-side rejection and same-workspace retry. Logs:
`/tmp/usdgen-async-length-integration-build-{1,2}.log` and
`/tmp/usdgen-async-length-integration-focused-{1,2}.log`.

The corrected integration then passed T0/T1 **101/101** (25.02 seconds) and
**30 consecutive runs each** of Length, compaction, stages, queue and session
task graph (150 runs, 63.38 seconds). Logs:
`/tmp/usdgen-async-length-integration-t0t1.log` and
`/tmp/usdgen-async-length-integration-repeat-30.log`.

CUDA memcheck reported **zero errors** for integrated stages, queue and session
task graph; initcheck reported **zero errors** for integrated stages. Selected
stock-SDK T2 tests passed **6/6** (9.67 seconds). The final CPU-off rebuild,
including the DSO-safe hook's no-op branch, passed the three execution-pipeline,
async-session and scene-owner tests **3/3** (0.14 seconds). Logs:
`/tmp/usdgen-async-length-integration-memcheck-{stages,queue,session}.log`,
`/tmp/usdgen-async-length-integration-initcheck-stages.log`,
`/tmp/usdgen-async-length-integration-t2.log`, and
`/tmp/usdgen-async-length-cpuoff-{build,test}-final.log`.
Whitespace checks passed and the external OpenUSD worktree remained clean.
These are targeted correctness checks, not a leak-free or performance/soak claim.

Remaining phase-6 slices are explicit, not implied complete by these results:

- **RBF/deformation:** expression-driven sample budget still uses scalar
  D2H plus a stream fence; surface update, binding/factorization/solve and
  deformation need fresh proof-gated APIs and retained candidate ownership.
  The rest-bound RBF math remains in use; this slice changes execution timing,
  not the deformation model or expression-driven controls.
- **Source controls:** expression-driven `useRest` and `resampleTo` require a
  batched pinned scalar-control relay before source upload/resampling can start.
  Invalid proven values must fail without poisoning; no upload may consume
  uncommitted control values.
- **Finalization:** tile validation, bounds validation and per-tile span/bounds
  readback still use stream fences. Replace them with bounded asynchronous
  stages retaining geometry and metadata buffers through publication proof.
  This is tile metadata, not a license to read back hair geometry.
- **Allocation/retirement and lifecycle:** ordinary allocations, pageable
  uploads and teardown still have blocking behavior, and relay posting under
  arbitrary host OOM remains fail-fast. Existing sanitizer leak findings remain
  open. Timelines, aggregate memory accounting, semantic DAG/cache coverage and
  performance/soak evidence are still required to close the numbered gates.

Preferred-provider design input was obtained through two concurrent Spark Qwen
requests and two concurrent Hivemind requests, with no OpenAI inference
substitution. Spark artifacts are in
`/tmp/usdgen-length-async-review-20260912-165731`; response IDs are
`chatcmpl-9560a128a7335c2c` and `chatcmpl-996d964d8a50c6f1` (HTTP 200).
Hivemind artifacts are `/tmp/usdgen-length-async-hivemind-a.json` and
`/tmp/usdgen-length-async-hivemind-b.json`, model `qwen3.8-27b@q4_k_m`,
1800 output tokens each. The Hivemind prompts were brief conceptual prompts,
not source audits; provider output is advisory, not verification. Root and
implementers rejected worst-case output allocation, readiness polling,
callback-side CUDA allocation, timeout-as-proof, and the false claim that exact
allocation requires a blocking host wait. CUDA Graph capture remains a later
phase; these fresh APIs must reject active capture before allocating/submitting.

### Next phase-6 slice: asynchronous finalization and topology comparison

Implementation is in progress and **not validated**. This slice removes the
common terminal waits used by every completed groom, including the hidden
stream fence inside `CompareCurveTopology`; wrapping that synchronous function
in a worker would not satisfy the intended execution contract.

The finalizer uses a bounded three-phase native-callback relay: tile status,
bounds status, then pinned per-tile metadata plus a fresh GPU topology comparison
result. Bounds outputs are not copied until their validation status is proven,
because invalid bounds deliberately leave those outputs untouched. Hair points,
offsets and stable IDs remain device-resident; only status and tile metadata
cross to the host. Each phase requires native success and launcher-return proof.

The candidate retains job/source/operator storage, charged pinned metadata and
status buffers, the previous generation and its CUDA consumer lease. Missing
proof retains that entire candidate and poisons the workspace; a proven semantic
failure or pre-submit allocation rejection preserves last-good publication and
reclaims admission. Finalization admission/in-flight guards must reject reentry
without double completion. Cancellation suppresses public publication, not the
device work needed to establish safe retirement.

`CudaCurveTopologyCompare` adds `BeginFresh`, `EnqueueFreshStatus`,
`CommitFreshFinish(bool*)` and `HasUnprovenWork` while retaining the legacy
synchronous comparison. Its status joins the finalizer's last native callback.
Valid unequal topology is a successful comparison, not an execution error.
Topology-version reuse still requires exact GPU offset/ID equality, identical
tile layout, and matching C3 type/basis/wrap. Generation factories remain on an
ordinary worker: they perform CUDA queries, retirement admission and allocations
and must not be described as callback-safe or wholly host-only.

Required validation includes direct async/sync tile/bounds/version parity,
retained previous leases, zero/all-culled inputs, malformed topology, all three
held phases under a single-worker runtime, metadata terminal-before-launcher
return, capacity/reentry rejection, allocation/fault recovery, cancellation and
supersession without early public publication. Existing private cooker state
and outer current-ticket publication acceptance must be audited together before
changing that boundary. RBF and source-expression controls remain separate open
phase-6 work.

### Finalization integration review and first runtime failures

2026-09-12. The three-phase finalizer and fresh topology comparator are now
implemented and used by both queue and Session task callers, but this checkpoint
is **not validated**. Root review corrected partial-metadata-copy proof loss
(including no-previous generations), device-selection/poison quarantine,
pre-submit allocation reclamation, cleanup-before-user-completion, nonmutating
reentry, and accepted-launch exception ownership. Proven topology cleanup no
longer adds an explicit event synchronize; unproven work remains quarantined.

The full CUDA build passed in
`/tmp/usdgen-async-final-integration-build-3.log`, after namespace and test-local
name-collision compile fixes (build logs 1 and 2). Build 4 also passed. Initial
focused runs are archived as `/tmp/usdgen-async-final-focused-{1,2}.log`; they
**failed** and must not be represented as integration validation:

- Fresh unequal-topology tests accidentally retained `b=a` from earlier
  malformed-view tests while mutating B's buffers. Restoring independent views
  fixed that fixture; the topology test passed in focused run 2.
- The direct Length fixture used thresholds 1 and 2, culling both scaled curves.
  Thus some earlier async/sync Length integration comparisons were vacuous for
  nonempty geometry (the separate cull and primitive tests still have their own
  evidence). The non-cull fixture now uses small thresholds and explicitly
  requires two curves/four points. This correction supersedes any inference
  that those old comparisons alone proved nonempty Length integration.
- The strengthened direct test exposed a reader lifetime error in its failure
  helper: a temporary lease was destroyed after its stream. The gdb stack in
  `/tmp/usdgen-async-final-stage-crash.log` locates `Consumer::Complete` recording
  into that destroyed stream. The helper now scopes the lease before stream
  destruction and actually reads retained nonempty geometry. Rerun is required.
- Queue and Session simultaneous cap-one tests still stalled with the first
  finalizer's test-only metadata launcher-return gate held. Diagnostics in
  `/tmp/usdgen-async-final-callers-diagnostic.log` show B did not reach terminal
  completion, rather than merely returning an unexpected outcome. The launched
  debugger stack `/tmp/usdgen-async-final-queue-stacks-3.log` shows the spinning
  finalizer test worker and sleeping other workers; why this blocks independent
  progress across the configured arenas is not yet proven. A nonblocking
  ready/released test handshake is being implemented; do not infer a closed
  production overlap or fairness gate from that test-seam change.
- Tile-lease fixture upload once reported CUDA OOM, then passed an isolated
  recheck and focused run 2. The first failure's physical cause is unclassified;
  do not call it fixed by a successful retry.

Preferred-provider finalization advice is retained in
`/tmp/usdgen-topology-finalize-source-review-20260912-172457` (Spark source
excerpts, HTTP 200, IDs `chatcmpl-b61cd0d7aefc881b` and
`chatcmpl-948a3fd5abe75c76`) and
`/tmp/usdgen-finalize-hivemind-retry-{a,b}.json` (Hivemind compressed/conceptual
scope, 302/288 input tokens, not full source audits). Initial conceptual prompts
are not source audits. Suggestions involving polling, callback-side CUDA,
invented APIs, and unsafe lifetime shortcuts were rejected.

Read-only preparation for the next source-control/RBF work also used two
concurrent requests per preferred provider, without OpenAI substitution. Spark
source-excerpt artifacts are in
`/tmp/usdgen-rbf-phase6-review-20260912-173649`, HTTP 200 IDs
`chatcmpl-a64638a8c4c0aa42` and `chatcmpl-be1c9e1032260938` (5191/4752 input
tokens). Hivemind artifacts are `/tmp/usdgen-rbf-hivemind-{a,b}.response.json`,
HTTP 200, model `qwen3.8-27b@q4_k_m`, 300/307 input tokens: source-informed
conceptual summaries, not verbatim-source reviews. In particular, the claimed
existing RBF cache use-after-free omitted current synchronous completion and
was rejected as unsupported.

Next implementation boundaries remain the runtime expression scalar controls
before source upload; fresh surface bind/update; RBF extent, rank, LU, input
validation and solve-status stages; and fresh deformation/publication. They
must retain old/candidate cache and input ownership through native proof, with
no callback-side CUDA and no polling. No RBF/source-control implementation is
claimed by this reconnaissance. All numbered gates remain OPEN.

### Async finalization: corrected integration and limited validation

2026-09-12. This supersedes the failed runtime checkpoint immediately above;
it does **not** close phase 6 or any other numbered gate.

The strengthened direct stage test now proves a nonempty two-curve/four-point
Length baseline before checking asynchronous finalization against the legacy
reference. Width changes produce genuinely different values while reusing
topology version; culling changes topology version. All-cull and a separate
canonical zero-source generation are covered. Tests retain and read previous
geometry through terminal holds and unsafe failures, releasing each test lease
before destroying its consumer stream.

The metadata launcher-return test seam now retains the phase-return signal
without parking a worker. A single Ready/Released/Returned atomic mask joins
early release and ready publication without a cross-atomic lost wakeup; it
claims its one-shot return before touching the permanent slot. Native callback
proof remains independently required. Unarmed production execution still
returns directly. This removes the test-induced stall and allows the cap-one
tests to reach B's finalizer rejection; it is not a general timeline proof of
cross-arena/GPU fairness. Source/operator test-only return gates were not
rewritten in this slice.

Caller regressions also now respect two existing contracts: cap-one admission
is restored before independent permanent-quarantine fault tests, and a failed
current Session cook publishes a diagnostic snapshot retaining its last-good
generation and dirty state. The tests check retained generation/commit count,
errors and retryability, rather than incorrectly requiring snapshot-pointer
identity. No Session acceptance API or OpenUSD change was needed.

Recorded validation:

- Full CUDA builds 7 and 8 passed; focused run 4 passed all six topology,
  stage, queue, Session task-graph and tile tests (2.76 s):
  `/tmp/usdgen-async-final-integration-build-{7,8}.log` and
  `/tmp/usdgen-async-final-focused-4.log`.
- T0/T1: **101/101**, 14.20 s,
  `/tmp/usdgen-async-final-t0t1.log`.
- **210 successful repeated executions**: 30 each of topology, direct stages,
  queue, Session task graph, tile lease, Length and compaction, 90.45 s,
  `/tmp/usdgen-async-final-repeat-30.log`. This includes the corrected nonempty
  stage fixture and repeated tile-lease rechecks, without explaining away the
  earlier isolated OOM.
- CUDA memcheck: zero reported errors for topology, stages, queue and Session;
  initcheck: zero reported errors for topology and stages. Logs:
  `/tmp/usdgen-async-final-topology-{memcheck,initcheck}.log`,
  `/tmp/usdgen-async-final-stages-{memcheck,initcheck}-1.log`,
  `/tmp/usdgen-async-final-{queue,session}-memcheck.log`.
- Selected stock-renderer/interop T2 regressions: **6/6**, 9.59 s,
  `/tmp/usdgen-async-final-t2.log`. These do not establish the still-open
  unpatched GPU-resident renderer handoff.
- CUDA-disabled pipeline, async Session and scene-owner targets rebuilt and
  passed **3/3**, 0.15 s:
  `/tmp/usdgen-async-final-cpuoff-{build,test}.log`. This is fallback compilation
  evidence, not an implemented or validated Metal/Vulkan adapter.
- After the final API-comment clarification (inline accepted setup failure
  versus proven successful completion), full build 9 passed and T0/T1 again
  passed **101/101**, 14.38 s:
  `/tmp/usdgen-async-final-integration-build-9.log` and
  `/tmp/usdgen-async-final-t0t1-final.log`. Root's whitespace check passed and
  the external OpenUSD worktree remained clean.

The asynchronous queue/Session finalizer no longer invokes the legacy tile,
bounds, metadata or topology stream fences. The explicit synchronous API is
retained. RBF/deformation and source expression scalar controls still have
synchronous boundaries; ordinary allocation/free and lifecycle behavior, host
OOM relay-post recovery, aggregate memory/cache/DAG correctness and measured
overlap/performance remain unfinished. Existing CPU sanitizer/leak findings
remain open; CUDA error-summary zero is not a leak-clean claim.

### Next phase-6 slice: runtime source-expression controls

Implementation is in progress and **not validated**. `useRest` and `resampleTo`
currently evaluate on CUDA but synchronously copy their values before source
upload. The asynchronous source relay will retain one admission through fresh
expression contexts, program-status proof, compact scalar readback, source
upload and optional resampling. Literal-only source execution and the explicit
synchronous compatibility API remain supported.

Program-status proof must precede scalar D2H: an invalid expression can leave
its output untouched, so batching speculative value copies with program status
would read uninitialized data. Only after native success, launcher return and
host program-status commit may the next phase copy requested groom scalars.
The relay owns a fresh evaluator per candidate, preserving prior workspace
evaluator fields on rejected expressions/controls without changing the generic
evaluator's publication transaction. The compact readback helper has eight
request slots; this bounds this control-transfer packet, not the number of
operator attributes or expression outputs.

The proven effective controls must undergo the same C3/rest/RBF compatibility
validation as legacy preparation before upload. Connected values override
literal fallbacks; an invalid connected value must not silently use its
fallback. Invalid resample counts, invalid expressions, or effective
`useRest=false` feeding RBF fail with last-good generation and dirty state
retained. Missing completion proof retains all evaluator, packet, job and
producer owners and poisons the workspace. Proven semantic rejection and
clean allocation failure reclaim admission; no callback may run CUDA or free
CUDA-owned data.

Required evidence includes nonempty and canonical empty sources, distinct
rest/posed points, runtime control changes and resample topology, invalid
fallback overridden by a valid connection, semantic failure/retry, each held
phase under a one-worker runtime, nonblocking terminal-before-return tests,
admission/reentry, callback-install/native failure and retained old leases.
All existing finalizer/Length regressions remain required. RBF bind/solve/
deformation and the broader phase gates remain separate unfinished work.

Source-control integration review, 2026-09-12: the scalar helper draft required
two corrections before validation. Missing requested fields must not advance
the compact packet index, and a normal submitted packet must be committable
after the parent's terminal proof; only a latched enqueue failure prevents
that commit. The revised tests include sparse missing-first/missing-middle
requests and reuse. This is source-review evidence, not a passing test result.
Unsafe callback fault cases must also run after capacity-one admission tests:
quarantined slots are permanently occupied, so resetting quarantine or ignoring
those charges to make subsequent tests pass would violate the contract.

The corrected helper and its unit-test translation unit compile in the CUDA
build (`/tmp/usdgen-async-source-controls-helper-compile-2.log`). This targeted
object build deliberately excludes the still-editing execution relay; it is
not a linked or runtime validation of source controls.

The first linked source-control run exposed a **test-contract error**, not a
point-selection defect: the new fixture expected `useRest=true` to replace
loaded positions with the authored rest channel. Overlay 14's CurveSource
groom-control checkpoint and `04-operators.md` explicitly define `useRest` as
a space declaration; the channels remain independent. Root briefly changed
source selection based on that mistaken expectation, then the full suite
correctly rejected it in Tools, Session and Hierarchy tests (98/101 passed in
`/tmp/usdgen-async-source-controls-t0t1-1.log`). That production change was withdrawn.
The correct new oracle checks unchanged loaded points, independent rest data,
`alreadyDeformed`, runtime resampling, and the rest-domain RBF admission rule.
Parity with legacy execution alone cannot establish that semantic contract.

Source-control integration checkpoint: the corrected production relay and
tests build successfully (`/tmp/usdgen-async-source-controls-integration-build-4.log`)
and pass all **101 T0/T1 tests**, 14.23 seconds
(`/tmp/usdgen-async-source-controls-t0t1-2.log`). The stages test now checks
loaded and canonical-rest positions independently, with `alreadyDeformed`
changing only according to the evaluated declaration. Tests also exercise
connected invalid integer controls, expression semantic failure before scalar
readback, clean retry, all three held control phases, same-job reentry,
terminal-before-launcher return, permanent unsafe quarantine and old leases.
Queue and Session tests cover independent CPU progress while each control
phase is held, last-good retention and dirty-state behavior on failure.

The new relay holds one bounded source admission across context/program/scalar
proof, upload and optional resampling. Native callbacks only signal permanent
identities. Scalar launcher-return gating is a test-only nonblocking atomic
join; it does not park a worker. Proven control candidates are destroyed under
the selected device before user completion; unproven candidates remain charged
and poison the workspace. `CudaGroomScalarReadback` retains exact native bytes
for up to eight requested groom scalars, rejects ambiguous fields/requests,
and does not publish a partially copied packet. The synchronous source API is
still a compatibility path. This checkpoint closes no numbered phase gate.

Additional sanitizer coverage found an older **test-seam ordering** problem.
Queue and Session `initcheck` runs timed out in a held resampling CPU-probe
assertion even though memcheck and 210 ordinary repeated executions passed.
The Queue diagnostic recorded an accepted probe that did not run in 10000 ms,
no completion/publication, one occupied source slot and zero quarantined slots
(`/tmp/usdgen-async-source-controls-queue-initcheck-diagnostic.log`). These were
failed tests despite the sanitizer reporting zero memory errors.

The old resample hook inserted a blocking native test callback before
`FinishFreshAsync` queued its status D2H. Moving that test hold into the terminal
callback, immediately before signaling, allowed both runs to advance to the
same problem in the older Width hooks. The five operator phase hooks now use
the same post-status, pre-signal placement. Their raw permanent-slot gate
pointers are retained by the relay; no callback performs CUDA calls, frees
owners or invokes user completion. The existing bounded native test wait is
not used in production. Separate launcher-return seams retain their meanings.
No timeout was raised and no production scheduling workaround was added.
The precise CUDA-instrumentation/TBB interaction was not established, so this
is not evidence of a production scheduler repair or a general fairness bound.

With those test-only changes, the complete Queue, SessionTaskGraph and Stages
executables pass both CUDA memcheck and initcheck with **zero reported errors**
(`...-{queue,session,stages}-{memcheck,initcheck}-postoperator.log`, prefix
`/tmp/usdgen-async-source-controls`). Earlier failed `*-initcheck-final.log`
and `*-initcheck-postgate.log` records remain superseded, not counted as passes.
The scalar helper's independent memcheck/initcheck also passed with zero
reported errors. Intentional unproven-work quarantine is not a leak-clean
claim, and the previously recorded CPU ASan/LSan acceptance gap remains open.

Final source-control regression ledger (before RBF edits): the final linked
CUDA build is `...-integration-build-7.log`. Its full T0/T1 run passed
**101/101**, 14.06 seconds (`...-t0t1-final.log`); seven focused executables
each passed 30 repetitions (**210 executions**, 84.36 seconds,
`...-repeat-30-final.log`). The CUDA-disabled rebuild and three CPU-side
execution/session/scene-owner tests passed, 0.13 seconds
(`...-cpuoff-{build,test}-final.log`). The six stock interop/Storm T2 tests
passed, 9.49 seconds (`...-t2-final.log`). All abbreviated paths here use
`/tmp/usdgen-async-source-controls`. The external OpenUSD worktree was also
verified clean. These T2 tests do not establish GPU-resident renderer handoff,
and CUDA-disabled tests do not establish a CPU geometry execution fallback.

### RBF asynchronous conversion: reviewed phase boundaries

The RBF conversion is now being implemented in separate low-level RBF and
surface-binding slices, with an independent execution/cache integration
review. No new RBF asynchronous runtime validation is claimed yet. A
preceding literal-source review
used two concurrent Spark Qwen requests and two concurrent Hivemind requests,
all HTTP 200, with artifacts in `/tmp/usdgen-rbf-review-20260912`. Spark used
`qwen3.8-flash-next` (response IDs `chatcmpl-87636fcaa98f8e7f` and
`chatcmpl-a1958af4a228a84e`); Hivemind used `qwen3.8-27b@q4_k_m` through its
native API. Each response reached its 1600-token output limit, so these are
bounded advisory reviews, not complete audits or test evidence. Root rejected
low-level callback ownership, event-query polling, and retaining borrowed views
without their actual storage owners.

The current code requires these proof boundaries, not merely replacing its
final `Finish` call:

- Surface rest binding: validate topology and finite positions, select/gather
  deterministic samples, then prove compact status and actual sample count.
  RBF matrix allocation depends on the proven actual count.
- RBF rest factorization: extent/status proof precedes host normalization;
  affine-rank proof precedes LU; LU status proof precedes publication of the
  new rest binding. Identity coefficient initialization must also be covered
  by the final binding proof, not enqueued after its completion is announced.
  Retain extent, matrix, solver workspace, pivots and pinned status owners.
- Posed surface update: prove current sample/root-target validation before
  using their values. Preserve the previous published sample/root buffers on
  semantic rejection.
- RBF solve: prove RHS/input validation **before** invoking cuSOLVER, then
  prove solver status before publishing candidate coefficients. A single
  callback after both stages would lose the existing semantic guard.
- Curve deformation: retain the rest field, candidate posed state, expression
  outputs, masks, root targets, geometry and private output through validation
  and final output-write proof. No curve geometry readback is introduced.

The expression-driven `rbfSamples` control uses the proven compact scalar
packet after program success. Same-rest reuse must retain the factorization
while staging current samples/coefficients; changed-rest binding remains a
private candidate until its entire prerequisite chain succeeds. Neither a
failed pose nor a rejected candidate may seed later cache or dirty state.
All input views retain their actual owners across phase transitions. Fresh
commits are host-only; selected-device cleanup is an ordinary worker action
before user completion. Clean rejection and proven semantic failure reclaim;
missing proof poisons and permanently charges the whole retained candidate.
The synchronous compatibility APIs remain explicitly separate.

The integration review identified two additional cache-transaction seams in
the current synchronous executor. `ExecutionState::Prepare` replaces the RBF
cache immediately when the rest key changes, and `UpdateRbf` installs a newly
bound cache before posed validation, solve, or deformation has succeeded.
Neither behavior is an acceptable publication boundary for the new async
path. It must keep the old accepted cache through a rejected rest change and
stage both a changed-rest binding and a same-rest pose privately. Same-rest
reuse should share immutable factorization storage with an independent pose
candidate, not duplicate the dense matrix or share a concurrently mutable
cuSOLVER stream handle. The source/control job retains its input owners until
all corresponding completion proofs, and the accepted generation retains its
own immutable geometry independently of cache candidates.

The required integration tests include failure after successful factorization,
after successful solve, and in a downstream operator/finalizer; none may make
candidate pose/cache state accepted or clear dirty state. A valid retry must
match a cold cook. Rest-key/budget edits must change binding identity only at
the accepted transaction boundary; same-rest pose changes preserve rest
identity. Existing RBF session identity/lease tests are reusable but do not
alone prove these rejection boundaries. These are implementation requirements,
not evidence that the current executor already meets them.

### RBF primitive implementation checkpoint (limited, not integrated)

Surface binding now has draft fresh Bind/Update producer and compact-proof
APIs with caller-owned native completion proof, host-only commits, prior
buffer parking, clean allocation rejection/retry and unsafe-submission
quarantine. Root's review removed CUDA frees from commit paths, corrected
pending-proof tracking and legacy/fresh destructor-mode handling, and required
explicit fault tests. The first frozen surface objects and test object compile
(`...-surface-objects-2.log`). A standalone executable linking those new
objects ahead of the previously validated GPU archive passes the surface test,
CUDA memcheck and initcheck with zero reported errors
(`...-surface-standalone-{run,memcheck,initcheck}-1.log`). This is deliberately
not a full library rebuild while the RBF/deformer dependencies are being edited.
Further fresh deduplication, capture, and full-buffer retention cases remain
under review. Quarantine fault coverage is not a leak-clean claim.

The first broad RBF draft failed root review: it changed numerical precision,
legacy multiple-evaluation behavior and lifetime semantics. That rewrite was
withdrawn; the original numerical kernels and legacy implementation were
restored before an additive candidate-only Bind slice was introduced. Its
extent, affine-rank and LU/identity-initialization phases now have separate
external-proof commits. The targeted objects compile
(`...-bind-objects-1.log`); a standalone executable passes the existing legacy
RBF test plus the new happy-path Bind phase sequence, and CUDA memcheck reports
zero errors (`...-bind-standalone-{run,memcheck}-1.log`). These abbreviated paths
use `/tmp/usdgen-async-rbf`. This first bind-only checkpoint does not yet prove
transactional accepted-factorization reuse, pose solve, fresh evaluation or
deformation. The next slice must distinguish accepted and rejected candidates
and retain the accepted factorization through a rejected rebind.

A second actual provider-review batch used two concurrent Spark Qwen requests
and two concurrent Hivemind requests on literal draft surface/deformer files.
All four returned HTTP 200; logs are
`/tmp/usdgen-rbf-fresh-review-1789264674270-{spark,hive}-{surface,deform}.log`.
Spark response IDs were `chatcmpl-9901ed2e61d9a059` and
`chatcmpl-909e9ff48fa5ccce`; Hivemind identified
`qwen3.8-27b@q4_k_m`. Each response reached 1600 output tokens. These are advisory
reviews of changing drafts, not frozen-source verification. Root rejected
suggestions to free pinned proof storage from host-only commits and claims
that `begin >= end` permits empty curves; the validation explicitly rejects
them. The provider requests have finished, and no OpenAI inference endpoint
was substituted. All numbered implementation gates remain open.

The subsequent accepted-factorization/evaluation slice supersedes the
bind-only limitation above: a rejected fresh rebind retains the prior accepted
factorization; fresh identity evaluation has its own pinned status packet and
does not invalidate the factorization after a nonfinite CV rejection. Unsafe
evaluation quarantines the factorization it may still read as well as its
packet. Preflight capture/device/pointer validation precedes construction or
retirement of CUDA-owning candidates. Test hooks distinguish allocation
rejection from failure after accepted submission; unsafe objects cannot be
reused. Root corrected an initially invalid test ordering that attempted to
reuse a deliberately quarantined binding. No quarantine reset was introduced.

The fresh deformer stages are shape/profile validation, RBF evaluation,
deformation application, and final private-output copy, each with its own
parent-proven completion boundary. Shape and RBF rejection stop before reading
unwritten warped data; an invalid expression field stops before copying staged
output. Root corrected a provenance check accidentally placed only in the
legacy path, and legacy/fresh modes are now explicitly exclusive for this
primitive. Tests cover identity/root locking, wrong-binding commit rejection,
no early destination change, malformed-offset and nonfinite-field rejection,
full same-object retry, host-pointer preflight, capture and empty geometry.
Surface tests additionally cover full prior sample/root retention, effective
deduplicated counts, first-bind nonfinite rejection/retry, and partial compact
status-copy failure. These low-level APIs borrow inputs: their future parent
relay must retain the actual storage owners, not just views.

Root's full CUDA build passed (`/tmp/usdgen-async-rbf-integration-build-2.log`).
All **101 T0/T1 tests passed**, 25.32 seconds, serial test dispatch
(`/tmp/usdgen-async-rbf-t0t1-1.log`). The complete Rbf, SurfaceBinding and Deform
test executables each passed CUDA memcheck and initcheck with **zero reported
errors** (`/tmp/usdgen-async-rbf-{Rbf,SurfaceBinding,Deform}-{memcheck,initcheck}-final.log`).
These tests deliberately supply external synchronization as their native-proof
stand-in; they do not prove production callback ordering, stream overlap,
interactive latency, or asynchronous RBF execution integration. The CPU
ASan/LSan, solver headroom, ordinary CUDA allocation/free, aggregate
memory/cache, and stock GPU renderer-handoff gates remain open.

### Fresh posed RBF primitive checkpoint

The low-level CUDA binding now stages a posed solve in two separate private
candidate phases. `BeginFreshSolve` copies posed drivers and computes/returns
the RHS finite-input flag into a pinned packet; `CommitFreshSolveInput` is a
host-only acceptance boundary and rejects a non-finite RHS before cuSOLVER is
called. `BeginFreshSolveFactors` runs `getrs` against the accepted immutable
rest LU while writing only a candidate coefficient allocation, and
`CommitFreshSolve` exchanges that allocation into the accepted field only
after the externally proved solver-info packet is clean. Old coefficients are
parked for ordinary later worker retirement, never freed by a commit. Thus an
input or solver rejection preserves the prior accepted pose and a later fresh
evaluation remains valid. The binding serializes its own fresh factor-handle
use; it is not a cross-workspace sharing mechanism. Root's integration review
also tied each pose candidate to the exact accepted factor owner captured by
`BeginFreshSolve`. An input-proven candidate now prevents a rest rebind until
its solve transaction ends, and the solver phase verifies that identity before
dispatch. This closes an illegal interleaving where a candidate sized and
normalized for one rest binding could otherwise consume a newly accepted
factorization.

`testUsdGenCudaRbf` covers identity-to-pose acceptance, duplicate/early
commit rejection, a non-finite pose rejection that preserves the prior pose,
and the pre/post-submission fault model. The forced full CUDA rebuild passed
(`/tmp/usdgen-rbf-posed-full-build.log`). All **108 runnable tests passed** in
35.71 seconds with the conditional Storm-surgery test skipped
(`/tmp/usdgen-rbf-posed-full-ctest.log`), and the RBF executable passed
**30/30** consecutive runs in 12.74 seconds
(`/tmp/usdgen-rbf-posed-repeat-30.log`). After an initial sanitizer startup
was rejected by temporary device-memory pressure from concurrent local-model
review, the unchanged executable passed both CUDA memcheck and initcheck with
zero reported errors (`/tmp/usdgen-rbf-posed-{memcheck,initcheck}-retry.log`).
This is low-level proof
coverage only: the execution relay/cache still uses the legacy synchronous
RBF path. Its next work must retain source/surface owners through the staged
callbacks, keep rest-cache and same-rest pose candidates private until the
downstream deformation/finalizer transaction accepts, and avoid sharing a
mutable cuSOLVER handle across workspaces.

The subsequent ten-repeat regression batch is **not a pass**. Six focused
executables passed all ten repetitions, but ExecutionStages failed its third
iteration at the source-control callback-gate completion assertion (original
line 435), 10.46 seconds for the failed invocation
(`/tmp/usdgen-async-rbf-repeat-10.log`). This is an existing source-relay test
path, not a new fresh RBF caller. Root is adding phase/timing diagnostics and
reviewing predecessor test-callback installation; the exact failure cause is
not established yet. The single full-suite pass and six clean CUDA sanitizer
runs above remain valid observations but do not erase this repeat failure.

The failure-only diagnostic now records gate name/index, Execute admission and
elapsed time, gate-wait time, completion count/outcome, and relay occupancy and
quarantine. A diagnostic-only rerun passed **60/60**, 41.17 seconds
(`/tmp/usdgen-async-rbf-stage-diagnostic-repeat.log`), so the original timeout
was not causally reproduced. Static review nevertheless found two ordering
issues worth correcting: the three source-control test gates were predecessor
callbacks installed before a separate terminal callback, and the initial
context launcher restored its selected device only after signaling launcher
return. Controls now retain per-phase gates and hold them inside the terminal
callback immediately before its final slot signal, matching the existing
resample/operator test protocol. Initial context success and catch paths
restore the selected device before the launcher-return join. There is no new
production wait, mutex, polling loop, callback CUDA operation, or raised test
timeout. The full rebuild passed
(`/tmp/usdgen-async-rbf-integration-build-3.log`). These are ordering/lifetime
corrections, not a proven explanation of the earlier intermittent failure.

Post-correction validation supersedes that unresolved repeat result as a
regression observation, but not as a causal diagnosis. Seven focused CUDA
executables passed **30/30 repetitions each (210 executions total)** in
101.58 seconds (`/tmp/usdgen-async-rbf-repeat-30-postgate.log`). Queue,
SessionTaskGraph and ExecutionStages each passed both CUDA memcheck and
initcheck with zero reported errors
(`/tmp/usdgen-async-rbf-{ExecutionQueue,SessionTaskGraph,ExecutionStages}-{memcheck,initcheck}-postgate.log`).
The final full T0/T1 run passed **101/101**, 25.35 seconds
(`/tmp/usdgen-async-rbf-t0t1-final.log`), and the six selected CUDA-interop and
stock-Storm T2 checks passed **6/6**, 9.61 seconds
(`/tmp/usdgen-async-rbf-t2-final.log`). This stronger repeat evidence shows
the corrected test protocol is stable under the recorded load; it still does
not establish the historical failure's precise cause, asynchronous posed-RBF
execution, renderer device-generation handoff, or completion of phase 6.

### Production async RBF relay and publication-transaction checkpoint

The production CUDA operator relay now admits `UsdGenDeform` and carries one
fixed-slot candidate through evaluated groom controls, optional rest-surface
bind, posed surface update, RBF extent/rank/LU phases, posed input/solve,
deformer shape/evaluate/apply, and final private point copy. Each native phase
has a distinct callback identity and external terminal proof. The relay keeps
all uploaded surface and mask owners, compact scalar-readback storage,
factorization/solve state, deformer scratch, and destination points alive until
the corresponding proof. Empty geometry consumes the operator without
creating or changing an RBF binding. Clean allocation rejection before the
first mask copy is reclaimable; submitted or unproved work retains the relay
and poisons the workspace under the existing fail-closed contract.

RBF operator success is no longer cache publication. Fresh surface updates and
fresh coefficients park their prior accepted buffers, and an
`ExecutionState` transaction owns that provisional state through terminal
tile, bounds, topology and generation construction. Both asynchronous and
compatibility synchronous finalizers accept the transaction only after a
generation exists; clean finalizer rejection rolls it back. Dropping a proved
but unfinalized job also rolls it back under the borrowed workspace's device.
A changed-rest candidate keeps the old cache/key/identity/counters installed
until acceptance. Accepted rest changes retain the historical counter scope by
starting the new identity at bind/solve `1/1`; sample-budget rebinds on the same
rest continue the current binding counters. Binding keys now include rest
normal data and domain as well as rest points/topology.

Execution-stage coverage now proves async RBF parity with the synchronous
graph, same-rest finalizer rejection, changed-rest rejection, abandoned-job
rollback, retry acceptance, counter/key/identity stability, changed-rest
acceptance, empty geometry, and async-Deform-to-synchronous-finalizer handoff.
Low-level RBF and surface tests separately prove pending-decision guards,
accept/rollback behavior, restoration of the prior identity field and surface
targets, and durable retry. The design audit and implementation/test passes
were split across the requested Luna and Terra subagents; root reviewed the
publication boundary, added the finalizer transaction and abandonment cases,
and corrected cache-counter compatibility exposed by the complete suite.

The forced CUDA build completed. The final full run completed **109 tests** in
36.03 seconds: 108 passed and the environment-conditional Storm-surgery test
was skipped.
ExecutionStages, RbfSession, SessionTaskGraph, Rbf and SurfaceBinding each
passed **20/20 repetitions** (100 executions). ExecutionStages, Rbf,
SurfaceBinding and SessionTaskGraph each passed CUDA memcheck and initcheck
with **zero reported errors**; logs are
`/tmp/usdgen-rbf-relay-<executable>-{memcheck,initcheck}.log`. A CUDA-disabled
build of the touched library plus ExecutionTaskGraph and Sessions succeeded,
and both CPU tests passed. `git diff --check` is clean.

This checkpoint closes production asynchronous posed-RBF execution and its
final-publication rollback boundary. It does **not** close phase 6 as a whole:
the dependency-ready compiler/capability matrix, broader backend-neutral
Metal/Vulkan lowering, renderer handoff gates, aggregate memory/cache budgets,
and the remaining plan acceptance gates are still open.

### CUDA linear capability/task-plan metadata checkpoint

The next bounded compiler slice adds `executionPlan.h`, a backend-neutral
immutable vocabulary for backend capability rows, compiled operator metadata,
semantic task IDs, authored order, explicit dependencies, barriers,
cancellation points, resource access/producer lineage and task estimates. The
CUDA matrix is versioned and currently contains the four actually implemented
families: CurveSource, Width, Length and RBF Deform. A successful CUDA compile
attaches one exact metadata snapshot marked `LinearAuthoredChain`; unknown byte
and time estimates remain explicitly unavailable rather than being represented
as zero-cost work. This header contains no CUDA synchronization or allocation
type and is an intended seam for later Metal/Vulkan matrices, but those
matrices/adapters are not implemented.

The CUDA queue now validates dense task identities and lowers source,
semantic-operator and terminal work from the compiled metadata's dependencies,
semantic ordinals and terminal task ID. It no longer independently invents the
predecessor IDs or publication identity. The execution state remains one
serial mutable geometry lane, so this is deliberately not branch/fan-in
execution. Resource annotations are descriptive and dependencies remain
explicit; they are not yet automatic hazard-edge generation. Review corrected
Width and Length to declare Widths as read/write, preserving producer lineage
from Source through Width and Length to Publication. Source, topology-changing
Length and Publication barriers are explicit. Cancellation is honestly
declared `BeforeTaskOnly`.

Unknown operator families and locally supported but non-linear compositions
now receive distinct `unsupported operator` and `unsupported composition`
diagnostics; configuration validation retains its existing precise
operator-specific diagnostics. The focused metadata test pins all matrix
rows/ranges/flags, exact Source→Width→Length→Deform→Publication identities and
dependencies, authored/semantic order, barriers, resources/producers,
unavailable estimates, cancellation semantics and the three diagnostic
classes. The metadata and queue tests each passed **20/20 repetitions**. The
post-slice CUDA build passed, followed by **109 passed tests plus one skipped
conditional Storm test** (110 total) in 41.89 seconds. The metadata-driven
queue passed CUDA memcheck and initcheck with zero reported errors
(`/tmp/usdgen-linear-plan-queue-{memcheck,initcheck}.log`). The CUDA-disabled
core/imaging build and focused ExecutionTaskGraph/Sessions tests also passed;
`git diff --check` remains clean.

This advances but does **not** close phase 4. Arbitrary fan-in/fan-out,
reference/map and nested lowering, deterministic merge semantics, automatic
resource-hazard edges, real cost/peak estimates and admission, multi-location
safe points, plan signatures/cache keys, randomized hierarchy semantic parity,
and CPU/Metal/Vulkan capability population remain open.

### Geometry-input contract and logical-value checkpoint

The next bounded slice makes the dataflow boundary explicit without claiming
that the current CUDA mutable workspace can execute branches. Four independent
local design reviews (two Spark Qwen and two native Hivemind Qwen requests)
converged on the same requirement: fan-out needs independently retained value
storage, and fan-in is undefined until the consuming operator declares an
actual merge operation. Those reviews were advisory; the implementation below
was checked against the repository's concrete operator and scheduler APIs.

Every registered operator version now has an exact geometry-input arity.
Scatter and CurveSource declare zero; all current transformers, including Grow
despite its topology ownership, declare one. The registry caches this contract
at startup, and the compiler validates it before moving any retained graph
state. Consequently the CPU scheduler can no longer silently reduce an
unsupported multi-input node to `inputs.front()`. Diagnostics name the node,
type, expected arity and authored arity. A rejected fresh or incremental
compile leaves the prior graph untouched. This is an operator semantic
contract, not a scheduler-imposed claim that a meaningful multi-input Width or
Length operation exists.

The CUDA plan metadata now assigns explicit logical input and output value IDs
to every resource access. Each value records its resource kind, producer,
monotonic per-resource logical version and storage guarantee. Authored inputs
are `ExternalImmutable`, terminal generations are `PublishedImmutable`, and
all intermediate CUDA values are truthfully
`ExclusiveWorkspaceVersion`. The latter classification exposes the current
aliasing constraint: these descriptors do not make the workspace immutable,
and adding fan-out dependency edges would still be incorrect.

A new CPU fan-out test executes Source to two Width siblings with terminal and
sibling lexical/authored order permuted. It proves both siblings run, terminal
results remain identical, the source is unchanged, and all writable width
planes have distinct storage. It also pins exact diagnostics for missing unary
input, unary two-input fan-in, and a source with an input, plus destination
graph preservation after rejection. The fan-out test passed 20 consecutive
runs. The CUDA metadata test covers all logical value links and storage
classes. After correcting two older tests that had relied on invalid
input-less Width fixtures, the full CUDA-enabled suite completed **111 total:
110 passed and one environment-conditional Storm test skipped** in 36.59
seconds. The focused
CUDA-disabled fan-out, contract and expression-binding tests passed **3/3**;
`git diff --check` is clean.

This checkpoint proves executable unary CPU fan-out and prevents accidental
fan-in semantics. It does **not** implement CUDA fan-out, any multi-input merge
operator, reference/map lowering, immutable CUDA branch snapshots, branch
resource lifetime accounting, or completion of phase 4. The next executable
CUDA DAG slice must allocate/retain a distinct output value at a branch point
and lower only operators whose declared input/merge contract matches that
value topology.

### CUDA source-rooted unary Width DAG checkpoint

The next bounded CUDA slice implements real source-rooted unary Width fan-out
without claiming concurrent branch execution. Two local Spark Qwen reviews and
two native Hivemind Qwen reviews independently converged on the same core
requirements: deterministic semantic-node identity, immutable predecessor
views, separately owned per-node outputs, declared-terminal publication and
proof-driven cleanup. The implementation was split between production and test
work and then independently audited against those requirements. Review
artifacts are retained under
`/tmp/usdgen-cuda-fanout-review-20260912/`.

CUDA graph admission now finds exactly one CurveSource regardless of authored
vector position, validates unary Width inputs and source reachability, performs
a deterministic Kahn topological normalization with authored semantic index as
the ready-node tie break, and requires an explicit terminal when a branch has
multiple sinks. Missing predecessors, cycles/disconnected nodes, fan-in and
ambiguous terminals have exact diagnostics. Length and Deform keep the prior
linear lowering contract and branched graphs containing either are rejected
precisely; topology-changing branch snapshots are not implied.

The backend-neutral plan vocabulary now includes `SourceRootedUnaryDag` and
`JobOwnedImmutable`. CUDA task IDs are dense normalized execution identities,
while each task retains its authored semantic node. Width resource inputs point
to the declared predecessor's logical value and output versions advance from
that input lineage, so sibling outputs may share a version number while having
distinct value IDs and owners. A separate prior-task dependency explicitly
serializes the current exclusive evaluator/workspace. Publication depends on
both the declared terminal producer and the last serialized task and consumes
the terminal's Width value.

At execution time, source and every Width output occupy per-semantic-node value
slots for the lifetime of the job. Both synchronous execution and the
production asynchronous queue select each operator's declared predecessor
before evaluation, retain each result in a distinct device buffer, and select
the declared terminal before finalization. Consequently a later-executed
sibling cannot overwrite the value selected for publication. A failing sibling
also leaves the prior published generation pointer-identical and readable.

The focused test covers shuffled authored order, deterministic recompilation,
source fan-out, a nested branch, logical-producer versus physical-serialization
edges, synchronous and queued terminal selection, last-good retention after a
runtime sibling failure, and all structural rejection classes. It passed 20
consecutive repetitions. Independent Compute Sanitizer memcheck and initcheck
runs each reported zero errors. The final CUDA-enabled build passed, followed
by **111 passed tests and one environment-conditional Storm test skipped** (112
total) in 37.05 seconds. The CUDA-disabled `usdGen`, ExecutionTaskGraph and CPU
fan-out targets built, and the two focused tests passed. `git diff --check` is
clean.

This advances but does **not** close phase 4. CUDA Width branches are still
serialized on one stream through the exclusive workspace dependency. True
overlap requires per-branch evaluator/stream ownership and completion-driven
resource retirement. Topology-changing branch snapshots, declared multi-input
merge operators, reference/map lowering, automatic resource-hazard generation,
cost/peak admission, randomized hierarchy parity and Metal/Vulkan capability
population remain open.

### CUDA Width DAG concurrent-dispatch and COW checkpoint (2026-09-12)

The serialized Width-DAG checkpoint is superseded by a bounded concurrent
lowering. Two local Spark `qwen3.8-flash-next` reviews and two native Hivemind
`qwen3.8-27b@q4_k_m` reviews completed with HTTP 200 under
`/tmp/usdgen-cuda-branch-overlap-20260912/`. Their useful consensus was fixed
nonblocking branch streams, task-local evaluator/geometry state, distinct
owned results, cross-stream event waits and deterministic test gates. Host
future waits, callback-blocking timing tests, `cudaStreamAbort` and speculative
cleanup of unproven work were rejected.

The CUDA workspace now pre-creates eight nonblocking branch streams. Ready
Width tasks select a stable stream from normalized operator identity, take a
private geometry snapshot and retain evaluator state in their relay. Siblings
read the same immutable source geometry but each materializes a distinct
job-owned Width allocation. A descendant enqueues the predecessor buffer's
device-event wait onto its own stream; terminal selection enqueues the same
ownership wait before moving only the declared terminal allocation into final
storage. No sibling rebinds or mutates the shared execution geometry.
Publication joins all launched operator tasks for lifetime/failure safety while
its logical Width input remains the declared terminal. Source-only publication
has an explicit Source dependency.

The backend-neutral capability vocabulary now advertises
`UsdGenCapabilityCopyOnWriteWrites`: every resource written by that lowering is
task-private while untouched inputs remain shared immutable views. CUDA Width
capability version 2 carries this flag and no exclusive-workspace flag. Width
DAG source values and branch Width values are described as
`JobOwnedImmutable`; distinct outputs never alias, while multiple readers may
share one immutable input. The compatibility linear Width lowering remains
exclusive because it still uses the primary mutable workspace lane. This
contract contains no CUDA stream/event types and is intended to be reusable by
future Metal and Vulkan capability tables.

The CPU implementation is the corresponding plane-granular COW path:
untouched `VtArray` planes alias upstream storage, and only planes named by
`PlanesTouched()` receive fresh writable allocations. This audit found and
fixed a persistent-node bug: non-owning nodes previously refreshed inherited
topology and per-curve arrays only when their destination was empty, allowing
stale aliases after an upstream topology/stable-ID revision. They now refresh
totals, topology version, offsets and per-curve aliases from the current input
on every preparation. The fan-out regression proves distinct writable Width
planes, shared point/hairT planes, and a second-run topology change with reused
branches. Extra-plane binding remains outside this claim because those ports
are not yet scheduled.

The CUDA fan-out and nested-branch tests use a launcher-side multi-claim
rendezvous after stream assignment. They prove two independent ready tasks are
admitted concurrently on distinct branch streams while a descendant stays
behind its authored predecessor. This is deliberately **not** a claim of
measured device-kernel interval overlap; an event-timestamp/trace performance
gate remains open. Width DAG passed 20 consecutive repetitions. Focused CUDA
tests passed 4/4; Compute Sanitizer memcheck and initcheck each reported zero
errors before the metadata/CPU-only COW clarification. The final CUDA-enabled
suite passed **111 tests with one environment-conditional Storm test skipped**
(112 total) in 79.64 seconds. The CUDA-disabled core and focused DAG/CPU fan-out
targets built and passed 2/2. `git diff --check` is clean.

This still does **not** close phases 4–6. Topology-changing branch revisions,
declared multi-input merge/fan-in, reference/map lowering, extra-plane COW,
automatic hazard generation, cost/peak admission, randomized hierarchy parity,
Metal/Vulkan capability population and an actual GPU overlap trace remain open.
RBF accept/rollback atomicity and rollback-status propagation are also separate
correctness work; they are not implied by the Width-only COW contract.

### RBF resolution transaction hardening checkpoint (2026-09-12)

The asynchronous CUDA RBF publication path now resolves its provisional field
solve and surface update as one checked logical transaction. Two local
`qwen3.8-flash-next` reviews (`chatcmpl-b6b948eb1b2df94f`,
`chatcmpl-b1ae564d4da7c8b4`) and two Hivemind reviews
(`chatcmpl-r29yyzz9izmpc8qpzh829h` on `qwen3.8-27b@q4_0`, and
`chatcmpl-h7nfb8n7fjoks8vd1n8e8` on `qwen3.8-27b@q4_k_s`) were inspected from
`/tmp/usdgen-rbf-atomic-20260912/`. The first four longer requests all timed
out after 180 seconds with zero bytes and one bounded Hivemind retry returned
HTTP 500; those failed calls contributed no findings. The usable responses
agreed that arbitrary sequential irreversible accepts cannot be made strictly
atomic without a common commit primitive or undo. They also identified the
implementable property of these concrete APIs: successful Accept is a
host-only, allocation-free ownership transition whose complete readiness can
be checked before either resource mutates.

`CudaRbfBinding` and `CudaSurfaceBinding` therefore expose non-mutating
accept/rollback readiness predicates. `ExecutionState::ResolvePendingRbf`
preflights both resources before changing either. A normal preflight rejection
performs zero concrete resolution calls. After both predicates succeed, the
exclusive transaction owner invokes both concrete operations. Rollback always
attempts field and surface independently and aggregates the two statuses; it
no longer short-circuits and silently skips the surface. Cache identity,
counters, state pointer and statistics publish only after both accepts succeed.

An impossible concrete failure after successful preflight is not represented
as a recoverable atomic rollback: one irreversible transition may already have
occurred. Sync and async callers now fail closed, poison the workspace and
retain/quarantine the transaction owners rather than freeing uncertain CUDA
storage. Operator-relay failure cleanup routes proved provisional state through
the same resolver, checks rollback status, and guards its `noexcept` fallback
packet allocation. No native callback runs CUDA, no host wait was added, and no
stream-abort or speculative-free mechanism is assumed. This state model is
portable to Metal/Vulkan even though the current resource methods are CUDA.

The new `testUsdGenCudaRbfTransaction` covers eight deterministic cases:
field/surface accept-preflight, rollback-preflight, accept-commit and
rollback-commit faults. It proves zero mutation after either preflight fault,
both rollback attempts after either concrete rollback fault, exactly-once
failed completion, one quarantined relay, poisoned retry rejection, and
pointer-identical last-good generation/statistics/identity. Its success case
proves both accepts occur once and solve statistics advance once. Root also
corrected the test seam routing so integrated faults reach the GPU implementation
inside `libusdGen`, rather than a duplicate static-library instance.

The transaction test passed 20 consecutive repetitions. Compute Sanitizer
memcheck and initcheck each reported zero errors. Five focused RBF/queue/stage
tests passed 5/5. The final CUDA-enabled suite passed **112 tests with one
environment-conditional Storm test skipped** (113 total) in 37.33 seconds. An
earlier full run had two transient Storm process crashes; both isolated reruns
passed and the subsequent complete 113-test run was clean. The CUDA-disabled
core and focused task-graph/CPU-fan-out tests built and passed 2/2.
`git diff --check` is clean.

This checkpoint hardens the production asynchronous graph path; it does not
make the legacy synchronous `UpdateRbf` path transactional, because that path
still mutates the accepted cache before terminal finalization. Converting or
retiring that compatibility path remains required. The broader phase 4–6
items—topology-changing DAG values, fan-in/merge, reference/map lowering,
automatic hazards and admission, extra-plane COW, actual GPU overlap tracing,
and Metal/Vulkan capability populations—also remain open.

### Synchronous CUDA RBF COW transaction checkpoint (2026-09-12)

The synchronous compatibility entry point no longer uses the legacy
`UpdateRbf` helper or mutates accepted binding data during operator execution.
It now drives the same fresh Surface/RBF/Deformer stages as the asynchronous
executor and synchronizes its own nonblocking stream at each native proof
boundary before invoking the corresponding `CommitFresh*` operation. Rest
changes and sample-budget changes build a private cache candidate; same-rest
updates share the accepted immutable factorization while staging private posed
samples and coefficients. In both cases, the cache pointer, key, identity and
counters remain unchanged until terminal generation construction succeeds and
`ResolvePendingRbf(true)` accepts both resources.

Two local Spark `qwen3.8-flash-next` reviews
(`chatcmpl-8ab604befcd9215e`, `chatcmpl-b0b61a94c0c73c6a`) and two usable
Hivemind `qwen3.8-27b@q4_0` reviews
(`chatcmpl-nw2n9w2r0xtrqh2up95sh`,
`chatcmpl-cver80jaex57g981tsuvhs`) were inspected from
`/tmp/usdgen-rbf-sync-transaction-20260912/`. One initial Hivemind lifecycle
request timed out, one portability request returned HTTP 500, and one bounded
retry returned HTTP 400; these failed calls contributed no findings. The
usable reviews agreed on a synchronous driver over the existing fresh stages,
proof-only stream fences, private rebind candidates, deferred cache acceptance
and deterministic finalization/retry coverage.

Clean post-solve finalization failure rolls the staged field and surface back
and preserves the exact last-good generation and binding statistics. An
impossible or unproved submitted-work failure poisons the workspace and retains
its owners for context teardown; it never converts missing proof into an
implicit free. A new executor-routed fault hook avoids duplicate static CUDA
test state. Synchronous acceptance also refreshes the workspace statistics from
the newly accepted cache rather than publishing the provisional snapshot.

`testUsdGenCudaRbfTransaction` now covers synchronous-versus-queued geometry
and accounting parity, same-rest rollback and one-solve retry, changed-rest and
budget-only rejected rebinds followed by valid identity changes, and an unsafe
post-submit failure followed by poisoned retry rejection. It passed 20
consecutive repetitions. Compute Sanitizer memcheck and initcheck each reported
zero errors. Four focused CUDA RBF/stage tests passed 4/4. The final
CUDA-enabled suite passed **112 tests with one environment-conditional Storm
test skipped** (113 total) in 26.30 seconds. The CUDA-disabled core and focused
ExecutionTaskGraph/CPU fan-out targets built and passed 2/2. `git diff --check`
is clean.

This is resource-granular COW, not a whole-geometry clone: CUDA Width branches
and changed RBF candidates own their writes, while unchanged geometry and
accepted factors are immutable shared inputs. CPU untouched `VtArray` planes
likewise alias upstream storage and touched planes materialize privately.
`extraCv`/`extraCurve` ports are still not scheduled and therefore remain
outside the COW claim. Topology-changing DAG revisions, fan-in/merge,
reference/map lowering, automatic hazards and admission, extra-plane COW,
actual GPU overlap tracing and Metal/Vulkan capability population remain open.

### Backend-neutral dependency and physical-hazard lowering checkpoint (2026-09-12)

The plan contract now separates immutable logical value flow from physical
storage hazards. `UsdGenExecutionDependencyCompiler` derives direct semantic
data edges from each resource input's logical value/producer, coalesces edge
reasons into typed provenance bits, and emits standard writer-to-reader,
reader-to-writer and writer-to-writer ordering only for explicitly declared
physical hazards. Hazards are keyed by backend-neutral
`(ExecutionDataKind, identity)`, so two CUDA lanes, Metal heaps or Vulkan
allocations of the same kind do not falsely conflict. No native stream, event,
heap or semaphore type enters the public metadata.

Hazard orientation uses a stable Kahn order over the semantic/value graph;
authored order key and task ID are only the ready-set tie break. This preserves
declared producer direction even when task-vector/authored order is shuffled.
A second cycle check protects future policies. Invalid value producers,
workspace-as-logical-value misuse, invalid hazard access, dense-ID violations,
dependency cycles and contradictory `exclusiveWorkspace` summaries fail plan
construction with exact diagnostics. Duplicate Read+Write declarations for one
identity fold deterministically to ReadWrite.

Publication now declares a direct lifetime join over every launched task.
That join is distinct from its logical terminal input: every branch owner must
finish before task-private allocations can move or retire, while the selected
terminal value remains determined by authored graph semantics rather than
completion order. CUDA plan construction runs lowering and validation, the
queue revalidates immutable metadata at admission, and the CUDA capability
matrix version is 3 so older cached plans cannot cross this contract change.
Linear mutable operators explicitly declare workspace identity zero as
ReadWrite. Width-DAG operators declare no physical workspace hazard: siblings
share immutable geometry/value inputs and privately own their COW Width output,
so automatic lowering does not reserialize them.

The required parallel review used two Spark `qwen3.8-flash-next` responses
(`chatcmpl-b5e7204a4c2a5355`, `chatcmpl-b92ff1a900c6e4c3`) and two Hivemind
`qwen3.8-27b@q4_k_m` responses. All four returned HTTP 200; request, response,
status and stderr artifacts are retained in
`/tmp/usdgen-fanin-review-20260912/`. Their useful consensus was immutable
producer versions, explicit physical conflicts, stable authored join order and
failure propagation. Suggestions that assumed undeclared lane masks or a
generic reducer were not adopted because current unary operator mathematics do
not define those semantics.

The CPU-safe dependency test covers direct producer edges, concurrent immutable
sibling reads, read/read and every reader/writer hazard combination, distinct
physical identities, stable shuffled lowering, provenance coalescing, complete
publication joins and malformed/cycle diagnostics. CUDA metadata tests prove
the same contract is emitted and accepted by the production queue. Exact
compiler diagnostics retain the current zero-input CurveSource and unary
Width/Length/Deform arities; lowering does not silently reinterpret a
multi-geometry-input node as a merge. The five focused tests passed 5/5;
TaskGraph and CUDA Width DAG each passed 20 consecutive repetitions. Compute
Sanitizer memcheck and initcheck on the CUDA Width DAG each reported zero
errors. The CUDA-disabled TaskGraph and CPU COW fan-out targets built and passed
2/2. The final CUDA-enabled suite passed **112 tests with one
environment-conditional Storm test skipped** (113 total) in 26.13 seconds.
`git diff --check` is clean.

This advances phase 4 but does not claim arbitrary fan-in execution. No shipped
geometry operator currently declares merge/reduction mathematics, so accepting
two geometry inputs would change authored meaning. A future multi-input
capability must define deterministic input order, output topology/stable-ID
rules, RNG identity and a concrete CUDA reducer before that case becomes
executable. Reference/map task lowering, topology-changing branch revisions,
cost/peak estimates and admission, extra-plane COW, actual GPU interval traces,
immutable cache/dirty propagation, CUDA Graph specialization and concrete
Metal/Vulkan capability adapters remain open.

### CUDA Width DAG device-overlap witness checkpoint (2026-09-12)

The CUDA Width-DAG test path now records actual device concurrency on the
production branch streams. After the existing two-launcher host rendezvous,
each legal Width branch enqueues a one-thread device probe as its first
independent command. The probes atomically update active, maximum-active and
arrival counters, then wait only until the expected arrivals appear or a
strictly clamped `clock64()` deadline expires. `maxActive >= 2` is therefore a
device-side witness that two probe kernels executed simultaneously; the
bounded timeout makes a dependency-ordered/same-stream control finish with
`maxActive <= 1` instead of deadlocking. This deliberately does not claim that
every short Width kernel overlaps, nor arbitrary fan-in or performance gain.

The hook is dormant unless explicitly armed and adds no CUDA command or
synchronization to normal execution. Armed state is initialized on a private
nonblocking stream and handed to branch streams by an event wait. Every relay
retains shared witness ownership through its terminal callback; an unproved
failure abandons/quarantines the device state rather than freeing storage still
referenced by submitted work. Snapshot readback is test-only and occurs after
queue drain. The exposed result contains only status, counts, task IDs and lane
IDs, with no CUDA native type, so Metal/Vulkan can implement the same evidence
contract independently.

COW remains the data contract, not merely a capability bit. Sibling Width
tasks read an immutable predecessor view and each allocate a distinct owned
`DeviceBuffer<float>`; only the authored terminal owner's buffer is moved into
publication. Probe scratch is separate and never aliases or mutates source,
predecessor or sibling Width storage. The positive test uses different sibling
Width values while requiring the selected terminal value, and the serial
negative control plus re-arm verifies that counters cannot manufacture an
overlap result from dependency-ordered tasks.

The required external review used two Spark `qwen3.8-flash-next` responses
(`chatcmpl-ba23343d4d5d6fbd`, `chatcmpl-aac00566f5f54fb4`) and two successful
Hivemind `qwen3.8-27b@q4_0` responses. Initial concurrent Hivemind attempts
against `qwen3.8-27b@q4_k_m` returned one model-load HTTP 500 and one timeout;
the serialized retries returned HTTP 200. Requests, responses and status
artifacts are retained under
`/tmp/usdgen-width-device-overlap-20260912/`. Suggestions to treat CUDA event
brackets as proof of simultaneous execution, reset the whole CUDA device, or
expose opaque native handles in plan metadata were rejected.

The focused CUDA Width DAG target passed 20 consecutive runs after replacing a
flaky fixed dwell with the bounded arrival rendezvous. Compute Sanitizer
memcheck and initcheck each reported zero errors. The CUDA-disabled TaskGraph
and CPU COW fan-out targets built and passed 2/2. The full CUDA-enabled build
completed and the suite passed **112 tests with one environment-conditional
Storm test skipped** (113 total) in 37.12 seconds. `git diff --check` is clean.

This supplies device-side evidence for legal Width branch-stream concurrency
while preserving resource-granular COW. It does not close topology-changing
branch values, declared multi-input merge/fan-in, reference/map lowering,
cost/peak admission, extra-plane COW, immutable-cache dirty propagation, CUDA
Graph specialization, or concrete Metal/Vulkan adapters.

### Backend-neutral task estimates and COW graph peak checkpoint (2026-09-12)

Execution-plan metadata now carries backend-neutral, checked memory estimates
without claiming an admission policy that the allocator cannot yet enforce.
Each task reports steady bytes, retained-output bytes, producer-retention
bytes, scratch-peak bytes, memory availability/conservatism, and optional time
availability/microseconds. The plan reports concurrent peak bytes, immutable
shared-input bytes, availability and whether the total is a conservative upper
bound. All byte addition and multiplication goes through the shared checked
arithmetic helper, so overflow makes an estimate unavailable instead of
wrapping. The CUDA capability matrix is version 4 for this metadata contract.

The current static CUDA formulas are deliberately auditable. CurveSource
payload is 32 bytes per point plus 12 bytes per curve and a 4-byte terminal
offset, or 24 bytes per curve when roots are present. A no-resample source has
no task scratch; a statically sized resample retains the new output and its
original producer value. Width retains a private 4-byte-per-point COW output
and uses a private 4-byte-per-point staging buffer, two 257-float LUTs and two
integer error scalars. Publication scratch is 116 bytes per tile plus 32 fixed
bytes, derived from device and pinned spans, extrema, bounds scratch, statuses
and the optional topology result. Compile-time CUDA size assertions guard the
layout assumptions behind that publication formula.

COW is explicit in the graph peak rather than hidden behind a generic total.
An immutable source allocation shared by sibling branches is identified and
counted once. Every live branch contributes its distinct retained Width output
and private staging/LUT/error scratch, and branch outputs remain live through
publication. Thus a two-sibling Width fan-out peaks at the shared source plus
two private retained Width buffers plus two private Width scratch sets; nested
Width producer retention is also represented without counting the same
immutable producer identity twice. Tests verify exact totals, distinct branch
ownership, stable estimates after task-order shuffling, empty inputs, dynamic
resample unavailability and overflow behavior through the production checked
arithmetic API.

Length and Deform expose the portions that are known but leave memory
availability false because their CUB, cuSolver or runtime survivor/budget
workspace is not fully bounded yet. Expression-dependent cardinality is also
unavailable. No execution-time numbers are fabricated: time remains
unavailable until measured or analytically justified.

The required external review used two Spark `qwen3.8-flash-next` responses
(`chatcmpl-a00a6a9c6a000ccd`, `chatcmpl-b08aaf47e1184023`) and two Hivemind
`qwen3.8-27b@q4_0` responses. All four returned HTTP 200; requests, responses
and status artifacts are retained under
`/tmp/usdgen-task-estimates-20260912/`. Their common warning was adopted:
snapshot-only admission is racy, and reserving the graph peak separately from
the existing exact buffer permits would double-charge memory. Enforcement is
therefore intentionally deferred until a reservation ticket can transfer and
consume credits through actual allocations.

The three focused estimate targets passed, then each executable passed 20
consecutive runs. CUDA-disabled TaskGraph and CPU COW fan-out coverage built
and passed 2/2. Compute Sanitizer memcheck and initcheck on the CUDA Width DAG
each reported zero errors. The full CUDA-enabled build completed and the suite
passed **112 tests with one environment-conditional Storm test skipped** (113
total) in 37.73 seconds.

This completes truthful static metadata and COW-aware peak estimation, not
memory admission. Reservation-ticket admission, complete Length/RBF workspace
bounds, reference/map tasks, topology-changing DAG values, extra-plane COW,
immutable-cache dirty propagation, CUDA Graph specialization and concrete
Metal/Vulkan adapters remain open.

### Transferable peak-credit reservation checkpoint (2026-09-12)

The backend-neutral resource pool now has a move-only
`UsdGenExecutionMemoryReservation` and an explicit `Pending` ledger category.
`TryReserveMemory` atomically precharges one isolated byte balance against the
same per-device capacity used by ordinary allocation permits; it does not use
a diagnostic snapshot, job-id map, global current-job state or thread-local
allocator context. `Consume` transfers part of that job's Pending balance into
an ordinary Active, Pinned, Cache or Scratch permit without changing total
charged bytes. Ordinary reservations and permits cannot target Pending.

The credit is recyclable because this reservation represents a concurrent
peak, not cumulative allocation traffic. Releasing a consumed child while the
reservation is live moves its exact bytes back to Pending so a later graph
phase can consume them. Closing the reservation returns only its then-unused
Pending remainder; retained COW outputs and other outstanding children remain
charged and release normally after their real lifetime ends. Child release and
reservation close share one backend-neutral state mutex as a conservation
transaction. An abandoned child never recycles: its exact category and total
charge remain permanently conservative after an unproved backend free.

This primitive preserves the COW model. A future CUDA job ticket will reserve
the already-computed graph peak in which an immutable sibling source is counted
once and every private branch output/scratch set is counted independently.
The pool does not deduplicate source data across concurrently admitted jobs;
doing so without a shared physical-allocation identity and lifetime would be
unsound. Likewise no implicit TLS is used because CUDA callbacks and task
continuations may execute on different host threads.

An allocation-path audit caught and corrected a conservative-bound defect
before admission was enabled: a literal CurveSource resample owns both a
device error scalar and a pinned-host error scalar, so its declared scratch is
`2 * sizeof(int)`, not one integer. Static-resample metadata now tests that
exact value. Length, RBF and expression-dependent graphs remain unavailable
for enforced admission because their complete workspace/cardinality bounds
are not yet known.

The required parallel architecture review used two local Spark
`qwen3.8-flash-next` responses (`chatcmpl-9e44b7248ea16bd8`,
`chatcmpl-9da1d556598b15da`) and two Hivemind
`qwen3.8-27b@q4_0` responses. The initial Hivemind requests exceeded that
server's context limit and returned HTTP 500; compact parallel retries both
returned HTTP 200. Requests, responses and transport status are retained under
`/tmp/usdgen-reservation-review-20260912/`. The adopted consensus was explicit
per-reservation transfer, no snapshot preflight, no TLS and fail-closed unknown
estimates. Suggestions to globally deduplicate COW source bytes across jobs or
to make the ledger own backend allocation handles were rejected because the
current contract has neither shared physical identity nor backend-native types.

Tests cover atomic saturation, no-steal isolation, over-consume and invalid
category rejection, move/idempotent semantics, category transfer, cumulative
phase allocation greater than the reserved peak through recycling, outstanding
child lifetime, abandon quarantine, and concurrent child-release/reservation-
close linearization. The resource executable passed 100 consecutive runs.
Focused CUDA estimator/Width-DAG tests passed 3/3; CUDA-disabled resource,
TaskGraph and CPU COW fan-out coverage passed 3/3. The full CUDA-enabled build
completed and the suite passed **112 tests with one environment-conditional
Storm test skipped** (113 total) in 38.02 seconds. `git diff --check` is clean.

This checkpoint supplies the conservation primitive, not queue admission.
Enabling CUDA admission remains gated on explicit propagation into every
allocation covered by a statically available Source/Width graph estimate:
DeviceBuffer reset, source/resample storage, private Width COW output and
scratch, finalization device/pinned storage and topology scratch must consume
the job ticket rather than independently charging the pool. Persistent
workspace/evaluator/cache storage and already-published generations must remain
on their existing permits. Turning admission on before that end-to-end split
would either double-charge or allow an uncovered allocation to bypass the
ticket.

### Async CUDA Source/Width peak admission checkpoint (2026-09-12)

Known conservative Source/Width plans are now admitted before their async job
is submitted to the shared task graph. Job creation selects the workspace's
CUDA device and atomically precharges `concurrentPeakBytes` through the
backend-neutral memory-reservation primitive. Capacity rejection therefore
occurs before source allocation or device work, preserves the queue's last-good
snapshot, and reports the precise CUDA budget diagnostic. Arithmetic overflow
also fails before submission. Plans whose memory remains unavailable or
non-conservative (currently Length, RBF and expression-dependent paths) keep
the existing exact-per-allocation behavior; the direct synchronous
`ExecuteCudaGraph` path also remains exact-per-allocation and is not claimed as
peak-admitted.

The reservation is propagated explicitly through every allocation represented
by the admitted estimate: CurveSource payload, static resample output and
diagnostics, Width LUT/mask staging, private Width output, Width internal
staging and error state, finalization device and pinned-host storage, and the
optional topology comparison. A reservation-backed allocation has no pool or
free-memory fallback. Same-size `DeviceBuffer` reuse is detected before credit
consumption, while a fresh-allocation failure preserves the old buffer and its
permit. Phase-local child permits recycle into the same job's Pending balance;
publication closes only unused Pending credit, leaving generation-owned child
permits charged until their actual COW storage dies. Unsafe CUDA quarantine
closes unused Pending credit before abandoning submitted allocation permits, so
unproved storage remains charged without leaking the unrelated remainder of
the ticket.

COW remains the physical and accounting contract. A fan-out job reserves its
immutable source once, and each sibling Width task consumes separate private
output and scratch credit. Tests retain the existing pointer-distinct sibling
outputs and terminal-selection checks, and now also stop the async graph while
the branches are live: the pool total rises by exactly the compiled graph peak
with a nonzero Pending remainder. The test-only 12-byte overlap witness is
charged before that baseline because it is deliberately outside plan
metadata. After publication Pending is zero while the selected generation's
owned allocation remains charged. Source-gated linear execution proves the
same exact-total/no-double-charge property.

The end-to-end allocation audit corrected the earlier resample classification.
`CudaCurveResample::CommitFreshFinish` moves the resampler into the published
generation, so its device and pinned-host error scalars remain live with that
owner. Static-resample metadata therefore classifies
`2 * sizeof(int)` as steady auxiliary storage and zero as resample scratch.
This paragraph supersedes the scratch classification in the preceding
transferable-reservation checkpoint; the total conservative bound is unchanged.

The integration reused the required two local Spark
`qwen3.8-flash-next` reviews (`chatcmpl-9e44b7248ea16bd8`,
`chatcmpl-9da1d556598b15da`) and two successful Hivemind
`qwen3.8-27b@q4_0` compact reviews retained under
`/tmp/usdgen-reservation-review-20260912/`, followed by a separate read-only
CUDA allocation audit. The implemented boundary follows their common advice:
explicit per-job transfer, no TLS, no snapshot-based admission and no global
cross-job COW deduplication.

Focused Queue and Width-DAG executables each passed 20 consecutive runs, and
the reservation-resource executable passed 100. Compute Sanitizer memcheck and
initcheck reported zero errors for both async Queue and Width-DAG paths.
CUDA-disabled resource, TaskGraph and CPU COW fan-out tests built and passed
3/3. The full CUDA-enabled build completed and the suite passed **112 tests
with one environment-conditional Storm test skipped** (113 total) in 37.71
seconds. `git diff --check` is clean.

This closes async queue admission only for the currently conservative
Source/Width subset. At this checkpoint the synchronous direct path was still
open; the following checkpoint supersedes that limitation. Also open were
complete Length/RBF/expression estimates and reservation propagation, published-lifetime
category reclassification, topology-changing DAG values, declared fan-in,
reference/map lowering, extra-plane COW, immutable-cache dirty propagation,
CUDA Graph specialization, and concrete Metal/Vulkan resource adapters using
the same backend-neutral estimate and reservation contracts.

### Direct synchronous CUDA peak admission checkpoint (2026-09-13)

Direct `ExecuteCudaGraph` calls now use the same atomic peak-admission contract
as the async queue for the known conservative Source/literal-Width subset. The
workspace device is selected and `concurrentPeakBytes` is reserved before
`ExecutionState::Prepare` or any device work. A request with only peak minus
one byte available fails without changing pool usage or creating pending work,
and reports `CUDA: CUDA execution job memory reservation exceeds the available
device budget`. Size overflow also fails before allocation. Length, RBF,
source-expression and Width-expression plans remain deliberately outside this
boundary and continue to use exact-per-allocation permits.

The synchronous reservation is explicitly threaded through Source and static
resample storage, Width profile/mask scratch, each private Width output, Width
internal scratch, finalization tiles/bounds/status storage and optional topology
comparison scratch. The reservation outlives `ExecutionState`; closing it
releases only unused Pending credit, while child permits retained by the
returned generation remain charged until their actual buffers die. Existing
workspace, evaluator, cache and previous-generation allocations retain their
independent exact permits.

COW is preserved both physically and in the ledger. An immutable source is
charged once for the graph, sibling Width branches retain distinct value IDs,
owners and output buffers, and each private divergent output consumes its own
child permit. The conservative synchronous bound intentionally includes some
async-only diagnostic/pinned-host storage and sums fan-out scratch that the
serial direct executor does not overlap; unused credit therefore returns from
Pending on close instead of weakening the bound or double-charging source data.
A static-resample-plus-Width test covers the same peak-minus-one rejection and
successful retained-generation accounting.

Pool admission remains the linearizable `TryReserveMemory` operation. CUDA free
memory and immutable configured headroom are consulted only as a best-effort
guard against allocations made outside this pool; a resource snapshot is not
used as the admission decision. No CUDA-native handle was added to the portable
estimate/reservation interfaces, leaving the contract suitable for future Metal
and Vulkan adapters without pretending those adapters already exist.

The design was challenged by two local Spark `qwen3.8-flash-next` reviews
(`chatcmpl-96dd666516912e61`, `chatcmpl-8558e4c6541bc718`) and two successful
Hivemind `qwen3.8-27b@q4_0` reviews retained under
`/tmp/usdgen-sync-reservation-review-20260912/`. A separate read-only CUDA
allocation audit found no uncovered normal-path device allocation in the
admitted subset. Implementation and tests were split across Terra and Luna
workers, then reviewed and validated in the root thread. Suggestions premised
on CUDA Graph capture, mutable shared source buffers, non-atomic admission or
general dynamic-branch support were rejected because those capabilities are
not part of this executor or checkpoint.

Focused resource, plan, queue, stage, Width-DAG and topology tests passed. The
Width-DAG, stage and topology executables each passed 20 consecutive warm-device
runs; resource coverage passed 100 repetitions. Compute Sanitizer memcheck and
initcheck reported zero errors for Width-DAG, stages and topology. A cold-device
run initially saw transient CUDA stream/workspace initialization failures in
Width-DAG/topology before their test logic; both direct reruns and the repeated
runs were clean. CUDA-disabled resource, TaskGraph and CPU COW fan-out tests
built and passed 3/3. The full CUDA-enabled build completed and the suite passed
**112 tests with one expected Storm surgery skip** (113 total) in 37.95 seconds.

At this checkpoint complete Length/RBF/expression estimates and reservation
propagation were still open; the following checkpoint closes literal linear
Length only. Also open are published-lifetime category reclassification,
topology-changing DAG values, declared fan-in, reference/map lowering,
extra-plane COW, immutable cache dirty propagation, CUDA Graph specialization,
and concrete Metal/Vulkan resource adapters using the same backend-neutral
contracts.

### CUDA literal-Length runtime-refined admission checkpoint (2026-09-13)

Literal linear Source/Length graphs, optionally containing literal Width, now
receive reserve-before-work admission in both the asynchronous queue and direct
`ExecuteCudaGraph` path. Compile-time metadata remains honest: the graph's
static `memoryAvailable` and `conservativeUpperBound` stay false because CUB's
scan workspace depends on the selected CUDA implementation/device, while the
new backend-neutral `runtimeRefinementAvailable` bit states that a conservative
bound can be completed after device selection and before task submission or
`Prepare`. The CUDA capability matrix is version 5 for this executor-contract
change. Expression-driven source/operators, RBF and nonlinear plans do not set
the bit and retain exact-per-allocation behavior.

The CUDA adapter exposes a null-storage CUB scan query using the same pointer
iterator types as compaction. It performs no usdGen allocation or kernel launch.
Failure to select the workspace device, query CUB, perform checked arithmetic,
or atomically reserve the result rejects the job before execution. No fixed CUB
coefficient, padding guess, resource snapshot admission, TLS or global current-
job state is used. CUB/CUDA types remain outside the execution-plan and memory-
reservation contracts so Metal and Vulkan can provide their own backend workspace
refinements later.

For input point count `P`, curve count `C`, selected-device CUB bytes `S(C)` and
normalized geometry payload `G(P,C)`, the refinement assumes every curve and
point survives. `G` is `32P + 12C + 4`, or `32P + 24C + 4` when paired root
bindings are present. A retained compactor contributes device error/counts,
four `uint32[C]` scan arrays, `S(C)`, and asynchronous pinned counts. Length
adds its external mask/changed-points/keep buffers, internal point/keep staging,
device status and asynchronous pinned status. Publication and, when present,
the worst serial Width overlap are included. Direct execution intentionally
uses the async-safe bound and simply returns unused credit.

COW and owner lifetimes determine the geometry multiplier rather than logical
node count alone. The immutable source is charged once and the compacted output
is a private retained owner. A normal later Length covers current and candidate
compacted geometries and both retained compactor workspaces. Static resampling
followed by a second or later Length additionally keeps the resampled geometry
alive alongside source, current compacted and candidate storage, so the bound
uses three full survivor geometries. This undercount was found during the root
audit and fixed before validation. Width storage is added only when a Width node
exists. The compiled partial Length estimate was also corrected to the actual
known mask, `24P`, `18C`, and device/pinned status/count bytes, while remaining
unavailable until runtime CUB refinement.

Every covered allocation now consumes the explicit job ticket: external Length
mask/changed-points/keep, `CudaLength` staging/device and pinned diagnostics,
compaction error/count/prefix/scan storage, pinned survivor counts, exact private
survivor output channels, and existing Source/Width/publication allocations.
Partial and all-cull results consume less output credit; closing the reservation
returns unused Pending while the published compactor's child permits remain
charged until its COW generation owner dies. Proven failures recycle normally;
only native work lacking completion proof follows the existing quarantine rule.

The required external challenge used two local Spark
`qwen3.8-flash-next` reviews (`chatcmpl-8a1059c4f72b2f01`,
`chatcmpl-90bed470946606a0`) and two successful Hivemind
`qwen3.8-27b@q4_0` reviews. All returned HTTP 200; requests, responses and status
artifacts are retained under
`/tmp/usdgen-length-admission-review-20260913/`. Their shared recommendations
for selected-device refinement, all-survivor output and explicit ticket
propagation were adopted. Suggestions to add guessed safety padding, assert
physical-address reuse, reserve against physical pages, or quarantine ordinary
proved failures were rejected because those are not contracts of CUB, the byte
ledger or the executor.

Tests cover CUB zero/nonzero query accounting, runtime-refinement metadata,
full and peak-minus-one rejection with an unchanged ledger/last-good generation,
direct and staged async success, Pending retirement, retained private owners,
partial/all cull, and the resample-plus-second-Length lifetime multiplier. Queue
gates prove a held admitted Length job owns Pending and returns to baseline after
drain. The five affected executables passed 20 consecutive runs each. Compute
Sanitizer memcheck and initcheck reported zero errors for stages, Length and
compaction. CUDA-disabled resource, TaskGraph and CPU COW fan-out tests built
and passed 3/3. The full CUDA-enabled build completed and the suite passed
**112 tests with one expected Storm surgery skip** (113 total) in 37.60 seconds.

Still open are expression/evaluator conservative bounds and ticket
propagation, published-lifetime category reclassification, topology-
changing DAG values, declared fan-in, reference/map lowering, extra-plane COW,
immutable-cache dirty propagation, CUDA Graph specialization, and concrete
Metal/Vulkan resource adapters using the same runtime-refinement contract.

### CUDA literal-RBF runtime-refined admission checkpoint (2026-09-13)

Literal `CurveSource -> Deform` graphs with no source/operator expressions and
no resample now reserve one selected-device CUDA ticket before async queue
submission or direct `ExecuteCudaGraph` preparation. Static graph memory stays
unavailable: the plan instead truthfully sets `runtimeRefinementAvailable` for
this exact prepared-surface shape. The CUDA capability matrix is version 6 for
this executor-contract change. Expression-driven RBF sample budgets,
source expressions, resampling and longer/mixed chains retain exact
per-allocation admission.

For surface vertex count `V`, offset/index counts `Fo`/`Fi`, groom point/curve
counts `P`/`C`, clamped sample count `N=min(V, rbfSamples)`, augmented order
`M=N+4`, and selected-device LU workspace query result `W` in doubles, the cold/rebind ticket
delta is `32V + 8(Fo+Fi) + 36P + 64N + 12C + 8M^2 + 8W + 52M + 1696`, plus the
already-audited Source geometry and final-publication scratch. Existing
accepted cache/output is already charged in the shared pool and remains live
through accept/rollback; it is therefore intentionally not recharged to the
new ticket. The adapter calls the same legacy `cusolverDnDgetrf_bufferSize`
routine used by execution with null matrix storage after selecting the target
device, before any usdGen allocation or submission. This matters for small
systems: the selected implementation reports 96 doubles at `M=8`, exceeding
`M*M=64`, so a documented generic-API matrix-size assumption is not a safe
substitute for the legacy API query.

The fixed `1696` term includes mask/profile, deformer status/proof and the
fresh RBF packets. It intentionally does not double-count the bind
`freshActual` host proof: `BeginFreshUpdate` discards that proof before the
update/solve/evaluate/deformer peak, where the live surface fixed storage is
the device error, device actual count and update pinned code (12 bytes). This
is a phase maximum, not an accumulation of mutually exclusive proof handles.

One parent reservation is propagated through Source/finalization and every
RBF-owned allocation: scratch uploads/mask/deformer buffers, async scalar
readback, SurfaceBinding candidates and their pinned proofs, and RBF bind,
solve and evaluate candidates. Candidate surface/RBF device buffers and pinned
packets use `Cache` permits, so an accepted cache continues to own its child
charges after the job closes; temporary deformer/upload permits recycle to
Pending. Same-size reuse remains before credit consumption. Proven failure
releases normally, while unproved work keeps its existing quarantine permit;
the old accepted cache/output is never mutated before publication resolution.

External review artifacts are retained under
`/tmp/usdgen-rbf-admission-review-20260913/`: Spark architecture
`chatcmpl-863d38df55efd7fb` (`spark-architecture.json`) and adversarial
`chatcmpl-99e7340cfe54152e` (`spark-adversarial.json`); Hivemind architecture
`hive-architecture.json` and adversarial `hive-adversarial.json` (both HTTP
200). The reviews drove the explicit COW baseline distinction, selected-device
bound, no-snapshot/no-TLS design, and proof-lifetime audit. Root validation
then caught and removed the initial unsafe `W=M*M` assumption: an actual
selected-device query reports 96 doubles for `M=8`, versus 64 matrix elements.

Focused execution-plan, queue, stages, RBF transaction, surface-binding, RBF
and deform tests pass 7/7. RBF transaction, plan, stages, surface-binding, RBF
and deform each passed 20 consecutive runs; the queue test had one unrelated
Length gate timing failure in the combined repetition and then passed 20/20 in
isolation. Compute Sanitizer memcheck and initcheck report zero errors for the
RBF transaction test. After a coherent full relink, the CUDA-enabled suite
passes 112 tests with one expected Storm surgery skip (113 total) in 37.05
seconds. `git diff --check` is clean.

### CUDA expression/evaluator admission and COW checkpoint (2026-09-13)

CUDA parameter programs now expose a checked, allocation-site-derived estimate
for one complete fresh evaluator candidate. It counts each used domain context
once, then every binding's literal, typed output, device IR/error storage and
pinned IR/error proof storage. Groom context cost is 8 bytes. Point and
Primitive contexts use the actual generated channel presence and include the
always-materialized owner and arc-length arrays. `hairT` affects validation and
field values but adds no separate expression-context allocation. All products
and sums reject overflow. The estimate is explicitly the new-candidate delta:
an old published or one retired evaluator candidate remains a globally charged
COW baseline and is never charged to the new job ticket a second time.

The reservation and lifetime category now propagate through synchronous and
fresh evaluator APIs, expression-context construction, expression-program
upload, literals, outputs, device status, and pinned proof packets. Async source
controls and their fixed 64-byte scalar packet use Scratch permits. Successful
operator evaluator candidates use Cache permits and keep their charges after
the job reservation closes. A fresh candidate is allocated privately while the
published field set remains readable; only terminally proven program success
atomically publishes the replacement and retires the old owner. Allocation or
semantic failure leaves the published fields intact, and an unproved CUDA path
retains/quarantines its permit under the existing proof rule.

Fixed-cardinality expression graphs now participate in aggregate admission.
Source `useRest` expressions, Width expressions, Length expressions bounded by
the all-survivor input, and Deform expressions other than `rbfSamples` are
eligible. Length adds every evaluator candidate to the selected-device CUB
refinement. Literal RBF adds source/operator candidates and scalar readback to
its selected-device cuSOLVER refinement. Expression-driven source `resampleTo`
remains unavailable because no finite output-cardinality contract exists;
expression-driven `rbfSamples` remains unavailable because the selected-device
legacy LU workspace cannot be safely bounded before that scalar is proved.
Incomplete RBF root bindings are likewise excluded from refinement. The CUDA
capability matrix is version 7. The logical estimate/reservation interfaces
remain backend-neutral for future Metal/Vulkan adapters; only CUDA supplies
these physical allocation constants today.

Root validation caught an integration undercount before completion: the
literal-resample branch replaced, rather than incremented, source steady bytes
and erased the evaluator candidate. It now checked-adds the resampler status
bytes, preserving both the evaluator and COW geometry terms. The affected CUDA
session then passed 20 consecutive runs.

The required external challenge used two local Spark
`qwen3.8-flash-next` reviews (`chatcmpl-a43de2902d3d4f1c`,
`chatcmpl-a453b0120add702b`) and two Hivemind `qwen3.8-27b@q4_0` reviews
(`resp_0ef23cc4b43832f4c1f94eae2b502e6d6cee36196739cbd2`,
`resp_c498799bb751486cbe9e881a51096cb6d9355b722208d32d`). Requests and
responses are retained under
`/tmp/usdgen-expression-admission-review-20260913/`; all four returned HTTP
200. Terra/Luna workers implemented the low-level propagation and tests, while
the root thread integrated planner/refinement eligibility and performed the
final allocation audit.

Tests cover the exact fresh-candidate bound, one-byte-short denial, overflow,
Pending-to-Cache/Scratch transfer, 64-byte scalar readback, warm published-field
COW preservation, private replacement publication, retained cache lifetime,
bounded/unbounded planner eligibility and incomplete RBF roots. The four
focused evaluator/plan/stage/RBF executables passed 20 consecutive runs, and
the affected CUDA session passed 20 consecutive runs. Compute Sanitizer
memcheck and initcheck report zero errors for the evaluator transaction test.
The coherent CUDA build and full suite pass 112 tests with one expected Storm
surgery skip (113 total); `git diff --check` is clean.

Still open are published-generation category reclassification beyond evaluator
cache ownership, topology-changing DAG values, declared fan-in, reference/map
lowering, extra-plane COW, immutable-cache dirty propagation, CUDA Graph
specialization, and concrete Metal/Vulkan resource adapters.

### CUDA published-generation COW accounting checkpoint (2026-09-13)

Successful CUDA publication now changes the lifetime category of every
allocation physically retained by the new immutable owner from `Active` to
`Pinned` without changing `usedBytes`. `DeviceBuffer::Reclassify` delegates to
the allocation permit, and Source, resample and compaction owners enumerate
their active geometry channels plus retained status, pinned proof, count,
prefix and scan storage. Private point and width revision planes are likewise
reclassified. The transition is owner-exclusive and sequentially idempotent;
it is never concurrent with permit release or abandonment.

The generation owner is the COW boundary. A point or width revision
reclassifies only its newly allocated private plane. It does not recurse into
the immutable base generation, so shared source/resample/compaction storage is
charged and pinned exactly once. Tile-metadata wrappers reuse the same concrete
owner and perform no resource transition. Generation construction first
retains the concrete owner, then creates the public generation, and performs
the transition only after that creation succeeds. Failed publication therefore
cannot manufacture `Pinned` bytes, while the prior published generation and
its leases remain unchanged.

Normal finalization now seals the job reservation after publication and
candidate cleanup but before user-visible completion. Retained generation
permits remain globally charged as `Pinned`; unused ticket credit returns from
`Pending`. Proven terminal Source/operator/finalization failures also close the
reservation before their callback. Unproved native work continues to abandon
its permit into the bounded quarantine path. Source and compaction teardown
were hardened so partially submitted or otherwise unproved CUDA storage is
quarantined rather than freed, while sequential pre-submit failures discard
their candidate and staging state normally.

The required external review used two local Spark `qwen3.8-flash-next`
responses (`chatcmpl-9965db9bcd1dab9a`,
`chatcmpl-ba12cd2b1da7f47c`) and two Hivemind `qwen3.8-27b@q4_0`
responses (`resp_0576db4a7409a33bd62aa48c37c8dd30036cd0f7f493df2f`,
`resp_26205ac5778f9916ec88d2a6926925e458875553998c9a25`). All returned HTTP
200; artifacts are retained under
`/tmp/usdgen-published-reclass-review-20260913/`. Their common recommendation
that the ledger remain authoritative, shared COW bases remain untouched, and
failed publication create no pinned charge was adopted.

Retirement coverage proves generic `Active -> Pinned` conservation; Source,
resample and compaction retained-owner accounting; point- and width-revision
COW isolation; metadata-wrapper identity; failed-publication invariance; and
return to the initial ledger after generations and leases drain. The retirement
test passed 20/20 consecutive runs; Source and compaction tests passed; the
TaskGraph and generic pipeline regressions each passed 100/100. Compute
Sanitizer memcheck and initcheck report zero errors for CUDA retirement. A
coherent 119-target CUDA build completed, and the suite excluding the known
queue stress test passed 111 tests with one expected Storm surgery skip (112
selected tests total) in 37.18 seconds.

The integrated CUDA queue still has an independently reproducible intermittent
cross-graph drain stall under the multi-producer/reentrant stress section (a
bounded repetition timed out on run 3/5). Temporary diagnostics showed no
active CUDA relay and an admitted dispatcher job whose scheduled graph work did
not progress while only the pipeline graph was being drained. Scheduler
experiments were removed; this remains an explicit execution-graph follow-up,
not a COW-accounting success claim.

Still open are the cross-graph drain/progress fix, topology-changing DAG values,
declared fan-in, reference/map lowering, extra-plane COW, immutable-cache dirty
propagation, CUDA Graph specialization, and concrete Metal/Vulkan resource
adapters using the same backend-neutral accounting contract.

### Backend/device actor and cross-graph progress checkpoint (2026-09-13)

The cross-graph drain stall is closed. `UsdGenExecutionTaskGraph` is now one
process-lifetime actor per backend/physical-device key with a fixed worker pool,
bounded job/task/edge admission, mutex-serialized ready ownership and no
dependency on any caller's TBB arena or flow graph. CUDA queues and session
cookers from distinct runtimes therefore share an independently driven device
dispatcher. `Drain` covers queued/running tasks and terminal callbacks,
including a successor submitted reentrantly from a completion, without asking
one flow graph to make progress for a sibling graph.

Task completion retains the actor until an asynchronous terminal arrives and
uses a two-phase exactly-once gate so synchronous completion cannot release a
dependent before its `run` call returns. Admission is released before the job
callback, callbacks execute outside the actor lock, and shutdown cancels
unstarted nodes while waiting for already-launched proofs. The registry owns
the actor strongly for process lifetime: transient acquisition inside a
pipeline callback cannot destroy and synchronously join the device dispatcher
on that callback stack. Explicit `Shutdown` is consequently an
application/DSO-only, permanently closing operation. A zero interactive burst
is rejected rather than creating an accidental background-starvation policy.

This scheduling change does not weaken the preceding COW contract. Published
Source/resample/compaction owners still reclassify only their physically owned
planes to `Pinned`; point/width revisions still reclassify only the new private
plane and never recurse through the immutable base generation. Completion and
reentrant admission occur only after the accepted candidate has sealed its
reservation, so shared COW storage is neither recharged nor released by actor
handoff.

The required external review artifacts are retained under
`/tmp/usdgen-cross-graph-review-20260913/`. Local Spark/Qwen architecture and
adversarial reviews returned HTTP 200 as `chatcmpl-b2d51341f29752b4` and
`chatcmpl-9c91456fa65a8273`. Hivemind adversarial and retried architecture
reviews both returned HTTP 200 (`hive-adversarial.json` and
`hive-architecture-retry.json`; the native responses expose no request id).
Terra supplied the independent backend/device actor design and Luna supplied
the shutdown, registry-lifetime, starvation and concurrency test matrix. Their
common recommendation to remove the sibling-arena progress dependency and
make the dispatcher the stable device owner was adopted.

Regression coverage now includes a one-worker completion that submits a
successor on the same CUDA queue, a completion crossing between two independent
one-worker runtimes on the same CUDA device, three concurrent queue producers
followed by a deterministic greatest-ticket publication, generic async fan-in,
completion reentry, bounded admission, fairness, aging, failure pruning and
shutdown. Generic TaskGraph/stress passed 200 consecutive iterations before
terminal-path hardening and 100 consecutive iterations afterward; pipeline
passed 100/100. The CUDA queue passed 50/50 before the final lifetime hardening
and 28 consecutive post-hardening processes before the externally occupied GPU
reported `cudaErrorMemoryAllocation` at initial stream creation; this was a
startup resource failure, not a timeout or graph assertion. A coherent CUDA
build and full suite pass **112 tests with one expected Storm surgery skip**
(113 total) in 29.59 seconds. Compute Sanitizer memcheck and initcheck report
zero errors for the queue test. The CUDA-disabled TaskGraph, stress, pipeline
and CPU COW fan-out build and pass 4/4. `git diff --check` is clean.

Still open are topology-changing DAG values, general declared fan-in,
reference/map lowering, extra-plane COW, immutable-cache dirty propagation,
CUDA Graph specialization, and concrete Metal/Vulkan resource adapters using
the same backend-neutral actor and accounting contracts.

### Topology-changing CUDA DAG and COW checkpoint (2026-09-13)

CUDA lowering now accepts the deliberately restricted topology-changing shape
`CurveSource -> Length -> Width DAG`. There is exactly one Length, it is the
direct child of Source, and it must dominate every Width. Length remains on the
primary stream with its exclusive-workspace hazard; only Width tasks use the
branch streams. Deform branches, Length below a branch, multiple Lengths and
multi-input operators remain rejected because no shipped operator defines the
required immutable merge value.

Plan lowering carries a complete inherited resource-to-logical-value
environment per semantic node. Each unary child inherits its predecessor's
values before binding its own reads and writes. Consequently every Width below
Length reads the compacted geometry, curve topology, stable IDs, roots and
width value produced by Length, independent of authored node order. Source,
Length and Width outputs are `JobOwnedImmutable`; each Width creates a private
width plane and never mutates or reclassifies the compacted base. The execution
capability matrix is version 8 for this contract.

Length owns one immutable post-compaction snapshot: survivor points/rest/
widths, offsets, stable IDs, hairT and root channels. Scatter is committed on
the primary stream before the compactor records its device-use event. Every
downstream branch enqueues a wait on that event, and chained Width values use
their private owner's event in the same way. Finalization requires every DAG
operator to finish and publication depends on the complete task frontier, so a
nonterminal sibling cannot be discarded early or silently ignored on failure.
Publication transfers only the compacted owner and the explicitly selected
terminal Width overlay. `Pinned` is an execution-resource ledger category,
not conversion to CUDA page-locked host allocation; reclassification changes
the permit category while retaining the same device allocation.

Selected-device runtime admission extends the all-survivor Length bound to
this shape. It includes source and survivor geometry, two simultaneously live
compactor packets, the queried CUB scan workspace, Length scratch, publication
scratch, evaluator candidates and a conservative private output/scratch packet
for every Width. This can over-reserve for deep graphs, but it cannot omit a
concurrent sibling or the compactor workspace.

Generic renderer-neutral scalar-array metadata now accepts Float32, Int32,
UInt32 and UInt64 arities 1 through 16 when stride matches element size.
Vector-semantic types retain their exact arities. This supplies the R24
Int32x3/Float32x3 value-description substrate only: generic extra CV/curve
planes are not yet bound, transformed by resample/compaction, scheduled or
published, so extra-plane COW remains open and is not part of this checkpoint.

The required external review artifacts are retained under
`/tmp/usdgen-topology-cow-review-20260913/`. Spark architecture and adversarial
reviews returned HTTP 200 as `chatcmpl-8296e0bc78f9f430` and
`chatcmpl-9897510815e53d31`; Hivemind architecture and adversarial reviews
returned HTTP 200 as `resp_60ba0980033fdff7cf26e6f039a7e5701e4b6359bdc4cb07`
and `resp_74d96155f68fef7d9e4544af350639327406321af7a05865`.
Their stream-visibility, sibling-failure, workspace-accounting and physical-
reclassification challenges were checked against the concrete implementation.
A topology-specific failing nonterminal sibling now proves both direct and
queued execution abort atomically, preserve the accepted COW generation and
leave Pinned/Pending accounting unchanged.

The topology DAG test passes directly after adding that failure coverage, and
its updated binary reports zero errors under both Compute Sanitizer memcheck
and initcheck. Earlier coherent validation for this slice passed the nine
affected CUDA executables and the complete CUDA suite (112 passes plus one
expected Storm skip). Repetition can still be
prevented at process startup by the externally occupied GPU returning
`cudaErrorMemoryAllocation`; no unrelated process is terminated to manufacture
test capacity. The final two-way-parallel 113-test run passed every CUDA and
COW test but `testUsdGenStormLook` segfaulted while sharing the graphics device;
that unrelated Storm test passed immediately in isolation. The CUDA-disabled
core build and TaskGraph/stress/pipeline/DeviceGeneration/CPU fan-out tests
pass. `git diff --check` is clean.

Still open are general declared fan-in and merge semantics, reference/map
lowering, topology-aware extra-plane COW, immutable-cache dirty propagation,
CUDA Graph specialization, and concrete Metal/Vulkan resource adapters.

### Extra-plane COW and Session DAG checkpoint (2026-09-13)

CPU topology-preserving execution now treats named extra planes as plane-granular
COW data. `InputPrimvars()` and `OutputPrimvars()` compile into deterministic,
bounded named slots. Every node refreshes untouched `extraCv` and `extraCurve`
handles from its current immutable predecessor, while each declared output gets
private storage with the predecessor's domain, type and arity. Fan-out siblings
therefore share read-only `VtArray` payloads but never share a writable output.
Incremental recompilation coverage changes Source resampling topology and proves
that stale aliases are replaced and private arrays are resized before raw chunk
pointers are exposed.

The CPU tile boundary now preserves constant/uniform/vertex interpolation,
float/int type and arity for extra-curve and extra-CV planes. It no longer drops
CV planes, drops integer data or coerces all metadata to uniform float. The tile
contract includes a real `guideIndex` producer and proves an exact uniform
Int32x3 payload through Session publication and the imaging data-source path.

CUDA now has a generic named-channel revision owner over an immutable device
generation. A revision may add or replace generic channel storage by name;
standard geometry channels cannot be shadowed. Construction validates name,
domain, cardinality, scalar type, arity, stride, device and physical byte size
transactionally. Only accepted private allocations are reclassified
`Active -> Pinned`; the base is never recursively reclassified. Lookup inherits
unchanged planes through the generation chain, consumer acquisition retains and
waits on both base and private producer events, and retirement releases each
overlay exactly once. This is an ownership/transport substrate only: it does
not claim CUDA operator lowering for named extra planes.

`UsdGenSessionCooker::CookCudaAsync` now submits the compiler-lowered task
dependencies rather than rebuilding a linear `prior` chain. It validates dense
task IDs and dependency provenance, schedules source and operators from metadata,
and makes publication the declared full join. Session coverage holds both Width
siblings at a test rendezvous, observes two distinct branch streams, proves no
early publication, and verifies that the declared terminal COW width is selected.

The backend-neutral generation identity now names CUDA, Metal and Vulkan
explicitly while retaining stable existing enum values. A generic owner test
constructs valid Metal and Vulkan generations without importing either native
API, pinning the portability boundary for their future synchronization adapters.

The required review used two local Spark `qwen3.8-flash-next` responses
(`chatcmpl-80abe0c8265a65c9`, `chatcmpl-a5b70e8c7432a6d4`) and two Hivemind
`qwen3.8-27b@q4_0` responses
(`resp_a45806cd78c46c783f9a38c09f8e50de3f074f6004ab34e9`,
`resp_dd51e687cd8ed62348d65b6c23e810ab7f01ccdcb4620f6e`). All returned HTTP
200; artifacts are retained under
`/tmp/usdgen-extra-plane-review-20260913/`. Their common requirements—explicit
schema validation, immutable-base/private-overlay isolation, transactional
accounting, complete base-plus-overlay waits, fence-bound retirement and a
backend-neutral public channel contract—are reflected in this slice. Their
warning about topology-changing extra planes remains an explicit rejection
boundary until resample/compaction transforms exist.

The complete coherent CUDA build and 114-test suite pass, with one expected
Storm surgery skip. The CUDA named-channel, CUDA Session task-graph, CPU fan-out
and tile-contract tests pass directly as well. Compute Sanitizer memcheck reports
zero errors for both new CUDA-focused tests. The focused Session test covers
runtime DAG fan-out and publication join; the named-channel test covers invalid
metadata rollback, two-level inherited lookup, exact Int32x3/Float32x3 reads and
full ledger drain. `git diff --check` is clean.

Still open are topology-aware resample/compaction of extra planes and actual
CUDA operator production of named channels; general declared fan-in/merge;
reference/map lowering; immutable-cache dirty/version propagation; CUDA Graph
specialization; and concrete Metal/Vulkan synchronization/resource adapters.

### COW reference/map values and execution-version identity checkpoint (2026-09-13)

CPU compilation now materializes authored reference-role `CurveSet` descriptors
as copy-on-write values. Each candidate is assembled privately, including its
planar point arrays and per-guide data, and is published to the compiled graph
through `shared_ptr<const UsdGenReferenceSet>`. Consumers receive only const
reference pointers and const resolved metadata; a later recompile constructs a
new owner rather than modifying an accepted generation. Map descriptors follow
the same immutable-after-publication boundary through const graph spans. Their
identity includes path, type, resolved asset path, texture generation and a
canonical parameter ordering, so authoring-order-only edits do not invalidate a
capture.

External values are completely validated and materialized before incremental
compilation moves any descriptor or node state. Duplicate descriptors, duplicate
node relationships, missing targets and inconsistent curve topology fail closed.
A rejected recompile preserves both the prior COW owners and an executable prior
graph. After successful compilation every node is rebound by path, including
reused nodes, so descriptor reordering cannot leave stale value indices.

The CPU scheduler executes reference-role nodes as a fenced lane before ordinary
curve-role nodes. It passes authored reference values and map metadata through
the capture/evaluation contexts and folds their immutable identities into capture
reuse. This checkpoint intentionally covers authored `CurveSet` payloads; it does
not claim reference-node-produced chaining, map texture sampling, or unchunked
multi-level reference topology.

CUDA validation rejects every authored node reference or map before graph layout,
parameter compilation, memory admission, stream creation or allocation. This is
the fail-closed boundary until CUDA has an upload ABI and lifetime owner for those
values; it is not CUDA reference/map transport.

`UsdGenExecutionInputVersions` supplies a canonical semantic tuple for source,
reference, map and surface identities/generations. Aliased relationship-only
records coalesce with their payload generation, contradictory nonzero generations
invalidate cache admission, ordering is canonical, and equality over every field
is authoritative. Its FNV hash uses an explicit byte order and is stable across
host endianness. Backend, capability, device index and device-context generation
remain a separate `UsdGenExecutionContext`; the composite key additionally names
description, plan digest, layout digest and exact frame bits. CPU, CUDA, Metal and
Vulkan are explicit context identities, but only CPU and CUDA have executors.

Curve/map/surface generation edits update the graph's version tuple and dirty the
affected nodes plus their descendants. This is the identity and invalidation
contract for a future immutable execution cache; it does not yet implement cache
storage, lease retention, eviction, or accepted-tuple compare-and-publish.

The required independent review artifacts are retained under
`/tmp/usdgen-reference-cache-review-20260913/`. The two local Spark/Qwen reviews
returned HTTP 200 as `chatcmpl-9b4ee1616a0c5c2e` and
`chatcmpl-99927ddef47e324b`; the two Hivemind reviews returned HTTP 200 as
`resp_fadf6ed00fff90ae695f0c48ad41d32e7e7ac170cd04f59e` and
`resp_7ed17d6f09dd0b82b3423ae2e45b5616e8eece1d22f52ce6`. Their common
requirements—canonical tuples, full-field equality, semantic input/context
separation, immutable COW publication, exact generation fencing and CUDA
fail-closed behavior—are reflected here.

A coherent CUDA build succeeds. The seven focused reference/map, execution-cache,
CUDA-plan, graph, CPU fan-out, invalidation and device-generation tests pass. The
complete CUDA suite passes **115 tests with one expected Storm surgery skip**
(116 total) in 26.78 seconds. The coherent CUDA-disabled build passes 72 tests
with the same expected Storm surgery skip (73 total) in 11.93 seconds. The
200-to-201-node incremental compile gate also passes 20 consecutive runs;
reference-free graphs do not instantiate operators during external-value
preflight. `git diff --check` is clean.

Still open are reference-node-produced chaining and actual map sampling; immutable
cache storage/leases/eviction and exact accepted-tuple publication; CUDA native
reference/map upload and binding; topology-aware named-plane transforms and CUDA
operators that produce named planes; general declared fan-in/merge; CUDA Graph
specialization; and concrete Metal/Vulkan synchronization/resource adapters.

### Immutable cache store and topology-plane COW checkpoint (2026-09-13)

The execution-cache identity now has a concrete, backend-neutral bounded store.
Published cache state, entry snapshots and payloads are immutable COW data: an
insert or MRU promotion constructs a complete candidate state and swaps it only
after validation, allocation and eviction succeed. Full key equality remains
authoritative after hashing, including under deliberate hash collisions, and
backend/capability/device/context identity partitions otherwise equal semantic
inputs. Exact duplicate keys coalesce to the first accepted immutable payload;
conflicting byte charges, zero-byte values, invalid keys and oversized values
fail without changing the published state.

LRU and byte accounting are bounded. Copyable leases retain their exact snapshot
and explicitly pin it against eviction. Lease counts are separate from
`shared_ptr` ownership because retired COW state generations may overlap a new
publication without representing external consumers. Evicted state and payload
destruction occurs after the cache mutex is released, so payload destructors may
safely re-enter cache inspection. The candidate transaction also preserves the
old size and byte charge when all possible victims are leased or an allocation
fails. This store remains substrate: it is not yet owned by Session/cooker, does
not produce execution cache hits, and does not compare-and-publish an exact
accepted input tuple into Session publication.

CPU now has strict, topology-aware transforms for named extra planes. Resampling
and curve compaction validate source and target topology, unique sorted names,
domain, interpolation, scalar type, arity, exact cardinality and overflow before
publishing private output vectors. Vertex Float planes resample linearly and
vertex Int planes use nearest-neighbor selection without coercion. Uniform and
constant curve planes retain their type and arity; compaction applies the exact
strictly ascending survivor map to curves and complete CV spans. All output
planes are newly owned COW data, and malformed topology, duplicate cross-domain
names, invalid survivor sets and allocation failures leave the target planes
unchanged. These helpers are directly tested but are not yet wired into a real
topology-changing CPU operator or descriptor ingestion of authored extra planes.

CUDA compilation now rejects a topology-changing node with named input or output
primvars during graph preflight, before capability lookup, layout construction,
stream creation or device allocation. The validator registers the built-in
operators itself, so direct `CompileCudaGraph` callers cannot bypass the check.
This is a precise fail-closed boundary, not named-plane CUDA transport: upload,
bindings, resample/compaction kernels and operator-produced named channels remain
open. Metal and Vulkan likewise remain backend identities and neutral contracts,
not implemented resource or synchronization adapters.

The required parallel review artifacts are retained under
`/tmp/usdgen-cache-plane-review-20260913/`. Two local Spark
`qwen3.8-flash-next` reviews returned HTTP 200 as
`chatcmpl-973097ee34d328dd` and `chatcmpl-8f2dc31e46989a93`; two Hivemind
`qwen3.8-27b@q4_0` reviews returned HTTP 200 as
`resp_6b66893330fcd977f550e3094479afd9524e584bdaf23fe1` and
`resp_d5b43d7f82dbd333c381637f77ad77f0f8101cda64c0be4c`. Their collision,
transaction, lease/COW, destructor re-entry, topology/cardinality and fail-closed
transport findings are covered by the implementation and tests.

Both coherent builds succeed. The cache concurrency/lease test passes 100
consecutive repetitions after separating external leases from retired-state
ownership. The CUDA topology-plane preflight and CPU plane-transform tests pass
50 consecutive repetitions. The complete CUDA-disabled suite passes 72 tests
with one expected Storm surgery skip (73 total), and the complete CUDA suite
passes 115 tests with the same expected skip (116 total). No new device kernel
or allocation is exercised by this checkpoint's CUDA rejection path, so Compute
Sanitizer is not applicable. `git diff --check` is clean.

Still open are Session-integrated immutable cache lookup/storage/eviction and
exact accepted-tuple publication; actual CPU topology-operator wiring and source
named-plane ingestion; CUDA named-plane upload, binding, resample/compaction
kernels and operator production; reference-node-produced chaining, map sampling
and CUDA reference upload; general declared fan-in/merge; CUDA Graph
specialization; and concrete Metal/Vulkan resource/synchronization adapters.

### Session COW cache and CUDA named-plane topology checkpoint (2026-09-13)

Each Session cooker now owns a bounded immutable execution cache and uses it for
CPU publication. The exact key combines description, semantic plan and layout
digests, frame bits, canonical source/reference/map/surface versions, backend
capability identity and evaluation context. The semantic digest includes value
parameters, ramps, expression bindings, presentation terms, resolved map
parameters and external descriptor metadata; key equality, not its hash, remains
authoritative. CUDA lookup and admission remain disabled until device identity,
events, memory accounting and retirement can be retained as one cache lease.

Successful CPU evaluation produces a deferred candidate. Only the Session
pipeline's current accepted publication action may admit it, and it rechecks the
candidate generation and freshly recomputed key. Failed, cancelled, superseded
or newly dirtied work cannot seed the cache. A hit copies the immutable
generation into a fresh publication identity while its `VtArray` payloads remain
COW-shared. Because a hit does not evaluate the cooker's mutable graph buffers,
the next preparation rebuilds the graph baseline before any real execution; a
later incremental miss therefore cannot reuse buffers belonging to a different
cached publication.

CUDA now has a concrete named-channel topology transform substrate. It rebuilds
every Generic overlay from an immutable source generation onto a completed
resample or compaction generation, producing private device buffers without
modifying either input owner. Float32 point channels interpolate, Int32 point
channels select the nearest source CV, primitive channels follow stable curve
IDs, and groom-constant channels copy once. Type, arity (1--16), stride, domain,
cardinality, stable IDs, device and generation order are checked transactionally.
Source/target/channel leases wait on their producer events; allocations consume
the caller's aggregate reservation when supplied; failed or unproven submissions
are rolled back or quarantined; successful private outputs record their use and
retire through the existing CUDA owner/resource service.

This utility deliberately synchronizes its stream before returning. The CUDA
execution graph still rejects topology-changing nodes with named planes because
descriptor ingestion and async invocation of the transform are not wired into
source/operator tasks. That is a fail-closed graph boundary, not a claim that
the new transform is Metal/Vulkan portable. Metal and Vulkan remain explicit
backend-neutral identities only; they still need concrete allocation, fence,
queue and retirement adapters.

The required parallel review artifacts are retained under
`/tmp/usdgen-session-cuda-plane-review-20260913/`. Two local Spark
`qwen3.8-flash-next` reviews returned HTTP 200 as
`chatcmpl-8d6b96b73c5348f4` and `chatcmpl-b9709ac34ee198a4`; two Hivemind
`qwen3.8-27b@q4_0` reviews returned HTTP 200 as
`resp_3b04135515bae150b6cbd4b4da4e62060caf0465b1a6ef35` and
`resp_13a08c00c383863e1c2c28117355a37515b3f35e059c8b7d`. Their common
requirements--accepted-publication-only cache admission, full semantic keys,
immutable/private output ownership, exact topology validation, reservation
rollback, producer-event waits and honest portability boundaries--are reflected
in this slice.

Both coherent builds succeed. The CUDA-disabled CPU suite passes 72 tests with
one expected Storm surgery skip (73 total). The CUDA suite passes 116 tests with
the same expected skip (117 total). The Session cache test passes 100 consecutive
runs and the CUDA named-topology COW test passes 50 consecutive runs. Compute
Sanitizer memcheck and initcheck both report zero errors for the CUDA transform
test. The first simultaneous cross-build suite run exposed a shared Storm scratch
directory collision; the isolated test and the subsequent coherent CUDA suite
both pass. `git diff --check` is clean.

Still open are CUDA execution-graph ingestion and async scheduling of named-plane
topology transforms, CUDA operator production/upload of named channels, CPU
topology-operator wiring and authored source-plane ingestion, device execution
cache leases, cross-Session cache/coalescing and metrics, reference-node-produced
chaining, map sampling and CUDA reference upload, general declared fan-in/merge,
CUDA Graph specialization, and concrete Metal/Vulkan adapters.

### Authored-plane COW ingestion and async CUDA transaction checkpoint (2026-09-13)

The backend-neutral graph descriptor now carries explicitly authored Float32 or
Int32 named planes in Point, Primitive, or Groom domains with arity 1--16.
Compilation validates unique non-empty names, enum values, multiplication
overflow, typed payload exclusivity, and exact topology cardinality before
mutating the destination graph. There is no implicit previous-generation
carry-forward: removing the authored relation removes the planes, while an
invalid descriptor leaves the prior compiled graph and publication intact.

CPU CurveSource materializes those planes through `VtArray` COW. A
topology-preserving capture initially shares the immutable descriptor payload;
unchanged downstream fan-out retains that same read-only allocation. A source
resample or any declared writer materializes private storage before changing
values. Point Float32 values interpolate, Point Int32 values use the nearest
source CV, and Primitive/Groom data retain their uniform/constant domain. Tests
compare backing addresses as well as values and prove that writes and topology
rebuilds cannot mutate the immutable source or a sibling branch.

CUDA source execution now uploads the same descriptor planes into private,
reservation-charged device buffers on the job stream. Their metadata and bytes
remain unpublished until final generation construction succeeds; the generation
owner records producer events, exposes read-only leases, reclassifies only the
accepted buffers, and retires them through the existing service. An end-to-end
Session test retains a named-channel lease across a later descriptor revision
and proves that the older generation still reads its original value. Thus CUDA
uses generation-level COW ownership (private copy on revision), rather than
aliasing mutable device storage across publications.

Topology transport now also has a nonblocking candidate transaction. Published
generation mode retains source/target geometry and named-channel leases;
job-facing raw mode accepts unpublished geometry/plane views and an optional
lifetime token. Both modes allocate private outputs, enqueue validation and
transforms, record output events, and arm exactly one terminal native callback.
Only a host relay presenting that exact callback status may call `Commit` or
`CommitPlanes`; lost or unproved work is quarantined. The synchronous compatibility
functions are implemented on top of this transaction. Full CurveSource-resample
and Length relay integration remains closed until these raw candidates are
inserted as explicit execution phases, so authored planes with a topology change
still fail before device work instead of publishing stale-cardinality data.

The four required reviews are retained under
`/tmp/usdgen-cuda-named-graph-review-20260913/`. Local Spark
`qwen3.8-flash-next` returned HTTP 200 as `chatcmpl-be69f8bdc068268a` and
`chatcmpl-9563256a1b20b6ce`; Hivemind `qwen3.8-27b@q4_0` returned HTTP 200 as
`resp_19c4c2d0bdf635113bdd97014033c8ff8ea7202f011b77b3` and
`resp_9bab6f0dab854d2cae2829fc564a2fdcb2169f9d05f4fdf8`. Their shared
requirements--private write-once outputs, generation fencing, exact callback
proof, reservation accounting, accepted-publication-only visibility, and no
stale implicit carry-forward--define this checkpoint's ownership boundary.

Focused validation passes for CPU fan-out/authored planes, the immutable
execution cache, CUDA same-generation named-channel ownership, CUDA async/raw
topology transactions, and the CUDA Session authored-plane path. The raw CUDA
transaction test passes three consecutive runs. Compute Sanitizer memcheck and
initcheck report zero errors for that transaction, and Session COW ingestion is
also memcheck-clean. After a complete dependency rebuild, the CUDA suite passes
116 tests with the expected Storm surgery skip (117 total). Its parallel run
exposed one transient `benchUsdGenStorm` process failure; the benchmark passes
immediately in an isolated rerun. `git diff --check` is clean.

Still open are the CUDA source-resample and Length `BeginRaw`/`CommitPlanes`
relay phases; operator-produced named channels and binding slots on CUDA;
device execution-cache leases; cross-Session cache/coalescing and metrics;
reference-node-produced chaining, map sampling and CUDA reference upload;
general declared fan-in/merge; CUDA Graph specialization; and concrete
Metal/Vulkan allocation, synchronization and retirement adapters.

### CUDA execution-relay named-plane COW checkpoint (2026-09-13)

The previously open CurveSource-resample and Length-compaction transports are
now part of the actual CUDA execution graph. CurveSource owns a sixth permanent
relay phase: the resampler is proved by its original phase, then a host worker
starts `BeginRaw`/`FinishAsync`, and only the distinct named-topology callback
allows `CommitPlanes`. Length similarly uses phase 5 of its existing 14-phase
operator relay (the remaining phase numbers are shared only with the mutually
exclusive RBF candidate). Neither native callback calls CUDA or publishes
state; it only records its status against a permanent slot identity and wakes a
host worker.

Both paths retain old geometry and named buffers until exact callback proof.
The transform writes reservation-charged private buffers, and the worker swaps
geometry plus all named planes as one job-state transition only after successful
commit. Callback installation failure, native failure, cancellation,
supersession, device mismatch, or missing proof leaves the prior public
generation untouched. Unprovable work, its callback userdata, pinned status,
raw lifetime token, and device buffers are quarantined together, and the
workspace is poisoned rather than permitting unsafe reuse. Synchronous graph
entry points use heap callback userdata as well; a failed stream synchronization
retains it instead of returning a dangling stack address.

Raw named inputs can now carry their producer `DeviceBuffer`, causing
`BeginRaw` to validate view identity and enqueue its producer-event wait before
any transform kernel. Empty Point/Primitive planes accept the valid null/zero
representation. Compaction rejects duplicate survivor IDs in the target as well
as duplicate/missing source IDs. Named-channel revision publication and
same-generation publication share the same strict type, arity, stride, domain,
cardinality, device, read-only and name-collision validator.

CUDA source upload canonicalizes authored Point spans and Primitive records by
the same stable-ID permutation used by CurveSource before resampling or later
compaction; Groom data stays constant. Its pageable host staging is job-owned
until source H2D proof. CPU incremental capture identity now hashes the selected
CurveSet topology and complete typed authored payload, so a Recompile value edit
or plane removal cannot retain a stale `VtArray` alias. Compile-time validation
also rejects authored names which shadow standard C3 channels, including the
special `displayColor` publication lane.

The backend-neutral plan vocabulary now includes `NamedChannels`. Source writes
it, Length reads/writes it, and Publication reads the terminal version, with
explicit logical producer/value IDs. Memory estimates distinguish original and
resampled Point cardinality, retain source and output plane bytes simultaneously,
include device and pinned proof status, and charge the literal-Length runtime
reservation for both its current input planes and private survivor candidates.
This metadata is directly reusable by future Metal/Vulkan planners even though
their allocation, fence and queue adapters remain unimplemented.

The requested parallel review cycle is retained under
`/tmp/usdgen-cow-relay-review-20260913/`. Local Spark
`qwen3.8-flash-next` returned HTTP 200 as `chatcmpl-b74979807378ebe1` and
`chatcmpl-adb0f9c7c52a235f`; Hivemind `qwen3.8-27b@q4_0` returned HTTP 200 as
`resp_63fff021746f156033b5cfb7db00498c70b0be129881e42d` and
`resp_f581b68771b62bf3ebd4217a204a3ad3cefb6c487f8898d8`. Their actionable
callback-lifetime, cross-stream RAW, sync-failure, stable-ID, empty-buffer,
partial-publication, device-affinity and metadata-validation findings are
covered here. Suggestions premised on already-instantiated CUDA Graph launches
do not apply to these current stream-relay tasks and remain part of the separate
future CUDA Graph specialization work.

Focused execution-plan, source Session, Length Session, async stage/queue,
same-generation ownership, and raw topology tests pass. The Length Session test
checks Float Point and arity-2 Int Primitive survivor mapping, Groom constants,
zero-survivor cardinality, and an old named lease after later publications.
Compute Sanitizer memcheck is clean for both source-resample and Length Session;
Length initcheck is clean; the raw topology transaction reports zero racecheck
hazards. CPU fan-out tests cover backing-address COW sharing, detach-on-transform,
incremental value refresh and authored-plane removal. After a complete dependency
rebuild, the CUDA suite passes 116 tests with the expected Storm surgery skip
(117 total); no isolated rerun was needed for this coherent pass.

Still open are CUDA operator-produced named channels and declared binding slots;
device execution-cache leases; cross-Session cache/coalescing and metrics;
reference-node-produced chaining, map sampling and CUDA reference upload;
general declared fan-in/merge; CUDA Graph specialization; and concrete
Metal/Vulkan allocation, synchronization and retirement adapters.

### CUDA declared-slot COW safety gate (2026-09-13)

CUDA validation now rejects `InputPrimvars()` or `OutputPrimvars()` on every
operator whose launcher has no explicit named-binding implementation, including
topology-preserving operators. Previously only topology-changing declarations
were rejected, so a future value-only capability row could have admitted a
kernel while silently ignoring its slots. The CUDA capability matrix is version
9 to invalidate plans cached under that weaker contract.

This gate deliberately does not disguise Width, Length or Deform as named-plane
writers. None of the shipped operators declares a named slot; the production
named data currently comes from authored CurveSource planes, is transformed by
Source resample and Length compaction, and otherwise stays an immutable shared
COW input. A real slot writer must preserve the same rule: untouched planes
alias the predecessor owner, while each written name receives private candidate
storage and becomes visible only with the geometry/channel transaction after
exact completion proof. Failed, cancelled or superseded candidates publish no
partial delta and cannot mutate a retained generation.

The focused execution-plan test now registers both topology-changing and
topology-preserving undeclared binding probes and proves both fail before CUDA
allocation or submission. Generic operator-produced bindings remain open until
a real producer such as GuideInterpolate or Clump lands with fixed domain,
scalar type and arity metadata, per-lineage resolution, a private-output CUDA
launcher, memory accounting and rollback tests.

### Per-Session CUDA execution-cache COW checkpoint (2026-09-13)

The Session cooker now admits completed CUDA executions to its immutable
execution cache only from the accepted publication action. Failed, cancelled,
superseded, or subsequently dirtied candidates never become resident. CUDA
keys are created after workspace selection and include capability matrix
version 9, the actual device index, the evaluation context, and an owner-local
workspace compatibility epoch in addition to the complete plan, input, layout,
and frame tuple. Replacing a workspace clears resident entries as one COW state
publication before advancing that epoch; an outstanding lookup lease remains
valid and continues to retain its owner after the clear.

An exact CUDA hit performs no execution. It creates a fresh host generation and
a fresh device publication identity over the cached generation's same read-only
`UsdGenDeviceOwner`. Geometry topology identity remains unchanged, value
identity advances with the new publication, and no GPU buffer or named-channel
payload is copied or made writable. This is the cache's COW data contract:
immutable source, resample, compaction, point/width override, and named-plane
allocations are shared until a later operator explicitly creates a private
candidate delta. Consumer leases and final owner destruction continue through
the existing CUDA event/callback retirement service.

CUDA owners now report saturating owner-exclusive retained bytes. Source,
resample, and compaction account for their live geometry buffers plus retained
status, scan, and pinned-host proof allocations; point/width overrides and
named-channel buffers are included by the generation owner. A COW overlay does
not charge its separately owned base again. The cache uses this exact device
charge together with the small host publication/signature charge for eviction,
while the execution resource pool remains the authority for physical
Active/Pinned allocation permits. Cache admission therefore adds no duplicate
device permit.

The real-CUDA cooker test proves deferred admission, a nonzero owner charge,
accepted insertion, exact-hit fresh host/device IDs, pointer-identical COW
ownership, unchanged resident size and bytes, frame and parameter misses, stale
candidate rejection, and retirement after both publication and cache ownership
drain. The generic cache test proves `Clear()` drops resident size/bytes while
an existing lease remains readable. Enabling hits exposed a task-graph test
whose supposedly new frame had been seeded by earlier callback-gate coverage;
that test now uses an unseen tuple so it continues to validate admitted task
supersession rather than the intentional zero-execution hit path.

Focused CUDA and neutral cache/device-generation tests pass, as do the CUDA-
disabled build and its neutral cache/device tests. Compute Sanitizer memcheck
and initcheck each report zero errors for the CUDA Session path. After the test
key correction, the full CUDA suite passes 116 tests with the expected Storm
surgery skip (117 total). `git diff --check` is clean.

This closes the per-cooker CUDA execution-cache COW slice. Still open are an
explicit poisoned/device-loss notification that clears cache residency before
workspace replacement, cross-Session cache/coalescing and cache metrics, CUDA
operator-produced named channels and declared binding slots, reference-node-
produced chaining, map sampling and CUDA reference upload, general declared
fan-in/merge, CUDA Graph specialization, and concrete Metal/Vulkan allocation,
synchronization, cache-context, and retirement adapters.

### CUDA poison-to-cache invalidation checkpoint (2026-09-13)

The CUDA workspace's existing atomic poison state is now observable through a
read-only backend API. SessionCooker checks it before every synchronous or
asynchronous cache lookup and again after task-graph completion, after first
destroying the last job that borrows the workspace. A poisoned transition
atomically clears resident execution-cache state, discards any uncommitted
candidate, retires the workspace through its existing conservative quarantine
destructor, clears its description identity, and advances the compatibility
epoch. The next request must therefore allocate a new workspace and execute;
it cannot republish an older cached COW owner.

Invalidation itself is fail-closed. If publishing the empty COW cache state
fails, the poisoned workspace remains installed, the pending candidate is
discarded, and every retry re-enters invalidation; the code does not hide stale
resident entries behind a replacement epoch. Candidate admission also rejects
an installed poisoned workspace, covering candidates already handed to the
Session publication boundary. Public generations and consumer leases are not
part of workspace residency, so clearing the cache cannot mutate or prematurely
free their immutable buffers.

The CUDA stage test now proves the injected unsafe callback-install path marks
the workspace poisoned. The CUDA Session test seeds the original frame's cache,
retains its named-channel COW lease, injects a native named-topology failure,
then reissues the exact original descriptor/frame tuple. Recovery succeeds on a
different owner rather than hitting the stale cache, while the retained old
lease still reads its original values. Focused Session, staged execution, and
task-graph tests pass; the CUDA-disabled neutral build/tests pass; Compute
Sanitizer memcheck and initcheck are clean; and the full CUDA suite again passes
116 tests with the expected Storm surgery skip (117 total).

This closes invalidation for internal unproven-work poison transitions. A
platform-facing notification/classification API for CUDA device/context loss
before an unsafe transaction is observed remains open, as do equivalent Metal
and Vulkan device-loss signals. Such notification must enter this same cache
clear, candidate discard, epoch advance, and workspace retirement transition;
it must not create a separate synthetic cache path.

### Backend-neutral context-loss fence and complete cache identity checkpoint (2026-09-13)

Session now exposes synchronous and callback-safe device-context-loss entry
points using only `{backend, deviceIndex}`. The command owner assigns a
monotonic loss serial, revokes staged device edits, marks the Session dirty,
and cancels older publication; it never dereferences cooker or native workspace
state. An eager invalidation work item consumes the fence on the serialized
cooker lane, or a newer commit captures and consumes the same fence before its
first cache lookup if that eager item is superseded. This command/work split is
backend-neutral and provides the same ordering point for future Metal and
Vulkan adapters without exposing their native queues or handles.

CUDA consumes a matching fence by atomically marking its workspace lost, then
using the established poison transition to clear resident COW entries, discard
pending candidates, advance compatibility identity, and quarantine/reset the
workspace. A notification for another backend or device cannot invalidate the
active CUDA workspace. An in-flight job may reach native terminal retirement,
but pipeline epoch cancellation suppresses its publication action and therefore
its cache admission. Exact-tuple recovery then executes in a replacement
workspace rather than republishing an owner from the lost context.

Enabling accepted Session admission exposed a pre-existing logic error: the
publication action treated the dirt consumed by its own cook as if it were a
newer mutation, so Session-driven cooks never seeded the cache. Admission now
relies on the authoritative pipeline ticket—every newer input mutation calls
`CancelPending`—and retains exact generation/key validation in the cooker.
This made the real Session cache active and revealed missing semantic fields in
the plan digest. Cache identity now includes full curve topology, points/rest,
world transform, widths, binding prim/uv, stable IDs, root frames, guide blend,
authored planes and generation; and full surface topology, rest/current/motion
samples, normals, velocities, UVs, subsets, transform and generation. Node
input paths are also explicit. Consequently hierarchy rest edits, affine RBF
pose edits, curve-ID topology changes, and similar same-frame changes cannot
collide with an older cache entry.

The CUDA Session test proves a true exact hit first, backend filtering for a
Vulkan loss event, command-owner cancellation while the CUDA source callback is
held, a superseded stale publication, different owner/device storage on exact-
tuple recovery, and continued readability of the old immutable COW consumer.
The hierarchy, tools, and RBF Session regressions prove the expanded descriptor
identity. Task-graph callback tests use distinct tuples where their purpose is
to exercise native work rather than the intentional cache-hit path.

The full CUDA suite passes 116 tests with the expected Storm surgery skip (117
total). The CUDA-disabled neutral build and cache/device/async Session tests
pass. Compute Sanitizer memcheck and initcheck report zero errors for the CUDA
Session context-loss/COW path, and `git diff --check` is clean.

This closes the public Session notification and serialized CUDA invalidation
contract. Actual renderer/driver device-loss detectors still need to call the
new API, and future Metal/Vulkan owners must implement native readiness,
quarantine, allocation, synchronization, cache-context, and retirement behind
the same fence. Cross-Session cache/coalescing and metrics, operator-produced
named bindings, reference-node-produced chaining, map sampling/upload, general
declared fan-in/merge, and CUDA Graph specialization remain open.

### Cache observability and shared-domain COW requirements (2026-09-13)

Cache behavior is now observable in the publication-coherent Session stats as
resident hits, misses, successful admissions, and rejected admissions. A miss
is counted once when exact lookup fails; a usable resident republish converts
that lookup to a hit. Admission is counted only inside the accepted pipeline
publication action, after the cooker's exact generation, key, byte-charge, and
workspace checks. The immutable snapshot is refreshed after that action so the
publication and its admission result are visible atomically. CPU and CUDA
direct-cooker tests carry the previous stats baseline explicitly and prove one
miss/admission followed by one exact hit; stale and deliberately mischarged
candidates increment rejection without altering residency.

The COW rule is authoritative for both the current per-Session cache and any
future shared cache domain. A hit must create a distinct
`UsdGenGeneration` and distinct `UsdGenDeviceGeneration` publication wrapper,
then share only the cached immutable `UsdGenDeviceOwner` and its read-only
geometry/named-channel allocations. It must never hand Session B Session A's
generation object. Existing CUDA tests prove fresh wrapper identities with the
same owner on a hit, private replacement storage after context loss or edited
input, and continued readability through an old consumer lease. Cache clear
publishes empty residency without mutating leased COW payloads.

A cross-Session audit found that simply making the current cooker store global
would be unsafe. Its compatibility epoch and context-loss fence are currently
Session-local, and its exclusive overlay byte charge assumes the reachable COW
base remains owned elsewhere. A correct shared domain therefore needs a stable
native context identity scoped by backend and logical device, one atomic
compatibility epoch shared by every participating Session, and ownership-aware
accounting that pins each reachable immutable base exactly once. Context loss
must invalidate matching resident and in-flight work for the entire domain.

The smallest safe cross-Session slice is shared completed-result storage with
optimistic duplicate insertion: concurrent executions may finish, but the
first accepted immutable artifact wins and later exact candidates coalesce at
insertion without replacing it. Blocking worker futures are forbidden by the
runtime contract. True in-flight coalescing comes later through nonblocking
waiter registration and must preserve accepted-publication-only admission.
`executionCacheCoalesced` is reserved and remains zero until that mechanism is
implemented; it is not presented as a functioning metric. The eventual metric
must count joins rather than ordinary resident hits or duplicate attempts.

Focused neutral and real-CUDA cache tests pass with these counters and COW
ownership assertions. The complete CUDA build passes all 116 runnable tests
with the expected Storm surgery skip, the CUDA-disabled neutral cache/device/
async Session tests pass, and Compute Sanitizer memcheck and initcheck both
report zero Session-path errors. Cross-Session storage and nonblocking
in-flight coalescing remain open behind the stricter domain requirements above.

### Shared completed-result domain and reference-source COW checkpoint (2026-09-13)

The completed-result cache can now be explicitly shared by otherwise private
Sessions. `UsdGenExecutionCacheDomain` is keyed by backend, logical device and
an adapter-owned stable context identity; it owns the immutable COW store, a
monotonic context epoch and a cross-Session device-publication watermark.
Default Sessions still construct private domains, so sharing is opt-in and
does not silently change existing ownership scope. The original two-argument
Session and cooker constructors remain real overloads for binary compatibility;
the domain-aware overload is additive. Public resident-size/byte queries use
atomic `shared_ptr` loads while a default cooker may atomically replace its
private CPU/CUDA domain.

Domain lookup and insertion serialize the epoch comparison with store access.
Invalidation increments the epoch before publishing an empty store, and a
failed COW clear leaves the domain fail-closed until a retry succeeds. A stale
candidate therefore cannot race the clear and repopulate old-context
residency. Each cooker observes the shared epoch before its next CUDA use and
quarantines only its private workspace when another Session advanced it.
Internal CUDA poison and matching external context-loss fences invalidate the
shared domain before resetting the reporting cooker's workspace. Backend and
device mismatches remain isolated; a nonzero explicit context identity is
preserved rather than rewritten by the cooker.

Cross-Session hits share data, not publication objects. The receiving cooker
copies the immutable host generation, allocates a fresh host publication and
uses `Republish()` to allocate a distinct device wrapper over the same
read-only `UsdGenDeviceOwner`. A domain-wide watermark satisfies the device
wrapper's strictly-newer identity rule even when the receiving Session has no
local history. CPU tests likewise prove distinct generation objects whose
`VtArray` storage remains COW-identical. CUDA tests prove distinct host/device
wrappers, pointer-identical immutable owners, a retained consumer geometry
lease readable across domain invalidation, stale-candidate rejection, and a
replacement owner/allocation on exact-tuple recovery.

Cache byte accounting is now deliberately conservative for COW chains.
`ExclusiveRetainedBytes()` remains the exact delta used by the physical
execution-resource permit ledger. The new backend-neutral
`InclusiveRetainedBytes()` is the cache-admission view; CUDA revision owners
recursively include immutable predecessors they retain. This supersedes the
earlier per-cooker assumption that every base necessarily has a separate
resident charge. Shared bases may be counted more than once, causing earlier
eviction, but a derived resident entry can no longer under-account physical
memory after its base record disappears.

This slice shares completed artifacts only. Nonblocking in-flight miss
coalescing remains open, so concurrent first misses may execute independently
and coalesce only when their completed immutable candidates insert. A context
epoch is rechecked after a hit is wrapped and stale admission is rejected, but
a fully coordinated physical device-loss broadcast that cancels another
Session's already-running native job/publication still requires domain
subscriber or publication-token fencing. Adapters must not destroy a native
context merely because one Session recorded the logical fence. CUDA
`contextIdentity == 0` remains the trusted primary-context convention until a
native context provider supplies a verifiable identity; Metal/Vulkan adapters
must validate their own nonzero logical-device identities. Accordingly
`executionCacheCoalesced` remains reserved at zero.

CPU reference-node-produced chaining is no longer metadata-only. The new
`UsdGenReferenceSource` is a zero-geometry-input Reference-role generator with
exactly one resolved reference slot. Capture installs the compiled immutable
reference buffer by COW-sharing its planar arrays; the scheduler gives the
node a private topology publication while downstream writers continue through
the existing plane-granular COW preparation. Reference materialization now
preserves ragged offsets, generated `hairT`, stable IDs, skin prim/UV, root
frame axes and typed named point/primitive planes. Role-aware slot validation
rejects a missing or unresolved source before replacing the last-good graph.

The CPU reference regression runs `ReferenceSource -> Width` despite reverse
namespace order and proves actual topology, points, IDs, roots, named planes,
width output, immutable old-reference readability after a generation edit,
and last-good output after a rejected recompile. CUDA reference transport
remains fail-closed: the next device slice is a reference-source upload owner
feeding one topology-preserving consumer with no map, resample, compaction or
fan-in. General reference DAGs and 2D map payload/sampling remain open.

After a full reconfigure for the new operator source, the coherent CUDA build
passes all 116 runnable tests with the expected Storm surgery skip. The
CUDA-disabled build passes the focused reference, cache, device-generation and
async Session tests. Compute Sanitizer memcheck and initcheck both report zero
errors for the expanded CUDA Session test, including shared-domain reuse,
inclusive COW accounting and retained-lease recovery. `git diff --check` is
clean.

### CUDA reference-source Width/COW checkpoint (2026-09-13)

CUDA capability matrix version 11 adds exact, deliberately narrow
`UsdGenReferenceSource -> Width+` and
`UsdGenReferenceSource -> Length -> Width+` lowerings. Compile-time layout, memory
estimation and runtime preparation resolve the source through the node's
single `references` relationship rather than pretending it is a normal
`curves` relationship. The immutable reference descriptor arrays remain COW
values on the host; source preparation reads them into H2D staging without
mutating them, and the existing `CudaCurveSource` owner supplies the device
topology, points, generated `hairT`, stable IDs and optional complete root
prim/UV binding. This is not claimed as zero-copy across host and device
memory spaces.

Every Width consumer reads the immutable source geometry and allocates only
its private writable width result. Publication and exact cache hits retain the
device owner through the established COW lease contract: a cache hit creates
fresh host and device publication wrappers while sharing the immutable owner,
and an edited reference generation produces a new owner without changing a
previous consumer lease. Inclusive cache accounting continues to charge any
reachable COW predecessor, while execution-resource accounting remains the
exclusive allocation delta.

The accepted shapes require one or more Width consumers, with at most one
direct Length topology trunk between the source and those Widths. Source-only
graphs, Length-only graphs, multiple or misplaced Lengths, Deform, maps,
multiple references, fan-in, resampling parameters and named reference planes
fail before device work. `rootFrame` and `guideBlend`
also fail closed because this slice has no device publication channels for
them; accepting and silently dropping either would violate the reference
contract. Geometry payload upload is H2D-only, although compact status and
tile/bounds proof readbacks remain part of the established CUDA transaction.

Focused real-CUDA tests prove plan dependencies, points, topology, stable IDs,
root bindings, Width values, fresh-wrapper/same-owner cache reuse, a
reference-generation cache miss, and readability of the retained old lease.
They also cover the fail-closed unsupported forms and Width-DAG COW ownership.
General reference DAGs, reference named-plane/root-frame/guide-blend transport,
2D map upload/sampling and in-flight cross-Session coalescing remain open.

The post-change coherent CUDA build passes all 116 runnable tests with the
expected Storm surgery skip. The CUDA-disabled reference/cache/device/async
tests pass, Compute Sanitizer memcheck and initcheck report zero errors for the
expanded CUDA Session path, and `git diff --check` is clean.

### Shared COW publication fence and owner sequencing checkpoint (2026-09-13)

The completed-result domain now issues an epoch-bound publication fence for an
exact backend/device/cache key. CPU and CUDA cache hits and freshly cooked
results capture this fence, and the Session publishes its immutable snapshot
only through `PublishIfCurrent`. Domain invalidation and that final local
publication action share one serialization point, so a result from an old
context epoch cannot become visible after invalidation. CPU cache keys now use
the domain's actual epoch as well; they no longer assume epoch zero.

The execution pipeline keeps its single-writer work lane occupied through the
accepted publication and terminal callback. A pending successor starts only
after that handoff completes, preventing it from mutating the same cooker while
the prior result is still admitting or publishing COW state. Deterministic
tests hold publication open, queue a successor, and prove the successor cannot
enter early. Shared-domain tests likewise prove stale fences reject publication
while current fences admit it without mutating retained leases.

Context-loss notifications are retained as an ordered batch. Only identical
backend/device targets coalesce (latest wins); distinct targets survive bounded
ingress pressure. Replayed batch prefixes are idempotent on the serialized
cooker, while a matching CUDA loss invalidates resident cache state, advances
the domain epoch, and replaces the private workspace. Published and externally
leased generations remain immutable COW data throughout recovery. This is
Session delivery, not a process-global device-loss registry: a notification for
an inactive domain does not discover or invalidate unrelated domains.

The domain intentionally runs only a bounded, allocation-free publication
exchange while holding its serialization mutex. Native GPU work and waits are
forbidden there. Store transactions return an opaque deferred-retirement holder
to the domain, keeping replaced immutable states and rejected candidate payloads
alive until the outer epoch mutex has also been released. Generation and Session
snapshot candidates are likewise allocated before entering the fence; the
exchange returns its displaced immutable owner and destroys it after unlocking.
The generic fence API enforces a non-void retirement result at compile time.
Eviction, invalidation, and fenced-generation tests use a native owner whose
destructor re-enters domain lookup; all complete deterministically, proving
native/COW owner destruction is outside both cache locks.

### CUDA reference Length/COW and proof-or-quarantine checkpoint (2026-09-13)

The version-11 CUDA reference path now admits exactly one direct Length node
before one or more Width leaves. Literal Length planning resolves the immutable
reference relationship, accounts source/root buffers and peak private topology
storage, and reserves memory before device execution. Runtime tests prove the
actual compacted topology and points, preserved complete root bindings, Width
fan-out, nonzero pending admission, changed reference-generation misses, and
that the old device lease remains readable after the replacement owner is
published. This remains COW by ownership: reference host arrays are never
mutated, every topology-changing result is private, and published cache hits
share only immutable owners.

CUDA Width and Length now apply a proof-or-quarantine lifetime rule to every
submitted upload/kernel path. Failed device selection, event/stream completion,
readback, or publication proof marks work unproven, poisons the workspace, and
quarantines buffers and their resource charges instead of freeing possibly
borrowed storage. Raw named-channel resample/compaction paths extend that rule
to the source, topology candidate, compactor, and all borrowed COW plane owners.
RBF upload/finalization follows the same transaction boundary. Proven semantic
errors and pre-submit allocation errors still release normally, preserving
resource balance and allowing safe retries.

### Portable backend factory contract checkpoint (2026-09-13)

Backend selection now has stable explicit CPU, CUDA, Invalid, Metal and Vulkan
values, canonical parsing/names, device-cache identity construction,
availability, and capability-version reporting behind a native-header-free
contract. CUDA reports capability matrix version 11 in enabled and disabled
builds. Metal and Vulkan are explicit unavailable factories with distinct
cache identities and precise compiler diagnostics; neither can silently fall
back to CPU or leave a partially compiled graph. Concrete Metal/Vulkan
allocation, command submission, synchronization, loss handling and retirement
adapters remain open.

The coherent CUDA build passes all 117 runnable tests with the expected Storm
surgery skip. The focused CUDA-disabled backend, pipeline, cache, async Session
and CPU reference/map tests pass. Compute Sanitizer memcheck and initcheck report
zero errors for the expanded CUDA Session path, and memcheck also reports zero
errors for the reference Width DAG and named-channel topology paths.

### Backend-neutral map binding transport checkpoint (2026-09-13)

`UsdGenNodeDesc` now carries canonical purpose-aware map bindings alongside the
legacy flat `maps` array. Each binding retains its target, typed purpose
(`MaskSource`, `LengthSource`, or compatibility-only `Generic`) and the exact
authored relationship token when one exists. Builders continue to fill `maps`
for old direct consumers, but compiler lowering preserves every typed slot, so
one map may legitimately serve both mask and length roles without being
collapsed. Maps-only descriptors lower only to `Generic` with no inferred
semantic meaning; a future sampler must reject that form unless it explicitly
supports a generic slot.

Both descriptor builders now resolve the schema-defined
`usdGen:map:file` asset (rather than the obsolete `usdGen:source` spelling),
carrying its resolver result or authored fallback as a stage-free value. The
compiler validates typed/legacy agreement, target presence, duplicate typed
slots, and relationship-token/purpose consistency before moving an incremental
graph. Map-binding purpose and token participate in structural and session-cache
identity. Rejected ambiguous descriptors retain the last good graph and its
immutable map values.

Focused validation passed with CUDA disabled (`testUsdGenCpuReferenceMap` and
`testUsdGenHydraParity` in separate engine/imaging builds) and CUDA enabled
(`testUsdGenCpuReferenceMap`). The CPU test covers deterministic map identity,
same-map multi-purpose slot retention, ambiguity rejection, and last-good COW
preservation; the Hydra fixture proves Stage/Hydra parity for authored mask and
length relationships plus the resolved asset path. No CUDA map upload or
sampling is implemented by this checkpoint; CUDA remains fail-closed for map
inputs.

### Immutable image payload and CUDA sampler foundation (2026-09-13)

The backend-neutral image primitive stores one immutable shared/COW float
payload with explicit dimensions, channel count, and row orientation. CPU
sampling defines the oracle for nearest and bilinear filtering, clamp/repeat/
mirror/black addressing, scalar channel or luminance selection, scale/offset,
output clamping, and default values. Construction rejects non-finite texels,
invalid channel access, signed-index overflow, and host size/stride overflow;
copying the payload preserves the same immutable storage identity.

The CUDA sampler uploads into a private device allocation and matches the CPU
oracle. Same-size reuploads never overwrite storage that another stream may
still read: the prior device allocation and its immutable pageable host source
retire together only after a non-blocking event query proves completion.
Grid-stride execution covers batches beyond the CUDA grid cap without launch-
dimension arithmetic overflow. Candidate keepers are allocated before
submission, so a failed copy, event, wait, launch, or completion proof can
quarantine both device storage and the host source without allocating in the
failure path. The sampler object is deliberately non-copyable and non-movable
to preserve that retirement transaction.

CPU/CUDA parity, cross-stream replacement, oversized-batch tail coverage,
normal resource-pool return, and boundary validation pass. Compute Sanitizer
memcheck and initcheck report zero errors, and the coherent CUDA suite passes
all 119 runnable tests with the expected Storm surgery skip. This checkpoint is
a standalone sampling foundation: Hio asset decoding and graph wiring remain
open. The next honest slice is a root-domain `ImageMap -> Width` path using the
typed binding contract, followed by a CUDA capability-matrix version bump only
when that graph path is executable.

### Typed ImageMap-to-Width COW execution checkpoint (2026-09-13)

The first image-backed graph path is now executable on both the CPU reference
lane and CUDA. `UsdGenMapDesc` carries a shared immutable decoded image
payload; compiled plans retain that COW value directly and never borrow mutable
descriptor storage. Width admits exactly one typed `MaskSource` targeting a
valid `UsdGenImageMap`, with root-domain `st` UVs, raw color space, a scalar
channel, nearest/bilinear filtering, and clamp/repeat/mirror/black addressing.
Legacy generic bindings and missing payloads fail before device submission.
Both ordinary and reference sources require exactly one root UV per curve.

CPU capture samples the immutable payload once per curve and stores the result
in the capture-owned curve mask. The CUDA plan stores the same immutable
payload/options, declares an external read of the new `ImageMaps` execution
data kind, and charges both immutable texels and the Width-private sampled-mask
buffer. Each Width branch owns its own `CudaImage` upload and sampled-mask
buffer; no sibling can overwrite shared texture state. The async relay retains
those owners through the terminal callback, while synchronous and asynchronous
failure paths apply the existing proof-or-quarantine rule before releasing any
possibly borrowed storage. Equal `map:clamp` components disable map-level
clamping, after which the canonical default mask remap still clamps the sampled
source to `[0,1]` before `mask:amount`.

The CUDA capability matrix is version 12 and its Width row is version 3. Tests
prove plan resource metadata and exact scratch accounting, CPU root-UV sampling,
CUDA execution, invalid-payload rejection, and replacement COW behavior. An old
compiled plan continues to sample its original texels after a new descriptor
increments `textureGeneration` and publishes a replacement payload. The async
Session test additionally retains the old device generation while the new
generation reads the replacement pixels, proving both old host data and old
device widths remain readable and unchanged.

After a coherent rebuild, all 120 registered CUDA tests pass (119 runnable and
the expected Storm surgery skip). The focused CUDA-disabled backend, cache,
pipeline, and CPU reference/map tests pass. Compute Sanitizer memcheck and
initcheck report zero errors for both the mapped Width DAG and the expanded
async Session path (`/tmp/usdgen-image-width-{dag,session}-{memcheck,initcheck}.log`).
`git diff --check` is clean.

This checkpoint accepts already-decoded direct descriptors. Stage and Hydra
builders still carry the resolved asset path without decoding it, so authored
USD image maps intentionally fail closed until an Hio decode/cache layer can
publish immutable payload generations. Ptex, non-root/per-CV domains,
non-raw color transforms, map composition, the remaining mask terms, and
concrete Metal/Vulkan execution adapters also remain open.

### Authored ImageMap COW generation checkpoint (2026-09-13)

Stage and Hydra descriptor construction now resolve authored `UsdGenImageMap`
assets through one shared Hio-backed cache. A successful decode is normalized
into the backend-neutral immutable float payload and stamped with the cache's
monotonic texture generation before the descriptor reaches compilation. Both
builders therefore share the exact same payload owner for the same asset and
generation, while missing or unsupported images add a deterministic validation
error and fail closed. The decoder currently accepts native, uncompressed Hio
images with one through four unsigned-byte, half-float, or float components;
Ptex, compressed/block formats, and non-raw color transforms remain open.

The cache itself is copy-on-write. Readers retain an immutable table snapshot;
new keys allocate and copy outside the cache mutex, then publish only if the
generation and source table still match. Same-key concurrent misses share one
loading entry and one immutable decoded payload. Invalidation atomically
advances the generation and swaps in a preallocated empty table, with the
retired table and all payload owners destroyed after unlocking. A decoder that
crosses invalidation completes for existing waiters but cannot publish stale
pixels as current. Tests retain an old payload across invalidation, prove its
pixels remain unchanged, coalesce concurrent same-key decoding, and repeatedly
race two distinct COW table extensions to prove neither cache entry is lost.

Imaging sessions retain their last immutable descriptor value. Every incoming
descriptor is re-resolved on the session's serialized owner before submission,
so a descriptor captured before invalidation cannot enter as generation zero or
stale data. `ReloadMaps()` now invalidates the shared cache and stages a fresh
descriptor generation for each live session in owner order; a session without
a retained descriptor explicitly requests a new one. A later commit publishes
the replacement while already-published CPU/CUDA plans continue owning and
reading the prior payload generation unchanged.

The Stage decode, Hydra shared-owner parity, async session reload, session-store
ordering, and concurrent cache tests pass in both CUDA-enabled and CUDA-disabled
builds. The existing typed `ImageMap -> Width` CPU/CUDA tests remain the
execution proof for these immutable payloads. `git diff --check` is clean.

The next structural execution-graph slice is explicit binary Width fan-in: two
ordered source-rooted Width values with identical non-width immutable data,
producing a fresh width-only COW plane on CPU and CUDA. General merge semantics,
per-asset targeted invalidation, additional image formats, and concrete
Metal/Vulkan adapters remain deliberately separate work.

### Explicit binary WidthBlend COW fan-in checkpoint (2026-09-13)

Direct graph descriptors now admit one deliberately narrow binary value merge,
`UsdGenWidthBlend`. It consumes exactly two distinct ordered, source-rooted
Width values and uses the existing `usdGen:blend` field as `t`: exact endpoint
copies at zero and one, and `left + (right - left) * t` otherwise. The operator
requires algorithm version zero, `enabled=true`, finite `t` in `[0,1]`, and no
generic parameters, ramps, expressions, maps, references, curves, surfaces, or
mode. These rules are enforced before incremental compiler state can move and
again at backend admission where applicable. Authored operand order survives
Kahn sorting and incremental recompilation.

Compatibility is semantic rather than pointer- or generation-based. Both
inputs must have equal non-width curve data and topology: points, root data,
IDs, masks, offsets, chunk layout, and named planes. Width arrays and value
versions may differ. CPU evaluation aliases the immutable non-width data from
the left input but materializes a fresh width `VtArray`; it never mutates either
predecessor. Tests prove the output width storage differs from both inputs,
unchanged planes remain shared COW data, exact endpoints hold, and swapping the
ordered operands changes the result.

CUDA capability matrix version 13 adds WidthBlend capability version 1. The
plan exposes `SourceRootedValueDag`, distinct from the unary-DAG shape, and
lowers two logical Width reads plus one fresh COW Width write. Static estimates
charge the private output plane and both retained predecessor planes. Execution
waits on both producer events on the destination branch stream and retains both
owners through output-use recording and terminal publication. Synchronous and
asynchronous failures extend proof-or-quarantine to the two inputs and fresh
output, so potentially borrowed device storage is never reclaimed without a
completion proof.

Focused CPU and CUDA plan/DAG tests pass in CUDA-enabled builds, and the CPU COW
test passes in a CUDA-disabled build. After a coherent full rebuild, all 121
registered CUDA tests pass (120 runnable and the expected Storm surgery skip).
Compute Sanitizer memcheck and initcheck report zero errors for the CUDA Width
DAG. Stage/Hydra-authored WidthBlend is not claimed by this checkpoint:
plan 14's newer binding overlay makes composed hierarchy authoritative and
explicitly says authored `usdGen:input`/`usdGen:terminal` are neither required
nor authoritative. The merge is therefore a compiler-owned/direct-descriptor
execution primitive; exposing binary authoring would require a separate
user-level revision of that binding contract, not an inferred builder change.
General merge operators and concrete Metal/Vulkan execution adapters remain
open.

### Dependency-ready COW scheduling and bounded CUDA Graph checkpoint (2026-09-13)

The CPU scheduler now advances each reference or curve lane through stable,
dense-ID dependency-ready frontiers rather than evaluating every node as one
serial topological chain. All captures in a frontier still run serially on the
commit thread, including every graph-wide repartition. Only after that layout
has settled does the commit thread prepare each output: planes written by an
operator are materialized as private COW arrays, while untouched planes remain
immutable aliases of the completed predecessor. One flattened node/chunk
dispatch may then overlap independent ready branches. Dirty-byte clearing,
value-version changes, tile aggregation, diagnostics, and node statistics are
committed serially in dense order. The existing single-node sparse fast path is
retained. A held two-branch fan-out test proves both branches enter evaluation
concurrently, their writable Width storage is distinct from the source and
from each other, and their untouched point data still shares the immutable
source storage.

CUDA Graph legality is now exercised at the smallest honest boundary: one
preallocated `LaunchWidthBlend` leaf. The test-local graph owner retains only
the native graph and executable handles; the caller retains both immutable
input `DeviceBuffer` owners and the fresh output owner, records output use
after every launch, and proves completion before graph destruction. Capture
and replay match the uncaptured kernel for exact blend endpoints, swapped
ordered operands, and launch-grid boundary counts. Empty work is rejected
before capture begins and does not poison a subsequent legal capture on the
same stream. This is not yet a reusable execution-plan graph cache: current
buffers are per-job, so a pointer-bound executable has no useful cross-job
identity, and CUDA exposes no exact portable native graph-memory query. Stable
workspace slots, exclusive in-flight leases, conservative cache charging, and
proof-or-quarantine eviction remain required before runtime graph caching can
be enabled.

The backend-neutral completed-result cache also has a nonblocking exact-key
coalescing primitive. One registration becomes leader; followers attach
without waiting on the domain mutex. Successful resolution inserts one
immutable generation and gives every recipient a distinct lease over the same
COW snapshot/payload owner. Failure, cancellation, duplicate resolution, and
epoch invalidation cannot seed the cache; stale callbacks are drained outside
the domain lock, and retained leases keep old data readable. Runtime Session
integration remains open. Registration currently occurs inside cooker work,
but a follower must complete through its own serialized pipeline owner and
publication fence; directly publishing from the domain callback would violate
that ownership boundary. The required Session/pipeline continuation mechanism
is a separate structural slice, not part of this checkpoint.

Focused CPU fan-out, execution-cache, CUDA Width, and imaging-library builds
pass, and `git diff --check` is clean. A coherent 121-test run passed every
functional test and the expected Storm-surgery skip; its only initial failure
was the E-6 wall-clock gate at 0.208 ms against 0.200 ms, which passed on the
immediate isolated rerun. General merge operators, reusable CUDA Graph caching,
Session-level in-flight coalescing, and concrete Metal/Vulkan execution
adapters remain open.

### Owner-safe COW coalescing, runtime WidthBlend graphs, and portable factory checkpoint (2026-09-13)

Exact-key in-flight coalescing is now integrated with both CPU and CUDA
Sessions. The cooker registers only after preparation has produced the full
execution key. One Session remains the executing leader; exact followers park
without occupying a work lane or launching duplicate graph work. Resolution
passes only an immutable cache lease and publication fence into each
follower's own serialized pipeline continuation. CPU followers receive
distinct `UsdGenGeneration` wrappers whose `VtArray` planes share the same
immutable COW storage. CUDA followers receive distinct device-generation
wrappers produced by `Republish`, while their immutable native owner remains
shared. Cancellation, supersession, shutdown, stale epochs, and rejected
leader publication cannot seed residency. Leader registration is deliberately
silent: invalidating the leader notifies followers but cannot settle the
leader's pipeline before its actual backend terminal proof and release its
workspace early.

The WidthBlend leaf now has a reusable workspace-private CUDA Graph cache. Its
pointer-bound executable owns stable private left, right, and result slots;
each job copies both immutable inputs into those slots, launches the captured
blend, then copies into a newly allocated publication output. Thus replay does
not turn the graph cache into mutable published storage: every generation
still has a fresh COW Width plane, and retained generations remain readable
after the graph workspace is destroyed. One exclusive in-flight lease prevents
slot mutation; a busy, disabled, unready, or recoverably failed entry takes
the ordinary fresh-output launch path without waiting. The exact key includes
ordered operands, count, blend bits, algorithm version, and launch geometry.
Idle key changes evict and recapture. Resource accounting charges the three
exact slot allocations plus a conservative 64 KiB Cache permit for native
graph state. Pre-submit capture/allocation rejection falls back cleanly;
post-submit loss of terminal proof quarantines all possibly borrowed owners
and retained charges.

A backend-neutral adapter boundary now separates immutable compilation from
owner-private mutable execution. `UsdGenExecutionPlanHandle` carries neutral
metadata and an opaque immutable backend payload. A registered adapter
compiles that handle and creates one `UsdGenExecutionBackendExecutor` per
Session/command owner; only the executor may retain a workspace, device
context, previous generation, reservations, and cancellation/completion
state. The neutral implementation imports no native SDK types. A small CUDA
provider translation unit supplies the current capability version without
leaking CUDA headers into the interface. CPU and CUDA still execute through
their existing Session-owned paths during migration. Metal and Vulkan have
stable identities and precise fail-closed unavailable contracts, but no
concrete execution adapters are claimed yet.

Validation covers the ownership contract directly. Concurrent CPU Sessions
produce distinct wrappers over shared immutable `VtArray` storage; concurrent
CUDA Sessions produce distinct wrappers over one immutable native owner;
superseded and shutdown followers settle without publication. CUDA graph
capture, replay, key eviction, accounting, recoverable capture rejection, and
clean retirement all pass, including retained COW reads after workspace
destruction. The named-channel COW fixture now records its producer-use event
before publication; without that proof its synthetic plane violated the same
consumer-wait contract. It passes 20 consecutive runs after correction.
Focused CUDA Session and Width DAG tests each pass five consecutive runs;
CUDA-enabled and CUDA-disabled backend/cache/Session tests pass; Compute
Sanitizer memcheck and initcheck report zero errors for both CUDA Session and
Width DAG. A coherent CUDA-enabled run passes all 122 registered tests (121
runnable plus the expected Storm-surgery skip). `git diff --check` is clean.
General merge operators, migration of CPU/CUDA Session execution through the
new factory, and concrete Metal/Vulkan executors remain open.

### Session-owned backend routing and CUDA Noise COW checkpoint (2026-09-13)

Session execution now enters through one private, owner-lifetime backend
executor instead of constructing unrelated CPU/CUDA routing state for each
request. The executor owns one `UsdGenSessionCooker` for the Session lifetime
and selects CPU or CUDA from each immutable descriptor, so switching the
descriptor backend does not discard the cooker's last-good generation,
completed cache, in-flight coalescing domain, cancellation state, or COW
owners. Work closures, follower continuations, publication actions, terminal
replies, and context-loss handling all retain the same executor. Destruction
cancels controls, shuts down the serialized pipeline, shuts down the executor,
and only then releases owner state. Metal and Vulkan remain precise
fail-closed routes. This is deliberately a private bridge: the public neutral
adapter request still lacks Session dirty-state, host-generation, cache, and
publication semantics and is not falsely registered as a production Session
executor.

CUDA capability-matrix version 14 adds executable `UsdGenNoise`. The plan
lowers its evaluated groom/primitive/point fields, magnitude and mask LUTs,
immutable authored rest-root frames, private staging, pinned scalar status,
and a fresh Active point plane. Source root frames are canonicalized with the
source stable IDs and retained as immutable COW inputs. After an upstream
Length cull or reorder, both validation and the Noise kernel resolve the
surviving frame by binary stable-ID lookup; they never reinterpret the reduced
curve index as a source-frame index. The current lowering accepts the authored
default `auto` and explicit `rest` space with final sampling. Explicit
`deformed` space fails closed until motion-tail/deformed-root-frame lowering is
implemented.

`CudaNoise::ApplyFresh` computes only into private staging. Its nonblocking
finish copies into the candidate point plane and reads one device status into
charged pinned storage before invoking the permanent relay callback. Only a
proved callback may commit the candidate into job state; the previously
accepted points and every retained device generation remain untouched.
Allocation and semantic rejection before submission release normally.
Partially submitted uploads first attempt a stream completion proof. Only lost
proof quarantines device storage, abandons the pinned permit with its leaked
allocation charge intact, and poisons the workspace. Thus resource accounting
never reports quarantined bytes as reusable, while recoverable validation or
capacity failures do not exhaust future reservations.

The low-level Noise oracle now checks fresh-publication COW directly: the
candidate output retains its sentinel until `FinishFreshAsync`, the accepted
input remains byte-identical, and ordinary Active/Scratch/Pinned charges return
to baseline after producer destruction. The Session oracle keeps earlier
device generations leased while later Noise revisions publish, proves that
editing a culled curve's frame cannot affect survivors, proves that editing a
survivor's own frame does affect only that stable ID, exercises runtime groom,
primitive, and point expressions including `noise:seed`, and verifies
disabled/blend-zero identity plus last-good preservation for malformed
bindings and unsupported deformed space. It passes ten consecutive runs.

A coherent CUDA build passes all 123 registered tests (122 runnable plus the
expected Storm-surgery skip). The focused Noise/Session/resource/task-graph
matrix passes 9/9, and the CUDA-disabled backend/Session/CPU fan-out matrix
passes 4/4. Compute Sanitizer memcheck and initcheck each report zero errors
for both the low-level Noise fresh protocol and the Session Noise graph.
`git diff --check` is clean.

The next graph-completeness slice is CUDA Scatter followed by Grow, including
topology COW publication and named-plane/root-binding transport. General merge
semantics still require an explicit ABI rather than inferred reducer behavior.
Migration of the complete public backend-adapter request, and concrete native
Metal/Vulkan executors, remain open cross-platform work.

### Scatter/Grow CPU topology-COW prerequisite (2026-09-13)

The CPU reference path now defines the data-plane ownership rule needed by the
CUDA Scatter/Grow slice. Grow validates `curveCount * segments` before the
`uint32_t` multiplication, resamples every supported named vertex plane into
the new CV cardinality, and gives uniform and constant planes fresh copied
owners. The scheduler recognizes that an owning Grow capture has already
performed this topology transform and therefore does not overwrite those
owners with generic upstream pass-through descriptors. A retained or mutated
Grow result consequently cannot alter the source generation's named data.

Scatter now rejects negative or non-finite density during binding/capture and
rejects per-face or aggregate root counts beyond `uint32_t` before emission or
allocation. Focused COW and validation oracles cover float linear vertex
resampling, integer nearest resampling, uniform/constant copying, distinct
owners, mutation isolation, and negative, NaN, and oversized density. The
`testUsdGenCpuFanout`, `testUsdGenAsyncSession`, and `testUsdGenWidth` tests pass
in both the CUDA-enabled and CUDA-disabled builds (six focused passes total),
and `git diff --check` remains clean.

This establishes the normative CUDA requirement: generated topology must
publish geometry, topology, root bindings, root frames, built-in channels,
and supported named channels as one fresh COW generation. A CUDA implementation
must not narrow the contract by silently dropping named data; unsupported plane
types or interpolation must fail closed before publication.

### Scatter/Grow CUDA producer and generation ownership (2026-09-13)

`CudaScatterGrow` now expands an immutable captured root payload into private
CUDA points, rest points, widths, hairT, offsets, stable IDs, root face/UV
bindings, and T/B/N frame planes. Its pending result remains invisible until
the caller observes successful native callback completion and commits it.
Both populated and empty committed owners reject reuse. Empty capture produces
zero curves and points with offsets `[0]`. The literal controls validate CV
count `[2,64]`, finite nonnegative lengths/random multipliers, and direction;
reversed random endpoints are canonicalized to the CPU convention. Stream
device and capture checks precede allocation and submission.

Every device input/output allocation consumes the supplied reservation. On
lost completion proof, all potentially borrowed device input and output
storage is quarantined. On proved commit, temporary root uploads are released;
published output planes transition to Pinned. The dedicated generation maker
transfers the completed producer into the common immutable CUDA owner and
consumer-retirement protocol. Geometry leases expose optional rest-frame
T/B/N views, and point revisions retain those frames through their base owner.

Root validation of the new publication test proves that an old lease remains
readable after its original wrapper is dropped and another generation is
published, that fresh point storage has a distinct address, that root frames
survive publication/revision, and that ordinary retirement returns resource
usage to baseline. The low-level test compares random lengths against the
authoritative host `UsdGenDraw01`/`kSaltGrow`. A separate preflight test checks
invalid-control and capture rejection without allocation, reversed random
endpoints, and immutable empty owners. Both new tests and the existing CUDA
Session and named-channel tests pass (4/4); Compute Sanitizer memcheck and
initcheck each report zero errors for the producer/publication test.

This is a producer/publication checkpoint, not completed graph support. The
strict layout skeleton identifies Scatter followed by Grow, but the CUDA
capability matrix remains version 14 and does not advertise either operator.
Authoritative Scatter capture in plan preparation, exact graph estimates,
asynchronous Session operator dispatch, named-plane topology transport, and
graph-level parity/failure tests remain to be connected. The low-level lift
formula still follows the existing CPU implementation; authored lift/uvBlend
must not be enabled in graph admission before resolving the documented
direction-rotation semantics. Native Metal/Vulkan execution also remains open.

### Authoritative Scatter handoff and capture-order tiles (2026-09-13)

`PrepareCudaScatterInput` now resolves a static random Scatter node and its
rest surface, validates face/index/UV cardinalities and native parameter
types, then invokes the registered CPU Scatter Bind/Capture implementation.
The resulting immutable root payload preserves every position, stable ID,
root face/UV and T/B/N frame in the CPU's exact Morton order. Invalid input
does not replace an earlier payload. Tests compare all components against
direct authoritative capture and cover zero density, malformed density type,
truncated indices, non-finite UVs, duplicate subset faces, and unsupported maps.

The handoff explicitly rejects unsupported authored mask effects. Inspection
of `EvaluateMask` showed that the current CPU M1 implementation only evaluates
its random term, while Scatter drops its unsupported-term diagnostics. Thus
non-neutral amount, inversion, combine, range and ramp settings must not be
admitted by CUDA under a claim of authoritative parity. Their full semantics
remain open work; a zero density is supported, but `mask:amount=0` is currently
rejected rather than silently producing nonzero roots. True flip also remains
rejected pending reconciliation of CPU frame handedness with the schema.

`CurveTileOrder::IdentityCaptureOrder` adds a narrowly checked tile path for
unchanged capture order: capture and output counts must match and IDs must be
equal at every index. Tile ranges use capture chunk ordinals directly. This
preserves Morton order without imposing numeric stable-ID sorting. The default
sorted-subset path is unchanged. The authoritative producer is responsible
for uniqueness validation before opting into identity order. Tests cover
unsorted acceptance, exact ranges, mismatched IDs/counts, invalid mode, and
unchanged output sentinels on rejection.

The Grow producer now ties finish submission to its original producer stream;
an unrelated stream cannot supply completion proof. Its requirements API
computes exact input, output and peak device payload bytes before device
selection, with checked cardinality and arithmetic. Tests compare those byte
counts to actual resource-pool usage before and after commit, and prove a
wrong-stream finish cannot authorize publication. Five focused CUDA tests
pass (Scatter input, producer, producer preflight, tiles, and Session), along
with the CUDA-disabled CPU fan-out test. Compute Sanitizer memcheck reports
zero errors for producer preflight and tile construction.

The compiler/Session connection remains open: compile and retain the captured
payload, consume the requirements in task/graph admission, dispatch the fresh
producer through an asynchronous relay, select identity-order publication,
and add end-to-end CPU/CUDA graph parity and failure tests. Capability rows
remain unadvertised until this path is executable. Named-plane transformation,
full operator compositions, public backend adapter migration and native
Metal/Vulkan execution remain part of the overall execution-graph scope.

### Scatter/Grow Session integration audit (2026-09-13, in progress)

The dedicated `testUsdGenCudaScatterGrowSession` now builds and exercises a
300-root authoritative CPU reference with five CVs per curve. Its assertions
cover Morton-order stable IDs, positions/rest positions, hairT, description
width, root face/UV and full T/B/N frames, contiguous complete tile coverage,
and direct-executor parity. It retains a first-generation geometry lease
across a length edit and requires fresh point allocation with unchanged old
bytes and unchanged topology identity. Unsupported authored controls and an
injected finalization-allocation failure must preserve last-good publication;
a subsequent zero-density generation must recover successfully.
Held native source and final-metadata callbacks additionally require the
launchers to return without publishing, preserve readability of an older COW
lease, and reject duplicate finalization. Admission regressions cover wrong
native parameter types, duplicate Grow controls, unsupported mode, and double
length values that overflow the float device representation.

This test is not yet a passing checkpoint: the initial run stopped at missing
Scatter capability admission. Compiler/source integration is actively being
connected. Review additionally requires the asynchronous finalization path to
use the existing native tile/topology relay, rather than synchronous topology
comparison or publication without tiles. Synchronous failure handling must
retain callback state if stream completion cannot be proved. Matrix version
15 and its nine-row test oracle are being prepared together; their presence
alone is not evidence of executable support. Memory estimates, failure gates,
CPU/CUDA parity, and the full regression suite remain verification gates.

Producer failure-path follow-up: unproved H2D retirement now retains its
immutable host root owner alongside quarantined device allocations. The
keepalive is allocated before submission, so the noexcept destructor does
not allocate while abandoning completion proof. Normal successful commit
releases this staging ownership. The isolated
`testUsdGenCudaScatterGrowHostQuarantine` proves the host weak owner stays
live after producer destruction and that device charges remain retained;
ordinary producer preflight still returns to its resource baseline. Both
tests pass, and Compute Sanitizer memcheck reports zero errors for ordinary
preflight. End-to-end Session verification is still pending: the first build
of the integrated finalization path exposed private-workspace access and a
reservation-pointer assignment error, which are being corrected.

### Strict Scatter/Grow Session COW checkpoint (2026-09-13)

The strict static Scatter→Grow path is now executable through both Session's
native asynchronous source/finalization relays and direct `ExecuteCudaGraph`.
Capability matrix version 15 records the two version-0 authored operators.
Compiler metadata honestly models one fused `UsdGenScatterGrow` producer
task plus publication, rather than a nonexistent separate Grow dispatch.
Both authored capability rows remain available for inspection. Six immutable
job-owned geometry values feed publication; explicit workspace hazards and a
publication lifetime join pass the shared dependency compiler. Session and
the CUDA queue validate task counts against native stages, not authored node
counts, which may differ after fusion.

Graph admission now budgets retained output plus the larger of input staging
and publication scratch. Publication uses identity-capture-order tiles,
native bounds/topology proof, and the existing finalization relay. Description
default width is honored. Nonempty Grow mode, wrong native parameter types,
duplicates, nonfinite/non-float-representable length, unsupported lift/uvBlend,
and unsupported masks remain explicit admission errors. Clean source setup
failures release their relay; post-submission proof loss retains the producer,
poisons the workspace, and consumes the existing failure-injection hooks.

`testUsdGenCudaScatterGrowSession` passes CPU point/root-binding/frame parity,
Morton ID order, complete tiles, direct execution, retained-old-lease COW,
same-topology length edits, held source and final-metadata callbacks,
last-good preservation on invalid authoring and finalization allocation
failure, and empty-output recovery. Its non-neutral Scatter random-mask case
also passes the CPU oracle: CPU Grow's own identity mask overrides the
inherited Scatter mask, so applying the Scatter mask again would be incorrect.
No extra mask transport was added based on the superseded audit suspicion.
Compute Sanitizer memcheck and initcheck both report zero errors for the
end-to-end test. The CUDA capability-plan test and CUDA-disabled CPU fan-out
test also pass. The full CUDA rebuild succeeds and all 127 runnable tests
pass out of 128 registered, with the expected Storm surgery skip.

This checkpoint does not complete general Scatter/Grow graph compositions,
Grow masks/expressions, full named-plane topology transformation, authored
lift/uvBlend/flip semantics, public backend-adapter migration, or native
Metal/Vulkan execution. Those remain part of the original execution-graph
scope; the fused two-node path is an implemented step, not a scope reduction.

### Generated-topology downstream COW prerequisite (2026-09-13)

`MakeScatterGrowGeneration` now accepts optional fresh point and width
owners, using the same validation, readiness, reclassification and retirement
path as other CUDA topology owners. `PublishFinal` forwards those overrides
instead of discarding them. Direct tests verify output pointer/value identity,
unchanged original points/rest data and T/B/N frames, lease lifetime after
factory inputs and generation wrappers are released, exact retained-byte and
Pinned-category accounting, and final return to baseline. The accounting
oracle drains prior test retirements before comparing whole-pool category
deltas; asynchronous cleanup of a preceding fixture must not perturb this
owner's assertion. Memcheck and initcheck both pass with zero errors.

Poisoned workspaces are now rejected before job reservation or direct/source
submission, including jobs queued before context loss. Dedicated Scatter/Grow
source callback-install and native-status failure tests prove permanent
quarantine and workspace poisoning. Both pass, as does the strict Session
test. Existing execution-stage poison oracles now assert rejection at job
creation while still checking that already-queued source work cannot start;
the execution-stage regression passes after this contract update.

The next active composition is Scatter→Grow followed by Width/WidthBlend
value DAGs. A new `testUsdGenCudaScatterGrowWidthDag` covers both linear and
binary fan-in shapes with shuffled authored ordinals, no authored CurveSource,
CPU/CUDA point/width/ID parity, direct execution, tile metadata, swapped
ordered blend inputs, and retained-old-lease immutability across fresh width
publication. The test and runtime integration are in progress, not a passing
capability checkpoint. Lowering must retain one fused source, real native
Width tasks, all immutable predecessor values and publication lifetime joins,
and a phase-correct budget derived from generated rather than authored C3
cardinality. Broader topology/deformation compositions remain open.

### Grow-rooted Width DAG integration audit (2026-09-13, in progress)

The CUDA compiler/runtime extension now builds with matrix version 16 and
real Width/WidthBlend tasks after the fused source. Grow controls and seed
come from the explicit Grow semantic node, not the final Width node. Root
review corrected a publication-task identity bug: the metadata constructor
must receive the terminal task id, not the terminal logical value id.

The unmasked linear and branching CPU/CUDA cases run successfully, including
shuffled authored ordinals, length randomness, ordered fan-in, singleton and
empty output, direct/Session parity, and retained-old-generation COW. The
expanded test also saturates the resource ledger to peak-minus-one and
requires both entry points to reject without category changes or poisoning.
Width resource metadata now explicitly records immutable stable-ID and root
binding reads used by parameter/mask evaluation and image sampling, for both
authored-source and generated-source paths.

The ImageMap variant exposed a real CPU reference discrepancy: at incoming
width 0.025 and half mask, CUDA follows the documented interpolation toward
the target; CPU Width instead multiplies the target by the mask, while CPU
Grow leaves the generated width absent. For the tested fan-in this produces
0.284375012 on CUDA versus 0.275000006 on CPU. The CPU Width/Grow correction
and its mask/default-width regression tests are in progress. CUDA must not
be changed to reproduce that discrepancy. This remains an incomplete
composition checkpoint until all parity and regression gates pass.

The first joint rebuild after the CPU Width/Grow changes succeeds. Focused
validation passes Width, CPU fan-out, CUDA execution metadata, and the existing
CUDA Width DAG, but still fails generated-source width parity in both the new
Width DAG and strict Scatter/Grow Session tests. Inspection identifies another
integration dependency: generic CPU scheduler plane preparation synthesizes
zero-filled absent width storage on Scatter, which Grow then treats as an
authored input. The scheduler/source width policy is being corrected, not the
CUDA numerical oracle. The older CPU ImageMap unit test also assumed masked
target multiplication; its replacement explicitly supplies a nonzero incoming
width and checks interpolation toward the target.

A separate CPU CurveSource audit confirms its legacy surface-backed loader
currently writes a hardcoded width of 1.0. A validated description-default
fallback with capture invalidation and retained-generation COW tests is in
progress. Full CPU C3 CurveSet point/rest/width loading remains open and is not
implied by that fallback correction.

The corrected scheduler preserves absent optional planes on source generators;
Grow now materializes/resamples its private width owner and the generated-source
CPU/CUDA parity tests pass, including ImageMap, default-width edits, explicit
Grow terminals with downstream branches, and empty/singleton output. Compute
Sanitizer memcheck and initcheck both report zero errors for
`testUsdGenCudaScatterGrowWidthDag` and `testUsdGenCudaScatterGrowSession`.

Registering the previously unbuilt `testUsdGenCurveRagged` exposed obsolete
vector/chunk APIs and a CPU Deform fixture even though Deform now requires
CUDA. The source-topology test now uses a real identity Width consumer; this
does not claim CPU Deform support. It exposed a genuine CPU Width bug: ragged
chunks have uniform cvCount zero, so the old loop wrote no widths. Width now
iterates per-curve offsets rebased to the chunk's first CV. A direct regression
uses unequal spans and nonzero absolute offsets, and the pipeline test checks
description-default widths, recapture COW, and negative/NaN/infinite rejection.
The full post-fix regression checkpoint is pending rebuild and execution.

### Grow-rooted Width DAG COW checkpoint (2026-09-13)

The joint rebuild and complete CUDA-enabled CTest run now pass: **131 passed,
1 expected StormSurgery skip, 132 registered**, with no failures. The
CUDA-disabled Width, CPU fan-out, CPU reference/map, and registered ragged
source tests pass **4/4**. On the final rebuilt binaries, Compute Sanitizer
memcheck and initcheck each report **zero errors** for both generated-source
Width DAG and strict Scatter/Grow Session tests. `git diff --check` is clean.

This validates matrix-version-16 Scatter→Grow→Width/WidthBlend lowering,
generated-cardinality admission, explicit immutable source/resource reads,
publication task identity, retained old-generation COW, CPU local mask
interpolation, width fallback/resampling, and ragged CPU Width traversal.
It is not completion of the overall plan: full CPU C3 CurveSource ingestion,
broader topology/deformation graph combinations, renderer residency evidence,
and the remaining performance/fairness gates are still open. The next source
audit must address authored points/rest/widths/IDs/root data as a coherent C3
input, not treat the legacy surface fallback as the finished source loader.

### Authored CPU C3 source integration (2026-09-13, in progress)

The next implementation replaces the surface-backed surrogate when a
CurveSource has an authored curves relationship. An engine-only `curveLoader`
consumes the actual C3 descriptor, validates into a private candidate, sorts
all channels consistently by stable ID, materializes widths and canonical
hairT, and resamples geometry and named planes without mutating input or old
generation owners. The surface-backed path remains compatibility-only when
no curves relationship is authored. Missing rest-surface projection/frame
construction is an explicit remaining implementation, not silently approximated.

Compiler lowering now resolves literal useRest=false C3 sources in auto space
to Deformed and includes the resolved space in incremental reuse decisions;
an explicit contradictory rest-space override rejects before graph mutation.
Source payload hashing must independently guarantee posed-point refresh.
New CPU and CUDA-comparison tests share the same C3 fixture and check authored
rest/current points, widths, stable-ID reorder, ragged/resampled topology,
named channels, COW recapture, and invalid-input retention. These changes are
not yet a validated capability checkpoint.

The initial authored-source build passes the CPU tests but fails the new
CPU/CUDA comparison: CPU selected authored rest as its current points when
useRest=true. Plan 05 §2.1 and CUDA source publication keep these as separate
channels. The correction introduces an immutable `UsdGenCurveBuffer::rest`
AoS owner alongside current SoA points, transports it through references and
Width branches using COW, and compares both arrays against CUDA. The fixture
deliberately keeps points and rest different; making them equal would hide
the defect. Grow must also construct its new rest geometry from rest roots,
not retain old-cardinality storage or merely resample the input curve shape.
Its upstream ragged-root addressing now requires explicit input offsets and
rebasing, independently of its uniform output offsets. Those changes and
their source→Grow regression are in progress, pending joint validation.

The authored-source CPU/CUDA comparison now passes, including separate current
and rest points, widths, IDs, canonical hairT, named channels, default/constant
widths, empty input, and resampling. CPU tests also prove rest-owner aliasing
through Width and private generated rest rooted independently from posed
geometry through ragged Source→Grow. Six focused CUDA-disabled tests pass;
the shared CUDA source comparison is memcheck/initcheck clean. A non-resampling
load now preserves authored CVs exactly: the test uses 14 CVs where rebuilding
index 7 from floating-point hairT would otherwise interpolate accidentally.

The first full suite exposed two stale fixtures, not a passing checkpoint:
ExecutionCache supplied an empty C3 set but expected nonempty publication from
scalp geometry, and ScenePublication authored rebind=never without bindings.
The fixtures now author real curve data and explicit bindings/root frames;
publication/cache assertions remain intact. Root binding computation and
missing-frame construction remain open implementation requirements and are
not covered by those fixture corrections. Full-suite revalidation is pending.

### Authored C3 points/rest COW checkpoint (2026-09-13)

After the complete dependency rebuild, the CUDA-enabled suite passes **133
tests with 1 expected StormSurgery skip (134 registered), no failures**. The
six focused CUDA-disabled source/graph/Width/reference/fan-out/ragged tests
also pass. `testUsdGenCudaCpuSourceParity` reports **zero errors** under both
Compute Sanitizer memcheck and initcheck. `git diff --check` is clean.

Validated scope: authored C3 geometry rather than scalp surrogates; separate
current and rest payloads; exact non-resampled CV preservation; sorted stable
IDs with aligned widths, rest, bindings and named channels; constant/default
width materialization; ragged and resampled output; source/Width rest COW;
reference rest fallback and recapture; private Grow rest construction with
independent posed/rest roots; and upstream ragged-root addressing. Literal
posed-source space classification and payload-refresh tests are covered.

Remaining source work is explicit: automatic closest-point binding, optional
root-frame construction from surfaces, reference-lane CurveSource controls,
and full motion-sample/tail execution. Current C3 loading requires authored
bindings and frames and rejects unsupported construction paths; this is not
the full plan-05 loader gate. CUDA C3→Grow composition, other valid graph
combinations, production residency tracing, and performance/fairness/soak
gates also remain open. The overall execution-graph objective is not complete.

### Surface-derived rest root frames (2026-09-13, in progress)

An engine-neutral `surfaceRootFrames` helper now constructs frames from
already-authored parent-face bindings and an authored/default-time rest mesh.
It covers triangle barycentric, quad bilinear and n-gon fan parameters,
geometric or authored normals, Gram-Schmidt tangents and explicit degeneracy
drop masks. It does not perform automatic closest-point binding. Surface
transforms remain transport metadata rather than being baked into local frames.

The CPU C3 loader uses the helper when frames are absent and compacts every
channel before stable-ID canonicalization. CUDA integration uses the same
immutable host capture, transactional source compaction and native device
upload/Noise execution; this is not a claim of GPU-side frame construction
in the production source path. CUDA named-plane allocation must follow the
surviving cardinality while reads retain original source offsets. Synthesized
IDs retain original indices, never post-drop renumbering. Older COW owners
remain unchanged, including when a new candidate fails validation.

Initial focused checks pass for the CPU loader, portable frame helper and
CUDA source compaction utility. CPU/CUDA source parity, native frame-math
comparison, Noise Session and complete-suite revalidation are pending; this
is not yet a validated capability checkpoint. Noise also needs a genuine
rest snapshot even when its source selects posed processing space. Early
admission validation and runtime checks must agree about that requirement.

The six focused CUDA-enabled checks pass, including native tri/quad frame
math parity, CPU/CUDA source parity with partial/all-degenerate drops and
n-gon bindings, and derived-versus-authored Noise outputs after Length cull.
The first full 136-test run exposed two crashes. CpuFanout's authored-source
fixture used invalid two-vertex surface faces; capture rejected them and the
test subsequently indexed missing planes. The fixture now supplies triangles
and guards failed prerequisites without weakening its channel assertions.
CudaExecutionQueue's crash has not reproduced under GDB, 20 standalone
repetitions, or CUDA memcheck (zero errors). Its root cause is not established;
a passing rerun must not be described as a proven queue-race fix.

### Surface-frame integration verification (2026-09-13)

The complete rebuilt CUDA-enabled suite rerun passes **135 tests plus one
expected StormSurgery skip (136 registered), no failures**, in 34.79 seconds.
The rebuilt CUDA-disabled frame/source/fan-out checks pass 3/3. Both
`testUsdGenCudaCpuSourceParity` and `testUsdGenCudaSurfaceRootFrameParity`
report zero errors under memcheck and initcheck. The queue test also passes
both sanitizers; its earlier isolated full-suite crash remains unexplained,
not fixed by assertion or by repetition. `git diff --check` is clean.

Covered contracts include authored-frame bypass (no surface rest reconstruction
required), actual surface payload/normal edits invalidating CPU capture without
a generation bump, native tri/quad frame-math agreement with nonuniform normal
interpolation, float UV boundary handling, partial/all-degenerate compaction,
original-index fallback IDs, aligned named channels, and retained CPU/device
COW owners. CUDA Noise consumes derived axes after stable-ID-aware Length
culling and rejects a posed source with no genuine rest snapshot while keeping
its last-good generation.

Still open: admission-time source/frame/rest validation matching runtime;
nodeStats propagation of source-frame drops (currently diagnostics); automatic
closest-point binding; the broader source/motion/composition gates listed
above; and the unreproduced queue crash audit. Production frame construction
here is shared immutable host capture followed by CUDA upload, not use of the
native `CudaRestRootFrames` producer. Metal/Vulkan can reuse the engine helper,
but no Metal/Vulkan execution backend has been implemented or tested by this
checkpoint. The overall execution-graph objective remains active.

### Noise source admission (2026-09-13)

`ValidateCudaGraph` now rejects Noise source captures with missing, fallback,
wrong-cardinality or non-finite rest data before scheduling device work. This
is independent of literal/connected `useRest`: the native Noise kernel always
reads rest points. Authored matrices are validated without requiring surface
reconstruction; missing matrices require complete resolvable rest-surface
bindings accepted by the shared frame helper. Degeneracy remains a valid
compaction result, not an admission rejection. The execution-plan and Noise
Session tests pass 2/2 after rebuild, including malformed matrices, missing
bindings, invalid triangle UV, surface-rest provenance and authored bypass.
General source admission parity beyond these Noise prerequisites remains open.

The queue/pipeline lifetime audit found no concrete explanation for the earlier
unreproduced crash: workspace teardown follows pipeline drain, task relays
retain job owners, and native relay slots are process-lifetime with guarded
reuse. This is evidence against those specific suspected lifetime faults, not
proof that the observed crash has been fixed.

Automatic binding now has an engine-neutral helper implementation in progress.
Plan 05 §3.4 clarifies bilinear closest-point coordinates for warped quads,
consistent with frame reconstruction; triangle/fan patches keep exact triangle
tests and the eight-centroid candidate policy. No automatic stage writes are
authorized or introduced. Loader/CUDA integration, binding caching, numeric
validation and E-4 performance evidence are not yet complete.

### Shared automatic root capture (2026-09-13, in progress)

The portable `surfaceRootBindings` helper and shared `curveRootCapture` policy
are now connected to CPU C3 loading and CUDA source preparation. They produce
fresh original-order root data, preserving valid authored bindings under
`onError`, recomputing under `always`, and explicitly dropping invalid roots
under `never`. CUDA validates and sorts all source data before stable-ID
compaction; named-channel upload follows the surviving topology. The shared
source comparison now checks device rootPrim/rootUV as well as geometry,
widths, rest, hairT, IDs and named channels. No stage data is written back.

Source-local roots are transformed into surface-local space for binding.
Computed frames are converted back to source-local space: tangents use the
relative affine, normals its inverse transpose, origins the full affine, and
the result is re-orthonormalized/validated before publication. Authored frames
remain verbatim. Numerical tests cover repair subset preservation, failed
always-rebind dropping stale validity, empty sources, warped quad round trips,
nonuniform transforms and retained COW owners. Initial focused checks pass;
n-gon boundary precision and full-suite revalidation are pending.

This is shared CPU host capture feeding native CUDA execution, not a new
device-native closest-point producer. Persistent surface-epoch binding caches,
parallel binding performance/E-4, nodeStats counters, admission for all source
consumers and remaining graph/backend/motion gates are still incomplete.

The first 137-test integration run exposed two expected-boundary corrections:
the real standalone Width example has `rebind=never` and no surface or root
channels, so capture must preserve that optional absence rather than require
a binding surface or invent frames; and CudaSession's former invalid-face
rejection must now prove successful `onError` repair while preserving valid
siblings and retaining the repaired last-good generation on later failures.
Both paths have dedicated passing regression checks. Noise still rejects an
unbound source because it consumes root frames.

N-gon bindings now choose the geometric candidate before encoding. A fan
boundary uses an equivalent prior-fan representation when possible; a float
boundary approximation is accepted only if it reconstructs within 1e-5.
Otherwise the root is unresolved, not silently bound to a farther face.
Dedicated vertex/edge/large-scale precision tests are registered. Seven
focused CUDA-enabled tests and four CUDA-disabled tests pass; the source
parity and automatic-binding Noise Session fixtures are memcheck/initcheck
clean. The final 138-test full-suite rerun is pending.

### Automatic binding integration verification (2026-09-13)

After complete rebuild, the CUDA-enabled suite passes **137 tests plus one
expected StormSurgery skip (138 registered), no failures**, in 35.22 seconds.
The CUDA-disabled binding/boundary/source/fan-out checks pass 4/4. The shared
CPU/CUDA source parity and automatic-binding Noise Session tests each report
zero errors under memcheck and initcheck. `git diff --check` is clean.

This checkpoint proves the exercised CPU/CUDA source repair and publication
paths, including fresh immutable binding/frame candidates, aligned stable-ID
compaction, optional unbound-source channels, retained old-generation owners,
and last-good publication after subsequent invalid captures. It does not prove
global closest-point optimality for arbitrary bilinear patches or all meshes:
the solver is bounded and the search explicitly considers eight face-centroid
candidates. Persistent epoch-owned spatial caches, binding parallelism and E-4
performance, nodeStats propagation, all-consumer admission consistency, and
broader numerical/adversarial coverage remain open. Earlier performance
benchmarks also retain informational failing timing gates; a passing CTest
exit status is not performance acceptance. The previous isolated queue crash
has not recurred, but remains unproven rather than declared fixed.

The overall execution-graph objective remains active, including the previously
listed CUDA graph-composition, motion-tail, renderer-residency and cross-platform
backend work. Portable host capture is available to future Metal/Vulkan
backends; those backends are not implemented by this checkpoint.

### Immutable binding-index reuse (2026-09-13, in progress)

`UsdGenRestSurfaceBindingCache` owns a COW snapshot of spatial inputs plus
face offsets, centroids and a const nanoflann index. `Bind` uses call-local
scratch and fresh result owners, so concurrent read-only queries do not mutate
the cache. `Matches` compares authoritative surface identity,
provenance, transforms, rest points and topology; posed points and normals do
not invalidate the spatial index. VtArray identity gives the unchanged COW
case a fast comparison without substituting generation-only validation.
The broad `surfaceGeneration` counter also changes on posed-point edits, so
it does not force a rebuild when the actual rest/topology/transform payload
is unchanged; this is explicitly tested alongside edits without any bump.

CPU CurveSource operators and CUDA execution workspaces now retain one
immutable cache each. Shared root capture constructs a replacement privately
and leaves the old slot intact on failure; the CPU loader additionally waits
until subsequent named-plane/topology validation succeeds before committing
its cache slot. The CUDA workspace can reuse valid spatial state independently
of later device publication, without mutating any prior cache or generation.
The four initial cache/source checks pass, including concurrent queries,
payload edits without generation bumps, loader failure retention, CUDA
workspace isolation and retained device root-UV leases.

Full-suite and CUDA-disabled revalidation are pending. Compile/admission still
may build a temporary search index before the runtime workspace lookup;
parallel query execution, ledger admission/eviction of retained host cache
bytes, nodeStats and E-4 performance remain open. `BytesOwned` reports retained
spatial footprint, not a claim that those host allocations are already charged
to execution resource pools.

### Immutable binding-index verification (2026-09-13)

After rebuilding the payload-based invalidation fix, the CUDA-enabled full
suite passes **139 tests plus one expected StormSurgery skip (140 registered),
no failures**, in 29.72 seconds. The CUDA-disabled cache/source/binding checks
pass 3/3. The CUDA cache fixture reports zero memcheck and initcheck errors.
This includes ignoring generation-only/posed-point changes while detecting
actual spatial edits without a generation bump, preserving retained COW
snapshots, and isolating workspace cache ownership.

These results precede the next parallel-query implementation and production
surface-binding benchmark; they do not establish E-4 performance acceptance,
host-cache ledger accounting, or completion of the broader execution plan.

### Parallel immutable binding queries (2026-09-13, in progress)

The shared CPU/CUDA host-capture helper now schedules 128 or more roots with
TBB, using grain 32 within the caller's arena. Each worker has fixed k=8
scratch arrays and writes disjoint elements through pointers obtained from
fresh, detached result arrays before scheduling. No worker mutates VtArray
ownership or a shared unresolved counter. The count is reduced after joining;
transform failures discard the entire candidate and preserve the prior output.
The immutable index is shared read-only across concurrent Bind calls.

Tests compare a serial request with a repeated parallel request, concurrent
parallel results with distinct owners, worker-failure COW retention, and
parallel unresolved-count reduction. A new production surface-binding
benchmark exercises 4096 triangles and nonplanar bilinear quads at 100k/1M
roots with an eight-way arena, reporting cold Create+Bind and warm Bind
separately. This k=8 face-patch workload is not the k=3 guide-interpolation
workload and cannot by itself close the GuideInterpolate E-4 requirement.
Final build, timing and full-suite verification are pending.

### Parallel binding verification and measured gap (2026-09-13)

The complete CUDA-enabled rebuild and suite pass **140 tests plus one expected
StormSurgery skip (141 registered), no failures**, in 29.48 seconds. Three
CUDA-disabled cache/source/binding checks pass, and the updated CUDA cache
fixture remains memcheck/initcheck clean. This includes the explicit parallel
unresolved reduction and failure-retention tests. The fixed-size candidate
sort uses a bounded insertion loop to avoid GCC's small-array std::sort
array-bounds warning; candidate distance/face ordering is unchanged.

An isolated eight-participant arena benchmark reports warm median Bind times:

| Surface (4096 faces) | 100k roots | 1M roots | 10x-root scale | Cache footprint |
| --- | ---: | ---: | ---: | ---: |
| Triangles | 7.614 ms | 49.920 ms | 6.56x | 344509 bytes |
| Nonplanar bilinear quads | 116.486 ms | 1158.830 ms | 9.95x | 410041 bytes |

Cold Create+Bind is 9.501/57.213 ms for triangles and 116.819/1162.826 ms
for quads, respectively. Actual cold and every timed result are validated
outside their timing intervals. Quad timing fails both the benchmark's 25 ms
and 300 ms limits; the default informational CTest return does not hide or
close that gap. Conservative candidate bounds and fixed-size quad seed
scratch are the next optimization under review. These timings are a
surface-binding workload result, not GuideInterpolate E-4 acceptance.

### Conservative quad culling (2026-09-13, verification in progress)

Selected quad candidates now compute a control-vertex AABB lower distance
bound before the full solver. The box is expanded by scale-aware double
roundoff slack and outward nextafter; a candidate is skipped only when that
bound is strictly worse than the current best. Bilinear points for [0,1] UVs
lie in this box, so viable candidates and ties still run the unchanged bounded
solver. Quad seeds use fixed storage with their original iteration order.
Independent numerical review found no correction required under the existing
finite-input contracts. This is pruning within k=8, not expanded candidate
coverage or a new global-nearest guarantee.

Regression fixtures compare selected parent and exact emitted UV to separate
per-face solves for <=8 faces, covering centroid-nearest traps, coincident
quads, warped boundaries and translated surfaces. The oracle compares encoded
UV reconstruction distances on well-separated/exact-tie fixtures, not a proof
for arbitrary near ties or the pre-existing extreme-coordinate KD limits.
Six focused CUDA-enabled and three CUDA-disabled checks pass; CUDA cache
memcheck/initcheck remain clean.

An isolated `USDGEN_GATE=1 ./benchUsdGenSurfaceBindings` run exits zero:
triangles warm median 6.992/48.714 ms and bilinear quads 15.440/151.274 ms
at 100k/1M roots. Cold Create+Bind is 8.599/56.375 ms and 15.708/154.817 ms,
respectively. Quads improve about 7.5x over the measured unculled path;
their 10x-root scale is 9.80x. Footprints are unchanged. This closes the
benchmark's exercised warm limits, not all E-4 workloads, host-ledger admission,
or the wider CUDA/backend execution objective. Final full-suite rerun pending.

Final verification after the conservative-culling rebuild: **140 tests pass
plus one expected StormSurgery skip (141 registered), no failures**, in
29.63 seconds. `git diff --check` is clean. The isolated hard-fail benchmark
above is the timing evidence; concurrent full-suite timings are not substituted
for it. Cache host-byte admission/eviction and nodeStats propagation remain
open, as do the previously listed CUDA graph compositions, motion-tail,
renderer residency and Metal/Vulkan execution work. No overall completion is
claimed at this checkpoint.

### Automatic-root reservation correction (2026-09-13, in progress)

Review found that static source/Width estimates and selected-device Length
refinement still inferred optional root planes from authored skinPrim/skinPrimUv
alone. Automatic capture may produce both planes when those inputs are absent
or partial. The common estimate now reserves root channels whenever a surface
is resolved or either authored root plane is present, using the all-survivor
cardinality. A fully unbound surface-free source still omits the term; invalid
or dropped candidates may conservatively over-reserve. ReferenceSource's
surface-free authored transport is retained. This correction applies to device
geometry/evaluator/resampling/compaction bounds, not the separate host-index
ledger gap. RBF's existing complete-authored-root refinement eligibility is
unchanged in this slice.

CUDA-enabled and CUDA-disabled core builds pass; focused plan/cache tests
pass 2/2. Plan tests compare complete authored, absent and partial-root source
reservations. Tight-cap runtime Length tests and full-suite verification are
pending.

### Native C3 Grow implementation started (2026-09-13)

The composition audit confirmed that current layout treats every Grow as
Scatter->Grow. Reusing that producer unchanged would discard C3's separate
current/rest roots, ragged source spans and inherited width profiles. A
dedicated native CurveGrow producer is being implemented with fresh output
owners, explicit retained input ownership, stable-ID randomness and the
existing asynchronous commit/quarantine protocol. Executor lowering, named
channel transforms, generation publication and complete C3->Grow->Width DAG
validation are required follow-on integration work; no support is claimed
until those paths are exercised end to end.

The audit also exposed a pre-existing CPU Grow mismatch: plan 04 §2.2 and
plan 02's property contract define lift as degrees rotated about the root-frame
binormal, whereas `ops/grow.cpp` currently adds it as a positional distance.
The native producer checkpoint must reject nonzero lift rather than copy that
incorrect behavior. Correct angular lift on both paths, uvBlend, mapped/masked
controls and full named-plane/executor integration remain required work, not
closed by literal zero-lift kernel parity.

The broad CUDA-disabled rebuild succeeds. Its 84-test run has 82 passes,
one expected StormSurgery skip and one timing failure: Graph's E-6 incremental
recompile measured 0.215567 ms against 0.2 ms during parallel testing; its
functional assertions passed. Five isolated repeats pass. This records a
load-sensitive timing result, not a fixed performance defect or an all-green
CUDA-disabled suite. The timing gate has not been weakened.

### Native CurveGrow producer checkpoint (2026-09-13, validation in progress)

`gpu/curveGrow.{h,cu}` now provides a device-input producer with a required
opaque shared input owner, private current/rest/width/hairT/topology/binding
and frame outputs, double length/random capture arithmetic, ragged root
addressing, and a device plus pinned validation-status packet. The producer
retains input ownership until successful callback proof/commit; lost proof
quarantines the input owner and every output/status allocation. Published
objects cannot be mutated through BeginFresh. Named channels and graph/Session
lowering are not yet wired to this producer.

Normal and separate intentional-quarantine tests pass, covering input owner
retention, exact explicit allocation charges, distinct rest roots, random
lengths, directions, inherited/fallback widths, invalid/empty offsets and
nonfinite target rejection. Memcheck and initcheck report zero errors. The
first initcheck attempt was terminated after exposing a self-dependent
test-only blocking stream callback; the test now checks early commit before
installing the terminal callback and does not block stream API progress.

Review also found that float width sample positions on spans above 2^24 could
round past the last index (or to 2^32). Native and CPU Grow now clamp the
endpoint before integer conversion; native arithmetic boundary tests exercise
large counts without allocating huge geometry. Full rebuild/revalidation of
this final clamp is pending. Native callback success is a required caller
precondition for CommitFreshFinish. As with ScatterGrow, unproved teardown
abandons the native ready event as well as quarantining allocations; this is
not a claim of leak-free recovery from lost native proof.

The automatic-root runtime reservation test now exercises direct/asynchronous
publication and retained root binding channels under cap rejection. An
intermittent shared-ledger snapshot assertion appeared during development;
the new block was moved after existing ledger-sensitive tests, and an
incorrect expectation that Source->Length publishes T/B/N was removed.
Twenty-five subsequent repeated runs pass, but that is not proof that every
concurrent-retirement snapshot interleaving has been eliminated. Final full
suite and sanitizer verification are pending.

### CurveGrow primitive and reservation verification (2026-09-13)

After the complete rebuild, the CUDA-enabled suite passes **142 tests plus
one expected StormSurgery skip (143 registered), no failures**, in 30.25
seconds. Four focused CUDA-disabled Graph/fan-out/source/cache checks pass
after the CPU endpoint clamp; the earlier broad CPU timing miss remains
recorded above. Normal CurveGrow, intentional CurveGrow quarantine, and
ExecutionStages (including auto-root cap/reservation checks) each report zero
memcheck and initcheck errors. `git diff --check` is clean.

This verifies the exercised native producer and admission corrections, not
end-to-end C3->Grow graph support. Required next work includes generation
ownership/publication, native root-frame transport for Grow, named-plane
topology transforms, asynchronous executor lowering, complete control
semantics (including correcting angular lift), and their failure/lease/DAG
tests. Host binding-cache admission, nodeStats, motion-tail, renderer residency
and cross-platform backend execution remain open as previously recorded.

### C3 Grow generation publication and angular controls (2026-09-13)

The native C3 Grow producer now has `MakeCurveGrowGeneration` publication.
Its fresh topology owner participates in native readiness, consumer stream
waits, retirement/quarantine, retained-byte accounting and published-resource
reclassification. Geometry leases expose current/rest positions, inherited
widths, hairT, offsets, stable IDs, root bindings and T/B/N. Generic named
planes and private point/width overrides use the existing backend-neutral
generation metadata and CUDA owner contract; this does not implement a Metal
or Vulkan executor.

`testUsdGenCudaCurveGrowGeneration` exercises source immutability, old/new
point-revision leases with shared rest/width/frame storage, named-plane
retention after generation wrappers are released, empty publication, direct
override acceptance and wrong-cardinality rejection, and return to the
resource-pool baseline after retirement. The first factory/lease checkpoint
passed its focused tests and the intermediate 144-test suite (one expected
StormSurgery skip). Final rebuild of the expanded cases and angular changes
is pending below; the intermediate result is not their verification.

CPU and native Grow angular lift is implemented against plan-04 section
2.2: degrees about root-frame B, leaving current/rest roots anchored, with
finite/range validation, a zero-lift fast path and capture-digest invalidation.
Tests cover positive/negative angles around a non-world-Y B axis, CPU capture
and posed-value evaluation, distinct native current/rest roots, independent
lift/vector digest changes, and nonfinite/out-of-range rejection. End-to-end C3->Grow
graph admission, source-frame/named-plane transport, asynchronous stage
relays and memory estimates remain required. The graph validator still
rejects this composition; publication alone is not graph execution support.

The CUDA-disabled focused run again exposed the recorded E-6 timing
sensitivity: Graph measured 0.222447 ms against its unchanged 0.2 ms bound
while sanitizer work was active. Its functional assertions, CpuFanout and
CpuCurveSource passed; five subsequent isolated Graph repetitions passed.
This is retained as a timing miss, not evidence that the performance gate
is reliably satisfied under concurrent load.
The final CPU-only rerun concurrent with the CUDA suite also missed E-6
(0.265647 ms); CpuFanout and CpuCurveSource again passed. No timing bound was
weakened and no performance completion is claimed.

Final verification: the complete CUDA-enabled rebuild passes **143 tests
plus one expected StormSurgery skip (144 registered), zero failures**, in
30.60 seconds. The final native Grow and generation/override/lease tests
report zero memcheck and initcheck errors. The expanded CPU Grow capture,
Evaluate and digest assertions pass in both builds. An invalid test-only
`ChunkDesc::inCvCount` assignment found during rebuild was removed; the input
CV count belongs to `ChunkView`.

The post-suite CPU-only focused run still misses E-6 at 0.218895 ms while
its functional assertions and both source/fan-out tests pass. Thus concurrent
suite activity alone does not explain all observed misses. E-6 timing
reliability remains unproved; the earlier five isolated passing repeats do
not supersede these failures. No graph-lowering capability has been enabled
by this checkpoint. The next integration remains C3->Grow stage ownership,
root-frame and named-plane transport, asynchronous execution and admission.

### C3 Grow execution-graph integration (2026-09-13, validation in progress)

C3 CurveSource now lowers to a real native Grow topology stage followed by
Width/WidthBlend descendants, independently of fused Scatter/Grow. Grow must
dominate both inputs of every blend; bypass-source terminals are rejected.
CUDA capability matrix version 17 / Grow capability version 2 invalidates
older compiled capability assumptions. Grow currently accepts literal native
segments, length/random, root-normal/root-tangent/literal-vector direction and
angular lift. Nonidentity uvBlend, maps/masks, expressions and nonliteral
controls still fail closed and remain implementation work, not CPU fallback.

The stage retains uploaded source/resample/frame owners through native Grow
completion and terminal stable-ID tile construction. Its private output is
published through MakeCurveGrowGeneration. Generic named planes undergo a
second native resampling transaction when source resampling precedes Grow.
The operator relay uses separate Grow and named-topology native completion
phases; lost proof quarantines the job instead of replacing a prior lease.
Proved named-plane rejection releases the raw candidate's self-retained job.

Memory metadata conservatively bounds source/Grow/Width cardinalities with
the maximum of source-resampled and grown points; it accounts for both source
and output T/B/N, native status storage and named-topology status. This can
over-reserve after a shrinking Grow and is not an exact liveness refinement.
Dynamic source-cardinality plans retain the existing unavailable-static-bound
contract rather than claiming a proved peak.

The new parity test exercises CPU, Session, direct, all-async and mixed
async-source/sync-operator/async-publication paths, with ragged IDs, distinct
posed/rest roots, inherited widths, bindings/TBN, named planes, retained
leases, source resampleTo=3 and 30-degree lift. Testing found and corrected a
general mixed-mode reservation bug: synchronous compatibility stages closed
Pending credit but left a closed reservation object for later async allocators.
Those quiescent boundaries now detach the closed object so later stages use
exact per-allocation admission. Failure/retirement paths retain the object
for any already-running consumers.

Dedicated process-isolated failure tests cover saturated-cap rejection and
callback-installation loss after Grow launch or its named-plane transform;
the old generation remains readable and unused Pending credit closes. Initial
focused runs pass. Full-suite/sanitizer validation and expanded boundary cases
are pending; this checkpoint does not close the rest of the execution plan.

Boundary coverage now includes empty publication, current-space C3 without
authored rest (with authored frames), absent named planes, bypass-branch
rejection and a source-only terminal rejection. Grow does not inherit Noise's
stricter authored-rest requirement. A test teardown crash was traced with GDB
to releasing geometry leases after destroying their consumer CUDA stream;
the test now releases those leases first, following the existing lease API.

The E-6 compiler unchanged-descriptor path now retains its already-proved
parameter-value digest instead of rehashing every reused node's parameters.
The existing field-wise descriptor equality checks params, ramps, enabled,
blend, type/version and excludes expression-bound descriptors from this fast
path. Regression tests verify an actual Width value edit refreshes its digest
and schedules fresh evaluation while unchanged source data retains its digest.
The compiler's transactional rebuild behavior is preserved. Focused functional
checks pass, but CPU-only E-6 still measured 0.209968 ms against 0.2 ms during
concurrent CUDA tests; the optimization does not establish gate reliability.

### C3 Grow integration verification (2026-09-13)

The final full CUDA-enabled rebuild passes **148 tests plus one expected
StormSurgery skip (149 registered), no failures**, in 32.79 seconds. The three
focused CUDA-disabled Graph/CpuFanout/CpuCurveSource checks pass in the final
isolated run. The earlier CPU E-6 timing miss remains recorded; passing this
rerun does not establish reliability across concurrent load. Informational
E-1/E-7 performance misses likewise remain open, not converted to acceptance
by a green default CTest run.

C3 Grow Session/direct/all-async/mixed dispatch and all four process-isolated
Grow/named-plane callback installation/native-status failure modes report
zero memcheck and initcheck errors. Failure cases assert retained charged
storage beyond the pre-Grow allocation baseline, zero unused Pending credit,
poisoned failed workspaces and readable unchanged prior geometry. Synchronous
finalization now explicitly rejects an in-flight operator before detaching
its closed reservation. `git diff --check` is clean.

The first broad run caught the old Grow capability-version expectation in
the plan test; it was updated to the actual version-2 row, retaining exact
matrix/row/flag assertions. The subsequent complete suites pass. Literal
C3->Grow->Width/WidthBlend graph execution is now implemented and exercised;
this supersedes the earlier publication-only checkpoint, not the remaining
control, graph-composition, backend, residency or performance requirements.

### Fused Scatter/Grow control parity (2026-09-13, validation in progress)

The fused native producer is being brought into parity with CPU and C3 Grow:
lift is an angular rotation about each root's B axis, not a root displacement;
literal lift in [-90,90] and attribute/root-tangent direction are admitted.
Length/random arithmetic remains double until the per-strand float target.
CUDA capability matrix version 18 / Grow row version 3 tracks this extension.
Nonzero uvBlend and arbitrary direction primvars remain unsupported.

Native output checks now use a device error plus pinned host status, consumed
only after the matching native completion proof. Both status allocations are
included in producer steady memory and the graph's concurrent peak, including
empty sources. Output geometry remains private until successful commit; old
COW leases must remain unchanged across lifted replacements and failures.

New CPU/direct/Session parity cases cover +/-30 and +/-90 degrees, preserved
strand lengths, retained old leases and a non-world-Y root B axis. Empty-source
metadata checks cover retained status storage in addition to the zero offset.
These edits are not yet verified: the initial focused build caught a CUDA
status enum comparison in native teardown, which is being corrected before
rerunning. The preceding 149-test checkpoint applies to the earlier state,
not these in-progress edits.

Focused validation corrected two integration issues: the publication ledger
test now checks the status pair's Scratch-to-Pinned transfer separately from
geometry's Active-to-Pinned transfer, and the fused source's test-only hold
now runs inside its terminal callback after status readback. A predecessor
hold could stall readback submission under initcheck until its timeout; that
run reported no memory errors but failed the source completion assertion.
After relocating the hold, both the primitive and Session parity executable
pass memcheck and initcheck with zero errors. A full rebuild and suite rerun
are in progress; invalid/out-of-range lift and unsupported uvBlend rejection
are also explicitly covered.

### Fused Scatter/Grow parity verification (2026-09-13)

The rebuilt CUDA-enabled suite passes 148 tests plus one expected StormSurgery
skip (149 registered), with no failures, in 32.56 seconds. The final Session
executable and both process-isolated source callback installation/native
failure modes each pass memcheck and initcheck with zero errors; the updated
native primitive also passes both tools. COW replacement, preserved old
leases, angular/root-tangent parity and status-storage ledger assertions are
covered. This closes the literal fused lift/direction parity slice only.
Nonzero uvBlend, general controls/compositions, Metal/Vulkan implementations,
residency and the previously recorded performance gates remain open.

### Grow uvBlend implementation (2026-09-13, validation in progress)

The next control slice implements literal finite uvBlend in [0,1] on CPU,
C3 CUDA Grow and fused Scatter/Grow. Section 2.2 of plan/04 now specifies
normalized linear interpolation toward retained root T after angular lift,
with exact zero/one endpoints and a lifted-direction fallback for degenerate
tangents or cancelled blends. The older dPdu/dPdv wording did not define a
selector; the pseudocode's dPdu target is represented by root T. No new raw
derivative plane or cross-platform-specific interpretation is introduced.

CPU capture hashes and stores uvBlend and applies it to both rest capture
and current evaluation. CUDA compilation forwards the control to both native
producers; matrix version 19 / Grow row version 4 invalidates earlier cached
capability assumptions. Existing immutable source frames are read-only and
the native output remains private until successful completion/publication.
CPU same-graph recapture/COW tests and CPU/CUDA Session and primitive parity
tests are being extended; this checkpoint is not a verification claim.

Initial functional CPU and both CUDA Session parity checks pass. The native
antipodal test oracle was corrected to use exact opposing axes at zero lift:
cosf(pi/2) leaves a residual larger than the specified 1e-12 cancellation
threshold and is not an exact antipodal input. CPU capture digesting includes
the authored uvBlend type, preventing malformed edits from silently reusing a
zero-blend capture through ParamView's default conversion. Same-graph tests
cover valid recapture and invalid scalar/range edits with preserved COW data.
During a concurrent CUDA rebuild, CPU E-6 measured 0.209951 ms against 0.2 ms;
the subsequent focused three-test CPU rerun passes. This repeats the known
timing-gate reliability gap and does not close it.

### Grow uvBlend verification (2026-09-13)

The complete CUDA-enabled suite passes 148 tests plus one expected
StormSurgery skip (149 registered), no failures, in 32.68 seconds. Both native
Grow primitive executables and both CPU/direct/Session parity executables
pass memcheck and initcheck with zero errors. The final focused CPU-only
Graph/CpuFanout/CpuCurveSource rerun also passes; the earlier concurrent-build
E-6 timing miss remains open as recorded above.

Coverage includes zero/interior/one blends, length preservation, independent
lift-before-blend endpoint oracles, exact antipodal and zero-tangent fallback,
invalid values, CPU same-graph recapture, current/rest roots, and immutable old
COW leases across blend edits. This supersedes the earlier nonzero-uvBlend
restriction for literal controls on the admitted C3 and fused Scatter Grow
graphs. Image/other map-driven controls, general expressions/compositions,
Metal/Vulkan backends, residency and outstanding performance gates are not
completed by this checkpoint.

### Grow LengthSource ImageMap integration (2026-09-13, validation in progress)

CPU Grow previously ignored length:source while CUDA rejected maps. A shared
backend-neutral resolver now defines the initial image-map path: one typed
LengthSource relationship, decoded immutable ImageMap payload, root/st/raw
scalar sampling with authored filter/wrap/channel/scale/offset/clamp/default.
Grow adds no further sample clamp. A negative or nonfinite effective sample
or target length is rejected rather than publishing reversed/invalid curves.
Other map types, generic expressions and arbitrary map domains remain open.

Both CUDA Grow producers own an optional image uploader and per-root sample
buffer. Sampling precedes the Grow kernel on the same stream; the image,
host pixels and sampled values live through the existing native completion
proof. Proven commit frees this temporary storage; lost proof quarantines it
with the producer. Scatter root UV upload must precede sampling, a sequencing
issue corrected during review. No additional host geometry fallback or relay
phase is used. No-map calls keep their existing API defaults and fast path.

Graph lowering forwards the map to synchronous, asynchronous and direct
entrypoints. Metadata records the immutable image read and adds image texels
plus one sampled float per root to producer scratch; matrix 20 / Grow row 5
tracks the contract extension. CPU and Session tests now cover sampled length,
payload replacement with retained COW generations, and negative-sample
last-good behavior. The focused build is in progress after correcting a local
diagnostic-variable scope; these new map edits are not yet verified.

Focused map parity, empty-source and negative-sample last-good checks pass on
CPU and both CUDA graph paths. Native test fixtures were corrected to rebuild
inputs after the original lifetime tests deliberately released them, and to
destroy the additional mapped producer before asserting the original
producer's resource categories. The strengthened native assertions release
both caller image references immediately after Begin, prove host-image
retention until completion, and verify exact map scratch release at commit.

The first broad run's only failure was that Scatter fixture scope; it was
corrected without weakening resource assertions. Both updated primitive and
both Session executables now pass memcheck and initcheck. Scatter Session's
held-callback test initially timed out under instrumentation while issuing
a CUDA readback during the artificial hold. It now checks old bytes before
and after the hold and keeps the nonblocking/pending assertions inside it;
the instrumented reruns pass with zero errors. Failure fixtures now include
image sampling for both fused source failure modes and all four C3 Grow/
named-topology callback failures. Full verification is still in progress.

CPU-only CurveSource map/COW and fan-out tests pass. E-6 again missed its
0.2 ms limit (0.206463 ms); the existing performance reliability gap remains
open independently of map correctness.

### Grow LengthSource ImageMap verification (2026-09-13)

The final CUDA-enabled full rebuild/suite passes 148 tests plus one expected
StormSurgery skip (149 registered), with no failures, in 33.90 seconds. Both
native primitive and both CPU/direct/Session parity executables pass memcheck
and initcheck with zero errors. The two mapped fused-source callback failure
modes and all four mapped C3 Grow/named-topology callback failure modes also
pass both tools with zero errors. Quarantined allocations remain intentional
on lost proof; these results do not claim leak-free recovery.

CPU-only CurveSource map/COW and fan-out checks pass. The final isolated Graph
run again misses only E-6 timing: 0.205135 ms against 0.2 ms. Its exact rebuilt
node, digest, fresh-evaluation and source-space assertions pass. This remains
an outstanding performance requirement, not a green CPU-only suite claim.
`git diff --check` is clean.

Typed root ImageMap LengthSource is now implemented on the admitted C3 Grow
and fused Scatter/Grow graphs, including temporary image ownership/accounting,
empty publication and last-good preservation. Ptex/Expr/Paint/Noise maps,
broader masks/expressions/compositions, Metal/Vulkan implementations, residency
and the remaining performance requirements are still open.

### E-6 static operator-contract reuse (2026-09-13, validation in progress)

Reference/map preflight constructed and destroyed a fresh operator for every
node merely to query its static role and reference-input arity, even when no
references were authored. The registry now captures those fields alongside
geometry arity during its existing registration probe, keyed by the actual
(type, version) entry. GetOperatorContract preserves newest-version selection
for requested version zero and exact matching for nonzero versions. Compiler
preflight reads one contract per descriptor node and reuses it through both
validation passes; it does not skip reference checks or move the old graph
before validation succeeds.

Factory-counter tests prove one probe per registration, no constructions for
contract queries, one executable construction for a fresh one-node graph,
and no new construction on unchanged recompile. Version-specific geometry,
reference arity and role, absent versions, exact zero/nonzero reference-count
diagnostics, and preservation of the old operator/digest after failed
recompile are exercised. Focused CPU graph/fan-out/reference/map/CurveSource
checks pass; the final expanded reference test also passes.

The unchanged E-6 timing gate improves in many runs but is not reliably closed:
a ten-process audit measured [.175, .259, .112, .113, .194, .105, .111, .107,
.184, .207] ms (printed precision), with 8 passing and 2 timing failures.
All structural/digest assertions passed. Exact failing timings were 0.258655
and 0.206831 ms against the unchanged 0.2 ms gate. This is evidence of removed
work, not evidence that E-6 is complete. Full CUDA-enabled validation follows.

The subsequent CUDA-enabled full build succeeded, but the 149-test suite
finished with 147 passes, one expected StormSurgery skip and one failure in
`testUsdGenCudaScatterGrowWidthDag` (33.03 seconds). Its first Session commit
reported only `CUDA operator stage failed`; the output did not identify the
fixture variant. This checkpoint is not green and the cause is not established.
Both asynchronous queue entrypoints now distinguish superseded, submission and
completion failures and identify the operator index. The DAG fixture also
prints its variant. Focused reproduction and diagnosis are in progress; these
diagnostic changes do not alter GPU execution or relax COW assertions.

### Empty-external compiler fast path and diagnostic verification (2026-09-13)

Compiler preflight now elides empty reference/map containers only when all
four authored collections (references, curves, legacy maps, typed map bindings)
are empty. Cached Reference-role/arity checks still reject missing required
slots with the original exact diagnostic. Final binding clears resolved handles
before skipping empty work; nonempty binding validation and general DAG
descendant construction remain unchanged. Added tests cover removing the last
legacy and typed-only map binding so stale handles cannot survive a recompile.
CPU-only reference/map, CurveSource/COW and fan-out tests pass.

A fresh ten-process CPU-only E-6 audit passes 10/10 with printed timings
[.112, .104, .180, .107, .107, .108, .108, .109, .107, .128] ms against the
unchanged 0.2 ms gate. Structural/digest assertions also pass. This improves
the observed margin but does not erase the earlier timing failures or prove
cross-platform performance acceptance.

The instrumented ScatterGrowWidthDag executable passes 20/20 focused runs;
the subsequent full CUDA-enabled suite passes 148 tests plus the expected
StormSurgery skip (149 registered), in 33.05 seconds. The DAG executable also
passes memcheck and initcheck with zero errors, including its retained COW
generation and predecessor-publication assertions. The additional binding-
removal tests were added after the full-suite binary build and verified
separately in both CPU-only and CUDA-enabled builds. The original intermittent
CUDA operator failure remains unexplained; these passing runs are not a
root-cause fix. No Metal/Vulkan kernels or broader operator compositions were
implemented in this compiler/diagnostic slice.

A read-only source/Width relay audit found no concrete successful-path race:
source completion follows committed COW value recording, Width publication
is bracketed by the in-flight counter, and reservation consumption/release is
mutex-linearized. A failed branch may close the shared reservation while a
sibling still runs, making subsequent sibling allocations fail closed; this
does not explain the initial failure. Indexed failure diagnostics remain the
next evidence source if it recurs.

### Length topology-trunk WidthBlend fan-in (2026-09-13, in progress)

The executor's Length compaction already records an immutable width value and
its completion event before releasing Width descendants. WidthBlend already
waits on both ordered predecessor planes and the shared compacted geometry.
Layout validation nevertheless rejected every Length-rooted WidthBlend. That
blanket rejection is removed while retaining the single direct topology trunk
and both-input dominance proof; joining pre-Length and post-Length geometry
remains invalid in this representation.

Literal-Length runtime admission now accepts this SourceRootedValueDag and
charges every WidthBlend output using the existing conservative per-Width
upper bound. Optional CUDA Graph storage remains separately cache-charged,
with the ordinary native launch used when capture cannot be admitted. The
runtime estimate also sums each mapped Width's private image texels and
per-root samples, which the previous literal-Length estimate omitted. Matrix
version 21 / WidthBlend capability 2 invalidate prior executor assumptions.
CPU/direct/Session parity, compaction, ordered fan-in, mapped branches and
retained COW generation validation are being added; this slice is not yet
verified.

Resumed with Astra coordinating review, Terra completing test implementation,
and Luna providing an independent audit. The first expanded test compiled
after correcting interrupted test API calls, then exposed a CPU reference
limitation: CPU Length kept three curves/nine CVs where CUDA culling expects
two curves/seven CVs. CPU Length collapses rejected curves rather than
compacting them; its accepted cutExtend control also lacks the corresponding
evaluation path. These CPU capabilities remain open, and cannot be claimed as
a full parity oracle for the CUDA topology contract. The test is being
separated into supported CPU parity and explicit host topology expectations.

Review also identified a pre-existing source-terminal correctness gap in the
Length DAG: selecting CurveSource after Length executes restores only widths,
not the complete source geometry/named-channel snapshot. A proper immutable
source snapshot and source-owner publication selection are required; rejecting
this valid composition as unsupported would not satisfy the graph goal.

### Length fan-in verification and remaining source selection (2026-09-13)

The Length topology-trunk WidthBlend extension builds and passes the complete
CUDA-enabled suite: 148 passes plus one expected StormSurgery skip, 149 tests
registered, 33.73 seconds. The expanded WidthDag executable separately passes
memcheck and initcheck with zero errors. Cases include ragged scale, cut/extend
shortening, two-survivor and empty culling, ordered operand edits with retained
COW generations, a direct Length blend operand, Length and Width predecessor
terminals, and 128x128 image-masked Width branches via direct and Session paths.
The exact mapped Pending reservation delta is image texels plus one float per
source curve; old generations remain readable after the map changes.

Reference qualification is important: CPU Length also leaves a ragged scale
CV unchanged (GPU P=(.25,.25,.3), CPU P=(.3,.3,.6) in the fixture). The new test
therefore uses CPU Source/Width/Blend with identity Length, then independent
test-only host polyline transformation/compaction for these constant Width and
root-map fixtures. It is not a general CPU Length parity claim or a production
geometry fallback. CPU Length ragged scaling, true culling and cutExtend remain
separate implementation gaps.

The literal-Length named-channel reservation now takes
max(authoredNamedBytes + resampledNamedBytes, 2 * resampledNamedBytes), with
checked arithmetic, to cover both downsampling and later compaction phases.
A regression added after the full suite proves the exact 60-byte increment
for one float plane downsampled from nine to six CVs; the final focused run and
both sanitizer runs include this assertion. CPU-only build and focused
fan-out/reference-map/CurveSource tests also pass. Diff whitespace checks pass.

Astra completed the ownership/admission review and Terra contributed fixture
and test implementation. Later Terra/Luna assignments remained pending_init
and were interrupted; root finished the oracle and validation locally. No
source-terminal snapshot implementation landed in that interruption. The
complete Source-terminal geometry/named-channel selection fix remains the
next concrete correctness task, with Astra's design: retain source views and
original named owners, restore the full selected snapshot at the publication
join, and transfer Source/Resampled ownership instead of Compacted ownership.
Do not claim that source-terminal case is covered by the passing fan-in tests.

### Source-terminal snapshot selection after Length (2026-09-13, validating)

ExecutionState now records the completed source geometry, topology kind,
hairT, root bindings and deformation flag alongside the immutable source width
value. Source/resampler owners were already retained. When Source is the
selected terminal, both direct and asynchronous Length completion preserve
the original named-channel owners before installing compacted named planes
for descendants. No extra geometry copy or host readback is introduced.

At the all-task publication join, source selection restores the complete
snapshot exactly once and selects MakeSourceGeneration/MakeResampledGeneration
instead of transferring the unused compactor. Unselected compactor/Width
owners survive until ordinary proven state teardown, so retrying finalization
cannot observe dangling logical views. Matrix version 22 invalidates prior
executor assumptions. Astra reviewed ownership and identified a device-context
ordering issue; both finalizers now select the workspace device before
SelectTerminal performs any stream waits.

Focused tests pass for same-cardinality scaling, two-survivor culling, empty
descendants and source downsampling, selecting Source through direct CUDA and
Session. Checks include points/rest/widths/IDs/offsets, hairT, root prim/UV and
named float payloads against the actual CPU Source terminal, not the incomplete
CPU Length oracle. A rejected finalization-relay admission followed by a
synchronous retry proves named ownership survives repeated selection. Full
suite and sanitizer validation follow; multiple-device execution is not yet
empirically validated on this single-device host.

Source-terminal verification is complete for this slice: the full CUDA-enabled
build and 149-test suite pass (148 passed, one expected StormSurgery skip),
34.49 seconds. The final expanded WidthDag executable passes memcheck and
initcheck with zero errors, including full source payload selection, an old
source generation retained across workspace reuse, and the finalization retry.
The CUDA-disabled core rebuild and focused fan-out/reference-map/CurveSource
checks pass; `git diff --check` is clean. No claim is made that this closes
arbitrary topology branches, Source selection around Grow, CPU Length gaps,
Metal/Vulkan, or the remaining execution/resource/performance gates.

### Source-terminal snapshot selection around C3 Grow (2026-09-13, validating)

Grow retains source/resampler/TBN inputs in a shared native input bundle. At
the all-task join, Source selection now waits on the bundle's source owners
and returns those owners to ExecutionState for ordinary immutable source
publication. No handoff occurs before Grow/native named-topology proof, and
the unused Grow output and frame bundle remain retained through state teardown.
AcceptCurveGrow preserves original named owners only after successful named
topology commit, using the same source snapshot contract as Length. The layout
now allows Source as publication terminal while still requiring Grow to
dominate every descendant Width/WidthBlend operand. Matrix version 23 tracks
the new selection contract; this is not admission of arbitrary Grow suffixes.

Tests select Source around Grow with named float/int planes, distinct current
and rest points, ragged source widths/IDs, upsampling, downsampling and empty
input. The source's CatmullRom basis must survive instead of inheriting Grow's
BSpline basis. Source publication retains its existing frame-channel contract
(no exposed TBN); generated Grow TBN must not leak into the selected source.
Direct and Session tests and retained-source checks are in validation.

Grow Source-terminal selection is now verified: full CUDA-enabled rebuild and
suite pass 152 tests plus one expected StormSurgery skip (153 registered),
36.60 seconds. Retained direct and Session source generations remain unchanged
across source/resampling edits. A fully asynchronous job with finalization
relay capacity zero rejects admission after selecting Source, then succeeds
on retry without losing reclaimed source/resampler or named owners.

Four new process-isolated tests select Source while injecting Grow/named
callback installation and native completion failures. They prove pre-submit
cap rejection is ledger-neutral, unsafe storage remains charged/quarantined,
Pending returns to zero, and the previous two-CV source generation stays
readable. The expanded CurveGrowSession executable and all four new failure
modes pass memcheck and initcheck with zero errors. Intentional quarantine is
not a leak-free recovery claim. CPU-only rebuild and focused fan-out,
reference/map and CurveSource tests pass; diff whitespace checks are clean.
Astra reviewed the owner handoff, selected-device waits, retry behavior and
existing conservative memory bound; root implemented and validated the slice.

The next admitted-composition gap is Grow followed by Noise. It needs correct
linear async progression and access to Grow's immutable TBN frames (the old
source frame owners have moved into growInputs), not just a validator change.
Grow followed by Length additionally needs compactor publication precedence,
expanded runtime admission and survivor frame transport. These, CPU Length
gaps, broader branches/maps, residency, platform adapters and the outstanding
performance/reliability requirements remain open.

### C3 Grow to Noise composition and COW frame ownership (2026-09-13, validating)

The new admitted slice is the ordered linear CurveSource -> Grow -> Noise/Width
suffix. Existing shuffled Grow -> Width/WidthBlend topology DAGs and Source
terminal selection remain intact. Grow must directly consume CurveSource;
Grow -> Length, Noise -> Grow and broader branching suffixes remain open,
not silently admitted through the linear path. Matrix version 24, Grow row 6
and Noise row 2 identify this executor contract.

Noise borrows the complete immutable source-order TBN owner bundle, including
after its move into growInputs, with the original sorted stable IDs. Both
execution paths wait on and record use against the selected owners. Linear
asynchronous Grow completion advances the next operator. Grow-generated rest
permits post-Grow Noise with useRest=false and no authored source rest, while
ordinary Source -> Noise retains its strict rest requirement. Native Noise
accepts canonical empty C3 offsets [0] and validates that zero on the device;
malformed empty offsets still fail. No geometry readback fallback is added.

Noise output and staging estimates now charge float3 rather than float, and
the source estimate charges retained TBN storage for Noise-only graphs as
well as Grow. Direct lost-proof handling preserves borrowed frames, geometry
producers and previous point/width overlays, and retains heap callback state
when completion cannot be proved. Published generations continue owning
private changed point planes and immutable rest/topology/named payloads.

The first full rebuild succeeded. Native Noise, NoiseSession, ExecutionPlan
and WidthDag focused checks passed; the new GrowNoise comparison exposed a
CPU reference discrepancy: ops/noise.cpp displaces a scalar field along rootN,
whereas the native CUDA operator uses vector3 SeExpr FBM projected through
TBN (already independently tested in testUsdGenCudaNoise). CPU seed and
restoration semantics also differ. This is an open CPU Noise gap, not CUDA/CPU
parity evidence. The composition regression is being corrected to use CPU
Source/Grow/Width with identity Noise plus an independent host vector-field
reference for explicitly bounded constant controls and preserveLength=0.
Native Noise including canonical/malformed empty tests already passes
memcheck and initcheck with zero errors. Expanded composition and full-suite
verification are still pending. Astra orchestrates/reviews; Terra implements
the executor, Luna implements native empty support and composition/estimate
regressions, and root performs integration, native tests and validation.

Final verification for this slice is complete: full CUDA-enabled rebuild and
153-test suite pass (152 passed, one expected StormSurgery skip), 36.52 seconds.
The expanded CurveGrowSession checks direct, fully staged asynchronous and
Session execution against the independent bounded vector/TBN oracle. Its
baseline explicitly sets magnitude to zero: the CPU operator's enabled
parameter alone did not suppress its scalar displacement. Tests cover a
rotated frame paired with sorted IDs, source up/downsampling, empty topology,
and generated rest without authored source rest. A Noise magnitude edit
produces fresh storage and changed positions while a held generation's points,
rest and widths remain byte-identical. Existing named/topology/root/frame
payload comparisons remain in place. Guard tests reject Noise-before-Grow
and Grow-before-Length until their execution contracts are implemented.

Native Noise, NoiseSession and the expanded CurveGrowSession each pass
memcheck and initcheck with zero errors. CUDA-disabled rebuild and focused
CPU CurveSource, fan-out and reference/map tests pass; diff whitespace checks
are clean. Astra's final ownership/admission/oracle review found no remaining
blocker for this slice. Lost-proof handling was reviewed; this does not claim
new injected Noise failure recovery or leak-free context-loss recovery.

Next work remains Grow -> Length publication/admission/survivor-frame
transport, wider graph compositions and parameter/map coverage, CPU Length
and Noise semantic gaps, residency/renderer handoff, platform implementations
and the outstanding performance/reliability gates. Metal/Vulkan remain
backend-neutral seams, not implemented execution adapters. The overall plan
and active implementation goal remain open.

### Grow to Length survivor-frame publication (2026-09-13, validating)

Matrix 25 (Grow 7, Length 2) adds the ordered C3 Grow -> Length/Width
composition. The existing Grow -> Noise/Width path is preserved; mixed
Grow/Noise/Length is still rejected rather than claiming an unverified empty
survivor-frame contract. The published compactor now takes precedence over
the retained Grow input, preventing publication of stale pre-Length geometry.

CudaCurveCompaction accepts optional aligned TBN planes. Both legacy and
fresh counts/scatter APIs validate complete shape, finite frame values and
optional input frame-ID correspondence on the device. Survivor frames copy
in geometry order; output frame IDs borrow the compactor's stable-ID plane,
not a duplicate allocation. Frame storage participates in waits, use records,
retained-byte accounting, publication reclassification and quarantine.
Generation owners expose compacted frames without imposing Grow's stricter
root-binding requirement on otherwise valid generic framed compaction.

Runtime admission accounts for retained source/Grow geometry, simultaneous
old/new compactors, optional source resampling, all live frame sets, named
planes, Grow image-map scratch and selected-device CUB storage. Direct Length
lost-proof handling now retains its borrowed Grow/source/frame inputs.

The first CUDA-enabled build passed. New native frame tests cover survivor
ordering, keep-all/none, fresh asynchronous publication, missing/non-finite
frames and wrong ID association without replacing an accepted output. Four
process-isolated Grow/Length failure tests cover callback installation and
native completion failure at Length and counts phases, requiring pre-submit
Pending admission, ledger-neutral budget rejection, charged quarantine and
readable old publication. All five executables/modes pass memcheck and
initcheck with zero errors; intentional quarantine is not recovery evidence.

Full composition validation remains in progress. Its first host reference
incorrectly scaled around authored rest roots rather than current Grow roots;
the translated useRest=false fixture exposed that mistake. Default keepParam
cutExtend must independently sample clipped arc distances, not substitute
radial scale. Partial-cull verification must map every named/root/TBN/hair
payload, not just counts, and topology-changing Session COW must retain old
bytes. These requirements remain validation gates before this slice is
reported complete; CPU Length itself remains an independent semantic gap.

The expanded Grow/Length tests now pass. The bounded host oracle uses current
Grow points, searches the whole polyline for keepParam shortening, and maps
every survivor's points/rest/width/hairT/IDs/root bindings/TBN and named planes
(including empty descriptors). Unsupported Length controls are rejected by
the oracle. Distinct rotated frames make wrong ID/frame associations visible.
Direct, fully staged asynchronous and Session checks cover scale, direct and
Session cutExtend, partial/total culling, source resample-to-3/to-2, and repeated
Length operators. The same Session changes from full output to one survivor
to empty while held old point/rest/width bytes remain identical and held
survivor payloads remain readable. Native retained-byte tests confirm exactly
36 extra bytes per survivor for TBN, without duplicate stable-ID storage.

Full CUDA-enabled rebuild and 157-test suite pass (156 passed, one expected
StormSurgery skip), 38.98 seconds. Native compactor and all four Grow/Length
failure modes pass memcheck/initcheck with zero errors. Expanded
CurveGrowSession passes memcheck. Its first initcheck run reported zero memory
errors but failed an existing UV-blend direct-generation assertion, after the
new Length checks had completed. A rerun passed. Added failure diagnostics
and rebuilt; ten further initcheck runs all pass with zero errors. This
intermittent assertion failure is retained as an unresolved reliability item,
not erased by the passing reruns or claimed fixed. CUDA-disabled rebuild and
focused CPU source/fan-out/reference-map checks pass; diff checks are clean.

Astra coordinated/reviewed; Terra implemented executor changes, Luna native
compaction/generation support and initial tests, and root completed strict
oracle/survivor/COW tests, budget/failure tests and final validation. The next
composition gap is mixed Grow/Length/Noise, including empty-survivor frame
inputs and conservative runtime admission. Broader graph branches/maps,
CPU Length/Noise parity, residency, cross-platform adapters and reliability
gates remain open; the overall goal is not complete.

### Mixed Grow/Length/Noise execution (2026-09-13, validating)

Matrix 26 (Grow 8, Length 3, Noise 3) admits ordered mixed Noise/Length/Width
suffixes after direct C3 Grow. Length still publishes its compacted geometry,
and later Noise publishes a private point overlay; Noise-before-Length feeds
the current overlay into compaction rather than discarding it early. Grow
must still directly follow CurveSource; broader DAG placement is not claimed.

Noise uses immutable original source-order frames with capture stable IDs.
Native Noise explicitly supports stable-ID lookup against a larger original
frame set after partial culling, so no frame duplication or host round trip
is needed. Empty geometry is passed empty frame views while original frame
owners remain retained and waited/recorded normally. Runtime Length admission
now includes Noise, its LUT/status scratch, and three simultaneous point
planes (old Noise output, new output, native staging). Non-Grow Length/Noise
also charges its original source-frame set. Checked arithmetic is retained.

Independent bounded host transforms now compose over a CPU Source/Grow/
constant-Width baseline, without executing CPU Length/Noise for expected
results. Tests cover both orders, partial/all culls before Noise, Noise on
both sides of an all-cull, repeated Length, nonempty repeated Noise, and
empty Source. Direct, Session and staged asynchronous paths compare full
geometry/rest/width/topology/IDs/roots/TBN/named payloads. A held Session
generation remains byte-identical across the mixed edits. Four new isolated
mixed-pipeline Length failure tests cover Pending admission, ledger-neutral
cap rejection, charged quarantine and a previous nonzero Noise publication.

Focused mixed, failure, metadata and existing NoiseSession checks pass.
CUDA-enabled and disabled builds pass; focused CPU checks pass. Full-suite
and sanitizer validation are in progress. Astra reviewed production and the
host oracle; Terra implemented the executor, Luna provided a read-only audit,
and root implemented/refined the tests and performs final validation.

Mixed-suffix verification is complete for the tested contract: full build and
161-test suite pass (160 passed, one expected StormSurgery skip), 40.53 seconds.
Eight mixed cases include a nonempty repeated-Noise chain, exercising the
old/new/staging point peak. Partial-cull and all-cull staged jobs now complete
through asynchronous final publication as well as source/operators. The
expanded CurveGrowSession, existing NoiseSession and four mixed-pipeline
failure modes each pass memcheck and initcheck with zero errors (12 commands).
CUDA-disabled rebuild and focused CPU tests pass; whitespace checks are clean.
The previously observed intermittent UV-blend assertion did not recur in
this validation, but its cause remains unresolved and it is not claimed fixed.

Remaining scope includes branch-aware mixed geometry values (not just ordered
chains), fuller parameter/map and operator composition coverage, CPU reference
semantic gaps, residency/renderer handoff, cross-platform execution adapters,
and the open reliability/performance gates. These results do not complete the
overall execution-graph goal or establish working Metal/Vulkan backends.

### Same-topology Noise value DAG — implementation in progress

The next slice admits Source/Noise/Width/WidthBlend graphs with unchanged
topology. Per-node point and width origins resolve actual predecessor values;
Noise owns a private point plane, Width/WidthBlend own private width planes,
and untouched planes remain immutable shared views. Terminal selection must
transfer ultimate owners, including Width inherited through repeated Noise,
and remain safe when asynchronous final publication is rejected and retried.
Noise tasks share a serialized lane while root-frame last-use events remain
mutable; the raw asynchronous API also requires an explicit in-flight guard.

WidthBlend currently requires equal point provenance in this CUDA slice.
Different producers with numerically equal points can be valid CPU inputs,
so this is a backend capability limitation, not generally invalid authoring.
Topology-changing branches and arbitrary mixed fan-in remain open.

New regression cases use a memoized, edge-following host oracle with the
independent native Noise math, not the semantically different CPU Noise op.
They cover shuffled branches, all terminal kinds, inherited point/width
owners, reversed blend operands, resampling, empty input, staged execution,
publication retry, resource dependencies, and byte-identical retained COW
generations. At this checkpoint the test objects compile; production review,
full build, execution tests and sanitizer verification are not yet complete.
Astra orchestrates/reviews; Terra implements; Luna independently audits and
validates; root integrates regression coverage and final verification.

Matrix 27 implementation and focused verification now pass (Noise v4,
Width v4, WidthBlend v3; Grow v8 and Length v3 unchanged). Thirteen DAG
variants pass direct and Session publication; selected cases also pass
staged asynchronous operators and rejected/retried final publication. The
separate NoiseSession regression pauses one independent Noise sibling at
its context callback, proves the other sibling's admission is rejected
without a callback, then releases and successfully retries/finalizes.

The first expanded memcheck exposed a real CUDA capture conflict: three API
errors (including error 900 during DeviceBuffer<float>::release from
OperatorRelayService::Finish, and invalidated EndCapture) preceded a DAG
Session publication failure. A repeat passed before any fix, establishing
intermittency rather than correctness. The WidthBlend cache used global
capture while unrelated worker threads could legally retire buffers. Its
capture window contains only one kernel over preallocated, cache-private
slots, protected by the cache mutex/in-flight lease. Capture now uses
thread-local mode, retaining same-thread unsafe-call restrictions without
forbidding unrelated thread cleanup. CUDA 13 runtime header capture-mode
documentation and the wrapper's allocation-free window were inspected.

After that fix the full CUDA build and 161-test suite pass (160 passed,
one expected StormSurgery skip), 40.60 seconds. CUDA-disabled build and
all three focused CPU source/fanout/reference-map tests also pass. Repeated
post-fix sanitizer verification is in progress. This does not prove that
the older intermittent UV-blend assertion had the same cause; that earlier
reliability finding remains open until separately established.

Post-fix sanitizer verification is complete for this slice: five memcheck
and five initcheck runs of the expanded CurveGrowSession, plus memcheck and
initcheck of NoiseSession and WidthDag, all pass with zero errors (14
commands). WidthDag retains its graph-cache capture/replay/eviction coverage.
Whitespace validation is clean. No claim is made that repeated tests replace
a deterministic capture/cleanup overlap regression; adding that targeted
host gate remains useful follow-up coverage.

Next implementation scope remains topology-changing branch values and
mixed fan-in, fuller parameter/map composition coverage, CPU semantic gaps,
residency/renderer handoff, Metal/Vulkan execution adapters, and the remaining
reliability/performance gates. This verified same-topology COW slice does not
complete the overall plan.

### Full geometry-value snapshots and topology-trunk branches — in progress

The previous verified slice advanced the execution graph but its node values
still reconstructed topology from a single source snapshot. The next change
replaces that assumption with per-node geometry, hairT, root bindings, curve
metadata, deformed state, and topology/named-owner identities. Noise and
Width inherit the complete input snapshot and replace only their private
written plane. Terminal selection must resolve the selected topology and
named owner as well as its point/width overlays.

The next admitted family has one direct Grow or Length topology trunk that
dominates all Noise/Width/WidthBlend descendants. Original Source terminal
selection remains valid. Source-side sibling branches, independent topology
producers, and arbitrary topology joins remain unimplemented; they need
independent retained topology/named-owner bundles, not global lookup. This
boundary is an incremental implementation checkpoint, not a narrowed goal.

Admission must charge every retained DAG Noise output plus native staging,
instead of reusing the linear chain's three-plane bound. New tests add 24
topology-trunk variants to the 13 same-topology cases, with direct/Session/
staged publication, retries, all terminal kinds, partial/all culling,
shortening, empty source, resampling, seven Noise outputs, COW retention and
CatmullRom-vs-BSpline terminal metadata checks. Implementation and validation
are in progress; matrix 28 expectations are not yet verified.

Matrix 28 implementation now passes focused execution and metadata tests:
Grow v9, Length v4, Noise v5, Width v5, WidthBlend v4. Full node snapshots
are inherited by Noise/Width/Blend; Grow and Length seed their snapshots
after native/named completion. The admitted single-trunk family uses explicit
topology/named identities validated against its uniquely retained owner;
this is not yet a general shared topology/named bundle for arbitrary branches.
Predecessor waits require the selected native owner, and direct Width/Blend
proof loss now retains Grow/compaction storage as well as overlay buffers.

Literal-Length admission uses checked arithmetic for max(3, NoiseCount+1)
point planes in value DAGs, plus one serialized Noise scratch/frame packet.
Linear paths retain their existing three-plane bound. A final classification
review caught and prevented rejection of previously supported Noise-before-
Length chains. Four added regressions explicitly retain those linear paths
(scale, partial/all cull, and an upstream Width), bringing this fixture to
41 graph variants. The sibling-Noise admission guard is also exercised after
Length has culled the source to two curves.

Both full builds (CUDA enabled/disabled) pass, as do the three focused GPU
tests and three focused CPU tests. CurveGrowSession and NoiseSession each
pass memcheck and initcheck with zero errors (four commands). Full-suite
verification is running. No CPU Noise/Length semantic parity or unrestricted
topology fan-in is inferred from these independent-oracle regressions.

Matrix 28 full-suite verification is complete: 160 passed and one expected
StormSurgery skip (161 registered tests), 41.27 seconds. The four sanitizer
commands and both builds above are on the final implementation; whitespace
checks are clean. Astra orchestrated and reviewed the production changes,
Terra implemented them, Luna added/audited the topology-sibling guard test,
and root integrated the 41-variant oracle coverage and ran final verification.

The next ownership step is independent topology/named bundles for branches
that bypass a topology trunk and for multiple topology producers. Source
ownership must remain stable while Grow consumes its input bundle; merely
relaxing the domination check would introduce cross-task ownership/context
races. Arbitrary mixed fan-in, broader parameter/map coverage, CPU semantic
parity, renderer/residency integration, non-CUDA platform implementations,
and the open reliability/performance gates still require implementation and
evidence. The overall goal remains open.

### Independent source/transformed owner bundles — in progress

Matrix 28's verified snapshots still depended on a uniquely retained topology
trunk. The next change gives geometry values shared topology-owner and
named-plane-owner bundles. The source bundle retains native Source/resampler
and original private TBN; Grow borrows that stable shared lifetime instead
of moving storage out from under source-side consumers. Accepted Grow/Length
results get distinct bundles after native/named proof. Publication selects
and transfers the terminal's actual bundle only after the all-task join.

The next admitted family permits source-side branches alongside one direct
Grow or Length producer. Same point/topology provenance is still required
for WidthBlend. Multiple independent topology producers remain future work
until their candidate storage, contexts, counts, and admission are wired;
this work does not silently relax checks on global mutable state.

Tests add 26 source-side Noise/Width/Blend branch cases and seven no-Noise
Width/Blend cases, for 74 graph variants including the previous 41. Coverage
includes selecting source-side output while a sibling culls every curve,
resampling, empty input, selected-ancestry TBN/basis contracts, publication
retry and old-generation COW retention. Production implementation and
runtime verification of these new cases are not complete at this checkpoint.

Concurrency review caught two issues during this refactor: a mutable global
named-bundle selector would recreate cross-branch context races, and scanning
all topology-owner slots for source frames or failure proof can race with a
sibling publishing its output slot. Source-frame access must use a stable
source reference; named transforms must use explicit predecessor references;
failure proof must remain with the active producer rather than unsafely
reading a sibling's mutable native pointer. These are integration blockers,
not accepted limitations. A deterministic regression now pauses a source-side
Width launcher while Length completes, then checks source-owned publication;
the launcher gate is released before its async future joins on failure.

### Matrix 29 owner-bundle integration — validation checkpoint

The production bundle integration is implemented. Source/resampler/private
frame ownership stays in a stable shared source bundle; accepted Grow/Length
native results and named planes live in distinct bundles. Noise/Width/Blend
copy their predecessor's topology and named owner references while retaining
private changed-point/width planes. Named transforms take explicit input
references, and terminal transfer uses a retry-safe selected-owner identity.
Source private TBN remains unpublished; existing linear Grow-to-Length frame
propagation remains intact. Direct proof-loss handling quarantines all retained
native, private-frame and named bundles. Async failure proof does not inspect
a sibling's mutable native candidate.

Integration verification caught and fixed a missing relay frame field and a
Length predecessor wait that still consulted the old global compactor after
ownership moved into a bundle. Source waits now prefer the stable bundle.
The three focused CUDA tests pass, including 74 graph variants and the
deterministically held source-side Width launcher. An additional regression
holds a Grow generation across source-side and transformed branch publication,
checks its complete payload after each update, and verifies byte-identical
points/rest/widths and published TBN after both updates.

Both full builds (CUDA enabled/disabled) and three focused CPU tests pass.
CurveGrowSession (including the additional held-Grow regression) and
NoiseSession each pass memcheck and initcheck with zero errors: four commands.
The first full suite had 159 passes, one expected StormSurgery skip, and one
failure: WidthDag still expected a now-admitted ReferenceSource source-side
Width to be rejected. That case is being replaced with positive selected-
branch payload checks; final full-suite verification remains pending.

Matrix 29 versions are Source 2, ReferenceSource 2, Grow 10, Length 5,
Noise 6, Width 6, and WidthBlend 5. New branched topology support is bounded
to one direct Grow OR Length producer, alongside source-side and transformed
Noise/Width/Blend consumers, including no-Noise Width branches. Existing linear
Grow-to-Length and repeated/mixed Length chains remain supported. Multiple
topology producers in value DAGs and arbitrary topology fan-in remain open.
The same-point/topology-provenance WidthBlend restriction is a CUDA capability
limit, not a claim of invalid authoring. No CPU Noise/Length semantic parity,
Metal/Vulkan implementation, or completion of the overall plan is inferred.

### Matrix 29 final verification

The final full suite passes: 160 passed, one expected StormSurgery skip,
161 registered tests, 41.49 seconds. Both full builds pass; the CUDA-disabled
CPU Source/Fanout/ReferenceMap checks pass. CurveGrowSession, NoiseSession,
and WidthDag each pass memcheck and initcheck with zero errors (six commands).
Whitespace checks are clean.

The two obsolete source-side-branch rejection assertions were replaced with
positive payload checks. ReferenceSource now has direct and Session terminal
swaps between source-side Width and transformed Length/Width, retaining the
old source generation and checking geometry, rest, widths, IDs, offsets,
hairT, root bindings, and intentionally empty published TBN. Its bounded
oracle preserves the full source payload and independently scales current
points; integration corrected its reader-stream declaration, uniform empty-
offset handling, and VtArray-to-vector conversion before final verification.
Existing ReferenceSource authored-named-plane/root-frame restrictions and
Width-consumer requirements remain; named bundle coverage comes from the
CurveSource variants. The CurveSource selected-Length/source-Width sibling
case also executes and validates payload; unrelated negative checks remain.

Astra orchestrated and reviewed, Terra implemented owner changes and branch
tests, Luna added the concurrency/retained-Grow checks, and root integrated
the 74-variant matrix, reviewed corrections, and ran verification. This closes
the bounded source/transformed COW owner-bundle slice, not the overall goal.
Next graph work is multiple topology-producing value-DAG branches and their
per-candidate contexts/counts/admission, followed by broader valid fan-in and
parameter/map coverage. Renderer/residency integration, non-CUDA adapters,
cross-platform proof, and outstanding reliability/performance gates remain
open as described above.

### Multiple Length-producing value DAGs — in progress

The next implementation removes the single-Length-trunk restriction for
CurveSource value DAGs without Grow: independent and nested Length nodes
consume explicit predecessor snapshots, including Noise and Width outputs.
This requires relay-local result publication rather than a temporary shared
global compactor/context, exact predecessor frame ownership, and checked
memory bounds for every retained compaction and named-plane bundle. A shared
async admission guard must match the serialized Noise/Length workspace lane;
independent native candidates alone do not prove safe concurrent mutation of
input-plane last-use bookkeeping. Serialization does not reduce the number
of retained immutable outputs that admission must charge.

Root added 32 graph variants to the recursive independent-oracle fixture,
bringing it to 106 including the previous 74. They select independent/nested
Length and source-side terminals, include resampling, empty sources, culling,
no-Noise branches, a Width predecessor, and six retained Length results.
The fixture object compiles; production integration and runtime verification
are pending. Astra is coordinating Terra's executor/admission changes and
Luna's deterministic raw-async Length/Noise guard/retry tests. Existing
Grow-containing families remain unchanged until their shared candidate state
can be separated; this checkpoint does not declare multi-Grow branches or
arbitrary topology fan-in implemented.

ReferenceSource's equivalent multi-Length/Width family is also being included
deliberately, retaining its existing authored-plane/root-frame restrictions.
Its former multiple-Length rejection test now has a full-payload direct and
Session oracle for two successive half-length scales, preserving a prior
source-side publication. All changed test objects compile. Runtime evidence
is still pending; candidate version expectations are CUDA matrix 30,
Length 6, Noise 7, and Grow 11 (the latter for the shared raw-async admission
contract, not additional Grow graph compositions).

Initial runtime verification passed the 106-variant fixture, ReferenceSource
chain, and shared-lane contention tests; metadata testing caught a stale
matrix-version expectation, corrected to 30. Both full builds and the three
CPU checks pass. The initial CurveGrowSession memcheck/initcheck runs report
zero errors. Final verification is pending a reviewed extension to all
no-Grow Length value DAGs, including a single non-direct Length: four more
source-side/selected-trunk cases bring the fixture to 110 variants. Former
ordered single Noise-to-Length cases now expect value-DAG metadata while
retaining their full payload assertions. Review also identified direct
proof-loss retention of predecessor point/width overlays as necessary;
the common quarantine helper is being extended before final verification.

### Matrix 30 final verification — predecessor-owned Length DAGs

Matrix 30 implements no-Grow Length value DAGs rooted at CurveSource or the
supported ReferenceSource contract: independent and nested Lengths consume
their actual Source/Noise/Width/Length predecessor snapshots. A single
non-direct Length is included, with and without Noise. Grow-containing
families keep their existing composition bounds. Length results publish
directly into per-node native/named bundles; async completion does not first
overwrite shared global geometry, roots or compaction storage. Topology and
deformed metadata inherit the actual predecessor, and direct frame selection
uses the predecessor's bundle.

Admission charges all retained Length geometry/compactor/named output bundles
with checked count arithmetic. The shared raw-async Grow/Noise/Length lane
remains held through scratch teardown and slot cleanup, releasing before the
completion callback; unsafe work retains its owners. Deterministic tests prove
Length-versus-Length and both Length/Noise rejection-and-retry directions.
The direct quarantine helper now includes per-node point and width overlays,
so a Length borrowing a Noise/Width output cannot release that input after
losing completion proof. This is not a claim of concurrent topology-kernel
execution; the retained outputs coexist, while mutable workspace work remains
serialized.

The final recursive-oracle fixture has 112 graph variants, including the
six retained Length outputs, and still checks direct, Session, staged async
publication/retry, full payload, source/grown frame contracts and retained COW
data. ReferenceSource also has explicit two-Length direct/Session full-payload
coverage. Review fixed the overlapping legacy direct-source guard for
no-Noise Width-to-Length graphs. Metadata assertions now distinguish unary
task DAGs (no WidthBlend) from value fan-in DAGs; the four previously ordered
Noise-to-Length cases keep full payload verification on the new unary-DAG
lowering.

Final evidence: both CUDA-enabled and CUDA-disabled full builds pass; the
three CPU Source/Fanout/ReferenceMap checks pass; the full suite has 160 passes
and one expected StormSurgery skip (161 registered), 42.43 seconds.
CurveGrowSession, NoiseSession and WidthDag each pass memcheck and initcheck
on the final source with zero errors (six commands). Whitespace checks pass.
Versions are CUDA matrix 30, Grow 11, Length 6, Noise 7; other rows unchanged.
Astra coordinated/reviewed, Terra implemented, Luna added guard tests, and
root integrated oracle coverage and ran final validation.

The overall goal remains open. Next topology work is per-candidate Grow
ownership/admission for branched Grow producers and Grow consuming transformed
or culled predecessors, followed by joins with different point/topology
provenance. Broader parameter/map proof, retirement/reliability/performance
gates, renderer/residency integration, and non-CUDA implementations and
cross-platform validation remain outstanding. No CPU Noise/Length semantic
parity or Metal/Vulkan implementation is claimed by this checkpoint.

### Branched direct-source Grow ownership — in progress

The next implementation gives each Grow invocation its own native and named
candidate packet, rather than selecting shared job-level Grow fields. Multiple
Grow nodes directly consuming CurveSource may then coexist with independent
source branches and Length/Noise/Width/Blend descendants. Completion must
publish only that candidate's geometry, named planes, roots and metadata;
direct and async failures must retain the corresponding inputs and outputs.
The shared geometry-stage admission lane remains in force through teardown.

Root added 34 graph variants (146 total): two Grow branches with different
segment counts/lengths, selecting either topology or downstream Noise/Width,
two downstream Lengths with scale/cull, source-side selections, resampling,
empty input and no-Noise graphs. Six retained Grow outputs are exercised both
with and without Length to cover runtime-refined and static admission paths.
Luna added a held-Grow raw-async test rejecting sibling Grow/Length/Noise,
then retrying and selecting the six-CV-per-curve Grow output. Runtime evidence
is pending while Terra implements the per-candidate owner path and counted
Grow/Length memory bounds under Astra's review.

Grow after a culled/transformed predecessor is not silently admitted: native
Grow indexes T/B/N by curve ordinal and requires frame counts equal to input
curve count. That family needs explicit aligned-frame selection/gather and
its input lifetime proof, separate from direct-source multi-Grow ownership.
Different-provenance joins and the broader open plan requirements remain.

Four isolated MultiGrow failure tests now register native/named callback
installation and completion failures on GrowB after GrowA has accepted its
output. They retain an older six-CV GrowB publication, verify budget rejection
before submission, and check poisoned-workspace/quarantine accounting and
readability of the old COW generation. All four use the existing GPU resource
lock, timeout and skip contract. Test objects compile; native execution is
pending. Review additionally requires direct Grow launch failures to poison
and retain the candidate when BeginFresh reports unproven work, and runtime
reservation to charge native status storage for every retained Grow producer.

### Matrix 31 final verification — multiple direct-source Grow values

Multiple Grow nodes directly rooted at CurveSource are implemented, alongside
supported Length/Noise/Width/Blend descendants and independent source-side
branches. Ordinary Grow cardinality is no longer accidentally constrained by
the fused Scatter/Grow validator; Scatter keeps its one-Grow restriction.
Each value-DAG Grow uses its own native/named candidate in direct and async
execution, publishing an independent topology/named bundle from the exact
input snapshot. Source owners are shared without being stolen. Async failure
proof inspects that relay's candidate, and direct launch/commit/accept failures
preserve and poison only when native work is unproven. Candidate scratch is
destroyed before releasing the shared geometry-stage lane.

The static no-Length estimator sums every Grow output/status/frame packet.
Length runtime refinement now charges retained G+L geometries, source plus
G+L named bundles, private source plus G+L frame packets, every Grow map, and
checked per-Grow device/pinned status storage. The six-Grow fixtures exercise
both static and runtime-refined paths; geometry-stage serialization does not
remove any retained-output charge. Native direct-source Grow copies the
source TBN verbatim, so original source-frame lookup remains valid for Noise
on these Grow branches even when their lengths, lifts or segment counts differ.

All eight focused tests pass, including the 146-variant recursive oracle,
held-Grow rejection/retry, and four second-Grow quarantine cases. The full
suite passes: 164 passed and one expected StormSurgery skip, 165 registered,
45.92 seconds. Both full builds (CUDA enabled/disabled) and the three CPU
Source/Fanout/ReferenceMap checks pass. CurveGrowSession, NoiseSession and
WidthDag each pass memcheck and initcheck with zero errors on the final source
(six commands). Whitespace checks pass. Versions are CUDA matrix 31, Grow 12,
Length 7; other rows unchanged. Astra orchestrated/reviewed, Terra implemented,
Luna added guard/failure tests, and root integrated oracle coverage and ran
verification.

The overall goal remains open. Multiple direct-source Grow is no longer a
remaining gap. Next is Grow from exact non-source predecessor snapshots:
Noise/Width retaining Source topology can reuse aligned private source axes;
Grow-to-Grow can use the predecessor's published TBN; Source-origin Length
culling needs survivor-aligned private frame selection/gather and lifetime
proof. These input families still require implementation and validation.
Different-provenance WidthBlend joins require full non-width equality proof;
broader operator/parameter/map coverage, CPU semantic gaps, renderer/residency,
non-CUDA adapters, cross-platform validation, and reliability/performance
gates remain open. No general topology fan-in or Metal/Vulkan implementation
is claimed by this milestone.

### Grow from transformed and culled predecessors — in progress

The next implementation extends native Grow with an optional stable-ID frame
domain. Without it, the ordinal-aligned API remains unchanged; with it, Grow
looks up each current curve ID in sorted source frame IDs on device. Malformed
domains, missing IDs, and duplicate/unsorted IDs must fail closed. This avoids
geometry readback and a blocking frame-gather finish in async execution.
Source private frames remain unpublished until Grow produces its own output.

Executor input ownership must retain both the source-frame bundle and the
actual predecessor geometry/topology/named data. Async Grow additionally needs
the job lifetime for unique point/width overlays; successful completion must
break that ownership cycle, while lost proof retains it. Direct failure must
quarantine borrowed overlays before candidate teardown. Terra is implementing
native mapping and executor ownership under Astra's review; Luna is adding
native mapping validation and an async culled-predecessor regression.

Root added 45 graph variants, bringing the recursive-oracle fixture to 187
(runtime-counted; earlier narrative counts overstated the baseline by four).
These cover Grow after Width/Noise/Grow/Length, partial/all cull, resampling,
empty input, no-Noise branches, and selecting unchanged source-side siblings.
For non-source Grow, the oracle constructs a synthetic C3 input from the exact
host predecessor (points/rest/widths, IDs/spans, root bindings/TBN and named
planes), then invokes only the independent CPU Grow operation. It does not
substitute CPU Noise/Length for the established bounded host transforms.
Test object compilation caught and corrected the constant-plane domain enum
name. Runtime validation also corrected an obsolete Noise-before-Grow rejection
and a no-Noise fixture helper that failed to rewire a removed terminal. The
187-variant fixture passes (7.10 seconds), including direct, Session and staged
async execution and held-generation COW payload checks. Native Grow, NoiseSession,
WidthDag and capability metadata checks passed the initial focused run. Full
CUDA-enabled and disabled builds and three CPU Source/Fanout/ReferenceMap checks
pass. Final full-suite and sanitizer verification remain pending, as does new
failure-injection coverage for Grow borrowing transformed/culled predecessors.

### Parallel work queue after the Matrix 32 checkpoint

The user requested 10–20 Terra/Luna workers under Astra orchestration. The
session permits four active agents including root, so Astra coordinates the
following 16 bounded tasks in two-worker batches while root integrates and
validates. This is a queue, not a claim of 16 simultaneous agents or completed
scope. Production changes wait for the current validation freeze to finish.

1. Borrowed Noise-overlay and culled-predecessor Grow failure fixtures.
2. Eight isolated failure registrations and GPU resource-lock properties.
3. Async full non-width equality primitive for distinct-provenance WidthBlend.
4. Native equality positive, mismatch, empty and failure tests.
5. Distinct-provenance WidthBlend admission, relay proof and memory integration.
6. Independent equal-branch Blend, COW and retry graph tests.
7. Exact-predecessor RBF candidate and per-value point ownership.
8. Deform branch/non-source input payload and failure regressions.
9. Scatter/Grow output ownership integration with the value executor.
10. Scatter descendant Length/Noise/Width selected-terminal tests.
11. ReferenceSource authored named-channel leased-bundle transport.
12. Reference named-channel branch and retained-generation regressions.
13. CPU Length compaction/reparameterization semantic corrections.
14. CPU/CUDA bounded Length parity matrix.
15. Deterministic reproduction/diagnostics for the historical UV assertion,
    followed by a cause-grounded correction if reproduced.
16. Admission, retirement and cancellation stress with budget-drift and
    bounded-progress assertions.

Each implementation must honor actual source/rest/binding semantics; blanket
validator relaxation is not an implementation. Renderer residency, remaining
operator/parameter/map requirements and platform/reliability gates still need
their own evidence. Metal/Vulkan implementation is excluded by the current
user scope clarification above.
The queue is not exhaustive: replacing current application registry/cache/
capture mutexes with scheduled ownership remains required by the original
non-negotiable architecture. Fairness, latency, soak and stock-SDK GPU-residency
claims likewise require direct evidence, not inference from the DAG tests.

### Matrix 32 validation follow-up — final suite pending

All eight memcheck/initcheck combinations for CurveGrowSession, NoiseSession,
WidthDag and native CurveGrow report zero errors. Eight new isolated Grow
failure tests initially passed; the first expanded 173-test suite exposed one
test-isolation race in CulledNoiseGrowNativeFailure's strict cap-ledger check
(171 passed, one failed, one expected StormSurgery skip). Its temporary Noise
witness generation was destroyed immediately before the budget snapshots,
allowing asynchronous retirement to change those snapshots. The witness's
plan, workspace, generation, diagnostics and lease now remain alive throughout
the assertions. No budget assertion was relaxed and no production synchronization
was added. Repeated eight-case and full-suite verification are pending on this
test-only correction; the earlier failed suite is not claimed as a pass.

### Matrix 32 verified — exact-predecessor Grow and stable-ID frames

CUDA matrix 32 / Grow 13 now supports Grow consuming supported Width, Noise,
Grow and Length predecessor snapshots, including partial/all cull, resampling
and empty geometry. Source private TBN is an immutable stable-ID domain;
native Grow looks up each active ID on device and copies its mapped frame to
the output. Ordinal native callers retain the empty-map ABI. Full supplied
maps are validated, including unused tails: duplicate, unsorted or missing IDs
fail semantic commit; malformed pointer/count/trio inputs fail preflight.
Valid mapped empty domains and malformed empty-domain inputs have native tests.
No geometry D2H fallback or blocking frame-gather finish was introduced.

Grow retains both the actual predecessor topology/named payload and original
source-frame owner. Async candidates additionally retain the job for unique
point/width overlays, release that cycle only after proved completion, and
quarantine lost-proof ownership. The direct named-candidate failure path now
also honors its sticky unproven-work state. Compile and preparation use the
same ancestry-specific source-rest check: a downstream or unrelated Grow
cannot hide missing authored rest from a Source-origin Noise node.

Final evidence: the 187-variant recursive graph fixture passes (the measured
count corrects earlier inferred 191/146 narrative totals); staged culled
Length→Grow checks exact survivor IDs and TBN; held generations retain all
checked payloads. Eight new borrowed-overlay/cull Grow failure tests pass
20 repetitions each (160 passes, 75.49 seconds) after the witness-lifetime
test correction. The full expanded suite passes: 172 passed, one expected
StormSurgery skip, 173 registered, 52.45 seconds. CUDA-enabled and disabled
builds and the three CPU Source/Fanout/ReferenceMap checks pass. Native Grow,
CurveGrowSession, NoiseSession and WidthDag each pass memcheck and initcheck
with zero errors (eight combinations). Whitespace checks pass.

The overall goal remains OPEN. The next pair is implementing a separate full
non-width equality primitive and its native tests in new files; those files
were not built or included in this Matrix 32 verification. Distinct-provenance
WidthBlend integration remains pending. The 16-task queue and architectural,
renderer/residency and reliability gates remain active; non-CUDA backend
implementation stays excluded by the user's scope clarification.

### Vulkan scope restored — ten-task worker queue

The user's newer request restores Vulkan implementation (Metal remains out).
This supersedes the exclusion in the historical Matrix 32 checkpoint. The
four-active-agent limit permits root + Astra + two Terra/Luna workers, so ten
Vulkan tasks are queued in batches, not represented as ten active agents:

1. Native loader/device/compute-queue discovery and backend contract.
2. Charged Vulkan buffer/memory ownership.
3. Completion tokens and asynchronous retirement.
4. Reproducible shader compilation/SPIR-V toolchain.
5. CurveSource upload and immutable native generation.
6. Width compute kernel and COW output ownership.
7. Immutable plan routing and truthful capabilities.
8. Owner-private asynchronous job submission.
9. Payload, ownership, cancellation and failure tests.
10. Source/Width Session integration and real-device validation.

This first end-to-end foundation is not the whole Vulkan implementation.
Remaining supported operators and graph compositions still require native
implementation and evidence. No CUDA shim or CPU fallback satisfies it.
The local loader and NVIDIA/lavapipe ICD files exist, but headers, the Vulkan
pkg-config package, vulkaninfo and GLSL compiler commands were not found.
Device availability is not yet proven merely by installed ICD files.
Provisioning a pinned development toolchain is therefore part of the work.
Synchronization design follows the native rules in the
[Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
and [queue guide](https://docs.vulkan.org/guide/latest/queues.html), not assumed
equivalence with CUDA events.

Local toolchain provisioning is now verified: four exact ARM64 package versions
and SHA256 hashes are recorded in `scripts/vulkan-toolchain-manifest.txt`;
root verified all downloaded hashes. Extracted tools/headers live under the
ignored `.vulkan-toolchain/root` directory, without system installation.
Root independently ran vulkaninfo: NVIDIA GB10 API 1.4.312, proprietary driver
580.173.02, and llvmpipe API 1.4.318 enumerate successfully. This proves native
device discovery, not application compute execution. The local development
package's `libvulkan.so` symlink lacks its runtime target, so build integration
must link the installed loader explicitly rather than assume that symlink is
usable. Shader execution, generation ownership and the native factory remain
unimplemented/unverified at this checkpoint.

The manifest-driven `scripts/bootstrap-vulkan-toolchain.sh` now verifies exact
ARM64 package names, versions, architecture and SHA256 before extraction;
root's cached rerun and shell syntax check pass. Usage lives in the tracked
`scripts/vulkan-toolchain.md`, not only in the ignored cache.
The first scalar Width shader and descriptor/push-constant ABI are implemented
under `libs/usdGen/usdGen/vulkan/shaders/`. Compilation for Vulkan 1.2 and
SPIR-V validation pass. Its initial scalar set/multiply path deliberately has
no claim of full parameter/map/profile coverage or executed device parity.
The native dispatch test and reusable Vulkan ownership/executor integration
remain required next steps.

### First Vulkan hardware compute proof

The opt-in `USDGEN_BUILD_VULKAN_TESTS` target builds and validates SPIR-V,
links the host Vulkan loader, and uses the shared GPU test resource lock.
Root completed the native Width probe and ran it successfully on the NVIDIA
GB10 (0.51 seconds). It creates a real Vulkan device/queue/pipeline/buffers,
uses explicit host/compute visibility barriers, and waits on a bounded native
fence before test-only readback. Set/multiply output, a 257-point partial
workgroup, overflow/invalid-input status, empty-control preflight, output
canary, and input byte preservation (including NaN bits) are checked. An
unproven fence causes isolated-test failure without destroying potentially
in-flight handles; this is not a production retirement implementation.

This proves scalar Width hardware dispatch and immutable input storage, not
full generation COW, resource charging, asynchronous owner integration,
CurveSource upload, Session execution, or full Width controls. The production
Vulkan factory remains unavailable until those implementations exist. Repeated
hardware runs now pass 20 consecutive repetitions (8.50 seconds). Further
backend integration and production ownership validation remain pending.

### Full non-width comparator foundation verified; integration open

The new CUDA `gpu/nonWidthCompare` primitive compares independently owned
non-width geometry, optional rest/hair/root/TBN/mask views, ordered named
payload metadata/data, and chunk descriptors. Floats compare numerically
(signed zero equal, NaN unequal), while widths are excluded. Supplied frame
IDs are independently validated against each geometry domain even when the
other input is already unequal. Native scalar status and lifetime retention
follow an enqueue/parent-proof/host-commit contract; no geometry readback is
used. Native malformed-view and proved-unequal results remain distinct.

Root corrected two test-only mutable-to-const DeviceView assignments during
compilation. The native test passes (0.48 seconds), memcheck and initcheck each
report zero errors, and both full CUDA-enabled/disabled builds pass. Full CUDA
suite: 173 passed plus one expected StormSurgery skip, 174 registered,
51.21 seconds. WidthBlend graph admission has NOT been relaxed, capability
versions remain Matrix 32 / Grow 13, and comparator integration, explicit
lost-proof injection and executor memory estimates still require work.

The next Vulkan device/context/charged-buffer files are separate unbuilt work
at this checkpoint. They must prove actual allocation-size accounting,
scheduled queue ownership, durable physical-device/resource-key identity,
and retained context plus charge on lost completion before factory/Session
integration. No production Vulkan availability is claimed by the probe or
this new-file foundation.

Comparator follow-up: root added strict full-budget rejection checks (no
submitted work or ledger delta) and simulated parent-lost-proof quarantine
with a real borrowed native owner. Its weak lifetime and comparator charges
survive candidate destruction and a later unrelated successful stream fence.
The extended native fixture passes 20 repetitions (8.88 seconds), memcheck
and initcheck remain zero-error. These are explicit primitive-state tests;
executor callback failure injection still belongs to integration work.

Adding Vulkan `.cpp` files exposed the core's recursive source glob pulling
native headers into ordinary CUDA/CPU builds. Root excluded that directory
from the generic core and wired an explicit opt-in `usdGenVulkanNative` target
instead. CPU-only build passes after this isolation change. The Vulkan native
target remains a foundation under test, not a registered backend.

Vulkan owner-library compilation now passes. `DeviceContext` retains the
factory's actual native lifetime and physical UUID, validates compute-family
support, and treats the matching instance/device/queue association as an
explicit trusted factory contract. The factory-owned resource key is not yet
a validated cross-context UUID registry. `ChargedBuffer` reserves the actual
`VkMemoryRequirements::size` before allocation, rolls back through an already
allocated owner, and marks submissions before queue submission. Fence polling
is nonblocking; ordinary `VK_NOT_READY` preserves state and charge. Successful
proof allows holder reuse; lost proof retains context, submission and charge
permanently. Native resource tests and cross-context registry/Session
integration remain unverified; compilation is not lifecycle proof.

### Vulkan resource lifecycle hardware proof

Root fixed the resource-test include path, built the target, and verified both
normal and lost-proof modes on NVIDIA GB10. All three Vulkan probes (resource
lifecycle, quarantine, scalar Width) then passed 20 consecutive repetitions
each: 60 successful executions in 25.11 seconds. Full CUDA-enabled and
CUDA-disabled builds also pass after the native-source isolation change.

The lifecycle fixture checks exact native allocation-size charging, full-cap
and unsupported-memory rollback, separately owned output with held input bytes
unchanged, two actual fill submissions, pending nonblocking polls, fence reuse,
and rejection of already-signaled fences. The quarantine fixture retains the
actual device/command/fence owner and its allocation charge even after a later
fence succeeds. This is native resource/COW-storage evidence, not full published
generation COW or production Session availability. Cross-context device identity,
scheduled queue integration, generation publication, and executor integration
remain open. No Vulkan validation-layer run is claimed.

The user requested ten additional Luna/Terra Vulkan workers with Astra
orchestration. The session has four active-agent slots including root, so work
is dispatched in dependency-aware batches with at most two implementation or
validation workers alongside root and Astra, not ten simultaneous workers.
Metal remains excluded.

Post-isolation full CUDA regression also passes: 173 passed, one expected
StormSurgery skip, 174 registered, 93.72 seconds. This run includes the extended
non-width comparator cap/quarantine tests. ScenePublication took 20.13 seconds
but passed; no latency improvement is inferred from this correctness run.

### Vulkan worker queue and independent lifecycle negatives

Astra's ten dependency-aware assignments are: (1) owner-confined UUID identity
and immutable resource-pool entries, (2) lifecycle negatives, (3) charged Source
uploads and immutable generation owners, (4) full Source COW tests, (5) reusable
Width pipeline/submission, (6) Width result/status/COW cases, (7) nonblocking
Source/Width publication job, (8) cancellation/quarantine/retention tests,
(9) real backend adapter, and (10) Session routing/revision tests. Backend
availability remains gated on actual end-to-end support. Thread creation also
hit a retained-thread limit, so available Luna/Terra workers are reused rather
than claiming ten fresh concurrent agents.

The lifecycle worker returned an incomplete null-input-only draft twice. Root
completed the fixture and extracted a shared real native-device test owner.
The new cases exercise ten invalid context configurations, immutable pool
configuration mismatch, invalid buffer construction, null fence/lifetime,
double arm, pending poll, real submission-lifetime retention and release,
and a separate destructor-triggered quarantine whose charge and native owners
survive later fence completion. All five Vulkan probes pass together on GB10
(2.10 seconds). These remain native ownership tests, not production Session
or complete cross-platform execution evidence.

Both independent lifecycle cases additionally pass 20 consecutive repetitions
each (40 executions, 16.55 seconds). Root then added immutable logical buffer
extent, usage and context accessors, distinct from native allocation-size
accounting, for the upcoming Width pipeline's input contract. Metadata checks
are covered by the lifecycle fixture; the rebuilt five-probe suite passes.

### Vulkan factory and reusable Width native implementation

The matching `vulkan-validationlayers` 1.4.328.1-1~1 arm64 package is now pinned
by SHA256 in the workspace bootstrap manifest and extracted without system
installation. Loader diagnostics explicitly confirmed insertion of the local
Khronos layer into both instance and NVIDIA device dispatch. The repeatable
`scripts/run-vulkan-validation.sh` enables core and synchronization validation
with submit-time checks; CTest rejects Validation Error, VUID and SYNC-HAZARD
diagnostics even if the process exits zero. Configuration follows the matching
[layer documentation](https://vulkan.lunarg.com/doc/view/1.4.328.1/linux/khronos_validation_layer.html).
This is not GPU-assisted shader instrumentation. Initial five-probe validation
run passes with no validation diagnostics (2.12 seconds).

Terra's `DeviceFactory` publishes immutable UUID/config/pool entries through a
scheduled owner. Its registry implementation is in one shared factory DSO,
not duplicated in static native consumers. Reply command admission is reserved
before registry handoff; resolved/rejected replies use the caller's owner.
Cancellation has a documented thread-neutral terminal callback exception when
reply posting is unavailable. Native lifetime captures are excluded from the
resolver, and native context validation is a separate operation. Existing
low-level trusted `DeviceContext::Create` remains available; no claim that
arbitrary externally supplied raw handles prove their own association is made.

Root's factory test passes with API/synchronization validation: independent
instances of the same GB10 UUID share one pool, 32 requests initiated in an
owner frame converge on one entry, configuration mismatches reject, different
UUID entries do not alias, mismatched native UUIDs cannot construct a context,
and allocations from two native contexts charge the same exact ledger.
This does not yet verify Windows DSO loading or all cancellation fault paths.

Astra completed the reusable `WidthPipeline` after the implementation worker
returned without code. It owns a native shader/pipeline, allocates fresh
charged output/status per candidate, retains immutable input, polls a fence
without blocking, and reads back only scalar semantic status. A preallocated
retained-state holder preserves all native owners and allocation charges on
lost proof. It checks the fixed 256-lane shader's device limits and accepts only
trusted, externally validated shader bytecode matching the compiled ABI.

Root's hardware fixture passes normal and quarantine modes with validation
(1.00 second): 257-value replace followed by multiply using the first device
output, held input and intermediate bytes unchanged, scalar semantic failure
without publication, empty-control handling, strict full-cap rollback, and
retained whole-candidate charge after lost proof and a later queue fence.
Geometry readback exists only in the test fixture. Full Source payload upload,
generation publication, graph/Session integration, broader Width controls and
the remaining CUDA graph coverage remain open; production Vulkan availability
is not enabled. CUDA-enabled and disabled full builds still pass.

All eight Vulkan fixtures then passed 20 consecutive repetitions with API and
synchronization validation enabled: 160 executions in 81.52 seconds. Root
subsequently extended factory tests with full reply-ingress rejection and
actual callback-state lifetime checks, and Width tests with wrong-context,
wrong-usage and explicit arithmetic-overflow status rejection. The rebuilt
eight-test validation suite passes again (3.62 seconds). These added cases were
not part of the preceding 160-run count. No validation diagnostics were emitted.

### Shared device metadata preflight for CUDA and Vulkan

Root extracted the existing neutral geometry/channel validation into
`ValidateUsdGenDeviceMetadata`, so a backend can validate before native
allocation without manufacturing a ready owner. `UsdGenDeviceGeneration::Create`
uses the same implementation. The additional checked
`UsdGenDeviceChannelStorageBytes` helper computes the minimum typed strided
span, including canonical zero-element storage, and rejects overflow without
changing its output. Publication now also rejects unrepresentable byte spans.
Generic scalar-array arity and vector-type arity retain the existing contract;
unsupported enums or Generic Topology domains are not silently coerced.

New direct preflight/packed/strided/empty/overflow/type tests pass in the
CUDA-disabled build. Full CUDA and CPU-only builds pass; the CUDA suite passes
173 tests plus the expected StormSurgery skip, 174 registered, 98.24 seconds.
This preserves native backend separation while strengthening shared admission.

Initial Vulkan Source drafts failed review for canonical empties, typed payload
validation and immutable publication lifetime. They remain unintegrated while
the native upload and complete prevalidation paths are rewritten. No Source
completion or production Vulkan availability is claimed by those draft files.

### Full native Vulkan Source upload verified

The Source rewrite now validates and packs complete host capture metadata and
payload before native allocation, then copies charged coherent staging buffers
into charged device-local planes. Nonblocking fence proof releases staging only
after every owner is proved; pending or lost proof retains the entire packet.
Ready publication cannot be resubmitted or poisoned through its upload handle.
Root corrected two remaining compile issues (removed helper reference and a
nonexistent resource-category enumerator); device outputs use Pinned and
temporary staging uses Scratch. No production geometry readback is used.

Public planes retain points, canonical offsets, stable IDs, optional rest,
width/hair/root/mask and typed named payloads. Private source T/B/N frames remain
separate from published channels. Chunks including rest bounds, tile metadata,
curve basis/wrap, deformation state and revisions are preserved. Unique IDs
retain authored order rather than imposing an unsupported sorting requirement.

Root's full hardware fixture verifies 16 public and three private planes byte
for byte, multiple retained Source revisions, absent optionals, canonical empty
offsets, metadata/typed payload rejection, bounded allocation rollback, staging
retirement, idempotent Ready proof, shared repeated publication, and exact
quarantine charge/native lifetime after a later fence. Both normal/quarantine
modes passed 20 repetitions each under API/synchronization validation (40 runs,
18.87 seconds). All ten Vulkan fixtures pass together (4.70 seconds); both
ordinary CUDA and CPU-only builds pass after source-target integration.

Next is a Candidate-proven Source-to-Width COW child: exact predecessor owner,
successful semantic proof, strictly newer value revision, shared non-width
planes/frames/chunks and only a fresh width allocation. That integration,
neutral generation/Session publication and the remaining CUDA graph work are
still open. The native Source proof does not enable production Vulkan backend
availability or establish completion of this plan.

### Native Source-to-Width COW and notification capability evidence

The native Width candidate now exposes proved success, exact input owner,
context and cardinality. Source-to-Width publication requires all four plus a
strictly newer value revision. A child shares every non-width allocation,
private source frame and chunk with its immutable predecessor, retaining only
the fresh proved width output. Empty generations retain canonical metadata;
wrong predecessors, contexts, counts, revisions and semantic failures reject.
Exclusive and inclusive retained-byte accessors count actual charged native
allocation sizes across the retained COW chain, not logical payload sizes.

The COW hardware fixture passed 20 repetitions under API/synchronization
validation (10.99 seconds). After adding exact footprint assertions, all 11
native Vulkan fixtures passed together, with the new notification capability
probe skipped (12 registered, 5.60 seconds). No validation errors occurred.

The GB10 driver advertises VK_KHR_external_fence_fd but does not report
SYNC_FD fence exportability. The Linux probe therefore correctly skips before
attempting export. This is not evidence of a working pollable notification
bridge. Autonomous completion, the owner-scheduled Source/Width job, neutral
publication and Session routing remain open; production Vulkan availability
is not enabled by these native proofs.

Retained-byte audit clarification: the footprint accessors describe one
generation's delta/retained chain. Repeated TakeReady or sibling adoption of
the same Width candidate can alias allocations, so these values cannot be
summed across cache entries as exact physical usage. The charged pool remains
authoritative; cache integration needs owner deduplication or an explicit
single-adoption contract. Header documentation now states this limitation.

### Additional Terra/Luna Vulkan implementation and validation wave

The requested ten additional simultaneous workers exceed the four-active-agent
limit (including root). Two new named workers were created before the retained
thread limit rejected further creation; bounded implementation/review tasks
reuse Terra/Luna threads under Astra coordination. This is not ten concurrent
agents. Metal stays excluded.

New native regressions verify a Generic Float32 channel with stride five and
logical span nine bytes, including exact readback and malformed payload size
rejection (validation pass, 0.44 seconds). An initial audit's alleged Vulkan
four-byte buffer-copy size restriction was rejected by independent primary-spec
review and this hardware proof; no unnecessary source padding was introduced.
The separate COW accounting fixture verifies source aliases, sibling candidate
adoption, exact root/child/grandchild physical ledger deltas and complete permit
release after the last descendant drops (validation pass, 0.46 seconds).

The owner-scheduled Source/Width job is now built in the opt-in native library.
It reserves progress/terminal delivery before submission, completes pending
proof despite suppression, settles once, and retains the entire job on loss of
both owner-delivery paths. Root review corrected a terminal-state overwrite
race; the fixture's external request ownership was also corrected before its
lifetime assertions. Success, suppression before/after upload, invalid controls,
repeated start, request lifetime and missing width failure pass under validation
(0.56 seconds). This test explicitly drives notifications after test-only native
proof: no autonomous backend execution is claimed. Retained completed handles
still hold admission credits in this baseline; immutable control detachment is
under review rather than adding an application mutex.

The separate semaphore SYNC_FD probe also skips on GB10: exportability is
unsupported (0.47 seconds). A capability-gated timeline notification probe is
next, with a dedicated native waiter outside framework/owner workers and a
host-signaled control timeline. Production completion-service integration,
neutral adapters, Session routing and the full plan remain open.

The timeline capability alternative is now proved on GB10: 20 repetitions of
GPU-signaled completion and host-signaled control wake passed under validation
(16.58 seconds). Each wait runs on a dedicated native test thread, not an owner
or framework worker; the original proof fence remains intact. Root corrected
test destruction ordering so result storage outlives the RAII joining thread
on failure returns. The bounded test waits are not a production polling loop.
Any future service must be scoped to one logical VkDevice; physical UUID pool
deduplication does not imply shared logical device/semaphore ownership.

Luna implemented Astra's immutable shared control-detachment design for the
job, with atomic shared-pointer snapshots and the existing single-winner
failure route. Terminal settlement no longer leaves admission credits held by
retained completed handles. Root's capacity-two regression retains the first
completed job, checks zero outstanding credits after Drain, and successfully
starts another job. The extended job fixture passed 20 repetitions under
validation (9.72 seconds). This does not promise immediate callback-reentrant
credit recycling before the current mailbox frame returns, nor hardware
lock-freedom of standard-library shared-pointer atomics. Both ordinary CUDA
and CPU-only builds pass after the native-only additions.

Final integrated native validation for this wave: 15 passes and two explicit
unsupported SYNC_FD capability skips, 17 registered tests, 8.16 seconds; no API
or synchronization validation errors. Next is the bounded logical-device
timeline completion service and real job notification integration. The current
Source capture/packing work also needs bounded off-owner preparation before a
production adapter can claim the short-owner execution/performance contract.

### Logical-device completion admission foundation

DeviceContext and DeviceFactory now preserve explicit trusted proof that
timeline semaphores were enabled when the supplied VkDevice was created.
Default is false even on supporting hardware; a true claim is additionally
checked against physical support before native context admission. The factory
test creates a genuinely enabled device and verifies both unclaimed/claimed
contexts and shared physical resource accounting (validation pass, 0.93 seconds).
This remains a trusted creation contract because Vulkan cannot query which
features were enabled on an arbitrary externally supplied logical device.

The execution pipeline now exposes exact serialized-owner identity using its
existing thread-local owner scope. Tests distinguish this owner, another owner,
parallel preparation work and external callers, so queue-confined service APIs
need not mistake any framework callback for their bound queue owner. The
CPU-only pipeline test passes (0.34 seconds); the full CUDA build passes and
the full CUDA suite passes 173 tests plus the expected StormSurgery skip,
174 registered, 94.88 seconds. These are admission prerequisites, not proof
that the completion service or production backend is implemented yet.

### Autonomous native Vulkan Source/Width execution proof

Terra's bounded logical-device completion service and Luna's optional job
integration are now built in the opt-in native target. One dedicated native
waiter observes GPU-completion and host-control timelines. Each watch reserves
owner command admission and native lifetime before phase submission. A proxy
fence on the ordered follow-up submit additionally proves prior fence signals;
the dedicated waiter performs one bounded proof wait, never blocking an owner
or framework worker. User delivery and final lifetime retirement occur on the
bound owner, not the native waiter. No production geometry readback is used.

Review corrected missed already-complete notifications, unsafe slot-field
reads, callback/retirement races, concurrent host timeline signaling, early
upload marking and omitted Width-watch retirement. A separate Source fixture
passes through the real completion service (0.85 seconds), and eight concurrent
Source-to-Width jobs autonomously complete without test-driven Notify calls or
fence waits (0.95 seconds). After stopping the native producer, test-only
readback verifies all eight width outputs and revisions, zero command credits
and complete native charge release. The manual job regression still passes
(0.43 seconds), three tests together 2.24 seconds under native validation.

This is meaningful autonomous native execution, not Session backend completion.
Failure sweeps, pending-close exact-once delivery, stale/early notifications and
autonomous cancellation are still under validation. Native availability remains
gated, and off-owner Source preparation, neutral adapters, Session routing,
renderer interoperability and the remaining CUDA graph work are still open.

### Completion failure transport and shutdown investigation

Pending close now routes required failure delivery to the owner; unavailable
owner routing uses the separate retained thread-neutral failure callback.
Unproved work remains charged and retained. An actual unavailable-owner test
found that passing `std::move(slot.ticket)` to an rvalue-reference parameter did
not consume the ticket on early rejection. Root now moves it to a local before
posting, so rejected admission releases without touching a reused slot after
successful handoff. Root/Astra reviewed the ownership against pipeline code.

Owner-side marked-watch abandonment now releases only exclusively claimed,
unused ticket admission, retains the native graph, settles sibling watches and
wakes the waiter. Extra early notifications are gated by owner-side delivered
proof rather than prematurely retiring pending watches. Known pre-submit Width
allocation failure still needs a distinct prepare/submit boundary: marking
before combined Begin is conservative but may unnecessarily quarantine the
service. This admission-fidelity gap is not claimed solved.

The first expanded repetition run was NOT green: five fixtures passed 20
repetitions each; the autonomous fixture passed once then timed out at 60
seconds (138.70 seconds total). A diagnostic autonomous rerun passed 30 times
(27.58 seconds), which does not erase or explain that timeout. Root identified
a separate concrete Close/Join lost-wake interleaving: the waiter could capture
the next control target after the close signal, then wait for a second signal
that never arrives. The waiter now rechecks closing/lost after target capture
and immediately before blocking. Service-mode native Poll(NotReady) after
consumed proxy proof now quarantines/fails instead of waiting with no remaining
event; manual polling mode is unchanged. The original timeout's attribution
remains unproven. A new 256-iteration idle-close fixture and another expanded
repeat run are being used to validate these changes before broader claims.

After those corrections, all seven service/job fixtures passed 20 repetitions
each (140 runs, 129.22 seconds) under API/synchronization validation. This
includes 5,120 immediate idle-service shutdowns, 160 fully autonomous two-phase
jobs, a separate 160-job early-notification robustness case, pending-close and
unavailable-owner exact-once failure/retention/credit checks, and the manual
job regression. No validation errors were emitted in this run. The earlier
timeout remains historical evidence with unproven attribution; this successful
run is not presented as a reproduced root-cause proof. Final native-suite
integration is being checked separately.

Final integrated native Vulkan validation passes 21 fixtures with two explicit
unsupported SYNC_FD capability skips, 23 registered, 14.84 seconds. Both CUDA
and CPU-only full builds remain green. Next required implementation is precise
Width pre-submit admission marking, followed by off-owner preparation and
neutral/Session integration; the broader execution-graph goal remains open.

### Width pre-submit admission validation

WidthPipeline now accepts an optional admission hook after preparation and
immediately before queue submission. SourceWidthJob marks the reserved watch
at that boundary; pre-mark failure cancels the watch, while possibly submitted
work retains conservative quarantine. Empty and invalid inputs never invoke
the hook. The new WidthAdmission fixture checks allocation rejection, rejected
and throwing hooks, exact resource rollback, reuse of the same one-slot
completion service, autonomous native completion, and unchanged COW input.
Its initial setup incorrectly used Pending with the allocation-only TryReserve
API; using Scratch for artificial budget pressure fixes that fixture setup.
The targeted validation passes (0.88 seconds). Full native validation then
passes 22 tests with two unsupported SYNC_FD skips, 24 registered, 15.23 seconds.
Off-owner preparation and neutral/Session integration remain open. Astra is
coordinating reusable Terra/Luna workers within the four-active-agent limit;
no claim is made that ten additional agents can run concurrently.

### Context-free Source preparation and owner handoff

VulkanPreparedSource now validates and packs a CPU-only immutable packet with
no DeviceContext/native ownership. VulkanSourceUpload can adopt that packet
on the queue owner without repacking; the legacy Create delegates through the
same validation. The CPU fixture covers immutable points/IDs/named bytes,
private TBN tags, valid stride-5/span-9 storage, invalid spans/cardinality,
canonical empty offsets and concurrent preparation. Its first test setup used
two Point-domain values for four points; correcting it to the two-element
Primitive domain produced a passing test (0.01 seconds). Native accounting
also passes prepared packet reuse, independent uploads, original width bytes
after caller mutation, and the existing COW alias/ancestor accounting (0.48 s).

SourceWidthJob has an optional shared TaskGraph preparer. Start reserves a
dedicated owner command ticket; owner ingress extracts only CPU capture data,
then a bounded task packs off-owner. An immutable result returns through the
reserved ticket, native adoption stays owner-confined, and preparation does
not mutate the pipeline work epoch. Preparing-state notifications are gated;
state CAS preserves concurrent terminal failures, and unavailable transport
retains the native-bearing relay before thread-neutral terminal fallback.
The compatibility path without a preparer remains explicit. Production Session
routing must use off-owner preparation; it is not enabled by this seam alone.

Review caught an agent replacement of the established autonomous test instead
of an additive edit. Root reconstructed and reviewed its substantive coverage:
genuinely enabled timeline logical device, eight Await-driven jobs, separate
32-early-notification mode, pre-start suppression, request lifetime, exact
owner, unchanged work epoch, width/points readback and zero credits/pool.
Prepared and prepared-plus-early variants are additive. All four variants
passed first validation (3.69 seconds); repetitions and preparation failure
tests are separate gates. CUDA and CPU-only full builds remain green/no-work.
CPU capture byte admission, owner staging memcpy cost, native-generation
lease adapter, compiler/Session routing and the wider CUDA graph remain open.

The four async modes subsequently passed 20 repetitions each (80 executions,
640 two-phase jobs, 73.94 seconds). The full native suite passes 25 tests plus
two unsupported SYNC_FD skips, 27 registered, 17.38 seconds. These runs do not
resolve the earlier historical autonomous timeout's unproven attribution.
Deterministic queued-preparation cancellation/rejection/shutdown checks are
being implemented separately before claiming complete preparation lifecycle
coverage.

The new preparation lifecycle fixture uses a held asynchronous TaskCompletion
with maxActiveTasks=1 (no blocked worker), a real pipeline cancellation token,
and explicit owner ordering. Its three modes prove queued cancellation before
packing/upload, maxJobs=1 preparation admission rejection, and owner shutdown
before preparation return. They check one terminal callback, owner versus
thread-neutral delivery, zero native plane allocations/command credits/task
admissions, normal request release, and deliberate native/request retention
on unavailable transport. Test-only Await-rejection signal preservation and
fail-fast handling prevent failed assertions from unwinding retained callback
stack references. All three first runs pass under Vulkan validation (1.31 s).

Root also rechecked the outstanding CUDA WidthBlend restriction directly:
cudaExecution.cpp still rejects different point/topology origins at compilation
and GetOperatorInput, despite the existing asynchronous full non-width GPU
comparator. Wiring its proof into retained operator phases remains required;
the preparer work does not imply that broader supported CUDA compositions or
Vulkan Session production routing are complete. Metal remains excluded.

Final preparation lifecycle validation passes 20 repetitions of each mode
(60 runs). Final integrated native validation passes 28 tests with two explicit
unsupported SYNC_FD skips, 30 registered, 18.37 seconds. CUDA and CPU-only full
builds were rechecked after final CMake registration and remain green/no-work.
This is verified progress on preparation and COW ownership, not completion of
the execution-graph goal. Next work includes the immutable native-generation
lease/retirement adapter and Session routing, alongside the retained CUDA
non-width comparison phase for semantically valid cross-origin WidthBlend.

### Cross-origin CUDA proof and new Vulkan worker waves

CUDA cross-origin WidthBlend now admits supported single-source geometry DAGs
and proves full non-width equality before launching the blend. The same-origin
fast path remains unchanged; proof-required tasks reserve 16 bytes and bind
the right predecessor's geometry, topology, stable IDs, root bindings and
named-channel logical values explicitly. Backend capability versions are now
CUDA 33 / WidthBlend 6. Private source frame equivalence is established through
preserved source-domain provenance and compared ordered stable IDs; this is
limited to the admitted source/Grow/Length/Noise/Width family, not ScatterGrow
or arbitrary reference geometry. Both direct and asynchronous paths retain
owners on unproved work; proved unequal inputs reject without poisoning.

CUDA and CPU-only full builds pass. WidthDag's expanded equality, inequality,
named-data, frame-absent reference, independent Grow, prior-generation COW,
callback-failure and logical-predecessor checks pass (0.85 seconds). The first
full CUDA run passes 172 tests, skips expected StormSurgery and fails one
old GrowSession compile-rejection assertion (174 registered, 92.90 seconds).
That fixture has been updated to require structural admission followed by
runtime rejection of unequal data. The first targeted rerun exposed a fourth
obsolete source/Grow bypass assertion after the first three were corrected;
after correcting that assertion too, GrowSession passes (6.60 seconds).
WidthDag also passes its targeted rerun (0.81 seconds). This is a full-suite
run plus a passing corrected-failure rerun, not a fresh all-green full run.

At the user's request, Astra now coordinates ten new Terra/Luna assignments
in five two-worker waves: native generation/consumer leases and COW tests;
SourceWidth publication and lifecycle tests; bounded Source-to-Width compiler
and admission tests; opt-in execution integration and regressions; narrowly
gated Session integration and end-to-end tests. The four-active-agent limit
includes root and Astra, so only two workers can run concurrently. The first
pair is active; the remaining eight are queued assignments, not running
agents. Root owns builds/GPU validation and CMake registration. Vulkan
production availability remains disabled pending end-to-end proof. The
original excluded item 8 is Metal, not the eighth Vulkan worker assignment.

The fresh integrated CUDA rerun is now green: 173 passing tests and the
expected StormSurgery skip, 174 registered, 93.13 seconds. WidthDag also
passes 20 consecutive executions (15.64 seconds); compute-sanitizer memcheck
and initcheck each report zero errors. The native Vulkan full build passes
after the shared-core changes. Vulkan adapter wave review caught raw pipeline
lifetime and post-Mark consumer-allocation hazards before integration; those
are being addressed with preadmission and an explicit external lifetime
domain. Adapter tests are not yet built/validated at this checkpoint.
The existing native Vulkan suite also passes after that rebuild: 28 passing
tests plus two unsupported SYNC_FD skips, 30 registered, 18.38 seconds with
synchronization validation enabled. This is baseline validation, not evidence
for the still-unregistered adapter fixture.
CPU-only integration also passes: 83 passing tests and the expected
StormSurgery skip, 84 registered, 37.21 seconds.

### Native Vulkan generation leases and COW publication bridge

The opt-in Vulkan library now includes a real neutral device-generation owner
over proved VulkanSourceGeneration snapshots. Exact device context, queue
token and scheduled queue owner are required for consumer admission. Each
consumer reserves its owner-return command and completion watch before any
native buffer becomes visible; completion posts a same-queue fence proof,
and unproved work retains the native graph. Public typed leases exclude
private source T/B/N and share neutral copy-lifetime semantics. Republish
reuses the owner; width children retain immutable non-width allocations and
charge their exclusive width delta separately from inclusive ancestors.

VulkanGenerationAdapterDomain provides an explicit external Close boundary,
self-retaining the queue until admitted consumers/operations retire. Close
does not spin waiting for a held lease, and it does not shut down a shared
completion service. A premature last external reference drop cannot release
the queue from a completion callback. Further concurrent-close/admission
review is ongoing; this is not a claim of fully validated Session teardown.

The first worker test was only a scaffold; root completed and registered the
actual native fixture rather than accepting that as implementation evidence.
It uploads source data, computes a COW Width child, publishes both, checks
metadata/republish/no extra pool allocation, and submits copies of every
public plane on the exact queue owner. A host-signalled timeline gate keeps
the GPU reads pending while all source/child/publication wrappers are dropped.
Copied leases retain storage; third-consumer admission with three occupied
watch slots rejects without leaking command credits. Completion runs through
native proof, verifies exact source and child bytes, returns every plane
allocation/credit, and closes the domain externally. Root's added premature
queue/domain external last-drop case also passes. First validation passed
(0.90 seconds); the strengthened fixture passed 20 repeats (17.63 seconds).

The SourceWidthJob null-width result after a marked submission is now tagged
LostProof rather than ordinary Failed, preserving downstream domain retention.
This classification change still needs the integrated native rerun. The next
worker pair is implementing native-job-to-neutral publication and lifecycle
tests, and a further worker has started the bounded Source-to-Width compiler.
These are intermediate slices: no Vulkan Session/registry availability is
enabled, and the full supported-operator execution goal remains open.

Domain review subsequently tightened Close/admission ordering to seq_cst and
released failed-admission queue/service snapshots before returning activity
credits. External metadata publication now uses a counted, allocation-free
RAII access guard, including diagnostic-allocation exceptions. Root also
verified rejection from a different scheduled owner, not merely an external
thread. The adapter and new publication-job targets build and pass together
(1.76 seconds). The earlier integrated 31-test native run passed 29 cases
with two unsupported SYNC_FD skips (19.18 seconds), including the marked-Width
failure classification change but before the final domain refinements.

VulkanPublicationJob now wraps the proved native SourceWidth job, requires
off-owner preparation, and holds a domain operation from admission through
terminal delivery. It publishes a neutral generation only on the exact owner;
superseded/failed candidates do not manufacture a result, while lost proof
retains the operation graph on either delivery thread. Redundant outer CPU
capture/request ownership was removed. The normal test proves two independent
published jobs retain their respective width values and identities; this is
not a claim of source sharing between separate jobs.

Root added three deterministic publication lifecycle modes using a held
asynchronous task completion (no blocked preparation worker): real queued
cancellation, preparation admission rejection, and owner shutdown before
preparation returns. Close is refused while the domain operation still owns
the queued job even though no native plane exists yet. Normal rejection and
cancellation return all activity/command/task/plane credits and release the
request; unavailable owner delivery preserves the request/job/domain by
intentional process-isolated quarantine. All four publication modes pass
their first runs (3.46 seconds). Final repetition of those four modes plus
the refined adapter fixture passes 20 times each: 100 executions, 86.07 seconds
under native Vulkan synchronization validation. CPU-only and CUDA full builds
also remain green/no-work after the opt-in CMake changes.

Eight of the ten additional assignments have now started. The descriptor-only
compiler and CPU fixture have landed for review/build; an injected-domain
native executor and full-payload CPU-reference fixture are active. Compiler
estimates are explicitly incomplete, not a promise of conservative physical
allocation admission. Resource metadata and source-control parity are still
being reviewed. No Session or backend registry route is enabled by this work.

The bounded compiler now explicitly emits CurveTopology and StableIds values
alongside geometry and widths, preserving those immutable values through
Width and publication. Its terminal points at publication task 2 rather than
the preceding Width task. The first CPU fixture build caught an incorrect
operator-capability field name (semanticNode versus authoredOrderKey); that
fixture was corrected. Compiler/metadata tests now pass (0.01 seconds),
including immutable capture, controls, negative authoring and the explicitly
staged Source-before-Width descriptor-vector restriction. This is still only
a two-node literal profile, not completion of general Vulkan graph semantics.

Final integrated native validation at this checkpoint passes 34 tests with
two unsupported SYNC_FD skips, 36 registered, 22.68 seconds. It includes the
refined domain, COW adapter, publication lifecycle modes, marked-Width failure
classification, and descriptor compiler. The next executor/source decoder and
CPU-reference native fixture are in progress and are not included in those
36 registered tests. The planned Session bridge will use an explicitly
injected shared per-device scheduled queue owner, separate from each Session
command owner; it must retain the existing acceptance/last-good/cache/dirty
semantics and the physical-device UUID resource budget. No route/availability
change is authorized merely by the success of these lower-level fixtures.

The injected Source-to-Width plan executor and native CPU-reference fixture
now build and pass. Source capture is CPU-only preparation; geometry operators
and publication stay native. The fixture compares all six public planes with
the real CPU compiler/scheduler for primvar ordering, index IDs, default widths,
ignored short/duplicate authored IDs under index mode, and canonical-empty
rest modes. Exact WidthPipeline context identity is checked before capture.
The first native run crashed in Capture: the duplicate-ID scan used `i != curves`
starting at one, which was invalid for zero curves. Root corrected it to
`i < curves` and reran the latest worker changes. Targeted compiler/executor
validation passes (0.93 seconds), followed by 20 executor repetitions
(18.46 seconds). Integrated native validation now passes 35 tests with the
same two unsupported SYNC_FD skips, 37 registered, 23.94 seconds.

This proof is for the private injected executor, not a Session route. The final
two assignments cover the neutral provider/owner-return bridge and its tests;
common Session preparation, publication/cache acceptance, authoritative value
revisions, context loss and runtime packaging remain integration gates. Frozen
curveGeneration alone must not stand in for changed source/Width value revisions.
Existing Source-to-Width COW and retained-generation tests remain in the native
suite; independent jobs are not claimed to share source allocations.

Root added `UsdGenCompiler::CompileInjectedDevice` as the common structural
compilation gate for the explicitly injected Vulkan provider. It bypasses only
the global backend-availability lookup, retains ordinary authoring/expression,
operator-contract and graph validation, and transactionally returns the native
plan plus routing graph. It does not capture/evaluate CPU geometry. Its native
CPU fixture proves success through the real narrow provider compiler, untouched
graph/plan on failure, common-kernel rejection even with a faulty provider,
and unchanged unavailable behavior through ordinary Compile. That test passes
(0.02 seconds); complete CPU-only and CUDA builds pass, followed by 12 targeted
CUDA/core graph/cache/Session regressions (8.96 seconds).

All ten additional worker assignments have now launched. The final pair is
implementing the provider transport and native two-owner fixture. Root added
their opt-in CMake registration; this new target is not yet validated and is
not evidence of a real Session route. Review gates include exact context/owner
identity, typed-plan provenance, terminal failure delivery, and retained native
ownership on failed return transport. Common Cooker/Session integration remains
to be connected after those gates. Existing cache plan digests already include
full source points/rest/widths/IDs and node literals, so same frozen generation
with changed payload does not require weakening or replacing that key. Native
revision metadata will use two independently reserved monotonic shared-domain
IDs (source/topology, then final Width), not a frozen generation or hash; cache
republishing preserves those revisions. This is allocation-scoped revision
identity, not topology-stability across separately allocated jobs.

The shared Cooker publication path is now factored into
`_FinalizeDevicePublication` and used by real asynchronous CUDA execution.
It retains presentation/store preparation, domain publication fencing, report
and stats updates, and deferred cache admission; CUDA binding diagnostics are
allocated before that state advances. `_Prepare` now accepts an injected plan
compiler while preserving the common dirty/reset/last-good baseline handling.
Latest CPU-only and CUDA full builds pass, and 12 CUDA/core Session/cache
regressions pass (10.89 seconds). These helpers are not yet a Vulkan Session
route on their own.

The Vulkan provider now checks exact owner/context and plan provenance before
native admission, uses explicit publication identity and authoritative native
revisions, preallocates terminal state, and retains the provider/request/result
graph when return transport cannot safely accept it. The native fixture submits
from a distinct return owner and proves six leased planes against the CPU oracle
through service-owned asynchronous copies and completion proof, without native
fence waits in owner callbacks. Its first run failed in the CPU oracle because
the fixture omitted `rebind=never`; the descriptor also lacked Vulkan backend
selection. Root corrected those authoring fields and added diagnostic output.
The corrected fixture passes, then passes 20 repetitions (9.38 seconds) after
adding wrong-context, invalid-revision, foreign-plan and capacity rejection
checks, plus exact neutral publication/device/native-version assertions.

Plan provenance storage is bounded: default 256 registrations over provider
lifetime, explicit exhaustion without evicting previously accepted handles.
The fixture uses capacity one and proves the first plan remains executable
after a second compile is refused. Expired-slot reclamation and sustained
Session recompilation remain work; this bound is not an unlimited-runtime
claim. False return transport still requires the Session-owned relay to settle
its async gate exactly once; provider quarantine is not itself that completion.
Real Session injection, coalescing/context-loss/cache acceptance and runtime
packaging remain outstanding. Production Vulkan availability stays disabled.

The explicitly injected Session route is now connected through the private,
native-header-free `CreateUsdGenDeviceSession` factory. It requires a canonical
shared cache domain with the provider's exact logical context/device key;
same-key independently constructed domains are rejected. Ordinary Sessions and
the global backend registry remain unchanged/unavailable for Vulkan. Each cook
pre-reserves its original Session command-owner return ticket before native
admission. The result relay returns immutable data to that owner; failed return
transport retains the native-bearing envelope and releases only the async gate,
without reading mutable cooker state from a device/foreign thread. Provider
attachment precedes context-loss handling. Session shutdown does not close the
shared provider/device domain.

CookDeviceAsync now uses the shared compiler/prepare/finalize path, exact shared
cache-domain epoch, provider identity stamps, two allocated native revision IDs,
cache lookup/coalesced publication and existing Session last-good acceptance.
The initial build caught a missing closure terminator in root's return binder;
it was corrected. The first real native Session run passed initial/changed
six-plane CPU-reference comparisons but failed the cache revision check.
Instrumentation confirmed a real hit (two entries, one hit, two admissions,
zero admission failures): legacy neutral Republish deliberately rewrites value
revision to publication ID. Root added explicit `RepublishPreservingRevisions`
for the Vulkan cache/coalesced route, leaving legacy CUDA behavior unchanged.
Its neutral unit fixture initially held the new alias past the existing owner
retirement assertion; releasing that alias at the same boundary corrected the
fixture and the unit passes (0.02 seconds).

The corrected real Session fixture passes (0.51 seconds), including unchanged
frozen curveGeneration with a Width edit, new/held-old native payload parity,
reverting to a cached native owner with preserved revisions/new publication ID,
and an unsupported edit retaining last-good output. This is actual Session
evidence, but only for the bounded Source-to-literal-Width profile. Dedicated
coalescing, cancellation, context-loss, lost-return and long-lived registry
reclamation gates remain incomplete.

`USDGEN_ENABLE_VULKAN_RUNTIME` now builds/exports the injected native runtime,
shared factory and validated Width shader independently of test enablement;
the package config resolves Vulkan/Threads only for that runtime option.
A fresh tests-OFF/runtime-ON configuration succeeds in a temporary build after
supplying this machine's explicit Vulkan SDK include/loader paths, produces the
validated shader, and registers zero tests. A full standalone runtime/install
consumer build is still pending; configuration/shader evidence alone is not
claimed as that packaging gate. Global automatic Vulkan registration remains
off and Metal remains outside this implementation scope.

Final verification for this integration checkpoint: real Vulkan Session test
passes 20 repetitions under synchronization validation (9.79 seconds), including
the noncanonical same-key cache-domain rejection. Latest complete CPU-only and
CUDA builds pass. Fourteen targeted CUDA/core device-generation, backend,
cache, async/owner and Session regression tests pass (11.20 seconds), preserving
legacy CUDA republish behavior. This checkpoint does not substitute for a fresh
whole-suite run or the outstanding lifecycle/operator/packaging gates above.

Provider provenance no longer has a lifetime compilation limit: fixed-capacity
atomic shared-record slots retain weak plan handles, replace only expired
records, and keep reader snapshots alive during provenance checks. Live plans
cannot be evicted; concurrent admission may still fail closed at capacity.
No application mutex was added, but this is not a claim that atomic shared_ptr
operations are hardware lock-free. The provider fixture fills capacity two,
rejects a third live plan, then performs 16 compile/drop/reuse cycles while the
first live plan remains executable. Latest provider and real Session native
tests pass together (0.97 seconds).

A private optional `UsdGenSessionDeviceObservers::coalescedRegistered(epoch,
leader)` notification now follows retained coalesced registration. It is an
immutable, nonblocking, thread-neutral observation callback, not permission to
enter Session/cooker state. It allows the lifecycle fixture to await actual
leader/follower registration before releasing a held preparation task instead
of asserting overlap from timing. Deterministic multi-Session proof is still
under construction at this checkpoint.

The standalone runtime packaging gate now has stronger evidence: a complete
fresh runtime-ON/tests-OFF/CUDA-OFF build succeeds, installs to a scratch prefix,
and a separate downstream `tests/consumer` project configures against that
installed package, builds, links and runs both the ordinary consumer and its
new opt-in Vulkan consumer. The latter uses installed Session/provider/factory
headers and symbols, verifies safe invalid-input rejection, and checks the
installed SPIR-V magic through exported `usdGen_VULKAN_WIDTH_SHADER`. No GPU is
required for that downstream packaging probe; it is not substituted for native
execution tests. The fresh standalone CTest inventory remains zero. Build and
install evidence is Linux ARM64 on the current Vulkan SDK/loader; Windows and
other platform packaging combinations remain unverified.

Real multi-Session coalescing now has a deterministic native fixture. A held
asynchronous preparation task (not a blocked worker) prevents native progress;
bounded external futures prove the task is held and that the two original
CommitAsync requests registered as leader/follower before release. Both results
are checked against the real CPU six-plane oracle. Stronger checks acquire
both exact-queue leases and compare every public VkBuffer and plane layout,
then verify distinct publication IDs with preserved topology/value revisions.
A forwarding instrumentation wrapper delegates to the real provider and
proves exactly one accepted provider native-job request, not a hardware dispatch
count. Final task/command/native ledgers return to zero. The first fixture
passes (0.52 seconds); strengthened buffer/request checks pass 20 repetitions
(9.20 seconds). Follower cancellation remains a separate pending mode.

A fresh complete native checkpoint passes 38 tests with two unsupported
SYNC_FD skips, 40 registered, 25.58 seconds. That integrated run precedes the
stronger test-only buffer/request assertions, which have their separate repeated
proof above. Latest CPU-only and CUDA full builds remain green after the private
registration observer addition; fresh full regression runs are underway.

The new follower-cancellation mode observes a real follower registration while
the leader's preparation gate is held, supersedes the follower through
SetGraphDesc, and waits for its original CommitAsync callback before releasing
the leader. It verifies Superseded, no follower native publication and retained
dirty state, followed by a Published leader with full payload parity and exactly
one accepted provider request. Both strengthened normal coalescing and this
cancellation mode pass 20 repetitions each (40 executions, 18.44 seconds) under
native synchronization validation. The cancellation fixture begins without a
previous follower generation; it does not claim coverage of every last-good
baseline or lost-return case.

Fresh complete regression results after these core integration/observer edits:
CUDA passes 173 tests plus the expected StormSurgery skip out of 174 (94.74
seconds); CPU-only passes 83 plus the same expected skip out of 84 (37.06
seconds). The standard install probes may be environment-conditional; the
separate explicit installed-runtime consumer build/run described above supplies
the actual Vulkan packaging evidence. Shared context loss/replacement, failed
Session return transport, admission rollback, concurrent registry stress and
broader graph/operator coverage remain required work. The full goal remains
open.

### Vulkan Session lifecycle follow-through (2026-09-14)

The real two-Session logical-loss fixture now verifies shared cache/provider
epoch invalidation, rejection by the latched old provider, last-good retention
and dirty/error state in both Sessions. It copies all six public planes through
leases acquired before the logical loss. A second actual VkDevice/context on
the same physical UUID shares the accounting pool but uses a distinct canonical
execution cache; injecting the old cache is rejected, its first cook is a miss,
and old retained output remains unchanged. Replacement retirement restores the
prior pool balance, and final teardown reaches zero. This is logical loss plus
an actual replacement context, not physical device-removal simulation. The
first-phase fixture passed before the stronger replacement assertions; the
strengthened fixture has passed its first run and is undergoing repetitions.

Pipeline Shutdown retains its original entry point and adds a nonblocking
external admission-closed observer overload. The close flag is stored before
observation and draining follows even if the observer throws (counted as a
callback failure). Private Session injection forwards this lifecycle observer;
ordinary Sessions do not install one. A native-free test proves closed-ticket
rejection, exception containment, async work drain and exactly one terminal.

A forwarding test provider retains a real proved Vulkan result until an
external Session destructor has observably closed command admission. Returning
that result then deterministically exercises rejected owner transport, without
polling or blocking a queue worker. Duplicate return is rejected; the original
commit settles once as Superseded and publishes no generation. Quarantine
retains the immutable generation and its charged allocations after Session
destruction. Since native work is proved and no consumer lease is outstanding,
the adapter can legitimately close; the fixture checks retained charges after
that close rather than incorrectly demanding a failed close. This process-
isolated quarantine intentionally does not reach zero native bytes. The test
and native-free shutdown test pass (0.62 seconds combined).

The provider provenance fixture now races four external compiler callers for
one reclaimable slot (64 bounded attempts), keeps an older live plan throughout,
checks post-race slot reclamation, and then executes that original plan through
the native provider. Contended admission rejection is permitted; losing live
provenance is not. Its first native run passes (0.46 seconds).

A separate admission-rollback mode retains every native queue command ticket
without blocking any worker. A real Session cook must fail without allocation
or publication, remain dirty and leave only the deliberately held credits.
Releasing those tickets retries the same Session through the ordinary complete
parity/cache/last-good fixture. Repeated validation and fresh complete native,
CPU-only and CUDA regressions are in progress; broader operators and global
Vulkan backend availability remain deliberately unclaimed.

Validation checkpoint: context-loss/replacement, lost-return quarantine,
native-ingress rollback and concurrent-registry/provider tests each pass 20
repetitions (80 executions, 44.31 seconds) under synchronization validation.
The fresh full native suite passes 42 tests with two unsupported SYNC_FD skips
out of 44 registered (27.09 seconds). CPU-only and CUDA complete builds are
also green after the shutdown observer change; their fresh full runs follow
serially so cross-build GPU use cannot overlap.

The fresh CPU-only suite passes 83 tests plus the expected StormSurgery skip
out of 84 (26.94 seconds). The first fresh CUDA full run instead passes 172,
skips StormSurgery and reports a SEGFAULT in testUsdGenSessionStoreOwner
(174 registered, 55.22 seconds). The failing process emitted no captured
output and no accessible core was found; buffered output does not establish
where it failed. An isolated 30-repeat run passes (8.40 seconds), which does
not diagnose or fix the intermittent crash. A second full CUDA run is in
progress and read-only Session/store lifetime review is underway. Do not
replace the first failure with an unconditional green regression claim.

The second complete CUDA run passes 173 tests with the expected StormSurgery
skip (174 registered, 53.33 seconds). Read-only Astra/Terra/Luna review found
no demonstrated default-Session race in the new optional observer path, and
no production change is justified by the available crash evidence. The first
unexplained SessionStoreOwner SIGSEGV remains an open regression observation;
the rerun is not a fix. All build/test handles for this checkpoint are terminal.

Next bounded Vulkan implementation design (not yet implemented): scalar Width
root/tip scaling and taper, followed by fixed-topology Length and deterministic
Noise. Width profile must retain and prove the exact source hairT plane as well
as width; WithWidth publication must reject a candidate made with another
source's hairT even if its width owner matches. Keep the existing flat dispatch
bitwise unchanged. For profile math, follow CPU order (taper before root/tip)
and preserve `input * (1 + (target - 1))`, never simplify to `input * target`.
CUDA's scale-before-taper composition is semantically comparable but cannot
be claimed bitwise equal for all scalar combinations.

A proposed additive profile module preserves bindings 0/1/2 (input/output/
status), adds immutable hairT at binding 3, and retains the existing 12-byte
count/width/replace push-constant prefix. Keep legacy Create/Begin usable;
profile admission requires an explicitly supplied trusted profile-capable
module, never silently running an old flat-only shader. This design is pending
implementation/review, not additional backend availability. Parallel ownership
can separate pipeline/shader/provenance, compiler/job wiring and tests after
the interface is frozen. Metal remains excluded.

### Vulkan scalar Width profiles implemented (2026-09-14)

The additive profile path now executes literal rootScale/tipScale/taper/
taperStart on Vulkan, through compiler -> plan executor -> source/Width job ->
neutral publication -> real injected Session. The separately built and
spirv-val-validated widthProfile.spv uses four bindings and a 32-byte push block
with the original 12-byte prefix. Legacy Create/Begin retain the original
three-binding flat shader. Non-neutral plans on a flat-only pipeline reject
before source capture/native admission. Neutral controls ignore taperStart
semantically when taper is zero, but all authored scalar ranges are validated.
Ramps, maps, expressions and non-unit envelopes remain outside this slice.

Profile candidates retain exact immutable width and hairT buffer owners;
hairT must have the exact nonempty byte count, storage usage, matching native
context and completed input proof. GPU validation rejects nonfinite or
out-of-range hairT rather than clamping it; semantic overflow exposes no
output. WithWidth additionally checks that the candidate's hairT owner is the
base generation's exact plane owner. Non-width planes, named data, chunks and
source-frame ownership remain shared; only the fresh width allocation is
exclusive. The neutral metadata now includes a seventh logical value for the
source hairT named-channel bundle with explicit source write and Width/
publication reads. Logical source estimates include its bytes; estimates
remain marked incomplete/non-conservative, not a physical admission promise.

New direct-source tests prove wrong-source hairT rejection, immutable shared
plane identity, exact retained-byte deltas, input preservation, replace and
multiply output, rejected admission rollback, empty behavior, malformed GPU
hairT and overflow status. Real Session tests change only a profile control
while preserving source generation and scalar width, check old/new six-plane
CPU parity, and return to the original descriptor to prove exact cache owner/
revision reuse. The real plan executor adds profile and taper endpoint parity
plus flat-only capability rejection before capture. No production geometry
readback or CPU evaluation fallback was introduced.

The first compiler/source/Session/executor profile run passes (2.08 seconds).
Strengthened repetitions initially caught a test-helper contract error: new
boundary calls passed null request state to a helper that asserts a supplied
request owner remains live through completion. Those calls now provide real
anchors; production code was unchanged for this correction. Final repetitions
and the full native checkpoint are underway, not yet claimed here.

The runtime-ON/tests-OFF/CUDA-OFF build and install pass with both shader
modules. A separately configured installed consumer builds and runs, checking
both exported shader paths, installed profile controls and the additive API's
invalid-input rejection. This is Linux ARM64 evidence, not cross-platform
packaging completion. The unrelated intermittent SessionStoreOwner SIGSEGV
remains unexplained; its stdout is now unbuffered so future CTest crashes retain
the last completed contract check. With that diagnostic-only test change,
StoreOwner and pending-exit modes pass 20 repetitions each (13.21 seconds).

Final profile checkpoint: compiler, direct Source/Width profile, real Session
profile and strengthened plan-executor fixtures each pass 20 repetitions
(80 executions, 39.77 seconds). The complete native suite passes 44 tests with
two unsupported SYNC_FD skips out of 46 registered (28.31 seconds). This covers
the unchanged legacy flat Width and existing lifecycle fixtures alongside the
new profile modes. Native runtime-only build/install and separate installed
consumer remain green. No CUDA production code changed in this profile slice;
the previous full CUDA rerun and its unresolved earlier intermittent failure
remain the authoritative broader CUDA evidence.

Next Length design, not implemented: fixed-topology scalar scale changes only
packed Float32 points (arity 3, stride 12); use scalar GLSL indexing rather than
std430 vec3-array stride 16. Preserve immutable rest, widths, hairT, offsets,
IDs, source-frame/root-binding planes and rest bounds. CPU factor-one is an
exact copy; other factors use root + (point - root) * factor. CUDA derives
factor via current-length multiplication/division, so semantic parity must
not be advertised as universal bitwise parity. Validate the entire topology
before writes or use uniquely owned point writes plus complete validation;
malformed overlapping offset ranges cannot be allowed to race.

Integrating Source -> Length -> Width into the staged asynchronous job needs
an explicit intermediate geometry value and THREE authoritative revisions.
Current Session reservation has only source/final revisions: do not invent
source+1 under concurrent reservations or reuse final for both stages while
WithWidth requires strict revision ordering. Extend the revision contract and
reservation policy coherently with the typed plan before this integration.
Audit active geometry extents/alreadyDeformed/publication bounds for WithPoints;
immutable rest bounds alone do not justify stale active bounds. Preserve the
existing Source -> Width path; neither a three-node chain nor a later direct
Length terminal would constitute general DAG/branch coverage. Metal remains
excluded and the full execution-graph goal remains open.

### Vulkan Length integration and ten additional worker assignments (2026-09-14)

The user requested ten additional Terra/Luna workers under Astra orchestration.
The runtime permits four active agents including root, so Astra is dispatching
ten distinct bounded assignments in waves with two implementation workers.
Metal remains excluded. Root retains build, GPU validation, packaging and
integration ownership; workers have disjoint file ownership and do not run GPU
tests concurrently.

The scalar Length pipeline, points-only COW generation, optional Length stage
in the source/width job, compiler metadata and explicit intermediate revision
contract now build. Session allocates each intermediate from the shared domain
sequencer instead of assuming consecutive IDs. The neutral revision test passes,
including interleaved reservations. The CPU suite before the pending Length
layout correction passes 83 tests with one StormSurgery skip (84 total,
26.70 seconds). Runtime-only build/install and an installed consumer pass with
all three shader modules; this is Linux ARM64 evidence only.

Initial native integration tests are NOT yet green. Executor readback reports
points scalar 6 actual 200 versus CPU expected 225 on a ragged source. Inspection
finds CPU effective-buffer routing stops at Length's topology classification
despite its non-owning capture, leaving downstream uniform chunk layouts before
later source offsets are inherited. A targeted correction and regression are
being assigned; the native per-offset kernel must not reproduce a bad CPU root.
Both new Session Length modes also fail their CPU oracle shape checks. Direct
native tests reach the final empty-COW check after passing nonempty scalar,
sharing, malformed-offset, overflow and provenance checks; the empty fixture
incorrectly omitted its source offset-sentinel owner and has been corrected,
with rerun pending. No complete Vulkan Length parity claim yet.

Integration follow-up: the platform refused a sixth new worker with `agent
thread limit reached`, including root's independent spawn attempt. Exactly
five new Terra/Luna threads were created, and Astra reused workers to complete
ten bounded assignments: COW ownership, Session Length integration, direct
native COW tests, provider revision rejection, native numerical/barrier audit,
CPU ragged-layout regression, job phase tests, executor rejection tests, source
metadata/private-frame checks and final read-only numerical/provenance audit.
Assignment nine is source-generation evidence, not a new neutral-adapter lease
test. Assignment seven manually drives proofs; autonomous Session coverage is
provided separately. Worker build/test claims are not validation evidence;
root rebuilds and runs the actual NVIDIA fixture.

The CPU layout fix follows non-owning Length to its effective upstream buffer
when partitioning and makes Length evaluation use rebased per-curve offsets.
Actual owning captures remain topology boundaries. Executor and both Session
Length modes now pass exact six-plane CPU comparisons (three tests, 2.06 s).
Direct native Length COW passed twenty repetitions (11.10 s) before the later
metadata additions. Provider negative tests initially had an out-of-bounds
`nodes[2]` write in a two-element test descriptor; GDB identified that exact
test constructor, and corrected provider tests pass (0.47 s). This was a test
bug, not a native provider failure.

The post-fix CPU suite passes 83 tests with one skip (84 total, 26.73 s).
The first CUDA full run passes 173 tests with one skip and one failure out of
175 (52.58 s): only the new CPU empty-layout regression incorrectly required
an offset sentinel. The corrected regression compares the terminal empty
topology exactly against its source and passes in both CPU and CUDA builds.
Existing intermittent SessionStoreOwner failure did not reproduce; it remains
historically unexplained, not claimed fixed. Final full/repeated native checks
and the corrected full CUDA rerun are still pending at this checkpoint.

Final integration builds pass, including the literal-only rejection of animated
Length parameters. Additional metadata tests preserve both true and false
`alreadyDeformed` provenance, invalidate point-derived tile bounds and preserve
private source frames. Their temporary false-deformed fixture initially retained
a context beyond the teardown assertion; it now lives inside the Length test
scope. No production lifetime behavior was changed for that fixture correction.

The corrected full CUDA suite passes 174 tests with one expected StormSurgery
skip out of 175 (51.78 seconds). The complete native Vulkan suite passes 48
tests with two unsupported SYNC_FD skips out of 50 (30.19 seconds), with Vulkan
synchronization validation enabled. The final runtime-only build/install and
installed consumer pass with flat Width, Width profile and Length shader paths.
Seven targeted compiler/COW/job/executor/provider/Session tests are now running
twenty repetitions each; their result will be recorded after completion.

Admitted scope remains Source -> optional scalar Length(scale) -> Width, with
optional scalar Width profiles. This is not general Vulkan DAG, branching,
Length set/cull, mapped controls, Noise, Grow or RBF support; global Vulkan
backend registration remains unavailable. Metal remains excluded. Broader
execution-graph and cross-platform work remains open.

Final repeated checkpoint: all seven targeted tests pass twenty consecutive
executions each (140 executions, 69.40 seconds), including the final metadata,
literal-admission and teardown fixture corrections. No Vulkan validation or
synchronization diagnostics were reported. All root build/test handles for this
batch are terminal; the broader goal is not marked complete.

### Vulkan value-indexed fan-out/fan-in implementation (2026-09-14)

The preceding turn made verified progress. The next implementation removes the
fixed Source/optional-Length/Width chain assumption for the currently supported
operator family. Astra coordinates a typed topologically ordered stage list and
value-indexed native job; Terra owns a native ordered WidthBlend primitive and
exact non-width COW provenance, with Luna assigned independent fixtures.
Root owns revision admission, build/package wiring and real Session validation.

The intended admission is one source with Width, scalar Length and ordered
WidthBlend stages. Inputs refer to exact immutable predecessor values. Width
forks inherit non-width provenance; a points-producing stage establishes a new
identity. Blend requires matching non-width provenance, not merely matching
counts or revisions. Cross-origin equality remains rejected until a native
proof exists. Authored-vector order must not substitute for topological sorting;
dead nodes and invalid joins must be diagnosed explicitly.

Session now reserves one revision per semantic operator for admitted unary and
value DAG metadata as well as linear metadata; the provider uses the typed
plan's intermediate count. Root has added fourth-shader build/install/consumer
wiring and Session DAG fixtures for endpoints, ordered-input reversal, reversed
authored order, longer branches, shared Length trunks, Width profiles, retained
old payloads and cache reuse. Native implementation and test execution are still
pending here, so these additions are not a completion or parity claim. Initial
execution may serialize topological phases on one queue; this is not a claim
of concurrent GPU branch execution. Metal remains excluded.

Initial implementation evidence: the compiler, direct WidthBlend COW fixture
and native PlanExecutor pass (three tests, 1.69 seconds). Both real Session DAG
modes pass (1.29 seconds), including the shared Length trunk/profile mode and
a second nested WidthBlend. The direct primitive verifies exact endpoint and
interior arithmetic, immutable transported planes/private frames, ordered
candidate ownership, revision rejection, unrelated origins including empty
sources, pre-submit rollback and GPU rejection of negative/NaN/infinite widths.
The first build exposed a missing closing brace in the new native primitive;
it was corrected and the implementation expanded for readable review before
these runs. These are focused results, not the final full-suite checkpoint.

The typed compiler admits up to 64 operator stages, rejects unused nodes,
cycles, malformed ordered inputs and incompatible non-width lineages, and
produces per-value dependency metadata with a publication join. Its terminal
is Width or WidthBlend; Length-only terminal remains excluded. The native job
retains predecessor generations by value index and reuses the bounded service
watch protocol between serialized phases. A separate manual fixture now
suppresses publication specifically at BlendPending, with proof, callback,
command-credit and ledger assertions; root execution is still pending.

The complete CPU suite passes 84 tests with one StormSurgery skip out of 85
(26.66 seconds). The complete native Vulkan suite passes 52 tests with two
unsupported SYNC_FD skips out of 54 (33.44 seconds), including the BlendPending
suppression fixture. The full CUDA regression suite passes 174 tests with one
StormSurgery skip out of 175 (53.07 seconds). No CUDA operator implementation
changed in this slice; shared Session revision admission was rebuilt and
checked in both configurations. The earlier intermittent SessionStoreOwner
failure remains historically unexplained and did not recur in this run.

The runtime-ON/tests-OFF/CUDA-OFF build and installed consumer pass with all four
shader modules. Final cosmetic header defaults and documentation have been
rebuilt without the new missing-field warnings. Six focused native tests are
running twenty repetitions each after that freeze; results are pending here.
No agent edits remain outstanding. This is Linux ARM64/NVIDIA validation, not
a Windows or other-driver portability claim.

Final DAG repetition checkpoint: all six targeted tests pass twenty consecutive
executions each (120 executions, 66.85 seconds) after the final rebuild, with
Vulkan synchronization validation enabled. Installed consumer rebuild/run also
passes against the final headers and libraries. All build/test handles are
terminal. The next gaps include cross-origin non-width equality proof, broader
operator/source admission, end-to-end empty DAG publication coverage, and real
resource-aware branch concurrency rather than serialized stage submission.
Global Vulkan backend availability and cross-platform renderer residency are
not enabled or claimed by this private-injection checkpoint. Metal remains
excluded; the full execution-graph objective remains active.

### Vulkan cross-origin non-width proof implementation (2026-09-14)

This checkpoint extends the private Vulkan Width/Length/WidthBlend DAG path,
not global backend availability. Distinct Length-produced point owners may
join only after native non-width equality proof; shared non-width identity
retains the existing zero-comparison fast path. COW publication replaces only
width and retains the left non-width packet. The proof is bound to the exact
ordered predecessor generation owners, not revision IDs or matching counts.

The new comparator admits complete semantic geometry/tile/chunk metadata and
all public non-width/private frame schemas on the host, ignoring allocation
and revision identity. It packs payloads into charged, zero-padded native
scratch and compares exact bytes on the GPU, including non-word-aligned spans.
Only a four-byte status is read back. Temporary buffers have checked sizing,
device limits, producer/transfer/compute/host barriers, original-fence proof,
and preallocated lost-proof quarantine. Even empty comparisons submit a status
job. This is byte equality, not approximate floating-point equality.

The typed compiler marks proof-requiring joins and records the right geometry
dependency plus logical scratch estimates (not physical allocation bounds).
Executor admission rejects missing comparator modules before source capture.
The job adds ComparePending with independent reserved completion watches;
semantic operator revisions are unchanged. Unequal data fails without
publishing, cancellation settles after native proof, and failed/lost proofs
retain the predecessor/native ownership graph.

Requested parallelization was executed as ten bounded assignments, with six
new Terra/Luna threads and four assignments on reused workers after the
platform rejected additional threads. At most four agents including root
were active. Astra coordinated integration and final review; root alone ran
builds/GPU validation and packaging. Assignment accounting:

1. New Luna: COW identity and exact ordered proof-binding audit.
2. New Terra: complete semantic non-width metadata helper.
3. New Terra: native comparator implementation hardening.
4. New Luna: empty/named/private-frame/width-only comparator fixtures.
5. New Luna: revision and stage dependency audit.
6. New Terra: proof-binding, stale/missing/swapped proof, byte-tail and rollback tests.
7. Reused Luna: manual ComparePending cancellation fixture.
8. Reused Terra: missing comparator pre-capture admission fixture.
9. Reused Luna: shader ABI, byte-tail and arithmetic audit.
10. Reused Terra: comparison/watch lifecycle audit.

These numbers describe this new assignment batch, not the user's earlier
excluded plan item. Metal remains excluded. Worker findings were independently
reviewed: an alleged stage-index off-by-one was rejected because stage i
produces value i+1. Root/Astra found and fixed a real raw plane-index mismatch
when optional width is present on only one predecessor; regression covers both
directions. Scratch metadata was corrected to scratchPeakBytes.

Root Session coverage now includes separate equal Length branches, unequal
branches retaining last-good, old-generation byte checks, and empty DAG native
publication. Empty native packing exposes points/offsets/IDs plus the Width COW
view; optional empty rest/hairT are absent, and offsets retain the zero sentinel.
The initial empty fixture incorrectly required six planes and failed three
Session modes; correcting that expectation yielded seven focused tests passing
with synchronization validation (4.10 seconds). The latest optional-width
pairing regression and metadata correction are rebuilding; full-suite and
installed-consumer results are pending at this entry. A fifth packaged shader,
nonWidthCompare.spv, is now exported and exercised by the installed consumer.

Remaining scope includes broader operator/source admission, actual GPU branch
concurrency, broader fault-injection/soak coverage and cross-platform renderer
residency. This checkpoint does not claim Windows/other-driver validation or
enable global Vulkan availability. The full execution-graph goal remains active.

Final cross-origin validation checkpoint: native build and SPIR-V validation
pass. Full Vulkan suite passes 55 tests with two unsupported SYNC_FD skips
(57 total, 34.34 seconds), with synchronization validation enabled. The first
full run caught a stale revision literal in root's optional-width regression
(10 below empty source revision 31); changing the test child to revision 32
restored the intended proof/plane-pairing exercise. Final comparator admission
rollback assertions were added afterward and the comparator, cross-origin
Session and ComparePending cancellation tests each pass five repetitions
(15 executions, 8.50 seconds). No production code changed after the full pass.

Runtime-ON/tests-OFF/CUDA-OFF build, installation, separate consumer build and
consumer execution pass against final native headers/library and all five
shader modules. git diff --check is clean. All worker assignments and root
build/test processes are terminal. CPU/CUDA suites were not rerun for this
Vulkan-only checkpoint; their preceding recorded results remain historical
evidence, not new runs. Linux ARM64/NVIDIA GB10 only; broader execution-graph
implementation and cross-platform validation remain unfinished.

### Vulkan C3 authored named-channel transport (2026-09-14)

The preceding turn was verified progress. This checkpoint removes the Vulkan
graph admission/transport gap for C3 authored Point, Primitive and Groom
Float32/Int32 planes, arity 1–16. It does not broaden root binding, source
transforms, maps, expressions or operator mathematics.

A shared native-free authoredNamedChannels.h helper validates schemas, global
duplicate/reserved names, exact cardinality and finite float payloads. Capture
checks the canonical curve permutation and both offset sequences before
gathering ragged Point and Primitive tuples. Groom constants share immutable
descriptor-backed VtArray storage through COW. No CPU operator evaluation or
production geometry readback was added: host work is source packet preparation.
Native Source packing uploads the prepared channels, and Width/Length/Blend
retain the same immutable non-width owners. The existing cross-origin GPU
proof compares these payloads and schemas as part of the full packet.

The compiler includes named payload bytes in logical source/proof estimates;
resource value 6 explicitly represents the immutable hairT plus authored named
bundle and remains a source write and read by all operators/publication. These
remain logical estimates, not physical allocation bounds. Existing descriptor
hashing already includes names/types/domains/arities/payloads; native Session
tests prove value-only and schema changes with unchanged curveGeneration do
not reuse stale cached data.

Astra coordinated two reused Terra workers for the helper/capture and
compiler/executor tests, while owning compiler integration/review. Root added
the real Session schema/byte oracle, CMake mode, installed-header probe and
ran all builds/GPU checks. Compiler coverage spans 96 type/domain/arity
combinations plus malformed, duplicate and reserved schemas. Native executor
coverage checks six representative named planes against CPU bytes through
Length/Width and WidthBlend; its separate cross-origin case is compiler/CPU
oracle coverage only. The real Session test supplies actual native
cross-origin proof coverage with Point Float32 pairs, Primitive Int32 triples
and Groom Float32 arity 16, plus an Int32 schema mutation. It independently
checks expected canonical tuple order for ragged counts {3,2}, IDs {20,10},
old immutable generations, nested/reordered joins, named-only edits,
equal/unequal Length branches, and empty source packets retaining Groom data.

Initial focused validation exposed a test-only producer assertion error:
the Source writes value 6 with no upstream producer; only downstream reads
name producer task 0. Root corrected that assertion and removed an unused
oracle parameter. Native tests passed in that initial run. The full rebuilt
Vulkan suite then passed 56 tests with two unsupported SYNC_FD skips (58 total,
34.97 seconds), with synchronization validation. Runtime-only build/install
and the separate installed consumer (including the new neutral helper header)
also passed. Additional malformed-schema assertions and targeted repetitions
are being verified after the full pass; production code is unchanged.

Broader operator/source compositions, actual GPU branch concurrency,
remaining CUDA value-DAG work and cross-platform/reliability gates remain
open. Metal remains excluded. CPU/CUDA full suites were not rerun for this
Vulkan-only checkpoint; the execution-graph goal remains active.

Final named-channel checkpoint: compiler, native executor and real named
Session fixtures each pass five consecutive executions (15 total, 9.21
seconds) after the final malformed-schema assertions. Installed consumer
execution passes, git diff --check is clean, and all worker/build/test handles
are terminal. No production edits followed the full 56-pass/two-skip run.

### CUDA RBF immutable value-DAG integration (2026-09-14, in progress)

The named-channel checkpoint was verified progress. The next implementation
targets the existing CUDA RBF composition gap rather than another Vulkan-only
admission extension. Code inspection found Deform excluded from value-DAG
layout, a global rather than per-lineage double-deformation check, sync/async
RBF stages reading global geometry instead of their authored predecessor, and
only one pending RBF publication transaction per job. Merely permitting the
graph or serializing the kernels would still lose earlier deferred updates.

Astra coordinates a Terra production worker on the coupled compiler/runtime
and a Terra test worker on compiler/WidthDag assertions. Root owns the new
RbfTransaction --value-dag hardware fixture and CMake registration plus all
build/GPU checks. Required implementation is exact immutable predecessor
selection, point-only COW value recording, per-lineage deformation semantics,
and a per-node pending transaction collection with all-entry preflight before
publication acceptance/rollback. Native staged RBF already accepts arbitrary
input geometry; no CPU fallback or new deformation mathematics is needed.

Root's eight-case fixture covers independent RBF/Width siblings,
selected Source/Width terminals, Width/Length-scale/Length-cull predecessors,
and two independent RBF branches joined after Width. It uses ragged curves,
stable IDs, root bindings and a named point channel, compares sync/async
results with independently executed linear references, and checks warm
finalization rollback/retry and old immutable generation data. Implementation
and validation are pending; this entry is not completion evidence.

Follow-up targeted CUDA checkpoint: rebuilt runtime and all eight RBF DAG
variants pass, including empty input and two-entry sync/async rollback/retry.
WidthDag and legacy RbfTransaction pass. ExecutionPlan passes after correcting
stale fixture expectations for the eight Deform resources, unary-DAG shape,
inherited point producer through Width, and semantic dependencies alongside
workspace serialization. The rebuilt full CUDA suite subsequently passes
175 tests with one StormSurgery skip (176 total, 53.33 seconds). CPU-only
build passes with the existing unused NoiseNeedsAuthoredSourceRest warning;
its full suite passes 84 tests with one StormSurgery skip (85 total,
27.07 seconds). Vulkan validation is separate
and pending the bound-source integration below.

### Vulkan bound C3 source capture wave (2026-09-14, in progress)

User requested ten more Luna/Terra implementation agents. Four active slots
include root and Astra, so implementation is dispatched in two-worker waves.
First wave actually launched: one reused Terra owns source capture integration;
one new Terra owns compiler admission and resource estimates. Eight further
assignments are planned, not yet launched at this checkpoint: malformed/drop
capture fixtures, native rooted-source parity, named survivor gather, compiler
surface/rebind negatives, private-frame COW/proof binding, rooted Session
lifecycle, pre-capture admission/lifetime rollback, and final numerical/schema
review. Do not interpret ten assignments as ten newly spawned agents.

Slice scope is immutable bound C3 source preparation and native upload using
the existing curve loader, with exact root-binding/frame and named-channel
ownership retained through the Vulkan DAG. Identity world transforms,
resampleTo zero and existing guideBlend exclusions remain. No CPU geometry
operator fallback/readback or global Vulkan availability change is authorized
by this checkpoint. Implementation and native validation remain pending.

Bound-source integration checkpoint: the production capture now uses
UsdGenCurveLoader on the immutable descriptor snapshot and reapplies Session
revision authority. Compiler admission supports zero or one resolved surface,
retains the existing explicit rebind=never surface-free policy, validates
authored frame structure, and declares a conditional RootBindings bundle
(rootPrim/rootUV/private T/B/N), estimated at 48 bytes per input curve before
root drops. Publication carries alreadyDeformed=!useRest; points and rest
remain distinct. An intermediate unconditional one-surface restriction was
caught in root review and corrected before validation.

The unchanged Vulkan suite passes 56 tests with two unsupported SYNC_FD
skips (58 total, 35.13 seconds), with synchronization validation. This proves
existing unbound/named/DAG behavior survives the loader integration, not yet
all new bound-source behavior. The new CPU source-capture contract fixture
passes after an explicit TfToken test compile fix. Native rooted executor
fixtures are being added and are not yet validation evidence. Runtime-only
build/install and separate installed-consumer build/execution also pass.

New rooted tests checkpoint: compiler, CPU capture contract, and native
PlanExecutor each pass five repetitions (15 executions, 5.83 seconds).
Native coverage includes onError/always/never-drop, ragged canonical stable
IDs and six named planes, public root schema/byte parity with an independently
executed CPU graph, Length-to-Width and WidthBlend preservation, and both
alreadyDeformed values. Private frame payload equality still needs its
dedicated proof/COW fixture; public-plane exclusion is not proof of payload
correctness. Root corrected the compiler negative test to reject perspective
components while positively admitting a translated affine frame.

Six of the ten assignments have now been dispatched: five new Luna/Terra
threads plus one reused Terra. The first five are complete; assignment six
adds independently expected survivor tuples instead of relying only on a
shared-loader CPU oracle. Remaining assignments cover private-frame COW/proof,
rooted Session lifecycle including all-roots-dropped, pre-capture rollback,
and final review. The broader execution-graph goal remains unfinished.

Assignment seven subsequently launched as a new Terra on the separate native
RootFrameCow fixture. Current total is seven dispatched, six new distinct
threads plus one reused; five complete and two running. Root build/test
handles are all terminal at this checkpoint.

Further rooted validation checkpoint: the independent ragged survivor
fixture passes five runs (0.06 seconds), including seven surviving CV tuples
and all-dropped Groom constants. Root corrected two fixture assumptions:
the four-CV surviving curve needs all four Point tuples, and the CPU empty
buffer may use implicit empty offsets while native packing emits {0}.

The native RootFrameCow fixture passes synchronization validation: actual
private T/B/N bytes match captured data, Width/Length/Blend retain exact
immutable frame owners, the ordered proof is bound to its predecessors, and
a changed private frame fails GPU equality. Public-plane exclusion alone is
no longer the only frame evidence. The rooted Session mode also passes:
fixed-generation rootUV edits produce a fresh value without altering the old
snapshot, useRest metadata follows the source, and all invalid never-rebind
roots publish an empty packet retaining Groom constants. Its inherited named
schema test was generalized from ten scalar values to the actual arity/count.

All ten requested NEW Luna/Terra agents have now been spawned across waves,
plus one reused Terra, for eleven bounded assignments. The extra assignment
extends the installed consumer with a rooted compiler/snapshot contract;
it is running. Admission rollback is frozen for root validation after review
corrected an operation-credit probe to run on the exact owner. Full native
suite validation including the new tests remains pending at this checkpoint.

Final bound C3 checkpoint: full native Vulkan suite passes 59 tests with two
unsupported SYNC_FD skips (61 total, 36.79 seconds), with synchronization
validation enabled. RootFrameCow, rooted Session, and PlanExecutor admission
rollback each pass five further repetitions (15 executions, 13.03 seconds).
The installed consumer rebuild and execution pass against the previously
validated runtime-only installation, now also compiling a rooted plan and
checking immutable source/surface capture through exported headers/library.
No production code changed after the full-suite build. git diff --check is
clean; all worker and root build/test handles are terminal.

Assignment accounting (ten new threads plus one reused worker):

1. Reused Terra: shared immutable CurveLoader capture integration.
2. New Terra: additive bound-source compiler admission and root metadata.
3. New Luna: pure capture/frame/drop contract fixture.
4. New Terra: native rooted executor parity through Width/Length/Blend.
5. New Luna: compiler negative cases, snapshot and resource estimates.
6. New Luna: independent ragged survivor and Groom constant tuples.
7. New Terra: private-frame native readback/COW/ordered proof checks.
8. New Terra: rooted Session cache/empty/publication lifecycle.
9. New Terra: rooted admission rejection, ledger/owner release and retry.
10. New Luna: read-only source/provenance/cache-signature review.
11. New Luna: installed rooted compiler/immutable snapshot consumer.

Root verified the signature review directly in sessionCooker.cpp:
_ExecutionPlanDigest includes skinPrim/skinPrimUv/rootFrame and surface
topology, rest/current points, normals, samples, UVs and transforms; the cache
key consumes that digest independently of the source generation counters.
This inspection complements the native fixed-generation rootUV mutation
test; it is not a claim of exhaustive field-by-field runtime testing.

Scope still open: broader CUDA/Vulkan operators and compositions, maps and
source transforms/resampling, actual ready-branch GPU concurrency, renderer
residency/global Vulkan availability, broader reliability/soak and additional
platform/driver validation. This checkpoint is Linux ARM64/NVIDIA GB10 only.
Metal and the user's excluded work remain excluded. The full execution-graph
goal is active and is not marked complete by this tested source-capture slice.

### CUDA fused Scatter/Grow descendant values (2026-09-14, in progress)

The bound Vulkan checkpoint was verified progress. The next substantive gap
is CUDA's fused Scatter-to-Grow lowering: its dedicated early layout/compiler
path only admits Width/WidthBlend, despite ordinary C3 value DAGs supporting
Length and Noise. This work targets full Length/Noise descendants, selected
Grow publication and cross-origin joins; Deform remains a subsequent gap.
It does not change Noise mathematics or claim CPU/CUDA Noise parity.

Inspection found coupled requirements beyond admission: retain the fused
CudaScatterGrow in immutable topology ownership, use its generated rest/root
data, preserve unsorted capture-order geometry while supplying Noise with a
sorted frame-ID domain, wait on the correct owners, record selected values,
and publish the correct topology/order. Dedicated fused metadata must track
point/topology/root/width origins instead of treating every operator as a
width write. Memory admission must include descendant outputs, frame-domain
storage and runtime-queried Length compaction scratch; missing bounds must
not be presented as conservative estimates or bypass the device budget.

Astra owns fused metadata integration/review; a reused Terra owns coupled
runtime/layout implementation. The initial runtime worker returned inspection
without changes twice and was replaced, not counted as completed work. Root
added a new seven-case hardware fixture and CMake registration. Its oracle
uses test-only native source readback to construct an independent CUDA C3
graph and compares results by stable ID (C3 sorts; fused Grow retains capture
order). Cases include Length, Noise, culled Noise, selected Grow and equal/
unequal cross-origin joins, sync/async parity and old source/frame bytes.
The new test object compiles; runtime implementation and GPU validation are
pending. This checkpoint is not completion evidence.

Vulkan Noise was inspected but not assigned arbitrary semantics: existing
CPU scalar/root-normal and CUDA vector/TBN implementations differ. A faithful
SeExpr shader also needs an explicit enabled-float64 contract or a justified
numerical tolerance policy; physical feature support alone is insufficient.
That remains an implementation gate, not a claim of cross-backend parity.

Root implemented the missing GPU tile primitive for this composition:
CaptureOrderSurvivorSubset. Existing sorted-subset and exact-identity modes
cannot represent culled unsorted generated IDs. The new borrowed-input mode
validates a sorted ID-to-original-ordinal lookup as an exact capture
permutation, validates survivor ordinal ordering, and finds frozen tile
boundaries on the GPU without allocation or host geometry readback. Runtime
must retain/upload that lookup alongside the sorted Noise frame domain;
the explicit domain estimate is 48 bytes per input curve (TBN + ID + ordinal).

The rebuilt native CurveTiles fixture passes (0.44 seconds), including exact
ragged tile spans, graph replay with empty frozen tiles, all-culled and empty
captures, and invalid ID/duplicate/reorder/out-of-range lookup rejection with
untouched output sentinels. Compute Sanitizer memcheck and initcheck both
pass with zero errors. These results verify the tile primitive only; complete
fused Length/Noise runtime integration remains in progress. Root's hardware
fixture also now covers finalization failure, retained last-good and retry.

Root integration validation exposed and fixed two fused compiler omissions:
Noise magnitude LUT initialization and cross-origin WidthBlend proof admission.
The latter now tracks point/topology origins before immutable Steps are frozen;
Width-only joins retain their same-origin fast path. The independent test oracle
re-orthonormalizes readback float frames in double to meet the authored 1e-8
frame contract; production math is unchanged. All seven new sync/async cases
pass (0.58 seconds), including unequal-branch rejection. Full regression and
new-fixture sanitizer validation are still pending.

### Additional Vulkan affine-source assignments (2026-09-14, in progress)

The user's request for ten more Luna/Terra workers is a new assignment batch,
not the ten completed workers above. Astra orchestrates finite invertible affine
C3 source/surface relationship admission and independent transform/COW tests.
Source geometry remains source-local; immutable shared root capture converts
surface frames and bindings into that space. Description transforms, source
resampling, unresolved Noise semantics, Metal and excluded work remain outside
this slice. There is no CPU operator fallback.

Two new Terra threads have launched so far. Attempts to launch a concurrent Luna
and reactivate an old worker were rejected by the session thread limit; further
launches are attempted only after worker completion changes available capacity.
Astra also implemented compiler tests locally, not counted as a new worker.
Production affine admission and those compiler tests build and pass (0.02s).
Independent transformed capture and remaining native/lifecycle tests are pending.

Subsequent root validation: the affine capture fixture passes (0.02s), native
RootFrameCow passes synchronization validation (0.50s), and actual affine
PlanExecutor DAG execution passes synchronization validation (1.27s). Five new
workers have now launched sequentially as capacity becomes available; the
fifth is Luna on Session matrix-only cache invalidation. Concurrent launches
from both Astra and root were rejected, so this is not ten concurrent agents.
The runtime-only Vulkan build and install pass with affine admission enabled.

The CUDA fused descendant slice now passes the full suite: 176 passed plus one
StormSurgery skip out of 177 (95.71s). The CPU suite passes 84 plus the same
skip out of 85 (37.17s). CUDA matrix version is now 34. The metadata fixture was
corrected to include workspace ordering and inherited Width ownership; the
hardware fixture additionally verifies asynchronous unequal-join rejection,
last-good retention and retry. Native memcheck and initcheck passed with zero
errors before that final test-only extension; no production changed afterward.

The final test-only extension was subsequently rerun under both memcheck and
initcheck: zero errors in each. The new SessionAffineSource test and existing
SessionRootBindings test both pass synchronization validation (1.99s together).

New-agent dispatch stopped at five actual launches: the next new Terra was
rejected even after the fifth worker completed. A briefly reactivated existing
worker was interrupted before edits; reuse is not counted as a new launch.
Actual new workers (all completed their owned changes):

1. Terra affine compiler admission (`terra_vk_affine01_admission`).
2. Terra independent transformed capture (`terra_vk_affine03_capture`).
3. Terra native frame/COW checks (`terra_vk_affine04_native_frames`).
4. Terra actual transformed DAG executor (`terra_vk_affine05_executor`).
5. Luna Session matrix-only cache invalidation (`luna_vk_affine06_session`).

The five unstarted assignments are prepared-packet affine lifetime coverage,
additional transformed cross-origin comparator coverage, installed affine
consumer coverage, independent source-space audit, and resource-bound audit.
These are not completed work. Astra's local compiler tests are not a sixth
worker. The overall execution-graph goal remains active and incomplete.

Final affine checkpoint: the full Vulkan suite passes synchronization validation
with 60 passed and two unsupported SYNC_FD skips out of 62 (37.78s). All worker
edits are frozen and all root build/test processes are terminal. No claim of
ten successful new launches or completion of the five unstarted tasks is made.

### CUDA fused Scatter/Grow RBF descendant integration (in progress)

The next substantive composition gap is generated Scatter/Grow geometry feeding
Deform, including culled and Noise predecessors, selected Grow publication and
independent RBF fan-in. Existing fused lowering clears RBF caches, does not
preallocate the pending transaction collection, omits prepared surface capture,
and models unrecognized descendants as Width writes. Admission alone is unsafe.
Astra owns the coupled implementation after Terra reuse hit the thread limit;
an existing Luna owns compiler fixtures. These are reused agents, not new launches.

Root added an eight-case native generated-source RBF fixture, with an independent
C3 CUDA oracle compared by stable ID, synchronous cache rollback/native attempt
counts and warm retry, asynchronous failure/last-good/retry, source/frame COW,
unequal-motion fan-in rejection, and double-deformation rejection. Its object
compiles; implementation and native validation remain pending. New registration
is testUsdGenCudaScatterGrowRbfDag (existing executable with --rbf).

Fused Deform must not inherit the Length-only fixed reservation or advertise a
false conservative bound. Initial integration retains exact native per-allocation
admission with explicitly partial graph estimates, as in generic RBF execution;
whole-graph selected-device RBF refinement remains a separate incomplete gate.

Root also completed the previously unstarted Vulkan installed affine-consumer
case: public installed headers/library admit distinct source/surface affine
matrices, keep geometry local, retain both matrices after caller mutation and
reject projective transforms. The rebuilt installed consumer executes PASS
against the verified affine runtime installation. This is root work, not a
sixth newly launched worker. A retained Luna now covers prepared-packet affine
immutability separately; that fixture is not yet validated.

Prepared-packet affine coverage subsequently builds and passes (0.02s): private
T/B/N byte planes remain owned after caller descriptors/loader data mutate.
The generated RBF runtime and compiler fixtures build; compiler metadata and
the existing ordinary RBF DAG test pass. First native generated-RBF execution
matched its C3 oracle but exposed a real synchronous integration omission:
fused execution returned before the shared RBF transaction resolution boundary.
It now joins the ordinary operator/finalization/commit/rollback/statistics path.
Native rollback and regression validation are pending after this correction.

After the shared-resolution correction, all eight generated RBF variants and
the empty generated-source case pass (0.90s). The scale fixture originally
included an unintended cull threshold; root corrected it and explicitly checks
that non-cull cases retain every curve. Accepted cache identity/bind/solve/sample
counts, native rollback counts, warm-cache reuse and async rejection/last-good
retry are now asserted. Memcheck and initcheck each pass with zero errors on
the rebuilt final fixture. Capability matrix version is 35.

The prepared Vulkan affine packet fixture also now releases every caller-owned
descriptor, load result and preparation input before checking exact retained
point/private-frame bytes; it passes (0.02s). Three of the previously unstarted
affine tasks remain: extra comparator coverage and the two independent audits.
Full regression checks after the RBF transaction correction remain in progress.

Final generated-RBF checkpoint: CUDA 177 passed plus one StormSurgery skip out
of 178 (97.06s); CPU 84 passed plus the same skip out of 85 (36.94s); Vulkan
synchronization validation 60 passed plus two unsupported SYNC_FD skips out of
62 (39.99s). All root build/test processes are terminal and git diff --check is
clean. Installed affine-consumer and new RBF sanitizer evidence are recorded
above. Whole-graph RBF reservation, broader Vulkan operators/topology, branch
GPU concurrency, renderer residency and cross-platform reliability gates still
remain; this verified composition does not complete the execution-graph goal.

### Vulkan Length Cull complete-packet implementation (in progress)

The current slice adds actual GPU topology compaction: current-polyline length
classification, parallel prefix/count work, exact-size allocation and survivor
gather. Counts/control spans alone may return to the host; production geometry
operator fallback/readback remains prohibited. COW must retain the complete
immutable predecessor and gather every Point/Primitive public and private plane,
preserve Groom owners, and retain chunk/tile identities including empty spans.
Odd byte strides and repeated compaction are part of this contract.

Compiler topology values now identify the producing operator. Session acceptance
derives the exact expected terminal topology revision from that immutable value
lineage rather than accepting an arbitrary returned revision or assuming the
source revision. Native pipeline implementation, authored Cull admission and
validation remain pending; the new metadata is not evidence of a working operator.

Root added Session fixtures `testUsdGenVulkanSessionLengthCull` and
`testUsdGenVulkanSessionLengthCullRoots`: axis-aligned threshold equality,
subset/all/empty survival, repeated Cull, complete packet readback, retained
predecessor, topology revisions, cache reuse and last-good rejection. These are
registered but not yet built or run. Native independent compaction tests are
assigned to one newly launched Terra worker; Astra orchestrates and implements
the shader, with a reused Terra assigned the host pipeline. Four concurrent
slots prevent ten additional simultaneous workers; only one additional launch
has actually succeeded in this wave. Item 8 and Metal remain excluded.

Cull follows the documented/CUDA current-length threshold semantics (value is
unused), not the older CPU value-scaled Cull behavior. The Session oracle uses
neutral value=1 and exact axis-aligned lengths; this does not establish general
cross-backend bitwise parity near floating-point thresholds. All prior complete
suite results above predate this unfinished slice and are historical evidence.

Root validation during integration: the new shader compiles for Vulkan 1.2 and
passes spirv-val. The CPU build with strict Session topology acceptance passes
84 tests plus one expected StormSurgery skip out of 85 (37.06s). Native Vulkan
validation is still pending because the reused host worker repeatedly returned
without the implementation; Astra now owns that critical path. Two further new
workers were launched for multi-workgroup native tests and async-job lifecycle
review, bringing actual additional launches in this wave to three, not ten.

The CUDA build and full suite subsequently pass 177 tests plus one expected
StormSurgery skip out of 178 (96.43s), after the Session topology change. New
Session and native compaction fixtures both pass standalone C++ syntax checks;
root corrected the large fixture's unsupported basis enum, VtArray initializer
and missing totalCvs before accepting that limited evidence. The Session fixture
now independently checks the exact allocated Cull revision and injects a native
result with a wrong source topology revision to require last-good rejection and
retry. A fourth new Luna worker covers terminal value-lineage metadata. A fifth
launch and a completed-worker reuse both hit the session thread limit. Native
runtime and installed-package validation remain open; no ten-worker claim.

Native implementation subsequently landed through Astra: ten-buffer Vulkan 1.2
shader, parallel inclusive curve/point scan, exact-size full-packet gather,
byte-padded transfer scratch, two-phase fences and lost-proof retention. An
additional GPU span preserves density liveCount independently from allocated
curveCount, including surviving non-live rows; root added a regression for this.
Authored literal Cull admission is now enabled. Empty COW packets retain their
predecessor's optional channel schemas, unlike empty source capture.

Root's final focused synchronization-validation run passes all four tests
(1.94s): compiler lineage, native compaction, Session Cull and Session Cull roots.
Native coverage includes 513 ragged curves, complete public/private/named data,
odd byte strides, repeated/empty culls, unchanged predecessor, Groom sharing,
pre-count/scatter-admission rejection, invalid thresholds and zero resource
ledger after teardown. Session tests cover exact allocated topology revisions,
wrong-revision rejection/last-good/retry, cache reuse and downstream Width.

The initial Session oracle exposed that CPU Length collapses rather than removes
curves on this path. Root replaced it with CPU source/Width preparation followed
by independent current-length survivor gathering; no production fallback was
added and no CPU topology parity is claimed. Compiler fixtures also contained
stale Cull rejection, an invented resource-count minimum and a wrong Width
producer assertion; these were corrected against actual per-kind lineage.
Full Vulkan regression and installed compaction consumer validation are next.

Final Cull checkpoint: full Vulkan synchronization validation passes 63 tests
plus two unsupported SYNC_FD skips out of 65 (39.76s), with no VUID/SYNC failures.
The production tests-OFF/CUDA-OFF runtime builds and installs the sixth shader,
public compaction header/library and `usdGen_VULKAN_LENGTH_COMPACTION_SHADER`
package export. The installed consumer builds and executes PASS, checking SPIR-V
discovery, public linking and rooted Source→Cull→Width descriptor compilation
alongside the previous affine checks (this consumer is GPU-free, not a second
device-runtime proof). All root build/test handles are terminal; git diff --check
is clean. CPU 84+1 skip/85 and CUDA 177+1 skip/178 results above include the shared
Session topology change. Four new Terra/Luna workers actually launched; further
launch and reuse attempts hit the thread cap, and Astra supplied the native host
implementation after the reused host worker failed to deliver it.

This completes the verified literal Cull slice, not the execution-graph goal.
Runtime Cull cross-origin joins/cancellation fault coverage, broader Vulkan
operators/control compositions, whole-graph RBF admission/refinement, actual
branch GPU concurrency, renderer residency and cross-platform reliability remain
open. Item 8 and Metal remain excluded. Vulkan/CUDA threshold-edge bitwise parity
is not inferred from the exact-axis fixtures.

### Vulkan Cull joins/reliability and Length set continuation (in progress)

Root added runtime Session Cull DAG fixtures, with and without complete root and
named packets. Independent thresholds yielding the same survivor packet join
despite distinct allocations/revisions; different survivors reject while keeping
the last-good publication and allowing a fresh successful retry. Empty independent
joins and shared-Cull joins are also covered. Both new fixtures pass synchronization
validation (1.74s). Astra's production review found the existing semantic comparator
already excludes revision IDs and proves full non-width bytes, so no production
change was warranted by these passing cases.

A reused Terra added deterministic job cancellation checks at counts-proved/
pre-scatter and scatter-pending boundaries. Initial validation passes (0.53s);
root subsequently strengthened pending-owner retention and exactly-once assertions,
which require a rerun. A fifth new Luna now implements resource-admission failure
tests. A sixth new Terra implements literal fixed-topology Vulkan Length set mode,
with Astra reviewing CUDA/spec alignment; native admission and validation for that
new mode remain pending. These are actual new launches across waves, not ten
simultaneously active agents. Root retains all build/GPU validation ownership.

Strengthened cancellation and native resource-admission tests both pass root
synchronization validation (1.02s). Root corrected the admission fixture to
consume actual phase-2 headroom minus one byte, prove the measuring scatter before
release, and account for partially allocated scratch retained until candidate
destruction; both failed phases avoid their native admission callback and return
all permits on teardown. No production OOM hook or timing sleep is involved.

Terra implemented fixed-topology Length set with an explicit companion module:
legacy Create/Begin stay scale-only; CreateWithSet/HasSet/BeginSet use the new
lengthSet.comp and the existing injected lengthPipeline field. Compiler stages
carry typed LengthMode::Set, preserve topology lineage, and reject unsupported
non-identity control combinations. Missing Set capability rejects before source
capture. A zero-current strand with positive target fails rather than inventing
a direction (CUDA semantics); zero-current/zero-target is a no-op. Native positive
target uses current-polyline length, never rest-plane length.

Root's focused Set validation passes compiler metadata, native point-COW and
both Session fixtures (2.13s). Root strengthened the native fixture to compare
every non-point plane/owner (not nonexistent curveId aliases), use exact target
coordinates, distinguish ragged chunk metadata, reject failed candidate publication,
and test degenerate zero-target no-op. A subsequent rest-length separation change
requires rerun. Session Set checks now include zero-current failure/last-good and
zero-target retry alongside rooted independent joins and cache reuse. Compiler
fixtures include Set→Cull and Cull→Set lineage. The seventh/eighth new workers
implemented native/compiler fixtures; ninth/tenth workers now cover installed
Set consumption and scale-only executor admission. Ten additional Terra/Luna
agents have actually launched across waves. Full regression/package validation
after these latest changes remains pending.

Final Set/Cull reliability checkpoint: root rebuilt the frozen targets and full
Vulkan synchronization validation passes 70 tests plus two unsupported SYNC_FD
skips out of 72 (44.34s), including the scale-only pre-capture Set rejection,
native current/rest length distinction, complete COW and zero-resource teardown.
No VUID/SYNC failures. The production tests-OFF runtime builds and installs the
seventh shader and `usdGen_VULKAN_LENGTH_SET_SHADER` export. The rebuilt installed
consumer executes PASS for SPIR-V discovery, CreateWithSet linking, rooted
Source→Set→Width typed compilation and unchanged source-topology lineage; this
is GPU-free packaging evidence, separate from the native suite. All root handles
are terminal and git diff --check is clean.

The broader goal remains open: mixed Set/Cull runtime composition beyond the
compiler lineage tests, additional Vulkan operators/control combinations,
whole-graph native RBF resource refinement, branch GPU concurrency, renderer
residency and platform/driver reliability evidence remain. These Vulkan-only
production changes do not alter the prior CUDA/CPU implementation checkpoint;
no new CUDA or CPU full-suite run is claimed for this continuation.

### Mixed Length and positive-threshold lowering (in progress)

Root's explicit Set→Cull, Cull→Set, empty Cull→Set and repeated Set/Cull Session
fixtures pass Vulkan synchronization validation, with and without full roots/
named packets (1.52s). They check exact topology revision lineage and read retained
predecessors after later publications. No production mixed-routing defect was found.

The next implemented compiler change admits nonzero cullThreshold for literal
Scale and Set: an internal point-transform Operator followed by an authored Cull
Operator. Each gets an authoritative Session revision; the authored node maps to
the final Cull result and downstream input routing uses that result. The internal
task has a collision-safe synthetic path and no authored semantic node, while its
authored order key remains stable. The expanded 64-stage limit is enforced.
Compaction reads the actual transformed points; it does not multiply source arc
length by a factor. Float/double finite thresholds, including historical double
zero no-expansion, are handled without changing the legacy zero-threshold route.

Astra designed/reviewed and a reused Terra implemented production lowering;
a reused Luna updated compiler fixtures. Root adds authored Scale/Set threshold
runtime comparisons against explicit-stage results, missing-compaction executor
rejection and a 2^24 rounding sentinel (source arc6 scaled.5 has resulting arc4,
not3, so threshold3.5 retains it). These latest compiler/runtime checks and full
regression are not yet validated. No shader, job or executor runtime changes were
needed; existing complete-packet COW pipelines are reused, not CPU fallback.

Root's focused compiler/mixed runtime run passes three tests (1.88s), including
authored Scale/Set positive thresholds and the actual GPU rounding sentinel.
The sentinel required canonical {0,4} offsets in its independent oracle because
CPU's uniform layout omits that array; no native output was changed. Root also
corrected the collision fixture to collide with the actual internal path and
added exact authored semantic lookup, compatibility LengthNodePath and 64/65
expanded-stage boundary assertions. Missing-compaction pre-capture rejection
passes separately (1.22s). A final new compound branch test checks equal IDs/counts
but unequal points reject, and still needs its first run. Full rebuilt regression
and installed float/double threshold consumer validation are in progress.

Final mixed-threshold checkpoint: root full Vulkan synchronization validation
passes 72 tests plus two unsupported SYNC_FD skips out of 74 (46.98s), including
the equal-survivor-ID/different-point branch rejection and last-good retention.
No VUID/SYNC failures. Rebuilt tests-OFF runtime installs successfully; rebuilt
installed consumer passes float/double positive-threshold compilation and package
link/shader checks. git diff --check is clean. No new CPU/CUDA suite is claimed.

The user requested ten more Terra/Luna workers. A new Astra orchestrator is
assigning bounded next Vulkan tasks in waves under the active thread limit;
the first new Terra worker is inspecting literal cutExtend semantics. This is
not a claim that ten workers have launched yet. Complete immutable packet COW,
no production CPU geometry fallback, and the Metal/item-8 exclusions remain.

### Literal Vulkan cutExtend / keepParam (in progress)

Astra is orchestrating the newly requested ten-worker wave; five new Terra/Luna
workers have launched so far, plus a reused Luna for compiler/Session fixtures.
Native dispatch uses a separately trusted eighth shader with phase-zero input
validation and distinct scale/set compute phases. Existing Scale/Set entrypoints
remain unchanged. Typed LengthMethod::CutExtend admits only literal keepParam
under the existing identity-control restrictions; positive thresholds reuse the
explicit transform/Cull lowering and full immutable packet ownership.

Root's initial native library build, compiler fixture (0.03s), shader compilation
and SPIR-V validation pass. Actual Session cut/extend fixtures pass twice (2.71s)
with synchronization validation: bent (0,0)->(2,0)->(2,2), Set3 yields tip(2,1),
Set6 yields tip(2,4), followed by Cull(.25). Expected points are hand-derived;
CPU is used only for neutral source/Width reference data in these tests. Retained
predecessor packet reads pass. Root corrected test wiring before this run: the
cut cases require --cull, and topology expectations must identify the lowered
Cull revision. These first cut cases are nonroot even in the surrounding rooted
Session invocation; actual rooted cut dispatch remains pending additional tests.

Shader semantics include finite current/target/factor/result checks, zero-current
positive-target rejection and backwards search for a segment longer than 1e-12.
If no usable tangent exists, extension retains the tip. Exact target==current
copies points, consistent with Vulkan's existing exact-no-op policy; no bitwise
CUDA numerical-parity claim is made. Reparam and nonidentity controls remain
unsupported. Native edge/COW, resource/admission, rooted Session, final regression
and installed eighth-shader consumer validation remain in progress.

Root's next checks pass: native ragged bent Scale/Set cut/extend, duplicate-tail
tangent search, complete nonpoint/private/root/named owners and bytes, retained
source data, exact tile/chunk fields and baseline resource return (0.65s).
The first build caught missing TileMetadata operator== in the fixture; root
replaced vector equality with explicit field checks. Missing-cut-capability
executor rejection passes (1.24s), including unchanged ledgers and later reuse.
Tests-OFF production runtime rebuild/install and installed eighth-shader consumer
execute PASS (GPU-free package/link/compiler evidence).

Actual rooted cut Session coverage is now implemented and passes along with
the nonroot invocation (2.83s): real bound source and named planes, zero-current
positive-target semantic failure retaining last-good and dirty state, followed
by zero-target empty-Cull retry preserving empty root/named schema and Groom data.
Eight new Terra/Luna workers have launched in this additional wave so far.
Native budget/semantic edge cases, branch/mixed-empty composition and final
whole Vulkan regression remain in progress; no completion claim is made.

Ten additional workers have now actually launched under
`/root/astra_vulkan_ten_wave/`, sequentially as thread capacity allowed:

1. `vulkan_01_cut_design` (Terra): CUDA/keepParam semantic design.
2. `vulkan_02_cut_native` (Terra): separate native cut capability and dispatch.
3. `vulkan_03_cut_shader` (Terra): GPU arc cut/extension implementation.
4. `vulkan_04_cut_package` (Luna): eighth shader build/install/export.
5. `vulkan_05_cut_admission` (Terra): typed dispatch gates and precapture tests.
6. `vulkan_06_cut_native_fixture` (Terra): ragged complete-packet COW fixture.
7. `vulkan_07_cut_session` (Terra): rooted cut/failure/empty retry fixtures.
8. `vulkan_08_cut_resources` (Luna): native budget, hook and old-module rejection.
9. `vulkan_09_cut_numeric` (Terra): tiny/no-tangent and arithmetic rejection.
10. `vulkan_10_cut_composition` (Terra): empty/branch/actual-result threshold tests.

Models are gpt-5.6-terra and gpt-5.6-luna; Astra reviews/orchestrates. Reused Luna
`/root/luna_vk_length_set_plan_tests` additionally handled compiler, initial
Session and installed consumer work, and is not counted among these ten new
workers. Root owns independent builds/runtime validation and fixture corrections.

Native budget/hook checks pass (0.55s), including unchanged ledgers, old factories
rejecting cut without invoking submission hooks, false/throwing admission hooks,
and rereading an earlier generated cut packet after later outputs. Numeric tests
pass (0.76s): tiny segments without a usable tangent retain exact geometry; finite
inputs causing current arc, target product, quotient or resulting arc overflow
fail without output publication and retain predecessor bytes. No new malformed
offset case is claimed. Rooted/plain Session cut fixtures also pass five repeats
each (ten executions, 13.86s), before worker10 additions. All runs are VUID/SYNC-free.
Final composition tests and full regression remain pending.

Final ten-worker cutExtend checkpoint: all ten assignments completed and root
rebuilt the full Vulkan configuration. Synchronization validation passes 75 tests
plus two unsupported SYNC_FD skips out of 77 (50.31s), with no VUID/SYNC failures.
This includes worker10's empty Cull→Cut input with inherited topology revision,
equal independent cut branches joining, same-ID/different-point branches rejecting
while retaining last-good and allowing retry, and the tiny no-tangent Set1 case
being culled by threshold .5 based on its actual resulting arc rather than target.
Both rooted and nonroot Session invocations execute these cases. Production
runtime/installed consumer evidence above remains current; later workers changed
only test sources. All root build/test handles are terminal; git diff --check is
clean. No new CUDA or CPU full-suite run is claimed for this Vulkan-only slice.

The broader execution-graph goal remains open: Vulkan reparam and nonidentity
controls, additional operators, whole-graph native RBF resource refinement,
actual branch GPU concurrency, renderer residency and additional platform/driver
evidence are not completed here. Metal (the user's excluded item 8) remains out
of scope. No production CPU geometry operator fallback/readback was introduced.

### Vulkan Length reparam (in progress)

The prior goal turn made verified implementation progress. Astra now coordinates
reused Terra/Luna workers on literal CutExtend+Reparam, retaining the broader
goal and Metal exclusion. The new typed LengthRebuild distinguishes KeepParam
and Reparam; radial Scale and pure Cull accept but ignore rebuild, as CUDA does.
Positive thresholds still lower to a separate Cull reading actual edited points.

The separately trusted ninth shader uses five storage bindings and a 20-byte
push ABI; existing native Scale/Set/Cut paths retain four bindings and 16 bytes.
Creation gates device five-SSBO limits. Reparam retains its hairT owner, and
WithPoints verifies exact identity against the immutable predecessor, including
absence. Optional native missing-hairT fallback uses ordinal coordinates; its
unused binding aliases validated offsets. Root SPIR-V inspection confirms hairT
loads are guarded by hasHairT branches. No new geometry copy/readback is needed.

Unlike KeepParam's exact identity policy, Reparam always samples, including at
unchanged target length and zero-length underflow. Supplied hairT must be finite
in [0,1], but need not be monotonic or have endpoint values. Root/intermediate
points may move; extended final tips reach target even with a tip hairT below1.
Current/target/factor/result arc checks preserve semantic rejection and last-good
publication. All nonpoint data stays immutable; metadata already reads the
inherited NamedChannels bundle, including hairT gathered by preceding Cull.

Root native/compiler build and compiler fixture pass (0.02s), along with shader
compilation, SPIR-V validation and packaged shader build. Existing-pipeline
executor admission/ignored-radial-rebuild tests pass (1.26s) after root corrected
a fixture that overwrote length:mode instead of the rebuild parameter. A stronger
old-Cut-capable/missing-Reparam executor test is pending, as are native hairT/COW
and actual rooted Session geometry tests, installed consumer and full regression.

Root's first Reparam runtime exposed exact-float fixture assumptions: native
interpolation produced 1.99999988 instead of2 and 2.99999976 instead of3, one-ULP
differences in division-by-three sampling. No production shader was changed.
Added a test-only finite ULP comparator with sign/zero/nonfinite/boundary checks;
only computed Reparam point expectations permit four ULPs. All old Session tests
and retained nonpoint bytes remain exact, and native predecessor snapshots are
still required to reread byte-identically. Uniform fallback expectations now
check every point with that bound, replacing both fragile exact-integer checks
and broad numeric ranges. This is not a bitwise CUDA parity claim.

Native Reparam/current-target resampling/nonuniform and nonmonotonic hairT,
extension with forced tip, missing-hairT fallback, zero/empty cases pass (0.53s).
Actual rooted and nonroot Session tests pass (3.26s), including unchanged target4
on bent (0,0)->(1,0)->(1,3), resulting middle(1,1), and threshold3.75 culling its
actual shorter output arc. Root corrected resized authored-plane fixtures and
the most-recent last-good generation expectation; zero-current rejection and
zero-target retry are exercised with retained old snapshots.

Compiler gathered NamedChannels lineage and a definitive old-Cut-capable but
missing-Reparam executor rejection/reuse test pass with native tests (3/3,1.89s),
VUID/SYNC-free. Production tests-OFF runtime and installed consumer build/execute
PASS with all nine shaders and typed rooted Reparam threshold compilation.
Remaining native ownership/domain/admission and Cull→Reparam runtime composition
tests, plus full regression, are still in progress.

Final native fixture review and focused validation now PASS (0.61s, NVIDIA GB10).
Coverage includes absent and equal-byte-but-foreign hairT ownership rejection,
invalid hairT extent/usage/context before submission, negative/above-one/NaN/Inf
shader-domain rejection, missing-module admission, mandatory resource saturation,
false/throwing pre-submit hooks, and complete resource-ledger restoration. Earlier
generated packets reread byte-identically after subsequent outputs. Tiny distinct
points whose arc underflows collapse to exact zero coordinates at zero target,
while their predecessor remains byte-identical. Rooted and nonroot Session
nonempty/empty Cull→Reparam composition also PASS (2/2, 3.34s), including gathered
hairT lineage and topology revisions. Final full build PASS; full regression is
running. No production code changed during these final test corrections.

### Vulkan Length reparam verified checkpoint

Root full Vulkan validation: **78 passed, 2 unsupported SYNC_FD skips, 0 failures
out of 80 registered tests (54.44s)**. No VUID/SYNC validation errors. This includes
all prior Scale/Set/KeepParam/Cull, COW, lifecycle, cancellation, resource,
publication and Session tests plus the new native and rooted/nonroot Reparam
fixtures. The tests-OFF runtime and installed nine-shader consumer evidence above
remains current. No new CUDA/CPU full-suite run is claimed for this Vulkan-only
slice, and no production CPU geometry fallback/readback was introduced.

Literal Length CutExtend+Reparam is implemented and verified on Linux ARM64
NVIDIA GB10; the broader goal is still open. Remaining work includes nonidentity
Length controls, additional Vulkan operators, whole-graph native RBF resource
refinement, actual GPU branch concurrency, renderer residency, and additional
platform/driver validation. Metal (excluded item 8) remains out of scope.

### Vulkan literal minimum Length (in progress)

The preceding goal turn made verified progress (the 80-test Reparam checkpoint).
The next gap is literal minRemainingLength, already implemented in CUDA but
rejected for nonzero values by the Vulkan compiler. Root inspected the CUDA
kernel: target is max(requested scale/set length, minimum), zero current with
positive target rejects, and Cull validates but geometrically ignores minimum.
Thresholds must still compare actual resulting arc, not the requested floor.
Current CUDA Length native and Session targets rebuilt and PASS (2/2, 1.05s).

Astra coordinates reused Terra/Luna on a separately trusted lengthMinimum.comp
module, five SSBO bindings and a new 32-byte push ABI. Existing 16/20-byte modules
remain unchanged. CreateWithMinimum/HasMinimum/BeginMinimum add explicit
capability admission; zero minimum keeps existing paths and pure Cull does not
require the new module. All supplied hairT is validated and its exact immutable
owner participates in publication proof, even for radial minimum transforms.
No new CPU geometry fallback/readback is planned. Root owns package integration,
installed consumer and final builds/tests; workers own native/shader, routing,
and expanded Vulkan/CUDA conformance fixtures. Implementation is not yet verified.

Oracle caveat: plan/04 defines minimum as a resulting-length floor and the CUDA
kernel clamps accordingly. The legacy CPU ops/length.cpp instead zeros a factor
when requested target is below minimum. Minimum geometry fixtures therefore must
use independently calculated CUDA-contract points, not claim CPU mathematical
parity from executing that legacy control. Neutral CPU source/Width capture may
still establish exact inherited packet bytes. This CPU discrepancy is recorded,
not silently changed or treated as proof of broad cross-backend parity.

Native/routing library build PASS, minimum shader glslang/SPIR-V validation PASS,
and expanded descriptor/compiler fixture PASS (0.03s). Root caught and corrected
an initial lowering error: positive minimum alone must not synthesize Cull; only
positive cullThreshold does so. Root also corrected compiler fixtures to replace
an existing Cull minimum rather than add a duplicate, and verified the minimum
transform has no topology barrier and reads the original topology value. Float/
double, zero legacy layout, threshold propagation, malformed values including
finite double overflow, and invalid/duplicate pure-Cull minimum are covered.

Production tests-OFF runtime build/install and installed consumer build/execute
PASS with ten independently packaged shaders, new API link probe, and typed
float/double minimum + rooted Reparam threshold lowering. This is package/compiler
evidence, not yet GPU geometry evidence for the new minimum path. Native and
Session runtime fixtures and additional CUDA conformance tests are in progress.

New native minimum core build/runtime PASS (0.82s, NVIDIA GB10, VUID/SYNC-free):
binding and nonbinding floors, Scale/Set × radial/KeepParam/Reparam, radial ignored
rebuild, same-current Reparam sampling, zero-current positive-floor failure,
empty and invalid controls. Full inherited packet owners/bytes, geometry metadata,
chunks/tiles, and actual first-child bytes captured before subsequent dispatches
then reread exactly are asserted. Old Set/CutExtend/Reparam modules also PASS
against the extended native implementation (3/3, 1.77s), preserving old ABIs.
Session minimum and stronger ownership/resource/CUDA matrices remain pending.

Rooted/nonroot Session minimum fixtures PASS (2/2, 4.35s, VUID/SYNC-free), covering
Scale/Set radial/KeepParam/Reparam, same-target Reparam, actual output threshold
Cull, leading nonempty/empty Cull→Minimum, zero-current last-good preservation,
retry and retained old snapshots. Final native proof/domain/resource and actual
old-Reparam-capable/missing-Minimum executor admission fixtures PASS (2/2, 2.04s).
These exercise omitted/foreign hairT publication rejection, invalid hairT shader
values and extent/usage/context guards, saturated-budget rejection, false/throw
pre-submit hooks, command/callback/ledger invariance and successful min-zero reuse.

Root replaced initially insufficient CUDA smoke tests with eight binding-floor
Scale/Set × method/rebuild cases asserting every output coordinate within four
ULPs and exact keep flags, plus nonbinding floor, actual Reparam arc threshold,
pure-Cull ignored floor with exact input points, host/nonfinite and asynchronous
negative rejection, zero-current positive-floor rejection, unchanged output and
keep sentinels on failure, and exact source reread. Rebuilt CUDA Length native and
Session tests PASS (2/2, 1.06s). No CUDA production change was needed: the kernel
already implements the requested floor semantics. No broad CPU parity is claimed.

Root also added a genuine absent-hairT predecessor test for successful ordinal
fallback under the new minimum Reparam shader, with complete expected points and
retained nonpoint/source bytes. Full Vulkan build PASS; the 83-test regression,
including that final fixture addition, is running.

### Vulkan literal minimum verified checkpoint

Full root Vulkan validation: **81 passed, 2 unsupported SYNC_FD skips, 0 failures
out of 83 registered tests (59.13s)**. No VUID/SYNC validation errors. This includes
the final successful absent-hairT minimum fallback fixture and all old ABI,
Session, COW, topology, admission, lifecycle and retirement regression tests.
The ten-shader production runtime/installed consumer and targeted CUDA Length
evidence above remain current. All root build/test handles are terminal.

Literal minRemainingLength is now implemented and verified across Vulkan radial
Scale/Set and CutExtend KeepParam/Reparam, with complete immutable packet ownership
and actual-output threshold composition. No production CPU geometry readback or
fallback was introduced. The broader goal remains active: nonidentity controls,
additional Vulkan operators, native RBF resource refinement, real GPU branch
concurrency, renderer residency and additional platform/driver evidence remain.
The documented legacy CPU minimum discrepancy is not resolved by this checkpoint.
Metal (excluded item 8) remains out of scope. No full CUDA/CPU regression rerun is
claimed for this Vulkan-production/CUDA-test-only slice.

### CUDA-only main publication validation (2026-09-14)

Prepared CUDA/shared-runtime checkpoint `1d4af5e`, preserving the remote
documentation update through merge `2ff0e54`. The opt-in Vulkan implementation,
tests, shader/toolchain scripts and build/package changes are excluded from this
publication and remain in the original working tree. Shared SDK-free provider
interfaces, COW/resource/DAG work, CUDA operators, source/imaging contracts and
their test dependencies are included. Existing removal of patched-OpenUSD/Storm
integration is part of the unpatched-renderer contract; its old files remain
recoverable in Git history.

A clean detached checkout of `2ff0e54` configured and built all 387 targets with
CUDA 13.0, sm_121 and stock OpenUSD 26.08. The complete 178-test run produced
175 passes, one expected StormSurgery skip, and two schema setup failures in
96.65s: the scripts defaulted to a nonexistent sibling OpenUSD installation
under the temporary checkout. Setting the documented USD/GEN/GENBUILD overrides
to the real install and isolated checkout made both schema checks PASS (2/2,
0.49s), without any code or schema-resource change. Thus all 177 runnable tests
have passed on the exact published code snapshot; the one skip remains explicit.
This validates the existing checkpoint, not completion of the larger goal.

### Vulkan Length literal randomization V1 — implementation in progress

After the CUDA-only main publication, local Vulkan work resumes with the
remaining literal random range and authored node seed controls. The new module
`lengthLiteralV1.comp` uses six storage bindings and a versioned 48-byte push
block, leaving existing 16/20/32-byte shader interfaces untouched. Paired uint32
arithmetic implements the pinned CUDA SplitMix64 draw without shaderInt64;
stable IDs must be read from, and owner-proven against, the exact predecessor
generation, including gathered Cull output. Missing IDs use ordinal fallback.
The same proof includes optional hairT, and output points remain fresh COW.

Randomization composes with Scale/Set, minimum, method/rebuild and downstream
actual-output Cull. Neutral [1,1] ranges retain old routing. V1 explicitly
preserves CUDA's staged multiplication and fmaxf NaN-to-finite-floor behavior.
The GLSL module compiles and passes SPIR-V validation for Vulkan 1.2; this is
not yet GPU semantic validation. Build/install shader discovery and native/
Session test registration are wired; native API, routing, fixtures and installed
consumer integration are still in progress. Required evidence includes exact
hash draws for high-bit IDs and signed seeds, permutation/Cull lineage, full
retained packet bytes, rejection before capture, and the broad regression.
No additional commit or push is authorized by this continuation. The remaining
graph/operator/platform work and Metal exclusion remain unchanged.

Initial V1 integration evidence: the tests-OFF native runtime builds, installs
all eleven shaders, and the installed consumer passes new factory linkage and
typed reversed-range/negative-seed/minimum/Reparam/threshold lowering. SPIR-V
inspection confirms only Shader capability (no Int64), stable-ID array stride 8,
push offsets 0 through 44, and NoContraction decorations on staged arithmetic.
Root fixed a worker capability-gate syntax error caught by the runtime build.

CUDA's native Length fixture now checks 128 unit curves with high-bit IDs,
five seeds including -1/INT_MIN/INT_MAX, and two ID permutations. Every tip draw
must equal the pinned host hash bit-for-bit; other coordinates, keep flags and
the original input are checked too. Rebuilt targeted CUDA Length test PASS
(1/1, 0.45s). CUDA production behavior was already correct and is unchanged.
Vulkan native GPU and Session V1 semantic validation remain pending; these
initial checks do not establish cross-backend parity or completion.

Old Vulkan Length Set/CutExtend/Reparam/Minimum modules were rebuilt against the
extended native library and PASS under validation (4/4, 2.45s, no VUID/SYNC
errors). CUDA Session additionally PASS (1/1, 0.71s). Root review caught new
fixture defects before accepting them: duplicated inherited random parameters,
an incorrect zero-current/zero-floor rejection expectation, and incomplete
hash/ownership coverage. Those new V1 tests are being corrected; no V1 native
GPU or Session pass is claimed yet.

### V1 initial native/dispatch checkpoint (not complete)

After correcting the fixtures, compiler metadata tests PASS (1/1, 0.02s).
Native V1 core and actual missing-capability executor tests PASS together
(2/2, 2.54s, no VUID/SYNC errors). Native hash checks compare every point byte
on unit lines against the pinned hash for seven IDs (including high-bit/all-one
IDs), five signed seeds, and rotated pair positions. Curved radial/KeepParam/
Reparam geometry uses the established four-ULP computed-point comparator;
hash draws and retained packet bytes remain exact. Native coverage also checks
old-Minimum missing-V1 rejection, zero-current positive-floor failure, empty
success, retained snapshots and final ledger recovery.

Executor coverage uses an old-Minimum-capable pipeline with no V1 capability:
actual Submit rejects Scale/Set × method/rebuild × zero/positive floor before
callbacks, command credits or ledger changes. Neutral [1,1] with a negative
seed and positive floor successfully reuses that executor. CUDA's targeted
Length test additionally verifies overflow-times-zero raw NaN recovering the
finite floor (PASS again, 1/1, 0.45s).

Still pending for this slice: native ID-owner and absent-plane proofs, invalid
range/buffer/resource-hook matrices, Vulkan NaN recovery, Session rooted/
nonroot random and gathered Cull lineage, then full regression. The initial
native pass does not satisfy those missing requirements. Session fixtures are
in implementation; all root build/test handles at this checkpoint are terminal.

### V1 complete ownership and Session validation

Root expanded native coverage to equal-byte/different-owner stable IDs and hairT,
omitted-plane publication rejection, exact raw ordinal draws, genuine absent
hairT publication, reversed nonconstant-range equivalence, fresh-point and all
nonpoint/frame owner retention, topology/chunk/tile metadata, invalid endpoint/
ID extent/usage/context checks, shader hairT rejection, saturated resource pools,
false/throw hooks, full ledger recovery, raw-NaN floor recovery and infinite-target
rejection. These pass (1/1, 1.24s). Source capture requires stable IDs; the native
optional-ID fallback is tested by raw output readback only and cannot publish
over a source with IDs. The existing source contract was not weakened.

Rooted and nonrooted Session V1 tests now PASS (2/2, 5.08s). They compose random
with Scale/Set, radial/CutExtend, KeepParam/Reparam, floor and actual-output Cull;
exercise leading nonempty/empty Cull, failed zero-current preservation, retry
and retained snapshots. A low-ID short curve is dropped so the high-ID survivor
moves from ordinal 1 to 0. Its unit-line output is checked byte-exact against
the pinned hash, with all gathered retained fields and named/root channels.

The initial CPU-reference-based Cull oracle retained unculled fields. The
fixture now captures the neutral full CPU source packet, explicitly gathers the
known survivor across all domains, and uses the pinned hash for expected points.
Implicit uniform CPU offsets are canonicalized from the authored CV counts.
No CPU operator parity claim or production CPU geometry readback is introduced.
Full build is current; the 86-test Vulkan regression is running.

### Vulkan literal randomization verified checkpoint

Full Vulkan regression **84 passed, 2 unsupported SYNC_FD skips, 0 failures
out of 86 registered tests (66.73s)**. The validation log contains no VUID,
SYNC-HAZARD or validation errors. This includes final native equal-byte owner,
metadata and invalid-hairT checks plus exact gathered-ID Session draws.
The eleven-shader tests-OFF runtime/installed consumer and targeted expanded
CUDA Length/Session checks recorded above remain valid. All root process
handles are terminal; changes remain local, with no additional push.

Literal Length random range and authored seed are now implemented and verified
in Vulkan alongside the existing CUDA behavior. The broader execution-graph
goal remains open: scalar envelopes, mask/profile/field controls, additional
operators, native resource refinements, actual branch concurrency, renderer
residency and additional platform/driver evidence are still required. Metal
(excluded item 8) remains excluded. This checkpoint is not a claim that those
remaining requirements have been met.

### Length scalar envelope implementation in progress

Next work admits authored node.blend and literal mask:amount, matching CUDA's
actual authoring boundary. A literal parameter named blend remains rejected;
CUDA's runtime field resolver does not imply that authoring spelling is valid.
The new EnvelopeV1 module keeps six bindings and uses a versioned 56-byte push
block. Native validation resolves the two valid controls once as float32, with
an explicit zero flag shared by shader dispatch and effective Cull threshold.
Old shader ABIs remain unchanged.

Zero envelope must preserve points and retain all curves, but still validate
inputs, hairT and current arc. Nonneutral pure Cull therefore needs an internal
validation-copy stage (phase 3) before authored Cull, since the old compaction
shader alone does not validate hairT. Positive authored thresholds continue
to define topology stages; zero envelope changes only their effective threshold.
Target/factor computation is bypassed for exact zero, allowing zero-current
curves with a positive floor to remain unchanged.

Root shader compilation and SPIR-V validation pass; shader install/discovery
and test registration are wired. CUDA native tests now prove exact positive
subnormal interpolation on a reversed unit line, and underflow-to-zero exact
points/keep flags for Scale and pure Cull (1/1 PASS, 0.44s). Vulkan native,
routing, fixtures, tiny-value behavior and full regression remain in progress.
No new push is authorized or performed.

CUDA envelope coverage also now asserts full output coordinates for partial
Set application across radial, CutExtend KeepParam and Reparam on ragged bent
curves, with blend .5 × amount .5 and exact keep flags. The expanded native
Length test passes (1/1, 0.44s); no CUDA production fix was necessary.
The installed consumer probe now includes scalar envelope lowering composed
with random/minimum/Reparam, but has not yet been rebuilt against the pending
Vulkan routing changes. Native and compiler/Session implementation continues.

Initial native-envelope integration builds successfully in the tests-OFF
runtime target, including generation ownership consumers and all twelve
compiled/validated shaders. This proves the new native API/56-byte module
integrates without changing old interfaces; no envelope GPU semantic or
installed-consumer pass is claimed yet. The routing worker remains active.

### Envelope routing and numeric correction checkpoint

Envelope routing now implements internal validation-copy for nonneutral pure
Cull, validates direct job/executor input lineage, and retains old neutral
routes. Compiler and actual missing-Envelope executor tests PASS (2/2, 1.51s).
The tests-OFF runtime, twelve-shader installation and installed consumer pass
the new factory/link/discovery and envelope+random+minimum+Reparam lowering.

Root's native envelope fixture exposed a real device discrepancy: the
positive-subnormal expected tip 0x80400000 became zero. Both enumerated Vulkan
devices report shaderDenormPreserveFloat32=false, so requiring the optional
DenormPreserve execution mode would not solve this on the active hardware.
The [Vulkan float-controls property contract](https://registry.khronos.org/vulkan/specs/latest/man/html/VkPhysicalDeviceFloatControlsPropertiesKHR.html)
defines that capability gate.

Root implemented shared GPU integer binary32 RN-even add/multiply operations
for the final staged envelope interpolation in float32Envelope.glsl. It
preserves subnormal/signed-zero behavior without shaderInt64, device float
controls, CPU operator fallback or geometry readback. The native envelope
fixture now PASS (1/1, 1.44s): exact tiny values, six partial method cases,
pure-Cull validation copy, zero-target bypass, full inherited COW/provenance,
invalid ranges/buffers/hairT, NaN recovery and resource rollback.

Broader independent GPU arithmetic checks against host binary32 operations
using the same helper are being added; Session fixtures and full regression
also remain pending. This initial passing matrix does not yet establish the
new arithmetic helper's full numeric contract. No further push was performed.

### Envelope independent arithmetic and Session evidence

Root rejected and replaced a delegated placeholder arithmetic test and an
incorrect test shader; neither was counted as evidence. The real test uses raw
charged buffers, the production native dispatch/proof path, and the exact shared
float32Envelope.glsl helper. It checks addition, multiplication and staged blend
against volatile host float32 operations under round-to-nearest/even, with
contraction disabled. A targeted edge matrix plus 20,000 deterministic random
triples gives **21,805 cases / 65,415 comparisons**: every non-NaN result is
bit-exact, including signed zero, infinities and subnormals; NaNs match by class.
The GPU arithmetic test passes.

Rooted and nonrooted Envelope Session fixtures also pass. They cover partial
Scale/Set and radial/KeepParam/Reparam, zero envelope retaining all curves
despite positive thresholds, pure-Cull validation-copy routing, exact unchanged
packets, and zero-current positive-floor retry/last-good preservation. Together
with arithmetic, **3/3 tests PASS (6.49s), no VUID/SYNC errors**.
The complete 90-test Vulkan regression is now building/running; broad
execution-graph completion remains unproven and the goal stays active.

### Vulkan scalar envelope verified checkpoint

Full current Vulkan build and regression: **88 passed, 2 unsupported SYNC_FD
skips, 0 failures out of 90 registered tests (73.71s)**. The final validation
log has no VUID, SYNC-HAZARD or validation errors. This includes the shared
integer-arithmetic matrix, partial/zero envelope native and rooted/plain
Session checks, capability admission, old shader paths, resource accounting,
lifecycle and cancellation tests. The twelve-shader tests-OFF runtime was
rebuilt/reinstalled after helper extraction, and its installed consumer passes.

Authored Length node.blend and literal mask:amount are now implemented and
verified in the local Vulkan path alongside the already-correct CUDA behavior.
No full CUDA/CPU rerun is claimed for this Vulkan-production/CUDA-test slice;
the expanded targeted CUDA Length evidence above remains current. All root
process handles are terminal. No additional commit or push was made.

The broader goal remains active: disabled controls, profiles/fields/maps,
additional Vulkan operators, native resource refinements, actual GPU branch
concurrency, renderer residency and other platform/driver evidence are still
open. Metal (excluded item 8) remains out of scope. This checkpoint does not
redefine completion around the controls already implemented.

### CUDA publication and aggregate RBF admission follow-up

The requested CUDA-only publication is complete: `origin/main` was verified at
`0e51d173ae659db12a2c9c65942e86e943e38a25`. The core CUDA/shared implementation
was already published; this commit adds the remaining CUDA Length numeric and
envelope regressions. The targeted CUDA Length test passed again (1/1, 0.46s).
Vulkan implementation, build integration and these ongoing plan notes remain
local. This does not authorize another publication of subsequent work.

The next implementation addresses selected-device aggregate RBF admission.
Both compiler metadata and runtime refinement currently require exactly one
Deform operator, although fixed-cardinality linear Deform/Width chains already
execute. Those chains therefore fall back to per-allocation admission and can
begin source/RBF work before a later Width allocation fails. The implementation
will share descriptor eligibility between metadata and runtime refinement and
account for each retained Width output, private staging, LUTs and proof scalars.

Root's independent allocation audit also found that the existing single-RBF
formula omits authored named-channel uploads: `EstimateGeometryBytes` does not
include them, while `UploadAuthoredPlanes` consumes the parent reservation.
The shared recipe must include point, primitive and groom planes and agree with
the source root-binding shape. New tests must exercise cold and warm admission
with exactly the reported available bytes, reject at one byte below that bound
before native work, and preserve retained COW channels/cache state on rollback.
These are implementation requirements, not yet verified completion claims.

This first aggregate recipe covers one Deform with literal Width prefix/tail
stages; generated sources, topology-changing mixed graphs, multiple Deforms
and value-DAG aggregate RBF admission remain required follow-on work. Static
estimates must remain explicitly partial when selected-device refinement is
needed, and unsupported shapes must not advertise refinement availability.

### Aggregate RBF implementation and first native evidence

Implemented the shared `FindLiteralRbfShape` compiler/runtime recipe for one
Deform plus literal Width prefix/tail stages. It resolves complete source root
bindings, no resampling, a prepared surface and literal bounded sample count;
expression-driven Width, maps, other operators and DAG shapes remain excluded.
Root validation caught and corrected an initial eligibility bug: parameter
compilation creates a non-null program even with no expression bindings, so
the recipe must inspect `Bindings().empty()`, not reject every program pointer.

The selected-device LU bound now includes authored point/primitive/groom
planes once, and each Width adds `8P + 2*kUsdGenRampLutSize*4 + 8` bytes for
retained output, private staging, LUTs and status scalars. Root-binding sizing
uses the common source shape predicate. Static metadata remains partial while
runtime refinement becomes available for the covered chains.

The new isolated `testUsdGenCudaRbfAggregateAdmission` exercises four accepted
shapes: single RBF, Width prefix, two Width tails, and prefix plus two tails.
It proves the exact named-plane byte delta, per-Width byte delta, cold/warm
direct and native asynchronous admission at the reported bound, rejection one
byte below it without RBF accept/rollback attempts, finalization rollback and
exact-budget retry. Independent reference generations retain ragged points,
rest, widths, IDs, offsets, hairT, roots and named planes across these changes.
The initial native matrix passed **10 consecutive runs (9.07s total)**; compiler
metadata also passed. Broader regression and strengthened public metadata/tile
comparisons are pending at this checkpoint.

The pressure fixture exposed two retirement boundaries that must not be
confused with admission failures. A successful/failed completion can precede
return of relay worker call stacks retaining private candidates. Published
generation/consumer owners also use the separate deferred retirement service.
The fixture explicitly retains native jobs, joins terminal relay worker graphs
through a narrowly documented test-only barrier, and drains the existing
generation retirement service before taking exact-byte baselines. The barrier
is valid only after all accepted stages have delivered terminal callbacks and
no concurrent launcher remains; it is not a general GPU drain. Every fixture
requires exact per-resource-kind recovery at teardown. Clean rollback may
discard existing scratch (observed 384 bytes); the test rejects charge growth
and refills pressure to the exact bound before retrying.

`Queue::Drain` retirement semantics were not changed by this slice and remain a
separate lifecycle follow-up if a stronger production quiescence contract is
required. A proposed source-without-surface case was not counted as native
coverage: the existing compiler rejects that authoring. No production fallback,
geometry readback or additional publication was introduced.

### 2026-09-14 checkpoint: ExecutionStages retirement repair + RBF DAG admission

`testUsdGenCudaExecutionStages` `runAutoRootVariant` failed intermittently at
the partial-root variant: correct 6102-byte literal-Length rejection, but
`usedBytes` fell 12-20 bytes between the saturated snapshot and the rejection
assertion. Root cause: `SourceOwner`/consumer destructors retire into the
deferred generation/consumer retirement service, which frees byte permits on
its worker; the test joined only the relay arenas. Fix (test-only):
`FindUsdGenExecutionRetirementService({Cuda, device})->Drain()` after each
relay join plus `ProducerReady()` terminal proof before pressure snapshots.
No budget/identity assertion was weakened. Evidence: RED reproduced with
`--repeat until-fail:10` (`FAIL 2000`, `peak=6102 used=...105/...125`); GREEN
10 consecutive passes; full suite **179/179, 0 failures, serial, no
exclusions**.

`FindLiteralRbfShape`/`ReserveLiteralRbfExecution` now cover fixed-cardinality
source-rooted value DAGs of literal Deform/Width/WidthBlend nodes. Source,
named channels, source parameters and publication are charged once; each
Width/WidthBlend output owns the established per-Width bound; each proof step
owns 16 bytes of comparator device/pinned status; each distinct Deform owns
its surface/sample/matrix/solver/order terms plus parameter candidates
separately (identical descriptors never deduped; selected-device LU queries
reused by sample count). Left-lineage double-Deform is re-verified
structurally; the layout remains the semantic gate. New
`testUsdGenCudaRbfDagAdmission` proves exact-budget admit, one-byte-below
reject, atomic two-branch rollback (+2/+2 accept/rollback deltas), equal and
unequal sibling joins (unequal fails closed with `differ outside Width`, zero
accepts), retained COW channels against an independent reference, and
cold/warm direct/async parity. Boundary locks: double-Deform lineage,
expression-driven Width exclusion, non-`rbfSamples` Deform preservation, and
reference-rooted Deform DAG layout rejection (reference sources lower only to
Width DAGs or a single direct Length trunk; that extension belongs to the
composition item). Full suite **180/180, 0 failures**. Remaining in this item:
Scatter/Grow sources, topology-changing Length/Grow descendants, and
remaining parameter/map shapes.

### 2026-09-15 validation: ExecutionStages retirement repair (TODO item 1 done)

Revalidated the 2026-09-14 repair against current sources with no new code
changes: `testUsdGenCudaExecutionStages` 10/10 `--repeat until-fail:10`,
`testUsdGenCudaRbfAggregateAdmission` 10/10, full suite **189/189, 0
failures, serial (`-j 1`), no exclusions** (100.92s; suite grew from 179 to
189 registered tests since the handoff; the single non-run is the expected
`testUsdGenStormSurgery` skip). Test binary (build-codex,
2026-09-15 07:07) is newer than every touched source, so `ninja: no work to
do` is a current build, not a stale one. No budget/identity assertion was
touched. TODO item 1 is closed; the broader composition/concurrency work
(items 2-8) remains as scoped in `plans/TODO.md`.

### 2026-09-15 validation: aggregate RBF DAG admission (TODO item 2 done)

Audited every item-2 bullet against code + batteries with no new code
changes: shape detection (`FindLiteralRbfDagShape`/`FindLiteralRbfScatterDagShape`),
reserve dispatch (`ReserveLiteralRbfExecution` -> DAG `:2244` / scatter `:2513`),
structural double-Deform lineage gate, per-Width/proof/Deform charging with
per-kind ledger assertions. Proof, all `--repeat until-fail:5` green:
`RbfDagAdmission`, `RbfScatterDagAdmission`, `RbfTopologyDagAdmission`,
`RbfParamMapAdmission`, plus `RbfAggregateAdmission`, `RbfValueDag`,
`ScatterGrowRbfDag`, `CompositionMatrix`, `ControlEval`. Reference-rooted Deform
DAG battery covers exact/below-budget, cold/warm, ledger recovery. Full suite
**189/189, 0 failures, serial**. TODO item 2 is closed; residual open-ended
parameter/map shapes are bounded by the item-3 composition matrix.

### 2026-09-14 checkpoint: reference-rooted RBF DAG lowering (composition)

Reference sources lower as a third shape: a literal Deform/Width/WidthBlend
value DAG with the same literal rules as authored-source RBF DAGs, including
linear-ordered reference chains (never legacy). The Deform target must name
one resolved surface and the reference must carry complete transported root
bindings. Admission resolves the transported curve set and charges it like an
authored source (named planes are validation-rejected for references, so the
authored-plane term is empty). One executor divergence was found and fixed:
the async source commit marked reference values deformed (generic
`!useRest`), while the direct path marks them lineage roots; async Deform
then rejected a legal graph. `ExecutionState` now carries the transport flag
and the commit reproduces the direct marker. `testUsdGenCudaRbfDagAdmission`
covers the reference battery (exact/below-budget, two-branch rollback,
async parity); the composition probe locks the shape and refinability.

### 2026-09-14 checkpoint: Scatter/Grow-rooted RBF DAG admission (Item 2 tail)

Scatter→Grow plans lower Deform/Width/WidthBlend descendants through the
capture route with their own metadata site: `runtimeRefinementAvailable`
was unconditionally false for Deform plans and is now set from the literal
scatter-DAG shape test. Admission charges the compiled Grow requirements
peak (capture upload, generated output, status, map scratch) once, with
per-Deform/Width/proof terms at the generated cardinality
(`requirements.pointCount`, captured curve count); generated curves own no
authored planes. Length/Noise/Grow descendants keep their existing routes.
`testUsdGenCudaRbfTransaction --aggregate-scatter-dag-admission` proves
exact-budget admit, one-byte-below reject, atomic two-branch rollback,
unequal sibling fail-closed join, cold/warm direct/async parity and ledger
recovery. Two executor observations are recorded as follow-ups, not
encoded as rules: a single-quad V=4 surface fails fresh rest binding
while tri V=5 binds (floor unisolated), and capture rootUVs interpolate
the surface `uv` attribute while the binder validates barycentric-in-face
(fixtures must satisfy both).

### 2026-09-14 checkpoint: Length/Grow-descendant RBF DAG admission (Item 2 tail)

`FindLiteralRbfShape` accepts Length and C3-Grow steps in authored-source
RBF DAGs (Noise and resampling stay excluded; Grow requires literal
segments in [2,64] for cardinality; linear-ordered Grow chains keep their
pre-existing Noise/Length/Width-suffix rule). Admission charges the
full-input worst case: grown cardinality uplifts Width/Deform/named terms,
Length compactor/scratch/survivor terms mirror the Length recipe with an
exact selected-device CUB query (the stream is now threaded through the RBF
reserve), Grow frames/maps/status mirror it, and every step's parameter
candidates are charged (literal programs contribute nothing, as the Length
route proves). `testUsdGenCudaRbfTransaction
--aggregate-topology-dag-admission` proves Length-trunk and C3-Grow Deform
DAGs (exact/below-budget, two-branch rollback, async parity); the
composition probe locks the shapes and the linear-Grow-Deform rejection.
Scatter+Length+Deform stays on the fallback route (execution unproven;
zero regression risk by construction).

### 2026-09-14 checkpoint: parameter/map shapes in RBF DAG admission (Item 2 tail)

Width steps in Deform-bearing RBF DAGs (authored and capture routes) now
admit expression bindings and image maps: bindings ride the per-step
candidate charging already in place, and each mapped Width adds its image
upload plus root-domain sample plane. Width-only DAGs stay on the
task-estimate route (no solver/cache shape to refine) and linear chains
stay literal-only. `testUsdGenCudaRbfTransaction
--aggregate-param-map-admission` proves an expression-driven plus
image-mapped Width DAG (exact/below-budget, two-branch rollback, async
parity); the composition probe locks DAG refinability. With this slice,
every compiled RBF shape carries an aggregate charging proof: linear,
value-DAG, reference-rooted, capture-route, Length/Grow-descendant, and
parameter/map variants.

### 2026-09-14 checkpoint: consuming-input control evaluation proofs (Item 3)

`testUsdGenCudaControlEval` proves runtime evaluation of consuming-input
controls with device values: primitive-domain float (`$value * $u` gives
per-curve widths .3/.3/.3/.2/.2 in stable id order), point-domain float
(`$value * $t` gives 0/.5/1/0/1), primitive-to-point inheritance (rootUV
consumed per point), exact muted-operator no-op (a disabled Width aliases
its input: identical counts and widths to the operator-free reference),
and primitive-domain cull (`$value * $u` thresholds keep only curve 7).
No implementation gap was found on these paths; kernels broadcast
groom→all, primitive→curve, point→exact with bounds checks, and bool
fields accept groom/primitive only, matching validation.

### 2026-09-14 checkpoint: Vulkan muted Width/Length passthrough (Item 4)

Muted topology-preserving operators now alias instead of rejecting on
Vulkan, matching CUDA and plan/04 R14: `VulkanSourceWidthStage.disabled`
is plan data, validation keeps full strictness, job admission skips
control/capability checks for muted stages (structural ordering kept),
and `BeginStage` forwards the input generation with no dispatch and no
new value version. Length-cull muted skips identically (keep-all).
WidthBlend and sources keep their enabled requirements, as on CUDA.
Proofs: plan locks for muted Width/Length stages plus a job-level
disabled-Width test (Ready without WidthPending, readback {1,2} against
a factor-9 replace, source version preserved, ledger clean).

Build-configuration incident and rule: reconfiguring build-vulkan with
`-DCMAKE_BUILD_TYPE=Release` broke four exact-value tests (1-2 ULP FMA
diffs between fused CPU oracles and unfused GPU evaluation). The tree was
built without an explicit type (unfused oracles); restoring the empty
type returns the suite to green. NEVER pass `-DCMAKE_BUILD_TYPE` for
build-vulkan; build-codex stays Release. `-ffp-contract=off` pins were
added to the Source/Session oracle test targets as defense in depth,
following the existing EnvelopeArithmetic/Width precedent.

### 2026-09-14 checkpoint: WidthBlend-join overlap witness (Item 5)

Independent WidthBlends already take branch streams through the shared
`widthBranch` gate/witness path, so the overlap extension needed no
scheduler change: `testUsdGenCudaRbfTransaction --blend-overlap` arms
gate and witness after four Widths complete, launches both blends on
threads, and asserts rendezvous, distinct task/lane identities, absolute
blend values (2.25/4.5), parity, and ledger recovery, with `maxActive >=
2` observed where the device reports concurrent kernels. A witness
disarm hook releases probe allocations for exact baselines.

### 2026-09-14 checkpoint: bridge lifecycle proofs (Item 6 slice 1)

`testUsdGenCudaGlInterop --lifecycle` (new `testUsdGenCudaGlLifecycle`)
proves version acceptance, last-good visibility and renderer retirement
through the CUDA-GL bridge end to end with traces: version 1 publishes,
version 2 supersedes into fresh ranges, destroying version 1 cannot
disturb the displayed version 2, a failed version 3 admission never
becomes displayable (last-good version 2 keeps displaying), and an
explicit GL fence retires each displayed version before destruction,
with owner release and clean diagnostics. The plugin handoff
(`devicePublication` gating, tile computation attachment) and the tool
consumer loop remain the next slices; gates stay closed until those are
demonstrated.

### 2026-09-14 checkpoint: tile-scoped bridge transfers (Item 6 slice 2)

`testUsdGenCudaGlInterop --tiles` (new `testUsdGenCudaGlTiles`) proves
the per-tile publication primitive: a session device generation with two
tiles transfers tile-1 points and widths whose bytes match the
whole-generation slice exactly (528/528 and 176/176 green pixels in a
GPU-side comparison shader, no host crossing), an out-of-range tile id
fails closed with an error, and a render fence proves retirement.

### 2026-09-14 checkpoint: multi-groom admission contention (Item 7 slice 1)

`testUsdGenCudaRbfTransaction --multi-groom-admission` (new
`testUsdGenCudaMultiGroomAdmission`) proves two independent grooms sharing
one device pool: identical descriptions admit identical peaks through
independent workspaces with distinct owners; an async job holding groom
A's exact peak makes groom B's creation fail one byte below budget with
no ledger movement and no attempt-counter advance; after release, groom B
admits exactly. Full ledger recovery at teardown.

### 2026-09-14 checkpoint: native allocation audit (Item 7 slice 1)

Device memory flows through one choke point (`DeviceBuffer::reset`,
always permit-tracked with conservative abandon/quarantine); every
pinned host allocation is permit-paired; Vulkan `ChargedBuffer::Create`
takes a kind; workspace streams and cuSOLVER handles are created with
their owners and destroyed with them. The single exception is the CPU
Noise GPU fast path (`ops/noise_gpu.cu`): process-lifetime static
stream, device scratch and pinned staging grown to high water without
ledger permits, failing over to the CPU loop. It is recorded as explicit
headroom (bounded per workload, sequential commit thread, graceful
fallback), not tracked peak. The single `cudaDeviceSynchronize`
(`gpu/pointOverride.cu` destructor) is a rare-path conservative cover
for unknown consumer streams with quarantine fallback. No waits were
found on CUDA callback threads; scalar-readback D2H waits and
failure-path/lifecycle drains are necessary.

### 2026-09-14 checkpoint: Item 8 packaging verification and requirement reconciliation

Builds after the shared-interface changes (`cudaExecution.h` stays
CUDA-free; reserves are file-local): CUDA-on 189/189 serial, CPU-only
85/85, Vulkan opt-in 91/91 (empty build type; never pass
`-DCMAKE_BUILD_TYPE` for build-vulkan or FP contraction breaks
exact-value oracles). Tests-OFF configure+build+install succeeds;
all three installs validate (layout, comment-tolerant plugInfo with
resolved libraries). `testUsdGenInstallTree`/`testUsdGenConsumer` pass
per variant with `USDGEN_INSTALL_PREFIX` set. Two packaging bugs fixed:
the exported config hard-required a Vulkan SDK for all consumers
(now quiet-probed; Vulkan targets fail at generate time only when
linked), and the install-tree test expected private archives/headers
that N-7 deliberately never installs (flipped to absence locks,
matching the B-1 no-third-party gate). Backend audit:
CPU/CUDA-available (CUDA only when enabled), Metal/Vulkan explicitly
unavailable with fail-closed validation and no implicit fallback;
production Vulkan selection stays closed (injected-provider success is
not renderer readiness). Other-driver/platform evidence: none (single
NVIDIA Linux workstation); portability unestablished by construction.
No commits were made (not requested); all Vulkan work remains local.

Requirement status by TODO item: Item 1 done (retirement repair,
179/179 then 189/189). Item 2 done in full (linear, value-DAG,
reference-rooted, capture-route, Length/Grow-descendant and
parameter/map admission with aggregate proofs). Item 3: matrix,
runtime-eval proofs and reference-Deform done; remaining valid
combinations (e.g. Scatter+Length+Deform execution), Noise paths and
missing operators stay open. Item 4: muted Width/Length done; Vulkan
operator breadth, field/expression/map inputs, oracle discrepancies,
provenance checks and old-shader rejection stay open. Item 5: audit
and blend-join witness done; lane expansion, Vulkan per-task state and
Graph extension stay open. Item 6: bridge lifecycle and tile transfers
done; plugin handoff wiring and the tool-loop commit driver stay open
(gates closed). Item 7: multi-groom contention and allocation audit
done; fairness/aging/eviction/device-loss/soak evidence stays open.
Item 8: per-variant builds/installs/consumers/backend audit done;
other drivers and release readiness stay open.
