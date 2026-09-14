#include "gpu/curveGrow.h"
#include "usdGen/executionResources.h"
#include "usdGenMath/hash.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace usdGen;
using namespace usdGen::gpu;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK failed: %s (%d)\n", #x, __LINE__); return 1; } } while (false)

static float3 V(float x, float y, float z) { return make_float3(x, y, z); }
static bool Near(float a, float b) { return std::fabs(a-b) < 3.e-5f; }
static bool Near(float3 a, float3 b) { return Near(a.x,b.x) && Near(a.y,b.y) && Near(a.z,b.z); }
struct Relay { std::atomic<int> status{-1}; };
static void Done(cudaStream_t, cudaError_t status, void* data) noexcept { static_cast<Relay*>(data)->status.store(int(status)); }
template <class T> static bool Upload(DeviceBuffer<T>& dst, std::vector<T> const& values, cudaStream_t stream) {
    return dst.reset(values.size()) == cudaSuccess &&
        (values.empty() || (cudaMemcpyAsync(dst.data(), values.data(), values.size()*sizeof(T), cudaMemcpyHostToDevice, stream) == cudaSuccess &&
                            cudaStreamSynchronize(stream) == cudaSuccess));
}
template <class T> static bool Download(DeviceView<const T> input, std::vector<T>* output, cudaStream_t stream) {
    return output->size() == input.size && (!input.size ||
        cudaMemcpyAsync(output->data(), input.data, input.size*sizeof(T), cudaMemcpyDeviceToHost, stream) == cudaSuccess) &&
        cudaStreamSynchronize(stream) == cudaSuccess;
}

struct InputOwner {
    DeviceBuffer<float3> points, rest, rootT, rootB, rootN;
    DeviceBuffer<float> widths;
    DeviceBuffer<uint32_t> offsets;
    DeviceBuffer<uint64_t> ids;
    DeviceBuffer<uint64_t> frameStableIds;
    DeviceBuffer<int32_t> rootPrim;
    DeviceBuffer<float2> rootUV;
    CurveGrowInput Input(size_t curves, size_t pointsCount) const {
        return {{points.view(), rest.view(), widths.view(), offsets.view(), ids.view(), curves, pointsCount},
                rootPrim.view(), rootUV.view(), rootT.view(), rootB.view(), rootN.view(), frameStableIds.view()};
    }
};

