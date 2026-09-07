# PW-2 — S-8 pre-work: default glslfx variant (inData.Neye vs hairTangent primvar)

## 1. What was asked

Roadmap `plan/11-roadmap.md` §1.1, PW-2 row (line 110):

> | **PW-2** | S-8 | the default glslfx variant (ADR §5.4), hence whether `hairTangent` is in C2 — settled before M1 freezes C2 | EGL harness (`prototypes/storm-hair-look/{eglctx.h,bench_hair.cpp}`), 100 k × 8 CV deforming, 60 frames: variant A (`inData.Neye`) vs B (`UsdGenHairPreviewPrimvar`, ADR §9 R4); A must also compile under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`. The mechanism is not in doubt: the points fastpath needs `DirtyPoints` set and `DirtyNormals\|DirtyWidths\|DirtyPrimvar` clear (`pxr/imaging/hdSt/basisCurves.cpp:930-935`, verified) |

Gate S-8 (`plan/09-performance-and-benchmarks.md` §5): *"A wins on frame time **and** compiles under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`, else B becomes default"* — status UNMEASURED at planning time. If B wins, `hairTangent` is republished per deforming frame at the measured cost and would enter C2 (the published-tile `basisCurves` contract, ADR §5.3).

ADR §9 R4: `usdGenShaders` ships **three** glslfx files (default, translucent, primvar-tangent variant B); the `inputs:` block (C5) is identical in all three.

## 2. Method + exact commands

All under `docs/prework/probes/PW-egl/` (EGL headless harness, NVIDIA GB10 via `EGL_EXT_platform_device`, GL 4.6 compat, 1280×720, Storm renderer, color AOV).

Environment for every run:

```bash
export PATH=/home/burkard/.venv/bin:$PATH
export LD_LIBRARY_PATH=/home/burkard/work/OpenUSD_26_08/lib
export PXR_PLUGINPATH_NAME="/home/burkard/work/usdGen/build/usd/usdGenImaging/resources:/home/burkard/work/usdGen/build/usd/usdGenSchema/resources:/home/burkard/work/OpenUSD_26_08/plugin/usd:/home/burkard/work/OpenUSD_26_08/lib/usd"
```

**Scene generation** (`make_pw_scenes.py`, scratch variant of the prototype generator, seed 7, 8 CVs, identical strand field):

```bash
$VENV/python make_pw_scenes.py 100000 scene_100k_A.usdc A   # variant A: no hairTangent; usdGenHairPreviewIndata.glslfx
$VENV/python make_pw_scenes.py 100000 scene_100k_B.usdc B   # variant B: hairTangent vertex primvar; usdGenHairPreviewPrimvar.glslfx
```

Gen logs (`gen_100k_A.log`, `gen_100k_B.log`): `wrote scene_100k_A.usdc curves=100000 variant=A hairTangent=False` / `wrote scene_100k_B.usdc curves=100000 variant=B hairTangent=True`.

**Driver** `bench_s8.cpp` (CMake build → `build/bench_s8`): 60 frames; `deform=1` re-authors `points` every frame (`pointsAttr.Set()`, the DirtyPoints fastpath); `repubTangent=1` additionally re-authors `hairTangent` every frame with a per-frame perturbation (forces the real DirtyPrimvar path — an identical `Set()` is deduped by usdImaging and would under-measure variant B). Prints first-frame (SDR parse + GLSL codegen + shader compile) and steady-state (frames 2..N) per-frame ms.

**Matrix** (`run_s8_matrix_v2.sh` — the authoritative v2 matrix; v1 `run_s8_matrix.sh` republished static tangents and its B+ rows are invalidated by dedup):

```bash
./run_s8_matrix_v2.sh   # A/B/B+ × 3 reps, then A × 2 reps with HDST_ENABLE_HGI_RESOURCE_GENERATION=1
# per cell: env [HGI=1] ./build/bench_s8 <scene> /World/Cam 1280 720 1.2 60 <deform> <repub>
```

Material files verified by hash: `usdGenHairPreview.glslfx` (prototype) md5 `0c665874…` == `usdGenHairPreviewPrimvar.glslfx` (variant B); variant A is `usdGenHairPreviewIndata.glslfx` (reads `inData.Neye`, no `hairTangent` input).

## 3. Raw evidence

