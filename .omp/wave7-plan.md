# Wave 7 plan — M2..M8 milestone decomposition (no code)

Date: 2026-09-09. Sources: `plan/11-roadmap.md` §2 (milestones/exit gates),
`plan/09-performance-and-benchmarks.md` §5 (registry — owns every threshold),
`plan/README.md` §3 (registry ownership), `plan/10-build-dependencies-testing.md`
§5.6/§6.5 (test-name registry). Tree verified live this session
(`libs/`, `plugin/`, `tests/`, `usdGenShaders/`, `thirdparty/`, `python/`,
`docs/m1/`, `docs/freezes/`, `docs/review/m1-teststatus.md`,
`.omp/checkpoint-m1.md`). M0 committed (`831c869`); M1 wip committed
(`1daff8e`) + uncommitted T2-tier edits (Storm bodies, surgery, goldens).

Conventions: gate one-liners below are `Tier — threshold` transcribed from 09 §5,
**not** re-derived. "M1 provides" = verified in the tree, not in the plan.
`MEASURED-pass`/`UNMEASURED` are the registry's status words (README §2).

## M1 leftovers (do NOT re-plan; close in Wave 6 / Storm+T1 lanes)

Gates not yet MEASURED-pass given today's state — all in flight, none redone here:

| Gate | State today |
|---|---|
| SI-1 | `tests/testUsdGenTileContract.cpp` (24 KB body) exists; gate unmeasured |
| SI-7 | `tests/testUsdGenAdapter.cpp` (172 B), `testUsdGenRestAdapter.cpp` (180 B) = stubs |
| S-1 / S-5 / S-6 / S-12 | `tests/perf/benchUsdGenStorm.cpp` (520-line driver) exists; unmeasured; needs quiet GPU |
| S-9 | `tests/testUsdGenStormRefine.cpp` exists; unmeasured |
| S-8-tangent | `tests/testUsdGenStormTangent.cpp` **DOES NOT EXIST** (ctest M1-STUB echo only) |
| S-8-hgi | **GREEN** — `tests/testUsdGenStormHgiResource.cpp` + `docs/m1/adr-s8-hgi-default.md`: A/B mean pixel diff 0.00000, B (HGI off) ships as default. Nothing redone. |
| L-1 | `tests/testUsdGenStormMaterial.cpp` **DOES NOT EXIST** (ctest M1-STUB echo only) |
| L-2 | `tests/testUsdGenStormLook.cpp` + `stormTestUtils.h` + `tests/golden/stormLook_A.png` exist; unmeasured |
| S-11 | `tests/testUsdGenStormSurgery.cpp` (P0 race contract) exists but **mislabeled** — see contradiction C1 |

M1 already green (guard, do not regress): E-1/E-2/E-6/E-7/E-8, SI-2/SI-3/SI-4/SI-5,
SI-8 (`testUsdGenAutoApply`), C1 freeze (`docs/freezes/C1.md` + checker 44/44 +
322/322), C5 (`checkC5.py` green), N-5 `testUsdGenSchemaUpToDate`, B-1 link/include
rules. C2/C5 freeze at M1 exit — S-12 must run **before** the C2 freeze is tagged.

## M2 — Deform and freeze · 4 wk / 11 eng-wk · freezes C3

Exit gates (11 §2.0; 09 §5.5 row: `E-1r, E-3; SI-2 re-run; SI-9, SI-10; S-2, S-3, S-4`):

