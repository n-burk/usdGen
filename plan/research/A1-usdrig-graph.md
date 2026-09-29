# A1 — usdRig (RigExec) execution model: OpenExec computations, VDF mover graph, evaluator, invalidation

Research report for the usdGen (hair/fur) architecture plan. Every claim below cites the file and
line it was read from (absolute paths). Items that could not be verified are marked UNVERIFIED.

Repo under study: `<usdrig-src>` (library prefix `RigExec`, C++ namespace `rigExec`).
OpenUSD source used for cross-checks: `<openusd-src>` (v26.08).

---

## 0. Executive picture

RigExec is **two engines glued together by one evaluator**, not one OpenExec graph:

```
composed UsdStage (never written by the engine)
   |
   |  Compile(): discover prims by TYPE under <RigExecRoot>, walk <root>/Movers in
   |  reverse-sibling post-order, resolve every relationship into path bindings,
   |  validate cycles/read phases, build ExecUsd requests ("tap sets"), hash structure
   v
RigExecRigEvaluator (libs/rigExec/rigEvaluator.{h,cpp})
   |-- (A) OpenExec/ExecUsd: controls, joints, solvers, weight packets, blend channels,
   |       mover parameter packets  -> RigExecTapSet over ExecUsdSystem (parallel VDF engine)
   |-- (B) In-memory VdfNetwork per points target per generation: RigExecMoverGraph
   |       (_RevisionNode chain evaluated with VdfSimpleExecutor, serial)
   |-- (C) CPU scalar reference (_EvaluateChain) used as a parity oracle
   v
immutable RigExecRigPose (one generation)  ->  rigExecImaging snapshot store (atomic swap)
   ->  Hydra 2.0 filtering scene indices  ->  Storm
```

- Evidence for the split: `<usdrig-src>/libs/rigExec/rigEvaluator.h:1-11` ("Transforms and solvers evaluate through OpenExec; the geometry mover chains evaluate through the in-memory RigExecMoverGraph. NOTHING is authored").
- Evidence that the engine authors nothing and there is no derived stage anymore: `<usdrig-src>/docs/mover-graph-cutover.md:1-6`; `rigEvaluator.h:214-216` (`GetEvaluationStage()` "This IS the source stage").
- Evidence that OpenExec is used **unpatched**: `<usdrig-src>/README.md:25-26`; `<usdrig-src>/CMakeLists.txt:4-9` (`find_package(pxr REQUIRED CONFIG ...)`).

The most important design facts for a sibling hair project are:

1. Ordering is **namespace order** (reverse-sibling post-order under `Movers`), not relationship order (README.md:139-160; rigEvaluator.cpp:82-106).
2. Inputs are **relationships authored on the consuming prim** (`rigExec:transform`, `rigExec:cage`, `rigExec:driverFrames` ...), plus per-property `rigExecReadPhase` string metadata selecting which revision of a written target is read (moverGraph.h:63-111; plugInfo.json:8-13).
3. OpenExec 26.08 has **no reverse-relationship accessor and no prim→attribute accessor**, and value overrides of array attributes are type-checked against the element type; RigExec routes around all three with in-memory bindings, `ComputeWithOverrides`, and boxed packet types (mover-graph-cutover.md:89-110; types.h:44-66).
4. The point-array chain is a hand-built `VdfNetwork` rebuilt **every generation**, evaluated **serially** with `VdfSimpleExecutor`; no cross-frame VDF cache exists on that side (moverGraph.cpp:1385-1453; rigEvaluator.cpp:7009-7231).
5. Invalidation: value edits are caught synchronously by ExecUsd's own notice listener and by `RigExecImagingRegistry::_OnObjectsChanged`, which just re-evaluates at the last time; a structural digest mismatch triggers a **full recompile** of the rig (README.md:307-308; rigEvaluator.cpp:5598-5610; registry.cpp:423-471).

---

## 1. How prims become graph nodes

### 1.1 Discovery: by type under the rig root, no membership lists

| Kind | Rule | Evidence |
|---|---|---|
| Rig root | prim of type `RigExecRoot`; evaluator is constructed with `(stage, rigPath)` | `rigEvaluator.h:182`; `schema.usda:95-115` ("The rig declares no membership lists") |
| Joints | every `RigExecJoint` in `UsdPrimRange(rig)`, unioned with targets of any `rigExec:joints` relationship | `rigEvaluator.cpp:842-878` |
| Controls | every `RigExecControl` in `UsdPrimRange(rig)` (no union with solver `rigExec:controls`: "reading a control does not make it one; the type does") | `rigEvaluator.cpp:880-907` |
| Volume weights | `RigExecSphereWeight/PlaneWeight/CurveWeight` anywhere beneath the rig | `rigEvaluator.cpp:110-116, 909-928` |
| Aggregate solvers | `RigExecFkChain, TwoBoneIk, BlendPointFrames, TwistDistribution, Ribbon` by TYPE anywhere ("`Solvers` is the convention, not a requirement") | `rigEvaluator.cpp:932-962` |
| Movers | any prim under `<rig>/Movers` that has a `rigExec:moves` relationship; prims without it are grouping scopes; movers with an empty target list are **inert** (a notice, not an error) | `rigEvaluator.cpp:1836-1885` |

Rig-level output gate: a rig with no controls, joints, volumes, movers, and no inert movers fails Compile with "Rig publishes no outputs" (`rigEvaluator.cpp:2652-2659`). Instance proxies / prototype-hosted rigs are rejected (`rigEvaluator.cpp:1634-1638`).

### 1.2 Ordering: reverse-sibling post-order (one traversal primitive)

```cpp
// <usdrig-src>/libs/rigExec/rigEvaluator.cpp:82-106
std::vector<UsdPrim>
_GetMoverExecutionOrder(const UsdPrim &movers)
{
    std::vector<UsdPrim> ordered;
    if (movers) {
        for (const UsdPrim &prim : UsdPrimRange(movers)) { ordered.push_back(prim); }
        std::reverse(ordered.begin(), ordered.end());
    }
    return ordered;
}
```

- The same primitive feeds both the structural digest and compilation ("If those walks ever disagree, an order edit can retain the old epoch digest while executing a different chain", `rigEvaluator.cpp:88-91`).
- Each accepted mover gets an integer `ordinal` in that order (`rigEvaluator.cpp:1843, 1886`; `RigExecMoverRecord::ordinal` at `rigEvaluator.h:33-39`).
- Spec statement of the rule and why (usdview shows children top-to-bottom; the bottom sibling fires first): `<usdrig-src>/docs/spec.md:317` (§4.2) and `README.md:139-160`.
- The rigging library documents the consequence: "ADD ORDER IS REVERSE APPLICATION ORDER: the last added runs first" (`<usdrig-src>/libs/rigExecRigging/rigBuilder.h:17-22`).
- Multiple writers of one target are an ordinary stack; no reorder opinion is required (`rigEvaluator.cpp:2661-2679`).

### 1.3 Targets (`rigExec:moves`) and canonicalization

- `rigExec:moves` is declared by the applied API `RigExecMoverAPI` together with `inputs:enabled`, `inputs:defaultWeight` and `rigExec:weightObject` (`<usdrig-src>/libs/rigExecSchema/schema.usda:49-93`).
- A prim target canonicalizes to its `.points` property (`_PointsOf`, `moverGraph.cpp:744-747`); structural/topology properties (`faceVertexCounts`, `curveVertexCounts`, `basis`, `wrap`, ...) are never writable targets (`rigEvaluator.cpp:1890-1898`).
- Three output domains land in one pose: point chains (`VtVec3fArray`), property chains (float/vec3f/matrix "math movers"), and frame chains (constraints on transform providers) (`rigEvaluator.h:104-123`, `rigEvaluator.h:409-500`).

### 1.4 Relationship-based inputs and their build-time resolution

There is no generic port model: **each mover schema declares fixed relationship names**, and `RigExecResolveRevisionBinding` maps them to exact property paths per schema type:

