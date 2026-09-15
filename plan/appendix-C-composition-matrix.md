# Appendix C — Capability/composition matrix (from code, 2026-09-14)

Enforced by `tests/testUsdGenCudaCompositionMatrix.cpp`
(`testUsdGenCudaCompositionMatrix`). Every verdict below names the owning
`file:line`; the probe locks the CUDA layout verdicts. Vulkan cells are read
from the implemented pipelines and their tests; the probe does not execute
Vulkan plans.

## 1. Source roles

| Source | CUDA shapes | Notes |
|---|---|---|
| `UsdGenCurveSource` (hair lane) | linear legacy, unary DAG, value DAG, topology DAGs | Exactly one source per graph; exactly one curve target; no upstream/reference/map inputs (`cudaExecution.cpp:3190`, `:3182`). `lane != hair` rejected: reference-lane not integrated (`:3217`). Non-identity description/curve transforms rejected when a Deform is present (`:3213`). |
| `UsdGenCurveSource` + `resampleTo` | compiles | Excluded from literal RBF admission only, not from compilation. |
| `UsdGenCurveSource` + `useRest=false` + Deform | rejected | `already-deformed CurveSource cannot feed rest-to-animated RBF Deform` (`:3208`). |
| `UsdGenReferenceSource` | Width DAG; single direct Length trunk + Width branches; literal Deform/Width/WidthBlend value DAGs | Exactly one `Reference`-role curve set, no named planes, no rootFrame/guideBlend, no params/ramps/expressions/blend (`:3139`, `:582`). Reference Deform targets name one real surface and require complete transported root bindings. Even linear-ordered reference chains lower as DAGs, never legacy. Async commit reproduces the direct `deformed=false` lineage marker for reference transports. |
| `UsdGenScatter` + `UsdGenGrow` | capture route, Width/Length/Noise/Deform descendants | Exactly one Scatter with one direct Grow input (`:2446`). Deform is an allowed descendant (`:2469`) but stays outside literal RBF admission (`FindLiteralRbfDagShape` rejects `scatterGrow`). |
| Bare `UsdGenScatter` | rejected | No topology producer without its Grow consumer. |

Source expressions: only groom-native `resampleTo` (int) / `useRest` (bool)
(`:3167`). Non-identity source param masks rejected (`:3223`).

## 2. Capability matrices

CUDA `GetCudaExecutionCapabilityMatrix`, version 35
(`cudaExecution.cpp:1684`): Scatter 1, Grow 13, CurveSource 2, Width 6,
Length 7, Noise 7, Deform 1, ReferenceSource 2, WidthBlend 6. Unknown operator
names fail at validation, never silently (`:3027`, `:3084`).

Vulkan plan capability version 1 (`vulkan/planExecutor.h:48`,
`vulkan/sessionProvider.cpp:33`). Implemented native pipelines:
Source/SourceWidth (`sourceWidthJob`, `sourceGeneration`), Width
(`widthPipeline`), WidthBlend (`widthBlendPipeline` + non-width proof
`nonWidthComparePipeline`), Length scale/compaction (`lengthScalePipeline`,
`lengthCompactionPipeline`), publication (`publicationJob`), sessions and
completion service. Muted Width/Length stages alias their input with no
dispatch (same rule as CUDA). No Scatter/Grow/Noise/Deform pipelines
exist; their tests are absent too. That gap belongs to Item 4, not to
this matrix.

## 3. Composition shapes (CUDA)

- **Legacy linear** (`LinearAuthoredChain`): source index 0, authored order,
  terminal last, Deform present. The only shape with `taskDag == false`.
  Literal RBF admission covers one Deform + literal Width prefix/tail.
- **Unary DAG** (`SourceRootedUnaryDag`, `taskDag`, no blend): Width-only
  (`widthDag`), single-Length-trunk, multi-Length value, C3 Grow value, and
  no-blend Deform/Noise mixes. A Width→Length chain lowers here, not as a
  value DAG.
- **Value DAG** (`SourceRootedValueDag`, `taskDag`, blend present): any graph
  containing `UsdGenWidthBlend`, including sibling-Deform joins.
- **Topology DAGs** (`topologyDag` + `geometryValueDag` / `sameTopologyDag`):
  single Length trunk, multi-Length values, Grow values.
