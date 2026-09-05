// Probe 4: is the OpenExec control plane (UsdExecImagingStageSceneIndex)
// actually constructible in this 26.08 install, and what does it expose?
#include "pxr/pxr.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/mergingSceneIndex.h"
#include "pxr/usdImaging/usdExecImaging/stageSceneIndexFactory.h"
#include "pxr/usdImaging/usdExecImaging/stageSceneIndexInterface.h"
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

static void DumpPrim(const HdSceneIndexBaseRefPtr &si, const SdfPath &p) {
    HdSceneIndexPrim prim = si->GetPrim(p);
    std::cout << "  " << p << " type='" << prim.primType << "' names:";
    if (prim.dataSource) {
        for (const TfToken &n : prim.dataSource->GetNames())
            std::cout << " " << n;
    } else std::cout << " <null ds>";
    std::cout << "\n";
    for (const SdfPath &c : si->GetChildPrimPaths(p)) DumpPrim(si, c);
}

int main(int argc, char **argv)
{
    UsdExecImagingStageSceneIndexInterfaceRefPtr si =
        UsdExecImagingCreateStageSceneIndex();
    std::cout << "UsdExecImagingCreateStageSceneIndex() -> "
              << (si ? "NON-NULL (PXR_BUILD_EXEC=ON)" : "null (exec OFF)")
              << "\n";
    if (!si) return 0;
    UsdStageRefPtr stage = UsdStage::Open(argc > 1 ? argv[1] : "scene.usda");
    si->SetStage(stage);
    si->SetTime(UsdTimeCode(1.0));
    si->ApplyPendingUpdates();
    std::cout << "exec scene index contents:\n";
    DumpPrim(si, SdfPath::AbsoluteRootPath());
    for (const char *p : {"/World", "/World/Scalp", "/World/Clump", "/World/Hair"}) {
        HdSceneIndexPrim pr = si->GetPrim(SdfPath(p));
        std::cout << "  direct GetPrim(" << p << ") type='" << pr.primType << "' ds=";
        if (pr.dataSource) { for (const TfToken &n : pr.dataSource->GetNames()) std::cout << " " << n; } else std::cout << "null";
        std::cout << "\n";
    }
    std::cout << "displayName = " << si->GetDisplayName() << "\n";
    return 0;
}
