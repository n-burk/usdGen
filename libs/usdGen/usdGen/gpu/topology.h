#ifndef USDGEN_GPU_TOPOLOGY_H
#define USDGEN_GPU_TOPOLOGY_H

#include "curveGeometry.h"

namespace usdGen::gpu {

// Compares only the layout identity of two curve sets.  Point positions and
// all other per-point channels are deliberately not inspected.  The function
// synchronously publishes only the scalar result; geometry never crosses to
// the host.  cudaErrorInvalidValue denotes a malformed topology (including
// bad offset sentinels), while a well-formed but different topology returns
// cudaSuccess and sets equal=false.
cudaError_t CompareCurveTopology(DeviceCurveGeometryView a,
                                 DeviceCurveGeometryView b,
                                 cudaStream_t stream, bool* equal);

} // namespace usdGen::gpu

#endif // USDGEN_GPU_TOPOLOGY_H
