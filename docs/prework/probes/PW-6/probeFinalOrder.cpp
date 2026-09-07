// Minimal companion probe for PW-6: composed child-order checks after each
// reorder variant, without the usdImaging chain (avoids the exit-time
// double-free seen in probeReorder). Only Sdf/Usd state is touched.
#include "pxr/pxr.h"

#include "pxr/base/plug/registry.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"

#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void RegisterUsdGenPlugins()
{
    static const std::vector<std::string> kPaths = {
        "/home/burkard/work/usdGen/build/usd/usdGenSchema/resources",
        "/home/burkard/work/usdGen/build/usd/usdGenImaging/resources"};
    PlugRegistry::GetInstance().RegisterPlugins(kPaths);
}

const SdfPath kOps = SdfPath("/Groom/Description/Ops");

void
Print(const char *tag, const UsdStageRefPtr &stage)
{
    std::string joined;
    for (const TfToken &t :
         stage->GetPrimAtPath(kOps).GetAllChildrenNames()) {
        if (!joined.empty()) joined += " ";
        joined += t.GetString();
    }
    std::printf("%-24s %s\n", tag, joined.c_str());
}
} // namespace

int
main(int argc, char **argv)
{
    if (argc < 2) {
        std::printf("usage: probeFinalOrder <stage.usda>\n");
        return 2;
    }
    RegisterUsdGenPlugins();
    UsdStageRefPtr stage = UsdStage::Open(std::string(argv[1]));
    if (!stage) {
        std::printf("FAILED to open %s\n", argv[1]);
        return 1;
    }

    Print("authored             ", stage);

    SdfLayerRefPtr root = stage->GetRootLayer();
    SdfPrimSpecHandle spec = root->GetPrimAtPath(kOps);

    spec->SetNameChildrenOrder(
        {TfToken("OpScatter"), TfToken("OpLength"), TfToken("OpNoise")});
    Print("after Sdf reorder(A) ", stage);

    std::vector<TfToken> b;
    b.push_back(TfToken("OpNoise"));
    b.push_back(TfToken("OpScatter"));
    b.push_back(TfToken("OpLength"));
    stage->GetPrimAtPath(kOps).SetChildrenReorder(b);
    Print("after primOrder(B)   ", stage);

    stage->GetPrimAtPath(kOps).ClearChildrenReorder();
    spec->RemoveFromNameChildrenOrder(TfToken("OpScatter"));
    spec->RemoveFromNameChildrenOrder(TfToken("OpLength"));
    spec->RemoveFromNameChildrenOrder(TfToken("OpNoise"));
    Print("after clear          ", stage);

    std::printf("probeFinalOrder: DONE\n");
    return 0;
}
