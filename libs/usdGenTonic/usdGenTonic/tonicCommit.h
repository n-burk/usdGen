// usdGenTonic — the asynchronous commit pipeline (plan/17 section 3, P1).
//
// UI thread                  commit worker                  UI thread (idle)
// release -> Enqueue() ---> snapshot -> build SdfLayer ---> SwapIfIdle()
//                             (anonymous, on no stage)       TransferContent
//                                                            in one ChangeBlock
//
// The worker never touches Qt, the UsdStage, or the bake; the UI thread never
// serialises, cooks, or waits on CUDA. Only this TU (plus hydrate/save, which
// run on the UI thread) links `usd`; gate B-1 stays true for `usdGen` itself.
#ifndef USDGEN_TONIC_COMMIT_H
#define USDGEN_TONIC_COMMIT_H

#include "usdGenTonic/api.h"
#include "usdGenTonic/tonicModel.h"

#include "pxr/pxr.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenTonic {

// Where one groom lands. The committer owns every prim under groomPath plus
// the GuideInterpolate op under the linked description; nothing else.
struct USDGENTONIC_API TonicCommitPaths {
    SdfPath groomPath = SdfPath("/TonicGroom");
    SdfPath descriptionPath;  // empty = no GuideInterpolate fill-in
    SdfPath scalpPath;  // empty = no scalp link, live primvar, or graph

    SdfPath TubesPath() const { return groomPath.AppendChild(TfToken("Tubes")); }
    // The prim NAME of one tube: "tube<n>", or "group<n>" for the negative
    // ids the on-the-fly parents mint (a '-' is not a legal prim name).
    static std::string TubeName(int tubeId);
    // The path a ROOT tube takes (directly under Tubes). Nested children
    // hang off their parent, so their paths come from TonicTubePaths.
    SdfPath TubePath(int tubeId) const;
    SdfPath GuidesPath() const
    {
        return groomPath.AppendChild(TfToken("Guides"));
    }
    // Commit-owned sparse cage rails.  Output's CurveSource interpolates its
    // dense hairs at cook time; it never writes dense output arrays to USD.
    SdfPath OutputCurvesPath() const
    {
        return groomPath.AppendChild(TfToken("OutputCurves"));
    }
    SdfPath OutputRegionMapPath() const
    {
        return groomPath.AppendChild(TfToken("OutputRegionMap"));
    }
    SdfPath OutputPath() const
    {
        return groomPath.AppendChild(TfToken("Output"));
    }
    SdfPath ScalpGraphPath() const
    {
        return groomPath.AppendChild(TfToken("ScalpGraph"));
    }
    SdfPath RegionMapPath() const
    {
        return groomPath.AppendChild(TfToken("RegionMap"));
    }
    SdfPath RegionExprPath() const
    {
        return groomPath.AppendChild(TfToken("RegionExpr"));
    }
    // Per-level map/expression prims (plan/17 §4.5): level 1 is the
    // unsuffixed RegionMap/RegionExpr above; deeper levels (P4) share the
    // versioned file through maps with firstChannel = level - 1.
    SdfPath LevelMapPath(int level) const;
    SdfPath LevelExprPath(int level) const;
    // The operator the fill-in authors when the description has none.
    SdfPath InterpOpPath() const;
};

