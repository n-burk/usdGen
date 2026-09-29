// usdGenPomade — selection state + marquee/lasso (plan/18 §2.3).
#include "usdGenPomade/pomadeSelection.h"

#include <algorithm>
#include <cmath>

namespace usdGenPomade {

bool
PomadeSelectionKindIsTubeKeyed(uint32_t kind)
{
    return kind == PomadePick_TubeVert || kind == PomadePick_CenterCV ||
           kind == PomadePick_SectionCV || kind == PomadePick_SectionRing;
}

void
PomadeSelection::Clear(uint32_t kindMask)
{
    uint32_t const mask = kindMask ? kindMask : PomadeSelect_All;
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
        _hover = PomadeSelectionItem();
        changed = true;
    }
    if (changed) {
        _Bump();
    }
}

void
PomadeSelection::Apply(PomadeSelectMode mode,
                      std::vector<PomadeSelectionItem> const &items)
{
    bool changed = false;
    if (mode == PomadeSelect_Set) {
        // Set replaces the kinds it names, not the whole selection: a Tube
        // mode click must not drop the graph nodes another mode selected.
        // Re-setting what is already selected has to be a true no-op —
        // a drag re-sets the same item on every sample, and each bump
        // would republish the viewport for nothing.
        uint32_t kinds = 0;
        for (PomadeSelectionItem const &item : items) {
            kinds |= item.kind;
        }
        std::set<PomadeSelectionItem> desired(items.begin(), items.end());
        std::set<PomadeSelectionItem> current;
        for (PomadeSelectionItem const &item : _items) {
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
    for (PomadeSelectionItem const &item : items) {
        if (!item.kind) {
            continue;
        }
        if (mode == PomadeSelect_Toggle) {
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
PomadeSelection::Contains(PomadeSelectionItem const &item) const
{
    return _items.find(item) != _items.end();
}

size_t
PomadeSelection::Count(uint32_t kindMask) const
{
    uint32_t const mask = kindMask ? kindMask : PomadeSelect_All;
    size_t n = 0;
    for (PomadeSelectionItem const &item : _items) {
        if (item.kind & mask) {
            ++n;
        }
    }
    return n;
}

std::vector<PomadeSelectionItem>
PomadeSelection::Items(uint32_t kindMask) const
{
    uint32_t const mask = kindMask ? kindMask : PomadeSelect_All;
    std::vector<PomadeSelectionItem> out;
    out.reserve(_items.size());
    for (PomadeSelectionItem const &item : _items) {
        if (item.kind & mask) {
            out.push_back(item);
        }
    }
    return out;
}

void
PomadeSelection::RemoveWholeTubeItems(std::vector<int> const &tubeIds)
{
    if (tubeIds.empty()) {
        return;
    }
    bool changed = false;
    for (auto it = _items.begin(); it != _items.end();) {
        if (it->kind == PomadePick_TubeVert &&
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
PomadeSelection::SetHover(PomadeSelectionItem const &item)
{
    if (_hover == item) {
        return;
    }
    _hover = item;
    _Bump();
}

void
PomadeSelection::RemapTube(int parentTubeId,
                          std::vector<int> const &childTubeIds)
{
    bool parentSelected = false;
    bool changed = false;
    for (auto it = _items.begin(); it != _items.end();) {
        if (PomadeSelectionKindIsTubeKeyed(it->kind) &&
            it->id == parentTubeId) {
            parentSelected = parentSelected || it->kind == PomadePick_TubeVert;
            it = _items.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (parentSelected) {
        for (int child : childTubeIds) {
            PomadeSelectionItem item;
            item.kind = PomadePick_TubeVert;
            item.id = child;
            changed = _items.insert(item).second || changed;
        }
    }
    if (_hover.kind && PomadeSelectionKindIsTubeKeyed(_hover.kind) &&
        _hover.id == parentTubeId) {
        _hover = PomadeSelectionItem();
        changed = true;
    }
    if (changed) {
        _Bump();
    }
}

bool
PomadeSelection::PruneTubes(std::vector<int> const &liveTubeIds)
{
    bool changed = false;
    auto live = [&](int id) {
        return std::find(liveTubeIds.begin(), liveTubeIds.end(), id) !=
               liveTubeIds.end();
    };
    for (auto it = _items.begin(); it != _items.end();) {
        if (PomadeSelectionKindIsTubeKeyed(it->kind) && !live(it->id)) {
            it = _items.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (_hover.kind && PomadeSelectionKindIsTubeKeyed(_hover.kind) &&
        !live(_hover.id)) {
        _hover = PomadeSelectionItem();
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
_SelectRegion(PomadePickSets const &sets, uint32_t kindMask,
              float const viewProj[16], int w, int h,
              unsigned char const *tubeVertMask,
              unsigned char const *guideCVMask,
              std::vector<PomadePickCandidate> *out, InsideFn const &inside)
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
        if (!PomadeProjectPoint(p, viewProj, w, h, &px, &py, &ndcZ)) {
            return;
        }
        if (!inside(px, py)) {
            return;
        }
        PomadePickCandidate hit;
        hit.kind = kind;
        hit.index = index;
        hit.subIndex = subIndex;
        out->push_back(hit);
    };
    if ((kindMask & PomadePick_TubeVert) && sets.tubeVerts) {
        for (int i = 0; i < sets.tubeVertCount; ++i) {
            if (tubeVertMask) {
                if (tubeVertMask[i]) {
                    PomadePickCandidate hit;
                    hit.kind = PomadePick_TubeVert;
                    hit.index = i;
                    out->push_back(hit);
                }
            } else {
                consider(PomadePick_TubeVert, sets.tubeVerts + size_t(i) * 3,
                         i, -1);
            }
        }
    }
    for (int i = 0; i < sets.centerCVCount; ++i) {
        consider(PomadePick_CenterCV, sets.centerCVs + size_t(i) * 3, i, -1);
    }
    for (int i = 0; i < sets.sectionCVCount; ++i) {
        if (sets.sectionRingVerts > 0) {
            consider(PomadePick_SectionCV, sets.sectionCVs + size_t(i) * 3,
                     i / sets.sectionRingVerts, i % sets.sectionRingVerts);
        } else {
            consider(PomadePick_SectionCV, sets.sectionCVs + size_t(i) * 3, i,
                     -1);
        }
    }
    for (int i = 0; i < sets.graphNodeCount; ++i) {
        consider(PomadePick_GraphNode, sets.graphNodes + size_t(i) * 3, i, -1);
    }
    if ((kindMask & PomadePick_Guide) && sets.guideCVs) {
        for (int g = 0; g < sets.guideCount; ++g) {
            for (int c = 0; c < sets.guideCvCount; ++c) {
                size_t const flat =
                    size_t(g) * size_t(sets.guideCvCount) + size_t(c);
                if (guideCVMask) {
                    if (guideCVMask[flat]) {
                        PomadePickCandidate hit;
                        hit.kind = PomadePick_Guide;
                        hit.index = g;
                        hit.subIndex = c;
                        out->push_back(hit);
                    }
                } else {
                    consider(PomadePick_Guide, sets.guideCVs + flat * 3, g, c);
                }
            }
        }
    }
    if (sets.graphEdgeIds) {
        for (int i = 0; i < sets.graphEdgeCVCount; ++i) {
            consider(PomadePick_GraphEdge, sets.graphEdgeCVs + size_t(i) * 3,
                     sets.graphEdgeIds[i], i);
        }
    }
    if (sets.regionIds) {
        for (int i = 0; i < sets.regionCount; ++i) {
            consider(PomadePick_Region, sets.regionCenters + size_t(i) * 3,
                     sets.regionIds[i], -1);
        }
    }
    for (int i = 0; i < sets.ringCount; ++i) {
        consider(PomadePick_SectionRing, sets.ringCenters + size_t(i) * 3, i,
                 -1);
    }
    return true;
}

}  // namespace

bool
PomadeSelectRectCpu(PomadePickSets const &sets, uint32_t kindMask,
                   float const viewProj[16], int w, int h, float x0, float y0,
                   float x1, float y1, unsigned char const *tubeVertMask,
                   unsigned char const *guideCVMask,
                   std::vector<PomadePickCandidate> *out)
{
    return _SelectRegion(sets, kindMask, viewProj, w, h, tubeVertMask,
                         guideCVMask, out,
                         [&](float px, float py) {
                             return PomadePointInRect(px, py, x0, y0, x1, y1);
                         });
}

bool
PomadeSelectPolygonCpu(PomadePickSets const &sets, uint32_t kindMask,
                      float const viewProj[16], int w, int h,
                      float const *xy, int pointCount,
                      unsigned char const *tubeVertMask,
                      unsigned char const *guideCVMask,
                      std::vector<PomadePickCandidate> *out)
{
    if (!xy || pointCount < 3) {
        return false;
    }
    return _SelectRegion(sets, kindMask, viewProj, w, h, tubeVertMask,
                         guideCVMask, out, [&](float px, float py) {
                             return PomadePointInPolygon(px, py, xy,
                                                        pointCount);
                         });
}

PomadePickHit
PomadePickExtraKindsCpu(PomadePickSets const &sets, uint32_t kindMask,
                       float const viewProj[16], int w, int h, float x,
                       float y, float radiusPx)
{
    PomadePickHit best;
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
        if (!PomadeProjectPoint(p, viewProj, w, h, &px, &py, &ndcZ)) {
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
            if (!(kindMask & PomadePick_GraphEdge) || !a || !b) {
                return;
            }
            float ax, ay, az;
            float bx, by, bz;
            if (!PomadeProjectPoint(a, viewProj, w, h, &ax, &ay, &az) ||
                !PomadeProjectPoint(b, viewProj, w, h, &bx, &by, &bz)) {
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
                best.kind = PomadePick_GraphEdge;
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
            consider(PomadePick_Region, sets.regionCenters + size_t(i) * 3,
                     sets.regionIds[i], -1);
        }
    }
    for (int i = 0; i < sets.ringCount; ++i) {
        consider(PomadePick_SectionRing, sets.ringCenters + size_t(i) * 3, i,
                 -1);
    }
    return best;
}

}  // namespace usdGenPomade