| Gate | Tier + threshold (09 §5) | Deliverables (files/targets) | M1 provides (verified) | Packet sketch (owner → 1 acceptance criterion) |
|---|---|---|---|---|
| E-1r | T0 — ragged (`cvCount==0`+`cvOffsets`) ≤ 2× uniform path | `benchUsdGenChain --ragged` green; ragged buffers in `libs/usdGen/usdGen/curveBuffer.h`, `UsdGenCurveSource` kernel | `libs/usdGen/usdGen/` engine (graph/scheduler/compiler/store) + `benchUsdGenChain.cpp` exist | EngineRagged owns `libs/usdGen/**` → `benchUsdGenChain --ragged` reports ratio ≤ 2.0 |
| E-3 | T0 — terminal-param edit ≤ 0.6 ms (MEASURED-pass EV-004, re-produced on shipped chain) | `benchUsdGenSparse --terminal` green; no new file | `tests/perf/benchUsdGenSparse.cpp` exists | EngineSparse owns `tests/perf/benchUsdGenSparse.cpp` → `--terminal` ≤ 0.6 ms |
| SI-9 | T1 — pruning-wrapper per-deforming-frame cost inside class-A ≤ 1.0 ms share on production-density skinned scalp; red ⇒ SC-13 | NEW `tests/testUsdGenPruningCost.cpp` + wrapper in `libs/usdGenImaging/usdGenImaging/` (private `HdSiExtComputationPrimvarPruningSceneIndex` wrapper); companion `tests/testUsdGenSkelInterop.cpp` (resolves `skinnedPoints` with `HD_ENABLE_SCENE_INDEX_EMULATION` off) | Imaging session/bridge/router/primAdapter exist; `chain-order/stages/skelAndRig.usda` fixture named by plan (carry-in; verify presence at M2 kickoff) | ImagingPrune owns `libs/usdGenImaging/usdGenImaging/*Prun*` + `tests/testUsdGenPruningCost.cpp` → wrapper cost recorded ≤ 1.0 ms |
| SI-10 | T1 — two indices on one session: identical generation+prim set+frame for one commit | NEW `tests/testUsdGenSessions.cpp` over `usdGenImagingSession.{h,cpp}` (exists) | `usdGenImagingSession.h/.cpp` exist | ImagingSessions owns `tests/testUsdGenSessions.cpp` → two attached indices agree, exit 0 |
| S-2 | T2 — deform frame ≤ 2.0 ms delta over static @100k×8CV 720p 49 tiles | `benchUsdGenStorm --deform` green | bench driver + EGL harness (`usdGenShaders/test/eglctx.h`) exist | StormDeform owns `tests/perf/benchUsdGenStorm.cpp` `--deform` path → delta ≤ 2.0 ms |
| S-3 | T2 — 1-of-49-tile edit ≤ 1.2× static | `benchUsdGenStorm --onetile` green | same driver | StormOneTile owns `--onetile` path (same file, serialized after StormDeform) → ≤ 1.2× static |
| S-4 | T2 — camera frames ¼ of G4: `itemsDrawn` drops ≥ 3× at 32 and 196 tiles, flat at 1 prim | `benchUsdGenStorm --cull` green + G4 fixture (§4.4) | driver exists; G4 fixture to be generated (`gen_hair_stages.py` exists in `tests/perf/`) | FixturesG4 owns `tests/perf/gen_hair_stages.py` + scenes → G4 authored, `--cull` asserts pass |
| (non-gate) | Freeze/undo: `SubtreeSnapshot` + `SetActive(false)`; `testUsdGenFrozenReentry` (bit match), `testUsdGenStageEdits` (RemovePrim containment), `test_usdgen_undo.py`, `test_usdgen_freeze_author.py`; freeze ≤ 1 ms + sidecar reopen ≤ 1 ms | `UsdGenFreeze`/`UsdGenSculptLayer` kernels; `python/usdgen/` currently **EMPTY** — scripts are new files | Schema already declares `UsdGenFreeze`, `UsdGenCurveSource`, `UsdGenSculptLayer`, `UsdGenDeform`, `UsdGenCurveAPI` (schema.usda verified); engine `ops/` has only scatter/grow/noise/length/width — Deform/CurveSource/Freeze/SculptLayer kernels are NEW | EngineFreeze owns `libs/usdGen/usdGen/ops/deform.cpp` + `curveSource.cpp` + `freeze.cpp` → frozen output re-enters bit-identical |

