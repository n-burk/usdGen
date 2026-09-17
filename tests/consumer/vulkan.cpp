// Installed-header/link/shader discovery probe; no GPU is required.
#include "usdGen/sessionDeviceIntegration.h"
#include "usdGen/authoredNamedChannels.h"
#include "usdGen/op.h"
#include "usdGen/vulkan/sessionProvider.h"
#include "usdGen/vulkan/deviceFactory.h"
#include "usdGen/vulkan/executionPlan.h"
#include "usdGen/vulkan/widthPipeline.h"
#include "usdGen/vulkan/lengthScalePipeline.h"
#include "usdGen/vulkan/lengthCompactionPipeline.h"
#include "usdGen/vulkan/widthBlendPipeline.h"
#include "usdGen/vulkan/nonWidthComparePipeline.h"

#include <cstdint>
#include <algorithm>
#include <fstream>
#include <cstdio>

int main() {
    std::ifstream shader(USDGEN_VULKAN_SHADER, std::ios::binary);
    uint32_t magic = 0;
    shader.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!shader || magic != 0x07230203u) return 1;
    std::ifstream length(USDGEN_VULKAN_LENGTH_SHADER, std::ios::binary);
    magic = 0;
    length.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!length || magic != 0x07230203u) return 8;
    std::ifstream lengthSet(USDGEN_VULKAN_LENGTH_SET_SHADER, std::ios::binary);
    magic = 0;
    lengthSet.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!lengthSet || magic != 0x07230203u) return 34;
    std::ifstream lengthCutExtend(USDGEN_VULKAN_LENGTH_CUT_EXTEND_SHADER, std::ios::binary);
    magic = 0;
    lengthCutExtend.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!lengthCutExtend || magic != 0x07230203u) return 41;
    std::ifstream lengthReparam(USDGEN_VULKAN_LENGTH_REPARAM_SHADER, std::ios::binary);
    magic = 0;
    lengthReparam.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!lengthReparam || magic != 0x07230203u) return 47;
    std::ifstream lengthMinimum(USDGEN_VULKAN_LENGTH_MINIMUM_SHADER, std::ios::binary);
    magic = 0;
    lengthMinimum.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!lengthMinimum || magic != 0x07230203u) return 51;
    std::ifstream lengthLiteralV1(USDGEN_VULKAN_LENGTH_LITERAL_V1_SHADER, std::ios::binary);
    magic = 0;
    lengthLiteralV1.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!lengthLiteralV1 || magic != 0x07230203u) return 55;
    std::ifstream lengthEnvelopeV1(USDGEN_VULKAN_LENGTH_ENVELOPE_V1_SHADER, std::ios::binary);
    magic = 0;
    lengthEnvelopeV1.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!lengthEnvelopeV1 || magic != 0x07230203u) return 59;
    std::ifstream blend(USDGEN_VULKAN_WIDTH_BLEND_SHADER, std::ios::binary);
    magic = 0;
    blend.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!blend || magic != 0x07230203u) return 10;
    std::ifstream compare(USDGEN_VULKAN_NON_WIDTH_COMPARE_SHADER, std::ios::binary);
    magic = 0;
    compare.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!compare || magic != 0x07230203u) return 12;
    std::ifstream compaction(USDGEN_VULKAN_LENGTH_COMPACTION_SHADER, std::ios::binary);
    magic = 0;
    compaction.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!compaction || magic != 0x07230203u) return 30;
    if (usdGen::vulkan::LengthCompactionPipeline::Create({}, {})) return 31;
    if (usdGen::vulkan::VulkanSessionProvider::Create({})) return 2;
    if (usdGen::vulkan::DeviceFactory::CreateContext({}, {})) return 3;
    if (usdGen::CreateUsdGenDeviceSession(1, 8, {}, {})) return 4;
    if (usdGen::vulkan::LengthScalePipeline::Create({}, {})) return 9;
    if (usdGen::vulkan::LengthScalePipeline::CreateWithSet({}, {}, {})) return 35;
    if (usdGen::vulkan::LengthScalePipeline::CreateWithCutExtend({}, {}, {}, {})) return 42;
    if (usdGen::vulkan::LengthScalePipeline::CreateWithReparam({}, {}, {}, {}, {})) return 48;
    if (usdGen::vulkan::LengthScalePipeline::CreateWithMinimum({}, {}, {}, {}, {}, {})) return 52;
    if (usdGen::vulkan::LengthScalePipeline::CreateWithLiteralV1({}, {}, {}, {}, {}, {}, {})) return 56;
    if (usdGen::vulkan::LengthScalePipeline::CreateWithEnvelopeV1({}, {}, {}, {}, {}, {}, {}, {})) return 60;
    if (usdGen::vulkan::WidthBlendPipeline::Create({}, {})) return 11;
    if (usdGen::vulkan::NonWidthComparePipeline::Create({}, {})) return 13;
    if (!usdGen::ValidateAuthoredNamedChannels(usdGen::UsdGenCurveSetDesc{}, 0, 0)) return 14;

    // Exercise the installed descriptor compiler with a rooted C3 source;
    // this remains native-runtime-free and does not require a Vulkan device.
    usdGen::UsdGenGraphDesc rooted;
    rooted.description = SdfPath("/Groom");
    rooted.executionBackend = usdGen::UsdGenExecutionBackend::Vulkan;
    usdGen::UsdGenCurveSetDesc curves;
    curves.path = SdfPath("/Groom/C3");
    curves.role = usdGen::UsdGenRole::Curves;
    curves.curveRole = TfToken("hair");
    curves.curveVertexCounts = {2};
    curves.points = {{0, 0, 0}, {0, 1, 0}};
    curves.rest = curves.points;
    curves.widths = {.02f, .03f};
    curves.curveId = {41};
    curves.frozenEpoch = "epoch-19";
    curves.skinPrim = {0};
    curves.skinPrimUv = {{.25f, .25f}};
    curves.rootFrame = {GfMatrix4d(1.0)};
    rooted.curveSets.push_back(curves);
    usdGen::UsdGenSurfaceDesc surface;
    surface.path = SdfPath("/Groom/Scalp");
    surface.restPoints = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    surface.points = surface.restPoints;
    surface.faceVertexCounts = {3};
    surface.faceVertexIndices = {0, 1, 2};
    surface.uv = {{0, 0}, {1, 0}, {0, 1}};
    surface.worldMatrix = GfMatrix4d(1.0);
    rooted.surfaces.push_back(surface);
    usdGen::UsdGenNodeDesc source;
    source.path = SdfPath("/Groom/Ops/source");
    source.type = TfToken("UsdGenCurveSource");
    source.curves = {curves.path};
    source.surfaces = {surface.path};
    source.params = {{TfToken("useRest"), VtValue(true), false},
        {TfToken("resampleTo"), VtValue(0), false},
        {TfToken("rebind"), VtValue(TfToken("never")), false}};
    usdGen::UsdGenNodeDesc width;
    width.path = SdfPath("/Groom/Ops/width");
    width.type = TfToken("UsdGenWidth");
    width.inputs = {source.path};
    width.params = {{TfToken("width"), VtValue(.125f), false}};
    rooted.nodes = {source, width};
    rooted.terminal = width.path;
    auto culled = rooted;
    usdGen::UsdGenNodeDesc cull;
    cull.path = SdfPath("/Groom/Ops/cull");
    cull.type = TfToken("UsdGenLength"); cull.inputs = {source.path};
    cull.params = {{TfToken("length:mode"), VtValue(TfToken("cull")), false},
                   {TfToken("cullThreshold"), VtValue(.25f), false}};
    culled.nodes.back().inputs = {cull.path};
    culled.nodes.insert(culled.nodes.end() - 1, cull);
    auto cullHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(culled);
    if (!cullHandle) return 32;
    auto cullPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(cullHandle->Payload());
    if (!cullPlan || cullPlan->Steps().size() != 2 ||
        cullPlan->Steps()[0].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthCull ||
        cullPlan->Steps()[0].cullThreshold != .25f) return 33;
    usdGen::UsdGenDiagnostics rootedDiagnostics;
    auto rootedHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(rooted, &rootedDiagnostics);
    if (!rootedHandle || rootedDiagnostics.HasErrors()) return 15;
    auto rootedPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        rootedHandle->Payload());
    if (!rootedPlan || !rootedPlan->HasRootBindings()) return 16;
    if (rootedPlan->SourceControls().useRest != true ||
        rootedPlan->SourceControls().rebind != TfToken("never") ||
        rootedPlan->Source().skinPrim != VtIntArray({0}) ||
        rootedPlan->Source().skinPrimUv != VtVec2fArray({GfVec2f(.25f, .25f)}) ||
        rootedPlan->Source().rootFrame.size() != 1 ||
        rootedPlan->Source().rootFrame[0] != GfMatrix4d(1.0)) return 17;

    // A rooted Source -> Length Set -> Width graph keeps Source's topology
    // lineage: Set is fixed-topology geometry COW, not a topology barrier.
    auto rootedSet = rooted;
    usdGen::UsdGenNodeDesc set;
    set.path = SdfPath("/Groom/Ops/set");
    set.type = TfToken("UsdGenLength");
    set.inputs = {source.path};
    set.params = {{TfToken("length:mode"), VtValue(TfToken("set")), false},
                  {TfToken("length:value"), VtValue(1.25f), false}};
    rootedSet.nodes.back().inputs = {set.path};
    rootedSet.nodes.insert(rootedSet.nodes.end() - 1, set);
    rootedSet.terminal = width.path;
    usdGen::UsdGenDiagnostics setDiagnostics;
    auto rootedSetHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(rootedSet, &setDiagnostics);
    if (!rootedSetHandle || setDiagnostics.HasErrors()) return 36;
    auto rootedSetPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        rootedSetHandle->Payload());
    if (!rootedSetPlan || rootedSetPlan->Steps().size() != 2 ||
        rootedSetPlan->Steps()[0].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthScale ||
        rootedSetPlan->Steps()[0].lengthMode != usdGen::vulkan::VulkanSourceWidthStage::LengthMode::Set ||
        rootedSetPlan->LengthFactor() != 1.25f || rootedSetPlan->SourceNodePath() != source.path ||
        rootedSetPlan->LengthNodePath() != set.path) return 37;
    auto rootedSetMetadata = rootedSetHandle->Metadata();
    if (!rootedSetMetadata) return 38;
    auto findTask = [&](SdfPath const& path) {
        auto found = std::find_if(rootedSetMetadata->Tasks().begin(), rootedSetMetadata->Tasks().end(),
            [&](auto const& task) { return task.path == path; });
        return found == rootedSetMetadata->Tasks().end() ? nullptr : &*found;
    };
    auto findUse = [](usdGen::UsdGenExecutionTaskMetadata const& task,
                      usdGen::UsdGenExecutionDataKind kind,
                      usdGen::UsdGenExecutionResourceAccess access) {
        auto found = std::find_if(task.resources.begin(), task.resources.end(),
            [&](auto const& use) { return use.resource == kind && use.access == access; });
        return found == task.resources.end() ? nullptr : &*found;
    };
    auto setTask = findTask(set.path);
    auto setWidthTask = findTask(width.path);
    if (!setTask || !setWidthTask || setTask->topologyBarrier) return 39;
    auto setTopology = findUse(*setTask, usdGen::UsdGenExecutionDataKind::CurveTopology,
                               usdGen::UsdGenExecutionResourceAccess::Read);
    auto setGeometry = findUse(*setTask, usdGen::UsdGenExecutionDataKind::CurveGeometry,
                               usdGen::UsdGenExecutionResourceAccess::ReadWrite);
    auto setWidthTopology = findUse(*setWidthTask, usdGen::UsdGenExecutionDataKind::CurveTopology,
                                    usdGen::UsdGenExecutionResourceAccess::Read);
    auto setWidthGeometry = findUse(*setWidthTask, usdGen::UsdGenExecutionDataKind::CurveGeometry,
                                    usdGen::UsdGenExecutionResourceAccess::Read);
    if (!setTopology || setTopology->producerTask != 0 || setTopology->inputValue == UINT32_MAX ||
        !setGeometry || setGeometry->producerTask != 0 || setGeometry->outputValue == UINT32_MAX ||
        !setWidthTopology || setWidthTopology->producerTask != 0 ||
        setWidthTopology->inputValue != setTopology->inputValue ||
        !setWidthGeometry || setWidthGeometry->producerTask != setTask->id ||
        setWidthGeometry->inputValue != setGeometry->outputValue) return 40;

    auto rootedCut = rootedSet;
    rootedCut.nodes[1].params.push_back({TfToken("length:method"), VtValue(TfToken("cutExtend")), false});
    rootedCut.nodes[1].params.push_back({TfToken("rebuild"), VtValue(TfToken("keepParam")), false});
    rootedCut.nodes[1].params.push_back({TfToken("cullThreshold"), VtValue(.25f), false});
    usdGen::UsdGenDiagnostics cutDiagnostics;
    auto rootedCutHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(rootedCut, &cutDiagnostics);
    if (!rootedCutHandle || cutDiagnostics.HasErrors()) return 43;
    auto rootedCutPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        rootedCutHandle->Payload());
    if (!rootedCutPlan || rootedCutPlan->Steps().size() != 3 ||
        rootedCutPlan->Steps()[0].lengthMode != usdGen::vulkan::VulkanSourceWidthStage::LengthMode::Set ||
        rootedCutPlan->Steps()[0].lengthMethod != usdGen::vulkan::VulkanSourceWidthStage::LengthMethod::CutExtend ||
        rootedCutPlan->Steps()[1].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthCull ||
        rootedCutPlan->Steps()[1].lengthMethod != usdGen::vulkan::VulkanSourceWidthStage::LengthMethod::CutExtend)
        return 44;
    auto rootedCutMetadata = rootedCutHandle->Metadata();
    if (!rootedCutMetadata) return 45;
    auto const* rootedCutCull = rootedCutMetadata->FindSemanticTask(1);
    if (!rootedCutCull || rootedCutCull->path != set.path || rootedCutCull->semanticNode != 1 ||
        !rootedCutCull->topologyBarrier) return 46;
    auto rootedReparam = rootedCut;
    for (auto& parameter : rootedReparam.nodes[1].params)
        if (parameter.name == TfToken("rebuild")) parameter.value = VtValue(TfToken("reparam"));
    usdGen::UsdGenDiagnostics reparamDiagnostics;
    auto rootedReparamHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(
        rootedReparam, &reparamDiagnostics);
    if (!rootedReparamHandle || reparamDiagnostics.HasErrors()) return 49;
    auto rootedReparamPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        rootedReparamHandle->Payload());
    if (!rootedReparamPlan || rootedReparamPlan->Steps().size() != 3 ||
        rootedReparamPlan->Steps()[0].lengthRebuild != usdGen::vulkan::VulkanSourceWidthStage::LengthRebuild::Reparam ||
        rootedReparamPlan->Steps()[1].lengthRebuild != usdGen::vulkan::VulkanSourceWidthStage::LengthRebuild::Reparam ||
        rootedReparamPlan->Steps()[1].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthCull)
        return 50;

    // The new minimum shader is independently discoverable. Both literal
    // scalar spellings survive typed lowering into transform then Cull.
    for (auto const& minimum : {VtValue(2.5f), VtValue(2.5)}) {
        auto minimumDesc = rootedReparam;
        minimumDesc.nodes[1].params.push_back(
            {TfToken("minRemainingLength"), minimum, false});
        auto minimumHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(minimumDesc);
        if (!minimumHandle) return 53;
        auto minimumPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
            minimumHandle->Payload());
        if (!minimumPlan || minimumPlan->Steps().size() != 3 ||
            minimumPlan->Steps()[0].minRemainingLength != 2.5f ||
            minimumPlan->Steps()[0].lengthMode != usdGen::vulkan::VulkanSourceWidthStage::LengthMode::Set ||
            minimumPlan->Steps()[0].lengthMethod != usdGen::vulkan::VulkanSourceWidthStage::LengthMethod::CutExtend ||
            minimumPlan->Steps()[0].lengthRebuild != usdGen::vulkan::VulkanSourceWidthStage::LengthRebuild::Reparam ||
            minimumPlan->Steps()[1].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthCull)
            return 54;
    }

    // Installed compilation preserves nonneutral reversed random endpoints and
    // the authored signed seed, composed with minimum/rebuild/threshold.
    auto randomDesc = rootedReparam;
    randomDesc.nodes[1].seed = -12345;
    randomDesc.nodes[1].params.push_back(
        {TfToken("length:random"), VtValue(GfVec2f(2.0f, .25f)), false});
    randomDesc.nodes[1].params.push_back(
        {TfToken("minRemainingLength"), VtValue(2.5f), false});
    auto randomHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(randomDesc);
    if (!randomHandle) return 57;
    auto randomPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        randomHandle->Payload());
    if (!randomPlan || randomPlan->Steps().size() != 3 ||
        randomPlan->Steps()[0].randomLo != 2.0f ||
        randomPlan->Steps()[0].randomHi != .25f ||
        randomPlan->Steps()[0].randomSeed != -12345 ||
        randomPlan->Steps()[0].minRemainingLength != 2.5f ||
        randomPlan->Steps()[0].lengthRebuild != usdGen::vulkan::VulkanSourceWidthStage::LengthRebuild::Reparam ||
        randomPlan->Steps()[1].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthCull)
        return 58;

    auto envelopeDesc = randomDesc;
    envelopeDesc.nodes[1].params.push_back(
        {TfToken("mask"), VtValue(.5f), false});
    auto envelopeHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(envelopeDesc);
    if (!envelopeHandle) return 61;
    auto envelopePlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        envelopeHandle->Payload());
    if (!envelopePlan || envelopePlan->Steps().size() != 3 ||
        envelopePlan->Steps()[0].lengthBlend != 1.f ||
        envelopePlan->Steps()[0].lengthMaskAmount != .5f ||
        envelopePlan->Steps()[0].randomSeed != -12345 ||
        envelopePlan->Steps()[0].minRemainingLength != 2.5f)
        return 62;

    // WidthBlend must lower the authored literal weight into its stage.
    usdGen::UsdGenNodeDesc rightW = width; rightW.path = SdfPath("/Groom/Ops/widthR");
    rightW.params = {{TfToken("width"), VtValue(.5f), false}};
    usdGen::UsdGenNodeDesc join;
    join.path = SdfPath("/Groom/Ops/join"); join.type = TfToken("UsdGenWidthBlend");
    join.inputs = {width.path, rightW.path};
    join.params = {{TfToken("widthBlend:weight"), VtValue(.25f), false}};
    auto joinDesc = rooted;
    joinDesc.nodes = {source, rightW, width, join}; joinDesc.terminal = join.path;
    auto joinHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(joinDesc);
    if (!joinHandle) return 63;
    auto joinPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        joinHandle->Payload());
    if (!joinPlan || joinPlan->Steps().size() != 3 ||
        joinPlan->Steps()[2].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::WidthBlend ||
        joinPlan->Steps()[2].blend != .25f ||
        joinPlan->Steps()[2].rightInput == joinPlan->Steps()[2].input)
        return 64;

    // A positive threshold on a fixed-topology Scale/Set is lowered to an
    // internal transform followed by an authored Cull.  Exercise both
    // accepted literal scalar spellings and retain the authored semantic
    // ordinal/path on the lowered Cull.
    auto checkThresholdLowering = [&](VtValue threshold) {
        auto thresholdDesc = rootedSet;
        thresholdDesc.nodes[1].params.push_back(
            {TfToken("cullThreshold"), threshold, false});
        usdGen::UsdGenDiagnostics thresholdDiagnostics;
        auto thresholdHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(
            thresholdDesc, &thresholdDiagnostics);
        if (!thresholdHandle || thresholdDiagnostics.HasErrors()) return false;
        auto thresholdPlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
            thresholdHandle->Payload());
        if (!thresholdPlan || thresholdPlan->Steps().size() != 3 ||
            thresholdPlan->IntermediateCount() != 2 ||
            thresholdPlan->Steps()[0].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthScale ||
            thresholdPlan->Steps()[0].lengthMode != usdGen::vulkan::VulkanSourceWidthStage::LengthMode::Set ||
            thresholdPlan->Steps()[1].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::LengthCull ||
            thresholdPlan->Steps()[1].cullThreshold != .25f ||
            thresholdPlan->Steps()[2].kind != usdGen::vulkan::VulkanSourceWidthStage::Kind::Width ||
            thresholdPlan->LengthNodePath() != thresholdDesc.nodes[1].path)
            return false;
        auto thresholdMetadata = thresholdHandle->Metadata();
        if (!thresholdMetadata || thresholdMetadata->Tasks().size() != 5)
            return false;
        auto const* authoredCull = thresholdMetadata->FindSemanticTask(1);
        if (!authoredCull || authoredCull->path != thresholdDesc.nodes[1].path ||
            !authoredCull->topologyBarrier)
            return false;
        auto widthIt = std::find_if(thresholdMetadata->Tasks().begin(), thresholdMetadata->Tasks().end(),
            [&](auto const& task) { return task.path == width.path; });
        if (widthIt == thresholdMetadata->Tasks().end()) return false;
        auto const* terminalTopology = findUse(*widthIt,
            usdGen::UsdGenExecutionDataKind::CurveTopology,
            usdGen::UsdGenExecutionResourceAccess::Read);
        if (!terminalTopology || terminalTopology->inputValue == UINT32_MAX ||
            terminalTopology->inputValue >= thresholdMetadata->Values().size() ||
            terminalTopology->producerTask != authoredCull->id ||
            thresholdMetadata->Values()[terminalTopology->inputValue].producerTask != authoredCull->id)
            return false;
        return true;
    };
    if (!checkThresholdLowering(VtValue(.25f)) ||
        !checkThresholdLowering(VtValue(.25))) return 41;
    auto captured = rootedPlan->Descriptor();
    if (!captured || captured->surfaces.size() != 1 ||
        captured->surfaces[0].worldMatrix != GfMatrix4d(1.0) ||
        captured->surfaces[0].faceVertexIndices != VtIntArray({0, 1, 2}) ||
        captured->surfaces[0].uv != VtVec2fArray({{0, 0}, {1, 0}, {0, 1}}) ||
        captured->nodes[0].params[1].value != VtValue(0)) return 18;
    rooted.curveSets[0].skinPrim[0] = 99;
    rooted.curveSets[0].rootFrame[0][0][0] = 2.0;
    rooted.surfaces[0].points[0] = GfVec3f(9, 9, 9);
    rooted.nodes[0].params[0].value = VtValue(false);
    if (rootedPlan->Source().skinPrim != VtIntArray({0}) ||
        rootedPlan->Source().rootFrame[0] != GfMatrix4d(1.0) ||
        rootedPlan->SourceControls().useRest != true ||
        rootedPlan->Descriptor()->surfaces[0].points[0] != GfVec3f(0, 0, 0)) return 19;
    // Affine admission is available through the installed API as well. Keep
    // authored geometry source-local and retain both immutable transforms.
    auto affine = *captured;
    GfMatrix4d sourceMatrix(1.0), surfaceMatrix(1.0);
    sourceMatrix[0][0] = 2.0; sourceMatrix[1][1] = 3.0; sourceMatrix[3][0] = 4.0;
    surfaceMatrix[0][0] = 0.0; surfaceMatrix[0][1] = 1.0;
    surfaceMatrix[1][0] = -1.0; surfaceMatrix[1][1] = 0.0; surfaceMatrix[3][2] = 2.0;
    affine.curveSets[0].worldMatrix = sourceMatrix;
    affine.surfaces[0].worldMatrix = surfaceMatrix;
    usdGen::UsdGenDiagnostics affineDiagnostics;
    auto affineHandle = usdGen::vulkan::CompileVulkanSourceWidthPlan(affine, &affineDiagnostics);
    if (!affineHandle || affineDiagnostics.HasErrors()) return 20;
    auto affinePlan = std::static_pointer_cast<const usdGen::vulkan::VulkanSourceWidthPlan>(
        affineHandle->Payload());
    if (!affinePlan || !affinePlan->HasRootBindings() ||
        affinePlan->Source().worldMatrix != sourceMatrix ||
        affinePlan->Descriptor()->surfaces[0].worldMatrix != surfaceMatrix ||
        affinePlan->Source().points != captured->curveSets[0].points ||
        affinePlan->Source().rest != captured->curveSets[0].rest) return 21;
    affine.curveSets[0].worldMatrix = GfMatrix4d(1.0);
    affine.surfaces[0].worldMatrix = GfMatrix4d(1.0);
    if (affinePlan->Source().worldMatrix != sourceMatrix ||
        affinePlan->Descriptor()->surfaces[0].worldMatrix != surfaceMatrix) return 22;
    affine.curveSets[0].worldMatrix[0][3] = .25;
    usdGen::UsdGenDiagnostics invalidAffine;
    if (usdGen::vulkan::CompileVulkanSourceWidthPlan(affine, &invalidAffine) ||
        !invalidAffine.HasErrors()) return 23;
    std::puts("Installed Vulkan runtime headers, libraries and shader: PASS");
    return 0;
}
