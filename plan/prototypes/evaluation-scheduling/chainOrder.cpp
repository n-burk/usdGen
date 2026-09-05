// Probe 2: where does a renderer-level scene index plugin land relative to
// HdsiSceneGlobalsSceneIndex (registered by UsdImagingGLEngine at phase 0,
// InsertionOrderAtStart -- engine.cpp:182-193) and hdGp?
//
// We do NOT create a render index (needs a render delegate / GL). We call
// HdSceneIndexPluginRegistry::AppendSceneIndicesForRenderer directly, which
// is what HdRenderIndex::HdRenderIndex does at renderIndex.cpp:206-214.
#include "pxr/pxr.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hdsi/sceneGlobalsSceneIndex.h"

#include <cstdio>
#include <vector>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

class _Tag;
TF_DECLARE_REF_PTRS(_Tag);
class _Tag : public HdSingleInputFilteringSceneIndexBase
{
public:
    static _TagRefPtr New(const HdSceneIndexBaseRefPtr &in,
                          const std::string &name) {
        _TagRefPtr r = TfCreateRefPtr(new _Tag(in));
        r->SetDisplayName(name);
        return r;
    }
    HdSceneIndexPrim GetPrim(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetPrim(p); }
    SdfPathVector GetChildPrimPaths(const SdfPath &p) const override {
        return _GetInputSceneIndex()->GetChildPrimPaths(p); }
protected:
    _Tag(const HdSceneIndexBaseRefPtr &in)
        : HdSingleInputFilteringSceneIndexBase(in) {}
    void _PrimsAdded(const HdSceneIndexBase&,
        const HdSceneIndexObserver::AddedPrimEntries&e) override {_SendPrimsAdded(e);}
    void _PrimsRemoved(const HdSceneIndexBase&,
        const HdSceneIndexObserver::RemovedPrimEntries&e) override {_SendPrimsRemoved(e);}
    void _PrimsDirtied(const HdSceneIndexBase&,
        const HdSceneIndexObserver::DirtiedPrimEntries&e) override {_SendPrimsDirtied(e);}
};

void _Walk(const HdSceneIndexBaseRefPtr &si, int depth)
{
    if (!si) return;
    printf("%*s%s\n", depth*2, "", si->GetDisplayName().c_str());
    if (const HdFilteringSceneIndexBase *f =
            dynamic_cast<const HdFilteringSceneIndexBase *>(get_pointer(si))) {
        for (const HdSceneIndexBaseRefPtr &in : f->GetInputScenes())
            _Walk(in, depth+1);
    }
}

} // namespace

int main(int argc, char **argv)
{
    const std::string renderer = argc > 1 ? argv[1] : "GL";

    // Mimic UsdImagingGLEngine's app scene indices (engine.cpp:146-193):
    // phase 0, InsertionOrderAtStart, empty renderer string => all renderers.
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        std::string(),
        [](const std::string &, const HdSceneIndexBaseRefPtr &in,
           const HdContainerDataSourceHandle &) -> HdSceneIndexBaseRefPtr {
            HdSceneIndexBaseRefPtr si =
                HdsiSceneGlobalsSceneIndex::New(in);
            si->SetDisplayName("APP: HdsiSceneGlobalsSceneIndex (phase 0/AtStart)");
            return si;
        },
        nullptr, /*phase=*/0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);

    // Candidate registrations for the hair plugin, at several phases.
    const int phases[] = {0, 1, 2, 3, 10};
    for (int p : phases) {
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            std::string(),
            [p](const std::string &, const HdSceneIndexBaseRefPtr &in,
                const HdContainerDataSourceHandle &) -> HdSceneIndexBaseRefPtr {
                return _Tag::New(in,
                    TfStringPrintf("HAIR-CANDIDATE phase %d (AtEnd)", p));
            },
            nullptr, p, HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
    }

    printf("=== JSON-registered plugin ids for renderer '%s' (in run order) ===\n",
           renderer.c_str());
    for (const TfToken &t :
            HdSceneIndexPluginRegistry::GetInstance()
                .LoadAndGetSceneIndexPluginIds(renderer, std::string())) {
        printf("  %s\n", t.GetText());
    }

    HdRetainedSceneIndexRefPtr input = HdRetainedSceneIndex::New();
    input->SetDisplayName("INPUT (stands for post-merging notice batching SI)");

    HdSceneIndexBaseRefPtr terminal =
        HdSceneIndexPluginRegistry::GetInstance()
            .AppendSceneIndicesForRenderer(renderer, input, "probe", "");

    printf("\n=== resolved chain, terminal first (upstream indented) ===\n");
    _Walk(terminal, 0);
    return 0;
}
