// usdGenTonic — the scalp graph implementation (G1).
#include "usdGenTonic/tonicGraph.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <utility>

namespace usdGenTonic {

std::vector<float> TonicTraceEdgeCpu(TonicScalpMesh const &mesh,
                                     float const a[3], float const b[3],
                                     int maxSamples)
{
    std::vector<float> empty;
    if (!mesh.finalized || !a || !b || maxSamples < 2) {
        return empty;
    }
    float dx = b[0] - a[0];
    float dy = b[1] - a[1];
    float dz = b[2] - a[2];
    float const dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    float const h = mesh.meanEdgeLength > 0.0f ? mesh.meanEdgeLength : dist;
    int n = h > 0.0f ? int(std::ceil(dist / h)) + 1 : 2;
    n = std::min(std::max(n, 2), maxSamples);
    std::vector<float> poly;
    poly.reserve(size_t(n) * 3);
    for (int i = 0; i < n; ++i) {
        float const t = n == 1 ? 0.0f : float(i) / float(n - 1);
        float const q[3] = {a[0] + dx * t, a[1] + dy * t, a[2] + dz * t};
        TonicHit const hit = TonicClosestPointCpu(mesh, q);
        if (!hit.hit) {
            return empty;
        }
        poly.push_back(hit.px);
        poly.push_back(hit.py);
        poly.push_back(hit.pz);
    }
    // Pin the ends exactly (closest-point is already exact on the surface,
    // but the endpoints must equal the node positions bit-for-bit so shared
    // boundaries join without cracks).
    poly[0] = a[0];
    poly[1] = a[1];
    poly[2] = a[2];
    poly[size_t(n - 1) * 3 + 0] = b[0];
    poly[size_t(n - 1) * 3 + 1] = b[1];
    poly[size_t(n - 1) * 3 + 2] = b[2];
    return poly;
}

void TonicRegionColor(int regionId, float rgb[3])
{
    // Golden-ratio hue ramp at fixed saturation/value, HSV -> RGB.
    float const h = std::fmod(float(regionId) * 0.61803398875f, 1.0f);
    float const s = 0.65f;
    float const v = 0.95f;
    float const c = v * s;
    float const hh = h * 6.0f;
    float const x = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    int const sector = int(hh) % 6;
    if (sector == 0) {
        r = c;
        g = x;
    } else if (sector == 1) {
        r = x;
        g = c;
    } else if (sector == 2) {
        g = c;
        b = x;
    } else if (sector == 3) {
        g = x;
        b = c;
    } else if (sector == 4) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    float const m = v - c;
    rgb[0] = r + m;
    rgb[1] = g + m;
    rgb[2] = b + m;
}

namespace {

float _Dist3(float const a[3], float const b[3])
{
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float _DistToPolyline(float const p[3], std::vector<float> const &poly)
{
    float best = std::numeric_limits<float>::infinity();
    size_t const n = poly.size() / 3;
    for (size_t i = 0; i + 1 < n; ++i) {
        float const *a = &poly[i * 3];
        float const *b = &poly[(i + 1) * 3];
        float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float const len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float t = 0.0f;
        if (len2 > 0.0f) {
            t = ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] +
                 (p[2] - a[2]) * ab[2]) /
                len2;
            t = std::min(std::max(t, 0.0f), 1.0f);
        }
        float dx = p[0] - (a[0] + ab[0] * t);
        float dy = p[1] - (a[1] + ab[1] * t);
        float dz = p[2] - (a[2] + ab[2] * t);
        best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return best;
}

// Union-find over region ids (linkedRegions).
struct _UnionFind {
    std::vector<int> parent;
    int Find(int x)
    {
        if (parent[size_t(x)] != x) {
            parent[size_t(x)] = Find(parent[size_t(x)]);
        }
        return parent[size_t(x)];
    }
    void Unite(int a, int b)
    {
        a = Find(a);
        b = Find(b);
        if (a != b) {
            parent[size_t(std::max(a, b))] = std::min(a, b);
        }
    }
};

}  // namespace

int TonicScalpGraph::NodeCount() const
{
    int n = 0;
    for (auto const &nd : _nodes) {
        n += nd.alive ? 1 : 0;
    }
    return n;
}

int TonicScalpGraph::EdgeCount() const
{
    int n = 0;
    for (auto const &e : _edges) {
        n += e.alive ? 1 : 0;
    }
    return n;
}

TonicGraphNode const *TonicScalpGraph::FindNode(int id) const
{
    for (auto const &nd : _nodes) {
        if (nd.id == id && nd.alive) {
            return &nd;
        }
    }
    return nullptr;
}

TonicGraphEdge const *TonicScalpGraph::FindEdge(int id) const
{
    for (auto const &e : _edges) {
        if (e.id == id && e.alive) {
            return &e;
        }
    }
    return nullptr;
}

TonicGraphNode *TonicScalpGraph::_MutableNode(int id)
{
    for (auto &nd : _nodes) {
        if (nd.id == id && nd.alive) {
            return &nd;
        }
    }
    return nullptr;
}

TonicGraphEdge *TonicScalpGraph::_MutableEdge(int id)
{
    for (auto &e : _edges) {
        if (e.id == id && e.alive) {
            return &e;
        }
    }
    return nullptr;
}

int TonicScalpGraph::InterpId(int regionId) const
{
    if (regionId < 0 || regionId >= int(_regions.size())) {
        return -1;
    }
    _UnionFind uf;
    uf.parent.resize(_regions.size());
    std::iota(uf.parent.begin(), uf.parent.end(), 0);
    for (auto const &pr : _linked) {
        if (pr.first >= 0 && pr.first < int(_regions.size()) &&
            pr.second >= 0 && pr.second < int(_regions.size())) {
            uf.Unite(pr.first, pr.second);
        }
    }
    return uf.Find(regionId);
}

int TonicScalpGraph::Valence(int nodeId) const
{
    int v = 0;
    for (auto const &e : _edges) {
        if (e.alive && (e.a == nodeId || e.b == nodeId)) {
            ++v;
        }
    }
    return v;
}

std::vector<int> TonicScalpGraph::NodeRegions(int nodeId) const
{
    std::vector<int> out;
    for (auto const &r : _regions) {
        if (std::find(r.loop.begin(), r.loop.end(), nodeId) != r.loop.end()) {
            out.push_back(r.id);
        }
    }
    return out;
}

std::vector<int> TonicScalpGraph::EdgeRegions(int edgeId) const
{
    std::vector<int> out;
    TonicGraphEdge const *e = FindEdge(edgeId);
    if (!e) {
        return out;
    }
    for (auto const &r : _regions) {
        size_t const n = r.loop.size();
        for (size_t i = 0; i < n; ++i) {
            int const a = r.loop[i];
            int const b = r.loop[(i + 1) % n];
            if ((a == e->a && b == e->b) || (a == e->b && b == e->a)) {
                out.push_back(r.id);
                break;
            }
        }
    }
    return out;
}

bool TonicScalpGraph::IsWatertight() const
{
    // Every alive edge must bound >= 1 region; an edge claimed twice must be
    // claimed by two DISTINCT regions (the shared boundary appears in both
    // loops — plan/17 §2.2's polygon-style encoding).
    for (auto const &e : _edges) {
        if (!e.alive) {
            continue;
        }
        std::vector<int> rs = EdgeRegions(e.id);
        std::sort(rs.begin(), rs.end());
        rs.erase(std::unique(rs.begin(), rs.end()), rs.end());
        if (rs.empty() || rs.size() > 2) {
            return false;
        }
    }
    return true;
}

std::vector<std::vector<int>> TonicScalpGraph::CoincidentSets(float eps) const
{
    std::vector<std::vector<int>> sets;
    std::vector<char> used(_nodes.size(), 0);
    for (size_t i = 0; i < _nodes.size(); ++i) {
        if (!_nodes[i].alive || used[i]) {
            continue;
        }
        std::vector<int> group;
        group.push_back(_nodes[i].id);
        used[i] = 1;
        for (size_t j = i + 1; j < _nodes.size(); ++j) {
            if (!_nodes[j].alive || used[j]) {
                continue;
            }
            if (_Dist3(_nodes[i].p, _nodes[j].p) <= eps) {
                group.push_back(_nodes[j].id);
                used[j] = 1;
            }
        }
        if (group.size() > 1) {
            sets.push_back(group);
        }
    }
    return sets;
}

int TonicScalpGraph::AddNode(TonicHit const &hit)
{
    if (!hit.hit) {
        _diagnostic = "TonicScalpGraph::AddNode: miss is not a node";
        return -1;
    }
    TonicGraphNode nd;
    nd.id = _NextNodeId();
    nd.faceId = hit.faceId;
    nd.u = hit.u;
    nd.v = hit.v;
    nd.p[0] = hit.px;
    nd.p[1] = hit.py;
    nd.p[2] = hit.pz;
    nd.n[0] = hit.nx;
    nd.n[1] = hit.ny;
    nd.n[2] = hit.nz;
    _nodes.push_back(nd);
    return nd.id;
}

bool TonicScalpGraph::MoveNode(TonicScalpMesh const &mesh, int nodeId,
                               TonicHit const &hit)
{
    TonicGraphNode *nd = _MutableNode(nodeId);
    if (!nd || !hit.hit) {
        _diagnostic = "TonicScalpGraph::MoveNode: bad node or miss";
        return false;
    }
    nd->faceId = hit.faceId;
    nd->u = hit.u;
    nd->v = hit.v;
    nd->p[0] = hit.px;
    nd->p[1] = hit.py;
    nd->p[2] = hit.pz;
    nd->n[0] = hit.nx;
    nd->n[1] = hit.ny;
    nd->n[2] = hit.nz;
    _RetraceIncident(mesh, nodeId);
    return ExtractRegions(mesh);
}

bool
TonicScalpGraph::MoveNodes(TonicScalpMesh const &mesh,
                           std::vector<int> const &nodeIds,
                           std::vector<TonicHit> const &hits)
{
    if (nodeIds.empty() || nodeIds.size() != hits.size()) {
        _diagnostic = "TonicScalpGraph::MoveNodes: mismatched targets";
        return false;
    }
    std::set<int> moved;
    for (size_t i = 0; i < nodeIds.size(); ++i) {
        if (!hits[i].hit || !_MutableNode(nodeIds[i]) ||
            !moved.insert(nodeIds[i]).second) {
            _diagnostic = "TonicScalpGraph::MoveNodes: bad or duplicate node";
            return false;
        }
    }
    auto targetPoint = [&](int nodeId, float out[3]) {
        for (size_t i = 0; i < nodeIds.size(); ++i) {
            if (nodeIds[i] == nodeId) {
                out[0] = hits[i].px;
                out[1] = hits[i].py;
                out[2] = hits[i].pz;
                return true;
            }
        }
        TonicGraphNode const *node = FindNode(nodeId);
        if (!node) {
            return false;
        }
        out[0] = node->p[0];
        out[1] = node->p[1];
        out[2] = node->p[2];
        return true;
    };
    for (TonicGraphEdge const &edge : _edges) {
        if (!edge.alive) {
            continue;
        }
        float a[3], b[3];
        if (!targetPoint(edge.a, a) || !targetPoint(edge.b, b)) {
            _diagnostic = "TonicScalpGraph::MoveNodes: dead edge endpoint";
            return false;
        }
        float const dx = a[0] - b[0];
        float const dy = a[1] - b[1];
        float const dz = a[2] - b[2];
        if (dx * dx + dy * dy + dz * dz <= 1.0e-12f) {
            _diagnostic = "TonicScalpGraph::MoveNodes: collapsed edge";
            return false;
        }
    }
    // Keep this low-level operation atomic too: callers at the model layer
    // take an undo snapshot, while direct graph users must never observe one
    // endpoint of an edge moving when its mate cannot be retraced.
    TonicScalpGraph const before = *this;
    auto rollback = [&](char const *reason) {
        *this = before;
        _diagnostic = reason;
        return false;
    };
    for (size_t i = 0; i < nodeIds.size(); ++i) {
        TonicGraphNode *node = _MutableNode(nodeIds[i]);
        TonicHit const &hit = hits[i];
        node->faceId = hit.faceId;
        node->u = hit.u;
        node->v = hit.v;
        node->p[0] = hit.px;
        node->p[1] = hit.py;
        node->p[2] = hit.pz;
        node->n[0] = hit.nx;
        node->n[1] = hit.ny;
        node->n[2] = hit.nz;
    }
    // Re-trace each affected edge once after every endpoint has reached its
    // sample. This avoids a visible half-edge and an intermediate region
    // extraction while dragging a whole edge.
    for (TonicGraphEdge &edge : _edges) {
        if (!edge.alive ||
            (!moved.count(edge.a) && !moved.count(edge.b))) {
            continue;
        }
        TonicGraphNode const *a = FindNode(edge.a);
        TonicGraphNode const *b = FindNode(edge.b);
        if (!a || !b) {
            return rollback("TonicScalpGraph::MoveNodes: dead edge endpoint");
        }
        std::vector<float> poly = TonicTraceEdgeCpu(mesh, a->p, b->p);
        if (poly.size() < 6) {
            return rollback("TonicScalpGraph::MoveNodes: K2 trace failed");
        }
        edge.polyline = std::move(poly);
    }
    if (!ExtractRegions(mesh)) {
        return rollback("TonicScalpGraph::MoveNodes: region extraction failed");
    }
    return true;
}

int TonicScalpGraph::Connect(TonicScalpMesh const &mesh, int a, int b,
                             bool extract)
{
    TonicGraphNode const *na = FindNode(a);
    TonicGraphNode const *nb = FindNode(b);
    if (!na || !nb || a == b) {
        _diagnostic = "TonicScalpGraph::Connect: bad or equal nodes";
        return -1;
    }
    for (auto const &e : _edges) {
        if (e.alive &&
            ((e.a == a && e.b == b) || (e.a == b && e.b == a))) {
            _diagnostic = "TonicScalpGraph::Connect: already connected";
            return -1;
        }
    }
    std::vector<float> poly = TonicTraceEdgeCpu(mesh, na->p, nb->p);
    if (poly.size() < 6) {
        _diagnostic = "TonicScalpGraph::Connect: K2 trace failed";
        return -1;
    }
    TonicGraphEdge e;
    e.id = _NextEdgeId();
    e.a = a;
    e.b = b;
    e.polyline = std::move(poly);
    _edges.push_back(e);
    if (extract && !ExtractRegions(mesh)) {
        return -1;
    }
    return e.id;
}

int TonicScalpGraph::SplitEdge(TonicScalpMesh const &mesh, int edgeId,
                               TonicHit const &hit)
{
    TonicGraphEdge *e = _MutableEdge(edgeId);
    if (!e || !hit.hit) {
        _diagnostic = "TonicScalpGraph::SplitEdge: bad edge or miss";
        return -1;
    }
    int const a = e->a;
    int const b = e->b;
    e->alive = false;
    int const mid = AddNode(hit);
    if (mid < 0) {
        return -1;
    }
    if (Connect(mesh, a, mid) < 0 || Connect(mesh, mid, b) < 0) {
        _diagnostic = "TonicScalpGraph::SplitEdge: re-connect failed";
        return -1;
    }
    return mid;
}

void TonicScalpGraph::_DropDegenerateEdges()
{
    // Degenerate (a == b) edges die; duplicate pairs collapse to one.
    std::set<std::pair<int, int>> seen;
    for (auto &e : _edges) {
        if (!e.alive) {
            continue;
        }
        if (e.a == e.b) {
            e.alive = false;
            continue;
        }
        std::pair<int, int> key = std::minmax(e.a, e.b);
        if (!seen.insert(key).second) {
            e.alive = false;
        }
    }
}

void TonicScalpGraph::_RetraceIncident(TonicScalpMesh const &mesh, int nodeId)
{
    TonicGraphNode const *nd = FindNode(nodeId);
    if (!nd) {
        return;
    }
    for (auto &e : _edges) {
        if (!e.alive || (e.a != nodeId && e.b != nodeId)) {
            continue;
        }
        TonicGraphNode const *o =
            FindNode(e.a == nodeId ? e.b : e.a);
        if (!o) {
            e.alive = false;
            continue;
        }
        float const *pa = (e.a == nodeId) ? nd->p : o->p;
        float const *pb = (e.a == nodeId) ? o->p : nd->p;
        std::vector<float> poly = TonicTraceEdgeCpu(mesh, pa, pb);
        if (poly.size() >= 6) {
            e.polyline = std::move(poly);
        }
    }
}

bool TonicScalpGraph::Weld(TonicScalpMesh const &mesh, int keep, int drop)
{
    if (keep == drop || !FindNode(keep) || !FindNode(drop)) {
        _diagnostic = "TonicScalpGraph::Weld: bad or equal nodes";
        return false;
    }
    TonicGraphNode *dn = _MutableNode(drop);
    dn->alive = false;
    for (auto &e : _edges) {
        if (!e.alive) {
            continue;
        }
        if (e.a == drop) {
            e.a = keep;
        }
        if (e.b == drop) {
            e.b = keep;
        }
    }
    _DropDegenerateEdges();
    _RetraceIncident(mesh, keep);
    return ExtractRegions(mesh);
}

int TonicScalpGraph::WeldAllWithinRadius(TonicScalpMesh const &mesh,
                                         float radius)
{
    int welds = 0;
    for (;;) {
        // Greedy closest pair; graph scale keeps this O(n^2)-trivial.
        float best = radius;
        int bk = -1, bd = -1;
        for (auto const &a : _nodes) {
            if (!a.alive) {
                continue;
            }
            for (auto const &b : _nodes) {
                if (!b.alive || b.id <= a.id) {
                    continue;
                }
                float const d = _Dist3(a.p, b.p);
                if (d < best) {
                    best = d;
                    bk = a.id;
                    bd = b.id;
                }
            }
        }
        if (bk < 0) {
            break;
        }
        if (!Weld(mesh, bk, bd)) {
            break;
        }
        ++welds;
    }
    return welds;
}

std::vector<int> TonicScalpGraph::Unweld(TonicScalpMesh const &mesh,
                                            int nodeId)
{
    std::vector<int> created;
    TonicGraphNode const *nd = FindNode(nodeId);
    if (!nd) {
        _diagnostic = "TonicScalpGraph::Unweld: unknown node";
        return created;
    }
    std::vector<int> regions = NodeRegions(nodeId);
    if (regions.size() < 2) {
        return created;  // nothing shared: unweld is a no-op
    }
    // One coincident copy per additional region; incident edges distribute by
    // their first region (edges with no region stay on the original).
    std::vector<int> copies;
    copies.push_back(nodeId);
    for (size_t i = 1; i < regions.size(); ++i) {
        TonicGraphNode dup = *nd;
        dup.id = _NextNodeId();
        copies.push_back(dup.id);
        _nodes.push_back(dup);
        created.push_back(dup.id);
    }
    for (auto &e : _edges) {
        if (!e.alive || (e.a != nodeId && e.b != nodeId)) {
            continue;
        }
        std::vector<int> ers = EdgeRegions(e.id);
        int slot = 0;
        if (!ers.empty()) {
            auto it = std::find(regions.begin(), regions.end(), ers[0]);
            if (it != regions.end()) {
                slot = int(it - regions.begin());
            }
        }
        if (e.a == nodeId) {
            e.a = copies[size_t(slot)];
        }
        if (e.b == nodeId) {
            e.b = copies[size_t(slot)];
        }
    }
    _DropDegenerateEdges();
    ExtractRegions(mesh);
    return created;
}

bool TonicScalpGraph::DeleteEdge(TonicScalpMesh const &mesh, int edgeId)
{
    TonicGraphEdge *e = _MutableEdge(edgeId);
    if (!e) {
        _diagnostic = "TonicScalpGraph::DeleteEdge: unknown edge";
        return false;
    }
    e->alive = false;
    return ExtractRegions(mesh);
}

bool TonicScalpGraph::DeleteNode(TonicScalpMesh const &mesh, int nodeId)
{
    TonicGraphNode *nd = _MutableNode(nodeId);
    if (!nd) {
        _diagnostic = "TonicScalpGraph::DeleteNode: unknown node";
        return false;
    }
    std::vector<int> neighbours;
    for (auto &e : _edges) {
        if (!e.alive) {
            continue;
        }
        if (e.a == nodeId) {
            neighbours.push_back(e.b);
            e.alive = false;
        } else if (e.b == nodeId) {
            neighbours.push_back(e.a);
            e.alive = false;
        }
    }
    nd->alive = false;
    // Valence-2 re-links the loop so a region survives a node deletion.
    if (neighbours.size() == 2 && neighbours[0] != neighbours[1] &&
        FindNode(neighbours[0]) && FindNode(neighbours[1])) {
        Connect(mesh, neighbours[0], neighbours[1]);
    }
    return ExtractRegions(mesh);
}

int TonicScalpGraph::SnapNode(float const p[3], float radius) const
{
    if (!p) {
        return -1;
    }
    int best = -1;
    float bestD = radius;
    for (auto const &nd : _nodes) {
        if (!nd.alive) {
            continue;
        }
        float const d = _Dist3(p, nd.p);
        if (d <= bestD) {
            bestD = d;
            best = nd.id;
        }
    }
    return best;
}

int TonicScalpGraph::SnapEdge(float const p[3], float radius) const
{
    if (!p) {
        return -1;
    }
    int best = -1;
    float bestD = radius;
    for (auto const &e : _edges) {
        if (!e.alive) {
            continue;
        }
        float const d = _DistToPolyline(p, e.polyline);
        if (d <= bestD) {
            bestD = d;
            best = e.id;
        }
    }
    return best;
}

bool TonicScalpGraph::LinkRegions(int r0, int r1)
{
    if (r0 == r1 || r0 < 0 || r1 < 0 || r0 >= int(_regions.size()) ||
        r1 >= int(_regions.size())) {
        _diagnostic = "TonicScalpGraph::LinkRegions: bad region ids";
        return false;
    }
    std::pair<int, int> key = std::minmax(r0, r1);
    if (std::find(_linked.begin(), _linked.end(), key) != _linked.end()) {
        return true;
    }
    _linked.push_back(key);
    for (auto &r : _regions) {
        r.interpId = InterpId(r.id);
    }
    return true;
}

bool TonicScalpGraph::UnlinkRegions(int r0, int r1)
{
    std::pair<int, int> key = std::minmax(r0, r1);
    auto it = std::find(_linked.begin(), _linked.end(), key);
    if (it == _linked.end()) {
        return false;
    }
    _linked.erase(it);
    for (auto &r : _regions) {
        r.interpId = InterpId(r.id);
    }
    return true;
}

void TonicScalpGraph::ClearLinks()
{
    _linked.clear();
    for (auto &r : _regions) {
        r.interpId = r.id;
    }
}

std::vector<std::pair<int, int>> TonicScalpGraph::MirrorX(
    TonicScalpMesh const &mesh)
{
    std::vector<std::pair<int, int>> mapping;
    if (!mesh.finalized) {
        _diagnostic = "TonicScalpGraph::MirrorX: mesh is not finalized";
        return mapping;
    }
    // Snapshot the alive set first: appended nodes must not mirror themselves.
    std::vector<TonicGraphNode> src;
    std::vector<std::pair<int, int>> srcEdges;
    for (auto const &nd : _nodes) {
        if (nd.alive) {
            src.push_back(nd);
        }
    }
    for (auto const &e : _edges) {
        if (e.alive) {
            srcEdges.emplace_back(e.a, e.b);
        }
    }
    std::map<int, int> twin;
    for (auto const &nd : src) {
        TonicHit const hit = TonicMirrorXCpu(mesh, nd.p);
        if (!hit.hit) {
            continue;
        }
        int const nid = AddNode(hit);
        if (nid < 0) {
            continue;
        }
        twin[nd.id] = nid;
        mapping.emplace_back(nd.id, nid);
    }
    for (auto const &pr : srcEdges) {
        auto ia = twin.find(pr.first);
        auto ib = twin.find(pr.second);
        if (ia == twin.end() || ib == twin.end()) {
            continue;
        }
        Connect(mesh, ia->second, ib->second);
    }
    ExtractRegions(mesh);
    return mapping;
}

bool TonicScalpGraph::ExtractRegions(TonicScalpMesh const &mesh)
{
    _regions.clear();
    if (!mesh.finalized) {
        _diagnostic = "TonicScalpGraph::ExtractRegions: mesh not finalized";
        return false;
    }
    // Incident-edge table over alive nodes.
    std::map<int, std::vector<int>> outgoing;  // node -> neighbour ids
    std::map<std::pair<int, int>, int> edgeOf;  // directed edge -> edge id
    for (auto const &e : _edges) {
        if (!e.alive || !FindNode(e.a) || !FindNode(e.b) || e.a == e.b) {
            continue;
        }
        outgoing[e.a].push_back(e.b);
        outgoing[e.b].push_back(e.a);
        edgeOf[{e.a, e.b}] = e.id;
        edgeOf[{e.b, e.a}] = e.id;
    }
    if (outgoing.empty()) {
        return true;  // no edges: no regions, still a valid graph
    }
    // Angular sort of each node's outgoing edges in its tangent chart
    // (plan/17 G1: planar face extraction). Basis: node normal + arbitrary
    // tangent; angles are CCW around the normal.
    std::map<int, std::vector<int>> ordered;
    for (auto const &kv : outgoing) {
        TonicGraphNode const *nd = FindNode(kv.first);
        float n[3] = {nd->n[0], nd->n[1], nd->n[2]};
        float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(nl > 0.0f)) {
            n[0] = 0.0f;
            n[1] = 1.0f;
            n[2] = 0.0f;
            nl = 1.0f;
        }
        n[0] /= nl;
        n[1] /= nl;
        n[2] /= nl;
        float ref[3] = {1.0f, 0.0f, 0.0f};
        if (std::fabs(n[0]) > 0.9f) {
            ref[0] = 0.0f;
            ref[1] = 0.0f;
            ref[2] = 1.0f;
        }
        float t1[3] = {ref[0] - n[0] * (ref[0] * n[0] + ref[1] * n[1] +
                                        ref[2] * n[2]),
                       ref[1] - n[1] * (ref[0] * n[0] + ref[1] * n[1] +
                                        ref[2] * n[2]),
                       ref[2] - n[2] * (ref[0] * n[0] + ref[1] * n[1] +
                                        ref[2] * n[2])};
        float l1 = std::sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
        if (!(l1 > 0.0f)) {
            t1[0] = 1.0f;
            t1[1] = 0.0f;
            t1[2] = 0.0f;
            l1 = 1.0f;
        }
        t1[0] /= l1;
        t1[1] /= l1;
        t1[2] /= l1;
        float t2[3] = {n[1] * t1[2] - n[2] * t1[1],
                       n[2] * t1[0] - n[0] * t1[2],
                       n[0] * t1[1] - n[1] * t1[0]};
        std::vector<std::pair<float, int>> scored;
        for (int nb : kv.second) {
            TonicGraphNode const *on = FindNode(nb);
            float d[3] = {on->p[0] - nd->p[0], on->p[1] - nd->p[1],
                          on->p[2] - nd->p[2]};
            float const x = d[0] * t1[0] + d[1] * t1[1] + d[2] * t1[2];
            float const y = d[0] * t2[0] + d[1] * t2[1] + d[2] * t2[2];
            scored.emplace_back(std::atan2(y, x), nb);
        }
        std::sort(scored.begin(), scored.end());
        for (auto const &pr : scored) {
            ordered[kv.first].push_back(pr.second);
        }
    }
    // Half-edge walk: at node b arriving from a, leave along the edge
    // previous to (b -> a) in CCW order (next in CW order), so every face
    // keeps to the left. Each directed edge belongs to exactly one face.
    std::set<std::pair<int, int>> used;
    std::vector<std::vector<int>> faces;
    for (auto const &kv : edgeOf) {
        if (!used.insert(kv.first).second) {
            continue;
        }
        std::vector<int> face;
        int a = kv.first.first;
        int b = kv.first.second;
        for (;;) {
            face.push_back(a);
            if (face.size() > outgoing.size() * 4 + 8) {
                face.clear();  // non-planar input: abandon this walk
                break;
            }
            std::vector<int> const &ring = ordered[b];
            auto it = std::find(ring.begin(), ring.end(), a);
            if (it == ring.end()) {
                face.clear();
                break;
            }
            size_t const idx = size_t(it - ring.begin());
            int const c = ring[(idx + ring.size() - 1) % ring.size()];
            a = b;
            b = c;
            if (a == kv.first.first && b == kv.first.second) {
                break;
            }
            if (!used.insert({a, b}).second) {
                // Re-entered a claimed directed edge mid-walk: the walk is
                // inconsistent (dangling spur); abandon it.
                face.clear();
                break;
            }
        }
        if (face.size() >= 3) {
            faces.push_back(face);
        }
    }
    // Signed area per face in its first node's tangent chart: interior faces
    // are CCW (positive); the single outer boundary is CW (negative) and is
    // excluded from the regions. A fully closed partition has no outer face.
    struct _Face {
        std::vector<int> loop;
        double area = 0.0;
    };
    std::vector<_Face> kept;
    for (auto const &loop : faces) {
        // Distinct-node check (a spur walk repeats nodes).
        std::set<int> uniq(loop.begin(), loop.end());
        if (uniq.size() < 3) {
            continue;
        }
        TonicGraphNode const *nd = FindNode(loop[0]);
        float n[3] = {nd->n[0], nd->n[1], nd->n[2]};
        float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (!(nl > 0.0f)) {
            n[1] = 1.0f;
            nl = 1.0f;
        }
        n[0] /= nl;
        n[1] /= nl;
        n[2] /= nl;
        float ref[3] = {1.0f, 0.0f, 0.0f};
        if (std::fabs(n[0]) > 0.9f) {
            ref[0] = 0.0f;
            ref[2] = 1.0f;
        }
        float t1[3] = {ref[0] - n[0] * (ref[0] * n[0] + ref[2] * n[2]),
                       -n[1] * (ref[0] * n[0] + ref[2] * n[2]),
                       ref[2] - n[2] * (ref[0] * n[0] + ref[2] * n[2])};
        float l1 = std::sqrt(t1[0] * t1[0] + t1[1] * t1[1] + t1[2] * t1[2]);
        if (!(l1 > 0.0f)) {
            t1[0] = 1.0f;
            t1[1] = t1[2] = 0.0f;
            l1 = 1.0f;
        }
        t1[0] /= l1;
        t1[1] /= l1;
        t1[2] /= l1;
        float t2[3] = {n[1] * t1[2] - n[2] * t1[1],
                       n[2] * t1[0] - n[0] * t1[2],
                       n[0] * t1[1] - n[1] * t1[0]};
        double area = 0.0;
        size_t const m = loop.size();
        for (size_t i = 0; i < m; ++i) {
            TonicGraphNode const *p0 = FindNode(loop[i]);
            TonicGraphNode const *p1 = FindNode(loop[(i + 1) % m]);
            float d0[3] = {p0->p[0] - nd->p[0], p0->p[1] - nd->p[1],
                           p0->p[2] - nd->p[2]};
            float d1[3] = {p1->p[0] - nd->p[0], p1->p[1] - nd->p[1],
                           p1->p[2] - nd->p[2]};
            double x0 = d0[0] * t1[0] + d0[1] * t1[1] + d0[2] * t1[2];
            double y0 = d0[0] * t2[0] + d0[1] * t2[1] + d0[2] * t2[2];
            double x1 = d1[0] * t1[0] + d1[1] * t1[1] + d1[2] * t1[2];
            double y1 = d1[0] * t2[0] + d1[1] * t2[1] + d1[2] * t2[2];
            area += x0 * y1 - x1 * y0;
        }
        kept.push_back(_Face{loop, 0.5 * area});
    }
    // Drop the outer faces: every negative-area face is the boundary of
    // its loop component walked clockwise (each disjoint loop has its own;
    // a closed partition has none). If every face is negative the charts
    // disagree globally (pathological input); keep everything rather than
    // return an empty graph.
    {
        size_t positive = 0;
        for (auto const &face : kept) {
            positive += (face.area >= 0.0) ? 1 : 0;
        }
        if (positive > 0) {
            kept.erase(std::remove_if(kept.begin(), kept.end(),
                                      [](_Face const &face) {
                                          return face.area < 0.0;
                                      }),
                       kept.end());
        }
    }
    // Deterministic region order: by the loop's minimum node id.
    std::sort(kept.begin(), kept.end(), [](_Face const &a, _Face const &b) {
        return *std::min_element(a.loop.begin(), a.loop.end()) <
               *std::min_element(b.loop.begin(), b.loop.end());
    });
    // Prune links that name dead regions (region ids are dense per extract).
    _linked.erase(std::remove_if(_linked.begin(), _linked.end(),
                                 [&](std::pair<int, int> const &pr) {
                                     return pr.first < 0 ||
                                            pr.first >= int(kept.size()) ||
                                            pr.second < 0 ||
                                            pr.second >= int(kept.size());
                                 }),
                  _linked.end());
    _UnionFind uf;
    uf.parent.resize(kept.size());
    std::iota(uf.parent.begin(), uf.parent.end(), 0);
    for (auto const &pr : _linked) {
        uf.Unite(pr.first, pr.second);
    }
    for (size_t i = 0; i < kept.size(); ++i) {
        TonicGraphRegion r;
        r.id = int(i);
        r.loop = kept[i].loop;
        TonicRegionColor(r.id, r.color);
        r.interpId = uf.Find(int(i));
        // Boundary polyline: join the edge polylines loop-order.
        for (size_t k = 0; k < r.loop.size(); ++k) {
            int const a = r.loop[k];
            int const b = r.loop[(k + 1) % r.loop.size()];
            auto it = edgeOf.find({a, b});
                if (it == edgeOf.end()) {
                it = edgeOf.find({b, a});
            }
            if (it == edgeOf.end()) {
                continue;
            }
            TonicGraphEdge const *e = FindEdge(it->second);
            if (!e || e->polyline.size() < 6) {
                continue;
            }
            bool const fwd = (e->a == a);
            size_t const n = e->polyline.size() / 3;
            for (size_t s = 0; s < n; ++s) {
                if (k > 0 && s == 0) {
                    continue;  // share the joint sample, no duplicates
                }
                size_t const idx = fwd ? s : (n - 1 - s);
                r.boundary.push_back(e->polyline[idx * 3 + 0]);
                r.boundary.push_back(e->polyline[idx * 3 + 1]);
                r.boundary.push_back(e->polyline[idx * 3 + 2]);
            }
        }
        // Flood seed: the face nearest the loop centroid.
        float c[3] = {0.0f, 0.0f, 0.0f};
        for (int nid : r.loop) {
            TonicGraphNode const *nd = FindNode(nid);
            c[0] += nd->p[0];
            c[1] += nd->p[1];
            c[2] += nd->p[2];
        }
        c[0] /= float(r.loop.size());
        c[1] /= float(r.loop.size());
        c[2] /= float(r.loop.size());
        TonicHit const seed = TonicClosestPointCpu(mesh, c);
        r.seedFace = seed.hit ? seed.faceId : -1;
        _regions.push_back(std::move(r));
    }
    return true;
}