// One committed tube. The whole hierarchy rides this vector (V0b): one entry
// per model tube, parents before children, with the links and the deltas the
// schema stores so hydrate can re-derive every level.
struct USDGENTONIC_API TonicSnapshotTube {
    int tubeId = 0;
    int regionId = -1;  // -1 = unrooted (the root-disc stream)
    int level = 1;
    int parentTubeId = -1;  // -1 = L1 root or an on-the-fly group parent
    int childIndex = -1;    // index within the parent's subdivision
    // Shape + fill + subdivide + locks, including the row-major local-to-
    // world frameReference rotation. It is authored per tube so the frame
    // used to interpret hierarchical deltas survives reload.
    TonicModel::TubeSnapshot tube;
    std::vector<float> centerDeltas;   // 3 per center CV, in the derived frames
    std::vector<float> sectionDeltas;  // 2 per section CV, in the section plane
    // (scale, twist) residual per section.  Empty is the legacy spelling and
    // means exact zero residuals.
    std::vector<float> sectionDeltaTransforms;
    // Flattened (section, parentSlot, childSlot) triples for the inherited
    // L1 boundary corners K7 may align. Empty is the legacy spelling.
    std::vector<int> inheritedBoundaryBindings;
    bool transientParent = false;  // on-the-fly parent, not committed
    bool persistent = false;       // UsdGenTubeHierarchyAPI:persistent
    bool imported = false;         // bridge import: explicit, locked shape
    // True while a subdivision hangs off this tube: its own fill is
    // suspended and the children carry the density (plan/17 §2.3).
    bool fillSuspended = false;
    std::vector<int> members;      // group-parent members (tube ids)
    // Mesh-fill roots for THIS tube: the scalp faces it owns after the
    // hierarchy partition (empty = the root-disc stream).
    std::vector<int> regionFaces;
    // Exact graph support for an L1 tube. Moving this boundary inside a
    // coarse face changes roots even when regionFaces is unchanged.
    std::vector<float> regionBoundary;
    // Everything TonicGuidesFromSnapshot reads off this entry, hashed
    // (plan/18 §7 G6). Filled by TonicSnapshotFromModel after the faces are
    // partitioned, so it is the LAST thing computed about a tube. Two
    // snapshots whose entry hashes agree produce byte-identical guides,
    // which is what lets TonicGuideCache skip the refill. The snapshot
    // version is deliberately NOT in it: it moves on every commit.
    uint64_t contentHash = 0;
};

// Everything one layer build needs: plain data, no model access, safe to
// move to the worker. The fill-in flags are captured on the UI thread from
// the composed stage (the worker must not read the stage).
struct USDGENTONIC_API TonicSnapshot {
    uint64_t version = 0;
    // Clear Generated Curves is authored state, so a commit/hydrate round
    // trip must not silently regenerate the cleared Guides prim.
    bool generatedCurvesSuppressed = false;
    // Commit-only hairs are independent of the interactive Guide visibility
    // and Clear Generated Curves state. Missing legacy attributes hydrate to
    // these defaults.
    bool outputEnabled = false;
    float outputDensityMultiplier = 1.0f;
    float outputWidth = 0.01f;
    int outputPtexResolution = -1;
    std::vector<TonicSnapshotTube> tubes;
    TonicModel::GraphSnapshot graph;  // scalp graph + live primvar + map file
    SdfPath scalpPath;  // empty = author no scalp opinion
    // Mesh-fill inputs: the scalp copy plus the L1 tube's region faces
    // (faces at its interpolation id). Empty faces mean the disc stream.
    // The per-tube split of those faces lives on each TonicSnapshotTube:
    // a subdivision hands its faces down to the child cell that claims
    // them, so each leaf fills only its own share.
    TonicScalpMesh scalp;
    bool hasScalp = false;
    int tubeRegionId = -1;
    std::vector<int> tubeRegionFaces;
    bool createInterpOp = false;
    bool setInterpGuides = false;
    bool setInterpRegion = false;
    SdfPath interpOpPath;  // the op the flags target (existing or new)
};

// Snapshot the model's whole hierarchy (UI thread; cheap host-vector
// copies). Entries come out parents-first in ascending id order, transient
// group parents excluded (they are not committed). Returns a snapshot with
// no tubes when the model holds no tube.
TonicSnapshot USDGENTONIC_API
TonicSnapshotFromModel(TonicModel const &model);

// Where every tube in the snapshot lands: children nest under their parent
// (the schema's "hierarchy is prim hierarchy"), roots and group parents sit
// directly under <groom>/Tubes.
std::map<int, SdfPath> USDGENTONIC_API
TonicTubePaths(TonicSnapshot const &snapshot, TonicCommitPaths const &paths);

