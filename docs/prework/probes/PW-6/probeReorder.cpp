// PW-6 / SI-11 (record only): notice behavior for 'reorder nameChildren' under
// a usdGen Description with the usdGen prim adapters attached.
//
// Stage shape (usdGenReorder.usda):
//   /Groom (UsdGenGroom)
//     /Groom/Description (UsdGenDescription)
//       /Groom/Description/Ops (Xform)
//         OpNoise   (UsdGenNoise)
//         OpLength  (UsdGenLength)
//         OpScatter (UsdGenScatter)
//
// Method:
//   1. Open the stage, build the usdImaging chain via UsdImagingCreateSceneIndices
//      (PXR_PLUGINPATH_NAME includes the usdGen build-tree plugin resources, so
//      the 5 usdGen prim adapters + 2 usdGen scene-index plugins are registered).
//   2. Attach one recording HdSceneIndexObserver to EVERY index in the chain
//      (terminal + all filtering inputs), so we can see both what the usd
//      stage scene index emits and whether any usdGen index/adapter emits.
//   3. Control edit (usdGen:magnitude on OpNoise) to prove the observer
//      wiring actually records notices for plugin-field edits.
//   4. Variant A: Sdf-level `reorder nameChildren` —
//      SdfPrimSpecifier::SetNameChildrenOrder on /Groom/Description/Ops.
//      Then UsdImagingStageSceneIndex::ApplyPendingUpdates().
//   5. Variant B: Usd-level reorder opinion —
//      UsdPrim::SetChildrenReorder (authors the primOrder metadata).
//      Then ApplyPendingUpdates().
//   6. Print the composed child order at the stage and at the terminal index
//      after each step.
//
// NOTE on the roadmap wording: `UsdAPITable::ReorderChild` does not exist in
// OpenUSD 26.08's public API (verified by grep of include/pxr). The two public
// reorder entry points in this build are:
//   - SdfPrimSpecifier::SetNameChildrenOrder  (reorder nameChildren statement)
//   - UsdPrim::SetChildrenReorder            (primOrder metadata opinion)
// Both are exercised here.
//
#include "pxr/pxr.h"

#include "pxr/base/arch/demangle.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/tf/type.h"
#include "pxr/base/plug/registry.h"

#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"

#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/sdf/primSpec.h"

#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"

#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Explicitly register the usdGen build-tree plugin resources (belt) in
// addition to PXR_PLUGINPATH_NAME (suspence): plain TfType::FindByName did
// not lazily pick up the PXR_PLUGINPATH_NAME entries in this build, so we
// register deterministically. Mirrors testUsdGenPluginDiscovery.
void
RegisterUsdGenPlugins()
{
    static const std::vector<std::string> kPaths = {
        "/home/burkard/work/usdGen/build/usd/usdGenSchema/resources",
        "/home/burkard/work/usdGen/build/usd/usdGenImaging/resources"};
    PlugRegistry::GetInstance().RegisterPlugins(kPaths);
}

const SdfPath kOps = SdfPath("/Groom/Description/Ops");
const SdfPath kNoise = SdfPath("/Groom/Description/Ops/OpNoise");

struct Recorder {
    std::string label;
    int added = 0;
    int removed = 0;
    int renamed = 0;
    int dirtied = 0;
    std::vector<std::string> lines;
    void Line(const std::string &s) { lines.push_back(s); }
    void Reset() {
        added = removed = renamed = dirtied = 0;
        lines.clear();
    }
    void Dump(const char *phase) const {
        std::printf("[%s] %-34s added=%d removed=%d renamed=%d dirtied=%d\n",
                    phase, label.c_str(), added, removed, renamed, dirtied);
        for (const std::string &l : lines) std::printf("    %s\n", l.c_str());
        std::printf("\n");
    }
};

class NoticeRecorder : public HdSceneIndexObserver {
public:
    explicit NoticeRecorder(Recorder *r) : _r(r) {}

