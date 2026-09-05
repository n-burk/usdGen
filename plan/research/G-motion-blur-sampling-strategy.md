# G — Motion-blur sampling strategy for generated hair (gap fill)

Scope: decide and verify how usdGen's output prims (basisCurves minted or overlaid by the hair scene index) should answer Hydra's sampled-data-source protocol so that (1) Storm never pays for motion samples, (2) hdPrman-class renderers get correct deformation blur, and (3) the hair graph is not evaluated N times per frame unless that is actually what the renderer asked for. All paths are absolute; `OpenUSD` = `/home/burkard/work/OpenUSD` (v26.08 source), `usdRig` = `/home/burkard/work/usdRig`, `hdPrman` = `OpenUSD/third_party/renderman/plugin/hdPrman`.

Decision in one line: **serve motion from a retained per-offset point cache that is filled lazily on the first `GetContributingSampleTimesForInterval(start,end)` pull (or eagerly by an app-supplied preflight profile), emit velocities only as an explicit cheap "linear" profile, and never evaluate the whole styler chain per sample — evaluate only the deform-transport tail per sample.** Rationale and evidence follow.

---

## 1. Who asks for samples, and with what interval (question a)

### 1.1 The contract every consumer follows

`HdSampledDataSource` (`OpenUSD/pxr/imaging/hd/dataSource.h:196-228`):

- `GetValue(Time shutterOffset)` — "frame-relative time"; "the scene index producing this datasource is responsible for [tracking the frame]"; "expected to be threadsafe" (:203-210).
- `GetContributingSampleTimesForInterval(start, end, outSampleTimes)` — returned samples "don't need to be within startTime and endTime"; a boundary sample outside the window may be returned and "callers should expect it and interpolate"; returning `false` means "uniform across the shutter window and the caller should call GetValue(0)" (:212-228).

Consequence for usdGen: a points source may legally answer with whole-frame bracketing offsets (e.g. `[-1, 0, +1]`) even when asked for `[-0.25, 0.25]`; hdPrman accepts and resamples that (see 1.4).

### 1.2 Storm (the viewport) never asks for more than one sample

| Fact | Evidence |
|---|---|
| No `SamplePrimvar`/`SampleTransform`/`GetContributingSampleTimesForInterval` call anywhere in hdSt (non-test) | `grep` over `OpenUSD/pxr/imaging/hdSt` returns only the velocity SI plugin files (`velocityMotionResolvingSceneIndexPlugin.{h,cpp}`) |
| Storm reads points straight from the scene-index data source at offset 0 | `OpenUSD/pxr/imaging/hdSt/basisCurves.cpp:975-990` — `pointsSchema.GetPrimvarValue()` then `value = valueDs->GetValue(0.0f);` |
| Other vertex/varying primvars go through the delegate's `GetPrimvar`, which is also `GetValue(0.0f)` | `basisCurves.cpp:917,1112,1200`; `OpenUSD/pxr/imaging/hd/sceneIndexAdapterSceneDelegate.cpp:2296-2312` (`valueDataSource->GetValue(0.0f)`) |
| The delegate's no-interval `SamplePrimvar` overloads use a **[0,0]** fallback interval | `sceneIndexAdapterSceneDelegate.cpp:142-143` (`_fallbackStartTime = 0.0f; _fallbackEndTime = 0.0f`), used at `:2325, :2350, :2511, :2762` |

So the "looks like the render" requirement cannot include motion blur in Storm at all; the viewport profile is single-sample by construction and must cost exactly one `GetValue(0)` per publish.

### 1.3 The adapter delegate's multi-sample path (what any Hydra-1.0-style renderer sees)

`HdSceneIndexAdapterSceneDelegate::_SamplePrimvar` (`sceneIndexAdapterSceneDelegate.cpp:2369-2482`):

```
valueSource = primvars.GetPrimvar(key).GetPrimvarValue()          // :2380-2390
isVarying = valueSource->GetContributingSampleTimesForInterval(startTime, endTime, &times)   // :2437-2439
if (!isVarying) times = {0.0f}                                     // :2450
if (times.size() > maxSampleCount) times.resize(maxSampleCount)    // :2455-2457
for i: sampleTimes[i] = times[i]; sampleValues[i] = valueSource->GetValue(times[i])   // :2459-2461
```

