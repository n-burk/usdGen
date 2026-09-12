// usdGen imaging — session store implementation (06-imaging.md §3.7, §3.9).
//
// The wiring contract (06 §3.9): the renderer-level UsdGenGroomSceneIndex
// attaches a session in its constructor, routes observer notices through
// UsdGenDirtyRouter into UsdGenPendingDirty, and commits on triggers
// (a)/(b)/(c). GetPrim paths never commit — they atomic_load the latest
// generation (I7). Superseded commits leave the session dirty (03 §5.6).
#include "usdGenImaging/usdGenImagingSession.h"

#include "usdGen/generationStore.h"
#include "usdGen/session.h"

#include "pxr/pxr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/warning.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

PXR_NAMESPACE_USING_DIRECTIVE

namespace usdGenImaging {

UsdGenImagingSession::UsdGenImagingSession(
    UsdGenSessionKey const &key,
    std::shared_ptr<usdGen::UsdGenSession> engineSession,
    double frame,
    usdGen::UsdGenContext context)
    : _key(key),
      _engine(std::move(engineSession)),
      _frame(frame),
      _context(context)
{
}

void UsdGenImagingSession::SetTime(double frame) { _frame = frame; }

void UsdGenImagingSession::SetContext(usdGen::UsdGenContext context)
{
    _context = context;
}

void UsdGenImagingSession::Commit(usdGen::UsdGenCommitReason reason)
{
    _Commit(reason, nullptr);
}

void UsdGenImagingSession::StageAndCommit(
    usdGen::UsdGenGraphDesc const &desc, usdGen::UsdGenCommitReason reason)
{
    _Commit(reason, &desc);
}

void UsdGenImagingSession::_Commit(usdGen::UsdGenCommitReason reason,
                                  usdGen::UsdGenGraphDesc const *desc)
{
    if (!_engine) return;

    // Only the stager consumes _needsDesc. Clearing it here would discard
    // structural/reload marks arriving after its snapshot, or marks on a
    // store-driven commit that did not stage a fresh description at all.

    // Serialize session-side bookkeeping with the engine snapshot commit:
    // generation, report, diagnostics, stats and routing are copied from one
    // paired immutable result, so callbacks reread NOTHING live.
    // Hold _stageCommitMutex across the engine Commit: pairs with
    // StageAndCommit so staging+commit are atomic w.r.t. concurrent
    // session commits. Callbacks remain inside this serialization boundary
    // to preserve publication order, but do not hold _republishMutex.
    // They may pull generations or detach, but must not recursively commit.
    std::unique_lock<std::mutex> stageLock(_stageCommitMutex);
    _engine->SetContext(_context.load());
    if (desc) _engine->SetGraphDesc(*desc);
    const int64_t before = _generation.load();
    auto snapshot = _engine->CommitSnapshot(_frame.load(), reason);
    if (!snapshot) return;
    for (auto const& error : snapshot->diagnostics.errors)
        TF_WARN("usdGen commit rejected: %s", error.c_str());
    for (auto const& warning : snapshot->diagnostics.warnings)
        TF_WARN("usdGen commit: %s", warning.c_str());
    CommitPayload payload;
    usdGen::UsdGenGenerationConstPtr const &gen = snapshot->generation;
    if (gen && gen->id != before) {
        _generation = gen->id;
        payload.published = true;
        payload.generation = gen;
        payload.report = snapshot->report;
        payload.routing = snapshot->routing;
        payload.diagnostics = snapshot->diagnostics;
        payload.stats = snapshot->stats;
    }
    if (!payload.published) return;

    // Copy under callback lock, invoke without that lock: a callback may re-enter
    // (GetPrim atomic_loads, Detach takes the store lock, not this one).
    std::vector<std::function<void(CommitPayload const &)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(_republishMutex);
        callbacks.reserve(_callbacks.size());
        for (auto &entry : _callbacks) {
            callbacks.push_back(entry.second);
        }
    }
    for (auto &cb : callbacks) {
        cb(payload);
    }
}

void UsdGenImagingSession::StageDesc(usdGen::UsdGenGraphDesc const &desc)
{
    // Standalone staging is serialized but does not reserve a subsequent
    // commit. Production callers use StageAndCommit instead.
    std::lock_guard<std::mutex> lock(_stageCommitMutex);
    if (_engine) _engine->SetGraphDesc(desc);
}

int UsdGenImagingSession::RegisterRepublishCallback(
    std::function<void(CommitPayload const &)> cb)
{
    std::lock_guard<std::mutex> lock(_republishMutex);
    const int token = _nextCallbackToken++;
    _callbacks.emplace_back(token, std::move(cb));
    return token;
}