- **ScatterGrow** (`scatterGrow`, `taskDag`, `widthDag` iff no geometry
  descendant): capture-source route; `geometryValueDag` when Length/Noise/
  Deform descendants exist.

Terminal: explicit terminal names any node; otherwise exactly one sink is
inferred, else `branched CUDA Width graphs require an explicit terminal`
(`:3006`). Selected sibling terminals are legal (a blend terminal with
non-terminal Deform siblings).

## 4. Pair/triple rules

- Width below anything (unary, one input). Length below Source directly =
  topology trunk (`:2717` enforces directness for the trunk candidate).
- Length below Width = valueDAG input (no trunk claim).
- Deform below Source/Width/Length/Grow-Noise values: legal once per
  lineage; a second Deform below a deformed predecessor fails:
  `a second rest-to-animated deformation would apply surface motion twice`
  (`:2740`, scatter route `:2529`, legacy `:2645`). Sibling Deforms on
  independent lineages are legal and each owns a native cache
  (`testUsdGenCudaRbfTransaction --value-dag` variant 5: two bindings).
- Deform below Scatter→Grow: layout-legal (`:2469`); literal RBF admission
  charges the compiled Grow requirements peak plus per-Deform/Width/proof
  terms at the generated cardinality. Empty-source RBF execution was already
  proven in the ScatterGrow tests; the non-empty battery lives in
  `testUsdGenCudaRbfTransaction --aggregate-scatter-dag-admission`.
- Deform below ReferenceSource: rejected at layout (see §1).
- Deform requires exactly one surface, `rbf` mode, deformed/final sampling,
  and the surface must equal the source root-binding surface
  (`ValidateDeform`, `:425`; match check `:3090`). `rbfSamples` literal
  range 4–46336.
- Noise: rest/auto space, final sampling; authored rest required unless a
  Grow ancestor generated it (`NoiseNeedsAuthoredSourceRest`, `:1751`).
- Grow (C3 value): exactly one direct CurveSource input in the linear form
  (`:3123`); Noise/Length/Width suffixes only — a linear Deform suffix
  stays rejected. Branched Grow values feed Deform branches at the grown
  cardinality (curves × literal segments); admission charges the full-input
  worst case with Length-style compactor/scratch/survivor terms.
- WidthBlend: exactly two distinct ordered inputs (`:2673`, `:2687`); blend
  finite in [0,1] (`ValidateWidthBlend`, `:209`).

## 5. Fan-out/fan-in

Fan-out is free (independent siblings, concurrent Width branches). Fan-in
exists only as WidthBlend (binary, ordered; duplicate inputs rejected as
authoring errors, `:2687`). Cross-topology/point-origin joins set
`requiresNonWidthProof`; the runtime comparator proves bitwise non-width
equality and unequal joins fail closed (`differ outside Width`,
`cudaExecution.cpp:10028`). Topology/curve-type/basis/wrap and frame-domain
mismatches fail before comparison (`:9992`).

## 6. Reference / map / expression edges

- ReferenceInputs: only the single ReferenceSource reference; everything
  else rejected (`ValidateCudaResolvedInputs`, `:570`).
- Maps: only Width `maskImage` (typed image path, `:310`); Length, Noise,
  Deform, WidthBlend reject maps. Anonymous ramps rejected everywhere;
  named knot parameters only.
- Expressions per operator (name → type/domain):
  - Source: `resampleTo` int / `useRest` bool, groom only.
  - Length: floats incl. `blend`, `length:random` float2,
    `enabled` bool groom-only; `cullThreshold`/random not point (`:132`).
- Width: floats incl. `blend`, `replace`/`enabled` bool
  (`enabled` groom-only, `:188`). Expression-driven and image-mapped
  Widths refine inside Deform-bearing RBF DAGs (candidate charging plus
  upload/sample charging); Width-only DAGs stay on the task-estimate
  route and linear chains stay literal-only.
  - Noise: floats, `noise:octaves`/`noise:seed` int, `enabled`/`cumulative`
    bool; `enabled` groom-only, `cumulative`/`noise:seed` not point (`:286`).
  - Deform: `blend`/`mask:amount` float, `rbfSamples` int (groom-only),
    `enabled`/`lockRoots` bool (`enabled` groom-only, `lockRoots` not
    point, `:469`). `rbfSamples` expressions change LU dimensions and stay
    outside literal admission.
  - WidthBlend: no expressions at all (`:213`); blend is a literal node
    field in [0,1].