- Interval comes from the render delegate (`SamplePrimvar(id,key,startTime,endTime,...)`, `:2331-2341`); legacy (emulated-delegate) prims get `[lowest, max]` (`:2427-2435`).
- The `HdTimeSampleArray<VtValue, CAPACITY>` convenience wrapper calls twice if `authoredSamples > CAPACITY` (`OpenUSD/pxr/imaging/hd/sceneDelegate.h:1144-1175`, `TF_VERIFY(authoredSamples == authoredSamplesSecondAttempt)`) — the sample count must be stable across calls within one Sync.
- `_GetInputPrim` caches one `GetPrim` result per thread (`:182-190`): the prim container is fetched once per Sync per thread, but `GetValue(t)` is called once per sample time.
- Same protocol for xform (`SampleTransform`, `:2516-2565`) and ext-computation inputs (`:2767-2830`).

### 1.4 hdPrman: the only in-tree renderer that performs deformation blur

| Step | Evidence |
|---|---|
| Shutter comes from the camera's frame-relative `shutter:open/close` (UsdGeomCamera defaults **0.0/0.0** = no blur) unless `disableMotionBlur`/`instantaneousShutter` render settings override | `hdPrman/renderParam.cpp:4515-4560`; `OpenUSD/pxr/usd/usdGeom/schema.usda:2394-2404` |
| The resolved interval is stored in `_shutterInterval` and pushed to a **static** in the motion-blur SI plugin | `renderParam.cpp:5099-5110` → `HdPrman_MotionBlurSceneIndexPlugin::SetShutterInterval` (`motionBlurSceneIndexPlugin.cpp:1129-1136`, statics at `:64-67`, defaults `HDPRMAN_SHUTTEROPEN/CLOSE_DEFAULT 0.f` in `renderParam.h:72-73`) |
| basisCurves points: `HdPrman_ConvertPointsPrimvar(..., renderParam->GetShutterInterval(), ...)` → `SamplePrimvar(id, points, shutter[0], shutter[1], &boxedPoints)` with `HDPRMAN_MAX_TIME_SAMPLES` = 16 (or 4) | `hdPrman/basisCurves.cpp:130-134`; `renderParam.cpp:352-376`; `renderParam.h:67-69` |
| All N point samples are handed to Riley: `primvars.SetTimes(shutterTimes)` + one `SetPointDetail(k_P, ..., i)` per sample; samples whose size mismatches are dropped | `renderParam.cpp:428-444` |
| Every **non-P** primvar is collapsed to one sample by `samples.Resample(time)` ("HdPrman also does not yet support time-sampled primvars other than P") | `renderParam.cpp:814-847` |
| The motion-blur SI **ignores the interval it is passed** and uses `_shutterOpen*blurScale .. _shutterClose*blurScale` | `motionBlurSceneIndexPlugin.cpp:265-268` (comment "We *ONLY* use the interval coming to us from RenderParam::SetRileyOptions()"), `:344-345` |
| It only blurs `points` (+ instancer primvars) and `xform/matrix`; other primvars are forced to a single offset-0 sample | `_IsBlurablePrimvar` `:227-246`; `_PrimvarsDataSource::Get` comment `:747-756` |
| It requires the upstream source to return `true` **and ≥2 times**, then checks that `GetValue(t).GetArraySize()` is identical at every returned time (an extra `GetValue` per sample!) | `:349-380` |
| It can re-distribute samples to `ri:object:geosamples` evenly between the first and last returned time, and un-scales `blurScale` | `:382-410` |
| `GetValue(t)` forwards to the source at `t*blurScale` | `:434-452` |
| Plugin order: velocity SI at `hdPrman:phase2` (`lastBefore` phase3/motionBlur), motion-blur SI at `hdPrman:phase3` (`firstAfter` phase2), both `loadWithRenderer` RIS/XPU only | `hdPrman/plugInfo.json:35-60` |

hdEmbree declares `motionBlurSupport=false` (`OpenUSD/pxr/imaging/plugin/hdEmbree/rendererPlugin.cpp:42-48`) and only samples light transforms (`hdEmbree/light.cpp:129`) — no deformation blur; the capability bit lives in `HdSceneIndexCreateArgsSchema` (`OpenUSD/pxr/imaging/hd/sceneIndexCreateArgsSchema.h:40-41,88-95`), which usdRig already reads as `MotionBlurSupport` (`usdRig/libs/rigExecImaging/bridge.h:48`).

