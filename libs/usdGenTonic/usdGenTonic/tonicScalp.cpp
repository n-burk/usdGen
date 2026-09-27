// usdGenTonic — scalp binding + LBVH + raycast implementation (K1 host).
#include "usdGenTonic/tonicScalp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace usdGenTonic {

namespace {

float _Dot3(float const a[3], float const b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void _Cross3(float const a[3], float const b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Newell normal (unnormalized) + centroid + area of one face.
void _FaceFrame(TonicScalpMesh const &mesh, int face, float n[3], float c[3],
                float *area)
{
    int const *counts = mesh.faceVertexCounts.data();
    int const *indices = mesh.faceVertexIndices.data();
    int const *offsets = mesh.faceOffsets.data();
    int const nv = counts[face];
    int const off = offsets[face];
    float const *pts = mesh.points.data();
    n[0] = n[1] = n[2] = 0.0f;
    c[0] = c[1] = c[2] = 0.0f;
    for (int i = 0; i < nv; ++i) {
        float const *p0 = pts + size_t(indices[off + i]) * 3;
        float const *p1 = pts + size_t(indices[off + (i + 1) % nv]) * 3;
        n[0] += (p0[1] - p1[1]) * (p0[2] + p1[2]);
        n[1] += (p0[2] - p1[2]) * (p0[0] + p1[0]);
        n[2] += (p0[0] - p1[0]) * (p0[1] + p1[1]);
        c[0] += p0[0];
        c[1] += p0[1];
        c[2] += p0[2];
    }
    c[0] /= float(nv);
    c[1] /= float(nv);
    c[2] /= float(nv);
    float const len = std::sqrt(_Dot3(n, n));
    *area = 0.5f * len;
    if (len > 0.0f) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    } else {
        n[0] = 0.0f;
        n[1] = 1.0f;
        n[2] = 0.0f;
    }
}

// Watertight dominant-axis shear test.  Möller–Trumbore's independently
// rounded barycentrics can both fall just outside two fan triangles sharing
// an edge, leaving an artist-visible crack.  Shared vertices below take the
// same transformed coordinates and each determinant is separately rounded,
// so a reversed shared edge is exactly sign-opposed.  There is deliberately
// no broad epsilon: silhouette misses remain misses.
double _EdgeDet(double ax, double ay, double bx, double by)
{
    volatile double const lhs = ax * by;
    volatile double const rhs = ay * bx;
    return lhs - rhs;
}

float _RayTriangle(float const origin[3], float const dir[3], float const v0[3],
                   float const v1[3], float const v2[3])
{
    float const inf = std::numeric_limits<float>::infinity();
    int kz = 0;
    if (std::fabs(dir[1]) > std::fabs(dir[kz])) {
        kz = 1;
    }
    if (std::fabs(dir[2]) > std::fabs(dir[kz])) {
        kz = 2;
    }
    if (dir[kz] == 0.0f) {
        return inf;
    }
    int kx = (kz + 1) % 3;
    int ky = (kx + 1) % 3;
    if (dir[kz] < 0.0f) {
        std::swap(kx, ky);
    }
    double const sx = double(dir[kx]) / double(dir[kz]);
    double const sy = double(dir[ky]) / double(dir[kz]);
    double const sz = 1.0 / double(dir[kz]);
    double a[3], b[3], c[3];
    auto shear = [&](float const p[3], double out[3]) {
        double const x = double(p[kx]) - double(origin[kx]);
        double const y = double(p[ky]) - double(origin[ky]);
        double const z = double(p[kz]) - double(origin[kz]);
        out[0] = x - sx * z;
        out[1] = y - sy * z;
        out[2] = z * sz;
    };
    shear(v0, a);
    shear(v1, b);
    shear(v2, c);
    double const u = _EdgeDet(c[0], c[1], b[0], b[1]);
    double const v = _EdgeDet(a[0], a[1], c[0], c[1]);
    double const w = _EdgeDet(b[0], b[1], a[0], a[1]);
    bool const anyNegative = u < 0.0 || v < 0.0 || w < 0.0;
    bool const anyPositive = u > 0.0 || v > 0.0 || w > 0.0;
    if (anyNegative && anyPositive) {
        return inf;
    }
    double const det = u + v + w;
    if (det == 0.0) {
        return inf;
    }
    double const t = (u * a[2] + v * b[2] + w * c[2]) / det;
    return std::isfinite(t) && t >= 0.0 ? float(t) : inf;
}

bool _RayAabb(float const origin[3], float const inv[3], float tMax,
              TonicScalpBvhNode const &node)
{
    float t0 = 0.0f;
    float t1 = tMax;
    float const mn[3] = {node.minX, node.minY, node.minZ};
    float const mx[3] = {node.maxX, node.maxY, node.maxZ};
    for (int a = 0; a < 3; ++a) {
        float lo = (mn[a] - origin[a]) * inv[a];
        float hi = (mx[a] - origin[a]) * inv[a];
        if (lo > hi) {
            std::swap(lo, hi);
        }
        t0 = std::max(t0, lo);
        t1 = std::min(t1, hi);
        if (t0 > t1) {
            return false;
        }
    }
    return true;
}

// Closest point on triangle v0/v1/v2 to p (Ericson §5.1.5). Returns the
// squared distance and writes the closest point.
float _ClosestOnTriangle(float const p[3], float const v0[3], float const v1[3],
                         float const v2[3], float out[3])
{
    float ab[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
    float ac[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
    float ap[3] = {p[0] - v0[0], p[1] - v0[1], p[2] - v0[2]};
    float const d1 = _Dot3(ab, ap);
    float const d2 = _Dot3(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        out[0] = v0[0];
        out[1] = v0[1];
        out[2] = v0[2];
    } else {
        float bp[3] = {p[0] - v1[0], p[1] - v1[1], p[2] - v1[2]};
        float const d3 = _Dot3(ab, bp);
        float const d4 = _Dot3(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) {
            out[0] = v1[0];
            out[1] = v1[1];
            out[2] = v1[2];
        } else {
            float const vc = d1 * d4 - d3 * d2;
            float cp[3] = {p[0] - v2[0], p[1] - v2[1], p[2] - v2[2]};
            float const d5 = _Dot3(ab, cp);
            float const d6 = _Dot3(ac, cp);
            if (d6 >= 0.0f && d5 <= d6) {
                out[0] = v2[0];
                out[1] = v2[1];
                out[2] = v2[2];
            } else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
                float const w = d1 / (d1 - d3);
                out[0] = v0[0] + w * ab[0];
                out[1] = v0[1] + w * ab[1];
                out[2] = v0[2] + w * ab[2];
            } else {
                float const vb = d5 * d2 - d1 * d6;
                if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
                    float const w = d2 / (d2 - d6);
                    out[0] = v0[0] + w * ac[0];
                    out[1] = v0[1] + w * ac[1];
                    out[2] = v0[2] + w * ac[2];
                } else {
                    float const va = d3 * d6 - d5 * d4;
                    if (va <= 0.0f && (d4 - d3) >= 0.0f &&
                        (d5 - d6) >= 0.0f) {
                        float const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                        out[0] = v1[0] + w * (v2[0] - v1[0]);
                        out[1] = v1[1] + w * (v2[1] - v1[1]);
                        out[2] = v1[2] + w * (v2[2] - v1[2]);
                    } else {
                        float const denom = 1.0f / (va + vb + vc);
                        float const v = vb * denom;
                        float const w = vc * denom;
                        out[0] = v0[0] + ab[0] * v + ac[0] * w;
                        out[1] = v0[1] + ab[1] * v + ac[1] * w;
                        out[2] = v0[2] + ab[2] * v + ac[2] * w;
                    }
                }
            }
        }
    }
    float dx = p[0] - out[0];
    float dy = p[1] - out[1];
    float dz = p[2] - out[2];
    return dx * dx + dy * dy + dz * dz;
}

// Morton helpers: 10 bits per axis over the centroid bbox.
uint32_t _Expand10(uint32_t v)
{
    v = (v * 0x00010001u) & 0xFF0000FFu;
    v = (v * 0x00000101u) & 0x0F00F00Fu;
    v = (v * 0x00000011u) & 0xC30C30C3u;
    v = (v * 0x00000005u) & 0x49249249u;
    return v;
}

uint32_t _Morton3(float x, float y, float z)
{
    auto q = [](float t) -> uint32_t {
        t = std::min(std::max(t, 0.0f), 1.0f);
        return std::min(uint32_t(t * 1024.0f), uint32_t(1023));
    };
    return (_Expand10(q(x)) << 2) | (_Expand10(q(y)) << 1) | _Expand10(q(z));
}

int _CommonPrefix(uint32_t const *codes, int n, int i, int j)
{
    if (j < 0 || j >= n) {
        return -1;
    }
    uint32_t const x = codes[i] ^ codes[j];
    if (x != 0) {
        int lz = 0;
        for (uint32_t m = 0x80000000u; (x & m) == 0; m >>= 1) {
            ++lz;
        }
        return lz;
    }
    // Identical codes: break the tie by index (Karras §3: δ(i,j) gains the
    // index suffix so the ordering stays total).
    return 32 + (i < j ? 1 : -1);
}

}  // namespace

bool TonicScalpFinalize(TonicScalpMesh *mesh, std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        if (mesh) {
            mesh->finalized = false;
        }
        return false;
    };
    if (!mesh) {
        return fail("TonicScalpFinalize: null mesh");
    }
    size_t const pointCount = mesh->points.size() / 3;
    if (mesh->points.size() % 3 != 0 || pointCount == 0) {
        return fail("TonicScalpFinalize: points must be non-empty xyz triples");
    }
    float minP[3] = {mesh->points[0], mesh->points[1], mesh->points[2]};
    float maxP[3] = {minP[0], minP[1], minP[2]};
    for (size_t i = 3; i < mesh->points.size(); i += 3) {
        for (int axis = 0; axis != 3; ++axis) {
            minP[axis] = std::min(minP[axis], mesh->points[i + axis]);
            maxP[axis] = std::max(maxP[axis], mesh->points[i + axis]);
        }
    }
    float const boundsDx = maxP[0] - minP[0];
    float const boundsDy = maxP[1] - minP[1];
    float const boundsDz = maxP[2] - minP[2];
    mesh->boundsDiagonal = std::sqrt(boundsDx * boundsDx +
                                     boundsDy * boundsDy +
                                     boundsDz * boundsDz);
    size_t const faceCount = mesh->faceVertexCounts.size();
    if (faceCount == 0) {
        return fail("TonicScalpFinalize: the scalp needs at least one face");
    }
    mesh->faceOffsets.assign(faceCount + 1, 0);
    for (size_t f = 0; f < faceCount; ++f) {
        if (mesh->faceVertexCounts[f] < 3) {
            return fail("TonicScalpFinalize: every face needs >= 3 vertices");
        }
        mesh->faceOffsets[f + 1] =
            mesh->faceOffsets[f] + mesh->faceVertexCounts[f];
    }
    if (size_t(mesh->faceOffsets.back()) != mesh->faceVertexIndices.size()) {
        return fail("TonicScalpFinalize: indices do not match the counts");
    }
    for (int v : mesh->faceVertexIndices) {
        if (v < 0 || size_t(v) >= pointCount) {
            return fail("TonicScalpFinalize: a face index is out of range");
        }
    }
    // A face subset names parent faces (plan/02 §2.20 rule 2). Repeats are
    // harmless (several subsets union, rule 4); a face the mesh does not
    // have is an authoring error, never a silent clamp.
    std::vector<int> &active = mesh->activeFaces;
    std::sort(active.begin(), active.end());
    active.erase(std::unique(active.begin(), active.end()), active.end());
    if (!active.empty() &&
        (active.front() < 0 || size_t(active.back()) >= faceCount)) {
        return fail("TonicScalpFinalize: a face subset index is out of range");
    }
    mesh->faceActive.assign(faceCount, active.empty() ? 1 : 0);
    for (int f : active) {
        mesh->faceActive[size_t(f)] = 1;
    }
    mesh->faceCentroids.assign(faceCount * 3, 0.0f);
    mesh->faceNormals.assign(faceCount * 3, 0.0f);
    mesh->faceAreas.assign(faceCount, 0.0f);
    mesh->faceMin.assign(faceCount * 3, 0.0f);
    mesh->faceMax.assign(faceCount * 3, 0.0f);
    double edgeSum = 0.0;
    size_t edgeCount = 0;
    for (size_t f = 0; f < faceCount; ++f) {
        int const nv = mesh->faceVertexCounts[f];
        int const off = mesh->faceOffsets[f];
        float n[3], c[3], area = 0.0f;
        _FaceFrame(*mesh, int(f), n, c, &area);
        mesh->faceCentroids[f * 3 + 0] = c[0];
        mesh->faceCentroids[f * 3 + 1] = c[1];
        mesh->faceCentroids[f * 3 + 2] = c[2];
        mesh->faceNormals[f * 3 + 0] = n[0];
        mesh->faceNormals[f * 3 + 1] = n[1];
        mesh->faceNormals[f * 3 + 2] = n[2];
        mesh->faceAreas[f] = area;
        float mn[3] = {std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity()};
        float mx[3] = {-mn[0], -mn[1], -mn[2]};
        for (int i = 0; i < nv; ++i) {
            float const *p0 =
                mesh->points.data() +
                size_t(mesh->faceVertexIndices[size_t(off + i)]) * 3;
            float const *p1 =
                mesh->points.data() +
                size_t(mesh->faceVertexIndices[size_t(off + (i + 1) % nv)]) * 3;
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], p0[a]);
                mx[a] = std::max(mx[a], p0[a]);
            }
            float dx = p1[0] - p0[0];
            float dy = p1[1] - p0[1];
            float dz = p1[2] - p0[2];
            edgeSum += std::sqrt(dx * dx + dy * dy + dz * dz);
            ++edgeCount;
        }
        for (int a = 0; a < 3; ++a) {
            mesh->faceMin[f * 3 + a] = mn[a];
            mesh->faceMax[f * 3 + a] = mx[a];
        }
    }
    mesh->meanEdgeLength =
        edgeCount ? float(edgeSum / double(edgeCount)) : 0.0f;
    {
        std::vector<float> areas = mesh->faceAreas;
        std::nth_element(areas.begin(), areas.begin() + areas.size() / 2,
                         areas.end());
        mesh->medianFaceArea = areas[areas.size() / 2];
    }
    // Dual graph through shared vertices (one pass over corners).
    mesh->faceNeighbours.assign(faceCount, {});
    std::unordered_map<int, std::vector<int>> vertFaces;
    vertFaces.reserve(mesh->faceVertexIndices.size() * 2);
    for (size_t f = 0; f < faceCount; ++f) {
        int const nv = mesh->faceVertexCounts[f];
        int const off = mesh->faceOffsets[f];
        for (int i = 0; i < nv; ++i) {
            vertFaces[mesh->faceVertexIndices[size_t(off + i)]].push_back(
                int(f));
        }
    }
    std::vector<char> seen(faceCount, 0);
    std::vector<int> touched;
    for (size_t f = 0; f < faceCount; ++f) {
        touched.clear();
        int const nv = mesh->faceVertexCounts[f];
        int const off = mesh->faceOffsets[f];
        for (int i = 0; i < nv; ++i) {
            for (int g : vertFaces[mesh->faceVertexIndices[size_t(off + i)]]) {
                if (size_t(g) != f && !seen[size_t(g)]) {
                    seen[size_t(g)] = 1;
                    touched.push_back(g);
                    mesh->faceNeighbours[f].push_back(g);
                }
            }
        }
        for (int g : touched) {
            seen[size_t(g)] = 0;
        }
        std::sort(mesh->faceNeighbours[f].begin(), mesh->faceNeighbours[f].end());
    }
    mesh->finalized = true;
    return true;
}

