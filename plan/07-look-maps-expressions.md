# Look, maps and expressions

Date: 2026-09-04. Status: plan v1 (draft, pending review).

This document specifies everything usdGen does with colour: the `UsdGenLookAPI` bake that turns
maps, Ptex and SeExpr expressions into curve primvars; the Storm preview shaders that make a groom
look rendered in the viewport; the one `Material` prim with three terminals that keeps Storm,
hdPrman and the "plugin not installed" case in agreement; the seven `UsdGenMap` prim types that are
this plugin's texture-loading prims (request **R7**, §0.1); and the two vendored libraries — Ptex 2.4.3 and
`wdas/SeExpr` `main@8f8c8f2` — that evaluate them on the CPU. It is written for the engineer who has
to implement the bake and the shaders without access to the design conversation: every parameter
name, every default, every file:line and every measured number is here.

Reads with: `02-schema.md` (prim types, property namespaces, the reserved `Maps` scope),
`03-execution-engine.md` (capture/evaluate split, capture epochs, the per-worker task arena),
`04-operators.md` (the mask block, which operators consume maps), `06-imaging.md` (the tile contract
C2, invalidation discipline, the `__usdGenRender` scope), `08-tools.md` (the map-paint and
expression-editor UX), `09-performance-and-benchmarks.md` (Storm frame times and the gate
protocol), `10-build-dependencies-testing.md` (the `usdGen_ptex` / `usdGen_seexpr` / `usdGenShaders`
targets and the EGL harness), `11-roadmap.md` (M1 and M4 contents).

---

## 0. Requirements and evidence

### 0.1 What the request asks

* **R6** — "Storm viewport look close to a rendered result, driven by the material's properties."
* **R7** — "Colour from image maps or Ptex maps referenced/painted on surfaces through
  plugin-specific texture prims; SeExpr2 expressions producing textures/attributes dynamically."

**Two `R` families, never mixed.** `R6`/`R7` here are the *request requirements* of
`design/brief-v1.md` §1 and appear only in this subsection and the header paragraph. Everywhere else
in this document an `Rn` is an **ADR §9 ruling** and is always written with its ADR section — `ADR
§9.2 R7`, `ADR §9.5 R40` — so the two can never be confused.

R6 is a *shader* problem and R7 a *sampling* problem, and this document keeps them apart. R6 is
solved on the GPU by a glslfx surface shader reading four declared primvars plus `displayColor`. R7
is solved on the CPU, at capture time, by samplers that write those primvars and operator inputs —
never by handing a texture to Storm, except for the one UV path that is measured to work.

### 0.2 Settled decisions this document implements

| # | Decision | Where it lands here |
|---|---|---|
| S13 | Asset attributes resolve stage-free; `asset[]` gets **no** reload tracking; one `asset` per map prim; reload is an explicit action | §5.1, §5.5 |
| S29 | Curve contract: mandatory primvars `points, widths, hairT, hairTangent, hairId, st, displayColor`, `minScreenSpaceWidths = 1.0`, `refineLevel = 2`, no `normals` | §1.4, §2.6 |
| S30 | One refineLevel and one material per tile set; never co-dirty `displayColor` with `points`; a primvar appearing is dirtied as `primvars/<name>` once | §1.5, §3.4, §8.2 |
| ADR §8 | `02-schema.md` §2.12 and §2.14 are the normative source for `usdGen:map:*`, `usdGen:paint:*` and `usdGen:look:*` names, types and defaults; C1 freezes them at the end of M1. Every place this document adds a property is flagged as an addition to be folded back | §1.1, §5.1, §8.1 |
| S35 | v1 Storm look = a plugin glslfx surface shader with `UsdShade` shader defs discovered from a plugin `shaderDefs.usda`; never read `inData` from the material. ADR §9.1 R4 fixes the shipped set at **three** files and three defs (default, translucent, primvar-tangent variant B) | §2 |
| S36 | One `Material` with three terminals: `outputs:surface` (UsdPreviewSurface), `outputs:mtlx:surface` (MaterialX), Storm-specific (glslfx) | §3, §4 |
| S37 | All map/Ptex/SeExpr evaluation is **CPU at capture time**, baked to per-curve/per-CV primvars; Storm textures only for UV-mapped scalp colour via `st`; Ptex is mesh-only and compiled out of this install | §1.2, §5, §6 |
| S38 | Vendored `wdas/SeExpr` `main@8f8c8f2` (interpreter only, static + hidden, `rand()` added), Ptex v2.4.3 (static + hidden), SeExpr noise as the single noise implementation | §6, §7 |
| ADR §1 (S29) | `hairId` is a uniform **float** in [0,1) — the shipped glslfx declares it `float`; ADR §9.2 R12 fixes the formula as `UsdGenHash32(curveId, 0) / 2^32` over the stable 64-bit id | §1.2, §1.4, §2.3 |
| ADR §1 (S11) | Colour ramp = `float[] <p>:positions` + `color3f[] <p>:colors` + interpolation (a `TsSpline` cannot hold colours) | §1.1 |
| ADR §1 (S36) | `USDGEN_STORM_MATERIAL_OVERRIDE` ships **default OFF**, decided by gate L-1 (M0 pre-work, binding at M1 — ADR §9.5 R40) | §3.2, §3.3 |
| ADR §5.4 | The `hairTangent` fork is a measured decision (gate S-8), not a mandate | §2.5 |
| ADR §6 | Look = risk §7 + artist §7.1 (`inputs:` block verbatim) + performance §9.1 (MaterialX chain with an explicit world-space tangent); `rand()` added; SeExpr `noise` keeps 0..1; one `PtexFilter` per worker. ADR §9.4 R33 fixes *how* the world-space tangent is obtained: a `transformvector` node inside the MaterialX network, **never** a context-dependent primvar | all of it |
| ADR §9.1 R1 | Engine principles P1–P8 are the invariants **I1–I8** (P0/P1/P2 are the motion profiles); `EV-nnn` is the only citation handle for a measured number | §0.3, §5.4 |
| ADR §9.1 R4 | Shader-def identifiers are prim names: `UsdGenHairPreview`, `UsdGenHairPreviewTranslucent`, `UsdGenHairPreviewPrimvar`; three glslfx files, one `inputs:` block | §2.1, §2.4 |
| ADR §9.2 R7 | `02-schema.md` is the single normative property registry; every `usdGen:look:*` and `usdGen:map:*` row here conforms to it, and a property 02 lacks is a fold-in **into 02** | §1.1, §5.1 |
| ADR §9.2 R8 | `UsdGenLookAPI` gains `usdGen:look:bakeMode` and `usdGen:look:colorMapMode`, keeps `bakeTarget`/`jitterSeed`/`bakePrimvar`, and names the scalar exponent `usdGen:look:rampExponent` | §1.1, §1.3 |
| ADR §9.2 R11 | Ramp interpolation is `linear \| catmullRom \| bspline \| constant`, default **`"catmullRom"`**; a colour ramp is `positions` + `colors` + that token | §1.1, §1.2 |
| ADR §9.2 R12 | `curveId` is `uint64[] primvars:usdGen:curveId`; the hash is the pinned SplitMix64 finalizer; `hairId = UsdGenHash32(curveId, 0) / 2^32`; the per-curve draw is `UsdGenDraw01(seed, curveId, salt)` | §1.2, §1.4, §5.4 |
| ADR §9.4 R31 | `08-tools.md` §1.4 is the single source of the C ABI (contract C4); `ReloadMaps` is one of its entry points | §5.5 |
| ADR §9.4 R32 | Commit trigger (c) **always** applies, with or without an app driver | §8.2 |
| ADR §9.4 R33 | `hairTangentWorld` is rejected; tiles publish no context-dependent primvars | §1.4, §4.1, §4.2 |
| ADR §9.5 R38 | v1 = the M0–M7 deliverable set, and it names `UsdGenPtexMap` at M4 | §6 |
| ADR §9.5 R40 | `09-performance-and-benchmarks.md` §5 is the single gate registry; L-1/S-8 are M0 pre-work binding at M1, L-2 is T2 M1, L-3/L-4/L-5 are T0–T1 M4 | §9.1 |
| ADR §9.5 R42, R43 | Every number carries MEASURED (`EV-nnn`) / `DERIVED from EV-nnn` / UNMEASURED (with its gate) / ASSUMPTION; ledger rows are `EV-001…EV-092`; `hdSt/renderDelegate.cpp:695-707` is the accepted range | §0.3, §3.2 |
| ADR §7 | M1 freezes contract C5 (glslfx parameter names); M4 delivers Image/Ptex/Expr/Paint maps, the SeExpr set, the `LookAPI` bake and reload | §9 |

### 0.3 The measurements this document rests on

Every number below was produced on this host (Linux aarch64, NVIDIA GB10, driver 580.173.02,
GL 4.6 compatibility profile through an EGL device-platform context; `research/ENVIRONMENT.md`
CORRECTIONS block).

| Fact | Value | Status | Source |
|---|---|---|---|
| `usdGenHairPreview.glslfx` parses in Sdr | 20 inputs, `primvars` metadata `hairId\|hairT\|hairTangent\|st` | MEASURED | `research/G-storm-hair-look-prototype.md` §2.1; `prototypes/storm-hair-look/sdr_parse_test.py` |
| It compiles and renders in Storm | 0 warnings, 0 GL errors | MEASURED | same, §2.2; `prototypes/storm-hair-look/hair_pv_tangent.png` |
| Storm frame time, 200 k curves × 8 CV, refineLevel 2, 1280×720, this glslfx | 23.93 ms (42 fps) | MEASURED | **EV-021** (`appendix-A-evidence-ledger.md` §2.3); same, §5 |
| Per-frame `points` re-author + upload at 1.6 M CVs (19.2 MB) | +2.46 ms | MEASURED | **EV-022** (`appendix-A-evidence-ledger.md` §2.3); same, §5 |
| A material glslfx reading `inData.Neye` fails to compile at refineLevel 0 | `error C1009: "Neye" is not member of struct "CurveVertexData"` | MEASURED | same, §2.5 |
| `patchCoord` on curves is `vec4(0, v, 0, 0)` with `v` **across** the width | — | MEASURED | same, §2.5; `pxr/imaging/hdSt/shaders/basisCurves.glslfx:1293`, TES at `:746` |
| Screen-derivative tangent is visibly sparkly on 1–2 px strands | — | MEASURED (visual) | same, §2.5 |
| `float[2]`/`float[4]` glslfx parameters bind as `vec2`/`vec4` | — | MEASURED | same, §2.4; `appendix-A-evidence-ledger.md` §6 K11 |
| `displayOpacity` with a bound material does nothing; with none it promotes to `masked` | — | MEASURED | same, §2.6 |
| MaterialX `chiang_hair_bsdf` compiles and renders in Storm but is near-black | melanin sweep to 0.03 | MEASURED | same, §3 |
| `ND_geompropvalue_vector3(geomprop="hairTangent")` does feed `curve_direction` in Storm | — | MEASURED | same, §3 |
| UsdPreviewSurface + `UsdPrimvarReader_float2` on a **uniform** `st` + `UsdUVTexture` works on curves | per-curve root-UV lookup | MEASURED | same, §4; `prototypes/storm-hair-look/hair_preview_tex.png` |
| SeExpr interpreter cost | 13 ns (`$u*$v+1`), 34 ns (`map()`+`hash`), 106–117 ns (noise/fbm/voronoi) per eval; 50 M evals/s on 8 threads; prep 7–53 µs | MEASURED | **EV-067**, **EV-068**, **EV-069** (`appendix-A-evidence-ledger.md` §2.8); `research/A8-seexpr-ptex-libs.md` §1.6; `prototypes/thirdparty-bench/seexpr_bench.cpp` |
| `rand()` is **not** a SeExpr2 builtin | runtime: `Function rand has no definition` | MEASURED | `research/A8-seexpr-ptex-libs.md` §1.7; `prototypes/thirdparty-bench/se_min.cpp` |
| Ptex lookup cost | 23 ns bilinear (1-texel footprint), 26 ns box (4-texel), 35 ns/lookup/thread at 8 threads = 228 M/s | MEASURED | **EV-070** (`appendix-A-evidence-ledger.md` §2.8); `research/A8-seexpr-ptex-libs.md` §2.8; `prototypes/thirdparty-bench/ptex_test.cpp` |
| Ptex absent from this OpenUSD install; `.ptx` in a Storm material is silently 1×1 black | `HdStIsSupportedPtexTexture("a.ptx") == 0` | MEASURED | `research/A8-seexpr-ptex-libs.md` §0, §2.9; `pxr/imaging/hdSt/ptexTextureObject.cpp:121-229` (guard), `:233-268` (fallback) |
| Hio formats in this install | read/write `png jpg tga bmp hdr exr`; read `avif`; **no** `tif/tx/ptx` | MEASURED | `research/A8-seexpr-ptex-libs.md` §3.2 |
| Overwriting a map file on disk invalidates nothing (`stage->Reload()` emits no notice) | — | MEASURED | `research/G-stage-free-parameter-and-time-transport.md`, S13 |
| Live paint: `primvars/<name>/primvarValue` per move = 0.14–0.24 µs and no descriptor rebuild; `primvars/<name>` once on appearance = 2.06 µs; the bare `primvars` locator = 4.10 µs **with** a descriptor rebuild | — | MEASURED | **EV-034** (`appendix-A-evidence-ledger.md` §2.4); `research/G-tool-loop-array-transport-and-cv-picking.md` §5 |
| Varying-interpolation `widths` cost a CPU expansion | 6.48 ms to expand 600 000 → 800 000 values on 100 k curves | MEASURED | **EV-033** (`appendix-A-evidence-ledger.md` §2.4); `research/G-storm-throughput-and-prim-granularity.md` §2 "Topology strategy" item 5; Key facts |

Number tags follow ADR §9.5 R42: **MEASURED** (a number produced on this host, cited by its
`EV-nnn` ledger row), **DERIVED from EV-nnn** (scaled, summed or interpolated — never MEASURED),
**UNMEASURED** with the gate that settles it, or **ASSUMPTION**. Every number in this document
carries one. `appendix-A-evidence-ledger.md` §2 has assigned the handles this document needs:
**EV-021**, **EV-022** (§2.3), **EV-033**, **EV-034** (§2.4) and **EV-067**–**EV-070** (§2.8) are
named in the Source column above, and **EV-019**, **EV-020**, **EV-023** — the rest of the
refineLevel sweep — are cited where the sweep is used (§2.6). The rows with no `EV-nnn` are
qualitative results (a shader compiles, a format is absent, a notice is not emitted) that the ledger
records as host facts (§1), verified file:line facts (§3) or corrections (§6) rather than as
measurement rows; for those the report section is the citation. Anything not in this table and not tagged inline is UNMEASURED or an ASSUMPTION, and
says which gate settles it.

---

## 1. The look pipeline

### 1.1 `UsdGenLookAPI`

`UsdGenLookAPI` is a single-apply API schema applied to `UsdGenDescription` (ADR §2.1). It is
codeless (S9) and its properties are published to Hydra by the same generic adapter mapping as every
other `usdGen:` property (S10; see `06-imaging.md`). It carries **only** what the bake needs, plus
the parameters that must agree between the bake and the shader. Everything else about the look —
specular lobes, transmission, opacity, width falloff — lives on the `Shader` prims of §2 and §4 and
is never duplicated here.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:look:rootColor` | `color3f` | `(0.035, 0.018, 0.008)` | Linear albedo at the root. Same value, same name as the glslfx input. |
| `usdGen:look:tipColor` | `color3f` | `(0.210, 0.115, 0.045)` | Linear albedo at the tip. |
| `usdGen:look:rampExponent` | `float` | `1.0` | Exponent applied to `hairT` before the root→tip lerp; >1 keeps the root colour longer. Feeds the glslfx input **named `colorRamp`** (§2.2; the glslfx name is what C5 freezes, not the schema name). |
| `usdGen:look:colorRamp:positions` | `float[]` | `[]` | Optional multi-stop ramp; when non-empty it **replaces** `rootColor`/`tipColor`/`rampExponent`. Encoding per ADR §1 (S11): a `TsSpline` cannot hold colours. |
| `usdGen:look:colorRamp:colors` | `color3f[]` | `[]` | Same length as `:positions`. |
| `usdGen:look:colorRamp:interpolation` | `uniform token` | `"catmullRom"` | `linear \| catmullRom \| bspline \| constant` (ADR §9.2 R11; `02-schema.md` §2.14 declares the row and §2.17 states how each token builds the 257-entry LUT). |
| `usdGen:look:colorMap` | `rel` | — | Exactly one `UsdGenMap` target. Sampled at the curve root, or once per CV when the map's `usdGen:map:domain = "cv"` and `usdGen:look:bakeMode = "perCV"` (§5.1, §5.2). Absent ⇒ white. |
| `usdGen:look:colorMapMode` | `uniform token` | `"bake"` | `bake` (CPU, at capture) or `shader` (the tool wires the map to the shader's `rootColorMap` texture input and bakes nothing). **A map is either baked or wired, never both.** `shader` is legal **only** when `usdGen:look:colorMap` targets a `UsdGenImageMap` whose `usdGen:map:uvSet` is the surface's `st` set and whose file is a Hio-readable format (§5.4); any other target is a compile error naming the reason. In `shader` mode the tool authors, under the `Material`, a `UsdUVTexture` on the map's `usdGen:map:file` fed by a `UsdPrimvarReader_float2(varname="st")`, connects it to `inputs:rootColorMap`, and bakes nothing. |
| `usdGen:look:hueJitter` | `float` | `0.0` | Per-curve hue jitter amplitude, driven by `hairId` and `jitterSeed`. Same meaning as the glslfx `randomHue`. |
| `usdGen:look:valueJitter` | `float` | `0.0` | Per-curve brightness jitter. Glslfx `randomValue`. |
| `usdGen:look:jitterSeed` | `uniform int` | `0` | Salt for both jitters, so two descriptions sharing one look do not jitter identically (§1.2). |
| `usdGen:look:bakeMode` | `uniform token` | `"perCurve"` | `perCurve \| perCV` (ADR §9.2 R8). Selects the **granularity** of the bake: `perCurve` ⇒ one `uniform` value per curve, `perCV` ⇒ one `vertex` value per CV. The no-bake state is `bakeTarget = "none"`, expressed once. See §1.3. |
| `usdGen:look:bakeTarget` | `uniform token` | `"displayColor"` | `displayColor \| primvar \| none`. Selects the **destination**: `displayColor` ⇒ `primvars:displayColor`; `primvar` ⇒ `primvars:<bakePrimvar>`; `none` ⇒ nothing. See §1.3 and the filtering caveat in §1.4. |
| `usdGen:look:bakePrimvar` | `token` | `"usdGen:albedo"` | The primvar written when `bakeTarget = "primvar"`. Naming it `"displayColor"` selects the same destination as `bakeTarget = "displayColor"`; the two spellings are one state and the tool normalises to `bakeTarget = "displayColor"` when it authors (§1.4). |

**Reconciliation with `02-schema.md` §2.14**, the normative source for this schema (ADR §9.2 R7), whose names
C1 freezes at the end of M1 (ADR §3). Every row above is that table's name, type, default and
allowed-token set verbatim — including the three ADR §9.2 R8 ordered into it, which 02 §2.14 now
carries: `usdGen:look:bakeMode`, `usdGen:look:colorMapMode` (both `uniform token`) and
`usdGen:look:rampExponent` (`float`, `1.0`), alongside 02's unchanged `bakeTarget`, `jitterSeed` and
`bakePrimvar`. This table repeats them for the reader of the bake; it does not add them, and where
the two ever disagree 02 §2.14 wins. R8's `rampExponent` is deliberately *not* called `colorRamp`:
a property sharing a base name with the `colorRamp:*` arrays would be rendered as one control group
by a panel generated from `UsdPrimDefinition::GetPropertyNames()` (ADR §6). `colorMapMode` is what
makes the route-3 texture path of §5.6 representable at all.

**Settled:** the two properties do **not** collapse into one, and both ship. `bakeMode` and
`bakeTarget` are two statements, not one: **`bakeMode` is the granularity** (`perCurve` ⇒
`uniform`, `perCV` ⇒ `vertex`) and **`bakeTarget` is the destination** (`displayColor`,
`primvars:<bakePrimvar>`, or `none`). The no-bake state is expressed exactly once, by
`bakeTarget = "none"`; `bakeMode` is then ignored, so `bakeMode` carries no `none` token
(`02-schema.md` §2.14).

The tool's **Groom → Create hair material** action authors the three terminals of §3 *from these
values*, so the Storm preview, the render material and the universal fallback start out consistent
by construction; after that they are ordinary USD attributes an artist can edit in usdview's
property editor. `usdGen:look:*` names are frozen at the end of M1 under contract C1 (ADR §3).

### 1.2 The bake order

The bake is a pure function of the capture state. It runs once per capture epoch, on the capture
threads, in exactly this order. `c` indexes the curve within the capture, `curveId(c)` is its stable
**64-bit** id (`uint64[] primvars:usdGen:curveId`, ADR §9.2 R12), `t` is `hairT` (0 at the root, 1 at
the tip), and

```
hairId(c) = UsdGenHash32(curveId(c), 0) / 2^32        # ADR §9.2 R12; the pinned SplitMix64 finalizer
```

— the same value the tile publishes as `primvars/hairId` (`06-imaging.md` §4.1). The **id**, never
the index: a scatter, a density scrub or a resample renumbers curves, and a jitter keyed on the index
would re-randomise every strand when it did (ADR §2.3, §9.2 R12–R13).

The per-curve draw is `04-operators.md` §0.6's call-site helper, declared
`inline float UsdGenDraw01(int seed, uint64_t curveId, uint32_t salt)` over the pinned
`inline float UsdGenHash01(uint64_t key, uint32_t salt)` (ADR §9.2 R12; the same two signatures
appear in `02-schema.md` §2.19.1). Both already take the 64-bit id, so nothing has to widen. The
look bake's salt is **`kSaltLookJitter`**, one row of 04 §0.6's per-use salt table.

At the default `jitterSeed = 0` the bake draws `hairId(c)` itself instead of calling
`UsdGenDraw01`, because `hairId` is all the shader has: `float seed = UsdGenHair_GetHairId();`
(`prototypes/storm-hair-look/usdGenHairPreview.glslfx:404`), so CPU and GPU jitter agree bit for bit
at seed 0. A **non-zero** `jitterSeed` re-randomises the CPU side only — there is no seed input in
the `inputs:` block of §2.2 and C5 adds none — so it is meaningful when the jitter is baked (§1.3),
and the tool warns when it is set while the jitter is left to the shader.

```
step 0  j(c)        = (jitterSeed == 0) ? hairId(c)          # bit-identical to the shader's draw
                                        : UsdGenDraw01(jitterSeed, curveId(c), kSaltLookJitter)
