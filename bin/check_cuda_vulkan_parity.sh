#!/usr/bin/env bash
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
# Hardware gate: every selected test must execute both GPU implementations.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "${USDGEN_VULKAN_ALLOW_SOFTWARE:-0}" = 1 ]; then
    echo "The hardware parity gate excludes software Vulkan; unset USDGEN_VULKAN_ALLOW_SOFTWARE." >&2
    exit 1
fi
export GENBUILD="${1:-${GENBUILD:-$script_dir/../build}}"
GENBUILD="$(cd "$GENBUILD" && pwd)"
source "$script_dir/_env.sh"
for option in USDGEN_ENABLE_CUDA USDGEN_ENABLE_VULKAN_RUNTIME USDGEN_BUILD_VULKAN_TESTS USDGEN_BUILD_TESTS; do
    if ! grep -q "^${option}:BOOL=ON$" "$GENBUILD/CMakeCache.txt"; then
        echo "Parity requires -D${option}=ON in $GENBUILD" >&2
        exit 1
    fi
done
command -v vulkaninfo >/dev/null || {
    echo "Install Vulkan tools (vulkaninfo) and the Khronos validation layer." >&2
    exit 1
}
# Require actual layer insertion and GPU selection, even under an ICD override.
# Clear loader/layer disable controls so inherited settings cannot weaken the gate.
unset VK_LOADER_LAYERS_DISABLE VK_LAYER_DISABLES
export USDGEN_VULKAN_REQUIRE_HARDWARE=1
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
if ! VK_LOADER_DEBUG=layer vulkaninfo --summary > "$GENBUILD/vulkan-parity-device.txt" 2>&1; then
    echo "Vulkan hardware preflight failed; see $GENBUILD/vulkan-parity-device.txt." >&2
    exit 1
fi
if ! grep -q 'Insert instance layer "VK_LAYER_KHRONOS_validation"' "$GENBUILD/vulkan-parity-device.txt"; then
    echo "The Vulkan loader did not activate VK_LAYER_KHRONOS_validation." >&2
    exit 1
fi
ctest --test-dir "$GENBUILD" -L '^cuda-vulkan-parity$' --no-tests=error \
    --output-on-failure --output-junit "$GENBUILD/cuda-vulkan-parity.xml"
"$PY" - "$GENBUILD/cuda-vulkan-parity.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET

tests = list(ET.parse(sys.argv[1]).iter("testcase"))
required = {
    "testUsdGenCudaVulkanSessionParity", "testUsdGenCudaVulkanParity",
    "testUsdGenVulkanStyleVkParity", "testUsdGenVulkanSurfaceVk",
    "testUsdGenVulkanRbfVkParity", "testUsdGenVulkanCsourceVkParity",
    "testUsdGenVulkanExprVkParity", "testUsdGenVulkanMapVkSample",
    "testUsdGenVulkanPicktileVkParity", "testUsdGenVulkanResampleVk",
}
skipped = [test.get("name") for test in tests if test.find("skipped") is not None]
missing = sorted(required - {test.get("name") for test in tests})
failed = [test.get("name") for test in tests if test.find("failure") is not None or test.find("error") is not None]
if skipped or missing or failed:
    sys.exit("CUDA/Vulkan parity is unproven: " + "; ".join(
        label + ": " + ", ".join(names) for label, names in
        (("skipped", skipped), ("missing", missing), ("failed", failed)) if names))
print(f"CUDA/Vulkan hardware parity: {len(tests)} tests passed without skips")
PY
