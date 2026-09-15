# Workspace-local Vulkan toolchain

Run `scripts/bootstrap-vulkan-toolchain.sh`. It consumes the pinned manifest,
verifies SHA256 and package metadata before extraction, and never installs
system-wide.

```sh
export VULKAN_SDK="$PWD/.vulkan-toolchain/root/usr"
export PATH="$PWD/.vulkan-toolchain/root/usr/bin:$PATH"
vulkaninfo --summary
glslang --version
spirv-as --version
```

Runtime ICDs and the Vulkan loader remain host-provided.
For CMake's FindVulkan, pass cache arguments explicitly:
`-DVulkan_INCLUDE_DIR="$PWD/.vulkan-toolchain/root/usr/include"`
and `-DVulkan_LIBRARY=/lib/aarch64-linux-gnu/libvulkan.so.1`.
The extracted development-package `libvulkan.so` symlink is not a bundled
runtime loader and must not be used as if its target were present locally.

The native compute probe is opt-in and does not enable a production backend:

```sh
cmake -S . -B build-vulkan -G Ninja \
  -DUSDGEN_ENABLE_CUDA=OFF -DUSE_GPU_NOISE=OFF \
  -DUSDGEN_BUILD_VULKAN_TESTS=ON \
  -DVulkan_INCLUDE_DIR="$PWD/.vulkan-toolchain/root/usr/include" \
  -DVulkan_LIBRARY=/lib/aarch64-linux-gnu/libvulkan.so.1 \
  -DUSDGEN_GLSLANG_EXECUTABLE="$PWD/.vulkan-toolchain/root/usr/bin/glslang" \
  -DUSDGEN_SPIRV_VAL_EXECUTABLE="$PWD/.vulkan-toolchain/root/usr/bin/spirv-val"
cmake --build build-vulkan --target testUsdGenVulkanWidth
ctest --test-dir build-vulkan -R '^testUsdGenVulkanWidth$' --output-on-failure
```

The probe selects NVIDIA explicitly. A missing NVIDIA compute device must be
reported as unavailable, not silently replaced by llvmpipe. Shader validation
and a software-driver test do not by themselves prove hardware execution.

The manifest also pins the Khronos validation layer. After building the native
test targets, run `bash scripts/run-vulkan-validation.sh build-vulkan` to enable
core API and synchronization validation (including submit-time checks). This
uses only workspace-local layer files and the host driver. CTest fails on
validation diagnostics even if a driver call and test process return success.
Extra CTest arguments may follow the build directory, for example
`--repeat until-fail:20`. Do not overlap these hardware tests with other GPU
test or sanitizer processes.

Layer configuration follows the matching
[Khronos validation-layer documentation](https://vulkan.lunarg.com/doc/view/1.4.328.1/linux/khronos_validation_layer.html).
This is not GPU-assisted shader instrumentation and does not prove untested
operators, platforms, or production backend availability.
