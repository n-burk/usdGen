// usdGenTonic — the asynchronous commit pipeline (plan/17 section 3, P1).
#include "usdGenTonic/tonicCommit.h"

// The cook the stage swap starts lives on the description's imaging
// session; cancelling it is the plan/17 §3.2 rule 2 half of G7.
#include "usdGenImaging/usdGenImagingSession.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/value.h"
#include "pxr/usd/sdf/attributeSpec.h"
#include "pxr/usd/sdf/changeBlock.h"
#include "pxr/usd/sdf/copyUtils.h"
#include "pxr/usd/sdf/listOp.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/propertySpec.h"
#include "pxr/usd/sdf/relationshipSpec.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/relationship.h"
#include "pxr/usd/usdGeom/mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenTonic {

std::string
TonicCommitPaths::TubeName(int tubeId)
{
    char buf[32];
    // '-' is not a legal prim-name character, so the negative ids the
    // on-the-fly parents mint spell as group<n>.
    std::snprintf(buf, sizeof(buf), tubeId < 0 ? "group%d" : "tube%d",
                  tubeId < 0 ? -tubeId : tubeId);
    return buf;
}

SdfPath
TonicCommitPaths::TubePath(int tubeId) const
{
    return TubesPath().AppendChild(TfToken(TubeName(tubeId)));
}

std::map<int, SdfPath>
TonicTubePaths(TonicSnapshot const &snapshot, TonicCommitPaths const &paths)
{
    std::map<int, SdfPath> out;
    std::map<int, int> parentOf;
    for (TonicSnapshotTube const &entry : snapshot.tubes) {
        parentOf[entry.tubeId] = entry.parentTubeId;
    }
    // Entries arrive parents-first, so one pass resolves every path. -1 is
    // the "no parent" sentinel, never a reference to the on-the-fly parent
    // that happens to carry id -1, so a root stays a root.
    for (TonicSnapshotTube const &entry : snapshot.tubes) {
        auto const parent = entry.parentTubeId == -1
                                ? out.end()
                                : out.find(entry.parentTubeId);
        out[entry.tubeId] =
            parent == out.end()
                ? paths.TubePath(entry.tubeId)
                : parent->second.AppendChild(
                      TfToken(TonicCommitPaths::TubeName(entry.tubeId)));
    }
    return out;
}

SdfPath
TonicCommitPaths::LevelMapPath(int level) const
{
    if (level <= 1) {
        return RegionMapPath();
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "RegionMapL%d", level);
    return groomPath.AppendChild(TfToken(buf));
}

SdfPath
TonicCommitPaths::LevelExprPath(int level) const
{
    if (level <= 1) {
        return RegionExprPath();
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "RegionExprL%d", level);
    return groomPath.AppendChild(TfToken(buf));
}

SdfPath
TonicCommitPaths::InterpOpPath() const
{
    // No description bound — "Bind scalp" on a stage with no usdGen
    // description, which is most of the T3 scripts — means there is no
    // op to fill in. Answer with an empty path: appending to the empty
    // one is an Sdf coding error, and every enqueue raised two of them
    // (here, then again from EnqueuePlan's empty-path retry). On the
    // main thread those are posted TfErrors that usdview's paintGL
    // decorator re-raises. Callers already treat an empty opPath as
    // "nothing to author": TonicPlanGuideInterpolateFill returns early
    // on the same condition without setting createInterpOp.
    if (descriptionPath.IsEmpty()) {
        return SdfPath();
    }
    // NOTE (chain order): v1 chains execute in reverse namespace order, so
    // this name also fixes the op's chain position. The fill-in never moves
    // an artist's stack; arranging operators around the tonic op is the
    // Output panel's job (P5), not the committer's.
    return descriptionPath.AppendChild(TfToken("Ops"))
        .AppendChild(TfToken("tonicInterp"));
}

