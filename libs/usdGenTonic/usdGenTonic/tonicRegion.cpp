// usdGenTonic — region rasterisation implementation (K3 host).
#include "usdGenTonic/tonicRegion.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace usdGenTonic {

bool TonicFlattenLoops(TonicScalpGraph const &graph, TonicRegionLoops *loops,
                       std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!loops) {
        return fail("TonicFlattenLoops: null loops");
    }
    loops->points.clear();
    loops->loopBegin.clear();
    loops->loopCount.clear();
    loops->planeN.clear();
    loops->planeP.clear();
    loops->basisU.clear();
    loops->basisV.clear();
    loops->interpIds.clear();
    loops->regionIds.clear();
    loops->valid = false;
    for (auto const &r : graph.Regions()) {
        size_t const n = r.boundary.size() / 3;
        if (n < 3) {
            continue;  // degenerate region: claims nothing
        }
        // Newell normal over the boundary polyline.
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            float const *p0 = &r.boundary[i * 3];
            float const *p1 = &r.boundary[((i + 1) % n) * 3];
            nx += (p0[1] - p1[1]) * (p0[2] + p1[2]);
            ny += (p0[2] - p1[2]) * (p0[0] + p1[0]);
            nz += (p0[0] - p1[0]) * (p0[1] + p1[1]);
        }
        float const nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(nl > 0.0f)) {
            continue;
        }
        nx /= nl;
        ny /= nl;
        nz /= nl;
        float ref[3] = {1.0f, 0.0f, 0.0f};
        if (std::fabs(nx) > 0.9f) {
            ref[0] = 0.0f;
            ref[2] = 1.0f;
        }
        float ux = ref[0] - nx * (ref[0] * nx + ref[2] * nz);
        float uy = -ny * (ref[0] * nx + ref[2] * nz);
        float uz = ref[2] - nz * (ref[0] * nx + ref[2] * nz);
        float const ul = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (!(ul > 0.0f)) {
            continue;
        }
        ux /= ul;
        uy /= ul;
        uz /= ul;
        float const vx = ny * uz - nz * uy;
        float const vy = nz * ux - nx * uz;
        float const vz = nx * uy - ny * ux;
        loops->loopBegin.push_back(int(loops->points.size() / 3));
        loops->loopCount.push_back(int(n));
        loops->points.insert(loops->points.end(), r.boundary.begin(),
                             r.boundary.end());
        loops->planeN.push_back(nx);
        loops->planeN.push_back(ny);
        loops->planeN.push_back(nz);
        loops->planeP.push_back(r.boundary[0]);
        loops->planeP.push_back(r.boundary[1]);
        loops->planeP.push_back(r.boundary[2]);
        loops->basisU.push_back(ux);
        loops->basisU.push_back(uy);
        loops->basisU.push_back(uz);
        loops->basisV.push_back(vx);
        loops->basisV.push_back(vy);
        loops->basisV.push_back(vz);
        loops->interpIds.push_back(r.interpId);
        loops->regionIds.push_back(r.id);
    }
    loops->valid = true;
    return true;
}

