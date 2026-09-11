# CUDA execution and persistent GPU geometry — discussion contract

User requirement: operators use CUDA, and hair geometry remains on GPU buffers
through evaluation, rendering and tool usage. This applies to the whole active
pipeline, including SeExpr parameter evaluation. The requirement is accepted for
this evolving design; the mechanisms below are proposals, not implemented support.
Animation transport uses the RBF contract in [rbf-deformation.md](rbf-deformation.md):
rest sample bindings, solve state, current sample positions and curve evaluation
remain on the GPU, including per-frame and shutter-sample deformation.

## Authoring and runtime boundaries

The mock records one requirement on the groom, inherited by descriptions and ops:

```usda
custom uniform token usdGen:execution:backend = "cuda"
```

This is a new proposed property. `cuda` requires compatible kernels and renderer
integration; it does not silently fall back to CPU geometry evaluation. There is
no per-op backend switch, authored GPU ordinal, CUDA pointer, allocation size or
buffer handle. Device selection, streams and storage belong to session state.
Prim hierarchy, parameter values/connections and point/primitive/groom metadata
retain their authoring meanings independently of kernel launch layout.

USD and CPU code compose the asset, compile dependencies and expressions, submit
commands and manage resource lifetimes. Authored scalp/guide arrays are uploaded
when first loaded or explicitly changed. Thereafter the working geometry and
derived fields stay device-resident across evaluation, interaction and drawing.
A CPU-only upstream deformer still needs an upload at its input boundary; the
design must report that cost. Fully device-resident upstream animation requires
a compatible shared-buffer producer as well. Neither case permits downloading
generated hair between grooming operators or before rendering.

```text
USD hierarchy, parameters, SeExpr source       brush/camera commands
                 |                                  |
         CPU graph/program compilation              |
                 |                                  v
GPU: source buffers -> CUDA operators <-> CUDA picking/selection/brushes
                              |
                     completed buffer generation
                              |
                    graphics buffer interop
                              |
                     GPU renderer consumes it
```

## Device geometry and scheduling

One runtime geometry view describes device allocations for CV positions/rest
positions, widths, strand offsets/counts, stable 64-bit IDs, root bindings,
reference guides, masks, generated primvars and selection/sculpt state. Arrays
are device arrays; a CPU descriptor carries only handles, types, sizes, strides,
generation IDs and synchronization information. Fields can remain SoA for
compute. Any interleave, tessellation, index generation, compaction or conversion
needed by the renderer must also run on the GPU.

The hierarchy compiler derives logical preceding-result dependencies. Each
operator reads a prior device view and produces a new logical revision. Unchanged
channels share allocations. Modified channels use an output allocation or an
in-place update only when no renderer, tool, undo checkpoint or concurrent kernel
can still read the previous version. Avoid cloning the entire groom per operator.
Logical hierarchy order remains authoritative even when independent work runs
concurrently or kernels are fused.

Topology counts, prefix sums, live-curve lists and bounds are computed on CUDA.
Allocate from a device pool with capacity separate from logical length; renderer
draw counts must honor logical length, never expose padding as extra curves.
Bounded scalar allocation requests or aggregate bounds may reach the CPU when
required by a host API. Point-, strand- or topology-sized readback is excluded
from the interactive/render path. Memory pressure must trigger controlled
eviction/recomputation or a reported allocation failure, not hidden host spilling.

A published generation owns strong references to its allocations and a producer
completion event. Tools/renderers wait on the appropriate device synchronization
before reading. Renderer completion fences protect allocations from reuse.
Mapping/unmapping or external semaphores also mediate ownership between CUDA and
the graphics API. CPU publication of a handle alone does not establish GPU memory
visibility. Keep the last completed generation while a new one is being produced;
never mutate buffers still in use by the displayed frame.

## SeExpr executes with the operator on CUDA

Parse and type-check SeExpr on the host, lower its typed expression representation
to device code, and evaluate it during operator execution. One groom invocation,
one invocation per primitive or one per point implements the same context contract
in [operator-expressions.md](operator-expressions.md). Scalar controls may be
uploaded; geometry-dependent variables are read directly from device views.
Constants broadcast and compatible expressions may be fused into kernel code.