namespace {

// -- Sdf authoring helpers (worker-safe: layer-local, no stage) --------------

SdfPrimSpecHandle
_EnsurePrim(SdfLayerHandle const &layer, SdfPath const &path,
            SdfSpecifier spec, char const *typeName)
{
    SdfPrimSpecHandle prim = SdfCreatePrimInLayer(layer, path);
    if (!prim) {
        return SdfPrimSpecHandle();
    }
    prim->SetSpecifier(spec);
    if (typeName) {
        prim->SetTypeName(typeName);
    }
    return prim;
}

template <class T>
bool
_SetAttr(SdfPrimSpecHandle const &prim, char const *name,
         SdfValueTypeName const &typeName, T const &value,
         SdfVariability variability = SdfVariabilityVarying,
         bool custom = false)
{
    SdfAttributeSpecHandle attr =
        SdfAttributeSpec::New(prim, name, typeName, variability, custom);
    if (!attr) {
        return false;
    }
    attr->SetDefaultValue(VtValue(value));
    return true;
}

bool
_SetRelTargets(SdfPrimSpecHandle const &prim, char const *name,
               SdfPathVector const &targets)
{
    SdfRelationshipSpecHandle rel =
        SdfRelationshipSpec::New(prim, name, /*custom*/ false);
    if (!rel) {
        return false;
    }
    SdfTargetsProxy proxies = rel->GetTargetPathList();
    proxies.ClearEditsAndMakeExplicit();
    proxies.GetExplicitItems() = targets;
    return true;
}

bool
_SetConnections(SdfPrimSpecHandle const &prim, char const *name,
                SdfValueTypeName const &typeName, SdfPathVector const &conns)
{
    SdfAttributeSpecHandle attr =
        SdfAttributeSpec::New(prim, name, typeName);
    if (!attr) {
        return false;
    }
    SdfConnectionsProxy proxies = attr->GetConnectionPathList();
    proxies.ClearEditsAndMakeExplicit();
    proxies.GetExplicitItems() = conns;
    return true;
}

bool
_PrependApiSchema(SdfPrimSpecHandle const &prim, char const *schema)
{
    SdfTokenListOp listOp;
    std::string err;
    if (!listOp.SetPrependedItems(TfTokenVector{TfToken(schema)}, &err)) {
        return false;
    }
    prim->SetInfo(TfToken("apiSchemas"), VtValue(listOp));
    return true;
}

bool
_SetInterpolation(SdfPrimSpecHandle const &prim, char const *attrName,
                  char const *interp)
{
    for (SdfAttributeSpecHandle const &attr : prim->GetAttributes()) {
        if (attr->GetName() == attrName) {
            attr->SetInfo(TfToken("interpolation"), VtValue(TfToken(interp)));
            return true;
        }
    }
    return false;
}

// C++ spellings of the relationship/connection getters (out-params, unlike
// the python GetTargets()/GetConnections()).
SdfPathVector
_Targets(UsdRelationship const &rel)
{
    SdfPathVector targets;
    rel.GetTargets(&targets);
    return targets;
}

SdfPathVector
_Connections(UsdAttribute const &attr)
{
    SdfPathVector conns;
    attr.GetConnections(&conns);
    return conns;
}

// The deepest tube whose cell contains `p`, walking down from tube 0 with
// the same K14 partition the map bake uses. Bridge imports are not cells
// (they are not part of a subdivision), so a foreign guide always lands
// under a derived tube.
int
_OwningLeafTube(TonicModel const &model, float const p[3])
{
    int current = 0;
    for (;;) {
        std::vector<int> cells;
        for (int kid : model.GetTubeChildren(current)) {
            if (!model.IsTubeImported(kid)) {
                cells.push_back(kid);
            }
        }
        if (cells.empty()) {
            return current;
        }
        TonicTubeDesc parent;
        if (!model.GetTubeDesc(current, &parent) ||
            parent.centerX.size() < 2) {
            return current;
        }
        std::vector<TonicFrame> frames;
        std::string err;
        if (!TonicCenterFramesCpu(parent.centerX.data(), parent.centerY.data(),
                                  parent.centerZ.data(),
                                  int(parent.centerX.size()), &frames, &err) ||
            frames.empty()) {
            return current;
        }
        // Ascending id IS ascending childIndex (id = parent * 16 + 1 + k).
        std::vector<float> centers;
        for (int kid : cells) {
            TonicTubeDesc child;
            if (!model.GetTubeDesc(kid, &child) || child.centerX.empty()) {
                return current;
            }
            centers.push_back(child.centerX[0]);
            centers.push_back(child.centerY[0]);
            centers.push_back(child.centerZ[0]);
        }
        float const root[3] = {parent.centerX[0], parent.centerY[0],
                               parent.centerZ[0]};
        int const cell = TonicOwningChildCell(root, frames[0], centers.data(),
                                              int(cells.size()), p);
        if (cell < 0) {
            return current;
        }
        current = cells[size_t(cell)];
    }
}

// Hand every mesh-fill face down the hierarchy to the leaf that owns it.
// The snapshot is ordered parents-first, so one forward pass cascades from
// L1 to the deepest level.
void
_PartitionRegionFaces(TonicSnapshot *snapshot)
{
    for (size_t i = 0; i < snapshot->tubes.size(); ++i) {
        TonicSnapshotTube &parent = snapshot->tubes[i];
        if (parent.regionFaces.empty()) {
            continue;
        }
        std::vector<size_t> cells;
        for (size_t j = 0; j < snapshot->tubes.size(); ++j) {
            TonicSnapshotTube const &child = snapshot->tubes[j];
            if (child.parentTubeId == parent.tubeId && !child.imported &&
                !child.tube.centerX.empty()) {
                cells.push_back(j);
            }
        }
        if (cells.empty()) {
            continue;
        }
        std::sort(cells.begin(), cells.end(), [&](size_t a, size_t b) {
            return snapshot->tubes[a].childIndex <
                   snapshot->tubes[b].childIndex;
        });
        TonicTubeDesc const pd = TonicTubeDescFromSnapshot(parent.tube);
        std::vector<TonicFrame> frames;
        std::string err;
        if (pd.centerX.size() < 2 ||
            !TonicCenterFramesCpu(pd.centerX.data(), pd.centerY.data(),
                                  pd.centerZ.data(), int(pd.centerX.size()),
                                  &frames, &err) ||
            frames.empty()) {
            continue;
        }
        float const rootCenter[3] = {pd.centerX[0], pd.centerY[0],
                                     pd.centerZ[0]};
        std::vector<float> childCenters;
        childCenters.reserve(cells.size() * 3);
        for (size_t j : cells) {
            childCenters.push_back(snapshot->tubes[j].tube.centerX[0]);
            childCenters.push_back(snapshot->tubes[j].tube.centerY[0]);
            childCenters.push_back(snapshot->tubes[j].tube.centerZ[0]);
        }
        std::vector<int> const faces = parent.regionFaces;
        parent.regionFaces.clear();
        for (int f : faces) {
            if (size_t(f) * 3 + 2 >= snapshot->scalp.faceCentroids.size()) {
                continue;
            }
            int const cell = TonicOwningChildCell(
                rootCenter, frames[0], childCenters.data(), int(cells.size()),
                &snapshot->scalp.faceCentroids[size_t(f) * 3]);
            if (cell >= 0) {
                snapshot->tubes[cells[size_t(cell)]].regionFaces.push_back(f);
            }
        }
    }
}

// -- V6 (plan/18 §7 G6): the per-version dirty set --------------------------
//
// FNV-1a over the bytes the guide fill reads. Floats go in by their bit
// pattern, not their value: the fill is bit-exact, so the hash has to be
// too (and -0.0f must not collide with 0.0f, which is exactly the case a
// value comparison would miss after a drag returns a CV to its start).
uint64_t constexpr kHashSeed = 1469598103934665603ull;

uint64_t _HashBytes(uint64_t h, void const *data, size_t bytes)
{
    auto const *p = static_cast<unsigned char const *>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= uint64_t(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t _HashInt(uint64_t h, int64_t v)
{
    return _HashBytes(h, &v, sizeof(v));
}

uint64_t _HashFloat(uint64_t h, float v)
{
    return _HashBytes(h, &v, sizeof(v));
}

uint64_t _HashFloats(uint64_t h, std::vector<float> const &v)
{
    h = _HashInt(h, int64_t(v.size()));
    return v.empty() ? h : _HashBytes(h, v.data(), v.size() * sizeof(float));
}

uint64_t _HashFillParams(uint64_t h, TonicModel::FillParams const &fill)
{
    h = _HashFloat(h, fill.density);
    h = _HashInt(h, fill.cvCount);
    h = _HashInt(h, fill.seed);
    h = _HashFloat(h, fill.edgeBias);
    h = _HashFloats(h, fill.lengthProfile);
    return h;
}

// Everything TonicGuidesFromSnapshot reads off one entry. The scalp is a
// shared input and is hashed once into the snapshot-wide salt instead.
uint64_t _HashSnapshotTube(TonicSnapshotTube const &entry)
{
    uint64_t h = kHashSeed;
    h = _HashInt(h, entry.tubeId);
    h = _HashInt(h, entry.regionId);
    h = _HashInt(h, entry.level);
    h = _HashInt(h, entry.tube.hasTube ? 1 : 0);
    h = _HashInt(h, entry.fillSuspended ? 1 : 0);
    h = _HashInt(h, entry.imported ? 1 : 0);
    h = _HashInt(h, int64_t(entry.members.size()));
    h = _HashInt(h, entry.tube.shape.rings);
    h = _HashInt(h, entry.tube.shape.ringVerts);
    h = _HashFloat(h, entry.tube.shape.radius);
    h = _HashFloat(h, entry.tube.shape.length);
    h = _HashFloats(h, entry.tube.centerX);
    h = _HashFloats(h, entry.tube.centerY);
    h = _HashFloats(h, entry.tube.centerZ);
    h = _HashInt(h, int64_t(entry.tube.sections.size()));
    for (TonicTubeSection const &section : entry.tube.sections) {
        h = _HashFloat(h, section.t);
        h = _HashFloats(h, section.u);
        h = _HashFloats(h, section.v);
    }
    h = _HashFillParams(h, entry.tube.fill);
    h = _HashInt(h, int64_t(entry.regionFaces.size()));
    if (!entry.regionFaces.empty()) {
        h = _HashBytes(h, entry.regionFaces.data(),
                       entry.regionFaces.size() * sizeof(int));
    }
    return h;
}

// The shared fill input: the scalp copy the worker carries. One number for
// the whole mesh, so a re-bind or a re-rasterise drops every cached slice.
uint64_t _HashScalp(TonicSnapshot const &snapshot)
{
    uint64_t h = kHashSeed;
    h = _HashInt(h, snapshot.hasScalp ? 1 : 0);
    h = _HashFloats(h, snapshot.scalp.points);
    h = _HashInt(h, int64_t(snapshot.scalp.faceVertexCounts.size()));
    if (!snapshot.scalp.faceVertexCounts.empty()) {
        h = _HashBytes(h, snapshot.scalp.faceVertexCounts.data(),
                       snapshot.scalp.faceVertexCounts.size() * sizeof(int));
    }
    h = _HashInt(h, int64_t(snapshot.scalp.faceVertexIndices.size()));
    if (!snapshot.scalp.faceVertexIndices.empty()) {
        h = _HashBytes(h, snapshot.scalp.faceVertexIndices.data(),
                       snapshot.scalp.faceVertexIndices.size() * sizeof(int));
    }
    return h;
}

} // namespace

uint64_t
TonicSnapshotTubeHash(TonicSnapshotTube const &entry)
{
    return _HashSnapshotTube(entry);
}

void
TonicHashSnapshot(TonicSnapshot *snapshot)
{
    if (!snapshot) {
        return;
    }
    for (TonicSnapshotTube &entry : snapshot->tubes) {
        entry.contentHash = _HashSnapshotTube(entry);
    }
}

TonicGuideCache::Entry const *
TonicGuideCache::Find(int tubeId, uint64_t contentHash) const
{
    auto const it = _entries.find(tubeId);
    if (it == _entries.end() || it->second.contentHash != contentHash) {
        return nullptr;
    }
    return &it->second;
}

void
TonicGuideCache::Store(int tubeId, Entry entry)
{
    _entries[tubeId] = std::move(entry);
}

bool
TonicGuideCache::NoteScalp(uint64_t scalpHash)
{
    if (scalpHash == _scalpHash) {
        return false;
    }
    _entries.clear();
    _scalpHash = scalpHash;
    return true;
}

size_t
TonicGuideCache::Reap(std::vector<int> const &live)
{
    std::vector<int> sorted = live;
    std::sort(sorted.begin(), sorted.end());
    size_t dropped = 0;
    for (auto it = _entries.begin(); it != _entries.end();) {
        if (std::binary_search(sorted.begin(), sorted.end(), it->first)) {
            ++it;
        } else {
            it = _entries.erase(it);
            ++dropped;
        }
    }
    return dropped;
}

void
TonicGuideCache::Clear()
{
    _entries.clear();
    _scalpHash = 0;
    _lastReused = 0;
    _lastRefilled = 0;
}

TonicSnapshot
TonicSnapshotFromModel(TonicModel const &model)
{
    TonicSnapshot snapshot;
    TonicModel::TubeSnapshot tube = model.Snapshot();
    snapshot.version = tube.version;
    snapshot.tubeRegionId = model.GetTubeRegionId();
    if (tube.hasTube) {
        TonicSnapshotTube entry;
        entry.tubeId = 0;
        // The model's real rooting, -1 included: hydrate replays the
        // same disc-vs-mesh rule from the stored region id, so the two
        // can never disagree (unrooted tubes stay on the disc stream
        // even when a scalp is bound).
        entry.regionId = snapshot.tubeRegionId;
        entry.level = 1;
        entry.tube = std::move(tube);
        snapshot.tubes.push_back(std::move(entry));
    }
    // V6: every further L1 root (one per closed region, plan/18 §7 G14).
    // They are ordinary store tubes with no parent, emitted right after
    // tube 0 so the breadth-first walk below can seed on all of them.
    std::vector<int> const l1Ids = model.L1TubeIds();
    for (int id : l1Ids) {
        if (id == 0) {
            continue;
        }
        TonicModel::TubeRecord record;
        if (!model.GetTubeRecord(id, &record) || !record.hasTube) {
            continue;
        }
        TonicSnapshotTube entry;
        entry.tubeId = id;
        entry.regionId = record.actual.regionId;
        entry.level = 1;
        entry.parentTubeId = -1;
        entry.childIndex = -1;
        entry.tube = TonicSnapshotFromTubeRecord(record);
        entry.tube.version = snapshot.version;
        snapshot.tubes.push_back(std::move(entry));
    }
    // -- V0b: the rest of the hierarchy, parents before children ---------
    //
    // Order matters twice over: TonicTubePaths resolves a child's prim path
    // from its parent's, and hydrate has to subdivide a parent before it can
    // restore that parent's children.
    if (!snapshot.tubes.empty()) {
        std::vector<int> const ids = model.TubeIds();
        std::map<int, TonicModel::TubeRecord> records;
        for (int id : ids) {
            if (id == 0) {
                continue;
            }
            TonicModel::TubeRecord record;
            if (model.GetTubeRecord(id, &record) && record.hasTube) {
                records[id] = std::move(record);
            }
        }
        // Emission order: every root (parent -1) first, then their
        // descendants breadth-first in ascending id order. Tube 0 is
        // already in; on-the-fly parents come last so their member paths
        // resolve. Transient parents are never committed (§5.4).
        std::vector<int> queue;
        for (TonicSnapshotTube const &entry : snapshot.tubes) {
            queue.push_back(entry.tubeId);  // the L1 roots, already emitted
        }
        std::vector<int> groups;
        for (auto const &kv : records) {
            if (kv.second.actual.parentTubeId < 0 &&
                (!kv.second.members.empty() || kv.second.transientParent)) {
                if (kv.second.transientParent && !kv.second.persistent) {
                    continue;
                }
                groups.push_back(kv.first);
            }
        }
        for (size_t head = 0; head < queue.size(); ++head) {
            int const parentId = queue[head];
            for (auto const &kv : records) {
                if (kv.second.actual.parentTubeId != parentId) {
                    continue;
                }
                if (kv.second.transientParent && !kv.second.persistent) {
                    continue;
                }
                queue.push_back(kv.first);
                TonicModel::TubeRecord const &record = kv.second;
                TonicSnapshotTube entry;
                entry.tubeId = kv.first;
                entry.regionId = record.actual.regionId;
                entry.level = record.actual.level;
                entry.parentTubeId = record.actual.parentTubeId;
                entry.childIndex = record.actual.childIndex;
                entry.tube = TonicSnapshotFromTubeRecord(record);
                entry.tube.version = snapshot.version;
                entry.transientParent = record.transientParent;
                entry.persistent = record.persistent;
                entry.imported = record.imported;
                entry.members = record.members;
                for (size_t i = 0; i < record.deltas.centerDu.size(); ++i) {
                    entry.centerDeltas.push_back(record.deltas.centerDu[i]);
                    entry.centerDeltas.push_back(record.deltas.centerDv[i]);
                    entry.centerDeltas.push_back(record.deltas.centerDw[i]);
                }
                for (auto const &sec : record.deltas.sections) {
                    for (size_t i = 0; i < sec.u.size(); ++i) {
                        entry.sectionDeltas.push_back(sec.u[i]);
                        entry.sectionDeltas.push_back(sec.v[i]);
                    }
                }
                snapshot.tubes.push_back(std::move(entry));
            }
        }
        for (int id : groups) {
            TonicModel::TubeRecord const &record = records[id];
            TonicSnapshotTube entry;
            entry.tubeId = id;
            entry.regionId = record.actual.regionId;
            entry.level = record.actual.level;
            entry.parentTubeId = -1;
            entry.childIndex = -1;
            entry.tube = TonicSnapshotFromTubeRecord(record);
            entry.tube.version = snapshot.version;
            entry.transientParent = record.transientParent;
            entry.persistent = record.persistent;
            entry.imported = record.imported;
            entry.members = record.members;
            snapshot.tubes.push_back(std::move(entry));
        }
        // Fill suspension (§2.3): a tube with a SUBDIVIDED child stops
        // filling; a bridge import is not a subdivision, so a parent that
        // only carries imports keeps its own fill.
        for (TonicSnapshotTube &entry : snapshot.tubes) {
            if (entry.tubeId < 0) {
                continue;  // -1 is "no parent", not this group's id
            }
            for (TonicSnapshotTube const &other : snapshot.tubes) {
                if (other.parentTubeId == entry.tubeId && !other.imported) {
                    entry.fillSuspended = true;
                    break;
                }
            }
        }
    }
    snapshot.graph = model.SnapshotGraph();
    // One region map + expression per level the hierarchy actually reaches
    // (plan/17 §4.5: channel k selects level k + 1).
    for (TonicSnapshotTube const &entry : snapshot.tubes) {
        snapshot.graph.levelCount =
            std::max(snapshot.graph.levelCount, entry.level);
    }
    // Mesh-fill inputs: the scalp copy plus the tube's region faces at
    // its interpolation id (the SnapshotGraph rasterise is always
    // current, so this never reads a stale map).
    std::shared_ptr<TonicScalpMesh const> scalp = model.GetScalp();
    // Union-find over the linked pairs (min id wins), mirroring
    // TonicScalpGraph::InterpId. (Sized by resize, not by a single-argument
    // size constructor: `X(size_t(n))` parses as a function declaration —
    // the most vexing parse.) Resolved once for every L1 root, not only
    // tube 0's region (V6, plan/18 §7 G14).
    int const regionCount = int(snapshot.graph.regionLoops.size());
    std::vector<int> regionParent;
    regionParent.resize(size_t(std::max(regionCount, 0)));
    for (int i = 0; i < regionCount; ++i) {
        regionParent[size_t(i)] = i;
    }
    for (auto const &pr : snapshot.graph.linked) {
        if (pr.first < 0 || pr.first >= regionCount || pr.second < 0 ||
            pr.second >= regionCount) {
            continue;
        }
        auto root = [&](int x) {
            while (regionParent[size_t(x)] != x) {
                regionParent[size_t(x)] =
                    regionParent[size_t(regionParent[size_t(x)])];
                x = regionParent[size_t(x)];
            }
            return x;
        };
        int const a = root(pr.first), b = root(pr.second);
        regionParent[size_t(std::max(a, b))] = std::min(a, b);
    }
    auto interpOf = [&](int regionId) {
        if (regionId < 0 || regionId >= regionCount) {
            return -1;
        }
        int x = regionId;
        while (regionParent[size_t(x)] != x) {
            regionParent[size_t(x)] =
                regionParent[size_t(regionParent[size_t(x)])];
            x = regionParent[size_t(x)];
        }
        return x;
    };
    if (scalp && scalp->finalized) {
        snapshot.scalp = *scalp;
        snapshot.hasScalp = true;
        int const interp = interpOf(snapshot.tubeRegionId);
        if (interp >= 0) {
            for (size_t f = 0; f < snapshot.graph.faceRegions.size(); ++f) {
                if (snapshot.graph.faceRegions[f] == interp) {
                    snapshot.tubeRegionFaces.push_back(int(f));
                }
            }
        }
    }
    // Per-tube mesh-fill roots (V0b): the L1 tube owns the region's faces,
    // and every subdivision hands its faces down to the child cell that
    // claims them — the same K14 partition the region map bakes per level
    // (§2.3, §4.5), so a strand's guide and its map channel agree.
    if (!snapshot.tubes.empty() && snapshot.hasScalp) {
        snapshot.tubes[0].regionFaces = snapshot.tubeRegionFaces;
        // Every other L1 root owns the faces of its own region, resolved
        // through the same interpolation id (linked regions share a fill
        // set, §2.2). Channel 0 of the bake and the mesh-fill roots then
        // agree per root instead of only for tube 0.
        for (TonicSnapshotTube &entry : snapshot.tubes) {
            if (entry.tubeId == 0 || entry.parentTubeId >= 0 ||
                entry.level != 1 || entry.regionId < 0) {
                continue;
            }
            int const interp = interpOf(entry.regionId);
            if (interp < 0) {
                continue;
            }
            for (size_t f = 0; f < snapshot.graph.faceRegions.size(); ++f) {
                if (snapshot.graph.faceRegions[f] == interp) {
                    entry.regionFaces.push_back(int(f));
                }
            }
        }
        _PartitionRegionFaces(&snapshot);
    }
    // Last, because the face partition above rewrites regionFaces: the hash
    // has to see the entry exactly as the guide fill will (plan/18 §7 G6).
    TonicHashSnapshot(&snapshot);
    return snapshot;
}

TonicSnapshotGuides
TonicGuidesFromSnapshot(TonicSnapshot const &snapshot)
{
    return TonicGuidesFromSnapshot(snapshot, nullptr);
}

TonicSnapshotGuides
TonicGuidesFromSnapshot(TonicSnapshot const &snapshot, TonicGuideCache *cache)
{
    TonicSnapshotGuides out;
    uint64_t nextId = 1000;
    std::vector<int> live;
    size_t reused = 0, refilled = 0;
    if (cache) {
        live.reserve(snapshot.tubes.size());
        // A changed scalp changes every mesh fill, so it is checked before
        // the first lookup, not after the build (plan/18 §7 G6).
        cache->NoteScalp(_HashScalp(snapshot));
    }
    for (TonicSnapshotTube const &entry : snapshot.tubes) {
        if (!entry.tube.hasTube || entry.fillSuspended || entry.imported ||
            !entry.members.empty()) {
            // An on-the-fly parent is an aggregate over tubes that fill
            // themselves; filling it too would double every strand.
            continue;
        }
        if (cache) {
            live.push_back(entry.tubeId);
        }
        // The dirty set: an entry whose bytes are unchanged refills to the
        // same guides, so its slice is copied rather than regenerated.
        TonicGuideCache::Entry const *hit =
            cache ? cache->Find(entry.tubeId, entry.contentHash) : nullptr;
        if (hit) {
            ++reused;
            out.points.insert(out.points.end(), hit->points.begin(),
                              hit->points.end());
            out.counts.insert(out.counts.end(), hit->counts.begin(),
                              hit->counts.end());
            out.frames.insert(out.frames.end(), hit->frames.begin(),
                              hit->frames.end());
            for (int g = 0; g < hit->guideCount; ++g) {
                out.ids.push_back(nextId++);
                out.tubeIds.push_back(entry.tubeId);
                out.levels.push_back(entry.level);
                out.regionIds.push_back(entry.regionId);
            }
            continue;
        }
        // Mesh roots when this tube owns scalp faces, else the root-disc
        // stream. The root hash stream is keyed by the tube id, so siblings
        // that inherited one set of fill params still differ.
        TonicGuideSet guides;
        if (snapshot.hasScalp && !entry.regionFaces.empty()) {
            guides = TonicGenerateGuidesOnScalpForTube(
                entry.tube, snapshot.scalp, entry.regionFaces.data(),
                int(entry.regionFaces.size()), entry.tubeId);
        } else {
            guides = TonicGenerateGuidesForTube(entry.tube, entry.tubeId);
        }
        ++refilled;
        out.points.insert(out.points.end(), guides.points.begin(),
                          guides.points.end());
        out.counts.insert(out.counts.end(), guides.counts.begin(),
                          guides.counts.end());
        out.frames.insert(out.frames.end(), guides.frames.begin(),
                          guides.frames.end());
        for (int g = 0; g < guides.guideCount; ++g) {
            out.ids.push_back(nextId++);
            out.tubeIds.push_back(entry.tubeId);
            out.levels.push_back(entry.level);
            out.regionIds.push_back(entry.regionId);
        }
        if (cache) {
            TonicGuideCache::Entry stored;
            stored.contentHash = entry.contentHash;
            stored.points = guides.points;
            stored.counts = guides.counts;
            stored.frames = guides.frames;
            stored.guideCount = guides.guideCount;
            cache->Store(entry.tubeId, std::move(stored));
        }
    }
    if (cache) {
        // Evict the tubes this version dropped (merge, delete, undo) so the
        // cache never outgrows the model.
        cache->Reap(live);
        cache->_lastReused = reused;
        cache->_lastRefilled = refilled;
    }
    return out;
}

bool
TonicBuildCommitLayer(TonicSnapshot const &snapshot,
                      TonicCommitPaths const &paths,
                      SdfLayerRefPtr *outLayer,
                      std::string *err,
                      TonicGuideCache *cache)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    if (!outLayer) {
        return fail("TonicBuildCommitLayer: null out-layer");
    }
    if (snapshot.tubes.empty()) {
        return fail("TonicBuildCommitLayer: snapshot holds no tubes");
    }
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("usdGenTonic-commit");
    if (!layer) {
        return fail("TonicBuildCommitLayer: cannot create anonymous layer");
    }

    // -- groom container ----------------------------------------------------
    SdfPrimSpecHandle groom =
        _EnsurePrim(layer, paths.groomPath, SdfSpecifierDef, "UsdGenTonicGroom");
    if (!groom) {
        return fail("TonicBuildCommitLayer: cannot author the groom prim");
    }
    if (!_SetAttr(groom, "usdGen:tonic:version", SdfValueTypeNames->Token,
                  TfToken("1"))) {
        return fail("TonicBuildCommitLayer: cannot author the groom version");
    }
    if (!paths.descriptionPath.IsEmpty() &&
        !_SetRelTargets(groom, "usdGen:tonic:description",
                        SdfPathVector{paths.descriptionPath})) {
        return fail("TonicBuildCommitLayer: cannot author the description link");
    }
    if (!snapshot.scalpPath.IsEmpty() &&
        !_SetRelTargets(groom, "usdGen:tonic:scalp",
                        SdfPathVector{snapshot.scalpPath})) {
        return fail("TonicBuildCommitLayer: cannot author the scalp link");
    }

    // -- tubes (hierarchy = prim hierarchy, every level) --------------------
    SdfPrimSpecHandle tubes =
        _EnsurePrim(layer, paths.TubesPath(), SdfSpecifierDef, "Scope");
    if (!tubes) {
        return fail("TonicBuildCommitLayer: cannot author the Tubes scope");
    }
    std::map<int, SdfPath> const tubePaths = TonicTubePaths(snapshot, paths);
    for (TonicSnapshotTube const &entry : snapshot.tubes) {
        TonicModel::TubeSnapshot const &tube = entry.tube;
        if (!tube.hasTube) {
            continue;
        }
        auto const pathIt = tubePaths.find(entry.tubeId);
        if (pathIt == tubePaths.end()) {
            return fail("TonicBuildCommitLayer: tube has no prim path");
        }
        SdfPrimSpecHandle prim =
            _EnsurePrim(layer, pathIt->second, SdfSpecifierDef, "UsdGenTube");
        if (!prim) {
            return fail("TonicBuildCommitLayer: cannot author a tube prim");
        }
        int const rings = int(tube.centerX.size());
        VtVec3fArray centers;
        centers.resize(size_t(rings));
        for (int r = 0; r < rings; ++r) {
            centers[size_t(r)] = GfVec3f(tube.centerX[size_t(r)],
                                         tube.centerY[size_t(r)],
                                         tube.centerZ[size_t(r)]);
        }
        // Authored sections (default circles for an untouched test tube,
        // so the commit bytes match the P1 form exactly).
        std::vector<TonicTubeSection> sections = tube.sections;
        if (sections.size() < 2) {
            sections = TonicDefaultSections(tube);
        }
        int const ringVerts = tube.shape.ringVerts;
        VtFloatArray sectionT;
        sectionT.resize(sections.size());
        VtVec2fArray sectionCvs;
        sectionCvs.resize(sections.size() * size_t(ringVerts));
        for (size_t r = 0; r < sections.size(); ++r) {
            sectionT[r] = sections[r].t;
            // The schema carries plane CVs only: scale and twist bake into
            // the committed pairs (hydrate restores scale 1 / twist 0 with
            // identical placement, so guides round-trip bit-exactly).
            float const ct = std::cos(sections[r].twist);
            float const st = std::sin(sections[r].twist);
            for (int s = 0; s < ringVerts; ++s) {
                float const uu =
                    sections[r].u[size_t(s)] * sections[r].scale;
                float const vv =
                    sections[r].v[size_t(s)] * sections[r].scale;
                sectionCvs[r * size_t(ringVerts) + size_t(s)] =
                    GfVec2f(uu * ct - vv * st, uu * st + vv * ct);
            }
        }
        VtFloatArray lengthProfile;
        lengthProfile.resize(tube.fill.lengthProfile.size());
        for (size_t i = 0; i < tube.fill.lengthProfile.size(); ++i) {
            lengthProfile[i] = tube.fill.lengthProfile[i];
        }
        // Deltas ride the schema's own layout: 3 floats per center CV in
        // the derived frames, 2 per section vertex in the section plane.
        VtVec3fArray centerDeltas;
        centerDeltas.resize(entry.centerDeltas.size() / 3);
        for (size_t i = 0; i < centerDeltas.size(); ++i) {
            centerDeltas[i] = GfVec3f(entry.centerDeltas[i * 3],
                                      entry.centerDeltas[i * 3 + 1],
                                      entry.centerDeltas[i * 3 + 2]);
        }
        VtVec2fArray sectionDeltas;
        sectionDeltas.resize(entry.sectionDeltas.size() / 2);
        for (size_t i = 0; i < sectionDeltas.size(); ++i) {
            sectionDeltas[i] = GfVec2f(entry.sectionDeltas[i * 2],
                                       entry.sectionDeltas[i * 2 + 1]);
        }
        bool attrsOk =
            _SetAttr(prim, "usdGen:tonic:regionId", SdfValueTypeNames->Int,
                     entry.regionId) &&
            _SetAttr(prim, "usdGen:tonic:level", SdfValueTypeNames->Int,
                     entry.level) &&
            _SetAttr(prim, "usdGen:tonic:centerPoints",
                     SdfValueTypeNames->Point3fArray, centers) &&
            _SetAttr(prim, "usdGen:tonic:sectionT",
                     SdfValueTypeNames->FloatArray, sectionT) &&
            _SetAttr(prim, "usdGen:tonic:sectionCvCount",
                     SdfValueTypeNames->Int, ringVerts) &&
            _SetAttr(prim, "usdGen:tonic:sectionCvs",
                     SdfValueTypeNames->Float2Array, sectionCvs) &&
            _SetAttr(prim, "usdGen:tonic:childIndex", SdfValueTypeNames->Int,
                     entry.childIndex) &&
            _SetAttr(prim, "usdGen:tonic:centerDeltas",
                     SdfValueTypeNames->Point3fArray, centerDeltas) &&
            _SetAttr(prim, "usdGen:tonic:sectionDeltas",
                     SdfValueTypeNames->Float2Array, sectionDeltas) &&
            _SetAttr(prim, "usdGen:tonic:subdivide:count",
                     SdfValueTypeNames->Int, tube.subdivide.count) &&
            _SetAttr(prim, "usdGen:tonic:subdivide:seed",
                     SdfValueTypeNames->Int, tube.subdivide.seed) &&
            _SetAttr(prim, "usdGen:tonic:subdivide:splitMode",
                     SdfValueTypeNames->Token,
                     TfToken(tube.subdivide.splitMode)) &&
            _SetAttr(prim, "usdGen:tonic:fill:density",
                     SdfValueTypeNames->Float, tube.fill.density) &&
            _SetAttr(prim, "usdGen:tonic:fill:cvCount", SdfValueTypeNames->Int,
                     tube.fill.cvCount) &&
            _SetAttr(prim, "usdGen:tonic:fill:lengthProfile",
                     SdfValueTypeNames->FloatArray, lengthProfile) &&
            _SetAttr(prim, "usdGen:tonic:fill:seed", SdfValueTypeNames->Int,
                     tube.fill.seed) &&
            _SetAttr(prim, "usdGen:tonic:fill:edgeBias",
                     SdfValueTypeNames->Float, tube.fill.edgeBias) &&
            _SetAttr(prim, "usdGen:tonic:locked", SdfValueTypeNames->Bool,
                     tube.locked || entry.imported) &&
            _SetAttr(prim, "usdGen:tonic:lockParents", SdfValueTypeNames->Bool,
                     tube.lockParents) &&
            _SetAttr(prim, "usdGen:tonic:lockChildren",
                     SdfValueTypeNames->Bool, tube.lockChildren);
        if (!attrsOk) {
            return fail("TonicBuildCommitLayer: cannot author tube attributes");
        }
        // An on-the-fly parent the artist kept carries the members it was
        // built from (plan/17 §5.4 "Make persistent"); transient ones never
        // reach the snapshot at all.
        if (!entry.members.empty() || entry.persistent) {
            SdfPathVector members;
            for (int member : entry.members) {
                auto const it = tubePaths.find(member);
                if (it != tubePaths.end()) {
                    members.push_back(it->second);
                }
            }
            if (!_PrependApiSchema(prim, "UsdGenTubeHierarchyAPI") ||
                !_SetRelTargets(prim, "usdGen:tonic:members", members) ||
                !_SetAttr(prim, "usdGen:tonic:persistent",
                          SdfValueTypeNames->Bool, entry.persistent)) {
                return fail("TonicBuildCommitLayer: cannot author the tube "
                            "hierarchy API");
            }
        }
    }

    // -- scalp graph (P2: the shared planar graph, polygon-style) ---------
    {
        SdfPrimSpecHandle graphPrim =
            _EnsurePrim(layer, paths.ScalpGraphPath(), SdfSpecifierDef,
                        "UsdGenScalpGraph");
        if (!graphPrim) {
            return fail(
                "TonicBuildCommitLayer: cannot author the scalp graph");
        }
        // Nodes are parallel arrays; edges and region loops name node
        // INDICES into them (0-based), not the model's stable ids: the
        // stage encoding is positional, the stable ids are model-side.
        TonicModel::GraphSnapshot const &gs = snapshot.graph;
        std::map<int, int> indexOf;
        VtIntArray nodeFaceIds;
        VtVec2fArray nodeUVs;
        nodeFaceIds.reserve(gs.nodes.size());
        nodeUVs.reserve(gs.nodes.size());
        for (size_t i = 0; i < gs.nodes.size(); ++i) {
            indexOf[gs.nodes[i].id] = int(i);
            nodeFaceIds.push_back(gs.nodes[i].faceId);
            nodeUVs.push_back(
                GfVec2f(gs.nodes[i].u, gs.nodes[i].v));
        }
        VtVec2iArray edges;
        edges.reserve(gs.edges.size());
        for (auto const &pr : gs.edges) {
            auto ia = indexOf.find(pr.first);
            auto ib = indexOf.find(pr.second);
            if (ia == indexOf.end() || ib == indexOf.end()) {
                return fail("TonicBuildCommitLayer: graph edge names an "
                            "absent node");
            }
            edges.push_back(GfVec2i(ia->second, ib->second));
        }
        VtIntArray regionCounts;
        VtIntArray regionIndices;
        regionCounts.reserve(gs.regionLoops.size());
        for (auto const &loop : gs.regionLoops) {
            regionCounts.push_back(int(loop.size()));
            for (int nid : loop) {
                auto it = indexOf.find(nid);
                if (it == indexOf.end()) {
                    return fail("TonicBuildCommitLayer: region loop names "
                                "an absent node");
                }
                regionIndices.push_back(it->second);
            }
        }
        VtVec3fArray regionColors;
        regionColors.reserve(gs.regionLoops.size());
        for (size_t i = 0; i < gs.regionColors.size() / 3; ++i) {
            regionColors.push_back(
                GfVec3f(gs.regionColors[i * 3], gs.regionColors[i * 3 + 1],
                        gs.regionColors[i * 3 + 2]));
        }
        VtVec2iArray linked;
        linked.reserve(gs.linked.size());
        for (auto const &pr : gs.linked) {
            linked.push_back(GfVec2i(pr.first, pr.second));
        }
        bool graphOk =
            _SetAttr(graphPrim, "usdGen:tonic:nodeFaceIds",
                     SdfValueTypeNames->IntArray, nodeFaceIds) &&
            _SetAttr(graphPrim, "usdGen:tonic:nodeUVs",
                     SdfValueTypeNames->Float2Array, nodeUVs) &&
            _SetAttr(graphPrim, "usdGen:tonic:edges",
                     SdfValueTypeNames->Int2Array, edges) &&
            _SetAttr(graphPrim, "usdGen:tonic:regionNodeCounts",
                     SdfValueTypeNames->IntArray, regionCounts) &&
            _SetAttr(graphPrim, "usdGen:tonic:regionNodeIndices",
                     SdfValueTypeNames->IntArray, regionIndices) &&
            _SetAttr(graphPrim, "usdGen:tonic:regionColors",
                     SdfValueTypeNames->Color3fArray, regionColors) &&
            _SetAttr(graphPrim, "usdGen:tonic:linkedRegions",
                     SdfValueTypeNames->Int2Array, linked) &&
            _SetAttr(graphPrim, "usdGen:tonic:snapRadius",
                     SdfValueTypeNames->Float, gs.snapRadius);
        if (!graphOk) {
            return fail("TonicBuildCommitLayer: cannot author graph fields");
        }
    }

    // -- live region primvar (P2: the preview map, every swap) -------------
    // A per-face int primvar on the scalp mesh. The Ptex is the contract;
    // this primvar is the live preview the HUD and the bake read from.
    if (!snapshot.scalpPath.IsEmpty() &&
        !snapshot.graph.faceRegions.empty()) {
        SdfPrimSpecHandle scalp =
            _EnsurePrim(layer, snapshot.scalpPath, SdfSpecifierOver, nullptr);
        if (!scalp) {
            return fail("TonicBuildCommitLayer: cannot over the scalp mesh");
        }
        VtIntArray regions;
        regions.assign(snapshot.graph.faceRegions.begin(),
                       snapshot.graph.faceRegions.end());
        if (!_SetAttr(scalp, "primvars:usdGen:tonicRegion",
                      SdfValueTypeNames->IntArray, regions,
                      SdfVariabilityUniform, /*custom*/ true) ||
            !_SetInterpolation(scalp, "primvars:usdGen:tonicRegion",
                               "uniform")) {
            return fail("TonicBuildCommitLayer: cannot author the live "
                        "region primvar");
        }
    }

    // -- committed guides (the existing usdGen guide contract) --------------
    //
    // One fill per LEAF tube (§2.3: a subdivided parent's own fill is
    // suspended, a bridge import carries explicit geometry instead), in the
    // snapshot's parents-first order. Hydrate re-runs the same function over
    // the hydrated model and memcmps the result, so the two cannot drift.
    TonicSnapshotGuides const guideSet =
        TonicGuidesFromSnapshot(snapshot, cache);
    VtVec3fArray points;
    points.resize(guideSet.points.size() / 3);
    for (size_t i = 0; i < points.size(); ++i) {
        points[i] = GfVec3f(guideSet.points[i * 3], guideSet.points[i * 3 + 1],
                            guideSet.points[i * 3 + 2]);
    }
    VtIntArray const counts(guideSet.counts.begin(), guideSet.counts.end());
    VtUInt64Array const ids(guideSet.ids.begin(), guideSet.ids.end());
    VtIntArray const tubeIds(guideSet.tubeIds.begin(), guideSet.tubeIds.end());
    VtIntArray const levels(guideSet.levels.begin(), guideSet.levels.end());
    VtIntArray const regionIds(guideSet.regionIds.begin(),
                               guideSet.regionIds.end());
    VtMatrix4dArray frames;
    frames.resize(guideSet.frames.size() / 16);
    for (size_t g = 0; g < frames.size(); ++g) {
        double const *src = &guideSet.frames[g * 16];
        frames[g].Set(src[0], src[1], src[2], src[3], src[4], src[5], src[6],
                      src[7], src[8], src[9], src[10], src[11], src[12],
                      src[13], src[14], src[15]);
    }
    SdfPrimSpecHandle guidesPrim = _EnsurePrim(layer, paths.GuidesPath(),
                                               SdfSpecifierDef, "BasisCurves");
    if (!guidesPrim) {
        return fail("TonicBuildCommitLayer: cannot author the Guides prim");
    }
    bool guidesOk =
        _PrependApiSchema(guidesPrim, "UsdGenCurveAPI") &&
        _SetAttr(guidesPrim, "type", SdfValueTypeNames->Token, TfToken("cubic"),
                 SdfVariabilityUniform) &&
        _SetAttr(guidesPrim, "basis", SdfValueTypeNames->Token,
                 TfToken("bspline"), SdfVariabilityUniform) &&
        _SetAttr(guidesPrim, "wrap", SdfValueTypeNames->Token, TfToken("pinned"),
                 SdfVariabilityUniform) &&
        _SetAttr(guidesPrim, "curveVertexCounts", SdfValueTypeNames->IntArray,
                 counts) &&
        _SetAttr(guidesPrim, "points", SdfValueTypeNames->Point3fArray,
                 points) &&
        _SetAttr(guidesPrim, "primvars:usdGen:role", SdfValueTypeNames->Token,
                 TfToken("guide"), SdfVariabilityUniform) &&
        _SetAttr(guidesPrim, "primvars:usdGen:curveId",
                 SdfValueTypeNames->UInt64Array, ids,
                 SdfVariabilityUniform) &&
        _SetAttr(guidesPrim, "primvars:usdGen:rootFrame",
                 SdfValueTypeNames->Matrix4dArray, frames,
                 SdfVariabilityUniform) &&
        _SetAttr(guidesPrim, "primvars:tubeId", SdfValueTypeNames->IntArray,
                 tubeIds, SdfVariabilityUniform, /*custom*/ true) &&
        _SetAttr(guidesPrim, "primvars:hierarchyLevel",
                 SdfValueTypeNames->IntArray, levels, SdfVariabilityUniform,
                 /*custom*/ true) &&
        _SetAttr(guidesPrim, "primvars:regionId", SdfValueTypeNames->IntArray,
                 regionIds, SdfVariabilityUniform, /*custom*/ true) &&
        _SetInterpolation(guidesPrim, "primvars:usdGen:curveId", "uniform") &&
        _SetInterpolation(guidesPrim, "primvars:usdGen:rootFrame", "uniform") &&
        _SetInterpolation(guidesPrim, "primvars:tubeId", "uniform") &&
        _SetInterpolation(guidesPrim, "primvars:hierarchyLevel", "uniform") &&
        _SetInterpolation(guidesPrim, "primvars:regionId", "uniform");
    if (!guidesOk) {
        return fail("TonicBuildCommitLayer: cannot author guide attributes");
    }

    // -- region maps + expressions (P2: one level; P4: per level) ---------
    // The versioned .ptx is written by the bake worker; the §3.1 build
    // re-authors the CURRENT file (from the snapshot) so a guide swap never
    // wipes the baked reference. The bake swap is what advances it.
    int const levelCount = std::max(snapshot.graph.levelCount, 1);
    for (int level = 1; level <= levelCount; ++level) {
        SdfPath const mapPath = paths.LevelMapPath(level);
        SdfPath const exprPath = paths.LevelExprPath(level);
        SdfPrimSpecHandle regionMap =
            _EnsurePrim(layer, mapPath, SdfSpecifierDef, "UsdGenPtexMap");
        if (!regionMap) {
            return fail(
                "TonicBuildCommitLayer: cannot author the RegionMap");
        }
        // usdGen:map:clamp is (0, 0): region ids are -1..N, and both the
        // schema and the evaluator default to [0, 1], which would merge
        // uncovered strands into region 0. Equal components disable it.
        bool mapOk =
            _SetAttr(regionMap, "usdGen:map:filter", SdfValueTypeNames->Token,
                     TfToken("nearest")) &&
            _SetAttr(regionMap, "usdGen:map:firstChannel",
                     SdfValueTypeNames->Int, level - 1) &&
            _SetAttr(regionMap, "usdGen:map:channelCount",
                     SdfValueTypeNames->Int, 1) &&
            _SetAttr(regionMap, "usdGen:map:clamp",
                     SdfValueTypeNames->Float2, GfVec2f(0.0f, 0.0f));
        if (!snapshot.graph.bakedMapFile.empty()) {
            mapOk = mapOk &&
                    _SetAttr(regionMap, "usdGen:map:file",
                             SdfValueTypeNames->Asset,
                             SdfAssetPath(snapshot.graph.bakedMapFile));
        }
        if (!mapOk) {
            return fail(
                "TonicBuildCommitLayer: cannot author RegionMap fields");
        }
        SdfPrimSpecHandle regionExpr = _EnsurePrim(
            layer, exprPath, SdfSpecifierDef, "UsdGenExpression");
        if (!regionExpr) {
            return fail(
                "TonicBuildCommitLayer: cannot author the RegionExpr");
        }
        SdfAttributeSpecHandle outputsResult = SdfAttributeSpec::New(
            regionExpr, "outputs:result", SdfValueTypeNames->Float,
            SdfVariabilityVarying, /*custom*/ true);
        if (!outputsResult) {
            return fail(
                "TonicBuildCommitLayer: cannot author outputs:result");
        }
        if (!_SetAttr(regionExpr, "usdGen:expr:source",
                      SdfValueTypeNames->String,
                      std::string("ptex(\"regionMap\")"),
                      SdfVariabilityUniform, /*custom*/ true) ||
            !_SetRelTargets(regionExpr, "input:regionMap",
                            SdfPathVector{mapPath})) {
            return fail(
                "TonicBuildCommitLayer: cannot author the RegionExpr");
        }
    }

    // -- GuideInterpolate fill-in (only what the plan allows) ---------------
    if (!paths.descriptionPath.IsEmpty() &&
        (snapshot.createInterpOp || snapshot.setInterpGuides ||
         snapshot.setInterpRegion)) {
        SdfPrimSpecHandle op;
        if (snapshot.createInterpOp) {
            if (!_EnsurePrim(layer, paths.descriptionPath, SdfSpecifierOver,
                             nullptr)) {
                return fail("TonicBuildCommitLayer: cannot over the "
                            "description");
            }
            SdfPath const opsPath =
                paths.descriptionPath.AppendChild(TfToken("Ops"));
            if (!_EnsurePrim(layer, opsPath, SdfSpecifierDef, "Scope")) {
                return fail("TonicBuildCommitLayer: cannot author the Ops "
                            "scope");
            }
            op = _EnsurePrim(layer, snapshot.interpOpPath, SdfSpecifierDef,
                             "UsdGenGuideInterpolate");
        } else {
            op = _EnsurePrim(layer, snapshot.interpOpPath, SdfSpecifierOver,
                             nullptr);
        }
        if (!op) {
            return fail("TonicBuildCommitLayer: cannot author the "
                        "GuideInterpolate op");
        }
        if (snapshot.setInterpGuides &&
            !_SetRelTargets(op, "usdGen:guides",
                            SdfPathVector{paths.GuidesPath()})) {
            return fail("TonicBuildCommitLayer: cannot connect usdGen:guides");
        }
        if (snapshot.setInterpRegion &&
            !_SetConnections(op, "usdGen:region", SdfValueTypeNames->Float,
                            SdfPathVector{paths.RegionExprPath()})) {
            return fail("TonicBuildCommitLayer: cannot connect usdGen:region");
        }
    }

    *outLayer = layer;
    return true;
}

TonicFillPlan
TonicPlanGuideInterpolateFill(UsdStagePtr const &stage,
                              TonicCommitPaths const &paths)
{
    TonicFillPlan plan;
    plan.opPath = paths.InterpOpPath();
    if (!stage || paths.descriptionPath.IsEmpty()) {
        return plan;
    }
    UsdPrim const desc = stage->GetPrimAtPath(paths.descriptionPath);
    if (!desc) {
        return plan;
    }
    // Any UsdGenGuideInterpolate under <desc>/Ops counts, whatever its name:
    // the tool fills in, it never authors a second op beside an artist's.
    UsdPrim existing;
    UsdPrim const ops =
        stage->GetPrimAtPath(paths.descriptionPath.AppendChild(TfToken("Ops")));
    if (ops) {
        for (UsdPrim const &child : ops.GetChildren()) {
            if (child.GetTypeName() == TfToken("UsdGenGuideInterpolate")) {
                existing = child;
                break;
            }
        }
    }
    if (!existing) {
        plan.createInterpOp = true;
        plan.setInterpGuides = true;
        plan.setInterpRegion = true;
        return plan;
    }
    plan.opPath = existing.GetPath();
    if (_Targets(existing.GetRelationship(TfToken("usdGen:guides"))).empty()) {
        plan.setInterpGuides = true;
    }
    UsdAttribute const region =
        existing.GetAttribute(TfToken("usdGen:region"));
    if (!region.HasAuthoredValueOpinion() &&
        _Connections(region).empty()) {
        plan.setInterpRegion = true;
    }
    return plan;
}

TonicHydrateResult
TonicHydrateModel(UsdStagePtr const &stage, SdfPath const &groomPath,
                  TonicModel *model)
{
    TonicHydrateResult result;
    auto fail = [&](std::string const &what) {
        result.diagnostic = "TonicHydrateModel: " + what;
        return result;
    };
    if (!stage || !model) {
        return fail("null stage or model");
    }
    UsdPrim const groom = stage->GetPrimAtPath(groomPath);
    if (!groom || groom.GetTypeName() != TfToken("UsdGenTonicGroom")) {
        return fail("no UsdGenTonicGroom at " + groomPath.GetString());
    }
    TfToken version;
    if (!groom.GetAttribute(TfToken("usdGen:tonic:version")).Get(&version) ||
        version != "1") {
        return fail("unsupported groom version (want \"1\")");
    }
    UsdPrim const graph =
        stage->GetPrimAtPath(groomPath.AppendChild(TfToken("ScalpGraph")));
    if (!graph || graph.GetTypeName() != TfToken("UsdGenScalpGraph")) {
        return fail("groom has no UsdGenScalpGraph shell");
    }
    // -- P2 graph hydrate -------------------------------------------------
    // The stage encoding is positional (nodes are parallel arrays; edges and
    // loops name indices); the model takes stable ids, so hydrate maps index
    // i to id i and verifies the re-extracted loops, colours and live
    // primvar against the stored opinions bit-exactly.
    VtIntArray nodeFaceIds;
    VtVec2fArray nodeUVs;
    VtVec2iArray storedEdges;
    VtIntArray regionCounts;
    VtIntArray regionIndices;
    VtVec3fArray storedColors;
    VtVec2iArray storedLinked;
    float storedSnapRadius = 0.05f;
    graph.GetAttribute(TfToken("usdGen:tonic:nodeFaceIds")).Get(&nodeFaceIds);
    graph.GetAttribute(TfToken("usdGen:tonic:nodeUVs")).Get(&nodeUVs);
    graph.GetAttribute(TfToken("usdGen:tonic:edges")).Get(&storedEdges);
    graph.GetAttribute(TfToken("usdGen:tonic:regionNodeCounts"))
        .Get(&regionCounts);
    graph.GetAttribute(TfToken("usdGen:tonic:regionNodeIndices"))
        .Get(&regionIndices);
    graph.GetAttribute(TfToken("usdGen:tonic:regionColors")).Get(&storedColors);
    graph.GetAttribute(TfToken("usdGen:tonic:linkedRegions")).Get(&storedLinked);
    graph.GetAttribute(TfToken("usdGen:tonic:snapRadius")).Get(&storedSnapRadius);
    if (nodeUVs.size() != nodeFaceIds.size()) {
        return fail("graph node arrays have mismatched lengths");
    }
    size_t loopTotal = 0;
    for (int c : regionCounts) {
        if (c < 0) {
            return fail("graph region counts are negative");
        }
        loopTotal += size_t(c);
    }
    if (loopTotal != regionIndices.size()) {
        return fail("graph region indices do not match the counts");
    }
    SdfPathVector scalpTargets =
        _Targets(groom.GetRelationship(TfToken("usdGen:tonic:scalp")));
    bool haveScalp = false;
    UsdPrim scalpPrim;
    if (!scalpTargets.empty()) {
        scalpPrim = stage->GetPrimAtPath(scalpTargets[0]);
        UsdGeomMesh const scalpMesh(scalpPrim);
        if (!scalpPrim || !scalpMesh) {
            return fail("scalp link names no mesh prim");
        }
        VtVec3fArray pts;
        VtIntArray counts, indices;
        if (!scalpMesh.GetPointsAttr().Get(&pts) ||
            !scalpMesh.GetFaceVertexCountsAttr().Get(&counts) ||
            !scalpMesh.GetFaceVertexIndicesAttr().Get(&indices)) {
            return fail("scalp mesh has no readable topology");
        }
        std::vector<float> points(pts.size() * 3);
        for (size_t i = 0; i < pts.size(); ++i) {
            points[i * 3 + 0] = pts[i][0];
            points[i * 3 + 1] = pts[i][1];
            points[i * 3 + 2] = pts[i][2];
        }
        if (!model->BindScalp(points,
                              std::vector<int>(counts.begin(), counts.end()),
                              std::vector<int>(indices.begin(),
                                               indices.end()))) {
            return fail(model->GetDiagnostic());
        }
        haveScalp = true;
    }
    if (!nodeFaceIds.empty()) {
        if (!haveScalp) {
            return fail("graph nodes but no scalp link");
        }
        std::shared_ptr<TonicScalpMesh const> scalp = model->GetScalp();
        TonicModel::GraphSnapshot gs;
        gs.snapRadius = storedSnapRadius;
        for (size_t i = 0; i < nodeFaceIds.size(); ++i) {
            TonicGraphNode nd;
            nd.id = int(i);
            nd.faceId = nodeFaceIds[i];
            nd.u = nodeUVs[i][0];
            nd.v = nodeUVs[i][1];
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            if (!TonicFacePosition(*scalp, nd.faceId, nd.u, nd.v, &px, &py,
                                   &pz)) {
                return fail("graph node addresses no surface point");
            }
            nd.p[0] = px;
            nd.p[1] = py;
            nd.p[2] = pz;
            if (size_t(nd.faceId) >= scalp->faceVertexCounts.size()) {
                return fail("graph node face is out of range");
            }
            nd.n[0] = scalp->faceNormals[size_t(nd.faceId) * 3 + 0];
            nd.n[1] = scalp->faceNormals[size_t(nd.faceId) * 3 + 1];
            nd.n[2] = scalp->faceNormals[size_t(nd.faceId) * 3 + 2];
            gs.nodes.push_back(nd);
        }
        for (auto const &e : storedEdges) {
            if (e[0] < 0 || e[1] < 0 ||
                size_t(e[0]) >= gs.nodes.size() ||
                size_t(e[1]) >= gs.nodes.size()) {
                return fail("graph edge names absent nodes");
            }
            gs.edges.emplace_back(e[0], e[1]);
        }
        for (auto const &pr : storedLinked) {
            if (pr[0] < 0 || pr[1] < 0 ||
                size_t(pr[0]) >= regionCounts.size() ||
                size_t(pr[1]) >= regionCounts.size()) {
                return fail("linked regions name absent regions");
            }
            gs.linked.emplace_back(pr[0], pr[1]);
        }
        if (!model->RestoreGraph(gs)) {
            return fail(model->GetDiagnostic());
        }
        // The graph round-trip asserts: re-extracted loops, colours and the
        // re-rasterised live primvar equal the stored opinions bit-exactly.
        TonicModel::GraphSnapshot check = model->SnapshotGraph();
        bool loopsEqual =
            check.regionLoops.size() == regionCounts.size();
        size_t flat = 0;
        for (size_t r = 0; loopsEqual && r < check.regionLoops.size(); ++r) {
            loopsEqual = check.regionLoops[r].size() ==
                         size_t(regionCounts[r]);
            for (size_t k = 0; loopsEqual && k < check.regionLoops[r].size();
                 ++k, ++flat) {
                loopsEqual =
                    check.regionLoops[r][k] == regionIndices[flat];
            }
        }
        loopsEqual = loopsEqual && flat == regionIndices.size();
        bool colorsEqual =
            check.regionColors.size() == storedColors.size() * 3;
        for (size_t i = 0; colorsEqual && i < storedColors.size(); ++i) {
            colorsEqual =
                check.regionColors[i * 3 + 0] == storedColors[i][0] &&
                check.regionColors[i * 3 + 1] == storedColors[i][1] &&
                check.regionColors[i * 3 + 2] == storedColors[i][2];
        }
        if (!loopsEqual || !colorsEqual) {
            return fail("re-extracted regions differ from the stored graph "
                        "(foreign or hand-edited graph data)");
        }
        result.graphNodeCount = check.nodes.size();
        result.graphRegionCount = check.regionLoops.size();
    }
    if (haveScalp && bool(scalpPrim)) {
        // The live primvar must equal the re-rasterised map bit-exactly
        // (empty graphs rasterise to all -1, which the committer authors
        // whenever a scalp is bound).
        VtIntArray storedPrimvar;
        bool const hasPrimvar =
            scalpPrim.GetAttribute(TfToken("primvars:usdGen:tonicRegion"))
                .Get(&storedPrimvar);
        TonicModel::GraphSnapshot check = model->SnapshotGraph();
        if (hasPrimvar &&
            (storedPrimvar.size() != check.faceRegions.size() ||
             !std::equal(storedPrimvar.begin(), storedPrimvar.end(),
                         check.faceRegions.begin()))) {
            return fail("re-rasterised regions differ from the stored "
                        "primvar (foreign or hand-edited map data)");
        }
    }
    result.graphRoundTrip = true;

    // -- V0b: the whole tube hierarchy (plan/17 §2.5, plan/18 §7 G3) -------
    //
    // Every level is REBUILT, never read as geometry: the L1 tube is
    // restored outright, each deeper level is re-derived from its parent
    // with the stored (tubeId, seed) subdivision and then takes the stored
    // shape + deltas verbatim. Bridge imports come back through
    // ImportLockedTube, on-the-fly parents through their members.
    UsdPrim const tubes =
        stage->GetPrimAtPath(groomPath.AppendChild(TfToken("Tubes")));
    if (!tubes) {
        return fail("groom has no Tubes scope");
    }
    struct _StageTube {
        UsdPrim prim;
        int level = 1;
        int childIndex = -1;
        int regionId = -1;
        bool locked = false;  // a child's locked bit marks a bridge import
        bool persistent = false;
        SdfPathVector members;
        TonicModel::TubeSnapshot shape;
        TonicShapeDeltas deltas;
    };
    auto readTube = [&](UsdPrim const &prim, _StageTube *out) -> std::string {
        VtVec3fArray centers;
        VtFloatArray sectionT;
        int ringVerts = 0;
        VtVec2fArray sectionCvs;
        if (!prim.GetAttribute(TfToken("usdGen:tonic:centerPoints"))
                 .Get(&centers) ||
            centers.empty() ||
            !prim.GetAttribute(TfToken("usdGen:tonic:sectionT"))
                 .Get(&sectionT) ||
            !prim.GetAttribute(TfToken("usdGen:tonic:sectionCvCount"))
                 .Get(&ringVerts) ||
            !prim.GetAttribute(TfToken("usdGen:tonic:sectionCvs"))
                 .Get(&sectionCvs)) {
            return "tube is missing center/section attributes";
        }
        if (sectionCvs.empty()) {
            return "tube has no section CVs";
        }
        if (sectionCvs.size() != sectionT.size() * size_t(ringVerts)) {
            return "tube section CVs do not match sectionT x cvCount";
        }
        TonicModel::TubeSnapshot &snapshot = out->shape;
        snapshot.hasTube = true;
        snapshot.shape.rings = int(centers.size());
        snapshot.shape.ringVerts = ringVerts;
        // Radius is the section-plane extent of the first ring CV; length
        // spans the center column. Both round-trip the committed tube.
        snapshot.shape.radius = std::sqrt(sectionCvs[0][0] * sectionCvs[0][0] +
                                          sectionCvs[0][1] * sectionCvs[0][1]);
        snapshot.centerX.resize(centers.size());
        snapshot.centerY.resize(centers.size());
        snapshot.centerZ.resize(centers.size());
        for (size_t i = 0; i < centers.size(); ++i) {
            snapshot.centerX[i] = centers[i][0];
            snapshot.centerY[i] = centers[i][1];
            snapshot.centerZ[i] = centers[i][2];
        }
        snapshot.shape.length = centers.back()[1] - centers.front()[1];
        // Stored CVs carry baked scale/twist (see the layer builder), so
        // hydrate restores scale 1 / twist 0 with identical placement.
        snapshot.sections.resize(sectionT.size());
        for (size_t r = 0; r < sectionT.size(); ++r) {
            TonicTubeSection section;
            section.t = sectionT[r];
            section.u.resize(size_t(ringVerts));
            section.v.resize(size_t(ringVerts));
            for (int s = 0; s < ringVerts; ++s) {
                GfVec2f const cv =
                    sectionCvs[r * size_t(ringVerts) + size_t(s)];
                section.u[size_t(s)] = cv[0];
                section.v[size_t(s)] = cv[1];
            }
            snapshot.sections[r] = std::move(section);
        }
        prim.GetAttribute(TfToken("usdGen:tonic:regionId")).Get(&out->regionId);
        prim.GetAttribute(TfToken("usdGen:tonic:level")).Get(&out->level);
        prim.GetAttribute(TfToken("usdGen:tonic:childIndex"))
            .Get(&out->childIndex);
        prim.GetAttribute(TfToken("usdGen:tonic:fill:density"))
            .Get(&snapshot.fill.density);
        prim.GetAttribute(TfToken("usdGen:tonic:fill:cvCount"))
            .Get(&snapshot.fill.cvCount);
        prim.GetAttribute(TfToken("usdGen:tonic:fill:seed"))
            .Get(&snapshot.fill.seed);
        prim.GetAttribute(TfToken("usdGen:tonic:fill:edgeBias"))
            .Get(&snapshot.fill.edgeBias);
        VtFloatArray lengthProfile;
        if (prim.GetAttribute(TfToken("usdGen:tonic:fill:lengthProfile"))
                .Get(&lengthProfile)) {
            snapshot.fill.lengthProfile.assign(lengthProfile.begin(),
                                               lengthProfile.end());
        }
        prim.GetAttribute(TfToken("usdGen:tonic:subdivide:count"))
            .Get(&snapshot.subdivide.count);
        prim.GetAttribute(TfToken("usdGen:tonic:subdivide:seed"))
            .Get(&snapshot.subdivide.seed);
        TfToken splitMode;
        if (prim.GetAttribute(TfToken("usdGen:tonic:subdivide:splitMode"))
                .Get(&splitMode)) {
            snapshot.subdivide.splitMode = splitMode.GetString();
        }
        prim.GetAttribute(TfToken("usdGen:tonic:locked")).Get(&snapshot.locked);
        prim.GetAttribute(TfToken("usdGen:tonic:lockParents"))
            .Get(&snapshot.lockParents);
        prim.GetAttribute(TfToken("usdGen:tonic:lockChildren"))
            .Get(&snapshot.lockChildren);
        out->locked = snapshot.locked;
        prim.GetAttribute(TfToken("usdGen:tonic:persistent"))
            .Get(&out->persistent);
        out->members =
            _Targets(prim.GetRelationship(TfToken("usdGen:tonic:members")));
        VtVec3fArray centerDeltas;
        VtVec2fArray sectionDeltas;
        prim.GetAttribute(TfToken("usdGen:tonic:centerDeltas"))
            .Get(&centerDeltas);
        prim.GetAttribute(TfToken("usdGen:tonic:sectionDeltas"))
            .Get(&sectionDeltas);
        for (auto const &d : centerDeltas) {
            out->deltas.centerDu.push_back(d[0]);
            out->deltas.centerDv.push_back(d[1]);
            out->deltas.centerDw.push_back(d[2]);
        }
        if (!sectionDeltas.empty() &&
            sectionDeltas.size() == sectionT.size() * size_t(ringVerts)) {
            out->deltas.sections.resize(sectionT.size());
            for (size_t r = 0; r < sectionT.size(); ++r) {
                TonicTubeSection &sec = out->deltas.sections[r];
                sec.t = sectionT[r];
                sec.scale = 0.0f;
                sec.twist = 0.0f;
                sec.u.resize(size_t(ringVerts));
                sec.v.resize(size_t(ringVerts));
                for (int s = 0; s < ringVerts; ++s) {
                    GfVec2f const d =
                        sectionDeltas[r * size_t(ringVerts) + size_t(s)];
                    sec.u[size_t(s)] = d[0];
                    sec.v[size_t(s)] = d[1];
                }
            }
        }
        out->prim = prim;
        return std::string();
    };
    auto tubeChildren = [](UsdPrim const &prim) {
        std::vector<UsdPrim> out;
        for (UsdPrim const &child : prim.GetChildren()) {
            if (child.GetTypeName() == TfToken("UsdGenTube")) {
                out.push_back(child);
            }
        }
        return out;
    };

    std::vector<UsdPrim> const roots = tubeChildren(tubes);
    // V6: one L1 root per region (plan/18 §7 G14). Their ids are carried by
    // the prim names the committer wrote (`tube<id>`), which is what keeps a
    // hydrated groom's guide tubeIds equal to the committed ones.
    std::vector<std::pair<int, UsdPrim>> l1Prims;
    std::vector<UsdPrim> groupPrims;
    for (UsdPrim const &root : roots) {
        SdfPathVector const members =
            _Targets(root.GetRelationship(TfToken("usdGen:tonic:members")));
        if (!members.empty()) {
            groupPrims.push_back(root);
            continue;
        }
        std::string const name = root.GetName().GetString();
        int id = -1;
        if (name.rfind("tube", 0) == 0 && name.size() > 4) {
            char *end = nullptr;
            long const parsed = std::strtol(name.c_str() + 4, &end, 10);
            if (end && *end == 0 && parsed >= 0 && parsed < (1 << 24)) {
                id = int(parsed);
            }
        }
        if (id < 0 || (id % 16) != 0) {
            return fail("an L1 UsdGenTube is not named tube<id> with an L1 "
                        "root id (0, or a multiple of 16)");
        }
        l1Prims.emplace_back(id, root);
    }
    if (l1Prims.empty()) {
        return fail("groom has no L1 UsdGenTube");
    }
    std::sort(l1Prims.begin(), l1Prims.end(),
              [](std::pair<int, UsdPrim> const &a,
                 std::pair<int, UsdPrim> const &b) {
                  return a.first < b.first;
              });
    if (l1Prims.front().first != 0) {
        return fail("the groom's L1 roots do not include tube0");
    }
    for (size_t i = 1; i < l1Prims.size(); ++i) {
        if (l1Prims[i].first == l1Prims[i - 1].first) {
            return fail("two L1 UsdGenTubes carry the same id");
        }
    }
    // Group prims mint negative ids in name order, which is the order the
    // committer wrote them: hydrating the same way re-mints the same ids.
    std::sort(groupPrims.begin(), groupPrims.end(),
              [](UsdPrim const &a, UsdPrim const &b) {
                  return a.GetPath() < b.GetPath();
              });

    UsdPrim const l1Prim = l1Prims.front().second;
    _StageTube l1;
    if (std::string const rerr = readTube(l1Prim, &l1); !rerr.empty()) {
        return fail(rerr);
    }
    model->ClearHierarchy();
    if (!model->Restore(l1.shape)) {
        return fail(model->GetDiagnostic());
    }
    model->SetTubeRegionId(l1.regionId);
    model->SetFreezeRoots(false);
    result.tubeCount = 1;

    // Breadth-first: a parent has to exist (and be subdivided) before its
    // children can be restored.
    std::map<SdfPath, int> idOfPath;
    idOfPath[l1Prim.GetPath()] = 0;
    std::vector<std::pair<UsdPrim, int>> queue{{l1Prim, 0}};
    for (size_t i = 1; i < l1Prims.size(); ++i) {
        _StageTube extra;
        if (std::string const rerr = readTube(l1Prims[i].second, &extra);
            !rerr.empty()) {
            return fail(rerr);
        }
        TonicModel::TubeRecord install;
        install.actual = TonicTubeDescFromSnapshot(extra.shape);
        install.actual.regionId = extra.regionId;
        install.subdivide = extra.shape.subdivide;
        install.fill = extra.shape.fill;
        install.lockParents = extra.shape.lockParents;
        install.lockChildren = extra.shape.lockChildren;
        install.hasTube = true;
        if (!model->InstallL1Tube(l1Prims[i].first, install)) {
            return fail(model->GetDiagnostic());
        }
        idOfPath[l1Prims[i].second.GetPath()] = l1Prims[i].first;
        ++result.tubeCount;
        queue.emplace_back(l1Prims[i].second, l1Prims[i].first);
    }
    for (size_t head = 0; head < queue.size(); ++head) {
        UsdPrim const parentPrim = queue[head].first;
        int const parentId = queue[head].second;
        std::vector<UsdPrim> children = tubeChildren(parentPrim);
        if (children.empty()) {
            continue;
        }
        std::vector<_StageTube> records(children.size());
        for (size_t i = 0; i < children.size(); ++i) {
            if (std::string const rerr = readTube(children[i], &records[i]);
                !rerr.empty()) {
                return fail(rerr);
            }
        }
        std::vector<size_t> order(children.size());
        for (size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return records[a].childIndex < records[b].childIndex;
        });
        int subdivided = 0;
        for (size_t i : order) {
            if (!records[i].locked) {
                ++subdivided;
            }
        }
        if (subdivided > 0) {
            std::string splitMode = "kmeans";
            int seed = 0;
            int parentCount = 0;
            if (parentId == 0) {
                splitMode = l1.shape.subdivide.splitMode;
                seed = l1.shape.subdivide.seed;
                parentCount = l1.shape.subdivide.count;
            } else {
                TonicModel::TubeRecord parentRecord;
                if (!model->GetTubeRecord(parentId, &parentRecord)) {
                    return fail("a committed parent tube is missing from the "
                                "model");
                }
                splitMode = parentRecord.subdivide.splitMode;
                seed = parentRecord.subdivide.seed;
                parentCount = parentRecord.subdivide.count;
            }
            if (parentCount != subdivided) {
                return fail("a tube's subdivide:count does not match its "
                            "committed child count (foreign or hand-edited "
                            "hierarchy)");
            }
            std::vector<int> kids;
            if (!model->SubdivideTube(parentId, parentCount, splitMode.c_str(),
                                      seed, &kids)) {
                return fail(model->GetDiagnostic());
            }
        }
        for (size_t i : order) {
            _StageTube const &record = records[i];
            int childId = 0;
            if (record.locked) {
                // A bridge import: explicit geometry, no derivation. It is
                // created after the subdivided siblings, so its childIndex
                // (and therefore its id) continues past them.
                std::vector<float> secT, secU, secV;
                for (auto const &sec : record.shape.sections) {
                    secT.push_back(sec.t);
                    for (size_t s = 0; s < sec.u.size(); ++s) {
                        secU.push_back(sec.u[s]);
                        secV.push_back(sec.v[s]);
                    }
                }
                if (!model->ImportLockedTube(
                        parentId, record.shape.centerX.data(),
                        record.shape.centerY.data(),
                        record.shape.centerZ.data(),
                        int(record.shape.centerX.size()), secT.data(),
                        secU.data(), secV.data(), int(secT.size()),
                        record.shape.shape.ringVerts, &childId)) {
                    return fail(model->GetDiagnostic());
                }
            } else {
                childId = parentId * 16 + 1 + record.childIndex;
            }
            TonicModel::TubeRecord install;
            if (!model->GetTubeRecord(childId, &install)) {
                return fail("a committed child tube has no re-derived "
                            "counterpart (foreign or hand-edited hierarchy)");
            }
            install.actual = TonicTubeDescFromSnapshot(record.shape);
            // regionId is authored per tube, not carried by the shape.
            install.actual.regionId = record.regionId;
            install.deltas = record.deltas;
            install.subdivide = record.shape.subdivide;
            install.fill = record.shape.fill;
            install.lockParents = record.shape.lockParents;
            install.lockChildren = record.shape.lockChildren;
            install.persistent = record.persistent;
            install.imported = record.locked;
            if (!model->RestoreTubeRecord(childId, install)) {
                return fail(model->GetDiagnostic());
            }
            idOfPath[record.prim.GetPath()] = childId;
            ++result.tubeCount;
            queue.emplace_back(record.prim, childId);
        }
    }