bool TonicScalpBvhBuild(TonicScalpMesh const &mesh, TonicScalpBvh *bvh,
                        std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!bvh) {
        return fail("TonicScalpBvhBuild: null bvh");
    }
    bvh->nodes.clear();
    bvh->order.clear();
    bvh->root = -1;
    bvh->valid = false;
    if (!mesh.finalized) {
        return fail("TonicScalpBvhBuild: mesh is not finalized");
    }
    // Leaves are the growth surface only: a face subset's inactive faces
    // never enter the tree, so no traversal (host or device) can hit them.
    if (mesh.activeFaces.empty()) {
        bvh->order.resize(mesh.faceVertexCounts.size());
        std::iota(bvh->order.begin(), bvh->order.end(), 0);
    } else {
        bvh->order = mesh.activeFaces;
    }
    int const n = int(bvh->order.size());
    if (n == 0) {
        bvh->valid = true;
        return true;
    }
    // Morton codes over the centroid bbox.
    float mn[3] = {std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::infinity()};
    float mx[3] = {-mn[0], -mn[1], -mn[2]};
    for (int f : bvh->order) {
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], mesh.faceCentroids[size_t(f) * 3 + a]);
            mx[a] = std::max(mx[a], mesh.faceCentroids[size_t(f) * 3 + a]);
        }
    }
    float span[3] = {std::max(mx[0] - mn[0], 1e-12f),
                     std::max(mx[1] - mn[1], 1e-12f),
                     std::max(mx[2] - mn[2], 1e-12f)};
    // Indexed by parent face id; only the active entries are read.
    std::vector<uint32_t> codes;
    codes.resize(mesh.faceVertexCounts.size());
    for (int f : bvh->order) {
        codes[size_t(f)] = _Morton3(
            (mesh.faceCentroids[size_t(f) * 3 + 0] - mn[0]) / span[0],
            (mesh.faceCentroids[size_t(f) * 3 + 1] - mn[1]) / span[1],
            (mesh.faceCentroids[size_t(f) * 3 + 2] - mn[2]) / span[2]);
    }
    std::stable_sort(bvh->order.begin(), bvh->order.end(),
                     [&](int a, int b) { return codes[size_t(a)] < codes[size_t(b)]; });
    std::vector<uint32_t> sorted;
    sorted.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        sorted[size_t(i)] = codes[size_t(bvh->order[size_t(i)])];
    }
    // Leaves first (indices 0..n-1), then interior nodes (Karras §4: one
    // interior node per split, found by the δ-range walk).
    bvh->nodes.resize(size_t(2 * n - 1));
    for (int i = 0; i < n; ++i) {
        int const f = bvh->order[size_t(i)];
        TonicScalpBvhNode &leaf = bvh->nodes[size_t(i)];
        leaf.minX = mesh.faceMin[size_t(f) * 3 + 0];
        leaf.minY = mesh.faceMin[size_t(f) * 3 + 1];
        leaf.minZ = mesh.faceMin[size_t(f) * 3 + 2];
        leaf.maxX = mesh.faceMax[size_t(f) * 3 + 0];
        leaf.maxY = mesh.faceMax[size_t(f) * 3 + 1];
        leaf.maxZ = mesh.faceMax[size_t(f) * 3 + 2];
        leaf.left = leaf.right = -1;
        leaf.faceBegin = i;
        leaf.faceEnd = i + 1;
    }
    if (n == 1) {
        bvh->root = 0;
        bvh->valid = true;
        return true;
    }
    // Interior node i covers a Morton range; its split is where δ is minimal.
    // Iterative Karras: for each interior slot, findRange + findSplit.
    std::vector<int> nodeFor(n - 1, -1);
    for (int i = 0; i < n - 1; ++i) {
        nodeFor[size_t(i)] = n + i;
    }
    auto findSplit = [&](int first, int last) {
        int const prefix = _CommonPrefix(sorted.data(), n, first, last);
        int split = first;
        int step = last - first;
        do {
            step = (step + 1) >> 1;
            int const next = split + step;
            if (next < last &&
                _CommonPrefix(sorted.data(), n, first, next) > prefix) {
                split = next;
            }
        } while (step > 1);
        return split;
    };
    // Build recursively over ranges; depth is O(log n) expected.
    std::function<int(int, int)> build = [&](int first, int last) -> int {
        if (first == last) {
            return first;  // leaf index
        }
        int const split = findSplit(first, last);
        int const left = build(first, split);
        int const right = build(split + 1, last);
        int const slot = nodeFor[size_t(split)];
        TonicScalpBvhNode &node = bvh->nodes[size_t(slot)];
        node.left = left;
        node.right = right;
        node.faceBegin = first;
        node.faceEnd = last + 1;
        TonicScalpBvhNode const &a = bvh->nodes[size_t(left)];
        TonicScalpBvhNode const &b = bvh->nodes[size_t(right)];
        node.minX = std::min(a.minX, b.minX);
        node.minY = std::min(a.minY, b.minY);
        node.minZ = std::min(a.minZ, b.minZ);
        node.maxX = std::max(a.maxX, b.maxX);
        node.maxY = std::max(a.maxY, b.maxY);
        node.maxZ = std::max(a.maxZ, b.maxZ);
        return slot;
    };
    bvh->root = build(0, n - 1);
    bvh->valid = true;
    return true;
}

