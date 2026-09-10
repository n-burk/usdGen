# usdGen plan execution routing and checkpoints

Follow the user's authorized scope and plan/README.md; use plan/11-roadmap.md
for milestone order and gates. Inspect current evidence before assigning work.

For the coordinator: maintain a shared roster of at most thirteen active workers
across the entire delegation tree. All worker types share this total budget:
- `task`: local Qwen (`qwen38-next/qwen3.8-flash-next:low`).
- `reviewer`: Astra (`openai-codex/gpt-6-astra:high`).
- `task_remote`, `large-context-task-executor`, `scout`, and `security-reviewer`:
  Meta Muse Spark Contributor (`meta/muse-spark-1.3-contributor`), at most ten
  active Meta workers.
- `lean-context-task-runner`: Hivemind (`hivemind/qwen3.8-27b`), at most two
  active instances for fast, bounded, lower-context work.
- `sonic`: Apple (`apple-fast/system`), at most one active instance for short
  tasks that fit its 4K context.
Local Qwen and Astra workers count toward the same thirteen-worker total,
reducing the slots available to other workers while active. Verify actual provider
routing in session metadata when uncertain. Keep Hivemind assignments focused
with targeted reads and compact handoffs; route large-context work to Meta.

Recursion depth is three. Nested delegation shares this same thirteen-worker
budget; it does not create another pool. Before nested dispatch, reserve capacity
through the coordinator and update the shared roster; if capacity cannot be
confirmed, work inline or return the subtask to the coordinator. Release slots
when workers finish. Provider request caps are four for local Qwen workers,
ten for Meta, two for Hivemind, and one for Apple; these caps limit simultaneous
model calls, not live worker sessions. Enforce the shared worker budget and
per-provider worker limits through roster management. Do not automatically
substitute providers when the assigned endpoint is unavailable.

Before dispatch, verify that every referenced path exists and copy exact
section headings from the source. Canonical files include:
- plan/02-schema.md
- plan/03-execution-engine.md
- plan/04-operators.md
- plan/05-static-curves-and-deformation.md
- plan/06-imaging.md
- plan/07-look-maps-expressions.md
- plan/08-tools.md
- plan/09-performance-and-benchmarks.md
- plan/10-build-dependencies-testing.md
Do not invent plan/05-build-and-tests.md or plan/07-shaders-and-pipelines.md.
For an invalid reference, consult plan/README.md once and record the correction.

Give each worker one acceptance criterion or small coupled fix, exact owned
files, relevant sections, dependencies and commands. Aim for 5K-12K initial
context. Do not assign an entire milestone or whole registry in one packet.
Keep reasoning focused on the next verifiable action. Batch targeted reads;
write edits in small complete chunks within the 4K output budget.

Checkpoint by 20 requests, wrap up by the 24-request notice; the runtime stop
at 36 requests is a partial handoff, never completion. A checkpoint records
changed files, exact passing/failing commands, first error, pending criterion,
and the next action. Start a new bounded packet after a handoff; do not keep
reviving the same broad assignment. After two failures of the same check,
isolate the cause or return a concrete dependency blocker.

Reserve one integration owner for CMakeLists.txt, shared headers and generated
interfaces. Workers use disjoint source ownership and report interface deltas.
Serialize builds in the shared build directory; build only the affected target.
Use CMake-generated compile/link commands, not reconstructed library names.
Avoid full rebuilds, full test suites and GPU benchmarks while workers edit.
Run required milestone-wide validation after integration is stable, with GPU
inference quiescent for performance gates. A missing stub marker is not proof
of implementation; require an inspected diff and executed acceptance checks.

Keep the active roster and verified gates in a compact checkpoint file under
.omp/ (one coordinator writer). Reconcile persisted worker sessions with live
jobs after restart; old session files do not prove workers are still running.
Wait at most 60 seconds per coordinator wait and inspect meaningful progress
between waits. Escalate a repeatedly failing or unproductive lane by narrowing
its packet, not by increasing context or launching overlapping workers.