Edges to M1: tile publisher + adapter (re-entry path), `UsdGenRestAPI` adapter (first
consumer: `rest/points` at Default time), session multi-index (SI-10 builds on M1 session),
bench driver + EGL harness, C3 frozen here (`testUsdGenContracts` gains C3 name list).

## M3 — Guides and clumps · 5 wk / 13 eng-wk

| Gate | Tier + threshold | Deliverables | M1 provides | Packet sketch |
|---|---|---|---|---|
| E-4 | T0 — kNN capture 4k guides × 100k/1M roots @8thr: ≤ 25 ms at 100k **and linear in roots**; > 300 ms at 1M ⇒ SC-6 | NEW `tests/perf/benchUsdGenKnn.cpp`; vendored `thirdparty/nanoflann/nanoflann.hpp` (verified present) + reference lane (I3) in `libs/usdGen/` | nanoflann header present; engine arena/scheduler present | EngineKnn owns `tests/perf/benchUsdGenKnn.cpp` + capture path → ≤ 25 ms @100k, linear |
| E-5 | T0 — RSS ≤ 800 MB @1M×8CV 5 nodes default caching (UNMEASURED/at-risk; DERIVED 950–1050 MB) | NEW `tests/perf/benchUsdGenMemory.cpp`; eviction score `recomputeCostMs/bytes` from `UsdGenNodeStats` | `stats.h` exists (verify eviction fields at kickoff) | EngineMem owns `tests/perf/benchUsdGenMemory.cpp` → RSS recorded; red ⇒ SC-7a/7b path |
| E-1 re-runs | T0 — 7-node chain green; E-1 at 1M on `head1M` recorded (09 sets **no 1M threshold** — record only until 09 registers one) | `head1M` fixture variant (§4.4); possible 09 §5.1 E-1 row edit (plan-owned, filed against 09) | `tests/scenes/make_g3_100k.py` pattern to copy | Fixtures1M owns `head1M` scene → E-1 re-run recorded |
| (assertions) | T1 guide-edit dirties only referencing tiles; cross-chunk clump-centre correctness | `UsdGenGuideInterpolate` + multi-level `UsdGenClump` kernels (`guideIndex`/`guideWeight` elementSize 3; `clumpId_<level>`) | Schema declares `UsdGenGuideInterpolate`, `UsdGenClump`, `UsdGenGuideSet`, `UsdGenSmooth`, `UsdGenResample`, `UsdGenDirection` (verified); engine lacks all six kernels | EngineGuides owns `ops/guideInterpolate.cpp` + `ops/clump.cpp` → guide edit dirties only referencing tiles |

Edges: M2→M3 (guides are C3 curves; C3 must be frozen first). Contingency SC-6 (+1 wk).

## M4 — Maps and expressions · 4 wk / 11 eng-wk