bool TonicScalpBvhRefit(TonicScalpMesh const &mesh, TonicScalpBvh *bvh)
{
    if (!bvh || !bvh->valid || !mesh.finalized) {
        return false;
    }
    // One leaf per active face, in Morton order.
    int const n = int(bvh->order.size());
    if (n == 0 || bvh->nodes.empty()) {
        return true;
    }
    // Leaves 0..n-1, then interior nodes bottom-up (children always precede
    // parents: interior slot n+split with split < n-1... not guaranteed in
    // general, so refit by explicit post-order from the root).
    for (int i = 0; i < n; ++i) {
        int const f = bvh->order[size_t(i)];
        TonicScalpBvhNode &leaf = bvh->nodes[size_t(i)];
        // Recompute the face AABB from the current points (the mesh may have
        // deformed since finalize; counts/indices are unchanged).
        int const nv = mesh.faceVertexCounts[size_t(f)];
        int const off = mesh.faceOffsets[size_t(f)];
        float mn[3] = {std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity()};
        float mx[3] = {-mn[0], -mn[1], -mn[2]};
        for (int k = 0; k < nv; ++k) {
            float const *p =
                mesh.points.data() +
                size_t(mesh.faceVertexIndices[size_t(off + k)]) * 3;
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], p[a]);
                mx[a] = std::max(mx[a], p[a]);
            }
        }
        leaf.minX = mn[0];
        leaf.minY = mn[1];
        leaf.minZ = mn[2];
        leaf.maxX = mx[0];
        leaf.maxY = mx[1];
        leaf.maxZ = mx[2];
    }
    std::vector<int> stack;
    std::vector<int> post;
    stack.push_back(bvh->root);
    while (!stack.empty()) {
        int const node = stack.back();
        stack.pop_back();
        post.push_back(node);
        TonicScalpBvhNode const &nd = bvh->nodes[size_t(node)];
        if (nd.left >= 0) {
            stack.push_back(nd.left);
            stack.push_back(nd.right);
        }
    }
    for (auto it = post.rbegin(); it != post.rend(); ++it) {
        TonicScalpBvhNode &node = bvh->nodes[size_t(*it)];
        if (node.left < 0) {
            continue;
        }
        TonicScalpBvhNode const &a = bvh->nodes[size_t(node.left)];
        TonicScalpBvhNode const &b = bvh->nodes[size_t(node.right)];
        node.minX = std::min(a.minX, b.minX);
        node.minY = std::min(a.minY, b.minY);
        node.minZ = std::min(a.minZ, b.minZ);
        node.maxX = std::max(a.maxX, b.maxX);
        node.maxY = std::max(a.maxY, b.maxY);
        node.maxZ = std::max(a.maxZ, b.maxZ);
    }
    return true;
}

