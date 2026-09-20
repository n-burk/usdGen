// usdGenTonic — the asynchronous region-map bake implementation (P2).
#include "usdGenTonic/tonicBake.h"

#include "usdGenTonic/tonicHierarchy.h"
#include "usdGenTonic/tonicModel.h"

#include "usdGen/maps/ptexMap.h"

#ifdef USDGEN_TONIC_HAS_CUDA
#include "usdGenTonic/tonicKernels.h"
#include "usdGen/gpu/deviceBuffer.h"
#include <cuda_runtime.h>
#endif

// Last: <Ptexture.h> brings in <windows.h>.
#include <Ptexture.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <thread>

namespace usdGenTonic {

namespace {

namespace fs = std::filesystem;

struct _BakeFace {
    int coarse = -1;
    int sub = 0;  // n-gon sub-face, else 0
    int resU = 1;
    int resV = 1;
    float corners[12];
};

struct _BakePlan {
    std::vector<_BakeFace> faces;  // ptex-face order
    std::vector<int> firstIds;  // per coarse face (UsdGenPtexFirstFaceIds)
    int channels = 1;
};

bool _PlanFaces(TonicBakeInput const &input, TonicRegionMaps const &maps,
                std::vector<int> const &resLog2, _BakePlan *plan,
                std::string *err)
{
    TonicScalpMesh const &mesh = *input.scalp;
    size_t const faceCount = mesh.faceVertexCounts.size();
    int total = 0;
    plan->firstIds = usdGen::UsdGenPtexFirstFaceIds(
        mesh.faceVertexCounts.data(), faceCount, &total);
    if (plan->firstIds.empty() || total < 0) {
        if (err) {
            *err = "TonicBakePtex: ptex face ids overflow";
        }
        return false;
    }
    plan->channels = std::max(input.levelCount, 1);
    plan->faces.clear();
    plan->faces.reserve(size_t(total));
    for (size_t f = 0; f < faceCount; ++f) {
        int const nv = mesh.faceVertexCounts[f];
        int const subs = (nv == 4) ? 1 : nv;
        int const r = resLog2[f];
        for (int s = 0; s < subs; ++s) {
            _BakeFace bf;
            bf.coarse = int(f);
            bf.sub = s;
            int rr = (subs == 1) ? r : std::max(r - 1, 0);
            bf.resU = bf.resV = 1 << rr;
            float const *pts = mesh.points.data();
            if (!usdGen::UsdGenPtexFaceCorners(
                    pts, mesh.points.size() / 3,
                    mesh.faceVertexCounts.data(),
                    mesh.faceVertexIndices.data(), mesh.faceOffsets.data(),
                    faceCount, int(f), s, bf.corners)) {
                if (err) {
                    *err = "TonicBakePtex: bad face corners";
                }
                return false;
            }
            plan->faces.push_back(bf);
        }
    }
    return true;
}

void _TexelPosition(_BakeFace const &bf, int tu, int tv, float out[3])
{
    float const u = (float(tu) + 0.5f) / float(bf.resU);
    float const v = (float(tv) + 0.5f) / float(bf.resV);
    float const w0 = (1.0f - u) * (1.0f - v);
    float const w1 = u * (1.0f - v);
    float const w2 = u * v;
    float const w3 = (1.0f - u) * v;
    for (int a = 0; a < 3; ++a) {
        out[a] = w0 * bf.corners[a] + w1 * bf.corners[3 + a] +
                 w2 * bf.corners[6 + a] + w3 * bf.corners[9 + a];
    }
}

bool _WritePtexFile(_BakePlan const &plan,
                    std::vector<std::vector<float>> const &texelCache,
                    std::string const &outPath, std::string *err)
{
    auto fail = [&](std::string const &what) {
        if (err) {
            *err = "TonicBakePtex: " + what;
        }
        return false;
    };
    if (texelCache.size() != plan.faces.size()) {
        return fail("texel cache does not cover every ptex face");
    }
    std::string const tmp = outPath + ".tmp";
    {
        Ptex::String error;
        PtexPtr<PtexWriter> writer(PtexWriter::open(
            tmp.c_str(), Ptex::mt_quad, Ptex::dt_float, plan.channels, -1,
            int(plan.faces.size()), error, true));
        if (!writer) {
            return fail("cannot open " + tmp + ": " + error.c_str());
        }
        bool ok = true;
        for (size_t id = 0; id < plan.faces.size() && ok; ++id) {
            _BakeFace const &bf = plan.faces[id];
            int adjfaces[4] = {-1, -1, -1, -1};
            int adjedges[4] = {0, 0, 0, 0};
            auto log2i = [](int n) {
                int r = 0;
                while ((1 << (r + 1)) <= n) {
                    ++r;
                }
                return r;
            };
            Ptex::FaceInfo info(Ptex::Res(log2i(bf.resU), log2i(bf.resV)),
                                adjfaces, adjedges);
            size_t const want =
                size_t(bf.resU) * size_t(bf.resV) * size_t(plan.channels);
            if (texelCache[id].size() != want) {
                return fail("ptex face " + std::to_string(id) + " caches " +
                            std::to_string(texelCache[id].size()) +
                            " floats for " + std::to_string(want));
            }
            ok = writer->writeFace(int(id), info, texelCache[id].data());
        }
        ok = writer->close(error) && ok;
        if (!ok) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return fail("write failed: " + std::string(error.c_str()));
        }
    }
    // Tmp -> rename: the versioned path never names a half-written file.
    std::error_code ec;
    fs::rename(tmp, outPath, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("rename to " + outPath + " failed");
    }
    return true;
}

// A face whose id is unchanged can still need re-classification: the
// res-0 collapse keys on corner uniformity, so a graph edit can flip a
// face's resolution (and a level/res-override change flips every face's
// texel count) without touching its id. Any cache entry whose size misses
// the plan is stale by construction and joins the dirty set.
void _HealStaleCache(_BakePlan const &plan,
                     std::vector<std::vector<float>> const &texelCache,
                     std::vector<char> *isDirty)
{
    for (size_t id = 0; id < plan.faces.size(); ++id) {
        _BakeFace const &bf = plan.faces[id];
        size_t const want = size_t(bf.resU) * size_t(bf.resV) *
                            size_t(plan.channels);
        if (id < texelCache.size() && texelCache[id].size() != want) {
            (*isDirty)[size_t(bf.coarse)] = 1;
        }
    }
}

// The hierarchy side of the channel layout (plan/17 §4.5, plan/18 §7 G4):
// channel 0 is the L1 interpolation id, channel k the id of the level-(k+1)
// tube the texel falls in. Built once per bake from TonicBakeInput::tubes.
struct _LevelIndex {
    // interp id -> L1 tube id.
    std::map<int, int> l1OfRegion;
    std::map<int, TonicBakeTube> byId;
    std::map<int, std::vector<int>> childrenOf;  // childIndex order