bool TonicPointInRegionCpu(TonicRegionLoops const &loops, int region,
                           float const p[3])
{
    if (!loops.valid || region < 0 ||
        size_t(region) >= loops.loopCount.size() || !p) {
        return false;
    }
    float const *n = &loops.planeN[size_t(region) * 3];
    float const *pp = &loops.planeP[size_t(region) * 3];
    float const *u = &loops.basisU[size_t(region) * 3];
    float const *v = &loops.basisV[size_t(region) * 3];
    float d[3] = {p[0] - pp[0], p[1] - pp[1], p[2] - pp[2]};
    // Project along the plane normal (chart-local regions only; the flood
    // fill in TonicRasteriseRegionsCpu rejects far-side strays).
    float const along = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    float q[3] = {p[0] - along * n[0], p[1] - along * n[1],
                  p[2] - along * n[2]};
    float dq[3] = {q[0] - pp[0], q[1] - pp[1], q[2] - pp[2]};
    float const qx = dq[0] * u[0] + dq[1] * u[1] + dq[2] * u[2];
    float const qy = dq[0] * v[0] + dq[1] * v[1] + dq[2] * v[2];
    // Ray-cast (+x) over the projected polygon; boundary counts as inside.
    int const begin = loops.loopBegin[size_t(region)];
    int const count = loops.loopCount[size_t(region)];
    auto proj = [&](int i, float *x, float *y) {
        float const *s = &loops.points[size_t(begin + i) * 3];
        float dd[3] = {s[0] - pp[0], s[1] - pp[1], s[2] - pp[2]};
        *x = dd[0] * u[0] + dd[1] * u[1] + dd[2] * u[2];
        *y = dd[0] * v[0] + dd[1] * v[1] + dd[2] * v[2];
    };
    bool inside = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        float xi, yi, xj, yj;
        proj(i, &xi, &yi);
        proj(j, &xj, &yj);
        // Boundary test: q on segment (i, j) within float tolerance.
        float const ex = xi - xj;
        float const ey = yi - yj;
        float const len2 = ex * ex + ey * ey;
        if (len2 > 0.0f) {
            float t = ((qx - xj) * ex + (qy - yj) * ey) / len2;
            t = std::min(std::max(t, 0.0f), 1.0f);
            float const bx = xj + ex * t - qx;
            float const by = yj + ey * t - qy;
            if (bx * bx + by * by <= 1e-12f * (1.0f + len2)) {
                return true;
            }
        }
        if ((yi > qy) != (yj > qy)) {
            float const xt = xj + (qy - yj) * (xi - xj) / (yi - yj);
            if (qx <= xt) {
                inside = !inside;
            }
        }
    }
    return inside;
}

int TonicClassifyPointCpu(TonicRegionLoops const &loops, float const p[3])
{
    if (!loops.valid || !p) {
        return -1;
    }
    int best = -1;
    for (size_t r = 0; r < loops.loopCount.size(); ++r) {
        if (TonicPointInRegionCpu(loops, int(r), p)) {
            int const id = loops.interpIds[r];
            best = (best < 0) ? id : std::min(best, id);
        }
    }
    return best;
}

