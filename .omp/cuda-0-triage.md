# cuda-0 triage — GPU noise (SeExpr2::FBM) — ownership + CMake hook

## CPU ground truth
`SeExpr2::FBM<3,1,false,float>` — `libs/usdGen/usdGen/ops/noise.cpp:181-277`
(hot loop `:255-274`).
- Domain per CV: rest-pinned `root*corr + (1-corr)*hashVec3 + hairT*freq`.
- Output: `perCv` / `perCurve` buffers; uniform-cvCount fast path exists.
- Bit-exactness contract: same `HashVec3` bits + same float op order + SeExpr
  RNG boundaries. No reassociation on GPU; hash table must be reproduced in
  constant/global memory bit-for-bit.

## Environment
- Toolchain: CUDA 13.0, nvcc V13.0.88 (`/usr/local/cuda`), host GB10 = sm_121.
- OpenCL: dead — no CL ICD/vendor on aarch64 GB10 (see gpu-backend-feasibility.md).
- Arch verdict (this slot): see CudaCfgProbe yield — probe sm_121 first, sm_120 fallback.

## Ownership — CUDA1a (this slot, done)
- CMake: root `CMakeLists.txt`, guarded block at end of library section:
  `option(USE_GPU_NOISE ... OFF)` → `check_language(CUDA)` + `enable_language(CUDA)`
  + `FindCUDAToolkit` + configure-time try_compile arch probe (sm_121→sm_120).
  OFF default: no CUDA involvement at all → configure stays green on CI hosts.
- `target_sources(usdGen PRIVATE .../noise_gpu.cu)` is gated on `EXISTS` so the
  hook is inert until CUDA1b lands the file.

## Ownership — CUDA1b / CUDA1c (next)
- NEW `libs/usdGen/usdGen/ops/noise_gpu.cu` (kernel; pinned-buffer transfer,
  single stream, launch from Capture thread).
- `UsdGenNoiseOp::Capture` fallback branch ONLY — `Evaluate` + capture contract
  untouched (integration point per feasibility note §1).
- CUDA1c: bit-exact GPU-vs-CPU field diff harness, then timing.