    // On-the-fly parents the artist kept (§5.4 "Make persistent").
    for (UsdPrim const &groupPrim : groupPrims) {
        _StageTube group;
        if (std::string const rerr = readTube(groupPrim, &group); !rerr.empty()) {
            return fail(rerr);
        }
        std::vector<int> members;
        for (SdfPath const &member : group.members) {
            auto const it = idOfPath.find(member);
            if (it == idOfPath.end()) {
                return fail("an on-the-fly parent names a tube that is not "
                            "in the groom");
            }
            members.push_back(it->second);
        }
        int groupId = 0;
        if (!model->GroupTubes(members, /*transientParent*/ false, &groupId)) {
            return fail(model->GetDiagnostic());
        }
        if (group.persistent && !model->MakeTubePersistent(groupId)) {
            return fail(model->GetDiagnostic());
        }
        TonicModel::TubeRecord install;
        if (!model->GetTubeRecord(groupId, &install)) {
            return fail("the re-created on-the-fly parent is missing");
        }
        install.actual = TonicTubeDescFromSnapshot(group.shape);
        install.actual.regionId = group.regionId;
        install.subdivide = group.shape.subdivide;
        install.fill = group.shape.fill;
        install.lockParents = group.shape.lockParents;
        install.lockChildren = group.shape.lockChildren;
        install.persistent = group.persistent;
        install.members = members;
        if (!model->RestoreTubeRecord(groupId, install)) {
            return fail(model->GetDiagnostic());
        }
        idOfPath[groupPrim.GetPath()] = groupId;
        ++result.tubeCount;
    }