// The guides one snapshot commits: K8/K9/K10 per LEAF tube (a subdivided
// parent's fill is suspended, an import carries explicit geometry and is not
// filled), concatenated in the snapshot's tube order. The committer authors
// this and hydrate re-runs it to assert bit-equality, so the two can never
// drift apart.
struct USDGENTONIC_API TonicSnapshotGuides {
    std::vector<float> points;   // 3 per CV, curve-major
    std::vector<int> counts;     // CVs per curve
    std::vector<uint64_t> ids;   // stable curve ids
    std::vector<double> frames;  // 16 doubles per curve
    std::vector<int> tubeIds;    // per curve
    std::vector<int> levels;     // per curve
    std::vector<int> regionIds;  // per curve
};
TonicSnapshotGuides USDGENTONIC_API
TonicGuidesFromSnapshot(TonicSnapshot const &snapshot);

// The content hash TonicGuideCache keys on: everything the guide fill reads
// off one entry, by float BIT PATTERN so the key is as exact as the fill.
// TonicSnapshotFromModel fills every entry's contentHash with it; a snapshot
// assembled by hand (the reference-scale fixture, an importer) must call
// TonicHashSnapshot before handing it to a cache, or every tube reads as
// dirty forever.
uint64_t USDGENTONIC_API
TonicSnapshotTubeHash(TonicSnapshotTube const &entry);
void USDGENTONIC_API TonicHashSnapshot(TonicSnapshot *snapshot);

// Per-version dirty set for the guide refill (plan/18 §7 G6).
//
// Before V6 every commit re-ran K8/K9/K10 over EVERY tube, so moving one
// CV of one tube in the reference groom refilled 12 000 guides to change
// 5 of them. The fill is deterministic per (tubeId, tube bytes, owned
// faces, scalp), so a tube whose TonicSnapshotTube::contentHash is
// unchanged since the last commit yields byte-identical guides and its
// slice is copied out of the cache instead of regenerated.
//
// The cache is owned by the committer (one per groom) and touched only on
// the worker thread. `scalpHash` guards the shared input: a rebound or
// re-rasterised scalp invalidates every entry at once. Passing no cache
// (the default) regenerates everything, which is what hydrate's
// bit-equality assertion needs — it must never be answered from a cache
// built by the writer it is checking.
class USDGENTONIC_API TonicGuideCache {
public:
    // Slices are keyed by tube id; ids the newest snapshot does not carry
    // are evicted by Reap() at the end of each build.
    struct Entry {
        uint64_t contentHash = 0;
        std::vector<float> points;
        std::vector<int> counts;
        std::vector<double> frames;
        int guideCount = 0;
    };
    // The cached slice for `tubeId` when its hash still matches, else null.
    Entry const *Find(int tubeId, uint64_t contentHash) const;
    void Store(int tubeId, Entry entry);
    // The shared fill input. Clears every slice and returns true when
    // `scalpHash` differs from the one the cache was filled against: a
    // rebound or re-rasterised scalp moves every mesh-filled root.
    bool NoteScalp(uint64_t scalpHash);
    // Drop every tube not in `live` (merge, delete, undo). Returns the
    // number of entries dropped.
    size_t Reap(std::vector<int> const &live);
    void Clear();
    size_t Size() const { return _entries.size(); }
    // Test introspection: tubes answered from the cache / refilled by the
    // last TonicGuidesFromSnapshot call that was handed this cache.
    size_t LastReused() const { return _lastReused; }
    size_t LastRefilled() const { return _lastRefilled; }

private:
    friend TonicSnapshotGuides USDGENTONIC_API
    TonicGuidesFromSnapshot(TonicSnapshot const &, TonicGuideCache *);
    std::map<int, Entry> _entries;
    uint64_t _scalpHash = 0;
    size_t _lastReused = 0;
    size_t _lastRefilled = 0;
};