step 1  base(c[,i]) = colorMap.domain == "cv" ? colorMap(cv(c,i)) : colorMap(root(c))
                                                            # ImageMap | PtexMap | ExprMap | PaintMap
                                                            #   | NoiseMap | CombineMap | GuideProximityMap
                                                            # absent, or colorMapMode == "shader"  ->  (1,1,1)
                                                            # domain == "cv" is legal only when
                                                            #   bakeMode == "perCV"; otherwise a compile
                                                            #   error naming both properties
step 2  ramp(t)     = positions/colors ? sampleRamp(t)     # colorRamp:interpolation, one of
                                                           #   linear | catmullRom | bspline | constant,
                                                           #   default catmullRom, over the 257-entry
                                                           #   LUT of 02-schema.md 2.17 (ADR 9.2 R11)
                                       : mix(rootColor, tipColor, pow(clamp(t,0,1), max(rampExponent,1e-3)))
step 3  jitterH(c)  = hueShift(  ., (fract(j(c)*17) - 0.5) * hueJitter )
        jitterV(c)  = scale(     ., 1 + (fract(j(c)*41) - 0.5) * 2 * valueJitter )
step 4  perCV       = only when bakeMode == "perCV": evaluate steps 1-3 at each CV's own t
                      (and, under domain == "cv", at each CV's own map sample, §5.2)
step 5  destination = bakeTarget == "displayColor" -> primvars:displayColor
                      bakeTarget == "primvar"      -> primvars:<bakePrimvar>
                      bakeTarget == "none"         -> nothing
        interpolation = bakeMode == "perCurve" -> uniform, one value per curve
                        bakeMode == "perCV"    -> vertex,  one value per CV
                        (ignored when bakeTarget == "none": nothing is written)
```

Steps 2 and 3 are written to match the shipped shader **bit for concept**, so that moving a factor
from CPU to GPU changes where it is computed and not what it looks like: the fract-multipliers 17
and 41 and the hue rotation around the luminance axis are copied from
`prototypes/storm-hair-look/usdGenHairPreview.glslfx` (`UsdGenHairHueShift`, and the jitter block of
`UsdGenHair.Surface`). Keeping them identical is a testable property, in two halves: the CPU half is
`testUsdGenLookBake` (T0, §9.2), which checks the bake against a hand-computed reference; the GPU
half is gate L-2's golden image, whose explicit parity clause is in §9.1.

The order matters in one place only: the map multiplies the *base* and the ramp mixes two *colours*,
so `ramp(map × root, map × tip)` and `map × ramp(root, tip)` are the same, but a multi-stop ramp is
not a mix, and the jitter is a hue rotation which does not commute with a multiply. Implement the
order as written.

### 1.3 Baked versus shaded: the compute-once rule

Each factor is applied exactly once. **`bakeMode` says at what granularity** and **`bakeTarget` says
into which primvar**; the two are orthogonal, and every one of the six pairs is legal (the two
granularities are moot when `bakeTarget = "none"`).

| Factor | `bakeMode = "perCurve"` (default) | `bakeMode = "perCV"` | `bakeTarget = "none"` |
|---|---|---|---|
| base colour (map/ptex/expr) | CPU, capture → the destination, `uniform` | CPU, capture → the destination, `vertex` | not evaluated |
| root/tip ramp | glslfx, per fragment, from `hairT` | CPU, capture, per CV | glslfx |
| per-curve hue/value jitter | glslfx, per fragment, from `hairId` | CPU, capture | glslfx |
| shader inputs authored by the tool | glslfx `rootColor`, `tipColor`, `colorRamp` (← `rampExponent`), `randomHue` (← `hueJitter`), `randomValue` (← `valueJitter`) | the same five neutralised (`rootColor = tipColor = (1,1,1)`, `colorRamp = 1`, `randomHue = randomValue = 0`) | from `UsdGenLookAPI` |
| memory | 12 B/curve → **1.2 MB at 100 k curves** (DERIVED from the MEASURED 12 B per `color3f`, `research/G-storm-hair-look-prototype.md` §2.5) | 12 B/CV → **19.2 MB at 1.6 M CVs** (DERIVED from the same MEASURED 12 B/CV; the identical 19.2 MB appears as the measured points payload in that report's §5) | 0 |

`bakeMode = "perCurve"` into `bakeTarget = "displayColor"` is the default pair because it is the
cheapest thing that still lets a painted scalp map drive strand colour, and because the root→tip
gradient is exactly what a fragment shader does well. `perCV` exists for three real cases: a base
colour that must vary *along* the strand (a `UsdGenExprMap` or `UsdGenNoiseMap` at
`usdGen:map:domain = "cv"`, §5.1 — the only maps whose value changes per CV), a ramp the target
renderer cannot reproduce, and the fallback case below. `bakeTarget = "none"` exists for grooms whose
colour is entirely a shader decision.

Known and accepted degradation: at `bakeMode = "perCurve"`, the universal `outputs:surface` fallback
(UsdPreviewSurface reading `displayColor`) shows the base colour **without** the root/tip ramp,
because UsdPreviewSurface has no notion of `hairT`. A studio that needs the ramp in the fallback sets
`bakeMode = "perCV"` with the destination resolving to `displayColor` — i.e. either
`bakeTarget = "displayColor"`, or `bakeTarget = "primvar"` with `bakePrimvar = "displayColor"`, which
name the same primvar (§1.1) — putting the fully resolved per-CV colour where every consumer already
looks. This is documented in the user docs, not worked around.

**Two conditional rows of the C2 tile contract follow, and `06-imaging.md` §4.1, which owns C2,
carries both** (ADR §3, §5.3):

1. `primvars/displayColor`, `VtVec3fArray`, role `color`, **`uniform` at `bakeMode = "perCurve"` and
   `vertex` at `bakeMode = "perCV"`** when the destination resolves to `displayColor`; absent at
   `bakeTarget = "none"`.
2. `primvars/<bakePrimvar>`, same type and role, same interpolation rule — an **optional** row,
   present only at `bakeTarget = "primvar"` with a `bakePrimvar` other than `displayColor`.

Interpolation belongs to the primvar *descriptor*, not its value, so a `bakeMode` change dirties
`primvars/displayColor` once — the bare per-primvar locator — and not `…/primvarValue` (§1.5;
2.06 µs MEASURED, **EV-034**, `research/G-tool-loop-array-transport-and-cv-picking.md` §5).

### 1.4 What lands on the tile

The bake writes exactly one primvar of the tile contract C2 — `displayColor`, or the primvar
`bakePrimvar` names; the rest come from the engine (`06-imaging.md` §4.1).

| Primvar | Interp | Type | Produced by |
|---|---|---|---|
| `points` | vertex | `point3f[]` | terminal node buffer, interleaved at the Hydra boundary |
| `widths` | vertex, or constant when uniform | `float[]` | `UsdGenWidth` / description default. **Never `varying`** — 6.48 ms to expand 600 000 → 800 000 varying values on 100 k curves (MEASURED, **EV-033**; `research/G-storm-throughput-and-prim-granularity.md` §2 "Topology strategy" item 5 and Key facts; `pxr/imaging/hdSt/basisCurvesComputations.h:69-158` defines `HdSt_ExpandVarying` and `:236-250` is the varying branch that calls it at `:248`) |
| `hairT` | vertex | `float[]` | generator, arc-length normalised at capture |
| `hairTangent` | vertex | `vector3f[]` (**object space**) | published only under Variant B (§2.5). There is no world-space companion: ADR §9.4 R33 rejects `hairTangentWorld` outright, and the render network transforms this primvar with a node instead (§4.1). A tile's primvar set never varies with the session context (ADR §5.3) |
| `hairId` | uniform | `float[]` | `UsdGenHash32(curveId, 0) / 2^32` at capture (ADR §9.2 R12) |
| `st` | uniform | `texCoord2f[]` | the root UV on the emitting surface |
| `displayColor` | `uniform` at `bakeMode = "perCurve"`, `vertex` at `perCV` — the conditional C2 row of §1.3 | `color3f[]` | **the §1.2 bake** |
| `<bakePrimvar>` | the same rule | `color3f[]` | the §1.2 bake when `bakeTarget = "primvar"` names something other than `displayColor`; the optional C2 row of §1.3 |
| `minScreenSpaceWidths` | constant | `float` | `1.0` |

A `bakePrimvar` other than `displayColor` reaches a **custom** material only. Storm filters primvars
down to those the bound material declares (`isPrimvarFilteringNeeded = true`,
`pxr/imaging/hdSt/renderDelegate.cpp:709`; `HDST_ENABLE_MATERIAL_PRIMVAR_FILTERING` defaults true,
`pxr/imaging/hdSt/materialNetworkShader.cpp:30-35`; the terminal's declared primvars become material
params at `pxr/imaging/hdSt/materialNetwork.cpp:1128-1139`). The shipped shaders declare `hairT`,
`hairId`, `st` and — under Variant B only — `hairTangent`.

`displayColor` and `displayOpacity` survive because HdSt seeds every material shader's
primvar-filter set with them unconditionally: `_GetExtraIncludedShaderPrimvarNames()`
(`pxr/imaging/hdSt/materialNetworkShader.cpp:383-413`) begins with `displayColor` and
`displayOpacity`, and `_CollectPrimvarNames()` (`:415-437`, recomputed in `SetParams`, `:235`) is
what `HdStGetPrimvarDescriptors` filters against (`pxr/imaging/hdSt/primUtils.cpp:114-142`).
`minScreenSpaceWidths` survives for the other reason — it is a builtin name on the prim
(`pxr/imaging/hdSt/basisCurves.cpp:1357-1386`, whose comment says that names consumed in
`basisCurves.glslfx` must be claimed there to survive filtering; `displayColor` is **not** in that
list). Every other primvar a shader needs must appear in the glslfx `attributes` block. So a groom
that bakes to `primvars:usdGen:albedo` and binds a shipped shader publishes an array nothing reads;
the tool warns on that combination.

### 1.5 When the bake runs, and what invalidates it

The bake is part of **capture**, never of **evaluate** (S37, and `03-execution-engine.md`): it is a
function of topology, stable ids, root bindings and map samples, all of which are keyed by the
128-bit capture epoch (ADR §4.1). Consequences that are load-bearing for the frame budget:

* Deforming frames never re-run it. `displayColor` is therefore a **static** primvar for the life of
  a capture epoch, which is what keeps a deforming groom on Storm's `DirtyPoints` fast path (ADR
  §5.4; S30's "never co-dirty `displayColor` with `points`" is the same rule stated as invalidation
  discipline).
* A colour-only change (an edit to `usdGen:look:*`, or a map reload) dirties
  `primvars/displayColor/primvarValue` on the affected tiles and nothing else. That maps to
  `HdChangeTracker::DirtyPrimvar`, and HdSt re-pulls **every** vertex/varying/uniform primvar of
  those prims when it sees it (`research/A5-storm-curves-shading.md` §7). So a rebake is charged
  against the **edit** budget, not the frame budget. This is the reason the pipeline is built the
  way it is.
* The epoch digest that decides whether a rebake is needed includes: surface topology hash, rest
  points hash, seed, the operator's capture parameters, every map asset's resolved path, and the
  session's texture generation counter (§5.5, ADR §4.1, `design/proposal-performance.md` §9.3).
* A primvar *appearing* for the first time — the first time a groom bakes to a new `bakePrimvar` —
  is dirtied as `primvars/<name>` once (ADR §5.2, MEASURED at 2.06 µs, **EV-034**,
  `research/G-tool-loop-array-transport-and-cv-picking.md` §5). A `bakeMode` flip is the same class
  of event for a primvar that already exists: it changes the descriptor's interpolation, so it
  dirties `primvars/<name>` once and **not** `…/primvarValue` (§1.3).

### 1.6 A worked example

```usda
def UsdGenDescription "Hair" (
    prepend apiSchemas = ["UsdGenLookAPI", "MaterialBindingAPI"]
)
{
    rel usdGen:surface   = </Char/Scalp>
    rel usdGen:terminal  = </Char/Groom/Hair/Ops/Frizz>
    rel material:binding = </Char/Looks/HairLook>

    color3f usdGen:look:rootColor      = (0.035, 0.018, 0.008)
    color3f usdGen:look:tipColor       = (0.210, 0.115, 0.045)
    float   usdGen:look:rampExponent   = 1.6
    rel     usdGen:look:colorMap       = </Char/Groom/Hair/Maps/ScalpTint>
    float   usdGen:look:hueJitter      = 0.06
    float   usdGen:look:valueJitter    = 0.12
    uniform token usdGen:look:colorMapMode = "bake"
    uniform int   usdGen:look:jitterSeed   = 0
    uniform token usdGen:look:bakeMode     = "perCurve"
    uniform token usdGen:look:bakeTarget   = "displayColor"

    def Scope "Maps"
    {
        def UsdGenImageMap "ScalpTint" {
            asset usdGen:map:file            = @./maps/scalp_tint.exr@
            token usdGen:map:uvSet           = "st"
            token usdGen:map:colorSpace      = "auto"  # tool-authored; the schema default is "raw" (§5.1)
            token usdGen:map:filter          = "bilinear"
            token usdGen:map:wrap            = "clamp"
            uniform token usdGen:map:channel = "rgb"   # uniform: 02-schema.md 2.12, ADR 9.2 R7
            uniform token usdGen:map:domain  = "root"
        }
        def UsdGenExprMap "LengthVar" {
            string        usdGen:expr:source     = "0.8 + 0.4*rand($id) + 0.3*fbm($Pref*3, 3)"
            uniform token usdGen:expr:returnType = "float"
            uniform int   usdGen:expr:seed       = 0
            rel           usdGen:expr:maps       = [ </Char/Groom/Hair/Maps/ScalpTint> ]
        }
    }
    # ... Ops, Guides, Prototypes, Frozen per the reserved layout (ADR 2.2)
}
```

---

## 2. Storm preview shaders

### 2.1 The files that ship

`usdGenShaders` installs its resources under `lib/usd/usdGenShaders/resources/shaders/`
(`10-build-dependencies-testing.md`).

| File | `materialTag` | Bound to | Tangent source |
|---|---|---|---|
| `usdGenHairPreview.glslfx` | `defaultMaterialTag` (opaque + alpha-to-coverage) | usdGen tiles — the default for scalp hair | the variant gate S-8 selects (§2.5) |
| `usdGenHairPreviewTranslucent.glslfx` | `translucent` (OIT via `HdxOitRenderTask`) | usdGen tiles carrying fine fur / peach fuzz | same |
| `usdGenHairPreviewPrimvar.glslfx` | `defaultMaterialTag` | curves usdGen does **not** own, and wire/mesh contexts | always the `hairTangent` primvar with the screen-derivative fallback (ADR §5.4 Variant B) |

Three files, and three `UsdShade` shader defs to match, is what ADR §9.1 R4 fixes; the `inputs:`
block (C5, §2.2) is identical in all three. Two tags mean two files rather than one parameterised
file because the material tag is baked into the glslfx metadata and *also* splits draw batches, so
it cannot be a parameter (S35; MEASURED, `research/G-storm-hair-look-prototype.md` §2.6 —
`appendix-A-evidence-ledger.md` §2 carries no measurement row for the batch split, so the report
section is the citation, ADR §9.5 R43). The first two rows are byte-identical apart from the one
`"materialTag"` line: diffing the prototype's `usdGenHairPreview.glslfx` against
`usdGenHairPreview_translucent.glslfx` gives exactly one changed line, `:203`. The third row is the
variant the prototype rendered; it exists so the same look is available on curves usdGen did not
synthesize — imported guides in a wire repr, a frozen groom another plugin owns — where Variant A's
preconditions do not hold.

One consequence of the S-8 fork on this file set. If gate S-8 selects **Variant B**, the tangent
block of `usdGenHairPreview.glslfx` and `usdGenHairPreviewTranslucent.glslfx` becomes the Variant B
block and `usdGenHairPreviewPrimvar.glslfx` becomes byte-identical to the first. R4's three files and
three shader defs still ship — the third def keeps its identifier so nothing that already binds
`UsdGenHairPreviewPrimvar` has to be re-authored — and whether it is later retired as a duplicate is
recorded with the S-8 result at the M1 exit, not decided here. And **translucent Variant B is not
shipped in v1**: a fine-fur groom on curves usdGen does not own uses the opaque tag, which is
consistent with §2.7 rule 1 (one tag per description) already forbidding a mixed description.

### 2.2 The `inputs:` block (contract C5), verbatim

Frozen at the end of M1 (ADR §3, C5). The `parameters` and `textures` entries of the glslfx become
the Sdr node's inputs, which become `inputs:` attributes on the `Shader` prim. Nineteen parameters
plus one texture = the 20 inputs Sdr reports (MEASURED, `research/G-storm-hair-look-prototype.md`
§2.1). Defaults are copied verbatim from
`prototypes/storm-hair-look/usdGenHairPreview.glslfx:93-171` (`parameters`) and `:172-178`
(`textures`); the `attributes` block of §2.3 is at `:179-201` and the `metadata` tag at `:202-204`.

| Input | Type | Default | Meaning |
|---|---|---|---|
| `rootColor` | `color3f` | `(0.035, 0.018, 0.008)` | Linear albedo at the root of the strand. |
| `tipColor` | `color3f` | `(0.210, 0.115, 0.045)` | Linear albedo at the tip of the strand. |
| `colorRamp` | `float` | `1.0` | Exponent applied to `hairT` before the root/tip lerp. Authored from `usdGen:look:rampExponent` (§1.1); the two names differ on purpose and only the glslfx one is under C5. |
| `diffuseGain` | `float` | `0.55` | Weight of the Kajiya-Kay `sin(T,L)` diffuse lobe. |
| `diffuseWrap` | `float` | `0.35` | 0 = hard terminator, 1 = fully wrapped; fakes multiple scattering in dense fur. |
| `specular1Gain` | `float` | `0.09` | PEAK reflectance of the primary (R) highlight; the lobe is a unit-peak Gaussian. |
| `specular1Color` | `color3f` | `(1, 1, 1)` | Tint of the R lobe. |
| `specular1Width` | `float` | `0.120` | Angular width of the R lobe. |
| `specular1Shift` | `float` | `-0.045` | Longitudinal shift of the R lobe (Marschner `alpha_R`). |
| `specular2Gain` | `float` | `0.05` | PEAK reflectance of the secondary (TRT) highlight. |
| `specular2Color` | `color3f` | `(1, 1, 1)` | Extra tint on TRT; multiplied by the strand albedo. |
| `specular2Width` | `float` | `0.300` | Angular width of TRT; should exceed `specular1Width`. |
| `specular2Shift` | `float` | `0.090` | Longitudinal shift of TRT (Marschner `alpha_TRT`). |
| `transmissionGain` | `float` | `0.25` | Weight of the cheap forward-scattering (TT) rim term. |
| `transmissionColor` | `color3f` | `(1.0, 0.62, 0.38)` | Tint of the forward-scattering term. |
| `opacity` | `float` | `1.0` | Constant opacity multiplier; multiplied by `displayOpacity` when that primvar exists. |
| `widthFalloff` | `float` | `0.0` | 0 = flat across the strip; 1 = alpha falls to 0 at the strand silhouette. |
| `randomHue` | `float` | `0.0` | Per-curve hue jitter driven by `hairId`. |
| `randomValue` | `float` | `0.0` | Per-curve brightness jitter driven by `hairId`. |
| `selfOcclusion` | `float` | `0.5` | Canopy self-shadow driven by `hairT`; scales the direct lobes and the dome irradiance alike. |
| `rootColorMap` | texture (`color3f`) | `(1, 1, 1)` | Multiplied into the strand albedo. Wire to a `UsdUVTexture` whose `st` comes from a `UsdPrimvarReader_float2` on the per-curve root UV. Unconnected ⇒ the declared white fallback. |

The four specular defaults above were retuned in M1 against the shipped
viewport (the lobe is now a **unit-peak** Gaussian, so a gain IS a peak
reflectance); `docs/freezes/C5.md` §"Default retune" records the old values and
why. C5 freezes the names and their order, not the defaults.

The white fallback is not incidental: `HioGlslfxConfig` honours a `"default"` key in the `textures`
block (`pxr/imaging/hio/glslfxConfig.cpp:34` maps `defVal → "default"`, read at `:548-549`), so an
unconnected texture input yields white rather than the parser's black
(`pxr/usdImaging/plugin/sdrGlslfx/parserPlugin.cpp:236-242`). Without it, every groom with no scalp
map would render black.

Array-typed parameters (`float[2]`, `float[4]`) are **legal** — they bind as `vec2`/`vec4` through
`SdrShaderProperty::GetTypeAsSdfType()` folding Float+arraySize (MEASURED,
`research/G-storm-hair-look-prototype.md` §2.4) — but the shipped file uses none and C5 adds none.

### 2.3 The `attributes` block

```json
"attributes": {
    "hairT":       { "type": "float", "default": 0.0 },
    "hairTangent": { "type": "vec3",  "default": [0.0, 0.0, 1.0] },
    "hairId":      { "type": "float", "default": 0.0 },
    "st":          { "type": "vec2",  "default": [0.0, 0.0] }
}
```

`SdrGlslfxParserPlugin` joins these names into the node's `primvars` metadata with `|`
(`pxr/usdImaging/plugin/sdrGlslfx/parserPlugin.cpp:262-275`), which is what survives Storm's primvar
filtering — MEASURED as `hairId|hairT|hairTangent|st`. Each becomes an `HdGet_<name>()` accessor,
uniform ones resolving through `GetAggregatedElementID()` (the curve index) and vertex ones through
the interstage block (MEASURED accessor-by-accessor,
`research/G-storm-hair-look-prototype.md` §2.4).

`hairId` is declared `"type": "float"`. An `int` primvar will not bind, the accessor silently reads
0.0, and per-curve jitter dies with no error — the reason ADR §1 (S29) fixes `hairId` as a uniform
float in [0,1). Under Variant A (§2.5) the `hairTangent` entry is removed from this block so the
primvar is not requested. That is legal because **C5 covers the `parameters` and `textures` entries
of the `configuration` block — the `inputs:` block of §2.2 — and nothing else** (ADR §3: "glslfx
parameter names (the shipped file's `inputs:` block, verbatim)"). The `attributes` and `metadata`
blocks are outside the contract and differ between the shipped files by design.

### 2.4 Discovery and registration

`usdGenShaders` ships `UsdGenShadersDiscoveryPlugin`, a `SdrDiscoveryPlugin` subclass modelled
exactly on OpenUSD's own `UsdShadersDiscoveryPlugin`
(`pxr/usd/plugin/usdShaders/discoveryPlugin.cpp:30-70`): it resolves its `shaders` resource
directory through `PlugFindPluginResource`, opens `shaders/shaderDefs.usda` on a stage, and hands
each `Shader` prim to `UsdShadeShaderDefUtils::GetDiscoveryResults`. Its plugInfo entry declares
`"bases": ["SdrDiscoveryPlugin"]`, `"Type": "library"` with the generated `LibraryPath` pattern, and
`"ShaderResources": "shaders"` so `HioGlslfx` can resolve the `.glslfx` files — mirroring
`pxr/usd/plugin/usdShaders/plugInfo.json` (`10-build-dependencies-testing.md` has the full file).

```usda
#usda 1.0
def Shader "UsdGenHairPreview" (doc = "usdGen hair preview surface for Storm (opaque + A2C)")
{
    uniform token info:implementationSource = "sourceAsset"
    uniform asset info:glslfx:sourceAsset   = @./usdGenHairPreview.glslfx@
    color3f inputs:rootColor = (0.035, 0.018, 0.008)
    # ... the other 18 inputs of 2.2, with the same defaults ...
    token outputs:surface (sdrMetadata = { string renderType = "terminal surface" })
}
def Shader "UsdGenHairPreviewTranslucent" ( ... @./usdGenHairPreviewTranslucent.glslfx@ ... ) { ... }
def Shader "UsdGenHairPreviewPrimvar"     ( ... @./usdGenHairPreviewPrimvar.glslfx@ ... )     { ... }
```

**Identifier correction.** The source chapters name these shader defs `usdGen:HairPreview` /
`usdGen:HairPreviewTranslucent` (ADR §6 via `design/proposal-artist.md` §7.1). That identifier is
not authorable: the Sdr identifier *is* the prim name (`pxr/usd/usdShade/shaderDefUtils.cpp:51-52`,
`const TfToken &identifier = shaderDefPrim.GetName();`) and a USD prim name cannot contain `:`
(verified: `Sdf.Path.IsValidIdentifier('usdGen:HairPreview') == False`). The identifiers are
therefore **`UsdGenHairPreview`**, **`UsdGenHairPreviewTranslucent`** and
**`UsdGenHairPreviewPrimvar`**, with no underscores — `SdrFsHelpersSplitShaderIdentifier` tokenises
on `_` and treats trailing numeric tokens as a version
(`pxr/usd/sdr/filesystemDiscoveryHelpers.cpp:127-175`). `usdGen:HairPreview` survives as the
*display* name only.

Both discovery paths work (MEASURED, `research/G-storm-hair-look-prototype.md` §2.1):
`info:glslfx:sourceAsset` through `Sdr.Registry().GetShaderNodeFromAsset(..., sourceType="glslfx")`
and `info:glslfx:sourceCode` through `GetShaderNodeFromSourceCode`. usdGen ships the asset form.

### 2.5 The tangent policy (ADR §5.4, gate S-8)

The strand tangent is the single decision that separates "hair" from "shiny plastic tubes". There
are three sources and all three were measured:

| Source | Result | Evidence |
|---|---|---|
| `inData.Neye` — the tangent Storm's `orient()` stores for ROUND/HALFTUBE/implicit-ribbon layouts | Works at refineLevel ≥ 1. **Hard-fails to compile** at refineLevel 0, with no `widths`, or on any mesh, because the WIRE layout's block has only `Peye`; Storm then silently substitutes the fallback shader | MEASURED, `research/G-storm-hair-look-prototype.md` §2.5; `pxr/imaging/hdSt/shaders/basisCurves.glslfx:1212-1218`, `:866`, `:889`, `:1272-1274`; failure text at `pxr/imaging/hdSt/glslProgram.cpp:211` and `pxr/imaging/hdSt/drawBatch.cpp:385` |
| a `vertex vec3 hairTangent` primvar, object space, transformed by `GetWorldToViewMatrix() * HdGet_transform()` | Best image: a clean, coherent highlight band | MEASURED, `prototypes/storm-hair-look/hair_pv_tangent.png` |
| screen-derivative reconstruction `T = cross(N, dPdv)` | Works everywhere and costs no data, but is visibly sparkly on 1–2 px strands | MEASURED (visual), `research/G-storm-hair-look-prototype.md` §2.5 (image `hair_deriv_tangent.png`, not carried in; regenerate with `prototypes/storm-hair-look/render_hair.cpp`) |

ADR §5.4 turns this into a measured fork with one primvar policy:

* **Variant A (default on usdGen tiles).** Tangent from `inData.Neye`. This narrows S35's blanket
  "never read `inData`" on ADR §5.4's authority, and only under three preconditions that hold for
  usdGen tiles and nothing else: the scene index pins `displayStyle/refineLevel = 2`, it always
  publishes `widths`, and it binds this shader to nothing but its own tiles (ADR §5.3, §5.4). No
  `hairTangent` primvar is published, so a deforming frame changes `points` and nothing else and
  stays on Storm's `DirtyPoints` fast path.
* **Variant B (`usdGenHairPreviewPrimvar.glslfx`).** Tangent from the `hairTangent` vertex primvar
  under `#ifdef HD_HAS_hairTangent`, with the screen-derivative path as the graceful fallback and a
  final view-aligned fallback for the wire repr — verbatim the `UsdGenHair.Tangent` block of the
  prototype. Cost: 12 B/CV (a `vector3f`, 3 × 4 B) → **19.2 MB at 1.6 M CVs** —
  `DERIVED from EV-075`, which records exactly 19.2 MB per `float3` sample at this size
  (`appendix-A-evidence-ledger.md` §2.9; ADR §9.5 R42).
* **Gate S-8 (EGL harness, M1)** measures A vs B frame time on a 100 k deforming groom and confirms
  that A compiles with `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` — the Metal/Vulkan proxy, since on
  HgiGL the GLSL resource path is the one that runs (`pxr/imaging/hdSt/codeGen.cpp:166`, `:189-195`).
  If A fails either check, B becomes the default and `hairTangent` is republished on every deforming
  frame at a cost of roughly **+1.5–2.5 ms at 100 k × 8 CV** — `DERIVED from EV-022`, not measured
  (ADR §5.4, §9.5 R42, which classifies this figure explicitly; the provenance row is
  `appendix-A-evidence-ledger.md` §2.11); gate S-8 is what measures it. The `DirtyPrimvar`
  path re-uploads every non-points primvar, which is where the figure comes from.

Note the one thing Variant B cannot do: `ApplyInstanceTransform()` is not available in the fragment
stage (the `Instancing.Transform` mixin is VS/TES/PTVS only,
`pxr/imaging/hdSt/basisCurvesShaderKey.cpp:202,284,319,367,395,435`), so for point-instanced curve
prototypes with a rotating instance transform the primvar tangent is wrong. Grooms inside instancers
therefore do not publish `hairTangent` and fall through to the derivative path
(`06-imaging.md` covers instancer prototypes).

### 2.6 refineLevel and widths

* Tiles pin `displayStyle/refineLevel = 2` (RIBBON + ROUND: one quad column, smooth fake-tube
  normal). Level 3 costs nothing extra on thin strands because width-wise tessellation only adds
  columns above ~10 px (MEASURED, **EV-023** over the sweep **EV-019**/**EV-020**/**EV-021**,
  `appendix-A-evidence-ledger.md` §2.3; `research/G-storm-hair-look-prototype.md` §5).
* Level 0 is **slower** than level 1 at 200 k curves (12.43 vs 8.17 ms, MEASURED, **EV-021**): at level 0 cubic
  curves are drawn as a `LineList` polyline through every CV. Interaction LOD is curve decimation,
  never a refineLevel drop; gate S-9 decides separately whether a refineLevel-1 "tumble tier" ships
  (ADR §5.5).
* `widths` is required for anything but wire. Publish `vertex` (or a single `constant` when the
  width is uniform) and never `varying`: varying costs a measured 6.48 ms to expand 600 000 →
  800 000 values on 100 k curves (MEASURED, **EV-033**;
  `research/G-storm-throughput-and-prim-granularity.md` §2 "Topology strategy" item 5;
  `pxr/imaging/hdSt/basisCurvesComputations.h:69-158` defines `HdSt_ExpandVarying`, `:236-250` is the
  varying branch and `:248` the call).
  The shader's `widthFalloff` input, not the width itself, is what softens the silhouette.
* `minScreenSpaceWidths = 1.0` (constant) keeps sub-pixel strands from aliasing away.
* usdview's complexity presets map Low/Medium/High/Very High → refineLevel 0/1/2/3. The two halves
  live in two files: the preset names and their complexity *floats* (1.0 / 1.1 / 1.2 / 1.3) at
  `pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43`, and the complexity→refineLevel conversion in
  `UsdImagingGLEngine`'s `_GetRefineLevel` at `pxr/usdImaging/usdImagingGL/engine.cpp:2317-2350`
  (MEASURED and recorded in `research/G-storm-hair-look-prototype.md` §5). The per-prim
  `displayStyle` the scene index authors is what makes usdGen tiles independent of that. Whether the
  per-prim override always wins over the app's complexity is UNMEASURED and is checked by gate L-2.

### 2.7 Material tag, alpha, and the translucent ordering caveat

Tag precedence is `displayInOverlay > translucentToSelection > material opinion > displayOpacity →
masked > default` (`pxr/imaging/hdSt/primUtils.cpp:292-327`), and the strongest material opinion is
the glslfx's own `"metadata": { "materialTag": ... }`
(`pxr/imaging/hdSt/materialNetwork.cpp:66-80`). Therefore:

* With a usdGen material bound, a `displayOpacity` primvar does **nothing** for blending (MEASURED).
  With no material bound, `displayOpacity` promotes the draw item to `masked` — screen-door dither,
  not translucency (MEASURED, `research/G-storm-hair-look-prototype.md` §2.6). `displayOpacity` is
  not an opacity control and the tool never offers it as one.
* `defaultMaterialTag` gives `blendEnable = false, depthMask = true, enableAlphaToCoverage = true`
  (`pxr/imaging/hdx/taskController.cpp:392-400`) — the mode that antialiases thin strands. A2C only
  bites with a multisampled AOV, which is why the quality comparison is a tier-4 gate (R-2).
* `"translucent"` routes the prims to `HdxOitRenderTask`
  (`pxr/imaging/hdx/taskController.cpp:331-336`), i.e. order-independent transparency — visibly
  better for fine fur (MEASURED, `prototypes/storm-hair-look/hair_translucent.png`).

**The ordering caveat.** OIT is order-independent *within* the translucent pass, but the pass runs
after the opaque one and writes into its own per-pixel buffers, from which only the resolved AOVs
are read afterwards (`pxr/imaging/hdx/taskController.cpp:333-336`, `:409-419`). Two consequences,
both design rules:

1. **One tag per description.** Mixing `defaultMaterialTag` and `translucent` tiles in one
   description splits the draw batch and breaks gate S-5 (`drawBatches == 1`) as surely as mixing
   materials or refineLevels does (S30). Choose the tag at the description level; a groom that needs
   both is two descriptions.
2. Translucent hair does not write depth, so an opaque prop intersecting it will not occlude it
   correctly inside the strand volume. For scalp hair over an opaque scalp mesh — the normal case —
   this is invisible. It is called out because the failure looks like a groom bug and is not.

### 2.8 The rendered evidence

Three images are carried into the repository as golden references. The rest were produced in the
research session's scratchpad, which no longer exists; cite the report section that records each
result, never a scratchpad path, and regenerate the image with the harness
(`prototypes/storm-hair-look/render_hair.cpp` and `bench_hair.cpp`, run through `eglctx.h`) when one
is needed.

| Image | Proves |
|---|---|
| `prototypes/storm-hair-look/hair_pv_tangent.png` | The shipped glslfx compiles and renders in Storm at refineLevel 2, with a coherent tangent-driven highlight. The primary golden image for gate L-2. |
| `prototypes/storm-hair-look/hair_translucent.png` | The `translucent`/OIT variant blends correctly through overlapping strands (same scene, `opacity = 0.35`, `widthFalloff = 1`). |
| `prototypes/storm-hair-look/hair_preview_tex.png` | The route-3 fallback: `UsdPreviewSurface` + `UsdPrimvarReader_float2` on a **uniform** `st` + `UsdUVTexture` gives a per-curve root-UV lookup — the measurement that validates the whole "bake maps to per-curve primvars / sample scalp colour by `st`" strategy. |
| `hair_deriv_tangent.png`, `hair_indata_patch.png`, `hair_indata_wire.png`, `hair_mtlx_light.png`, `hair_dispopacity.png`, `hair_camlight.png` | Not carried into `plan/prototypes/`; the results they record live in `research/G-storm-hair-look-prototype.md` §2.5, §2.6, §2.7 and §3, which is what to cite. The file names are regeneration hints for `render_hair.cpp`, nothing more. |

The scenes are `prototypes/storm-hair-look/hair_scene.usda` (4 000 curves × 8 CV on a sphere scalp,
with the full C2 primvar set), `hair_scene_preview.usda` and `hair_scene_mtlx2.usda`.

### 2.9 Rules the shaders obey, and why

1. **Root→tip comes from the `hairT` primvar, never from `patchCoord`**, whose `y` runs *across*
   the width (`basisCurves.glslfx:1293`, TES at `:746`, wire repr at `:1244`). `patchCoord.y` is good
   for silhouette effects only — which is what `widthFalloff` uses it for.
2. **Never read `inData`** from a shader that may be bound outside usdGen tiles — that is S35's
   rule, and it is what `usdGenHairPreviewPrimvar.glslfx` obeys. The only exception is Variant A on
   usdGen's own tiles, under the three preconditions of §2.5.
3. **No `normals`.** Authored normals force ORIENTED ribbons
   (`pxr/imaging/hdSt/basisCurves.cpp:324-327`) and lose the fake-tube normal that reads as hair.
4. **No `#import`.** The prototype defines `UGH_PI` itself rather than importing
   `surfaceHelpers.glslfx`: the import resolves through `HioGlslfx`'s plugin resource registry and
   adds a failure mode for nothing.
5. **Guard every optional accessor** with `HD_HAS_<name>`; a groom omitting `hairT`, `hairId`,
   `displayColor` or `displayOpacity` must still render.
6. **Lights may be absent** (`NUM_LIGHTS` can be 0; the light loop and dome-light block are
   `#if`-guarded). Separately, `UsdAppUtilsFrameRecorder` with `SetCameraLightEnabled(false)` renders
   black even with `UsdLux` lights in the stage (MEASURED, §2.7 of the prototype report;
   `pxr/usdImaging/usdAppUtils/frameRecorder.cpp:436-454`), so the harness sets lighting explicitly.

---

## 3. Storm binding

### 3.1 One `Material`, three terminals

```usda
def Material "HairLook"
{
    token outputs:surface.connect        = </Char/Looks/HairLook/preview.outputs:surface>
    token outputs:glslfx:surface.connect = </Char/Looks/HairLook/storm.outputs:surface>
    token outputs:mtlx:surface.connect   = </Char/Looks/HairLook/mtlx/surface.outputs:out>

    def Shader "storm" {
        uniform token info:id = "UsdGenHairPreview"   # the Sdr identifier §2.4 registers;
                                                      # info:implementationSource defaults to "id"
        color3f inputs:rootColor   = (0.035, 0.018, 0.008)
        color3f inputs:tipColor    = (0.210, 0.115, 0.045)
        float   inputs:colorRamp   = 1.6
        float   inputs:randomHue   = 0.06
        float   inputs:randomValue = 0.12
        # ... the remaining inputs of 2.2 ...
        token   outputs:surface
    }
    def Shader "preview" {                     # universal fallback, every delegate
        uniform token info:id = "UsdPreviewSurface"
        # inputs:diffuseColor <- UsdPrimvarReader_float3(varname="displayColor")
        token outputs:surface
    }
    def Scope "mtlx" { }                       # 4.1
}
```

The five inputs shown are the ones the tool copies from `UsdGenLookAPI` (§1.1); the rest are shader
craft. The glslfx terminal is authored on `outputs:glslfx:surface`, not on the plain
`outputs:surface`, because the plain terminal belongs to the universal fallback.

**The Material references the shader by its registered Sdr identifier, never by asset path.** HdSt
resolves the `.glslfx` from the *node identifier*: `_GetGlslfxForTerminal` calls
`SdrRegistry::GetShaderNodeByIdentifierAndType(nodeTypeId, HioGlslfxTokens->glslfx)` and then
`GetResolvedImplementationURI()` (`pxr/imaging/hdSt/materialNetwork.cpp:120-135`), and UsdImaging
builds that identifier from `UsdShadeNodeDefAPI::GetShaderId()` — falling back to re-parsing the
authored asset through `GetShaderNodeForSourceType(renderContext)` only when
`info:implementationSource` is not `"id"` (`pxr/usdImaging/usdImaging/dataSourceMaterial.cpp:554-569`).
A relative `info:glslfx:sourceAsset` authored into a show layer would resolve against **that layer**,
not against `lib/usd/usdGenShaders/resources/shaders/`, so the shader would not be found on any real
stage — and it would defeat the discovery plugin §2.4 exists to ship. `info:glslfx:sourceAsset`
therefore appears in exactly one file, `shaderDefs.usda` (§2.4).

### 3.2 How Storm resolves the terminal (gate L-1)

The brief marks this UNVERIFIED (S36). The source chain is complete and was re-read for this
document; it says `outputs:glslfx:surface` wins:

1. UsdImaging's material data source exposes **one child per render context**, derived from the
   output's namespace: `outputs:surface → ""` (universal), `outputs:ri:surface → "ri"`,
   `outputs:glslfx:surface → "glslfx"`
   (`pxr/usdImaging/usdImaging/dataSourceMaterial.cpp:54-66`, names assembled at `:676-700`).
2. `HdStRenderDelegate` declares its material render contexts as `{glslfx, mtlx}`, in that order
   (`pxr/imaging/hdSt/renderDelegate.cpp:695-707` — `_RenderDelegateInfo()`, with the
   `info.materialRenderContexts = {...}` assignment at `:702-707`; ADR §9.5 R43 fixes `:695-707` as
   the accepted range), and its shader source types as the same list (`:710`).
3. `HdMaterialSchema::GetMaterialNetwork(renderContexts)` returns the **first** context present in
   that order and only falls back to the universal context when none matches
   (`pxr/imaging/hd/materialSchema.cpp:61-75`).
4. Backend emulation calls exactly that with the delegate's list
   (`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:1331-1339`), and the same list drives
   material dirty-bit translation (`:312-316`, `:472-476`).
5. Independently, `HdsiMaterialRenderContextFilteringSceneIndex` exists to express the same
   preference as a scene index — "the first render context encountered in
   `renderContextPriorityOrder`" (`pxr/imaging/hdsi/materialRenderContextFilteringSceneIndex.h:20-45`).

**Gate L-1** (tier 2, M1) confirms it at runtime rather than on paper: build the three-terminal
material, render through the EGL harness, and assert that the fragment shader contains
`UsdGenHair.Surface` and not the UsdPreviewSurface source. Until L-1 is green the plan behaves as if
the answer were unknown, which costs one env var and about eighty lines of code.

### 3.3 The override path

`USDGEN_STORM_MATERIAL_OVERRIDE` ships **default OFF** (ADR §1 S36, §5.6). When set to `1` — or when
gate L-1 fails, in which case the shipped default flips and this becomes the normal path — the scene
index:

1. reads its renderer display name from `inputArgs`
   (`__rendererDisplayName`, `pxr/imaging/hd/sceneIndexPluginRegistry.h:28`), and acts only when it
   is `"GL"` (S2 lists the display names usdGen must accept);
2. synthesizes `<Description>/__usdGenRender/material_storm` (ADR §2.2) whose **plain**
   `outputs:surface` is the glslfx network, with `primOrigin` set to the `UsdGenDescription` path so
   picking still selects the description;
3. authors `materialBindings/allPurpose` on its own tiles to point at it, by hand — like `purpose`
   and `visibility`, because usdGen runs post-flattening (ADR §5.3);
4. leaves the authored binding alone for every non-Storm delegate.

This path is written to be **deleted**, not maintained: when L-1 is green in CI on two consecutive
releases, the env var, the synthesized prim and the branch go away.

### 3.4 Batching rules

One material and one refineLevel per tile set, or the draw batch fragments (S30, and
`research/G-storm-throughput-and-prim-granularity.md` §1.11 "Culling, batching, draw calls"). Gate S-5 asserts
`drawBatches == 1 && drawCalls == 1` for 49 tiles with one material. In practice this means:

* the description's material binding is **copied by hand** onto every tile it owns — usdGen
  publishes post-flattening (S4), so `materialBindings/allPurpose` is authored per tile from the
  description's binding exactly as `purpose` and `visibility` are (ADR §5.3). Nothing is inherited;
  per-tile material variation is not offered in v1;
* guide prims (`__usdGenRender/guides/<setName>`, `purpose = guide`) are a separate binding and a
  separate batch by construction, which is fine — they are few;
* card/archive instancers get material variety through *multiple prototypes*, because Storm has no
  per-instance material (S33; see `06-imaging.md`).

---

## 4. The render material

### 4.1 The MaterialX chain

Authored on `outputs:mtlx:surface` of the same `Material` prim. The chain is
(`design/proposal-performance.md` §9.1, MEASURED end-to-end in
`research/G-storm-hair-look-prototype.md` §3):

```
ND_deon_hair_absorption_from_melanin   ->  ND_chiang_hair_roughness  ->  ND_chiang_hair_bsdf  ->  ND_surface
        (melanin_concentration,               (longitudinal, azimuthal,        (tint_R/TT/TRT, ior 1.55,
         melanin_redness)                      scale_TT, scale_TRT)             cuticle_angle, curve_direction)
ND_geompropvalue_vector3(geomprop = "hairTangent")  ->  ND_transformvector_vector3(fromspace = "object",
                                       tospace = "world")  ->  ND_chiang_hair_bsdf.curve_direction
```

Nodedefs, all re-read in the install for this document: `ND_chiang_hair_bsdf` at
`/home/burkard/work/OpenUSD_26_08/libraries/pbrlib/pbrlib_defs.mtlx:146-158` (with
`curve_direction … defaultgeomprop="Tworld"` at `:157`), `ND_deon_hair_absorption_from_melanin` at
`:433-439`, `ND_chiang_hair_roughness` at `:455-463` — the file is 465 lines long, so any citation
past `:463` is out of range — and `ND_transformvector_vector3` at
`/home/burkard/work/OpenUSD_26_08/libraries/stdlib/stdlib_defs.mtlx:2821`, whose `fromspace` and
`tospace` string inputs are at `:2823-2824` (`research/A7-prior-art-grooming.md` §5.3).

The chain ships as a document rather than as four nodes re-authored per Material: `usdGenShaders`
installs `usdGenHair.mtlx` beside its glslfx files, at
`lib/usd/usdGenShaders/resources/usdGenHair.mtlx` (`10-build-dependencies-testing.md`, the
`usdGenShaders` row of its target table and its install tree). The tool's **Create hair material**
action references that document's node graph from `outputs:mtlx:surface`; it does not re-author the
four nodes per Material.

**The explicit tangent geomprop is mandatory.** `curve_direction` declares
`defaultgeomprop="Tworld"`, and `HdStMaterialXShaderGen` computes `Tworld` as a TBN tangent when a
texcoord primvar is present and otherwise as `cross(normalWorld, (0,1,0))`
(`pxr/imaging/hdSt/materialXShaderGen.cpp:29-46`). On curves with no texcoord node in the network
the `#else` branch is taken and the "strand direction" is unrelated to the hair. Wiring an explicit
`ND_geompropvalue_vector3` fixes it — verified in the regenerated GLSL dump, where
`#define HD_HAS_hairTangent 1`, `HdGet_hairTangent()` and
`..._curvedirection_out = vd.i_geomprop_hairTangent` all appear
(`research/G-storm-hair-look-prototype.md` §3).

**The object→world transform is a node, not a second primvar.** usdGen's `hairTangent` primvar is
object space (the prototype's scene had an identity transform, which is why the probe worked with no
transform node at all). ADR §9.4 R33 settles the bridge: *"the render MaterialX network transforms
the object-space `hairTangent` (or derives it from positions) with a node; tiles publish no
context-dependent primvars."* So there is **no `hairTangentWorld`** — not in the render context, not
anywhere; a tile's primvar set does not vary with the session context (ADR §5.3).

The render material therefore needs the object-space `hairTangent` on the tile, and which
`hairTangent` policy lands in C2 is gate **S-8**'s decision, on the row `06-imaging.md` §4.1 owns:
today it reads *"absent by default (variant A); `VtVec3fArray`, `vertex`, object space when gate S-8
selects variant B"*. If S-8 selects Variant A there is no `hairTangent` on the tiles and the render
network derives `curve_direction` from positions instead (ADR §9.4 R33) — in practice the renderer's own curve
derivative, which is what a studio `outputs:ri:surface` shader uses (§4.4). Whether a stock MaterialX
node graph can express that derivative without a primvar is UNVERIFIED and is on no critical path:
the MaterialX terminal is the portable fallback, not the shipped production look.

How `hairTangent` is computed when Variant B publishes it: **central differences of `points` at
publish** — the object-space formula of `design/proposal-risk.md` §7.2, one pass over the tile's CV
buffer, normalised, with forward and backward differences at the two ends. Nothing multiplies it:
`xform/matrix` on the tile is the surface's world matrix and points are surface-local
(`06-imaging.md` §4.1, S4), which is exactly the `fromspace = "object"` the node names.

**Storm shows route 1, never route 2.** The MaterialX hair network compiles and renders in Storm
with no warning, and looks near-black even at melanin 0.03, because Storm's MaterialX light loop
integrates a single reflection closure per light and the hair BSDF's energy is mostly in the
TT/TRT transmission lobes (MEASURED; the closure-level explanation is UNVERIFIED and is not on any
critical path). The three-terminal material means Storm never sees it.

### 4.2 Primvars the render material consumes

| Primvar | Interp | Used for |
|---|---|---|
| `points`, `widths` | vertex | geometry; hdPrman maps `widths → RixStr.k_width` (`third_party/renderman/plugin/hdPrman/renderParam.cpp:497-504`) |
| `hairTangent` | vertex | `curve_direction`, after the `ND_transformvector_vector3` object→world node of §4.1. Present only under Variant B; absent under Variant A, where the direction comes from the renderer's curve derivative (ADR §5.4, §9.4 R33) |
| `hairT` | vertex | root→tip variation in a studio shader |
| `hairId` | uniform | per-strand randomisation |
| `st` | uniform | root-UV texture lookups on the scalp |
| `displayColor` | `uniform` at `bakeMode = "perCurve"`, `vertex` at `perCV` (§1.3) | the §1.2 bake; a studio shader multiplies it into `tint_R/TT/TRT` or into base colour |
| `clumpId_<level>`, `guideIndex`/`guideWeight` | uniform | operator-emitted, available to a shader that wants per-clump variation (`04-operators.md`) |

hdPrman has no hair-specific attributes of its own (`research/A7-prior-art-grooming.md` §5.4); the
look lives entirely in the material network, which is why the primvar set is the contract.

### 4.3 The universal fallback

`outputs:surface` is a `UsdPreviewSurface` whose `diffuseColor` is a `UsdPrimvarReader_float3` on
`displayColor`, plus optionally a `UsdUVTexture` on the uniform `st` for a scalp map. It is
isotropic GGX and does not look like hair — that is the point. It answers a delegate that supports
neither `glslfx` nor `mtlx`, a stage opened where usdGen is not installed (the tiles are gone, but
the bound material still resolves for frozen curves), and USD-native round-tripping.

### 4.4 Swapping in a studio hair shader

A studio replaces the render look by re-targeting one connection:

```usda
over "HairLook" {
    token outputs:ri:surface.connect = </Show/Looks/StudioHair.outputs:out>
}
```

`HdPrmanRenderDelegate::GetMaterialRenderContexts()` returns `{ri, mtlx}`
(`third_party/renderman/plugin/hdPrman/renderDelegate.cpp:803-810`), so `outputs:ri:surface` is
resolved ahead of the MaterialX terminal by the §3.2 mechanism, and Storm ignores it because `ri` is
not in `{glslfx, mtlx}` — the Storm preview and the universal fallback are untouched. The
requirements on the replacement are exactly the primvar table of §4.2, and the
single hard rule is that whatever it uses for strand direction must be fed a real tangent: either a
geomprop on the object-space `hairTangent` put through an object→world transform node (§4.1), or the
renderer's own curve derivative — which is the only route when gate S-8 selects Variant A and no
`hairTangent` is published. usdGen never authors into `outputs:ri:` itself.

A studio that also wants its own Storm preview overrides `outputs:glslfx:surface` the same way; the
tiles' primvar set is unchanged, and the only usdGen setting to revisit is `bakeMode` (§1.3), since
a shader that applies its own ramp must not receive a per-CV baked one.

---

## 5. Maps — the plugin's texture-loading prims

### 5.1 The prim types

`UsdGenMap` is abstract and derives from `UsdTyped` (ADR §2.1) — deliberately **not** from
`UsdGenOperator`, so it inherits none of `usdGen:input`, `readPhase` or `space`, which are
meaningless on a map. Map prims live under the reserved `<Description>/Maps/` scope (ADR §2.2).
Property names follow the schema document's namespaced form (`usdGen:map:*`, `usdGen:expr:*`,
`usdGen:paint:*`, `usdGen:noise:*`, `usdGen:combine:*`, `usdGen:proximity:*`) — `02-schema.md`
§2.12 is the normative registry (ADR §9.2 R7), and where `design/proposal-risk.md` §3.6 used a flat
form, 02 took `design/proposal-artist.md` §3.5's namespaced form under ADR §8.

