#!/bin/bash
# PW-2 (S-8) benchmark matrix v2 (perturbed-tangent republish for variant B)
set -u
cd "$(dirname "$0")"
export LD_LIBRARY_PATH=/home/burkard/work/OpenUSD_26_08/lib
export PXR_PLUGINPATH_NAME="$PWD:/home/burkard/work/OpenUSD_26_08/plugin/usd"
LOG=results_s8_v2.txt
: > "$LOG"
run() { # label scene deform repub [env...]
  local label=$1 scene=$2 deform=$3 repub=$4; shift 4
  echo "## $label" >> "$LOG"
  env "$@" ./build/bench_s8 "$scene" /World/Cam 1280 720 1.2 60 "$deform" "$repub" 2>&1 | grep -v focusDistance >> "$LOG"
  echo >> "$LOG"
}
i=1
while [ $i -le 3 ]; do
  run "A r$i (inData.Neye, no hairTangent, deform)" scene_100k_A.usdc 1 0
  run "B r$i (hairTangent static, points-only deform)" scene_100k_B.usdc 1 0
  run "B+ r$i (hairTangent republished, perturbing)" scene_100k_B.usdc 1 1
  i=$((i+1))
done
run "A-HGI r1 (HDST_ENABLE_HGI_RESOURCE_GENERATION=1)" scene_100k_A.usdc 1 0 HDST_ENABLE_HGI_RESOURCE_GENERATION=1
run "A-HGI r2 (HDST_ENABLE_HGI_RESOURCE_GENERATION=1)" scene_100k_A.usdc 1 0 HDST_ENABLE_HGI_RESOURCE_GENERATION=1