    bool empty() const { return byId.empty(); }
};

_LevelIndex _BuildLevelIndex(TonicBakeInput const &input,
                             TonicRegionLoops const &loops)
{
    _LevelIndex index;
    for (TonicBakeTube const &tube : input.tubes) {
        index.byId[tube.tubeId] = tube;
        if (tube.parentTubeId != -1) {
            index.childrenOf[tube.parentTubeId].push_back(tube.tubeId);
        } else if (tube.level == 1) {
            int interp = tube.regionId;
            if (tube.regionId >= 0 &&
                size_t(tube.regionId) < loops.interpIds.size()) {
                interp = loops.interpIds[size_t(tube.regionId)];
            }
            index.l1OfRegion.emplace(interp, tube.tubeId);
        }
    }
    for (auto &kv : index.childrenOf) {
        std::sort(kv.second.begin(), kv.second.end(), [&](int a, int b) {
            return index.byId[a].childIndex < index.byId[b].childIndex;
        });
    }
    return index;
}

// Fill texels[1 .. channels-1] for one point. `region` is the channel-0 id.
void _ClassifyLevels(_LevelIndex const &index, int region, float const p[3],
                     int channels, float *texels)
{
    for (int c = 1; c < channels; ++c) {
        texels[c] = 0.0f;
    }
    if (channels <= 1 || index.empty()) {
        return;
    }
    auto const l1 = index.l1OfRegion.find(region);
    if (l1 == index.l1OfRegion.end()) {
        return;
    }
    int current = l1->second;
    for (int c = 1; c < channels; ++c) {
        auto const kids = index.childrenOf.find(current);
        if (kids == index.childrenOf.end() || kids->second.empty()) {
            return;  // this level has no tube here: 0, per §2.2
        }
        auto const parent = index.byId.find(current);
        if (parent == index.byId.end()) {
            return;
        }
        std::vector<float> centers;
        centers.reserve(kids->second.size() * 3);
        for (int kid : kids->second) {
            auto const it = index.byId.find(kid);
            if (it == index.byId.end()) {
                return;
            }
            centers.push_back(it->second.rootCenter[0]);
            centers.push_back(it->second.rootCenter[1]);
            centers.push_back(it->second.rootCenter[2]);
        }
        int const cell = TonicOwningChildCell(
            parent->second.rootCenter, parent->second.rootFrame,
            centers.data(), int(kids->second.size()), p);
        if (cell < 0) {
            return;
        }
        current = kids->second[size_t(cell)];
        texels[c] = float(current);
    }
}

// Classify every texel of the plan's dirty coarse faces on the CPU.
void _ClassifyDirtyCpu(TonicRegionLoops const &loops, _BakePlan const &plan,
                       _LevelIndex const &levels,
                       std::vector<char> const &isDirty,
                       std::vector<std::vector<float>> *texelCache,
                       TonicBakeStats *stats)
{
    for (size_t id = 0; id < plan.faces.size(); ++id) {
        _BakeFace const &bf = plan.faces[id];
        if (!isDirty[size_t(bf.coarse)]) {
            continue;
        }
        std::vector<float> texels(size_t(bf.resU) * size_t(bf.resV) *
                                  size_t(plan.channels));
        for (int tv = 0; tv < bf.resV; ++tv) {
            for (int tu = 0; tu < bf.resU; ++tu) {
                float p[3];
                _TexelPosition(bf, tu, tv, p);
                int const region = TonicClassifyPointCpu(loops, p);
                size_t const o = (size_t(tv) * size_t(bf.resU) + size_t(tu)) *
                                 size_t(plan.channels);
                texels[o] = float(region);
                _ClassifyLevels(levels, region, p, plan.channels,
                                &texels[o]);
                if (stats) {
                    ++stats->texelsClassified;
                }
            }
        }
        (*texelCache)[id] = std::move(texels);
        if (stats) {
            ++stats->facesClassified;
        }
    }
}

#ifdef USDGEN_TONIC_HAS_CUDA

// Lowest-priority stream for the bake (plan/17 §3.1a): a bake in flight
// cannot delay a move on tonicStream.
cudaStream_t _BakeStream()
{
    int least = 0, greatest = 0;
    if (cudaDeviceGetStreamPriorityRange(&least, &greatest) != cudaSuccess) {
        return nullptr;
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithPriority(&stream, cudaStreamNonBlocking, least) !=
        cudaSuccess) {
        return nullptr;
    }
    return stream;
}

// A pinned host block for the classify download: cudaMemcpyAsync on the
// bake stream only overlaps (and only stays off the default stream) when
// the destination is page-locked.
struct _PinnedInts {
    int *data = nullptr;
    size_t count = 0;
    ~_PinnedInts()
    {
        if (data) {
            cudaFreeHost(data);
        }
    }
    bool reset(size_t n)
    {
        if (data) {
            cudaFreeHost(data);
            data = nullptr;
            count = 0;
        }
        if (n == 0) {
            return true;
        }
        void *p = nullptr;
        if (cudaHostAlloc(&p, n * sizeof(int), cudaHostAllocDefault) !=
            cudaSuccess) {
            return false;
        }
        data = static_cast<int *>(p);
        count = n;
        return true;
    }
};

bool _ClassifyDirtyGpu(TonicRegionLoops const &loops, _BakePlan const &plan,
                       _LevelIndex const &levels,
                       std::vector<char> const &isDirty,
                       std::vector<std::vector<float>> *texelCache,
                       cudaStream_t stream, TonicBakeStats *stats,
                       std::string *err)
{
    auto fail = [&](char const *what) {
        if (err) {
            *err = what;
        }
        return false;
    };
    // Gather dirty texel positions on the host (bilinear: cheap), classify
    // on the device in ~1M-texel launches, scatter back per ptex face.
    struct _Span {
        size_t id;
        size_t begin;
        size_t count;
    };
    std::vector<_Span> spans;
    std::vector<float> positions;
    for (size_t id = 0; id < plan.faces.size(); ++id) {
        _BakeFace const &bf = plan.faces[id];
        if (!isDirty[size_t(bf.coarse)]) {
            continue;
        }
        _Span span{id, positions.size() / 3, 0};
        for (int tv = 0; tv < bf.resV; ++tv) {
            for (int tu = 0; tu < bf.resU; ++tu) {
                float p[3];
                _TexelPosition(bf, tu, tv, p);
                positions.push_back(p[0]);
                positions.push_back(p[1]);
                positions.push_back(p[2]);
                ++span.count;
            }
        }
        spans.push_back(span);
    }
    if (spans.empty()) {
        return true;
    }
    int const regionCount = int(loops.loopCount.size());
    if (regionCount == 0) {
        // No regions: every dirty texel is -1, no kernel needed.
        for (auto const &span : spans) {
            _BakeFace const &bf = plan.faces[span.id];
            std::vector<float> texels(size_t(bf.resU) * size_t(bf.resV) *
                                      size_t(plan.channels));
            for (size_t t = 0; t < size_t(bf.resU) * size_t(bf.resV); ++t) {
                texels[t * size_t(plan.channels)] = -1.0f;
            }
            // No regions means no L1 tube either, so every level channel
            // stays 0; nothing to classify.
            (*texelCache)[span.id] = std::move(texels);
            if (stats) {
                ++stats->facesClassified;
                stats->texelsClassified +=
                    size_t(bf.resU) * size_t(bf.resV);
            }
        }
        return true;
    }
    usdGen::gpu::DeviceBuffer<float> dPos, dLoop, dN, dP, dU, dV;
    usdGen::gpu::DeviceBuffer<int> dBegin, dCount, dInterp, dOut;
    size_t const nPoints = positions.size() / 3;
    if (dPos.reset(positions.size()) != cudaSuccess ||
        dLoop.reset(loops.points.size()) != cudaSuccess ||
        dBegin.reset(loops.loopBegin.size()) != cudaSuccess ||
        dCount.reset(loops.loopCount.size()) != cudaSuccess ||
        dN.reset(loops.planeN.size()) != cudaSuccess ||
        dP.reset(loops.planeP.size()) != cudaSuccess ||
        dU.reset(loops.basisU.size()) != cudaSuccess ||
        dV.reset(loops.basisV.size()) != cudaSuccess ||
        dInterp.reset(loops.interpIds.size()) != cudaSuccess ||
        dOut.reset(nPoints) != cudaSuccess) {
        return fail("TonicBakePtex: device allocation failed");
    }
    auto upload = [&](auto &buf, auto const &host) {
        return cudaMemcpyAsync(buf.data(), host.data(),
                               host.size() * sizeof(host[0]),
                               cudaMemcpyHostToDevice, stream) == cudaSuccess;
    };
    if (!upload(dPos, positions) || !upload(dLoop, loops.points) ||
        !upload(dBegin, loops.loopBegin) || !upload(dCount, loops.loopCount) ||
        !upload(dN, loops.planeN) || !upload(dP, loops.planeP) ||
        !upload(dU, loops.basisU) || !upload(dV, loops.basisV) ||
        !upload(dInterp, loops.interpIds)) {
        return fail("TonicBakePtex: device upload failed");
    }
    char launchErr[256] = {0};
    if (!TonicLaunchClassifyPoints(dPos.data(), int(nPoints), dLoop.data(),
                                   dBegin.data(), dCount.data(), dN.data(),
                                   dP.data(), dU.data(), dV.data(),
                                   dInterp.data(), regionCount, dOut.data(),
                                   stream, launchErr, sizeof(launchErr))) {
        if (err) {
            *err = std::string("TonicBakePtex: classify failed: ") + launchErr;
        }
        return false;
    }
    // D2H on the BAKE stream into pinned memory, with one event the worker
    // waits on (§3.1a). Nothing here touches the default stream, so a bake
    // in flight cannot serialise against an interaction launch, and the UI
    // thread never synchronises with either stream.
    _PinnedInts ids;
    if (!ids.reset(nPoints)) {
        return fail("TonicBakePtex: pinned host allocation failed");
    }
    if (cudaMemcpyAsync(ids.data, dOut.data(), nPoints * sizeof(int),
                        cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        return fail("TonicBakePtex: device download failed");
    }
    cudaEvent_t done = nullptr;
    if (cudaEventCreate(&done) != cudaSuccess) {
        return fail("TonicBakePtex: event creation failed");
    }
    cudaEventRecord(done, stream);
    cudaError_t const wait = cudaEventSynchronize(done);
    cudaEventDestroy(done);
    if (wait != cudaSuccess) {
        return fail("TonicBakePtex: classify wait failed");
    }
    for (auto const &span : spans) {
        _BakeFace const &bf = plan.faces[span.id];
        std::vector<float> texels(size_t(bf.resU) * size_t(bf.resV) *
                                  size_t(plan.channels), 0.0f);
        for (size_t t = 0; t < span.count; ++t) {
            int const region = ids.data[span.begin + t];
            float *texel = &texels[t * size_t(plan.channels)];
            texel[0] = float(region);
            if (plan.channels > 1) {
                float p[3];
                _TexelPosition(bf, int(t % size_t(bf.resU)),
                               int(t / size_t(bf.resU)), p);
                _ClassifyLevels(levels, region, p, plan.channels, texel);
            }
        }
        (*texelCache)[span.id] = std::move(texels);
        if (stats) {
            ++stats->facesClassified;
            stats->texelsClassified += span.count;
        }
    }
    return true;
}

#endif  // USDGEN_TONIC_HAS_CUDA

}  // namespace

void TonicCollectBakeTubes(TonicModel const &model,
                           std::vector<TonicBakeTube> *out)
{
    if (!out) {
        return;
    }
    out->clear();
    for (int id : model.TubeIds()) {
        if (model.IsTubeImported(id)) {
            continue;  // a bridge import is not a subdivision cell
        }
        TonicTubeDesc desc;
        if (!model.GetTubeDesc(id, &desc) || desc.centerX.size() < 2) {
            continue;
        }
        std::vector<TonicFrame> frames;
        std::string err;
        if (!TonicCenterFramesCpu(desc.centerX.data(), desc.centerY.data(),
                                  desc.centerZ.data(),
                                  int(desc.centerX.size()), &frames, &err) ||
            frames.empty()) {
            continue;
        }
        TonicBakeTube tube;
        tube.tubeId = id;
        tube.parentTubeId = desc.parentTubeId;
        tube.level = desc.level;
        tube.regionId = desc.regionId;
        tube.childIndex = desc.childIndex;
        tube.rootCenter[0] = desc.centerX[0];
        tube.rootCenter[1] = desc.centerY[0];
        tube.rootCenter[2] = desc.centerZ[0];
        tube.rootFrame = frames[0];
        out->push_back(tube);
    }
}

std::string TonicBakeFileName(std::string const &baseName, uint64_t mapVersion)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), ".v%llu.ptx",
                  (unsigned long long)mapVersion);
    return baseName + buf;
}