bool TonicRasteriseRegionsCpu(TonicScalpMesh const &mesh,
                              TonicScalpGraph const &graph,
                              TonicRegionMaps *maps, std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!maps) {
        return fail("TonicRasteriseRegionsCpu: null maps");
    }
    maps->faceRegion.clear();
    maps->covered.clear();
    maps->intersected.clear();
    maps->uncoveredCount = 0;
    maps->intersectedCount = 0;
    if (!mesh.finalized) {
        return fail("TonicRasteriseRegionsCpu: mesh not finalized");
    }
    size_t const faceCount = mesh.faceVertexCounts.size();
    maps->faceRegion.assign(faceCount, -1);
    maps->faceRegionId.assign(faceCount, -1);
    maps->covered.assign(faceCount, 0);
    maps->intersected.assign(faceCount, 0);
    TonicRegionLoops loops;
    if (!TonicFlattenLoops(graph, &loops, err)) {
        return false;
    }
    // Faces outside a bound face subset are not scalp: never claimed, never
    // counted as uncovered, and never a bridge for the flood below.
    int activeCount = 0;
    for (size_t f = 0; f < faceCount; ++f) {
        activeCount += TonicScalpFaceActive(mesh, int(f)) ? 1 : 0;
    }
    if (loops.loopCount.empty()) {
        maps->uncoveredCount = activeCount;  // no regions: all uncovered
        return true;
    }
    // Candidate inside-sets per region (loop-plane test at face centroids).
    size_t const regionCount = loops.loopCount.size();
    std::vector<std::vector<char>> inside(
        regionCount, std::vector<char>(faceCount, 0));
    for (size_t r = 0; r < regionCount; ++r) {
        for (size_t f = 0; f < faceCount; ++f) {
            float const *c = &mesh.faceCentroids[f * 3];
            inside[r][f] = TonicScalpFaceActive(mesh, int(f)) &&
                                   TonicPointInRegionCpu(loops, int(r), c)
                               ? 1
                               : 0;
        }
    }
    // Connectivity flood per region from its seed: only inside faces
    // reachable from the seed through inside faces are claimed. The seed is
    // the face nearest the loop centroid; when it tests outside (thin
    // crescents), the first inside face seeds instead.
    auto const &regions = graph.Regions();
    for (size_t r = 0; r < regionCount; ++r) {
        int seed = -1;
        int const regionId = loops.regionIds[r];
        if (regionId >= 0 && size_t(regionId) < regions.size()) {
            seed = regions[size_t(regionId)].seedFace;
            if (seed < 0 || size_t(seed) >= faceCount ||
                !inside[r][size_t(seed)]) {
                seed = -1;
            }
        }
        if (seed < 0) {
            for (size_t f = 0; f < faceCount; ++f) {
                if (inside[r][f]) {
                    seed = int(f);
                    break;
                }
            }
        }
        if (seed < 0) {
            continue;  // region claims no face at all (degenerate loop)
        }
        std::vector<char> seen(faceCount, 0);
        std::queue<int> work;
        work.push(seed);
        seen[size_t(seed)] = 1;
        while (!work.empty()) {
            int const f = work.front();
            work.pop();
            // Claim the face for this region (lowest region id wins ties;
            // regions visit in id order, so first claim wins).
            if (maps->faceRegion[size_t(f)] < 0) {
                maps->faceRegion[size_t(f)] = loops.interpIds[r];
                maps->faceRegionId[size_t(f)] = loops.regionIds[r];
                maps->covered[size_t(f)] = 1;
            } else if (maps->faceRegion[size_t(f)] != loops.interpIds[r]) {
                // A second DISTINCT interp id claims this face: root-level
                // intersection (plan/17 requirement 6). Linked regions share
                // one interp id and never raise this bit.
                maps->intersected[size_t(f)] = 1;
                maps->faceRegion[size_t(f)] =
                    std::min(maps->faceRegion[size_t(f)], loops.interpIds[r]);
            }
            for (int g : mesh.faceNeighbours[size_t(f)]) {
                if (!seen[size_t(g)] && inside[r][size_t(g)]) {
                    seen[size_t(g)] = 1;
                    work.push(g);
                }
            }
        }
    }
    for (size_t f = 0; f < faceCount; ++f) {
        if (!TonicScalpFaceActive(mesh, int(f))) {
            continue;
        }
        maps->uncoveredCount += maps->covered[f] ? 0 : 1;
        maps->intersectedCount += maps->intersected[f] ? 1 : 0;
    }
    return true;
}

