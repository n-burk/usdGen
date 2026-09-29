# G — Storm hair "looks rendered": prototype, three routes, measured

Gap key: `storm-hair-look`. Target: OpenUSD **v26.08** source `<openusd-src>`, install
`$USD`. All probe sources, scenes, generated GLSL dumps and screenshots live in
`<session-scratch>`
(referred to below as `$P/`).

---

## 0. ENVIRONMENT CORRECTION (this changes what the whole plan can verify)

`ENVIRONMENT.md` says Storm cannot run here because `garch` on Linux is GLX-only
(`pxr/imaging/garch/glPlatformContextGLX.cpp`) and there is no X display. **That is true of garch's
*context creation*, but Storm itself never creates a context** — `HgiGL` only calls `GarchGLApiLoad()`
(`<openusd-src>/pxr/imaging/hgiGL/hgi.cpp:54`), and `GarchGLApiLoad` resolves entry points
through `glXGetProcAddressARB` from libGL (`pxr/imaging/garch/glApi.cpp:3130-3164`), which under GLVND
dispatches against **whatever** context is current — including one created by EGL. A grep over
`hgiGL/ hdSt/ hdx/ usdImagingGL/` finds **zero** references to `GlfGLContext` outside unit-test drawing
harnesses (only `hgiGL/capabilities.cpp:52`, `hgiGL/hgi.cpp:54`, and the two `unitTestGLDrawing.cpp` files).

So a context made with `EGL_EXT_platform_device` is enough. I built one by hand (no EGL headers on this
host; entry points declared and `dlopen`ed in `$P/eglctx.h`) and **Storm renders headlessly on this machine**:

```
$ $P/eglprobe
devices=3
dev 0: EGL 1.5 vendor=NVIDIA
dev 0: surfaceless=yes no_config=yes
dev 0: GL_VERSION=4.5.0 NVIDIA 580.173.02   GL_RENDERER=measurement host   GLSL=4.50 NVIDIA
OK-CONTEXT dev=0
```

Two details matter and are baked into `$P/eglctx.h`:

| Symptom | Cause | Fix (in `eglctx.h`) |
|---|---|---|
| ~25 `HgiGLPostPendingGLErrors ... GL error: invalid enum` per frame, final image all-white | strict **core** profile: HdSt/HgiGL save/restore a few legacy enums; core rejects them | request `EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x2` (compatibility) → GL 4.6 compat, **0 GL errors** |
| `GL error: invalid operation` in `HgiGL_ScopedStateHolder` ctor + `ResolveFramebuffer` | **surfaceless** context leaves the default FBO incomplete; draw-buffer queries fail | `eglChooseConfig` with `EGL_PBUFFER_BIT|EGL_OPENGL_BIT` + a 64×64 pbuffer, `eglMakeCurrent(d, surf, surf, ctx)` |

Consequence: **every "UNMEASURED / needs a workstation" claim about Storm in the earlier reports (A5 in
particular) can be turned into a measurement on this box.** All numbers and screenshots below are real,
produced by `$P/build/render_hair` and `$P/build/bench_hair`. `usdrecord` still core-dumps because its
`main` creates a GLX window before anything else; the fix is to link `eglctx.h` into your own driver, as
`$P/render_hair.cpp` does (39 lines around `UsdAppUtilsFrameRecorder`).

---

## 1. Verdict up front

| Route | Compiles in Storm 26.08? | Looks like hair? | Recommendation |
|---|---|---|---|
| **(1) Plugin glslfx surface shader** (`$P/usdGenHairPreview.glslfx`) | **Yes — rendered, 0 warnings** | **Yes** (`$P/hair_pv_tangent.png`) | **v1 look.** Ship two copies: `defaultMaterialTag` (opaque + alpha-to-coverage) and `translucent` (OIT). |
| **(2) MaterialX `chiang_hair_bsdf`** | **Yes — compiles and renders, no crash, no warning** | **No.** Near-black even at melanin 0.03 (`$P/hair_mtlx_light.png`) because Storm's MaterialX lighting only integrates a reflection closure and `Tworld` is a fake tangent | Not v1. Author it in the USD material anyway as the *render-time* look (hdPrman/Karma consume it correctly); Storm shows route 1. |
| **(3) UsdPreviewSurface + uniform `st` + `UsdUVTexture`** | **Yes** | Colour maps work perfectly (`$P/hair_preview_tex.png`), but shading is isotropic GGX — no strand highlight | **Ship as the fallback/compat material** for non-Storm delegates and for "no plugin installed". |

---

## 2. Route 1 — the plugin glslfx (the deliverable)

`$P/usdGenHairPreview.glslfx`, 497 lines. Kajiya-Kay diffuse with a wrap term + two Marschner-lite
longitudinal Gaussian lobes (R and TRT) + a cheap TT rim + root→tip albedo ramp + per-curve hue/value
jitter + optional scalp colour map + width-wise soft edge.

### 2.1 Sdr parses it (verified headlessly)

`$P/sdr_parse_test.py` (run with `PYTHONPATH=$USD/lib/python3.12/site-packages`):

```
node: 611668652201644050<><glslfx> (context: 'glslfx', ...)
  sourceType : glslfx      context : glslfx
  metadata   : {'sdrUsdEncodingVersion': '-1',
                'primvars': 'hairId|hairT|hairTangent|st', 'domain': 'rendering'}
  inputs (20): colorRamp/diffuseGain/... (float), rootColor/tipColor/specular1Color/... (color),
               rootColorMap (color, from the "textures" block, default (1,1,1))
  outputs: []
sourceCode node: 18053392800633449744 inputs: 20
```