bool TonicBakePtex(TonicBakeInput const &input, std::string const &outPath,
                   std::vector<int> const *dirtyFaces,
                   std::vector<std::vector<float>> *texelCache,
                   TonicBakeStats *stats, std::string *err)
{
    auto fail = [&](std::string const &what) {
        if (err) {
            *err = "TonicBakePtex: " + what;
        }
        return false;
    };
    if (!input.scalp || !input.scalp->finalized || !texelCache) {
        return fail("bad scalp or null texel cache");
    }
    TonicScalpMesh const &mesh = *input.scalp;
    size_t const faceCount = mesh.faceVertexCounts.size();
    TonicRegionLoops loops;
    if (!TonicFlattenLoops(input.graph, &loops, err)) {
        return false;
    }
    TonicRegionMaps maps;
    if (!TonicRasteriseRegionsCpu(mesh, input.graph, &maps, err)) {
        return false;
    }
    std::vector<int> const resLog2 =
        TonicFaceResLog2(mesh, maps, loops, input.resOverride);
    _BakePlan plan;
    if (!_PlanFaces(input, maps, resLog2, &plan, err)) {
        return false;
    }
    if (texelCache->size() != plan.faces.size()) {
        texelCache->assign(plan.faces.size(), {});
    }
    std::vector<char> isDirty(faceCount, dirtyFaces ? 0 : 1);
    if (dirtyFaces) {
        for (int f : *dirtyFaces) {
            if (f >= 0 && size_t(f) < faceCount) {
                isDirty[size_t(f)] = 1;
            }
        }
    }
    _HealStaleCache(plan, *texelCache, &isDirty);
    if (stats) {
        *stats = TonicBakeStats();
        stats->ptexFaces = plan.faces.size();
        stats->channels = plan.channels;
    }
    // The synchronous entry classifies on the CPU (deterministic, no stream
    // plumbing); the worker's chunked path takes the GPU lane when one
    // exists. Both call the same inside-test and the same level walk.
    _LevelIndex const levels = _BuildLevelIndex(input, loops);
    _ClassifyDirtyCpu(loops, plan, levels, isDirty, texelCache, stats);
    return _WritePtexFile(plan, *texelCache, outPath, err);
}

