// Hydra notices belong to the serialized frontend, never a scene worker.
#include "usdGenImaging/groomSceneIndexPlugin.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/systemMessages.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
int failures=0;
void Check(bool okay,char const* message) {
    std::printf("%s: %s\n",okay?"ok":"FAIL",message);
    if(!okay) ++failures;
}
class Observer final : public HdSceneIndexObserver {
public:
    std::thread::id frontend=std::this_thread::get_id();
    std::atomic<bool> wrongThread{false};
    std::vector<std::string> events; // frontend only, even on a failing test
    UsdGenGroomSceneIndex* owner=nullptr;
    bool synchronizeRejected=false;
    std::function<void()> onAdd;
    bool OnFrontend() {
        if(std::this_thread::get_id()==frontend) return true;
        wrongThread.store(true); return false;
    }
    void PrimsAdded(HdSceneIndexBase const&,AddedPrimEntries const& entries) override {
        if(!OnFrontend()) return;
        for(auto const& entry:entries) events.push_back("A:"+entry.primPath.GetString());
        try { owner->Synchronize(); }
        catch(std::logic_error const&) { synchronizeRejected=true; }
        auto action=std::move(onAdd); onAdd={};
        if(action) action();
    }
    void PrimsRemoved(HdSceneIndexBase const&,RemovedPrimEntries const& entries) override {
        if(!OnFrontend()) return;
        for(auto const& entry:entries) events.push_back("R:"+entry.primPath.GetString());
    }
    void PrimsDirtied(HdSceneIndexBase const&,DirtiedPrimEntries const& entries) override {
        if(!OnFrontend()) return;
        for(auto const& entry:entries) events.push_back("D:"+entry.primPath.GetString());
    }
    void PrimsRenamed(HdSceneIndexBase const&,RenamedPrimEntries const&) override {}
};
bool PollUntil(HdSceneIndexBase& index,std::function<bool()> const& predicate) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    do {
        index.SystemMessage(HdSystemMessageTokens->asyncPoll,nullptr);
        if(predicate()) return true;
        std::this_thread::yield();
    } while(std::chrono::steady_clock::now()<deadline);
    return false;
}
}
int main() {
    auto input=HdRetainedSceneIndex::New();
    auto index=UsdGenGroomSceneIndex::New(input,6101);
    auto* owner=dynamic_cast<UsdGenGroomSceneIndex*>(index.operator->());
    if(!owner) return 1;
    Observer observer; observer.owner=owner;
    index->AddObserver(TfCreateWeakPtr(&observer));
    auto add=[&](char const* path,char const* type="Scope") {
        input->AddPrims({{SdfPath(path),TfToken(type),HdRetainedContainerDataSource::New()}});
    };
    index->SystemMessage(HdSystemMessageTokens->asyncPoll,nullptr);
    Check(observer.events.empty(),"poll before opt-in is harmless");
    add("/ordinary");
    Check(observer.events==std::vector<std::string>{"A:/ordinary"} && !observer.wrongThread,
          "filtered default notice arrives synchronously on original caller thread");
    Check(observer.synchronizeRejected,"explicit Synchronize re-entry is rejected");
    observer.events.clear();
    observer.onAdd=[&] { add("/nested"); };
    add("/outer");
    Check(observer.events==std::vector<std::string>{"A:/outer","A:/nested"},
          "default reentrant edit is delivered after outer packet and before return");
    index->SystemMessage(HdSystemMessageTokens->asyncAllow,nullptr);
    observer.events.clear();
    add("/deferred");
    Check(observer.events.empty() && !observer.wrongThread,
          "async ingress does not emit notices before polling");
    Check(PollUntil(*index,[&] { return !observer.events.empty(); }),"asyncPoll delivers completed work");
    Check(observer.events==std::vector<std::string>{"A:/deferred"} && !observer.wrongThread,
          "async notice is delivered once on polling thread");
    observer.events.clear();
    input->RemovePrims({{SdfPath("/deferred")}});
    add("/deferred");
    Check(PollUntil(*index,[&] { return observer.events.size()>=2; }),"poll drains replacement");
    Check(observer.events==std::vector<std::string>{"R:/deferred","A:/deferred"},
          "one event sequence proves removal precedes replacement addition");
    // Authored roots are live upstream; synthetic namespaces advance only
    // with the corresponding frontend publication packet.
    observer.events.clear();
    add("/asyncGroom","UsdGenGroom");
    Check(index->GetChildPrimPaths(SdfPath("/asyncGroom")).empty(),
          "synthetic render scope is invisible before async delivery");
    Check(PollUntil(*index,[&] {
        return index->GetChildPrimPaths(SdfPath("/asyncGroom"))==
            SdfPathVector{SdfPath("/asyncGroom/__usdGenRender")};
    }),"poll installs matching synthetic snapshot");
    observer.events.clear();
    input->RemovePrims({{SdfPath("/asyncGroom")}});
    owner->Synchronize();
    Check(index->GetChildPrimPaths(SdfPath("/asyncGroom")).empty(),
          "explicit frontend Synchronize installs completed removal");
    Check(!observer.wrongThread,"no notification escaped onto a worker");
    index->RemoveObserver(TfCreateWeakPtr(&observer));
    index.Reset(); UsdGenGroomSceneIndex::DrainRetired();
    return failures?1:0;
}
