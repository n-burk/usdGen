// usdGenTonic — TonicModel implementation (plan/17 §2.1): the tube, the
// scalp graph, the hierarchy store, the guide fill, picking and undo.
#include "usdGenTonic/tonicModel.h"

#include "usdGenTonic/tonicCheck.h"

#ifdef USDGEN_TONIC_HAS_CUDA
#include "usdGenTonic/tonicKernels.h"
#include "usdGen/gpu/deviceBuffer.h"
#include <cuda_runtime.h>
#endif

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace usdGenTonic {

struct TonicModel::_Device {
#ifdef USDGEN_TONIC_HAS_CUDA
    usdGen::gpu::DeviceBuffer<float> positions;  // 3 * vertexCount
    usdGen::gpu::DeviceBuffer<float> normals;    // 3 * vertexCount
    usdGen::gpu::DeviceBuffer<float> centerX;
    usdGen::gpu::DeviceBuffer<float> centerY;
    usdGen::gpu::DeviceBuffer<float> centerZ;
    // P3 K4/K5 lane (sized on the sections path only). Frames ride a
    // float buffer (9 per frame): DeviceBuffer instantiates a fixed type
    // list in usdGenGpu, which cannot see TonicFrame, so the launch
    // reinterprets the data pointer (see the static_assert below).
    usdGen::gpu::DeviceBuffer<float> frames;
    usdGen::gpu::DeviceBuffer<float> sectionT;
    usdGen::gpu::DeviceBuffer<float> sectionU;  // nSec * ringVerts
    usdGen::gpu::DeviceBuffer<float> sectionV;
    usdGen::gpu::DeviceBuffer<float> sectionScale;
    usdGen::gpu::DeviceBuffer<float> sectionTwist;
    usdGen::gpu::DeviceBuffer<float> ringT;
    // K11 production pick caches (TN-2): guide CVs uploaded once per
    // guide generation, reduction scratch sized per pick (same-size
    // reset is a no-op, so steady-state picks allocate nothing). Best/
    // scratch ride float buffers (4 per TonicDevicePickBest: the fixed
    // DeviceBuffer type list cannot see that struct either).
    usdGen::gpu::DeviceBuffer<float> guideCVs;  // 3 * guides * cv
    uint64_t guideCVsVersion = 0;               // 0 = nothing uploaded
    usdGen::gpu::DeviceBuffer<float> pickBest;
    usdGen::gpu::DeviceBuffer<float> pickScratch;
    // K11b marquee lane (V1): one byte per candidate plus the lasso
    // polygon. Grow-only like the pick scratch, so a drag that marquees
    // on every sample allocates once.
    usdGen::gpu::DeviceBuffer<unsigned char> selectMask;
    usdGen::gpu::DeviceBuffer<float> selectPolygon;
    cudaStream_t stream = nullptr;
    bool streamOwned = false;
#endif
};

TonicModel::TonicModel() : _device(new _Device())
{
#ifdef USDGEN_TONIC_HAS_CUDA
    if (cudaStreamCreate(&_device->stream) == cudaSuccess) {
        _device->streamOwned = true;
    }
#endif
}

TonicModel::~TonicModel()
{
#ifdef USDGEN_TONIC_HAS_CUDA
    if (_device && _device->streamOwned && _device->stream) {
        cudaStreamDestroy(_device->stream);
        _device->stream = nullptr;
    }
#endif
}

namespace {

// Production lane choice (plan/17 section 4.1, plan/18 section 7 G11): the
// device lane when the CUDA mirror is healthy, the CPU twin otherwise and
// whenever the lane refuses (a tube over the device caps, an allocation
// failure). Only lanes proven BIT-exact against their twin are dispatched
// this way, so the answer never depends on which one ran; K8/K9/K10 agree
// only to a tolerance and stay on the twin (see tonicKernels.h). The lanes
// take the legacy default stream, which a cudaStreamCreate stream
// synchronises with, so they stay ordered behind the model's K4/K5 work.
bool _LaneParentAverage(bool device,
                        std::vector<TonicTubeDesc> const &children,
                        TonicTubeDesc *out, std::string *err)
{
#ifdef USDGEN_TONIC_HAS_CUDA
    if (device) {
        std::vector<TonicTubeDesc> one;
        std::string lerr;
        if (TonicParentAverageDevice(&children, 1, &one, nullptr, &lerr) &&
            one.size() == 1) {
            *out = one[0];
            return true;
        }
    }
#else
    (void)device;
#endif
    return TonicParentAverageCpu(children, out, err);
}

bool _LaneSmoothnessScore(bool device, float const *cx, float const *cy,
                          float const *cz, int nCv, float *out,
                          std::string *err)
{
#ifdef USDGEN_TONIC_HAS_CUDA
    if (device) {
        std::string lerr;
        if (TonicSmoothnessScoreDevice(cx, cy, cz, nCv, out, nullptr,
                                       &lerr)) {
            return true;
        }
    }
#else
    (void)device;
#endif
    return TonicSmoothnessScoreCpu(cx, cy, cz, nCv, out, err);
}

bool _LaneTubeIntersect(bool device, TonicTubeDesc const *tubes,
                        int tubeCount, int *outFlags, std::string *err)
{
#ifdef USDGEN_TONIC_HAS_CUDA
    if (device) {
        std::string lerr;
        if (TonicTubeIntersectDevice(tubes, tubeCount, outFlags, nullptr,
                                     &lerr)) {
            return true;
        }
    }
#else
    (void)device;
#endif
    return TonicTubeIntersectCpu(tubes, tubeCount, outFlags, err);
}

}  // namespace

bool
TonicModel::BuildTestTube(TonicTubeShape shape)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (shape.rings < 2 || shape.ringVerts < 3 ||
        shape.radius <= 0.0f || shape.length <= 0.0f) {
        _diagnostic = "TonicModel::BuildTestTube: shape out of range "
                      "(rings >= 2, ringVerts >= 3, radius/length > 0)";
        return false;
    }
    _ClearUndoLocked();  // rebuild invalidates pre-mutation snapshots
    _shape = shape;
    _host.centerX.assign(size_t(shape.rings), 0.0f);
    _host.centerY.resize(size_t(shape.rings));
    _host.centerZ.assign(size_t(shape.rings), 0.0f);
    for (int r = 0; r < shape.rings; ++r) {
        _host.centerY[size_t(r)] =
            shape.length * float(r) / float(shape.rings - 1);
    }
    // P3 rings start as default circles on the legacy display path; the
    // first section op switches to K5. Guides start empty (RefillGuides).
    {
        TubeSnapshot seed;
        seed.shape = shape;
        seed.centerX = _host.centerX;
        seed.centerY = _host.centerY;
        seed.centerZ = _host.centerZ;
        _sections = TonicDefaultSections(seed);
    }
    _useSections = false;
    _tubeRegionId = -1;
    _roots.clear();
    _guides = TonicGuideSet();
    ++_guideVersion;
    if (!Sync()) {
        return false;
    }
    // Topology always changes on a rebuild (counts/faces are re-emitted).
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::MoveCenterRing(int ring, float dx, float dz)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ring < 0 || ring >= _shape.rings ||
        _host.centerX.size() != size_t(_shape.rings)) {
        _diagnostic = "TonicModel::MoveCenterRing: ring index out of range";
        return false;
    }
    _PushUndoLocked();
    _host.centerX[size_t(ring)] += dx;
    _host.centerZ[size_t(ring)] += dz;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::Sync()
{
    if (!_TessellateHost()) {
        return false;
    }
    if (!_SyncDevice()) {
        return false;
    }
    return true;
}

bool
TonicModel::HasCudaMirror() const
{
#ifdef USDGEN_TONIC_HAS_CUDA
    return _device && _device->streamOwned;
#else
    return false;
#endif
}

bool
TonicModel::DeviceFallback() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _deviceFallback;
}

char const *
TonicModel::DeviceFallbackReason() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _deviceFallbackReason.c_str();
}

#ifdef USDGEN_TONIC_HAS_CUDA
void
TonicModel::_DropDeviceLocked(char const *reason)
{
    if (_deviceFallback) {
        return;
    }
    _deviceFallback = true;
    _deviceFallbackReason = reason ? reason : "device mirror dropped";
    if (!_device) {
        return;
    }
    // Release every buffer first so the OOM that brought us here is
    // actually relieved, then destroy the stream.
    _device->positions.release();
    _device->normals.release();
    _device->centerX.release();
    _device->centerY.release();
    _device->centerZ.release();
    _device->frames.release();
    _device->sectionT.release();
    _device->sectionU.release();
    _device->sectionV.release();
    _device->sectionScale.release();
    _device->sectionTwist.release();
    _device->ringT.release();
    _device->guideCVs.release();
    _device->guideCVsVersion = 0;
    _device->pickBest.release();
    _device->pickScratch.release();
    if (_device->streamOwned && _device->stream) {
        cudaStreamDestroy(_device->stream);
    }
    _device->stream = nullptr;
    _device->streamOwned = false;
}

// Fault injection for the P6 T0 (and only the T0): a non-empty
// USDGEN_TONIC_FORCE_DEVICE_FALLBACK makes the next device sync drop
// the mirror deterministically, proving the fallback path runs.
static bool
DeviceFallbackForced()
{
    char const *forced = std::getenv("USDGEN_TONIC_FORCE_DEVICE_FALLBACK");
    return forced && *forced;
}
#endif

bool
TonicModel::CopyDeviceToHost(float *posOut, float *nrmOut,
                             size_t floatCount) const
{
#ifdef USDGEN_TONIC_HAS_CUDA
    if (!posOut || !nrmOut || !_device || !_device->streamOwned ||
        !_device->stream) {
        return false;
    }
    if (_device->positions.size() != floatCount ||
        _device->normals.size() != floatCount) {
        return false;
    }
    // The tessellate kernel was enqueued on _device->stream by Sync; wait for
    // it before the D2H so the staged snapshot is exactly the last Sync.
    if (cudaStreamSynchronize(_device->stream) != cudaSuccess) {
        return false;
    }
    if (cudaMemcpy(posOut, _device->positions.data(),
                   floatCount * sizeof(float),
                   cudaMemcpyDeviceToHost) != cudaSuccess) {
        return false;
    }
    if (cudaMemcpy(nrmOut, _device->normals.data(),
                   floatCount * sizeof(float),
                   cudaMemcpyDeviceToHost) != cudaSuccess) {
        return false;
    }
    return true;
#else
    (void)posOut;
    (void)nrmOut;
    (void)floatCount;
    return false;
#endif
}

uint32_t
TonicModel::TakeDirty()
{
    std::lock_guard<std::mutex> lock(_mutex);
    uint32_t dirty = _dirty;
    _dirty = TonicDirty_Clean;
    return dirty;
}

bool
TonicModel::SetFillParams(FillParams params)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!(params.density >= 0.0f) || params.cvCount < 2 ||
        params.cvCount > 64) {
        _diagnostic = "TonicModel::SetFillParams: density must be finite "
                      "and >= 0, cvCount in [2, 64]";
        return false;
    }
    if (params.edgeBias < -1.0f || params.edgeBias > 1.0f) {
        _diagnostic = "TonicModel::SetFillParams: edgeBias in [-1, 1]";
        return false;
    }
    if (params.lengthProfile.size() % 2 != 0) {
        _diagnostic = "TonicModel::SetFillParams: lengthProfile holds "
                      "(position, value) pairs";
        return false;
    }
    _fill = std::move(params);
    ++_version;
    return true;
}

TonicModel::FillParams
TonicModel::GetFillParams() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _fill;
}

bool
TonicModel::SetSubdivideParams(SubdivideParams params)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (params.count < 2 || params.count > 8 ||
        (params.splitMode != "kmeans" && params.splitMode != "edge")) {
        _diagnostic = "TonicModel::SetSubdivideParams: count in [2, 8], "
                      "splitMode kmeans | edge";
        return false;
    }
    _subdivide = std::move(params);
    ++_version;
    return true;
}

TonicModel::SubdivideParams
TonicModel::GetSubdivideParams() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _subdivide;
}

void
TonicModel::SetLockFlags(bool locked, bool lockParents, bool lockChildren)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _locked = locked;
    _lockParents = lockParents;
    _lockChildren = lockChildren;
    ++_version;
}

void
TonicModel::GetLockFlags(bool *locked, bool *lockParents,
                         bool *lockChildren) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (locked) {
        *locked = _locked;
    }
    if (lockParents) {
        *lockParents = _lockParents;
    }
    if (lockChildren) {
        *lockChildren = _lockChildren;
    }
}

TonicModel::TubeSnapshot
TonicModel::Snapshot() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    TubeSnapshot snapshot;
    snapshot.shape = _shape;
    snapshot.centerX = _host.centerX;
    snapshot.centerY = _host.centerY;
    snapshot.centerZ = _host.centerZ;
    snapshot.sections = _sections;
    snapshot.fill = _fill;
    snapshot.subdivide = _subdivide;
    snapshot.locked = _locked;
    snapshot.lockParents = _lockParents;
    snapshot.lockChildren = _lockChildren;
    snapshot.version = _version.load();
    snapshot.hasTube =
        !_host.positions.empty() &&
        _host.centerX.size() == size_t(_shape.rings);
    return snapshot;
}

bool
TonicModel::Restore(TubeSnapshot const &snapshot)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!snapshot.hasTube || snapshot.shape.rings < 2 ||
        snapshot.shape.ringVerts < 3 || snapshot.shape.radius <= 0.0f ||
        snapshot.shape.length <= 0.0f ||
        snapshot.centerX.size() != size_t(snapshot.shape.rings) ||
        snapshot.centerY.size() != size_t(snapshot.shape.rings) ||
        snapshot.centerZ.size() != size_t(snapshot.shape.rings)) {
        _diagnostic = "TonicModel::Restore: snapshot holds no valid tube";
        return false;
    }
    _shape = snapshot.shape;
    _host.centerX = snapshot.centerX;
    _host.centerY = snapshot.centerY;
    _host.centerZ = snapshot.centerZ;
    // Committed sections are authoritative (hydrate installs them); an
    // empty ring list means a pre-P3 snapshot and rebuilds the default
    // circles on the legacy display path.
    if (snapshot.sections.empty()) {
        TubeSnapshot seed = snapshot;
        _sections = TonicDefaultSections(seed);
        _useSections = false;
    } else {
        if (snapshot.sections.size() < 2) {
            _diagnostic = "TonicModel::Restore: need >= 2 sections";
            return false;
        }
        for (auto const &s : snapshot.sections) {
            if (int(s.u.size()) != snapshot.shape.ringVerts ||
                int(s.v.size()) != snapshot.shape.ringVerts) {
                _diagnostic = "TonicModel::Restore: section ring mismatch";
                return false;
            }
        }
        _sections = snapshot.sections;
        _useSections = true;
    }
    _roots.clear();
    _guides = TonicGuideSet();
    ++_guideVersion;
    _fill = snapshot.fill;
    _subdivide = snapshot.subdivide;
    _locked = snapshot.locked;
    _lockParents = snapshot.lockParents;
    _lockChildren = snapshot.lockChildren;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    ++_version;
    return true;
}

int
TonicGuideCountForDensity(float density)
{
    if (!(density > 0.0f)) {
        return 0;
    }
    // The no-scalp count rule (root area pinned to 1); the scalp-bound
    // form (K8 per-face, density * area) lives in TonicRootSampleMeshCpu.
    return int(density + 0.5f);
}

std::vector<TonicTubeSection>
TonicDefaultSections(TonicModel::TubeSnapshot const &snapshot)
{
    std::vector<TonicTubeSection> sections;
    int const rings = int(snapshot.centerX.size()) > 0
                          ? int(snapshot.centerX.size())
                          : snapshot.shape.rings;
    int const ringVerts = snapshot.shape.ringVerts;
    if (rings < 2 || ringVerts < 3) {
        return sections;
    }
    float const twoPi = 6.28318530717958647692f;
    sections.resize(size_t(rings));
    for (int r = 0; r < rings; ++r) {
        TonicTubeSection section;
        section.t = float(r) / float(rings - 1);
        section.u.resize(size_t(ringVerts));
        section.v.resize(size_t(ringVerts));
        for (int s = 0; s < ringVerts; ++s) {
            float const a = twoPi * float(s) / float(ringVerts);
            section.u[size_t(s)] = snapshot.shape.radius * std::cos(a);
            section.v[size_t(s)] = snapshot.shape.radius * std::sin(a);
        }
        sections[size_t(r)] = std::move(section);
    }
    return sections;
}

TonicTubeDesc
TonicTubeDescFromSnapshot(TonicModel::TubeSnapshot const &snapshot)
{
    TonicTubeDesc desc;
    desc.centerX = snapshot.centerX;
    desc.centerY = snapshot.centerY;
    desc.centerZ = snapshot.centerZ;
    desc.ringVerts = snapshot.shape.ringVerts;
    bool sectionsOk = snapshot.sections.size() >= 2;
    for (auto const &s : snapshot.sections) {
        if (int(s.u.size()) != desc.ringVerts ||
            int(s.v.size()) != desc.ringVerts) {
            sectionsOk = false;
            break;
        }
    }
    desc.sections =
        sectionsOk ? snapshot.sections : TonicDefaultSections(snapshot);
    return desc;
}

namespace {

// Shared K9/K10 tail: fill `roots` through the tube and resample to the
// fill CV count with root frames. Returns false on any kernel error.
bool _FillAndResample(TonicTubeDesc const &tube,
                      std::vector<TonicFrame> const &frames,
                      std::vector<TonicGuideRoot> const &roots,
                      TonicModel::FillParams const &fill, TonicGuideSet *guides)
{
    TonicFillDesc fd;
    fd.density = fill.density;
    fd.cvCount = fill.cvCount;
    fd.seed = fill.seed;
    fd.edgeBias = fill.edgeBias;
    fd.lengthProfile = fill.lengthProfile;
    std::vector<float> filled;
    std::vector<float> lengths;
    std::string err;
    if (!TonicGuideFillCpu(tube, frames, roots, fd, &filled, &lengths,
                           &err)) {
        return false;
    }
    int const guideCount = int(roots.size());
    std::vector<int> inCounts(size_t(guideCount), fill.cvCount);
    float rootDirs[9] = {frames[0].nx, frames[0].ny, frames[0].nz,
                         frames[0].bx, frames[0].by, frames[0].bz,
                         frames[0].tx, frames[0].ty, frames[0].tz};
    std::vector<float> resampled;
    std::vector<int> outCounts;
    std::vector<double> outFrames;
    if (!TonicGuideResampleCpu(filled.data(), inCounts.data(), guideCount,
                               fill.cvCount, rootDirs, &resampled, &outCounts,
                               &outFrames, &err)) {
        return false;
    }
    guides->points = std::move(resampled);
    guides->counts = std::move(outCounts);
    guides->frames = std::move(outFrames);
    guides->ids.resize(size_t(guideCount));
    for (int g = 0; g < guideCount; ++g) {
        guides->ids[size_t(g)] = uint64_t(1000 + g);
    }
    guides->guideCount = guideCount;
    guides->cvCount = fill.cvCount;
    return true;
}

} // namespace

TonicGuideSet
TonicGenerateGuides(TonicModel::TubeSnapshot const &snapshot)
{
    return TonicGenerateGuidesForTube(snapshot, 0);
}

TonicGuideSet
TonicGenerateGuidesOnScalp(TonicModel::TubeSnapshot const &snapshot,
                           TonicScalpMesh const &scalp, int const *regionFaces,
                           int regionFaceCount)
{
    return TonicGenerateGuidesOnScalpForTube(snapshot, scalp, regionFaces,
                                             regionFaceCount, 0);
}

TonicModel::TubeSnapshot
TonicSnapshotFromTubeRecord(TonicModel::TubeRecord const &record)
{
    TonicModel::TubeSnapshot snapshot;
    if (!record.hasTube || record.actual.centerX.size() < 2) {
        return snapshot;
    }
    TonicTubeDesc const &desc = record.actual;
    snapshot.hasTube = true;
    snapshot.shape.rings = int(desc.centerX.size());
    snapshot.shape.ringVerts = desc.ringVerts;
    snapshot.centerX = desc.centerX;
    snapshot.centerY = desc.centerY;
    snapshot.centerZ = desc.centerZ;
    snapshot.sections = desc.sections;
    // The legacy cylinder fields only feed TonicDefaultSections, which an
    // authored tube never reaches; keep them consistent anyway.
    snapshot.shape.radius = desc.sections.empty()
                                ? 0.5f
                                : TonicSectionMeanRadius(desc.sections.front());
    snapshot.shape.length =
        TonicCenterArcLength(desc.centerX.data(), desc.centerY.data(),
                             desc.centerZ.data(), int(desc.centerX.size()));
    snapshot.fill = record.fill;
    snapshot.subdivide = record.subdivide;
    snapshot.locked = record.locked;
    snapshot.lockParents = record.lockParents;
    snapshot.lockChildren = record.lockChildren;
    return snapshot;
}

TonicGuideSet
TonicGenerateGuidesForTube(TonicModel::TubeSnapshot const &snapshot,
                           int tubeId)
{
    TonicGuideSet guides;
    if (!snapshot.hasTube) {
        return guides;
    }
    int const count = TonicGuideCountForDensity(snapshot.fill.density);
    int const cvCount = snapshot.fill.cvCount;
    if (count <= 0 || cvCount < 2) {
        return guides;
    }
    TonicTubeDesc const tube = TonicTubeDescFromSnapshot(snapshot);
    if (tube.centerX.size() < 2 || tube.sections.size() < 2) {
        return guides;
    }
    std::vector<TonicFrame> frames;
    std::string err;
    if (!TonicCenterFramesCpu(tube.centerX.data(), tube.centerY.data(),
                              tube.centerZ.data(), int(tube.centerX.size()),
                              &frames, &err)) {
        return guides;
    }
    float const rootRadius =
        TonicSectionMeanRadius(tube.sections.front());
    if (!(rootRadius > 0.0f)) {
        return guides;
    }
    float const rootCenter[3] = {tube.centerX[0], tube.centerY[0],
                                 tube.centerZ[0]};
    std::vector<TonicGuideRoot> roots;
    // The root stream is keyed by (tubeId, seed), so siblings that share
    // fill params still draw distinct root patterns (plan/17 §4.1).
    if (!TonicRootSampleDiscCpu(tubeId, snapshot.fill.seed, rootRadius,
                                rootCenter, frames[0], count, &roots, 0,
                                &err)) {
        return guides;
    }
    if (!_FillAndResample(tube, frames, roots, snapshot.fill, &guides)) {
        return TonicGuideSet();
    }
    return guides;
}

TonicGuideSet
TonicGenerateGuidesOnScalpForTube(TonicModel::TubeSnapshot const &snapshot,
                                  TonicScalpMesh const &scalp,
                                  int const *regionFaces, int regionFaceCount,
                                  int tubeId)
{
    TonicGuideSet guides;
    if (!snapshot.hasTube || !scalp.finalized) {
        return guides;
    }
    if (!regionFaces || regionFaceCount <= 0) {
        return TonicGenerateGuidesForTube(snapshot, tubeId);
    }
    TonicTubeDesc const tube = TonicTubeDescFromSnapshot(snapshot);
    if (tube.centerX.size() < 2 || tube.sections.size() < 2) {
        return guides;
    }
    std::vector<TonicFrame> frames;
    std::string err;
    if (!TonicCenterFramesCpu(tube.centerX.data(), tube.centerY.data(),
                              tube.centerZ.data(), int(tube.centerX.size()),
                              &frames, &err)) {
        return guides;
    }
    float const rootRadius =
        TonicSectionMeanRadius(tube.sections.front());
    if (!(rootRadius > 0.0f)) {
        return guides;
    }
    float const rootCenter[3] = {tube.centerX[0], tube.centerY[0],
                                 tube.centerZ[0]};
    std::vector<TonicGuideRoot> roots;
    if (!TonicRootSampleMeshCpu(
            scalp.points.data(), scalp.faceVertexCounts.data(),
            scalp.faceVertexIndices.data(), scalp.faceOffsets.data(),
            int(scalp.faceVertexCounts.size()), regionFaces, regionFaceCount,
            snapshot.fill.density, tubeId, snapshot.fill.seed, rootCenter,
            frames[0], rootRadius, &roots, 0, &err)) {
        return guides;
    }
    if (roots.empty()) {
        return guides;
    }
    if (!_FillAndResample(tube, frames, roots, snapshot.fill, &guides)) {
        return TonicGuideSet();
    }
    return guides;
}

// -- P2: scalp binding, graph, region maps -------------------------------

bool
TonicModel::BindScalp(std::vector<float> const &points,
                      std::vector<int> const &faceVertexCounts,
                      std::vector<int> const &faceVertexIndices)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::shared_ptr<TonicScalpMesh> scalp(new TonicScalpMesh());
    scalp->points = points;
    scalp->faceVertexCounts = faceVertexCounts;
    scalp->faceVertexIndices = faceVertexIndices;
    std::string err;
    if (!TonicScalpFinalize(scalp.get(), &err)) {
        _diagnostic = err;
        return false;
    }
    TonicScalpBvh bvh;
    if (!TonicScalpBvhBuild(*scalp, &bvh, &err)) {
        _diagnostic = err;
        return false;
    }
    _ClearUndoLocked();  // rebind invalidates region-linked snapshots
    _scalp = std::move(scalp);
    _bvh = std::move(bvh);
    _graph.Clear();
    _maps = TonicRegionMaps();
    _loops = TonicRegionLoops();
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    ++_version;
    ++_mapVersion;
    return true;
}

bool
TonicModel::HasScalp() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return bool(_scalp) && _scalp->finalized;
}

std::shared_ptr<TonicScalpMesh const>
TonicModel::GetScalp() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _scalp;
}

TonicHit
TonicModel::Raycast(float const origin[3], float const dir[3]) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_scalp || !_scalp->finalized || !_bvh.valid) {
        return TonicHit();
    }
    return TonicRaycastCpu(*_scalp, _bvh, origin, dir);
}

TonicHit
TonicModel::ClosestPoint(float const p[3]) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_scalp || !_scalp->finalized || !p) {
        return TonicHit();
    }
    return TonicClosestPointCpu(*_scalp, p);
}

namespace {

// Mirror-twin a placed node across x = 0 (model flag). Best-effort: a miss
// (asymmetric scalp) twins nothing, and the placed node survives.
void _MaybeMirror(TonicScalpMesh const &scalp, TonicScalpGraph *graph,
                  TonicHit const &hit)
{
    float const mirrored[3] = {-hit.px, hit.py, hit.pz};
    TonicHit const twin = TonicClosestPointCpu(scalp, mirrored);
    if (twin.hit) {
        graph->AddNode(twin);
    }
}

}  // namespace

int
TonicModel::GraphAddNode(TonicHit const &hit)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphAddNode: no scalp bound";
        return -1;
    }
    int const id = _graph.AddNode(hit);
    if (id < 0) {
        _diagnostic = _graph.GetDiagnostic();
        return -1;
    }
    if (_mirrorX) {
        _MaybeMirror(*_scalp, &_graph, hit);
    }
    _graph.ExtractRegions(*_scalp);
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "add node");
    ++_version;
    ++_mapVersion;
    return id;
}

bool
TonicModel::GraphMoveNode(int nodeId, TonicHit const &hit)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphMoveNode: no scalp bound";
        return false;
    }
    if (!_graph.MoveNode(*_scalp, nodeId, hit)) {
        _diagnostic = _graph.GetDiagnostic();
        return false;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "move node");
    ++_version;
    ++_mapVersion;
    return true;
}

int
TonicModel::GraphConnect(int a, int b)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphConnect: no scalp bound";
        return -1;
    }
    int const id = _graph.Connect(*_scalp, a, b);
    if (id < 0) {
        _diagnostic = _graph.GetDiagnostic();
        return -1;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "connect");
    ++_version;
    ++_mapVersion;
    return id;
}

int
TonicModel::GraphSplitEdge(int edgeId, TonicHit const &hit)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphSplitEdge: no scalp bound";
        return -1;
    }
    int const id = _graph.SplitEdge(*_scalp, edgeId, hit);
    if (id < 0) {
        _diagnostic = _graph.GetDiagnostic();
        return -1;
    }
    if (_mirrorX) {
        _MaybeMirror(*_scalp, &_graph, hit);
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "split edge");
    ++_version;
    ++_mapVersion;
    return id;
}

bool
TonicModel::GraphWeld(int keep, int drop)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphWeld: no scalp bound";
        return false;
    }
    if (!_graph.Weld(*_scalp, keep, drop)) {
        _diagnostic = _graph.GetDiagnostic();
        return false;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "weld");
    ++_version;
    ++_mapVersion;
    return true;
}

