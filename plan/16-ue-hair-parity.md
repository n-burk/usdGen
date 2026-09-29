# 16 — Strand hair shading on stock Storm

Target look: dense dark combed hair, soft deep self-shadow, broad low-contrast
sheen, lighter flyaways, sub-pixel strands that fade instead of aliasing, skin
visible through the temple fade, hair shadow on the scalp.

Comparison harness: `examples/head-hair-closeup.usda` (generator
`examples/tools/make_head_hair.py`). `bin/render_ue_parity.ps1 -Label <x>`
writes captures under `renders/ue-parity/`, which is gitignored.

Constraint (unchanged): stock OpenUSD Storm, no renderer plugin, no OpenUSD
patch. Everything is a glslfx material + primvars published by the engine.
The C5-frozen inputs of `UsdGenHairPreview*` are NOT touched.

## What the host renderer does, and our stock-Storm equivalent

| the host renderer HairStrands | Here |
|---|---|
| Karis 2016 Marschner approximation (`HairShading`: R, TT, TRT with `Hair_g` normalized Gaussians, shifts -2a/a/4a with a=0.035, variances r^2, r^2/2, 2r^2, n=1.55 Fresnel, closed-form N terms, absorption `pow(BaseColor, k/cosThetaD)`), inputs BaseColor/Roughness/Specular/Scatter/Backlit | WS1: new glslfx `UsdGenHairStrands`, exact port |
| Dual scattering (Zinke 2008) driven by hair count from deep-opacity / voxel density; global forward-scatter transmittance + spread, local back-scatter term; replaces the `Scatter` diffuse fake when strands have hair-count data | WS1 shader + WS2 data: directional hair count = our baked optical depth (expected fibre crossings) |
| Voxel density volume, ray-marched per light and for sky/AO | WS2: keep CPU voxel bake; raise angular fidelity and resolution, tessellated-curve splat, exclude self |
| Stable rasterization: strand never thinner than ~1 px, coverage scaled by true/clamped width; per-sample visibility + coverage compositing | WS1 (shipped): coverage = truePixelWidth / clampedPixelWidth, spent by writing `gl_SampleMask` with a per-strand hashed subset — neither of Storm's compositing modes can carry it, and usdview never enables alpha-to-coverage at all. Translucent/OIT variant kept for hero stills that fit its fragment pool |
| Sky light on hair: BSDF evaluated against a bent/unoccluded direction with voxel AO; no Lambert | WS1: dome evaluated through the same BSDF (few fixed directions + irradiance), scaled by directional visibility |
| Hair casts onto skin (deep shadow/voxel) | WS3: Storm shadow maps where the host enables them; document limit |
| Material Hair Attributes: root UV, seed, U along strand, per-strand colour/roughness; melanin/redness/dye helpers | WS1: `hairT`, `hairId`, `st`, `displayColor`; melanin/redness/dye inputs |

## Workstreams

### WS1 — `UsdGenHairStrands` glslfx (Opus) — SHIPPED

`usdGenShaders/resources/shaders/usdGenHairStrandsBsdf.glslfx` holds the whole
implementation; `usdGenHairStrands.glslfx` (opaque) and
`...Translucent.glslfx` (OIT) import it and differ only in `materialTag` and
the stochastic-coverage selector that follows from it. 5th and 6th entries in
`shaderDefs.usda`, outside C5 — the three frozen files are untouched and
`checkC5.py` still passes. Variant A tangent, NEGATED: the host renderer's `N` points toward
the ROOT and Storm's is the root-to-tip parameter derivative.

19 inputs, the host renderer's names: `baseColor`, `tipColor`, `colorRamp`, `useMelanin`(0),
`melanin`(0.5), `redness`(0), `dyeColor`(1,1,1), `roughness`(0.35),
`specular`(0.5), `scatter`(**0.0**), `backlit`(1.0), `opacity`(1),
`randomHue`, `randomValue`, `randomRoughness`, `selfShadow`(1.0),
`hairCoverageScale`(1.0), `minPixelWidth` informational, texture
`baseColorMap`.

