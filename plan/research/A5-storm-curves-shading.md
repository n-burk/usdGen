# A5 — Storm (HdSt) in OpenUSD 26.08: basis-curves rendering, materials on curves, custom glslfx, textures/Ptex, image formats, picking, performance

Scope: what Storm can do for viewport hair/fur in the *installed* OpenUSD 26.08 at `/home/burkard/work/OpenUSD_26_08`, with evidence from the source tree `/home/burkard/work/OpenUSD` (tag v26.08). All paths below are absolute unless prefixed with `pxr/`, which means `/home/burkard/work/OpenUSD/pxr/`. "UNVERIFIED" marks claims I could not confirm by reading code or running anything.

---

## 0. Environment facts that constrain the plan (from the install's CMake cache)

File: `/home/burkard/work/OpenUSD_26_08/build/OpenUSD/CMakeCache.txt`

| Flag | Value | Consequence |
|---|---|---|
| `PXR_ENABLE_PTEX_SUPPORT` | `OFF` | `HdStIsSupportedPtexTexture()` always returns false (`pxr/imaging/hdSt/ptexTextureObject.cpp:46-54`); `HdStPtexTextureObject::_Load()` body is compiled out (`ptexTextureObject.cpp:111-229`, guard at :121). No Ptex library on the machine. |
| `PXR_BUILD_OPENIMAGEIO_PLUGIN` | `OFF` | No `hioOiio`: no TIFF/TX/DPX/PSD/etc. reading. |
| `PXR_ENABLE_MATERIALX_SUPPORT` | `ON` | `HdSt_ApplyMaterialXFilter` is compiled in (`pxr/imaging/hdSt/materialNetwork.cpp:1173-1179`); MaterialX 1.39.5 libs + `libraries/` present in the install. |
| `PXR_ENABLE_GL_SUPPORT` | `ON`; `PXR_ENABLE_VULKAN_SUPPORT`/`METAL` `OFF` | GL tessellation path (TCS/TES) is the one that runs; the Metal post-tess (PTCS/PTVS) mixins are disabled at runtime (`pxr/imaging/hdSt/basisCurvesShaderKey.cpp:471-479`). |
| `PXR_ENABLE_OPENVDB_SUPPORT` | `OFF` | irrelevant for hair. |
| `PXR_BUILD_EXEC` | `ON`, `PXR_BUILD_USDVIEW` `ON`, `PXR_PYTHON_INSTALL_DIR=lib/python3.12/site-packages` | consistent with the task brief. |

Installed plugins: `/home/burkard/work/OpenUSD_26_08/plugin/usd/{hdStorm, hioAvif, hioOpenEXR, sdrGlslfx, usdShaders}` (directory listing). Symbols `HdStPtexSamplerObject*` exist in `lib/libusd_hdSt.so` (nm), but they are the stub build; no `libPtex*` and no OpenImageIO in `lib/`.

OpenSubdiv 3.6.1 in the install has `include/opensubdiv/far/ptexIndices.h` (Far::PtexIndices is core OSD and does not need the Ptex library), and `libosdCPU/libosdGPU` — nothing Ptex-file related. So the plan cannot promise `.ptx` reading without adding the wdas/ptex library **and** rebuilding OpenUSD with `PXR_ENABLE_PTEX_SUPPORT=ON` (or writing our own reader; see §6).

---

## 1. Authoritative capability table — hair in Storm 26.08

| Capability | Status | Evidence |
|---|---|---|
| Rprim type `basisCurves` supported by Storm | Yes | `pxr/imaging/hdSt/renderDelegate.cpp:73` (in `_SupportedRprimTypes`), `:420` (creates `HdStBasisCurves`). |
| Curve types | `linear`, `cubic` only; anything else is coerced to linear with a warning | `pxr/imaging/hd/basisCurvesTopology.cpp:149-158`. |
| Cubic bases in the shader | `bezier`, `bspline`, `catmullRom`, `centripetalCatmullRom`; unknown basis → warning + `Curves.LinearBasis` mixin | `pxr/imaging/hdSt/basisCurvesShaderKey.cpp:142-153`; basis mixins `pxr/imaging/hdSt/shaders/basisCurves.glslfx:1026-1209`. |
| Wrap modes | `nonperiodic`, `periodic`, `pinned` (pinned only for vStep==1 i.e. bspline/catmullRom), `segmented` (linear only, GL_LINES pairs) | index builders `pxr/imaging/hdSt/basisCurvesComputations.cpp:49-103` (segmented), `:106-184` (line strips; catmullRom skips first/last seg unless pinned, :112-117), `:187-393` (cubic; pinned :261-307). |
| Per-prim draw styles | POINTS, WIRE (lines / isolines), RIBBON (camera-facing or oriented flat strip), HALFTUBE (tessellated half tube) | enum `pxr/imaging/hdSt/basisCurvesShaderKey.h:41-46`; selection `pxr/imaging/hdSt/basisCurves.cpp:303-350`. |
| Normal styles | ORIENTED (authored `normals`), HAIR (camera-facing, faceted fragment normal), ROUND (fake tube normal) | `basisCurvesShaderKey.h:48-52`; fragment mixins `basisCurves.glslfx:1315-1375`. |
| Refinement control | `HdDisplayStyle::refineLevel` (int 0..8) from `displayStyle/refineLevel`; 0 = lines, 1 = ribbon+HAIR, 2 = ribbon+ROUND, ≥3 = halftube+ROUND (or ribbon+ORIENTED whenever `normals` exist and level>0). Env `HD_ENABLE_REFINED_CURVES=1` forces patch geomStyle everywhere. | `basisCurves.cpp:322-343`, `:1312-1320`; `pxr/imaging/hd/basisCurves.cpp:18-19,52-55,60-70`; `pxr/imaging/hd/sceneDelegate.h:64-69`. |
| Repr → geomStyle | `hull`, `smoothHull`, `wireOnSurf`, `solidWireOnSurf`, `refined`, `refinedWireOnSurf`, `refinedSolidWireOnSurf` → Patch; `wire`, `refinedWire` → Wire; `points` → Points | `pxr/imaging/hd/renderIndex.cpp:1048-1067`. |
| Primvars consumed by the geometry shader | `points` (vertex, required), `widths` (vertex or varying; constant-size-1 also accepted), `normals` (vertex or varying), `displayColor`, `displayOpacity` (any interpolation incl. uniform per curve), plus Storm-builtins `pointSizeScale`, `screenSpaceWidths` (bool), `minScreenSpaceWidths` (float) | `basisCurves.cpp:1357-1387`; `pxr/imaging/hd/basisCurves.cpp:30-34`; `basisCurves.glslfx:309-329, 770-791, 1230-1235, 1283-1288`. |
| Vertex/varying primvar value types | half, float, vec2f..vec4f, double, vec2d..vec4d, int, vec2i..vec4i, int16/32, uint16/32; others fall back to raw `HdVtBufferSource` with a warning | `basisCurves.cpp:783-849`. |
| Uniform (per-curve) primvars | Supported; array length must equal number of curves or the primvar is dropped with a validation warning. Fragment shader reaches them via `GetElementID()` = `primitiveParam[segment]` = curve index | `basisCurves.cpp:1169-1267` (:1208-1214); `pxr/imaging/hdSt/codeGen.cpp:5564-5573`; `basisCurvesComputations.cpp:41-45`. |
| Screen-space / minimum pixel widths | Supported (`screenSpaceWidths`, `minScreenSpaceWidths` primvars, also for `points`) | `basisCurves.glslfx:309-329`, `:770-791`; `pxr/usdImaging/usdImaging/basisCurvesAdapter.cpp:32-36,472-473`. |
| Native instancing (point instancer prototypes) | Yes — curve VS/TES include the `Instancing.Transform` mixin and `HdStUpdateInstancerData` runs per draw item | `basisCurvesShaderKey.cpp:202,284,319,367,395,435`; `basisCurves.cpp:183-199`. |
| GPU skinning (deferred skinning) of curves | Yes — `Vertex.SkinPoints` mixin is always in the curve VS; usdSkelImaging's points-resolving SI handles `basisCurves` | `basisCurvesShaderKey.cpp:203`; `pxr/imaging/hdSt/shaders/skinning.glslfx:14-25` (requires `skel_geomBindTransform`, `hydra_influences`, `hydra_skinningXforms`…); `pxr/usdImaging/usdSkelImaging/pointsResolvingSceneIndex.cpp:33`. |
| ExtComputation (GPU compute) supplying `points` | Yes — computed primvars are consulted for vertex primvars and in the points fast path | `basisCurves.cpp:882-896, 949-970`. |
| Topological visibility (hide individual curves/points without rebuilding buffers) | Supported by HdSt (`invisibleCurves`/`invisiblePoints` in `HdBasisCurvesTopology`) but the Hydra-2 adapter builds the topology without them | `basisCurves.cpp:649-668`; `pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:865-925` (constructor call has no invisible sets). |
| Materials on curves | Same `HdStMaterial` path as meshes: the material's `surfaceShader()` is called from `Curves.Fragment.Patch` / `Curves.Fragment.Wire` through `ShadingTerminal()` | `basisCurves.glslfx:1246,1294`; `pxr/imaging/hdSt/shaders/terminals.glslfx:51-124`. |
| Lighting on HAIR normals | Lighting is whatever the material's `surfaceShader` computes from `Neye`. For HAIR the FS normal is `cross(dFdx(P), dFdy(P))` (faceted, screen-derivative). ROUND/HALFTUBE give a smooth fake-tube normal; ORIENTED uses authored normals. | `basisCurves.glslfx:1365-1375, 1317-1348, 1353-1362`. |
| Kajiya-Kay / Marschner / hair BSDF built into Storm | **None.** `grep -i hair|kajiya|marschner` over `hdSt/shaders/*.glslfx`, `hdSt/*.cpp`, `usdShaders/shaders/*.glslfx`, `hdMtlx` finds nothing besides the basisCurves "HAIR" normal style. | grep result (no hits). |
| Unlit curves | `displayStyle/shadingStyle = constantLighting` switches the terminal to `Fragment.SurfaceUnlit` (`integrateLightsConstant`) | `basisCurves.cpp:365-372`; `pxr/imaging/hdSt/tokens.h:37`; `terminals.glslfx` `Fragment.SurfaceUnlit`. |
| Custom glslfx surface shader (per-material) | Yes via Sdr "glslfx" source type: `info:implementationSource = sourceAsset` + `info:glslfx:sourceAsset = @file.glslfx@` or `info:glslfx:sourceCode` | `pxr/usdImaging/plugin/sdrGlslfx/parserPlugin.cpp:29-31,174-295`; `pxr/usd/usdShade/shaderDefUtils.cpp:46-119`; `materialNetwork.cpp:120-164`; tests `pxr/usdImaging/usdImagingGL/testenv/testUsdImagingGLSdr/object.usda:30-52`, `usdGlslfxEmbedded.usda:33-34`. |
| Custom shader reading primvars | Yes: `attributes` block in the glslfx → Sdr `primvars` metadata → `HdGet_<name>()` in FS; also any primvar named via a `UsdPrimvarReader_*` node; primvar filtering keeps only declared ones (`HDST_ENABLE_MATERIAL_PRIMVAR_FILTERING`, default true) | `parserPlugin.cpp:262-275`; `materialNetwork.cpp:1126-1136, 366-395`; `pxr/imaging/hdSt/materialNetworkShader.cpp:30-35`; `pxr/imaging/hdSt/primUtils.cpp:120-139,161-207`. |
| Custom shader sampling UV textures | Yes: `textures` block → `HdGet_<tex>(vec2 st)` / `HdGet_<tex>()` (uses the primvar wired to `st`/`uv`) | `materialNetwork.cpp:650-870`; `codeGen.cpp:4013-4110, 4271-4290`. |
| UDIM | Yes (`<UDIM>` in path) | `materialNetwork.cpp:754-755`. |
| Ptex textures | Compiled-in code path exists (texture type, binder, GLSL lookup), but disabled in this build, and its GLSL accessor needs mesh-only `GetPatchCoord()` | `pxr/imaging/hdSt/enums.h:32-39`; `textureObjectRegistry.cpp:63-64`; `textureBinder.cpp:69,203-204,525-526`; `codeGen.cpp:6617-6660` (uses `GetPatchCoord(localIndex)`), which is only defined in `pxr/imaging/hdSt/shaders/mesh.glslfx:73,145,227,...` and not in `basisCurves.glslfx`. |
| MaterialX materials on curves | The same fragment-shader path; Storm accepts UsdPreviewSurface/standard_surface/open_pbr_surface/gltf_pbr families plus any nodedef whose single output is `surfaceshader` | `pxr/imaging/hdSt/materialXFilter.cpp:800-860`. MaterialX 1.39.5 ships `chiang_hair_bsdf` GLSL (`/home/burkard/work/OpenUSD_26_08/libraries/pbrlib/genglsl/pbrlib_genglsl_impl.mtlx:28-29`), but whether Storm's shadergen handles a hair BSDF terminal is UNVERIFIED (no test, no code path referencing it). |
| Picking curves (prim/instance/element=curve index) | Yes; `elementId` AOV carries the curve index | `pxr/imaging/hdx/pickTask.cpp:205-215, 1042-1044`; `codeGen.cpp:5564-5573`. |
| Picking individual CVs (`pointId`) | Only when the prim is drawn with the **points** repr (POINTS draw style); `pointId = hd_VertexID - baseVertexOffset` | `basisCurvesShaderKey.cpp:208-220, 504-511`; `pxr/imaging/hdSt/shaders/pointId.glslfx` (`PointId.Vertex.PointParam`). |
| Point selection highlight on curves | `HdSelection::AddPoints`, rendered by the points repr (`Selection.Vertex.PointSel` mixin) | `pxr/imaging/hd/selection.h:70-78`; `basisCurvesShaderKey.cpp:212-216`. |