Common to every `UsdGenMap`:

| Property | Type | Default | Meaning |
|---|---|---|---|
| `usdGen:map:domain` | `uniform token` | `"root"` | `root` (one sample per curve, at the root) or `cv` (one per CV). Under `cv` the sample's `P`, `N` and `t` are the **CV's** while `st`, `faceId` and `faceUV` stay the **root's** — a CV has no surface projection (§5.2) — so `cv` changes the value only for maps that read position or `t` (`UsdGenExprMap`, `UsdGenNoiseMap`, `UsdGenGuideProximityMap`); for the three UV/face-addressed types it repeats the root value and the tool warns. `cv` multiplies the sample count by the CV count and is opt-in; in the look bake it is legal only at `usdGen:look:bakeMode = "perCV"` (§1.2 step 1). |
| `usdGen:map:channel` | `uniform token` | `"r"` | `r \| g \| b \| a \| rgb \| luminance`. |
| `usdGen:map:scale` | `float` | `1.0` | Applied after the channel selection. |
| `usdGen:map:offset` | `float` | `0.0` | Applied after `scale`. |
| `usdGen:map:clamp` | `float2` | `(0, 1)` | Output clamp; an empty (equal) range means no clamp. |
| `usdGen:map:default` | `float` | `0.0` | The value a failed sample returns (missing file, parse error, unbound face, out-of-range channel). Never an exception in a capture loop; one `TF_WARN` per map per session. It is what the error policy of §5.5 and §7.7 returns, and `02-schema.md` §2.12 carries the row (R7). |