int
TonicModel::GraphWeldAll(float radius)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphWeldAll: no scalp bound";
        return -1;
    }
    int const welds = _graph.WeldAllWithinRadius(*_scalp, radius);
    if (welds > 0) {
        _dirty |= TonicDirty_Graph | TonicDirty_Regions;
        _PushGraphUndoLocked(graphBefore, "weld all");
        ++_version;
        ++_mapVersion;
    }
    return welds;
}

std::vector<int>
TonicModel::GraphUnweld(int nodeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    std::vector<int> created;
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphUnweld: no scalp bound";
        return created;
    }
    created = _graph.Unweld(*_scalp, nodeId);
    if (!created.empty()) {
        _dirty |= TonicDirty_Graph | TonicDirty_Regions;
        _PushGraphUndoLocked(graphBefore, "unweld");
        ++_version;
        ++_mapVersion;
    }
    return created;
}

bool
TonicModel::GraphDeleteEdge(int edgeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphDeleteEdge: no scalp bound";
        return false;
    }
    if (!_graph.DeleteEdge(*_scalp, edgeId)) {
        _diagnostic = _graph.GetDiagnostic();
        return false;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "delete edge");
    ++_version;
    ++_mapVersion;
    return true;
}

bool
TonicModel::GraphDeleteNode(int nodeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphDeleteNode: no scalp bound";
        return false;
    }
    if (!_graph.DeleteNode(*_scalp, nodeId)) {
        _diagnostic = _graph.GetDiagnostic();
        return false;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "delete node");
    ++_version;
    ++_mapVersion;
    return true;
}

int
TonicModel::GraphSnapNode(float const p[3], float radius) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _graph.SnapNode(p, radius);
}

int
TonicModel::GraphSnapEdge(float const p[3], float radius) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _graph.SnapEdge(p, radius);
}

bool
TonicModel::GraphLinkRegions(int r0, int r1)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_graph.LinkRegions(r0, r1)) {
        _diagnostic = _graph.GetDiagnostic();
        return false;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "link regions");
    ++_version;
    ++_mapVersion;
    return true;
}

bool
TonicModel::GraphUnlinkRegions(int r0, int r1)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    if (!_graph.UnlinkRegions(r0, r1)) {
        _diagnostic = "TonicModel::GraphUnlinkRegions: no such link";
        return false;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    _PushGraphUndoLocked(graphBefore, "unlink regions");
    ++_version;
    ++_mapVersion;
    return true;
}

TonicStrokeResult
TonicModel::GraphStroke(std::vector<TonicHit> const &samples, float snapRadius,
                        float simplifyEps)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    TonicStrokeResult empty;
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphStroke: no scalp bound";
        return empty;
    }
    TonicStrokeResult result =
        TonicStrokeToChain(*_scalp, &_graph, samples, snapRadius, simplifyEps);
    if (!result.nodeIds.empty()) {
        _dirty |= TonicDirty_Graph | TonicDirty_Regions;
        _PushGraphUndoLocked(graphBefore, "graph stroke");
        ++_version;
        ++_mapVersion;
    }
    return result;
}

std::vector<std::pair<int, int>>
TonicModel::GraphMirrorX()
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const graphBefore = _GraphUndoStateLocked();
    std::vector<std::pair<int, int>> mapping;
    if (!_scalp) {
        _diagnostic = "TonicModel::GraphMirrorX: no scalp bound";
        return mapping;
    }
    mapping = _graph.MirrorX(*_scalp);
    if (!mapping.empty()) {
        _dirty |= TonicDirty_Graph | TonicDirty_Regions;
        _PushGraphUndoLocked(graphBefore, "mirror graph");
        ++_version;
        ++_mapVersion;
    }
    return mapping;
}

void
TonicModel::SetSnapRadius(float radius)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _snapRadius = radius;
}

float
TonicModel::GetSnapRadius() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _snapRadius;
}

void
TonicModel::SetMirrorX(bool on)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _mirrorX = on;
}

bool
TonicModel::GetMirrorX() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _mirrorX;
}

uint64_t
TonicModel::GetMapVersion() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _mapVersion;
}

bool
TonicModel::Rasterise()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_scalp) {
        _diagnostic = "TonicModel::Rasterise: no scalp bound";
        return false;
    }
    std::string err;
    if (!TonicRasteriseRegionsCpu(*_scalp, _graph, &_maps, &err)) {
        _diagnostic = err;
        return false;
    }
    if (!TonicFlattenLoops(_graph, &_loops, &err)) {
        _diagnostic = err;
        return false;
    }
    _dirty |= TonicDirty_Regions;
    // K3 is the gesture-end step (§4.2), and it is the first moment the new
    // region ids have claimed faces, so this is where the L1 tubes are
    // re-attached to the regions a graph edit left behind (plan/17 §5.1,
    // plan/18 §7 G14). A refusal leaves the maps and the tubes untouched
    // and reports through GetDiagnostic; the rasterise itself stands.
    if (!_SyncRegionTubesLocked(nullptr, nullptr)) {
        return false;
    }
    return true;
}

void
TonicModel::NoteBakedMapFile(uint64_t mapVersion, std::string const &path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _bakedMapVersion = mapVersion;
    _bakedMapFile = path;
}

TonicModel::GraphSnapshot
TonicModel::SnapshotGraph() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    GraphSnapshot snapshot;
    snapshot.snapRadius = _snapRadius;
    snapshot.mapVersion = _mapVersion;
    snapshot.bakedMapFile = _bakedMapFile;
    snapshot.bakedMapVersion = _bakedMapVersion;
    snapshot.hasScalp = bool(_scalp) && _scalp->finalized;
    if (!snapshot.hasScalp) {
        return snapshot;
    }
    snapshot.scalpFaceCount = _scalp->faceVertexCounts.size();
    for (auto const &nd : _graph.Nodes()) {
        if (nd.alive) {
            snapshot.nodes.push_back(nd);
        }
    }
    for (auto const &e : _graph.Edges()) {
        if (e.alive) {
            snapshot.edges.emplace_back(e.a, e.b);
        }
    }
    snapshot.linked = _graph.LinkedPairs();
    for (auto const &r : _graph.Regions()) {
        snapshot.regionLoops.push_back(r.loop);
        snapshot.regionColors.push_back(r.color[0]);
        snapshot.regionColors.push_back(r.color[1]);
        snapshot.regionColors.push_back(r.color[2]);
    }
    // The live primvar is derived, not stored: rasterise here so the
    // snapshot always carries the graph's current map, even when K3 has not
    // run since the last edit (the worker must never read the live model).
    TonicRegionMaps maps;
    std::string err;
    if (TonicRasteriseRegionsCpu(*_scalp, _graph, &maps, &err)) {
        snapshot.faceRegions = std::move(maps.faceRegion);
        snapshot.faceRegionIds = std::move(maps.faceRegionId);
        snapshot.uncoveredCount = maps.uncoveredCount;
        snapshot.intersectedCount = maps.intersectedCount;
    }
    return snapshot;
}

bool
TonicModel::RestoreGraph(GraphSnapshot const &snapshot)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_scalp) {
        _diagnostic = "TonicModel::RestoreGraph: no scalp bound";
        return false;
    }
    std::string err;
    if (!_graph.Restore(*_scalp, snapshot.nodes, snapshot.edges,
                        snapshot.linked, &err)) {
        _diagnostic = err;
        return false;
    }
    _snapRadius = snapshot.snapRadius;
    if (!TonicRasteriseRegionsCpu(*_scalp, _graph, &_maps, &err) ||
        !TonicFlattenLoops(_graph, &_loops, &err)) {
        _diagnostic = err;
        return false;
    }
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
    ++_version;
    ++_mapVersion;
    return true;
}

bool
TonicModel::_TessellateHost()
{
    if (_useSections) {
        return _TessellateSectionsHost();
    }
    // Legacy cylinder path (the P0/P1 position contract). The K4 frame
    // cache still refreshes so gizmo and fill code reads one source.
    {
        std::string ferr;
        std::vector<TonicFrame> frames;
        if (TonicCenterFramesCpu(_host.centerX.data(), _host.centerY.data(),
                                 _host.centerZ.data(), _shape.rings, &frames,
                                 &ferr)) {
            _frames = std::move(frames);
        }
    }
    int const vertexCount = TonicTubeVertexCount(_shape);
    int const quadCount = TonicTubeQuadCount(_shape);
    if (vertexCount <= 0 || quadCount <= 0) {
        _diagnostic = "TonicModel: tessellate with no tube built";
        return false;
    }
    _host.positions.resize(size_t(vertexCount) * 3);
    _host.normals.resize(size_t(vertexCount) * 3);
    float const *cx = _host.centerX.data();
    float const *cy = _host.centerY.data();
    float const *cz = _host.centerZ.data();
    float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    for (int v = 0; v < vertexCount; ++v) {
        TonicTessellatedVertex const tv =
            TonicTessellateVertex(_shape, v, cx, cy, cz);
        _host.positions[size_t(v) * 3 + 0] = tv.px;
        _host.positions[size_t(v) * 3 + 1] = tv.py;
        _host.positions[size_t(v) * 3 + 2] = tv.pz;
        _host.normals[size_t(v) * 3 + 0] = tv.nx;
        _host.normals[size_t(v) * 3 + 1] = tv.ny;
        _host.normals[size_t(v) * 3 + 2] = tv.nz;
        if (v == 0) {
            minX = maxX = tv.px;
            minY = maxY = tv.py;
            minZ = maxZ = tv.pz;
        } else {
            minX = std::min(minX, tv.px);
            minY = std::min(minY, tv.py);
            minZ = std::min(minZ, tv.pz);
            maxX = std::max(maxX, tv.px);
            maxY = std::max(maxY, tv.py);
            maxZ = std::max(maxZ, tv.pz);
        }
    }
    _host.extentMin[0] = minX;
    _host.extentMin[1] = minY;
    _host.extentMin[2] = minZ;
    _host.extentMax[0] = maxX;
    _host.extentMax[1] = maxY;
    _host.extentMax[2] = maxZ;
    // Quad strip between adjacent rings, CCW from outside.
    _host.faceVertexCounts.assign(size_t(quadCount), 4);
    _host.faceVertexIndices.resize(size_t(quadCount) * 4);
    int const rv = _shape.ringVerts;
    for (int r = 0; r < _shape.rings - 1; ++r) {
        for (int s = 0; s < rv; ++s) {
            int const q = r * rv + s;
            int const s1 = (s + 1) % rv;
            _host.faceVertexIndices[size_t(q) * 4 + 0] = r * rv + s;
            _host.faceVertexIndices[size_t(q) * 4 + 1] = r * rv + s1;
            _host.faceVertexIndices[size_t(q) * 4 + 2] = (r + 1) * rv + s1;
            _host.faceVertexIndices[size_t(q) * 4 + 3] = (r + 1) * rv + s;
        }
    }
    return true;
}

bool
TonicModel::_TessellateSectionsHost()
{
    TonicTubeDesc const tube = _BuildTubeDescLocked();
    if (tube.centerX.size() < 2 || tube.sections.size() < 2) {
        _diagnostic = "TonicModel: tessellate with no tube built";
        return false;
    }
    std::string err;
    std::vector<TonicFrame> frames;
    if (!TonicCenterFramesCpu(tube.centerX.data(), tube.centerY.data(),
                              tube.centerZ.data(), int(tube.centerX.size()),
                              &frames, &err)) {
        _diagnostic = err;
        return false;
    }
    _frames = frames;
    std::vector<float> ringT;
    if (!TonicTessellateCpu(tube, frames, _segmentsPerSpan, &_host.positions,
                            &_host.normals, &ringT, &err)) {
        _diagnostic = err;
        return false;
    }
    int const rv = tube.ringVerts;
    int const nRings = int(_host.positions.size() / size_t(rv) / 3);
    float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    size_t const vertexCount = _host.positions.size() / 3;
    for (size_t v = 0; v < vertexCount; ++v) {
        float const px = _host.positions[v * 3 + 0];
        float const py = _host.positions[v * 3 + 1];
        float const pz = _host.positions[v * 3 + 2];
        if (v == 0) {
            minX = maxX = px;
            minY = maxY = py;
            minZ = maxZ = pz;
        } else {
            minX = std::min(minX, px);
            minY = std::min(minY, py);
            minZ = std::min(minZ, pz);
            maxX = std::max(maxX, px);
            maxY = std::max(maxY, py);
            maxZ = std::max(maxZ, pz);
        }
    }
    _host.extentMin[0] = minX;
    _host.extentMin[1] = minY;
    _host.extentMin[2] = minZ;
    _host.extentMax[0] = maxX;
    _host.extentMax[1] = maxY;
    _host.extentMax[2] = maxZ;
    int const quadCount = (nRings - 1) * rv;
    _host.faceVertexCounts.assign(size_t(quadCount), 4);
    _host.faceVertexIndices.resize(size_t(quadCount) * 4);
    for (int r = 0; r < nRings - 1; ++r) {
        for (int s = 0; s < rv; ++s) {
            int const q = r * rv + s;
            int const s1 = (s + 1) % rv;
            _host.faceVertexIndices[size_t(q) * 4 + 0] = r * rv + s;
            _host.faceVertexIndices[size_t(q) * 4 + 1] = r * rv + s1;
            _host.faceVertexIndices[size_t(q) * 4 + 2] = (r + 1) * rv + s1;
            _host.faceVertexIndices[size_t(q) * 4 + 3] = (r + 1) * rv + s;
        }
    }
    return true;
}

bool
TonicModel::_SyncDevice()
{
#ifdef USDGEN_TONIC_HAS_CUDA
    if (_useSections) {
        return _SyncSectionsDevice();
    }
    if (!_device || !_device->streamOwned || !_device->stream) {
        // No device on this host (GPU-less CI) or dropped by the P6
        // fallback: the host mirror is authoritative and the CUDA mirror
        // simply does not exist. This is the §3.4 CPU fallback, not an
        // error.
        return true;
    }
    if (DeviceFallbackForced()) {
        _DropDeviceLocked("forced by USDGEN_TONIC_FORCE_DEVICE_FALLBACK");
        return true;
    }
    int const vertexCount = TonicTubeVertexCount(_shape);
    int const rings = _shape.rings;
    if (_device->positions.reset(size_t(vertexCount) * 3) != cudaSuccess ||
        _device->normals.reset(size_t(vertexCount) * 3) != cudaSuccess ||
        _device->centerX.reset(size_t(rings)) != cudaSuccess ||
        _device->centerY.reset(size_t(rings)) != cudaSuccess ||
        _device->centerZ.reset(size_t(rings)) != cudaSuccess) {
        // P6 OOM fallback: the op still succeeds on the host mirror.
        _DropDeviceLocked("TonicModel: device buffer allocation failed");
        return true;
    }
    if (cudaMemcpyAsync(_device->centerX.data(), _host.centerX.data(),
                        sizeof(float) * size_t(rings),
                        cudaMemcpyHostToDevice,
                        _device->stream) != cudaSuccess ||
        cudaMemcpyAsync(_device->centerY.data(), _host.centerY.data(),
                        sizeof(float) * size_t(rings),
                        cudaMemcpyHostToDevice,
                        _device->stream) != cudaSuccess ||
        cudaMemcpyAsync(_device->centerZ.data(), _host.centerZ.data(),
                        sizeof(float) * size_t(rings),
                        cudaMemcpyHostToDevice,
                        _device->stream) != cudaSuccess) {
        _DropDeviceLocked("TonicModel: center column upload failed");
        return true;
    }
    char errBuf[256] = {0};
    if (!TonicLaunchTessellate(_shape,
                               _device->centerX.data(),
                               _device->centerY.data(),
                               _device->centerZ.data(),
                               _device->positions.data(),
                               _device->normals.data(),
                               _device->stream, errBuf, sizeof(errBuf))) {
        std::string const why =
            std::string("TonicModel: tessellate kernel failed: ") + errBuf;
        _DropDeviceLocked(why.c_str());
        return true;
    }
    if (_device->positions.recordUse(_device->stream) != cudaSuccess ||
        _device->normals.recordUse(_device->stream) != cudaSuccess) {
        _DropDeviceLocked("TonicModel: device use tracking failed");
        return true;
    }
    (void)std::memset(errBuf, 0, sizeof(errBuf));
#else
    (void)0;
#endif
    return true;
}

#ifdef USDGEN_TONIC_HAS_CUDA
bool
TonicModel::_SyncSectionsDevice()
{
    if (!_device || !_device->streamOwned || !_device->stream) {
        return true;  // GPU-less CI (or the P6 fallback dropped the
                      // mirror): the host mirror is authoritative.
    }
    if (DeviceFallbackForced()) {
        _DropDeviceLocked("forced by USDGEN_TONIC_FORCE_DEVICE_FALLBACK");
        return true;
    }
    TonicTubeDesc const tube = _BuildTubeDescLocked();
    int const nCv = int(tube.centerX.size());
    int const nSec = int(tube.sections.size());
    int const rv = tube.ringVerts;
    int const nRings = TonicTessellatedRingCount(tube, _segmentsPerSpan);
    if (nCv < 2 || nSec < 2 || nRings <= 0) {
        _diagnostic = "TonicModel: sections device sync with no tube";
        return false;
    }
    static_assert(sizeof(TonicFrame) == 9 * sizeof(float) &&
                      alignof(TonicFrame) == alignof(float),
                  "TonicFrame must stay 9 packed floats");
    if (_device->positions.reset(size_t(nRings) * size_t(rv) * 3) !=
            cudaSuccess ||
        _device->normals.reset(size_t(nRings) * size_t(rv) * 3) !=
            cudaSuccess ||
        _device->centerX.reset(size_t(nCv)) != cudaSuccess ||
        _device->centerY.reset(size_t(nCv)) != cudaSuccess ||
        _device->centerZ.reset(size_t(nCv)) != cudaSuccess ||
        _device->frames.reset(size_t(nCv) * 9) != cudaSuccess ||
        _device->sectionT.reset(size_t(nSec)) != cudaSuccess ||
        _device->sectionU.reset(size_t(nSec) * size_t(rv)) != cudaSuccess ||
        _device->sectionV.reset(size_t(nSec) * size_t(rv)) != cudaSuccess ||
        _device->sectionScale.reset(size_t(nSec)) != cudaSuccess ||
        _device->sectionTwist.reset(size_t(nSec)) != cudaSuccess ||
        _device->ringT.reset(size_t(nRings)) != cudaSuccess) {
        _DropDeviceLocked("TonicModel: device buffer allocation failed");
        return true;
    }
    // Sized by assign: `X(size_t(n))` would parse as a function
    // declaration (the most vexing parse).
    std::vector<float> secT, secU, secV, secS, secTw;
    secT.assign(size_t(nSec), 0.0f);
    secU.assign(size_t(nSec) * size_t(rv), 0.0f);
    secV.assign(size_t(nSec) * size_t(rv), 0.0f);
    secS.assign(size_t(nSec), 0.0f);
    secTw.assign(size_t(nSec), 0.0f);
    for (int i = 0; i < nSec; ++i) {
        TonicTubeSection const &s = tube.sections[size_t(i)];
        secT[size_t(i)] = s.t;
        secS[size_t(i)] = s.scale;
        secTw[size_t(i)] = s.twist;
        for (int k = 0; k < rv; ++k) {
            secU[size_t(i) * size_t(rv) + size_t(k)] = s.u[size_t(k)];
            secV[size_t(i) * size_t(rv) + size_t(k)] = s.v[size_t(k)];
        }
    }
    cudaStream_t const stream = _device->stream;
    auto upload = [&](void *dst, void const *src, size_t bytes) {
        return cudaMemcpyAsync(dst, src, bytes, cudaMemcpyHostToDevice,
                               stream) == cudaSuccess;
    };
    if (!upload(_device->centerX.data(), tube.centerX.data(),
                sizeof(float) * size_t(nCv)) ||
        !upload(_device->centerY.data(), tube.centerY.data(),
                sizeof(float) * size_t(nCv)) ||
        !upload(_device->centerZ.data(), tube.centerZ.data(),
                sizeof(float) * size_t(nCv)) ||
        !upload(_device->sectionT.data(), secT.data(),
                sizeof(float) * size_t(nSec)) ||
        !upload(_device->sectionU.data(), secU.data(),
                sizeof(float) * secU.size()) ||
        !upload(_device->sectionV.data(), secV.data(),
                sizeof(float) * secV.size()) ||
        !upload(_device->sectionScale.data(), secS.data(),
                sizeof(float) * size_t(nSec)) ||
        !upload(_device->sectionTwist.data(), secTw.data(),
                sizeof(float) * size_t(nSec))) {
        _DropDeviceLocked("TonicModel: tube upload failed");
        return true;
    }
    char errBuf[256] = {0};
    TonicFrame *frames = reinterpret_cast<TonicFrame *>(_device->frames.data());
    if (!TonicLaunchCenterFrames(_device->centerX.data(),
                                 _device->centerY.data(),
                                 _device->centerZ.data(), nCv, frames, stream,
                                 errBuf, sizeof(errBuf))) {
        std::string const why =
            std::string("TonicModel: center frames kernel failed: ") + errBuf;
        _DropDeviceLocked(why.c_str());
        return true;
    }
    if (!TonicLaunchTubeTessellate(
            _device->centerX.data(), _device->centerY.data(),
            _device->centerZ.data(), nCv, frames,
            _device->sectionT.data(), _device->sectionU.data(),
            _device->sectionV.data(), _device->sectionScale.data(),
            _device->sectionTwist.data(), nSec, rv, _segmentsPerSpan,
            _device->positions.data(), _device->normals.data(),
            _device->ringT.data(), stream, errBuf, sizeof(errBuf))) {
        std::string const why =
            std::string("TonicModel: tube tessellate kernel failed: ") +
            errBuf;
        _DropDeviceLocked(why.c_str());
        return true;
    }
    if (_device->positions.recordUse(stream) != cudaSuccess ||
        _device->normals.recordUse(stream) != cudaSuccess) {
        _DropDeviceLocked("TonicModel: device use tracking failed");
        return true;
    }
    return true;
}
#endif

// -- P3: Tube mode --------------------------------------------------------

bool
TonicModel::_RegionTubeDescLocked(int regionId, int centerCount,
                                  int ringVerts, float length,
                                  TonicTubeDesc *out, float *outRadius)
{
    if (!out || !outRadius) {
        return false;
    }
    if (!_scalp || !_scalp->finalized) {
        _diagnostic = "TonicModel::BuildTubeFromRegion: no scalp bound";
        return false;
    }
    if (regionId < 0 || regionId >= _graph.RegionCount() || centerCount < 2 ||
        ringVerts < 3 || ringVerts > 32 || !(length > 0.0f)) {
        _diagnostic = "TonicModel::BuildTubeFromRegion: bad region, counts "
                      "or length";
        return false;
    }
    // Region faces at the region's interpolation id (linked tubes share
    // the fill set, §2.2); read from the last Rasterise.
    int const interp = _graph.InterpId(regionId);
    std::vector<int> faces;
    for (size_t f = 0; f < _maps.faceRegion.size(); ++f) {
        if (_maps.faceRegion[f] == interp) {
            faces.push_back(int(f));
        }
    }
    if (faces.empty()) {
        _diagnostic = "TonicModel::BuildTubeFromRegion: region claims no "
                      "faces (rasterise first)";
        return false;
    }
    double cx = 0, cy = 0, cz = 0, nx = 0, ny = 0, nz = 0, area = 0;
    for (int f : faces) {
        double const a = double(_scalp->faceAreas[size_t(f)]);
        cx += double(_scalp->faceCentroids[size_t(f) * 3 + 0]) * a;
        cy += double(_scalp->faceCentroids[size_t(f) * 3 + 1]) * a;
        cz += double(_scalp->faceCentroids[size_t(f) * 3 + 2]) * a;
        nx += double(_scalp->faceNormals[size_t(f) * 3 + 0]) * a;
        ny += double(_scalp->faceNormals[size_t(f) * 3 + 1]) * a;
        nz += double(_scalp->faceNormals[size_t(f) * 3 + 2]) * a;
        area += a;
    }
    cx /= area;
    cy /= area;
    cz /= area;
    double nl = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(nl > 1e-12)) {
        nx = 0;
        ny = 1;
        nz = 0;
        nl = 1;
    }
    nx /= nl;
    ny /= nl;
    nz /= nl;
    // Fallback radius (used where the boundary gives no crossing): the
    // farthest claimed centroid from the area centroid, in the root plane.
    double fallback = 0;
    for (int f : faces) {
        double dx = double(_scalp->faceCentroids[size_t(f) * 3 + 0]) - cx;
        double dy = double(_scalp->faceCentroids[size_t(f) * 3 + 1]) - cy;
        double dz = double(_scalp->faceCentroids[size_t(f) * 3 + 2]) - cz;
        double const along = dx * nx + dy * ny + dz * nz;
        dx -= along * nx;
        dy -= along * ny;
        dz -= along * nz;
        fallback = std::max(fallback, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (!(fallback > 1e-6)) {
        fallback = 1e-6;
    }
    out->centerX.assign(size_t(centerCount), 0.0f);
    out->centerY.assign(size_t(centerCount), 0.0f);
    out->centerZ.assign(size_t(centerCount), 0.0f);
    for (int i = 0; i < centerCount; ++i) {
        double const s = double(i) / double(centerCount - 1);
        out->centerX[size_t(i)] = float(cx + nx * length * s);
        out->centerY[size_t(i)] = float(cy + ny * length * s);
        out->centerZ[size_t(i)] = float(cz + nz * length * s);
    }
    // -- root section fitted to the region boundary (plan/18 §7 G12) -------
    //
    // The section CVs are read in the K4 root frame, so the fit runs in
    // that frame's (n, b) basis: the boundary loop is projected into the
    // root plane and each ring slot takes the distance from the centroid to
    // the first boundary crossing along its own direction. A slot with no
    // crossing (a non-star-shaped lobe, or a boundary that failed to trace)
    // falls back to the extent radius, and every slot is clamped into
    // [0.1, 4] x the fitted median so one bad crossing cannot invert a ring.
    std::vector<TonicFrame> frames;
    std::string ferr;
    if (!TonicCenterFramesCpu(out->centerX.data(), out->centerY.data(),
                              out->centerZ.data(), centerCount, &frames,
                              &ferr) ||
        frames.empty()) {
        _diagnostic = ferr.empty() ? "TonicModel::BuildTubeFromRegion: root "
                                     "frame failed"
                                   : ferr;
        return false;
    }
    TonicFrame const &f0 = frames.front();
    std::vector<float> bu, bv;
    if (regionId < int(_graph.Regions().size())) {
        std::vector<float> const &loop = _graph.Regions()[size_t(regionId)]
                                             .boundary;
        for (size_t i = 0; i + 2 < loop.size(); i += 3) {
            double const dx = double(loop[i + 0]) - cx;
            double const dy = double(loop[i + 1]) - cy;
            double const dz = double(loop[i + 2]) - cz;
            bu.push_back(float(dx * f0.nx + dy * f0.ny + dz * f0.nz));
            bv.push_back(float(dx * f0.bx + dy * f0.by + dz * f0.bz));
        }
    }
    float const twoPi = 6.28318530717958647692f;
    std::vector<float> radii;
    radii.assign(size_t(ringVerts), float(fallback));
    if (bu.size() >= 3) {
        for (int s = 0; s < ringVerts; ++s) {
            float const a = twoPi * float(s) / float(ringVerts);
            double const dx = std::cos(a), dy = std::sin(a);
            double best = -1.0;
            for (size_t i = 0; i < bu.size(); ++i) {
                size_t const j = (i + 1) % bu.size();
                double const ax = bu[i], ay = bv[i];
                double const ex = double(bu[j]) - ax;
                double const ey = double(bv[j]) - ay;
                double const den = dx * ey - dy * ex;
                if (std::fabs(den) < 1e-12) {
                    continue;
                }
                double const t = (ax * ey - ay * ex) / den;
                double const w = (ax * dy - ay * dx) / den;
                if (t > 1e-6 && w >= 0.0 && w <= 1.0 &&
                    (best < 0.0 || t < best)) {
                    best = t;
                }
            }
            if (best > 0.0) {
                radii[size_t(s)] = float(best);
            }
        }
        std::vector<float> sorted = radii;
        std::sort(sorted.begin(), sorted.end());
        float const median = sorted[sorted.size() / 2];
        if (median > 1e-6f) {
            for (float &r : radii) {
                r = std::min(std::max(r, 0.1f * median), 4.0f * median);
            }
        }
    }
    double mean = 0;
    for (float r : radii) {
        mean += double(r);
    }
    mean /= double(ringVerts);
    out->ringVerts = ringVerts;
    out->regionId = regionId;
    out->level = 1;
    out->parentTubeId = -1;
    out->childIndex = -1;
    out->sections.resize(size_t(centerCount));
    for (int r = 0; r < centerCount; ++r) {
        TonicTubeSection &section = out->sections[size_t(r)];
        section.t = float(r) / float(centerCount - 1);
        section.scale = 1.0f;
        section.twist = 0.0f;
        section.u.resize(size_t(ringVerts));
        section.v.resize(size_t(ringVerts));
        for (int s = 0; s < ringVerts; ++s) {
            float const a = twoPi * float(s) / float(ringVerts);
            section.u[size_t(s)] = radii[size_t(s)] * std::cos(a);
            section.v[size_t(s)] = radii[size_t(s)] * std::sin(a);
        }
    }
    // The shape fields the display path still reads: `radius` is the mean
    // fitted radius, which is what the overlay widths scale from.
    _diagnostic.clear();
    *outRadius = float(mean);
    return true;
}

bool
TonicModel::_InstallTube0DescLocked(TonicTubeDesc const &desc)
{
    _shape.rings = int(desc.centerX.size());
    _shape.ringVerts = desc.ringVerts;
    _host.centerX = desc.centerX;
    _host.centerY = desc.centerY;
    _host.centerZ = desc.centerZ;
    _sections = desc.sections;
    _useSections = true;  // authored tubes always take the K5 path
    _tubeRegionId = desc.regionId;
    _roots.clear();
    _guides = TonicGuideSet();
    ++_guideVersion;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points | TonicDirty_Guides;
    return true;
}

int
TonicModel::_NextL1TubeIdLocked() const
{
    for (int id = 16;; id += 16) {
        if (_tubes.find(id) == _tubes.end()) {
            return id;
        }
    }
}

bool
TonicModel::BuildTubeFromRegion(int regionId, int centerCount, int ringVerts,
                                float length)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ringVerts < 8 || ringVerts > 32) {
        _diagnostic = "TonicModel::BuildTubeFromRegion: ring CV count is "
                      "8..32 (plan/17 §5.2); the bridge import path takes "
                      "3..32";
        return false;
    }
    TonicTubeDesc desc;
    float fitted = 0.0f;
    if (!_RegionTubeDescLocked(regionId, centerCount, ringVerts, length,
                               &desc, &fitted)) {
        return false;
    }
    // Which tube owns this region? An existing owner is refreshed in
    // place, the first build takes tube 0, and every later region appends
    // a new L1 root.
    int target = -1;
    auto const owner = _regionTube.find(regionId);
    if (owner != _regionTube.end()) {
        target = owner->second;
    } else if (_host.centerX.size() < 2) {
        target = 0;  // the model has no first L1 tube yet
    } else if (_tubeRegionId < 0 || _tubeRegionId == regionId) {
        // The first L1 tube is unrooted (a test tube, or a groom hydrated
        // before the map existed), or it already owns this region.
        target = 0;
    } else {
        target = _NextL1TubeIdLocked();
    }
    if (target != 0 && _SubtreeCarriesDeltasLocked(target)) {
        _diagnostic = "TonicModel::BuildTubeFromRegion: the region's tube "
                      "carries child deltas; merge the children first";
        return false;
    }
    desc.tubeId = target;
    HierarchyRollback const snap = _SnapshotHierarchyLocked();
    bool ok = true;
    if (target == 0) {
        _shape.length = length;
        _shape.radius = fitted;
        ok = _InstallTube0DescLocked(desc);
    } else {
        auto it = _tubes.find(target);
        if (it == _tubes.end()) {
            HierarchyTube entry;
            entry.actual = desc;
            entry.derived = desc;
            TonicClearDeltas(desc, &entry.deltas);
            entry.fill = _fill;
            _tubes[target] = entry;
        } else {
            it->second.actual = desc;
            it->second.derived = desc;
            TonicClearDeltas(desc, &it->second.deltas);
        }
        _dirty |= TonicDirty_Topology | TonicDirty_Points;
        ok = _PropagateDownLocked(target);
    }
    if (!ok) {
        _RestoreHierarchyLocked(snap);
        return false;
    }
    _PushUndoSnapshotLocked(snap);
    _regionTube[regionId] = target;
    _tubeRegionKey[target] = _RegionKeyLocked(regionId);
    ++_version;
    ++_mapVersion;  // a new L1 root changes the level-0 channel layout
    return true;
}