Both discovery paths work: `Sdr.Registry().GetShaderNodeFromAsset(..., sourceType="glslfx")`
(`info:glslfx:sourceAsset`) and `GetShaderNodeFromSourceCode(src, "glslfx")` (`info:glslfx:sourceCode`).
The `attributes` block becomes the node's `primvars` metadata verbatim
(`pxr/usdImaging/plugin/sdrGlslfx/parserPlugin.cpp:262-275`), which is what survives Storm's primvar
filtering. The `textures` block honours a `"default"` key (`pxr/imaging/hio/glslfxConfig.cpp:34` maps
`defVal → "default"`, read at `:548-550`), so an unconnected texture input yields a usable white fallback
rather than the parser's black (`parserPlugin.cpp:236-242`).

### 2.2 It compiles and renders

`$P/render_hair.cpp` + `$P/hair_scene.usda` (4 000 cubic/bspline/pinned curves, 8 CVs each, on a sphere
"scalp", `widths` vertex, `hairT` vertex float, `hairTangent` vertex vec3, `hairId` uniform float,
`displayColor` uniform color3f, `st` uniform texCoord2f, `minScreenSpaceWidths` constant 1.0).

```
$ LD_LIBRARY_PATH=$USD/lib HAIR_CAMLIGHT=1 \
  ./build/render_hair hair_scene.usda hair_camlight.png /World/Cam 800
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
Record -> hair_camlight.png : OK          # zero warnings, zero GL errors
```

Screenshots: `$P/hair_camlight.png` (complexity 1.5), `$P/hair_pv_tangent.png` (complexity 1.2, the
recommended refineLevel 2 = RIBBON+ROUND), `$P/hair_translucent.png` (translucent/OIT variant).

### 2.3 The generated fragment shader — what is actually in scope

Dumped with `TF_DEBUG=HDST_DUMP_SHADER_SOURCEFILE` into `$P/dump/program4_shader7_FRAGMENT_SHADER.glsl`
(2 596 lines). Verified layout, with the code that produces each part:

| Line in dump | Content | Producer |
|---|---|---|
| 41 | `// //////// Codegen Defines ////////` | `codeGen.cpp:2063` |
| 168-172 | `#define HD_HAS_displayColor 1`, `HD_HAS_hairId`, `HD_HAS_hairT`, `HD_HAS_hairTangent` | `codeGen.cpp:2200,2214` |
| 196 | `// //////// Codegen Decl ////////` | `codeGen.cpp:2064` |
| 342-347 | `in centroid float u; in centroid float v;` then **`in CurveVertexData { vec4 Peye; vec3 Neye; } inData;`** | `_GenerateGLSLResources` BLOCK case, `codeGen.cpp:1122-1155`, fed by the `Curves.Fragment.Patch` layout `basisCurves.glslfx:1258-1267` |
| 350-356 | `in PrimvarData { float hairT; vec3 hairTangent; vec3 points; float widths; } inPrimvars;` | interstage primvar plumbing |
| 359 | `// //////// Codegen Accessors ////////` (all `HdGet_*`) | `codeGen.cpp:2065`; final FS string is `_genDefines + _genDecl + resDecl + _osd + _genAccessors + _genFS` (`codeGen.cpp:2914-2916`) |
| 861-862 | `#define NUM_LIGHTS 16`, `#define USE_SHADOWS 0` | lighting shader |
| ~975-1300 | `simpleLighting.glslfx`: `LightSource`, `GetLightCount()`, `GetLightSource(i)`, `lightDistanceAttenuation`, `lightSpotAttenuation`, `shadowing(lightIndex, Peye)` | `renderPassState.cpp:543-550` puts lighting first in the shader vector; `drawBatch.cpp:446` appends the material last; `codeGen.cpp:2513-2538` concatenates in that order |
| 2050-2350 | **our material source**, `vec4 surfaceShader(...)` at **2202** | `codeGen.cpp:2536-2537` |
| 2516 | `Curves.Fragment.Patch`'s `void main(void)` and `fragmentNormal(Peye, inData.Neye, v)` | `codeGen.cpp:2577` |

**Therefore: a material glslfx's `surfaceShader()` CAN legally read `inData.Neye` on HdStGL** — the block
is declared 1 850 lines before it. This settles A5's first open question. But see §2.5 for why you should
not.

Note the resource-generation path taken: `HDST_ENABLE_HGI_RESOURCE_GENERATION` defaults **false**
(`codeGen.cpp:166`) and `IsEnabledHgiResourceGeneration` returns `isEnabled || !_IsHgiOpenGL(hgi)`
(`codeGen.cpp:189-195`), so on HgiGL the **GLSL** path (`_CompileWithGeneratedGLSLResources`) runs. On
Metal/Vulkan the Hgi path runs and interstage blocks are re-shaped — untestable here, flagged in §7.

### 2.4 Primvar and parameter binding — all measured from the dump

