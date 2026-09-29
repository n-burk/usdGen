// usdGenPomade — region rasterisation (plan/17 §4.1 K3, P2).
//
// K3 maps graph regions to the scalp at two resolutions from the same inputs:
// per face (the live primvars:usdGen:pomadeRegion, every swap) and per Ptex
// texel (the versioned .ptx, baked by the worker). It also produces the
// coverage map (faces with no region) and the root-intersection map (faces
// claimed twice) that the Graph-mode HUD overlays.
//
// Classification rule (our design; plan/17 Gap G4 covers the unpublished
// rasterisation): a point is inside a region when its projection onto the
// region loop's plane (Newell normal of the boundary polyline) falls inside
// the projected boundary polygon (ray-cast, boundary counts as inside), AND
// the face is connected to the region's seed face through inside faces (dual
// flood fill). The flood keeps far-side faces of a closed scalp out of a
// chart-local region. Faces claimed by two regions keep the lowest region
// id and raise the intersection bit; unclaimed faces bake as -1. Faces
// outside a bound face subset are never claimed and never counted as
// uncovered: they are not scalp (plan/02 §2.20).
//
// Values written are interpolation ids (linked regions share one id), so the
// primvar and the bake agree exactly, which testUsdGenPomadeRegionBake proves
// through the real ptex() expression.
//
// This TU is USD-free and Qt-free. The CUDA classifier (pomadeKernels.cu,
// PomadeLaunchClassifyPoints) evaluates the same point-in-polygon over the
// same flattened loops; PomadeClassifyPointCpu is its twin (plan/17 TN-6).
#ifndef USDGEN_POMADE_REGION_H
#define USDGEN_POMADE_REGION_H

#include "usdGenPomade/api.h"
#include "usdGenPomade/pomadeGraph.h"
#include "usdGenPomade/pomadeScalp.h"

#include <string>
#include <vector>

namespace usdGenPomade {

// Flattened region loops for classification (host mirror of the device
// upload in PomadeLaunchClassifyPoints).
struct USDGENPOMADE_API PomadeRegionLoops {
    std::vector<float> points;  // xyz per boundary sample, loop-major
    std::vector<int> loopBegin;  // per region: first sample index
    std::vector<int> loopCount;  // per region: sample count
    std::vector<float> planeN;  // per region: unit Newell normal xyz
    std::vector<float> planeP;  // per region: a point on the plane xyz
    std::vector<float> basisU;  // per region: tangent basis xyz
    std::vector<float> basisV;
    std::vector<int> interpIds;  // per loop: bake value
    std::vector<int> regionIds;  // per loop: source region id (degenerate
                                 // regions are skipped, so loops != regions)
    bool valid = false;
};

USDGENPOMADE_API bool PomadeFlattenLoops(PomadeScalpGraph const &graph,
                                       PomadeRegionLoops *loops,
                                       std::string *err);

// Inside-test of one point against one region (projected ray-cast).
USDGENPOMADE_API bool PomadePointInRegionCpu(PomadeRegionLoops const &loops,
                                           int region, float const p[3]);
// Full classification of one point: lowest claiming region's interp id, or
// -1 when no region claims it. (No flood fill: the caller constrains by
// connectivity; the GPU twin matches this exactly.)
USDGENPOMADE_API int PomadeClassifyPointCpu(PomadeRegionLoops const &loops,
                                          float const p[3]);

struct USDGENPOMADE_API PomadeRegionMaps {
    std::vector<int> faceRegion;  // per-face interp id, -1 = uncovered
    std::vector<int> faceRegionId;  // per-face lowest claiming region id
    std::vector<char> covered;  // per-face: claimed by >= 1 region
    std::vector<char> intersected;  // per-face: claimed by >= 2 regions
    int uncoveredCount = 0;
    int intersectedCount = 0;
};

// Per-face rasterisation with the connectivity flood (§ header rule).
USDGENPOMADE_API bool PomadeRasteriseRegionsCpu(PomadeScalpMesh const &mesh,
                                              PomadeScalpGraph const &graph,
                                              PomadeRegionMaps *maps,
                                              std::string *err);

// Per-face Ptex texel resolution (plan/17 §4.5): log2 of the face area over
// the median, clamped to 2..6 (4..64 per side), artist-overridable.
// Boundary-bearing faces use at least 64x64; only faces proven clear of all
// region boundaries may collapse to 1x1 (resLog2 0). `resOverride >= 0`
// forces one resolution for every face of the growth surface; faces outside
// a bound face subset always bake 1x1 (they are never claimed).
USDGENPOMADE_API std::vector<int> PomadeFaceResLog2(
    PomadeScalpMesh const &mesh, PomadeRegionMaps const &maps,
    PomadeRegionLoops const &loops, int resOverride = -1);

}  // namespace usdGenPomade

#endif  // USDGEN_POMADE_REGION_H