int
TonicModel::TubeForRegion(int regionId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const it = _regionTube.find(regionId);
    if (it != _regionTube.end()) {
        return it->second;
    }
    if (_tubeRegionId == regionId && _host.centerX.size() >= 2) {
        return 0;
    }
    return -1;
}

int
TonicModel::RegionForTube(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    TonicTubeDesc desc;
    if (!_TubeDescLocked(tubeId, &desc)) {
        return -1;
    }
    return desc.regionId;
}

std::vector<int>
TonicModel::L1TubeIds() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _L1TubeIdsLocked();
}

std::vector<int>
TonicModel::_L1TubeIdsLocked() const
{
    std::vector<int> out;
    if (_host.centerX.size() >= 2) {
        out.push_back(0);
    }
    for (auto const &kv : _tubes) {
        if (kv.first > 0 && kv.second.actual.parentTubeId < 0 &&
            kv.second.members.empty() && !kv.second.transientParent) {
            out.push_back(kv.first);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<int>
TonicModel::_RegionKeyLocked(int regionId) const
{
    std::vector<int> key;
    if (regionId < 0 || regionId >= int(_graph.Regions().size())) {
        return key;
    }
    key = _graph.Regions()[size_t(regionId)].loop;
    std::sort(key.begin(), key.end());
    key.erase(std::unique(key.begin(), key.end()), key.end());
    return key;
}

bool
TonicModel::_SubtreeCarriesDeltasLocked(int tubeId) const
{
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId != tubeId) {
            continue;
        }
        if (kv.second.imported) {
            return true;
        }
        for (float d : kv.second.deltas.centerDu) {
            if (std::fabs(d) > 1e-7f) {
                return true;
            }
        }
        for (float d : kv.second.deltas.centerDv) {
            if (std::fabs(d) > 1e-7f) {
                return true;
            }
        }
        for (float d : kv.second.deltas.centerDw) {
            if (std::fabs(d) > 1e-7f) {
                return true;
            }
        }
        for (auto const &sec : kv.second.deltas.sections) {
            for (float d : sec.u) {
                if (std::fabs(d) > 1e-7f) {
                    return true;
                }
            }
            for (float d : sec.v) {
                if (std::fabs(d) > 1e-7f) {
                    return true;
                }
            }
        }
        if (_SubtreeCarriesDeltasLocked(kv.first)) {
            return true;
        }
    }
    return false;
}

void
TonicModel::_DropSubtreeLocked(int tubeId)
{
    std::vector<int> kids;
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == tubeId) {
            kids.push_back(kv.first);
        }
    }
    for (int k : kids) {
        _DropSubtreeLocked(k);
    }
    _tubes.erase(tubeId);
    _tubeRegionKey.erase(tubeId);
}

bool
TonicModel::_RerootTubeLocked(int tubeId, int regionId)
{
    TonicTubeDesc old;
    if (!_TubeDescLocked(tubeId, &old)) {
        return false;
    }
    float length = 0.0f;
    {
        size_t const n = old.centerX.size();
        float const dx = old.centerX[n - 1] - old.centerX[0];
        float const dy = old.centerY[n - 1] - old.centerY[0];
        float const dz = old.centerZ[n - 1] - old.centerZ[0];
        length = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (!(length > 1e-6f)) {
        length = 1.0f;
    }
    TonicTubeDesc desc;
    float fitted = 0.0f;
    if (!_RegionTubeDescLocked(regionId, int(old.centerX.size()),
                               old.ringVerts, length, &desc, &fitted)) {
        return false;
    }
    desc.tubeId = tubeId;
    if (tubeId == 0) {
        _shape.length = length;
        _shape.radius = fitted;
        return _InstallTube0DescLocked(desc);
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return false;
    }
    it->second.actual = desc;
    it->second.derived = desc;
    TonicClearDeltas(desc, &it->second.deltas);
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    return _PropagateDownLocked(tubeId);
}

bool
TonicModel::_SyncRegionTubesLocked(std::vector<int> *outRebuilt,
                                   std::vector<int> *outRemoved)
{
    if (outRebuilt) {
        outRebuilt->clear();
    }
    if (outRemoved) {
        outRemoved->clear();
    }
    // Every L1 tube that is rooted in a region, with the node loop it was
    // rooted in. A tube with no key (an unrooted disc tube, or one built
    // before a scalp was bound) is left alone.
    std::vector<int> const l1 = _L1TubeIdsLocked();
    std::map<int, std::vector<int>> keys;
    for (int id : l1) {
        auto const it = _tubeRegionKey.find(id);
        if (it != _tubeRegionKey.end() && !it->second.empty()) {
            keys[id] = it->second;
        }
    }
    if (keys.empty()) {
        _regionTube.clear();
        for (int id : l1) {
            TonicTubeDesc desc;
            if (_TubeDescLocked(id, &desc) && desc.regionId >= 0 &&
                desc.regionId < _graph.RegionCount()) {
                _regionTube[desc.regionId] = id;
            }
        }
        return true;
    }
    int const regionCount = _graph.RegionCount();
    std::vector<std::vector<int>> current(size_t(std::max(regionCount, 0)));
    for (int r = 0; r < regionCount; ++r) {
        current[size_t(r)] = _RegionKeyLocked(r);
    }
    // Pass 1: exact loop matches keep their tube (a pure renumbering).
    std::map<int, int> claim;  // region -> tube
    std::map<int, int> placed;  // tube -> region
    for (int r = 0; r < regionCount; ++r) {
        for (auto const &kv : keys) {
            if (placed.count(kv.first) || claim.count(r)) {
                continue;
            }
            if (kv.second == current[size_t(r)]) {
                claim[r] = kv.first;
                placed[kv.first] = r;
            }
        }
    }
    // Pass 2: the rest go to the unclaimed region they share most nodes
    // with. That covers both directions of §5.1: a split region hands its
    // tube to the larger half, and a merge hands the merged region to the
    // tube that contributed more of its loop.
    std::vector<int> rebuilt, removed;
    for (auto const &kv : keys) {
        if (placed.count(kv.first)) {
            continue;
        }
        int best = -1;
        size_t bestShare = 0;
        for (int r = 0; r < regionCount; ++r) {
            if (claim.count(r)) {
                continue;
            }
            size_t share = 0;
            for (int node : current[size_t(r)]) {
                if (std::binary_search(kv.second.begin(), kv.second.end(),
                                       node)) {
                    ++share;
                }
            }
            if (share > bestShare) {
                bestShare = share;
                best = r;
            }
        }
        if (best >= 0 && bestShare >= 2) {
            claim[best] = kv.first;
            placed[kv.first] = best;
            rebuilt.push_back(kv.first);
        } else {
            removed.push_back(kv.first);
        }
    }
    if (rebuilt.empty() && removed.empty()) {
        _regionTube.clear();
        for (auto const &kv : claim) {
            _regionTube[kv.first] = kv.second;
            _tubeRegionKey[kv.second] = current[size_t(kv.first)];
        }
        return true;
    }
    // §5.1's refusal: re-deriving or dropping a tube whose children carry
    // authored deltas would throw that work away.
    for (int id : rebuilt) {
        if (_SubtreeCarriesDeltasLocked(id)) {
            _diagnostic = "TonicModel::SyncRegionTubes: the graph edit "
                          "re-roots a tube whose children carry deltas; "
                          "merge the children first";
            return false;
        }
    }
    for (int id : removed) {
        if (id == 0 || _SubtreeCarriesDeltasLocked(id)) {
            _diagnostic = "TonicModel::SyncRegionTubes: the graph edit "
                          "removes a tube that carries child deltas (or the "
                          "first L1 tube); merge the children first";
            return false;
        }
    }
    HierarchyRollback const snap = _SnapshotHierarchyLocked();
    for (int id : rebuilt) {
        if (!_RerootTubeLocked(id, placed[id])) {
            _RestoreHierarchyLocked(snap);
            return false;
        }
    }
    for (int id : removed) {
        _DropSubtreeLocked(id);
    }
    _regionTube.clear();
    for (auto const &kv : claim) {
        _regionTube[kv.first] = kv.second;
        _tubeRegionKey[kv.second] = current[size_t(kv.first)];
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    if (outRebuilt) {
        *outRebuilt = rebuilt;
    }
    if (outRemoved) {
        *outRemoved = removed;
    }
    return true;
}

bool
TonicModel::SyncRegionTubes(std::vector<int> *outRebuilt,
                            std::vector<int> *outRemoved)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _SyncRegionTubesLocked(outRebuilt, outRemoved);
}

bool
TonicModel::InstallL1Tube(int tubeId, TubeRecord const &record)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId <= 0 || (tubeId % 16) != 0) {
        _diagnostic = "TonicModel::InstallL1Tube: an L1 root id is a "
                      "positive multiple of 16";
        return false;
    }
    if (_tubes.count(tubeId)) {
        _diagnostic = "TonicModel::InstallL1Tube: the id is taken";
        return false;
    }
    if (record.actual.centerX.size() < 2 ||
        record.actual.sections.size() < 2) {
        _diagnostic = "TonicModel::InstallL1Tube: degenerate shape";
        return false;
    }
    HierarchyTube entry;
    entry.actual = record.actual;
    entry.actual.tubeId = tubeId;
    entry.actual.level = 1;
    entry.actual.parentTubeId = -1;
    entry.actual.childIndex = -1;
    entry.derived = entry.actual;
    TonicClearDeltas(entry.actual, &entry.deltas);
    entry.subdivide.count = record.subdivide.count;
    entry.subdivide.seed = record.subdivide.seed;
    entry.subdivide.splitMode = record.subdivide.splitMode == "edge"
                                    ? TonicSplit_Edge
                                    : TonicSplit_KMeans;
    entry.fill = record.fill;
    entry.lockParents = record.lockParents;
    entry.lockChildren = record.lockChildren;
    _tubes[tubeId] = entry;
    if (entry.actual.regionId >= 0) {
        _regionTube[entry.actual.regionId] = tubeId;
        _tubeRegionKey[tubeId] = _RegionKeyLocked(entry.actual.regionId);
    }
    ++_version;
    ++_mapVersion;
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    return true;
}

bool
TonicModel::MoveCenterCV(int cv, float dx, float dy, float dz)
{
    std::lock_guard<std::mutex> lock(_mutex);
    int const n = int(_host.centerX.size());
    if (cv < 0 || cv >= n || n < 2) {
        _diagnostic = "TonicModel::MoveCenterCV: CV index out of range";
        return false;
    }
    _PushUndoLocked();
    float const centerT = float(cv) / float(n - 1);
    for (int i = 0; i < n; ++i) {
        float const t = float(i) / float(n - 1);
        float const w = TonicSoftWeight(t, centerT, _softRadius);
        if (w <= 0.0f && i != cv) {
            continue;
        }
        float const k = (_softRadius > 0.0f) ? w : (i == cv ? 1.0f : 0.0f);
        _host.centerX[size_t(i)] += dx * k;
        _host.centerY[size_t(i)] += dy * k;
        _host.centerZ[size_t(i)] += dz * k;
    }
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::InsertCenterCV(int atIndex)
{
    std::lock_guard<std::mutex> lock(_mutex);
    int const n = int(_host.centerX.size());
    if (atIndex < 0 || atIndex > n || n < 2) {
        _diagnostic = "TonicModel::InsertCenterCV: index out of range";
        return false;
    }
    // Insert at the Catmull-Rom midpoint of the bracketing span (linear
    // midpoint at the ends).
    int i0 = atIndex - 1, i1 = atIndex;
    if (i0 < 0) {
        i0 = 0;
        i1 = 1;
    } else if (i1 >= n) {
        i0 = n - 2;
        i1 = n - 1;
    }
    float const mx = 0.5f * (_host.centerX[size_t(i0)] +
                             _host.centerX[size_t(i1)]);
    float const my = 0.5f * (_host.centerY[size_t(i0)] +
                             _host.centerY[size_t(i1)]);
    float const mz = 0.5f * (_host.centerZ[size_t(i0)] +
                             _host.centerZ[size_t(i1)]);
    _PushUndoLocked();
    _host.centerX.insert(_host.centerX.begin() + atIndex, mx);
    _host.centerY.insert(_host.centerY.begin() + atIndex, my);
    _host.centerZ.insert(_host.centerZ.begin() + atIndex, mz);
    _shape.rings = int(_host.centerX.size());
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::DeleteCenterCV(int index)
{
    std::lock_guard<std::mutex> lock(_mutex);
    int const n = int(_host.centerX.size());
    if (index < 0 || index >= n || n <= 2) {
        _diagnostic = "TonicModel::DeleteCenterCV: need >= 2 CVs left";
        return false;
    }
    _PushUndoLocked();
    _host.centerX.erase(_host.centerX.begin() + index);
    _host.centerY.erase(_host.centerY.begin() + index);
    _host.centerZ.erase(_host.centerZ.begin() + index);
    _shape.rings = int(_host.centerX.size());
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::SetTubeLength(float length)
{
    std::lock_guard<std::mutex> lock(_mutex);
    int const n = int(_host.centerX.size());
    if (!(length > 0.0f) || n < 2) {
        _diagnostic = "TonicModel::SetTubeLength: need a tube + length > 0";
        return false;
    }
    double total = 0;
    for (int i = 1; i < n; ++i) {
        double dx = double(_host.centerX[size_t(i)] -
                           _host.centerX[size_t(i - 1)]);
        double dy = double(_host.centerY[size_t(i)] -
                           _host.centerY[size_t(i - 1)]);
        double dz = double(_host.centerZ[size_t(i)] -
                           _host.centerZ[size_t(i - 1)]);
        total += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (!(total > 1e-12)) {
        _diagnostic = "TonicModel::SetTubeLength: degenerate center";
        return false;
    }
    double const k = double(length) / total;
    _PushUndoLocked();
    for (int i = 1; i < n; ++i) {
        _host.centerX[size_t(i)] =
            _host.centerX[0] +
            float((double(_host.centerX[size_t(i)]) -
                   double(_host.centerX[0])) *
                  k);
        _host.centerY[size_t(i)] =
            _host.centerY[0] +
            float((double(_host.centerY[size_t(i)]) -
                   double(_host.centerY[0])) *
                  k);
        _host.centerZ[size_t(i)] =
            _host.centerZ[0] +
            float((double(_host.centerZ[size_t(i)]) -
                   double(_host.centerZ[0])) *
                  k);
    }
    _shape.length = length;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::MatchSurface()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_scalp || !_scalp->finalized || _host.centerX.empty()) {
        _diagnostic = "TonicModel::MatchSurface: need a bound scalp + tube";
        return false;
    }
    float const p[3] = {_host.centerX[0], _host.centerY[0], _host.centerZ[0]};
    TonicHit const hit = TonicClosestPointCpu(*_scalp, p);
    if (!hit.hit) {
        _diagnostic = "TonicModel::MatchSurface: no surface point";
        return false;
    }
    _PushUndoLocked();
    _host.centerX[0] = hit.px;
    _host.centerY[0] = hit.py;
    _host.centerZ[0] = hit.pz;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

int
TonicModel::GetCenterCVCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return int(_host.centerX.size());
}

bool
TonicModel::GetCenterCV(int cv, float *x, float *y, float *z) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (cv < 0 || cv >= int(_host.centerX.size()) || !x || !y || !z) {
        return false;
    }
    *x = _host.centerX[size_t(cv)];
    *y = _host.centerY[size_t(cv)];
    *z = _host.centerZ[size_t(cv)];
    return true;
}

int
TonicModel::GetSectionCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return int(_sections.size());
}

bool
TonicModel::GetSection(int ring, TonicTubeSection *section) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ring < 0 || ring >= int(_sections.size()) || !section) {
        return false;
    }
    *section = _sections[size_t(ring)];
    return true;
}

bool
TonicModel::MoveSectionRing(int ring, float du, float dv)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ring < 0 || ring >= int(_sections.size())) {
        _diagnostic = "TonicModel::MoveSectionRing: ring out of range";
        return false;
    }
    _PushUndoLocked();
    TonicTubeSection &s = _sections[size_t(ring)];
    for (size_t i = 0; i < s.u.size(); ++i) {
        s.u[i] += du;
        s.v[i] += dv;
    }
    _useSections = true;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::ScaleSectionRing(int ring, float scale)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ring < 0 || ring >= int(_sections.size()) || !(scale > 0.0f)) {
        _diagnostic = "TonicModel::ScaleSectionRing: bad ring or scale";
        return false;
    }
    _PushUndoLocked();
    _sections[size_t(ring)].scale *= scale;
    _useSections = true;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::TwistSectionRing(int ring, float radians)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ring < 0 || ring >= int(_sections.size())) {
        _diagnostic = "TonicModel::TwistSectionRing: ring out of range";
        return false;
    }
    _PushUndoLocked();
    _sections[size_t(ring)].twist += radians;
    _useSections = true;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::MoveSectionCV(int ring, int slot, float du, float dv)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ring < 0 || ring >= int(_sections.size()) || slot < 0 ||
        slot >= int(_sections[size_t(ring)].u.size())) {
        _diagnostic = "TonicModel::MoveSectionCV: ring/slot out of range";
        return false;
    }
    _PushUndoLocked();
    TonicTubeSection &s = _sections[size_t(ring)];
    s.u[size_t(slot)] += du;
    s.v[size_t(slot)] += dv;
    _useSections = true;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::AddSectionRing(float t)
{
    std::lock_guard<std::mutex> lock(_mutex);
    int const nSec = int(_sections.size());
    if (nSec < 2) {
        _diagnostic = "TonicModel::AddSectionRing: no tube";
        return false;
    }
    float const t0 = _sections.front().t;
    float const t1 = _sections.back().t;
    if (!(t > t0) || !(t < t1)) {
        _diagnostic = "TonicModel::AddSectionRing: t must sit strictly "
                      "inside the section range";
        return false;
    }
    int k = 0;
    while (k + 1 < nSec - 1 && _sections[size_t(k + 1)].t < t) {
        ++k;
    }
    TonicTubeSection const &s0 = _sections[size_t(k)];
    TonicTubeSection const &s1 = _sections[size_t(k + 1)];
    float f = (s1.t > s0.t) ? (t - s0.t) / (s1.t - s0.t) : 0.0f;
    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    TonicTubeSection added;
    added.t = t;
    added.u.resize(s0.u.size());
    added.v.resize(s0.v.size());
    for (size_t i = 0; i < s0.u.size(); ++i) {
        added.u[i] = s0.u[i] + (s1.u[i] - s0.u[i]) * f;
        added.v[i] = s0.v[i] + (s1.v[i] - s0.v[i]) * f;
    }
    added.scale = s0.scale + (s1.scale - s0.scale) * f;
    added.twist = s0.twist + (s1.twist - s0.twist) * f;
    _PushUndoLocked();
    _sections.insert(_sections.begin() + k + 1, std::move(added));
    _useSections = true;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::RemoveSectionRing(int ring)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (ring < 0 || ring >= int(_sections.size()) ||
        _sections.size() <= 2) {
        _diagnostic = "TonicModel::RemoveSectionRing: need >= 2 rings left";
        return false;
    }
    _PushUndoLocked();
    _sections.erase(_sections.begin() + ring);
    _useSections = true;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::CopySectionRing(int src, int dst)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (src < 0 || src >= int(_sections.size()) || dst < 0 ||
        dst >= int(_sections.size())) {
        _diagnostic = "TonicModel::CopySectionRing: ring out of range";
        return false;
    }
    _PushUndoLocked();
    float const t = _sections[size_t(dst)].t;
    _sections[size_t(dst)] = _sections[size_t(src)];
    _sections[size_t(dst)].t = t;
    _useSections = true;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::SetSoftSelection(float center, float radius)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!(radius >= 0.0f)) {
        _diagnostic = "TonicModel::SetSoftSelection: radius must be >= 0";
        return false;
    }
    _softCenter = center;
    _softRadius = radius;
    ++_version;
    return true;
}

void
TonicModel::GetSoftSelection(float *center, float *radius) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (center) {
        *center = _softCenter;
    }
    if (radius) {
        *radius = _softRadius;
    }
}

bool
TonicModel::RelaxCenter(float strength, int iterations)
{
    std::lock_guard<std::mutex> lock(_mutex);
    int const n = int(_host.centerX.size());
    if (!(strength >= 0.0f) || !(strength <= 1.0f) || iterations < 1 ||
        n < 3) {
        _diagnostic = "TonicModel::RelaxCenter: strength in [0, 1], "
                      "iterations >= 1, need >= 3 CVs";
        return false;
    }
    _PushUndoLocked();
    // K13-driven Laplacian smooth of the interior CVs: each CV's share
    // of `strength` ramps with its kink score over [threshold/2,
    // threshold], recomputed per iteration as the curve settles. Quiet
    // CVs (weight 0) are bit-identical no-ops.
    float const lo = 0.5f * kTonicSmoothnessSpike;
    float const span = kTonicSmoothnessSpike - lo;
    for (int it = 0; it < iterations; ++it) {
        std::vector<float> scores(size_t(n), 0.0f);
        std::string derr;
        if (!_LaneSmoothnessScore(HasCudaMirror(), _host.centerX.data(),
                                  _host.centerY.data(), _host.centerZ.data(),
                                  n, scores.data(), &derr)) {
            _diagnostic = derr;
            return false;
        }
        std::vector<float> nx = _host.centerX, ny = _host.centerY,
                            nz = _host.centerZ;
        for (int i = 1; i + 1 < n; ++i) {
            float w =
                (scores[size_t(i)] - lo) / span;
            w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
            float const ax = 0.5f * (_host.centerX[size_t(i - 1)] +
                                     _host.centerX[size_t(i + 1)]);
            float const ay = 0.5f * (_host.centerY[size_t(i - 1)] +
                                     _host.centerY[size_t(i + 1)]);
            float const az = 0.5f * (_host.centerZ[size_t(i - 1)] +
                                     _host.centerZ[size_t(i + 1)]);
            nx[size_t(i)] += (ax - nx[size_t(i)]) * strength * w;
            ny[size_t(i)] += (ay - ny[size_t(i)]) * strength * w;
            nz[size_t(i)] += (az - nz[size_t(i)]) * strength * w;
        }
        _host.centerX = std::move(nx);
        _host.centerY = std::move(ny);
        _host.centerZ = std::move(nz);
    }
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::SnapRootToScalp()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_scalp || !_scalp->finalized || _host.centerX.empty()) {
        _diagnostic = "TonicModel::SnapRootToScalp: need a bound scalp + "
                      "tube";
        return false;
    }
    float const p[3] = {_host.centerX[0], _host.centerY[0], _host.centerZ[0]};
    TonicHit const hit = TonicClosestPointCpu(*_scalp, p);
    if (!hit.hit) {
        _diagnostic = "TonicModel::SnapRootToScalp: no surface point";
        return false;
    }
    _PushUndoLocked();
    float const dx = hit.px - _host.centerX[0];
    float const dy = hit.py - _host.centerY[0];
    float const dz = hit.pz - _host.centerZ[0];
    for (size_t i = 0; i < _host.centerX.size(); ++i) {
        _host.centerX[i] += dx;
        _host.centerY[i] += dy;
        _host.centerZ[i] += dz;
    }
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    ++_version;
    return true;
}

bool
TonicModel::SetDisplaySegments(int segments)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (segments < 1 || segments > 16) {
        _diagnostic = "TonicModel::SetDisplaySegments: segments in [1, 16]";
        return false;
    }
    if (segments == _segmentsPerSpan) {
        return true;
    }
    _segmentsPerSpan = segments;
    if (!_host.positions.empty() && !Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    ++_version;
    return true;
}

int
TonicModel::GetDisplaySegments() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _segmentsPerSpan;
}

void
TonicModel::SetTubeRegionId(int regionId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _tubeRegionId = regionId;
    ++_version;
}

int
TonicModel::GetTubeRegionId() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _tubeRegionId;
}

std::vector<int>
TonicModel::TubeRegionFaces() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<int> faces;
    if (!_scalp || _tubeRegionId < 0 ||
        _tubeRegionId >= _graph.RegionCount()) {
        return faces;
    }
    int const interp = _graph.InterpId(_tubeRegionId);
    for (size_t f = 0; f < _maps.faceRegion.size(); ++f) {
        if (_maps.faceRegion[f] == interp) {
            faces.push_back(int(f));
        }
    }
    return faces;
}

TonicTubeDesc
TonicModel::BuildTubeDesc() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _BuildTubeDescLocked();
}

