# Operator hierarchy and expressions — schema discussion draft

This extends `operator-network.usda` following the user's requirement: every
non-string operator attribute can connect to SeExpr, and each parameter declares
whether it evaluates at groom, primitive or point granularity. Operators execute
bottom-up from the composed description hierarchy, following usdRig. SeExpr values
are evaluated during execution. Operators and expression evaluation use CUDA;
geometry remains GPU-resident through rendering and tool usage, as specified in
[gpu-execution.md](gpu-execution.md).
Animation deformation uses rest-bound RBF surface motion, with a separate
guide-motion transfer role described in [rbf-deformation.md](rbf-deformation.md).
These requirements are user decisions; the concrete schema spellings and context
details below are proposals. This is a proposed
authoring contract, not evidence of complete runtime support. Schema and runtime
implementation status is tracked separately in [overlay 14](../14-hierarchy-cuda-implementation.md).
It supersedes the example's map-only expression wiring and explicit input chain;
`02-schema.md` and overlay `13` describe the old baseline.

## Hierarchy determines execution dependencies

Follow `../usdRig/README.md` “Movers and execution order” and
`../usdRig/libs/rigExec/rigEvaluator.cpp::_GetMoverExecutionOrder`: children before
parents, lower siblings before upper siblings in the **final composed child order**.
This is reverse-sibling post-order, not sorting by prim name or creation timestamp.

```text
hair (UsdGenDescription)
  Ops
    deform                  6
    width                   5
    style (Scope)
      frizz                 4
      clump                 3
    interpolate             2
      scatter               1
  Expressions               parameter providers, not stack operations
  Guides                    reference geometry, not stack operations
```

The `Ops` scope is an organizational boundary beneath the description, matching
usdRig's `Movers` scope. Nested scopes group operators; nested operators execute
after all of their descendants. A scope creates no separate geometry stream.
The description owns the implicit working result, so no per-op target is needed
for this single-stream example. Scatter creates roots; Interpolate consumes the
preceding roots; subsequent stylers consume the preceding curve result. The
compiler derives those dependencies and logical value revisions in memory. No
`usdGen:input` chain or `usdGen:terminal` selector is authored. The description
publishes the last logical result, regardless of worker completion order.

The top `deform` operator is now explicitly `usdGen:mode = "rbf"`. It transforms
the preceding styled rest result using bound rest/animated surface samples.
`interpolate` constructs the rest groom from rest guides; it is not the separate
animation guide-motion transfer stage. Ordinary animation does not reconstruct
the rest groom or reselect guide neighbors. Time-dependent SeExpr upstream still
executes when its inputs change, and RBF must consume that new rest-space result.

Nesting does not mean independent branches or an implicit merge. Additional
geometry sources or multiple output streams need explicit type/target semantics
before they can be combined. That extension is outside this example.

Expression connections and guide/surface/map relationships add data dependencies;
they do not reorder the operator stack. Expressions inspect the value preceding
their consuming operator. A request for a later result that introduces a cycle is
a compilation error. Enable toggles preserve the operator's logical position;
disabled stylers pass through their preceding result. A missing required source
is an error rather than an invented empty stream.

Composition, reparenting, activation, load state, and child-order changes rebuild
the affected compiled ordering/dependencies. The runtime adapter must transport
composed child order explicitly if Hydra child enumeration cannot preserve it;
alphabetical discovery or sorting paths is not a substitute. Runtime graph
assembly remains sourced through adapters/Hydra, consistent with the architecture.

## Attribute connections and evaluation metadata

```usda
float usdGen:width = 0.03 (
    customData = { dictionary usdGen = { string evaluation = "point" } }
)
float usdGen:width.connect = </Character/Groom/hair/Expressions/strandWidth.outputs:result>
```

One connection selects one typed expression output. The literal remains the input
`$value` and supplies the ordinary parameter value when disconnected. The result
**replaces** the parameter value; multiplication happens only if the expression
asks for it. Existing operator ramps and masks then apply normally. Invalid
connections, expression errors, non-finite results and invalid parameter ranges
produce a diagnostic naming the consumer attribute and expression; they do not
silently fall back to the literal. Keep the last valid published generation.

Every numeric or boolean parameter is eligible, including inherited controls,
integers, vectors, colors, matrices and numeric arrays. This is generic attribute
support, not a separate `length:source`-style relationship for each parameter.
Strings stay literal. This draft also treats tokens and asset paths as string-like
configuration; relationships are graph/reference edges, not value attributes.

