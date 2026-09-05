// Minimal repro of the OpenUSD 26.08 OpenExec resync bug that makes
// UsdStage::RemovePrim post a coding error whenever an ExecUsdSystem is
// attached to the stage.  No RigExec code is involved.
//
//   pxr/exec/esfUsd/stageData.cpp:360   UsdPrimDefaultPredicate(resyncedPrim)
//   pxr/usd/usd/primFlags.cpp:21-25     TF_CODING_ERROR on an invalid prim
//
// Build:
//   g++ -std=c++17 probeExecResyncRemovePrim.cpp -o probeExecResyncRemovePrim \
//     -I$USD/include -L$USD/lib -Wl,-rpath,$USD/lib \
//     -lusd_tf -lusd_sdf -lusd_usd -lusd_execUsd
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/exec/execUsd/system.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/base/tf/diagnostic.h"
#include <cstdio>
#include <memory>

PXR_NAMESPACE_USING_DIRECTIVE

static int Run(bool withExecSystem)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));

    std::unique_ptr<ExecUsdSystem> system;
    if (withExecSystem) {
        system = std::make_unique<ExecUsdSystem>(UsdStageConstRefPtr(stage));
    }

    stage->DefinePrim(SdfPath("/World/Child"), TfToken("Xform"));

    TfErrorMark mark;
    stage->RemovePrim(SdfPath("/World/Child"));
    int errorCount = 0;
    std::string first;
    for (auto it = mark.GetBegin(); it != mark.GetEnd(); ++it) {
        if (errorCount == 0) first = it->GetCommentary();
        ++errorCount;
    }
    mark.Clear();
    std::printf("%-24s RemovePrim posted %d error(s)%s%s\n",
                withExecSystem ? "with ExecUsdSystem:" : "no ExecUsdSystem:",
                errorCount, errorCount ? "  first: " : "", first.c_str());
    return errorCount;
}

int main()
{
    const int without = Run(false);
    const int with = Run(true);
    std::printf("RESULT: baseline=%d withExec=%d -> %s\n", without, with,
                (without == 0 && with > 0) ? "REPRODUCED" : "not reproduced");
    return (without == 0 && with > 0) ? 0 : 1;
}