**Answer (a):** The hair points data source is asked, per Sync, `GetContributingSampleTimesForInterval(open·blurScale, close·blurScale)` where open/close are the camera's frame-relative shutter (Storm: never; hdPrman: once via its SI, plus the ordinality check), followed by `GetValue(t)` for each returned `t` — at least twice per sample time under hdPrman (`:365-373` check + delegate loop `:2459-2461`). The source must therefore **return retained arrays** (VtArray copy-on-write, no recompute) and must produce a **stable sample list** within a Sync.

---

## 2. Velocities: are they honoured for basisCurves, and must they be blocked (question b)

### 2.1 `HdsiVelocityMotionResolvingSceneIndex` semantics (`OpenUSD/pxr/imaging/hdsi/velocityMotionResolvingSceneIndex.{h,cpp}`)

| Rule | Evidence |
|---|---|
| Supported prim types include `basisCurves` (also points, nurbsCurves, nurbsPatch, tetMesh, mesh, instancer) | `.cpp:710-722` (`PrimTypeSupportsVelocityMotion`) |
| Affected primvar for geometry is `points` only; `velocities`/`accelerations` are read from `primvars/<name>/primvarValue` on the **input** prim container | `.cpp:60-69`; `_VelocityMotionValidForCurrentFrame` `.cpp:333-345` |
| Validity: velocities data source must exist and be `VtVec3fArray`; source and velocities must share the same left-bracketing frame-relative sample time (a source with **no** samples is treated as sample time 0); `velocities.size() >= points.size()` | `.cpp:356-405` |
| When valid, sample times reported are exactly `{startTime, endTime}` plus `max(3, nonlinearSampleCount)-1` interior times if accelerations exist | `.cpp:176-190` |
| `GetValue(t)` = `P(sampleTime) + ((t - sampleTime)/tcps)·V (+ ½·Δt²·A)` | `.cpp:236-263` |
| `timeCodesPerSecond` comes from `HdSceneGlobalsSchema` on the input scene (UsdImaging publishes the stage's value, `OpenUSD/pxr/usdImaging/usdImaging/dataSourceStage.cpp:69-71`); the `fps` inputArg Storm/hdPrman pass is **ignored** (`New(..., /* inputArgs */)` `.cpp:685-696`), fallback 24 (`.cpp:57`) | |
| Per-prim override token `__velocityMotionMode` ∈ {enable, disable, ignore, noAcceleration}, authored by an upstream SI | `.h:29-35, 72-88`; hdPrman maps `ri:object:vblur` onto it (`hdPrman/velocityMotionResolvingSceneIndexPlugin.cpp:68-76`, `_VblurInterpretingSceneIndex`) |
| Dirtying: a dirty `velocities`/`accelerations`/`nonlinearSampleCount`/`__velocityMotionMode` is widened to dirty `points`; a dirty scene-globals `timeCodesPerSecond` dirties every supported prim with `UniversalSet` | `.cpp:753-855` |

Installed for Storm at `hdSt:phase0`, `lastBefore hdSt:phase1` (`OpenUSD/pxr/imaging/hdSt/plugInfo.json`, `HdSt_VelocityMotionResolvingSceneIndexPlugin` block; phase 0 `InsertionOrderAtEnd` in `hdSt/velocityMotionResolvingSceneIndexPlugin.cpp:36-45`). Because usdRig's (and usdGen's) scene indices are appended by `UsdImagingSceneIndexPlugin` during `UsdImagingCreateSceneIndices`, before any renderer plugin SI (`usdRig/libs/rigExecImaging/sceneIndexPlugin.cpp:23-45`; A2 report), the velocity SI always sits **downstream** of the hair SI in both Storm and hdPrman.

### 2.2 What that means for hair