// The same guides, with the unchanged tubes copied out of `cache` instead
// of refilled. `cache` may be null (identical to the overload above).
TonicSnapshotGuides USDGENTONIC_API
TonicGuidesFromSnapshot(TonicSnapshot const &snapshot,
                        TonicGuideCache *cache);

// Build the commit layer from a snapshot (worker thread; Sdf only, no stage,
// no Qt). The layer is anonymous and on no stage. Every prim the committer
// owns is rebuilt from the snapshot, so a swap never carries stale opinions:
// groom + Tubes + ScalpGraph shell + Guides + RegionMap shell + RegionExpr,
// plus the GuideInterpolate op exactly as the snapshot's fill-in flags say.
// Returns false with *err set when nothing could be built.
bool USDGENTONIC_API TonicBuildCommitLayer(TonicSnapshot const &snapshot,
                                           TonicCommitPaths const &paths,
                                           SdfLayerRefPtr *outLayer,
                                           std::string *err,
                                           TonicGuideCache *cache = nullptr,
                                           TonicGuideCache *outputCache = nullptr);

// Decide the GuideInterpolate fill-in against the COMPOSED stage (UI thread,
// at enqueue time). The tool never rewrites an operator stack an artist has
// hand-authored; it only fills in what is empty:
//   * no UsdGenGuideInterpolate under <desc>/Ops -> create one (named by
//     TonicCommitPaths::InterpOpPath) with guides + region connections;
//   * an op exists -> set usdGen:guides only when it has no targets, and
//     connect usdGen:region to the RegionExpr only when it has no connection.
// A missing description (or empty descriptionPath) plans no fill-in.
struct USDGENTONIC_API TonicFillPlan {
    bool createInterpOp = false;
    bool setInterpGuides = false;
    bool setInterpRegion = false;
    SdfPath opPath;  // existing op, or InterpOpPath() when creating
};
TonicFillPlan USDGENTONIC_API
TonicPlanGuideInterpolateFill(UsdStagePtr const &stage,
                              TonicCommitPaths const &paths);

// Hydrate the model from a committed stage (UI thread, plan/17 section 2.5).
// Reads the scalp graph (P2) and the WHOLE tube hierarchy under
// <groom>/Tubes: the L1 tube is restored outright, every deeper level is
// re-derived from its parent with the stored (tubeId, seed) subdivision and
// then carries the stored shape + deltas, on-the-fly parents come back
// through their members, and bridge imports through ImportLockedTube.
// The guides are then regenerated from the hydrated hierarchy and asserted
// bit-equal against the stored <groom>/Guides: a stage the tool produced
// always round-trips, so a mismatch is a hard failure, never a silent
// accept. Stored curves that no model tube claims are FOREIGN (hand-
// authored, Houdini) and are imported as locked tubes one level below the
// tube whose region roots them (plan/17 §2.5, §5.7) instead of failing.
// The graph round-trips the same way: the extracted loops and the
// re-rasterised live primvar must equal the stored opinions bit-exactly.
struct USDGENTONIC_API TonicHydrateResult {
    bool ok = false;
    bool guidesBitEqual = false;
    bool graphRoundTrip = false;
    size_t guideCount = 0;
    size_t graphNodeCount = 0;
    size_t graphRegionCount = 0;
    size_t tubeCount = 0;          // tubes hydrated, imports included
    size_t importedTubeCount = 0;  // foreign guides turned into locked tubes
    std::string diagnostic;
};
TonicHydrateResult USDGENTONIC_API
TonicHydrateModel(UsdStagePtr const &stage, SdfPath const &groomPath,
                  TonicModel *model);

