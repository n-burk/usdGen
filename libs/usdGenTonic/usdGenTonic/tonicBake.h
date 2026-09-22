// usdGenTonic — the asynchronous region-map bake (plan/17 §3.1a, P2).
//
// A second write-behind pipeline beside the §3.1 stage pipeline, with the
// same rule: nothing in it ever runs on the UI thread or on the interaction
// stream, and nothing in §3.1 waits for it.
//
//   graph/hierarchy edit -> Enqueue(mapVersion m)
//     bake worker: rasterise dirty faces (K3, bakeStream) -> D2H (pinned,
//                  event) -> write regionMap.v<m>.ptx (tmp -> rename)
//     UI idle:     author usdGen:map:file = regionMap.v<m>.ptx on
//                  <groom>/RegionMap (one attribute, one ChangeBlock)
//
// Own version counter (bumps only on graph/hierarchy edits), own CUDA stream
// at the lowest priority (never tonicStream), own worker thread with
// coalescing (takes the latest mapVersion when it wakes; a superseded bake
// cancels at the next ~4K-face tile boundary), incremental rasterisation
// (an exact classifier-input repeat reuses texels; a changed graph or
// hierarchy refreshes them; the Ptex writer still writes the full file), and
// versioned filenames (the swap never points at a
// half-written file, and usdGen's imageMapCache, keyed by path, never serves
// a stale map).
//
// Channel layout: [L1 region, L2 tube, L3 tube, ...], one float channel per
// hierarchy level (plan/17 §2.2). Channel 0 is the scalp-graph interpolation
// id; channel k is the id of the level-(k+1) tube whose K14 cell the texel
// falls in, 0 where that level has no tube there (V0b: the hierarchy comes
// in through TonicBakeInput::tubes). Ids are integers stored as floats
// (GuideInterpolate groups values within 1/1024), `nearest` filtering keeps
// borders hard.
#ifndef USDGEN_TONIC_BAKE_H
#define USDGEN_TONIC_BAKE_H

#include "usdGenTonic/api.h"
#include "usdGenTonic/tonicGraph.h"
#include "usdGenTonic/tonicRegion.h"
#include "usdGenTonic/tonicScalp.h"
#include "usdGenTonic/tonicTube.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace usdGenTonic {

// One tube's share of the level channels (V0b, plan/18 §7 G4). Channel k of
// the map is the id of the level-(k+1) tube a texel falls in, and the K14
// partition that decides it is planar in the parent's root frame, so the
// bake only needs each tube's root frame, root center and links — never its
// full geometry.
struct USDGENTONIC_API TonicBakeTube {
    int tubeId = 0;
    int parentTubeId = -1;  // -1 at L1
    int level = 1;
    int regionId = -1;  // L1 only: the scalp-graph region this tube roots in
    int childIndex = -1;
    float rootCenter[3] = {0.0f, 0.0f, 0.0f};
    TonicFrame rootFrame;
};

// Everything one bake needs: plain data, no model access, safe to move to
// the worker. The mesh is shared (a 60K-face copy per enqueue would be pure
// waste); the graph is small and copied.
struct USDGENTONIC_API TonicBakeInput {
    std::shared_ptr<TonicScalpMesh const> scalp;
    TonicScalpGraph graph;
    int levelCount = 1;  // hierarchy levels baked as channels
    int resOverride = -1;  // per-face texel res override, or -1 for auto
    std::string outDir;  // versioned .ptx files land here
    std::string baseName = "regionMap";  // regionMap.v<m>.ptx
    // A dedicated one-channel categorical map for Output CurveSource.  Its
    // texels are the deepest live tube id plus one, or zero outside a Tonic
    // root support.  This deliberately differs from RegionMap channel zero,
    // which holds graph interpolation ids.
    bool outputOwnerMap = false;
    // The hierarchy that fills channels >= 1. Parents before children; a
    // bridge import is not a subdivision cell and does not belong here.
    // Empty leaves every level channel at 0, which is what a groom with no
    // hierarchy means.
    std::vector<TonicBakeTube> tubes;
};

class TonicModel;

// Collect the subdivision cells the level channels need (UI thread, at
// enqueue): every tube in the model except the bridge imports, which are
// explicit geometry rather than a cell of their parent's partition. The
// root frames come from K4 over each tube's center curve.
USDGENTONIC_API void TonicCollectBakeTubes(TonicModel const &model,
                                           std::vector<TonicBakeTube> *out);

// Bakes the input to `outPath` synchronously (the worker's unit of work,
// also the T1 test's direct entry point). Writes to a temp file and renames,
// so `outPath` never names a half-written file. `dirtyFaces` (when non-null)
// restricts texel classification to those coarse faces; cached texels for
// the rest come from `texelCache` (indexed by ptex face id, resized as
// needed). Cache entries whose size misses the plan (a res flip, a level
// change, a fresh cache) re-classify regardless of the dirty set. Reports
// per-face classification counts for the incremental test. Returns false
// with *err set on any failure.
struct USDGENTONIC_API TonicBakeStats {
    size_t facesClassified = 0;  // coarse faces texel-classified this run
    size_t texelsClassified = 0;
    size_t ptexFaces = 0;
    int channels = 1;
};
USDGENTONIC_API bool TonicBakePtex(
    TonicBakeInput const &input, std::string const &outPath,
    std::vector<int> const *dirtyFaces,
    std::vector<std::vector<float>> *texelCache, TonicBakeStats *stats,
    std::string *err);

