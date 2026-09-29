// usdGenPomade — test-tube tessellation formula, shared CPU/GPU spelling.
//
// P0 carries only the static test tube (plan/17 §6 P0). The tessellation is a
// straight cylinder along +Y: `rings` cross-sections of `ringVerts` vertices,
// radius `radius`, spanning [0, length]. Ring `r` sits at y = length * r /
// (rings - 1); vertex `i` of ring `r` sits at angle 2*pi*i / ringVerts.
//
// The CPU twin (PomadeTessellateVertex, used by PomadeModel and by every
// GPU-less build) and the CUDA kernel (pomadeKernels.cu, used when
// USDGEN_POMADE_HAS_CUDA) implement this formula; the two must stay identical
// (plan/17 TN-6 states the parity rule that P3 will gate).
#ifndef USDGEN_POMADE_TESSELLATE_H
#define USDGEN_POMADE_TESSELLATE_H

#include <cmath>

#ifdef __CUDACC__
#include <cuda_runtime.h>
#define USDGEN_POMADE_HD __host__ __device__
#else
#define USDGEN_POMADE_HD
#endif

namespace usdGenPomade {

// Plain-C struct so both the C++ model and the CUDA kernel read it.
struct PomadeTubeShape {
    int rings = 5;      // cross-sections along the center (>= 2)
    int ringVerts = 8;  // vertices per ring (>= 3)
    float radius = 0.5f;
    float length = 4.0f;
};

USDGEN_POMADE_HD inline int PomadeTubeVertexCount(PomadeTubeShape const &shape)
{
    return shape.rings * shape.ringVerts;
}

USDGEN_POMADE_HD inline int PomadeTubeQuadCount(PomadeTubeShape const &shape)
{
    return (shape.rings - 1) * shape.ringVerts;
}

// Position and outward normal of vertex `index` (ring-major: ring = index /
// ringVerts, slot = index % ringVerts). Center column at (cx[r], y, cz[r]).
struct PomadeTessellatedVertex {
    float px, py, pz;
    float nx, ny, nz;
};

USDGEN_POMADE_HD inline PomadeTessellatedVertex PomadeTessellateVertex(
    PomadeTubeShape const &shape, int index,
    float const *centerX, float const *centerY, float const *centerZ)
{
    int const ring = index / shape.ringVerts;
    int const slot = index % shape.ringVerts;
    float const twoPi = 6.28318530717958647692f;
    float const angle = twoPi * float(slot) / float(shape.ringVerts);
    // cosf/sinf resolve to the device intrinsics under __CUDA_ARCH__ and to
    // the libm overloads on host; both spellings stay in this header so the
    // CPU twin and the kernel cannot drift.
#ifdef __CUDA_ARCH__
    float const c = cosf(angle);
    float const s = sinf(angle);
#else
    float const c = cosf(angle);
    float const s = sinf(angle);
#endif
    PomadeTessellatedVertex v;
    v.px = centerX[ring] + shape.radius * c;
    v.py = centerY[ring];
    v.pz = centerZ[ring] + shape.radius * s;
    v.nx = c;
    v.ny = 0.0f;
    v.nz = s;
    return v;
}

#undef USDGEN_POMADE_HD

} // namespace usdGenPomade

#endif // USDGEN_POMADE_TESSELLATE_H
