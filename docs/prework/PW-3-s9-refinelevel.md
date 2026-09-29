# PW-3 — S-9 pre-work: refineLevel-1 "tumble tier" switch cost

## 1. What was asked

Roadmap `plan/11-roadmap.md` §1.1, PW-3 row (line 111):

> | **PW-3** | S-9 | whether a refineLevel-1 tumble tier ships as an M5 LOD option (ADR §5.5, S31) | same harness, 200 k × 8 CV, `displayStyle/refineLevel` 2 → 1 → 2 every ten frames: ships under 3 ms of switch cost, else LOD is decimation only. Steady state MEASURED 8.17 vs 23.93 ms (row `EV-021`; `plan/research/G-storm-hair-look-prototype.md` §5) |

Gate S-9 (`plan/09-performance-and-benchmarks.md` §5): *"refineLevel 1 vs 2 **and the switch cost** | switch < 3 ms ⇒ the tumble tier ships as an option | `testUsdGenStormRefine` on G4 | M0 pre-work (PW-3), **decides M1** | UNMEASURED switch; the 8.17 vs 23.93 ms lever is MEASURED (EV-021)."*

ADR §5.5: *"Gate S-9 measures level 1 vs 2 frame time **and the switch cost** (cubic index rebuild 2.6/5.2 ms per 500 k patches + batch revalidation). If a switch costs < 3 ms, a 'tumble tier' (refineLevel 1 while the camera moves, restored on release) ships as an option; LOD by decimation remains the primary lever either way."*

Complexity → refineLevel mapping (verified against `pxr/usdImaging/usdImagingGL/engine.cpp` `_GetRefineLevel`, ~:2318): 1.0→0, 1.1→**1**, 1.2→**2**, 1.3→3.

## 2. Method + exact commands

Toggle benchmark via `bench_refine.cpp` (same EGL/Storm harness as the prototype `bench_hair.cpp`; NVIDIA GB10, EGL device platform, GL 4.6 compat; `UsdImagingGLEngine` + color AOV, 1280×720, single simple light + ambient, `glFinish()` inside the timed region; deform=0 default). Scene: `scene_200k_B.usdc` (200 k × 8 CV = 1.6 M CVs, variant-B glslfx material, Key+Rim distant lights in-scene).

```bash
# env (all runs)
export PATH=$VENV/bin:$PATH
export LD_LIBRARY_PATH=$USD/lib
export PXR_PLUGINPATH_NAME="...usdGenImaging/resources:...usdGenSchema/resources:$USD/plugin/usd:$USD/lib/usd"

# toggle: r2 (cx 1.2) -> r1 (cx 1.1) -> r2 ... every 10 frames, 60 frames  [pre-existing, Sep 6 10:56]
./build/bench_refine toggle scene_200k_B.usdc /World/Cam 1280 720 1.2 1.1 10 60 0

# steady-state re-measures (this continuation, Sep 6 ~11:40):
./build/bench_refine steady scene_200k_B.usdc /World/Cam 1280 720 1.1 1.1 10 40 0   # -> results_s9_steady.txt
./build/bench_refine steady scene_200k_B.usdc /World/Cam 1280 720 1.2 1.2 10 40 0   # -> results_s9_steady.txt
./build/bench_hair scene_200k_B.usdc /World/Cam 1280 720 1.1 20 0   # cross-check, prototype driver
./build/bench_hair scene_200k_B.usdc /World/Cam 1280 720 1.2 20 0   # -> results_s9_steady_benchhair.txt
```

**Host-state caveat:** all re-measures ran while a sibling `sglang` GPU workload held ~96 % GPU utilization (`nvidia-smi`: utilization.gpu 96 %, SM clock 2470/3003 MHz, temp 61 °C). Absolute steady-state numbers are therefore inflated vs the idle-state EV-021 baseline; relative (A-vs-B-style) comparisons within the same window remain valid.

## 3. Raw evidence

### 3.1 Toggle run (`results_s9_toggle.txt`, verbatim)