NVRTC is a candidate for compiling **generated CUDA C++** to device code; it does
not compile SeExpr source directly. A SeExpr frontend/lowering layer and GPU
implementations of every exposed builtin, map lookup and noise function are
required. NVIDIA documents NVRTC's CUDA C++ input and PTX/cubin output in the
[NVRTC documentation](https://docs.nvidia.com/cuda/nvrtc/index.html).

The public expression function set must have defined GPU behavior. An unsupported
function fails compilation with the expression/attribute path; it must not cause
a geometry readback to run a host interpreter. Full requested function support
remains implementation work, not an excuse to declare the GPU backend complete
with a smaller undocumented language. Validate integer conversion, vector/array
shape, random seeding, noise behavior and numerical tolerances against a host
reference evaluator. Stable integer curve IDs remain exact in device storage.

## Rendering consumes the device result

Hydra still describes prim identity, material binding, visibility and dirty state.
The geometry payload needs a renderer integration that accepts shared GPU
allocations. Passing a CUDA address as if it were a host `VtArray` is invalid.
Publishing ordinary CPU curve arrays and re-uploading them does not meet this
requirement even if the operator preceding publication was a CUDA kernel.

For Storm, prototype a buffer integration with its actual graphics backend and
resource registry. OpenGL resources can be registered/mapped for CUDA access;
external-memory/semaphore interop is another API-dependent route. Shared resource
identity, device matching, ownership transfer and fences must be verified together.
NVIDIA describes these mechanisms in its
[graphics interoperability guide](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html).
These facilities enable a proposed bridge; they do not establish that stock Storm
already accepts usdGen's CUDA geometry. A GPU-to-GPU layout copy is acceptable;
a round trip through host geometry is not. Direct resource reuse is preferred.

Check topology, widths, primvars, culling, picking and motion samples as well as
positions. Keeping just positions on-device is insufficient. Render-time
generation follows the same contract. Each supported delegate, including the
plan's hdPrman target, requires a verified GPU resource ingestion route. Existing
CPU publication compatibility does not establish this capability; without such
a route that delegate is unsupported in this mode. Do not silently weaken the
residency requirement to claim compatibility.

## Tools operate on the same device generations

The UI sends compact commands: cursor/ray, brush radius/strength, camera data,
selection mode, target operator and generation ID. CUDA performs point/strand
picking, hit filtering, neighborhood searches, falloff, combing, smoothing and
sculpt edits. Visibility-aware picks use the matching displayed frame's depth or
ID resources through GPU interop. Selection sets and spatial acceleration data
remain device-resident. Reject or remap stale-generation picks before applying
edits to a new topology; stable IDs identify strands across buffer relocation.

Return only small hit records, status and aggregate counts to the UI. Do not
download all CVs, projection arrays or selected geometry to NumPy for each move.
Brush edits enter the logical stack at an explicit tool edit location, preserving
the consuming operator's preceding-value semantics and downstream invalidation.
CUDA updates affected buffers, then rendering uses the resulting device generation.

Undo/redo keeps bounded device delta/checkpoint buffers and a CPU command history;
replay uses deterministic seeds and versioned operator/expression semantics.
Mouse-up commits the edit transaction without automatically downloading geometry.
Durable authoring needs a replayable stroke/sculpt command representation with
stable targets and versions; that serialization contract is still to be designed.
Do not leave edits only in GPU memory or pretend the old CPU sculpt-array commit
already satisfies this requirement.

Explicit geometry bake/export or a requested checkpoint serialization may read
back the required data. Those are named persistence boundaries outside the live
tool/render loop. Ordinary asset saves can serialize authored controls and the
replayable edit history without copying live hair geometry. No implicit readback
on a brush move, release, undo, redraw or renderer synchronization.

## Current gaps and acceptance evidence

The current `libs/usdGen/usdGen/ops/noise_gpu.cu` uses host-to-device input copies
and device-to-host output copies (`cudaMemcpyAsync` in its capture function).
That is a CUDA-assisted CPU pipeline, not this persistent device pipeline.
The inspected OpenUSD `pxr/imaging/hdSt/basisCurves.cpp` consumes `VtValue` primvars
and builds `HdVtBufferSource` objects for its normal upload path. A renderer bridge,
device geometry owner and SeExpr GPU compiler remain required.

Before declaring the design implemented, demonstrate a complete hierarchy cook,
expression edit, time/deform update, brush drag/release, undo/redo and rendered
frame with traced transfer counts. After initial source uploads, require zero
bulk geometry/topology/field device-to-host transfers and no generated-hair host
re-upload. Count GPU-to-GPU conversions and bounded metadata separately. Check
CUDA allocation lifetime, interop fences, rapid edits while frames are in flight,
topology growth/shrink, deterministic identity, picking against the displayed
generation and numerical/image correctness. Measure GPU time and peak memory;
the old CPU benchmark results do not predict these costs.

This supersedes plan `01`'s rejection of a GPU evaluator, `03`'s host-buffer/TBB
execution contract, `06`'s CPU curve publication, `07`'s host SeExpr capture and
`08`'s NumPy brush/CPU picking and release-time array authoring assumptions for
this design. Existing production files are unchanged while the schema discussion
continues. Their old constraints cannot override these user requirements.
