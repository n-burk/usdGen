// usdGenTonic — P3 CPU twins (K4/K5/K8–K11). Same operation order as the
// CUDA kernels in tonicKernels.cu; TN-6 parity is asserted in T0.
#include "usdGenTonic/tonicTube.h"

#include <algorithm>
#include <cmath>

namespace usdGenTonic {

bool TonicCenterFramesCpu(float const *cx, float const *cy, float const *cz,
                          int nCv, std::vector<TonicFrame> *frames,
                          std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!cx || !cy || !cz || !frames || nCv < 2) {
        return fail("TonicCenterFramesCpu: need >= 2 center CVs");
    }
    frames->resize(size_t(nCv));
    float tPrev[3];
    TonicCenterTangent(cx, cy, cz, nCv, 0, tPrev);
    float n[3];
    TonicPerp3(tPrev, n);
    for (int i = 0; i < nCv; ++i) {
        float t[3];
        TonicCenterTangent(cx, cy, cz, nCv, i, t);
        if (i > 0) {
            float p0[3] = {cx[i - 1], cy[i - 1], cz[i - 1]};
            float p1[3] = {cx[i], cy[i], cz[i]};
            TonicReflectNormal(p0, p1, tPrev, t, n);
            tPrev[0] = t[0];
            tPrev[1] = t[1];
            tPrev[2] = t[2];
        }
        float b[3];
        TonicCross3(t, n, b);
        TonicFrame f;
        f.tx = t[0];
        f.ty = t[1];
        f.tz = t[2];
        f.nx = n[0];
        f.ny = n[1];
        f.nz = n[2];
        f.bx = b[0];
        f.by = b[1];
        f.bz = b[2];
        (*frames)[size_t(i)] = f;
    }
    return true;
}

bool TonicSampleCenterCpu(TonicTubeDesc const &tube,
                          std::vector<TonicFrame> const &frames, float t,
                          float *px, float *py, float *pz, TonicFrame *frame,
                          std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    int const n = int(tube.centerX.size());
    if (n < 2 || !px || !py || !pz || !frame ||
        tube.centerY.size() != size_t(n) ||
        tube.centerZ.size() != size_t(n) || frames.size() != size_t(n)) {
        return fail("TonicSampleCenterCpu: bad tube or frame count");
    }
    float cp[3];
    TonicEvalCenter(tube.centerX.data(), tube.centerY.data(),
                    tube.centerZ.data(), n, t, cp);
    *px = cp[0];
    *py = cp[1];
    *pz = cp[2];
    TonicNlerpFrame(frames.data(), n, t, frame);
    return true;
}

float TonicSectionMeanRadius(TonicTubeSection const &section)
{
    if (section.u.empty()) {
        return 0.0f;
    }
    double acc = 0.0;
    size_t const n = std::min(section.u.size(), section.v.size());
    for (size_t i = 0; i < n; ++i) {
        double const uu = double(section.u[i]) * double(section.scale);
        double const vv = double(section.v[i]) * double(section.scale);
        acc += std::sqrt(uu * uu + vv * vv);
    }
    return n > 0 ? float(acc / double(n)) : 0.0f;
}

int TonicTessellatedRingCount(TonicTubeDesc const &tube, int segmentsPerSpan)
{
    int const nSec = int(tube.sections.size());
    if (nSec < 2 || segmentsPerSpan < 1) {
        return 0;
    }
    return (nSec - 1) * segmentsPerSpan + 1;
}