```cpp
// <usdrig-src>/libs/rigExec/moverGraph.cpp:769-880 (excerpt)
if (schemaType == "RigExecMatrixMover") { ... _Targets(moverPrim, "rigExec:transform") ... }
else if (schemaType == "RigExecBlendShapeMover") { binding.blendInputs = _Targets(moverPrim, "rigExec:blendInputs"); std::sort(...); }
else if (schemaType == "RigExecLatticeMover") { binding.cagePoints = _PointsOf(cages[0]); ... }
else if (schemaType == "RigExecSurfaceMover") { surfacePoints/topologyCounts/topologyIndices from rigExec:surface }
else if (schemaType == "RigExecCurvenetMover") { rigExec:curvenet -> curvenetPoints }
else if (schemaType == "RigExecCurveMover") { rigExec:bindCoordinates, rigExec:driverFrames }
// every point-chain mover: rigExec:weightObject
```

- The binding struct (`moverGraph.h:267-306`) carries `transform, weightObject, base, topologyCounts/Indices, cagePoints, surfacePoints, bindCoords, driverFrames, widths, curvenet, curvenetPoints, blendInputs` plus a `phases` map and `transformPhase`.
- Schema type → frozen op: `RigExecRevisionOpForSchema` (`moverGraph.cpp:451-485`) — a `switch` on type name; `RigExecCurveMover` branches on `rigExec:mode` (`ribbon` vs `emitGuidePoints`).
- The op set is closed: `enum class RigExecRevisionOp { Matrix, BlendShape, VolumeCorrect, Smooth, Lattice, SurfaceProject, Ribbon, EmitGuidePoints, Curvenet, RecomputeNormals, RecomputeExtent }` (`moverGraph.h:52-64`). "No runtime operation dispatch inside a node" (`moverGraph.h:49-51`).

### 1.5 Read phases (which revision an input sees)

- Authored as **string metadata `rigExecReadPhase` on the relationship/attribute that names the input** (`moverGraph.h:63-77, 105-111`; registered in `<usdrig-src>/plugin/rigExecSchema/resources/plugInfo.json:8-13` as `SdfMetadata { "rigExecReadPhase": {"appliesTo": "properties", "type": "string"} }`). Legacy role-named attributes (`rigExec:cageReadPhase`, `rigExec:transformReadPhase`, ...) are still honored (`RigExecResolveReadPhase`, `moverGraph.cpp:600-660`).
- Values: `base | preceding | final | <absolute prim path>` (`RigExecParseReadPhase`, `moverGraph.cpp:557-598`). `AtPrim` naming a Scope means "after everything beneath it" because the walk is post-order (`moverGraph.h:86-92`).
- Example authoring: `rel rigExec:cage = </Asset/Geom/Cage> ( rigExecReadPhase = "final" )` (`<usdrig-src>/examples/13_ReadPhases.usda:177-179`).
- Resolution at evaluation: `RigExecChainSnapshots::Lookup(target, phase, readerMover)` (`moverGraph.cpp:671-725`): `Final` → recorded final; `Preceding` → revision before the reader in the same chain (null for the first revision = read the stage); `AtPrim` → the last recorded revision at or beneath the named prim.
- Cost control: `Compile` reduces every phase to the single revision it names and records it in `_snapshotPoints` (`rigEvaluator.cpp:4084-4166`); `Evaluate` calls `graph.Evaluate(head)` for an intermediate head only at those points (`rigEvaluator.cpp:7216-7224`) because evaluating an intermediate head re-runs the chain prefix ("would make a long chain quadratic", `rigEvaluator.h:375-382`).
- Validation: a phase on a property no mover writes is an error; `preceding` by a mover that does not write that chain is an error; an AtPrim self-read naming a later-or-equal ordinal is an error (`rigEvaluator.cpp:4093-4160`); `final` transform reads with a later frame-writer ordinal are rejected (`rigEvaluator.cpp:2681-2735`).

### 1.6 Cycles and cross-chain ordering

| Check | Where |
|---|---|
| Solver→solver dependency graph (indirect edges via joints another solver poses; direct edges via solver-valued relationships like `rigExec:inputA/B`); iterative DFS colouring; error names the cycle | `rigEvaluator.cpp:2965-3050` |
| Chain order between points targets: Kahn topological sort over `binding.phases` and curvenet edges; deterministic because it iterates a `std::map` in path order; unsatisfiable → compile error | `rigEvaluator.cpp:3991-4068` |
| Property-chain order + cycle rejection | `rigEvaluator.cpp:3771-3776, 3989` |
| Scalar attribute-connection cycles, weight-object composition cycles | `rigEvaluator.cpp:467-547, 3108-3139` |
| Spec position: "Cycles are schema validation errors ... RigExec never treats a cycle as a solver" | `docs/spec.md:1206` (§6.6) |

---

## 2. How the network is built, scheduled and executed

RigExec has **two** execution paths that a designer must keep apart.

### 2.1 Path A — OpenExec (ExecUsd) for everything that is a per-prim scalar value

Registration style (`EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA`) with a fixed static input list per schema. Representative registrations:

| Schema | Computations | Inputs (accessor → value) | Evidence |
|---|---|---|---|
| `RigExecJoint`, `RigExecControl`, `RigExecVolumeWeight` (abstract base) | `computeRestFrame`, `computePointFrame`, `computeMatrix` | `AttributeValue<GfMatrix4d>(rest:space/posed:space)`, avar doubles, `Attribute(posed:space).Connections<GfMatrix4d>(computeValue)`, `NamespaceAncestor<RigExecPointFrame>(computePointFrame/computeRestFrame)` | `<usdrig-src>/libs/rigExec/computations.cpp:333-419` (macro `RIGEXEC_REGISTER_XFORMABLE`, instantiated at 389-419) |
| `RigExecFkChain` | `computePointFrameArray` | `Relationship(rigExec:controls).TargetedObjects<RigExecPointFrame>(computePointFrame/computeRestFrame).Required()` | `computations.cpp:465-478` |
| `RigExecTwoBoneIk`, `RigExecBlendPointFrames`, `RigExecTwistDistribution` | `computePointFrameArray` | relationship-targeted frames + `AttributeValue<double/float/TfToken>` | `computations.cpp:480-730` (blocks following FkChain) |
| `RigExecStaticWeight`, `DynamicWeight`, `SphereWeight`, `PlaneWeight`, `CurveWeight`, `CombineWeight` | `computeWeightPacket` (+ `computeFalloffLut` stub on volumes) | `AttributeValue<float>(rigExec:values)` read vectorized; `Relationship(rigExec:weightTarget).TargetedObjects<GfVec3f>(computeValue)` for the points | `<usdrig-src>/libs/rigExec/moverKernels.cpp:1244-1388` |
| `RigExecBlendSample`, `RigExecBlendInput` | `computeBlendSampleData`, `computeBlendChannel` | | `moverKernels.cpp:1390-1431` |
| each mover type | `computeMoverParameters`, `computeMoverStatus` | `RIGEXEC_MOVER_COMMON_INPUTS` = `inputs:enabled`, `inputs:defaultWeight`, `Relationship(rigExec:weightObject).TargetedObjects<RigExecWeightPacket>(computeWeightPacket)` | `moverKernels.cpp:1425-1431, 1433-1667` |
| `RigExecRibbon` | `rigExec:computeDriverPoints`, `rigExec:computeRestDriverPoints` (return EMPTY, meant to be **overridden**), `computePointFrameArray` | `AttributeValue<int>(rigExec:sampleCount)` + the two packet computations | `moverKernels.cpp:1493-1539` |

Notes on the exec side:

