#include "usdGenImaging/usdGenEngineBridge.h"

#include "pxr/base/tf/diagnostic.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace usdGenImaging {

void UsdGenEngineBridge::RegisterCommitThread(std::thread::id tid)
{
    bool expected = false;
    if (_hasCommitThread.compare_exchange_strong(expected, true))
    {
        _commitThread = tid;
    }
}

bool UsdGenEngineBridge::HasCommitThread() const
{
    return _hasCommitThread.load();
}

bool UsdGenEngineBridge::IsCommitThread() const
{
    return !_hasCommitThread.load() || std::this_thread::get_id() == _commitThread;
}

void UsdGenEngineBridge::GateCommit(SdfPath const &descriptionPath, char const *where)
{
    if (!_hasCommitThread.load())
    {
        RegisterCommitThread(std::this_thread::get_id());
        return;
    }
    if (std::this_thread::get_id() != _commitThread)
    {
        TF_CODING_ERROR(
            "usdGen: engine commit for '%s' from a thread other than the registered "
            "commit thread (%s); Hydra commit must stay on the scene-authoring thread "
            "(E-2).",
            descriptionPath.GetText(),
            where);
    }
}

void UsdGenEngineBridge::SetTimingHook(TimingHook hook)
{
    std::lock_guard<std::mutex> lock(_hookMutex);
    _timingHook = std::move(hook);
}

void UsdGenEngineBridge::DrainStats(usdGen::UsdGenStats const &stats)
{
    TimingHook hook;
    {
        std::lock_guard<std::mutex> lock(_hookMutex);
        hook = _timingHook;  // copy: call outside the lock
    }
    if (!hook)
    {
        // Still advance the cursor so entries do not replay on first hook set.
        _seenEntries = stats.ringHead;
        _firstDrain = false;
        return;
    }

    const size_t n = stats.ring.size();
    const size_t head = stats.ringHead;
    size_t first = _firstDrain ? head : _seenEntries;  // never replay history on install
    for (size_t i = first; i != head; i = (i + 1) % n)
    {
        hook(stats.ring[i]);
    }
    _seenEntries = head;
    _firstDrain = false;
}

}  // namespace usdGenImaging

PXR_NAMESPACE_CLOSE_SCOPE
