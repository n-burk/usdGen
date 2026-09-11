#include "rbf.h"

#include <cmath>
#include <climits>
#include <limits>

namespace usdGen { namespace gpu {
namespace {
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
__global__ void evalKernel(const float3* cvs, float3* out, int c, const float3* samples, const double* coef, int n, int m, double cx, double cy, double cz, double invScale, double scale, int* flags) {
    int i=blockIdx.x*blockDim.x+threadIdx.x; if(i>=c) return; float3 p=cvs[i];
    if(!isfinite(p.x)||!isfinite(p.y)||!isfinite(p.z)){mark(flags,1);return;}
    double x=(p.x-cx)*invScale,y=(p.y-cy)*invScale,z=(p.z-cz)*invScale;
    double ox=x+coef[n]+coef[n+1]*x+coef[n+2]*y+coef[n+3]*z;
    double oy=y+coef[m+n]+coef[m+n+1]*x+coef[m+n+2]*y+coef[m+n+3]*z;
    double oz=z+coef[2*m+n]+coef[2*m+n+1]*x+coef[2*m+n+2]*y+coef[2*m+n+3]*z;
    for(int j=0;j<n;++j){ float3 s=samples[j]; double dx=x-(s.x-cx)*invScale,dy=y-(s.y-cy)*invScale,dz=z-(s.z-cz)*invScale; double r=sqrt(dx*dx+dy*dy+dz*dz); r*=r*r; ox+=coef[j]*r;oy+=coef[m+j]*r;oz+=coef[2*m+j]*r; }
    out[i]=make_float3((float)(ox*scale+cx),(float)(oy*scale+cy),(float)(oz*scale+cz));
}
inline bool ok(cudaError_t e) { return e==cudaSuccess; }
}

CudaRbfBinding::~CudaRbfBinding() { if (stateReady_) { cudaEventSynchronize(stateReady_); cudaEventDestroy(stateReady_); } if (evalReady_) cudaEventDestroy(evalReady_); if (solver_) cusolverDnDestroy(solver_); }
RbfStatus CudaRbfBinding::fail(RbfStatus s, const char* why) { diagnostic_=why; return s; }

RbfStatus CudaRbfBinding::Bind(DeviceView<const float3> samples, double smoothing, cudaStream_t stream) {
    if (evalPending_) return fail(RbfStatus::InvalidArgument,"RBF Finish is required before rebinding a pending evaluation");
    sampleCount_=order_=0; solved_=false; evalPending_=false; diagnostic_.clear();
    if (!samples.data || samples.size < 4 || samples.size > 46336u || !std::isfinite(smoothing) || smoothing < 0.0)
        return fail(RbfStatus::InvalidArgument,"RBF requires 4+ samples and finite non-negative smoothing");
    const int n=(int)samples.size, m=n+4; // m*m and all launch indices remain signed-int safe
    if (stateReady_ && !ok(cudaStreamWaitEvent(stream,stateReady_,0))) return fail(RbfStatus::CudaError,"RBF state wait failed");
    if (!stateReady_ && !ok(cudaEventCreateWithFlags(&stateReady_,cudaEventDisableTiming))) return fail(RbfStatus::CudaError,"RBF state event creation failed");
    if (!evalReady_ && !ok(cudaEventCreateWithFlags(&evalReady_,cudaEventDisableTiming))) return fail(RbfStatus::CudaError,"RBF evaluation event creation failed");
    if (!solver_ && cusolverDnCreate(&solver_) != CUSOLVER_STATUS_SUCCESS) return fail(RbfStatus::SolverError,"cuSOLVER create failed");
    if (!ok(rest_.reset(n)) || !ok(matrix_.reset(size_t(m)*m)) || !ok(coefficients_.reset(size_t(m)*3)) || !ok(pivots_.reset(m)) || !ok(info_.reset(1)) || !ok(flags_.reset(1)) || !ok(evalFlags_.reset(1)) || !ok(work_.reset(size_t(m)*m)) || !ok(gram_.reset(16))) return fail(RbfStatus::CudaError,"RBF device allocation failed");
    if (!ok(cudaMemcpyAsync(rest_.data(),samples.data,n*sizeof(float3),cudaMemcpyDeviceToDevice,stream))) return fail(RbfStatus::CudaError,"RBF rest copy failed");
    DeviceBuffer<float> extents; if(!ok(extents.reset(6))) return fail(RbfStatus::CudaError,"RBF extent allocation failed");
    initExtent<<<1,1,0,stream>>>(extents.data(),flags_.data()); extentKernel<<<32,128,0,stream>>>(rest_.data(),n,extents.data(),flags_.data());
    float e[6]; int flag=0;
    if(!ok(cudaMemcpyAsync(e,extents.data(),sizeof(e),cudaMemcpyDeviceToHost,stream)) || !ok(cudaMemcpyAsync(&flag,flags_.data(),sizeof(flag),cudaMemcpyDeviceToHost,stream)) || !ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF extent query failed");
    if(flag) return fail(RbfStatus::NonFiniteInput,"RBF rest samples contain non-finite values");
    center_[0]=(double)e[0] + ((double)e[3]-(double)e[0])*.5; center_[1]=(double)e[1] + ((double)e[4]-(double)e[1])*.5; center_[2]=(double)e[2] + ((double)e[5]-(double)e[2])*.5; scale_=std::max((double)e[3]-e[0],std::max((double)e[4]-e[1],(double)e[5]-e[2]));
    if(!std::isfinite(scale_) || scale_ <= 0.0) return fail(RbfStatus::RankDeficient,"RBF rest samples have zero extent");
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
    if(!sampleCount_ || !solved_ || !cvs.data || !out.data || !cvs.size || cvs.size!=out.size || cvs.size>size_t(INT_MAX)) return fail(RbfStatus::InvalidArgument,"RBF Evaluate requires a solved binding and equal non-empty bounded device views");
    if(!ok(cudaStreamWaitEvent(stream,stateReady_,0)) || (!evalPending_ && !ok(cudaMemsetAsync(evalFlags_.data(),0,sizeof(int),stream)))) return fail(RbfStatus::CudaError,"RBF evaluation state setup failed"); evalKernel<<<(cvs.size+255)/256,256,0,stream>>>(cvs.data,out.data,(int)cvs.size,rest_.data(),coefficients_.data(),(int)sampleCount_,(int)order_,center_[0],center_[1],center_[2],1.0/scale_,scale_,evalFlags_.data());
    if(cudaGetLastError()!=cudaSuccess || !ok(cudaEventRecord(stateReady_,stream)) || !ok(cudaEventRecord(evalReady_,stream))) return fail(RbfStatus::CudaError,"RBF evaluation launch failed"); evalPending_=true; return RbfStatus::Ok;
}
RbfStatus CudaRbfBinding::Finish(cudaStream_t stream) {
    if(!evalPending_) return RbfStatus::Ok;
    int bad=0; if(!ok(cudaStreamWaitEvent(stream,evalReady_,0)) || !ok(cudaMemcpyAsync(&bad,evalFlags_.data(),sizeof(bad),cudaMemcpyDeviceToHost,stream)) || !ok(cudaStreamSynchronize(stream))) return fail(RbfStatus::CudaError,"RBF evaluation completion query failed");
    evalPending_=false; if(bad) { solved_=false; return fail(RbfStatus::NonFiniteInput,"RBF evaluation CVs contain non-finite values; generation rejected"); } return RbfStatus::Ok;
}
}}
