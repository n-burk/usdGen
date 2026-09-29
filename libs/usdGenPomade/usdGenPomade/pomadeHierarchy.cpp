// usdGenPomade — P4 hierarchy core implementation (K14/K7 CPU twins).
#include "usdGenPomade/pomadeHierarchy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace usdGenPomade {
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

bool ValidateTube(PomadeTubeDesc const &tube, char const *who, std::string *err)
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
    // K14 provenance only: an original parent polygon corner.  Clip-created
    // intersections and subdivided samples deliberately stay -1.
    int parentSlot = -1;
};

// Placed ring coords: the K5 spelling (scale, then twist rotation).
void PlaceRing(PomadeTubeSection const &s, std::vector<Pt> *out)
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
void InterpSectionAt(std::vector<PomadeTubeSection> const &secs, int rv,
                     float t, std::vector<Pt> *placed, float *radius)
{
    int const nSec = int(secs.size());
    int k = 0;
    while (k + 1 < nSec - 1 && secs[size_t(k + 1)].t < t) {
        ++k;
    }
    PomadeTubeSection const &s0 = secs[size_t(k)];
    PomadeTubeSection const &s1 = secs[size_t(k + 1)];
    PomadeTubeSection const &sP = secs[size_t(k > 0 ? k - 1 : k)];
    PomadeTubeSection const &sN = secs[size_t(k + 2 < nSec ? k + 2 : k + 1)];
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

// Deterministic k-means over pts (k-means++ init from the Pomade hash
// stream keyed by key, Lloyd's to convergence). assign[i] in [0, k).
void KMeans(std::vector<Pt> const &pts, int k, uint64_t key,
            std::vector<int> *assign, std::vector<Pt> *centroids)
{
    int const n = int(pts.size());
    assign->assign(size_t(n), 0);
    centroids->resize(size_t(k));
    // k-means++: first center = hash draw, rest by squared distance.
    uint64_t stream = key;
    auto draw = [&]() { return PomadeHash01(stream++, kPomadeSaltRoot); };
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

// Drop consecutive (cyclic) vertices closer than tol.  Sutherland-Hodgman
// emits a corner AND its intersection when a bisector passes through that
// corner, and two bisectors meeting at a Voronoi vertex emit that vertex
// twice; the resulting zero-length edge survives the pad and the K5
// per-slot interpolation, and float noise then decides whether the
// material triangulator sees a degenerate ring at a fill station.  A
// merged pair keeps the original parent corner (K14 provenance).
void DropCoincidentVertices(std::vector<Pt> *poly, double tol)
{
    if (poly->size() < 4 || !(tol > 0.0)) {
        return;
    }
    double const tol2 = tol * tol;
    auto close = [tol2](Pt const &a, Pt const &b) {
        double const du = double(a.u) - double(b.u);
        double const dv = double(a.v) - double(b.v);
        return du * du + dv * dv < tol2;
    };
    auto absorb = [](Pt *kept, Pt const &dropped) {
        if (kept->parentSlot < 0 && dropped.parentSlot >= 0) {
            *kept = dropped;
        }
    };
    std::vector<Pt> out;
    out.reserve(poly->size());
    for (Pt const &p : *poly) {
        if (!out.empty() && close(out.back(), p)) {
            absorb(&out.back(), p);
            continue;
        }
        out.push_back(p);
    }
    while (out.size() > 3 && close(out.back(), out.front())) {
        absorb(&out.front(), out.back());
        out.pop_back();
    }
    if (out.size() >= 3) {
        poly->swap(out);
    }
}

// Relative edge-length margin K14 treats as a tie when it densifies or pads
// a child cell (see SubdivideLongEdges).
constexpr double kSplitTieRel = 1e-4;

// Subdivide edges longer than maxLen (inserted points stay on the polygon).
// Stops once poly reaches budget points.  tieRel > 0 treats every edge
// within that relative margin of the longest as tied and splits the lowest
// index among them: a regular parent ring has edges equal up to float
// noise, and letting that noise pick the edge re-lays a K14 child's slots
// whenever an unrelated edit perturbs its parent by an ulp (K6 then adds
// the stored per-slot residuals to the wrong slots).
void SubdivideLongEdges(std::vector<Pt> *poly, double maxLen, int budget,
                        double tieRel = 0.0)
{
    std::vector<double> lens;
    for (;;) {
        int n = int(poly->size());
        if (n >= budget) {
            return;
        }
        int bi = -1;
        double bl = maxLen;
        lens.resize(size_t(n));
        for (int i = 0; i < n; ++i) {
            Pt const &P = (*poly)[size_t(i)];
            Pt const &Q = (*poly)[size_t((i + 1) % n)];
            double const du = double(Q.u) - double(P.u);
            double const dv = double(Q.v) - double(P.v);
            double const len = std::sqrt(du * du + dv * dv);
            lens[size_t(i)] = len;
            if (len > bl) {
                bl = len;
                bi = i;
            }
        }
        if (bi < 0) {
            return;
        }
        if (tieRel > 0.0) {
            double const tied = std::max(maxLen, bl * (1.0 - tieRel));
            for (int i = 0; i < bi; ++i) {
                if (lens[size_t(i)] > tied) {
                    bi = i;
                    break;
                }
            }
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

// K14 slot alignment between adjacent child sections.  Each section's
// Voronoi-clipped cell starts at whichever parent corner first falls in
// the clip, so once the parent rings differ per station (a K7 merge
// averages its children) slot 0 lands on a different corner at each
// station.  K5 interpolates per slot, so a rotated neighbour twists the
// ring into a figure-eight that the concave material triangulator
// (rightly) refuses.  Give `poly` the reference's winding, then rotate it
// cyclically to the shift with the least summed squared slot distance.
// Both rings are compared about their own vertex means, normalised by
// their mean radius, so a tapering or drifting section still matches by
// shape.  Ties keep the smallest shift: deterministic, so hydrate's
// re-derivation reproduces the stored child bit-exactly.
void AlignRingToReference(std::vector<Pt> *poly, std::vector<Pt> const &ref)
{
    int const n = int(poly->size());
    if (n < 3 || int(ref.size()) != n) {
        return;
    }
    double const area = PolyArea(*poly);
    double const refArea = PolyArea(ref);
    if ((area > 0.0 && refArea < 0.0) || (area < 0.0 && refArea > 0.0)) {
        std::reverse(poly->begin(), poly->end());
    }
    auto normalised = [n](std::vector<Pt> const &ring,
                          std::vector<double> *u, std::vector<double> *v) {
        double mu = 0.0, mv = 0.0;
        for (Pt const &p : ring) {
            mu += double(p.u);
            mv += double(p.v);
        }
        mu /= double(n);
        mv /= double(n);
        double radius = 0.0;
        for (Pt const &p : ring) {
            double const du = double(p.u) - mu;
            double const dv = double(p.v) - mv;
            radius += std::sqrt(du * du + dv * dv);
        }
        radius /= double(n);
        double const inv = radius > 0.0 ? 1.0 / radius : 1.0;
        u->resize(size_t(n));
        v->resize(size_t(n));
        for (int i = 0; i < n; ++i) {
            (*u)[size_t(i)] = (double(ring[size_t(i)].u) - mu) * inv;
            (*v)[size_t(i)] = (double(ring[size_t(i)].v) - mv) * inv;
        }
    };
    std::vector<double> pu, pv, ru, rv;
    normalised(*poly, &pu, &pv);
    normalised(ref, &ru, &rv);
    std::vector<double> costs(size_t(n), 0.0);
    double bestCost = std::numeric_limits<double>::infinity();
    for (int k = 0; k < n; ++k) {
        double cost = 0.0;
        for (int i = 0; i < n; ++i) {
            size_t const j = size_t((i + k) % n);
            double const du = pu[j] - ru[size_t(i)];
            double const dv = pv[j] - rv[size_t(i)];
            cost += du * du + dv * dv;
        }
        costs[size_t(k)] = cost;
        bestCost = std::min(bestCost, cost);
    }
    // Shifts within a hair of the best (1% of the mean radius per slot,
    // squared) are ties: a symmetric cell must keep the same answer when
    // an unrelated edit perturbs it by an ulp, or K6 re-derivation would
    // hand its stored residuals to rotated slots.
    double const tied = bestCost + 1e-4 * double(n);
    int best = 0;
    while (costs[size_t(best)] > tied) {
        ++best;
    }
    if (best != 0) {
        std::rotate(poly->begin(), poly->begin() + best, poly->end());
    }
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

bool RootFrameEqual(PomadeTubeDesc const &a, PomadeTubeDesc const &b)
{
    if (a.frameReference != b.frameReference) {
        return false;
    }
    if (a.rootFramePinned != b.rootFramePinned) {
        return false;
    }
    if (!a.rootFramePinned) {
        return true;
    }
    PomadeFrame const &x = a.rootFrame;
    PomadeFrame const &y = b.rootFrame;
    return x.tx == y.tx && x.ty == y.ty && x.tz == y.tz &&
           x.nx == y.nx && x.ny == y.ny && x.nz == y.nz &&
           x.bx == y.bx && x.by == y.by && x.bz == y.bz;
}

bool BoundaryBindingsEqual(std::vector<PomadeParentBoundaryBinding> const &a,
                           std::vector<PomadeParentBoundaryBinding> const &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].section != b[i].section ||
            a[i].parentSlot != b[i].parentSlot ||
            a[i].childSlot != b[i].childSlot) {
            return false;
        }
    }
    return true;
}

bool DescEqual(PomadeTubeDesc const &a, PomadeTubeDesc const &b)
{
    if (a.ringVerts != b.ringVerts || a.tubeId != b.tubeId ||
        !RootFrameEqual(a, b) ||
        a.regionId != b.regionId || a.level != b.level ||
        a.parentTubeId != b.parentTubeId || a.childIndex != b.childIndex ||
        !BoundaryBindingsEqual(a.inheritedBoundaryBindings,
                               b.inheritedBoundaryBindings) ||
        a.sections.size() != b.sections.size() ||
        !FloatVecEqual(a.centerX, b.centerX) ||
        !FloatVecEqual(a.centerY, b.centerY) ||
        !FloatVecEqual(a.centerZ, b.centerZ)) {
        return false;
    }
    for (size_t i = 0; i < a.sections.size(); ++i) {
        PomadeTubeSection const &sa = a.sections[i];
        PomadeTubeSection const &sb = b.sections[i];
        if (sa.t != sb.t || sa.scale != sb.scale || sa.twist != sb.twist ||
            !FloatVecEqual(sa.u, sb.u) || !FloatVecEqual(sa.v, sb.v)) {
            return false;
        }
    }
    return true;
}

// K7 only conforms inherited outer material corners. K14 records the child
// sample that physically retained each original parent corner; intersections
// and inserted cut detail deliberately never participate here.
struct BoundTarget {
    float p[3] = {};
    bool hasOwner = false;
    int childOffset = -1;
    int childIndex = std::numeric_limits<int>::max();
    PomadeParentBoundaryBinding binding;
};

bool SectionSlotWorld(PomadeTubeDesc const &tube,
                      std::vector<PomadeFrame> const &frames,
                      int sectionIndex, int slot, float out[3],
                      std::string *err)
{
    if (sectionIndex < 0 || sectionIndex >= int(tube.sections.size()) ||
        slot < 0 || slot >= int(tube.sections[size_t(sectionIndex)].u.size()) ||
        slot >= int(tube.sections[size_t(sectionIndex)].v.size())) return false;
    PomadeTubeSection const &section = tube.sections[size_t(sectionIndex)];
    float center[3] = {}; PomadeFrame frame; std::string derr;
    if (!PomadeSampleCenterCpu(tube, frames, section.t, &center[0], &center[1],
                              &center[2], &frame, &derr)) return fail(err, derr.c_str());
    float const scale = section.scale > 1e-6f ? section.scale : 1e-6f;
    float const ct = std::cos(section.twist), st = std::sin(section.twist);
    float const ru = section.u[size_t(slot)] * scale;
    float const rv = section.v[size_t(slot)] * scale;
    float const u = ru * ct - rv * st, v = ru * st + rv * ct;
    out[0] = center[0] + frame.nx * u + frame.bx * v;
    out[1] = center[1] + frame.ny * u + frame.by * v;
    out[2] = center[2] + frame.nz * u + frame.bz * v;
    return true;
}

bool InterpBoundTarget(std::vector<PomadeTubeSection> const &sections,
                       std::vector<BoundTarget> const &targets, int slot,
                       int ringVerts, float t, float out[3])
{
    int const n = int(sections.size());
    int k = 0;
    while (k + 1 < n - 1 && sections[size_t(k + 1)].t < t) ++k;
    int const next = k + 1, prev = k > 0 ? k - 1 : k;
    int const following = k + 2 < n ? k + 2 : next;
    float const dt = sections[size_t(next)].t > sections[size_t(k)].t
        ? sections[size_t(next)].t - sections[size_t(k)].t : 1.0f;
    float f = std::max(0.0f, std::min(1.0f, (t - sections[size_t(k)].t) / dt));
    for (int axis = 0; axis < 3; ++axis) {
        float const a = targets[size_t(k * ringVerts + slot)].p[axis];
        float const b = targets[size_t(next * ringVerts + slot)].p[axis];
        float const p = targets[size_t(prev * ringVerts + slot)].p[axis];
        float const q = targets[size_t(following * ringVerts + slot)].p[axis];
        out[axis] = PomadeHermite(a, b, k == 0 ? b - a : 0.5f * (b - p),
                                 next == n - 1 ? b - a : 0.5f * (q - a), f);
    }
    return true;
}

bool PomadeParentHoldingEdgesCpu(std::vector<PomadeTubeDesc> const &children,
                                std::vector<PomadeTubeDesc> const *priorChildren,
                                PomadeTubeDesc const &hint,
                                PomadeTubeDesc *parentOut, std::string *err)
{
    if (!parentOut || hint.ringVerts < 3 || hint.centerX.size() < 2 ||
        hint.sections.size() < 2) return fail(err, "PomadeMergeTubesCpu: bad holding-edge layout");
    int const nSec = int(hint.sections.size()), rv = hint.ringVerts;
    std::vector<PomadeFrame> hintFrames; std::string derr;
    if (!PomadeTubeFramesCpu(hint, &hintFrames, &derr)) return fail(err, derr.c_str());
    // Seed a COMPLETE absolute target boundary from the current parent. This
    // makes unbound/internal edits a bit-exact no-op and keeps every other
    // L1 edge world-stable when a holding edge changes the center/frame.
    std::vector<BoundTarget> targets(size_t(nSec * rv));
    for (int s = 0; s < nSec; ++s) for (int slot = 0; slot < rv; ++slot) {
        if (!SectionSlotWorld(hint, hintFrames, s, slot,
                              targets[size_t(s * rv + slot)].p, err)) return false;
    }
    // Resolve frozen material ownership before inspecting edited coordinates.
    for (size_t offset = 0; offset < children.size(); ++offset) {
        PomadeTubeDesc const &child = children[offset];
        for (PomadeParentBoundaryBinding const &binding : child.inheritedBoundaryBindings) {
            if (binding.section < 0 || binding.section >= nSec ||
                binding.parentSlot < 0 || binding.parentSlot >= rv ||
                binding.section >= int(child.sections.size()) || binding.childSlot < 0 ||
                binding.childSlot >= int(child.sections[size_t(binding.section)].u.size()) ||
                binding.childSlot >= int(child.sections[size_t(binding.section)].v.size())) continue;
            BoundTarget &target = targets[size_t(binding.section * rv + binding.parentSlot)];
            if (!target.hasOwner || child.childIndex < target.childIndex) {
                target.hasOwner = true; target.childOffset = int(offset);
                target.childIndex = child.childIndex; target.binding = binding;
            }
        }
    }
    bool changed = false;
    for (BoundTarget &target : targets) {
        if (!target.hasOwner) continue;
        PomadeTubeDesc const &child = children[size_t(target.childOffset)];
        std::vector<PomadeFrame> childFrames;
        if (!PomadeTubeFramesCpu(child, &childFrames, &derr)) return fail(err, derr.c_str());
        float actual[3] = {};
        if (!SectionSlotWorld(child, childFrames, target.binding.section,
                              target.binding.childSlot, actual, err)) continue;
        float reference[3] = {target.p[0], target.p[1], target.p[2]};
        if (priorChildren && target.childOffset < int(priorChildren->size())) {
            PomadeTubeDesc const &prior =
                (*priorChildren)[size_t(target.childOffset)];
            std::vector<PomadeFrame> priorFrames;
            if (!PomadeTubeFramesCpu(prior, &priorFrames, &derr)) {
                return fail(err, derr.c_str());
            }
            // Binding identity is frozen on actual material slots.  The
            // gesture snapshot has the same identity, so this distinguishes
            // an internal CV edit from a true holding-corner movement even
            // when a curved child frame differs from fresh K14 geometry.
            if (!SectionSlotWorld(prior, priorFrames, target.binding.section,
                                  target.binding.childSlot, reference, err)) {
                return false;
            }
        }
        double const dx = double(actual[0]) - reference[0];
        double const dy = double(actual[1]) - reference[1];
        double const dz = double(actual[2]) - reference[2];
        if (dx * dx + dy * dy + dz * dz > 2.5e-9) {
            target.p[0] = actual[0]; target.p[1] = actual[1]; target.p[2] = actual[2];
            changed = true;
        }
    }
    if (!changed) { *parentOut = hint; return true; }
    PomadeTubeDesc result = hint;
    // A holding edit that remains in every existing parent section plane is
    // fully representable by UVs.  Do not unnecessarily recenter its cage:
    // bending K4 would rotate those planes and turn exact planar targets into
    // projection error.  Nonplanar targets retain the centroid-curve path.
    bool planarTargets = true;
    for (int s = 0; s < nSec && planarTargets; ++s) {
        float center[3] = {}; PomadeFrame frame;
        if (!PomadeSampleCenterCpu(hint, hintFrames, hint.sections[size_t(s)].t,
                                  &center[0], &center[1], &center[2],
                                  &frame, &derr)) return fail(err, derr.c_str());
        for (int slot = 0; slot < rv; ++slot) {
            BoundTarget const &target = targets[size_t(s * rv + slot)];
            float const dx = target.p[0] - center[0];
            float const dy = target.p[1] - center[1];
            float const dz = target.p[2] - center[2];
            float const axial = dx * frame.tx + dy * frame.ty + dz * frame.tz;
            float const magnitude = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::fabs(axial) > 1e-5f * (1.0f + magnitude)) {
                planarTargets = false;
                break;
            }
        }
    }
    std::vector<PomadeFrame> frames;
    if (!planarTargets) {
        for (int cv = 1; cv < int(result.centerX.size()); ++cv) {
            float const t = float(cv) / float(result.centerX.size() - 1);
            float sum[3] = {};
            for (int slot = 0; slot < rv; ++slot) {
                float p[3] = {};
                InterpBoundTarget(hint.sections, targets, slot, rv, t, p);
                sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
            }
            result.centerX[size_t(cv)] = sum[0] / float(rv);
            result.centerY[size_t(cv)] = sum[1] / float(rv);
            result.centerZ[size_t(cv)] = sum[2] / float(rv);
        }
        if (!PomadeTubeFramesCpu(result, &frames, &derr)) return fail(err, derr.c_str());
    } else {
        frames = hintFrames;
    }
    for (int s = 0; s < nSec; ++s) {
        PomadeTubeSection &section = result.sections[size_t(s)];
        float center[3] = {}; PomadeFrame frame;
        if (!PomadeSampleCenterCpu(result, frames, section.t, &center[0], &center[1],
                                  &center[2], &frame, &derr)) return fail(err, derr.c_str());
        float const scale = section.scale > 1e-6f ? section.scale : 1e-6f;
        float const ct = std::cos(section.twist), st = std::sin(section.twist);
        for (int slot = 0; slot < rv; ++slot) {
            float const dx = targets[size_t(s * rv + slot)].p[0] - center[0];
            float const dy = targets[size_t(s * rv + slot)].p[1] - center[1];
            float const dz = targets[size_t(s * rv + slot)].p[2] - center[2];
            float const u = dx * frame.nx + dy * frame.ny + dz * frame.nz;
            float const v = dx * frame.bx + dy * frame.by + dz * frame.bz;
            section.u[size_t(slot)] = (u * ct + v * st) / scale;
            section.v[size_t(slot)] = (-u * st + v * ct) / scale;
        }
    }
    *parentOut = std::move(result);
    return true;
}

}  // namespace

float PomadeCenterArcLength(float const *cx, float const *cy, float const *cz,
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

bool PomadeValidateSubdivide(PomadeSubdivideDesc const &params, std::string *err)
{
    if (params.count < 2 || params.count > 8) {
        return fail(err, "PomadeValidateSubdivide: count in [2, 8]");
    }
    if (params.splitMode != PomadeSplit_KMeans &&
        params.splitMode != PomadeSplit_Edge) {
        return fail(err, "PomadeValidateSubdivide: splitMode kmeans | edge");
    }
    if (params.splitMode == PomadeSplit_Edge) {
        if (params.count != 2) {
            return fail(err,
                        "PomadeValidateSubdivide: edge mode needs count == 2");
        }
        double const n = double(params.edgeA) * double(params.edgeA) +
                         double(params.edgeB) * double(params.edgeB);
        if (!(n > 0.0)) {
            return fail(err,
                        "PomadeValidateSubdivide: degenerate edge line");
        }
    }
    return true;
}

bool PomadeSubdivideTubeCpu(PomadeTubeDesc const &parent,
                           std::vector<PomadeFrame> const &parentFrames,
                           PomadeSubdivideDesc const &params,
                           std::vector<PomadeTubeDesc> *children,
                           std::string *err)
{
    if (!children) {
        return fail(err, "PomadeSubdivideTubeCpu: null output");
    }
    if (!PomadeValidateSubdivide(params, err) ||
        !ValidateTube(parent, "parent", err)) {
        return false;
    }
    int const nCv = int(parent.centerX.size());
    int const nSec = int(parent.sections.size());
    if (parentFrames.size() != size_t(nCv)) {
        return fail(err, "PomadeSubdivideTubeCpu: frames match center CVs");
    }
    // Root ring in placed coords + sub-region partition.
    std::vector<Pt> rootRing;
    PlaceRing(parent.sections.front(), &rootRing);
    for (size_t slot = 0; slot < rootRing.size(); ++slot) {
        rootRing[slot].parentSlot = int(slot);
    }
    double rootMeanU = 0.0, rootMeanV = 0.0;
    RingMean(rootRing, &rootMeanU, &rootMeanV);
    float const rootRadius = MeanRadius(rootRing);
    if (!(rootRadius > 0.0f)) {
        // A K6 reject here is a malformed propagated root chart, not a
        // partitioning failure. Preserve enough authored and placed detail
        // to distinguish scalar collapse, float cancellation, and slot
        // layout drift in a later-state replay.
        PomadeTubeSection const &rootSection = parent.sections.front();
        float minU = std::numeric_limits<float>::infinity();
        float maxU = -std::numeric_limits<float>::infinity();
        float minV = std::numeric_limits<float>::infinity();
        float maxV = -std::numeric_limits<float>::infinity();
        std::string rawUv, placedUv;
        for (size_t i = 0; i < rootRing.size(); ++i) {
            Pt const &placed = rootRing[i];
            minU = std::min(minU, placed.u);
            maxU = std::max(maxU, placed.u);
            minV = std::min(minV, placed.v);
            maxV = std::max(maxV, placed.v);
            rawUv += (i == 0 ? "" : ";") +
                std::to_string(rootSection.u[i]) + "," +
                std::to_string(rootSection.v[i]);
            placedUv += (i == 0 ? "" : ";") +
                std::to_string(placed.u) + "," +
                std::to_string(placed.v);
        }
        std::string const text =
            "PomadeSubdivideTubeCpu: degenerate root ring tube=" +
            std::to_string(parent.tubeId) + " rootRv=" +
            std::to_string(parent.ringVerts) + " scale=" +
            std::to_string(rootSection.scale) + " twist=" +
            std::to_string(rootSection.twist) + " mean=(" +
            std::to_string(rootMeanU) + "," + std::to_string(rootMeanV) +
            ") radius=" + std::to_string(rootRadius) + " bounds=(" +
            std::to_string(minU) + "," + std::to_string(maxU) + "," +
            std::to_string(minV) + "," + std::to_string(maxV) +
            ") rawUv=" + rawUv + " placedUv=" + placedUv;
        return fail(err, text.c_str());
    }
    std::vector<int> assign(rootRing.size(), 0);
    auto centroids = std::vector<Pt>(size_t(params.count));
    if (params.splitMode == PomadeSplit_KMeans) {
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
                            "PomadeSubdivideTubeCpu: edge leaves a side "
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
        return fail(err, "PomadeSubdivideTubeCpu: degenerate root ring");
    }
    // First retain the established similarity chart.  A strongly asymmetric
    // asymmetric chart can make one of those carried Voronoi cells empty even though
    // every root KMeans cluster has material.  In that case retry the whole
    // KMeans split with per-section means of the fixed root assignments.  Do
    // not mix the two charts: child centers and every section must use the
    // same generators.
    std::vector<std::vector<Pt>> cells(
        size_t(nSec * params.count));
    std::vector<std::vector<Pt>> sectionGenerators(
        size_t(nSec), std::vector<Pt>(size_t(params.count)));
    std::vector<std::vector<Pt>> sectionRings(static_cast<size_t>(nSec));
    std::vector<bool> sameCharts(size_t(nSec), false);
    for (int s = 0; s < nSec; ++s) {
        std::vector<Pt> &ring = sectionRings[size_t(s)];
        PlaceRing(parent.sections[size_t(s)], &ring);
        for (size_t slot = 0; slot < ring.size(); ++slot) {
            ring[slot].parentSlot = int(slot);
        }
        double secMeanU = 0.0, secMeanV = 0.0;
        RingMean(ring, &secMeanU, &secMeanV);
        float const secRadius = MeanRadius(ring);
        sameCharts[size_t(s)] = secMeanU == rootMeanU &&
                              secMeanV == rootMeanV &&
                              secRadius == rootRadius;
        if (params.splitMode == PomadeSplit_KMeans) {
            if (sameCharts[size_t(s)] || !(secRadius > 0.0f)) {
                sectionGenerators[size_t(s)] = centroids;
            } else {
                double const q = double(secRadius) / double(rootRadius);
                for (int c = 0; c < params.count; ++c) {
                    sectionGenerators[size_t(s)][size_t(c)].u = float(
                        secMeanU + (double(centroids[size_t(c)].u) -
                                    rootMeanU) * q);
                    sectionGenerators[size_t(s)][size_t(c)].v = float(
                        secMeanV + (double(centroids[size_t(c)].v) -
                                    rootMeanV) * q);
                }
            }
        }
    }
    auto cellValid = [&](std::vector<Pt> const &poly) {
        double const area = std::fabs(PolyArea(poly));
        double const emptyTol =
            1e-12 * double(rootRadius) * double(rootRadius);
        return poly.size() >= 3 && area > emptyTol;
    };
    auto buildKMeansCells = [&](bool materialMeans) {
        bool allValid = true;
        for (int s = 0; s < nSec; ++s) {
            std::vector<Pt> &generators = sectionGenerators[size_t(s)];
            if (materialMeans) {
                std::fill(generators.begin(), generators.end(), Pt{});
                std::vector<int> counts(size_t(params.count), 0);
                for (size_t i = 0; i < sectionRings[size_t(s)].size(); ++i) {
                    int const c = assign[i];
                    generators[size_t(c)].u += sectionRings[size_t(s)][i].u;
                    generators[size_t(c)].v += sectionRings[size_t(s)][i].v;
                    ++counts[size_t(c)];
                }
                for (int c = 0; c < params.count; ++c) {
                    if (counts[size_t(c)] == 0) {
                        return false;
                    }
                    generators[size_t(c)].u /= float(counts[size_t(c)]);
                    generators[size_t(c)].v /= float(counts[size_t(c)]);
                }
            }
            for (int c = 0; c < params.count; ++c) {
                std::vector<Pt> &cell =
                    cells[size_t(s * params.count + c)];
                cell = ClipToCell(sectionRings[size_t(s)], generators, c);
                allValid = cellValid(cell) && allValid;
            }
        }
        return allValid;
    };

    bool materialFallback = false;
    if (params.splitMode == PomadeSplit_KMeans) {
        if (!buildKMeansCells(/*materialMeans=*/false)) {
            materialFallback = true;
            if (!buildKMeansCells(/*materialMeans=*/true)) {
                // Keep this exact failure actionable. A valid convex section
                // contains each mean of its assigned root-material slots, so
                // an empty material cell identifies either a collapsed
                // generator pair or an invalid/reordered chart.
                for (int s = 0; s < nSec; ++s) {
                    std::vector<int> counts(size_t(params.count), 0);
                    for (int a : assign) {
                        ++counts[size_t(a)];
                    }
                    for (int c = 0; c < params.count; ++c) {
                        std::vector<Pt> const &cell =
                            cells[size_t(s * params.count + c)];
                        if (cellValid(cell)) {
                            continue;
                        }
                        Pt const &g =
                            sectionGenerators[size_t(s)][size_t(c)];
                        double nearestGenerator2 =
                            std::numeric_limits<double>::infinity();
                        for (int o = 0; o < params.count; ++o) {
                            if (o == c) {
                                continue;
                            }
                            Pt const &other = sectionGenerators[size_t(s)]
                                                               [size_t(o)];
                            double const du = double(g.u) - double(other.u);
                            double const dv = double(g.v) - double(other.v);
                            nearestGenerator2 = std::min(
                                nearestGenerator2, du * du + dv * dv);
                        }
                        bool inside = false;
                        std::vector<Pt> const &ring =
                            sectionRings[size_t(s)];
                        double const signedRingArea = PolyArea(ring);
                        int winding = 0;
                        bool convex = true;
                        std::string ringUv;
                        for (size_t i = 0, j = ring.size() - 1;
                             i < ring.size(); j = i++) {
                            Pt const &a = ring[i];
                            Pt const &b = ring[j];
                            bool const crosses =
                                (a.v > g.v) != (b.v > g.v);
                            if (crosses &&
                                double(g.u) <
                                    (double(b.u) - double(a.u)) *
                                            (double(g.v) - double(a.v)) /
                                            (double(b.v) - double(a.v)) +
                                        double(a.u)) {
                                inside = !inside;
                            }
                            ringUv += (i == 0 ? "" : ";") +
                                std::to_string(a.u) + "," +
                                std::to_string(a.v);
                            Pt const &next = ring[(i + 1) % ring.size()];
                            double const cross =
                                (double(a.u) - double(b.u)) *
                                    (double(next.v) - double(a.v)) -
                                (double(a.v) - double(b.v)) *
                                    (double(next.u) - double(a.u));
                            if (std::fabs(cross) > 1e-10) {
                                int const sign = cross > 0.0 ? 1 : -1;
                                convex = convex &&
                                    (winding == 0 || winding == sign);
                                winding = sign;
                            }
                        }
                        std::string const text =
                            "PomadeSubdivideTubeCpu: material partition "
                            "empty c=" + std::to_string(c) +
                            " s=" + std::to_string(s) +
                            " verts=" + std::to_string(int(cell.size())) +
                            " area=" +
                            std::to_string(std::fabs(PolyArea(cell))) +
                            " ringArea=" +
                            std::to_string(std::fabs(PolyArea(ring))) +
                            " signedRingArea=" +
                            std::to_string(signedRingArea) +
                            " convex=" + std::to_string(int(convex)) +
                            " gen=(" + std::to_string(g.u) + "," +
                            std::to_string(g.v) + ") assigned=" +
                            std::to_string(counts[size_t(c)]) +
                            " nearestGen=" +
                            std::to_string(std::sqrt(nearestGenerator2)) +
                            " inside=" + std::to_string(int(inside)) +
                            " ringUv=" + ringUv;
                        return fail(err, text.c_str());
                    }
                }
                return fail(err,
                    "PomadeSubdivideTubeCpu: material partition leaves a "
                    "sub-region empty");
            }
        }
    } else {
        for (int s = 0; s < nSec; ++s) {
            double secMeanU = 0.0, secMeanV = 0.0;
            RingMean(sectionRings[size_t(s)], &secMeanU, &secMeanV);
            float const secRadius = MeanRadius(sectionRings[size_t(s)]);
            double const a = double(params.edgeA);
            double const b = double(params.edgeB);
            double cut = double(params.edgeC);
            if (!sameCharts[size_t(s)] && secRadius > 0.0f) {
                double const q = double(secRadius) / double(rootRadius);
                double const rootC =
                    a * rootMeanU + b * rootMeanV + cut;
                cut = q * rootC - (a * secMeanU + b * secMeanV);
            }
            for (int c = 0; c < params.count; ++c) {
                std::vector<Pt> &cell =
                    cells[size_t(s * params.count + c)];
                cell = ClipHalfPlane(sectionRings[size_t(s)], a, b, cut,
                                     c == 1);
                if (!cellValid(cell)) {
                    std::string const text =
                        "PomadeSubdivideTubeCpu: empty sub-region c=" +
                        std::to_string(c) + " s=" + std::to_string(s) +
                        " verts=" + std::to_string(int(cell.size())) +
                        " area=" + std::to_string(std::fabs(PolyArea(cell))) +
                        " (raise the ring density or change the seed)";
                    return fail(err, text.c_str());
                }
            }
        }
    }

    children->resize(size_t(params.count));
    for (int c = 0; c < params.count; ++c) {
        PomadeTubeDesc &child = (*children)[size_t(c)];
        child.tubeId = parent.tubeId * 16 + 1 + c;
        child.regionId = parent.regionId;
        child.level = parent.level + 1;
        child.parentTubeId = parent.tubeId;
        child.childIndex = c;
        child.rootFramePinned = parent.rootFramePinned;
        child.rootFrame = parent.rootFrame;
        child.frameReference = parent.frameReference;
        child.inheritedBoundaryBindings.clear();
        // Centers follow the legacy similarity centroid unless this split
        // needs the material-generator fallback, in which case they use the
        // matching assigned-material mean at each center-CV station.
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
            PomadeFrame const &fr = parentFrames[size_t(i)];
            float ox = 0.0f, oy = 0.0f;
            if (materialFallback) {
                int count = 0;
                for (size_t slot = 0; slot < ringAt.size(); ++slot) {
                    if (assign[slot] == c) {
                        ox += ringAt[slot].u;
                        oy += ringAt[slot].v;
                        ++count;
                    }
                }
                if (count == 0) {
                    return fail(err,
                        "PomadeSubdivideTubeCpu: empty material cluster");
                }
                ox /= float(count);
                oy /= float(count);
            } else {
                float const ratio =
                    rootRadius > 0.0f ? radiusAt / rootRadius : 1.0f;
                ox = centroids[size_t(c)].u * ratio;
                oy = centroids[size_t(c)].v * ratio;
            }
            child.centerX[size_t(i)] =
                parent.centerX[size_t(i)] + fr.nx * ox + fr.bx * oy;
            child.centerY[size_t(i)] =
                parent.centerY[size_t(i)] + fr.ny * ox + fr.by * oy;
            child.centerZ[size_t(i)] =
                parent.centerZ[size_t(i)] + fr.nz * ox + fr.bz * oy;
        }
        // Sections were clipped as one consistent split above. Recenter on
        // the same generator chart used by the child center offsets.
        auto clipped = std::vector<std::vector<Pt>>(size_t(nSec));
        int target = 0;
        for (int s = 0; s < nSec; ++s) {
            clipped[size_t(s)] = cells[size_t(s * params.count + c)];
            DropCoincidentVertices(&clipped[size_t(s)], 1e-4 * spacing);
            SubdivideLongEdges(&clipped[size_t(s)], 2.0 * spacing,
                               kMaxRingVerts, kSplitTieRel);
            // Recenter with the same generator chart as the center CVs.
            {
                float ou = 0.0f, ov = 0.0f;
                if (materialFallback) {
                    Pt const &generator =
                        sectionGenerators[size_t(s)][size_t(c)];
                    ou = generator.u;
                    ov = generator.v;
                } else {
                    std::vector<Pt> ringAt;
                    float radiusAt = 0.0f;
                    InterpSectionAt(parent.sections, parent.ringVerts,
                                    parent.sections[size_t(s)].t, &ringAt,
                                    &radiusAt);
                    float const ratio =
                        rootRadius > 0.0f ? radiusAt / rootRadius : 1.0f;
                    ou = centroids[size_t(c)].u * ratio;
                    ov = centroids[size_t(c)].v * ratio;
                }
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
        // The previous section's final slot order: section 0 (the scalp
        // attachment chart) is the anchor, every later section follows
        // its predecessor.  The K14 bindings below are recorded from the
        // aligned polygon, so their child slot index rotates with it.
        std::vector<Pt> previous;
        for (int s = 0; s < nSec; ++s) {
            std::vector<Pt> poly = clipped[size_t(s)];
            if (int(poly.size()) > target) {
                poly = ResamplePoly(poly, target);
            } else {
                // Exact-length pad: a 0 bound splits the longest edge
                // first, so every inserted point stays on the polygon.
                // (Fully degenerate runs fall back to the resample.)
                SubdivideLongEdges(&poly, 0.0, target, kSplitTieRel);
                if (int(poly.size()) != target) {
                    poly = ResamplePoly(clipped[size_t(s)], target);
                }
            }
            if (s > 0) {
                AlignRingToReference(&poly, previous);
            }
            previous = poly;
            PomadeTubeSection &cs = child.sections[size_t(s)];
            cs.t = parent.sections[size_t(s)].t;
            cs.scale = 1.0f;
            cs.twist = 0.0f;
            cs.u.resize(size_t(target));
            cs.v.resize(size_t(target));
            std::vector<bool> recorded(size_t(parent.ringVerts), false);
            for (int i = 0; i < target; ++i) {
                cs.u[size_t(i)] = poly[size_t(i)].u;
                cs.v[size_t(i)] = poly[size_t(i)].v;
                int const parentSlot = poly[size_t(i)].parentSlot;
                if (parentSlot >= 0 && parentSlot < parent.ringVerts &&
                    !recorded[size_t(parentSlot)]) {
                    child.inheritedBoundaryBindings.push_back(
                        {s, parentSlot, i});
                    recorded[size_t(parentSlot)] = true;
                }
            }
        }
    }
    return true;
}

bool PomadeParentAverageCpu(std::vector<PomadeTubeDesc> const &children,
                           PomadeTubeDesc *parentOut, std::string *err)
{
    if (!parentOut) {
        return fail(err, "PomadeParentAverageCpu: null output");
    }
    if (children.empty()) {
        return fail(err, "PomadeParentAverageCpu: no children");
    }
    for (auto const &ch : children) {
        if (!ValidateTube(ch, "child", err)) {
            return false;
        }
    }
    int const nCv = int(children[0].centerX.size());
    int const nSec = int(children[0].sections.size());
    int const rv = children[0].ringVerts;
    // Siblings retain their common scalp plane when averaged back into a
    // parent. An aggregate of unrelated planes has no single pinned frame.
    parentOut->rootFramePinned = children[0].rootFramePinned;
    parentOut->rootFrame = children[0].rootFrame;
    parentOut->frameReference = children[0].frameReference;
    for (auto const &child : children) {
        if (!RootFrameEqual(children[0], child)) {
            parentOut->rootFramePinned = false;
            // A K7 aggregate of differently oriented material charts has
            // no common transported reference.  Its freshly fitted section
            // data is expressed in the legacy world chart.
            parentOut->frameReference = {{
                1.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 1.0f}};
            break;
        }
    }
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
    std::vector<PomadeFrame> pframes;
    {
        std::string derr;
        if (!PomadeTubeFramesCpu(*parentOut, &pframes, &derr)) {
            return fail(err, derr.c_str());
        }
    }
    parentOut->ringVerts = rv;
    parentOut->sections.resize(size_t(nSec));
    // Per-child frames for world placement.
    std::vector<std::vector<PomadeFrame>> cframes(children.size());
    for (size_t c = 0; c < children.size(); ++c) {
        std::string derr;
        if (!PomadeTubeFramesCpu(children[c], &cframes[c], &derr)) {
            return fail(err, derr.c_str());
        }
    }
    for (int s = 0; s < nSec; ++s) {
        float const t = children[0].sections[size_t(s)].t;
        float pc[3];
        PomadeFrame pf;
        {
            // Center + frame of the averaged parent at t.
            PomadeTubeDesc tmp = *parentOut;
            tmp.sections = children[0].sections;  // t-domain carrier
            std::string derr;
            if (!PomadeSampleCenterCpu(tmp, pframes, t, &pc[0], &pc[1], &pc[2],
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
            PomadeTubeDesc ctmp = children[c];
            float cc[3];
            PomadeFrame cf;
            std::string derr;
            if (!PomadeSampleCenterCpu(ctmp, cframes[c], t, &cc[0], &cc[1],
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
                        "PomadeParentAverageCpu: degenerate union ring");
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
        PomadeTubeSection &ps = parentOut->sections[size_t(s)];
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
    // see PomadeMergeTubesCpu).
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

bool PomadeMergeTubesCpu(std::vector<PomadeTubeDesc> const &children,
                        PomadeSubdivideDesc const &params,
                        PomadeTubeDesc const *roundTripParent,
                        PomadeTubeDesc *parentOut, std::string *err,
                        std::vector<PomadeTubeDesc> const *priorChildren)
{
    if (!parentOut) {
        return fail(err, "PomadeMergeTubesCpu: null output");
    }
    if (children.empty()) {
        return fail(err, "PomadeMergeTubesCpu: no children");
    }
    if (!PomadeValidateSubdivide(params, err)) {
        return false;
    }
    if (roundTripParent) {
        // Untouched fast path: re-derive and bit-compare.
        if (!ValidateTube(*roundTripParent, "hint", err)) {
            return false;
        }
        std::vector<PomadeFrame> frames;
        std::string derr;
        if (!PomadeTubeFramesCpu(*roundTripParent, &frames, &derr)) {
            return fail(err, derr.c_str());
        }
        std::vector<PomadeTubeDesc> expect;
        if (!PomadeSubdivideTubeCpu(*roundTripParent, frames, params, &expect,
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
        // Only original parent corners inherited by a child can drive K7.
        // Internal K14 cut edges are child-local sculpt detail, not a reason
        // to refit the whole L1 volume.
        return PomadeParentHoldingEdgesCpu(children, priorChildren,
                                          *roundTripParent, parentOut, err);
    }
    return PomadeParentAverageCpu(children, parentOut, err);
}

bool PomadeDeriveChildCpu(PomadeTubeDesc const &parent,
                         std::vector<PomadeFrame> const &parentFrames,
                         PomadeSubdivideDesc const &params, int childIndex,
                         PomadeTubeDesc *derived, std::string *err)
{
    if (!derived) {
        return fail(err, "PomadeDeriveChildCpu: null output");
    }
    std::vector<PomadeTubeDesc> all;
    if (!PomadeSubdivideTubeCpu(parent, parentFrames, params, &all, err)) {
        return false;
    }
    if (childIndex < 0 || childIndex >= int(all.size())) {
        return fail(err, "PomadeDeriveChildCpu: child index out of range");
    }
    *derived = all[size_t(childIndex)];
    return true;
}

bool PomadeComputeDeltasCpu(PomadeTubeDesc const &actual,
                           PomadeTubeDesc const &derived,
                           std::vector<PomadeFrame> const &derivedFrames,
                           PomadeShapeDeltas *deltas, std::string *err)
{
    if (!deltas) {
        return fail(err, "PomadeComputeDeltasCpu: null output");
    }
    int const nCv = int(derived.centerX.size());
    if (int(actual.centerX.size()) != nCv ||
        derivedFrames.size() != size_t(nCv) ||
        actual.sections.size() != derived.sections.size()) {
        return fail(err, "PomadeComputeDeltasCpu: layout mismatch");
    }
    deltas->centerDu.assign(size_t(nCv), 0.0f);
    deltas->centerDv.assign(size_t(nCv), 0.0f);
    deltas->centerDw.assign(size_t(nCv), 0.0f);
    for (int i = 0; i < nCv; ++i) {
        float const wx = actual.centerX[size_t(i)] - derived.centerX[size_t(i)];
        float const wy = actual.centerY[size_t(i)] - derived.centerY[size_t(i)];
        float const wz = actual.centerZ[size_t(i)] - derived.centerZ[size_t(i)];
        PomadeFrame const &fr = derivedFrames[size_t(i)];
        deltas->centerDu[size_t(i)] = wx * fr.nx + wy * fr.ny + wz * fr.nz;
        deltas->centerDv[size_t(i)] = wx * fr.bx + wy * fr.by + wz * fr.bz;
        deltas->centerDw[size_t(i)] = wx * fr.tx + wy * fr.ty + wz * fr.tz;
    }
    deltas->sections.resize(derived.sections.size());
    for (size_t s = 0; s < derived.sections.size(); ++s) {
        PomadeTubeSection const &sa = actual.sections[s];
        PomadeTubeSection const &sd = derived.sections[s];
        if (sa.t != sd.t || sa.u.size() != sd.u.size() ||
            sa.v.size() != sd.v.size()) {
            return fail(err, "PomadeComputeDeltasCpu: section mismatch");
        }
        PomadeTubeSection &dd = deltas->sections[s];
        dd.t = sd.t;
        // Section scale/twist are authored section-space residuals just like
        // the ring coordinates.  Dropping them here made a K7 rebase erase a
        // child Scale/Twist edit on the next parent K6.
        dd.scale = sa.scale - sd.scale;
        dd.twist = sa.twist - sd.twist;
        dd.u.resize(sd.u.size());
        dd.v.resize(sd.u.size());
        for (size_t i = 0; i < sd.u.size(); ++i) {
            dd.u[i] = sa.u[i] - sd.u[i];
            dd.v[i] = sa.v[i] - sd.v[i];
        }
    }
    return true;
}

bool PomadeRescaleCenterLength(float const *cx, float const *cy,
                              float const *cz, int nCv, float targetLen,
                              std::vector<float> *ox, std::vector<float> *oy,
                              std::vector<float> *oz, std::string *err)
{
    if (!ox || !oy || !oz) {
        return fail(err, "PomadeRescaleCenterLength: null output");
    }
    if (!cx || !cy || !cz || nCv < 2 || !(targetLen > 0.0f)) {
        return fail(err, "PomadeRescaleCenterLength: bad curve or length");
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
        return fail(err, "PomadeRescaleCenterLength: degenerate curve");
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

bool PomadeResampleDescRingsCpu(PomadeTubeDesc const &tube, int ringVerts,
                               PomadeTubeDesc *out, std::string *err)
{
    if (!out) {
        return fail(err, "PomadeResampleDescRingsCpu: null output");
    }
    if (!ValidateTube(tube, "tube", err)) {
        return false;
    }
    if (ringVerts < 3 || ringVerts > 256) {
        return fail(err, "PomadeResampleDescRingsCpu: ringVerts in [3, 256]");
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

bool PomadeAlignSectionRingCpu(PomadeTubeSection *section,
                              PomadeTubeSection const &neighbour,
                              std::vector<int> *outFrom)
{
    if (outFrom) {
        outFrom->clear();
    }
    if (!section) {
        return false;
    }
    size_t const n = section->u.size();
    if (n < 3 || section->v.size() != n || neighbour.u.size() != n ||
        neighbour.v.size() != n) {
        return false;
    }
    // Compare the rings as K5 places them (scale, then twist) so a twisted
    // or scaled neighbour is matched by shape, not by raw chart coords.
    std::vector<Pt> placed, reference;
    PlaceRing(*section, &placed);
    PlaceRing(neighbour, &reference);
    if (placed.size() != n || reference.size() != n) {
        return false;
    }
    // Carry each slot's index through the alignment in the provenance
    // field (unused by the comparison).
    for (size_t i = 0; i < n; ++i) {
        placed[i].parentSlot = int(i);
    }
    AlignRingToReference(&placed, reference);
    bool changed = false;
    for (size_t i = 0; i < n; ++i) {
        changed = changed || placed[i].parentSlot != int(i);
    }
    if (!changed) {
        return false;
    }
    std::vector<float> u(n), v(n);
    std::vector<int> from(n);
    for (size_t i = 0; i < n; ++i) {
        size_t const j = size_t(placed[i].parentSlot);
        from[i] = int(j);
        u[i] = section->u[j];
        v[i] = section->v[j];
    }
    section->u = std::move(u);
    section->v = std::move(v);
    if (outFrom) {
        *outFrom = std::move(from);
    }
    return true;
}

bool PomadeHierarchicalSculptCpu(
    PomadeTubeDesc const &parentNew,
    std::vector<PomadeFrame> const &parentNewFrames,
    PomadeSubdivideDesc const &params, int childIndex,
    PomadeTubeDesc const &oldActual, PomadeTubeDesc const &oldDerived,
    PomadeShapeDeltas const &oldStored, bool lockChildren,
    bool preserveLength, PomadeTubeDesc *outActual,
    PomadeShapeDeltas *outStored, std::string *err)
{
    if (!outActual || !outStored) {
        return fail(err, "PomadeHierarchicalSculptCpu: null output");
    }
    PomadeTubeDesc derivedNew;
    if (!PomadeDeriveChildCpu(parentNew, parentNewFrames, params,
        childIndex, &derivedNew, err)) {
        return false;
    }
    // Keep the child's established ring layout when K14 repartitions a
    // parent.  The stored residual is authored against that layout; applying
    // a raw new child and then replacing its reference would otherwise turn a
    // no-op K6 into an implicit resample of the actual sculpt.
    if (derivedNew.ringVerts != oldActual.ringVerts) {
        PomadeTubeDesc matched;
        if (!PomadeResampleDescRingsCpu(derivedNew, oldActual.ringVerts,
                                       &matched, err)) {
            return false;
        }
        derivedNew = std::move(matched);
    }
    return PomadeHierarchicalSculptApplyCpu(derivedNew, oldActual,
        oldDerived, oldStored, lockChildren, preserveLength, outActual,
        outStored, err);
}

bool PomadeHierarchicalSculptApplyCpu(
    PomadeTubeDesc const &derivedNew, PomadeTubeDesc const &oldActual,
    PomadeTubeDesc const &oldDerived, PomadeShapeDeltas const &oldStored,
    bool lockChildren, bool preserveLength, PomadeTubeDesc *outActual,
    PomadeShapeDeltas *outStored, std::string *err)
{
    if (!outActual || !outStored) {
        return fail(err, "PomadeHierarchicalSculptCpu: null output");
    }
    // Layout contract: center counts, section counts and section t values
    // must match (topology ops re-subdivide instead). RING counts may
    // change freely -- a moved parent re-partitions -- and deltas below
    // resample across the change by arc length.
    if (derivedNew.centerX.size() != oldActual.centerX.size() ||
        derivedNew.sections.size() != oldActual.sections.size()) {
        return fail(err,
                    "PomadeHierarchicalSculptCpu: center/section counts "
                    "changed under the sculpt (re-subdivide)");
    }
    for (size_t s = 0; s < derivedNew.sections.size(); ++s) {
        if (derivedNew.sections[s].t != oldActual.sections[s].t) {
            return fail(err,
                        "PomadeHierarchicalSculptCpu: section t drifted "
                        "(re-subdivide)");
        }
    }
    if (derivedNew.ringVerts != oldActual.ringVerts &&
        !oldActual.inheritedBoundaryBindings.empty()) {
        return fail(err,
                    "PomadeHierarchicalSculptCpu: boundary material slots "
                    "need an explicit topology remap");
    }
    auto resampleDeltaRing = [&](std::vector<float> const &su,
                                 std::vector<float> const &sv,
                                 int newCount, std::vector<float> *ou,
                                 std::vector<float> *ov) {
        // Equal layouts retain the authored slot identity exactly. Resampling
        // a nonuniform delta polygon to its own count redistributes values by
        // delta-vector arc length, so repeated no-op K6 passes otherwise
        // shear a child even when neither parent nor layout changed.
        if (int(su.size()) == newCount && int(sv.size()) == newCount) {
            *ou = su;
            *ov = sv;
            return;
        }
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
    std::vector<PomadeFrame> newFrames;
    {
        std::string derr;
        if (!PomadeTubeFramesCpu(derivedNew, &newFrames, &derr)) {
            return fail(err, derr.c_str());
        }
    }
    int const nCv = int(derivedNew.centerX.size());
    PomadeTubeDesc actual = derivedNew;
    // Derived slot order may be regenerated during K6. When the actual
    // layout is unchanged, its inherited boundary triples remain the sole
    // material identity for K7; never infer a replacement from fresh clips.
    if (oldActual.ringVerts == derivedNew.ringVerts) {
        actual.inheritedBoundaryBindings =
            oldActual.inheritedBoundaryBindings;
    }
    if (lockChildren) {
        // Rigid ride: world deltas frozen, stored deltas copied exactly.
        if (oldActual.centerX.size() != oldDerived.centerX.size() ||
            oldActual.sections.size() != oldDerived.sections.size()) {
            return fail(err,
                        "PomadeHierarchicalSculptCpu: stale child layout");
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
                            "PomadeHierarchicalSculptCpu: stale child "
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
            actual.sections[s].scale +=
                oldActual.sections[s].scale - oldDerived.sections[s].scale;
            actual.sections[s].twist +=
                oldActual.sections[s].twist - oldDerived.sections[s].twist;
        }
        *outActual = actual;
        *outStored = oldStored;
        return true;
    }
    // Re-apply stored deltas in the new derived frames.
    if (oldStored.centerDu.size() != size_t(nCv) ||
        oldStored.sections.size() != derivedNew.sections.size()) {
        return fail(err, "PomadeHierarchicalSculptCpu: stored layout drift");
    }
    for (int i = 0; i < nCv; ++i) {
        PomadeFrame const &fr = newFrames[size_t(i)];
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
        actual.sections[s].scale += oldStored.sections[s].scale;
        actual.sections[s].twist += oldStored.sections[s].twist;
    }
    if (preserveLength) {
        float const oldLen =
            PomadeCenterArcLength(oldActual.centerX.data(),
                                 oldActual.centerY.data(),
                                 oldActual.centerZ.data(), nCv);
        std::vector<float> ox, oy, oz;
        std::string derr;
        if (!PomadeRescaleCenterLength(actual.centerX.data(),
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
        sculpted = sculpted || (s.scale != 0.0f) || (s.twist != 0.0f);
        for (float v : s.u) {
            sculpted = sculpted || (v != 0.0f);
        }
        for (float v : s.v) {
            sculpted = sculpted || (v != 0.0f);
        }
    }
    if (!sculpted) {
        PomadeClearDeltas(derivedNew, outStored);
        *outActual = actual;
        return true;
    }
    if (!PomadeComputeDeltasCpu(actual, derivedNew, newFrames, outStored,
                               err)) {
        return false;
    }
    *outActual = actual;
    return true;
}

int PomadeOwningChildCell(float const parentRootCenter[3],
                         PomadeFrame const &parentRootFrame,
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

}  // namespace usdGenPomade