- **Honoured:** yes. A hair basisCurves prim carrying `primvars/points` (retained, no time samples) plus `primvars/velocities` (retained, same size) satisfies the validity test with `sampleTime = 0` (`.cpp:369-381`), so hdPrman's phase-2 SI turns it into `{open, close}` samples `P + (t/tcps)·V`, and the phase-3 motion-blur SI sees two equal-size samples and blurs (`motionBlurSceneIndexPlugin.cpp:349-380`).
- **Free in Storm:** at offset 0 with `sampleTime = 0`, `scaledTime = 0` (`.cpp:243-246`), so the velocity SI returns `P` unchanged; the cost is two locator lookups per pull. Storm only benefits when the stage time is between authored USD samples (`sampleTime < 0`), which never happens for retained hair.
- **Blocking is a correctness rule, not a stylistic one.** usdRig blocks `velocities` and `accelerations` whenever it owns `points` (`usdRig/libs/rigExecImaging/sceneIndices.cpp:1356-1363`; dirties them too, `:2473-2477`). The reason is visible in the validity test: an upstream curve prim (e.g. a cached sim with authored per-frame velocities) overlaid with retained SI points would still pass the check (both left-bracketing times resolve to 0) and prman would extrapolate the **stale** upstream velocity from the **new** points. Blocking works because `HdOverlayContainerDataSource` returns `nullptr` for a child that is an `HdBlockDataSource` (`OpenUSD/pxr/imaging/hd/overlayContainerDataSource.cpp:94-97`; block definition `dataSource.h:267-282`), and `HdContainerDataSource::Get(container, locator)` stops at a null child (`dataSource.cpp:37-46`), so the velocity SI sees "No velocities" (`.cpp:346-352`).
- **Rule for usdGen:** when the hair SI owns `points` on a prim that pre-exists upstream (static-curve deform mode), it must either block `velocities`+`accelerations` or replace them with velocities consistent with its own points. For prims the SI mints itself there is nothing to block, but the SI must still not leave a `velocities` entry unless it is intentionally in the "linear" profile (section 4).

---

## 3. What the upstream surface actually returns (question c)

### 3.1 UsdImaging surfaces

`UsdImagingDataSourceAttribute<T>` (`OpenUSD/pxr/usdImaging/usdImaging/dataSourceAttribute.h`):

- `GetTypedValue(offset)` evaluates the attribute at `stageGlobals.GetTime() + offset` (`:41-50`) — so **any** offset is answerable; USD interpolates between authored samples.
- `GetContributingSampleTimesForInterval` returns `false` unless `ValueMightBeTimeVarying()` and the time is numeric (`:80-84`); otherwise it returns the authored samples inside `[t+start, t+end]` plus the **bracketing** samples on either side (`:86-113`), converted to frame-relative floats (`:118-122`), `true` iff more than one (`:125`).
- Practical result: a mesh animated per frame and a shutter of `[-0.25, 0.25]` answers `[-1, 0, +1]` — three whole-frame offsets, two of which lie outside the window. hdPrman accepts this (§1.1) and, absent `ri:object:geosamples`, will ask for exactly those three offsets (`motionBlurSceneIndexPlugin.cpp:382-386`).
- Time changes: `UsdImagingStageSceneIndex::SetTime` sends one dirty per flagged time-varying locator (`OpenUSD/pxr/usdImaging/usdImaging/stageSceneIndex.cpp:357-372, 905-916`); attributes flag themselves at construction (`dataSourceAttribute.h:233-243`). The hair SI therefore receives `primvars/points/primvarValue` dirties from the surface each frame and must not re-evaluate until pulled.

### 3.2 RigExec surfaces

- Multi-sample only when the app called `EvaluateAndPublishSamples(baseTime, shutterOffsets, support)` (`usdRig/libs/rigExecImaging/bridge.cpp:1257-1364`): the rig is evaluated once per offset (`:1284-1287`), all samples are captured before one atomic publish (`:1279-1282, 1348-1349`), and preflight refuses `>1` offset when `motionBlurSupport == False` (`:1233-1254`).
- The published data source reports the retained offsets when `>= 2` (`sceneIndices.cpp:306-312`, `return false` below two) and `GetValue(t)` returns the **nearest** retained sample, never interpolating (`:292-303`). The ordinary per-frame path (`EvaluateAndPublish`) publishes one sample and the source answers `false`.
- Nothing in the shipped usdview plugin calls `EvaluateAndPublishSamples`; only tests do (`usdRig/tests/testRigExecImaging.cpp:506-540, 984-1000`, offsets `{-0.5, 0, 0.5}`). So today a hair graph sitting on a RigExec surface gets **one** sample unless the host app drives the sampled path.

### 3.3 UsdSkel surfaces (and any ext-computation-backed modifier)