| Gate | Tier + threshold | Deliverables | M1 provides | Packet sketch |
|---|---|---|---|---|
| L-3 | T0 — bitwise-identical baked primvars @1 vs 8 threads; `isThreadSafe()` true all shipped fns; `rand()` reproducible | NEW `tests/testUsdGenExpr.cpp`, `tests/testUsdGenLookBake.cpp` | `thirdparty/seexpr/SeExpr2/` exists but **no downloaded sources verified** (dir listed empty); offline-build gate needs `USE_PREGENERATED_FILES` fallback | ExprDet owns `tests/testUsdGenExpr.cpp` → `--threads` bitwise pass |
| L-4 | T0/T1 — map capture ≤ **200 ms** (image+Ptex+expr @1M roots 8thr, ASSUMPTION) + `PtexCache::Stats.fileReopens == 0` at default cache size | NEW `tests/perf/benchUsdGenMaps.cpp`; `usdGen_seexpr` + `usdGen_ptex` static targets (M0 built noise-only `Noise.h/.cpp`) | `thirdparty/ptex/` dir exists, **contents unverified**; map schema types declared (`UsdGenImageMap/PtexMap/ExprMap/PaintMap/NoiseMap/CombineMap/GuideProximityMap`, verified) | MapsPerf owns `tests/perf/benchUsdGenMaps.cpp` → ≤ 200 ms, zero reopens |
| L-5 | T1 — `ReloadMaps` recaptures exactly changed-map nodes; dirties exactly baked primvar `primvarValue` on affected tiles; `displayColor` never co-dirtied with `points` | NEW `tests/testUsdGenMapReload.cpp`; `UsdGenImaging_ReloadMaps` (imaging) | primAdapter/dirty-router exist | ImagingReload owns `tests/testUsdGenMapReload.cpp` → exact dirty set, exit 0 |
| T-EXPR-1 | T0 — `SE_EXPR_PLUGINS` cleared pre-`init()`; closed fn set; parse error → fallback value, never Hydra exception | `testUsdGenExpr --sandbox` | same as L-3 | ExprSandbox owns `--sandbox` path → fallback-without-exception proven |
| T-PTEX-1 | T0 — ids match `Far::PtexIndices` on quads/n-gons/all-tri; GeomSubset doesn't shift | NEW `tests/testUsdGenPtex.cpp` | subset-target rule exists in M1 scope (Mesh+GeomSubset `usdGen:surface`) | PtexIds owns `tests/testUsdGenPtex.cpp` → agreement on all three topologies |
| (goldens) | per-curve density/expr/Ptex-mask goldens; `UsdGenScale` T0+T1; paint round-trip ≤ 1e-4 | `UsdGenScale` kernel (schema declares it, engine lacks it) | schema declares `UsdGenScale` | EngineScale owns `ops/scale.cpp` → value + invalidation tests green |

Edges: M1→M4 (capture exists); M3→M4 soft (GuideProximityMap wants reference lane).
Re-runs (not exits): E-1/E-5/E-8 with map nodes in chain.

## M5 — Tools · 5 wk / 13 eng-wk · freezes C4