    // -- guides: regenerate, assert, then import what no tube claims -------
    UsdPrim const guides =
        stage->GetPrimAtPath(groomPath.AppendChild(TfToken("Guides")));
    VtVec3fArray storedPoints;
    VtIntArray storedCounts;
    VtUInt64Array storedIds;
    VtIntArray storedTubeIds;
    bool const guidesReadable =
        bool(guides) &&
        guides.GetAttribute(TfToken("points")).Get(&storedPoints) &&
        guides.GetAttribute(TfToken("curveVertexCounts")).Get(&storedCounts) &&
        guides.GetAttribute(TfToken("primvars:usdGen:curveId")).Get(&storedIds);
    if (!guidesReadable) {
        return fail("groom has no readable Guides prim");
    }
    guides.GetAttribute(TfToken("primvars:tubeId")).Get(&storedTubeIds);
    TonicSnapshot const check = TonicSnapshotFromModel(*model);
    TonicSnapshotGuides const regen = TonicGuidesFromSnapshot(check);
    result.guideCount = regen.counts.size();
    // Stored offsets per curve, and which curves a model tube claims.
    std::vector<size_t> storedOffset(storedCounts.size(), 0);
    size_t running = 0;
    for (size_t i = 0; i < storedCounts.size(); ++i) {
        storedOffset[i] = running;
        running += size_t(std::max(storedCounts[i], 0));
    }
    if (running != storedPoints.size()) {
        return fail("stored Guides counts do not cover its points");
    }
    std::vector<int> knownTubes;
    for (TonicSnapshotTube const &entry : check.tubes) {
        knownTubes.push_back(entry.tubeId);
    }
    std::vector<size_t> claimed, foreign;
    for (size_t i = 0; i < storedCounts.size(); ++i) {
        int const tubeId =
            i < storedTubeIds.size() ? storedTubeIds[i] : -1;
        bool const known =
            i < storedTubeIds.size() &&
            std::find(knownTubes.begin(), knownTubes.end(), tubeId) !=
                knownTubes.end();
        (known ? claimed : foreign).push_back(i);
    }
    bool equal = claimed.size() == regen.counts.size();
    size_t regenOffset = 0;
    for (size_t g = 0; equal && g < claimed.size(); ++g) {
        size_t const s = claimed[g];
        equal = storedCounts[s] == regen.counts[g] &&
                storedIds[s] == regen.ids[g];
        for (int c = 0; equal && c < regen.counts[g]; ++c) {
            equal = std::memcmp(&storedPoints[storedOffset[s] + size_t(c)],
                                &regen.points[(regenOffset + size_t(c)) * 3],
                                sizeof(float) * 3) == 0;
        }
        regenOffset += size_t(regen.counts[g]);
    }
    result.guidesBitEqual = equal;
    if (!equal) {
        return fail("regenerated guides differ from the stored Guides "
                    "(foreign or hand-edited guide data)");
    }
    // Foreign guides (hand-authored, Houdini) are not fill output: they
    // become locked tubes one level under the tube whose region roots them
    // (plan/17 §2.5, §5.7), so the next commit carries them as geometry.
    for (size_t s : foreign) {
        int const nCv = storedCounts[s];
        if (nCv < 2) {
            return fail("a foreign guide has fewer than two CVs");
        }
        std::vector<float> cx, cy, cz;
        for (int c = 0; c < nCv; ++c) {
            GfVec3f const p = storedPoints[storedOffset[s] + size_t(c)];
            cx.push_back(p[0]);
            cy.push_back(p[1]);
            cz.push_back(p[2]);
        }
        float const root[3] = {cx[0], cy[0], cz[0]};
        int parentId = _OwningLeafTube(*model, root);
        TonicTubeDesc parentDesc;
        if (!model->GetTubeDesc(parentId, &parentDesc)) {
            return fail("a foreign guide has no tube to import under");
        }
        std::vector<float> secT, secU, secV;
        for (auto const &sec : parentDesc.sections) {
            secT.push_back(sec.t);
            for (int v = 0; v < parentDesc.ringVerts; ++v) {
                secU.push_back(sec.u[size_t(v)] * sec.scale);
                secV.push_back(sec.v[size_t(v)] * sec.scale);
            }
        }
        int importedId = 0;
        if (!model->ImportLockedTube(parentId, cx.data(), cy.data(), cz.data(),
                                     nCv, secT.data(), secU.data(),
                                     secV.data(), int(secT.size()),
                                     parentDesc.ringVerts, &importedId)) {
            return fail(model->GetDiagnostic());
        }
        ++result.importedTubeCount;
        ++result.tubeCount;
    }
    result.ok = true;
    return result;
}

