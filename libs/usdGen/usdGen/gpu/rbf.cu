#include "rbf.h"
#include "cudaCompat.h"
#include "deviceResources.h"

#include <cmath>
#include <climits>
#include <cstddef>
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
std::atomic<bool> disableEvalCache{false};
std::atomic<uint64_t> evalCacheHits{0}, evalCacheMisses{0};
__device__ inline void mark(int* f, int v) { atomicOr(f, v); }
// Each thread folds its grid stride into registers and each warp reduces
// with shuffles; the 4 warp folds then combine through shared memory
// and lane 0 lands the 7 words with plain stores (was: 24 CAS atomics
// from the warp leaders, which cost 14us once the extent moved to mapped
// host memory). Min/max over finite floats is exact and order-free, so
// the shared fold writes bitwise the same extent for any combination
// order; mixed-sign zeros can only name a zero extent, which fails
// identically either way. The non-finite flag reduces the same way (a
// warp OR, then a 4-way lane-0 OR): 1 iff any element was non-finite,
// exactly what the old atomicOr wrote. Threads beyond n fold
// neutrally, and the shuffle mask is the converged warp so partial
// blocks stay correct. Single-block launches only: every caller below
// uses <<<1,128>>>, so warps 0-3 are exactly the block.
__global__ void extentKernel(const float3* p, int n, float* e, int* flags) {
    __shared__ float red[4][6];
    __shared__ int bad[4];
    int const tid = threadIdx.x, warp = tid >> 5, lane = tid & 31;
    float mnx = INFINITY, mny = INFINITY, mnz = INFINITY;
    float mxx = -INFINITY, mxy = -INFINITY, mxz = -INFINITY;
    int localBad = 0;
    for (int i = blockIdx.x * blockDim.x + threadIdx.x; i < n; i += blockDim.x * gridDim.x) {
        float3 v = p[i];
        if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) { localBad = 1; continue; }
        mnx = fminf(mnx, v.x); mny = fminf(mny, v.y); mnz = fminf(mnz, v.z);
        mxx = fmaxf(mxx, v.x); mxy = fmaxf(mxy, v.y); mxz = fmaxf(mxz, v.z);
    }
    unsigned const mask = __activemask();
    for (int d = 16; d > 0; d >>= 1) {
        mnx = fminf(mnx, __shfl_down_sync(mask, mnx, d));
        mny = fminf(mny, __shfl_down_sync(mask, mny, d));
        mnz = fminf(mnz, __shfl_down_sync(mask, mnz, d));
        mxx = fmaxf(mxx, __shfl_down_sync(mask, mxx, d));
        mxy = fmaxf(mxy, __shfl_down_sync(mask, mxy, d));
        mxz = fmaxf(mxz, __shfl_down_sync(mask, mxz, d));
        localBad |= __shfl_down_sync(mask, localBad, d);
    }
    if (lane == 0) {
        red[warp][0] = mnx; red[warp][1] = mny; red[warp][2] = mnz;
        red[warp][3] = mxx; red[warp][4] = mxy; red[warp][5] = mxz;
        bad[warp] = localBad;
    }
    __syncthreads();
    if (tid == 0) {
        float ax = INFINITY, ay = INFINITY, az = INFINITY;
        float ix = -INFINITY, iy = -INFINITY, iz = -INFINITY;
        int f = 0;
        for (int w = 0; w < 4; ++w) {
            ax = fminf(ax, red[w][0]); ay = fminf(ay, red[w][1]); az = fminf(az, red[w][2]);
            ix = fmaxf(ix, red[w][3]); iy = fmaxf(iy, red[w][4]); iz = fmaxf(iz, red[w][5]);
            f |= bad[w];
        }
        e[0] = ax; e[1] = ay; e[2] = az; e[3] = ix; e[4] = iy; e[5] = iz;
        *flags = f;
    }
}
__global__ void polynomialGram(const float3* p, int n, double* gram, double cx, double cy, double cz, double invScale) {
    for (int i=blockIdx.x*blockDim.x+threadIdx.x;i<n;i+=blockDim.x*gridDim.x) {
        float3 q=p[i]; double v[4]={1.,(q.x-cx)*invScale,(q.y-cy)*invScale,(q.z-cz)*invScale};
        for(int r=0;r<4;++r) for(int c=0;c<4;++c) atomicAdd(&gram[r*4+c],v[r]*v[c]);
    }
}
// Lands the 16-word gram into mapped proof memory. The polynomialGram
// atomics stay on device memory (host atomics would serialize over the
// interconnect); this single-block copy replaces the gram D2H node and
// its ~7us drain bubble, with bitwise the same gram bytes.
__global__ void landGram(const double* gram, double* mapped) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < 16) mapped[i] = gram[i];
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
// The sample normalization rides in the matrix build (threads k<n write
// the normalized samples the old normalizeSamples launch wrote): the two
// launches wrote disjoint addresses, so one launch writes bitwise the
// same matrix and samples and saves a launch + a grid teardown on every
// bind. m*m > n always, so threads 0..n-1 exist in the build grid.
__global__ void buildMatrix(const float3* p, double* a, int n, int m, double cx, double cy, double cz, double invScale, double lambda, int* flags, double* sn) {
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
    if (k < n) { float3 s=p[k]; sn[k]=(s.x-cx)*invScale; sn[n+k]=(s.y-cy)*invScale; sn[2*n+k]=(s.z-cz)*invScale; }
}
// The polynomial-tail zeroing rides in the RHS kernel (threads 0-3 write
// the twelve tail slots before the bounds check): the two launches wrote
// disjoint addresses, so one launch writes bitwise the same RHS and saves
// a launch + a grid teardown on every solve. n >= 4 always, so threads
// 0-3 exist in the RHS grid.
__global__ void rhsKernel(const float3* rest, const float3* current, double* rhs, int n, int m, double invScale, int* flags) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i<4) rhs[n+i]=rhs[m+n+i]=rhs[2*m+n+i]=0.0;
    if(i>=n) return;
    float3 a=rest[i], b=current[i];
    if(!isfinite(b.x)||!isfinite(b.y)||!isfinite(b.z)) { mark(flags,1); return; }
    rhs[i]=((double)b.x-a.x)*invScale; rhs[i+m]=((double)b.y-a.y)*invScale; rhs[i+2*m]=((double)b.z-a.z)*invScale;
}
// 128-wide blocks run the fp64 eval loop ~70us faster per 1M CVs than
// 256-wide (11.98 -> 11.91ms; 64 is no faster). One thread per CV either
// way, so the grouping is bitwise-transparent.
constexpr int kEvalBlock = 128;
// Total device bytes (R words plus the two proof copies) below which the
// direct Evaluate caches the pose-invariant radii across calls. 1M CVs at
// n=100 need 812MB, so the production and bench shapes engage; anything
// larger keeps today's direct kernel with no added work.
constexpr size_t kEvalCacheMaxBytes = size_t(1) << 30;
// Bitwise device memcmp: sets *flag iff any word differs. Plain stores
// race benignly (every writer stores 1); the caller zeroes first and
// reads after the stream syncs.
__global__ void verifyKernel(const int* a, const int* b, size_t words, int* flag) {
    size_t const stride = size_t(blockDim.x) * gridDim.x;
    for (size_t i = size_t(blockIdx.x) * size_t(blockDim.x) + threadIdx.x; i < words; i += stride)
        if (a[i] != b[i]) *flag = 1;
}
// Fills the R cache: R[j*count+i] is the radius-cubed kernel value for CV
// i and sample j. The r expression is evalKernel's text verbatim (same
// operations in the same order), so every cached word is bitwise what the
// eval loop computed; only the grid is transposed (one thread per (i,j)
// with consecutive threads on consecutive CVs) so the 8-byte writes
// coalesce. Deliberately no finiteness check: a non-finite CV writes a
// NaN word the cached evaluator never reads (it returns before the loop,
// exactly like evalKernel), and the evaluator still raises the flag.
__global__ void rFillKernel(const float3* cvs, const double* sn, double* r, size_t count, int n, double cx, double cy, double cz, double invScale) {
    size_t i = size_t(blockIdx.x) * size_t(blockDim.x) + threadIdx.x;
    int j = blockIdx.y;
    if (i >= count || j >= n) return;
    float3 p = cvs[i];
    double x = (p.x - cx) * invScale, y = (p.y - cy) * invScale, z = (p.z - cz) * invScale;
    double dx = x - sn[j], dy = y - sn[n + j], dz = z - sn[2 * n + j];
    double rr = sqrt(dx * dx + dy * dy + dz * dz);
    rr *= rr * rr;
    r[size_t(j) * count + i] = rr;
}
// Cached-R evaluation: evalKernel with the radius-cubed loop carried by
// the cache instead of recomputed. Every other expression (normalize,
// polynomial, accumulate, output, flag) is evalKernel's text verbatim,
// so a verified cache evaluates bitwise what the direct kernel did.
__global__ void evalCachedKernel(const float3* cvs, float3* out, int c, const double* r, const double* coef, int n, int m, double cx, double cy, double cz, double invScale, double scale, int* flags) {
    int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=c) return; float3 p=cvs[i];
    if(!isfinite(p.x)||!isfinite(p.y)||!isfinite(p.z)){mark(flags,1);return;}
    double x=(p.x-cx)*invScale,y=(p.y-cy)*invScale,z=(p.z-cz)*invScale;
    double ox=x+coef[n]+coef[n+1]*x+coef[n+2]*y+coef[n+3]*z;
    double oy=y+coef[m+n]+coef[m+n+1]*x+coef[m+n+2]*y+coef[m+n+3]*z;
    double oz=z+coef[2*m+n]+coef[2*m+n+1]*x+coef[2*m+n+2]*y+coef[2*m+n+3]*z;
