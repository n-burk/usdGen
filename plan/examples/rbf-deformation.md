# RBF animation deformation — research and proposed contract

The user requires RBF-driven animation deformation, referencing Unreal groom
animation and HiPhyEngine's Curves Motion Mapper. This extends the hierarchy,
execution-time SeExpr and persistent CUDA geometry requirements in the companion
drafts. `UsdGenDeform` in the mock now authors `usdGen:mode = "rbf"`; RBF is a
required animation path, replacing the old plan's rigidFrame-first/v2-RBF split.
No RBF kernel or renderer integration was implemented in this design change.

## What the references establish

**Unreal:** global groom interpolation uses rest and animated samples on a skinned
mesh to compute an RBF deformation. It is distinct from the guide-to-strand
interpolation controls, and can be selected per LOD. The documentation describes
its purpose as preserving groom shape through substantial skin deformation; it
does not specify the exact RBF kernel, regularization or linear solver.
[Epic: Groom Interpolation](https://dev.epicgames.com/documentation/en-us/unreal-engine/groom-interpolation-in-unreal-engine).

Epic exposes the binding sample count and suggests about 100 or fewer samples
as a general starting point, with increased samples trading cost for accuracy.
This is guidance, not a mathematical limit or a performance measurement for
usdGen. Source-to-target groom transfer based on UV correspondence is a separate
binding operation, not permission to rematch a changing mesh every frame.
[Epic: Setting Up Bindings](https://dev.epicgames.com/documentation/en-us/unreal-engine/setting-up-bindings-for-grooms-in-unreal-engine).

Epic's public root-data API lists rest sample positions, mesh sample indices and
an interpolation-weight matrix with sample-count-squared storage. That supports
separating persistent binding data from changing animated samples; it does not
establish any particular per-hair weight layout in our implementation.
[Epic: FHairStrandsRootData](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/HairStrandsCore/FHairStrandsRootData).

**HiPhy:** the supplied video is “3. Use Motion Mapper to Transfer Motion” by
HiPhyEngine. Its description links the supplied Motion Mapper documentation.
YouTube metadata was accessible, but caption retrieval returned an empty response;
the precise demonstration at 9:27 was not independently reviewed. The behavior
below is grounded in the author's documentation, not inferred from the timestamp.
[Video at 9:27](https://www.youtube.com/watch?v=dvKzz7LUSgg&t=567s).

HiPhy's mapper binds driven curves to guides and transfers their motion. Its
controls include binding radius, maximum guide count, optional curve frames and
ID-based grouping. Its IDs express group membership and need not be unique;
they are different from usdGen's unique strand identities. Dynamic parting can
adjust bundling when guides separate. The documentation motivates binding as a
way to avoid chatter from reconstructing hair using moving guides. It does **not**
state that its interpolation is RBF. We should adopt the stable-binding and
frame-aware motion-transfer concepts without attributing an undocumented algorithm.
[HiPhy: Curves Motion Mapper](https://hiphyengine.github.io/aux-nodes/curves-motion-mapper-in-blender/).

## Two motion roles

| Role | Drivers | Result |
|---|---|---|
| Surface animation | Rest/animated skin samples | RBF deformation of the styled rest groom and, when needed, its guide baseline |
| Guide motion | Bound reference/current guide positions and frames | Transfer additional rigged/simulated guide motion onto the dense groom |

The first is the required RBF path in the mock. The second needs its own motion
operator contract; the existing `UsdGenGuideInterpolate` is the rest-shape
generator and must not quietly become a per-frame rebinding system. A simulation
solver is optional and separate from both: RBF alone supplies no dynamics,
collision response, inextensibility or guaranteed volume preservation.

The initial RBF implementation should evaluate the final styled rest CVs directly.
This preserves the input groom's detailed shape at rest without regenerating
clumps, frizz or strand identities. A guide-only RBF path followed by dense motion
transfer is a later optimization to compare for quality and cost, not assumed
equivalent to evaluating the field on every CV.

If both motion roles are used, define their baselines explicitly: RBF produces
`hairBase = F(hairRest)` and `guideBase = F(guideRest)`. Transfer guide motion from
`guideBase` to the rigged/simulated guides onto `hairBase`. Equal actual/baseline
guides must produce zero extra motion. Applying the complete rest-to-animated
guide transform after RBF would apply surface motion twice. Neither stage changes
the hierarchy's rule that a consumer reads its preceding logical result.

## Proposed RBF field, not a reconstruction of Unreal's implementation

Bind a stable set of rest-space surface samples `s[i]`. Store correspondence to
the driver surface, so execution samples the same material locations `q[i,t]`
under animation. Do not repeat nearest-neighbor selection from posed positions.
Use a normalized binding coordinate system with explicit conversions for rest
and current inputs; avoid silently mixing object/world spaces or applying the
character transform twice.

A candidate is a cubic RBF plus an affine polynomial, solved for displacement:

```text
d[i,t] = q[i,t] - s[i]
K[i,j] = phi(length(s[i] - s[j])),  phi(r) = r^3
P[i]   = [1, s[i].x, s[i].y, s[i].z]

[ K + lambda*I   P ] [ a(t) ] = [ d(t) ]
[ P^T           0 ] [ b(t) ]   [  0   ]

F_t(x) = x + sum_i a_i(t)*phi(length(x-s[i])) + [1,x,y,z]*b(t)
```

This is a standard polynomial-augmented RBF formulation; cubic kernels require
at least a linear polynomial, distinct samples at zero smoothing and full-rank
polynomial support. In 3D, coplanar centers fail that affine rank condition.
[SciPy: RBFInterpolator mathematical notes](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.RBFInterpolator.html).
SciPy is a reference for the equations, not a proposed host implementation.

Our proposed acceptance rules are rest identity, rigid-motion reproduction,
stable sample correspondence and bounded numerical error. For a full-rank system,
affine motion can be represented by the polynomial alone, including rotations.
RBF smoothing is not a remedy for rank deficiency in `P`. Deduplicate samples,
check conditioning and residuals, and report invalid bindings. Flat surface
patches need an explicitly designed frame/normal-augmented construction or a
rank-aware formulation before being supported; never claim an arbitrary epsilon
fixes an underdetermined off-surface rotation.

Keep exact root attachment separate from field approximation. At full envelope,
for a locked strand with rest root `r`, evaluate its bound animated triangle root
`rTarget`; use `delta = rTarget - F_t(r)` and `candidateCV = F_t(restCV) + delta`.
This proposed whole-strand correction pins the root without introducing a one-CV
kink. Then apply ordinary operator blend/mask semantics; a zero envelope must
still pass through exactly. Revalidate root behavior with partial masks and
guide-motion transfer. Length preservation or volume correction must be explicit
later operations; an RBF warp does not guarantee either.

## Bind once, execute on CUDA

At bind/rebind, CUDA chooses deterministic surface samples, records persistent
correspondences and builds/factorizes the normalized small RBF system. Reuse that
factorization for the three coordinate right-hand sides each frame. Use a solver
appropriate to the augmented system; it is not a positive-definite matrix suitable
for an unqualified Cholesky factorization. Bind state, factors, coefficients,
surface/guide positions, hair CVs and root corrections remain in GPU allocations.

During execution: update animated samples, solve coefficients on CUDA, evaluate
the field and root corrections on CUDA, update dependent bounds/render buffers,
then publish a completed device generation. Evaluate every motion-blur time from
its corresponding surface/guide pose. Time scrubbing must not depend on the
previous frame's RBF result. SeExpr parameters evaluate at execution time too.

For `N` samples and `C` CVs, a reused dense factorization gives roughly `O(N^2)`
solve work per coordinate and direct field evaluation costs `O(C*N)`. Do not
assume an `N`-weight table per CV is free: 800,000 CVs * 100 float32 weights alone
would consume 320,000,000 bytes (about 305 MiB), before geometry or caches.
Start by comparing direct tiled evaluation against cached weights; retain the
existing GPU residency requirement in either case. No timing is established here.

Surface animation changes current sample values, not rest binding. New rest skin,
topology/correspondence changes, sample-count changes or solver-policy changes
invalidate the relevant binding. Rest-groom edits invalidate CV-dependent data
but need not invalidate the skin system. Changing an expression-driven sample
count requests a controlled rebind before the new generation executes, not
untracked per-frame neighbor changes. Cache identity includes rest-data digest,
sample selection, solver/version and domain transforms. Durable bind caches may
be regenerated from authored rest inputs; CUDA handles never appear in USD.

For guide-motion transfer, bind IDs/arc-length coordinates/weights and reference
frames once, and use the animated versions of those same guides. Evaluate in
guide space with explicit scale handling. Keep rest neighbor membership stable;
parting may smoothly adjust weights within the bound candidates when explicitly
enabled. Frame-aware rotation/twist transfer needs continuous frames and a
documented rotation blend, not position-only deltas. Binding diagnostics must
identify unbound strands instead of leaving them unpredictably motionless.

## Mock schema and validation scope

`UsdGenDeform` already declares `usdGen:mode`, `usdGen:rbfSamples`, surface binding
and `usdGen:lockRoots`. The mock uses these names with `mode = "rbf"`, a five-sample
toy surface and a groom-level sample count. Numeric controls remain eligible for
SeExpr; counts are structural binding controls, while root locking is per strand.
No new exposed kernel/solver knobs are necessary until we settle their semantics.

The scalp now has five non-coplanar vertices and two animated poses. Its first
triangle retains the two existing guide roots. This avoids the earlier single
triangle's affine rank deficiency and is only a small correctness fixture, not
a production sample distribution. For this fixture all five vertices can serve
as the samples; production sampling must be spatially representative.

Required runtime checks include rest identity, translation/rotation, bend/twist,
animated roots, nonuniform scale, close/opposing surface regions, degenerate
sample layouts, strand/guide ID stability, curls under guide twist, changing
parting, frame scrubbing, motion samples, tool edits and zero geometry readbacks.
Compare to a CPU mathematical oracle for validation only. The current
`libs/usdGen/usdGen/ops/deform.cpp` does not implement an RBF solve; setting the
authored token alone cannot deliver this behavior. Existing plan kernel-status
claims must not be used as proof of the new path.