bool
TonicSaveGroom(UsdStagePtr const &stage, SdfLayerHandle const &live,
              std::string const &filePath, std::string *err)
{
    auto fail = [&](std::string const &what) {
        if (err) {
            *err = "TonicSaveGroom: " + what;
        }
        return false;
    };
    if (!stage || !live) {
        return fail("null stage or live layer");
    }
    if (filePath.size() < 6 ||
        (filePath.compare(filePath.size() - 5, 5, ".usdc") != 0 &&
         filePath.compare(filePath.size() - 5, 5, ".USDC") != 0)) {
        return fail("groom files are .usdc, never .usda (S42)");
    }
    SdfLayerRefPtr file = SdfLayer::FindOrOpen(filePath);
    if (!file) {
        file = SdfLayer::CreateNew(filePath);
    }
    if (!file) {
        return fail("cannot open or create " + filePath);
    }
    file->TransferContent(live);
    if (!file->Save()) {
        return fail("cannot save " + filePath);
    }
    // Re-parent: the live sublayer stays strongest so editing continues
    // there; the saved file sits directly beneath it.
    SdfLayerHandle const session = stage->GetSessionLayer();
    if (!session) {
        return fail("stage has no session layer");
    }
    std::vector<std::string> sublayers;
    sublayers.push_back(live->GetIdentifier());
    sublayers.push_back(file->GetIdentifier());
    for (std::string const &existing : session->GetSubLayerPaths()) {
        if (existing != live->GetIdentifier() &&
            existing != file->GetIdentifier()) {
            sublayers.push_back(existing);
        }
    }
    {
        SdfChangeBlock block;
        session->SetSubLayerPaths(sublayers);
    }
    return true;
}

