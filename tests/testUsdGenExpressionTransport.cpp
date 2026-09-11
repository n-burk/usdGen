#include "usdGenImaging/usdGenGraphDescBuilder.h"
#include "usdGenImaging/usdGenGraphDescBuilderStage.h"

#include "pxr/pxr.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usdImaging/usdImaging/sceneIndices.h"
#include "pxr/usdImaging/usdImaging/stageSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"

#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int Fail(char const *s) { std::fprintf(stderr, "FAIL: %s\n", s); return 1; }
class Notice final : public HdSceneIndexObserver {
public:
 explicit Notice(SdfPath path = SdfPath("/Character/Groom/hair")) : prefix(std::move(path)) {}
 SdfPath prefix;
 bool seen=false;
 void Reset() { seen=false; }
 void PrimsAdded(HdSceneIndexBase const&, AddedPrimEntries const&e) override { Hit(e); }
 void PrimsRemoved(HdSceneIndexBase const&, RemovedPrimEntries const&e) override { Hit(e); }
 void PrimsDirtied(HdSceneIndexBase const&, DirtiedPrimEntries const&e) override { Hit(e); }
 void PrimsRenamed(HdSceneIndexBase const&, RenamedPrimEntries const&) override {}
 template<class E> void Hit(E const&e) { for(auto const&x:e) seen|=x.primPath.HasPrefix(prefix); }
};

bool Same(usdGen::UsdGenGraphDesc const &a, usdGen::UsdGenGraphDesc const &b)
{
    if (a.executionBackend != b.executionBackend ||
        a.expressions.size() != b.expressions.size() || a.nodes.size() != b.nodes.size()) return false;
    for (size_t i=0; i<a.expressions.size(); ++i) {
        auto const &x=a.expressions[i], &y=b.expressions[i];
        if (x.path!=y.path || x.source!=y.source || x.outputs.size()!=y.outputs.size()) return false;
        for (size_t j=0;j<x.outputs.size();++j)
            if (x.outputs[j].name!=y.outputs[j].name || x.outputs[j].nativeType!=y.outputs[j].nativeType ||
                x.outputs[j].shape.scalar!=y.outputs[j].shape.scalar || x.outputs[j].shape.components!=y.outputs[j].shape.components ||
                x.outputs[j].shape.isArray!=y.outputs[j].shape.isArray) return false;
    }
    for (size_t i=0;i<a.nodes.size();++i) {
        auto const &x=a.nodes[i].expressionBindings, &y=b.nodes[i].expressionBindings;
        if (x.size()!=y.size()) return false;
        for (size_t j=0;j<x.size();++j)
            if (x[j].expression!=y[j].expression || x[j].output!=y[j].output ||
                x[j].destination!=y[j].destination || x[j].nativeType!=y[j].nativeType ||
                x[j].domain!=y[j].domain || x[j].literal!=y[j].literal) return false;
    }
    return true;
}
}