TonicTubeDesc
TonicModel::_BuildTubeDescLocked() const
{
    TonicTubeDesc desc;
    desc.centerX = _host.centerX;
    desc.centerY = _host.centerY;
    desc.centerZ = _host.centerZ;
    desc.sections = _sections;
    desc.ringVerts = _shape.ringVerts;
    desc.regionId = _tubeRegionId;
    desc.level = 1;
    return desc;
}

std::vector<TonicFrame>
TonicModel::GetFrames() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _frames;
}

// -- P3: Fill mode --------------------------------------------------------

bool
TonicModel::SetPreviewFraction(float fraction)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!(fraction >= 0.0f) || !(fraction <= 1.0f)) {
        _diagnostic = "TonicModel::SetPreviewFraction: fraction in [0, 1]";
        return false;
    }
    _previewFraction = fraction;
    ++_version;
    return true;
}

float
TonicModel::GetPreviewFraction() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _previewFraction;
}

void
TonicModel::SetFreezeRoots(bool freeze)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _freezeRoots = freeze;
    ++_version;
}

bool
TonicModel::GetFreezeRoots() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _freezeRoots;
}

bool
TonicModel::RefillGuides(float fraction)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (fraction < 0.0f) {
        fraction = _previewFraction;
    }
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    if (_host.centerX.size() < 2) {
        _diagnostic = "TonicModel::RefillGuides: no tube";
        return false;
    }
    TonicTubeDesc tube;
    tube.centerX = _host.centerX;
    tube.centerY = _host.centerY;
    tube.centerZ = _host.centerZ;
    tube.sections = _sections;
    tube.ringVerts = _shape.ringVerts;
    tube.regionId = _tubeRegionId;
    std::vector<TonicFrame> frames;
    std::string err;
    if (!TonicCenterFramesCpu(tube.centerX.data(), tube.centerY.data(),
                              tube.centerZ.data(), int(tube.centerX.size()),
                              &frames, &err)) {
        _diagnostic = err;
        return false;
    }
    _frames = frames;
    float const rootRadius =
        TonicSectionMeanRadius(tube.sections.front());
    if (!(rootRadius > 0.0f)) {
        _diagnostic = "TonicModel::RefillGuides: degenerate root section";
        return false;
    }
    float const rootCenter[3] = {tube.centerX[0], tube.centerY[0],
                                 tube.centerZ[0]};
    // Mesh roots when the tube is region-rooted on a bound scalp with
    // claimed faces; otherwise the root-disc stream.
    std::vector<int> regionFaces;
    bool const meshFill =
        _scalp && _scalp->finalized && _tubeRegionId >= 0 &&
        _tubeRegionId < _graph.RegionCount();
    if (meshFill) {
        int const interp = _graph.InterpId(_tubeRegionId);
        for (size_t f = 0; f < _maps.faceRegion.size(); ++f) {
            if (_maps.faceRegion[f] == interp) {
                regionFaces.push_back(int(f));
            }
        }
    }
    bool const useMesh = meshFill && !regionFaces.empty();
    if (!useMesh) {
        int const full =
            TonicGuideCountForDensity(_fill.density);
        int const count = int(float(full) * fraction + 0.5f);
        // Frozen roots keep their stored positions; only the tail is
        // re-sampled, continuing the same dart stream.
        int const frozen =
            _freezeRoots ? std::min(int(_roots.size()), count) : 0;
        std::vector<TonicGuideRoot> roots = _roots;
        if (!TonicRootSampleDiscCpu(0, _fill.seed, rootRadius, rootCenter,
                                    frames[0], count, &roots, frozen, &err)) {
            _diagnostic = err;
            return false;
        }
        _roots = std::move(roots);
    } else {
        // Area-weighted mesh stream at the preview-scaled density. Under
        // freeze the kept prefix seeds the stream, so a frozen full set
        // differs from a from-scratch one (per-face spacing depends on
        // density); frozen roots are sacred by design. The committer and
        // hydrate always run frozen=0, so they agree bit-exactly.
        float const density = _fill.density * fraction;
        int frozen = _freezeRoots ? int(_roots.size()) : 0;
        std::vector<TonicGuideRoot> roots = _roots;
        if (!TonicRootSampleMeshCpu(
                _scalp->points.data(), _scalp->faceVertexCounts.data(),
                _scalp->faceVertexIndices.data(), _scalp->faceOffsets.data(),
                int(_scalp->faceVertexCounts.size()), regionFaces.data(),
                int(regionFaces.size()), density, 0, _fill.seed, rootCenter,
                frames[0], rootRadius, &roots, frozen, &err)) {
            _diagnostic = err;
            return false;
        }
        _roots = std::move(roots);
    }
    TonicGuideSet guides;
    if (!_roots.empty()) {
        TonicFillDesc fd;
        fd.density = _fill.density;
        fd.cvCount = _fill.cvCount;
        fd.seed = _fill.seed;
        fd.edgeBias = _fill.edgeBias;
        fd.lengthProfile = _fill.lengthProfile;
        std::vector<float> filled;
        std::vector<float> lengths;
        if (!TonicGuideFillCpu(tube, frames, _roots, fd, &filled, &lengths,
                               &err)) {
            _diagnostic = err;
            return false;
        }
        std::vector<int> inCounts(_roots.size(), _fill.cvCount);
        float rootDirs[9] = {frames[0].nx, frames[0].ny, frames[0].nz,
                             frames[0].bx, frames[0].by, frames[0].bz,
                             frames[0].tx, frames[0].ty, frames[0].tz};
        std::vector<float> resampled;
        std::vector<int> outCounts;
        std::vector<double> outFrames;
        if (!TonicGuideResampleCpu(filled.data(), inCounts.data(),
                                   int(_roots.size()), _fill.cvCount, rootDirs,
                                   &resampled, &outCounts, &outFrames, &err)) {
            _diagnostic = err;
            return false;
        }
        guides.points = std::move(resampled);
        guides.counts = std::move(outCounts);
        guides.frames = std::move(outFrames);
        guides.ids.resize(_roots.size());
        for (size_t g = 0; g < _roots.size(); ++g) {
            guides.ids[g] = uint64_t(1000 + g);
        }
        guides.guideCount = int(_roots.size());
        guides.cvCount = _fill.cvCount;
    }
    _guides = std::move(guides);
    ++_guideVersion;
    _dirty |= TonicDirty_Guides;
    ++_version;
    return true;
}

TonicModel::GuidePreview
TonicModel::GetGuidePreview() const
{
    // The current set IS the preview: RefillGuides(preview) during a drag
    // leaves the preview-density set here, RefillGuides(1.0) on release
    // leaves the full set (the release frame always shows full fidelity,
    // §7). No second fraction applies here.
    std::lock_guard<std::mutex> lock(_mutex);
    GuidePreview preview;
    preview.points = _guides.points;
    preview.counts = _guides.counts;
    preview.guideCount = _guides.guideCount;
    preview.cvCount = _guides.cvCount;
    return preview;
}

TonicGuideSet const &
TonicModel::GetGuides() const
{
    return _guides;
}

std::vector<TonicGuideRoot> const &
TonicModel::GetRoots() const
{
    return _roots;
}

// -- P3: pick -------------------------------------------------------------

#ifdef USDGEN_TONIC_HAS_CUDA
namespace {

// Heavyweight (tube verts + guide CVs) candidates above this reduce on
// the device; at or below it the launch overhead exceeds the scan, so
// the CPU twin runs (same rule and tie order either way; the lane's
// TN-6 parity is bit-exact on identical inputs).
constexpr int kPickDeviceThreshold = 4096;

// Test hook (P6 precedent): a non-empty USDGEN_TONIC_FORCE_CPU_PICK
// disables the device path so the T0 can prove CPU/device agreement.
bool
CpuPickForced()
{
    char const *forced = std::getenv("USDGEN_TONIC_FORCE_CPU_PICK");
    return forced && *forced;
}

// The TonicPickCpu consider() rule as a fold predicate: `cand` (a
// later kind's winner) replaces `best` on a strictly nearer pixel, or
// an equal pixel at strictly lesser depth. Full ties keep `best`
// (the earlier kind), exactly like the sequential scan.
bool
PickHitBetter(TonicPickHit const &cand, TonicPickHit const &best)
{
    return cand.hit &&
           (!best.hit || cand.distPx < best.distPx ||
            (cand.distPx == best.distPx && cand.depth < best.depth));
}

}  // namespace

bool
TonicModel::_ReducePickLocked(float const *devicePositions, int candidateCount,
                              float const viewProj[16], int w, int h, float x,
                              float y, float radiusPx,
                              TonicDevicePickBest *out) const
{
    static_assert(sizeof(TonicDevicePickBest) == 4 * sizeof(float) &&
                      alignof(TonicDevicePickBest) <= alignof(float),
                  "the pick record must pack into 4 floats");
    if (!devicePositions || !viewProj || !out || candidateCount <= 0 ||
        !_device || !_device->stream) {
        return false;
    }
    int const grid = (candidateCount + 255) / 256;
    int const slices = (grid + 255) / 256;
    // Grow-only: tube/guide reduces alternate sizes every pick, so a
    // plain reset would free+malloc twice per pick. The high-water mark
    // persists (bounded by the largest pick ever served); the kernels
    // only ever write their own prefix.
    if (_device->pickScratch.size() < size_t(grid) * 4 &&
        _device->pickScratch.reset(size_t(grid) * 4) != cudaSuccess) {
        return false;
    }
    if (_device->pickBest.size() < size_t(slices) * 4 &&
        _device->pickBest.reset(size_t(slices) * 4) != cudaSuccess) {
        return false;
    }
    char errBuf[256] = {0};
    if (!TonicLaunchPickReduce(
            devicePositions, candidateCount, viewProj, w, h, x, y, radiusPx,
            reinterpret_cast<TonicDevicePickBest *>(
                _device->pickBest.data()),
            reinterpret_cast<TonicDevicePickBest *>(
                _device->pickScratch.data()),
            _device->stream, errBuf, sizeof(errBuf))) {
        return false;
    }
    if (cudaStreamSynchronize(_device->stream) != cudaSuccess) {
        return false;
    }
    if (cudaMemcpy(out, _device->pickBest.data(),
                   sizeof(TonicDevicePickBest),
                   cudaMemcpyDeviceToHost) != cudaSuccess) {
        return false;
    }
    return true;
}

bool
TonicModel::_UploadGuideCVsLocked(TonicPickSets const &sets) const
{
    if (!sets.guideCVs || sets.guideCount <= 0 || sets.guideCvCount <= 0 ||
        !_device) {
        return false;
    }
    size_t const flat = size_t(sets.guideCount) * size_t(sets.guideCvCount);
    if (flat == 0 || flat > size_t(INT_MAX)) {
        return false;
    }
    if (_device->guideCVsVersion == _guideVersion &&
        _device->guideCVs.size() == flat * 3) {
        return true;  // already uploaded this generation
    }
    if (_device->guideCVs.reset(flat * 3) != cudaSuccess) {
        return false;
    }
    if (cudaMemcpy(_device->guideCVs.data(), sets.guideCVs,
                   flat * 3 * sizeof(float),
                   cudaMemcpyHostToDevice) != cudaSuccess) {
        return false;
    }
    _device->guideCVsVersion = _guideVersion;
    return true;
}

bool
TonicModel::_PickDeviceLocked(TonicPickSets const &sets, uint32_t kindMask,
                              float const viewProj[16], int w, int h, float x,
                              float y, float radiusPx,
                              TonicPickHit *out) const
{
    if (!out || !_device) {
        return false;
    }
    TonicPickHit best;  // kind order below mirrors TonicPickCpu exactly:
                        // tube verts, small kinds, guides.
    if ((kindMask & TonicPick_TubeVert) && sets.tubeVertCount > 0) {
        if (_device->positions.size() !=
            size_t(sets.tubeVertCount) * 3) {
            return false;  // mirror desync: let the CPU twin run instead
        }
        TonicDevicePickBest wb;
        if (!_ReducePickLocked(_device->positions.data(), sets.tubeVertCount,
                               viewProj, w, h, x, y, radiusPx, &wb)) {
            return false;
        }
        if (wb.hit) {
            best.hit = true;
            best.kind = TonicPick_TubeVert;
            best.index = wb.index;
            best.subIndex = -1;
            best.distPx = wb.distPx;
            best.depth = wb.depth;
        }
    }
    {
        // Centers, sections and graph nodes are small-kind CPU work (no
        // device mirror exists for their variable layouts); the twin
        // call preserves their internal order.
        TonicPickSets small = sets;
        small.tubeVerts = nullptr;
        small.tubeVertCount = 0;
        small.guideCVs = nullptr;
        small.guideCount = 0;
        small.guideCvCount = 0;
        TonicPickHit folded =
            TonicPickCpu(small, kindMask, viewProj, w, h, x, y, radiusPx);
        if (PickHitBetter(folded, best)) {
            best = folded;
        }
    }
    if ((kindMask & TonicPick_Guide) && sets.guideCount > 0 &&
        sets.guideCvCount > 0) {
        size_t const flat =
            size_t(sets.guideCount) * size_t(sets.guideCvCount);
        if (flat == 0 || flat > size_t(INT_MAX)) {
            return false;
        }
        if (!_UploadGuideCVsLocked(sets)) {
            return false;
        }
        TonicDevicePickBest wb;
        if (!_ReducePickLocked(_device->guideCVs.data(), int(flat), viewProj,
                               w, h, x, y, radiusPx, &wb)) {
            return false;
        }
        if (wb.hit) {
            TonicPickHit folded;
            folded.hit = true;
            folded.kind = TonicPick_Guide;
            folded.index = wb.index / sets.guideCvCount;
            folded.subIndex = wb.index % sets.guideCvCount;
            folded.distPx = wb.distPx;
            folded.depth = wb.depth;
            if (PickHitBetter(folded, best)) {
                best = folded;
            }
        }
    }
    *out = best;
    return true;
}
#endif  // USDGEN_TONIC_HAS_CUDA

TonicPickSets
TonicModel::_BuildPickSetsLocked(_PickScratch *scratch) const
{
    TonicPickSets sets;
    if (!scratch) {
        return sets;
    }
    if (!_host.positions.empty()) {
        sets.tubeVerts = _host.positions.data();
        sets.tubeVertCount = int(_host.positions.size() / 3);
    }
    if (!_host.centerX.empty()) {
        scratch->center.resize(_host.centerX.size() * 3);
        for (size_t i = 0; i < _host.centerX.size(); ++i) {
            scratch->center[i * 3 + 0] = _host.centerX[i];
            scratch->center[i * 3 + 1] = _host.centerY[i];
            scratch->center[i * 3 + 2] = _host.centerZ[i];
        }
        sets.centerCVs = scratch->center.data();
        sets.centerCVCount = int(_host.centerX.size());
    }
    if (!_sections.empty() && !_frames.empty() &&
        _frames.size() == _host.centerX.size()) {
        // Section CVs in world space (root ring placed through the K5
        // interpolation at each section t; twist/scale applied).
        scratch->section.reserve(_sections.size() *
                                 _sections.front().u.size() * 3);
        for (auto const &s : _sections) {
            float cp[3];
            TonicFrame fr;
            cp[0] = cp[1] = cp[2] = 0.0f;
            TonicEvalCenter(_host.centerX.data(), _host.centerY.data(),
                            _host.centerZ.data(), int(_host.centerX.size()),
                            s.t, cp);
            TonicNlerpFrame(_frames.data(), int(_frames.size()), s.t, &fr);
            float const ct = std::cos(s.twist), st = std::sin(s.twist);
            double rx = 0.0, ry = 0.0, rz = 0.0;
            for (size_t i = 0; i < s.u.size(); ++i) {
                float const uu = s.u[i] * s.scale;
                float const vv = s.v[i] * s.scale;
                float const ru = uu * ct - vv * st;
                float const rvv = uu * st + vv * ct;
                float const px = cp[0] + fr.nx * ru + fr.bx * rvv;
                float const py = cp[1] + fr.ny * ru + fr.by * rvv;
                float const pz = cp[2] + fr.nz * ru + fr.bz * rvv;
                scratch->section.push_back(px);
                scratch->section.push_back(py);
                scratch->section.push_back(pz);
                rx += px;
                ry += py;
                rz += pz;
            }
            // The ring's own candidate is its centroid: a marquee should
            // catch a ring when its centre is inside the band, not when
            // one vertex clips a corner.
            double const n = double(s.u.size() ? s.u.size() : 1);
            scratch->ringCenters.push_back(float(rx / n));
            scratch->ringCenters.push_back(float(ry / n));
            scratch->ringCenters.push_back(float(rz / n));
        }
        sets.sectionCVs = scratch->section.data();
        sets.sectionCVCount = int(scratch->section.size() / 3);
        sets.sectionRingVerts = int(_sections.front().u.size());
        sets.ringCenters = scratch->ringCenters.data();
        sets.ringCount = int(scratch->ringCenters.size() / 3);
    }
    for (auto const &nd : _graph.Nodes()) {
        if (nd.alive) {
            scratch->nodes.push_back(nd.p[0]);
            scratch->nodes.push_back(nd.p[1]);
            scratch->nodes.push_back(nd.p[2]);
            scratch->nodeIds.push_back(nd.id);
        }
    }
    if (!scratch->nodes.empty()) {
        sets.graphNodes = scratch->nodes.data();
        sets.graphNodeCount = int(scratch->nodes.size() / 3);
    }
    for (auto const &e : _graph.Edges()) {
        if (!e.alive || e.polyline.size() < 6) {
            continue;
        }
        int const n = int(e.polyline.size() / 3);
        for (int i = 0; i < n; ++i) {
            scratch->edgeCVs.push_back(e.polyline[size_t(i) * 3 + 0]);
            scratch->edgeCVs.push_back(e.polyline[size_t(i) * 3 + 1]);
            scratch->edgeCVs.push_back(e.polyline[size_t(i) * 3 + 2]);
            scratch->edgeIds.push_back(e.id);
        }
    }
    if (!scratch->edgeIds.empty()) {
        sets.graphEdgeCVs = scratch->edgeCVs.data();
        sets.graphEdgeIds = scratch->edgeIds.data();
        sets.graphEdgeCVCount = int(scratch->edgeIds.size());
    }
    for (auto const &r : _graph.Regions()) {
        if (r.isOutside || r.boundary.size() < 9) {
            continue;
        }
        double cx = 0.0, cy = 0.0, cz = 0.0;
        size_t const n = r.boundary.size() / 3;
        for (size_t i = 0; i < n; ++i) {
            cx += r.boundary[i * 3 + 0];
            cy += r.boundary[i * 3 + 1];
            cz += r.boundary[i * 3 + 2];
        }
        scratch->regionCenters.push_back(float(cx / double(n)));
        scratch->regionCenters.push_back(float(cy / double(n)));
        scratch->regionCenters.push_back(float(cz / double(n)));
        scratch->regionIds.push_back(r.id);
    }
    if (!scratch->regionIds.empty()) {
        sets.regionCenters = scratch->regionCenters.data();
        sets.regionIds = scratch->regionIds.data();
        sets.regionCount = int(scratch->regionIds.size());
    }
    if (!_guides.points.empty() && _guides.cvCount > 0) {
        sets.guideCVs = _guides.points.data();
        sets.guideCount = _guides.guideCount;
        sets.guideCvCount = _guides.cvCount;
    }
    return sets;
}

TonicPickHit
TonicModel::Pick(float const viewProj[16], int w, int h, float x, float y,
                 float radiusPx, uint32_t kindMask) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    _PickScratch scratch;
    TonicPickSets const sets = _BuildPickSetsLocked(&scratch);
    // The V1 kinds (edge, region, ring) fold in after the five the shared
    // twin scans, with that twin's rule, so an exact dead heat still goes
    // to the earlier kind.
    TonicPickHit const extra = TonicPickExtraKindsCpu(
        sets, kindMask, viewProj, w, h, x, y, radiusPx);
#ifdef USDGEN_TONIC_HAS_CUDA
    if (_device && _device->streamOwned && _device->stream &&
        !CpuPickForced()) {
        size_t heavy = 0;
        if (kindMask & TonicPick_TubeVert) {
            heavy += size_t(std::max(sets.tubeVertCount, 0));
        }
        if (kindMask & TonicPick_Guide) {
            heavy += size_t(std::max(sets.guideCount, 0)) *
                     size_t(std::max(sets.guideCvCount, 0));
        }
        if (heavy > size_t(kPickDeviceThreshold)) {
            TonicPickHit hit;
            if (_PickDeviceLocked(sets, kindMask, viewProj, w, h, x, y,
                                  radiusPx, &hit)) {
                return PickHitBetter(extra, hit) ? extra : hit;
            }
            // Any device failure falls through to the CPU twin (P6
            // style, but non-sticky: the mirror itself is untouched).
        }
    }
#else
    (void)0;
#endif
    TonicPickHit const base =
        TonicPickCpu(sets, kindMask, viewProj, w, h, x, y, radiusPx);
    return PickHitBetter(extra, base) ? extra : base;
}

// -- V1: selection (plan/18 §2.3) --------------------------------------------

std::vector<int>
TonicModel::_LiveTubeIdsLocked() const
{
    std::vector<int> ids;
    if (_host.centerX.size() >= 2) {
        ids.push_back(0);
    }
    for (auto const &kv : _tubes) {
        ids.push_back(kv.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

TonicSelectionItem
TonicModel::_ItemFromCandidateLocked(_PickScratch const &scratch,
                                     uint32_t kind, int index,
                                     int subIndex) const
{
    TonicSelectionItem item;
    item.kind = kind;
    switch (kind) {
    case TonicPick_TubeVert:
        // A surface hit selects the tube it belongs to. Only tube 0 has a
        // vertex mirror today (the audit's G2 tube-0 limitation), so every
        // surface candidate is tube 0's.
        item.id = 0;
        break;
    case TonicPick_CenterCV:
        item.id = 0;
        item.subId = index;
        break;
    case TonicPick_SectionCV:
        item.id = 0;
        item.subId = index;     // ring
        item.subSubId = subIndex;  // slot
        break;
    case TonicPick_SectionRing:
        item.id = 0;
        item.subId = index;
        break;
    case TonicPick_GraphNode:
        item.id = index >= 0 && index < int(scratch.nodeIds.size())
                      ? scratch.nodeIds[size_t(index)]
                      : -1;
        break;
    case TonicPick_GraphEdge:
    case TonicPick_Region:
        item.id = index;  // already the stable id (the sets carry it)
        break;
    case TonicPick_Guide:
        item.id = index;
        break;
    default:
        item.kind = 0;
        break;
    }
    return item;
}

TonicSelectionItem
TonicModel::SelectionItemFromHit(TonicPickHit const &hit) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!hit.hit) {
        return TonicSelectionItem();
    }
    _PickScratch scratch;
    _BuildPickSetsLocked(&scratch);
    return _ItemFromCandidateLocked(scratch, hit.kind, hit.index,
                                    hit.subIndex);
}

void
TonicModel::SelectionClear(uint32_t kindMask)
{
    std::lock_guard<std::mutex> lock(_mutex);
    uint64_t const before = _selection.Generation();
    _selection.Clear(kindMask);
    if (_selection.Generation() != before) {
        _dirty |= TonicDirty_Selection;
        ++_version;
    }
}

void
TonicModel::SelectionApply(TonicSelectMode mode,
                           std::vector<TonicSelectionItem> const &items)
{
    std::lock_guard<std::mutex> lock(_mutex);
    uint64_t const before = _selection.Generation();
    _selection.Apply(mode, items);
    if (_selection.Generation() != before) {
        _dirty |= TonicDirty_Selection;
        ++_version;
    }
}

void
TonicModel::SelectionSetHover(TonicSelectionItem const &item)
{
    std::lock_guard<std::mutex> lock(_mutex);
    uint64_t const before = _selection.Generation();
    _selection.SetHover(item);
    if (_selection.Generation() != before) {
        _dirty |= TonicDirty_Selection;
        ++_version;
    }
}

TonicSelectionItem
TonicModel::SelectionHover() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    _selection.PruneTubes(_LiveTubeIdsLocked());
    return _selection.Hover();
}

std::vector<TonicSelectionItem>
TonicModel::SelectionItems(uint32_t kindMask) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    _selection.PruneTubes(_LiveTubeIdsLocked());
    return _selection.Items(kindMask);
}

size_t
TonicModel::SelectionCount(uint32_t kindMask) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    _selection.PruneTubes(_LiveTubeIdsLocked());
    return _selection.Count(kindMask);
}

uint64_t
TonicModel::SelectionGeneration() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    _selection.PruneTubes(_LiveTubeIdsLocked());
    return _selection.Generation();
}

bool
TonicModel::_ItemPositionLocked(TonicSelectionItem const &item,
                                _PickScratch const &scratch,
                                float out[3]) const
{
    auto tubeCentroid = [&](int tubeId, float o[3]) {
        TonicTubeDesc desc;
        if (!_TubeDescLocked(tubeId, &desc) || desc.centerX.empty()) {
            return false;
        }
        double cx = 0.0, cy = 0.0, cz = 0.0;
        for (size_t i = 0; i < desc.centerX.size(); ++i) {
            cx += desc.centerX[i];
            cy += desc.centerY[i];
            cz += desc.centerZ[i];
        }
        double const n = double(desc.centerX.size());
        o[0] = float(cx / n);
        o[1] = float(cy / n);
        o[2] = float(cz / n);
        return true;
    };
    switch (item.kind) {
    case TonicPick_TubeVert:
        return tubeCentroid(item.id, out);
    case TonicPick_CenterCV: {
        TonicTubeDesc desc;
        if (!_TubeDescLocked(item.id, &desc) || item.subId < 0 ||
            item.subId >= int(desc.centerX.size())) {
            return false;
        }
        out[0] = desc.centerX[size_t(item.subId)];
        out[1] = desc.centerY[size_t(item.subId)];
        out[2] = desc.centerZ[size_t(item.subId)];
        return true;
    }
    case TonicPick_SectionCV: {
        if (item.id != 0 || scratch.ringVerts <= 0 || item.subId < 0 ||
            item.subSubId < 0) {
            return false;
        }
        size_t const flat = size_t(item.subId) * size_t(scratch.ringVerts) +
                            size_t(item.subSubId);
        if (flat * 3 + 2 >= scratch.section.size()) {
            return false;
        }
        out[0] = scratch.section[flat * 3 + 0];
        out[1] = scratch.section[flat * 3 + 1];
        out[2] = scratch.section[flat * 3 + 2];
        return true;
    }
    case TonicPick_SectionRing: {
        if (item.id != 0 || item.subId < 0) {
            return false;
        }
        size_t const o = size_t(item.subId) * 3;
        if (o + 2 >= scratch.ringCenters.size()) {
            return false;
        }
        out[0] = scratch.ringCenters[o + 0];
        out[1] = scratch.ringCenters[o + 1];
        out[2] = scratch.ringCenters[o + 2];
        return true;
    }
    case TonicPick_GraphNode: {
        TonicGraphNode const *node = _graph.FindNode(item.id);
        if (!node || !node->alive) {
            return false;
        }
        out[0] = node->p[0];
        out[1] = node->p[1];
        out[2] = node->p[2];
        return true;
    }
    case TonicPick_GraphEdge: {
        TonicGraphEdge const *edge = _graph.FindEdge(item.id);
        if (!edge || !edge->alive || edge->polyline.size() < 6) {
            return false;
        }
        size_t const mid = (edge->polyline.size() / 3) / 2;
        out[0] = edge->polyline[mid * 3 + 0];
        out[1] = edge->polyline[mid * 3 + 1];
        out[2] = edge->polyline[mid * 3 + 2];
        return true;
    }
    case TonicPick_Region: {
        for (size_t i = 0; i < scratch.regionIds.size(); ++i) {
            if (scratch.regionIds[i] == item.id) {
                out[0] = scratch.regionCenters[i * 3 + 0];
                out[1] = scratch.regionCenters[i * 3 + 1];
                out[2] = scratch.regionCenters[i * 3 + 2];
                return true;
            }
        }
        return false;
    }
    case TonicPick_Guide: {
        if (_guides.cvCount <= 0 || item.id < 0 ||
            item.id >= _guides.guideCount) {
            return false;
        }
        size_t const root = size_t(item.id) * size_t(_guides.cvCount) * 3;
        if (root + 2 >= _guides.points.size()) {
            return false;
        }
        out[0] = _guides.points[root + 0];
        out[1] = _guides.points[root + 1];
        out[2] = _guides.points[root + 2];
        return true;
    }
    case TonicPick_Level: {
        // A level's position is the centroid of the tubes at it, which is
        // where a "frame this level" gizmo belongs.
        double cx = 0.0, cy = 0.0, cz = 0.0;
        int n = 0;
        for (int id : _LiveTubeIdsLocked()) {
            TonicTubeDesc desc;
            if (!_TubeDescLocked(id, &desc) || desc.level != item.id) {
                continue;
            }
            float c[3];
            if (tubeCentroid(id, c)) {
                cx += c[0];
                cy += c[1];
                cz += c[2];
                ++n;
            }
        }
        if (!n) {
            return false;
        }
        out[0] = float(cx / n);
        out[1] = float(cy / n);
        out[2] = float(cz / n);
        return true;
    }
    default:
        return false;
    }
}

