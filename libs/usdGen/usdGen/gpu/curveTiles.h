// Stable capture-order tile ranges for compacted CUDA curve geometry.
#ifndef USDGEN_GPU_CURVE_TILES_H
#define USDGEN_GPU_CURVE_TILES_H

#include "deviceBuffer.h"

#include <cstddef>
#include <cstdint>

namespace usdGen::gpu {

struct CurveTileOptions {
    uint32_t chunkSize = 512;
    uint32_t tileTarget = 64;
};

// All ranges are local to the compacted survivor geometry.  `tile` is the
// stable capture-order tile ordinal; a range may be empty after culling.
struct CurveTileSpan {
    uint32_t tile = 0;
    uint32_t firstCurve = 0;
    uint32_t curveCount = 0;
    uint32_t firstPoint = 0;
    uint32_t pointCount = 0;
};

struct CurveTileInput {
    DeviceView<const uint64_t> captureStableIds;
    DeviceView<const uint64_t> survivorStableIds;
    DeviceView<const uint32_t> survivorCurveOffsets;
    size_t captureCurveCount = 0;
    size_t survivorCurveCount = 0;
    size_t survivorPointCount = 0;
};

struct CurveTileRequirements {
    size_t chunkCount = 0;
    size_t tileCount = 0;
    size_t chunksPerTile = 0;
    size_t curvesPerTile = 0;
    uint32_t chunkSize = 0;
    uint32_t tileTarget = 0;
    int deviceIndex = -1;
    cudaStream_t stream = nullptr;
};

struct CurveTileOutput {
    DeviceView<CurveTileSpan> spans; // capacity >= requirements.tileCount
    DeviceView<uint32_t> status;     // one scalar: 0 valid, 1 invalid input
};

// Setup-only scalar arithmetic.  Zero selects the 512/64 defaults; other
// values clamp chunkSize to [128,1024] and tileTarget to [32,256].  It binds
// the current device and (when supplied) stream, and never reads device
// geometry or allocates.
cudaError_t GetCurveTileRequirements(CurveTileOptions options,
    size_t captureCurveCount, size_t survivorCurveCount,
    size_t survivorPointCount, CurveTileRequirements* result,
    cudaStream_t stream = nullptr);

// Enqueue-only borrowed-device-view builder.  Capture IDs and survivor IDs
// must be strictly increasing; survivors must be an ordered subset of capture
// IDs.  Survivor offsets must be canonical nonempty-curve offsets.  Invalid
// device data sets status=1 and leaves spans untouched.  Tile membership is
// frozen by capture ordinal: every emitted tile initially owns at least one
// capture chunk, and empty post-cull tiles are emitted deliberately.
// No allocation, host wait/readback, locks, or geometry/index/draw-count
// publication occurs here.  Prepare requirements before CUDA graph capture.
cudaError_t BuildCurveTiles(CurveTileInput input,
    CurveTileRequirements const& requirements, CurveTileOutput output,
    cudaStream_t stream);

} // namespace usdGen::gpu

#endif // USDGEN_GPU_CURVE_TILES_H