// Versioned filename for a map version (no directory join).
USDGENTONIC_API std::string TonicBakeFileName(std::string const &baseName,
                                              uint64_t mapVersion);

class USDGENTONIC_API TonicBakeWorker {
public:
    TonicBakeWorker();
    ~TonicBakeWorker();
    TonicBakeWorker(TonicBakeWorker const &) = delete;
    TonicBakeWorker &operator=(TonicBakeWorker const &) = delete;

    // Request a bake of `input` as map version `mapVersion` (UI thread, at
    // release after a graph/hierarchy edit). Stores only the latest input;
    // the worker takes it when it wakes (coalescing).
    void Enqueue(uint64_t mapVersion, TonicBakeInput input);

    // Latest bake finished by the worker (UI thread, at idle). Returns false
    // when no bake has completed since the last TakeCompleted.
    bool TakeCompleted(uint64_t *mapVersion, std::string *path);

    // Tell the worker which version the UI swapped (UI thread, after the
    // one-attribute swap). Older versioned files the live layer no longer
    // references are deleted by the worker.
    void NoteSwapped(uint64_t mapVersion, std::string const &path);

    uint64_t PendingVersion() const { return _pendingVersionAtomic.load(); }
    uint64_t CompletedVersion() const
    {
        return _completedVersionAtomic.load();
    }
    // Test introspection: bakes finished, bakes cancelled at a tile boundary
    // because a newer version arrived, and coarse faces classified by the
    // last finished bake (the incremental proof).
    size_t BakeCount() const { return _bakeCount.load(); }
    size_t CancelCount() const { return _cancelCount.load(); }
    size_t LastBakedFaces() const { return _lastBakedFaces.load(); }
    // Stale-file sweeps the worker ran (a bake completion and every
    // NoteSwapped that frees an older file trigger one).
    size_t SweepCount() const { return _sweepCount.load(); }
    // Exceptions the worker loop caught and survived (plan/18 §7 G5).
    size_t WorkerThrowCount() const { return _workerThrowCount.load(); }
    // Test hook for the §3.4 failure mode: make the next N bakes throw.
    void ThrowOnNextBakesForTest(int count);
    // Test hook: hold the worker between bakes so coalescing is
    // deterministic (enqueue N versions, resume, expect one bake).
    void PauseWorker(bool pause);
    // Test hook: wait for a version to complete (worker-side polling).
    bool WaitCompleted(uint64_t mapVersion, int timeoutMs = 10000);
    std::string TakeDiagnostic();

private:
    void _WorkerLoop();
    // One bake, chunked at ~4K-face tile boundaries with a supersede check
    // between chunks. Returns false when cancelled (no file written, empty
    // error) or on failure (error set). `bakeStream` is the worker's
    // lowest-priority CUDA stream, or null for the CPU lane.
    bool _Bake(uint64_t mapVersion, TonicBakeInput const &input,
               std::string *outPath, std::string *err, void *bakeStream);
    // Delete versioned files older than the newest completion that the live
    // layer no longer references. Runs after a completed bake AND on the
    // NoteSwapped wake, which is what makes the last-but-one file go away
    // when no further bake follows.
    void _SweepStale(std::string const &outDir, std::string const &baseName);

    std::thread _worker;
    mutable std::mutex _mutex;
    std::condition_variable _wake;
    bool _stop = false;
    bool _paused = false;
    bool _baking = false;
    uint64_t _pendingVersion = 0;
    TonicBakeInput _pendingInput;
    bool _hasPending = false;
    // Latest completion (version + final path), consumed by TakeCompleted.
    // Only the latest is kept: the UI swaps the newest file, never a queue.
    uint64_t _completedVersion = 0;
    std::string _completedPath;
    bool _hasCompleted = false;
    // Incremental state, worker-thread-only: per-face ids from the last
    // finished bake (the dirty set is the diff) and the per-ptex-face texel
    // cache. `_cacheDir` also holds the private classifier-cache identity.
    std::vector<int> _cachedFaceRegion;
    std::vector<std::vector<float>> _texelCache;
    std::string _cacheDir;
    uint64_t _swappedVersion = 0;
    std::string _swappedPath;
    // Set by NoteSwapped, consumed by the worker: a swap can free an older
    // versioned file even when no bake follows.
    bool _sweepRequested = false;
    std::string _lastOutDir;
    std::string _lastBaseName;

    std::atomic<uint64_t> _pendingVersionAtomic{0};
    std::atomic<uint64_t> _completedVersionAtomic{0};
    std::atomic<size_t> _bakeCount{0};
    std::atomic<size_t> _cancelCount{0};
    std::atomic<size_t> _lastBakedFaces{0};
    std::atomic<size_t> _sweepCount{0};
    std::atomic<size_t> _workerThrowCount{0};
    std::atomic<int> _throwBakes{0};
    std::string _diagnostic;
};

}  // namespace usdGenTonic

#endif  // USDGEN_TONIC_BAKE_H