| Authored as | Generated accessor | Evidence (dump line) |
|---|---|---|
| `uniform float hairId` | `float HdGet_hairId(int){ int index = GetAggregatedElementID(); return float(hairId[index]); }` | 688-694, with `GetElementID() = hd_int_get(HdGet_primitiveParam())` at 662 |
| `uniform texCoord2f st` | `vec2 HdGet_st(int){ int index = GetAggregatedElementID(); return vec2(st[index]); }` | 695-701 |
| `uniform color3f displayColor` | `vec3 HdGet_displayColor()` via `GetAggregatedElementID()` | 680-687 |
| `vertex float hairT` | `float HdGet_hairT(int){ return float(inPrimvars.hairT); }` | 702-705 |
| `vertex vector3f hairTangent` | `vec3 HdGet_hairTangent(int){ return vec3(inPrimvars.hairTangent); }` | 706-709 |
| glslfx `parameters` scalar/color | `vec3 HdGet_rootColor(){ ... shaderData[shaderCoord].rootColor_fallback; }` | 761-767 |
| glslfx `textures` (unconnected) | `vec3 HdGet_rootColorMap()` → the declared white fallback | 852-858 |

**`float[2]` / `float[4]` glslfx parameters DO work** — this *contradicts* my own first reading of
`resourceBinder.cpp:686-699` and answers A5's last open question. Probe `$P/probe_arrayparam.glslfx`
adds `"arrayParam2": {"default":[0.5,0.25]}` and `"arrayParam4": {...}` and calls
`HdGet_arrayParam2().x + HdGet_arrayParam4().w`. It renders with no warning, and
`$P/dump_array/program4_shader7_FRAGMENT_SHADER.glsl` contains:

```
287:  vec2 arrayParam2_fallback;
288:  vec4 arrayParam4_fallback;
723:vec2 HdGet_arrayParam2(int localIndex) { ... return vec2(shaderData[shaderCoord].arrayParam2_fallback); }
730:vec4 HdGet_arrayParam4(int localIndex) { ... }
```

Mechanism (verified in Python): `SdrGlslfxParserPlugin` makes them `Float` + `arraySize` 2/4
(`parserPlugin.cpp:100-147`), and `SdrShaderProperty::GetTypeAsSdfType()` folds that to **`float2` /
`float4`**, so the fallback `VtValue` reaching `HdSt_MaterialParam` is a `GfVec2f`/`GfVec4f`, not a
`VtFloatArray`:

```
arrayParam2  sdrType=float arraySize=2  sdfType=float2  defaultAsSdf=(0.5, 0.25)
arrayParam4  sdrType=float arraySize=4  sdfType=float4  defaultAsSdf=(0.5, 0.25, 0.125, 1)
```

`HdGetValueTupleType(GfVec2f) = {HdTypeFloatVec2, 1}` (`pxr/imaging/hd/types.cpp:244-260`) →
`GetGLSLTypename` → `vec2` (`glConversions.cpp:245-...`). The commented-out `HdGet_result()` in
`testUsdImagingGLSdr/surface_texture.glslfx:59` is stale, not a live limitation.

### 2.5 The strand tangent — the one real design decision

`patchCoord` handed to `surfaceShader` on curves is `vec4(0, v, 0, 0)` where **`v` is ACROSS the width**,
not along the strand: the TES sets `u = gl_TessCoord.y; v = gl_TessCoord.x`
(`basisCurves.glslfx:745-746`) and the FS builds `patchCoord = vec4(0, v, 0, 0)`
(`basisCurves.glslfx:1293`; the wire repr hard-codes `vec4(0, 0.5, 0, 0)` at `:1244`). **The task brief's
"root/tip colour lerp on curve v" is not achievable from `patchCoord`** — root→tip must be a primvar. The
shipped shader uses a `vertex float hairT`.

Three tangent sources, all tested:

| Source | Result | Evidence |
|---|---|---|
| **`inData.Neye`** (the tangent stored by `orient()` for HALFTUBE / ROUND / implicit ribbon, `basisCurves.glslfx:866, 889, 1272-1274`) | Works at refineLevel ≥1. **Hard-fails at refineLevel 0 / no `widths` / on any mesh**, because the WIRE layout's block has only `Peye` (`basisCurves.glslfx:1212-1218`). Storm then silently swaps in the fallback shader. | `$P/probe_indata.glslfx` at complexity 1.2 → `Record: OK`. Same shader at complexity 1.0 → `Failed to compile shader (FRAGMENT_SHADER): 0(2206) : error C1009: "Neye" is not member of struct "CurveVertexData"` (`glslProgram.cpp:211`) then `Failed to compile shader for prim /World/Hair.` (`drawBatch.cpp:385`). Images `$P/hair_indata_patch.png` vs `$P/hair_indata_wire.png`. |
| **`vertex vec3 hairTangent` primvar** (object space, transformed by `GetWorldToViewMatrix() * HdGet_transform()`) | **Best.** Clean coherent highlight band. `HdGet_transform()` is a *constant* primvar accessor emitted into `_genAccessors` (`codeGen.cpp:5243-5406`), i.e. present in the fragment stage — confirmed in the dump. `ApplyInstanceTransform()` is **not** in the FS (the `Instancing.Transform` mixin is VS/TES/PTVS only, `basisCurvesShaderKey.cpp:202,284,319,367,395,435`), so this path ignores instance rotation. | `$P/hair_pv_tangent.png` |
| **Screen-derivative reconstruction** `T = cross(N, dPdv)`, `dPdv = (dPdx·dv/dx + dPdy·dv/dy)/(‖∇v‖²)` | Works everywhere, needs no extra data, but is **visibly sparkly** — derivatives are constant per 2×2 quad and strands are 1-2 px wide. | `$P/hair_deriv_tangent.png` (same scene, `hairTangent` primvar removed) |