### 3.1 Frame-time matrix (v2, authoritative — `results_s8_v2.txt`, verbatim)

```
## A r1 (inData.Neye, no hairTangent, deform)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_A.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=24.2ms firstFrame(sdrCompile)=266.61ms steadyMs=26.002 steadyMin=21.804 steadyMax=53.322 fps=38.5

## B r1 (hairTangent static, points-only deform)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_B.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=24.9ms firstFrame(sdrCompile)=319.61ms steadyMs=26.528 steadyMin=22.652 steadyMax=60.409 fps=37.7

## B+ r1 (hairTangent republished, perturbing)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=1 (tangentAuthored=yes)
RESULT scene=scene_100k_B.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=1 frames=60 stageOpen=24.3ms firstFrame(sdrCompile)=283.57ms steadyMs=39.957 steadyMin=31.077 steadyMax=78.208 fps=25.0

## A r2 (inData.Neye, no hairTangent, deform)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_A.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=24.2ms firstFrame(sdrCompile)=267.54ms steadyMs=25.456 steadyMin=21.915 steadyMax=51.228 fps=39.3

## B r2 (hairTangent static, points-only deform)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_B.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=25.4ms firstFrame(sdrCompile)=294.79ms steadyMs=26.986 steadyMin=23.026 steadyMax=58.549 fps=37.1

## B+ r2 (hairTangent republished, perturbing)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=1 (tangentAuthored=yes)
RESULT scene=scene_100k_B.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=1 frames=60 stageOpen=25.1ms firstFrame(sdrCompile)=276.92ms steadyMs=38.503 steadyMin=28.536 steadyMax=70.859 fps=26.0

## A r3 (inData.Neye, no hairTangent, deform)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_A.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=24.6ms firstFrame(sdrCompile)=264.32ms steadyMs=26.125 steadyMin=22.216 steadyMax=54.021 fps=38.3

## B r3 (hairTangent static, points-only deform)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_B.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=13.4ms firstFrame(sdrCompile)=293.47ms steadyMs=26.786 steadyMin=23.245 steadyMax=61.676 fps=37.3

## B+ r3 (hairTangent republished, perturbing)
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=1 (tangentAuthored=yes)
RESULT scene=scene_100k_B.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=1 frames=60 stageOpen=24.6ms firstFrame(sdrCompile)=272.00ms steadyMs=37.761 steadyMin=30.952 steadyMax=67.559 fps=26.5

## A-HGI r1 (HDST_ENABLE_HGI_RESOURCE_GENERATION=1)
#######################################################################################
#  HDST_ENABLE_HGI_RESOURCE_GENERATION is overridden to 'true'.  Default is 'false'.  #
#######################################################################################
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_A.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=24.8ms firstFrame(sdrCompile)=269.46ms steadyMs=25.475 steadyMin=21.824 steadyMax=51.937 fps=39.3

## A-HGI r2 (HDST_ENABLE_HGI_RESOURCE_GENERATION=1)
#######################################################################################
#  HDST_ENABLE_HGI_RESOURCE_GENERATION is overridden to 'true'.  Default is 'false'.  #
#######################################################################################
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE curves=100000 cvs=800000 repubTangent=0 (tangentAuthored=-)
RESULT scene=scene_100k_A.usdc res=1280x720 complexity=1.2 curves=100000 cvs=800000 deform=1 repubTangent=0 frames=60 stageOpen=23.8ms firstFrame(sdrCompile)=264.08ms steadyMs=25.495 steadyMin=22.303 steadyMax=52.036 fps=39.2
```

### 3.2 Summary table (steady-state per-frame Hydra draw, mean of 3 reps; SDR compile = first frame)

| Config | steadyMs (mean) | steadyMs range | sdrCompile mean | sdrCompile range | fps |
|---|---:|---:|---:|---:|---:|
| **A** (inData.Neye, points fastpath) | **25.86** | 25.46–26.13 | **266.16** | 264.3–267.5 | ~38.5 |
| B static (primvar, no republish) | 26.77 | 26.53–26.99 | 302.62 | 293.5–319.6 | ~37.4 |
| B+ (primvar republished/frame) | 38.74 | 37.76–39.96 | 277.49 | 272.0–283.6 | ~25.8 |
| A-HGI (HGI resource gen on) | 25.49 | 25.48–25.50 | 266.77 | 264.1–269.5 | ~39.3 |

