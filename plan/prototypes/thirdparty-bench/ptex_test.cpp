#include <Ptexture.h>
#include <PtexUtils.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <cmath>
int main() {
    // --- write a quad-mesh ptex: 2 faces sharing edge (face0 right edge <-> face1 left edge)
    Ptex::String err;
    const int nfaces = 2;
    PtexPtr<PtexWriter> w(PtexWriter::open("quad.ptx", Ptex::mt_quad, Ptex::dt_float, 3, /*alphachan*/-1, nfaces, err, /*genmipmaps*/true));
    if (!w) { printf("open failed: %s\n", err.c_str()); return 1; }
    for (int f = 0; f < nfaces; ++f) {
        Ptex::Res res(f == 0 ? 6 : 4, f == 0 ? 6 : 5);   // per-face resolution: 64x64 and 16x32
        int adjfaces[4] = {-1, f==0 ? 1 : -1, -1, f==1 ? 0 : -1};
        int adjedges[4] = {0, f==0 ? Ptex::e_left : 0, 0, f==1 ? Ptex::e_right : 0};
        Ptex::FaceInfo info(res, adjfaces, adjedges, /*isSubface*/false);
        std::vector<float> data(res.size() * 3);
        for (int y = 0; y < res.v(); ++y) for (int x = 0; x < res.u(); ++x) {
            float* p = &data[(y * res.u() + x) * 3];
            p[0] = float(x) / (res.u()-1); p[1] = float(y) / (res.v()-1); p[2] = f;
        }
        if (!w->writeFace(f, info, data.data(), /*stride*/0)) { printf("writeFace failed\n"); return 1; }
    }
    w->writeMeta("usdGen:source", "test");
    if (!w->close(err)) { printf("close failed: %s\n", err.c_str()); return 1; }

    // --- write a triangle-mesh ptex (must be square res)
    {
        PtexPtr<PtexWriter> wt(PtexWriter::open("tri.ptx", Ptex::mt_triangle, Ptex::dt_uint8, 1, -1, 1, err, true));
        Ptex::Res res(3,3); int af[4]={-1,-1,-1,-1}; int ae[4]={0,0,0,0};
        Ptex::FaceInfo info(res, af, ae);
        std::vector<unsigned char> d(res.size(), 200);
        bool ok = wt->writeFace(0, info, d.data()); ok &= wt->close(err);
        printf("triangle ptex write ok=%d %s\n", ok, err.c_str());
    }

    // --- read back via cache + filter
    PtexPtr<PtexCache> cache(PtexCache::create(/*maxFiles*/16, /*maxMem*/256<<20, /*premultiply*/false));
    PtexPtr<PtexTexture> tx(cache->get("quad.ptx", err));
    if (!tx) { printf("get failed: %s\n", err.c_str()); return 1; }
    printf("numFaces=%d meshType=%d dataType=%d nchan=%d alpha=%d hasMipMaps=%d\n", tx->numFaces(), tx->meshType(), tx->dataType(), tx->numChannels(), tx->alphaChannel(), tx->hasMipMaps());
    for (int f = 0; f < tx->numFaces(); ++f) { const Ptex::FaceInfo& fi = tx->getFaceInfo(f); printf(" face %d res=%dx%d adj=[%d %d %d %d] isSubface=%d\n", f, fi.res.u(), fi.res.v(), fi.adjface(0), fi.adjface(1), fi.adjface(2), fi.adjface(3), fi.isSubface()); }
    float px[3]; tx->getPixel(0, 63, 0, px, 0, 3); printf(" getPixel(face0,u=63,v=0)=%g %g %g\n", px[0], px[1], px[2]);
    PtexFilter::Options opts(PtexFilter::f_bilinear, /*lerp*/false, /*sharpness*/0, /*noedgeblend*/false);
    PtexPtr<PtexFilter> filt(PtexFilter::getFilter(tx, opts));
    float r[3];
    filt->eval(r, 0, 3, /*faceid*/0, /*u*/0.5f, /*v*/0.25f, /*uw1*/1.f/64, /*vw1*/0, /*uw2*/0, /*vw2*/1.f/64);
    printf(" filter eval face0 (0.5,0.25) = %g %g %g\n", r[0], r[1], r[2]);
    filt->eval(r, 0, 3, 0, 0.999f, 0.5f, 0.05f, 0, 0, 0.05f);  // straddles shared edge -> blends with face 1
    printf(" filter eval face0 (0.999,0.5) wide = %g %g %g (blend across edge)\n", r[0], r[1], r[2]);

    // timing: single thread bilinear lookups
    const int N = 2000000; auto t0 = std::chrono::steady_clock::now(); double acc = 0;
    for (int i = 0; i < N; ++i) { float u = (i % 1000) * 0.001f, v = ((i / 1000) % 1000) * 0.001f; filt->eval(r, 0, 3, i & 1, u, v, 1.f/64, 0, 0, 1.f/64); acc += r[0]; }
    auto t1 = std::chrono::steady_clock::now();
    printf(" bilinear filter eval: %.0f ns/lookup (acc=%g)\n", std::chrono::duration<double,std::nano>(t1-t0).count()/N, acc);
    // timing: box filter with wider footprint
    PtexPtr<PtexFilter> filtBox(PtexFilter::getFilter(tx, PtexFilter::Options(PtexFilter::f_box)));
    t0 = std::chrono::steady_clock::now(); acc = 0;
    for (int i = 0; i < N; ++i) { float u = (i % 1000) * 0.001f, v = ((i / 1000) % 1000) * 0.001f; filtBox->eval(r, 0, 3, i & 1, u, v, 4.f/64, 0, 0, 4.f/64); acc += r[0]; }
    t1 = std::chrono::steady_clock::now();
    printf(" box filter eval (4 texel footprint): %.0f ns/lookup (acc=%g)\n", std::chrono::duration<double,std::nano>(t1-t0).count()/N, acc);
    // multithread: shared cache+texture, ONE FILTER PER THREAD
    const int T = 8; std::vector<double> sums(T); t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> th;
    for (int t = 0; t < T; ++t) th.emplace_back([&, t]{ PtexPtr<PtexTexture> tex(cache->get("quad.ptx", err)); PtexPtr<PtexFilter> f(PtexFilter::getFilter(tex, opts)); float rr[3]; double s=0; for (int i=0;i<N;++i){ float u=(i%1000)*0.001f, v=((i/1000)%1000)*0.001f; f->eval(rr,0,3,i&1,u,v,1.f/64,0,0,1.f/64); s+=rr[0]; } sums[t]=s; });
    for (auto& x : th) x.join(); t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
    printf(" %d threads, per-thread filter, shared cache: %.0f ns/lookup/thread, %.1f M lookups/s aggregate\n", T, ms*1e6/N, T*N/ms/1e3);
    PtexCache::Stats st; cache->getStats(st); printf(" cache stats: memUsed=%zu filesOpen=%zu blockReads=%zu\n", st.memUsed, st.filesOpen, st.blockReads);
    return 0;
}