void UsdGenImagingSession::UnregisterRepublishCallback(int token)
{
    std::lock_guard<std::mutex> lock(_republishMutex);
    _callbacks.erase(
        std::remove_if(_callbacks.begin(), _callbacks.end(),
                       [token](std::pair<int,
                           std::function<void(CommitPayload const &)>>
                                   const &entry) {
                           return entry.first == token;
                       }),
        _callbacks.end());
}

int64_t UsdGenImagingSession::Generation() const noexcept { return _generation.load(); }

usdGen::UsdGenGenerationConstPtr UsdGenImagingSession::LatestGeneration() const noexcept
{
    return _engine ? _engine->Generation() : usdGen::UsdGenGenerationConstPtr();
}

int UsdGenImagingSession::AttachedIndices() const noexcept
{
    return _attached.load();
}
void UsdGenImagingSession::NoteAttach() { _attached.fetch_add(1); }
void UsdGenImagingSession::NoteDetach() { _attached.fetch_sub(1); }

// ---------------------------------------------------------------------------

UsdGenSessionStore &UsdGenSessionStore::GetInstance()
{
    static UsdGenSessionStore instance;
    return instance;
}

UsdGenSessionHandle UsdGenSessionStore::Attach(UsdGenSessionKey const &key)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(key);
    if (it != _sessions.end()) {
        it->second->NoteAttach();
        return it->second;
    }
    auto session = TfCreateRefPtr(new UsdGenImagingSession(
        key, std::make_shared<usdGen::UsdGenSession>()));
    _sessions.emplace(key, session);
    session->NoteAttach();
    return session;
}

void UsdGenSessionStore::Detach(UsdGenSessionKey const &key)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(key);
    if (it == _sessions.end()) return;
    it->second->NoteDetach();
    if (it->second->AttachedIndices() <= 0) {
        _sessions.erase(it);  // last index gone: store drops its strong ref
    }
}

UsdGenSessionHandle UsdGenSessionStore::Find(UsdGenSessionKey const &key) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(key);
    if (it != _sessions.end()) return it->second;
    return UsdGenSessionHandle();
}

std::vector<UsdGenImagingSessionRefPtr> UsdGenSessionStore::LiveSessions() const
{
    std::vector<UsdGenImagingSessionRefPtr> out;
    std::lock_guard<std::mutex> lock(_mutex);
    out.reserve(_sessions.size());
    for (auto &entry : _sessions) {
        out.push_back(entry.second);
    }
    return out;
}

void UsdGenSessionStore::SetTime(double frame)
{
    // The UsdGenImaging_SetTime C ABI (06 §6.1, item 9) forwards here: the
    // app owns time from now on (rule b deactivates, 06 §3.9).
    // Threading note: the commits run inline on the caller thread. Engine
    // commits serialize on the engine commit mutex and a superseded run
    // returns immediately (03 §5.6), so racing SetTime calls are safe; the
    // shipping ABI should marshal to the registered commit thread (06 §4.4).
    // The lock is NOT held across engine calls (S15).
    for (auto const &session : LiveSessions()) {
        session->MarkAppDriver();
        session->SetTime(frame);
        session->Commit(usdGen::UsdGenCommitReason::SetTime);
    }
}

void UsdGenSessionStore::SetContext(usdGen::UsdGenContext context)
{
    for (auto const &session : LiveSessions()) {
        session->SetContext(context);
    }
}

void UsdGenSessionStore::Commit(usdGen::UsdGenCommitReason reason)
{
    for (auto const &session : LiveSessions()) {
        session->Commit(reason);
    }
}

int64_t UsdGenSessionStore::Generation() const noexcept
{
    int64_t gen = -1;
    for (auto const &session : LiveSessions()) {
        gen = std::max(gen, session->Generation());
    }
    return gen;
}

void UsdGenSessionStore::ReloadMaps()
{
    // S13. The M1 engine exposes no session-level map-reload entry: the map
    // textureGeneration rides inside UsdGenGraphDesc::maps (06 §3.7), so the
    // reload is delivered by re-pulling the desc — MarkNeedsDesc routes the
    // next commit through the desc builder (descBuilder reads
    // ArAssetInfo::GetGeneration per map at build time).
    static std::once_flag warned;
    std::call_once(warned, []() {
        TF_WARN("usdGen ReloadMaps: no engine-level map reload hook in M1; "
                "marking sessions desc-dirty so the next commit re-resolves "
                "map textureGenerations (06 §3.7 S13).");
    });
    for (auto const &session : LiveSessions()) {
        session->MarkNeedsDesc();
    }
}

}  // namespace usdGenImaging