void TonicScalpGraph::Clear()
{
    _nodes.clear();
    _edges.clear();
    _regions.clear();
    _linked.clear();
    _nextNodeId = 0;
    _nextEdgeId = 0;
    _diagnostic.clear();
}

bool TonicScalpGraph::Restore(TonicScalpMesh const &mesh,
                              std::vector<TonicGraphNode> const &nodes,
                              std::vector<std::pair<int, int>> const &edges,
                              std::vector<std::pair<int, int>> const &linked,
                              std::string *err)
{
    Clear();
    int maxNode = -1;
    for (auto const &nd : nodes) {
        // Parent-mesh range, not the face subset: a subset never renumbers
        // faces, and a node on a face a later subset edit dropped still
        // addresses real geometry (plan/02 §2.20 rule 2).
        if (nd.faceId < 0 ||
            size_t(nd.faceId) >= mesh.faceVertexCounts.size()) {
            if (err) {
                *err = "TonicScalpGraph::Restore: node face out of range";
            }
            Clear();
            return false;
        }
        _nodes.push_back(nd);
        _nodes.back().alive = true;
        maxNode = std::max(maxNode, nd.id);
    }
    _nextNodeId = maxNode + 1;
    for (auto const &pr : edges) {
        if (!FindNode(pr.first) || !FindNode(pr.second)) {
            if (err) {
                *err = "TonicScalpGraph::Restore: edge names unknown nodes";
            }
            Clear();
            return false;
        }
        TonicGraphNode const *na = FindNode(pr.first);
        TonicGraphNode const *nb = FindNode(pr.second);
        std::vector<float> poly = TonicTraceEdgeCpu(mesh, na->p, nb->p);
        if (poly.size() < 6) {
            if (err) {
                *err = "TonicScalpGraph::Restore: K2 trace failed";
            }
            Clear();
            return false;
        }
        TonicGraphEdge e;
        e.id = _NextEdgeId();
        e.a = pr.first;
        e.b = pr.second;
        e.polyline = std::move(poly);
        _edges.push_back(e);
    }
    if (!ExtractRegions(mesh)) {
        if (err) {
            *err = _diagnostic;
        }
        Clear();
        return false;
    }
    for (auto const &pr : linked) {
        LinkRegions(pr.first, pr.second);
    }
    return true;
}