- Skinned points are published as **ext computations**, not plain primvars, unless `HD_ENABLE_DEFERRED_SKINNING` is set (`OpenUSD/pxr/usdImaging/usdSkelImaging/pointsResolvingSceneIndex.h:15-20`; `dataSourceResolvedPointsBasedPrim.cpp:1086-1100`; env setting `OpenUSD/pxr/imaging/hd/skinningSettings.cpp:15,29-34`).
- A downstream SI can only read them as ordinary sampled points after `HdSiExtComputationPrimvarPruningSceneIndex` (`OpenUSD/pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h:19-34`), which hdPrman inserts at `hdPrman:phase0` (`hdPrman/plugInfo.json:168-178`) — i.e. **after** the app-level hair SI. Storm does not insert it (GPU skinning). Sample times for skinning transforms are the union of the animation's translation/rotation/scale samples plus `{start, 0, end}` (`dataSourceResolvedSkeletonPrim.cpp:62-108`).
- Consequence: usdGen must either evaluate the skinning ext computation itself (CPU `HdExtComputationUtils`) or document that skel-driven surfaces require the pruning SI ahead of it. Flagged as an open question; not needed for RigExec/UsdImaging surfaces.

### 3.4 The reference forwarding pattern

Hydra's own procedural test forwards sample times from an input points source and evaluates lazily per offset (`OpenUSD/pxr/usdImaging/usdImagingGL/testenv/TestUsdImagingGLHdGpProcedurals.cpp:436-478`: `GetContributingSampleTimesForInterval` → `_pointsDs->GetContributingSampleTimesForInterval(...)`, `GetTypedValue(t)` → `_pointsDs->GetValue(t)`). The multi-input union helper is `HdGetMergedContributingSampleTimesForInterval(count, sources, start, end, out)` (`OpenUSD/pxr/imaging/hd/dataSource.h:389`, `.cpp:75-110`), used by usdSkelImaging (`dataSourceResolvedSkeletonPrim.cpp:76-77`).

---

## 4. Decision: the usdGen motion profile and data-source API

### 4.1 Three profiles, one data source

| Profile | When | `GetContributingSampleTimesForInterval` | `GetValue(t)` | Evaluations per frame |
|---|---|---|---|---|
| **P0 single** (default; Storm; hdEmbree; prman with shutter 0/0) | no motion requested | `false` | retained offset-0 points | 1 × chain |
| **P1 linear** (opt-in per groom: `usdGen:motion:mode = velocities`) | renderer supports blur, artist accepts linear blur | `false`; SI additionally publishes `primvars/velocities` (and blocks/overrides upstream ones) | retained offset-0 points; the downstream velocity SI extrapolates | 1 × chain + 1 velocity pass (see §5) |
| **P2 sampled** (`usdGen:motion:mode = samples`, or app preflight) | prman-class blur; explicit offsets | `true` + retained offsets (≥2) | nearest/lerp of retained per-offset arrays | k × transport tail (+ 1 × chain), see §4.3 |

Only one of P1/P2 may be active on a prim: when P2 publishes samples the SI must block `velocities`/`accelerations` exactly as usdRig does (`sceneIndices.cpp:1356-1363`), because a valid velocities entry would make the downstream velocity SI ignore the samples' interior and re-linearise (`velocityMotionResolvingSceneIndex.cpp:176-190` replaces the sample list with `{start,end}`).

### 4.2 Where the offsets come from (P2)

Two sources, both supported by the same cache:

1. **App preflight (usdRig-style, deterministic):** `UsdGenImagingBridge::EvaluateAndPublishSamples(baseTime, offsets, MotionBlurSupport)` mirroring `bridge.h:48-72` / `bridge.cpp:1233-1364`. The render tool (usdview plugin or batch driver) reads the render camera's `shutter:open/close` (the same values hdPrman resolves at `renderParam.cpp:4536-4541`) and any `ri:object:geosamples`, builds the offset list, evaluates every offset, publishes one generation. Pulls never evaluate.
2. **Lazy on first pull (needed for stock usdview + hdPrman where nobody calls preflight):** the first `GetContributingSampleTimesForInterval(start,end)` on a prim with `mode = samples` computes the offset list = union of `{start, end}` with the surface's own contributing times clamped to the window (or the raw surface list when `usdGen:motion:forwardSurfaceSamples = true`, which reproduces UsdImaging's `[-1,0,+1]`), evaluates the missing offsets under the graph mutex, retains them, and returns the list. Later `GetValue(t)` calls (prman's ordinality check `:365-373`, the delegate loop `:2459-2461`) hit the cache.

Lazy evaluation runs inside `HdRenderIndex::SyncAll`, which is multithreaded across prims; per A6 the exec system has one global time (`OpenExec ChangeTime`, A6 §3 row "One time per system"), so all per-offset evaluation must be serialised behind one mutex and keyed by a generation digest. Every prim of the same graph pulled in the same Sync waits once, then reads the cache.

### 4.3 Per-sample cost must not be the whole chain

The decisive design lever is **where time enters the graph**. In XGen/Houdini-style grooms the generator, clump, frizz, etc. run in surface rest/UV space and the deformed result is obtained by transporting the rest-space curve with the surface's per-point deformation (A7: Houdini's "skin-only / capture-and-deform" deform options). Under that split:

