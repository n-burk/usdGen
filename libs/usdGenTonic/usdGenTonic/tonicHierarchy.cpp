// usdGenTonic — P4 hierarchy core implementation (K14/K7 CPU twins).
#include "usdGenTonic/tonicHierarchy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace usdGenTonic {
namespace {

// K5 tessellation caps ringVerts at 32; hierarchy output honours the same
// cap (fat clips uniformize down to it, documented in the header).
constexpr int kMaxRingVerts = 32;
constexpr int kKMeansIters = 64;

bool fail(std::string *err, char const *msg)
{
    if (err) {
        *err = msg;
    }
    return false;
}

bool ValidateTube(TonicTubeDesc const &tube, char const *who, std::string *err)
{
    int const nCv = int(tube.centerX.size());
    int const nSec = int(tube.sections.size());
    if (nCv < 2 || tube.centerY.size() != size_t(nCv) ||
        tube.centerZ.size() != size_t(nCv)) {
        return fail(err, "hierarchy: bad center CVs");
    }
    if (nSec < 2 || tube.ringVerts < 3 || tube.ringVerts > 256) {
        return fail(err, "hierarchy: bad sections or ringVerts");
    }
    for (auto const &s : tube.sections) {
        if (int(s.u.size()) != tube.ringVerts ||
            int(s.v.size()) != tube.ringVerts) {
            return fail(err, "hierarchy: section ring size mismatch");
        }
    }
    for (int i = 1; i < nSec; ++i) {
        if (!(tube.sections[size_t(i)].t >= tube.sections[size_t(i - 1)].t)) {
            return fail(err, "hierarchy: section t must ascend");
        }
    }
    (void)who;
    return true;
}

struct Pt {
    float u = 0.0f, v = 0.0f;
};

// Placed ring coords: the K5 spelling (scale, then twist rotation).
void PlaceRing(TonicTubeSection const &s, std::vector<Pt> *out)
{
    float const sc = s.scale > 1e-6f ? s.scale : 1e-6f;
    float const ct = std::cos(s.twist), st = std::sin(s.twist);
    out->resize(s.u.size());
    for (size_t i = 0; i < s.u.size(); ++i) {
        float const uu = s.u[i] * sc, vv = s.v[i] * sc;
        (*out)[i].u = uu * ct - vv * st;
        (*out)[i].v = uu * st + vv * ct;
    }
}

float MeanRadius(std::vector<Pt> const &pts)
{
    if (pts.empty()) {
        return 0.0f;
    }
    double su = 0.0, sv = 0.0;
    for (auto const &p : pts) {
        su += p.u;
        sv += p.v;
    }
    double const cu = su / double(pts.size()), cv = sv / double(pts.size());
    double acc = 0.0;
    for (auto const &p : pts) {
        double const du = double(p.u) - cu, dv = double(p.v) - cv;
        acc += std::sqrt(du * du + dv * dv);
    }
    return float(acc / double(pts.size()));
}

void RingMean(std::vector<Pt> const &pts, double *meanU, double *meanV)
{
    if (pts.empty()) {
        *meanU = 0.0;
        *meanV = 0.0;
        return;
    }
    double su = 0.0, sv = 0.0;
    for (auto const &p : pts) {
        su += p.u;
        sv += p.v;
    }
    *meanU = su / double(pts.size());
    *meanV = sv / double(pts.size());
}

// Section ring interpolated at t: Hermite per slot with the K5 bracketing
// and end rules (central inside, one-sided at the ends); scale/twist take
// the same path as scalars.
void InterpSectionAt(std::vector<TonicTubeSection> const &secs, int rv,
                     float t, std::vector<Pt> *placed, float *radius)
{
    int const nSec = int(secs.size());
    int k = 0;
    while (k + 1 < nSec - 1 && secs[size_t(k + 1)].t < t) {
        ++k;
    }
    TonicTubeSection const &s0 = secs[size_t(k)];
    TonicTubeSection const &s1 = secs[size_t(k + 1)];
    TonicTubeSection const &sP = secs[size_t(k > 0 ? k - 1 : k)];
    TonicTubeSection const &sN = secs[size_t(k + 2 < nSec ? k + 2 : k + 1)];
    float const dt = s1.t > s0.t ? s1.t - s0.t : 1.0f;
    float f = (t - s0.t) / dt;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    bool const first = (k == 0), last = (k + 1 == nSec - 1);
    auto slope = [&](std::vector<float> const &a, std::vector<float> const &b,
                     std::vector<float> const &p, std::vector<float> const &n,
                     int s, bool m0) {
        if (m0) {
            return first ? b[size_t(s)] - a[size_t(s)]
                         : 0.5f * (b[size_t(s)] - p[size_t(s)]);
        }
        return last ? b[size_t(s)] - a[size_t(s)]
                    : 0.5f * (n[size_t(s)] - a[size_t(s)]);
    };
    // Scalar Hermite needs vectors; spell the two scalars directly.
    float const m0sc = first ? s1.scale - s0.scale
                             : 0.5f * (s1.scale - sP.scale);
    float const m1sc =
        last ? s1.scale - s0.scale : 0.5f * (sN.scale - s0.scale);
    float const m0tw =
        first ? s1.twist - s0.twist : 0.5f * (s1.twist - sP.twist);
    float const m1tw =
        last ? s1.twist - s0.twist : 0.5f * (sN.twist - s0.twist);
    auto herm = [](float a, float b, float m0, float m1, float u) {
        float const u2 = u * u, u3 = u2 * u;
        return (2.0f * u3 - 3.0f * u2 + 1.0f) * a +
               (u3 - 2.0f * u2 + u) * m0 + (-2.0f * u3 + 3.0f * u2) * b +
               (u3 - u2) * m1;
    };
    float sc = herm(s0.scale, s1.scale, m0sc, m1sc, f);
    float const tw = herm(s0.twist, s1.twist, m0tw, m1tw, f);
    if (!(sc > 1e-6f)) {
        sc = 1e-6f;
    }
    float const ct = std::cos(tw), st = std::sin(tw);
    placed->resize(size_t(rv));
    for (int s = 0; s < rv; ++s) {
        float const uu =
            herm(s0.u[size_t(s)], s1.u[size_t(s)],
                 slope(s0.u, s1.u, sP.u, sN.u, s, true),
                 slope(s0.u, s1.u, sP.u, sN.u, s, false), f) *
            sc;
        float const vv =
            herm(s0.v[size_t(s)], s1.v[size_t(s)],
                 slope(s0.v, s1.v, sP.v, sN.v, s, true),
                 slope(s0.v, s1.v, sP.v, sN.v, s, false), f) *
            sc;
        (*placed)[size_t(s)].u = uu * ct - vv * st;
        (*placed)[size_t(s)].v = uu * st + vv * ct;
    }
    if (radius) {
        *radius = MeanRadius(*placed);
    }
}

// Deterministic k-means over pts (k-means++ init from the Tonic hash
// stream keyed by key, Lloyd's to convergence). assign[i] in [0, k).
void KMeans(std::vector<Pt> const &pts, int k, uint64_t key,
            std::vector<int> *assign, std::vector<Pt> *centroids)
{
    int const n = int(pts.size());
    assign->assign(size_t(n), 0);
    centroids->resize(size_t(k));
    // k-means++: first center = hash draw, rest by squared distance.
    uint64_t stream = key;
    auto draw = [&]() { return TonicHash01(stream++, kTonicSaltRoot); };
    (*centroids)[0] = pts[size_t(draw() * float(n)) % size_t(n)];
    std::vector<double> best(size_t(n), std::numeric_limits<double>::max());
    for (int c = 1; c < k; ++c) {
        for (int i = 0; i < n; ++i) {
            double const du = double(pts[size_t(i)].u) -
                              double((*centroids)[size_t(c - 1)].u);
            double const dv = double(pts[size_t(i)].v) -
                              double((*centroids)[size_t(c - 1)].v);
            double const d2 = du * du + dv * dv;
            if (d2 < best[size_t(i)]) {
                best[size_t(i)] = d2;
            }
        }
        double total = 0.0;
        for (double b : best) {
            total += b;
        }
        int pick = 0;
        if (total > 0.0) {
            double const r = double(draw()) * total;
            double acc = 0.0;
            for (int i = 0; i < n; ++i) {
                acc += best[size_t(i)];
                if (acc >= r) {
                    pick = i;
                    break;
                }
            }
        } else {
            pick = int(draw() * float(n)) % n;
        }
        (*centroids)[size_t(c)] = pts[size_t(pick)];
    }
    // Lloyd's.
    std::vector<int> counts(size_t(k), 0);
    for (int it = 0; it < kKMeansIters; ++it) {
        bool changed = false;
        for (int i = 0; i < n; ++i) {
            int bc = 0;
            double bd = std::numeric_limits<double>::max();
            for (int c = 0; c < k; ++c) {
                double const du = double(pts[size_t(i)].u) -
                                  double((*centroids)[size_t(c)].u);
                double const dv = double(pts[size_t(i)].v) -
                                  double((*centroids)[size_t(c)].v);
                double const d2 = du * du + dv * dv;
                if (d2 < bd) {  // strict: lowest index wins ties
                    bd = d2;
                    bc = c;
                }
            }
            if ((*assign)[size_t(i)] != bc) {
                (*assign)[size_t(i)] = bc;
                changed = true;
            }
        }
        std::fill(counts.begin(), counts.end(), 0);
        std::vector<double> su(size_t(k), 0.0), sv(size_t(k), 0.0);
        for (int i = 0; i < n; ++i) {
            int const c = (*assign)[size_t(i)];
            counts[size_t(c)]++;
            su[size_t(c)] += pts[size_t(i)].u;
            sv[size_t(c)] += pts[size_t(i)].v;
        }
        // Empty rescue: farthest point from its centroid, lowest index.
        for (int c = 0; c < k; ++c) {
            if (counts[size_t(c)] == 0) {
                int fi = 0;
                double fd = -1.0;
                for (int i = 0; i < n; ++i) {
                    int const oc = (*assign)[size_t(i)];
                    double const du = double(pts[size_t(i)].u) -
                                      double((*centroids)[size_t(oc)].u);
                    double const dv = double(pts[size_t(i)].v) -
                                      double((*centroids)[size_t(oc)].v);
                    double const d2 = du * du + dv * dv;
                    if (d2 > fd) {
                        fd = d2;
                        fi = i;
                    }
                }
                (*assign)[size_t(fi)] = c;
                changed = true;
            }
        }
        for (int c = 0; c < k; ++c) {
            int cnt = 0;
            double au = 0.0, av = 0.0;
            for (int i = 0; i < n; ++i) {
                if ((*assign)[size_t(i)] == c) {
                    cnt++;
                    au += pts[size_t(i)].u;
                    av += pts[size_t(i)].v;
                }
            }
            if (cnt > 0) {
                (*centroids)[size_t(c)].u = float(au / double(cnt));
                (*centroids)[size_t(c)].v = float(av / double(cnt));
            }
        }
        if (!changed) {
            break;
        }
    }
}

// Sutherland–Hodgman clip of poly to the half-plane a*u + b*v + c >= 0
// (keepPositive) or <= 0.
std::vector<Pt> ClipHalfPlane(std::vector<Pt> const &poly, double a, double b,
                              double c, bool keepPositive)
{
    std::vector<Pt> out;
    int const n = int(poly.size());
    if (n == 0) {
        return out;
    }
    auto side = [&](Pt const &p) {
        double const s = a * double(p.u) + b * double(p.v) + c;
        return keepPositive ? s >= 0.0 : s <= 0.0;
    };
    for (int i = 0; i < n; ++i) {
        Pt const &P = poly[size_t(i)];
        Pt const &Q = poly[size_t((i + 1) % n)];
        bool const inP = side(P), inQ = side(Q);
        if (inQ) {
            if (!inP) {
                double const sp =
                    a * double(P.u) + b * double(P.v) + c;
                double const sq =
                    a * double(Q.u) + b * double(Q.v) + c;
                double const f = (sp != sq) ? sp / (sp - sq) : 0.0;
                Pt x;
                x.u = float(double(P.u) + (double(Q.u) - double(P.u)) * f);
                x.v = float(double(P.v) + (double(Q.v) - double(P.v)) * f);
                out.push_back(x);
            }
            out.push_back(Q);
        } else if (inP) {
            double const sp = a * double(P.u) + b * double(P.v) + c;
            double const sq = a * double(Q.u) + b * double(Q.v) + c;
            double const f = (sp != sq) ? sp / (sp - sq) : 0.0;
            Pt x;
            x.u = float(double(P.u) + (double(Q.u) - double(P.u)) * f);
            x.v = float(double(P.v) + (double(Q.v) - double(P.v)) * f);
            out.push_back(x);
        }
    }
    return out;
}

// Clip poly to the Voronoi cell of centroids[c].
std::vector<Pt> ClipToCell(std::vector<Pt> const &poly,
                           std::vector<Pt> const &centroids, int c)
{
    std::vector<Pt> out = poly;
    for (int o = 0; o < int(centroids.size()); ++o) {
        if (o == c) {
            continue;
        }
        // |p - cc| <= |p - co| ⟺ 2(co-cc)·p <= |co|² - |cc|².
        double const ax = double(centroids[size_t(o)].u) -
                          double(centroids[size_t(c)].u);
        double const ay = double(centroids[size_t(o)].v) -
                          double(centroids[size_t(c)].v);
        double const rhs =
            (double(centroids[size_t(o)].u) * double(centroids[size_t(o)].u) +
             double(centroids[size_t(o)].v) * double(centroids[size_t(o)].v)) -
            (double(centroids[size_t(c)].u) * double(centroids[size_t(c)].u) +
             double(centroids[size_t(c)].v) * double(centroids[size_t(c)].v));
        // 2a·p - rhs <= 0  ⟺  (-2a)·p + rhs >= 0.
        out = ClipHalfPlane(out, -2.0 * ax, -2.0 * ay, rhs, true);
        if (out.empty()) {
            break;
        }
    }
    return out;
}

double PolyArea(std::vector<Pt> const &poly)
{
    double acc = 0.0;
    int const n = int(poly.size());
    for (int i = 0; i < n; ++i) {
        Pt const &P = poly[size_t(i)];
        Pt const &Q = poly[size_t((i + 1) % n)];
        acc += double(P.u) * double(Q.v) - double(Q.u) * double(P.v);
    }
    return 0.5 * acc;
}

// Subdivide edges longer than maxLen (inserted points stay on the polygon).
// Stops once poly reaches budget points.
void SubdivideLongEdges(std::vector<Pt> *poly, double maxLen, int budget)
{
    for (;;) {
        int n = int(poly->size());
        if (n >= budget) {
            return;
        }
        int bi = -1;
        double bl = maxLen;
        for (int i = 0; i < n; ++i) {
            Pt const &P = (*poly)[size_t(i)];
            Pt const &Q = (*poly)[size_t((i + 1) % n)];
            double const du = double(Q.u) - double(P.u);
            double const dv = double(Q.v) - double(P.v);
            double const len = std::sqrt(du * du + dv * dv);
            if (len > bl) {
                bl = len;
                bi = i;
            }
        }
        if (bi < 0) {
            return;
        }
        Pt const &P = (*poly)[size_t(bi)];
        Pt const &Q = (*poly)[size_t((bi + 1) % n)];
        Pt mid;
        mid.u = float((double(P.u) + double(Q.u)) * 0.5);
        mid.v = float((double(P.v) + double(Q.v)) * 0.5);
        poly->insert(poly->begin() + bi + 1, mid);
    }
}

// Arc-length resample of a closed polygon to count points.
std::vector<Pt> ResamplePoly(std::vector<Pt> const &poly, int count)
{
    std::vector<Pt> out;
    int const n = int(poly.size());
    if (n == 0 || count <= 0) {
        return out;
    }
    std::vector<double> cum(size_t(n + 1), 0.0);
    for (int i = 0; i < n; ++i) {
        Pt const &P = poly[size_t(i)];
        Pt const &Q = poly[size_t((i + 1) % n)];
        double const du = double(Q.u) - double(P.u);
        double const dv = double(Q.v) - double(P.v);
        cum[size_t(i + 1)] = cum[size_t(i)] + std::sqrt(du * du + dv * dv);
    }
    double const total = cum[size_t(n)];
    if (!(total > 0.0)) {
        out.assign(size_t(count), poly[0]);
        return out;
    }
    out.reserve(size_t(count));
    int seg = 0;
    for (int i = 0; i < count; ++i) {
        double const s = total * double(i) / double(count);
        while (seg + 1 < n && cum[size_t(seg + 1)] < s) {
            ++seg;
        }
        double const s0 = cum[size_t(seg)], s1 = cum[size_t(seg + 1)];
        double const f = (s1 > s0) ? (s - s0) / (s1 - s0) : 0.0;
        Pt const &P = poly[size_t(seg % n)];
        Pt const &Q = poly[size_t((seg + 1) % n)];
        Pt x;
        x.u = float(double(P.u) + (double(Q.u) - double(P.u)) * f);
        x.v = float(double(P.v) + (double(Q.v) - double(P.v)) * f);
        out.push_back(x);
    }
    return out;
}

// Piecewise-linear arc-length resample of a center curve to outN points.
void ResampleCenter(float const *cx, float const *cy, float const *cz, int n,
                    int outN, std::vector<float> *ox,
                    std::vector<float> *oy, std::vector<float> *oz)
{
    ox->resize(size_t(outN));
    oy->resize(size_t(outN));
    oz->resize(size_t(outN));
    std::vector<double> cum(size_t(n), 0.0);
    for (int i = 1; i < n; ++i) {
        double const dx = double(cx[i]) - double(cx[i - 1]);
        double const dy = double(cy[i]) - double(cy[i - 1]);
        double const dz = double(cz[i]) - double(cz[i - 1]);
        cum[size_t(i)] =
            cum[size_t(i - 1)] + std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    double const total = cum[size_t(n - 1)];
    int seg = 0;
    for (int i = 0; i < outN; ++i) {
        double const s =
            outN > 1 ? total * double(i) / double(outN - 1) : 0.0;
        while (seg + 1 < n - 1 && cum[size_t(seg + 1)] < s) {
            ++seg;
        }
        double const s0 = cum[size_t(seg)], s1 = cum[size_t(seg + 1)];
        double const f = (s1 > s0) ? (s - s0) / (s1 - s0) : 0.0;
        (*ox)[size_t(i)] = float(double(cx[seg]) +
                                 (double(cx[seg + 1]) - double(cx[seg])) * f);
        (*oy)[size_t(i)] = float(double(cy[seg]) +
                                 (double(cy[seg + 1]) - double(cy[seg])) * f);
        (*oz)[size_t(i)] = float(double(cz[seg]) +
                                 (double(cz[seg + 1]) - double(cz[seg])) * f);
    }
}

bool FloatVecEqual(std::vector<float> const &a, std::vector<float> const &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

bool DescEqual(TonicTubeDesc const &a, TonicTubeDesc const &b)
{
    if (a.ringVerts != b.ringVerts || a.tubeId != b.tubeId ||
        a.regionId != b.regionId || a.level != b.level ||
        a.parentTubeId != b.parentTubeId || a.childIndex != b.childIndex ||
        a.sections.size() != b.sections.size() ||
        !FloatVecEqual(a.centerX, b.centerX) ||
        !FloatVecEqual(a.centerY, b.centerY) ||
        !FloatVecEqual(a.centerZ, b.centerZ)) {
        return false;
    }
    for (size_t i = 0; i < a.sections.size(); ++i) {
        TonicTubeSection const &sa = a.sections[i];
        TonicTubeSection const &sb = b.sections[i];
        if (sa.t != sb.t || sa.scale != sb.scale || sa.twist != sb.twist ||
            !FloatVecEqual(sa.u, sb.u) || !FloatVecEqual(sa.v, sb.v)) {
            return false;
        }
    }
    return true;
}

}  // namespace

float TonicCenterArcLength(float const *cx, float const *cy, float const *cz,
                           int nCv)
{
    double acc = 0.0;
    for (int i = 1; i < nCv; ++i) {
        double const dx = double(cx[i]) - double(cx[i - 1]);
        double const dy = double(cy[i]) - double(cy[i - 1]);
        double const dz = double(cz[i]) - double(cz[i - 1]);
        acc += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return float(acc);
}

bool TonicValidateSubdivide(TonicSubdivideDesc const &params, std::string *err)
{
    if (params.count < 2 || params.count > 8) {
        return fail(err, "TonicValidateSubdivide: count in [2, 8]");
    }
    if (params.splitMode != TonicSplit_KMeans &&
        params.splitMode != TonicSplit_Edge) {
        return fail(err, "TonicValidateSubdivide: splitMode kmeans | edge");
    }
    if (params.splitMode == TonicSplit_Edge) {
        if (params.count != 2) {
            return fail(err,
                        "TonicValidateSubdivide: edge mode needs count == 2");
        }
        double const n = double(params.edgeA) * double(params.edgeA) +
                         double(params.edgeB) * double(params.edgeB);
        if (!(n > 0.0)) {
            return fail(err,
                        "TonicValidateSubdivide: degenerate edge line");
        }
    }
    return true;
}

bool TonicSubdivideTubeCpu(TonicTubeDesc const &parent,
                           std::vector<TonicFrame> const &parentFrames,
                           TonicSubdivideDesc const &params,
                           std::vector<TonicTubeDesc> *children,
                           std::string *err)
{
    if (!children) {
        return fail(err, "TonicSubdivideTubeCpu: null output");
    }
    if (!TonicValidateSubdivide(params, err) ||
        !ValidateTube(parent, "parent", err)) {
        return false;
    }
    int const nCv = int(parent.centerX.size());
    int const nSec = int(parent.sections.size());
    if (parentFrames.size() != size_t(nCv)) {
        return fail(err, "TonicSubdivideTubeCpu: frames match center CVs");
    }
    // Root ring in placed coords + sub-region partition.
    std::vector<Pt> rootRing;
    PlaceRing(parent.sections.front(), &rootRing);
    double rootMeanU = 0.0, rootMeanV = 0.0;
    RingMean(rootRing, &rootMeanU, &rootMeanV);
    float const rootRadius = MeanRadius(rootRing);
    if (!(rootRadius > 0.0f)) {
        return fail(err, "TonicSubdivideTubeCpu: degenerate root ring");
    }
    std::vector<int> assign(rootRing.size(), 0);
    auto centroids = std::vector<Pt>(size_t(params.count));
    if (params.splitMode == TonicSplit_KMeans) {
        uint64_t const key = (uint64_t(uint32_t(parent.tubeId)) << 32) ^
                             uint64_t(uint32_t(params.seed)) ^
                             0x484B3134ull;  // "HK14"
        KMeans(rootRing, params.count, key, &assign, &centroids);
    } else {
        for (size_t i = 0; i < rootRing.size(); ++i) {
            double const s = double(params.edgeA) * double(rootRing[i].u) +
                             double(params.edgeB) * double(rootRing[i].v) +
                             double(params.edgeC);
            assign[i] = (s >= 0.0) ? 1 : 0;
        }
        for (int c = 0; c < 2; ++c) {
            double su = 0.0, sv = 0.0;
            int cnt = 0;
            for (size_t i = 0; i < rootRing.size(); ++i) {
                if (assign[i] == c) {
                    su += rootRing[i].u;
                    sv += rootRing[i].v;
                    cnt++;
                }
            }
            if (cnt == 0) {
                return fail(err,
                            "TonicSubdivideTubeCpu: edge leaves a side "
                            "empty");
            }
            centroids[size_t(c)].u = float(su / double(cnt));
            centroids[size_t(c)].v = float(sv / double(cnt));
        }
    }
    // Mean parent arc spacing (chord subdivision budget).
    double spacing = 0.0;
    for (size_t i = 0; i < rootRing.size(); ++i) {
        Pt const &P = rootRing[i];
        Pt const &Q = rootRing[(i + 1) % rootRing.size()];
        double const du = double(Q.u) - double(P.u);
        double const dv = double(Q.v) - double(P.v);
        spacing += std::sqrt(du * du + dv * dv);
    }
    spacing /= double(rootRing.size());
    if (!(spacing > 0.0)) {
        return fail(err, "TonicSubdivideTubeCpu: degenerate root ring");
    }
    children->resize(size_t(params.count));
    for (int c = 0; c < params.count; ++c) {
        TonicTubeDesc &child = (*children)[size_t(c)];
        child.tubeId = parent.tubeId * 16 + 1 + c;
        child.regionId = parent.regionId;
        child.level = parent.level + 1;
        child.parentTubeId = parent.tubeId;
        child.childIndex = c;
        // Centers: parent center offset by the centroid, scaled by the
        // local-to-root radius ratio (§2.3 step 2).
        child.centerX.resize(size_t(nCv));
        child.centerY.resize(size_t(nCv));
        child.centerZ.resize(size_t(nCv));
        for (int i = 0; i < nCv; ++i) {
            float const ti =
                nCv > 1 ? float(i) / float(nCv - 1) : 0.0f;
            std::vector<Pt> ringAt;
            float radiusAt = 0.0f;
            InterpSectionAt(parent.sections, parent.ringVerts, ti, &ringAt,
                            &radiusAt);
            float const ratio =
                rootRadius > 0.0f ? radiusAt / rootRadius : 1.0f;
            TonicFrame const &fr = parentFrames[size_t(i)];
            float const ox = centroids[size_t(c)].u * ratio;
            float const oy = centroids[size_t(c)].v * ratio;
            child.centerX[size_t(i)] =
                parent.centerX[size_t(i)] + fr.nx * ox + fr.bx * oy;
            child.centerY[size_t(i)] =
                parent.centerY[size_t(i)] + fr.ny * ox + fr.by * oy;
            child.centerZ[size_t(i)] =
                parent.centerZ[size_t(i)] + fr.nz * ox + fr.bz * oy;
        }
        // Sections: carry the root partition to every parent ring by the
        // section's centroid/radius similarity, clip there, recenter on
        // the child's own center (rings are child-relative, matching the
        // center offsets above), subdivide long chords, pad all sections
        // to one uniform count. An unchanged chart reuses the root cell
        // bit-exactly.
        auto clipped = std::vector<std::vector<Pt>>(size_t(nSec));
        int target = 0;
        for (int s = 0; s < nSec; ++s) {
            std::vector<Pt> ring;
            PlaceRing(parent.sections[size_t(s)], &ring);
            double secMeanU = 0.0, secMeanV = 0.0;
            RingMean(ring, &secMeanU, &secMeanV);
            float const secRadius = MeanRadius(ring);
            bool const sameChart = (secMeanU == rootMeanU &&
                                    secMeanV == rootMeanV &&
                                    secRadius == rootRadius);
            if (params.splitMode == TonicSplit_KMeans) {
                if (sameChart || !(secRadius > 0.0f)) {
                    clipped[size_t(s)] = ClipToCell(ring, centroids, c);
                } else {
                    double const q = double(secRadius) / double(rootRadius);
                    std::vector<Pt> carried(size_t(params.count));
                    for (int o = 0; o < params.count; ++o) {
                        carried[size_t(o)].u = float(
                            secMeanU +
                            (double(centroids[size_t(o)].u) - rootMeanU) * q);
                        carried[size_t(o)].v = float(
                            secMeanV +
                            (double(centroids[size_t(o)].v) - rootMeanV) * q);
                    }
                    clipped[size_t(s)] = ClipToCell(ring, carried, c);
                }
            } else {
                double const a = double(params.edgeA);
                double const b = double(params.edgeB);
                double cut = double(params.edgeC);
                if (!sameChart && secRadius > 0.0f) {
                    double const q = double(secRadius) / double(rootRadius);
                    double const rootC =
                        a * rootMeanU + b * rootMeanV + cut;
                    cut = q * rootC - (a * secMeanU + b * secMeanV);
                }
                clipped[size_t(s)] = ClipHalfPlane(ring, a, b, cut, c == 1);
            }
            double const clippedArea =
                std::fabs(PolyArea(clipped[size_t(s)]));
            double const emptyTol =
                1e-12 * double(rootRadius) * double(rootRadius);
            if (clipped[size_t(s)].size() < 3 || clippedArea <= emptyTol) {
                std::string const text =
                    "TonicSubdivideTubeCpu: empty sub-region c=" +
                    std::to_string(c) + " s=" + std::to_string(s) +
                    " verts=" +
                    std::to_string(int(clipped[size_t(s)].size())) +
                    " area=" + std::to_string(clippedArea) +
                    " (raise the ring density or change the seed)";
                return fail(err, text.c_str());
            }
            SubdivideLongEdges(&clipped[size_t(s)], 2.0 * spacing,
                               kMaxRingVerts);
            // Recenter on the child center: same scaled centroid the
            // center CVs use (radius ratio at this section's t).
            {
                std::vector<Pt> ringAt;
                float radiusAt = 0.0f;
                InterpSectionAt(parent.sections, parent.ringVerts,
                                parent.sections[size_t(s)].t, &ringAt,
                                &radiusAt);
                float const ratio =
                    rootRadius > 0.0f ? radiusAt / rootRadius : 1.0f;
                float const ou = centroids[size_t(c)].u * ratio;
                float const ov = centroids[size_t(c)].v * ratio;
                for (auto &p : clipped[size_t(s)]) {
                    p.u -= ou;
                    p.v -= ov;
                }
            }
            target = std::max(target, int(clipped[size_t(s)].size()));
        }
        if (target > kMaxRingVerts) {
            target = kMaxRingVerts;
        }
        child.ringVerts = target;
        child.sections.resize(size_t(nSec));
        for (int s = 0; s < nSec; ++s) {
            std::vector<Pt> poly = clipped[size_t(s)];
            if (int(poly.size()) > target) {
                poly = ResamplePoly(poly, target);
            } else {
                // Exact-length pad: a 0 bound splits the longest edge
                // first, so every inserted point stays on the polygon.
                // (Fully degenerate runs fall back to the resample.)
                SubdivideLongEdges(&poly, 0.0, target);
                if (int(poly.size()) != target) {
                    poly = ResamplePoly(clipped[size_t(s)], target);
                }
            }
            TonicTubeSection &cs = child.sections[size_t(s)];
            cs.t = parent.sections[size_t(s)].t;
            cs.scale = 1.0f;
            cs.twist = 0.0f;
            cs.u.resize(size_t(target));
            cs.v.resize(size_t(target));
            for (int i = 0; i < target; ++i) {
                cs.u[size_t(i)] = poly[size_t(i)].u;
                cs.v[size_t(i)] = poly[size_t(i)].v;
            }
        }
    }
    return true;
}

bool TonicParentAverageCpu(std::vector<TonicTubeDesc> const &children,
                           TonicTubeDesc *parentOut, std::string *err)
{
    if (!parentOut) {
        return fail(err, "TonicParentAverageCpu: null output");
    }
    if (children.empty()) {
        return fail(err, "TonicParentAverageCpu: no children");
    }
    for (auto const &ch : children) {
        if (!ValidateTube(ch, "child", err)) {
            return false;
        }
    }
    int const nCv = int(children[0].centerX.size());
    int const nSec = int(children[0].sections.size());
    int const rv = children[0].ringVerts;
    // Centers: arc-length-resample every child to nCv, mean in double.
    std::vector<std::vector<float>> rx(children.size()), ry(children.size()),
        rz(children.size());
    for (size_t c = 0; c < children.size(); ++c) {
        ResampleCenter(children[c].centerX.data(), children[c].centerY.data(),
                       children[c].centerZ.data(),
                       int(children[c].centerX.size()), nCv, &rx[c], &ry[c],
                       &rz[c]);
    }
    parentOut->centerX.resize(size_t(nCv));
    parentOut->centerY.resize(size_t(nCv));
    parentOut->centerZ.resize(size_t(nCv));
    double const inv = 1.0 / double(children.size());
    for (int i = 0; i < nCv; ++i) {
        double ax = 0.0, ay = 0.0, az = 0.0;
        for (size_t c = 0; c < children.size(); ++c) {
            ax += rx[c][size_t(i)];
            ay += ry[c][size_t(i)];
            az += rz[c][size_t(i)];
        }
        parentOut->centerX[size_t(i)] = float(ax * inv);
        parentOut->centerY[size_t(i)] = float(ay * inv);
        parentOut->centerZ[size_t(i)] = float(az * inv);
    }
    // Parent frames for the section planes.
    std::vector<TonicFrame> pframes;
    {
        std::string derr;
        if (!TonicCenterFramesCpu(parentOut->centerX.data(),
                                  parentOut->centerY.data(),
                                  parentOut->centerZ.data(), nCv, &pframes,
                                  &derr)) {
            return fail(err, derr.c_str());
        }
    }
    parentOut->ringVerts = rv;
    parentOut->sections.resize(size_t(nSec));
    // Per-child frames for world placement.
    std::vector<std::vector<TonicFrame>> cframes(children.size());
    for (size_t c = 0; c < children.size(); ++c) {
        std::string derr;
        if (!TonicCenterFramesCpu(children[c].centerX.data(),
                                  children[c].centerY.data(),
                                  children[c].centerZ.data(),
                                  int(children[c].centerX.size()),
                                  &cframes[c], &derr)) {
            return fail(err, derr.c_str());
        }
    }
    for (int s = 0; s < nSec; ++s) {
        float const t = children[0].sections[size_t(s)].t;
        float pc[3];
        TonicFrame pf;
        {
            // Center + frame of the averaged parent at t.
            TonicTubeDesc tmp = *parentOut;
            tmp.sections = children[0].sections;  // t-domain carrier
            std::string derr;
            if (!TonicSampleCenterCpu(tmp, pframes, t, &pc[0], &pc[1], &pc[2],
                                      &pf, &derr)) {
                return fail(err, derr.c_str());
            }
        }
        // Union of the child rings at t, in the parent plane.
        std::vector<Pt> cloud;
        for (size_t c = 0; c < children.size(); ++c) {
            std::vector<Pt> ring;
            InterpSectionAt(children[c].sections, children[c].ringVerts, t,
                            &ring, nullptr);
            TonicTubeDesc ctmp = children[c];
            float cc[3];
            TonicFrame cf;
            std::string derr;
            if (!TonicSampleCenterCpu(ctmp, cframes[c], t, &cc[0], &cc[1],
                                      &cc[2], &cf, &derr)) {
                return fail(err, derr.c_str());
            }
            for (auto const &p : ring) {
                float const wx =
                    cc[0] + cf.nx * p.u + cf.bx * p.v;
                float const wy =
                    cc[1] + cf.ny * p.u + cf.by * p.v;
                float const wz =
                    cc[2] + cf.nz * p.u + cf.bz * p.v;
                float const dx = wx - pc[0], dy = wy - pc[1], dz = wz - pc[2];
                Pt q;
                q.u = dx * pf.nx + dy * pf.ny + dz * pf.nz;
                q.v = dx * pf.bx + dy * pf.by + dz * pf.bz;
                cloud.push_back(q);
            }
        }
        // Canonical refit: angle-sort around the centroid, drop exact
        // duplicates, then grow (on-polygon midpoint splits) or shrink
        // (even stride picks) to rv. Every output vertex is an input
        // vertex or an on-polygon midpoint, so a second pass over
        // identical copies reproduces the ring bit-exactly.
        double su = 0.0, sv = 0.0;
        for (auto const &p : cloud) {
            su += p.u;
            sv += p.v;
        }
        double const cu = su / double(cloud.size());
        double const cv = sv / double(cloud.size());
        std::sort(cloud.begin(), cloud.end(), [&](Pt const &a, Pt const &b) {
            double const aa =
                std::atan2(double(a.v) - cv, double(a.u) - cu);
            double const bb =
                std::atan2(double(b.v) - cv, double(b.u) - cu);
            if (aa != bb) {
                return aa < bb;
            }
            double const da = (double(a.u) - cu) * (double(a.u) - cu) +
                              (double(a.v) - cv) * (double(a.v) - cv);
            double const db = (double(b.u) - cu) * (double(b.u) - cu) +
                              (double(b.v) - cv) * (double(b.v) - cv);
            return da < db;
        });
        std::vector<Pt> ring;
        ring.reserve(cloud.size());
        for (auto const &p : cloud) {
            if (ring.empty() || p.u != ring.back().u ||
                p.v != ring.back().v) {
                ring.push_back(p);
            }
        }
        if (ring.size() > 1 && ring.front().u == ring.back().u &&
            ring.front().v == ring.back().v) {
            ring.pop_back();
        }
        if (ring.size() < 3) {
            return fail(err,
                        "TonicParentAverageCpu: degenerate union ring");
        }
        if (int(ring.size()) < rv) {
            SubdivideLongEdges(&ring, 0.0, rv);
        } else if (int(ring.size()) > rv) {
            std::vector<Pt> picked;
            picked.reserve(size_t(rv));
            int const n = int(ring.size());
            for (int i = 0; i < rv; ++i) {
                int const idx =
                    int((int64_t(i) * int64_t(n)) / int64_t(rv)) % n;
                picked.push_back(ring[size_t(idx)]);
            }
            ring.swap(picked);
        }
        TonicTubeSection &ps = parentOut->sections[size_t(s)];
        ps.t = t;
        ps.scale = 1.0f;
        ps.twist = 0.0f;
        ps.u.resize(size_t(rv));
        ps.v.resize(size_t(rv));
        for (int i = 0; i < rv; ++i) {
            ps.u[size_t(i)] = ring[size_t(i)].u;
            ps.v[size_t(i)] = ring[size_t(i)].v;
        }
    }
    // Output identity: copies of one tube keep its identity (this is what
    // makes averaging idempotent); siblings sharing a parent yield that
    // parent; anything else is a transient on-the-fly aggregate the caller
    // assigns (merge-selected passes its own sibling identity instead --
    // see TonicMergeTubesCpu).
    bool const sameTube = [&]() {
        for (auto const &ch : children) {
            if (ch.tubeId != children[0].tubeId) {
                return false;
            }
        }
        return true;
    }();
    bool sameParent = children[0].parentTubeId >= 0;
    for (auto const &ch : children) {
        if (ch.parentTubeId != children[0].parentTubeId) {
            sameParent = false;
        }
    }
    if (sameTube) {
        parentOut->tubeId = children[0].tubeId;
        parentOut->regionId = children[0].regionId;
        parentOut->level = children[0].level;
        parentOut->parentTubeId = children[0].parentTubeId;
        parentOut->childIndex = children[0].childIndex;
    } else if (sameParent) {
        parentOut->tubeId = children[0].parentTubeId;
        parentOut->regionId = children[0].regionId;
        parentOut->level =
            children[0].level > 1 ? children[0].level - 1 : 1;
        parentOut->parentTubeId = -1;
        parentOut->childIndex = -1;
    } else {
        int minLevel = children[0].level;
        for (auto const &ch : children) {
            minLevel = std::min(minLevel, ch.level);
        }
        parentOut->tubeId = -1;  // transient: caller assigns identity
        parentOut->regionId = children[0].regionId;
        parentOut->level = minLevel > 1 ? minLevel - 1 : 1;
        parentOut->parentTubeId = -1;
        parentOut->childIndex = -1;
    }
    return true;
}

bool TonicMergeTubesCpu(std::vector<TonicTubeDesc> const &children,
                        TonicSubdivideDesc const &params,
                        TonicTubeDesc const *roundTripParent,
                        TonicTubeDesc *parentOut, std::string *err)
{
    if (!parentOut) {
        return fail(err, "TonicMergeTubesCpu: null output");
    }
    if (children.empty()) {
        return fail(err, "TonicMergeTubesCpu: no children");
    }
    if (!TonicValidateSubdivide(params, err)) {
        return false;
    }
    if (roundTripParent) {
        // Untouched fast path: re-derive and bit-compare.
        if (!ValidateTube(*roundTripParent, "hint", err)) {
            return false;
        }
        int const nCv = int(roundTripParent->centerX.size());
        std::vector<TonicFrame> frames;
        std::string derr;
        if (!TonicCenterFramesCpu(roundTripParent->centerX.data(),
                                  roundTripParent->centerY.data(),
                                  roundTripParent->centerZ.data(), nCv,
                                  &frames, &derr)) {
            return fail(err, derr.c_str());
        }
        std::vector<TonicTubeDesc> expect;
        if (!TonicSubdivideTubeCpu(*roundTripParent, frames, params, &expect,
                                   &derr)) {
            return fail(err, derr.c_str());
        }
        bool untouched = (expect.size() == children.size());
        for (size_t i = 0; untouched && i < expect.size(); ++i) {
            untouched = DescEqual(expect[i], children[i]);
        }
        if (untouched) {
            *parentOut = *roundTripParent;
            return true;
        }
        // Else fall through to the K7 aggregate.
    }
    return TonicParentAverageCpu(children, parentOut, err);
}

bool TonicDeriveChildCpu(TonicTubeDesc const &parent,
                         std::vector<TonicFrame> const &parentFrames,
                         TonicSubdivideDesc const &params, int childIndex,
                         TonicTubeDesc *derived, std::string *err)
{
    if (!derived) {
        return fail(err, "TonicDeriveChildCpu: null output");
    }
    std::vector<TonicTubeDesc> all;
    if (!TonicSubdivideTubeCpu(parent, parentFrames, params, &all, err)) {
        return false;
    }
    if (childIndex < 0 || childIndex >= int(all.size())) {
        return fail(err, "TonicDeriveChildCpu: child index out of range");
    }
    *derived = all[size_t(childIndex)];
    return true;
}

bool TonicComputeDeltasCpu(TonicTubeDesc const &actual,
                           TonicTubeDesc const &derived,
                           std::vector<TonicFrame> const &derivedFrames,
                           TonicShapeDeltas *deltas, std::string *err)
{
    if (!deltas) {
        return fail(err, "TonicComputeDeltasCpu: null output");
    }
    int const nCv = int(derived.centerX.size());
    if (int(actual.centerX.size()) != nCv ||
        derivedFrames.size() != size_t(nCv) ||
        actual.sections.size() != derived.sections.size()) {
        return fail(err, "TonicComputeDeltasCpu: layout mismatch");
    }
    deltas->centerDu.assign(size_t(nCv), 0.0f);
    deltas->centerDv.assign(size_t(nCv), 0.0f);
    deltas->centerDw.assign(size_t(nCv), 0.0f);
    for (int i = 0; i < nCv; ++i) {
        float const wx = actual.centerX[size_t(i)] - derived.centerX[size_t(i)];
        float const wy = actual.centerY[size_t(i)] - derived.centerY[size_t(i)];
        float const wz = actual.centerZ[size_t(i)] - derived.centerZ[size_t(i)];
        TonicFrame const &fr = derivedFrames[size_t(i)];
        deltas->centerDu[size_t(i)] = wx * fr.nx + wy * fr.ny + wz * fr.nz;
        deltas->centerDv[size_t(i)] = wx * fr.bx + wy * fr.by + wz * fr.bz;
        deltas->centerDw[size_t(i)] = wx * fr.tx + wy * fr.ty + wz * fr.tz;
    }
    deltas->sections.resize(derived.sections.size());
    for (size_t s = 0; s < derived.sections.size(); ++s) {
        TonicTubeSection const &sa = actual.sections[s];
        TonicTubeSection const &sd = derived.sections[s];
        if (sa.t != sd.t || sa.u.size() != sd.u.size()) {
            return fail(err, "TonicComputeDeltasCpu: section mismatch");
        }
        TonicTubeSection &dd = deltas->sections[s];
        dd.t = sd.t;
        dd.scale = 0.0f;
        dd.twist = 0.0f;
        dd.u.resize(sd.u.size());
        dd.v.resize(sd.u.size());
        for (size_t i = 0; i < sd.u.size(); ++i) {
            dd.u[i] = sa.u[i] - sd.u[i];
            dd.v[i] = sa.v[i] - sd.v[i];
        }
    }
    return true;
}

bool TonicRescaleCenterLength(float const *cx, float const *cy,
                              float const *cz, int nCv, float targetLen,
                              std::vector<float> *ox, std::vector<float> *oy,
                              std::vector<float> *oz, std::string *err)
{
    if (!ox || !oy || !oz) {
        return fail(err, "TonicRescaleCenterLength: null output");
    }
    if (!cx || !cy || !cz || nCv < 2 || !(targetLen > 0.0f)) {
        return fail(err, "TonicRescaleCenterLength: bad curve or length");
    }
    std::vector<double> cum(size_t(nCv), 0.0);
    for (int i = 1; i < nCv; ++i) {
        double const dx = double(cx[i]) - double(cx[i - 1]);
        double const dy = double(cy[i]) - double(cy[i - 1]);
        double const dz = double(cz[i]) - double(cz[i - 1]);
        cum[size_t(i)] =
            cum[size_t(i - 1)] + std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    double const total = cum[size_t(nCv - 1)];
    if (!(total > 0.0)) {
        return fail(err, "TonicRescaleCenterLength: degenerate curve");
    }
    if (float(total) == targetLen) {
        // Already there: copy bit-exactly (zero moves stay fixed points).
        ox->assign(cx, cx + nCv);
        oy->assign(cy, cy + nCv);
        oz->assign(cz, cz + nCv);
        return true;
    }
    double const k = double(targetLen) / total;
    ox->resize(size_t(nCv));
    oy->resize(size_t(nCv));
    oz->resize(size_t(nCv));
    // Uniform scaling about the root: every segment scales by k, so the
    // total hits targetLen exactly (up to float rounding) with the shape
    // and CV distribution preserved.
    for (int i = 0; i < nCv; ++i) {
        (*ox)[size_t(i)] =
            float(double(cx[0]) + (double(cx[i]) - double(cx[0])) * k);
        (*oy)[size_t(i)] =
            float(double(cy[0]) + (double(cy[i]) - double(cy[0])) * k);
        (*oz)[size_t(i)] =
            float(double(cz[0]) + (double(cz[i]) - double(cz[0])) * k);
    }
    return true;
}

bool TonicResampleDescRingsCpu(TonicTubeDesc const &tube, int ringVerts,
                               TonicTubeDesc *out, std::string *err)
{
    if (!out) {
        return fail(err, "TonicResampleDescRingsCpu: null output");
    }
    if (!ValidateTube(tube, "tube", err)) {
        return false;
    }
    if (ringVerts < 3 || ringVerts > 256) {
        return fail(err, "TonicResampleDescRingsCpu: ringVerts in [3, 256]");
    }
    *out = tube;
    out->ringVerts = ringVerts;
    for (auto &sec : out->sections) {
        std::vector<Pt> poly(sec.u.size());
        for (size_t i = 0; i < sec.u.size(); ++i) {
            poly[i].u = sec.u[i];
            poly[i].v = sec.v[i];
        }
        std::vector<Pt> rs = ResamplePoly(poly, ringVerts);
        sec.u.resize(size_t(ringVerts));
        sec.v.resize(size_t(ringVerts));
        for (int i = 0; i < ringVerts; ++i) {
            sec.u[size_t(i)] = rs[size_t(i)].u;
            sec.v[size_t(i)] = rs[size_t(i)].v;
        }
    }
    return true;
}

bool TonicHierarchicalSculptCpu(
    TonicTubeDesc const &parentNew,
    std::vector<TonicFrame> const &parentNewFrames,
    TonicSubdivideDesc const &params, int childIndex,
    TonicTubeDesc const &oldActual, TonicTubeDesc const &oldDerived,
    TonicShapeDeltas const &oldStored, bool lockChildren,
    bool preserveLength, TonicTubeDesc *outActual,
    TonicShapeDeltas *outStored, std::string *err)
{
    if (!outActual || !outStored) {
        return fail(err, "TonicHierarchicalSculptCpu: null output");
    }
    TonicTubeDesc derivedNew;
    if (!TonicDeriveChildCpu(parentNew, parentNewFrames, params,
        childIndex, &derivedNew, err)) {
        return false;
    }
    return TonicHierarchicalSculptApplyCpu(derivedNew, oldActual,
        oldDerived, oldStored, lockChildren, preserveLength, outActual,
        outStored, err);
}

bool TonicHierarchicalSculptApplyCpu(
    TonicTubeDesc const &derivedNew, TonicTubeDesc const &oldActual,
    TonicTubeDesc const &oldDerived, TonicShapeDeltas const &oldStored,
    bool lockChildren, bool preserveLength, TonicTubeDesc *outActual,
    TonicShapeDeltas *outStored, std::string *err)
{
    if (!outActual || !outStored) {
        return fail(err, "TonicHierarchicalSculptCpu: null output");
    }
    // Layout contract: center counts, section counts and section t values
    // must match (topology ops re-subdivide instead). RING counts may
    // change freely -- a moved parent re-partitions -- and deltas below
    // resample across the change by arc length.
    if (derivedNew.centerX.size() != oldActual.centerX.size() ||
        derivedNew.sections.size() != oldActual.sections.size()) {
        return fail(err,
                    "TonicHierarchicalSculptCpu: center/section counts "
                    "changed under the sculpt (re-subdivide)");
    }
    for (size_t s = 0; s < derivedNew.sections.size(); ++s) {
        if (derivedNew.sections[s].t != oldActual.sections[s].t) {
            return fail(err,
                        "TonicHierarchicalSculptCpu: section t drifted "
                        "(re-subdivide)");
        }
    }
    auto resampleDeltaRing = [&](std::vector<float> const &su,
                                 std::vector<float> const &sv,
                                 int newCount, std::vector<float> *ou,
                                 std::vector<float> *ov) {
        std::vector<Pt> poly(su.size());
        for (size_t i = 0; i < su.size(); ++i) {
            poly[i].u = su[i];
            poly[i].v = sv[i];
        }
        std::vector<Pt> rs = ResamplePoly(poly, newCount);
        ou->resize(size_t(newCount));
        ov->resize(size_t(newCount));
        for (int i = 0; i < newCount; ++i) {
            (*ou)[size_t(i)] = rs[size_t(i)].u;
            (*ov)[size_t(i)] = rs[size_t(i)].v;
        }
    };
    std::vector<TonicFrame> newFrames;
    {
        std::string derr;
        if (!TonicCenterFramesCpu(derivedNew.centerX.data(),
                                  derivedNew.centerY.data(),
                                  derivedNew.centerZ.data(),
                                  int(derivedNew.centerX.size()), &newFrames,
                                  &derr)) {
            return fail(err, derr.c_str());
        }
    }
    int const nCv = int(derivedNew.centerX.size());
    TonicTubeDesc actual = derivedNew;
    if (lockChildren) {
        // Rigid ride: world deltas frozen, stored deltas copied exactly.
        if (oldActual.centerX.size() != oldDerived.centerX.size() ||
            oldActual.sections.size() != oldDerived.sections.size()) {
            return fail(err,
                        "TonicHierarchicalSculptCpu: stale child layout");
        }
        for (int i = 0; i < nCv; ++i) {
            actual.centerX[size_t(i)] +=
                oldActual.centerX[size_t(i)] - oldDerived.centerX[size_t(i)];
            actual.centerY[size_t(i)] +=
                oldActual.centerY[size_t(i)] - oldDerived.centerY[size_t(i)];
            actual.centerZ[size_t(i)] +=
                oldActual.centerZ[size_t(i)] - oldDerived.centerZ[size_t(i)];
        }
        for (size_t s = 0; s < derivedNew.sections.size(); ++s) {
            if (oldActual.sections[s].u.size() !=
                oldDerived.sections[s].u.size()) {
                return fail(err,
                            "TonicHierarchicalSculptCpu: stale child "
                            "section layout");
            }
            std::vector<float> wu(oldActual.sections[s].u.size()),
                wv(oldActual.sections[s].u.size());
            for (size_t i = 0; i < wu.size(); ++i) {
                wu[i] =
                    oldActual.sections[s].u[i] - oldDerived.sections[s].u[i];
                wv[i] =
                    oldActual.sections[s].v[i] - oldDerived.sections[s].v[i];
            }
            std::vector<float> ru, rv;
            resampleDeltaRing(wu, wv,
                              int(derivedNew.sections[s].u.size()), &ru, &rv);
            for (size_t i = 0; i < ru.size(); ++i) {
                actual.sections[s].u[i] += ru[i];
                actual.sections[s].v[i] += rv[i];
            }
        }
        *outActual = actual;
        *outStored = oldStored;
        return true;
    }
    // Re-apply stored deltas in the new derived frames.
    if (oldStored.centerDu.size() != size_t(nCv) ||
        oldStored.sections.size() != derivedNew.sections.size()) {
        return fail(err, "TonicHierarchicalSculptCpu: stored layout drift");
    }
    for (int i = 0; i < nCv; ++i) {
        TonicFrame const &fr = newFrames[size_t(i)];
        float const du = oldStored.centerDu[size_t(i)];
        float const dv = oldStored.centerDv[size_t(i)];
        float const dw = oldStored.centerDw[size_t(i)];
        actual.centerX[size_t(i)] += fr.nx * du + fr.bx * dv + fr.tx * dw;
        actual.centerY[size_t(i)] += fr.ny * du + fr.by * dv + fr.ty * dw;
        actual.centerZ[size_t(i)] += fr.nz * du + fr.bz * dv + fr.tz * dw;
    }
    for (size_t s = 0; s < derivedNew.sections.size(); ++s) {
        std::vector<float> ru, rv;
        resampleDeltaRing(oldStored.sections[s].u, oldStored.sections[s].v,
                          int(derivedNew.sections[s].u.size()), &ru, &rv);
        for (size_t i = 0; i < ru.size(); ++i) {
            actual.sections[s].u[i] += ru[i];
            actual.sections[s].v[i] += rv[i];
        }
    }
    if (preserveLength) {
        float const oldLen =
            TonicCenterArcLength(oldActual.centerX.data(),
                                 oldActual.centerY.data(),
                                 oldActual.centerZ.data(), nCv);
        std::vector<float> ox, oy, oz;
        std::string derr;
        if (!TonicRescaleCenterLength(actual.centerX.data(),
                                      actual.centerY.data(),
                                      actual.centerZ.data(), nCv, oldLen, &ox,
                                      &oy, &oz, &derr)) {
            return fail(err, derr.c_str());
        }
        actual.centerX.swap(ox);
        actual.centerY.swap(oy);
        actual.centerZ.swap(oz);
    }
    // Zero sculpt stays zero: derivation drift (length rescale) is not
    // authoring, so unedited children keep exact-zero deltas (this is what
    // lets untouched-detection compare == 0.0; commit persists the length
    // basis separately -- see the header).
    bool sculpted = false;
    for (float v : oldStored.centerDu) {
        sculpted = sculpted || (v != 0.0f);
    }
    for (float v : oldStored.centerDv) {
        sculpted = sculpted || (v != 0.0f);
    }
    for (float v : oldStored.centerDw) {
        sculpted = sculpted || (v != 0.0f);
    }
    for (auto const &s : oldStored.sections) {
        for (float v : s.u) {
            sculpted = sculpted || (v != 0.0f);
        }
        for (float v : s.v) {
            sculpted = sculpted || (v != 0.0f);
        }
    }
    if (!sculpted) {
        TonicClearDeltas(derivedNew, outStored);
        *outActual = actual;
        return true;
    }
    if (!TonicComputeDeltasCpu(actual, derivedNew, newFrames, outStored,
                               err)) {
        return false;
    }
    *outActual = actual;
    return true;
}

int TonicOwningChildCell(float const parentRootCenter[3],
                         TonicFrame const &parentRootFrame,
                         float const *childRootCenters, int childCount,
                         float const p[3])
{
    if (!childRootCenters || childCount <= 0) {
        return -1;
    }
    auto plane = [&](float const *q, float *u, float *v) {
        float const dx = q[0] - parentRootCenter[0];
        float const dy = q[1] - parentRootCenter[1];
        float const dz = q[2] - parentRootCenter[2];
        *u = dx * parentRootFrame.nx + dy * parentRootFrame.ny +
             dz * parentRootFrame.nz;
        *v = dx * parentRootFrame.bx + dy * parentRootFrame.by +
             dz * parentRootFrame.bz;
    };
    float pu = 0.0f, pv = 0.0f;
    plane(p, &pu, &pv);
    int best = 0;
    float bestD2 = std::numeric_limits<float>::infinity();
    for (int c = 0; c < childCount; ++c) {
        float cu = 0.0f, cv = 0.0f;
        plane(childRootCenters + size_t(c) * 3, &cu, &cv);
        float const du = pu - cu, dv = pv - cv;
        float const d2 = du * du + dv * dv;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = c;
        }
    }
    return best;
}

}  // namespace usdGenTonic
