# Instanced fur in MoonRay (hdMoonray)

usdGen's instanced curves render in MoonRay's Hydra delegate: a
PointInstancer fur scene (`tools/fur_gen.py`, optionally baked with
`usdGenBakeFur`) translates through `hd_usd2rdl` to an `RdlCurveGeometry`
prototype plus an `RdlInstancerGeometry` carrying positions, orientations,
scales and the per-instance primvars (`displayColor`, `hairId`, baked
`furTau*`), and `moonray` renders it. One minimal MoonRay-side patch is
applied (below); the usdGen-side changes are additive and leave Storm
rendering bit-identical.

## What changed

**Generator fallback material.** The generated `FurLook` material used to
carry only `outputs:glslfx:surface`, which hdMoonray cannot see, so every
strand fell back to MoonRay's error material. It now also carries a
universal `outputs:surface` with a `UsdPreviewSurface` whose diffuseColor
reads the per-instance `displayColor` through a `UsdPrimvarReader_float3`
(fallback: the coat's mean brown). Storm prefers its `glslfx` render
context over the universal one, so its shading is unchanged; hdMoonray
(and any other delegate) takes the universal network. A MoonRay-native
hair terminal (`HairMaterial_v3`) is deliberately not authored:
UsdImaging drops material nodes whose `info:id` has no Sdr definition,
and MoonRay's Sdr plugins are not on the translation path, so that
terminal would resolve to no material at all. Pinned by
`tests/checks/check_fur_gen_material.py` (`testUsdGenFurGenMaterial`).

**Imaging instancer wire format.** `UsdGenInstancer::BuildInstancerDataSource`
now publishes USD's own PointInstancer encoding: `hydra:instanceRotations`
as `VtQuathArray` (hdMoonray accepts only quath or legacy vec4f there; a
`VtQuatfArray` mis-syncs every instance), float varyings of arity 3/2
packed as `VtVec3f/2fArray` (hdMoonray ignores `elementSize`, so flat
float arrays would be misread as one scalar per instance), and
`displayColor` with the `color` role. `Bake` keeps full float precision;
only the data source rounds. Storm accepts both encodings. Pinned by
`tests/testUsdGenInstance.cpp` §(10).

`usdGenBakeFur` needed no change: the baked `varying` float3 primvars
already translate to instancer `UserData`.

**Synthetic tile materials.** Procedural groom tiles used to bind a
glslfx-only synthetic material (`material_storm`), which made
hdMoonray look for an RDL DSO named `UsdGenHairStrands` (or
`UsdGenValuePreview` / `UsdGenScalpShadow`) and fail. The tile
publisher now emits both networks for every synthetic material: the
same glslfx node and parameters under the `glslfx` context Storm
prefers, plus a universal `UsdPreviewSurface` whose diffuseColor
reads the baked per-curve `displayColor` through a
`UsdPrimvarReader_float3` (fallback: the look's root color, mid-gray
for the value preview). Pinned by `tests/testUsdGenValuePreview.cpp`.

**Renderer-specific scalp-shadow publication.** The shadow cap is a Storm
approximation, not a physical surface. The groom scene-index plugin reads
Hydra's renderer display name and publishes that cap only to Storm (`GL` /
`Storm`). Other delegates receive neither its mesh nor its material or
notices. Strand publications and the shared CPU session remain unchanged.
Publishing the cap to MoonRay exposed the emitter cage through the subdivided
skin and added false shadows and polygon bands. Transparency also produced
artifacts in this build, so the fix excludes the geometry at publication.
Ray tracers shade and shadow the actual strands. Covered by
`tests/testUsdGenScenePublication.cpp` (cap present for raster, absent from
ray-tracing enumeration/GetPrim/notices, strands retained).

**Texture inputs.** This MoonRay build rejects untiled PNGs in its texture
cache, leaving connected surface inputs black. Supply tiled, mipmapped
EXRs (including dome maps), for example with OpenImageIO `maketx --oiio
-d half --format openexr`. Preserve the source colour space; linear atlas
values should use `sourceColorSpace = raw` in `UsdUVTexture`.

**MoonRay patch (one hunk).** `BasisCurves.cc` rejects any wrap
other than `nonperiodic`, but its tessellation ignores wrap
entirely, so `pinned` (USD's default, and the correct authoring for
fur in Storm) renders exactly like `nonperiodic`. The patch accepts
`pinned`; only `periodic` (closed curves) still errors. Unpatched
delegates log the error but emit the same geometry.

## Rendering

From a shell with the MoonRay build and the USD prefix on `PATH`:

