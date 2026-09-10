# GPU Backend Feasibility — fBm Noise (E-1)

## Verified current state
- **Toolchain (host, arm64/GB10):** CUDA 13.0, `nvcc` V13.0.88 present; GPU = NVIDIA GB10 (sm_121 — supported by CUDA 13.x). **OpenCL: no runtime** — `/etc/OpenCL/ICD` absent, no `clinfo`; OpenCL path is a dead end (NVIDIA has ended CL driver support).
- **E-1 today is CPU-only:** `libs/usdGen/usdGen/ops/noise.cpp`
  - `UsdGenNoiseOp::Capture` (:181–277) pins the fBm field at capture time: per-curve root sample :255–261 and per-CV loop :263–274, both calling `SeExpr2::FBM<3,1,false,float>` on rest-pinned positions (I3, :236–241).
  - `UsdGenNoiseOp::Evaluate` (:279+) is the per-frame envelope: rebuilds the magnitude ramp LUT from live params (review M-5) and splats the captured `perCv` field onto chunk `px/py/pz`.
  - No GLSL/CUDA/OpenCL code exists in the repo (grep-verified zero).

## Options
1. **CUDA kernel (fBm + capture) — recommended.**
   - New `noise_gpu.cu` computing the same pinned fBm field (per-cv loop is embarrassingly parallel; root samples per curve trivial).
   - **Integration point:** `UsdGenNoiseOp::Capture` (compute field → `cap.perCv`, `cap.perCurve`), **not** a new node type: the capture payload, mask block, and `Evaluate` envelope all stay untouched — minimal blast radius, identical downstream contract. `Evaluate` remains pure CPU (tiny work per frame: LUT + splat).
   - **Build:** `FindCUDAToolkit` (CMake ≥3.17), opt-in `USE_GPU_NOISE` option linking `usdGen` against the CUDA runtime; target `sm_120` (GB10 = sm_121, CUDA 13 supports it). Keep CPU `SeExpr2::FBM` path as the default/fallback so CI without a GPU stays green.
   - **PXR/arena interplay:** no conflict — PXR/TBB arenas allocate host memory only; the kernel reads raw `px/py/pz/hairT` pointers already in host (pinned) memory. Launch from the single Capture thread, one stream, no arena involvement.
   - **Risks:** (a) per-capture H2D/D2H copies — mitigate with `cudaMallocHost` pinned buffers sized to `totalCvs`; (b) sm_121 is new — pin CUDA ≥13 and `sm_120a`-compatible math; (c) determinism — fBm on GPU must reproduce the CPU hash exactly (same `HashVec3` bits, same float ops; validate with a diff harness).
2. **OpenCL kernel — rejected:** no CL ICD/runtime on the host; GB10 aarch64 has no supported CL vendor.
3. **GLSL — rejected for E-1:** no GPU surface; only relevant if a future render node consumes the field.

## Recommendation
**CUDA.** Implement `UsdGenNoiseOp::Capture` GPU path behind `USE_GPU_NOISE=ON` (default OFF), `FindCUDAToolkit`, `sm_120`, pinned-memory transfer of the `perCv`/`perCurve` buffers; keep `Evaluate` and the entire capture contract unchanged. Estimated size: 1 new `.cu` (~200 LOC), 1 CMake hook, fallback branch in `Capture`. First milestone: bit-exact GPU vs CPU field diff on one frame.