bool
TonicModel::SelectionBounds(float outMin[3], float outMax[3]) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!outMin || !outMax) {
        return false;
    }
    _selection.PruneTubes(_LiveTubeIdsLocked());
    _PickScratch scratch;
    _BuildPickSetsLocked(&scratch);
    bool any = false;
    float mn[3] = {0.0f, 0.0f, 0.0f};
    float mx[3] = {0.0f, 0.0f, 0.0f};
    for (TonicSelectionItem const &item : _selection.Items(0)) {
        float p[3];
        if (!_ItemPositionLocked(item, scratch, p)) {
            continue;
        }
        for (int a = 0; a < 3; ++a) {
            mn[a] = any ? std::min(mn[a], p[a]) : p[a];
            mx[a] = any ? std::max(mx[a], p[a]) : p[a];
        }
        any = true;
    }
    if (!any) {
        return false;
    }
    for (int a = 0; a < 3; ++a) {
        outMin[a] = mn[a];
        outMax[a] = mx[a];
    }
    return true;
}

bool
TonicModel::_ApplyRegionSelectLocked(
    std::vector<TonicPickCandidate> const &hits, _PickScratch const &scratch,
    TonicSelectMode mode, uint32_t kindMask)
{
    std::vector<TonicSelectionItem> items;
    items.reserve(hits.size());
    for (TonicPickCandidate const &hit : hits) {
        TonicSelectionItem const item =
            _ItemFromCandidateLocked(scratch, hit.kind, hit.index,
                                     hit.subIndex);
        if (item.kind) {
            items.push_back(item);
        }
    }
    uint64_t const before = _selection.Generation();
    if (mode == TonicSelect_Set) {
        // An empty band clears the kinds it was asked for: dragging over
        // nothing is how an artist deselects.
        _selection.Clear(kindMask);
    }
    _selection.Apply(mode == TonicSelect_Set ? TonicSelect_Add : mode, items);
    if (_selection.Generation() == before) {
        return false;
    }
    _dirty |= TonicDirty_Selection;
    ++_version;
    return true;
}

#ifdef USDGEN_TONIC_HAS_CUDA
bool
TonicModel::_SelectMaskDeviceLocked(float const *devicePositions,
                                    int candidateCount,
                                    float const viewProj[16], int w, int h,
                                    float x0, float y0, float x1, float y1,
                                    float const *xy, int pointCount,
                                    std::vector<unsigned char> *out) const
{
    if (!devicePositions || candidateCount <= 0 || !out || !_device ||
        !_device->stream) {
        return false;
    }
    if (_device->selectMask.size() < size_t(candidateCount) &&
        _device->selectMask.reset(size_t(candidateCount)) != cudaSuccess) {
        return false;
    }
    float const *devicePolygon = nullptr;
    if (xy && pointCount >= 3) {
        size_t const floats = size_t(pointCount) * 2;
        if (_device->selectPolygon.size() < floats &&
            _device->selectPolygon.reset(floats) != cudaSuccess) {
            return false;
        }
        if (cudaMemcpy(_device->selectPolygon.data(), xy,
                       floats * sizeof(float),
                       cudaMemcpyHostToDevice) != cudaSuccess) {
            return false;
        }
        devicePolygon = _device->selectPolygon.data();
    }
    char errBuf[256] = {0};
    if (!TonicLaunchSelectMask(devicePositions, candidateCount, viewProj, w,
                               h, x0, y0, x1, y1, devicePolygon,
                               devicePolygon ? pointCount : 0,
                               _device->selectMask.data(), _device->stream,
                               errBuf, sizeof(errBuf))) {
        return false;
    }
    if (cudaStreamSynchronize(_device->stream) != cudaSuccess) {
        return false;
    }
    out->resize(size_t(candidateCount));
    return cudaMemcpy(out->data(), _device->selectMask.data(),
                      size_t(candidateCount) * sizeof(unsigned char),
                      cudaMemcpyDeviceToHost) == cudaSuccess;
}

void
TonicModel::_MaskHeavyKindsLocked(TonicPickSets const &sets,
                                  uint32_t kindMask,
                                  float const viewProj[16], int w, int h,
                                  float x0, float y0, float x1, float y1,
                                  float const *xy, int pointCount,
                                  std::vector<unsigned char> *tubeMask,
                                  std::vector<unsigned char> *guideMask,
                                  unsigned char const **tubePtr,
                                  unsigned char const **guidePtr) const
{
    *tubePtr = nullptr;
    *guidePtr = nullptr;
    if (!_device || !_device->streamOwned || !_device->stream ||
        CpuPickForced()) {
        return;
    }
    if ((kindMask & TonicPick_TubeVert) &&
        sets.tubeVertCount > kPickDeviceThreshold &&
        _device->positions.size() == size_t(sets.tubeVertCount) * 3) {
        if (_SelectMaskDeviceLocked(_device->positions.data(),
                                    sets.tubeVertCount, viewProj, w, h, x0,
                                    y0, x1, y1, xy, pointCount, tubeMask)) {
            *tubePtr = tubeMask->data();
        }
    }
    if ((kindMask & TonicPick_Guide) && sets.guideCount > 0 &&
        sets.guideCvCount > 0) {
        size_t const flat =
            size_t(sets.guideCount) * size_t(sets.guideCvCount);
        if (flat > size_t(kPickDeviceThreshold) && flat <= size_t(INT_MAX) &&
            _UploadGuideCVsLocked(sets) &&
            _SelectMaskDeviceLocked(_device->guideCVs.data(), int(flat),
                                    viewProj, w, h, x0, y0, x1, y1, xy,
                                    pointCount, guideMask)) {
            *guidePtr = guideMask->data();
        }
    }
}
#endif

bool
TonicModel::SelectRect(float const viewProj[16], int w, int h, float x0,
                       float y0, float x1, float y1, uint32_t kindMask,
                       TonicSelectMode mode)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!viewProj || w <= 0 || h <= 0) {
        _diagnostic = "TonicModel::SelectRect: bad viewport";
        return false;
    }
    _selection.PruneTubes(_LiveTubeIdsLocked());
    _PickScratch scratch;
    TonicPickSets const sets = _BuildPickSetsLocked(&scratch);
    std::vector<unsigned char> tubeMask;
    std::vector<unsigned char> guideMask;
    unsigned char const *tubePtr = nullptr;
    unsigned char const *guidePtr = nullptr;
#ifdef USDGEN_TONIC_HAS_CUDA
    _MaskHeavyKindsLocked(sets, kindMask, viewProj, w, h, x0, y0, x1, y1,
                          nullptr, 0, &tubeMask, &guideMask, &tubePtr,
                          &guidePtr);
#endif
    std::vector<TonicPickCandidate> hits;
    if (!TonicSelectRectCpu(sets, kindMask, viewProj, w, h, x0, y0, x1, y1,
                            tubePtr, guidePtr, &hits)) {
        _diagnostic = "TonicModel::SelectRect: bad arguments";
        return false;
    }
    _ApplyRegionSelectLocked(hits, scratch, mode, kindMask);
    return true;
}

bool
TonicModel::SelectPolygon(float const viewProj[16], int w, int h,
                          float const *xy, int pointCount, uint32_t kindMask,
                          TonicSelectMode mode)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!viewProj || w <= 0 || h <= 0 || !xy || pointCount < 3) {
        _diagnostic = "TonicModel::SelectPolygon: a lasso needs >= 3 points";
        return false;
    }
    _selection.PruneTubes(_LiveTubeIdsLocked());
    _PickScratch scratch;
    TonicPickSets const sets = _BuildPickSetsLocked(&scratch);
    std::vector<unsigned char> tubeMask;
    std::vector<unsigned char> guideMask;
    unsigned char const *tubePtr = nullptr;
    unsigned char const *guidePtr = nullptr;
#ifdef USDGEN_TONIC_HAS_CUDA
    _MaskHeavyKindsLocked(sets, kindMask, viewProj, w, h, 0.0f, 0.0f, 0.0f,
                          0.0f, xy, pointCount, &tubeMask, &guideMask,
                          &tubePtr, &guidePtr);
#endif
    std::vector<TonicPickCandidate> hits;
    if (!TonicSelectPolygonCpu(sets, kindMask, viewProj, w, h, xy, pointCount,
                               tubePtr, guidePtr, &hits)) {
        _diagnostic = "TonicModel::SelectPolygon: bad arguments";
        return false;
    }
    _ApplyRegionSelectLocked(hits, scratch, mode, kindMask);
    return true;
}

// -- V1: gizmo and brush overlays (plan/18 §2.4) -----------------------------

bool
TonicModel::SetGizmo(TonicGizmoRecord const &record)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (record.kind < TonicGizmo_None ||
        record.kind > TonicGizmo_NodeTranslate) {
        _diagnostic = "TonicModel::SetGizmo: unknown gizmo kind";
        return false;
    }
    if (_gizmo == record) {
        return true;  // the move loop re-sets an unchanged gizmo per sample
    }
    _gizmo = record;
    _dirty |= TonicDirty_Gizmo;
    ++_version;
    return true;
}

TonicGizmoRecord
TonicModel::GetGizmo() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _gizmo;
}

bool
TonicModel::SetBrushRing(TonicBrushRingRecord const &record)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_brush == record) {
        return true;
    }
    _brush = record;
    _dirty |= TonicDirty_Brush;
    ++_version;
    return true;
}

TonicBrushRingRecord
TonicModel::GetBrushRing() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _brush;
}

// -- P4: hierarchy store -----------------------------------------------------

namespace {

// Ring-layout match: same ringVerts, section count and t values. K7
// aggregates carry the child layout; writeback refits them to the target
// tube's own layout -- EXCEPT on a match, where the aggregate passes
// through untouched so hint-exact merges stay bit-exact.
bool _SameRingLayout(TonicTubeDesc const &a, TonicTubeDesc const &b)
{
    if (a.ringVerts != b.ringVerts ||
        a.sections.size() != b.sections.size()) {
        return false;
    }
    for (size_t i = 0; i < a.sections.size(); ++i) {
        if (a.sections[i].t != b.sections[i].t) {
            return false;
        }
    }
    return true;
}

bool _FitAggregateForWriteback(TonicTubeDesc const &agg,
                               TonicTubeDesc const &current,
                               TonicTubeDesc *out, std::string *derr)
{
    if (_SameRingLayout(agg, current)) {
        *out = agg;
        return true;
    }
    return TonicResampleDescRingsCpu(agg, current.ringVerts, out, derr);
}

}  // namespace

bool TonicModel::_TubeDescLocked(int tubeId, TonicTubeDesc *out) const
{
    if (!out) {
        return false;
    }
    if (tubeId == 0) {
        if (_host.centerX.size() < 2) {
            return false;
        }
        *out = _BuildTubeDescLocked();
        out->tubeId = 0;
        out->level = 1;
        out->regionId = _tubeRegionId;
        out->parentTubeId = -1;
        out->childIndex = -1;
        return true;
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return false;
    }
    *out = it->second.actual;
    return true;
}

bool TonicModel::_SetCenterCVLocked(int cv, float x, float y, float z)
{
    int const n = int(_host.centerX.size());
    if (cv < 0 || cv >= n || n < 2) {
        _diagnostic = "TonicModel: CV index out of range";
        return false;
    }
    _host.centerX[size_t(cv)] = x;
    _host.centerY[size_t(cv)] = y;
    _host.centerZ[size_t(cv)] = z;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Points;
    return true;
}

bool TonicModel::_WriteDescToTube0Locked(TonicTubeDesc const &desc)
{
    int const nCv = int(desc.centerX.size());
    if (nCv < 2 || desc.sections.size() < 2 || desc.ringVerts < 3 ||
        desc.ringVerts > 32) {
        _diagnostic = "TonicModel: merged desc fails tube-0 limits";
        return false;
    }
    _shape.rings = nCv;
    _shape.ringVerts = desc.ringVerts;
    _host.centerX = desc.centerX;
    _host.centerY = desc.centerY;
    _host.centerZ = desc.centerZ;
    _sections = desc.sections;
    _useSections = true;
    _roots.clear();
    _guides = TonicGuideSet();
    ++_guideVersion;
    if (!Sync()) {
        return false;
    }
    _dirty |= TonicDirty_Topology;
    return true;
}

bool TonicModel::_PropagateDownLocked(int tubeId)
{
    // Children in ascending id order (map order is deterministic).
    std::vector<int> kids;
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == tubeId) {
            kids.push_back(kv.first);
        }
    }
    if (kids.empty()) {
        return true;
    }
    TonicTubeDesc parentDesc;
    if (!_TubeDescLocked(tubeId, &parentDesc)) {
        return false;
    }
    std::vector<TonicFrame> frames;
    std::string derr;
    if (!TonicCenterFramesCpu(parentDesc.centerX.data(),
                              parentDesc.centerY.data(),
                              parentDesc.centerZ.data(),
                              int(parentDesc.centerX.size()), &frames,
                              &derr)) {
        _diagnostic = derr;
        return false;
    }
    // The params this parent split with (tube 0: the model params).
    TonicSubdivideDesc params;
    if (tubeId == 0) {
        params.count = _subdivide.count;
        params.seed = _subdivide.seed;
        params.splitMode =
            _subdivide.splitMode == "edge" ? TonicSplit_Edge : TonicSplit_KMeans;
    } else {
        auto pit = _tubes.find(tubeId);
        if (pit == _tubes.end()) {
            return false;
        }
        params = pit->second.subdivide;
    }
    // TN-1: derive ALL children with one subdivision (was: a full
    // subdivide per child, twice — inside the sculpt and again for the
    // derived refresh). Same deterministic function, same inputs, so
    // the per-child descs — and everything downstream — are
    // bit-identical; only the redundant recomputation is gone.
    std::vector<TonicTubeDesc> derivedAll;
    if (!TonicSubdivideTubeCpu(parentDesc, frames, params, &derivedAll,
                               &derr)) {
        _diagnostic = derr;
        return false;
    }
    for (int kid : kids) {
        auto it = _tubes.find(kid);
        if (it == _tubes.end()) {
            return false;
        }
        HierarchyTube &entry = it->second;
        if (entry.imported) {
            continue;  // bridge imports keep their explicit shape: K6
                       // never re-derives them (and their subtree, rooted
                       // at an unchanged actual, cannot move either)
        }
        int const childIndex = entry.actual.childIndex;
        if (childIndex < 0 ||
            size_t(childIndex) >= derivedAll.size()) {
            // Same spelling the per-child derive used to report.
            _diagnostic = "TonicDeriveChildCpu: child index out of range";
            return false;
        }
        bool const locked = _globalLockChildren || entry.lockChildren;
        TonicTubeDesc actual;
        TonicShapeDeltas stored;
        if (!TonicHierarchicalSculptApplyCpu(
                derivedAll[size_t(childIndex)], entry.actual, entry.derived,
                entry.deltas, locked, /*preserveLength=*/true, &actual,
                &stored, &derr)) {
            _diagnostic = derr;
            return false;
        }
        entry.actual = actual;
        entry.derived = derivedAll[size_t(childIndex)];
        entry.deltas = stored;
        if (!_PropagateDownLocked(kid)) {
            return false;
        }
    }
    return true;
}

bool TonicModel::_PropagateUpLocked(int tubeId)
{
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return true;  // tube 0 has no parent
    }
    int const parentId = it->second.actual.parentTubeId;
    if (parentId < 0) {
        return true;
    }
    if (_globalLockParents || it->second.lockParents) {
        return true;  // the edited tube gates the whole upward chain
    }
    // Gather the parent's children actuals in ascending id order.
    std::vector<TonicTubeDesc> sibs;
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == parentId) {
            sibs.push_back(kv.second.actual);
        }
    }
    if (sibs.empty()) {
        return true;
    }
    TonicTubeDesc parentDesc;
    if (!_TubeDescLocked(parentId, &parentDesc)) {
        return false;
    }
    TonicSubdivideDesc params;
    if (parentId == 0) {
        params.count = _subdivide.count;
        params.seed = _subdivide.seed;
        params.splitMode =
            _subdivide.splitMode == "edge" ? TonicSplit_Edge : TonicSplit_KMeans;
    } else {
        auto pit = _tubes.find(parentId);
        if (pit == _tubes.end()) {
            return false;
        }
        if (pit->second.imported) {
            // Bridge imports keep their explicit shape: K7 reads them
            // (as siblings, above) but never overwrites them. The chain
            // still continues upward so ancestors refresh against the
            // import's unchanged actual.
            return _PropagateUpLocked(parentId);
        }
        params = pit->second.subdivide;
    }
    std::string derr;
    TonicTubeDesc agg;
    if (!TonicMergeTubesCpu(sibs, params, &parentDesc, &agg, &derr)) {
        _diagnostic = derr;
        return false;
    }
    if (parentId == 0) {
        // Keep tube 0 on its own ring layout (a layout change here would
        // desync every later K6 re-derivation); pass hint-exact merges
        // through untouched.
        TonicTubeDesc fit;
        if (!_FitAggregateForWriteback(agg, parentDesc, &fit, &derr)) {
            _diagnostic = derr;
            return false;
        }
        if (!_WriteDescToTube0Locked(fit)) {
            return false;
        }
        return true;
    }
    auto pit = _tubes.find(parentId);
    if (pit == _tubes.end()) {
        return false;
    }
    // Refit the aggregate to the parent's own ring layout, then refresh
    // its deltas against its derived baseline (hint-exact merges pass
    // through untouched).
    TonicTubeDesc refit;
    if (!_FitAggregateForWriteback(agg, pit->second.actual, &refit, &derr)) {
        _diagnostic = derr;
        return false;
    }
    refit.tubeId = pit->second.actual.tubeId;
    refit.regionId = pit->second.actual.regionId;
    refit.level = pit->second.actual.level;
    refit.parentTubeId = pit->second.actual.parentTubeId;
    refit.childIndex = pit->second.actual.childIndex;
    std::vector<TonicFrame> dframes;
    if (!TonicCenterFramesCpu(pit->second.derived.centerX.data(),
                              pit->second.derived.centerY.data(),
                              pit->second.derived.centerZ.data(),
                              int(pit->second.derived.centerX.size()),
                              &dframes, &derr)) {
        _diagnostic = derr;
        return false;
    }
    TonicShapeDeltas deltas;
    if (!TonicComputeDeltasCpu(refit, pit->second.derived, dframes, &deltas,
                               &derr)) {
        _diagnostic = derr;
        return false;
    }
    pit->second.actual = refit;
    pit->second.deltas = deltas;
    return _PropagateUpLocked(parentId);
}

bool TonicModel::SubdivideTube(int tubeId, int count, const char *splitMode,
                               int seed, std::vector<int> *outChildren)
{
    return SubdivideTube(tubeId, count, splitMode, seed, 1.0f, 0.0f, 0.0f,
                         outChildren);
}

bool TonicModel::SubdivideTube(int tubeId, int count, const char *splitMode,
                               int seed, float edgeA, float edgeB,
                               float edgeC, std::vector<int> *outChildren)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!outChildren) {
        _diagnostic = "TonicModel::SubdivideTube: null output";
        return false;
    }
    TonicSubdivideDesc params;
    params.count = count;
    params.seed = seed;
    params.edgeA = edgeA;
    params.edgeB = edgeB;
    params.edgeC = edgeC;
    std::string mode = splitMode ? splitMode : "kmeans";
    params.splitMode =
        mode == "edge" ? TonicSplit_Edge : TonicSplit_KMeans;
    if (mode != "kmeans" && mode != "edge") {
        _diagnostic = "TonicModel::SubdivideTube: splitMode kmeans | edge";
        return false;
    }
    std::string verr;
    if (!TonicValidateSubdivide(params, &verr)) {
        _diagnostic = verr;
        return false;
    }
    TonicTubeDesc desc;
    if (!_TubeDescLocked(tubeId, &desc)) {
        _diagnostic = "TonicModel::SubdivideTube: unknown tube";
        return false;
    }
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == tubeId) {
            _diagnostic =
                "TonicModel::SubdivideTube: tube already has children "
                "(merge or re-subdivide instead)";
            return false;
        }
    }
    std::vector<TonicFrame> frames;
    std::string derr;
    if (!TonicCenterFramesCpu(desc.centerX.data(), desc.centerY.data(),
                              desc.centerZ.data(), int(desc.centerX.size()),
                              &frames, &derr)) {
        _diagnostic = derr;
        return false;
    }
    std::vector<TonicTubeDesc> kids;
    if (!TonicSubdivideTubeCpu(desc, frames, params, &kids, &derr)) {
        _diagnostic = derr;
        return false;
    }
    _PushUndoLocked();
    // Children inherit the parent's fill params (§2.3 step 4); the parent's
    // own fill is suspended from here until the children are merged back.
    FillParams inherited = _fill;
    {
        auto pit = _tubes.find(tubeId);
        if (pit != _tubes.end()) {
            inherited = pit->second.fill;
        }
    }
    outChildren->clear();
    for (auto &k : kids) {
        HierarchyTube entry;
        entry.actual = k;
        entry.derived = k;
        TonicClearDeltas(k, &entry.deltas);
        entry.fill = inherited;
        int const id = k.tubeId;
        _tubes[id] = entry;
        outChildren->push_back(id);
    }
    if (tubeId == 0) {
        _subdivide.count = count;
        _subdivide.seed = seed;
        _subdivide.splitMode = mode;
        _subdivide.edgeA = edgeA;
        _subdivide.edgeB = edgeB;
        _subdivide.edgeC = edgeC;
    } else {
        auto pit = _tubes.find(tubeId);
        if (pit != _tubes.end()) {
            pit->second.subdivide = params;
        }
    }
    // plan/18 §2.3: a subdivide of a selected parent moves the selection
    // to its children. The parent is still there (it is now a control
    // cage), but the artist's next edit belongs to what they just made.
    _selection.RemapTube(tubeId, *outChildren);
    ++_version;
    ++_mapVersion;
    _dirty |= TonicDirty_Topology | TonicDirty_Selection;
    return true;
}

bool TonicModel::MergeChildren(int tubeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<int> kids;
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == tubeId) {
            kids.push_back(kv.first);
        }
    }
    if (kids.empty()) {
        return true;
    }
    std::vector<TonicTubeDesc> actuals;
    for (int id : kids) {
        actuals.push_back(_tubes.find(id)->second.actual);
    }
    TonicTubeDesc parentDesc;
    if (!_TubeDescLocked(tubeId, &parentDesc)) {
        _diagnostic = "TonicModel::MergeChildren: unknown tube";
        return false;
    }
    TonicSubdivideDesc params;
    if (tubeId == 0) {
        params.count = _subdivide.count;
        params.seed = _subdivide.seed;
        params.splitMode =
            _subdivide.splitMode == "edge" ? TonicSplit_Edge : TonicSplit_KMeans;
        params.edgeA = _subdivide.edgeA;
        params.edgeB = _subdivide.edgeB;
        params.edgeC = _subdivide.edgeC;
    } else {
        auto pit = _tubes.find(tubeId);
        if (pit == _tubes.end()) {
            return false;
        }
        params = pit->second.subdivide;
    }
    std::string derr;
    TonicTubeDesc merged;
    if (!TonicMergeTubesCpu(actuals, params, &parentDesc, &merged, &derr)) {
        _diagnostic = derr;
        return false;
    }
    _PushUndoLocked();
    // Remove the whole subtree below tubeId.
    std::vector<int> doomed = kids;
    for (size_t i = 0; i < doomed.size(); ++i) {
        for (auto const &kv : _tubes) {
            if (kv.second.actual.parentTubeId == doomed[i]) {
                doomed.push_back(kv.first);
            }
        }
    }
    for (int id : doomed) {
        _tubes.erase(id);
    }
    if (tubeId == 0) {
        TonicTubeDesc fit;
        if (!_FitAggregateForWriteback(merged, parentDesc, &fit, &derr)) {
            _diagnostic = derr;
            return false;
        }
        if (!_WriteDescToTube0Locked(fit)) {
            return false;
        }
    } else {
        auto pit = _tubes.find(tubeId);
        if (pit == _tubes.end()) {
            return false;
        }
        TonicTubeDesc refit;
        if (!_FitAggregateForWriteback(merged, pit->second.actual, &refit,
                                       &derr)) {
            _diagnostic = derr;
            return false;
        }
        refit.tubeId = pit->second.actual.tubeId;
        refit.regionId = pit->second.actual.regionId;
        refit.level = pit->second.actual.level;
        refit.parentTubeId = pit->second.actual.parentTubeId;
        refit.childIndex = pit->second.actual.childIndex;
        std::vector<TonicFrame> dframes;
        if (!TonicCenterFramesCpu(pit->second.derived.centerX.data(),
                                  pit->second.derived.centerY.data(),
                                  pit->second.derived.centerZ.data(),
                                  int(pit->second.derived.centerX.size()),
                                  &dframes, &derr)) {
            _diagnostic = derr;
            return false;
        }
        TonicShapeDeltas deltas;
        if (!TonicComputeDeltasCpu(refit, pit->second.derived, dframes,
                                   &deltas, &derr)) {
            _diagnostic = derr;
            return false;
        }
        pit->second.actual = refit;
        pit->second.deltas = deltas;
    }
    ++_version;
    ++_mapVersion;
    _dirty |= TonicDirty_Topology;
    return true;
}

bool TonicModel::MergeSelected(std::vector<int> const &tubeIds, int *outKept)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!outKept || tubeIds.empty()) {
        _diagnostic = "TonicModel::MergeSelected: want tube ids + outKept";
        return false;
    }
    int const parentId = _tubes.find(tubeIds[0]) == _tubes.end()
                             ? -2
                             : _tubes.find(tubeIds[0])->second.actual.parentTubeId;
    for (int id : tubeIds) {
        auto it = _tubes.find(id);
        if (it == _tubes.end() ||
            it->second.actual.parentTubeId != parentId || parentId < 0) {
            _diagnostic =
                "TonicModel::MergeSelected: want siblings sharing a parent";
            return false;
        }
    }
    std::vector<TonicTubeDesc> actuals;
    for (int id : tubeIds) {
        actuals.push_back(_tubes.find(id)->second.actual);
    }
    std::string derr;
    TonicTubeDesc merged;
    if (!_LaneParentAverage(HasCudaMirror(), actuals, &merged, &derr)) {
        _diagnostic = derr;
        return false;
    }
    int const kept = tubeIds[0];
    auto kit = _tubes.find(kept);
    TonicTubeDesc refit;
    if (!_FitAggregateForWriteback(merged, kit->second.actual, &refit,
                                   &derr)) {
        _diagnostic = derr;
        return false;
    }
    refit.tubeId = kept;
    refit.regionId = kit->second.actual.regionId;
    refit.level = kit->second.actual.level;
    refit.parentTubeId = parentId;
    refit.childIndex = kit->second.actual.childIndex;
    std::vector<TonicFrame> dframes;
    if (!TonicCenterFramesCpu(kit->second.derived.centerX.data(),
                              kit->second.derived.centerY.data(),
                              kit->second.derived.centerZ.data(),
                              int(kit->second.derived.centerX.size()),
                              &dframes, &derr)) {
        _diagnostic = derr;
        return false;
    }
    TonicShapeDeltas deltas;
    if (!TonicComputeDeltasCpu(refit, kit->second.derived, dframes, &deltas,
                               &derr)) {
        _diagnostic = derr;
        return false;
    }
    _PushUndoLocked();
    kit->second.actual = refit;
    kit->second.deltas = deltas;
    for (size_t i = 1; i < tubeIds.size(); ++i) {
        _tubes.erase(tubeIds[i]);
    }
    *outKept = kept;
    ++_version;
    ++_mapVersion;
    _dirty |= TonicDirty_Topology;
    return true;
}

int TonicModel::GetTubeCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    int n = int(_tubes.size());
    if (_host.centerX.size() >= 2) {
        ++n;
    }
    return n;
}

int TonicModel::GetTubeLevel(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId == 0) {
        return _host.centerX.size() >= 2 ? 1 : -1;
    }
    auto it = _tubes.find(tubeId);
    return it == _tubes.end() ? -1 : it->second.actual.level;
}

std::vector<int> TonicModel::GetTubeChildren(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<int> kids;
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == tubeId) {
            kids.push_back(kv.first);
        }
    }
    return kids;
}

