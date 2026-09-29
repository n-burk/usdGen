# PW-4 — L-1 pre-work: Storm render-context material resolution (glslfx vs mtlx terminal)

## 1. What was asked

Roadmap `plan/11-roadmap.md` §1.1, PW-4 row (line 112):

> | **PW-4** | L-1 | the `USDGEN_STORM_MATERIAL_OVERRIDE` default (ADR §5.6, OFF) and whether `material_storm` is ever synthesized | one `Material` carrying both `outputs:surface` and `outputs:glslfx:surface` through the harness, diffed against two single-terminal renders. Source says glslfx wins: `materialRenderContexts = {glslfx, mtlx}` (`pxr/imaging/hdSt/renderDelegate.cpp:695-707`, `_RenderDelegateInfo()`; verified, ADR §9 R43) |

Gate L-1 (`plan/09-performance-and-benchmarks.md` §5): *"Storm render-context resolution | `outputs:glslfx:surface` resolves before plain `outputs:surface`; if not, flip `USDGEN_STORM_MATERIAL_OVERRIDE` to default ON | `testUsdGenStormMaterial` | M0 pre-work (**PW-4**), **decides M1** | UNMEASURED; source says it should."*

ADR §5.6: *"Three terminals on one `Material` (S36). `USDGEN_STORM_MATERIAL_OVERRIDE` default **OFF**; L-1 checks that Storm resolves `outputs:glslfx:surface` first. If it does not, flip the default and synthesize `<Description>/__usdGenRender/material_storm` (with `primOrigin`) and bind tiles to it."*

## 2. Method + exact commands

Four 4 000-curve (× 8 CV) scenes from `make_pw4_scenes.py` (shared geometry, camera, Key+Rim lights; only the `Material` terminals differ):

```python
KINDS = {
    "glslfx_only":     [("universal", "glslfx")],            # outputs:surface -> usdGenHairPreviewPrimvar.glslfx
    "mtlx_only":       [("universal", "mtlx")],              # outputs:surface -> chiang hair mtlx network
    "glslfx_ctx_only": [("glslfx", "glslfx")],               # outputs:glslfx:surface -> glslfx
    "both":            [("universal", "mtlx"), ("glslfx", "glslfx")],  # THE dual-terminal case
}
$VENV/python make_pw4_scenes.py   # writes pw4_*.usda (glslfx sourceAsset relative to the .usda)
```

Render (EGL headless, Storm, sRGB, complexity 1.5 → refineLevel 2, 640×640, cam `/World/Cam`):

```bash
export PATH=$VENV/bin:$PATH
export LD_LIBRARY_PATH=$USD/lib
export PXR_PLUGINPATH_NAME="...usdGenImaging/resources:...usdGenSchema/resources:$USD/plugin/usd:$USD/lib/usd"
cd docs/prework/probes/PW-egl
./build/render_hair pw4_both.usda pw4_both.png /World/Cam 640     # rendered this continuation
./build/render_hair pw4_both.usda pw4_both_r2.png /World/Cam 640  # determinism check
```

(`pw4_mtlx_only.png`, `pw4_glslfx_only.png`, `pw4_glslfx_ctx_only.png` were rendered in the previous attempt with the same command shape; all four PNGs are 640×640 RGBA.)

Pixel diff (`diff_png.py`: sha256 of each file, mean/max absolute per-channel difference, fraction of pixels differing > 8/255):

```bash
$VENV/bin/python diff_png.py pw4_both.png pw4_glslfx_only.png
$VENV/bin/python diff_png.py pw4_both.png pw4_mtlx_only.png
$VENV/bin/python diff_png.py pw4_glslfx_only.png pw4_mtlx_only.png       # control
$VENV/bin/python diff_png.py pw4_glslfx_only.png pw4_glslfx_ctx_only.png # terminal-name control
# output captured in pw4_diffs.txt
```

## 3. Raw evidence

### 3.1 Pixel diffs (`pw4_diffs.txt`, verbatim)

```
=== diff: both vs glslfx_only ===
A=pw4_both.png sha256=9b754c969c8a8c55a9f11084aa2ae4e3685abc0c155111e63f614b316ec5185d shape=(640, 640, 3)
B=pw4_glslfx_only.png sha256=efcae1e83f368f93334d8e159920d38df87ca642fb7c9f7a9f3b89a683d151d7 shape=(640, 640, 3)
meanAbsDiff=0.2742 maxAbsDiff=255 fracPixelsDiff>8=0.009768

=== diff: both vs mtlx_only ===
A=pw4_both.png sha256=9b754c969c8a8c55a9f11084aa2ae4e3685abc0c155111e63f614b316ec5185d shape=(640, 640, 3)
B=pw4_mtlx_only.png sha256=eb1ebc03fd3994d4faa5355fe4e265f07ca8ffd3cf8c96b8e55d6b28321743c7 shape=(640, 640, 3)
meanAbsDiff=37.3172 maxAbsDiff=255 fracPixelsDiff>8=0.365640

=== diff: glslfx_only vs mtlx_only (control) ===
A=pw4_glslfx_only.png sha256=efcae1e83f368f93334d8e159920d38df87ca642fb7c9f7a9f3b89a683d151d7 shape=(640, 640, 3)
B=pw4_mtlx_only.png sha256=eb1ebc03fd3994d4faa5355fe4e265f07ca8ffd3cf8c96b8e55d6b28321743c7 shape=(640, 640, 3)
meanAbsDiff=37.2049 maxAbsDiff=255 fracPixelsDiff>8=0.363591

=== diff: glslfx_only vs glslfx_ctx_only (terminal-name control) ===
A=pw4_glslfx_only.png sha256=efcae1e83f368f93334d8e159920d38df87ca642fb7c9f7a9f3b89a683d151d7 shape=(640, 640, 3)
B=pw4_glslfx_ctx_only.png sha256=efcae1e83f368f93334d8e159920d38df87ca642fb7c9f7a9f3b89a683d151d7 shape=(640, 640, 3)
meanAbsDiff=0.0000 maxAbsDiff=0 fracPixelsDiff>8=0.000000
```