**Decision: the plugin authors a `vertex vec3 hairTangent` primvar; the glslfx uses it under
`#ifdef HD_HAS_hairTangent` and falls back to the derivative path, and never touches `inData`.** That is
what `$P/usdGenHairPreview.glslfx` does. Cost: 12 B/CV (1.6 M CVs → 19 MB), and it is renderer-agnostic
(hdPrman/Karma/Cycles can all read a tangent primvar).

### 2.6 Material tag, opacity and OIT — measured

`_ComputeMaterialTag` precedence (`pxr/imaging/hdSt/primUtils.cpp:292-327`):
`displayInOverlay > translucentToSelection > **material opinion** > displayOpacity → masked > default`.

* **A bound material's tag always wins over `displayOpacity`.** With our glslfx bound, a `displayOpacity`
  primvar does nothing for blending.
* With **no material bound**, `displayOpacity` promotes the draw item to **`masked`, not `translucent`**
  — i.e. alpha-test + alpha-to-coverage, not blending. Measured: `$P/hair_scene_dispopacity.usda`
  (material unbound, `uniform displayOpacity = 0.35`) renders as dithered screen-door hair,
  `$P/hair_dispopacity.png`.
* The strongest opinion is the glslfx's own `"metadata": {"materialTag": ...}`
  (`pxr/imaging/hdSt/materialNetwork.cpp:66-80`). `"translucent"` routes the prims to `HdxOitRenderTask`
  (`hdx/taskController.cpp:331-336`) — order-independent transparency, exactly right for dense fur.
  `"defaultMaterialTag"`/`"masked"` get `blendEnable=false, depthMask=true, enableAlphaToCoverage=true`
  (`taskController.cpp:392-398`), which is what keeps sub-pixel strands from popping.
* Measured comparison: `$P/usdGenHairPreview_translucent.glslfx` (identical but tag `translucent`,
  `opacity=0.35`, `widthFalloff=1`) → `$P/hair_translucent.png`, visibly softer and correctly blended
  through overlapping strands.

**Ship both files.** One `usdGenHairPreview.glslfx` (opaque + A2C, the default for scalp hair) and one
`usdGenHairPreviewTranslucent.glslfx` (OIT, for fine fur / peach fuzz / soft edges). They must be separate
files because the tag is baked into the glslfx metadata and a material's tag also splits draw batches.

### 2.7 Lights

`SetLightingState(lights=[], ...)` — what `UsdAppUtilsFrameRecorder` does when
`SetCameraLightEnabled(false)` (`usdAppUtils/frameRecorder.cpp:436-454`) — produced a **completely black
image even with two `UsdLux` `DistantLight`s in the stage** (`$P/hair_glslfx.png`). With the camera light
enabled the same scene lights up correctly. Do not conclude "scene lights don't work in Storm" from this;
conclude that the `usdrecord`/FrameRecorder path needs at least one `GlfSimpleLight` and that the tools
plan must set lighting explicitly. UNVERIFIED which of `HdxSimpleLightTask`'s inputs is being cleared.

---

## 3. Route 2 — MaterialX `chiang_hair_bsdf`: it runs, and it does not look like hair

Authored in USD as `ND_deon_hair_absorption_from_melanin` → `ND_chiang_hair_roughness` →
`ND_chiang_hair_bsdf` → `ND_surface`, bound through `outputs:mtlx:surface`
(`$P/make_scene_mtlx.py`, `$P/hair_scene_mtlx.usda`). Nodedefs at
`$USD/libraries/pbrlib/pbrlib_defs.mtlx:146-158` (bsdf) and `:433-441` (melanin).

**It compiles and renders.** No warnings, no fallback. The generated FS
(`$P/dump_mtlx/program5_shader9_FRAGMENT_SHADER.glsl`, 3 721 lines, 96 `chiang` hits) contains
`mx_chiang_hair_bsdf(...)` at line 3303 and the calls at 3443/3459. So A5's "Storm + hair BSDF is
UNVERIFIED" is now **verified: it works mechanically**.

The look is wrong for two independent reasons:

1. **`curve_direction` gets a fake tangent.** The nodedef declares
   `<input name="curve_direction" type="vector3" defaultgeomprop="Tworld"/>`
   (`pbrlib_defs.mtlx:156`), and `HdStMaterialXShaderGen` computes `Tworld` as
   ```
   #ifdef HD_HAS_st
       mat3 TBN = ComputeTBNMatrix(positionWorld, normalWorld, HdGet_st());
       vec3 tangentWorld = TBN[0];
   #else
       vec3 bitangentWorld = vec3(0, 1, 0);
       vec3 tangentWorld = cross(normalWorld, bitangentWorld);
   ```
   (`pxr/imaging/hdSt/materialXShaderGen.cpp:29-46`; verbatim in the dump at lines 2239-2251).
   The `#else` branch is taken (the network has no texcoord node, so `HD_HAS_st` is undefined in that
   program) → the "strand direction" is `cross(N,(0,1,0))`, unrelated to the hair.
   **Fix that works:** an explicit `ND_geompropvalue_vector3` node with `geomprop = "hairTangent"`
   connected to `curve_direction`. Verified end-to-end — `$P/hair_scene_mtlx2.usda`, and the regenerated
   dump `$P/dump_mtlx2/program5_shader9_FRAGMENT_SHADER.glsl` has
   `169: #define HD_HAS_hairTangent 1`, `687: vec3 HdGet_hairTangent(...)`,
   `2260-2261: #ifdef HD_HAS_hairTangent HdGet_hairTangent(),`,
   `3426: ..._curvedirection_out = vd.i_geomprop_hairTangent;`.
   Note MaterialX wants it in **world** space; our primvar is object space (identity here).