- `enabled` is a runtime-evaluated control on Length/Width/Noise/Deform,
  not a layout skip (consumed via `boolean("enabled", …)` at lowering and
  execution).

## 7. Topology changes

Only Scatter/Grow/Length/CurveSource change topology, each under a topology
barrier flag; Width/Noise/WidthBlend never do; Deform writes a point-only
COW revision reusing the predecessor topology snapshot (`:2626`). Length
survivor counts are device results: admission charges the worst case (every
input survives) with a selected-device CUB scan query
(`ReserveLiteralLengthExecution`, `:2035`).

## 8. Verdict catalog

Invalid authoring (reject before any device work): duplicate paths, unknown
operators, arity violations (unary/WidthBlend), duplicate blend inputs,
blend range, cycles, non-rooted inputs, source fan-in, multi-sink without
terminal, bad terminal, surface mismatch, `rbfSamples` range, CurveSource
cardinality/transform/lane/useRest violations, ReferenceSource shape
violations, map-on-non-Width, anonymous ramps, mistyped expressions.

Missing math (compiles or fails only for lack of an evaluator, not shape):
none currently isolated — every compiled shape above executes in at least
one test, and every admitted shape carries its charging proof.

Open executor questions (observed, unresolved): the fresh rest binder
rejects a single-quad V=4 surface while tri V=5 binds at budgets 4–6 —
whether the floor is vertex count, face count, or topology is unisolated;
capture rootUVs interpolate the surface `uv` attribute while the binder
validates barycentric-in-face, so fixture UVs must satisfy both (tri sums)
until the UV-space contract is settled.

Executor-only limitations (shape legal, evaluator deferred by design):
`resampleTo` + literal RBF admission; expression-driven
Width/WidthBlend admission; empty-program vs expression distinction
preserved (empty programs are literal, not expressions).

## 9. Coverage map

- `testUsdGenCudaCompositionMatrix` (this appendix): 29 layout cells
  incl. the reference-Deform third shape (UnaryDag, refinable), the
  Scatter→Grow→Deform capture route (UnaryDag, refinable), Length-trunk
  and branched-Grow Deform DAGs (refinable), the linear-Grow-Deform
  rejection lock, and expression-Width DAG refinability.
- `testUsdGenCudaRbfTransaction --aggregate-admission`: linear RBF
  admission proofs. `--aggregate-dag-admission`: DAG admission proofs,
  reference-rooted Deform battery (exact/below-budget, rollback,
  async parity), lineage/expression boundary locks.
  `--aggregate-scatter-dag-admission`: capture-route admission proofs
  (exact/below-budget, two-branch rollback, async parity, unequal join).
  `--aggregate-topology-dag-admission`: Length-trunk and C3-Grow Deform
  DAG batteries (exact/below-budget, rollback, async parity).
  `--aggregate-param-map-admission`: expression-driven and image-mapped
  Width battery (exact/below-budget, rollback, async parity).
- `testUsdGenCudaControlEval`: consuming-input value proofs — primitive
  and point float controls with typed broadcasting, primitive-to-point
  inheritance, exact muted-operator no-op, and primitive-domain cull with
  survivor identity. `--value-dag`: sibling
  execution + rollback.
- `testUsdGenCudaWidthDag`: fan-out/fan-in, Length trunk, topology blend,
  RBF-below-Length/Width execution, double-Deform rejection.
- `testUsdGenCudaExecutionPlan`: metadata flags incl. refinability matrix.
- `testUsdGenCudaExecutionStages`: Length auto-root pressure behavior.
- Vulkan operator tests under `tests/testUsdGenVulkan*.cpp` (see §2).

## 10. Handoff

This matrix completes Item 3 bullet 1; the reference-Deform third shape
has since been implemented (layout + validation + admission + async/direct
parity proof). Remaining Item 3 work: implement the
remaining semantically valid combinations named above, complete
consuming-input runtime evaluation for numeric/bool/inherited controls
across domains, and finish missing operators with resource/COW contracts
derived from this matrix — never by adding names to the capability table.
