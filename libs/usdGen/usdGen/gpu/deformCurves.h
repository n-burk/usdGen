#ifndef USDGEN_GPU_DEFORM_CURVES_H
#define USDGEN_GPU_DEFORM_CURVES_H
#include "curveGeometry.h"
#include "rbf.h"
namespace usdGen { namespace gpu {
class CudaRbfCurveDeformer {
public:
 ~CudaRbfCurveDeformer();
 // primitive/point envelopes may be empty (identity 1); rootTargets is one per curve.
 RbfStatus Deform(CudaRbfBinding& rbf, DeviceCurveGeometryView geometry, DeviceView<const float3> rootTargets, float groomEnvelope, DeviceView<const float> primitiveEnvelope, DeviceView<const float> pointEnvelope, DeviceView<float3> output, cudaStream_t stream);
 RbfStatus Finish(CudaRbfBinding& rbf,cudaStream_t stream);
private: DeviceBuffer<float3> warped_; DeviceBuffer<int> flags_; cudaEvent_t ready_=nullptr; bool pending_=false, poisoned_=false;
};
}}
#endif