- Time-invariant head (distribution, seeds, stylers in rest space): evaluated once per edit, cached across frames and offsets.
- Time-varying tail (surface `points` at offset `t` → per-root frame → transport of N cvs, plus any world-space styler such as gravity/collide/sim): evaluated once per **offset**.

So P2 cost = 1 × head (amortised) + k × tail, not k × chain. Stylers that read deformed-space inputs (collision with world geometry, simulation) push themselves and everything downstream into the tail; the plan should mark each styler with `restSpace | deformedSpace` so the scheduler knows what has to be re-run per offset. usdRig itself does not have this lever (it re-runs the full rig per offset, `bridge.cpp:1284-1287`), which is why its spec keeps automatic shutter negotiation deferred (`usdRig/docs/spec.md:1673`).

### 4.4 Cache shape

- Key: `(graphGeneration, surfaceGeneration, absoluteTime = frame + offset)`; value: `VtVec3fArray` per output prim (+ per-sample extent). Keying by absolute time makes whole-frame offsets (`[-1,0,+1]`) reuse the previous frame's `+1` as this frame's `0` for free when scrubbing forward; sub-frame offsets (`±0.25`) never coincide and simply evict.
- Capacity: `k+1` samples per prim; 100k curves × 16 cvs = 1.6 M points = **19.2 MB per sample** (measured, §5), so k = 3 ≈ 58 MB per groom; k = 9 (prman `geosamples 9`) ≈ 173 MB. Storm (P0) holds one.
- Invalidation: any graph/surface dirty clears all offsets of that prim; the SI then emits `primvars/points/primvarValue` (+ `velocities`/`accelerations` when it owns them, + `extent`) exactly as usdRig does (`sceneIndices.cpp:2473-2477`). The hdPrman motion-blur SI forwards dirties untouched unless `blurScale`/`ri:object:*` change (`motionBlurSceneIndexPlugin.cpp:1038-1093`).
- Only `points` (and the prim `xform/matrix`) need samples; hdPrman collapses every other primvar to offset 0 (`renderParam.cpp:833-847`), so widths, normals, colours, `st`, and texture-driven primvars are single-sample retained sources (`false` from `GetContributingSampleTimesForInterval`).

### 4.5 Interpolation inside `GetValue(t)`

usdRig returns the nearest retained sample (`sceneIndices.cpp:292-303`). For hair, prefer **linear interpolation between the two bracketing retained offsets** when `t` is not exactly a retained offset (prman re-distributes to `geosamples` between the first and last time, `motionBlurSceneIndexPlugin.cpp:391-410`, so it will ask for non-retained times) and clamp outside; the cost is one memory-bound pass (§5).

---

## 5. Cost model (question d) — floors measured, chain cost to be measured on the prototype

Measured on this machine (`Cortex-X925`, single thread, `g++ -O2`, scratch file `mbbench.cpp` in the scratchpad), 1.6 M `float3` points (100k curves × 16 cvs):

| Operation over 1.6 M points | Time |
|---|---|
| `P + (t/fps)·V` (velocity extrapolation of one sample) | 0.97 ms |
| `lerp(P0, P1, a)` (interpolate between two retained offsets) | 0.99 ms |
| `(P1 − P0)·fps` (finite-difference velocity from two evaluations) | 1.00 ms |
| copy one sample (`VtArray` detach) | 0.49 ms |
| bytes per sample | 19.2 MB |

Model, with `H` = time-invariant head, `T` = per-offset tail (transport + deformed-space stylers), `k` = number of offsets, `m` = memory-bound pass ≈ 1 ms:

