// Immutable CUDA generation -> renderer-neutral GPU group control.
#ifndef USDGEN_IMAGING_DEVICE_CURVE_GROUP_PUBLISHER_H
#define USDGEN_IMAGING_DEVICE_CURVE_GROUP_PUBLISHER_H

#include "usdGenImaging/api.h"

#if defined(USDGEN_HAS_CUDA_GL_INTEROP) && \
    __has_include("pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h")

#include "usdGen/generationStore.h"
#include "pxr/imaging/hdSt/basisCurvesGpuGroupDataSource.h"

#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

/// Builds the opaque upstream control for a CUDA device generation.  It reads
/// only immutable scalar presentation and device tile metadata; the returned
/// providers defer every CUDA/GL operation until HdSt's normal Prepare path.
class UsdGenDeviceCurveGroupPublisher final
{
public:
    struct Result {
        HdStBasisCurvesGpuGroupDataSourceHandle control;
        std::string reason;
        explicit operator bool() const { return bool(control); }
    };

    /// expectedDescription is the exact descriptor scope. ticket must be
    /// nonzero.  A null result fails closed and describes no partial group.
    static Result Build(
        usdGen::UsdGenGenerationConstPtr const &generation,
        SdfPath const &expectedDescription, uint64_t ticket,
        bool ownsSubtree = false);
};

} // namespace usdGenImaging

#endif
#endif // USDGEN_IMAGING_DEVICE_CURVE_GROUP_PUBLISHER_H