TonicBakeWorker::TonicBakeWorker()
    : _worker(&TonicBakeWorker::_WorkerLoop, this)
{
}

TonicBakeWorker::~TonicBakeWorker()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop = true;
    }
    _wake.notify_all();
    if (_worker.joinable()) {
        _worker.join();
    }
}

void TonicBakeWorker::Enqueue(uint64_t mapVersion, TonicBakeInput input)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _pendingVersion = mapVersion;
        _pendingInput = std::move(input);
        _hasPending = true;
        _pendingVersionAtomic.store(mapVersion);
    }
    _wake.notify_one();
}

bool TonicBakeWorker::TakeCompleted(uint64_t *mapVersion, std::string *path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_hasCompleted) {
        return false;
    }
    if (mapVersion) {
        *mapVersion = _completedVersion;
    }
    if (path) {
        *path = _completedPath;
    }
    _hasCompleted = false;
    return true;
}

void TonicBakeWorker::NoteSwapped(uint64_t mapVersion, std::string const &path)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _swappedVersion = mapVersion;
        _swappedPath = path;
        // The wake predicate needs its own reason: with no pending bake the
        // worker would go straight back to sleep and the file the previous
        // swap pinned would survive forever.
        _sweepRequested = true;
    }
    _wake.notify_one();
}