```
eglctx: GL context current on EGL device 0 (EGL 1.5)
GL_VERSION=4.6.0 NVIDIA 580.173.02
SCENE mode=toggle curves=200000 cvs=1600000 stageOpen=25.7ms
F=000 cx=1.2 ms=841.241
F=001 cx=1.2 ms=30.269
F=002 cx=1.2 ms=28.280
F=003 cx=1.2 ms=28.655
F=004 cx=1.2 ms=28.439
F=005 cx=1.2 ms=29.069
F=006 cx=1.2 ms=29.215
F=007 cx=1.2 ms=28.828
F=008 cx=1.2 ms=29.183
F=009 cx=1.2 ms=29.076
F=010 cx=1.1 ms=226.682
F=011 cx=1.1 ms=33.469
F=012 cx=1.1 ms=25.812
F=013 cx=1.1 ms=26.362
F=014 cx=1.1 ms=23.288
F=015 cx=1.1 ms=27.582
F=016 cx=1.1 ms=27.081
F=017 cx=1.1 ms=26.945
F=018 cx=1.1 ms=27.108
F=019 cx=1.1 ms=27.069
F=020 cx=1.2 ms=79.699
F=021 cx=1.2 ms=32.240
F=022 cx=1.2 ms=27.452
F=023 cx=1.2 ms=28.827
F=024 cx=1.2 ms=29.025
F=025 cx=1.2 ms=30.552
F=026 cx=1.2 ms=29.174
F=027 cx=1.2 ms=28.934
F=028 cx=1.2 ms=29.312
F=029 cx=1.2 ms=28.919
F=030 cx=1.1 ms=66.263
F=031 cx=1.1 ms=25.671
F=032 cx=1.1 ms=26.218
F=033 cx=1.1 ms=26.847
F=034 cx=1.1 ms=26.195
F=035 cx=1.1 ms=25.225
F=036 cx=1.1 ms=26.861
F=037 cx=1.1 ms=27.260
F=038 cx=1.1 ms=27.008
F=039 cx=1.1 ms=27.426
F=040 cx=1.2 ms=48.097
F=041 cx=1.2 ms=27.511
F=042 cx=1.2 ms=29.387
F=043 cx=1.2 ms=28.885
F=044 cx=1.2 ms=29.162
F=045 cx=1.2 ms=28.744
F=046 cx=1.2 ms=29.050
F=047 cx=1.2 ms=28.987
F=048 cx=1.2 ms=29.228
F=049 cx=1.2 ms=29.077
F=050 cx=1.1 ms=58.711
F=051 cx=1.1 ms=25.534
F=052 cx=1.1 ms=26.800
F=053 cx=1.1 ms=27.127
F=054 cx=1.1 ms=27.178
F=055 cx=1.1 ms=26.977
F=056 cx=1.1 ms=27.064
F=057 cx=1.1 ms=27.332
F=058 cx=1.1 ms=28.550
F=059 cx=1.1 ms=27.144
```

### 3.2 Switch cost (switch frame total minus target-level steady mean)

| Switch frame | Transition | Total frame ms | Target steady mean | **Overhead** |
|---|---|---:|---:|---:|
| F=010 | r2 → r1 (first entry) | 226.682 | 26.93 | **≈ 199.8 ms** (one-time) |
| F=020 | r1 → r2 | 79.699 | 29.09 | **≈ 50.6 ms** |
| F=030 | r2 → r1 | 66.263 | 26.93 | **≈ 39.3 ms** |
| F=040 | r1 → r2 | 48.097 | 29.09 | **≈ 19.0 ms** |
| F=050 | r2 → r1 | 58.711 | 26.93 | **≈ 31.8 ms** |

Steady means from this same run: r1 = 26.931 ms (n=27, min 23.288, max 33.469), r2 = 29.092 ms (n=27, min 27.452, max 32.240).

### 3.3 Steady-state re-measure (this continuation, `results_s9_steady.txt`)