2. **Even so, it renders near-black.** `$P/hair_mtlx_tangent.png` (melanin 0.55) and
   `$P/hair_mtlx_light.png` (melanin **0.03**, i.e. near-white hair) are both dominated by black strands
   with a thin gold rim. Storm's MaterialX light loop integrates a single reflection closure per light
   (`materialXShaderGen.cpp:51-...` `MxHdLightString`); the hair BSDF's energy lives mostly in the TT/TRT
   transmission lobes, which that loop does not drive. UNVERIFIED at the closure-type level — I did not
   instrument `closureData.closureType` — but the melanin sweep rules out "it's just dark hair".

**Plan consequence:** author the MaterialX hair network in the USD material as the *render-time* look
(hdPrman/Karma read it correctly — see `G-hdprman-and-usdrecord-render-time-chain.md`), and bind the
plugin glslfx as the Storm-specific opinion. UsdShade supports exactly this: `outputs:surface` (glslfx
terminal) alongside `outputs:mtlx:surface` and `outputs:ri:surface` on the same `Material` prim, and
Storm's declared shader source types are `glslfx` then `mtlx`
(`pxr/imaging/hdSt/renderDelegate.cpp:700-706`).

---

## 4. Route 3 — UsdPreviewSurface + uniform `st` + `UsdUVTexture`

**Verified working, including the specific question "is a uniform-interpolation `float2 st` on curves
bound for texture sampling?" — yes.**

