#include <chrono>
#include <cstdio>
#include <vector>
#include <random>
struct V3{float x,y,z;};
int main(){
  const size_t n=100000*16; std::vector<V3> P(n),V(n),P2(n),R(n);
  std::mt19937 rng(1); std::uniform_real_distribution<float> d(0,1);
  for(size_t i=0;i<n;++i){P[i]={d(rng),d(rng),d(rng)};V[i]={d(rng),d(rng),d(rng)};P2[i]={d(rng),d(rng),d(rng)};}
  auto bench=[&](const char*name,auto f){f();auto t=std::chrono::steady_clock::now();for(int r=0;r<20;++r)f();
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count()/20;printf("%-42s %8.3f ms\n",name,ms);};
  const float t=0.25f/24.f, a=0.5f, fps=24.f;
  bench("P + t*V (velocity extrapolate, 1 sample)",[&]{for(size_t i=0;i<n;++i){R[i]={P[i].x+t*V[i].x,P[i].y+t*V[i].y,P[i].z+t*V[i].z};}});
  bench("lerp(P0,P1,a)",[&]{for(size_t i=0;i<n;++i){R[i]={P[i].x+a*(P2[i].x-P[i].x),P[i].y+a*(P2[i].y-P[i].y),P[i].z+a*(P2[i].z-P[i].z)};}});
  bench("(P1-P0)*fps (finite-difference velocity)",[&]{for(size_t i=0;i<n;++i){R[i]={(P2[i].x-P[i].x)*fps,(P2[i].y-P[i].y)*fps,(P2[i].z-P[i].z)*fps};}});
  bench("copy 1 sample (VtArray detach cost)",[&]{R=P;});
  printf("points=%zu bytes/sample=%.1f MB\n",n,n*12/1e6);
  return (int)(R[7].x>1e9f);
}