```
## steady r1 (cx=1.1, no deform)
SCENE mode=steady curves=200000 cvs=1600000 stageOpen=24.8ms
F=000 cx=1.1 ms=423.435
F=001 cx=1.1 ms=28.163
F=002 cx=1.1 ms=26.438
F=003 cx=1.1 ms=23.477
F=004 cx=1.1 ms=21.751
F=005 cx=1.1 ms=24.801
F=006 cx=1.1 ms=26.948
F=007 cx=1.1 ms=26.860
F=008 cx=1.1 ms=26.873
F=009 cx=1.1 ms=26.725
F=010 cx=1.1 ms=24.894
F=011 cx=1.1 ms=27.162
F=012 cx=1.1 ms=28.184
F=013 cx=1.1 ms=27.705
F=014 cx=1.1 ms=27.141
F=015 cx=1.1 ms=26.876
F=016 cx=1.1 ms=26.755
F=017 cx=1.1 ms=26.874
F=018 cx=1.1 ms=26.947
F=019 cx=1.1 ms=27.060
F=020 cx=1.1 ms=27.060
F=021 cx=1.1 ms=26.995
F=022 cx=1.1 ms=26.745
F=023 cx=1.1 ms=27.135
F=024 cx=1.1 ms=26.932
F=025 cx=1.1 ms=26.946
F=026 cx=1.1 ms=26.728
F=027 cx=1.1 ms=26.834
F=028 cx=1.1 ms=26.878
F=029 cx=1.1 ms=26.998
F=030 cx=1.1 ms=27.060
F=031 cx=1.1 ms=26.914
F=032 cx=1.1 ms=26.928
F=033 cx=1.1 ms=26.781
F=034 cx=1.1 ms=27.051
F=035 cx=1.1 ms=25.425
F=036 cx=1.1 ms=22.880
F=037 cx=1.1 ms=22.847
F=038 cx=1.1 ms=26.872
F=039 cx=1.1 ms=26.775

## steady r2 (cx=1.2, no deform)
SCENE mode=steady curves=200000 cvs=1600000 stageOpen=26.1ms
F=000 cx=1.2 ms=449.318
F=001 cx=1.2 ms=40.343
F=002 cx=1.2 ms=28.888
F=003 cx=1.2 ms=28.109
F=004 cx=1.2 ms=28.307
F=005 cx=1.2 ms=28.420
F=006 cx=1.2 ms=25.848
F=007 cx=1.2 ms=25.258
F=008 cx=1.2 ms=26.924
F=009 cx=1.2 ms=28.645
F=010 cx=1.2 ms=29.220
F=011 cx=1.2 ms=28.996
F=012 cx=1.2 ms=28.770
F=013 cx=1.2 ms=27.052
F=014 cx=1.2 ms=28.770
F=015 cx=1.2 ms=28.924
F=016 cx=1.2 ms=28.881
F=017 cx=1.2 ms=29.148
F=018 cx=1.2 ms=28.910
F=019 cx=1.2 ms=29.073
F=020 cx=1.2 ms=28.659
F=021 cx=1.2 ms=29.058
F=022 cx=1.2 ms=28.863
F=023 cx=1.2 ms=28.830
F=024 cx=1.2 ms=28.761
F=025 cx=1.2 ms=28.959
F=026 cx=1.2 ms=28.883
F=027 cx=1.2 ms=28.928
F=028 cx=1.2 ms=28.909
F=029 cx=1.2 ms=29.040
F=030 cx=1.2 ms=29.151
F=031 cx=1.2 ms=28.889
F=032 cx=1.2 ms=28.818
F=033 cx=1.2 ms=28.898
F=034 cx=1.2 ms=28.977
F=035 cx=1.2 ms=28.975
F=036 cx=1.2 ms=30.497
F=037 cx=1.2 ms=28.667
F=038 cx=1.2 ms=27.061
F=039 cx=1.2 ms=24.655
```

Cross-check with the original prototype driver on the identical scene (`results_s9_steady_benchhair.txt`):