#pragma unroll 1
    for(int j=0;j<n;++j){ double rr=r[size_t(j)*size_t(c)+size_t(i)]; ox+=coef[j]*rr;oy+=coef[m+j]*rr;oz+=coef[2*m+j]*rr; }
    out[i]=make_float3((float)(ox*scale+cx),(float)(oy*scale+cy),(float)(oz*scale+cz));
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
void TestDisableCudaRbfEvalCache(bool disable) noexcept { disableEvalCache.store(disable, std::memory_order_release); }
uint64_t CudaRbfEvalCacheHitsForTesting() noexcept { return evalCacheHits.load(std::memory_order_acquire); }
uint64_t CudaRbfEvalCacheMissesForTesting() noexcept { return evalCacheMisses.load(std::memory_order_acquire); }

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

struct CudaRbfBinding::DirectProofs {
    float extent[6];
    int flag;
    double gram[16];
    int info;
};

struct CudaRbfBinding::FreshState {
    enum class Phase { Extent, ExtentReady, Rank, RankReady, Lu, SolveInput,
                       SolveInputReady, Solve, Evaluate, Complete };
    DeviceBuffer<float3> rest, current;
    DeviceBuffer<double> matrix, work, coefficients, gram, norm;
    DeviceBuffer<int> pivots, info, flags;
    DeviceBuffer<float> extents;
    cusolverDnHandle_t solver = nullptr;
    // Extent and flag stay adjacent so one proof copy returns both.
    struct Packet { float extent[6]; int flag = 0; double gram[16]; int info = 0; } *host = nullptr;
    static_assert(offsetof(Packet, flag) == sizeof(Packet::extent), "extent proof must be one contiguous copy");
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
        !ok(fresh->extents.reset(7, reservation, UsdGenExecutionResourceKind::Cache)) || cusolverDnCreate(&fresh->solver) != CUSOLVER_STATUS_SUCCESS ||
        cusolverDnDgetrf_bufferSize(fresh->solver, fresh->m, fresh->m, fresh->matrix.data(), fresh->m, &fresh->lwork) != CUSOLVER_STATUS_SUCCESS ||
        !ok(fresh->work.reset(fresh->lwork, reservation, UsdGenExecutionResourceKind::Cache))) return fail(RbfStatus::CudaError, "fresh RBF candidate allocation failed");
    fresh_ = std::move(fresh); auto& f = *fresh_;
    f.unproven = true; f.phase = FreshState::Phase::Extent;
    if (!ok(cudaMemcpyAsync(f.rest.data(), samples.data, f.n*sizeof(float3), cudaMemcpyDeviceToDevice, stream))) { f.failed=true; return fail(RbfStatus::CudaError,"fresh RBF rest copy failed"); }
    float* ex = f.extents.data(); int* exFlag = reinterpret_cast<int*>(ex + 6);
    extentKernel<<<1,128,0,stream>>>(f.rest.data(),f.n,ex,exFlag);
    if (cudaGetLastError()!=cudaSuccess || !ok(cudaMemcpyAsync(f.host->extent,f.extents.data(),sizeof(f.host->extent)+sizeof(f.host->flag),cudaMemcpyDeviceToHost,stream))) { f.failed=true; return fail(RbfStatus::CudaError,"fresh RBF extent submit failed"); }
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
    buildMatrix<<<(f.m*f.m+255)/256,256,0,stream>>>(f.rest.data(),f.matrix.data(),f.n,f.m,f.center[0],f.center[1],f.center[2],1./f.scale,f.smoothing,f.flags.data(),f.norm.data());
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
    // The retired solve packet's storage is private to this binding and is
    // fully overwritten below (posed D2D, rhsKernel, getrs, both proof
    // D2Hs), so the new candidate adopts it instead of freeing and
    // re-allocating four device buffers plus a host packet every pose.
    // reset() still re-sizes (or re-creates abandoned buffers), so a
    // rebind, a device move, or a quarantine behaves exactly as before,
    // and the forced-allocation seam still fires first, as before.
    auto candidate = std::move(retiredSolve_);
    if (!candidate) candidate = std::make_unique<FreshState>();
    candidate->device = ownerDevice; candidate->n = acceptedFresh_->n; candidate->m = acceptedFresh_->m;
    candidate->factorOwner = acceptedFresh_.get();
    candidate->failed = false;
    if (failFreshSolveAllocation.exchange(false, std::memory_order_acq_rel))
        return fail(RbfStatus::CudaError, "forced fresh RBF solve allocation failure");
    if (!candidate->host) {
        auto permit = TryReserveCudaExecutionBytes(sizeof(FreshState::Packet), UsdGenExecutionResourceKind::Cache, reservation);
        if (!permit || cudaHostAlloc(reinterpret_cast<void**>(&candidate->host), sizeof(*candidate->host), cudaHostAllocDefault) != cudaSuccess)
            return fail(RbfStatus::CudaError, "fresh RBF solve proof packet allocation failed");
        candidate->hostPermit = std::move(*permit);
    }
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
    // Same adoption as the solve packet (see BeginFreshSolve): the eval
    // packet holds only a host proof packet, fully overwritten by the flag
    // D2H below, so it is reused across evaluations instead of freed and
    // re-allocated every time.
    auto packet = std::move(retiredEval_);
    if (!packet) packet = std::make_unique<FreshState>();
    packet->device = ownerDevice;
    packet->failed = false;
    if (failFreshEvaluateAllocation.exchange(false, std::memory_order_acq_rel))
        return fail(RbfStatus::CudaError,"forced fresh RBF evaluation allocation failure");
    if (!packet->host) {
        auto permit=TryReserveCudaExecutionBytes(sizeof(FreshState::Packet),UsdGenExecutionResourceKind::Cache,reservation);
        if (!permit || cudaHostAlloc(reinterpret_cast<void**>(&packet->host),sizeof(*packet->host),cudaHostAllocDefault)!=cudaSuccess)
            return fail(RbfStatus::CudaError,"fresh RBF evaluation packet allocation failed");
        packet->hostPermit=std::move(*permit);
    }
    packet->unproven=true; packet->phase=FreshState::Phase::Evaluate;
    auto& f=*acceptedFresh_;
    freshEval_=std::move(packet); auto& e=*freshEval_;
    if (!ok(cudaMemsetAsync(f.flags.data(),0,sizeof(int),stream))) { e.failed=true; return fail(RbfStatus::CudaError,"fresh RBF evaluation status reset failed"); }
    evalKernel<<<(cvs.size+kEvalBlock-1)/kEvalBlock,kEvalBlock,0,stream>>>(cvs.data,output.data,(int)cvs.size,f.norm.data(),f.coefficients.data(),f.n,f.m,f.center[0],f.center[1],f.center[2],1./f.scale,f.scale,f.flags.data());
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
    // The direct-path proofs are never referenced by fresh candidates, so
    // they free on every destruction path, including a fresh abandon.
    if (proofHost_) { cudaFreeHost(proofHost_); proofHost_ = nullptr; }
    if (HasUnprovenWork()) { AbandonFresh(); return; }
    if (stateReady_) { cudaEventSynchronize(stateReady_); cudaEventDestroy(stateReady_); }
    if (evalReady_) cudaEventDestroy(evalReady_); if (solver_) cusolverDnDestroy(solver_);
}
RbfStatus CudaRbfBinding::fail(RbfStatus s, const char* why) { diagnostic_=why; return s; }
bool CudaRbfBinding::ensureProofs() {
    int device = -1;
    if (!ok(cudaGetDevice(&device))) return false;
    if (!proofHost_) {
        void* p = nullptr;
        if (!ok(cudaHostAlloc(&p, sizeof(DirectProofs),
                              cudaHostAllocMapped | cudaHostAllocPortable)))
            return false;
        proofHost_ = static_cast<DirectProofs*>(p);
        proofDevice_ = -1;
    }
    if (device != proofDevice_) {
        void* d = nullptr;
        if (!ok(cudaHostGetDevicePointer(&d, proofHost_, 0))) return false;
        proofDev_ = static_cast<DirectProofs*>(d);
        proofDevice_ = device;
    }
    return true;
}

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
    if (!ok(rest_.reset(n)) || !ok(normSamples_.reset(3*size_t(n))) || !ok(matrix_.reset(size_t(m)*m)) || !ok(coefficients_.reset(size_t(m)*3)) || !ok(pivots_.reset(m)) || !ok(evalFlags_.reset(1)) || !ok(gram_.reset(16))) return fail(RbfStatus::CudaError,"RBF device allocation failed");
    if (!ok(cudaMemcpyAsync(rest_.data(),samples.data,n*sizeof(float3),cudaMemcpyDeviceToDevice,stream))) return fail(RbfStatus::CudaError,"RBF rest copy failed");
    // Zero-copy proofs (see Solve): the extent, gram, and info words live
    // in mapped host memory, read after the stream syncs, so no D2H node
    // (each carried a ~7us drain bubble) separates the phases. work_ still
    // sizes once below, after the bufferSize query; nothing reads it before
    // that. buildMatrix ignores its flags argument (hence nullptr).
    if(!ensureProofs()) return fail(RbfStatus::CudaError,"RBF proof allocation failed");
    extentKernel<<<1,128,0,stream>>>(rest_.data(),n,proofDev_->extent,&proofDev_->flag);
    if(!ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF extent query failed");
    if(proofHost_->flag) return fail(RbfStatus::NonFiniteInput,"RBF rest samples contain non-finite values");
    center_[0]=(double)proofHost_->extent[0] + ((double)proofHost_->extent[3]-(double)proofHost_->extent[0])*.5; center_[1]=(double)proofHost_->extent[1] + ((double)proofHost_->extent[4]-(double)proofHost_->extent[1])*.5; center_[2]=(double)proofHost_->extent[2] + ((double)proofHost_->extent[5]-(double)proofHost_->extent[2])*.5; scale_=std::max((double)proofHost_->extent[3]-proofHost_->extent[0],std::max((double)proofHost_->extent[4]-proofHost_->extent[1],(double)proofHost_->extent[5]-proofHost_->extent[2]));
    if(!std::isfinite(scale_) || scale_ <= 0.0) return fail(RbfStatus::RankDeficient,"RBF rest samples have zero extent");
    if(cudaGetLastError()!=cudaSuccess) return fail(RbfStatus::CudaError,"RBF extent launch failed");
    if(!ok(cudaMemsetAsync(gram_.data(),0,16*sizeof(double),stream))) return fail(RbfStatus::CudaError,"RBF rank diagnostic reset failed");
    polynomialGram<<<32,128,0,stream>>>(rest_.data(),n,gram_.data(),center_[0],center_[1],center_[2],1.0/scale_);
    landGram<<<1,32,0,stream>>>(gram_.data(),proofDev_->gram);
    // The rank proof and the LU share one synchronization: buildMatrix and
    // getrf submit before the gram is known, and the rank decision is
    // checked first after the single sync, so error precedence (rank before
    // LU status) and every success-path byte are unchanged.
    buildMatrix<<<(m*m+255)/256,256,0,stream>>>(rest_.data(),matrix_.data(),n,m,center_[0],center_[1],center_[2],1.0/scale_,smoothing,nullptr,normSamples_.data());
    int lwork=0; if(cusolverDnDgetrf_bufferSize(solver_,m,m,matrix_.data(),m,&lwork)!=CUSOLVER_STATUS_SUCCESS || !ok(work_.reset(lwork))) return fail(RbfStatus::SolverError,"cuSOLVER LU workspace failed");
    if(cusolverDnSetStream(solver_,stream)!=CUSOLVER_STATUS_SUCCESS || cusolverDnDgetrf(solver_,m,m,matrix_.data(),m,work_.data(),pivots_.data(),&proofDev_->info)!=CUSOLVER_STATUS_SUCCESS) return fail(RbfStatus::SolverError,"cuSOLVER LU failed");
    if(!ok(cudaStreamSynchronize(stream)))return fail(RbfStatus::CudaError,"RBF LU status query failed");
    double gram[16]; for(int i=0;i<16;++i) gram[i]=proofHost_->gram[i]; // fullAffineRank eliminates in place
    if(!fullAffineRank(gram)) return fail(RbfStatus::RankDeficient,"RBF samples lack numerically full affine 3D support");
    if(proofHost_->info>0) return fail(RbfStatus::RankDeficient,"RBF augmented LU is singular (including coplanar affine support)"); if(proofHost_->info<0)return fail(RbfStatus::SolverError,"RBF LU invalid argument");
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
    // Zero-copy proofs: the flag and info words live in mapped host memory,
    // so neither proof needs a D2H node (each carried a ~7us drain bubble).
    // The host zeroes the flag directly (was: a device memset node) and reads
    // both words after the single sync below.
    if(!ensureProofs()) return fail(RbfStatus::CudaError,"RBF proof allocation failed");
    proofHost_->flag = 0;
    rhsKernel<<<(sampleCount_+255)/256,256,0,stream>>>(rest_.data(),current_.data(),coefficients_.data(),(int)sampleCount_,(int)order_,1.0/scale_,&proofDev_->flag);
    // The triangular solve submits before the flag is known, so one sync
    // proves both words: the mapped proofs removed the D2H nodes that used
    // to pin a drain bubble here. The flag is checked first after the sync,
    // so error precedence (NonFinite before SolverError) and every
    // success-path byte are unchanged; cuSOLVER completes normally on
    // non-finite RHS, so the error path just wastes one submit.
    if(cusolverDnSetStream(solver_,stream)!=CUSOLVER_STATUS_SUCCESS || cusolverDnDgetrs(solver_,CUBLAS_OP_N,(int)order_,3,matrix_.data(),(int)order_,pivots_.data(),coefficients_.data(),(int)order_,&proofDev_->info)!=CUSOLVER_STATUS_SUCCESS)return fail(RbfStatus::SolverError,"cuSOLVER triangular solve failed");
    if(!ok(cudaStreamSynchronize(stream)))return fail(RbfStatus::CudaError,"RBF solve status query failed"); if(proofHost_->flag)return fail(RbfStatus::NonFiniteInput,"RBF current samples contain non-finite values"); if(proofHost_->info)return fail(RbfStatus::SolverError,"RBF solve returned an error"); if(!ok(cudaEventRecord(stateReady_,stream)))return fail(RbfStatus::CudaError,"RBF solve event failed"); solved_=true; return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::Evaluate(DeviceView<const float3> cvs, DeviceView<float3> out, cudaStream_t stream) {
    if (HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF proof is pending");
    if(!sampleCount_ || !solved_ || !cvs.data || !out.data || !cvs.size || cvs.size!=out.size || cvs.size>size_t(INT_MAX)) return fail(RbfStatus::InvalidArgument,"RBF Evaluate requires a solved binding and equal non-empty bounded device views");
    if(!ok(cudaStreamWaitEvent(stream,stateReady_,0)) || (!evalPending_ && !ok(cudaMemsetAsync(evalFlags_.data(),0,sizeof(int),stream)))) return fail(RbfStatus::CudaError,"RBF evaluation state setup failed");
    // Pose-invariant radii: R depends only on the CVs, the rest samples,
    // and the rest-derived center/scale, all verified bitwise below, so a
    // hit evaluates through the cache and a miss refills it first. The
    // checks above bound cvs.size to INT_MAX and Bind bounds n to 46336,
    // so the products below cannot overflow 64 bits. Over the cap (or
    // with the test seam set) the direct kernel runs exactly as before.
    int const n = (int)sampleCount_;
    size_t const count = cvs.size;
    size_t const rBytes = count * size_t(n) * sizeof(double);
    size_t const copyBytes = count * sizeof(float3) + size_t(n) * sizeof(float3);
    bool const cacheable = !disableEvalCache.load(std::memory_order_relaxed) &&
        rBytes > 0 && rBytes + copyBytes <= kEvalCacheMaxBytes;
    bool cached = false;
    if (cacheable) {
        bool const shapeOk = rValid_ && rN_ == n && rCount_ == count;
        bool hit = false;
        if (shapeOk) {
            if(!ensureProofs()) return fail(RbfStatus::CudaError,"RBF proof allocation failed");
            proofHost_->flag = 0;
            verifyKernel<<<(3 * count + 255) / 256, 256, 0, stream>>>(
                reinterpret_cast<int const*>(cvs.data), reinterpret_cast<int const*>(rCvs_.data()),
                3 * count, &proofDev_->flag);
            verifyKernel<<<(size_t(3) * size_t(n) + 255) / 256, 256, 0, stream>>>(
                reinterpret_cast<int const*>(rest_.data()), reinterpret_cast<int const*>(rRest_.data()),
                size_t(3) * size_t(n), &proofDev_->flag);
            if(cudaGetLastError()!=cudaSuccess || !ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF evaluation cache verification failed");
            hit = (proofHost_->flag == 0);
        }
        if (!shapeOk) {
            if(!ok(rCache_.reset(count * size_t(n))) || !ok(rCvs_.reset(count)) || !ok(rRest_.reset(size_t(n)))) return fail(RbfStatus::CudaError,"RBF evaluation cache allocation failed");
            rN_ = n;
            rCount_ = count;
        }
        if (!shapeOk || !hit) {
            rValid_ = false;
            if(!ok(cudaMemcpyAsync(rCvs_.data(),cvs.data,count * sizeof(float3),cudaMemcpyDeviceToDevice,stream)) || !ok(cudaMemcpyAsync(rRest_.data(),rest_.data(),size_t(n) * sizeof(float3),cudaMemcpyDeviceToDevice,stream))) return fail(RbfStatus::CudaError,"RBF evaluation cache proof copy failed");
            dim3 const fillGrid(static_cast<unsigned int>((count + 255) / 256), static_cast<unsigned int>(n));
            rFillKernel<<<fillGrid, 256, 0, stream>>>(cvs.data,normSamples_.data(),rCache_.data(),count,n,center_[0],center_[1],center_[2],1.0/scale_);
            if(cudaGetLastError()!=cudaSuccess) return fail(RbfStatus::CudaError,"RBF evaluation cache fill failed");
            rValid_ = true;
            evalCacheMisses.fetch_add(1, std::memory_order_relaxed);
        } else {
            evalCacheHits.fetch_add(1, std::memory_order_relaxed);
        }
        cached = true;
    }
    if (cached)
        evalCachedKernel<<<(count+kEvalBlock-1)/kEvalBlock,kEvalBlock,0,stream>>>(cvs.data,out.data,(int)count,rCache_.data(),coefficients_.data(),n,(int)order_,center_[0],center_[1],center_[2],1.0/scale_,scale_,evalFlags_.data());
    else
        evalKernel<<<(count+kEvalBlock-1)/kEvalBlock,kEvalBlock,0,stream>>>(cvs.data,out.data,(int)count,normSamples_.data(),coefficients_.data(),n,(int)order_,center_[0],center_[1],center_[2],1.0/scale_,scale_,evalFlags_.data());
    if(cudaGetLastError()!=cudaSuccess || !ok(cudaEventRecord(stateReady_,stream)) || !ok(cudaEventRecord(evalReady_,stream))) return fail(RbfStatus::CudaError,"RBF evaluation launch failed"); evalPending_=true; return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::Finish(cudaStream_t stream) {
    if (HasUnprovenWork()) return fail(RbfStatus::InvalidArgument,"fresh RBF proof is pending");
    if(!evalPending_) return RbfStatus::Ok;
    int bad=0; if(!ok(cudaStreamWaitEvent(stream,evalReady_,0)) || !ok(cudaMemcpyAsync(&bad,evalFlags_.data(),sizeof(bad),cudaMemcpyDeviceToHost,stream)) || !ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF evaluation completion query failed");
    evalPending_=false; if(bad) { solved_=false; return fail(RbfStatus::NonFiniteInput,"RBF evaluation CVs contain non-finite values; generation rejected"); } return RbfStatus::Ok;
}
}}
