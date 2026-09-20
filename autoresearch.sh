#!/usr/bin/env bash
# autoresearch.sh — usdGen graph-execution compile + operator benchmark harness.
#
# Measures compile and operator/execution time across the CPU, CUDA, and Vulkan
# backends using a single multi-backend target (benchUsdGenExecCompileOp) built
# in the Release `build-both` tree (CUDA + Vulkan runtime both enabled).
#
# Contract:
#   * exit 0 on success, non-zero on any build/run failure
#   * emit one "METRIC <name>=<value_ms>" line per measured quantity on stdout
#   * the primary metric is exec_total_ms (last METRIC line)
#   * deterministic workload: fixed seeds, fixed data, no network, median-of-N
#
# Per-backend metrics (secondary):
#   cpu_compile_ms, cpu_recompile_ms, cpu_commit_ms
#   cuda_compile_ms, cuda_exec_ms        (0.0 if CUDA disabled / no device)
#   vulkan_compile_ms, vulkan_exec_ms    (0.0 if Vulkan disabled / no NVIDIA GPU)
# Primary:
#   exec_total_ms = cpu_compile + cpu_commit + cuda_compile + cuda_exec
#                   + vulkan_compile + vulkan_exec
#
# Lower is better.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# ninja lives in the user venv, not the default PATH.
export PATH="$HOME/.venv/bin:$PATH"
command -v ninja >/dev/null 2>&1 || { echo "FATAL: ninja not found" >&2; exit 1; }

BUILD_DIR="build-both"
SPV="build-both/vulkan/width.spv"

# Configure the combined CUDA+Vulkan Release tree only if needed.
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "configuring $BUILD_DIR (Release, CUDA+Vulkan)..." >&2
    cmake -S . -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DUSDGEN_BUILD_TESTS=ON \
        -DUSDGEN_ENABLE_CUDA=ON \
        -DUSDGEN_ENABLE_VULKAN_RUNTIME=ON
fi

# Build only the benchmark target (+ its usdGen deps) incrementally.
cmake --build "$BUILD_DIR" --target benchUsdGenExecCompileOp

# Load the shared libraries produced by this build tree.
export LD_LIBRARY_PATH="$ROOT/$BUILD_DIR:$ROOT/$BUILD_DIR/libs/usdGen:$ROOT/$ROOT/imaging:$ROOT/$BUILD_DIR/schema:${LD_LIBRARY_PATH:-}"

if [ ! -f "$SPV" ]; then
    echo "FATAL: Vulkan width SPIR-V not found at $SPV" >&2
    exit 1
fi

# Run and let stdout (the METRIC lines) pass through. MESA/TU device-probe
# messages go to stderr and are harmless; they do not affect the exit code.
exec "$ROOT/$BUILD_DIR/benchUsdGenExecCompileOp" "$SPV"
