# usdGen — architecture proposal (artist-workflow lens)

Date: 2026-09-04. Status: **proposal**, written against `design/brief-v1.md`. Every settled
decision of that brief (S1–S46) is a constraint here and is cited, never re-argued. This document
answers the eight open areas D1–D8 concretely enough to start implementing: exact prim type and
property names with types and defaults, `.usda` for the three canonical workflows, C++ class and
function signatures, library decomposition with CMake target names, the operator catalogue with
phase assignment, the look pipeline, the tool set, a phased roadmap with exit criteria, and risks.

Status vocabulary (owner's `docs/spec.md` convention): **EXISTS** = code is already in the
scratchpad; **MEASURED** = a number was measured on this host; **UNMEASURED** = needs a
workstation/GPU protocol; **PLANNED** = this proposal. Anything not backed by the brief or a
report is marked **ASSUMPTION**.

---

## 0. Request and lens

The request (brief §1, R1–R9) is an procedural hair/fur grooming *and instancing* plugin for
OpenUSD 26.08 / Hydra 2.0, on top of usdRig, with dynamic in-memory evaluation, render-time curve
generation in one binary, fast static-curve load and deformation, chainable stylers, Storm looks
close to the render, image/Ptex/SeExpr-driven colour and attributes, hard interactivity, and
usdview tools that freeze an operator and let an artist groom by hand.

**The lens this proposal optimises for is the grooming artist.** Concretely that means five
things, and every design choice below is traceable to one of them:

* **A1 — The stage is the UI.** What an artist sees in usdview's prim tree and in a `.usda` diff
  must read like a groom: a description, a stack of named operators in the order they run, a
  guide set, maps, a material. No opaque blobs, no `primvars:` smuggling of parameters.
* **A2 — Vocabulary parity.** An artist arriving from a host groomer, a DCC or a host renderer must find the
  parameter they already know, under a name they can guess, with the same units and the same
  mask/ramp block on *every* operator (A7 §7 recommends exactly this).
* **A3 — The loop, not the frame.** A brush stroke, a slider drag and a scrub must all cost the
  same small number of milliseconds. The architecture is built around "what happens 60 times a
  second", not around "what happens at render".
* **A4 — Freeze is a first-class verb.** Grooming is iterative: generate, freeze, comb, restyle
  on top. Frozen data must be an ordinary input, not a special mode (S42 makes this true).
* **A5 — A TD can add an operator in an afternoon.** Two extension tiers: SeExpr for the TD who
  is not a C++ programmer, a 3-method C++ interface plus one registry macro for the one who is.

---

## 1. Assumptions (decisions taken without a live user)

1. **ASSUMPTION** — Grooms are authored per *asset*, in the asset's layer stack, and shot-level
   work is limited to overrides, freezes and sim caches. This justifies putting the operator
   stack in namespace under the character rather than in a separate "groom file" that references
   the scalp.
2. **ASSUMPTION** — The primary artist application is usdview plus the usdGen plugin. A DCC
   bridge between host applications is out of scope for v1–v3; the C ABI and pxr_boost module (S39) are the
   integration surface if one is written later.
3. **ASSUMPTION** — Units are the stage's `metersPerUnit`; `density` is *hairs per square stage
   unit* on the **rest** surface, which is the only definition stable under deformation. A host groomer
   quotes density per unit²; A7 §9.1 keeps the same.
4. **ASSUMPTION** — v1 targets ≤ 1 M rendered curves and ≤ 200 k interactive curves per
   description. 200 k × 8 CV at refineLevel 2 is 23.9 ms/frame on this GB10 (S31, MEASURED), so
   the interactive LOD ladder (§3.6) is mandatory above ~150 k.
5. **ASSUMPTION** — The `UsdGenMask` abstract prim base named in S9's parenthetical is realised
   here as (a) an applied API schema `UsdGenMaskAPI` — the per-operator mask *slot* — plus (b)
   the abstract prim base `UsdGenMap` for the field that feeds it. Rationale from the lens: an
   artist authors a mask as a block on the operator (a host groomer and other host applications all do), and the only
   thing that deserves its own prim is the *field* (an image, a Ptex, an expression, a paint
   layer). S9's normative content — codeless schemas, a common abstract base — is honoured.
6. **ASSUMPTION** — Operator prims are non-imageable (`UsdTyped` base). They must still appear in
   usdview's prim browser, which lists all prims regardless of type, and they must not acquire
   `visibility`/`purpose`, whose meaning would be ambiguous against `usdGen:enabled`.
7. **ASSUMPTION** — The schema ships one prim type per *operator concept*, not per *mode*
   (`UsdGenScatter` with `usdGen:mode = random|uniform|points|atGuides`, not four types). Fewer
   types read better in the prim tree and match a host groomer's single "generator" with a mode; the cost
   is unused properties, which usdview hides behind "show only authored".
8. **ASSUMPTION** — Chain edges are always authored by the tool as `usdGen:input` (S26). The
   schema *additionally* defines an implicit fallback — an operator with no authored
   `usdGen:input` inside a Description's stack scope takes its preceding sibling — so hand-written
   `.usda` stays terse and `reorder nameChildren` reorders a stack. The resolved edge, however
   derived, is what enters the structural digest (S26).
9. **ASSUMPTION** — Colour ramps are authored as knot arrays (S11) in v1; the `TsSpline`-as-data-
   source form (S11, MEASURED in `G-stage-free` probe6) is added in v2 so usdRig's existing spline
   graph editor (A3 §0) can edit a clump profile with no new UI.

---

## 2. From the artist's mental model to the object model

An a host groomer artist thinks: *description → a generator → a stack of modifiers → guides → maps →
a look*. A a DCC artist thinks: *a chain of SOPs with masks*. Both map onto the same five
nouns, and this proposal names them exactly once:

| Artist noun | usdGen prim | Hydra consequence |
|---|---|---|
| "my character's hair, all of it" | `UsdGenGroom` | nothing; a grouping + defaults scope |
| "the eyebrow description" | `UsdGenDescription` | 32–256 `basisCurves` chunk prims + guides + optional instancer (S27, S33) |
| "the clump modifier" | `UsdGenClump` (a `UsdGenOperator`) | one node in the TBB DAG (S21) |
| "the density map I painted" | `UsdGenPaintMap` (a `UsdGenMap`) | CPU sample at capture, baked to a per-curve array (S37) |
| "my guides" | `UsdGenGuideSet` + child `BasisCurves` | a `guide`-purpose curves prim + a CV `points` prim (S40) |
| "the frozen groom" | `BasisCurves` + the S42 primvar contract | a source node in the DAG (S42, `G-freeze-bake` §2.2) |

Two invariants make the tree readable:

* **The stack scope is the stack.** Every Description owns a `Scope "Ops"` whose children, in
  namespace order, are the operator chain. usdview shows them top-to-bottom in that order; the
  stack editor reorders with `reorder nameChildren`, which is one undoable metadata edit and no
  prim churn (important: prim add/remove is a resync, S46/A3 §8, and `RemovePrim` under an
  OpenExec system raises, S41).
* **Everything the evaluator generates lives under one reserved scope it owns**,
  `<Description>/__usdGenRender`, which exists only in Hydra and is never authored. That mirrors
  usdRig's `<rig>/__RigExecGenerated` ownership rule (A2 §5) and keeps the USD tree free of
  machine-generated prims — an artist never sees `curves_037` in the browser, only in the Hydra
  browser when debugging.

---

## 3. D1 — Schema

### 3.1 Naming rules

| Rule | Value |
|---|---|
| Schema family / prefix | `UsdGen…`, codeless (`skipCodeGeneration = true`), usdRig's gen_schema tooling (S9) |
| Property namespace | `usdGen:` |
| Grouped parameters | second level: `usdGen:clump:amount`, `usdGen:clump:ramp:knots`. `UsdImagingDataSourceMapped` turns these into nested locators `usdGen/clump/amount`, `usdGen/clump/ramp/knots` (MEASURED, `G-stage-free` §2), which is exactly the granularity the dirty pipeline wants (§4.5) |
| Relationships | `usdGen:input`, `usdGen:surface`, `usdGen:curves`, `usdGen:guides`, `usdGen:prototypes`, `usdGen:mask:source`, `usdGen:clump:centers` |
| Reserved Hydra scope | `<Description>/__usdGenRender` |
| Reserved namespace children of a Description | `Ops`, `Guides`, `Maps`, `Prototypes`, `Frozen` (each a `Scope`) |
| Version stamp | `uniform int usdGen:schemaVersion = 1` on `UsdGenGroom`; readers refuse a higher major with a `TF_WARN` and publish nothing rather than mis-evaluating |

Every `usdGen:*` property reaches Hydra through the prim adapter of §5.6 (S10). `primvars:usdGen:*`
is used **only** for the frozen-curve contract (S42), where it must be a primvar to be visible
and to dirty precisely (MEASURED, `G-freeze-bake` §2.3).

### 3.2 Containers

**`UsdGenGroom`** (: `UsdGeomImageable`) — one per character; the place an artist sets studio-wide
defaults and the global density switch.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:surface` | `rel` | — | default bound surface for all descriptions (mesh, or `GeomSubset`s) |
| `usdGen:densityScale` | `float` | 1.0 | viewport multiplier on every description's density (a host groomer "preview density") |
| `usdGen:renderDensityScale` | `float` | 1.0 | multiplier applied only when the delegate is not Storm (a host groomer "Render Density Multiplier", A7 §8) |
| `usdGen:schemaVersion` | `uniform int` | 1 | |
| `usdGen:sessionId` | `uniform string` | "" | stable key for the stage-free registry path (S15, `G-stage-free` §7) |

**`UsdGenDescription`** (: `UsdGeomBoundable`) — the artist's "object". Boundable, not just
Imageable, so a `UsdGeomComputeExtentFunction` can be registered for it and *frame-selected* frames
the hair (the usdRig precedent, A2 §6: `UsdGeomBBoxCache` ignores Hydra entirely).

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:surface` | `rel` | inherits Groom | mesh / `GeomSubset` targets. Instance-proxy targets are translated by S7's `ProxyPathTranslationDataSourceNames` |
| `usdGen:output` | `rel` | last op in `Ops` | terminal operator; the thing that is drawn |
| `usdGen:primitive` | `uniform token` | `"splines"` | `splines`, `cards`, `archives`, `spheres` (a host groomer primitive types, A7 §1.1) |
| `usdGen:chunkCount` | `uniform int` | 64 | S27's 32–256; clamped |
| `usdGen:chunkPolicy` | `uniform token` | `"surfaceLocality"` | `surfaceLocality` \| `index`; locality is what makes frustum culling work (`G-storm-throughput` §2) |
| `usdGen:interactive:maxCurves` | `int` | 50000 | LOD ceiling while a tool is dragging (S31: decimate count, never refineLevel) |
| `usdGen:interactive:mode` | `token` | `"decimate"` | `decimate` \| `full` |
| `usdGen:widths:default` | `float` | 0.01 | fallback strand width in stage units |
| `usdGen:motion:mode` | `token` | `"single"` | `single` \| `velocities` \| `samples` (S32 profiles P0/P1/P2) |
| `usdGen:motion:sampleCount` | `int` | 3 | 2..16, used by P2 |
| `usdGen:pickTarget` | `token` | `"description"` | `description` \| `terminal`; drives the synthesized `primOrigin` (S29) |
| `usdGen:cache:budgetMB` | `int` | 0 | 0 = per-node caching everywhere (S24); >0 turns off caching on the cheapest nodes first |

`material:binding` on the Description is inherited by every chunk prim (§7).

### 3.3 The operator base

**`UsdGenOperator`** (abstract, : `UsdTyped`) — every operator, generator, styler, deformer,
freeze and sculpt layer derives from it, so a TD's own type inherits the whole vocabulary and the
adapter picks it up for free (`includeDerivedPrimTypes: true`, S10, A4 §2.1).

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:input` | `rel` | (implicit preceding sibling, §1.8) | upstream operator(s); multi-target for ops that merge |
| `usdGen:enabled` | `bool` | 1 | a host groomer per-modifier enable; disabling is a *pass-through*, not a recompile (§4.6) |
| `usdGen:blend` | `float` | 1.0 | a DCC "Blend": final lerp between the input and this operator's result |
| `usdGen:seed` | `uniform int` | 0 | every hash in the operator is `hash(seed, curveId, salt)` |
| `usdGen:space` | `uniform token` | per type | `rest` \| `deformed` \| `world`; drives S25's `restSpace/deformedSpace` classification and therefore motion-blur cost (§4.8) |
| `usdGen:readPhase` | `uniform token` | `"final"` | `base` \| `preceding` \| `final` \| `@<prim>`, retained from usdRig for surface reads (S26) |
| `usdGen:surface` | `rel` | inherits Description | per-operator surface override (e.g. clump against a different mesh) |
| `usdGen:label` | `string` | "" | free-text note shown in the stack editor |

**`UsdGenMaskAPI`** (applied API schema; applied to every operator by the tool at creation) — the
uniform mask block, the single biggest parity item with a host groomer/a DCC (A7 §7).

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:mask:source` | `rel` | — | at most one `UsdGenMap`; composition is a `UsdGenCombineMap` (usdRig's weight-object shape, A1 §6) |
| `usdGen:mask:invert` | `bool` | 0 | |
| `usdGen:mask:range` | `float2` | (0,1) | remap of the source before use |
| `usdGen:mask:random` | `float` | 0.0 | per-curve `rand(seed,id)` multiplier amount (a host groomer `rand()` masks) |
| `usdGen:mask:randomSeed` | `uniform int` | 0 | |
| `usdGen:mask:ramp:knots` | `float2[]` | `[(0,1),(1,1)]` | along-curve ramp, (t, value) sorted by t (S11) |
| `usdGen:mask:ramp:interpolation` | `uniform token` | `"linear"` | `constant`\|`linear`\|`smooth`\|`monotoneCubic` |
| `usdGen:mask:ramp` | `float` (with `.spline`) | — | v2: whole `TsSpline` published by the adapter, parameter axis reinterpreted as root→tip (S11, MEASURED probe6) |
| `usdGen:mask:rangeMin` / `:rangeMax` / `:effectPosition` / `:falloff` | `float` | 0,1,0.5,0.5 | a DCC's four-parameter curve-mask shortcut, an alternative to the ramp (A7 §7) |
| `usdGen:mask:noise:amount` | `float` | 0.0 | a DCC "Noise Mask" |
| `usdGen:mask:noise:frequency` | `float` | 1.0 | |
| `usdGen:mask:noise:gain` / `:bias` | `float` | 0.5 / 0.5 | |
| `usdGen:mask:region` | `rel` | — | region/parting map that constrains membership (a host groomer region maps) |

The resolved mask is `w(h,t) = clamp(range(source(h)) ^invert * lerp(1, rand, amount) * noise(h)) * ramp(t)`,
computed once per capture into one `VtFloatArray` per operator (per curve) plus one 257-entry ramp
LUT (usdRig's falloff-LUT precedent, A1 §6). **MEASURED cost anchor:** SeExpr evaluation is
13–117 ns/eval and Ptex lookup 23 ns (S38), so a 1 M-root mask is ~25–120 ms of *capture*, never
per frame.

### 3.4 Generators, stylers, deformers, freeze, sculpt

Full parameter tables are in §6 (D4). The type list, with base and Hydra effect:

| Prim type | Base | Topology | Space (default) |
|---|---|---|---|
| `UsdGenScatter` | `UsdGenGenerator` | creates roots | rest |
| `UsdGenGrow` | `UsdGenGenerator` | sets CV count | rest |
| `UsdGenGuideSet` | `UsdGenGenerator` | curve set from child `BasisCurves` or N% scatter | rest |
| `UsdGenGuideInterpolate` | `UsdGenGenerator` | sets CV count | rest |
| `UsdGenCurveSource` | `UsdGenGenerator` | reads a stage `BasisCurves` into the graph | rest |
| `UsdGenClump` | `UsdGenStyler` | no | rest |
| `UsdGenNoise` | `UsdGenStyler` | no | rest |
| `UsdGenCurl` | `UsdGenStyler` | no | rest |
| `UsdGenBend` | `UsdGenStyler` | no | rest |
| `UsdGenDirection` | `UsdGenStyler` | no | rest |
| `UsdGenLength` | `UsdGenStyler` | cull/reparam only | rest |
| `UsdGenWidth` | `UsdGenStyler` | no | rest |
| `UsdGenSmooth` / `UsdGenStraighten` | `UsdGenStyler` | no | rest |
| `UsdGenDisplace` | `UsdGenStyler` | no | rest |
| `UsdGenWave` | `UsdGenStyler` | no | rest |
| `UsdGenResample` | `UsdGenStyler` | CV count | rest |
| `UsdGenCollide` | `UsdGenStyler` | no | **deformed** |
| `UsdGenWind` | `UsdGenStyler` | no | **deformed** |
| `UsdGenDeform` | `UsdGenDeformer` | no | **deformed** |
| `UsdGenFreeze` | `UsdGenOperator` | no | inherits |
| `UsdGenSculptLayer` | `UsdGenOperator` | no | rest |
| `UsdGenInstance` | `UsdGenOperator` | emits an instancer | deformed |
| `UsdGenExprOp` | `UsdGenStyler` | no | authored |

`UsdGenFreeze` is the artist's "Groom Bake" (A7 §9.3): it names a `BasisCurves` prim holding the
snapshot and a mode.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:frozen:curves` | `rel` | — | the frozen `BasisCurves` (S42 contract) |
| `usdGen:frozen:mode` | `token` | `"frozen"` | `frozen` (read the snapshot; upstream is not evaluated) \| `live` (pass through; the snapshot is kept but stale) |
| `usdGen:frozen:epoch` | `uniform string` | "" | must equal the target's `primvars:usdGen:frozenEpoch`; mismatch → **stale**, drawn in the stack editor with a warning badge and evaluated as `live` |
| `usdGen:frozen:tier` | `uniform token` | `"session"` | `session` \| `sublayer` \| `payload` (S42 T1/T2/T3) |

`UsdGenSculptLayer` stores per-CV deltas in the root frame, keyed by stable `curveId`, so an
upstream parameter tweak that keeps ids still applies (A7 §9.3):

| Property | Type | Default |
|---|---|---|
| `usdGen:sculpt:curveIds` | `int[]` | `[]` (sorted, matches `primvars:usdGen:curveId`) |
| `usdGen:sculpt:cvOffsets` | `int[]` | `[]` (prefix sum, size = curveIds+1) |
| `usdGen:sculpt:deltas` | `vector3f[]` | `[]` |
| `usdGen:sculpt:space` | `uniform token` | `"rootFrame"` (\| `object`) |
| `usdGen:sculpt:weight` | `float` | 1.0 (a host groomer sculpt-layer weight) |
| `usdGen:sculpt:freezeMask` | `int[]` | `[]` — curve ids whose downstream stylers are suppressed (a host groomer Freeze brush) |

Multiple sculpt layers stack in chain order and blend additively.

### 3.5 Maps and expressions

**`UsdGenMap`** (abstract, : `UsdTyped`) — a field evaluated on the **CPU at capture time** and
baked to per-curve or per-CV arrays (S37).

| Common property | Type | Default |
|---|---|---|
| `usdGen:map:domain` | `uniform token` | `"root"` — `root` (per curve, at the root UV) \| `cv` (per CV) |
| `usdGen:map:channel` | `uniform token` | `"r"` — `r`\|`g`\|`b`\|`a`\|`rgb`\|`luminance` |
| `usdGen:map:scale` / `:offset` | `float` | 1.0 / 0.0 |
| `usdGen:map:clamp` | `float2` | (0,1) (empty = no clamp) |

| Concrete type | Key properties |
|---|---|
| `UsdGenImageMap` | `asset usdGen:map:file` (one `asset`, never `asset[]` — S13: asset arrays get no reload tracking), `token usdGen:map:uvSet = "st"`, `token usdGen:map:wrap = "clamp"`, `token usdGen:map:filter = "bilinear"` |
| `UsdGenPtexMap` | `asset usdGen:map:file` (`.ptx`), `token usdGen:map:filter = "bilinear"`, `float usdGen:map:blur = 0`, `int usdGen:map:faceOffset = 0` |
| `UsdGenExprMap` | `string usdGen:expr:source` (SeExpr text), `token usdGen:expr:returnType = "float"` (`float`\|`color`), `rel usdGen:expr:maps` (named maps reachable from `map("name")`) |
| `UsdGenPaintMap` | `rel usdGen:paint:surface`, `token usdGen:paint:primvar = "usdGen:paint:<name>"`, `token usdGen:paint:interpolation = "faceVarying"`, `asset usdGen:paint:bakedFile` (EXR or `.ptx` after a bake) |
| `UsdGenNoiseMap` | `token usdGen:noise:type = "fbm"`, `float usdGen:noise:frequency/lacunarity/gain`, `int usdGen:noise:octaves` — SeExpr's noise is the single noise implementation everywhere (S38) |
| `UsdGenCombineMap` | `rel usdGen:combine:inputs` (ordered), `token usdGen:combine:mode = "multiply"` (`multiply`\|`add`\|`subtract`\|`max`\|`min`\|`average`\|`overlay`, usdRig's `RigExecCombineWeight` vocabulary, A1 §6) |
| `UsdGenGuideProximityMap` | `rel usdGen:guides`, `float usdGen:proximity:radius`, `float usdGen:proximity:decay` |

A `UsdGenPaintMap` is what the paint brush writes *during* a session: a plain primvar on the
surface mesh (`primvars:usdGen:paint:density`, faceVarying or vertex), which means the painted
value is a normal USD attribute an artist can inspect, diff and hand-edit. A separate "Bake maps"
action turns it into a `.ptx`/EXR when it needs to leave the session (§8.6). This is the
round-trip A3 §7.2 recommends, and it never writes image files during interaction.

### 3.6 LOD and density, from the artist's side

| Control | Where | Effect |
|---|---|---|
| Preview density | `UsdGenGroom.usdGen:densityScale` | multiplies every description's density in Storm |
| Render density | `usdGen:renderDensityScale` | applied only for non-Storm delegates |
| Interactive ceiling | `UsdGenDescription.usdGen:interactive:maxCurves` | during a drag, the generator emits at most N curves; the culled strands keep their CVs and get `widths = 0` so element counts never change mid-drag (S28) |
| Density scrub | the density slider itself | same trick: topology is held at max-density, culled strands collapse to a degenerate CV with zero width; the real count change is committed on release (S28, `G-storm-throughput` §2) |

Never touch `refineLevel` for LOD: refineLevel 0 is *slower* than 1 at 200 k curves (MEASURED,
S31), so `displayStyle/refineLevel = 2` is pinned per chunk prim (S29) and LOD is curve
decimation only.

### 3.7 `.usda` example (a) — generate-and-style on a usdRig-deformed scalp

```usda
#usda 1.0
(
    defaultPrim = "Char"
    metersPerUnit = 0.01
    upAxis = "Y"
)

def Xform "Char"
{
    def Mesh "Scalp" (
        prepend apiSchemas = ["UsdGenRestAPI"]      # rest points at Default() (S12)
    )
    {
        int[] faceVertexCounts = [...]
        int[] faceVertexIndices = [...]
        point3f[] points = [...]                     # deformed by usdRig upstream (R5, S1)
        texCoord2f[] primvars:st = [...] ( interpolation = "faceVarying" )
        float[] primvars:usdGen:paint:density = [...] ( interpolation = "faceVarying" )
    }

    def Scope "Rig" { }                               # usdRig movers; usdGen never reads them

    def UsdGenGroom "Groom"
    {
        rel usdGen:surface = </Char/Scalp>
        float usdGen:densityScale = 1
        uniform int usdGen:schemaVersion = 1

        def UsdGenDescription "hair" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            rel material:binding = </Char/Looks/HairLook>
            uniform token usdGen:primitive = "splines"
            uniform int usdGen:chunkCount = 64
            int usdGen:interactive:maxCurves = 40000
            rel usdGen:output = </Char/Groom/hair/Ops/op05_frizz>
            uniform token usdGen:motion:mode = "single"

            def Scope "Guides"
            {
                def UsdGenGuideSet "guides" { uniform token usdGen:guides:source = "children" }
                def BasisCurves "g000" { ... }        # S42 contract; hand-groomable
                def BasisCurves "g001" { ... }
            }

            def Scope "Maps"
            {
                def UsdGenPaintMap "densityPaint"
                {
                    rel usdGen:paint:surface = </Char/Scalp>
                    uniform token usdGen:paint:primvar = "usdGen:paint:density"
                    uniform token usdGen:paint:interpolation = "faceVarying"
                }
                def UsdGenExprMap "lengthVar"
                {
                    uniform token usdGen:map:domain = "root"
                    string usdGen:expr:source = '''
                        # length falls off toward the neck, plus 15% per-hair variation
                        $base = 1.0 - smoothstep(0.35, 0.75, $v);
                        $base * (0.85 + 0.30 * rand($id))
                    '''
                }
                def UsdGenImageMap "tipTint"
                {
                    asset usdGen:map:file = @./maps/tipTint.exr@
                    uniform token usdGen:map:channel = "rgb"
                }
            }

            def Scope "Ops"
            {
                def UsdGenScatter "op01_scatter"
                {
                    uniform token usdGen:mode = "random"
                    float usdGen:density = 2400
                    uniform int usdGen:seed = 7
                    int usdGen:relaxIterations = 4
                    bool usdGen:areaCompensation = 1
                    rel usdGen:mask:source = </Char/Groom/hair/Maps/densityPaint>
                }

                def UsdGenGuideInterpolate "op02_interp" (
                    prepend apiSchemas = ["UsdGenMaskAPI"]
                )
                {
                    rel usdGen:input = </Char/Groom/hair/Ops/op01_scatter>
                    rel usdGen:guides = </Char/Groom/hair/Guides/guides>
                    int usdGen:maxGuides = 3
                    float usdGen:influenceRadius = 6.0
                    float usdGen:influenceDecay = 2.0
                    float usdGen:maxGuideAngle = 75
                    float usdGen:blendInSkinSpace = 1.0
                    uniform token usdGen:blendMethod = "extrudeAndBlend"
                    int usdGen:cvCount = 8
                    rel usdGen:length:source = </Char/Groom/hair/Maps/lengthVar>
                }

                def UsdGenClump "op03_clumpBig" (
                    prepend apiSchemas = ["UsdGenMaskAPI"]
                )
                {
                    rel usdGen:input = </Char/Groom/hair/Ops/op02_interp>
                    float usdGen:clump:amount = 0.75
                    float usdGen:clump:size = 1.6
                    uniform int usdGen:clump:seed = 12
                    float2[] usdGen:clump:profile:knots = [(0, 0), (0.35, 0.55), (1, 1)]
                    float usdGen:clump:stray:amount = 0.25
                    float usdGen:clump:stray:rate = 0.06
                    int usdGen:clump:levels = 1
                    float usdGen:blend = 1.0
                    float2[] usdGen:mask:ramp:knots = [(0, 0), (0.2, 1), (1, 1)]
                }

                def UsdGenClump "op04_clumpFine" (
                    prepend apiSchemas = ["UsdGenMaskAPI"]
                )
                {
                    rel usdGen:input = </Char/Groom/hair/Ops/op03_clumpBig>
                    float usdGen:clump:amount = 0.45
                    float usdGen:clump:size = 0.5
                    uniform int usdGen:clump:seed = 91
                    float usdGen:clump:noise:amount = 0.08
                    float usdGen:clump:noise:frequency = 3.0
                    float usdGen:clump:noise:correlation = 0.7
                }

                def UsdGenNoise "op05_frizz" (
                    prepend apiSchemas = ["UsdGenMaskAPI"]
                )
                {
                    rel usdGen:input = </Char/Groom/hair/Ops/op04_clumpFine>
                    float usdGen:noise:magnitude = 0.09
                    float usdGen:noise:frequency = 5.0
                    float usdGen:noise:correlation = 0.35
                    int usdGen:noise:octaves = 3
                    uniform token usdGen:space = "rest"
                    float2[] usdGen:mask:ramp:knots = [(0, 0), (0.4, 0.2), (1, 1)]
                    float usdGen:mask:random = 0.4
                }
            }
        }
    }

    def Scope "Looks" { def Material "HairLook" { } }   # §7
}
```

What an artist reads out of that in usdview: one Description, five operators in run order, the
guide set they can select and comb, three maps they can repaint, one material. Nothing in the tree
is machine-generated. This is A1.

### 3.8 `.usda` example (b) — frozen curves deformed with the surface, plus a sculpt layer

The freeze operator does not replace the chain; it *caps* it. Upstream operators stay in the
stack, greyed out in the stack editor, and can be re-enabled by switching one token — a host groomer's
"deactivates all modifiers below it" (A7 §9.3), reversibly.

```usda
def UsdGenDescription "hair"
{
    rel usdGen:surface = </Char/Scalp>
    rel usdGen:output = </Char/Groom/hair/Ops/op08_sculpt>

    def Scope "Frozen"
    {
        def BasisCurves "bake_v003"
        {
            int[] curveVertexCounts = [8, 8, 8, ...]
            point3f[] points = [...]                    # exact size: sum(counts)  (S28)
            float[] widths = [...] ( interpolation = "vertex" )
            uniform token type = "cubic"
            uniform token basis = "bspline"
            uniform token wrap = "pinned"                                   # (S29)
            point3f[] primvars:rest = [...] ( interpolation = "vertex" )    # shares the points buffer -> +26 bytes in crate (MEASURED)
            int[] primvars:skinprim = [...] ( interpolation = "uniform" )
            texCoord2f[] primvars:skinprimuv = [...] ( interpolation = "uniform" )
            int[] primvars:usdGen:curveId = [...] ( interpolation = "uniform" )
            string primvars:usdGen:frozenEpoch = "sha1:8d31f0…" ( interpolation = "constant" )
            matrix4d[] primvars:usdGen:rootFrame = [...] ( interpolation = "uniform" )
        }
    }

    def Scope "Ops"
    {
        def UsdGenScatter "op01_scatter" { ... }            # kept, inert while the freeze is frozen
        def UsdGenGuideInterpolate "op02_interp" { ... }
        def UsdGenClump "op03_clumpBig" { ... }

        def UsdGenFreeze "op06_bake"
        {
            rel usdGen:input = </Char/Groom/hair/Ops/op03_clumpBig>
            rel usdGen:frozen:curves = </Char/Groom/hair/Frozen/bake_v003>
            uniform token usdGen:frozen:mode = "frozen"
            uniform string usdGen:frozen:epoch = "sha1:8d31f0…"
            uniform token usdGen:frozen:tier = "sublayer"
        }

        def UsdGenDeform "op07_deform"
        {
            rel usdGen:input = </Char/Groom/hair/Ops/op06_bake>
            uniform token usdGen:mode = "rigidFrame"       # rigidFrame | rbf | pointDeform
            bool usdGen:twistAware = 1
            float usdGen:preserveShape = 0.0
            uniform token usdGen:space = "deformed"
        }

        def UsdGenSculptLayer "op08_sculpt"
        {
            rel usdGen:input = </Char/Groom/hair/Ops/op07_deform>
            float usdGen:sculpt:weight = 1.0
            uniform token usdGen:sculpt:space = "rootFrame"
            int[] usdGen:sculpt:curveIds = [41, 42, 43, 88, ...]
            int[] usdGen:sculpt:cvOffsets = [0, 8, 16, 24, ...]
            vector3f[] usdGen:sculpt:deltas = [...]
            int[] usdGen:sculpt:freezeMask = [88]
        }
    }
}
```

Three properties of this shape matter for the artist:

1. **A frozen prim re-enters the graph straight from the scene index** — no stage read, no special
   loader (MEASURED, `G-freeze-bake` §2.2; S42). So a `BasisCurves` produced by usdGen, by a sim,
   by an Alembic import or by another department is *the same kind of input*. That is R4's "rigged
   or simulated curves" for free (use `UsdGenCurveSource` when there is no freeze operator).
2. **The sculpt deltas are keyed by `curveId`**, so re-running the generator with the same seed and
   surface keeps the comb work. When `primvars:usdGen:frozenEpoch` no longer matches
   `usdGen:frozen:epoch`, the freeze is *stale*: the stack editor badges it, the evaluator falls
   back to `live`, and a "Rebase sculpt" action re-matches deltas by nearest root UV.
3. **Undo of a live freeze is `SetActive(false)`**, never `RemovePrim` (S41 — `RemovePrim` under an
   attached OpenExec system posts a spurious `Tf.ErrorException`, `esfUsd/stageData.cpp:360`,
   MEASURED, and with usdview's session-layer edit target a `RemovePrim` on a root-layer prim
   silently does nothing, `G-freeze-bake` §1.5).

### 3.9 `.usda` example (c) — cards / archives instancer

```usda
def UsdGenDescription "featherCards"
{
    rel usdGen:surface = </Bird/Body>
    uniform token usdGen:primitive = "cards"
    rel usdGen:output = </Bird/Groom/featherCards/Ops/op02_cards>

    def Scope "Prototypes"                     # pruned from the output; re-rooted under the instancer (S33)
    {
        def Mesh "cardA" { ... }               # 1 quad, uv'd, own material binding
        def Mesh "cardB" { ... }
        def Xform "quill" ( prepend references = @./archives/quill.usda@ ) { }   # a subtree = an archive
    }

    def Scope "Ops"
    {
        def UsdGenScatter "op01_scatter"
        {
            uniform token usdGen:mode = "random"
            float usdGen:density = 40
            uniform int usdGen:seed = 3
            rel usdGen:mask:source = </Bird/Groom/featherCards/Maps/coverage>
        }

        def UsdGenInstance "op02_cards"
        {
            rel usdGen:input = </Bird/Groom/featherCards/Ops/op01_scatter>
            rel usdGen:prototypes = [
                </Bird/Groom/featherCards/Prototypes/cardA>,
                </Bird/Groom/featherCards/Prototypes/cardB>,
                </Bird/Groom/featherCards/Prototypes/quill>
            ]
            float[] usdGen:instance:weights = [0.6, 0.3, 0.1]     # prototype choice by hashed weight
            uniform token usdGen:instance:orient = "surfaceFrame" # surfaceFrame | curveTangent | world
            float usdGen:instance:scale = 1.0
            float2 usdGen:instance:scaleRandom = (0.85, 1.2)
            float usdGen:instance:twist = 0.0
            float usdGen:instance:twistRandom = 25.0
            float usdGen:instance:normalOffset = 0.02
            bool usdGen:instance:alignToCurve = 1                 # use the generated curve's tangent
            token[] usdGen:instance:variationPrimvars = ["displayColor", "usdGen:featherAge"]
        }
    }
}
```

What usdGen synthesizes downstream (S33, verified end-to-end in `G-instancing` probes 1/3/4):

```
/Bird/Groom/featherCards/__usdGenRender/inst_op02_cards        primType = "instancer"
    instancerTopology/prototypes      = [ …/inst_op02_cards/Prototypes/cardA, …/cardB, …/quill ]
    instancerTopology/instanceIndices = [ VtIntArray, VtIntArray, VtIntArray ]
    primvars/hydra:instanceTranslations (instance)  primvars/hydra:instanceRotations (instance)
    primvars/hydra:instanceScales (instance)        primvars/displayColor (instance)
    primOrigin/scenePath = /Bird/Groom/featherCards
/Bird/Groom/featherCards/__usdGenRender/inst_op02_cards/Prototypes/cardA   primType = "mesh"
    instancedBy/paths = [ …/inst_op02_cards ]      # exactly one (TF_CODING_ERROR otherwise)
    instancedBy/prototypeRoots = [ …/Prototypes/cardA ]
```

Rules carried straight from the evidence: prototypes are namespace children of the instancer
(S33); `instancedBy` holds exactly one path; per-instance variation is `instance`-interpolated
primvars (Storm applies **no** filtering to them, `hdSt/primUtils.cpp:211-221`); material variety
is multiple prototypes because Storm has no per-instance material binding (S33); an interactive
tweak of card placement dirties `primvars/hydra:instanceTranslations`, never `instancerTopology`
(S33); every synthesized prim carries `primOrigin` or usdview selects nothing (S29, `G-tool-loop`
§4). Hair generated on a natively instanced scalp is emitted once per propagated prototype path,
with `instancedBy` hand-authored from `__usdPrimInfo.isNiPrototype`/`niPrototypePath` and a
**relative** `primOrigin` (S34) — propagated prototype names are hashes and are discovered, never
constructed.

### 3.10 Versioning

* `usdGen:schemaVersion` (major) on the Groom. Minor additions are new properties with schema
  fallbacks, which cost nothing to old readers.
* Operator types never change the meaning of a property; a behaviour change gets a new property
  with the old default preserving the old behaviour (`usdGen:clump:method = "linearBlend"` today,
  `"extrudeAndBlend"` opt-in), which is what an artist expects from a shot that must not shift.
* The frozen-curve contract (S42) is versioned by the `frozenEpoch` string's prefix
  (`sha1:` today); a reader that does not know a prefix treats the freeze as stale, never as valid.

---

## 4. D2 — Graph and engine API

### 4.1 Shape

Bespoke TBB DAG, not a persistent `VdfNetwork` (S21: 4× faster at 100 k curves, 9–15× at 1 M,
3–250× on sparse edits, half the RSS — MEASURED). One `UsdGenGraph` per Description. Curve data is
**SoA internally, interleaved once at the Hydra boundary** (S22: 25 → 81 GFLOP/s for the same
kernel, MEASURED). Chunks are 512 curves, always on curve boundaries (S23).

### 4.2 Buffers

```cpp
// usdGen/curveBuffer.h
namespace usdGen {

/// A named planar attribute plane. Per-CV planes have cvCount entries,
/// per-curve planes curveCount entries.
struct UsdGenPlane {
    TfToken       name;          // "width", "hairT", "displayColor:r", …
    TfToken       interpolation; // vertex | uniform | constant
    VtFloatArray  data;
};

/// One node's output. SoA; never handed to Hydra in this form (S22).
class UsdGenCurveBuffer {
public:
    // Topology (per curve)
    VtIntArray    vertexCounts;      // size = curveCount
    VtIntArray    cvOffsets;         // size = curveCount + 1, prefix sum
    VtIntArray    curveId;           // stable ids, sorted ascending within a chunk
    VtIntArray    rootPrim;          // face index on the bound surface
    VtVec2fArray  rootUV;            // barycentric/uv within that face
    VtFloatArray  rootFrame;         // 9 floats per curve, row-major 3x3 (rest frame)

    // Geometry (per CV), planar
    VtFloatArray  px, py, pz;
    VtFloatArray  width;
    std::vector<UsdGenPlane> planes; // hairT, hairTangent{x,y,z}, displayColor{r,g,b}, clumpId, …

    size_t   CurveCount() const { return vertexCounts.size(); }
    size_t   CvCount()    const { return px.size(); }
    uint64_t topologyVersion = 0;    // bumped when curveCount or vertexCounts change
    uint64_t valueVersion    = 0;

    void     SwapValuesFrom(UsdGenCurveBuffer *other);
};

/// AoS conversion, once, at the scene-index boundary.
VtVec3fArray UsdGenInterleavePoints(const UsdGenCurveBuffer &);
VtVec3fArray UsdGenInterleavePlane3(const UsdGenCurveBuffer &, const TfToken &prefix);
} // namespace usdGen
```

### 4.3 Chunks and views

```cpp
struct UsdGenChunk {
    uint32_t index;
    uint32_t curveBegin, curveCount;   // always whole curves (S23)
    uint32_t cvBegin,    cvCount;
    GfRange3f extent;                  // maintained per chunk, per frame (S29)
};

/// A writable window into one node's output plus a read-only window into its input.
struct UsdGenChunkView {
    const UsdGenChunk *chunk;
    const UsdGenCurveBuffer *in;
    UsdGenCurveBuffer       *out;      // this node owns it (S24)
    const int   *vertexCounts;         // in->vertexCounts + curveBegin
    const int   *cvOffsets;            // relative to cvBegin
    const float *ipx, *ipy, *ipz;
    float       *opx, *opy, *opz;
    const float *mask;                 // per-curve resolved mask, or nullptr
    const float *rampLut;              // 257-entry along-curve LUT, or nullptr
};
```

Chunk size default 512 (128–1024 measured acceptable; 4096 is a cliff at 9.40 ms vs 1.83 ms,
MEASURED, S23).

### 4.4 The operator interface (this is the TD extension point)

```cpp
// usdGen/op.h
enum class UsdGenSpace { Rest, Deformed, World };            // S25

struct UsdGenCaptureContext {
    const UsdGenSurfaceSample   *surface;       // rest + deformed positions, normals, dPdu/dPdv, adjacency
    const UsdGenCurveBuffer     *input;         // upstream topology after its own capture
    const UsdGenMapEvaluator    *maps;          // image / ptex / expr / paint, CPU (S37)
    const UsdGenGuideSetView    *guides;
    uint64_t                     epochDigest;   // key of the capture cache (S25)
    uint32_t                     seed;
    WorkDispatcher              *dispatcher;
};

struct UsdGenEvalContext {
    float                        time;
    float                        shutterOffset;  // 0 for P0/P1 (S32)
    const UsdGenSurfaceSample   *surface;        // at time + shutterOffset
    const UsdGenCurveBuffer     *guideBuffer;
    uint32_t                     seed;
};

class UsdGenOp {
public:
    virtual ~UsdGenOp();

    virtual const TfToken &TypeName() const = 0;
    virtual UsdGenSpace    Space() const { return UsdGenSpace::Rest; }
    virtual bool           ChangesTopology() const { return false; }

    /// Read parameters out of the Hydra `usdGen` container. Called on compile and on
    /// every parameter dirty; must be cheap and must not allocate curve-sized memory.
    virtual bool Configure(const UsdGenParams &p, UsdGenDiagnostics *diag) = 0;

    /// Topology-dependent work: scatter, stable ids, kd-trees, guide weights, clump ids,
    /// map/Ptex/SeExpr samples, mask arrays. Cached by `epochDigest` (S25).
    virtual void Capture(const UsdGenCaptureContext &ctx) {}

    /// Generators only: produce the output topology (curve count, CV counts, ids, bindings).
    virtual void GenerateTopology(const UsdGenCaptureContext &ctx,
                                  UsdGenCurveBuffer *out) {}

    /// Per-frame, per-chunk, called from tbb::parallel_for. Must write only `out`'s chunk.
    virtual void Evaluate(const UsdGenEvalContext &ctx,
                          const UsdGenChunkView &view) const = 0;

    /// Which upstream chunks this node's chunk reads. Identity for per-curve stylers;
    /// a fan-in set for clumping across chunk boundaries (§4.7).
    virtual void UpstreamChunks(uint32_t myChunk, std::vector<uint32_t> *out) const;

    /// Parameter tokens that dirty this node. Used to build the locator→node map (§4.5).
    virtual const TfTokenVector &ParameterNames() const = 0;

    /// Optional: declare the primvars this operator writes, so the publisher knows the
    /// chunk prim's primvar set without evaluating.
    virtual const TfTokenVector &OutputPrimvars() const;
};
using UsdGenOpPtr = std::unique_ptr<UsdGenOp>;
```

Registration — one macro, one plugin library, no core change:

```cpp
// usdGen/opRegistry.h
struct UsdGenOpInfo {
    TfToken   primType;        // "UsdGenClump"
    TfToken   displayName;     // "Clump"
    TfToken   category;        // generator | styler | deformer | utility
    int       minSchemaVersion = 1;
};

class UsdGenOpRegistry {
public:
    static UsdGenOpRegistry &GetInstance();
    using Factory = std::function<UsdGenOpPtr()>;
    void       Register(const UsdGenOpInfo &, Factory);
    UsdGenOpPtr Create(const TfToken &primType) const;
    std::vector<UsdGenOpInfo> GetAllOps() const;      // drives the "Add operator" menu
};

#define USDGEN_REGISTER_OP(cls, primTypeStr, displayStr, categoryStr)                 \
    TF_REGISTRY_FUNCTION(usdGen::UsdGenOpRegistry) {                                  \
        usdGen::UsdGenOpRegistry::GetInstance().Register(                             \
            {TfToken(primTypeStr), TfToken(displayStr), TfToken(categoryStr)},        \
            []{ return usdGen::UsdGenOpPtr(new cls); });                              \
    }
```

A TD's out-of-tree operator therefore needs: a codeless schema `.usda` snippet declaring a type
deriving from `UsdGenStyler`, a 60-line C++ class, one macro, one `plugInfo.json`. The adapter
picks up its properties automatically because it builds mappings from
`UsdPrimDefinition::GetPropertyNames()` (S10, MEASURED in `G-stage-free` §1) and registers for
derived types (`includeDerivedPrimTypes: true`). **The non-C++ tier is `UsdGenExprOp`**: a styler
whose kernel is a SeExpr expression over `$P $t $id $u $v $N $T` returning a displacement, at
13–117 ns/eval with one thread-safe `VarBlock` per TBB worker (S38, MEASURED).

### 4.5 Dirty propagation: locator → node → chunk → Hydra leaf

Four hops, each of which must be narrow or interactivity dies (R8).

| Hop | Mechanism |
|---|---|
| Hydra locator → node parameter | The adapter publishes `usdGen/<group>/<param>` (nested, MEASURED `G-stage-free` §2). The session keeps `std::unordered_map<HdDataSourceLocator, std::pair<UsdGenNodeId, TfToken>>` built at compile from every op's `ParameterNames()` |
| node parameter → node dirty | `UsdGenGraph::DirtyParameter(node, token)` marks all chunks of that node and all downstream nodes dirty; `Configure` is re-run for that node only |
| surface `primvars/points/primvarValue` → chunk dirty | The surface reader (S3) computes which chunks own roots on the changed surface and calls `DirtyChunks`; a whole-surface change dirties every chunk of every `deformed`-space node (rest-space head stays cached) |
| brush stroke → chunk dirty | The tool pushes a curve-id footprint; the session maps ids → chunks and calls `DirtyChunks` on the terminal node only (live override path, §8.4) |
| node output → Hydra leaves | The publisher emits `primvars/points/primvarValue` (+ `extent`) per changed chunk prim, and only those (S30). `primvars/<name>` bare only when a primvar *appears* (MEASURED, `G-tool-loop` §5: a `primvarValue`-only dirty does not refresh descriptors, which is exactly what we want per move) |

Measured targets from the prototype (S21, 100 k curves, 5 stylers, 20 threads): full run
1.72–1.91 ms; 1 % of chunks dirty on all 5 nodes 0.035–0.044 ms; last-parameter edit
0.179–0.411 ms; first-parameter edit 1.75–1.97 ms. At 1 M curves: full 7.6 ms, 0.5 % sparse
0.29 ms.

### 4.6 Compile, digests and sub-graph recompiles

```cpp
class UsdGenGraph {
public:
    static UsdGenGraphPtr Compile(const UsdGenSceneView &scene,
                                  const SdfPath &descriptionPath,
                                  UsdGenDiagnostics *diag);

    uint64_t StructuralDigest() const;                  // paths, types, edges, read phases (S26)
    bool     Recompile(const UsdGenSceneView &scene, UsdGenNodeIdSet affected);

    void DirtyParameter(UsdGenNodeId, const TfToken &);
    void DirtyChunks   (UsdGenNodeId, TfSpan<const uint32_t>);
    void DirtyTopology (UsdGenNodeId);
    void DirtySurface  (const SdfPath &, UsdGenSurfaceDirtyBits);

    const UsdGenCurveBuffer &Evaluate(UsdGenNodeId terminal, const UsdGenEvalContext &);
    const UsdGenCurveBuffer &EvaluateSample(UsdGenNodeId terminal, float shutterOffset); // P2

    UsdGenNodeId TerminalNode() const;
    const UsdGenNodeStats &Stats(UsdGenNodeId) const;   // diagnostics (§4.10)
};
```

* Edges come from `usdGen:input`, resolved with the implicit-sibling fallback (§1.8); order is
  **Kahn topological with namespace order as tie-break**; cycles are compile errors reported on the
  offending prim (S26).
* The **structural digest** covers operator paths, types, resolved edges, read phases, surface
  bindings and the *shape* of array parameters; it excludes animated scalars and every value that
  a slider changes (S26). Digest mismatch recompiles only the affected sub-graph.
* `usdGen:enabled = 0` is **not** structural: the node stays in the graph and becomes a memcpy
  pass-through. Toggling an operator is therefore a value edit costing one chain re-run of the
  tail, not a recompile. (Artist consequence: A/B-ing a modifier is instant.)

### 4.7 Two hard cases: guide interpolation and clumps across chunks

Both need queries beyond a chunk, and both are solved in **capture**, never in evaluate:

* **Guide interpolation.** Capture builds a kd-tree (nanoflann 1.12.1, S38) over guide roots in
  *rest* space, filters candidates by angle, region and parting lines, and stores per curve
  `guideIdx[maxGuides]` + `guideW[maxGuides]` — the host renderer `groom_closest_guides`/
  `groom_guide_weights` shape (A7 §9.1 G6). Evaluate is then per-curve embarrassingly parallel
  (cost class A) reading a *whole-buffer* guide array that lives outside the chunk system, because
  guides are a separate, small node (typically 100–2000 curves) evaluated to completion before any
  hair chunk runs. `UpstreamChunks` returns the identity set.
* **Clumping.** Capture assigns `clumpId` per curve from a kd-tree over clump centres and stores,
  per chunk, the set of *clump-centre curve indices* it needs. Clump centres are themselves a
  small buffer (a nested scatter at `clumpDensity`, or explicit clump curves, or a map) that is
  fully evaluated before the hair pass, so a hair chunk never reads another hair chunk. Fractal
  levels (`usdGen:clump:levels`) are a loop with a barrier between levels inside the node.
  `UpstreamChunks` is identity.

The one operator that *does* fan in across chunks is `UsdGenSmooth` in `neighbours` mode; it
declares its neighbour chunk set from the capture-time kd-tree and the graph honours it.

### 4.8 Motion blur

Three profiles exactly as S32/`G-motion-blur` §4:

| Profile | Trigger | Cost |
|---|---|---|
| P0 single | default; Storm never samples | 1 × chain |
| P1 velocities | `usdGen:motion:mode = "velocities"` | 1 × chain + one finite-difference pass (0.97–1.00 ms per 1.6 M points, MEASURED); blocks upstream `velocities`/`accelerations` |
| P2 samples | `= "samples"`, lazily on the first interval pull or an app preflight | 1 × rest-space head (cached) + k × deformed-space tail |

The `usdGen:space` classification is what makes P2 affordable: only nodes marked `deformed`/`world`
(and everything downstream of them) re-run per shutter offset. In the canonical stack of §3.7 that
is *nothing* — the entire style chain is rest-space and the per-offset work is the deform transport
alone. Sample cache: `k+1` per prim, 19.2 MB per 100 k × 16 CV sample (MEASURED), keyed by
`(graphGeneration, surfaceGeneration, frame + offset)`.

### 4.9 Memory and threading

* Per-node output buffers by default (S24). Budget ≈ 6 × curve data for a 5-deep chain: **MEASURED
  668 MB at 1 M curves × 8 CV** with per-node caching, 293 MB without. `usdGen:cache:budgetMB` turns
  caching off starting with the cheapest nodes (cost model = measured ms/chunk from §4.10).
* Handoff to Hydra is a `VtArray` copy-construct (refcount bump, 0.00005 ms MEASURED) and one CoW
  detach on the next edit (2.32 vs 1.97 ms for a 9.6 MB buffer, MEASURED).
* Threads: **one commit thread** (the serialized evaluate+publish path, S18), **TBB workers**
  inside `tbb::parallel_for` over chunks, **Hydra readers** doing nothing but `atomic_load` of the
  published generation (S19: 0 torn reads under 8 readers × 20 publishes, MEASURED). Never cook in
  `GetPrim`; never cook in every `_PrimsDirtied` (S17).
* Kernels compile with `-ffp-contract=off` (S22/S45; usdRig's `testRigExecCurvenet` fails without
  it, MEASURED). No hand-written NEON: GCC 13.3 autovectorises the planar kernels to 80.9 GFLOP/s
  (MEASURED, S22).

### 4.10 Diagnostics (an artist-facing feature, not just a dev one)

```cpp
struct UsdGenNodeStats {
    double   captureMs, lastEvalMs, meanEvalMs;
    uint64_t curvesIn, curvesOut, chunksDirty, captureHits, captureMisses;
    uint32_t warnings;      // e.g. "guide angle rejected 42% of candidates"
};
```

Published per generation and read by the groom panel's **stack profiler column**: each operator row
shows its ms and its "% of frame", so an artist can see that `op03_clumpBig` is 60 % of their frame
and lower `levels` instead of guessing. `USDGEN_TRACE=1` additionally routes into `TraceCollector`.

---

## 5. D3 — Imaging library structure

### 5.1 Objects

| Class | Kind | Role |
|---|---|---|
| `UsdGenImagingRegistry` | process-global singleton (S15) | owns sessions, keyed by (weak `UsdStage*` \| `sessionId`) + groom root; survives renderer switch and stage replace |
| `UsdGenImagingSession` | per groom root | owns `UsdGenGraph`s (one per Description), the capture caches, the generation store, the live-override store, the map/Ptex/SeExpr caches |
| `UsdGenHairSceneIndex` | `HdSingleInputFilteringSceneIndexBase` | the renderer-level index (S1); publishes chunk prims, guides, instancers; forwards everything else untouched |
| `UsdGenHairSceneIndexPlugin` | `HdSceneIndexPlugin` | `loadWithRenderer: ""`, C++ registration at **phase 0, `InsertionOrderAtEnd`**, tags `["usdGen:groom"]`, ordering `after ["hd:sceneGlobals"]`, `before ["hdGp:proceduralResolution", "hdPrman:motionBlur"]` (S1, S2) |
| `UsdGenUsdImagingPlugin` | `UsdImagingSceneIndexPlugin` | **metadata only** — returns its input unchanged; exists solely for `InstanceDataSourceNames()` and `ProxyPathTranslationDataSourceNames()` (S7) |
| `UsdGenPrimAdapter` | `UsdImagingSceneIndexPrimAdapter` | publishes every `usdGen:*` property as a typed `usdGen` container via `UsdImagingDataSourceMapped` (S10) |
| `UsdGenRestAPIAdapter` | `UsdImagingAPISchemaAdapter` | `UsdGenRestAPI` → `usdGen/rest/points` sampled at `UsdTimeCode::Default()`, honouring an authored `primvars:rest` (S12) |
| `UsdGenSurfaceReader` | internal | wraps a **private** `HdSiExtComputationPrimvarPruningSceneIndex` around the input, pass-through when nothing is computed; never spliced into the shared chain (S3) |
| `UsdGenGenerationStore` | internal | atomic snapshot publish + diff (S18/S19) |
| `UsdGenLiveOverrideStore` | internal | per-prim point overrides during a drag (S40) |
| `UsdGenPublisher` | internal | turns a `UsdGenCurveBuffer` into chunk prim data sources; asserts exact array sizes (S28) |

### 5.2 The scene index

```cpp
class UsdGenHairSceneIndex : public HdSingleInputFilteringSceneIndexBase
{
public:
    static UsdGenHairSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input,
                                          HdContainerDataSourceHandle const &inputArgs);

    HdSceneIndexPrim GetPrim(const SdfPath &) const override;      // atomic_load only (S19)
    SdfPathVector    GetChildPrimPaths(const SdfPath &) const override;

    // Commit points, in priority order (S18)
    void Commit();                       // (a) explicit, from the usdview plugin
    void SetTime(double frame);

protected:
    void _PrimsAdded  (...) override;    // accumulate + forward unchanged
    void _PrimsRemoved(...) override;
    void _PrimsDirtied(...) override;    // (b) `/ sceneGlobals/currentFrame` triggers a commit

private:
    std::atomic<bool>            _pendingCommit;   // (c) backstop, read in GetPrim
    UsdGenImagingSessionWeakPtr  _session;
    TfToken                      _rendererDisplayName;   // from inputArgs
};
```

Commit discipline verbatim from S18: accumulate in notice handlers, forward input notices
immediately and unchanged, cook nothing there; commit on (a) the app's `Commit()`, (b) the
scene-globals `currentFrame` dirty — MEASURED as the last notice of every un-batched frame, exactly
2 `PrimsDirtied` calls per frame — or (c) a lock-free `atomic<bool>` backstop on the first
`GetPrim` of a generated prim. Progressive generation uses `asyncAllow`/`asyncPoll`, with the
usdview plugin setting `_allowAsync` during `registerPlugins` (S20).

### 5.3 What a Description publishes

```
<Description>/__usdGenRender/
    curves_000 … curves_063        primType "basisCurves"   (S27: 32–256, never 1, never 10^4)
    guides                         primType "basisCurves", purpose = guide
    guides/cvs                     primType "points"        (CV display, S40)
    inst_<opName>                  primType "instancer"     (S33)
    inst_<opName>/Prototypes/<n>   re-rooted prototypes with instancedBy
```

The prim **set is allocated once at compile and never changes during interaction** (S27, S28) —
adding/removing prims bumps the rprim index version and forces a full dirty-list gather
(`G-storm-throughput` §2). Each chunk prim carries (S29): `points`, `widths` (vertex or constant,
never varying — varying costs a MEASURED 6.48 ms CPU expansion per 100 k curves), `hairT` (vertex
float), `hairTangent` (vertex vec3, object space), `hairId` (uniform), `st` (uniform float2, root
UV), `displayColor` (uniform), `minScreenSpaceWidths` (constant 1.0), `type=cubic basis=bspline
wrap=pinned`, **no `normals`**, `displayStyle/refineLevel = 2`, an `extent` recomputed every
deforming frame, blocked `velocities`/`accelerations`, `primOrigin{scenePath}`, and
`__dependencies` declared per chunk (32–256 edges, never per curve) (S30).

### 5.4 Locating the surface

```cpp
class UsdGenSurfaceBinding {
public:
    // Resolve usdGen:surface (a path array from the adapter) to concrete Hydra paths,
    // following: GeomSubset -> parent mesh; instance proxy -> prototype (S7);
    // native prototype -> the propagated path discovered from __usdPrimInfo (S34).
    SdfPathVector Resolve(const HdSceneIndexBaseRefPtr &input, const SdfPathVector &targets) const;

    // Rest + deformed sample, post ext-computation pruning (S3).
    bool Sample(const HdSceneIndexBaseRefPtr &input, const SdfPath &,
                float time, UsdGenSurfaceSample *out) const;
};
```

`Sample` reads through the private pruning wrapper so UsdSkel-skinned scalps (blocked
`primvars/points`, ext-computation points) work under Storm as well as hdPrman (S3), and it reads
rest points from `usdGen/rest/points` (S12), falling back to an authored `primvars:rest` (S12,
A DCC convention). Everything read is post-flattening: `xform/matrix` is world with
`resetXformStack=true`, and dirtiness is per prim, never hierarchical (S4).

### 5.5 Output spaces and invalidation

Chunk prims carry `xform = surface world matrix, resetXformStack = true` and **surface-local
points**, and re-dirty their own `xform` when the surface's is dirtied (S4). Any scene index that
overrides a prim's `primvars` container must dirty the bare `primvars` locator as well as its
leaves, or UsdSkel's resolved prim freezes (S5 — a measured usdRig/UsdSkel interop bug to file).
Never co-dirty `displayColor` with `points` (S30).

### 5.6 The adapter

```cpp
class UsdGenPrimAdapter : public UsdImagingSceneIndexPrimAdapter
{
public:
    TfTokenVector GetImagingSubprims(UsdPrim const &) override;              // { TfToken() }
    TfToken       GetImagingSubprimType(UsdPrim const &, TfToken const &) override; // "" (data only)
    HdContainerDataSourceHandle GetImagingSubprimData(
        UsdPrim const &, TfToken const &, const UsdImagingDataSourceStageGlobals &) override;
    HdDataSourceLocatorSet InvalidateImagingSubprim(
        UsdPrim const &, TfToken const &, TfTokenVector const &properties,
        UsdImagingPropertyInvalidationType) override;
};
```

plugInfo: `"primTypeName": "UsdGenOperator"`, `"includeDerivedPrimTypes": true` (S10, A4 §2.1), so
one entry covers every current and future operator type, including a TD's. Mappings are built
**generically from `UsdPrimDefinition::GetPropertyNames()`** — never from a hand-written superset,
because `UsdImagingDataSourceMapped::Get` posts a `TF_CODING_ERROR` for a property the prim does
not have (MEASURED, `G-stage-free` §2). Without the adapter, `usdGen:*` attributes and
relationships are invisible **and produce no notice at all**, including `usdGen:input` retargets
(MEASURED, S10) — this is the single fact that makes the adapter mandatory rather than optional.

Ramps: knot arrays are ordinary typed attributes. The `TsSpline` form is a custom
`AttributeMapping::factory` returning `HdRetainedTypedSampledDataSource<TsSpline>` and **not**
calling `FlagAsTimeVarying` (MEASURED, probe6) — otherwise every `SetTime` would dirty a static
ramp and force a full recook (MEASURED, `G-stage-free` §2.1).

### 5.7 Extents and framing

Two channels, both from usdRig's proven shape (A2 §6): the Hydra `extent` on every chunk prim
(cheap, recomputed with the points), and a `UsdGeomComputeExtentFunction` registered on
`UsdGenDescription` (Boundable) that reads the live generation only when it describes this stage
and time and otherwise falls back to the bound surface's extent. That is what makes "select the
description, press F" frame the hair instead of the origin.

---

## 6. D4 — Operator catalogue, v1 / v2 / v3

Reference: A7 §9. Cost classes: **A** = per-curve parallel; **B** = spatial structure at capture;
**C** = per-CV surface/BVH query. Space is the default `usdGen:space`.

### 6.1 v1 — "a groom you can ship"

| Type | Mode / key params (defaults) | Class | Space | Parity |
|---|---|---|---|---|
| `UsdGenScatter` | `mode = random\|uniform\|points\|atGuides`; `density = 100`, `seed = 0`, `relaxIterations = 0` (0–50), `areaCompensation = 1`, `spacingU/V = 0.1` (uniform), `rootPrims[]`/`rootUVs[]` (points) | A (relax B) | rest | a host groomer generator; a DCC Scatter |
| `UsdGenGrow` | `segments = 8`, `length = 1.0`, `lengthRandom = (1,1)`, `direction = normal\|attribute\|vector`, `lift = 0`, `uvBlend = 0` | A | rest | a DCC Guide Initialize |
| `UsdGenGuideSet` | `source = children\|scatter`, `density = 10`, `cvCount = 8`, `rebuild = 1` | A | rest | a host groomer "guides at 10 %" |
| `UsdGenGuideInterpolate` | `maxGuides = 3`, `influenceRadius = 5`, `influenceDecay = 2`, `maxGuideAngle = 90`, `blendInSkinSpace = 1`, `blendMethod = linearBlend\|extrudeAndBlend`, `useUniqueGuide = 0`, `randomizeGuide = 0`, `cvCount = 0` (0 = max of guides), `clumpCrossover = 0` | capture B, eval A | rest | a host groomer relative interpolation; a DCC Hair Generate; a host renderer 3-guide weights |
| `UsdGenCurveSource` | `rel usdGen:curves`, `resample = 0`, `cvCount = 0` | A | rest | reads frozen / imported / simulated curves (R4) |
| `UsdGenDeform` | `mode = rigidFrame\|pointDeform`, `twistAware = 1`, `lockRoots = 1`, `preserveShape = 0` | A | **deformed** | a DCC Guide Deform; a host renderer binding (R3) |
| `UsdGenClump` | `amount = 0.5` + `profile` ramp, `size = 1.0` \| `density`, `seed`, `stray:amount/rate/falloff = 0/0/1`, `volumize = 0`, `noise:amount/frequency/correlation = 0/1/0`, `levels = 1`, `sizeReduction = 0.5`, `tightnessReduction = 0.8`, `method = linearBlend\|extrudeAndBlend`, `preserveLength = 100` | capture B, eval A×levels | rest | a host groomer Clumping; a DCC Hair Clump 2.0 |
| `UsdGenNoise` (frizz) | `magnitude = 0.05` + ramp, `frequency = 3`, `correlation = 0.5`, `octaves = 1`, `lacunarity = 2`, `gain = 0.5`, `space`, `preserveLength = 1` | A | rest | a host groomer Noise; a DCC Frizz |
| `UsdGenLength` | `mode = set\|add\|multiply\|cutAbsolute\|cutRelative`, `value = 1`, `valueRandom = (1,1)`, `method = scale\|cutExtend`, `rebuild = keepParam\|reparam`, `cullThreshold = 0` | A | rest | a host groomer Cut; a DCC Set Length |
| `UsdGenWidth` | `width = 0.01`, `widthRamp`, `taper = 0`, `taperStart = 0.5`, `rootScale = 1`, `tipScale = 1`, `replace = 1` | A | rest | a host renderer root/tip scale; a host groomer Width Ramp |
| `UsdGenDirection` | `direction = (0,1,0)`, `amount = 0`, `lift = 0`, `mode = rigid\|perSegment`, `followSkinContour = 0` | A | rest | a host groomer Tilt U/V/N |
| `UsdGenFreeze` | §3.4 | — | — | a host groomer Groom Bake |
| `UsdGenSculptLayer` | §3.4 | A | rest | a host groomer sculpt layers |

Maps in v1: `UsdGenImageMap`, `UsdGenPaintMap`, `UsdGenExprMap`, `UsdGenCombineMap`,
`UsdGenNoiseMap`. Mask block (`UsdGenMaskAPI`) on every operator from day one — it is the parity
item, and retrofitting it later would change every operator's behaviour.

### 6.2 v2 — "a groom you can style"

| Type | Key params | Class | Space |
|---|---|---|---|
| `UsdGenCurl` | `radius` + ramp, `frequency = 2`, `phase`, `phaseRandom`, `taper = 1`, `axisMode = curveTangent\|guide`, `clockwise = 1` | A | rest |
| `UsdGenBend` | `angle = 0`, `angleRandom`, `axisMode = rootDirection\|uniform\|attribute`, `axis`, ramp | A | rest |
| `UsdGenSmooth` | `strength = 0.5`, `iterations = 1`, `mode = alongCurve\|neighbours`, `searchRadius`, `numNeighbors`, `lockRoot = 1` | A / B | rest |
| `UsdGenStraighten` | `tangentStraightness = 0`, `normalStraightness = 0` | A | rest |
| `UsdGenDisplace` | `amount = 0`, `base = 0.5`, `scale = 1`, `offset = 0`, map input | A | rest |
| `UsdGenWave` | `frequency`, `amplitude` (tangent + normal) | A | rest |
| `UsdGenResample` | `cvCount`, `mode = uniform\|curvature` | A | rest |
| `UsdGenInstance` | §3.9 | A | deformed |
| `UsdGenPtexMap` | §3.5 | — | — |
| `UsdGenExprOp` | `source`, `returnType = displacement\|width\|color`, `domain = cv\|curve` | A | authored |
| `UsdGenGuideProximityMap` | §3.5 | B | rest |

v2 also adds: ramps as `TsSpline` (S11), the P1 velocity motion profile (S32), sculpt-layer
**rebase**, region/parting maps on `UsdGenGuideInterpolate`.

### 6.3 v3 — "a groom that interacts with the world"

| Type | Key params | Class |
|---|---|---|
| `UsdGenCollide` | `target = skin\|colliders\|sdf`, `offset`, `pushRange`, `pushAmount`, `iterations`, `resolveType = flexible\|stiff`, `lockRoots` | C, deformed |
| `UsdGenWind` | `direction`, `constStrength`, `gustStrength`, `shearStrength`, `shearFreq`, `stiffness` + ramp, `seed`, `loopFrames` | A, deformed |
| `UsdGenBraid` | `strands = 3`, `radius`, `frequency`, `flare` — topology ×3 | A |
| `UsdGenPart` | parting lines from a curve set: `radius`, `strength` | B |
| `UsdGenSimSource` | reads a sim cache and re-times it | A |

Also v3: P2 sampled motion blur across the whole catalogue, Ptex *writing* from the paint tool, an
OpenExec control-plane backend behind the same operator interface (S16 — the ExecUsd control plane
is not plugin-extensible in 26.08, so this stays a future adapter and curve arrays never go through
OpenExec).

### 6.4 The rules every operator obeys

1. Mask block on every operator; the resolved mask is a per-curve float array plus a 257-entry ramp
   LUT, computed once per capture.
2. Seeds are `hash(usdGen:seed, curveId, salt)` where `salt` is a per-operator constant, so two
   `UsdGenClump`s with the same seed do *not* produce correlated randomness.
3. `preserveLength` restores rest segment lengths root-locked after any displacement, as a host groomer and
   a DCC both do.
4. Every operator declares `Space()`. A `deformed`-space operator pushes itself and everything
   downstream into the per-shutter-offset tail (§4.8) — the stack editor shows a small motion icon
   on those rows so an artist can see what a render will cost.
5. Topology-changing operators (`Scatter`, `Grow`, `GuideInterpolate`, `Resample`, `Length` with
   `cull`, `Braid`) bump `topologyVersion` and force the chunk partition to be recomputed;
   everything else never does.

---

## 7. D5 — Look pipeline

### 7.1 The three terminals on one `Material`

S36: one `Material` prim carries three terminals, authored by the tool's "Create hair material"
action from a template that the artist then tweaks in usdview's property editor.

```usda
def Material "HairLook"
{
    token outputs:surface.connect        = </Char/Looks/HairLook/preview.outputs:surface>
    token outputs:glslfx:surface.connect = </Char/Looks/HairLook/storm.outputs:surface>
    token outputs:mtlx:surface.connect   = </Char/Looks/HairLook/mtlx/surface.outputs:out>

    def Shader "storm"                        # EXISTS: probes/storm-hair-look/usdGenHairPreview.glslfx
    {
        uniform token info:implementationSource = "sourceAsset"
        uniform asset info:glslfx:sourceAsset = @usdGenHairPreview.glslfx@
        color3f inputs:rootColor  = (0.035, 0.018, 0.008)
        color3f inputs:tipColor   = (0.210, 0.115, 0.045)
        float   inputs:colorRamp  = 1.0
        float   inputs:diffuseGain = 0.55
        float   inputs:diffuseWrap = 0.35
        float   inputs:specular1Gain = 0.30
        float   inputs:specular1Width = 0.075
        float   inputs:specular1Shift = -0.045
        float   inputs:specular2Gain = 0.16
        float   inputs:specular2Width = 0.22
        float   inputs:specular2Shift = 0.090
        float   inputs:transmissionGain = 0.25
        color3f inputs:transmissionColor = (1.0, 0.62, 0.38)
        float   inputs:randomHue = 0.0
        float   inputs:randomValue = 0.0
        float   inputs:widthFalloff = 0.0
        token   outputs:surface
    }

    def Shader "preview" { uniform token info:id = "UsdPreviewSurface" ... }   # reads displayColor / st
    def Scope  "mtlx"    { ... ND_chiang_hair_bsdf with an explicit world-space tangent geomprop ... }
}
```

**MEASURED** (S35, `G-storm-hair-look`): the glslfx parses in Sdr (20 inputs, `primvars` metadata
`hairId|hairT|hairTangent|st`), compiles and renders in Storm with zero warnings on this GB10;
`float[2]`/`float[4]` parameters bind fine; a bound material's tag beats `displayOpacity`, so two
files ship — `usdGenHairPreview.glslfx` (`defaultMaterialTag`, alpha-to-coverage: the default for
scalp hair) and `usdGenHairPreviewTranslucent.glslfx` (`translucent`, routed to `HdxOitRenderTask`:
fine fur and peach fuzz). Both are discovered from a plugin `shaderDefs.usda` as
`usdGen:HairPreview` / `usdGen:HairPreviewTranslucent`.

Three shader rules that are not negotiable: never read `inData` from the material (it hard-fails at
refineLevel 0 and on meshes — MEASURED); root→tip comes from the `hairT` **primvar**, never from
`patchCoord` (whose `v` runs *across* the width — MEASURED); the tangent comes from the
`hairTangent` vertex primvar with a screen-derivative fallback (the derivative-only path is visibly
sparkly on 1–2 px strands — MEASURED).

### 7.2 Which terminal Storm actually takes

S36 flags as **UNVERIFIED** whether Storm prefers a `glslfx:` render-context output over plain
`outputs:surface`. The design does not bet on it: `UsdGenHairSceneIndex` knows its
`rendererDisplayName` from `inputArgs` (S2), and **overlays the chunk prims'
`materialBindings` per delegate** — Storm gets the glslfx shader path, everything else gets the
plain `outputs:surface`/`mtlx` binding. If the render-context resolution turns out to work, the
overlay becomes a no-op and is deleted. Test gate: `test_usdgen_material_binding` asserts the bound
terminal per delegate name.

### 7.3 Colour, maps, expressions

All map/Ptex/SeExpr evaluation is **CPU at capture time**, baked to per-curve or per-CV primvars —
colour, density, length, masks, widths (S37). Storm textures are used only for UV-mapped scalp
colour through the uniform `st` root-UV primvar, which is **MEASURED** to work: a
`UsdPrimvarReader_float2` on uniform `st` into a `UsdUVTexture` gives a per-curve root-UV lookup
(S37, `G-storm-hair-look` §4). Consequences an artist notices: a painted density map affects
generation (CPU, capture); a painted *tint* map can either be baked to `displayColor` (works
everywhere, one value per strand) or wired as a Storm texture (per-strand, filtered on the GPU).
The tool's map editor offers both and says which.

Ptex: compiled out of this OpenUSD install and mesh-only in Storm's GLSL (S37), so usdGen ships its
own static Ptex 2.4.3 (S38) and uses it **only** on the CPU. Face ids come from
`Far::PtexIndices::GetFaceId` on a `TopologyRefiner` built by `PxOsdRefinerFactory::Create`, and
`GetAdjacency` supplies exactly the `adjfaces`/`adjedges` a `PtexWriter` needs (A8 §2.6, §2.7).
Quads map 1:1; an n-gon occupies n consecutive face ids. **MEASURED:** 23 ns/lookup bilinear,
228 M lookups/s at 8 threads.

SeExpr: upstream `wdas/SeExpr` `main` @8f8c8f2, interpreter only, static and hidden, with `rand()`
added (S38). The a host groomer variable set an artist expects, bound through one `VarBlockCreator` and one
thread-safe `VarBlock` per TBB worker:

| Slot | Contents |
|---|---|
| Variables | `$u $v` (root surface uv), `$id`, `$faceId`, `$P $N $dPdu $dPdv` (deformed), `$Pref $Nref` (rest), `$t` (0 root → 1 tip, `cv` domain only), `$frame`, `$cLength $cWidth`, `$descId` |
| Functions | SeExpr's builtins plus `map("name" [,s,t] [,channel])`, `ptex("name" [,faceId,u,v])`, `rand([min,max][,seed])` |
| Notes | SeExpr's `noise` is 0..1, a host groomer's is −1..1; usdGen keeps SeExpr semantics and ships `snoise` for the signed form, documented in the expression editor's help pane (A8 §1.8) |

`${DESC}`-style macros are pre-substituted before parse; asset paths resolve through the adapter's
already-resolved `SdfAssetPath` (S13 — resolution is free and stage-free).

### 7.4 Map reload and the paint round trip

Overwritten map files are **not** picked up automatically (S13). usdGen ships an explicit **Reload
maps** action (menu + `UsdGenImaging_ReloadMaps()`), which bumps a texture generation counter and
invalidates every capture that consumed a map. `asset[]` is never used — one `asset` per map prim,
or a child prim per map — because asset arrays get no reload tracking (MEASURED, S13).

Paint round trip: brush → `primvars:usdGen:paint:<name>` on the mesh (live override per move,
one stage write at release) → optionally **Bake map** → `.ptx` (via `PtexWriter`, adjacency from
`Far::PtexIndices`) or EXR (via `HioImage`; the install has `hioOpenEXR` and `hioAvif` plus stb
formats, no OIIO — S38) → the `UsdGenPaintMap` gains `usdGen:paint:bakedFile` and stops reading
the primvar. Nothing writes an image file during interaction.

---

## 8. D6 — Tooling: UX and architecture

### 8.1 Panels

Conventions copied verbatim from usdRig (S43): `PluginContainer` with state in an
`__init__`-initialised object, lazy Qt imports so every model module stays importable headlessly, a
shared undo stack, the **signal's** frame not `dataModel.currentFrame`, `UpdateViewport()` after
every edit and undo, one `_resetGUI` per batched freeze, a `USDGEN_IMAGING_DLL` override for
out-of-tree builds, tests that select the terminal SI by the `"[Terminal SI]"` prefix.

| Panel | Module | Contents |
|---|---|---|
| **Groom panel** | `groomPanel.py` (Qt) | description list; per-description density / interactive-LOD sliders; motion profile; "Create hair material"; **stack profiler** (ms per operator, §4.10); edit-target label (usdview defaults to the session layer — the volume panel's precedent) |
| **Stack editor** | `stackUI.py` (Qt) + `stackModel.py` (Qt-free) | the `Ops` scope as a reorderable list: enable checkbox, type icon, label, blend slider, mask badge, motion badge, stale-freeze badge; add/duplicate/delete; drag-reorder = one `reorder nameChildren` edit |
| **Brush shelf** | `brushUI.py` (Qt) + `brushKernels.py` (numpy, Qt-free) | the tools of §8.3, radius/strength/falloff, symmetry, "affect guides / affect hair" |
| **Map editor** | `mapUI.py` (Qt) | map prim list, expression editor with the variable/function help pane, bake and reload actions |
| **Freeze bar** | in the groom panel | Freeze at frame / Freeze range / Unfreeze / Rebase sculpt / Commit to layer (T1/T2/T3, S42) |

Every model module (`stackModel`, `brushKernels`, `freezeAuthor`, `usdGenUndo`) is Qt-free and
tested headlessly, exactly as usdRig's plans require (A3 §6.2 "Qt-free rule").

### 8.2 The transport (S39)

```c
/* usdGenImaging/registry.h — ctypes, control plane only, no arrays */
int       UsdGenImaging_Activate(long long stageCacheId, const char *groomRoot, double frame);
int       UsdGenImaging_SetTime(double frame);
int       UsdGenImaging_Commit(void);
void      UsdGenImaging_Deactivate(void);
long long UsdGenImaging_GetGeneration(void);
int       UsdGenImaging_BeginLiveOverride(const char *primPath);
int       UsdGenImaging_ClearLiveOverride(const char *primPath);
int       UsdGenImaging_PickCV(const char *primPath, const float viewProj[16],
                               int w, int h, float x, float y, float radiusPx,
                               int *outCurve, int *outCv, float *outDistPx);
int       UsdGenImaging_FootprintCV(const char *primPath, const float viewProj[16],
                                    int w, int h, float x, float y, float radiusPx,
                                    int *outIdx, int maxOut, int *outCount);
int       UsdGenImaging_ReloadMaps(void);
int       UsdGenImaging_SetInteractiveLOD(const char *descPath, int maxCurves);
int       UsdGenImaging_GetNodeStats(const char *opPath, double *outMs, long long *outCurves);
```

```c++
// python/_usdGenImaging.cpp — pxr_boost.python, arrays only
PXR_BOOST_PYTHON_MODULE(_usdGenImaging)
{
    bp::def("ReadCurvePoints",        &ReadCurvePoints);   // (path) -> VtVec3fArray   0.13 us
    bp::def("ReadCurveCounts",        &ReadCurveCounts);   // (path) -> VtIntArray
    bp::def("ReadCurveWidths",        &ReadCurveWidths);
    bp::def("ReadCurveIds",           &ReadCurveIds);
    bp::def("SetLiveOverride",        &SetLiveOverride);        // (path, VtVec3fArray) 0.14 us
    bp::def("SetLiveOverrideIndexed", &SetLiveOverrideIndexed); // (path, VtIntArray, VtVec3fArray) 1.2 us / 2k
    bp::def("SetPaintOverride",       &SetPaintOverride);       // (meshPath, name, VtFloatArray)
    bp::def("GetGeneration",          &GetGeneration);
}
```

MEASURED (S39): `VtArray` crosses in O(1) (0.13 µs at 1 M CVs); the whole per-move Python cost of a
stroke is **21 µs** (numpy gather + displace 16.6 µs, sparse ctypes push 4.1 µs) — 0.13 % of a
16.7 ms frame. Gotchas that must be in the code: `from pxr import Vt, Gf` before the first module
call (the Vt converters live in `pxr/Vt/_vt.so`); never `Vt.*Array.FromBuffer` or
`attr.Set(ndarray)` on groom-sized arrays (483 µs vs 2.4 µs); pin BLAS threads to 1 at import
(a `(100k,4)@(4,4)` matmul measured 22 ms unpinned under load vs 0.56 ms pinned);
`Usd*.Define()` cannot run inside `Sdf.ChangeBlock`.

### 8.3 The brushes, and the loop each one runs

Every brush follows the same four-phase contract (S40): **press** captures a base and opens an
undo bracket; **move** recomputes from the press-time base and pushes a live override — no stage
traffic; **release** writes once inside one `Sdf.ChangeBlock` and clears the override;
**escape** aborts. Recomputing from the base (never incrementally) is what makes a stroke
idempotent and abortable (usdRig's `gizmoDrag` rule).

| Brush | Target | What move computes | Commits to |
|---|---|---|---|
| **Comb** | guides (or hair with `affectHair`) | rotate CVs about the root frame toward the stroke direction, falloff by surface distance and `hairT` | `points` of the guide prims, or a `UsdGenSculptLayer`'s deltas |
| **Grab** | guides | translate CVs inside the footprint, root-locked | same |
| **Smooth** | guides | Laplacian along the curve, `strength × falloff` | same |
| **Length** | guides / hair | scale arc length, `mode = grow\|cut` | guide `points`, or `usdGen:sculpt` |
| **Cut** | hair | mark curves shorter, writes a length delta | `UsdGenSculptLayer` |
| **Clump** | hair | paints a clump id / clump amount per curve | a `UsdGenPaintMap` feeding `usdGen:clump:amount`'s mask |
| **Density paint** | surface | accumulate a faceVarying/vertex float under the cursor | `primvars:usdGen:paint:density` on the mesh |
| **Map paint** | surface | same, on any named channel; colour or scalar | `primvars:usdGen:paint:<name>` |
| **Place guide** | guide set | plant a new guide at the surface hit, initialised from the current hair | a new `BasisCurves` under `Guides` (a resync — done on release only, never per move) |
| **Freeze paint** | hair | mark curve ids as frozen | `usdGen:sculpt:freezeMask` |

Per-move mechanics that the evidence pins down:

* **Picking** is CPU in C++: 166 µs for the nearest CV at 100 k CVs, 1.66 ms at 1 M single-threaded
  (MEASURED, S40) — 8× cheaper than one `view.pick()` (1.3–1.42 ms). Above ~300 k CVs the tool uses
  the root pre-filter (nearest root, then CV within that curve). The camera is resolved once per
  drag, never per move (`resolveCamera()` emits `signalFrustumChanged`).
* **CV display** is a synthesized `points` child prim with `widths` and `displayColor`
  (S40) — chosen over the `displayStyle:reprSelector` points slot because dot size matters for a
  comb tool. CV *highlight* through Hydra selection is not reachable from usdview (`HdSelectionSchema`
  has no point indices, MEASURED), so selection state is encoded in `displayColor`.
* **A primvar appearing** (turning on a brush falloff overlay) needs a `primvars/<name>` dirty
  once; per-move it is `primvars/<name>/primvarValue` only (MEASURED, `G-tool-loop` §5).
* **Never create or remove prims per move.** Overlays are defined once and toggled with
  `visibility`; the "Place guide" brush defines its prim on release.

### 8.4 The live-override path, end to end

```
press   picker.Pick(x_phys, y_phys)                      # 1.3-1.4 ms, once
        base = _usdGenImaging.ReadCurvePoints(prim)      # 0.34 us, zero-copy np.asarray view
        recorder = usdGenUndo.EditRecorder(stage, attrs).Begin()
        lib.UsdGenImaging_BeginLiveOverride(prim)
move    idx, disp = brushKernels.Comb(base, stroke, radius, falloff)     # 16.6 us numpy
        _usdGenImaging.SetLiveOverrideIndexed(prim, idx, disp)           # 1.2 us / 2k CVs
        api.UpdateViewport()
release with Sdf.ChangeBlock(): writer.Write(prim, final)                # 2.4 us
        undoStack.Push(recorder.Commit("Comb hair"))
        lib.UsdGenImaging_ClearLiveOverride(prim)
        api.UpdateViewport()
```

The C++ side publishes a generation with **only the touched leaves dirtied** (S40), so the
evaluator does not re-run and usdview does not rebuild its prim view. The stage is written exactly
once per stroke.

### 8.5 Undo

`SubtreeSnapshot` beside usdRig's `AttributeSnapshot` (S41): `Sdf.CopySpec` into an anonymous
stash, O(1) memory because `VtArray` is copy-on-write, **0.1–0.24 ms** MEASURED. Rules that came
out of the probes and must be in the code:

* Undo of a **live** freeze is `prim.SetActive(False)`; the spec is really removed only when the
  entry leaves the bounded stack — because anything that leaves no prim at a resynced path raises a
  spurious `Tf.ErrorException` under an attached OpenExec system (S41, `esfUsd/stageData.cpp:360`,
  MEASURED and isolated to stock OpenUSD).
* Every structural edit goes in one `Sdf.ChangeBlock`; `Define` happens outside it.
* An undo entry records its **layer**, and a freeze may only be undone in the layer it was authored
  into — with usdview's session-layer edit target, `RemovePrim` on a root-layer prim silently does
  nothing (MEASURED, `G-freeze-bake` §1.5).
* Removals are batched: N removals in one change block coalesce into one exception carrying N
  errors, contained by `try/except Tf.ErrorException` plus a re-assert that the prim is gone
  (`Tf.ErrorMark` is not bound in Python).

### 8.6 Freeze, bake and commit

| Action | Tier | Cost (MEASURED) |
|---|---|---|
| Freeze at current frame | T1 session layer | 0.4 ms author, 0.08 ms for 100 k CVs of attribute writing |
| Commit to sidecar | T2 `.usdc` sublayer | 10.6 ms author+save, 9.6 MB for 100 k × 8 CV; reopen 0.4–0.8 ms |
| Heavy asset | T3 `.usdc` payload | 21.5 ms compose (the price of being unloadable) |

Never `.usda` (368 ms save, 532 ms first `points.Get()` — a 660× reopen difference, MEASURED).
Never bake motion samples (24 point samples = 230 MB with no compression); author `velocities`
instead (19.2 MB). `primvars:rest` at freeze time is **free** (+26 bytes) as long as the freeze
hands crate the same `VtArray` object it hands `points` (MEASURED). Freezes are siblings under the
Description's `Frozen` scope; the tooling never re-authors that parent scope.

### 8.7 Hotkeys and status

`B` brush shelf, `[` / `]` radius, `Shift` = smooth, `Ctrl` = invert, `F` freeze at frame,
`Alt+F` unfreeze, `G` toggle guide display, `D` toggle interactive LOD, `Ctrl+Z/Y` undo/redo.
Tools never claim `Alt`/`Meta` presses and return `False` when they have nothing under the cursor,
so usdview's camera and picking keep working (usdRig's rule, A3 §3).

Status line reports, in one row: `curves 187 432 · chunks 64 · eval 2.1 ms · gen 41 · LOD 40k ·
edit target: session`. The edit-target label is not cosmetic — usdview points the edit target at
the session layer by default and an artist who does not know that loses work.

### 8.8 Progressive feedback

Nothing an artist waits on is allowed to be a blank viewport. Three mechanisms, in increasing cost:

1. **Interactive LOD** (§3.6) caps the curve count while a tool is dragging and restores the full
   count on release. The status line says `LOD 40k` so the artist knows what they are looking at.
2. **Chunk-order progressive publish.** Chunks are published as they finish, in surface-locality
   order seeded from the camera, so a heavy re-generation fills in from the middle of the screen
   outward rather than appearing all at once. This is the `asyncAllow`/`asyncPoll` path (S20); the
   usdview plugin enables the 100 ms poll itself by setting `_allowAsync` during
   `registerPlugins`.
3. **Capture progress.** Long captures (kd-trees, 1 M-root Ptex/SeExpr) report through
   `UsdGenNodeStats` into the stack profiler row, which shows a progress fraction instead of a
   time while the capture runs. The previous generation stays on screen until the new one is
   complete — there is never a frame with half a groom.

### 8.9 testusdview coverage

| Script | Asserts |
|---|---|
| `testUsdviewUsdGenActivate.py` | activation, generation counter advances per frame, chunk prims present in the terminal SI, `primOrigin` resolves to the Description |
| `testUsdviewUsdGenStack.py` | add / reorder / disable an operator; terminal SI points change; one undo restores |
| `testUsdviewUsdGenComb.py` | press/move×N/release; exactly one stage write; the authored `points` equal the terminal SI's `primvars/points`; Ctrl+Z restores |
| `testUsdviewUsdGenFreeze.py` | freeze at frame N; authored `points` == SI points; unfreeze via `SetActive(False)`; the stack editor shows the upstream ops greyed |
| `testUsdviewUsdGenPaint.py` | paint density; the primvar appears with one `primvars/<name>` dirty; hair count changes on release only |
| `testUsdviewUsdGenCards.py` | instancer present, prototypes re-rooted, a click selects the Description |
| `testUsdviewUsdGenLook.py` | the bound terminal per renderer name; a framebuffer pixel-fraction check against a golden |

---

## 9. D8 — Library decomposition, naming and build

### 9.1 Targets

| CMake target | Kind | Contents | Links |
|---|---|---|---|
| `usdGenMath` | STATIC, PIC, `-ffp-contract=off` | pure kernels: strip/clump/noise/curl/bend/length/width kernels, RMF frames, arc-length tables, extent, envelope/blend, ramp LUTs, hash | `arch tf gf vt`, optional `rigExec::rigExecMath` |
| `usdGenThirdParty` | INTERFACE | wraps `SeExpr2_static`, `Ptex_static`, `nanoflann` (all static + `-fvisibility=hidden`) | `ZLIB::ZLIB`, `Threads::Threads` |
| `usdGenSchema` | resource | codeless `schema.usda`, generated `plugInfo.json` + `generatedSchema.usda` | — |
| `usdGen` | SHARED | `UsdGenGraph`, `UsdGenOp` + the whole catalogue, `UsdGenOpRegistry`, capture caches, map/Ptex/SeExpr evaluators, chunking, diagnostics | `usdGenMath usdGenThirdParty tf gf vt sdf hd work trace TBB::tbb` |
| `usdGenImaging` | SHARED | registry, session, scene index + plugin, prim adapter + API-schema adapter, publisher, generation store, live overrides, instancer synthesis, extent function, the C ABI | `usdGen hd hdsi hf usdImaging usdGeom usd usdUtils` |
| `usdGenShaders` | resource | `usdGenHairPreview.glslfx`, `usdGenHairPreviewTranslucent.glslfx`, `shaderDefs.usda`, the MaterialX hair template | — |
| `_usdGenImaging` | MODULE | pxr_boost.python array surface (S39) | `usdGenImaging usd_boost python` |
| `usdGenPy` | python package | `usdGen/` facade: `Groom`, `Description`, `Stack`, `Freeze`, strict schema authoring | — |
| `usdGenUsdview` | python plugin | the panels of §8.1 | — |
| `usdGenTest` | STATIC | EGL harness (`eglctx.h`, EXISTS), scene-index fixtures, golden-image compare | `usdGenImaging hdSt` |

`usdGenImaging` is the only library that knows about Hydra; `usdGen` is the only one that knows
about operators; `usdGenMath` knows about neither and is unit-testable in isolation. That split is
what makes the roadmap's phase gates meaningful.

### 9.2 CMake skeleton (S44)

```cmake
cmake_minimum_required(VERSION 3.26)
project(usdGen VERSION 0.1.0 LANGUAGES C CXX)
set(USD_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../OpenUSD_26_08" CACHE PATH "")
list(APPEND CMAKE_PREFIX_PATH "${USD_INSTALL_DIR}")     # pxrConfig's find_dependency() needs this
find_package(pxr REQUIRED CONFIG PATHS "${USD_INSTALL_DIR}" NO_DEFAULT_PATH)

find_package(rigExec CONFIG QUIET)                      # optional (S44)
if(TARGET rigExec::rigExecMath AND NOT TARGET TBB::tbb)
    # guard the measured CMake 3.28 double-pxrConfig trap
endif()

set(CMAKE_CXX_STANDARD 17)
add_compile_options(-ffp-contract=off)                  # S22/S45, measured necessary
set(CMAKE_INSTALL_RPATH "$ORIGIN")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
```

Third-party via `FetchContent` with an offline fallback `FETCHCONTENT_SOURCE_DIR_<NAME>` pointing
at `thirdparty/` (both libraries are already built at `scratchpad/thirdparty/install`), static and
hidden, headers and `.so`s **never** installed next to USD (a stray `libPtex.so` in `lib/` would be
picked up by a site's Ptex-enabled `libusd_hdSt.so` through `$ORIGIN` — A8 §6.2).

plugInfo generation copies usdRig's pattern verbatim (S44): `"LibraryPath"` =
`$<TARGET_FILE_NAME:usdGenImaging>`, generated into `<build>/usd/<name>/resources`, so the same
relative hop works in build and install trees; the codeless schema's plugInfo is rewritten from
`"Type": "resource"` to `"Type": "library"` so `Plug` can dlopen it for the compute-extent
function. `usdGenConfig.cmake` guards `if(NOT TARGET usd)`.

### 9.3 Test harness tiers (S45)

| Tier | What | Where it runs |
|---|---|---|
| 1 | Headless scene-index tests over the real `UsdImagingCreateSceneIndices` chain, sub-100 ms, no GL — the primary regression suite | this host |
| 2 | Storm correctness and GPU timing headlessly via the **EGL device-platform context** (`probes/storm-hair-look/eglctx.h`, EXISTS, renders on the GB10); fallback Xvfb+llvmpipe on `DISPLAY=:77` for CPU-only numbers | this host |
| 3 | `testusdview` scripts for the app loop (§8.8) | this host, on `:77` |
| 4 | Workstation protocols: MSAA quality, Metal/Vulkan Hgi path, non-NVIDIA drivers, instancer scaling (`G-instancing` §9), multi-prim Sync cost | elsewhere, documented |

Budget anchor: usdRig costs ~1.35 ms/frame on `ArmShotAnim`, leaving **~15 ms/frame for usdGen at
60 fps** (S45).

---

## 10. D7 — Roadmap

Estimates are engineer-weeks for one full-time engineer, and assume the prototypes listed in each
phase are carried into the repo rather than rewritten.

### Phase 0 — Skeleton and the chain (2 weeks)

Deliverables: repo, CMake, `usdGenMath` + `usdGen` + `usdGenImaging` targets, codeless schema with
`UsdGenGroom`/`UsdGenDescription`/`UsdGenOperator`/`UsdGenScatter`/`UsdGenGrow`, the prim adapter,
the metadata-only UsdImaging plugin, the renderer-level scene index publishing **one** hard-coded
chunk of straight hairs on a static mesh.

De-risked already: chain placement at phase 0 `InsertionOrderAtEnd` (MEASURED, S1); codeless schema
registration with zero C++ (MEASURED); adapter mappings from `UsdPrimDefinition` (MEASURED).

Exit criteria / test gates:
* `test_usdgen_chain_order`: with `usdSkelImaging` and `rigExecImaging` loaded, usdGen's index sits
  strictly after the whole UsdImaging chain and before every Storm plugin. **Loud on failure.**
* `test_usdgen_adapter`: every `usdGen:*` property of every registered type appears under
  `usdGen/...`, and editing one emits exactly one nested locator.
* `test_usdgen_curves_valid`: `points.size() == Σ curveVertexCounts` asserted by the publisher;
  `type/basis/wrap` authored; no `normals`.
* EGL: a 720p render of 4 k straight curves is non-empty and matches a golden within tolerance.

### Phase 1 — The engine and the interactive loop (4 weeks)

Deliverables: `UsdGenGraph` (TBB DAG, chunking, per-node buffers, capture/evaluate split), the
deferred-commit/atomic-publish/diffed-dirties machinery, `UsdGenGuideSet`,
`UsdGenGuideInterpolate`, `UsdGenClump`, `UsdGenNoise`, `UsdGenLength`, `UsdGenWidth`,
`UsdGenDirection`, the mask block, knot ramps, the C ABI + pxr_boost module, the groom panel and
stack editor.

Carried in: `probes/data-plane-engine-prototype-benchmark/tbbBench.cpp` (the DAG shape and its
numbers), `probes/tool-loop/{cTransport,bpTransport}.cpp` (the two transports),
`probes/evalsched/*` (the commit-point measurements).

Exit criteria:
* 100 k curves, 5-op chain, full run ≤ 2.5 ms; 1 % sparse edit ≤ 0.1 ms (MEASURED prototype floors
  are 1.72–1.91 ms and 0.035–0.044 ms).
* Exactly 2 `PrimsDirtied` calls observed per frame change; zero cooks inside `GetPrim` other than
  the atomic backstop; zero notices emitted from `GetPrim`.
* `test_usdgen_publish_race`: 8 reader threads × 20 publishes, 0 torn reads.
* A slider drag on `usdGen:clump:amount` re-runs only that node and its tail (assert via
  `UsdGenNodeStats`).

### Phase 2 — Look, maps and expressions (3 weeks)

Deliverables: `usdGenShaders` with both glslfx files and `shaderDefs.usda`, the three-terminal
material template + "Create hair material", per-delegate binding overlay, `UsdGenImageMap`,
`UsdGenPaintMap`, `UsdGenExprMap`, `UsdGenCombineMap`, `UsdGenNoiseMap`, the SeExpr variable/
function set, Reload maps, the map editor.

Carried in: `probes/storm-hair-look/usdGenHairPreview.glslfx` (EXISTS, renders), `eglctx.h`,
`thirdparty/install` SeExpr + Ptex builds.

Exit criteria:
* Golden-image tests through the EGL harness for opaque and translucent looks, root/tip ramp,
  per-curve jitter and a scalp colour map through uniform `st`.
* `test_usdgen_material_binding` asserts which terminal each delegate name resolves to (closes
  S36's UNVERIFIED item either way).
* A 1 M-root SeExpr density expression captures in ≤ 150 ms and never re-evaluates on a frame
  change.

### Phase 3 — Freeze, sculpt and the brushes (3 weeks)

Deliverables: `UsdGenFreeze`, `UsdGenCurveSource`, `UsdGenDeform`, `UsdGenSculptLayer`,
`SubtreeSnapshot` undo, the T1/T2/T3 landing tiers, comb/grab/smooth/length/cut/density-paint/
map-paint/place-guide brushes, CV display, CPU CV picking.

Carried in: `probes/freeze-bake/*` (the whole contract and its costs),
`probes/tool-loop/bench_pick_cpp.py` (the pick kernel).

Exit criteria:
* Freeze → comb → restyle → unfreeze round-trips with ids preserved; a stale epoch is detected and
  badged rather than silently misaligned.
* A comb stroke costs ≤ 40 µs of Python per move and writes the stage exactly once.
* `RemovePrim` never appears in an interactive path; undo of a live freeze is `SetActive(False)`.
* `testUsdviewUsdGenComb.py` and `testUsdviewUsdGenFreeze.py` pass on `:77`.

### Phase 4 — Instancing: cards, archives, native instances (2 weeks)

Deliverables: `UsdGenInstance`, instancer synthesis with re-rooted prototypes and hand-authored
`instancedBy`, prototype pruning, `instance`-interpolated variation primvars, native-instance
support via `__usdPrimInfo` discovery, `InstanceDataSourceNames`/`ProxyPathTranslationDataSourceNames`.

Carried in: `probes/instancing/instProbe{1,3,4}.cpp` (all four questions already answered).

Exit criteria: a click on a card selects the Description (primOrigin, relative inside prototypes);
moving cards dirties `primvars/hydra:instanceTranslations` only; hair on an 8×-instanced scalp
produces one prototype rprim, not eight.

### Phase 5 — Scale, motion blur and render parity (3 weeks)

Deliverables: P1/P2 motion profiles, the sample cache, interactive LOD ladder and density-scrub
trick, chunk partition by surface locality, `usdrecord`/hdPrman parity pass, `usdGen:cache:budgetMB`.

Exit criteria:
* 1 M curves render in `usdrecord` with a Storm and (where available) an hdPrman-class delegate
  from the same binary, no stage access downstream of the stage scene index (S8).
* A density drag at 200 k curves holds element counts fixed and commits the real count on release.
* P2 with 3 samples costs ≤ 1 × head + 3 × tail, verified through `UsdGenNodeStats`.

### Phase 6 — v2 catalogue and TD extension (4 weeks)

`UsdGenCurl`, `Bend`, `Smooth`, `Straighten`, `Displace`, `Wave`, `Resample`, `UsdGenExprOp`,
`UsdGenPtexMap`, `TsSpline` ramps, sculpt rebase, region/parting maps, the TD extension guide with
a worked out-of-tree operator example.

Exit criteria: an operator written outside the repo (schema snippet + 60 lines + macro + plugInfo)
loads, appears in the "Add operator" menu, and evaluates — with no change to usdGen's source.

**v3** (`Collide`, `Wind`, `Braid`, `Part`, `SimSource`, Ptex writing, an OpenExec control-plane
backend) is scoped after a production shakedown, not scheduled here.

---

## 11. Risks, stop conditions and bugs to contain

| # | Risk | Likelihood | Impact | Mitigation / stop condition |
|---|---|---|---|---|
| R-1 | Storm prefers plain `outputs:surface` over the `glslfx:` render context (S36 UNVERIFIED) | medium | look pipeline detour | The per-delegate binding overlay (§7.2) makes this a no-op either way. Decided in Phase 2 by one test |
| R-2 | `RemovePrim` under OpenExec raises (S41/S46, stock OpenUSD bug at `esfUsd/stageData.cpp:360`) | certain | breaks any tool that deletes | Never `RemovePrim` interactively; `SetActive(false)`; contain with `try/except` + re-assert. File upstream |
| R-3 | usdRig's `RigExecResultsSceneIndex` does not dirty the bare `primvars` locator (S5/S46) | certain | UsdSkel prims freeze | usdGen dirties the bare locator on everything it overrides; file against usdRig |
| R-4 | Multi-prim Storm Sync cost is unmeasured (all Storm numbers used one prim) | high | the 32–256 chunk choice could be wrong | Phase 1 EGL benchmark at 1/32/128/512 prims before committing the default; the chunk count is a Description property, so a wrong default is a re-tune, not a redesign |
| R-5 | 26.08's `overridesSceneIndexCallback` slot is unreachable from stock usdview (`G-instancing` open question) | high | no "generate upstream of propagation" path | S33 already chose full downstream synthesis, which is proven; the upstream path is an optimisation only |
| R-6 | Capture cost dominates on huge grooms (kd-trees + SeExpr + Ptex at 1 M roots ≈ 25–150 ms) | medium | first-edit latency | Capture is parallel and cached by epoch digest; the progressive path (S20, `asyncAllow`) shows partial results; the stack profiler shows the artist which operator is paying |
| R-7 | Per-node buffers at 1 M curves cost 668 MB (MEASURED) | medium | memory on modest workstations | `usdGen:cache:budgetMB` disables caching on cheap nodes; the single-buffer mode costs 293 MB with a slower first edit |
| R-8 | Ptex not in the OpenUSD install; a site's own Ptex may collide | low | ABI crash | Static + hidden, never installed beside USD (A8 §6.2) |
| R-9 | A pxr_boost module is pinned to the USD build (mangled `pxrInternal_v0_26_8`) | certain | rebuild per USD version | The ctypes C ABI stays the version-tolerant surface; the module is optional and its absence degrades tools, not correctness (S8's discipline applied to Python) |
| R-10 | Sculpt deltas orphaned by a topology change | medium | lost comb work | `frozenEpoch` staleness detection + a rebase action; deltas for missing ids are kept, never dropped |
| R-11 | Artists reorder the stack by renaming instead of `reorder nameChildren` | medium | confusing chains | The tool always authors `usdGen:input`; names are cosmetic. The implicit-sibling fallback only applies when no relationship is authored |

**Stop conditions.** (1) If the Phase 1 gate cannot reach ≤ 2.5 ms for a 5-op 100 k-curve chain,
stop and re-examine chunking before adding operators — every later phase assumes that floor.
(2) If the Phase 4 native-instance test cannot produce one prototype rprim for an 8×-instanced
scalp, cut per-instance groom variation from v1 and document it. (3) If the Phase 0 chain-order
test ever fails after an OpenUSD upgrade, nothing else may ship until it passes — everything in
this design assumes usdGen runs after every deformer.

---

## 12. Out of scope (deliberately)

Simulation (usdGen consumes sim caches through `UsdGenCurveSource`, it does not run a solver);
A DCC bridge; GPU evaluation of the curve chain (the data plane is CPU; Storm only draws);
per-instance material binding (Storm has none — S33); XPD/Alembic import (a converter, not a
plugin feature); authoring by the evaluator (R1: nothing the evaluator computes is ever written to
the stage except through an explicit freeze/bake action).

---

## Appendix — evidence index for this proposal

| Claim class | Source |
|---|---|
| Chain placement, notices per frame, commit points | brief S1–S2, S17–S20; `G-chain-order`, `G-evaluation-scheduling` |
| Stage-free parameters, ramps, rest points, registry | S8–S16; `G-stage-free` §1–7 |
| Engine shape, chunking, SoA, memory | S21–S26; `G-data-plane` §0, §4–8 |
| Curve contract, prim granularity, invalidation, Storm timings | S27–S32; `G-storm-throughput` §2, `G-storm-hair-look` §5 |
| Instancing | S33–S34; `G-instancing` §0–8 |
| Look, maps, Ptex, SeExpr | S35–S38; `G-storm-hair-look` §2, §7; A8 §1–4 |
| Tools, transport, picking, paint, freeze, undo | S39–S43; `G-tool-loop`, `G-freeze-bake`, A3 §7–8 |
| Build, deps, tests | S44–S45; `B-usdrig-build`, A8 §6 |
| Operator vocabulary and prior art | A7 §1–9 |
| Prototypes carried into the repo | `probes/storm-hair-look/*`, `probes/data-plane-engine-prototype-benchmark/*`, `probes/tool-loop/*`, `probes/freeze-bake/*`, `probes/instancing/*`, `probes/G-stage-free/*` |
