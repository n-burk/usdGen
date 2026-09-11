// usdGen imaging — session key + session + process-global session store
// (06-imaging.md §3.7; ADR §4.5 "sessions across chains").
//
// Stage-free key (plan 13 §7): a nonempty usdGen:sessionId explicitly
// shares a groom session across renderer chains. Without one, the groom
// path is scoped to its render instance; unrelated stages never collide
// merely because they use the same path. No stage registry is consulted.
//
// The store holds the session alive (strong refs); scene indices hold weak
// handles and re-attach on construction (S15). A renderer switch therefore
// re-attaches to the same session and continues its generation counter.
#ifndef USDGEN_IMAGING_SESSION_H
#define USDGEN_IMAGING_SESSION_H

#include "usdGenImaging/api.h"
#include "usdGen/session.h"
#include "usdGen/types.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/refBase.h"
#include "pxr/base/tf/weakPtr.h"
#include "pxr/usd/sdf/path.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

struct UsdGenSessionKey
{
    std::string     sessionId;  // explicit cross-renderer sharing identity
    SdfPath         groomRoot;  // the UsdGenGroom prim path this session serves
    uint64_t        renderInstanceId = 0; // used only without sessionId

    bool operator==(UsdGenSessionKey const &rhs) const
    {
        return sessionId == rhs.sessionId && groomRoot == rhs.groomRoot &&
               (!sessionId.empty() || renderInstanceId == rhs.renderInstanceId);
    }
};

struct UsdGenSessionKeyHash
{
    std::size_t operator()(UsdGenSessionKey const &k) const
    {
        const std::size_t h0 = k.sessionId.empty()
            ? std::hash<uint64_t>()(k.renderInstanceId) : 0;
        const std::size_t h1 = std::hash<std::string>()(k.sessionId);
        const std::size_t h2 = std::hash<std::string>()(k.groomRoot.GetText());
        return h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 << 1 | h2 >> 31);
    }
};

/// One usdGen session: owns the engine session, the current frame/context,
/// and the publication generation counter. Several scene indices may attach
/// to one session (renderer switch, Storm + hdPrman preflight in one
/// process); the session owns ONE current frame, ONE context, ONE
/// generation — each attached index republishes the same generation. The
/// prim set is a session property and never differs per index.
class UsdGenImagingSession : public TfRefBase, public TfWeakBase
{
public:
    UsdGenImagingSession(
        UsdGenSessionKey const &key,
        std::shared_ptr<usdGen::UsdGenSession> engineSession,
        double frame = 0.0,
        usdGen::UsdGenContext context = usdGen::UsdGenContext::Interactive);

    UsdGenSessionKey const &Key() const { return _key; }
    usdGen::UsdGenSession *Engine() const { return _engine.get(); }

    /// Trigger (a)/(b) time source (06 §3.9). Frame-only: never commits.
    void SetTime(double frame);
    /// ADR §2.3 explicit context (session property, shared by all indices).
    void SetContext(usdGen::UsdGenContext context);
    /// Trigger (a) explicit commit (after a live-override change) and
    /// trigger (c) end-of-batch commit (06 §3.9, R32: unconditional).
    /// Publishes a generation only when the session is dirty or a graph desc
    /// was staged; the publication generation advances in lockstep with the
    /// engine's, so an app-driven commit reaches the app through the
    /// republish callbacks.
    void Commit(usdGen::UsdGenCommitReason reason);
    /// Install a freshly pulled descriptor and evaluate it under one lock.
    /// Unlike separate StageDesc/Commit calls, no store-driven commit can
    /// consume a different descriptor between those operations.
    void StageAndCommit(usdGen::UsdGenGraphDesc const &desc,
                        usdGen::UsdGenCommitReason reason);

    /// 06 §3.9 rule (b): once an external SetTime has driven this session,
    /// frame dirties never trigger commits (the app owns time). Set by the
    /// store-level SetTime only; scene indices never set it.
    void MarkAppDriver() noexcept { _hasAppDriver.store(true); }
    bool HasAppDriver() const noexcept { return _hasAppDriver.load(); }

    /// The staged UsdGenGraphDesc (built by the owning scene index) is to be
    /// consumed by the next Commit. Set on structural changes and map
    /// reloads (S13); never on frame-only dirties (S14: one desc pull per
    /// topology generation, not per frame).
    void MarkNeedsDesc() noexcept { _needsDesc.store(true); }
    bool NeedsDesc() const noexcept { return _needsDesc.load(); }

    /// Immutable per-commit payload for republish callbacks (P0 races):
    /// the generation published by THIS commit plus a COPY of its dirty
    /// report. Callbacks must never reread live Engine()->Generation() /
    /// LastReport() — a concurrent commit can replace them mid-iteration
    /// (report/generation pairing + data race). Empty published==false
    /// means "nothing published, no republish".
    struct CommitPayload {
        bool published = false;
        usdGen::UsdGenGenerationConstPtr generation;
        usdGen::UsdGenDirtyReport report;
    };

