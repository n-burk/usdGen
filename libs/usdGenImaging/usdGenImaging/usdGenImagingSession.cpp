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
    if (_engine) _engine->SetContext(context);
}

void UsdGenImagingSession::Commit(usdGen::UsdGenCommitReason reason)
{
    if (!_engine) return;

    // The staged graph desc (if any) was SetGraphDesc'd by the owning index
    // before this call; the engine copies it and recompiles (03 §2.2).
    // NeedsDesc is consumed whether or not this run published: on supersede
    // the desc stays staged inside the engine and the leftover _dirty makes
    // the next commit recompile with it (03 §5.6).
    _needsDesc.store(false);

    const int64_t before = _generation;
    usdGen::UsdGenGenerationConstPtr published;
    // Engine Commit is serialized on the engine's commit mutex (03 §5.4);
    // when superseded mid-run it returns the PREVIOUS generation and leaves
    // the session dirty (03 §5.6) — id == before, so no notice storm here.
    usdGen::UsdGenGenerationConstPtr gen = _engine->Commit(_frame, reason);
    if (gen && gen->id != before) {
        _generation = gen->id;
        published = gen;
    }
    if (!published) return;

    // Copy under lock, invoke unlocked: a callback may re-enter the session
    // (GetPrim atomic_loads, Detach takes the store lock, not this one).
    std::vector<std::function<void(usdGen::UsdGenCommitReason)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(_republishMutex);
        callbacks.reserve(_callbacks.size());
        for (auto &entry : _callbacks) {
            callbacks.push_back(entry.second);
        }
    }
    for (auto &cb : callbacks) {
        cb(reason);
    }
}

int UsdGenImagingSession::RegisterRepublishCallback(
    std::function<void(usdGen::UsdGenCommitReason)> cb)
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
                           std::function<void(usdGen::UsdGenCommitReason)>>
                                   const &entry) {
                           return entry.first == token;
                       }),
        _callbacks.end());
}

int64_t UsdGenImagingSession::Generation() const noexcept { return _generation; }

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

namespace {

struct _SdfPathTextHash
{
    std::size_t operator()(SdfPath const &p) const noexcept
    {
        return std::hash<std::string>{}(p.GetText());
    }
};

// One registry, shared by SetStage/FindGroomStage; entries are weak so the
// registry never extends a stage's lifetime.
std::mutex &_StageRegistryMutex()
{
    static std::mutex mutex;
    return mutex;
}
std::unordered_map<SdfPath, UsdStageWeakPtr, _SdfPathTextHash>
&_StageRegistry()
{
    static std::unordered_map<SdfPath, UsdStageWeakPtr, _SdfPathTextHash> reg;
    return reg;
}

}  // namespace

void UsdGenSessionStore::SetStage(SdfPath const &groomRoot,
                                  UsdStageRefPtr const &stage)
{
    // Called from the UsdGenGroom prim adapter while the stage index builds
    // its prim data sources (06 §3.7 stage-key contract).
    std::lock_guard<std::mutex> lock(_StageRegistryMutex());
    _StageRegistry()[groomRoot] = stage;
}

UsdStageWeakPtr UsdGenSessionStore::FindGroomStage(SdfPath const &groomRoot)
{
    std::lock_guard<std::mutex> lock(_StageRegistryMutex());
    auto it = _StageRegistry().find(groomRoot);
    return it == _StageRegistry().end() ? UsdStageWeakPtr() : it->second;
}

}  // namespace usdGenImaging