// "Save groom" (UI thread). Copies the live layer's content into the file
// layer at `filePath` (created when missing; .usdc, never .usda per S42),
// saves it, and re-parents the layer stack so the live sublayer sits beneath
// the saved file no longer... precisely: session sublayers become
// [live, file], the live layer staying strongest so editing continues there.
// Returns false with *err set on any failure; the stage is untouched then.
bool USDGENTONIC_API TonicSaveGroom(UsdStagePtr const &stage,
                                    SdfLayerHandle const &live,
                                    std::string const &filePath,
                                    std::string *err);

// "Save groom" with the baked map (plan/17 §3.1a, P2): TonicSaveGroom plus a
// copy of the latest baked version beside the saved layer as `regionMap.ptx`
// and a repoint of usdGen:map:file on <groom>/RegionMap in the SAVED file
// layer, written RELATIVE to that layer so the saved groom is portable.
// The live layer keeps its versioned reference untouched. UI thread: this
// re-parents the session layer stack, which is main-thread-only work; the
// file copy is the only part that scales with the map.
// An empty `mapFile` saves the groom with no map copy (same as TonicSaveGroom).
bool USDGENTONIC_API TonicSaveGroomAndMaps(
    UsdStagePtr const &stage, SdfLayerHandle const &live,
    std::string const &filePath, std::string const &mapFile,
    TonicCommitPaths const &paths, std::string *err);

// The bake pipeline's swap (UI thread, at idle, plan/17 §3.1a): authors
// exactly one attribute, usdGen:map:file on the RegionMap prim, in one
// SdfChangeBlock. Microseconds of main-thread work; the versioned filename
// means the swap never points at a half-written file.
bool USDGENTONIC_API TonicBakeSwapMapFile(SdfLayerHandle const &live,
                                          SdfPath const &regionMapPath,
                                          std::string const &file,
                                          std::string *err);

// The Clump wiring offer (plan/17 §2.2, P2): when a UsdGenClump operator
// exists under <desc>/Ops and its usdGen:clump:map is unconnected, the tool
// OFFERS RegionExprL<n> for the deepest baked level — it never rewrites the
// connection itself. Returns the op path to offer for, or an empty path when
// there is no offer (no Clump op, or every Clump map is already connected).
struct USDGENTONIC_API TonicClumpOffer {
    SdfPath opPath;  // empty = no offer
    SdfPath exprPath;  // the RegionExprL<n> to offer
};
TonicClumpOffer USDGENTONIC_API
TonicPlanClumpFill(UsdStagePtr const &stage, TonicCommitPaths const &paths,
                   int deepestLevel);

// The committer: version coalescing, a build worker, and the idle swap.
class USDGENTONIC_API TonicCommitter {
public:
    // `model` is non-owning and must outlive the committer; all model reads
    // go through TonicModel::Snapshot under the model's mutex. The worker
    // thread starts here and joins in the destructor.
    TonicCommitter(TonicModel *model, TonicCommitPaths paths);
    ~TonicCommitter();
    TonicCommitter(TonicCommitter const &) = delete;
    TonicCommitter &operator=(TonicCommitter const &) = delete;

    // Request a commit of the model's latest version (UI thread, at release).
    // Stores only the version + fill-in plan; the worker takes the latest
    // when it wakes, so ten fast strokes produce one layer build. The
    // fill-in plan is captured from `stage` here (may be null to skip the
    // GuideInterpolate fill-in for this version).
    void Enqueue(UsdStagePtr const &stage = UsdStagePtr());
    // Enqueue with a caller-computed fill-in plan (the ctypes path: Python
    // plans over pxr and passes the flags, since no UsdStage crosses the C
    // ABI). An empty plan authors no description opinions for this version.
    // The artist-owned-output guard runs here, against `stage` when one is
    // given: a refused enqueue parks the model version as failed and leaves
    // the reason for TakeDiagnostic, exactly like a failed build. Every
    // enqueue clears the parked failure, so an explicit re-enqueue retries.
    void EnqueuePlan(TonicFillPlan const &plan,
                     UsdStagePtr const &stage = UsdStagePtr());

