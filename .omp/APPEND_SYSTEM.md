# usdGen plan execution routing and checkpoints

Follow the user's authorized scope and plan/README.md; use plan/11-roadmap.md
for milestone order and gates. Inspect current evidence before assigning work.

Local Qwen is reserved exclusively for the main coordinator thread. Keep the
main/default/orchestrator/plan/slow model on qwen38-coordinator. Never launch
workers, reviewers, advisors, nested agents, or utility calls on qwen38-next,
qwen38-coordinator, or the legacy local qwen38 provider.

Maintain a shared roster of at most thirteen active workers across the entire
delegation tree. All worker types share this total budget:
- `task`, `task_remote`, `large-context-task-executor`, and `scout`:
  Meta Muse Spark Contributor
  (`meta/muse-spark-1.3-contributor`), at most ten active Meta workers.
- `lean-context-task-runner`: Hivemind (`hivemind/qwen3.8-27b`), at most two
  active instances for focused work within its 80128-token context.
- `sonic`: Apple (`apple-fast/system`), at most one active instance. Its entire
  context is only 4K; use short self-contained packets and minimal tools.
- `nemotron`: OpenRouter (`openrouter/nvidia/nemotron-3.5-lightning:free`),
  at most one active instance; use only the explicitly free endpoint.
- `reviewer`, `security-reviewer`, and the passive advisor use Sol at medium
  effort (`openai-codex/gpt-5.6-sol:medium`). Reviewers share the same thirteen-worker
  total. They must not use local Qwen or Astra.

Recursion depth is three. Nested delegation shares the thirteen-worker budget;
it does not create another pool. Reserve capacity through the coordinator
before nested dispatch. Release slots when workers finish. Provider request
caps are coordinator 1, Meta 10, Hivemind 2, Apple 1, and OpenRouter 1. These
limit simultaneous calls, not live worker sessions. The legacy local-worker
alias remains for compatibility only and must never be selected for delegation.
Do not fall back to local Qwen when a remote endpoint fails. Return a concrete
blocker or choose another allowed remote provider with a fitting context window;
never substitute a paid OpenRouter endpoint for the free Nemotron endpoint.

Existing workers must checkpoint and finish before replacement on the new
routing; preserve their files and scoped handoffs. Do not resume a persisted
local-Qwen worker under its old model. Verify actual provider routing in session
metadata when uncertain. Keep Hivemind/Apple packets compact; use Meta for
large-context work.

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
