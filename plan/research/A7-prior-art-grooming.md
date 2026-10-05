# A7 — Prior art for a USD/Hydra grooming plugin: a host groomer, a host-renderer groom, host-application hair nodes, RenderMan and host-renderer hair

Researcher report for the `usdGen` architecture plan. Every non-trivial claim carries either an absolute `file:line` citation into `<openusd-src>` (tag v26.08) / `$USD` (install), or a URL to the vendor documentation that was fetched during this session. Claims that could not be fetched are marked **UNVERIFIED**. Where vendor pages were partially returned (the host vendor/the host vendor pages are JS-heavy and several returned 503/404), the partial extraction is stated as such.

Conventions used below: `t ∈ [0,1]` is normalized arc-length along a curve (root = 0, tip = 1); `P[i]` are CVs; `N_s`, `T_u`, `T_v` are skin normal / tangents at the root; `mask(h)` is a per-curve scalar weight; `ramp(t)` is a per-CV scalar.

---

## 0. Executive summary (what to copy, what to avoid)

| Design decision | Recommended source | Why |
|---|---|---|
| Node/operator model | a DCC SOP grooming (Guide Process / Hair Clump / Hair Generate) + a host groomer IGS modifier stack | Both are explicit, ordered, per-operator masked, and separate *guides* (sparse, editable) from *hair* (dense, generated). A DCC's parameter vocabulary (Blend, Curve Mask ramp, Noise Mask, override-by-attribute/texture) is the most complete. |
| Scatter + stable ids | a host groomer generator (Poisson random / uniform rows-cols / specified points / at guides / from XPD, `Generator Seed`) | Stable seed → stable ids across edits; density × mask; per-face/ptex masks. |
| Guide interpolation | a host groomer "relative interpolation" (per-guide range-of-influence, normalized weights, guides rebuilt to a common CV count, per-guide `Blend`) + a DCC's `Influence Radius`/`Influence Decay`/`Max Guide Count`/`Max Guide Angle`/`Clump Crossover` + a host renderer's "Use Unique Guide"/"Randomize Guide" | Gives an artist-tunable, cheap (kd-tree, N nearest) scheme with parting-line/region support. |
| Multi-level clumping | a host groomer classic `Clumping` (point maps `.xuv`, second map at 2× density) and a DCC `Hair Clump 2.0` (fractal iterations with size/tightness reduction + goal feedback, stray, curling before clumping) | Covers the "curve generator → clump → generator → clump → frizz" chain in the user request. |
| Masks | a DCC triad: **Skin mask** (group/paint/texture), **Curve mask** (ramp with Range/Effect Position/Falloff), **Noise mask** (freq/gain/bias) + a host groomer `map()`/SeExpr expressions | Uniform across every operator; cheap to evaluate. |
| Expressions | SeExpr (a host groomer) globals + function set | Matches the user's SeExpr2 requirement; global variables list captured in §1.3. |
| Deforming with a surface | a DCC Guide Deform (`rest` on skin & guides; `skinprim`/`skinprimuv`; Cosserat "Preserve Shape" pass) and a host renderer binding (root→triangle + RBF samples ≤100) | Exactly the "load static curves and deform with a deforming surface" requirement. |
| Freeze/sculpt | a host groomer IGS Sculpt modifier with weighted **sculpt layers**, a host groomer `Groom Bake` (XPD cache that deactivates upstream modifiers), a DCC Guide Groom `Clear`, a DCC "Apply modifier" | Layer = delta on top of a frozen upstream result; upstream recompute invalidates the layer unless ids are stable. |
| Shading | MaterialX `chiang_hair_bsdf` + `deon_hair_absorption_from_melanin` + `chiang_hair_roughness` (present in the 26.08 install with GLSL implementations) with PxrMarschnerHair and a host-renderer hair shader's parameter names as the UI vocabulary | Storm has a `Curves.Fragment.Hair` shader stage but no hair BSDF; MaterialX is the only path to a "looks like the render" viewport. |

---

## 1. the host vendor's host groomer (classic "a host groomer Core" and Interactive Groom Splines)

### 1.1 Data model

* **Collection / Description / Patch.** A Collection holds Descriptions; each Description is bound to patches (faces of a mesh) and carries a Primitive Type, a Generator, and a modifier stack. ("a host groomer is a geometry instancer that lets you populate the surface of polygon meshes with an arbitrary number of primitives either randomly or uniformly placed." — Generator search summary, host application documentation).
* **Primitive types** (search summary of a host groomer Primitives attributes page, host application documentation):
  * **Splines** — "curves made up of two or more segments. Use Splines for fur, simple stiff shapes, and complex ribbon shapes like long or curly hair."
  * **Spheres** — sphere with length/width/depth.
  * **Cards** — "individual NURBS planes that attached to the patch at the plane pivot point."
  * **Archives** — "instances of Alembic-based geometry" via `.xarc`.
  * **Groomable splines** — "non-renderable spline primitives that you sculpt and position" with brushes; "as you sculpt the splines, a host groomer automatically updates a series Ptex maps for each attribute that you paint"; maps are "saved as PTEX files in the Description's groom folder"; **"Groomable Splines do not follow deforming or animated meshes."** (host application documentation).
* **Generator attributes** (fetched, host application documentation):

| Attribute | Doc text |
|---|---|
| Generate Primitives | "Randomly across the surface" = "Poisson distribution over the patch. This is most useful for fur, grass, and weeds."; "In uniform rows and columns" (parametric rows/cols, `Spacing`); "At specified locations" (Specify Points Tool); "At guide locations"; "From XPD File" (`Input Dir`) |
| Generator Seed | "Sets the procedural seed value. You can set Generate Seed to the same value in different Descriptions to generate primitives on the same points of the polygon surface." |
| Flip to other side of surface | grows on the back side |
| Density | "Controls the number of primitives created by a host groomer over a given area." |
| Mask | "This number is a multiplier on the Density. Use an expression or image map here to vary the density of primitives over parts of the surface." |
| Spacing | spacing for rows/cols |
| Compensate Normals | "Averages the growth normals for primitives to prevent visible faceting on curved surfaces." |
| Compensate for uneven parameterization / Create Parameterization Map | redistributes on meshes with mixed face sizes via a map |

* **Spline primitive attributes** (fetched, host application documentation): `Control using` (Attributes vs Guides), `Attr CV Count`, `Modifier CV Count` ("the number of Control Vertices (CVs) that each spline has available for the modifier"), `Uniform CVs` (reparameterize), `Length`, `Width`, `Width Ramp` (root→tip multiplier), `Taper`, `Taper Start`, `Tilt U`/`Tilt V`/`Tilt N`/`Around N` (orientation vs surface normal), `Bend Param[i]`/`Bend U[i]`/`Bend V[i]` (bend at parameter), `Display Width`, `Face Camera`, `Tube Shade`. In guide mode "Length scales primitive based on the guide length", "Width scales primitive based on the guide width" (search summary of same page).

### 1.2 Guide interpolation (a host groomer "relative interpolation")

Fetched from host application documentation

1. Active-guide selection: a host groomer computes "Voronoi edges that separate guide-defined regions"; guides are "eliminated based on curvature (that is, on wrong side of the model) and region maps".
2. Weights: "a host groomer determines weights for these guides by intersecting a ray formed from the guide through the primitive with the guides range-of-influence" — "the closer you are to the guide's range-of-influence boundary, the lower the weight"; weights "are normalized".
3. Shape: "each active guide is translated to the primitives location. If necessary, a host groomer rebuilds each guide to the same number of CVs ... For each CV, a host groomer uses the weighted average of the guides."
4. Per-guide `Blend` attribute "lets you enlarge the range-of-influence on your guides, causing them to blend with their neighbors" (e.g. 0.2 on the face, 0.8 elsewhere).
5. Failure modes: "bald patches" when guide regions don't overlap a surface area; extrapolation "returns inferior results" — surround primitives with guides.

Pseudocode equivalent (our reconstruction):
```
active = guides whose Voronoi cell (on the surface, region-map- and backface-filtered) touches root r
for g in active: w_g = dist(r, boundary of cell_g along ray g->r) ; w_g = w_g^blend_g ; normalize
hair(t) = r + Σ_g w_g * (G_g(t) - root_g) rotated by frame(root_g)->frame(r) [relative interpolation]
```

* **Guide animation** (fetched, host application documentation): `Use Animation`, `Cache File` ("Animates the guides using a cached curve animation from a host hair system"), `Live Mode`, `Create Hair System`, `Attach Hair System`. Production practice: disable Live Mode and point `Cache File Name` at an Alembic of the guides for batch render (a vendor knowledge-base summary in search results).

### 1.3 Expressions (SeExpr)