The USD output type must match the destination type. SeExpr numeric results are
converted with validation: booleans must be 0 or 1; integers must be integral and
representable without precision loss. Vector/matrix/array results require exact
component counts; matrices use row-major component order. An array is one typed
parameter payload per evaluation, not an implicit list of point samples. Array
shape is resolved before evaluating it, from the authored literal or an earlier
topology step. Dynamic array construction and exact 64-bit integer arithmetic
need explicit engine support; scalar-only expression plumbing is insufficient
to implement the full requirement.

### Current transport versus execution support

The Stage and Hydra graph-descriptor paths preserve declared shapes for USD
bool/numeric scalars, role aliases, vectors, quaternions, matrices, and fixed
arrays. An authored empty array has length zero; an array declaration without a
typed value is shape-unknown and is rejected. `uchar` deliberately remains
unsupported because the value ABI has no byte scalar type; it is not widened to
an integer type.

This is authoring/validation transport, not blanket CUDA execution support. The
CUDA compiler still admits only the parameter/type/domain combinations each
operator implements (for example, Length accepts primitive or groom `float2`
`usdGen:length:random`). Transport does not establish executable support for
matrices, fixed arrays, or exact 64-bit arithmetic; unsupported combinations
must produce diagnostics. Strings, tokens, asset paths, and other
configuration-like attributes remain literal as described above.

`customData.usdGen.evaluation` records spatial evaluation granularity. It is
independent of USD's temporal `uniform`/`varying` variability and of primvar
interpolation. The mock explicitly annotates every authored numeric operator
attribute, connected or otherwise. A finished schema must provide defaults for
every eligible inherited and concrete attribute, so unauthored parameters are
equally well-defined. Allowed granularities belong in the parameter contract:
CV count and algorithm version are groom controls; width can vary per point.
An unsupported granularity is a compile error, never a silent reduction.

## What each granularity means

For this draft, `groom` means the current Description's evaluation domain. The
containing `UsdGenGroom` may hold several independently evaluated descriptions.

| Granularity | Evaluation domain | Example |
|---|---|---|
| `groom` | Once for the current description/operator invocation | CV count, enabled, seed |
| `primitive` | Once per input strand or root record | Clump strength, guide influence radius |
| `point` | Once per input strand CV | Width, noise magnitude |

Before strands exist, a source may explicitly declare its domain as surface
faces/vertices or candidate roots. It must do so before variables are bound;
there is no imaginary output strand to inspect. This example keeps Scatter's
parameters groom-level. Generator `cvCount` is evaluated before CV allocation.
Groom constants broadcast to primitive/point consumers; primitive values broadcast
to their own points. There is no implicit point-to-primitive or primitive-to-groom
reduction. The expression program itself is reusable; its consumer owns the rate
and input context, so sharing a prim does not imply sharing computed results.

## Shared SeExpr context

Use one variable registry, with a core available at all three granularities:

| Variable | Meaning at every granularity |
|---|---|
| `$value` | The consuming attribute's unconnected literal/default at the requested time; never its own expression result |
| `$frame` | Current USD time-code value |
| `$time` | Seconds: `$frame / timeCodesPerSecond` |
| `$index`, `$count` | Current element index and domain size: `0,1` at groom level; strand index/count at primitive level; flattened CV index/count at point level |
| `$seed` | The operator's authored seed before expression override; when driving seed itself, this remains its literal to avoid recursion |
| `$descId` | A deterministic, double-exact 32-bit hash of the description path; renaming changes it |

Geometry variables extend that same registry. At point granularity, the point
inherits its primitive's root context; names do not change between operators.

| Variables | Primitive | Point |
|---|---|---|
| `$primIndex`, `$primCount` | Strand index/count | Owning strand index/count |
| `$idLo`, `$idHi` | Exact 32-bit halves of stable curve ID | Same owning curve ID |
| `$id` | Legacy `double(curveId)` convenience, not exact above 2^53 | Same owning curve ID |
| `$u`, `$v`, `$faceId` | Root UV and parent face index | Same root binding |
| `$P`, `$Pref` | Root position in current/rest input geometry | CV position in current/rest input geometry |
| `$rootP`, `$rootPref` | Current/rest root position | Same root positions |
| `$N`, `$Nref`, `$dPdu`, `$dPdv`, `$dPduref`, `$dPdvref` | The strand's **rest** root frame: normal, tangent, bitangent. usdGen keeps one root frame, so the `ref` spellings are the same vectors | Same root frame |
| `$t` | 0 at the root | Normalized arc length, root 0 to tip 1 |
| `$pointIndex`, `$pointCount` | Unavailable | CV index within strand and that strand's CV count |
| `$cLength`, `$cWidth` | Incoming strand length and root width | Incoming strand length and current CV width |