bool TonicTessellateCpu(TonicTubeDesc const &tube,
                        std::vector<TonicFrame> const &frames,
                        int segmentsPerSpan, std::vector<float> *positions,
                        std::vector<float> *normals,
                        std::vector<float> *ringT, std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    int const nCv = int(tube.centerX.size());
    int const nSec = int(tube.sections.size());
    int const rv = tube.ringVerts;
    if (nCv < 2 || nSec < 2 || rv < 3 || rv > 32 || segmentsPerSpan < 1 ||
        !positions || !normals || !ringT ||
        frames.size() != size_t(nCv)) {
        return fail("TonicTessellateCpu: bad tube, frames or span count");
    }
    for (auto const &s : tube.sections) {
        if (int(s.u.size()) != rv || int(s.v.size()) != rv) {
            return fail("TonicTessellateCpu: section ring size mismatch");
        }
    }
    for (int i = 1; i < nSec; ++i) {
        if (!(tube.sections[size_t(i)].t >= tube.sections[size_t(i - 1)].t)) {
            return fail("TonicTessellateCpu: section t must ascend");
        }
    }
    int const nRings = TonicTessellatedRingCount(tube, segmentsPerSpan);
    positions->resize(size_t(nRings) * size_t(rv) * 3);
    normals->resize(size_t(nRings) * size_t(rv) * 3);
    ringT->resize(size_t(nRings));
    float const t0 = tube.sections.front().t;
    float const t1 = tube.sections.back().t;
    float const span = t1 > t0 ? t1 - t0 : 1.0f;
    for (int r = 0; r < nRings; ++r) {
        float const t = t0 + span * float(r) / float(nRings - 1);
        (*ringT)[size_t(r)] = t;
        int k = 0;
        while (k + 1 < nSec - 1 && tube.sections[size_t(k + 1)].t < t) {
            ++k;
        }
        TonicTubeSection const &s0 = tube.sections[size_t(k)];
        TonicTubeSection const &s1 = tube.sections[size_t(k + 1)];
        TonicTubeSection const &sP = tube.sections[size_t(k > 0 ? k - 1 : k)];
        TonicTubeSection const &sN =
            tube.sections[size_t(k + 2 < nSec ? k + 2 : k + 1)];
        float const dt = s1.t > s0.t ? s1.t - s0.t : 1.0f;
        float f = (t - s0.t) / dt;
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        // Central differences inside, one-sided at the ends (matches
        // TonicEvalCenter; straight tapers reproduce exactly).
        bool const first = (k == 0);
        bool const last = (k + 1 == nSec - 1);
        float const m0sc = first ? s1.scale - s0.scale
                                 : 0.5f * (s1.scale - sP.scale);
        float const m1sc = last ? s1.scale - s0.scale
                                : 0.5f * (sN.scale - s0.scale);
        float const m0tw = first ? s1.twist - s0.twist
                                 : 0.5f * (s1.twist - sP.twist);
        float const m1tw = last ? s1.twist - s0.twist
                                : 0.5f * (sN.twist - s0.twist);
        float sc = TonicHermite(s0.scale, s1.scale, m0sc, m1sc, f);
        float tw = TonicHermite(s0.twist, s1.twist, m0tw, m1tw, f);
        if (!(sc > 1e-6f)) {
            sc = 1e-6f;
        }
        float const ct = std::cos(tw), st = std::sin(tw);
        float cp[3];
        TonicFrame fr;
        std::string derr;
        if (!TonicSampleCenterCpu(tube, frames, t, &cp[0], &cp[1], &cp[2],
                                 &fr, &derr)) {
            return fail(derr.c_str());
        }
        float nA[3] = {fr.nx, fr.ny, fr.nz};
        float bA[3] = {fr.bx, fr.by, fr.bz};
        for (int s = 0; s < rv; ++s) {
            float const m0u =
                first ? s1.u[size_t(s)] - s0.u[size_t(s)]
                      : 0.5f * (s1.u[size_t(s)] - sP.u[size_t(s)]);
            float const m1u =
                last ? s1.u[size_t(s)] - s0.u[size_t(s)]
                     : 0.5f * (sN.u[size_t(s)] - s0.u[size_t(s)]);
            float const m0v =
                first ? s1.v[size_t(s)] - s0.v[size_t(s)]
                      : 0.5f * (s1.v[size_t(s)] - sP.v[size_t(s)]);
            float const m1v =
                last ? s1.v[size_t(s)] - s0.v[size_t(s)]
                     : 0.5f * (sN.v[size_t(s)] - s0.v[size_t(s)]);
            float uu =
                TonicHermite(s0.u[size_t(s)], s1.u[size_t(s)], m0u, m1u, f);
            float vv =
                TonicHermite(s0.v[size_t(s)], s1.v[size_t(s)], m0v, m1v, f);
            uu *= sc;
            vv *= sc;
            float const ru = uu * ct - vv * st;
            float const rvv = uu * st + vv * ct;
            size_t const o = (size_t(r) * size_t(rv) + size_t(s)) * 3;
            (*positions)[o + 0] = cp[0] + nA[0] * ru + bA[0] * rvv;
            (*positions)[o + 1] = cp[1] + nA[1] * ru + bA[1] * rvv;
            (*positions)[o + 2] = cp[2] + nA[2] * ru + bA[2] * rvv;
            float nn[3] = {nA[0] * ru + bA[0] * rvv, nA[1] * ru + bA[1] * rvv,
                           nA[2] * ru + bA[2] * rvv};
            float const nl = TonicLen3(nn);
            if (nl > 1e-12f) {
                (*normals)[o + 0] = nn[0] / nl;
                (*normals)[o + 1] = nn[1] / nl;
                (*normals)[o + 2] = nn[2] / nl;
            } else {
                (*normals)[o + 0] = fr.tx;
                (*normals)[o + 1] = fr.ty;
                (*normals)[o + 2] = fr.tz;
            }
        }
    }
    return true;
}

