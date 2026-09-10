# Wave status — 2026-09-10 19:55Z (coordinator)

## ENDPOINTS
Meta/kimi: 429 storms (bursts ~2-5min ok, then 10-retry death; cooldown-in-progress, ONE retry spawn ~20:12Z). hivemind: 400 tool_choice regression on fresh spawns + 80K cap. nemotron role → deepseek remap, stream-timeout dead. = infra-wide outage; everything queues behind Meta window.

## CUDA (user-explicit; corrected scope per advisor, VERIFIED on disk)
Packet-CUDA-FIX (fire once at cooldown end, Meta fresh spawn, ≤12 calls, write/fix-first):
1. FIX libs/usdGen/usdGen/ops/noise.cpp Capture (~239-278,287-320): derive per-curve counts from upstream.cvOffsets (field name via grep); ragged ⇒ cvCount=0 / CPU-only; keep GPU dispatch uniform-only.
2. FIX Evaluate (~365-413): per-curve count from CvRagged (view exposes Cv(c,i)+count source — grep CvRagged), NOT view->cvCount; ragged loops zero times today (366-380).
3. tests/testGpuCpuParity.cpp: G0 uniform GPU-vs-CPU bit-exact (USE_GPU_NOISE); G1 ragged end-to-end ON-build correctness (nonuniform cvOffsets, both stages); #else skip.
4. ROOT CMakeLists.txt register usdgen_gpu_parity (+add_test; def gated). Build /tmp/ug-on3 (ON Release), ctest -R Gpu, OFF-tree build/ skip-check, scoped commit if porcelain clean.
Acceptance: pasted ragged test PASSED + uniform parity + first-error-if-stuck. NEVER fabricate.
Lane history: ParityGate(429)→2(research-36calls BLOCKED; notebook good)→3(cancel)→4(research, cancel)→5(hivemind 400)→GateMeta(429×2 dead)→Nemo(dead).

## LANDED
ledger-shard-a1 ✓ a2 ✓ b1 ✓ c ✓ | adjudication-1.md RATIFY+include-fix applied (usdGenGraphDescBuilder.cpp:13, uncommitted).

## OWED
b2(07-08, B3 banked reads, re-GO when Meta), E(expanded: design/research/00/12/AppA-B), Aggregator-FINAL(gate a1+a2+b1+b2+c+e), fur PNGs ×3 (lane dead; respawn fresh Meta), B1 storm triage (B1Triage idle-dead; respawn 2-call), E1 GPU timing (CheckpointGit PKT-T banked, self-HOLDs if GPU≥10%), Integrator commits (held; HEAD 1daff8e, ~48 dirty).
