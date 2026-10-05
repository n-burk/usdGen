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
  // A non-finite rest sample is rejected at the extent proof on both the
  // direct and fresh paths, and the binding stays usable afterwards.
  std::vector<float3> nanRest=rest; nanRest[2].y=NAN;
  check(cudaMemcpyAsync(dr,nanRest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  CudaRbfBinding nanDirect; assert(nanDirect.Bind({dr,5},0,s)==RbfStatus::NonFiniteInput);
  CudaRbfBinding nanStaged;
  assert(nanStaged.BeginFreshBind({dr,5},0,s)==RbfStatus::Ok);
  check(cudaStreamSynchronize(s)); assert(nanStaged.CommitFreshBindExtent()==RbfStatus::NonFiniteInput);
  check(cudaMemcpyAsync(dr,rest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(nanDirect.Bind({dr,5},0,s)==RbfStatus::Ok);
  // A non-finite posed sample is rejected by the direct solve flag proof,
  // and the binding stays usable afterwards.
  std::vector<float3> nanPose=rest; nanPose[1].z=NAN;
  check(cudaMemcpyAsync(dp,nanPose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(nanDirect.Solve({dp,5},s)==RbfStatus::NonFiniteInput);
  check(cudaMemcpyAsync(dp,rest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(nanDirect.Solve({dp,5},s)==RbfStatus::Ok);
  // Fresh binding has three externally-proved, host-only commit boundaries.
  // These calls deliberately synchronize only in the test as the parent's
  // native-completion proof stand-in; production must not do so in commits.
  check(cudaMemcpyAsync(dr,rest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  CudaRbfBinding staged;
  assert(!staged.CanAcceptFreshSolve() && !staged.CanRollbackFreshSolve());
  // Commits cannot be guessed or replayed, and capture rejection has no
  // candidate side effect (the ordinary stream remains reusable).
  assert(staged.CommitFreshBindExtent()==RbfStatus::InvalidArgument);
  cudaGraph_t graph=nullptr; check(cudaStreamBeginCapture(s,cudaStreamCaptureModeGlobal));
  assert(staged.BeginFreshBind({dr,5},0,s)==RbfStatus::InvalidArgument);
  check(cudaStreamEndCapture(s,&graph)); check(cudaGraphDestroy(graph));
  assert(staged.BeginFreshBind({dr,5},0,s)==RbfStatus::Ok && staged.HasUnprovenWork() && staged.freshSampleCount()==0);
  check(cudaStreamSynchronize(s)); assert(staged.CommitFreshBindExtent()==RbfStatus::Ok && !staged.HasUnprovenWork());
  assert(staged.CommitFreshBindExtent()==RbfStatus::InvalidArgument);
  assert(staged.BeginFreshBindRank(s)==RbfStatus::Ok && staged.HasUnprovenWork());
  check(cudaStreamSynchronize(s)); assert(staged.CommitFreshBindRank()==RbfStatus::Ok && !staged.HasUnprovenWork());
  assert(staged.BeginFreshBindLu(s)==RbfStatus::Ok && staged.HasUnprovenWork());
  check(cudaStreamSynchronize(s)); assert(staged.CommitFreshBindLu()==RbfStatus::Ok && !staged.HasUnprovenWork());
  assert(staged.CommitFreshBindLu()==RbfStatus::InvalidArgument);
  assert(staged.freshSampleCount()==5);
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok && staged.HasUnprovenWork());
  check(cudaStreamSynchronize(s)); assert(staged.CommitFreshEvaluate()==RbfStatus::Ok && !staged.HasUnprovenWork());
  check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s)); check(cudaStreamSynchronize(s));
  for(int i=0;i<5;i++) assert(near(got[i],rest[i]));
  // The fresh pose has two proof boundaries.  It writes only private
  // coefficients until solver status is proven, then exposes them only to a
  // downstream fresh evaluation.  Acceptance remains an explicit final
  // transaction boundary.
  std::vector<float3> freshPose(5); for(int i=0;i<5;i++) freshPose[i]=f(rest[i].x+3,rest[i].y-2,rest[i].z+1);
  check(cudaMemcpyAsync(dp,freshPose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(staged.CommitFreshSolveInput()==RbfStatus::InvalidArgument);
  assert(staged.BeginFreshSolve({dp,5},s)==RbfStatus::Ok && staged.HasUnprovenWork());
  check(cudaStreamSynchronize(s)); assert(staged.CommitFreshSolveInput()==RbfStatus::Ok && !staged.HasUnprovenWork());
  // The private RHS is tied to the exact accepted factorization captured by
  // BeginFreshSolve; a rebind cannot interleave before its solve/commit.
  assert(staged.BeginFreshBind({dr,5},0,s)==RbfStatus::InvalidArgument);
  assert(staged.BeginFreshSolveFactors(s)==RbfStatus::Ok && staged.HasUnprovenWork());
  check(cudaStreamSynchronize(s)); assert(staged.CommitFreshSolve()==RbfStatus::Ok && !staged.HasUnprovenWork());
  // A solved pose cannot be overtaken by a rebind/new solve, and once
  // downstream evaluation is submitted neither decision may be guessed
  // before that proof is complete.
  assert(staged.BeginFreshBind({dr,5},0,s)==RbfStatus::InvalidArgument);
  assert(staged.BeginFreshSolve({dp,5},s)==RbfStatus::InvalidArgument);
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(!staged.CanAcceptFreshSolve() && !staged.CanRollbackFreshSolve());
  assert(staged.AcceptFreshSolve()==RbfStatus::InvalidArgument);
  assert(staged.RollbackFreshSolve()==RbfStatus::InvalidArgument);
  assert(staged.CommitFreshEvaluate()==RbfStatus::Ok); check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s)); check(cudaStreamSynchronize(s));
  for(int i=0;i<5;i++) assert(near(got[i],freshPose[i]));
  // Simulate a downstream publication rejection after a successful solve:
  // rollback swaps the old accepted coefficients back without rebinding.
  assert(staged.CanAcceptFreshSolve() && staged.CanRollbackFreshSolve());
  assert(staged.RollbackFreshSolve()==RbfStatus::Ok);
  assert(!staged.CanAcceptFreshSolve() && !staged.CanRollbackFreshSolve());
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshEvaluate()==RbfStatus::Ok); check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s)); check(cudaStreamSynchronize(s));
  for(int i=0;i<5;i++) assert(near(got[i],rest[i]));
  // A clean retry can solve the same pose and make it durable only through
  // AcceptFreshSolve after its downstream proof.
  assert(staged.BeginFreshSolve({dp,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshSolveInput()==RbfStatus::Ok);
  assert(staged.BeginFreshSolveFactors(s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshSolve()==RbfStatus::Ok);
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshEvaluate()==RbfStatus::Ok);
  assert(staged.CanAcceptFreshSolve() && staged.CanRollbackFreshSolve());
  assert(staged.AcceptFreshSolve()==RbfStatus::Ok);
  assert(!staged.CanAcceptFreshSolve() && !staged.CanRollbackFreshSolve());
  assert(staged.RollbackFreshSolve()==RbfStatus::InvalidArgument);
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshEvaluate()==RbfStatus::Ok); check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s)); check(cudaStreamSynchronize(s));
  for(int i=0;i<5;i++) assert(near(got[i],freshPose[i]));
  // A rejected posed candidate cannot alter the last accepted coefficients.
  pose[0]=f(NAN,0,0); check(cudaMemcpyAsync(dp,pose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(staged.BeginFreshSolve({dp,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshSolveInput()==RbfStatus::NonFiniteInput && !staged.HasUnprovenWork());
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshEvaluate()==RbfStatus::Ok); check(cudaMemcpyAsync(got.data(),do_,5*sizeof(float3),cudaMemcpyDeviceToHost,s)); check(cudaStreamSynchronize(s));
  for(int i=0;i<5;i++) assert(near(got[i],freshPose[i]));
  TestFailNextFreshRbfSolveAllocation();
  assert(staged.BeginFreshSolve({dr,5},s)==RbfStatus::CudaError && !staged.HasUnprovenWork());
  pose[0]=f(NAN,0,0); check(cudaMemcpyAsync(dp,pose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(staged.BeginFreshEvaluate({dp,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshEvaluate()==RbfStatus::NonFiniteInput);
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshEvaluate()==RbfStatus::Ok);
  // Test seams distinguish clean pre-enqueue failure (retryable, old field
  // retained) from a failure after a submitted D2H proof (permanent unsafe
  // candidate that cannot be committed or reused).
  CudaRbfBinding allocFault;
  TestFailNextFreshRbfBindAllocation();
  assert(allocFault.BeginFreshBind({dr,5},0,s)==RbfStatus::CudaError && !allocFault.HasUnprovenWork());
  assert(allocFault.BeginFreshBind({dr,5},0,s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(allocFault.CommitFreshBindExtent()==RbfStatus::Ok);
  assert(allocFault.BeginFreshBindRank(s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(allocFault.CommitFreshBindRank()==RbfStatus::Ok);
  assert(allocFault.BeginFreshBindLu(s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(allocFault.CommitFreshBindLu()==RbfStatus::Ok && allocFault.freshSampleCount()==5);
  CudaRbfBinding inputUnsafe;
  assert(inputUnsafe.BeginFreshBind({dr,5},0,s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(inputUnsafe.CommitFreshBindExtent()==RbfStatus::Ok);
  assert(inputUnsafe.BeginFreshBindRank(s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(inputUnsafe.CommitFreshBindRank()==RbfStatus::Ok);
  assert(inputUnsafe.BeginFreshBindLu(s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(inputUnsafe.CommitFreshBindLu()==RbfStatus::Ok);
  TestFailNextFreshRbfSolveAfterInputSubmit();
  assert(inputUnsafe.BeginFreshSolve({dr,5},s)==RbfStatus::CudaError && inputUnsafe.HasUnprovenWork());
  check(cudaStreamSynchronize(s));
  assert(inputUnsafe.CommitFreshSolveInput()==RbfStatus::InvalidArgument); inputUnsafe.AbandonFresh();
  assert(allocFault.BeginFreshSolve({dr,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(allocFault.CommitFreshSolveInput()==RbfStatus::Ok);
  TestFailNextFreshRbfSolveAfterSolverSubmit();
  assert(allocFault.BeginFreshSolveFactors(s)==RbfStatus::CudaError && allocFault.HasUnprovenWork());
  check(cudaStreamSynchronize(s));
  assert(allocFault.CommitFreshSolve()==RbfStatus::InvalidArgument); allocFault.AbandonFresh();
  TestFailNextFreshRbfEvaluateAllocation();
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::CudaError && !staged.HasUnprovenWork());
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s)); assert(staged.CommitFreshEvaluate()==RbfStatus::Ok);
  CudaRbfBinding bindUnsafe;
  TestFailNextFreshRbfBindAfterSubmit();
  assert(bindUnsafe.BeginFreshBind({dr,5},0,s)==RbfStatus::CudaError && bindUnsafe.HasUnprovenWork());
  assert(bindUnsafe.CommitFreshBindExtent()==RbfStatus::InvalidArgument); bindUnsafe.AbandonFresh();
  // A proven rejected rebind cannot replace the accepted identity field.
  check(cudaMemcpyAsync(dr,plane.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(staged.BeginFreshBind({dr,5},0,s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshBindExtent()==RbfStatus::Ok);
  assert(staged.BeginFreshBindRank(s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshBindRank()==RbfStatus::RankDeficient && staged.freshSampleCount()==5);
  check(cudaMemcpyAsync(dr,rest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::Ok); check(cudaStreamSynchronize(s));
  assert(staged.CommitFreshEvaluate()==RbfStatus::Ok);
  // Unsafe evaluation is terminal: quarantine is permanent, so this is the
  // final use of staged and both fresh entry points reject thereafter.
  TestFailNextFreshRbfEvaluateAfterSubmit();
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::CudaError && staged.HasUnprovenWork());
  assert(staged.CommitFreshEvaluate()==RbfStatus::InvalidArgument); staged.AbandonFresh();
  assert(staged.BeginFreshEvaluate({dr,5},{do_,5},s)==RbfStatus::InvalidArgument);
  assert(staged.BeginFreshBind({dr,5},0,s)==RbfStatus::InvalidArgument);
  cudaFree(dr);cudaFree(dp);cudaFree(do_);cudaStreamDestroy(s);cudaStreamDestroy(s2);
}