- Custom value types registered with `TF_REGISTRY_FUNCTION(ExecTypeRegistry)`: `RigExecPointFrame, RigExecPointFrameArray, RigExecPointsPacket, RigExecWeightPacket, RigExecFalloffLut, RigExecBlendSampleData, RigExecBlendChannel, RigExecMoverParameters, RigExecMoverStatus` (`<usdrig-src>/libs/rigExec/types.cpp:96-107`). No `VtArray` is ever registered (`types.h:3-6`; spec §12.1 `docs/spec.md:1801`).
- Rules RigExec wrote down for the DSL (all verified against OpenUSD source in `<usdrig-src>/docs/exec-api-notes.md`): one `EXEC_REGISTER_COMPUTATIONS_FOR_SCHEMA` per schema per process (§1.1); inputs are optional by default, `.Required()` to enforce (§3.3); multiple relationship targets arrive on one input read with `VdfReadIterator<T>` (§4c); arrays enter as element vectors (`AttributeValue<GfVec3f>`, §4b); callbacks must be pure/thread-safe (§8).
- Inheritance: exec composes a prim's computations by walking the full ancestor type vector, so registering once on the abstract `RigExecVolumeWeight` serves the three concrete volumes (`computations.cpp:402-416`: "Registered ONCE on the ABSTRACT base ... exec/definitionRegistry.cpp _GetFullyExpandedSchemaTypeVector").
- There is **no `"Exec"` plugInfo block** anywhere in usdRig (grep of `plugin/`, `libs/`, `cmake/` for `"Exec"` returned nothing). The registrations run at static init because `librigExec` is a link dependency of `librigExecImaging`, which Plug loads as a library-type plugin (`CMakeLists.txt:119-131`, `plugin/rigExecImaging/resources/plugInfo.json.in`), and of every test/tool binary. `exec-api-notes.md:91-94` documents this as an allowed path ("If your computations are linked into the host app ... no Exec plugInfo block is needed").

The client side is `RigExecTapSet` (`<usdrig-src>/libs/rigExec/tapSet.h:122-178`, `tapSet.cpp`):

```cpp
// tapSet.cpp:46-97 (Prepare)  /  tapSet.cpp:105-168 (Evaluate)
keys.emplace_back(attr)  or  keys.emplace_back(prim, address.publicComputation);
_request = ExecUsdSystem::BuildRequest(std::move(keys), valueCallback -> _dirty=true, timeCallback -> _dirty=true);
_system->PrepareRequest(*_request);
...
_system->ChangeTime(time);
view = overrides.empty() ? _system->Compute(*_request)
                         : _system->ComputeWithOverrides(*_request, std::move(execOverrides));
snapshot._values.push_back(view.Get(i));   // copied immediately; cache view must not outlive request/system
```