int TonicModel::GetTubeCenterCount(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId == 0) {
        return _host.centerX.size() >= 2 ? int(_host.centerX.size()) : -1;
    }
    auto it = _tubes.find(tubeId);
    return it == _tubes.end() ? -1 : int(it->second.actual.centerX.size());
}

bool TonicModel::GetTubeCenterCV(int tubeId, int cv, float *x, float *y,
                                 float *z) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!x || !y || !z) {
        return false;
    }
    if (tubeId == 0) {
        int const n = int(_host.centerX.size());
        if (cv < 0 || cv >= n || n < 2) {
            return false;
        }
        *x = _host.centerX[size_t(cv)];
        *y = _host.centerY[size_t(cv)];
        *z = _host.centerZ[size_t(cv)];
        return true;
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return false;
    }
    int const n = int(it->second.actual.centerX.size());
    if (cv < 0 || cv >= n) {
        return false;
    }
    *x = it->second.actual.centerX[size_t(cv)];
    *y = it->second.actual.centerY[size_t(cv)];
    *z = it->second.actual.centerZ[size_t(cv)];
    return true;
}

std::shared_ptr<TonicModel::GraphUndoState const>
TonicModel::_GraphUndoStateLocked() const
{
    if (!_scalp || !_scalp->finalized) {
        return nullptr;
    }
    if (_graphUndoCache && _graphUndoCacheMap == _mapVersion) {
        return _graphUndoCache;  // the graph cannot have changed
    }
    auto state = std::make_shared<GraphUndoState>();
    for (auto const &nd : _graph.Nodes()) {
        if (nd.alive) {
            state->nodes.push_back(nd);
        }
    }
    for (auto const &e : _graph.Edges()) {
        if (e.alive) {
            state->edges.emplace_back(e.a, e.b);
        }
    }
    state->linked = _graph.LinkedPairs();
    state->snapRadius = _snapRadius;
    _graphUndoCache = state;
    _graphUndoCacheMap = _mapVersion;
    return _graphUndoCache;
}

void TonicModel::_RestoreGraphUndoLocked(
    std::shared_ptr<GraphUndoState const> const &state)
{
    if (!state || !_scalp || !_scalp->finalized) {
        return;
    }
    std::string err;
    if (!_graph.Restore(*_scalp, state->nodes, state->edges, state->linked,
                        &err)) {
        return;  // the diagnostic the caller carries is the one that matters
    }
    _snapRadius = state->snapRadius;
    // The region maps are derived, exactly as RestoreGraph derives them:
    // an undone graph edit has to leave the tint and the bake key right.
    TonicRasteriseRegionsCpu(*_scalp, _graph, &_maps, &err);
    TonicFlattenLoops(_graph, &_loops, &err);
    // The live graph IS this state now, so the next snapshot shares it
    // instead of rebuilding an identical copy.
    _graphUndoCache = state;
    _graphUndoCacheMap = _mapVersion;
    _dirty |= TonicDirty_Graph | TonicDirty_Regions;
}

TonicModel::HierarchyRollback
TonicModel::_SnapshotHierarchyLocked() const
{
    HierarchyRollback snap;
    snap.tubes = _tubes;
    snap.cx = _host.centerX;
    snap.cy = _host.centerY;
    snap.cz = _host.centerZ;
    snap.sections = _sections;
    snap.shape = _shape;
    snap.useSections = _useSections;
    snap.roots = _roots;
    snap.guides = _guides;
    snap.graph = _GraphUndoStateLocked();
    snap.regionTube = _regionTube;
    snap.tubeRegionKey = _tubeRegionKey;
    snap.tube0Region = _tubeRegionId;
    return snap;
}

void TonicModel::_RestoreHierarchyLocked(HierarchyRollback const &snap)
{
    std::string keep = _diagnostic;
    _tubes = snap.tubes;
    _host.centerX = snap.cx;
    _host.centerY = snap.cy;
    _host.centerZ = snap.cz;
    _sections = snap.sections;
    _shape = snap.shape;
    _useSections = snap.useSections;
    _roots = snap.roots;
    _guides = snap.guides;
    ++_guideVersion;
    _regionTube = snap.regionTube;
    _tubeRegionKey = snap.tubeRegionKey;
    _tubeRegionId = snap.tube0Region;
    _RestoreGraphUndoLocked(snap.graph);
    // Only re-tessellate when there IS a tube. A graph-only model reaches
    // this path from V1 on (graph edits are undoable now), and the legacy
    // tessellation reads _shape.rings CVs out of the center columns, which
    // a tube-less model does not have.
    if (!_host.centerX.empty()) {
        Sync();  // best effort; the original error below is what matters
    }
    _diagnostic = keep;
    _dirty |= TonicDirty_Topology;
}

// Accounted bytes of one undo snapshot (payload only: vector contents
// plus a per-node allowance for the map and section structs; vector
// capacities and allocator overhead are deliberately excluded — the
// budget is approximate by contract, and this side under-reports).
uint64_t
TonicModel::_UndoSnapshotBytes(HierarchyRollback const &snap)
{
    uint64_t bytes = 0;
    auto sectionBytes = [](std::vector<TonicTubeSection> const &sections) {
        uint64_t sub = 0;
        for (auto const &s : sections) {
            sub += 64;  // struct + vector shells
            sub += uint64_t(s.u.size() + s.v.size()) * sizeof(float);
        }
        return sub;
    };
    auto descBytes = [&](TonicTubeDesc const &d) {
        uint64_t sub = 64;  // struct shell + scalars
        sub += uint64_t(d.centerX.size() + d.centerY.size() +
                        d.centerZ.size()) *
               sizeof(float);
        sub += sectionBytes(d.sections);
        return sub;
    };
    for (auto const &kv : snap.tubes) {
        bytes += 64;  // map node + key
        bytes += descBytes(kv.second.actual);
        bytes += descBytes(kv.second.derived);
        bytes += 32;  // deltas shell
        bytes += uint64_t(kv.second.deltas.centerDu.size() +
                          kv.second.deltas.centerDv.size() +
                          kv.second.deltas.centerDw.size()) *
                 sizeof(float);
        bytes += sectionBytes(kv.second.deltas.sections);
        bytes += 32;  // subdivide params + flags + members shell
        bytes += uint64_t(kv.second.members.size()) * sizeof(int);
    }
    bytes += uint64_t(snap.cx.size() + snap.cy.size() + snap.cz.size()) *
             sizeof(float);
    bytes += sectionBytes(snap.sections);
    bytes += sizeof(snap.shape) + sizeof(snap.useSections);
    bytes += uint64_t(snap.roots.size()) * sizeof(TonicGuideRoot);
    bytes += 32;  // guide set shell
    bytes += uint64_t(snap.guides.points.size()) * sizeof(float);
    bytes += uint64_t(snap.guides.counts.size()) * sizeof(int);
    if (snap.graph) {
        // Counted per step even though consecutive steps share the state:
        // over-reporting evicts a little early, which is the safe side of
        // an approximate budget (and a graph is kilobytes, not megabytes).
        bytes += uint64_t(snap.graph->nodes.size()) * sizeof(TonicGraphNode);
        bytes += uint64_t(snap.graph->edges.size() +
                          snap.graph->linked.size()) *
                 (2 * sizeof(int));
    }
    bytes += uint64_t(snap.label.size());
    return bytes;
}

void TonicModel::_EnforceUndoBudgetLocked()
{
    while (!_undoStack.empty() &&
           (int(_undoStack.size()) > _undoMaxDepth ||
            _undoBytes > _undoMaxBytes)) {
        _undoBytes -= _UndoSnapshotBytes(_undoStack.front());
        _undoStack.pop_front();
    }
}

void TonicModel::_ClearRedoLocked()
{
    _redoStack.clear();
    _redoBytes = 0;
}

void TonicModel::_PushUndoSnapshotLocked(HierarchyRollback const &snap)
{
    // A new mutation branches the history: whatever redo held is
    // unreachable from here (plan/18 §2.5). This runs before the
    // gesture and budget early-outs, so the rule holds even when undo
    // is switched off.
    _ClearRedoLocked();
    if (_gestureDepth > 0) {
        // Inside a bracket the press-time snapshot IS the undo step
        // (plan/18 §3.2): a 200-sample drag pushes one entry, not 200.
        return;
    }
    if (_undoMaxDepth <= 0) {
        return;  // disabled: no stack, no accounting
    }
    HierarchyRollback labelled = snap;
    labelled.label = _nextUndoLabel.empty() ? std::string("edit")
                                            : _nextUndoLabel;
    _nextUndoLabel.clear();
    _undoBytes += _UndoSnapshotBytes(labelled);
    _undoStack.push_back(std::move(labelled));
    _EnforceUndoBudgetLocked();
}

void TonicModel::_PushUndoLocked()
{
    _PushUndoSnapshotLocked(_SnapshotHierarchyLocked());
}

void TonicModel::_PushGraphUndoLocked(
    std::shared_ptr<GraphUndoState const> const &before, char const *label)
{
    HierarchyRollback snap = _SnapshotHierarchyLocked();
    snap.graph = before;
    if (_nextUndoLabel.empty() && label) {
        _nextUndoLabel = label;
    }
    _PushUndoSnapshotLocked(snap);
}

bool TonicModel::Undo(uint32_t *outDirty)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (outDirty) {
        *outDirty = TonicDirty_Clean;
    }
    if (_undoStack.empty()) {
        return true;  // nothing to undo: no-op success
    }
    HierarchyRollback snap = std::move(_undoStack.back());
    _undoStack.pop_back();
    _undoBytes -= _UndoSnapshotBytes(snap);
    // What we are about to leave becomes the redo step, under the same
    // label: undoing "sculpt" makes "sculpt" the thing Ctrl+Y redoes.
    HierarchyRollback forward = _SnapshotHierarchyLocked();
    forward.label = snap.label;
    _redoBytes += _UndoSnapshotBytes(forward);
    _redoStack.push_back(std::move(forward));
    while (!_redoStack.empty() &&
           (int(_redoStack.size()) > _undoMaxDepth ||
            _redoBytes > _undoMaxBytes)) {
        _redoBytes -= _UndoSnapshotBytes(_redoStack.front());
        _redoStack.pop_front();
    }
    _RestoreHierarchyLocked(snap);
    ++_version;
    ++_mapVersion;
    uint32_t const bits =
        TonicDirty_Points | TonicDirty_Topology | TonicDirty_Guides;
    _dirty |= bits;
    if (outDirty) {
        *outDirty = _dirty;
    }
    return true;
}

bool TonicModel::Redo(uint32_t *outDirty)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (outDirty) {
        *outDirty = TonicDirty_Clean;
    }
    if (_gestureDepth > 0) {
        _diagnostic = "TonicModel::Redo: a gesture is open";
        return false;
    }
    if (_redoStack.empty()) {
        return true;  // nothing to redo: no-op success
    }
    HierarchyRollback snap = std::move(_redoStack.back());
    _redoStack.pop_back();
    _redoBytes -= _UndoSnapshotBytes(snap);
    // Push the state we leave straight onto the undo stack: going through
    // _PushUndoSnapshotLocked would clear the redo stack we are walking.
    HierarchyRollback back = _SnapshotHierarchyLocked();
    back.label = snap.label;
    if (_undoMaxDepth > 0) {
        _undoBytes += _UndoSnapshotBytes(back);
        _undoStack.push_back(std::move(back));
        _EnforceUndoBudgetLocked();
    }
    _RestoreHierarchyLocked(snap);
    ++_version;
    ++_mapVersion;
    uint32_t const bits =
        TonicDirty_Points | TonicDirty_Topology | TonicDirty_Guides;
    _dirty |= bits;
    if (outDirty) {
        *outDirty = _dirty;
    }
    return true;
}

int TonicModel::GetRedoDepth() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return int(_redoStack.size());
}

bool TonicModel::GetUndoLabel(int depth, std::string *out) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!out) {
        return false;
    }
    if (depth >= 0) {
        if (depth >= int(_undoStack.size())) {
            return false;
        }
        *out = _undoStack[_undoStack.size() - 1 - size_t(depth)].label;
        return true;
    }
    size_t const back = size_t(-depth) - 1;  // -1 is the next redo
    if (back >= _redoStack.size()) {
        return false;
    }
    *out = _redoStack[_redoStack.size() - 1 - back].label;
    return true;
}

// -- V1: the gesture bracket (plan/18 §3.2) ----------------------------------

bool TonicModel::BeginGesture(char const *label)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_gestureDepth > 0) {
        _diagnostic = "TonicModel::BeginGesture: a gesture is already open";
        return false;
    }
    _gestureBase = _SnapshotHierarchyLocked();
    _nextUndoLabel = label && *label ? label : "gesture";
    _PushUndoSnapshotLocked(_gestureBase);  // the one step of the drag
    _gestureDepth = 1;
    return true;
}

bool TonicModel::EndGesture()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_gestureDepth <= 0) {
        _diagnostic = "TonicModel::EndGesture: no gesture is open";
        return false;
    }
    _gestureDepth = 0;
    _gestureBase = HierarchyRollback();
    ++_version;  // one coalescing key for the whole drag
    return true;
}

bool TonicModel::CancelGesture(uint32_t *outDirty)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (outDirty) {
        *outDirty = TonicDirty_Clean;
    }
    if (_gestureDepth <= 0) {
        _diagnostic = "TonicModel::CancelGesture: no gesture is open";
        return false;
    }
    HierarchyRollback const base = _gestureBase;
    _gestureDepth = 0;
    _gestureBase = HierarchyRollback();
    // Drop the step Begin pushed: the drag never happened as far as the
    // history is concerned. Pushes were suppressed while it was open, so
    // the top of the stack is that step.
    if (!_undoStack.empty()) {
        _undoBytes -= _UndoSnapshotBytes(_undoStack.back());
        _undoStack.pop_back();
    }
    _RestoreHierarchyLocked(base);
    // No version bump: the stage never saw the drag (plan/18 §2.5). The
    // VIEWPORT did, so the caller still has to publish what changed back.
    _dirty |= TonicDirty_Points | TonicDirty_Topology | TonicDirty_Guides;
    if (outDirty) {
        *outDirty = _dirty;
    }
    return true;
}

int TonicModel::GetGestureDepth() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _gestureDepth;
}

int TonicModel::GetUndoDepth() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return int(_undoStack.size());
}

uint64_t TonicModel::GetUndoBytes() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _undoBytes;
}

bool TonicModel::SetUndoBudget(int maxDepth, uint64_t maxBytes)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (maxDepth < 0) {
        _diagnostic = "TonicModel::SetUndoBudget: depth must be >= 0";
        return false;
    }
    _undoMaxDepth = maxDepth;
    _undoMaxBytes = maxBytes;
    _EnforceUndoBudgetLocked();
    return true;
}

void TonicModel::_ClearUndoLocked()
{
    _undoStack.clear();
    _undoBytes = 0;
    _ClearRedoLocked();  // a cleared history has nothing to redo either
}

void TonicModel::ClearUndo()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _ClearUndoLocked();
}

bool TonicModel::MoveTubeCenterCV(int tubeId, int cv, float dx, float dy,
                                  float dz)
{
    std::lock_guard<std::mutex> lock(_mutex);
    HierarchyRollback const snap = _SnapshotHierarchyLocked();
    auto const rollback = [&](bool ok) {
        if (!ok) {
            _RestoreHierarchyLocked(snap);
        }
        return ok;
    };
    if (tubeId == 0) {
        int const n = int(_host.centerX.size());
        if (cv < 0 || cv >= n || n < 2) {
            _diagnostic = "TonicModel::MoveTubeCenterCV: bad tube-0 CV";
            return false;
        }
        // Same soft-selection application as MoveCenterCV, minus its
        // version bump (one gesture, one version; propagation below).
        float const centerT = float(cv) / float(n - 1);
        for (int i = 0; i < n; ++i) {
            float const t = float(i) / float(n - 1);
            float const w = TonicSoftWeight(t, centerT, _softRadius);
            if (w <= 0.0f && i != cv) {
                continue;
            }
            float const k =
                (_softRadius > 0.0f) ? w : (i == cv ? 1.0f : 0.0f);
            _host.centerX[size_t(i)] += dx * k;
            _host.centerY[size_t(i)] += dy * k;
            _host.centerZ[size_t(i)] += dz * k;
        }
        if (!Sync()) {
            return rollback(false);
        }
        _dirty |= TonicDirty_Points;
    } else {
        auto it = _tubes.find(tubeId);
        if (it == _tubes.end()) {
            _diagnostic = "TonicModel::MoveTubeCenterCV: unknown tube";
            return false;
        }
        HierarchyTube &entry = it->second;
        int const n = int(entry.actual.centerX.size());
        if (cv < 0 || cv >= n) {
            _diagnostic = "TonicModel::MoveTubeCenterCV: bad CV index";
            return false;
        }
        // Soft selection along the center is a property of the MOVE, not
        // of tube 0 (plan/17 §5.2, and the §5.2 V0b note that says the
        // per-tube spellings keep tube 0's behaviour "soft selection
        // included"). Before V6 only the tube-0 branch above applied it,
        // so a drag on a child snapped one CV while the same drag on the
        // root feathered. Same TonicSoftWeight, same radius, same
        // single-CV behaviour at radius 0.
        float const centerT = n > 1 ? float(cv) / float(n - 1) : 0.0f;
        for (int i = 0; i < n; ++i) {
            float const t = n > 1 ? float(i) / float(n - 1) : 0.0f;
            float const w = TonicSoftWeight(t, centerT, _softRadius);
            if (w <= 0.0f && i != cv) {
                continue;
            }
            float const k =
                (_softRadius > 0.0f) ? w : (i == cv ? 1.0f : 0.0f);
            entry.actual.centerX[size_t(i)] += dx * k;
            entry.actual.centerY[size_t(i)] += dy * k;
            entry.actual.centerZ[size_t(i)] += dz * k;
        }
        std::vector<TonicFrame> dframes;
        std::string derr;
        if (!TonicCenterFramesCpu(entry.derived.centerX.data(),
                                  entry.derived.centerY.data(),
                                  entry.derived.centerZ.data(),
                                  int(entry.derived.centerX.size()), &dframes,
                                  &derr)) {
            _diagnostic = derr;
            return rollback(false);
        }
        if (!TonicComputeDeltasCpu(entry.actual, entry.derived, dframes,
                                   &entry.deltas, &derr)) {
            _diagnostic = derr;
            return rollback(false);
        }
        _dirty |= TonicDirty_Points;
    }
    if (!_PropagateDownLocked(tubeId)) {
        return rollback(false);
    }
    if (!_PropagateUpLocked(tubeId)) {
        return rollback(false);
    }
    _PushUndoSnapshotLocked(snap);
    ++_version;
    return true;
}

std::vector<float> TonicModel::ReadTubeDeltas(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<float> flat;
    if (tubeId == 0) {
        flat.assign(_host.centerX.size() * 3, 0.0f);
        return flat;
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return flat;
    }
    TonicShapeDeltas const &d = it->second.deltas;
    flat.reserve(d.centerDu.size() * 3);
    for (size_t i = 0; i < d.centerDu.size(); ++i) {
        flat.push_back(d.centerDu[i]);
        flat.push_back(d.centerDv[i]);
        flat.push_back(d.centerDw[i]);
    }
    return flat;
}

void TonicModel::SetTubeLockParents(int tubeId, bool on)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId < 0) {
        _globalLockParents = on;
        return;
    }
    if (tubeId == 0) {
        _lockParents = on;
        return;
    }
    auto it = _tubes.find(tubeId);
    if (it != _tubes.end()) {
        it->second.lockParents = on;
    }
}

void TonicModel::SetTubeLockChildren(int tubeId, bool on)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId < 0) {
        _globalLockChildren = on;
        return;
    }
    if (tubeId == 0) {
        _lockChildren = on;
        return;
    }
    auto it = _tubes.find(tubeId);
    if (it != _tubes.end()) {
        it->second.lockChildren = on;
    }
}

bool TonicModel::GroupTubes(std::vector<int> const &tubeIds,
                            bool transientParent, int *outParent)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!outParent || tubeIds.empty()) {
        _diagnostic = "TonicModel::GroupTubes: want tube ids + outParent";
        return false;
    }
    std::vector<TonicTubeDesc> actuals;
    for (int id : tubeIds) {
        TonicTubeDesc desc;
        if (!_TubeDescLocked(id, &desc)) {
            _diagnostic = "TonicModel::GroupTubes: unknown member tube";
            return false;
        }
        actuals.push_back(desc);
    }
    std::string derr;
    TonicTubeDesc agg;
    if (!_LaneParentAverage(HasCudaMirror(), actuals, &agg, &derr)) {
        _diagnostic = derr;
        return false;
    }
    _PushUndoLocked();
    int const id = _nextGroupId--;
    agg.tubeId = id;
    HierarchyTube entry;
    entry.actual = agg;
    entry.derived = agg;  // self-derived: deltas stay zero until edited
    TonicClearDeltas(agg, &entry.deltas);
    entry.transientParent = transientParent;
    entry.persistent = false;
    entry.members = tubeIds;
    _tubes[id] = entry;
    *outParent = id;
    ++_version;
    ++_mapVersion;
    _dirty |= TonicDirty_Topology;
    return true;
}

bool TonicModel::MakeTubePersistent(int tubeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        _diagnostic = "TonicModel::MakeTubePersistent: unknown tube";
        return false;
    }
    _PushUndoLocked();
    it->second.persistent = true;
    ++_version;
    return true;
}

// -- V5: brush shaping (plan/18 section 3.6, section 7 G13) ---------------
//
// The audit found comb/twist/falloff/mirror living in Python or nowhere:
// the ABI took pre-weighted per-CV deltas, so every brush's shape was the
// tool's opinion and no two callers had to agree. These helpers are that
// maths, once, in C++; the loop now passes the stroke.
namespace {

bool
_TonicIsSculptBrush(std::string const &b)
{
    return b == "grab" || b == "smooth" || b == "comb" || b == "lengthen" ||
           b == "shorten" || b == "twist";
}

// Row-major USD projection, top-left-origin physical pixels: the same
// convention Tonic_Pick and tonicCamera carry, so a brush radius in
// pixels means the pixels the artist is looking at.
bool
_TonicProjectPixel(float const *m, int w, int h, float px, float py,
                   float pz, float *outX, float *outY)
{
    double const x = double(px), y = double(py), z = double(pz);
    double const cx = x * m[0] + y * m[4] + z * m[8] + m[12];
    double const cy = x * m[1] + y * m[5] + z * m[9] + m[13];
    double const cw = x * m[3] + y * m[7] + z * m[11] + m[15];
    if (!(cw > 0.0)) {
        return false;  // behind the camera: outside every brush
    }
    double const ndcX = cx / cw;
    double const ndcY = cy / cw;
    *outX = float((ndcX * 0.5 + 0.5) * double(w));
    *outY = float((1.0 - (ndcY * 0.5 + 0.5)) * double(h));
    return true;
}

float
_TonicSmoothStep(double s)
{
    if (s <= 0.0) {
        return 0.0f;
    }
    if (s >= 1.0) {
        return 1.0f;
    }
    return float(s * s * (3.0 - 2.0 * s));
}

// Screen-disc weight times the t-window weight (plan/17 section 5.5).
// tRadius <= 0 means "no t bound", which is what TonicToolState's
// brushTRadius = 0 has always documented.
float
_TonicBrushWeight(double distPx, double radiusPx, double t, double tCenter,
                  double tRadius)
{
    float w = 0.0f;
    if (radiusPx > 0.0) {
        w = _TonicSmoothStep(1.0 - distPx / radiusPx);
    } else {
        w = (distPx == 0.0) ? 1.0f : 0.0f;
    }
    if (w <= 0.0f || !(tRadius > 0.0)) {
        return w;
    }
    double const d = t - tCenter;
    return w * _TonicSmoothStep(1.0 - (d < 0.0 ? -d : d) / tRadius);
}

// One stroke shaped into per-CV deltas over the positions the stroke sees
// (the tube's own for the primary pass, mirrored across x = 0 for the
// symmetric pass). Only the CVs that actually move come back.
bool
_TonicShapeStroke(std::string const &brush, std::vector<float> const &cx,
                  std::vector<float> const &cy, std::vector<float> const &cz,
                  float const *viewProj, int w, int h, float cursorX,
                  float cursorY, float radiusPx, float const *deltaWorld,
                  float amount, float tCenter, float tRadius,
                  std::vector<int> *outIds, std::vector<float> *outDeltas,
                  std::string *err)
{
    int const n = int(cx.size());
    if (n < 2 || int(cy.size()) != n || int(cz.size()) != n) {
        if (err) {
            *err = "TonicModel::SculptStrokeShaped: tube has no center curve";
        }
        return false;
    }
    std::vector<float> weight(size_t(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        float sx = 0.0f, sy = 0.0f;
        if (!_TonicProjectPixel(viewProj, w, h, cx[size_t(i)], cy[size_t(i)],
                                cz[size_t(i)], &sx, &sy)) {
            continue;
        }
        double const dx = double(sx) - double(cursorX);
        double const dy = double(sy) - double(cursorY);
        double const t = double(i) / double(n - 1);
        weight[size_t(i)] = _TonicBrushWeight(std::sqrt(dx * dx + dy * dy),
                                              double(radiusPx), t,
                                              double(tCenter),
                                              double(tRadius));
    }
    std::vector<float> delta(size_t(n) * 3, 0.0f);
    double const dwx = double(deltaWorld[0]);
    double const dwy = double(deltaWorld[1]);
    double const dwz = double(deltaWorld[2]);
    double const dwLen = std::sqrt(dwx * dwx + dwy * dwy + dwz * dwz);
    if (brush == "grab") {
        for (int i = 0; i < n; ++i) {
            double const k = double(weight[size_t(i)]);
            delta[size_t(i) * 3 + 0] = float(dwx * k);
            delta[size_t(i) * 3 + 1] = float(dwy * k);
            delta[size_t(i) * 3 + 2] = float(dwz * k);
        }
    } else if (brush == "comb") {
        if (!(dwLen > 1e-12)) {
            outIds->clear();
            outDeltas->clear();
            return true;  // no direction: a no-op, never a NaN
        }
        double const mag = (amount > 0.0f) ? double(amount) : dwLen;
        double const ux = dwx / dwLen, uy = dwy / dwLen, uz = dwz / dwLen;
        for (int i = 0; i < n; ++i) {
            double const k = double(weight[size_t(i)]) * mag;
            delta[size_t(i) * 3 + 0] = float(ux * k);
            delta[size_t(i) * 3 + 1] = float(uy * k);
            delta[size_t(i) * 3 + 2] = float(uz * k);
        }
    } else if (brush == "smooth") {
        double strength = (amount > 0.0f) ? double(amount) : 1.0;
        strength = strength > 1.0 ? 1.0 : strength;
        for (int i = 1; i < n - 1; ++i) {
            double const k = double(weight[size_t(i)]) * strength;
            double const ax = 0.5 * (double(cx[size_t(i - 1)]) +
                                     double(cx[size_t(i + 1)]));
            double const ay = 0.5 * (double(cy[size_t(i - 1)]) +
                                     double(cy[size_t(i + 1)]));
            double const az = 0.5 * (double(cz[size_t(i - 1)]) +
                                     double(cz[size_t(i + 1)]));
            delta[size_t(i) * 3 + 0] = float((ax - double(cx[size_t(i)])) * k);
            delta[size_t(i) * 3 + 1] = float((ay - double(cy[size_t(i)])) * k);
            delta[size_t(i) * 3 + 2] = float((az - double(cz[size_t(i)])) * k);
        }
    } else if (brush == "lengthen" || brush == "shorten") {
        // Root-pinned stretch along the CV's own offset from the root:
        // at weight 1 everywhere this is the uniform rescale by
        // (1 + amount) the length field does, and under a brush it grows
        // only the span the artist is over. "shorten" is the same with a
        // negative amount.
        double const scale = (brush == "shorten" && amount > 0.0f)
                                 ? -double(amount)
                                 : double(amount);
        for (int i = 1; i < n; ++i) {
            double const k = double(weight[size_t(i)]) * scale;
            delta[size_t(i) * 3 + 0] =
                float((double(cx[size_t(i)]) - double(cx[0])) * k);
            delta[size_t(i) * 3 + 1] =
                float((double(cy[size_t(i)]) - double(cy[0])) * k);
            delta[size_t(i) * 3 + 2] =
                float((double(cz[size_t(i)]) - double(cz[0])) * k);
        }
    } else {  // twist
        double ax = double(cx[size_t(n - 1)]) - double(cx[0]);
        double ay = double(cy[size_t(n - 1)]) - double(cy[0]);
        double az = double(cz[size_t(n - 1)]) - double(cz[0]);
        double const alen = std::sqrt(ax * ax + ay * ay + az * az);
        if (!(alen > 1e-12)) {
            outIds->clear();
            outDeltas->clear();
            return true;  // degenerate chord: nothing to rotate about
        }
        ax /= alen;
        ay /= alen;
        az /= alen;
        for (int i = 1; i < n; ++i) {
            double const theta = double(amount) * double(weight[size_t(i)]);
            if (theta == 0.0) {
                continue;
            }
            double const ox = double(cx[size_t(i)]) - double(cx[0]);
            double const oy = double(cy[size_t(i)]) - double(cy[0]);
            double const oz = double(cz[size_t(i)]) - double(cz[0]);
            double const dot = ox * ax + oy * ay + oz * az;
            double const parX = ax * dot, parY = ay * dot, parZ = az * dot;
            double const perX = ox - parX, perY = oy - parY, perZ = oz - parZ;
            // Rodrigues about the root-to-tip chord.
            double const crX = ay * perZ - az * perY;
            double const crY = az * perX - ax * perZ;
            double const crZ = ax * perY - ay * perX;
            double const ct = std::cos(theta), st = std::sin(theta);
            delta[size_t(i) * 3 + 0] =
                float(perX * (ct - 1.0) + crX * st);
            delta[size_t(i) * 3 + 1] =
                float(perY * (ct - 1.0) + crY * st);
            delta[size_t(i) * 3 + 2] =
                float(perZ * (ct - 1.0) + crZ * st);
        }
    }
    outIds->clear();
    outDeltas->clear();
    for (int i = 0; i < n; ++i) {
        float const dx = delta[size_t(i) * 3 + 0];
        float const dy = delta[size_t(i) * 3 + 1];
        float const dz = delta[size_t(i) * 3 + 2];
        if (dx == 0.0f && dy == 0.0f && dz == 0.0f) {
            continue;
        }
        outIds->push_back(i);
        outDeltas->push_back(dx);
        outDeltas->push_back(dy);
        outDeltas->push_back(dz);
    }
    return true;
}

}  // namespace

bool TonicModel::SculptStroke(int tubeId, const char *brush, int const *cvIds,
                              float const *deltas, int cvCount,
                              bool preserveLength, bool mirrorX)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::string const b = brush ? brush : "";
    if (!_TonicIsSculptBrush(b)) {
        _diagnostic = "TonicModel::SculptStroke: unknown brush";
        return false;
    }
    if (cvCount < 0 || (cvCount > 0 && (!cvIds || !deltas))) {
        _diagnostic = "TonicModel::SculptStroke: want CV ids + deltas";
        return false;
    }
    if (cvCount == 0) {
        return true;  // empty stroke: valid no-op, no version bump
    }
    HierarchyRollback const snap = _SnapshotHierarchyLocked();
    if (!_SculptApplyLocked(tubeId, b, cvIds, deltas, cvCount,
                            preserveLength, mirrorX)) {
        _RestoreHierarchyLocked(snap);
        return false;
    }
    _PushUndoSnapshotLocked(snap);
    ++_version;
    return true;
}

