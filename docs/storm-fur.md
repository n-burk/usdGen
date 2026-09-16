# Storm fur shading and self-shadowing

The implementation works with stock OpenUSD 26.08 Storm/OpenGL. It provides
geometry-derived self-shadowing, R/TT/TRT strand highlights, absorption-colored
transmission, pixel-footprint highlight filtering, and bounded colored fill.
It does **not** establish quality or performance parity with Unreal Engine.

## Findings

The procedural groom publishes tiled BasisCurves, not one draw primitive per
hair. Its former self-shadow was a smoothstep of `hairT`, independent of strand
density, nearby curves, and light direction. The previous specular/transmission
functions also normalized zero-length projected directions for axial views.

The old `tools/fur_gen.py` used cone prototypes, `points` instead of PointInstancer
`positions`, no `protoIndices`, repeated IDs, and invalid visibility/camera
attributes. The generator now produces valid native instanced cubic curves,
vertex taper/root-to-tip coordinates, unique IDs, per-instance colors, a bound
hair material, and a combed coat. Existing files in `renders/fur` are historical;
regenerate them into a new output path using the tool below.

## Implementation

`furOcclusion.cpp` voxelizes the entire groom's strand control polygons into a
48-cubed grid in world space. Each segment deposits projected width times length
for the three axes, using trilinear splats. Six prefix sweeps integrate optical
depth toward positive/negative X/Y/Z. Trilinear gathers publish two float3
vertex primvars, `furTauP` and `furTauN`. The temporary density/sweep arrays are
about 1.7 MiB at resolution 48; published depth costs 24 bytes per CV, plus the
Hydra packed copy. Work is linear in segment samples, voxels, and output CVs.

All tiles of a procedural description share the volume. Width, point, topology,
and transform edits rebuild it; affected receiving tiles get primvar dirties even
when their own geometry did not change. Unchanged geometry shares the preceding
immutable arrays. The normal generation cache also retains these planes. Light
and camera motion perform no bake. Other descriptions are separate volumes.

The shader interpolates directional optical depth with squared light-direction
weights and evaluates Beer-Lambert transmittance. Environment occlusion averages
six directional transmittances. Reflection/refraction lobes use longitudinal
Gaussians, azimuthal terms, Schlick Fresnel, and absorption derived from albedo.
Artist gains share a bounded budget; this is an approximate real-time model,
not a normalized path-traced hair BSDF. The material still multiplies host Storm
shadow maps when enabled. Opaque alpha-to-coverage and translucent OIT variants
share the same scattering code. All existing input names/defaults are retained.

Depth is packed into vector attributes because twelve independent instance
scalar buffers exceed the GL per-stage SSBO limit on the tested RTX 4090.

## Use

Procedural CPU grooms automatically receive the density data and use the existing
`UsdGenHairPreview` default material at high/veryhigh complexity. There is no new
renderer plugin or OpenUSD patch. CUDA-to-stock-Storm publication remains
unimplemented in this checkout; this change does not add that handoff.

For native PointInstancer scenes, use the offline bake utility. It accepts one
BasisCurves strand per prototype, with constant or vertex widths, default-time
transforms, and instance masks. It includes all instancers in a shared volume,
then stores root/tip optical depth as four float3 **instance** primvars. The
viewport geometry stays instanced. Prototype `hairT` interpolates those values.
Rebake after moving/deforming/replacing instances; rotating lights and cameras
works without rebaking. Nested/mesh prototypes and animated bakes are not handled.

From an environment configured like `bin/launch_usdview.ps1`:

```powershell
.\bin\build_usdgen.ps1
python tools/fur_gen.py 200000 build/fur_200k.usda
.\build\usdGenBakeFur.exe build/fur_200k.usda build/fur_200k_shadowed.usda 48
.\bin\launch_usdview.ps1 build/fur_200k_shadowed.usda
```

Select High or Very High complexity. `inputs:selfOcclusion = 0.5` applies the
baked density; 0 disables it, 1 doubles extinction. Tune root/tip colors, lobe
widths/gains, and transmission through the existing material inputs. The bake
CLI clamps grid resolution to 8–128; 48 is the procedural path's default.

The generator itself needs only Python. The bake executable needs the USD/core
DLLs on PATH (the standard project launcher environment). A typical Windows
prefix here is `D:/work/usdRig/usd-install`.

## Verification and measured limits

`testUsdGenFurOcclusion` tests cross-tile occlusion, light-direction asymmetry,
width response, transform changes, occluder removal, no-op COW reuse, finite
depth, malformed geometry rejection, dirty notices, and typed Hydra transport.
`checkC5.py` validates all three shader definitions and defaults through Sdr.

`usdGenShaders/test/validate_fur.py` renders all three material variants, checks
that shadows darken actual pixels, and verifies direct-light relighting. It
times 60 steady frames after 10 warmups with `glFinish`, excluding image export
and the bake. Example:

```powershell
python usdGenShaders/test/validate_fur.py build/fur_200k_shadowed.usda build/fur_validation_200k --usdrecord D:/work/usdRig/usd-install/bin/usdrecord --width 1600
```

On the RTX 4090 in this workspace, the 200,000-instance / 1.6-million-CV scene at
1600x1200 measured 6.27 ms median with shadows and 5.95 ms with extinction disabled.
The shadowed p95 was 16.17 ms (desktop/scheduling variability); these are synchronized
CPU wall times, not isolated GPU timer queries. Baking and exporting the native
instanced scene took 1.61 seconds. The separate 50,000-strand / 100,000-CV core bake
test measured 21.25 ms. These workloads are different; neither is a guarantee for
an arbitrary animated groom. JSON and rendered evidence are under
`build/fur_validation_200k`.

The focused eight-test suite and Sdr checks passed. The broader T1 run completed
83 passes, three platform skips, and a session-store exit callback-count failure;
that test passed when rerun alone. The parallel shutdown failure is not resolved
or hidden by the fur changes.

## What remains below a full engine hair renderer

Six-axis depth is a low-frequency angular approximation: it can leak or
over-occlude at oblique directions. Point-light depth is integrated beyond the
light, so nearby lights are approximate. Voxelization uses control polygons,
not exactly tessellated splines; nonuniform transforms use conservative maximum
width scaling. Receiver-cell density includes some strand self-contribution.
Native instances interpolate only root/tip depth, which loses interior detail
on long or tightly curved hair. The bake does not account for material opacity.

There is no new GPU density update pass, deep opacity shadow atlas, temporal
reconstruction, shadow LOD, or scene-wide mesh/hair mutual shadow pipeline.
Stock host shadow support is still needed for fur shadows on the scalp/other
objects and mesh shadows on fur. CPU bake costs are unsuitable for large animated
grooms every frame. Metal/Vulkan are untested, and the variant-A tangent still
depends on Storm's GL curve-patch layout.

Unreal's groom pipeline uses density voxelization and optional dedicated deep
shadow maps, with view-dependent voxel sizing and specialized visibility and
composition passes. Achieving or exceeding that full feature/quality envelope
requires renderer integration and matched scenes/hardware benchmarks, beyond
the stock material/scene-index path implemented here.

References: [Epic groom pipeline and performance](https://dev.epicgames.com/documentation/unreal-engine/groom-scalability-and-performance-with-unreal-engine),
[Pixar volumetric hair methods](https://graphics.pixar.com/library/Hair/paper.pdf).