void TonicBakeWorker::PauseWorker(bool pause)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _paused = pause;
    }
    _wake.notify_all();
}

bool TonicBakeWorker::WaitCompleted(uint64_t mapVersion, int timeoutMs)
{
    auto const deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (_completedVersionAtomic.load() >= mapVersion) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::string TonicBakeWorker::TakeDiagnostic()
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::string out = _diagnostic;
    _diagnostic.clear();
    return out;
}

void TonicBakeWorker::_WorkerLoop()
{
#ifdef USDGEN_TONIC_HAS_CUDA
    cudaStream_t bakeStream = _BakeStream();  // lowest priority, may be null
#else
    void *bakeStream = nullptr;
#endif
    for (;;) {
        uint64_t version = 0;
        TonicBakeInput input;
        bool cleanup = false;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [&] {
                return _stop || (!_paused && !_baking &&
                                 (_hasPending || _sweepRequested));
            });
            if (_stop) {
                break;
            }
            if (!_hasPending) {
                // A swap freed an older versioned file and no bake follows:
                // sweep and go back to sleep (plan/18 §7 G5).
                _sweepRequested = false;
                std::string const dir = _lastOutDir;
                std::string const base = _lastBaseName;
                lock.unlock();
                if (!dir.empty()) {
                    _SweepStale(dir, base);
                }
                continue;
            }
            // Coalescing: take the latest enqueue, whatever woke us.
            version = _pendingVersion;
            input = std::move(_pendingInput);
            _hasPending = false;
            _sweepRequested = false;
            _baking = true;
            _lastOutDir = input.outDir;
            _lastBaseName = input.baseName;
        }
        std::string outPath;
        std::string err;
        bool ok = false;
        try {
            if (int const pending = _throwBakes.load(); pending > 0) {
                _throwBakes.store(pending - 1);
                throw std::runtime_error(
                    "TonicBakeWorker: injected bake failure (test hook)");
            }
            ok = _Bake(version, input, &outPath, &err,
#ifdef USDGEN_TONIC_HAS_CUDA
                       bakeStream
#else
                       nullptr
#endif
            );
        } catch (std::exception const &e) {
            // §3.4: the map keeps its previous file, the model is untouched
            // and this thread survives to serve the next enqueue.
            _workerThrowCount.fetch_add(1);
            ok = false;
            err = std::string("TonicBakeWorker: worker threw: ") + e.what();
        } catch (...) {
            _workerThrowCount.fetch_add(1);
            ok = false;
            err = "TonicBakeWorker: worker threw an unknown exception";
        }
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _baking = false;
            if (!ok) {
                if (err.empty()) {
                    // Cancelled at a tile boundary: no file, no completion.
                    _cancelCount.fetch_add(1);
                } else {
                    _diagnostic = err;
                }
                _wake.notify_one();  // a newer version may be waiting
                continue;
            }
            _bakeCount.fetch_add(1);
            _completedVersion = version;
            _completedPath = outPath;
            _hasCompleted = true;
            _completedVersionAtomic.store(version);
            cleanup = true;
        }
        if (cleanup) {
            _SweepStale(input.outDir, input.baseName);
        }
    }