namespace {

// Fan-triangle raycast of one face; returns t or +inf.
float _RayFace(TonicScalpMesh const &mesh, int face, float const origin[3],
               float const dir[3])
{
    float const inf = std::numeric_limits<float>::infinity();
    int const nv = mesh.faceVertexCounts[size_t(face)];
    int const off = mesh.faceOffsets[size_t(face)];
    float const *pts = mesh.points.data();
    int const *indices = mesh.faceVertexIndices.data();
    float const *v0 = pts + size_t(indices[off]) * 3;
    float best = inf;
    for (int i = 1; i + 1 < nv; ++i) {
        float const *v1 = pts + size_t(indices[off + i]) * 3;
        float const *v2 = pts + size_t(indices[off + i + 1]) * 3;
        best = std::min(best, _RayTriangle(origin, dir, v0, v1, v2));
    }
    return best;
}

}  // namespace

TonicHit TonicRaycastCpu(TonicScalpMesh const &mesh, TonicScalpBvh const &bvh,
                         float const origin[3], float const dir[3])
{
    TonicHit miss;
    if (!mesh.finalized || !bvh.valid || bvh.root < 0 || !origin || !dir) {
        return miss;
    }
    float const dl = std::sqrt(_Dot3(dir, dir));
    if (!(dl > 0.0f) || !std::isfinite(dl)) {
        return miss;
    }
    float d[3] = {dir[0] / dl, dir[1] / dl, dir[2] / dl};
    float inv[3];
    for (int a = 0; a < 3; ++a) {
        inv[a] = d[a] != 0.0f ? 1.0f / d[a]
                              : std::numeric_limits<float>::infinity() *
                                    (d[a] >= 0.0f ? 1.0f : -1.0f);
    }
    float bestT = std::numeric_limits<float>::infinity();
    int bestFace = -1;
    // Explicit stack, near-first by construction (no ordering heuristic: the
    // tMax shrink keeps the walk exact, and LBVH depth is O(log n)).
    int stack[64];
    int top = 0;
    stack[top++] = bvh.root;
    while (top > 0) {
        int const node = stack[--top];
        TonicScalpBvhNode const &nd = bvh.nodes[size_t(node)];
        if (!_RayAabb(origin, inv, bestT, nd)) {
            continue;
        }
        if (nd.left < 0) {
            for (int i = nd.faceBegin; i < nd.faceEnd; ++i) {
                int const f = bvh.order[size_t(i)];
                float const t = _RayFace(mesh, f, origin, d);
                if (t < bestT) {
                    bestT = t;
                    bestFace = f;
                }
            }
        } else {
            if (top + 2 > 64) {
                continue;  // pathological depth: keep the best hit so far
            }
            stack[top++] = nd.left;
            stack[top++] = nd.right;
        }
    }
    if (bestFace < 0) {
        return miss;
    }
    TonicHit hit;
    hit.hit = true;
    hit.faceId = bestFace;
    hit.t = bestT;
    hit.px = origin[0] + d[0] * bestT;
    hit.py = origin[1] + d[1] * bestT;
    hit.pz = origin[2] + d[2] * bestT;
    hit.nx = mesh.faceNormals[size_t(bestFace) * 3 + 0];
    hit.ny = mesh.faceNormals[size_t(bestFace) * 3 + 1];
    hit.nz = mesh.faceNormals[size_t(bestFace) * 3 + 2];
    float u = 0.0f, v = 0.0f;
    int ptex = -1;
    if (TonicFaceCoordinate(mesh, bestFace, hit.px, hit.py, hit.pz, &u, &v,
                            &ptex)) {
        hit.u = u;
        hit.v = v;
        hit.ptexFaceId = ptex;
    }
    return hit;
}

