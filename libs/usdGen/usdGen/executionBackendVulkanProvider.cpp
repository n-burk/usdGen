// Small capability-version bridge for the neutral backend contract.
//
// Mirrors executionBackendCudaProvider.cpp: the neutral registry must not
// include a concrete native adapter header. The typed Vulkan plan compiler
// pins capability version 1 (planExecutor.h Request::capabilityVersion).
#include <cstdint>

namespace usdGen {

uint32_t UsdGenVulkanCapabilityVersionForBackendContract() noexcept
{
    return 1;
}

} // namespace usdGen
