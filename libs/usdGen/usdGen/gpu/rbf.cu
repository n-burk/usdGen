#include "rbf.h"
#include "cudaCompat.h"
#include "deviceResources.h"

#include <cmath>
#include <climits>
#include <limits>
#include <atomic>

namespace usdGen { namespace gpu {
namespace {
std::atomic<bool> failFreshBindAllocation{false}, failFreshBindAfterSubmit{false};
std::atomic<bool> failFreshSolveAllocation{false}, failFreshSolveAfterInputSubmit{false},
    failFreshSolveAfterSolverSubmit{false};
std::atomic<bool> failFreshEvaluateAllocation{false}, failFreshEvaluateAfterSubmit{false};
std::atomic<bool> failFreshResolvePreflight{false}, failFreshResolveCommit{false};
std::atomic<uint64_t> freshAcceptAttempts{0}, freshRollbackAttempts{0};
__device__ inline void mark(int* f, int v) { atomicOr(f, v); }
__device__ inline void atomicMinFloat(float* address, float value) {
    int* bits = reinterpret_cast<int*>(address); int old = *bits, assumed;
    do { assumed = old; if (__int_as_float(assumed) <= value) break;
         old = atomicCAS(bits, assumed, __float_as_int(value)); } while (old != assumed);
}
__device__ inline void atomicMaxFloat(float* address, float value) {
    int* bits = reinterpret_cast<int*>(address); int old = *bits, assumed;
    do { assumed = old; if (__int_as_float(assumed) >= value) break;
         old = atomicCAS(bits, assumed, __float_as_int(value)); } while (old != assumed);
}
__global__ void extentKernel(const float3* p, int n, float* e, int* flags) {
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += blockDim.x * gridDim.x) {
        float3 v = p[i];
        if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) { mark(flags, 1); continue; }
        atomicMinFloat(&e[0], v.x); atomicMinFloat(&e[1], v.y); atomicMinFloat(&e[2], v.z);
        atomicMaxFloat(&e[3], v.x); atomicMaxFloat(&e[4], v.y); atomicMaxFloat(&e[5], v.z);
    }
}
__global__ void initExtent(float* e, int* flags) {
    if (threadIdx.x == 0) { e[0]=e[1]=e[2]=INFINITY; e[3]=e[4]=e[5]=-INFINITY; *flags=0; }
}
__global__ void polynomialGram(const float3* p, int n, double* gram, double cx, double cy, double cz, double invScale) {
    for (int i=blockIdx.x*blockDim.x+threadIdx.x;i<n;i+=blockDim.x*gridDim.x) {
        float3 q=p[i]; double v[4]={1.,(q.x-cx)*invScale,(q.y-cy)*invScale,(q.z-cz)*invScale};
        for(int r=0;r<4;++r) for(int c=0;c<4;++c) atomicAdd(&gram[r*4+c],v[r]*v[c]);
    }
}
bool fullAffineRank(double g[16]) {
    // Scaled Gaussian elimination on P^T P.  A relative threshold catches
    // tilted/coplanar layouts that happen not to produce an exact LU zero.
    double maxDiag=0.; for(int i=0;i<4;++i) maxDiag=std::max(maxDiag,std::abs(g[i*4+i]));
    if(!(maxDiag>0.) || !std::isfinite(maxDiag)) return false;
    for(int c=0;c<4;++c) { int pivot=c; for(int r=c+1;r<4;++r) if(std::abs(g[r*4+c])>std::abs(g[pivot*4+c]))pivot=r;
        if(std::abs(g[pivot*4+c]) <= maxDiag*1e-11) return false;
        if(pivot!=c) for(int k=c;k<4;++k) std::swap(g[c*4+k],g[pivot*4+k]);
        for(int r=c+1;r<4;++r) { double q=g[r*4+c]/g[c*4+c]; for(int k=c;k<4;++k) g[r*4+k]-=q*g[c*4+k]; }
    } return true;
}

cudaError_t validateFreshPointer(const void* p, size_t count, cudaStream_t stream, int* device) {
    cudaStreamCaptureStatus capture = cudaStreamCaptureStatusNone;
    if (cudaStreamIsCapturing(stream, &capture) != cudaSuccess || capture != cudaStreamCaptureStatusNone)
        return cudaErrorInvalidValue;
    if (cudaGetDevice(device) != cudaSuccess) return cudaErrorInvalidDevice;
    int streamDevice = -1;
    if (stream && (cudaStreamGetDevice(stream, &streamDevice) != cudaSuccess || streamDevice != *device))
        return cudaErrorInvalidDevice;
    if (!p || !count) return p || !count ? cudaSuccess : cudaErrorInvalidValue;
    cudaPointerAttributes attributes{};
    if (cudaPointerGetAttributes(&attributes, p) != cudaSuccess ||
        attributes.type != cudaMemoryTypeDevice || attributes.device != *device)
        return cudaErrorInvalidDevicePointer;
    return cudaSuccess;
}
__global__ void buildMatrix(const float3* p, double* a, int n, int m, double cx, double cy, double cz, double invScale, double lambda, int* flags) {
    int k = blockIdx.x * blockDim.x + threadIdx.x;
    if (k >= m*m) return;
    int row = k % m, col = k / m; // column major
    double value = 0.0;
    if (row < n && col < n) {
        float3 x=p[row], y=p[col];
        double dx=(x.x-cx)*invScale-(y.x-cx)*invScale, dy=(x.y-cy)*invScale-(y.y-cy)*invScale, dz=(x.z-cz)*invScale-(y.z-cz)*invScale;
        double r=sqrt(dx*dx+dy*dy+dz*dz); value=r*r*r + (row==col ? lambda : 0.0);
    } else if (row < n) { float3 x=p[row]; int q=col-n; value=q==0?1.0:(q==1?(x.x-cx)*invScale:(q==2?(x.y-cy)*invScale:(x.z-cz)*invScale)); }
    else if (col < n) { float3 x=p[col]; int q=row-n; value=q==0?1.0:(q==1?(x.x-cx)*invScale:(q==2?(x.y-cy)*invScale:(x.z-cz)*invScale)); }
    a[k]=value;
}
__global__ void rhsKernel(const float3* rest, const float3* current, double* rhs, int n, int m, double invScale, int* flags) {
    int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=n) return;
    float3 a=rest[i], b=current[i];
    if(!isfinite(b.x)||!isfinite(b.y)||!isfinite(b.z)) { mark(flags,1); return; }
    rhs[i]=((double)b.x-a.x)*invScale; rhs[i+m]=((double)b.y-a.y)*invScale; rhs[i+2*m]=((double)b.z-a.z)*invScale;
}
__global__ void zeroTail(double* rhs, int n, int m) { int i=blockIdx.x*blockDim.x+threadIdx.x; if(i<4) rhs[n+i]=rhs[m+n+i]=rhs[2*m+n+i]=0.0; }
__global__ void normalizeSamples(const float3* p, double* sn, int n, double cx, double cy, double cz, double invScale) {
    int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=n) return;
    float3 s=p[i];
    sn[i]=(s.x-cx)*invScale; sn[n+i]=(s.y-cy)*invScale; sn[2*n+i]=(s.z-cz)*invScale;
}
__global__ void evalKernel(const float3* cvs, float3* out, int c, const double* sn, const double* coef, int n, int m, double cx, double cy, double cz, double invScale, double scale, int* flags) {
    int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=c) return; float3 p=cvs[i];
    if(!isfinite(p.x)||!isfinite(p.y)||!isfinite(p.z)){mark(flags,1);return;}
    double x=(p.x-cx)*invScale,y=(p.y-cy)*invScale,z=(p.z-cz)*invScale;
    double ox=x+coef[n]+coef[n+1]*x+coef[n+2]*y+coef[n+3]*z;
    double oy=y+coef[m+n]+coef[m+n+1]*x+coef[m+n+2]*y+coef[m+n+3]*z;
    double oz=z+coef[2*m+n]+coef[2*m+n+1]*x+coef[2*m+n+2]*y+coef[2*m+n+3]*z;
    // sn holds (sample-center)*invScale per axis (normalizeSamples, once per
    // bind): the same doubles the inline normalization computed, so every
    // iteration below is bitwise what it was, minus 3 converts + 9 flops.