TonicHit TonicClosestPointCpu(TonicScalpMesh const &mesh, float const p[3],
                              bool activeOnly)
{
    TonicHit miss;
    if (!mesh.finalized || !p) {
        return miss;
    }
    float const *pts = mesh.points.data();
    int const *indices = mesh.faceVertexIndices.data();
    int const faceCount = int(mesh.faceVertexCounts.size());
    float bestD2 = std::numeric_limits<float>::infinity();
    int bestFace = -1;
    float best[3] = {0.0f, 0.0f, 0.0f};
    float cand[3];
    for (int f = 0; f < faceCount; ++f) {
        if (activeOnly && !TonicScalpFaceActive(mesh, f)) {
            continue;
        }
        int const nv = mesh.faceVertexCounts[size_t(f)];
        int const off = mesh.faceOffsets[size_t(f)];
        // AABB pre-filter (squared distance to the box).
        float boxD2 = 0.0f;
        for (int a = 0; a < 3; ++a) {
            float const lo = mesh.faceMin[size_t(f) * 3 + a];
            float const hi = mesh.faceMax[size_t(f) * 3 + a];
            if (p[a] < lo) {
                float const dd = lo - p[a];
                boxD2 += dd * dd;
            } else if (p[a] > hi) {
                float const dd = p[a] - hi;
                boxD2 += dd * dd;
            }
        }
        if (boxD2 >= bestD2) {
            continue;
        }
        float const *v0 = pts + size_t(indices[off]) * 3;
        for (int i = 1; i + 1 < nv; ++i) {
            float const *v1 = pts + size_t(indices[off + i]) * 3;
            float const *v2 = pts + size_t(indices[off + i + 1]) * 3;
            float const d2 = _ClosestOnTriangle(p, v0, v1, v2, cand);
            if (d2 < bestD2) {
                bestD2 = d2;
                bestFace = f;
                best[0] = cand[0];
                best[1] = cand[1];
                best[2] = cand[2];
            }
        }
    }
    if (bestFace < 0) {
        return miss;
    }
    TonicHit hit;
    hit.hit = true;
    hit.faceId = bestFace;
    hit.t = std::sqrt(std::max(bestD2, 0.0f));
    hit.px = best[0];
    hit.py = best[1];
    hit.pz = best[2];
    hit.nx = mesh.faceNormals[size_t(bestFace) * 3 + 0];
    hit.ny = mesh.faceNormals[size_t(bestFace) * 3 + 1];
    hit.nz = mesh.faceNormals[size_t(bestFace) * 3 + 2];
    float u = 0.0f, v = 0.0f;
    int ptex = -1;
    if (TonicFaceCoordinate(mesh, bestFace, hit.px, hit.py, hit.pz, &u, &v,
                            &ptex)) {
        hit.u = u;
        hit.v = v;
        hit.ptexFaceId = ptex;
    }
    return hit;
}