`scatter` defaults to 0, not the 1.0 this plan first specified: it is the host renderer's own
default and what `HairSampleToGBufferData` forces on the strands path, and at
1.0 the Kajiya term — which the host renderer adds on top of dual scattering, un-attenuated,
as `sqrt(BaseColor)` — replaces the authored dark coat with a flat pale mass
(`renders/ue-parity/ws1_scatter{0,1}_temple.png`).

Implemented: the Karis lobes line for line with the host renderer's constants, in the host renderer's energy
units (no extra NoL, no artist budget, no tonemap, `light.diffuse.rgb` as
radiance in the same convention `previewSurface.glslfx` uses); Zinke dual
scattering from the baked directional hair count, with `a_f`/`a_b` as
39-coefficient angle-resolved fits of the host renderer's LUT integrand rather than a shipped
texture; the sky evaluated through the BSDF as a normalised 8-tap fibre-frame
quadrature; the host renderer's coverage compensation; and the pixel footprint as the host renderer's `Area`
variance widening.

The one thing that is not a port: the host renderer spends coverage through a per-sample
visibility buffer, and neither of Storm's compositing modes can. The opaque
file therefore drives `gl_SampleMask` itself — a hashed subset whose popcount
is `alpha * multisampleCount`, stochastically rounded, rotated per strand — and
outputs alpha 1. That is the default binding because Storm's OIT pool is
`8 * width * height` fragments for the whole frame and a groom exhausts it,
losing whole regions; and because usdview never enables alpha-to-coverage at
all, so spending coverage through alpha has no mechanism there. It costs
early-Z (+14% on the wide shot). Measured composited opacity over the dense
mass: 0.976 in usdrecord, 0.968 in usdview, against 0.948 with 20% of pixels
opaque for OIT. `docs/storm-fur.md` carries the numbers, the failure modes and
the approximations.

### WS2 — density data (Opus)
`furOcclusion.cpp`: (a) splat along the evaluated cubic, not the control
polygon; (b) subtract the receiver strand's own contribution; (c) raise
default resolution (48 -> 96, measure) or fit the grid to the groom bounds
per description; (d) more directions than 6 if the SSBO budget allows (pack
as additional vec3 primvars; limit noted in docs/storm-fur.md) — evaluate
L1 SH of optical depth (4 coeffs = 1 vec4) vs. 6/14 fixed directions and pick
by render comparison; (e) publish hair COUNT semantics explicitly so the
shader's dual scattering constants match the host renderer's.
Tests: extend `tests/testUsdGenFurOcclusion.cpp`.

### WS3 — default binding, look plumbing, shadows (Opus/Sonnet)
The binding and the look plumbing SHIPPED with WS1:
`UsdGenTilePublisher::DefaultMaterialIdentifier()` returns `UsdGenHairStrands`,
`BuildDefaultMaterialDataSource(UsdGenLookDesc const&)` carries the
description's root/tip colour, ramp exponent and hue/value jitter into the
material's parameters, and `DefaultMaterialLookDigest()` lets the groom scene
index dirty the synthetic material prim when the look changes (it is built from
its path in `GetPrim`, so nothing else would announce a look edit).

Hair shadow onto the scalp also SHIPPED, but not through Storm's shadow maps:
those do not exist. `inputs:shadow:enable` is never read by anything
(`HdStLight` asks for `hasShadow`, nothing in usdImaging produces
`shadowParams`, `HdxTaskController::SetEnableShadows` has no callers in pxr, and
the only `HdxShadowMatrixComputation` in OpenUSD is a unit-test one), so no
UsdLux light of any type casts a shadow through usdview or usdrecord — enabling
it on the key light moves 56 of 1 228 800 pixels, which is antialiasing noise.