int main()
{
    std::string const file = std::string(USDGEN_TEST_SOURCE_DIR) + "/plan/examples/operator-network.usda";
    UsdStageRefPtr const stage = UsdStage::Open(file);
    if (!stage) return Fail("open operator-network mock");
    SdfPath const descPath("/Character/Groom/hair");
    auto stageDesc = usdGenImaging::BuildGraphDescFromStage(stage, descPath);
    UsdImagingCreateSceneIndicesInfo info; info.stage = stage;
    UsdImagingSceneIndices const indices = UsdImagingCreateSceneIndices(info);
    Notice notice; HdSceneIndexObserverPtr observer = TfCreateWeakPtr(&notice);
    indices.finalSceneIndex->AddObserver(observer);
    auto hydraDesc = usdGenImaging::BuildGraphDescFromHydra(*indices.finalSceneIndex, descPath);
    if (!Same(stageDesc, hydraDesc)) return Fail("Stage/Hydra expression descriptor parity");
    if (stageDesc.executionBackend != usdGen::UsdGenExecutionBackend::Cuda) return Fail("cuda backend transport");

    // An authored but unknown backend is not a request for the legacy CPU
    // path.  Check both transports while they share this live scene index.
    UsdAttribute backend = stage->GetAttributeAtPath(
        SdfPath("/Character/Groom.usdGen:execution:backend"));
    if (!backend) return Fail("backend attribute lookup");
    backend.Set(TfToken("not-a-backend"));
    indices.stageSceneIndex->ApplyPendingUpdates();
    auto invalidStage = usdGenImaging::BuildGraphDescFromStage(stage, descPath);
    auto invalidHydra = usdGenImaging::BuildGraphDescFromHydra(
        *indices.finalSceneIndex, descPath);
    if (invalidStage.executionBackend != usdGen::UsdGenExecutionBackend::Invalid ||
        invalidHydra.executionBackend != usdGen::UsdGenExecutionBackend::Invalid)
        return Fail("unknown backend did not fail closed");

    // Clearing the authored opinion exposes the schema's resolved cuda
    // fallback, which is still a valid backend request.
    backend.Clear();
    indices.stageSceneIndex->ApplyPendingUpdates();
    auto absentStage = usdGenImaging::BuildGraphDescFromStage(stage, descPath);
    auto absentHydra = usdGenImaging::BuildGraphDescFromHydra(
        *indices.finalSceneIndex, descPath);
    if (absentStage.executionBackend != usdGen::UsdGenExecutionBackend::Cuda ||
        absentHydra.executionBackend != usdGen::UsdGenExecutionBackend::Cuda)
        return Fail("schema backend fallback was not transported");

    // A genuinely undeclared property remains the legacy CPU baseline.  Use
    // an untyped parent Scope so no UsdGenGroom schema fallback can supply a
    // backend value.
    SdfLayerRefPtr const noBackendLayer =
        SdfLayer::CreateAnonymous("expression-transport-no-backend.usda");
    if (!noBackendLayer->ImportFromString(R"usda(
#usda 1.0
def Scope "Groom"
{
    def UsdGenDescription "hair"
    {
        string usdGen:width:default = "bad"
        string usdGen:tileTarget = "bad"
        def Scope "Ops"
        {
            def UsdGenWidth "width"
            {
                # Deliberately malformed dedicated scalars: both builders
                # must preserve each type error instead of using defaults.
                string usdGen:enabled = "bad"
                string usdGen:seed = "bad"
                string usdGen:blend = "bad"
                string usdGen:algorithmVersion = "bad"
            }
        }
    }
}
)usda")) return Fail("open no-backend fixture");
    UsdStageRefPtr const noBackendStage = UsdStage::Open(noBackendLayer);
    if (!noBackendStage) return Fail("open no-backend stage");
    UsdImagingCreateSceneIndicesInfo noBackendInfo;
    noBackendInfo.stage = noBackendStage;
    UsdImagingSceneIndices const noBackendIndices =
        UsdImagingCreateSceneIndices(noBackendInfo);
    auto noBackendStageDesc = usdGenImaging::BuildGraphDescFromStage(
        noBackendStage, SdfPath("/Groom/hair"));
    auto noBackendHydraDesc = usdGenImaging::BuildGraphDescFromHydra(
        *noBackendIndices.finalSceneIndex, SdfPath("/Groom/hair"));
    if (noBackendStageDesc.executionBackend !=
            usdGen::UsdGenExecutionBackend::CpuReference ||
        noBackendHydraDesc.executionBackend !=
            usdGen::UsdGenExecutionBackend::CpuReference)
        return Fail("undeclared backend did not retain CPU baseline");
    if (noBackendStageDesc.validationErrors.size() < 6 ||
        noBackendHydraDesc.validationErrors.size() < 6) {
        for (auto const& e : noBackendStageDesc.validationErrors) std::fprintf(stderr, "Stage: %s\n", e.c_str());
        for (auto const& e : noBackendHydraDesc.validationErrors) std::fprintf(stderr, "Hydra: %s\n", e.c_str());
        std::fprintf(stderr, "Hydra nodes: %zu\n", noBackendHydraDesc.nodes.size());
        return Fail("malformed dedicated scalar was silently defaulted");
    }
    Notice recoveryNotice(SdfPath("/Groom/hair"));
    HdSceneIndexObserverPtr recoveryObserver = TfCreateWeakPtr(&recoveryNotice);
    noBackendIndices.finalSceneIndex->AddObserver(recoveryObserver);
    auto widthPrim = noBackendStage->GetPrimAtPath(SdfPath("/Groom/hair/Ops/width"));
    auto descriptionPrim = noBackendStage->GetPrimAtPath(SdfPath("/Groom/hair"));
    if (!widthPrim.GetAttribute(TfToken("usdGen:enabled")).Set(true) ||
        !widthPrim.GetAttribute(TfToken("usdGen:blend")).Set(1.0f) ||
        !widthPrim.GetAttribute(TfToken("usdGen:seed")).Set(0) ||
        !widthPrim.GetAttribute(TfToken("usdGen:algorithmVersion")).Set(0) ||
        !descriptionPrim.GetAttribute(TfToken("usdGen:width:default")).Set(.01f) ||
        !descriptionPrim.GetAttribute(TfToken("usdGen:tileTarget")).Set(64))
        return Fail("repair malformed dedicated values");
    noBackendIndices.stageSceneIndex->ApplyPendingUpdates();
    if (!recoveryNotice.seen) return Fail("dedicated value repair emitted no notice");
    auto repaired = usdGenImaging::BuildGraphDescFromHydra(
        *noBackendIndices.finalSceneIndex, SdfPath("/Groom/hair"));
    if (!repaired.validationErrors.empty()) return Fail("stale dedicated type diagnostics survived repair");
    noBackendIndices.finalSceneIndex->RemoveObserver(recoveryObserver);

    // Restore the fixture's authored request for the remaining transport
    // assertions below.
    backend.Set(TfToken("cuda"));
    indices.stageSceneIndex->ApplyPendingUpdates();
    if (stageDesc.expressions.size()!=6) return Fail("six expression programs");
    size_t bindings=0; for (auto const &n:stageDesc.nodes) bindings += n.expressionBindings.size();
    if (bindings!=7) return Fail("seven expression connections");

    // Each edit uses the same live scene index and its own observer window:
    // a notice from a different edit must not mask a missing invalidation.
    UsdAttribute source = stage->GetAttributeAtPath(SdfPath("/Character/Groom/hair/Expressions/strandWidth.usdGen:expr:source"));
    source.Set(std::string("$value * 2"));
    indices.stageSceneIndex->ApplyPendingUpdates();
    if (!notice.seen) return Fail("source edit emitted no scene-index notice");
    auto sourceLive = usdGenImaging::BuildGraphDescFromHydra(*indices.finalSceneIndex, descPath);
    bool changed=false; for (auto const &e:sourceLive.expressions) if (e.path.GetName()==TfToken("strandWidth")) changed = e.source == "$value * 2";
    if (!changed) return Fail("live source edit was not transported");

    notice.Reset();
    UsdAttribute width = stage->GetAttributeAtPath(SdfPath("/Character/Groom/hair/Ops/width.usdGen:width"));
    width.ClearConnections();
    indices.stageSceneIndex->ApplyPendingUpdates();
    if (!notice.seen) return Fail("disconnect emitted no scene-index notice");
    auto disconnectLive = usdGenImaging::BuildGraphDescFromHydra(*indices.finalSceneIndex, descPath);
    size_t disconnectBindings=0; for (auto const &n:disconnectLive.nodes) disconnectBindings += n.expressionBindings.size();
    if (disconnectBindings != 6) return Fail("live disconnect was not transported");

    notice.Reset();
    UsdAttribute magnitude = stage->GetAttributeAtPath(
        SdfPath("/Character/Groom/hair/Ops/style/frizz.usdGen:noise:magnitude"));
    magnitude.SetCustomDataByKey(TfToken("usdGen:evaluation"), VtValue("groom"));
    indices.stageSceneIndex->ApplyPendingUpdates();
    if (!notice.seen) return Fail("metadata edit emitted no scene-index notice");
    auto live = usdGenImaging::BuildGraphDescFromHydra(*indices.finalSceneIndex, descPath);
    bool point=false; for (auto const &n:live.nodes) for (auto const &b:n.expressionBindings)
        if (b.destination == TfToken("usdGen:noise:magnitude")) point |= b.domain == usdGen::expr::Domain::Groom;
    if (!point) return Fail("live evaluation metadata edit was not transported");
    indices.finalSceneIndex->RemoveObserver(observer);
    return 0;
}