TonicHit TonicMirrorXCpu(TonicScalpMesh const &mesh, float const p[3])
{
    if (!p) {
        return TonicHit();
    }
    float const mirrored[3] = {-p[0], p[1], p[2]};
    return TonicClosestPointCpu(mesh, mirrored);
}

namespace {

// Bilinear patch evaluation: p(u,v) over corners c0..c3.
void _Bilinear(float const c[12], float u, float v, float out[3])
{
    float const w0 = (1.0f - u) * (1.0f - v);
    float const w1 = u * (1.0f - v);
    float const w2 = u * v;
    float const w3 = (1.0f - u) * v;
    for (int a = 0; a < 3; ++a) {
        out[a] = w0 * c[a] + w1 * c[3 + a] + w2 * c[6 + a] + w3 * c[9 + a];
    }
}

// Inverse bilinear in the patch's tangent plane (Newell normal): 6 Newton
// iterations from (0.5, 0.5), then clamped. Matches the Ptex sampler's
// convention for quads (ptexMap.h), so node (u, v) addresses baked texels.
bool _InverseBilinear(float const c[12], float px, float py, float pz,
                      float *u, float *v)
{
    float e1[3] = {c[3] - c[0], c[4] - c[1], c[5] - c[2]};
    float e2[3] = {c[9] - c[0], c[10] - c[1], c[11] - c[2]};
    float n[3];
    _Cross3(e1, e2, n);
    float const nl = std::sqrt(_Dot3(n, n));
    if (!(nl > 0.0f)) {
        *u = *v = 0.0f;
        return false;
    }
    n[0] /= nl;
    n[1] /= nl;
    n[2] /= nl;
    // Tangent basis: longest edge from c0, crossed with the normal.
    float t1[3] = {e1[0], e1[1], e1[2]};
    float l1 = std::sqrt(_Dot3(t1, t1));
    float const l2 = std::sqrt(_Dot3(e2, e2));
    if (l2 > l1) {
        t1[0] = e2[0];
        t1[1] = e2[1];
        t1[2] = e2[2];
        l1 = l2;
    }
    if (!(l1 > 0.0f)) {
        *u = *v = 0.0f;
        return false;
    }
    t1[0] /= l1;
    t1[1] /= l1;
    t1[2] /= l1;
    float t2[3];
    _Cross3(n, t1, t2);
    float q0[2], q1[2], q2[2], q3[2], qp[2];
    float const *cs[4] = {c, c + 3, c + 6, c + 9};
    float *qs[4] = {q0, q1, q2, q3};
    for (int i = 0; i < 4; ++i) {
        float d[3] = {cs[i][0] - c[0], cs[i][1] - c[1], cs[i][2] - c[2]};
        qs[i][0] = _Dot3(d, t1);
        qs[i][1] = _Dot3(d, t2);
    }
    {
        float d[3] = {px - c[0], py - c[1], pz - c[2]};
        qp[0] = _Dot3(d, t1);
        qp[1] = _Dot3(d, t2);
    }
    float uu = 0.5f, vv = 0.5f;
    for (int it = 0; it < 6; ++it) {
        float const w0 = (1 - uu) * (1 - vv);
        float const w1 = uu * (1 - vv);
        float const w2 = uu * vv;
        float const w3 = (1 - uu) * vv;
        float fx = w0 * q0[0] + w1 * q1[0] + w2 * q2[0] + w3 * q3[0] - qp[0];
        float fy = w0 * q0[1] + w1 * q1[1] + w2 * q2[1] + w3 * q3[1] - qp[1];
        float j00 = (1 - vv) * (q1[0] - q0[0]) + vv * (q2[0] - q3[0]);
        float j01 = (1 - uu) * (q3[0] - q0[0]) + uu * (q2[0] - q1[0]);
        float j10 = (1 - vv) * (q1[1] - q0[1]) + vv * (q2[1] - q3[1]);
        float j11 = (1 - uu) * (q3[1] - q0[1]) + uu * (q2[1] - q1[1]);
        float const det = j00 * j11 - j01 * j10;
        if (std::fabs(det) < 1e-24f) {
            break;
        }
        uu -= (fx * j11 - fy * j01) / det;
        vv -= (j00 * fy - j10 * fx) / det;
    }
    *u = std::min(std::max(uu, 0.0f), 1.0f);
    *v = std::min(std::max(vv, 0.0f), 1.0f);
    return true;
}

// Sub-quad corners of corner k of an n-gon (the Ptex n-gon convention:
// corner k, edge-midpoint k/k+1, centroid, edge-midpoint k-1/k).
void _SubQuad(TonicScalpMesh const &mesh, int face, int k, float c[12])
{
    int const nv = mesh.faceVertexCounts[size_t(face)];
    int const off = mesh.faceOffsets[size_t(face)];
    int const *indices = mesh.faceVertexIndices.data();
    float const *pts = mesh.points.data();
    float const *pk = pts + size_t(indices[off + k]) * 3;
    float const *pn = pts + size_t(indices[off + (k + 1) % nv]) * 3;
    float const *pp = pts + size_t(indices[off + (k + nv - 1) % nv]) * 3;
    float const *cc = mesh.faceCentroids.data() + size_t(face) * 3;
    for (int a = 0; a < 3; ++a) {
        c[a] = pk[a];
        c[3 + a] = 0.5f * (pk[a] + pn[a]);
        c[6 + a] = cc[a];
        c[9 + a] = 0.5f * (pp[a] + pk[a]);
    }
}

}  // namespace