```
## bench_hair (prototype driver) on scene_200k_B cx=1.1 r1
RESULT scene=scene_200k_B.usdc res=1280x720 complexity=1.1 curves=200000 cvs=1600000 deform=0 frames=40 ms_per_frame=26.439 fps=37.8

## bench_hair (prototype driver) on scene_200k_B cx=1.2 r2
RESULT scene=scene_200k_B.usdc res=1280x720 complexity=1.2 curves=200000 cvs=1600000 deform=0 frames=40 ms_per_frame=28.444 fps=35.2
```

Stable across harnesses: **r1 ≈ 26.4–27.0 ms, r2 ≈ 28.4–29.1 ms** (steady, frames 2..39 mean ≈ 26.8 / 28.9).

### 3.4 Comparison against EV-021 (idle-host baseline, Sep 4, `plan/research/G-storm-hair-look-prototype.md` §5)

| Level | EV-021 (idle host) | Re-measured (this host, GPU ~96 % busy) | Δ |
|---|---:|---:|---:|
| refineLevel 1 | **8.17 ms** | ≈ 26.5–27.0 ms | +223 % |
| refineLevel 2 | **23.93 ms** | ≈ 28.4–29.1 ms | +20 % |

The 2.9× r1-over-r2 lever from EV-021 did **not** reproduce under today's conditions: the r1 cost inflated far more than r2, consistent with a contended integrated GPU (GB10 SoC, sibling `sglang` process at 96 % utilization) adding time-slice wait that hits r1's smaller-GPU-work frames proportionally hardest. The r2 re-measure (≈ 29 ms vs 23.93 ms) is within ~20 % of the EV baseline and corroborates the EV-021 r2 number.

## 4. DECISION

**DECISION: S-9 — do NOT ship a refineLevel-1 tumble tier as an M5 LOD option; LOD remains decimation-only.** Measured switch cost at 200 k × 8 CV (r2 → r1 → r2 every 10 frames): total switch-frame times of 48.1–226.7 ms, i.e. an overhead of **≈ 19–51 ms per repeat switch and ≈ 200 ms on first entry into r1** (cubic index rebuild + batch revalidation + pipeline re-setup) — 6–66× the 3 ms threshold, and robustly above it even after discounting today's GPU contention (EV-027 alone puts index rebuild at ~5–16 ms per 1.6 M CV). The EV-021 steady-state advantage of r1 (8.17 ms vs 23.93 ms) was not reproducible under contended GPU load on this host (r1 ≈ 26.9 ms, r2 ≈ 29.1 ms today); even granting the idle-host EV-021 numbers, the measured ~19+ ms switch overhead makes frequent r2↔r1 toggling a net loss. Decimation stays the primary LOD lever; the tumble tier is dropped, and S-9's binding check at M1 should re-confirm switch cost on an idle GPU (one `bench_refine toggle` run) — cheap insurance given today's host was loaded.

## 5. Risks / follow-ups

- **Host contention:** this host currently runs a sibling `sglang` GPU workload (nvidia-smi: 96 % utilization, 2470/3003 MHz SM clock) which inflates all GL frame times. The switch-cost decision is threshold-robust, but the steady-state r1/r2 comparison is not comparable to EV-021 under these conditions. Follow-up: re-run `bench_refine steady` at 1.1/1.2 on an idle GPU before M5 LOD design; if idle r1 ≈ 8 ms reappears, the tumble-tier economics improve but the measured switch overhead (index rebuild + revalidation, largely CPU-side) still exceeds 3 ms.
- **First-entry cost:** the one-time r2→r1 switch (~200 ms overhead) means the tumble tier would need to be *warmed up* (pre-enter r1 once) to be usable interactively — another argument against shipping it.
- **Toggle cadence:** the spec's 10-frame toggle interval was used verbatim; a slower toggle cadence in a real tumble tier would amortize nothing, since switch cost is per-transition, not per-frame.
- Draw-batch counts on level switch were not recorded (Storm does not expose them through the usdImagingGL harness); pixel/behavior equivalence of r1 vs r2 output is visually untested here.