bool
TonicBakeSwapMapFile(SdfLayerHandle const &live, SdfPath const &regionMapPath,
                     std::string const &file, std::string *err)
{
    auto fail = [&](std::string const &what) {
        if (err) {
            *err = "TonicBakeSwapMapFile: " + what;
        }
        return false;
    };
    if (!live) {
        return fail("null live layer");
    }
    if (regionMapPath.IsEmpty() || !regionMapPath.IsAbsolutePath()) {
        return fail("region map path must be absolute");
    }
    if (file.empty()) {
        return fail("no baked file to point at");
    }
    SdfPrimSpecHandle const map = live->GetPrimAtPath(regionMapPath);
    if (!map) {
        return fail("live layer carries no " + regionMapPath.GetString());
    }
    SdfChangeBlock block;
    // The spec usually exists already (the §3.1 build authors it): look it
    // up first, since New() on an existing spec logs a coding error.
    SdfAttributeSpecHandle attr;
    for (SdfAttributeSpecHandle const &cand : map->GetAttributes()) {
        if (cand->GetName() == "usdGen:map:file") {
            attr = cand;
            break;
        }
    }
    if (!attr) {
        attr = SdfAttributeSpec::New(map, "usdGen:map:file",
                                     SdfValueTypeNames->Asset);
    }
    if (!attr) {
        return fail("cannot author usdGen:map:file");
    }
    attr->SetDefaultValue(VtValue(SdfAssetPath(file)));
    return true;
}