    enum SwapResult {
        Swapped,          // live now carries the committed version
        PartialProgress,  // one subtree copied; call again next idle slot
        NothingPending,   // no committed version newer than live
        SkippedGesture,   // a gesture is active; the swap waits
        SkippedStale,     // a newer version is already being built
        Detached,         // stage closed/reloaded: Reattach first (P6 §3.4)
    };

    // Swap the latest built layer into `live` (UI thread, from idle only).
    // One SdfChangeBlock per call: either a full TransferContent, or one
    // child-prim subtree via SdfCopySpec when the swap is over budget (the
    // partial-transfer fallback; every slot stays under budget at the cost
    // of a few frames of stage staleness, which is acceptable because the
    // viewport shows the model, not the stage). Never swaps during a
    // gesture, and never swaps a version a newer build has superseded.
    SwapResult SwapIfIdle(SdfLayerHandle const &live, bool gestureActive);

    // Stage closed or reloaded under the tool (UI thread, plan/17 §3.4).
    // Detach drops the shelf layer built for the old stage (it counts as
    // dropped: built but never swapped) and idles the worker; the model
    // survives untouched. SwapIfIdle reports Detached and never touches
    // the dead live layer until Reattach. Enqueues while detached are
    // dropped (their fill-in plans target the old stage); the tool
    // re-enqueues after Reattach, and the next swap re-hydrates — or
    // re-creates — the groom prim in the new live layer from the
    // surviving model. Committed versions are per live-layer lineage, so
    // Reattach resets the committed watermark: the fresh live layer
    // carries nothing yet.
    void Detach();
    void Reattach();
    bool IsDetached() const;

    // Abandon the usdGen cook the last swap started (plan/17 §3.2 rule 2,
    // plan/18 §7 G7). The stage swap dirties <groom>/Guides, the dirty
    // router dirties the linked description, and its session cooks; when
    // the artist presses again, that cook is describing a groom the model
    // has already left. This bumps the cancellation token on every live
    // session rooted at the committer's description path, so a request
    // still queued on that session's owner is dropped before the engine
    // sees it. Requests the engine already holds keep its own supersede
    // behaviour. The commit-owned Output description is cancelled separately
    // from the legacy description path. Returns the number of sessions whose
    // token moved (0 when neither path is linked or cooking).
    //
    // Callable from the gesture thread: the token is an atomic and this
    // takes no owner command and no lock.
    size_t CancelDescriptionCooks() const;

    void SetSwapBudgetMs(double ms) { _swapBudgetMs.store(ms); }
    // Retarget the scalp link (UI thread; takes effect on the next build).
    // Empty clears the link (no scalp opinion, no live primvar, no graph).
    void SetScalpPath(SdfPath const &scalpPath);
    SdfPath GetScalpPath() const;
    double LastSwapMs() const { return _lastSwapMs.load(); }
    uint64_t CommittedVersion() const { return _committedVersion.load(); }
    uint64_t PendingVersion() const { return _pendingVersionAtomic.load(); }
    // Test introspection: layer builds finished, and built layers dropped
    // because a newer version superseded them before the swap (cancellation)
    // or a stage Detach shelved them for a dead live layer.
    size_t BuildCount() const { return _buildCount.load(); }
    size_t DroppedCount() const { return _droppedCount.load(); }
    bool PartialMode() const { return _partialMode.load(); }
    // Test hook: hold the worker between builds so coalescing is
    // deterministic (enqueue N versions, resume, expect one build).
    void PauseWorker(bool pause);
    // Test hooks for the reference-scale swap path: install a synthetic
    // built layer (the model holds one tube; the TN-4 scene is built
    // directly) and force the partial fallback for it.
    void SetReadyForTest(SdfLayerRefPtr const &layer, uint64_t version);
    void ForcePartialModeForTest(bool on);
    // Test hook for the §3.4 failure mode: make the next N builds throw
    // inside the worker. The loop catches it, records the diagnostic and
    // keeps both the previous built layer and the thread (plan/18 §7 G5).
    void ThrowOnNextBuildsForTest(int count);
    size_t WorkerThrowCount() const { return _workerThrowCount.load(); }
    // The last build (or enqueue guard) failure, cleared by the read. The
    // tool's pump takes it once per idle slot so a failure reaches the
    // artist exactly once.
    std::string TakeDiagnostic();
    // The version whose build failed or whose enqueue the guard refused
    // (0 when the latest enqueue has not failed). The worker never retries
    // it on its own, so a pending version at or below it is not in flight.
    uint64_t FailedVersion() const;

private:
    void _WorkerLoop();
    // The reserved Output path an artist owns (no outputOwned marker), or
    // an empty path when enabling Tonic output may author all three.
    SdfPath _BlockedOutputPath(UsdStagePtr const &stage) const;
    SwapResult _SwapFull(SdfLayerHandle const &live, SdfLayerHandle const &built,
                         uint64_t version);
    SwapResult _SwapPartialSlot(SdfLayerHandle const &live,
                                SdfLayerHandle const &built, uint64_t version);
    void _BeginPartial(SdfLayerHandle const &built);

