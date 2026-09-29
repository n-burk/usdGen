// usdGenPomade — the model's selection (plan/18 §2.3, plan/17 §2.1/§4.6).
//
// plan/17 §2.1 lists `selection` on PomadeModel and it was never there: every
// mode kept its own idea of what was picked in Python, and the viewport drew
// none of it. This is that state, in the model, so that
//
//   * the scene index can colour what is selected (the `selected` primvar on
//     the level meshes and the white/yellow CV dots of plan/18 §2.4a);
//   * a commit cannot lose it — the committer never touches the model;
//   * every mode reads one set instead of six private ones.
//
// Selection is NOT on the undo stack (plan/18 §2.3): an artist who undoes a
// move expects the geometry back, not the click that preceded it.
//
// Items are (kind, id, subId, subSubId) tuples, one spelling per kind:
//
//   PomadePick_TubeVert     tube        id = tube id
//   PomadePick_CenterCV     center CV   id = tube id, subId = CV index
//   PomadePick_SectionCV    section CV  id = tube id, subId = ring,
//                                      subSubId = slot
//   PomadePick_SectionRing  ring        id = tube id, subId = ring
//   PomadePick_GraphNode    node        id = node id
//   PomadePick_GraphEdge    edge        id = edge id
//   PomadePick_Region       region      id = region id
//   PomadePick_Guide        guide       id = guide index
//   PomadePick_Level        level       id = hierarchy level (1-based)
//
// The set is ordered by that tuple, so two runs that select the same things
// read back in the same order and a test can compare them directly.
#ifndef USDGEN_POMADE_SELECTION_H
#define USDGEN_POMADE_SELECTION_H

#include "usdGenPomade/api.h"
#include "usdGenPomade/pomadeTube.h"

#include <cstdint>
#include <set>
#include <vector>

namespace usdGenPomade {

struct USDGENPOMADE_API PomadeSelectionItem {
    uint32_t kind = 0;
    int id = -1;
    int subId = -1;
    int subSubId = -1;

    bool operator<(PomadeSelectionItem const &o) const
    {
        if (kind != o.kind) {
            return kind < o.kind;
        }
        if (id != o.id) {
            return id < o.id;
        }
        if (subId != o.subId) {
            return subId < o.subId;
        }
        return subSubId < o.subSubId;
    }
    bool operator==(PomadeSelectionItem const &o) const
    {
        return kind == o.kind && id == o.id && subId == o.subId &&
               subSubId == o.subSubId;
    }
    bool operator!=(PomadeSelectionItem const &o) const
    {
        return !(*this == o);
    }
};

// How a new pick combines with what is already selected.
enum PomadeSelectMode : int {
    PomadeSelect_Set = 0,     // replace the kinds named by the mask
    PomadeSelect_Add = 1,     // union
    PomadeSelect_Toggle = 2,  // symmetric difference
};

class USDGENPOMADE_API PomadeSelection {
public:
    // Every mutator bumps the generation; the publisher keys its per-level
    // selection hash off the items, and the model keys its SELECTION dirty
    // off this counter, so a click that changes nothing publishes nothing.
    uint64_t Generation() const { return _generation; }

    // Drop every item whose kind is in `kindMask` (0 = every kind). The
    // hover item is cleared only when its own kind is in the mask.
    void Clear(uint32_t kindMask);
    // Replace / union / toggle `items` (all of one kind or mixed; Set
    // clears exactly the kinds present in `items` first).
    void Apply(PomadeSelectMode mode,
               std::vector<PomadeSelectionItem> const &items);
    bool Contains(PomadeSelectionItem const &item) const;
    size_t Count(uint32_t kindMask) const;
    // Ascending by (kind, id, subId, subSubId). `kindMask` 0 = every kind.
    std::vector<PomadeSelectionItem> Items(uint32_t kindMask) const;
    // Component editing supersedes a whole-tube selection for the same
    // owner. Used by area selection in Add/Toggle mode so a stale hierarchy
    // selection cannot turn a newly selected CV drag into a whole-tube drag.
    void RemoveWholeTubeItems(std::vector<int> const &tubeIds);

    // The one hovered item (kind 0 = nothing). Hover is separate from the
    // selection: it follows the cursor and never survives a press.
    void SetHover(PomadeSelectionItem const &item);
    PomadeSelectionItem Hover() const { return _hover; }

    // A subdivide replaces one tube by its children (plan/18 §2.3): the
    // parent's TUBE item becomes one per child, and its CV/ring items are
    // dropped — a child's CV layout is the parent's subdivision, not the
    // parent's CVs, so keeping the indices would point at the wrong cvs.
    // A no-op when the parent was not selected.
    void RemapTube(int parentTubeId, std::vector<int> const &childTubeIds);

    // Drop every tube-keyed item (tube, center CV, section CV, ring) whose
    // tube is not in `liveTubeIds`. Called on read, so a merge, an undo or
    // a hydrate can never leave the selection naming a tube that is gone
    // without every one of those paths having to remember to prune.
    // Returns true when something was dropped.
    bool PruneTubes(std::vector<int> const &liveTubeIds);

private:
    void _Bump() { ++_generation; }
    std::set<PomadeSelectionItem> _items;
    PomadeSelectionItem _hover;
    uint64_t _generation = 0;
};

// True for the kinds whose `id` is a tube id (the ones PruneTubes governs).
USDGENPOMADE_API bool PomadeSelectionKindIsTubeKeyed(uint32_t kind);

// -- marquee and lasso (plan/18 §2.3) -----------------------------------------
//
// Both run over the SAME PomadePickSets the point pick uses and project with
// the same PomadeProjectPoint, so "what the rubber band caught" and "what a
// click at that pixel would have hit" can never disagree about a candidate.
// Output is appended in candidate order (kind by kind, in the enum order the
// point pick scans), never sorted here: the model turns candidates into
// selection items and the set does the ordering.
//
// Heavyweight kinds (tube verts, guide CVs) are scanned here too; the model
// runs them on the device when the candidate count earns it and falls back
// to these. `deviceTubeMask` / `deviceGuideMask`, when non-null, are the
// device lane's per-candidate results and replace the scan for that kind.
USDGENPOMADE_API bool PomadeSelectRectCpu(
    PomadePickSets const &sets, uint32_t kindMask, float const viewProj[16],
    int w, int h, float x0, float y0, float x1, float y1,
    unsigned char const *tubeVertMask, unsigned char const *guideCVMask,
    std::vector<PomadePickCandidate> *out);

USDGENPOMADE_API bool PomadeSelectPolygonCpu(
    PomadePickSets const &sets, uint32_t kindMask, float const viewProj[16],
    int w, int h, float const *xy, int pointCount,
    unsigned char const *tubeVertMask, unsigned char const *guideCVMask,
    std::vector<PomadePickCandidate> *out);

// The four kinds V1 adds to the point pick (edge, region, ring — level is
// not screen-pickable). Folded into the winner by PomadeModel::Pick with the
// PomadePickCpu rule, after the kinds that TU already scans, so the tie order
// stays "earlier kind wins a dead heat".
USDGENPOMADE_API PomadePickHit PomadePickExtraKindsCpu(
    PomadePickSets const &sets, uint32_t kindMask, float const viewProj[16],
    int w, int h, float x, float y, float radiusPx);

}  // namespace usdGenPomade

#endif  // USDGEN_POMADE_SELECTION_H