std::vector<int> TonicFaceResLog2(TonicScalpMesh const &mesh,
                                  TonicRegionMaps const &maps,
                                  TonicRegionLoops const &loops,
                                  int resOverride)
{
    size_t const faceCount = mesh.faceVertexCounts.size();
    std::vector<int> res(faceCount, 2);
    // A face outside a bound subset is uniformly unclaimed: one texel,
    // whatever the override.
    auto collapseInactive = [&]() {
        for (size_t f = 0; f < faceCount; ++f) {
            if (!TonicScalpFaceActive(mesh, int(f))) {
                res[f] = 0;
            }
        }
    };
    if (resOverride >= 0) {
        std::fill(res.begin(), res.end(),
                  std::min(std::max(resOverride, 0), 12));
        collapseInactive();
        return res;
    }
    float const median =
        mesh.medianFaceArea > 0.0f ? mesh.medianFaceArea : 1.0f;
    for (size_t f = 0; f < faceCount; ++f) {
        float const ratio = mesh.faceAreas[f] / median;
        int r = 5;  // 32x32 at the median area (plan/17 §3.1a cost note)
        if (ratio > 0.0f) {
            r = 5 + int(std::floor(std::log2(ratio)));
        }
        r = std::min(std::max(r, 2), 6);
        res[f] = r;
    }
    // A loop can be wholly inside a coarse face, so corner/centroid tests do
    // not prove that its texels are uniform.  First mark every face whose
    // AABB overlaps a boundary segment's AABB.  This is deliberately
    // conservative (it can retain resolution on a neighbouring face), but
    // cannot collapse a face a boundary crosses or encloses.  Those faces
    // need enough samples to retain sub-face region distinctions.
    std::vector<char> hasBoundary(faceCount, 0);
    if (loops.valid) {
        constexpr float kEpsilon = 1e-5f;
        for (size_t r = 0; r < loops.loopCount.size(); ++r) {
            int const begin = loops.loopBegin[r];
            int const count = loops.loopCount[r];
            if (count < 2) {
                continue;
            }
            float loopMin[3] = {loops.points[size_t(begin) * 3],
                                loops.points[size_t(begin) * 3 + 1],
                                loops.points[size_t(begin) * 3 + 2]};
            float loopMax[3] = {loopMin[0], loopMin[1], loopMin[2]};
            for (int i = 1; i < count; ++i) {
                float const *p = &loops.points[size_t(begin + i) * 3];
                for (int axis = 0; axis < 3; ++axis) {
                    loopMin[axis] = std::min(loopMin[axis], p[axis]);
                    loopMax[axis] = std::max(loopMax[axis], p[axis]);
                }
            }
            for (size_t f = 0; f < faceCount; ++f) {
                if (hasBoundary[f]) {
                    continue;
                }
                bool overlapsLoop = true;
                for (int axis = 0; axis < 3; ++axis) {
                    if (loopMax[axis] < mesh.faceMin[f * 3 + axis] -
                                            kEpsilon ||
                        loopMin[axis] > mesh.faceMax[f * 3 + axis] +
                                            kEpsilon) {
                        overlapsLoop = false;
                        break;
                    }
                }
                if (!overlapsLoop) {
                    continue;
                }
                for (int i = 0; i < count; ++i) {
                    float const *a = &loops.points[size_t(begin + i) * 3];
                    float const *b = &loops.points[
                        size_t(begin + ((i + 1) % count)) * 3];
                    bool overlap = true;
                    for (int axis = 0; axis < 3; ++axis) {
                        float const lo = std::min(a[axis], b[axis]);
                        float const hi = std::max(a[axis], b[axis]);
                        if (hi < mesh.faceMin[f * 3 + axis] - kEpsilon ||
                            lo > mesh.faceMax[f * 3 + axis] + kEpsilon) {
                            overlap = false;
                            break;
                        }
                    }
                    if (overlap) {
                        hasBoundary[f] = 1;
                        break;
                    }
                }
            }
        }
    }
    for (size_t f = 0; f < faceCount; ++f) {
        if (hasBoundary[f]) {
            res[f] = std::max(res[f], 6);  // 64x64 on a boundary-bearing face
        }
    }
    // Faces fully inside one region collapse to 1x1 only when no region
    // boundary can affect them.  Corner-exact classification is cheap here
    // (one point test per corner), but is insufficient by itself for a
    // closed small region contained by the face.
    if (loops.valid && maps.faceRegion.size() == faceCount) {
        for (size_t f = 0; f < faceCount; ++f) {
            int const own = maps.faceRegion[f];
            if (own < 0 || maps.intersected[f] || hasBoundary[f]) {
                continue;
            }
            int const nv = mesh.faceVertexCounts[f];
            int const off = mesh.faceOffsets[f];
            bool uniform = true;
            for (int i = 0; i < nv && uniform; ++i) {
                float const *p =
                    mesh.points.data() +
                    size_t(mesh.faceVertexIndices[size_t(off + i)]) * 3;
                uniform = TonicClassifyPointCpu(loops, p) == own;
            }
            if (uniform) {
                res[f] = 0;
            }
        }
    }
    collapseInactive();
    return res;
}

}  // namespace usdGenTonic