    void PrimsAdded(const HdSceneIndexBase &,
                    const AddedPrimEntries &entries) override
    {
        for (const AddedPrimEntry &e : entries) {
            _r->added++;
            _r->Line("ADDED    " + e.primPath.GetString() + "  type=" +
                     e.primType.GetString());
        }
    }
    void PrimsRemoved(const HdSceneIndexBase &,
                      const RemovedPrimEntries &entries) override
    {
        for (const RemovedPrimEntry &e : entries) {
            _r->removed++;
            _r->Line("REMOVED  " + e.primPath.GetString());
        }
    }
    void PrimsRenamed(const HdSceneIndexBase &,
                      const RenamedPrimEntries &entries) override
    {
        for (const RenamedPrimEntry &e : entries) {
            _r->renamed++;
            _r->Line("RENAMED  " + e.oldPrimPath.GetString() + " -> " +
                     e.newPrimPath.GetString());
        }
    }
    void PrimsDirtied(const HdSceneIndexBase &,
                      const DirtiedPrimEntries &entries) override
    {
        for (const DirtiedPrimEntry &e : entries) {
            _r->dirtied++;
            std::string locs;
            for (const HdDataSourceLocator &l : e.dirtyLocators) {
                if (!locs.empty()) locs += ",";
                locs += l.GetString();
            }
            _r->Line("DIRTIED  " + e.primPath.GetString() +
                     (locs.empty() ? "  locs=<empty>"
                                   : std::string("  locs=[") + locs + "]"));
        }
    }

private:
    Recorder *_r;
};

std::string
_Label(const HdSceneIndexBaseRefPtr &si)
{
    std::string disp = si->GetDisplayName();
    std::string type = ArchGetDemangled(typeid(*get_pointer(si)).name());
    const size_t p = type.rfind("::");
    if (p != std::string::npos) type = type.substr(p + 2);
    if (disp.empty() || disp == type) return type;
    return type + " [\"" + disp + "\"]";
}

// Collect the terminal and every filtering input, outermost first.
void
_CollectChain(const HdSceneIndexBaseRefPtr &terminal,
              std::vector<HdSceneIndexBaseRefPtr> *out)
{
    out->push_back(terminal);
    if (HdFilteringSceneIndexBaseRefPtr f =
            TfDynamic_cast<HdFilteringSceneIndexBaseRefPtr>(terminal)) {
        for (const HdSceneIndexBaseRefPtr &in : f->GetInputScenes())
            _CollectChain(in, out);
    }
}

std::string
_JoinTokens(const TfTokenVector &names)
{
    std::string s;
    for (const TfToken &t : names) {
        if (!s.empty()) s += " ";
        s += t.GetString();
    }
    return s.empty() ? "<no children>" : s;
}

void
_PrintOrders(const char *tag, const UsdStageRefPtr &stage,
             const HdSceneIndexBaseRefPtr &terminal)
{
    std::printf("%s stage child order of %s : %s\n", tag, kOps.GetText(),
                _JoinTokens(stage->GetPrimAtPath(kOps).GetAllChildrenNames()).c_str());
    if (terminal) {
        std::string torder;
        for (const SdfPath &p : terminal->GetChildPrimPaths(kOps)) {
            if (!torder.empty()) torder += " ";
            torder += p.GetName();
        }
        std::printf("%s terminal child order of %s : %s\n", tag,
                    kOps.GetText(),
                    torder.empty() ? "<no children>" : torder.c_str());
    }
}

} // namespace