namespace {

// Douglas–Peucker over the sample positions (3D, in rest units).
void _SimplifyRec(std::vector<TonicHit> const &samples, size_t first,
                  size_t last, float eps, std::vector<char> *keep)
{
    if (last <= first + 1) {
        return;
    }
    float const *a = &samples[first].px;
    float const *b = &samples[last].px;
    // NOTE: TonicHit stores px,py,pz adjacent (px then py then pz).
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float const len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float best = 0.0f;
    size_t split = first;
    for (size_t i = first + 1; i < last; ++i) {
        float const *p = &samples[i].px;
        float t = 0.0f;
        if (len2 > 0.0f) {
            t = ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] +
                 (p[2] - a[2]) * ab[2]) /
                len2;
            t = std::min(std::max(t, 0.0f), 1.0f);
        }
        float dx = p[0] - (a[0] + ab[0] * t);
        float dy = p[1] - (a[1] + ab[1] * t);
        float dz = p[2] - (a[2] + ab[2] * t);
        float const d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > best) {
            best = d;
            split = i;
        }
    }
    if (best > eps) {
        (*keep)[split] = 1;
        _SimplifyRec(samples, first, split, eps, keep);
        _SimplifyRec(samples, split, last, eps, keep);
    }
}

}  // namespace

TonicStrokeResult TonicStrokeToChain(TonicScalpMesh const &mesh,
                                     TonicScalpGraph *graph,
                                     std::vector<TonicHit> const &samples,
                                     float snapRadius, float simplifyEps)
{
    TonicStrokeResult result;
    if (!graph || !mesh.finalized || samples.size() < 2) {
        return result;
    }
    std::vector<char> keep(samples.size(), 0);
    keep.front() = keep.back() = 1;
    _SimplifyRec(samples, 0, samples.size() - 1, simplifyEps, &keep);
    std::vector<TonicHit> pts;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (keep[i] && samples[i].hit) {
            pts.push_back(samples[i]);
        }
    }
    if (pts.size() < 2) {
        return result;
    }
    float const *s0 = &pts.front().px;
    float const *s1 = &pts.back().px;
    float dx = s1[0] - s0[0];
    float dy = s1[1] - s0[1];
    float dz = s1[2] - s0[2];
    bool const closes =
        std::sqrt(dx * dx + dy * dy + dz * dz) <= snapRadius && pts.size() > 2;
    // Resolve the chain ends: weld to a snapped node, split-and-weld on a
    // snapped edge, else place fresh nodes.
    auto resolveEnd = [&](TonicHit const &h, bool *welded) {
        int const node = graph->SnapNode(&h.px, snapRadius);
        if (node >= 0) {
            *welded = true;
            return node;
        }
        int const edge = graph->SnapEdge(&h.px, snapRadius);
        if (edge >= 0) {
            int const mid = graph->SplitEdge(mesh, edge, h);
            if (mid >= 0) {
                *welded = true;
                return mid;
            }
        }
        return graph->AddNode(h);
    };
    std::vector<int> chain;
    chain.push_back(resolveEnd(pts.front(), &result.weldedStart));
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        // Interior points resolve exactly like the ends: a stroke over an
        // existing corner shares it (and a stroke across an edge splits
        // it) instead of doubling the boundary.
        bool welded = false;
        chain.push_back(resolveEnd(pts[i], &welded));
    }
    if (closes) {
        chain.push_back(chain.front());
        result.weldedEnd = true;
        result.closedLoop = true;
    } else {
        chain.push_back(resolveEnd(pts.back(), &result.weldedEnd));
    }
    for (int id : chain) {
        if (id < 0) {
            return TonicStrokeResult();  // a placement failed: no chain
        }
    }
    result.nodeIds = chain;
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
        int const eid = graph->Connect(mesh, chain[i], chain[i + 1]);
        if (eid >= 0) {
            result.edgeIds.push_back(eid);
        }
    }
    graph->ExtractRegions(mesh);
    return result;
}

}  // namespace usdGenTonic
