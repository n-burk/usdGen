#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sdk_root="${repo_root}/.vulkan-toolchain/root/usr"
build_dir="${1:-${repo_root}/build-vulkan}"
if [[ $# -gt 0 ]]; then shift; fi
layer_dir="${sdk_root}/share/vulkan/explicit_layer.d"
library_dir="${sdk_root}/lib/aarch64-linux-gnu"
[[ -f "${layer_dir}/VkLayer_khronos_validation.json" &&
   -f "${library_dir}/libVkLayer_khronos_validation.so" ]] || {
    echo "Run scripts/bootstrap-vulkan-toolchain.sh to provision the pinned validation layer." >&2
    exit 1
}

# Explicit layer enablement fails instance creation if the layer cannot load.
# CTest rejects validation diagnostics even when the native test exits zero.
exec env VK_LAYER_PATH="${layer_dir}" \
    LD_LIBRARY_PATH="${library_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
    VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true \
    VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION=true \
    ctest --test-dir "${build_dir}" -R '^testUsdGenVulkan' --output-on-failure -V "$@"