int
main(int argc, char **argv)
{
    if (argc < 2) {
        std::printf("usage: probeReorder <stage.usda>\n");
        return 2;
    }

    RegisterUsdGenPlugins();

    std::printf("=== registered usdGen plugin types ===\n");
    const char *usdGenTypes[] = {
        "UsdGenGroomAdapter",
        "UsdGenDescriptionAdapter",
        "UsdGenOperatorAdapter",
        "UsdGenMapAdapter",
        "UsdGenGuideSetAdapter",
        "UsdGenRestAPIAdapter",
        "UsdGenGroomSceneIndexPlugin",
        "UsdGenMetadataSceneIndexPlugin",
    };
    for (const char *name : usdGenTypes) {
        const TfType t = TfType::FindByName(name);
        std::printf("  %-34s %s\n", name,
                    t.IsUnknown() ? "not registered" : "REGISTERED");
    }
    std::printf("\n");

    UsdStageRefPtr stage = UsdStage::Open(std::string(argv[1]));
    if (!stage) {
        std::printf("FAILED to open %s\n", argv[1]);
        return 1;
    }

    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    const UsdImagingSceneIndices sis = UsdImagingCreateSceneIndices(info);
    const HdSceneIndexBaseRefPtr terminal = sis.finalSceneIndex;

    std::printf("=== usdImaging chain (terminal -> deepest) ===\n");
    std::vector<HdSceneIndexBaseRefPtr> chain;
    _CollectChain(terminal, &chain);
    for (size_t i = 0; i < chain.size(); ++i)
        std::printf("  %d. %s\n", i, _Label(chain[i]).c_str());
    std::printf("\n");

    // One recorder per chain index, so we can attribute notices.
    std::vector<Recorder> recorders(chain.size());
    std::vector<NoticeRecorder *> observers(chain.size());
    for (size_t i = 0; i < chain.size(); ++i) {
        recorders[i].label = _Label(chain[i]);
        observers[i] = new NoticeRecorder(&recorders[i]);
        chain[i]->AddObserver(HdSceneIndexObserverPtr(observers[i]));
    }

    std::printf("=== baseline ===\n");
    _PrintOrders("baseline", stage, terminal);
    std::printf("\n");

    // ---------------------------------------------------------------
    // Control: a plugin-field edit (usdGen:magnitude) must produce a
    // dirtied notice somewhere in the chain -> proves observer wiring.
    // ---------------------------------------------------------------
    std::printf("--- control: Set usdGen:magnitude=0.25 on %s ---\n",
                 kNoise.GetText());
    stage->GetPrimAtPath(kNoise)
        .GetAttribute(TfToken("usdGen:magnitude"))
        .Set(0.25f);
    if (sis.stageSceneIndex) sis.stageSceneIndex->ApplyPendingUpdates();
    for (auto &rec : recorders) rec.Dump("control/magnitude");
    std::printf("orders after control:\n");
    _PrintOrders("post-control", stage, terminal);
    std::printf("\n");
    for (auto &rec : recorders) rec.Reset();

    // ---------------------------------------------------------------
    // Variant A: Sdf-level `reorder nameChildren` statement.
    // SdfPrimSpecifier::SetNameChildrenOrder authors the
    // `reorder nameChildren` statement on /Groom/Description/Ops.
    // ---------------------------------------------------------------
    std::printf("--- variant A: Sdf reorder nameChildren "
                 "(Scatter, Length, Noise) ---\n");
    SdfLayerRefPtr root = stage->GetRootLayer();
    SdfPrimSpecHandle opsSpec = root->GetPrimAtPath(kOps);
    opsSpec->SetNameChildrenOrder(
        {TfToken("OpScatter"), TfToken("OpLength"), TfToken("OpNoise")});
    if (sis.stageSceneIndex) sis.stageSceneIndex->ApplyPendingUpdates();
    for (auto &rec : recorders) rec.Dump("variantA/sdf-reorder");
    std::printf("orders after variant A:\n");
    _PrintOrders("post-A", stage, terminal);
    std::printf("\n");
    for (auto &rec : recorders) rec.Reset();

    // ---------------------------------------------------------------
    // Variant B: Usd-level reorder opinion (primOrder metadata) via
    // UsdPrim::SetChildrenReorder.
    // ---------------------------------------------------------------
    std::printf("--- variant B: UsdPrim::SetChildrenReorder "
                 "(Noise, Scatter, Length) ---\n");
    std::vector<TfToken> reorderB;
    reorderB.push_back(TfToken("OpNoise"));
    reorderB.push_back(TfToken("OpScatter"));
    reorderB.push_back(TfToken("OpLength"));
    stage->GetPrimAtPath(kOps).SetChildrenReorder(reorderB);
    if (sis.stageSceneIndex) sis.stageSceneIndex->ApplyPendingUpdates();
    for (auto &rec : recorders) rec.Dump("variantB/primOrder");
    std::printf("orders after variant B:\n");
    _PrintOrders("post-B", stage, terminal);
    std::printf("\n");
    for (auto &rec : recorders) rec.Reset();

    // ---------------------------------------------------------------
    // Sanity: clear both opinions and confirm the order returns to the
    // authored nameChildren order.
    // ---------------------------------------------------------------
    std::printf("--- sanity: clear reorder opinions ---\n");
    stage->GetPrimAtPath(kOps).ClearChildrenReorder();
    opsSpec->RemoveFromNameChildrenOrder(TfToken("OpScatter"));
    opsSpec->RemoveFromNameChildrenOrder(TfToken("OpLength"));
    opsSpec->RemoveFromNameChildrenOrder(TfToken("OpNoise"));
    if (sis.stageSceneIndex) sis.stageSceneIndex->ApplyPendingUpdates();
    for (auto &rec : recorders) rec.Dump("sanity/clear");
    _PrintOrders("final", stage, terminal);

    for (size_t i = 0; i < chain.size(); ++i)
        chain[i]->RemoveObserver(HdSceneIndexObserverPtr(observers[i]));
    delete[] observers.data();

    std::printf("probeReorder: DONE\n");
    return 0;
}
