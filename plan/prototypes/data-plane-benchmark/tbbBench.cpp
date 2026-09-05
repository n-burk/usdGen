// Bespoke TBB DAG prototype for the same workload as vdfBench:
//   source pool (VtVec3fArray, N curves x 8 CVs)
//     -> S styler nodes, each with per-chunk dirty bits
// Two storage modes:
//   --per-node-buffers 1  every node owns a VtArray (per-node caching, 5x mem)
//   --per-node-buffers 0  one buffer passed through the chain (VDF-like)
//
// Uses tbb::parallel_for directly with a fixed chunk grain; VtArray gives
// copy-on-write when a result is handed to Hydra.

#include "pxr/pxr.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/work/threadLimits.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include "kernel.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

using Clock = std::chrono::steady_clock;
static double MsSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

static int GetArg(int argc, char **argv, const char *name, int def)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], name)) return atoi(argv[i + 1]);
    }
    return def;
}

namespace {

struct Node {
    float amount = 0.25f;
    uint32_t seed = 1;
    VtVec3fArray out;              // per-node buffer (mode 1 only)
    std::vector<uint8_t> dirty;    // per-chunk dirty bits
};

}  // namespace

int main(int argc, char **argv)
{
    const int curves       = GetArg(argc, argv, "--curves", 100000);
    const int stylers      = GetArg(argc, argv, "--stylers", 5);
    const int runs         = GetArg(argc, argv, "--runs", 5);
    const int chunkCurves  = GetArg(argc, argv, "--chunk", 1024);
    const int perNodeBufs  = GetArg(argc, argv, "--per-node-buffers", 1);
    const int sparsePermil = GetArg(argc, argv, "--sparse-permil", 10);
    const int serialMode   = GetArg(argc, argv, "--serial", 0);

    const size_t nElem = size_t(curves) * probe::CVS;
    const size_t nChunks = (size_t(curves) + chunkCurves - 1) / chunkCurves;

    printf("# tbbBench curves=%d stylers=%d chunk=%d perNodeBuffers=%d "
           "serial=%d chunks=%zu threads=%u\n",
           curves, stylers, chunkCurves, perNodeBufs, serialMode, nChunks,
           WorkGetConcurrencyLimit());

    VtVec3fArray source(nElem);
    {
        GfVec3f *p = source.data();
        for (int c = 0; c < curves; ++c) {
            const float fx = float(c % 512) * 0.01f;
            const float fz = float(c / 512) * 0.01f;
            for (int k = 0; k < probe::CVS; ++k) {
                p[size_t(c) * probe::CVS + k] =
                    GfVec3f(fx, float(k) * 0.1f, fz);
            }
        }
    }

    std::vector<Node> nodes(stylers);
    auto tBuild = Clock::now();
    for (int s = 0; s < stylers; ++s) {
        nodes[s].amount = 0.25f + 0.05f * s;
        nodes[s].seed = 1u + uint32_t(s) * 7919u;
        nodes[s].dirty.assign(nChunks, 1);
        if (perNodeBufs) nodes[s].out.resize(nElem);
    }
    VtVec3fArray shared;
    if (!perNodeBufs) shared.resize(nElem);
    printf("build_ms                    %8.2f\n", MsSince(tBuild));

    auto runChain = [&](void) {
        if (perNodeBufs) {
            const GfVec3f *in = source.cdata();
            for (int s = 0; s < stylers; ++s) {
                Node &n = nodes[s];
                GfVec3f *outp = n.out.data();
                const GfVec3f *inp = in;
                auto body = [&](const tbb::blocked_range<size_t> &r) {
                    for (size_t ci = r.begin(); ci != r.end(); ++ci) {
                        if (!n.dirty[ci]) continue;
                        const size_t c0 = ci * chunkCurves;
                        const size_t c1 =
                            std::min<size_t>(c0 + chunkCurves, size_t(curves));
                        // copy input chunk into this node's buffer, then style
                        memcpy(outp + c0 * probe::CVS, inp + c0 * probe::CVS,
                               (c1 - c0) * probe::CVS * sizeof(GfVec3f));
                        probe::StyleAoS(
                            reinterpret_cast<float *>(outp), c0, c1,
                            n.amount, n.seed);
                    }
                };
                if (serialMode) {
                    body(tbb::blocked_range<size_t>(0, nChunks));
                } else {
                    tbb::parallel_for(
                        tbb::blocked_range<size_t>(0, nChunks, 1), body);
                }
                std::fill(n.dirty.begin(), n.dirty.end(), 0);
                in = n.out.cdata();
            }
        } else {
            GfVec3f *buf = shared.data();
            // one buffer: seed it from the source for dirty chunks only
            auto seedBody = [&](const tbb::blocked_range<size_t> &r) {
                for (size_t ci = r.begin(); ci != r.end(); ++ci) {
                    if (!nodes[0].dirty[ci]) continue;
                    const size_t c0 = ci * chunkCurves;
                    const size_t c1 =
                        std::min<size_t>(c0 + chunkCurves, size_t(curves));
                    memcpy(buf + c0 * probe::CVS,
                           source.cdata() + c0 * probe::CVS,
                           (c1 - c0) * probe::CVS * sizeof(GfVec3f));
                }
            };
            if (serialMode) seedBody(tbb::blocked_range<size_t>(0, nChunks));
            else tbb::parallel_for(tbb::blocked_range<size_t>(0, nChunks, 1),
                                   seedBody);
            for (int s = 0; s < stylers; ++s) {
                Node &n = nodes[s];
                auto body = [&](const tbb::blocked_range<size_t> &r) {
                    for (size_t ci = r.begin(); ci != r.end(); ++ci) {
                        if (!n.dirty[ci]) continue;
                        const size_t c0 = ci * chunkCurves;
                        const size_t c1 =
                            std::min<size_t>(c0 + chunkCurves, size_t(curves));
                        probe::StyleAoS(reinterpret_cast<float *>(buf), c0, c1,
                                        n.amount, n.seed);
                    }
                };
                if (serialMode) body(tbb::blocked_range<size_t>(0, nChunks));
                else tbb::parallel_for(
                        tbb::blocked_range<size_t>(0, nChunks, 1), body);
                std::fill(n.dirty.begin(), n.dirty.end(), 0);
            }
        }
    };

    auto markAll = [&](void) {
        for (Node &n : nodes) std::fill(n.dirty.begin(), n.dirty.end(), 1);
    };
    auto markSparse = [&](bool scattered) {
        for (Node &n : nodes) std::fill(n.dirty.begin(), n.dirty.end(), 0);
        const size_t hitChunks =
            std::max<size_t>(1, (nChunks * size_t(sparsePermil)) / 1000);
        if (scattered) {
            const size_t stride = std::max<size_t>(1, nChunks / hitChunks);
            for (size_t ci = 0; ci < nChunks; ci += stride)
                for (Node &n : nodes) n.dirty[ci] = 1;
        } else {
            for (size_t ci = 0; ci < hitChunks; ++ci)
                for (Node &n : nodes) n.dirty[ci] = 1;
        }
    };
    auto markFromNode = [&](int s) {
        for (Node &n : nodes) std::fill(n.dirty.begin(), n.dirty.end(), 0);
        for (int i = s; i < stylers; ++i)
            std::fill(nodes[i].dirty.begin(), nodes[i].dirty.end(), 1);
    };

    // cold
    {
        auto t0 = Clock::now();
        runChain();
        double s = 0;
        const GfVec3f *r = perNodeBufs ? nodes[stylers - 1].out.cdata()
                                       : shared.cdata();
        for (size_t i = 0; i < nElem; i += 977) s += r[i][0] + r[i][1] + r[i][2];
        printf("run_cold_ms                 %8.2f   checksum=%.4f\n",
               MsSince(t0), s);
    }
    for (int r = 0; r < runs; ++r) {
        markAll();
        auto t0 = Clock::now();
        runChain();
        printf("run_full_ms                 %8.2f\n", MsSince(t0));
    }
    for (int r = 0; r < runs; ++r) {
        markSparse(false);
        auto t0 = Clock::now();
        runChain();
        printf("run_after_sparse_ms         %8.3f\n", MsSince(t0));
    }
    for (int r = 0; r < runs; ++r) {
        markSparse(true);
        auto t0 = Clock::now();
        runChain();
        printf("run_after_scatter_ms        %8.3f\n", MsSince(t0));
    }
    for (int r = 0; r < runs; ++r) {
        markFromNode(stylers - 1);
        auto t0 = Clock::now();
        runChain();
        printf("run_after_lastparam_ms      %8.3f\n", MsSince(t0));
    }
    for (int r = 0; r < runs; ++r) {
        markFromNode(0);
        auto t0 = Clock::now();
        runChain();
        printf("run_after_firstparam_ms     %8.3f\n", MsSince(t0));
    }
    // Handing the result to Hydra: VtArray copy-on-write share = a refcount
    // bump, no copy.
    {
        double keep = 0;
        for (int r = 0; r < runs; ++r) {
            auto t0 = Clock::now();
            VtVec3fArray handoff =
                perNodeBufs ? nodes[stylers - 1].out : shared;
            const double ms = MsSince(t0);
            keep += handoff[0][0];
            printf("extract_vtarray_cow_ms      %8.4f  n=%zu\n",
                   ms, handoff.size());
        }
        // Cost of the next full run once a copy is outstanding: VtArray
        // detaches on the next write accessor.
        VtVec3fArray outstanding =
            perNodeBufs ? nodes[stylers - 1].out : shared;
        markAll();
        auto t0 = Clock::now();
        runChain();
        printf("run_full_with_outstanding_ref_ms %8.2f keep=%.3f\n",
               MsSince(t0), keep + outstanding[0][0]);
    }
    return 0;
}