// Rolled on purpose: sm_121 lowers each sqrt to a software-routine call
    // that serializes the warp, so unrolling only spends registers (50 at
    // 4x, 40 rolled) and costs occupancy. The op order is unchanged.
#pragma unroll 1
    for(int j=0;j<n;++j){ double dx=x-sn[j],dy=y-sn[n+j],dz=z-sn[2*n+j]; double r=sqrt(dx*dx+dy*dy+dz*dz); r*=r*r; ox+=coef[j]*r;oy+=coef[m+j]*r;oz+=coef[2*m+j]*r; }
    out[i]=make_float3((float)(ox*scale+cx),(float)(oy*scale+cy),(float)(oz*scale+cz));
}
inline bool ok(cudaError_t e) { return e==cudaSuccess; }
}

void TestFailNextFreshRbfBindAllocation() noexcept { failFreshBindAllocation.store(true, std::memory_order_release); }
void TestFailNextFreshRbfBindAfterSubmit() noexcept { failFreshBindAfterSubmit.store(true, std::memory_order_release); }
void TestFailNextFreshRbfSolveAllocation() noexcept { failFreshSolveAllocation.store(true, std::memory_order_release); }
void TestFailNextFreshRbfSolveAfterInputSubmit() noexcept { failFreshSolveAfterInputSubmit.store(true, std::memory_order_release); }
void TestFailNextFreshRbfSolveAfterSolverSubmit() noexcept { failFreshSolveAfterSolverSubmit.store(true, std::memory_order_release); }
void TestFailNextFreshRbfEvaluateAllocation() noexcept { failFreshEvaluateAllocation.store(true, std::memory_order_release); }
void TestFailNextFreshRbfEvaluateAfterSubmit() noexcept { failFreshEvaluateAfterSubmit.store(true, std::memory_order_release); }
void TestFailNextFreshRbfResolvePreflight() noexcept { failFreshResolvePreflight.store(true, std::memory_order_release); }
void TestFailNextFreshRbfResolveCommit() noexcept { failFreshResolveCommit.store(true, std::memory_order_release); }
uint64_t FreshRbfAcceptAttemptCountForTesting() noexcept { return freshAcceptAttempts.load(std::memory_order_acquire); }
uint64_t FreshRbfRollbackAttemptCountForTesting() noexcept { return freshRollbackAttempts.load(std::memory_order_acquire); }

cudaError_t GetCudaRbfLuWorkspaceElements(size_t sampleCount,
                                          size_t* elements) noexcept {
    if (!elements || sampleCount < 4 || sampleCount > 46336u)
        return cudaErrorInvalidValue;
    *elements = 0;
    cusolverDnHandle_t solver = nullptr;
    if (cusolverDnCreate(&solver) != CUSOLVER_STATUS_SUCCESS)
        return cudaErrorUnknown;
    int const order = static_cast<int>(sampleCount) + 4;
    int work = -1;
    // bufferSize depends on dimensions and the selected implementation; it
    // does not dereference A. A null-storage query avoids allocating the very
    // matrix whose aggregate ticket is being computed.
    auto const query = cusolverDnDgetrf_bufferSize(
        solver, order, order, nullptr, order, &work);
    auto const destroy = cusolverDnDestroy(solver);
    if (query != CUSOLVER_STATUS_SUCCESS || destroy != CUSOLVER_STATUS_SUCCESS ||
        work < 0)
        return cudaErrorUnknown;
    *elements = static_cast<size_t>(work);
    return cudaSuccess;
}