    /// Atomic desc-dirty consume for staging (S14): returns true and clears
    /// the flag when a desc pull is owed. Marks arriving after consumption
    /// stay set for the next commit (no lost ReloadMaps/structural marks).
    bool ConsumeNeedsDesc() noexcept
    {
        return _needsDesc.exchange(false);
    }

    /// Install a descriptor without committing (offline/test clients).
    /// Production staging uses StageAndCommit for an atomic pair.
    void StageDesc(usdGen::UsdGenGraphDesc const &desc);

    /// Monotonic publication generation (UsdGenImaging_GetGeneration).
    int64_t Generation() const noexcept;
    /// Latest published engine generation (lock-free atomic load, I7).
    usdGen::UsdGenGenerationConstPtr LatestGeneration() const noexcept;

    int AttachedIndices() const noexcept;
    void NoteAttach();
    void NoteDetach();
    /// Republish hooks (06 §3.7 "each index republishes the same
    /// generation"): invoked after a Commit that published, on the commit
    /// thread, with THIS commit's immutable payload. Callbacks must not
    /// block on GetPrim and must not reread live engine state.
    int RegisterRepublishCallback(
        std::function<void(CommitPayload const &)> cb);
    void UnregisterRepublishCallback(int token);
    UsdGenSessionKey        _key;
    std::shared_ptr<usdGen::UsdGenSession> _engine;
    std::atomic<double>       _frame{0.0};
    std::atomic<usdGen::UsdGenContext> _context{usdGen::UsdGenContext::Interactive};
    std::atomic<int64_t>      _generation{-1};
    std::atomic<int>          _attached{0};
    std::atomic<bool>         _hasAppDriver{false};
    std::atomic<bool>         _needsDesc{false};

    // Callbacks are copied under _republishMutex and invoked with the lock
    // released, so a callback may re-enter the session (GetPrim, Detach).
    // Staging serialization (Review 2): StageDesc + Commit run under
    // _stageCommitMutex so a concurrent store-level commit cannot
    // interleave between staging and commit. Never _stateMutex-adjacent.
    std::mutex _republishMutex;
    std::mutex _stageCommitMutex;
    std::vector<std::pair<int, std::function<void(CommitPayload const &)>>>
        _callbacks;
    int _nextCallbackToken = 0;

private:
    void _Commit(usdGen::UsdGenCommitReason reason,
                 usdGen::UsdGenGraphDesc const *desc);
};

using UsdGenImagingSessionRefPtr = TfRefPtr<UsdGenImagingSession>;
using UsdGenSessionHandle = UsdGenImagingSessionRefPtr;

/// Process-global session store (06 §3.7).
class UsdGenSessionStore
{
public:
    static UsdGenSessionStore &GetInstance();

    /// Find-or-create the session for \p key; increments its attach count.
    /// The engine session is created with the thread limit resolved by the
    /// engine (USDGEN_THREAD_LIMIT / one-shot calibration).
    UsdGenSessionHandle Attach(UsdGenSessionKey const &key);
    /// Decrements the attach count; drops the store's strong reference at
    /// zero so the session is destroyed once every index detaches.
    void Detach(UsdGenSessionKey const &key);
    UsdGenSessionHandle Find(UsdGenSessionKey const &key) const;

    /// Registry-level forwarding (06 §3.7): applied to every live session.
    /// SetTime additionally marks every session app-driven (06 §3.9 rule b)
    /// and commits it at the new frame — the future UsdGenImaging_SetTime
    /// C ABI (06 §6.1) forwards here.
    void SetTime(double frame);
    void SetContext(usdGen::UsdGenContext context);
    void Commit(usdGen::UsdGenCommitReason reason);
    int64_t Generation() const noexcept;  // max over live sessions
    /// S13: ArNotice::ResolverChanged + bump every map's textureGeneration
    /// (usdGenImaging owns the counter handoff to the engine via
    /// UsdGenGraphDesc::maps). No session-level map-reload hook exists in
    /// the M1 engine, so this marks every session desc-dirty (the next
    /// commit re-pulls the desc, whose maps carry the bumped generation)
    /// and warns once.
    void ReloadMaps();

    /// Snapshot of the live sessions (strong refs), for iteration without
    /// holding the store lock across engine calls.
    std::vector<UsdGenImagingSessionRefPtr> LiveSessions() const;

private:
    UsdGenSessionStore() = default;
    UsdGenSessionStore(const UsdGenSessionStore &) = delete;
    UsdGenSessionStore &operator=(const UsdGenSessionStore &) = delete;

    mutable std::mutex _mutex;  // released before any re-entrant call
    std::unordered_map<UsdGenSessionKey, UsdGenImagingSessionRefPtr,
                       UsdGenSessionKeyHash> _sessions;
};

}  // namespace usdGenImaging

#endif  // USDGEN_IMAGING_SESSION_H