* A host groomer uses SeExpr-based expressions (search summary, host application documentation). Variables start with `$`; statements end with `;`.
* **Global variables** (fetched verbatim, host application documentation): `$aCount`, `$u`, `$v` (surface params), `$cLength`, `$cWidth`, `$cDepth` (final computed values), `$descId`, `$faceid`, `$frame`, `$id` (primitive id), `$objectId`/`$patchId`, `$P`/`$Pg`/`$Pref`/`$Prefg` and world-space `$Pw`/`$Pgw`/`$Prefw`/`$Prefgw`, `$dPdu`/`$dPdv` (+ `g`/`ref` variants), `$N`/`$Ng`/`$Nref`/`$Nrefg`, `$Cs`, `$As`. ("g" = without displacement, "ref" = reference/rest surface.)
* **a host groomer-specific functions** (fetched, host application documentation): `map("mapname" [, s, t] [, channel])` — "Evaluates the mapname at the current (u,v) or the provided (s,t)"; `rand([[min],[max]][,seed])`; `noise([x][,y][,z])` Perlin in [-1,1]; `fit(x,a1,b1,a2,b2)`; `clamp`, `smoothstep`, `linearstep`, `gaussstep`, `boxstep`, `remap(x,source,range,falloff,interpolation)`, `contrast`, `gamma`, `bias`, `component(x,y,z)`, `alignU/V/N([X])` ("Align the primitive with a given vector"), `guidesAttr(N)` ("Gets the host application attribute value for the attribute named N on the current guide"), `shadow(x)`, plus standard math/trig/vector (`dot`, `cross`, `norm`, `ortho`, `length`, `dist`, `angle`, `hypot`, `deg`, `rad`, `cbrt`, ...).
* **Full SeExpr function set** (fetched, https://wdas.github.io/SeExpr/doxygen/userdoc.html): noise family `noise/snoise/vnoise/cnoise/pnoise/perlin/sperlin/vperlin/cperlin/cellnoise/ccellnoise`, fractal `fbm/cfbm/vfbm/turbulence/cturbulence/vturbulence(v, octaves=6, lacunarity=2, gain=0.5)`, 4D variants `*4(v,t,...)`, `voronoi/cvoronoi/pvoronoi(v,type,jitter,fbmScale,...)`, selection `cycle(index,lo,hi)`, `pick(index,lo,hi,[weights])`, `choose(index,c1,c2,...)`, `wchoose(index,c1,w1,...)`, `hash(seed...)`, color `hsi/midhsi/rgbtohsl/hsltorgb`, ramps `curve(param,pos0,val0,interp0,...)` with interp codes `0=none,1=linear,2=smooth,3=spline,4=monotone`, `spline(param,y1..yn)`, `mix/compress/expand/invert`, vector `up/rotate/ortho`, `printf`, operators including `->` apply-chaining and `~` (1-x). Map path substitution examples: `map('${DESC}/paint/density')` style paths (the `${DESC}` variable resolves to the description's data path; ptex maps painted for Mask/Region live in sub-folders — search summary of host application documentation).

### 1.4 Classic modifier stack

Stack order: "The modifier at the bottom of the stack gets computed first. A host groomer uses its output as the input for the modifier above it" (host application documentation). Every modifier has a `Mask` ("Lets you control how the modifier affects primitives in specified areas of the surface ... load or an expressions or create a Ptex map"). Most have a root→tip "Scale" ramp per scalar parameter and `Bake Options` (`Mode` Live/Baked, `Bake Dir` — XPD).

| Modifier | Parameters (verbatim where fetched) | Source |
|---|---|---|
| **Clumping** | `Clump` ("amount of the clumping effect"), `Clump Scale` (root→tip ramp), `Clump Volumize` ("amount the clump primitives volumize prior to clumping"), `Copy` ("blending of the clumping effect between neighboring clumps"), `Copy Scale`, `Copy Variance` (per-primitive ±), `Cut` ("amount that each primitive in a clump is cut"), `Noise`, `Noise Scale`, `Noise Frequency` ("frequency of noise per clump"), `Noise Correlation` ("percentage of how much each clump's noise should correlate to its neighboring clumps"), plus `Mask, Frame, Flatness, Offset, Curl` with scale ramps; `Setup Maps` (clump point maps), `Preview Guides`, `Export Guides`, `CV Attr`, `Color Preview` | host application documentation |
| **Clump maps** | `Setup Maps` → Generate Clumping Maps: `Density` ("number of points created on the polygon mesh surface over a given area"), `Mask`, `Use Control Maps` (auto-on when a Region Map exists "which ensures that clumps cannot cross the boundaries defined by the Region Map"), `Generate` → `.xuv` point map. Multi-level: "when you create a second map, set the Density of the point map to twice the Density value you used for the first map." | host application documentation ; host application documentation |
| **Coil** | `Mask`, `Count` ("number of coils. This value is the product of multiplying Length and a frequency value"), `Count Scale`, `Radius` ("radius at the base of the curve"), `Radius Scale` | host application documentation |
| **Noise** | `Mask`, `Frequency` ("cycles per-unit length of the primitive"), `Magnitude` ("maximum distance that CVs can be displaced"), `Magnitude Scale`, `Correlation` ("amount that the effect of noise correlates between neighboring primitives"), `Preserve Length` (0–100 % blend to original length), Bake `Mode` Live/Baked, `Bake Dir` | host application documentation |
| **Cut** | `Amount` ("amount of the primitive to be cut, starting at the tip"), `Rebuild Type` = `Keep Param` (same CV count/spacing) / `Reparam` ("redistributing the CVs based on the new length") | host application documentation |
| **Mesh Cut** | cut at intersections with Alembic geometry | index page only |
| **Collision** | `Mask`, `Resolve Type` = `Flexible` ("Pushes the CVs inside a collision object onto the closest points on its surface") / `Stiff` ("Locates the first segment ... that intersects the collision object" and rotates it rigidly) / `Wire Flex` / `Wire Stiff` (moves along AnimWires), `Mesh Files` (Alembic collider), `Iterations` | host application documentation |
| **Control Wires** | `Mask`, `Magnitude`, `Magnitude Scale`, `Smoothness` ("amount of control each wire has on each primitive"; 0 = nearest wire), `Breakage` ("falloff of the interpolation by setting a falloff radius along the primitives from base to the tip"), `Live Mode`, `Wire File` (.abc), `Ref Wire Frame` | host application documentation |
| **AnimWires** | animate primitives with a DCC curves/NURBS (a host hair system) | index page |
| **Force** | `Mask`, `Stiffness`, `Stiffness Scale`; Directional (ellipsoid `Center`, `X/Y/Z Vec`, `Magnitude`, `Range` falloff), `Force` vector (default -1 Y = gravity), Sphere force (`Center`, `Radius`, `Magnitude`, `Falloff`) | host application documentation |
| **Wind** | `Mask`, `Stiffness`, `Direction`, `Const Strength`, `Gust Strength`, `Shear Strength`, `Shear Freq`, `Seed`, `Pref Noise` (stabilizes noise on deforming meshes), `Low S/Low Bias/Hi S/Hi Bias/Hi Freq`, `Rigid Body`, `Clump` (needs clump map), `Loopable`/`Loop Start/End Frame`/`Loop Noise Span` | host application documentation |
| **Preserve Clumps** | `Mask`, `Magnitude`; keeps clump shapes on deforming meshes using an internal anim wire; not with AnimWires | host application documentation |
| **Groom Bake** | "save information output from a modifier or chain of modifiers to an XPD cache file"; "deactivates all modifiers below it"; "modifiers that follow the Groom Bake in the chain remain active, and read the data from the XPD file"; one per Description; avoid for huge counts | host application documentation |
| Snapshot / Debug / Particle / Plane & Block Animation | bake primitive info to files; debugging; particle-driven; animation helpers | index page |

### 1.5 Interactive Groom Splines (IGS)

* Node type `xgmSplineDescription`; brushes ("sculpting brushes, modifiers, and sculpting layers"); "a DCC processes the effects of modifiers from the bottom of the stack to the top" (host application documentation).
* **Modifier nodes** (same page): `xgmModifierSculpt` (sculpt layers, per-layer weight 0–1), `xgmModifierScale` (global length), `xgmCurveToSpline` (curves/Alembic → hairs, guides, wires), `xgmModifierClump`, `xgmModifierCollide`, `xgmModifierCut`, `xgmModifierDisplace`, `xgmModifierGuide`, `xgmModifierLinearWire`, `xgmModifierNoise`, `xgmModifierSplineCache` (Alembic playback). (No Coil/Bend/Freeze/Width/Direction *modifier nodes* exist; those are brushes or description attributes — **Freeze** is a brush per the tool list search; Width/Taper live on `descriptionShape`.)
* **IGS Clump** (host application documentation): `Clump` ("pulls surrounding hairs closer to the clump's center"), `Clump Scale` ramp, `Clump Volumize`, `Clump Variance` (push hairs toward nearby clumps), `Preserve Length` (0 stretch → 100 keep); **Clump Points**: `Randomness`, `Density` ("number of clump points per unit area"), `Density Mask`, `Seed`; **Clump Map**: `Auto Update`, `Radius Variance`, `Map Subdivision Level`, `Use Control Map`, `Control Using`, `Custom Map`, `Control Mask`, `Update Clump Map`; **Advanced Shape**: `Flatness`, `Offset`, `Curl`, `Orient` (+ ramps); **Secondary**: `Copy`, `Copy Variance`, `Cut`, `Noise`, `Noise Frequency`, `Noise Correlation` (+ ramps). Search summary adds `Noise Ramp` ("more noise at the hair root and less at the tips") and `Clump Ramp`.
* **IGS Cut** (host application documentation): `Cut Mode` Absolute/Relative, `Amount` (world units from tip), `Percentage`, `Min Remaining Length`, `Redistribute CVs` (on: CVs redistributed; off: "CVs collapse to the cut point").
* **IGS Noise** (host application documentation): `Frequency` (cycles per unit length), `Correlation`, `Preserve Length`, modes `Live` / `Baked` (Alembic) + `Bake Noise Data`.
* **IGS Displacement** (host application documentation): `Displace`/`Vector Displacement` map inputs, `Coordinate System` (a DCC XYZ / a sculpting application XZY), `Scale`, `Base` (0.5 = gray does nothing), `Offset` (distance from surface), `Bump`.
* **IGS Guide / Linear Wire** (host application documentation): Guide: `Blend` ("range of influence that each guide has on surrounding hairs"), `Use Region Map`, `Region Mask` (0..1 multiplier), `Region Map` ("Divides mesh surfaces into distinct regions by color, allowing guides to control hair interpolation only within their respective regions"). Wire: `Smoothness`, `Breakage`, `Reference State`. Shared: `Create` (guides at "10% default hair density"), `Use Selected Curve as Guide/Wire`, `Make Guides/Wires Dynamic` (a host hair system).
* **descriptionShape** (host application documentation): `Width Scale` ("Globally scales the per-CV width value"), `Taper` (± tip vs base), `Taper Start` (0 base .. 1 tip), `Width Ramp`, `Tube Shade` (host viewport only), `Render Density Multiplier` (render-time only density change), CV display options.
* **Sculpt layers**: "Sculpt modifiers contain sculpt layers which store and apply the grooming effects of the Interactive Grooming Tool brushes. By default, Sculpt modifiers have one sculpt layer" (search summary of host application documentation); layer weight 0.000–1.000 (host application documentation).
* **Brushes** (partial, search summary of host application documentation): Density (add/remove/redistribute), Comb, Grab, Smooth ("straightening effect ... or blend their orientation"); the full list (Place, Length, Width, Cut, Clump, Noise, Freeze, Part, Direction, Curl) is **UNVERIFIED** from this session (page not fetched).

### 1.6 Render-time evaluation and caching

* Classic a host groomer is a procedural: for batch render, "Export Patches for Batch Render" (File menu, Operation=Render, Renderer set to a host renderer) writes patch Alembics; "Without proper configuration, Export Patches for Batch Render requires redrawing a host groomer every frame"; recommended to "disable Live Mode and point to the alembic cache for the curve" and enable the host renderer "Use Aux Render Path" with the exported Patch Alembic for motion blur (search summaries: host application documentation). The host renderer `host_groomer_procedural` "allows keeping geometry rendering procedural until the final render stage" (search summary; exact procedural node docs **UNVERIFIED**).
* Two bake formats: **XPD** (Groom Bake / modifier `Baked` mode; "The primitive generator reads these files during previews and renders") and **Alembic** (IGS `Baked` mode, `xgmSplineCache`, Export Patches).
* Clump maps are `.xuv` point files; painted attributes are Ptex under `${DESC}/paint/...`.

---

## 2. A host renderer Groom (Hair Strands)

### 2.1 Alembic groom schema (fetched, host application documentation)

* Uses `Alembic::AbcGeom::ICurves`; "Property names must be lowercase with no spaces or special characters"; prefix `groom_`.
* General: `groom_version_major` (int16, =1), `groom_version_minor` (int16, =5), `groom_tool` (string), `groom_properties` (string).
* Geometry properties:

| Name | Type | Scope | Meaning |
|---|---|---|---|
| `groom_guide` | int8/16/32 | Constant/Uniform | 0 = strand, 1 = guide ("Guides are generated from the imported strands" if absent) |
| `groom_group_id` | int32 | Constant/Uniform | grouping; "up to 15 groom_group_id per groom asset" (search summary) |
| `groom_root_uv` | float[2] | Uniform | root UV; fallback "computed by projecting the root of the strand onto a sphere" |
| `groom_id` | int32 | Uniform | strand id (debug) |
| `groom_color` | float[3] | Vertex | per-vertex color; "Falls back to black if absent" |
| `groom_roughness` | float | (per doc list, not fetched) | **UNVERIFIED** scope |
| `groom_width` | float | Constant / Uniform / Vertex | width; a DCC widths converted; default 1 cm if missing |
| `groom_closest_guides` | int32[3] | Uniform | precomputed 3 guide indices ("interpolation data is computed outside of a host renderer") |
| `groom_guide_weights` | float32[3] | Uniform | weights for the 3 guides |

Design note: **3 guides + 3 weights per strand** is a host renderer's fixed interpolation arity.

### 2.2 Interpolation, guides, RBF (fetched, host application documentation and 4.27 reference host application documentation)

* `Guide Type`: Imported / Generated ("algorithmic from strands") / Rigged (bone-based guides; `Rigged guide num. curves`, `Rigged guide num points`).
* `Hair to Guide Density`: ratio of strands used as guides when generating.
* `Interpolation Quality`: Low = "Nearest neighbor search" (minutes), Medium = "Curve shape matching search within limited spatial range", High = full "curve shape matching search" ("dozens of minutes").
* `Interpolation Distance`: Parametric / Root / Index / Distance metrics for guide↔strand pairing.
* `Randomize Guide` ("Slightly randomizes guide selection to eliminate visual clumping"), `Use Unique Guide` ("Single guide per strand reduces interpolation cost").
* `RBF Interpolation` toggle; `RBF Type` / `Hair Interpolation Type`: Rigid Transform, Offset Transform, Smooth Transform ("translation + computed rotation"), No Skinning; per-LOD Auto/Enable/Disable.
* `Enable Guide-Cache Support` for runtime simulation caches.
* `Curve Decimation`, `Vertex Decimation` (LOD).

### 2.3 Binding (fetched, host application documentation)

* Root projection: "the root binding data for each rendered strand is shown by a colored white line at the root of the hair, along with the corresponding triangle on which the root is bound" (root → triangle + barycentrics).
* RBF: `Num Interpolation Points` — "Using more samples means more accurate deformation but also implies additional cost. In general, a 100 samples or less should be good enough."
* `Source Skeletal Mesh` / `Target Skeletal Mesh` transfer "assumes both ... share the same UV mapping"; `Matching Section`; `Groom Binding Type` Skeletal Mesh / Geometry Cache; per-LOD `Binding Type` Rigid vs Skinning.
* Without a binding asset "projection occurs at runtime" (4.27 reference).

### 2.4 Strands rendering, materials, LOD, physics

* Strands (fetched, host application documentation): `Hair Width` (cm), `Hair Root Scale`/`Hair Tip Scale` ("linearly interpolated from the root to the tip"), `Hair Shadow Density` (voxel transmission scale), `Hair Raytracing Radius Scale`, `Use Stable Rasterization`, `Scatter Scene Lighting`, `Use Hair Raytracing Geometry`, `Voxelize`; 4.27 adds `Hair Clip Length` (normalized, 1 = none).
* Materials (fetched, host application documentation): "Hair shading model" + "Use with Hair Strands"; `Hair Attributes` expression outputs U/V ("U ... along the hair, 0 root 1 tip"), Length, Radius, Seed ("Random value in 0 to 1, uniform along the curve"), Tangent, Root UV, Base Color, Roughness, Depth/Coverage/Auxiliary (cards/meshes), Atlas UVs, Group Index, Clump ID, AO.
* LOD (4.27 reference): `LOD Bias`, `Curve Decimation`, `Vertex Decimation`; geometry types strands/cards/meshes; per-LOD screen size and binding type (page partially fetched; angular threshold/thickness scale **UNVERIFIED**).
* Physics (fetched, host application documentation): GPU "XPBD (Position-Based Simulation of Compliant Dynamics)"; "Cosserat Rod and Angular Spring" models; substeps + solver iterations; guides with 4/8/16/32 points per strand; body collision via physics assets; self-collision from "an average velocity field built from the rasterization of the particles' velocity onto a regular voxel grid". Named parameter groups (Strand Parameters, Bend/Stretch/Twist constraints, External Forces, Collision) exist per search summary; exact fields **UNVERIFIED**.
* **"Curve modifiers" in a host renderer**: a host renderer has no per-strand *styling* modifier stack. Post-import deformation is done with **Deformer Graphs** (fetched, host application documentation): nodes `Groom Input` ("access to all the properties of the primary groom"), `Write Groom Output` (write position, radius, or both — "only existing attributes within a groom can be written out"), `Guides Input`, and `Custom Compute Kernel` with domain `Curve` ("one GPU thread per curve") or `Control Points` ("one GPU thread per control point"), implicit `Index`. a character asset "Hair Art Directability" (fetched, host application documentation) adds joint-based deformation templates `DG_GuidesSkinning`, `DB_StrandsSkinning`, Spline deformers, driven by a Groom Dataflow graph with per-group `Deformation` flag and simulation disabled. So a host renderer's answer to "modifiers on rigged/simulated curves" is *GPU kernels per curve/CV, after skinning/simulation, writing position/radius* — a good execution-model reference for our Hydra-side deformers.

---

## 3. A DCC grooming (SOP Groom nodes)

All fetched from host application documentation

### 3.1 Hair Generate 2.0 (`hairgen`)
* Distribution: `Mode` = "Scatter On Surface" / "Per Point"; `Group`; `Density` (overridable by attribute/texture); `Scatter Seed`; `Relax Iterations` ("more even distribution ... at the cost of more computation time"); `Relax Using Normal Attribute`.
* Optimization: `Prune` + `Pruning Ratio`; `Limit To Bounding Box` (`Box Size`, `Center`) — interactive preview aids.
* Guide weights: `Use Guides`; `Assume Uniform Segment Count & Length` ("faster guide interpolation algorithm"); `Compute Weights Using Skin Coordinates` ("robust and accurate, but can look less natural than weight computation based on guide distance"); `Blending Method` = Linear Blend / Extrude And Blend ("Extrudes the curve along each guide and then blends those extruded curves"); `Blend in Skin Space` (1 = rotate relative to skin normal at root; 0 = keep guide orientation); `Guide Group`; `Influence Radius`; `Influence Decay` ("how steeply the weight of a guide drops with distance"); `Maximum Guide Count`; `Max Guide Angle` (ignore guides facing away from the root normal by more than N°); `Clump Crossover` (0 = only dominant clump); `Create Guide & Weight Attribs` (array attribs of indices+weights).
* Unguided hairs: `Grow Unguided Hair`, `Use Initial Direction Attribute`, `Initial Dir Attrib`, `Segments`, `Length`, `Minimum Length`.
* Thickness: `Create Thickness Attribute`, `Thickness`, `Hair Profile` ramp.
* Attribute transfer from skin (point/vertex/prim/detail at the root location) and from guides.

### 3.2 Hair Clump 2.0 (`hairclump`)
* General: `Blend`; `Clump Size` ("When no clump curves are provided, uses a set of curves from the first input ... spaced such that clumps have roughly this size"); `Search Beyond Clump Radius`; `Crossover Rate`; `Random Seed`.
* Shape: `Method` Linear Blend / Extrude And Blend; `Preserve Length Differences`; `Extend To Match`; `Shorten To Match`; `Accurate Bundling` (uses `width`), `Hair Width`, `Hair Width Scale`; `Tightness`; `Stray Amount`, `Stray Rate` (% of curves allowed to stray), `Stray Falloff`; `Clump Profile` ("tightness of the clump along it's length").
* Fractal Clumping: `Iterations`; `Goal Feedback` (0 = each iteration independent, 1 = "uses the curve shapes computed in the previous iteration"); `Size Reduction`; `Tightness Reduction`.
* Curling (applied to clump curves before clumping): `Curling`, `Amplitude` + ramp, `Frequency` ("number of full turns per length unit") + ramp.
* Outputs: `clumpid`, `tightness` attributes; transfer of clump curve attributes.

### 3.3 Guide Process (`guideprocess`) — the styler multi-op
Operations (each with `Active`, `Solo`, `Blend` overridable by attribute/texture): **Set Direction** (`Uniform Direction` / `Direction Attribute`, `Direction Amount` "Rotate towards a direction around the skin normal", `Lift Amount`, `Mode` Rigidly Rotate / Rotate Each Segment); **Set Lift** (`Lift`, Min/Max randomized, `Follow Skin Contour`); **Set Length** (`Mode` Set/Add/Subtract/Multiply, `Method` Scale (around root) / Cut-Extend (at tip), randomize Min/Max, `Cull Threshold`); **Displace** (`Amount` along skin normal); **Wave** (`Frequency X/Y`, `Amplitude X/Y` in tangent/normal); **Straighten** (`Tangent Straightness`, `Normal Straightness`); **Smooth** (`Smoothing Mode` Object/Skin space, `Search Radius`, `Num Neighbors`); **Frizz** (`Frequency` ± random, `Limit Frequency to Representable Values`, `Amplitude` ± random); **Bend** (`Bend Axis Mode` Root Direction / Uniform Axis / Curve Attribute / Skin Attribute, `Angle` ± random, `Randomness Bias`); **Set Simulation Attributes** (multiparm, VEXpression).
Masks: `Curve Mask` (`Range In Absolute Length`, `Range Min/Max`, `Effect Position`, `Falloff` "pointy (low) to wide, bell-shaped (high)", `Influence Width`, `Curve Mask Ramp`), `Noise Mask` (`Amount`, `Frequency`, `Gain`, `Bias`), `Skin Mask` via group/paint; `Random Seed`; `Visualize Masks`; `Curve Per Skin Point`.

### 3.4 Guide Mask (`guidemask`)
`Input Mask` (attr/texture), Noise Mask (`Amount, Frequency, Gain, Bias, Center Noise Around One, Fractal Noise, Max Octaves, Lacunarity, Roughness`), Length Mask (Longer/Same/Shorter/Normalized Ramp/Range Ramp, `Falloff Range/Decay`, `Length Ramp`), Skin Curvature Mask (`Concave/Convex Maximum`, `Smoothing Strength`, `Curvature Ramp`), Geometry Mask (VDB SDF of 4th input: `Voxel Size`, `Interior/Exterior Range`, `Depth Ramp`, blur), Random Mask (`Seed`, `Fraction`, `Variation`, `Gain`, combine modes), Curve Mask (as above); outputs prim or point attribute, optional group / integer attribute with `Mask Threshold`.

### 3.5 Guide Deform (`guidedeform`)
Inputs: animated skin, rest skin, guides, guide interpolation mesh, rest/deformed deformers. `Deform Method` = Guide Shape Interpolation / Guide Weights / Guide Interpolation Mesh / Surface Deform / Point Deform; `Treat Skin as Subdivision Surface`; `Use Orient Attribute` (`restorient`/`orient` quaternions "twist-aware"), `Orient Blend`. Guide Shape Interpolation: `Mode` Capture and Deform / Capture (stores `guides`/`weights`) / Deform; `Max Candidates`, `Min/Max Guides`, `Blur Width` (Gaussian on shape distance), `Weight Threshold`, `Length Penalty Scale`. **Preserve Shape** = "Cosserat rod quasi-static relaxation pass": `Iterations`, `Stretch Stiffness`, `Bend Stiffness`, `Ref Pos Strength`, `Lock Roots`; `Preserve Clumps`. Uses the `rest` point attribute if present.

### 3.6 Other guide SOPs
* **Guide Groom** (`guidegroom`): brushes Draw (Guide/Parting Line, `Segment Mode` Fixed/Adaptive), Plant (Single/Scatter, `Interpolate Guides`, `Interpolate Relative to Skin`), Move, Relax, Delete, Cull, Smooth, Clump, Sculpt, Sculpt with Physics (a host solver; Live/Settle/Damped), Blur, Deintersect, Transform Handle (`Soft Transform`), Brush (`Bend`, `Bend Falloff`), Adjust Length (Add/Subtract/Set; Scale vs Cut/Extend), Lift, Straighten, Paint Group, Paint Mask; common `Surface Brush`, `Respect Parting Lines`, `Screen/Object Radius`, `Strength`, `Softness`, `Spacing`, `Per Point`, `Maintain Length`, `Constrain Length`, `Lock Root`, `Collide with Skin`, `Ray Bias`, `Edit Selection as Clump`; `Clear` "Clears all edits, which resets back to the unmodified input".
* **Guide Advect** (`guideadvect`): `Operation` = Constrained Advection / Fill Collision Field / Fill Velocity Field; `Follow Skin`; `Sampling Quality`; segment mode (`Keep Input Segment Count` / `Adaptive` + `Segment Length`); `Stop on Collision`; `Limit Length`.
* **Guide Initialize** (`guideinit`): `Wind Amount`, `Wind Direction`, `Tangential to Skin`, `UV Blend`, `UV Rotation`, `Lift`.
* **Guide Skin Attribute Lookup** (`guideskinattriblookup`): `Prim Num Attribute` (`skinprim`), `Prim UVW Attribute` (`skinprimuv`), `Use Rest Attribute If Present On Both Inputs` (stability under deformation), point/vertex/prim/detail attribute lists.
* **Guide Collide With VDB** (`guidecollidevdb`): `Blend`, Curve Mask block, `Collide With Skin`, `Surface Offset`, `Push Range`, `Push Amount`.
* **Guide Partition** (`guidepartition`): draws parting lines; `Radius`, `Strength` ("1.0 means that guides on one side of the parting line may not affect hair on the other side at all"), resample.
* **Guide Tangent Space** (`guidetangentspace`): "constructs a coherent tangent space along a curve" by propagating a root normal (from guide `N`, skin tangent attr, skin UV gradient, or constant) with minimal twist; outputs normal/tangent/bitangent.

### 3.7 a DCC Procedural: Hair (LOP) — the USD render-time analogue (fetched, host application documentation)
* Modes: **Generate** (interpolate render curves from guides at render time; "shorter fur/hair") and **Deform** (deform existing render curves by guides; requires `primvars:skinprim` and `primvars:skinprimuv` on guides).
* Inputs: guide `BasisCurves` with `primvars:rest`; skin mesh with `primvars:rest` and optional `primvars:density`, `primvars:hairlengthscale`; a render `BasisCurves` prim that "must be directly editable on the stage"; guides get `purpose = guide`, render prim gets a render purpose and is hidden unless a delegate runs the procedural.
* Parameters mirror Hair Generate: density, length min/max scale, width scale, influence radius/decay, max guide count, angle threshold, clump crossover, segment resampling, curve type Linear/Cubic, wrap, root relaxation, seed; deform options: skin-only, capture-and-deform, pre-weighted; single-guide optimization; velocity for motion blur; `Animated Procedural` flag; instanceable references "require some planning-ahead". Uses the `a host procedural API` pattern (exact schema names **UNVERIFIED** beyond the doc summary).

---

## 4. A DCC 3.5+ hair-curve node-group assets

A DCC's manual blocks automated fetches (HTTP 403); the list and parameters below come from search-result summaries of the official pages (host application documentation) plus the fetched 3.6 `create_guide_index_map.rst` (host application documentation). Categories: Deformation, Generation, Guides, Read, Utility, Write.

| Node (category) | Inputs / behaviour (as documented) |
|---|---|
| Create Guide Index Map (Guides) | Inputs `Guides`, `Guide Distance` (min spacing between chosen guides), `Guide Mask`, `Group ID`; outputs `Geometry`, `Guide Curves`, `Guide Index` ("index of the closest curve with the same Group ID value"), `Guide Selection`; writes int attribute `guide_curve_index`. |
| Clump Hair Curves (Guides) | `Clump Factor` (0 none, 1 fully to guide), `Shape` (0 = constant, 0.5 = linear falloff root→tip), `Tip Spread` (random tip variation), `Guide Distance`, `Guide Mask`; uses existing guide map if present else generates one. |
| Curl Hair Curves (Guides) | `Guide Index`, `Guide Distance`, `Radius` (tapers toward tip), `Frequency` ("full rotations along the length"; can vary per point), `Factor` (random phase), `Seed`. |
| Braid Hair Curves (Guides) | `Guide Distance`, `Braid Frequency`, `Braid Radius`, `Strand Thickness`, `Flare` (length of flare at the end). |
| Frizz Hair Curves (Deformation) | `Factor`, `Distance` (displacement magnitude), `Shape`, `Seed`; option for cumulative offsets ("each point's offset depends on the displacement of previous points") and length preservation. |
| Hair Curves Noise (Deformation) | `Factor`, `Shape`, `Scale` (noise by root position), `Distance` (noise along curve), `Roughness` (random per-curve offset), `Seed`, `Detail`/preserve segment length. |
| Roll Hair Curves (Deformation) | `Roll Length`, `Roll Radius`, `Roll Depth`, `Roll Taper` (rolls the tip into a coil). |
| Shrinkwrap Hair Curves (Deformation) | `Factor/Blend`, `Offset Distance (Min)`, `Lock Roots`. |
| Trim Hair Curves (Deformation) | `Length Factor`, `Length`, `Replace Length`, `Mask`, `Randomize`. |
| Smooth Hair Curves (Deformation) | `Strength` (negative = exaggerate), iterations. |
| Straighten Hair Curves (Deformation) | aligns points on the root→tip line. |
| Displace Hair Curves (Deformation) | displacement vector offset (surface-normal aligned). |
| Blend Hair Curves (Deformation) | blends each curve's shape with neighbours (parameters **UNVERIFIED**). |
| Interpolate Hair Curves (Generation) | `Surface` (geometry/object), `Interpolation Density`, `Guide Mask` (image texture), `Density Mask`, `Guide Index`, `Guide Distance`, follow-surface-normal option (summary). |
| Generate Hair Curves (Generation) | UV map for roots, `Surface Rest Position`, `Length`, `Resolution` (points per curve). |
| Duplicate Hair Curves (Generation) | copies within a radius around each guide, `Uniform Density`, `Seed`. |
| Attach Hair Curves to Surface (Utility) | projects roots to closest surface point, `UV Map`, optional align to surface orientation. |
| Redistribute Curve Points (Utility) | equal spacing along each curve. |
| Restore Curve Segment Length (Utility) | re-imposes original segment lengths after deformation. |
| Set Hair Curve Profile (Write) | `Replace Radius` (else multiply), `Profile` ramp for radius along the curve. |
| Read nodes | curve root/tip/parameter/length readers (names **UNVERIFIED**). |

Key a DCC conventions worth adopting: a single **`guide_curve_index` attribute** created once and reused by every guide-based node; `Shape` as a one-number falloff (0 = constant, 0.5 = linear); `Seed` on every random node; optional length preservation as a post-step.

---

## 5. Hair rendering references

### 5.1 Fiber scattering math
* **Kajiya–Kay (1989)** "Rendering fur with three dimensional textures" (https://dl.acm.org/doi/10.1145/74334.74361): anisotropic model on the fibre tangent `T`: `diffuse = K_d * sin(T,L)`, `specular = K_s * (cos(T,L)cos(T,V) + sin(T,L)sin(T,V))^p` (formula from standard literature; not re-fetched, well established).
* **Marschner et al. 2003** "Light Scattering from Human Hair Fibers" (https://graphics.stanford.edu/papers/hair/hair-sg03final.pdf): fibre = "transparent elliptical cylinder with an absorbing interior and a surface covered with tilted scales"; lobes **R**, **TT**, **TRT** (+glints from eccentricity); cuticle tilt shifts the highlights.
* **pbrt-v4 §"Scattering from Hair"** (fetched, https://pbr-book.org/4ed/Reflection_Models/Scattering_from_Hair): parameters `beta_m` (longitudinal roughness), `beta_n` (azimuthal roughness), `alpha` (cuticle tilt, ~2°), `eta` (1.55), `sigma_a`; `v[0] = (0.726 β_m + 0.812 β_m² + 3.7 β_m^20)²`, `v[1] = 0.25 v[0]`, `v[2] = 4 v[0]`; azimuthal logistic scale `s = sqrt(π/8) (0.265 β_n + 1.194 β_n² + 5.372 β_n^22)`; attenuation `T = exp(-σ_a · 2 cosγ_t / cosθ_t)`, `a_0 = f`, `a_p = a_{p-1} T f`; pigments **eumelanin σ_a = (0.419, 0.697, 1.37)**, **pheomelanin σ_a = (0.187, 0.4, 1.05)**, `σ_a = c_e·eu + c_p·pheo`; concentrations ≈ black 8, brown 1.3, blonde 0.3. Colour→absorption (Chiang 2016, quoted in search summary): `σ_a = (ln(C) / (5.969 − 0.215β_n + 2.532β_n² − 10.73β_n³ + 5.574β_n⁴ + 0.245β_n⁵))²`.

### 5.2 Production shaders (UI vocabulary)
* **PxrMarschnerHair** (fetched, https://rmanwiki-26.pixar.com/space/REN26/19661553/PxrMarschnerHair): Diffuse `Model` Zinke/Kajiya, `Diffuse Gain` (0), `Diffuse Color`; Specular R (`Gain`, `Color`, `Cone Angle`), TRT "Secondary Specular" (gain/color/cone angle, "darker and more saturated than the Transmit Specular Color"), TT "Transmit Specular" (gain/color/cone angle), `Glint` (gain/color, `Glint Width` 10–25), `Specular Offset`, `Refractive Index`, `Fresnel Mix`, `Eccentricity` (0.85–1.0 realistic), `Glow Gain/Color`, `Specular Energy Compensation`, `Presence`, `Eccentricity Direction`, `Shadow Color`; LPE lobes `HairDiffuse`, `HairSpecularR/TT/TRT/GLINTS`, user lobes `HairAlbedo`, `HairTangent`, `Position`.
* **A host renderer hair shader** (fetched from host application documentation): `Base` (1), `Base Color` (white; "controls absorption inside hair fibers"), `Melanin` (blonde ≈0.2, red/brown ≈0.5, black 1.0), `Melanin Redness` (0..1, pheomelanin proportion), `Melanin Randomize` (0); `Roughness` (0.2), `Anisotropic Roughness` toggle + `Azimuthal Roughness` (0.2), `IOR` (1.55; 1.4–1.6 wet), `Shift` (0° default; human 0–10°, e.g. 2.3–3.7°), `Scattering Mode` Approximate/Accurate/Adaptive, `Specular Tint` (white), `2nd Specular Tint` (white), `Transmission Tint` (white); Advanced `Opacity`, `Indirect Diffuse/Specular` (1), `Extra Depth`, `Extra Samples`. Recommended texture workflow: "set Melanin to 0 and link the texture to the Base Color".

### 5.3 MaterialX hair nodes shipped in the 26.08 install (Storm-relevant)
* `$USD/libraries/pbrlib/pbrlib_defs.mtlx:146-158` — `ND_chiang_hair_bsdf`: `tint_R`, `tint_TT`, `tint_TRT` (color3, 1), `ior` (1.55), `roughness_R` (0.1,0.1), `roughness_TT` (0.05,0.05), `roughness_TRT` (0.2,0.2), `cuticle_angle` (0.5), `absorption_coefficient` (vector3), `normal` (Nworld), `curve_direction` (defaultgeomprop **Tworld**), output `BSDF`.
* `pbrlib_defs.mtlx:433-442` — `ND_deon_hair_absorption_from_melanin`: `melanin_concentration` (0.25), `melanin_redness` (0.5), `eumelanin_color`, `pheomelanin_color` ("constant from d'Eon et al. 2011, converted to color via exp(-c)") → `absorption`.
* `pbrlib_defs.mtlx:445-452` — `ND_chiang_hair_absorption_from_color`: `color`, `azimuthal_roughness` (0.2) → `absorption`.
* `pbrlib_defs.mtlx:455-467` — `ND_chiang_hair_roughness`: `longitudinal` (0.1), `azimuthal` (0.2), `scale_TT` (0.5), `scale_TRT` (2.0) → three vector2 roughnesses.
* GLSL implementations exist: `libraries/pbrlib/genglsl/mx_chiang_hair_bsdf.glsl` and `pbrlib_genglsl_impl.mtlx:28-29, 80-87`.
* Storm's MaterialX shader-gen builds tangents from the mesh TBN or a fallback `cross(N, up)` (`<openusd-src>/pxr/imaging/hdSt/materialXShaderGen.cpp:34-44`) — for curves the per-fragment tangent will *not* be the curve direction unless we feed it via a primvar. Whether Storm compiles `chiang_hair_bsdf` end-to-end for BasisCurves is **UNVERIFIED** (no hair references in `hdSt/materialXFilter.cpp` or `hdMtlx`).

### 5.4 USD BasisCurves conventions and Hydra consumption
* Schema (`<openusd-src>/pxr/usd/usdGeom/schema.usda:1594-1760`): "often used to render dense aggregate geometry like hair or grass" (l.1596-1598); cubic bases `bezier` vstep 3, `catmullRom` 1, `bspline` 1 (l.1619-1623); `pinned` wrap adds phantom points `P[-1] = 2P[0] − P[1]`, `P[n] = 2P[n−1] − P[n−2]` and allows cubic curves with as few as 2 CVs (l.1642-1661); validity table (l.1672-1678); primvar interpolation: `vertex` (cubic) vs `varying` (per segment end, linear) with explicit count formulas (l.1691-1760). `Curves.widths` (l.104-111 of the `Curves` block at `schema.usda` ~l.1480-1594): "ribbon width" if normals exist, else "cylinder width"; "If 'widths' and 'primvars:widths' are both specified, the latter has precedence."
* usdImaging reads `primvars:widths`/`primvars:normals` first, then falls back to `widths`/`normals` attrs (`pxr/usdImaging/usdImaging/basisCurvesAdapter.cpp:140-263`), maps `pinned` to `HdTokens->pinned` (l.367-368), and forwards `screenSpaceWidths`/`minScreenSpaceWidths` primvars (l.34-35, 472-473).
* Hydra built-in curve primvars are `points`, `normals`, `widths` (`pxr/imaging/hd/basisCurves.cpp:33-37`); `HD_ENABLE_REFINED_CURVES` env forces refinement (l.18-19, 54).
* **Storm** (`pxr/imaging/hdSt/basisCurves.cpp`): without refinement cubic curves are *downcast to linear* (l.292-301); with `refineLevel>0` and authored `widths`: authored `normals` → `RIBBON/ORIENTED`, else refine>2 → `HALFTUBE/ROUND`, refine>1 → `RIBBON/ROUND`, else `RIBBON/HAIR` (l.322-341); `_SupportsRefinement = refineLevel > 0 || HD_ENABLE_REFINED_CURVES` (l.1312-1320); user widths/normals detection (l.1323-1329). Shader stage `Curves.Fragment.Hair` computes a screen-derivative normal (`pxr/imaging/hdSt/shaders/basisCurves.glslfx:1365-1375`). Storm-only primvars `screenSpaceWidths` and `minScreenSpaceWidths` ("preventing thin curves such as hair from undesirably aliasing when their screen space width would otherwise dip below one pixel") are always preserved (`hdSt/basisCurves.cpp:1359-1384`). Pinned curves are unpacked in `hdSt/basisCurvesComputations.cpp:261-350`.
* usdview complexity → refine level: `low=1.0, medium=1.1, high=1.2, veryhigh=1.3` (`pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43`), converted to `refineLevel 0..8` in `pxr/usdImaging/usdImagingGL/engine.cpp:2320-2350` and applied as a display-style fallback (l.2358-2363). Hence **usdview "Low" complexity draws cubic hair as linear wires**; our tooling must bump complexity or set `refineLevel` on our prims.
* **hdPrman** (`<openusd-src>/third_party/renderman/plugin/hdPrman/basisCurves.cpp:59-127`): maps `cubic`→`Ri:type cubic` with basis bspline/bezier/catmullrom, `linear`, wrap periodic/nonperiodic, `Ri:nvertices` uniform; Hydra `points`→`P`, `normals`→`N` ("Hydra \"normals\" becomes Renderman \"N\"") and `widths`→`RixStr.k_width` (`hdPrman/renderParam.cpp:497-504`). No hair-specific attributes in hdPrman (grep `hair` empty).
* There is no hair schema in USD 26.08 (`grep -rl Hair pxr/usd/*/schema.usda` hits only doc strings).

---

## 6. Feature matrix

Legend: ● full, ◐ partial/indirect, ○ absent, ? unverified.

| Feature | a host groomer classic | a host groomer IGS | a host renderer Groom | a DCC | a DCC hair nodes |
|---|---|---|---|---|---|
| Scatter random (Poisson) | ● | ● (Density brush + seed) | ○ (import) | ● (scatter + relax) | ● (Interpolate/Generate density) |
| Scatter uniform rows/cols | ● | ○ | ○ | ◐ (Per Point) | ○ |
| Scatter at points / at guides / from file | ● | ◐ | ○ | ◐ (Per Point, groups) | ◐ (Duplicate) |
| Density map (ptex/texture/expr) | ● ptex+expr | ● maps | ○ | ● attr/texture | ● image/density mask |
| Stable ids / seed | ● Generator Seed, `$id` | ● | ● `groom_id` | ● Scatter Seed | ● Seed |
| Guide interpolation | ● Voronoi ROI, per-guide Blend, region maps | ● Guide modifier (Blend, region) | ● 3 guides/weights, quality tiers | ● radius/decay/max count/angle/crossover, parting lines | ● nearest guide (1) via index map |
| RBF surface transfer | ○ | ○ | ● (≤100 samples) | ◐ (Point Deform) | ○ |
| Deform with animated surface (rest→anim) | ◐ (patches follow; groomable splines don't) | ● | ● binding | ● Guide Deform (+Cosserat) | ◐ (Surface Rest Position, Attach) |
| Clump (multi-level, maps) | ● .xuv maps, 2nd map 2× | ● clump points/map | ○ (Clump ID for shading only) | ● fractal iterations | ◐ (chain nodes) |
| Clump noise/copy/cut/volumize | ● | ● | ○ | ◐ (stray, curling, tightness profile) | ◐ (Tip Spread) |
| Noise / frizz (multi-freq) | ● Noise (freq, correlation) | ● | ○ | ● Frizz + Noise mask, Wave | ● Frizz, Noise |
| Curl / coil | ● Coil | ◐ (Curl in clump, Curl brush) | ○ | ● curling in Hair Clump | ● Curl, Roll, Braid |
| Bend / lift / direction | ● Bend params, Tilt/Around N | ◐ brushes | ○ | ● Bend, Set Lift, Set Direction, Guide Initialize | ◐ (Straighten/Displace) |
| Cut / length | ● Cut (Keep Param/Reparam) | ● Cut modifier, Scale | ◐ Clip Length | ● Set Length + Cull Threshold | ● Trim |
| Width profile / taper | ● Width Ramp, Taper | ● Width Scale, Taper, Ramp | ● root/tip scale | ● Thickness + Hair Profile | ● Set Hair Curve Profile |
| Smooth / straighten | ◐ brush | ● Smooth brush | ○ | ● Smooth/Straighten ops | ● Smooth/Straighten |
| Collision / shrinkwrap | ● Collision modifier | ● Collide | ● physics only | ● Collide With VDB, Deintersect | ● Shrinkwrap |
| Displace from surface | ○ | ● Displacement | ○ | ● Displace | ● Displace |
| Wind/forces | ● Force, Wind | ○ (a host hair system) | ● a host solver | ● a host solver | ○ (sim nodes) |
| Region / parting lines | ● Region maps | ● Region map | ○ | ● Guide Partition | ◐ Group ID |
| Expressions | ● SeExpr | ● SeExpr | ◐ material graph | ● a host expression language | ● node fields |
| Masks per operator | ● Mask expr/ptex + Scale ramps | ● | ○ | ● skin/curve/noise masks | ● Factor fields, Shape |
| Sculpt layers / freeze | ◐ Groom Bake XPD | ● Sculpt layers, Freeze brush | ○ | ◐ Guide Groom edits, Clear | ◐ sculpt mode + Apply |
| Bake/cache | ● XPD, Alembic patches | ● Alembic | ● cooked at import | ● File Cache | ● Apply |
| Render-time procedural | ● host_groomer_procedural | ◐ | ● runtime GPU | ● Hair Procedural LOP | ○ |
| LOD | ○ | ○ | ● decimation, cards, meshes | ◐ prune | ○ |
| Physics on strands | ◐ a host hair system | ◐ a host hair system | ● a host solver | ● a host solver | ○ |

---

## 7. How each system does masks

| System | Per-curve weight source | Along-curve weight | Notes |
|---|---|---|---|
| a host groomer classic | `Mask` attribute on every modifier = SeExpr expression, `map()` of a Ptex/image, `rand()`, `$id` hashes | Per-parameter "Scale" ramps R→T (Clump Scale, Magnitude Scale, Count Scale, Radius Scale, Copy Scale, Noise Scale) | Region maps constrain guide/clump membership; `Preserve Length` blends length back |
| a host groomer IGS | `Mask` + `Density Mask` on clump points; sculpt layer weight | Clump/Noise/Copy Ramps; Width Ramp | `Region Mask` multiplier 0..1 |
| a DCC | `Blend` overridable by attribute/texture; Skin mask via group/painted attr; Guide Mask SOP (noise/length/curvature/geometry/random) producing a `mask` attribute | Curve Mask: ramp *or* (`Range Min/Max`, `Effect Position`, `Falloff`, `Influence Width`); can be absolute length | Noise Mask (freq/gain/bias) modulates per curve; `Visualize Masks` |
| a DCC | `Factor`/`Mask` fields (any attribute/texture) | `Shape` (0 const, 0.5 linear) | Guide Mask filters guide candidates |
| a host renderer | none per operator; per-strand attributes go to materials (`Seed`, `Root UV`, `Clump ID`, `Group Index`) | material U | Deformer-graph kernels read any attribute |

Recommendation: implement one `UsdGenMaskAPI`-style block on every operator with **(a)** per-curve weight = `map × expr × random × group`, **(b)** along-curve ramp = spline ramp with the host application four-parameter shortcut, **(c)** optional noise mask, all evaluated once per topology epoch and cached as a float array.

---

## 8. Dirty propagation, caching and interactivity in each system

* **a host groomer classic**: whole modifier stack recomputes bottom-up on any change; heavy stages are frozen with `Groom Bake` (XPD) which "deactivates all modifiers below it"; individual modifiers have `Mode = Baked` reading XPD; clump maps are precomputed `.xuv` point sets (regenerated on demand, `Auto Update` in IGS); preview density is decoupled from render density (`Render Density Multiplier`).
* **a host groomer IGS**: modifiers are a DCC DG nodes → standard DG dirty propagation; Noise has `Baked` (Alembic) mode with explicit `Bake Noise Data`; Spline Cache plays Alembic.
* **a DCC**: per-SOP cook cache; Hair Generate offers `Prune`/`Pruning Ratio` and `Limit To Bounding Box` for interactive work; Guide Deform separates `Capture` (expensive, once) from `Deform` (per frame) via stored `guides`/`weights` attributes; Guide Skin Attribute Lookup uses `rest` for stability; the Hair Procedural LOP defers generation to render with an `Animated Procedural` flag to avoid per-frame re-evaluation.
* **a DCC**: geometry-nodes tree re-evaluates fully per depsgraph update (no per-node cache), but guide selection is cached as the `guide_curve_index` attribute if present.
* **a host renderer**: interpolation build is an offline import step (minutes); runtime is GPU per-frame: skinning/RBF → simulation (guides) → interpolation to strands → deformer graph → render; LOD decimation precomputed.

Implication for `usdGen`: split every operator into **capture** (topology-dependent, cache keyed by scatter seed + surface topology + guide set: ids, root prim/uv, guide indices+weights, clump ids, kd-trees) and **evaluate** (per-frame, per-curve parallel, keyed by upstream point-array version). This matches a DCC's Capture/Deform split, a host renderer's import/runtime split and a host groomer's `.xuv`/XPD caches.

---

## 9. Proposed canonical operator set for `usdGen`

Common contract for every operator prim (`UsdGen*` schemas): inputs = `surface` (rest+deformed mesh, via usdRig/Hydra scene index), `curves` (upstream `BasisCurves`-shaped buffer: `curveVertexCounts`, `P`, `widths`, per-curve `id`, `rootPrim`, `rootUV`, `rootFrame`), optional `guides`, `maps` (texture/ptex prims), `mask` block (§7), `seed`. Output = curves buffer + primvars. "Topology" below = changes curve count or CV count.

Cost classes: **A** = O(CV) embarrassingly per-curve parallel; **B** = needs a spatial structure over roots (kd-tree build O(n log n) once per capture, query O(log n) per curve); **C** = surface queries (closest point / SDF) per CV.

### 9.1 Generators (topology-creating)

**G1 `ScatterRandom`** — purpose: a host groomer "Randomly across the surface" / a DCC Scatter. Inputs: surface (rest), density map, mask, region map. Params: `density` (float, 100/unit², ≥0), `seed` (int, 0), `relaxIterations` (int, 0, 0–50; a DCC), `useAreaCompensation` (bool, true), `flip` (bool). Math:
```
for face f: expected = density * area_rest(f) * mean(mask over f)
  n_f = floor(expected) + (hash(seed,f) < frac(expected))
  for k in 0..n_f-1: (b0,b1,b2) = low-discrepancy sample k of hash(seed,f); id = hash64(seed,f,k)
  optional Poisson-disk relax on the surface (a DCC "Relax Iterations")
emit root(prim=f, uv=b, frame from N_s, dPdu) with stable id
```
Topology: yes. Parallel: per face. Cost: A (per face), relax = B. Stable ids: `(seed, faceIndex, k)` → changing density only adds/removes the tail per face (a host groomer `Generator Seed` semantics).

**G2 `ScatterUniform`** — rows/cols in parametric space: `spacingU/V`, `jitter`. Math: grid over each face's UV extents; id = `hash(seed,f,i,j)`. Cost A.

**G3 `ScatterPoints`** — explicit `rootPrims[]`, `rootUVs[]` (authored by the Place/Plant tool). Cost A.

**G4 `ScatterAtGuides`** — one hair per guide root. Cost A.

**G5 `GrowFromRoots`** — turns roots into initial straight/advected curves (a DCC Guide Initialize / unguided hairs): `segments` (int, 8), `length` (float, 1) + map/random min/max, `direction` = surfaceNormal | attribute | wind (`windDir`, `windAmount`, `tangentialToSkin`), `uvBlend`, `lift` (deg). Math: `P[i] = root + lift-rotated dir * length * i/(n-1)`. Topology: sets CV count. Cost A.

**G6 `GuideInterpolate`** (the "curve generator" when guides exist) — inputs: roots (from G1–G3), guides (with root prim/uv and rest frames). Params: `maxGuides` (int, 3 like a host renderer / a host groomer typical), `influenceRadius` (float), `influenceDecay` (float, 2), `maxGuideAngle` (deg, 90), `blendInSkinSpace` (0..1, 1), `blendMethod` (linear | extrudeAndBlend), `useUniqueGuide` (bool), `randomizeGuide` (0..1), `regionMap`/`partingLines` (radius, strength), `clumpCrossover`, `cvCount` (int, 0 = max of guides). Capture:
```
kd = kdtree(guide roots in rest space); for root r: cand = knn(kd, r, maxCandidates within R)
  drop guides with angle(N_s(r), N_s(g)) > maxGuideAngle or region(g) != region(r) or across a parting line (strength s → w *= 1-s)
  w_g = (1 - d/R)^decay  (or a host groomer Voronoi-boundary distance); w /= Σw; keep top maxGuides (optionally 1 + hash jitter)
  store guideIdx[3], guideW[3]  == a host renderer groom_closest_guides/groom_guide_weights
Evaluate (per frame): frame_r = rootFrame(r) ; for g: local_g = frame_g^-1 * (G_g(t) - root_g) resampled to cvCount
  P(t) = r + frame_r * Σ w_g local_g          (blendInSkinSpace=1; else world offsets)
```
Topology: sets CV count. Parallel: per curve. Cost: capture B, evaluate A. Optional RBF variant (`rbfSamples` ≤100) for surface-transfer only, not for hair shape.

**G7 `DeformWithSurface`** ("static curves + deforming surface"): inputs curves with `rest` (P_rest) and root binding (prim, uv); params: `mode` = rigidFrame | rbf | pointDeform, `rbfSamples` (100), `twistAware` (bool), `preserveShape` (Cosserat iterations, stretch/bend stiffness, refPosStrength, lockRoots — a DCC). Math: `P = frame_anim(root) * frame_rest(root)^-1 * P_rest` per CV (rigid), or RBF displacement field from ≤N surface samples (a host renderer). Topology: no. Cost A (rigid) / B (RBF). This is the host application "Deform" mode of the Hair Procedural and a host renderer binding.

### 9.2 Stylers (no topology change unless noted)

**S1 `Clump`** — inputs: curves, optional clump centers (from a nested scatter G1 with `clumpDensity`, or `clumpCurves`, or `clumpMap` texture/ptex), mask. Params: `clump` (0..1, 0.5) + ramp, `clumpSize` (float) or `clumpDensity`, `clumpSeed`, `tightnessProfile` ramp, `volumize` (0..1), `stray` (amount 0..1, rate 0..1, falloff), `copy` (0..1) & `copyVariance`, `noise` (amount, frequency, correlation) + ramp, `cut` (0..1), `curl` (amplitude+ramp, frequency+ramp — applied to clump curves before clumping), `flatness`, `offset`, `orient`, `preserveLength` (0..100 %), `levels` (int, 1), `sizeReduction` (0.5), `tightnessReduction` (0.8), `goalFeedback` (0..1, 1), `method` = linearBlend | extrudeAndBlend, `crossover` (0..1). Capture: kd-tree of clump roots; per hair `clumpId = nearest` (or by map colour / region) + `w_stray = (hash(id) < strayRate) ? strayAmount : 0`. Evaluate:
```
level loop L = 0..levels-1: size_L = size*sizeReduction^L ; tight_L = tight*tightnessReduction^L
  C = clump curves for level L (level>0: derived from previous level's output if goalFeedback else from input)
  for hair h with clump c: for CV i, t_i:
     target = C_c(t_i) (linear) or C_c(t_i) + frame_c(t_i) * local(P_i - guideExtrude) (extrudeAndBlend)
     w = clump * ramp(t_i) * tightProfile(t_i) * mask(h) * (1 - stray(h)) * (1 - copyVariance*hash)
     P_i = lerp(P_i, target + volumize * radial(hash(h)) * (1-t_i), w)
  if preserveLength: restoreSegmentLengths(P, restLengths, root-locked)
optional cut: shorten a hash-selected fraction by cut*length; noise: P_i += noiseVec(clumpRoot*corr + hash(h)*(1-corr), t_i*freq) * ramp(t_i)
```
Topology: no (unless `cut` with reparam). Parallel: per curve (per level barrier). Cost: capture B, evaluate A × levels. Emits `clumpId[level]` primvars for shading (a host renderer `Clump ID`).

**S2 `Noise/Frizz`** — params: `magnitude` (float) + ramp, `frequency` (cycles per unit length), `correlation` (0..1), `octaves` (1–6), `lacunarity` (2), `gain` (0.5), `space` = rest | skin | world, `seed`, `preserveLength`, `cumulative` (bool, a DCC), `limitFrequencyToResolution` (bool, a DCC). Math: `P_i += magnitude * ramp(t_i) * mask * vfbm(rootPos_rest * corrScale + (1-correlation)*hash3(id) + [0,0,t_i*frequency])` evaluated in the root frame so it follows the deforming surface. Cost A.

**S3 `Curl/Coil`** — params: `radius` + ramp, `frequency` (turns per unit length) or `count`, `phase` (+random), `taper` (bool), `axisMode` = curveTangent | guide, `clockwise`, `seed`. Math: build minimal-twist frame (T,N,B) along the (smoothed) curve (a DCC Guide Tangent Space); `P_i += r(t_i) * (cos(2π f s_i + φ) N_i + sin(...) B_i)` with `s_i` = arc length; `restoreSegmentLengths` optional. Cost A. `Roll` (a DCC) = same op restricted to the tip with `rollLength/depth/taper`; `Braid` (optional, a DCC): three sub-strands per guide offset by phase 0/120/240° with `braidRadius`, `braidFrequency`, `strandThickness`, `flare` — topology yes (3×). 

**S4 `Bend`** — params: `angle` (deg) + random ± bias, `axisMode` = rootDirection | uniform | curveAttr | skinAttr, `axis`, ramp. Math: for each segment `k`, rotate CVs `i ≥ k` about axis through `P[k]` by `angle * (ramp(t_k) - ramp(t_{k-1}))` (cumulative bend, a DCC "Rotate Each Segment"). Cost A.

**S5 `Direction/Lift`** — params: `direction` (vector or attr/map), `amount` (0..1), `lift` (deg or set/add/min/max), `mode` = rigid | perSegment, `followSkinContour`. Math: project target dir to the tangent plane at root; rotate the curve rigidly about `N_s` by `amount * angle`; lift = rotate about `N_s × T` to reach elevation. Cost A. (a host groomer Tilt U/V/N & Around N are the same op parameterised as angles.)

**S6 `Length/Cut/Trim`** — params: `mode` = set | add | subtract | multiply | cutAbsolute | cutRelative, `value` (+random min/max, map), `method` = scale | cutExtend, `minRemainingLength`, `cullThreshold` (remove hairs shorter than), `rebuild` = keepParam | reparam. Math: arc-length table; scale: `P_i = root + (P_i - root) * s`; cut: find `s_cut`, truncate, then either collapse CVs to cut point (keepParam) or resample `cvCount` CVs uniformly (reparam). Topology: `cull` yes; `reparam` keeps CV count. Cost A.

**S7 `Width`** — params: `width` (float), `widthRamp`, `taper`, `taperStart`, `rootScale`/`tipScale` (a host renderer), `replace` (bool). Math: `w_i = base(w_i or width) * ramp(t_i) * (t_i < taperStart ? 1 : lerp(1, 1-taper, (t_i-taperStart)/(1-taperStart))) * mask`. Writes `widths` (vertex). Cost A.

**S8 `Smooth`** — params: `strength` (−1..1), `iterations`, `mode` = alongCurve (Laplacian) | neighbours (a DCC: `searchRadius`, `numNeighbors`, object/skin space), `lockRoot`. Cost A (along) / B (neighbours).

**S9 `Straighten`** — params: `tangentStraightness`, `normalStraightness` (a DCC) or single `factor`; Math: `P_i = lerp(P_i, root + (tip-root) * s_i/len, k)` with per-plane decomposition. Cost A.

**S10 `Shrinkwrap/Collide`** — params: `target` = skin | collider prims | SDF volume, `offset`, `pushRange`, `pushAmount`, `iterations`, `lockRoots`, `resolveType` = flexible (push CVs to closest surface point) | stiff (rotate first intersecting segment). Cost C (closest-point/SDF query per CV; use a prebuilt BVH per surface epoch).

**S11 `Displace`** — params: `amount` (+map: height or vector displacement), `base` (0.5), `scale`, `offset`, `coordSys`; Math: `P_i += N_s(root) * (map(uv)-base)*scale + offset`. Cost A.

**S12 `Wave`** — a DCC: `frequency`/`amplitude` in tangent and normal directions; Math: `P_i += A_x sin(2π f_x s_i) T_u + A_y sin(2π f_y s_i) N_s`. Cost A.

**S13 `Scale`** (global length multiplier, a host groomer IGS) — trivial, Cost A.

**S14 `RestoreSegmentLength`/`Resample`** (utility): re-impose rest segment lengths (a DCC), or resample to `cvCount` (topology: CV count). Cost A.

**S15 `Wind/Force`** (optional, time-dependent): a host groomer Wind params (`direction`, `constStrength`, `gustStrength`, `shearStrength`, `shearFreq`, `stiffness` + ramp, `seed`, loopable). Math: per-CV displacement `d = (const + gust*fbm(P,t)) dir + shear*fbm2(P,t) perp` scaled by `(1-stiffness) * t_i²`; `restoreSegmentLengths`. Cost A.

### 9.3 Freeze / sculpt-layer semantics

* **`Freeze` op**: snapshot of the upstream curve buffer (points, widths, ids, root bindings) written to the stage (or to a sidecar file) as a `BasisCurves` + `usdGen:rest`; downstream ops read the frozen buffer; upstream ops are deactivated while the freeze is live (a host groomer Groom Bake: "deactivates all modifiers below it"; modifiers above "remain active, and read the data from the XPD file"). Frozen data should carry `frozenEpoch` = hash of (scatter seed, surface topology, guide ids) so a mismatch marks the freeze *stale* rather than silently misaligning ids.
* **`SculptLayer` op**: stores per-curve, per-CV **deltas in the root frame** keyed by stable `id` (so surface deformation and upstream parameter tweaks that keep ids still apply), with a layer `weight` 0..1 (a host groomer sculpt layer weight), and optional `Freeze` brush weights per curve (a mask that zeroes downstream stylers for that curve). Multiple layers blend additively in order. Deltas for ids that no longer exist are kept but ignored; a "rebase" tool re-expresses deltas against a new upstream when topology changes (a DCC `Clear` is the destructive alternative).
* **Guides** are themselves a freeze of a sparse subset (a host groomer "Create guides at 10% density"; a DCC `Create Guide Index Map`), editable with Comb/Grab/Smooth/Length/Lift/Straighten/Cut/Plant brushes (a DCC Guide Groom tool list in §3.6).

### 9.4 Texture/map inputs
* Map prims: `UsdGenTextureMap` (UV-mapped image via the surface's `st`), `UsdGenPtexMap` (face-id + per-face UV; Ptex is *not* available on this machine → needs bundled reader), `UsdGenExprMap` (SeExpr with the host groomer global set in §1.3 and a `map()` function), `UsdGenPaintMap` (usdview brush-painted per-face texel or per-vertex data). Evaluate once per capture into per-curve floats; a host groomer evaluates `map()` at `$u,$v` of the root (`map("name"[,s,t][,channel])`).

---

## Key facts

* a host groomer modifier stacks evaluate bottom-up, each modifier consuming the previous output — host application documentation
* a host groomer generators: Poisson random / uniform rows-cols / specified points / at guides / from XPD, with `Generator Seed`, `Density`, `Mask` ("multiplier on the Density"), `Compensate Normals` — host application documentation
* a host groomer guide interpolation = Voronoi range-of-influence, ray-boundary distance weights, normalized, guides rebuilt to common CV count, per-guide `Blend`; region maps and backface culling filter guides — host application documentation
* a host groomer expression globals `$u $v $id $cLength $cWidth $cDepth $descId $faceid $frame $patchId $P/$Pref/$Pw... $dPdu $dPdv $N $Cs $As` — host application documentation functions incl. `map("name"[,s,t][,channel])`, `rand`, `noise`, `fit`, `remap`, `guidesAttr`, `alignU/V/N` — host application documentation SeExpr full set (fbm/turbulence/voronoi/pick/choose/curve interp codes 0–4) — https://wdas.github.io/SeExpr/doxygen/userdoc.html.
* a host groomer Clumping params (Clump/Scale/Volumize/Copy/Copy Variance/Cut/Noise/Noise Frequency/Noise Correlation/Flatness/Offset/Curl/Frame, `.xuv` point maps, second map at 2× density) — host application documentation and host application documentation
* a host groomer Groom Bake writes XPD and "deactivates all modifiers below it"; later modifiers read the XPD — host application documentation
* a host groomer IGS modifier node set: Sculpt (layers, weight 0–1), Scale, Curve to Spline, Clump, Collide, Cut, Displacement, Guide, Linear Wire, Noise, Spline Cache — host application documentation
* a host renderer Alembic groom schema: `groom_guide`, `groom_group_id`, `groom_root_uv`, `groom_id`, `groom_color`, `groom_width` (const/uniform/vertex; default 1 cm), `groom_closest_guides` int32[3], `groom_guide_weights` float[3] — host application documentation
* a host renderer interpolation quality tiers (nearest neighbour / limited shape match / full shape match), distance metrics, `Use Unique Guide`, `Randomize Guide`, RBF types — host application documentation binding = root→triangle + RBF with ≤100 samples — host application documentation
* a host renderer has no styling modifier stack; post-skinning deformation uses Deformer Graph kernels per curve / per control point writing position and radius — host application documentation
* a DCC Hair Generate: density/seed/relax, `Influence Radius`, `Influence Decay`, `Maximum Guide Count`, `Max Guide Angle`, `Clump Crossover`, Linear vs Extrude-And-Blend, `Blend in Skin Space`, prune/bbox for interactivity — host application documentation
* a DCC Hair Clump 2.0: Clump Size, Tightness, Stray Amount/Rate/Falloff, Clump Profile, fractal `Iterations`/`Goal Feedback`/`Size Reduction`/`Tightness Reduction`, Curling amplitude/frequency ramps — host application documentation
* a DCC Guide Process ops and mask model (Blend, Curve Mask with Range/Effect Position/Falloff, Noise Mask, Skin Mask) — host application documentation Guide Mask sources — host application documentation
* a DCC Guide Deform: capture/deform split, `rest` attrs, Cosserat "Preserve Shape" — host application documentation Hair Procedural LOP Generate/Deform at render time with `primvars:rest`, `primvars:skinprim(uv)`, `primvars:density` — host application documentation
* a DCC: `guide_curve_index` attribute shared by Clump/Curl/Braid/Interpolate; `Shape` 0 = constant, 0.5 = linear; Clump `Tip Spread`; Frizz cumulative offsets & length preservation — host application documentation and the 3.6 rst host application documentation
* Hair BSDF math (β_m→v, β_n→s, melanin σ_a constants, colour→σ_a polynomial) — https://pbr-book.org/4ed/Reflection_Models/Scattering_from_Hair.
* MaterialX 1.39.5 in the install has `chiang_hair_bsdf` (tint_R/TT/TRT, ior 1.55, roughness_R/TT/TRT, cuticle_angle, absorption_coefficient, curve_direction=Tworld), `deon_hair_absorption_from_melanin`, `chiang_hair_absorption_from_color`, `chiang_hair_roughness` with GLSL impls — `$USD/libraries/pbrlib/pbrlib_defs.mtlx:146-158,433-467`; `libraries/pbrlib/genglsl/pbrlib_genglsl_impl.mtlx:28-29,80-87`.
* Storm downcasts cubic curves to linear when unrefined and picks RIBBON/HALFTUBE/ORIENTED by refine level and presence of `widths`/`normals` — `<openusd-src>/pxr/imaging/hdSt/basisCurves.cpp:292-341,1312-1329`; `minScreenSpaceWidths`/`screenSpaceWidths` primvars exist for hair anti-aliasing — `hdSt/basisCurves.cpp:1359-1384`; `Curves.Fragment.Hair` shader stage — `hdSt/shaders/basisCurves.glslfx:1365-1375`.
* usdview complexity low/medium/high/veryhigh = 1.0/1.1/1.2/1.3 → refineLevel 0..8 fallback — `pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43`, `pxr/usdImaging/usdImagingGL/engine.cpp:2320-2363`.
* USD BasisCurves: pinned wrap phantom points, vstep table, `varying` vs `vertex` primvar sizing, `primvars:widths` precedence — `pxr/usd/usdGeom/schema.usda:1594-1760` and the `Curves.widths` doc (~l.1583-1590); usdImaging fallback order — `pxr/usdImaging/usdImaging/basisCurvesAdapter.cpp:140-263,367-368`.
* hdPrman maps Hydra `normals`→`N`, `widths`→`RixStr.k_width`, cubic basis tokens → Ri Basis — `third_party/renderman/plugin/hdPrman/renderParam.cpp:499-504`, `hdPrman/basisCurves.cpp:59-127`.
* Hydra built-in curve primvars are exactly `points`, `normals`, `widths`; `HD_ENABLE_REFINED_CURVES` forces refinement — `pxr/imaging/hd/basisCurves.cpp:18-19,33-37,54`.

## Open questions

1. A host groomer IGS brush list (Place/Length/Width/Cut/Clump/Noise/Freeze/Part/Direction/Curl) and their per-brush parameters were not fetched (tool page blocked); only Density/Comb/Grab/Smooth are confirmed.
2. A host groomer `host_groomer_procedural` (a host renderer) parameter set and exact render-time evaluation contract (what is evaluated per frame vs cached) — not fetched.
3. A host renderer: exact physics parameter fields (Strand Parameters, Bend/Stretch/Twist constraint fields, External Forces, Collision) and LOD fields beyond decimation (angular threshold, thickness scale, screen size) — pages returned only a TOC.
4. A host renderer `groom_roughness` scope/type and whether a `groom_basecolor`/precomputed-guides property exists in v1.5 — not in the fetched table.
5. A DCC node parameter lists for Blend Hair Curves, Displace Hair Curves, Interpolate Hair Curves (full input set), Duplicate Hair Curves (radius/count) and Read-category node names — host application documentation blocked automated fetch (403); values above come from search summaries.
6. A DCC Hair Clump's clump-curve auto-selection algorithm ("spaced such that clumps have roughly this size") and the Hair Procedural LOP's exact USD schema/attribute names (`a host procedural API`) — only doc summaries.
7. Whether Storm's MaterialX path compiles `chiang_hair_bsdf` for BasisCurves and how `Tworld`/`curve_direction` would be supplied (Storm's shader-gen derives tangents from mesh TBN or `cross(N, up)`, `hdSt/materialXShaderGen.cpp:34-44`) — needs a prototype.
8. Kajiya–Kay formula and Marschner lobe parameter defaults quoted from general literature, not re-fetched from the papers.
9. PxrMarschnerHair parameter *defaults* (IOR, cone angles, glint width) beyond those listed were not present on the fetched page.
