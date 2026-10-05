#include "gpu/rbf.h"
#include <cuda_runtime.h>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  // Direct-evaluate R cache: the cold miss, the hit, refills, rebinding,
  // reshaping, and the flag path are all bitwise against the seam-disabled
  // uncached kernel on the same binding; the path counters prove which
  // path each Evaluate took.
  {
    auto readback = [&](float3* d, int nn){ std::vector<float3> v(nn); check(cudaMemcpyAsync(v.data(),d,size_t(nn)*sizeof(float3),cudaMemcpyDeviceToHost,s)); check(cudaStreamSynchronize(s)); return v; };
    auto sameBits = [&](std::vector<float3> const& a, std::vector<float3> const& b){ return a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(float3))==0; };
    std::vector<float3> crest={f(0,0,0),f(1,0,0),f(0,1,0),f(0,0,1),f(1,1,1)};
    std::vector<float3> cpose(5); for(int i=0;i<5;i++) cpose[i]=f(-crest[i].y+2,crest[i].x-3,crest[i].z+4);
    std::vector<float3> ccvs={f(.1f,.2f,.3f),f(.4f,.5f,.6f),f(.7f,.8f,.9f),f(1.1f,1.2f,1.3f),f(-.5f,.25f,2.f)};
    float3 *cr,*cp,*cc,*co; check(cudaMalloc(&cr,5*sizeof(float3))); check(cudaMalloc(&cp,5*sizeof(float3))); check(cudaMalloc(&cc,5*sizeof(float3))); check(cudaMalloc(&co,5*sizeof(float3)));
    check(cudaMemcpyAsync(cr,crest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    check(cudaMemcpyAsync(cp,cpose.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    check(cudaMemcpyAsync(cc,ccvs.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    CudaRbfBinding cb; assert(cb.Bind({cr,5},0,s)==RbfStatus::Ok); assert(cb.Solve({cp,5},s)==RbfStatus::Ok);
    uint64_t h0=CudaRbfEvalCacheHitsForTesting(), m0=CudaRbfEvalCacheMissesForTesting();
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    std::vector<float3> missOut=readback(co,5);
    assert(CudaRbfEvalCacheMissesForTesting()==m0+1 && CudaRbfEvalCacheHitsForTesting()==h0);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    std::vector<float3> hitOut=readback(co,5);
    assert(CudaRbfEvalCacheHitsForTesting()==h0+1);
    assert(sameBits(missOut,hitOut));
    // The seam-disabled uncached kernel is bitwise the same output, and
    // moves neither counter.
    TestDisableCudaRbfEvalCache(true);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    assert(sameBits(readback(co,5),hitOut));
    assert(CudaRbfEvalCacheHitsForTesting()==h0+1 && CudaRbfEvalCacheMissesForTesting()==m0+1);
    TestDisableCudaRbfEvalCache(false);
    // Rebinding identical rest keeps the cache: still a hit, same bits.
    assert(cb.Bind({cr,5},0,s)==RbfStatus::Ok); assert(cb.Solve({cp,5},s)==RbfStatus::Ok);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    assert(sameBits(readback(co,5),hitOut));
    assert(CudaRbfEvalCacheHitsForTesting()==h0+2);
    // A 1-ULP rest change misses and refills; the refill matches uncached.
    std::vector<float3> crest2=crest; crest2[0].x=std::nextafterf(crest2[0].x,2.f);
    check(cudaMemcpyAsync(cr,crest2.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    assert(cb.Bind({cr,5},0,s)==RbfStatus::Ok); assert(cb.Solve({cp,5},s)==RbfStatus::Ok);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    std::vector<float3> refillOut=readback(co,5);
    assert(CudaRbfEvalCacheMissesForTesting()==m0+2);
    TestDisableCudaRbfEvalCache(true);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    assert(sameBits(readback(co,5),refillOut));
    TestDisableCudaRbfEvalCache(false);
    // A 1-ULP CV change misses and refills; the refill matches uncached.
    check(cudaMemcpyAsync(cr,crest.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    assert(cb.Bind({cr,5},0,s)==RbfStatus::Ok); assert(cb.Solve({cp,5},s)==RbfStatus::Ok);
    std::vector<float3> ccvs2=ccvs; ccvs2[3].z=std::nextafterf(ccvs2[3].z,-2.f);
    check(cudaMemcpyAsync(cc,ccvs2.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    std::vector<float3> cvsRefill=readback(co,5);
    assert(CudaRbfEvalCacheMissesForTesting()==m0+3);
    TestDisableCudaRbfEvalCache(true);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    assert(sameBits(readback(co,5),cvsRefill));
    TestDisableCudaRbfEvalCache(false);
    // In-place evaluation through a warm cache matches uncached bitwise.
    check(cudaMemcpyAsync(cc,ccvs.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    assert(cb.Evaluate({cc,5},{cc,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    std::vector<float3> inplace=readback(cc,5);
    assert(CudaRbfEvalCacheMissesForTesting()==m0+4);
    check(cudaMemcpyAsync(cc,ccvs.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    TestDisableCudaRbfEvalCache(true);
    assert(cb.Evaluate({cc,5},{cc,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    assert(sameBits(readback(cc,5),inplace));
    TestDisableCudaRbfEvalCache(false);
    // A count change reshapes the cache; coming back restores the bits.
    float3 *cc8,*co8; check(cudaMalloc(&cc8,8*sizeof(float3))); check(cudaMalloc(&co8,8*sizeof(float3)));
    std::vector<float3> ccvs8(8); for(int i=0;i<8;i++) ccvs8[i]=f(.1f*float(i),.2f*float(i)+.05f,.3f*float(i)-.07f);
    check(cudaMemcpyAsync(cc8,ccvs8.data(),8*sizeof(float3),cudaMemcpyHostToDevice,s));
    assert(cb.Evaluate({cc8,8},{co8,8},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    std::vector<float3> wideOut=readback(co8,8);
    assert(CudaRbfEvalCacheMissesForTesting()==m0+5);
    TestDisableCudaRbfEvalCache(true);
    assert(cb.Evaluate({cc8,8},{co8,8},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    assert(sameBits(readback(co8,8),wideOut));
    TestDisableCudaRbfEvalCache(false);
    check(cudaMemcpyAsync(cc,ccvs.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok); assert(cb.Finish(s)==RbfStatus::Ok);
    assert(sameBits(readback(co,5),hitOut));
    // A NaN CV refills (miss) and then hits over the NaN proof copy: the
    // hit-path evaluator raises the flag exactly like the direct kernel,
    // and the generation stays poisoned the same way.
    std::vector<float3> nanCvs=ccvs; nanCvs[1].y=NAN;
    check(cudaMemcpyAsync(cc,nanCvs.data(),5*sizeof(float3),cudaMemcpyHostToDevice,s));
    uint64_t h1=CudaRbfEvalCacheHitsForTesting(), m1=CudaRbfEvalCacheMissesForTesting();
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok);
    assert(CudaRbfEvalCacheMissesForTesting()==m1+1);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::Ok);
    assert(CudaRbfEvalCacheHitsForTesting()==h1+1);
    assert(cb.Finish(s)==RbfStatus::NonFiniteInput);
    assert(cb.Evaluate({cc,5},{co,5},s)==RbfStatus::InvalidArgument);
    cudaFree(cr); cudaFree(cp); cudaFree(cc); cudaFree(co); cudaFree(cc8); cudaFree(co8);
  }
  // Over the cache cap the fallback kernel runs: neither counter moves
  // and the identity field reproduces its inputs.
  {
    int const bn=1000, bp=200000;
    std::vector<float3> brest(size_t(bn), f(0,0,0));
    for(int i=0;i<bn;i++){ int x=i%10,y=(i/10)%10,z=i/100; brest[size_t(i)]=f(float(x)+.01f*float(i%7),float(y)+.01f*float((i+3)%7),float(z)+.01f*float((i+5)%7)); }
    float3 *bdr,*bdp,*bdc,*bdo;
    check(cudaMalloc(&bdr,size_t(bn)*sizeof(float3))); check(cudaMalloc(&bdp,size_t(bn)*sizeof(float3)));
    check(cudaMalloc(&bdc,size_t(bp)*sizeof(float3))); check(cudaMalloc(&bdo,size_t(bp)*sizeof(float3)));
    check(cudaMemcpyAsync(bdr,brest.data(),size_t(bn)*sizeof(float3),cudaMemcpyHostToDevice,s));
    check(cudaMemcpyAsync(bdp,brest.data(),size_t(bn)*sizeof(float3),cudaMemcpyHostToDevice,s));
    std::vector<float3> bcvs(size_t(bp), f(0,0,0));
    for(int i=0;i<bp;i++) bcvs[size_t(i)]=f(.01f*float(i%1000),.02f*float((i+7)%1000),.03f*float((i+13)%1000));
    check(cudaMemcpyAsync(bdc,bcvs.data(),size_t(bp)*sizeof(float3),cudaMemcpyHostToDevice,s));
    CudaRbfBinding bb; assert(bb.Bind({bdr,size_t(bn)},0,s)==RbfStatus::Ok); assert(bb.Solve({bdp,size_t(bn)},s)==RbfStatus::Ok);
    uint64_t h0=CudaRbfEvalCacheHitsForTesting(), m0=CudaRbfEvalCacheMissesForTesting();
    assert(bb.Evaluate({bdc,size_t(bp)},{bdo,size_t(bp)},s)==RbfStatus::Ok); assert(bb.Finish(s)==RbfStatus::Ok);
    assert(CudaRbfEvalCacheHitsForTesting()==h0 && CudaRbfEvalCacheMissesForTesting()==m0);
    std::vector<float3> bout(size_t(bp), f(0,0,0)); check(cudaMemcpyAsync(bout.data(),bdo,size_t(bp)*sizeof(float3),cudaMemcpyDeviceToHost,s)); check(cudaStreamSynchronize(s));
    for(int i=0;i<bp;i++){ assert(std::isfinite(bout[size_t(i)].x)&&std::isfinite(bout[size_t(i)].y)&&std::isfinite(bout[size_t(i)].z)); assert(near(bout[size_t(i)],bcvs[size_t(i)])); }
    cudaFree(bdr); cudaFree(bdp); cudaFree(bdc); cudaFree(bdo);
  }
  cudaFree(dr);cudaFree(dp);cudaFree(do_);cudaStreamDestroy(s);cudaStreamDestroy(s2);
}