    struct _PartialSlot {
        SdfPath path;
        bool selfOnly = false;  // prim self (type + own props), no children
        // OutputCurves, OutputRegionMap and Output are one active source
        // graph: cage topology, categorical ownership and source relation
        // land in one SdfChangeBlock.
        bool outputPair = false;
    };

    TonicModel *_model;
    TonicCommitPaths _paths;
    // The per-version dirty set for the guide refill (plan/18 §7 G6).
    // Worker-thread only: every read and write happens inside _WorkerLoop's
    // call to TonicBuildCommitLayer, so it needs no lock of its own.
    TonicGuideCache _guideCache;
    // Output uses a density-scaled private snapshot. Keep it separate from
    // interactive Guides so toggling Output cannot evict their cache; width
    // is intentionally not part of this geometry key.
    TonicGuideCache _outputGuideCache;

    std::thread _worker;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    bool _stop = false;
    bool _paused = false;
    bool _detached = false;
    bool _building = false;  // worker holds a snapshot newer than `ready`
    // Latest enqueue: a version plus the UI-captured fill-in plan. The worker
    // takes these when it wakes (coalescing) and snapshots the live model
    // itself, so the snapshot always matches the version it builds.
    uint64_t _pendingVersion = 0;
    TonicFillPlan _pendingPlan;
    // The version whose build failed (or threw). The worker will not retry
    // it: the predicate below would otherwise spin on a persistent failure
    // at full speed. A newer enqueue clears it by being a newer version.
    uint64_t _failedVersion = 0;
    // Latest built layer and its version; replaced wholesale (the replaced
    // layer counts as dropped when it never swapped). A strong reference:
    // anonymous layers die with their last RefPtr, so a weak handle here
    // would expire the moment the worker's local goes out of scope.
    SdfLayerRefPtr _ready;
    uint64_t _readyVersion = 0;
    // Partial-transfer cursor into _partialSlots for _partialVersion.
    std::vector<_PartialSlot> _partialSlots;
    size_t _partialNext = 0;
    uint64_t _partialVersion = 0;

    std::atomic<uint64_t> _committedVersion{0};
    std::atomic<uint64_t> _pendingVersionAtomic{0};
    std::atomic<double> _swapBudgetMs{5.0};
    std::atomic<double> _lastSwapMs{0.0};
    std::atomic<size_t> _buildCount{0};
    std::atomic<size_t> _droppedCount{0};
    std::atomic<bool> _partialMode{false};
    std::atomic<int> _throwBuilds{0};
    std::atomic<size_t> _workerThrowCount{0};
    std::string _diagnostic;
};

} // namespace usdGenTonic

#endif // USDGEN_TONIC_COMMIT_H