### 3.2 Determinism + residual characterization

```
$ sha256sum pw4_both.png pw4_both_r2.png
9b754c969c8a8c55a9f11084aa2ae4e3685abc0c155111e63f614b316ec5185d  pw4_both.png
9b754c969c8a8c55a9f11084aa2ae4e3685abc0c155111e63f614b316ec5185d  pw4_both_r2.png
```

Two independent renders of the dual-terminal scene are bit-identical; the pipeline is deterministic in this harness.

Residual `both` vs `glslfx_only` (meanAbsDiff 0.27/255 ≈ 0.11 %): 4 001 pixels (0.977 %) differ by > 8/255, spread across the hair region (y 110–639, x 0–639; row-band density 365 / 1797 / 1218 / 621 over 160-px bands) — i.e. distributed edge/AA-level differences, not a materially different look. The terminal-name control proves the render is deterministic, so the residual is caused by the dual-terminal material itself (most plausibly a different material hash → different draw ordering → AA edge shifts), not by noise.

Draw-batch counts: **not recorded** (Storm does not expose batch stats through the `UsdAppUtilsFrameRecorder` harness used here).

### 3.3 Source evidence (planning-time, verified)

`pxr/imaging/hdSt/renderDelegate.cpp:695-707` `_RenderDelegateInfo()`: `materialRenderContexts = {glslfx, mtlx}` — glslfx is first; ADR S36 / R43.

## 4. DECISION

**DECISION: L-1 — measured: on a `Material` carrying both `outputs:surface` (chiang mtlx network) and `outputs:glslfx:surface` (usdGen glslfx), Storm renders the glslfx terminal: the dual-terminal render is near-identical to the glslfx-only render (meanAbsDiff 0.27/255; 0.98 % of pixels > 8/255, all edge/AA-level, deterministic across runs) and radically different from the mtlx-only render (meanAbsDiff 37.3; 36.6 % of pixels > 8/255, matching the glslfx-vs-mtlx control at 37.2 / 36.4 %).** The source prediction (`materialRenderContexts = {glslfx, mtlx}`, `renderDelegate.cpp:695-707`) is confirmed. Therefore `USDGEN_STORM_MATERIAL_OVERRIDE` keeps its **OFF** default (ADR §5.6): usdGen tiles bind their material through the glslfx render-context terminal and no per-delegate binding override is needed. **`material_storm` is never synthesized** — the `<Description>/__usdGenRender/material_storm` + `primOrigin` fallback stays documented but is not built/shipped.

## 5. Risks / follow-ups

- **Residual 0.98 % pixel diff** between `both` and `glslfx_only`: consistent with a material-hash-dependent draw order (AA edges), not a look difference. If M1's `testUsdGenStormMaterial` wants a bit-exact assertion, use the `glslfx_ctx_only`-style single-terminal material in the test scene; assert "matches glslfx within meanAbsDiff < 1.0" for the dual-terminal case. Worth one workstation check of whether the extra mtlx terminal fragments batches (roadmap §1.1 table-13 concern: "a differing shader hash or BAR pointer fragments the batch") — pixel evidence alone can't see batching.
- **Context coverage:** this was measured in the GL/Storm context with HGI resource generation **off**. The HGI (Metal/Vulkan proxy) path is exercised separately for the glslfx variant in PW-2 (variant A compiled and ran under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`); re-run the dual-terminal scene under that flag at M1 (`testUsdGenStormMaterial`) to confirm the same terminal order there.
- **mtlx network presence:** the mtlx chiang network in the `both` scene references `hairTangent` via `ND_geompropvalue_vector3`; since the glslfx terminal wins, that network is never evaluated — but its shader prims do exist in the material. No errors were raised; keep the material lean in shipped usdGen scenes (glslfx terminal only) to avoid any mtlx graph compilation cost on renderers that do resolve mtlx.
- Draw-batch statistics were not captured; add an optional `Hd` stats dump to `testUsdGenStormMaterial` if batch fragmentation becomes a question at M1.