#ifdef USDGEN_TONIC_HAS_CUDA
    if (bakeStream) {
        cudaStreamDestroy(bakeStream);
    }
#else
    (void)bakeStream;
#endif
}

void TonicBakeWorker::_SweepStale(std::string const &outDir,
                                  std::string const &baseName)
{
    // Keep the file the live layer references and the latest completion;
    // delete older versioned files. Runs on the worker only.
    std::error_code ec;
    if (outDir.empty() || !fs::is_directory(outDir, ec)) {
        return;
    }
    std::string const prefix = baseName + ".v";
    for (auto const &entry : fs::directory_iterator(outDir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string const name = entry.path().filename().string();
        if (name.size() <= prefix.size() + 4 ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - 4, 4, ".ptx") != 0) {
            continue;
        }
        std::string const full = entry.path().string();
        std::lock_guard<std::mutex> lock(_mutex);
        if (full == _swappedPath || full == _completedPath) {
            continue;
        }
        unsigned long long v = 0;
        if (std::sscanf(name.c_str() + prefix.size(), "%llu", &v) == 1 &&
            v < _completedVersionAtomic.load()) {
            fs::remove(entry.path(), ec);
        }
    }
    _sweepCount.fetch_add(1);
}

void TonicBakeWorker::ThrowOnNextBakesForTest(int count)
{
    _throwBakes.store(count < 0 ? 0 : count);
}

