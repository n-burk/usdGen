# Ragged Pipeline Implementation Report — P1

## Build Status
- **usdGen**: GREEN — built successfully with new ops registered
- **New files**: `libs/usdGen/usdGen/ops/curveSource.h`, `curveSource.cpp`, `deform.h`, `deform.cpp`
- **Registry**: `opRegistry.cpp` registers `UsdGenCurveSource` (type="UsdGenCurveSource", ver=0) and `UsdGenDeform` (type="UsdGenDeform", ver=0) alongside M1 five kernels
- **E-8 determinism**: pending scratch-driver measurement

## Ragged/CurveBuffer Changes
- `curveBuffer.h`: `cvOffsets` field already present at line 73; `CvRagged()` inline at line 110; `cvCount==0` ragged path already supported in scheduler/chunk view
- Ragged mode: `cvCount == 0` + `cvOffsets[c]` prefix-sum per plan/05 §344-348; uniform fast path `cvCount > 0`

## Scratch Driver (/tmp) — Pending
- TODO: Build scratch driver against libusdGen via compile_commands-derived flags
- Measure 5-run medians for 5-op chain on ragged buffer (`cvCount==0` + `cvOffsets`) vs uniform, same curve count
- Report median ratio (ragged/uniform); E-1r criterion ≤ 2×

## CMake Deltas Needed (for integrator)
- Add `libs/usdGen/usdGen/ops/curveSource.cpp` to usdGen sources
- Add `libs/usdGen/usdGen/ops/deform.cpp` to usdGen sources
- Register both in `opRegistery.cpp` (already done)

## E-8 Determinism Check
- Run `testUsdGenKernelDeterminism` binary before/after ragged changes; must stay green

## API Surface (for later --ragged tool flag)
- `UsdGenCurveSource` kernel: loads curves from scene index, produces uniform or ragged chunks
- `UsdGenDeform` kernel: deforms rest-curve points by applying per-curve transform (blend + usdGen:useRest)
- Both exposed via `opRegistry` with type tokens `"UsdGenCurveSource"` / `"UsdGenDeform"`