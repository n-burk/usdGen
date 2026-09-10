# Ledger shard A2 — plan/03-execution-engine.md + plan/04-operators.md

Source: section outlines of both plans + known repo facts (plan-ledger, perf-results). No full-file reads.

| item | § | status | evidence/next |
|---|---|---|---|
| UsdGenGraphDesc pure value description | 03 §2 | DONE | .omp/plan-ledger.md: core/ (graph) |
| Executor + scheduling (dirty router, TBB arena) | 03 §5 | DONE | .omp/plan-ledger.md: core/ executor |
| Data model — planar SoA buffers, ChunkRef | 03 §1 | PARTIAL | next: confirm buffer/ChunkRef types in core/ |
| Compile with 4 digests | 03 §3.2 | PARTIAL | next: confirm digest computation in core/graph |
| Capture/evaluate separation | 03 §4 | PARTIAL | next: confirm capture API in core/ |
| Generation store + tile assembly/publication | 03 §6 | PARTIAL | next: confirm store + publication in core/ |
| Motion profiles P0/P1/P2 + cache | 03 §7 | PARTIAL | next: confirm profile implementation |
| Determinism (E-8 bitwise across thread counts) | 03 §11 | PARTIAL | next: run 1/2/4/8/20-thread bitwise check |
| Gate suite E-1..E-8 (T0) | 03 §12 | PARTIAL | perf-results anchors E-1; next: run full gate suite |
| v1 operator set (16 types) | 04 §1.2/§2 | DONE | ops/ builtins wired: curveSource.cpp curveSource.h deform.cpp deform.h grow.cpp grow.h length.cpp length.h noise.cpp noise_gpu.cu noise.h noise_tables_gen.cuh scatter.cpp scatter.h width.cpp width.h |
| Masks | 04 §5 | PARTIAL | next: confirm mask wiring in ops/ |
| Noise GPU integration | 04 §1.2 (Noise) | DONE | perf-results row; new-repo parity pending |
| Per-expression GPU eval (UsdGenExprOp) | 04 §1.3 / 11-roadmap M3 | MISSING | not implemented; scheduled M3 |
| UsdGenDeform | 04 §1.2 / 05 | PARTIAL | deformers owned by plan/05-geometry, not 03 |