| Gate | Tier + threshold | Deliverables | M1 provides | Packet sketch |
|---|---|---|---|---|
| T-1 | T3 — ≤ 1 ms Python+engine per brush move @100k (`testusdview` under Xvfb :77, llvmpipe CPU numbers) | `bin/run_testusdview_usdgen_comb.sh` + `testUsdviewUsdGenComb.py`; `usdgen` package (`lib/python/usdgen/`: `_usdGen`, `usdGenLib.py`, builder facade); BLAS pinned (`OPENBLAS/OMP_NUM_THREADS=1`) | `python/usdgen/` **EMPTY**; `tests/python/` has no `test_usdgen_*.py` yet; C ABI absent | ToolsComb owns `python/usdgen/usdGenLib.py` → per-move ≤ 1 ms |
| T-2 | T1 — `PickCV` ≤ 0.25 ms @100k CV, ≤ 2.5 ms @1M (MEASURED-pass EV-064) | NEW `tests/testUsdGenPick.cpp` (+ engine half `benchPick` in `testUsdGenEngineBench`); 19-entry C ABI in `registry.cpp` (`UsdGenImaging_*` incl. `GetLastError`) | `registry.{h,cpp}` exist (extend, don't fork) | ImagingPick owns `tests/testUsdGenPick.cpp` → both budgets pass |
| T-3 | T3 — CPU pick vs `view.pick()` over 100 px disagree on occlusion only | `testUsdviewUsdGenPick.py` | — | ToolsPickAcc owns `testUsdviewUsdGenPick.py` → disagreement set ⊆ occluded |
| T-4 | T3 — one freeze on 2205-prim stage: ≤ 5 ms author (excl. `_resetGUI`) **and exactly one** `_resetGUI` (one `Sdf.ChangeBlock`) | `testUsdviewUsdGenFreeze.py`; freeze bar/status UI | M2 freeze path | ToolsFreeze owns freeze batching → 1 `_resetGUI`, author ≤ 5 ms |
| S-7 | T2 — parked-CV density scrub ≤ 1.5× points-only frame | `benchUsdGenStorm --scrub` | bench driver (new mode) | StormScrub owns `--scrub` path → ≤ 1.5× |
| C4 | T1 — `testUsdGenAbi` holds 19 entry points (name: see C2) | `testUsdGenAbi*` binary; installed `usdGenUsdview` PluginContainer | no ABI test exists today | ToolsAbi owns ABI test → 19/19 symbols match 08 §1.4 |

Edges: M2+M3→M5 (brushes need freeze/undo + guides); M4→M5 soft (Map-paint brush).
RC-4 fps number explicitly NOT a gate here.

## M6 — Instancing · 3 wk / 7 eng-wk (only droppable milestone)

| Gate | Tier + threshold | Deliverables | M1 provides | Packet sketch |
|---|---|---|---|---|
| T-INST-1 | T1 — click resolves to `UsdGenDescription` through `primOrigin` (`ComputeInstancerContext` non-empty) | NEW `tests/testUsdGenInstancerPick.cpp` (+ unlabelled T3 `testUsdviewUsdGenCards.py`) | generation store + session exist | ImagingInstPick owns `tests/testUsdGenInstancerPick.cpp` → round-trip resolves |
| T-INST-2 | T1 — `usdGen:surface` rebases inside propagated native prototypes; `instancedBy` exactly one target | NEW `tests/testUsdGenInstancer.cpp`; instancer builder (`__usdGenRender/inst_<op>/Prototypes/<n>`) | schema declares `UsdGenInstance` (verified); no builder exists | ImagingInst owns instancer builder → rebase + single-target asserts pass |

Edges: M1→M6 (second publisher over generation store). T2 demo (example c, ≤ 0.5 %
vs golden) is a demo, not a gate id.

## M7 — Render time + hardening · 5 wk / 11 eng-wk

| Gate | Tier + threshold | Deliverables | M1 provides | Packet sketch |
|---|---|---|---|---|
| S-11 | T2 — `head1M` static draw L2 720p, **record only, no threshold** (tests §2.5 linear fit past 200k) | `benchUsdGenStorm --scene bench/head1M.usda --static` recorded to ledger | head1M fixture (from M3) | Storm1M owns the `--scene` run → number recorded with host tag |
| P2 assert | T1 — P2 k=3 costs 1 head + 3 tail evals, read from `UsdGenStats` | `UsdGenMotionCache` keyed `(graphGen, surfaceGen, absTime)`; `EvaluateSample` deformed-tail-only | `stats.h` exists; no motion cache | EngineMotion owns `motionCache` → stats show 1+3 evals |
| usdrecord GL | T3 Xvfb — canonical stages render with no usdview, no C API | runner over `docs/workstation-protocol.md` (exists, written M0) | protocol doc exists | ToolsRecord owns GL record run → all canonical stages render |
| hdPrman contract | T1 headless — 2–16 stable times, constant CV count, zero extra chain evals | NEW `tests/testUsdGenMotionSamples.cpp` (06 §10.2; 10 §5.6 carries the name) | — | ImagingSamples owns `tests/testUsdGenMotionSamples.cpp` → contract asserts pass |
| Re-runs | E-5@1M + motion cache; FULL E-*/SI-*/S-* on G1–G4; R-1 built | T4 runner (`10-…` §5.2); installed public headers (not-installed boundary closes) | B1–B12 bench protocol ships as `benchUsdGenStorm` sweep | ReleaseRunner owns T4 runner → R-1 running (release exits, not M7) |

Edges: M2→M7 (P2 re-evaluates deformed tail); M6→M7 (instance transforms blur).
Real `usdrecord -r "RenderMan RIS"` is RC-7 (release), not M7.

## M8 — Breadth · continuous (no milestone exit; per-landing)

| Gate | Tier + threshold | Deliverables | M1 provides | Packet sketch |
|---|---|---|---|---|
| T-5 | T3 — plugin sets `_allowAsync` without CLI flag (1 `asyncAllow`, ~10 `asyncPoll`/s) | `testUsdviewUsdGenAsync.py` ← `evaluation-scheduling/asyncProbe.cpp` (carry-in; verify at kickoff) | async plumbing absent | ToolsAsync owns async enablement → poll-rate assert passes |
| S-10 | T2 — PrimsRemoved/Added vs in-place `points` dirty on frozen prim | NEW `tests/testUsdGenStormInPlace.cpp` | frozen-prim path (M2) | StormInPlace owns `tests/testUsdGenStormInPlace.cpp` → both paths measured |
| per-operator | T0 value + T1 invalidation per landing: Curl/Bend/Straighten/Displace/Wave/Part(+brush)/ExprOp/uniform-Scatter/TsSpline/sculpt-rebase | kernels in `ops/`; schema declares Curl/Bend/Straighten/Displace/Wave/Part/ExprOp already (verified — names frozen early, kernels M8) | schema-declared, engine-missing (intended) | EngineBreadth owns one `ops/<name>.cpp` per landing → value + invalidation tests green |

E-1 stays green as catalogue grows. Simulation (Collide/Wind per plan) is v3 — out.

## Roadmap / registry contradictions found

- **C1 — S-11 mislabeled in tree.** `tests/testUsdGenStormSurgery.cpp:1` claims
  "gate S-11" for a commit-vs-render race contract (monotone generation, no torn
  snapshot). Registry 09 §5.3: S-11 = `head1M` static-draw **record only** via
  `benchUsdGenStorm --scene bench/head1M.usda --static` (M7). Root CMakeLists
  bakes the error in: `set(_usdgen_storm_labels_testUsdGenStormSurgery "T2;gate:S-11")`.
  The race contract has **no registry id**; either mint one (09 owns ids) or fold the
  assertions into SI-3/SI-4. Until then the M7 S-11 exit has no driver and the M1
  suite carries a gate label the registry doesn't define. (10 §5.6 T2 list also
  omits `testUsdGenStormSurgery` entirely.)