- One `ExecUsdSystem` per tap set (`tapSet.cpp:17-21`), and the evaluator owns **four** tap sets per rig, each with its own system: `_taps` (authoritative), `_guideTaps` (observational solver guides), `_solverFrameTaps` (joint-posing solvers' aggregates, evaluated first), `_restFrameTaps` (rest frames for implied IK lengths) (`rigEvaluator.h:264-321`; construction at `rigEvaluator.cpp:3273-3300`).
- Overrides (`RigExecValueOverride`, `tapSet.h:72-84`) can target a prim computation **or an attribute's computeValue** (`tapSet.cpp:128-146`).
- Overrides apply to one call only; interest renewal caveat for `ComputeWithOverrides` is noted in spec §6.3 (`docs/spec.md:1158`).

### 2.2 Path B — the in-memory VdfNetwork for point arrays (`RigExecMoverGraph`)

Node shape:

```cpp
// <usdrig-src>/libs/rigExec/moverGraph.cpp:56-80
class _RevisionNode final : public VdfNode {
    _RevisionNode(VdfNetwork *network, RigExecRevisionOp op)
        : VdfNode(network,
              VdfInputSpecs()
                  .ReadConnector<RigExecMoverParameters>(_tokens->parameters)
                  .ReadConnector<RigExecMoverStatus>(_tokens->status)
                  .ReadWriteConnector<GfVec3f>(_tokens->previous, _tokens->out),
              VdfOutputSpecs().Connector<GfVec3f>(_tokens->out)), _op(op) {}
    void Compute(const VdfContext &ctx) const override;   // switch(_op)
};
```

Graph construction and evaluation:

```cpp
// moverGraph.cpp:1385-1453
VdfMaskedOutput RigExecMoverGraph::AddPointSource(target, points) {
    auto *source = new VdfInputVector<GfVec3f>(&_network, count); source->SetValue(i, points[i]);
    return VdfMaskedOutput(source->GetOutput(), VdfMask::AllOnes(count ? count : 1));
}
VdfMaskedOutput RigExecMoverGraph::AddRevision(op, previous, parameters, status) {
    // parameters/status are VdfInputVector<..>(&_network, 1) constants for this generation
    _network.Connect(paramSource->GetOutput(), revision, _tokens->parameters, VdfMask::AllOnes(1));
    _network.Connect(statusSource->GetOutput(), revision, _tokens->status, VdfMask::AllOnes(1));
    _network.Connect(previous.GetOutput(), revision, _tokens->previous, previous.GetMask());
    return VdfMaskedOutput(revision->GetOutput(_tokens->out), previous.GetMask());
}
VtVec3fArray RigExecMoverGraph::Evaluate(output) {
    VdfRequest request(output); VdfSchedule schedule;
    VdfScheduler::Schedule(request, &schedule, /* topologicalSort */ true);
    VdfSimpleExecutor executor; executor.Run(schedule);
    auto values = executor.GetOutputValue(*output.GetOutput(), output.GetMask())->GetReadAccessor<GfVec3f>();
    ...copy into VtVec3fArray
}
```

Facts that matter for a hair system:

| Fact | Evidence |
|---|---|
| The graph is a **per-generation throwaway**: `RigExecMoverGraph graph;` is constructed inside the per-target loop of `Evaluate()` and destroyed at loop end; nothing is cached across frames on the VDF side | `rigEvaluator.cpp:6997-7009, 7230-7231` |
| Executor is `VdfSimpleExecutor` — depth-first, serial, "computes all the outputs in the schedule" | `moverGraph.cpp:1439`; `<openusd-src>/pxr/exec/vdf/simpleExecutor.h:28-33, 58-70` |
| The parallel engine exists (`VdfParallelExecutorEngine`, "does not perform cycle detection") and is what ExecSystem's runtime uses, gated by `VDF_ENABLE_PARALLEL_EVALUATION_ENGINE` (default true) | `<openusd-src>/pxr/exec/vdf/parallelExecutorEngine.h:32-45`; `<openusd-src>/pxr/exec/exec/runtime.cpp:56, 261`; `<openusd-src>/pxr/exec/vdf/types.cpp:13` |
| Masks: `VdfMask::AllOnes(count)` is the only mask used; masks are threaded through but no sparse/per-element masking is exploited ("per-element masks driving sparse recomputation" is listed as a future benefit of the graph form) | `moverGraph.cpp:1394, 1425`; `moverGraph.h:11-17` |
| Pass-through is a zero-copy reference: `ctx.SetOutputToReferenceInput(previous)` for disabled/failed/kind-mismatch revisions | `moverGraph.cpp:101-113, 296-303, 350-356` |
| In-place write through the READWRITE connector: `VdfReadWriteIterator<GfVec3f> out(ctx, previous)`; **never `Allocate`** on a READWRITE output (fails "output cannot hold a boxed value" and silently passes through) | `moverGraph.cpp:132-137, 401-410`; `docs/mover-graph-cutover.md:511-517` |
| Anything talking to VDF directly must force `ExecTypeRegistry::GetInstance()` before use or the executor cannot distinguish registered value types | `docs/mover-graph-cutover.md:517-519`; `tests/testRigExecMoverGraph.cpp:594` |
| Cardinality is fixed per epoch: a kernel that changes point count passes through; derived chains "cannot resize" through the READWRITE connector | `moverGraph.cpp:124-127, 308-316`; spec §7.7 `docs/spec.md:1383-1392` |
| The per-mover packet (`RigExecMoverParameters`, `types.h:158-226`) is fully materialized (`std::vector` copies of topology, cage, bind coords, frames) per revision per generation; large static inputs are copied every frame | `moverGraph.cpp:1082-1380` (`RigExecAssembleParameters`) |
| Scratch discipline: collect the vectorized input into a `std::vector<GfVec3f>`, run a kernel, blend by envelope, write back before returning | `_RunScratchKernel`, `moverGraph.cpp:97-139` |

### 2.3 The per-generation evaluate sequence (`RigExecRigEvaluator::Evaluate`)

`rigEvaluator.cpp:5594-7470` in order:

1. Recompile if uncompiled or if `_ComputeStructureDigest() != _structureDigest` (5598-5610).
2. Property chains (math movers) evaluated off the stage first → results become **attribute overrides** for exec and a `RigExecResolvedInputs` overlay for static reads (5625-5650; `moverGraph.h:112-197`).
3. Ribbon driver points read at `time` and at `Default()` → boxed `RigExecPointsPacket` overrides on `rigExec:computeDriverPoints` / `rigExec:computeRestDriverPoints` (5652-5673).
4. Implied TwoBoneIk lengths from a rest-frame tap set (5671-5740).
5. Solver aggregates evaluated in `_solverFrameTaps` and **iterated to a fixed point** with each joint's `computePointFrame` overridden by its extracted element; bound `maxRounds = _jointSolverArrayTaps.size() + 1`; non-convergence refuses to publish (5789-5900).
6. Baked falloff LUT overrides appended (5910-5914).
7. Authoritative request evaluated once with all overrides (5922).
8. Frame constraints (pose domain) applied in memory in mover order (frame chains; `rigEvaluator.h:409-483`).
9. Point chains: for each target in `_chainOrder`, build a fresh `RigExecMoverGraph`, `AddPointSource(base points at time)`, one `AddRevision` per `_GraphRevision` with packets assembled from tapped provider values (`transform`, `weights`, `driverFrames`, blend channels) and phase-resolved static reads, then `graph.Evaluate(head)` → `pose.movedProperties[target]` (6997-7236).
10. Derived normals/extent chains keyed by the points target, evaluated right after with the final points as base (7236-7268).
11. Optional CPU parity oracle (`cpuParityMode`) and counters `moverGraphParityMismatches/Agreements` (`rigEvaluator.h:118-166`).

### 2.4 What is and is not cached

- ExecUsd caches computed values across `ChangeTime` and only invalidates values whose inputs differ (`docs/execusd-api-notes.md:73-77`, verified in the notes against `testExecUsdRequest.cpp`). RigExec keeps its prepared requests across frames (Prepare once in Compile).
- The Profile Mover's cut-mesh/factorization is cached on the evaluator keyed by a digest of rest net, spline indices, rest points and topology (`RigExecCurvenetBindCache`, `moverGraph.h:308-350`; `moverGraph.cpp:487-535`; digest at `moverGraph.cpp:1239-1255`).
- Everything on the VDF mover graph side is recomputed per generation (see §2.2).
- Spec budgets that the design targets (not measured in this repo): warm pose+proxy p95 ≤ 14 ms, single callback p95 ≤ 2 ms, local structural recompile p95 ≤ 100 ms (`docs/spec.md:2052-2076`). No timing instrumentation exists in `rigEvaluator.cpp` (grep for Timer/TRACE found nothing).

---

## 3. Invalidation: attribute edit vs structural edit

### 3.1 Value (attribute) edits

- ExecUsdSystem subscribes to `UsdNotice::ObjectsChanged` itself and invalidates cached values synchronously during authoring; request callbacks fire synchronously on the authoring thread and must not re-enter exec (`docs/execusd-api-notes.md:58-62, 178-183`). RigExec's callbacks only flip `_dirty` (`tapSet.cpp:82-88`; `ConsumeDirty()` at `tapSet.h:163`).
- The imaging registry registers its own `TfNotice` listener for `UsdNotice::ObjectsChanged` on the stage **before** `Compile()` so it is delivered **after** exec's listener (Tf prepends deliverers; registering after Compile made published generations one edit stale — measured on `11_VolumeWeights`) (`<usdrig-src>/libs/rigExecImaging/registry.cpp:225-266`).
- `_OnObjectsChanged` filters resynced/changed-info paths by prefix against active asset roots and, if relevant, calls `SetTime(_lastTime)` — i.e. **it re-evaluates the whole rig at the current time**; no per-index dirty mapping is consumed on this path ("The evaluator's epoch digest turns structural edits into recompiles; value edits flow through exec invalidation") (`registry.cpp:423-471`).
- `SetTime` serializes on a plain `std::mutex`, evaluates every session, and atomically publishes; a failed evaluation publishes a cleared generation (`registry.cpp:343-380`).
- The usdview plugin's job is small: `Tf.Notice.Register(Usd.Notice.ObjectsChanged, ...)` to (re)activate when a `RigExecRoot` appears, and `currentFrameChanged -> RigExecImaging_SetTime(frame)` via ctypes (`<usdrig-src>/plugin/rigExecUsdview/rigExecUsdview.py:63-69, 429-437, 479-500, 600-610`).

### 3.2 Structural edits: digest → full recompile

- Structural identity = a string digest over: mover paths, schema types, targets, ordinals, relationship identities (some sorted), read-phase metadata, attribute connection chains (paths, types, sample counts), weight-object shape (representation, policy, sorted sparse support, values size, baked falloff LUT bytes), volume shape tokens, frame-binding provider types (`rigEvaluator.cpp:984-1230`).
- Deliberately excluded (value-only): `inputs:falloffMin/Max`, `invert`, `strength`, `scaleX/Y/Z`, `extentU/V` ("hashing them would recompile every frame an artist scrubs one", `rigEvaluator.cpp:1175-1183`). `uniform` tokens that select an op (`rigExec:mode`, `*ReadPhase`) are rejected at compile if time-sampled (`docs/mover-graph-cutover.md:360-365`; digest hashes `#samples=`, `rigEvaluator.cpp:1030-1040`).
- Granularity: **whole rig**. `Evaluate` recomputes the digest each generation; any mismatch calls `Compile()` again, which tears down all four `ExecUsdSystem`s and rebuilds them (`rigEvaluator.cpp:5603-5610, 3060-3066`). README limitation: "Structural edits currently trigger a full in-memory recompile rather than an incremental graph update" (`README.md:307-308`). Spec's aspiration of incremental epochs (§6.1 "Structural changes incrementally rebuild affected areas", `docs/spec.md:1128`) is **not implemented**.
- Compile is transactional: Phase A validates into locals; "a failed structural edit keeps the previous epoch publishable" (`rigEvaluator.cpp:1640-1643`), although after Phase B teardown a failed compile leaves the evaluator uncompiled (`rigEvaluator.cpp:3065-3069`).
- On the imaging side, a changed `GetBindingEpochDigest()` publishes a replacement `BindingEpoch` before value notices (`<usdrig-src>/libs/rigExecImaging/bridge.cpp:1212-1222`; spec §10.4 `docs/spec.md:1632-1665`).

### 3.3 Cost of one digest

`_ComputeStructureDigest` walks the whole `Movers` subtree, every relationship target, every attribute connection chain, and re-bakes falloff LUTs for volumes (`rigEvaluator.cpp:1224-1229`) **on every `Evaluate`**. For a hair graph with many operators this is a per-frame cost to design around (see §10).

---

## 4. The snapshot / generation model

- Evaluator output: `RigExecRigPose` — a plain value struct (`std::map`s of frames, matrices, `VtValue` moved properties, resolved weight fields, diagnostics, parity/convergence counters) (`rigEvaluator.h:52-178`). It is immutable by convention (returned by value; no mutators).
- `RigExecSnapshot` (tap-level) copies `VtValue`s out of the `ExecUsdCacheView` immediately; `IsValid()` vs `IsComplete()` distinguish "ran" from "every tap produced a value" (`tapSet.h:86-118`; `tapSet.cpp:154-167`).
- Imaging: `RigExecSnapshotStore::Publish(shared_ptr<RigExecImagingSnapshot>)` diffs against the previous generation per prim and per leaf, returns a dirty vector, and `std::atomic_store`s the new generation; `Get()` is a lock-free `std::atomic_load` (`<usdrig-src>/libs/rigExecImaging/snapshotStore.h` — see the dump `/tmp/.../research/_rigExecImagingHeaders.txt`, `class RigExecSnapshotStore`, Publish/_Diff; `libs/rigExecImaging/snapshotStore.h:328-377` per grep).
- Structural vs value dirt on the Hydra side is decided by `_Diff`: leaf-set ownership changes, guide count/type changes, overlay on/off, asset-root change ⇒ structural (universal dirty / resync); otherwise per-leaf value dirt (snapshotStore.h `_Diff`).
- Generation counter: `snapshot->generation = ++_generation` in the bridge (`bridge.cpp:1167, 1344`); C API `RigExecImaging_GetGeneration()` (`registry.h:486-487` in the dump).
- Spec framing: "Evaluation always completes before publication ... Hydra pulls never compute or wait" (`docs/spec.md:1410-1426`, §8.2).

---

## 5. How kernels are written

| Topic | What RigExec does | Evidence |
|---|---|---|
| Pure CPU reference kernels over `std::vector<GfVec3f>` | `RigExecApplyVolumeCorrect`, `RigExecApplyLaplacianSmooth`, `RigExecComputeVertexNormals`, `RigExecComputeExtent`, `RigExecApplyLattice`, `RigExecApplySurfaceProject`, `RigExecSampleCurveRMF`, `RigExecApplyRibbonTransport` | `<usdrig-src>/libs/rigExecMath/geometryKernels.h:22-114` |
| SIMD | One SSE2 kernel `RigExecApplyWeightedMatrixSimd` (row-vector convention, per-point, float math) with scalar fallback on non-SSE targets; `RIGEXEC_DISABLE_SSE2` compile define; `RIGEXEC_ENABLE_SIMD` env var (default true) chooses the path at runtime | `<usdrig-src>/libs/rigExecMath/simdKernels.cpp:1-80`; `moverGraph.cpp:401`; `moverKernels.cpp:924` |
| No TBB / no Work* in kernels | grep across `libs/` for `WorkParallel|tbb|std::thread` found none in `rigExec`/`rigExecMath`; only `std::mutex`/`std::atomic` in `rigExecImaging` | grep result (registry.cpp, snapshotStore.h, sceneIndices.cpp only) |
| Threading rules | Callbacks are pure and read only declared inputs; VDF parallel engine may run exec callbacks concurrently; RigExec's own mover graph runs serially; publication is under one mutex; Hydra `GetPrim()` only reads the atomic snapshot | `docs/exec-api-notes.md` §8 item 11; `docs/spec.md:1164-1184` (§6.4 table); `registry.cpp:343-346` |
| Determinism | fixed reduction order (sorted blend inputs `moverGraph.cpp:818`), Kahn over `std::map`, digests over sorted targets, double for frames / float for bulk geometry, "Reductions use a fixed tree; no result depends on unordered-container iteration" | `docs/spec.md:1199-1208` (§6.6); `rigEvaluator.cpp:4028-4030` |
| Envelope blend with exact endpoints | `RigExecBlendEnvelope(preceding, full, w)` returns the endpoint for `w<=0`/`w>=1` without arithmetic | `<usdrig-src>/libs/rigExecMath/envelope.h:16-30` |
| Failure semantics | disabled → pass-through; invalid packet → `moverFailed` + pass-through with `firstBadAddress`; never a partial write (envelope resolved before any element is written) | `moverGraph.cpp:956-975` (`RigExecStatusForParameters`); `moverGraph.cpp:357-361, 394-399` |
| Precision | frames are `GfVec3d` point frames `[O,X,Y,Z]`; points are `GfVec3f` | `<usdrig-src>/libs/rigExecMath/pointFrame.h:37-54` |

Spec §6.5 spells out the allowed private layouts (direct SIMD over contiguous VDF storage, ephemeral SoA scratch, 64-byte-aligned strips, AVX2/NEON, optional GPU for whole chain segments) and forbids any private layout in registered types, taps, snapshots or Hydra (`docs/spec.md:1186-1197`). Only the SSE2 matrix kernel is implemented.

---

## 6. The weight-field concept (a model for masks on hair operators)

Weight objects are separate prims that publish a `computeWeightPacket`; a mover binds at most one via `rigExec:weightObject`, and that packet becomes the mover's **common envelope** applied after the full-strength candidate (`schema.usda:49-93`; `_RunScratchKernel` `moverGraph.cpp:126-131`).

```cpp
// <usdrig-src>/libs/rigExec/types.h:67-114
struct RigExecWeightPacket {
    TfToken representation;      // constant, dense, or sparse
    TfToken rangePolicy;         // strict or clamp
    std::vector<float> values;   // 0, logicalCount, or indices.size()
    std::vector<int> indices;    // sparse support (sorted)
    float defaultWeight = 0.0f;
    bool valid = false;
    float Resolve(size_t i, size_t count) const;
    static RigExecWeightPacket Constant(float weight);
    bool ResolveAll(size_t count, std::vector<float> *resolved) const;
};
```

| Kind | Schema | Inputs | Kernel | Evidence |
|---|---|---|---|---|
| Painted (static) | `RigExecStaticWeight` (`rigExec:weightTarget`, `representation`, `rangePolicy`, `rigExec:values`, `rigExec:indices`, `rigExec:defaultWeight`) | authored arrays read vectorized; sparse pairs sorted, duplicates rejected; dense default must be 0 | `_BuildStaticWeightPacket` | `moverKernels.cpp:167-233`; `schema.usda:943-980` |
| Dynamic | `RigExecDynamicWeight` (`rigExec:baseWeight`, `rigExec:operation`, `inputs:driver/scale/bias`) | base packet via relationship + scalars | `_BuildDynamicWeightPacket` | `moverKernels.cpp:236-310`; `schema.usda:981-1008` |
| Volumetric | `RigExecSphereWeight/PlaneWeight/CurveWeight` (placeable `RigExecXformable`s; `inputs:falloffMin/Max/invert/strength`, `rigExec:falloffProfile`, `rigExec:falloffCurve` spline, `rigExec:samplePhase`, `rigExec:sampleSource`, scales/extents) | target points via `Relationship(rigExec:weightTarget).TargetedObjects<GfVec3f>(computeValue)`; own placement via `computePointFrame`; falloff LUT via an **overridden stub** `computeFalloffLut` (exec has no spline accessor) | `RigExecSphereWeightField` etc. in `weightFields.h` | `moverKernels.cpp:412-475, 1280-1362`; `types.h:97-127`; `<usdrig-src>/libs/rigExecMath/weightFields.h` |
| Composition | `RigExecCombineWeight` (`rigExec:inputWeights`, `rigExec:combineMode` = multiply/add/subtract/max/min/average/overlay) | ordered fold; Subtract/Overlay are order-dependent | `RigExecCombineWeightFields` | `weightFields.h:167-219`; `moverKernels.cpp:1364-1388` |

Useful details for hair masks:

- Dense volume fields are computed in the **volume's local space** so a non-uniform placement gives ellipsoidal/sheared isosurfaces for free (`weightFields.h:1-16`).
- `rigExec:samplePhase = current` samples the in-flight points at the mover's position in the stack rather than the authored base (`rigEvaluator.h:508-512`; `rigEvaluator.cpp:3210`).
- Falloff profile/curve is epoch-structural and baked to a 257-entry LUT (`weightFields.h:33-49`; `rigEvaluator.cpp:1211-1229`).
- Resolved fields are published per generation for overlay drawing (`RigExecRigPose::weightFields/weightFrames`, `rigEvaluator.h:122-147`).
- Range policy: `strict` fails the mover on out-of-range, `clamp` clamps (`moverKernels.cpp:151-164`).

---

## 7. Existing curve-related ops and what is reusable

| Op | Schema / entry | What it computes | Reuse verdict |
|---|---|---|---|
| `RigExecSampleCurveRMF(controlPoints, sampleCount)` | `geometryKernels.cpp:412-538` | Dense presample of a **nonperiodic cubic uniform B-spline** (stock BasisCurves `cubic/bspline`; <4 CVs → linear polygon), equal arc-length resampling, central-difference tangents, rotation-minimizing normals by **double reflection (Wang et al. 2008)** with a deterministic initial normal (world axis least parallel to t0) | Directly reusable for guide-curve frames (parallel transport along a guide, twist-free). Limitations: only bspline basis, single curve, float, arc-length only (`rigExec:parameterization = parametric` is declared in schema but the kernel always arc-lengths — `schema.usda:611-613` vs kernel) |
| `RigExecRibbon` solver | `moverKernels.cpp:1193-1241, 1493-1539`; `schema.usda:581-650` | Publishes `RigExecPointFrameArray` of `sampleCount` frames (posed + rest) from live and Default-time driver points supplied as `RigExecPointsPacket` overrides | Pattern reusable: "frames along a curve as an aggregate value" ; `startFrame/endFrame/twistFrames` relationships are declared but UNVERIFIED whether consumed by the kernel (kernel reads only sampleCount and the two packets) |
| `RigExecCurveMover` mode `ribbon` | `moverGraph.cpp:207-246`; `RigExecApplyRibbonTransport` `geometryKernels.cpp:541-586` | Per point: `u = bindCoords[i][0]` → interpolate between two rigid maps `PointsToMatrix(rest_k, posed_k)`; bind coords come from an authored `primvars:st` (`examples/ArmRig.usda:314`) | Reusable as the "deform static curves with a deforming surface/curve" primitive **if** the binding coordinate concept is generalized (only `u` is used; `v`,`w` are ignored in the implementation despite spec §7.5 `p' = c(u)+v n(u)+w b(u)`) |
| `RigExecCurveMover` mode `emitGuidePoints` | `moverGraph.cpp:194-206` | Overwrites each point with `frames[i].Origin()` (requires `frames.size()==points.size()`) | Trivial; shows how a mover can **write a BasisCurves/Points target from an aggregate** |
| `RigExecCurveWeight` | `moverKernels.cpp:567-598`; `weightFields.h:129-160` | Distance-to-polyline weight field | Reusable for "mask by proximity to a guide curve" |
| Curvenet / Profile Mover | `libs/rigExecMath/curvenet.h`, `profileMover.h`, `cutMesh.h`, `sparseSolve.h`; `schema.usda:1544-1620`; `docs/curvenet.md` | Net of cubic splines over a shared point pool (index sharing = connectivity), sampled per spline, frames/gradients per segment side, harmonic interpolation + Poisson reconstruction with a cached Cholesky factorization | Not hair-related, but `RigExecCurvenet` (inherits `Points`) is the precedent for a **curve topology prim expressed as a PointBased with an index array**, and `RigExecCurvenetBindCache` is the precedent for epoch-keyed expensive binds |
| Extent for curves | `RigExecComputeExtent(points, widths)` | two-element extent widened by widths | Reusable for BasisCurves extent maintenance |

Missing for hair (not present anywhere in usdRig): multi-curve batches (`curveVertexCounts` handling in kernels), widths chains, Catmull-Rom/Bezier/linear bases in `RigExecSampleCurveRMF`, per-curve parameterization, surface (u,v)+normal attachment (only `SurfaceProject` closest-point exists, `geometryKernels.h:78-84`), any instancing/scatter, any texture sampling.

---

## 8. Python facade and schema authoring

### 8.1 Layers

| Layer | Contents | Evidence |
|---|---|---|
| `libs/rigExecRigging` (C++ SHARED, links `rigExecMath sdf tf gf vt usd`, **not** `rigExec`) | `RigExecRigBuilder` + typed handles (`RigExecMoverHandle::SetEnabled/SetDefaultWeight/SetWeightObject/SetMoves/SetReadPhase`, `RigExecControlHandle`, `RigExecJointHandle`, solver handles, constraint handles, weight handles); `RigExecSchemaPrim` strict authoring (`Define/Get/HasAPI/ApplyAPI/SetAttribute/SetRelationship/ClearAttribute/SetReadPhase`) that resolves every property from the composed `UsdPrimDefinition` and throws on undeclared names | `CMakeLists.txt:106-117`; `rigBuilder.h:50-460` (grep index); `<usdrig-src>/libs/rigExecRigging/schemaAuthoring.h:33-99` |
| `python/_rigexec.cpp` (pybind11 module `_rigexec`) | `PointFrame`, `Rig` (`compile`, `evaluate(time)`, `binding_epoch_digest`, `mover_order`), `Pose` (`joint_frame`, `joint_matrix`, `control_frame`, `provider_xform`, `solver_frames`, `moved_property`, `moved_properties`, `weight_field`, `weight_frame`), `SchemaPrim`, `Handle` + all typed handles, `Builder` | `<usdrig-src>/python/_rigexec.cpp:565-1104` |
| `python/rigexec/__init__.py` | Windows DLL directory setup, `load_schema_plugin()`, `identity()`, `schema` namespace (`_SchemaNamespace.define(stage, path, schema_type)`, per-class `define/get`), `ControlAPI.apply`, `MoverAPI.apply`, `Builder` wrapper with `create(stage, rig_root, partition)`, `new_mover_chain`, `chain.under(parent_mover)` | `<usdrig-src>/python/rigexec/__init__.py:1-140, 184-553`; `README.md:173-240` |
| Schema plugin | codeless (`skipCodeGeneration = true`) `schema.usda`, generated `plugInfo.json` + `generatedSchema.usda`; CMake rewrites `"Type": "resource"` → `"library"` and injects a `LibraryPath` to `librigExecImaging` so Plug can load compute-extent code on demand; `implementsComputeExtent` added to Boundable RigExec types | `schema.usda:18-25`; `CMakeLists.txt:425-473` |

### 8.2 Contracts worth copying

- Type-name based discovery means the schema can stay codeless; the evaluator compares `prim.GetTypeName()` strings (`rigEvaluator.cpp:110-131`, `moverGraph.cpp:451-485`).
- Strict authoring rejects misspelled properties instead of creating custom attributes (`README.md:222-224`; `schemaAuthoring.h:60-76`).
- `mover_order()` exposes compiled ordinals so tools can show the stack in the same order the engine runs (`_rigexec.cpp:624`).
- The usdview "graph editor" in this repo is an **animation-curve (spline) editor**, not a node-graph editor (`docs/graph-editor.md` sections "Tangent types", "Infinity"; `plugin/rigExecUsdview/graphModel.py` is all `Ts` spline manipulation). There is no node-graph authoring UI to reuse.

---

## 9. The OpenExec constraints usdRig documents, and how each was routed around

Source: `<usdrig-src>/docs/mover-graph-cutover.md:89-110` ("The OpenExec constraint that shapes all of this") plus verified OpenUSD lines.

| Constraint (OpenExec 26.08) | Verified at | RigExec's route-around |
|---|---|---|
| Input accessors are exactly `Attribute, Relationship, Prim, Stage, NamespaceAncestor, Computation, Metadata, Constant`; header says `// XXX:TODO Property, NamespaceParent, NamespaceChildren, etc.` — **no reverse-relationship accessor** | `<openusd-src>/pxr/exec/exec/computationBuilders.h:840-841` | Solver→joint binding kept in memory (`_jointSolverBinding`) and each joint's `computePointFrame` supplied by `ComputeWithOverrides`; iterated to a fixed point (`rigEvaluator.h:279-296`; `rigEvaluator.cpp:5789-5900`) |
| A `Relationship` accessor requests computations on the **targets**, it cannot name an attribute of them (prim→attribute accessors are TODO) | `computationBuilders.h:584, 840-841`; cutover.md:20-24 | Ribbon driver points read by the evaluator and pushed as overrides on stub computations (`moverKernels.cpp:1493-1539`) |
| Value override of a `point3f[]` attribute is type-checked against the **element** type, so an array cannot be overridden | `<openusd-src>/pxr/exec/exec/system.cpp:100-175` (`outputType != overrideType` → coding error); cutover.md:25-30 | Box the array: `RigExecPointsPacket { std::vector<GfVec3f> points; }` registered as a scalar (`types.h:44-66`) |
| No accessor for an attribute's spline (`// XXX:TODO Accessors for AnimSpline`) | `computationBuilders.h:584-585` | Bake falloff splines to a LUT at Compile and override a stub `computeFalloffLut` (`types.h:97-127`; `rigEvaluator.cpp:1211-1229`) |
| `VtArray` is not an exec value/result type; arrays flow as element vectors | `docs/exec-api-notes.md` §2 (static_assert in typeRegistry) | Read with `VdfReadIterator<GfVec3f>`; point arrays for the chain are handled outside exec in the VdfNetwork |
| Computations cannot add prims/properties or change topology | spec §7.7 `docs/spec.md:1383-1392` | Fixed cardinality per epoch; derived chains cannot resize |
| Static input lists; no dynamic property/namespace-child discovery | spec §3.4 gap matrix `docs/spec.md:201` | Every mover schema declares fixed relationship names; the evaluator resolves paths at compile |
| Callbacks synchronous; no cancellation; `ChangeTime/Compute` undocumented for concurrency | `docs/execusd-api-notes.md:58-62, 178-183`; spec §6.3 `docs/spec.md:1162` | One mutex around evaluate+publish; dirty flags only in callbacks |
| One registration macro per schema per process | `docs/exec-api-notes.md:39-41`; `computations.cpp:402-416` | Split registrations by TYPE across files (abstract base for shared computations) |

Also chosen around, not forced: the original design of registering the point chain as generated "application prims" with `AttributeExpression<GfVec3f>` plus a passive `outputs:value` bridge (spec §7.2 `docs/spec.md:1291-1314`; macro `RIGEXEC_REGISTER_ARRAY_HOST` still present but unused, `moverKernels.cpp:1674-1751`) was **abandoned** because it required authoring a derived stage and a recomposition; the in-memory `VdfNetwork` replaced it (cutover.md:1-12; `moverGraph.h:1-17`).

---

## 10. Recommendations for a sibling `usdGen` hair project

### 10.1 What to link against vs re-implement

| Piece | Link? | Rationale |
|---|---|---|
| `rigExec::rigExecMath` (STATIC, deps `arch tf gf vt`) | **Yes** for `RigExecSampleCurveRMF`, `RigExecApplyRibbonTransport`, `RigExecComputeExtent`, `RigExecPointFrame`/`RigExecPointsToMatrix`, weight-field kernels (`weightFields.h`), `RigExecBlendEnvelope` | Pure, USD/exec-free, tested (`testRigExecMath`, `testRigExecWeightFields`); exported by the CMake package (`CMakeLists.txt:59-76`, `cmake/rigExecConfig.cmake.in`) |
| `rigExec::rigExec` (SHARED; exec/execUsd/vdf) | **Optional, narrow**: `RigExecTapSet` as the ExecUsd façade (Add/AddResolved/Prepare/Evaluate with overrides, dirty flag), and `RigExecRigEvaluator` only if usdGen wants to *drive* a rig in-process (e.g. read `movedProperties` for a deformed scalp) | The evaluator is rig-specific (joints/solvers/constraints). Its exec **types** (`RigExecPointFrame` etc.) must not be re-registered by usdGen; consuming them requires linking `rigExec` |
| `rigExec::rigExecImaging` | Consult only. The hair system should read the deformed scalp from the **Hydra scene index** (the user requirement) rather than from `RigExecRigPose`, so the two engines stay decoupled | usdRig publishes final points/normals/extent via `RigExecResultsSceneIndex` (dump `_rigExecImagingHeaders.txt`, `class RigExecResultsSceneIndex`) |
| `RigExecMoverGraph` / `_RevisionNode` | **Re-implement** (it is `final`, the op enum is closed, and the node holds a `RigExecMoverParameters` packet of rig-specific fields) | `moverGraph.h:52-64, 449-489` |
| Schema authoring pattern (`RigExecSchemaPrim`, codeless schema, pybind facade) | **Copy the pattern**, new library | `schemaAuthoring.h`; `_rigexec.cpp` |

### 10.2 Graph model for `generator -> clump -> generator -> clump -> frizz` wired by relationship

RigExec's ordering rule (namespace order) works because every mover writes a **pre-existing** target property and the chain is implicit. A hair operator chain has different needs: operators produce **new** curve sets (topology born inside the graph), and users want explicit wiring. The evidence above suggests this shape:

1. **Explicit input relationship per operator** (`usdGen:input` → previous operator prim), the way usdRig uses `rigExec:driverFrames`/`rigExec:cage`, and derive execution order by **topological sort over relationships** (Kahn over a `std::map`, exactly as `rigEvaluator.cpp:4025-4068` does for chain order) rather than by namespace. Cycle detection: the iterative colour-DFS at `rigEvaluator.cpp:3028-3050`. Keep namespace order only as a deterministic tie-break.
2. **Do not put the curve arrays in OpenExec.** OpenExec 26.08 cannot create topology (§9 table), cannot override arrays without boxing, and cannot reach an attribute through a relationship. Put per-operator scalar/packet parameters in exec (`computeOperatorParameters` per schema, with `AttributeValue<T>` inputs and `Relationship(...).TargetedObjects<Packet>(...)` for masks/textures), and keep the curve data in an in-memory VdfNetwork (or a plain hand-written DAG) of boxed **`CurveBatch` packets** (`std::vector<GfVec3f> points; std::vector<int> vertexCounts; widths; per-curve ids; primvars`). RigExec's `RigExecPointsPacket` is the precedent (`types.h:44-66`).
3. **Persist the network across frames.** Unlike RigExec (graph rebuilt per generation, serial `VdfSimpleExecutor`), a hair graph must be built once per epoch and re-evaluated per value change. Two options both supported by 26.08 headers: (a) keep `VdfNetwork` + `VdfSchedule` alive and re-run an executor per generation, or (b) use `VdfDataManagerBasedExecutor<VdfParallelDataManagerVector, ...>` with the parallel engine (`<openusd-src>/pxr/exec/exec/runtime.cpp:56, 261` shows the composition ExecSystem uses; whether a non-exec client can instantiate that executor is UNVERIFIED). RigExec proves that hand-built `VdfNode`s with `ReadWriteConnector` and `SetOutputToReferenceInput` work, and documents the two traps (`cutover.md:511-519`).
4. **Dirty propagation:** mirror RigExec's split. Value edits → `ExecUsdRequest` value callbacks (per request index) + a stage `ObjectsChanged` listener registered **before** the ExecUsdSystem is created (`registry.cpp:225-266`); map dirty request indices to operator nodes (RigExec does not do this today — it re-evaluates all; a hair graph should). Structural edits → digest over operator paths/types/relationships/read phases only (`rigEvaluator.cpp:984-1230`), excluding animated scalars, and rebuild only the affected sub-DAG (RigExec's known limitation, `README.md:307-308`).
5. **Masks:** adopt the weight-object model verbatim: a separate prim publishing a packet (`representation` constant/dense/sparse, `rangePolicy`, combine prims, volumes with `samplePhase`), bound by one relationship, applied as a post-kernel envelope with exact endpoints (`envelope.h`). Extend `representation` with `texture`/`ptex`/`expr` producers that publish the same packet type; the LUT-override trick (`computeFalloffLut`) shows how to inject data exec cannot read (a texture, a SeExpr result) as a boxed value.
6. **Read phases** are worth keeping for `base | preceding | final | @prim` reads of a surface that usdRig deforms upstream (metadata on the relationship, `plugInfo.json:8-13` + `RigExecParseReadPhase`).
7. **Freeze/commit tooling:** RigExec has none, but `RigExecRigPose::movedProperties` + `RigExecSchemaPrim::SetAttribute` show the two halves: read a generation's value, author it as a `point3f[]` with the strict schema layer. The usdview loop to copy is `rigExecUsdview.py` (Tf notice → C API `Activate/SetTime` → snapshot store → scene index), i.e. app → data edit → C++ re-evaluate → atomic publish → `_SendPrimsDirtied`.

