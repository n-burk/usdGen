#include "gpu/rbf.h"
#include <cuda_runtime.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace usdGen::gpu;
static float3 f(float x,float y,float z){return make_float3(x,y,z);}
static bool near(float3 a,float3 b,float e=2e-4f){return fabs(a.x-b.x)<e&&fabs(a.y-b.y)<e&&fabs(a.z-b.z)<e;}
static void check(cudaError_t e){if(e!=cudaSuccess){std::fprintf(stderr,"CUDA: %s\n",cudaGetErrorString(e));std::exit(1);}}
int main(){
  cudaStream_t s, s2; check(cudaStreamCreate(&s)); check(cudaStreamCreate(&s2));
  // Non-coplanar support is deliberately required by the full affine basis.
  std::vector<float3> rest={f(0,0,0),f(1,0,0),f(0,1,0),f(0,0,1),f(1,1,1)};
  float3 *dr,*dp,*do_; check(cudaMalloc(&dr,5*sizeof(float3)));check(cudaMalloc(&dp,5*sizeof(float3)));check(cudaMalloc(&do_,5*sizeof(float3)));
  check(cudaMemcpyAsync(dr,rest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  CudaRbfBinding r; assert(r.Bind({dr,5},0,s)==RbfStatus::Ok);
  // Bind exposes the explicit identity state; no uninitialized coefficients.
  assert(r.Evaluate({dr,5},{do_,5},s2)==RbfStatus::Ok); assert(r.Finish(s2)==RbfStatus::Ok);
  std::vector<float3> got(5);check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s2));check(cudaStreamSynchronize(s2));for(int i=0;i<5;i++)assert(near(got[i],rest[i]));
  // Identity and repeated Solve reuse the persistent factorization.
  check(cudaMemcpyAsync(dp,rest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s)); assert(r.Solve({dp,5},s)==RbfStatus::Ok); assert(r.Evaluate({dr,5},{do_,5},s)==RbfStatus::Ok); assert(r.Finish(s)==RbfStatus::Ok);
  check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s));check(cudaStreamSynchronize(s));for(int i=0;i<5;i++)assert(near(got[i],rest[i]));
  // Affine rotation around Z plus translation must be represented exactly.
  std::vector<float3> pose(5);for(int i=0;i<5;i++)pose[i]=f(-rest[i].y+2,rest[i].x-3,rest[i].z+4);
  check(cudaMemcpyAsync(dp,pose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));assert(r.Solve({dp,5},s)==RbfStatus::Ok);assert(r.Evaluate({dr,5},{do_,5},s2)==RbfStatus::Ok); assert(r.Finish(s2)==RbfStatus::Ok); check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s2));check(cudaStreamSynchronize(s2));for(int i=0;i<5;i++)assert(near(got[i],pose[i]));
  // Interpolation at drivers and a nonlinear displacement (test-only readback oracle).
  for(int i=0;i<5;i++)pose[i].z+=rest[i].x*rest[i].y+.2f*rest[i].z*rest[i].z;
  check(cudaMemcpyAsync(dp,pose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));assert(r.Solve({dp,5},s)==RbfStatus::Ok);assert(r.Evaluate({dr,5},{do_,5},s)==RbfStatus::Ok);assert(r.Finish(s)==RbfStatus::Ok);check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s));check(cudaStreamSynchronize(s));for(int i=0;i<5;i++)assert(near(got[i],pose[i],1e-3f));
  // Multiple async evaluations share one completion diagnostic; a NaN poisons
  // the generation and prevents a subsequent publish/evaluate.
  pose[0].x=NAN; check(cudaMemcpyAsync(dp,pose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));assert(r.Evaluate({dp,5},{do_,5},s)==RbfStatus::Ok);assert(r.Evaluate({dr,5},{do_,5},s2)==RbfStatus::Ok);assert(r.Finish(s2)==RbfStatus::NonFiniteInput);assert(r.Evaluate({dr,5},{do_,5},s)==RbfStatus::InvalidArgument);
  // A planar binding is rejected even with smoothing: polynomial rank is absent.
  std::vector<float3> plane={f(0,0,0),f(1,0,0),f(0,1,0),f(1,1,0),f(.2f,.3f,0)};check(cudaMemcpyAsync(dr,plane.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));CudaRbfBinding bad;assert(bad.Bind({dr,5},1e-3,s)==RbfStatus::RankDeficient);
  cudaFree(dr);cudaFree(dp);cudaFree(do_);cudaStreamDestroy(s);cudaStreamDestroy(s2);
}