- **C2 — ABI test name.** 11 §0.2 + §6.4/§7 name it `testUsdGenAbi` (T1, loads
  `libusdGenImaging.so`); 10 §5.6 registers `testUsdGenAbiSymbols` (**T0**, nm-based)
  and separately `testUsdGenArrays`. Both can't own C4; reconcile before M5.
- **C3 — E-1 observed vs registry.** Checkpoint Wave 5 records `benchUsdGenChain`
  "Passed" at E-1 2.8–2.9 ms; registry E-1 is ≤ 1.5 ms @8thr (MEASURED 1.02 EV-008).
  Either the M1 wip gate uses a looser limit or E-1 is still red — confirm the
  enforced number before M1 tags; SC-11 keys off 1.5 ms / 3×.
- **C4 — 09 §5.5 vs 11 §2.0 phrasing.** 09 §5.5 M1 row lists exits but compresses the
  three PW decisions to "the S-8 / S-9 / L-1 decisions taken"; 11 §2.2's table is the
  fuller statement. No semantic clash, but quoters should cite 11 §2.2 for M1 scope.
- **C5 — SI-9 driver split.** 09 §5.2 assigns SI-9 to `testUsdGenPruningCost` and
  names `testUsdGenSkelInterop` a companion assertion; 11 §2.3 text leads with the
  two named tests (`testUsdGenSkelInterop`, `testUsdGenFrozenReentry`) and never
  names the cost driver. M2 kickoff should treat the cost driver as the gate and the
  interop test as its companion, per 09.
- **C6 — L-4 scope.** 11 §2.5 proposes the ≤ 200 ms threshold as ASSUMPTION;
  registry 09 §5.4 L-4 states it flatly. Fine (09 owns thresholds), but M4 must not
  re-litigate the number from 11's text.
- Non-contradiction verified: S-12 at M1 "before C2 freezes" (11 §2.2 + 09 §5.3 agree);
  T-INST-1 = T1 (09 §5.4, 10 §5.6, 11 §2.7 agree); no T4 exit (R-1/R-2/R-3 release-only
  everywhere); M6 droppable / M4-resequence costs (11 §3.2) consistent with edge table.