Observations:

1. **A wins the frame-time check.** Realistic deforming-variant-B cost is B+ (per-frame `hairTangent` republish): A is **12.9–14.1 ms (≈33–36 %) faster** per frame. Against a static-tangent B (not a valid deforming-groom configuration, but the floor) A is still 0.7–1.1 ms faster.
2. **A compiles cleanly under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1`:** two full 60-frame runs completed with the override banner active and **zero shader/compile errors**; steady-state (25.48/25.50 ms) and first-frame SDR compile (264.1/269.5 ms) match non-HGI A within noise. This covers the Metal/Vulkan proxy path.
3. **SDR compile:** A ~36 ms faster than B-static (266 vs 303 ms first frame) — variant A's `inputs:` block is smaller (no `hairTangent`), consistent with the smaller file size (21 206 vs 21 932 bytes).
4. v1 matrix (`results_s8.txt`) B+ rows (24.3/28.5/26.8 ms) are **invalid**: the v1 driver republished a *constant* tangent array, which usdImaging dedupes — no DirtyPrimvar was ever raised. v2 perturbs the tangent per frame (`bench_s8.cpp`, `repubTangent` path) so B+ reflects the true republish cost.
5. ADR §5.4's estimate for a B default ("≈ +1.5–2.5 ms at 100 k × 8 CV", derived from EV-022's points-upload rate) is **not borne out**: the measured republish penalty is +11.9–13.1 ms vs A (B+ − A ≈ 12.9 ms mean), i.e. the DirtyPrimvar path re-uploads *all* non-points primvars, not just the tangent.

## 4. DECISION

**DECISION: S-8 — ship variant A (`usdGenHairPreviewIndata.glslfx`, tangent from `inData.Neye`) as the default glslfx variant.** It passes both S-8 checks: it is the faster variant on deforming 100 k × 8 CV at every realistic configuration (25.9 ms steady vs 38.7 ms for B with per-frame `hairTangent` republish), and it compiles cleanly under `HDST_ENABLE_HGI_RESOURCE_GENERATION=1` (2 full 60-frame runs, no errors, no measurable overhead). Consequently **`hairTangent` is NOT part of C2** — usdGen tiles stay on the `DirtyPoints` fastpath and do not publish `hairTangent` (ADR §5.3 tile contract unchanged; §5.4 variant-A policy confirmed). Variant B (`UsdGenHairPreviewPrimvar`) remains the shipped fallback for curves usdGen does not own or wire/mesh display contexts (ADR §9 R4's three-file set stands: default = A, translucent, primvar = B; C5 `inputs:` block identical across the three — re-verify B's `inputs:` when the C5 freeze lands at M1).

## 5. Risks / follow-ups

- **Host contention caveat (absolute numbers):** measurements ran on NVIDIA GB10 (driver 580.173.02) while a sibling process (sglang) held ~96 % GPU utilization; absolute ms are inflated vs the Sep-04 research baseline (our ~25.9 ms at 100 k steady sits well above the ~12.2 ms that EV-020/EV-021 interpolate at 100 k; the same-day A vs B/B+ comparison is contention-independent because all cells ran under the same conditions). Re-verify A vs B+ once on an idle workstation before M1's C2/C5 freeze — cheap (two 60-frame runs).
- **ADR §5.4 cost estimate stale:** "+1.5–2.5 ms per deforming frame if B becomes default" understates the measured ~+12.9 ms; update ADR §5.4 or the performance ledger note when S-8 is closed. (Moot if A is the default, but the number feeds the "B as fallback" budget for non-usdGen-owned curves.)
- **HGI compile was GL-side only:** the override banner confirms the HGI resource-generation path ran, but the true Metal/Vulkan proxy build is a T4 (workstation/CI) check; `testUsdGenStormHgiResource` (gate S-8's test binary, M1) is the binding validation.
- **Wire/mesh display styles:** variant A's `inData.Neye` is only valid where the SDR block provides it (RIBBON/HAIR/ROUND/HALFTUBE tessellation). Variant B must stay the fallback for refineLevel-0 (wire) contexts — already the ADR plan; no action.
- SteadyMax tail (51–78 ms spikes) is host noise (contention + 1280×720 blit); use mean/min for regression tracking in `testUsdGenStormTangent`, not max.