The shadow is published as geometry instead: one synthetic Mesh per
description, `<description>/__usdGenRender/scalpShadow`, the haired part of the
emitting surface carrying HAIR-ONLY `furTauP`/`furTauN`, bound to a seventh
glslfx (`UsdGenScalpShadow`, outside C5) that writes black with alpha `1 - T`
under the translucent tag, so the pass multiplies whatever skin material the
user authored. `T` reuses WS1's `a_f` fit and WS2's cosine-power-4
reconstruction, weighted per light by `radiance * attenuation * max(0, N.L)`,
with no `HairCount - 1` shift because the receiver is skin rather than a fibre.
It works under usdview's default headlight + dome, where a shadow map would
have nothing to offer. `USDGEN_SCALP_SHADOW=0` publishes no cap.

Cost: +20 ms on the head groom's bake (one extra hair-only sweep plus the cap),
+2.6 ms of frame time. `renders/ue-parity/ws3_*` plus three before/after crop
sheets; `docs/storm-fur.md` carries the measurements and the limits.

### WS5 — strand anti-aliasing (Opus) — SHIPPED, mostly as negative results

the host renderer hides strand aliasing behind 8x MSAA with per-sample visibility plus TAA/TSR.
Stock Storm has none of that, and at the old 4x default a sub-pixel strand had
five coverage levels to spend, so the coat read as speckle and broken dashes.

What shipped:

* **8x MSAA**, set in `bin/launch_usdview.ps1` and `bin/record_usd.ps1`
  (`HDX_MSAA_SAMPLE_COUNT`, guarded so a caller's value wins; `-Msaa N` per
  render). head-hair-closeup at 1280: 14.84 / 16.71 / 24.52 ms and spike score
  0.00631 / 0.00536 / 0.00530 at 4x / 8x / 16x, against 0.00176 for a converged
  reference. 8 takes nearly all of it for +12.6%; 16 costs +65% for nothing.
* **A supersampled reference**, `-Supersample N` on both render scripts plus
  `bin/downsample_linear.py` (box filter in linear light, alpha-weighted). This
  is what separates "the shader is wrong" from "this needs more samples", and
  it is how everything below was judged.
* **The no-`hairId` sample-mask collision fixed** — see docs/storm-fur.md.
  Native `BasisCurves` bound to this material had every strand in a pixel take
  an identical mask; coverage 0.251 against a converged 0.353, now 0.325.

What was tried and rejected, each on measurement:

| | result |
|---|---|
| per-fragment rounding dither | speckle +8%; converts banding into equal noise |
| deterministic rounding | indistinguishable at 8x and above |
| forcing the `Area`/footprint widening (0.01, 0.04) | no effect; the flecks are different strands winning different pixels, not one strand's lobe being too sharp |
| cross-strand coverage profile | spike 0.00536 -> 0.00588, coverage 0.3110 -> 0.3064 |
| per-sample shading / true per-sample visibility | spike 0.00536 -> 0.00603 at **3x** frame cost (16.71 -> 50.49 ms) |
| `minScreenSpaceWidths` 1.3 | untested — the scene used was unrepresentative, and a real test needs the C2 primvar varied on a usdGen groom |

The two profile/per-sample results have the same cause and it is worth keeping:
the shipped mask lights *exactly* k of N samples, so it is stratified; both
alternatives redistribute the same expected coverage with higher variance. A
strand wider than a pixel also already gets exact edge coverage from the
rasterizer, so any shader-side profile double-softens it. the host renderer can afford
per-sample because its visibility buffer resolves coverage analytically rather
than stochastically.

The residual gap to the converged reference is one shading sample per pixel and
a finite sample count. Nothing inside a stock-Storm fragment shader closes it;
it needs more samples or a temporal filter, and Storm has no temporal filter.

### WS4 — verification (Sonnet)
`checkC5.py` still passes for the frozen three; new Sdr checks for the new
defs; `validate_fur.py` extended to the new material; ctest T0/T1 green;
`render_ue_parity.ps1 -Label <stage>` after each workstream; update
`docs/storm-fur.md` and `examples/README.md`.

## Order
WS1 first (largest visual delta, shader only, uses existing furTauP/N), review
renders; then WS2 and WS3 in parallel (disjoint files: usdGen core vs
usdGenImaging publisher); then WS4; then groom/scene tuning against the target.
