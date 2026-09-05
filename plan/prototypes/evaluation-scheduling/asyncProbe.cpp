// Probe 4: does the asyncAllow/asyncPoll SystemMessage reach a filtering
// scene index buried in the chain, and are notices it emits during the poll
// observed by the caller (which is how UsdImagingGLEngine::
// PollForAsynchronousUpdates decides to redraw, engine.cpp:2608-2659)?
#include "pxr/pxr.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/systemMessages.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include <cstdio>
#include <vector>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

static std::vector<std::string> g_trace;

class _Node;
TF_DECLARE_REF_PTRS(_Node);
class _Node : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _NodeRefPtr New(const HdSceneIndexBaseRefPtr &in,
                           const std::string &n, bool isProducer=false) {
        return TfCreateRefPtr(new _Node(in, n, isProducer));
    }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetPrim(p); }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p); }
    int polls = 0, allows = 0;
protected:
    _Node(const HdSceneIndexBaseRefPtr &in, const std::string &n, bool p)
        : HdSingleInputFilteringSceneIndexBase(in), _n(n), _producer(p) {}
    void _PrimsAdded(const HdSceneIndexBase&,
        const HdSceneIndexObserver::AddedPrimEntries&e) override {_SendPrimsAdded(e);}
    void _PrimsRemoved(const HdSceneIndexBase&,
        const HdSceneIndexObserver::RemovedPrimEntries&e) override {_SendPrimsRemoved(e);}
    void _PrimsDirtied(const HdSceneIndexBase&,
        const HdSceneIndexObserver::DirtiedPrimEntries&e) override {_SendPrimsDirtied(e);}
    void _SystemMessage(const TfToken &t,
                        const HdDataSourceBaseHandle &) override {
        g_trace.push_back(_n + " <- " + t.GetString());
        if (t == HdSystemMessageTokens->asyncAllow) { ++allows; _async = true; }
        else if (t == HdSystemMessageTokens->asyncPoll && _async) {
            ++polls;
            if (_producer && polls <= 3) {
                // emit incremental progress, exactly what a progressive hair
                // generator would do
                _SendPrimsDirtied({{SdfPath("/Hair"),
                    HdDataSourceLocator(TfToken("primvars"))}});
                g_trace.push_back("    " + _n + " emitted PrimsDirtied");
            }
        }
    }
private:
    std::string _n; bool _producer; bool _async = false;
};

int main()
{
    HdRetainedSceneIndexRefPtr root = HdRetainedSceneIndex::New();
    root->AddPrims({{SdfPath("/Hair"), TfToken("basisCurves"),
                     HdRetainedContainerDataSource::New()}});
    _NodeRefPtr a = _Node::New(root, "upstreamA");
    _NodeRefPtr hair = _Node::New(a, "HAIR-PLUGIN", /*producer=*/true);
    _NodeRefPtr b = _Node::New(hair, "downstreamB");
    _NodeRefPtr terminal = _Node::New(b, "terminal");

    printf("=== SystemMessage(asyncAllow) from the terminal ===\n");
    terminal->SystemMessage(HdSystemMessageTokens->asyncAllow, nullptr);
    for (auto &s : g_trace) printf("  %s\n", s.c_str());

    printf("\n=== 4 x SystemMessage(asyncPoll), with a temp observer on the "
           "terminal (engine.cpp:2648-2655) ===\n");
    struct _Obs : public HdSceneIndexObserver {
        bool changed = false;
        void PrimsAdded(const HdSceneIndexBase&, const AddedPrimEntries&) override {changed=true;}
        void PrimsRemoved(const HdSceneIndexBase&, const RemovedPrimEntries&) override {changed=true;}
        void PrimsDirtied(const HdSceneIndexBase&, const DirtiedPrimEntries&) override {changed=true;}
        void PrimsRenamed(const HdSceneIndexBase&, const RenamedPrimEntries&) override {}
    };
    for (int i = 0; i < 4; ++i) {
        g_trace.clear();
        _Obs ob;
        terminal->AddObserver(HdSceneIndexObserverPtr(&ob));
        terminal->SystemMessage(HdSystemMessageTokens->asyncPoll, nullptr);
        terminal->RemoveObserver(HdSceneIndexObserverPtr(&ob));
        printf("  poll %d -> PollForAsynchronousUpdates would return %s\n",
               i, ob.changed ? "TRUE (redraw)" : "false (no redraw)");
        for (auto &s : g_trace) printf("      %s\n", s.c_str());
    }
    printf("\nHAIR-PLUGIN saw allows=%d polls=%d\n", hair->allows, hair->polls);
    return 0;
}
