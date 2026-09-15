#ifndef USDGEN_VULKAN_WIDTH_PROFILE_CONTROLS_H
#define USDGEN_VULKAN_WIDTH_PROFILE_CONTROLS_H

namespace usdGen::vulkan {
struct VulkanWidthProfileControls {
    float rootScale = 1.0f;
    float tipScale = 1.0f;
    float taper = 0.0f;
    float taperStart = 0.5f;
    bool IsNeutral() const noexcept {
        return rootScale == 1.0f && tipScale == 1.0f && taper == 0.0f;
    }
};
}
#endif
