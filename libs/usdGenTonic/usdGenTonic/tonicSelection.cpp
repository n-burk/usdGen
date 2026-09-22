// usdGenTonic — selection state + marquee/lasso (plan/18 §2.3).
#include "usdGenTonic/tonicSelection.h"

#include <algorithm>
#include <cmath>

namespace usdGenTonic {

bool
TonicSelectionKindIsTubeKeyed(uint32_t kind)
{
    return kind == TonicPick_TubeVert || kind == TonicPick_CenterCV ||
           kind == TonicPick_SectionCV || kind == TonicPick_SectionRing;
}

void
TonicSelection::Clear(uint32_t kindMask)
{
    uint32_t const mask = kindMask ? kindMask : TonicSelect_All;
    bool changed = false;
    for (auto it = _items.begin(); it != _items.end();) {
        if (it->kind & mask) {
            it = _items.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (_hover.kind && (_hover.kind & mask)) {
        _hover = TonicSelectionItem();
        changed = true;
    }
    if (changed) {
        _Bump();
    }
}

void
TonicSelection::Apply(TonicSelectMode mode,
                      std::vector<TonicSelectionItem> const &items)
{
    bool changed = false;
    if (mode == TonicSelect_Set) {
        // Set replaces the kinds it names, not the whole selection: a Tube
        // mode click must not drop the graph nodes another mode selected.
        // Re-setting what is already selected has to be a true no-op —
        // a drag re-sets the same item on every sample, and each bump
        // would republish the viewport for nothing.
        uint32_t kinds = 0;
        for (TonicSelectionItem const &item : items) {
            kinds |= item.kind;
        }
        std::set<TonicSelectionItem> desired(items.begin(), items.end());
        std::set<TonicSelectionItem> current;
        for (TonicSelectionItem const &item : _items) {
            if (item.kind & kinds) {
                current.insert(item);
            }
        }
        if (current == desired) {
            return;
        }
        for (auto it = _items.begin(); it != _items.end();) {
            if (it->kind & kinds) {
                it = _items.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
    for (TonicSelectionItem const &item : items) {
        if (!item.kind) {
            continue;
        }
        if (mode == TonicSelect_Toggle) {
            auto it = _items.find(item);
            if (it != _items.end()) {
                _items.erase(it);
                changed = true;
                continue;
            }
        }
        changed = _items.insert(item).second || changed;
    }
    if (changed) {
        _Bump();
    }
}

bool
TonicSelection::Contains(TonicSelectionItem const &item) const
{
    return _items.find(item) != _items.end();
}

size_t
TonicSelection::Count(uint32_t kindMask) const
{
    uint32_t const mask = kindMask ? kindMask : TonicSelect_All;
    size_t n = 0;
    for (TonicSelectionItem const &item : _items) {
        if (item.kind & mask) {
            ++n;
        }
    }
    return n;
}

std::vector<TonicSelectionItem>
TonicSelection::Items(uint32_t kindMask) const
{
    uint32_t const mask = kindMask ? kindMask : TonicSelect_All;
    std::vector<TonicSelectionItem> out;
    out.reserve(_items.size());
    for (TonicSelectionItem const &item : _items) {
        if (item.kind & mask) {
            out.push_back(item);
        }
    }
    return out;
}

void
TonicSelection::RemoveWholeTubeItems(std::vector<int> const &tubeIds)
{
    if (tubeIds.empty()) {
        return;
    }
    bool changed = false;
    for (auto it = _items.begin(); it != _items.end();) {
        if (it->kind == TonicPick_TubeVert &&
            std::find(tubeIds.begin(), tubeIds.end(), it->id) !=
                tubeIds.end()) {
            it = _items.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        _Bump();
    }
}

void
TonicSelection::SetHover(TonicSelectionItem const &item)
{
    if (_hover == item) {
        return;
    }
    _hover = item;
    _Bump();
}

void
TonicSelection::RemapTube(int parentTubeId,
                          std::vector<int> const &childTubeIds)
{
    bool parentSelected = false;
    bool changed = false;
    for (auto it = _items.begin(); it != _items.end();) {
        if (TonicSelectionKindIsTubeKeyed(it->kind) &&
            it->id == parentTubeId) {
            parentSelected = parentSelected || it->kind == TonicPick_TubeVert;
            it = _items.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (parentSelected) {
        for (int child : childTubeIds) {
            TonicSelectionItem item;
            item.kind = TonicPick_TubeVert;
            item.id = child;
            changed = _items.insert(item).second || changed;
        }
    }
    if (_hover.kind && TonicSelectionKindIsTubeKeyed(_hover.kind) &&
        _hover.id == parentTubeId) {
        _hover = TonicSelectionItem();
        changed = true;
    }
    if (changed) {
        _Bump();
    }
}

bool
TonicSelection::PruneTubes(std::vector<int> const &liveTubeIds)
{
    bool changed = false;
    auto live = [&](int id) {
        return std::find(liveTubeIds.begin(), liveTubeIds.end(), id) !=
               liveTubeIds.end();
    };
    for (auto it = _items.begin(); it != _items.end();) {
        if (TonicSelectionKindIsTubeKeyed(it->kind) && !live(it->id)) {
            it = _items.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (_hover.kind && TonicSelectionKindIsTubeKeyed(_hover.kind) &&
        !live(_hover.id)) {
        _hover = TonicSelectionItem();
        changed = true;
    }
    if (changed) {
        _Bump();
    }
    return changed;
}

// -- marquee and lasso --------------------------------------------------------

namespace {

// One pass over every candidate set, with `inside` deciding the shape. The
// rect and the lasso differ in that predicate and in nothing else, which is
// what keeps their candidate order (and so the selection they produce)
// identical.
template <class InsideFn>
bool
_SelectRegion(TonicPickSets const &sets, uint32_t kindMask,
              float const viewProj[16], int w, int h,
              unsigned char const *tubeVertMask,
              unsigned char const *guideCVMask,
              std::vector<TonicPickCandidate> *out, InsideFn const &inside)
{
    if (!viewProj || w <= 0 || h <= 0 || !out) {
        return false;
    }
    auto consider = [&](uint32_t kind, float const *p, int index,
                        int subIndex) {
        if (!(kindMask & kind) || !p) {
            return;
        }
        float px, py, ndcZ;
        if (!TonicProjectPoint(p, viewProj, w, h, &px, &py, &ndcZ)) {
            return;
        }
        if (!inside(px, py)) {
            return;
        }
        TonicPickCandidate hit;
        hit.kind = kind;
        hit.index = index;
        hit.subIndex = subIndex;
        out->push_back(hit);
    };
    if ((kindMask & TonicPick_TubeVert) && sets.tubeVerts) {
        for (int i = 0; i < sets.tubeVertCount; ++i) {
            if (tubeVertMask) {
                if (tubeVertMask[i]) {
                    TonicPickCandidate hit;
                    hit.kind = TonicPick_TubeVert;
                    hit.index = i;
                    out->push_back(hit);
                }
            } else {
                consider(TonicPick_TubeVert, sets.tubeVerts + size_t(i) * 3,
                         i, -1);
            }
        }
    }
    for (int i = 0; i < sets.centerCVCount; ++i) {
        consider(TonicPick_CenterCV, sets.centerCVs + size_t(i) * 3, i, -1);
    }
    for (int i = 0; i < sets.sectionCVCount; ++i) {
        if (sets.sectionRingVerts > 0) {
            consider(TonicPick_SectionCV, sets.sectionCVs + size_t(i) * 3,
                     i / sets.sectionRingVerts, i % sets.sectionRingVerts);
        } else {
            consider(TonicPick_SectionCV, sets.sectionCVs + size_t(i) * 3, i,
                     -1);
        }
    }
    for (int i = 0; i < sets.graphNodeCount; ++i) {
        consider(TonicPick_GraphNode, sets.graphNodes + size_t(i) * 3, i, -1);
    }
    if ((kindMask & TonicPick_Guide) && sets.guideCVs) {
        for (int g = 0; g < sets.guideCount; ++g) {
            for (int c = 0; c < sets.guideCvCount; ++c) {
                size_t const flat =
                    size_t(g) * size_t(sets.guideCvCount) + size_t(c);
                if (guideCVMask) {
                    if (guideCVMask[flat]) {
                        TonicPickCandidate hit;
                        hit.kind = TonicPick_Guide;
                        hit.index = g;
                        hit.subIndex = c;
                        out->push_back(hit);
                    }
                } else {
                    consider(TonicPick_Guide, sets.guideCVs + flat * 3, g, c);
                }
            }
        }
    }
    if (sets.graphEdgeIds) {
        for (int i = 0; i < sets.graphEdgeCVCount; ++i) {
            consider(TonicPick_GraphEdge, sets.graphEdgeCVs + size_t(i) * 3,
                     sets.graphEdgeIds[i], i);
        }
    }
    if (sets.regionIds) {
        for (int i = 0; i < sets.regionCount; ++i) {
            consider(TonicPick_Region, sets.regionCenters + size_t(i) * 3,
                     sets.regionIds[i], -1);
        }
    }
    for (int i = 0; i < sets.ringCount; ++i) {
        consider(TonicPick_SectionRing, sets.ringCenters + size_t(i) * 3, i,
                 -1);
    }
    return true;
}

}  // namespace

bool
TonicSelectRectCpu(TonicPickSets const &sets, uint32_t kindMask,
                   float const viewProj[16], int w, int h, float x0, float y0,
                   float x1, float y1, unsigned char const *tubeVertMask,
                   unsigned char const *guideCVMask,
                   std::vector<TonicPickCandidate> *out)
{
    return _SelectRegion(sets, kindMask, viewProj, w, h, tubeVertMask,
                         guideCVMask, out,
                         [&](float px, float py) {
                             return TonicPointInRect(px, py, x0, y0, x1, y1);
                         });
}

bool
TonicSelectPolygonCpu(TonicPickSets const &sets, uint32_t kindMask,
                      float const viewProj[16], int w, int h,
                      float const *xy, int pointCount,
                      unsigned char const *tubeVertMask,
                      unsigned char const *guideCVMask,
                      std::vector<TonicPickCandidate> *out)
{
    if (!xy || pointCount < 3) {
        return false;
    }
    return _SelectRegion(sets, kindMask, viewProj, w, h, tubeVertMask,
                         guideCVMask, out, [&](float px, float py) {
                             return TonicPointInPolygon(px, py, xy,
                                                        pointCount);
                         });
}

TonicPickHit
TonicPickExtraKindsCpu(TonicPickSets const &sets, uint32_t kindMask,
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
    if (sets.graphEdgeIds) {
        // Edges pick by segment, not by vertex. The trace resamples each
        // edge uniformly (ceil(dist / h) + 1 vertices), so an even vertex
        // count parks every vertex away from the edge's midpoint and a
        // vertex-only pick misses a click sitting exactly on the edge.
        // Consecutive same-id vertices are one polyline: the build pushes
        // each edge's run together.
        auto considerSegment = [&](float const *a, float const *b, int edgeId,
                                   int subIndex) {
            if (!(kindMask & TonicPick_GraphEdge) || !a || !b) {
                return;
            }
            float ax, ay, az;
            float bx, by, bz;
            if (!TonicProjectPoint(a, viewProj, w, h, &ax, &ay, &az) ||
                !TonicProjectPoint(b, viewProj, w, h, &bx, &by, &bz)) {
                return;
            }
            float const abx = bx - ax, aby = by - ay;
            float const len2 = abx * abx + aby * aby;
            float t = 0.0f;
            if (len2 > 0.0f) {
                t = ((x - ax) * abx + (y - ay) * aby) / len2;
                t = std::min(std::max(t, 0.0f), 1.0f);
            }
            float const dx = ax + abx * t - x, dy = ay + aby * t - y;
            float const d2 = dx * dx + dy * dy;
            if (d2 > r2) {
                return;
            }
            float const dist = std::sqrt(d2);
            float const ndcZ = az + (bz - az) * t;
            if (!best.hit || dist < best.distPx ||
                (dist == best.distPx && ndcZ < best.depth)) {
                best.hit = true;
                best.kind = TonicPick_GraphEdge;
                best.index = edgeId;
                best.subIndex = subIndex;
                best.distPx = dist;
                best.depth = ndcZ;
            }
        };
        int runStart = 0;
        for (int i = 1; i <= sets.graphEdgeCVCount; ++i) {
            if (i < sets.graphEdgeCVCount &&
                sets.graphEdgeIds[i] == sets.graphEdgeIds[runStart]) {
                continue;
            }
            for (int j = runStart; j + 1 < i; ++j) {
                considerSegment(sets.graphEdgeCVs + size_t(j) * 3,
                                sets.graphEdgeCVs + size_t(j + 1) * 3,
                                sets.graphEdgeIds[j], j);
            }
            runStart = i;
        }
    }
    if (sets.regionIds) {
        for (int i = 0; i < sets.regionCount; ++i) {
            consider(TonicPick_Region, sets.regionCenters + size_t(i) * 3,
                     sets.regionIds[i], -1);
        }
    }
    for (int i = 0; i < sets.ringCount; ++i) {
        consider(TonicPick_SectionRing, sets.ringCenters + size_t(i) * 3, i,
                 -1);
    }
    return best;
}

}  // namespace usdGenTonic