All geometry reads are from the consuming operator's **input**, never its output.
Current/rest positions follow the operator's resolved space and available input
snapshot; an expression on a rest-space node cannot secretly read a future Deform
result. The new `$P`/`$Pref` definition is intentionally explicit and replaces the
old map-context assumption for these general parameter expressions.

Geometry variables are unavailable at groom granularity, and curve variables are
unavailable before curve creation. The compiler checks actual referenced variables
against the consumer's domain and available channels. It reports unavailable
variables instead of binding them to zero. Thus the core vocabulary is common
everywhere, while geometric data keeps its meaning. Geometry-wide reductions, if
needed, must be explicit upstream computations.

`$patchId`, `$Cs` and `$As` were listed here through M1 and are gone: usdGen has
no patch table, and the engine's curve buffer carries no surface colour or
opacity at evaluation time, so nothing could ever fill them. They are removed
rather than left declared-but-unwritten, which is what made an expression naming
one fail with a poison value instead of a message. `expr::Frontend::VariableDocs()`
is the single source of truth for this table.

## Compilation and evaluation timing

**Compile programs; evaluate values during execution**, as clarified by the user.
Compilation parses and type-checks source, binds the common variable registry,
and builds dependency/program state. It does not bake per-strand/per-point values
into the graph. When an operator executes, it binds its current input context and
evaluates its parameter expressions at the declared granularity. Point values
cannot be computed until an upstream generator has produced points. Frame/input
changes refresh dependent results without reparsing unchanged source.

Graph compilation establishes dependency order and prepares programs. During
execution, groom-level topology parameters resolve before allocation;
primitive/point parameters resolve after their input domain exists. A changed
topology result rebuilds the affected allocation and dependent evaluation state;
it does not imply a change to authored operator order. Expression parsing and
device-code compilation run on the CPU; parameter expressions execute on CUDA
at groom, primitive or point granularity. Their results remain in device buffers
or kernel registers and are not authored back into USD. Supporting this requires
a SeExpr device compiler and device implementations of the exposed functions;
the existing host interpreter/noise implementation is not that backend.
Results may be cached only when all referenced inputs/context are unchanged;
caching preserves execution-time semantics and cannot freeze a time-dependent
or preceding-geometry-dependent expression at compile/capture time.

## Required implementation changes after design review

The current adapter transports parameter values and relationship targets. It does
not read attribute connections or this metadata, and `UsdGenExpression` is a new
proposed type. USD parsing alone therefore cannot validate execution. In particular,
`UsdAttribute.Get()` does not evaluate these connections.

Implementation needs a schema/type registration for expressions, typed output
discovery, and adapter transport for source code, connection property paths,
literal values and evaluation metadata. Metadata and connection edits need
explicit invalidation; leaving `customData` on the USD side is insufficient.
The graph compiler must derive geometry dependencies from hierarchy, discover
parameter/reference dependencies, reject cycles and incompatible types/rates, and specialize shared
programs per consuming context. Cache keys include the consumer, rate, literal,
source/type and referenced context dependencies. Kernels must accept parameter
fields at their declared rates, including topology and boolean controls.
Those fields, geometry inputs/outputs, masks, and tool state must use the shared
device-buffer contract. Expression code can be fused into an operator kernel
where doing so preserves evaluation order, types and variable semantics.

This revises `02`'s explicit-input ordering rule, overlay `13`'s closed
relationship-only edge inventory, and plan `07`'s map-specific root/CV,
capture-only expression model. Hierarchy determines geometry revisions;
`.connect` adds parameter dependencies. The new `Expressions/` scope also extends
the reserved layout. Existing map prims remain useful for sampled fields, but
they are no longer the only entry point for SeExpr parameters. These revisions
are contained in this discussion draft until the production contracts are updated.
The previous CPU/TBB execution, CPU expression capture and CPU array publication
assumptions are additionally superseded by [gpu-execution.md](gpu-execution.md).