namespace {

// Dart-throwing state shared by the disc and mesh samplers: candidates are
// drawn in hash order and kept when farther than minDist from every kept
// root (a Poisson-disk approximation; deterministic per (tubeId, seed)).
bool _FarEnough(float x, float y, std::vector<float> const &keptX,
                std::vector<float> const &keptY, float minDist2)
{
    for (size_t i = 0; i < keptX.size(); ++i) {
        float const dx = x - keptX[i], dy = y - keptY[i];
        if (dx * dx + dy * dy < minDist2) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool TonicRootSampleDiscCpu(int tubeId, int seed, float radius,
                            float const center[3], TonicFrame const &frame,
                            int count, std::vector<TonicGuideRoot> *roots,
                            int frozen, std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!center || !roots || count < 0 || frozen < 0 || frozen > count ||
        !(radius > 0.0f)) {
        return fail("TonicRootSampleDiscCpu: bad count, radius or buffers");
    }
    if (frozen > int(roots->size())) {
        return fail("TonicRootSampleDiscCpu: frozen exceeds stored roots");
    }
    roots->resize(size_t(count));
    if (count == 0) {
        return true;
    }
    // Kept-root rejection works in the normalised disc so frozen roots
    // (stored with their ru/rv) seed the stream exactly.
    std::vector<float> keptX, keptY;
    keptX.reserve(size_t(count));
    keptY.reserve(size_t(count));
    for (int i = 0; i < frozen; ++i) {
        keptX.push_back((*roots)[size_t(i)].ru);
        keptY.push_back((*roots)[size_t(i)].rv);
    }
    float const twoPi = 6.28318530717958647692f;
    // Target spacing for `count` blue-noise roots in the unit disc.
    float const minDist =
        count > 1 ? 0.9f * std::sqrt(3.14159265f / float(count)) : 0.0f;
    float const minDist2 = minDist * minDist;
    uint64_t const key =
        (uint64_t(uint32_t(tubeId)) << 32) | uint64_t(uint32_t(seed));
    int const budget = count * 40 + 64;
    int accepted = frozen;
    for (int cand = 0; cand < budget && accepted < count; ++cand) {
        float const u1 = TonicHash01(uint64_t(cand) * 2 + key, kTonicSaltRoot);
        float const u2 =
            TonicHash01(uint64_t(cand) * 2 + 1 + (key ^ 0x85ebca6bu),
                        kTonicSaltRoot);
        float const rr = std::sqrt(u1);
        float const th = twoPi * u2;
        float const ru = rr * std::cos(th), rv = rr * std::sin(th);
        if (minDist2 > 0.0f && !_FarEnough(ru, rv, keptX, keptY, minDist2)) {
            continue;
        }
        keptX.push_back(ru);
        keptY.push_back(rv);
        TonicGuideRoot root;
        root.faceId = -1;
        root.u = ru;
        root.v = rv;
        root.ru = ru;
        root.rv = rv;
        root.px = center[0] + (frame.nx * ru + frame.bx * rv) * radius;
        root.py = center[1] + (frame.ny * ru + frame.by * rv) * radius;
        root.pz = center[2] + (frame.nz * ru + frame.bz * rv) * radius;
        (*roots)[size_t(accepted)] = root;
        ++accepted;
    }
    // Best-effort tail: the budget is generous, but a degenerate (tiny)
    // count must still fill exactly `count` roots.
    for (int i = accepted; i < count; ++i) {
        float const u1 =
            TonicHash01(uint64_t(budget + i) * 2 + key, kTonicSaltRoot);
        float const u2 = TonicHash01(
            uint64_t(budget + i) * 2 + 1 + (key ^ 0x85ebca6bu),
            kTonicSaltRoot);
        float const rr = std::sqrt(u1);
        float const th = twoPi * u2;
        TonicGuideRoot root;
        root.faceId = -1;
        root.ru = rr * std::cos(th);
        root.rv = rr * std::sin(th);
        root.u = root.ru;
        root.v = root.rv;
        root.px = center[0] + (frame.nx * root.ru + frame.bx * root.rv) *
                                  radius;
        root.py = center[1] + (frame.ny * root.ru + frame.by * root.rv) *
                                  radius;
        root.pz = center[2] + (frame.nz * root.ru + frame.bz * root.rv) *
                                  radius;
        (*roots)[size_t(i)] = root;
    }
    return true;
}

bool TonicRootSampleMeshCpu(float const *points, int const *faceCounts,
                            int const *faceIndices, int const *faceOffsets,
                            int faceCount, int const *regionFaces,
                            int regionFaceCount, float density, int tubeId,
                            int seed, float const rootCenter[3],
                            TonicFrame const &rootFrame, float rootRadius,
                            std::vector<TonicGuideRoot> *roots, int frozen,
                            std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!points || !faceCounts || !faceIndices || !faceOffsets ||
        !regionFaces || !rootCenter || !roots || faceCount <= 0 ||
        regionFaceCount <= 0 || !(density >= 0.0f) || frozen < 0 ||
        !(rootRadius > 0.0f)) {
        return fail("TonicRootSampleMeshCpu: bad mesh, density or buffers");
    }
    auto triArea = [&](float const a[3], float const b[3], float const c[3]) {
        float e1[3], e2[3], cr[3];
        TonicSub3(b, a, e1);
        TonicSub3(c, a, e2);
        TonicCross3(e1, e2, cr);
        return 0.5f * TonicLen3(cr);
    };
    uint64_t const key =
        (uint64_t(uint32_t(tubeId)) << 32) | uint64_t(uint32_t(seed));
    std::vector<TonicGuideRoot> fresh;
    for (int rf = 0; rf < regionFaceCount; ++rf) {
        int const f = regionFaces[rf];
        if (f < 0 || f >= faceCount) {
            return fail("TonicRootSampleMeshCpu: region face out of range");
        }
        int const nv = faceCounts[f];
        int const off = faceOffsets[f];
        if (nv < 3) {
            continue;
        }
        float const *v0 = points + size_t(faceIndices[off]) * 3;
        float area = 0.0f;
        for (int k = 1; k + 1 < nv; ++k) {
            float const *v1 = points + size_t(faceIndices[off + k]) * 3;
            float const *v2 = points + size_t(faceIndices[off + k + 1]) * 3;
            area += triArea(v0, v1, v2);
        }
        float const expected = density * area;
        int take = int(expected);
        float const frac = expected - float(take);
        if (TonicHash01(key ^ (uint64_t(f) * 0x9E3779B1u), kTonicSaltRoot) <
            frac) {
            ++take;
        }
        // Per-face dart throwing in barycentric space, rejection in the
        // normalised root plane (shared with the disc path).
        std::vector<float> keptX, keptY;
        float const minDist =
            take > 1 ? 0.9f * std::sqrt(3.14159265f / float(take)) *
                           std::sqrt(area / 3.14159265f) / rootRadius
                     : 0.0f;
        float const minDist2 = minDist * minDist;
        int const budget = take * 40 + 16;
        int accepted = 0;
        auto emit = [&](float ba, float bb) {
            float const bc = 1.0f - ba - bb;
            int tri = int(ba * float(nv - 2));
            if (tri < 0) {
                tri = 0;
            }
            if (tri > nv - 3) {
                tri = nv - 3;
            }
            float const *t1 = points + size_t(faceIndices[off + tri]) * 3;
            float const *t2 =
                points + size_t(faceIndices[off + tri + 1]) * 3;
            float const *t3 =
                points + size_t(faceIndices[off + tri + 2]) * 3;
            float const w1 = std::fabs(bb), w2 = std::fabs(bc);
            float const wsum = std::fabs(ba) + w1 + w2;
            float const i0 = wsum > 0.0f ? std::fabs(ba) / wsum : 1.0f / 3.0f;
            float const i1 = wsum > 0.0f ? w1 / wsum : 1.0f / 3.0f;
            float const i2 = wsum > 0.0f ? w2 / wsum : 1.0f / 3.0f;
            float p[3] = {t1[0] * i0 + t2[0] * i1 + t3[0] * i2,
                          t1[1] * i0 + t2[1] * i1 + t3[1] * i2,
                          t1[2] * i0 + t2[2] * i1 + t3[2] * i2};
            float d[3];
            TonicSub3(p, rootCenter, d);
            float nA[3] = {rootFrame.nx, rootFrame.ny, rootFrame.nz};
            float bA[3] = {rootFrame.bx, rootFrame.by, rootFrame.bz};
            TonicGuideRoot root;
            root.faceId = f;
            root.u = i1;
            root.v = i2;
            root.px = p[0];
            root.py = p[1];
            root.pz = p[2];
            root.ru = TonicDot3(d, nA) / rootRadius;
            root.rv = TonicDot3(d, bA) / rootRadius;
            return root;
        };
        for (int cand = 0; cand < budget && accepted < take; ++cand) {
            uint64_t const ck = key ^ (uint64_t(f) * 0x9E3779B1u) ^
                                (uint64_t(cand) * 0x85EBCA77u);
            float const ba = TonicHash01(ck, kTonicSaltRoot);
            float const bb = TonicHash01(ck ^ 0xC2B2AE35u, kTonicSaltRoot);
            TonicGuideRoot root = emit(ba, bb);
            float rl = std::sqrt(root.ru * root.ru + root.rv * root.rv);
            if (rl > 1.0f) {
                root.ru /= rl;
                root.rv /= rl;
            }
            if (minDist2 > 0.0f &&
                !_FarEnough(root.ru, root.rv, keptX, keptY, minDist2)) {
                continue;
            }
            keptX.push_back(root.ru);
            keptY.push_back(root.rv);
            fresh.push_back(root);
            ++accepted;
        }
        for (int i = accepted; i < take; ++i) {
            uint64_t const ck = key ^ (uint64_t(f) * 0x9E3779B1u) ^
                                (uint64_t(budget + i) * 0x85EBCA77u);
            TonicGuideRoot root =
                emit(TonicHash01(ck, kTonicSaltRoot),
                     TonicHash01(ck ^ 0xC2B2AE35u, kTonicSaltRoot));
            float rl = std::sqrt(root.ru * root.ru + root.rv * root.rv);
            if (rl > 1.0f) {
                root.ru /= rl;
                root.rv /= rl;
            }
            fresh.push_back(root);
        }
    }
    if (frozen > int(roots->size())) {
        return fail("TonicRootSampleMeshCpu: frozen exceeds stored roots");
    }
    std::vector<TonicGuideRoot> out;
    out.reserve(size_t(frozen) + fresh.size());
    for (int i = 0; i < frozen; ++i) {
        out.push_back((*roots)[size_t(i)]);
    }
    for (auto const &r : fresh) {
        out.push_back(r);
    }
    *roots = std::move(out);
    return true;
}

bool TonicGuideFillCpu(TonicTubeDesc const &tube,
                       std::vector<TonicFrame> const &frames,
                       std::vector<TonicGuideRoot> const &roots,
                       TonicFillDesc const &fill, std::vector<float> *points,
                       std::vector<float> *lengthScales, std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    int const nCv = int(tube.centerX.size());
    int const nSec = int(tube.sections.size());
    if (nCv < 2 || nSec < 2 || frames.size() != size_t(nCv) || !points ||
        !lengthScales || fill.cvCount < 2 || fill.cvCount > 64) {
        return fail("TonicGuideFillCpu: bad tube, frames or cvCount");
    }
    if (fill.edgeBias < -1.0f || fill.edgeBias > 1.0f) {
        return fail("TonicGuideFillCpu: edgeBias must be in [-1, 1]");
    }
    if (fill.lengthProfile.size() % 2 != 0) {
        return fail("TonicGuideFillCpu: lengthProfile must hold pairs");
    }
    int const guideCount = int(roots.size());
    int const cvCount = fill.cvCount;
    points->assign(size_t(guideCount) * size_t(cvCount) * 3, 0.0f);
    lengthScales->assign(size_t(guideCount), 1.0f);
    if (guideCount == 0) {
        return true;
    }
    float const rootRadius = TonicSectionMeanRadius(tube.sections.front());
    if (!(rootRadius > 0.0f)) {
        return fail("TonicGuideFillCpu: degenerate root section");
    }
    float const t0 = tube.sections.front().t;
    float const t1 = tube.sections.back().t;
    float const span = t1 > t0 ? t1 - t0 : 1.0f;
    float const biasExp = 1.0f - 0.5f * fill.edgeBias;
    int const pairCount = int(fill.lengthProfile.size() / 2);
    for (int g = 0; g < guideCount; ++g) {
        TonicGuideRoot const &root = roots[size_t(g)];
        float r = std::sqrt(root.ru * root.ru + root.rv * root.rv);
        float th = std::atan2(root.rv, root.ru);
        if (r > 1.0f) {
            r = 1.0f;
        }
        float const rb = std::pow(r, biasExp);
        float const ru = rb * std::cos(th), rv = rb * std::sin(th);
        float length = TonicEvalLengthProfile(fill.lengthProfile.data(),
                                              pairCount, rb);
        if (!(length > 0.0f)) {
            length = 0.0f;
        }
        if (length > 1.0f) {
            length = 1.0f;
        }
        (*lengthScales)[size_t(g)] = length;
        for (int c = 0; c < cvCount; ++c) {
            float const s = float(c) / float(cvCount - 1);
            float const t = t0 + span * s * length;
            float cp[3];
            TonicFrame fr;
            std::string derr;
            if (!TonicSampleCenterCpu(tube, frames, t, &cp[0], &cp[1], &cp[2],
                                     &fr, &derr)) {
                return fail(derr.c_str());
            }
            // The root offset rides up the tube through the section
            // interpolation: scale by the local mean radius so guides
            // expand and contract with the tube.
            int k = 0;
            while (k + 1 < nSec - 1 && tube.sections[size_t(k + 1)].t < t) {
                ++k;
            }
            TonicTubeSection const &s0 = tube.sections[size_t(k)];
            TonicTubeSection const &s1 = tube.sections[size_t(k + 1)];
            float f = (s1.t > s0.t) ? (t - s0.t) / (s1.t - s0.t) : 0.0f;
            f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
            float const r0 = TonicSectionMeanRadius(s0);
            float const r1 = TonicSectionMeanRadius(s1);
            float const rr = (r0 + (r1 - r0) * f) / rootRadius;
            size_t const o = (size_t(g) * size_t(cvCount) + size_t(c)) * 3;
            (*points)[o + 0] =
                cp[0] + (fr.nx * ru + fr.bx * rv) * rootRadius * rr;
            (*points)[o + 1] =
                cp[1] + (fr.ny * ru + fr.by * rv) * rootRadius * rr;
            (*points)[o + 2] =
                cp[2] + (fr.nz * ru + fr.bz * rv) * rootRadius * rr;
        }
    }
    return true;
}

bool TonicGuideResampleCpu(float const *points, int const *counts,
                           int guideCount, int cvCount,
                           float const rootDirs[9], std::vector<float> *out,
                           std::vector<int> *outCounts,
                           std::vector<double> *outFrames, std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!points || !counts || !rootDirs || !out || !outCounts || !outFrames ||
        guideCount < 0 || cvCount < 2 || cvCount > 64) {
        return fail("TonicGuideResampleCpu: bad counts or buffers");
    }
    out->assign(size_t(guideCount) * size_t(cvCount) * 3, 0.0f);
    outCounts->assign(size_t(guideCount), cvCount);
    outFrames->assign(size_t(guideCount) * 16, 0.0);
    size_t cursor = 0;
    for (int g = 0; g < guideCount; ++g) {
        int const n = counts[g];
        if (n < 2) {
            return fail("TonicGuideResampleCpu: guide needs >= 2 CVs");
        }
        float const *src = points + cursor * 3;
        cursor += size_t(n);
        std::vector<float> cum(size_t(n), 0.0f);
        for (int i = 1; i < n; ++i) {
            float dx = src[size_t(i) * 3 + 0] - src[size_t(i - 1) * 3 + 0];
            float dy = src[size_t(i) * 3 + 1] - src[size_t(i - 1) * 3 + 1];
            float dz = src[size_t(i) * 3 + 2] - src[size_t(i - 1) * 3 + 2];
            cum[size_t(i)] =
                cum[size_t(i - 1)] + std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        float const total = cum.back();
        for (int c = 0; c < cvCount; ++c) {
            float const s = float(c) / float(cvCount - 1) * total;
            int seg = 0;
            while (seg + 1 < n - 1 && cum[size_t(seg + 1)] < s) {
                ++seg;
            }
            float const c0 = cum[size_t(seg)], c1 = cum[size_t(seg + 1)];
            float f = (c1 > c0) ? (s - c0) / (c1 - c0) : 0.0f;
            f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
            size_t const o = (size_t(g) * size_t(cvCount) + size_t(c)) * 3;
            (*out)[o + 0] = src[size_t(seg) * 3 + 0] +
                             (src[size_t(seg + 1) * 3 + 0] -
                              src[size_t(seg) * 3 + 0]) *
                                 f;
            (*out)[o + 1] = src[size_t(seg) * 3 + 1] +
                             (src[size_t(seg + 1) * 3 + 1] -
                              src[size_t(seg) * 3 + 1]) *
                                 f;
            (*out)[o + 2] = src[size_t(seg) * 3 + 2] +
                             (src[size_t(seg + 1) * 3 + 2] -
                              src[size_t(seg) * 3 + 2]) *
                                 f;
        }
        // Root frame (UsdGenCurveAPI row-major): rows 0/1/2 are the tube
        // root N/B/T directions, row 3 is the root position.
        size_t const f = size_t(g) * 16;
        (*outFrames)[f + 0] = rootDirs[0];
        (*outFrames)[f + 1] = rootDirs[1];
        (*outFrames)[f + 2] = rootDirs[2];
        (*outFrames)[f + 3] = 0.0;
        (*outFrames)[f + 4] = rootDirs[3];
        (*outFrames)[f + 5] = rootDirs[4];
        (*outFrames)[f + 6] = rootDirs[5];
        (*outFrames)[f + 7] = 0.0;
        (*outFrames)[f + 8] = rootDirs[6];
        (*outFrames)[f + 9] = rootDirs[7];
        (*outFrames)[f + 10] = rootDirs[8];
        (*outFrames)[f + 11] = 0.0;
        (*outFrames)[f + 12] = src[0];
        (*outFrames)[f + 13] = src[1];
        (*outFrames)[f + 14] = src[2];
        (*outFrames)[f + 15] = 1.0;
    }
    return true;
}

TonicPickHit TonicPickCpu(TonicPickSets const &sets, uint32_t kindMask,
                          float const viewProj[16], int w, int h, float x,
                          float y, float radiusPx)
{
    TonicPickHit best;
    if (!viewProj || w <= 0 || h <= 0 || !(radiusPx >= 0.0f)) {
        return best;
    }
    float const r2 = radiusPx * radiusPx;
    auto consider = [&](uint32_t kind, float const *p, int index,
                        int subIndex) {
        if (!(kindMask & kind) || !p) {
            return;
        }
        float px, py, ndcZ;
        if (!TonicProjectPoint(p, viewProj, w, h, &px, &py, &ndcZ)) {
            return;
        }
        float const dx = px - x, dy = py - y;
        float const d2 = dx * dx + dy * dy;
        if (d2 > r2) {
            return;
        }
        float const dist = std::sqrt(d2);
        if (!best.hit || dist < best.distPx ||
            (dist == best.distPx && ndcZ < best.depth)) {
            best.hit = true;
            best.kind = kind;
            best.index = index;
            best.subIndex = subIndex;
            best.distPx = dist;
            best.depth = ndcZ;
        }
    };
    for (int i = 0; i < sets.tubeVertCount; ++i) {
        consider(TonicPick_TubeVert, sets.tubeVerts + size_t(i) * 3, i, -1);
    }
    for (int i = 0; i < sets.centerCVCount; ++i) {
        consider(TonicPick_CenterCV, sets.centerCVs + size_t(i) * 3, i, -1);
    }
    for (int i = 0; i < sets.sectionCVCount; ++i) {
        int sub = -1;
        if (sets.sectionRingVerts > 0) {
            sub = i % sets.sectionRingVerts;
            consider(TonicPick_SectionCV, sets.sectionCVs + size_t(i) * 3,
                     i / sets.sectionRingVerts, sub);
        } else {
            consider(TonicPick_SectionCV, sets.sectionCVs + size_t(i) * 3, i,
                     -1);
        }
    }
    for (int i = 0; i < sets.graphNodeCount; ++i) {
        consider(TonicPick_GraphNode, sets.graphNodes + size_t(i) * 3, i, -1);
    }
    for (int g = 0; g < sets.guideCount; ++g) {
        for (int c = 0; c < sets.guideCvCount; ++c) {
            consider(TonicPick_Guide,
                     sets.guideCVs +
                         (size_t(g) * size_t(sets.guideCvCount) + size_t(c)) *
                             3,
                     g, c);
        }
    }
    return best;
}

}  // namespace usdGenTonic