`$P/make_scene_preview.py` binds `UsdPreviewSurface` whose `diffuseColor` comes from a `UsdUVTexture`
(`./scalp_map.png`, a 64×64 checker+gradient written by hand in `make_scene_preview.py`'s sibling snippet)
whose `st` comes from `UsdPrimvarReader_float2(varname="st")`, with `st` authored as a **uniform**
(per-curve) `texCoord2f[]`. Result `$P/hair_preview_tex.png`: every strand is flat-shaded with the texel
at its root UV, and the checker/gradient is unmistakable across the head. Code path:
`_MakeMaterialParamsForTexture` finds the primvar wired to `st`/`uv` and registers it as an additional
primvar (`materialNetwork.cpp:800-870`), and the accessor is `HdGet_<tex>()` using `HdGet_st().xy`
(`codeGen.cpp:4271-4290`); `HdGet_st()` itself resolves through `GetAggregatedElementID()` for uniform
interpolation, as shown in §2.4.

Limits confirmed by eye: no anisotropy, no root/tip variation without more primvars, and with ROUND
normals it reads as shiny plastic tubes, not hair.

---

## 5. Measured performance (real, this machine)

`$P/bench_hair.cpp`: `UsdImagingGLEngine` + `color` AOV, 1280×720, one camera light, 8 warm-up frames,
`glFinish()` inside the timed region. GPU **measurement host**, driver 580.173.02, GL 4.6 compat.
Scene: cubic/bspline/pinned curves, 8 CVs, `widths` vertex, our glslfx material, `minScreenSpaceWidths=1`.
Complexity → refineLevel per `usdImagingGL/engine.cpp:2317-2350`: 1.0→0, 1.1→1, 1.2→2, 1.3→3.

| curves | CVs | refine 0 (wire) | refine 1 (ribbon/HAIR) | **refine 2 (ribbon/ROUND)** | refine 3 (halftube/ROUND) |
|---|---|---|---|---|---|
| 4 000 | 32 000 | 0.62 ms (1620 fps) | 0.82 ms | **0.85 ms (1183 fps)** | 0.84 ms |
| 40 000 | 320 000 | 2.75 ms | 4.63 ms | **5.01 ms (200 fps)** | 4.99 ms |
| 200 000 | 1 600 000 | 12.43 ms | 8.17 ms | **23.93 ms (42 fps)** | 23.69 ms |

Per-frame CPU re-author of `points` + upload (`pointsAttr.Set()` then `Render()`, 40 frames, refineLevel 2):

| curves | CVs | static | deforming | delta | delta per MB of points |
|---|---|---|---|---|---|
| 4 000 | 32 000 (0.38 MB) | 0.85 ms | 1.06 ms | +0.22 ms | 0.57 ms/MB |
| 40 000 | 320 000 (3.8 MB) | 5.01 ms | 5.84 ms | +0.83 ms | 0.22 ms/MB |
| 200 000 | 1 600 000 (19.2 MB) | 23.93 ms | 26.39 ms | +2.46 ms | 0.13 ms/MB |

Reading of these numbers for the plan:
* **200 k full-length hairs at refineLevel 2 is ~42 fps at 720p** on this GPU with our glslfx. That is
  interactive. refineLevel 3 costs nothing extra here because the strands are ≤2 px wide, so the halftube
  width-wise tessellation stays at 1 column (`basisCurves.glslfx:548-603` only adds columns above ~10 px).
* **Deformation is cheap**: streaming 19.2 MB of new points costs ~2.5 ms *including* the `UsdAttribute::Set`.
  A scene-index publishing `primvars/points` directly (no USD authoring) should be at or below this. This
  supports the plan's "deform static curves with a deforming surface at interactive rates".
* **refineLevel 0 is NOT always the fast mode.** At 200 k it is *slower* than refineLevel 1 (12.4 ms vs
  8.2 ms) — at level 0 cubic curves are drawn as a `LineList` polyline through every CV
  (`basisCurves.cpp:292-301`), i.e. 7 line segments per curve, whereas level 1 emits 4-CV patches whose
  screen-space tessellation factor collapses to ~1 for thin hair. So the LOD ladder for interaction
  should be **decimate curve count**, not drop refineLevel.
  (Reproduced twice; not thermal noise.)
* These are single-prim numbers. All hair was one `basisCurves` prim, so this is the best case for
  batching (one draw item). Splitting into many prims will add per-prim Sync cost — unmeasured.

---

## 6. Workstation protocol (what still needs a human at a display)

Everything above already ran headless; these are the checks a display adds.

1. **usdview visual sign-off.**
   `usdview --renderer GL $P/hair_scene.usda`, complexity **High** (= refineLevel 2). Compare against
   `$P/hair_pv_tangent.png`. Toggle *Display → Complexity* Low/Medium/High/Very High and confirm the
   ladder 0/1/2/3 (`usdAppUtils/complexityArgs.py:40-43`).
2. **Interactive frame time under camera motion**, which the offscreen bench cannot capture (no
   swap/present, no window compositor): enable `usdview`'s HUD (`Show → HUD → GPU stats`), tumble, read
   the ms counter. Expected to bracket the table in §5.
3. **MSAA / alpha-to-coverage quality.** `enableAlphaToCoverage` is on for the default tag
   (`taskController.cpp:396-398`) but only bites with a multisampled AOV. In `usdview`, compare
   `HD_ENABLE_SAMPLE_ALPHA_TO_COVERAGE` on/off and 1× vs 4× MSAA on ~1 px strands; then compare against
   the `translucent`/OIT build. Record which reads better at 1080p and at 4K.
4. **Non-NVIDIA drivers.** The glslfx uses only core GLSL 4.5 plus Storm's own macros; still, compile it
   once on AMD (RADV/amdgpu-pro) and once on Intel with `TF_DEBUG=HDST_DUMP_FAILING_SHADER_SOURCE`.
5. **Metal / Vulkan.** `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` forces the Hgi resource path even on GL
   (`codeGen.cpp:166,189-195`) and is *mandatory* on Metal/Vulkan. Run the whole probe set with that env
   var set to catch interstage-block/naming differences before shipping to a Mac. **Not testable here**
   (only `libusd_hgiGL.so` is built).
6. **Ptex.** Out of scope for Storm (`PXR_ENABLE_PTEX_SUPPORT=OFF`, and the Ptex accessor needs mesh-only
   `GetPatchCoord`, `codeGen.cpp:6617-6660` vs `mesh.glslfx:73`). CPU-side Ptex → `uniform displayColor` /
   `uniform st` is the route; §4 proves the uniform-primvar side works.

Re-running the headless probes after any change:

```
cd $P
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=$USD -DCMAKE_BUILD_TYPE=Release
ninja -C build
export LD_LIBRARY_PATH=$USD/lib
PYTHONPATH=$USD/lib/python3.12/site-packages $VENV/bin/python3 make_scene.py
HAIR_CAMLIGHT=1 ./build/render_hair hair_scene.usda out.png /World/Cam 800
TF_DEBUG=HDST_DUMP_SHADER_SOURCEFILE HAIR_CAMLIGHT=1 ./build/render_hair hair_scene.usda /tmp/x.png /World/Cam 400
./build/bench_hair hair_scene.usda /World/Cam 1280 720 1.2 60 0
```
(`make_scene_40000.py` / `make_scene_200000.py` regenerate the large scenes; they were deleted after the
benchmark because they are 34 MB / 172 MB of `.usda`.)

---

## 7. Recommendation for the plan

**v1 Storm look = route 1**, shipped as two glslfx files in the plugin's resource dir plus two
`UsdShade` `Shader` defs (`usdGen:HairPreview`, `usdGen:HairPreviewTranslucent`) discovered by
`UsdShadeShaderDefUtils` from a `shaderDefs.usda` in the plugin, exactly like `usdShaders` does. The
generator scene index authors, per hair prim:
`points`(vertex) · `widths`(vertex) · **`hairT`**(vertex float) · **`hairTangent`**(vertex vec3) ·
`hairId`(uniform float) · `st`(uniform float2, root UV) · `displayColor`(uniform color3f) ·
`minScreenSpaceWidths`(constant 1.0), with `type=cubic basis=bspline wrap=pinned` and
`displayStyle/refineLevel = 2`. No `normals` (authored normals force ORIENTED ribbons,
`basisCurves.cpp:324-327`).

**Fallback for non-Storm delegates and for "plugin not installed" = route 3.** Storm resolves a
material terminal by walking its declared shader source types in order — `glslfx`, then `mtlx`
(`pxr/imaging/hdSt/renderDelegate.cpp:700-706`) — and looks the terminal node up as
`GetShaderNodeByIdentifierAndType(nodeTypeId, "glslfx")` (`materialNetwork.cpp:120-164`). A single USD
`Material` prim can therefore carry three terminals: `outputs:surface` → the UsdPreviewSurface network
(universal, works in every delegate and when the plugin is absent), `outputs:mtlx:surface` → the
MaterialX hair network (render-time), and a Storm-specific `outputs:glslfx:surface` → our
`usdGen:HairPreview` shader. **Verify that Storm actually prefers a `glslfx:` render-context output over
the plain `outputs:surface` before relying on this** — I bound the glslfx on the plain `outputs:surface`
in every probe and did not test render-context resolution; the safe alternative, which needs no such
resolution, is for the generator scene index to author whichever material binding the current render
delegate wants (it knows the delegate). The two networks stay colour-compatible because both read the
same `uniform st` and `uniform displayColor` primvars.

**Route 2 is the render-time look**, authored on `outputs:mtlx:surface` of the same `Material` prim, with
an explicit `ND_geompropvalue_vector3(geomprop="hairTangentWorld")` into `curve_direction` (world space!)
so it is correct for hdPrman/Karma and does not silently pick up `cross(N,(0,1,0))`.

## Key facts

- **Storm renders headlessly on this host** via an EGL device-platform, compatibility-profile,
  pbuffer-backed GL 4.6 context; `HgiGL` only needs `GarchGLApiLoad()` (`hgiGL/hgi.cpp:54`, `garch/glApi.cpp:3130-3164`) and no HdSt/Hgi/Hdx/UsdImagingGL code references `GlfGLContext` outside test harnesses. Harness: `$P/eglctx.h`, driver `$P/render_hair.cpp`. Core profile ⇒ ~25 `GL_INVALID_ENUM`/frame and a white image; surfaceless ⇒ `GL_INVALID_OPERATION` in `HgiGL_ScopedStateHolder` + failed resolve. Compat + pbuffer ⇒ 0 errors.
- The plugin glslfx `$P/usdGenHairPreview.glslfx` **parses in Sdr** (20 inputs, `primvars` metadata `hairId|hairT|hairTangent|st`) via both `GetShaderNodeFromAsset(..., "glslfx")` and `GetShaderNodeFromSourceCode`, and **compiles and renders in Storm with zero warnings** (`$P/hair_pv_tangent.png`).
- The FS translation unit order is `_genDefines + _genDecl + resDecl + _osd + _genAccessors + _genFS`, and inside `_genFS`: codegen bits → lighting shader → render-pass shader → **material** → geometric shader (`codeGen.cpp:2513-2538, 2577, 2914-2916`; `renderPassState.cpp:543-550`; `drawBatch.cpp:446`). Confirmed in `$P/dump/program4_shader7_FRAGMENT_SHADER.glsl` (inData block at 344, material `surfaceShader` at 2202, `Curves.Fragment.Patch` `main()` at 2516).
- A material glslfx **may** read `inData.Neye` (the strand tangent) on HdStGL, **but must not**: at refineLevel 0 the wire layout has only `Peye` (`basisCurves.glslfx:1212-1218`) and the shader fails with `error C1009: "Neye" is not member of struct "CurveVertexData"` (`glslProgram.cpp:211`), after which Storm substitutes the fallback shader (`drawBatch.cpp:385`). Measured with `$P/probe_indata.glslfx` at complexity 1.0 vs 1.2.
- `patchCoord` on curves is `vec4(0, v, 0, 0)` with **v across the width** (`basisCurves.glslfx:1293`; TES `v = gl_TessCoord.x` at `:746`), so root→tip must come from a primvar — the brief's "root/tip lerp on curve v" is not achievable from `patchCoord`.
- A `vertex vec3` tangent primvar gives a clean highlight; screen-derivative reconstruction is visibly sparkly on 1-2 px strands. Side-by-side: `$P/hair_pv_tangent.png` vs `$P/hair_deriv_tangent.png`.
- `HdGet_transform()` (constant primvar) **is** available in the fragment stage (`codeGen.cpp:5243-5406` writes to `_genAccessors`); `ApplyInstanceTransform()` is **not** (`basisCurvesShaderKey.cpp:202,284,319,367,395,435`).
- **`float[2]` / `float[4]` glslfx parameters bind fine** as `vec2`/`vec4` through `HdGet_` — verified in the generated GLSL (`$P/dump_array/...:287-288, 723-735`); mechanism is `SdrShaderProperty::GetTypeAsSdfType()` folding Float+arraySize2/4 to `float2`/`float4`. This *reverses* A5's open question and the hint in `surface_texture.glslfx:59`.
- **`displayOpacity` on curves does not give translucency.** A bound material's tag wins (`primUtils.cpp:292-327`); with no material, `displayOpacity` promotes the item to **`masked`** (alpha-test + alpha-to-coverage), rendering as screen-door dither (`$P/hair_dispopacity.png`). Real blending requires the glslfx metadata `"materialTag": "translucent"`, which routes prims to `HdxOitRenderTask` (`hdx/taskController.cpp:331-336`) — measured, and visibly better for fine fur (`$P/hair_translucent.png`).
- **MaterialX `chiang_hair_bsdf` compiles and renders in Storm 26.08** (`mx_chiang_hair_bsdf` at line 3303 of `$P/dump_mtlx/program5_shader9_FRAGMENT_SHADER.glsl`) but looks near-black even at melanin 0.03, because `Tworld` is `cross(normalWorld,(0,1,0))` when no texcoord primvar is present (`materialXShaderGen.cpp:29-46`, verbatim at dump lines 2239-2251) and Storm's MaterialX light loop does not drive the TT/TRT lobes.
- An explicit `ND_geompropvalue_vector3(geomprop="hairTangent")` **does** feed `curve_direction` in Storm — verified in `$P/dump_mtlx2/...`: `#define HD_HAS_hairTangent 1` (169), `HdGet_hairTangent` (687), `vd.i_geomprop_hairTangent` used as `curve_direction` (3426).
- **UsdPreviewSurface + `UsdPrimvarReader_float2` on a UNIFORM `st` + `UsdUVTexture` works on curves** — per-curve root-UV texture lookup, proven by the checker/gradient mapping in `$P/hair_preview_tex.png`; `HdGet_st()` resolves via `GetAggregatedElementID()`.
- Measured Storm frame times at 1280×720 on measurement host (driver 580.173.02), one draw item, our glslfx: 4 k curves 0.85 ms; 40 k 5.0 ms; **200 k curves / 1.6 M CVs 23.9 ms (42 fps)** at refineLevel 2. Per-frame points re-author + upload adds 0.22 / 0.83 / 2.46 ms respectively.
- refineLevel 0 is *slower* than refineLevel 1 at 200 k curves (12.4 vs 8.2 ms) because cubic curves at level 0 are drawn as a per-CV `LineList` polyline (`basisCurves.cpp:292-301`). Interaction LOD should reduce curve **count**, not refine level.
- `usdrecord`/`UsdAppUtilsFrameRecorder` with `SetCameraLightEnabled(false)` renders the scene **black** despite two `UsdLux` `DistantLight`s (`frameRecorder.cpp:436-454` passes an empty `GlfSimpleLightVector`); the tools plan must set lighting explicitly.
- glslfx `textures` entries honour a `"default"` value (`hio/glslfxConfig.cpp:34, 548-550`), so an unconnected map input yields white rather than the parser's black fallback (`parserPlugin.cpp:236-242`).
- On HgiGL the **GLSL** resource path runs, not the Hgi one (`HDST_ENABLE_HGI_RESOURCE_GENERATION` defaults false, `codeGen.cpp:166`; `IsEnabledHgiResourceGeneration` returns true only for non-GL, `:189-195`). Everything verified here is the GL path.

## Decisions this settles

1. **v1 Storm hair look = a plugin glslfx surface shader.** The file exists, parses, compiles and renders: `$P/usdGenHairPreview.glslfx`. Ship it plus an identical `translucent`-tagged copy.
2. **Do not read `inData` from the material.** Use a `vertex vec3 hairTangent` primvar (`#ifdef HD_HAS_hairTangent`) with the screen-derivative reconstruction as the graceful fallback. This keeps the same material valid on wire reprs, on meshes, and (probably) on Metal/Vulkan.
3. **Root→tip variation is a `vertex float hairT` primvar, not `patchCoord`.** `patchCoord.y` is the width coordinate and is only good for silhouette/edge effects.
4. **The generator's mandatory primvar set** is `points, widths, hairT, hairTangent, hairId, st, displayColor, minScreenSpaceWidths`, `type=cubic basis=bspline wrap=pinned`, no `normals`, `displayStyle/refineLevel=2`.
5. **Opacity strategy is a material-tag decision, not a primvar decision.** Opaque + alpha-to-coverage by default; a second `translucent`/OIT material for fine fur. `displayOpacity` alone is a trap (it yields `masked`, and only when no material is bound).
6. **MaterialX hair is the render-time look, not the Storm look.** Author it on `outputs:mtlx:surface` with an explicit world-space tangent geomprop; do not expect Storm to preview it.
7. **UsdPreviewSurface + uniform `st` is the compat path** and is confirmed to sample scalp colour maps per curve — which also validates the "bake maps/SeExpr/Ptex to a per-curve `st` or `displayColor`" strategy from A5 §5.
8. **Array-typed glslfx parameters are allowed** (`float[2]`, `float[4]`), so the shader interface can use packed pairs where that is natural.
9. **Interaction LOD = curve decimation**, not refineLevel reduction.
10. **The whole project can develop and regression-test Storm output headlessly on this host** using `$P/eglctx.h`. Add it to the plugin's test harness; it removes the "needs a workstation" caveat from most of the imaging plan.

## Open questions

- Why does `UsdAppUtilsFrameRecorder` with an empty `GlfSimpleLightVector` suppress the stage's `UsdLux` lights entirely? (Affects the `usdrecord`-based golden-image tests the plan wants.) Not chased.
- Storm MaterialX: at the closure level, which of `chiang_hair_bsdf`'s lobes actually receive energy from `MxHdLightString`? Instrumenting `closureData.closureType` would say whether a small upstream patch could make MaterialX hair usable in Storm.
- Does the Hgi resource-generation path (`HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, and mandatory on Metal/Vulkan) preserve `inData`/`inPrimvars` block naming and the fragment-stage availability of `HdGet_transform()`? Testable here with the env var but not run; **must** be run before shipping to a Mac.
- Per-prim `displayStyle/refineLevel` from a scene index (A5 §10 claims it wins over usdview's complexity because the override is an underlay) — I drove refineLevel via `renderParams.complexity` instead. Worth a direct test with a custom scene index once the plugin exists.
- Multi-prim Sync cost: all benchmarks used one `basisCurves` prim. The cost of splitting a groom into N prims (per clump / per spatial chunk) for culling and partial updates is unmeasured.
- Shadow behaviour: `USE_SHADOWS` was 0 in every run (no shadow-casting lights configured). The `shadowing(i, Peye)` call in the shader is compiled out and therefore untested.
- The 200 k / refineLevel-0 inversion (wire slower than ribbon) reproduced twice but the mechanism (LineList over all CVs vs collapsed tessellation) is inferred from `basisCurves.cpp:292-301`, not profiled.
- Whether Storm prefers a `outputs:glslfx:surface` render-context terminal over the plain `outputs:surface` (needed for the three-terminal Material described in §7). Every probe bound the glslfx on the plain `outputs:surface`.