| Profile | Per frame (viewport scrub) | Per render frame (prman, k offsets) | Blur quality |
|---|---|---|---|
| P0 | `T` (head cached) | `T` | none |
| P1 velocities from surface velocities | `T + m` (transport the surface's `velocities` through the same per-root frame) | `T + m`; prman then does `{open, close}` internally | linear; ignores styler non-linearity |
| P1 velocities from two evaluations | `2T + m` | `2T + m` | linear, but consistent with the full tail |
| P2 samples | `T` (Storm never pulls samples; lazy path idle) | `k·T + (k−1)·m` (+ `m` per non-retained `GetValue`) | exact at offsets, curved with k ≥ 3 |
| usdRig-style full re-evaluation | — | `k·(H+T)` | exact |

Observations that fall out of the evidence rather than the prototype:

- Because Storm never calls the sampled path, P2 costs the viewport nothing as long as offsets are computed lazily or by preflight; P1 costs the viewport nothing beyond one extra 1 ms pass, which is why P1 should still be opt-in (it changes the published primvar set).
- hdPrman touches each retained sample at least twice (`GetValue` in the ordinality check and again in the delegate loop) — two `VtArray` shared-copies, negligible if retained; catastrophic if `GetValue` recomputed.
- With UsdImaging surfaces reporting `[-1, 0, +1]`, forwarding the surface's times triples the tail cost relative to `{open, close}` and evaluates at times far outside a `±0.25` shutter; clamping to the window (`{open, 0, close}`) is both cheaper and closer to the shutter. Recommend clamp by default, forward as an option for exact parity with the surface's own blur.
- What must be measured on the data-plane prototype: `H` and `T` for the reference chain (generator → clump → generator → clump → frizz) at 100k curves; the per-offset transport kernel; and whether `T` is dominated by the surface frame lookup (which is itself memory-bound) or by deformed-space stylers.

---

## 6. Data-source API sketch (what every output prim publishes)

```
class UsdGenSampledPointsDataSource : HdTypedSampledDataSource<VtVec3fArray> {
  VtVec3fArray GetTypedValue(Time t) override;           // retained or lerp of retained offsets; never evaluates in P0/P1
  bool GetContributingSampleTimesForInterval(Time s, Time e, std::vector<Time>*) override;
      // P0/P1: return false.  P2: fill cache (lazy or preflight), return retained offsets (>=2) else false.
};
// prim container = HdOverlayContainerDataSource(strongRoot, upstream)  (usdRig pattern, sceneIndices.cpp:1699-1709)
// strongRoot.primvars = { points: sampled, velocities: Block|retained(P1), accelerations: Block, widths/normals/...: retained }
```

Dirty notices per publish: `primvars/points/primvarValue`, `primvars/velocities`, `primvars/accelerations`, `extent/min|max`, expanded with `HdContainerDataSourceEditor::ComputeDirtyLocators` as usdRig does (`sceneIndices.cpp:2459-2505`). Thread safety: all `GetValue`/`GetContributingSampleTimesForInterval` calls must be reentrant (`dataSource.h:203-210`); the lazy P2 fill takes the graph mutex and publishes into an immutable generation before returning.

---

## Key facts

- Storm never requests motion samples: no `SamplePrimvar`/`SampleTransform` in hdSt; basisCurves reads `GetValue(0.0f)` directly (`OpenUSD/pxr/imaging/hdSt/basisCurves.cpp:975-990`); the delegate fallback interval is `[0,0]` (`sceneIndexAdapterSceneDelegate.cpp:142-143`).
- The generic multi-sample path is `_SamplePrimvar`: `GetContributingSampleTimesForInterval(start,end)` then one `GetValue(t)` per time, truncated to `maxSampleCount` (`sceneIndexAdapterSceneDelegate.cpp:2437-2461`); the `HdTimeSampleArray` wrapper re-queries and `TF_VERIFY`s a stable count (`sceneDelegate.h:1144-1175`).
- hdPrman's shutter is the camera's frame-relative `shutter:open/close` (default 0/0 → no blur) pushed into a static in its motion-blur SI (`renderParam.cpp:4536-4541, 5099-5110`; `motionBlurSceneIndexPlugin.cpp:64-67,1129-1136`); the SI ignores the interval it is passed (`:265-268`), requires ≥2 equal-size samples (`:349-380`), may re-distribute to `ri:object:geosamples` (`:382-410`), and blurs only `points`/xform (`:227-246`); every other primvar is collapsed to offset 0 (`renderParam.cpp:833-847`).
- `HdsiVelocityMotionResolvingSceneIndex` supports basisCurves (`velocityMotionResolvingSceneIndex.cpp:710-722`), turns retained `points`+`velocities` into `{start,end}` samples (`:176-190, 236-263`), and is downstream of app-level SIs in Storm (`hdSt:phase0`) and hdPrman (`hdPrman:phase2`); `tcps` comes from scene globals (`:268-279`, `dataSourceStage.cpp:69-71`), not from the plugin `fps` arg (`:685-696`).
- Blocking works because overlay containers turn `HdBlockDataSource` into null (`overlayContainerDataSource.cpp:94-97`), so the velocity SI reports "No velocities" (`:346-352`); usdRig blocks `velocities`/`accelerations` whenever it owns points (`usdRig/libs/rigExecImaging/sceneIndices.cpp:1356-1363`).
- UsdImaging surfaces answer any offset by interpolation (`dataSourceAttribute.h:41-50`) but report authored samples plus bracketing ones — `[-1,0,+1]` for per-frame animation and a sub-frame shutter (`:86-125`).
- RigExec surfaces return samples only when the app called `EvaluateAndPublishSamples` (`bridge.cpp:1257-1364`), which evaluates the entire rig once per offset (`:1284-1287`); the source reports offsets only when ≥2 (`sceneIndices.cpp:306-312`) and returns the nearest sample (`:292-303`); no shipped tool calls it (only `tests/testRigExecImaging.cpp:506-540, 984-1000`).
- UsdSkel surfaces are ext computations until `HdSiExtComputationPrimvarPruningSceneIndex` (hdPrman phase 0, not Storm) unless `HD_ENABLE_DEFERRED_SKINNING` (`usdSkelImaging/pointsResolvingSceneIndex.h:15-20`, `dataSourceResolvedPointsBasedPrim.cpp:1086-1100`, `hd/skinningSettings.cpp:15-34`, `hdPrman/plugInfo.json:168-178`).
- Measured floors for 1.6 M points: ~1.0 ms per memory-bound pass (velocity extrapolate, lerp, finite difference), 0.49 ms copy, 19.2 MB per sample (`scratchpad/mbbench.cpp`, Cortex-X925).
- Decision: retained per-offset cache filled lazily on first interval pull or by app preflight; velocities only as an explicit opt-in profile; per-offset re-evaluation limited to the deform-transport tail by splitting stylers into rest-space vs deformed-space.

## Open questions

- Whether `UsdImagingFlatteningSceneIndex` (inside `UsdImagingNiPrototypePropagatingSceneIndex`, per usdRig `snapshotStore.h:97-102`) preserves upstream xform sample times when composing parent transforms — determines whether hair under an animated parent gets transform blur from prman without help. Not read.
- How `HdSiExtComputationPrimvarPruningSceneIndex` evaluates CPU skinning per requested offset and its cost (`extComputationPrimvarPruningSceneIndex.cpp:112,234,486-496` seen only by grep); whether usdGen should evaluate skinning itself for skel-driven surfaces.
- Whether hdPrman's `SyncAll` really pulls the hair prim's sample list on multiple threads concurrently for prims of the same graph (assumed from render-index parallel Sync; not traced in 26.08 `renderIndex.cpp`).
- Actual `H`/`T` chain timings for the reference operator chain at 100k curves — only the memory-bound floors were measured.
- Whether any other in-tree or external Hydra 2.0 renderer (hdEmbree declares no blur; hdArnold/hdCycles not on this machine) calls `GetContributingSampleTimesForInterval` with a sub-frame interval directly on the scene index rather than through the adapter delegate.
- (Resolved) `HdSceneIndexCreateArgsSchema.motionBlurSupport` is only set by hdEmbree in tree (`hdEmbree/rendererPlugin.cpp:42-48`); hdPrman has no `GetSceneIndexCreateArgs`/`SetMotionBlurSupport` anywhere under `hdPrman/*.cpp` (grep), so for prman the bit is **absent** and usdGen must follow usdRig's `MotionBlurSupport::Absent` rule — the application's explicit profile is authoritative (`usdRig/libs/rigExecImaging/bridge.cpp:1244-1246`, `docs/spec.md:1611`). The bit cannot be used to detect prman-class renderers; the camera shutter (or the lazy first pull) must.