TonicClumpOffer
TonicPlanClumpFill(UsdStagePtr const &stage, TonicCommitPaths const &paths,
                   int deepestLevel)
{
    TonicClumpOffer offer;
    offer.exprPath = paths.LevelExprPath(std::max(deepestLevel, 1));
    if (!stage || paths.descriptionPath.IsEmpty()) {
        return offer;
    }
    UsdPrim const ops =
        stage->GetPrimAtPath(paths.descriptionPath.AppendChild(TfToken("Ops")));
    if (!ops) {
        return offer;
    }
    // The first Clump op whose usdGen:clump:map is unconnected gets the
    // offer; connected maps are never touched (the fill-in rule, §2.2).
    for (UsdPrim const &child : ops.GetChildren()) {
        if (child.GetTypeName() != TfToken("UsdGenClump")) {
            continue;
        }
        UsdAttribute const map =
            child.GetAttribute(TfToken("usdGen:clump:map"));
        if (_Connections(map).empty()) {
            offer.opPath = child.GetPath();
            return offer;
        }
    }
    return offer;
}

bool
TonicSaveGroomAndMaps(UsdStagePtr const &stage, SdfLayerHandle const &live,
                      std::string const &filePath, std::string const &mapFile,
                      TonicCommitPaths const &paths, std::string *err)
{
    if (!TonicSaveGroom(stage, live, filePath, err)) {
        return false;
    }
    if (mapFile.empty()) {
        return true;
    }
    auto fail = [&](std::string const &what) {
        if (err) {
            *err = "TonicSaveGroomAndMaps: " + what;
        }
        return false;
    };
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path const dest =
        fs::path(filePath).parent_path() / "regionMap.ptx";
    fs::copy_file(mapFile, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return fail("cannot copy " + mapFile + " to " + dest.string());
    }
    SdfLayerRefPtr file = SdfLayer::FindOrOpen(filePath);
    if (!file) {
        return fail("cannot reopen " + filePath);
    }
    {
        SdfChangeBlock block;
        SdfPrimSpecHandle map =
            SdfCreatePrimInLayer(file, paths.RegionMapPath());
        if (!map) {
            return fail("cannot over the RegionMap in the saved file");
        }
        SdfAttributeSpecHandle attr;
        for (SdfAttributeSpecHandle const &cand : map->GetAttributes()) {
            if (cand->GetName() == "usdGen:map:file") {
                attr = cand;
                break;
            }
        }
        if (!attr) {
            attr = SdfAttributeSpec::New(map, "usdGen:map:file",
                                         SdfValueTypeNames->Asset);
        }
        if (!attr) {
            return fail("cannot author usdGen:map:file in the saved file");
        }
        // Relative to the saved layer: a groom that moves with its folder
        // keeps resolving (an absolute path would pin it to this machine).
        attr->SetDefaultValue(
            VtValue(SdfAssetPath("./" + dest.filename().string())));
    }
    if (!file->Save()) {
        return fail("cannot save " + filePath);
    }
    return true;
}

TonicCommitter::TonicCommitter(TonicModel *model, TonicCommitPaths paths)
    : _model(model)
    , _paths(std::move(paths))
{
    _worker = std::thread(&TonicCommitter::_WorkerLoop, this);
}

TonicCommitter::~TonicCommitter()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop = true;
    }
    _wake.notify_all();
    if (_worker.joinable()) {
        _worker.join();
    }
}

void
TonicCommitter::Enqueue(UsdStagePtr const &stage)
{
    TonicFillPlan plan;
    if (stage) {
        plan = TonicPlanGuideInterpolateFill(stage, _paths);
    } else {
        plan.opPath = _paths.InterpOpPath();
    }
    EnqueuePlan(plan);
}

size_t
TonicCommitter::CancelDescriptionCooks() const
{
    if (_paths.descriptionPath.IsEmpty()) {
        return 0;
    }
    return usdGenImaging::UsdGenSessionStore::GetInstance().CancelCooks(
        _paths.descriptionPath);
}

void
TonicCommitter::SetScalpPath(SdfPath const &scalpPath)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _paths.scalpPath = scalpPath;
}

SdfPath
TonicCommitter::GetScalpPath() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _paths.scalpPath;
}

void
TonicCommitter::EnqueuePlan(TonicFillPlan const &plan)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _pendingVersion = _model->GetVersion();
        _pendingPlan = plan;
        if (_pendingPlan.opPath.IsEmpty()) {
            _pendingPlan.opPath = _paths.InterpOpPath();
        }
        _pendingVersionAtomic.store(_pendingVersion);
    }
    _wake.notify_one();
}

void
TonicCommitter::Detach()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _detached = true;
    if (_ready && _readyVersion > _committedVersion.load()) {
        // Built for the dead stage: never swappable, same accounting as
        // superseded-on-the-shelf.
        _droppedCount.fetch_add(1);
    }
    _ready = SdfLayerRefPtr();
    _readyVersion = 0;
    _partialSlots.clear();
    _partialNext = 0;
    _partialVersion = 0;
    // The pending enqueue (if any) carries the dead stage's fill-in plan;
    // drop it so only a post-Reattach enqueue can build.
    _pendingVersion = 0;
    _pendingPlan = TonicFillPlan();
    _pendingVersionAtomic.store(0);
}

void
TonicCommitter::Reattach()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _detached = false;
    // Same guarantee as Detach: anything enqueued while detached targeted
    // the dead stage, so the tool must enqueue fresh for the new one.
    _pendingVersion = 0;
    _pendingPlan = TonicFillPlan();
    _pendingVersionAtomic.store(0);
    // A new live-layer lineage: the fresh live layer carries nothing yet,
    // whatever the old lineage committed.
    _committedVersion.store(0);
    _wake.notify_one();
}