int main(int argc, char** argv) {
    for (uint32_t span : {2u, 16777218u, UINT32_MAX}) {
        CHECK(usdGen::gpu::detail::CurveGrowWidthLowerIndex(0.f, span) == 0);
        CHECK(usdGen::gpu::detail::CurveGrowWidthLowerIndex(1.f, span) == span - 1);
        CHECK(usdGen::gpu::detail::CurveGrowWidthLowerIndex(.5f, span) < span);
    }
    int count=0, device=-1;
    CHECK(cudaGetDeviceCount(&count)==cudaSuccess); if (!count) return 77;
    CHECK(cudaGetDevice(&device)==cudaSuccess);
    CHECK(ConfigureCudaExecutionResources(device, {size_t{8} << 20, 0}));
    auto pool=FindUsdGenExecutionResourcePool({UsdGenExecutionResourceBackend::Cuda,device}); CHECK(pool);
    auto const baseline=pool->Snapshot();
    cudaStream_t stream=nullptr; CHECK(cudaStreamCreate(&stream)==cudaSuccess);
    if (argc == 2 && std::string(argv[1]) == "--quarantine") {
        auto input=std::make_shared<InputOwner>();
        CHECK(Upload(input->points,{V(0,0,0),V(0,0,1)},stream));
        CHECK(Upload(input->rest,{V(0,0,0),V(0,0,1)},stream));
        CHECK(Upload(input->offsets,{0u,2u},stream)&&Upload(input->ids,{1ull},stream));
        CHECK(Upload(input->rootPrim,{0},stream)&&Upload(input->rootUV,{make_float2(0,0)},stream));
        CHECK(Upload(input->rootT,{V(1,0,0)},stream)&&Upload(input->rootB,{V(0,1,0)},stream)&&Upload(input->rootN,{V(0,0,1)},stream));
        auto const inputBaseline=pool->Snapshot(); std::weak_ptr<InputOwner> weak=input;
        { CudaCurveGrow grow; CurveGrowControls c;
          CHECK(grow.BeginFresh(input->Input(1,2),input,c,stream)==CurveGrowStatus::Ok);
          input.reset(); grow.MarkUnprovenWork(); }
        CHECK(!weak.expired() && pool->Snapshot().usedBytes > inputBaseline.usedBytes);
        // The stream is synchronized only so process teardown has no racing
        // device work; the intentionally abandoned owner remains charged.
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess);
        CHECK(cudaStreamDestroy(stream)==cudaSuccess); return 0;
    }
    {
        auto input=std::make_shared<InputOwner>();
        CHECK(Upload(input->points,{V(120,0,0),V(121,0,0),V(122,0,0),V(110,0,0),V(111,0,0)},stream));
        CHECK(Upload(input->rest,{V(20,0,0),V(21,0,0),V(22,0,0),V(10,0,0),V(11,0,0)},stream));
        CHECK(Upload(input->widths,{.1f,.2f,.4f,1.f,.5f},stream));
        CHECK(Upload(input->offsets,{0u,3u,5u},stream));
        CHECK(Upload(input->ids,{40ull,7ull},stream));
        CHECK(Upload(input->rootPrim,{2,1},stream));
        CHECK(Upload(input->rootUV,{make_float2(.2f,.3f),make_float2(.4f,.5f)},stream));
        CHECK(Upload(input->rootT,{V(1,0,0),V(0,1,0)},stream));
        CHECK(Upload(input->rootB,{V(0,1,0),V(-1,0,0)},stream));
        CHECK(Upload(input->rootN,{V(0,0,1),V(0,0,2)},stream));
        CurveGrowControls controls; controls.cvCount=4; controls.seed=19; controls.length=2.0;
        controls.randomLo=.5; controls.randomHi=1.5;
        CurveGrowRequirements requirements;
        CHECK(GetCurveGrowRequirements(2,4,&requirements)==CurveGrowStatus::Ok);
        auto const inputBaseline=pool->Snapshot();
        std::weak_ptr<InputOwner> weak=input;
        CudaCurveGrow grow;
        CHECK(grow.BeginFresh(input->Input(2,5),input,controls,stream)==CurveGrowStatus::Ok);
        CHECK(pool->Snapshot().usedBytes-inputBaseline.usedBytes==requirements.peakBytes);
        input.reset(); CHECK(!weak.expired());
        Relay relay; CHECK(grow.FinishFreshAsync(stream,Done,&relay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess && relay.status.load()==int(cudaSuccess));
        CHECK(grow.CommitFreshFinish()==CurveGrowStatus::Ok && weak.expired());
        CHECK(pool->Snapshot().usedBytes==baseline.usedBytes+requirements.peakBytes);
        auto geometry=grow.view(); CHECK(geometry.curveCount==2 && geometry.pointCount==8 && grow.generation()==1);
        std::vector<float3> points(8), rest(8), t(2), b(2), n(2); std::vector<float> widths(8), hair(8);
        std::vector<uint32_t> offsets(3); std::vector<uint64_t> ids(2); std::vector<int32_t> prim(2); std::vector<float2> uv(2);
        CHECK(Download(geometry.points,&points,stream)&&Download(geometry.restPoints,&rest,stream)&&Download(geometry.widths,&widths,stream)&&Download(grow.hairT(),&hair,stream)&&Download(geometry.curveOffsets,&offsets,stream)&&Download(geometry.stableIds,&ids,stream)&&Download(grow.rootPrim(),&prim,stream)&&Download(grow.rootUV(),&uv,stream)&&Download(grow.rootT(),&t,stream)&&Download(grow.rootB(),&b,stream)&&Download(grow.rootN(),&n,stream));
        CHECK(offsets==std::vector<uint32_t>({0,4,8}) && ids==std::vector<uint64_t>({40,7}) && prim==std::vector<int32_t>({2,1}));
        CHECK(Near(t[0],V(1,0,0))&&Near(b[1],V(-1,0,0))&&Near(n[1],V(0,0,2))&&Near(uv[1].x,.4f));
        float const rootsX[]={120.f,110.f}, restsX[]={20.f,10.f};
        float const sourceWidths[][3]={{.1f,.2f,.4f},{1.f,.5f,0.f}};
        uint32_t const sourceCount[]={3,2}; uint64_t const sourceId[]={40,7};
        for(uint32_t c=0;c<2;++c) for(uint32_t i=0;i<4;++i) {
            float h=float(i)/3.f;
            float target=static_cast<float>(controls.length*(controls.randomLo+
                double(UsdGenDraw01(controls.seed,sourceId[c],kSaltGrow))*(controls.randomHi-controls.randomLo)));
            float d=target*h;
            CHECK(Near(points[c*4+i],V(rootsX[c],0,d)) && Near(rest[c*4+i],V(restsX[c],0,d)) && Near(hair[c*4+i],h));
            float pos=h*float(sourceCount[c]-1); uint32_t lo=uint32_t(pos), hi=std::min(lo+1,sourceCount[c]-1);
            CHECK(Near(widths[c*4+i],sourceWidths[c][lo]+(sourceWidths[c][hi]-sourceWidths[c][lo])*(pos-float(lo))));
        }
        // The first producer deliberately releases its source owner on
        // commit; build an independent source owner for this map transaction.
        auto mapInput = std::make_shared<InputOwner>();
        CHECK(Upload(mapInput->points,{V(120,0,0),V(121,0,0),V(122,0,0),V(110,0,0),V(111,0,0)},stream) &&
              Upload(mapInput->rest,{V(20,0,0),V(21,0,0),V(22,0,0),V(10,0,0),V(11,0,0)},stream) &&
              Upload(mapInput->widths,{.1f,.2f,.4f,1.f,.5f},stream) &&
              Upload(mapInput->offsets,{0u,3u,5u},stream) && Upload(mapInput->ids,{40ull,7ull},stream) &&
              Upload(mapInput->frameStableIds,{7ull,40ull},stream) &&
              Upload(mapInput->rootPrim,{2,1},stream) &&
              Upload(mapInput->rootUV,{make_float2(.2f,.3f),make_float2(.4f,.5f)},stream) &&
              Upload(mapInput->rootT,{V(0,1,0),V(1,0,0)},stream) &&
              Upload(mapInput->rootB,{V(-1,0,0),V(0,1,0)},stream) &&
              Upload(mapInput->rootN,{V(0,0,2),V(0,0,1)},stream));
        auto mapImage = UsdGenImagePayload::Create(1, 1, 1, std::vector<float>{.5f});
        CHECK(mapImage);
        std::weak_ptr<const UsdGenImagePayload> weakMap = mapImage;
        GrowLengthMap lengthMap; lengthMap.image = mapImage;
        lengthMap.options.filter = UsdGenImageFilter::Nearest;
        lengthMap.options.clampOutput = false;
        CurveGrowRequirements mappedRequirements;
        CHECK(GetCurveGrowRequirements(2, 4, &mappedRequirements, mapImage->TexelCount()) ==
              CurveGrowStatus::Ok &&
              mappedRequirements.mapScratchBytes == 3 * sizeof(float));
        CurveGrowControls mapped = controls; mapped.length = 2.0; mapped.randomLo = 1.0; mapped.randomHi = 1.0;
        CudaCurveGrow mappedGrow; Relay mappedRelay;
        auto const beforeMapped = pool->Snapshot();
        CHECK(mappedGrow.BeginFresh(mapInput->Input(2,5),mapInput,mapped,stream,nullptr,&lengthMap)==CurveGrowStatus::Ok);
        CHECK(pool->Snapshot().usedBytes - beforeMapped.usedBytes == mappedRequirements.outputBytes +
              mappedRequirements.statusBytes + mappedRequirements.mapScratchBytes);
        lengthMap.image.reset(); mapImage.reset(); CHECK(!weakMap.expired());
        CHECK(mappedGrow.FinishFreshAsync(stream,Done,&mappedRelay)==CurveGrowStatus::Ok &&
              cudaStreamSynchronize(stream)==cudaSuccess &&
              mappedRelay.status.load()==int(cudaSuccess) &&
              mappedGrow.CommitFreshFinish()==CurveGrowStatus::Ok);
        CHECK(pool->Snapshot().usedBytes - beforeMapped.usedBytes == mappedRequirements.outputBytes +
              mappedRequirements.statusBytes);
        std::vector<float3> mappedPoints(8), mappedT(2);
        CHECK(Download(mappedGrow.view().points,&mappedPoints,stream) &&
              Download(mappedGrow.rootT(),&mappedT,stream) &&
              Near(mappedPoints[3],V(120,0,1.0f)));
        CHECK(Near(mappedT[0],V(1,0,0)) && Near(mappedT[1],V(0,1,0)));
        // Mapped frame metadata is preflight-validated before any device work.
        auto malformedInput = mapInput->Input(2,5);
        malformedInput.frameStableIds = {mapInput->frameStableIds.data(), 1};
        CudaCurveGrow malformedFrames;
        CHECK(malformedFrames.BeginFresh(malformedInput, mapInput, mapped, stream) ==
              CurveGrowStatus::InvalidTopology);
        malformedInput = mapInput->Input(2,5);
        malformedInput.rootB = {};
        CHECK(malformedFrames.BeginFresh(malformedInput, mapInput, mapped, stream) ==
              CurveGrowStatus::InvalidTopology);
        malformedInput = mapInput->Input(2,5);
        malformedInput.frameStableIds = {nullptr, 2};
        CHECK(malformedFrames.BeginFresh(malformedInput, mapInput, mapped, stream) ==
              CurveGrowStatus::InvalidTopology);
        // Validate the complete map, including entries not referenced by an
        // active curve. Device rejection is a proved semantic failure.
        CHECK(Upload(mapInput->rootT,{V(0,1,0),V(1,0,0),V(1,0,0)},stream) &&
              Upload(mapInput->rootB,{V(-1,0,0),V(0,1,0),V(0,1,0)},stream) &&
              Upload(mapInput->rootN,{V(0,0,2),V(0,0,1),V(0,0,1)},stream));
        for (auto const& invalidIds : std::vector<std::vector<uint64_t>>{
                 {7,40,40}, {7,40,30}, {7,41,50}}) {
            CHECK(Upload(mapInput->frameStableIds,invalidIds,stream));
            CudaCurveGrow rejected; Relay rejectedRelay;
            CHECK(rejected.BeginFresh(mapInput->Input(2,5),mapInput,mapped,stream) ==
                  CurveGrowStatus::Ok);
            CHECK(rejected.FinishFreshAsync(stream,Done,&rejectedRelay) == CurveGrowStatus::Ok &&
                  cudaStreamSynchronize(stream) == cudaSuccess &&
                  rejectedRelay.status.load() == int(cudaSuccess) &&
                  rejected.CommitFreshFinish() == CurveGrowStatus::InvalidTopology &&
                  !rejected.HasUnprovenWork());
            std::vector<float3> heldPoints(8), heldT(2);
            CHECK(Download(mappedGrow.view().points,&heldPoints,stream) &&
                  Download(mappedGrow.rootT(),&heldT,stream) &&
                  std::memcmp(heldPoints.data(),mappedPoints.data(),8*sizeof(float3)) == 0 &&
                  std::memcmp(heldT.data(),mappedT.data(),2*sizeof(float3)) == 0);
        }
        CHECK(Upload(mapInput->rootT,{V(0,1,0),V(1,0,0)},stream) &&
              Upload(mapInput->rootB,{V(-1,0,0),V(0,1,0)},stream) &&
              Upload(mapInput->rootN,{V(0,0,2),V(0,0,1)},stream) &&
              Upload(mapInput->frameStableIds,{7ull,40ull},stream));
        uint64_t const validFrameIds[] = {7ull,40ull};
        CHECK(cudaMemcpy(mapInput->frameStableIds.data(), validFrameIds,
                         sizeof(validFrameIds), cudaMemcpyHostToDevice) == cudaSuccess);
        lengthMap.image.reset(); CHECK(weakMap.expired());
        auto negativeImage = UsdGenImagePayload::Create(1, 1, 1, std::vector<float>{-1.0f});
        CHECK(negativeImage);
        GrowLengthMap negativeMap; negativeMap.image = negativeImage;
        negativeMap.options.filter = UsdGenImageFilter::Nearest;
        negativeMap.options.clampOutput = false;
        CudaCurveGrow negativeMapGrow; Relay negativeMapRelay;
        CHECK(negativeMapGrow.BeginFresh(mapInput->Input(2,5),mapInput,mapped,stream,nullptr,
                                         &negativeMap) == CurveGrowStatus::Ok &&
              negativeMapGrow.FinishFreshAsync(stream,Done,&negativeMapRelay) == CurveGrowStatus::Ok &&
              cudaStreamSynchronize(stream) == cudaSuccess &&
              negativeMapRelay.status.load() == int(cudaSuccess) &&
              negativeMapGrow.CommitFreshFinish() == CurveGrowStatus::NonFiniteInput &&
              !negativeMapGrow.HasUnprovenWork());
        UsdGenImageSampleOptions badOptions = negativeMap.options;
        badOptions.scale = std::numeric_limits<float>::infinity();
        GrowLengthMap invalidMap{negativeImage, badOptions};
        CudaCurveGrow invalidMapGrow;
        CHECK(invalidMapGrow.BeginFresh(mapInput->Input(2,5),mapInput,mapped,stream,nullptr,
                                        &invalidMap) == CurveGrowStatus::InvalidArgument);
        auto const* published=geometry.points.data;
        CHECK(grow.BeginFresh({}, {}, controls, stream)==CurveGrowStatus::InvalidArgument && grow.view().points.data==published);
        grow.ReclassifyPublishedGeneration();
    }
    CHECK(pool->Snapshot().usedBytes==baseline.usedBytes);

    // Empty C3 still carries a one-element offsets owner and never launches a
    // zero-grid kernel.
    {
        auto input=std::make_shared<InputOwner>(); CHECK(Upload(input->offsets,{0u},stream));
        CudaCurveGrow empty; CurveGrowControls c; Relay relay;
        CHECK(empty.BeginFresh(input->Input(0,0),input,c,stream)==CurveGrowStatus::Ok);
        CHECK(empty.FinishFreshAsync(stream,Done,&relay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess&&relay.status.load()==int(cudaSuccess));
        CHECK(empty.CommitFreshFinish()==CurveGrowStatus::Ok&&empty.curveCount()==0&&empty.pointCount()==0);
        std::vector<uint32_t> offsets(1); CHECK(Download(empty.view().curveOffsets,&offsets,stream)&&offsets[0]==0);
        CHECK(Upload(input->frameStableIds,{7ull},stream));
        auto malformed = input->Input(0,0);
        malformed.rootT = {nullptr,1}; malformed.rootB = {nullptr,1};
        malformed.rootN = {nullptr,1};
        CudaCurveGrow rejected;
        CHECK(rejected.BeginFresh(malformed,input,c,stream) == CurveGrowStatus::InvalidTopology);
        // An empty active topology may retain a structurally valid map domain.
        CHECK(Upload(input->rootT,{V(1,0,0)},stream) &&
              Upload(input->rootB,{V(0,1,0)},stream) &&
              Upload(input->rootN,{V(0,0,1)},stream));
        CudaCurveGrow mappedEmpty; Relay mappedRelay;
        CHECK(mappedEmpty.BeginFresh(input->Input(0,0),input,c,stream) == CurveGrowStatus::Ok &&
              mappedEmpty.FinishFreshAsync(stream,Done,&mappedRelay) == CurveGrowStatus::Ok &&
              cudaStreamSynchronize(stream) == cudaSuccess &&
              mappedRelay.status.load() == int(cudaSuccess) &&
              mappedEmpty.CommitFreshFinish() == CurveGrowStatus::Ok &&
              mappedEmpty.curveCount() == 0 && mappedEmpty.pointCount() == 0);
    }
    {
        auto malformed=std::make_shared<InputOwner>(); CHECK(Upload(malformed->offsets,{1u},stream));
        CudaCurveGrow empty; CurveGrowControls c; Relay relay;
        CHECK(empty.BeginFresh(malformed->Input(0,0),malformed,c,stream)==CurveGrowStatus::Ok);
        CHECK(empty.FinishFreshAsync(stream,Done,&relay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess&&relay.status.load()==int(cudaSuccess));
        CHECK(empty.CommitFreshFinish()==CurveGrowStatus::InvalidTopology&&!empty.HasUnprovenWork());
    }
    CHECK(pool->Snapshot().usedBytes==baseline.usedBytes);

    // Absent input widths get the explicit fallback. RootTangent and literal
    // directions share the same fresh topology path; reversed random bounds
    // are canonicalized before the device draw.
    {
        auto input=std::make_shared<InputOwner>();
        CHECK(Upload(input->points,{V(1,2,3),V(1,2,4)},stream)&&Upload(input->rest,{V(9,8,7),V(9,8,8)},stream));
        CHECK(Upload(input->offsets,{0u,2u},stream)&&Upload(input->ids,{99ull},stream)&&Upload(input->rootPrim,{3},stream));
        CHECK(Upload(input->rootUV,{make_float2(.3f,.4f)},stream)&&Upload(input->rootT,{V(1,0,0)},stream)&&Upload(input->rootB,{V(0,1,0)},stream)&&Upload(input->rootN,{V(0,0,1)},stream));
        CurveGrowControls c; c.cvCount=2; c.length=2; c.randomLo=1.5; c.randomHi=.5; c.fallbackWidth=.25f; c.direction=CurveGrowDirection::RootTangent;
        CudaCurveGrow tangent; Relay relay;
        CHECK(tangent.BeginFresh(input->Input(1,2),input,c,stream)==CurveGrowStatus::Ok);
        // No terminal callback has been installed: commit must retain the
        // pending owner. Do not block a stream callback while issuing CUDA
        // calls on that stream; instrumentation may make those calls wait.
        CHECK(tangent.CommitFreshFinish()==CurveGrowStatus::NoPendingUpdate&&tangent.HasUnprovenWork());
        CHECK(tangent.FinishFreshAsync(stream,Done,&relay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess&&relay.status.load()==int(cudaSuccess)&&tangent.CommitFreshFinish()==CurveGrowStatus::Ok);
        std::vector<float3> points(2),rest(2); std::vector<float> widths(2); CHECK(Download(tangent.view().points,&points,stream)&&Download(tangent.view().restPoints,&rest,stream)&&Download(tangent.view().widths,&widths,stream));
        float target=float(2.0*(.5+double(UsdGenDraw01(0,99,kSaltGrow))));
        CHECK(Near(points[1],V(1+target,2,3))&&Near(rest[1],V(9+target,8,7))&&Near(widths[0],.25f)&&Near(widths[1],.25f));
        CurveGrowControls literal=c; literal.direction=CurveGrowDirection::Literal; literal.literalDirection=V(0,1,0);
        CudaCurveGrow literalGrow; Relay literalRelay;
        CHECK(literalGrow.BeginFresh(input->Input(1,2),input,literal,stream)==CurveGrowStatus::Ok&&literalGrow.FinishFreshAsync(stream,Done,&literalRelay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess&&literalRelay.status.load()==int(cudaSuccess)&&literalGrow.CommitFreshFinish()==CurveGrowStatus::Ok);
        points.assign(2,{}); CHECK(Download(literalGrow.view().points,&points,stream)&&Near(points[1],V(1,2+target,3)));
        // Use a non-world-Y frame axis so the test detects hard-coded
        // rotation axes in the native lowering.
        CHECK(Upload(input->rootB,{V(0,0,1)},stream) &&
              Upload(input->rootN,{V(1,0,0)},stream));
        CurveGrowControls angular=c; angular.length=1.0; angular.randomLo=1.0; angular.randomHi=1.0;
        angular.direction=CurveGrowDirection::RootNormal; angular.lift=90.0f;
        CudaCurveGrow lifted; Relay liftedRelay;
        CHECK(lifted.BeginFresh(input->Input(1,2),input,angular,stream)==CurveGrowStatus::Ok &&
              lifted.FinishFreshAsync(stream,Done,&liftedRelay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess &&
              liftedRelay.status.load()==int(cudaSuccess) &&
              lifted.CommitFreshFinish()==CurveGrowStatus::Ok);
        points.assign(2,{}); rest.assign(2,{});
        CHECK(Download(lifted.view().points,&points,stream) &&
              Download(lifted.view().restPoints,&rest,stream) &&
              Near(points[1],V(1,3,3)) && Near(rest[1],V(9,9,7)));
        angular.lift=-90.0f;
        CudaCurveGrow negativeLift; Relay negativeRelay;
        CHECK(negativeLift.BeginFresh(input->Input(1,2),input,angular,stream)==CurveGrowStatus::Ok &&
              negativeLift.FinishFreshAsync(stream,Done,&negativeRelay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess &&
              negativeRelay.status.load()==int(cudaSuccess) &&
              negativeLift.CommitFreshFinish()==CurveGrowStatus::Ok);
        points.assign(2,{}); CHECK(Download(negativeLift.view().points,&points,stream) &&
              Near(points[1],V(1,1,3)));

        // uvBlend is applied after lift: 0 preserves the lifted direction,
        // 1 selects normalized rootT, and interior values normalize the mix.
        auto runBlend = [&](float blend, float3 tangent, float3 expected, float lift = 90.0f) {
            if (!Upload(input->rootT,{tangent},stream)) return false;
            CurveGrowControls blendControls = angular;
            blendControls.lift = lift;
            blendControls.uvBlend = blend;
            CudaCurveGrow blended; Relay blendedRelay;
            if (!(blended.BeginFresh(input->Input(1,2),input,blendControls,stream)==CurveGrowStatus::Ok &&
                  blended.FinishFreshAsync(stream,Done,&blendedRelay)==CurveGrowStatus::Ok &&
                  cudaStreamSynchronize(stream)==cudaSuccess &&
                  blendedRelay.status.load()==int(cudaSuccess) &&
                  blended.CommitFreshFinish()==CurveGrowStatus::Ok)) return false;
            points.assign(2,{});
            if (!(Download(blended.view().points,&points,stream) && Near(points[1],expected))) return false;
            return true;
        };
        constexpr float kDiag = 0.7071067811865475f;
        CHECK(runBlend(0.0f,V(1,0,0),V(1,3,3)) &&
              runBlend(1.0f,V(1,0,0),V(2,2,3)) &&
              runBlend(0.5f,V(1,0,0),V(1+kDiag,2+kDiag,3)) &&
              // Antipodal cancellation must retain the lifted direction.
              runBlend(0.5f,V(-1,0,0),V(2,2,3),0.0f) &&
              // A degenerate tangent has the same retain-lift behavior.
              runBlend(1.0f,V(0,0,0),V(1,3,3)));
        CurveGrowControls badBlend = angular; badBlend.uvBlend = 1.01f;
        CudaCurveGrow rejectedBlend;
        CHECK(rejectedBlend.BeginFresh(input->Input(1,2),input,badBlend,stream)==CurveGrowStatus::InvalidArgument);
        badBlend.uvBlend = std::numeric_limits<float>::quiet_NaN();
        CHECK(rejectedBlend.BeginFresh(input->Input(1,2),input,badBlend,stream)==CurveGrowStatus::InvalidArgument);
        CurveGrowControls lift=c; lift.lift=90.1f; CudaCurveGrow rejected;
        CHECK(rejected.BeginFresh(input->Input(1,2),input,lift,stream)==CurveGrowStatus::InvalidArgument);
        lift.lift=std::numeric_limits<float>::infinity();
        CudaCurveGrow nonFiniteLift;
        CHECK(nonFiniteLift.BeginFresh(input->Input(1,2),input,lift,stream)==CurveGrowStatus::InvalidArgument);
        CurveGrowControls overflow=c; overflow.length=std::numeric_limits<double>::max(); CudaCurveGrow nonfinite; Relay overflowRelay;
        CHECK(nonfinite.BeginFresh(input->Input(1,2),input,overflow,stream)==CurveGrowStatus::Ok&&nonfinite.FinishFreshAsync(stream,Done,&overflowRelay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess&&overflowRelay.status.load()==int(cudaSuccess));
        CHECK(nonfinite.CommitFreshFinish()==CurveGrowStatus::NonFiniteInput&&!nonfinite.HasUnprovenWork());
    }
    CHECK(pool->Snapshot().usedBytes==baseline.usedBytes);

    // Structural rejection is entirely pre-submit; malformed ragged offsets
    // are detected by the device status and leave no committed output.
    {
        auto input=std::make_shared<InputOwner>();
        CHECK(Upload(input->points,{V(0,0,0)},stream)&&Upload(input->rest,{V(0,0,0)},stream)&&Upload(input->widths,{.1f},stream));
        CHECK(Upload(input->offsets,{0u,1u},stream)&&Upload(input->ids,{1ull},stream)&&Upload(input->rootPrim,{0},stream));
        CHECK(Upload(input->rootUV,{make_float2(0,0)},stream)&&Upload(input->rootT,{V(1,0,0)},stream)&&Upload(input->rootB,{V(0,1,0)},stream)&&Upload(input->rootN,{V(0,0,1)},stream));
        CudaCurveGrow invalid; CurveGrowControls c; c.cvCount=1;
        CHECK(invalid.BeginFresh(input->Input(1,1),input,c,stream)==CurveGrowStatus::InvalidArgument);
        c.cvCount=2; Relay relay;
        CHECK(invalid.BeginFresh(input->Input(1,1),input,c,stream)==CurveGrowStatus::Ok);
        CHECK(invalid.FinishFreshAsync(stream,Done,&relay)==CurveGrowStatus::Ok);
        CHECK(cudaStreamSynchronize(stream)==cudaSuccess&&relay.status.load()==int(cudaSuccess));
        CHECK(invalid.CommitFreshFinish()==CurveGrowStatus::InvalidTopology&&invalid.view().points.data==nullptr&&!invalid.HasUnprovenWork());
    }
    CHECK(pool->Snapshot().usedBytes==baseline.usedBytes);
    CHECK(cudaStreamDestroy(stream)==cudaSuccess);
    return 0;
}
