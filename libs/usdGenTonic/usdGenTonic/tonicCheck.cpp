// usdGenTonic — P4 check kernels implementation (K12/K13 CPU twins).
#include "usdGenTonic/tonicCheck.h"

#include <cmath>

namespace usdGenTonic {
namespace {

bool fail(std::string *err, char const *msg)
{
    if (err) {
        *err = msg;
    }
    return false;
}

bool ValidateRootTube(TonicTubeDesc const &tube, std::string *err)
{
    int const nCv = int(tube.centerX.size());
    if (nCv < 2 || tube.centerY.size() != size_t(nCv) ||
        tube.centerZ.size() != size_t(nCv)) {
        return fail(err, "check: bad center CVs");
    }
    if (tube.sections.empty() || tube.ringVerts < 3 ||
        tube.ringVerts > kTonicCheckRingMax) {
        return fail(err, "check: bad sections or ringVerts");
    }
    for (auto const &s : tube.sections) {
        if (int(s.u.size()) != tube.ringVerts ||
            int(s.v.size()) != tube.ringVerts) {
            return fail(err, "check: section ring size mismatch");
        }
    }
    return true;
}

}  // namespace

bool TonicSmoothnessScoreCpu(float const *cx, float const *cy,
                             float const *cz, int nCv, float *out,
                             std::string *err)
{
    if (!cx || !cy || !cz || !out) {
        return fail(err, "TonicSmoothnessScoreCpu: null input or output");
    }
    if (nCv < 1) {
        return fail(err, "TonicSmoothnessScoreCpu: need >= 1 CV");
    }
    out[0] = 0.0f;
    if (nCv == 1) {
        return true;
    }
    out[nCv - 1] = 0.0f;
    for (int i = 1; i + 1 < nCv; ++i) {
        float const p0[3] = {cx[i - 1], cy[i - 1], cz[i - 1]};
        float const p1[3] = {cx[i], cy[i], cz[i]};
        float const p2[3] = {cx[i + 1], cy[i + 1], cz[i + 1]};
        out[i] = TonicKinkScore(p0, p1, p2);
    }
    return true;
}

bool TonicRootChartCpu(TonicTubeDesc const &tube,
                       std::vector<TonicFrame> const &frames, float center[3],
                       float nrm[3], float bin[3],
                       std::vector<float> *worldRing, std::string *err)
{
    if (!center || !nrm || !bin || !worldRing) {
        return fail(err, "TonicRootChartCpu: null output");
    }
    if (!ValidateRootTube(tube, err)) {
        return false;
    }
    int const nCv = int(tube.centerX.size());
    if (frames.size() != size_t(nCv)) {
        return fail(err, "TonicRootChartCpu: frames match center CVs");
    }
    float cp[3];
    TonicFrame fr;
    std::string derr;
    if (!TonicSampleCenterCpu(tube, frames, tube.sections.front().t, &cp[0],
                              &cp[1], &cp[2], &fr, &derr)) {
        return fail(err, derr.c_str());
    }
    center[0] = cp[0];
    center[1] = cp[1];
    center[2] = cp[2];
    nrm[0] = fr.nx;
    nrm[1] = fr.ny;
    nrm[2] = fr.nz;
    bin[0] = fr.bx;
    bin[1] = fr.by;
    bin[2] = fr.bz;
    // Placed root ring (the K5 spelling: scale, then twist rotation).
    TonicTubeSection const &s = tube.sections.front();
    float const sc = s.scale > 1e-6f ? s.scale : 1e-6f;
    float const ct = std::cos(s.twist), st = std::sin(s.twist);
    int const rv = tube.ringVerts;
    worldRing->resize(size_t(rv) * 3);
    for (int i = 0; i < rv; ++i) {
        float const uu = s.u[size_t(i)] * sc;
        float const vv = s.v[size_t(i)] * sc;
        float const ru = uu * ct - vv * st;
        float const rvv = uu * st + vv * ct;
        (*worldRing)[size_t(i) * 3 + 0] = cp[0] + fr.nx * ru + fr.bx * rvv;
        (*worldRing)[size_t(i) * 3 + 1] = cp[1] + fr.ny * ru + fr.by * rvv;
        (*worldRing)[size_t(i) * 3 + 2] = cp[2] + fr.nz * ru + fr.bz * rvv;
    }
    return true;
}

bool TonicRootPairOverlapCpu(float const centerI[3], float const nrmI[3],
                             float const binI[3], float const *worldRingI,
                             int countI, float const *worldRingJ, int countJ,
                             int *outHit, int *broadHit, std::string *err)
{
    if (!centerI || !nrmI || !binI || !worldRingI || !worldRingJ) {
        return fail(err, "TonicRootPairOverlapCpu: null input");
    }
    if (countI < 3 || countI > kTonicCheckRingMax || countJ < 3 ||
        countJ > kTonicCheckRingMax) {
        return fail(err, "TonicRootPairOverlapCpu: ring counts in [3, 32]");
    }
    float minI[3], maxI[3], minJ[3], maxJ[3];
    TonicRingAabb3(worldRingI, countI, minI, maxI);
    TonicRingAabb3(worldRingJ, countJ, minJ, maxJ);
    bool const broad = TonicAabbsOverlap3(minI, maxI, minJ, maxJ);
    if (broadHit) {
        *broadHit = broad ? 1 : 0;
    }
    if (outHit) {
        *outHit = broad ? TonicRootPairNarrowHit(
                              centerI, nrmI, binI, worldRingI, countI,
                              worldRingJ, countJ)
                        : 0;
    }
    return true;
}

bool TonicTubeIntersectCpu(TonicTubeDesc const *tubes, int tubeCount,
                           int *outFlags, std::string *err)
{
    if (tubeCount < 0 || (tubeCount > 0 && !tubes) || !outFlags) {
        return fail(err, "TonicTubeIntersectCpu: null input or output");
    }
    for (int i = 0; i < tubeCount; ++i) {
        outFlags[i] = 0;
    }
    if (tubeCount < 2) {
        return true;
    }
    // One chart + world ring + AABB per tube (AABBs precomputed: pairs do
    // six comparisons, and only broad hits pay for the narrow phase).
    std::vector<float> centers(size_t(tubeCount) * 3), nrms(size_t(tubeCount) *
                                                                3);
    std::vector<float> bins(size_t(tubeCount) * 3);
    std::vector<std::vector<float>> rings(static_cast<size_t>(tubeCount));
    std::vector<float> mins(size_t(tubeCount) * 3),
        maxs(size_t(tubeCount) * 3);
    std::vector<int> ids(static_cast<size_t>(tubeCount)),
        parents(static_cast<size_t>(tubeCount));
    for (int i = 0; i < tubeCount; ++i) {
        ids[size_t(i)] = tubes[i].tubeId;
        parents[size_t(i)] = tubes[i].parentTubeId;
    }
    for (int i = 0; i < tubeCount; ++i) {
        TonicTubeDesc const &tube = tubes[i];
        if (!ValidateRootTube(tube, err)) {
            return false;
        }
        std::vector<TonicFrame> frames;
        std::string derr;
        if (!TonicCenterFramesCpu(tube.centerX.data(), tube.centerY.data(),
                                  tube.centerZ.data(),
                                  int(tube.centerX.size()), &frames, &derr)) {
            return fail(err, derr.c_str());
        }
        if (!TonicRootChartCpu(tube, frames, &centers[size_t(i) * 3],
                               &nrms[size_t(i) * 3], &bins[size_t(i) * 3],
                               &rings[size_t(i)], err)) {
            return false;
        }
        TonicRingAabb3(rings[size_t(i)].data(), tube.ringVerts,
                       &mins[size_t(i) * 3], &maxs[size_t(i) * 3]);
    }
    for (int i = 0; i < tubeCount; ++i) {
        for (int j = i + 1; j < tubeCount; ++j) {
            if (TonicPairRelated(ids.data(), parents.data(), tubeCount, i,
                                 j)) {
                continue;
            }
            if (!TonicAabbsOverlap3(&mins[size_t(i) * 3],
                                    &maxs[size_t(i) * 3],
                                    &mins[size_t(j) * 3],
                                    &maxs[size_t(j) * 3])) {
                continue;
            }
            int const hit = TonicRootPairNarrowHit(
                &centers[size_t(i) * 3], &nrms[size_t(i) * 3],
                &bins[size_t(i) * 3], rings[size_t(i)].data(),
                tubes[i].ringVerts, rings[size_t(j)].data(),
                tubes[j].ringVerts);
            if (hit) {
                outFlags[i] = 1;
                outFlags[j] = 1;
            }
        }
    }
    return true;
}

}  // namespace usdGenTonic