bool TonicFaceCoordinate(TonicScalpMesh const &mesh, int face, float px,
                         float py, float pz, float *u, float *v,
                         int *ptexFaceId)
{
    if (!mesh.finalized || face < 0 ||
        size_t(face) >= mesh.faceVertexCounts.size() || !u || !v) {
        return false;
    }
    int const nv = mesh.faceVertexCounts[size_t(face)];
    int const off = mesh.faceOffsets[size_t(face)];
    int const *indices = mesh.faceVertexIndices.data();
    float const *pts = mesh.points.data();
    if (nv == 4) {
        float c[12];
        for (int i = 0; i < 4; ++i) {
            float const *p = pts + size_t(indices[off + i]) * 3;
            c[i * 3 + 0] = p[0];
            c[i * 3 + 1] = p[1];
            c[i * 3 + 2] = p[2];
        }
        if (!_InverseBilinear(c, px, py, pz, u, v)) {
            return false;
        }
        if (ptexFaceId) {
            *ptexFaceId = face;  // quads: one Ptex id per coarse face
        }
        return true;
    }
    // N-gons: the winning sub-face is the one whose bilinear re-evaluation at
    // the inverted (fu, fv) lands closest to p.
    float bestD2 = std::numeric_limits<float>::infinity();
    int bestK = 0;
    float bestFu = 0.0f, bestFv = 0.0f;
    for (int k = 0; k < nv; ++k) {
        float c[12];
        _SubQuad(mesh, face, k, c);
        float fu = 0.0f, fv = 0.0f;
        if (!_InverseBilinear(c, px, py, pz, &fu, &fv)) {
            continue;
        }
        float q[3];
        _Bilinear(c, fu, fv, q);
        float dx = q[0] - px;
        float dy = q[1] - py;
        float dz = q[2] - pz;
        float const d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            bestK = k;
            bestFu = fu;
            bestFv = fv;
        }
    }
    *u = (float(bestK) + bestFu) / float(nv);
    *v = bestFv;
    if (ptexFaceId) {
        // Ptex quad-mesh convention: quads before this face hold one id,
        // n-gons before it hold one id per corner (UsdGenPtexFirstFaceIds).
        int first = 0;
        for (int f = 0; f < face; ++f) {
            int const m = mesh.faceVertexCounts[size_t(f)];
            first += (m == 4) ? 1 : m;
        }
        *ptexFaceId = first + bestK;
    }
    return true;
}

