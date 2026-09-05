// Downstream link probe: proves a sibling project (usdGen) can consume the
// installed rigExec CMake package and call into both rigExec and
// rigExecImaging without any usdRig source in its build.
#include "rigExec/rigEvaluator.h"
#include "rigExecImaging/registry.h"
#include "pxr/base/plug/registry.h"
#include "pxr/usd/usd/stage.h"
#include <cstdio>
PXR_NAMESPACE_USING_DIRECTIVE
int main(int argc, char **argv)
{
    if (argc > 1) PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
    UsdStageRefPtr stage = UsdStage::Open(argv[2]);
    if (!stage) { std::printf("FAIL: stage\n"); return 1; }
    rigExec::RigExecRigEvaluator evaluator(stage, SdfPath(argv[3]));
    std::vector<std::string> errors;
    if (!evaluator.Compile(&errors)) {
        for (const std::string &e : errors) std::printf("FAIL compile: %s\n", e.c_str());
        return 1;
    }
    const rigExec::RigExecRigPose pose = evaluator.Evaluate(UsdTimeCode(1024.0));
    if (!pose.valid) { std::printf("FAIL evaluate\n"); return 1; }
    std::printf("  pose: %zu moved properties\n", pose.movedProperties.size());
    rigExec::RigExecImagingRegistry &reg =
        rigExec::RigExecImagingRegistry::GetInstance();
    (void)reg;
    std::printf("consumerProbe: PASS (linked rigExec + rigExecImaging out of tree)\n");
    return 0;
}