struct CudaRbfBinding::FreshState {
    enum class Phase { Extent, ExtentReady, Rank, RankReady, Lu, SolveInput,
                       SolveInputReady, Solve, Evaluate, Complete };
    DeviceBuffer<float3> rest, current;
    DeviceBuffer<double> matrix, work, coefficients, gram, norm;
    DeviceBuffer<int> pivots, info, flags;
    DeviceBuffer<float> extents;
    cusolverDnHandle_t solver = nullptr;
    struct Packet { float extent[6]; double gram[16]; int flag = 0, info = 0; } *host = nullptr;
    UsdGenExecutionResourcePermit hostPermit;
    FreshState* factorOwner = nullptr;
    int device = -1, n = 0, m = 0, lwork = 0;
    double smoothing = 0, center[3] = {}, scale = 1;
    Phase phase = Phase::Extent;
    bool unproven = false, failed = false;
    ~FreshState() {
        if (unproven) { abandon(); return; }
        if (device < 0) { hostPermit.Abandon(); return; }
        int old = -1; bool const got = cudaGetDevice(&old) == cudaSuccess;
        if (device >= 0 && cudaSetDevice(device) != cudaSuccess) { abandon(); return; }
        if (solver) cusolverDnDestroy(solver);
        if (host) { if (cudaFreeHost(host) == cudaSuccess) hostPermit.Release(); else hostPermit.Abandon(); }
        if (got && old != device) cudaSetDevice(old);
    }
    void abandon() noexcept {
        rest.quarantine(); current.quarantine(); matrix.quarantine(); work.quarantine(); coefficients.quarantine(); gram.quarantine(); norm.quarantine();
        pivots.quarantine(); info.quarantine(); flags.quarantine(); extents.quarantine();
        solver = nullptr; host = nullptr; hostPermit.Abandon();
    }
};

bool CudaRbfBinding::HasUnprovenWork() const noexcept {
    return (fresh_ && fresh_->unproven) || (freshSolve_ && freshSolve_->unproven) ||
        (freshEval_ && freshEval_->unproven);
}
size_t CudaRbfBinding::freshSampleCount() const { return acceptedFresh_ ? size_t(acceptedFresh_->n) : 0; }
void CudaRbfBinding::AbandonFresh() noexcept {
    if (fresh_ && fresh_->unproven) fresh_->abandon();
    if (freshSolve_ && freshSolve_->unproven) {
        freshSolve_->abandon();
        // cuSOLVER reads these accepted factorization buffers while producing
        // the private coefficient candidate.
        if (acceptedFresh_) acceptedFresh_->abandon();
    }
    if (freshEval_ && freshEval_->unproven) {
        freshEval_->abandon();
        // The evaluator may still dereference these factors on its stream.
        if (acceptedFresh_) acceptedFresh_->abandon();
    }
}

