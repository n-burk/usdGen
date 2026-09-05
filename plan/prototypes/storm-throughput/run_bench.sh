#!/usr/bin/env bash
# Storm hair throughput benchmark -- run this on a workstation WITH a display.
# (garch on Linux is GLX-only in 26.08, so Storm cannot run headless.)
set -euo pipefail

USD=${USD:-/path/to/OpenUSD_install}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/hairbench}
export PYTHONPATH="$USD/lib/python3.12/site-packages:${PYTHONPATH:-}"
export HAIRBENCH_OUT="$OUT"
mkdir -p "$OUT"

STAGES=${STAGES:-"$HERE/stages"}

# ---------------------------------------------------------------- 0. stages
if [ ! -f "$STAGES/hair_1prim.usdc" ]; then
  python3 "$HERE/gen_hair_stages.py" --out "$STAGES" --curves 100000 --cv 8 --frames 24
fi

# --------------------------------------------------- 1. wall-clock, usdview
# STATIC / DEFORM / EDIT / SCRUB timings at refineLevel 0..3, one json per stage.
for s in hair_1prim hair_32chunks hair_1000prims hair_1prim_linear hair_32chunks_anim; do
  echo "=== $s ==="
  "$USD/bin/testusdview" --renderer GL --viewportSize 1920 1080 \
      --testScript "$HERE/bench_usdview.py" "$STAGES/$s.usdc" \
      2> "$OUT/$s.stderr" | tee "$OUT/$s.stdout"
done

# ------------------------------------------------------------- 2. counters
# HdPerfLog is OFF unless HD_ENABLE_PERFLOG=1 (pxr/imaging/hd/perfLog.cpp:22-27).
# There is no pxr.Hd python module, so counters need the C++ harness.
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_PREFIX_PATH="$USD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$HERE/build"
for s in hair_1prim hair_32chunks hair_1000prims; do
  for r in 1 2 3; do
    HD_ENABLE_PERFLOG=1 "$HERE/build/hairbench" "$STAGES/$s.usdc" $r \
        | tee "$OUT/counters_${s}_refine${r}.txt"
  done
done

# ------------------------------------------------------- 3. batch structure
# One line per batch; count them and check they do NOT scale with prim count.
for s in hair_1prim hair_32chunks hair_1000prims; do
  HD_ENABLE_PERFLOG=1 TF_DEBUG=HDST_DRAW_BATCH \
      "$HERE/build/hairbench" "$STAGES/$s.usdc" 2 \
      2> "$OUT/batches_$s.log" >/dev/null || true
  echo "$s batches: $(grep -c 'Creating.*batch\|Validating' "$OUT/batches_$s.log" || true)"
done

# --------------------------------------------- 4. per-prim sync attribution
for s in hair_32chunks hair_1000prims; do
  HD_ENABLE_PERFLOG=1 TF_DEBUG="HD_RPRIM_UPDATED HD_DIRTY_LIST" \
      "$HERE/build/hairbench" "$STAGES/$s.usdc" 2 \
      2> "$OUT/rprim_$s.log" >/dev/null || true
done

# ------------------------------------------- 5. commit phase breakdown (Trace)
# Gives Resolve / Resize / "Reallocate buffer arrays" / Copy / Flush inside
# HdStResourceRegistry::_Commit (resourceRegistry.cpp:861-1010) and
# HdRenderIndex::SyncAll's Pre-Sync / Scene Delegate Sync / Rprim Sync scopes.
for s in hair_1prim hair_32chunks hair_1000prims; do
  "$USD/bin/testusdview" --renderer GL --viewportSize 1920 1080 \
      --traceToFile "$OUT/trace_$s.json" --traceFormat chrome \
      --testScript "$HERE/bench_usdview.py" "$STAGES/$s.usdc" >/dev/null 2>&1 || true
done

# ---------------------------------------------------- 6. culling A/B (1 vs N)
for s in hair_1prim hair_32chunks hair_1000prims; do
  for cull in 1 0; do
    HD_ENABLE_PERFLOG=1 HD_ENABLE_GPU_FRUSTUM_CULLING=$cull \
      "$HERE/build/hairbench" "$STAGES/$s.usdc" 2 \
      | tee "$OUT/cull${cull}_$s.txt" | grep -E "^STATIC|itemsDrawn|drawCalls"
  done
done

echo "results in $OUT"
