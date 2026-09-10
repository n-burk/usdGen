# Checkpoint cuda1 — staging snapshot (PKT-K, 2026-09-10)

Snapshot of `git status --porcelain`; no add/commit performed.

## Core CUDA noise port (staging set)
- `libs/usdGen/usdGen/ops/noise_gpu.cu` (NEW): device port of noise kernels (CUDA sm_121).
- `libs/usdGen/usdGen/ops/noise_tables_gen.cuh` (NEW): regenerated __constant__ table G[514][3] as double.
- `CMakeLists.txt` (M): GPU hook + numeric-arch + fp-contract extension.
- `libs/usdGen/usdGen/ops/noise.cpp` (M): #ifdef USDGEN_USE_GPU_NOISE capture branch.

## Other modified (M)
- `.omp/APPEND_SYSTEM.md`: perf-append system doc, edited.
- `.omp/checkpoint-m1.md`: prior checkpoint doc, edited.
- `docs/freezes/C2.md`: freeze doc, edited.
- `docs/m1/interfaces.md`: interface doc, edited.
- `docs/prework/PW-1-nanoflann-kdtree.md`, `PW-2-s8-glslfx-variant.md`, `PW-3-s9-refinelevel.md`: prework docs, edited.
- `libs/usdGen/usdGen/opRegistry.cpp`: op registration, modified.
- `libs/usdGen/usdGen/scheduler.cpp`: scheduler, modified.
- `libs/usdGen/usdGen/session.cpp`: session, modified.
- `libs/usdGenImaging/usdGenImaging/groomSceneIndexPlugin.{cpp,h}`, `primAdapter.{cpp,h}`: plugin/adapter, modified.
- `libs/usdGenImaging/usdGenImaging/usdGenDirtyRouter.cpp`, `usdGenGraphDescBuilder.cpp`: dirty-routing/graph-desc, modified.
- `libs/usdGenImaging/usdGenImaging/usdGenImagingSession.{cpp,h}`, `usdGenRestApiDataSource.cpp`: imaging session/REST source, modified.
- `tests/perf/benchUsdGenChain.cpp`, `benchUsdGenSparse.cpp`, `benchUsdGenStorm.cpp`, `gen_hair_stages.py`: perf benches, modified.
- `tests/testUsdGenAdapter.cpp`, `testUsdGenInvalidation.cpp`, `testUsdGenRestAdapter.cpp`, `testUsdGenTileContract.cpp`: unit tests, modified.

## Other untracked (??)
- `.omp/adjudication-1.md`, `coord-audit-1.md`, `cuda-0-triage.md`, `e1-waiver-decision.md`, `gate-status.md`, `gpu-backend-feasibility.md`, `locator-contract.md`, `locator-impl-report.md`, `m2-packets.md`, `perf-results.md`, `planner-queue.md`, `quarantine/`, `ragged-impl-report.md`, `sessions-recover.md`, `wave-execution-plan.md`, `wave7-plan.md`: .omp working docs.
- `cmake_sessions_registration_snippet.txt`: scratch CMake snippet.
- `docs/m1/adr-s8-hgi-default.md`: ADR doc.
- `libs/usdGen/usdGen/ops/curveSource.{cpp,h}`: new curveSource op.
- `libs/usdGen/usdGen/ops/deform.{cpp,h}`: new deform op.
- `libs/usdGen/usdGen/version.cpp`: new version op.
- `libs/usdGenImaging/usdGenImaging/testHook.{cpp,h}`: test hooks.
- `tests/stormTestUtils.h`: shared storm test utils.
- `tests/testUsdGenSessions.cpp`, `testUsdGenSessions_fixed.cpp`, `testUsdGenStormHgiResource.cpp`, `testUsdGenStormMaterial.cpp`, `testUsdGenStormRefine.cpp`, `testUsdGenStormTangent.cpp`: new tests.
- `usdGenBench.json`: bench config/output.

PKT-TRACK (2026-09-10): `git add` staged noise.cpp (M), noise_gpu.cu (A), noise_tables_gen.cuh (A); `git ls-files --error-unmatch` passes both new files; no commit.
