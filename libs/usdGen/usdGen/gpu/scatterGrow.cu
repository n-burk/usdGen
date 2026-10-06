#include "scatterGrow.h"
#include "cudaCompat.h"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace usdGen::gpu {
namespace {
constexpr int kNonFinite = 2;
constexpr int kPendingStatus = std::numeric_limits<int>::min();
ScatterGrowStatus Status(cudaError_t e) { return e == cudaSuccess ? ScatterGrowStatus::Ok : ScatterGrowStatus::CudaError; }
bool Finite(double v) { return std::isfinite(v); }
bool Finite(float v) { return std::isfinite(v); }
bool Finite(float2 v) { return Finite(v.x) && Finite(v.y); }
bool Finite(float3 v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }

__device__ uint64_t Hash64(uint64_t key, uint32_t salt) {
    uint64_t z = (key ^ (uint64_t(salt) * 0x9E3779B97F4A7C15ull)) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
__device__ float DrawGrow(int seed, uint64_t id, uint32_t salt = 0x47726F77u) {
    uint64_t key = Hash64(uint64_t(uint32_t(seed)), salt) ^ id;
    return float(uint32_t(Hash64(key, salt) >> 32) >> 8) * 0x1.0p-24f;
}
__device__ float3 Normalize(float3 v) {
    float l2 = v.x*v.x + v.y*v.y + v.z*v.z;
    // Match CPU Grow's authored-direction fallback.
    float length = sqrtf(l2);
    if (!(length > 1.0e-12f) || !isfinite(length)) return make_float3(0, 1, 0);
    return make_float3(v.x/length, v.y/length, v.z/length);
}
__device__ float3 RotateAroundB(float3 direction, float3 axis, float degrees) {
    if (degrees == 0.0f) return direction;
    axis = Normalize(axis);
    constexpr float pi = 3.14159265358979323846f;
    float const radians = degrees * (pi / 180.0f);
    float const c = cosf(radians), s = sinf(radians);
    float const dot = axis.x * direction.x + axis.y * direction.y + axis.z * direction.z;
    float3 const cross = make_float3(
        axis.y * direction.z - axis.z * direction.y,
        axis.z * direction.x - axis.x * direction.z,
        axis.x * direction.y - axis.y * direction.x);
    float const oneMinusC = 1.0f - c;
    return make_float3(
        direction.x * c + cross.x * s + axis.x * dot * oneMinusC,
        direction.y * c + cross.y * s + axis.y * dot * oneMinusC,
        direction.z * c + cross.z * s + axis.z * dot * oneMinusC);
}
__device__ bool FiniteDevice(float x) { return isfinite(x); }
__device__ bool FiniteDevice(float3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}
uint64_t Hash64Host(uint64_t key, uint32_t salt) {
    uint64_t z = (key ^ (uint64_t(salt) * 0x9E3779B97F4A7C15ull)) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float DrawGrowHost(int seed, uint64_t id, uint32_t salt = 0x47726F77u) {
    uint64_t key = Hash64Host(uint64_t(uint32_t(seed)), salt) ^ id;
    return float(uint32_t(Hash64Host(key, salt) >> 32) >> 8) * 0x1.0p-24f;
}
float3 NormalizeHost(float3 v) {
    float const l2 = v.x*v.x + v.y*v.y + v.z*v.z;
    float const length = std::sqrt(l2);
    if (!(length > 1.0e-12f) || !Finite(length)) return make_float3(0, 1, 0);
    return make_float3(v.x/length, v.y/length, v.z/length);
}
float3 RotateAroundBHost(float3 direction, float3 axis, float degrees) {
    if (degrees == 0.0f) return direction;
    axis = NormalizeHost(axis);
    constexpr float pi = 3.14159265358979323846f;
    float const radians = degrees * (pi / 180.0f);
    float const c = std::cos(radians), s = std::sin(radians);
    float const dot = axis.x * direction.x + axis.y * direction.y + axis.z * direction.z;
    float3 const cross = make_float3(
        axis.y * direction.z - axis.z * direction.y,
        axis.z * direction.x - axis.x * direction.z,
        axis.x * direction.y - axis.y * direction.x);
    float const oneMinusC = 1.0f - c;
    return make_float3(
        direction.x * c + cross.x * s + axis.x * dot * oneMinusC,
        direction.y * c + cross.y * s + axis.y * dot * oneMinusC,
        direction.z * c + cross.z * s + axis.z * dot * oneMinusC);
}
__global__ void GrowKernel(float3 const* roots, uint64_t const* ids,
    int32_t const* rootPrimIn, float2 const* rootUVIn, float3 const* rootTIn,
    float3 const* rootBIn, float3 const* rootNIn, uint32_t curves, uint32_t cvCount,
    int seed, double length, double lo, double hi, float lift, float azimuth, float azimuthRandom, float width,
    ScatterGrowDirection direction, float3 literal,
    float3* points, float3* rest, float* widths, float* hairT, uint32_t* offsets,
    uint64_t* outIds, int32_t* rootPrim, float2* rootUV, float3* rootT,
    float3* rootB, float3* rootN, int* error) {
    uint32_t c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= curves) return;
    offsets[c] = c * cvCount;
    if (c == curves - 1) offsets[curves] = curves * cvCount;
    float3 dir = direction == ScatterGrowDirection::RootNormal ? rootNIn[c] :
        direction == ScatterGrowDirection::RootTangent ? rootTIn[c] : literal;
    dir = Normalize(dir);
    dir = RotateAroundB(dir, rootBIn[c], lift);
    float const angle = azimuth + azimuthRandom * 360.0f *
        (DrawGrow(seed, ids[c], 0x4772417Au) - 0.5f); // kSaltGrowAzimuth
    dir = RotateAroundB(dir, rootNIn[c], angle);
    // Match CPU Grow: random/length arithmetic is double, then the captured
    // per-curve target is narrowed to float.
    double targetDouble = length * (lo + double(DrawGrow(seed, ids[c])) * (hi - lo));
    float target = float(targetDouble);
    if (!FiniteDevice(target)) { atomicCAS(error, 0, kNonFinite); return; }
    uint32_t first = c * cvCount;
    for (uint32_t i = 0; i < cvCount; ++i) {
        float t = float(i) / float(cvCount - 1);
        float d = target * t;
        if (!FiniteDevice(d)) { atomicCAS(error, 0, kNonFinite); return; }
        float3 p = make_float3(roots[c].x + dir.x*d, roots[c].y + dir.y*d, roots[c].z + dir.z*d);
        if (!FiniteDevice(p)) { atomicCAS(error, 0, kNonFinite); return; }
        points[first+i] = rest[first+i] = p; widths[first+i] = width; hairT[first+i] = t;
    }
    outIds[c] = ids[c]; rootPrim[c] = rootPrimIn[c]; rootUV[c] = rootUVIn[c];
    rootT[c] = rootTIn[c]; rootB[c] = rootBIn[c]; rootN[c] = rootNIn[c];
}

template <class T> cudaError_t Allocate(DeviceBuffer<T>& dst, std::vector<T> const& src,
                                         UsdGenExecutionMemoryReservation* r) { return dst.reset(src.size(), r); }
template <class T> cudaError_t Copy(DeviceBuffer<T>& dst, std::vector<T> const& src, cudaStream_t s) {
    return src.empty() ? cudaSuccess : cudaMemcpyAsync(dst.data(), src.data(), src.size()*sizeof(T), cudaMemcpyHostToDevice, s);
}
} // namespace

ScatterGrowStatus GetScatterGrowRequirements(size_t curves, uint32_t cvs,
                                             ScatterGrowRequirements* result) {
    if (!result || cvs < 2 || cvs > 64) return ScatterGrowStatus::InvalidArgument;
    if (curves > std::numeric_limits<uint32_t>::max() / cvs)
        return ScatterGrowStatus::InvalidTopology;
    ScatterGrowRequirements candidate;
    candidate.pointCount = curves * cvs;
    // Input: root position, ID, face, UV, and three frame vectors.
    size_t const rootBytes = 4 * sizeof(float3) + sizeof(uint64_t) +
        sizeof(int32_t) + sizeof(float2);
    size_t const pointBytes = 2 * sizeof(float3) + 2 * sizeof(float);
    size_t const curveBytes = 3 * sizeof(float3) + sizeof(uint64_t) +
        sizeof(int32_t) + sizeof(float2) + sizeof(uint32_t);
    auto multiply = [](size_t a, size_t b, size_t* out) {
        if (a > std::numeric_limits<size_t>::max() / b) return false;
        *out = a * b;
        return true;
    };
    auto add = [](size_t* out, size_t value) {
        if (value > std::numeric_limits<size_t>::max() - *out) return false;
        *out += value;
        return true;
    };
    size_t curveOutput = 0;
    if (!multiply(curves, rootBytes, &candidate.inputBytes) ||
        !multiply(candidate.pointCount, pointBytes, &candidate.outputBytes) ||
        !multiply(curves, curveBytes, &curveOutput) ||
        !add(&candidate.outputBytes, curveOutput) ||
        !add(&candidate.outputBytes, sizeof(uint32_t)))
        return ScatterGrowStatus::InvalidTopology;
    candidate.peakBytes = candidate.inputBytes;
    if (!add(&candidate.peakBytes, candidate.outputBytes))
        return ScatterGrowStatus::InvalidTopology;
    candidate.statusBytes = 2 * sizeof(int); // device status + pinned relay status
    if (!add(&candidate.peakBytes, candidate.statusBytes))
        return ScatterGrowStatus::InvalidTopology;
    *result = candidate;
    return ScatterGrowStatus::Ok;
}

void CudaScatterGrow::Storage::quarantine() noexcept { points.quarantine(); restPoints.quarantine(); rootT.quarantine(); rootB.quarantine(); rootN.quarantine(); widths.quarantine(); hairT.quarantine(); offsets.quarantine(); stableIds.quarantine(); rootPrim.quarantine(); rootUV.quarantine(); }
size_t CudaScatterGrow::Storage::bytes() const noexcept { return points.bytes()+restPoints.bytes()+rootT.bytes()+rootB.bytes()+rootN.bytes()+widths.bytes()+hairT.bytes()+offsets.bytes()+stableIds.bytes()+rootPrim.bytes()+rootUV.bytes(); }
void CudaScatterGrow::Storage::Reclassify(UsdGenExecutionResourceKind kind) noexcept { points.Reclassify(kind); restPoints.Reclassify(kind); rootT.Reclassify(kind); rootB.Reclassify(kind); rootN.Reclassify(kind); widths.Reclassify(kind); hairT.Reclassify(kind); offsets.Reclassify(kind); stableIds.Reclassify(kind); rootPrim.Reclassify(kind); rootUV.Reclassify(kind); }
ScatterGrowStatus CudaScatterGrow::Storage::recordUse(cudaStream_t s) { cudaError_t e=points.recordUse(s); if(e==cudaSuccess)e=restPoints.recordUse(s); if(e==cudaSuccess)e=widths.recordUse(s); if(e==cudaSuccess)e=hairT.recordUse(s); if(e==cudaSuccess)e=offsets.recordUse(s); if(e==cudaSuccess)e=stableIds.recordUse(s); if(e==cudaSuccess)e=rootPrim.recordUse(s); if(e==cudaSuccess)e=rootUV.recordUse(s); if(e==cudaSuccess)e=rootT.recordUse(s); if(e==cudaSuccess)e=rootB.recordUse(s); if(e==cudaSuccess)e=rootN.recordUse(s); return Status(e); }
ScatterGrowStatus CudaScatterGrow::Storage::waitOn(cudaStream_t s) const { cudaError_t e=points.waitOn(s); if(e==cudaSuccess)e=restPoints.waitOn(s); if(e==cudaSuccess)e=widths.waitOn(s); if(e==cudaSuccess)e=hairT.waitOn(s); if(e==cudaSuccess)e=offsets.waitOn(s); if(e==cudaSuccess)e=stableIds.waitOn(s); if(e==cudaSuccess)e=rootPrim.waitOn(s); if(e==cudaSuccess)e=rootUV.waitOn(s); if(e==cudaSuccess)e=rootT.waitOn(s); if(e==cudaSuccess)e=rootB.waitOn(s); if(e==cudaSuccess)e=rootN.waitOn(s); return Status(e); }
ScatterGrowStatus CudaScatterGrow::Storage::synchronizeUse() const { cudaError_t e=points.synchronizeUse(); if(e==cudaSuccess)e=restPoints.synchronizeUse(); if(e==cudaSuccess)e=widths.synchronizeUse(); if(e==cudaSuccess)e=hairT.synchronizeUse(); if(e==cudaSuccess)e=offsets.synchronizeUse(); if(e==cudaSuccess)e=stableIds.synchronizeUse(); if(e==cudaSuccess)e=rootPrim.synchronizeUse(); if(e==cudaSuccess)e=rootUV.synchronizeUse(); if(e==cudaSuccess)e=rootT.synchronizeUse(); if(e==cudaSuccess)e=rootB.synchronizeUse(); if(e==cudaSuccess)e=rootN.synchronizeUse(); return Status(e); }

CudaScatterGrow::~CudaScatterGrow() {
    if (unprovenWork_) {
        active_.quarantine(); pending_.quarantine(); pendingInput_.quarantine(); error_.quarantine();
        // The device destination and immutable host source belong to the
        // same unproved transfer. Neither may be freed on this path.
        (void)rootsQuarantineOwner_.release();
        hostError_ = nullptr;
        hostErrorPermit_.Abandon();
        ready_ = nullptr; return;
    }
    int prior=-1; bool owns=active_.points.size()||pending_.points.size()||error_.size()||
        ready_||hostError_;
    bool selected=!owns || (cudaGetDevice(&prior)==cudaSuccess && deviceIndex_>=0 && cudaSetDevice(deviceIndex_)==cudaSuccess);
    bool proved=!owns || (selected && (!ready_ || cudaEventSynchronize(ready_)==cudaSuccess) &&
        active_.synchronizeUse()==ScatterGrowStatus::Ok && error_.synchronizeUse()==cudaSuccess);
    if (!proved) {
        active_.quarantine(); pending_.quarantine(); pendingInput_.quarantine(); error_.quarantine();
        (void)rootsQuarantineOwner_.release();
        hostError_ = nullptr;
        hostErrorPermit_.Abandon();
        ready_=nullptr;
    } else {
        if (ready_) cudaEventDestroy(ready_);
        if (hostError_) {
            if (cudaFreeHost(hostError_) == cudaSuccess) hostErrorPermit_.Release();
            else hostErrorPermit_.Abandon();
            hostError_ = nullptr;
        }
    }
    if (selected && prior>=0 && prior!=deviceIndex_) cudaSetDevice(prior);
}

ScatterGrowStatus CudaScatterGrow::validateStream(cudaStream_t stream) const {
    // Preflight belongs before any allocation: a first-use foreign/captured
    // stream must not bind this owner or enqueue root copies.
    int current=-1; if(cudaGetDevice(&current)!=cudaSuccess)return ScatterGrowStatus::CudaError;
    if(deviceIndex_>=0 && current!=deviceIndex_)return ScatterGrowStatus::InvalidArgument;
    cudaStreamCaptureStatus capture;
    if(cudaStreamIsCapturing(stream,&capture)!=cudaSuccess)return ScatterGrowStatus::CudaError;
    if(capture!=cudaStreamCaptureStatusNone)return ScatterGrowStatus::InvalidArgument;
    // cudaStreamGetDevice is not capture-safe on every supported runtime.
    if(stream) { int d=-1; if(cudaStreamGetDevice(stream,&d)!=cudaSuccess)return ScatterGrowStatus::CudaError; if(d!=current || (deviceIndex_>=0 && d!=deviceIndex_))return ScatterGrowStatus::InvalidArgument; }
    return ScatterGrowStatus::Ok;
}
ScatterGrowStatus CudaScatterGrow::validate(
    std::shared_ptr<const ScatterGrowRoots> const& r, ScatterGrowControls const& c,
    size_t* total) const {
    return ValidateRoots(r, c, total);
}
ScatterGrowStatus CudaScatterGrow::ValidateRoots(
    std::shared_ptr<const ScatterGrowRoots> const& r, ScatterGrowControls const& c,
    size_t* total) {
    if(!r || c.cvCount<2 || c.cvCount>64 || !Finite(c.length)||!Finite(c.randomLo)||
       !Finite(c.randomHi)||!Finite(c.lift)||!Finite(c.fallbackWidth)||c.length<0||
       c.randomLo<0||c.randomHi<0||c.fallbackWidth<0 || c.lift < -90.0f ||
       c.lift > 90.0f || c.direction>ScatterGrowDirection::Literal ||
       !Finite(c.azimuth) || c.azimuth < -360.0f || c.azimuth > 360.0f ||
       !Finite(c.azimuthRandom) || c.azimuthRandom < 0.0f || c.azimuthRandom > 1.0f ||
       (c.direction==ScatterGrowDirection::Literal&&!Finite(c.literalDirection)))
        return ScatterGrowStatus::InvalidArgument;
    size_t n=r->positions.size();
    if(r->stableIds.size()!=n||r->rootPrim.size()!=n||r->rootUV.size()!=n||
       r->rootT.size()!=n||r->rootB.size()!=n||r->rootN.size()!=n)
        return ScatterGrowStatus::InvalidTopology;
    ScatterGrowRequirements requirements;
    auto requirementStatus = GetScatterGrowRequirements(n, c.cvCount, &requirements);
    if (requirementStatus != ScatterGrowStatus::Ok) return requirementStatus;
    *total = requirements.pointCount;
    // Duplicate stable ids through a small open-addressed set: the same
    // insertion order and the same first-dup-in-index-order report as the
    // unordered_set, without its per-node allocation. Capacity is a power
    // of two past 2n (load <= 0.5); the SplitMix avalanche keeps
    // sequential untrusted ids from clustering.
    size_t setCap = 16;
    while (setCap <= n) setCap *= 2;
    setCap *= 2;
    std::vector<uint64_t> setKeys(setCap);
    std::vector<unsigned char> setUsed(setCap, 0);
    size_t const setMask = setCap - 1;
    for(size_t i=0;i<n;++i) {
        if(!Finite(r->positions[i])||!Finite(r->rootUV[i])||!Finite(r->rootT[i])||
           !Finite(r->rootB[i])||!Finite(r->rootN[i]))
            return ScatterGrowStatus::NonFiniteInput;
        uint64_t const id = r->stableIds[i];
        size_t slot = size_t(Hash64Host(id, 0x9E3779B9u) & uint64_t(setMask));
        while (true) {
            if (!setUsed[slot]) {
                setUsed[slot] = 1;
                setKeys[slot] = id;
                break;
            }
            if (setKeys[slot] == id)
                return ScatterGrowStatus::DuplicateStableId;
            slot = (slot + 1) & setMask;
        }

        // Catch deterministic target and output overflow before reserving or
        // submitting any work.  The device repeats this check and reports a
        // native status as well, since float contraction can differ at the
        // last bit near FLT_MAX.
        float3 direction = c.direction == ScatterGrowDirection::RootNormal ? r->rootN[i] :
            c.direction == ScatterGrowDirection::RootTangent ? r->rootT[i] : c.literalDirection;
        direction = RotateAroundBHost(NormalizeHost(direction), r->rootB[i], c.lift);
        float const azimuth = c.azimuth + c.azimuthRandom * 360.0f *
            (DrawGrowHost(c.seed, r->stableIds[i], 0x4772417Au) - 0.5f);
        direction = RotateAroundBHost(direction, r->rootN[i], azimuth);
        if (!Finite(direction)) return ScatterGrowStatus::NonFiniteInput;
        double const targetDouble = c.length *
            (c.randomLo + double(DrawGrowHost(c.seed, r->stableIds[i])) *
             (c.randomHi - c.randomLo));
        float const target = static_cast<float>(targetDouble);
        if (!Finite(target)) return ScatterGrowStatus::NonFiniteInput;
        for (uint32_t j = 0; j != c.cvCount; ++j) {
            float const t = float(j) / float(c.cvCount - 1);
            float const distance = target * t;
            float3 const output = make_float3(
                r->positions[i].x + direction.x * distance,
                r->positions[i].y + direction.y * distance,
                r->positions[i].z + direction.z * distance);
            if (!Finite(distance) || !Finite(output))
                return ScatterGrowStatus::NonFiniteInput;
        }
    }
    return ScatterGrowStatus::Ok;
}
void CudaScatterGrow::discardPending() noexcept {
    pending_=Storage{};
    pendingInput_=Storage{};
    error_.reset(0);
    if (hostError_) {
        if (cudaFreeHost(hostError_) == cudaSuccess) hostErrorPermit_.Release();
        else hostErrorPermit_.Abandon();
        hostError_ = nullptr;
    }
    rootsOwner_.reset(); rootsQuarantineOwner_.reset();
    pendingWork_=finishScheduled_=false; pendingCurves_=pendingPoints_=0;
}
ScatterGrowStatus CudaScatterGrow::BeginFresh(
    std::shared_ptr<const ScatterGrowRoots> roots, ScatterGrowControls controls,
    cudaStream_t stream, UsdGenExecutionMemoryReservation* reserve) {
    if(pendingWork_||generation_)return ScatterGrowStatus::InvalidArgument; auto s=validateStream(stream); if(s!=ScatterGrowStatus::Ok)return s; if(controls.randomLo>controls.randomHi)std::swap(controls.randomLo,controls.randomHi); size_t total=0; s=validate(roots,controls,&total); if(s!=ScatterGrowStatus::Ok)return s;
    int d=-1; if(cudaGetDevice(&d)!=cudaSuccess)return ScatterGrowStatus::CudaError; if(deviceIndex_<0)deviceIndex_=d;
    Storage in; // temporary input storage, kept alive through terminal proof
    cudaError_t e=Allocate(in.points,roots->positions,reserve); if(e==cudaSuccess)e=Allocate(in.stableIds,roots->stableIds,reserve); if(e==cudaSuccess)e=Allocate(in.rootPrim,roots->rootPrim,reserve); if(e==cudaSuccess)e=Allocate(in.rootUV,roots->rootUV,reserve); if(e==cudaSuccess)e=Allocate(in.rootT,roots->rootT,reserve); if(e==cudaSuccess)e=Allocate(in.rootB,roots->rootB,reserve); if(e==cudaSuccess)e=Allocate(in.rootN,roots->rootN,reserve);
    if(e!=cudaSuccess)return Status(e);
    e=pending_.points.reset(total,reserve); if(e==cudaSuccess)e=pending_.restPoints.reset(total,reserve); if(e==cudaSuccess)e=pending_.widths.reset(total,reserve); if(e==cudaSuccess)e=pending_.hairT.reset(total,reserve); if(e==cudaSuccess)e=pending_.offsets.reset(roots->positions.size()+1,reserve); if(e==cudaSuccess)e=pending_.stableIds.reset(roots->positions.size(),reserve); if(e==cudaSuccess)e=pending_.rootPrim.reset(roots->positions.size(),reserve); if(e==cudaSuccess)e=pending_.rootUV.reset(roots->positions.size(),reserve); if(e==cudaSuccess)e=pending_.rootT.reset(roots->positions.size(),reserve); if(e==cudaSuccess)e=pending_.rootB.reset(roots->positions.size(),reserve); if(e==cudaSuccess)e=pending_.rootN.reset(roots->positions.size(),reserve); if(e==cudaSuccess)e=error_.reset(1,reserve,UsdGenExecutionResourceKind::Scratch); if(e!=cudaSuccess){discardPending(); return Status(e);}
    if (!hostError_) {
        auto permit = TryReserveCudaExecutionBytes(sizeof(int),
            UsdGenExecutionResourceKind::Scratch, reserve);
        if (!permit || cudaHostAlloc(reinterpret_cast<void**>(&hostError_), sizeof(int),
                                     cudaHostAllocDefault) != cudaSuccess) {
            discardPending();
            return ScatterGrowStatus::CudaError;
        }
        hostErrorPermit_ = std::move(*permit);
    }
    try {
        rootsQuarantineOwner_ = std::make_unique<std::shared_ptr<const ScatterGrowRoots>>(roots);
    } catch (...) {
        discardPending();
        return ScatterGrowStatus::CudaError;
    }
    rootsOwner_=std::move(roots); pendingCurves_=rootsOwner_->positions.size(); pendingPoints_=total; pendingWork_=true;
    producerStream_ = stream;
    // From this point H2D copies and the kernel may be in flight.  A later
    // failure has no completion proof and must quarantine rather than free.
    pendingInput_=std::move(in); unprovenWork_=true;
    *hostError_ = kPendingStatus;
    e = cudaMemsetAsync(error_.data(), 0, sizeof(int), stream);
    if (e != cudaSuccess) return Status(e);
    e=Copy(pendingInput_.points,rootsOwner_->positions,stream); if(e==cudaSuccess)e=Copy(pendingInput_.stableIds,rootsOwner_->stableIds,stream); if(e==cudaSuccess)e=Copy(pendingInput_.rootPrim,rootsOwner_->rootPrim,stream); if(e==cudaSuccess)e=Copy(pendingInput_.rootUV,rootsOwner_->rootUV,stream); if(e==cudaSuccess)e=Copy(pendingInput_.rootT,rootsOwner_->rootT,stream); if(e==cudaSuccess)e=Copy(pendingInput_.rootB,rootsOwner_->rootB,stream); if(e==cudaSuccess)e=Copy(pendingInput_.rootN,rootsOwner_->rootN,stream);
    if(e!=cudaSuccess) return Status(e);
    if (!pendingCurves_) {
        e = cudaMemsetAsync(pending_.offsets.data(), 0, sizeof(uint32_t), stream);
        if (e != cudaSuccess) return Status(e);
        return ScatterGrowStatus::Ok;
    }
    GrowKernel<<<(unsigned(pendingCurves_)+127)/128,128,0,stream>>>(pendingInput_.points.data(),pendingInput_.stableIds.data(),pendingInput_.rootPrim.data(),pendingInput_.rootUV.data(),pendingInput_.rootT.data(),pendingInput_.rootB.data(),pendingInput_.rootN.data(),uint32_t(pendingCurves_),controls.cvCount,controls.seed,controls.length,controls.randomLo,controls.randomHi,controls.lift,controls.azimuth,controls.azimuthRandom,controls.fallbackWidth,controls.direction,controls.literalDirection,pending_.points.data(),pending_.restPoints.data(),pending_.widths.data(),pending_.hairT.data(),pending_.offsets.data(),pending_.stableIds.data(),pending_.rootPrim.data(),pending_.rootUV.data(),pending_.rootT.data(),pending_.rootB.data(),pending_.rootN.data(),error_.data());
    e=cudaGetLastError(); if(e!=cudaSuccess)return Status(e); return ScatterGrowStatus::Ok;
}

ScatterGrowStatus CudaScatterGrow::FinishFreshAsync(
    cudaStream_t stream,
    void (*callback)(cudaStream_t, cudaError_t, void*) noexcept, void* userdata) {
    if (!pendingWork_ || finishScheduled_ || !callback)
        return ScatterGrowStatus::NoPendingUpdate;
    if (stream != producerStream_) return ScatterGrowStatus::InvalidArgument;
    auto status = validateStream(stream);
    if (status != ScatterGrowStatus::Ok) return status;
    if (!ready_ && cudaEventCreateWithFlags(&ready_, cudaEventDisableTiming) != cudaSuccess)
        return ScatterGrowStatus::CudaError;
    status = pending_.recordUse(stream);
    if (status == ScatterGrowStatus::Ok) status = pendingInput_.recordUse(stream);
    if (status == ScatterGrowStatus::Ok) status = Status(error_.recordUse(stream));
    if (status != ScatterGrowStatus::Ok) return status;
    auto error = cudaEventRecord(ready_, stream);
    if (error != cudaSuccess) return Status(error);
    error = cudaMemcpyAsync(hostError_, error_.data(), sizeof(int),
                            cudaMemcpyDeviceToHost, stream);
    if (error != cudaSuccess) return Status(error);
    error = cudaStreamAddCallback(stream, callback, userdata, 0);
    // A rejected installation does not authorize commit. The caller still
    // owns the unproved producer and must retain it or establish cleanup proof.
    if (error == cudaSuccess) finishScheduled_ = true;
    return Status(error);
}
ScatterGrowStatus CudaScatterGrow::CommitFreshFinish() {
    if(!pendingWork_||!finishScheduled_) return ScatterGrowStatus::NoPendingUpdate;
    if (!hostError_ || *hostError_ == kPendingStatus)
        return ScatterGrowStatus::NoPendingUpdate;
    if (*hostError_ != 0) {
        ScatterGrowStatus const status = *hostError_ == kNonFinite
            ? ScatterGrowStatus::NonFiniteInput : ScatterGrowStatus::InvalidTopology;
        unprovenWork_ = false;
        discardPending();
        return status;
    }
    active_=std::move(pending_); pendingInput_=Storage{};
    rootsOwner_.reset(); rootsQuarantineOwner_.reset();
    curves_=pendingCurves_; points_=pendingPoints_;
    pendingCurves_=pendingPoints_=0; pendingWork_=finishScheduled_=false;
    unprovenWork_=false; ++generation_; return ScatterGrowStatus::Ok;
}
DeviceCurveGeometryView CudaScatterGrow::view() const { return {active_.points.view(),active_.restPoints.view(),active_.widths.view(),active_.offsets.view(),active_.stableIds.view(),curves_,points_}; }
DeviceView<const float> CudaScatterGrow::hairT() const{return active_.hairT.view();} DeviceView<const int32_t> CudaScatterGrow::rootPrim() const{return active_.rootPrim.view();} DeviceView<const float2> CudaScatterGrow::rootUV() const{return active_.rootUV.view();} DeviceView<const float3> CudaScatterGrow::rootT() const{return active_.rootT.view();} DeviceView<const float3> CudaScatterGrow::rootB() const{return active_.rootB.view();} DeviceView<const float3> CudaScatterGrow::rootN() const{return active_.rootN.view();}
ScatterGrowStatus CudaScatterGrow::recordUse(cudaStream_t s){auto x=validateStream(s);return x==ScatterGrowStatus::Ok?active_.recordUse(s):x;} ScatterGrowStatus CudaScatterGrow::waitOn(cudaStream_t s) const{auto x=validateStream(s);return x==ScatterGrowStatus::Ok?active_.waitOn(s):x;} size_t CudaScatterGrow::ExclusiveRetainedBytes() const noexcept{return active_.bytes()+error_.bytes()+hostErrorPermit_.Bytes();}
void CudaScatterGrow::ReclassifyPublishedGeneration() noexcept {
    active_.Reclassify(UsdGenExecutionResourceKind::Pinned);
    error_.Reclassify(UsdGenExecutionResourceKind::Pinned);
    hostErrorPermit_.Reclassify(UsdGenExecutionResourceKind::Pinned);
}
} // namespace usdGen::gpu