---

## 2. How Storm draws a basisCurves prim (the mechanics the plugin must target)

### 2.1 Sync → draw item → geometric shader

`HdStBasisCurves::Sync` (`basisCurves.cpp:74-130`) runs `_UpdateRepr` → `_UpdateDrawItem` (`:160-255`), in this order: material network shader (on NewRepr or any dirty primvar, `:172-176`), instancer data (`:183-199`), constant primvars/transform/extent (`:202-218`), topology (`:223-230`), vertex primvars (`:233-236`), varying + element primvars (`:237-244`). Then, only if `DirtyDisplayStyle | DirtyMaterialId | DirtyTopology | NewRepr`, it rebuilds the geometric shader for all reprs (`:99-105, 118-121`).

Geometric-shader selection (`basisCurves.cpp:275-415`):

```cpp
TfToken curveType = _topology->GetCurveType();          // :290
bool supportsRefinement = _SupportsRefinement(_refineLevel); // refineLevel>0 || HD_ENABLE_REFINED_CURVES (:1312-1320)
if (!supportsRefinement) { curveType = HdTokens->linear; curveBasis = TfToken(); } // :293-301 cubic drawn as polyline through CVs
...
case HdBasisCurvesGeomStylePatch:                         // :320
  if (_SupportsRefinement(_refineLevel) && _SupportsUserWidths(drawItem)) {   // widths BAR resource must exist (:1323-1325)
    if (_SupportsUserNormals(drawItem)) { RIBBON, ORIENTED }                  // :324-327
    else if (_refineLevel > 2)          { HALFTUBE, ROUND }                   // :329-332
    else if (_refineLevel > 1)          { RIBBON, ROUND }                     // :333-336
    else                                { RIBBON, HAIR }                      // :337-340
  }
  // else stays WIRE/HAIR (default at :303-306)
```

Consequences:
- **No `widths` primvar ⇒ wire only**, regardless of refine level (`:322-323`).
- **Authored `normals` ⇒ ORIENTED ribbons** at any level ≥1; there is no way to get HALFTUBE while normals exist.
- Changing the geometric shader marks *all* draw batches dirty for deep validation (`:403-414`) — avoid flipping refineLevel/normals per frame.

GL primitive types (`pxr/imaging/hdSt/geometricShader.cpp:296-335, 188-241`): linear WIRE → `LineList` with 2 indices/segment; linear RIBBON/HALFTUBE → `PatchList` of 2 CVs; cubic anything (even WIRE) → `PatchList` of 4 CVs with tessellation (`basisCurvesShaderKey.cpp:177-181`: "cubic curves get drawn via isolines in a tessellation shader even in wire mode").

### 2.2 Tessellation levels (fixed constants, not tunable from the scene)

`basisCurves.glslfx:280-300`: `GetMaxTess()=40`, `GetPixelToTessRatio()=20`. Length-wise level for a cubic segment is `clamp(screenLengthOfControlPolygon/20, 0, 40)` (`:528-543`, wire `:465-480`). HALFTUBE width-wise level is `clamp(2*screenWidth/20, 1, 40)` evaluated at u=0,.33,.66,1 (`:548-603`). Linear RIBBON uses tess factors 1 (`:377-380`); linear HALFTUBE derives width-wise factors from screen width (`:387-411`). So a cubic hair segment spanning 800 px costs 40 quad rows; a 2 px-wide hair costs 1 column (ribbon) — HALFTUBE only adds columns when the hair is ≥10 px wide on screen.

### 2.3 Index buffers and topology sharing

