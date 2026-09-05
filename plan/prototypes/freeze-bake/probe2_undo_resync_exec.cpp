// Gap G, part (a): which *undo* operations trip the OpenUSD 26.08 OpenExec
// resync coding error, and which are safe.  Extends
// probes/B-build/probeExecResyncRemovePrim.cpp with the exact removal shapes a
// freeze/undo would use.
//
//   pxr/exec/esfUsd/stageData.cpp:360   UsdPrimDefaultPredicate(resyncedPrim)
//   pxr/usd/usd/primFlags.cpp:21-25     TF_CODING_ERROR "Applying predicate to
//                                       invalid prim."
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/changeBlock.h"
#include "pxr/usd/sdf/copyUtils.h"
#include "pxr/exec/execUsd/system.h"
#include "pxr/base/tf/errorMark.h"
#include <cstdio>
#include <functional>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

struct Case { const char *name; std::function<void(UsdStageRefPtr&)> setup;
              std::function<void(UsdStageRefPtr&)> act; };

static int RunCase(const Case &c, bool withExec)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    c.setup(stage);
    std::unique_ptr<ExecUsdSystem> system;
    if (withExec)
        system = std::make_unique<ExecUsdSystem>(UsdStageConstRefPtr(stage));

    TfErrorMark mark;
    c.act(stage);
    int n = 0; std::string first;
    for (auto it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
        if (!n) first = it->GetCommentary();
        ++n;
    }
    mark.Clear();
    if (withExec)
        std::printf("  withExec: %d error(s)%s%s\n", n,
                    n ? "  first: " : "", first.c_str());
    else
        std::printf("%-42s baseline: %d error(s)\n", c.name, n);
    return n;
}

static void MakeSubtree(UsdStageRefPtr &s)
{
    s->DefinePrim(SdfPath("/World/Frozen"), TfToken("Xform"));
    s->DefinePrim(SdfPath("/World/Frozen/A"), TfToken("BasisCurves"));
    s->DefinePrim(SdfPath("/World/Frozen/B"), TfToken("BasisCurves"));
    s->DefinePrim(SdfPath("/World/Frozen/B/C"), TfToken("BasisCurves"));
}

static void SdfRemove(const SdfLayerHandle &layer, const SdfPath &p)
{
    SdfPrimSpecHandle parent = layer->GetPrimAtPath(p.GetParentPath());
    if (parent) parent->RemoveNameChild(layer->GetPrimAtPath(p));
    else layer->RemoveRootPrim(layer->GetPrimAtPath(p));
}

int main()
{
    std::vector<Case> cases = {
      {"A stage.RemovePrim(leaf)",
       [](UsdStageRefPtr &s){ s->DefinePrim(SdfPath("/World/Child"), TfToken("Xform")); },
       [](UsdStageRefPtr &s){ s->RemovePrim(SdfPath("/World/Child")); }},

      {"B Sdf RemoveNameChild(subtree root)",
       MakeSubtree,
       [](UsdStageRefPtr &s){ SdfRemove(s->GetRootLayer(), SdfPath("/World/Frozen")); }},

      {"C Sdf RemoveNameChild in ChangeBlock",
       MakeSubtree,
       [](UsdStageRefPtr &s){ SdfChangeBlock b;
                              SdfRemove(s->GetRootLayer(), SdfPath("/World/Frozen")); }},

      {"D SetActive(false) on subtree root",
       MakeSubtree,
       [](UsdStageRefPtr &s){ s->GetPrimAtPath(SdfPath("/World/Frozen")).SetActive(false); }},

      {"E DefinePrim (add subtree, no removal)",
       [](UsdStageRefPtr &){},
       [](UsdStageRefPtr &s){ MakeSubtree(s); }},

      {"F SdfCopySpec over existing subtree",
       MakeSubtree,
       [](UsdStageRefPtr &s){
           SdfLayerRefPtr stash = SdfLayer::CreateAnonymous("stash");
           SdfCreatePrimInLayer(stash, SdfPath("/World"));
           SdfCopySpec(s->GetRootLayer(), SdfPath("/World/Frozen"),
                       stash, SdfPath("/World/Frozen"));
           SdfCopySpec(stash, SdfPath("/World/Frozen"),
                       s->GetRootLayer(), SdfPath("/World/Frozen")); }},

      {"G remove then re-add in one ChangeBlock",
       MakeSubtree,
       [](UsdStageRefPtr &s){
           SdfLayerRefPtr stash = SdfLayer::CreateAnonymous("stash");
           SdfCreatePrimInLayer(stash, SdfPath("/World"));
           SdfCopySpec(s->GetRootLayer(), SdfPath("/World/Frozen"),
                       stash, SdfPath("/World/Frozen"));
           { SdfChangeBlock b;
             SdfRemove(s->GetRootLayer(), SdfPath("/World/Frozen"));
             SdfCopySpec(stash, SdfPath("/World/Frozen"),
                         s->GetRootLayer(), SdfPath("/World/Frozen")); } }},

      {"H mute a sublayer holding the freeze",
       [](UsdStageRefPtr &s){
           SdfLayerRefPtr bake = SdfLayer::CreateAnonymous("bake");
           SdfCreatePrimInLayer(bake, SdfPath("/World/Frozen"));
           bake->GetPrimAtPath(SdfPath("/World/Frozen"))->SetTypeName("BasisCurves");
           bake->GetPrimAtPath(SdfPath("/World/Frozen"))
               ->SetSpecifier(SdfSpecifierDef);
           s->GetSessionLayer()->InsertSubLayerPath(bake->GetIdentifier());
           s->SetMetadata(TfToken("comment"), bake->GetIdentifier()); },
       [](UsdStageRefPtr &s){
           std::string id; s->GetMetadata(TfToken("comment"), &id);
           s->MuteLayer(id); }},

      {"I remove sublayer path holding the freeze",
       [](UsdStageRefPtr &s){
           SdfLayerRefPtr bake = SdfLayer::CreateAnonymous("bake2");
           SdfCreatePrimInLayer(bake, SdfPath("/World/Frozen"));
           bake->GetPrimAtPath(SdfPath("/World/Frozen"))
               ->SetSpecifier(SdfSpecifierDef);
           s->GetSessionLayer()->InsertSubLayerPath(bake->GetIdentifier());
           s->SetMetadata(TfToken("comment"), bake->GetIdentifier()); },
       [](UsdStageRefPtr &s){
           std::string id; s->GetMetadata(TfToken("comment"), &id);
           s->GetSessionLayer()->RemoveSubLayerPath(0); }},
    };

    int failures = 0;
    for (const Case &c : cases) {
        const int base = RunCase(c, false);
        const int with = RunCase(c, true);
        if (base == 0 && with > 0) ++failures;
    }
    std::printf("\n%d of %zu cases trip the exec resync error\n",
                failures, cases.size());
    return 0;
}