RbfStatus CudaRbfBinding::BeginFreshBind(DeviceView<const float3> samples, double smoothing,
                                         cudaStream_t stream,
                                         UsdGenExecutionMemoryReservation* reservation) {
    if (HasUnprovenWork() || evalPending_ || samples.size < 4 || samples.size > 46336u ||
        !std::isfinite(smoothing) || smoothing < 0. || freshSolve_ ||
        (fresh_ && fresh_->phase != FreshState::Phase::Complete))
        return fail(RbfStatus::InvalidArgument, "invalid fresh RBF rest binding");
    int ownerDevice = -1;
    if (validateFreshPointer(samples.data, samples.size, stream, &ownerDevice) != cudaSuccess)
        return fail(RbfStatus::InvalidArgument, "fresh RBF input/stream provenance invalid");
    auto fresh = std::make_unique<FreshState>();
    fresh->device = ownerDevice;
    // A prior candidate reached a host-proven terminal state.  This is an
    // ordinary worker-side retirement point, never a fresh commit.
    // Only a proven previous *pending* candidate is retired here.  The
    // accepted factorization remains live until a later LU commit succeeds.
    retiredFresh_.reset(); retiredSolve_.reset(); retiredEval_.reset();
    // A rejected/proven pending candidate is retired at this ordinary Begin,
    // leaving CommitLU's old-accepted parking slot empty.
    if (fresh_ && !fresh_->unproven) fresh_.reset();
    fresh->n = static_cast<int>(samples.size); fresh->m = fresh->n + 4; fresh->smoothing = smoothing;
    if (failFreshBindAllocation.exchange(false, std::memory_order_acq_rel))
        return fail(RbfStatus::CudaError, "forced fresh RBF bind allocation failure");
    auto permit = TryReserveCudaExecutionBytes(sizeof(FreshState::Packet), UsdGenExecutionResourceKind::Cache, reservation);
    if (!permit || cudaHostAlloc(reinterpret_cast<void**>(&fresh->host), sizeof(*fresh->host), cudaHostAllocDefault) != cudaSuccess)
        return fail(RbfStatus::CudaError, "fresh RBF proof packet allocation failed");
    fresh->hostPermit = std::move(*permit);
    if (!ok(fresh->rest.reset(fresh->n, reservation, UsdGenExecutionResourceKind::Cache)) || !ok(fresh->norm.reset(3*size_t(fresh->n), reservation, UsdGenExecutionResourceKind::Cache)) || !ok(fresh->matrix.reset(size_t(fresh->m)*fresh->m, reservation, UsdGenExecutionResourceKind::Cache)) ||
        !ok(fresh->coefficients.reset(size_t(fresh->m)*3, reservation, UsdGenExecutionResourceKind::Cache)) || !ok(fresh->pivots.reset(fresh->m, reservation, UsdGenExecutionResourceKind::Cache)) ||
        !ok(fresh->info.reset(1, reservation, UsdGenExecutionResourceKind::Cache)) || !ok(fresh->flags.reset(1, reservation, UsdGenExecutionResourceKind::Cache)) || !ok(fresh->gram.reset(16, reservation, UsdGenExecutionResourceKind::Cache)) ||
        !ok(fresh->extents.reset(6, reservation, UsdGenExecutionResourceKind::Cache)) || cusolverDnCreate(&fresh->solver) != CUSOLVER_STATUS_SUCCESS ||
        cusolverDnDgetrf_bufferSize(fresh->solver, fresh->m, fresh->m, fresh->matrix.data(), fresh->m, &fresh->lwork) != CUSOLVER_STATUS_SUCCESS ||
        !ok(fresh->work.reset(fresh->lwork, reservation, UsdGenExecutionResourceKind::Cache))) return fail(RbfStatus::CudaError, "fresh RBF candidate allocation failed");
    fresh_ = std::move(fresh); auto& f = *fresh_;
    f.unproven = true; f.phase = FreshState::Phase::Extent;
    if (!ok(cudaMemcpyAsync(f.rest.data(), samples.data, f.n*sizeof(float3), cudaMemcpyDeviceToDevice, stream))) { f.failed=true; return fail(RbfStatus::CudaError,"fresh RBF rest copy failed"); }
    initExtent<<<1,1,0,stream>>>(f.extents.data(), f.flags.data()); extentKernel<<<32,128,0,stream>>>(f.rest.data(),f.n,f.extents.data(),f.flags.data());
    if (cudaGetLastError()!=cudaSuccess || !ok(cudaMemcpyAsync(f.host->extent,f.extents.data(),sizeof(f.host->extent),cudaMemcpyDeviceToHost,stream)) || !ok(cudaMemcpyAsync(&f.host->flag,f.flags.data(),sizeof(int),cudaMemcpyDeviceToHost,stream))) { f.failed=true; return fail(RbfStatus::CudaError,"fresh RBF extent submit failed"); }
    if (failFreshBindAfterSubmit.exchange(false, std::memory_order_acq_rel)) { f.failed=true; return fail(RbfStatus::CudaError,"forced fresh RBF bind post-submit failure"); }
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::CommitFreshBindExtent() {
    if (!fresh_ || fresh_->phase != FreshState::Phase::Extent || !fresh_->unproven || fresh_->failed) return fail(RbfStatus::InvalidArgument,"fresh RBF extent proof missing");
    auto& f=*fresh_; f.unproven=false;
    if (f.host->flag) { f.phase=FreshState::Phase::Complete; return fail(RbfStatus::NonFiniteInput,"RBF rest samples contain non-finite values"); }
    f.center[0]=double(f.host->extent[0])+(double(f.host->extent[3])-f.host->extent[0])*.5; f.center[1]=double(f.host->extent[1])+(double(f.host->extent[4])-f.host->extent[1])*.5; f.center[2]=double(f.host->extent[2])+(double(f.host->extent[5])-f.host->extent[2])*.5;
    f.scale=std::max(double(f.host->extent[3])-f.host->extent[0],std::max(double(f.host->extent[4])-f.host->extent[1],double(f.host->extent[5])-f.host->extent[2]));
    if (!(f.scale>0.) || !std::isfinite(f.scale)) { f.phase=FreshState::Phase::Complete; return fail(RbfStatus::RankDeficient,"RBF rest samples have zero extent"); }
    f.phase=FreshState::Phase::ExtentReady; return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::BeginFreshBindRank(cudaStream_t stream) {
    if (!fresh_ || fresh_->phase != FreshState::Phase::ExtentReady || HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF extent not committed");
    auto& f=*fresh_; int d=-1; if (validateFreshPointer(nullptr,0,stream,&d)!=cudaSuccess || d!=f.device) return fail(RbfStatus::InvalidArgument,"fresh RBF stream invalid");
    f.unproven=true; f.phase=FreshState::Phase::Rank;
    if (!ok(cudaMemsetAsync(f.gram.data(),0,16*sizeof(double),stream))) { f.failed=true; return fail(RbfStatus::CudaError,"fresh RBF rank reset failed"); }
    polynomialGram<<<32,128,0,stream>>>(f.rest.data(),f.n,f.gram.data(),f.center[0],f.center[1],f.center[2],1./f.scale);
    if (cudaGetLastError()!=cudaSuccess || !ok(cudaMemcpyAsync(f.host->gram,f.gram.data(),sizeof(f.host->gram),cudaMemcpyDeviceToHost,stream))) { f.failed=true; return fail(RbfStatus::CudaError,"fresh RBF rank submit failed"); }
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::CommitFreshBindRank() {
    if (!fresh_ || fresh_->phase != FreshState::Phase::Rank || !fresh_->unproven || fresh_->failed) return fail(RbfStatus::InvalidArgument,"fresh RBF rank proof missing");
    fresh_->unproven=false; if (!fullAffineRank(fresh_->host->gram)) { fresh_->phase=FreshState::Phase::Complete; return fail(RbfStatus::RankDeficient,"RBF samples lack numerically full affine 3D support"); }
    fresh_->phase=FreshState::Phase::RankReady; return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::BeginFreshBindLu(cudaStream_t stream) {
    if (!fresh_ || fresh_->phase != FreshState::Phase::RankReady || HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF rank not committed");
    auto& f=*fresh_; int d=-1; if (validateFreshPointer(nullptr,0,stream,&d)!=cudaSuccess || d!=f.device) return fail(RbfStatus::InvalidArgument,"fresh RBF stream invalid");
    f.unproven=true; f.phase=FreshState::Phase::Lu;
    normalizeSamples<<<(f.n+255)/256,256,0,stream>>>(f.rest.data(),f.norm.data(),f.n,f.center[0],f.center[1],f.center[2],1./f.scale);
    buildMatrix<<<(f.m*f.m+255)/256,256,0,stream>>>(f.rest.data(),f.matrix.data(),f.n,f.m,f.center[0],f.center[1],f.center[2],1./f.scale,f.smoothing,f.flags.data());
    if (cudaGetLastError()!=cudaSuccess || cusolverDnSetStream(f.solver,stream)!=CUSOLVER_STATUS_SUCCESS || cusolverDnDgetrf(f.solver,f.m,f.m,f.matrix.data(),f.m,f.work.data(),f.pivots.data(),f.info.data())!=CUSOLVER_STATUS_SUCCESS || !ok(cudaMemsetAsync(f.coefficients.data(),0,f.coefficients.size()*sizeof(double),stream)) || !ok(cudaMemcpyAsync(&f.host->info,f.info.data(),sizeof(int),cudaMemcpyDeviceToHost,stream))) { f.failed=true; return fail(RbfStatus::SolverError,"fresh RBF LU submit failed"); }
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::CommitFreshBindLu() {
    if (!fresh_ || fresh_->phase != FreshState::Phase::Lu || !fresh_->unproven || fresh_->failed) return fail(RbfStatus::InvalidArgument,"fresh RBF LU proof missing");
    fresh_->unproven=false; if (fresh_->host->info>0) { fresh_->phase=FreshState::Phase::Complete; return fail(RbfStatus::RankDeficient,"RBF augmented LU is singular"); } if (fresh_->host->info<0) { fresh_->phase=FreshState::Phase::Complete; return fail(RbfStatus::SolverError,"RBF LU invalid argument"); }
    fresh_->phase=FreshState::Phase::Complete;
    // Host ownership exchange only: old accepted factors are parked, never
    // released from this commit.
    retiredFresh_ = std::move(acceptedFresh_);
    acceptedFresh_ = std::move(fresh_);
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::BeginFreshSolve(DeviceView<const float3> posed, cudaStream_t stream,
                                          UsdGenExecutionMemoryReservation* reservation) {
    if (!acceptedFresh_ || freshSolve_ || evalPending_ || HasUnprovenWork() ||
        !posed.data || posed.size != size_t(acceptedFresh_->n))
        return fail(RbfStatus::InvalidArgument, "fresh RBF solve requires an accepted matching binding");
    int ownerDevice = -1;
    if (validateFreshPointer(posed.data, posed.size, stream, &ownerDevice) != cudaSuccess ||
        ownerDevice != acceptedFresh_->device)
        return fail(RbfStatus::InvalidArgument, "fresh RBF posed input/stream provenance invalid");
    auto candidate = std::make_unique<FreshState>();
    candidate->device = ownerDevice; candidate->n = acceptedFresh_->n; candidate->m = acceptedFresh_->m;
    candidate->factorOwner = acceptedFresh_.get();
    retiredSolve_.reset();
    if (failFreshSolveAllocation.exchange(false, std::memory_order_acq_rel))
        return fail(RbfStatus::CudaError, "forced fresh RBF solve allocation failure");
    auto permit = TryReserveCudaExecutionBytes(sizeof(FreshState::Packet), UsdGenExecutionResourceKind::Cache, reservation);
    if (!permit || cudaHostAlloc(reinterpret_cast<void**>(&candidate->host), sizeof(*candidate->host), cudaHostAllocDefault) != cudaSuccess)
        return fail(RbfStatus::CudaError, "fresh RBF solve proof packet allocation failed");
    candidate->hostPermit = std::move(*permit);
    if (!ok(candidate->current.reset(candidate->n, reservation, UsdGenExecutionResourceKind::Cache)) || !ok(candidate->coefficients.reset(size_t(candidate->m) * 3, reservation, UsdGenExecutionResourceKind::Cache)) ||
        !ok(candidate->flags.reset(1, reservation, UsdGenExecutionResourceKind::Cache)) || !ok(candidate->info.reset(1, reservation, UsdGenExecutionResourceKind::Cache)))
        return fail(RbfStatus::CudaError, "fresh RBF solve candidate allocation failed");
    freshSolve_ = std::move(candidate); auto& c = *freshSolve_; auto const& f = *acceptedFresh_;
    c.unproven = true; c.phase = FreshState::Phase::SolveInput;
    if (!ok(cudaMemcpyAsync(c.current.data(), posed.data, c.n * sizeof(float3), cudaMemcpyDeviceToDevice, stream)) ||
        !ok(cudaMemsetAsync(c.flags.data(), 0, sizeof(int), stream))) {
        c.failed = true; return fail(RbfStatus::CudaError, "fresh RBF posed input copy failed");
    }
    rhsKernel<<<(c.n + 255) / 256, 256, 0, stream>>>(f.rest.data(), c.current.data(), c.coefficients.data(),
        c.n, c.m, 1.0 / f.scale, c.flags.data());
    zeroTail<<<1, 4, 0, stream>>>(c.coefficients.data(), c.n, c.m);
    if (cudaGetLastError() != cudaSuccess ||
        !ok(cudaMemcpyAsync(&c.host->flag, c.flags.data(), sizeof(int), cudaMemcpyDeviceToHost, stream))) {
        c.failed = true; return fail(RbfStatus::CudaError, "fresh RBF posed-input proof submit failed");
    }
    if (failFreshSolveAfterInputSubmit.exchange(false, std::memory_order_acq_rel)) {
        c.failed = true; return fail(RbfStatus::CudaError, "forced fresh RBF posed-input post-submit failure");
    }
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::CommitFreshSolveInput() {
    if (!freshSolve_ || freshSolve_->phase != FreshState::Phase::SolveInput ||
        !freshSolve_->unproven || freshSolve_->failed)
        return fail(RbfStatus::InvalidArgument, "fresh RBF posed-input proof missing");
    auto& c = *freshSolve_; c.unproven = false;
    if (c.host->flag) {
        c.phase = FreshState::Phase::Complete;
        retiredSolve_ = std::move(freshSolve_);
        return fail(RbfStatus::NonFiniteInput, "RBF current samples contain non-finite values");
    }
    c.phase = FreshState::Phase::SolveInputReady;
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::BeginFreshSolveFactors(cudaStream_t stream) {
    if (!acceptedFresh_ || !freshSolve_ || freshSolve_->phase != FreshState::Phase::SolveInputReady ||
        freshSolve_->factorOwner != acceptedFresh_.get() || HasUnprovenWork())
        return fail(RbfStatus::InvalidArgument, "fresh RBF posed input not committed");
    int device = -1;
    if (validateFreshPointer(nullptr, 0, stream, &device) != cudaSuccess || device != acceptedFresh_->device)
        return fail(RbfStatus::InvalidArgument, "fresh RBF solve stream invalid");
    auto& c = *freshSolve_; auto& f = *acceptedFresh_;
    c.unproven = true; c.phase = FreshState::Phase::Solve;
    if (cusolverDnSetStream(f.solver, stream) != CUSOLVER_STATUS_SUCCESS ||
        cusolverDnDgetrs(f.solver, CUBLAS_OP_N, f.m, 3, f.matrix.data(), f.m, f.pivots.data(),
            c.coefficients.data(), f.m, c.info.data()) != CUSOLVER_STATUS_SUCCESS ||
        !ok(cudaMemcpyAsync(&c.host->info, c.info.data(), sizeof(int), cudaMemcpyDeviceToHost, stream))) {
        c.failed = true; return fail(RbfStatus::SolverError, "fresh RBF triangular-solve submit failed");
    }
    if (failFreshSolveAfterSolverSubmit.exchange(false, std::memory_order_acq_rel)) {
        c.failed = true; return fail(RbfStatus::CudaError, "forced fresh RBF solve post-submit failure");
    }
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::CommitFreshSolve() {
    if (!freshSolve_ || freshSolve_->phase != FreshState::Phase::Solve ||
        !freshSolve_->unproven || freshSolve_->failed)
        return fail(RbfStatus::InvalidArgument, "fresh RBF solve proof missing");
    auto& c = *freshSolve_; c.unproven = false;
    if (c.host->info) {
        c.phase = FreshState::Phase::Complete;
        retiredSolve_ = std::move(freshSolve_);
        return fail(RbfStatus::SolverError, "RBF solve returned an error");
    }
    // The previous coefficient storage stays in the proved solve packet.
    // Downstream fresh evaluation consumes the new coefficients, but no pose
    // becomes durably accepted until the parent resolves the transaction.
    std::swap(acceptedFresh_->coefficients, c.coefficients);
    c.phase = FreshState::Phase::Complete;
    freshSolvePendingAcceptance_ = true;
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::AcceptFreshSolve() {
    freshAcceptAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!CanAcceptFreshSolve())
        return fail(RbfStatus::InvalidArgument, "fresh RBF pose acceptance requires proven downstream completion");
    if (failFreshResolveCommit.exchange(false, std::memory_order_acq_rel))
        return fail(RbfStatus::CudaError, "forced fresh RBF resolve commit failure");
    // BeginFreshSolve retires the previous packet before any submission, so
    // this ownership handoff cannot free a live CUDA allocation.
    retiredSolve_ = std::move(freshSolve_);
    freshSolvePendingAcceptance_ = false;
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::RollbackFreshSolve() {
    freshRollbackAttempts.fetch_add(1, std::memory_order_relaxed);
    if (!CanRollbackFreshSolve())
        return fail(RbfStatus::InvalidArgument, "fresh RBF pose rollback requires proven downstream completion");
    if (failFreshResolveCommit.exchange(false, std::memory_order_acq_rel))
        return fail(RbfStatus::CudaError, "forced fresh RBF resolve commit failure");
    std::swap(acceptedFresh_->coefficients, freshSolve_->coefficients);
    retiredSolve_ = std::move(freshSolve_);
    freshSolvePendingAcceptance_ = false;
    return RbfStatus::Ok;
}
bool CudaRbfBinding::CanAcceptFreshSolve() const noexcept {
    return !failFreshResolvePreflight.exchange(false, std::memory_order_acq_rel) &&
        freshSolvePendingAcceptance_ && freshSolve_ && !freshSolve_->unproven &&
        !freshEval_ && !HasUnprovenWork();
}
bool CudaRbfBinding::CanRollbackFreshSolve() const noexcept {
    return !failFreshResolvePreflight.exchange(false, std::memory_order_acq_rel) &&
        freshSolvePendingAcceptance_ && freshSolve_ && acceptedFresh_ &&
        !freshSolve_->unproven && !freshEval_ && !HasUnprovenWork();
}
RbfStatus CudaRbfBinding::BeginFreshEvaluate(DeviceView<const float3> cvs, DeviceView<float3> output,
                                             cudaStream_t stream,
                                             UsdGenExecutionMemoryReservation* reservation) {
    if (!acceptedFresh_ || evalPending_ || freshEval_ || HasUnprovenWork() || !cvs.data || !output.data || !cvs.size || cvs.size != output.size || cvs.size > size_t(INT_MAX))
        return fail(RbfStatus::InvalidArgument,"fresh RBF evaluation requires an accepted binding and matching device views");
    int ownerDevice = -1;
    if (validateFreshPointer(cvs.data,cvs.size,stream,&ownerDevice) != cudaSuccess ||
        validateFreshPointer(output.data,output.size,stream,&ownerDevice) != cudaSuccess || ownerDevice != acceptedFresh_->device)
        return fail(RbfStatus::InvalidArgument,"fresh RBF evaluation provenance invalid");
    auto packet = std::make_unique<FreshState>();
    packet->device = ownerDevice;
    retiredEval_.reset();
    if (failFreshEvaluateAllocation.exchange(false, std::memory_order_acq_rel))
        return fail(RbfStatus::CudaError,"forced fresh RBF evaluation allocation failure");
    auto permit=TryReserveCudaExecutionBytes(sizeof(FreshState::Packet),UsdGenExecutionResourceKind::Cache,reservation);
    if (!permit || cudaHostAlloc(reinterpret_cast<void**>(&packet->host),sizeof(*packet->host),cudaHostAllocDefault)!=cudaSuccess)
        return fail(RbfStatus::CudaError,"fresh RBF evaluation packet allocation failed");
    packet->hostPermit=std::move(*permit); packet->unproven=true; packet->phase=FreshState::Phase::Evaluate;
    auto& f=*acceptedFresh_;
    freshEval_=std::move(packet); auto& e=*freshEval_;
    if (!ok(cudaMemsetAsync(f.flags.data(),0,sizeof(int),stream))) { e.failed=true; return fail(RbfStatus::CudaError,"fresh RBF evaluation status reset failed"); }
    evalKernel<<<(cvs.size+255)/256,256,0,stream>>>(cvs.data,output.data,(int)cvs.size,f.norm.data(),f.coefficients.data(),f.n,f.m,f.center[0],f.center[1],f.center[2],1./f.scale,f.scale,f.flags.data());
    if (cudaGetLastError()!=cudaSuccess || !ok(cudaMemcpyAsync(&e.host->flag,f.flags.data(),sizeof(int),cudaMemcpyDeviceToHost,stream))) { e.failed=true; return fail(RbfStatus::CudaError,"fresh RBF evaluation submit failed"); }
    if (failFreshEvaluateAfterSubmit.exchange(false, std::memory_order_acq_rel)) { e.failed=true; return fail(RbfStatus::CudaError,"forced fresh RBF evaluation post-submit failure"); }
    return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::CommitFreshEvaluate() {
    if (!freshEval_ || freshEval_->phase != FreshState::Phase::Evaluate || !freshEval_->unproven || freshEval_->failed)
        return fail(RbfStatus::InvalidArgument,"fresh RBF evaluation proof missing");
    freshEval_->unproven=false;
    if (freshEval_->host->flag) { retiredEval_ = std::move(freshEval_); return fail(RbfStatus::NonFiniteInput,"RBF evaluation CVs contain non-finite values"); }
    retiredEval_ = std::move(freshEval_); return RbfStatus::Ok;
}

CudaRbfBinding::CudaRbfBinding() = default;
CudaRbfBinding::~CudaRbfBinding() {
    // A fresh candidate is independently quarantined; never turn destruction
    // into an implicit proof wait for that submission.
    if (HasUnprovenWork()) { AbandonFresh(); return; }
    if (stateReady_) { cudaEventSynchronize(stateReady_); cudaEventDestroy(stateReady_); }
    if (evalReady_) cudaEventDestroy(evalReady_); if (solver_) cusolverDnDestroy(solver_);
}
RbfStatus CudaRbfBinding::fail(RbfStatus s, const char* why) { diagnostic_=why; return s; }

RbfStatus CudaRbfBinding::Bind(DeviceView<const float3> samples, double smoothing, cudaStream_t stream) {
    if (HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF proof is pending");
    if (evalPending_) return fail(RbfStatus::InvalidArgument,"RBF Finish is required before rebinding a pending evaluation");
    sampleCount_=order_=0; solved_=false; evalPending_=false; diagnostic_.clear();
    if (!samples.data || samples.size < 4 || samples.size > 46336u || !std::isfinite(smoothing) || smoothing < 0.0)
        return fail(RbfStatus::InvalidArgument,"RBF requires 4+ samples and finite non-negative smoothing");
    const int n=(int)samples.size, m=n+4; // m*m and all launch indices remain signed-int safe
    if (stateReady_ && !ok(cudaStreamWaitEvent(stream,stateReady_,0))) return fail(RbfStatus::CudaError,"RBF state wait failed");
    if (!stateReady_ && !ok(cudaEventCreateWithFlags(&stateReady_,cudaEventDisableTiming))) return fail(RbfStatus::CudaError,"RBF state event creation failed");
    if (!evalReady_ && !ok(cudaEventCreateWithFlags(&evalReady_,cudaEventDisableTiming))) return fail(RbfStatus::CudaError,"RBF evaluation event creation failed");
    if (!solver_ && cusolverDnCreate(&solver_) != CUSOLVER_STATUS_SUCCESS) return fail(RbfStatus::SolverError,"cuSOLVER create failed");
    if (!ok(rest_.reset(n)) || !ok(normSamples_.reset(3*size_t(n))) || !ok(matrix_.reset(size_t(m)*m)) || !ok(coefficients_.reset(size_t(m)*3)) || !ok(pivots_.reset(m)) || !ok(info_.reset(1)) || !ok(flags_.reset(1)) || !ok(evalFlags_.reset(1)) || !ok(work_.reset(size_t(m)*m)) || !ok(gram_.reset(16))) return fail(RbfStatus::CudaError,"RBF device allocation failed");
    if (!ok(cudaMemcpyAsync(rest_.data(),samples.data,n*sizeof(float3),cudaMemcpyDeviceToDevice,stream))) return fail(RbfStatus::CudaError,"RBF rest copy failed");
    DeviceBuffer<float> extents; if(!ok(extents.reset(6))) return fail(RbfStatus::CudaError,"RBF extent allocation failed");
    initExtent<<<1,1,0,stream>>>(extents.data(),flags_.data()); extentKernel<<<32,128,0,stream>>>(rest_.data(),n,extents.data(),flags_.data());
    float e[6]; int flag=0;
    if(!ok(cudaMemcpyAsync(e,extents.data(),sizeof(e),cudaMemcpyDeviceToHost,stream)) || !ok(cudaMemcpyAsync(&flag,flags_.data(),sizeof(flag),cudaMemcpyDeviceToHost,stream)) || !ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF extent query failed");
    if(flag) return fail(RbfStatus::NonFiniteInput,"RBF rest samples contain non-finite values");
    center_[0]=(double)e[0] + ((double)e[3]-(double)e[0])*.5; center_[1]=(double)e[1] + ((double)e[4]-(double)e[1])*.5; center_[2]=(double)e[2] + ((double)e[5]-(double)e[2])*.5; scale_=std::max((double)e[3]-e[0],std::max((double)e[4]-e[1],(double)e[5]-e[2]));
    if(!std::isfinite(scale_) || scale_ <= 0.0) return fail(RbfStatus::RankDeficient,"RBF rest samples have zero extent");
    normalizeSamples<<<(n+255)/256,256,0,stream>>>(rest_.data(),normSamples_.data(),n,center_[0],center_[1],center_[2],1.0/scale_);
    if(cudaGetLastError()!=cudaSuccess) return fail(RbfStatus::CudaError,"RBF sample normalization failed");
    if(!ok(cudaMemsetAsync(gram_.data(),0,16*sizeof(double),stream))) return fail(RbfStatus::CudaError,"RBF rank diagnostic reset failed");
    polynomialGram<<<32,128,0,stream>>>(rest_.data(),n,gram_.data(),center_[0],center_[1],center_[2],1.0/scale_);
    double gram[16]; if(!ok(cudaMemcpyAsync(gram,gram_.data(),sizeof(gram),cudaMemcpyDeviceToHost,stream))||!ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF rank diagnostic query failed");
    if(!fullAffineRank(gram)) return fail(RbfStatus::RankDeficient,"RBF samples lack numerically full affine 3D support");
    buildMatrix<<<(m*m+255)/256,256,0,stream>>>(rest_.data(),matrix_.data(),n,m,center_[0],center_[1],center_[2],1.0/scale_,smoothing,flags_.data());
    int lwork=0; if(cusolverDnDgetrf_bufferSize(solver_,m,m,matrix_.data(),m,&lwork)!=CUSOLVER_STATUS_SUCCESS || !ok(work_.reset(lwork))) return fail(RbfStatus::SolverError,"cuSOLVER LU workspace failed");
    if(cusolverDnSetStream(solver_,stream)!=CUSOLVER_STATUS_SUCCESS || cusolverDnDgetrf(solver_,m,m,matrix_.data(),m,work_.data(),pivots_.data(),info_.data())!=CUSOLVER_STATUS_SUCCESS) return fail(RbfStatus::SolverError,"cuSOLVER LU failed");
    int info=0; if(!ok(cudaMemcpyAsync(&info,info_.data(),sizeof(info),cudaMemcpyDeviceToHost,stream))||!ok(cudaStreamSynchronize(stream)))return fail(RbfStatus::CudaError,"RBF LU status query failed");
    if(info>0) return fail(RbfStatus::RankDeficient,"RBF augmented LU is singular (including coplanar affine support)"); if(info<0)return fail(RbfStatus::SolverError,"RBF LU invalid argument");
    // A newly bound field has the mathematically defined rest (identity) state.
    if(!ok(cudaMemsetAsync(coefficients_.data(),0,coefficients_.size()*sizeof(double),stream)) || !ok(cudaEventRecord(stateReady_,stream))) return fail(RbfStatus::CudaError,"RBF identity-state initialization failed");
    sampleCount_=n; order_=m; smoothing_=smoothing; solved_=true; return RbfStatus::Ok;
}

RbfStatus CudaRbfBinding::Solve(DeviceView<const float3> posed, cudaStream_t stream) {
    if (HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF proof is pending");
    solved_=false;
    if (evalPending_) return fail(RbfStatus::InvalidArgument,"RBF Finish is required before changing a pending evaluation state");
    if(!sampleCount_ || posed.size!=sampleCount_ || !posed.data) return fail(RbfStatus::InvalidArgument,"RBF Solve samples do not match binding");
    if(!ok(cudaStreamWaitEvent(stream,stateReady_,0))) return fail(RbfStatus::CudaError,"RBF state wait failed");
    if(!ok(current_.reset(sampleCount_)) || !ok(cudaMemcpyAsync(current_.data(),posed.data,sampleCount_*sizeof(float3),cudaMemcpyDeviceToDevice,stream))) return fail(RbfStatus::CudaError,"RBF current sample copy failed");
    if(!ok(cudaMemsetAsync(flags_.data(),0,sizeof(int),stream))) return fail(RbfStatus::CudaError,"RBF solve flag reset failed"); rhsKernel<<<(sampleCount_+255)/256,256,0,stream>>>(rest_.data(),current_.data(),coefficients_.data(),(int)sampleCount_,(int)order_,1.0/scale_,flags_.data()); zeroTail<<<1,4,0,stream>>>(coefficients_.data(),(int)sampleCount_,(int)order_);
    int flag=0; if(!ok(cudaMemcpyAsync(&flag,flags_.data(),sizeof(flag),cudaMemcpyDeviceToHost,stream))||!ok(cudaStreamSynchronize(stream)))return fail(RbfStatus::CudaError,"RBF input validation failed"); if(flag)return fail(RbfStatus::NonFiniteInput,"RBF current samples contain non-finite values");
    if(cusolverDnSetStream(solver_,stream)!=CUSOLVER_STATUS_SUCCESS || cusolverDnDgetrs(solver_,CUBLAS_OP_N,(int)order_,3,matrix_.data(),(int)order_,pivots_.data(),coefficients_.data(),(int)order_,info_.data())!=CUSOLVER_STATUS_SUCCESS)return fail(RbfStatus::SolverError,"cuSOLVER triangular solve failed");
    int info=0; if(!ok(cudaMemcpyAsync(&info,info_.data(),sizeof(info),cudaMemcpyDeviceToHost,stream))||!ok(cudaStreamSynchronize(stream)))return fail(RbfStatus::CudaError,"RBF solve status query failed"); if(info)return fail(RbfStatus::SolverError,"RBF solve returned an error"); if(!ok(cudaEventRecord(stateReady_,stream)))return fail(RbfStatus::CudaError,"RBF solve event failed"); solved_=true; return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::Evaluate(DeviceView<const float3> cvs, DeviceView<float3> out, cudaStream_t stream) {
    if (HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF proof is pending");
    if(!sampleCount_ || !solved_ || !cvs.data || !out.data || !cvs.size || cvs.size!=out.size || cvs.size>size_t(INT_MAX)) return fail(RbfStatus::InvalidArgument,"RBF Evaluate requires a solved binding and equal non-empty bounded device views");
    if(!ok(cudaStreamWaitEvent(stream,stateReady_,0)) || (!evalPending_ && !ok(cudaMemsetAsync(evalFlags_.data(),0,sizeof(int),stream)))) return fail(RbfStatus::CudaError,"RBF evaluation state setup failed"); evalKernel<<<(cvs.size+255)/256,256,0,stream>>>(cvs.data,out.data,(int)cvs.size,normSamples_.data(),coefficients_.data(),(int)sampleCount_,(int)order_,center_[0],center_[1],center_[2],1.0/scale_,scale_,evalFlags_.data());
    if(cudaGetLastError()!=cudaSuccess || !ok(cudaEventRecord(stateReady_,stream)) || !ok(cudaEventRecord(evalReady_,stream))) return fail(RbfStatus::CudaError,"RBF evaluation launch failed"); evalPending_=true; return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::Finish(cudaStream_t stream) {
    if (HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF proof is pending");
    if(!evalPending_) return RbfStatus::Ok;
    int bad=0; if(!ok(cudaStreamWaitEvent(stream,evalReady_,0)) || !ok(cudaMemcpyAsync(&bad,evalFlags_.data(),sizeof(bad),cudaMemcpyDeviceToHost,stream)) || !ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF evaluation completion query failed");
    evalPending_=false; if(bad) { solved_=false; return fail(RbfStatus::NonFiniteInput,"RBF evaluation CVs contain non-finite values; generation rejected"); } return RbfStatus::Ok;
}
}}