`usdGen:label` (`string`, `""`, cosmetic) is on every map too; `02-schema.md` §2.12 carries it and
this document has nothing to add about it.

Concrete types:

| Type | Key properties | Backend |
|---|---|---|
| `UsdGenImageMap` | `asset usdGen:map:file`, `token usdGen:map:uvSet = "st"`, `token usdGen:map:wrap = "clamp"` (`clamp\|repeat\|mirror\|black`), `token usdGen:map:filter = "bilinear"` (`nearest\|bilinear`), `token usdGen:map:colorSpace = "raw"` (`auto\|raw\|sRGB`) | `HioImage` (§5.4) |
| `UsdGenPtexMap` | `asset usdGen:map:file` (`.ptx`), `token usdGen:map:filter = "bilinear"` (`nearest \| bilinear \| box \| gaussian \| bicubic \| bspline \| catmullrom \| mitchell` — the eight `PtexFilter::FilterType` values of §5.3, a **different** token set from `UsdGenImageMap`'s two), `float usdGen:map:blur = 0`, `int usdGen:map:firstChannel = 0`, `int usdGen:map:channelCount = 1`, `token usdGen:map:borderMode = "clamp"` (`clamp\|black\|periodic`). `firstChannel`/`channelCount` select the texels `PtexFilter::eval` returns; the common `usdGen:map:channel` then selects *within* that window and must be consistent with it (`channelCount = 1` ⇒ only `r`; `channelCount = 3` ⇒ `rgb` or `luminance`). An inconsistent pair is a compile error naming both. | vendored Ptex 2.4.3 (§6) |
| `UsdGenExprMap` | `string usdGen:expr:source`, `uniform token usdGen:expr:returnType = "float"` (`float\|color`), `uniform int usdGen:expr:seed = 0`, `rel usdGen:expr:maps` (the maps reachable from `map()`/`ptex()`) | vendored SeExpr (§7) |
| `UsdGenPaintMap` | `rel usdGen:paint:surface`, `token usdGen:paint:primvar = "usdGen:paint:density"`, `token usdGen:paint:interpolation = "faceVarying"`, `token usdGen:paint:storage = "primvar"` (`primvar\|file`), `int usdGen:paint:resolution = 256`, `asset usdGen:paint:bakedFile` | primvar or file (§8) |
| `UsdGenNoiseMap` | `token usdGen:noise:type = "fbm"` (`perlin\|snoise\|fbm\|turbulence\|cellnoise\|voronoi`), `float usdGen:noise:frequency = 1`, `float usdGen:noise:lacunarity = 2`, `float usdGen:noise:gain = 0.5`, `int usdGen:noise:octaves = 3`, `uniform int usdGen:noise:seed = 0`, `token usdGen:noise:space = "rest"` (`rest \| deformed` — there is **no** `world` token, because post-flattening deformed space *is* world space, ADR §9.2 R9 and S4) | SeExpr's `Noise.h` (§7.4) |
| `UsdGenCombineMap` | `rel usdGen:combine:inputs` (ordered), `token usdGen:combine:mode = "multiply"` (`multiply\|add\|subtract\|max\|min\|average\|overlay`) | pure arithmetic |
| `UsdGenGuideProximityMap` | `rel usdGen:guides`, `float usdGen:proximity:radius = 1`, `float usdGen:proximity:decay = 2` | the reference-lane kd-tree (`03-execution-engine.md`) |

**One `asset` per map prim, never `asset[]`** (S13): an asset array gets no reload tracking, so
several files means several map prims and a `UsdGenCombineMap` to join them.

**Agreement with `02-schema.md` §2.12, which is normative and whose names C1 freezes at the end of
M1 (ADR §3, §9.2 R7).** Every name, type, default and allowed-token set above is that table's
verbatim. The four properties this document originally needed and 02 lacked have landed there —
`float usdGen:map:default = 0.0` (the failed-sample value the error policy of §5.5 and §7.7
returns), `token usdGen:noise:space = "rest"` with allowed tokens `rest | deformed` (§5.2 has three
sampling spaces and a noise map has to say which it evaluates in), `int usdGen:paint:resolution =
256` (the per-face `Res` a `.ptx` bake needs, §8.3, and what XGen writes as `#3dpaint, N`) and
`token usdGen:paint:storage = "primvar"` (§8.1: three storage states need a property that says which
one is live) — and so has the fifth item, which was not a new property but a missing token list:
`usdGen:map:filter` is declared by **two** types with **two** allowed sets, and a codeless schema
needs `allowedTokens` per declaring type, so 02 §2.12 now carries `nearest | bilinear` on
`UsdGenImageMap` and the eight `PtexFilter::FilterType` names on `UsdGenPtexMap` (§5.3). Nothing in
this section is an addition to 02 any more; where the two ever disagree, 02 §2.12 wins (R7). Note in
particular that `usdGen:map:colorSpace` defaults to
**`"raw"`**: `"auto"` linearises 8-bit files and `"raw"` does not, so the two documents disagreeing
on this default would give the same map a different colour. The tool authors `"auto"` on a new
*colour* map (§5.3); the schema default stays `"raw"` so a scalar mask is never silently
transfer-transformed.

### 5.2 Sampling spaces

Every sample is taken at a *curve root* whose location on the emitting surface the engine already
has from the scatter capture. Under `usdGen:map:domain = "cv"` the position, normal and `t` move to
the CV while the surface coordinates (`st`, `faceId`, `faceUV`) stay the root's, because a CV has no
surface projection (§5.1). The map type decides which of that state it reads:

| Space | Meaning | Who uses it |
|---|---|---|
| `st` | the root's UV in the surface's uv set named by `usdGen:map:uvSet` (default `st`), taken from the mesh's `primvars:<uvSet>`; also the tile's uniform `st` primvar | `UsdGenImageMap`, `UsdGenPaintMap` with a UV-space baked file |
| `faceId + (u, v)` | face index **into the parent mesh's faces** plus the face-local parametric coordinate | `UsdGenPtexMap`, `UsdGenPaintMap` in per-face storage |
| rest | `$Pref`, `$Nref`, `$dPduref`, `$dPdvref` — the surface at `UsdTimeCode::Default()`, from the `UsdGenRestAPI` adapter (S12) | `UsdGenNoiseMap` with `space = "rest"` (the default), expressions |
| `deformed` | `$P`, `$N` on the deformed surface at the current frame. usdGen reads post-flattening, so this space carries the world transform in `xform/matrix` and ADR §9.2 R9 rules that **deformed space is world space**: there is no separate `world` token and no world-space variable (S4) | `UsdGenNoiseMap` with `space = "deformed"`, expressions |

Two rules that come straight from ADR §2.3 and must not drift:

* **Face ids are always indices into the parent mesh's faces**, never into a `GeomSubset`'s
  membership list. A subset restricts *which* faces are scattered; it never renumbers anything. The
  same rule governs `primvars:skinprim` on frozen curves (S42).
* **A subset edit is a recapture**, because it changes the scatter domain and therefore every stable
  id downstream.

Rest space is the default for noise: rest-space noise does not swim when the surface deforms — the
same reason operators declare `restSpace | deformedSpace` (S25).

### 5.3 Interpolation, channels and colour space

* **Image maps.** Bilinear by default, `nearest` for masks an artist wants crisp. Wrap modes are
  implemented in usdGen's own sampler; Hio only decodes. Colour space `auto` means "honour the
  file": usdGen opens with `HioImage::SourceColorSpace::Auto` and asks
  `HioImage::IsColorSpaceSRGB()` (`pxr/imaging/hio/image.h:151`; the stb implementation decides from
  `_sourceColorSpace` and the file gamma, `pxr/imaging/hio/stbImage.cpp:302-320`). **Hio never
  converts texels** — usdGen's own sampler applies the sRGB→linear transfer function
  (`c <= 0.04045 ? c/12.92 : pow((c+0.055)/1.055, 2.4)`, per channel, alpha untouched) when that
  query returns true. `raw` and `sRGB` force the answer. The schema default is `raw` (§5.1); the
  tool authors `auto` on a new colour map and leaves `raw` on a new scalar map (density, length,
  masks).
* **Ptex maps.** The filter token maps 1:1 onto `PtexFilter::FilterType` (`Ptexture.h:944-953`)
  (`nearest→f_point`, `bilinear→f_bilinear`, `box→f_box`, `gaussian→f_gaussian`,
  `bicubic→f_bicubic`, `bspline→f_bspline`, `catmullrom→f_catmullrom`, `mitchell→f_mitchell`), and
  `usdGen:map:blur` becomes the `blur` argument of `PtexFilter::eval` (§6.2).
* **Primvar-backed paint maps.** The value at a root is interpolated from the surface primvar
  according to *its* interpolation: `constant` → the single value; `uniform` → the face's value
  (piecewise constant, no interpolation); `vertex`/`varying` → bilinear in the face's (u, v) over
  the face's corner values (barycentric for triangles); `faceVarying` → the same, indexed by
  face-corner offsets. usdGen has the mesh topology at capture already (it scatters on it), so this
  costs no extra pull.
* **Channels.** `luminance` uses Rec. 709 weights `(0.2126, 0.7152, 0.0722)`. `rgb` on a map feeding
  a scalar parameter is an authoring error and is reported at compile time, not silently averaged.

### 5.4 Loading: the map library

Map evaluation lives in the engine (`usdGen`), which links **no `usd`, no `usdImaging`, no `hd`**
(ADR §4.2). So maps reach it as pure values inside `UsdGenGraphDesc`: `usdGenImaging` reads the map
prims from data sources, resolves the asset path (the adapter delivers a resolved
`SdfAssetPath` — resolution is free and stage-free, S13) and fills a `UsdGenMapDesc`.

```cpp
// usdGen/mapLibrary.h  — engine side, no pxr imaging dependencies
struct UsdGenMapSample {                 // where one sample is taken
    int       surfaceId;                 // index into the graph's surface table
    int       faceId;                    // parent-mesh face index
    GfVec2f   faceUV;                    // face-local parametric coordinate
    GfVec2f   st;                        // uv-set coordinate at the same point
    GfVec3f   P, N, Pref, Nref;          // deformed and rest position/normal
    uint64_t  curveId;                   // stable id -- 64-bit (ADR §9.2 R12;
                                         //   03-execution-engine.md 1.2's VtUInt64Array curveId)
    float     t;                         // root(0) -> tip(1); 0 when domain == root
    float     frame;
};

class UsdGenMapEvalContext;              // per-worker scratch: Ptex filters, SeExpr VarBlocks

class UsdGenMapSampler {
public:
    virtual ~UsdGenMapSampler();
    virtual int  ChannelCount() const = 0;
    virtual void Sample(UsdGenMapEvalContext *ctx,
                        const UsdGenMapSample &s, float *out) const = 0;
};

class UsdGenMapLibrary {                 // one per session (UsdGenImagingRegistry, S15)
public:
    const UsdGenMapSampler *Get(const UsdGenMapDesc &desc, std::string *error);
    std::unique_ptr<UsdGenMapEvalContext> MakeContext() const;   // one per TBB worker
    void      Reload();                  // 5.5
    uint64_t  TextureGeneration() const;
    uint64_t  Digest(const UsdGenMapDesc &desc) const;           // folds into the capture epoch
};
```

`Sample` is `const` and must be callable from every worker at once; all mutable per-evaluation state
(the `PtexFilter`, the SeExpr `VarBlock`, scratch buffers) lives in `UsdGenMapEvalContext`, of which
the capture code makes exactly one per worker in the private `tbb::task_arena` (ADR §4.1
invariant **I8** — ADR §9.1 R1 renames the engine principles P1–P8 to I1–I8 and reserves P0/P1/P2
for the motion profiles).
`PtexSeparableFilter` carries per-eval scratch (`PtexSeparableFilter.h:80-81`) and
`SeExpr2::Expression::evalFP` writes into shared interpreter storage unless handed a thread-safe
`VarBlock` (`Expression.cpp:304-309`, `Interpreter.cpp:31-43`) — both verified in
`research/A8-seexpr-ptex-libs.md` §1.5, §2.4.

Two caches sit under the library, both keyed by `(resolvedPath, textureGeneration)`:
`UsdGenImageCache` (decoded `HioImage` planes, budget `USDGEN_IMAGE_CACHE_MB`, default 512) and
`UsdGenPtexTextureCache` (§6.2). Machine tuning is env/config and is never authored into an asset
(ADR §2.3). `USDGEN_IMAGE_CACHE_MB` (default 512, ASSUMPTION, settled by gate L-4) is registered in
the single env-var registry ADR §9.4 R35 assigns to `10-build-dependencies-testing.md` §3.5,
alongside `USDGEN_PTEX_CACHE_MB` and `USDGEN_PTEX_MAX_FILES` (§6.4). No document but that one may
introduce an env var.

Image decoding uses `HioImage::OpenForReading(path, subimage, mip, colorSpace)` +
`Read(StorageSpec)` (`pxr/imaging/hio/image.h:100-127`), which in this install covers
`png jpg jpeg tga bmp hdr exr` read/write and `avif` read (MEASURED). A map naming a `.tif`/`.tx`
fails at compile time with a diagnostic that names the formats that do work — silently sampling the
default value would look like a broken groom.

### 5.5 Reload

Overwriting a map file on disk invalidates nothing: `stage->Reload()` produces no notice (MEASURED,
S13). usdGen therefore ships an explicit action, in the tool's menu and on the C ABI, whose single
source is `08-tools.md` §1.4 (contract C4, ADR §9.4 R31): `int UsdGenImaging_ReloadMaps(void)`.

```
UsdGenImaging_ReloadMaps()
    -> registry bumps the session's _textureGeneration
    -> UsdGenMapLibrary::Reload(): purge the image cache, PtexCache::purgeAll(), drop compiled
       expressions whose map handles changed
    -> every capture epoch that folds a map digest changes value
    -> the affected nodes re-capture; nothing else moves
    -> the affected tiles dirty primvars/displayColor/primvarValue, or primvars/<bakePrimvar>/
       primvarValue under bakeTarget == "primvar" (and any operator-driven
       primvars the recapture changed); never displayColor together with points (S30)
```

`ArNotice::ResolverChanged` is honoured when a host sends it — MEASURED to produce exactly the right
per-property dirties — but usdGen never depends on it, and it never sends one on the user's behalf.

The `usdGen:map:*` value edits (scale, offset, channel, filter) are ordinary parameter edits: they
bump the capture epoch of the nodes that read the map and nothing else, because the map digest folds
`(type, resolved path, every authored parameter, textureGeneration)`.

### 5.6 Where maps may be read — and where they may not

| Consumer | Where it reads | When |
|---|---|---|
| the mask block on every operator (`UsdGenMaskAPI`) | `rel usdGen:mask:source` (and `rel usdGen:mask:region`) | capture (`04-operators.md` §5.2; the property is `usdGen:mask:source` — **there is no `usdGen:mask:map`**, ADR §9.2 R16, and `02-schema.md` §2.13 is canonical for the block) |
| scatter density, length variation, width, clump amount, region/parting | operator-specific `rel` | capture |
| `UsdGenLookAPI.usdGen:look:colorMap` | §1.1 | capture |
| `UsdGenExprMap` via `map("<primName>")` / `ptex("<primName>")` | `rel usdGen:expr:maps` | capture, at expression prep + eval |
| a Storm material's `rootColorMap` | a `UsdUVTexture` on the uniform `st` | GPU, per fragment — the one non-capture path. The texture is synthesized by the tool from a `UsdGenImageMap` (§1.1, `colorMapMode = "shader"`) and is not itself sampled by the engine |

**No operator may call a map, a Ptex lookup or an expression inside `Evaluate()`** — the per-frame,
per-chunk half of the operator interface (S25, S37; `03-execution-engine.md`). The arithmetic says
why: a 1 M-root expression mask costs ≈ 0.1 s single-threaded and ≈ 20 ms on 8 threads
(`DERIVED from EV-067` and `EV-069` — 106–117 ns/eval and 50 M evals/s aggregate,
`appendix-A-evidence-ledger.md` §2.8, `research/A8-seexpr-ptex-libs.md` §1.6).
That is once-per-edit money, and ten times the ≤ 2.0 ms per-frame deform delta gate S-2 allows
(`09-performance-and-benchmarks.md` §5.3). This rule is checked mechanically, not by review:
`Evaluate()` receives a chunk view that does not expose the map library at all.

---

## 6. Ptex

`UsdGenPtexMap` is a **v1** feature delivered in **M4**. ADR §9.5 R38 amends ADR §6 on exactly this
point: *"v1 = the M0–M7 deliverable set"*, and it names `UsdGenPtexMap` (M4) and `UsdGenInstance`
cards/archives/spheres (M6) in that set; v2 is M8 breadth, where `UsdGenExprOp` (§7.8) sits. Ptex
therefore ships with the map milestone, not with the M8 breadth pass. Read the version tier and the
milestone as two statements: the **tier** says which release the feature belongs to, the
**milestone** says when it is built. `02-schema.md` §1 and §2.12 both read **`v1 (M4)`** for
`UsdGenPtexMap`, which is ADR §9.5 R38's assignment; 02 is normative for the type's names and types
(ADR §9.2 R7) and this document adds nothing to that row.

### 6.1 Why usdGen ships its own

This OpenUSD install has no Ptex: `PXR_ENABLE_PTEX_SUPPORT` defaults OFF
(`cmake/defaults/Options.cmake:36`) and was OFF for this build, so
`HdStIsSupportedPtexTexture("a.ptx")` returns 0 (MEASURED by a probe linked against the installed
`libusd_hdSt.so`). A `.ptx` reaching a Storm material is **silently** a 1×1 black texture: the whole *body* of
`HdStPtexTextureObject::_Load()` is inside `#ifdef PXR_PTEX_SUPPORT_ENABLED`
(`pxr/imaging/hdSt/ptexTextureObject.cpp:121-229`); the ten lines before it only clear the textures
and set `_format = HgiFormatInvalid` (`:111-120`), which is why `_Commit()` then uploads the 1×1
"PtexTextureFallback" texel (`:233-268`). Even with the flag on, Storm's Ptex accessor needs the mesh-only
`GetPatchCoord()` (`pxr/imaging/hdSt/codeGen.cpp:6617-6660`, defined in `mesh.glslfx` and not in
`basisCurves.glslfx`), so Ptex on curves would still not work.

usdGen therefore vendors **Ptex v2.4.3** (zlib-based; v2.5.0 switched to libdeflate, whose headers
are absent here), built static with hidden visibility as the CMake target `usdGen_ptex` and linked
into `libusdGen.so` only (ADR §6). Its `.so` is never installed next to USD's libraries: a
`libPtex.so` in `lib/` would be picked up by the `$ORIGIN` rpath of a site's Ptex-enabled
`libusd_hdSt.so` (`research/A8-seexpr-ptex-libs.md` §6.2). Registering a `HioImage` subclass for
`.ptx` is rejected and recorded so it is not re-proposed: it would make Storm's *UV* texture path
treat a Ptex file as a 2-D image (`research/A8-seexpr-ptex-libs.md` §3.3).

### 6.2 The runtime shape

```cpp
// one per session, owned by UsdGenMapLibrary
PtexCache *cache = PtexCache::create(/*maxFiles*/ maxFiles,
                                     /*maxMem  */ size_t(cacheMB) << 20,
                                     /*premultiply*/ false,
                                     /*inputHandler*/ nullptr,
                                     /*errorHandler*/ &_ptexErrors);   // Ptexture.h:711
cache->setSearchPath(searchPath);                                      // Ptexture.h:725

// per map, once: PtexTexture *tex = cache->get(resolvedPath, err);    // Ptexture.h:757
// per worker, once per texture:
PtexFilter::Options opts(filterType, /*lerp*/ true, /*sharpness*/ 0, /*noedgeblend*/ false);
                                                                       // Ptexture.h:956-967
PtexFilter *filter = PtexFilter::getFilter(tex, opts);                 // Ptexture.h:971
// per sample:
filter->eval(out, firstChannel, channelCount, faceId, u, v,
             uw1, vw1, uw2, vw2, /*width*/ 1.0f, /*blur*/ blur);       // Ptexture.h:997
```

* **One `PtexFilter` per worker**, never shared: `PtexSeparableFilter` holds `float *_result` and
  `float _weight` scratch (`PtexSeparableFilter.h:80-81`). `PtexFilter` is a **global-namespace**
  class (`Ptexture.h:937`) whose `FilterType` (`:944-953`) and `Options` (`:956-967`) are nested
  inside it; the `Ptex::` namespace holds `String`, `Res`, `FaceInfo`, `MeshType` and `DataType`
  only, so `Ptex::PtexFilter::Options` names nothing and does not compile. The `PtexCache` itself is
  fully thread-safe and lock-free for cached data (`Ptexture.h:678-681`, `PtexCache.cpp:61-66`), and
  8-thread lookups were verified correct in `prototypes/thirdparty-bench/ptex_test.cpp`.
* **Footprint.** A hair root has no screen-space derivatives, so the filter region is one texel:
  `uw1 = 1/res.u()`, `vw2 = 1/res.v()` from the face's `Res`, with `vw1 = uw2 = 0`; the artist's
  `usdGen:map:blur` widens it. The region is "a parallelogram centred at (u,v) with sides
  [uw1,vw1] and [uw2,vw2]" (`Ptexture.h:977-981`).
* **Release discipline.** `PtexTexture` and `PtexFilter` are reference counted; usdGen wraps both in
  `PtexPtr` (`Ptexture.h:1032`) held by the map sampler and the per-worker context, so a
  `Reload()` that purges the cache while a capture is in flight keeps the in-use reference valid
  (`Ptexture.h:759-762`).

### 6.3 Face ids

The mapping from a USD mesh face to a Ptex face id comes from OpenSubdiv, which is installed and
does not need the Ptex library:

```cpp
PxOsdTopologyRefinerSharedPtr refiner =
    PxOsdRefinerFactory::Create(meshTopology, TfToken("usdGenPtex"));   // pxOsd/refinerFactory.h:34-41
OpenSubdiv::Far::PtexIndices ptexIndices(*refiner);                     // far/ptexIndices.h:46-88
int faceId = ptexIndices.GetFaceId(coarseFaceIndex);                    // -1 if none
```

Rules, all verified in `research/A8-seexpr-ptex-libs.md` §2.5–2.6:

* **Quads map 1:1.** An n-gon (n ≠ 4) in a quad-mesh file occupies **n consecutive face ids**,
  flagged `flag_subface`; the root's face-local (u, v) must then be remapped into the correct
  quadrant. usdGen computes the quadrant from the root's parametric coordinate on the coarse face.
* **The refiner is built from the parent mesh, never from a `GeomSubset`** (ADR §2.3). A subset
  target restricts scatter; ids are unchanged.
* **Subdivision does not renumber.** `Far::PtexIndices` is built over the refiner's base level, so
  face ids are per *coarse* face whatever `subdivisionScheme` says — which matches how Ptex files are
  authored and how XGen addresses them. Refined patch parameters carry the same ptex index in
  `Far::PatchParam` (`pxr/imaging/hdSt/codeGen.cpp:5524`), for consistency with Storm's convention
  ("ptexId matches the primitiveID for quadrangulated or triangulated meshes",
  `codeGen.cpp:5620-5621`) should a site ever enable Storm Ptex.
* **Triangles.** usdGen treats `mt_quad` files against `PtexIndices` as the canonical mapping and
  supports `mt_triangle` files only for all-triangle meshes, where the mapping is 1:1. Storm's fan
  triangulation of a base face produces a different sub-face count from Ptex's "subdivide once" rule
  for tri meshes, and the exact tri (u,v)↔vertex convention is UNVERIFIED
  (`research/A8-seexpr-ptex-libs.md`, open questions). A `mt_triangle` file on a mixed mesh is a
  compile error with that sentence as the message.
* The face-id map is part of the surface capture and is cached with it; a topology edit recaptures.

### 6.4 Cost and cache sizing

MEASURED, **EV-070** (`appendix-A-evidence-ledger.md` §2.8; `research/A8-seexpr-ptex-libs.md` §2.8,
`prototypes/thirdparty-bench/ptex_test.cpp`): 23 ns per bilinear lookup with a one-texel footprint,
26 ns for a 4-texel box filter, and 35 ns per lookup per thread at 8 threads = 228 M lookups/s
aggregate. A 1 M-root density lookup is therefore ≈ 23 ms single-threaded and ≈ 4.4 ms on 8 threads
(`DERIVED from EV-070`) — cheaper than the expression that usually consumes it.

Cache defaults, both ASSUMPTION, both settled by gate L-4: `USDGEN_PTEX_MAX_FILES = 32` and
`USDGEN_PTEX_CACHE_MB = 256`. Both are in the single env-var registry ADR §9.4 R35 assigns to
`10-build-dependencies-testing.md` §3.5, alongside `USDGEN_IMAGE_CACHE_MB` (§5.4). The thrash signal
is exact rather than statistical:
`PtexCache::Stats` exposes `fileReopens` and `blockReads` (`Ptexture.h:779-790`), and gate L-4 asserts
`fileReopens == 0` over a full capture of the benchmark groom. If it is non-zero the defaults rise;
if memory is tight the same counter says what it costs.

### 6.5 Writing

Painting produces `.ptx` through `PtexWriter::open(path, mt_quad, dt_float, nchannels, alphachan,
nfaces, err, genmipmaps=true)` and `writeFace(faceId, FaceInfo, data, stride)`
(`Ptexture.h:827`, `:907`, `:919`). The adjacency a `FaceInfo` needs is exactly what
`Far::PtexIndices::GetAdjacency(refiner, face, quadrant, adjFaces, adjEdges)` returns
(`far/ptexIndices.h:63-88`) — `setadjfaces`/`setadjedges` take it unmodified. `PtexWriter::edit(path,
incremental=true, ...)` appends without rewriting the file (`Ptexture.h:850`). The pattern was
exercised end to end in `prototypes/thirdparty-bench/ptex_test.cpp` (two-face quad file with a
shared edge, cross-face blending confirmed at `u = 0.999`). See §8.3.

---

## 7. SeExpr2

### 7.1 The library

Upstream `wdas/SeExpr`, `main` at commit `8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc` (2026-01-27 —
the newest tag, v3.0.1, predates the C++17 fix), interpreter only, built static with hidden
visibility as the CMake target `usdGen_seexpr` and linked into `libusdGen.so` (ADR §6, S38). The
configure flags, the bison/flex requirement (the repository ships an empty `src/SeExpr2/generated/`)
and the aarch64 `ENABLE_SSE4=OFF` rule are in `10-build-dependencies-testing.md`, all verified to
build here in about a minute (`research/A8-seexpr-ptex-libs.md` §1.2). The namespace and library are
`SeExpr2` regardless of the "v3" generation naming; the licence is Apache-2.0 with §6 (Trademarks)
replaced — permissive, one NOTICE entry.

The LLVM backend stays off: it needs LLVM ≥ 3.8 headers (absent here) and raises prep cost for a
steady-state win the interpreter's 13–117 ns does not need. KSeExpr (the Krita fork) is rejected:
GPL-3.0-or-later, a hard Qt dependency, and a renamed non-ABI-compatible namespace.

### 7.2 The variable set

Registered through one `SeExpr2::VarBlockCreator` per compiled program
(`VarBlock.h:84-126`), so evaluation costs no virtual `ExprVarRef` call per hair. Names are given
here as they appear in an expression (with the `$`); `registerVariable` takes them without it.

| Variable | Type | Value |
|---|---|---|
| `$u`, `$v` | float | the root's surface parameters in the map's uv set |
| `$id` | float | the stable curve id. SeExpr has only doubles, and `curveId` is 64-bit (ADR §9.2 R12), so the slot carries `double(curveId)` — exact to 2^53 and the value `rand($id)`/`hash($id)` are seeded from. Never compare `$id` for equality against an id printed elsewhere above that bound |
| `$faceId` | float | parent-mesh face index (§5.2); `$faceid` is accepted as an XGen-compatible alias |
| `$patchId` | float | the surface index within the description's surface table |
| `$descId` | float | a stable hash of the `UsdGenDescription` path |
| `$P`, `$N`, `$dPdu`, `$dPdv` | vec3 | deformed surface point, normal and derivatives at the root |
| `$Pref`, `$Nref`, `$dPduref`, `$dPdvref` | vec3 | the same on the **rest** surface (S12: `UsdTimeCode::Default()`, honouring an authored `primvars:rest`) |
| `$t` | float | root(0) → tip(1); 0 when `usdGen:map:domain == "root"` |
| `$frame` | float | the session's current frame |
| `$cLength`, `$cWidth`, `$cDepth` | float | the curve's computed length, width and depth *so far* in the chain |
| `$Cs`, `$As` | vec3, float | the **surface's own** `primvars:displayColor` / `displayOpacity` sampled at the root — never the `UsdGenLookAPI` result. Inside a `UsdGenExprMap` reached from `usdGen:look:colorMap` they carry the surface value, so the §1.2 look bake never depends on its own output; `(1,1,1)` and `1.0` when the surface authors neither |

XGen's world-space aliases (`$Pw`, `$Prefw`) are deliberately absent: usdGen reads post-flattening,
so deformed space already carries the world transform and ADR §9.2 R9 recognises no separate world
space (S4). A porting note covers it; a second name for the same vector would not.

> **What ships today.** The table above is the M4 capture-time *map* language.
> The **runtime parameter** lane (`usdGen:width.connect` and friends) declares
> its own variables in `libs/usdGen/usdGen/expressions/context.cpp`, and
> `expr::Frontend::VariableDocs()` is the single source of truth for them —
> the usdview editor's variable browser and this table are both generated from
> it, so they cannot disagree. Differences from the table above:
>
> * `$N` and `$Nref` are the same vector, and so are `$dPdu`/`$dPduref` and
>   `$dPdv`/`$dPdvref`: usdGen keeps ONE root frame per strand, the **rest**
>   frame the generator bound (`UsdGenCurveBuffer::rootN/rootT/rootB`). There
>   is no deformed root frame to report, so reporting one would be a lie.
>   `$dPdu` is the frame's tangent and `$dPdv` its bitangent.
> * `$faceId` is the strand's `rootPrim`, the parent-mesh face index.
> * `$patchId`, `$Cs` and `$As` are **gone**. They were declared through M1 but
>   no field builder ever wrote them, so an expression naming one evaluated to
>   a poison value and the whole cook was refused with an unhelpful message.
>   usdGen has no patch table, and the engine's curve buffer carries no surface
>   colour or opacity at evaluation time. They are removed rather than left
>   silently unavailable; when a surface-colour channel exists they can come
>   back with an implementation behind them.
> * `$idLo`/`$idHi` are added, because `$id` is a double and exact only to
>   2^53 while `curveId` is 64-bit.
> * `$cDepth` is absent: usdGen has no depth channel.
>
> Every entry carries a one-line doc and its domain list (`groom`,
> `primitive`, `point`), and a variable used outside its domain is a compile
> error naming the variable and the domain, not a zero.

This is otherwise the XGen dialect artists expect (`research/A7-prior-art-grooming.md` §1.3,
`research/A8-seexpr-ptex-libs.md` §1.8). Variables an expression does not reference cost nothing:
they are registered on the creator but never filled.

Every one of these values is something the groom evaluator already holds per root at capture, which
is the whole reason expressions are capture-time.

### 7.3 The function set

> **What ships today.** The rest of §7 describes the M4 capture-time *map*
> expression language, which runs SeExpr's own interpreter. The **runtime
> parameter** lane that ships now (`usdGen:width.connect` and friends,
> `14-hierarchy-cuda-implementation.md`) is a different evaluator: the vendored
> SeExpr frontend parses and type-checks, then the program is lowered to a
> small IR that both the CUDA kernel and the CPU reference lane interpret from
> one shared source (`libs/usdGen/usdGen/expressions/irExec.h`), over one
> shared builtin library (`expressions/exprMath.h`).
> `expr::Frontend::SupportedFunctions()` is the single source of truth for what
> it accepts, with a name, arity range, signature, one-line doc, result width
> and category for each entry. The set is the SeExpr2 builtin library, which is
> what XGen expressions are written against:
>
> | Category | Functions |
> |---|---|
> | `math` | `abs acos acosd asin asind atan atan2 atan2d atand bias boxstep cbrt ceil clamp compress contrast cos cosd cosh cycle deg exp expand fit floor fmod gamma gaussstep hypot invert linearstep log log10 max min mix pow rad remap round sin sind sinh smoothstep sqrt tan tand tanh trunc` |
> | `noise` | `ccellnoise cellnoise cfbm cnoise cturbulence cvoronoi fbm hash noise pnoise pvoronoi rand snoise turbulence vfbm vnoise voronoi vturbulence` |
> | `vector` | `angle cross dist dot length norm ortho rotate up` |
> | `color` | `hsi hsltorgb midhsi rgbtohsl saturate` |
> | `curve` | `ccurve curve spline` |
> | `control` | `choose pick wchoose` |
> | `sampling` | `geoSampler ptex` (CPU lane only) |
>
> **Sampling (2026-09-16).** An expression reads data outside its own strand
> only through a relationship on its `UsdGenExpression` prim named
> `input:<name>`, and names it by that string:
> `geoSampler("<name>", "<element expression>" [, iterate [, reduce [, query]]])`
> iterates the `point`/`prim`/`geometry` elements of the meshes, curves and
> points the relationship targets (a group prim contributes the gprims below
> it), evaluates the one-line element expression with the element's own
> variables plus `$Q`/`$Qdist`, and reduces with `nearest`, `nearest2`, `min`,
> `max`, `sum` or `mean` about the query (default `$P`); `ptex("<name>")`
> reads the `UsdGenPtexMap` it targets at the strand root, applying
> `usdGen:map:channel/scale/offset/clamp/default`. The frontend lowers each
> call site to `IROp::Sample` with its constant strings in
> `IRProgram::samplers`; `UsdGenCpuParameters` resolves the slots against the
> descriptor's `UsdGenExpressionInputDesc` / `UsdGenGeometryDesc` pools
> (`libs/usdGen/usdGen/expressions/samplers.{h,cpp}`); CUDA admission refuses
> any program with a sampler. The targets are recorded as scene-index
> dependencies of the description and as `geometryRefs` in the routing table,
> so an edit of a sampled prim, or a retarget of the relationship, recooks the
> consumer. Examples: `examples/guide-interpolate-plane.usda`,
> `examples/clump-ptex-plane.usda`.
>
> **Value preview (2026-09-16).** `usdGen:preview:source` on a
> `UsdGenDescription` (with `colorMap`, `range`, `evaluation`, `shading`)
> replaces the published `displayColor` with a colour-mapped value and binds
> the synthetic `__usdGenRender/material_preview[_flat]` (the
> `UsdGenValuePreview` glslfx, which shows `displayColor` unmodified) over any
> authored material. The source is a `UsdGenExpression` (evaluated over the
> terminal strands), a `UsdGenPtexMap` (a private `ptex("map")` expression; the
> builders add the map to `maps`), or an operator attribute (the node's
> `UsdGenCpuParameters` values over its input strands, matched to the terminal
> by curve id, else its authored literal). `libs/usdGen/usdGen/valuePreview.
> {h,cpp}`; the session cooker folds a colour digest into the generation so a
> preview edit rebuilds tiles whose geometry did not move. CPU lane only;
> never fails a cook (problems are warnings, strands show the no-value
> colour). The SeExpr editor authors it in the session layer
> (`plugin/usdGenTools/python/usdGenTools/exprPreview.py`).
>
> **Omitted, each with its own diagnostic rather than the generic list:**
> `printf`/`sprintf` (no output from a cooked groom); `map`/`texture`
> (image maps other than Ptex are not yet available in a runtime expression);
> `file`/`system`/`exec`; `def` user functions; `swatch` (an alias for
> `choose`); and the 4D noise spellings `noise(x,y)`, `snoise4`, `vnoise4`,
> `cnoise4`, `fbm4`, `vfbm4`, `cfbm4` — usdGen vendors the 3D gradient table
> only, and a 2D or 4D lattice would need its own table emitted for both lanes.
>
> **Deliberate differences from stock SeExpr2**, each asserted in
> `tests/testUsdGenSeExprOracle.cpp` so they stay deliberate:
>
> * `dist` is bound as `dist(vector, vector)`, which is what its own docstring,
>   XGen's reference and every other vector builtin say. `ExprBuiltins.cpp`
>   binds it as six scalars.
> * `clamp` with `hi < lo` refuses the whole evaluation instead of answering
>   with a bound. An inverted range is an authoring mistake and a silent answer
>   hides it.
> * `rand` is XGen's, not SeExpr2's, which has no `rand` at all. It is
>   `hash($seed, $id, <call site index> [, seeds...])`, so it is stable per
>   strand, identical on both lanes, and two `rand()` calls in one expression
>   are independent. `rand(min, max, seed)` scales into the range.
> * `cbrt` and `trunc` are declared by usdGen because `ExprBuiltins.cpp` omits
>   them on Windows; the language must not depend on the platform it was built
>   for. All four names are declared through the per-expression `resolveFunc`
>   hook, never through the process-wide `ExprFunc::define` table (§7.3 below).
>
> **Parity.** `tests/testUsdGenSeExprOracle.cpp` links the vendored SeExpr2
> archive and asserts exact double equality between `exprMath.h` and Disney's
> own `Noise.cpp`/`ExprBuiltins.cpp`/`Curve.cpp`, for the lattices themselves
> and for compiled expressions run through the real interpreter.
> `tests/testUsdGenCudaExpressionParity.cpp` then asserts the two execution
> lanes agree BIT FOR BIT, including the whole noise/hash/cellnoise/voronoi
> and curve/spline/choose/pick family: SeExpr2's lattices are pure `+ - * /`
> over a gradient table that `expressions/exprNoiseTables.h` emits once for
> both lanes, and every double-to-integer conversion goes through
> `ExprInt`/`ExprUInt32Wrap` rather than a bare cast (x86 wraps where PTX
> saturates, which would otherwise break `cellnoise` and `voronoi` on the
> device only). The libm-quality transcendentals — `sin cos tan asin acos atan
> sinh cosh tanh exp log log10 pow cbrt`, and the builtins that reach one
> (`angle rotate up gamma bias contrast gaussstep`) — are each correctly
> rounded on both lanes but not necessarily to the same bits, so they are
> compared to a tolerance instead.
>
> **The language.** An expression may be several statements separated by `;`,
> with `#` comments anywhere; the last expression is the value. Local variables
> (`$a = ...;`) may be reassigned and may hold vectors; `if (cond) { ... } else
> { ... }` blocks assign locals and lower to a `Select` over what each branch
> assigned. Both arms are evaluated and the result selected, which is safe
> because every function is pure, but it is not a way to skip work. A local may
> not shadow a registry variable; that is a compile error naming it. No
> strings, no `def`, no `printf`.
>
> **Limits.** A program may use at most 256 values (one per emitted
> instruction) and 4096 instructions; the frontend common-subexpression
> eliminates, so the ceiling is generous in practice — roughly a `curve()` of
> 30 knots or a `ccurve()` of 8. The limit is deliberately modest because the
> CUDA kernel spends that many doubles of thread-local memory on every launch
> whether the program needs them or not; exceeding it is a diagnostic, not a
> slower groom for everybody. `curve`/`ccurve` knots must be constants: they
> are sorted, given SeExpr's sentinels, given centred-difference derivatives
> and monotone-clamped at COMPILE time and lowered into the instruction stream
> as immediates, so dragging a point in the UI rewrites numbers only.

The full SeExpr2 builtin set is available: math and trigonometry, `clamp round max min invert
compress expand fit gamma bias contrast boxstep linearstep smoothstep gaussstep mix`, colour
`hsi midhsi hsltorgb rgbtohsl`, noise `noise snoise vnoise cnoise pnoise cellnoise ccellnoise
turbulence vturbulence cturbulence fbm vfbm cfbm fbm4 vfbm4 cfbm4 voronoi cvoronoi pvoronoi`,
vector `dist length norm dot cross angle ortho rotate up`, selection `cycle pick choose wchoose
hash`, ramps `curve` / `ccurve` / `spline` (interp codes 0–4), and `printf`/`sprintf`
(`ExprBuiltins.cpp:1719-1849`, enumerated in `research/A8-seexpr-ptex-libs.md` §1.7).

usdGen adds three, all through an overridden `Expression::resolveFunc` — **never** through the
global `ExprFunc::define` table, which is process-wide, shared with any other SeExpr consumer in the
process, and documented as not thread-safe at the call site (`ExprFunc.cpp:139-147`, "NOT THREAD
SAFE, it assumes you have a mutex from callee"; the table's own mutex is at `:110-134`;
`research/A8-seexpr-ptex-libs.md` §1.3, §1.5). The `resolveFunc` pattern is verified in
`prototypes/thirdparty-bench/seexpr_bench.cpp`, and the shipping runtime lane follows the same rule:
`expressions/frontend.cpp`'s `CheckedExpression::resolveFunc` declares `rand`, `dist`, `cbrt` and
`trunc` as type-check stubs, and `ExprFuncNode::prep` consults it *before* the global table
(`ExprNode.cpp`), so a name the stock table binds differently can be corrected without touching it.
The stubs are never evaluated: the program is lowered to usdGen's IR and SeExpr2's evaluator never
runs on the runtime lane.

| Function | Signature | Implementation |
|---|---|---|
| `map` | `map("<mapPrimName>" [, s, t] [, channel])` | an `ExprFuncSimple` whose `prep` checks arg 0 is `ExprType().String().Constant()` and whose `evalConstant` resolves the name against `usdGen:expr:maps` **once**, caching the `UsdGenMapSampler*` in the node's `Data` (the `CurveFuncX` pattern, `ExprBuiltins.cpp:1295-1340`) |
| `ptex` | `ptex("<mapPrimName>" [, faceId, u, v] [, channel])` | the same, restricted to `UsdGenPtexMap` targets |
| `rand` | `rand([min], [max], [seed])` | **not** a SeExpr2 builtin — verified at runtime: `Function rand has no definition`. usdGen implements it on `hash` so it is deterministic and reproducible across threads and runs |

Argument 0 is a **prim name**, not a path and not a file; it is resolved at prep against the targets
of `usdGen:expr:maps`, and an unknown name is a compile error naming the targets that do exist. This
is what keeps the expression language from being a file-system API (§7.6).

`${DESC}`-style macros from XGen are pre-substituted over the expression string before parsing, and
the resulting asset references go through Ar like any other (S13). SeExpr preserves comments
(`Expression::_comments`, `Expression.h:291`), so XGen's `#3dpaint, 200` (paint resolution) and
`#0.10,1.00` (slider range) annotations survive a round trip and the tool's expression editor reads
them (`08-tools.md`).

### 7.4 Noise semantics

**SeExpr's `noise()` returns 0..1; XGen's returns −1..1.** usdGen keeps SeExpr semantics and ships
`snoise` as the signed form, documented in the user docs and in the expression editor's help pane
(ADR §6). SeExpr's `Noise.h` templates (`Noise`, `PNoise`, `FBM`, `CellNoise`, `Noise.h:23-36`) are
the **single** noise implementation in the product — the `UsdGenNoise` styler, the `UsdGenNoiseMap`
and `noise()` in an expression all call the same code, so they agree bit for bit. Having `noise()`
mean two things in one product is worse than a one-line porting note. (Verified: `noise($P*4)` at
integer `P` prints 0.5.)

The `usdGenMath` target exposes thin wrappers so the C++ stylers do not include SeExpr headers.
`usdGenMath` is a **CMake target name, never a C++ scope**: the file-scope namespace is
`namespace usdGen { … }` (`10-build-dependencies-testing.md` §7.1) and kernels are free functions
`UsdGenXxxKernel(...)` (`03-execution-engine.md` §8.3), so the wrappers are the free functions
`UsdGenNoise3`, `UsdGenFbm3`, `UsdGenCellNoise3` and `UsdGenVoronoi3`, declared in
`usdGenMath/noise.h` inside `namespace usdGen`. They are compiled with `-ffp-contract=off` like the
rest of the target (ADR §4.1), which is what makes gate E-8's bitwise determinism hold.

### 7.5 Compile caching, thread safety and cost

```cpp
class UsdGenExprProgram {                       // one per distinct (source, returnType, seed)
public:
    bool        IsValid() const;                // SeExpr2::Expression::isValid()  (Expression.h:133)
    const char *Error()   const;                // parseError()                    (Expression.h:140)
    int         ChannelCount() const;           // 1 for float, 3 for color
    void        Eval(UsdGenMapEvalContext *ctx, const UsdGenMapSample &s, float *out) const;
};
```

* **Cache key** is `hash(source text, returnType, seed, sorted map-target names)`. Prep is lazy and
  happens on the first `isValid()`/`evalFP()`; it costs 7–53 µs (MEASURED, **EV-068**), so a groom with a
  hundred distinct expressions pays a few milliseconds once per compile, not per capture.
* **Thread safety** is one `VarBlock` per worker, created with `creator.create(/*makeThreadSafe*/
  true)` and held in `UsdGenMapEvalContext`. That flag makes the interpreter memcpy its constant
  arrays into the block per evaluation (`Interpreter.cpp:39-46`) at a MEASURED ~10 % cost
  (128 ns vs 117 ns on the noise+fbm expression, **EV-069** against **EV-067**) — cheap, and the
  alternative (one `Expression` per
  worker) multiplies prep by the thread count. `VarBlock::indirectIndex` lets one block address SoA
  arrays, which is how a whole chunk is evaluated without touching the block between roots.
* **Never** evaluate one `Expression` from several threads without a thread-safe `VarBlock`:
  `evalFP()` writes into the shared `_interpreter->d` (`Expression.cpp:304-309`). This was verified
  the hard way in the research round, along with the rule that `ExprVarRef::eval(double*)` must
  write exactly `type().dim()` doubles or it corrupts adjacent interpreter slots.
* **Cost** (MEASURED, **EV-067** and **EV-069**, `appendix-A-evidence-ledger.md` §2.8;
  `prototypes/thirdparty-bench/seexpr_bench.cpp`): 13 ns for `$u*$v+1`, 34 ns for
  `map("length")*(0.5+hash($id))`, 106 ns for a cellnoise+voronoi+smoothstep expression, 117 ns for
  `fit(noise($P*4)+0.5*fbm($P*2,4),0,1.5,0.2,1.0)`; 159 ns/eval/thread at 8 threads = 50 M evals/s
  aggregate. A million roots × ten attributes is `DERIVED from EV-067`/`EV-069` at ≈ 1 s
  single-threaded and ≈ 0.15 s on 8 threads:
  acceptable once per edit, unacceptable per frame — which is §5.6's rule, restated as arithmetic.

### 7.6 Sandboxing

The expression language is artist-authored data that arrives with a scene file, so it is treated as
untrusted input:

1. **No JIT.** Interpreter only; the LLVM backend is not compiled in
   (`ENABLE_LLVM_BACKEND=OFF`), so no expression can produce executable pages.
2. **No plugin loading.** `ExprFunc::init()` reads the colon-delimited `SE_EXPR_PLUGINS`
   environment variable and `dlopen`s every path in it (`ExprFunc.h:49-50`, `ExprFunc.cpp:158-159`,
   `research/A8-seexpr-ptex-libs.md` §1.3). usdGen calls `unsetenv("SE_EXPR_PLUGINS")` exactly once,
   under a `std::once_flag`,
   before the first expression is compiled, and emits a `TfWarn` naming the variable if it was set.
   A DCC that legitimately uses SeExpr plugins elsewhere must load them before usdGen's first
   compile; that trade is documented and is the safe direction.
3. **No file access from the language.** `map()` and `ptex()` take a prim *name* resolved against
   `usdGen:expr:maps` (§7.3). There is no function that opens a path.
4. **No unbounded work.** The language has no loops and no recursion; the only per-evaluation cost
   an artist can inflate is expression length, which is linear and is shown in the stack profiler
   column (`08-tools.md`).
5. **`printf`/`sprintf` stay available** but are routed through `TfDebug`-guarded output and
   rate-limited to the first 64 calls per capture — otherwise a `printf` in a 1 M-root expression is
   a self-inflicted hang.

### 7.7 Error reporting

A parse or prep failure is a **compile error on the map prim**, not a runtime surprise:

* `UsdGenExprProgram::IsValid()` is checked when the graph compiles. On failure the node is marked
  invalid, `Error()` (SeExpr's `parseError()`, which carries the character offset) is reported once
  per `(prim path, source digest)` through `TfWarn`, and the map evaluates to `usdGen:map:default`
  everywhere so the groom still renders and still looks obviously wrong.
* The tool's expression editor shows the same string inline against the offending character and
  refuses to commit an invalid expression to the stage unless the artist insists — the message and
  the badge come from the same source (`08-tools.md`).
* An expression whose `returnType` disagrees with its use (a `color` feeding a scalar parameter) is
  the same class of error, caught at prep by `Expression::returnType()` / `isVec()`
  (`Expression.h:177-182`).
* `Expression::isThreadSafe()` only reports whether a *called function* declared itself unsafe
  (`Expression.h:162`); usdGen's three additions all declare themselves safe, and the flag is
  asserted in a T0 test so a future addition cannot silently break the parallel capture.

### 7.8 Where expressions may run

| Site | Tier | When it runs |
|---|---|---|
| `UsdGenExprMap` | M4 | **capture only** — as a map, wherever a map may be read (§5.6) |
| the `expr` slot of the mask block (`UsdGenMaskAPI`) | M4 | capture only |
| `UsdGenExprOp` — a styler whose kernel is an expression | v2 (ADR §9.5 R38), M8 | **capture-time only**, and it may not change topology |

`UsdGenExprOp` is the non-C++ extension tier for TDs (ADR §3). Its capture-time-only restriction is
not a soft guideline: an operator that evaluated an expression per CV per frame would cost
≈ 0.1 µs × CV count every frame — ≈ 160 ms per frame at 1.6 M CVs single-threaded and ≈ 32 ms on 8
threads (`DERIVED from EV-067` and `EV-069` — 106–117 ns/eval and 50 M evals/s aggregate,
`appendix-A-evidence-ledger.md` §2.8, `research/A8-seexpr-ptex-libs.md` §1.6) — against a whole-frame
budget of about 15 ms (S45; ASSUMPTION — 16.6 ms less usdRig's own tail, `appendix-A-evidence-ledger.md`
§2.11. `09-performance-and-benchmarks.md` §0.2 is the only frame ledger, ADR §9.5 R41). The interface enforces it — `UsdGenExprOp` implements `Capture()` and gets the
identity `Evaluate()`; its output is a per-curve or per-CV field consumed by the ordinary kernels.

---

## 8. PaintMap

### 8.1 Where painted values live

A `UsdGenPaintMap` has three storage states and moves between them by an explicit action:

| State | Where the values are | Set by |
|---|---|---|
| in-session | a live override on the surface prim's `primvars:usdGen:paint:<name>` (never yet written to the stage) | every brush move |
| authored | `primvars:usdGen:paint:<name>` on the **surface mesh**, in the current edit target | brush release, inside one undo bracket |
| baked | `usdGen:paint:bakedFile` — an `.exr` (UV space) or a `.ptx` (per-face) beside the asset; the primvar may then be removed | the explicit **Bake map** action |

**`usdGen:paint:storage` selects the source, and it is the only thing that does.** `primvar` (the
default) reads `primvars:usdGen:paint:<name>` from the surface prim through the scene index — which
covers both the in-session and the authored state, because a live override and an authored value
arrive at the scene index the same way. `file` reads `usdGen:paint:bakedFile`. **Bake map** writes
the file and flips the token in the same undo bracket. An authored `bakedFile` with
`storage = "primvar"` is ignored; `storage = "file"` with no `bakedFile` is a compile error and the
map returns `usdGen:map:default` (§5.1) so the groom still renders.

Storing painted values as an ordinary primvar on the mesh is deliberate: it is a normal USD
attribute an artist can inspect, diff, version and hand-edit, it composes with layers and variants,
and nothing writes an image file during interaction. `usdGen:paint:primvar` names it,
`usdGen:paint:interpolation` says how it is laid out (`uniform` per face, `vertex`, `faceVarying`),
and §5.3 says how a root's value is interpolated from it.

The map prim, not the mesh, is what operators reference — so an artist can retarget a mask from a
painted primvar to a Ptex file to an expression without touching a single operator.

### 8.2 How the brush writes them

The loop is the same live-override loop as comb and cut (S40, `08-tools.md`), with one difference:
it touches a prim usdGen does **not** own.

1. **Press.** Resolve the surface point under the cursor with
   `UsdGenImaging_ClosestSurfacePoint(surfacePath, p, &faceId, uv, P)`; snapshot the primvar array
   for undo (`SubtreeSnapshot`, S41).
2. **Move.** Update the values in the session's live buffer and dirty **only**
   `primvars:usdGen:paint:<name>/primvarValue` on the surface prim — 0.24 µs, no descriptor rebuild
   (MEASURED, **EV-034**, `research/G-tool-loop-array-transport-and-cv-picking.md` §5). The recapture
   of the affected operator runs on the same commit: ADR §4.3 **trigger (c)**, which ADR §9.4 R32
   makes unconditional — it applies with or without an app driver, committing synchronously at the
   end of the `_PrimsDirtied` batch that carried the dirty (`06-imaging.md` §3.9).
3. **Appearance, and the S5 locator.** The first time the primvar exists, dirty
   `primvars:usdGen:paint:<name>` once (MEASURED 2.06 µs, **EV-034**); a universal dirty does **not** refresh
   primvar descriptors and is silently a no-op for this purpose
   (`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:510-522`). The **bare `primvars` locator**
   that S5 requires for an overlaid upstream prim is dirtied here too — once, when usdGen first
   overlays that prim's `primvars` container, and once more when the overlay is removed. It costs
   4.10 µs (**EV-034**) and *does* rebuild the descriptor cache, which is exactly why it is a
   once-per-overlay event: sending it per move would rebuild the primvar-descriptor cache on every
   mouse move and is forbidden (MEASURED, same report §5; the mechanism is the last-element test at
   `sceneIndexAdapterSceneDelegate.cpp:511-515`, which clears the cached descriptors for any
   `primvars` locator whose last element is not `primvarValue`/`indexedPrimvarValue`/`indices`).
   Without the bare locator a UsdSkel-resolved scalp freezes — a measured interop bug (S5, ADR
   §5.2). Keep the announced set so disappearance can be dirtied too.
4. **Release.** One `attr.Set(vt)` (MEASURED 1.63–1.82 µs at any array size, **EV-058**,
   `appendix-A-evidence-ledger.md` §2.7) into the edit target, inside one
   `Sdf.ChangeBlock` and one undo bracket.

Progressive feedback during the stroke is the mask visualisation mode (`08-tools.md`): the painted
field is shown on the scalp while the groom behind it updates at the recapture rate.

### 8.3 Bake to file

**Bake map** converts the primvar to a file and rewrites `usdGen:paint:bakedFile`:

* **UV space → `.exr`**, float or half, through `HioImage::OpenForWriting` + `Write`; the EXR plugin
  in this install is the built-in nanoexr writer and handles float/half up to four channels
  (`pxr/imaging/plugin/hioOpenEXR/OpenEXRImage.cpp:831-938`). PNG is offered only for 8-bit colour:
  the stb writer quantises float to 8-bit for everything but `.hdr`
  (`pxr/imaging/hio/stbImage.cpp:615-700`).
* **Per face → `.ptx`**, through `PtexWriter` with adjacency from `Far::PtexIndices::GetAdjacency`
  (§6.5). `usdGen:paint:resolution` (default 256) gives the per-face `Res`, which is the same
  quantity XGen writes as `#3dpaint, N`.

After a bake `usdGen:paint:storage` is `"file"`, the sampler reads the file, the primvar can be
deleted from the mesh, and the capture epoch changes because the map digest now includes the new
resolved path.

### 8.4 Ptex versus per-face primvar

| | primvar on the mesh | `.ptx` file |
|---|---|---|
| resolution | one value per face (`uniform`), or per corner (`vertex`/`faceVarying`) | up to `Res(ulog2, vlog2)` texels per face, independently per face |
| storage | in the layer: diffable, composable, versionable with the asset | an opaque binary sidecar |
| write cost during a stroke | none (live override; one `attr.Set` on release) | none — files are only written by **Bake map** |
| read cost at capture | array indexing, effectively free | 23 ns/lookup (MEASURED, **EV-070**) |
| memory | `numFaces × 4 B` (or corners × 4 B) | bounded by `USDGEN_PTEX_CACHE_MB` |
| tooling | usdview, `usdcat`, any USD tool | `ptxinfo`, a Ptex-aware paint tool |
| good for | masks, density, region/parting ids, coarse tint — anything whose detail is at or below face resolution | fine painted detail: hairline shapes, painted length falloff, parting lines on a low-poly scalp |
| bad at | detail finer than the mesh | review, diffing, and any workflow that wants the value in the layer |

Default: **primvar**. The tool offers "Bake to Ptex" when the artist zooms past face resolution and
says in one sentence what they give up. Both are the same `UsdGenPaintMap` prim, so nothing
downstream changes when they switch.

---

## 9. Testing

Tiers are T0–T4, fixed by ADR §9.1 R2 and defined in `10-build-dependencies-testing.md` §5.1: T0 engine,
no USD and no Hydra; T1 headless scene index over the real `UsdImagingCreateSceneIndices` chain;
T2 Storm through the EGL device-platform harness (`prototypes/storm-hair-look/eglctx.h`) on the
GB10; T3 `testusdview` under a user-space Xvfb on `DISPLAY=:77` (llvmpipe — CPU numbers, never quoted
as Storm numbers), with `10-build-dependencies-testing.md` §5.2 owning how CI stands that server up
(the `USDGEN_XVFB_ROOT` entry of its §3.5 variable registry), since
the research scratchpad it originally ran in no longer exists (§2.8); T4 workstation protocol, a
release criterion and never a milestone exit.

### 9.1 Gates this document is accountable for

| Gate | Tier | Milestone | Assertion |
|---|:--:|:--:|---|
| **L-1** (ADR §9.5 R40) | T2 | M0 pre-work, binding at M1 | With the three-terminal material bound, the compiled fragment shader contains `UsdGenHair.Surface` and not the UsdPreviewSurface source — i.e. Storm resolves `outputs:glslfx:surface` before plain `outputs:surface`. Green ⇒ `USDGEN_STORM_MATERIAL_OVERRIDE` stays OFF; red ⇒ flip the default and synthesize `material_storm` (§3.3). |
| **S-8** (ADR §5.4, §9.5 R40) | T2 | M0 pre-work, binding at M1 | Variant A vs Variant B frame time on a 100 k deforming groom, and Variant A compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`. Decides which tangent block ships in the two default files. |
| **L-2** (ADR §9.5 R40) | T2 | M1 | **All three** shipped `.glslfx` files (§2.1) parse in Sdr with **exactly** the 20 inputs of §2.2 and the primvars metadata each variant declares (`hairId\|hairT\|hairTangent\|st` for Variant B, `hairId\|hairT\|st` for Variant A); compile and render with 0 warnings and 0 GL errors; the golden image matches `prototypes/storm-hair-look/hair_pv_tangent.png` within a fractional-pixel-difference tolerance; the per-prim `displayStyle/refineLevel` wins over the app's complexity. **CPU/GPU parity clause:** a groom at `bakeMode = "perCV"` with the destination resolving to `displayColor` (§1.3) and neutralised shader inputs, and the same groom at `bakeMode = "perCurve"` with authored shader inputs, match to within one 8-bit code value in the golden image — this is what proves the §1.2 bake order and the glslfx jitter block agree. Guards contract C5. |
| **L-3** (ADR §9.5 R40) | T0 | M4 | Map and expression determinism: the same groom captured at 1 thread and at 8 threads produces bitwise-identical baked primvars; `Expression::isThreadSafe()` is true for every shipped function; `rand()` is reproducible across runs. |
| **L-4** (ADR §9.5 R40) | T0/T1 | M4 | Map capture cost at 1 M roots on 8 threads is within its threshold, and `PtexCache::Stats.fileReopens == 0` with the default cache size; it also settles the three ASSUMPTION cache defaults of §5.4 and §6.4. The threshold is **≤ 200 ms for one image map + one Ptex map + one expression at 1 M roots** (an ASSUMPTION consistent with the ≈ 20 ms + ≈ 4.4 ms DERIVED figures of §5.6 and §6.4), registered as L-4's pass criterion in `09-performance-and-benchmarks.md` §5.4. |
| **L-5** (ADR §9.5 R40) | T1 | M4 | `UsdGenImaging_ReloadMaps()` re-captures exactly the nodes whose maps changed and dirties exactly the baked primvar's `primvarValue` (`primvars/displayColor/primvarValue`, or `primvars/<bakePrimvar>/primvarValue` when `bakeTarget = "primvar"` names a custom primvar) on exactly the affected tiles; an unrelated description does not move; `displayColor` is never co-dirtied with `points`. |
| **S-1 … S-7** (`09-performance-and-benchmarks.md` §5.3) | T2 | M1/M7 | The Storm throughput gates. Two of them bind this document: **S-5** (`drawBatches == 1`, `drawCalls == 1` for one material and one refineLevel — the batching rules of §3.4) and **S-2** (a deform frame costs ≤ 2.0 ms over static — which holds only because the bake does not run per frame, §1.5). |
| **R-2** (`09-performance-and-benchmarks.md` §5.4) | T4 | release | MSAA / alpha-to-coverage versus OIT quality on 1-px strands at 1080p and 4K: visual sign-off on which tag reads better. |
| **R-3** (`09-performance-and-benchmarks.md` §5.4) | T4 | release | The whole shader set compiles with `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`. The **compile** half runs on this host — the env var forces the Hgi resource path even on HgiGL (`return isEnabled \|\| !_IsHgiOpenGL(hgi)`, `pxr/imaging/hdSt/codeGen.cpp:189-197`) — and is exactly what S-8 checks at M1. What cannot be closed here is the **rendered result** on a real Metal or Vulkan backend, since `libusd_hgiGL.so` is the only Hgi backend built. |

**ADR §9.5 R40 makes `09-performance-and-benchmarks.md` §5 the single gate registry**, and its §5.4
already carries L-1, L-2, L-3, L-4 and L-5 with the tiers, milestones and pass criteria this table
repeats — L-1 T2 M0 pre-work deciding M1, L-2 T2 M1, L-3 T0 M4, L-4 T0/T1 M4 (≤ 200 ms), L-5 T1 M4 —
alongside T-EXPR-1 and T-PTEX-1, the two M4 T0 gates that also cover this document's §7.6 sandbox and
§6.3 face ids. `00-request-and-scope.md`'s gate-family list,
`10-build-dependencies-testing.md` §6.5's gate→test map and `11-roadmap.md`'s milestone exits cite
09 §5.4 rather than restate it. This document defines the **assertions**; it does not define a
gate's identity, its tier or its milestone, and where 09 §5.4 and this table ever disagree, 09 §5.4
wins (R40).

The same registry fixes **S-8** and **L-1**: R40 places both as M0 pre-work binding at
M1 — what `09-performance-and-benchmarks.md` §5.3 (S-8) and §5.4 (L-1) already record as "M0
pre-work, decides M1", and what ADR §7's pre-work list means by "S-8/S-9/L-1 via the EGL harness" in
M0. The measurement happens in M0; the decision it forces — which tangent block ships, whether
`USDGEN_STORM_MATERIAL_OVERRIDE` flips — binds at the M1 exit.

### 9.2 Tests by tier

**T0** — `testUsdGenMaps`, `testUsdGenExpr`, `testUsdGenPtex`, `testUsdGenLookBake`, plus the
benchmark `benchUsdGenMaps` that carries gate L-4 (`10-build-dependencies-testing.md` §5.6): sampler
correctness per map type and per interpolation; channel/scale/offset/clamp arithmetic; the §1.2 bake
order against a hand-computed reference; `hairId = UsdGenHash32(curveId, 0) / 2^32` over the stable
64-bit `curveId`, never the curve index (ADR §9.2 R12); every SeExpr variable bound
and readable; `rand()` determinism; `noise()` in 0..1 and `snoise` in −1..1; Ptex face ids for a quad
mesh, an n-gon mesh and a subset-restricted scatter; gates L-3 and L-4.

**T1** — `testUsdGenLookInvalidation`, `testUsdGenMapReload`: a `usdGen:look:*` edit dirties
`primvars/displayColor/primvarValue` and nothing else; a `bakeTarget` change dirties
`primvars/<new>` once on appearance and a `bakeMode` change dirties `primvars/<name>` once for the
interpolation flip (§1.3, §1.5); gate L-5; a `GeomSubset` edit recaptures and renumbers nothing;
a missing map file yields one diagnostic and a groom that still renders.

**T2** — the CTest names are `10-build-dependencies-testing.md` §5.6's, one per gate:
`testUsdGenStormLook` (gate L-2), `testUsdGenStormMaterial` (gate L-1), and
`testUsdGenStormTangent` + `testUsdGenStormHgiResource` (gate S-8). Golden images for the opaque,
translucent and preview-texture materials; the assertion that a `.ptx` never appears in any material
network usdGen authors. These are the names `09-performance-and-benchmarks.md` §5.4's Test column
and `10-build-dependencies-testing.md` §6.5's gate→test map both carry, so all three documents now
name one test per gate; 10 §5.6 remains the registry that owns them.

**T3** — `testUsdviewUsdGenPaint.py` (`08-tools.md` §9.3; `10-build-dependencies-testing.md` §5.6)
covers the paint round trip (one primvar write per stroke, undo restores, **Bake map** produces a
readable `.exr` and `.ptx`) and a parse error reported at the right character offset;
`testUsdviewUsdGenLook.py` checks the bound terminal per renderer name against a golden. **T4** — R-2, R-3, and a visual sign-off against the render material in a real
hdPrman install.

---

## 10. Out of scope

1. **A full production hair shading model in Storm.** The shipped glslfx is Kajiya-Kay diffuse plus
   two Marschner-lite longitudinal lobes plus a cheap TT rim. No glints, no eccentricity, no
   multiple-scattering integration, no dual scattering, no per-lobe LPE outputs. The render material
   (§4) is where a physically-based model lives; Storm's job is to predict it, not to reproduce it.
2. **Making MaterialX hair work in Storm.** It compiles and renders and looks near-black; fixing the
   MaterialX light loop is an upstream change, not usdGen's.
3. **Ptex sampling on the GPU.** Impossible in this install and meaningless on curves even with
   `PXR_ENABLE_PTEX_SUPPORT=ON` (§6.1). If a site enables Storm Ptex for its meshes, usdGen's static
   hidden Ptex must not collide with it — that is a build rule (`10-build-dependencies-testing.md`),
   not a feature.
4. **A `HioImage` plugin for `.ptx`** (§6.1).
5. **Per-instance materials** on card/archive instancers: Storm has none; variety comes from
   multiple prototypes (S33).
6. **UDIM for `UsdGenImageMap` in v1.** Storm's own texture path supports `<UDIM>`
   (`pxr/imaging/hdSt/materialNetwork.cpp:754-755`), but the CPU sampler does not in v1; a UDIM set
   is several `UsdGenImageMap` prims and a `UsdGenCombineMap` until it is added.
7. **Automatic pickup of overwritten map files.** Explicit `ReloadMaps` only (S13, §5.5).
8. **Painting directly into a `.ptx` during a stroke.** Painting writes primvars; files are written
   by an explicit bake (§8).
9. **An expression language other than SeExpr**, and OSL of any kind.
10. **Colour management beyond the sRGB/raw decision of §5.3.** No OCIO: it is not in this install.

---

## 11. Sources

**ADR and brief.** `design/adr-v1.md` §9 addendum, which is binding over every earlier section it
touches: **R1** (engine principles P1–P8 are invariants I1–I8; `EV-nnn` is the only handle for a
measured number), **R2** (test tiers T0–T4, hyphenated gate ids), **R4** (`UsdGenHairPreview`,
`UsdGenHairPreviewTranslucent`, `UsdGenHairPreviewPrimvar`; three shipped glslfx files),
**R7** (02 is the normative property registry), **R8** (`bakeMode`, `colorMapMode`, `rampExponent`
in 02 §2.14), **R9** (no `world` space token), **R11** (ramp interpolation is
`linear|catmullRom|bspline|constant`, default `catmullRom`), **R12** (64-bit `curveId`, the pinned
hash, `hairId`), **R16** (`usdGen:mask:source`), **R31** (`08-tools.md` §1.4 is the single source of
the C ABI), **R32** (commit trigger (c) always applies), **R33** (`hairTangentWorld` rejected),
**R35** (the single env-var registry), **R38** (v1 = M0–M7, `UsdGenPtexMap` at M4), **R40** (09 §5
is the single gate registry; L-1/S-8 M0 pre-work binding at M1, L-2 M1, L-3/L-4/L-5 M4), **R42**
(number tags), **R43** (`EV-001…` ledger rows; `hdSt/renderDelegate.cpp:695-707` is the accepted
range), **R45** (cross-references must resolve).
Then §1 (amendments S11, S29, S36), §2.1–2.3 (type hierarchy,
reserved layout, contested properties), §3 (contracts C1, C5), §4.1–4.3 (capture/evaluate, epochs,
commit triggers), §5.2–5.6 (invalidation, tile contract, `hairTangent` fork, refineLevel, Storm
material binding), §6 (look = risk §7 + artist §7.1 + performance §9.1), §7 (milestones M1/M4,
gates), §8 (document map). `design/brief-v1.md` §2.5 (S27–S32), §2.7 (S35–S38), §2.8 (S39–S43),
§2.2 (S8–S16), §2.9 (S44–S46).

**Proposals.** `design/proposal-risk.md` §7.1–7.5 (three terminals and the hedge, the mandatory
primvar set, capture-time colour/maps/expressions, Storm delivery details, reload and the paint
round trip), §3.6 (map properties), §9.1 (test tiers). `design/proposal-artist.md` §7.1–7.4 (the
shader block and its inputs, terminal resolution, colour/maps/expressions, reload and paint), §3.5
(map schema), §3.6 (LOD and density). `design/proposal-performance.md` §9.1–9.3 (the three-terminal
table, the MaterialX chain, baking, reload), §6.3 (what every synthesized prim carries), §7.4 (the
mask block), §11.2 (gates).

**Judges.** `design/judge-evidence.md` §0 (render-context resolution in source; the `hairId` type
contradiction), D5 scores. `design/judge-artist.md` D5 (the `UsdGenLookAPI` bake order and the
`USDGEN_STORM_MATERIAL_OVERRIDE` kill switch), item 24 (the explicit MaterialX chain), item 28.
`design/judge-delivery.md` §8 items 4, 6, 11 (the tangent fork, the `GeomSubset`/Ptex face-id
question, which terminal Storm prefers).

**Research.** `research/G-storm-hair-look-prototype.md` §0 (EGL harness), §1 (three routes), §2.1–2.7
(Sdr parse, compile/render, generated FS layout, primvar and parameter binding, the tangent
decision, material tag/OIT, lights), §3 (MaterialX), §4 (UsdPreviewSurface + uniform `st`), §5
(frame times), §6 (workstation protocol), §7 (recommendation).
`research/A5-storm-curves-shading.md` §1 (capability table), §3 (output representation), §4a–4c
(shading routes), §5 (three colour-map routes), §6 (Hio formats), §7 (per-frame cost).
`research/A8-seexpr-ptex-libs.md` §0 (host inventory), §1.1–1.10 (SeExpr), §2.1–2.9 (Ptex), §3
(Hio), §4 (noise libraries), §6 (dependency strategy), §7 (licensing).
`research/A7-prior-art-grooming.md` §1.3 (the XGen expression dialect), §5.1–5.4 (hair shading
references, the MaterialX hair nodes in this install, BasisCurves conventions), §9.4 (map inputs).
`research/G-storm-throughput-and-prim-granularity.md` §2 "Topology strategy" item 5 and Key facts
(interpolation choices, varying widths), §1.11 (batching). `research/G-tool-loop-array-transport-and-cv-picking.md` §5 (live primvar paint).
`research/A3-usdrig-tools.md` §6 (document conventions). `research/ENVIRONMENT.md` (host facts and
the CORRECTIONS block).

**Prototypes.** `prototypes/storm-hair-look/usdGenHairPreview.glslfx` (the shipped shader; the
`parameters` and `textures` entries of its `configuration` block — the `inputs:` block of §2.2 — are
contract C5, and the `attributes` and `metadata` blocks are not),
`usdGenHairPreview_translucent.glslfx` — the prototype's on-disk name, which ships as
`usdGenHairPreviewTranslucent.glslfx` for shader def `UsdGenHairPreviewTranslucent` (§2.1, ADR §9.1
R4; `appendix-B-prototype-inventory.md` §7 records the same rename) — `eglctx.h`,
`render_hair.cpp`, `bench_hair.cpp`, `sdr_parse_test.py`, `make_scene.py`,
`hair_pv_tangent.png`, `hair_translucent.png`, `hair_preview_tex.png`.
`prototypes/thirdparty-bench/seexpr_bench.cpp`, `se_min.cpp`, `ptex_test.cpp`.

**OpenUSD 26.08 source** (`/home/burkard/work/OpenUSD`, tag v26.08). Every `pxr/...:line` cited
inline in this document was re-read against that tree while writing it; the consolidated file:line
index lives in `appendix-A-evidence-ledger.md` §3.6. The load-bearing ones are the terminal-resolution
chain (`pxr/imaging/hdSt/renderDelegate.cpp:695-707` for the render contexts and `:710` for the
shader source types — ADR §9.5 R43, `pxr/imaging/hd/materialSchema.cpp:61-75`,
`pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:1331-1339`,
`pxr/usdImaging/usdImaging/dataSourceMaterial.cpp:54-66, 676-700`), the material-tag chain
(`pxr/imaging/hdSt/primUtils.cpp:292-327`, `pxr/imaging/hdSt/materialNetwork.cpp:66-80`,
`pxr/imaging/hdx/taskController.cpp:331-336, 392-400`), the shader-identifier chain that makes
`info:id` and not an asset path the right authoring form
(`pxr/imaging/hdSt/materialNetwork.cpp:120-135`,
`pxr/usdImaging/usdImaging/dataSourceMaterial.cpp:554-569`), the complexity→refineLevel conversion
(`pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43`,
`pxr/usdImaging/usdImagingGL/engine.cpp:2317-2350`), the glslfx plumbing
(`pxr/usdImaging/plugin/sdrGlslfx/parserPlugin.cpp:236-242, 262-275`,
`pxr/imaging/hio/glslfxConfig.cpp:34, 548-549`, `pxr/usd/usdShade/shaderDefUtils.cpp:51-52`,
`pxr/usd/sdr/filesystemDiscoveryHelpers.cpp:127-175`,
`pxr/usd/plugin/usdShaders/discoveryPlugin.cpp:30-70`), the curve shader facts
(`pxr/imaging/hdSt/shaders/basisCurves.glslfx:746, 1212-1218, 1244, 1272-1274, 1293`,
`pxr/imaging/hdSt/basisCurves.cpp:292-301, 322-343`), the primvar-filter chain (`pxr/imaging/hdSt/primUtils.cpp:114-142`,
`pxr/imaging/hdSt/materialNetworkShader.cpp:235, 383-413, 415-437`,
`pxr/imaging/hdSt/basisCurves.cpp:1357-1386`), and the Ptex stubs
(`pxr/imaging/hdSt/ptexTextureObject.cpp:111-120, 121-229, 233-268`,
`cmake/defaults/Options.cmake:36`).

**Installed and vendored third party**: `opensubdiv/far/ptexIndices.h:46-88` and
`libraries/pbrlib/pbrlib_defs.mtlx:146-158, 433-439, 455-463` and
`libraries/stdlib/stdlib_defs.mtlx:2821-2824` in the install (the pbrlib file is 465 lines long —
citations past `:463` are out of range and were corrected in this revision); Ptex v2.4.3
(`Ptexture.h`, `PtexSeparableFilter.h:80-81`, `PtexCache.cpp:61-66`) and SeExpr `main@8f8c8f2`
(`Expression.h`, `Expression.cpp:304-309`, `Interpreter.cpp:31-46`, `VarBlock.h`, `ExprFuncX.h`,
`ExprFunc.h:49-50, 62, 68, 87-118`, `ExprFunc.cpp:110-134, 139-147, 158-159`,
`ExprBuiltins.cpp:1295-1340, 1719-1849`, `Noise.h:23-36`) as
built in the research round; signatures quoted inline above.
