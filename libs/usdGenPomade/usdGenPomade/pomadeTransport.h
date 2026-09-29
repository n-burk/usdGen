// usdGenPomade — host-staged transport to Storm (plan/17 §4.3 Phase A).
//
// Device -> pinned host -> VtArray. Pinned staging keeps the D2H copy off the
// pageable path; the VtArray fill is the one copy Hydra 2.0 retained data
// sources require (there is no external-buffer aliasing in VtArray, so the
// "no second copy" HdVtBufferSource form stays a Hydra 1.x note). Phase B
// (CUDA-GL interop through UsdGenCudaGlComputation) replaces only this TU's
// staging, never the model or the publisher's locator contract.
#ifndef USDGEN_POMADE_TRANSPORT_H
#define USDGEN_POMADE_TRANSPORT_H

#include "usdGenPomade/api.h"

#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"

#include <cstddef>
#include <cstdint>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenPomade {

class PomadeModel;

// Page-locked staging block. Uses cudaHostAlloc when the CUDA mirror exists,
// aligned malloc otherwise; identical copy semantics either way.
class USDGENPOMADE_API PomadePinnedStaging {
public:
    PomadePinnedStaging();
    ~PomadePinnedStaging();
    PomadePinnedStaging(PomadePinnedStaging const &) = delete;
    PomadePinnedStaging &operator=(PomadePinnedStaging const &) = delete;

    // Resize (preserving nothing) and return the write pointer, or null on
    // allocation failure.
    float *Reset(size_t floatCount);
    // Grow-only form (plan/18 §2.2): keeps the block when it is already big
    // enough, so a gesture allocates on its first frame and never again.
    // The returned pointer is valid for at least `floatCount` floats.
    float *Ensure(size_t floatCount);
    float *Data() { return _data; }
    float const *Data() const { return _data; }
    size_t FloatCount() const { return _floatCount; }
    bool IsPinned() const { return _pinned; }

private:
    float *_data = nullptr;
    size_t _floatCount = 0;
    bool _pinned = false;
};

// One published snapshot of the test tube as Hydra-ready arrays. Produced by
// PomadeStageTubeMesh, consumed by the scene index's data-source assembler.
struct USDGENPOMADE_API PomadeStagedTubeMesh {
    VtVec3fArray points;
    VtVec3fArray normals;
    VtIntArray faceVertexCounts;
    VtIntArray faceVertexIndices;
    GfVec3f extentMin;
    GfVec3f extentMax;
    uint64_t version = 0;
};

// Stage the model's current tube through pinned host memory into `out`.
// Returns false (leaving `out` untouched) only when the model holds no tube.
bool USDGENPOMADE_API PomadeStageTubeMesh(PomadeModel const &model,
                                       PomadePinnedStaging *positions,
                                       PomadePinnedStaging *normals,
                                       PomadeStagedTubeMesh *out);

} // namespace usdGenPomade

#endif // USDGEN_POMADE_TRANSPORT_H