bool TonicFacePosition(TonicScalpMesh const &mesh, int face, float u, float v,
                       float *px, float *py, float *pz)
{
    if (!mesh.finalized || face < 0 ||
        size_t(face) >= mesh.faceVertexCounts.size() || !px || !py || !pz) {
        return false;
    }
    int const nv = mesh.faceVertexCounts[size_t(face)];
    int const off = mesh.faceOffsets[size_t(face)];
    int const *indices = mesh.faceVertexIndices.data();
    float const *pts = mesh.points.data();
    u = std::min(std::max(u, 0.0f), 1.0f);
    v = std::min(std::max(v, 0.0f), 1.0f);
    if (nv == 4) {
        float c[12];
        for (int i = 0; i < 4; ++i) {
            float const *p = pts + size_t(indices[off + i]) * 3;
            c[i * 3 + 0] = p[0];
            c[i * 3 + 1] = p[1];
            c[i * 3 + 2] = p[2];
        }
        float out[3];
        _Bilinear(c, u, v, out);
        *px = out[0];
        *py = out[1];
        *pz = out[2];
        return true;
    }
    float const scaled = std::min(u * float(nv), float(nv) - 1e-6f);
    int const k = int(scaled);
    float const fu = scaled - float(k);
    float c[12];
    _SubQuad(mesh, face, k, c);
    float out[3];
    _Bilinear(c, fu, v, out);
    *px = out[0];
    *py = out[1];
    *pz = out[2];
    return true;
}

}  // namespace usdGenTonic
