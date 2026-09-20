// usdGenTonic — region rasterisation (plan/17 §4.1 K3, P2).
//
// K3 maps graph regions to the scalp at two resolutions from the same inputs:
// per face (the live primvars:usdGen:tonicRegion, every swap) and per Ptex
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
// id and raise the intersection bit; unclaimed faces bake as -1.
//
// Values written are interpolation ids (linked regions share one id), so the
// primvar and the bake agree exactly, which testUsdGenTonicRegionBake proves
// through the real ptex() expression.
//
// This TU is USD-free and Qt-free. The CUDA classifier (tonicKernels.cu,
// TonicLaunchClassifyPoints) evaluates the same point-in-polygon over the
// same flattened loops; TonicClassifyPointCpu is its twin (plan/17 TN-6).
#ifndef USDGEN_TONIC_REGION_H
#define USDGEN_TONIC_REGION_H

#include "usdGenTonic/api.h"
#include "usdGenTonic/tonicGraph.h"
#include "usdGenTonic/tonicScalp.h"

#include <string>
#include <vector>

namespace usdGenTonic {

// Flattened region loops for classification (host mirror of the device
// upload in TonicLaunchClassifyPoints).
struct USDGENTONIC_API TonicRegionLoops {
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

USDGENTONIC_API bool TonicFlattenLoops(TonicScalpGraph const &graph,
                                       TonicRegionLoops *loops,
                                       std::string *err);

// Inside-test of one point against one region (projected ray-cast).
USDGENTONIC_API bool TonicPointInRegionCpu(TonicRegionLoops const &loops,
                                           int region, float const p[3]);
// Full classification of one point: lowest claiming region's interp id, or
// -1 when no region claims it. (No flood fill: the caller constrains by
// connectivity; the GPU twin matches this exactly.)
USDGENTONIC_API int TonicClassifyPointCpu(TonicRegionLoops const &loops,
                                          float const p[3]);

struct USDGENTONIC_API TonicRegionMaps {
    std::vector<int> faceRegion;  // per-face interp id, -1 = uncovered
    std::vector<int> faceRegionId;  // per-face lowest claiming region id
    std::vector<char> covered;  // per-face: claimed by >= 1 region
    std::vector<char> intersected;  // per-face: claimed by >= 2 regions
    int uncoveredCount = 0;
    int intersectedCount = 0;
};

// Per-face rasterisation with the connectivity flood (§ header rule).
USDGENTONIC_API bool TonicRasteriseRegionsCpu(TonicScalpMesh const &mesh,
                                              TonicScalpGraph const &graph,
                                              TonicRegionMaps *maps,
                                              std::string *err);

// Per-face Ptex texel resolution (plan/17 §4.5): log2 of the face area over
// the median, clamped to 2..6 (4..64 per side), artist-overridable; faces
// fully inside one region collapse to 1x1 (resLog2 0). `resOverride >= 0`
// forces one resolution for every face.
USDGENTONIC_API std::vector<int> TonicFaceResLog2(
    TonicScalpMesh const &mesh, TonicRegionMaps const &maps,
    TonicRegionLoops const &loops, int resOverride = -1);

}  // namespace usdGenTonic

#endif  // USDGEN_TONIC_REGION_H