bool
TonicCommitter::IsDetached() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _detached;
}

void
TonicCommitter::_WorkerLoop()
{
    for (;;) {
        uint64_t version = 0;
        TonicFillPlan plan;
        TonicCommitPaths paths;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [&] {
                return _stop ||
                    (!_paused && !_detached &&
                     _pendingVersion > _readyVersion &&
                     _pendingVersion != _failedVersion && !_building);
            });
            if (_stop) {
                return;
            }
            // Coalescing: take the latest enqueue, whatever woke us.
            version = _pendingVersion;
            plan = _pendingPlan;
            paths = _paths;
            _building = true;
        }
        // Snapshot the live model (its own mutex) and build with no stage.
        // Everything from here to the handover is wrapped: a throw in the
        // snapshot or the layer build must leave the previous live layer,
        // the model and this thread alone (plan/17 §3.4).
        SdfLayerRefPtr built;
        std::string err;
        bool builtOk = false;
        uint64_t builtVersion = version;
        try {
            if (int const pending = _throwBuilds.load(); pending > 0) {
                _throwBuilds.store(pending - 1);
                throw std::runtime_error(
                    "TonicCommitter: injected build failure (test hook)");
            }
            TonicSnapshot snapshot = TonicSnapshotFromModel(*_model);
            snapshot.scalpPath = paths.scalpPath;
            snapshot.version = snapshot.tubes.empty()
                                   ? version
                                   : snapshot.tubes[0].tube.version;
            snapshot.createInterpOp = plan.createInterpOp;
            snapshot.setInterpGuides = plan.setInterpGuides;
            snapshot.setInterpRegion = plan.setInterpRegion;
            snapshot.interpOpPath = plan.opPath;
            builtVersion = snapshot.version;
            builtOk = TonicBuildCommitLayer(snapshot, paths, &built, &err,
                                            &_guideCache);
        } catch (std::exception const &e) {
            _workerThrowCount.fetch_add(1);
            builtOk = false;
            built.Reset();
            err = std::string("TonicCommitter: worker threw: ") + e.what();
        } catch (...) {
            _workerThrowCount.fetch_add(1);
            builtOk = false;
            built.Reset();
            err = "TonicCommitter: worker threw an unknown exception";
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _building = false;
            if (!builtOk) {
                _diagnostic = err;
                // A bad snapshot (or a throw) keeps the previous live layer
                // and the previous built layer; the model is unaffected
                // (plan/17 §3.4). The version is parked so the worker does
                // not re-run the same failing build in a tight loop.
                _failedVersion = version;
                continue;
            }
            _buildCount.fetch_add(1);
            if (_detached) {
                // Detached mid-build: the fill-in plan targets the dead
                // stage, so this layer can never swap. Drop it like a
                // superseded build; the tool re-enqueues after Reattach.
                _droppedCount.fetch_add(1);
                continue;
            }
            if (_pendingVersion > builtVersion) {
                // Superseded mid-build: abandon this layer (cancellation)
                // and rebuild the latest.
                _droppedCount.fetch_add(1);
                _wake.notify_one();
                continue;
            }
            if (_ready && _readyVersion > _committedVersion.load()) {
                // A built layer that never swapped: superseded on the shelf.
                _droppedCount.fetch_add(1);
            }
            _ready = built;
            _readyVersion = builtVersion;
        }
    }
}

TonicCommitter::SwapResult
TonicCommitter::SwapIfIdle(SdfLayerHandle const &live, bool gestureActive)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_detached) {
            return Detached;
        }
    }
    if (!live) {
        return NothingPending;
    }
    if (gestureActive) {
        return SkippedGesture;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_partialVersion != 0) {
        if (_readyVersion > _partialVersion) {
            // A newer layer landed mid-transfer: abandon the stale partial
            // and start over below.
            _partialSlots.clear();
            _partialNext = 0;
            _partialVersion = 0;
        } else {
            return _SwapPartialSlot(live, _ready, _partialVersion);
        }
    }
    if (!_ready || _readyVersion <= _committedVersion.load()) {
        return NothingPending;
    }
    if (_building && _pendingVersion > _readyVersion) {
        // A newer version is already being built; the worker will hand a
        // newer layer soon, so this swap waits.
        return SkippedStale;
    }
    if (_partialMode.load()) {
        _BeginPartial(_ready);
        _partialVersion = _readyVersion;
        return _SwapPartialSlot(live, _ready, _readyVersion);
    }
    return _SwapFull(live, _ready, _readyVersion);
}

TonicCommitter::SwapResult
TonicCommitter::_SwapFull(SdfLayerHandle const &live,
                          SdfLayerHandle const &built, uint64_t version)
{
    auto t0 = std::chrono::steady_clock::now();
    {
        SdfChangeBlock block;
        live->TransferContent(built);
    }
    auto t1 = std::chrono::steady_clock::now();
    double const ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    _lastSwapMs.store(ms);
    _committedVersion.store(version);
    _partialSlots.clear();
    _partialNext = 0;
    _partialVersion = 0;
    if (ms > _swapBudgetMs.load()) {
        // Over budget: subsequent swaps go one subtree per idle slot.
        _partialMode.store(true);
    }
    return Swapped;
}

namespace {

// Copy one prim's self (specifier, type, and — unless `shellOnly` — its own
// properties) without children. The Guides self-slot is shell-only: its
// properties arrive in later per-property slots (TN-4), but stale ones are
// pruned here so a removed property never lingers on live.
void
_CopyPrimSelf(SdfLayerHandle const &built, SdfLayerHandle const &live,
              SdfPath const &path, bool shellOnly = false)
{
    SdfPrimSpecHandle const src = built->GetPrimAtPath(path);
    SdfPrimSpecHandle const dst = SdfCreatePrimInLayer(live, path);
    if (!src || !dst) {
        return;
    }
    dst->SetSpecifier(src->GetSpecifier());
    if (!src->GetTypeName().IsEmpty()) {
        dst->SetTypeName(src->GetTypeName().GetString());
    }
    if (!shellOnly) {
        for (SdfAttributeSpecHandle const &attr : src->GetAttributes()) {
            SdfCopySpec(built, attr->GetPath(), live, attr->GetPath());
        }
        for (SdfRelationshipSpecHandle const &rel : src->GetRelationships()) {
            SdfCopySpec(built, rel->GetPath(), live, rel->GetPath());
        }
    }
    std::vector<SdfPropertySpecHandle> staleProps;
    for (SdfAttributeSpecHandle const &attr : dst->GetAttributes()) {
        if (!built->GetAttributeAtPath(attr->GetPath())) {
            staleProps.push_back(attr);
        }
    }
    for (SdfRelationshipSpecHandle const &rel : dst->GetRelationships()) {
        if (!built->GetRelationshipAtPath(rel->GetPath())) {
            staleProps.push_back(rel);
        }
    }
    for (SdfPropertySpecHandle const &prop : staleProps) {
        dst->RemoveProperty(prop);
    }
}

// Drop live children of `parentPath` the built layer no longer carries.
void
_DropStaleChildren(SdfLayerHandle const &built, SdfLayerHandle const &live,
                   SdfPath const &parentPath)
{
    SdfPrimSpecHandle const dst = live->GetPrimAtPath(parentPath);
    if (!dst) {
        return;
    }
    std::vector<SdfPrimSpecHandle> stale;
    for (SdfPrimSpecHandle const &child : dst->GetNameChildren()) {
        if (!built->GetPrimAtPath(child->GetPath())) {
            stale.push_back(child);
        }
    }
    for (SdfPrimSpecHandle const &child : stale) {
        dst->RemoveNameChild(child);
    }
}

} // namespace

void
TonicCommitter::_BeginPartial(SdfLayerHandle const &built)
{
    _partialSlots.clear();
    _partialNext = 0;
    // Slot 0 is the groom prim itself (type + own properties, no children).
    // Tubes go one per slot (SdfCopySpec per tube): at reference scale the
    // Tubes scope alone is megabytes, so a per-groom-child split would still
    // blow the slot budget. Guides goes as the prim shell plus one slot per
    // property for the same reason. Every other groom child and the
    // description opinions (if any) take one slot each, in name order.
    _partialSlots.push_back(_PartialSlot{_paths.groomPath, true});
    SdfPath const tubesPath = _paths.TubesPath();
    if (SdfPrimSpecHandle const groom = built->GetPrimAtPath(_paths.groomPath)) {
        std::vector<SdfPath> children;
        for (SdfPrimSpecHandle const &child : groom->GetNameChildren()) {
            if (child->GetPath() != tubesPath) {
                children.push_back(child->GetPath());
            }
        }
        std::sort(children.begin(), children.end());
        for (SdfPath const &child : children) {
            if (child == _paths.GuidesPath()) {
                // Guides is the one fat non-tube child (192 K guide CVs +
                // root frames at reference scale): one SdfCopySpec for the
                // whole subtree blows the slot budget, so it goes as the
                // prim self plus one slot per property (TN-4).
                _partialSlots.push_back(_PartialSlot{child, true});
                SdfPrimSpecHandle const guides =
                    built->GetPrimAtPath(child);
                if (guides) {
                    std::vector<SdfPath> props;
                    for (SdfAttributeSpecHandle const &attr :
                         guides->GetAttributes()) {
                        props.push_back(attr->GetPath());
                    }
                    for (SdfRelationshipSpecHandle const &rel :
                         guides->GetRelationships()) {
                        props.push_back(rel->GetPath());
                    }
                    std::sort(props.begin(), props.end());
                    for (SdfPath const &prop : props) {
                        _partialSlots.push_back(_PartialSlot{prop, false});
                    }
                }
            } else {
                _partialSlots.push_back(_PartialSlot{child, false});
            }
        }
    }
    if (built->GetPrimAtPath(tubesPath)) {
        _partialSlots.push_back(_PartialSlot{tubesPath, true});
        std::vector<SdfPath> tubePaths;
        for (SdfPrimSpecHandle const &child :
             built->GetPrimAtPath(tubesPath)->GetNameChildren()) {
            tubePaths.push_back(child->GetPath());
        }
        std::sort(tubePaths.begin(), tubePaths.end());
        for (SdfPath const &tube : tubePaths) {
            _partialSlots.push_back(_PartialSlot{tube, false});
        }
    }
    if (!_paths.descriptionPath.IsEmpty() &&
        built->GetPrimAtPath(_paths.descriptionPath)) {
        _partialSlots.push_back(_PartialSlot{_paths.descriptionPath, false});
    }
}

TonicCommitter::SwapResult
TonicCommitter::_SwapPartialSlot(SdfLayerHandle const &live,
                                 SdfLayerHandle const &built, uint64_t version)
{
    auto t0 = std::chrono::steady_clock::now();
    {
        SdfChangeBlock block;
        _PartialSlot const &slot = _partialSlots[_partialNext];
        if (_partialNext == 0) {
            // Groom self, plus stale-subtree cleanup so a partial swap
            // never leaves removed tubes/children behind: the live layer is
            // tool-owned, so anything the built layer no longer carries is
            // dropped. Ancestors for the later slots are ensured here too.
            _CopyPrimSelf(built, live, slot.path);
            _DropStaleChildren(built, live, _paths.groomPath);
            _DropStaleChildren(built, live, _paths.TubesPath());
            if (!_paths.descriptionPath.IsEmpty()) {
                if (built->GetPrimAtPath(_paths.descriptionPath)) {
                    SdfCreatePrimInLayer(live, _paths.descriptionPath);
                } else if (live->GetPrimAtPath(_paths.descriptionPath)) {
                    SdfPrimSpecHandle const parent = live->GetPrimAtPath(
                        _paths.descriptionPath.GetParentPath());
                    if (parent) {
                        parent->RemoveNameChild(live->GetPrimAtPath(
                            _paths.descriptionPath));
                    }
                }
            }
        } else if (slot.selfOnly) {
            // The Guides self-slot carries the shell only; its properties
            // follow in per-property slots (TN-4).
            _CopyPrimSelf(built, live, slot.path,
                          /*shellOnly*/ slot.path == _paths.GuidesPath());
        } else {
            SdfCopySpec(built, slot.path, live, slot.path);
        }
        ++_partialNext;
    }
    auto t1 = std::chrono::steady_clock::now();
    _lastSwapMs.store(std::chrono::duration<double, std::milli>(t1 - t0).count());
    if (_partialNext >= _partialSlots.size()) {
        _committedVersion.store(version);
        _partialSlots.clear();
        _partialNext = 0;
        _partialVersion = 0;
        return Swapped;
    }
    return PartialProgress;
}

void
TonicCommitter::PauseWorker(bool pause)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _paused = pause;
    }
    _wake.notify_all();
}

void
TonicCommitter::SetReadyForTest(SdfLayerRefPtr const &layer, uint64_t version)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _ready = layer;
    _readyVersion = version;
}

void
TonicCommitter::ForcePartialModeForTest(bool on)
{
    _partialMode.store(on);
}

void
TonicCommitter::ThrowOnNextBuildsForTest(int count)
{
    _throwBuilds.store(count < 0 ? 0 : count);
}

std::string
TonicCommitter::TakeDiagnostic()
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::string diagnostic = _diagnostic;
    _diagnostic.clear();
    return diagnostic;
}

} // namespace usdGenTonic