- Cubic: one `GfVec4i` per segment (`basisCurvesComputations.cpp:32-35`); segments per curve = `(count-4)/vStep + 1` (nonperiodic) or `max(count/vStep,1)` (periodic) (`:325-327`); curves with `count < 2` are skipped (`:312-317`); for non-pinned, vertices beyond the end repeat the last CV (`:341-343`). Bezier `vStep=3`, others `vStep=1` (`:254-259`).
- `pinned` adds duplicated phantom segments instead of expanding primvars (`:261-307`).
- Linear/unrefined: `GfVec2i` per segment (`:36-39`); CatmullRom/centripetal drop first and last segments unless `pinned` (`:112-117`).
- A `primitiveParam` int buffer (curve index per segment) is always built (`:41-45, 419-423`) and is what `GetElementID()` returns.
- Topology objects and index ranges are **shared by hash** (`basisCurves.cpp:671-689, 717-719`: `RegisterBasisCurvesTopology` / `RegisterBasisCurvesIndexRange` keyed on `HdBasisCurvesTopology::ComputeHash()` which hashes basis, type, wrap, `curveVertexCounts`, `curveIndices` (`pxr/imaging/hd/basisCurvesTopology.cpp:189-206`) plus the `refined` bool). Many prims with identical `curveVertexCounts` share one index buffer.

### 2.4 Primvar interpolation rules on curves

| Interpolation | Expected size | Storm handling |
|---|---|---|
| `vertex` | `Σ curveVertexCounts` (or `1+max(curveIndices)` if indexed) | Used as is; size 1 broadcast; wrong size → fallback value (1 / (1,0,0) …) + warning (`basisCurvesComputations.h:200-234`). |
| `varying` | linear: same as vertex; cubic: `Σ(numSegs+1)` nonperiodic or `Σ numSegs` periodic (`pxr/imaging/hd/basisCurvesTopology.cpp:228-274`) | Expanded CPU-side to per-vertex by `HdSt_ExpandVarying` (`basisCurvesComputations.h:72-158`); **periodic expansion not implemented** (warning, `:82-87`). Stored in a separate *varying* BAR (`basisCurves.cpp:1146-1149`). |
| `uniform` | number of curves | element BAR; FS reads by curve index (`basisCurves.cpp:1183-1216`). |
| `constant` | 1 | constant BAR (`primUtils.cpp` `HdStPopulateConstantPrimvars`, called at `basisCurves.cpp:205-213`). |
| `faceVarying` | n/a | not consumed by curves. |

In the GL tessellation-evaluation stage, **vertex** primvars are blended with the cubic basis weights (`codeGen.cpp:5981-5985`: `out = basis[0]*in[i0] + … + basis[3]*in[i3]`), while **varying** primvars go through `InterpolatePrimvar(...)` (`codeGen.cpp:~6070-6075`), which for cubic curves is `mix(inPv2, inPv1, u)` — linear between the two interior CVs of the segment (`basisCurves.glslfx:1001-1023`) — and for linear curves is the basis blend (`:962-996`). `widths` and `normals` have their own dedicated evaluators: `Curves.Cubic.Widths.Basis` (basis blend of 4 CV widths) vs `.Linear` (`mix(w[2], w[1], u)`), chosen by whether `widths` was found among *varying* primvars (`basisCurves.cpp:1083-1100`, `basisCurvesShaderKey.cpp:234-242`, `basisCurves.glslfx:930-957`); same for normals (`:899-927`).

Note the shader authors' own caveats: `u` runs backwards in several places and `mix(end, start, u)` is used deliberately (`basisCurves.glslfx:20-36`); the `v` direction is inconsistent between representations; curve `patchCoord` passed to the material is `vec4(0, v, 0, 0)` (`:1293`) or `vec4(0,0.5,0,0)` for wire (`:1244`).

### 2.5 Dirty bits and buffer rebuild costs

Hydra-2 locator → dirty bits for `basisCurves` (`pxr/imaging/hd/dirtyBitsTranslator.cpp`):
- `basisCurves/topology` (any child) → `DirtyTopology` (`:638-644`).
- `primvars/points` → `DirtyPoints`; `primvars/normals` → `DirtyNormals`; `primvars/widths` → `DirtyWidths`; any other `primvars/<x>` → `DirtyPrimvar`; the bare `primvars` prefix → `DirtyPrimvar | DirtyPoints | DirtyNormals | DirtyWidths` (`:822-847`).
- `displayStyle/refineLevel` (and the other displayStyle children) → `DirtyDisplayStyle`; `displayStyle/reprSelector` → `DirtyRepr` (`:702-756`).
- `materialBindings` → `DirtyMaterialId` (`:889-890`); `xform` → `DirtyTransform`; `visibility` → `DirtyVisibility`.

What each costs in `HdStBasisCurves`:
- **Points-only change** (`DirtyPoints` without `DirtyPrimvar/Normals/Widths`): the fast path (`basisCurves.cpp:930-998`) reads only `primvars/points` from the terminal scene index, creates one `HdSt_BasisCurvesPrimvarInterpolaterComputation` (a copy into a `VtArray`, `basisCurvesComputations.h:184-280`), and `UpdateNonUniformBufferArrayRange` keeps the existing BAR if the element count is unchanged (`:1025-1028`; `HdStCanSkipBARAllocationOrUpdate`, `primUtils.cpp:395-413`). `HdStResourceRegistry::Commit` then resolves sources in parallel (`resourceRegistry.cpp:862-907`, TBB), resizes ranges whose element count changed (`:913-925`), reallocates buffer arrays only if needed (`:961-973`), and copies through a staging buffer (`:984-1000`). The topology, index buffers and geometric shader are untouched, and draw batches stay valid.
- **`DirtyPrimvar`** re-pulls *all* vertex primvar descriptors and values (`:875-928`) — so publish per-curve colours etc. with their own locator (`primvars/<name>`) and avoid dirtying the whole `primvars` container each frame.
- **Topology change** (`curveVertexCounts` etc.) rebuilds `HdBasisCurvesTopology` (hash over the whole counts array), the index buffer (CPU loop over all segments), the `primitiveParam` buffer, marks the topology range `SizeVarying` (`:735-742`), and, because `_PropagateDirtyBits` adds `DirtyPrimvar` (`:417-428`), re-uploads every primvar. If the vertex count changes, the vertex BAR is resized and the whole striped VBO that aggregates this prim with others is reallocated and its unchanged ranges are blit-copied (`pxr/imaging/hdSt/vboMemoryManager.cpp:272-425`, perf counter `vboRelocated` `:279`).
- **Geometric-shader change** (refineLevel, material id, topology) → `HdStMarkDrawBatchesDirty` → all batches deep-validated (`basisCurves.cpp:403-414`).

---

## 3. Recommended output representation for fast viewport hair

Recommendation (with the reasons from §2):

| Choice | Recommendation | Why (evidence) |
|---|---|---|
| `type` | `cubic` for the final look; `linear` for interactive/edit mode | Cubic costs a 4-CV patch + TCS/TES per segment even in wire mode (`basisCurvesShaderKey.cpp:177-181`); linear WIRE is a plain `LineList` (`geometricShader.cpp:303-304`). At refineLevel 0 cubic is *drawn as linear polyline through CVs anyway* (`basisCurves.cpp:293-301`). |
| `basis` | `catmullRom` (interpolating; groom CVs lie on the curve) or `bspline`; use `wrap = pinned` so the curve reaches the end CVs without duplicating primvars | Pinned support: `basisCurvesComputations.cpp:261-307`; catmullRom otherwise drops the first/last segment (`:112-117`, `:325-327`). Avoid `bezier` (vStep 3 → CV count must be 3n+1; varying expansion rules differ, `basisCurvesComputations.h:115-149`). |
| `wrap` | `pinned` (or `nonperiodic` with 2 duplicated end CVs) | as above; `periodic` has no varying-primvar expansion (`basisCurvesComputations.h:82-87`). |
| `widths` | **Required** for anything but wire (`basisCurves.cpp:322-323`). Use `vertex` interpolation (basis-evaluated width, `basisCurves.glslfx:932-942`) or a single constant (`authoredSize == 1`, `basisCurvesComputations.h:205-208`) | `varying` widths force `Curves.Cubic.Widths.Linear` (`basisCurves.cpp:1095-1096`), i.e. linear in each segment — fine, but vertex is cheaper (no CPU expansion). |
| `normals` | **Omit** for hair (get HAIR/ROUND camera-facing styles); provide only for flat ribbons like feathers/leaves | `basisCurves.cpp:324-327`. |
| `refineLevel` | Ask for **2** (RIBBON+ROUND: single quad column, smooth fake-tube normal, `basisCurves.glslfx:1329-1348`) for hair; **3** (HALFTUBE) only when the strands are thick on screen (width-wise tess only adds columns when >10 px, `:548-603`). usdview's "complexity" presets map Low/Medium/High/Very High → refineLevel 0/1/2/3 (`pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43`, `pxr/usdImaging/usdImagingGL/engine.cpp:2318-2351`). | See §10 for how to set it per prim from a scene index. |
| `minScreenSpaceWidths` | Author `1.0` (constant float primvar) so sub-pixel hair does not alias/vanish | `basisCurves.glslfx:781-789`; primvar name `basisCurves.cpp:1382`. |
| Per-curve colour | `uniform` `displayColor` (one vec3 per curve) — read directly by the fragment mixins (`basisCurves.glslfx:1283-1285`) with no interpolation cost | `basisCurves.cpp:1169-1267`; `codeGen.cpp:5564-5573`. |
| Root-to-tip variation | `vertex` primvars (basis-blended in TES, `codeGen.cpp:5981-5985`) or a `vertex` float `st`-like parameter | |