```powershell
$env:PATH = "D:\work\moonray\build-windows\bin;D:\work\usdRig\usd-install\lib;D:\work\usdRig\usd-install\bin;" + $env:PATH
python tools/fur_gen.py 200000 build/fur_200k.usda
.\build\usdGenBakeFur.exe build/fur_200k.usda build/fur_200k_shadowed.usda
hd_usd2rdl -in build/fur_200k.usda -out build/fur_200k.rdla
moonray -in build/fur_200k.rdla -out build/fur_200k.exr
```

## usdview

`bin/launch_usdview.ps1` wires the MoonRay build's Hydra delegate into
the viewer: with a `../moonray/build-windows` tree beside the repo
(override with `MOONRAY_BUILD`, as `USD` overrides the OpenUSD prefix)
it registers the build's `hd_moonray`/`hd_moonray_debug` plugInfos ahead
of the prefix (the prefix ships the same plugin names with `LibraryPath`s
that resolve to nothing; first registration wins), puts the build's `bin/`
ahead of the prefix on `PATH` (the only shared DLL names are the identical
TBB 2020.3 pair and Embree, where the build's 4.4.1 over the prefix's 4.3.3
is the safe direction), and sets `ARRAS_SESSION_PATH` (session definitions)
and `RDL2_DSO_PATH` (the staged `rdl2dso` set) for interactive rendering.
The caller's values win for all three variables. The adapters and Sdr
plugInfos are deliberately not registered: their `LibraryPath`s point at
DLLs that exist only in the build's `bin/`. Without a MoonRay build the
launcher behaves exactly as before. Covered by
`tests/checks/check_usdview_env.py` (`testUsdGenUsdviewEnv`); `-PrintEnv`
dumps the assembled environment as JSON without needing GL.

The launcher enables OpenUSD `--allow-async` only for its Storm (`GL`) path.
Select a MoonRay renderer explicitly (for this Windows build, **Moonray
(debug)**) and it keeps the existing complete-operation publication behavior:

```powershell
.\bin\launch_usdview.ps1 --renderer HdMoonrayRendererDebugPlugin examples\felt\felt_sphere.usda
```

Windows verification, 2026-09-24: the in-process **Moonray (debug)** delegate
(`HdMoonrayRendererDebugPlugin`) renders the live puppet groom to convergence
through usdview. The ARRAS-backed `HdMoonrayRendererPlugin` instead reports
that local sessions are unsupported by this client. Choose the in-process
delegate for this build. The earlier blanket statement that interactive
MoonRay rendering was broken is superseded by this verification.

## Scene-authored lookdev settings

The local hdMoonray build now consumes the active `UsdRenderSettings` prim
selected by the stage's `renderSettingsPrimPath`. Both plugins register the
render-settings filtering and dependency-forwarding scene indices before
Hydra constructs its scene-index chain. Namespaced attributes such as
`moonray:sceneVariable:sampling_mode = "adaptive"` override the host settings;
`disableMotionBlur` is also honored. Use enum names rather than numeric RDL
values, because the generic value converter interprets numbers as enum ordinals.

Changes apply live. Deactivating or removing the settings prim restores host
settings, and removed SceneVariables overrides reset to their native defaults.
This does not implement offline render-product export: viewport framing and
resolution remain controlled by the host. Restart usdview to load rebuilt DLLs.

`examples/felt/check_groom.py` checks the actual renderer values, the exported
SceneVariables, live edits and removal using **Moonray (debug)**. The felt sphere
uses a 32–128 sample adaptive lookdev preset with restrained secondary sampling.

## Known limits

Material binding policy is renderer-specific. Storm suppresses bindings at
Low complexity (and its synthetic default at Medium) because its wire repr
cannot compile the hair shader. MoonRay and other delegates retain authored
and universal fallback materials at every complexity. Previously the Storm
workaround also affected MoonRay, turning the felt grey after switching from
Storm at usdview's default Low setting. `testUsdGenViewportOverlay` covers
both policies at levels 0–3, for synthetic and authored materials.

* The `pinned` patch above lives in the MoonRay working tree
  (`moonray/hydra/hdMoonray/lib/hydramoonray/BasisCurves.cc`);
  re-apply it after syncing MoonRay until it lands upstream.
* MoonRay tessellates `bspline` unclamped (spans over each sliding
  4-CV window), while Storm clamps `pinned` ends. Strand roots/tips sit
  slightly inside their control points in MoonRay (about 14% of the
  strand length on the generator's 8-CV strand). One static file cannot
  serve two basis evaluations; Storm is the shape reference.
* The baked `furTau*` occlusion is inert under the preview material: it
  reaches MoonRay as instancer `UserData`, but only the Storm glslfx
  shader consumes it.
* Procedural groom tiles cannot be file-translated yet: the usdGen
  scene index does not engage under `hd_usd2rdl`'s delegate path
  (the groom prim is skipped), so tiles reach MoonRay only through
  an interactive Hydra session, using the in-process delegate on this
  Windows build. The tile material networks are pinned at the data-source
  level by the material tests.