// One stroke's worth of model mutation, with the CALLER owning the
// rollback snapshot, the undo push and the version bump: SculptStroke runs
// it once, SculptStrokeShaped runs it twice (the stroke and its mirror)
// inside one bracket, which is what makes a symmetric stroke one undo step
// and one version.
bool TonicModel::_SculptApplyLocked(int tubeId, std::string const &b,
                                    int const *cvIds, float const *deltas,
                                    int cvCount, bool preserveLength,
                                    bool mirrorX)
{
    // Snapshot the pre-stroke length for preserveLength.
    TonicTubeDesc before;
    if (!_TubeDescLocked(tubeId, &before)) {
        _diagnostic = "TonicModel::SculptStroke: unknown tube";
        return false;
    }
    float const preLen = TonicCenterArcLength(
        before.centerX.data(), before.centerY.data(), before.centerZ.data(),
        int(before.centerX.size()));
    for (int k = 0; k < cvCount; ++k) {
        int const cv = cvIds[k];
        float dx = deltas[size_t(k) * 3 + 0];
        float const dy = deltas[size_t(k) * 3 + 1];
        float const dz = deltas[size_t(k) * 3 + 2];
        if (mirrorX) {
            dx = -dx;
        }
        if (tubeId == 0) {
            int const n = int(_host.centerX.size());
            if (cv < 0 || cv >= n || n < 2) {
                _diagnostic = "TonicModel::SculptStroke: bad CV index";
                return false;
            }
            _host.centerX[size_t(cv)] += dx;
            _host.centerY[size_t(cv)] += dy;
            _host.centerZ[size_t(cv)] += dz;
        } else {
            auto it = _tubes.find(tubeId);
            if (it == _tubes.end()) {
                _diagnostic = "TonicModel::SculptStroke: unknown tube";
                return false;
            }
            int const n = int(it->second.actual.centerX.size());
            if (cv < 0 || cv >= n) {
                _diagnostic = "TonicModel::SculptStroke: bad CV index";
                return false;
            }
            it->second.actual.centerX[size_t(cv)] += dx;
            it->second.actual.centerY[size_t(cv)] += dy;
            it->second.actual.centerZ[size_t(cv)] += dz;
        }
    }
    if (b == "smooth") {
        // One 0.5 relax pass over the stroked CVs (interior only).
        for (int k = 0; k < cvCount; ++k) {
            int const cv = cvIds[k];
            auto relax = [&](std::vector<float> &x, std::vector<float> &y,
                             std::vector<float> &z) {
                int const n = int(x.size());
                if (cv <= 0 || cv >= n - 1) {
                    return;
                }
                x[size_t(cv)] = 0.5f * x[size_t(cv)] +
                                0.25f * (x[size_t(cv - 1)] + x[size_t(cv + 1)]);
                y[size_t(cv)] = 0.5f * y[size_t(cv)] +
                                0.25f * (y[size_t(cv - 1)] + y[size_t(cv + 1)]);
                z[size_t(cv)] = 0.5f * z[size_t(cv)] +
                                0.25f * (z[size_t(cv - 1)] + z[size_t(cv + 1)]);
            };
            if (tubeId == 0) {
                relax(_host.centerX, _host.centerY, _host.centerZ);
            } else {
                auto it = _tubes.find(tubeId);
                relax(it->second.actual.centerX, it->second.actual.centerY,
                      it->second.actual.centerZ);
            }
        }
    }
    std::string derr;
    if (preserveLength) {
        TonicTubeDesc cur;
        if (!_TubeDescLocked(tubeId, &cur)) {
            return false;
        }
        std::vector<float> ox, oy, oz;
        if (!TonicRescaleCenterLength(cur.centerX.data(), cur.centerY.data(),
                                      cur.centerZ.data(),
                                      int(cur.centerX.size()), preLen, &ox,
                                      &oy, &oz, &derr)) {
            _diagnostic = derr;
            return false;
        }
        if (tubeId == 0) {
            _host.centerX.swap(ox);
            _host.centerY.swap(oy);
            _host.centerZ.swap(oz);
        } else {
            auto it = _tubes.find(tubeId);
            it->second.actual.centerX.swap(ox);
            it->second.actual.centerY.swap(oy);
            it->second.actual.centerZ.swap(oz);
        }
    }
    if (tubeId == 0) {
        if (!Sync()) {
            return false;
        }
    } else {
        auto it = _tubes.find(tubeId);
        HierarchyTube &entry = it->second;
        std::vector<TonicFrame> dframes;
        if (!TonicCenterFramesCpu(entry.derived.centerX.data(),
                                  entry.derived.centerY.data(),
                                  entry.derived.centerZ.data(),
                                  int(entry.derived.centerX.size()), &dframes,
                                  &derr)) {
            _diagnostic = derr;
            return false;
        }
        if (!TonicComputeDeltasCpu(entry.actual, entry.derived, dframes,
                                   &entry.deltas, &derr)) {
            _diagnostic = derr;
            return false;
        }
    }
    _dirty |= TonicDirty_Points;
    if (!_PropagateDownLocked(tubeId)) {
        return false;
    }
    if (!_PropagateUpLocked(tubeId)) {
        return false;
    }
    return true;
}

bool
TonicModel::SculptStrokeShaped(int tubeId, const char *brush,
                               float const *viewProj, int w, int h, float x,
                               float y, float radiusPx,
                               float const *deltaWorld, float amount,
                               float tCenter, float tRadius,
                               bool preserveLength, bool mirrorX,
                               int *outTouched)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (outTouched) {
        *outTouched = 0;
    }
    std::string const b = brush ? brush : "";
    if (!_TonicIsSculptBrush(b)) {
        _diagnostic = "TonicModel::SculptStrokeShaped: unknown brush";
        return false;
    }
    if (!viewProj || !deltaWorld) {
        _diagnostic = "TonicModel::SculptStrokeShaped: want a projection "
                      "and a stroke delta";
        return false;
    }
    if (w < 1 || h < 1) {
        _diagnostic = "TonicModel::SculptStrokeShaped: want a viewport size";
        return false;
    }
    // Lengthen SETS the length, so it is the one brush the
    // length-preserving default cannot apply to.
    bool const keepLength =
        preserveLength && b != "lengthen" && b != "shorten";
    // The shaping happened here, so the apply must take the deltas RAW:
    // no second smooth pass, no dx negation.
    static std::string const kRaw("grab");

    TonicTubeDesc desc;
    if (!_TubeDescLocked(tubeId, &desc)) {
        _diagnostic = "TonicModel::SculptStrokeShaped: unknown tube";
        return false;
    }
    std::vector<int> ids;
    std::vector<float> deltas;
    std::string err;
    if (!_TonicShapeStroke(b, desc.centerX, desc.centerY, desc.centerZ,
                           viewProj, w, h, x, y, radiusPx, deltaWorld, amount,
                           tCenter, tRadius, &ids, &deltas, &err)) {
        _diagnostic = err;
        return false;
    }

    // The symmetric half: the tube whose root is the mirror of this one's
    // across x = 0. Sculpted in MIRRORED space (positions and stroke both
    // reflected), then the delta is reflected back, so "mirror-X" means
    // the same stroke on the other side rather than a negated dx here.
    int mirrorTube = 0;
    bool haveMirror = false;
    std::vector<int> mirrorIds;
    std::vector<float> mirrorDeltas;
    if (mirrorX && !desc.centerX.empty()) {
        float const mx = -desc.centerX.front();
        float const my = desc.centerY.front();
        float const mz = desc.centerZ.front();
        double best = -1.0;
        auto const consider = [&](int id, TonicTubeDesc const &cand) {
            if (cand.centerX.empty()) {
                return;
            }
            double const dx = double(cand.centerX.front()) - double(mx);
            double const dy = double(cand.centerY.front()) - double(my);
            double const dz = double(cand.centerZ.front()) - double(mz);
            double const d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (best < 0.0 || d < best) {
                best = d;
                mirrorTube = id;
            }
        };
        if (_host.centerX.size() >= 2) {
            TonicTubeDesc hostDesc;
            if (_TubeDescLocked(0, &hostDesc)) {
                consider(0, hostDesc);
            }
        }
        for (auto const &kv : _tubes) {
            consider(kv.first, kv.second.actual);
        }
        float const arc = TonicCenterArcLength(
            desc.centerX.data(), desc.centerY.data(), desc.centerZ.data(),
            int(desc.centerX.size()));
        double const tolerance = std::max(0.05 * double(arc), 1e-4);
        haveMirror = (best >= 0.0 && best <= tolerance);
    }
    if (haveMirror) {
        TonicTubeDesc mdesc;
        if (!_TubeDescLocked(mirrorTube, &mdesc)) {
            _diagnostic = "TonicModel::SculptStrokeShaped: unknown tube";
            return false;
        }
        std::vector<float> rx(mdesc.centerX.size());
        for (size_t i = 0; i < mdesc.centerX.size(); ++i) {
            rx[i] = -mdesc.centerX[i];
        }
        if (!_TonicShapeStroke(b, rx, mdesc.centerY, mdesc.centerZ, viewProj,
                               w, h, x, y, radiusPx, deltaWorld, amount,
                               tCenter, tRadius, &mirrorIds, &mirrorDeltas,
                               &err)) {
            _diagnostic = err;
            return false;
        }
        for (size_t i = 0; i < mirrorDeltas.size(); i += 3) {
            mirrorDeltas[i] = -mirrorDeltas[i];
        }
    }

    int touched = int(ids.size()) + int(mirrorIds.size());
    if (touched == 0) {
        return true;  // nothing under the brush: no version, no undo step
    }
    HierarchyRollback const snap = _SnapshotHierarchyLocked();
    if (!ids.empty() &&
        !_SculptApplyLocked(tubeId, kRaw, ids.data(), deltas.data(),
                            int(ids.size()), keepLength, false)) {
        _RestoreHierarchyLocked(snap);
        return false;
    }
    if (!mirrorIds.empty() && mirrorTube != tubeId &&
        !_SculptApplyLocked(mirrorTube, kRaw, mirrorIds.data(),
                            mirrorDeltas.data(), int(mirrorIds.size()),
                            keepLength, false)) {
        _RestoreHierarchyLocked(snap);
        return false;
    }
    if (mirrorTube == tubeId) {
        touched = int(ids.size());  // one tube, one pass: the stroke IS
                                    // its own mirror on the plane
    }
    _PushUndoSnapshotLocked(snap);
    ++_version;
    if (outTouched) {
        *outTouched = touched;
    }
    return true;
}

bool
TonicModel::SubdivideTubeAlongEdge(int tubeId, float const *worldA,
                                   float const *worldB, int seed,
                                   std::vector<int> *outChildren)
{
    float a = 1.0f, b = 0.0f, c = 0.0f;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!worldA || !worldB) {
            _diagnostic = "TonicModel::SubdivideTubeAlongEdge: want two "
                          "world points";
            return false;
        }
        TonicTubeDesc desc;
        if (!_TubeDescLocked(tubeId, &desc)) {
            _diagnostic = "TonicModel::SubdivideTubeAlongEdge: unknown tube";
            return false;
        }
        std::vector<TonicFrame> frames;
        std::string derr;
        if (!TonicCenterFramesCpu(desc.centerX.data(), desc.centerY.data(),
                                  desc.centerZ.data(),
                                  int(desc.centerX.size()), &frames, &derr) ||
            frames.empty()) {
            _diagnostic = derr.empty() ? "TonicModel::SubdivideTubeAlongEdge:"
                                         " no root frame"
                                       : derr;
            return false;
        }
        // The root frame is the chart K14 partitions the root ring in:
        // u along the frame normal, v along its binormal, origin at
        // center CV 0 (tonicHierarchy.cpp PlaceRing).
        TonicFrame const &fr = frames.front();
        double const ox = double(desc.centerX.front());
        double const oy = double(desc.centerY.front());
        double const oz = double(desc.centerZ.front());
        auto const chart = [&](float const *p, double *u, double *v) {
            double const dx = double(p[0]) - ox;
            double const dy = double(p[1]) - oy;
            double const dz = double(p[2]) - oz;
            *u = dx * double(fr.nx) + dy * double(fr.ny) + dz * double(fr.nz);
            *v = dx * double(fr.bx) + dy * double(fr.by) + dz * double(fr.bz);
        };
        double uA = 0.0, vA = 0.0, uB = 0.0, vB = 0.0;
        chart(worldA, &uA, &vA);
        chart(worldB, &uB, &vB);
        double la = vB - vA;
        double lb = -(uB - uA);
        double const norm = std::sqrt(la * la + lb * lb);
        if (!(norm > 1e-9)) {
            _diagnostic = "TonicModel::SubdivideTubeAlongEdge: the stroke "
                          "is a point in the root plane";
            return false;
        }
        la /= norm;
        lb /= norm;
        a = float(la);
        b = float(lb);
        c = float(-(la * uA + lb * vA));
    }
    return SubdivideTube(tubeId, 2, "edge", seed, a, b, c, outChildren);
}

std::vector<float>
TonicModel::SmoothnessScores() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<float> out;
    int const n = int(_host.centerX.size());
    if (n < 1) {
        return out;
    }
    out.assign(size_t(n), 0.0f);
    std::string derr;
    if (!_LaneSmoothnessScore(HasCudaMirror(), _host.centerX.data(),
                              _host.centerY.data(), _host.centerZ.data(), n,
                              out.data(), &derr)) {
        out.clear();
    }
    return out;
}

bool
TonicModel::CheckRootIntersections()
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<int> ids;
    std::vector<TonicTubeDesc> descs;
    if (_host.centerX.size() >= 2) {
        ids.push_back(0);
        descs.push_back(_BuildTubeDescLocked());
        descs.back().tubeId = 0;
    }
    for (auto const &kv : _tubes) {
        ids.push_back(kv.first);
        descs.push_back(kv.second.actual);
    }
    std::vector<int> flags(descs.size(), 0);
    std::string derr;
    if (!_LaneTubeIntersect(HasCudaMirror(), descs.data(),
                            int(descs.size()), flags.data(), &derr)) {
        _diagnostic = derr;
        return false;
    }
    _intersectFlags.clear();
    for (size_t i = 0; i < ids.size(); ++i) {
        _intersectFlags[ids[i]] = flags[i];
    }
    return true;
}

std::vector<int>
TonicModel::IntersectedTubes() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<int> out;
    for (auto const &kv : _intersectFlags) {
        if (kv.second) {
            out.push_back(kv.first);
        }
    }
    return out;
}

std::vector<int>
TonicModel::TubeIds() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<int> out;
    if (_host.centerX.size() >= 2) {
        out.push_back(0);
    }
    for (auto const &kv : _tubes) {
        out.push_back(kv.first);
    }
    return out;
}

bool
TonicModel::ImportLockedTube(int parentId, float const *cx, float const *cy,
                             float const *cz, int nCv, float const *secT,
                             float const *secU, float const *secV, int nSec,
                             int ringVerts, int *outTubeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _ImportLockedTubeLocked(parentId, cx, cy, cz, nCv, secT, secU,
                                   secV, nSec, ringVerts, outTubeId);
}

bool
TonicModel::ImportSweptMesh(int parentId, float const *points, int ringCount,
                            int ringVerts, int *outTubeId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!points || !outTubeId) {
        _diagnostic = "TonicModel::ImportSweptMesh: null input or output";
        return false;
    }
    if (ringCount < 2 || ringVerts < 3 || ringVerts > 32) {
        _diagnostic = "TonicModel::ImportSweptMesh: need >= 2 rings, "
                      "ringVerts in [3, 32]";
        return false;
    }
    // Ring centroids are the centers (K4 needs >= 2 CVs: guaranteed).
    std::vector<float> cx(static_cast<size_t>(ringCount)),
        cy(static_cast<size_t>(ringCount)),
        cz(static_cast<size_t>(ringCount));
    for (int r = 0; r < ringCount; ++r) {
        double sx = 0.0, sy = 0.0, sz = 0.0;
        for (int k = 0; k < ringVerts; ++k) {
            size_t const o =
                (size_t(r) * size_t(ringVerts) + size_t(k)) * 3;
            sx += points[o + 0];
            sy += points[o + 1];
            sz += points[o + 2];
        }
        cx[size_t(r)] = float(sx / double(ringVerts));
        cy[size_t(r)] = float(sy / double(ringVerts));
        cz[size_t(r)] = float(sz / double(ringVerts));
    }
    std::vector<TonicFrame> frames;
    std::string derr;
    if (!TonicCenterFramesCpu(cx.data(), cy.data(), cz.data(), ringCount,
                              &frames, &derr)) {
        _diagnostic = derr;
        return false;
    }
    // Sections: uniform t, rings projected into the K4 charts.
    TonicTubeDesc centers;
    centers.centerX = cx;
    centers.centerY = cy;
    centers.centerZ = cz;
    std::vector<float> secT(static_cast<size_t>(ringCount));
    std::vector<float> secU(static_cast<size_t>(ringCount) *
                            static_cast<size_t>(ringVerts));
    std::vector<float> secV(static_cast<size_t>(ringCount) *
                            static_cast<size_t>(ringVerts));
    for (int r = 0; r < ringCount; ++r) {
        float const t = ringCount > 1 ? float(r) / float(ringCount - 1) : 0.0f;
        secT[size_t(r)] = t;
        float cp[3];
        TonicFrame fr;
        if (!TonicSampleCenterCpu(centers, frames, t, &cp[0], &cp[1], &cp[2],
                                  &fr, &derr)) {
            _diagnostic = derr;
            return false;
        }
        for (int k = 0; k < ringVerts; ++k) {
            size_t const o =
                (size_t(r) * size_t(ringVerts) + size_t(k)) * 3;
            float const dx = points[o + 0] - cp[0];
            float const dy = points[o + 1] - cp[1];
            float const dz = points[o + 2] - cp[2];
            size_t const s = size_t(r) * size_t(ringVerts) + size_t(k);
            secU[s] = dx * fr.nx + dy * fr.ny + dz * fr.nz;
            secV[s] = dx * fr.bx + dy * fr.by + dz * fr.bz;
        }
    }
    return _ImportLockedTubeLocked(parentId, cx.data(), cy.data(), cz.data(),
                                   ringCount, secT.data(), secU.data(),
                                   secV.data(), ringCount, ringVerts,
                                   outTubeId);
}

bool
TonicModel::_ImportLockedTubeLocked(int parentId, float const *cx,
                                    float const *cy, float const *cz, int nCv,
                                    float const *secT, float const *secU,
                                    float const *secV, int nSec, int ringVerts,
                                    int *outTubeId)
{
    if (!cx || !cy || !cz || !secT || !secU || !secV || !outTubeId) {
        _diagnostic = "TonicModel::ImportLockedTube: null input or output";
        return false;
    }
    if (nCv < 2 || nSec < 1 || ringVerts < 3 || ringVerts > 32) {
        _diagnostic = "TonicModel::ImportLockedTube: need >= 2 CVs, >= 1 "
                      "section, ringVerts in [3, 32]";
        return false;
    }
    for (int s = 0; s < nSec; ++s) {
        if (!(secT[s] >= 0.0f) || !(secT[s] <= 1.0f) ||
            (s > 0 && secT[s] < secT[s - 1])) {
            _diagnostic = "TonicModel::ImportLockedTube: section t must "
                          "ascend in [0, 1]";
            return false;
        }
    }
    int parentLevel = 0, parentRegion = 0;
    if (parentId == 0) {
        if (_host.centerX.size() < 2) {
            _diagnostic = "TonicModel::ImportLockedTube: no tube 0";
            return false;
        }
        parentLevel = 1;
    } else {
        auto pit = _tubes.find(parentId);
        if (pit == _tubes.end()) {
            _diagnostic = "TonicModel::ImportLockedTube: unknown parent";
            return false;
        }
        parentLevel = pit->second.actual.level;
        parentRegion = pit->second.actual.regionId;
    }
    // Next free childIndex under the parent (imports append after any
    // subdivided siblings); the id then cannot collide with them. A
    // fixup loop covers adversarial cases (negative group parents).
    int childIndex = 0;
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == parentId &&
            kv.second.actual.childIndex >= childIndex) {
            childIndex = kv.second.actual.childIndex + 1;
        }
    }
    int id = parentId * 16 + 1 + childIndex;
    while (id == 0 || _tubes.count(id) > 0) {
        ++childIndex;
        id = parentId * 16 + 1 + childIndex;
    }
    TonicTubeDesc actual;
    actual.centerX.assign(cx, cx + nCv);
    actual.centerY.assign(cy, cy + nCv);
    actual.centerZ.assign(cz, cz + nCv);
    actual.ringVerts = ringVerts;
    actual.tubeId = id;
    actual.regionId = parentRegion;
    actual.level = parentLevel + 1;
    actual.parentTubeId = parentId;
    actual.childIndex = childIndex;
    actual.sections.resize(size_t(nSec));
    for (int s = 0; s < nSec; ++s) {
        TonicTubeSection &sec = actual.sections[size_t(s)];
        sec.t = secT[s];
        sec.scale = 1.0f;
        sec.twist = 0.0f;
        sec.u.assign(secU + size_t(s) * size_t(ringVerts),
                     secU + (size_t(s) + 1) * size_t(ringVerts));
        sec.v.assign(secV + size_t(s) * size_t(ringVerts),
                     secV + (size_t(s) + 1) * size_t(ringVerts));
    }
    HierarchyTube entry;
    entry.actual = actual;
    entry.derived = actual;  // explicit shape: no derivation to match
    TonicClearDeltas(actual, &entry.deltas);
    entry.imported = true;
    // An import inherits its parent's fill like a subdivided child would,
    // so an artist who unlocks it later starts from the family's density.
    {
        auto pit = _tubes.find(parentId);
        entry.fill = pit != _tubes.end() ? pit->second.fill : _fill;
    }
    _PushUndoLocked();
    _tubes[id] = entry;
    *outTubeId = id;
    ++_version;
    ++_mapVersion;
    _dirty |= TonicDirty_Topology;
    return true;
}

int
TonicModel::GetTubeSectionCount(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId == 0) {
        return int(_sections.size());
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return -1;
    }
    return int(it->second.actual.sections.size());
}

bool
TonicModel::GetTubeSection(int tubeId, int ring,
                           TonicTubeSection *section) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!section) {
        return false;
    }
    if (tubeId == 0) {
        if (ring < 0 || ring >= int(_sections.size())) {
            return false;
        }
        *section = _sections[size_t(ring)];
        return true;
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end() || ring < 0 ||
        ring >= int(it->second.actual.sections.size())) {
        return false;
    }
    *section = it->second.actual.sections[size_t(ring)];
    return true;
}

bool
TonicModel::IsTubeImported(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tubes.find(tubeId);
    return it != _tubes.end() && it->second.imported;
}

// -- V0b: the stage contract (plan/18 §7 G1-G3) ------------------------------

bool
TonicModel::GetTubeDesc(int tubeId, TonicTubeDesc *out) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _TubeDescLocked(tubeId, out);
}

bool
TonicModel::GetTubeRecord(int tubeId, TubeRecord *out) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!out) {
        return false;
    }
    *out = TubeRecord();
    if (tubeId == 0) {
        if (_host.centerX.size() < 2 || !_TubeDescLocked(0, &out->actual)) {
            return false;
        }
        out->subdivide = _subdivide;
        out->fill = _fill;
        out->locked = _locked;
        out->lockParents = _lockParents;
        out->lockChildren = _lockChildren;
        out->hasTube = true;
        return true;
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return false;
    }
    HierarchyTube const &entry = it->second;
    out->actual = entry.actual;
    out->derived = entry.derived;
    out->deltas = entry.deltas;
    out->subdivide.count = entry.subdivide.count;
    out->subdivide.seed = entry.subdivide.seed;
    out->subdivide.splitMode =
        entry.subdivide.splitMode == TonicSplit_Edge ? "edge" : "kmeans";
    out->fill = entry.fill;
    // A store tube carries no independent "locked" bit: an import is the
    // frozen kind (plan/17 §5.7), and that is what the stage's
    // usdGen:tonic:locked records for a child.
    out->locked = entry.imported;
    out->lockParents = entry.lockParents;
    out->lockChildren = entry.lockChildren;
    out->transientParent = entry.transientParent;
    out->persistent = entry.persistent;
    out->imported = entry.imported;
    out->members = entry.members;
    out->hasTube = true;
    return true;
}

void
TonicModel::ClearHierarchy()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _tubes.clear();
    _intersectFlags.clear();
    _regionTube.clear();
    _tubeRegionKey.clear();
    _nextGroupId = -1;
    _ClearUndoLocked();
    ++_version;
    ++_mapVersion;
    _dirty |= TonicDirty_Topology;
}

bool
TonicModel::RestoreTubeRecord(int tubeId, TubeRecord const &record)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId == 0) {
        _diagnostic = "TonicModel::RestoreTubeRecord: tube 0 restores through "
                      "Restore()";
        return false;
    }
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        _diagnostic = "TonicModel::RestoreTubeRecord: unknown tube";
        return false;
    }
    if (record.actual.centerX.size() < 2 ||
        record.actual.sections.size() < 2) {
        _diagnostic = "TonicModel::RestoreTubeRecord: degenerate shape";
        return false;
    }
    HierarchyTube &entry = it->second;
    // The derived shape stays the one this model just re-derived from the
    // parent (hydrate subdivides before it restores), so `actual` and
    // `deltas` are installed verbatim: a hydrated tube reports exactly the
    // bytes the committer wrote, which is what the round-trip test asserts.
    TonicTubeDesc actual = record.actual;
    actual.tubeId = entry.actual.tubeId;
    actual.parentTubeId = entry.actual.parentTubeId;
    actual.childIndex = entry.actual.childIndex;
    actual.level = entry.actual.level;
    entry.actual = std::move(actual);
    if (!record.deltas.centerDu.empty()) {
        entry.deltas = record.deltas;
    }
    entry.subdivide.count = record.subdivide.count;
    entry.subdivide.seed = record.subdivide.seed;
    entry.subdivide.splitMode = record.subdivide.splitMode == "edge"
                                    ? TonicSplit_Edge
                                    : TonicSplit_KMeans;
    entry.fill = record.fill;
    entry.lockParents = record.lockParents;
    entry.lockChildren = record.lockChildren;
    entry.transientParent = record.transientParent;
    entry.persistent = record.persistent;
    if (record.imported) {
        entry.imported = true;
        entry.derived = entry.actual;
    }
    if (!record.members.empty()) {
        entry.members = record.members;
    }
    ++_version;
    _dirty |= TonicDirty_Topology | TonicDirty_Points;
    return true;
}

