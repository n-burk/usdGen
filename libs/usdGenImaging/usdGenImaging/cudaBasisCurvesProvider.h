// usdGen CUDA BasisCurves provider for the private Storm extension.
#ifndef USDGEN_IMAGING_CUDA_BASIS_CURVES_PROVIDER_H
#define USDGEN_IMAGING_CUDA_BASIS_CURVES_PROVIDER_H

#include "pxr/pxr.h"

#if defined(USDGEN_HAS_CUDA_GL_INTEROP) && \
    __has_include("pxr/imaging/hdSt/basisCurvesGpuDataSource.h")

#include "usdGen/deviceGeneration.h"
#include "pxr/imaging/hdSt/basisCurvesGpuDataSource.h"
#include "pxr/base/gf/bbox3d.h"

#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

/// Immutable app-side adapter for one CUDA curve generation.  Bounds are an
/// explicit conservative caller contract in this first path: the bridge does
/// not download or infer GPU bounds.  The private Storm extension consumes
/// the returned bundle only after its post-Commit Ready check.
class UsdGenCudaBasisCurvesProvider final
    : public HdStBasisCurvesGpuDataSource
{
public:
    struct CreateInfo {
        std::shared_ptr<const usdGen::UsdGenDeviceGeneration> generation;
        TfToken curveType;
        TfToken curveBasis;
        TfToken curveWrap;
        GfBBox3d conservativeBounds;
        bool basisWidthInterpolation = false;
        bool basisNormalInterpolation = false;
    };

    explicit UsdGenCudaBasisCurvesProvider(CreateInfo info);

    HdStBasisCurvesGpuBundleSharedPtr Prepare(
        HdStResourceRegistry *registry,
        HdStBasisCurvesGpuPrepareRequest const &request) override;

private:
    CreateInfo const _info;
};

} // namespace usdGenImaging

#endif
#endif