bool TonicBakeWorker::_Bake(uint64_t mapVersion, TonicBakeInput const &input,
                            std::string *outPath, std::string *err,
                            void *bakeStream)
{
    auto fail = [&](std::string const &what) {
        if (err) {
            *err = "TonicBakeWorker: " + what;
        }
        return false;
    };
    // Cancelled bakes report success=false with an EMPTY error (the loop
    // counts them as cancellations, not failures).
    auto cancelled = [&] {
        std::lock_guard<std::mutex> lock(_mutex);
        return _stop || (_hasPending && _pendingVersion > mapVersion);
    };
    if (!input.scalp || !input.scalp->finalized) {
        return fail("bad scalp input");
    }
    if (input.outDir.empty()) {
        return fail("no output directory");
    }
    std::error_code ec;
    fs::create_directories(input.outDir, ec);
    if (ec) {
        return fail("cannot create " + input.outDir);
    }
    TonicScalpMesh const &mesh = *input.scalp;
    size_t const faceCount = mesh.faceVertexCounts.size();
    TonicRegionLoops loops;
    if (!TonicFlattenLoops(input.graph, &loops, err)) {
        return false;
    }
    TonicRegionMaps maps;
    if (!TonicRasteriseRegionsCpu(mesh, input.graph, &maps, err)) {
        return false;
    }
    // Incremental dirty set: faces whose interp id changed since the last
    // finished bake. A new output dir, face count, level count or res
    // override invalidates the whole cache.
    std::vector<int> const resLog2 =
        TonicFaceResLog2(mesh, maps, loops, input.resOverride);
    _BakePlan plan;
    if (!_PlanFaces(input, maps, resLog2, &plan, err)) {
        return false;
    }
    bool full = _cachedFaceRegion.size() != faceCount ||
                _cacheDir != input.outDir ||
                _texelCache.size() != plan.faces.size();
    std::vector<char> isDirty(faceCount, full ? 1 : 0);
    if (!full) {
        for (size_t f = 0; f < faceCount; ++f) {
            if (_cachedFaceRegion[f] != maps.faceRegion[f]) {
                isDirty[f] = 1;
            }
        }
    } else if (_texelCache.size() != plan.faces.size()) {
        _texelCache.assign(plan.faces.size(), {});
    }
    _HealStaleCache(plan, _texelCache, &isDirty);
    if (cancelled()) {
        if (err) {
            err->clear();
        }
        return false;
    }
    // Chunked classification at ~4K-face tile boundaries with a supersede
    // check between chunks (plan/17 §3.1a cancellation).
    _LevelIndex const levels = _BuildLevelIndex(input, loops);
    size_t const kChunkFaces = 4096;
    TonicBakeStats stats;
    stats.ptexFaces = plan.faces.size();
    stats.channels = plan.channels;
    for (size_t begin = 0; begin < faceCount; begin += kChunkFaces) {
        size_t const end = std::min(begin + kChunkFaces, faceCount);
        std::vector<char> chunkDirty(faceCount, 0);
        bool anyDirty = false;
        for (size_t f = begin; f < end; ++f) {
            if (isDirty[f]) {
                chunkDirty[f] = 1;
                anyDirty = true;
            }
        }
        if (anyDirty) {
#ifdef USDGEN_TONIC_HAS_CUDA
            // The GPU lane classifies this chunk on the bake stream; any
            // failure falls back to the CPU twin for the chunk (never a
            // failed bake: §3.4 owns the same fallback for interaction).
            std::string gpuErr;
            if (bakeStream == nullptr ||
                !_ClassifyDirtyGpu(loops, plan, levels, chunkDirty,
                                   &_texelCache, (cudaStream_t)bakeStream,
                                   &stats, &gpuErr)) {
                _ClassifyDirtyCpu(loops, plan, levels, chunkDirty,
                                  &_texelCache, &stats);
            }
#else
            (void)bakeStream;
            _ClassifyDirtyCpu(loops, plan, levels, chunkDirty, &_texelCache,
                              &stats);
#endif
        }
        if (cancelled()) {
            if (err) {
                err->clear();
            }
            return false;
        }
    }
    std::string const finalPath =
        (fs::path(input.outDir) / TonicBakeFileName(input.baseName, mapVersion))
            .string();
    if (!_WritePtexFile(plan, _texelCache, finalPath, err)) {
        return false;
    }
    if (cancelled()) {
        // A newer version arrived during the file write: the versioned file
        // is complete and harmless on disk, but it is NOT reported, so the
        // UI never swaps a superseded bake (a stale file is never referenced).
        fs::remove(finalPath, ec);
        if (err) {
            err->clear();
        }
        return false;
    }
    _cachedFaceRegion = maps.faceRegion;
    _cacheDir = input.outDir;
    _lastBakedFaces.store(stats.facesClassified);
    if (outPath) {
        *outPath = finalPath;
    }
    return true;
}

}  // namespace usdGenTonic