bool
TonicModel::SetTubeFillParams(int tubeId, FillParams params)
{
    if (tubeId == 0) {
        return SetFillParams(std::move(params));
    }
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        _diagnostic = "TonicModel::SetTubeFillParams: unknown tube";
        return false;
    }
    if (!(params.density >= 0.0f) || params.cvCount < 2 ||
        params.cvCount > 64 || !(params.edgeBias >= -1.0f) ||
        !(params.edgeBias <= 1.0f) || params.lengthProfile.size() % 2 != 0) {
        _diagnostic = "TonicModel::SetTubeFillParams: density >= 0, cvCount in "
                      "[2, 64], edgeBias in [-1, 1], paired profile";
        return false;
    }
    it->second.fill = std::move(params);
    ++_version;
    _dirty |= TonicDirty_Guides;
    return true;
}

bool
TonicModel::GetTubeFillParams(int tubeId, FillParams *out) const
{
    if (!out) {
        return false;
    }
    if (tubeId == 0) {
        *out = GetFillParams();
        return true;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        return false;
    }
    *out = it->second.fill;
    return true;
}

bool
TonicModel::IsTubeFillSuspended(int tubeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (tubeId != 0 && _tubes.find(tubeId) == _tubes.end()) {
        return false;
    }
    for (auto const &kv : _tubes) {
        if (kv.second.actual.parentTubeId == tubeId) {
            return true;
        }
    }
    return false;
}

bool
TonicModel::_FinishChildEditLocked(int tubeId, bool layoutChanged,
                                   uint32_t dirtyBits)
{
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        _diagnostic = "TonicModel: unknown tube";
        return false;
    }
    HierarchyTube &entry = it->second;
    bool const derivedChild = !entry.imported &&
                              entry.actual.parentTubeId >= 0 &&
                              entry.derived.centerX.size() >= 2;
    if (layoutChanged && derivedChild) {
        _diagnostic = "TonicModel: the operation changes a derived child's "
                      "layout; edit the parent or re-subdivide instead";
        return false;
    }
    if (derivedChild) {
        std::vector<TonicFrame> dframes;
        std::string derr;
        if (!TonicCenterFramesCpu(entry.derived.centerX.data(),
                                  entry.derived.centerY.data(),
                                  entry.derived.centerZ.data(),
                                  int(entry.derived.centerX.size()), &dframes,
                                  &derr)) {
            _diagnostic = derr;
            return false;
        }
        if (!TonicComputeDeltasCpu(entry.actual, entry.derived, dframes,
                                   &entry.deltas, &derr)) {
            _diagnostic = derr;
            return false;
        }
    } else {
        // An import or an on-the-fly parent is self-derived: it has no
        // derivation to measure against, so the deltas stay zero and the
        // baseline follows the edit (K6/K7 read it, never write it).
        entry.derived = entry.actual;
        TonicClearDeltas(entry.actual, &entry.deltas);
    }
    _dirty |= dirtyBits;
    if (!_PropagateDownLocked(tubeId)) {
        return false;
    }
    return _PropagateUpLocked(tubeId);
}

bool
TonicModel::_EditChildTube(int tubeId, bool layoutChanged, uint32_t dirtyBits,
                           std::function<bool(TonicTubeDesc &)> const &edit)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _tubes.find(tubeId);
    if (it == _tubes.end()) {
        _diagnostic = "TonicModel: unknown tube";
        return false;
    }
    HierarchyRollback const snap = _SnapshotHierarchyLocked();
    if (!edit(it->second.actual) ||
        !_FinishChildEditLocked(tubeId, layoutChanged, dirtyBits)) {
        _RestoreHierarchyLocked(snap);
        return false;
    }
    _PushUndoSnapshotLocked(snap);
    ++_version;
    return true;
}

// The per-tube editing surface (plan/18 §7 G2). Tube 0 delegates to the
// single-tube spellings above, which keep their exact behaviour (soft
// selection, the _useSections flip, the legacy shape fields); every other id
// runs the store path plus K6 down / K7 up propagation.

bool
TonicModel::InsertTubeCenterCV(int tubeId, int atIndex)
{
    if (tubeId == 0) {
        return InsertCenterCV(atIndex);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ true,
        TonicDirty_Topology | TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            int const n = int(desc.centerX.size());
            if (atIndex < 0 || atIndex > n || n < 2) {
                _diagnostic =
                    "TonicModel::InsertTubeCenterCV: index out of range";
                return false;
            }
            int i0 = atIndex - 1, i1 = atIndex;
            if (i0 < 0) {
                i0 = 0;
                i1 = 1;
            } else if (i1 >= n) {
                i0 = n - 2;
                i1 = n - 1;
            }
            float const mx =
                0.5f * (desc.centerX[size_t(i0)] + desc.centerX[size_t(i1)]);
            float const my =
                0.5f * (desc.centerY[size_t(i0)] + desc.centerY[size_t(i1)]);
            float const mz =
                0.5f * (desc.centerZ[size_t(i0)] + desc.centerZ[size_t(i1)]);
            desc.centerX.insert(desc.centerX.begin() + atIndex, mx);
            desc.centerY.insert(desc.centerY.begin() + atIndex, my);
            desc.centerZ.insert(desc.centerZ.begin() + atIndex, mz);
            return true;
        });
}

bool
TonicModel::DeleteTubeCenterCV(int tubeId, int index)
{
    if (tubeId == 0) {
        return DeleteCenterCV(index);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ true,
        TonicDirty_Topology | TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            int const n = int(desc.centerX.size());
            if (index < 0 || index >= n || n <= 2) {
                _diagnostic =
                    "TonicModel::DeleteTubeCenterCV: need >= 2 CVs left";
                return false;
            }
            desc.centerX.erase(desc.centerX.begin() + index);
            desc.centerY.erase(desc.centerY.begin() + index);
            desc.centerZ.erase(desc.centerZ.begin() + index);
            return true;
        });
}

bool
TonicModel::SetTubeLengthFor(int tubeId, float length)
{
    if (tubeId == 0) {
        return SetTubeLength(length);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            int const n = int(desc.centerX.size());
            if (!(length > 0.0f) || n < 2) {
                _diagnostic =
                    "TonicModel::SetTubeLengthFor: need a tube + length > 0";
                return false;
            }
            std::vector<float> ox, oy, oz;
            std::string derr;
            if (!TonicRescaleCenterLength(desc.centerX.data(),
                                          desc.centerY.data(),
                                          desc.centerZ.data(), n, length, &ox,
                                          &oy, &oz, &derr)) {
                _diagnostic = derr;
                return false;
            }
            desc.centerX = std::move(ox);
            desc.centerY = std::move(oy);
            desc.centerZ = std::move(oz);
            return true;
        });
}

bool
TonicModel::MatchTubeSurface(int tubeId)
{
    if (tubeId == 0) {
        return MatchSurface();
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (!_scalp || !_scalp->finalized || desc.centerX.empty()) {
                _diagnostic =
                    "TonicModel::MatchTubeSurface: need a bound scalp + tube";
                return false;
            }
            float const p[3] = {desc.centerX[0], desc.centerY[0],
                                desc.centerZ[0]};
            TonicHit const hit = TonicClosestPointCpu(*_scalp, p);
            if (!hit.hit) {
                _diagnostic = "TonicModel::MatchTubeSurface: no surface point";
                return false;
            }
            desc.centerX[0] = hit.px;
            desc.centerY[0] = hit.py;
            desc.centerZ[0] = hit.pz;
            return true;
        });
}

bool
TonicModel::SnapTubeRootToScalp(int tubeId)
{
    if (tubeId == 0) {
        return SnapRootToScalp();
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (!_scalp || !_scalp->finalized || desc.centerX.empty()) {
                _diagnostic = "TonicModel::SnapTubeRootToScalp: need a bound "
                              "scalp + tube";
                return false;
            }
            float const p[3] = {desc.centerX[0], desc.centerY[0],
                                desc.centerZ[0]};
            TonicHit const hit = TonicClosestPointCpu(*_scalp, p);
            if (!hit.hit) {
                _diagnostic =
                    "TonicModel::SnapTubeRootToScalp: no surface point";
                return false;
            }
            float const dx = hit.px - desc.centerX[0];
            float const dy = hit.py - desc.centerY[0];
            float const dz = hit.pz - desc.centerZ[0];
            for (size_t i = 0; i < desc.centerX.size(); ++i) {
                desc.centerX[i] += dx;
                desc.centerY[i] += dy;
                desc.centerZ[i] += dz;
            }
            return true;
        });
}

bool
TonicModel::RelaxTubeCenter(int tubeId, float strength, int iterations)
{
    if (tubeId == 0) {
        return RelaxCenter(strength, iterations);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            int const n = int(desc.centerX.size());
            if (!(strength >= 0.0f) || !(strength <= 1.0f) ||
                iterations < 1 || n < 3) {
                _diagnostic =
                    "TonicModel::RelaxTubeCenter: strength in [0, 1], "
                    "iterations >= 1, need >= 3 CVs";
                return false;
            }
            float const lo = 0.5f * kTonicSmoothnessSpike;
            float const span = kTonicSmoothnessSpike - lo;
            for (int it = 0; it < iterations; ++it) {
                std::vector<float> scores(size_t(n), 0.0f);
                std::string derr;
                if (!_LaneSmoothnessScore(
                        HasCudaMirror(), desc.centerX.data(),
                        desc.centerY.data(), desc.centerZ.data(), n,
                        scores.data(), &derr)) {
                    _diagnostic = derr;
                    return false;
                }
                std::vector<float> nx = desc.centerX, ny = desc.centerY,
                                   nz = desc.centerZ;
                for (int i = 1; i + 1 < n; ++i) {
                    float w = (scores[size_t(i)] - lo) / span;
                    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
                    float const ax = 0.5f * (desc.centerX[size_t(i - 1)] +
                                             desc.centerX[size_t(i + 1)]);
                    float const ay = 0.5f * (desc.centerY[size_t(i - 1)] +
                                             desc.centerY[size_t(i + 1)]);
                    float const az = 0.5f * (desc.centerZ[size_t(i - 1)] +
                                             desc.centerZ[size_t(i + 1)]);
                    nx[size_t(i)] += (ax - nx[size_t(i)]) * strength * w;
                    ny[size_t(i)] += (ay - ny[size_t(i)]) * strength * w;
                    nz[size_t(i)] += (az - nz[size_t(i)]) * strength * w;
                }
                desc.centerX = std::move(nx);
                desc.centerY = std::move(ny);
                desc.centerZ = std::move(nz);
            }
            return true;
        });
}

bool
TonicModel::MoveTubeSectionRing(int tubeId, int ring, float du, float dv)
{
    if (tubeId == 0) {
        return MoveSectionRing(ring, du, dv);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (ring < 0 || ring >= int(desc.sections.size())) {
                _diagnostic =
                    "TonicModel::MoveTubeSectionRing: ring out of range";
                return false;
            }
            TonicTubeSection &s = desc.sections[size_t(ring)];
            for (size_t i = 0; i < s.u.size(); ++i) {
                s.u[i] += du;
                s.v[i] += dv;
            }
            return true;
        });
}

bool
TonicModel::ScaleTubeSectionRing(int tubeId, int ring, float scale)
{
    if (tubeId == 0) {
        return ScaleSectionRing(ring, scale);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (ring < 0 || ring >= int(desc.sections.size()) ||
                !(scale > 0.0f)) {
                _diagnostic =
                    "TonicModel::ScaleTubeSectionRing: bad ring or scale";
                return false;
            }
            desc.sections[size_t(ring)].scale *= scale;
            return true;
        });
}

bool
TonicModel::TwistTubeSectionRing(int tubeId, int ring, float radians)
{
    if (tubeId == 0) {
        return TwistSectionRing(ring, radians);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (ring < 0 || ring >= int(desc.sections.size())) {
                _diagnostic =
                    "TonicModel::TwistTubeSectionRing: ring out of range";
                return false;
            }
            desc.sections[size_t(ring)].twist += radians;
            return true;
        });
}

bool
TonicModel::MoveTubeSectionCV(int tubeId, int ring, int slot, float du,
                              float dv)
{
    if (tubeId == 0) {
        return MoveSectionCV(ring, slot, du, dv);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (ring < 0 || ring >= int(desc.sections.size()) || slot < 0 ||
                slot >= int(desc.sections[size_t(ring)].u.size())) {
                _diagnostic =
                    "TonicModel::MoveTubeSectionCV: ring/slot out of range";
                return false;
            }
            TonicTubeSection &s = desc.sections[size_t(ring)];
            s.u[size_t(slot)] += du;
            s.v[size_t(slot)] += dv;
            return true;
        });
}

bool
TonicModel::AddTubeSectionRing(int tubeId, float t)
{
    if (tubeId == 0) {
        return AddSectionRing(t);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ true,
        TonicDirty_Topology | TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            int const nSec = int(desc.sections.size());
            if (nSec < 2) {
                _diagnostic = "TonicModel::AddTubeSectionRing: no tube";
                return false;
            }
            float const t0 = desc.sections.front().t;
            float const t1 = desc.sections.back().t;
            if (!(t > t0) || !(t < t1)) {
                _diagnostic = "TonicModel::AddTubeSectionRing: t must sit "
                              "strictly inside the section range";
                return false;
            }
            int k = 0;
            while (k + 1 < nSec - 1 && desc.sections[size_t(k + 1)].t < t) {
                ++k;
            }
            TonicTubeSection const &s0 = desc.sections[size_t(k)];
            TonicTubeSection const &s1 = desc.sections[size_t(k + 1)];
            float const span = s1.t - s0.t;
            float const f = span > 0.0f ? (t - s0.t) / span : 0.0f;
            TonicTubeSection ins;
            ins.t = t;
            ins.scale = s0.scale + (s1.scale - s0.scale) * f;
            ins.twist = s0.twist + (s1.twist - s0.twist) * f;
            ins.u.resize(s0.u.size());
            ins.v.resize(s0.v.size());
            for (size_t i = 0; i < s0.u.size(); ++i) {
                ins.u[i] = s0.u[i] + (s1.u[i] - s0.u[i]) * f;
                ins.v[i] = s0.v[i] + (s1.v[i] - s0.v[i]) * f;
            }
            desc.sections.insert(desc.sections.begin() + (k + 1),
                                 std::move(ins));
            return true;
        });
}

bool
TonicModel::RemoveTubeSectionRing(int tubeId, int ring)
{
    if (tubeId == 0) {
        return RemoveSectionRing(ring);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ true,
        TonicDirty_Topology | TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (ring < 0 || ring >= int(desc.sections.size()) ||
                desc.sections.size() <= 2) {
                _diagnostic =
                    "TonicModel::RemoveTubeSectionRing: need >= 2 rings left";
                return false;
            }
            desc.sections.erase(desc.sections.begin() + ring);
            return true;
        });
}

bool
TonicModel::CopyTubeSectionRing(int tubeId, int src, int dst)
{
    if (tubeId == 0) {
        return CopySectionRing(src, dst);
    }
    return _EditChildTube(
        tubeId, /*layoutChanged*/ false, TonicDirty_Points,
        [&](TonicTubeDesc &desc) {
            if (src < 0 || src >= int(desc.sections.size()) || dst < 0 ||
                dst >= int(desc.sections.size())) {
                _diagnostic =
                    "TonicModel::CopyTubeSectionRing: ring out of range";
                return false;
            }
            float const t = desc.sections[size_t(dst)].t;
            desc.sections[size_t(dst)] = desc.sections[size_t(src)];
            desc.sections[size_t(dst)].t = t;
            return true;
        });
}

// -- V0: viewport publication views + display state (plan/18 §2.1, §2.2) -----

std::vector<TonicModel::TubeView>
TonicModel::SnapshotTubes() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<TubeView> out;
    out.reserve(_tubes.size() + 1);
    if (_host.centerX.size() >= 2) {
        TubeView view;
        if (_TubeDescLocked(0, &view.desc)) {
            out.push_back(std::move(view));
        }
    }
    for (auto const &kv : _tubes) {
        TubeView view;
        view.desc = kv.second.actual;
        view.desc.tubeId = kv.first;
        view.imported = kv.second.imported;
        view.persistent = kv.second.persistent;
        view.transientParent = kv.second.transientParent;
        out.push_back(std::move(view));
    }
    return out;
}

int
TonicModel::GetMaxTubeLevel() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    int level = _host.centerX.size() >= 2 ? 1 : 0;
    for (auto const &kv : _tubes) {
        level = std::max(level, kv.second.actual.level);
    }
    return level;
}

bool
TonicModel::SetLevelDisplay(int level, bool visible, bool xray)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (level < 1) {
        _diagnostic = "TonicModel::SetLevelDisplay: level must be >= 1";
        return false;
    }
    LevelDisplay next;
    next.visible = visible;
    next.xray = xray;
    auto it = _levelDisplay.find(level);
    if (it != _levelDisplay.end()) {
        // The two-argument form owns visibility and x-ray only: a ladder
        // that dropped this level to centers-only keeps that until it
        // restores it (plan/18 §3.7), and the policy's x-ray strength and
        // centers flag survive a Levels-panel visibility toggle.
        next.centersOnly = it->second.centersOnly;
        next.xrayOpacity = it->second.xrayOpacity;
        next.centers = it->second.centers;
        next.guides = it->second.guides;
        if (it->second.visible == next.visible &&
            it->second.xray == next.xray) {
            return true;
        }
    } else if (next.visible && !next.xray) {
        return true;  // already the default
    }
    _levelDisplay[level] = next;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

bool
TonicModel::SetLevelDrawMode(int level, bool visible, bool xray,
                             bool centersOnly)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (level < 1) {
        _diagnostic = "TonicModel::SetLevelDrawMode: level must be >= 1";
        return false;
    }
    LevelDisplay next;
    next.visible = visible;
    next.xray = xray;
    next.centersOnly = centersOnly;
    auto it = _levelDisplay.find(level);
    if (it != _levelDisplay.end()) {
        next.xrayOpacity = it->second.xrayOpacity;
        next.centers = it->second.centers;
        next.guides = it->second.guides;
        if (it->second.visible == next.visible &&
            it->second.xray == next.xray &&
            it->second.centersOnly == next.centersOnly) {
            return true;
        }
    } else if (next.visible && !next.xray && !next.centersOnly) {
        return true;  // already the default
    }
    _levelDisplay[level] = next;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

bool
TonicModel::SetLevelDraw(int level, LevelDisplay const &draw)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (level < 1) {
        _diagnostic = "TonicModel::SetLevelDraw: level must be >= 1";
        return false;
    }
    LevelDisplay next = draw;
    next.xrayOpacity = std::max(0.0f, std::min(1.0f, next.xrayOpacity));
    auto it = _levelDisplay.find(level);
    if (it != _levelDisplay.end()) {
        // The policy owns the look, the ladder owns centers-only: a policy
        // push in the middle of a heavy drag must not restore the level the
        // ladder just dropped (plan/18 §3.7).
        next.centersOnly = it->second.centersOnly;
        if (it->second.visible == next.visible &&
            it->second.xray == next.xray &&
            it->second.xrayOpacity == next.xrayOpacity &&
            it->second.centers == next.centers &&
            it->second.guides == next.guides) {
            return true;
        }
    } else {
        LevelDisplay const dflt;
        if (next.visible == dflt.visible && next.xray == dflt.xray &&
            next.xrayOpacity == dflt.xrayOpacity &&
            next.centers == dflt.centers && next.guides == dflt.guides &&
            !next.centersOnly) {
            return true;  // already the default
        }
    }
    _levelDisplay[level] = next;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

bool
TonicModel::SetRingDisplay(int mode)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (mode < Rings_Off || mode > Rings_All) {
        _diagnostic = "TonicModel::SetRingDisplay: mode is 0..2";
        return false;
    }
    if (_ringDisplay == mode) {
        return true;
    }
    _ringDisplay = mode;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

int
TonicModel::GetRingDisplay() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _ringDisplay;
}

bool
TonicModel::SetAmplifiedHair(bool show)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_amplifiedHair == show) {
        return true;
    }
    _amplifiedHair = show;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

bool
TonicModel::GetAmplifiedHair() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _amplifiedHair;
}

void
TonicModel::ResolveHairDisplay(bool *tiles, bool *guides) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    bool const gesture = _gestureDepth > 0;
    if (tiles) {
        *tiles = _amplifiedHair && !gesture;
    }
    if (guides) {
        // The preview steps aside only for tiles that are actually up.
        *guides = !(_amplifiedHair && !gesture);
    }
}

TonicModel::LevelDisplay
TonicModel::GetLevelDisplay(int level) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _levelDisplay.find(level);
    return it == _levelDisplay.end() ? LevelDisplay() : it->second;
}

bool
TonicModel::SetFocusLevel(int level)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (level < 0) {
        _diagnostic = "TonicModel::SetFocusLevel: level must be >= 0";
        return false;
    }
    if (level == _focusLevel) {
        return true;
    }
    _focusLevel = level;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

int
TonicModel::GetFocusLevel() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _focusLevel;
}

bool
TonicModel::SetDisplayScale(float worldPerPixel)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!(worldPerPixel >= 0.0f) || !(worldPerPixel < 1e30f)) {
        _diagnostic =
            "TonicModel::SetDisplayScale: want a finite value >= 0";
        return false;
    }
    if (worldPerPixel == _displayScale) {
        return true;
    }
    _displayScale = worldPerPixel;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

float
TonicModel::GetDisplayScale() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _displayScale;
}

bool
TonicModel::SetGroomPath(std::string const &path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_groomPath == path) {
        return true;
    }
    _groomPath = path;
    _dirty |= TonicDirty_Display;
    ++_version;
    return true;
}

std::string
TonicModel::GetGroomPath() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _groomPath;
}

// -- V0 clump palette (plan/18 §2.4a) ----------------------------------------

namespace {

// The plan's 16 sRGB hex entries, in order.
uint32_t const kTonicPaletteSrgb[16] = {
    0x2F6BFFu, 0xFFD400u, 0xE0248Fu, 0x4CD62Bu,
    0x8A3FFFu, 0xFF7A1Au, 0x00C8D6u, 0xFF3B3Bu,
    0xA6E22Eu, 0xF062F0u, 0x1FA3FFu, 0xFFB000u,
    0x17C77Au, 0xC43CFFu, 0xFF5E9Au, 0x7BD3FFu,
};

float _SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

int
TonicClumpPaletteSize()
{
    return 16;
}

TonicRgb
TonicClumpPaletteEntry(int slot)
{
    int const n = TonicClumpPaletteSize();
    int const i = ((slot % n) + n) % n;
    uint32_t const hex = kTonicPaletteSrgb[i];
    TonicRgb out;
    out.r = _SrgbToLinear(float((hex >> 16) & 0xFFu) / 255.0f);
    out.g = _SrgbToLinear(float((hex >> 8) & 0xFFu) / 255.0f);
    out.b = _SrgbToLinear(float(hex & 0xFFu) / 255.0f);
    return out;
}

TonicRgb
TonicClumpColor(int regionId, int level, int childIndex)
{
    TonicRgb base = TonicClumpPaletteEntry(regionId);
    if (level <= 1 || childIndex < 0) {
        return base;
    }
    // Alternating lighten/darken, one step per sibling PAIR.
    //
    // Both operations keep the hue exactly. Lightening is a lerp towards
    // white, so (g-b) and (max-min) are scaled by the same (1-amount) and
    // their ratio — the hue — is unchanged; darkening scales all three
    // channels, which changes nothing but value. That is what "one lock,
    // one hue family" means and what the T0 test checks.
    //
    // The step sizes are the V9 correction. V8 used 0.12/(level-1), which
    // is ±12 % of the remaining headroom at L2 and ±6 % at L3 — below the
    // spread the shader's own key/fill/ambient rig puts across a single
    // curved tube, so siblings were measurably different and visually
    // identical (the 2026-09-18 braid frame). 0.22 with a 0.10 widening
    // per further sibling pair, shrinking by 0.8 per level below 2, keeps
    // an L4 sibling at ±14 % and still separates neighbours on screen.
    int const magnitude = childIndex / 2 + 1;
    float const direction = (childIndex % 2 == 0) ? 1.0f : -1.0f;
    float step = 0.22f + 0.10f * float(magnitude - 1);
    for (int l = 2; l < level; ++l) {
        step *= 0.8f;
    }
    float amount = direction * step;
    amount = std::max(-0.6f, std::min(0.6f, amount));
    float const *src[3] = {&base.r, &base.g, &base.b};
    TonicRgb out;
    float *dst[3] = {&out.r, &out.g, &out.b};
    for (int c = 0; c < 3; ++c) {
        float const v = *src[c];
        *dst[c] = amount >= 0.0f ? v + (1.0f - v) * amount
                                 : v * (1.0f + amount);
    }
    return out;
}

// -- V9 display policy (plan/18 section 2.4a) --------------------------------
//
// The table itself lives in the header comment; this is the only place it
// is evaluated. Pure functions, no lock, no model: Tonic_SetDisplayPolicy
// walks the levels and hands each answer to TonicModel::SetLevelDraw.

int
TonicDisplayModeFromNames(char const *mode, char const *subMode)
{
    if (!mode) {
        return -1;
    }
    std::string const m(mode);
    std::string const s(subMode ? subMode : "");
    if (m == "graph") {
        return TonicDisplayMode_Graph;
    }
    if (m == "tube") {
        // Ring and Section both edit the cross-sections, and both want the
        // rings readable against an opaque surface.
        return (s == "ring" || s == "section") ? TonicDisplayMode_TubeRing
                                               : TonicDisplayMode_TubeCenter;
    }
    if (m == "fill") {
        return TonicDisplayMode_Fill;
    }
    if (m == "hierarchy") {
        return TonicDisplayMode_Hierarchy;
    }
    if (m == "sculpt") {
        return TonicDisplayMode_Sculpt;
    }
    if (m == "output") {
        return TonicDisplayMode_Output;
    }
    return -1;
}

TonicModel::LevelDisplay
TonicPolicyLevelDisplay(int displayMode, int level, int focusLevel)
{
    TonicModel::LevelDisplay out;
    out.visible = true;
    out.xray = false;
    out.xrayOpacity = TonicModel::kDefaultXrayOpacity;
    out.centers = true;
    // The guide preview belongs to Fill (where it is being authored) and
    // Output (where it is the result). Everywhere else it is a wall of
    // hair in front of the control geometry the mode exists to edit.
    out.guides = displayMode == TonicDisplayMode_Fill ||
                 displayMode == TonicDisplayMode_Output;
    // "Nothing focused" must not mean "everything is a ghost": with no
    // focus every level reads as the focused one.
    bool const focused = focusLevel <= 0 || level == focusLevel;
    switch (displayMode) {
    case TonicDisplayMode_Graph:
    case TonicDisplayMode_Output:
        out.centers = false;
        return out;
    case TonicDisplayMode_TubeRing:
        if (!focused) {
            out.xray = true;
            out.xrayOpacity = TonicModel::kDefaultXrayOpacity;
        }
        return out;
    case TonicDisplayMode_Fill:
        out.xray = true;
        out.xrayOpacity = TonicModel::kDefaultXrayOpacity;
        return out;
    case TonicDisplayMode_TubeCenter:
    case TonicDisplayMode_Hierarchy:
    case TonicDisplayMode_Sculpt:
        out.xray = true;
        out.xrayOpacity = focused ? TonicModel::kDefaultXrayOpacity
                                  : TonicModel::kFaintXrayOpacity;
        return out;
    default:
        return out;
    }
}

int
TonicPolicyRingDisplay(int displayMode)
{
    switch (displayMode) {
    case TonicDisplayMode_Graph:
    case TonicDisplayMode_Output:
        return TonicModel::Rings_Off;
    case TonicDisplayMode_TubeRing:
        return TonicModel::Rings_All;
    default:
        return TonicModel::Rings_Selected;
    }
}

} // namespace usdGenTonic
