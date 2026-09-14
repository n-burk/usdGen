#ifndef USDGEN_GPU_GROW_LENGTH_MAP_H
#define USDGEN_GPU_GROW_LENGTH_MAP_H

#include "usdGen/imagePayload.h"

#include <memory>

namespace usdGen::gpu {

// Immutable native Grow length-map input. The producer retains the image
// through upload, sampling, and terminal completion proof.
struct GrowLengthMap {
    std::shared_ptr<const UsdGenImagePayload> image;
    UsdGenImageSampleOptions options{};
};

} // namespace usdGen::gpu

#endif // USDGEN_GPU_GROW_LENGTH_MAP_H