### 10.3 Costs to budget

- Per-generation packet materialization copies topology/cage arrays per revision (`RigExecAssembleParameters`); for hair with 100k+ CVs the packets must hold shared immutable buffers (spec §7.1 `RigExecSharedBuffer`, `docs/spec.md:1270-1283`, was planned but not implemented — `types.h` uses `std::vector`).
- The structure digest is a full subtree walk per `Evaluate` (`rigEvaluator.cpp:984-1230`); cache it and recompute only on `ObjectsChanged` resyncs.
- Intermediate reads cost a chain-prefix re-evaluation each (`rigEvaluator.h:375-382`); keep every operator output cached instead.

---

## Key facts

- Mover execution order is reverse-sibling post-order of the composed `<rig>/Movers` subtree, implemented as reversed `UsdPrimRange` — `<usdrig-src>/libs/rigExec/rigEvaluator.cpp:82-106`; README.md:139-160.
- Movers are discovered by having a `rigExec:moves` relationship; empty target lists are inert (notice, not error); grouping scopes are skipped — `rigEvaluator.cpp:1836-1885`.
- Joints/controls/solvers/volumes are discovered by type name under the rig; there are no membership lists — `rigEvaluator.cpp:842-962`; `schema.usda:95-115`.
- Side inputs are fixed relationship names per schema resolved at build time into exact property paths (`RigExecResolveRevisionBinding`) — `moverGraph.cpp:769-880`; `moverGraph.h:267-306`.
- Read phase is `rigExecReadPhase` string metadata on the input property (`base|preceding|final|<abs prim path>`), registered via `SdfMetadata` in the schema plugInfo — `moverGraph.h:63-111`; `moverGraph.cpp:557-660`; `plugin/rigExecSchema/resources/plugInfo.json:8-13`.
- Chain order across targets is a deterministic Kahn topological sort over phase/curvenet edges; cycles are compile errors — `rigEvaluator.cpp:3991-4068`.
- Solver→solver cycles are rejected by DFS at compile; solver→joint overrides iterate to a fixed point with bound `N+1` rounds and refuse to publish on non-convergence — `rigEvaluator.cpp:2965-3050, 5789-5900`.
- The point chain is a hand-built `VdfNetwork` of `_RevisionNode`s (`ReadWriteConnector<GfVec3f>(previous,out)`), sources are `VdfInputVector`, evaluated with `VdfScheduler::Schedule(..., topologicalSort=true)` + `VdfSimpleExecutor` — `moverGraph.cpp:56-80, 1385-1453`.
- That graph is rebuilt from scratch every generation per target; no cross-frame VDF cache — `rigEvaluator.cpp:6997-7009, 7230-7231`.
- Masks are `VdfMask::AllOnes` only; no sparse recompute is exploited — `moverGraph.cpp:1394, 1425`.
- Pass-through uses `SetOutputToReferenceInput(previous)`; in-place writes use `VdfReadWriteIterator(ctx, previous)`; `Allocate` on a READWRITE output silently degrades — `moverGraph.cpp:104-137, 401-410`; `docs/mover-graph-cutover.md:511-519`.
- Exec-side values go through `RigExecTapSet` = one `ExecUsdSystem` + one move-only `ExecUsdRequest`; `Evaluate` = `ChangeTime` + `Compute`/`ComputeWithOverrides` + immediate `VtValue` copy; callbacks only set an atomic dirty flag — `tapSet.cpp:17-168`.
- The evaluator owns four tap sets per rig (`_taps`, `_guideTaps`, `_solverFrameTaps`, `_restFrameTaps`) — `rigEvaluator.h:264-321`; `rigEvaluator.cpp:3273-3300`.
- Registered exec types: PointFrame, PointFrameArray, PointsPacket, WeightPacket, FalloffLut, BlendSampleData, BlendChannel, MoverParameters, MoverStatus; no VtArray — `types.cpp:96-107`.
- No `"Exec"` plugInfo block exists; computations register at static init because `rigExec` is linked into `rigExecImaging` (a Plug library plugin) and tools — grep result; `CMakeLists.txt:119-131`.
- Structural edits are detected by a string digest recomputed every `Evaluate`; any mismatch triggers a full rig recompile that tears down all ExecUsdSystems — `rigEvaluator.cpp:984-1230, 3060-3066, 5603-5610`; README.md:307-308.
- Animated scalar inputs (falloffMin/Max, strength, scales) are deliberately excluded from the digest; op-selecting uniform tokens are rejected if time-sampled — `rigEvaluator.cpp:1175-1183`; `docs/mover-graph-cutover.md:360-365`.
- Value-edit re-evaluation comes from `RigExecImagingRegistry::_OnObjectsChanged` (registered before Compile so it runs after exec's listener), which calls `SetTime(_lastTime)` for the whole rig — `libs/rigExecImaging/registry.cpp:225-266, 423-471`.
- Generations are immutable `RigExecRigPose` values; imaging publishes `shared_ptr<RigExecImagingSnapshot>` via `std::atomic_store` after a per-leaf diff that yields precise dirt or structural resync — `rigEvaluator.h:52-178`; `snapshotStore.h` (`Publish/_Diff`, dump).
- OpenExec 26.08 constraints routed around: no reverse-relationship accessor, no prim→attribute accessor, element-typed array overrides, no spline accessor — `<openusd-src>/pxr/exec/exec/computationBuilders.h:584-585, 840-841`; `<openusd-src>/pxr/exec/exec/system.cpp:100-175`; `docs/mover-graph-cutover.md:89-110`.
- The only SIMD kernel is SSE2 weighted-matrix (`RIGEXEC_ENABLE_SIMD` env, `RIGEXEC_DISABLE_SSE2` define); no TBB/Work usage in kernels — `libs/rigExecMath/simdKernels.cpp:1-80`; grep result.
- `RigExecSampleCurveRMF` implements arc-length resampling of a cubic uniform B-spline with double-reflection RMF frames; `RigExecApplyRibbonTransport` uses only `u` of the bind coords — `libs/rigExecMath/geometryKernels.cpp:412-586`.
- `RigExecRibbon` obtains driver points as `RigExecPointsPacket` overrides on stub computations because a relationship cannot reach an attribute — `moverKernels.cpp:1493-1539`; `rigEvaluator.cpp:5653-5673`.
- Weight packets: constant/dense/sparse with strict/clamp policy; volumes publish dense fields in local space; combine modes multiply/add/subtract/max/min/average/overlay — `types.h:67-114`; `moverKernels.cpp:151-233, 412-475`; `weightFields.h:167-219`.
- Profile Mover bind (cut mesh + Cholesky) is cached on the evaluator keyed by an input digest — `moverGraph.h:308-350`; `moverGraph.cpp:487-535, 1239-1255`.
- Python: pybind11 `_rigexec` (`Rig.compile/evaluate`, `Pose.moved_property`, `SchemaPrim`, typed handles, `Builder`) over `rigExecRigging`; strict authoring resolves properties from the composed prim definition — `python/_rigexec.cpp:565-1104`; `libs/rigExecRigging/schemaAuthoring.h:33-99`.
- The usdview loop is: `Tf.Notice(ObjectsChanged)` + `currentFrameChanged` → ctypes `RigExecImaging_Activate/SetTime` → C++ evaluate+publish → scene-index dirt — `plugin/rigExecUsdview/rigExecUsdview.py:63-69, 429-437, 600-610`.
- The usdview "graph editor" in usdRig is a spline editor, not a node graph — `docs/graph-editor.md`; `plugin/rigExecUsdview/graphModel.py`.
- CMake package exports `rigExec::rigExecMath`, `rigExec::rigExec`, `rigExec::rigExecRigging`, `rigExec::rigExecImaging` plus `rigExec_PLUGINPATHS/PYTHON_DIR/LIBRARY_DIR`; consumers must have the same pxr on `CMAKE_PREFIX_PATH` — `CMakeLists.txt:522-545`; `cmake/rigExecConfig.cmake.in`.

## Open questions

- Whether a non-`ExecSystem` client can instantiate `VdfDataManagerBasedExecutor<VdfParallelDataManagerVector, VdfParallelExecutorEngine>` (the parallel executor `exec/runtime.cpp:56, 261` composes) and reuse a `VdfSchedule` across generations with `InvalidateValues` for sparse recompute — headers exist (`vdf/executorInterface.h:189-203`) but no usdRig code exercises it; UNVERIFIED.
- Whether RigExec's `VdfNetwork` nodes could be kept alive across frames at all: `_RevisionNode` takes its parameter packet as a `VdfInputVector` constant, so a persistent network would need `SetValue` + executor invalidation; UNVERIFIED that `VdfInputVector::SetValue` after scheduling is supported without re-scheduling.
- `RigExecRibbon`'s `rigExec:startFrame/endFrame/twistFrames` and `rigExec:parameterization = parametric` are declared in the schema but the registered kernel reads only `sampleCount` and the two packets; UNVERIFIED whether any other code path consumes them.
- Linux build status: README says Linux is "Intended, but not yet verified" (`README.md:302`), and no `build/` directory exists in the checkout; the CMake package for `find_package(rigExec)` has not been produced on this machine.
- Whether `RigExecImagingRegistry::_OnObjectsChanged` runs on the authoring thread synchronously inside `UsdStage` change processing (implied by `registry.cpp:225-249`), and hence whether re-entrancy from a usdGen listener on the same stage could deadlock its non-recursive mutex — not tested.
- Per-frame cost of `_ComputeStructureDigest` on large rigs is unmeasured (no instrumentation found).
- Whether the closed `RigExecRevisionOp` enum / `final` node class can be extended without forking `moverGraph.cpp` — it cannot as written; UNVERIFIED whether maintainers intend a plugin ABI (spec §12.1 says "There is no public RigExec array or geometry kernel registration ABI", `docs/spec.md:1985`).