Cost intuition (per frame, N curves × S segments each):
- Wire linear: N·S line segments, one VS per CV, no tessellation.
- Cubic ribbon L2: N·S patches; TES invocations ≈ N·S·(tessLevel+1)·2 with tessLevel = clamp(pixels/20, 0, 40); fragment cost ∝ covered pixels.
- Cubic halftube L3: as ribbon ×(width columns+1), only for strands ≥10 px wide.
- The HAIR normal (`cross(dFdx, dFdy)`, `basisCurves.glslfx:1372-1375`) is faceted along the strip; ROUND is smooth and is the better default for "looks rendered".

---

## 4. Getting realistic hair shading

### 4a. UsdPreviewSurface + per-curve `displayColor`

Mechanism: bind a `UsdPreviewSurface` material (or none — then Storm's fallback shader is used, `pxr/imaging/hdSt/material.cpp:222-232`). In the curve FS the base colour starts from `displayColor`/`displayOpacity` (`basisCurves.glslfx:1282-1288`) and is passed as `color` into `ShadingTerminal → surfaceShader(Peye, Neye, color, patchCoord)` (`terminals.glslfx:51-52, 93-100`). The preview surface ignores `color` unless `diffuseColor` is wired to a primvar reader; so to get *per-curve colour with a preview surface* connect `inputs:diffuseColor` to a `UsdPrimvarReader_float3` with `varname = displayColor` (or any name) — the primvar-redirect param becomes `HdGet_diffuseColor()` (`materialNetwork.cpp:366-395`; `codeGen.cpp:2242-2263` for `HD_HAS_` on redirects). Lighting is the preview surface's GGX/Lambert on `Neye` (`pxr/usd/plugin/usdShaders/shaders/previewSurface.glslfx:119-133, 384-420`). With ROUND normals this gives a plausible "tube" look; it is not anisotropic hair shading.

Limits: no anisotropy, no tangent-based specular, no root/tip darkening except via primvars; opacity via `displayOpacity` primvar requires the material tag machinery (`_displayOpacityFromPrimvars`, `basisCurves.cpp:924-926`, `:1119-1121`).

### 4b. Custom glslfx surface shader shipped by the plugin (recommended)

This is the path that yields Kajiya-Kay/Marschner-lite in Storm without patching USD.

**Discovery/registration.** A `UsdShade` `Shader` prim with
```
uniform token info:implementationSource = "sourceAsset"
uniform asset  info:glslfx:sourceAsset   = @usdGenHair.glslfx@   # or info:glslfx:sourceCode = """..."""
```
is discovered by `UsdShadeShaderDefUtils::GetDiscoveryResults` (`pxr/usd/usdShade/shaderDefUtils.cpp:46-119`, source type = the middle token of `info:<type>:sourceAsset`, `:102`) and parsed by `SdrGlslfxParserPlugin` (discovery type `"glslfx"`, `parserPlugin.cpp:29-31`; plugin registered in `pxr/usdImaging/plugin/sdrGlslfx/plugInfo.json`; installed at `/home/burkard/work/OpenUSD_26_08/plugin/usd/sdrGlslfx`). Storm looks the terminal node up as `GetShaderNodeByIdentifierAndType(nodeTypeId, "glslfx")` (`materialNetwork.cpp:131-132`) and loads the file (`:135-152`) or the embedded source (`:155-160`). Storm's declared shader source types are `glslfx` and `mtlx` (`renderDelegate.cpp:700-706`). Examples: `pxr/usdImaging/usdImagingGL/testenv/testUsdImagingGLSdr/object.usda:30-52` (asset) and `usdGlslfxEmbedded.usda:33-34` (source code).

**What the glslfx must contain** (`pxr/imaging/hio/glslfxConfig.cpp` keys: `techniques` :251, `parameters` :377, `textures` :520, `attributes` :590, `source` :313, `default`/`defVal` :454, `documentation` :465, `role` :479):
```
-- glslfx version 0.1
-- configuration
{
  "techniques": { "default": { "surfaceShader": { "source": ["UsdGenHair.Surface"] } } },
  "parameters": { "rootColor": {"default":[0.1,0.05,0.02]}, "tipColor": {...}, "specShift": {"default":0.1}, ... },
  "textures":   { "colorMap": { "documentation": "..." } },
  "attributes": { "st": {"type":"vec2"}, "hairV": {"type":"float"}, "displayColor": {"type":"vec3"} },
  "metadata":   { "materialTag": "defaultMaterialTag" }
}
-- glsl UsdGenHair.Surface
vec4 surfaceShader(vec4 Peye, vec3 Neye, vec4 color, vec4 patchCoord) { ... }
```
(shape taken from `testUsdImagingGLSdr/surface_texture.glslfx:1-64` and `previewSurface.glslfx:12-29`). `parameters` become Sdr inputs (float/int/color/matrix/float[2..4]; `parserPlugin.cpp:46-172, 204-228`), `textures` become colour inputs (`:230-260`), and `attributes` become the node's `primvars` metadata (`:262-275`), which is what survives primvar filtering (`materialNetwork.cpp:1126-1136`). The `metadata.materialTag` is the strongest opinion for the draw-batch material tag (`materialNetwork.cpp:70`).

**What `surfaceShader()` receives and can call** (from `terminals.glslfx:51-124`, `basisCurves.glslfx:1276-1312`):
- `Peye` eye-space position, `Neye` = the curve normal (HAIR: faceted screen-derivative; ROUND: fake tube normal), `color` = `displayColor/displayOpacity` (or grey), `patchCoord = vec4(0, v, 0, 0)` where `v∈[0,1]` is across the width. There is **no tangent** parameter; but `inData.Neye` in the FS is the *tangent* for HALFTUBE/ROUND/implicit ribbons (`basisCurves.glslfx:867, 889`, comment `:1272-1274`). A custom shader can read the interpolated block member `inData.Neye` directly — it is in scope in the fragment stage (`Curves.Fragment.Patch` layout `:1258-1267`) — to get the strand tangent for Kajiya-Kay. UNVERIFIED that Storm's glslfx composer allows a material mixin to reference `inData` (it is a plain `in block`, so GLSL scoping should permit it; a compile test is needed).
- Alternative tangent source without relying on `inData`: `dFdx/dFdy(Peye)` give the strip plane; or the plugin can emit a `vertex` `vec3 tangent` primvar and declare it in `attributes` (basis-blended in TES, `codeGen.cpp:5981-5985`).
- Lights: `GetLightCount()`, `GetLightSource(i)` (`LightSource` has `position`, `diffuse`, `specular`, `spotDirection`, `attenuation`, `isIndirectLight`, shadow indices; `pxr/imaging/glf/shaders/simpleLighting.glslfx:102-115, 161-175`), and the standard `integrateLightsDefault(Peye, Neye, LightingInterfaceProperties)` (`:236, :384`). Dome light IBL helpers used by preview surface: `HdGet_domeLightIrradiance`, `HdGet_domeLightPrefilter`, `HdGet_domeLightBRDF` (`previewSurface.glslfx:347-370`). Note `NUM_LIGHTS` may be 0 (`simpleLighting.glslfx:145, 236`).
- Selection/colour overrides are applied by the terminal (`terminals.glslfx:82-84, 117-121`); exposure at `:108`.
- Primvars: `HdGet_<name>()` for every declared attribute; uniform ones resolve via `GetElementID()` (curve index), vertex/varying ones via the interpolated in-block (`codeGen.cpp:3552-3558` accessors; `HD_HAS_<name>` defines `:2114-2263`).
- Textures: `HdGet_<tex>()` samples using the primvar wired to the node's `st` input (`materialNetwork.cpp:800-870`; `codeGen.cpp:4271-4290`), or `HdGet_<tex>(vec2 uv)` with explicit coordinates (`codeGen.cpp:4108-4111`), and `HdGetSampler_<tex>()` (`:3903-3945`).

**Limits.** One surface terminal, no node graphs beyond primvar readers / UsdUVTexture / transform2d feeding the terminal (`materialNetwork.cpp:1078-1086`: "It cannot convert arbitrary material networks to Storm"). Shader compiles are keyed by source text; changing the *source* rebatches everything that uses the material (`material.cpp:253-260`); changing *parameter values* only updates the material's shader BAR. Doubles are converted to float (`parserPlugin.cpp:152-155`). Bool → int (`:165-168`). Vec3 defaults are typed as `Color` (`:131`).

### 4c. MaterialX

`HdSt_ApplyMaterialXFilter` runs on every non-volume terminal when MaterialX is enabled (`materialNetwork.cpp:1173-1179`). Recognised families: `UsdPreviewSurface`, `standard_surface`, `open_pbr_surface`, `gltf_pbr`; otherwise any nodedef with exactly one `surfaceshader` output (`materialXFilter.cpp:800-860`). The installed MaterialX 1.39.5 has a GLSL implementation of `chiang_hair_bsdf` (`/home/burkard/work/OpenUSD_26_08/libraries/pbrlib/genglsl/pbrlib_genglsl_impl.mtlx:28-29`) and the melanin helpers (`:80-83`). Whether a `surface` node driven by `chiang_hair_bsdf` compiles and lights correctly through Storm's `HdStMaterialXShaderGen` (which supplies its own lighting integration and `Neye`, not a curve tangent) is UNVERIFIED — there is no hair test in `pxr/imaging/hdSt/testenv`, and Storm's MaterialX shader gen has no tangent input from curves. Treat MaterialX hair as a stretch goal; the custom glslfx (4b) is the dependable route. For non-hair-BSDF looks (e.g. `standard_surface` with anisotropy), MaterialX on curves works exactly like on meshes because the terminal path is identical (`basisCurves.glslfx:1294`).

---

## 5. Getting colour maps to the shader — three routes

| Route | What the plugin emits | Storm side | Pros / cons |
|---|---|---|---|
| **A. Bake on CPU into primvars** (recommended first) | `uniform` `displayColor` (per curve, sampled at the root's surface (u,v) / face) and optionally `vertex` colour for root→tip gradients | Read via `HdGet_displayColor()` in the fragment mixins (`basisCurves.glslfx:1283-1285`) or by any material via a primvar reader. Uniform primvar length must equal `numCurves` (`basisCurves.cpp:1208-1214`). | Renderer-agnostic (works in every Hydra delegate and in exports), no texture plumbing, SeExpr/Ptex/any format evaluated by the plugin with its own loaders. Cost: one vec3 per curve; re-upload on change through `primvars/displayColor` locator → `DirtyPrimvar` (`dirtyBitsTranslator.cpp:843`) which re-pulls all vertex primvars (§2.5). |
| **B. Texture sampled in the shader with root (u,v)** | `uniform` (or `vertex`) `float2 st` primvar per curve = root surface UV; material = custom glslfx (or UsdPreviewSurface) with a `UsdUVTexture` whose `st` input is a `UsdPrimvarReader_float2(varname="st")` | `_MakeMaterialParamsForTexture` finds the primvar connected to `st`/`uv` and adds it as an additional primvar (`materialNetwork.cpp:800-845`); accessor `HdGet_<tex>()` uses `HdGet_st().xy` (`codeGen.cpp:4271-4290`). Formats limited to Hio (§6). UDIM supported (`materialNetwork.cpp:754-755`). | Colour changes need no re-upload; texture memory shared across all curves; batches split by texture (`commandBuffer.cpp:157-176`; `HDST_DRAW_BATCH_TEXTURE_AGGREGATION_THRESHOLD=10`, `:62`). Cons: needs a UV layout on the scalp; no Ptex; the colour is Storm-only (other renderers need the same material). |
| **C. Ptex in the shader** | would need `faceIndex`-style primvars + a Ptex texture node | Storm's Ptex pipeline: texture type `HdStTextureType::Ptex` (`enums.h:36`), picked when the Sdr node has `isPtex` metadata (`materialNetwork.cpp:683-686`; the only such node is the deprecated `HwPtexTexture_1` in `pxr/usd/usdHydra/shaders/shaderDefs.usda:3-8`), object created at `textureObjectRegistry.cpp:63-64`, bound as texel `sampler2DArray` + layout `usampler1DArray` (`textureBinder.cpp:203-204`), GLSL lookups `PtexLookup/PtexMipmapLookup(vec4 patchCoord, …)` where `faceID = int(patchCoord.w)` (`pxr/imaging/hdSt/shaders/ptexTexture.glslfx:113-123, 167-175`). The generated accessor calls `GetPatchCoord(localIndex)` (`codeGen.cpp:6617-6660`), which is defined only in `mesh.glslfx` (`:73,145,227,581,684,774,867,957`) — **not** in `basisCurves.glslfx`. | **Not viable in this install** (`PXR_ENABLE_PTEX_SUPPORT=OFF`, `HdStIsSupportedPtexTexture` returns false `ptexTextureObject.cpp:46-54`) and, even when enabled, Ptex sampling on curves would need a `GetPatchCoord` definition for curves or the `HdGet_<tex>(vec4 patchCoord)` overload (`codeGen.cpp:6645-6660`) called from a custom shader with a per-curve `faceIndex` primvar — a plugin-side workaround that is UNVERIFIED to compile. |

Conclusion for the plan: implement **A** as the baseline (the plugin already has to evaluate maps/Ptex/SeExpr on the CPU for the groom operators), and **B** as the "textured hair" material path for Storm using the plugin's glslfx. Ptex stays CPU-only (§6).

---

## 6. Image loading: what the plugin can read via Hio in this install

Registered `HioImage` plugins (plugInfo `imageTypes`):
- Built-in stb: `bmp, jpg, jpeg, png, tga, hdr` (`pxr/imaging/hio/plugInfo.json:8`; installed `/home/burkard/work/OpenUSD_26_08/lib/usd/hio/resources/plugInfo.json:8`).
- `hioOpenEXR`: `exr` (`/home/burkard/work/OpenUSD_26_08/plugin/usd/hioOpenEXR/resources/plugInfo.json:8`).
- `hioAvif`: `avif` (`/home/burkard/work/OpenUSD_26_08/plugin/usd/hioAvif/resources/plugInfo.json:8`).
- **Not available**: `tif/tiff`, `tx`, `dpx`, `psd`, `gif`, `webp`, `.ptx/.ptex` (no `hioOiio`, `PXR_BUILD_OPENIMAGEIO_PLUGIN=OFF`).

So Storm textures and the plugin's own CPU samplers (if implemented with `HioImage::OpenForReading`) get PNG/JPEG/TGA/BMP/HDR/EXR/AVIF. For scalp colour maps this is enough (PNG/EXR).

Ptex options for the plugin:
1. Build wdas/ptex (BSD) and link it **into the plugin only** (CPU sampling of `.ptx` per curve root using the scalp mesh's face index + face (u,v)) — independent of Storm. This is the route that matches "colour via primvars" (§5A).
2. Rebuild OpenUSD with `PXR_ENABLE_PTEX_SUPPORT=ON` (`ptexTextureObject.cpp:21-24, 91-108, 121`) to get Storm-side Ptex on *meshes*; still no help for curves (§5C).
3. Convert Ptex → per-face UV atlas or bake to primvars offline.
OpenSubdiv in the install carries `Far::PtexIndices` (`include/opensubdiv/far/ptexIndices.h`) which gives the Ptex face numbering of a subdiv mesh without the Ptex library — useful for option 1 to map scalp faces to Ptex faces consistently.

UsdUVTexture cannot reference a `.ptx`: with Ptex disabled the file would go through `HdStTextureType::Uv` and fail in Hio (no plugin for the extension); with Ptex enabled the type is still Uv unless the Sdr node carries `isPtex` (`materialNetwork.cpp:683-686`), and `UsdUVTexture`'s definition has only `role = texture` (`pxr/usd/plugin/usdShaders/shaders/shaderDefs.usda`, `sdrMetadata` of `UsdUVTexture`).

---

## 7. Performance notes

**Per-frame points update for N curves (deforming hair on a rigged scalp).**
- Best case is exactly `DirtyPoints` only (dirty the `primvars/points` locator, nothing else; `dirtyBitsTranslator.cpp:837-838`). Cost: `HdSceneIndexAdapterSceneDelegate` pulls the value, `HdStBasisCurves::_PopulateVertexPrimvars` fast path (`basisCurves.cpp:930-998`) copies the array into an interpolater computation (one `VtArray<GfVec3f>` copy per prim, `basisCurvesComputations.h:200-204`), `Commit` resolves in parallel (`resourceRegistry.cpp:871`), stages, and uploads (`:984-1000`). No index rebuild, no batch invalidation.
- Keep the **vertex count constant** to avoid range resize + VBO reallocation and blit (`vboMemoryManager.cpp:272-425`). If curve counts must change interactively, prefer changing *which* curves are visible (topological visibility — see §1 caveat) or padding, over changing `curveVertexCounts`.
- Avoid dirtying `primvars` wholesale: `DirtyPrimvar` re-pulls every vertex/varying/uniform primvar (`basisCurves.cpp:875-928, 1094-1122, 1192-1221`).
- GPU alternative: publish `points` as an **ExtComputation** output (compute-shader deformation driven by the scalp's deformed points) — supported for curves via `HdSt_GetExtComputationPrimvarsComputations` (`basisCurves.cpp:888-896, 949-970`), or use Storm's deferred-skinning mixin (`skinning.glslfx`) when the deformation is skeletal. This removes the per-frame CPU array copy/upload entirely, but it is Storm-specific.

**Topology changes.** Rebuild `HdBasisCurvesTopology` (hash over counts), CPU index build (`basisCurvesComputations.cpp:187-393`), `primitiveParam` rebuild, `DirtyPrimvar` propagation (`basisCurves.cpp:421-424`), and — because `DirtyTopology` also triggers `updateGeometricShader` (`:99-105`) — `HdSt_GeometricShader::Create` lookup and a batch deep-validation (`:403-414`). Do this at edit-commit time, not per frame.

**Draw item batching.** Storm batches draw items whose key `hash(geometricShader hash, bufferArraysHash [, textureSourceHash])` matches (`commandBuffer.cpp:157-176`) and `_IsAggregated` passes: same geometric shader object, same instancer level count, and every BAR (topology, vertex, varying, element, constant, instance index, instance primvars) aggregated in the same buffer array (`drawBatch.cpp:220-262`); plus material compatibility (`_CanAggregateMaterials`, `:180-204`, by material shader hash) and texture hash (`:215-216`). `GetBufferArraysHash` is built from BAR *versions* (`drawItem.cpp` `GetBufferArraysHash`), so any BAR reallocation changes the key and forces a batch rebuild. Practical implications:
- All hair prims should share the same curve type/basis/wrap/refineLevel/normal presence and the same material (or same glslfx + same texture set) to end up in one indirect batch.
- Buffer aggregation is keyed by `bufferSpecs + usageHint` (`vboMemoryManager.cpp:63-70`); prims with the same primvar set (names + types) aggregate; a prim with an extra primvar lands in a different buffer array → separate batch.
- Curves do **not** use the shared-vertex-primvar dedup that meshes have (`HdStIsEnabledSharedVertexPrimvar` is used in `pxr/imaging/hdSt/mesh.cpp:1817` and not referenced anywhere in `basisCurves.cpp`), but index buffers *are* shared by topology hash (§2.3).
- GPU frustum culling operates per draw item (`HD_ENABLE_GPU_FRUSTUM_CULLING`, `pxr/imaging/hdSt/indirectDrawBatch.cpp:66-70`; tiny-prim culling `HD_ENABLE_GPU_TINY_PRIM_CULLING`, `renderDelegate.cpp:54`), and is only effective if the extent is sane — publish a correct `extent` for each hair prim.

**One big prim vs many prims.** One prim per groom description (hundreds of thousands of curves) gives: one draw item, one BAR set, one topology hash, minimal per-prim Sync overhead (the Sync loop is per prim), one culling unit (no per-clump culling), and a single `points` upload of size `Σ counts`. Many prims (e.g. per clump or per scalp face) give finer culling and cheaper partial updates (only dirty prims re-upload) at the cost of per-prim Sync/BAR bookkeeping and batch fragmentation whenever the primvar sets differ. Recommendation: one prim per groom *layer/description* (like XGen descriptions), optionally split into a few spatial chunks (dozens, not thousands) when the scalp is large; keep all chunks identical in primvar layout. Instancing (`HdInstancer`) of a few prototype curves is available (§1) but is a poor fit for unique hair; use it only for LOD/preview cards.

**Tessellation budget.** With `GetMaxTess()=40` per segment, a fully-refined cubic hair of 8 segments at full-screen length can emit 320 TES rows; at typical screen sizes the level is `pixels/20`. Interactive tools should lower `refineLevel` (2→1 or 0) while dragging: this is a `DirtyDisplayStyle` (geometric shader swap) — cheap in data but forces batch validation once per change.

**Shader compile.** The geometric shader is created through `HdSt_GeometricShader::Create(shaderKey, registry)` (`basisCurves.cpp:398-399`) and cached by key; the first frame for each (type, basis, drawStyle, normalStyle, widths/normals interpolation, terminal) combination compiles GLSL. Keep the number of distinct combinations small.

---

## 8. Picking curves and CVs (comb/groom tool)

`HdxPickTask` renders id AOVs `primId, instanceId, elementId, edgeId, pointId, Neye, depth` (`pickTask.cpp:205-215`) into an offscreen pass whose collection (and thus repr) is provided by the caller (`pickTask.cpp:165-167` comment; `HdxPickTaskContextParams::collection`). Hits are decoded into `HdxPickHit{objectId, instancerId, instanceIndex, elementIndex, edgeIndex, pointIndex, worldSpaceHitPoint, worldSpaceHitNormal, normalizedDepth}` (`pickTask.h:82-110`; `pickTask.cpp` `_ResolveHit`).

Curve specifics:
- `elementIndex` = **curve index** (`GetElementID()` returns `primitiveParam[segment]`, `codeGen.cpp:5564-5573`), for both wire and patch draw styles. `edgeIndex` is -1 for curves (no edge id mixin in the curve FS; `basisCurvesShaderKey.cpp:483-557` has no edge mixin).
- `pointIndex` is produced **only by the POINTS draw style** (points repr): the `PointId.Vertex.PointParam` mixin is included only when `isPrimTypePoints` (`basisCurvesShaderKey.cpp:208-220`; FS `:504-511`), and `GetPointId() = hd_VertexID - GetBaseVertexOffset()` = index into the prim's `points` array (`pointId.glslfx`, `PointId.Vertex.PointParam`). Points are rasterised as `gl_PointSize` from `GetPointRasterSize` × `pointSizeScale` (`basisCurves.glslfx:188-196`); GL "round points" capability via `HGIGL_ENABLE_NATIVE_ROUND_POINTS` (`pxr/imaging/hgiGL/capabilities.cpp:169-171, 200-201`).
- Pick targets: `pickPrimsAndInstances` (default), `pickFaces`, `pickEdges`, `pickPoints`, `pickPointsAndInstances` (`pickTask.h:46-55`, default `:276`); `_IsValidHit` for `pickPoints` requires `pointId != -1` (`pickTask.cpp:1374-1410`).
- Resolve modes: `resolveNearestToCamera` (min depth over the pick rect, `:1413-1450`), `resolveNearestToCenter`, `resolveUnique` (hash of prim/instance/element/edge/point ids, `:1521-...`, `:1361-1368`), `resolveAll` (every pixel), `resolveDeep` (`pickTask.h:37-41`).
- `UsdImagingGLEngine::TestIntersection(PickParams{resolveMode}, view, proj, root, params)` exposes **only** `resolveMode` (`pxr/usdImaging/usdImagingGL/engine.h:348-372`); it never sets `pickTarget` (0 occurrences in `engine.cpp`) and uses `_intersectCollection`, whose repr follows the render params' draw mode (`engine.cpp:1238, 1248`; `:2407-2430`: `DRAW_POINTS` → `points` repr, else `smoothHull`/`refined` etc.). usdview's stageView calls it with `resolveNearestToCenter` (`pxr/usdImaging/usdviewq/stageView.py:2263-2269`).

Comb-tool implications:
1. Curve-level picking (select a strand/clump) works out of the box with the engine API: `elementIndex` is the curve index, `worldSpaceHitPoint` gives the hit position, `worldSpaceHitNormal` comes from the `Neye` AOV.
2. CV-level picking needs a pick pass drawn with the **points repr** and `pickTarget = pickPoints`. Options: (a) call `TestIntersection` while temporarily setting `renderParams.drawMode = DRAW_POINTS` (the intersect collection then uses the `points` repr, `engine.cpp:2411-2412`), then read `hit.pointIndex` — but `pickTarget` stays `pickPrimsAndInstances`, so `pointId` is populated only where a point pixel was hit and `resolveNearestToCenter` may return a non-point hit first; (b) run an app-level `HdxPickTask` with explicit `HdxPickTaskContextParams{pickTarget=pickPoints, resolveMode, collection(points repr), resolution}` through the engine's task controller scene index (`engine.cpp:1243-1260` shows the pattern: set `HdxPickTokens->pickParams` in the task context and execute `GetPickingTaskPaths()`); (c) a CPU-side ray/frustum vs CV test in the plugin using its own curve data (simplest, renderer-independent, and needed anyway for brush falloff). Recommend (c) for the brush and (a)/(b) only for click selection; UNVERIFIED that the engine's Python bindings expose enough of (b).
3. Selection highlighting of CVs uses `HdSelection::AddPoints` (`pxr/imaging/hd/selection.h:70-78`) and is rendered only in the points repr (`Selection.Vertex.PointSel` mixin, `basisCurvesShaderKey.cpp:212-216`), so a comb tool should draw a **separate "guide CV" points prim** (or switch the guide prim to the points repr via `displayStyle/reprSelector`) while editing.

---

## 9. Storm limitations for widths/normals interpolation on cubic curves

1. Varying widths/normals are linear per segment between the two interior CVs (`Curves.Cubic.Widths.Linear`, `basisCurves.glslfx:952-957`; normals `:921-927`), with an explicit hack clamping `u` to `[1e-3, 1-1e-3]` to hide orientation flips for oriented bezier curves (`:924-926`, also linear `:811`).
2. Periodic curves: varying primvars are expanded as if non-periodic (`basisCurvesComputations.h:82-87`, "XXX(HYD-2238)").
3. Bezier varying expansion assumes `nVerts = 3n+1`; wrong counts hit `TF_VERIFY` (`:148-149`).
4. Vertex-interpolated widths on a **catmullRom/bspline** curve are basis-blended (`:932-942`) — the width at a CV is therefore *not* the authored value (B-spline smoothing); for exact per-CV widths use `varying` (linear) or pre-compensate.
5. Oriented ribbons compute `Neye` per vertex and `orient()` uses `cross(tangent, normal)`; the implicit ribbon uses `tangent × (0,0,1)` (camera normal) (`:880-895`), so ribbons degenerate when the tangent is parallel to the view direction (no special handling; the tangent fallback only handles zero-length tangents, `:854-860`).
6. HAIR normal is faceted (`:1367-1375`, "results in faceted shading"); the polarity of the interpolated `Neye` is unstable ("instability in the cross-product in the TessEval shader").
7. The HALFTUBE tube normal is built from `cross(position, tangent)` (view-dependent, `:866-875, 1317-1324`), fine for hair but not a true geometric normal.
8. `u`/`v` conventions differ from RenderMan and are internally inconsistent (`:20-36`), so material code that depends on `patchCoord.y` (= `v` across the width) should treat it as unsigned distance from the centre `abs(v-0.5)*2`.
9. `HdStBasisCurves` builds no normals of its own; without `widths` you get wire only (§2.1).
10. Uniform primvars with the wrong length are silently dropped (warning) (`basisCurves.cpp:1208-1214`); vertex/varying with the wrong length are replaced by a fallback constant (`basisCurvesComputations.h:222-234, 260-272`) — the hair will render with width 1.0 or colour (1,0,0), which is an easy-to-miss authoring bug; the plugin's publisher must assert sizes.
11. `type`/`basis` sanity: `cubic` + `basis=linear` is downgraded to linear (`hd/basisCurvesTopology.cpp:154-158`); default basis in Hydra 2 when unspecified is `bezier` (`sceneIndexAdapterSceneDelegate.cpp:~885`), default type `linear`, default wrap `nonperiodic` — always author all three.

---

## 10. Hydra-2 plumbing the plugin's scene index must produce

Prim type `basisCurves` with data sources:
- `basisCurves/topology/{curveVertexCounts, curveIndices?, basis, type, wrap}` (`pxr/imaging/hd/basisCurvesSchema.h:36-38`, `basisCurvesTopologySchema.h:35-41`); read by `HdSceneIndexAdapterSceneDelegate::GetBasisCurvesTopology` (`sceneIndexAdapterSceneDelegate.cpp:865-925`).
- `primvars/<name>/{primvarValue | indexedPrimvarValue+indices, interpolation, role}` with interpolation tokens `constant|uniform|varying|vertex|faceVarying|instance` (`pxr/imaging/hd/primvarSchema.h:36-49`). UsdImaging maps the USD attributes `points`, `normals`, `widths`, `velocities`, `accelerations` into these (`pxr/usdImaging/usdImaging/dataSourceGprim.cpp:33-59`), deriving interpolation from the attribute's `interpolation` metadata (`dataSourcePrimvars.cpp:32-38, 240-243`). A plugin scene index should emit the same shapes directly.
- `displayStyle/{refineLevel, flatShadingEnabled, displacementEnabled, displayInOverlay, occludedSelectionShowsThrough, pointsShadingEnabled, materialIsFinal, shadingStyle, reprSelector, cullStyle}` (`pxr/imaging/hd/legacyDisplayStyleSchema.h:35-46`). **Per-prim refineLevel is honoured**: `GetDisplayStyle` reads `displayStyle/refineLevel` from the prim's data source (`sceneIndexAdapterSceneDelegate.cpp:3018-3066`), and `usdImagingGL`'s `HdsiLegacyDisplayStyleOverrideSceneIndex` only supplies an *underlay* (`HdOverlayContainerDataSource::New(prim.dataSource, _underlayDs)` in `GetPrim`, `pxr/imaging/hdsi/legacyDisplayStyleOverrideSceneIndex.cpp`; fallback value set from the complexity slider in `engine.cpp:2354-2363`). So the hair scene index can pin its own `refineLevel` (e.g. 2) independent of usdview's complexity setting, and can flip `shadingStyle=constantLighting` for guides. `reprSelector` per prim (e.g. `points` for editable guides) maps to `DirtyRepr` (`dirtyBitsTranslator.cpp:~750-752`).
- `materialBindings` for the material (`dirtyBitsTranslator.cpp:889-890`), `xform`, `visibility`, `extent`, `purpose`.

Invalidation: send `HdSceneIndexObserver::DirtiedPrimEntries` with the narrowest locator (`primvars/points` for deformation; `primvars/displayColor` for colour; `basisCurves/topology` + all primvars for regeneration; `displayStyle/refineLevel` for LOD).

Environment knobs worth documenting for users/tests: `HD_ENABLE_REFINED_CURVES=1` (always patch geomStyle; `hd/basisCurves.cpp:18-19,60-70`), `HDST_ENABLE_MATERIAL_PRIMVAR_FILTERING` (`materialNetworkShader.cpp:30`), `HD_ENABLE_GPU_FRUSTUM_CULLING`, `HD_ENABLE_GPU_TINY_PRIM_CULLING` (`renderDelegate.cpp:54`), `HDST_DRAW_BATCH_TEXTURE_AGGREGATION_THRESHOLD` (`commandBuffer.cpp:62`), `HD_MAX_VBO_SIZE` (1 GiB, `vboMemoryManager.cpp:38`), `HDST_ENABLE_DRAW_ITEMS_CACHE` (`renderPass.cpp:30`), `HGIGL_ENABLE_NATIVE_ROUND_POINTS` (`hgiGL/capabilities.cpp:169-171`), debug flags `HD_RPRIM_UPDATED` (per-prim shader-key log, `basisCurves.cpp:352-359, 394-396`) and `HDST_DRAW_BATCH` (`commandBuffer.cpp:402, 462`).

---

## Key facts

- Storm renders `basisCurves` with four draw styles (POINTS/WIRE/RIBBON/HALFTUBE) and three normal styles (ORIENTED/HAIR/ROUND); the choice is driven only by `displayStyle/refineLevel`, presence of `widths`, and presence of `normals` — `pxr/imaging/hdSt/basisCurves.cpp:303-350`, `basisCurvesShaderKey.h:41-52`.
- Without a `widths` primvar a curve is always drawn as wire; with `normals` it is always an oriented ribbon; refineLevel 0 draws cubic curves as polylines through the CVs — `basisCurves.cpp:292-301, 322-343`.
- refineLevel mapping: 1→ribbon/HAIR, 2→ribbon/ROUND, ≥3→halftube/ROUND; usdview complexity Low/Medium/High/VeryHigh = refineLevel 0/1/2/3 — `basisCurves.cpp:329-340`, `pxr/usdImaging/usdAppUtils/complexityArgs.py:40-43`, `pxr/usdImaging/usdImagingGL/engine.cpp:2318-2351`.
- Per-prim `displayStyle/refineLevel` (and `shadingStyle`, `reprSelector`) from a scene index wins over usdview's global fallback because the override scene index is an underlay — `pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:3018-3066`, `pxr/imaging/hdsi/legacyDisplayStyleOverrideSceneIndex.cpp` (`GetPrim`, `HdOverlayContainerDataSource::New(prim.dataSource, _underlayDs)`).
- Tessellation constants are hard-coded: max 40 levels per segment, 20 px per tess level; halftube width columns only above ~10 px on-screen width — `pxr/imaging/hdSt/shaders/basisCurves.glslfx:280-300, 528-603`.
- Cubic index buffers are 4-int patches per segment; `bezier` vStep 3, others vStep 1; `pinned` implemented by duplicating end CVs in the index buffer; catmullRom drops end segments unless pinned — `pxr/imaging/hdSt/basisCurvesComputations.cpp:32-35, 254-259, 261-307, 112-117`.
- Varying primvars on cubic curves are CPU-expanded to per-vertex and interpolated linearly between interior CVs in the TES; periodic expansion is unimplemented — `pxr/imaging/hdSt/basisCurvesComputations.h:72-158`, `basisCurves.glslfx:1001-1023`, `pxr/imaging/hdSt/codeGen.cpp:5981-5985, ~6070`.
- Uniform (per-curve) primvars work in the fragment shader via `GetElementID()` = `primitiveParam[segment]`; length must equal the curve count — `codeGen.cpp:5564-5573`, `basisCurves.cpp:1208-1214`.
- A `DirtyPoints`-only update takes a fast path that touches nothing but the points buffer source; `DirtyTopology` propagates to `DirtyPrimvar` and rebuilds indices and geometric shader, invalidating all batches — `basisCurves.cpp:930-998, 417-428, 403-414`.
- Hydra-2 locators map: `primvars/points`→DirtyPoints, `primvars/widths`→DirtyWidths, `primvars/normals`→DirtyNormals, other `primvars/*`→DirtyPrimvar, `basisCurves/topology`→DirtyTopology, `displayStyle/*`→DirtyDisplayStyle/DirtyRepr — `pxr/imaging/hd/dirtyBitsTranslator.cpp:638-644, 702-756, 822-847`.
- Draw batching key = geometric shader hash + BAR versions (+ texture hash); prims batch only with identical shaders, primvar layouts and aggregated buffers — `pxr/imaging/hdSt/commandBuffer.cpp:157-176`, `drawBatch.cpp:220-262`, `drawItem.cpp` (`GetBufferArraysHash`).
- Curves share index buffers by topology hash but do not use mesh-style shared vertex primvars — `basisCurves.cpp:671-689, 717-719`; `HdStIsEnabledSharedVertexPrimvar` only in `pxr/imaging/hdSt/mesh.cpp:1817`.
- Storm has no hair BSDF; lighting of curves is whatever the bound material's `surfaceShader(Peye, Neye, color, patchCoord)` computes; curve `patchCoord` is `vec4(0, v, 0, 0)` — grep (no hits), `terminals.glslfx:51-52`, `basisCurves.glslfx:1293-1294`.
- Custom glslfx surface shaders are first-class: `info:implementationSource=sourceAsset` + `info:glslfx:sourceAsset` (or `sourceCode`) → `SdrGlslfxParserPlugin` → Storm loads by `GetShaderNodeByIdentifierAndType(id, "glslfx")`; `attributes`/`textures`/`parameters` in the glslfx configuration become primvars/textures/inputs — `pxr/usd/usdShade/shaderDefUtils.cpp:46-119`, `pxr/usdImaging/plugin/sdrGlslfx/parserPlugin.cpp:174-295`, `pxr/imaging/hdSt/materialNetwork.cpp:120-164, 1126-1136`, `testUsdImagingGLSdr/object.usda:30-52`.
- The custom shader has access to `GetLightCount()/GetLightSource(i)` and dome-light IBL accessors (as `previewSurface.glslfx` does), and can obtain the strand tangent from the FS `inData.Neye` block for HALFTUBE/ROUND/implicit ribbons (declared as the tangent in `orient()`) — `pxr/imaging/glf/shaders/simpleLighting.glslfx:102-175`, `previewSurface.glslfx:347-370, 384-420`, `basisCurves.glslfx:866-895, 1258-1274`.
- UV textures for curves need a `st` primvar wired through `UsdPrimvarReader_float2`; UDIM is supported — `materialNetwork.cpp:754-755, 800-870`; `codeGen.cpp:4271-4290`.
- Ptex is disabled in this install (`PXR_ENABLE_PTEX_SUPPORT:BOOL=OFF`, `HdStIsSupportedPtexTexture` returns false), only the deprecated `HwPtexTexture_1` carries `isPtex`, and the generated Ptex accessor requires mesh-only `GetPatchCoord()` — `/home/burkard/work/OpenUSD_26_08/build/OpenUSD/CMakeCache.txt`, `pxr/imaging/hdSt/ptexTextureObject.cpp:46-54`, `pxr/usd/usdHydra/shaders/shaderDefs.usda:3-8`, `codeGen.cpp:6617-6660`, `mesh.glslfx:73`.
- Hio can read bmp/jpg/jpeg/png/tga/hdr (stb), exr (hioOpenEXR), avif (hioAvif); no OIIO, so no tif/tx/psd/dpx and no .ptx — `pxr/imaging/hio/plugInfo.json:8`, `/home/burkard/work/OpenUSD_26_08/plugin/usd/hioOpenEXR/resources/plugInfo.json:8`, `.../hioAvif/resources/plugInfo.json:8`, CMakeCache `PXR_BUILD_OPENIMAGEIO_PLUGIN=OFF`.
- MaterialX is enabled and Storm accepts any nodedef with a single `surfaceshader` output; MaterialX 1.39.5 ships `chiang_hair_bsdf` GLSL, but Storm support for a hair BSDF terminal is unverified — `pxr/imaging/hdSt/materialXFilter.cpp:800-860`, `/home/burkard/work/OpenUSD_26_08/libraries/pbrlib/genglsl/pbrlib_genglsl_impl.mtlx:28-29`.
- Picking returns the curve index as `elementIndex` for any repr; CV `pointIndex` only in the points repr; `UsdImagingGLEngine::TestIntersection` exposes only `resolveMode` (not `pickTarget`) and uses the render draw-mode's repr — `pickTask.cpp:205-215, 1374-1410`, `basisCurvesShaderKey.cpp:208-220`, `engine.h:348-372`, `engine.cpp:1238-1260, 2407-2430`.
- Storm builtins for curves: `pointSizeScale`, `screenSpaceWidths`, `minScreenSpaceWidths` (author `minScreenSpaceWidths=1` to keep thin hair visible) — `basisCurves.cpp:1357-1387`, `basisCurves.glslfx:770-791`.
- Deferred GPU skinning and ExtComputation-driven `points` both work for curves — `basisCurvesShaderKey.cpp:203`, `skinning.glslfx:14-25`, `usdSkelImaging/pointsResolvingSceneIndex.cpp:33`, `basisCurves.cpp:888-896, 949-970`.
- Instancing of curve prototypes is supported (VS/TES include `Instancing.Transform`; `HdStUpdateInstancerData` runs per draw item) — `basisCurvesShaderKey.cpp:202`, `basisCurves.cpp:183-199`.

## Open questions

- Whether a material glslfx mixin may legally reference the curve stage's `inData.Neye` (the tangent) from `surfaceShader()`; it is in the same fragment shader translation unit, but I have not compiled a test. Fallback: emit a `vertex` tangent primvar from the plugin.
- Whether Storm's MaterialX shader generation accepts a `chiang_hair_bsdf`-based surface (needs a compile test in `testusdview` with the installed MaterialX 1.39.5).
- Whether any Hydra-2 scene index path populates `invisibleCurves`/`invisiblePoints` (the adapter's topology conversion does not; a custom scene delegate would be needed, or an `HdSceneIndexAdapterSceneDelegate` change) — untested.
- Exact behaviour of `TestIntersection` in `DRAW_POINTS` mode for CV picking (the engine never sets `pickTarget=pickPoints`, so `resolveNearestToCenter` may return non-point hits); an app-level `HdxPickTask` invocation from Python was not verified to be exposed.
- The real per-frame cost figures (ms for N=100k curves × 8 CVs `points` upload; batch-rebuild cost) — need a benchmark with `HD_RPRIM_UPDATED`/`HDST_DRAW_BATCH` tracing on the GB10 GPU.
- Whether `HdStBasisCurves` handles a `points` array larger than `Σ counts` gracefully for padding strategies (topology visibility comment at `basisCurves.cpp:652-655` suggests yes for the visibility BAR; the interpolater truncates only when `curveIndices` are present, `basisCurvesComputations.h:210-221`) — needs a test.
- Whether `HdSt_MaterialParam` fallback typing accepts `float[2]/[4]` array-typed glslfx parameters for `HdGet_` (parser emits `arraySize` 2/4, `parserPlugin.cpp:102-150`); the sample shader comments out `HdGet_result()` (`surface_texture.glslfx:59`), hinting at limitations.
